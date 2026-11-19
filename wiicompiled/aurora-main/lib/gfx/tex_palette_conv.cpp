#include "tex_palette_conv.hpp"

#include "../internal.hpp"
#include "../gpu/gpu.hpp"
#include "../gpu/shader.hpp"
#include "texture.hpp"

#include <array>

namespace aurora::gfx::tex_palette_conv {
static Module Log("aurora::gfx::tex_palette_conv");

using namespace std::string_view_literals;

static constexpr std::string_view VertexBody = R"(
  vec2 p = vidx == 0u ? vec2(-1.0, 1.0) : (vidx == 1u ? vec2(-1.0, -3.0) : vec2(3.0, 1.0));
  vec2 uv = vidx == 0u ? vec2(0.0, 0.0) : (vidx == 1u ? vec2(0.0, 2.0) : vec2(2.0, 0.0));
  out_pos = vec4(p, 0.0, 1.0);
  vout.uv = uv;)"sv;

// Direct: R16Sint index texture + TLUT -> RGBA8
static constexpr std::string_view ShaderDirect = R"(
  vec2 texSize = vec2(DIMS(src));
  ivec2 coord = ivec2(floor(vin.uv * texSize));
  int idx = LOAD(src, coord).r;
  out_color = LOAD(tlut, ivec2(idx, 0));)"sv;

// FromFloat8: f32 texture (R8Unorm) -> 8-bit index -> TLUT -> RGBA8
static constexpr std::string_view ShaderFromFloat8 = R"(
  vec2 texSize = vec2(DIMS(src));
  ivec2 coord = ivec2(floor(vin.uv * texSize));
  float r = LOAD(src, coord).r;
  out_color = LOAD(tlut, ivec2(int(r * 255.0), 0));)"sv;

// FromFloat4: f32 texture (R8Unorm) -> 4-bit index -> TLUT -> RGBA8
static constexpr std::string_view ShaderFromFloat4 = R"(
  vec2 texSize = vec2(DIMS(src));
  ivec2 coord = ivec2(floor(vin.uv * texSize));
  float r = LOAD(src, coord).r;
  out_color = LOAD(tlut, ivec2(int(r * 15.0), 0));)"sv;

static gpu::GraphicsPipeline g_directPipeline;
static gpu::GraphicsPipeline g_fromFloat8Pipeline;
static gpu::GraphicsPipeline g_fromFloat4Pipeline;
static gpu::Sampler g_sampler;

static gpu::GraphicsPipeline create_pipeline(std::string_view fragBody, gpu::shader::TextureKind srcKind,
                                             const char* label) {
  const gpu::shader::Interface vs{
      .stage = SDL_GPU_SHADERSTAGE_VERTEX,
      .varyings = {{"vec2", "uv"}},
  };
  const gpu::shader::Interface fs{
      .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
      .textures = {{"src", srcKind}, {"tlut", gpu::shader::TextureKind::Float}},
      .varyings = {{"vec2", "uv"}},
  };
  const auto vertexLabel = fmt::format("{} vertex", label);
  const auto fragmentLabel = fmt::format("{} fragment", label);
  const auto vertex = gpu::shader::create(vs, {}, VertexBody, vertexLabel.c_str());
  const auto fragment = gpu::shader::create(fs, {}, fragBody, fragmentLabel.c_str());
  if (!vertex || !fragment) {
    return nullptr;
  }

  const SDL_GPUColorTargetDescription colorTarget{.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM};
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
  return gpu::create_graphics_pipeline(info, label);
}

static const gpu::GraphicsPipeline& pipeline_for_variant(Variant variant) {
  switch (variant) {
  case Variant::Direct:
    return g_directPipeline;
  case Variant::FromFloat8:
    return g_fromFloat8Pipeline;
  case Variant::FromFloat4:
    return g_fromFloat4Pipeline;
  }
  FATAL("invalid palette conv variant {}", static_cast<int>(variant));
}

void initialize() {
  g_directPipeline = create_pipeline(ShaderDirect, gpu::shader::TextureKind::Int, "TexPaletteConv Direct");
  g_fromFloat8Pipeline =
      create_pipeline(ShaderFromFloat8, gpu::shader::TextureKind::Float, "TexPaletteConv FromFloat8");
  g_fromFloat4Pipeline =
      create_pipeline(ShaderFromFloat4, gpu::shader::TextureKind::Float, "TexPaletteConv FromFloat4");
  constexpr SDL_GPUSamplerCreateInfo samplerInfo{
      .min_filter = SDL_GPU_FILTER_NEAREST,
      .mag_filter = SDL_GPU_FILTER_NEAREST,
      .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
      .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
  };
  g_sampler = gpu::create_sampler(samplerInfo);
}

void shutdown() {
  g_directPipeline = {};
  g_fromFloat8Pipeline = {};
  g_fromFloat4Pipeline = {};
  g_sampler = {};
}

void run(gpu::Encoder& enc, const ConvRequest& req) {
  const auto& pipeline = pipeline_for_variant(req.variant);
  if (!pipeline || !g_sampler) {
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
    Log.error("TexPaletteConv: failed to begin render pass: {}", SDL_GetError());
    return;
  }
  SDL_BindGPUGraphicsPipeline(pass, pipeline->handle);
  const std::array bindings{
      SDL_GPUTextureSamplerBinding{.texture = req.src->texture->handle, .sampler = g_sampler->handle},
      SDL_GPUTextureSamplerBinding{.texture = req.tlut->texture->handle, .sampler = g_sampler->handle},
  };
  SDL_BindGPUFragmentSamplers(pass, 0, bindings.data(), bindings.size());
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
  SDL_EndGPURenderPass(pass);
}

} // namespace aurora::gfx::tex_palette_conv
