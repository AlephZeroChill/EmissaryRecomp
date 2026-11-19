#include "tex_copy_conv.hpp"
#include "tex_copy_format_contract.hpp"

#include "../internal.hpp"
#include "../gx/gx.hpp"
#include "../gpu/gpu.hpp"
#include "../gpu/shader.hpp"
#include "texture.hpp"
#include "../gx/gx_fmt.hpp"

#include <absl/container/flat_hash_map.h>

#include <array>

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace aurora::gfx::tex_copy_conv {
static Module Log("aurora::gfx::tex_copy_conv");

static constexpr std::string_view UVTransformMembers =
    "vec2 offset; vec2 scale; vec4 copy_filter; vec4 flags;"sv;

static constexpr std::string_view VertexBody = R"(
  vec2 p = vidx == 0u ? vec2(-1.0, 1.0) : (vidx == 1u ? vec2(-1.0, -3.0) : vec2(3.0, 1.0));
  vec2 uv = vidx == 0u ? vec2(0.0, 0.0) : (vidx == 1u ? vec2(0.0, 2.0) : vec2(2.0, 0.0));
  out_pos = vec4(p, 0.0, 1.0);
  vout.uv = uv * uv_xf.scale + uv_xf.offset;)"sv;

static constexpr std::string_view ColorHelpers = R"(
float intensity(vec3 rgb) {
    // ITU-R BT.601 luma coefficients
    return dot(rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0;
}

float quantize4(float v) {
    return floor(v * 16.0) / 15.0;
}

vec4 apply_opaque_alpha(vec4 c, vec4 flags) {
    if (flags.x != 0.0) {
        return vec4(c.rgb, 1.0);
    }
    return c;
}

vec2 clamp_copy_uv(vec2 uv, vec4 flags) {
    return vec2(uv.x, clamp(uv.y, flags.z, flags.w));
}
)"sv;

// sample_copy(vin.uv) -> `copied`
static constexpr std::string_view ColorSampleBody = R"(
  vec4 copy_current = SAMPLE(src, clamp_copy_uv(vin.uv, uv_xf.flags));
  vec4 copied = copy_current;
  if (uv_xf.copy_filter.w != 0.0) {
    vec2 tex_size = vec2(DIMS(src));
    vec2 pixel_size = vec2(1.0, 1.0) / tex_size;
    float row_stride = max(uv_xf.flags.y, 1.0);
    vec4 copy_prev = SAMPLE(src, clamp_copy_uv(vin.uv - vec2(0.0, pixel_size.y * row_stride), uv_xf.flags));
    vec4 copy_next = SAMPLE(src, clamp_copy_uv(vin.uv + vec2(0.0, pixel_size.y * row_stride), uv_xf.flags));
    vec3 prev_rgb = floor(copy_prev.rgb * 255.0 + vec3(0.5));
    vec3 current_rgb = floor(copy_current.rgb * 255.0 + vec3(0.5));
    vec3 next_rgb = floor(copy_next.rgb * 255.0 + vec3(0.5));
    vec3 filtered_rgb = min(
        floor((prev_rgb * uv_xf.copy_filter.x +
               current_rgb * uv_xf.copy_filter.y +
               next_rgb * uv_xf.copy_filter.z) / 64.0),
        vec3(255.0));
    copied = vec4(filtered_rgb / 255.0, copy_current.a);
  }
  copied = apply_opaque_alpha(copied, uv_xf.flags);
)"sv;

