#include "game/unitclass_db.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "formats/text_vars.hpp"
#include "formats/world_text.hpp"
#include "game/unit_state.hpp"

namespace joa::game {
namespace {

using formats::DbfTable;
using formats::KvLine;
using formats::ResArchive;

std::string upper(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = char(std::toupper(static_cast<unsigned char>(c)));
    return o;
}
std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}
int index_of(std::string_view v, std::initializer_list<const char*> names, int fallback) {
    int i = 0;
    for (const char* n : names) {
        if (upper(trim(v)) == n) return i;
        ++i;
    }
    return fallback;
}
// First token of a value (the original reads whitespace-separated tokens).
std::string tok(const std::string& v) {
    std::string t = trim(v);
    if (auto sp = t.find_first_of(" \t"); sp != std::string::npos) t.resize(sp);
    return t;
}

struct Kv {
    std::vector<KvLine> lines;
    const std::string* get(std::string_view key) const {
        for (const auto& l : lines)
            if (formats::iequals(trim(l.key), key)) return &l.value;
        return nullptr;
    }
    std::string str(std::string_view key) const { auto p = get(key); return p ? trim(*p) : std::string(); }
    int i(std::string_view key, int def = 0) const { auto p = get(key); return p ? std::atoi(p->c_str()) : def; }
    float f(std::string_view key, float def = 0) const { auto p = get(key); return p ? float(std::atof(p->c_str())) : def; }
};

std::vector<std::pair<std::string, Kv>> load_kv_res(const std::filesystem::path& p, std::string* err) {
    std::vector<std::pair<std::string, Kv>> out;
    auto res = ResArchive::open(p, err);
    if (!res) return out;
    for (const auto& e : res->entries()) out.push_back({e.name, Kv{formats::parse_kv_text(res->text(e))}});
    return out;
}

std::optional<DbfTable> table(const ResArchive& set, std::string_view name) {
    const auto* e = set.find(name);
    return e ? DbfTable::parse(set.data(*e)) : std::nullopt;
}
std::string cell(const DbfTable& t, size_t r, std::string_view col) {
    int f = t.field_index(col);
    return f < 0 ? std::string() : t.text(r, size_t(f));
}

int effect_from_name(std::string_view s) {
    static const char* names[] = {"MAX_HIT_POINT", "MAX_ENERGY_POINT", "STRENGTH", "DEXTERITY", "DEFENSE", "LEADERSHIP",
                                  "WEAPON_DAMAGE", "WEAPON_DAMAGE_MULTIPLIER", "REDUCE_MELEE_DAMAGE_SUSTAINED",
                                  "ARROW_DEFLECT_OFF", "A_ATTACK_DAMAGE", "B_ATTACK_DAMAGE"};
    for (int i = 0; i < 12; ++i)
        if (upper(trim(s)) == names[i]) return i + 1;
    // 0x4a45f0 order differs from the numeric order for 0xd..0xf (CONFIRMED).
    static const std::pair<const char*, int> rest[] = {
        {"RUSH_ATTACK_DAMAGE", 0xe}, {"DOWN_ATTACK_DAMAGE", 0xf}, {"JUMP_ATTACK_B_DAMAGE", 0xd}, {"RANGE_DISTANCE", 0x10},
        {"RATE_OF_FIRE", 0x11}, {"MORALE_BASE", 0x12}, {"MORALE_DROP", 0x13}, {"FATAL_KILL", 0x14},
        {"EP_CONSUME", 0x15}, {"DAMAGE_SUSTAINED", 0x16}, {"TROOP_SIZE", 0x17}};
    for (auto& [n, v] : rest)
        if (upper(trim(s)) == n) return v;
    return 0;
}

}  // namespace

std::optional<int> ItemDef::effect_value(int effect, int level) const {
    if (primary_effect == effect) return damage + (level - min_level) * inc_damage;
    for (const auto& x : extra)
        if (x.type == effect) return x.value + (level - min_level) * x.inc;
    return std::nullopt;
}

int GameData::unit_index(std::string_view code) const {
    for (size_t i = 0; i < units.size(); ++i)
        if (formats::iequals(units[i].code, code)) return int(i);
    return -1;
}
int GameData::item_index(std::string_view code) const {
    if (code.empty() || formats::iequals(code, "NONE")) return 0;
    for (size_t i = 1; i < items.size(); ++i)
        if (formats::iequals(items[i].code, code)) return int(i);
    return 0;
}
int GameData::script_index(std::string_view code) const {
    for (size_t i = 0; i < scripts.size(); ++i)
        if (formats::iequals(scripts[i].code, code)) return int(i);
    return -1;
}
int GameData::nation_id(std::string_view code) const {
    for (size_t i = 0; i < nations.size(); ++i)
        if (formats::iequals(nations[i], code) || (code.size() >= 6 && formats::iequals(nations[i], code.substr(0, 6))))
            return int(i) + 1;
    return 0;
}

