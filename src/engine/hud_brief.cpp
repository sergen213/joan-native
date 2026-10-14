// Mission-brief topic pages (objmenu). Positions are the exe's 1024x768 constants; evidence, measurements and gaps:
// work/ui/notes_MissionBrief.md (per-page RE: work/ui/MissionBrief/re_*.md).
#include "engine/hud_brief.hpp"

#include "engine/menu_options_store.hpp"
#include "engine/menu_saveload.hpp"
#include "engine/ui_font.hpp"
#include "formats/dds.hpp"
#include "formats/model_mesh.hpp"
#include "formats/res_archive.hpp"
#include "formats/scenario_def.hpp"
#include "formats/text_strings.hpp"
#include "formats/world_text.hpp"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace joa::engine {
namespace {

constexpr float kPi = 3.14159265f;
const glm::vec4 kInk = ui_rgba(21, 21, 21);  // siege page texts (0x739540)

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

std::string replace(std::string s, std::string_view key, std::string_view value) {
    if (const auto p = s.find(key); p != std::string::npos) s.replace(p, key.size(), value);
    return s;
}

std::string text_of(const std::optional<std::vector<std::uint8_t>>& bytes) {
    std::string s;
    if (bytes)
        for (const std::uint8_t c : *bytes)
            if (c != '\r') s += char(c);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// Page scaffold of FUN_0071f690 / FUN_006f21c0 by en2d layer: back = woodtex (10) and the black 1024x768 quad of
// FUN_00732bf0 / FUN_00737030 (9, pages 2 and 3); then page widgets on layers 6..9 (OBJMENU mask, 2D battleground), the sphere
// shading (5, pass 1), the View3D render, ring and stone halves (5, pass 2) and the layer 1..4 widgets.
void page_back(Ui& ui, bool black) {
    ui.sprite("INGAME_B", "woodtex", {0, 0});
    if (black) ui.fill({0, 0, 1024, 768}, ui_rgba(0, 0, 0));
}

void page_sphere(Ui& ui) { ui.sprite("INGAME_B", "sphere", {40, 24}); }

void page_ring(Ui& ui) {
    ui.sprite("INGAME_B", "ring", {0, 0});
    ui.sprite("INGAME_B", "left", {0, 0});
    ui.sprite("INGAME_B", "right", {512, 0});
}

// LONGBARs (0,0x84)/(0,0x298) with the page heading (OBJMENU 4..8/44/63 upper-cased, FUN_0071f690 switch), BACK
// (0x70: rect 100x30 at (back_x, 0x298), fades in at 6/s after 200 ms), an optional second bar button (OBJMENU
// `extra` at extra_x: 2D/3D VIEW, HELP; fades in at 6/s) and the layer-1 backdrop labelled "Mission Brief"
// (OBJMENU 15). Returns 1 when BACK or Escape asks for the topic menu (FUN_00721a60(0)), 2 when `extra` is clicked.
int page_front(Ui& ui, const HudModel& m, int heading, int back_x = 0x1ce, int extra = 0, int extra_x = 0, bool live = true) {
    menu_heading_bars(ui, -48, upper(ui.text("OBJMENU", heading)));
    const UiFont* f = ui.font("Goudy14(22) white");
    const float a = live ? 1.0f : 0.0f;
    const bool back = ui_text_button(ui, f, ui.text("OBJMENU", 9), {back_x, 0x298, back_x + 0x64, 0x2b6}, a * ui_fade(m.brief_time, 0.2f, 6));
    const bool second = extra && ui_text_button(ui, f, ui.text("OBJMENU", extra), {extra_x, 0x298, extra_x + 0x64, 0x2b6}, a * ui_fade(m.brief_time, 0, 6));
    ingame_backdrop_front(ui, 1e9f, -1.0f, ui.text("OBJMENU", 15));
    return back || (live && ui.shortcut(SDL_SCANCODE_ESCAPE)) ? 1 : second ? 2 : 0;
}

// en2d sprite button (0x6211d0: ot / dw / ov records, 68x20 arrows); returns true on click.
bool arrow(Ui& ui, const char* base, glm::vec2 pos, float alpha) {
    const glm::vec4 r{pos.x, pos.y, 68, 20};
    const bool hot = alpha >= 1.0f && ui.hover(r);
    ui.sprite("COMMELE2", std::string(base) + (hot && ui.input().mouse_down ? "_dw" : hot ? "_ov" : "_ot"), pos, {1, 1, 1, alpha});
    return hot && ui.clicked(r);
}

// Sprite with alpha (most page parts fade with one widget).
void spr(Ui& ui, std::string_view set, std::string_view name, glm::vec2 pos, float a) { ui.sprite(set, name, pos, {1, 1, 1, a}); }

// ---- shared list (FUN_0071d2d0): TXT_FV frame at (0x19,0xb8), sliding in from x -356 (3500 px/s, -1500 px/s^2,
// fade 6/s); 10 row buttons (0x17, 0xdc + 40 i)-(0x17f, 0x103 + 40 i) fading in at 4/s after 200 ms; aw_u (0xa9,0xc2)
// while scroll > 0 and aw_d (0xa9,0x26c) while scroll < total - 10, also 4/s after 200 ms. ----
constexpr int row_y(int i) { return 0xdc + 40 * i; }

// Returns the scroll step clicked (-1, 0, +1).
int list_frame(Ui& ui, const HudModel& m, int total, int max_scroll = -1) {
    const float t = m.brief_time, a = ui_fade(t, 0.2f, 4);
    spr(ui, "OBJMENU", "txt_fv", {-356.0f + ui_slide(t, 0, 381), 0xb8}, ui_fade(t, 0, 6));
    if (max_scroll < 0) max_scroll = total - 10;
    int step = 0;
    if (m.brief_scroll > 0 && arrow(ui, "aw_u", {0xa9, 0xc2}, a)) step = -1;
    if (m.brief_scroll < max_scroll && arrow(ui, "aw_d", {0xa9, 0x26c}, a)) step = 1;
    return step;
}

bool row_hover(Ui& ui, int i) { return ui.hover({0x17, float(row_y(i)), 0x17f - 0x17 + 1, 0x28}); }

// Row button draw callback FUN_00723870: M_OT bar (M_OV while hovered) at (x1, y1), label word-wrapped left/top in
// (x1 + 0x32, y1 + 1)-(x2 - 0xe, y2 - 4) in the row's font, white.
void row_bar(Ui& ui, int i, std::string_view label, const UiFont* f, float a) {
    const int y = row_y(i);
    spr(ui, "OBJMENU", row_hover(ui, i) ? "m_ov" : "m_ot", {0x17, float(y)}, a);
    ui.text_block(f, label, {73, float(y + 1), 369 - 73, 34}, {1, 1, 1, a}, 0, TextAlign::Left, TextOp::Diffuse);
}

// Section header row: OBJMENU fancybar at (0x67, y) with OBJMENU `text` centred in (0x67, y)-(0x12f, y + 0x24),
// Goudy12 Bold Black, black.
void row_header(Ui& ui, int i, int text, float a) {
    const int y = row_y(i);
    spr(ui, "OBJMENU", "fancybar", {0x67, float(y)}, a);
    ui_text_in_rect(ui, ui.font("Goudy12 Bold Black"), ui.text("OBJMENU", text), {0x67, y, 0x12f, y + 0x24}, {0, 0, 0, a});
}

// Row icon (40x40 at (0x1f, y - 3)) under OBJMENU box_bg.
void row_icon(Ui& ui, int i, std::string_view icon, float a) {
    const glm::vec2 p{0x1f, float(row_y(i) - 3)};
    spr(ui, "OBJMENU", icon, p, a);
    spr(ui, "OBJMENU", "box_bg", p, a);
}

// ---- the 3D map (View3D of the *_map.bm scene, FUN_0072d6f0 -> FUN_00731a60; pages 2, 3, 5) ----
// Camera as View3D_set_camera_params: look-at target, yaw (screen-up direction of a straight-down view), pitch
// (90 deg = straight down), distance; `focal` = view height / 2 / tan(fov / 2) = 384 / tan(22.5 deg) (fov param 1.0).
struct MapCamera {
    glm::vec3 target{0.0f};
    float yaw = 0.0f, pitch = 0.0f, dist = 600.0f, focal = 927.06f;
};

struct MapIcon {        // extra model on the map (map icons, commanders, the player cursor)
    std::string model;  // model/<name>.SMO
    glm::vec2 pos;      // map units (world * 200 / 16384); z = top of the map mesh under it (ray, FUN_005d4a80)
    float yaw = 0.0f;
};

struct MapRender {
    TextureHandle tex;
    glm::vec2 origin{0.0f};  // screen position of the image's top-left (camera at `target`, view centre `centre`)
    glm::ivec2 size{0};
};

// ponytail: UiQuad has no 3D, so the scene is rasterised on the CPU once per (scene, camera, icons) into a baked
// texture: perspective-correct UVs, z-buffer, per-vertex D3D lighting (light ambient x 1 + point lights x SMO diffuse
// (colour 1) x N.L, no attenuation inside Radius), nearest texel. Pans shift the image instead of re-rendering
// (relief parallax ignored); specular, fog, filtering and material flags of the icon models are not reproduced.
MapRender map_render(Ui& ui, const std::string& bm_path, const MapCamera& cam, glm::vec2 centre, const std::vector<MapIcon>& icons) {
    std::string key = bm_path;
    char buf[160];
    std::snprintf(buf, sizeof buf, "|%.3f|%.3f|%.3f|%.4f|%.4f|%.2f|%.2f", cam.target.x, cam.target.y, cam.target.z, cam.yaw, cam.pitch,
                  cam.dist, cam.focal);
    key += buf;
    for (const MapIcon& i : icons) {
        std::snprintf(buf, sizeof buf, "|%s@%.2f,%.2f,%.3f", i.model.c_str(), i.pos.x, i.pos.y, i.yaw);
        key += buf;
    }
    static std::map<std::string, std::pair<glm::vec2, glm::ivec2>> placement;  // ponytail: grows per distinct camera
    MapRender out;
    out.tex = ui.baked_texture(key, [&]() -> Image {
        const formats::Vfs& vfs = ui.vfs();
        const auto bm = formats::parse_block_text(text_of(vfs.read(bm_path)));
        if (!bm) return {};
        const glm::vec3 h{std::cos(cam.yaw), std::sin(cam.yaw), 0.0f};
        const glm::vec3 f{std::cos(cam.pitch) * h.x, std::cos(cam.pitch) * h.y, -std::sin(cam.pitch)};
        const glm::vec3 r{std::sin(cam.yaw), -std::cos(cam.yaw), 0.0f}, u = glm::cross(r, f);
        const glm::vec3 eye = cam.target - f * cam.dist;
        glm::vec3 ambient{0.0f};
        std::vector<std::pair<glm::vec3, glm::vec3>> lights;  // position, colour
        for (const formats::TextBlock& b : bm->blocks) {
            if (b.type != "CreateLight") continue;
            const glm::vec3 rgb = glm::vec3(float(b.number("Red")), float(b.number("Green")), float(b.number("Blue"))) / 255.0f;
            const std::string* t = b.get("light_type");
            if (t && *t == "ambient") ambient += rgb;
            else lights.push_back({{float(b.number("X")), float(b.number("Y")), float(b.number("Z"))}, rgb});
        }
        struct V { glm::vec2 s; float iz; glm::vec2 uv; glm::vec3 c; };
        struct T { V v[3]; const formats::DdsImage* img; };
        std::vector<T> tris;
        std::vector<std::array<glm::vec3, 3>> ground;  // map mesh triangles in world space (icon heights)
        std::map<std::string, formats::DdsImage> images;
        auto add_model = [&](const std::string& name, glm::vec3 pos, float yaw, float scale, bool is_ground) {
            formats::Mesh mesh;
            std::string err;
            const auto bytes = vfs.read("model/" + name + ".SMO");
            if (!bytes || !formats::parse_mesh(*bytes, false, mesh, err)) return;
            const float ch = std::cos(yaw), sh = std::sin(yaw);
            auto rot = [&](const float* v) { return glm::vec3(ch * v[0] - sh * v[1], sh * v[0] + ch * v[1], v[2]); };
            for (const formats::Submesh& sub : mesh.submeshes) {
                const formats::DdsImage* img = nullptr;
                if (sub.texture >= 0) {
                    std::string tn = lower(mesh.textures[std::size_t(sub.texture)].name);
                    tn = tn.substr(0, tn.rfind('.'));
                    auto [it, fresh] = images.try_emplace(tn);
                    if (fresh) {
                        formats::DdsTexture dds;
                        const auto tb = vfs.read("texture/" + tn + ".dds");
                        if (tb && formats::parse_dds(*tb, dds, err, 1) && !dds.images.empty()) it->second = std::move(dds.images[0]);
                    }
                    if (!it->second.rgba.empty()) img = &it->second;
                }
                const glm::vec3 diffuse{sub.colors[1].r, sub.colors[1].g, sub.colors[1].b};
                std::vector<V> vs;
                std::vector<glm::vec3> wps;
                for (const formats::MeshVertex& mv : sub.vertices) {
                    const glm::vec3 wp = pos + rot(mv.pos) * scale, n = glm::normalize(rot(mv.normal));
                    glm::vec3 c = ambient;
                    for (const auto& [lpos, lc] : lights) c += lc * diffuse * std::max(0.0f, glm::dot(n, glm::normalize(lpos - wp)));
                    const glm::vec3 d = wp - eye;
                    const float z = std::max(glm::dot(d, f), 1.0f);
                    vs.push_back({{cam.focal * glm::dot(d, r) / z, -cam.focal * glm::dot(d, u) / z}, 1.0f / z, {mv.uv[0], mv.uv[1]},
                                  glm::min(c, glm::vec3(1.0f))});
                    wps.push_back(wp);
                }
                const std::vector<std::uint32_t> idx = formats::strip_to_triangles(sub.strip);
                for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
                    tris.push_back({{vs[idx[i]], vs[idx[i + 1]], vs[idx[i + 2]]}, img});
                    if (is_ground) ground.push_back({wps[idx[i]], wps[idx[i + 1]], wps[idx[i + 2]]});
                }
            }
        };
        for (const formats::TextBlock& b : bm->blocks)
            if (const std::string* name = b.get("ModelName"); b.type == "CreateMapActor" && name)
                add_model(*name, {float(b.number("X")), float(b.number("Y")), float(b.number("Z"))}, glm::radians(float(b.number("HD"))),
                          float(b.number("Scale", 1.0)), true);
        for (const MapIcon& icon : icons) {
            float z = 0.0f;  // highest map surface under the icon
            for (const auto& g : ground) {
                const glm::vec2 a(g[0]), b(g[1]), c(g[2]), p = icon.pos;
                const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                if (std::abs(d) < 1e-9f) continue;
                const float w0 = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / d;
                const float w1 = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / d;
                if (w0 >= 0 && w1 >= 0 && w0 + w1 <= 1) z = std::max(z, w0 * g[0].z + w1 * g[1].z + (1 - w0 - w1) * g[2].z);
            }
            add_model(icon.model, {icon.pos, z}, icon.yaw, 1.0f, false);
        }
        if (tris.empty()) return {};
        glm::vec2 lo{1e9f}, hi{-1e9f};
        for (const T& t : tris)
            for (const V& v : t.v) lo = glm::min(lo, v.s), hi = glm::max(hi, v.s);
        lo = glm::max(glm::floor(lo), glm::vec2(-1200.0f)), hi = glm::min(glm::ceil(hi), glm::vec2(1200.0f));
        Image img;
        img.width = int(hi.x - lo.x) + 1, img.height = int(hi.y - lo.y) + 1;
        img.rgba.assign(std::size_t(img.width) * std::size_t(img.height) * 4, 0);
        std::vector<float> zbuf(std::size_t(img.width) * std::size_t(img.height), 0.0f);  // 1/z, bigger = nearer
        for (const T& t : tris) {
            glm::vec2 p[3];
            for (int k = 0; k < 3; ++k) p[k] = t.v[k].s - lo;
            const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
            if (std::abs(area) < 1e-6f) continue;  // both windings drawn
            const int x0 = std::max(0, int(std::floor(std::min({p[0].x, p[1].x, p[2].x}))));
            const int x1 = std::min(img.width - 1, int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))));
            const int y0 = std::max(0, int(std::floor(std::min({p[0].y, p[1].y, p[2].y}))));
            const int y1 = std::min(img.height - 1, int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const glm::vec2 q{float(x) + 0.5f, float(y) + 0.5f};
                    float w[3];
                    for (int k = 0; k < 3; ++k) {
                        const glm::vec2 a = p[(k + 1) % 3], b = p[(k + 2) % 3];
                        w[k] = ((b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y)) / area;
                    }
                    if (w[0] < 0 || w[1] < 0 || w[2] < 0) continue;
                    const float iz = w[0] * t.v[0].iz + w[1] * t.v[1].iz + w[2] * t.v[2].iz;
                    float& zb = zbuf[std::size_t(y) * std::size_t(img.width) + std::size_t(x)];
                    if (iz <= zb) continue;
                    zb = iz;
                    glm::vec3 c = w[0] * t.v[0].c + w[1] * t.v[1].c + w[2] * t.v[2].c;
                    if (t.img) {
                        const glm::vec2 uv = (w[0] * t.v[0].uv * t.v[0].iz + w[1] * t.v[1].uv * t.v[1].iz + w[2] * t.v[2].uv * t.v[2].iz) / iz;
                        const int tw = int(t.img->width), th = int(t.img->height);
                        const int tx = std::clamp(int((uv.x - std::floor(uv.x)) * float(tw)), 0, tw - 1);
                        const int ty = std::clamp(int((uv.y - std::floor(uv.y)) * float(th)), 0, th - 1);
                        const std::uint8_t* s = &t.img->rgba[(std::size_t(ty) * std::size_t(tw) + std::size_t(tx)) * 4];
                        c *= glm::vec3(s[0], s[1], s[2]) / 255.0f;
                    }
                    std::uint8_t* d = &img.rgba[(std::size_t(y) * std::size_t(img.width) + std::size_t(x)) * 4];
                    for (int k = 0; k < 3; ++k) d[k] = std::uint8_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f));
                    d[3] = 255;
                }
        }
        placement[key] = {lo, {img.width, img.height}};
        return img;
    });
    if (auto it = placement.find(key); it != placement.end()) out.origin = centre + it->second.first, out.size = it->second.second;
    return out;
}

