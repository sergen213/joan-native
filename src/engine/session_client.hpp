#pragma once
// The game client: movies -> menus -> missions (GameFlow) with the world view, HUD, camera, input and audio
// of a Session. Windowed (SDL + GL/Vulkan) or headless (NullRenderer, no devices). docs/PLAYING.md.
#include "engine/cli.hpp"

namespace joa::engine {

// Exit code. Needs the original game data (scenario/scenario_01.scr).
int run_game(const Options& o);

}  // namespace joa::engine
