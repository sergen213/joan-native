#pragma once
// 2D interface core (en2d/interfac replacement): INTERFAC.SET sprites, bitmap fonts, t_interf.txr strings,
// the 1024x768 design canvas and immediate-mode hit testing. One Ui per renderer.
// RE notes and coordinate rules: docs/reverse-engineering/UI_SYSTEM.md.
//
// Coordinates: every draw/hit call takes 1024x768 design units. Frame() picks how they map to pixels:
//   Anchor::Center (default) = 4:3 safe area, centred and pillar/letterboxed (menus, dialogs);
//   a corner/edge anchor      = same scale, but positions keep their design distance to that real
//                               screen edge (HUD corners on 16:9/21:9).
// Scale = safe height / 768 (high-DPI and any resolution).
#include "engine/camera.hpp"
#include "engine/hd_pack.hpp"
#include "engine/renderer.hpp"
#include "engine/ui_font.hpp"
#include "engine/ui_model.hpp"
#include "formats/vfs_fs.hpp"

#include <glm/glm.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace joa::engine {

// sRGB byte colour -> linear UiQuad colour (alpha stays linear).
glm::vec4 ui_rgba(int r, int g, int b, int a = 255);
inline const glm::vec4 kUiWhite{1.0f};

// One INTERFAC.SET record (BITMAP, TEXTURE, SRC_X1..SRC_Y2 inclusive, ROTATION, WIDTH, HEIGHT).
struct UiSprite {
    TextureHandle texture;
    glm::vec2 uv0{0.0f}, uv1{1.0f};
    glm::vec2 size{0.0f};   // WIDTH x HEIGHT (drawn size, design units)
    glm::ivec4 src{0};      // texel rect
    bool rotated = false;   // ROTATION = 1: texels are stored rotated 90 degrees (only OPTMENU remap_fr)
};

struct UiFont {
    UiFontMetrics metrics;
    TextureHandle texture;  // atlas as shipped
    TextureHandle mask;     // same alpha, RGB forced white (TextOp::Diffuse)
};

enum class TextAlign : std::uint8_t { Left, Center, Right };
// Glyph colour source. en2d text calls pass texture-stage selectors to the quad draw 0x632a70 (tables 0xa1977c /
// 0xa19790); the common menu call (0x626130 with 0,1,2,0) takes RGB from the draw colour and alpha = glyph alpha x
// colour alpha (Diffuse). Modulate = atlas RGBA x colour (fonts whose atlas carries the colour, e.g. gold/outlined).
enum class TextOp : std::uint8_t { Modulate, Diffuse };

// GamepadPlay: pad state for UI navigation (PadUiTracker in game_input.hpp fills it from RawInput).
struct UiPad {
    glm::ivec2 dir{0};      // D-pad / left stick: one axis, -1/+1 (y down) on press and on auto-repeat
    bool activate = false;  // South (A / Cross) pressed
    bool back = false;      // East (B / Circle) or Start pressed: Escape on modal screens
    int page = 0;           // LB -1 / RB +1 pressed
    int button = -1;        // SDL_GamepadButton pressed this frame (remap capture), -1 = none
    bool connected = false; // a pad is open
    bool used = false;      // any pad input this frame (the last-used device wins)
};

struct UiInput {
    glm::vec2 mouse{-1.0f};       // window pixels (framebuffer space)
    bool mouse_down = false;      // left button held
    bool mouse_pressed = false;   // left button went down this frame
    bool mouse_released = false;  // left button went up this frame (original buttons fire on release)
    bool right_pressed = false;
    float wheel = 0.0f;
    int key_pressed = 0;          // SDL_Scancode pressed this frame (0 = none); Escape = SDL_SCANCODE_ESCAPE
    std::string text;             // UTF-8 text typed this frame (save description)
    // GamepadPlay:
    UiPad pad;
};

