#pragma once
// Minimal sub-allocator: 64 MiB VkDeviceMemory blocks per memory type, first-fit free lists.
#include "engine/vk_api.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace joa::engine::vk {

// First-fit free list over [0, size) with coalescing on free. Pure logic (unit-tested without a GPU).
class RangeAllocator {
public:
    explicit RangeAllocator(std::uint64_t size) : size_(size) { free_[0] = size; }
    std::optional<std::uint64_t> allocate(std::uint64_t size, std::uint64_t alignment);
    void free(std::uint64_t offset, std::uint64_t size);
    bool empty() const { return free_.size() == 1 && free_.begin()->second == size_; }

private:
    std::uint64_t size_;
    std::map<std::uint64_t, std::uint64_t> free_;  // offset -> length
};

struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    std::uint8_t* mapped = nullptr;  // persistently mapped when host visible
    void* block = nullptr;
};

// ponytail: blocks are never returned to the driver until destroy() except dedicated (oversized) ones.
class GpuAllocator {
public:
    // device_address: every block gets VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT (ray tracing: AS inputs, buffer references).
    void init(const Api& api, VkDevice device, const VkPhysicalDeviceMemoryProperties& props, bool device_address = false) {
        api_ = &api;
        device_ = device;
        props_ = props;
        device_address_ = device_address;
    }
    // `linear` separates buffers from optimal-tiling images (bufferImageGranularity). Throws on failure.
    Allocation allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
                        bool linear);
    void free(const Allocation& a);
    void destroy();

private:
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::uint32_t type = 0;
        bool linear = false, dedicated = false;
        std::uint8_t* mapped = nullptr;
        RangeAllocator ranges;
    };
    Block* new_block(std::uint32_t type, VkDeviceSize size, bool linear, bool dedicated);

    const Api* api_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props_{};
    bool device_address_ = false;
    std::vector<std::unique_ptr<Block>> blocks_;
};

}  // namespace joa::engine::vk
