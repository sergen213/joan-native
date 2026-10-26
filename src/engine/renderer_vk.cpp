#include "engine/renderer_vk.hpp"

#include "engine/fsr_vk.hpp"
#include "engine/vk_api.hpp"
#include "engine/vk_memory.hpp"
#include "engine/pbr_env.hpp"
#include "engine/post_frame.hpp"
#include "engine/rt_scene.hpp"
#include "engine/shadow_volume.hpp"
#include "engine/vk_rt.hpp"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace joa::engine {
namespace {
// Vulkan create-info structs are routinely partially brace-initialised (sType + leading members).
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

using namespace vk;

// SPIR-V compiled from shaders/vk/*.{vert,frag} by glslc at build time (-mfmt=num).
const std::uint32_t kMeshVert[] = {
#include "mesh.vert.inc"
};
const std::uint32_t kMeshFrag[] = {
#include "mesh.frag.inc"
};
const std::uint32_t kUiVert[] = {
#include "ui.vert.inc"
};
const std::uint32_t kUiFrag[] = {
#include "ui.frag.inc"
};
const std::uint32_t kFillVert[] = {
#include "shadow_fill.vert.inc"
};
const std::uint32_t kFillFrag[] = {
#include "shadow_fill.frag.inc"
};
const std::uint32_t kFillRmFrag[] = {
#include "shadow_fill_rm.frag.inc"
};
// Remaster (Enhanced/Ultra) passes: shaders/vk/{mesh_pbr,post*}, bodies shared with GL in shaders/common.
const std::uint32_t kMeshPbrVert[] = {
#include "mesh_pbr.vert.inc"
};
const std::uint32_t kMeshPbrFrag[] = {
#include "mesh_pbr.frag.inc"
};
const std::uint32_t kShadowCasterFrag[] = {
#include "shadow_caster.frag.inc"
};
const std::uint32_t kPostVert[] = {
#include "post.vert.inc"
};
const std::uint32_t kPostSsao[] = {
#include "post_ssao.frag.inc"
};
const std::uint32_t kPostAoApply[] = {
#include "post_ao_apply.frag.inc"
};
const std::uint32_t kPostComposite[] = {
#include "post_composite.frag.inc"
};
const std::uint32_t kPostExposureLum[] = {
#include "post_exposure_lum.frag.inc"
};
const std::uint32_t kPostExposureAdapt[] = {
#include "post_exposure_adapt.frag.inc"
};
const std::uint32_t kPostTaa[] = {
#include "post_taa.frag.inc"
};
const std::uint32_t kPostEasu[] = {
#include "post_easu.frag.inc"
};
const std::uint32_t kPostRcas[] = {
#include "post_rcas.frag.inc"
};
// AMD FSR 3 render-size inputs: R32F depth + full motion (fsr_upscale).
const std::uint32_t kPostFsrInputs[] = {
#include "post_fsr_inputs.frag.inc"
};
// Image-quality post (any preset): FXAA, SMAA 1x edges / weights / blend (third_party/smaa).
const std::uint32_t kPostFxaa[] = {
#include "post_fxaa.frag.inc"
};
const std::uint32_t kPostSmaaEdges[] = {
#include "post_smaa_edges.frag.inc"
};
const std::uint32_t kPostSmaaWeights[] = {
#include "post_smaa_weights.frag.inc"
};
const std::uint32_t kPostSmaaBlend[] = {
#include "post_smaa_blend.frag.inc"
};
// Ray-traced effects (docs/RAYTRACING.md): RT variant of mesh_pbr.frag + compute passes; used only when the
// device has ray queries (the SPIR-V declares RayQueryKHR, so it is never handed to other drivers).
const std::uint32_t kRtMeshFrag[] = {
#include "rt_mesh_pbr.frag.inc"
};
const std::uint32_t kRtShadow[] = {
#include "rt_shadow.comp.inc"
};
const std::uint32_t kRtReflect[] = {
#include "rt_reflect.comp.inc"
};
const std::uint32_t kRtDenoise[] = {
#include "rt_denoise.comp.inc"
};
const std::uint32_t kRtProbeTrace[] = {
#include "rt_probe_trace.comp.inc"
};
const std::uint32_t kRtProbeBlend[] = {
#include "rt_probe_blend.comp.inc"
};

std::atomic<std::uint32_t> g_validation_errors{0};
std::atomic<bool> g_validation_active{false};
std::atomic<bool> g_rt_available{false};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    if (!error && type == VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT) return VK_FALSE;  // loader chatter
    if (error) ++g_validation_errors;
    std::fprintf(stderr, "joan-native: vulkan %s: %s\n", error ? "ERROR" : "warning", data->pMessage);
    return VK_FALSE;
}

constexpr std::uint32_t kFramesInFlight = 2;
constexpr std::uint32_t kMaxTextures = 4096;  // descriptorIndexing guarantees >= 500k update-after-bind samplers
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_SRGB;
constexpr VkFormat kUiColorFormat = VK_FORMAT_R8G8B8A8_UNORM;  // UNORM view of the colour image for the UI pass
constexpr VkFormat kFgFormat = VK_FORMAT_R8G8B8A8_UNORM;  // FSR 3 frame generation source/output (sRGB-encoded bytes)
constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

// OpenGL clip space (y up, z in [-1,1]) -> Vulkan (y down, z in [0,1]); camera code stays GL-convention.
const glm::mat4 kClipCorrection(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0.5f, 1);

// Timestamp queries per frame slot; the ray-tracing ones are written only on RT frames (rt_timed).
enum Query : std::uint32_t {
    QFrameBegin, QSceneBegin, QUiBegin, QSceneEnd, QPresentBegin, QPresentEnd,
    QRtBegin, QRtAccel, QRtPrepass, QRtGi, QRtShadow, QRtReflect, QCount
};

struct FrameUniforms {
    glm::mat4 view_proj;
    glm::vec4 sun_dir, sun_color, ambient, fog_color, fog_range, camera_pos, framebuffer;
};
struct DrawGpu {  // std430 mirror of `Draw` in shaders/vk/common.glsl
    glm::mat4 model, normal_matrix;
    glm::vec4 tint;
    glm::vec4 diffuse;  // w = vertex_diffuse
    float alpha_cutoff;
    std::uint32_t unlit, texture, layer2, lightmap, light_count;
    std::uint32_t bump;  // bindless slot or kNoTexture
    float bump_scale;
    std::uint32_t lights[4];  // two 16-bit Lighting::point_lights indices each, low half first
};
static_assert(sizeof(DrawGpu) == 208);
struct LightGpu {  // std430 mirror of `Light`
    glm::vec4 pos_range, color, att;
};
constexpr std::uint32_t kNoTexture = 0xffffffffu;
struct RtInstanceGpu {  // std430 mirror of `RtInstance` in shaders/vk/rt_common.glsl
    VkDeviceAddress vertices, indices;
    std::uint32_t first_index, albedo, layer2, lightmap;
    glm::vec4 tint, diffuse;
    float cutoff;
    std::uint32_t flags, pad0, pad1;
};
static_assert(sizeof(RtInstanceGpu) == 80);
struct RtPush {  // rt_denoise.comp push block
    std::uint32_t mode, signal;
    std::int32_t step;
    std::uint32_t images;  // src | dst << 8 | history << 16 (u_img indices)
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    VkDeviceSize size = 0;
};
// update_mesh streaming: host-visible vertex/index buffers of kMeshRegions regions each, written in place. Every update
// frame writes a region no frame in flight reads: each in-flight frame reads its pose and its TAA previous pose, so with
// updates every frame and kFramesInFlight = 2 three regions are busy when the next frame's update runs.
constexpr std::uint32_t kMeshRegions = 4;
constexpr std::uint64_t kNeverRead = ~0ull;
struct Mesh {
    Buffer vertices, indices;  // dynamic: kMeshRegions regions of vertex_capacity / index_capacity bytes
    VkDeviceSize vertex_offset = 0, index_offset = 0;  // the bound region
    std::uint32_t index_count = 0;
    // Ray tracing (docs/RAYTRACING.md): device addresses (0 without RT), object-space bounds, CPU-skinning state.
    std::uint32_t vertex_count = 0, generation = 0;
    bool dynamic = false;
    VkDeviceAddress vertex_address = 0, index_address = 0;
    glm::vec3 centre{0.0f};
    float radius = 0.0f;
    // Streaming state (dynamic): region bound, frame_ of each region's last read (kNeverRead = never; cur's is
    // stale while it is cur), index content version held by each region, CPU copy of the indices (change test).
    VkDeviceSize vertex_capacity = 0, index_capacity = 0, vertex_bytes = 0;
    std::uint32_t region = 0, index_version = 0;
    std::array<std::uint64_t, kMeshRegions> region_read{kNeverRead, kNeverRead, kNeverRead, kNeverRead};
    std::array<std::uint32_t, kMeshRegions> region_indices{};
    std::vector<std::uint32_t> index_data;
    // TAA motion vectors (update_mesh): the vertices shown before the update of frame `updated` (prev = none): a
    // region of `vertices`, or of prev_owned (the static buffers the first update replaced, kept to the next update).
    VkBuffer prev = VK_NULL_HANDLE;
    VkDeviceSize prev_offset = 0, prev_bytes = 0;
    Buffer prev_owned;
    std::uint64_t updated = ~0ull;
};
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Allocation memory;
    std::uint32_t slot = 0;
    bool repeat = false, mipmaps = false;  // sampler choice (samplers_[repeat][mipmaps])
    int width = 0, height = 0;  // update_texture: same size, format and mip count only
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t mips = 1;
};
struct Image2D {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Allocation memory;
};
struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer scene = VK_NULL_HANDLE, present = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    // FSR 3 frame generation: the interpolated frame's own acquire + blit, presented before the real one.
    VkCommandBuffer present_generated = VK_NULL_HANDLE;
    VkSemaphore acquired_generated = VK_NULL_HANDLE;
    std::uint64_t retire_value = 0;  // timeline value reached once the GPU finished this slot's last frame
    Buffer uniforms, draws, lights, ui;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    bool timed = false, present_timed = false, rt_timed = false;
    Buffer rm_ubo, rm_materials;  // remaster frame block, MaterialExtra table (set 2)
    VkDescriptorSet rm_set = VK_NULL_HANDLE;
    // Interactive water wave field (FrameData::waves, set 2 binding 24): R16F staging + image, rewritten when the slot is idle.
    Buffer rm_waves;
    Image2D waves;
    Buffer env_staging;  // in-place environment map rewrites (ensure_env)
    Buffer rt_ubo, rt_instances;  // RtFrame, RtInstance table (set 3)
    VkDescriptorSet rt_set = VK_NULL_HANDLE;
};

VkImageSubresourceRange range(VkImageAspectFlags aspect, std::uint32_t mip = 0, std::uint32_t mips = VK_REMAINING_MIP_LEVELS) {
    return {aspect, mip, mips, 0, 1};
}

std::filesystem::path pipeline_cache_path() {
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::filesystem::path(x) / "joan-native/vk_pipeline.cache";
    if (const char* h = std::getenv("HOME"); h && *h) return std::filesystem::path(h) / ".cache/joan-native/vk_pipeline.cache";
    return {};
}

