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

UnitDb::UnitDb(const formats::Vfs& vfs)
    : units_(open_res(vfs, "unit/unit.res")), items_(open_res(vfs, "item/item.res")),
      std_set_(open_res(vfs, "Resource/STD.SET")) {}

std::optional<UnitLook> UnitDb::look(std::string_view code) const {
    const auto rec = record(units_, code);
    const std::string model = field(rec, "Model");
    if (none(model)) return std::nullopt;
    UnitLook out;
    out.model = "unit/" + model;
    if (const std::string s = field(rec, "ShadowModel"); !none(s)) out.shadow = "model/" + s;

    // Animation code -> STD.SET ANIMATE {CODE, DBFNAME} -> per-unit table; the idle clip is the
    // STAND/IDLE row without a combo suffix, its UPPERFILE.am2 holds clip UPPERNAME (CONFIRMED
    // layout, GAMEPLAY_CHARACTER_COMBAT.md anim record +0x2c/+0x4d).
    const std::string anim_code = field(rec, "Animation");
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
    for (const std::string& id : ids) {
        const auto item = record(items_, id);
        std::string mesh = field(item, "ModelName");
        if (none(mesh)) mesh = field(item, "Model");
        const std::string bone = field(item, "AttachBoneName");
        if (!none(mesh) && !none(bone)) out.items.push_back({"item/" + mesh, bone});
    }
    return out;
}

}  // namespace joa::engine