// Draws the scene image shifted by `shift` px, clipped to `clip` (x, y, w, h).
void draw_map(Ui& ui, const MapRender& m, glm::vec2 shift, glm::vec4 clip) {
    if (!m.tex || m.size.x <= 0) return;
    const glm::vec2 o = m.origin + shift;
    const glm::vec2 p0 = glm::max(o, glm::vec2(clip.x, clip.y)), p1 = glm::min(o + glm::vec2(m.size), glm::vec2(clip.x + clip.z, clip.y + clip.w));
    if (p1.x <= p0.x || p1.y <= p0.y) return;
    ui.image(m.tex, {p0.x, p0.y, p1.x - p0.x, p1.y - p0.y}, (p0 - o) / glm::vec2(m.size), (p1 - o) / glm::vec2(m.size));
}

float map_yaw(const HudModel& m) { return glm::radians(90.0f - 90.0f * float(m.brief_rotation)); }

// Pages 2/3: straight-down camera at distance 600 over the map centre (FUN_00732af0 / 0x736f30), viewport x 48..1023
// (centre (536, 384)); hovering a linked row pans the target to the territory (`goal`, map units) at 15 units per
// frame per axis (0x7343b0 / 0x738340). ponytail: 15/frame is taken as 900/s (60 Hz).
void topdown_map(Ui& ui, HudModel& m, std::optional<glm::vec2> goal) {
    const glm::vec2 want = goal ? *goal - m.brief_map_centre : glm::vec2(0.0f);
    const float step = 900.0f * ui.dt();
    for (int k = 0; k < 2; ++k)
        m.brief_pan[k] = std::abs(want[k] - m.brief_pan[k]) <= step ? want[k] : m.brief_pan[k] + std::copysign(step, want[k] - m.brief_pan[k]);
    MapCamera cam;
    cam.target = {m.brief_map_centre, 0.0f};
    cam.yaw = map_yaw(m), cam.pitch = kPi / 2, cam.dist = 600.0f;
    const MapRender r = map_render(ui, m.brief_map, cam, {536, 384}, {});
    const float s = cam.focal / cam.dist;  // px per map unit at z = 0
    const glm::vec2 ax{std::sin(cam.yaw), -std::cos(cam.yaw)}, ay{std::cos(cam.yaw), std::sin(cam.yaw)};
    draw_map(ui, r, -s * glm::vec2(glm::dot(m.brief_pan, ax), -glm::dot(m.brief_pan, ay)), {48, 0, 976, 768});
}

