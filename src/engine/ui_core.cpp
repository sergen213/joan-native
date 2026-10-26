#include "engine/ui_core.hpp"

#include "engine/assets_cache.hpp"  // lower()
#include "formats/dds.hpp"
#include "formats/save_sav.hpp"  // parse_bmp_header
#include "formats/res_archive.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

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

char fold(char c, bool on) { return on ? char(std::tolower(static_cast<unsigned char>(c))) : c; }  // as lower()

}  // namespace

template <bool Fold>
std::size_t Ui::KeyHash<Fold>::operator()(const Key& k) const {  // FNV-1a over a + sep + b
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](char c) { h = (h ^ std::uint8_t(fold(c, Fold))) * 1099511628211ull; };
    for (char c : k.a) mix(c);
    if (k.sep) mix(k.sep);
    for (char c : k.b) mix(c);
    return std::size_t(h);
}

template <bool Fold>
bool Ui::KeyEq<Fold>::operator()(const Key& k, std::string_view s) const {
    const std::size_t n = k.a.size() + (k.sep ? 1 : 0);
    if (s.size() != n + k.b.size()) return false;
    const auto eq = [](std::string_view x, std::string_view y) {
        for (std::size_t i = 0; i < x.size(); ++i)
            if (fold(x[i], Fold) != fold(y[i], Fold)) return false;
        return true;
    };
    return eq(k.a, s.substr(0, k.a.size())) && (!k.sep || fold(s[k.a.size()], Fold) == fold(k.sep, Fold)) && eq(k.b, s.substr(n));
}

template struct Ui::KeyHash<true>;
template struct Ui::KeyHash<false>;
template struct Ui::KeyEq<true>;
template struct Ui::KeyEq<false>;

glm::vec4 ui_rgba(int r, int g, int b, int a) { return {srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b), float(a) / 255.0f}; }

Ui::Ui(const formats::Vfs& vfs, IRenderer& renderer, std::vector<std::filesystem::path> hd_packs)
    : vfs_(vfs), renderer_(renderer), hd_(std::move(hd_packs)) {}

TextureData Ui::ui_image(std::string_view rel, int w, int h, std::vector<std::uint8_t> rgba) {
    TextureData t;
    t.width = w;
    t.height = h;
    t.pixels = std::move(rgba);
    t.mipmaps = false;
    t.repeat = false;
    if (const std::filesystem::path f = hd_.find(rel).albedo; !f.empty()) {
        formats::DdsTexture d;
        std::string err;
        if (load_image_file(f, d, err, 1) && !d.images.empty()) {
            t.width = int(d.images[0].width);
            t.height = int(d.images[0].height);
            t.pixels = std::move(d.images[0].rgba);
            t.mipmaps = true;  // minified below a 4x pack's native scale (UI scale < 4); the UI shaders bias -0.5
            ++hd_used_;
        } else {
            std::fprintf(stderr, "ui: hd %s: %s, original used\n", f.c_str(), err.c_str());
        }
    }
    return t;
}

TextureHandle Ui::create(const TextureData& t, glm::ivec2 orig) {
    const TextureHandle h = renderer_.create_texture(t);
    // HD image: clamp like the original (half an original texel, the shader adds half an HD texel).
    if (h && orig.x > 0 && orig.y > 0 && (t.width != orig.x || t.height != orig.y))
        hd_inset_[h.id] = 0.5f / glm::vec2(orig) - 0.5f / glm::vec2(t.width, t.height);
    return h;
}

Ui::~Ui() {
    if (hd_used_) std::fprintf(stderr, "ui: hd: %d images overridden\n", hd_used_);
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
    if (auto it = textures_.find(name); it != textures_.end()) return it->second.first;
    const std::string key = lower(name);
    std::pair<TextureHandle, glm::ivec2> entry{};
    if (auto img = load_dds(vfs_, name)) {
        entry.second = {int(img->width), int(img->height)};  // layout/UV basis stays the original size
        entry.first = create(ui_image("int8x6/" + std::string(name) + ".dds", int(img->width), int(img->height), std::move(img->rgba)), entry.second);
    } else {
        std::fprintf(stderr, "ui: missing texture int8x6/%s.dds\n", key.c_str());
    }
    return textures_.emplace(key, entry).first->second.first;
}