const char* device_type(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

class VkRenderer final : public IRenderer {
public:
    VkRenderer(SDL_Window* window, const Options& o) : window_(window), options_(o) {
        if (!SDL_Vulkan_LoadLibrary(nullptr)) throw std::runtime_error(std::string("SDL_Vulkan_LoadLibrary: ") + SDL_GetError());
        library_loaded_ = true;
        vk_.load_global(reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr()));
        if (!vk_.vkCreateInstance) throw std::runtime_error("Vulkan loader unavailable");
        std::uint32_t version = VK_API_VERSION_1_0;
        if (vk_.vkEnumerateInstanceVersion) vk_.vkEnumerateInstanceVersion(&version);
        if (version < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan 1.3 loader required");

        // Surfaceless under SDL's offscreen/dummy drivers: render to the offscreen target only.
        const std::string_view driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "";
        const bool want_surface = (SDL_GetWindowFlags(window) & SDL_WINDOW_VULKAN) && driver != "offscreen" && driver != "dummy";
        create_instance(want_surface);
        if (want_surface && !SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
            std::fprintf(stderr, "joan-native: vulkan: no surface (%s), rendering offscreen\n", SDL_GetError());
            surface_ = VK_NULL_HANDLE;
        }
        pick_device();
        create_device();
        allocator_.init(vk_, device_, memory_props_, rt_caps_.usable());  // buffer device addresses for RT
        create_descriptors();
        create_pipelines();
        create_slots();
        white_ = create_texture({1, 1, PixelFormat::Rgba8Unorm, {255, 255, 255, 255}, false, true});
        if (surface_) recreate_swapchain();
    }

    ~VkRenderer() override {
        if (device_) {
            vk_.vkDeviceWaitIdle(device_);
            report_timings();
            save_pipeline_cache();
            for (auto& [id, m] : meshes_) destroy(m);
            for (auto& [id, t] : textures_) destroy(t);
            for (auto& g : graveyard_) g.second();
            destroy_rt();
            fsr_.reset();
            destroy_fg_targets();
            destroy_remaster();
            destroy_target();
            destroy(present_bridge_);
            destroy_swapchain(swapchain_);
            for (VkSemaphore s : present_done_) vk_.vkDestroySemaphore(device_, s, nullptr);
            for (Slot& s : slots_) {
                vk_.vkDestroyCommandPool(device_, s.pool, nullptr);
                vk_.vkDestroySemaphore(device_, s.acquired, nullptr);
                vk_.vkDestroySemaphore(device_, s.acquired_generated, nullptr);
                destroy(s.uniforms);
                destroy(s.draws);
                destroy(s.lights);
                destroy(s.ui);
                destroy(s.env_staging);
                if (s.queries) vk_.vkDestroyQueryPool(device_, s.queries, nullptr);
            }
            vk_.vkDestroySemaphore(device_, timeline_, nullptr);
            vk_.vkDestroyCommandPool(device_, upload_pool_, nullptr);
            vk_.vkDestroyCommandPool(device_, async_pool_, nullptr);
            for (VkPipeline p : {opaque_, blend_, blend_zwrite_, additive_, volume_, fill_, ui_}) vk_.vkDestroyPipeline(device_, p, nullptr);
            vk_.vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
            vk_.vkDestroyPipelineLayout(device_, layout_, nullptr);
            vk_.vkDestroyPipelineLayout(device_, fill_layout_, nullptr);
            vk_.vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
            vk_.vkDestroyDescriptorSetLayout(device_, textures_layout_, nullptr);
            vk_.vkDestroyDescriptorSetLayout(device_, frame_layout_, nullptr);
            for (auto& row : samplers_)
                for (VkSampler s : row) vk_.vkDestroySampler(device_, s, nullptr);
            allocator_.destroy();
            vk_.vkDestroyDevice(device_, nullptr);
        }
        if (surface_) vk_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (messenger_) vk_.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        if (instance_) vk_.vkDestroyInstance(instance_, nullptr);
        if (library_loaded_) SDL_Vulkan_UnloadLibrary();
    }

    const char* name() const override { return "vulkan"; }

    MeshHandle create_mesh(const MeshData& data) override {
        const std::uint32_t id = next_id_++;
        meshes_[id] = upload(data);
        return {id};
    }

    // Same handle, rewritten in place (CPU-skinned characters and their shadow volumes, particles, water: every frame).
    // Host-visible streaming buffers of kMeshRegions regions: the first update of a frame writes a region no frame in
    // flight reads, later ones that frame rewrite it; indices are copied only when they changed or the region holds an
    // older version. Larger data regrows the buffers (x1.5); the old ones retire once in-flight frames finish.
    bool update_mesh(MeshHandle h, const MeshData& data) override {
        const auto it = meshes_.find(h.id);
        if (it == meshes_.end()) return false;
        Mesh& m = it->second;
        const VkDeviceSize vbytes = data.vertices.size() * sizeof(Vertex), ibytes = data.indices.size() * sizeof(std::uint32_t);
        if (m.updated != frame_) {
            // TAA motion vectors: the first update of a frame keeps the vertices shown so far as `prev` (read by this frame
            // too); a later one in the same frame keeps that. A changed vertex count has no previous pose.
            if (m.prev_owned.buffer) retire([this, b = m.prev_owned]() mutable { destroy(b); });
            m.prev_owned = {};
            m.prev = VK_NULL_HANDLE;
            const bool keep = m.vertex_bytes == vbytes;
            if (!m.dynamic) {  // first update of a create_mesh mesh: its device-local buffers are replaced
                if (keep) m.prev_owned = m.vertices, m.prev = m.vertices.buffer, m.prev_offset = 0;
                else retire([this, b = m.vertices]() mutable { destroy(b); });
                retire([this, b = m.indices]() mutable { destroy(b); });
                m.vertices = m.indices = {};
                m.dynamic = true;  // CPU-skinned: refit BLAS (vk_rt.hpp)
            } else {
                m.region_read[m.region] = frame_;
                if (keep) m.prev = m.vertices.buffer, m.prev_offset = m.vertex_offset;
            }
            m.prev_bytes = vbytes;
        } else if (m.prev && m.prev_bytes != vbytes) {
            m.prev = VK_NULL_HANDLE;
        }
        const auto grow = [](VkDeviceSize cap, VkDeviceSize bytes) { return (std::max({bytes, cap + cap / 2, VkDeviceSize(1)}) + 255) & ~VkDeviceSize(255); };
        if (!m.vertices.buffer || vbytes > m.vertex_capacity) {  // regions of the old buffer: never this frame's prev (smaller)
            if (m.vertices.buffer) retire([this, b = m.vertices]() mutable { destroy(b); });
            if (m.prev == m.vertices.buffer) m.prev = VK_NULL_HANDLE;
            m.vertex_capacity = grow(m.vertex_capacity, vbytes);
            m.vertices = create_buffer(m.vertex_capacity * kMeshRegions, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | rt_usage(), kHost, 0);
            m.region_read.fill(kNeverRead);
        }
        if (!m.indices.buffer || ibytes > m.index_capacity) {
            if (m.indices.buffer) retire([this, b = m.indices]() mutable { destroy(b); });
            m.index_capacity = grow(m.index_capacity, ibytes);
            m.indices = create_buffer(m.index_capacity * kMeshRegions, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | rt_usage(), kHost, 0);
            m.region_indices.fill(~0u);
        }
        if (m.updated != frame_) {
            // A region whose last reader retired (never the one just shown or the prev); with more frames queued than
            // regions allow, wait for the oldest reader.
            std::uint32_t pick = kMeshRegions, oldest = 0;
            for (std::uint32_t r = 0; r < kMeshRegions && pick == kMeshRegions; ++r) {
                const std::uint64_t f = m.region_read[r];
                if (f == kNeverRead || (f < frame_ && submit_value(f) <= completed_value_)) pick = r;
                else if (f < m.region_read[oldest]) oldest = r;
            }
            if (pick == kMeshRegions) wait(submit_value(m.region_read[pick = oldest]));
            m.region = pick;
        }
        m.vertex_offset = m.region * m.vertex_capacity;
        m.index_offset = m.region * m.index_capacity;
        if (vbytes) std::memcpy(m.vertices.memory.mapped + m.vertex_offset, data.vertices.data(), vbytes);
        if (data.indices != m.index_data) m.index_data = data.indices, ++m.index_version;
        if (m.region_indices[m.region] != m.index_version) {
            if (ibytes) std::memcpy(m.indices.memory.mapped + m.index_offset, data.indices.data(), ibytes);
            m.region_indices[m.region] = m.index_version;
        }
        m.vertex_bytes = vbytes;
        m.index_count = std::uint32_t(data.indices.size());
        ++m.generation;
        m.updated = frame_;
        rt_mesh_info(m, data);
        return true;
    }

    // Ray tracing: AS build input + shader device address usage on every mesh buffer when the device has RT.
    VkBufferUsageFlags rt_usage() const {
        return rt_caps_.usable() ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
    }
    VkDeviceAddress address(const Buffer& b) const {
        const VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, b.buffer};
        return vk_.vkGetBufferDeviceAddress(device_, &info);
    }
    void rt_mesh_info(Mesh& m, const MeshData& data) {
        if (!rt_caps_.usable()) return;
        m.vertex_count = std::uint32_t(data.vertices.size());
        m.vertex_address = address(m.vertices) + m.vertex_offset;
        m.index_address = address(m.indices) + m.index_offset;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const Vertex& v : data.vertices) lo = glm::min(lo, v.position), hi = glm::max(hi, v.position);
        m.centre = data.vertices.empty() ? glm::vec3(0.0f) : (lo + hi) * 0.5f;
        m.radius = data.vertices.empty() ? 0.0f : glm::length(hi - lo) * 0.5f;
    }

    void destroy_mesh(MeshHandle h) override {
        if (auto it = meshes_.find(h.id); it != meshes_.end()) {
            retire([this, m = it->second]() mutable { destroy(m); });
            if (rt_ready_) rt_accel_.forget_mesh(h.id);
            meshes_.erase(it);
        }
    }

    Mesh upload(const MeshData& data) {
        Mesh m;
        const VkDeviceSize vbytes = data.vertices.size() * sizeof(Vertex), ibytes = data.indices.size() * sizeof(std::uint32_t);
        m.vertices = create_buffer(std::max<VkDeviceSize>(vbytes, 4), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rt_usage(),
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        m.indices = create_buffer(std::max<VkDeviceSize>(ibytes, 4), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | rt_usage(),
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        Buffer staging = create_buffer(vbytes + ibytes + 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHost, 0);
        if (vbytes) std::memcpy(staging.memory.mapped, data.vertices.data(), vbytes);
        if (ibytes) std::memcpy(staging.memory.mapped + vbytes, data.indices.data(), ibytes);
        submit_upload(staging, [&](VkCommandBuffer cmd) {
            if (vbytes) {
                const VkBufferCopy c{0, 0, vbytes};
                vk_.vkCmdCopyBuffer(cmd, staging.buffer, m.vertices.buffer, 1, &c);
            }
            if (ibytes) {
                const VkBufferCopy c{vbytes, 0, ibytes};
                vk_.vkCmdCopyBuffer(cmd, staging.buffer, m.indices.buffer, 1, &c);
            }
            // Every later reader: vertex input, BLAS builds, ray-query shaders through device addresses; and later writers
            // (a later upload into this memory once the sub-allocator reuses it).
            const VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
            memory_barrier(cmd, b);
        });
        m.index_count = std::uint32_t(data.indices.size());
        m.vertex_bytes = vbytes;
        rt_mesh_info(m, data);
        return m;
    }

    TextureHandle create_texture(const TextureData& t) override {
        if (t.width <= 0 || t.height <= 0 || t.pixels.size() < std::size_t(t.width) * t.height * 4) return {};
        if (free_slots_.empty() && next_slot_ >= kMaxTextures) throw std::runtime_error("Vulkan: bindless texture table full");
        Texture tex;
        tex.width = t.width;
        tex.height = t.height;
        tex.format = t.format == PixelFormat::Rgba8Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        tex.mips = texture_mips(t);
        Image2D img = create_image(tex.format, {std::uint32_t(t.width), std::uint32_t(t.height)}, tex.mips,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
        tex.image = img.image;
        tex.view = img.view;
        tex.memory = img.memory;
        tex.repeat = t.repeat;
        tex.mipmaps = t.mipmaps;
        upload_texture(tex, t, VK_PIPELINE_STAGE_2_NONE);

        if (!free_slots_.empty()) {
            tex.slot = free_slots_.back();
            free_slots_.pop_back();
        } else {
            tex.slot = next_slot_++;
        }
        write_texture_slot(tex);
        const std::uint32_t id = next_id_++;
        textures_[id] = tex;
        return {id};
    }

    // Same size, format, sampler and mip count: the image is rewritten by a submit_upload. That runs on the frame queue
    // ahead of every later frame, and its first barrier waits for the earlier (possibly in-flight) frames' reads, so
    // neither the CPU nor a frame in flight waits on, or sees, a half-written image.
    bool update_texture(TextureHandle h, const TextureData& t) override {
        const auto it = textures_.find(h.id);
        if (it == textures_.end() || t.pixels.size() < std::size_t(t.width) * t.height * 4) return false;
        const Texture& tex = it->second;
        const VkFormat format = t.format == PixelFormat::Rgba8Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        if (t.width != tex.width || t.height != tex.height || format != tex.format || t.repeat != tex.repeat || t.mipmaps != tex.mipmaps ||
            texture_mips(t) != tex.mips)
            return false;
        upload_texture(tex, t, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        return true;
    }

    static std::uint32_t texture_mips(const TextureData& t) {
        return t.mipmaps && !t.mip_pixels.empty() ? std::uint32_t(t.mip_pixels.size()) + 1
               : t.mipmaps                        ? std::uint32_t(std::floor(std::log2(std::max(t.width, t.height)))) + 1
                                                  : 1;
    }

    // Staged copy of `t` into every level of tex.image (whose contents are discarded), ending SHADER_READ_ONLY; `after`
    // = the earlier work it must wait for (an update: the frames that sampled the old contents).
    void upload_texture(const Texture& tex, const TextureData& t, VkPipelineStageFlags2 after) {
        const bool own_mips = t.mipmaps && !t.mip_pixels.empty();
        const std::uint32_t mips = tex.mips;
        // Staging holds level 0, then the file mip levels back to back.
        std::vector<VkBufferImageCopy> copies;
        VkDeviceSize bytes = 0;
        for (std::uint32_t i = 0; i < (own_mips ? mips : 1); ++i) {
            const std::uint32_t w = std::max(t.width >> i, 1), h = std::max(t.height >> i, 1);
            copies.push_back({bytes, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1}, {0, 0, 0}, {w, h, 1}});
            bytes += VkDeviceSize(w) * h * 4;
        }
        Buffer staging = create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHost, 0);
        for (std::uint32_t i = 0; i < copies.size(); ++i) {
            const auto& src = i ? t.mip_pixels[i - 1] : t.pixels;
            const VkDeviceSize n = VkDeviceSize(copies[i].imageExtent.width) * copies[i].imageExtent.height * 4;
            std::memcpy(static_cast<std::uint8_t*>(staging.memory.mapped) + copies[i].bufferOffset, src.data(), std::min<VkDeviceSize>(n, src.size()));
        }
        submit_upload(staging, [&](VkCommandBuffer cmd) {
            const auto color = VK_IMAGE_ASPECT_COLOR_BIT;
            image_barrier(cmd, tex.image, range(color), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          after, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            // Rows are uploaded top-first, so UV (0,0) addresses the top-left texel (Direct3D convention).
            vk_.vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, std::uint32_t(copies.size()),
                                       copies.data());
            if (own_mips) {
                image_barrier(cmd, tex.image, range(color), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                return;
            }
            // Box-filtered mip chain by successive blits (sRGB formats filter in linear space, like glGenerateMipmap).
            std::int32_t w = t.width, h = t.height;
            for (std::uint32_t i = 1; i < mips; ++i) {
                image_barrier(cmd, tex.image, range(color, i - 1, 1), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                const std::int32_t nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
                const VkImageBlit blit{{color, i - 1, 0, 1}, {{0, 0, 0}, {w, h, 1}}, {color, i, 0, 1}, {{0, 0, 0}, {nw, nh, 1}}};
                vk_.vkCmdBlitImage(cmd, tex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &blit, VK_FILTER_LINEAR);
                image_barrier(cmd, tex.image, range(color, i - 1, 1), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                w = nw;
                h = nh;
            }
            image_barrier(cmd, tex.image, range(color, mips - 1, 1), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
    }

    void write_texture_slot(const Texture& tex) {
        const VkDescriptorImageInfo info{samplers_[tex.repeat][tex.mipmaps], tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = textures_set_;
        w.dstArrayElement = tex.slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vk_.vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    }

    void destroy_texture(TextureHandle h) override {
        if (auto it = textures_.find(h.id); it != textures_.end()) {
            retire([this, t = it->second]() mutable {
                destroy(t);
                free_slots_.push_back(t.slot);
            });
            textures_.erase(it);
        }
    }

    void set_graphics(const GraphicsSettings& g) override {
        if (g.shadow_map_size != gfx_.shadow_map_size && shadow_.image) {
            vk_.vkDeviceWaitIdle(device_);
            destroy_rm_shadow();
        }
        const bool aniso_changed = g.anisotropy != gfx_.anisotropy;
        gfx_ = g;
        if (aniso_changed) {  // new anisotropic samplers; rewrite the bindless slots that use them
            vk_.vkDeviceWaitIdle(device_);
            for (VkSampler s : {samplers_[0][1], samplers_[1][1]}) vk_.vkDestroySampler(device_, s, nullptr);
            create_samplers(true);
            for (const auto& [id, t] : textures_)
                if (t.mipmaps) write_texture_slot(t);
        }
        // AMD FSR 3.1.4 needs the remaster path's motion vectors and a capable device; else FSR1 at the same scale. It is
        // the anti-aliasing too, so MSAA is off under it (one sample per pixel, like Kabus 22).
        remaster_ = g.remastered();
        const bool fsr3 = g.upscale == Upscaler::Fsr3 && remaster_ && !fsr_missing_;
        if (g.upscale == Upscaler::Fsr3 && !fsr3)
            std::fprintf(stderr, "joan-native: vulkan FSR 3 unavailable (%s), using FSR1 at the same render scale\n",
                         remaster_ ? fsr_missing_ : "the Original look has no motion vectors");
        else if (g.frame_generation && generation_missing_)
            std::fprintf(stderr, "joan-native: vulkan FSR 3 frame generation unavailable (%s)\n", generation_missing_);
        if (!fsr3) fsr3_fallback(gfx_);
        if (fsr3) gfx_.msaa = 1;
        if (fsr3 != fsr3_ || (fsr3 && g.frame_generation && !generation_missing_) != generating_) {
            vk_.vkDeviceWaitIdle(device_);
            fsr_.reset();
        }
        fsr3_ = fsr3;
        generating_ = fsr3 && g.frame_generation && !generation_missing_;
        gfx_.frame_generation = generating_;
        if (generating_ && options_.present_mode != PresentMode::Fifo)
            std::fputs("joan-native: vulkan frame generation is paced by FIFO vsync; other present modes may drop generated frames\n", stderr);
        fsr_last_ = fg_last_ = ~0ull;
        const VkSampleCountFlags counts = props_.limits.framebufferColorSampleCounts & props_.limits.framebufferDepthSampleCounts &
                                          props_.limits.framebufferStencilSampleCounts;
        while (gfx_.msaa > 1 && !(counts & VkSampleCountFlags(gfx_.msaa))) gfx_.msaa /= 2;  // highest supported <= requested
        history_.valid = false;
        rt_history_ = false;
        if (remaster_)
            std::fprintf(stderr, "joan-native: vulkan graphics preset %s (remaster path%s%s)\n", preset_name(g.preset),
                         fsr3_ ? ", FSR 3.1.4 upscaling" : "", generating_ ? " + frame generation" : "");
        else if (g.original_post())
            std::fprintf(stderr, "joan-native: vulkan graphics preset %s (original look + image-quality post)\n", preset_name(g.preset));
        // Ray tracing is optional everywhere: without a remastered preset or ray-query hardware the raster
        // path of the same settings runs (shadow map or stencil volumes, IBL ambient + reflections) -- docs/RAYTRACING.md.
        if (g.rt.any()) {
            const std::string want = std::string(g.rt.reflections ? " reflections" : "") + (g.rt.shadows ? " shadows" : "") + (g.rt.gi ? " gi" : "");
            if (!remaster_)
                std::fprintf(stderr, "joan-native: vulkan ray tracing (%s ) ignored: the Original preset never changes\n", want.c_str());
            else if (!rt_caps_.usable())
                std::fprintf(stderr, "joan-native: vulkan ray tracing (%s ) unavailable (%s), using the raster path\n", want.c_str(),
                             rt_caps_.missing().c_str());
            else
                std::fprintf(stderr, "joan-native: vulkan ray tracing:%s (%d ray/px, max roughness %.2f, denoise %s)\n", want.c_str(),
                             g.rt.rays_per_pixel, g.rt.max_roughness, g.rt.denoise ? "on" : "off");
        }
    }

    bool rt_active() const { return remaster_ && gfx_.rt.any() && rt_caps_.usable(); }
    bool draws_shadow_volumes() const override { return !(rt_active() && gfx_.rt.shadows); }  // render_remaster's draw table

    void set_material_maps(TextureHandle albedo, const MaterialMaps& maps) override {
        if (albedo) material_maps_[albedo.id] = maps;
    }

    void begin_frame(glm::ivec2 size) override {
        if (recording_) end_frame();  // tolerate a missing end_frame
        slot_ = (slot_ + 1) % kFramesInFlight;
        Slot& s = slots_[slot_];
        wait(s.retire_value);
        collect_timings(s);
        collect_garbage();
        if (surface_) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window_, &w, &h);
            if (swapchain_dirty_ || std::uint32_t(w) != swapchain_extent_.width || std::uint32_t(h) != swapchain_extent_.height)
                recreate_swapchain();
        }
        size = glm::max(size, glm::ivec2(1));
        if (size != target_size_) create_target(size);

        check(vk_.vkResetCommandPool(device_, s.pool, 0), "vkResetCommandPool");
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                             VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        check(vk_.vkBeginCommandBuffer(s.scene, &begin), "vkBeginCommandBuffer");
        cmd_ = s.scene;
        s.timed = s.present_timed = false;
        if (s.queries) {
            vk_.vkCmdResetQueryPool(cmd_, s.queries, 0, QCount);
            timestamp(QFrameBegin);
            s.timed = true;
        }
        // The previous frame's blit/readback read the target; this frame overwrites all of it.
        image_barrier(cmd_, color_.image, range(VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        const VkPipelineStageFlags2 tests = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        image_barrier(cmd_, depth_.image, range(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT), VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, tests,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        fg_pending_ = fg_valid_ = false;
        recording_ = true;
    }

    void submit(const FrameData& f) override {
        if (recording_) {
            render_pass(&f);
            finish_scene();
        }
        submit_values_[frame_ % kSubmitValues] = timeline_value_;
        ++frame_;  // Mesh::updated of the next frame's update_mesh calls
    }

    void end_frame() override {
        if (recording_) {
            render_pass(nullptr);  // begin_frame without submit: black frame, like GL
            finish_scene();
        }
        if (!swapchain_) return;
        Slot& s = slots_[slot_];
        // FSR 3 frame generation: the generated frame first, then the real one. FIFO (the default) shows each for one
        // refresh, so the display paces them evenly while rendering runs at half the refresh rate.
        // ponytail: no CPU pacing thread (ETI Yami's present_loop); add one if non-FIFO or VRR presentation must be smooth.
        if (fg_valid_ && !present(s, s.present_generated, s.acquired_generated, fg_present_.image, false)) return;
        if (fg_valid_) ++fg_presents_;
        if (present(s, s.present, s.acquired, color_.image, s.timed) && generating_) ++real_presents_;
    }

    // Blits `source` (target-size, TRANSFER_SRC_OPTIMAL) into the next swapchain image and presents it; false when the
    // swapchain is out of date. Every call signals the timeline: the slot retires after its last present.
    bool present(Slot& s, VkCommandBuffer cmd, VkSemaphore acquired_semaphore, VkImage source, bool timed) {
        std::uint32_t index = 0;
        const VkResult acquired = vk_.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_semaphore, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain_dirty_ = true;
            return false;
        }
        if (acquired == VK_SUBOPTIMAL_KHR) swapchain_dirty_ = true;
        else check(acquired, "vkAcquireNextImageKHR");

        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                             VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        check(vk_.vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
        cmd_ = cmd;
        if (timed) {
            timestamp(QPresentBegin);
            s.present_timed = true;
        }
        const VkImage image = swapchain_images_[index];
        const auto color = VK_IMAGE_ASPECT_COLOR_BIT;
        image_barrier(cmd_, image, range(color), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const std::int32_t w = std::min<std::int32_t>(target_size_.x, std::int32_t(swapchain_extent_.width));
        const std::int32_t h = std::min<std::int32_t>(target_size_.y, std::int32_t(swapchain_extent_.height));
        if (w != std::int32_t(swapchain_extent_.width) || h != std::int32_t(swapchain_extent_.height)) {
            const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
            const VkImageSubresourceRange r = range(color);
            vk_.vkCmdClearColorImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &r);
            image_barrier(cmd_, image, range(color), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        // Blit (not copy): converts RGBA -> the swapchain's BGRA order; sRGB on both ends keeps bytes intact. A non-sRGB
        // swapchain would get the decoded (linear, too dark) values, so the sRGB-encoded bytes are first copied (no
        // conversion) into the UNORM present_bridge_, whose blit then carries them over as they are.
        VkImage blit_source = source;
        if (!swapchain_srgb_) {
            if (bridge_size_ != target_size_) {
                if (present_bridge_.image) retire([this, b = present_bridge_]() mutable { destroy(b); });
                // SAMPLED only because create_image also makes a view.
                present_bridge_ = create_image(kUiColorFormat, {std::uint32_t(target_size_.x), std::uint32_t(target_size_.y)}, 1,
                                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                               color);
                bridge_size_ = target_size_;
            }
            // Earlier presents' blits read the bridge (same queue: the barrier orders after them).
            image_barrier(cmd_, present_bridge_.image, range(color), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkImageCopy copy{{color, 0, 0, 1}, {0, 0, 0}, {color, 0, 0, 1}, {0, 0, 0}, {std::uint32_t(w), std::uint32_t(h), 1}};
            vk_.vkCmdCopyImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present_bridge_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);
            image_barrier(cmd_, present_bridge_.image, range(color), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT);
            blit_source = present_bridge_.image;
        }
        const VkImageBlit blit{{color, 0, 0, 1}, {{0, 0, 0}, {w, h, 1}}, {color, 0, 0, 1}, {{0, 0, 0}, {w, h, 1}}};
        vk_.vkCmdBlitImage(cmd_, blit_source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_NEAREST);
        image_barrier(cmd_, image, range(color), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        if (timed) timestamp(QPresentEnd);
        check(vk_.vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");

        const VkSemaphoreSubmitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, acquired_semaphore, 0,
                                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0};
        const VkSemaphoreSubmitInfo signals[2] = {
            {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, timeline_, ++timeline_value_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0},
            {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, present_done_[index], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0}};
        const VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmd_, 0};
        const VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, 1, &wait_info, 1, &cb, 2, signals};
        check(vk_.vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
        s.retire_value = timeline_value_;

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &present_done_[index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &index;
        const VkResult presented = vk_.vkQueuePresentKHR(queue_, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) swapchain_dirty_ = true;
        else check(presented, "vkQueuePresentKHR");
        return presented != VK_ERROR_OUT_OF_DATE_KHR;
    }

    Image read_back() override {
        Image img{target_size_.x, target_size_.y, {}};
        if (!color_.image || recording_ || !scene_rendered_) return img;
        const VkDeviceSize bytes = VkDeviceSize(img.width) * img.height * 4;
        Buffer staging = create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHost, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        immediate([&](VkCommandBuffer cmd) {
            // Target stays in TRANSFER_SRC after the scene (the scene's barrier already made its writes visible).
            const VkBufferImageCopy copy{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                         {std::uint32_t(img.width), std::uint32_t(img.height), 1}};
            vk_.vkCmdCopyImageToBuffer(cmd, color_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
            memory_barrier(cmd, {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
        });
        // Rows are already top-first (the clip correction flips y) and sRGB-encoded by the attachment format.
        img.rgba.assign(staging.memory.mapped, staging.memory.mapped + bytes);
        destroy(staging);
        for (std::size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;  // frames are opaque
        return img;
    }

private:
    // ---- setup ----------------------------------------------------------------------------------

    void create_instance(bool want_surface) {
        std::vector<const char*> extensions, layers;
        if (want_surface) {
            Uint32 n = 0;
            if (const char* const* sdl = SDL_Vulkan_GetInstanceExtensions(&n)) extensions.assign(sdl, sdl + n);
        }
        auto has_extension = [&](const char* layer, const char* ext) {
            std::uint32_t n = 0;
            vk_.vkEnumerateInstanceExtensionProperties(layer, &n, nullptr);
            std::vector<VkExtensionProperties> props(n);
            vk_.vkEnumerateInstanceExtensionProperties(layer, &n, props.data());
            return std::any_of(props.begin(), props.end(), [&](const auto& p) { return std::strcmp(p.extensionName, ext) == 0; });
        };
        constexpr const char* kValidation = "VK_LAYER_KHRONOS_validation";
        bool validation = false, sync_validation = false;
        if (options_.vk_validation) {
            std::uint32_t n = 0;
            vk_.vkEnumerateInstanceLayerProperties(&n, nullptr);
            std::vector<VkLayerProperties> props(n);
            vk_.vkEnumerateInstanceLayerProperties(&n, props.data());
            validation = std::any_of(props.begin(), props.end(), [&](const auto& p) { return std::strcmp(p.layerName, kValidation) == 0; });
            if (!validation)
                std::fputs("joan-native: vulkan: --vk-validation requested but VK_LAYER_KHRONOS_validation is not installed "
                           "(set VK_LAYER_PATH)\n", stderr);
        }
        if (validation) {
            layers.push_back(kValidation);
            if (has_extension(nullptr, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) || has_extension(kValidation, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            else
                validation = false;
            sync_validation = validation && has_extension(kValidation, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            if (sync_validation) extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }

        VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = debug_callback;
        const VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT, &messenger, 1, enables, 0, nullptr};

        const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "joan-native", 1, "joa_engine", 1, VK_API_VERSION_1_3};
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pNext = validation ? (sync_validation ? static_cast<const void*>(&features) : &messenger) : nullptr;
        info.pApplicationInfo = &app;
        info.enabledLayerCount = std::uint32_t(layers.size());
        info.ppEnabledLayerNames = layers.data();
        info.enabledExtensionCount = std::uint32_t(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        check(vk_.vkCreateInstance(&info, nullptr, &instance_), "vkCreateInstance");
        vk_.load_instance(instance_);
        if (validation) check(vk_.vkCreateDebugUtilsMessengerEXT(instance_, &messenger, nullptr, &messenger_), "vkCreateDebugUtilsMessengerEXT");
        g_validation_active = validation;
        if (validation)
            std::fprintf(stderr, "joan-native: vulkan validation enabled%s\n", sync_validation ? " (+synchronization)" : "");
    }

    // Empty string = usable; otherwise why not.
    std::string unsuitable(VkPhysicalDevice pd, std::uint32_t& family) {
        VkPhysicalDeviceProperties props;
        vk_.vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) return "Vulkan < 1.3";
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
        vk_.vkGetPhysicalDeviceFeatures2(pd, &f);
        if (!f13.dynamicRendering || !f13.synchronization2 || !f13.shaderDemoteToHelperInvocation || !f12.timelineSemaphore ||
            !f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound || !f12.descriptorBindingSampledImageUpdateAfterBind ||
            !f12.descriptorBindingUpdateUnusedWhilePending || !f12.shaderSampledImageArrayNonUniformIndexing)
            return "missing dynamic rendering/sync2/demote/timeline/descriptor indexing";
        anisotropy_ = f.features.samplerAnisotropy;
        independent_blend_ = f.features.independentBlend;
        if (surface_) {
            std::uint32_t n = 0;
            vk_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> exts(n);
            vk_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
            if (std::none_of(exts.begin(), exts.end(), [](const auto& e) { return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; }))
                return "no VK_KHR_swapchain";
        }
        std::uint32_t n = 0;
        vk_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vk_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, families.data());
        for (std::uint32_t i = 0; i < n; ++i) {
            if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_TRUE;
            if (surface_) vk_.vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
            if (!present) continue;
            family = i;
            timestamp_bits_ = families[i].timestampValidBits;
            return {};
        }
        return surface_ ? "no graphics queue that can present to the window" : "no graphics queue";
    }

    void pick_device() {
        std::uint32_t n = 0;
        vk_.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
        std::vector<VkPhysicalDevice> devices(n);
        vk_.vkEnumeratePhysicalDevices(instance_, &n, devices.data());
        if (devices.empty()) throw std::runtime_error("no Vulkan device");
        for (int attempt = 0; attempt < 2 && !physical_; ++attempt) {
            int best = -1, best_score = -1;
            for (std::uint32_t i = 0; i < n; ++i) {
                VkPhysicalDeviceProperties p;
                vk_.vkGetPhysicalDeviceProperties(devices[i], &p);
                std::uint32_t family = 0;
                const std::string why = unsuitable(devices[i], family);
                if (attempt == 0)
                    std::fprintf(stderr, "joan-native: vulkan device [%u] %s (%s, Vulkan %u.%u.%u)%s%s\n", i, p.deviceName,
                                 device_type(p.deviceType), VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                                 VK_API_VERSION_PATCH(p.apiVersion), why.empty() ? "" : ": unusable, ", why.c_str());
                if (!why.empty()) continue;
                const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 4
                                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 2
                                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 1
                                                                                           : 0;
                if (options_.gpu == int(i) || (options_.gpu < 0 && score > best_score)) {
                    best = int(i);
                    best_score = score;
                    if (options_.gpu == int(i)) break;
                }
            }
            if (options_.gpu >= 0 && best != options_.gpu)
                throw std::runtime_error("--gpu " + std::to_string(options_.gpu) + ": no such usable Vulkan device");
            if (best >= 0) {
                physical_ = devices[std::size_t(best)];
                unsuitable(physical_, queue_family_);  // re-derive family/timestamp bits for the winner
                vk_.vkGetPhysicalDeviceProperties(physical_, &props_);
                std::fprintf(stderr, "joan-native: vulkan using [%d] %s%s\n", best, props_.deviceName, surface_ ? "" : " (offscreen)");
            } else if (surface_) {
                // e.g. hardware driver on Xvfb (no DRI3): keep rendering, just don't present.
                std::fputs("joan-native: vulkan: no device can present to this window, rendering offscreen\n", stderr);
                vk_.vkDestroySurfaceKHR(instance_, surface_, nullptr);
                surface_ = VK_NULL_HANDLE;
            }
        }
        if (!physical_) throw std::runtime_error("no usable Vulkan 1.3 device");
        vk_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory_props_);
        probe_rt();
    }

    // Ray tracing capability (docs/RAYTRACING.md). JOA_VK_DISABLE_RT=1 forces the raster fallback.
    void probe_rt() {
        std::uint32_t n = 0;
        vk_.vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vk_.vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, exts.data());
        const auto has = [&](const char* e) {
            return std::any_of(exts.begin(), exts.end(), [&](const auto& p) { return std::strcmp(p.extensionName, e) == 0; });
        };
        RtCaps& c = rt_caps_;
        c.deferred_host_operations = has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        c.ray_tracing_pipeline = has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        const bool as_ext = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME), rq_ext = has(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        // Extension feature structs only when the extension exists.
        if (as_ext) as.pNext = f12.pNext, f12.pNext = &as;
        if (rq_ext) rq.pNext = f12.pNext, f12.pNext = &rq;
        vk_.vkGetPhysicalDeviceFeatures2(physical_, &f);
        c.acceleration_structure = as_ext && as.accelerationStructure;
        c.ray_query = rq_ext && rq.rayQuery;
        c.buffer_device_address = f12.bufferDeviceAddress;
        c.storage_image_indexing = f.features.shaderStorageImageArrayDynamicIndexing;
        const char* off = std::getenv("JOA_VK_DISABLE_RT");
        c.forced_off = off && *off && std::strcmp(off, "0") != 0;
        g_rt_available = c.usable();
        if (c.usable())
            std::fprintf(stderr, "joan-native: vulkan ray tracing available (ray query + acceleration structures%s)\n",
                         c.ray_tracing_pipeline ? "; RT pipeline present, unused" : "");
        else
            std::fprintf(stderr, "joan-native: vulkan ray tracing unavailable: %s\n", c.missing().c_str());
    }

    void create_device() {
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, queue_family_, 1, &priority};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.timelineSemaphore = VK_TRUE;
        f12.descriptorIndexing = VK_TRUE;
        f12.runtimeDescriptorArray = VK_TRUE;
        f12.descriptorBindingPartiallyBound = VK_TRUE;
        f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        f13.shaderDemoteToHelperInvocation = VK_TRUE;  // glslc lowers `discard` to demote for Vulkan 1.3
        VkPhysicalDeviceFeatures core{};
        core.samplerAnisotropy = anisotropy_;
        core.independentBlend = independent_blend_;  // remaster stencil fill: colour + masked ambient attachment
        // AMD FSR 3.1.4 (fsr_vk.hpp): its compute shaders write (and frame interpolation reads) formatless storage images.
        fsr_ffx_ = Fsr3::load(vk_.vkGetInstanceProcAddr, instance_);
        fsr_missing_ = fsr_ffx_ ? Fsr3::unsupported(vk_, physical_) : "Vulkan entry points missing";
        generation_missing_ = fsr_missing_ ? fsr_missing_ : Fsr3::generation_unsupported(vk_, physical_, kFgFormat);
        if (!fsr_missing_) {
            core.shaderStorageImageWriteWithoutFormat = core.shaderStorageImageExtendedFormats = VK_TRUE;
            core.shaderStorageImageReadWithoutFormat = !generation_missing_;
        }
        std::fprintf(stderr, "joan-native: vulkan FSR 3.1.4 %s%s; frame generation %s%s\n", fsr_missing_ ? "unavailable: " : "available",
                     fsr_missing_ ? fsr_missing_ : "", generation_missing_ ? "unavailable: " : "available",
                     generation_missing_ ? generation_missing_ : "");
        std::vector<const char*> exts;
        if (surface_) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        // Ray tracing: ray queries over acceleration structures, buffer references to the mesh data.
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq};
        if (rt_caps_.usable()) {
            f12.bufferDeviceAddress = VK_TRUE;
            f12.pNext = &as;
            as.accelerationStructure = VK_TRUE;
            rq.rayQuery = VK_TRUE;
            core.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
            for (const char* e : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                  VK_KHR_RAY_QUERY_EXTENSION_NAME})
                exts.push_back(e);
        }
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13};
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = std::uint32_t(exts.size());
        info.ppEnabledExtensionNames = exts.data();
        info.pEnabledFeatures = &core;
        check(vk_.vkCreateDevice(physical_, &info, nullptr, &device_), "vkCreateDevice");
        vk_.load_device(device_);
        vk_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, nullptr, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        const VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        check(vk_.vkCreateSemaphore(device_, &sem, nullptr, &timeline_), "vkCreateSemaphore");
        const VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                           queue_family_};
        check(vk_.vkCreateCommandPool(device_, &pool, nullptr, &upload_pool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, upload_pool_,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        check(vk_.vkAllocateCommandBuffers(device_, &alloc, &upload_cmd_), "vkAllocateCommandBuffers");
        check(vk_.vkCreateCommandPool(device_, &pool, nullptr, &async_pool_), "vkCreateCommandPool");
    }

    // [repeat][anisotropic]: mipmapped textures use `[graphics] anisotropy` (default 8x, as GL); aniso = false: plain linear.
    void create_samplers(bool aniso) {
        for (int repeat = 0; repeat < 2; ++repeat) {
            VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            s.magFilter = s.minFilter = VK_FILTER_LINEAR;
            s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            s.maxLod = VK_LOD_CLAMP_NONE;
            s.addressModeU = s.addressModeV = s.addressModeW = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            s.anisotropyEnable = aniso && anisotropy_ && gfx_.anisotropy > 1;
            s.maxAnisotropy = s.anisotropyEnable ? std::min(float(gfx_.anisotropy), props_.limits.maxSamplerAnisotropy) : 1.0f;
            check(vk_.vkCreateSampler(device_, &s, nullptr, &samplers_[repeat][aniso]), "vkCreateSampler");
        }
    }

    void create_descriptors() {
        create_samplers(false);
        create_samplers(true);

        // Set 0: one bindless table of every texture, updated in place (slots are only reused after the GPU retires them).
        const VkDescriptorBindingFlags bindless = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                  VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        const VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, nullptr,
                                                                1, &bindless};
        // FRAGMENT + COMPUTE: the ray-traced compute passes fetch albedo for alpha tests and hit shading.
        const VkDescriptorSetLayoutBinding tex{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures,
                                               VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags,
                                           VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 1, &tex};
        check(vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &textures_layout_), "vkCreateDescriptorSetLayout");
        // Set 1: per frame slot, frame uniforms + draw table.
        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorSetLayoutBinding frame[3] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
                                                       {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr},
                                                       {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr}};  // lights: per-pixel in the remaster path
        li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 3, frame};
        check(vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &frame_layout_), "vkCreateDescriptorSetLayout");

        const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures},
                                               {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kFramesInFlight}};
        const VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,
                                            VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 1 + kFramesInFlight, 3, sizes};
        check(vk_.vkCreateDescriptorPool(device_, &pi, nullptr, &descriptor_pool_), "vkCreateDescriptorPool");
        const VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptor_pool_, 1, &textures_layout_};
        check(vk_.vkAllocateDescriptorSets(device_, &ai, &textures_set_), "vkAllocateDescriptorSets");

        const VkDescriptorSetLayout sets[2] = {textures_layout_, frame_layout_};
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(std::uint32_t)};
        const VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 2, sets, 1, &push};
        check(vk_.vkCreatePipelineLayout(device_, &pl, nullptr, &layout_), "vkCreatePipelineLayout");
        // Shadow fill: no descriptors, the gamma-converted shadow colour as a push constant.
        const VkPushConstantRange fill_push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(glm::vec4)};
        const VkPipelineLayoutCreateInfo fl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 1, &fill_push};
        check(vk_.vkCreatePipelineLayout(device_, &fl, nullptr, &fill_layout_), "vkCreatePipelineLayout");
    }

    void load_pipeline_cache() {
        std::vector<char> data;
        if (const auto path = pipeline_cache_path(); !path.empty()) {
            std::ifstream f(path, std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(f), {});
        }
        // Only hand the driver data written by this exact device/driver (VkPipelineCacheHeaderVersionOne).
        auto u32 = [&](std::size_t at) {
            std::uint32_t v;
            std::memcpy(&v, data.data() + at, 4);
            return v;
        };
        const bool valid = data.size() >= 32 && u32(0) >= 32 && u32(4) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                           u32(8) == props_.vendorID && u32(12) == props_.deviceID &&
                           std::memcmp(data.data() + 16, props_.pipelineCacheUUID, VK_UUID_SIZE) == 0;
        const VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, nullptr, 0, valid ? data.size() : 0,
                                             valid ? data.data() : nullptr};
        check(vk_.vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_), "vkCreatePipelineCache");
        cache_loaded_ = valid;
    }

    void save_pipeline_cache() {
        const auto path = pipeline_cache_path();
        std::size_t size = 0;
        if (path.empty() || !pipeline_cache_ || vk_.vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS) return;
        std::vector<char> data(size);
        if (vk_.vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const auto tmp = path.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(data.data(), std::streamsize(size));
            if (!f) return;
        }
        std::filesystem::rename(tmp, path, ec);  // atomic replace: concurrent runs never see a torn file
    }

    // mesh: scene pass (colour + depth-stencil), else the UI pass. fill: the attribute-less shadow fill
    // triangle of the scene pass (blend ignored). zwrite: depth writes for a blended pipeline (Material::depth_write).
    // samples/a2c: the MSAA scene target of the Original look's image-quality path (alpha-to-coverage for alpha tests).
    VkPipeline make_pipeline(std::span<const std::uint32_t> vs, std::span<const std::uint32_t> fs, bool mesh, BlendMode blend,
                             bool fill = false, bool zwrite = false, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT, bool a2c = false) {
        VkShaderModule modules[2];
        for (int i = 0; i < 2; ++i) {
            const auto code = i ? fs : vs;
            const VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, code.size_bytes(), code.data()};
            check(vk_.vkCreateShaderModule(device_, &mi, nullptr, &modules[i]), "vkCreateShaderModule");
        }
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, modules[1], "main", nullptr}};
        const VkVertexInputBindingDescription mesh_binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription mesh_attrs[5] = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
            {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv2)},
            {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}};  // 3/4 (bones/weights): CPU-skinned, unbound
        const VkVertexInputBindingDescription ui_binding{0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription ui_attrs[4] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, position)},
                                                               {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, uv)},
                                                               {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UiVertex, color)},
                                                               {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UiVertex, uv_rect)}};
        const VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
                                                          fill ? 0u : 1u, mesh ? &mesh_binding : &ui_binding,
                                                          fill ? 0u : mesh ? 5u : 4u, mesh ? mesh_attrs : ui_attrs};
        const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
        const VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;  // dynamic
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;  // GL CCW winding survives the clip-space y flip
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0, samples};
        ms.alphaToCoverageEnable = a2c;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        const bool solid = blend == BlendMode::Opaque || blend == BlendMode::AlphaTest;
        const bool volume = blend == BlendMode::ShadowVolume;
        depth.depthTestEnable = mesh && !fill;
        depth.depthWriteEnable = mesh && !fill && (solid || zwrite);
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        // Stencil (reference dynamic): solid draws write 128 on shadow receivers else 0; volumes count
        // front +1 / back -1 (clamped, the original's INCRSAT/DECRSAT); the fill passes where 128 < stencil.
        VkStencilOpState keep{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0};
        depth.stencilTestEnable = mesh && (solid || volume || fill);
        depth.front = depth.back = keep;
        if (fill) depth.front.compareOp = depth.back.compareOp = VK_COMPARE_OP_LESS;
        else if (volume) depth.front.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP, depth.back.passOp = VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        else depth.front.passOp = depth.back.passOp = VK_STENCIL_OP_REPLACE;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = volume && !fill ? 0
                                             : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (fill || blend == BlendMode::AlphaBlend || blend == BlendMode::Additive) {  // glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA | ONE)
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            att.dstColorBlendFactor = att.dstAlphaBlendFactor =
                blend == BlendMode::Additive && !fill ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        const VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE,
                                                     VK_LOGIC_OP_COPY, 1, &att};
        const VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                                       VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, mesh ? 4u : 3u, dyn};
        // The UI (mesh == false) renders in its own pass into a UNORM view of the colour image, without depth
        // (gamma-space blending as in D3D8; see shaders/vk/ui.frag).
        const VkFormat color_format = mesh ? kColorFormat : kUiColorFormat;
        const VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 0, 1, &color_format,
                                                      mesh ? depth_format_ : VK_FORMAT_UNDEFINED, mesh ? depth_format_ : VK_FORMAT_UNDEFINED};
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &cb;
        info.pDynamicState = &dynamic;
        info.layout = fill ? fill_layout_ : layout_;
        VkPipeline p = VK_NULL_HANDLE;
        const VkResult r = vk_.vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &p);
        for (VkShaderModule m : modules) vk_.vkDestroyShaderModule(device_, m, nullptr);
        check(r, "vkCreateGraphicsPipelines");
        return p;
    }

    void create_pipelines() {
        // Depth + 8-bit stencil for the shadow volumes (the original asks for D24S8, ORIGINAL_BEHAVIOR.md);
        // Vulkan guarantees one of these two.
        for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
            VkFormatProperties p{};
            vk_.vkGetPhysicalDeviceFormatProperties(physical_, f, &p);
            if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                depth_format_ = f;
                break;
            }
        }
        if (depth_format_ == VK_FORMAT_UNDEFINED) throw std::runtime_error("vulkan: no depth-stencil attachment format");
        load_pipeline_cache();
        opaque_ = make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::Opaque);  // alpha test = discard in the same pipeline
        blend_ = make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::AlphaBlend);
        blend_zwrite_ = make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::AlphaBlend, false, true);
        additive_ = make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::Additive);
        volume_ = make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::ShadowVolume);
        fill_ = make_pipeline(kFillVert, kFillFrag, true, BlendMode::AlphaBlend, true);
        ui_ = make_pipeline(kUiVert, kUiFrag, false, BlendMode::AlphaBlend);
        std::fprintf(stderr, "joan-native: vulkan pipeline cache %s (%s)\n", cache_loaded_ ? "loaded" : "cold",
                     pipeline_cache_path().c_str());
    }

    void create_slots() {
        const bool timing = options_.gpu_timing && timestamp_bits_ > 0;
        if (options_.gpu_timing && !timing) std::fputs("joan-native: vulkan: queue has no timestamp support, --gpu-timing ignored\n", stderr);
        for (Slot& s : slots_) {
            const VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                               queue_family_};
            check(vk_.vkCreateCommandPool(device_, &pool, nullptr, &s.pool), "vkCreateCommandPool");
            VkCommandBuffer cmds[3];
            const VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, s.pool,
                                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 3};
            check(vk_.vkAllocateCommandBuffers(device_, &alloc, cmds), "vkAllocateCommandBuffers");
            s.scene = cmds[0];
            s.present = cmds[1];
            s.present_generated = cmds[2];
            const VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vk_.vkCreateSemaphore(device_, &sem, nullptr, &s.acquired), "vkCreateSemaphore");
            check(vk_.vkCreateSemaphore(device_, &sem, nullptr, &s.acquired_generated), "vkCreateSemaphore");
            s.uniforms = create_buffer(sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHost, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            const VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptor_pool_, 1, &frame_layout_};
            check(vk_.vkAllocateDescriptorSets(device_, &ai, &s.set), "vkAllocateDescriptorSets");
            ensure(s, s.lights, 64 * sizeof(LightGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            ensure(s, s.draws, 256 * sizeof(DrawGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (timing) {
                const VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, QCount};
                check(vk_.vkCreateQueryPool(device_, &q, nullptr, &s.queries), "vkCreateQueryPool");
            }
        }
    }

    // ---- resources --------------------------------------------------------------------------------

    Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
        Buffer b;
        b.size = size;
        const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
        check(vk_.vkCreateBuffer(device_, &info, nullptr, &b.buffer), "vkCreateBuffer");
        VkMemoryRequirements req;
        vk_.vkGetBufferMemoryRequirements(device_, b.buffer, &req);
        b.memory = allocator_.allocate(req, required, preferred, true);
        check(vk_.vkBindBufferMemory(device_, b.buffer, b.memory.memory, b.memory.offset), "vkBindBufferMemory");
        return b;
    }

    void destroy(Buffer& b) {
        if (!b.buffer) return;
        vk_.vkDestroyBuffer(device_, b.buffer, nullptr);
        allocator_.free(b.memory);
        b = {};
    }

    Image2D create_image(VkFormat format, VkExtent2D extent, std::uint32_t mips, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                         VkImageCreateFlags flags = 0, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
        Image2D img;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.flags = flags;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = mips;
        info.arrayLayers = 1;
        info.samples = samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        check(vk_.vkCreateImage(device_, &info, nullptr, &img.image), "vkCreateImage");
        VkMemoryRequirements req;
        vk_.vkGetImageMemoryRequirements(device_, img.image, &req);
        img.memory = allocator_.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false);
        check(vk_.vkBindImageMemory(device_, img.image, img.memory.memory, img.memory.offset), "vkBindImageMemory");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = range(aspect);
        check(vk_.vkCreateImageView(device_, &vi, nullptr, &img.view), "vkCreateImageView");
        return img;
    }

    void destroy(Image2D& i) {
        if (i.view) vk_.vkDestroyImageView(device_, i.view, nullptr);
        if (i.image) vk_.vkDestroyImage(device_, i.image, nullptr);
        allocator_.free(i.memory);
        i = {};
    }
    void destroy(Texture& t) {
        Image2D i{t.image, t.view, t.memory};
        destroy(i);
    }
    void destroy(Mesh& m) {
        destroy(m.vertices);
        destroy(m.indices);
        destroy(m.prev_owned);
    }

    // Grows a per-slot host buffer (the slot is idle: begin_frame waited for it).
    void ensure(Slot& s, Buffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage) {
        if (b.buffer && b.size >= bytes) return;
        destroy(b);
        b = create_buffer(std::max<VkDeviceSize>(bytes, 4096) * 3 / 2, usage, kHost, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (usage != VK_BUFFER_USAGE_STORAGE_BUFFER_BIT || !s.draws.buffer || !s.lights.buffer) return;
        const VkDescriptorBufferInfo infos[3] = {
            {s.uniforms.buffer, 0, VK_WHOLE_SIZE}, {s.draws.buffer, 0, VK_WHOLE_SIZE}, {s.lights.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[3]{};
        for (std::uint32_t i = 0; i < 3; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s.set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[i].pBufferInfo = &infos[i];
        }
        vk_.vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    }

    void create_target(glm::ivec2 size) {
        vk_.vkDeviceWaitIdle(device_);
        destroy_target();
        const VkExtent2D e{std::uint32_t(size.x), std::uint32_t(size.y)};
        color_ = create_image(kColorFormat, e, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = color_.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kUiColorFormat;
        vi.subresourceRange = range(VK_IMAGE_ASPECT_COLOR_BIT);
        check(vk_.vkCreateImageView(device_, &vi, nullptr, &color_ui_view_), "vkCreateImageView");
        depth_ = create_image(depth_format_, e, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        target_size_ = size;
        scene_rendered_ = false;
    }

    void destroy_target() {
        if (color_ui_view_) vk_.vkDestroyImageView(device_, color_ui_view_, nullptr);
        color_ui_view_ = VK_NULL_HANDLE;
        destroy(color_);
        destroy(depth_);
        target_size_ = {0, 0};
    }

    void destroy_swapchain(VkSwapchainKHR s) {
        if (s) vk_.vkDestroySwapchainKHR(device_, s, nullptr);
    }

    void recreate_swapchain() {
        vk_.vkDeviceWaitIdle(device_);
        VkSurfaceCapabilitiesKHR caps;
        check(vk_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window_, &w, &h);
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu)
            extent = {std::clamp(std::uint32_t(std::max(w, 0)), caps.minImageExtent.width, caps.maxImageExtent.width),
                      std::clamp(std::uint32_t(std::max(h, 0)), caps.minImageExtent.height, caps.maxImageExtent.height)};
        swapchain_dirty_ = false;
        if (extent.width == 0 || extent.height == 0) {  // minimised: render, don't present
            destroy_swapchain(swapchain_);
            swapchain_ = VK_NULL_HANDLE;
            swapchain_extent_ = {std::uint32_t(w), std::uint32_t(h)};
            return;
        }

        std::uint32_t n = 0;
        vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &n, formats.data());
        // 8-bit sRGB preferred (the colour target's bytes blit unchanged), else an 8-bit UNORM one that present() fills
        // through present_bridge_, else whatever the surface lists first.
        const auto rank = [](const VkSurfaceFormatKHR& f) {
            if (f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return 0;
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) return 2;
            return f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM ? 1 : 0;
        };
        VkSurfaceFormatKHR format = formats.at(0);
        for (const auto& f : formats)
            if (rank(f) > rank(format)) format = f;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw std::runtime_error("Vulkan: the surface's swapchain images cannot be transfer destinations (presentation blits into them)");
        vk_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vk_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &n, modes.data());
        const VkPresentModeKHR wanted = options_.present_mode == PresentMode::Mailbox     ? VK_PRESENT_MODE_MAILBOX_KHR
                                        : options_.present_mode == PresentMode::Immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                        : options_.present_mode == PresentMode::Relaxed   ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
                                                                                          : VK_PRESENT_MODE_FIFO_KHR;
        const VkPresentModeKHR mode = std::find(modes.begin(), modes.end(), wanted) != modes.end() ? wanted : VK_PRESENT_MODE_FIFO_KHR;

        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface_;
        info.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount) : caps.minImageCount + 1;
        info.imageFormat = format.format;
        info.imageColorSpace = format.colorSpace;
        info.imageExtent = extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                  ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                  : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
        info.presentMode = mode;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain_;
        VkSwapchainKHR created = VK_NULL_HANDLE;
        check(vk_.vkCreateSwapchainKHR(device_, &info, nullptr, &created), "vkCreateSwapchainKHR");
        destroy_swapchain(swapchain_);
        swapchain_ = created;
        swapchain_extent_ = extent;
        swapchain_srgb_ = format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_SRGB ||
                          format.format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
        vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        swapchain_images_.resize(n);
        vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapchain_images_.data());
        // One "render done" semaphore per image: present may still hold the previous one for an image.
        for (VkSemaphore s : present_done_) vk_.vkDestroySemaphore(device_, s, nullptr);
        present_done_.assign(n, VK_NULL_HANDLE);
        const VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore& s : present_done_) check(vk_.vkCreateSemaphore(device_, &sem, nullptr, &s), "vkCreateSemaphore");
        static const char* const names[] = {"immediate", "mailbox", "fifo", "fifo-relaxed"};
        std::fprintf(stderr, "joan-native: vulkan swapchain %ux%u, %u images, %s%s\n", extent.width, extent.height, n,
                     mode <= VK_PRESENT_MODE_FIFO_RELAXED_KHR ? names[mode] : "?", swapchain_srgb_ ? "" : ", non-sRGB format (bridged)");
    }

    // ---- frame ------------------------------------------------------------------------------------

    void render_pass(const FrameData* f) {
        Slot& s = slots_[slot_];
        if (s.timed) timestamp(QSceneBegin);
        if (f && remaster_) return render_remaster(s, *f);
        if (f && gfx_.original_post()) return render_original_post(s, *f);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = color_.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // outside the viewport is always black
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, {std::uint32_t(target_size_.x), std::uint32_t(target_size_.y)}};
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;
        info.pStencilAttachment = &depth;  // same image and clear (stencil 0)
        vk_.vkCmdBeginRendering(cmd_, &info);
        if (f) {
            draw_scene(s, *f, viewport(*f), base_pipes(), false);
            if (s.timed) timestamp(QUiBegin);
        } else if (s.timed) {
            timestamp(QUiBegin);
        }
        vk_.vkCmdEndRendering(cmd_);
        if (!f || (f->ui.empty() && f->ui_triangles.empty())) return;
        // UI pass: same image through the UNORM view, after the scene's colour writes.
        image_barrier(cmd_, color_.image, range(VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo ui_color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ui_color.imageView = color_ui_view_;
        ui_color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ui_color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ui_color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        info.pColorAttachments = &ui_color;
        info.pDepthAttachment = info.pStencilAttachment = nullptr;
        vk_.vkCmdBeginRendering(cmd_, &info);
        draw_ui(s, *f);
        vk_.vkCmdEndRendering(cmd_);
    }

    void finish_scene() {
        Slot& s = slots_[slot_];
        if (s.timed) timestamp(QSceneEnd);
        image_barrier(cmd_, color_.image, range(VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        check(vk_.vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
        const VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, timeline_, ++timeline_value_,
                                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
        const VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmd_, 0};
        const VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, 0, nullptr, 1, &cb, 1, &signal};
        check(vk_.vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
        s.retire_value = timeline_value_;
        recording_ = false;
        scene_rendered_ = true;
    }

    std::uint32_t texture_slot(TextureHandle h) const {
        const auto it = textures_.find(h.id);
        return it == textures_.end() ? textures_.at(white_.id).slot : it->second.slot;
    }

    struct ScenePipes {  // Original-look mesh pipelines of one sample count (alpha_test: alpha-to-coverage when multisampled)
        VkPipeline opaque, alpha_test, blend, blend_zwrite, additive, volume, fill;
    };
    ScenePipes base_pipes() const { return {opaque_, opaque_, blend_, blend_zwrite_, additive_, volume_, fill_}; }

    glm::ivec4 viewport(const FrameData& f) const {
        return f.view.viewport.z > 0 ? f.view.viewport : glm::ivec4(0, 0, target_size_.x, target_size_.y);
    }

    // vp: scene viewport (top-left origin) in the bound attachments; a2c: alpha-tested draws use alpha-to-coverage (MSAA).
    void draw_scene(Slot& s, const FrameData& f, glm::ivec4 vp, const ScenePipes& pipes, bool a2c) {
        const glm::vec4& cc = f.lighting.clear_color;
        const VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{cc.r, cc.g, cc.b, cc.a}}}};
        const VkClearRect rect{{{vp.x, vp.y}, {std::uint32_t(vp.z), std::uint32_t(vp.w)}}, 0, 1};
        vk_.vkCmdClearAttachments(cmd_, 1, &clear, 1, &rect);

        const Lighting& l = f.lighting;
        upload_frame_uniforms(s, f);

        // Opaque + alpha-tested first (stencil 128 on shadow receivers, else 0), then the shadow volumes and
        // the shadow fill, then blended back to front (depth writes only for Material::depth_write; same order as GL).
        order_.clear();
        for (const DrawItem& d : f.draws)
            if (d.material.blend == BlendMode::Opaque || d.material.blend == BlendMode::AlphaTest) order_.push_back(&d);
        const std::size_t first_volume = order_.size();
        if (l.shadow_color.a > 0.0f)
            for (const DrawItem& d : f.draws)
                if (d.material.blend == BlendMode::ShadowVolume) order_.push_back(&d);
        const std::size_t first_blended = order_.size();
        for (const DrawItem& d : f.draws)
            if (d.material.blend == BlendMode::AlphaBlend || d.material.blend == BlendMode::Additive) order_.push_back(&d);
        const glm::vec3 cam = f.view.camera_position;
        std::sort(order_.begin() + long(first_blended), order_.end(), [&](const DrawItem* a, const DrawItem* b) {
            const glm::vec3 pa(a->model[3]), pb(b->model[3]);
            return glm::dot(pa - cam, pa - cam) > glm::dot(pb - cam, pb - cam);
        });
        upload_draws(s, l, a2c);

        const VkDescriptorSet sets[2] = {textures_set_, s.set};
        vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
        const VkViewport viewport{float(vp.x), float(vp.y), float(vp.z), float(vp.w), 0.0f, 1.0f};
        vk_.vkCmdSetViewport(cmd_, 0, 1, &viewport);
        vk_.vkCmdSetScissor(cmd_, 0, 1, &rect.rect);
        VkPipeline bound = VK_NULL_HANDLE;
        const Mesh* bound_mesh = nullptr;
        int cull = -1;
        std::uint32_t stencil_ref = 0;
        vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref);
        // Fill where 128 < stencil (D3D STENCILFUNC LESS, ref 128), Lighting::shadow_color.
        const auto fill = [&] {
            vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = pipes.fill);
            vk_.vkCmdSetCullMode(cmd_, VK_CULL_MODE_NONE);
            cull = -1;
            vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref = 128);
            const glm::vec4 color = shadow_fill_color(l.shadow_color);
            vk_.vkCmdPushConstants(cmd_, fill_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof color, &color);
            vk_.vkCmdDraw(cmd_, 3, 1, 0, 0);
        };
        for (std::size_t i = 0; i < order_.size(); ++i) {
            if (i == first_blended && first_blended > first_volume) fill();
            const DrawItem& d = *order_[i];
            const auto it = meshes_.find(d.mesh.id);
            if (it == meshes_.end()) continue;
            const Mesh& m = it->second;
            const std::uint32_t first = std::min(d.first_index, m.index_count);
            const std::uint32_t count = d.index_count ? std::min(d.index_count, m.index_count - first) : m.index_count - first;
            if (!count) continue;
            const BlendMode b = d.material.blend;
            const VkPipeline p = b == BlendMode::AlphaBlend     ? (d.material.depth_write ? pipes.blend_zwrite : pipes.blend)
                                 : b == BlendMode::Additive     ? pipes.additive
                                 : b == BlendMode::ShadowVolume ? pipes.volume
                                 : b == BlendMode::AlphaTest    ? pipes.alpha_test
                                                                : pipes.opaque;
            if (p != bound) vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
            if (const std::uint32_t ref = i < first_volume && d.material.receive_shadow ? 128 : 0; i < first_volume && ref != stencil_ref)
                vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref = ref);
            if (int(d.material.two_sided) != cull) {
                cull = d.material.two_sided;
                vk_.vkCmdSetCullMode(cmd_, d.material.two_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            }
            if (&m != bound_mesh) {
                vk_.vkCmdBindVertexBuffers(cmd_, 0, 1, &m.vertices.buffer, &m.vertex_offset);
                vk_.vkCmdBindIndexBuffer(cmd_, m.indices.buffer, m.index_offset, VK_INDEX_TYPE_UINT32);
                bound_mesh = &m;
            }
            vk_.vkCmdDrawIndexed(cmd_, count, 1, first, 0, std::uint32_t(i));  // firstInstance = draw table index
        }
        if (first_blended == order_.size() && first_blended > first_volume) fill();
    }

    // Set 1 frame block (mesh lighting/fog, and the UI's framebuffer size).
    void upload_frame_uniforms(Slot& s, const FrameData& f) {
        const Lighting& l = f.lighting;
        FrameUniforms u;
        u.view_proj = kClipCorrection * f.view.projection * f.view.view;
        u.sun_dir = glm::vec4(glm::normalize(l.sun_direction), 0.0f);
        u.sun_color = glm::vec4(l.sun_color, 0.0f);
        u.ambient = glm::vec4(l.ambient, 0.0f);
        u.fog_color = glm::vec4(l.fog_color, 0.0f);
        u.fog_range = glm::vec4(l.fog_start, l.fog_end, 0.0f, 0.0f);
        u.camera_pos = glm::vec4(f.view.camera_position, 1.0f);
        u.framebuffer = glm::vec4(float(target_size_.x), float(target_size_.y), 0.0f, 0.0f);
        std::memcpy(s.uniforms.memory.mapped, &u, sizeof u);
    }

    // Point-light SSBO + the draw table (order_) for the frame slot `s`; a2c: flag alpha-tested draws (mesh.frag bit 2).
    void upload_draws(Slot& s, const Lighting& l, bool a2c = false) {
        ensure(s, s.lights, std::max<std::size_t>(l.point_lights.size(), 1) * sizeof(LightGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto* lights = reinterpret_cast<LightGpu*>(s.lights.memory.mapped);
        for (std::size_t i = 0; i < l.point_lights.size(); ++i) {
            const PointLight& p = l.point_lights[i];
            lights[i] = {glm::vec4(p.position, p.range), glm::vec4(p.color, 0.0f), glm::vec4(p.attenuation, 0.0f)};
        }
        ensure(s, s.draws, order_.size() * sizeof(DrawGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto* gpu = reinterpret_cast<DrawGpu*>(s.draws.memory.mapped);
        for (std::size_t i = 0; i < order_.size(); ++i) {
            const DrawItem& d = *order_[i];
            const Material& m = d.material;
            const std::uint32_t layer2 = m.layer2 ? texture_slot(m.layer2) : kNoTexture;
            const std::uint32_t lightmap = m.lightmap ? texture_slot(m.lightmap) : kNoTexture;
            DrawGpu g{d.model, glm::mat4(glm::transpose(glm::inverse(glm::mat3(d.model)))), m.tint,
                      glm::vec4(m.diffuse, m.vertex_diffuse ? 1.0f : 0.0f),
                      alpha_test_cutoff(m),
                      (m.unlit ? 1u : 0u) | (m.fog ? 0u : 2u) | (a2c && m.blend == BlendMode::AlphaTest ? 4u : 0u) | (m.volume_caster ? 8u : 0u),
                      texture_slot(m.albedo),
                      layer2, lightmap, 0, m.bump && m.bump_scale != 0.0f ? texture_slot(m.bump) : kNoTexture, m.bump_scale, {}};
            for (int k = 0; k < std::min<int>(d.light_count, kMaxDrawLights); ++k)
                if (d.lights[k] < l.point_lights.size())
                    g.lights[g.light_count / 2] |= std::uint32_t(d.lights[k]) << (16 * (g.light_count % 2)), ++g.light_count;
            gpu[i] = g;  // one write: the mapped buffer may be write-combined
        }
    }

    void draw_ui(Slot& s, const FrameData& f) {
        const std::span<const UiQuad> quads = f.ui;
        if (quads.empty() && f.ui_triangles.empty()) return;
        const std::size_t tri_base = quads.size() * 6;
        ensure(s, s.ui, (tri_base + f.ui_vertices.size()) * sizeof(UiVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        auto* v = reinterpret_cast<UiVertex*>(s.ui.memory.mapped);
        for (const UiQuad& q : quads) {
            const glm::vec2 p0 = q.position, p1 = q.position + q.size;
            const glm::vec4 r{glm::min(q.uv0, q.uv1) + q.clamp_inset, glm::max(q.uv0, q.uv1) - q.clamp_inset};
            const glm::vec2 uv[4] = {q.uv0, {q.uv1.x, q.uv0.y}, q.uv1, {q.uv0.x, q.uv1.y}};  // TL TR BR BL, turned by q.turns
            const UiVertex c[4] = {{p0, uv[q.turns & 3], q.color, r}, {{p1.x, p0.y}, uv[(q.turns + 1) & 3], q.color, r},
                                   {p1, uv[(q.turns + 2) & 3], q.color, r}, {{p0.x, p1.y}, uv[(q.turns + 3) & 3], q.color, r}};
            for (int k : {0, 1, 2, 0, 2, 3}) *v++ = c[k];
        }
        if (!f.ui_vertices.empty()) std::memcpy(v, f.ui_vertices.data(), f.ui_vertices.size_bytes());
        const VkDescriptorSet sets[2] = {textures_set_, s.set};
        vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
        const VkViewport viewport{0.0f, 0.0f, float(target_size_.x), float(target_size_.y), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {std::uint32_t(target_size_.x), std::uint32_t(target_size_.y)}};
        vk_.vkCmdSetViewport(cmd_, 0, 1, &viewport);
        vk_.vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_);
        vk_.vkCmdSetCullMode(cmd_, VK_CULL_MODE_NONE);
        const VkDeviceSize zero = 0;
        vk_.vkCmdBindVertexBuffers(cmd_, 0, 1, &s.ui.buffer, &zero);
        // Batch runs of quads sharing a texture.
        std::size_t start = 0;
        for (std::size_t i = 1; i <= quads.size(); ++i) {
            if (i < quads.size() && quads[i].texture.id == quads[start].texture.id) continue;
            const std::uint32_t slot = texture_slot(quads[start].texture);
            vk_.vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof slot, &slot);
            vk_.vkCmdDraw(cmd_, std::uint32_t((i - start) * 6), 1, std::uint32_t(start * 6), 0);
            start = i;
        }
        for (const UiTriangles& t : f.ui_triangles) {
            const glm::ivec4 c = glm::clamp(t.clip, glm::ivec4(0), glm::ivec4(target_size_, target_size_));
            if (c.z <= c.x || c.w <= c.y || !t.count) continue;
            const VkRect2D clip{{c.x, c.y}, {std::uint32_t(c.z - c.x), std::uint32_t(c.w - c.y)}};
            vk_.vkCmdSetScissor(cmd_, 0, 1, &clip);
            const std::uint32_t slot = texture_slot(t.texture);
            vk_.vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof slot, &slot);
            vk_.vkCmdDraw(cmd_, t.count, 1, std::uint32_t(tri_base + t.first), 0);
        }
    }

    // ---- remaster (Enhanced/Ultra): same passes and shaders as renderer_gl.cpp draw_scene_remaster ------

    enum class RmStencil { None, Write, Volume, Fill };
    enum class RmBlend { Off, Alpha, Add, RevSub };
    struct RmPipe {
        std::span<const std::uint32_t> vs, fs;
        bool mesh = false;
        std::vector<VkFormat> colors;
        VkFormat depth = VK_FORMAT_UNDEFINED;
        bool stencil_attachment = false;
        bool depth_test = false, depth_write = false;
        RmStencil stencil = RmStencil::None;
        RmBlend blend = RmBlend::Off;
        VkColorComponentFlags write_mask = 0xf;  // attachment 0
        bool mask_rest = false;                  // attachments 1.. write nothing
        bool depth_bias = false;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;  // MSAA scene passes (msaa > 1)
        bool a2c = false;                                       // alpha-to-coverage (alpha-tested draws under MSAA)
    };
    // Remaster scene-pass pipelines of one sample count (opaque_a2c only when multisampled).
    struct RmScenePipes {
        VkPipeline opaque, opaque_a2c, volume, fill, blend, blend_zwrite, additive, ao_apply;
    };
    struct MaterialExtraGpu {  // std430 mirror of MaterialExtra in shaders/vk/remaster_vk.glsl
        glm::mat4 prev_model;
        std::uint32_t normal, rough, metal, ao;
        std::int32_t cls, blend;
        float water;  // Material::water when `water=enhanced`, else -1
        std::int32_t pad1;
    };
    static_assert(sizeof(MaterialExtraGpu) == 96);
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkShaderStageFlags kRmStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    static constexpr VkFormat kLdrFormat = VK_FORMAT_R8G8B8A8_UNORM;
    // TAA motion vectors (mesh_pbr.glsl o_motion, location 2): attachment 2 of the opaque and translucent passes (the
    // translucent pass's attachment 1 is the then unused ambient target).
    static constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
    // Auto exposure reference (mesh_pbr.glsl o_reference, location 3): attachment 3 of the opaque and translucent passes,
    // blended like the HDR colour (post_exposure_lum.glsl).
    static constexpr VkFormat kReferenceFormat = VK_FORMAT_R16_SFLOAT;
    // set 2 (remaster_vk.glsl); 14-17: SMAA edges, weights, area, search; 18-21: auto exposure previous state, measurement,
    // state, reference; 22-23: stylised water refraction colour (RmTargets::opaque) and depth (pbr_shade.glsl shade_water);
    // 24: interactive water wave field (Slot::waves)
    static constexpr std::uint32_t kRmBindings = 25;
    static constexpr VkFormat kWavesFormat = VK_FORMAT_R16_SFLOAT;  // linear filtering is mandatory for R16F (not R32F)
    static constexpr VkFormat kExposureFormat = VK_FORMAT_R32_SFLOAT;  // 1x1 auto exposure state (post_exposure_adapt.glsl)

    VkPipeline make_rm_pipeline(const RmPipe& p) {
        VkShaderModule modules[2];
        for (int i = 0; i < 2; ++i) {
            const auto code = i ? p.fs : p.vs;
            const VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, code.size_bytes(), code.data()};
            check(vk_.vkCreateShaderModule(device_, &mi, nullptr, &modules[i]), "vkCreateShaderModule");
        }
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules[0], "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, modules[1], "main", nullptr}};
        // Binding 1 = the previous frame's vertices (CPU-skinned meshes, else binding 0's buffer): location 7 position.
        const VkVertexInputBindingDescription bindings[2] = {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                                             {1, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
        const VkVertexInputAttributeDescription attrs[6] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
                                                            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
                                                            {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
                                                            {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv2)},
                                                            {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)},
                                                            {7, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)}};
        const VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0,
                                                          p.mesh ? 2u : 0u, bindings, p.mesh ? 6u : 0u, attrs};
        const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
        const VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;  // dynamic
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        raster.depthBiasEnable = p.depth_bias;  // = GL glPolygonOffset(1.5, 2.0) of the shadow pass
        raster.depthBiasConstantFactor = 2.0f;
        raster.depthBiasSlopeFactor = 1.5f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0, p.samples};
        ms.alphaToCoverageEnable = p.a2c;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = p.depth_test;
        depth.depthWriteEnable = p.depth_write;
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkStencilOpState keep{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0};
        depth.stencilTestEnable = p.stencil != RmStencil::None;
        depth.front = depth.back = keep;
        if (p.stencil == RmStencil::Fill) depth.front.compareOp = depth.back.compareOp = VK_COMPARE_OP_LESS;
        else if (p.stencil == RmStencil::Volume)
            depth.front.passOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP, depth.back.passOp = VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        else if (p.stencil == RmStencil::Write) depth.front.passOp = depth.back.passOp = VK_STENCIL_OP_REPLACE;
        std::vector<VkPipelineColorBlendAttachmentState> atts(p.colors.size());
        for (std::size_t i = 0; i < atts.size(); ++i) {
            VkPipelineColorBlendAttachmentState& a = atts[i];
            a = {};
            // The remaster fill also darkens the ambient MRT (attachment 1, shadow_fill_rm.frag); the auto exposure reference
            // (attachment 3) is written and blended like the colour.
            const bool shaded = i == 0 || i == 3 || (i == 1 && p.stencil == RmStencil::Fill);
            a.colorWriteMask = i == 0 || i == 3 ? p.write_mask : shaded || !p.mask_rest ? 0xf : 0;
            if (!shaded || p.blend == RmBlend::Off) continue;
            a.blendEnable = VK_TRUE;
            a.colorBlendOp = a.alphaBlendOp = p.blend == RmBlend::RevSub ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
            a.srcColorBlendFactor = a.srcAlphaBlendFactor = p.blend == RmBlend::RevSub ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
            a.dstColorBlendFactor = a.dstAlphaBlendFactor = p.blend == RmBlend::Alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
        }
        const VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE,
                                                     VK_LOGIC_OP_COPY, std::uint32_t(atts.size()), atts.data()};
        const VkDynamicState dyn[4] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                                       VK_DYNAMIC_STATE_STENCIL_REFERENCE};
        const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 4, dyn};
        const VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 0,
                                                      std::uint32_t(p.colors.size()), p.colors.data(), p.depth,
                                                      p.stencil_attachment ? p.depth : VK_FORMAT_UNDEFINED};
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &cb;
        info.pDynamicState = &dynamic;
        info.layout = p.layout;
        VkPipeline pipe = VK_NULL_HANDLE;
        const VkResult r = vk_.vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &pipe);
        for (VkShaderModule m : modules) vk_.vkDestroyShaderModule(device_, m, nullptr);
        check(r, "vkCreateGraphicsPipelines (remaster)");
        return pipe;
    }

    // Scene-pass pipelines (opaque/volume/fill/translucent + AO apply into the HDR target) for `n` samples per pixel.
    RmScenePipes make_rm_scene(VkSampleCountFlagBits n) {
        const VkPipelineLayout L = rm_pipe_layout_;
        const VkFormat D = depth_format_;
        RmScenePipes p{};
        const std::vector<VkFormat> C{kHdrFormat, kHdrFormat, kMotionFormat, kReferenceFormat};  // HDR, ambient, motion, reference
        p.opaque = make_rm_pipeline({kMeshPbrVert, kMeshPbrFrag, true, C, D, true, true, true, RmStencil::Write, RmBlend::Off, 0xf, false, false, L, n});
        p.volume = make_rm_pipeline({kMeshPbrVert, kMeshPbrFrag, true, C, D, true, true, false, RmStencil::Volume, RmBlend::Off, 0, true, false, L, n});
        p.fill = make_rm_pipeline({kFillVert, kFillRmFrag, false, C, D, true, false, false, RmStencil::Fill, RmBlend::Alpha, 0xf, true, false,
                                     fill_layout_, n});
        p.blend = make_rm_pipeline({kMeshPbrVert, kMeshPbrFrag, true, C, D, true, true, false, RmStencil::None, RmBlend::Alpha, 0xf, true, false, L, n});
        p.blend_zwrite = make_rm_pipeline({kMeshPbrVert, kMeshPbrFrag, true, C, D, true, true, true, RmStencil::None, RmBlend::Alpha, 0xf, false,
                                             false, L, n});
        p.additive = make_rm_pipeline({kMeshPbrVert, kMeshPbrFrag, true, C, D, true, true, false, RmStencil::None, RmBlend::Add, 0xf, true, false, L, n});
        p.ao_apply = make_rm_pipeline({kPostVert, kPostAoApply, false, {kHdrFormat}, VK_FORMAT_UNDEFINED, false, false, false,
                                       RmStencil::None, RmBlend::RevSub, 0xf, false, false, L, n});
        if (n > 1)
            p.opaque_a2c = make_rm_pipeline({kMeshPbrVert, kMeshPbrFrag, true, C, D, true, true, true, RmStencil::Write, RmBlend::Off, 0xf, false,
                                             false, L, n, true});
        return p;
    }

    // Layouts, samplers, descriptor sets and pipelines, created on first remastered frame.
    void ensure_remaster() {
        if (rm_layout_) return;
        VkFormatProperties fp{};
        vk_.vkGetPhysicalDeviceFormatProperties(physical_, depth_format_, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
            throw std::runtime_error("vulkan: remaster path needs a sampleable depth-stencil format");
        if (!independent_blend_) throw std::runtime_error("vulkan: remaster path needs the independentBlend feature");
        for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
            vk_.vkGetPhysicalDeviceFormatProperties(physical_, f, &fp);
            if ((fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
                (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
                shadow_format_ = f;
                shadow_linear_ = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
                break;
            }
        }
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vk_.vkCreateSampler(device_, &si, nullptr, &rm_nearest_), "vkCreateSampler");
        si.magFilter = si.minFilter = shadow_linear_ ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.compareEnable = VK_TRUE;
        si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        check(vk_.vkCreateSampler(device_, &si, nullptr, &rm_shadow_sampler_), "vkCreateSampler");

        // Set 2 (remaster_vk.glsl): 0 frame block, 1-10 + 12 + 13 textures, 11 MaterialExtra table. Compute: the RT passes.
        VkDescriptorSetLayoutBinding b[kRmBindings];
        const VkShaderStageFlags stages = kRmStages;
        for (std::uint32_t i = 0; i < kRmBindings; ++i)
            b[i] = {i, i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : i == 11 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    1, stages, nullptr};
        const VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, kRmBindings, b};
        check(vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &rm_layout_), "vkCreateDescriptorSetLayout");
        const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight},
                                               {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (kRmBindings - 2) * kFramesInFlight}};
        const VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kFramesInFlight, 3, sizes};
        check(vk_.vkCreateDescriptorPool(device_, &pi, nullptr, &rm_pool_), "vkCreateDescriptorPool");
        for (Slot& s : slots_) {
            const VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, rm_pool_, 1, &rm_layout_};
            check(vk_.vkAllocateDescriptorSets(device_, &ai, &s.rm_set), "vkAllocateDescriptorSets");
            s.rm_ubo = create_buffer(sizeof(RemasterFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHost, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            s.rm_waves = create_buffer(VkDeviceSize(kWaterWaveGrid) * kWaterWaveGrid * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHost, 0);
            s.waves = create_image(kWavesFormat, {std::uint32_t(kWaterWaveGrid), std::uint32_t(kWaterWaveGrid)}, 1,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        const VkDescriptorSetLayout sets[3] = {textures_layout_, frame_layout_, rm_layout_};
        const VkPushConstantRange push{stages, 0, 16};  // = the RT layout's range, so sets 0-2 stay bound across both
        const VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 3, sets, 1, &push};
        check(vk_.vkCreatePipelineLayout(device_, &pl, nullptr, &rm_pipe_layout_), "vkCreatePipelineLayout");

        const VkPipelineLayout L = rm_pipe_layout_;
        const RmScenePipes one = make_rm_scene(VK_SAMPLE_COUNT_1_BIT);
        rm_opaque_ = one.opaque, rm_volume_ = one.volume, rm_fill_ = one.fill, rm_blend_ = one.blend, rm_blend_zwrite_ = one.blend_zwrite;
        rm_additive_ = one.additive, rm_ao_apply_ = one.ao_apply;
        rm_shadow_ = make_rm_pipeline({kMeshPbrVert, kShadowCasterFrag, true, {}, shadow_format_, false, true, true, RmStencil::None, RmBlend::Off,
                                       0xf, false, true, L});
        rm_ssao_ = make_rm_pipeline({kPostVert, kPostSsao, false, {VK_FORMAT_R8_UNORM}, VK_FORMAT_UNDEFINED, false, false, false,
                                     RmStencil::None, RmBlend::Off, 0xf, false, false, L});
        rm_composite_ = make_rm_pipeline({kPostVert, kPostComposite, false, {kLdrFormat}, VK_FORMAT_UNDEFINED, false, false, false,
                                          RmStencil::None, RmBlend::Off, 0xf, false, false, L});
        rm_exposure_[0] = make_rm_pipeline({kPostVert, kPostExposureLum, false, {kHdrFormat}, VK_FORMAT_UNDEFINED, false, false, false,
                                            RmStencil::None, RmBlend::Off, 0xf, false, false, L});
        rm_exposure_[1] = make_rm_pipeline({kPostVert, kPostExposureAdapt, false, {kExposureFormat}, VK_FORMAT_UNDEFINED, false, false, false,
                                            RmStencil::None, RmBlend::Off, 0xf, false, false, L});
        rm_taa_ = make_rm_pipeline({kPostVert, kPostTaa, false, {kLdrFormat}, VK_FORMAT_UNDEFINED, false, false, false, RmStencil::None,
                                    RmBlend::Off, 0xf, false, false, L});
        rm_easu_ = make_rm_pipeline({kPostVert, kPostEasu, false, {kLdrFormat}, VK_FORMAT_UNDEFINED, false, false, false, RmStencil::None,
                                     RmBlend::Off, 0xf, false, false, L});
        rm_final_ = make_rm_pipeline({kPostVert, kPostRcas, false, {kUiColorFormat}, VK_FORMAT_UNDEFINED, false, false, false,
                                      RmStencil::None, RmBlend::Off, 0xf, false, false, L});
        rm_fsr_inputs_ = make_rm_pipeline({kPostVert, kPostFsrInputs, false, {VK_FORMAT_R32_SFLOAT, kMotionFormat, VK_FORMAT_R8_UNORM},
                                           VK_FORMAT_UNDEFINED, false, false, false, RmStencil::None, RmBlend::Off, 0xf, false, false, L});
        const auto ldr_post = [&](std::span<const std::uint32_t> fs) {
            return make_rm_pipeline({kPostVert, fs, false, {kLdrFormat}, VK_FORMAT_UNDEFINED, false, false, false, RmStencil::None, RmBlend::Off, 0xf,
                                     false, false, L});
        };
        rm_fxaa_ = ldr_post(kPostFxaa);
        rm_smaa_[0] = ldr_post(kPostSmaaEdges);
        rm_smaa_[1] = ldr_post(kPostSmaaWeights);
        rm_smaa_[2] = ldr_post(kPostSmaaBlend);
    }

    struct RmTargets {
        glm::ivec2 scene{0, 0}, output{0, 0};
        Image2D hdr, ambient, depth, ao, ldr, hist[2];
        Image2D motion;  // kMotionFormat: TAA motion (previous minus current uv) of opaque + depth-writing blended draws
        Image2D reference;  // kReferenceFormat: auto exposure reference (Original-look luma)
        Image2D lum, exposure[2];  // auto exposure: 64x32 measurement, 1x1 state (ping-pong by frame parity)
        VkImageView depth_sample = VK_NULL_HANDLE;  // depth aspect only
        // samples > 1: the scene passes render into these and resolve into hdr/ambient/motion/reference/depth (sample 0 for depth).
        int samples = 1;
        Image2D ms_hdr, ms_ambient, ms_motion, ms_reference, ms_depth;
        // Opaque-only HDR (copy of hdr after the opaque + AO passes, before the translucent pass): FSR 3's autogen reactive
        // mask input, water refraction colour.
        Image2D opaque;
        Image2D water_depth;  // depth_format_: copy of the opaque depth for the stylised water (`water=enhanced`)
        // FSR 3 (fsr): render-size R32F depth + RG16F full motion (post_fsr_inputs), R8 reactive mask, display-size output.
        bool fsr = false;
        Image2D fsr_depth, fsr_motion, fsr_reactive, fsr_output;
    };

    // Fresh images start in their sampled layout, so every descriptor is valid from the first frame.
    void ensure_rm_targets(glm::ivec2 scene, glm::ivec2 output, int samples, bool fsr) {
        if (rmt_.scene == scene && rmt_.output == output && rmt_.samples == samples && rmt_.fsr == fsr && rmt_.hdr.image) return;
        vk_.vkDeviceWaitIdle(device_);
        destroy_rm_targets();
        rmt_.samples = samples;
        if (samples > 1) {
            const VkExtent2D e{std::uint32_t(scene.x), std::uint32_t(scene.y)};
            const auto n = VkSampleCountFlagBits(samples);
            rmt_.ms_hdr = create_image(kHdrFormat, e, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                       0, n);
            rmt_.ms_ambient = create_image(kHdrFormat, e, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, n);
            rmt_.ms_motion = create_image(kMotionFormat, e, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, n);
            rmt_.ms_reference = create_image(kReferenceFormat, e, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 0, n);
            rmt_.ms_depth = create_image(depth_format_, e, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, n);
            if (rm_ms_samples_ != samples) {
                destroy_rm_ms();
                rm_ms_ = make_rm_scene(n);
                rm_ms_samples_ = samples;
            }
        }
        rmt_.scene = scene;
        rmt_.output = output;
        const VkExtent2D e{std::uint32_t(scene.x), std::uint32_t(scene.y)};
        const VkImageUsageFlags color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        rmt_.hdr = create_image(kHdrFormat, e, 1, color | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        rmt_.opaque = create_image(kHdrFormat, e, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        rmt_.fsr = fsr;
        if (fsr) {
            const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            rmt_.fsr_depth = create_image(VK_FORMAT_R32_SFLOAT, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
            rmt_.fsr_motion = create_image(kMotionFormat, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
            rmt_.fsr_reactive = create_image(VK_FORMAT_R8_UNORM, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
            rmt_.fsr_output = create_image(kLdrFormat, {std::uint32_t(output.x), std::uint32_t(output.y)}, 1, storage, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        rmt_.ambient = create_image(kHdrFormat, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        const auto ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        rmt_.depth = create_image(depth_format_, e, 1,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, ds);
        // (Depth-attachment usage only for the DEPTH_STENCIL_READ_ONLY layout it is sampled in, like rmt_.depth.)
        rmt_.water_depth = create_image(depth_format_, e, 1,
                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                        VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = rmt_.depth.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = depth_format_;
        vi.subresourceRange = range(VK_IMAGE_ASPECT_DEPTH_BIT);
        check(vk_.vkCreateImageView(device_, &vi, nullptr, &rmt_.depth_sample), "vkCreateImageView");
        rmt_.ao = create_image(VK_FORMAT_R8_UNORM, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        rmt_.ldr = create_image(kLdrFormat, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        for (Image2D& h : rmt_.hist) h = create_image(kLdrFormat, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        rmt_.motion = create_image(kMotionFormat, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        rmt_.reference = create_image(kReferenceFormat, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        rmt_.lum = create_image(kHdrFormat, {64, 32}, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        for (Image2D& x : rmt_.exposure) x = create_image(kExposureFormat, {1, 1}, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        immediate([&](VkCommandBuffer cmd) {
            for (Image2D* i : {&rmt_.hdr, &rmt_.ambient, &rmt_.ao, &rmt_.ldr, &rmt_.hist[0], &rmt_.hist[1], &rmt_.motion, &rmt_.reference, &rmt_.lum,
                               &rmt_.exposure[0], &rmt_.exposure[1], &rmt_.opaque, &rmt_.fsr_depth, &rmt_.fsr_motion, &rmt_.fsr_reactive,
                               &rmt_.fsr_output})
                if (i->image)
                    image_barrier(cmd, i->image, range(VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            for (Image2D* i : {&rmt_.depth, &rmt_.water_depth})
                image_barrier(cmd, i->image, range(ds), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        history_.valid = false;
    }

    void destroy_rm_targets() {
        if (rmt_.depth_sample) vk_.vkDestroyImageView(device_, rmt_.depth_sample, nullptr);
        for (Image2D* i : {&rmt_.hdr, &rmt_.ambient, &rmt_.depth, &rmt_.ao, &rmt_.ldr, &rmt_.hist[0], &rmt_.hist[1], &rmt_.motion, &rmt_.reference,
                           &rmt_.ms_hdr, &rmt_.ms_ambient, &rmt_.ms_motion, &rmt_.ms_reference, &rmt_.ms_depth, &rmt_.lum, &rmt_.exposure[0],
                           &rmt_.exposure[1], &rmt_.opaque, &rmt_.fsr_depth, &rmt_.fsr_motion, &rmt_.fsr_reactive, &rmt_.fsr_output,
                           &rmt_.water_depth})
            destroy(*i);
        rmt_ = {};
    }

    void destroy_rm_ms() {  // callers made the device idle
        for (VkPipeline p : {rm_ms_.opaque, rm_ms_.opaque_a2c, rm_ms_.volume, rm_ms_.fill, rm_ms_.blend, rm_ms_.blend_zwrite, rm_ms_.additive,
                             rm_ms_.ao_apply})
            if (p) vk_.vkDestroyPipeline(device_, p, nullptr);
        rm_ms_ = {};
        rm_ms_samples_ = 1;
    }

    // The shadow map always exists (1x1 when disabled) so binding 1 stays valid.
    void ensure_rm_shadow() {
        if (shadow_.image) return;
        shadow_size_ = std::max(gfx_.shadow_map_size, 1);
        const std::uint32_t n = std::uint32_t(shadow_size_);
        shadow_ = create_image(shadow_format_, {n, n}, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT);
        immediate([&](VkCommandBuffer cmd) {
            image_barrier(cmd, shadow_.image, range(VK_IMAGE_ASPECT_DEPTH_BIT), VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
    }

    void destroy_rm_shadow() { destroy(shadow_); }

    void destroy_remaster() {
        destroy_rm_targets();
        destroy_rm_shadow();
        for (VkPipeline p : {rm_opaque_, rm_volume_, rm_fill_, rm_blend_, rm_blend_zwrite_, rm_additive_, rm_shadow_, rm_ssao_, rm_ao_apply_, rm_composite_,
                             rm_exposure_[0], rm_exposure_[1], rm_taa_, rm_easu_, rm_final_, rm_fxaa_, rm_smaa_[0], rm_smaa_[1], rm_smaa_[2],
                             rm_fsr_inputs_})
            if (p) vk_.vkDestroyPipeline(device_, p, nullptr);
        destroy_ms_pipes();
        destroy_rm_ms();
        destroy_original_target();
        destroy_post_targets();
        for (Slot& s : slots_) {
            destroy(s.rm_ubo);
            destroy(s.rm_materials);
            destroy(s.rm_waves);
            destroy(s.waves);
        }
        if (rm_pipe_layout_) vk_.vkDestroyPipelineLayout(device_, rm_pipe_layout_, nullptr);
        if (rm_pool_) vk_.vkDestroyDescriptorPool(device_, rm_pool_, nullptr);
        if (rm_layout_) vk_.vkDestroyDescriptorSetLayout(device_, rm_layout_, nullptr);
        for (VkSampler s : {rm_nearest_, rm_shadow_sampler_})
            if (s) vk_.vkDestroySampler(device_, s, nullptr);
    }

    void ensure_env(Slot& s, const Lighting& l) {
        const std::uint64_t key = env_key(l);
        if (env_radiance_ && key == env_key_) return;
        const EnvMaps m = build_env_maps(l);
        env_levels_ = m.radiance_levels;
        env_sky_up_ = m.sky_up;
        env_key_ = key;
        if (!env_radiance_) {
            env_radiance_ = create_texture(m.radiance);
            env_irradiance_ = create_texture(m.irradiance);
            return;
        }
        // Later changes (the fog colour follows the view direction: nearly every frame while turning) rewrite both
        // images in place from this idle slot's staging, recorded before the scene passes: no new images and no
        // immediate() queue wait. The barrier orders the copy after every earlier submission's reads.
        std::vector<VkBufferImageCopy> copies[2];
        VkDeviceSize bytes = 0;
        const TextureData* data[2] = {&m.radiance, &m.irradiance};
        for (int t = 0; t < 2; ++t)
            for (std::uint32_t i = 0; i <= data[t]->mip_pixels.size(); ++i) {
                const std::uint32_t w = std::max(data[t]->width >> i, 1), h = std::max(data[t]->height >> i, 1);
                copies[t].push_back({bytes, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1}, {0, 0, 0}, {w, h, 1}});
                bytes += VkDeviceSize(w) * h * 4;
            }
        ensure(s, s.env_staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        for (int t = 0; t < 2; ++t)
            for (std::size_t i = 0; i < copies[t].size(); ++i) {
                const auto& px = i ? data[t]->mip_pixels[i - 1] : data[t]->pixels;
                std::memcpy(s.env_staging.memory.mapped + copies[t][i].bufferOffset, px.data(), px.size());
            }
        const auto color = VK_IMAGE_ASPECT_COLOR_BIT;
        const TextureHandle handles[2] = {env_radiance_, env_irradiance_};
        for (int t = 0; t < 2; ++t) {
            const VkImage image = textures_.at(handles[t].id).image;
            image_barrier(cmd_, image, range(color), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vk_.vkCmdCopyBufferToImage(cmd_, s.env_staging.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       std::uint32_t(copies[t].size()), copies[t].data());
            image_barrier(cmd_, image, range(color), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    }

    // Set-2 inputs chosen per frame; null = a valid dummy (the Original path has no remaster targets).
    struct RmViews {
        VkImageView ldr, history, src, final;
    };

    // All of set 2 for this slot, rewritten every frame (the slot is idle: begin_frame waited for it). Inputs that do not
    // exist on this path (the Original look has no remaster targets or environment) get a valid dummy: the white texture,
    // or the (1x1 when off) shadow map for the depth bindings. waves: Slot::waves when uploaded this frame.
    void write_rm_set(Slot& s, const RmViews& v, VkImageView waves = VK_NULL_HANDLE) {
        const VkSampler linear = samplers_[0][0];
        const auto ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dro = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        const VkImageView white = textures_.at(white_.id).view;
        const auto or_white = [&](VkImageView view) { return view ? view : white; };
        const auto tex = [&](TextureHandle h) { return h ? textures_.at(h.id).view : white; };
        const int ew = int(history_.frame & 1u);  // auto exposure state written this frame (as GL)
        const VkDescriptorImageInfo images[kRmBindings] = {
            {},
            {rm_shadow_sampler_, shadow_.view, dro},
            {linear, tex(env_radiance_), ro},
            {linear, tex(env_irradiance_), ro},
            {linear, or_white(rmt_.hdr.view), ro},
            {rm_nearest_, or_white(rmt_.ambient.view), ro},
            {rm_nearest_, rmt_.depth_sample ? rmt_.depth_sample : shadow_.view, dro},
            {linear, or_white(rmt_.ao.view), ro},
            {linear, or_white(v.ldr), ro},
            {linear, or_white(v.history), ro},
            {linear, or_white(v.src), ro},
            {},
            {linear, or_white(v.final), ro},
            {rm_nearest_, or_white(rmt_.motion.view), ro},
            {linear, or_white(pt_.edges.view), ro},
            {linear, or_white(pt_.weights.view), ro},
            {linear, tex(smaa_area_), ro},
            {linear, tex(smaa_search_), ro},
            {rm_nearest_, or_white(rmt_.exposure[1 - ew].view), ro},
            {rm_nearest_, or_white(rmt_.lum.view), ro},
            {rm_nearest_, or_white(rmt_.exposure[ew].view), ro},
            {rm_nearest_, or_white(rmt_.reference.view), ro},
            {linear, or_white(rmt_.opaque.view), ro},
            {rm_nearest_, rmt_.water_depth.view ? rmt_.water_depth.view : shadow_.view, dro},
            {linear, or_white(waves), ro}};
        const VkDescriptorBufferInfo ubo{s.rm_ubo.buffer, 0, VK_WHOLE_SIZE}, materials{s.rm_materials.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[kRmBindings]{};
        for (std::uint32_t i = 0; i < kRmBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s.rm_set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            if (i == 0) w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, w[i].pBufferInfo = &ubo;
            else if (i == 11) w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w[i].pBufferInfo = &materials;
            else w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[i].pImageInfo = &images[i];
        }
        vk_.vkUpdateDescriptorSets(device_, kRmBindings, w, 0, nullptr);
    }

    // Coarse but simple: every remaster layout change is a full pipeline dependency.
    void rm_barrier(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
        image_barrier(cmd_, image, range(aspect), from, to, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    static VkRenderingAttachmentInfo attachment(VkImageView view, VkImageLayout layout, VkAttachmentLoadOp load, VkAttachmentStoreOp store,
                                                VkClearValue clear = {}) {
        VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        a.imageView = view;
        a.imageLayout = layout;
        a.loadOp = load;
        a.storeOp = store;
        a.clearValue = clear;
        return a;
    }

    // `stencil`: the depth image's stencil is an attachment too, described by `stencil_info` (default: same as depth).
    void begin_rendering(glm::ivec2 size, std::span<const VkRenderingAttachmentInfo> colors, const VkRenderingAttachmentInfo* depth,
                         bool stencil, const VkRenderingAttachmentInfo* stencil_info = nullptr) {
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, {std::uint32_t(size.x), std::uint32_t(size.y)}};
        info.layerCount = 1;
        info.colorAttachmentCount = std::uint32_t(colors.size());
        info.pColorAttachments = colors.data();
        info.pDepthAttachment = depth;
        info.pStencilAttachment = stencil ? (stencil_info ? stencil_info : depth) : nullptr;
        vk_.vkCmdBeginRendering(cmd_, &info);
        const VkViewport viewport{0.0f, 0.0f, float(size.x), float(size.y), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {std::uint32_t(size.x), std::uint32_t(size.y)}};
        vk_.vkCmdSetViewport(cmd_, 0, 1, &viewport);
        vk_.vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vk_.vkCmdSetCullMode(cmd_, VK_CULL_MODE_NONE);
    }

    void post_pass(VkPipeline pipeline, const Image2D& target, glm::ivec2 size, bool clear = false) {
        rm_barrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        const VkRenderingAttachmentInfo c = attachment(target.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                       clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE);
        begin_rendering(size, {&c, 1}, nullptr, false);
        vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vk_.vkCmdDraw(cmd_, 3, 1, 0, 0);
        vk_.vkCmdEndRendering(cmd_);
        rm_barrier(target.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    static bool is_terrain(const Material& m) { return m.lightmap && m.vertex_diffuse; }
    static bool casts_sun_shadow(const Material& m) {  // units: their stencil volumes instead (Material::volume_caster); water: none
        return !m.unlit && !m.volume_caster && m.blend != BlendMode::ShadowVolume && m.blend != BlendMode::Additive && !is_terrain(m) &&
               m.water < 0.0f;
    }

    void render_remaster(Slot& s, const FrameData& f) {
        const glm::ivec4 vp = f.view.viewport.z > 0 ? f.view.viewport : glm::ivec4(0, 0, target_size_.x, target_size_.y);
        const glm::ivec2 output(vp.z, vp.w), scene = scene_size(gfx_, output);
        ensure_remaster();
        // MSAA (msaa > 1): every raster scene pass multisampled; the ray-traced path keeps 1 sample (its prepass,
        // G-buffer and denoisers are per pixel; Ultra uses TAA).
        const int samples = rt_active() ? 1 : gfx_.msaa;
        const bool ms = samples > 1;
        ensure_rm_targets(scene, output, samples, fsr3_);
        ensure_rm_shadow();
        ensure_env(s, f.lighting);
        const Lighting& l = f.lighting;
        const RemasterFrame rf = build_remaster_frame(gfx_, f, scene, output, env_levels_, history_);
        previous_models(f.draws, motion_, prev_models_);
        std::memcpy(s.rm_ubo.memory.mapped, &rf, sizeof rf);
        upload_frame_uniforms(s, f);  // the UI reads its framebuffer size from set 1
        const bool shadows = gfx_.shadow_map_size > 0;
        const bool rt = rt_active();

        // Draw table: opaque/alpha-tested, the units' stencil volumes (Material::volume_caster; dropped with RT shadows,
        // whose rays hit the units' TLAS instances instead), blended back to front with the stylised water first (it is
        // drawn opaque over a copy of the opaque scene, every other blend over it).
        order_.clear();
        for (const DrawItem& d : f.draws)
            if (d.material.blend == BlendMode::Opaque || d.material.blend == BlendMode::AlphaTest) order_.push_back(&d);
        const std::size_t first_volume = order_.size();
        if (l.shadow_color.a > 0.0f && !(rt && gfx_.rt.shadows))
            for (const DrawItem& d : f.draws)
                if (d.material.blend == BlendMode::ShadowVolume) order_.push_back(&d);
        const std::size_t first_blended = order_.size();
        for (const DrawItem& d : f.draws)
            if (d.material.blend == BlendMode::AlphaBlend || d.material.blend == BlendMode::Additive) order_.push_back(&d);
        const glm::vec3 cam = f.view.camera_position;
        std::sort(order_.begin() + long(first_blended), order_.end(), [&](const DrawItem* a, const DrawItem* b) {
            if (const bool wa = gfx_.water && a->material.water >= 0.0f; wa != (gfx_.water && b->material.water >= 0.0f)) return wa;
            const glm::vec3 pa(a->model[3]), pb(b->model[3]);
            return glm::dot(pa - cam, pa - cam) > glm::dot(pb - cam, pb - cam);
        });
        upload_draws(s, l, ms);
        ensure(s, s.rm_materials, std::max<std::size_t>(order_.size(), 1) * sizeof(MaterialExtraGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto* extra = reinterpret_cast<MaterialExtraGpu*>(s.rm_materials.memory.mapped);
        const auto slot_or_none = [&](TextureHandle h) { return h ? texture_slot(h) : kNoTexture; };
        for (std::size_t i = 0; i < order_.size(); ++i) {
            const Material& m = order_[i]->material;
            MaterialMaps maps;
            const bool water = m.water >= 0.0f && gfx_.water;
            if (const auto it = material_maps_.find(m.albedo.id); it != material_maps_.end() && (m.water < 0.0f || water)) maps = it->second;
            if (is_terrain(m)) maps.cls = MaterialClass::Terrain;
            extra[i] = {prev_models_[std::size_t(order_[i] - f.draws.data())], slot_or_none(maps.normal), slot_or_none(maps.roughness),
                        slot_or_none(maps.metal), slot_or_none(maps.ao), std::int32_t(maps.cls), std::int32_t(m.blend), water ? m.water : -1.0f, 0};
        }
        const int w = int(history_.frame & 1u), r = 1 - w;  // TAA history written / read this frame (as GL)
        const bool taa = gfx_.aa == AntiAliasing::Taa && !fsr3_;  // FSR 3 replaces TAA and post AA; the final pass reads its output
        ensure_post_targets(scene, output);
        // Wave field: half floats into this (idle) slot's staging buffer, copied before the scene passes.
        const bool waves = rf.waves.w > 0.0f;
        if (waves) {
            auto* half = reinterpret_cast<std::uint16_t*>(s.rm_waves.memory.mapped);
            for (std::size_t i = 0; i < f.waves.height.size(); ++i) half[i] = std::uint16_t(glm::packHalf1x16(f.waves.height[i]));
            const auto color = VK_IMAGE_ASPECT_COLOR_BIT;
            rm_barrier(s.waves.image, color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkBufferImageCopy copy{0, 0, 0, {color, 0, 0, 1}, {0, 0, 0}, {std::uint32_t(kWaterWaveGrid), std::uint32_t(kWaterWaveGrid), 1}};
            vk_.vkCmdCopyBufferToImage(cmd_, s.rm_waves.buffer, s.waves.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            rm_barrier(s.waves.image, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        write_rm_set(s,
                     fsr3_ ? RmViews{rmt_.ldr.view, rmt_.hist[r].view, rmt_.ldr.view, rmt_.fsr_output.view}
                           : post_views(rmt_.ldr.view, rmt_.hist[r].view, taa ? rmt_.hist[w].view : rmt_.ldr.view),
                     waves ? s.waves.view : VK_NULL_HANDLE);
        const VkDescriptorSet sets[3] = {textures_set_, s.set, s.rm_set};
        vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rm_pipe_layout_, 0, 3, sets, 0, nullptr);
        if (rt) rt_prepare(s, f, scene, first_volume, first_blended);  // instance table, RtFrame, set 3 (graphics + compute)

        const Mesh* bound_mesh = nullptr;
        int cull = -1;
        const auto draw = [&](std::size_t i, bool force_two_sided) {
            const DrawItem& d = *order_[i];
            const auto it = meshes_.find(d.mesh.id);
            if (it == meshes_.end()) return;
            const Mesh& m = it->second;
            const std::uint32_t first = std::min(d.first_index, m.index_count);
            const std::uint32_t count = d.index_count ? std::min(d.index_count, m.index_count - first) : m.index_count - first;
            if (!count) return;
            if (const int c = force_two_sided || d.material.two_sided; c != cull) {
                cull = c;
                vk_.vkCmdSetCullMode(cmd_, c ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            }
            if (&m != bound_mesh) {
                // Binding 1: the pose shown last frame if update_mesh replaced it for this one, else this frame's vertices.
                const bool moved = m.updated == frame_ && m.prev;
                const VkBuffer vertices[2] = {m.vertices.buffer, moved ? m.prev : m.vertices.buffer};
                const VkDeviceSize offsets[2] = {m.vertex_offset, moved ? m.prev_offset : m.vertex_offset};
                vk_.vkCmdBindVertexBuffers(cmd_, 0, 2, vertices, offsets);
                vk_.vkCmdBindIndexBuffer(cmd_, m.indices.buffer, m.index_offset, VK_INDEX_TYPE_UINT32);
                bound_mesh = &m;
            }
            vk_.vkCmdDrawIndexed(cmd_, count, 1, first, 0, std::uint32_t(i));
        };
        const auto push_pass = [&](std::uint32_t pass) {
            vk_.vkCmdPushConstants(cmd_, rm_pipe_layout_, kRmStages, 0, sizeof pass, &pass);
        };
        const auto ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

        // Sun shadow map (casters as GL: lit opaque/alpha-tested, not the terrain).
        if (shadows) {
            rm_barrier(shadow_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            VkClearValue clear{};
            clear.depthStencil = {1.0f, 0};
            const VkRenderingAttachmentInfo d = attachment(shadow_.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                           VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, clear);
            begin_rendering(glm::ivec2(shadow_size_), {}, &d, false);
            vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rm_shadow_);
            push_pass(1);
            for (std::size_t i = 0; i < order_.size(); ++i)
                if ((i < first_volume || i >= first_blended) && casts_sun_shadow(order_[i]->material)) draw(i, true);
            vk_.vkCmdEndRendering(cmd_);
            rm_barrier(shadow_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        }

        rm_barrier(rmt_.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        // Ray tracing (docs/RAYTRACING.md): acceleration structures, G-buffer prepass (depth + normal/roughness),
        // then the compute passes; the forward opaque pass below reloads that depth and reads their results.
        if (rt) {
            if (s.timed) timestamp(QRtBegin);
            rt_accel_.build(cmd_, slot_);
            rt_bind(s);
            if (s.timed) timestamp(QRtAccel);
            rm_barrier(rmt_.ambient.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            rm_barrier(rmt_.depth.image, ds, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            VkClearValue zero{}, depth_clear{};
            depth_clear.depthStencil = {1.0f, 0};
            const VkRenderingAttachmentInfo colors[3] = {
                attachment(rt_gbuf_[rt_frame_ & 1u].view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, zero),
                attachment(rmt_.ambient.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                           VK_ATTACHMENT_STORE_OP_DONT_CARE),
                attachment(rmt_.motion.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                           VK_ATTACHMENT_STORE_OP_DONT_CARE)};
            const VkRenderingAttachmentInfo d = attachment(rmt_.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                           VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depth_clear);
            begin_rendering(scene, colors, &d, false);
            cull = -1;
            vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rt_prepass_);
            push_pass(2);
            for (std::size_t i = 0; i < first_volume; ++i) draw(i, false);
            vk_.vkCmdEndRendering(cmd_);
            if (s.timed) timestamp(QRtPrepass);
            rm_barrier(rmt_.depth.image, ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            rt_compute(s, scene);
            rm_barrier(rmt_.depth.image, ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        } else {
            rm_barrier(rmt_.depth.image, ds, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }

        // Opaque into HDR colour + ambient (+ the units' stencil volumes and fill).
        rm_barrier(rmt_.hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rm_barrier(rmt_.ambient.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rm_barrier(rmt_.reference.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        const RmScenePipes P = ms ? rm_ms_ : RmScenePipes{rm_opaque_, rm_opaque_, rm_volume_, rm_fill_, rm_blend_, rm_blend_zwrite_, rm_additive_, rm_ao_apply_};
        // MSAA: the scene passes write the multisampled images; `resolved` makes an attachment resolve into its 1x image.
        const auto resolved = [&](VkRenderingAttachmentInfo a, const Image2D& to, VkResolveModeFlagBits mode = VK_RESOLVE_MODE_AVERAGE_BIT) {
            a.resolveMode = mode;
            a.resolveImageView = to.view;
            a.resolveImageLayout = a.imageLayout;
            return a;
        };
        if (ms)
            for (Image2D* i : {&rmt_.ms_hdr, &rmt_.ms_ambient, &rmt_.ms_motion, &rmt_.ms_reference})
                rm_barrier(i->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (ms) rm_barrier(rmt_.ms_depth.image, ds, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        {
            VkClearValue cc{}, zero{}, depth_clear{}, reference_clear{};
            cc.color = {{l.clear_color.r, l.clear_color.g, l.clear_color.b, l.clear_color.a}};
            // Auto exposure reference: the background is the clear colour in both looks.
            reference_clear.color = {{glm::dot(glm::vec3(l.clear_color), glm::vec3(0.299f, 0.587f, 0.114f)), 0.0f, 0.0f, 0.0f}};
            depth_clear.depthStencil = {1.0f, 0};
            VkRenderingAttachmentInfo colors[4] = {
                attachment(rmt_.hdr.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, cc),
                attachment(rmt_.ambient.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                           zero),
                attachment(rmt_.motion.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                           zero),
                attachment(rmt_.reference.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                           reference_clear)};
            // RT: depth comes from the prepass (same jittered matrices, invariant positions), stencil starts at 0.
            VkRenderingAttachmentInfo d = attachment(rmt_.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                     rt ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                                     depth_clear);
            VkRenderingAttachmentInfo stencil = attachment(rmt_.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                           VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, depth_clear);
            if (ms) {  // resolve depth (sample 0) + ambient for SSAO / AO apply; HDR, motion and reference stay multisampled for the translucent pass
                colors[0].imageView = rmt_.ms_hdr.view;
                colors[1] = resolved(attachment(rmt_.ms_ambient.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                VK_ATTACHMENT_STORE_OP_DONT_CARE, zero),
                                     rmt_.ambient);
                colors[2].imageView = rmt_.ms_motion.view;
                colors[3].imageView = rmt_.ms_reference.view;
                d = resolved(attachment(rmt_.ms_depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                        VK_ATTACHMENT_STORE_OP_STORE, depth_clear),
                             rmt_.depth, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
                stencil = d;
            }
            begin_rendering(scene, colors, &d, true, &stencil);
            cull = -1;
            VkPipeline bound = rt ? rt_opaque_ : P.opaque;
            vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound);
            push_pass(0);
            std::uint32_t ref = ~0u;
            for (std::size_t i = 0; i < first_volume; ++i) {
                if (const std::uint32_t want = order_[i]->material.receive_shadow ? 128 : 0; want != ref)
                    vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, ref = want);
                if (ms) {  // alpha-to-coverage for alpha-tested draws
                    const VkPipeline want = order_[i]->material.blend == BlendMode::AlphaTest ? P.opaque_a2c : P.opaque;
                    if (want != bound) vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = want);
                }
                draw(i, false);
            }
            if (first_blended > first_volume) {
                vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, P.volume);
                for (std::size_t i = first_volume; i < first_blended; ++i) draw(i, false);
                vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, P.fill);
                vk_.vkCmdSetCullMode(cmd_, VK_CULL_MODE_NONE);
                vk_.vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, 128);
                const glm::vec4 color = shadow_fill_color(l.shadow_color);
                vk_.vkCmdPushConstants(cmd_, fill_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof color, &color);
                vk_.vkCmdDraw(cmd_, 3, 1, 0, 0);
                vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rm_pipe_layout_, 0, 3, sets, 0, nullptr);
                if (rt) vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rt_pipe_layout_, 3, 1, &s.rt_set, 0, nullptr);
            }
            vk_.vkCmdEndRendering(cmd_);
        }
        rm_barrier(rmt_.depth.image, ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        rm_barrier(rmt_.ambient.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        const Image2D& scene_hdr = ms ? rmt_.ms_hdr : rmt_.hdr;  // the image the scene passes write
        const VkRenderingAttachmentInfo hdr_load =
            attachment(scene_hdr.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
        if (gfx_.ssao) {
            post_pass(rm_ssao_, rmt_.ao, scene);
            rm_barrier(scene_hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            begin_rendering(scene, {&hdr_load, 1}, nullptr, false);
            vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, P.ao_apply);
            vk_.vkCmdDraw(cmd_, 3, 1, 0, 0);
            vk_.vkCmdEndRendering(cmd_);
        }
        // Opaque-only HDR (+ depth) before the translucent pass: FSR 3's autogen reactive mask input; the stylised water (first
        // of the blended draws, order_ above) refracts it and reads the depth (pbr_shade.glsl shade_water).
        const bool water = gfx_.water && first_blended < order_.size() && order_[first_blended]->material.water >= 0.0f;
        const VkExtent3D extent{std::uint32_t(scene.x), std::uint32_t(scene.y), 1};
        if (fsr3_ || water) {
            const auto color = VK_IMAGE_ASPECT_COLOR_BIT;
            rm_barrier(scene_hdr.image, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            rm_barrier(rmt_.opaque.image, color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (ms) {
                const VkImageResolve resolve{{color, 0, 0, 1}, {}, {color, 0, 0, 1}, {}, extent};
                vk_.vkCmdResolveImage(cmd_, scene_hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rmt_.opaque.image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);
            } else {
                const VkImageCopy copy{{color, 0, 0, 1}, {}, {color, 0, 0, 1}, {}, extent};
                vk_.vkCmdCopyImage(cmd_, scene_hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rmt_.opaque.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }
            rm_barrier(rmt_.opaque.image, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            rm_barrier(scene_hdr.image, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        if (water) {  // MSAA: rmt_.depth holds the opaque pass's resolved (sample 0) depth
            const VkImageCopy copy{{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {}, extent};
            rm_barrier(rmt_.depth.image, ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            rm_barrier(rmt_.water_depth.image, ds, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vk_.vkCmdCopyImage(cmd_, rmt_.depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rmt_.water_depth.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            rm_barrier(rmt_.water_depth.image, ds, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            rm_barrier(rmt_.depth.image, ds, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        }

        // Translucent, back to front; depth writes only for Material::depth_write blends.
        rm_barrier(rmt_.hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rm_barrier(rmt_.reference.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rm_barrier(rmt_.depth.image, ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        rm_barrier(rmt_.ambient.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (ms) {
            for (Image2D* i : {&rmt_.ms_hdr, &rmt_.ms_ambient, &rmt_.ms_motion, &rmt_.ms_reference})
                rm_barrier(i->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            rm_barrier(rmt_.ms_depth.image, ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        }
        {
            VkRenderingAttachmentInfo d = attachment(rmt_.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                     VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
            // {HDR, ambient (scratch, no longer read), motion, reference}: o_motion (location 2) of depth-writing blends lands on
            // the motion target, every draw blends its auto exposure reference (location 3).
            VkRenderingAttachmentInfo colors[4] = {
                hdr_load,
                attachment(rmt_.ambient.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                           VK_ATTACHMENT_STORE_OP_DONT_CARE),
                attachment(rmt_.motion.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE),
                attachment(rmt_.reference.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)};
            if (ms) {  // resolve HDR, motion, reference and depth (sample 0) for the composite, TAA and auto exposure
                colors[0] = resolved(hdr_load, rmt_.hdr);
                colors[1].imageView = rmt_.ms_ambient.view;
                colors[2] = resolved(attachment(rmt_.ms_motion.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                                                VK_ATTACHMENT_STORE_OP_DONT_CARE),
                                     rmt_.motion);
                colors[3] = resolved(attachment(rmt_.ms_reference.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                                                VK_ATTACHMENT_STORE_OP_DONT_CARE),
                                     rmt_.reference);
                d = resolved(attachment(rmt_.ms_depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                                        VK_ATTACHMENT_STORE_OP_DONT_CARE),
                             rmt_.depth, VK_RESOLVE_MODE_SAMPLE_ZERO_BIT);
            }
            begin_rendering(scene, colors, &d, true);
            cull = -1;
            push_pass(0);
            VkPipeline bound = VK_NULL_HANDLE;
            for (std::size_t i = first_blended; i < order_.size(); ++i) {
                const Material& m = order_[i]->material;
                const bool zwrite = m.depth_write || (gfx_.water && m.water >= 0.0f);  // stylised water: depth + motion of its surface
                const VkPipeline p = m.blend == BlendMode::Additive ? (rt ? rt_additive_ : P.additive)
                                     : zwrite                       ? (rt ? rt_blend_zwrite_ : P.blend_zwrite)
                                                                    : (rt ? rt_blend_ : P.blend);
                if (p != bound) vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
                draw(i, false);
            }
            vk_.vkCmdEndRendering(cmd_);
        }
        rm_barrier(rmt_.depth.image, ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        rm_barrier(rmt_.hdr.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        rm_barrier(rmt_.motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        rm_barrier(rmt_.ambient.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        rm_barrier(rmt_.reference.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // Auto exposure: the scene with its volumetric fog measured against the Original-look reference, the 1x1 state adapted
        // on the GPU (post_exposure_lum/adapt.glsl); the composite reads it.
        post_pass(rm_exposure_[0], rmt_.lum, {64, 32});
        post_pass(rm_exposure_[1], rmt_.exposure[w], {1, 1});

        // Post chain, then the final pass + UI into the display target (UNORM view: encoded bytes).
        post_pass(rm_composite_, rmt_.ldr, scene);
        if (taa) post_pass(rm_taa_, rmt_.hist[w], scene);
        if (fsr3_) fsr_upscale(f, scene, output, rf);
        post_tail(s, f, scene, output, vp);
        if (rt) {
            rt_prev_camera_ = cam;
            rt_history_ = true;
            ++rt_frame_;
        }
    }

    // ---- image-quality options on any preset (msaa, aa, render_scale, sharpness): docs/GRAPHICS_ROADMAP.md ----------
    // The Original look renders unchanged into a scene-size sRGB target (multisampled + resolved when msaa > 1); both
    // paths then share the post tail on set 2: [FXAA | SMAA] -> [EASU] -> RCAS/copy/downsample -> UI.

    struct OriginalTarget {
        glm::ivec2 size{0, 0};
        int samples = 0;
        Image2D color, ms_color, depth;              // color: single-sample (resolved) sRGB scene
        VkImageView color_unorm = VK_NULL_HANDLE;    // encoded bytes for the post passes
    };
    struct PostTargets {
        glm::ivec2 scene{0, 0}, output{0, 0};
        Image2D edges, weights, out, easu;  // SMAA edges/weights, FXAA/SMAA output (scene size); EASU (output size)
    };

    void ensure_ms_pipes() {
        if (ms_samples_ == gfx_.msaa) return;
        destroy_ms_pipes();
        if (gfx_.msaa > 1) {
            const auto n = VkSampleCountFlagBits(gfx_.msaa);
            ms_pipes_ = {make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::Opaque, false, false, n),
                         make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::Opaque, false, false, n, true),
                         make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::AlphaBlend, false, false, n),
                         make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::AlphaBlend, false, true, n),
                         make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::Additive, false, false, n),
                         make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::ShadowVolume, false, false, n),
                         make_pipeline(kFillVert, kFillFrag, true, BlendMode::AlphaBlend, true, false, n)};
        }
        ms_samples_ = gfx_.msaa;
    }

    void destroy_ms_pipes() {
        if (ms_pipes_.opaque) vk_.vkDeviceWaitIdle(device_);
        for (VkPipeline p : {ms_pipes_.opaque, ms_pipes_.alpha_test, ms_pipes_.blend, ms_pipes_.blend_zwrite, ms_pipes_.additive, ms_pipes_.volume,
                             ms_pipes_.fill})
            if (p) vk_.vkDestroyPipeline(device_, p, nullptr);
        ms_pipes_ = {};
        ms_samples_ = 1;
    }

    void ensure_original_target(glm::ivec2 size) {
        if (ot_.size == size && ot_.samples == gfx_.msaa && ot_.color.image) return;
        vk_.vkDeviceWaitIdle(device_);
        destroy_original_target();
        ot_.size = size;
        ot_.samples = gfx_.msaa;
        const VkExtent2D e{std::uint32_t(size.x), std::uint32_t(size.y)};
        const auto n = VkSampleCountFlagBits(ot_.samples);
        ot_.color = create_image(kColorFormat, e, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                                 VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = ot_.color.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kUiColorFormat;
        vi.subresourceRange = range(VK_IMAGE_ASPECT_COLOR_BIT);
        check(vk_.vkCreateImageView(device_, &vi, nullptr, &ot_.color_unorm), "vkCreateImageView");
        if (ot_.samples > 1)
            ot_.ms_color = create_image(kColorFormat, e, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                        VK_IMAGE_ASPECT_COLOR_BIT, 0, n);
        ot_.depth = create_image(depth_format_, e, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, n);
        immediate([&](VkCommandBuffer cmd) {  // sampled layout from the start: set 2 is written before the scene pass
            image_barrier(cmd, ot_.color.image, range(VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
    }

    void destroy_original_target() {
        if (ot_.color_unorm) vk_.vkDestroyImageView(device_, ot_.color_unorm, nullptr);
        for (Image2D* i : {&ot_.color, &ot_.ms_color, &ot_.depth}) destroy(*i);
        ot_ = {};
    }

    void ensure_post_targets(glm::ivec2 scene, glm::ivec2 output) {
        if (pt_.scene == scene && pt_.output == output && pt_.out.image) return;
        vk_.vkDeviceWaitIdle(device_);
        destroy_post_targets();
        pt_.scene = scene;
        pt_.output = output;
        const VkExtent2D e{std::uint32_t(scene.x), std::uint32_t(scene.y)}, eo{std::uint32_t(output.x), std::uint32_t(output.y)};
        const VkImageUsageFlags color = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        for (Image2D* i : {&pt_.edges, &pt_.weights, &pt_.out}) *i = create_image(kLdrFormat, e, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        pt_.easu = create_image(kLdrFormat, eo, 1, color, VK_IMAGE_ASPECT_COLOR_BIT);
        immediate([&](VkCommandBuffer cmd) {
            for (Image2D* i : {&pt_.edges, &pt_.weights, &pt_.out, &pt_.easu})
                image_barrier(cmd, i->image, range(VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        if (!smaa_area_) {
            smaa_area_ = create_texture(smaa_area_texture());
            smaa_search_ = create_texture(smaa_search_texture());
        }
    }

    void destroy_post_targets() {
        for (Image2D* i : {&pt_.edges, &pt_.weights, &pt_.out, &pt_.easu}) destroy(*i);
        pt_ = {};
    }

    bool use_easu(glm::ivec2 scene, glm::ivec2 output) const { return gfx_.upscale == Upscaler::Fsr1 && scene.x < output.x; }

    // pre_aa: the image the AA pass reads (composite or TAA output); the final pass reads AA, then EASU output.
    RmViews post_views(VkImageView ldr, VkImageView history, VkImageView pre_aa) const {
        const bool aa = gfx_.aa == AntiAliasing::Fxaa || gfx_.aa == AntiAliasing::Smaa;
        const VkImageView src = aa ? pt_.out.view : pre_aa;
        return {aa ? pre_aa : ldr, history, src, use_easu(pt_.scene, pt_.output) ? pt_.easu.view : src};
    }

    void post_tail(Slot& s, const FrameData& f, glm::ivec2 scene, glm::ivec2 output, glm::ivec4 vp) {
        const AntiAliasing aa = fsr3_ ? AntiAliasing::None : gfx_.aa;  // FSR 3 is the anti-aliasing
        if (aa == AntiAliasing::Fxaa) {
            post_pass(rm_fxaa_, pt_.out, scene);
        } else if (aa == AntiAliasing::Smaa) {
            post_pass(rm_smaa_[0], pt_.edges, scene, true);  // edge-free pixels discard: cleared target
            post_pass(rm_smaa_[1], pt_.weights, scene);
            post_pass(rm_smaa_[2], pt_.out, scene);
        }
        if (use_easu(scene, output)) post_pass(rm_easu_, pt_.easu, output);
        VkClearValue black{};
        black.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkRenderingAttachmentInfo out =
            attachment(color_ui_view_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, black);
        begin_rendering(target_size_, {&out, 1}, nullptr, false);
        const VkViewport viewport{float(vp.x), float(vp.y), float(vp.z), float(vp.w), 0.0f, 1.0f};
        const VkRect2D rect{{vp.x, vp.y}, {std::uint32_t(vp.z), std::uint32_t(vp.w)}};
        vk_.vkCmdSetViewport(cmd_, 0, 1, &viewport);
        vk_.vkCmdSetScissor(cmd_, 0, 1, &rect);
        vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rm_final_);
        vk_.vkCmdDraw(cmd_, 3, 1, 0, 0);
        if (fg_pending_) {  // FSR 3 frame generation: interpolate the HUD-less image, then the UI onto both frames
            vk_.vkCmdEndRendering(cmd_);
            frame_generate(s, f, vp);
            const VkRenderingAttachmentInfo load =
                attachment(color_ui_view_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
            begin_rendering(target_size_, {&load, 1}, nullptr, false);
        }
        if (s.timed) timestamp(QUiBegin);
        draw_ui(s, f);
        vk_.vkCmdEndRendering(cmd_);
    }

    // ---- AMD FSR 3.1.4 upscaling + frame generation (fsr_vk.hpp, docs/GRAPHICS_ROADMAP.md "FSR 3") -----------------
    // Render-size composite (tone-mapped, jittered by FSR's Halton sequence), R32F depth, full unjittered motion and the
    // autogen reactive mask -> display-size rmt_.fsr_output, read by the final pass (no RCAS there: FSR's own takes the
    // sharpness). History resets on resize, settings change, camera cuts (RemasterFrame exposure.y snap) and after any
    // frame without this pass.
    void fsr_upscale(const FrameData& f, glm::ivec2 scene, glm::ivec2 output, const RemasterFrame& rf) {
        const VkExtent2D render{std::uint32_t(scene.x), std::uint32_t(scene.y)}, display{std::uint32_t(output.x), std::uint32_t(output.y)};
        if (!fsr_ || fsr_->render().width != render.width || fsr_->render().height != render.height || fsr_->display().width != display.width ||
            fsr_->display().height != display.height || fsr_->generates() != generating_) {
            vk_.vkDeviceWaitIdle(device_);
            fsr_.reset();
            fsr_ = std::make_unique<Fsr3>(physical_, device_, render, display, generating_ ? kFgFormat : VK_FORMAT_UNDEFINED);
        }
        const auto color = VK_IMAGE_ASPECT_COLOR_BIT;
        for (const Image2D* i : {&rmt_.fsr_depth, &rmt_.fsr_motion, &rmt_.fsr_reactive})
            rm_barrier(i->image, color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        const auto out = [&](const Image2D& i) {
            return attachment(i.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE);
        };
        const VkRenderingAttachmentInfo inputs[3] = {out(rmt_.fsr_depth), out(rmt_.fsr_motion), out(rmt_.fsr_reactive)};
        begin_rendering(scene, inputs, nullptr, false);
        vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rm_fsr_inputs_);
        vk_.vkCmdDraw(cmd_, 3, 1, 0, 0);
        vk_.vkCmdEndRendering(cmd_);
        for (const Image2D* i : {&rmt_.fsr_depth, &rmt_.fsr_motion, &rmt_.fsr_reactive})
            rm_barrier(i->image, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        const auto image = [](const Image2D& i, VkFormat format, glm::ivec2 size, VkImageUsageFlags usage) {
            return Fsr3::Image{i.image, format, {std::uint32_t(size.x), std::uint32_t(size.y)}, usage};
        };
        constexpr VkImageUsageFlags target = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        const Fsr3::Image reactive = image(rmt_.fsr_reactive, VK_FORMAT_R8_UNORM, scene, target);
        // GL-convention perspective: near = P[3][2] / (P[2][2] - 1), far = P[3][2] / (P[2][2] + 1); device depth is [0, 1].
        const glm::mat4& p = f.view.projection;
        const bool perspective = p[2][3] != 0.0f;
        fsr_near_ = perspective ? p[3][2] / (p[2][2] - 1.0f) : 1.0f;
        fsr_far_ = perspective ? p[3][2] / (p[2][2] + 1.0f) : 100000.0f;
        fsr_fov_ = 2.0f * std::atan(1.0f / p[1][1]);
        // The simulation step (deterministic captures), not the wall clock.
        fsr_ms_ = f.dt > 0.0f ? std::clamp(f.dt * 1000.0f, 1.0f, 100.0f) : 1000.0f / 30.0f;
        fsr_reset_ = rf.exposure.y != 0.0f || fsr_last_ + 1 != frame_;
        fsr_last_ = frame_;
        // RemasterFrame jitter is GL NDC (y up); FSR wants render pixels, y down.
        fsr_->dispatch({cmd_, image(rmt_.ldr, kLdrFormat, scene, target), image(rmt_.fsr_depth, VK_FORMAT_R32_SFLOAT, scene, target),
                        image(rmt_.fsr_motion, kMotionFormat, scene, target), reactive, image(rmt_.fsr_output, kLdrFormat, output, storage),
                        {rf.jitter.x * float(scene.x) * 0.5f, -rf.jitter.y * float(scene.y) * 0.5f}, fsr_ms_, gfx_.sharpness, fsr_near_, fsr_far_,
                        fsr_fov_, fsr_reset_});
        fg_pending_ = generating_;
    }

    // FSR 3 frame generation (ETI Yami / IKV present path): the HUD-less display image (vp region of color_) is the
    // interpolation source; FSR writes the frame halfway between the previous and this one, and this frame's UI is drawn
    // onto that generated frame too (the HUD is never interpolated). end_frame presents it before the real frame. Called
    // between the final pass and the UI with color_ in COLOR_ATTACHMENT_OPTIMAL.
    void frame_generate(Slot& s, const FrameData& f, glm::ivec4 vp) {
        fg_pending_ = false;
        ensure_fg_targets(glm::ivec2(vp.z, vp.w));
        const auto color = VK_IMAGE_ASPECT_COLOR_BIT;
        const VkExtent3D extent{std::uint32_t(vp.z), std::uint32_t(vp.w), 1};
        rm_barrier(color_.image, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        rm_barrier(fg_source_.image, color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageCopy source{{color, 0, 0, 1}, {vp.x, vp.y, 0}, {color, 0, 0, 1}, {0, 0, 0}, extent};
        vk_.vkCmdCopyImage(cmd_, color_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fg_source_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &source);
        rm_barrier(fg_source_.image, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        rm_barrier(color_.image, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        const bool reset = fsr_reset_ || fg_last_ + 1 != frame_;  // interpolate only between consecutive FSR frames
        fg_last_ = frame_;
        rm_barrier(fg_generated_.image, color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);  // last frame's copy -> FSR's writes
        const VkExtent2D e{extent.width, extent.height};
        constexpr VkImageUsageFlags source_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        fsr_->interpolate({cmd_, {fg_source_.image, kFgFormat, e, source_usage},
                           {fg_generated_.image, kFgFormat, e, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT},
                           fsr_ms_, fsr_near_, fsr_far_, fsr_fov_, reset, ++fg_frame_});
        fg_valid_ = !reset;
        if (!fg_valid_) return;  // history reset: nothing to show between the frames
        rm_barrier(fg_generated_.image, color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);  // FSR's writes -> the copy
        rm_barrier(fg_present_.image, color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange all = range(color);
        vk_.vkCmdClearColorImage(cmd_, fg_present_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        rm_barrier(fg_present_.image, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageCopy place{{color, 0, 0, 1}, {0, 0, 0}, {color, 0, 0, 1}, {vp.x, vp.y, 0}, extent};
        vk_.vkCmdCopyImage(cmd_, fg_generated_.image, VK_IMAGE_LAYOUT_GENERAL, fg_present_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &place);
        rm_barrier(fg_present_.image, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        const VkRenderingAttachmentInfo ui =
            attachment(fg_present_ui_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE);
        begin_rendering(target_size_, {&ui, 1}, nullptr, false);
        draw_ui(s, f);
        vk_.vkCmdEndRendering(cmd_);
        rm_barrier(fg_present_.image, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }

    // fg_source_/fg_generated_: viewport size, kFgFormat; fg_present_: the target-size presented generated frame (sRGB like
    // color_, with a UNORM view for the UI pass).
    void ensure_fg_targets(glm::ivec2 output) {
        if (fg_output_ == output && fg_target_ == target_size_ && fg_present_.image) return;
        vk_.vkDeviceWaitIdle(device_);
        destroy_fg_targets();
        fg_output_ = output;
        fg_target_ = target_size_;
        const VkExtent2D e{std::uint32_t(output.x), std::uint32_t(output.y)}, t{std::uint32_t(target_size_.x), std::uint32_t(target_size_.y)};
        // FSR copies the source for the next interpolation (TRANSFER_SRC).
        fg_source_ = create_image(kFgFormat, e, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        fg_generated_ = create_image(kFgFormat, e, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT);
        fg_present_ = create_image(kColorFormat, t, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = fg_present_.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kUiColorFormat;
        vi.subresourceRange = range(VK_IMAGE_ASPECT_COLOR_BIT);
        check(vk_.vkCreateImageView(device_, &vi, nullptr, &fg_present_ui_), "vkCreateImageView");
        immediate([&](VkCommandBuffer cmd) {  // FSR declares the interpolation output UNORDERED_ACCESS (GENERAL)
            image_barrier(cmd, fg_generated_.image, range(VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        });
        fg_last_ = ~0ull;
    }

    void destroy_fg_targets() {
        if (fg_present_ui_) vk_.vkDestroyImageView(device_, fg_present_ui_, nullptr);
        fg_present_ui_ = VK_NULL_HANDLE;
        for (Image2D* i : {&fg_source_, &fg_generated_, &fg_present_}) destroy(*i);
        fg_output_ = fg_target_ = {0, 0};
    }

    void render_original_post(Slot& s, const FrameData& f) {
        const glm::ivec4 vp = viewport(f);
        const glm::ivec2 output(vp.z, vp.w), scene = scene_size(gfx_, output);
        ensure_remaster();  // set 2 layout, frame block, post pipelines
        ensure_rm_shadow();
        ensure_ms_pipes();
        ensure_original_target(scene);
        ensure_post_targets(scene, output);
        ensure(s, s.rm_materials, sizeof(MaterialExtraGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        const RemasterFrame rf = build_remaster_frame(gfx_, f, scene, output, 1, history_);
        std::memcpy(s.rm_ubo.memory.mapped, &rf, sizeof rf);
        write_rm_set(s, post_views(ot_.color_unorm, VK_NULL_HANDLE, ot_.color_unorm));

        const bool ms = ot_.samples > 1;
        const auto ds = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        rm_barrier(ot_.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (ms) rm_barrier(ot_.ms_color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        rm_barrier(ot_.depth.image, ds, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkClearValue black{}, depth_clear{};
        black.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        depth_clear.depthStencil = {1.0f, 0};
        VkRenderingAttachmentInfo color = attachment(ms ? ot_.ms_color.view : ot_.color.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                     VK_ATTACHMENT_LOAD_OP_CLEAR, ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
                                                     black);
        if (ms) {  // sRGB resolve averages in linear space
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = ot_.color.view;
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        const VkRenderingAttachmentInfo depth = attachment(ot_.depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                           VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, depth_clear);
        begin_rendering(scene, {&color, 1}, &depth, true);
        draw_scene(s, f, glm::ivec4(0, 0, scene), ms ? ms_pipes_ : base_pipes(), ms);
        vk_.vkCmdEndRendering(cmd_);
        rm_barrier(ot_.color.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const VkDescriptorSet sets[3] = {textures_set_, s.set, s.rm_set};
        vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rm_pipe_layout_, 0, 3, sets, 0, nullptr);
        post_tail(s, f, scene, output, vp);
    }

    // ---- ray tracing (docs/RAYTRACING.md) --------------------------------------------------------------

    static constexpr std::uint32_t kRtImages = 10;  // u_img: shadow raw, accumulation x2, filter x2; reflection the same
    static constexpr std::uint32_t kRtBindings = 14;
    static constexpr VkDescriptorType kRtTypes[kRtBindings] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};

    VkPipeline make_compute(std::span<const std::uint32_t> code) {
        VkShaderModule module = VK_NULL_HANDLE;
        const VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, code.size_bytes(), code.data()};
        check(vk_.vkCreateShaderModule(device_, &mi, nullptr, &module), "vkCreateShaderModule (RT)");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        ci.layout = rt_pipe_layout_;
        VkPipeline p = VK_NULL_HANDLE;
        const VkResult r = vk_.vkCreateComputePipelines(device_, pipeline_cache_, 1, &ci, nullptr, &p);
        vk_.vkDestroyShaderModule(device_, module, nullptr);
        check(r, "vkCreateComputePipelines (RT)");
        return p;
    }

    // Set 3 layout, pipelines, DDGI volume and the acceleration-structure manager, on the first RT frame.
    void ensure_rt() {
        if (rt_ready_) return;
        VkDescriptorSetLayoutBinding b[kRtBindings];
        for (std::uint32_t i = 0; i < kRtBindings; ++i) b[i] = {i, kRtTypes[i], i == 9 ? kRtImages : 1, kRmStages, nullptr};
        const VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, kRtBindings, b};
        check(vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &rt_layout_), "vkCreateDescriptorSetLayout (RT)");
        const VkDescriptorPoolSize sizes[5] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                               {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kFramesInFlight},
                                               {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6 * kFramesInFlight},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (kRtImages + 2) * kFramesInFlight}};
        const VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, kFramesInFlight, 5, sizes};
        check(vk_.vkCreateDescriptorPool(device_, &pi, nullptr, &rt_pool_), "vkCreateDescriptorPool (RT)");
        for (Slot& s : slots_) {
            const VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, rt_pool_, 1, &rt_layout_};
            check(vk_.vkAllocateDescriptorSets(device_, &ai, &s.rt_set), "vkAllocateDescriptorSets (RT)");
            s.rt_ubo = create_buffer(sizeof(RtFrame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHost, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        }
        const VkDescriptorSetLayout sets[4] = {textures_layout_, frame_layout_, rm_layout_, rt_layout_};
        const VkPushConstantRange push{kRmStages, 0, 16};
        const VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 4, sets, 1, &push};
        check(vk_.vkCreatePipelineLayout(device_, &pl, nullptr, &rt_pipe_layout_), "vkCreatePipelineLayout (RT)");

        const VkPipelineLayout L = rt_pipe_layout_;
        const VkFormat D = depth_format_;
        rt_prepass_ = make_rm_pipeline({kMeshPbrVert, kRtMeshFrag, true, {kHdrFormat, kHdrFormat, kMotionFormat}, D, false, true, true,
                                        RmStencil::None, RmBlend::Off, 0xf, true, false, L});
        const std::vector<VkFormat> C{kHdrFormat, kHdrFormat, kMotionFormat, kReferenceFormat};  // as make_rm_scene
        rt_opaque_ = make_rm_pipeline({kMeshPbrVert, kRtMeshFrag, true, C, D, true, true, true, RmStencil::Write, RmBlend::Off, 0xf, false, false, L});
        rt_blend_ = make_rm_pipeline({kMeshPbrVert, kRtMeshFrag, true, C, D, true, true, false, RmStencil::None, RmBlend::Alpha, 0xf, true, false, L});
        rt_blend_zwrite_ = make_rm_pipeline({kMeshPbrVert, kRtMeshFrag, true, C, D, true, true, true, RmStencil::None, RmBlend::Alpha, 0xf, false,
                                             false, L});
        rt_additive_ = make_rm_pipeline({kMeshPbrVert, kRtMeshFrag, true, C, D, true, true, false, RmStencil::None, RmBlend::Add, 0xf, true, false, L});
        rt_shadow_pipe_ = make_compute(kRtShadow);
        rt_reflect_pipe_ = make_compute(kRtReflect);
        rt_denoise_ = make_compute(kRtDenoise);
        rt_probe_trace_ = make_compute(kRtProbeTrace);
        rt_probe_blend_ = make_compute(kRtProbeBlend);

        // DDGI volume: 8x8 texels per probe in two atlases, per-probe state, one ray buffer.
        const ProbeGrid g;
        const std::uint32_t probes = std::uint32_t(g.dims.x * g.dims.y * g.dims.z);
        const VkExtent2D atlas{std::uint32_t(g.dims.x * g.dims.y * 8), std::uint32_t(g.dims.z * 8)};
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        rt_irr_ = create_image(kHdrFormat, atlas, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT);
        rt_depth_atlas_ = create_image(kHdrFormat, atlas, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT);
        rt_probe_state_ = create_buffer(probes * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        rt_probe_rays_ = create_buffer(VkDeviceSize(probes) * kProbeRays * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        immediate([&](VkCommandBuffer cmd) {
            const VkClearColorValue zero{};
            const VkImageSubresourceRange r = range(VK_IMAGE_ASPECT_COLOR_BIT);
            for (const Image2D* i : {&rt_irr_, &rt_depth_atlas_}) {
                image_barrier(cmd, i->image, r, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                              VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                vk_.vkCmdClearColorImage(cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
            }
        });
        rt_probes_reset_ = true;
        rt_accel_.init(vk_, device_, physical_, allocator_, sizeof(Vertex), [this](std::function<void()> fn) {
            graveyard_.emplace_back(timeline_value_ + (recording_ ? 1 : 0), std::move(fn));  // the frame being recorded may use it
        });
        rt_ready_ = true;
    }

    // Scene-size targets, all kept in GENERAL: G-buffer x2 (prepass attachment, read by compute), u_img table.
    void ensure_rt_targets(glm::ivec2 scene) {
        if (rt_size_ == scene && rt_gbuf_[0].image) return;
        vk_.vkDeviceWaitIdle(device_);
        destroy_rt_targets();
        const VkExtent2D e{std::uint32_t(scene.x), std::uint32_t(scene.y)};
        for (Image2D& g : rt_gbuf_)
            g = create_image(kHdrFormat, e, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        for (Image2D& i : rt_img_) i = create_image(kHdrFormat, e, 1, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        immediate([&](VkCommandBuffer cmd) {
            for (Image2D* i : {&rt_gbuf_[0], &rt_gbuf_[1]}) rt_general(cmd, *i);
            for (Image2D& i : rt_img_) rt_general(cmd, i);
        });
        rt_size_ = scene;
        rt_history_ = false;
    }
    void rt_general(VkCommandBuffer cmd, const Image2D& i) {
        image_barrier(cmd, i.image, range(VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE,
                      0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    }

    void destroy_rt_targets() {
        for (Image2D& g : rt_gbuf_) destroy(g);
        for (Image2D& i : rt_img_) destroy(i);
        rt_size_ = {0, 0};
    }

    void destroy_rt() {
        if (!rt_ready_) return;
        const auto& st = rt_accel_.stats;
        std::fprintf(stderr, "joan-native: vulkan ray tracing: %u BLAS (%u static built, %u compacted %.1f -> %.1f MiB, %u skinned rebuilds, %u refits), "
                             "%u TLAS instances last frame\n", st.blas, st.built, st.compacted, double(st.compacted_from) / 1048576.0,
                     double(st.compacted_to) / 1048576.0, st.rebuilds, st.refits, st.instances);
        rt_accel_.destroy();
        destroy_rt_targets();
        destroy(rt_irr_);
        destroy(rt_depth_atlas_);
        destroy(rt_probe_state_);
        destroy(rt_probe_rays_);
        for (Slot& s : slots_) {
            destroy(s.rt_ubo);
            destroy(s.rt_instances);
        }
        for (VkPipeline p : {rt_prepass_, rt_opaque_, rt_blend_, rt_blend_zwrite_, rt_additive_, rt_shadow_pipe_, rt_reflect_pipe_, rt_denoise_, rt_probe_trace_,
                             rt_probe_blend_})
            vk_.vkDestroyPipeline(device_, p, nullptr);
        vk_.vkDestroyPipelineLayout(device_, rt_pipe_layout_, nullptr);
        vk_.vkDestroyDescriptorPool(device_, rt_pool_, nullptr);
        vk_.vkDestroyDescriptorSetLayout(device_, rt_layout_, nullptr);
        rt_ready_ = false;
    }

    // CPU side of an RT frame: TLAS instances (+ their RtInstance records) from the draw table, the RtFrame block.
    void rt_prepare(Slot& s, const FrameData& f, glm::ivec2 scene, std::size_t first_volume, std::size_t first_blended) {
        ensure_rt();
        ensure_rt_targets(scene);
        const glm::vec3 cam = f.view.camera_position;
        rt_table_.clear();
        rt_accel_.begin(completed_value_, timeline_value_ + 1);
        for (std::size_t i = 0; i < order_.size(); ++i) {
            if (i >= first_volume && i < first_blended) continue;  // stencil volumes
            const DrawItem& d = *order_[i];
            const Material& m = d.material;
            const auto maps = material_maps_.find(m.albedo.id);
            const RtInstanceClass ic = rt_instance_class(m, maps == material_maps_.end() ? MaterialClass::Generic : maps->second.cls, is_terrain(m));
            const auto it = meshes_.find(d.mesh.id);
            if (!ic.mask || it == meshes_.end()) continue;
            const Mesh& mesh = it->second;
            const float scale = std::max({glm::length(glm::vec3(d.model[0])), glm::length(glm::vec3(d.model[1])), glm::length(glm::vec3(d.model[2]))});
            if (!rt_keep_instance(glm::vec3(d.model * glm::vec4(mesh.centre, 1.0f)), mesh.radius * scale, cam)) continue;
            const std::uint32_t first = std::min(d.first_index, mesh.index_count);
            std::uint32_t count = d.index_count ? std::min(d.index_count, mesh.index_count - first) : mesh.index_count - first;
            if ((count -= count % 3) == 0) continue;
            rt_accel_.add({d.mesh.id, mesh.generation, mesh.dynamic, mesh.vertex_address, mesh.index_address, mesh.vertex_count, first, count},
                          d.model, std::uint32_t(rt_table_.size()), ic.mask, !ic.alpha_test);
            rt_table_.push_back({mesh.vertex_address, mesh.index_address, first, texture_slot(m.albedo),
                                 m.layer2 ? texture_slot(m.layer2) : kNoTexture, m.lightmap ? texture_slot(m.lightmap) : kNoTexture, m.tint,
                                 glm::vec4(m.diffuse, m.vertex_diffuse ? 1.0f : 0.0f), ic.cutoff,
                                 // thin foliage cards have no inside; two-sided solids (walls) keep their back faces
                                 (is_terrain(m) ? 1u : 0u) | (m.two_sided && ic.mask == kRtMaskFoliage ? 2u : 0u), 0, 0});
        }
        ensure(s, s.rt_instances, std::max<std::size_t>(rt_table_.size(), 1) * sizeof(RtInstanceGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!rt_table_.empty()) std::memcpy(s.rt_instances.memory.mapped, rt_table_.data(), rt_table_.size() * sizeof(RtInstanceGpu));
        const RtFrame rf = build_rt_frame(gfx_.rt, f.lighting, cam, rt_prev_camera_, rt_history_, rt_frame_, env_sky_up_);
        std::memcpy(s.rt_ubo.memory.mapped, &rf, sizeof rf);
    }

    // After the TLAS build: set 3 for this frame (ping-pong targets) bound for graphics and compute.
    void rt_bind(Slot& s) {
        const std::uint32_t cur = rt_frame_ & 1u, shadow = gfx_.rt.denoise ? 3 : 0, reflection = gfx_.rt.denoise ? 8 : 5;
        const auto G = VK_IMAGE_LAYOUT_GENERAL;
        const VkDescriptorImageInfo sampled[6] = {{rm_nearest_, rt_gbuf_[cur].view, G},   {rm_nearest_, rt_gbuf_[1 - cur].view, G},
                                                  {rm_nearest_, rt_img_[shadow].view, G},  {rm_nearest_, rt_img_[reflection].view, G},
                                                  {samplers_[0][0], rt_irr_.view, G},     {samplers_[0][0], rt_depth_atlas_.view, G}};
        VkDescriptorImageInfo storage[kRtImages + 2];
        for (std::uint32_t i = 0; i < kRtImages; ++i) storage[i] = {VK_NULL_HANDLE, rt_img_[i].view, G};
        storage[kRtImages] = {VK_NULL_HANDLE, rt_irr_.view, G};
        storage[kRtImages + 1] = {VK_NULL_HANDLE, rt_depth_atlas_.view, G};
        const VkDescriptorBufferInfo buffers[4] = {{s.rt_ubo.buffer, 0, VK_WHOLE_SIZE}, {s.rt_instances.buffer, 0, VK_WHOLE_SIZE},
                                                   {rt_probe_state_.buffer, 0, VK_WHOLE_SIZE}, {rt_probe_rays_.buffer, 0, VK_WHOLE_SIZE}};
        const VkAccelerationStructureKHR tlas = rt_accel_.tlas();
        const VkWriteDescriptorSetAccelerationStructureKHR as{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR, nullptr, 1, &tlas};
        VkWriteDescriptorSet w[kRtBindings]{};
        for (std::uint32_t i = 0; i < kRtBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s.rt_set;
            w[i].dstBinding = i;
            w[i].descriptorCount = i == 9 ? kRtImages : 1;
            w[i].descriptorType = kRtTypes[i];
            if (i == 0) w[i].pBufferInfo = &buffers[0];
            else if (i == 1) w[i].pNext = &as;
            else if (i == 2) w[i].pBufferInfo = &buffers[1];
            else if (i <= 8) w[i].pImageInfo = &sampled[i - 3];
            else if (i <= 11) w[i].pImageInfo = &storage[i == 9 ? 0 : kRtImages + (i - 10)];
            else w[i].pBufferInfo = &buffers[i - 10];
        }
        vk_.vkUpdateDescriptorSets(device_, kRtBindings, w, 0, nullptr);
        const VkDescriptorSet sets[4] = {textures_set_, s.set, s.rm_set, s.rt_set};
        vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rt_pipe_layout_, 3, 1, &s.rt_set, 0, nullptr);
        vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, rt_pipe_layout_, 0, 4, sets, 0, nullptr);
    }

    // GI probe update, then per signal: trace -> temporal -> 3 a-trous iterations (steps 1, 2, 4).
    void rt_compute(Slot& s, glm::ivec2 scene) {
        const auto sync = [&] {
            memory_barrier(cmd_, {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT});
        };
        const auto run = [&](VkPipeline p, std::uint32_t x, std::uint32_t y, const RtPush* push) {
            vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            if (push) vk_.vkCmdPushConstants(cmd_, rt_pipe_layout_, kRmStages, 0, sizeof *push, push);
            vk_.vkCmdDispatch(cmd_, x, y, 1);
            sync();
        };
        if (rt_probes_reset_) {  // every slot "holds" an impossible cell: probes restart from their first update
            sync();  // the G-buffer prepass just read the probe state (write-after-read)
            vk_.vkCmdFillBuffer(cmd_, rt_probe_state_.buffer, 0, VK_WHOLE_SIZE, 0x7fffffffu);
            sync();
            rt_probes_reset_ = false;
        }
        const ProbeGrid g;
        const std::uint32_t probes = std::uint32_t(g.dims.x * g.dims.y * g.dims.z);
        if (gfx_.rt.gi) {
            run(rt_probe_trace_, probes, 1, nullptr);
            run(rt_probe_blend_, probes, 1, nullptr);
        }
        if (s.timed) timestamp(QRtGi);
        const std::uint32_t gx = std::uint32_t(scene.x + 7) / 8, gy = std::uint32_t(scene.y + 7) / 8, cur = rt_frame_ & 1u;
        const auto signal = [&](std::uint32_t sig, VkPipeline trace) {
            const std::uint32_t b = sig * 5;
            run(trace, gx, gy, nullptr);
            if (!gfx_.rt.denoise) return;
            RtPush p{0, sig, 1, b | (b + 1 + cur) << 8 | (b + 2 - cur) << 16};
            run(rt_denoise_, gx, gy, &p);
            std::uint32_t src = b + 1 + cur, dst = b + 3;
            for (std::int32_t step : {1, 2, 4}) {
                p = {1, sig, step, src | dst << 8};
                run(rt_denoise_, gx, gy, &p);
                src = dst;
                dst = dst == b + 3 ? b + 4 : b + 3;
            }
        };
        if (gfx_.rt.shadows) signal(0, rt_shadow_pipe_);
        if (s.timed) timestamp(QRtShadow);
        if (gfx_.rt.reflections) signal(1, rt_reflect_pipe_);
        if (s.timed) timestamp(QRtReflect);
        s.rt_timed = s.timed;
    }

    // ---- sync / helpers ---------------------------------------------------------------------------

    void wait(std::uint64_t value) {
        if (!value) return;
        const VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &timeline_, &value};
        check(vk_.vkWaitSemaphores(device_, &info, UINT64_MAX), "vkWaitSemaphores");
        completed_value_ = std::max(completed_value_, value);
    }

    // Timeline value reached once submit number `frame` (< frame_) stopped reading meshes; older submits than the ring
    // holds are bounded by its oldest entry (values never decrease).
    std::uint64_t submit_value(std::uint64_t frame) const {
        return submit_values_[std::max(frame, frame_ > kSubmitValues ? frame_ - kSubmitValues : 0) % kSubmitValues];
    }

    // Defers destruction until every frame submitted so far has retired.
    void retire(std::function<void()> fn) { graveyard_.emplace_back(timeline_value_, std::move(fn)); }

    void collect_garbage() {
        std::erase_if(graveyard_, [&](auto& g) {
            if (g.first > completed_value_) return false;
            g.second();
            return true;
        });
    }

    // Mesh/texture uploads without a CPU wait (an immediate() per grass patch / runtime mesh stalled the frame for the
    // whole in-flight GPU work, 10-19 ms): submitted at once on the frame queue, so the barrier `record` ends with orders
    // the copy before every later submission's reads; `staging` and the command buffer retire once it ran.
    void submit_upload(Buffer staging, const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, async_pool_,
                                                VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        check(vk_.vkAllocateCommandBuffers(device_, &alloc, &cmd), "vkAllocateCommandBuffers");
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                             VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        check(vk_.vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
        record(cmd);
        check(vk_.vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        const VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, timeline_, ++timeline_value_,
                                           VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
        const VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, cmd, 0};
        const VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, 0, nullptr, 1, &cb, 1, &signal};
        check(vk_.vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
        retire([this, staging, cmd]() mutable {
            destroy(staging);
            vk_.vkFreeCommandBuffers(device_, async_pool_, 1, &cmd);
        });
    }

    // ponytail: one-shot readbacks / target layout setup wait for the whole queue (rare: resizes, screenshots).
    void immediate(const std::function<void(VkCommandBuffer)>& record) {
        check(vk_.vkResetCommandPool(device_, upload_pool_, 0), "vkResetCommandPool");
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                             VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        check(vk_.vkBeginCommandBuffer(upload_cmd_, &begin), "vkBeginCommandBuffer");
        record(upload_cmd_);
        check(vk_.vkEndCommandBuffer(upload_cmd_), "vkEndCommandBuffer");
        const VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, upload_cmd_, 0};
        const VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2, nullptr, 0, 0, nullptr, 1, &cb, 0, nullptr};
        check(vk_.vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2");
        check(vk_.vkQueueWaitIdle(queue_), "vkQueueWaitIdle");
        completed_value_ = timeline_value_;
    }

    void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageSubresourceRange r, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                       VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = src_stage;
        b.srcAccessMask = src_access;
        b.dstStageMask = dst_stage;
        b.dstAccessMask = dst_access;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = r;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vk_.vkCmdPipelineBarrier2(cmd, &dep);
    }

    void memory_barrier(VkCommandBuffer cmd, const VkMemoryBarrier2& b) {
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &b;
        vk_.vkCmdPipelineBarrier2(cmd, &dep);
    }

    void timestamp(Query q) { vk_.vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slots_[slot_].queries, q); }

    void collect_timings(Slot& s) {
        if (!s.timed) return;
        std::uint64_t t[QCount] = {};
        const std::uint32_t n = s.present_timed ? QRtBegin : QPresentBegin;
        if (vk_.vkGetQueryPoolResults(device_, s.queries, 0, n, sizeof t, t, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        const double ms = double(props_.limits.timestampPeriod) * 1e-6;
        timing_.scene += double(t[QUiBegin] - t[QSceneBegin]) * ms;
        timing_.ui += double(t[QSceneEnd] - t[QUiBegin]) * ms;
        timing_.frame += double(t[QSceneEnd] - t[QFrameBegin]) * ms;
        if (s.present_timed) {
            timing_.present += double(t[QPresentEnd] - t[QPresentBegin]) * ms;
            ++timing_.presents;
        }
        ++timing_.frames;
        std::uint64_t r[QCount - QRtBegin] = {};
        if (s.rt_timed && vk_.vkGetQueryPoolResults(device_, s.queries, QRtBegin, QCount - QRtBegin, sizeof r, r, sizeof(std::uint64_t),
                                                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            for (std::uint32_t i = 0; i + 1 < QCount - QRtBegin; ++i) rt_timing_.pass[i] += double(r[i + 1] - r[i]) * ms;
            ++rt_timing_.frames;
        }
        s.timed = s.rt_timed = false;
    }

    void report_timings() {
        for (Slot& s : slots_) collect_timings(s);
        if (generating_ || fg_presents_)
            std::fprintf(stderr, "joan-native: vulkan frame generation: %llu generated + %llu real frames presented\n",
                         static_cast<unsigned long long>(fg_presents_), static_cast<unsigned long long>(real_presents_));
        if (!timing_.frames) return;
        const double n = double(timing_.frames);
        std::fprintf(stderr, "joan-native: vulkan gpu timing (%llu frames, mean ms): scene %.3f, ui %.3f, frame %.3f, present-blit %.3f\n",
                     static_cast<unsigned long long>(timing_.frames), timing_.scene / n, timing_.ui / n, timing_.frame / n,
                     timing_.presents ? timing_.present / double(timing_.presents) : 0.0);
        if (rt_timing_.frames) {
            const double k = double(rt_timing_.frames);
            const double* p = rt_timing_.pass;
            std::fprintf(stderr, "joan-native: vulkan ray tracing gpu timing (%llu frames, mean ms): acceleration structures %.3f, "
                                 "G-buffer prepass %.3f, gi probes %.3f, shadows %.3f, reflections %.3f\n",
                         static_cast<unsigned long long>(rt_timing_.frames), p[0] / k, p[1] / k, p[2] / k, p[3] / k, p[4] / k);
        }
    }

    SDL_Window* window_;
    Options options_;
    bool library_loaded_ = false;
    Api vk_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memory_props_{};
    std::uint32_t queue_family_ = 0, timestamp_bits_ = 0;
    VkBool32 anisotropy_ = VK_FALSE, independent_blend_ = VK_FALSE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    GpuAllocator allocator_;

    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::uint64_t timeline_value_ = 0, completed_value_ = 0;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer upload_cmd_ = VK_NULL_HANDLE;
    VkCommandPool async_pool_ = VK_NULL_HANDLE;  // submit_upload command buffers (freed as they retire)

    VkSampler samplers_[2][2]{};  // [repeat][anisotropic]
    VkDescriptorSetLayout textures_layout_ = VK_NULL_HANDLE, frame_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet textures_set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE, fill_layout_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    bool cache_loaded_ = false;
    VkPipeline opaque_ = VK_NULL_HANDLE, blend_ = VK_NULL_HANDLE, blend_zwrite_ = VK_NULL_HANDLE, additive_ = VK_NULL_HANDLE,
               volume_ = VK_NULL_HANDLE, fill_ = VK_NULL_HANDLE, ui_ = VK_NULL_HANDLE;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;  // D32_SFLOAT_S8_UINT or D24_UNORM_S8_UINT

    Slot slots_[kFramesInFlight];
    std::uint32_t slot_ = kFramesInFlight - 1;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool recording_ = false, scene_rendered_ = false;

    Image2D color_, depth_;
    VkImageView color_ui_view_ = VK_NULL_HANDLE;  // kUiColorFormat view of color_ (MUTABLE_FORMAT image)
    glm::ivec2 target_size_{0, 0};

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D swapchain_extent_{0, 0};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkSemaphore> present_done_;
    bool swapchain_dirty_ = false;
    bool swapchain_srgb_ = true;  // false: UNORM swapchain, presented through present_bridge_
    Image2D present_bridge_;      // kUiColorFormat copy of the presented image (sRGB-encoded bytes), target size
    glm::ivec2 bridge_size_{0, 0};

    std::unordered_map<std::uint32_t, Mesh> meshes_;
    std::unordered_map<std::uint32_t, Texture> textures_;
    std::uint32_t next_id_ = 1, next_slot_ = 0;
    std::vector<std::uint32_t> free_slots_;
    TextureHandle white_;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> graveyard_;
    std::vector<const DrawItem*> order_;

    GraphicsSettings gfx_{};
    bool remaster_ = false;
    // AMD FSR 3.1.4 (fsr_vk.hpp): fsr3_ = upscale=fsr3 runs this frame (remaster path, supported device); generating_ =
    // frame generation on. fsr_ffx_ = the backend's Vulkan entry points resolved (once per instance).
    bool fsr3_ = false, generating_ = false, fsr_ffx_ = false;
    const char* fsr_missing_ = nullptr;         // device reason FSR 3 cannot run (null: supported)
    const char* generation_missing_ = nullptr;  // device reason frame generation cannot run
    std::unique_ptr<Fsr3> fsr_;
    std::uint64_t fsr_last_ = ~0ull;  // frame_ of the last FSR dispatch (history continuity)
    float fsr_near_ = 1.0f, fsr_far_ = 1.0f, fsr_fov_ = 1.0f, fsr_ms_ = 16.7f;
    bool fsr_reset_ = true;  // this frame's FSR dispatch reset its history
    VkPipeline rm_fsr_inputs_ = VK_NULL_HANDLE;
    // Frame generation: fg_pending_ = this frame upscaled and interpolates in post_tail; fg_valid_ = this frame's generated
    // image (fg_present_) is presented before the real one; fg_last_ = frame_ of the last interpolation.
    bool fg_pending_ = false, fg_valid_ = false;
    std::uint64_t fg_last_ = ~0ull, fg_frame_ = 0, fg_presents_ = 0, real_presents_ = 0;
    glm::ivec2 fg_output_{0, 0}, fg_target_{0, 0};
    Image2D fg_source_, fg_generated_, fg_present_;
    VkImageView fg_present_ui_ = VK_NULL_HANDLE;
    std::unordered_map<std::uint32_t, MaterialMaps> material_maps_;  // by albedo texture id
    VkDescriptorSetLayout rm_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool rm_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout rm_pipe_layout_ = VK_NULL_HANDLE;
    VkSampler rm_nearest_ = VK_NULL_HANDLE, rm_shadow_sampler_ = VK_NULL_HANDLE;
    VkPipeline rm_opaque_ = VK_NULL_HANDLE, rm_volume_ = VK_NULL_HANDLE, rm_fill_ = VK_NULL_HANDLE, rm_blend_ = VK_NULL_HANDLE, rm_blend_zwrite_ = VK_NULL_HANDLE,
               rm_additive_ = VK_NULL_HANDLE, rm_shadow_ = VK_NULL_HANDLE, rm_ssao_ = VK_NULL_HANDLE, rm_ao_apply_ = VK_NULL_HANDLE,
               rm_composite_ = VK_NULL_HANDLE, rm_taa_ = VK_NULL_HANDLE, rm_easu_ = VK_NULL_HANDLE, rm_final_ = VK_NULL_HANDLE;
    VkPipeline rm_fxaa_ = VK_NULL_HANDLE, rm_smaa_[3]{}, rm_exposure_[2]{};  // rm_exposure_: post_exposure_lum, post_exposure_adapt
    ScenePipes ms_pipes_{};  // Original look into the MSAA target (msaa > 1)
    RmScenePipes rm_ms_{};   // remaster scene passes into the MSAA targets
    int rm_ms_samples_ = 1;
    int ms_samples_ = 1;
    OriginalTarget ot_{};
    PostTargets pt_{};
    TextureHandle smaa_area_{}, smaa_search_{};
    VkFormat shadow_format_ = VK_FORMAT_D32_SFLOAT;
    bool shadow_linear_ = true;
    RmTargets rmt_{};
    Image2D shadow_{};
    int shadow_size_ = 1;
    TextureHandle env_radiance_{}, env_irradiance_{};
    std::uint64_t env_key_ = 0;
    int env_levels_ = 1;
    RemasterHistory history_{};
    MotionHistory motion_;
    std::vector<glm::mat4> prev_models_;  // per FrameData::draws index, of the frame being drawn
    std::uint64_t frame_ = 0;              // submits so far
    static constexpr std::uint64_t kSubmitValues = 16;
    std::array<std::uint64_t, kSubmitValues> submit_values_{};  // timeline value after each recent submit (Mesh::region_read)

    // Ray tracing (docs/RAYTRACING.md).
    RtCaps rt_caps_;
    bool rt_ready_ = false, rt_history_ = false, rt_probes_reset_ = true;
    RtAccel rt_accel_;
    VkDescriptorSetLayout rt_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool rt_pool_ = VK_NULL_HANDLE;
    VkPipelineLayout rt_pipe_layout_ = VK_NULL_HANDLE;
    VkPipeline rt_prepass_ = VK_NULL_HANDLE, rt_opaque_ = VK_NULL_HANDLE, rt_blend_ = VK_NULL_HANDLE, rt_blend_zwrite_ = VK_NULL_HANDLE, rt_additive_ = VK_NULL_HANDLE,
               rt_shadow_pipe_ = VK_NULL_HANDLE, rt_reflect_pipe_ = VK_NULL_HANDLE, rt_denoise_ = VK_NULL_HANDLE, rt_probe_trace_ = VK_NULL_HANDLE,
               rt_probe_blend_ = VK_NULL_HANDLE;
    Image2D rt_gbuf_[2]{}, rt_img_[kRtImages]{}, rt_irr_{}, rt_depth_atlas_{};
    glm::ivec2 rt_size_{0, 0};
    Buffer rt_probe_state_, rt_probe_rays_;
    std::vector<RtInstanceGpu> rt_table_;
    glm::vec3 rt_prev_camera_{0.0f}, env_sky_up_{1.0f};
    std::uint32_t rt_frame_ = 0;
    struct {
        double pass[QCount - QRtBegin - 1] = {};  // accel, prepass, gi, shadows, reflections
        std::uint64_t frames = 0;
    } rt_timing_;

    struct {
        double scene = 0, ui = 0, frame = 0, present = 0;
        std::uint64_t frames = 0, presents = 0;
    } timing_;
};

}  // namespace

std::unique_ptr<IRenderer> create_vk_renderer(SDL_Window* window, const Options& options) {
    return std::make_unique<VkRenderer>(window, options);
}
std::uint32_t vk_validation_errors() { return g_validation_errors; }
bool vk_validation_active() { return g_validation_active; }
bool vk_ray_tracing_available() { return g_rt_available; }

}  // namespace joa::engine
