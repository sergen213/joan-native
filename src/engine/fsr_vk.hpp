#pragma once
// AMD FidelityFX Super Resolution 3.1.4 upscaler + frame generation (optical flow, frame interpolation) for the Vulkan
// renderer: FidelityFX SDK v1.1.4 Vulkan backend, third_party/fidelityfx-sdk (README.md: provenance, local patches).
// Wrapper ported from IKV src/fsr_vk.cpp; Vulkan resolved at runtime like the renderer (joa_ffx_load_vulkan).
// Inputs: render-size tone-mapped colour, R32F device depth (non-inverted), unjittered UV-space motion
// (previous uv = uv + motion). Records compute work into the caller's command buffer; never submits or waits.
#include "engine/vk_api.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace joa::engine {

class Fsr3 {
public:
    // Once per instance, before anything else: the backend's direct vk* calls. False = FSR 3 unavailable.
    static bool load(PFN_vkGetInstanceProcAddr, VkInstance);
    // nullptr when the device can run the upscaler / additionally frame generation on `display` images, else the reason.
    static const char* unsupported(const vk::Api&, VkPhysicalDevice);
    static const char* generation_unsupported(const vk::Api&, VkPhysicalDevice, VkFormat display);
    // generation_format != UNDEFINED also creates optical flow + frame interpolation (display size, that format).
    Fsr3(VkPhysicalDevice, VkDevice, VkExtent2D render, VkExtent2D display, VkFormat generation_format);
    ~Fsr3();
    Fsr3(const Fsr3&) = delete;
    Fsr3& operator=(const Fsr3&) = delete;

    struct Image {
        VkImage image;
        VkFormat format;
        VkExtent2D extent;
        VkImageUsageFlags usage;
    };
    // Every image is in SHADER_READ_ONLY_OPTIMAL and is returned in that layout (output needs STORAGE usage). One upscaler
    // call per frame: each call advances the SDK's 4-entry per-frame descriptor-view ring (FFX_MAX_QUEUED_FRAMES).
    struct Dispatch {
        VkCommandBuffer command;
        Image color, depth, motion, reactive, output;
        std::array<float, 2> jitter;  // render pixels, x right, y down
        float frame_ms, sharpness, near_plane, far_plane, fov_y;
        bool reset;
    };
    void dispatch(const Dispatch&);
    // After dispatch() in the same command buffer: the frame halfway between the previous and this `source` (display
    // size, HUD-less, SHADER_READ_ONLY_OPTIMAL) into `output` (GENERAL, storage). FSR keeps its own copy of source.
    struct Interpolation {
        VkCommandBuffer command;
        Image source, output;
        float frame_ms, near_plane, far_plane, fov_y;
        bool reset;
        std::uint64_t frame;  // +1 per call
    };
    void interpolate(const Interpolation&);
    bool generates() const;
    VkExtent2D render() const { return render_; }
    VkExtent2D display() const { return display_; }

private:
    struct Context;
    std::unique_ptr<Context> context_;
    std::vector<unsigned char> scratch_, generation_scratch_;
    VkExtent2D render_{}, display_{};
};

}  // namespace joa::engine
