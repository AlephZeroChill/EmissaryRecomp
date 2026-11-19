#include "pipeline.hpp"

#include "../gpu/gpu.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"
#include "aurora/profiling.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>

#include <absl/container/flat_hash_map.h>

namespace aurora::gx {
static Module Log("aurora::gx");

namespace {
struct ShaderConfigHash {
  size_t operator()(const ShaderConfig& config) const noexcept { return static_cast<size_t>(xxh3_hash(config)); }
};

struct CachedShaderModule {
  std::condition_variable ready;
  GXShaders module;
  bool compiling = true;
};

std::mutex sShaderModuleCacheMutex;
absl::flat_hash_map<ShaderConfig, std::shared_ptr<CachedShaderModule>, ShaderConfigHash> sShaderModuleCache;

GXShaders cached_shader_module(const ShaderConfig& config) {
  std::shared_ptr<CachedShaderModule> entry;
  {
    std::unique_lock lock{sShaderModuleCacheMutex};
    const auto it = sShaderModuleCache.find(config);
    if (it == sShaderModuleCache.end()) {
      entry = std::make_shared<CachedShaderModule>();
      sShaderModuleCache.emplace(config, entry);
    } else {
      entry = it->second;
      entry->ready.wait(lock, [&] { return !entry->compiling; });
      return entry->module;
    }
  }

  auto module = build_shader(config);
  {
    std::lock_guard lock{sShaderModuleCacheMutex};
    entry->module = module;
    entry->compiling = false;
  }
  entry->ready.notify_all();
  return module;
}
} // namespace

gpu::GraphicsPipeline create_pipeline(const PipelineConfig& config) {
  ZoneScoped;
  const auto shaders = cached_shader_module(config.shaderConfig);
  if (!shaders) {
    return nullptr;
  }
  return build_pipeline(config, shaders, "GX Pipeline");
}

void clear_shader_module_cache() {
  std::lock_guard lock{sShaderModuleCacheMutex};
  sShaderModuleCache.clear();
}

void render(const DrawData& data, const gfx::PassContext& ctx, DrawEncodeState& state,
            bool requireReadyPipeline, const gfx::Range* uniformRangeOverride) {
  if (!gfx::bind_pipeline(data.pipeline, ctx, state.currentPipeline, requireReadyPipeline)) {
    return;
  }

  // An interpolated presentation slot re-encodes the identical draw with only this range replaced; overriding here avoids copying the whole DrawData per draw per slot.
  const gfx::Range& uniformRange = uniformRangeOverride != nullptr ? *uniformRangeOverride : data.uniformRange;
  ctx.push_uniform(0, uniformRange);
  // Resolved when the draw was recorded; see GXBindGroups.
  if (data.bindGroups.resolvedTextureBindGroup != nullptr &&
      data.bindGroups.resolvedTextureBindGroup != state.boundTextureBindGroup) {
    const auto& textures = *data.bindGroups.resolvedTextureBindGroup;
    SDL_BindGPUFragmentSamplers(ctx.pass, 0, textures.raw.data(), textures.count);
    state.boundTextureBindGroup = data.bindGroups.resolvedTextureBindGroup;
  }
  if (data.dstAlpha != UINT32_MAX) {
    SDL_SetGPUBlendConstants(ctx.pass, SDL_FColor{0.f, 0.f, 0.f, static_cast<float>(data.dstAlpha) / 255.f});
  }
  if (!state.indexBufferBound) {
    // Bound once for the pass; draws select their range with firstIndex below.
    const SDL_GPUBufferBinding indexBinding{.buffer = gfx::g_indexBuffer->handle, .offset = 0};
    SDL_BindGPUIndexBuffer(ctx.pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    state.indexBufferBound = true;
  }
  SDL_DrawGPUIndexedPrimitives(ctx.pass, data.indexCount, data.instanceCount,
                               static_cast<uint32_t>(data.idxRange.offset / sizeof(uint16_t)), 0, 0);
}
} // namespace aurora::gx