glm::ivec2 Ui::texture_size(std::string_view name) {
    texture(name);
    return textures_.find(name)->second.second;  // texture() caches every name, found or not
}

TextureHandle Ui::bitmap(std::string_view game_path, bool reload) {
    const std::string key = "bmp:" + lower(game_path);
    if (auto it = textures_.find(key); it != textures_.end()) {
        if (!reload) return it->second.first;
        if (it->second.first) renderer_.destroy_texture(it->second.first);
        textures_.erase(it);
    }
    std::pair<TextureHandle, glm::ivec2> entry{};
    const bool native = std::filesystem::path(game_path).is_absolute();  // native save thumbnail (Session::save)
    const auto bytes = native ? formats::read_file(std::filesystem::path(game_path)) : vfs_.read(game_path);
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

TextureHandle Ui::baked_texture(std::string_view key, const std::function<Image()>& make) {
    const std::string k = "baked:" + lower(key);
    if (auto it = textures_.find(k); it != textures_.end()) return it->second.first;
    Image img = make();
    std::pair<TextureHandle, glm::ivec2> entry{};
    if (img.width > 0 && img.height > 0) entry = {upload(renderer_, img.width, img.height, std::move(img.rgba)), {img.width, img.height}};
    return textures_.emplace(k, entry).first->second.first;
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
    if (auto it = sprites_.find(Key{set, '/', name}); it != sprites_.end()) return it->second ? &*it->second : nullptr;
    const std::string key = lower(set) + "/" + lower(name);
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
            // An HD pack image is baked from the same rect scaled to its size.
            const TextureData src = ui_image("int8x6/" + r.texture + ".dds", int(img->width), int(img->height), std::move(img->rgba));
            const float kx = float(src.width) / float(img->width), ky = float(src.height) / float(img->height);
            const int x0 = int(float(s.src.x) * kx), y0 = int(float(s.src.y) * ky);
            const int sw = int(float(s.src.z + 1) * kx) - x0, sh = int(float(s.src.w + 1) * ky) - y0;
            TextureData baked;
            baked.width = sh;
            baked.height = sw;
            baked.pixels.resize(std::size_t(sw) * sh * 4);
            baked.mipmaps = src.mipmaps;
            baked.repeat = false;
            for (int y = 0; y < sh; ++y)
                for (int x = 0; x < sw; ++x) {
                    const int dx = y, dy = sw - 1 - x;  // counter-clockwise
                    const int sx = std::min(x0 + x, src.width - 1), sy = std::min(y0 + y, src.height - 1);
                    std::copy_n(&src.pixels[(std::size_t(sy) * src.width + sx) * 4], 4, &baked.pixels[(std::size_t(dy) * sh + dx) * 4]);
                }
            s.texture = create(baked, {s.src.w - s.src.y + 1, s.src.z - s.src.x + 1});
            out = s;
        }
    }
    if (!out) std::fprintf(stderr, "ui: missing sprite %s\n", key.c_str());
    auto& slot = sprites_[key];
    slot = out;
    return slot ? &*slot : nullptr;
}

const UiFont* Ui::font(std::string_view name) {
    if (auto it = fonts_.find(name); it != fonts_.end()) return it->second.get();
    const std::string key = lower(name);
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
            // Metrics come from the original atlas above; the textures may be an HD pack atlas (same layout, any size).
            TextureData tex = ui_image("int8x6/" + std::string(name) + ".dds", w, h, std::move(img->rgba));
            TextureData white = tex;
            for (std::size_t i = 0; i + 3 < white.pixels.size(); i += 4) white.pixels[i] = white.pixels[i + 1] = white.pixels[i + 2] = 255;
            f->mask = create(white, {w, h});
            f->texture = create(tex, {w, h});
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
    char n[16];  // std::to_string(index) digits
    const auto [end, ec] = std::to_chars(n, n + sizeof n, index);
    (void)ec;
    auto it = text_.find(Key{section, '|', std::string_view(n, std::size_t(end - n))});
    return it == text_.end() ? std::string_view{} : std::string_view(it->second);
}

