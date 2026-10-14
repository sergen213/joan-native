#include "engine/attach_items.hpp"

#include "formats/world_text.hpp"

namespace joa::engine {
namespace {

std::optional<formats::ResArchive> open_res(const formats::Vfs& vfs, std::string_view path) {
    auto bytes = vfs.read(path);
    return bytes ? formats::ResArchive::parse(std::move(*bytes)) : std::nullopt;
}

std::vector<formats::KvLine> record(const std::optional<formats::ResArchive>& res, std::string_view code) {
    if (!res) return {};
    const auto text = res->text(code);
    return text ? formats::parse_kv_text(*text) : std::vector<formats::KvLine>{};
}

std::string field(const std::vector<formats::KvLine>& rec, std::string_view key) {
    for (const auto& kv : rec)
        if (formats::iequals(kv.key, key)) return kv.value;
    return {};
}

bool none(const std::string& v) { return v.empty() || formats::iequals(v, "none"); }

std::optional<formats::DbfTable> table(const std::optional<formats::ResArchive>& res, std::string_view name) {
    const formats::ResEntry* e = res ? res->find(name) : nullptr;
    return e ? formats::DbfTable::parse(res->data(*e)) : std::nullopt;
}

}  // namespace

std::string item_anim(std::string_view model) {
    if (model.size() < 4 || !formats::iequals(model.substr(model.size() - 4), ".am1")) return {};
    const auto slash = model.rfind('/');
    const std::size_t from = slash == std::string_view::npos ? 0 : slash + 1;
    return "item/anim/" + std::string(model.substr(from, model.size() - from - 4)) + ".am2";
}

UnitDb::UnitDb(const formats::Vfs& vfs)
    : units_(open_res(vfs, "unit/unit.res")), items_(open_res(vfs, "item/item.res")),
      std_set_(open_res(vfs, "Resource/STD.SET")) {
    // GameData item codes are the Code field (ItemRes_parse 0x4a0160), not always the entry name.
    if (items_)
        for (const auto& e : items_->entries())
            if (const std::string c = field(record(items_, e.name), "Code"); !c.empty() && !formats::iequals(c, e.name))
                item_alias_.emplace_back(c, e.name);
}

std::vector<formats::KvLine> UnitDb::item_record(std::string_view code) const {
    for (const auto& [c, entry] : item_alias_)
        if (formats::iequals(c, code)) return record(items_, entry);
    return record(items_, code);
}

std::optional<UnitLook> UnitDb::look(std::string_view code, int level) const {
    const auto rec = record(units_, code);
    // Model level (Unit +0x3c5, UnitRes_load_record 0x404afc): 0 = Model / Animation, n = the n-th
    // ModelName / AnimationName pair after ModelLevel (class +0x84 / +0x90 + n*4). CONFIRMED.
    std::string model = field(rec, "Model"), anim_code = field(rec, "Animation");
    int n = 0;
    for (const auto& kv : rec) {
        if (formats::iequals(kv.key, "ModelName") && ++n == level) model = kv.value;
        else if (formats::iequals(kv.key, "AnimationName") && n == level && level > 0) anim_code = kv.value;
    }
    if (none(model)) return std::nullopt;
    UnitLook out;
    out.model = "unit/" + model;
    if (const std::string s = field(rec, "ShadowModel"); !none(s)) out.shadow = "model/" + s;

    // Animation code -> STD.SET ANIMATE {CODE, DBFNAME} -> per-unit table; the idle clip is the
    // STAND/IDLE row without a combo suffix, its UPPERFILE.am2 holds clip UPPERNAME (CONFIRMED
    // layout, GAMEPLAY_CHARACTER_COMBAT.md anim record +0x2c/+0x4d).
    out.anim_code = anim_code;
    if (const auto animate = table(std_set_, "ANIMATE")) {
        const int c = animate->field_index("CODE"), d = animate->field_index("DBFNAME");
        for (std::size_t r = 0; c >= 0 && d >= 0 && r < animate->record_count(); ++r) {
            if (!formats::iequals(animate->text(r, std::size_t(c)), anim_code)) continue;
            const auto t = table(std_set_, animate->text(r, std::size_t(d)));
            if (!t) break;
            const int pos = t->field_index("POSITION"), mov = t->field_index("MOVEMENT"), combo = t->field_index("COMBO"),
                      file = t->field_index("UPPERFILE"), name = t->field_index("UPPERNAME");
            if (pos < 0 || mov < 0 || combo < 0 || file < 0 || name < 0) break;
            for (std::size_t i = 0; i < t->record_count(); ++i)
                if (formats::iequals(t->text(i, std::size_t(pos)), "STAND") && formats::iequals(t->text(i, std::size_t(mov)), "IDLE") &&
                    t->text(i, std::size_t(combo)).empty()) {
                    out.anim = "anim/" + t->text(i, std::size_t(file)) + ".am2";
                    out.idle = t->text(i, std::size_t(name));
                    break;
                }
            break;
        }
    }
    if (out.anim.empty()) return std::nullopt;

    // Hand weapons, else the ranged weapon (archers hold their bow while idle; HYPOTHESIS, matches the
    // original captures of archers). An item's carried mesh is its first ModelName (Model is the
    // pickup/inventory mesh, e.g. JOAN_SWORD_LV1 itm_Warbrand.am1 vs sw_Warbrand.smo).
    std::vector<std::string> ids;
    for (const char* k : {"PWeaponId", "SWeaponId"})
        if (const std::string id = field(rec, k); !none(id)) ids.push_back(id);
    if (ids.empty())
        if (const std::string id = field(rec, "PRangeWeaponId"); !none(id)) ids.push_back(id);
    for (const std::string& id : ids)
        if (auto it = item(id); it && !it->bone.empty()) out.items.push_back(std::move(*it));
    return out;
}

std::optional<UnitLook::Item> UnitDb::item(std::string_view code, int level) const {
    // ItemRes model for a level (FUN_004a3f20, CONFIRMED): 0 = Model, n = the n-th ModelName (+0x73 + n*4),
    // Model when that slot is empty.
    const auto rec = item_record(code);
    std::string mesh = field(rec, "Model");
    int n = 0;
    for (const auto& kv : rec)
        if (formats::iequals(kv.key, "ModelName") && ++n == level && !none(kv.value)) mesh = kv.value;
    if (none(mesh)) return std::nullopt;
    std::string bone = field(rec, "AttachBoneName");
    if (none(bone)) bone.clear();
    return UnitLook::Item{"item/" + mesh, bone};
}

std::string UnitDb::item_emitter(std::string_view code, int slot) const {
    int n = 0;
    for (const auto& kv : item_record(code))
        if (formats::iequals(kv.key, "EmitterName") && n++ == slot) return none(kv.value) ? std::string() : kv.value;
    return {};
}

}  // namespace joa::engine