const BriefTown* find_town(const HudModel& m, std::string_view name) {
    for (const BriefTown& t : m.brief_towns)
        if (lower(t.name) == lower(name)) return &t;
    return nullptr;
}

glm::vec2 town_map(const BriefTown& t) { return t.world / 16384.0f * 200.0f; }

void focus_fade(Ui& ui, HudModel& m, bool in) {
    m.brief_focus = std::clamp(m.brief_focus + (in ? 6.0f : -6.0f) * ui.dt(), 0.0f, 1.0f);
}

// Lower picture panel (SCR_FHBG + SCR_FH frame at (0x186,0x1e2)).
void panel_back(Ui& ui, float a) { spr(ui, "OBJMENU", "scr_fhbg", {0x186, 0x1e2}, a); }
void panel_frame(Ui& ui, float a) { spr(ui, "OBJMENU", "scr_fh", {0x186, 0x1e2}, a); }

// "%1,4d": thousands separated with commas.
std::string grouped(int v) {
    std::string s = std::to_string(v);
    for (int i = int(s.size()) - 3; i > (v < 0 ? 1 : 0); i -= 3) s.insert(std::size_t(i), ",");
    return s;
}

// ---- 1 Background (objmenub.cpp: open FUN_007246f0, draw FUN_007240f0, input FUN_00724210) ----
// Text widget 0x81: Goudy14(22) white wrapped in (0x106,0xd2)-(0x314,0x262), line i at 0xd2 + (i - scroll + 1) * 18
// (16 px font + 2 and the top line offset measured: first line at 300 for scroll -4), scroll from -4; OBJMENU
// "mask" (wood with a soft window, layer 6) hides it outside y ~290..560. Arrows aw_u (0x1de,0xc0) while
// scroll > -4, aw_d (0x1de,0x261) while scroll < lines - 10. Fade-in 4/s after 200 ms.
// FUN_007246f0 plays SE MISSION_BRIEF_<mission 1..8> (the narrated background) when it opens the page with the
// .OBJ file loaded. Gap: the 100 ms auto-repeat of a held arrow.
void background(Ui& ui, HudModel& m) {
    if (m.brief_mission >= 1 && m.brief_mission <= 8 && !m.brief_text.empty())
        ui.sound_at("MISSION_BRIEF_" + std::to_string(m.brief_mission), m.brief_time, 0.0f);
    page_back(ui, false);
    const UiFont* f = ui.font("Goudy14(22) white");
    const float a = ui_fade(m.brief_time, 0.2f, 4);
    const std::vector<std::string> lines = f ? wrap_text(f->metrics, m.brief_text, 0x314 - 0x106) : std::vector<std::string>{};
    const int n = int(lines.size());
    for (int i = std::max(m.brief_scroll, 0); i < n; ++i) {
        const int y = 0xd2 + (i - m.brief_scroll + 1) * 18;
        if (y + 18 > 0x262) break;
        ui.text(f, lines[std::size_t(i)], {0x106, float(y)}, {1, 1, 1, a}, TextAlign::Left, 1.0f, TextOp::Diffuse);
    }
    ui.sprite("OBJMENU", "mask", {0, 0});
    page_sphere(ui);
    page_ring(ui);
    if (m.brief_scroll > -4 && arrow(ui, "aw_u", {0x1de, 0xc0}, a)) --m.brief_scroll;
    if (m.brief_scroll < n - 10 && arrow(ui, "aw_d", {0x1de, 0x261}, a)) ++m.brief_scroll;
    if (page_front(ui, m, 4)) m.brief_page = 0;
}

