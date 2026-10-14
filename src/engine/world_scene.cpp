#include "engine/world_scene.hpp"

#include "engine/camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace joa::engine {
namespace {

float srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
glm::vec3 srgb_to_linear(glm::vec3 c) { return {srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b)}; }

std::optional<std::string> read_text(const formats::Vfs& vfs, const std::string& path) {
    const auto b = vfs.read(path);
    if (!b) return std::nullopt;
    return std::string(b->begin(), b->end());
}

std::optional<formats::BlockText> read_blocks(const formats::Vfs& vfs, const std::string& path) {
    const auto t = read_text(vfs, path);
    return t ? formats::parse_block_text(*t) : std::nullopt;
}

double to_number(std::string_view s, double fallback = 0.0) {
    try {
        return std::stod(std::string(s));
    } catch (...) {
        return fallback;
    }
}

// .lgt light record (map/common.lgt etc.). Type 1 = directional, 0 = ambient (HYPOTHESIS from values:
// type 0 always has zero directions).
struct LgtLight {
    std::string stage;
    int type = -1;
    bool on = true;
    glm::vec3 rgb{0.0f};  // 0..255
    float hdir = 0, vdir = 0;  // degrees
};

std::vector<LgtLight> parse_lgt(std::string_view text) {
    std::vector<LgtLight> out;
    for (const auto& kv : formats::parse_kv_text(text)) {
        const std::string k = lower(kv.key);
        if (k == "ison") {
            out.emplace_back().on = to_number(kv.value) != 0;
            continue;
        }
        if (out.empty()) continue;
        LgtLight& l = out.back();
        if (k == "stagename") l.stage = kv.value;
        else if (k == "type") l.type = int(to_number(kv.value, -1));
        else if (k == "red") l.rgb.r = float(to_number(kv.value));
        else if (k == "green") l.rgb.g = float(to_number(kv.value));
        else if (k == "blue") l.rgb.b = float(to_number(kv.value));
        else if (k == "hdir") l.hdir = float(to_number(kv.value));
        else if (k == "vdir") l.vdir = float(to_number(kv.value));
    }
    return out;
}

// Gribb-Hartmann frustum planes (xyz normal, w distance), normals point inside.
std::array<glm::vec4, 6> frustum_planes(const glm::mat4& m) {
    const glm::mat4 t = glm::transpose(m);
    std::array<glm::vec4, 6> p{t[3] + t[0], t[3] - t[0], t[3] + t[1], t[3] - t[1], t[3] + t[2], t[3] - t[2]};
    for (auto& v : p) v /= glm::length(glm::vec3(v));
    return p;
}

bool sphere_visible(const std::array<glm::vec4, 6>& planes, glm::vec3 c, float r) {
    for (const auto& p : planes)
        if (glm::dot(glm::vec3(p), c) + p.w < -r) return false;
    return true;
}

}  // namespace

std::optional<ScenarioPaths> find_scenario(const formats::Vfs& vfs, std::string_view s) {
    std::string arg(s);
    std::replace(arg.begin(), arg.end(), '\\', '/');
    if (arg.find('/') != std::string::npos) {
        std::filesystem::path p(arg);
        if (p.is_absolute()) p = p.lexically_relative(vfs.root());
        ScenarioPaths r{p.parent_path().generic_string(), p.stem().string()};
        if (vfs.exists(r.dir + "/" + r.name + ".gri")) return r;
        return std::nullopt;
    }
    if (const auto dot = arg.rfind('.'); dot != std::string::npos) arg.resize(dot);
    const auto map_dir = vfs.resolve("map");
    if (!map_dir) return std::nullopt;
    std::vector<std::string> subdirs;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(*map_dir, ec))
        if (e.is_directory()) subdirs.push_back(e.path().filename().string());
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto& d : subdirs) {
        ScenarioPaths r{"map/" + d, arg};
        if (vfs.exists(r.dir + "/" + arg + ".gri") && vfs.exists(r.dir + "/" + arg + ".stg")) return r;
    }
    return std::nullopt;
}