std::unique_ptr<GameData> GameData::load(const std::filesystem::path& dir, std::string* err) {
    auto gd = std::make_unique<GameData>();
    gd->vfs_ = formats::Vfs(dir);
    auto path = [&](std::string_view p) { return gd->vfs_.resolve(p).value_or(dir / std::string(p)); };

    // GAMEPLAY.RES
    {
        auto res = ResArchive::open(path("Resource/GAMEPLAY.RES"), err);
        if (!res) return nullptr;
        std::vector<formats::GameVar> vars;
        for (const auto& e : res->entries())
            if (auto v = formats::parse_game_vars(res->text(e))) vars.insert(vars.end(), v->begin(), v->end());
        auto raw = [&](std::string_view sec, std::string_view key) -> const std::string* {
            for (const auto& v : vars)
                if (formats::iequals(v.section, sec) && formats::iequals(v.key, key)) return &v.value;
            return nullptr;
        };
        auto I = [&](const char* sec, const char* key, int& out) { if (auto p = raw(sec, key)) out = std::atoi(p->c_str()); };
        auto F = [&](const char* sec, const char* key, float& out) { if (auto p = raw(sec, key)) out = float(std::atof(p->c_str())); };
        Consts& c = gd->c;
        I("UNIT", "BODY_R", c.body_r); I("UNIT", "REACH_DT", c.reach_dt); I("UNIT", "FELL_DUR", c.fell_dur);
        I("UNIT", "INVC_DUR", c.invc_dur); I("UNIT", "STR_HP", c.str_hp); I("UNIT", "DEF_HP", c.def_hp);
        F("UNIT", "DEF_FALL", c.def_fall); F("UNIT", "DEF_HP_R", c.def_hp_r); I("UNIT", "RECOV_HP", c.recov_hp);
        I("UNIT", "EGATK_MP", c.egatk_mp); I("UNIT", "EGATK_HP", c.egatk_hp); I("UNIT", "RECOV_MP", c.recov_mp);
        I("UNIT", "RECOV_T", c.recov_t); I("UNIT", "CLB_IDLE", c.clb_idle); I("UNIT", "MAX_COM", c.max_com);
        I("UNIT", "CRI_DIFF", c.cri_diff); F("UNIT", "CRITEXP", c.critexp); I("UNIT", "LEVELARR", c.levelarr);
        F("UNIT", "AI_MOVE", c.ai_move); I("UNIT", "MINDEXS", c.mindexs); I("UNIT", "MAXDEXS", c.maxdexs);
        F("UNIT", "STOP_CC", c.stop_cc); F("UNIT", "IDLE2_CH", c.idle2_ch); F("UNIT", "IDLE3_CH", c.idle3_ch);
        F("UNIT", "IDLE_CC", c.idle_cc);
        I("UNIT", "MOR_NOR", c.mor_nor); F("UNIT", "MOR_CH_U", c.mor_ch_u); F("UNIT", "MOR_CH_T", c.mor_ch_t);
        I("UNIT", "WP_DAMG", c.wp_damg); I("UNIT", "AMR_DEF", c.amr_def); F("UNIT", "AMREXDEF", c.amrexdef);
        F("UNIT", "SIE_UDAM", c.sie_udam); I("UNIT", "F_ATTACK", c.f_attack); I("UNIT", "F_DEFEND", c.f_defend);
        I("UNIT", "ARR_ACCU", c.arr_accu); I("UNIT", "BLOCKPER", c.blockper); I("UNIT", "BLOCKANG", c.blockang);
        I("UNIT", "ARR_FALL", c.arr_fall); I("UNIT", "T_FALLF", c.t_fallf); I("UNIT", "V_FALLF", c.v_fallf);
        I("UNIT", "F_FALLR", c.f_fallr);
        const char* di[] = {"LINF_DI", "HINF_DI", "POLE_DI", "MACE_DI", "ARC_DI", "CRO_DI"};
        const char* dc[] = {"LCAP_DI", "HCAP_DI", "PCAP_DI", "MCAP_DI", "ACAP_DI", "CCAP_DI"};
        for (int k = 0; k < 6; ++k) I("UNIT", di[k], c.drop_pct[k]), I("UNIT", dc[k], c.drop_cap_pct[k]);
        I("UNIT", "HP_PER", c.hp_per); I("UNIT", "EP_PER", c.ep_per); I("UNIT", "STR_PER", c.str_per);
        I("UNIT", "DEF_PER", c.def_per); I("UNIT", "DEX_PER", c.dex_per); I("UNIT", "LDS_PER", c.lds_per);
        I("UNIT", "STR_DAM", c.str_dam); I("UNIT", "DEX_DAM", c.dex_dam); I("UNIT", "DEF_RAT", c.def_rat);
        I("UNIT", "UPG_TIME", c.upg_time); I("UNIT", "BRI_COMM", c.bri_comm); I("UNIT", "BRI_CAPT", c.bri_capt); F("DANGER", "INDANGER", c.indanger); I("MINIMAP", "VISION_R", c.vision_r);
        F("EXP", "MEM_EXP", c.mem_exp); F("EXP", "LD_EXP", c.ld_exp); I("R_ATT", "FALL_S", c.fall_s);
        I("R_ATT", "FALL_E", c.fall_e); F("R_ATT", "DIST_P", c.dist_p); I("C_BOUNS", "C_BONUSL", c.c_bonusl);
        I("C_BOUNS", "E_BONUSL", c.e_bonusl); I("C_BOUNS", "M_BONUSL", c.m_bonusl); I("KILL", "EP_NOR", c.ep_nor);
        I("KILL", "EP_CAP", c.ep_cap); I("KILL", "EP_GEN", c.ep_gen);
        F("CHARMOVE", "AIM_R", c.aim_r); F("CHARMOVE", "AIM_MXD", c.aim_mxd); F("CHARMOVE", "AIM_HA", c.aim_ha);
        F("CHARMOVE", "TURN_ANG", c.turn_ang); I("CHARMOVE", "JUMPMAXD", c.jumpmaxd); I("CHARMOVE", "CHAINPRO", c.chainpro);
        I("CHARMOVE", "CRI_TIME", c.cri_time);
        I("CRITICAL", "FADE_IN1", c.crit_fade_in1); I("CRITICAL", "FADE_IN2", c.crit_fade_in2);
        for (int k = 0; k < 3; ++k) I("CRITICAL", ("GLOW_IN" + std::to_string(k + 1)).c_str(), c.crit_glow_in[k]);
        const char* d[] = {"EASY", "NORM", "DIFF", "VDIFF"};
        for (int k = 0; k < 4; ++k) {
            F("GAME", (std::string(d[k]) + "_D").c_str(), c.diff_d[k]);
            F("GAME", (std::string(d[k]) + "_A").c_str(), c.diff_a[k]);
        }
        const char* tn[] = {"ARW", "PROJ", "EXPL", "STM", "STC", "CAN"};
        for (int k = 0; k < 6; ++k) {
            ThrowConsts& t = gd->throwi[k];
            std::string p = tn[k];
            float tmp = 0;
            I("THROWI", (p + "_TL").c_str(), t.tl); I("THROWI", (p + "_DL").c_str(), t.dl);
            F("THROWI", (p + "_M").c_str(), t.m);
            int iv = 0;
            I("THROWI", (p + "_MXD").c_str(), iv); t.mxd = float(iv); iv = 0;
            I("THROWI", (p + "_MND").c_str(), iv); t.mnd = float(iv); iv = 0;
            I("THROWI", (p + "_TR").c_str(), iv); t.tr = float(iv);
            F("THROWI", (p + "_FC").c_str(), t.fc); F("THROWI", (p + "_AV").c_str(), t.av);
            F("THROWI", (p + "_GL").c_str(), t.gl); F("THROWI", (p + "_ZE").c_str(), t.ze);
            (void)tmp;
            if (t.gl == 0) { t.v0 = 0; t.ft = float(t.tl); }
            else {  // 0x552776..0x55283c
                const float g = t.gl * 9.81f, h = t.ze * 1.0f / 46.1f;
                t.v0 = std::sqrt(2 * g * h);
                t.ft = float(int((t.v0 + std::sqrt(2 * g * h + t.v0 * t.v0)) / t.gl / 9.81f * 1000));
            }
        }
    }

    // STD.SET
    gd->stdset_ = ResArchive::open(path("Resource/STD.SET"), err);
    if (!gd->stdset_) return nullptr;
    const ResArchive& set = *gd->stdset_;
    if (auto t = table(set, "NATION"))
        for (size_t r = 0; r < t->record_count(); ++r) gd->nations.push_back(cell(*t, r, "NATIONCODE"));
    if (auto t = table(set, "LEVEL"))
        for (size_t r = 0; r < t->record_count(); ++r)
            gd->levels.push_back({std::atoi(cell(*t, r, "EXPERIENCE").c_str()), std::atoi(cell(*t, r, "ATT_POINT").c_str()),
                                  std::atoi(cell(*t, r, "SKI_POINT").c_str())});
    if (auto t = table(set, "ANIMATE"))
        for (size_t r = 0; r < t->record_count(); ++r) gd->animate_[upper(cell(*t, r, "CODE"))] = cell(*t, r, "DBFNAME");
    if (auto t = table(set, "UPITEM"))
        for (size_t r = 0; r < t->record_count(); ++r) {
            UpItem u;
            u.type = index_of(cell(*t, r, "TYPE"), {"", "STRENGTH", "DEXTERITY", "DEFENSE", "CHARISMA", "LEADERSHIP", "HIT_POINT",
                                                     "ENERGY_POINT", "", "NEW_COMBO", "JUMP_ATTACK_B", "DOWN_ATTACK",
                                                     "RUSH_ATTACK", "ENERGY_ATTACK", "RANGE_ATTACK", "HORSE_ATTACK_A",
                                                     "HORSE_ATTACK_B"}, 0);
            u.code = cell(*t, r, "CODE");
            u.para1 = cell(*t, r, "PARA1");
            u.p1 = std::atoi(u.para1.c_str());
            u.cost = std::atoi(cell(*t, r, "COST").c_str());
            u.require = cell(*t, r, "REQUIRE");
            u.desc = cell(*t, r, "DESC");
            gd->upitems.push_back(u);
        }
    if (auto t = table(set, "UPGRADE"))
        for (size_t r = 0; r < t->record_count(); ++r) {
            // UpgradeRes_find_by_code 0x531df0: the first row whose CODE matches (stricmp) wins; later rows
            // with the same code are never reached (ENGLISH_CROSSBOW_MAN's list has two).
            auto [slot, fresh] = gd->upgrade_lists.try_emplace(upper(cell(*t, r, "CODE")));
            if (!fresh) continue;
            auto& list = slot->second;
            if (auto ut = table(set, cell(*t, r, "DBFNAME")))
                for (size_t k = 0; k < ut->record_count(); ++k) {
                    const std::string code = cell(*ut, k, "CODE");
                    for (size_t u = 0; u < gd->upitems.size(); ++u)
                        if (formats::iequals(gd->upitems[u].code, code)) { list.push_back({std::atoi(cell(*ut, k, "LEVEL").c_str()), int(u)}); break; }
                }
        }
    std::vector<std::string> itemtypes;
    if (auto t = table(set, "ITEMTYPE"))
        for (size_t r = 0; r < t->record_count(); ++r) itemtypes.push_back(cell(*t, r, "TYPE"));

    // items
    gd->items.emplace_back();
    std::vector<std::string> usable;
    usable.emplace_back();
    for (auto& [rec, kv] : load_kv_res(path("item/item.res"), err)) {
        ItemDef it;
        it.code = kv.str("Code").empty() ? rec : kv.str("Code");
        it.name = kv.str("Name");
        it.item_class = upper(tok(kv.str("ItemClass")));
        const std::string ty = kv.str("ItemType");
        for (size_t k = 0; k < itemtypes.size(); ++k)
            if (formats::iequals(itemtypes[k], ty)) { it.type = int(k) + 1; break; }
        it.range = kv.i("Range");
        it.damage = kv.i("Damage");
        it.inc_damage = kv.i("IncDamage");
        it.min_level = kv.i("MinLevel", 0);
        it.max_level = kv.i("MaxLevel", 0);
        it.model = kv.str("Model");
        it.attach_bone = kv.str("AttachBoneName");
        it.quantity = kv.i("Quantity", 1);
        it.max_quantity = kv.i("MaxQuantity", 1);
        it.primary_effect = effect_from_name(kv.str("DamageType"));
        for (size_t k = 0; k < kv.lines.size(); ++k)
            if (formats::iequals(trim(kv.lines[k].key), "AdditionalEffectType")) {
                ItemDef::Extra x{effect_from_name(kv.lines[k].value), 0, 0};
                if (k + 1 < kv.lines.size()) x.value = std::atoi(kv.lines[k + 1].value.c_str());
                if (k + 2 < kv.lines.size()) x.inc = std::atoi(kv.lines[k + 2].value.c_str());
                it.extra.push_back(x);
            } else if (formats::iequals(trim(kv.lines[k].key), "EffectiveRequirementType")) {
                // 0x4a0160: type, then the next three values by position; EffectiveWhen "BELOW" -> 0, "OVER" -> 1
                // (anything else leaves +0xc uninitialised in the original; every shipped item says one of the two).
                ItemDef::Requirement q{effect_from_name(kv.lines[k].value), 0, 0, true};
                if (k + 1 < kv.lines.size()) q.value = std::atoi(kv.lines[k + 1].value.c_str());
                if (k + 2 < kv.lines.size()) q.inc = std::atoi(kv.lines[k + 2].value.c_str());
                if (k + 3 < kv.lines.size()) q.over = upper(trim(kv.lines[k + 3].value)) != "BELOW";
                it.require.push_back(q);
            }
        usable.push_back(kv.str("UsableUnit"));
        it.use_immediately = kv.i("UseImmediately") != 0;
        it.show_armory = kv.i("ShowArmoryInterface") != 0;
        it.can_drop = kv.i("CanDrop", 1) != 0;
        it.unique = kv.i("UniqueItem") != 0, it.can_sell = kv.i("CanSell", 1) != 0;
        it.width = kv.i("Width", 1), it.height = kv.i("Height", 1);
        it.defense = kv.i("Defense"), it.price = kv.i("Price"), it.pickable = kv.i("Pickable") != 0;
        it.description = kv.str("Description"), it.detail_file = kv.str("DetailDescriptionFilename");
        it.message = kv.str("MessageName");
        gd->items.push_back(std::move(it));
    }
    if (gd->items.size() <= 1) return nullptr;

    // drop lists
    for (auto& [rec, kv] : load_kv_res(path("item/dropitem.res"), err)) {
        auto& list = gd->drop_lists[upper(kv.str("Code").empty() ? rec : kv.str("Code"))];
        for (const auto& l : kv.lines) {
            const std::string k = trim(l.key);
            if (formats::iequals(k, "Code") || formats::iequals(k, "VERSION") || formats::iequals(k, "END")) continue;
            if (int id = gd->item_index(k)) list.push_back({id, std::atoi(l.value.c_str())});
        }
    }

    // attack scripts
    for (auto& [rec, kv] : load_kv_res(path("Resource/animscr.res"), err)) {
        AnimScript s;
        s.code = kv.str("Code").empty() ? rec : kv.str("Code");
        s.start_time = kv.i("StartTime"); s.end_time = kv.i("EndTime"); s.range = kv.i("Range");
        s.start_force_time = kv.i("StartForceTime");
        s.v0 = kv.i("StartVerticalAngle"); s.v1 = kv.i("EndVerticalAngle");
        s.h0 = kv.i("StartHorizontalAngle"); s.h1 = kv.i("EndHorizontalAngle");
        s.gravity = kv.f("GravityLevel", 3.0f);
        s.force_h = kv.i("AttackForce", 3000);
        s.force_v = kv.i("VerticalAttackForce", 500);
        gd->scripts.push_back(std::move(s));
    }

    // unit classes
    for (auto& [rec, kv] : load_kv_res(path("unit/unit.res"), err)) {
        UnitClass u;
        u.code = kv.str("Code").empty() ? rec : kv.str("Code");
        u.name = kv.str("Name"); u.short_name = kv.str("ShortName"); u.title = kv.str("Title");
        u.model = kv.str("Model"); u.animation = kv.str("Animation"); u.script_file = tok(kv.str("ScriptFile"));
        u.cfile = tok(kv.str("CFileName"));
        u.unit_class = index_of(kv.str("UnitClass"), {"CITIZEN", "LIGHT_INFANTRY", "HEAVY_INFANTRY", "POLEMAN", "MACEMAN",
                                                      "ARCHER", "CROSSBOW_MAN", "CAVALRY", "SIEGE_WEAPON", "CART"}, 1);
        u.special = index_of(kv.str("Special"), {"NORMAL", "MAIN", "HORSE", "ELEPHANT", "CANNON", "FAUCONNEAU", "CROSSBOW",
                                                 "ANIMAL", "SEIGE_TOWER", "BOMBARD", "TREBUCHET", "BATTERING_RAM",
                                                 "AUTOBOW", "CART", "SPECIAL"}, 0);
        u.ranking = index_of(kv.str("Ranking"), {"INFANTRY", "ARCHER", "CAVALRY", "CAPTAIN", "GENERAL", "KING"}, 0);
        std::string tt = kv.str("TriggerType") + " ";
        static const std::pair<const char*, unsigned> trig[] = {
            {"TRIGGER_ATTACK ", 1}, {"TRIGGER_ARROW_ATTACK ", 2}, {"TRIGGER_THROW ", 0x10}, {"TRIGGER_STONE_ATTACK ", 0x40},
            {"TRIGGER_INTERACT ", 0x200}, {"TRIGGER_DOWN_ATTACK ", 0x400}, {"TRIGGER_EXPLOSIVE_ARROW_ATTACK ", 0x2000},
            {"TRIGGER_EXPLOSIVE_KEG_ATTACK ", 0x4000}};
        for (auto& [n, bit] : trig)
            if (upper(tt).find(n) != std::string::npos) u.trigger_mask |= bit;
        u.hero = u.code == "JOAN_OF_ARC" || u.code == "JEAN_DE_METZ" || u.code == "LAHIRE" || u.code == "ALENCON";
        if (std::string sx = kv.str("Sex"); !sx.empty()) u.sex = sx[0];
        u.script_duration = kv.i("ScriptDuration"); u.height = kv.i("Height");
        u.walk = kv.f("WalkSpeed"); u.run = kv.f("RunSpeed"); u.shift_l = kv.f("ShiftLeftSpeed");
        u.shift_r = kv.f("ShiftRightSpeed"); u.shift_b = kv.f("ShiftBackSpeed");
        u.hp = kv.i("HP"); u.ep = kv.i("EP"); u.life = kv.i("Life"); u.experience = kv.i("Experience"); u.gold = kv.i("Gold");
        if (u.unit_class == 8) u.resource = kv.i("Resource");   // RecruitWorkshop: +0x165, read for siege weapons only
        u.earmor_id = kv.i("EArmorId");
        u.earmor_item = gd->item_index(tok(kv.str("EArmorCode")));
        u.pweapon = gd->item_index(tok(kv.str("PWeaponId")));
        u.sweapon = gd->item_index(tok(kv.str("SWeaponId")));
        u.prange = gd->item_index(tok(kv.str("PRangeWeaponId")));
        u.srange = gd->item_index(tok(kv.str("SRangeWeaponId")));
        u.drop_list = upper(tok(kv.str("DropItemList")));
        u.upgrade_list = upper(tok(kv.str("UpgradeList")));
        u.str = kv.i("Strength"); u.dex = kv.i("Dexterity"); u.def = kv.i("Defense"); u.lead = kv.i("Leadership");
        u.archery = kv.i("ArcherySkill"); u.horse_riding = kv.i("HorseRiding"); u.swimming = kv.i("SwimmingSkill");
        u.elephant_riding = kv.i("ElephantRiding"); u.wide_animal = kv.i("WideAnimal"); u.preaching = kv.i("Preaching");
        u.power_jump = kv.i("PowerJump"); u.disguise = kv.i("Disguise"); u.persuade = kv.i("Persuade");
        u.lock_picking = kv.i("LockPicking"); u.mountain = kv.i("Mountain");
        u.level = kv.i("Level", 1); u.loyalty = kv.i("Loyalty"); u.morale = kv.i("Morale"); u.reputation = kv.i("Reputation");
        u.behavior = kv.i("Behavior"); u.falling = kv.i("Falling", 20); u.kingdom = kv.i("KingdomId");
        u.cyl_radius = kv.f("CylinderRadius", 15); u.cyl_height = kv.f("CylinderHeight", 80); u.cyl_center_z = kv.f("CylinderCenterZ", 40);
        for (const auto& l : kv.lines) {   // coordinates go through atoi then (float) (CONFIRMED loader order)
            const std::string k = upper(trim(l.key));
            auto& ap = u.attach_points;
            if (k == "ATTACHPOINTX") ap.emplace_back(), ap.back().x = float(std::atoi(l.value.c_str()));
            else if (ap.empty()) continue;
            else if (k == "ATTACHPOINTY") ap.back().y = float(std::atoi(l.value.c_str()));
            else if (k == "ATTACHPOINTZ") ap.back().z = float(std::atoi(l.value.c_str()));
            else if (k == "ATTACHPOINTDIR") ap.back().dir = float(std::atof(l.value.c_str()));
            else if (k == "ATTACHPOINTUSAGE") ap.back().usage = index_of(trim(l.value).substr(0, 1), {"", "M", "F", "B"}, 0);
            else if (k == "ATTACHUNITWEAPON") ap.back().item = gd->item_index(tok(l.value));
        }
        if (int n = kv.i("AttachPointCount"); n >= 0 && n < int(u.attach_points.size())) u.attach_points.resize(size_t(n));
        gd->units.push_back(std::move(u));
    }
    for (size_t i = 1; i < gd->items.size(); ++i)
        if (!usable[i].empty()) gd->items[i].usable_unit = gd->unit_index(tok(usable[i]));
    if (gd->units.empty()) return nullptr;
    return gd;
}

