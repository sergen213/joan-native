#include "game/savegame_container.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

#include "formats/vfs_bytes.hpp"

namespace joa::game {

namespace fs = std::filesystem;

namespace {
constexpr char kMagic[8] = {'J', 'O', 'A', 'N', 'S', 'A', 'V', 'E'};

std::array<char, 4> tag_of(std::string_view t) {
    std::array<char, 4> a{' ', ' ', ' ', ' '};
    for (size_t i = 0; i < 4 && i < t.size(); ++i) a[i] = t[i];
    return a;
}

bool fsync_path(const fs::path& p, int flags) {
    const int fd = ::open(p.c_str(), flags);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

bool is_install_dir(const fs::path& dir) {
    std::error_code ec;
    for (const char* exe : {"joan.exe", "JoanofArc.exe"}) {
        if (fs::exists(dir / exe, ec)) return true;
        if (fs::exists(dir.parent_path() / exe, ec) && dir.filename() == "Save") return true;
    }
    return false;
}
}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::uint8_t b : data) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void SaveFile::add(std::string_view tag4, std::vector<std::uint8_t> data) {
    sections.push_back({tag_of(tag4), std::move(data)});
}

const SaveSection* SaveFile::find(std::string_view tag4) const {
    const auto t = tag_of(tag4);
    for (const auto& s : sections)
        if (s.tag == t) return &s;
    return nullptr;
}

std::vector<std::uint8_t> encode_save(const SaveFile& f) {
    std::vector<std::uint8_t> out;
    ByteWriter w(out);
    out.insert(out.end(), kMagic, kMagic + 8);
    w.u32(f.version);
    w.u32(static_cast<std::uint32_t>(f.sections.size()));
    for (const auto& s : f.sections) {
        out.insert(out.end(), s.tag.begin(), s.tag.end());
        w.u32(static_cast<std::uint32_t>(s.data.size()));
        w.u32(crc32(s.data));
        out.insert(out.end(), s.data.begin(), s.data.end());
    }
    w.u32(crc32(out));
    return out;
}

std::optional<SaveFile> decode_save(std::span<const std::uint8_t> bytes, std::string* err) {
    auto fail = [&](const char* m) -> std::optional<SaveFile> {
        if (err) *err = m;
        return std::nullopt;
    };
    if (bytes.size() < 20 || std::memcmp(bytes.data(), kMagic, 8) != 0) return fail("not a native save (bad magic)");
    formats::ByteReader tail(bytes, bytes.size() - 4);
    if (tail.u32() != crc32(bytes.first(bytes.size() - 4))) return fail("file CRC mismatch");
    formats::ByteReader r(bytes.first(bytes.size() - 4), 8);
    SaveFile f;
    f.version = r.u32();
    if (f.version == 0 || f.version > SaveFile::kVersion) return fail("unsupported save version");
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
        SaveSection s;
        auto t = r.bytes(4);
        if (!r.ok()) break;
        std::memcpy(s.tag.data(), t.data(), 4);
        const std::uint32_t size = r.u32(), crc = r.u32();
        auto d = r.bytes(size);
        if (!r.ok()) break;
        if (crc32(d) != crc) return fail("section CRC mismatch");
        s.data.assign(d.begin(), d.end());
        f.sections.push_back(std::move(s));
    }
    if (!r.ok() || !r.at_end()) return fail("truncated or malformed section table");
    return f;
}

const std::vector<SaveMigration>& save_migrations() {
    static const std::vector<SaveMigration> steps;  // steps[v-1] upgrades v -> v+1
    return steps;
}

bool migrate_save(SaveFile& f, std::string* err) {
    const auto& steps = save_migrations();
    while (f.version < SaveFile::kVersion) {
        if (f.version - 1 >= steps.size() || !steps[f.version - 1](f)) {
            if (err) *err = "no migration from save version " + std::to_string(f.version);
            return false;
        }
        ++f.version;
    }
    return true;
}

fs::path native_save_dir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    fs::path base = xdg && *xdg ? fs::path(xdg) : fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".local/share";
    return base / "joan-native" / "saves";
}

bool write_save_atomic(const fs::path& path, std::span<const std::uint8_t> bytes, int backups, std::string* err) {
    auto fail = [&](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };
    const fs::path dir = path.parent_path().empty() ? fs::path(".") : path.parent_path();
    if (is_install_dir(fs::absolute(dir))) return fail("refusing to write a native save into the original install");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return fail("cannot create " + dir.string() + ": " + ec.message());

    if (fs::exists(path, ec)) {
        auto bak = [&](int i) { return fs::path(path.string() + "." + std::to_string(i)); };
        if (backups > 0) {
            fs::remove(bak(backups), ec);
            for (int i = backups - 1; i >= 1; --i)
                if (fs::exists(bak(i), ec)) fs::rename(bak(i), bak(i + 1), ec);
            fs::copy_file(path, bak(1), fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("backup rotation failed: " + ec.message());
        }
    }

    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) return fail("cannot open " + tmp.string());
        o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!o) return fail("write failed: " + tmp.string());
    }
    if (!fsync_path(tmp, O_RDONLY)) return fail("fsync failed: " + tmp.string());
    fs::rename(tmp, path, ec);
    if (ec) return fail("rename failed: " + ec.message());
    fsync_path(dir, O_RDONLY | O_DIRECTORY);
    return true;
}

std::optional<std::vector<std::uint8_t>> read_file(const fs::path& path) {
    std::ifstream i(path, std::ios::binary);
    if (!i) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(i), {});
}

}  // namespace joa::game