// ---- 2 Objectives (open FUN_007353a0, rows FUN_00734a30, draw FUN_00732c70, input FUN_00734120, frame FUN_007343b0) ----
void objectives(Ui& ui, HudModel& m) {
    // Rows: sections Main (type 0, OBJMENU 17), Side (2, 18), Imperatives (3, 19), each with a header row when non-empty;
    // entry kind 4 pending (box_p), 5 passed (box_c), 6 failed (box_f; main objectives never show failed).
    struct Row { int kind, obj; };
    std::vector<Row> rows;
    for (int s = 0; s < 3; ++s) {
        const int type = s == 0 ? 0 : s + 1;
        bool header = false;
        for (std::size_t k = 0; k < m.brief_objectives.size(); ++k) {
            const BriefObjective& o = m.brief_objectives[k];
            if (o.type != type) continue;
            if (!header) rows.push_back({s + 1, -1}), header = true;
            rows.push_back({o.finish == 1 ? 5 : o.finish == 2 && type != 0 ? 6 : 4, int(k)});
        }
    }
    const float a = ui_fade(m.brief_time, 0.2f, 4);
    int hovered = -1;  // row slot under the mouse with an objective
    for (int i = 0; i < 10 && m.brief_scroll + i < int(rows.size()); ++i)
        if (rows[std::size_t(m.brief_scroll + i)].kind > 3 && row_hover(ui, i) && hovered < 0) hovered = i;
    const BriefObjective* ho = hovered >= 0 ? &m.brief_objectives[std::size_t(rows[std::size_t(m.brief_scroll + hovered)].obj)] : nullptr;
    const BriefTown* town = ho && !ho->territory.empty() ? find_town(m, ho->territory) : nullptr;
    if (ho && (ho->gold || ho->exp)) focus_fade(ui, m, true);
    else if (!town) focus_fade(ui, m, false);  // a linked entry without rewards keeps the panel as it was (0x7343b0)

    page_back(ui, true);
    page_sphere(ui);
    topdown_map(ui, m, town ? std::optional(town_map(*town)) : std::nullopt);
    page_ring(ui);
    const int step = list_frame(ui, m, int(rows.size()));
    const UiFont* f = ui.font("Goudy12 Bold Black");
    static constexpr const char* kBox[3] = {"box_p", "box_c", "box_f"};
    for (int i = 0; i < 10 && m.brief_scroll + i < int(rows.size()); ++i) {
        const Row& r = rows[std::size_t(m.brief_scroll + i)];
        if (r.kind <= 3) {
            row_header(ui, i, 16 + r.kind, a);
        } else {
            row_bar(ui, i, m.brief_objectives[std::size_t(r.obj)].text, f, a);
            row_icon(ui, i, kBox[r.kind - 4], a);
        }
    }
    const float fh = m.brief_focus;
    if (fh > 0) panel_back(ui, fh), panel_frame(ui, fh);
    if (ho) {
        if (fh > 0) {
            // Rewards: "REWARDS:" (OBJMENU 39) + goldbar; gold row (reward_b, goldicon) then exp row (expicon) at
            // y 0x212 / 0x246 (DAT_009df47c); amounts in goudy_bold_italic_24 at x 0x2e6, "(rewarded)" OBJMENU 40.
            ui.text(ui.font("Goudy14(22) white"), ui.text("OBJMENU", 39), {0x268, 0x1f0}, {1, 1, 1, fh}, TextAlign::Left, 1.0f, TextOp::Diffuse);
            spr(ui, "OBJMENU", "goldbar", {0x268, 0x20a}, fh);
            int y = 0x212;
            const UiFont* big = ui.font("goudy_bold_italic_24");
            for (int k = 0; k < 2; ++k) {
                const int v = k == 0 ? ho->gold : ho->exp;
                if (!v) continue;
                spr(ui, "OBJMENU", "reward_b", {0x25a, float(y)}, fh);
                if (k == 0) spr(ui, "OBJMENU", "goldicon", {0x266, float(y + 4)}, fh);
                else spr(ui, "OBJMENU", "expicon", {0x25e, float(y + 10)}, fh);
                const std::string s = ho->awarded ? replace(std::string(ui.text("OBJMENU", 40)), "%1,4d", grouped(v)) : std::to_string(v);
                ui.text(big, s, {0x2e6, float(y + 10)}, {1, 1, 1, fh});
                y = 0x246;
            }
        }
        if (town) {
            static constexpr const char* kFocus[3] = {"fc_pend", "fc_pass", "fc_fail"};
            spr(ui, "OBJMENU", kFocus[rows[std::size_t(m.brief_scroll + hovered)].kind - 4], {0x1cc, 0x134}, 1.0f);
            if (!town->picture.empty()) spr(ui, "OBJMENU", town->picture, {399, 0x1eb}, fh);
        }
    }
    m.brief_scroll += step;
    if (page_front(ui, m, 5)) m.brief_page = 0;
}

// ---- 3 Townships (open FUN_00738b50, rows FUN_00738920, draw FUN_007370b0, input FUN_007380b0, frame FUN_00738340) ----
// Rows = territories in file order: label (Goudy12 Bold Black, white), icon <side>_<type> (f_/e_/i_ + camp / town /
// castle; church and forest = i_other). Hover only: focus circle fc_fre/fc_eng/fc_neu at (0x1cc,0x134), camera pan
// to the territory, picture (ScrName) at (399,491) in the SCR_FH panel (fade 6/s).
void townships(Ui& ui, HudModel& m) {
    const int n = int(m.brief_towns.size());
    int hovered = -1;
    for (int i = 0; i < 10 && m.brief_scroll + i < n; ++i)
        if (row_hover(ui, i) && hovered < 0) hovered = m.brief_scroll + i;
    if (hovered >= 0) m.brief_select = hovered;
    focus_fade(ui, m, hovered >= 0);
    page_back(ui, true);
    page_sphere(ui);
    topdown_map(ui, m, hovered >= 0 ? std::optional(town_map(m.brief_towns[std::size_t(hovered)])) : std::nullopt);
    page_ring(ui);
    const int step = list_frame(ui, m, n);
    const float a = ui_fade(m.brief_time, 0.2f, 4);
    const UiFont* f = ui.font("Goudy12 Bold Black");
    static constexpr const char* kSide[3] = {"f_", "e_", "i_"};
    static constexpr const char* kType[6] = {"camp", "town", "castle", "castle", "other", "other"};
    for (int i = 0; i < 10 && m.brief_scroll + i < n; ++i) {
        const BriefTown& t = m.brief_towns[std::size_t(m.brief_scroll + i)];
        const int type = std::clamp(t.type, 0, 5);
        row_bar(ui, i, t.name, f, a);
        row_icon(ui, i, type >= 4 ? std::string("i_other") : std::string(kSide[std::clamp(t.side, 0, 2)]) + kType[type], a);
    }
    const float fh = m.brief_focus;
    if (hovered >= 0) {
        static constexpr const char* kFocus[3] = {"fc_fre", "fc_eng", "fc_neu"};
        spr(ui, "OBJMENU", kFocus[std::clamp(m.brief_towns[std::size_t(hovered)].side, 0, 2)], {0x1cc, 0x134}, fh);
    }
    if (fh > 0) {
        panel_back(ui, fh);
        if (m.brief_select >= 0 && m.brief_select < n && !m.brief_towns[std::size_t(m.brief_select)].picture.empty())
            spr(ui, "OBJMENU", m.brief_towns[std::size_t(m.brief_select)].picture, {399, 491}, fh);
        panel_frame(ui, fh);
    }
    m.brief_scroll += step;
    if (page_front(ui, m, 6)) m.brief_page = 0;
}

// ---- 4 Commanders (open FUN_00728a90, rows FUN_00728390, draw FUN_00725870, input FUN_007277a0) ----
// unit.res type code -> OBJMENU support icon and type name (OBJMENU 29..34).
std::pair<std::string, int> support_icon(std::string_view code) {
    struct E { const char* code; const char* icon; int text; };
    static constexpr E kTable[] = {
        {"FRENCH_ARCHER", "f_arch", 33},        {"FRENCH_LIGHT_INFANTRY", "f_linf", 29}, {"FRENCH_HEAVY_INFANTRY", "f_hinf", 30},
        {"FRENCH_POLEMAN", "f_pole", 31},       {"FRENCH_MACEMAN", "f_mace", 32},        {"FRENCH_CROSSBOW_MAN", "f_cross", 34},
        {"ENGLISH_ARCHER", "e_arch", 33},       {"ENGLISH_EX_ARCHER", "e_arch", 33},     {"ENGLISH_LIGHT_INFANTRY", "e_linf", 29},
        {"ENGLISH_HEAVY_INFANTRY", "e_hinf", 30}, {"ENGLISH_POLEMAN", "e_pole", 31},     {"ENGLISH_MACEMAN", "e_mace", 32},
        {"ENGLISH_CROSSBOW_MAN", "e_cross", 34}, {"BUR_LIGHTINFANTRY", "b_linf", 29},    {"BUR_HEAVYINFANTRY", "b_hinf", 30},
        {"BUR_ARCHER", "b_arch", 33},           {"MER_LIGHT_INFANTRY", "m_linf", 29},    {"MER_POLEMAN", "m_pole", 31},
        {"MER_ARCHER", "m_arch", 33},           {"MER_EX_ARCHER", "m_arch", 33},
    };
    for (const E& e : kTable)
        if (upper(code) == e.code) return {e.icon, e.text};
    return {"", 0};  // unknown type: the original leaves stale buffers; nothing is drawn here
}