int GameData::load_clip(const std::string& file, const std::string& name) {
    const std::string key = upper(file) + "|" + upper(name);
    if (auto it = clip_index_.find(key); it != clip_index_.end()) return it->second;
    auto& anim = am2_[upper(file)];
    if (!anim) {
        anim = std::make_shared<formats::Animation>();
        std::string e;
        auto bytes = vfs_.read("anim/" + file + ".am2");
        if (!bytes || !formats::parse_animation(*bytes, *anim, e)) anim->clips.clear();
    }
    int idx = -1;
    for (const auto& c : anim->clips)
        if (formats::iequals(c.name, name)) {
            ClipInfo ci;
            ci.length_ms = c.end - c.start;
            for (const auto& ev : anim->events)
                if (ev.time >= c.start && ev.time <= c.end) {
                    auto e2 = ev;
                    e2.time -= c.start;
                    ci.events.push_back(e2);
                }
            idx = int(clips.size());
            clips.push_back(std::move(ci));
            break;
        }
    clip_index_[key] = idx;
    return idx;
}

const std::array<float, 4>& GameData::item_sphere(int item) {
    auto [it, fresh] = item_sphere_.try_emplace(item, std::array<float, 4>{0, 0, 0, 10});
    if (!fresh || item <= 0 || item >= int(items.size())) return it->second;
    const ItemDef& d = items[size_t(item)];
    if (d.type >= 13 && d.type <= 18) return it->second;   // 0x41e850: arrows / bolts, sphere at the origin, r 10.0
    const auto dot = d.model.rfind('.');
    formats::Mesh m;
    std::string e;
    if (const auto bytes = vfs_.read("item/" + d.model);
        bytes && formats::parse_mesh(*bytes, dot != std::string::npos && formats::iequals(d.model.substr(dot), ".am1"), m, e))
        it->second = {m.sphere[0], m.sphere[1], m.sphere[2], m.sphere[3]};
    return it->second;
}

