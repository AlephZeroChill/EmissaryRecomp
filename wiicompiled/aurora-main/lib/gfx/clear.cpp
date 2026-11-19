#include "clear.hpp"

#include "../gpu/gpu.hpp"
#include "../gpu/shader.hpp"
#include "aurora/profiling.hpp"

#include <algorithm>

namespace {
SDL_GPUColorComponentFlags clear_write_mask(bool clearColor, bool clearAlpha) {
  SDL_GPUColorComponentFlags writeMask = 0;
  if (clearColor) {
    writeMask |= SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
  }
  if (clearAlpha) {
    writeMask |= SDL_GPU_COLORCOMPONENT_A;
  }
  return writeMask;
}
} // namespace

namespace aurora::gfx::clear {

using gpu::g_graphicsConfig;

gpu::GraphicsPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  const gpu::shader::Interface vs{.stage = SDL_GPU_SHADERSTAGE_VERTEX};
  constexpr std::string_view vsBody = R"(
  vec2 p = vidx == 0u ? vec2(-1.0, 1.0) : (vidx == 1u ? vec2(-1.0, -3.0) : vec2(3.0, 1.0));
  out_pos = vec4(p, 0.0, 1.0);)";
  const gpu::shader::Interface fs{
      .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
      // x: depth
      .uniforms = {{"clear", "vec4 value;"}},
      .writesDepth = true,
  };
  constexpr std::string_view fsBody = R"(
  out_color = vec4(1.0);
  out_depth = clear.value.x;)";
  const auto vertex = gpu::shader::create(vs, {}, vsBody, "EFB Clear vertex");
  const auto fragment = gpu::shader::create(fs, {}, fsBody, "EFB Clear fragment");
  if (!vertex || !fragment) {
    return nullptr;
  }
  const SDL_GPUColorTargetDescription colorTarget{
      .format = g_graphicsConfig.surfaceFormat,
      .blend_state =
          SDL_GPUColorTargetBlendState{
              .src_color_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR,
              .dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
              .color_blend_op = SDL_GPU_BLENDOP_ADD,
              .src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR,
              .dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO,
              .alpha_blend_op = SDL_GPU_BLENDOP_ADD,
              .color_write_mask = clear_write_mask(config.clearColor, config.clearAlpha),
              .enable_blend = true,
              .enable_color_write_mask = true,
          },
  };
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
      .multisample_state = {.sample_count = gpu::sample_count(config.msaaSamples)},
      .depth_stencil_state =
          {
              .compare_op = SDL_GPU_COMPAREOP_ALWAYS,
              .enable_depth_test = true,
              .enable_depth_write = config.clearDepth,
          },
      .target_info =
          {
              .color_target_descriptions = &colorTarget,
              .num_color_targets = 1,
              .depth_stencil_format = g_graphicsConfig.depthFormat,
              .has_depth_stencil_target = true,
          },
  };
  const auto label = fmt::format("EFB Clear Pipeline (color {}, alpha {}, depth {})", config.clearColor,
                                 config.clearAlpha, config.clearDepth);
  return gpu::create_graphics_pipeline(info, label.c_str());
}

void render(const DrawData& data, const PassContext& ctx, PipelineRef& currentPipeline) {
  if (!bind_pipeline(data.pipeline, ctx, currentPipeline)) {
    return;
  }

  const uint32_t targetWidth = ctx.targetWidth;
  const uint32_t targetHeight = ctx.targetHeight;
  SDL_PushGPUFragmentUniformData(ctx.cmd, 0, ctx.uniform(data.uniformRange), data.uniformRange.size);
  SDL_SetGPUBlendConstants(ctx.pass, data.color);
  const SDL_GPUViewport viewport{
      .x = 0.f,
      .y = 0.f,
      .w = static_cast<float>(targetWidth),
      .h = static_cast<float>(targetHeight),
      .min_depth = 0.f,
      .max_depth = 1.f,
  };
  SDL_SetGPUViewport(ctx.pass, &viewport);
  SDL_Rect scissor{0, 0, static_cast<int>(targetWidth), static_cast<int>(targetHeight)};
  if (data.useScissor) {
    const auto x = std::clamp<int32_t>(data.scissor.x, 0, static_cast<int32_t>(targetWidth));
    const auto y = std::clamp<int32_t>(data.scissor.y, 0, static_cast<int32_t>(targetHeight));
    const auto right =
        std::clamp<int32_t>(data.scissor.x + data.scissor.width, 0, static_cast<int32_t>(targetWidth));
    const auto bottom =
        std::clamp<int32_t>(data.scissor.y + data.scissor.height, 0, static_cast<int32_t>(targetHeight));
    if (right <= x || bottom <= y) {
      return;
    }
    scissor = SDL_Rect{x, y, right - x, bottom - y};
  }
  SDL_SetGPUScissor(ctx.pass, &scissor);
  SDL_DrawGPUPrimitives(ctx.pass, 3, 1, 0, 0);
}
} // namespace aurora::gfx::clear