WorldScene::WorldScene(AssetCache& assets, std::string_view scenario, float clock_minutes) : assets_(assets) {
    const auto& vfs = assets.vfs();
    const auto paths = find_scenario(vfs, scenario);
    if (!paths) throw std::runtime_error("scenario not found: " + std::string(scenario));
    const std::string base = paths->dir + "/" + paths->name;
    const auto gri_bytes = vfs.read(base + ".gri");
    std::string err;
    auto grid = gri_bytes ? formats::parse_gri(*gri_bytes, &err) : std::nullopt;
    if (!grid) throw std::runtime_error(base + ".gri: " + (gri_bytes ? err : "missing"));
    grid_ = std::move(*grid);
    const auto stg = read_blocks(vfs, base + ".stg");
    if (!stg) throw std::runtime_error(base + ".stg: missing or malformed");

    load_lighting(*paths, clock_minutes);
    load_terrain(*paths, *stg);
    load_objects(*stg);
    load_start(*paths);
}

WorldScene::~WorldScene() {
    IRenderer& r = assets_.renderer();
    r.destroy_mesh(terrain_mesh_);
    r.destroy_mesh(water_mesh_);
    r.destroy_texture(lightmap_);
}

void WorldScene::load_lighting(const ScenarioPaths& p, float clock) {
    const auto& vfs = assets_.vfs();
    // Day cycle of the world (scenario_0N.scr WorldFileName = <map>w, e.g. S1L1w.day: Normal_06..
    // Normal_Night, StartTime/Duration in minutes after midnight); the per-map S1L1.day (three
    // Special_Night stages) is only the fallback. Stage = the one running at `clock`.
    for (const std::string& f : {p.dir + "/" + p.name + "w.day", p.dir + "/" + p.name + ".day"}) {
        const auto day = read_text(vfs, f);
        if (!day) continue;
        std::string name, first;
        float start = 0;
        for (const auto& kv : formats::parse_kv_text(*day)) {
            const std::string k = lower(kv.key);
            if (k == "stagename") name = kv.value, first = first.empty() ? name : first;
            else if (k == "starttime") start = float(to_number(kv.value));
            else if (k == "duration") {
                const float end = start + float(to_number(kv.value));
                if ((clock >= start && clock < end) || (clock + 1440.0f >= start && clock + 1440.0f < end)) stats_.stage = name;
            }
        }
        if (stats_.stage.empty()) stats_.stage = first;
        if (!stats_.stage.empty()) break;
    }
    // Stage lights: the map's own .lgt first (S1L1.lgt is empty), then map/common.lgt, map/special.lgt.
    std::optional<LgtLight> sun, ambient;
    for (const std::string& f : {p.dir + "/" + p.name + ".lgt", std::string("map/common.lgt"), std::string("map/special.lgt")}) {
        const auto t = read_text(vfs, f);
        if (!t) continue;
        for (const LgtLight& l : parse_lgt(*t)) {
            if (!l.on || lower(l.stage) != lower(stats_.stage)) continue;
            if (l.type == 1 && !sun) sun = l;
            if (l.type == 0 && !ambient) ambient = l;
        }
        if (sun || ambient) break;
    }
    // .sca scattering colour of the stage (fog HYPOTHESIS below).
    glm::vec3 sca{0.336f, 0.494f, 0.49f};
    if (const auto t = read_text(vfs, "map/common.sca")) {
        bool in = false;
        for (const auto& kv : formats::parse_kv_text(*t)) {
            const std::string k = lower(kv.key);
            if (k == "stagename") in = lower(kv.value) == lower(stats_.stage);
            else if (in && k == "red") sca.r = float(to_number(kv.value));
            else if (in && k == "green") sca.g = float(to_number(kv.value));
            else if (in && k == "blue") sca.b = float(to_number(kv.value));
        }
    }

    Lighting& l = lighting_;
    const glm::vec3 sun_rgb = sun ? sun->rgb / 255.0f : glm::vec3(0.6f);
    const glm::vec3 amb_rgb = ambient ? ambient->rgb / 255.0f : glm::vec3(0.25f);
    // HYPOTHESIS: VDir = elevation, HDir = azimuth CCW from +X (degrees), light shines downward.
    const float v = glm::radians(sun ? sun->vdir : 60.0f), h = glm::radians(sun ? sun->hdir : 0.0f);
    l.sun_direction = -to_engine(std::cos(v) * std::cos(h), std::cos(v) * std::sin(h), std::sin(v));
    // Fixed-function lighting in gamma space: tex * (ambient + diffuseMaterial * sun * NdotL), where
    // nearly every material diffuse is 150/255 (MODELS_AND_ANIMATION.md); material ambient UNKNOWN (1).
    l.sun_color = srgb_to_linear(sun_rgb * (150.0f / 255.0f));
    l.ambient = srgb_to_linear(amb_rgb);
    // HYPOTHESIS: the engine's Rayleigh/Mie scattering is approximated by linear fog towards the stage
    // scattering colour; range matched by eye to the original S1L1 capture (s04_04_mission1_hud.png).
    l.fog_color = srgb_to_linear(sca);
    l.fog_start = 300.0f;
    l.fog_end = 6000.0f;
    l.clear_color = glm::vec4(l.fog_color, 1.0f);

    // Sky: the .day SkyMesh (model/sky01.smo) carries sunset textures (sky01..04); the engine tints it
    // through its scattering model (UNKNOWN). The original noon capture shows plain blue-grey haze,
    // so the clear colour (= fog colour) stands in for the sky; clouds (CloudTexture1/2) are a gap.
}

