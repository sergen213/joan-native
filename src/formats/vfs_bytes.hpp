// Bounds-checked little-endian reader shared by the WorldFormats parsers.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>

namespace joa::formats {

static_assert(std::endian::native == std::endian::little, "parsers assume a little-endian host");

// Any read past the end sets a sticky failure flag and yields zero/empty values,
// so parsers can read a whole structure and check ok() once.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data, std::size_t pos = 0)
        : d_(data), pos_(pos), ok_(pos <= data.size()) {}

    bool ok() const { return ok_; }
    std::size_t pos() const { return pos_; }
    std::size_t size() const { return d_.size(); }
    std::size_t remaining() const { return ok_ ? d_.size() - pos_ : 0; }
    bool at_end() const { return ok_ && pos_ == d_.size(); }
    void fail() { ok_ = false; }

    void seek(std::size_t p) {
        if (p > d_.size()) ok_ = false;
        else pos_ = p;
    }
    void skip(std::size_t n) { if (need(n)) pos_ += n; }

    template <class T>
    T get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v{};
        if (!need(sizeof(T))) return v;
        std::memcpy(&v, d_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    std::uint8_t u8() { return get<std::uint8_t>(); }
    std::uint16_t u16() { return get<std::uint16_t>(); }
    std::int16_t i16() { return get<std::int16_t>(); }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    std::int32_t i32() { return get<std::int32_t>(); }
    float f32() { return get<float>(); }

    std::span<const std::uint8_t> bytes(std::size_t n) {
        if (!need(n)) return {};
        auto s = d_.subspan(pos_, n);
        pos_ += n;
        return s;
    }
    // Fixed-size field holding a NUL-terminated string (bytes after the NUL are ignored).
    std::string fixed_string(std::size_t n) { return cstr(bytes(n)); }

    static std::string cstr(std::span<const std::uint8_t> s) {
        std::size_t len = 0;
        while (len < s.size() && s[len] != 0) ++len;
        return std::string(reinterpret_cast<const char*>(s.data()), len);
    }

private:
    bool need(std::size_t n) {
        if (!ok_ || n > d_.size() - pos_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> d_;
    std::size_t pos_;
    bool ok_;
};

}  // namespace joa::formats
