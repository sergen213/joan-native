#pragma once
#include "engine/cli.hpp"
#include "engine/renderer.hpp"

#include <cstdint>
#include <memory>

struct SDL_Window;

namespace joa::engine {
// Vulkan 1.3 backend (dynamic rendering, synchronization2, bindless textures). Presents through a
// swapchain when `window` was created with SDL_WINDOW_VULKAN on a real video driver; under SDL's
// offscreen/dummy drivers it renders surfaceless (read_back still works). Uses options.gpu,
// vk_validation, gpu_timing and present_mode. Throws std::runtime_error on failure.
std::unique_ptr<IRenderer> create_vk_renderer(SDL_Window* window, const Options& options);

// Process-wide counters from the validation debug messenger (tests assert errors == 0).
std::uint32_t vk_validation_errors();
bool vk_validation_active();  // the Khronos layer was loaded by the last Vulkan renderer
bool vk_ray_tracing_available();  // the last Vulkan renderer's device can run the ray-traced effects (docs/RAYTRACING.md)
}  // namespace joa::engine
