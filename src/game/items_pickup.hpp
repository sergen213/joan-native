// Floor-item pickup Unit_auto_pickup 0x49da30 (unit phase 0): generals (rank > 3) walking over a
// Pickable World item take it. Spec: docs/reverse-engineering/GAMEPLAY_CHARACTER_COMBAT.md "Floor items".
#pragma once

namespace joa::game {

class Mission;

// Installs World::auto_pickup (gold goes to MissionState::gold, the rest through World::give_item).
void install_auto_pickup(Mission& m);

}  // namespace joa::game
