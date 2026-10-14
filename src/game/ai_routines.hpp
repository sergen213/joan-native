// Per-class native AI routines (UnitAI_dispatch 0x5467b0 cases 1..15). Internal to joa_game.
// Spec: docs/reverse-engineering/GAMEPLAY_AI_TROOPS.md §A5, Part B §3A (classes 1-3), §3B (4-7), §3C (8-15).
// Each entry is the class main routine (dispatcher address in the comment): it reads the current task
// (AiSystem::curr_task, inheriting the leader's) and routes to the task routine. true = handled.
#pragma once

#include "game/ai_system.hpp"

namespace joa::game::ai {

enum Behaviour : int { NEAREST = 1, WEAKEST, IMPORTANT, NEAREST_ARCHER, NEAREST_HIGH_PRIORITY };
enum Mode : int { ATTACK = 1, ACTIVE = 2, IDLE_MODE = 3 };

bool light_infantry(AiSystem&, Unit&);    // 1  0x531f90  ai_routines_light.cpp
bool light_general(AiSystem&, Unit&);     // 2  0x5322c0
bool special_general(AiSystem&, Unit&);   // 3  0x532520
bool heavy_infantry(AiSystem&, Unit&);    // 4  0x532780  ai_routines_heavy.cpp
bool heavy_general(AiSystem&, Unit&);     // 5  0x532a00
bool archer(AiSystem&, Unit&);            // 6  0x532c60
bool archer_general(AiSystem&, Unit&);    // 7  0x532ef0
bool cannon(AiSystem&, Unit&);            // 8  0x533170  ai_routines_siege.cpp
bool autobow(AiSystem&, Unit&);           // 9  0x533370
bool batterram(AiSystem&, Unit&);         // 10 0x533440
bool camp_horse(AiSystem&, Unit&);        // 11 0x5335e0
bool cart(AiSystem&, Unit&);              // 12 0x5336e0
bool citizen(AiSystem&, Unit&);           // 13 0x533810
bool trebuchet(AiSystem&, Unit&);         // 14 0x533920
bool pig(AiSystem&, Unit&);               // 15 0x533b10

// Shared task routines used by every class (ai_routines_siege.cpp).
bool task_retreat(AiSystem&, Unit&);         // 0x545d50 RETREAT (11)
bool task_rts_move(AiSystem&, Unit&);        // 0x545ea0 RTS_MOVE_TO (12)
bool task_approach_place(AiSystem&, Unit&);  // 0x546050 APPROACH_PLACE (14)

// AI_after_block 0x546970: counter-attack roll after a BLOCK order ends, by ai_class.
// Each file handles its own classes and returns false for others.
bool after_block_light(AiSystem&, Unit&);    // classes 1-3: 0x535c20 / 0x538340 / 0x53abd0
bool after_block_heavy(AiSystem&, Unit&);    // classes 4-7: 0x53cb10 / 0x53f670 / 0x541270(u,1,0) / 0x5432a0
bool after_block_siege(AiSystem&, Unit&);    // classes 8,9,10,14: 0x544c40 / 0x544e00 / 0x545380 / 0x545d00

}  // namespace joa::game::ai
