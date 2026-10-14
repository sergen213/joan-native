// HD texture packs (engine/hd_pack.hpp, AssetCache overrides): synthetic packs in a temp dir; no GPU.
// The optional last part uses JOA_GAME_DIR plus a local HD Update extraction when both exist.
#include "check.hpp"
#include "engine/assets_cache.hpp"
#include "engine/hd_pack.hpp"
#include "engine/png.hpp"

#include <zlib.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <unistd.h>

using namespace joa::engine;
namespace fs = std::filesystem;

namespace {

struct StubRenderer final : IRenderer {
    std::uint32_t next = 1;
    std::map<std::uint32_t, TextureData> textures;  // pixels kept for checks
    std::map<std::uint32_t, MaterialMaps> maps;
    const char* name() const override { return "stub"; }
    MeshHandle create_mesh(const MeshData&) override { return MeshHandle{next++}; }
    void destroy_mesh(MeshHandle) override {}
    TextureHandle create_texture(const TextureData& t) override {
        textures[next] = t;
        return TextureHandle{next++};
    }
    void destroy_texture(TextureHandle) override {}
    void set_material_maps(TextureHandle albedo, const MaterialMaps& m) override { maps[albedo.id] = m; }
    void begin_frame(glm::ivec2) override {}
    void submit(const FrameData&) override {}
    void end_frame() override {}
    Image read_back() override { return {}; }
};

void write_bytes(const fs::path& p, const std::vector<std::uint8_t>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

// A8R8G8B8 DDS, size x size, full mip chain when mips, every level filled with bgra.
void write_dds(const fs::path& p, std::uint32_t size, bool mips, std::array<std::uint8_t, 4> bgra) {
    std::uint32_t hdr[31] = {};
    std::uint32_t levels = 1;
    if (mips)
        for (std::uint32_t s = size; s > 1; s /= 2) ++levels;
    hdr[0] = 124, hdr[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000, hdr[2] = size, hdr[3] = size, hdr[6] = levels;
    hdr[18] = 32, hdr[19] = 0x41, hdr[21] = 32, hdr[22] = 0x00ff0000, hdr[23] = 0x0000ff00, hdr[24] = 0x000000ff, hdr[25] = 0xff000000;
    std::vector<std::uint8_t> b{'D', 'D', 'S', ' '};
    b.insert(b.end(), reinterpret_cast<std::uint8_t*>(hdr), reinterpret_cast<std::uint8_t*>(hdr) + sizeof hdr);
    for (std::uint32_t l = 0, s = size; l < levels; ++l, s = std::max(1u, s / 2))
        for (std::uint32_t i = 0; i < s * s; ++i) b.insert(b.end(), bgra.begin(), bgra.end());
    write_bytes(p, b);
}

void write_rgba_png(const fs::path& p, int size, std::array<std::uint8_t, 4> rgba) {
    fs::create_directories(p.parent_path());
    std::vector<std::uint8_t> px;
    for (int i = 0; i < size * size; ++i) px.insert(px.end(), rgba.begin(), rgba.end());
    CHECK(write_png(p.string(), size, size, px));
}

// PNG from already-filtered scanlines.
std::vector<std::uint8_t> make_png(int w, int h, int depth, int type, const std::vector<std::uint8_t>& filtered,
                                   const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& extra = {}) {
    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    const auto be32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int s = 24; s >= 0; s -= 8) v.push_back(std::uint8_t(x >> s));
    };
    const auto chunk = [&](const std::string& t, const std::vector<std::uint8_t>& d) {
        be32(out, std::uint32_t(d.size()));
        const std::size_t start = out.size();
        out.insert(out.end(), t.begin(), t.end());
        out.insert(out.end(), d.begin(), d.end());
        be32(out, std::uint32_t(crc32(0, out.data() + start, uInt(out.size() - start))));
    };
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, std::uint32_t(w));
    be32(ihdr, std::uint32_t(h));
    ihdr.insert(ihdr.end(), {std::uint8_t(depth), std::uint8_t(type), 0, 0, 0});
    chunk("IHDR", ihdr);
    for (const auto& [t, d] : extra) chunk(t, d);
    uLongf n = compressBound(uLong(filtered.size()));
    std::vector<std::uint8_t> z(n);
    compress2(z.data(), &n, filtered.data(), uLong(filtered.size()), 9);
    z.resize(n);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

void png_decode_checks() {
    std::string err;
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    // 8-bit grey 5x4, rows filtered Sub, Up, Average, Paeth (forward filters, bpp = 1).
    const int W = 5, H = 4;
    std::vector<std::uint8_t> img(W * H), filtered;
    for (int i = 0; i < W * H; ++i) img[i] = std::uint8_t(i * 37 + 11);
    for (int y = 0; y < H; ++y) {
        const int f = y + 1;
        filtered.push_back(std::uint8_t(f));
        for (int x = 0; x < W; ++x) {
            const int a = x ? img[y * W + x - 1] : 0, b = y ? img[(y - 1) * W + x] : 0, c = x && y ? img[(y - 1) * W + x - 1] : 0;
            const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            const int pred = f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : (pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
            filtered.push_back(std::uint8_t(img[y * W + x] - pred));
        }
    }
    CHECK(decode_png(make_png(W, H, 8, 0, filtered), w, h, px, err));
    CHECK(w == W && h == H && px.size() == std::size_t(W * H * 4));
    bool grey_ok = px.size() == std::size_t(W * H * 4);
    for (int i = 0; grey_ok && i < W * H; ++i) grey_ok = px[i * 4] == img[i] && px[i * 4 + 2] == img[i] && px[i * 4 + 3] == 255;
    CHECK(grey_ok);

    // 2-bit palette 3x1 with tRNS: indices 2,0,1 -> 0b10'00'01'00.
    CHECK(decode_png(make_png(3, 1, 2, 3, {0, 0b10000100}, {{"PLTE", {10, 20, 30, 40, 50, 60, 70, 80, 90}}, {"tRNS", {0, 128}}}), w, h, px, err));
    CHECK(w == 3 && px.size() == 12);
    if (px.size() == 12) {
        CHECK(px[0] == 70 && px[1] == 80 && px[2] == 90 && px[3] == 255);
        CHECK(px[4] == 10 && px[7] == 0);
        CHECK(px[8] == 40 && px[11] == 128);
    }

    // 16-bit RGB 1x1 keeps the high bytes.
    CHECK(decode_png(make_png(1, 1, 16, 2, {0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc}), w, h, px, err));
    CHECK(px.size() == 4 && px[0] == 0x12 && px[1] == 0x56 && px[2] == 0x9a && px[3] == 255);

    // write_png round trip, RGBA.
    std::vector<std::uint8_t> rgba(7 * 3 * 4);
    for (std::size_t i = 0; i < rgba.size(); ++i) rgba[i] = std::uint8_t(i * 13);
    const fs::path tmp = fs::temp_directory_path() / ("joa_hd_png_" + std::to_string(getpid()) + ".png");
    CHECK(write_png(tmp.string(), 7, 3, rgba));
    joa::formats::DdsTexture t;
    CHECK(load_image_file(tmp, t, err) && t.images.size() == 1 && t.images[0].width == 7 && t.images[0].rgba == rgba);
    fs::remove(tmp);

    CHECK(!decode_png(std::vector<std::uint8_t>{1, 2, 3}, w, h, px, err));
    std::vector<std::uint8_t> laced = make_png(1, 1, 8, 0, {0, 1});
    laced[8 + 8 + 12] = 1;  // interlace byte (CRC now wrong too; rejected either way)
    CHECK(!decode_png(laced, w, h, px, err));
}

}  // namespace

int main() {
    png_decode_checks();

    const fs::path root = fs::temp_directory_path() / ("joa_hd_test_" + std::to_string(getpid()));
    fs::remove_all(root);
    const fs::path game = root / "game", data = root / "data", pack_a = data / "joan-native/hd/packA", pack_b = root / "packB";
    // Original data (mixed case like the real install; one spelling per directory, as on disk).
    write_dds(game / "TEXTURE/Item/Church.DDS", 4, true, {0, 0, 255, 255});      // red
    write_dds(game / "TEXTURE/unit/Knight.dds", 4, false, {255, 0, 0, 255});      // blue
    write_dds(game / "TEXTURE/TERRAIN/grass_t.dds", 4, false, {0, 255, 0, 255});  // green
    // Pack A (by name under $XDG_DATA_HOME): church as PNG with alpha + its normal map.
    write_rgba_png(pack_a / "Texture/ITEM/church.png", 16, {1, 2, 3, 200});
    write_rgba_png(pack_a / "Texture/ITEM/church_n.png", 16, {128, 128, 255, 255});
    // Pack B (absolute): a church that must lose to A, a roughness map that must not attach (A won the
    // albedo), a terrain override as DDS with an AO map, and a normal map for an un-overridden texture.
    write_dds(pack_b / "texture/item/CHURCH.dds", 8, true, {9, 9, 9, 255});
    write_rgba_png(pack_b / "texture/item/church_r.png", 8, {5, 5, 5, 255});
    write_dds(pack_b / "texture/terrain/Grass_T.DDS", 8, true, {7, 8, 9, 255});
    write_rgba_png(pack_b / "TEXTURE/terrain/grass_t_ao.png", 8, {200, 200, 200, 255});
    write_rgba_png(pack_b / "texture/unit/knight_n.png", 4, {128, 128, 255, 255});
    write_rgba_png(pack_b / "model/ignored.png", 4, {0, 0, 0, 255});  // outside texture trees

    setenv("XDG_DATA_HOME", data.c_str(), 1);
    CHECK(hd_pack_home() == data / "joan-native/hd");
    const auto packs = resolve_hd_packs({"packA", pack_b.string(), "missing", (root / "nope").string()});
    CHECK(packs.size() == 2 && packs[0] == pack_a && packs[1] == pack_b);

    {
        const HdPacks hd(packs);
        CHECK(hd.files() == 7);
        const HdOverride church = hd.find("TEXTURE\\ITEM\\CHURCH.dds");
        CHECK(church.albedo == pack_a / "Texture/ITEM/church.png");
        CHECK(church.normal == pack_a / "Texture/ITEM/church_n.png");
        CHECK(church.roughness.empty() && church.metal.empty() && church.ao.empty());
        const HdOverride grass = hd.find("texture/terrain/grass_t.tga");
        CHECK(grass.albedo == pack_b / "texture/terrain/Grass_T.DDS");
        CHECK(grass.ao == pack_b / "TEXTURE/terrain/grass_t_ao.png");
        const HdOverride knight = hd.find("texture/unit/knight.dds");
        CHECK(knight.albedo.empty() && knight.normal == pack_b / "texture/unit/knight_n.png");
        const HdOverride none = hd.find("texture/misc/sky.dds");
        CHECK(none.albedo.empty() && none.normal.empty() && none.ao.empty());
        CHECK(hd.find("model/ignored").albedo.empty());
        CHECK(HdPacks{}.find("texture/item/church.dds").albedo.empty());
    }

    CHECK(material_class("texture/terrain/sand_l01.dds") == MaterialClass::Terrain);
    CHECK(material_class("TEXTURE\\UNIT\\Joan.dds") == MaterialClass::Character);
    CHECK(material_class("texture/particle/fire.dds") == MaterialClass::Particle);
    CHECK(material_class("texture/item/sw_crecy.dds") == MaterialClass::Item);
    CHECK(material_class("texture/item/LongBow.dds") == MaterialClass::Item);
    CHECK(material_class("texture/item/orlean-sowrd.dds") == MaterialClass::Item);
    CHECK(material_class("texture/item/shieldfrench.dds") == MaterialClass::Item);
    CHECK(material_class("texture/item/bowl.dds") == MaterialClass::Building);
    CHECK(material_class("texture/item/pinetree01.dds") == MaterialClass::Vegetation);
    CHECK(material_class("texture/item/bushes02.dds") == MaterialClass::Vegetation);
    CHECK(material_class("texture/item/Church.dds") == MaterialClass::Building);
    CHECK(material_class("texture/misc/skybox1.dds") == MaterialClass::Generic);
    CHECK(material_class("int8x6/menu1024.dds") == MaterialClass::Generic);

    const joa::formats::Vfs vfs(game);
    {  // No packs: originals, class still registered.
        StubRenderer r;
        AssetCache assets(vfs, r);
        const TextureInfo* t = assets.texture("church.tga");
        CHECK(t && r.textures.at(t->handle.id).width == 4 && r.textures.at(t->handle.id).mip_pixels.size() == 2);
        CHECK(t && !t->has_alpha);
        CHECK(t && r.maps.count(t->handle.id) && r.maps.at(t->handle.id).cls == MaterialClass::Building && !r.maps.at(t->handle.id).normal);
        CHECK(assets.hd_overrides() == 0);
        CHECK(assets.texture_pixels("church").width == 4);
    }
    {
        StubRenderer r;
        AssetCache assets(vfs, r, packs);
        const TextureInfo* church = assets.texture("Church.tga");
        CHECK(church != nullptr);
        if (church) {
            const TextureData& td = r.textures.at(church->handle.id);
            CHECK(td.width == 16 && td.height == 16 && td.mip_pixels.empty() && td.format == PixelFormat::Rgba8Srgb && td.repeat);
            CHECK(td.pixels.size() == 16 * 16 * 4 && td.pixels[0] == 1 && td.pixels[3] == 200);
            CHECK(church->has_alpha);
            const MaterialMaps& m = r.maps.at(church->handle.id);
            CHECK(m.cls == MaterialClass::Building && m.normal && !m.roughness && !m.metal && !m.ao);
            if (m.normal) {
                const TextureData& n = r.textures.at(m.normal.id);
                CHECK(n.width == 16 && n.format == PixelFormat::Rgba8Unorm && n.repeat && n.mipmaps);
            }
        }
        const TextureInfo* grass = assets.texture("grass_t", false);
        CHECK(grass != nullptr);
        if (grass) {
            const TextureData& td = r.textures.at(grass->handle.id);
            CHECK(td.width == 8 && td.mip_pixels.size() == 3 && !td.repeat && td.pixels[0] == 9 && td.pixels[2] == 7);
            const MaterialMaps& m = r.maps.at(grass->handle.id);
            CHECK(m.cls == MaterialClass::Terrain && m.ao && !m.normal);
            if (m.ao) CHECK(!r.textures.at(m.ao.id).repeat && r.textures.at(m.ao.id).format == PixelFormat::Rgba8Unorm);
        }
        const TextureInfo* knight = assets.texture("KNIGHT");
        CHECK(knight && r.textures.at(knight->handle.id).width == 4 && r.textures.at(knight->handle.id).pixels[2] == 255);
        CHECK(knight && r.maps.at(knight->handle.id).cls == MaterialClass::Character && r.maps.at(knight->handle.id).normal);
        CHECK(assets.texture("nothing") == nullptr);
        CHECK(assets.hd_overrides() == 2);
        const auto px = assets.texture_pixels("church");
        CHECK(px.width == 16 && px.rgba.size() == 16 * 16 * 4 && px.rgba[1] == 2);
        CHECK(assets.texture_pixels("knight").width == 4);
    }
    // Broken override file: falls back to the original.
    write_bytes(pack_a / "texture/unit/knight.png", {1, 2, 3});
    {
        StubRenderer r;
        AssetCache assets(vfs, r, packs);
        const TextureInfo* knight = assets.texture("knight");
        CHECK(knight && r.textures.at(knight->handle.id).width == 4 && assets.hd_overrides() == 0);
    }
    fs::remove_all(root);

    // Real data: the community HD Update extracted locally (never committed) overrides church.dds.
    const char* dir = std::getenv("JOA_GAME_DIR");
    const fs::path hd_update = dir ? fs::path(dir) / "work/third_party_ref/hd_update/x" : fs::path{};
    const joa::formats::Vfs real(dir ? dir : ".");
    if (dir && real.exists("texture/item/church.dds") && fs::is_directory(hd_update)) {
        StubRenderer r0, r1;
        AssetCache plain(real, r0), hd(real, r1, {hd_update});
        const TextureInfo *a = plain.texture("church"), *b = hd.texture("church");
        CHECK(a && b && r1.textures.at(b->handle.id).width > r0.textures.at(a->handle.id).width);
        CHECK(hd.hd_overrides() == 1);
    } else {
        std::fprintf(stderr, "SKIP (part): no JOA_GAME_DIR data or no work/third_party_ref/hd_update/x\n");
    }
    return test_result();
}