static const std::string DepthHelpers = (gx::UseReversedZ ? R"(
uint gx_z24_from_depth(float depth) {
    return min(uint(clamp(1.0 - depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}
)"s
                                                          : R"(
uint gx_z24_from_depth(float depth) {
    return min(uint(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}
)"s) + R"(
uvec3 gx_depth_bytes(uint z24) {
    return uvec3((z24 >> 16u) & 0xffu, (z24 >> 8u) & 0xffu, z24 & 0xffu);
}

ivec2 clamp_tex_coord(ivec2 unclamped_coord, ivec2 tex_size) {
    return clamp(unclamped_coord, ivec2(0), tex_size - ivec2(1));
}

ivec2 clamp_copy_coord(ivec2 coord, ivec2 tex_size, vec4 flags) {
    int top = int(floor(flags.z * float(tex_size.y)));
    int bottom = int(floor(flags.w * float(tex_size.y)));
    return ivec2(coord.x, clamp(coord.y, top, bottom));
}
)"s;

// sample_depth_copy(vin.uv) -> `depth_bytes`
static constexpr std::string_view DepthSampleBody = R"(
  ivec2 tex_size = ivec2(DIMS(src));
  ivec2 current_coord = clamp_copy_coord(ivec2(floor(vin.uv * vec2(tex_size))), tex_size, uv_xf.flags);
  uvec3 current = gx_depth_bytes(gx_z24_from_depth(LOAD_DEPTH(src, clamp_tex_coord(current_coord, tex_size))));
  uvec4 depth_bytes = uvec4(current, 255u);
  if (uv_xf.copy_filter.w != 0.0) {
    // GX applies its vertical copy filter to depth copies too, filtering the high/middle/low Z bytes
    // independently before the destination format picks bytes. Alpha is unfiltered and 255.
    int row_stride = max(int(round_even(uv_xf.flags.y)), 1);
    ivec2 prev_coord = clamp_copy_coord(current_coord - ivec2(0, row_stride), tex_size, uv_xf.flags);
    ivec2 next_coord = clamp_copy_coord(current_coord + ivec2(0, row_stride), tex_size, uv_xf.flags);
    uvec3 prev = gx_depth_bytes(gx_z24_from_depth(LOAD_DEPTH(src, clamp_tex_coord(prev_coord, tex_size))));
    uvec3 next = gx_depth_bytes(gx_z24_from_depth(LOAD_DEPTH(src, clamp_tex_coord(next_coord, tex_size))));
    uvec3 coefficients = uvec3(uv_xf.copy_filter.xyz);
    uvec3 combined = prev * coefficients.x + current * coefficients.y + next * coefficients.z;
    uvec3 filtered = combined >> uvec3(6u);

    // The copy-filter accumulator wraps to nine bits when coefficients can
    // produce values at or above 512, before saturating to an eight-bit byte.
    if (coefficients.x + coefficients.y + coefficients.z >= 128u) {
      filtered = filtered & uvec3(0x1ffu);
    }
    filtered = min(filtered, uvec3(255u));
    depth_bytes = uvec4(filtered, 255u);
  }
)"sv;

// Passthrough blit (for scaling)
static constexpr std::string_view FragPassthrough = R"(
  out_color = copied;)"sv;

// GX_TF_I4: 4-bit intensity -> R8Unorm (quantized)
static constexpr std::string_view FragI4 = R"(
  vec3 rgb = copied.rgb;
  float i = quantize4(intensity(rgb));
  out_color = vec4(i, i, i, i);)"sv;

// GX_TF_I8: 8-bit intensity -> R8Unorm
static constexpr std::string_view FragI8 = R"(
  vec3 rgb = copied.rgb;
  float i = intensity(rgb);
  out_color = vec4(i, i, i, i);)"sv;

// GX_TF_IA4: 4-bit intensity + 4-bit alpha -> RG8Unorm
static constexpr std::string_view FragIA4 = R"(
  vec4 c = copied;
  float i = quantize4(intensity(c.rgb));
  float a = quantize4(c.a);
  out_color = vec4(i, i, i, a);)"sv;

// GX_TF_IA8: 8-bit intensity + 8-bit alpha -> RG8Unorm
static constexpr std::string_view FragIA8 = R"(
  vec4 c = copied;
  float i = intensity(c.rgb);
  out_color = vec4(i, i, i, c.a);)"sv;

// GX_TF_RGB565: Blit alpha to 1.0
static constexpr std::string_view FragRGB565 = R"(
  vec4 c = copied;
  out_color = vec4(c.rgb, 1.0);)"sv;

// GX_CTF_R4: 4-bit red -> R8Unorm
static constexpr std::string_view FragR4 = R"(
  float r = quantize4(copied.r);
  out_color = vec4(r, r, r, r);)"sv;

// GX_CTF_RA4: 4-bit red + 4-bit alpha -> RG8Unorm
static constexpr std::string_view FragRA4 = R"(
  vec4 c = copied;
  float r = quantize4(c.r);
  out_color = vec4(r, r, r, quantize4(c.a));)"sv;

// GX_CTF_RA8: 8-bit red + 8-bit alpha -> RG8Unorm
static constexpr std::string_view FragRA8 = R"(
  vec4 c = copied;
  out_color = vec4(c.r, c.r, c.r, c.a);)"sv;

// GX_CTF_A8: 8-bit alpha -> R8Unorm
static constexpr std::string_view FragA8 = R"(
  float a = copied.a;
  out_color = vec4(a, a, a, a);)"sv;

// GX_CTF_R8: 8-bit red -> R8Unorm
static constexpr std::string_view FragR8 = R"(
  float r = copied.r;
  out_color = vec4(r, r, r, r);)"sv;

// GX_CTF_G8: 8-bit green -> R8Unorm
static constexpr std::string_view FragG8 = R"(
  float g = copied.g;
  out_color = vec4(g, g, g, g);)"sv;

