#pragma once
// Vulkan entry points loaded at runtime from SDL's vkGetInstanceProcAddr (no link-time libvulkan).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <stdexcept>
#include <string>

namespace joa::engine::vk {

#define JOA_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion)
#define JOA_VK_INSTANCE(X)                                                                                                    \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures2)      \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkDestroySurfaceKHR)               \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT)          \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceProperties2)
#define JOA_VK_DEVICE(X)                                                                                                      \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueWaitIdle) X(vkQueueSubmit2) X(vkQueuePresentKHR)       \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)                       \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkResetCommandPool)                         \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores)               \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkCreateImage)               \
    X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)          \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateSampler) X(vkDestroySampler)                                \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool)        \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout)                 \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)                         \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) X(vkCreateQueryPool) X(vkDestroyQueryPool)    \
    X(vkGetQueryPoolResults) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp2) X(vkCmdPipelineBarrier2) X(vkCmdBeginRendering)    \
    X(vkCmdEndRendering) X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets)      \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor)             \
    X(vkCmdSetCullMode) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage)                          \
    X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdSetStencilReference) X(vkCmdFillBuffer) X(vkCmdCopyImage)            \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkGetBufferDeviceAddress) X(vkCmdResolveImage)                              \
    /* VK_KHR_acceleration_structure (null unless ray tracing is enabled, docs/RAYTRACING.md) */                               \
    X(vkCreateAccelerationStructureKHR) X(vkDestroyAccelerationStructureKHR) X(vkGetAccelerationStructureBuildSizesKHR)        \
    X(vkCmdBuildAccelerationStructuresKHR) X(vkGetAccelerationStructureDeviceAddressKHR)                                      \
    X(vkCmdWriteAccelerationStructuresPropertiesKHR) X(vkCmdCopyAccelerationStructureKHR)

struct Api {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define JOA_VK_FIELD(name) PFN_##name name = nullptr;
    JOA_VK_GLOBAL(JOA_VK_FIELD)
    JOA_VK_INSTANCE(JOA_VK_FIELD)
    JOA_VK_DEVICE(JOA_VK_FIELD)
#undef JOA_VK_FIELD

    void load_global(PFN_vkGetInstanceProcAddr gipa) {
        vkGetInstanceProcAddr = gipa;
#define JOA_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(gipa(nullptr, #name));
        JOA_VK_GLOBAL(JOA_VK_LOAD)
#undef JOA_VK_LOAD
    }
    // Extension entry points (surface, debug utils) stay null when the extension is not enabled.
    void load_instance(VkInstance instance) {
#define JOA_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
        JOA_VK_INSTANCE(JOA_VK_LOAD)
#undef JOA_VK_LOAD
    }
    void load_device(VkDevice device) {
#define JOA_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
        JOA_VK_DEVICE(JOA_VK_LOAD)
#undef JOA_VK_LOAD
    }
};

inline void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")");
}

}  // namespace joa::engine::vk
