// Ladder climbing (LADDER map items, ItemClass 11). Player only: the controller's interact key starts a
// climb (0x42d300 -> find 0x5183d0 -> 0x4fab10 / 0x4ac910) and, while climbing, forward / backward step
// one rung (0x42d270 -> 0x518890 up / 0x445000 down). The C_* climb states move the unit along the
// ladder and chain into each other (Unit_updateAction 0x7c7ab0 cases 0x5b..0x66).
// Spec: docs/GAMEPLAY_IMPLEMENTATION.md (ladders).
#pragma once

namespace joa::game {

class Mission;
struct Unit;
struct InputFrame;

// Installed as World::ladder_input. True when the frame was consumed (climbing, or a climb started).
bool ladder_input(Mission& m, Unit& u, const InputFrame& in);
// Ladder within 100 units of the unit's feet: bottom -> mode 1, top -> mode 0 (0x5183d0). 0 = none.
int ladder_find(Mission& m, const Unit& u, int& mode);
// 0x4fab10(ladder, 1, mode): facing test, then mount at the bottom (mode 1) or top (mode 0).
bool ladder_start(Mission& m, Unit& u, int ladder, int mode);
// Per tick: climbing states of every climbing unit.
void ladder_tick(Mission& m);

}  // namespace joa::game
