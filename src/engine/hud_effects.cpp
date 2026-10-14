// HudGaps: HUD effect state + draws the stock HUD lacked.
// - Interact indicator FUN_006d3590 (targets, each tick) / draw FUN_006d23b0: GMSG BG+BORDER at (384,100), text
//   Goudy12(18) yellow (font obj 0xa58ac0) wrapped + centred both ways (0x626e00) in (392,106)-(652,162); the bribe
//   line (0x626320) in (820,220)-(1000,320) without a box. Texts t_interf GMSG 2..12, item Description (+0x20); in
//   RTS mode only t_interf RTS 1 (crew needed) for the hovered siege weapon (Rts_pick_target DAT_00a5704d).
// - Target HUD (TUI set, head of FUN_006f9b60): set by FUN_006fe280 (unit hit by the player: Melee_tryHit /
//   Ranged_tryHit / Unit_onMessage) / FUN_006fe340 (gate / wall hit by the player), cleared 3000 ms after the
//   last set (HUD tick FUN_006fd8b0 = hud_tick, every game tick, which also chases the shown HP by <= 40 and the
//   player panel bars by <= 9 HP / <= 3 EP / +500 EXP).
// - Party hit flashes (status id 1 of +0x247, added by Unit_onMessage 0x75d540 on damage), danger blink
//   (GAMEPLAY.RES {DANGER} INDANGER), CSELECT/MSELECT blink, morale pips (FUN_006ffb40), half alpha for busy /
//   off-minimap units.
#include "engine/hud_effects.hpp"

#include "engine/game_ui_bridge.hpp"
#include "engine/hud_crit.hpp"
#include "engine/hud_exp.hpp"
#include "engine/menu_options_store.hpp"
#include "engine/ui_screens.hpp"
#include "formats/vfs_fs.hpp"
#include "game/ai_system.hpp"
#include "game/interact_player.hpp"
#include "game/mount_ride.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace joa::engine {
namespace {

const game::UnitClass* class_of(game::Mission& m, const game::Unit& u) {
    return u.cls >= 0 && std::size_t(u.cls) < m.data().units.size() ? &m.data().units[std::size_t(u.cls)] : nullptr;
}

// FUN_00703880: TUI portrait / siege icon of a targeted unit's class (unit.res code), "" = none.
std::string target_portrait(std::string_view code) {
    static constexpr std::pair<const char*, const char*> kTable[] = {
        {"ALEXANDER", "ALEXDLP"}, {"Batterram", "Batter"}, {"bombard", "bombard"}, {"CANNON", "canon"},
        {"CROSSBOW", "CROSSBOW"}, {"AUTOBOW", "CROSSBOW"}, {"BEGLEY", "BEGLEY"}, {"AUTOBOW2", "CROSSBOW"},
        {"TREBUCHET", "TRE"}, {"fauconneau", "FAU"}, {"SIEGE_TOWER", "SIEGE"}, {"GOLD_CART", "CART"},
        {"GOLD_CART5", "CART"}, {"MER_POLEMAN", "MPOLE"}, {"MER_POLEMAN_CAPTAIN", "MPOLECAP"}, {"MER_ARCHER", "M_ARC"},
        {"MER_EX_ARCHER", "M_ARC"}, {"MER_ARCHER_CAPTAIN", "M_ARCC"}, {"MER_EX_ARCHER_CAPTAIN", "M_ARCC"},
        {"MER_LIGHT_INFANTRY", "mltinf"}, {"MER_LIGHT_INFANTRY_CAPTAIN", "mltinfc"}, {"LORD_TALBOT", "TALBOT"},
        {"BUR_ARCHER", "BURA"}, {"BUR_ARCHER_CAPTAIN", "BURAC"}, {"BUR_HEAVYINFANTRY", "BURHI"},
        {"BUR_HEAVYINFANTRY_CAPTAIN", "BURHIC"}, {"BUR_LIGHTINFANTRY", "BURLI"}, {"BUR_LIGHTINFANTRY_CAPTAIN", "BURLIC"},
        {"John_de_la_pole", "JOHNDLP"}, {"william_de_la_pole", "WILLDLP"}, {"Luxembourg", "LUX"},
        {"MATTHEWGOUGH", "GOUGH"}, {"MARTINGOUGH", "GOUGH"}, {"Perrinet", "GRESSART"}, {"BEDFORD", "BEDFORD"},
        {"Philip_the_good", "PHILIP"}, {"WANDOMME", "WANDOMME"}, {"HUDLEG", "WANDOMME"}, {"ENGLISH_ARCHER", "ENGA"},
        {"ENGLISH_ARCHER_CAPTAIN", "ENGAC"}, {"ENGLISH_EX_ARCHER", "ENGA"}, {"ENGLISH_EX_ARCHER_CAPTAIN", "ENGAC"},
        {"ENGLISH_HEAVY_INFANTRY", "ENGHI"}, {"ENGLISH_HEAVY_INFANTRY_CAPTAIN", "ENGHIC"},
        {"ENGLISH_LIGHT_INFANTRY", "ENGLI"}, {"ENGLISH_LIGHT_INFANTRY_CAPTAIN", "ENGLIC"}, {"ENGLISH_POLEMAN", "ENGP"},
        {"ENGLISH_POLEMAN_CAPTAIN", "ENGPC"}, {"ENGLISH_MACEMAN", "ENGM"}, {"ENGLISH_MACEMAN_CAPTAIN", "ENGMC"},
        {"ENGLISH_CROSSBOW_MAN", "ENGC"}, {"ENGLISH_CROSSBOW_MAN_CAPTAIN", "ENGCC"}, {"WILLIAM_GLASDALE", "GLASDALE"},
        {"LORD_SCALES", "SCALES"}, {"JOHN_FASTOLF", "FASTOLF"}};
    for (const auto& [c, s] : kTable)
        if (formats::iequals(code, c)) return s;
    return {};
}

std::string text_of(Ui& ui, int line) { return std::string(ui.text("GMSG", line)); }

// Real HP of a target HUD unit / structure item (+0x135 / item +0xc2|+0xc6), -1 = gone.
int target_real_hp(game::Mission& m, int id, bool unit) {
    if (unit) {
        const game::Unit* u = id ? m.world().unit(id) : nullptr;
        return u && !u->deleted ? u->hp : -1;
    }
    const game::MapItem* it = id ? m.objects().item(id) : nullptr;
    return it ? it->hp : -1;
}

}  // namespace

