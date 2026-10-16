// Dear ImGui dev overlay through the OpenGL backend (dev_overlay_test.hpp).
#include "dev_overlay_test.hpp"

int main() { return vktest::run_dev_overlay_test(joa::engine::RendererKind::OpenGL, "gl_dev_overlay.png"); }
