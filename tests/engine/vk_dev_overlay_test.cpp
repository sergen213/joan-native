// Dear ImGui dev overlay through the Vulkan backend with validation on (dev_overlay_test.hpp).
#include "dev_overlay_test.hpp"

int main() { return vktest::run_dev_overlay_test(joa::engine::RendererKind::Vulkan, "vk_dev_overlay.png"); }
