// SMAA area/search lookup textures (third_party/smaa, MIT) as RGBA8 Unorm textures for both backends.
#include "engine/post_frame.hpp"

#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"

namespace joa::engine {
namespace {

TextureData expand(const unsigned char* src, int w, int h, int channels) {
    TextureData t{w, h, PixelFormat::Rgba8Unorm, std::vector<std::uint8_t>(std::size_t(w) * h * 4, 0), false, false, {}};
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i)
        for (int c = 0; c < channels; ++c) t.pixels[i * 4 + c] = src[i * channels + c];
    return t;
}

}  // namespace

TextureData smaa_area_texture() { return expand(areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT, 2); }
TextureData smaa_search_texture() { return expand(searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, 1); }

}  // namespace joa::engine
