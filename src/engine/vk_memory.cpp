#include "engine/vk_memory.hpp"

#include <algorithm>

namespace joa::engine::vk {
namespace {
constexpr VkDeviceSize kBlockSize = 64ull << 20;
}

std::optional<std::uint64_t> RangeAllocator::allocate(std::uint64_t size, std::uint64_t alignment) {
    if (size == 0) size = 1;
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        const auto [start, length] = *it;
        const std::uint64_t aligned = (start + alignment - 1) / alignment * alignment;
        if (aligned + size > start + length) continue;
        free_.erase(it);
        if (aligned > start) free_[start] = aligned - start;
        if (aligned + size < start + length) free_[aligned + size] = start + length - aligned - size;
        return aligned;
    }
    return std::nullopt;
}

void RangeAllocator::free(std::uint64_t offset, std::uint64_t size) {
    if (size == 0) size = 1;
    auto it = free_.emplace(offset, size).first;
    if (const auto next = std::next(it); next != free_.end() && offset + size == next->first) {
        it->second += next->second;
        free_.erase(next);
    }
    if (it != free_.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            free_.erase(it);
        }
    }
}

GpuAllocator::Block* GpuAllocator::new_block(std::uint32_t type, VkDeviceSize size, bool linear, bool dedicated) {
    const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, device_address_ ? &flags : nullptr, size, type};
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (api_->vkAllocateMemory(device_, &info, nullptr, &memory) != VK_SUCCESS) return nullptr;
    void* mapped = nullptr;
    if (props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        check(api_->vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    blocks_.push_back(std::make_unique<Block>(Block{memory, type, linear, dedicated, static_cast<std::uint8_t*>(mapped), RangeAllocator(size)}));
    return blocks_.back().get();
}

Allocation GpuAllocator::allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags required,
                                  VkMemoryPropertyFlags preferred, bool linear) {
    // Preferred flags first (e.g. host-visible VRAM), then the bare requirement.
    for (const VkMemoryPropertyFlags flags : {required | preferred, required}) {
        for (std::uint32_t type = 0; type < props_.memoryTypeCount; ++type) {
            if (!(req.memoryTypeBits & (1u << type)) || (props_.memoryTypes[type].propertyFlags & flags) != flags) continue;
            const bool dedicated = req.size > kBlockSize / 2;
            if (!dedicated)
                for (auto& b : blocks_) {
                    if (b->type != type || b->linear != linear || b->dedicated) continue;
                    if (const auto off = b->ranges.allocate(req.size, req.alignment))
                        return {b->memory, *off, req.size, b->mapped ? b->mapped + *off : nullptr, b.get()};
                }
            Block* b = new_block(type, dedicated ? req.size : kBlockSize, linear, dedicated);
            if (!b) continue;  // heap full: try the next compatible type
            const std::uint64_t off = *b->ranges.allocate(req.size, req.alignment);
            return {b->memory, off, req.size, b->mapped ? b->mapped + off : nullptr, b};
        }
    }
    throw std::runtime_error("Vulkan: out of device memory");
}

void GpuAllocator::free(const Allocation& a) {
    if (!a.block) return;
    const auto it = std::find_if(blocks_.begin(), blocks_.end(), [&](const auto& b) { return b.get() == a.block; });
    if (it == blocks_.end()) return;
    (*it)->ranges.free(a.offset, a.size);
    if ((*it)->dedicated) {
        api_->vkFreeMemory(device_, (*it)->memory, nullptr);
        blocks_.erase(it);
    }
}

void GpuAllocator::destroy() {
    for (auto& b : blocks_) api_->vkFreeMemory(device_, b->memory, nullptr);
    blocks_.clear();
}

}  // namespace joa::engine::vk
