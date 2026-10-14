#include "engine/water.hpp"

#include "engine/world_scene.hpp"
#include "formats/res_archive.hpp"
#include "formats/save_sav.hpp"  // parse_bmp_header
#include "formats/world_text.hpp"

#include <glm/gtc/color_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace joa::engine {
namespace {

// texteff.res (Resource\, Name34 container of "Key : value" materials, parser FUN_004ded90). Only the
// fields the water path reads. Layer order = D3D texture stage.
struct TexLayer {
    std::string texture;     // LayerTextureName
    int textures = 0;        // LayerNumTexture: > 1 = animated <name>1..<name>N
    float frame_ms = 0;      // LayerSecondsPerFrame: milliseconds per frame (compared with the ms tick 0xa500ba)
    bool bump = false;       // IsBump: D3DTOP_BUMPENVMAP source
    float m00 = 0, m01 = 0, m10 = 0, m11 = 0;  // BumpEnvMat
    bool cube = false;       // IsCubeMap: camera-space reflection vector texgen
};

std::vector<TexLayer> find_texteff(const formats::Vfs& vfs, std::string_view model) {
    const auto path = vfs.resolve("Resource/texteff.res");
    const auto arc = path ? formats::ResArchive::open(*path) : std::nullopt;
    if (!arc) return {};
    for (const formats::ResEntry& e : arc->entries()) {
        const auto bytes = arc->data(e);
        std::vector<TexLayer> layers;
        bool match = false;
        for (const auto& kv : formats::parse_kv_text(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
            const std::string k = lower(kv.key);
            if (k == "modelname") match = lower(kv.value) == lower(model);
            if (!match) continue;
            if (k == "alphaarg1") layers.emplace_back();  // first key of every layer
            if (layers.empty()) continue;
            TexLayer& l = layers.back();
            const float v = std::strtof(kv.value.c_str(), nullptr);
            if (k == "layertexturename") l.texture = kv.value;
            else if (k == "layernumtexture") l.textures = int(v);
            else if (k == "layersecondsperframe") l.frame_ms = v;
            else if (k == "isbump") l.bump = lower(kv.value) == "true";
            else if (k == "bumpenvmat00") l.m00 = v;
            else if (k == "bumpenvmat01") l.m01 = v;
            else if (k == "bumpenvmat10") l.m10 = v;
            else if (k == "bumpenvmat11") l.m11 = v;
            else if (k == "iscubemap") l.cube = lower(kv.value) == "true";
        }
        if (match) return layers;
    }
    return {};
}

// ponytail: BMP/TGA water textures are read from TEXTURE\MISC\ only (where every shipped one lives).
// 8-bit palettised BMP (Lake*.bmp height maps): palette R, G per texel, D3DX-style.
std::optional<TextureData> read_bmp8(const formats::Vfs& vfs, const std::string& path) {
    const auto b = vfs.read(path);
    const auto info = b ? formats::parse_bmp_header(*b) : std::nullopt;
    if (!info || info->bpp != 8 || info->width <= 0 || info->height == 0 || b->size() < 54 + 1024) return std::nullopt;
    const int w = info->width, h = std::abs(info->height);
    const std::size_t row = (std::size_t(w) + 3) & ~std::size_t(3);
    if (info->pixel_offset + row * h > b->size()) return std::nullopt;
    const std::uint8_t* pal = b->data() + 14 + (*b)[14];  // after BITMAPINFOHEADER (biSize)
    TextureData t;
    t.width = w, t.height = h, t.format = PixelFormat::Rgba8Unorm, t.pixels.resize(std::size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* src = b->data() + info->pixel_offset + row * std::size_t(info->height > 0 ? h - 1 - y : y);
        for (int x = 0; x < w; ++x) {
            std::uint8_t* d = &t.pixels[(std::size_t(y) * w + x) * 4];
            const std::uint8_t* p = pal + src[x] * 4;  // B G R x
            d[0] = p[2], d[1] = p[1], d[2] = p[0], d[3] = 255;
        }
    }
    return t;
}

// Uncompressed true-colour TGA (type 2, 24/32 bpp): waterpool.tga.
std::optional<TextureData> read_tga(const formats::Vfs& vfs, const std::string& path) {
    const auto b = vfs.read(path);
    if (!b || b->size() < 18 || (*b)[2] != 2) return std::nullopt;
    const int w = (*b)[12] | (*b)[13] << 8, h = (*b)[14] | (*b)[15] << 8, bpp = (*b)[16] / 8;
    const std::size_t off = 18 + (*b)[0];
    if ((bpp != 3 && bpp != 4) || w <= 0 || h <= 0 || off + std::size_t(w) * h * bpp > b->size()) return std::nullopt;
    const bool top_down = ((*b)[17] & 0x20) != 0;
    TextureData t;
    t.width = w, t.height = h, t.pixels.resize(std::size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* s = b->data() + off + (std::size_t(top_down ? y : h - 1 - y) * w + x) * bpp;
            std::uint8_t* d = &t.pixels[(std::size_t(y) * w + x) * 4];
            d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = bpp == 4 ? s[3] : 255;
        }
    return t;
}

}  // namespace

WaterField::WaterField(AssetCache& assets, std::string_view scenario, glm::vec2 origin) : assets_(assets) {
    const auto& vfs = assets.vfs();
    const auto paths = find_scenario(vfs, scenario);
    const auto bytes = paths ? vfs.read(paths->dir + "/" + paths->name + ".stg") : std::nullopt;
    const auto stg = bytes ? formats::parse_block_text(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()))
                           : std::nullopt;
    if (!stg) throw std::runtime_error("water: .stg missing or malformed for " + std::string(scenario));
    IRenderer& r = assets.renderer();
    for (const auto& b : stg->blocks) {
        if (b.type != "CreateWater") continue;
        // Field order (FUN_004f5210): scene_name, WaterName, WaterSoundCode, X, Y, Z, TextureDirection,
        // TextureWidth (default 150), Reflection, WaterDirection, Ripple, WIDTH, LENGTH, ParticleEffect1/2.
        // X, Y = centre (CONFIRMED: box X -/+ WIDTH/2, Y -/+ LENGTH/2 at Z).
        const std::string name = b.get("WaterName") ? *b.get("WaterName") : std::string{};
        const std::string reflection = b.get("Reflection") ? *b.get("Reflection") : std::string{};
        const float w = float(b.number("WIDTH")), len = float(b.number("LENGTH"));
        const float tw = std::max(float(b.number("TextureWidth", 150.0)), 1.0f);
        const int dir = int(b.number("TextureDirection"));
        Surface s;
        s.centre = {float(b.number("X")) + origin.x, float(b.number("Y")) + origin.y, float(b.number("Z"))};

        // Material (FUN_004f5210 with config+0x9d = 1, +0x9e = 1): WaterName "ice" -> icelake + texteff
        // "ice"; a Reflection texture + a texteff material named WaterName -> that material with the
        // Reflection texture in stage 1; otherwise waterpool + texteff NormalWater (texture x diffuse).
        Material& m = s.draw.material;
        m.blend = BlendMode::AlphaBlend;
        m.two_sided = true;
        float alpha = 128.0f / 255.0f;  // vertex diffuse alpha 0x80
        const std::vector<TexLayer> fx = reflection.empty() ? std::vector<TexLayer>{} : find_texteff(vfs, name);
        const auto bump = std::find_if(fx.begin(), fx.end(), [](const TexLayer& l) { return l.bump; });
        const TextureInfo* env = reflection.empty() ? nullptr : assets.texture(reflection);
        if (lower(name) == "ice") {
            if (const TextureInfo* t = assets.texture("icelake")) m.albedo = t->handle;
            alpha = 1.0f;  // texteff "ice": AlphaOp arg1 = texture alpha
        } else if (bump != fx.end() && env) {
            // texteff (e.g. BumpEnvWaterRiver): stage 0 D3DTOP_BUMPENVMAP of the animated height map at the
            // grid uv; stage 1 = Reflection texture x diffuse at the reflection texgen (build()) offset by
            // BUMPENVMAT * (du, dv). du = R, dv = G of an unsigned texture (0..1, D3D for non-signed formats).
            s.embm = true;
            m.albedo = env->handle;
            const float a = bump->m00 + bump->m10, c = bump->m01 + bump->m11;  // du = dv = height
            m.bump_scale = std::max({a, c, 1e-6f});
            // ponytail: one height-map animation per map (the first EMBM material's, baked with its matrix);
            // keyed per texteff material if a map ever mixes e.g. BumpEnvWaterRiver and BumpEnvWaterSea.
            if (bump_frames_.empty()) {
                frame_ms_ = std::max(bump->frame_ms, 1.0f);
                const int n = std::max(bump->textures, 1);
                for (int i = 1; i <= n; ++i) {
                    auto t = read_bmp8(vfs, "texture/misc/" + bump->texture + (n > 1 ? std::to_string(i) : std::string{}) + ".bmp");
                    if (!t) continue;
                    for (std::size_t p = 0; p < t->pixels.size(); p += 4) {
                        const float h = t->pixels[p];
                        t->pixels[p] = std::uint8_t(std::lround(h * a / m.bump_scale));
                        t->pixels[p + 1] = std::uint8_t(std::lround(h * c / m.bump_scale));
                    }
                    bump_frames_.push_back(r.create_texture(*t));
                }
            }
            if (bump_frames_.empty()) s.embm = false, m.bump_scale = 0;
            else m.bump = bump_frames_[0];
        }
        if (!m.albedo) {
            if (!pool_)
                if (const auto t = read_tga(vfs, "texture/misc/waterpool.tga")) pool_ = r.create_texture(*t);
            m.albedo = pool_;
        }

        // Vertex grid (CONFIRMED): cell 800, or min(WIDTH, LENGTH) / 3 when either is below 800;
        // int(W) / cell + 2 columns, evenly spaced. Normal +Z, diffuse RGB 65,51,80 (BumpEnvWaterSea:
        // 111,200,213), A 0x80. Stage-0 uv: (x - xmin, y - ymin) / TextureWidth, rotated by TextureDirection.
        const int cell = std::max(w < 800.0f || len < 800.0f ? int(std::min(w, len) / 3.0f) : 800, 1);
        const int nx = int(w) / cell, ny = int(len) / cell, cols = nx + 2, rows = ny + 2;
        const glm::vec3 rgb = lower(name) == "bumpenvwatersea" ? glm::vec3(111, 200, 213) : glm::vec3(65, 51, 80);
        const glm::vec4 color(glm::convertSRGBToLinear(rgb / 255.0f), alpha);
        const float ws = w / tw, ls = len / tw;
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < cols; ++i) {
                const float lx = float(i) * w / float(nx + 1), ly = float(j) * len / float(ny + 1);
                const float u = lx / tw, v = ly / tw;
                Vertex vx;
                vx.position = to_engine(lx - w * 0.5f, ly - len * 0.5f, 0.0f);
                vx.color = color;
                const glm::vec2 uv = dir == 90 ? glm::vec2(ls - v, u) : dir == 180 ? glm::vec2(ws - u, ls - v) : dir == 270 ? glm::vec2(v, ws - u) : glm::vec2(u, v);
                (s.embm ? vx.uv2 : vx.uv) = uv;
                s.mesh.vertices.push_back(vx);
            }
        for (int j = 0; j + 1 < rows; ++j)
            for (int i = 0; i + 1 < cols; ++i) {
                const auto a = std::uint32_t(j * cols + i), c = a + std::uint32_t(cols);
                s.mesh.indices.insert(s.mesh.indices.end(), {a, a + 1, c + 1, a, c + 1, c});
            }
        s.handle = r.create_mesh(s.mesh);
        s.draw.mesh = s.handle;
        s.draw.model = glm::translate(glm::mat4(1.0f), to_engine(s.centre.x, s.centre.y, s.centre.z));
        surfaces_.push_back(std::move(s));
    }
}

