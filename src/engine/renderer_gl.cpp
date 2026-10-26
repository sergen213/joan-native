#include "engine/renderer_gl.hpp"

#include "engine/gl_loader.hpp"
#include "engine/gl_shaders.hpp"
#include "engine/pbr_env.hpp"
#include "engine/post_frame.hpp"
#include "engine/shadow_volume.hpp"

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace joa::engine {
using namespace gl;

void gl_set_window_attributes() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // Scene renders into an offscreen sRGB target; the window surface only receives a blit.
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
}

namespace {

GLuint compile(GLenum type, const char* src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        glDeleteShader(s);
        throw std::runtime_error(std::string("GL shader compile failed: ") + log);
    }
    return s;
}

GLuint link(const char* vs_src, const char* fs_src) {
    const GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    const GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        glDeleteProgram(p);
        throw std::runtime_error(std::string("GL program link failed: ") + log);
    }
    return p;
}

class GlRenderer final : public IRenderer {
public:
    explicit GlRenderer(SDL_Window* window) : window_(window) {
        context_ = SDL_GL_CreateContext(window);
        if (!context_) throw std::runtime_error(std::string("SDL_GL_CreateContext: ") + SDL_GetError());
        SDL_GL_MakeCurrent(window, context_);
        if (const char* missing = gl::load(SDL_GL_GetProcAddress)) {
            SDL_GL_DestroyContext(context_);
            throw std::runtime_error(std::string("missing GL entry point ") + missing);
        }
        // Wayland never tears, and SDL's emulated vsync there (wait for the frame callback, then swap) stalls against
        // mutter's VRR frame clock: one frame in ~8 waited the panel's minimum refresh (20.9 ms on a 48-240 Hz monitor).
        // There: no swap interval, end_frame paces to the display refresh itself.
        if (const char* d = SDL_GetCurrentVideoDriver(); d && std::strcmp(d, "wayland") == 0) {
            SDL_GL_SetSwapInterval(0);
            // ponytail: the refresh of the display at startup; re-read it if windows moving between monitors matters.
            if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)); m && m->refresh_rate > 0.0f)
                frame_ns_ = Uint64(1e9 / double(m->refresh_rate));
        } else if (!SDL_GL_SetSwapInterval(-1)) {
            SDL_GL_SetSwapInterval(1);  // adaptive vsync (VRR friendly)
        }
        std::fprintf(stderr, "joan-native: OpenGL %s / %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                     reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

        mesh_.program = link(gl_shaders::mesh_vert, gl_shaders::mesh_frag);
        auto u = [&](const char* n) { return glGetUniformLocation(mesh_.program, n); };
        mesh_.model = u("u_model");
        mesh_.view_proj = u("u_view_proj");
        mesh_.normal_matrix = u("u_normal_matrix");
        mesh_.tint = u("u_tint");
        mesh_.alpha_cutoff = u("u_alpha_cutoff");
        mesh_.unlit = u("u_unlit");
        mesh_.no_fog = u("u_no_fog");
        mesh_.sun_dir = u("u_sun_dir");
        mesh_.sun_color = u("u_sun_color");
        mesh_.ambient = u("u_ambient");
        mesh_.fog_color = u("u_fog_color");
        mesh_.fog_range = u("u_fog_range");
        mesh_.camera_pos = u("u_camera_pos");
        mesh_.has_layer2 = u("u_has_layer2");
        mesh_.has_lightmap = u("u_has_lightmap");
        mesh_.bump_scale = u("u_bump_scale");
        mesh_.diffuse = u("u_diffuse");
        mesh_.vertex_diffuse = u("u_vertex_diffuse");
        mesh_.light_count = u("u_light_count");
        mesh_.light_pos_range = u("u_light_pos_range");
        mesh_.light_color = u("u_light_color");
        mesh_.light_att = u("u_light_att");
        mesh_.a2c = u("u_a2c");
        glUseProgram(mesh_.program);
        glUniform1i(u("u_albedo"), 0);
        glUniform1i(u("u_layer2"), 1);
        glUniform1i(u("u_lightmap"), 2);
        glUniform1i(u("u_bump"), 3);

        ui_.program = link(gl_shaders::ui_vert, gl_shaders::ui_frag);
        ui_.framebuffer = glGetUniformLocation(ui_.program, "u_framebuffer");
        glUseProgram(ui_.program);
        glUniform1i(glGetUniformLocation(ui_.program, "u_texture"), 0);
        fill_program_ = link(gl_shaders::shadow_fill_vert, gl_shaders::shadow_fill_frag);
        fill_color_ = glGetUniformLocation(fill_program_, "u_color");
        glGenVertexArrays(1, &fill_vao_);  // attribute-less full-screen triangle

        glGenVertexArrays(1, &ui_vao_);
        glGenBuffers(1, &ui_vbo_);
        glBindVertexArray(ui_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, ui_vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex), reinterpret_cast<void*>(offsetof(UiVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UiVertex), reinterpret_cast<void*>(offsetof(UiVertex, uv)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(UiVertex), reinterpret_cast<void*>(offsetof(UiVertex, color)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(UiVertex), reinterpret_cast<void*>(offsetof(UiVertex, uv_rect)));
        glBindVertexArray(0);

        white_ = upload({1, 1, PixelFormat::Rgba8Unorm, {255, 255, 255, 255}, false, true, {}});
    }

    ~GlRenderer() override {
        if (timing_.frames)
            std::fprintf(stderr, "joan-native: opengl gpu timing (%llu frames, mean ms): scene %.3f\n",
                         static_cast<unsigned long long>(timing_.frames), timing_.scene_ms / double(timing_.frames));
        if (timing_.queries[0]) glDeleteQueries(2, timing_.queries);
        destroy_remaster();
        for (auto& [id, m] : meshes_) release(m);
        for (auto& [id, t] : textures_) glDeleteTextures(1, &t);
        glDeleteTextures(1, &white_);
        destroy_target();
        glDeleteBuffers(1, &ui_vbo_);
        glDeleteVertexArrays(1, &ui_vao_);
        glDeleteVertexArrays(1, &fill_vao_);
        glDeleteProgram(fill_program_);
        glDeleteProgram(mesh_.program);
        glDeleteProgram(ui_.program);
        SDL_GL_DestroyContext(context_);
    }

    const char* name() const override { return "opengl"; }

    MeshHandle create_mesh(const MeshData& data) override {
        Mesh m;
        glGenVertexArrays(1, &m.vao);
        glGenBuffers(1, &m.vbo);
        glGenBuffers(1, &m.ibo);
        glBindVertexArray(m.vao);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
        fill(m, data);
        auto attr = [](GLuint i, GLint n, std::size_t off) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, n, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(off));
        };
        attr(0, 3, offsetof(Vertex, position));
        attr(1, 3, offsetof(Vertex, normal));
        attr(2, 2, offsetof(Vertex, uv));
        glEnableVertexAttribArray(3);
        glVertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, bones)));
        attr(4, 4, offsetof(Vertex, weights));
        attr(5, 2, offsetof(Vertex, uv2));
        attr(6, 4, offsetof(Vertex, color));
        glBindVertexArray(0);
        const std::uint32_t id = next_id_++;
        meshes_[id] = m;
        return {id};
    }

    bool update_mesh(MeshHandle h, const MeshData& data) override {
        const auto it = meshes_.find(h.id);
        if (it == meshes_.end()) return false;
        Mesh& m = it->second;
        glBindVertexArray(m.vao);  // its element binding is the mesh ibo
        // TAA motion vectors: the first update of a frame keeps the replaced (last shown) vertices as attribute 7.
        if (m.updated != frame_) {
            m.updated = frame_;
            m.prev_count = data.vertices.size() == m.vertex_count ? m.vertex_count : 0;
            if (m.prev_count) {
                const GLsizeiptr bytes = GLsizeiptr(m.vertex_count * sizeof(Vertex));
                if (!m.prev_vbo) {
                    glGenBuffers(1, &m.prev_vbo);
                    glBindBuffer(GL_ARRAY_BUFFER, m.prev_vbo);
                    glEnableVertexAttribArray(7);
                    glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
                }
                glEnableVertexAttribArray(7);
                glBindBuffer(GL_COPY_WRITE_BUFFER, m.prev_vbo);
                glBufferData(GL_COPY_WRITE_BUFFER, bytes, nullptr, GL_STREAM_COPY);
                glBindBuffer(GL_COPY_READ_BUFFER, m.vbo);
                glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, bytes);
            } else if (m.prev_vbo) {
                // The vertex count changed: prev_vbo is smaller than the new mesh, so fetching attribute 7 from it
                // would read past its end. A disabled attribute reads the constant default instead.
                glDisableVertexAttribArray(7);
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        fill(m, data);
        glBindVertexArray(0);
        return true;
    }

    void destroy_mesh(MeshHandle h) override {
        if (auto it = meshes_.find(h.id); it != meshes_.end()) {
            release(it->second);
            meshes_.erase(it);
        }
    }

    TextureHandle create_texture(const TextureData& t) override {
        if (t.width <= 0 || t.height <= 0 || t.pixels.size() < std::size_t(t.width) * t.height * 4) return {};
        const std::uint32_t id = next_id_++;
        textures_[id] = upload(t);
        return {id};
    }

    void destroy_texture(TextureHandle h) override {
        if (auto it = textures_.find(h.id); it != textures_.end()) {
            glDeleteTextures(1, &it->second);
            textures_.erase(it);
        }
    }

    void set_graphics(const GraphicsSettings& g) override {
        if (g.shadow_map_size != gfx_.shadow_map_size) destroy_shadow_map();
        if (g.anisotropy != gfx_.anisotropy)  // already uploaded mipmapped textures follow the new setting
            for (const auto& [id, t] : textures_) {
                GLint min = 0;
                glBindTexture(GL_TEXTURE_2D, t);
                glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &min);
                if (min == GL_LINEAR_MIPMAP_LINEAR) glTexParameteri(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, g.anisotropy);
            }
        if (g.msaa != gfx_.msaa) destroy_original_target();
        gfx_ = g;
        if (g.upscale == Upscaler::Fsr3)  // FSR 3 / frame generation are Vulkan-only
            std::fprintf(stderr, "joan-native: opengl: upscale=fsr3%s runs as FSR1 at the same render scale (Vulkan only)\n",
                         g.frame_generation ? " + frame generation" : "");
        fsr3_fallback(gfx_);
        if (gfx_.msaa > 1) {  // clamp to what the colour and depth formats support
            GLint max_samples = 1;
            glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
            gfx_.msaa = std::min(gfx_.msaa, std::max(max_samples, 1));
        }
        remaster_ = g.remastered();
        history_.valid = false;
        if (remaster_)
            std::fprintf(stderr, "joan-native: opengl graphics preset %s (remaster path)\n", preset_name(g.preset));
        else if (g.original_post())
            std::fprintf(stderr, "joan-native: opengl graphics preset %s (original look + image-quality post)\n", preset_name(g.preset));
        if (remaster_ && g.rt.any())  // docs/RAYTRACING.md: ray tracing is Vulkan-only
            std::fputs("joan-native: opengl: ray tracing needs the Vulkan renderer, using the raster path\n", stderr);
    }

    void set_material_maps(TextureHandle albedo, const MaterialMaps& maps) override {
        if (albedo) material_maps_[albedo.id] = maps;
    }

    void begin_frame(glm::ivec2 size) override {
        size = glm::max(size, glm::ivec2(1));
        if (size != target_size_) create_target(size);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glEnable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, size.x, size.y);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void submit(const FrameData& f) override {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glEnable(GL_FRAMEBUFFER_SRGB);
        const bool timed = timing_begin();
        if (remaster_) draw_scene_remaster(f);
        else if (gfx_.original_post()) draw_scene_original_post(f);
        else draw_scene(f, viewport(f), target_size_.y);
        if (timed) glQueryCounter(timing_.queries[1], GL_TIMESTAMP);
        draw_ui(f);
        ++frame_;
    }

    void end_frame() override {
        // Raw copy of already sRGB-encoded bytes to the window surface.
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlitFramebuffer(0, 0, target_size_.x, target_size_.y, 0, 0, std::min(w, target_size_.x), std::min(h, target_size_.y),
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        SDL_GL_SwapWindow(window_);
        if (frame_ns_) {  // Wayland frame limiter: SDL_DelayPrecise sleeps coarse, then spins the last stretch
            const Uint64 now = SDL_GetTicksNS();
            if (now < next_frame_ns_) SDL_DelayPrecise(next_frame_ns_ - now);
            next_frame_ns_ = std::max(next_frame_ns_ + frame_ns_, now);  // a late frame restarts the cadence
        }
    }

    Image read_back() override {
        Image img{target_size_.x, target_size_.y, {}};
        if (!fbo_) return img;
        img.rgba.resize(std::size_t(img.width) * img.height * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, img.width, img.height, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        const std::size_t row = std::size_t(img.width) * 4;
        std::vector<std::uint8_t> tmp(row);
        for (int y = 0; y < img.height / 2; ++y) {
            auto* a = img.rgba.data() + y * row;
            auto* b = img.rgba.data() + (img.height - 1 - y) * row;
            std::copy(a, a + row, tmp.data());
            std::copy(b, b + row, a);
            std::copy(tmp.data(), tmp.data() + row, b);
        }
        for (std::size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;  // frames are opaque
        return img;
    }

private:
    struct Mesh {
        GLuint vao = 0, vbo = 0, ibo = 0;
        std::uint32_t index_count = 0, vertex_count = 0;
        // update_mesh: prev_vbo holds the vertices shown before the update of frame `updated` (prev_count of them, 0 = none).
        GLuint prev_vbo = 0;
        std::uint32_t prev_count = 0;
        std::uint64_t updated = ~0ull;
        bool prev_pose(std::uint64_t frame) const { return updated == frame && prev_count && prev_count == vertex_count; }
    };
    struct MeshProgram {
        GLuint program = 0;
        GLint model, view_proj, normal_matrix, tint, alpha_cutoff, unlit, no_fog, sun_dir, sun_color, ambient, fog_color, fog_range, camera_pos,
            has_layer2, has_lightmap, light_count, light_pos_range, light_color, light_att, bump_scale, diffuse, vertex_diffuse, a2c;
    };
    struct UiProgram {
        GLuint program = 0;
        GLint framebuffer;
    };

    static void release(Mesh& m) {
        glDeleteBuffers(1, &m.vbo);
        glDeleteBuffers(1, &m.ibo);
        if (m.prev_vbo) glDeleteBuffers(1, &m.prev_vbo);
        glDeleteVertexArrays(1, &m.vao);
    }

    // Uploads into the currently bound GL_ARRAY_BUFFER / element buffer (of the bound VAO).
    static void fill(Mesh& m, const MeshData& data) {
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.vertices.size() * sizeof(Vertex)), data.vertices.data(), GL_STATIC_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(data.indices.size() * sizeof(std::uint32_t)), data.indices.data(), GL_STATIC_DRAW);
        m.index_count = std::uint32_t(data.indices.size());
        m.vertex_count = std::uint32_t(data.vertices.size());
    }

    GLuint upload(const TextureData& t) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // Rows are uploaded top-first, so UV (0,0) addresses the top-left texel (Direct3D convention).
        glTexImage2D(GL_TEXTURE_2D, 0, t.format == PixelFormat::Rgba8Srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, t.width, t.height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, t.pixels.data());
        const GLint wrap = t.repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t.mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        for (std::size_t i = 0; t.mipmaps && i < t.mip_pixels.size(); ++i)
            glTexImage2D(GL_TEXTURE_2D, GLint(i + 1), t.format == PixelFormat::Rgba8Srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                         std::max(t.width >> (i + 1), 1), std::max(t.height >> (i + 1), 1), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         t.mip_pixels[i].data());
        if (!t.mip_pixels.empty()) glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, GLint(t.mip_pixels.size()));
        if (t.mipmaps) {
            if (t.mip_pixels.empty()) glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY; ignored (GL error) if unsupported */, gfx_.anisotropy);
        }
        return tex;
    }

    GLuint texture(TextureHandle h) const {
        const auto it = textures_.find(h.id);
        return it == textures_.end() ? white_ : it->second;
    }

    void create_target(glm::ivec2 size) {
        destroy_target();
        glGenRenderbuffers(1, &color_rb_);
        glBindRenderbuffer(GL_RENDERBUFFER, color_rb_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_SRGB8_ALPHA8, size.x, size.y);
        glGenRenderbuffers(1, &depth_rb_);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, size.x, size.y);
        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rb_);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("GL framebuffer incomplete");
        target_size_ = size;
    }

    void destroy_target() {
        if (fbo_) glDeleteFramebuffers(1, &fbo_);
        if (color_rb_) glDeleteRenderbuffers(1, &color_rb_);
        if (depth_rb_) glDeleteRenderbuffers(1, &depth_rb_);
        fbo_ = color_rb_ = depth_rb_ = 0;
        target_size_ = {0, 0};
    }

    glm::ivec4 viewport(const FrameData& f) const {
        return f.view.viewport.z > 0 ? f.view.viewport : glm::ivec4(0, 0, target_size_.x, target_size_.y);
    }

    // ---- Original look + image-quality options (msaa, aa, render_scale, sharpness): docs/GRAPHICS_ROADMAP.md
    // The unchanged Original passes render into a scene-size sRGB target (multisampled when msaa > 1, resolved),
    // then the shared post tail (FXAA/SMAA -> EASU -> RCAS/downsample) writes the viewport of the display target.
    struct OriginalTarget {
        glm::ivec2 size{0, 0};
        int samples = 0;
        GLuint color = 0, fbo = 0, depth_rb = 0, ms_color = 0, ms_fbo = 0;
    };

    void ensure_original_target(glm::ivec2 size) {
        OriginalTarget& t = ot_;
        if (t.size == size && t.samples == gfx_.msaa && t.fbo) return;
        destroy_original_target();
        t.size = size;
        t.samples = gfx_.msaa;
        t.color = make_texture(GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, size, GL_LINEAR);
        // Post passes read the stored (encoded) bytes, like the remaster LDR target (EXT_texture_sRGB_decode).
        glTexParameteri(GL_TEXTURE_2D, 0x8A48 /* GL_TEXTURE_SRGB_DECODE_EXT */, 0x8A4A /* GL_SKIP_DECODE_EXT */);
        glGenRenderbuffers(1, &t.depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, t.depth_rb);
        if (t.samples > 1) {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, t.samples, GL_DEPTH24_STENCIL8, size.x, size.y);
            glGenRenderbuffers(1, &t.ms_color);
            glBindRenderbuffer(GL_RENDERBUFFER, t.ms_color);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, t.samples, GL_SRGB8_ALPHA8, size.x, size.y);
            glGenFramebuffers(1, &t.ms_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, t.ms_fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.ms_color);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth_rb);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("GL MSAA framebuffer incomplete");
            t.fbo = make_fbo({t.color}, 0);  // resolve target
        } else {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, size.x, size.y);
            t.fbo = make_fbo({t.color}, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth_rb);
        }
    }

    void destroy_original_target() {
        OriginalTarget& t = ot_;
        for (GLuint f : {t.fbo, t.ms_fbo})
            if (f) glDeleteFramebuffers(1, &f);
        for (GLuint r : {t.depth_rb, t.ms_color})
            if (r) glDeleteRenderbuffers(1, &r);
        if (t.color) glDeleteTextures(1, &t.color);
        t = {};
    }

    void draw_scene_original_post(const FrameData& f) {
        const glm::ivec4 vp = viewport(f);
        const glm::ivec2 output(vp.z, vp.w), scene = scene_size(gfx_, output);
        ensure_remaster_programs();
        ensure_original_target(scene);
        const RemasterFrame rf = build_remaster_frame(gfx_, f, scene, output, 1, history_);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof rf, &rf);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_);
        glBindFramebuffer(GL_FRAMEBUFFER, ot_.samples > 1 ? ot_.ms_fbo : ot_.fbo);
        glEnable(GL_FRAMEBUFFER_SRGB);
        draw_scene(f, glm::ivec4(0, 0, scene), scene.y, ot_.samples > 1);
        if (ot_.samples > 1) {  // resolve (linear-space average: the sRGB conversion is on)
            glBindFramebuffer(GL_READ_FRAMEBUFFER, ot_.ms_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ot_.fbo);
            glBlitFramebuffer(0, 0, scene.x, scene.y, 0, 0, scene.x, scene.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        glDisable(GL_FRAMEBUFFER_SRGB);  // the post passes write encoded bytes
        finish_post(ot_.color, scene, output, vp);
        glEnable(GL_FRAMEBUFFER_SRGB);
    }

    // vp: scene viewport in pixels (top-left origin) of a target `target_h` pixels high; multisampled: alpha-to-coverage.
    void draw_scene(const FrameData& f, glm::ivec4 vp, int target_h, bool multisampled = false) {
        const GLint y = target_h - vp.y - vp.w;  // top-left origin -> GL bottom-left
        glViewport(vp.x, y, vp.z, vp.w);
        glScissor(vp.x, y, vp.z, vp.w);
        glEnable(GL_SCISSOR_TEST);
        const glm::vec4& cc = f.lighting.clear_color;
        glClearColor(cc.r, cc.g, cc.b, cc.a);
        glDepthMask(GL_TRUE);
        glStencilMask(0xff);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        const Lighting& l = f.lighting;
        glUseProgram(mesh_.program);
        glUniformMatrix4fv(mesh_.view_proj, 1, GL_FALSE, glm::value_ptr(f.view.projection * f.view.view));
        glUniform3fv(mesh_.sun_dir, 1, glm::value_ptr(glm::normalize(l.sun_direction)));
        glUniform3fv(mesh_.sun_color, 1, glm::value_ptr(l.sun_color));
        glUniform3fv(mesh_.ambient, 1, glm::value_ptr(l.ambient));
        glUniform3fv(mesh_.fog_color, 1, glm::value_ptr(l.fog_color));
        glUniform2f(mesh_.fog_range, l.fog_start, l.fog_end);
        glUniform3fv(mesh_.camera_pos, 1, glm::value_ptr(f.view.camera_position));
        point_lights_ = l.point_lights;
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        // Opaque + alpha-tested first (stencil = 128 on shadow receivers, else 0), then the shadow
        // volumes and the shadow fill, then blended back to front (depth writes only for Material::depth_write).
        blended_.clear();
        volumes_.clear();
        glEnable(GL_STENCIL_TEST);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        int receive = -1;
        bool a2c_on = false;  // alpha-tested draws into a multisampled target: alpha-to-coverage
        for (const DrawItem& d : f.draws) {
            const BlendMode b = d.material.blend;
            if (b == BlendMode::ShadowVolume) volumes_.push_back(&d);
            else if (b == BlendMode::AlphaBlend || b == BlendMode::Additive) blended_.push_back(&d);
            else {
                if (int(d.material.receive_shadow) != receive)
                    receive = d.material.receive_shadow, glStencilFunc(GL_ALWAYS, receive ? 128 : 0, 0xff);
                if (const bool a2c = multisampled && b == BlendMode::AlphaTest; a2c != a2c_on) {
                    a2c_on = a2c;
                    if (a2c) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
                    else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
                    glUniform1i(mesh_.a2c, a2c ? 1 : 0);
                }
                draw_item(d);
            }
        }
        if (a2c_on) {
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            glUniform1i(mesh_.a2c, 0);
        }
        if (!volumes_.empty() && l.shadow_color.a > 0.0f) {
            // z-pass volumes: front faces +1, back faces -1 (clamped), no colour or depth writes.
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_FALSE);
            glStencilFunc(GL_ALWAYS, 0, 0xff);
            glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR);
            glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR);
            for (const DrawItem* d : volumes_) draw_item(*d);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            // Fill where 128 < stencil (D3D STENCILFUNC LESS, ref 128).
            glStencilFunc(GL_LESS, 128, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(fill_program_);
            glUniform4fv(fill_color_, 1, glm::value_ptr(shadow_fill_color(l.shadow_color)));
            glBindVertexArray(fill_vao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glUseProgram(mesh_.program);
            glEnable(GL_DEPTH_TEST);
        }
        glDisable(GL_STENCIL_TEST);
        glEnable(GL_BLEND);
        const glm::vec3 cam = f.view.camera_position;
        std::sort(blended_.begin(), blended_.end(), [&](const DrawItem* a, const DrawItem* b) {
            const glm::vec3 pa(a->model[3]), pb(b->model[3]);
            return glm::dot(pa - cam, pa - cam) > glm::dot(pb - cam, pb - cam);
        });
        for (const DrawItem* d : blended_) {
            glDepthMask(d->material.depth_write ? GL_TRUE : GL_FALSE);
            glBlendFunc(GL_SRC_ALPHA, d->material.blend == BlendMode::Additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
            draw_item(*d);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
    }

    void draw_item(const DrawItem& d) {
        const auto it = meshes_.find(d.mesh.id);
        if (it == meshes_.end()) return;
        const Mesh& m = it->second;
        const Material& mat = d.material;
        if (mat.two_sided) glDisable(GL_CULL_FACE);
        else {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        }
        glUniformMatrix4fv(mesh_.model, 1, GL_FALSE, glm::value_ptr(d.model));
        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(d.model)));
        glUniformMatrix3fv(mesh_.normal_matrix, 1, GL_FALSE, glm::value_ptr(nm));
        glUniform4fv(mesh_.tint, 1, glm::value_ptr(mat.tint));
        glUniform1f(mesh_.alpha_cutoff, alpha_test_cutoff(mat));
        glUniform1i(mesh_.unlit, mat.unlit ? 1 : 0);
        glUniform1i(mesh_.no_fog, mat.fog ? 0 : 1);
        glUniform1i(mesh_.has_layer2, mat.layer2 ? 1 : 0);
        glUniform1i(mesh_.has_lightmap, mat.lightmap ? 1 : 0);
        int lights = 0;
        if (!mat.unlit) {
            glm::vec4 pos_range[kMaxDrawLights];
            glm::vec3 color[kMaxDrawLights], att[kMaxDrawLights];
            for (int i = 0; i < std::min<int>(d.light_count, kMaxDrawLights); ++i) {
                if (d.lights[i] >= point_lights_.size()) continue;
                const PointLight& p = point_lights_[d.lights[i]];
                pos_range[lights] = glm::vec4(p.position, p.range);
                color[lights] = p.color;
                att[lights++] = p.attenuation;
            }
            if (lights) {
                glUniform4fv(mesh_.light_pos_range, lights, glm::value_ptr(pos_range[0]));
                glUniform3fv(mesh_.light_color, lights, glm::value_ptr(color[0]));
                glUniform3fv(mesh_.light_att, lights, glm::value_ptr(att[0]));
            }
        }
        glUniform1i(mesh_.light_count, lights);
        glUniform3fv(mesh_.diffuse, 1, glm::value_ptr(mat.diffuse));
        glUniform1i(mesh_.vertex_diffuse, mat.vertex_diffuse ? 1 : 0);
        const bool bump = mat.bump && mat.bump_scale != 0.0f;
        glUniform1f(mesh_.bump_scale, bump ? mat.bump_scale : 0.0f);
        if (bump) {
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, texture(mat.bump));
        }
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, texture(mat.lightmap));
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texture(mat.layer2));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture(mat.albedo));
        glBindVertexArray(m.vao);
        const std::uint32_t first = std::min(d.first_index, m.index_count);
        const std::uint32_t count = d.index_count ? std::min(d.index_count, m.index_count - first) : m.index_count - first;
        glDrawElements(GL_TRIANGLES, GLsizei(count), GL_UNSIGNED_INT, reinterpret_cast<void*>(std::size_t(first) * 4));
    }

    void draw_ui(const FrameData& f) {
        const std::span<const UiQuad> quads = f.ui;
        if (quads.empty() && f.ui_triangles.empty()) return;
        ui_vertices_.clear();
        for (const UiQuad& q : quads) {
            const glm::vec2 p0 = q.position, p1 = q.position + q.size;
            const glm::vec4 r{glm::min(q.uv0, q.uv1) + q.clamp_inset, glm::max(q.uv0, q.uv1) - q.clamp_inset};
            const glm::vec2 uv[4] = {q.uv0, {q.uv1.x, q.uv0.y}, q.uv1, {q.uv0.x, q.uv1.y}};  // TL TR BR BL, turned by q.turns
            const UiVertex v[4] = {{p0, uv[q.turns & 3], q.color, r}, {{p1.x, p0.y}, uv[(q.turns + 1) & 3], q.color, r},
                                   {p1, uv[(q.turns + 2) & 3], q.color, r}, {{p0.x, p1.y}, uv[(q.turns + 3) & 3], q.color, r}};
            ui_vertices_.insert(ui_vertices_.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
        }
        const GLint tri_base = GLint(ui_vertices_.size());
        ui_vertices_.insert(ui_vertices_.end(), f.ui_vertices.begin(), f.ui_vertices.end());
        glViewport(0, 0, target_size_.x, target_size_.y);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(ui_.program);
        glUniform2f(ui_.framebuffer, float(target_size_.x), float(target_size_.y));
        glBindVertexArray(ui_vao_);
        glBindBuffer(GL_ARRAY_BUFFER, ui_vbo_);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(ui_vertices_.size() * sizeof(UiVertex)), ui_vertices_.data(), GL_STREAM_DRAW);
        glActiveTexture(GL_TEXTURE0);
        // D3D8 blended the 2D interface in gamma space: ui.frag outputs sRGB-encoded colour and the target's
        // sRGB conversion is off, so blending operates on the stored (encoded) bytes.
        glDisable(GL_FRAMEBUFFER_SRGB);
        // Batch runs of quads sharing a texture.
        std::size_t start = 0;
        for (std::size_t i = 1; i <= quads.size(); ++i) {
            if (i < quads.size() && quads[i].texture.id == quads[start].texture.id) continue;
            glBindTexture(GL_TEXTURE_2D, texture(quads[start].texture));
            glDrawArrays(GL_TRIANGLES, GLint(start * 6), GLsizei((i - start) * 6));
            start = i;
        }
        if (!f.ui_triangles.empty()) {
            glEnable(GL_SCISSOR_TEST);
            for (const UiTriangles& t : f.ui_triangles) {
                const glm::ivec4 c = glm::clamp(t.clip, glm::ivec4(0), glm::ivec4(target_size_, target_size_));
                if (c.z <= c.x || c.w <= c.y || !t.count) continue;
                glScissor(c.x, target_size_.y - c.w, c.z - c.x, c.w - c.y);  // GL: bottom-left origin
                glBindTexture(GL_TEXTURE_2D, texture(t.texture));
                glDrawArrays(GL_TRIANGLES, tri_base + GLint(t.first), GLsizei(t.count));
            }
            glDisable(GL_SCISSOR_TEST);
        }
        glDisable(GL_BLEND);
        glEnable(GL_FRAMEBUFFER_SRGB);
        glBindVertexArray(0);
    }

    // ---- remaster (Enhanced/Ultra): docs/GRAPHICS_ROADMAP.md "Remaster" ------------------------------
    // Passes: sun shadow map -> opaque (HDR colour + ambient + motion + auto exposure reference MRTs, depth-stencil texture)
    // [-> stencil volumes if no shadow map] -> SSAO -> AO apply (reverse subtract) -> translucent -> auto exposure measure +
    // adapt -> composite (volumetric fog, exposure, ACES, sRGB encode) -> TAA -> EASU -> RCAS/copy into the display target.

    struct PbrProgram {
        GLuint program = 0;
        GLint model, normal_matrix, no_fog, pass, tint, alpha_cutoff, unlit, has_layer2, has_lightmap, bump_scale, maps, cls, blend, diffuse,
            vertex_diffuse, light_count, light_pos_range, light_color, light_att, prev_model, prev_pose, a2c, water;
    };
    struct RemasterTargets {
        glm::ivec2 scene{0, 0}, output{0, 0};
        GLuint hdr = 0, ambient = 0, depth = 0, ao = 0, ldr = 0, hist[2]{};
        GLuint velocity = 0;  // RG16F TAA motion (previous minus current uv) of the opaque + depth-writing blended draws
        GLuint reference = 0;  // R16F auto exposure reference: Original-look luma (mesh_pbr_frag.glsl o_reference)
        GLuint lum = 0, exposure[2]{};  // auto exposure: 64x32 measurement, 1x1 R32F state (ping-pong by frame parity)
        GLuint fbo_opaque = 0, fbo_trans = 0, fbo_hdr = 0, fbo_ao = 0, fbo_ldr = 0, fbo_hist[2]{}, fbo_lum = 0, fbo_exposure[2]{};
        // msaa > 1: multisampled scene renderbuffers (same attachment layout as fbo_opaque / fbo_trans / fbo_hdr), resolved
        // into the textures above after the opaque pass (depth, ambient, motion) and after the translucent pass (hdr, motion,
        // reference, depth).
        int samples = 1;
        GLuint ms_hdr = 0, ms_ambient = 0, ms_velocity = 0, ms_reference = 0, ms_depth = 0, fbo_opaque_ms = 0, fbo_trans_ms = 0, fbo_hdr_ms = 0;
        // Stylised water (pbr_shade.glsl shade_water): the opaque scene's colour (MSAA: resolved) and depth, copied before the
        // translucent pass.
        GLuint water_color = 0, water_depth = 0, fbo_water = 0;
        GLuint fbo_ambient = 0, fbo_velocity = 0, fbo_reference = 0;  // single-attachment resolve destinations
    };
    // Image-quality post tail (any preset): FXAA/SMAA at scene size, EASU at output size.
    struct PostTargets {
        glm::ivec2 scene{0, 0}, output{0, 0};
        GLuint edges = 0, weights = 0, out = 0, easu = 0;
        GLuint fbo_edges = 0, fbo_weights = 0, fbo_out = 0, fbo_easu = 0;
    };

    // GL texture units of the remaster samplers: Vulkan set-2 binding b <-> unit b + 7 (remaster_prelude.glsl).
    GLuint link_remaster(const char* vs_body, const char* fs_body, bool pbr) {
        const std::string pre = std::string(gl_shaders::remaster_prelude) + gl_shaders::remaster_common;
        const std::string vs = pre + vs_body, fs = pre + (pbr ? gl_shaders::pbr_shade : "") + fs_body;
        const GLuint p = link(vs.c_str(), fs.c_str());
        glUseProgram(p);
        static const std::pair<const char*, int> units[] = {
            {"u_albedo", 0},  {"u_layer2", 1},  {"u_lightmap", 2},    {"u_bump", 3},  {"u_normal_map", 4}, {"u_rough_map", 5}, {"u_metal_map", 6},
            {"u_ao_map", 7},  {"u_shadow", 8},  {"u_env", 9},         {"u_irradiance", 10}, {"u_hdr", 11},  {"u_ambient", 12}, {"u_depth", 13},
            {"u_ao", 14},     {"u_ldr", 15},    {"u_history", 16},    {"u_src", 17},  {"u_final", 19}, {"u_velocity", 20},
            {"u_aa_a", 21},   {"u_aa_b", 22},   {"u_area", 23},       {"u_search", 24}, {"u_exposure_prev", 25}, {"u_lum", 26},
            {"u_exposure", 27}, {"u_reference", 28}, {"u_water_color", 29}, {"u_water_depth", 30}, {"u_water_waves", 31}};
        for (const auto& [n, unit] : units)
            if (const GLint loc = glGetUniformLocation(p, n); loc >= 0) glUniform1i(loc, unit);
        if (const GLuint block = glGetUniformBlockIndex(p, "RemasterFrame"); block != GL_INVALID_INDEX) glUniformBlockBinding(p, block, 0);
        return p;
    }

    void ensure_remaster_programs() {
        if (pbr_.program) return;
        pbr_.program = link_remaster(gl_shaders::mesh_pbr_vert, gl_shaders::mesh_pbr_frag, true);
        auto u = [&](const char* n) { return glGetUniformLocation(pbr_.program, n); };
        pbr_ = {pbr_.program, u("u_model"), u("u_normal_matrix"), u("u_no_fog"), u("u_pass"), u("u_tint"), u("u_alpha_cutoff"), u("u_unlit"),
                u("u_has_layer2"), u("u_has_lightmap"), u("u_bump_scale"), u("u_maps"), u("u_class"), u("u_blend"), u("u_diffuse"),
                u("u_vertex_diffuse"), u("u_light_count"), u("u_light_pos_range"), u("u_light_color"), u("u_light_att"), u("u_prev_model"),
                u("u_prev_pose"), u("u_a2c"), u("u_water")};
        prog_ssao_ = link_remaster(gl_shaders::post_vert, gl_shaders::post_ssao, false);
        prog_ao_apply_ = link_remaster(gl_shaders::post_vert, gl_shaders::post_ao_apply, false);
        const std::string fog = gl_shaders::post_fog;
        prog_composite_ = link_remaster(gl_shaders::post_vert, (fog + gl_shaders::post_composite).c_str(), false);
        prog_exposure_[0] = link_remaster(gl_shaders::post_vert, (fog + gl_shaders::post_exposure_lum).c_str(), false);
        prog_exposure_[1] = link_remaster(gl_shaders::post_vert, gl_shaders::post_exposure_adapt, false);
        prog_taa_ = link_remaster(gl_shaders::post_vert, gl_shaders::post_taa, false);
        prog_easu_ = link_remaster(gl_shaders::post_vert, gl_shaders::post_easu, false);
        prog_rcas_ = link_remaster(gl_shaders::post_vert, gl_shaders::post_rcas, false);
        prog_fxaa_ = link_remaster(gl_shaders::post_vert, gl_shaders::post_fxaa, false);
        const std::string smaa = std::string(gl_shaders::smaa_config) + gl_shaders::SMAA;
        prog_smaa_[0] = link_remaster(gl_shaders::post_vert, (smaa + gl_shaders::post_smaa_edges).c_str(), false);
        prog_smaa_[1] = link_remaster(gl_shaders::post_vert, (smaa + gl_shaders::post_smaa_weights).c_str(), false);
        prog_smaa_[2] = link_remaster(gl_shaders::post_vert, (smaa + gl_shaders::post_smaa_blend).c_str(), false);
        glGenBuffers(1, &ubo_);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(RemasterFrame), nullptr, GL_DYNAMIC_DRAW);
        waves_ = make_texture(GL_R16F, GL_RED, GL_FLOAT, glm::ivec2(kWaterWaveGrid), GL_LINEAR);
    }

    static GLuint make_texture(GLenum internal, GLenum format, GLenum type, glm::ivec2 size, GLint filter) {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(internal), size.x, size.y, 0, format, type, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return t;
    }

    static GLuint make_fbo(std::initializer_list<GLuint> colors, GLuint depth, GLenum depth_attachment = GL_DEPTH_STENCIL_ATTACHMENT) {
        GLuint f = 0;
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        GLenum bufs[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
        GLsizei n = 0;
        for (GLuint c : colors) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + n, GL_TEXTURE_2D, c, 0);
            bufs[n] = GL_COLOR_ATTACHMENT0 + n;
            ++n;
        }
        if (depth) glFramebufferTexture2D(GL_FRAMEBUFFER, depth_attachment, GL_TEXTURE_2D, depth, 0);
        glDrawBuffers(std::max<GLsizei>(n, 1), bufs);
        glReadBuffer(n ? GL_COLOR_ATTACHMENT0 : GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("GL remaster framebuffer incomplete");
        return f;
    }

    void ensure_remaster_targets(glm::ivec2 scene, glm::ivec2 output) {
        RemasterTargets& t = rt_;
        if (t.scene == scene && t.output == output && t.samples == gfx_.msaa && t.hdr) return;
        destroy_remaster_targets();
        t.scene = scene;
        t.output = output;
        t.samples = gfx_.msaa;
        t.hdr = make_texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, scene, GL_LINEAR);
        t.ambient = make_texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, scene, GL_NEAREST);
        t.depth = make_texture(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, scene, GL_NEAREST);
        t.water_color = make_texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, scene, GL_LINEAR);
        t.water_depth = make_texture(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, scene, GL_NEAREST);
        t.fbo_water = make_fbo({t.water_color}, t.water_depth);
        t.ao = make_texture(GL_R8, GL_RED, GL_UNSIGNED_BYTE, scene, GL_LINEAR);
        t.ldr = make_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, scene, GL_LINEAR);
        for (GLuint& h : t.hist) h = make_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, scene, GL_LINEAR);
        t.velocity = make_texture(GL_RG16F, GL_RG, GL_HALF_FLOAT, scene, GL_NEAREST);
        t.reference = make_texture(GL_R16F, GL_RED, GL_HALF_FLOAT, scene, GL_NEAREST);
        t.lum = make_texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, {64, 32}, GL_NEAREST);
        t.fbo_lum = make_fbo({t.lum}, 0);
        for (int i = 0; i < 2; ++i) {
            t.exposure[i] = make_texture(GL_R32F, GL_RED, GL_FLOAT, {1, 1}, GL_NEAREST);
            t.fbo_exposure[i] = make_fbo({t.exposure[i]}, 0);
        }
        t.fbo_opaque = make_fbo({t.hdr, t.ambient, t.velocity, t.reference}, t.depth);
        // Draw buffers set per draw: o_motion (location 2) -> attachment 1, o_reference (location 3) -> attachment 2.
        t.fbo_trans = make_fbo({t.hdr, t.velocity, t.reference}, t.depth);
        t.fbo_hdr = make_fbo({t.hdr}, 0);
        t.fbo_ao = make_fbo({t.ao}, 0);
        t.fbo_ldr = make_fbo({t.ldr}, 0);
        for (int i = 0; i < 2; ++i) t.fbo_hist[i] = make_fbo({t.hist[i]}, 0);
        if (t.samples > 1) {
            const auto rb = [&](GLenum format) {
                GLuint r = 0;
                glGenRenderbuffers(1, &r);
                glBindRenderbuffer(GL_RENDERBUFFER, r);
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, t.samples, format, scene.x, scene.y);
                return r;
            };
            t.ms_hdr = rb(GL_RGBA16F);
            t.ms_ambient = rb(GL_RGBA16F);
            t.ms_velocity = rb(GL_RG16F);
            t.ms_reference = rb(GL_R16F);
            t.ms_depth = rb(GL_DEPTH24_STENCIL8);
            t.fbo_opaque_ms = make_rb_fbo({t.ms_hdr, t.ms_ambient, t.ms_velocity, t.ms_reference}, t.ms_depth);
            t.fbo_trans_ms = make_rb_fbo({t.ms_hdr, t.ms_velocity, t.ms_reference}, t.ms_depth);
            t.fbo_hdr_ms = make_rb_fbo({t.ms_hdr}, 0);
            t.fbo_ambient = make_fbo({t.ambient}, 0);
            t.fbo_velocity = make_fbo({t.velocity}, 0);
            t.fbo_reference = make_fbo({t.reference}, 0);
        }
        history_.valid = false;
    }

    static GLuint make_rb_fbo(std::initializer_list<GLuint> colors, GLuint depth) {
        GLuint f = 0;
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        GLenum bufs[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
        GLsizei n = 0;
        for (GLuint c : colors) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + n, GL_RENDERBUFFER, c);
            bufs[n] = GL_COLOR_ATTACHMENT0 + n;
            ++n;
        }
        if (depth) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
        glDrawBuffers(n, bufs);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw std::runtime_error("GL MSAA remaster framebuffer incomplete");
        return f;
    }

    // MSAA resolve: colour attachment `index` of the multisampled `from` into the single-attachment `to` (average), or depth.
    void resolve(GLuint from, int index, GLuint to, glm::ivec2 size) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, from);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
        if (index >= 0) glReadBuffer(GL_COLOR_ATTACHMENT0 + GLenum(index));
        glBlitFramebuffer(0, 0, size.x, size.y, 0, 0, size.x, size.y, index >= 0 ? GL_COLOR_BUFFER_BIT : GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }

    void destroy_remaster_targets() {
        RemasterTargets& t = rt_;
        for (GLuint f : {t.fbo_opaque, t.fbo_trans, t.fbo_hdr, t.fbo_ao, t.fbo_ldr, t.fbo_hist[0], t.fbo_hist[1], t.fbo_opaque_ms, t.fbo_trans_ms,
                         t.fbo_hdr_ms, t.fbo_ambient, t.fbo_velocity, t.fbo_reference, t.fbo_lum, t.fbo_exposure[0], t.fbo_exposure[1], t.fbo_water})
            if (f) glDeleteFramebuffers(1, &f);
        for (GLuint r : {t.ms_hdr, t.ms_ambient, t.ms_velocity, t.ms_reference, t.ms_depth})
            if (r) glDeleteRenderbuffers(1, &r);
        for (GLuint x : {t.hdr, t.ambient, t.depth, t.ao, t.ldr, t.hist[0], t.hist[1], t.velocity, t.reference, t.lum, t.exposure[0], t.exposure[1],
                         t.water_color, t.water_depth})
            if (x) glDeleteTextures(1, &x);
        t = {};
    }

    void ensure_post_targets(glm::ivec2 scene, glm::ivec2 output) {
        PostTargets& t = pt_;
        if (t.scene == scene && t.output == output && t.out) return;
        destroy_post_targets();
        t.scene = scene;
        t.output = output;
        t.edges = make_texture(GL_RG8, GL_RG, GL_UNSIGNED_BYTE, scene, GL_LINEAR);  // SMAA samples edges bilinearly
        t.weights = make_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, scene, GL_LINEAR);
        t.out = make_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, scene, GL_LINEAR);
        t.easu = make_texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, output, GL_LINEAR);
        t.fbo_edges = make_fbo({t.edges}, 0);
        t.fbo_weights = make_fbo({t.weights}, 0);
        t.fbo_out = make_fbo({t.out}, 0);
        t.fbo_easu = make_fbo({t.easu}, 0);
    }

    void destroy_post_targets() {
        PostTargets& t = pt_;
        for (GLuint f : {t.fbo_edges, t.fbo_weights, t.fbo_out, t.fbo_easu})
            if (f) glDeleteFramebuffers(1, &f);
        for (GLuint x : {t.edges, t.weights, t.out, t.easu})
            if (x) glDeleteTextures(1, &x);
        t = {};
    }

    // Shared tail of both paths: src (scene size, encoded LDR) -> [FXAA | SMAA] -> [EASU] -> RCAS/copy/downsample
    // into the viewport `vp` of the display target. Expects the frame block bound and GL_FRAMEBUFFER_SRGB off.
    void finish_post(GLuint src, glm::ivec2 scene, glm::ivec2 output, glm::ivec4 vp) {
        ensure_post_targets(scene, output);
        if (gfx_.aa == AntiAliasing::Fxaa) {
            bind_unit(15, src);
            fullscreen(prog_fxaa_, pt_.fbo_out, scene);
            src = pt_.out;
        } else if (gfx_.aa == AntiAliasing::Smaa) {
            if (!smaa_area_) {
                smaa_area_ = create_texture(smaa_area_texture());
                smaa_search_ = create_texture(smaa_search_texture());
            }
            bind_unit(15, src);
            glBindFramebuffer(GL_FRAMEBUFFER, pt_.fbo_edges);
            const glm::vec4 zero(0.0f);
            glClearBufferfv(GL_COLOR, 0, glm::value_ptr(zero));  // the edge pass discards edge-free pixels
            fullscreen(prog_smaa_[0], pt_.fbo_edges, scene);
            bind_unit(21, pt_.edges);
            bind_unit(23, texture(smaa_area_));
            bind_unit(24, texture(smaa_search_));
            fullscreen(prog_smaa_[1], pt_.fbo_weights, scene);
            bind_unit(22, pt_.weights);
            fullscreen(prog_smaa_[2], pt_.fbo_out, scene);
            for (int unit : {21, 22, 23, 24}) bind_unit(unit, 0);
            src = pt_.out;
        }
        if (gfx_.upscale == Upscaler::Fsr1 && scene.x < output.x) {
            bind_unit(17, src);
            fullscreen(prog_easu_, pt_.fbo_easu, output);
            src = pt_.easu;
        }
        bind_unit(19, src);
        for (int unit : {17, 16, 15, 13}) bind_unit(unit, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        const GLint y = target_size_.y - vp.y - vp.w;  // top-left origin -> GL bottom-left
        glViewport(vp.x, y, vp.z, vp.w);
        glUseProgram(prog_rcas_);
        glBindVertexArray(fill_vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        bind_unit(19, 0);
        glActiveTexture(GL_TEXTURE0);
    }

    void ensure_shadow_map() {
        if (shadow_tex_ || gfx_.shadow_map_size <= 0) return;
        const glm::ivec2 s(gfx_.shadow_map_size);
        shadow_tex_ = make_texture(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, s, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        shadow_fbo_ = make_fbo({}, shadow_tex_, GL_DEPTH_ATTACHMENT);
    }

    void destroy_shadow_map() {
        if (shadow_fbo_) glDeleteFramebuffers(1, &shadow_fbo_);
        if (shadow_tex_) glDeleteTextures(1, &shadow_tex_);
        shadow_fbo_ = shadow_tex_ = 0;
    }

    void destroy_remaster() {
        destroy_remaster_targets();
        destroy_post_targets();
        destroy_original_target();
        destroy_shadow_map();
        for (GLuint p : {pbr_.program, prog_ssao_, prog_ao_apply_, prog_composite_, prog_exposure_[0], prog_exposure_[1], prog_taa_, prog_easu_,
                         prog_rcas_, prog_fxaa_, prog_smaa_[0], prog_smaa_[1], prog_smaa_[2]})
            if (p) glDeleteProgram(p);
        if (ubo_) glDeleteBuffers(1, &ubo_);
        if (waves_) glDeleteTextures(1, &waves_);
    }

    void ensure_env(const Lighting& l) {
        const std::uint64_t key = env_key(l);
        if (env_radiance_ && key == env_key_) return;
        destroy_texture(env_radiance_);
        destroy_texture(env_irradiance_);
        const EnvMaps m = build_env_maps(l);
        env_radiance_ = create_texture(m.radiance);
        env_irradiance_ = create_texture(m.irradiance);
        env_levels_ = m.radiance_levels;
        env_key_ = key;
    }

    static bool is_terrain(const Material& m) { return m.lightmap && m.vertex_diffuse; }

    void bind_unit(int unit, GLuint tex) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, tex);
    }

    // pass 0 = scene, 1 = shadow caster (depth only).
    void draw_pbr(const DrawItem& d, int pass) {
        const auto it = meshes_.find(d.mesh.id);
        if (it == meshes_.end()) return;
        const Mesh& m = it->second;
        const Material& mat = d.material;
        if (mat.two_sided || pass == 1) glDisable(GL_CULL_FACE);
        else {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        }
        glUniformMatrix4fv(pbr_.model, 1, GL_FALSE, glm::value_ptr(d.model));
        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(d.model)));
        glUniformMatrix3fv(pbr_.normal_matrix, 1, GL_FALSE, glm::value_ptr(nm));
        glUniform1i(pbr_.pass, pass);
        glUniformMatrix4fv(pbr_.prev_model, 1, GL_FALSE, glm::value_ptr(prev_models_[std::size_t(&d - draws_base_)]));
        glUniform1i(pbr_.prev_pose, m.prev_pose(frame_) ? 1 : 0);
        glUniform4fv(pbr_.tint, 1, glm::value_ptr(mat.tint));
        // Shadow pass: blended foliage casts through an alpha test at 0.5.
        const float cutoff = pass == 1 && mat.blend == BlendMode::AlphaBlend ? 0.5f : alpha_test_cutoff(mat);
        glUniform1f(pbr_.alpha_cutoff, cutoff);
        glUniform1i(pbr_.unlit, mat.unlit ? 1 : 0);
        glUniform1i(pbr_.no_fog, mat.fog ? 0 : 1);
        glUniform1i(pbr_.has_layer2, mat.layer2 ? 1 : 0);
        glUniform1i(pbr_.has_lightmap, mat.lightmap ? 1 : 0);
        glUniform1i(pbr_.blend, int(mat.blend));
        glUniform3fv(pbr_.diffuse, 1, glm::value_ptr(mat.diffuse));
        glUniform1i(pbr_.vertex_diffuse, mat.vertex_diffuse ? 1 : 0);
        int lights = 0;
        if (!mat.unlit && pass == 0) {
            glm::vec4 pos_range[kMaxDrawLights];
            glm::vec3 color[kMaxDrawLights], att[kMaxDrawLights];
            for (int i = 0; i < std::min<int>(d.light_count, kMaxDrawLights); ++i) {
                if (d.lights[i] >= point_lights_.size()) continue;
                const PointLight& p = point_lights_[d.lights[i]];
                pos_range[lights] = glm::vec4(p.position, p.range);
                color[lights] = p.color;
                att[lights++] = p.attenuation;
            }
            if (lights) {
                glUniform4fv(pbr_.light_pos_range, lights, glm::value_ptr(pos_range[0]));
                glUniform3fv(pbr_.light_color, lights, glm::value_ptr(color[0]));
                glUniform3fv(pbr_.light_att, lights, glm::value_ptr(att[0]));
            }
        }
        glUniform1i(pbr_.light_count, lights);
        const bool bump = mat.bump && mat.bump_scale != 0.0f;
        glUniform1f(pbr_.bump_scale, bump ? mat.bump_scale : 0.0f);
        const bool water = mat.water >= 0.0f;
        glUniform1f(pbr_.water, water && gfx_.water ? mat.water : -1.0f);
        MaterialMaps maps;
        if (const auto mt = material_maps_.find(mat.albedo.id); mt != material_maps_.end() && (!water || gfx_.water)) maps = mt->second;
        if (is_terrain(mat)) maps.cls = MaterialClass::Terrain;
        glUniform1i(pbr_.cls, int(maps.cls));
        glUniform4i(pbr_.maps, maps.normal ? 1 : 0, maps.roughness ? 1 : 0, maps.metal ? 1 : 0, maps.ao ? 1 : 0);
        bind_unit(7, texture(maps.ao));
        bind_unit(6, texture(maps.metal));
        bind_unit(5, texture(maps.roughness));
        bind_unit(4, texture(maps.normal));
        if (bump) bind_unit(3, texture(mat.bump));
        bind_unit(2, texture(mat.lightmap));
        bind_unit(1, texture(mat.layer2));
        bind_unit(0, texture(mat.albedo));
        glBindVertexArray(m.vao);
        const std::uint32_t first = std::min(d.first_index, m.index_count);
        const std::uint32_t count = d.index_count ? std::min(d.index_count, m.index_count - first) : m.index_count - first;
        glDrawElements(GL_TRIANGLES, GLsizei(count), GL_UNSIGNED_INT, reinterpret_cast<void*>(std::size_t(first) * 4));
    }

    void fullscreen(GLuint program, GLuint fbo, glm::ivec2 size) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, size.x, size.y);
        glUseProgram(program);
        glBindVertexArray(fill_vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    void draw_scene_remaster(const FrameData& f) {
        const glm::ivec4 vp = f.view.viewport.z > 0 ? f.view.viewport : glm::ivec4(0, 0, target_size_.x, target_size_.y);
        const glm::ivec2 output(vp.z, vp.w), scene = scene_size(gfx_, output);
        ensure_remaster_programs();
        ensure_remaster_targets(scene, output);
        ensure_shadow_map();
        ensure_env(f.lighting);
        const Lighting& l = f.lighting;
        point_lights_ = l.point_lights;
        const RemasterFrame rf = build_remaster_frame(gfx_, f, scene, output, env_levels_, history_);
        previous_models(f.draws, motion_, prev_models_);
        draws_base_ = f.draws.data();
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof rf, &rf);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_);
        if (rf.waves.w > 0.0f) {  // interactive water wave field (FrameData::waves): unit 31 is only ever this texture;
            bind_unit(31, waves_);  // respecified whole (the driver orphans the copy an in-flight frame still reads)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, kWaterWaveGrid, kWaterWaveGrid, 0, GL_RED, GL_FLOAT, f.waves.height.data());
        }
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_FRAMEBUFFER_SRGB);  // float/UNORM targets; the final pass writes encoded bytes
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glUseProgram(pbr_.program);
        bind_unit(8, 0);  // the shadow map is written below; nothing samples it before the scene pass
        bind_unit(9, texture(env_radiance_));
        bind_unit(10, texture(env_irradiance_));

        // Sun shadow map: every lit opaque/alpha-tested draw except the terrain (its lightmap already holds
        // the baked terrain shading), slope-scaled bias + normal offset in the receiver.
        if (shadow_tex_) {
            glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo_);
            glViewport(0, 0, gfx_.shadow_map_size, gfx_.shadow_map_size);
            glClear(GL_DEPTH_BUFFER_BIT);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.5f, 2.0f);
            for (const DrawItem& d : f.draws)
                if (casts_sun_shadow(d.material)) draw_pbr(d, 1);
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
        bind_unit(8, shadow_tex_);

        // Opaque + alpha-tested into HDR colour + ambient, stencil as the original: the units' shadow volumes stay
        // (Material::volume_caster); the shadow map adds the world's shadows.
        const bool ms = rt_.samples > 1;
        glBindFramebuffer(GL_FRAMEBUFFER, ms ? rt_.fbo_opaque_ms : rt_.fbo_opaque);
        glViewport(0, 0, scene.x, scene.y);
        const glm::vec4 cc = l.clear_color, zero(0.0f);
        glClearBufferfv(GL_COLOR, 0, glm::value_ptr(cc));
        glClearBufferfv(GL_COLOR, 1, glm::value_ptr(zero));
        glClearBufferfv(GL_COLOR, 2, glm::value_ptr(zero));
        // Auto exposure reference: the background is the clear colour in both looks.
        const glm::vec4 reference_clear(glm::dot(glm::vec3(cc), glm::vec3(0.299f, 0.587f, 0.114f)), 0.0f, 0.0f, 0.0f);
        glClearBufferfv(GL_COLOR, 3, glm::value_ptr(reference_clear));
        glStencilMask(0xff);
        glClearStencil(0);
        glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        blended_.clear();
        volumes_.clear();
        glEnable(GL_STENCIL_TEST);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        int receive = -1;
        bool a2c_on = false;  // alpha-tested draws into the MSAA target: alpha-to-coverage
        for (const DrawItem& d : f.draws) {
            const BlendMode b = d.material.blend;
            if (b == BlendMode::ShadowVolume) volumes_.push_back(&d);
            else if (b == BlendMode::AlphaBlend || b == BlendMode::Additive) blended_.push_back(&d);
            else {
                if (int(d.material.receive_shadow) != receive)
                    receive = d.material.receive_shadow, glStencilFunc(GL_ALWAYS, receive ? 128 : 0, 0xff);
                if (const bool a2c = ms && b == BlendMode::AlphaTest; a2c != a2c_on) {
                    a2c_on = a2c;
                    if (a2c) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
                    else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
                    glUniform1i(pbr_.a2c, a2c ? 1 : 0);
                }
                draw_pbr(d, 0);
            }
        }
        if (a2c_on) {
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            glUniform1i(pbr_.a2c, 0);
        }
        if (!volumes_.empty() && l.shadow_color.a > 0.0f) {
            // Colour, ambient and reference only (the fill darkens them alike: shadow_fill_frag.glsl), never the motion target.
            const GLenum shaded[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_NONE, GL_COLOR_ATTACHMENT3};
            glDrawBuffers(4, shaded);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            glDepthMask(GL_FALSE);
            glStencilFunc(GL_ALWAYS, 0, 0xff);
            glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR);
            glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR);
            for (const DrawItem* d : volumes_) draw_pbr(*d, 0);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glStencilFunc(GL_LESS, 128, 0xff);
            glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUseProgram(fill_program_);
            glUniform4fv(fill_color_, 1, glm::value_ptr(shadow_fill_color(l.shadow_color)));
            glBindVertexArray(fill_vao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDisable(GL_BLEND);
            glEnable(GL_DEPTH_TEST);
            const GLenum all[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
            glDrawBuffers(4, all);
        }
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        if (ms) {  // SSAO and the AO apply read single-sample depth and ambient
            resolve(rt_.fbo_opaque_ms, -1, rt_.fbo_opaque, scene);
            resolve(rt_.fbo_opaque_ms, 1, rt_.fbo_ambient, scene);
        }

        if (gfx_.ssao) {
            bind_unit(13, rt_.depth);
            fullscreen(prog_ssao_, rt_.fbo_ao, scene);
            bind_unit(12, rt_.ambient);
            bind_unit(14, rt_.ao);
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glBlendFunc(GL_ONE, GL_ONE);
            fullscreen(prog_ao_apply_, ms ? rt_.fbo_hdr_ms : rt_.fbo_hdr, scene);
            glBlendEquation(GL_FUNC_ADD);
            glDisable(GL_BLEND);
            bind_unit(13, 0);
        }

        // Translucent back to front (same order as the Original path), depth writes only for depth_write blends. Stylised
        // water first: it is drawn opaque over a copy of the opaque scene (refraction, depth), every other blend over it.
        const glm::vec3 cam = f.view.camera_position;
        std::sort(blended_.begin(), blended_.end(), [&](const DrawItem* a, const DrawItem* b) {
            if (const bool wa = gfx_.water && a->material.water >= 0.0f; wa != (gfx_.water && b->material.water >= 0.0f)) return wa;
            const glm::vec3 pa(a->model[3]), pb(b->model[3]);
            return glm::dot(pa - cam, pa - cam) > glm::dot(pb - cam, pb - cam);
        });
        if (gfx_.water && !blended_.empty() && blended_.front()->material.water >= 0.0f) {
            resolve(ms ? rt_.fbo_hdr_ms : rt_.fbo_hdr, 0, rt_.fbo_water, scene);
            resolve(rt_.fbo_opaque, -1, rt_.fbo_water, scene);  // MSAA: rt_.depth was resolved after the opaque pass
            bind_unit(29, rt_.water_color);
            bind_unit(30, rt_.water_depth);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, ms ? rt_.fbo_trans_ms : rt_.fbo_trans);
        glViewport(0, 0, scene.x, scene.y);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glUseProgram(pbr_.program);
        // Depth-writing blends (alpha-tested hair, ..., and the stylised water: TAA reprojects and the volumetric fog measures
        // its surface, not the bed) also write their motion: o_motion (location 2) -> attachment 1; every draw blends its auto
        // exposure reference (location 3) -> attachment 2.
        const GLenum color_only[4] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT2},
                     with_motion[4] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
        int motion = -1;
        for (const DrawItem* d : blended_) {
            const bool zwrite = d->material.depth_write || (gfx_.water && d->material.water >= 0.0f);
            if (int(zwrite) != motion) motion = zwrite, glDrawBuffers(4, motion ? with_motion : color_only);
            glDepthMask(zwrite ? GL_TRUE : GL_FALSE);
            glBlendFunc(GL_SRC_ALPHA, d->material.blend == BlendMode::Additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
            draw_pbr(*d, 0);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        if (ms) {  // composite (colour, fog depth), TAA (motion) and the auto exposure (reference) read single-sample images
            glDrawBuffers(4, color_only);
            resolve(rt_.fbo_trans_ms, 0, rt_.fbo_hdr, scene);
            resolve(rt_.fbo_trans_ms, 1, rt_.fbo_velocity, scene);
            resolve(rt_.fbo_trans_ms, 2, rt_.fbo_reference, scene);
            resolve(rt_.fbo_trans_ms, -1, rt_.fbo_trans, scene);
        }

        // Auto exposure: the scene with its volumetric fog measured against the Original-look reference, the 1x1 state
        // adapted on the GPU (post_exposure_lum/adapt.glsl); the composite reads it.
        const int ew = int(history_.frame & 1u);
        bind_unit(11, rt_.hdr);
        bind_unit(13, rt_.depth);
        bind_unit(25, rt_.exposure[1 - ew]);
        bind_unit(28, rt_.reference);
        fullscreen(prog_exposure_[0], rt_.fbo_lum, {64, 32});
        bind_unit(26, rt_.lum);
        fullscreen(prog_exposure_[1], rt_.fbo_exposure[ew], {1, 1});
        bind_unit(27, rt_.exposure[ew]);

        // Post chain.
        fullscreen(prog_composite_, rt_.fbo_ldr, scene);
        GLuint src = rt_.ldr;
        if (gfx_.aa == AntiAliasing::Taa) {
            const int w = int(history_.frame & 1u), r = 1 - w;
            bind_unit(15, rt_.ldr);
            bind_unit(16, rt_.hist[r]);
            bind_unit(20, rt_.velocity);
            fullscreen(prog_taa_, rt_.fbo_hist[w], scene);
            src = rt_.hist[w];
        }
        for (int unit : {20, 25, 26, 27, 28}) bind_unit(unit, 0);
        finish_post(src, scene, output, vp);
        glEnable(GL_FRAMEBUFFER_SRGB);
    }

    // Units cast their original stencil volumes instead (Material::volume_caster); water casts nothing.
    static bool casts_sun_shadow(const Material& m) {
        return !m.unlit && !m.volume_caster && m.blend != BlendMode::ShadowVolume && m.blend != BlendMode::Additive && !is_terrain(m) &&
               m.water < 0.0f;
    }

    // Optional GPU timing of the scene (JOA_GPU_TIMING=1): timestamp pair around draw_scene*, read one frame late.
    bool timing_begin() {
        if (!timing_.enabled) return false;
        if (!timing_.queries[0]) glGenQueries(2, timing_.queries);
        else if (timing_.pending) {
            GLuint64 a = 0, b = 0;
            glGetQueryObjectui64v(timing_.queries[0], GL_QUERY_RESULT, &a);
            glGetQueryObjectui64v(timing_.queries[1], GL_QUERY_RESULT, &b);
            timing_.scene_ms += double(b - a) * 1e-6;
            ++timing_.frames;
        }
        glQueryCounter(timing_.queries[0], GL_TIMESTAMP);
        timing_.pending = true;
        return true;
    }

    SDL_Window* window_;
    SDL_GLContext context_ = nullptr;
    Uint64 frame_ns_ = 0, next_frame_ns_ = 0;  // Wayland frame limiter (0 = swap interval paces)
    MeshProgram mesh_{};
    UiProgram ui_{};
    std::unordered_map<std::uint32_t, Mesh> meshes_;
    std::unordered_map<std::uint32_t, GLuint> textures_;
    std::uint32_t next_id_ = 1;
    GLuint white_ = 0;
    GLuint fbo_ = 0, color_rb_ = 0, depth_rb_ = 0;
    glm::ivec2 target_size_{0, 0};
    GLuint ui_vao_ = 0, ui_vbo_ = 0;
    std::vector<const DrawItem*> blended_, volumes_;
    GLuint fill_program_ = 0, fill_vao_ = 0;
    GLint fill_color_ = -1;
    std::span<const PointLight> point_lights_;  // of the frame being drawn
    std::vector<UiVertex> ui_vertices_;

    GraphicsSettings gfx_{};
    bool remaster_ = false;
    std::unordered_map<std::uint32_t, MaterialMaps> material_maps_;  // by albedo texture id
    PbrProgram pbr_{};
    GLuint prog_ssao_ = 0, prog_ao_apply_ = 0, prog_composite_ = 0, prog_taa_ = 0, prog_easu_ = 0, prog_rcas_ = 0, ubo_ = 0;
    GLuint waves_ = 0;  // R16F kWaterWaveGrid^2 interactive water wave field (unit 31)
    GLuint prog_exposure_[2]{};  // post_exposure_lum, post_exposure_adapt
    GLuint prog_fxaa_ = 0, prog_smaa_[3]{};
    RemasterTargets rt_{};
    PostTargets pt_{};
    OriginalTarget ot_{};
    TextureHandle smaa_area_{}, smaa_search_{};
    GLuint shadow_tex_ = 0, shadow_fbo_ = 0;
    TextureHandle env_radiance_{}, env_irradiance_{};
    std::uint64_t env_key_ = 0;
    int env_levels_ = 1;
    RemasterHistory history_{};
    MotionHistory motion_;
    std::vector<glm::mat4> prev_models_;  // per FrameData::draws index, of the frame being drawn
    const DrawItem* draws_base_ = nullptr;
    std::uint64_t frame_ = 0;  // submits so far: Mesh::updated
    struct {
        bool enabled = std::getenv("JOA_GPU_TIMING") != nullptr;
        bool pending = false;
        GLuint queries[2]{};
        double scene_ms = 0;
        std::uint64_t frames = 0;
    } timing_;
};

}  // namespace

std::unique_ptr<IRenderer> create_gl_renderer(SDL_Window* window) { return std::make_unique<GlRenderer>(window); }

}  // namespace joa::engine
