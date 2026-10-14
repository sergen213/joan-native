// End-condition / victory engine (Game::process FUN_00471620, GAMEPLAY_AI_TROOPS.md Part D §3.3).
#include <cstdio>
#include <cstdlib>

#include "game/mission_state.hpp"

using namespace joa::game;

static int fails = 0;
#define CHECK(c) \
    do { \
        if (!(c)) std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c), ++fails; \
    } while (0)

static EndCondition& add(MissionState& s, std::int8_t type, bool des = true) {
    const int n = s.add_end_condition();
    EndCondition& e = *s.end_condition(n);
    e.type = type;
    e.has_des = des;
    return e;
}

int main() {
    std::int32_t awarded_exp = 0;
    auto award = [&](std::int32_t x) { awarded_exp += x; };

    {  // 40 records max; del removes the last; enabled = 1 on add.
        MissionState s;
        for (int i = 1; i <= 40; ++i) CHECK(s.add_end_condition() == i);
        CHECK(s.add_end_condition() == 0);
        CHECK(s.end.back().enabled == 1);
        CHECK(s.del_end_condition() == 39);
        CHECK(s.end_condition(40) == nullptr && s.end_condition(0) == nullptr);
    }
    {  // Win delay: timer starts when all WIN records are finished, win after >= delay, rewards once.
        MissionState s;
        s.win_delay_ms = 15000;
        s.gold = 100;
        EndCondition& w = add(s, EndCondition::Win);
        w.gold = 500, w.exp = 1000;
        EndCondition& side = add(s, EndCondition::Side);
        side.gold = 50;
        EndCondition& imp = add(s, EndCondition::Imperative);
        std::uint8_t finish = 0;
        CHECK(!s.process_end_conditions(finish, 33, 1, 1, award) && s.win_start_ms == 0);
        w.finish = 1;
        CHECK(!s.process_end_conditions(finish, 66, 2, 1, award));
        CHECK(s.win_start_ms == 66 && s.gold == 600 && awarded_exp == 1000 && w.awarded == 1);
        CHECK(!s.process_end_conditions(finish, 66 + 14999, 3, 1, award) && s.gold == 600 && awarded_exp == 1000);
        s.cut_scene = true;  // a cut scene blocks the win
        CHECK(!s.process_end_conditions(finish, 66 + 15000, 4, 1, award) && finish == 0);
        s.cut_scene = false;
        CHECK(s.process_end_conditions(finish, 66 + 15000, 5, 1, award) && finish == 1);
        CHECK(imp.finish == 1);   // imperative records marked finished
        CHECK(side.finish == 2);  // unfinished side quest failed (scenario != 5)
        CHECK(s.events.back().kind == MissionEvent::Kind::Victory);
    }
    {  // Losing record -> finish 2 immediately; win timer reset when a WIN record reopens.
        MissionState s;
        s.win_delay_ms = 1000;
        EndCondition& w = add(s, EndCondition::Win);
        EndCondition& l = add(s, EndCondition::Lose);
        std::uint8_t finish = 0;
        w.finish = 1;
        s.process_end_conditions(finish, 33, 1, 1, award);
        CHECK(s.win_start_ms == 33);
        w.finish = 0;
        s.process_end_conditions(finish, 66, 2, 1, award);
        CHECK(s.win_start_ms == 0);
        l.finish = 2;
        s.process_end_conditions(finish, 99, 3, 1, award);
        CHECK(finish == 2 && s.events.back().kind == MissionEvent::Kind::Defeat);
    }
    {  // Disabled or description-less records are ignored; scenario 5 keeps side quests open.
        MissionState s;
        add(s, EndCondition::Win).enabled = 0;
        add(s, EndCondition::Win, false);
        EndCondition& side = add(s, EndCondition::Side);
        std::uint8_t finish = 0;
        CHECK(s.process_end_conditions(finish, 33, 1, 5, award) && finish == 1);  // delay 0
        CHECK(side.finish == 0);
    }
    {  // Save/load round trip.
        MissionState s, t;
        add(s, EndCondition::Lose).des = "x";
        s.gold = 7, s.scenario_generals = {3, 4}, s.item_hints_b = {9};
        std::vector<std::uint8_t> b;
        ByteWriter w(b);
        s.save(w);
        joa::formats::ByteReader r(b);
        CHECK(t.load(r) && r.at_end() && t.end.size() == 1 && t.end[0].des == "x" && t.gold == 7 &&
              t.scenario_generals.size() == 2 && t.item_hints_b == s.item_hints_b);
    }
    std::printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails ? 1 : 0;
}