// GX_CTF_B8: 8-bit blue -> R8Unorm
static constexpr std::string_view FragB8 = R"(
  float b = copied.b;
  out_color = vec4(b, b, b, b);)"sv;

// GX_CTF_RG8: 8-bit red + 8-bit green -> RG8Unorm
static constexpr std::string_view FragRG8 = R"(
  vec4 c = copied;
  out_color = vec4(c.r, c.r, c.r, c.g);)"sv;

// GX_CTF_GB8: 8-bit green + 8-bit blue -> RG8Unorm
static constexpr std::string_view FragGB8 = R"(
  vec4 c = copied;
  out_color = vec4(c.g, c.g, c.g, c.b);)"sv;

// GX_TF_Z8 stores the high-Z byte in I8 storage, which samples as intensity
// replicated across all four channels.
static constexpr std::string_view FragZ8 = R"(
  float z8 = float(depth_bytes.r) / 255.0;
  out_color = vec4(z8, z8, z8, z8);)"sv;

// GX_TF_Z16 is the depth RA8 copy path: depth samples have opaque alpha and their high-Z byte in
// red, so the encoded [A, R] pair samples through IA8 as high-Z intensity with opaque alpha.
static constexpr std::string_view FragZ16 = detail::Z16FragmentShader;

// GX_TF_Z24X8 uses the RGBA8 storage shape for copies, with the 24-bit depth
// value available to z-texture sampling through RGB.
static constexpr std::string_view FragZ24X8 = R"(
  float r = float(depth_bytes.r) / 255.0;
  float g = float(depth_bytes.g) / 255.0;
  float b = float(depth_bytes.b) / 255.0;
  out_color = vec4(r, g, b, 1.0);)"sv;

struct ConvPipeline {
  GXTexFmt fmt;
  std::string_view fragShader;
  SDL_GPUTextureFormat outputFormat;
  const char* label;
};

