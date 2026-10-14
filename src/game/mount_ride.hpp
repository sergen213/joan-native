// Horses (class Special 2 HORSE, 3 ELEPHANT): mount / dismount FUN_004fc220, rider link FUN_00482930,
// rider/horse sync FUN_005174e0 (Unit_process phase 1), gear FUN_00470430 / FUN_00466d50 and rider stop
// Unit_rider_stop 0x4533e0. The player's horse controller (Ctrl_horse 0x43c5c0) is
// PlayerController::horse (mount_ride.cpp). Spec: docs/GAMEPLAY_IMPLEMENTATION.md "Horses".
#pragma once

namespace joa::game {

class World;
struct Unit;

int horse_set_rider(World& w, Unit& horse, int rider);   // FUN_00482930: 1 ok, 0 refused
int ride_horse(World& w, Unit& rider, int horse);        // FUN_004fc220: 0 refused, 1 ok, 2 bad horse; 0 = dismount
int find_mount(const World& w, const Unit& u, int special);  // FUN_0044f0b0: free mount within 70 (horse) / 150
void mount_sync(World& w, Unit& u);                      // FUN_005174e0
void rider_stop(World& w, Unit& rider);                  // Unit_rider_stop 0x4533e0
bool horse_gear(World& w, Unit& rider, int gear);        // 0 back (0x470430(0)), 1 walk (0x470430(1)), 2 run (0x466d50)

}  // namespace joa::game
