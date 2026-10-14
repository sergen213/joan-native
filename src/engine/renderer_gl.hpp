#pragma once
#include "engine/renderer.hpp"

#include <memory>

struct SDL_Window;

namespace joa::engine {
// Sets the GL 3.3 core context attributes; call before creating the SDL_WINDOW_OPENGL window.
void gl_set_window_attributes();
// Creates the context on `window` and the backend. Throws std::runtime_error on failure.
std::unique_ptr<IRenderer> create_gl_renderer(SDL_Window* window);
}  // namespace joa::engine
