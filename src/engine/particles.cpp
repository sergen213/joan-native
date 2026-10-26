#include "engine/particles.hpp"

#include "engine/world_scene.hpp"
#include "formats/script_particle.hpp"
#include "formats/world_text.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <cstdio>

namespace joa::engine {
namespace {

using formats::PsProperty;
using formats::PsScalar;

float srgb_to_linear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }

std::optional<std::string> read_text(const formats::Vfs& vfs, const std::string& path) {
    const auto b = vfs.read(path);
    if (!b) return std::nullopt;
    return std::string(b->begin(), b->end());
}

// Misc_random_float(a, b): uniform between the two bounds in either order.
float pick(std::minstd_rand& r, const PsScalar& s) {
    return s.lo + (s.hi - s.lo) * std::uniform_real_distribution<float>(0.0f, 1.0f)(r);
}

glm::vec3 pick3(std::minstd_rand& r, const std::array<PsScalar, 3>& v) {
    return to_engine(pick(r, v[0]), pick(r, v[1]), pick(r, v[2]));
}

glm::mat3 yaw(float radians) { return glm::mat3(glm::rotate(glm::mat4(1.0f), radians, glm::vec3(0, 1, 0))); }

}  // namespace

// One pscript compiled for simulation. Sequence field offsets in joan.exe (FUN_00678320 order):
// LifeTime +1, EmitRate +9, EmitRadius +0x31, Gravity +0x49, FollowCamera +0x69, Orientation +0x6a,
// OriR +0x76, Offset +0x7a, TimeDelay +0x86, SelfRotation +0x8a, SelfRotAxis +0x8b, SelfRotSpeed
// +0x97, Has_Rotation +0x9f, RChange +0x123, TexturePeriod +0x124, TextureNumber +0x128,
// NumParticles +0x134, Loop +0x138, IsSmo +0x13c.
struct ParticleSystem::Def {
    struct Event {
        enum Kind { Size, Color, Velocity } kind = Size;
        float time = 0;  // seconds of particle age; Initial = 0
        bool fade = false;
        std::vector<PsScalar> v;
        int next = -1;  // following event of the same kind, if it is a fade (its target)
    };
    struct Seq {
        PsScalar life, rate, spin;
        std::array<PsScalar, 3> radius{}, gravity{};
        glm::vec3 offset{0.0f};
        int mode = 1;  // FollowCamera
        glm::vec3 orientation{0.0f}, spin_axis{0.0f};
        bool self_rotation = false;
        const TextureInfo* texture = nullptr;
        int frames = 1;
        float period = 0;
        bool random_frame = false;
        std::size_t max_particles = 0;
        bool once = false;  // Loop != 0: emit NumParticles in total, then stop
        float delay = 0;
        std::vector<Event> events;
    };
    BlendMode blend = BlendMode::Additive;
    std::vector<Seq> seqs;
};

