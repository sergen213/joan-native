#pragma once
// RenderDoc in-application API (third_party/renderdoc/renderdoc_app.h, docs/GRAPHICS_ROADMAP.md "Developer tools").
// --renderdoc loads librenderdoc.so ($RENDERDOC_LIB, else the system library; reused when RenderDoc already injected
// it) before any window or GPU object exists. OpenGL restarts the process once with it preloaded; Vulkan uses
// RenderDoc's implicit layer. --renderdoc-capture N captures frame N of the frame loop.
#include "engine/cli.hpp"

namespace joa::engine {

void renderdoc_init(const Options& o);  // no-op unless o.renderdoc; logs why RenderDoc is unavailable
void renderdoc_frame(int frame);        // top of every frame-loop iteration: ends/starts the capture, logs new files
void renderdoc_report();                // after the frame loop, while the renderer still lives: same, no new capture

}  // namespace joa::engine