class Ui {
public:
    // hd_packs: resolved HD pack directories (resolve_hd_packs), first wins. int8x6 images (art, fonts) and the
    // CURSOR.RES pointers are replaced by pack files of any size, sampled in normalized coordinates so every
    // layout/UV stays on the original texel grid (docs/HD_ASSETS.md "UI").
    Ui(const formats::Vfs& vfs, IRenderer& renderer, std::vector<std::filesystem::path> hd_packs = {});
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // ---- data (cached, loaded on first use) ----
    // INTERFAC.SET table `set` (e.g. "MENU1024"), record `name` (case-insensitive). nullptr if missing.
    const UiSprite* sprite(std::string_view set, std::string_view name);
    // Any registered en2d font: .fin fonts ("Goudy_bold_24") or grid fonts ("24pt large text"). Case-insensitive.
    const UiFont* font(std::string_view name);
    // Resource/t_interf.txr string, e.g. text("MAINMENU", 1) = "New Campaign". Raw cp1252 bytes; "" if missing.
    std::string_view text(std::string_view section, int index);
    // int8x6/<name>.dds (any case) as a clamped UI texture; empty handle if missing.
    TextureHandle texture(std::string_view name);
    glm::ivec2 texture_size(std::string_view name);
    // Uncompressed 24/32-bit BMP at a game path (Save\<name>.BMP thumbnails) or an absolute file path (native save
    // thumbnails), cached by lowercase path; reload = re-read after the file was rewritten. Empty handle if missing.
    TextureHandle bitmap(std::string_view game_path, bool reload = false);
    // Caller-built texture cached by `key` for this Ui's lifetime: `make` runs on the first call only and returns
    // a w x h RGBA8 (sRGB) image (empty = missing). For pixels no INTERFAC.SET record describes (e.g. the brief's
    // lit/rotated 3D-map texture).
    TextureHandle baked_texture(std::string_view key, const std::function<Image()>& make);
    const formats::Vfs& vfs() const { return vfs_; }
    IRenderer& renderer() { return renderer_; }

    // ---- frame ----
    void begin(glm::ivec4 viewport, std::vector<UiQuad>& out, const UiInput& input, float dt);
    void frame(Anchor anchor = Anchor::Center);  // coordinate frame for following calls
    Anchor current_frame() const { return frame_; }
    const UiLayout& layout() const { return layout_; }
    const UiInput& input() const { return input_; }
    float dt() const { return dt_; }

    // design point/rect (x, y, w, h) -> window pixels in the current frame, and back
    glm::vec2 to_pixels(glm::vec2 design) const;
    glm::vec2 to_design(glm::vec2 pixels) const;
    glm::vec4 rect_pixels(glm::vec4 design) const;
    glm::vec2 mouse() const { return to_design(input_.mouse); }

    // ---- drawing (design units) ----
    // turns: texture turned turns x 90 degrees counter-clockwise inside `rect` (UiQuad::turns; Center-frame clipping
    // assumes 0).
    void image(TextureHandle tex, glm::vec4 rect, glm::vec2 uv0 = {0, 0}, glm::vec2 uv1 = {1, 1}, glm::vec4 color = kUiWhite,
               int turns = 0);
    void fill(glm::vec4 rect, glm::vec4 color);
    // Like fill(), but never clipped to the 1024x768 canvas: for full-window bars/black screens in pillar areas.
    void fill_window(glm::vec4 rect, glm::vec4 color);
    void sprite(const UiSprite* s, glm::vec2 pos, glm::vec4 color = kUiWhite);
    void sprite(const UiSprite* s, glm::vec4 rect, glm::vec4 color = kUiWhite);  // stretched
    void sprite(std::string_view set, std::string_view name, glm::vec2 pos, glm::vec4 color = kUiWhite);
    // True if INTERFAC.SET has the record (no texture decode, no warning) — for probing tile grids.
    bool has_sprite(std::string_view set, std::string_view name);
    // Single line at (x, y) = top of the line; align relative to x. Returns the text width. `scale` scales glyphs.
    int text(const UiFont* f, std::string_view s, glm::vec2 pos, glm::vec4 color = kUiWhite, TextAlign align = TextAlign::Left,
             float scale = 1.0f, TextOp op = TextOp::Modulate);
    int text(std::string_view font, std::string_view s, glm::vec2 pos, glm::vec4 color = kUiWhite, TextAlign align = TextAlign::Left,
             TextOp op = TextOp::Modulate);
    // Word-wrapped block; returns the number of lines. line_step 0 = font line height.
    int text_block(const UiFont* f, std::string_view s, glm::vec4 rect, glm::vec4 color = kUiWhite, int line_step = 0,
                   TextAlign align = TextAlign::Left, TextOp op = TextOp::Modulate);

    // ---- hit testing (design units, current frame) ----
    bool hover(glm::vec4 rect) const;
    // Left button released inside (en2d buttons act on release). `sound`: the button plays IF_BUTTON_MOUSE_DOWN
    // (button input 0x6218c0 when the widget's +0x90 flag is set: every menu/HUD button); false for non-buttons.
    bool clicked(glm::vec4 rect, bool sound = true);
    bool pressed(glm::vec4 rect) const;   // left button went down inside
    bool key(int scancode) const { return input_.key_pressed == scancode; }
    bool shortcut(int scancode);          // key() of a button's key shortcut (0x6218c0 key path): plays the click too