void commanders(Ui& ui, HudModel& m) {
    // Virtual list: FRENCH (OBJMENU 23) / ENGLISH (22) / MERCENARY (24) headers + units; header indices as
    // 0x728390 computes them (En/Me ignore an empty French group, which leaves a blank row 0 then).
    struct Row { int kind = 0, unit = -1; };  // kind 1..3 header French/English/Merc, 4..6 unit row of that nation
    int count[3]{};
    for (const BriefCommander& c : m.brief_commanders)
        if (c.nation >= 0 && c.nation < 3) ++count[c.nation];
    const int head[3] = {0, count[0] + 1, count[0] + 1 + (count[1] > 0 ? count[1] + 1 : 0)};
    std::vector<Row> rows;
    for (int g = 0; g < 3; ++g) {
        if (!count[g]) continue;
        int at = head[g];
        auto put = [&](Row r) { if (int(rows.size()) <= at) rows.resize(std::size_t(at) + 1); rows[std::size_t(at++)] = r; };
        put({g + 1, -1});
        for (std::size_t k = 0; k < m.brief_commanders.size(); ++k)
            if (m.brief_commanders[k].nation == g) put({g + 4, int(k)});
    }
    const int total = count[0] + count[1] + count[2] + (count[0] > 0) + (count[1] > 0) + (count[2] > 0);
    const float t = m.brief_time, a = ui_fade(t, 0.2f, 4);
    if (a >= 1.0f)  // rows react once fully faded in
        for (int i = 0; i < 10 && m.brief_scroll + i < int(rows.size()); ++i) {
            const Row& r = rows[std::size_t(m.brief_scroll + i)];
            if (r.kind > 3 && ui.clicked({0x17, float(row_y(i)), 0x17f - 0x17 + 1, 0x28})) m.brief_select = r.unit;
        }
    focus_fade(ui, m, m.brief_select >= 0);
    const float fh = m.brief_focus, slide = 1024.0f - ui_slide(t, 0, 474);  // IMG_F / TXT_FH move to x 0x226
    const UiFont* f12 = ui.font("Goudy12 Bold Black");
    const UiFont* f14 = ui.font("Goudy14(22) white");

    page_back(ui, false);
    page_sphere(ui);
    page_ring(ui);
    spr(ui, "OBJMENU", "txt_fv", {-356.0f + ui_slide(t, 0, 381), 0xb8}, ui_fade(t, 0, 6));
    if (fh > 0) {
        spr(ui, "OBJMENU", "txt_fh", {slide, 0x1e2}, fh);
        ui_text_in_rect(ui, f12, ui.text("OBJMENU", 21), {int(slide), 0x1e2, int(slide) + 355, 0x1e2 + 0x28}, {1, 1, 1, fh});
    }
    int step = 0;
    if (m.brief_scroll > 0 && arrow(ui, "aw_u", {0xa9, 0xc2}, a)) step = -1;
    if (m.brief_scroll < total - 10 && arrow(ui, "aw_d", {0xa9, 0x26c}, a)) step = 1;
    static constexpr const char* kIcon[3] = {"icon_f", "icon_e", "icon_m"};
    static constexpr int kHeader[3] = {23, 22, 24};
    for (int i = 0; i < 10 && m.brief_scroll + i < int(rows.size()); ++i) {
        const Row& r = rows[std::size_t(m.brief_scroll + i)];
        if (r.kind >= 4) row_bar(ui, i, m.brief_commanders[std::size_t(r.unit)].name, f14, a), row_icon(ui, i, kIcon[r.kind - 4], a);
        else if (r.kind > 0) row_header(ui, i, kHeader[r.kind - 1], a);
    }
    if (m.brief_select >= 0 && m.brief_select < int(m.brief_commanders.size())) {
        const BriefCommander& c = m.brief_commanders[std::size_t(m.brief_select)];
        static constexpr const char* kFlag[3] = {"bg_f", "bg_e", "bg_m"};
        spr(ui, "OBJMENU", kFlag[std::clamp(c.nation, 0, 2)], {0x23a, 0xca}, fh);
        if (c.team.empty()) {
            ui_text_in_rect(ui, f14, ui.text("OBJMENU", c.hostile ? 25 : 20), {0x226, 0x1f6, 0x38a, 0x228}, {1, 1, 1, fh});
        } else {
            // One icon per unit from (0x238,0x200), 30 px apart, second line at y 0x232 after ten; hp bar 20 px wide.
            glm::vec2 p{0x238, 0x200};
            for (std::size_t k = 0; k < c.team.size() && k < 20; ++k) {
                if (k == 10) p = {0x238, 0x232};
                const auto [icon, name] = support_icon(c.team[k].type);
                if (!icon.empty()) {
                    const bool hot = ui.hover({p.x, p.y, 0x1e, 0x32});
                    const float ia = fh * (hot ? 1.0f : 0.5f);
                    spr(ui, "OBJMENU", icon, p, ia);
                    spr(ui, "OBJMENU", "hp_bg", {p.x + 0xd, p.y + 0x2c}, ia);
                    if (const UiSprite* hp = ui.sprite("OBJMENU", "hp")) {
                        const float w = std::floor(std::clamp(c.team[k].hp, 0.0f, 1.0f) * 20.0f);
                        ui.image(hp->texture, {p.x + 0xf, p.y + 0x2e, w, 4}, hp->uv0, {hp->uv0.x + (hp->uv1.x - hp->uv0.x) * w / 20.0f, hp->uv1.y},
                                 {1, 1, 1, ia});
                    }
                    if (hot) {
                        ui.text(f12, ui.text("OBJMENU", name), {0x276, 0x26e}, {1, 1, 1, fh}, TextAlign::Left, 1.0f, TextOp::Diffuse);
                        const std::string lv = replace(std::string(ui.text("OBJMENU", 28)), "%1d", std::to_string(c.team[k].level));
                        ui.text(f12, lv, {0x33a + 1, 0x26e}, {1, 1, 1, fh}, TextAlign::Right, 1.0f, TextOp::Diffuse);
                    }
                }
                p.x += 0x1e;
            }
        }
        // Gap: the commander's 3D actor (View3D (571,203)-(886,449), F_IDLE, yaw 150 deg) is not rendered.
    }
    if (fh > 0) spr(ui, "OBJMENU", "img_f", {slide, 0xb6}, fh);  // drawn last (FUN_00727740, after pass 2)
    if (step) m.brief_scroll += step, m.brief_select = -1, m.brief_focus = 0.0f;
    if (page_front(ui, m, 7)) m.brief_page = 0;
}

// ---- 5 Battleground (open FUN_00730be0, 2D draw FUN_0072e480, input FUN_00730150) ----
// 3D (default): the map scene + icon models, camera at the map centre, yaw 90 - 90 rot, pitch 40 deg, fovy 45 deg,
// distance fitted so the map corner lands inside 680x424 px (FUN_0072d6f0, clamp 250..1000), full-screen viewport
// without colour clear. 2D: the minimap tiles of <scenario>W scaled by the zoom step (FUN_0072e480).
// Toggle (387,664)-(487,694) "2D VIEW" / "3D VIEW" (OBJMENU 26/27); BACK moves to x 0x219.
// Gaps: mouse drag / wheel camera control and 2D zoom; icon rotation in 2D.
float fit_distance(const HudModel& m, const MapCamera& base) {
    // Corner (x1, y1) for rot 0 (x1,y2 rot 1; x2,y2 rot 2; x2,y1 rot 3) projected at distance 1000.
    const glm::vec2 c = m.brief_map_centre;
    const int rot = m.brief_rotation & 3;
    const glm::vec2 corner{rot >= 2 ? 2 * c.x : 0.0f, rot == 1 || rot == 2 ? 2 * c.y : 0.0f};
    MapCamera cam = base;
    cam.dist = 1000.0f;
    const glm::vec3 h{std::cos(cam.yaw), std::sin(cam.yaw), 0.0f};
    const glm::vec3 f{std::cos(cam.pitch) * h.x, std::cos(cam.pitch) * h.y, -std::sin(cam.pitch)};
    const glm::vec3 r{std::sin(cam.yaw), -std::cos(cam.yaw), 0.0f}, u = glm::cross(r, f);
    const glm::vec3 d = glm::vec3(corner, 0.0f) - (cam.target - f * cam.dist);
    const float z = glm::dot(d, f);
    const float sx = std::trunc(cam.focal * glm::dot(d, r) / z), sy = std::trunc(-cam.focal * glm::dot(d, u) / z);
    return std::clamp(std::max(2 * std::abs(sx) * z / 680.0f, 2 * std::abs(sy) * z / 424.0f) + 1000.0f - z, 250.0f, 1000.0f);
}

