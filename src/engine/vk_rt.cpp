#include "engine/vk_rt.hpp"

#include <algorithm>
#include <cstring>

namespace joa::engine::vk {
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace {
constexpr VkBufferUsageFlags kAddress = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

void memory_barrier(const Api& vk, VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 dst_access) {
    const VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, nullptr, src, src_access, dst, dst_access};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vk.vkCmdPipelineBarrier2(cmd, &dep);
}

constexpr VkPipelineStageFlags2 kBuild = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
constexpr VkAccessFlags2 kAsRw = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
}  // namespace

void RtAccel::init(const Api& vk, VkDevice device, VkPhysicalDevice physical, GpuAllocator& allocator, VkDeviceSize vertex_stride,
                   Retire retire) {
    vk_ = &vk;
    device_ = device;
    allocator_ = &allocator;
    stride_ = vertex_stride;
    retire_ = std::move(retire);
    VkPhysicalDeviceAccelerationStructurePropertiesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &as};
    vk.vkGetPhysicalDeviceProperties2(physical, &p);
    scratch_align_ = std::max<VkDeviceSize>(as.minAccelerationStructureScratchOffsetAlignment, 1);
    const VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0,
                                  VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, kQueries};
    check(vk.vkCreateQueryPool(device, &q, nullptr, &queries_), "vkCreateQueryPool (AS compaction)");
    for (std::uint32_t i = kQueries; i-- > 0;) free_queries_.push_back(i);
}

void RtAccel::destroy() {
    if (!vk_) return;
    for (auto& [k, b] : blas_) {
        vk_->vkDestroyAccelerationStructureKHR(device_, b.as, nullptr);
        free(b.buffer);
    }
    blas_.clear();
    if (tlas_) vk_->vkDestroyAccelerationStructureKHR(device_, tlas_, nullptr);
    tlas_ = VK_NULL_HANDLE;
    for (RtBuffer* b : {&tlas_buffer_, &scratch_, &instance_buffers_[0], &instance_buffers_[1]}) free(*b);
    if (queries_) vk_->vkDestroyQueryPool(device_, queries_, nullptr);
    queries_ = VK_NULL_HANDLE;
    vk_ = nullptr;
}

RtBuffer RtAccel::create(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    RtBuffer b;
    b.size = size;
    const VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage | kAddress, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    check(vk_->vkCreateBuffer(device_, &info, nullptr, &b.buffer), "vkCreateBuffer (RT)");
    VkMemoryRequirements req;
    vk_->vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    b.memory = allocator_->allocate(req,
                                    host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                         : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                    host ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0, true);
    check(vk_->vkBindBufferMemory(device_, b.buffer, b.memory.memory, b.memory.offset), "vkBindBufferMemory (RT)");
    const VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, b.buffer};
    b.address = vk_->vkGetBufferDeviceAddress(device_, &ai);
    return b;
}

void RtAccel::free(RtBuffer& b) {
    if (b.buffer) vk_->vkDestroyBuffer(device_, b.buffer, nullptr);
    allocator_->free(b.memory);
    b = {};
}

void RtAccel::release(Blas& b) {
    if (b.query != kNoQuery) free_queries_.push_back(b.query);
    retire_([this, as = b.as, buf = b.buffer]() mutable {
        vk_->vkDestroyAccelerationStructureKHR(device_, as, nullptr);
        free(buf);
    });
    b = {};
}

void RtAccel::create_as(Blas& b, VkDeviceSize size, VkAccelerationStructureTypeKHR type) {
    b.buffer = create(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = b.buffer.buffer;
    ci.size = size;
    ci.type = type;
    check(vk_->vkCreateAccelerationStructureKHR(device_, &ci, nullptr, &b.as), "vkCreateAccelerationStructureKHR");
    const VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, nullptr, b.as};
    b.address = vk_->vkGetAccelerationStructureDeviceAddressKHR(device_, &ai);
}

VkAccelerationStructureGeometryKHR RtAccel::triangles(const RtGeometry& g) const {
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    auto& t = geo.geometry.triangles;
    t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;  // Vertex::position at offset 0
    t.vertexData.deviceAddress = g.vertices;
    t.vertexStride = stride_;
    t.maxVertex = g.vertex_count ? g.vertex_count - 1 : 0;
    t.indexType = VK_INDEX_TYPE_UINT32;
    t.indexData.deviceAddress = g.indices;
    geo.flags = 0;  // opaque or alpha-tested per instance (FORCE_[NO_]OPAQUE)
    return geo;
}