// ---------------------------------------------------------------- frame / transform

void Ui::begin(glm::ivec4 viewport, std::vector<UiQuad>& out, const UiInput& input, float dt) {
    out_ = &out;
    layout_ = ui_layout(viewport);
    input_ = input;
    dt_ = dt;
    frame_ = Anchor::Center;
    sounds.clear();
    click_sounded_ = false;

    // GamepadPlay: last-used device; roll the focus ring of the previous frame over.
    const bool woke = input.pad.used && !pad_mode_;  // the first pad input only shows the focus (no step)
    if (input.pad.used) pad_mode_ = true;
    else if (input.mouse_pressed || input.right_pressed || (input.mouse.x >= 0 && glm::distance(input.mouse, last_mouse_) > 2.0f))
        pad_mode_ = false;
    last_mouse_ = input.mouse;
    nav_prev_.swap(nav_rects_);
    nav_rects_.clear();
    nav_live_ = std::exchange(nav_open_, false);
    nav_sealed_ = false;
    adjustable_prev_ = std::exchange(adjustable_, false);
    adjust_dx_ = page_ = 0;
    if (!pad_focus()) return;
    if (focus_scope_ != nav_scope_) {
        focus_scope_ = nav_scope_, focus_ = 0, focus_moved_ = false;
    } else if (focus_moved_) {  // a chosen widget is followed when others appear/vanish (until then: the first one)
        const auto same = std::find(nav_prev_.begin(), nav_prev_.end(), focus_rect_);
        if (same != nav_prev_.end()) focus_ = int(same - nav_prev_.begin());
    }
    page_ = input.pad.page;
    if (input.pad.back && !input_.key_pressed) input_.key_pressed = SDL_SCANCODE_ESCAPE;
    if (input.pad.activate) input_.mouse_released = true;  // the "click" (dialog text advance needs no focus)
    if (nav_prev_.empty()) return;
    focus_ = std::clamp(focus_, 0, int(nav_prev_.size()) - 1);
    glm::ivec2 d = woke ? glm::ivec2(0) : input.pad.dir;
    if (adjustable_prev_ && d.x) adjust_dx_ = d.x, d.x = 0;
    if (d != glm::ivec2(0)) {
        // Nearest rect whose centre lies in that direction; the gap across the direction weighs 4x.
        const glm::vec4 f = nav_prev_[std::size_t(focus_)];
        const glm::vec2 fc{f.x + f.z / 2, f.y + f.w / 2};
        float best = 1e30f;
        int to = focus_;
        for (int i = 0; i < int(nav_prev_.size()); ++i) {
            const glm::vec4 r = nav_prev_[std::size_t(i)];
            const glm::vec2 c{r.x + r.z / 2, r.y + r.w / 2};
            const float along = d.x ? (c.x - fc.x) * float(d.x) : (c.y - fc.y) * float(d.y);
            const float across = d.x ? std::max({0.0f, r.y - (f.y + f.w), f.y - (r.y + r.w)}) : std::max({0.0f, r.x - (f.x + f.z), f.x - (r.x + r.z)});
            if (along >= 1.0f && along + 4 * across < best) best = along + 4 * across, to = i;
        }
        focus_moved_ |= to != focus_;
        focus_ = to;
    }
    focus_rect_ = nav_prev_[std::size_t(focus_)];
}

void Ui::nav_modal(int scope) {
    nav_open_ = true, nav_sealed_ = false;
    nav_scope_ = scope;
    nav_rects_.clear();
}

int Ui::nav_adjust() {
    if (!pad_focus()) return 0;
    adjustable_ = true;
    return std::exchange(adjust_dx_, 0);
}

