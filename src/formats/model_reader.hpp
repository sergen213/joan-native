#pragma once
// Bounds-checked little-endian reader shared by the model/anim/dds parsers (internal).
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace joa::formats::detail {

struct ByteReader {
  std::span<const uint8_t> d;
  size_t o = 0;
  bool ok = true;  // sticky: false after any out-of-bounds read

  size_t left() const { return ok ? d.size() - o : 0; }
  bool has(size_t n) { if (!ok || n > d.size() - o) ok = false; return ok; }
  // Reject element counts that cannot fit in the remaining bytes (prevents huge allocations).
  bool fits(uint64_t count, uint64_t elemSize) {
    if (!ok || (elemSize && count > (d.size() - o) / elemSize)) ok = false;
    return ok;
  }
  bool read(void* dst, size_t n) {
    if (!has(n)) { std::memset(dst, 0, n); return false; }
    std::memcpy(dst, d.data() + o, n);  // host is little-endian x86-64/aarch64
    o += n;
    return true;
  }
  uint32_t u32() { uint32_t v; read(&v, 4); return v; }
  int32_t i32() { int32_t v; read(&v, 4); return v; }
  float f32() { float v; read(&v, 4); return v; }
  uint8_t u8() { uint8_t v; read(&v, 1); return v; }
  bool skip(size_t n) { if (has(n)) o += n; return ok; }
  // Fixed-size NUL-padded string field.
  std::string str(size_t n) {
    if (!has(n)) return {};
    const char* p = reinterpret_cast<const char*>(d.data() + o);
    o += n;
    return std::string(p, strnlen(p, n));
  }
};

}  // namespace joa::formats::detail
