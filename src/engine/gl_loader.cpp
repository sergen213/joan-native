#include "engine/gl_loader.hpp"

namespace joa::engine::gl {
#define JOA_GL_DEFINE(type, name) type name = nullptr;
JOA_GL_FUNCTIONS(JOA_GL_DEFINE)
#undef JOA_GL_DEFINE

const char* load(ProcGetter get) {
#define JOA_GL_LOAD(type, name)                           \
    name = reinterpret_cast<type>(get(#name));            \
    if (!name) return #name;
    JOA_GL_FUNCTIONS(JOA_GL_LOAD)
#undef JOA_GL_LOAD
    return nullptr;
}
}  // namespace joa::engine::gl