std::string ui_format(std::string_view f, const std::vector<std::string>& args) {
    std::string out;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%' || i + 1 >= f.size()) {
            out += f[i];
            continue;
        }
        if (f[i + 1] == '%') {
            out += '%', ++i;
            continue;
        }
        std::size_t j = i + 1;
        int n = 0;
        while (j < f.size() && f[j] >= '0' && f[j] <= '9') n = n * 10 + (f[j++] - '0');
        if (n == 0 || j >= f.size()) {
            out += f[i];
            continue;
        }
        const std::string arg = n <= int(args.size()) ? args[std::size_t(n - 1)] : std::string();
        if ((f[j] == '?' || f[j] == 'j' || f[j] == 'p' || f[j] == 'q') && j + 1 < f.size() && f[j + 1] == '{') {
            const std::size_t end = f.find('}', j);
            if (end == std::string_view::npos) {
                out += f[i];
                continue;
            }
            // %N?{yes|no} (argument non-zero), %Nj{1st|2nd|...} (alternative N, none out of range),
            // %Np{one|many} (0 / 1 -> one), %Nq{zero|one|many} (0x5a6660 second pass).
            const int v = std::atoi(arg.c_str());
            const int pick = f[j] == '?' ? (!arg.empty() && arg != "0" ? 0 : 1)
                             : f[j] == 'j' ? v - 1
                             : f[j] == 'p' ? (v == 0 || v == 1 ? 0 : 1)
                                           : (v == 0 ? 0 : v == 1 ? 1 : 2);
            std::size_t a = j + 2;
            for (int k = 0; a <= end; ++k) {
                std::size_t b = f.find('|', a);
                if (b == std::string_view::npos || b > end) b = end;
                if (k == pick) {
                    out += f.substr(a, b - a);
                    break;
                }
                a = b + 1;
            }
            i = end;
            continue;
        }
        if (f[j] == 's' || f[j] == 'd') {
            out += arg;
            i = j;
            continue;
        }
        out += f[i];
    }
    return out;
}