bool GameData::bone_origin(const MoveRecord& r, const std::string& bone, float t, std::array<float, 3>& out) {
    const auto a = am2_.find(r.upper_file);
    if (a == am2_.end() || !a->second || bone.empty()) return false;
    auto& sk = am3_[r.upper_file];
    if (!sk) {
        sk = std::make_shared<formats::Skeleton>();
        std::string e;
        const auto bytes = vfs_.read("anim/" + r.upper_file + ".am3");
        if (!bytes || !formats::parse_skeleton(*bytes, *sk, e)) sk->nodes.clear();
    }
    size_t b = 0;
    while (b < sk->nodes.size() && !(sk->nodes[b].present && formats::iequals(sk->nodes[b].name, bone))) ++b;
    const formats::Animation& anim = *a->second;
    const formats::AnimClip* c = nullptr;
    for (const auto& x : anim.clips)
        if (formats::iequals(x.name, r.upper_name)) { c = &x; break; }
    if (!c || b >= anim.tracks.size() || anim.tracks[b].empty()) return false;
    const formats::Mat3x4 m = formats::sample_pose(anim, std::min(c->start + t, c->end))[b];
    out = {m[3], m[7], m[11]};
    return true;
}

bool root_motion_action(int a) {
    switch (a) {
    case CHAIN: case ATTACK: case JATTACKB: case ENGATK: case ENGATK2: case ENGATK3: case ENGATK4: case ATTACKED:
    case ATTKED2: case ATTKEDR: case THROW: case THROWN: case STANDUP: case RUSH: case DOWNATK: case JUMP_B:
    case JUMP_R: case JUMP_L:
        return true;
    default: return false;
    }
}

