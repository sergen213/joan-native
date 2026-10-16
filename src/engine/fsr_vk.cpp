#include "engine/fsr_vk.hpp"

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <fsr3upscaler/ffx_fsr3upscaler_private.h>
#include <joa_ffx_vulkan.h>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace joa::engine {
namespace {
// Vulkan create-info structs are routinely partially brace-initialised (sType + leading members).
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

// Joan's world unit: a unit's collision cylinder is 80 units for a ~1.8 m soldier (unitclass_db.hpp cyl_height).
constexpr float kMetresPerUnit = 1.8f / 80.0f;

void check(FfxErrorCode code, const char* what) {
    if (code != FFX_OK) throw std::runtime_error(std::string("FSR 3.1.4 ") + what + " failed: FfxErrorCode " + std::to_string(code));
}

void message(FfxMsgType type, const wchar_t* text) {
    std::string narrow;
    for (const wchar_t* p = text; p && *p; ++p) narrow += *p < 128 ? char(*p) : '?';
    std::fprintf(stderr, "joan-native: FSR 3.1.4 %s: %s\n", type == FFX_MESSAGE_TYPE_ERROR ? "error" : "warning", narrow.c_str());
}

FfxResource resource(const Fsr3::Image& image, const wchar_t* name, FfxResourceStates state = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ) {
    if (!image.image) return {};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = image.format;
    info.extent = {image.extent.width, image.extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.usage = image.usage;
    return ffxGetResourceVK(reinterpret_cast<void*>(image.image), ffxGetImageResourceDescriptionVK(image.image, info), name, state);
}

// The backend keeps the VkDeviceContext pointer until each effect's backend context exists: `vk` outlives that.
FfxInterface backend_interface(VkDeviceContext& vk, std::vector<unsigned char>& scratch, std::size_t contexts) {
    scratch.assign(ffxGetScratchMemorySizeVK(vk.vkPhysicalDevice, contexts), 0);
    FfxInterface backend{};
    check(ffxGetInterfaceVK(&backend, ffxGetDeviceVK(&vk), scratch.data(), scratch.size(), contexts), "Vulkan backend");
    joa_ffx_clamp_capabilities(backend);
    return backend;
}

}  // namespace

// FSR 3.1.4 dispatch writes dilated depth/motion and reconstructed previous depth into caller-owned "shared" resources
// (ffxFsr3UpscalerGetSharedResourceDescriptions), created here in the upscaler's own backend effect context; frame
// interpolation reads them like the SDK's FSR3 component does. Frame generation has its own backend (optical flow,
// frame interpolation and a shared-resource effect context holding the optical-flow vector/scene-change outputs).
struct Fsr3::Context {
    FfxFsr3UpscalerContext fsr{};
    std::array<FfxResourceInternal, 3> shared{{{-1}, {-1}, {-1}}};  // -1: never created, DestroyResourceVK ignores it
    // The SDK itself reinterprets the opaque context storage as its private struct.
    FfxFsr3UpscalerContext_Private* const state = reinterpret_cast<FfxFsr3UpscalerContext_Private*>(fsr.data);
    FfxInterface& backend() { return state->contextDescription.backendInterface; }
    std::uint32_t effect() { return state->effectContextId; }
    FfxInterface generation{};
    FfxOpticalflowContext flow{};
    FfxFrameInterpolationContext interpolation{};
    bool fsr_created = false, flow_created = false, interpolation_created = false, flow_effect_created = false;
    std::uint32_t flow_effect = 0;
    std::array<FfxResourceInternal, 2> flow_output{{{-1}, {-1}}};  // vector, scene change
    void release() {
        if (flow_effect_created) {
            for (auto& r : flow_output) generation.fpDestroyResource(&generation, r, flow_effect);
            generation.fpDestroyBackendContext(&generation, flow_effect);
        }
        if (interpolation_created) ffxFrameInterpolationContextDestroy(&interpolation);
        if (flow_created) ffxOpticalflowContextDestroy(&flow);
        if (fsr_created) {
            for (auto& r : shared) backend().fpDestroyResource(&backend(), r, effect());
            ffxFsr3UpscalerContextDestroy(&fsr);
        }
    }
};

bool Fsr3::load(PFN_vkGetInstanceProcAddr gipa, VkInstance instance) { return joa_ffx_load_vulkan(gipa, instance); }

const char* Fsr3::unsupported(const vk::Api& vk, VkPhysicalDevice physical) {
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vk.vkGetPhysicalDeviceFeatures2(physical, &f);
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
    vk.vkGetPhysicalDeviceProperties2(physical, &props);
    constexpr VkSubgroupFeatureFlags quad = VK_SUBGROUP_FEATURE_QUAD_BIT | VK_SUBGROUP_FEATURE_BASIC_BIT;
    return !f.features.shaderStorageImageWriteWithoutFormat         ? "shaderStorageImageWriteWithoutFormat unavailable"
           : !f.features.shaderStorageImageExtendedFormats          ? "shaderStorageImageExtendedFormats unavailable"
           : !(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
                     (subgroup.supportedOperations & quad) != quad ? "compute subgroup quad operations unavailable"
                                                                    : nullptr;
}

const char* Fsr3::generation_unsupported(const vk::Api& vk, VkPhysicalDevice physical, VkFormat display) {
    if (const char* why = unsupported(vk, physical)) return why;
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vk.vkGetPhysicalDeviceFeatures2(physical, &f);
    VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
    vk.vkGetPhysicalDeviceProperties2(physical, &props);
    VkFormatProperties format{};
    vk.vkGetPhysicalDeviceFormatProperties(physical, display, &format);
    constexpr VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                            VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    // Optical flow's block search reduces with subgroup add/min; interpolation reads the output without a format qualifier.
    return !(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) ? "compute subgroup arithmetic unavailable"
           : !f.features.shaderStorageImageReadWithoutFormat                    ? "shaderStorageImageReadWithoutFormat unavailable"
           : (format.optimalTilingFeatures & needed) != needed                  ? "RGBA8 lacks storage/sampled/attachment/transfer support"
                                                                                : nullptr;
}

Fsr3::Fsr3(VkPhysicalDevice physical, VkDevice device, VkExtent2D render, VkExtent2D display, VkFormat generation_format)
    : context_(std::make_unique<Context>()), render_(render), display_(display) {
    VkDeviceContext vk{device, physical, joa_ffx_device_proc};
    auto& c = *context_;
    FfxFsr3UpscalerContextDescription d{};
    d.backendInterface = backend_interface(vk, scratch_, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    d.maxRenderSize = {render.width, render.height};
    d.maxUpscaleSize = {display.width, display.height};
    d.fpMessage = message;
    check(ffxFsr3UpscalerContextCreate(&c.fsr, &d), "context creation");
    c.fsr_created = true;
    try {
        FfxFsr3UpscalerSharedResourceDescriptions shared{};
        check(ffxFsr3UpscalerGetSharedResourceDescriptions(&c.fsr, &shared), "shared resource descriptions");
        const FfxCreateResourceDescription* descriptions[]{&shared.dilatedDepth, &shared.dilatedMotionVectors, &shared.reconstructedPrevNearestDepth};
        for (std::size_t i = 0; i < 3; ++i) check(c.backend().fpCreateResource(&c.backend(), descriptions[i], c.effect(), &c.shared[i]), "shared resource");
        if (generation_format != VK_FORMAT_UNDEFINED) {
            // ponytail: the SDK's optical-flow/interpolation create leaks what it built when it fails (no release path
            // upstream, unlike the patched upscaler); the renderer then runs without frame generation.
            c.generation = backend_interface(vk, generation_scratch_, 3);
            FfxOpticalflowContextDescription flow{};
            flow.backendInterface = c.generation;
            flow.resolution = {display.width, display.height};
            check(ffxOpticalflowContextCreate(&c.flow, &flow), "optical flow context creation");
            c.flow_created = true;
            FfxFrameInterpolationContextDescription fi{};
            fi.backendInterface = c.generation;
            fi.maxRenderSize = {render.width, render.height};
            fi.displaySize = {display.width, display.height};
            fi.backBufferFormat = fi.previousInterpolationSourceFormat = ffxGetSurfaceFormatVK(generation_format);
            check(ffxFrameInterpolationContextCreate(&c.interpolation, &fi), "frame interpolation context creation");
            c.interpolation_created = true;
            check(c.generation.fpCreateBackendContext(&c.generation, FFX_EFFECT_SHAREDRESOURCES, nullptr, &c.flow_effect), "shared resource context");
            c.flow_effect_created = true;
            FfxOpticalflowSharedResourceDescriptions outputs{};
            check(ffxOpticalflowGetSharedResourceDescriptions(&c.flow, &outputs), "optical flow resource descriptions");
            check(c.generation.fpCreateResource(&c.generation, &outputs.opticalFlowVector, c.flow_effect, &c.flow_output[0]), "optical flow vector");
            check(c.generation.fpCreateResource(&c.generation, &outputs.opticalFlowSCD, c.flow_effect, &c.flow_output[1]), "optical flow scene change");
        }
    } catch (...) {
        c.release();
        throw;
    }
}

Fsr3::~Fsr3() { context_->release(); }

void Fsr3::dispatch(const Dispatch& in) {
    auto& c = *context_;
    FfxFsr3UpscalerDispatchDescription d{};
    d.commandList = ffxGetCommandListVK(in.command);
    d.color = resource(in.color, L"JOA_Color");
    d.depth = resource(in.depth, L"JOA_Depth");
    d.motionVectors = resource(in.motion, L"JOA_MotionVectors");
    d.reactive = resource(in.reactive, L"JOA_Reactive");
    d.dilatedDepth = c.backend().fpGetResource(&c.backend(), c.shared[0]);
    d.dilatedMotionVectors = c.backend().fpGetResource(&c.backend(), c.shared[1]);
    d.reconstructedPrevNearestDepth = c.backend().fpGetResource(&c.backend(), c.shared[2]);
    d.output = resource(in.output, L"JOA_Upscaled");
    d.jitterOffset = {in.jitter[0], in.jitter[1]};
    d.motionVectorScale = {float(render_.width), float(render_.height)};  // UV -> render pixels
    d.renderSize = {render_.width, render_.height};
    d.upscaleSize = {display_.width, display_.height};
    d.enableSharpening = in.sharpness > 0.0f;
    d.sharpness = in.sharpness;
    d.frameTimeDelta = in.frame_ms;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.near_plane;
    d.cameraFar = in.far_plane;
    d.cameraFovAngleVertical = in.fov_y;
    d.viewSpaceToMetersFactor = kMetresPerUnit;
    check(ffxFsr3UpscalerContextDispatch(&c.fsr, &d), "dispatch");
}

bool Fsr3::generates() const { return context_->interpolation_created; }

void Fsr3::interpolate(const Interpolation& in) {
    auto& c = *context_;
    if (!c.interpolation_created) throw std::logic_error("FSR 3.1.4 frame interpolation was not created");
    const auto vector = c.generation.fpGetResource(&c.generation, c.flow_output[0]);
    const auto scene_change = c.generation.fpGetResource(&c.generation, c.flow_output[1]);
    const auto source = resource(in.source, L"JOA_InterpolationSource");
    FfxOpticalflowDispatchDescription flow{};
    flow.commandList = ffxGetCommandListVK(in.command);
    flow.color = source;
    flow.opticalFlowVector = vector;
    flow.opticalFlowSCD = scene_change;
    flow.reset = in.reset;
    flow.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    check(ffxOpticalflowContextDispatch(&c.flow, &flow), "optical flow dispatch");
    // Same parameters the SDK's FSR3 component passes when its upscaler produced the dilated inputs (ffx_fsr3.cpp).
    FfxFrameInterpolationDispatchDescription fi{};
    fi.commandList = flow.commandList;
    fi.displaySize = {display_.width, display_.height};
    fi.renderSize = {render_.width, render_.height};
    fi.currentBackBuffer = source;
    fi.output = resource(in.output, L"JOA_Interpolated", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    fi.interpolationRect = {0, 0, int(display_.width), int(display_.height)};
    fi.opticalFlowVector = vector;
    fi.opticalFlowSceneChangeDetection = scene_change;
    fi.opticalFlowBlockSize = 8;
    fi.opticalFlowScale = {1.0f / float(display_.width), 1.0f / float(display_.height)};
    fi.cameraNear = in.near_plane;
    fi.cameraFar = in.far_plane;
    fi.cameraFovAngleVertical = in.fov_y;
    fi.viewSpaceToMetersFactor = kMetresPerUnit;
    fi.frameTimeDelta = in.frame_ms;
    fi.reset = in.reset;
    fi.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    fi.frameID = in.frame;
    fi.dilatedDepth = c.backend().fpGetResource(&c.backend(), c.shared[0]);
    fi.dilatedMotionVectors = c.backend().fpGetResource(&c.backend(), c.shared[1]);
    fi.reconstructedPrevDepth = c.backend().fpGetResource(&c.backend(), c.shared[2]);
    check(ffxFrameInterpolationDispatch(&c.interpolation, &fi), "frame interpolation dispatch");
}

}  // namespace joa::engine
