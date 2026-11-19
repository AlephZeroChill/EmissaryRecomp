#pragma once

#include "common.hpp"
#include "texture.hpp"
#include "../gpu/gpu.hpp"

namespace aurora::gfx {
static constexpr SDL_GPUTextureFormat to_sdl_format(u32 gxFormat) {
  switch (gxFormat) {
  case GX_TF_C4:
  case GX_TF_C8:
  case GX_TF_C14X2:
    return SDL_GPU_TEXTUREFORMAT_R16_INT;
  default:
    return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  }
}

struct ConvertedTexture {
  SDL_GPUTextureFormat format;
  uint32_t width;
  uint32_t height;
  uint32_t mips = 1;
  ByteBuffer data;
  bool hasArbitraryMips = false;
};

ConvertedTexture convert_texture(u32 format, uint32_t width, uint32_t height, uint32_t mips, ArrayRef<uint8_t> data);
ConvertedTexture convert_texture_palette(u32 textureFormat, uint32_t width, uint32_t height, uint32_t mips,
                                         ArrayRef<uint8_t> textureData, GXTlutFmt tlutFormat, uint16_t tlutEntries,
                                         ArrayRef<uint8_t> tlutData);
ConvertedTexture convert_tlut(u32 format, uint32_t width, ArrayRef<uint8_t> data);
GXTexFmt tlut_texture_format(GXTlutFmt format) noexcept;
} // namespace aurora::gfx