std::string interact_text(Ui& ui, const HudModel::Interact& in) {
    switch (in.line) {
        case -1: return {};
        case 0: return in.item_text;  // already formatted in fill (key / gate state)
        case 1: return ui_format(std::string(ui.text("RTS", 1)), {std::to_string(in.item_arg), in.name});  // FUN_004de080
        case 2: return ui_format(ui_format(text_of(ui, 2), {in.name}), {in.key});  // FUN_004da8a0 then 0x5a6660(key)
        case 10: return text_of(ui, 10);                                             // no key (0x4daa60)
        default: return ui_format(ui_format(text_of(ui, in.line), {}), {in.key});
    }
}

std::string bribe_text(Ui& ui, const HudModel::Interact& in) {
    if (in.bribe_line == 5)  // FUN_004da960(name, cost - gold)
        return ui_format(text_of(ui, 5), {in.bribe_name, std::to_string(in.bribe_cost - in.gold)});
    if (in.bribe_line == 3)  // FUN_004da8e0(name, cost, gold) then 0x5a6660(key)
        return ui_format(ui_format(text_of(ui, 3), {in.bribe_name, std::to_string(in.bribe_cost), std::to_string(in.gold)}),
                         {in.key});
    return {};
}

void hud_interact(Ui& ui, const HudModel& m) {
    ui.frame(Anchor::Center);  // exe literals (not W-n / H-n)
    const UiFont* f = ui.font("Goudy12(18) yellow");
    if (!f) return;
    if (const std::string s = interact_text(ui, m.interact); !s.empty()) {
        ui.sprite("GMSG", "BG", {384, 100});
        ui.sprite("GMSG", "BORDER", {384, 100});
        // 0x626e00 (x1 392, y1 106, x2 652, y2 162, centred both ways)
        const float w = 652 - 392 + 1, lh = float(f->metrics.line_height());
        const float h = float(wrap_text(f->metrics, s, int(w)).size()) * lh;
        ui.text_block(f, s, {392, std::max(106.0f, std::floor(106 + (162 + 1 - 106 - h) / 2)), w, h + 1}, kUiWhite, 0,
                      TextAlign::Center);
    }
    if (const std::string s = bribe_text(ui, m.interact); !s.empty()) ui_markup_block(ui, f, s, {820, 220, 1000 - 820 + 1, 320 - 220 + 1}, kUiWhite);
}

void hud_target(Ui& ui, const HudModel& m) {
    const HudModel::Target& t = m.target;
    if (!t.active) return;
    ui.frame(Anchor::TopLeft);
    const UiSprite* bar = ui.sprite("TUI", "EHPBAR");
    auto hp_bar = [&](float x, float full) {  // ftol(shown / max * full), at least 1 px while the shown HP is non-zero
        int w = int(float(t.hp) / float(std::max(1, t.hp_max)) * full);
        if (t.hp != 0 && w < 2) w = 1;
        if (bar && w > 0) {
            const glm::vec2 uv0 = bar->uv0, uv1{bar->uv0.x + (bar->uv1.x - bar->uv0.x) * float(w) / float(bar->size.x), bar->uv1.y};
            ui.image(bar->texture, {x, 114, float(w), float(bar->size.y)}, uv0, uv1);
            ui.sprite("TUI", "EHPTIP", {x + float(w), 114});
        }
    };
    if (t.unit && !t.siege) {
        if (!t.icon.empty()) ui.sprite("TUI", t.icon, {0, 0x15});
        ui.sprite("TUI", "SWORD", {0, 0x68});
        ui.sprite("TUI", "SWORDBG", {0x2e, 0x72});
        hp_bar(0x2f, 94.0f);
    } else {  // siege weapon or structure
        hp_bar(0x43, 78.0f);
        ui.sprite("TUI", "SBASE", {0, 0x50});
        if (!t.icon.empty()) ui.sprite("TUI", t.icon, {8, 0x3c});
    }
    if (!t.text.empty())
        if (const UiFont* f = ui.font("Goudy12(18) target unit")) ui.text(f, t.text, {8, 0x82});  // 0x6255d0, font obj 0xa56da0
}

