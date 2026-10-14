#include "engine/renderer_gl.hpp"

#include "engine/gl_loader.hpp"
#include "engine/gl_shaders.hpp"

#include <SDL3/SDL_video.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
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

struct UiVertex {
    glm::vec2 position, uv;
    glm::vec4 color;
};

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
        if (!SDL_GL_SetSwapInterval(-1)) SDL_GL_SetSwapInterval(1);  // adaptive vsync (VRR friendly)
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
        mesh_.sun_dir = u("u_sun_dir");
        mesh_.sun_color = u("u_sun_color");
        mesh_.ambient = u("u_ambient");
        mesh_.fog_color = u("u_fog_color");
        mesh_.fog_range = u("u_fog_range");
        mesh_.camera_pos = u("u_camera_pos");
        mesh_.has_layer2 = u("u_has_layer2");
        mesh_.has_lightmap = u("u_has_lightmap");
        glUseProgram(mesh_.program);
        glUniform1i(u("u_albedo"), 0);
        glUniform1i(u("u_layer2"), 1);
        glUniform1i(u("u_lightmap"), 2);

        ui_.program = link(gl_shaders::ui_vert, gl_shaders::ui_frag);
        ui_.framebuffer = glGetUniformLocation(ui_.program, "u_framebuffer");
        glUseProgram(ui_.program);
        glUniform1i(glGetUniformLocation(ui_.program, "u_texture"), 0);

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
        glBindVertexArray(0);

        white_ = upload({1, 1, PixelFormat::Rgba8Unorm, {255, 255, 255, 255}, false, true});
    }

    ~GlRenderer() override {
        for (auto& [id, m] : meshes_) release(m);
        for (auto& [id, t] : textures_) glDeleteTextures(1, &t);
        glDeleteTextures(1, &white_);
        destroy_target();
        glDeleteBuffers(1, &ui_vbo_);
        glDeleteVertexArrays(1, &ui_vao_);
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
        glBindVertexArray(it->second.vao);  // its element binding is the mesh ibo
        glBindBuffer(GL_ARRAY_BUFFER, it->second.vbo);
        fill(it->second, data);
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
        draw_scene(f);
        draw_ui(f.ui);
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
        std::uint32_t index_count = 0;
    };
    struct MeshProgram {
        GLuint program = 0;
        GLint model, view_proj, normal_matrix, tint, alpha_cutoff, unlit, sun_dir, sun_color, ambient, fog_color, fog_range, camera_pos,
            has_layer2, has_lightmap;
    };
    struct UiProgram {
        GLuint program = 0;
        GLint framebuffer;
    };

    static void release(Mesh& m) {
        glDeleteBuffers(1, &m.vbo);
        glDeleteBuffers(1, &m.ibo);
        glDeleteVertexArrays(1, &m.vao);
    }

    // Uploads into the currently bound GL_ARRAY_BUFFER / element buffer (of the bound VAO).
    static void fill(Mesh& m, const MeshData& data) {
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(data.vertices.size() * sizeof(Vertex)), data.vertices.data(), GL_STATIC_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(data.indices.size() * sizeof(std::uint32_t)), data.indices.data(), GL_STATIC_DRAW);
        m.index_count = std::uint32_t(data.indices.size());
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
        if (t.mipmaps) {
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY; ignored (GL error) if unsupported */, 8);
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

    void draw_scene(const FrameData& f) {
        const glm::ivec4 vp = f.view.viewport.z > 0 ? f.view.viewport : glm::ivec4(0, 0, target_size_.x, target_size_.y);
        const GLint y = target_size_.y - vp.y - vp.w;  // top-left origin -> GL bottom-left
        glViewport(vp.x, y, vp.z, vp.w);
        glScissor(vp.x, y, vp.z, vp.w);
        glEnable(GL_SCISSOR_TEST);
        const glm::vec4& cc = f.lighting.clear_color;
        glClearColor(cc.r, cc.g, cc.b, cc.a);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        const Lighting& l = f.lighting;
        glUseProgram(mesh_.program);
        glUniformMatrix4fv(mesh_.view_proj, 1, GL_FALSE, glm::value_ptr(f.view.projection * f.view.view));
        glUniform3fv(mesh_.sun_dir, 1, glm::value_ptr(glm::normalize(l.sun_direction)));
        glUniform3fv(mesh_.sun_color, 1, glm::value_ptr(l.sun_color));
        glUniform3fv(mesh_.ambient, 1, glm::value_ptr(l.ambient));
        glUniform3fv(mesh_.fog_color, 1, glm::value_ptr(l.fog_color));
        glUniform2f(mesh_.fog_range, l.fog_start, l.fog_end);
        glUniform3fv(mesh_.camera_pos, 1, glm::value_ptr(f.view.camera_position));
        glActiveTexture(GL_TEXTURE0);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);

        // Opaque + alpha-tested first, then blended back to front without depth writes.
        blended_.clear();
        for (const DrawItem& d : f.draws) {
            if (d.material.blend == BlendMode::AlphaBlend || d.material.blend == BlendMode::Additive)
                blended_.push_back(&d);
            else
                draw_item(d);
        }
        const glm::vec3 cam = f.view.camera_position;
        std::sort(blended_.begin(), blended_.end(), [&](const DrawItem* a, const DrawItem* b) {
            const glm::vec3 pa(a->model[3]), pb(b->model[3]);
            return glm::dot(pa - cam, pa - cam) > glm::dot(pb - cam, pb - cam);
        });
        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);
        for (const DrawItem* d : blended_) {
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
        glUniform1f(mesh_.alpha_cutoff, mat.blend == BlendMode::AlphaTest ? mat.alpha_cutoff : -1.0f);
        glUniform1i(mesh_.unlit, mat.unlit ? 1 : 0);
        glUniform1i(mesh_.has_layer2, mat.layer2 ? 1 : 0);
        glUniform1i(mesh_.has_lightmap, mat.lightmap ? 1 : 0);
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

    void draw_ui(std::span<const UiQuad> quads) {
        if (quads.empty()) return;
        ui_vertices_.clear();
        for (const UiQuad& q : quads) {
            const glm::vec2 p0 = q.position, p1 = q.position + q.size;
            const UiVertex v[4] = {{p0, q.uv0, q.color}, {{p1.x, p0.y}, {q.uv1.x, q.uv0.y}, q.color},
                                   {p1, q.uv1, q.color}, {{p0.x, p1.y}, {q.uv0.x, q.uv1.y}, q.color}};
            ui_vertices_.insert(ui_vertices_.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
        }
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
        // Batch runs of quads sharing a texture.
        std::size_t start = 0;
        for (std::size_t i = 1; i <= quads.size(); ++i) {
            if (i < quads.size() && quads[i].texture.id == quads[start].texture.id) continue;
            glBindTexture(GL_TEXTURE_2D, texture(quads[start].texture));
            glDrawArrays(GL_TRIANGLES, GLint(start * 6), GLsizei((i - start) * 6));
            start = i;
        }
        glDisable(GL_BLEND);
        glBindVertexArray(0);
    }

    SDL_Window* window_;
    SDL_GLContext context_ = nullptr;
    MeshProgram mesh_{};
    UiProgram ui_{};
    std::unordered_map<std::uint32_t, Mesh> meshes_;
    std::unordered_map<std::uint32_t, GLuint> textures_;
    std::uint32_t next_id_ = 1;
    GLuint white_ = 0;
    GLuint fbo_ = 0, color_rb_ = 0, depth_rb_ = 0;
    glm::ivec2 target_size_{0, 0};
    GLuint ui_vao_ = 0, ui_vbo_ = 0;
    std::vector<const DrawItem*> blended_;
    std::vector<UiVertex> ui_vertices_;
};

}  // namespace

std::unique_ptr<IRenderer> create_gl_renderer(SDL_Window* window) { return std::make_unique<GlRenderer>(window); }

}  // namespace joa::engine