void battleground_2d(Ui& ui, const HudModel& m) {
    const std::string table = m.scenario + "W";
    auto count = [&](int level, bool rows) {
        int n = 0;
        char name[32];
        for (;; ++n) {
            std::snprintf(name, sizeof name, "z%dr%02dc%02d", level, rows ? n + 1 : 1, rows ? 1 : n + 1);
            if (!ui.has_sprite(table, name)) return n;
        }
    };
    const int cols = std::max(count(1, false), 1), rows = std::max(count(1, true), 1);
    // Initial zoom: s = min(680 / map px, 424 / map px) -> zoom = trunc(2 s + 1) (1..5) -> s = 0.25 | (zoom - 1) / 2.
    const float fit = std::min(680.0f / float(cols * 200), 424.0f / float(rows * 200));
    const int zoom = std::clamp(fit > 0.25f ? int(2 * fit + 1) : 1, 1, 5);
    const float s = zoom == 1 ? 0.25f : float(zoom - 1) * 0.5f;
    const int level = s < 1.0f ? 0 : 1;  // (tw1 + tw0) / 2 < tw1 / s with 200 px tiles at both levels
    const glm::vec2 size{float(cols) * 200 * s, float(rows) * 200 * s};
    const glm::vec2 o{std::trunc(512 - size.x / 2), std::trunc(384 - size.y / 2) + 1};
    char name[32];
    std::snprintf(name, sizeof name, "z%db", level);
    if (const UiSprite* b = ui.sprite(table, name)) {  // inner (1,1)-(w-2,h-2) stretched over the four outside bands
        const glm::vec2 d = (b->uv1 - b->uv0) / b->size;
        const glm::vec2 uv0 = b->uv0 + d, uv1 = b->uv1 - d;
        for (const glm::vec4 r : {glm::vec4{0, 0, 1024, o.y}, {0, o.y + size.y, 1024, 768 - o.y - size.y}, {0, o.y, o.x, size.y},
                                  {o.x + size.x, o.y, 1024 - o.x - size.x, size.y}})
            if (r.z > 0 && r.w > 0) ui.image(b->texture, r, uv0, uv1);
    }
    const int tc = level ? cols : 1, tr = level ? rows : 1;
    for (int r = 0; r < tr; ++r)
        for (int c = 0; c < tc; ++c) {
            std::snprintf(name, sizeof name, "z%dr%02dc%02d", level, r + 1, c + 1);
            ui.sprite(ui.sprite(table, name), glm::vec4{o.x + size.x / float(tc) * float(c), o.y + size.y / float(tr) * float(r),
                                                        size.x / float(tc), size.y / float(tr)});
        }
    auto at = [&](glm::vec2 w) { return glm::vec2{o.x + w.x / m.map_size.x * size.x, o.y + size.y - w.y / m.map_size.y * size.y}; };
    for (const HudMapIcon& i : m.map_icons) {
        const UiSprite* s2 = i.dot == HudMapIcon::Dot::None ? ui.sprite("MAPICON", i.sprite) : nullptr;
        if (!s2) continue;
        const glm::vec2 p = at(i.world);
        const std::string n = lower(i.sprite);
        if (n.size() > 2 && n[0] == '!' && (n.ends_with("_h") || n.ends_with("_m"))) ui.sprite("MAPICON", "!outbox", glm::floor(p - glm::vec2(12)));
        ui.sprite(s2, glm::floor(p - s2->size / 2.0f));
    }
}

void battleground(Ui& ui, HudModel& m) {
    page_back(ui, false);
    if (m.brief_2d) battleground_2d(ui, m);
    page_sphere(ui);
    if (!m.brief_2d) {
        MapCamera cam;
        cam.target = {m.brief_map_centre, 0.0f};
        cam.yaw = map_yaw(m), cam.pitch = glm::radians(40.0f);
        cam.dist = fit_distance(m, cam);
        std::vector<MapIcon> icons;  // MODEL\<icon> at world * 200 / 16384 (0x730be0); the player as "Cursor"
        for (const HudMapIcon& i : m.map_icons)
            if (i.dot == HudMapIcon::Dot::None && !i.sprite.empty()) icons.push_back({i.sprite, i.world * (200.0f / 16384.0f), 0.0f});
        icons.push_back({"Cursor", m.player_world * (200.0f / 16384.0f), m.player_heading});
        draw_map(ui, map_render(ui, m.brief_map, cam, {512, 384}, icons), {0, 0}, {0, 0, 1024, 768});
    }
    page_ring(ui);
    const int hit = page_front(ui, m, 8, 0x219, m.brief_2d ? 27 : 26, 0x183);
    if (hit == 1) m.brief_page = 0;
    if (hit == 2) m.brief_2d = !m.brief_2d;
}

// ---- 8 Strategic Command (open FUN_007365e0, rows FUN_00736520, draw FUN_00735930, input StrategyHelp_process) ----
// 16 topics OBJMENU 45..60 (10 rows, top 0..6) -> des/strategic_<n>.txt in a frame-only G_POPUP box at (0x184,0xb8)
// 611x468: title (Goudy_bold_20, centred in (0x1b0,0xc4)-(0x3ba,0x110)), GPOPUP goldbar, STRATEGY ST<n>_<k> pictures
// at (0x1b0, 0x11c + 0x84 k) (count table DAT_009df500) and the text from (0x234, 0x11c), clipped at y 0x2a3:
// "/T" goudy_12, "/S" goudy_bold_14, "/N" goudy_italic_12, "~" = a goudy_12 line + 2; '_' toggles red (255,51,51).
// Key names (%1s.. in topics 1 and 2: RTS mode / zoom / rotate actions 23, 48..51) use the default bindings.
void gpopup_frame(Ui& ui, glm::vec4 r) {  // 9-slice GPOPUP frame, edges tiled (0x633500 clip extent)
    auto tile = [&](const char* n, glm::vec4 q) {
        const UiSprite* s = ui.sprite("GPOPUP", n);
        if (!s || q.z <= 0 || q.w <= 0) return;
        for (float oy = 0; oy < q.w; oy += s->size.y)
            for (float ox = 0; ox < q.z; ox += s->size.x) {
                const glm::vec2 part{std::min(s->size.x, q.z - ox), std::min(s->size.y, q.w - oy)};
                ui.image(s->texture, {q.x + ox, q.y + oy, part.x, part.y}, s->uv0, s->uv0 + (s->uv1 - s->uv0) * part / s->size);
            }
    };
    const float x = r.x, y = r.y, w = r.z, h = r.w;
    tile("frame_c", {x + 48, y + 48, w - 96, h - 96});
    tile("frame_tl", {x, y, 48, 48}), tile("frame_t", {x + 48, y, w - 96, 48}), tile("frame_tr", {x + w - 48, y, 48, 48});
    tile("frame_l", {x, y + 48, 48, h - 96}), tile("frame_r", {x + w - 48, y + 48, 48, h - 96});
    tile("frame_bl", {x, y + h - 48, 48, 48}), tile("frame_b", {x + 48, y + h - 48, w - 96, 48}), tile("frame_br", {x + w - 48, y + h - 48, 48, 48});
}

// Word-wrapped '_'-markup text (0x626320 with markup): returns the number of lines drawn from (x, y), step lh + 2.
int markup_text(Ui& ui, const UiFont* f, std::string_view s, glm::vec2 pos, int width, float clip_y, glm::vec4 color) {
    if (!f) return 0;
    auto plain = [](std::string_view t) { std::string r; for (char c : t) if (c != '_') r += c; return r; };
    std::vector<std::string> lines;
    std::string line;
    for (std::size_t i = 0; i <= s.size();) {
        const std::size_t j = std::min(s.find(' ', i), s.size());
        const std::string word(s.substr(i, j - i)), next = line.empty() ? word : line + " " + word;
        if (!line.empty() && f->metrics.measure(plain(next)) > width) lines.push_back(line), line = word;
        else line = next;
        i = j + 1;
    }
    if (!line.empty()) lines.push_back(line);
    const glm::vec4 red = ui_rgba(255, 51, 51, int(color.a * 255));
    bool hot = false;
    for (std::size_t l = 0; l < lines.size(); ++l) {
        const float y = pos.y + float(l) * float(f->metrics.line_height() + 2);
        float x = pos.x;
        std::string seg;
        auto flush = [&] { if (y + float(f->metrics.line_height()) <= clip_y) x += float(ui.text(f, seg, {x, y}, hot ? red : color, TextAlign::Left, 1.0f, TextOp::Diffuse)); seg.clear(); };
        for (char c : lines[l]) {
            if (c == '_') { flush(); hot = !hot; continue; }
            seg += c;
        }
        flush();
    }
    return int(lines.size());
}