// ---------------------------------------------------------------- GameUiBridge side

void GameUiBridge::on_world_events(const std::vector<game::GameEvent>& events, game::Mission& m, HudModel& hud) {
    game::World& w = m.world();
    for (const game::GameEvent& e : events)
        if (e.type == game::Ev::Hit) {
            hit_start_[e.unit] = w.now();
            if (e.other == w.player_id() && e.other != 0) {  // FUN_006fe280: a new target snaps the shown HP
                if (target_ != e.unit || !target_unit_) target_hp_ = std::max(0, target_real_hp(m, e.unit, true));
                target_ = e.unit, target_unit_ = true, target_time_ = w.now();
            }
        }
    hud_exp_events(events, m, hud);
    hud_crit_events(events, hud);
    hud_tick(m);  // Game_process_tick: FUN_006fd8b0 runs after the units (whose hits set the target)
}

// FUN_006fd8b0 (Game_process_tick, every tick): panel bars chase the player's HP by <= 9, EP by <= 3, EXP by +500
// (a smaller gap or any drop: exact); the target HUD clears 3000 ms after the last set and its HP chases by <= 40
// (straight to 0 when the target's HP is 0).
void GameUiBridge::hud_tick(game::Mission& m) {
    game::World& w = m.world();
    if (const game::Unit* p = w.unit(w.player_id()); p && !p->deleted) {
        if (bar_unit_ != p->id) bar_unit_ = p->id, bar_hp_ = p->hp, bar_ep_ = p->ep, bar_exp_ = p->xp;  // FUN_00703750
        bar_hp_ += std::clamp(p->hp - bar_hp_, -9, 9);
        bar_ep_ += std::clamp(p->ep - bar_ep_, -3, 3);
        bar_exp_ += std::min(p->xp - bar_exp_, 500);
    }
    if (target_ && w.now() - target_time_ > 3000) target_ = 0, target_hp_ = 0;
    if (const int real = target_real_hp(m, target_, target_unit_); real >= 0)
        target_hp_ = real == 0 ? 0 : target_hp_ + std::clamp(real - target_hp_, -40, 40);
}

void GameUiBridge::effects_event(const game::MissionEvent& e, game::Mission& m) {
    using K = game::MissionEvent::Kind;
    game::World& w = m.world();
    if (e.kind == K::InteractPrompt) interact_shown_ = e.a != 0;
    else if (e.kind == K::Interact && e.text == "bribe_no_gold") bribe_no_gold_ = true;
    else if (e.kind == K::Ui && e.text == "item_hit" && e.b == w.player_id() && e.b != 0) {  // FUN_006fe340
        if (target_ != e.a || target_unit_) target_hp_ = std::max(0, target_real_hp(m, e.a, false));
        target_ = e.a, target_unit_ = false, target_time_ = w.now();
    }
}

// FUN_004e8500 no-gold branch: FUN_004ad710(MESSAGE 2) once (DAT_00a591b5) -> an ordinary attention message.
void GameUiBridge::post_effects(Ui& ui, game::Mission& m) {
    if (!bribe_no_gold_) return;
    bribe_no_gold_ = false;
    m.ui().add_attention_message(m, std::string(ui.text("MESSAGE", 2)), "M0000", 0, "", 0);
}