namespace {

using Def = ParticleSystem::Def;

std::array<PsScalar, 3> vec3_of(const PsProperty& p) {
    std::array<PsScalar, 3> v{};
    for (std::size_t i = 0; i < 3 && i < p.value.c.size(); ++i) v[i] = p.value.c[i];
    return v;
}
glm::vec3 fixed3(const PsProperty& p) {
    const auto v = vec3_of(p);
    return to_engine(v[0].lo, v[1].lo, v[2].lo);
}
float num(const PsProperty& p) { return p.value.c.empty() ? 0.0f : p.value.c[0].lo; }

Def::Seq compile(AssetCache& assets, const formats::PsSequence& s) {
    Def::Seq q;
    for (const PsProperty& p : s.props) {
        const std::string n = lower(p.name);
        if (p.stage != PsProperty::Stage::Base) {
            Def::Event e;
            if (n == "size") e.kind = Def::Event::Size;
            else if (n == "color") e.kind = Def::Event::Color;
            else if (n == "velocity") e.kind = Def::Event::Velocity;
            else continue;
            e.fade = p.stage == PsProperty::Stage::Fade;
            e.time = e.fade ? p.fade_at : 0.0f;
            e.v = p.value.c;
            q.events.push_back(std::move(e));
            continue;
        }
        if (p.value.c.empty() && n != "texture") continue;
        if (n == "lifetime") q.life = p.value.c[0];
        else if (n == "emitrate") q.rate = p.value.c[0];
        else if (n == "emitradius") q.radius = vec3_of(p);
        else if (n == "gravity") q.gravity = vec3_of(p);
        else if (n == "offset") q.offset = fixed3(p);
        else if (n == "followcamera") q.mode = int(num(p));
        else if (n == "orientation") q.orientation = fixed3(p);
        else if (n == "selfrotation") q.self_rotation = num(p) != 0.0f;
        else if (n == "selfrotaxis") q.spin_axis = fixed3(p);
        else if (n == "selfrotspeed") q.spin = p.value.c[0];
        else if (n == "texture") q.texture = assets.texture(p.value.str, false);
        else if (n == "texturenumber") q.frames = std::max(1, int(num(p)));
        else if (n == "textureperiod") q.period = num(p);
        else if (n == "rchange") q.random_frame = num(p) != 0.0f;
        else if (n == "numparticles") q.max_particles = std::size_t(std::max(0.0f, num(p)));
        else if (n == "loop") q.once = num(p) != 0.0f;
        else if (n == "timedelay") q.delay = num(p);  // HYPOTHESIS: seconds (all world emitters use 0)
        // ponytail: OriR, Has_Rotation/Rotation_* (orbiting) and IsSmo/Model (mesh particles) are
        // combat-effect features, unused by the ambient emitters; add with the skill effects.
    }
    std::stable_sort(q.events.begin(), q.events.end(), [](const Def::Event& a, const Def::Event& b) { return a.time < b.time; });
    // McCuskey CreateFadeLists: an event fades towards the next event of its kind only if that is a fade.
    for (std::size_t i = 0; i < q.events.size(); ++i)
        for (std::size_t j = i + 1; j < q.events.size(); ++j)
            if (q.events[j].kind == q.events[i].kind) {
                if (q.events[j].fade) q.events[i].next = int(j);
                break;
            }
    return q;
}

// Event DoItToIt (FUN_0067d9a0 size, 0x67daa0 colour, 0x67e210 velocity): a non-fade event sets the
// value; then the step towards a random target of the linked fade event, (target - value) / dt.
glm::vec4 event_value(std::minstd_rand& r, const Def::Event& e) {
    auto c = [&](std::size_t i) { return i < e.v.size() ? e.v[i] : PsScalar{}; };
    switch (e.kind) {
    case Def::Event::Size: return glm::vec4(pick(r, c(0)));
    case Def::Event::Velocity: return glm::vec4(to_engine(pick(r, c(0)), pick(r, c(1)), pick(r, c(2))), 0.0f);
    case Def::Event::Color: {
        // One random weight for R, G and B together, alpha separately.
        const float t = std::uniform_real_distribution<float>(0.0f, 1.0f)(r);
        auto mix = [&](std::size_t i) { return c(i).hi + (c(i).lo - c(i).hi) * t; };
        return {mix(0), mix(1), mix(2), pick(r, c(3))};
    }
    }
    return {};
}

void apply(std::minstd_rand& r, const Def::Seq& s, const Def::Event& e, ParticleSystem::Particle& p) {
    glm::vec4 cur = e.kind == Def::Event::Size ? glm::vec4(p.size) : e.kind == Def::Event::Color ? p.color : glm::vec4(p.vel, 0.0f);
    if (!e.fade) cur = event_value(r, e);
    glm::vec4 step(0.0f);
    if (e.next >= 0) {
        const Def::Event& n = s.events[std::size_t(e.next)];
        const float dt = n.time - e.time;
        step = (event_value(r, n) - cur) / (dt == 0.0f ? 1.0f : dt);
    }
    switch (e.kind) {
    case Def::Event::Size: p.size = cur.x, p.size_step = step.x; break;
    case Def::Event::Color: p.color = cur, p.color_step = step; break;
    case Def::Event::Velocity: p.vel = glm::vec3(cur), p.vel_step = glm::vec3(step); break;
    }
}

void run_events(std::minstd_rand& r, const Def::Seq& s, ParticleSystem::Particle& p) {
    while (p.next_event < s.events.size() && s.events[p.next_event].time <= p.event_time) apply(r, s, s.events[p.next_event++], p);
}

}  // namespace