    // ---- sound effects (SOUND table codes, 2D at the listener) ----
    // Queued this frame (begin() clears); the owner of the AudioSystem plays them after drawing.
    void sound(std::string_view code) { sounds.emplace_back(code); }
    // Plays `code` in the frame where a screen clock t (advanced by dt() per frame) reaches `at`.
    void sound_at(std::string_view code, float t, float at) {
        if (t >= at && t - dt_ < at) sound(code);
    }
    std::vector<std::string> sounds;

    // ---- gamepad focus (GamepadPlay) ----
    // A modal screen (menu, overlay, popup) calls nav_modal(scope) each frame before its widgets; the rects it
    // then passes to hover()/clicked() form the focus ring (duplicates and rects inside an earlier one skipped,
    // registration order = focus index). While the pad is the last-used device and the previous frame was
    // modal: hover() = "this is the focused rect" (screens draw their hover state), D-pad/left stick moves the
    // focus to the nearest rect in that direction, South releases the "mouse" (clicked(), mouse_released),
    // East/Start press Escape. A new scope focuses rect 0; the last nav_modal of a frame owns the ring.
    void nav_modal(int scope);
    void nav_seal() { nav_sealed_ = true; }  // later widgets this frame stay out of the ring (HUD drawn after a modal window)
    int nav_adjust();       // in a focused value widget (slider/combo): pad left/right -1/+1, which then stop moving focus
    int nav_page() const { return pad_focus() ? page_ : 0; }  // LB/RB on a modal screen: -1/+1
    glm::vec4 nav_focus() const { return pad_focus() ? focus_rect_ : glm::vec4(0.0f); }  // focused rect, window pixels (x, y, w, h)
    bool pad_focus() const { return pad_mode_ && nav_live_; }

    // Mouse pointer (CURSOR.RES record `index`, 0 = arrow) at the mouse position. Call last. Hidden while pad focus drives a modal screen.
    void cursor(int index = 0);

private:
    void load_sprites();
    void load_text();
    // TextureData for UI image `rel` (game-relative path) decoded as w x h `rgba`: the HD pack file if one exists
    // (mipmapped), else these pixels as-is.
    TextureData ui_image(std::string_view rel, int w, int h, std::vector<std::uint8_t> rgba);
    // create_texture; an image larger than `orig` (HD pack) gets a clamp inset (hd_inset_) used by image().
    TextureHandle create(const TextureData& t, glm::ivec2 orig);

    const formats::Vfs& vfs_;
    IRenderer& renderer_;
    HdPacks hd_;
    int hd_used_ = 0;
    std::unordered_map<std::uint32_t, glm::vec2> hd_inset_;  // texture id -> UiQuad::clamp_inset
    struct SpriteRecord {
        std::string texture;
        glm::ivec4 src{0};
        glm::vec2 size{0.0f};
        bool rotated = false;
    };
    std::unordered_map<std::string, SpriteRecord> sprite_records_;          // "set/name" lowercase
    std::unordered_map<std::string, std::optional<UiSprite>> sprites_;      // resolved; nullopt = missing
    std::unordered_map<std::string, std::pair<TextureHandle, glm::ivec2>> textures_;  // lowercase name
    std::unordered_map<std::string, std::unique_ptr<UiFont>> fonts_;                 // lowercase name, null = failed
    std::unordered_map<std::string, std::string> text_;                             // "SECTION|n"
    std::vector<std::pair<TextureHandle, glm::vec2>> cursors_;                       // texture, hotspot
    bool sprites_loaded_ = false, text_loaded_ = false, cursors_loaded_ = false;

    std::vector<UiQuad>* out_ = nullptr;
    UiLayout layout_{};
    UiInput input_{};
    Anchor frame_ = Anchor::Center;
    float dt_ = 0.0f;
    std::vector<GlyphQuad> glyph_scratch_;
    // GamepadPlay focus ring (pixel rects; mutable: hover() registers).
    int nav_index(glm::vec4 design) const;
    mutable std::vector<glm::vec4> nav_rects_;
    std::vector<glm::vec4> nav_prev_;
    mutable bool nav_open_ = false;
    bool nav_live_ = false, pad_mode_ = false, adjustable_prev_ = false;
    bool adjustable_ = false, focus_moved_ = false, nav_sealed_ = false;
    int nav_scope_ = -1, focus_scope_ = -2, focus_ = 0, adjust_dx_ = 0, page_ = 0;
    glm::vec4 focus_rect_{0.0f};
    glm::vec2 last_mouse_{-1.0f};
    bool click_sounded_ = false;  // HudSounds: IF_BUTTON_MOUSE_DOWN queued this frame (one per frame)
};

}  // namespace joa::engine
