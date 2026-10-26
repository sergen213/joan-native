#include "formats/pfd_mesh.hpp"

#include "formats/vfs_bytes.hpp"

namespace joa::formats {

namespace {
bool fail(std::string* err, const std::string& m) {
    if (err) *err = m;
    return false;
}
constexpr std::int32_t kMaxCount = 1 << 22;  // sanity bound against garbage
}  // namespace

std::optional<PfdMesh> parse_pfd(std::span<const std::uint8_t> bytes, std::string* err) {
    ByteReader r(bytes);
    PfdMesh m;
    // min_size = smallest record: a count the remaining bytes cannot hold is rejected before anything
    // is allocated for it (12 bytes could otherwise ask for 2^22 polys, each with its own vector).
    auto count = [&](const char* what, std::size_t min_size) -> std::int32_t {
        if (!r.ok()) return 0;  // keep the first error
        const std::int32_t n = r.i32();
        if (!r.ok() || n < 0 || n > kMaxCount || std::uint64_t(n) * min_size > r.remaining()) {
            fail(err, std::string("bad ") + what + " count");
            r.fail();
            return 0;
        }
        return n;
    };

    const std::int32_t nm = count("model", 2);
    for (std::int32_t i = 0; i < nm && r.ok(); ++i) {
        const std::uint16_t len = r.u16();  // read_block: u16 length + bytes (always 0x80)
        m.models.push_back(ByteReader::cstr(r.bytes(len)));
    }
    const std::int32_t np = count("point", 12);
    m.points.resize(std::size_t(np));
    for (auto& p : m.points) { p.x = r.i32(); p.y = r.i32(); p.z = r.i32(); }
    const std::int32_t npoly = count("poly", 48);  // type, bbox[6], model, nv, >= 3 verts
    m.polys.resize(std::size_t(npoly));
    for (auto& p : m.polys) {
        if (!r.ok()) break;
        p.type = r.i32();
        for (auto& b : p.bbox) b = r.i32();
        p.model = r.i32();
        const std::int32_t nv = r.i32();
        if (nv < 3 || nv > 64) { fail(err, "bad vertex count"); return std::nullopt; }
        p.verts.resize(std::size_t(nv));
        for (auto& v : p.verts) {
            v = r.i32() - 1;
            if (v < 0 || v >= np) { fail(err, "vertex index out of range"); return std::nullopt; }
        }
        if (p.model < 0 || p.model > nm) { fail(err, "model index out of range"); return std::nullopt; }
    }
    const std::int32_t nl = count("link", 16);
    m.links.resize(std::size_t(nl));
    for (auto& l : m.links) {
        l.poly_a = r.i32(); l.edge_a = r.i32(); l.poly_b = r.i32(); l.edge_b = r.i32();
        if (!r.ok()) break;
        auto edge_ok = [&](std::int32_t poly, std::int32_t e, bool allow0) {
            if (poly < 1 || poly > npoly) return false;
            return (allow0 && e == 0) || (e >= 1 && e <= std::int32_t(m.polys[poly - 1].verts.size()));
        };
        if (!edge_ok(l.poly_a, l.edge_a, false) || !edge_ok(l.poly_b, l.edge_b, true)) {
            fail(err, "link out of range");
            return std::nullopt;
        }
    }
    if (r.i32() != 0) {
        PfdGrid g;
        g.x0 = r.i32(); g.y0 = r.i32(); g.cols = r.i32(); g.rows = r.i32(); g.cell_w = r.i32(); g.cell_h = r.i32();
        if (!r.ok() || g.cols <= 0 || g.rows <= 0 || g.cols > 4096 || g.rows > 4096 || g.cell_w <= 0 || g.cell_h <= 0) {
            fail(err, "bad grid header");
            return std::nullopt;
        }
        if (std::uint64_t(g.cols) * g.rows * 4 > r.remaining()) { fail(err, "truncated grid"); return std::nullopt; }
        g.h.resize(std::size_t(g.cols) * g.rows);
        for (auto& v : g.h) v = std::int16_t(r.i32());  // stored as i32, engine keeps i16 (plasma array)
        const std::int32_t nsub = count("sub-grid", 24);
        for (std::int32_t i = 0; i < nsub && r.ok(); ++i) {
            r.i32(); r.i32();
            const std::int32_t c = r.i32(), rr = r.i32();
            r.i32(); r.i32();
            if (c < 0 || rr < 0 || std::int64_t(c) * rr > kMaxCount) { fail(err, "bad sub-grid"); return std::nullopt; }
            r.skip(std::size_t(c) * rr * 4);
        }
        m.grid = std::move(g);
    }
    if (!r.ok()) { if (err && err->empty()) *err = "truncated"; return std::nullopt; }
    if (!r.at_end()) { fail(err, "trailing bytes"); return std::nullopt; }
    return m;
}

}  // namespace joa::formats
