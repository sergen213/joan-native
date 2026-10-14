#pragma once
#include "engine/cli.hpp"

namespace joa::engine {
// Runs the client (or the headless simulation) and returns the process exit code.
int run(const Options& options);
}