void GameUiBridge::party_effects(game::Mission& m, const game::Unit& u, HudUnit& h) {
    game::World& w = m.world();
    const auto it = hit_start_.find(u.id);
    h.hit_ms = it != hit_start_.end() && w.now() - it->second < 1000 ? w.now() - it->second : -1;
    h.danger = float(u.hp) / float(std::max(1, w.max_hp(u))) < m.data().c.indanger;  // 0x6f9b60: ratio < _DAT_00a3cd24
    h.morale = -1;
    int sum = u.morale, n = 1;
    for (int id : u.members)
        if (const game::Unit* t = w.unit(id); t && t->id != u.id && !t->deleted) sum += t->morale, ++n;
    if (n > 1) h.morale = sum / n;  // FUN_006ffb40
    h.busy = u.m.control != 0 || u.no_react;  // +0x2f / Unit_is_task_locked (+0x2e1)
    h.world = {u.pos.x, u.pos.y};
    // CSELECT: the character window's unit (FUN_00430340 / FUN_006e0df0), else in RTS mode the +0x1238 commander
    // (FUN_006f5ed0); MSELECT only for the character window's unit.
    h.commander_selected = char_unit ? char_unit == u.id : w.game_mode == 4 && rts_commander == u.id;
    h.troops_selected = char_unit != 0 && char_unit == u.id;
}

