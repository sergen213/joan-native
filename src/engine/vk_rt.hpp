#pragma once
// Vulkan acceleration structures for the ray-traced effects (docs/RAYTRACING.md). One BLAS per drawn
// (mesh, index range). Static ones are built in the frame command buffer and compacted once their
// compacted-size query has retired; meshes replaced through IRenderer::update_mesh (CPU skinning) get an
// ALLOW_UPDATE BLAS that is refit in one batched build per frame (rebuilt when the topology changes and
// every 64th update). The TLAS is rebuilt every frame from the culled instance list.
#include "engine/vk_api.hpp"
#include "engine/vk_memory.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <tuple>
#include <vector>

namespace joa::engine::vk {

struct RtBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};

// One drawn index range of a mesh (addresses of buffers created with SHADER_DEVICE_ADDRESS + AS input usage).
struct RtGeometry {
    std::uint32_t mesh = 0;        // MeshHandle id
    std::uint32_t generation = 0;  // bumped by every update_mesh
    bool dynamic = false;          // updated at least once (CPU-skinned)
    VkDeviceAddress vertices = 0, indices = 0;
    std::uint32_t vertex_count = 0, first_index = 0, index_count = 0;
};

class RtAccel {
public:
    // Destroys a resource once the frame currently being recorded (and every earlier one) has retired.
    using Retire = std::function<void(std::function<void()>)>;

    void init(const Api& vk, VkDevice device, VkPhysicalDevice physical, GpuAllocator& allocator, VkDeviceSize vertex_stride,
              Retire retire);
    void destroy();  // device idle

    // Per frame: begin(timeline value known complete), add() each instance, build() records all AS work into
    // `cmd` and ends with the TLAS readable by compute/fragment ray queries. `slot` = frame-in-flight index.
    void begin(std::uint64_t completed, std::uint64_t recording);
    void add(const RtGeometry& g, const glm::mat4& model, std::uint32_t custom_index, std::uint8_t mask, bool opaque);
    void build(VkCommandBuffer cmd, std::uint32_t slot);
    VkAccelerationStructureKHR tlas() const { return tlas_; }
    void forget_mesh(std::uint32_t mesh);  // destroy_mesh

    struct Stats {
        std::uint32_t blas = 0, built = 0, compacted = 0, refits = 0, rebuilds = 0, instances = 0;
        VkDeviceSize compacted_from = 0, compacted_to = 0;
    } stats;  // cumulative except blas/instances

private:
    static constexpr std::uint32_t kNoQuery = ~0u, kQueries = 4096;
    struct Blas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        RtBuffer buffer;
        VkDeviceAddress address = 0;
        bool dynamic = false;
        std::uint32_t generation = 0, vertex_count = 0, index_count = 0, updates = 0;
        std::uint32_t query = kNoQuery;  // static: compacted size pending
        std::uint64_t built_at = 0;      // timeline value of the building frame
        std::uint64_t touched = 0;       // last frame scheduled for build/refit
    };
    using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;  // mesh, first index, index count
    struct Pending {
        Blas* blas;
        RtGeometry g;
        bool update;
    };
    struct Instance {
        Blas* blas;
        glm::mat4 model;
        std::uint32_t custom;
        std::uint8_t mask;
        bool opaque;
    };

    RtBuffer create(VkDeviceSize size, VkBufferUsageFlags usage, bool host);
    void free(RtBuffer& b);
    void release(Blas& b);  // retire AS + buffer, free its query
    void create_as(Blas& b, VkDeviceSize size, VkAccelerationStructureTypeKHR type);
    VkAccelerationStructureGeometryKHR triangles(const RtGeometry& g) const;
    VkDeviceSize align(VkDeviceSize v) const { return (v + scratch_align_ - 1) / scratch_align_ * scratch_align_; }
    void ensure_scratch(VkDeviceSize bytes);

    const Api* vk_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    GpuAllocator* allocator_ = nullptr;
    VkDeviceSize stride_ = 0, scratch_align_ = 256;
    Retire retire_;
    std::uint64_t completed_ = 0, recording_ = 0;

    std::map<Key, Blas> blas_;
    std::vector<Pending> pending_;
    std::vector<Instance> instances_;
    VkQueryPool queries_ = VK_NULL_HANDLE;
    std::vector<std::uint32_t> free_queries_;

    RtBuffer scratch_, instance_buffers_[2];
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    RtBuffer tlas_buffer_;
};

}  // namespace joa::engine::vk