WaterField::~WaterField() {
    IRenderer& r = assets_.renderer();
    for (const Surface& s : surfaces_) r.destroy_mesh(s.handle);
    for (TextureHandle t : bump_frames_) r.destroy_texture(t);
    r.destroy_texture(pool_);
}

int WaterField::bump_count() const {
    return int(std::count_if(surfaces_.begin(), surfaces_.end(), [](const Surface& s) { return s.embm; }));
}

void WaterField::build(float time_s, glm::vec3 eye, std::vector<DrawItem>& draws) {
    IRenderer& r = assets_.renderer();
    // Height-map frame: the next one once LayerSecondsPerFrame ms have passed (Lake1..Lake65 loop).
    const TextureHandle frame = bump_frames_.empty() ? TextureHandle{}
                                                     : bump_frames_[std::size_t(time_s * 1000.0f / frame_ms_) % bump_frames_.size()];
    const glm::vec3 eye_o(eye.x, -eye.z, eye.y);  // engine -> original
    for (Surface& s : surfaces_) {
        if (s.embm) {
            // IsCubeMap layer: TCI_CAMERASPACEREFLECTIONVECTOR with D3DRS_LOCALVIEWER, texture transform =
            // inverse view rotation x 0xa17e18 (identity, row 3 = 0.5): uv = world R.xy + 0.5 with
            // R = 2(E.N)N - E, N = +Z, E = vertex -> eye. Per vertex, as the fixed-function texgen.
            for (Vertex& v : s.mesh.vertices) {
                const glm::vec3 p = s.centre + glm::vec3(v.position.x, -v.position.z, v.position.y);
                const glm::vec3 e = glm::normalize(eye_o - p);
                v.uv = {0.5f - e.x, 0.5f - e.y};
            }
            if (!r.update_mesh(s.handle, s.mesh)) {
                r.destroy_mesh(s.handle);
                s.draw.mesh = s.handle = r.create_mesh(s.mesh);
            }
            s.draw.material.bump = frame;
        }
        draws.push_back(s.draw);
    }
}

}  // namespace joa::engine