ParticleSystem::ParticleSystem(AssetCache& assets, std::string_view scenario, glm::vec3 origin) : assets_(assets) {
    const auto& vfs = assets.vfs();
    const auto paths = find_scenario(vfs, scenario);
    const auto text = paths ? read_text(vfs, paths->dir + "/" + paths->name + ".stg") : std::nullopt;
    const auto stg = text ? formats::parse_block_text(*text) : std::nullopt;
    if (!stg) return;
    for (const auto& b : stg->blocks) {
        if (b.type == "CreateParticle") {
            // FUN_00551930: X, Y, Z (sector-relative X/Y; main-stage origin 0), HD degrees.
            if (const std::string* n = b.get("ParticleName")) {
                ++stage_emitters_;
                add_emitter(*n, origin + to_engine(float(b.number("X")), float(b.number("Y")), float(b.number("Z"))),
                            glm::radians(float(b.number("HD"))));
            }
            continue;
        }
        if (b.type != "CreateObject") continue;
        const std::string* file = b.get("ObjectFile");
        const std::string* dir = b.get("Path");
        if (!file || !dir) continue;
        const auto pro = read_text(vfs, "item/" + *dir + *file + ".pro");
        if (!pro) continue;
        // ItemRes_parse: HasEmitter, then 4 x (EmitterName, X, Y, Z, Direction, LockType), in file order.
        struct Slot { std::string name; float x = 0, y = 0, z = 0, dir = 0; };
        std::vector<Slot> slots;
        bool has = false;
        for (const auto& kv : formats::parse_kv_text(*pro)) {
            const std::string k = lower(kv.key);
            const float v = std::strtof(kv.value.c_str(), nullptr);
            if (k == "hasemitter") has = v != 0.0f;
            else if (k == "emittername") slots.push_back({kv.value});
            else if (slots.empty()) continue;
            else if (k == "emitterx") slots.back().x = v;
            else if (k == "emittery") slots.back().y = v;
            else if (k == "emitterz") slots.back().z = v;
            else if (k == "emitterdirection") slots.back().dir = v;
        }
        if (!has) continue;
        // Same placement as WorldScene::load_objects. Item_create_actor starts slots 0 and 1 only; the
        // offset follows the object matrix (FUN_0041ab50 lock types 1/3/4: point_transform), heading
        // = object HD + EmitterDirection (HYPOTHESIS: degrees; 0 in every shipped emitter .pro).
        const float hd = float(b.number("HD"));
        glm::mat4 t = glm::translate(glm::mat4(1.0f), origin + to_engine(float(b.number("X")), float(b.number("Y")), float(b.number("Z"))));
        t = glm::rotate(t, glm::radians(hd), glm::vec3(0, 1, 0));
        t = glm::scale(t, glm::vec3(float(b.number("Scale", 1.0))));
        for (std::size_t i = 0; i < slots.size() && i < 2; ++i) {
            if (lower(slots[i].name) == "none" || slots[i].name.empty()) continue;
            ++item_emitters_;
            add_emitter(slots[i].name, glm::vec3(t * glm::vec4(to_engine(slots[i].x, slots[i].y, slots[i].z), 1.0f)),
                        glm::radians(hd + slots[i].dir));
        }
    }
    std::fprintf(stderr, "joan-native: particles emitters=%zu stage=%d items=%d missing=%d\n", emitters_.size(), stage_emitters_,
                 item_emitters_, missing_);
}