void RtAccel::ensure_scratch(VkDeviceSize bytes) {
    bytes += scratch_align_;  // room to align the start address
    if (scratch_.size >= bytes) return;
    if (scratch_.buffer) retire_([this, b = scratch_]() mutable { free(b); });
    scratch_ = create(bytes * 3 / 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
}

void RtAccel::begin(std::uint64_t completed, std::uint64_t recording) {
    completed_ = completed;
    recording_ = recording;
    pending_.clear();
    instances_.clear();
}

void RtAccel::add(const RtGeometry& g, const glm::mat4& model, std::uint32_t custom_index, std::uint8_t mask, bool opaque) {
    if (!g.vertices || !g.indices || g.index_count < 3 || !g.vertex_count) return;
    Blas& b = blas_[Key{g.mesh, g.first_index, g.index_count}];
    if (b.touched != recording_) {
        if (!b.as) {
            pending_.push_back({&b, g, false});
        } else if (g.dynamic && g.generation != b.generation) {
            // Refit in place while the topology matches; rebuild a static-built or changed one, and every
            // 64th update (refits degrade the tree as the pose drifts from the one it was built for).
            const bool refit = b.dynamic && b.vertex_count == g.vertex_count && (++b.updates % 64) != 0;
            if (!refit) release(b);
            pending_.push_back({&b, g, refit});
        }
        b.touched = recording_;
    }
    instances_.push_back({&b, model, custom_index, mask, opaque});
}

void RtAccel::forget_mesh(std::uint32_t mesh) {
    for (auto it = blas_.lower_bound(Key{mesh, 0, 0}); it != blas_.end() && std::get<0>(it->first) == mesh;) {
        release(it->second);
        it = blas_.erase(it);
    }
}

void RtAccel::build(VkCommandBuffer cmd, std::uint32_t slot) {
    // Earlier frames' ray queries / builds (same queue, earlier in submission order) before this frame's AS
    // writes, and earlier builds before the compaction copies that read them.
    memory_barrier(*vk_, cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, kBuild, kAsRw);

    // Compaction of static BLASes whose size query has retired.
    for (auto& [key, b] : blas_) {
        if (b.query == kNoQuery || b.built_at > completed_) continue;
        VkDeviceSize compact = 0;
        if (vk_->vkGetQueryPoolResults(device_, queries_, b.query, 1, sizeof compact, &compact, sizeof compact, VK_QUERY_RESULT_64_BIT) !=
                VK_SUCCESS ||
            compact == 0)
            continue;
        free_queries_.push_back(b.query);
        b.query = kNoQuery;
        Blas c;
        create_as(c, compact, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        const VkCopyAccelerationStructureInfoKHR copy{VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR, nullptr, b.as, c.as,
                                                      VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR};
        vk_->vkCmdCopyAccelerationStructureKHR(cmd, &copy);
        ++stats.compacted;
        stats.compacted_from += b.buffer.size;
        stats.compacted_to += compact;
        retire_([this, as = b.as, buf = b.buffer]() mutable {
            vk_->vkDestroyAccelerationStructureKHR(device_, as, nullptr);
            free(buf);
        });
        b.as = c.as;
        b.buffer = c.buffer;
        b.address = c.address;
    }

    // BLAS builds and refits, one batched call with a scratch region each.
    std::vector<VkAccelerationStructureGeometryKHR> geos(pending_.size());
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos(pending_.size());
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(pending_.size());
    std::vector<VkDeviceSize> offsets(pending_.size());
    std::vector<VkAccelerationStructureKHR> compactable;
    VkDeviceSize scratch = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        Pending& p = pending_[i];
        Blas& b = *p.blas;
        geos[i] = triangles(p.g);
        VkAccelerationStructureBuildGeometryInfoKHR& info = infos[i];
        info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = p.g.dynamic ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR
                                 : VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = p.update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geos[i];
        const std::uint32_t prims = p.g.index_count / 3;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vk_->vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &prims, &sizes);
        if (!p.update) {
            create_as(b, sizes.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
            b.dynamic = p.g.dynamic;
            b.vertex_count = p.g.vertex_count;
            b.index_count = p.g.index_count;
            b.built_at = recording_;
            ++(p.g.dynamic ? stats.rebuilds : stats.built);
            if (!p.g.dynamic && !free_queries_.empty()) {
                b.query = free_queries_.back();
                free_queries_.pop_back();
                compactable.push_back(b.as);
            }
        } else {
            ++stats.refits;
        }
        b.generation = p.g.generation;
        info.srcAccelerationStructure = p.update ? b.as : VK_NULL_HANDLE;
        info.dstAccelerationStructure = b.as;
        offsets[i] = scratch;
        scratch += align(p.update ? sizes.updateScratchSize : sizes.buildScratchSize);
        ranges[i] = {prims, p.g.first_index * std::uint32_t(sizeof(std::uint32_t)), 0, 0};
    }

    // TLAS input: this slot's host-visible instance array.
    std::vector<VkAccelerationStructureInstanceKHR> inst;
    inst.reserve(instances_.size());
    for (const Instance& i : instances_) {
        VkAccelerationStructureInstanceKHR v{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) v.transform.matrix[r][c] = i.model[c][r];
        v.instanceCustomIndex = i.custom & 0xffffffu;
        v.mask = i.mask;
        v.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                  (i.opaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
        v.accelerationStructureReference = i.blas->address;
        inst.push_back(v);
    }
    RtBuffer& ib = instance_buffers_[slot & 1];
    const VkDeviceSize ibytes = std::max<std::size_t>(inst.size(), 1) * sizeof(VkAccelerationStructureInstanceKHR);
    if (ib.size < ibytes) {
        if (ib.buffer) retire_([this, b = ib]() mutable { free(b); });
        ib = create(ibytes * 3 / 2, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true);
    }
    if (!inst.empty()) std::memcpy(ib.memory.mapped, inst.data(), inst.size() * sizeof inst[0]);

    VkAccelerationStructureGeometryKHR tgeo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tgeo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tgeo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tgeo.geometry.instances.data.deviceAddress = ib.address;
    VkAccelerationStructureBuildGeometryInfoKHR tinfo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tinfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tinfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tinfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tinfo.geometryCount = 1;
    tinfo.pGeometries = &tgeo;
    const std::uint32_t count = std::uint32_t(inst.size());
    VkAccelerationStructureBuildSizesInfoKHR tsizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vk_->vkGetAccelerationStructureBuildSizesKHR(device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tinfo, &count, &tsizes);
    if (!tlas_ || tlas_buffer_.size < tsizes.accelerationStructureSize) {
        if (tlas_) retire_([this, as = tlas_, b = tlas_buffer_]() mutable {
            vk_->vkDestroyAccelerationStructureKHR(device_, as, nullptr);
            free(b);
        });
        Blas t;
        create_as(t, std::max<VkDeviceSize>(tsizes.accelerationStructureSize, 4096) * 3 / 2, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
        tlas_ = t.as;
        tlas_buffer_ = t.buffer;
    }
    ensure_scratch(std::max(scratch, align(tsizes.buildScratchSize)));
    const VkDeviceAddress base = (scratch_.address + scratch_align_ - 1) / scratch_align_ * scratch_align_;

    if (!infos.empty()) {
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> pr(ranges.size());
        for (std::size_t i = 0; i < infos.size(); ++i) {
            infos[i].scratchData.deviceAddress = base + offsets[i];
            pr[i] = &ranges[i];
        }
        vk_->vkCmdBuildAccelerationStructuresKHR(cmd, std::uint32_t(infos.size()), infos.data(), pr.data());
    }
    memory_barrier(*vk_, cmd, kBuild, kAsRw, kBuild, kAsRw);
    if (!compactable.empty()) {
        for (auto& [key, b] : blas_)
            if (b.query != kNoQuery && b.built_at == recording_) {
                vk_->vkCmdResetQueryPool(cmd, queries_, b.query, 1);
                vk_->vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &b.as, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                                   queries_, b.query);
            }
    }
    tinfo.dstAccelerationStructure = tlas_;
    tinfo.scratchData.deviceAddress = base;
    const VkAccelerationStructureBuildRangeInfoKHR trange{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ptr = &trange;
    vk_->vkCmdBuildAccelerationStructuresKHR(cmd, 1, &tinfo, &ptr);
    memory_barrier(*vk_, cmd, kBuild, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    stats.blas = std::uint32_t(blas_.size());
    stats.instances = count;
}

}  // namespace joa::engine::vk