void strategy(Ui& ui, HudModel& m) {
    page_back(ui, false);
    page_sphere(ui);
    page_ring(ui);
    const int step = list_frame(ui, m, 16, 6);
    const float a = ui_fade(m.brief_time, 0.2f, 4);
    const UiFont* f = ui.font("Goudy12 Bold Black");
    for (int i = 0; i < 10; ++i) {
        row_bar(ui, i, ui.text("OBJMENU", 45 + m.brief_scroll + i), f, a);
        if (a >= 1.0f && ui.clicked({0x17, float(row_y(i)), 0x17f - 0x17 + 1, 0x28})) m.brief_select = m.brief_scroll + i, m.brief_doc.clear();
    }
    if (m.brief_select >= 0 && m.brief_select < 16) {
        const int sel = m.brief_select;
        if (m.brief_doc.empty()) m.brief_doc = text_of(ui.vfs().read("des/strategic_" + std::to_string(sel + 1) + ".txt"));
        gpopup_frame(ui, {0x184, 0xb8, 0x263, 0x1d4});
        const UiFont* title = ui.font("Goudy_bold_20");
        if (title) {
            const std::string s(ui.text("OBJMENU", 45 + sel));
            const auto lines = wrap_text(title->metrics, s, 0x3ba - 0x1b0);
            const float y0 = 0xc4 + std::floor((0x110 - 0xc4 - float(lines.size()) * float(title->metrics.line_height())) / 2);
            for (std::size_t l = 0; l < lines.size(); ++l)
                ui.text(title, lines[l], {(0x1b0 + 0x3ba) / 2.0f, y0 + float(l) * float(title->metrics.line_height())}, kUiWhite, TextAlign::Center,
                        1.0f, TextOp::Diffuse);
        }
        if (const UiSprite* g = ui.sprite("GPOPUP", "goldbar"))
            ui.image(g->texture, {0x1b0, 0x110, 0x20b, 8}, g->uv0, {g->uv0.x + (g->uv1.x - g->uv0.x) * 0x20b / g->size.x, g->uv1.y});
        static constexpr int kPictures[16] = {2, 2, 1, 1, 1, 2, 1, 2, 1, 2, 1, 1, 2, 2, 1, 1};  // DAT_009df500
        for (int k = 0; k < kPictures[sel]; ++k)
            ui.sprite("STRATEGY", "ST" + std::to_string(sel + 1) + "_" + std::to_string(k + 1), {0x1b0, float(0x11c + 0x84 * k)});
        std::string doc = m.brief_doc;
        if (sel <= 1) {
            const MenuModel defaults;
            const int actions[4] = {sel == 0 ? 23 : 48, 49, 50, 51};
            for (int k = 0; k < (sel == 0 ? 1 : 4); ++k)
                doc = replace(doc, "%" + std::to_string(k + 1) + "s", action_key_name(ui.vfs(), defaults, actions[k]));
        }
        const UiFont* body = ui.font("goudy_12");
        const UiFont* cur = ui.font("goudy_bold_14");
        float y = 0x11c;
        std::size_t p = 0;
        while (p <= doc.size()) {
            const std::size_t e = std::min(doc.find('\n', p), doc.size());
            const std::string_view line = std::string_view(doc).substr(p, e - p);
            p = e + 1;
            if (!line.empty() && line[0] == '~') {
                y += float((body ? body->metrics.line_height() : 12) + 2);
            } else if (line.size() >= 2 && line[0] == '/') {
                const char t = char(std::tolower(static_cast<unsigned char>(line[1])));
                cur = ui.font(t == 'n' ? "goudy_italic_12" : t == 's' ? "goudy_bold_14" : "goudy_12");
                if (cur) y += float(markup_text(ui, cur, line.substr(2), {0x234, y}, 0x3ba - 0x234, 0x2a3, kUiWhite) * (cur->metrics.line_height() + 2) - 2);
            }
        }
    }
    m.brief_scroll += step;
    if (page_front(ui, m, 44)) m.brief_page = 0;
}

// ---- 9 Siege Weapon (open FUN_0073b160, draw FUN_00739540, input SiegeMenu_show_help 0x73a320) ----
// Six fixed slots (ram, crossbow, bombard, cannon, fauconneau, trebuchet) at x 121 + 134 i, y 0x218: mb_siege icon at
// (x+5, y+5) under DEBRIEF sm_f / sm_fh (hover), alpha 1 selected / 0.4 otherwise, only for weapons present. The
// selection shows DEBRIEF frame (0x1bc,0xb4), fancybar + upper-cased name, the des text, Attack / Defense / Mobility
// pips (att_r / att_y / att_b), Light Infantry crew icons and the crew lines OBJMENU 67/68 + 69..73. HELP
// (0x219,0x298) opens SIEGE WEAPONS HELP (des/siege_help.txt) with a CLOSE button; BACK moves to x 0x183.
// Gap: the rotating 3D weapon model (View3D (104,197)-(443,556), 20 deg/s) is not rendered.
void siege(Ui& ui, HudModel& m) {
    static constexpr const char* kIcon[6] = {"batt", "cros", "bomb", "cann", "fauc", "treb"};
    static constexpr int kLine[6] = {69, 0, 70, 71, 72, 73};
    const UiFont* f12 = ui.font("Goudy12 Bold Black");
    const UiFont* f14 = ui.font("Goudy14(22) white");
    page_back(ui, false);  // page 9 has no black quad (FUN_0071f690 case 9)
    page_sphere(ui);
    page_ring(ui);
    for (int i = 0; i < 6; ++i) {
        if (!m.brief_weapons[std::size_t(i)]) continue;
        const glm::vec2 p{float(121 + 134 * i), 0x218};
        const bool hot = !m.brief_help && ui.hover({p.x, p.y, 104, 104});
        const float a = i == m.brief_select ? 1.0f : 0.4f;
        spr(ui, "OBJMENU", kIcon[i], p + glm::vec2(5), a);
        spr(ui, "DEBRIEF", hot ? "sm_fh" : "sm_f", p, a);
        if (hot && ui.clicked({p.x, p.y, 104, 104})) m.brief_select = i;
    }
    if (m.brief_select >= 0 && m.brief_select < 6) {
        const BriefWeapon& w = m.brief_weapon_info[std::size_t(m.brief_select)];
        ui.sprite("DEBRIEF", "frame", {0x1bc, 0xb4});
        ui.sprite("DEBRIEF", "fancybar", {0x210, 0xa4});
        ui_text_in_rect(ui, f12, upper(w.name), {0x210, 0xa4, 0x330, 0xcc}, {0, 0, 0, 1});
        if (f12) {
            const auto lines = wrap_text(f12->metrics, w.text, 0x356 - 0x1d0);
            const float lh = float(f12->metrics.line_height());
            const float y0 = 0xcc + std::floor((0x186 - 0xcc - float(lines.size()) * lh) / 2);
            for (std::size_t l = 0; l < lines.size(); ++l)
                ui.text(f12, lines[l], {(0x1d0 + 0x356) / 2.0f, y0 + float(l) * lh}, kInk, TextAlign::Center, 1.0f, TextOp::Diffuse);
        }
        ui.sprite("DEBRIEF", "goldbar", {0x1e8, 0x188});
        ui.sprite("DEBRIEF", "goldbar", {0x1e8, 0x1b4});
        const int labels[3] = {64, 65, 66}, lx[3] = {0x1e8, 0x270, 0x308}, px[3] = {0x1e4, 0x270, 0x2fe}, pips[3] = {w.attack, w.defense, w.mobility};
        static constexpr const char* kPip[3] = {"att_r", "att_y", "att_b"};
        for (int k = 0; k < 3; ++k) {
            ui.text(f14, ui.text("OBJMENU", labels[k]), {float(lx[k]), 0x18d}, kInk, TextAlign::Left, 1.0f, TextOp::Diffuse);
            for (int j = 0; j < std::min(pips[k], 5); ++j) ui.sprite("OBJMENU", kPip[k], {float(px[k] + 18 * j), 0x1a1});
        }
        for (int k = 0; k < w.crew; ++k) ui.sprite("OBJMENU", "linf", {float(0x1e8 + 0x2d * k), 0x1b0});
        const std::string crew = w.crew == 1 ? std::string(ui.text("OBJMENU", 67)) : replace(std::string(ui.text("OBJMENU", 68)), "%1d", std::to_string(w.crew));
        ui.text(f12, crew, {0x1d1, 0x1e2}, kInk, TextAlign::Left, 1.0f, TextOp::Diffuse);
        if (kLine[m.brief_select]) ui_text_in_rect(ui, f12, ui.text("OBJMENU", kLine[m.brief_select]), {0x1d1, 0x1f0, 0x370, 0x1fe}, kInk);
    }
    const int hit = page_front(ui, m, 63, 0x183, 75, 0x219, !m.brief_help);
    if (hit == 1) m.brief_page = 0;
    if (hit == 2) m.brief_help = true;
    else if (m.brief_help && ui_text_popup(ui, ui.text("OBJMENU", 74), text_of(ui.vfs().read("des/siege_help.txt")), 0x25f))
        m.brief_help = false;
}

}  // namespace

