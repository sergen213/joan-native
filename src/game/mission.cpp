#include "game/mission.hpp"

#include "formats/pfd_mesh.hpp"
#include "formats/world_text.hpp"
#include "game/multimap_nav.hpp"
#include "game/orders_queue.hpp"
#include "game/script_host.hpp"

namespace joa::game {

namespace {
constexpr int kTickMs = static_cast<int>(Vm::kTickMs);

std::optional<formats::BlockText> read_blocks(const formats::Vfs& vfs, const std::string& path) {
    auto b = vfs.read(path);
    if (!b) return std::nullopt;
    return formats::parse_block_text(std::string_view(reinterpret_cast<const char*>(b->data()), b->size()));
}

std::uint64_t fnv(std::uint64_t h, std::span<const std::uint8_t> d) {
    for (std::uint8_t c : d) h = (h ^ c) * 1099511628211ull;
    return h;
}
}  // namespace

Mission::Mission(GameData& gd, const formats::Vfs& vfs, CampaignProfile& profile) : gd_(gd), vfs_(vfs), profile_(profile) {}
Mission::~Mission() = default;

std::string Mission::map_dir() const {
    const std::string& w = info_.step.world_file;  // "scenario1\S1L1w"
    const auto p = w.find_last_of("\\/");
    return "MAP\\" + (p == std::string::npos ? std::string() : w.substr(0, p));
}

// Node origin = min corner of the rotated node box, X/Y/WIDTH/LENGTH in 32-unit steps
// (FUN_0047ad40). A missing .wor means one node named after the stem at the origin.
std::vector<WorldNode> world_nodes(const formats::Vfs& vfs, const std::string& map_dir, const formats::ScenarioStep& step) {
    std::vector<WorldNode> nodes;
    const std::string& w = step.world_file;
    const auto p = w.find_last_of("\\/");
    if (auto wor = read_blocks(vfs, map_dir + "\\" + (p == std::string::npos ? w : w.substr(p + 1)) + ".wor"))
        for (const auto& b : wor->blocks) {
            const std::string* n = b.get("NodeName");
            if (b.type != "CreateNode" || !n || n->empty()) continue;
            const double hd = b.number("HD") * 0.017453292, cx = b.number("X") * 32.0, cy = b.number("Y") * 32.0;
            const double hw = b.number("WIDTH") * 16.0, hl = b.number("LENGTH") * 16.0;
            const double c = std::fabs(std::cos(hd)), s = std::fabs(std::sin(hd));
            const double ex = c * hw + s * hl, ey = s * hw + c * hl;
            nodes.push_back({*n, float(int(float(cx - ex))), float(int(float(cy - ey))), float(cx + ex), float(cy + ey)});
        }
    if (nodes.empty()) nodes.push_back({step.script_file});
    return nodes;
}

// Terrain of a multi-node world: each node's .gri at its origin.
class NodeTerrain final : public ITerrainQuery {
public:
    struct Part {
        std::unique_ptr<GriTerrain> g;
        float ox, oy;
    };
    std::vector<Part> parts;
    float height(float x, float y) const override {
        const Part* p = at(x, y);
        return p ? p->g->height(x - p->ox, y - p->oy) : 0.0f;
    }
    Vec3 normal(float x, float y) const override {
        const Part* p = at(x, y);
        return p ? p->g->normal(x - p->ox, y - p->oy) : Vec3{0, 0, 1};
    }
    bool walkable(float x, float y) const override {
        const Part* p = at(x, y);
        return p && p->g->walkable(x - p->ox, y - p->oy);
    }

private:
    const Part* at(float x, float y) const {
        for (const Part& p : parts) {
            const auto& g = p.g->grid();
            const float w = g.cells_x * formats::TerrainGrid::kCellSize, h = g.cells_y * formats::TerrainGrid::kCellSize;
            if (x >= p.ox && y >= p.oy && x <= p.ox + w && y <= p.oy + h) return &p;
        }
        return parts.empty() ? nullptr : &parts.front();
    }
};

// Uni_load_create_unit 0x558a60 for every CreateUnit block, then FUN_00558890's team pass.
// Positions are node-local: the node origin is added (as for the .stg objects).
static void load_units(Mission& m, const formats::BlockText& uni, const WorldNode& node, std::vector<int>& loaded) {
    World& w = m.world();
    GameData& gd = m.data();
    for (const auto& b : uni.blocks) {
        if (b.type != "CreateUnit") continue;
        const std::string* file = b.get("UnitFile");
        if (!file) continue;
        const Vec3 pos{float(b.number("X")) + node.ox, float(b.number("Y")) + node.oy, float(b.number("Z"))};
        const std::string* kingdom = b.get("Kingdom");
        const int nation = kingdom ? gd.nation_id(*kingdom) : 0;
        const int id = m.add_unit(upper(*file));  // UnitArray_add: a CONTINUE-profile hero is restored
        Unit* u = w.unit(id);
        if (!u) continue;
        u->rank = gd.units[u->cls].ranking;  // Unit_set_rank(class +0x149)
        u->nation = nation;
        u->pos = {pos.x, pos.y, w.terrain() ? w.terrain()->height(pos.x, pos.y) : pos.z};
        w.set_yaw_now(*u, float(b.number("HD")));
        u->ai_controlled = true;  // +0x102 = 1
        if (const std::string* n = b.get("UnitName")) u->name = *n;
        if (const std::string* n = b.get("UnitShortName"); n && !n->empty()) u->short_name = *n;
        if (const std::string* g = b.get("Group")) u->team = std::atoi(g->c_str());
        // Task / Range: TOWER_ATTACK -> (8, 1, Range); other -> (task, 1, ftol(X), ftol(Y), Range).
        const std::string* task = b.get("Task");
        const int range = int(b.number("Range"));
        if (const int tid = task ? task_id_from_name(*task) : 0) {
            if (tid == 8) m.ai().give_task(*u, 8, 1, range, 0, 0, 0);
            else m.ai().give_task(*u, tid, 1, int(pos.x), int(pos.y), range, 0);
        }
        // Treasure: drop list by name (DropList_roll) else item code; TreasureLevel -1 = item default.
        if (const std::string* t = b.get("Treasure"); t && !t->empty()) {
            const int level = int(b.number("TreasureLevel", -1));
            if (auto dl = gd.drop_lists.find(upper(*t)); dl != gd.drop_lists.end()) {
                int total = 0;
                for (auto& e : dl->second) total += e.second;
                u->drop_item = 0;
                if (total > 0) {
                    int r = w.rng().next(total) + 1;
                    for (auto it = dl->second.rbegin(); it != dl->second.rend(); ++it)
                        if ((r -= it->second) < 1) {
                            u->drop_item = it->first;
                            u->drop_level = gd.items[it->first].min_level;
                            break;
                        }
                }
            } else if (const int item = gd.item_index(*t)) {
                u->drop_item = item;
                u->drop_level = level == -1 ? gd.items[item].min_level : level;
            }
        }
        w.set_level(*u, int(b.number("UnitLevel", 1)));  // includes AI auto-upgrade when rank < 4
        w.set_hp(*u, w.max_hp(*u));
        loaded.push_back(id);
    }
}

bool Mission::start(const CampaignMission& cm, std::uint32_t seed, std::string* err) {
    info_ = cm;
    seed_ = seed;
    const std::string dir = map_dir();
    const auto nodes = world_nodes(vfs_, dir, cm.step);

    // Terrain: every world node's .gri at its origin. Path meshes: every node's .pfd at its origin
    // plus the world-level <world>.pfd for cross-node routes (NavWorld).
    if (nodes.size() == 1) {
        if (auto t = GriTerrain::load_scenario(vfs_.root().string(), nodes.front().name)) terrain_ = std::move(t);
    } else {
        auto nt = std::make_unique<NodeTerrain>();
        for (const auto& n : nodes)
            if (auto g = GriTerrain::load_scenario(vfs_.root().string(), n.name)) nt->parts.push_back({std::move(g), n.ox, n.oy});
        if (!nt->parts.empty()) terrain_ = std::move(nt);
    }
    if (!terrain_) terrain_ = std::make_unique<FlatTerrain>();
    world_ = std::make_unique<World>(gd_, terrain_.get(), seed);
    world_->difficulty = profile_.difficulty;
    auto read_pfd = [&](const std::string& stem) -> std::optional<NavMesh> {
        if (auto b = vfs_.read(dir + "\\" + stem + ".pfd"))
            if (auto mesh = formats::parse_pfd(*b)) return NavMesh(*mesh);
        return std::nullopt;
    };
    if (nodes.size() == 1) {
        if (auto mesh = read_pfd(nodes.front().name)) nav_ = std::make_unique<NavWorld>(std::move(*mesh));
    } else {
        std::vector<NavLevel> levels;
        for (const auto& n : nodes)
            if (auto mesh = read_pfd(n.name)) levels.push_back({std::move(*mesh), n.ox, n.oy, n.x1, n.y1});
        const std::string& wf = cm.step.world_file;  // "scenario5\S5L1w" (S9L1 reuses scenario 5's world)
        if (!levels.empty()) nav_ = std::make_unique<NavWorld>(std::move(levels), read_pfd(wf.substr(wf.find_last_of("\\/") + 1)));
    }
    ai_ = std::make_unique<AiSystem>(*world_, nav_.get());
    phys_ = std::make_unique<CharPhysics>(nav_.get());  // navmesh walls, step-up, unit pushes, gravity
    world_->set_physics(phys_.get());
    ai_->chapter = cm.no;

    host_ = std::make_unique<MissionHost>(*this);
    vm_ = std::make_unique<Vm>(vfs_source_loader(vfs_), *host_, seed);
    world_->use_rng(&vm_->rng());

    // Battle::start_scenario: compile the step script, run _des once (end conditions).
    if (!vm_->load_scenario(cm.step)) {
        if (err) *err = "script load failed: " + (vm_->errors().empty() ? std::string() : vm_->errors().front());
        return false;
    }
    // Battle::load_world: map objects, places, units of every world node.
    std::string e;
    if (!objects_.load(*this, vfs_, dir, cm.stem(), &e) || !places_.load(*this, vfs_, dir, cm.stem(), &e)) {
        if (err) *err = e;
        return false;
    }
    for (const auto& n : nodes) {
        auto uni = read_blocks(vfs_, dir + "\\" + n.name + ".uni");
        if (!uni) continue;
        std::vector<int> loaded;
        load_units(*this, *uni, n, loaded);
        // FUN_00558890 team pass per file: rank > 2 with a team id lead; rank < 3 join the
        // leader(s) with their id.
        for (int id : loaded)
            if (Unit* u = world_->unit(id); u && u->rank > 2 && u->team != 0) ai_->create_team(*u);
        for (int id : loaded) {
            Unit* u = world_->unit(id);
            if (!u || u->rank >= 3 || u->team == 0) continue;
            for (int lid : loaded)
                if (const Unit* l = world_->unit(lid); l && ai_->is_leader(*l) && l->team == u->team) ai_->join_team(*u, lid);
        }
    }
    restore_profile();
    last_finish_ = 0;
    // Script init pass of Battle::load_world (vtbl[0xc] exec of the root before tick 1).
    vm_->load_pass();
    return true;
}

// Battle_load_world 0x41c810 profile restore (FUN_004728f0 / FUN_00472c40, gold, play time,
// command points).
void Mission::restore_profile() {
    state_.item_hints_a = profile_.item_hints_a;
    state_.item_hints_b = profile_.item_hints_b;
    state_.gold = profile_.gold;
    state_.play_ms = profile_.play_ms;
    world_->command_points = profile_.command_points;
}

bool Mission::continue_in_world(const CampaignMission& cm, std::string* err) {
    info_ = cm;
    ai_->chapter = cm.no;
    // Game_deinit keep path: enabled, finished, non-imperative objectives are dropped
    // (Game_drop_end_condition 0x471f90 frees the texts; the record stays and is ignored) and the
    // per-unit scenario counters (+0x1b0 +0x1b2 +0x1b5 +0x1b7 +0x1b9 +0x1bb) are cleared.
    for (auto& r : state_.end)
        if (r.enabled && r.finish && r.type != EndCondition::Imperative) r.has_des = false, r.des.clear(), r.territory_link.clear();
    for (Unit& u : world_->units()) u.kills = u.officer_kills = u.crit_count = u.crit_kills = u.troop_lost = u.troop_kills = 0;
    // Game_deinit closes every dialog / message / interface / cut scene (unconditional part);
    // Game_init_arrays clears the win delay and the cut-scene flag; Battle_start_scenario clears
    // DAT_00a5ba64. The engine-side UI settings stay.
    MissionUi ui;
    ui.auto_advance = ui_.auto_advance, ui.auto_ms = ui_.auto_ms, ui.auto_choice = ui_.auto_choice;
    ui.key_name = std::move(ui_.key_name);
    ui_ = std::move(ui);
    state_.win_delay_ms = 0, state_.win_start_ms = 0, state_.cut_scene = false, world_->cut_scene = false;
    state_.charmove_detect_block = 0;
    // Scenario_deinit + script compile: a fresh script tree; game clock and engine RNG continue.
    auto host = std::make_unique<MissionHost>(*this);
    auto vm = std::make_unique<Vm>(vfs_source_loader(vfs_), *host, seed_);
    vm->continue_from(*vm_);
    vm_ = std::move(vm);
    host_ = std::move(host);
    world_->use_rng(&vm_->rng());
    if (!vm_->load_scenario(cm.step)) {
        if (err) *err = "script load failed: " + (vm_->errors().empty() ? std::string() : vm_->errors().front());
        return false;
    }
    restore_profile();
    last_finish_ = 0;
    force_objectives_ = false;
    vm_->load_pass();
    return true;
}

void Mission::tick(const InputFrame& in) {
    ScenarioState& ss = vm_->state();
    if (ss.game_finish == 0 && state_.clock_running) {  // 0x4e5fcc (game speed != 0)
        state_.play_ms += kTickMs;
        state_.passed_ms += kTickMs;
    }
    advance_clock(state_, kTickMs);  // GameClock 0x476bb0
    if (nav_) nav_->stream(*world_);  // level streaming + unit (de)activation (World_draw 0x44c800)
    objects_.tick(*this);
    // World events feed the engine-side objective counters, in the phase that produced them.
    std::size_t seen = world_->events().size();
    auto feed = [&] {
        const auto& ev = world_->events();
        if (ev.size() > seen) process_world_events(*this, std::vector<GameEvent>(ev.begin() + std::ptrdiff_t(seen), ev.end()));
        seen = ev.size();
    };
    world_->tick_units(in);
    feed();
    places_.tick(*this);
    ss.counting_paused = state_.cut_scene || ui_.clock_stopped() || !state_.clock_running;
    vm_->tick();
    feed();
    if (force_objectives_) finish_objectives();
    state_.process_end_conditions(ss.game_finish, now(), ticks(), info_.no, [this](std::int32_t exp) {
        for (Unit& u : world_->units())  // heroes (FUN_0042ff90) with rank > 3: FUN_005229a0(exp, 1, 0)
            if (!u.deleted && u.rank > 3 && u.cls >= 0 && gd_.units[u.cls].hero) world_->add_xp(u, exp, true, false);
    });
    ui_.tick(*this);
    world_->tick_physics();
    feed();

    if (ss.game_finish != last_finish_) {
        if (ss.game_finish == 1 || ss.game_finish == 2)  // WinMsg::show / GameOver_lose_freeze_units
            for (Unit& u : world_->units())
                if (!u.deleted) ai_->set_idle(u);
        if (ss.game_finish == 2) {  // GameOver_lose_freeze_units
            state_.game_over = 1;
            if (state_.events.empty() || state_.events.back().kind != MissionEvent::Kind::Defeat) emit(MissionEvent::Kind::Defeat);
        }
        last_finish_ = ss.game_finish;
    }
    if (!state_.save_request_name.empty()) {  // save_game (FUNC 315), after the tick
        const std::string name = std::exchange(state_.save_request_name, {});
        const std::string desc = std::exchange(state_.save_request_desc, {});
        std::string e;
        const bool ok = !autosave || write_save_atomic(native_save_dir() / (name + ".jsav"), encode_save(save()), 3, &e);
        emit(MissionEvent::Kind::SaveGame, ok, 0, name + ": " + desc + (ok ? "" : " (" + e + ")"));
    }
}

int Mission::load_hero(std::string_view code, std::string_view nation) {
    if (!profile_.continued) return 0;  // DAT_00a58f95
    const HeroRecord* rec = profile_.find(code);
    if (!rec || rec->unit.cls < 0) return 0;
    const int id = world_->spawn_unit(gd_.units[rec->unit.cls].code, 0, {}, 0);
    Unit* u = world_->unit(id);
    if (!u) return 0;
    copy_persistent_unit(rec->unit, *u);  // FUN_004c96f0
    const int n = gd_.nation_id(nation);
    if (n == 0) return id;
    u->nation = n;
    u->ai_controlled = true;
    world_->set_hp(*u, world_->max_hp(*u));
    world_->set_ep(*u, world_->max_ep(*u));
    for (int mid : u->members)
        if (Unit* mu = world_->unit(mid)) mu->nation = n;
    return id;
}

int Mission::add_unit(std::string_view code) {
    if (const int id = load_hero(code, {})) {
        Unit& h = *world_->unit(id);
        world_->set_hp(h, world_->max_hp(h));
        world_->set_ep(h, world_->max_ep(h));
        h.troop_kills = h.troop_lost = 0;  // +0x1bb / +0x1b9
        return id;
    }
    return world_->spawn_unit(code, 0, {}, 0);
}

void Mission::store_profile() {
    for (int i = int(world_->units().size()); i >= 1; --i) {  // FUN_0051ff00
        Unit* u = world_->unit(i);
        if (!u || u->deleted || u->rank <= 3 || u->cls < 0) continue;
        if (info_.no != 5)  // quest items (item type 0xf) are not carried, except out of scenario 5
            std::erase_if(u->inv.bag, [&](const ItemInstance& it) {
                return it.item > 0 && it.item < int(gd_.items.size()) && gd_.items[it.item].type == 0xf;
            });
        const std::string& code = gd_.units[u->cls].code;
        HeroRecord* rec = profile_.find(code);
        if (!rec) rec = &profile_.heroes.emplace_back(HeroRecord{code, {}});
        rec->unit = *u;  // only the copy_persistent_unit subset is read back
    }
    profile_.item_hints_a = state_.item_hints_a;
    profile_.item_hints_b = state_.item_hints_b;
    profile_.gold = state_.gold;
    profile_.play_ms = state_.play_ms;
    profile_.command_points = world_->command_points;
    profile_.continued = true;
}

void Mission::finish_objectives() {
    force_objectives_ = true;  // re-applied after every script pass (scripts reset finish states)
    for (auto& r : state_.end)
        if (r.enabled && r.type == EndCondition::Win) r.finish = 1;
}

SaveFile Mission::save() const {
    SaveFile f;
    std::vector<std::uint8_t> b;
    {
        ByteWriter w(b);
        w.i32(info_.no), w.u32(seed_), w.u8(last_finish_), w.u8(force_objectives_), w.i32(world_->command_points);
        profile_.save(w);
    }
    f.add("MISN", std::move(b));
    b = {};
    world_->serialize(b);
    f.add("WRLD", std::move(b));
    b = {};
    ai_->serialize(b);
    f.add("AISY", std::move(b));
    b = {};
    vm_->save(b);
    f.add("VMST", std::move(b));
    auto section = [&](const char* tag, auto&& fn) {
        std::vector<std::uint8_t> d;
        ByteWriter w(d);
        fn(w);
        f.add(tag, std::move(d));
    };
    section("MSTA", [&](ByteWriter& w) { state_.save(w); });
    section("OBJS", [&](ByteWriter& w) { objects_.save(w); });
    section("PLCS", [&](ByteWriter& w) { places_.save(w); });
    section("UIST", [&](ByteWriter& w) { ui_.save(w); });
    if (nav_ && nav_->levels().size() > 1) f.add("NAVL", nav_->save_levels());  // level load states
    return f;
}

bool Mission::load(const SaveFile& f, std::string* err) {
    auto fail = [&](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };
    const char* tags[] = {"MISN", "WRLD", "AISY", "VMST", "MSTA", "OBJS", "PLCS", "UIST"};
    for (const char* t : tags)
        if (!f.find(t)) return fail(std::string("save lacks section ") + t);
    if (!world_->deserialize(f.find("WRLD")->data)) return fail("bad WRLD section");
    {
        formats::ByteReader r(f.find("MISN")->data);
        const int no = r.i32();
        if (no != info_.no) return fail("save is for scenario " + std::to_string(no));
        seed_ = r.u32();
        last_finish_ = r.u8();
        force_objectives_ = r.u8() != 0;
        world_->command_points = r.i32();
        if (!profile_.load(r) || !r.at_end()) return fail("bad MISN section");
    }
    if (!ai_->deserialize(f.find("AISY")->data)) return fail("bad AISY section");
    if (!vm_->load(f.find("VMST")->data)) return fail("bad VMST section (different scripts?)");
    world_->use_rng(&vm_->rng());
    auto section = [&](const char* tag, auto&& fn) {
        formats::ByteReader r(f.find(tag)->data);
        return fn(r) && r.at_end();
    };
    if (!section("MSTA", [&](formats::ByteReader& r) { return state_.load(r); })) return fail("bad MSTA section");
    if (!section("OBJS", [&](formats::ByteReader& r) { return objects_.load_state(r); })) return fail("bad OBJS section");
    if (!section("PLCS", [&](formats::ByteReader& r) { return places_.load_state(r); })) return fail("bad PLCS section");
    if (!section("UIST", [&](formats::ByteReader& r) { return ui_.load_state(r); })) return fail("bad UIST section");
    if (nav_ && nav_->levels().size() > 1) {
        const auto* s = f.find("NAVL");
        if (!s || !nav_->load_levels(s->data)) return fail("bad NAVL section");
        nav_->resync(*world_);
    }
    return true;
}

std::uint64_t Mission::hash() const {
    std::uint64_t h = 1469598103934665603ull;
    for (const auto& s : save().sections) h = fnv(fnv(h, std::span(reinterpret_cast<const std::uint8_t*>(s.tag.data()), 4)), s.data);
    return h;
}

}  // namespace joa::game
