// GameUiBridge: live mission state -> HudModel, mission events -> HUD flags, HUD commands -> game.
// Sources per field (joan.exe): player panel / party FUN_006f9b60, party list FUN_006ff950, minimap dots/icons
// FUN_0070fbe0, commander list 0x4cdd40 (work/ui/MissionBrief/re_commanders.md), objectives FUN_00704810.
// Overlay screens: game_ui_bridge_screens.cpp.
#include "engine/game_ui_bridge.hpp"

#include "engine/assets_cache.hpp"
#include "engine/game_camera.hpp"  // hold_voice
#include "engine/hud_crit.hpp"
#include "engine/hud_mapobj.hpp"
#include "engine/menu_saveload.hpp"
#include "engine/session.hpp"
#include "formats/save_sav.hpp"
#include "formats/world_text.hpp"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <ctime>
#include <filesystem>

namespace joa::engine {
namespace {

using K = game::MissionEvent::Kind;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const game::UnitClass* cls(game::Mission& m, const game::Unit& u) {
    const auto& units = m.data().units;
    return u.cls >= 0 && u.cls < int(units.size()) ? &units[std::size_t(u.cls)] : nullptr;
}

// n of the equipped armour item "<HERO>_ARMOR_LVn", 0 when none.
int armour_lv(game::Mission& m, const game::Unit& u) {
    const auto& items = m.data().items;
    if (u.armor_item <= 0 || u.armor_item >= int(items.size())) return 0;
    const std::string& code = items[std::size_t(u.armor_item)].code;  // GameData::item_index: items[0] = none
    return code.size() > 4 && code.compare(code.size() - 4, 3, "_LV") == 0 ? code.back() - '0' : 0;
}

// Party portrait (FUN_006f9b60): CFileName + armour item <CFILE>_ARMOR_LVn. CONFIRMED (decomp strings).
std::string party_portrait(game::Mission& m, const game::Unit& u) {
    const game::UnitClass* c = cls(m, u);
    if (!c) return {};
    const std::string f = c->cfile;
    const int lv = armour_lv(m, u);
    auto armour = [&](const char* stem, const std::array<int, 3>& map) -> std::string {
        if (lv < 1 || lv > 3) return {};
        return std::string(stem) + "C" + std::to_string(map[std::size_t(lv - 1)]);
    };
    if (formats::iequals(f, "JOAN")) return armour("JOAN", {2, 1, 3});
    if (formats::iequals(f, "JEAN")) return armour("JEAN", {1, 2, 3});
    if (formats::iequals(f, "LAHIRE")) return armour("LAHIRE", {1, 2, 3});
    if (formats::iequals(f, "ALENCON")) return armour("ALENCO", {1, 2, 3});
    return f;  // RICHEMON, CHARLES, FLAVY, BASTARD: the CFileName itself
}

// Party class table DAT_00a6a570 (FUN_006f9670): slot order of the bottom-right portraits. CONFIRMED.
constexpr const char* kPartyClasses[8] = {"JOAN_OF_ARC", "JEAN_DE_METZ", "LAHIRE", "ALENCON",
                                          "ORLEAN",      "CHARLIE7",     "arthur_de_richemont", "FLAVY"};
// Minimap hero icons (FUN_0070fbe0, "_M" variant; "_H" = mounted, HYPOTHESIS: not distinguished).
constexpr std::pair<const char*, const char*> kHeroIcons[] = {
    {"JOAN_OF_ARC", "!joan_M"}, {"JEAN_DE_METZ", "!metz_M"}, {"ALENCON", "!alen_M"}, {"LAHIRE", "!lahi_M"},
    {"ORLEAN", "!bast_M"},      {"FLAVY", "!flav_M"},       {"CHARLIE7", "!char_M"}, {"arthur_de_richemont", "!rich_M"}};

int party_slot(game::Mission& m, const game::Unit& u) {
    const game::UnitClass* c = cls(m, u);
    for (int i = 0; c && i < 8; ++i)
        if (formats::iequals(c->code, kPartyClasses[i])) return i;
    return -1;
}

HudUnit hud_unit(game::Mission& m, const game::Unit& u) {
    game::World& w = m.world();
    HudUnit h;
    h.name = u.name;
    h.hp_value = u.hp, h.hp_max = std::max(1, w.max_hp(u));
    h.energy_value = u.ep, h.energy_max = std::max(1, w.max_ep(u));
    h.hp = std::clamp(float(u.hp) / float(h.hp_max), 0.0f, 1.0f);
    h.energy = std::clamp(float(u.ep) / float(h.energy_max), 0.0f, 1.0f);
    h.level = u.level;
    // PLVLBAR: (xp - LEVEL[level-1]) / (LEVEL[level] - LEVEL[level-1]), LEVEL[0] = 0 for level 1.
    const game::LevelRow* cur = m.data().level_row(u.level);
    const game::LevelRow* prev = u.level > 1 ? m.data().level_row(u.level - 1) : nullptr;
    const int lo = prev ? prev->exp : 0, hi = cur ? cur->exp : lo;
    h.xp = hi > lo ? std::clamp(float(u.xp - lo) / float(hi - lo), 0.0f, 1.0f) : 0.0f;
    h.ammo = u.inv.ammo.quantity;
    for (int id : u.members)
        if (const game::Unit* t = w.unit(id); t && t->id != u.id && !t->deleted && !w.dead(*t)) ++h.units;
    h.hostile = w.hostile_to_player(u);
    return h;
}

// Minimap icons (FUN_0071bf10): every world level's map/scenarioN/<node>.map, positions + the level origin
// (Level +0x44/+0x48). File: i32 count, records of 0xa4 bytes: +0x02 name[64] (the
// set_minimap_icon_display_enable key), +0x44 MAPICON sprite, +0x92 i32 x, +0x96 i32 y. Layout measured from
// S1L1.map (HIGH); records are shown unless a script disabled them (HYPOTHESIS: the s04_04 HUD shows both camps).
void load_map_icons(const formats::Vfs& vfs, const std::string& map_dir, const game::WorldNode& node,
                    std::vector<std::pair<std::string, HudMapIcon>>& out) {
    const auto b = vfs.read(map_dir + "/" + node.name + ".map");
    if (!b || b->size() < 4) return;
    auto i32 = [&](std::size_t o) { return std::int32_t((*b)[o] | (*b)[o + 1] << 8 | (*b)[o + 2] << 16 | (*b)[o + 3] << 24); };
    auto str = [&](std::size_t o, std::size_t n) {
        std::string s(reinterpret_cast<const char*>(b->data() + o), n);
        return s.substr(0, s.find('\0'));
    };
    const int n = i32(0);
    for (int i = 0; i < n && 4 + std::size_t(i + 1) * 0xa4 <= b->size(); ++i) {
        const std::size_t r = 4 + std::size_t(i) * 0xa4;
        HudMapIcon icon;
        icon.world = {node.ox + float(i32(r + 0x92)), node.oy + float(i32(r + 0x96))};
        icon.sprite = str(r + 0x44, 12);
        out.emplace_back(str(r + 2, 64), std::move(icon));
    }
}

// World +0x54/+0x58 extent (FUN_0044e8c0): <world>.wor CreateGrid X / Y in 2048-unit blocks (S1L1 8 x 8 = 16384).
glm::vec2 world_extent(const formats::Vfs& vfs, const std::string& map_dir, const std::string& world) {
    const auto b = vfs.read(map_dir + "/" + world + ".wor");
    const auto wor = b ? formats::parse_block_text(std::string_view(reinterpret_cast<const char*>(b->data()), b->size())) : std::nullopt;
    if (wor)
        for (const auto& g : wor->blocks)
            if (g.type == "CreateGrid") return glm::vec2(float(g.number("X")), float(g.number("Y"))) * 2048.0f;
    return glm::vec2(16384.0f);
}

// Armour-dependent heroes (FUN_004ab330 / FUN_004aa3c0): <stem>1..3 from <hero>_ARMOR_LVn of the hero unit.
std::string hero_portrait(game::Mission& m, const game::Unit* u, const char* stem, const char* hero_class) {
    if (!u)
        for (const game::Unit& o : m.world().units())
            if (const game::UnitClass* c = cls(m, o); !o.deleted && c && formats::iequals(c->code, hero_class)) u = &o;
    return std::string(stem) + std::to_string(u ? std::clamp(armour_lv(m, *u), 1, 3) : 1);
}

// Attention message as the HUD shows it: icon by speaker (FUN_004aa3c0 / FUN_004ab330) and, with a speaker,
// "_<name>:_ <text>" (strings 0x9cf774 / 0x9d4718, HIGH).
HudMessage hud_message(game::Mission& m, const game::MissionUi::AttentionMessage& a) {
    HudMessage h{a.text, lower(message_portrait(m, a.speaker, a.icon)), m.scenario_no(), 0.0f};
    const bool plain = !a.speaker && (a.icon.empty() || formats::iequals(a.icon, "M0000") || formats::iequals(a.icon, "TMESS"));
    if (!plain) {
        const game::Unit* u = a.speaker ? m.world().unit(a.speaker) : nullptr;
        h.text = "_" + (u ? u->name : a.icon) + ":_ " + a.text;
    }
    return h;
}

}  // namespace

// FUN_004ab330 (attention message by unit) / FUN_00639b40 (cut-scene dialog box): unit class -> MESSAGE record.
// FUN_004aa3c0 (attention message by speaker name): name -> record. CONFIRMED tables (local OVERLAY_RE §8).
std::string message_portrait(game::Mission& m, int unit, std::string_view name) {
    static const std::pair<const char*, const char*> kByClass[] = {
        {"ORLEAN", "BASTARD"}, {"FLAVY", "FLAVY"}, {"BEGLEY", "FLAVY"}, {"BEDFORD", "BEDFORD"},
        {"ENGLISH_POLEMAN_CAPTAIN", "EPOLECAP"}, {"ENGLISH_LIGHT_INFANTRY_CAPTAIN", "ELINFCAP"},
        {"ENGLISH_HEAVY_INFANTRY_CAPTAIN", "EHINFCAP"}, {"ENGLISH_MACEMAN_CAPTAIN", "EMACECAP"},
        {"ENGLISH_LIGHT_INFANTRY", "ELINF"}, {"ENGLISH_SPY", "ELINF"}, {"BUR_LIGHTINFANTRY", "BLINF"},
        {"BUR_LIGHTINFANTRY_CAPTAIN", "BLINFC"}, {"BUR_ARCHER_CAPTAIN", "BLINFC"}, {"FRENCH_ARCHER_CAPTAIN", "F_ARCCAP"},
        {"FRENCH_CROSSBOW_MAN_CAPTAIN", "FCROSSC"}, {"FRENCH_HEAVY_INFANTRY_CAPTAIN", "FHINFCAP"}, {"DUNOIS", "FHINFCAP"},
        {"DLLLIER", "FHINFCAP"}, {"FRENCH_LIGHT_INFANTRY_CAPTAIN", "FLINFCAP"}, {"FRENCH_POLEMAN_CAPTAIN", "FPOLECAP"},
        {"FRENCH_ARCHER", "F_ARCH"}, {"FRENCH_CROSSBOW_MAN", "FCROSS"}, {"FRENCH_HEAVY_INFANTRY", "FHINF"},
        {"FRENCH_LIGHT_INFANTRY", "FLINF"}, {"FRENCH_POLEMAN", "FPOLE"}, {"FRENCH_MACEMAN", "FMACE"},
        {"FRENCH_MACEMAN_CAPTAIN", "FMACECAP"}, {"WILLIAM_GLASDALE", "GLASDALE"}, {"ARTHUR_DE_RICHEMONT", "RICHEMON"},
        {"LORD_SCALES", "SCALES"}, {"JOHN_FASTOLF", "FASTOLF"}, {"ENGLISH_MACEMAN", "EMACE"}, {"LORD_TALBOT", "TALBOT"},
        {"PRIEST_01", "C_PRIEST"}, {"POTION_SHOPKEEPER", "SHOPPE"}, {"POTION_SHOPKEEPER_2", "SHOPPE"},
        {"WEAPON_SHOPKEEPER", "BLACKSM"}, {"WEAPON_SHOPKEEPER_2", "BLACKSM"}, {"william_de_la_pole", "WILLIAM"},
        {"ALEXANDER", "ALEXDLP"}, {"John_de_la_pole", "JOHNDLP"}, {"Luxembourg", "LUX"}, {"MATTHEWGOUGH", "GOUGH"},
        {"Perrinet", "GRESSART"}, {"Philip_the_good", "phillp"}, {"WANDOMME", "WANDOMME"}, {"HUDLEG", "WANDOMME"},
        {"CITIZEN_01", "CITI1"}, {"CITIZEN_01_2", "CITI1"}, {"CITIZEN_02B", "CITI1A"}, {"CITIZEN_02B_2", "CITI1A"},
        {"CITIZEN_02A", "CITI2B"}, {"CITIZEN_02A_2", "CITI2B"}, {"CITIZEN_03", "CITI3A"}, {"CITIZEN_03_2", "CITI3A"},
        {"CITIZEN_04A", "CITI4"}, {"CITIZEN_04A_2", "CITI4"}, {"CITIZEN_04B", "CITI4B"}, {"CITIZEN_04B_2", "CITI4B"},
        {"CHARLIE7", "CHARLES"}, {"PRISONER_01", "CITI1"}, {"COUNCIL_01", "CITI1"}, {"ENGLISH_ARCHER_CAPTAIN", "EARCCAP"},
        {"ENGLISH_EX_ARCHER_CAPTAIN", "EARCCAP"}, {"MER_POLEMAN", "MPOLE"}, {"MER_POLEMAN_CAPTIAN", "MPOLECAP"},
        {"MER_LIGHT_INFANTRY_CAPTAIN", "MLTINFC"}, {"MER_LIGHT_INFANTRY", "MLTINF"}};
    static const std::pair<const char*, const char*> kByName[] = {
        {"Pierre Ossier", "CITI3A"}, {"Renaud Encrier", "CITI3A"}, {"Renier Chanevacier", "CITI3A"},
        {"Eudes Masuier", "CITI3A"}, {"Capellarius", "CITI1"}, {"Guillaume de Flavy", "FLAVY"},
        {"French Archer Captain", "F_ARCCAP"}, {"French CrossbowMan Captain", "FCROSSC"},
        {"French Heavy Infantry Captain", "FHINFCAP"}, {"French Poleman Captain", "FPOLECAP"},
        {"Sir William Glasdale", "GLASDALE"}, {"d'Illiers", "FHINFCAP"}, {"Dunois", "FHINFCAP"},
        {"Arthur de Richemont", "RICHEMON"}, {"Regent Bedford", "Bedford"}, {"Lord Talbot", "TALBOT"},
        {"William de la Pole", "WILLIAM"}, {"Alexander de la Pole", "ALEXDLP"}, {"John de la Pole", "JOHNDLP"},
        {"Marcel Parcheminier", "F_ARCCAP"}, {"Radulfus Bradcroft", "EHINFCAP"}, {"Jacobus Sprode", "EHINFCAP"},
        {"Nicholas Weston", "ELINFCAP"}, {"Triston Walsoken", "EMACECAP"}, {"Tower Guard", "F_ARCCAP"},
        {"Female Citizen", "CITI4"}, {"Renier Balancier", "flinfcap"}, {"Charles VII", "CHARLES"},
        {"Reiner Mille", "EARCCAP"}, {"Luxembourg", "LUX"}, {"WANDOMME", "WANDOMME"}, {"HUDLEG", "WANDOMME"},
        {"MATTHEWGOUGH", "GOUGH"}, {"Perrinet", "GRESSART"}};
    if (unit) {
        const game::Unit* u = m.world().unit(unit);
        const game::UnitClass* c = u && !u->deleted ? cls(m, *u) : nullptr;
        if (!c) return "M0000";
        if (formats::iequals(c->code, "ALENCON")) return hero_portrait(m, u, "ALENCON", "ALENCON");
        if (formats::iequals(c->code, "LAHIRE")) return hero_portrait(m, u, "LAHIRE", "LAHIRE");
        if (formats::iequals(c->code, "JEAN_DE_METZ") || formats::iequals(c->code, "JEAN_DE_METZ_2"))
            return hero_portrait(m, u, "METZ", "JEAN_DE_METZ");
        for (const auto& [code, rec] : kByClass)
            if (formats::iequals(c->code, code)) return rec;
        return "FLINF";
    }
    if (name.empty() || formats::iequals(name, "M0000")) return "M0000";
    if (formats::iequals(name, "TMESS")) return "TMESS";
    if (formats::iequals(name, "Duke of Alencon")) return hero_portrait(m, nullptr, "ALENCON", "ALENCON");
    if (formats::iequals(name, "La Hire")) return hero_portrait(m, nullptr, "LAHIRE", "LAHIRE");
    if (formats::iequals(name, "Jean de Metz")) return hero_portrait(m, nullptr, "METZ", "JEAN_DE_METZ");
    if (name.find("Bastard") != std::string_view::npos) return "BASTARD";  // name at 0x9d4b34 (HYPOTHESIS: "Bastard of Orleans")
    for (const auto& [n, rec] : kByName)
        if (formats::iequals(name, n)) return rec;
    return "CITI1";
}

void GameUiBridge::reset(game::Mission& m, HudModel& hud) {
    m.ui().auto_advance = auto_advance;
    unit_dots_valid_ = false;
    live_messages_.clear();
    party_.clear();
    brief_shown_ = false;
    weapon_slot_data_ = nullptr;
    typed_id_ = 0, typed_ = 0.0f;
    win_time_ = over_time_ = -1.0f;
    end_menu_ = false;
    hit_start_.clear(), interact_shown_ = bribe_no_gold_ = false, target_ = 0;  // HudGaps
    bar_unit_ = char_unit_ = 0;  // HudFinish: FUN_00703750 snaps the panel bars on the next fill
    sms_events_.clear(), sms_.clear(), sms_state_ = 0, sms_time_ = 0.0f;  // FUN_006e50c0 / 0x6e5e00
    hud.messages.clear();
    hud.show_mission_brief = hud.show_message_log = hud.show_objective = hud.show_quickload_prompt = false;
    hud.brief_page = 0;
    hud.mission = m.scenario_no();
    hud_mapobj_reset(m, hud);  // HudGaps mapobj: objective panel list / state
}

void GameUiBridge::on_events(const std::vector<game::MissionEvent>& events, game::Mission& m, HudModel& hud) {
    for (const game::MissionEvent& e : events) switch (e.kind) {
            case K::AttentionMessage:
                if (e.b == 1) {  // posted (FUN_004ad710)
                    if (const auto* a = m.ui().attention_message(e.a))
                        live_messages_[e.a] = hud_message(m, *a);
                } else if (auto it = live_messages_.find(e.a); it != live_messages_.end()) {  // gone: FUN_00499e40 log
                    hud.message_log.push_back(std::move(it->second));
                    live_messages_.erase(it);
                }
                break;
            case K::ObjectiveAdd: case K::ObjectiveUpdate: case K::ObjectiveFinish: case K::ObjectiveFail:
            case K::ObjectiveShow: case K::ObjectiveHide: hud_mapobj_event(e, m, hud); break;  // HudGaps mapobj
            case K::Brief:
                if (e.text == "close") {  // headless auto-close
                    if (brief_shown_) hud.show_mission_brief = false;
                    brief_shown_ = false;
                } else {
                    hud.show_mission_brief = brief_shown_ = true;
                    hud.brief_page = 0;
                }
                break;
            default:
                if (!sms_event(e)) effects_event(e, m);  // SMS line, else HudGaps: interact indicator / item_hit target
                break;
        }
}

void GameUiBridge::fill(game::Mission& m, const game::Snapshot& snap, const glm::mat4& view_proj, glm::ivec4 viewport,
                        HudModel& hud) {
    game::World& w = m.world();
    game::MissionUi& mui = m.ui();
    hud.mission = m.scenario_no();
    hud.rts_mode = w.game_mode == 4;
    // The player closed a script-opened brief (BACK / Escape in hud_game): its WAIT ends.
    if (brief_shown_ && !hud.show_mission_brief) {
        brief_shown_ = false;
        if (mui.brief_open) mui.close_interface();
    }

    // ---- player panel ----
    const game::Unit* player = w.unit(w.player_id());
    if (player && !player->deleted) {
        hud.player = hud_unit(m, *player);
        const game::UnitClass* c = cls(m, *player);
        hud.player.portrait = c && !c->cfile.empty() ? c->cfile + "1" : "";  // class +0x100 + "1"
        hud.player_world = {player->pos.x, player->pos.y};
        hud.player_heading = player->yaw_rad();
    }

    // ---- party (FUN_006ff950): live units of the player's nation, rank > 3, class in the party table ----
    std::array<int, 8> slots{};
    if (player)
        for (int id = int(w.units().size()); id >= 1; --id) {
            const game::Unit& u = w.units()[std::size_t(id - 1)];
            if (u.deleted || u.nation != player->nation || u.rank <= 3 || w.dead(u)) continue;
            if (const int s = party_slot(m, u); s >= 0) slots[std::size_t(s)] = id;
        }
    party_.clear();
    hud.party.clear();
    for (int id : slots)
        if (id) {
            const game::Unit& u = *w.unit(id);
            HudUnit h = hud_unit(m, u);
            h.portrait = party_portrait(m, u);
            h.holding = m.ai().is_holding(u);
            h.selected = id == w.player_id();
            h.id = id;
            party_effects(m, u, h);  // HudGaps
            hud.party.push_back(std::move(h));
            party_.push_back(id);
        }

    // ---- name bars (FUN_00520e80 text + FUN_00520270 LifeBarre at head + 95) ----
    hud.name_bars.clear();
    // Projects a 16x2-unit LifeBarre billboard centred at p: false when off screen.
    auto billboard = [&](const glm::vec3& p, HudUnit& h) {
        const glm::vec4 c = view_proj * glm::vec4(p, 1.0f);
        if (c.w <= 1.0f) return false;
        const glm::vec3 ndc = glm::vec3(c) / c.w;
        if (std::abs(ndc.x) > 1.05f || std::abs(ndc.y) > 1.05f || ndc.z > 1.0f) return false;
        // Billboard size: design px per world unit (vertical, 16 units up) / the reference 54 px per 16 units.
        const glm::vec4 c2 = view_proj * glm::vec4(p + glm::vec3(0, 16, 0), 1.0f);
        const float px = std::abs(c2.y / c2.w - ndc.y) * 0.5f * 768.0f;
        h.screen = {viewport.x + (ndc.x * 0.5f + 0.5f) * viewport.z, viewport.y + (0.5f - ndc.y * 0.5f) * viewport.w};
        h.bar_scale = std::clamp(px / 54.0f, 0.25f, 1.5f);
        h.on_screen = h.bar_scale > 0.3f;  // ponytail: far bars dropped by size; the original cull distance is unknown
        return true;
    };
    if (show_names && !mui.game_cut_scene && viewport.z > 0 && viewport.w > 0)
        for (const game::UnitView& v : snap.units) {
            if (v.dead || v.id == w.player_id()) continue;
            const game::Unit* u = w.unit(v.id);
            if (!u || u->deleted || u->name.empty()) continue;
            HudUnit h = hud_unit(m, *u);
            h.hp = std::clamp(float(v.hp) / float(std::max(1, v.max_hp)), 0.0f, 1.0f);
            if (billboard(to_engine(v.pos.x, v.pos.y, v.pos.z + 95.0f), h)) hud.name_bars.push_back(std::move(h));
        }
    // Workshop build bar (FUN_0046ba20 LifeBarre + bar_workshop at the facility + 200 z, shown by 0x46c4d0 while the
    // current weapon builds, fill = clamp(elapsed / build ms) by 0x46c540): a scene object, not tied to the names.
    if (!mui.game_cut_scene && viewport.z > 0 && viewport.w > 0)
        for (const game::Facility& f : m.places().facilities()) {
            if (f.kind != game::Facility::Workshop || f.current < 0 || f.current >= int(f.builds.size())) continue;
            const game::Facility::Build& b = f.builds[std::size_t(f.current)];
            const std::uint32_t now = std::uint32_t(w.now());
            if (now >= b.start_ms + std::uint32_t(b.ms)) continue;   // 0x46c460 hides it when done
            HudUnit h;
            h.workshop_bar = true;
            h.hp = std::clamp(float(now - b.start_ms) / float(std::max(1, b.ms)), 0.0f, 1.0f);
            if (billboard(to_engine(f.pos.x, f.pos.y, f.pos.z + 200.0f), h)) hud.name_bars.push_back(std::move(h));
        }

    // ---- minimap (FUN_0071bf10 / FUN_0071b6a0): everything follows the mission's world - INTERFAC.SET table
    // <world> tiles (S9L1 plays scenario 5's S5L1w), extent World +0x54/+0x58, every level's icons ----
    const std::string& wf = m.info().step.world_file;  // "scenario5\S5L1w"
    const std::string world = wf.substr(wf.find_last_of("\\/") + 1);
    hud.scenario = world.substr(0, world.size() - 1);  // table <scenario>W
    hud.brief_rotation = mui.minimap_rotation;
    hud.map_icons.clear();
    hud.minimap_waypoints.clear();
    if (map_icons_key_ != m.map_dir() + world) {
        map_icons_key_ = m.map_dir() + world, map_icons_.clear();
        for (const game::WorldNode& n : game::world_nodes(vfs_, m.map_dir(), m.info().step)) load_map_icons(vfs_, m.map_dir(), n, map_icons_);
        map_size_ = world_extent(vfs_, m.map_dir(), world);
    }
    hud.map_size = map_size_;
    for (const auto& [name, icon] : map_icons_) {
        auto it = mui.minimap_icons.find(name);
        if (it == mui.minimap_icons.end() || it->second) hud.map_icons.push_back(icon);
    }
    for (const auto& wp : mui.waypoints) {
        if (wp.type == 0) continue;
        glm::vec2 p{wp.pos[0], wp.pos[1]};
        if (const game::Unit* f = wp.follow ? w.unit(wp.follow) : nullptr) p = {f->pos.x, f->pos.y};
        hud.minimap_waypoints.push_back({p, "wp_" + std::to_string(std::clamp(int(wp.type), 1, 7))});  // MAPICON wp_1..wp_7
    }
    hud_mapobj_fill(m, hud);  // HudGaps mapobj: vision mask sources, objective lines
    // Unit dots: units x vision areas (0.5+ ms with armies). Units move only with the simulation tick; the vision areas
    // can also change between ticks (script areas, minimap toggle), so the key covers them too.
    std::uint64_t key = 1469598103934665603ull;
    const auto mix = [&](std::uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    const auto bits = [](float f) { std::uint32_t u; std::memcpy(&u, &f, sizeof u); return std::uint64_t(u); };
    mix(std::uint64_t(std::uint32_t(snap.time_ms))), mix(hud.minimap_fog);
    for (const HudModel::VisionArea& a : hud.minimap_vision) {
        mix(std::uint64_t(a.shape)), mix(bits(a.r));
        for (const glm::vec2& q : a.p) mix(bits(q.x) << 32 | bits(q.y));
    }
    if (!unit_dots_valid_ || key != unit_dots_key_) {
        unit_dots_valid_ = true, unit_dots_key_ = key;
        unit_dots_.clear();
        for (const game::Unit& u : w.units()) {  // FUN_0070fbe0
            if (u.deleted || w.dead(u) || u.id == w.player_id()) continue;
            const glm::vec2 p{u.pos.x, u.pos.y};
            if (!hud_vision_visible(hud, p)) continue;  // HudGaps mapobj: dot only on a set mask pixel
            if (w.hostile_to_player(u)) {
                unit_dots_.push_back({p, "", HudMapIcon::Dot::Red});
                continue;
            }
            const game::UnitClass* c = cls(m, u);
            const char* hero = nullptr;
            for (const auto& [code, icon] : kHeroIcons)
                if (c && formats::iequals(c->code, code)) hero = icon;
            if (hero) unit_dots_.push_back({p, hero});
            else unit_dots_.push_back({p, "", HudMapIcon::Dot::Green});
        }
    }
    hud.map_icons.insert(hud.map_icons.end(), unit_dots_.begin(), unit_dots_.end());

    // ---- objectives (brief: end conditions with a description, Win records first; HUD panel: hud_mapobj) ----
    hud.brief_objectives.clear();
    const auto& ends = m.state().end;
    for (int pass = 0; pass < 2; ++pass)
        for (const game::EndCondition& e : ends) {
            if (!e.has_des || !e.enabled || e.type == game::EndCondition::Lose) continue;
            if ((e.type == game::EndCondition::Win) != (pass == 0)) continue;
            hud.brief_objectives.push_back({e.type, e.finish, e.des, e.territory_link, e.gold, e.exp, e.awarded != 0});
        }
    hud.mission_time = int(m.state().passed_ms / 1000);
    hud.total_time = int(m.state().play_ms / 1000);
    hud.brief_strategy = hud.brief_siege = m.scenario_no() >= 5;

    // ---- brief: commanders (0x4cdd40: descending recno, rank > 3; FRENCH / ENGLISH / MERCEN) and siege weapons ----
    // Read only by the brief pages (hud_brief), i.e. while show_mission_brief: built then (every opening path sets the
    // flag before this fill), not every frame.
    if (hud.show_mission_brief) fill_brief(m, hud);

    // ---- live attention messages (FUN_004ace90 states 3..5: the panel is open; typed = chars at 0.04/ms) ----
    hud.messages.clear();
    for (const auto& a : mui.messages)
        if (a.state >= 3 && a.state <= 5) {
            HudMessage h = hud_message(m, a);
            h.age = a.len > 0 ? a.typed / float(a.len) * float(h.text.size()) / 40.0f : 0.0f;  // hud_game reveals age*40 chars
            hud.messages.push_back(std::move(h));
        }
    fill_effects(m, hud);  // HudGaps: interact indicator, target HUD, effect clock
    hud_crit_fill(m, hud);  // HudGaps crit: crit sequence/banner, kill interface counters
}

void GameUiBridge::fill_brief(game::Mission& m, HudModel& hud) {
    game::World& w = m.world();
    const game::GameData& gd = m.data();
    static constexpr const char* kWeapons[6] = {"BATTERRAM", "CROSSBOW", "BOMBARD", "CANNON", "FAUCONNEAU", "TREBUCHET"};
    static constexpr const char* kNations[3] = {"FRENCH", "ENGLISH", "MERCEN"};
    if (weapon_slot_data_ != &gd || weapon_slot_.size() != gd.units.size()) {  // class index -> kWeapons slot, -1 none
        weapon_slot_data_ = &gd;
        weapon_slot_.assign(gd.units.size(), -1);
        for (std::size_t i = 0; i < gd.units.size(); ++i)
            for (int k = 0; k < 6; ++k)
                if (formats::iequals(gd.units[i].code, kWeapons[k])) weapon_slot_[i] = std::int8_t(k);
    }
    int nation_ids[3];
    for (int k = 0; k < 3; ++k) nation_ids[k] = gd.nation_id(kNations[k]);
    hud.brief_commanders.clear();
    hud.brief_weapons.fill(false);
    for (int id = int(w.units().size()); id >= 1; --id) {
        const game::Unit& u = w.units()[std::size_t(id - 1)];
        if (u.deleted || w.dead(u)) continue;
        if (cls(m, u)) {
            if (const int k = weapon_slot_[std::size_t(u.cls)]; k >= 0) hud.brief_weapons[std::size_t(k)] = true;
        }
        if (u.rank <= 3 || int(hud.brief_commanders.size()) >= 31) continue;
        int nation = -1;
        for (int k = 0; k < 3; ++k)
            if (u.nation == nation_ids[k]) nation = k;
        if (nation < 0) continue;
        BriefCommander bc{u.name, nation, w.hostile_to_player(u), {}};
        for (int mid : u.members)
            if (const game::Unit* t = w.unit(mid); t && t->id != u.id && !t->deleted) {
                const game::UnitClass* tc = cls(m, *t);
                bc.team.push_back({tc ? tc->code : "", t->level, std::clamp(float(t->hp) / float(std::max(1, w.max_hp(*t))), 0.0f, 1.0f)});
            }
        hud.brief_commanders.push_back(std::move(bc));
    }
}

bool GameUiBridge::handle(const UiCommand& c, game::Mission& m) {
    game::World& w = m.world();
    switch (c.type) {
        case UiCommand::Type::HoldResume:
            if (c.value >= 0 && c.value < int(party_.size()))
                if (game::Unit* u = w.unit(party_[std::size_t(c.value)]))  // Unit_toggle_hold 0x5121c0 (+ its voice)
                    m.ai().toggle_hold(*u, true), hold_voice(m, *u);
            return true;
        default: return false;
    }
}

// Shown as the original's info panel: date FUN_00580350 + DATE|1 ("October 13, 2026"), time FUN_00580510
// "%d:%.2d %s" (12 h, AMPM|1/2), local time. Order: GameFile_enum_saves 0x4770a0 + comparator 0x477bd0 (saved
// time descending); native and imported saves share one list.
std::vector<SaveSlot> list_saves(const formats::Vfs& vfs) {
    static constexpr const char* kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                                                "July",    "August",   "September", "October", "November", "December"};
    std::vector<std::pair<std::int64_t, SaveSlot>> found;  // UTC seconds, slot
    auto add = [&](std::int64_t utc, const std::tm& tm, std::string file, std::string desc, int mission) {
        char time[16];
        std::snprintf(time, sizeof time, "%d:%.2d %s", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
        found.push_back({utc, {std::move(file), std::move(desc), mission,
                               std::string(kMonths[tm.tm_mon]) + " " + std::to_string(tm.tm_mday) + ", " + std::to_string(tm.tm_year + 1900),
                               time}});
    };
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(game::native_save_dir(), ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".jsav") continue;
        const auto meta = Session::read_meta(e.path());
        if (!meta) continue;
        const std::time_t t = meta->unix_time;
        std::tm tm{};
        localtime_r(&t, &tm);
        add(meta->unix_time, tm, e.path().string(), meta->description, meta->mission);
    }
    if (const auto dir = vfs.resolve("Save"))
        for (const auto& e : std::filesystem::directory_iterator(*dir, ec)) {
            if (!e.is_regular_file() || !formats::iequals(e.path().extension().string(), ".sav")) continue;
            const auto bytes = formats::read_file(e.path());
            const auto sav = bytes ? formats::parse_sav(*bytes) : std::nullopt;
            if (!sav) continue;
            const std::time_t t = formats::filetime_to_unix(sav->header.saved_local);  // already local
            std::tm tm{};
            gmtime_r(&t, &tm);
            add(formats::filetime_to_unix(sav->header.saved_utc), tm, e.path().string(), sav->header.description,
                int(sav->header.value_c));
        }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<SaveSlot> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

}  // namespace joa::engine