ParticleSystem::~ParticleSystem() {
    if (mesh_) assets_.renderer().destroy_mesh(mesh_);
}

const ParticleSystem::Def* ParticleSystem::system(const std::string& name) {
    const std::string key = lower(name);
    if (const auto it = defs_.find(key); it != defs_.end()) return it->second.get();
    std::unique_ptr<Def> def;
    // FUN_004c1b40 loads SCRIPT\PSCRIPT\*.txt by file name (the ParticleSystem " name " inside is ignored).
    std::string err;
    const auto text = read_text(assets_.vfs(), "script/pscript/" + name + ".txt");
    const auto ps = text ? formats::parse_particle_system(*text, &err) : std::nullopt;
    if (ps) {
        def = std::make_unique<Def>();
        int dest = 2;
        for (const PsProperty& p : ps->props)
            if (lower(p.name) == "destblending") dest = int(num(p));
        // Every pscript uses SourceBlending 5 (SRCALPHA). DestBlending 6 (INVSRCALPHA) = alpha blend;
        // 2 (ONE) additive; 7 (DESTALPHA) reads 1 from the alpha-less back buffer = additive (HYPOTHESIS).
        def->blend = dest == 6 ? BlendMode::AlphaBlend : BlendMode::Additive;
        for (const auto& s : ps->sequences) def->seqs.push_back(compile(assets_, s));
    } else {
        std::fprintf(stderr, "joan-native: particle system %s: %s\n", name.c_str(), text ? err.c_str() : "missing");
    }
    return defs_.emplace(key, std::move(def)).first->second.get();
}

ParticleSystem::Emitter* ParticleSystem::add_emitter(const std::string& name, glm::vec3 position, float heading) {
    const Def* def = system(name);
    if (!def) {
        ++missing_;
        return nullptr;
    }
    Emitter e;
    e.def = def;
    e.position = position;
    e.rotation = yaw(heading);
    e.seqs.resize(def->seqs.size());
    return &emitters_.emplace_back(std::move(e));
}

bool ParticleSystem::spawn(const std::string& name, glm::vec3 position, float heading, float start, float stop, int tag, glm::vec3 local) {
    Emitter* e = add_emitter(name, position, heading);
    if (!e) return false;
    e->age = -start;
    e->stop = stop - start;
    e->tag = tag;
    e->local = local;
    return true;
}

void ParticleSystem::follow(const std::unordered_map<int, glm::mat4>& frames) {
    for (Emitter& e : emitters_) {
        if (!e.tag) continue;
        const auto it = frames.find(e.tag);
        if (it == frames.end()) continue;
        e.position = glm::vec3(it->second * glm::vec4(e.local, 1.0f));
        e.rotation = glm::mat3(it->second);
    }
}

std::size_t ParticleSystem::particle_count() const {
    std::size_t n = 0;
    for (const Emitter& e : emitters_)
        for (const Sequence& s : e.seqs) n += s.particles.size();
    return n;
}