void WorldScene::load_terrain(const ScenarioPaths& p, const formats::BlockText& stg) {
    // Sector lightmaps texture/terrain/<map>_scene_sector_<N>.dds as one atlas: sector (ix, iy) at
    // column ix, row (rows-1-iy). The engine maps a sector's lightmap with u = (x-x0)/size,
    // v = 1-(y-y0)/size (vertex build in FUN_0055b?? world_s.cpp, decompile line ~444000), so the
    // atlas is addressed with u = x/cells_x, v = 1 - y/cells_y.
    struct Sector { int ix, iy; std::string id; };
    std::vector<Sector> sectors;
    int nsx = 0, nsy = 0;
    for (const auto& b : stg.blocks) {
        if (b.type != "CreateSceneSector") continue;
        const std::string* id = b.get("scene_id");
        if (!id) continue;
        Sector s{int(b.number("index_x")), int(b.number("index_y")), *id};
        nsx = std::max(nsx, s.ix + 1);
        nsy = std::max(nsy, s.iy + 1);
        sectors.push_back(s);
    }
    if (!sectors.empty()) {
        int tile = 0;
        std::vector<std::uint8_t> atlas;
        for (const Sector& s : sectors) {
            formats::DdsImage img = assets_.texture_pixels(p.name + "_" + s.id);
            if (img.rgba.empty()) img = assets_.texture_pixels("lightmap_default");
            if (img.rgba.empty() || img.width != img.height) continue;
            if (!tile) {
                tile = int(img.width);
                atlas.assign(std::size_t(nsx * tile) * nsy * tile * 4, 255);
            }
            if (int(img.width) != tile) continue;
            ++stats_.lightmaps;
            const int row0 = (nsy - 1 - s.iy) * tile, col0 = s.ix * tile;
            for (int y = 0; y < tile; ++y)
                std::copy_n(img.rgba.data() + std::size_t(y) * tile * 4, tile * 4,
                            atlas.data() + (std::size_t(row0 + y) * nsx * tile + col0) * 4);
        }
        if (tile) {
            TextureData t;
            t.width = nsx * tile;
            t.height = nsy * tile;
            t.pixels = std::move(atlas);
            t.repeat = false;
            lightmap_ = assets_.renderer().create_texture(t);
        }
    }

    // One vertex per grid node per (tex1, tex2) group; a cell takes its textures from its
    // lower-left node (engine: node+0x10 / node+0x30 names). Texturing per CONFIRMED stage setup
    // (case 0x11/0x13 at decompile ~523200): tex1, then BLENDDIFFUSEALPHA with tex2, alpha =
    // 255 - node byte 4; then * lightmap (uv1, clamp); then * vertex colour (node bytes 1,2,3 = R,G,B).
    // Ground UV repeats every header_c (4) cells: u from y, v from x (both decreasing).
    const auto& g = grid_;
    const int vx = g.verts_x(), vy = g.verts_y();
    auto tex_name = [&](std::uint16_t i) -> std::string {
        if (i == 0 || i > g.textures.size()) return {};
        const std::string n = lower(g.textures[i - 1].name);
        return n == "none" ? std::string{} : n;
    };
    std::map<std::pair<std::string, std::string>, std::vector<int>> groups;  // -> cell ids
    for (int x = 0; x < g.cells_x; ++x)
        for (int y = 0; y < g.cells_y; ++y) {
            if (!g.valid(x, y) || !g.valid(x + 1, y) || !g.valid(x, y + 1) || !g.valid(x + 1, y + 1)) continue;
            groups[{tex_name(g.tex1(x, y)), tex_name(g.tex2(x, y))}].push_back(x * g.cells_y + y);
        }
    const float repeat = float(std::max(g.header_c, 1));
    MeshData mesh;
    std::vector<std::int32_t> remap(std::size_t(vx) * vy);
    for (const auto& [names, cells] : groups) {
        std::fill(remap.begin(), remap.end(), -1);
        const bool two = !names.second.empty();
        auto vert = [&](int x, int y) -> std::uint32_t {
            std::int32_t& r = remap[std::size_t(x) * vy + y];
            if (r >= 0) return std::uint32_t(r);
            const auto h = [&](int ix, int iy) { return float(g.height(std::clamp(ix, 0, vx - 1), std::clamp(iy, 0, vy - 1))); };
            const auto& n = g.node(x, y);
            Vertex v;
            v.position = to_engine(x * formats::TerrainGrid::kCellSize, y * formats::TerrainGrid::kCellSize, h(x, y));
            const glm::vec3 nz{(h(x - 1, y) - h(x + 1, y)) / (2 * formats::TerrainGrid::kCellSize),
                               (h(x, y - 1) - h(x, y + 1)) / (2 * formats::TerrainGrid::kCellSize), 1.0f};
            v.normal = glm::normalize(to_engine(nz.x, nz.y, nz.z));
            v.uv = {float(g.cells_y - y) / repeat, float(g.cells_x - x) / repeat};
            v.uv2 = {float(x) / float(g.cells_x), 1.0f - float(y) / float(g.cells_y)};
            v.color = glm::vec4(srgb_to_linear(glm::vec3(n[1], n[2], n[3]) / 255.0f), two ? float(255 - n[4]) / 255.0f : 1.0f);
            r = std::int32_t(mesh.vertices.size());
            mesh.vertices.push_back(v);
            return std::uint32_t(r);
        };
        const auto first = std::uint32_t(mesh.indices.size());
        for (const int c : cells) {
            const int x = c / g.cells_y, y = c % g.cells_y;
            const std::uint32_t a = vert(x, y), b = vert(x + 1, y), cc = vert(x + 1, y + 1), d = vert(x, y + 1);
            // Diagonal alternates with (x + y) parity exactly like the engine's quad split.
            if (((x + y) & 1) == 0) mesh.indices.insert(mesh.indices.end(), {a, b, cc, a, cc, d});
            else mesh.indices.insert(mesh.indices.end(), {a, b, d, d, b, cc});
        }
        Material m;
        if (const TextureInfo* t = assets_.texture(names.first)) m.albedo = t->handle;
        if (two)
            if (const TextureInfo* t = assets_.texture(names.second)) m.layer2 = t->handle;
        m.lightmap = lightmap_;
        m.unlit = true;  // lighting is baked into the lightmap and vertex colours
        terrain_draws_.push_back({{}, m, glm::mat4(1.0f), first, std::uint32_t(mesh.indices.size()) - first});
    }
    stats_.terrain_triangles = int(mesh.indices.size() / 3);
    stats_.terrain_groups = int(terrain_draws_.size());
    terrain_mesh_ = assets_.renderer().create_mesh(mesh);
    for (auto& d : terrain_draws_) d.mesh = terrain_mesh_;

    // CreateWater: rectangles WIDTH (x) x LENGTH (y) at Z. HYPOTHESIS: X,Y is the centre; the
    // bump/env-mapped surface (Reflection, TextureWidth) is approximated by a translucent tint.
    MeshData water;
    for (const auto& b : stg.blocks) {
        if (b.type != "CreateWater") continue;
        const float x = float(b.number("X")), y = float(b.number("Y")), z = float(b.number("Z"));
        const float hw = float(b.number("WIDTH")) * 0.5f, hl = float(b.number("LENGTH")) * 0.5f;
        const auto base = std::uint32_t(water.vertices.size());
        for (const glm::vec2 c : {glm::vec2(-hw, -hl), glm::vec2(hw, -hl), glm::vec2(hw, hl), glm::vec2(-hw, hl)}) {
            Vertex v;
            v.position = to_engine(x + c.x, y + c.y, z);
            water.vertices.push_back(v);
        }
        water.indices.insert(water.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        ++stats_.water;
    }
    if (!water.indices.empty()) {
        water_mesh_ = assets_.renderer().create_mesh(water);
        Material m;
        m.tint = glm::vec4(lighting_.fog_color * 0.7f, 0.6f);
        m.blend = BlendMode::AlphaBlend;
        m.unlit = true;
        m.two_sided = true;
        water_draws_.push_back({water_mesh_, m, glm::mat4(1.0f)});
    }
}

void WorldScene::load_objects(const formats::BlockText& stg) {
    const auto& vfs = assets_.vfs();
    for (const auto& b : stg.blocks) {
        if (b.type != "CreateObject") continue;
        ++stats_.objects;
        const std::string* file = b.get("ObjectFile");
        const std::string* dir = b.get("Path");
        if (!file || !dir) continue;
        // Path is relative to ITEM\ ("Object\Models_Items\...\"). The item .pro names the model file.
        const std::string base = "item/" + *dir;
        std::string model_file = *file + ".SMO";
        if (const auto pro = read_text(vfs, base + *file + ".pro")) {
            for (const auto& kv : formats::parse_kv_text(*pro))
                if (lower(kv.key) == "model" && !kv.value.empty()) model_file = kv.value;
        } else if (!vfs.exists(base + model_file) && vfs.exists(base + *file + ".AM1")) {
            model_file = *file + ".AM1";
        }
        const std::string stem = model_file.substr(0, model_file.rfind('.'));
        std::string anim;
        if (lower(model_file).ends_with(".am1"))
            anim = vfs.exists(base + "anim/" + stem + ".am2") ? base + "anim/" + stem + ".am2" : "anim/" + stem + ".am2";
        const Model* m = assets_.model(base + model_file, anim);
        if (!m) {
            ++stats_.missing_models;
            const std::string miss = base + model_file;
            if (std::find(stats_.missing.begin(), stats_.missing.end(), miss) == stats_.missing.end()) stats_.missing.push_back(miss);
            continue;
        }
        // HD: degrees about +Z, same sense as unit headings (HYPOTHESIS for objects, checked visually
        // on wall/fence runs). VD (always 0 in S1L1) and WIDTH/LENGTH/HEIGHT (collision box) unused.
        const float scale = float(b.number("Scale", 1.0));
        glm::mat4 t = glm::translate(glm::mat4(1.0f), to_engine(float(b.number("X")), float(b.number("Y")), float(b.number("Z"))));
        t = glm::rotate(t, glm::radians(float(b.number("HD"))), glm::vec3(0, 1, 0));
        t = glm::scale(t, glm::vec3(scale));
        objects_.push_back({m, t, glm::vec3(t * glm::vec4(m->center, 1.0f)), m->radius * scale});
        ++stats_.placed;
    }
}

void WorldScene::load_start(const ScenarioPaths& p) {
    const auto& vfs = assets_.vfs();
    // Scripted placements in script/scenario/<map>.sce:
    //   var = FUNC("load_unit"|"add_unit", "CODE", ...)   FUNC("set_unit_pos", var, x, y, z)
    //   FUNC("set_unit_dir", var, degrees)
    struct Pending { std::string code; std::optional<glm::vec2> pos; float dir = 0; bool has_dir = false; };
    std::map<std::string, Pending> vars;
    auto trim = [](std::string s) {
        const auto a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string{} : s.substr(a, b - a + 1);
    };
    if (const auto sce = read_text(vfs, "script/scenario/" + p.name + ".sce")) {
        std::size_t pos = 0;
        while (pos < sce->size()) {
            const std::size_t end = std::min(sce->find('\n', pos), sce->size());
            std::string line(std::string_view(*sce).substr(pos, end - pos));
            pos = end + 1;
            if (const auto c = line.find("//"); c != std::string::npos) line.resize(c);
            const std::string l = lower(line);
            if (l.find("\"load_unit\"") != std::string::npos || l.find("\"add_unit\"") != std::string::npos) {
                const auto eq = l.find('='), q1 = l.find('"', l.find(',')), q2 = l.find('"', q1 + 1);
                if (eq == std::string::npos || q1 == std::string::npos || q2 == std::string::npos) continue;
                vars[trim(l.substr(0, eq))].code = line.substr(q1 + 1, q2 - q1 - 1);
                continue;
            }
            const bool is_pos = l.find("\"set_unit_pos\"") != std::string::npos, is_dir = l.find("\"set_unit_dir\"") != std::string::npos;
            if (!is_pos && !is_dir) continue;
            std::vector<std::string> args;
            for (std::size_t a = l.find(','); a != std::string::npos;) {
                const std::size_t b = l.find_first_of(",)", a + 1);
                args.push_back(trim(l.substr(a + 1, b == std::string::npos ? b : b - a - 1)));
                a = b != std::string::npos && l[b] == ',' ? b : std::string::npos;
            }
            // First placement wins (later script events move units during play).
            Pending& u = vars[args.empty() ? std::string{} : args[0]];
            if (args.size() >= 3 && is_pos && !u.pos) u.pos = glm::vec2(to_number(args[1]), to_number(args[2]));
            if (args.size() >= 2 && is_dir && !u.has_dir) u.dir = float(to_number(args[1])), u.has_dir = true;
        }
    }
    float x = grid_.cells_x * formats::TerrainGrid::kCellSize * 0.5f, y = grid_.cells_y * formats::TerrainGrid::kCellSize * 0.5f;
    float dir = 0;
    bool found = false;
    for (const auto& [var, u] : vars) {
        if (u.code.empty() || !u.pos) continue;
        units_.push_back({u.code, to_engine(u.pos->x, u.pos->y, grid_.height_at(u.pos->x, u.pos->y)), glm::radians(u.dir)});
        if (lower(u.code) == "joan_of_arc") x = u.pos->x, y = u.pos->y, dir = u.dir, found = true;
    }
    if (!found)
        if (const auto pla = read_blocks(vfs, p.dir + "/" + p.name + ".pla"))
            for (const auto& b : pla->blocks)
                if (const std::string* n = b.get("PLACE_NAME"); n && lower(*n) == "startpointplace") {
                    x = float(b.number("X")), y = float(b.number("Y"));
                    break;
                }
    start_ = to_engine(x, y, grid_.height_at(x, y));
    start_yaw_ = glm::radians(dir);
}

void WorldScene::build(const glm::mat4& view_proj, glm::vec3 camera_pos, std::vector<DrawItem>& draws) const {
    draws.insert(draws.end(), terrain_draws_.begin(), terrain_draws_.end());
    draws.insert(draws.end(), water_draws_.begin(), water_draws_.end());
    const auto planes = frustum_planes(view_proj);
    const float far2 = kDefaultFar * kDefaultFar;
    for (const PlacedObject& o : objects_) {
        const glm::vec3 d = o.center - camera_pos;
        if (glm::dot(d, d) > far2 + o.radius * o.radius * 4.0f || !sphere_visible(planes, o.center, o.radius)) continue;
        for (const ModelPart& part : o.model->parts) draws.push_back({o.model->mesh, part.material, o.transform, part.first_index, part.index_count});
    }
}

}  // namespace joa::engine
