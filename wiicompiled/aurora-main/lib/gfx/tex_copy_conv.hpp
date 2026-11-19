#pragma once

#include "common.hpp"

#include <dolphin/gx/GXEnum.h>

namespace aurora::gfx::tex_copy_conv {

enum class SampleFilter : uint8_t {
  Nearest,
  Linear,
};

struct ConvRequest {
  GXTexFmt fmt;
  gpu::Texture src;   // Resolved EFB / offscreen color/depth
  Range uniformRange; // UV transform uniform (offset + scale)
  TextureHandle dst;  // Destination texture
  SampleFilter sampleFilter = SampleFilter::Nearest;
  bool forceOpaqueAlpha = false;
};

bool needs_conversion(GXTexFmt fmt);

void initialize();
void shutdown();
// `uniforms` is the base of the CPU uniform arena req.uniformRange indexes into.
void run(gpu::Encoder& enc, const uint8_t* uniforms, const ConvRequest& req);
void blit(gpu::Encoder& enc, const uint8_t* uniforms, const ConvRequest& req);

} // namespace aurora::gfx::tex_copy_conv