void root_offset(const ClipInfo& c, float t, float& x, float& y) {   // FUN_00418a00
    x = y = 0;
    const auto& k = c.root;
    if (k.empty()) return;
    if (t >= k.back()[0]) { x = k.back()[1], y = k.back()[2]; return; }
    if (t < k[0][0]) {
        if (t >= 0.0f) x = t / k[0][0] * k[0][1], y = t / k[0][0] * k[0][2];
        return;
    }
    for (size_t i = 1; i < k.size(); ++i)
        if (t < k[i][0]) {
            const float f = (t - k[i - 1][0]) / (k[i][0] - k[i - 1][0]);
            x = (k[i][1] - k[i - 1][1]) * f + k[i - 1][1], y = (k[i][2] - k[i - 1][2]) * f + k[i - 1][2];
            return;
        }
}

// FUN_00417f20: the "Bip01" track's keys from the one at the clip start (0x5b7c70: bracketing key,
// the next one when the fraction > 0.99) through the clip end, as offsets from that key, appended
// with 0x418820's colinear merge. The "Mother" bone transform applies only to skeletons without
// Bip01 (siege / broken models), so it never matters here.
void GameData::build_root(int idx, const std::string& file, const std::string& name) {
    ClipInfo& ci = clips[size_t(idx)];
    if (ci.root_built) return;
    ci.root_built = true;
    const auto& anim = am2_[upper(file)];
    auto bytes = vfs_.read("anim/" + file + ".am3");
    formats::Skeleton sk;
    std::string e;
    if (!anim || !bytes || !formats::parse_skeleton(*bytes, sk, e)) return;
    size_t bip = sk.nodes.size();
    for (size_t i = 0; i < sk.nodes.size(); ++i)
        if (sk.nodes[i].present && sk.nodes[i].name == "Bip01") { bip = i; break; }
    const formats::AnimClip* clip = nullptr;
    for (const auto& c : anim->clips)
        if (formats::iequals(c.name, name)) { clip = &c; break; }
    if (!clip || bip >= anim->tracks.size()) return;
    const auto& keys = anim->tracks[bip];
    if (keys.empty()) return;
    size_t i = 0;
    while (i + 1 < keys.size() && keys[i + 1].time <= clip->start) ++i;
    if (i + 1 < keys.size() && keys[i].time < clip->start &&
        (clip->start - keys[i].time) / (keys[i + 1].time - keys[i].time) > 0.99f)
        ++i;
    const float x0 = keys[i].m[3], y0 = keys[i].m[7];
    auto& k = ci.root;
    for (; i < keys.size(); ++i) {
        const float t = keys[i].time;
        if (t < clip->start - 0.001f) continue;
        if (t > clip->end + 0.001f) break;
        const float kt = std::max(0.0f, t - clip->start), kx = keys[i].m[3] - x0, ky = keys[i].m[7] - y0;
        if (!k.empty()) {   // 0x418820: drop the last key when it lies within 1 unit of prev -> new
            const std::array<float, 3> p = k.size() < 2 ? std::array<float, 3>{} : k[k.size() - 2];
            const auto& l = k.back();
            const float f = (l[0] - p[0]) / (kt - p[0]);
            if (std::fabs(l[1] - ((kx - p[1]) * f + p[1])) <= 1.0f && std::fabs(l[2] - ((ky - p[2]) * f + p[2])) <= 1.0f)
                k.pop_back();
        }
        if (k.size() >= 50) break;   // ponytail: the 0x25c record holds 50 keys; longer tracks never seen
        k.push_back({kt, kx, ky});
    }
}

