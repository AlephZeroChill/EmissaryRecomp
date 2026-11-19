#include "common.hpp"

#include "../internal.hpp"
#include "../gpu/gpu.hpp"
#include "aurora/aurora.h"
#include "texture.hpp"
#include "texture_convert.hpp"
#include "../gx/gx_fmt.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>

#include <fmt/format.h>
#include <aurora/profiling.hpp>

namespace aurora::gfx {

namespace {
Module Log("aurora::gfx");

constexpr u32 div_ceil(u32 value, u32 divisor) noexcept { return (value + divisor - 1) / divisor; }

uint32_t clamp_mip_count(uint32_t width, uint32_t height, uint32_t mips, const char* label) noexcept {
  const uint32_t maxMips = max_texture_mip_count(width, height);
  const uint32_t safeMips = std::clamp(mips, 1u, maxMips);
  if (safeMips != mips) {
    Log.warn("{}: clamping mip count {} to {} for {}x{} texture", label, mips, safeMips, width, height);
  }
  return safeMips;
}

Extent3D physical_size(Extent3D size, TextureFormatInfo info) {
  const uint32_t width = ((size.width + info.blockWidth - 1) / info.blockWidth) * info.blockWidth;
  const uint32_t height = ((size.height + info.blockHeight - 1) / info.blockHeight) * info.blockHeight;
  return {.width = width, .height = height, .depthOrArrayLayers = size.depthOrArrayLayers};
}
} // namespace

TextureFormatInfo format_info(SDL_GPUTextureFormat format) noexcept {
  switch (format) {
    DEFAULT_FATAL("unimplemented texture format {}", static_cast<int>(format));
  case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
    return {1, 1, 1, false};
  case SDL_GPU_TEXTUREFORMAT_R8G8_UNORM:
  case SDL_GPU_TEXTUREFORMAT_R16_INT:
    return {1, 1, 2, false};
  case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
  case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
  case SDL_GPU_TEXTUREFORMAT_R32_FLOAT:
    return {1, 1, 4, false};
  case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM:
    return {4, 4, 8, true};
  case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM:
    return {4, 4, 16, true};
  }
}

uint64_t calc_texture_size(SDL_GPUTextureFormat format, u32 width, u32 height, u32 mips) noexcept {
  const auto info = format_info(format);
  uint64_t total = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const uint32_t mipWidth = std::max(width >> mip, 1u);
    const uint32_t mipHeight = std::max(height >> mip, 1u);
    const uint64_t widthBlocks = div_ceil(mipWidth, info.blockWidth);
    const uint64_t heightBlocks = div_ceil(mipHeight, info.blockHeight);
    const uint64_t mipBytes = widthBlocks * heightBlocks * info.blockSize;
    total += mipBytes;
  }
  return total;
}

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 format, ArrayRef<uint8_t> data,
                                    bool tlut, const char* label) noexcept {
  ZoneScoped;

  mips = clamp_mip_count(width, height, mips, label);
  auto handle = new_dynamic_texture_2d(width, height, mips, format, label);
  auto& ref = *handle;
  static const bool digests = std::getenv("AURORA_DRAW_DUMP") != nullptr;
  if (digests && data.data() != nullptr && data.size() != 0 && data.size() != UINT32_MAX) UNLIKELY {
    ref.debugDigest = xxh3_hash_s(data.data(), data.size());
  }

  ConvertedTexture converted;
  if (ref.gxFormat != InvalidTextureFormat) {
    if (tlut) {
      CHECK(ref.size.height == 1, "new_static_texture_2d[{}]: expected tlut height 1, got {}", label, ref.size.height);
      CHECK(ref.mipCount == 1, "new_static_texture_2d[{}]: expected tlut mipCount 1, got {}", label, ref.mipCount);
      converted = convert_tlut(ref.gxFormat, ref.size.width, data);
    } else {
      converted = convert_texture(ref.gxFormat, ref.size.width, ref.size.height, ref.mipCount, data);
    }
    if (!converted.data.empty()) {
      data = converted.data;
      ref.hasArbitraryMips = converted.hasArbitraryMips;
    }
  }

  if (!ref.texture) {
    return handle;
  }
  uint32_t offset = 0;
  for (uint32_t mip = 0; mip < mips; ++mip) {
    const Extent3D mipSize{
        .width = std::max(ref.size.width >> mip, 1u),
        .height = std::max(ref.size.height >> mip, 1u),
        .depthOrArrayLayers = ref.size.depthOrArrayLayers,
    };
    const auto info = format_info(ref.format);
    const auto physicalSize = physical_size(mipSize, info);
    const uint32_t widthBlocks = physicalSize.width / info.blockWidth;
    const uint32_t heightBlocks = physicalSize.height / info.blockHeight;
    const uint32_t bytesPerRow = widthBlocks * info.blockSize;
    const uint32_t dataSize = bytesPerRow * heightBlocks * mipSize.depthOrArrayLayers;
    if (offset > data.size() || dataSize > data.size() - offset) {
      Log.warn("new_static_texture_2d[{}]: stopping at mip {}: expected at least {} bytes, got {}", label, mip,
               static_cast<size_t>(offset) + dataSize, data.size());
      return handle;
    }
    if constexpr (UseTextureBuffer) {
      const auto range = push_texture_data(data.data() + offset, dataSize, bytesPerRow, heightBlocks);
      g_textureUploads.push_back(TextureUpload{
          .offset = range.offset,
          .pixelsPerRow = physicalSize.width,
          .rowsPerImage = physicalSize.height,
          .texture = ref.texture,
          .mip = mip,
          .layer = 0,
          .width = physicalSize.width,
          .height = physicalSize.height,
      });
    } else {
      gpu::write_texture(*ref.texture, 0, 0, physicalSize.width, physicalSize.height, mip, 0, data.data() + offset,
                         bytesPerRow, heightBlocks);
    }
    offset += dataSize;
  }
  if (data.size() != UINT32_MAX && offset < data.size()) {
    Log.warn("new_static_texture_2d[{}]: texture used {} bytes, but given {} bytes", label, offset, data.size());
  }
  return handle;
}

TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  ZoneScopedS(3);
  mips = clamp_mip_count(width, height, mips, label);
  const auto format = to_sdl_format(gxFormat);
  const Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  auto texture = gpu::create_texture_2d(width, height, format, SDL_GPU_TEXTUREUSAGE_SAMPLER, label, mips);
  return std::make_shared<TextureRef>(std::move(texture), size, format, mips, gxFormat);
}

TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  ZoneScoped;

  const auto format = gpu::g_graphicsConfig.surfaceFormat;
  const Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  auto texture = gpu::create_texture_2d(width, height, format,
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, label);
  return std::make_shared<TextureRef>(std::move(texture), size, format, 1, gxFormat);
}

TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  ZoneScoped;

  const auto format = to_sdl_format(gxFormat);
  const Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  auto texture = gpu::create_texture_2d(width, height, format,
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, label);
  return std::make_shared<TextureRef>(std::move(texture), size, format, 1, gxFormat);
}

void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {
  ZoneScoped;

  ConvertedTexture converted;
  if (ref.gxFormat != InvalidTextureFormat) {
    converted = convert_texture(ref.gxFormat, ref.size.width, ref.size.height, ref.mipCount, data);
    ref.hasArbitraryMips = converted.hasArbitraryMips;
    if (!converted.data.empty()) {
      data = converted.data;
    }
  }

  if (!ref.texture) {
    return;
  }
  uint32_t offset = 0;
  for (uint32_t mip = 0; mip < ref.mipCount; ++mip) {
    const Extent3D mipSize{
        .width = std::max(ref.size.width >> mip, 1u),
        .height = std::max(ref.size.height >> mip, 1u),
        .depthOrArrayLayers = ref.size.depthOrArrayLayers,
    };
    const auto info = format_info(ref.format);
    const auto physicalSize = physical_size(mipSize, info);
    const uint32_t widthBlocks = physicalSize.width / info.blockWidth;
    const uint32_t heightBlocks = physicalSize.height / info.blockHeight;
    const uint32_t bytesPerRow = widthBlocks * info.blockSize;
    const uint32_t dataSize = bytesPerRow * heightBlocks * mipSize.depthOrArrayLayers;
    if (offset > data.size() || dataSize > data.size() - offset) {
      Log.warn("write_texture: stopping at mip {}: expected at least {} bytes, got {}", mip,
               static_cast<size_t>(offset) + dataSize, data.size());
      return;
    }
    if constexpr (UseTextureBuffer) {
      const auto range = push_texture_data(data.data() + offset, dataSize, bytesPerRow, heightBlocks);
      g_textureUploads.push_back(TextureUpload{
          .offset = range.offset,
          .pixelsPerRow = physicalSize.width,
          .rowsPerImage = physicalSize.height,
          .texture = ref.texture,
          .mip = mip,
          .layer = 0,
          .width = physicalSize.width,
          .height = physicalSize.height,
      });
    } else {
      gpu::write_texture(*ref.texture, 0, 0, physicalSize.width, physicalSize.height, mip, 0, data.data() + offset,
                         bytesPerRow, heightBlocks);
    }
    offset += dataSize;
  }
  if (data.size() != UINT32_MAX && offset < data.size()) {
    Log.warn("write_texture: texture used {} bytes, but given {} bytes", offset, data.size());
  }
}
} // namespace aurora::gfx
