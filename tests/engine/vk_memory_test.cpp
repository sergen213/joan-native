// RangeAllocator (the free-list core of the Vulkan sub-allocator): alignment, reuse, coalescing.
#include "check.hpp"
#include "engine/vk_memory.hpp"

using joa::engine::vk::RangeAllocator;

int main() {
    RangeAllocator r(1024);
    const auto a = r.allocate(100, 1);
    const auto b = r.allocate(100, 256);  // aligned up; the 100..256 gap stays free
    const auto c = r.allocate(50, 1);
    CHECK(a && *a == 0);
    CHECK(b && *b == 256);
    CHECK(c && *c == 100);  // first fit lands in the alignment gap
    CHECK(!r.allocate(2048, 1));
    r.free(*b, 100);
    const auto d = r.allocate(600, 256);
    CHECK(d && *d == 256);  // freed block coalesced with the tail
    r.free(*a, 100);
    r.free(*c, 50);
    r.free(*d, 600);
    CHECK(r.empty());
    const auto e = r.allocate(1024, 1);
    CHECK(e && *e == 0);
    return test_result();
}