const MoveTable* GameData::moves(int cls) {
    if (cls < 0 || cls >= int(units.size())) return nullptr;
    UnitClass& u = units[cls];
    if (u.move_table >= 0) return &move_tables[u.move_table];
    auto it = animate_.find(upper(u.animation));
    if (it == animate_.end() || !stdset_) return nullptr;
    for (size_t i = 0; i < move_tables.size(); ++i)
        if (formats::iequals(move_tables[i].code, it->second)) return &move_tables[u.move_table = int(i)];
    auto t = table(*stdset_, it->second);
    if (!t) return nullptr;
    MoveTable mt;
    mt.code = it->second;
    auto num = [&](size_t r, std::string_view a, std::string_view b = {}) {
        std::string s = cell(*t, r, a);
        if (s.empty() && !b.empty()) s = cell(*t, r, b);
        return std::atoi(s.c_str());
    };
    for (size_t r = 0; r < t->record_count(); ++r) {
        MoveRecord m;
        m.position = index_of(cell(*t, r, "POSITION"), {"STAND", "JUMP"}, 0);
        m.movement = action_from_name(cell(*t, r, "MOVEMENT"));
        m.combo = upper(cell(*t, r, "COMBO"));
        std::string ca = cell(*t, r, "COMBOAVAIL"), ch = cell(*t, r, "CHAINAVAIL");
        m.combo_avail = ca.empty() ? 'N' : char(std::toupper(static_cast<unsigned char>(ca[0])));
        m.chain_avail = ch.empty() ? ' ' : ch[0];
        m.item_using = index_of(cell(*t, r, "ITEM_USING"), {"DEFAULT", "1SWORD", "2SWORD", "2HAMMER", "2SPEAR", "2STAFF",
                                                            "2FLAIL", "BOW", "CROSSBOW", "SHIELD"}, 0);
        m.special = index_of(cell(*t, r, "SPECIAL"), {"DEFAULT", "TIRED", "HAPPY", "POISON", "CAMP", "CANNON", "FAUCONN",
                                                      "BOMBARD", "CROSSBOW", "SIEGET", "TREB_L", "TREB_R", "TREB_F",
                                                      "BATTER", "BATTER_L", "BATTER_R"}, 0);
        m.energy_pt = num(r, "ENERGY_PT");
        m.damage = num(r, "DAMAGE");
        m.blend = num(r, "BLEND_TIME");
        m.start_time = num(r, "START_TIME");
        m.end_time = num(r, "END_TIME");
        m.upper_file = cell(*t, r, "UPPERFILE");
        m.upper_name = cell(*t, r, "UPPERNAME");
        m.script = script_index(cell(*t, r, "SCRIPT"));
        m.sound = cell(*t, r, "SOUND");
        if (m.sound.empty()) m.sound = cell(*t, r, "SOUND1");
        m.sound_time = num(r, "SOUND_TIME", "SOUNDTIME1");
        m.sound2 = cell(*t, r, "SOUND2");
        m.sound_time2 = num(r, "SOUNDTIME2");
        m.show_crit = num(r, "SHOW_CRIT") != 0;
        if (!m.upper_file.empty() && !m.upper_name.empty()) m.clip = load_clip(m.upper_file, m.upper_name);
        if (m.clip >= 0 && root_motion_action(m.movement)) build_root(m.clip, m.upper_file, m.upper_name);   // FUN_00415090
        mt.rows.push_back(std::move(m));
    }
    move_tables.push_back(std::move(mt));
    u.move_table = int(move_tables.size()) - 1;
    return &move_tables.back();
}

}  // namespace joa::game