// G_POPUP box (FUN_006d3cb0 with flags 4, body font 0xa5d3c8, title font 0xa47d88): title (goudy_bold_14) + goldbar,
// body (at most 0x3ff chars, Goudy12 Bold Black), goldbar, CLOSE (G_POPUP 3); the box grows to fit the text (no
// scrolling): height = 0x30 + title + 0x14 + text + 0x14 + 0x24, centred.
bool ui_text_popup(Ui& ui, std::string_view title, std::string_view body, float w) {
    const UiFont* tf = ui.font("goudy_bold_14");
    const UiFont* f12 = ui.font("Goudy12 Bold Black");
    const UiFont* f14 = ui.font("Goudy14(22) white");
    const float inner = w - 0x60;
    const auto tl = tf ? wrap_text(tf->metrics, title, int(inner)) : std::vector<std::string>{};
    const auto bl = f12 ? wrap_text(f12->metrics, body.substr(0, 0x3ff), int(inner)) : std::vector<std::string>{};
    const float th = tf ? float(tl.size()) * float(tf->metrics.line_height()) : 0, lh = f12 ? float(f12->metrics.line_height() + 2) : 0;
    const float h = std::max(96.0f, 0x30 + th + 0x14 + float(bl.size()) * lh + 0x14 + 0x24);
    const float x = std::floor((1024 - w) / 2), y = std::floor((768 - h) / 2);
    gpopup_frame(ui, {x, y, w, h});
    for (std::size_t l = 0; l < tl.size(); ++l)
        ui.text(tf, tl[l], {x + w / 2, y + 0x18 + float(l) * float(tf->metrics.line_height())}, kUiWhite, TextAlign::Center, 1.0f, TextOp::Diffuse);
    ui.sprite("GPOPUP", "goldbar", {x + 0x30, y + 0x18 + th + 8});
    for (std::size_t l = 0; l < bl.size(); ++l)
        ui.text(f12, bl[l], {x + 0x30, y + 0x18 + th + 0x14 + float(l) * lh}, kUiWhite, TextAlign::Left, 1.0f, TextOp::Diffuse);
    ui.sprite("GPOPUP", "goldbar", {x + 0x30, y + h - 0x48});
    const glm::vec4 b{x + (w - 172) / 2, y + h - 0x3c, 172, 36};
    const bool hot = ui.hover(b);
    ui.sprite("GPOPUP", hot && ui.input().mouse_down ? "b_dw" : hot ? "b_ov" : "b_ot", {b.x, b.y});
    ui_text_in_rect(ui, f14, ui.text("G_POPUP", 3), {int(b.x), int(b.y), int(b.x) + 171, int(b.y) + 35}, kUiWhite);
    return (hot && ui.clicked(b)) || ui.key(SDL_SCANCODE_ESCAPE) || ui.key(SDL_SCANCODE_RETURN);
}

void hud_brief(Ui& ui, HudModel& m) {
    if (m.brief_mission != m.mission) brief_content(ui.vfs(), m.mission, m);
    ui.frame(Anchor::Center);
    const int page = m.brief_page;
    switch (page) {
        case 1: background(ui, m); break;
        case 2: objectives(ui, m); break;
        case 3: townships(ui, m); break;
        case 4: commanders(ui, m); break;
        case 5: battleground(ui, m); break;
        case 8: strategy(ui, m); break;
        case 9: siege(ui, m); break;
        default: m.brief_page = 0; break;
    }
    if (m.brief_page != page) {  // FUN_00721a60 close: page state back to its opening values
        m.brief_time = 0.0f, m.brief_select = -1, m.brief_focus = 0.0f, m.brief_pan = {0, 0}, m.brief_help = false;
        m.brief_scroll = m.brief_page == 1 ? -4 : 0;
    } else {
        m.brief_time += ui.dt();
    }
}

void brief_content(const formats::Vfs& vfs, int mission, HudModel& m) {
    m.brief_mission = mission;
    char path[40];
    std::snprintf(path, sizeof path, "scenario/scenario_%02d.scr", mission);
    const auto def = formats::parse_scenario_def(text_of(vfs.read(path)));
    if (!def || def->steps.empty()) return;
    const formats::ScenarioStep& step = def->steps.front();
    // FUN_007246f0: "SCRIPT\SCENARIO\" + ObjectiveFileName + ".OBJ", read whole.
    m.brief_text = text_of(vfs.read("script/scenario/" + step.objective_file + ".obj"));
    // FUN_0072d6f0: "SCRIPT\SCENARIO\" + ObjectiveMapFileName + ".BM"; bounds = union of the actor boxes (X+X1..X+X2).
    m.brief_map = "script/scenario/" + step.objective_map_file + ".bm";
    if (const auto bm = formats::parse_block_text(text_of(vfs.read(m.brief_map)))) {
        glm::vec2 lo{1e9f}, hi{-1e9f};
        for (const formats::TextBlock& b : bm->blocks)
            if (b.type == "CreateMapActor") {
                const glm::vec2 p{float(b.number("X")), float(b.number("Y"))};
                lo = glm::min(lo, p + glm::vec2(float(b.number("X1")), float(b.number("Y1"))));
                hi = glm::max(hi, p + glm::vec2(float(b.number("X2")), float(b.number("Y2"))));
            }
        if (lo.x <= hi.x) m.brief_map_centre = (lo + hi) / 2.0f;
    }
    // Territories (CreateTerritory blocks of map/<WorldFileName>.ter, parser FUN_004f9330), file order.
    m.brief_towns.clear();
    if (const auto ter = formats::parse_block_text(text_of(vfs.read("map/" + step.world_file + ".ter")))) {
        static constexpr const char* kTypes[6] = {"camp", "town", "city", "castle", "church", "forest"};
        for (const formats::TextBlock& b : ter->blocks) {
            if (b.type != "CreateTerritory") continue;
            BriefTown t;
            if (const std::string* s = b.get("TerritoryName")) t.name = *s;
            if (const std::string* s = b.get("ScrName")) t.picture = *s;
            if (const std::string* s = b.get("Type"))
                for (int k = 0; k < 6; ++k)
                    if (lower(*s) == kTypes[k]) t.type = k;
            const std::string kingdom = b.get("Kingdom") ? lower(*b.get("Kingdom")) : "";
            t.side = kingdom == "french" ? 0 : kingdom == "english" || kingdom == "mercen" ? 1 : 2;
            t.world = {float(b.number("CoordinateX")), float(b.number("CoordinateY"))};
            m.brief_towns.push_back(std::move(t));
        }
    }
    // Siege weapons: unit/unit.res records + des/<weapon>.txt (0x739540 / 0x73a320). Pips: Attack = Dexterity / 5
    // (no SRangeWeaponId) or / 20, Defense = Defense / 5, Mobility = RunSpeed, each 0..5 (loops from 1 in steps).
    auto res_bytes = vfs.read("unit/unit.res");
    const auto res = res_bytes ? formats::ResArchive::parse(std::move(*res_bytes)) : std::nullopt;
    static constexpr const char* kCode[6] = {"BATTERRAM", "CROSSBOW", "BOMBARD", "CANNON", "FAUCONNEAU", "TREBUCHET"};
    static constexpr const char* kDes[6] = {"battering_ram", "crossbow", "bombard", "cannon", "fauconneau", "trebuchet"};
    for (int i = 0; i < 6; ++i) {
        BriefWeapon& w = m.brief_weapon_info[std::size_t(i)];
        w = {};
        w.text = text_of(vfs.read(std::string("des/") + kDes[i] + ".txt"));
        const auto text = res ? res->text(kCode[i]) : std::nullopt;
        if (!text) continue;
        auto field = [&, kv = formats::parse_kv_text(*text)](std::string_view k) {
            for (const formats::KvLine& l : kv)
                if (lower(l.key) == lower(k)) return std::string(formats::trim_ws(l.value));
            return std::string();
        };
        auto num = [&](std::string_view k) { return std::atof(field(k).c_str()); };
        auto pips = [](double v, int step) { int n = 0; for (int x = 1; x <= int(v) && n < 5; x += step) ++n; return n; };
        w.name = field("Name");
        const std::string ammo = lower(field("SRangeWeaponId"));
        w.attack = pips(num("Dexterity"), ammo.empty() || ammo == "none" ? 5 : 20);
        w.defense = pips(num("Defense"), 5);
        w.mobility = std::min(int(num("RunSpeed")), 5);
        w.crew = int(num("AttachPointCount"));
    }
}

}  // namespace joa::engine