void GameUiBridge::fill_effects(game::Mission& m, HudModel& hud) {
    game::World& w = m.world();
    hud.clock_ms = w.now();
    if (char_unit != char_unit_) {  // FUN_006f7d80 / portrait switch: DAT_00a4d75f = DAT_00a500b2
        char_unit_ = char_unit;
        if (char_unit) hud.select_ms = hud.ui_ms;
    }
    // ---- player panel bars (+0x20/+0x24/+0x28, chased in hud_tick) ----
    if (const game::Unit* pl = w.unit(w.player_id()); pl && !pl->deleted) {
        if (bar_unit_ != pl->id) bar_unit_ = pl->id, bar_hp_ = pl->hp, bar_ep_ = pl->ep, bar_exp_ = pl->xp;  // FUN_00703750
        HudUnit& h = hud.player;
        h.hp_bar = std::clamp(float(bar_hp_) / float(std::max(1, h.hp_max)), 0.0f, 1.0f);
        h.energy_bar = std::clamp(float(bar_ep_) / float(std::max(1, h.energy_max)), 0.0f, 1.0f);
        const game::LevelRow* cur = m.data().level_row(pl->level);
        const game::LevelRow* prev = pl->level > 1 ? m.data().level_row(pl->level - 1) : nullptr;
        const int lo = prev ? prev->exp : 0, hi = cur ? cur->exp : lo;
        h.xp_bar = hi > lo ? std::clamp(float(bar_exp_ - lo) / float(hi - lo), 0.0f, 1.0f) : 0.0f;
    }
    // ---- interact indicator: the targets FUN_006d3590 stores, read back by FUN_006d23b0 in its order ----
    HudModel::Interact& in = hud.interact;
    in = {};
    game::Unit* p = w.unit(w.player_id());
    if (w.game_mode == 4) {  // RTS branch: crew text of the hovered siege weapon (DAT_00a5704d) while it lacks crew
        const game::Unit* s = w.unit(rts_siege);
        const game::UnitClass* c = s && !s->deleted && !m.ui().game_cut_scene ? class_of(m, *s) : nullptr;
        if (c && !w.hostile_to_player(*s)) {
            const int slots = int(c->attach_points.size());   // class +0x1ad
            int crew = 0;                                      // +0x378 entries not deleted
            for (int id : s->m.crew)
                if (const game::Unit* t = w.unit(id); t && !t->deleted) ++crew;
            if (crew < slots) in.line = 1, in.item_arg = slots, in.name = c->name;
        }
    } else if (interact_shown_ && p && !p->deleted && !m.ui().game_cut_scene) {
        static const MenuModel kDefaultMenu;
        in.key = action_key_name(vfs_, menu ? *menu : kDefaultMenu, 0x11);
        const int talk = game::find_talker(m, *p, 100.0f), bribe = game::find_bribable(m, *p, 200.0f);
        const int pris = game::find_prisoner(m, *p, 100.0f), rec = m.ai().find_recruitable(*p);
        int mount = 0;
        if (!p->mount) {
            mount = game::find_mount(w, *p, 2);
            if (const game::Unit* hs = w.unit(mount); !hs || hs->m.rider != 0) mount = 0;
        }
        const int mr = game::find_interact_item(m, *p, 200.0f);
        const int barrack = game::find_recruit_facility(m, *p), shop = game::find_workshop(m, *p);
        auto name_of = [&](int id) { const game::Unit* u = w.unit(id); return u ? u->name : std::string(); };
        if (pris) in.line = 4;
        else if (talk) in.line = 2, in.name = name_of(talk);
        else if (rec) in.line = 12;
        else if (const game::MapItem* it = mr > 0 ? m.objects().item(m.objects().modify_item(mr)) : nullptr;
                 it && m.objects().res(it->res) && !m.objects().res(it->res)->description.empty()) {
            const std::string& d = m.objects().res(it->res)->description;
            in.line = 0;
            if (it->cls == 1) in.item_text = ui_format(d, {in.key, std::to_string(it->state)});   // gate: (key, +0xd1)
            else if (it->cls == 10 || it->cls == 11) in.item_text = ui_format(d, {in.key});        // box / ladder
            else in.item_text = d;                                                                 // raw
        } else if (mount) in.line = 11;
        else if (barrack) in.line = 6;
        else if (const game::Facility* fc = shop ? m.places().facility(shop) : nullptr) {
            in.line = 7 + std::clamp(fc->workshop_type, 0, 2);   // +0x79: 0 siege house, 1 wood, 2 iron
            if (fc->workshop_type != 0) {                         // resources: only Alencon (the player) within 1024
                const int ac = m.data().unit_index("ALENCON");
                bool near = false;
                for (const game::Unit& a : w.units())
                    if (!a.deleted && ac >= 0 && a.cls == ac) {
                        const float dx = a.pos.x - fc->pos.x, dy = a.pos.y - fc->pos.y, dz = a.pos.z - fc->pos.z;
                        near = a.id == p->id && std::sqrt(dx * dx + dy * dy + dz * dz) < 1024.0f;
                        break;
                    }
                if (!near) in.line = 10;
            }
        }
        if (const game::Unit* b = w.unit(bribe); b && !b->deleted) {
            const game::UnitClass* c = class_of(m, *b);
            in.bribe_name = c && c->ranking < 3 ? c->name : b->name;   // class +0x149 < 3: class name (+0xb4)
            in.bribe_cost = game::bribe_cost(m, *b);
            in.gold = m.state().gold;
            in.bribe_line = in.gold < in.bribe_cost ? 5 : 3;
        }
    }
    // ---- target HUD (timeout + shown HP: hud_tick) ----
    HudModel::Target& t = hud.target;
    t.active = false;
    if (target_unit_) {
        const game::Unit* u = target_ ? w.unit(target_) : nullptr;
        if (u && !u->deleted) {
            const game::UnitClass* c = class_of(m, *u);
            t.active = true, t.unit = true;
            t.siege = c && c->special != 0 && c->special != 1;
            t.icon = c ? target_portrait(c->code) : std::string();
            t.hp_max = std::max(1, w.max_hp(*u));
            t.text.clear();
            if (!u->name.empty()) {  // +0x31d: the unit has a name billboard
                const int lead = u->leader ? u->leader : !u->members.empty() ? u->id : 0;
                const game::Unit* l = w.unit(lead);
                t.text = (l && l->rank == 4 ? u->name : c ? c->name : std::string()) + " (" + std::to_string(u->level) + ")";
            }
        }
    } else if (const game::MapItem* it = target_ ? m.objects().item(target_) : nullptr) {
        const game::ObjRes* r = m.objects().res(it->res);
        if (r && r->struction > 0) {  // ItemRes +0x1c StructionType
            static constexpr const char* kIcons[] = {"TOWER", "WALL", "GATE", "WOODGATE", "DOOR", "TRUNK"};
            t.active = true, t.unit = false, t.siege = false, t.text.clear();
            t.icon = kIcons[std::clamp(r->struction, 1, 6) - 1];
            t.hp_max = std::max(1, r->struction == 3 && it->max_hp ? it->max_hp : r->hp);   // gate: item +0xc6, else res +0x5e
        }
    }
    t.hp = t.active ? target_hp_ : 0;
}

}  // namespace joa::engine
