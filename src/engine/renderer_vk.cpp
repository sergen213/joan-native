#include "engine/renderer_vk.hpp"

#include "engine/vk_api.hpp"
#include "engine/vk_memory.hpp"

#include <SDL3/SDL_video.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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

std::atomic<std::uint32_t> g_validation_errors{0};
std::atomic<bool> g_validation_active{false};

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
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkMemoryPropertyFlags kHost = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

// OpenGL clip space (y up, z in [-1,1]) -> Vulkan (y down, z in [0,1]); camera code stays GL-convention.
const glm::mat4 kClipCorrection(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0.5f, 1);

// Timestamp queries per frame slot.
enum Query : std::uint32_t { QFrameBegin, QSceneBegin, QUiBegin, QSceneEnd, QPresentBegin, QPresentEnd, QCount };

struct FrameUniforms {
    glm::mat4 view_proj;
    glm::vec4 sun_dir, sun_color, ambient, fog_color, fog_range, camera_pos, framebuffer;
};
struct DrawGpu {  // std430 mirror of `Draw` in shaders/vk/common.glsl
    glm::mat4 model, normal_matrix;
    glm::vec4 tint;
    float alpha_cutoff;
    std::uint32_t unlit, texture, layer2, lightmap;
    std::uint32_t pad[3];
};
static_assert(sizeof(DrawGpu) == 176);
constexpr std::uint32_t kNoTexture = 0xffffffffu;
struct UiVertex {
    glm::vec2 position, uv;
    glm::vec4 color;
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    VkDeviceSize size = 0;
};
struct Mesh {
    Buffer vertices, indices;
    std::uint32_t index_count = 0;
};
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Allocation memory;
    std::uint32_t slot = 0;
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
    std::uint64_t retire_value = 0;  // timeline value reached once the GPU finished this slot's last frame
    Buffer uniforms, draws, ui;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    bool timed = false, present_timed = false;
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
        allocator_.init(vk_, device_, memory_props_);
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
            destroy_target();
            destroy_swapchain(swapchain_);
            for (VkSemaphore s : present_done_) vk_.vkDestroySemaphore(device_, s, nullptr);
            for (Slot& s : slots_) {
                vk_.vkDestroyCommandPool(device_, s.pool, nullptr);
                vk_.vkDestroySemaphore(device_, s.acquired, nullptr);
                destroy(s.uniforms);
                destroy(s.draws);
                destroy(s.ui);
                if (s.queries) vk_.vkDestroyQueryPool(device_, s.queries, nullptr);
            }
            vk_.vkDestroySemaphore(device_, timeline_, nullptr);
            vk_.vkDestroyCommandPool(device_, upload_pool_, nullptr);
            for (VkPipeline p : {opaque_, blend_, additive_, ui_}) vk_.vkDestroyPipeline(device_, p, nullptr);
            vk_.vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
            vk_.vkDestroyPipelineLayout(device_, layout_, nullptr);
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

    // Same handle, new buffers; the old ones retire once in-flight frames finish.
    // ponytail: synchronous staging upload per call; switch to per-frame host-visible streaming if CPU skinning gets heavy.
    bool update_mesh(MeshHandle h, const MeshData& data) override {
        const auto it = meshes_.find(h.id);
        if (it == meshes_.end()) return false;
        retire([this, m = it->second]() mutable { destroy(m); });
        it->second = upload(data);
        return true;
    }

    void destroy_mesh(MeshHandle h) override {
        if (auto it = meshes_.find(h.id); it != meshes_.end()) {
            retire([this, m = it->second]() mutable { destroy(m); });
            meshes_.erase(it);
        }
    }

    Mesh upload(const MeshData& data) {
        Mesh m;
        const VkDeviceSize vbytes = data.vertices.size() * sizeof(Vertex), ibytes = data.indices.size() * sizeof(std::uint32_t);
        m.vertices = create_buffer(std::max<VkDeviceSize>(vbytes, 4), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        m.indices = create_buffer(std::max<VkDeviceSize>(ibytes, 4), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        Buffer staging = create_buffer(vbytes + ibytes + 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHost, 0);
        if (vbytes) std::memcpy(staging.memory.mapped, data.vertices.data(), vbytes);
        if (ibytes) std::memcpy(staging.memory.mapped + vbytes, data.indices.data(), ibytes);
        immediate([&](VkCommandBuffer cmd) {
            if (vbytes) {
                const VkBufferCopy c{0, 0, vbytes};
                vk_.vkCmdCopyBuffer(cmd, staging.buffer, m.vertices.buffer, 1, &c);
            }
            if (ibytes) {
                const VkBufferCopy c{vbytes, 0, ibytes};
                vk_.vkCmdCopyBuffer(cmd, staging.buffer, m.indices.buffer, 1, &c);
            }
            const VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, VK_PIPELINE_STAGE_2_COPY_BIT,
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                                     VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT};
            memory_barrier(cmd, b);
        });
        destroy(staging);
        m.index_count = std::uint32_t(data.indices.size());
        return m;
    }

    TextureHandle create_texture(const TextureData& t) override {
        if (t.width <= 0 || t.height <= 0 || t.pixels.size() < std::size_t(t.width) * t.height * 4) return {};
        if (free_slots_.empty() && next_slot_ >= kMaxTextures) throw std::runtime_error("Vulkan: bindless texture table full");
        const std::uint32_t mips = t.mipmaps ? std::uint32_t(std::floor(std::log2(std::max(t.width, t.height)))) + 1 : 1;
        const VkFormat format = t.format == PixelFormat::Rgba8Srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        Texture tex;
        Image2D img = create_image(format, {std::uint32_t(t.width), std::uint32_t(t.height)}, mips,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT);
        tex.image = img.image;
        tex.view = img.view;
        tex.memory = img.memory;
        const VkDeviceSize bytes = VkDeviceSize(t.width) * t.height * 4;
        Buffer staging = create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHost, 0);
        std::memcpy(staging.memory.mapped, t.pixels.data(), bytes);
        immediate([&](VkCommandBuffer cmd) {
            const auto color = VK_IMAGE_ASPECT_COLOR_BIT;
            image_barrier(cmd, tex.image, range(color), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            // Rows are uploaded top-first, so UV (0,0) addresses the top-left texel (Direct3D convention).
            const VkBufferImageCopy copy{0, 0, 0, {color, 0, 0, 1}, {0, 0, 0}, {std::uint32_t(t.width), std::uint32_t(t.height), 1}};
            vk_.vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
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
                              VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                w = nw;
                h = nh;
            }
            image_barrier(cmd, tex.image, range(color, mips - 1, 1), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        });
        destroy(staging);

        if (!free_slots_.empty()) {
            tex.slot = free_slots_.back();
            free_slots_.pop_back();
        } else {
            tex.slot = next_slot_++;
        }
        const VkDescriptorImageInfo info{samplers_[t.repeat][t.mipmaps], tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = textures_set_;
        w.dstArrayElement = tex.slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vk_.vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
        const std::uint32_t id = next_id_++;
        textures_[id] = tex;
        return {id};
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
        image_barrier(cmd_, depth_.image, range(VK_IMAGE_ASPECT_DEPTH_BIT), VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, tests,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        recording_ = true;
    }

    void submit(const FrameData& f) override {
        if (!recording_) return;
        render_pass(&f);
        finish_scene();
    }

    void end_frame() override {
        if (recording_) {
            render_pass(nullptr);  // begin_frame without submit: black frame, like GL
            finish_scene();
        }
        if (!swapchain_) return;
        Slot& s = slots_[slot_];
        std::uint32_t index = 0;
        const VkResult acquired = vk_.vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, s.acquired, VK_NULL_HANDLE, &index);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            swapchain_dirty_ = true;
            return;
        }
        if (acquired == VK_SUBOPTIMAL_KHR) swapchain_dirty_ = true;
        else check(acquired, "vkAcquireNextImageKHR");

        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                             VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        check(vk_.vkBeginCommandBuffer(s.present, &begin), "vkBeginCommandBuffer");
        cmd_ = s.present;
        if (s.timed) {
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
        // Blit (not copy): converts RGBA -> the swapchain's BGRA order; sRGB on both ends keeps bytes intact.
        const VkImageBlit blit{{color, 0, 0, 1}, {{0, 0, 0}, {w, h, 1}}, {color, 0, 0, 1}, {{0, 0, 0}, {w, h, 1}}};
        vk_.vkCmdBlitImage(cmd_, color_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, VK_FILTER_NEAREST);
        image_barrier(cmd_, image, range(color), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        if (s.present_timed) timestamp(QPresentEnd);
        check(vk_.vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");

        const VkSemaphoreSubmitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, s.acquired, 0,
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
        const char* swapchain_ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13};
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = surface_ ? 1 : 0;
        info.ppEnabledExtensionNames = &swapchain_ext;
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
    }

    void create_descriptors() {
        // [repeat][anisotropic]: GL sets 8x anisotropy on mipmapped textures, mirrored here.
        for (int repeat = 0; repeat < 2; ++repeat)
            for (int aniso = 0; aniso < 2; ++aniso) {
                VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                s.magFilter = s.minFilter = VK_FILTER_LINEAR;
                s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                s.maxLod = VK_LOD_CLAMP_NONE;
                s.addressModeU = s.addressModeV = s.addressModeW =
                    repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                s.anisotropyEnable = aniso && anisotropy_;
                s.maxAnisotropy = s.anisotropyEnable ? std::min(8.0f, props_.limits.maxSamplerAnisotropy) : 1.0f;
                check(vk_.vkCreateSampler(device_, &s, nullptr, &samplers_[repeat][aniso]), "vkCreateSampler");
            }

        // Set 0: one bindless table of every texture, updated in place (slots are only reused after the GPU retires them).
        const VkDescriptorBindingFlags bindless = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                                  VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        const VkDescriptorSetLayoutBindingFlagsCreateInfo flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, nullptr,
                                                                1, &bindless};
        const VkDescriptorSetLayoutBinding tex{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures, VK_SHADER_STAGE_FRAGMENT_BIT,
                                               nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &flags,
                                           VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 1, &tex};
        check(vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &textures_layout_), "vkCreateDescriptorSetLayout");
        // Set 1: per frame slot, frame uniforms + draw table.
        const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        const VkDescriptorSetLayoutBinding frame[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
                                                       {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr}};
        li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0, 2, frame};
        check(vk_.vkCreateDescriptorSetLayout(device_, &li, nullptr, &frame_layout_), "vkCreateDescriptorSetLayout");

        const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures},
                                               {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight}};
        const VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr,
                                            VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 1 + kFramesInFlight, 3, sizes};
        check(vk_.vkCreateDescriptorPool(device_, &pi, nullptr, &descriptor_pool_), "vkCreateDescriptorPool");
        const VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptor_pool_, 1, &textures_layout_};
        check(vk_.vkAllocateDescriptorSets(device_, &ai, &textures_set_), "vkAllocateDescriptorSets");

        const VkDescriptorSetLayout sets[2] = {textures_layout_, frame_layout_};
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(std::uint32_t)};
        const VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 2, sets, 1, &push};
        check(vk_.vkCreatePipelineLayout(device_, &pl, nullptr, &layout_), "vkCreatePipelineLayout");
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

    VkPipeline make_pipeline(std::span<const std::uint32_t> vs, std::span<const std::uint32_t> fs, bool mesh, BlendMode blend) {
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
        const VkVertexInputAttributeDescription ui_attrs[3] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, position)},
                                                               {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, uv)},
                                                               {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UiVertex, color)}};
        const VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 1,
                                                          mesh ? &mesh_binding : &ui_binding, mesh ? 5u : 3u, mesh ? mesh_attrs : ui_attrs};
        const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                                                              VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
        const VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1, nullptr};
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;  // dynamic
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;  // GL CCW winding survives the clip-space y flip
        raster.lineWidth = 1.0f;
        const VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, nullptr, 0,
                                                      VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable = mesh;
        depth.depthWriteEnable = mesh && (blend == BlendMode::Opaque || blend == BlendMode::AlphaTest);
        depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (blend == BlendMode::AlphaBlend || blend == BlendMode::Additive) {  // glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA | ONE)
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            att.dstColorBlendFactor = att.dstAlphaBlendFactor =
                blend == BlendMode::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        const VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, nullptr, 0, VK_FALSE,
                                                     VK_LOGIC_OP_COPY, 1, &att};
        const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE};
        const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 3, dyn};
        const VkFormat color_format = kColorFormat;
        const VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, nullptr, 0, 1, &color_format,
                                                      kDepthFormat, VK_FORMAT_UNDEFINED};
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
        info.layout = layout_;
        VkPipeline p = VK_NULL_HANDLE;
        const VkResult r = vk_.vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &info, nullptr, &p);
        for (VkShaderModule m : modules) vk_.vkDestroyShaderModule(device_, m, nullptr);
        check(r, "vkCreateGraphicsPipelines");
        return p;
    }

    void create_pipelines() {
        load_pipeline_cache();
        opaque_ = make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::Opaque);  // alpha test = discard in the same pipeline
        blend_ = make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::AlphaBlend);
        additive_ = make_pipeline(kMeshVert, kMeshFrag, true, BlendMode::Additive);
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
            VkCommandBuffer cmds[2];
            const VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, s.pool,
                                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
            check(vk_.vkAllocateCommandBuffers(device_, &alloc, cmds), "vkAllocateCommandBuffers");
            s.scene = cmds[0];
            s.present = cmds[1];
            const VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            check(vk_.vkCreateSemaphore(device_, &sem, nullptr, &s.acquired), "vkCreateSemaphore");
            s.uniforms = create_buffer(sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, kHost, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            const VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptor_pool_, 1, &frame_layout_};
            check(vk_.vkAllocateDescriptorSets(device_, &ai, &s.set), "vkAllocateDescriptorSets");
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

    Image2D create_image(VkFormat format, VkExtent2D extent, std::uint32_t mips, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
        Image2D img;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {extent.width, extent.height, 1};
        info.mipLevels = mips;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
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
    }

    // Grows a per-slot host buffer (the slot is idle: begin_frame waited for it).
    void ensure(Slot& s, Buffer& b, VkDeviceSize bytes, VkBufferUsageFlags usage) {
        if (b.buffer && b.size >= bytes) return;
        destroy(b);
        b = create_buffer(std::max<VkDeviceSize>(bytes, 4096) * 3 / 2, usage, kHost, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (usage != VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) return;
        const VkDescriptorBufferInfo infos[2] = {{s.uniforms.buffer, 0, VK_WHOLE_SIZE}, {s.draws.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[2]{};
        for (std::uint32_t i = 0; i < 2; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = s.set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[i].pBufferInfo = &infos[i];
        }
        vk_.vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    }

    void create_target(glm::ivec2 size) {
        vk_.vkDeviceWaitIdle(device_);
        destroy_target();
        const VkExtent2D e{std::uint32_t(size.x), std::uint32_t(size.y)};
        color_ = create_image(kColorFormat, e, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
        depth_ = create_image(kDepthFormat, e, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        target_size_ = size;
        scene_rendered_ = false;
    }

    void destroy_target() {
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
        VkSurfaceFormatKHR format = formats.at(0);
        for (const auto& f : formats)
            if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = f;
                break;
            }
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
        vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
        swapchain_images_.resize(n);
        vk_.vkGetSwapchainImagesKHR(device_, swapchain_, &n, swapchain_images_.data());
        // One "render done" semaphore per image: present may still hold the previous one for an image.
        for (VkSemaphore s : present_done_) vk_.vkDestroySemaphore(device_, s, nullptr);
        present_done_.assign(n, VK_NULL_HANDLE);
        const VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (VkSemaphore& s : present_done_) check(vk_.vkCreateSemaphore(device_, &sem, nullptr, &s), "vkCreateSemaphore");
        static const char* const names[] = {"immediate", "mailbox", "fifo", "fifo-relaxed"};
        std::fprintf(stderr, "joan-native: vulkan swapchain %ux%u, %u images, %s\n", extent.width, extent.height, n,
                     mode <= VK_PRESENT_MODE_FIFO_RELAXED_KHR ? names[mode] : "?");
    }

    // ---- frame ------------------------------------------------------------------------------------

    void render_pass(const FrameData* f) {
        Slot& s = slots_[slot_];
        if (s.timed) timestamp(QSceneBegin);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = color_.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // outside the viewport is always black
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = depth_.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, {std::uint32_t(target_size_.x), std::uint32_t(target_size_.y)}};
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;
        vk_.vkCmdBeginRendering(cmd_, &info);
        if (f) {
            draw_scene(s, *f);
            if (s.timed) timestamp(QUiBegin);
            draw_ui(s, f->ui);
        } else if (s.timed) {
            timestamp(QUiBegin);
        }
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

    void draw_scene(Slot& s, const FrameData& f) {
        const glm::ivec4 vp = f.view.viewport.z > 0 ? f.view.viewport : glm::ivec4(0, 0, target_size_.x, target_size_.y);
        const glm::vec4& cc = f.lighting.clear_color;
        const VkClearAttachment clear{VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{cc.r, cc.g, cc.b, cc.a}}}};
        const VkClearRect rect{{{vp.x, vp.y}, {std::uint32_t(vp.z), std::uint32_t(vp.w)}}, 0, 1};
        vk_.vkCmdClearAttachments(cmd_, 1, &clear, 1, &rect);

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

        // Opaque + alpha-tested first, then blended back to front without depth writes (same order as GL).
        order_.clear();
        for (const DrawItem& d : f.draws)
            if (d.material.blend != BlendMode::AlphaBlend && d.material.blend != BlendMode::Additive) order_.push_back(&d);
        const std::size_t first_blended = order_.size();
        for (const DrawItem& d : f.draws)
            if (d.material.blend == BlendMode::AlphaBlend || d.material.blend == BlendMode::Additive) order_.push_back(&d);
        const glm::vec3 cam = f.view.camera_position;
        std::sort(order_.begin() + long(first_blended), order_.end(), [&](const DrawItem* a, const DrawItem* b) {
            const glm::vec3 pa(a->model[3]), pb(b->model[3]);
            return glm::dot(pa - cam, pa - cam) > glm::dot(pb - cam, pb - cam);
        });
        ensure(s, s.draws, order_.size() * sizeof(DrawGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto* gpu = reinterpret_cast<DrawGpu*>(s.draws.memory.mapped);
        for (std::size_t i = 0; i < order_.size(); ++i) {
            const DrawItem& d = *order_[i];
            const Material& m = d.material;
            const std::uint32_t layer2 = m.layer2 ? texture_slot(m.layer2) : kNoTexture;
            const std::uint32_t lightmap = m.lightmap ? texture_slot(m.lightmap) : kNoTexture;
            gpu[i] = {d.model, glm::mat4(glm::transpose(glm::inverse(glm::mat3(d.model)))), m.tint,
                      m.blend == BlendMode::AlphaTest ? m.alpha_cutoff : -1.0f, m.unlit ? 1u : 0u, texture_slot(m.albedo), layer2,
                      lightmap, {}};
        }

        const VkDescriptorSet sets[2] = {textures_set_, s.set};
        vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
        const VkViewport viewport{float(vp.x), float(vp.y), float(vp.z), float(vp.w), 0.0f, 1.0f};
        vk_.vkCmdSetViewport(cmd_, 0, 1, &viewport);
        vk_.vkCmdSetScissor(cmd_, 0, 1, &rect.rect);
        VkPipeline bound = VK_NULL_HANDLE;
        const Mesh* bound_mesh = nullptr;
        int cull = -1;
        for (std::size_t i = 0; i < order_.size(); ++i) {
            const DrawItem& d = *order_[i];
            const auto it = meshes_.find(d.mesh.id);
            if (it == meshes_.end()) continue;
            const Mesh& m = it->second;
            const std::uint32_t first = std::min(d.first_index, m.index_count);
            const std::uint32_t count = d.index_count ? std::min(d.index_count, m.index_count - first) : m.index_count - first;
            if (!count) continue;
            const VkPipeline p = d.material.blend == BlendMode::AlphaBlend ? blend_ : d.material.blend == BlendMode::Additive ? additive_ : opaque_;
            if (p != bound) vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound = p);
            if (int(d.material.two_sided) != cull) {
                cull = d.material.two_sided;
                vk_.vkCmdSetCullMode(cmd_, d.material.two_sided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            }
            if (&m != bound_mesh) {
                const VkDeviceSize zero = 0;
                vk_.vkCmdBindVertexBuffers(cmd_, 0, 1, &m.vertices.buffer, &zero);
                vk_.vkCmdBindIndexBuffer(cmd_, m.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                bound_mesh = &m;
            }
            vk_.vkCmdDrawIndexed(cmd_, count, 1, first, 0, std::uint32_t(i));  // firstInstance = draw table index
        }
    }

    void draw_ui(Slot& s, std::span<const UiQuad> quads) {
        if (quads.empty()) return;
        ensure(s, s.ui, quads.size() * 6 * sizeof(UiVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        auto* v = reinterpret_cast<UiVertex*>(s.ui.memory.mapped);
        for (const UiQuad& q : quads) {
            const glm::vec2 p0 = q.position, p1 = q.position + q.size;
            const UiVertex c[4] = {{p0, q.uv0, q.color}, {{p1.x, p0.y}, {q.uv1.x, q.uv0.y}, q.color},
                                   {p1, q.uv1, q.color}, {{p0.x, p1.y}, {q.uv0.x, q.uv1.y}, q.color}};
            for (int k : {0, 1, 2, 0, 2, 3}) *v++ = c[k];
        }
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
    }

    // ---- sync / helpers ---------------------------------------------------------------------------

    void wait(std::uint64_t value) {
        if (!value) return;
        const VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &timeline_, &value};
        check(vk_.vkWaitSemaphores(device_, &info, UINT64_MAX), "vkWaitSemaphores");
        completed_value_ = std::max(completed_value_, value);
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

    // ponytail: one-shot uploads/readbacks wait for the whole queue; batch them if loading gets slow.
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
        const std::uint32_t n = s.present_timed ? QCount : QPresentBegin;
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
        s.timed = false;
    }

    void report_timings() {
        for (Slot& s : slots_) collect_timings(s);
        if (!timing_.frames) return;
        const double n = double(timing_.frames);
        std::fprintf(stderr, "joan-native: vulkan gpu timing (%llu frames, mean ms): scene %.3f, ui %.3f, frame %.3f, present-blit %.3f\n",
                     static_cast<unsigned long long>(timing_.frames), timing_.scene / n, timing_.ui / n, timing_.frame / n,
                     timing_.presents ? timing_.present / double(timing_.presents) : 0.0);
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
    VkBool32 anisotropy_ = VK_FALSE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    GpuAllocator allocator_;

    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::uint64_t timeline_value_ = 0, completed_value_ = 0;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer upload_cmd_ = VK_NULL_HANDLE;

    VkSampler samplers_[2][2]{};  // [repeat][anisotropic]
    VkDescriptorSetLayout textures_layout_ = VK_NULL_HANDLE, frame_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet textures_set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    bool cache_loaded_ = false;
    VkPipeline opaque_ = VK_NULL_HANDLE, blend_ = VK_NULL_HANDLE, additive_ = VK_NULL_HANDLE, ui_ = VK_NULL_HANDLE;

    Slot slots_[kFramesInFlight];
    std::uint32_t slot_ = kFramesInFlight - 1;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    bool recording_ = false, scene_rendered_ = false;

    Image2D color_, depth_;
    glm::ivec2 target_size_{0, 0};

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D swapchain_extent_{0, 0};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkSemaphore> present_done_;
    bool swapchain_dirty_ = false;

    std::unordered_map<std::uint32_t, Mesh> meshes_;
    std::unordered_map<std::uint32_t, Texture> textures_;
    std::uint32_t next_id_ = 1, next_slot_ = 0;
    std::vector<std::uint32_t> free_slots_;
    TextureHandle white_;
    std::vector<std::pair<std::uint64_t, std::function<void()>>> graveyard_;
    std::vector<const DrawItem*> order_;

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

}  // namespace joa::engine
