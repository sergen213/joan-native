#include "engine/renderdoc.hpp"

#include "renderdoc/renderdoc_app.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace joa::engine {
namespace {

RENDERDOC_API_1_6_0* g_api = nullptr;
int g_capture_frame = -1;
bool g_capturing = false;
std::uint32_t g_reported = 0;

void prepend_env(const char* name, const std::string& value) {
    const char* old = std::getenv(name);
    setenv(name, old && *old ? (value + ":" + old).c_str() : value.c_str(), 1);
}

// OpenGL is only hooked when librenderdoc.so is in the process before SDL dlopens libGL/libEGL (RenderDoc interposes
// dlopen/glX/egl symbols); a late dlopen cannot. Restart once with it preloaded: LD_PRELOAD gets the bare file name
// and LD_LIBRARY_PATH its directory, because LD_PRELOAD splits paths at spaces.
void restart_preloaded(const std::string& lib) {
    if (std::getenv("JOA_RENDERDOC_PRELOADED")) return;  // this already is the restart: the preload did not take
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::string cmdline((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<char*> argv;
    for (std::size_t p = 0; p < cmdline.size(); p += std::strlen(cmdline.data() + p) + 1) argv.push_back(cmdline.data() + p);
    if (argv.empty()) return;
    argv.push_back(nullptr);
    const std::filesystem::path path(lib);
    prepend_env("LD_PRELOAD", path.filename().string());
    if (path.has_parent_path()) prepend_env("LD_LIBRARY_PATH", path.parent_path().string());
    setenv("JOA_RENDERDOC_PRELOADED", "1", 1);
    std::fprintf(stderr, "joan-native: restarting with %s preloaded so RenderDoc hooks OpenGL\n", path.filename().c_str());
    execv("/proc/self/exe", argv.data());
    std::fprintf(stderr, "joan-native: RenderDoc restart failed: %s\n", std::strerror(errno));
}

}  // namespace

void renderdoc_init(const Options& o) {
    if (!o.renderdoc || g_api) return;
    const char* env = std::getenv("RENDERDOC_LIB");
    const std::string lib = env && *env ? env : "librenderdoc.so";
    void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_NOLOAD);  // already injected (preload, renderdoccmd, qrenderdoc)
    if (!h) {
        h = dlopen(lib.c_str(), RTLD_NOW);
        if (!h) {
            std::fprintf(stderr, "joan-native: RenderDoc unavailable: %s (install RenderDoc or set RENDERDOC_LIB=/path/to/librenderdoc.so)\n",
                         dlerror());
            return;
        }
        if (o.renderer == RendererKind::OpenGL) restart_preloaded(lib);  // returns only if the restart is not possible
    }
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(h, "RENDERDOC_GetAPI"));
    void* api = nullptr;
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_6_0, &api) || !api) {
        std::fprintf(stderr, "joan-native: RenderDoc unavailable: %s has no RenderDoc 1.6 API\n", lib.c_str());
        return;
    }
    g_api = static_cast<RENDERDOC_API_1_6_0*>(api);
    // RenderDoc's implicit Vulkan layer only loads with this set (its manifest's enable_environment); the manifest
    // itself must be registered (system install, or VK_ADD_IMPLICIT_LAYER_PATH for an extracted tarball).
    setenv("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1", 0);
    g_capture_frame = o.renderdoc_capture;
    int major = 0, minor = 0, patch = 0;
    g_api->GetAPIVersion(&major, &minor, &patch);
    std::fprintf(stderr, "joan-native: RenderDoc API %d.%d.%d loaded (%s); captures go to %s_*.rdc\n", major, minor, patch, lib.c_str(),
                 g_api->GetCaptureFilePathTemplate());
}

void renderdoc_frame(int frame) {
    if (!g_api) return;
    // Explicit start/end around one loop iteration (NULL device/window = the one active API): exact frame, and it
    // works without presentation (hidden --screenshot windows, Vulkan's offscreen fallback), unlike TriggerCapture.
    renderdoc_report();
    if (frame == g_capture_frame) g_api->StartFrameCapture(nullptr, nullptr), g_capturing = true;
}

void renderdoc_report() {
    if (!g_api) return;
    if (g_capturing && !g_api->EndFrameCapture(nullptr, nullptr))
        std::fprintf(stderr, "joan-native: RenderDoc capture of frame %d failed (Vulkan: is RenderDoc's layer registered? see "
                             "docs/GRAPHICS_ROADMAP.md)\n", g_capture_frame);
    g_capturing = false;
    for (; g_reported < g_api->GetNumCaptures(); ++g_reported) {
        std::uint32_t len = 0;
        std::uint64_t time = 0;
        g_api->GetCapture(g_reported, nullptr, &len, &time);
        std::string path(len, '\0');
        g_api->GetCapture(g_reported, path.data(), &len, &time);
        if (!path.empty() && path.back() == '\0') path.pop_back();
        std::fprintf(stderr, "joan-native: RenderDoc capture written: %s\n", path.c_str());
    }
}

}  // namespace joa::engine
