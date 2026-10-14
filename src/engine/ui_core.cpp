#include "engine/ui_core.hpp"

#include "engine/assets_cache.hpp"  // lower()
#include "formats/dds.hpp"
#include "formats/save_sav.hpp"  // parse_bmp_header
#include "formats/res_archive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace joa::engine {
namespace {


float srgb_to_linear(int v) {
    const float c = float(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::optional<formats::DdsImage> load_dds(const formats::Vfs& vfs, std::string_view name) {
    const auto bytes = vfs.read("int8x6/" + std::string(name) + ".dds");
    if (!bytes) return std::nullopt;
    formats::DdsTexture t;
    std::string err;
    if (!formats::parse_dds(*bytes, t, err, 1) || t.images.empty()) {
        std::fprintf(stderr, "ui: %.*s.dds: %s\n", int(name.size()), name.data(), err.c_str());
        return std::nullopt;
    }
    return std::move(t.images[0]);
}

TextureHandle upload(IRenderer& r, int w, int h, std::vector<std::uint8_t> rgba) {
    TextureData t;
    t.width = w;
    t.height = h;
    t.pixels = std::move(rgba);
    t.mipmaps = false;
    t.repeat = false;
    return r.create_texture(t);
}

}  // namespace

glm::vec4 ui_rgba(int r, int g, int b, int a) { return {srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b), float(a) / 255.0f}; }

Ui::Ui(const formats::Vfs& vfs, IRenderer& renderer) : vfs_(vfs), renderer_(renderer) {}

Ui::~Ui() {
    for (auto& [name, t] : textures_)
        if (t.first) renderer_.destroy_texture(t.first);
    for (auto& [name, f] : fonts_)
        if (f) {
            if (f->texture) renderer_.destroy_texture(f->texture);
            if (f->mask) renderer_.destroy_texture(f->mask);
        }
    for (auto& [tex, hot] : cursors_)
        if (tex) renderer_.destroy_texture(tex);
    for (auto& [key, s] : sprites_)
        if (s && s->rotated && s->texture) renderer_.destroy_texture(s->texture);
}

// ---------------------------------------------------------------- data

TextureHandle Ui::texture(std::string_view name) {
    const std::string key = lower(name);
    if (auto it = textures_.find(key); it != textures_.end()) return it->second.first;
    std::pair<TextureHandle, glm::ivec2> entry{};
    if (auto img = load_dds(vfs_, name)) {
        entry.second = {int(img->width), int(img->height)};
        entry.first = upload(renderer_, int(img->width), int(img->height), std::move(img->rgba));
    } else {
        std::fprintf(stderr, "ui: missing texture int8x6/%s.dds\n", key.c_str());
    }
    return textures_.emplace(key, entry).first->second.first;
}

glm::ivec2 Ui::texture_size(std::string_view name) {
    texture(name);
    return textures_[lower(name)].second;
}

TextureHandle Ui::bitmap(std::string_view game_path, bool reload) {
    const std::string key = "bmp:" + lower(game_path);
    if (auto it = textures_.find(key); it != textures_.end()) {
        if (!reload) return it->second.first;
        if (it->second.first) renderer_.destroy_texture(it->second.first);
        textures_.erase(it);
    }
    std::pair<TextureHandle, glm::ivec2> entry{};
    const auto bytes = vfs_.read(game_path);
    const auto info = bytes ? formats::parse_bmp_header(*bytes) : std::nullopt;
    if (info && (info->bpp == 24 || info->bpp == 32) && info->width > 0 && info->height != 0) {
        const int w = info->width, h = std::abs(info->height), bpp = info->bpp / 8;
        const std::size_t row = (std::size_t(w) * bpp + 3) & ~std::size_t(3);
        if (info->pixel_offset + row * h <= bytes->size()) {
            std::vector<std::uint8_t> px(std::size_t(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                const std::uint8_t* src = bytes->data() + info->pixel_offset + row * std::size_t(info->height > 0 ? h - 1 - y : y);
                for (int x = 0; x < w; ++x) {
                    std::uint8_t* d = &px[(std::size_t(y) * w + x) * 4];
                    d[0] = src[x * bpp + 2], d[1] = src[x * bpp + 1], d[2] = src[x * bpp], d[3] = 255;
                }
            }
            entry = {upload(renderer_, w, h, std::move(px)), {w, h}};
        }
    }
    return textures_.emplace(key, entry).first->second.first;
}

void Ui::load_sprites() {
    sprites_loaded_ = true;
    const auto path = vfs_.resolve("Resource/INTERFAC.SET");
    std::string err;
    const auto arc = path ? formats::ResArchive::open(*path, &err) : std::nullopt;
    if (!arc) {
        std::fprintf(stderr, "ui: Resource/INTERFAC.SET: %s\n", path ? err.c_str() : "missing");
        return;
    }
    for (const formats::ResEntry& e : arc->entries()) {
        const auto table = formats::DbfTable::parse(arc->data(e), &err);
        if (!table) continue;
        const int f_bitmap = table->field_index("BITMAP"), f_tex = table->field_index("TEXTURE");
        const int f_x1 = table->field_index("SRC_X1"), f_y1 = table->field_index("SRC_Y1");
        const int f_x2 = table->field_index("SRC_X2"), f_y2 = table->field_index("SRC_Y2");
        const int f_rot = table->field_index("ROTATION"), f_w = table->field_index("WIDTH"), f_h = table->field_index("HEIGHT");
        if (f_bitmap < 0 || f_tex < 0 || f_x1 < 0 || f_h < 0) continue;  // MINIINFO / HEADER: not sprite tables
        for (std::size_t r = 0; r < table->record_count(); ++r) {
            if (table->deleted(r)) continue;
            auto num = [&](int f) { return std::atoi(table->text(r, std::size_t(f)).c_str()); };
            SpriteRecord s;
            s.texture = table->text(r, std::size_t(f_tex));
            s.src = {num(f_x1), num(f_y1), num(f_x2), num(f_y2)};
            s.size = {float(num(f_w)), float(num(f_h))};
            s.rotated = f_rot >= 0 && num(f_rot) != 0;
            sprite_records_.emplace(lower(e.name) + "/" + lower(table->text(r, std::size_t(f_bitmap))), std::move(s));
        }
    }
}

const UiSprite* Ui::sprite(std::string_view set, std::string_view name) {
    if (!sprites_loaded_) load_sprites();
    const std::string key = lower(set) + "/" + lower(name);
    if (auto it = sprites_.find(key); it != sprites_.end()) return it->second ? &*it->second : nullptr;
    // Resolve on first use: only the atlases a screen draws get decoded.
    std::optional<UiSprite> out;
    if (auto rec = sprite_records_.find(key); rec != sprite_records_.end()) {
        const SpriteRecord& r = rec->second;
        UiSprite s;
        s.src = r.src;
        s.size = r.size;
        s.rotated = r.rotated;
        const glm::ivec2 ts = texture_size(r.texture);
        if (ts.x > 0 && !s.rotated) {
            s.texture = texture(r.texture);
            // Inclusive texel rect -> edge UVs.
            s.uv0 = {float(s.src.x) / ts.x, float(s.src.y) / ts.y};
            s.uv1 = {float(s.src.z + 1) / ts.x, float(s.src.w + 1) / ts.y};
            out = s;
        } else if (auto img = ts.x > 0 ? load_dds(vfs_, r.texture) : std::nullopt) {
            // ROTATION = 1: stored rotated; bake a WIDTH x HEIGHT texture turned 90 degrees counter-clockwise
            // (texel left edge -> bottom; verified on COMMELE2 frame vs s04_07_ingame_menu by MenuSaveLoad).
            // ponytail: one baked texture per rotated record (OPTMENU remap_fr, COMMELE2 frame, GPOPUP frame_l/r).
            const int sw = s.src.z - s.src.x + 1, sh = s.src.w - s.src.y + 1;
            std::vector<std::uint8_t> px(std::size_t(sw) * sh * 4);
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x) {
                    const int dx = y, dy = sw - 1 - x;  // counter-clockwise
                    const int sx = std::min(s.src.x + x, int(img->width) - 1), sy = std::min(s.src.y + y, int(img->height) - 1);
                    std::copy_n(&img->rgba[(std::size_t(sy) * img->width + sx) * 4], 4, &px[(std::size_t(dy) * sh + dx) * 4]);
                }
            s.texture = upload(renderer_, sh, sw, std::move(px));
            out = s;
        }
    }
    if (!out) std::fprintf(stderr, "ui: missing sprite %s\n", key.c_str());
    auto& slot = sprites_[key];
    slot = out;
    return slot ? &*slot : nullptr;
}

const UiFont* Ui::font(std::string_view name) {
    const std::string key = lower(name);
    if (auto it = fonts_.find(key); it != fonts_.end()) return it->second.get();
    std::unique_ptr<UiFont> f;
    if (auto img = load_dds(vfs_, name)) {
        const int w = int(img->width), h = int(img->height);
        std::optional<UiFontMetrics> m;
        if (const auto fin_bytes = vfs_.read("FONT/" + std::string(name) + ".fin")) {
            if (auto fin = formats::parse_fin(*fin_bytes)) m = make_fin_font(std::move(*fin), w, h);
        } else {
            for (const GridFontSpec& spec : grid_font_specs())
                if (lower(spec.name) == key) m = make_grid_font(spec, img->rgba, w, h);
        }
        if (m) {
            f = std::make_unique<UiFont>();
            f->metrics = std::move(*m);
            std::vector<std::uint8_t> white = img->rgba;
            for (std::size_t i = 0; i + 3 < white.size(); i += 4) white[i] = white[i + 1] = white[i + 2] = 255;
            f->mask = upload(renderer_, w, h, std::move(white));
            f->texture = upload(renderer_, w, h, std::move(img->rgba));
        }
    }
    if (!f) std::fprintf(stderr, "ui: cannot load font %s\n", key.c_str());
    return fonts_.emplace(key, std::move(f)).first->second.get();
}

void Ui::load_text() {
    text_loaded_ = true;
    for (const char* file : {"Resource/t_interf.txr", "Resource/t_basic.txr"}) {
        const auto bytes = vfs_.read(file);
        if (!bytes) continue;
        const std::string_view all(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        std::size_t p = 0;
        while (p < all.size()) {
            std::size_t e = all.find('\n', p);
            if (e == std::string_view::npos) e = all.size();
            std::string_view line = all.substr(p, e - p);
            p = e + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            // "id|SECTION|n|text"
            const std::size_t a = line.find('|'), b = line.find('|', a + 1), c = line.find('|', b + 1);
            if (a == 0 || a == std::string_view::npos || c == std::string_view::npos) continue;
            if (!std::all_of(line.begin(), line.begin() + long(a), [](char ch) { return ch >= '0' && ch <= '9'; })) continue;
            const std::string key = std::string(line.substr(a + 1, b - a - 1)) + "|" + std::string(line.substr(b + 1, c - b - 1));
            text_.emplace(key, std::string(line.substr(c + 1)));  // first file wins
        }
    }
}

std::string_view Ui::text(std::string_view section, int index) {
    if (!text_loaded_) load_text();
    auto it = text_.find(std::string(section) + "|" + std::to_string(index));
    return it == text_.end() ? std::string_view{} : std::string_view(it->second);
}

// ---------------------------------------------------------------- frame / transform

void Ui::begin(glm::ivec4 viewport, std::vector<UiQuad>& out, const UiInput& input, float dt) {
    out_ = &out;
    layout_ = ui_layout(viewport);
    input_ = input;
    dt_ = dt;
    frame_ = Anchor::Center;
}

void Ui::frame(Anchor anchor) { frame_ = anchor; }

glm::vec2 Ui::to_pixels(glm::vec2 d) const {
    const int a = int(frame_);
    const float s = layout_.scale;
    auto axis = [&](int mode, float v, float origin, float size, float safe_origin, float canvas) {
        return mode == 0 ? origin + v * s : mode == 1 ? safe_origin + v * s : origin + size - (canvas - v) * s;
    };
    return {axis(a % 3, d.x, layout_.origin.x, layout_.size.x, layout_.safe_origin.x, 1024.0f),
            axis(a / 3, d.y, layout_.origin.y, layout_.size.y, layout_.safe_origin.y, 768.0f)};
}

glm::vec2 Ui::to_design(glm::vec2 p) const {
    const glm::vec2 zero = to_pixels({0.0f, 0.0f});
    const float s = layout_.scale > 0.0f ? layout_.scale : 1.0f;
    return (p - zero) / s;
}

glm::vec4 Ui::rect_pixels(glm::vec4 r) const {
    const glm::vec2 p0 = glm::round(to_pixels({r.x, r.y})), p1 = glm::round(to_pixels({r.x + r.z, r.y + r.w}));
    return {p0.x, p0.y, p1.x - p0.x, p1.y - p0.y};
}

// ---------------------------------------------------------------- drawing

void Ui::image(TextureHandle tex, glm::vec4 rect, glm::vec2 uv0, glm::vec2 uv1, glm::vec4 color) {
    if (!out_ || rect.z <= 0.0f || rect.w <= 0.0f) return;
    if (frame_ == Anchor::Center) {
        // Off-canvas design space is off-screen in the original (4:3 modes only): clip to 1024x768 so slide-ins
        // never show in the pillar/letterbox bars; UVs follow proportionally.
        const glm::vec2 p0{rect.x, rect.y}, p1 = p0 + glm::vec2(rect.z, rect.w);
        const glm::vec2 c0 = glm::max(p0, glm::vec2(0.0f)), c1 = glm::min(p1, glm::vec2(1024.0f, 768.0f));
        if (c1.x <= c0.x || c1.y <= c0.y) return;
        const glm::vec2 duv = (uv1 - uv0) / (p1 - p0);
        uv1 = uv0 + (c1 - p0) * duv;
        uv0 = uv0 + (c0 - p0) * duv;
        rect = {c0.x, c0.y, c1.x - c0.x, c1.y - c0.y};
    }
    const glm::vec4 px = rect_pixels(rect);
    out_->push_back({{px.x, px.y}, {px.z, px.w}, uv0, uv1, color, tex});
}

void Ui::fill(glm::vec4 rect, glm::vec4 color) { image({}, rect, {0, 0}, {1, 1}, color); }

void Ui::sprite(const UiSprite* s, glm::vec2 pos, glm::vec4 color) {
    if (s) image(s->texture, {pos.x, pos.y, s->size.x, s->size.y}, s->uv0, s->uv1, color);
}

void Ui::sprite(const UiSprite* s, glm::vec4 rect, glm::vec4 color) {
    if (s) image(s->texture, rect, s->uv0, s->uv1, color);
}

void Ui::sprite(std::string_view set, std::string_view name, glm::vec2 pos, glm::vec4 color) { sprite(sprite(set, name), pos, color); }

bool Ui::has_sprite(std::string_view set, std::string_view name) {
    if (!sprites_loaded_) load_sprites();
    return sprite_records_.count(lower(set) + "/" + lower(name)) != 0;
}

int Ui::text(const UiFont* f, std::string_view s, glm::vec2 pos, glm::vec4 color, TextAlign align, float scale, TextOp op) {
    if (!f) return 0;
    glyph_scratch_.clear();
    const int w = layout_line(f->metrics, s, glyph_scratch_);
    float x = pos.x;
    if (align == TextAlign::Center) x -= std::floor(float(w) * scale / 2.0f);
    if (align == TextAlign::Right) x -= float(w) * scale;
    const glm::vec2 inv{1.0f / float(f->metrics.atlas_w), 1.0f / float(f->metrics.atlas_h)};
    for (const GlyphQuad& q : glyph_scratch_) {
        const glm::vec4 r{x + float(q.pos.x) * scale, pos.y + float(q.pos.y) * scale, float(q.size.x) * scale, float(q.size.y) * scale};
        image(op == TextOp::Diffuse ? f->mask : f->texture, r, glm::vec2(q.src.x, q.src.y) * inv,
              glm::vec2(q.src.z + 1, q.src.w + 1) * inv, color);
    }
    return w;
}

int Ui::text(std::string_view font_name, std::string_view s, glm::vec2 pos, glm::vec4 color, TextAlign align, TextOp op) {
    return text(font(font_name), s, pos, color, align, 1.0f, op);
}

int Ui::text_block(const UiFont* f, std::string_view s, glm::vec4 rect, glm::vec4 color, int line_step, TextAlign align, TextOp op) {
    if (!f) return 0;
    const std::vector<std::string> lines = wrap_text(f->metrics, s, int(rect.z));
    const int step = line_step > 0 ? line_step : f->metrics.line_height();
    float x = rect.x;
    if (align == TextAlign::Center) x += std::floor(rect.z / 2.0f);
    if (align == TextAlign::Right) x += rect.z;
    for (std::size_t i = 0; i < lines.size(); ++i) text(f, lines[i], {x, rect.y + float(i) * float(step)}, color, align, 1.0f, op);
    return int(lines.size());
}

// ---------------------------------------------------------------- hit testing

bool Ui::hover(glm::vec4 r) const {
    const glm::vec2 m = mouse();
    return m.x >= r.x && m.y >= r.y && m.x < r.x + r.z && m.y < r.y + r.w;
}
bool Ui::clicked(glm::vec4 r) const { return input_.mouse_released && hover(r); }
bool Ui::pressed(glm::vec4 r) const { return input_.mouse_pressed && hover(r); }

// ---------------------------------------------------------------- cursor

void Ui::cursor(int index) {
    if (!cursors_loaded_) {
        cursors_loaded_ = true;
        const auto table_bytes = vfs_.read("Resource/CURSOR.RES");
        const auto bitmaps_bytes = vfs_.read("Resource/I_CURSOR.RES");
        const auto pal_bytes = vfs_.read("Resource/pal_std.res");
        const auto table = table_bytes ? formats::DbfTable::parse(*table_bytes) : std::nullopt;
        const auto bitmaps = bitmaps_bytes ? formats::parse_bitmap_db(*bitmaps_bytes) : std::nullopt;
        const auto pal = pal_bytes ? formats::parse_palette(*pal_bytes) : std::nullopt;
        if (table && bitmaps && pal && table->fields().size() >= 4) {
            for (std::size_t r = 0; r < table->record_count(); ++r) {
                // Fields (stray 'H' prefix): FILENAME, HOTSPOT_X, HOTSPOT_Y, BITMAPPTR (u32 offset into I_CURSOR.RES).
                const auto ptr = table->raw(r, 3);
                const std::uint32_t off = ptr[0] | ptr[1] << 8 | ptr[2] << 16 | std::uint32_t(ptr[3]) << 24;
                const glm::vec2 hot{float(std::atoi(table->text(r, 1).c_str())), float(std::atoi(table->text(r, 2).c_str()))};
                for (const formats::IndexedBitmap& b : *bitmaps) {
                    if (b.offset != off) continue;
                    std::vector<std::uint8_t> px(std::size_t(b.width) * b.height * 4);
                    for (std::size_t i = 0; i < b.pixels.size() && i * 4 < px.size(); ++i) {
                        const std::uint8_t c = b.pixels[i];
                        px[i * 4 + 0] = pal->rgb[c][0];
                        px[i * 4 + 1] = pal->rgb[c][1];
                        px[i * 4 + 2] = pal->rgb[c][2];
                        px[i * 4 + 3] = c == 0xFF ? 0 : 255;  // 0xFF = transparent (HYPOTHESIS, matches the shapes)
                    }
                    cursors_.push_back({upload(renderer_, b.width, b.height, std::move(px)), hot});
                    break;
                }
            }
        }
        if (cursors_.empty()) std::fprintf(stderr, "ui: cursor resources unavailable\n");
    }
    if (!out_ || cursors_.empty() || input_.mouse.x < 0.0f) return;
    const auto& [tex, hot] = cursors_[std::size_t(std::clamp(index, 0, int(cursors_.size()) - 1))];
    const float s = layout_.scale;
    const glm::vec2 p = glm::round(input_.mouse - hot * s);
    out_->push_back({p, glm::round(glm::vec2(32.0f) * s), {0, 0}, {1, 1}, kUiWhite, tex});
}

}  // namespace joa::engine
