#pragma once

#include "common.hpp"

namespace aurora::gfx::clear {
struct DrawData {
  PipelineRef pipeline;
  Range uniformRange;
  SDL_FColor color;
  float depth = 0.f;
  bool useScissor = false;
  ClipRect scissor{};
};

constexpr uint32_t ClearPipelineConfigVersion = 3;
struct PipelineConfig {
  uint32_t version = ClearPipelineConfigVersion;
  uint32_t msaaSamples = 1;
  bool clearColor = true;
  bool clearAlpha = true;
  bool clearDepth = true;
  uint8_t _pad = 0;
};
static_assert(std::has_unique_object_representations_v<PipelineConfig>);

gpu::GraphicsPipeline create_pipeline(const PipelineConfig& config);
void render(const DrawData& data, const PassContext& ctx, PipelineRef& currentPipeline);
} // namespace aurora::gfx::clear