int Ui::nav_index(glm::vec4 design) const {
    const glm::vec4 p = rect_pixels(design);
    if (p.z <= 0 || p.w <= 0 || nav_sealed_) return -1;
    for (std::size_t i = 0; i < nav_rects_.size(); ++i) {
        const glm::vec4 q = nav_rects_[i];
        if (p == q) return int(i);
        if (p.x >= q.x && p.y >= q.y && p.x + p.z <= q.x + q.z && p.y + p.w <= q.y + q.w) return -1;  // arrow inside its row
    }
    nav_rects_.push_back(p);
    return int(nav_rects_.size()) - 1;
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

void Ui::image(TextureHandle tex, glm::vec4 rect, glm::vec2 uv0, glm::vec2 uv1, glm::vec4 color, int turns) {
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
    out_->push_back({{px.x, px.y}, {px.z, px.w}, uv0, uv1, color, tex, turns});
    if (const auto it = hd_inset_.find(tex.id); it != hd_inset_.end()) out_->back().clamp_inset = it->second;
}

void Ui::fill(glm::vec4 rect, glm::vec4 color) { image({}, rect, {0, 0}, {1, 1}, color); }

void Ui::fill_window(glm::vec4 rect, glm::vec4 color) {
    if (!out_ || rect.z <= 0.0f || rect.w <= 0.0f) return;
    const glm::vec4 px = rect_pixels(rect);
    out_->push_back({{px.x, px.y}, {px.z, px.w}, {0, 0}, {1, 1}, color, {}});
}

void Ui::sprite(const UiSprite* s, glm::vec2 pos, glm::vec4 color) {
    if (s) image(s->texture, {pos.x, pos.y, s->size.x, s->size.y}, s->uv0, s->uv1, color);
}

void Ui::sprite(const UiSprite* s, glm::vec4 rect, glm::vec4 color) {
    if (s) image(s->texture, rect, s->uv0, s->uv1, color);
}

void Ui::sprite(std::string_view set, std::string_view name, glm::vec2 pos, glm::vec4 color) { sprite(sprite(set, name), pos, color); }

bool Ui::has_sprite(std::string_view set, std::string_view name) {
    if (!sprites_loaded_) load_sprites();
    return sprite_records_.find(Key{set, '/', name}) != sprite_records_.end();
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
    const int i = nav_open_ ? nav_index(r) : -1;
    if (pad_focus()) return i >= 0 && i == focus_;
    const glm::vec2 m = mouse();
    return m.x >= r.x && m.y >= r.y && m.x < r.x + r.z && m.y < r.y + r.w;
}
bool Ui::clicked(glm::vec4 r, bool sound) {
    const bool hit = input_.mouse_released && hover(r);
    if (hit && sound && !std::exchange(click_sounded_, true)) this->sound("IF_BUTTON_MOUSE_DOWN");
    return hit;
}
bool Ui::shortcut(int scancode) {
    const bool hit = key(scancode);
    if (hit && !std::exchange(click_sounded_, true)) sound("IF_BUTTON_MOUSE_DOWN");
    return hit;
}
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
                    // HD pack: Resource/I_CURSOR/<byte offset>.png (hd-upscale writes one image per record).
                    cursors_.push_back({renderer_.create_texture(ui_image("Resource/I_CURSOR/" + std::to_string(off) + ".png", b.width, b.height, std::move(px))), hot});
                    break;
                }
            }
        }
        if (cursors_.empty()) std::fprintf(stderr, "ui: cursor resources unavailable\n");
    }
    if (!out_ || cursors_.empty() || input_.mouse.x < 0.0f || pad_focus()) return;
    const auto& [tex, hot] = cursors_[std::size_t(std::clamp(index, 0, int(cursors_.size()) - 1))];
    const float s = layout_.scale;
    const glm::vec2 p = glm::round(input_.mouse - hot * s);
    out_->push_back({p, glm::round(glm::vec2(32.0f) * s), {0, 0}, {1, 1}, kUiWhite, tex});
}

}  // namespace joa::engine