void ParticleSystem::build(float dt, const glm::mat4& view, const glm::mat4& view_proj, std::vector<DrawItem>& draws) {
    dt = std::clamp(dt, 0.0f, 0.25f);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const glm::mat3 cam_rot = glm::transpose(glm::mat3(view));  // columns: camera right, up, back (world)
    const glm::vec3 right = cam_rot[0], up = cam_rot[1];
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    const glm::vec3 flat_right = glm::length(glm::vec2(right.x, right.z)) > 1e-4f ? glm::normalize(glm::vec3(right.x, 0, right.z)) : glm::vec3(1, 0, 0);
    const auto planes = frustum_planes(view_proj);

    MeshData& mesh = mesh_data_;
    std::vector<DrawItem>& out = out_;
    mesh.vertices.clear(), mesh.indices.clear(), out.clear();
    const std::size_t live = particle_count();  // before this frame's deaths/births: close enough to reserve
    mesh.vertices.reserve(live * 4), mesh.indices.reserve(live * 6);
    for (Emitter& e : emitters_) {
        e.age += dt;
        // ponytail: every emitter is simulated every frame (S1L1: ~1k particles); the original only
        // updates off-screen emitters in catch-up steps (FUN_0041ab50, >= 10 s apart).
        glm::vec3 lo(1e30f), hi(-1e30f);
        float max_size = 0;
        for (std::size_t si = 0; si < e.seqs.size(); ++si) {
            const Def::Seq& s = e.def->seqs[si];
            Sequence& q = e.seqs[si];
            // FUN_0067f380 / FUN_00662f60: age, integrate, gravity, events; dead particles leave.
            std::erase_if(q.particles, [&](Particle& p) {
                const float t = dt + p.pending;
                p.pending = 0;
                p.age += t;
                p.event_time += t;
                if (p.age >= p.life) return true;
                p.pos += p.vel * t;
                p.size += p.size_step * t;
                p.color = glm::clamp(p.color + p.color_step * t, 0.0f, 255.0f);
                p.vel += p.vel_step * t;
                if (s.frames > 1) {
                    if (p.frame_time + dt < s.period) p.frame_time += dt;
                    else p.frame = s.random_frame ? int(rng_() % unsigned(s.frames)) : (p.frame + 1) % s.frames, p.frame_time = 0;
                }
                p.vel += pick3(rng_, s.gravity) * dt;
                if (s.self_rotation) p.angle = std::fmod(p.angle + dt * p.spin * glm::two_pi<float>(), glm::two_pi<float>());
                run_events(rng_, s, p);
                return false;
            });
            // Emission: rate x dt with a fractional carry, capped at NumParticles alive.
            if (!q.done && e.age >= s.delay && e.age < e.stop) {
                const float want = pick(rng_, s.rate) * dt;
                int n = int(want);
                q.acc += want - float(n);
                if (q.acc > 1.0f) n += int(q.acc), q.acc -= std::floor(q.acc);
                for (int k = 0; k < n && q.particles.size() < s.max_particles; ++k) {
                    // FUN_00681070: position = EmitRadius + Offset, lifetime, spin, then the time-0 events.
                    Particle p;
                    p.life = pick(rng_, s.life);
                    p.pos = pick3(rng_, s.radius) + s.offset;
                    p.spin = pick(rng_, s.spin);
                    if (p.life > 0) p.pending = std::fmod(unit(rng_) * dt, p.life);
                    run_events(rng_, s, p);
                    q.particles.push_back(p);
                    if (s.once && ++q.emitted >= int(s.max_particles)) {
                        q.done = true;
                        break;
                    }
                }
            }
            for (const Particle& p : q.particles) {
                const glm::vec3 w = e.rotation * p.pos;
                lo = glm::min(lo, w), hi = glm::max(hi, w), max_size = std::max(max_size, p.size);
            }
        }
        if (lo.x > hi.x) continue;
        const glm::vec3 centre = e.position + (lo + hi) * 0.5f;
        const float radius = glm::length(hi - lo) * 0.5f + max_size;
        const glm::vec3 d = centre - eye;
        bool visible = glm::dot(d, d) < (kWorldFar + radius) * (kWorldFar + radius);
        for (const auto& pl : planes) visible = visible && glm::dot(glm::vec3(pl), centre) + pl.w >= -radius;
        if (!visible) continue;

        // FUN_0067faf0: four corners per particle, colour = D3DCOLOR, u = frame strip of TextureNumber.
        for (std::size_t si = 0; si < e.seqs.size(); ++si) {
            const Def::Seq& s = e.def->seqs[si];
            const Sequence& q = e.seqs[si];
            if (q.particles.empty() || !s.texture) continue;
            const auto first = std::uint32_t(mesh.indices.size());
            for (const Particle& p : q.particles) {
                const glm::vec3 c = e.rotation * p.pos;
                // corner(x, y) = x*a + y*b; (-1,+1) samples (u0, 0), (+1,-1) samples (u1, 1).
                glm::vec3 a = right, b = -up, axis = -cam_rot[2];
                if (s.mode == 0) {
                    // Flat quad in the original XY (ground) plane, normal turned onto Orientation.
                    a = {1, 0, 0}, b = {0, 0, -1}, axis = s.spin_axis;
                    const float ol = glm::length(s.orientation);
                    const glm::vec3 x = glm::cross(glm::vec3(0, 1, 0), s.orientation);
                    if (ol > 1e-5f && glm::length(x) > 1e-5f) {
                        const glm::mat3 m(glm::rotate(glm::mat4(1.0f), std::acos(std::clamp(s.orientation.y / ol, -1.0f, 1.0f)), glm::normalize(x)));
                        a = m * a, b = m * b;
                    }
                } else if (s.mode == 2) {
                    a = flat_right, b = {0, 1, 0};  // vertical quad turned to the camera heading (HYPOTHESIS)
                } else if (s.mode == 3) {
                    // Stretched along the velocity, facing the camera.
                    const glm::vec3 v = e.rotation * p.vel;
                    const glm::vec3 side = glm::cross(v, eye - (e.position + c));
                    if (glm::length(v) < 1e-5f || glm::length(side) < 1e-5f) continue;
                    a = glm::normalize(side), b = glm::normalize(v);
                }
                if (s.self_rotation && p.angle != 0.0f && glm::length(axis) > 1e-5f) {
                    const glm::mat3 m(glm::rotate(glm::mat4(1.0f), p.angle, glm::normalize(axis)));
                    a = m * a, b = m * b;
                }
                const float h = p.size * 0.5f;
                const float du = 1.0f / float(s.frames), u0 = float(p.frame) * du;
                const glm::vec4 col(srgb_to_linear(p.color.r / 255.0f), srgb_to_linear(p.color.g / 255.0f), srgb_to_linear(p.color.b / 255.0f),
                                    p.color.a / 255.0f);
                const auto base = std::uint32_t(mesh.vertices.size());
                for (const auto& [x, y] : {std::pair{-1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}}) {
                    Vertex v;
                    v.position = c + h * (x * a + y * b);
                    v.uv = {x < 0 ? u0 : u0 + du, y > 0 ? 0.0f : 1.0f};
                    v.color = col;
                    mesh.vertices.push_back(v);
                }
                for (std::uint32_t i : {0u, 1u, 2u, 2u, 1u, 3u}) mesh.indices.push_back(base + i);
            }
            const auto count = std::uint32_t(mesh.indices.size()) - first;
            if (!count) continue;
            DrawItem di;
            di.material.albedo = s.texture->handle;
            di.material.blend = e.def->blend;
            di.material.unlit = true;
            di.material.two_sided = true;
            di.model = glm::translate(glm::mat4(1.0f), e.position);
            di.first_index = first;
            di.index_count = count;
            out.push_back(di);
        }
    }
    std::erase_if(emitters_, [](const Emitter& e) {
        return e.age >= e.stop && std::all_of(e.seqs.begin(), e.seqs.end(), [](const Sequence& q) { return q.particles.empty(); });
    });
    drawn_ = mesh.indices.size() / 6;
    if (out.empty()) return;
    IRenderer& r = assets_.renderer();
    if (!mesh_ || !r.update_mesh(mesh_, mesh)) {
        if (mesh_) r.destroy_mesh(mesh_);
        mesh_ = r.create_mesh(mesh);
    }
    for (DrawItem& d : out) d.mesh = mesh_;
    draws.insert(draws.end(), out.begin(), out.end());
}

}  // namespace joa::engine
