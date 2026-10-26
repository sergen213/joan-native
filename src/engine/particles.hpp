#pragma once
// Ambient particle emitters of an original scenario (joan.exe pemires.cpp; McCuskey-style event
// sequences, script/pscript/<name>.txt parsed by formats/script_particle.hpp). Sources:
//   .stg CreateParticle {ParticleName, X, Y, Z, HD}             (FUN_00551930; S1L1 waterfalls)
//   item .pro HasEmitter slots 0 and 1 of placed CreateObjects  (Item_create_actor 0x42f210 loops
//     `slot < 2`; slots 2/3 hold destruction/hit effects: building_smoke, Door_broke_dust, hitgate)
// Simulated with the frame dt (FUN_0041ab50: dt = frame ms / 1000). Rendered as one CPU-built quad
// per particle (FUN_0067faf0), unlit, blend from the system's SourceBlending/DestBlending, no depth
// write. Evidence and open points: docs/reverse-engineering/RENDERING_PIPELINE.md (particles).
#include "engine/assets_cache.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace joa::engine {

class ParticleSystem {
public:
    // Reads the scenario .stg (CreateParticle + CreateObject item .pro emitters), placed at the map node
    // offset `origin` (engine space). Unknown scenarios (e.g. "") yield no emitters; emitter names without
    // a usable pscript are counted in missing().
    ParticleSystem(AssetCache& assets, std::string_view scenario, glm::vec3 origin = glm::vec3(0.0f));
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // Advances every emitter by dt seconds, then appends one draw per visible emitter sequence.
    void build(float dt, const glm::mat4& view, const glm::mat4& view_proj, std::vector<DrawItem>& draws);
    // One-shot emitter (animscr.res attack-script slot): emits from `start` s to `stop` s after the call
    // and is removed once its particles are gone. tag != 0 with `local` (unit-frame engine offset): the
    // emitter follows that unit (follow()). False when the pscript is missing.
    bool spawn(const std::string& name, glm::vec3 position, float yaw, float start, float stop, int tag = 0,
               glm::vec3 local = glm::vec3(0.0f));
    // Moves tagged emitters to their unit's frame (translation + yaw of the unit model matrix).
    void follow(const std::unordered_map<int, glm::mat4>& frames);

    std::size_t emitter_count() const { return emitters_.size(); }
    int stage_emitters() const { return stage_emitters_; }  // .stg CreateParticle
    int item_emitters() const { return item_emitters_; }    // .pro HasEmitter slots 0/1
    int missing() const { return missing_; }
    std::size_t particle_count() const;                     // alive, all emitters
    std::size_t drawn_particles() const { return drawn_; }  // in the last build()

    struct Def;  // compiled pscript (particles.cpp)
    struct Particle {
        float life = 0, age = 0, pending = 0;  // seconds; pending = sub-frame birth offset
        float event_time = 0;
        std::size_t next_event = 0;
        glm::vec3 pos{0.0f}, vel{0.0f}, vel_step{0.0f};  // emitter-local engine axes, units (/s)
        float size = 0, size_step = 0;
        glm::vec4 color{255.0f}, color_step{0.0f};        // D3D byte range 0..255
        float angle = 0, spin = 0;                         // SelfRotation, radians / revolutions per s
        int frame = 0;
        float frame_time = 0;
    };
    struct Sequence {
        std::vector<Particle> particles;
        float acc = 0;    // fractional emission carry
        int emitted = 0;  // Loop != 0: total emitted
        bool done = false;
    };
    struct Emitter {
        const Def* def = nullptr;
        glm::vec3 position{0.0f};  // engine space
        glm::mat3 rotation{1.0f};  // yaw about +Y (HD / object heading)
        float age = 0;
        float stop = 1e30f;  // emission ends at this age (s); spawn() emitters are erased after it
        int tag = 0;
        glm::vec3 local{0.0f};
        std::vector<Sequence> seqs;
    };

private:
    const Def* system(const std::string& name);
    Emitter* add_emitter(const std::string& name, glm::vec3 position, float yaw);

    AssetCache& assets_;
    std::unordered_map<std::string, std::unique_ptr<Def>> defs_;  // lowercase name; null = missing
    std::vector<Emitter> emitters_;
    int stage_emitters_ = 0, item_emitters_ = 0, missing_ = 0;
    std::minstd_rand rng_{1};
    MeshHandle mesh_;
    MeshData mesh_data_;              // build() scratch, reused across frames
    std::vector<DrawItem> out_;
    std::size_t drawn_ = 0;
};

}  // namespace joa::engine