static constexpr std::array ConvPipelines{
    ConvPipeline{GX_TF_I4, FragI4, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv I4"},
    ConvPipeline{GX_TF_I8, FragI8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv I8"},
    ConvPipeline{GX_TF_IA4, FragIA4, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv IA4"},
    ConvPipeline{GX_TF_IA8, FragIA8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv IA8"},
    ConvPipeline{GX_TF_RGB565, FragRGB565, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv RGB565"},
    ConvPipeline{GX_CTF_R4, FragR4, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv R4"},
    ConvPipeline{GX_CTF_RA4, FragRA4, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv RA4"},
    ConvPipeline{GX_CTF_RA8, FragRA8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv RA8"},
    ConvPipeline{GX_CTF_A8, FragA8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv A8"},
    ConvPipeline{GX_CTF_R8, FragR8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv R8"},
    ConvPipeline{GX_CTF_G8, FragG8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv G8"},
    ConvPipeline{GX_CTF_B8, FragB8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv B8"},
    ConvPipeline{GX_CTF_RG8, FragRG8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv RG8"},
    ConvPipeline{GX_CTF_GB8, FragGB8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv GB8"},
};

static constexpr std::array DepthConvPipelines{
    ConvPipeline{GX_TF_Z8, FragZ8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv Z8"},
    ConvPipeline{GX_TF_Z16, FragZ16, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv Z16"},
    ConvPipeline{GX_TF_Z24X8, FragZ24X8, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "TexCopyConv Z24X8"},
};

static gpu::Sampler g_nearestSampler;
static gpu::Sampler g_linearSampler;
static absl::flat_hash_map<GXTexFmt, gpu::GraphicsPipeline> g_pipelines;
static absl::flat_hash_map<SDL_GPUTextureFormat, gpu::GraphicsPipeline> g_blitPipelines;

static gpu::GraphicsPipeline create_pipeline(const ConvPipeline& conv, const gpu::Shader& vertex, bool depth) {
  const gpu::shader::Interface fs{
      .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
      .uniforms = {{"uv_xf", std::string{UVTransformMembers}}},
      .textures = {{"src", depth ? gpu::shader::TextureKind::Depth : gpu::shader::TextureKind::Float}},
      .varyings = {{"vec2", "uv"}},
  };
  std::string body;
  body.reserve(DepthSampleBody.size() + conv.fragShader.size());
  body += depth ? DepthSampleBody : ColorSampleBody;
  body += conv.fragShader;
  const auto fragment =
      gpu::shader::create(fs, depth ? std::string_view{DepthHelpers} : ColorHelpers, body, conv.label);
  if (!vertex || !fragment) {
    return nullptr;
  }

  const SDL_GPUColorTargetDescription colorTarget{.format = conv.outputFormat};
  const SDL_GPUGraphicsPipelineCreateInfo info{
      .vertex_shader = vertex->handle,
      .fragment_shader = fragment->handle,
      .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
      .rasterizer_state =
          {
              .fill_mode = SDL_GPU_FILLMODE_FILL,
              .cull_mode = SDL_GPU_CULLMODE_NONE,
              .enable_depth_clip = true,
          },
      .target_info = {.color_target_descriptions = &colorTarget, .num_color_targets = 1},
  };
  return gpu::create_graphics_pipeline(info, conv.label);
}

bool needs_conversion(const GXTexFmt fmt) { return g_pipelines.contains(fmt); }

void initialize() {
  const gpu::shader::Interface vs{
      .stage = SDL_GPU_SHADERSTAGE_VERTEX,
      .uniforms = {{"uv_xf", std::string{UVTransformMembers}}},
      .varyings = {{"vec2", "uv"}},
  };
  const auto vertex = gpu::shader::create(vs, {}, VertexBody, "TexCopyConv vertex");

  // Native RAM readback uses RGBA even when the EFB/surface uses BGRA.
  // Build both variants here; frame workers only read the completed map.
  for (const auto format : {SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM}) {
    g_blitPipelines[format] = create_pipeline({GX_TF_RGBA8, FragPassthrough, format, "TexCopyConv Blit"}, vertex, false);
  }
  for (const auto& conv : ConvPipelines) {
    g_pipelines[conv.fmt] = create_pipeline(conv, vertex, false);
  }
  for (const auto& conv : DepthConvPipelines) {
    g_pipelines[conv.fmt] = create_pipeline(conv, vertex, true);
  }

  constexpr SDL_GPUSamplerCreateInfo nearestSamplerInfo{
      .min_filter = SDL_GPU_FILTER_NEAREST,
      .mag_filter = SDL_GPU_FILTER_NEAREST,
      .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
      .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
  };
  g_nearestSampler = gpu::create_sampler(nearestSamplerInfo);

  constexpr SDL_GPUSamplerCreateInfo linearSamplerInfo{
      .min_filter = SDL_GPU_FILTER_LINEAR,
      .mag_filter = SDL_GPU_FILTER_LINEAR,
      .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
      .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
  };
  g_linearSampler = gpu::create_sampler(linearSamplerInfo);
}

void shutdown() {
  g_pipelines.clear();
  g_blitPipelines.clear();
  g_nearestSampler = {};
  g_linearSampler = {};
}

static void execute(gpu::Encoder& enc, const uint8_t* uniforms, const ConvRequest& req,
                    const gpu::GraphicsPipeline& pipeline) {
  // Depth sources are only read with LOAD_DEPTH; SDL still binds a sampler with every texture.
  const auto& sampler = req.sampleFilter == SampleFilter::Linear && !gx::is_depth_format(req.fmt)
                            ? g_linearSampler
                            : g_nearestSampler;
  if (!pipeline || !sampler || !req.src) {
    return;
  }

  const SDL_GPUColorTargetInfo colorTarget{
      .texture = req.dst->texture->handle,
      .clear_color = {0.f, 0.f, 0.f, 0.f},
      .load_op = SDL_GPU_LOADOP_CLEAR,
      .store_op = SDL_GPU_STOREOP_STORE,
  };
  SDL_GPURenderPass* pass = enc.begin_render_pass(&colorTarget, 1, nullptr);
  if (pass == nullptr) {
    Log.error("TexCopyConv: failed to begin render pass: {}", SDL_GetError());
    return;
  }
  SDL_BindGPUGraphicsPipeline(pass, pipeline->handle);
  const SDL_GPUTextureSamplerBinding binding{.texture = req.src->handle, .sampler = sampler->handle};
  SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
  const void* uniform = uniforms + req.uniformRange.offset;
  SDL_PushGPUVertexUniformData(enc.cmd(), 0, uniform, req.uniformRange.size);
  SDL_PushGPUFragmentUniformData(enc.cmd(), 0, uniform, req.uniformRange.size);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
}

void run(gpu::Encoder& enc, const uint8_t* uniforms, const ConvRequest& req) {
  const auto it = g_pipelines.find(req.fmt);
  if (it == g_pipelines.end()) {
    Log.fatal("No copy conversion pipeline for format {}", static_cast<int>(req.fmt));
  }
  execute(enc, uniforms, req, it->second);
}

void blit(gpu::Encoder& enc, const uint8_t* uniforms, const ConvRequest& req) {
  const auto it = g_blitPipelines.find(req.dst->format);
  if (it == g_blitPipelines.end()) {
    Log.fatal("Unsupported blit destination format {}", static_cast<int>(req.dst->format));
  }
  execute(enc, uniforms, req, it->second);
}

} // namespace aurora::gfx::tex_copy_conv
