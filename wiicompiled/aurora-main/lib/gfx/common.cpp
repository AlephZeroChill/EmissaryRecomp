#include "common.hpp"
#include "../gx/shader_info.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "efb_ram_copy.hpp"
#include "../internal.hpp"
#include "../gpu/gpu.hpp"
#include "../gx/pipeline.hpp"
#include "../gx/gx_worker.hpp"
#include "pipeline_cache.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "texture_replacement.hpp"
#include "texture.hpp"
#include "../window.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>

#include <absl/container/flat_hash_map.h>
#include <magic_enum.hpp>

#include "aurora/profiling.hpp"

namespace aurora::gfx {
static Module Log("aurora::gfx");

using gpu::g_device;

struct DebugFrameData {
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> groups;
  std::vector<std::string> markers;
#endif
};
DebugFrameData g_debugFrame;

constexpr uint64_t StagingBufferSize = UniformBufferSize + VertexBufferSize + IndexBufferSize + StorageBufferSize +
                                       (UseTextureBuffer ? TextureUploadSize : 0);

struct ShaderDrawCommand {
  ShaderType type;
  union {
    clear::DrawData clear;
    gx::DrawData gx;
  };
};
enum class CommandType {
  SetViewport,
  SetScissor,
  Draw,
  DebugMarker,
};
struct Command {
  CommandType type;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> debugGroupStack;
#endif
  union Data {
    Viewport setViewport;
    ClipRect setScissor;
    ShaderDrawCommand draw;
    size_t debugMarkerIndex;
  } data;
};
} // namespace aurora::gfx

void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify) {
  aurora::g_guestWriteGenerationHook = generation;
  aurora::g_guestWriteNotifyHook = notify;
}

namespace aurora::gfx {
namespace {
// SDL's create-info structs carry padding and a properties id, so hash the fields that matter.
HashType hash_sampler(const SDL_GPUSamplerCreateInfo& info) {
  const std::array<uint32_t, 13> key{
      static_cast<uint32_t>(info.min_filter),
      static_cast<uint32_t>(info.mag_filter),
      static_cast<uint32_t>(info.mipmap_mode),
      static_cast<uint32_t>(info.address_mode_u),
      static_cast<uint32_t>(info.address_mode_v),
      static_cast<uint32_t>(info.address_mode_w),
      std::bit_cast<uint32_t>(info.mip_lod_bias),
      std::bit_cast<uint32_t>(info.max_anisotropy),
      static_cast<uint32_t>(info.compare_op),
      std::bit_cast<uint32_t>(info.min_lod),
      std::bit_cast<uint32_t>(info.max_lod),
      static_cast<uint32_t>(info.enable_anisotropy),
      static_cast<uint32_t>(info.enable_compare),
  };
  return xxh3_hash(key);
}

HashType hash_texture_bindings(const TextureBindingsRaw& raw, uint32_t count) {
  std::array<uintptr_t, MaxBoundTextures * 2 + 1> key{};
  for (uint32_t i = 0; i < count; ++i) {
    key[i * 2] = reinterpret_cast<uintptr_t>(raw[i].texture);
    key[i * 2 + 1] = reinterpret_cast<uintptr_t>(raw[i].sampler);
  }
  key.back() = count;
  return xxh3_hash(key);
}
HashType hash_texture_bindings(const TextureBindings& bindings) {
  return hash_texture_bindings(bindings.raw, bindings.count);
}
} // namespace
} // namespace aurora::gfx

namespace aurora::gfx {
namespace {
struct CachedBindGroup {
  std::unique_ptr<TextureBindings> bindings;
  uint32_t lastUsedFrame = 0;
};

constexpr uint32_t BindGroupCacheRetainFrames = 32;
constexpr uint32_t BindGroupCacheSweepPeriod = 16;
} // namespace

static absl::flat_hash_map<BindGroupRef, CachedBindGroup> g_cachedBindGroups;
// Bind groups dropped from the cache by clear_caches() while a sealed frame's recorded draws
// still reference them. Ownership moves here and is released at the next seal.
static std::vector<CachedBindGroup> g_retiredBindGroups;
static absl::flat_hash_map<SamplerRef, gpu::Sampler> g_cachedSamplers;

static ByteBuffer g_verts;
} // namespace aurora::gfx
namespace aurora::gx {
extern uint32_t g_arrayUploadEpoch;  // command_processor.cpp
}
namespace aurora::gfx {
static ByteBuffer g_uniforms;
static ByteBuffer g_indices;
static ByteBuffer g_storage;
static ByteBuffer g_textureUpload;
gpu::Buffer g_vertexBuffer;
gpu::Buffer g_indexBuffer;
gpu::Buffer g_storageBuffer;
constexpr size_t FrameSlotCount = 3;
// Per frame slot: an upload transfer buffer holding the slot's vertex, index and storage bytes, and a CPU
// arena for its uniform bytes, which draws push straight from memory. A slot is written again only after
// the frame that used it has been encoded (the producer joins the worker before sealing the next frame).
static std::array<gpu::TransferBuffer, FrameSlotCount> g_stagingBuffers;
static std::array<std::unique_ptr<uint8_t[]>, FrameSlotCount> g_uniformArenas;
static uint8_t* g_stagingMapped = nullptr;
static size_t currentStagingBuffer = 0;
// The uniform arena of the batch end_batch last closed: what render() reads.
static const uint8_t* g_batchUniforms = nullptr;
constexpr uint32_t UniformAlignment = 16;
constexpr uint32_t StorageAlignment = 16;
// Advanced once per logical frame in the seal prologue, under the renderer GPU mutex and with the
// producer blocked, so every later reader sees a value that no longer moves.
static uint32_t g_frameIndex = UINT32_MAX;

// for imgui debug
AuroraStats g_stats{};
uint32_t g_drawCallCount = 0;
uint32_t g_mergedDrawCallCount = 0;

using CommandList = std::vector<Command>;
struct RenderPass {
  gpu::Texture colorView;
  gpu::Texture resolveView; // MSAA resolve target; null if msaaSamples == 1
  gpu::Texture depthView;
  gpu::Texture copySourceTexture;
  gpu::Texture copySourceView;
  gpu::Texture copySourceDepthView;
  Extent3D targetSize;
  uint32_t msaaSamples = 1;

  TextureHandle resolveTarget;
  TextureHandle resolveSourceSnapshot;
  GXTexFmt resolveFormat = GX_TF_RGBA8;
  ClipRect resolveRect;
  ClipRect resolveSnapshotRect;
  Vec4<float> resolveSourceRect;
  Range resolveUniformRange;
  std::array<u32, 3> resolveCopyFilterCoefficients{0, 64, 0};
  Vec4<float> clearColorValue{0.f, 0.f, 0.f, 0.f};
  // 1.f is the forward-Z "farthest" clear value; under UseReversedZ farthest is 0.f instead (see
  // gx::clear_depth_value(), which the main render pass explicitly overrides this default with -
  // any OTHER pass that keeps this default, e.g. an offscreen render-to-texture pass composited
  // later, needs the same reversed-Z-aware value or its depth buffer starts "already nearest",
  // failing every subsequent depth test and making whatever's drawn into it vanish).
  float clearDepthValue = gx::UseReversedZ ? 0.f : 1.f;
  CommandList commands;
  bool clearColor = true;
  bool clearDepth = true;
  // The resolve destination outlives the frame with no re-issue path (one-shot
  // bake), so this pass may not drop draws even in skip-unready-pipelines mode.
  bool requireReadyPipelines = false;
  bool resolveHalfScale = false;
  bool resolveCopyFilterActive = false;
  bool resolveForceOpaqueAlpha = false;
  bool resolveNeedsConversion = false;
  bool resolveNeedsShaderSampling = false;
  bool resolveLinearSampling = false;
  bool snapshotColorResolveSource = false;
  std::vector<tex_palette_conv::ConvRequest> paletteConvs;
  // NSMBW: copies of this pass's resolve target into a region of another texture (add_resolve_patch).
  struct Patch {
    TextureHandle dst;
    TextureHandle native;  // the resolve target scaled to the patch's own size
    Range uniformRange;
    uint32_t x = 0, y = 0, width = 0, height = 0;
  };
  std::vector<Patch> patches;
};
static std::vector<RenderPass> g_renderPasses;
static u32 g_currentRenderPass = UINT32_MAX;

// Recycle command storage: discarding passes used to free their command lists too, so each frame
// rebuilt hundreds of KB from zero capacity. The passes themselves are cheap to recreate.
using CommandListPool = std::vector<CommandList>;
static CommandListPool g_commandListPool;
static constexpr size_t MaxPooledCommandLists = 32;
// The pool is the only recording storage both sides touch, a few pointer moves per frame, so a
// plain mutex is cheaper than the alternatives and keeps the vector's invariants.
static std::mutex g_commandListPoolMutex;

static CommandList acquire_command_list() noexcept {
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.empty()) {
    return {};
  }
  CommandList list = std::move(g_commandListPool.back());
  g_commandListPool.pop_back();
  list.clear();
  return list;
}

static void release_command_list(CommandList&& list) noexcept {
  if (list.capacity() == 0) {
    return;
  }
  std::lock_guard lock{g_commandListPoolMutex};
  if (g_commandListPool.size() >= MaxPooledCommandLists) {
    return;
  }
  list.clear();
  g_commandListPool.emplace_back(std::move(list));
}

static void release_render_pass(RenderPass& pass) noexcept { release_command_list(std::move(pass.commands)); }

static void recycle_render_passes(std::vector<RenderPass>& passes) noexcept {
  for (auto& pass : passes) {
    release_render_pass(pass);
  }
  passes.clear();
}

struct SealedFrameData {
  depth_peek::FrameMapping depthMapping;
  const uint8_t* uniforms = nullptr;
  DebugFrameData debug;
  std::vector<RenderPass> passes;
};

SealedFrame::SealedFrame() : m_data(std::make_unique<SealedFrameData>()) {}
SealedFrame::~SealedFrame() = default;
SealedFrame::SealedFrame(SealedFrame&&) noexcept = default;
SealedFrame& SealedFrame::operator=(SealedFrame&&) noexcept = default;

static RenderPass& push_render_pass(RenderPass&& pass) {
  auto& out = g_renderPasses.emplace_back(std::move(pass));
  if (out.commands.capacity() == 0) {
    out.commands = acquire_command_list();
  }
  return out;
}
// The producer may build the next frame's GX stream while the worker prepares the target, and
// this is the only renderer-owned property that path queries, so publish it explicitly.
static std::atomic_bool g_inOffscreen{false};
static std::optional<RenderPass> g_suspendedEfbPass;
static Viewport g_suspendedEfbViewport;
static ClipRect g_suspendedEfbScissor;
// Prefix referenced by a suspended EFB pass. Preserve its offsets across an
// offscreen split, without rendering it before the bake it may sample finishes.
static StagingSizes g_suspendedEfbBytes{};
static constexpr StagingSizes PhysicalStagingCapacity{
    VertexBufferSize, UniformBufferSize, IndexBufferSize, StorageBufferSize};
static StagingSizes g_stagingCapacity = PhysicalStagingCapacity;
static uint64_t g_stagingEpoch = 0;
static uint64_t g_stagingSplitCount = 0;
static StagingSizes g_stagingHighWater{};

StagingSizes staging_usage() noexcept {
  return {g_verts.size(), g_uniforms.size(), g_indices.size(), g_storage.size()};
}
StagingSizes staging_high_water() noexcept { return g_stagingHighWater; }
uint64_t staging_epoch() noexcept { return g_stagingEpoch; }
uint64_t staging_split_count() noexcept { return g_stagingSplitCount; }
uint64_t staging_uniform_bytes(uint64_t bytes) {
  return staging_padded(bytes, UniformAlignment);
}
uint64_t staging_storage_bytes(uint64_t bytes) {
  return staging_padded(bytes, StorageAlignment);
}
void set_staging_capacity_limits_for_testing(const StagingSizes& limits) {
  for (unsigned i = 0; i < limits.size(); ++i) {
    if (limits[i] > PhysicalStagingCapacity[i])
      throw StagingCapacityError("Test staging capacity exceeds physical buffer");
  }
  g_stagingCapacity = limits;
  g_stagingHighWater = {};
}
bool staging_has_space(const StagingSizes& demand) {
  // Async readback preparation runs in the worker's noexcept seal prologue.
  // Reserve all 32 slots plus the uniform binding's 3840-byte trailing window.
  const StagingSizes tail{0, gx::MaxUniformSize + efb_ram::MaxAsyncReadbackSlots * staging_uniform_bytes(48), 0, 0};
  const StagingSizes retained = g_suspendedEfbPass ? g_suspendedEfbBytes : StagingSizes{};
  if (!staging_fits(retained, demand, tail, g_stagingCapacity))
    throw StagingCapacityError("GPU operation exceeds staging capacity including retained EFB data");
  return staging_fits(staging_usage(), demand, tail, g_stagingCapacity);
}
void ensure_staging_space(const StagingSizes& demand) {
  if (staging_has_space(demand)) return;
  split_staging_batch();
  if (!staging_has_space(demand))
    throw StagingCapacityError("GPU operation still exceeds staging capacity after submission");
}

static void discard_suspended_efb_pass() noexcept {
  if (g_suspendedEfbPass) {
    release_render_pass(*g_suspendedEfbPass);
    g_suspendedEfbPass.reset();
  }
}

static bool has_current_render_pass() noexcept { return g_currentRenderPass < g_renderPasses.size(); }
static gpu::TextureWithSampler g_offscreenColor;
static gpu::TextureWithSampler g_offscreenDepth;

struct ResolveSourceSnapshotPool {
  TextureHandle entry;
};

// Snapshot resources live in the same safe frame slots as the mapped staging buffers, and copies
// within a frame are ordered, so one grow-only snapshot serves every resolve in that slot.
static std::array<ResolveSourceSnapshotPool, FrameSlotCount> g_resolveSourceSnapshotPools;
static size_t g_recordingSnapshotSlot = 0;

static TextureHandle new_resolve_source_snapshot(Extent3D size, SDL_GPUTextureFormat format) noexcept {
  auto texture =
      gpu::create_texture_2d(size.width, size.height, format, SDL_GPU_TEXTUREUSAGE_SAMPLER, "GX Copy Source Snapshot");
  return std::make_shared<TextureRef>(std::move(texture), size, format, 1, GX_TF_RGBA8);
}

static TextureHandle acquire_resolve_source_snapshot(uint32_t width, uint32_t height) noexcept {
  auto& pool = g_resolveSourceSnapshotPools[g_recordingSnapshotSlot];
  const auto format = gpu::g_graphicsConfig.surfaceFormat;

  if (pool.entry && pool.entry->size.width >= width && pool.entry->size.height >= height &&
      pool.entry->format == format) {
    return pool.entry;
  }

  const Extent3D size{
      std::max(width, pool.entry ? pool.entry->size.width : 0u),
      std::max(height, pool.entry ? pool.entry->size.height : 0u),
      1,
  };
  pool.entry = new_resolve_source_snapshot(size, format);
  return pool.entry;
}

struct ResolveSamplingPlan {
  bool needsConversion = false;
  bool needsShaderSampling = false;
  bool usesLinearSampling = false;
};

static ResolveSamplingPlan make_resolve_sampling_plan(const TextureHandle& target, GXTexFmt format,
                                                       const ClipRect& resolveRect, const Vec4<float>& sourceRect,
                                                       bool halfScale, bool copyFilterActive,
                                                       bool forceOpaqueAlpha) noexcept {
  const auto differs = [](float lhs, float rhs) { return std::abs(lhs - rhs) > 0.01f; };
  const auto differsFromDst = [&](uint32_t dst, float src) {
    return differs(static_cast<float>(dst), src);
  };
  const bool sourceMatchesIntegerRect =
      !differs(sourceRect.x(), static_cast<float>(resolveRect.x)) &&
      !differs(sourceRect.y(), static_cast<float>(resolveRect.y)) &&
      !differs(sourceRect.z(), static_cast<float>(resolveRect.width)) &&
      !differs(sourceRect.w(), static_cast<float>(resolveRect.height));
  const bool dstMatchesSource = target && !differsFromDst(target->size.width, sourceRect.z()) &&
                                !differsFromDst(target->size.height, sourceRect.w());
  const bool needsShaderSampling =
      halfScale || copyFilterActive || forceOpaqueAlpha || !sourceMatchesIntegerRect || !dstMatchesSource;
  return {
      .needsConversion = tex_copy_conv::needs_conversion(format),
      .needsShaderSampling = needsShaderSampling,
      .usesLinearSampling = halfScale || (needsShaderSampling && !copyFilterActive),
  };
}

static ClipRect calculate_resolve_snapshot_rect(const Extent3D& targetSize,
                                                const Vec4<float>& sourceRect,
                                                const ResolveSamplingPlan& samplingPlan,
                                                bool copyFilterActive) noexcept {
  const ClipRect fullTarget{
      .x = 0,
      .y = 0,
      .width = static_cast<int32_t>(targetSize.width),
      .height = static_cast<int32_t>(targetSize.height),
  };
  // Shader-sampled and converted copies can feed effects that read outside the copy rectangle
  // (MKW's DOF chain), so keep those full-target. Exact copies have a closed, crop-safe footprint.
  if (samplingPlan.needsConversion ||
      samplingPlan.needsShaderSampling ||
      targetSize.width == 0 || targetSize.height == 0 ||
      !std::isfinite(sourceRect.x()) || !std::isfinite(sourceRect.y()) ||
      !std::isfinite(sourceRect.z()) || !std::isfinite(sourceRect.w()) ||
      sourceRect.z() <= 0.0f || sourceRect.w() <= 0.0f) {
    return fullTarget;
  }

  // Linear sampling can touch one neighbor on every edge. The vertical GX
  // copy filter explicitly samples the previous and next rows.
  const int32_t haloX = samplingPlan.usesLinearSampling ? 1 : 0;
  const int32_t haloY = (samplingPlan.usesLinearSampling || copyFilterActive) ? 1 : 0;
  const int32_t targetWidth = static_cast<int32_t>(targetSize.width);
  const int32_t targetHeight = static_cast<int32_t>(targetSize.height);
  const int32_t left = std::clamp(static_cast<int32_t>(std::floor(sourceRect.x())) - haloX, 0, targetWidth - 1);
  const int32_t top = std::clamp(static_cast<int32_t>(std::floor(sourceRect.y())) - haloY, 0, targetHeight - 1);
  const int32_t right = std::clamp(static_cast<int32_t>(std::ceil(sourceRect.x() + sourceRect.z())) + haloX,
                                   left + 1, targetWidth);
  const int32_t bottom = std::clamp(static_cast<int32_t>(std::ceil(sourceRect.y() + sourceRect.w())) + haloY,
                                    top + 1, targetHeight);
  return {
      .x = left,
      .y = top,
      .width = right - left,
      .height = bottom - top,
  };
}

static void set_efb_targets(RenderPass& pass) {
  const bool msaa = gpu::g_graphicsConfig.msaaSamples > 1;
  pass.colorView = gpu::g_frameBuffer.texture;
  pass.resolveView = msaa ? gpu::g_frameBufferResolved.texture : nullptr;
  pass.depthView = gpu::g_depthBuffer.texture;
  pass.copySourceTexture = msaa ? gpu::g_frameBufferResolved.texture : gpu::g_frameBuffer.texture;
  pass.copySourceView = pass.copySourceTexture;
  pass.copySourceDepthView = gpu::g_depthBuffer.texture;
  pass.targetSize = gpu::g_frameBuffer.texture
                        ? Extent3D{gpu::g_frameBuffer.texture->width, gpu::g_frameBuffer.texture->height, 1}
                        : Extent3D{};
  pass.msaaSamples = gpu::g_graphicsConfig.msaaSamples;
}

struct OffscreenCacheKey {
  uint32_t width;
  uint32_t height;

  bool operator==(const OffscreenCacheKey& rhs) const { return width == rhs.width && height == rhs.height; }
  template <typename H>
  friend H AbslHashValue(H h, const OffscreenCacheKey& key) {
    return H::combine(std::move(h), key.width, key.height);
  }
};
struct OffscreenCacheEntry {
  gpu::TextureWithSampler color;
  gpu::TextureWithSampler depth;
};
static absl::flat_hash_map<OffscreenCacheKey, OffscreenCacheEntry> g_offscreenCache;
std::vector<TextureUpload> g_textureUploads;

static inline void push_command(CommandType type, const Command::Data& data) {
  if (!has_current_render_pass())
    UNLIKELY {
      Log.warn("Dropping command {}", magic_enum::enum_name(type));
      return;
    }
  g_renderPasses[g_currentRenderPass].commands.push_back({
      .type = type,
#ifdef AURORA_GFX_DEBUG_GROUPS
      .debugGroupStack = g_debugFrame.groups,
#endif
      .data = data,
  });
}

template <>
gx::DrawData* get_last_draw_command() {
  if (g_currentRenderPass >= g_renderPasses.size()) {
    return nullptr;
  }
  auto& last = g_renderPasses[g_currentRenderPass].commands.back();
  if (last.type != CommandType::Draw || last.data.draw.type != ShaderType::GX) {
    return nullptr;
  }
  return &last.data.draw.gx;
}

static void push_draw_command(ShaderDrawCommand data) {
  push_command(CommandType::Draw, Command::Data{.draw = data});
  ++g_drawCallCount;
}

Vec2<uint32_t> get_frame_buffer_size() noexcept {
  if (const auto& fb = gpu::g_frameBuffer.texture; fb && fb->width != 0 && fb->height != 0) {
    return {fb->width, fb->height};
  }
  const auto windowSize = window::get_window_size();
  return {windowSize.fb_width, windowSize.fb_height};
}

// Render-thread only: the frame worker recycles g_renderPasses in end_frame, so an off-thread
// read is a use-after-free rather than a stale value. Use get_frame_buffer_size() instead.
Vec2<uint32_t> get_render_target_size() noexcept {
  if (g_currentRenderPass < g_renderPasses.size()) {
    const auto& size = g_renderPasses[g_currentRenderPass].targetSize;
    return {size.width, size.height};
  }
  return get_frame_buffer_size();
}

static Viewport g_cachedViewport;
void set_viewport(const Viewport& cmd) noexcept {
  if (cmd != g_cachedViewport) {
    push_command(CommandType::SetViewport, Command::Data{.setViewport = cmd});
    g_cachedViewport = cmd;
  }
}

static ClipRect g_cachedScissor;
void set_scissor(const ClipRect& cmd) noexcept {
  if (cmd != g_cachedScissor) {
    push_command(CommandType::SetScissor, Command::Data{.setScissor = cmd});
    g_cachedScissor = cmd;
  }
}

template <>
void push_draw_command(clear::DrawData data) {
  if (data.uniformRange.size == 0) {
    ensure_staging_space({0, staging_uniform_bytes(16), 0, 0});
    const std::array clearUniform{
        std::clamp(data.depth, 0.f, 1.f),
        0.f,
        0.f,
        0.f,
    };
    data.uniformRange = push_uniform(clearUniform);
  }
  push_draw_command(ShaderDrawCommand{.type = ShaderType::Clear, .clear = data});
}

template <>
PipelineRef pipeline_ref(const clear::PipelineConfig& config) {
  return find_pipeline(ShaderType::Clear, config, [=] { return create_pipeline(config); });
}

bool add_resolve_patch(const TextureHandle& source, TextureHandle dst, uint32_t x, uint32_t y, uint32_t width,
                       uint32_t height) {
  if (!source || !dst || width == 0 || height == 0 || dst->format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM ||
      x + width > dst->size.width || y + height > dst->size.height || g_currentRenderPass >= g_renderPasses.size()) {
    return false;
  }
  // The pass that resolves `source`: the most recent one with it as its target.
  for (size_t i = g_currentRenderPass + 1; i-- > 0;) {
    auto& pass = g_renderPasses[i];
    if (pass.resolveTarget != source) {
      continue;
    }
    // One scratch texture per patch size: the blits and copies are encoded in order.
    static absl::flat_hash_map<uint64_t, TextureHandle> natives;
    auto& native = natives[(static_cast<uint64_t>(width) << 32) | height];
    if (!native) {
      native = new_conv_texture(width, height, GX_TF_RGBA8, "GX resolve patch");
    }
    const std::array blitUniform{
        0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 64.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
    };
    pass.patches.push_back({std::move(dst), native, push_uniform(blitUniform), x, y, width, height});
    return true;
  }
  return false;
}

void resolve_pass(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                  const Vec4<float>* sourceRectPixels, bool halfScale,
                  const std::array<u32, 3>* copyFilterCoefficients, bool forceOpaqueAlpha,
                  float copyFilterRowStride, bool clampTop, bool clampBottom, bool persistentCopy) {
  // Resolve current render pass
  if (!has_current_render_pass()) {
    Log.warn("Dropping resolve pass without an active render pass");
    return;
  }
  ensure_staging_space({0, 2 * staging_uniform_bytes(48), 0, 0});
  auto& prevPass = g_renderPasses[g_currentRenderPass];
  const auto targetWidth = static_cast<int32_t>(prevPass.targetSize.width);
  const auto targetHeight = static_cast<int32_t>(prevPass.targetSize.height);
  if (targetWidth <= 0 || targetHeight <= 0) {
    Log.warn("Dropping resolve pass with invalid target size {}x{}", targetWidth, targetHeight);
    return;
  }
  Vec4<float> sourceRect = sourceRectPixels != nullptr
                               ? *sourceRectPixels
                               : Vec4<float>{static_cast<float>(rect.x), static_cast<float>(rect.y),
                                             static_cast<float>(rect.width), static_cast<float>(rect.height)};
  if (targetWidth > 0 && targetHeight > 0) {
    const int32_t left = std::clamp(rect.x, 0, targetWidth - 1);
    const int32_t top = std::clamp(rect.y, 0, targetHeight - 1);
    const int32_t right = std::clamp(rect.x + rect.width, left + 1, targetWidth);
    const int32_t bottom = std::clamp(rect.y + rect.height, top + 1, targetHeight);
    rect = {
        .x = left,
        .y = top,
        .width = right - left,
        .height = bottom - top,
    };

    const float srcW = static_cast<float>(targetWidth);
    const float srcH = static_cast<float>(targetHeight);
    const float srcLeft = std::clamp(sourceRect.x(), 0.0f, srcW);
    const float srcTop = std::clamp(sourceRect.y(), 0.0f, srcH);
    const float srcRight = std::clamp(sourceRect.x() + sourceRect.z(), srcLeft, srcW);
    const float srcBottom = std::clamp(sourceRect.y() + sourceRect.w(), srcTop, srcH);
    sourceRect = {srcLeft, srcTop, std::max(srcRight - srcLeft, 1.0f), std::max(srcBottom - srcTop, 1.0f)};
  }
  prevPass.resolveTarget = std::move(texture);
  prevPass.requireReadyPipelines |= persistentCopy;
  prevPass.resolveRect = rect;
  prevPass.resolveSourceRect = sourceRect;
  prevPass.resolveFormat = resolveFormat;
  prevPass.resolveHalfScale = halfScale;
  prevPass.resolveForceOpaqueAlpha = forceOpaqueAlpha;
  prevPass.resolveCopyFilterCoefficients =
      copyFilterCoefficients != nullptr ? *copyFilterCoefficients : std::array<u32, 3>{0, 64, 0};
  prevPass.resolveCopyFilterActive = prevPass.resolveCopyFilterCoefficients[0] != 0 ||
                                     prevPass.resolveCopyFilterCoefficients[1] != 64 ||
                                     prevPass.resolveCopyFilterCoefficients[2] != 0;
  const auto samplingPlan = make_resolve_sampling_plan(
      prevPass.resolveTarget, resolveFormat, rect, sourceRect, halfScale,
      prevPass.resolveCopyFilterActive, forceOpaqueAlpha);
  prevPass.resolveNeedsConversion = samplingPlan.needsConversion;
  prevPass.resolveNeedsShaderSampling = samplingPlan.needsShaderSampling;
  prevPass.resolveLinearSampling = samplingPlan.usesLinearSampling;
  prevPass.snapshotColorResolveSource =
      !gx::is_depth_format(resolveFormat) && (clearColor || clearAlpha || clearDepth);
  Vec4<float> uniformSourceRect = sourceRect;
  float srcW = static_cast<float>(prevPass.targetSize.width);
  float srcH = static_cast<float>(prevPass.targetSize.height);
  if (prevPass.snapshotColorResolveSource) {
    prevPass.resolveSnapshotRect = calculate_resolve_snapshot_rect(
        prevPass.targetSize, sourceRect, samplingPlan, prevPass.resolveCopyFilterActive);
    prevPass.resolveSourceSnapshot = acquire_resolve_source_snapshot(
        static_cast<uint32_t>(prevPass.resolveSnapshotRect.width),
        static_cast<uint32_t>(prevPass.resolveSnapshotRect.height));
    uniformSourceRect = {
        sourceRect.x() - static_cast<float>(prevPass.resolveSnapshotRect.x),
        sourceRect.y() - static_cast<float>(prevPass.resolveSnapshotRect.y),
        sourceRect.z(), sourceRect.w()};
    srcW = static_cast<float>(prevPass.resolveSourceSnapshot->size.width);
    srcH = static_cast<float>(prevPass.resolveSourceSnapshot->size.height);
  }
  // GX's copy-clamp bits pin every vertical filter tap to the first or last source texel; without
  // it a filtered copy at internal resolution samples an unrelated EFB row as a visible border.
  const float clampTopPixels = clampTop ? uniformSourceRect.y() : 0.0f;
  const float clampBottomPixels =
      clampBottom ? uniformSourceRect.y() + uniformSourceRect.w() : srcH;
  const float clampTopUv = (clampTopPixels + 0.5f) / srcH;
  const float clampBottomUv = (clampBottomPixels - 0.5f) / srcH;
  // Push UV transform uniform for tex_copy_conv (crop region in UV space)
  const std::array resolveUniform{
      uniformSourceRect.x() / srcW,
      uniformSourceRect.y() / srcH,
      uniformSourceRect.z() / srcW,
      uniformSourceRect.w() / srcH,
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[0]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[1]),
      static_cast<float>(prevPass.resolveCopyFilterCoefficients[2]),
      prevPass.resolveCopyFilterActive ? 1.0f : 0.0f,
      prevPass.resolveForceOpaqueAlpha ? 1.0f : 0.0f,
      std::max(copyFilterRowStride, 1.0f),
      clampTopUv,
      clampBottomUv,
  };
  prevPass.resolveUniformRange = push_uniform(resolveUniform);
  const bool clearFullTarget = rect.x <= 0 && rect.y <= 0 &&
                               rect.width >= static_cast<int32_t>(prevPass.targetSize.width) &&
                               rect.height >= static_cast<int32_t>(prevPass.targetSize.height);
  const bool useAttachmentColorClear = clearFullTarget && clearColor && clearAlpha;
  const bool useAttachmentDepthClear = clearFullTarget && clearDepth;

  // Populate new render pass from previous
  const auto msaaSamples = prevPass.msaaSamples;
  RenderPass newPass{
      .colorView = prevPass.colorView,
      .resolveView = prevPass.resolveView,
      .depthView = prevPass.depthView,
      .copySourceTexture = prevPass.copySourceTexture,
      .copySourceView = prevPass.copySourceView,
      .copySourceDepthView = prevPass.copySourceDepthView,
      .targetSize = prevPass.targetSize,
      .msaaSamples = msaaSamples,
      .clearColorValue = clearColorValue,
      .clearDepthValue = clearDepthValue,
      .clearColor = useAttachmentColorClear,
      .clearDepth = useAttachmentDepthClear,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  if ((!useAttachmentColorClear && (clearColor || clearAlpha)) || (!useAttachmentDepthClear && clearDepth)) {
    // GX copy clears cover the copied EFB rectangle, not always the whole target, so use a scissored
    // clear draw unless the load op can clear all of it.
    push_draw_command(clear::DrawData{
        .pipeline = pipeline_ref(clear::PipelineConfig{
            .msaaSamples = msaaSamples,
            .clearColor = clearColor,
            .clearAlpha = clearAlpha,
            .clearDepth = clearDepth,
        }),
        .color =
            SDL_FColor{
                .r = clearColorValue.x(),
                .g = clearColorValue.y(),
                .b = clearColorValue.z(),
                .a = clearColorValue.w(),
            },
        .depth = clearDepthValue,
        .useScissor = !clearFullTarget,
        .scissor = rect,
    });
  }
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  if (!has_current_render_pass()) {
    Log.warn("Dropping palette conversion without an active render pass");
    return;
  }
  g_renderPasses[g_currentRenderPass].paletteConvs.push_back(std::move(req));
}

bool is_offscreen() noexcept { return g_inOffscreen; }

uint32_t get_sample_count() noexcept {
  if (!has_current_render_pass()) {
    return gpu::g_graphicsConfig.msaaSamples;
  }
  return g_renderPasses[g_currentRenderPass].msaaSamples;
}

void clear_caches() noexcept {
  g_offscreenCache.clear();
  // Retire rather than free; see g_retiredBindGroups.
  g_retiredBindGroups.reserve(g_retiredBindGroups.size() + g_cachedBindGroups.size());
  for (auto& entry : g_cachedBindGroups) {
    g_retiredBindGroups.emplace_back(std::move(entry.second));
  }
  g_cachedBindGroups.clear();
}

static OffscreenCacheEntry get_offscreen_textures(uint32_t width, uint32_t height) {
  OffscreenCacheKey key{width, height};
  if (const auto it = g_offscreenCache.find(key); it != g_offscreenCache.end()) {
    return it->second;
  }
  gpu::TextureWithSampler color{
      .texture = gpu::create_texture_2d(width, height, gpu::g_graphicsConfig.surfaceFormat,
                                        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                        "Offscreen Color"),
  };
  // Sampled too: a depth-format copy out of an offscreen pass reads it.
  gpu::TextureWithSampler depth{
      .texture = gpu::create_texture_2d(width, height, gpu::g_graphicsConfig.depthFormat,
                                        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER,
                                        "Offscreen Depth"),
  };
  OffscreenCacheEntry entry{
      .color = std::move(color),
      .depth = std::move(depth),
  };
  auto [insertIt, _] = g_offscreenCache.emplace(key, std::move(entry));
  return insertIt->second;
}

void begin_offscreen(uint32_t width, uint32_t height) {
  ZoneScoped;
  CHECK(g_currentRenderPass != UINT32_MAX, "begin_offscreen called outside of a frame");

  // If the current EFB pass has no resolve target, its output is unobservable.
  // Suspend it so that we can resume it after the offscreen pass.
  if (!g_inOffscreen) {
    auto& currentPass = g_renderPasses[g_currentRenderPass];
    if (!currentPass.resolveTarget) {
      g_suspendedEfbBytes = staging_usage();
      g_suspendedEfbPass = std::move(currentPass);
      g_renderPasses.pop_back();
      --g_currentRenderPass;
    }
    g_suspendedEfbViewport = g_cachedViewport;
    g_suspendedEfbScissor = g_cachedScissor;
  }

  // Create offscreen textures
  auto offscreenEntry = get_offscreen_textures(width, height);
  g_offscreenColor = std::move(offscreenEntry.color);
  g_offscreenDepth = std::move(offscreenEntry.depth);

  // Start a new pass with offscreen targets
  RenderPass newPass{
      .colorView = g_offscreenColor.texture,
      .depthView = g_offscreenDepth.texture,
      .copySourceTexture = g_offscreenColor.texture,
      .copySourceView = g_offscreenColor.texture,
      .copySourceDepthView = g_offscreenDepth.texture,
      .targetSize = {width, height, 1},
      .msaaSamples = 1,
      .clearColorValue = {0.f, 0.f, 0.f, 0.f},
      // See the RenderPass::clearDepthValue default's comment: this offscreen pass gets its own
      // depth buffer, and the farthest clear value is 0.f, not 1.f, under UseReversedZ.
      .clearDepthValue = gx::UseReversedZ ? 0.f : 1.f,
      .clearColor = true,
      .clearDepth = true,
  };
  push_render_pass(std::move(newPass));
  ++g_currentRenderPass;

  g_inOffscreen = true;

  g_cachedViewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_cachedScissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

void end_offscreen() {
  ZoneScoped;
  CHECK(g_inOffscreen, "end_offscreen called without begin_offscreen");

  g_inOffscreen = false;
  g_offscreenColor = {};
  g_offscreenDepth = {};

  // Resume suspended EFB pass, or start a new one (load existing content)
  if (g_suspendedEfbPass) {
    // Keeps its own recorded commands.
    g_renderPasses.emplace_back(std::move(*g_suspendedEfbPass));
    g_suspendedEfbPass.reset();
  } else {
    auto& pass = push_render_pass(RenderPass{});
    pass.clearColor = false;
    pass.clearDepth = false;
  }
  ++g_currentRenderPass;
  set_efb_targets(g_renderPasses[g_currentRenderPass]);

  g_cachedViewport = g_suspendedEfbViewport;
  g_cachedScissor = g_suspendedEfbScissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
}

template <>
void push_draw_command(gx::DrawData data) {
  push_draw_command(ShaderDrawCommand{.type = ShaderType::GX, .gx = data});
}

template <>
PipelineRef pipeline_ref(const gx::PipelineConfig& config) {
  return find_pipeline(ShaderType::GX, config, [=] { return create_pipeline(config); });
}

void initialize() {
  g_frameIndex = 0;
  depth_peek::initialize();
  tex_copy_conv::initialize();
  tex_palette_conv::initialize();
  texture_replacement::initialize();

  g_vertexBuffer = gpu::create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, VertexBufferSize, "Shared Vertex Buffer");
  g_indexBuffer = gpu::create_buffer(SDL_GPU_BUFFERUSAGE_INDEX, IndexBufferSize, "Shared Index Buffer");
  g_storageBuffer =
      gpu::create_buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, StorageBufferSize, "Shared Storage Buffer");
  for (size_t i = 0; i < g_stagingBuffers.size(); ++i) {
    g_stagingBuffers[i] = gpu::create_transfer_buffer(true, StagingBufferSize);
    g_uniformArenas[i] = std::make_unique<uint8_t[]>(UniformBufferSize);
  }
  currentStagingBuffer = 0;
  map_staging_buffer();

  gx::initialize();
  initialize_pipeline_cache();
}

void shutdown() {
  shutdown_pipeline_cache();
  gx::clear_shader_module_cache();
  efb_ram::shutdown();
  depth_peek::shutdown();
  tex_copy_conv::shutdown();
  tex_palette_conv::shutdown();
  texture_replacement::shutdown();
  gx::shutdown();

  g_textureUploads.clear();
  g_cachedBindGroups.clear();
  g_retiredBindGroups.clear();
  g_cachedSamplers.clear();
  g_vertexBuffer = {};
  g_indexBuffer = {};
  g_storageBuffer = {};
  if (g_stagingMapped != nullptr) {
    SDL_UnmapGPUTransferBuffer(g_device, g_stagingBuffers[currentStagingBuffer]->handle);
    g_stagingMapped = nullptr;
  }
  g_stagingBuffers.fill({});
  for (auto& arena : g_uniformArenas) {
    arena.reset();
  }
  g_batchUniforms = nullptr;
  for (auto& pool : g_resolveSourceSnapshotPools) {
    pool.entry.reset();
  }
  discard_suspended_efb_pass();
  g_renderPasses.clear();
  g_commandListPool.clear();
  g_currentRenderPass = UINT32_MAX;
  g_offscreenCache.clear();
  g_offscreenColor = {};
  g_offscreenDepth = {};
  g_inOffscreen = false;
  g_frameIndex = UINT32_MAX;
  currentStagingBuffer = 0;
}

// Maps the current slot's transfer buffer. Cycling hands back fresh memory if the GPU is still copying out of
// the slot's previous contents, so this never waits.
void map_staging_buffer() {
  if (g_stagingMapped != nullptr || !g_stagingBuffers[currentStagingBuffer]) {
    return;
  }
  g_stagingMapped = static_cast<uint8_t*>(
      SDL_MapGPUTransferBuffer(g_device, g_stagingBuffers[currentStagingBuffer]->handle, true));
  if (g_stagingMapped == nullptr) {
    Log.error("Failed to map the staging buffer: {}", SDL_GetError());
  }
}

static void unmap_staging_buffer() {
  if (g_stagingMapped != nullptr) {
    SDL_UnmapGPUTransferBuffer(g_device, g_stagingBuffers[currentStagingBuffer]->handle);
    g_stagingMapped = nullptr;
  }
}

static bool begin_frame_impl(bool clearEfb, bool capacityResume = false) {
  ZoneScoped;
  map_staging_buffer();
  if (g_stagingMapped == nullptr) {
    // The caller's retry loop drops the frame and any one-shot bakes recorded into it. Rate-limited.
    static uint32_t s_beginFrameMapFailCount = 0;
    if (s_beginFrameMapFailCount < 64 || (s_beginFrameMapFailCount & 255) == 0) {
      Log.warn("begin_frame aborted: staging buffer unmapped (frame={} occurrences={})", g_frameIndex,
               s_beginFrameMapFailCount + 1);
    }
    ++s_beginFrameMapFailCount;
    return false;
  }
  ++g_stagingEpoch;
  g_recordingSnapshotSlot = currentStagingBuffer;
  size_t bufferOffset = 0;
  const auto mapBuffer = [&](ByteBuffer& buf, uint64_t size) {
    buf = ByteBuffer{g_stagingMapped + bufferOffset, static_cast<size_t>(size)};
    bufferOffset += size;
  };
  mapBuffer(g_verts, VertexBufferSize);
  mapBuffer(g_indices, IndexBufferSize);
  mapBuffer(g_storage, StorageBufferSize);
  if constexpr (UseTextureBuffer) {
    mapBuffer(g_textureUpload, TextureUploadSize);
  }
  g_uniforms = ByteBuffer{g_uniformArenas[currentStagingBuffer].get(), static_cast<size_t>(UniformBufferSize)};

  g_drawCallCount = 0;
  g_mergedDrawCallCount = 0;
  if (clearEfb) {
    gx::begin_frame_interpolation();
  }
  discard_suspended_efb_pass();
  if (!capacityResume) gpu::clear_present_source_override();

  push_render_pass(RenderPass{});
  set_efb_targets(g_renderPasses[0]);
  g_renderPasses[0].clearColorValue = gx::g_gxState.clearColor;
  g_renderPasses[0].clearDepthValue = gx::clear_depth_value();
  g_renderPasses[0].clearColor = clearEfb;
  g_renderPasses[0].clearDepth = clearEfb;
  g_currentRenderPass = 0;
  // Refresh paired render viewport/scissor from logical state in case the FB size changed.
  const auto mappedRenderState = gx::map_logical_render_state();
  const bool renderStateChanged = gx::g_gxState.renderViewport != mappedRenderState.viewport ||
                                  gx::g_gxState.renderScissor != mappedRenderState.scissor;
  gx::g_gxState.renderViewport = mappedRenderState.viewport;
  gx::g_gxState.renderScissor = mappedRenderState.scissor;
  gx::g_gxState.stateDirty = gx::g_gxState.stateDirty || renderStateChanged;
  g_cachedViewport = mappedRenderState.viewport;
  g_cachedScissor = mappedRenderState.scissor;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_cachedScissor});
  begin_pipeline_frame();
  return true;
}

bool begin_frame() { return begin_frame_impl(true); }

bool resume_frame() { return begin_frame_impl(false); }

void abort_frame() noexcept {
  efb_ram::cancel();
  efb_ram::abort_async();
  g_verts.release();
  g_uniforms.release();
  g_indices.release();
  g_storage.release();
  if constexpr (UseTextureBuffer) {
    g_textureUploads.clear();
    g_textureUpload.release();
  }
  if (g_stagingMapped != nullptr) {
    // Pending interpolation tasks hold raw pointers into the slot's uniform arena;
    // they must be dropped before the slot is rotated.
    gx::drop_pending_frame_interpolation_uniforms();
    unmap_staging_buffer();
    currentStagingBuffer = (currentStagingBuffer + 1) % g_stagingBuffers.size();
    map_staging_buffer();
  }
  recycle_render_passes(g_renderPasses);
  g_currentRenderPass = UINT32_MAX;
  discard_suspended_efb_pass();
  g_inOffscreen = false;
  ++gx::g_arrayUploadEpoch;  // staging rotated: earlier array uploads are gone
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  gpu::clear_present_source_override();
  end_pipeline_frame();
}

static void end_batch_impl(gpu::Encoder& cmd, bool advanceFrame) {
  ZoneScoped;
  ASSERT(!advanceFrame || !g_inOffscreen, "end_frame called while offscreen rendering is active");
  if (advanceFrame) {
    gx::finalize_frame_interpolation();
  } else {
    // Mid-frame batch split: the staging buffer is about to be unmapped and rotated, so pending
    // interpolation tasks pointing into it would dangle. Tie the clear to the rotation itself.
    gx::drop_pending_frame_interpolation_uniforms();
  }
  g_uniforms.append_zeroes(gx::MaxUniformSize); // Pad the end of the buffer
  const auto used = staging_usage();
  for (unsigned i = 0; i < used.size(); ++i) g_stagingHighWater[i] = std::max(g_stagingHighWater[i], used[i]);
  uint64_t bufferOffset = 0;
  const auto& staging = *g_stagingBuffers[currentStagingBuffer];
  const auto writeBuffer = [&](ByteBuffer& buf, const gpu::Buffer& out, uint64_t size) {
    const auto writeSize = buf.size(); // Only need to copy this many bytes
    if (writeSize > 0) {
      gpu::upload_to_buffer(cmd, staging, static_cast<uint32_t>(bufferOffset), *out, 0,
                            static_cast<uint32_t>(AURORA_ALIGN(writeSize, 4)));
      buf.release();
    }
    bufferOffset += size;
    return writeSize;
  };
  using StageClock = std::chrono::steady_clock;
  const auto stageMs = [](StageClock::time_point t) {
    return std::chrono::duration<double, std::milli>(StageClock::now() - t).count();
  };
  auto stageMark = StageClock::now();
  unmap_staging_buffer();
  g_lastStagingUnmapMs = stageMs(stageMark);
  stageMark = StageClock::now();
  g_stats.drawCallCount = g_drawCallCount;
  g_stats.mergedDrawCallCount = g_mergedDrawCallCount;
  g_stats.lastVertSize = writeBuffer(g_verts, g_vertexBuffer, VertexBufferSize);
  g_stats.lastIndexSize = writeBuffer(g_indices, g_indexBuffer, IndexBufferSize);
  g_stats.lastStorageSize = writeBuffer(g_storage, g_storageBuffer, StorageBufferSize);
  g_stats.lastUniformSize = g_uniforms.size();
  g_uniforms.release();
  g_batchUniforms = g_uniformArenas[currentStagingBuffer].get();
  if constexpr (UseTextureBuffer) {
    g_stats.lastTextureUploadSize = g_textureUpload.size();
    for (const auto& item : g_textureUploads) {
      gpu::upload_to_texture(cmd, staging, static_cast<uint32_t>(item.offset + bufferOffset), item.pixelsPerRow,
                             item.rowsPerImage, *item.texture, 0, 0, item.width, item.height, item.mip, item.layer);
    }
    g_textureUploads.clear();
    g_textureUpload.release();
  }
  currentStagingBuffer = (currentStagingBuffer + 1) % g_stagingBuffers.size();
  g_lastStagingCopyMs = stageMs(stageMark);
  stageMark = StageClock::now();
  map_staging_buffer();
  g_lastStagingMapMs = stageMs(stageMark);
  g_currentRenderPass = UINT32_MAX;
  ++gx::g_arrayUploadEpoch;  // staging rotated: earlier array uploads are gone
  for (auto& array : gx::g_gxState.arrays) {
    array.cachedRange = {};
  }
  end_pipeline_frame();
  if (advanceFrame) {
    ++g_frameIndex;
  }
}

void end_frame(gpu::Encoder& cmd) { end_batch_impl(cmd, true); }

void end_batch(gpu::Encoder& cmd) { end_batch_impl(cmd, false); }

void split_staging_batch() {
  // Never called under the decoder's renderer lock: the worker needs that lock
  // to reach DONE. FIFO admission yields its unconsumed command first. The GX thread's batch-wide hold of it
  // (gx_worker.cpp run_batch) is dropped here for the wait.
  const gx::worker::BatchLockRelease batchLockRelease;
  aurora::wait_for_frame_worker();
  std::lock_guard gpuLock(aurora::renderer_gpu_mutex());
  if (!has_current_render_pass())
    throw StagingCapacityError("Cannot split staging outside an active render pass");
  {  // SPM diagnostic (env SPM_PASS_LOG): which staging buffer forced the split (V/U/I/S bytes used)
    static const bool passLog = std::getenv("SPM_PASS_LOG") != nullptr;
    static int logged = 0;
    if (passLog && logged++ < 20) {
      const auto u = staging_usage();
      std::fprintf(stderr, "[spmsplit] V %llu U %llu I %llu S %llu\n", (unsigned long long)u[0], (unsigned long long)u[1],
                   (unsigned long long)u[2], (unsigned long long)u[3]);
    }
  }
  gx::mark_frame_interpolation_replay_unsafe();
  const bool offscreen = g_inOffscreen;
  const auto viewport = g_cachedViewport;
  const auto scissor = g_cachedScissor;
  const auto renderViewport = gx::g_gxState.renderViewport;
  const auto renderScissor = gx::g_gxState.renderScissor;
  const auto& active = g_renderPasses[g_currentRenderPass];
  RenderPass continuation{
      .colorView = active.colorView, .resolveView = active.resolveView,
      .depthView = active.depthView, .copySourceTexture = active.copySourceTexture,
      .copySourceView = active.copySourceView, .copySourceDepthView = active.copySourceDepthView,
      .targetSize = active.targetSize, .msaaSamples = active.msaaSamples,
      .clearColor = false, .clearDepth = false,
      .requireReadyPipelines = active.requireReadyPipelines || offscreen,
  };
  auto suspended = std::move(g_suspendedEfbPass);
  g_suspendedEfbPass.reset();
  std::array<std::vector<uint8_t>, 4> retained;
  std::array<ByteBuffer*, 4> buffers{&g_verts, &g_uniforms, &g_indices, &g_storage};
  if (suspended) {
    for (unsigned i = 0; i < buffers.size(); ++i) {
      if (g_suspendedEfbBytes[i])
        retained[i].assign(buffers[i]->data(), buffers[i]->data() + g_suspendedEfbBytes[i]);
    }
  }
  gpu::Encoder encoder;
  end_batch(encoder);
  render(encoder);
  aurora::submit_staging_commands(std::move(encoder));
  after_submit();
  if (!begin_frame_impl(false, true))
    throw StagingCapacityError("Staging remap failed after capacity submission");
  recycle_render_passes(g_renderPasses);
  push_render_pass(std::move(continuation));
  g_currentRenderPass = 0;
  g_suspendedEfbPass = std::move(suspended);
  for (unsigned i = 0; i < buffers.size(); ++i) {
    if (!retained[i].empty()) buffers[i]->append(retained[i].data(), retained[i].size());
  }
  g_inOffscreen = offscreen;
  g_cachedViewport = viewport;
  g_cachedScissor = scissor;
  gx::g_gxState.renderViewport = renderViewport;
  gx::g_gxState.renderScissor = renderScissor;
  gx::g_gxState.stateDirty = true;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = viewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = scissor});
  ++g_stagingSplitCount;
}

uint32_t current_frame() noexcept { return g_frameIndex; }

// The only place that erases from g_cachedBindGroups, whose handles the frame being encoded still
// holds, so it runs in the seal prologue with the renderer mutex held and the producer excluded.
void expire_bind_group_cache() noexcept {
  if (g_cachedBindGroups.empty() || g_frameIndex == UINT32_MAX || g_frameIndex % BindGroupCacheSweepPeriod != 0) {
    return;
  }

  ZoneScoped;
  for (auto it = g_cachedBindGroups.begin(); it != g_cachedBindGroups.end();) {
    if (g_frameIndex - it->second.lastUsedFrame > BindGroupCacheRetainFrames) {
      g_cachedBindGroups.erase(it++);
    } else {
      ++it;
    }
  }
}

// Debug labels for the render passes. Formatting them per pass per slot costs a heap-allocating
// fmt::format for a name nothing reads outside a capture.
static const char* render_pass_label(u32 index) noexcept {
  static constexpr std::array<const char*, 16> kRenderPassLabels{
      "Render pass 0",  "Render pass 1",  "Render pass 2",  "Render pass 3",
      "Render pass 4",  "Render pass 5",  "Render pass 6",  "Render pass 7",
      "Render pass 8",  "Render pass 9",  "Render pass 10", "Render pass 11",
      "Render pass 12", "Render pass 13", "Render pass 14", "Render pass 15",
  };
  return index < kRenderPassLabels.size() ? kRenderPassLabels[index] : "Render pass";
}

static void render_pass_impl(const PassContext& ctx, const std::vector<RenderPass>& passes, u32 idx,
                             int32_t interpolatedFrame, DebugFrameData& debugFrame);

static void render_impl(std::vector<RenderPass>& renderPasses, gpu::Encoder& cmd, const uint8_t* uniforms,
                        int32_t interpolatedFrame, bool finalize, DebugFrameData& debugFrame,
                        const depth_peek::FrameMapping& depthMapping, bool recycle = true) {
  ZoneScoped;
  // Palette conversions and EFB copies run in every slot, in pass order: replay slots are encoded
  // before the native render, so skipping them made a slot sample copies this frame had not made yet
  // (the 2-1 mansion behind the 3D mountain layers vanished in every in-between frame, issue #24).
  const bool encodeTextureBakes = true;
  const bool nativeSlot = interpolatedFrame < 0;
  {  // SPM diagnostic (env SPM_PASS_LOG): every 300th frame, the render passes with their clears and command counts
    static const bool passLog = std::getenv("SPM_PASS_LOG") != nullptr;
    static u32 calls = 0, logged = 0;
    if (passLog && nativeSlot && ++calls % 300 == 0 && logged++ < 12) {
      std::fprintf(stderr, "[spmpass] frame call %u: %zu passes\n", calls, renderPasses.size());
      for (u32 i = 0; i < renderPasses.size(); ++i) {
        const auto& p = renderPasses[i];
        std::fprintf(stderr, "[spmpass]   #%u clearColor %d clearDepth %d cmds %zu size %ux%u resolve %d copySrc %d rgba %.2f,%.2f,%.2f,%.2f\n", i,
                     p.clearColor, p.clearDepth, p.commands.size(), p.targetSize.width, p.targetSize.height, p.resolveTarget ? 1 : 0,
                     p.copySourceTexture ? 1 : 0, p.clearColorValue.x(), p.clearColorValue.y(), p.clearColorValue.z(), p.clearColorValue.w());
      }
    }
  }
  for (u32 i = 0; i < renderPasses.size(); ++i) {
    const auto& passInfo = renderPasses[i];
    if (encodeTextureBakes) {
      for (const auto& conv : passInfo.paletteConvs) {
        tex_palette_conv::run(cmd, conv);
      }
    }
    const bool hasRenderWork = passInfo.clearColor || passInfo.clearDepth || !passInfo.commands.empty();
    if (i == renderPasses.size() - 1) {
      ASSERT(!passInfo.resolveTarget, "Final render pass must not have resolve target");
    } else if (!(passInfo.resolveTarget && encodeTextureBakes) && !hasRenderWork) {
      // Skip only empty intermediate passes: offscreen and scratch passes with resolves still have to
      // run for later samplers, and on a replay slot a resolve-only pass has nothing to encode.
      continue;
    }

    const SDL_GPUColorTargetInfo colorTarget{
        .texture = passInfo.colorView->handle,
        .clear_color =
            SDL_FColor{
                .r = passInfo.clearColorValue.x(),
                .g = passInfo.clearColorValue.y(),
                .b = passInfo.clearColorValue.z(),
                .a = passInfo.clearColorValue.w(),
            },
        .load_op = passInfo.clearColor ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
        .store_op = passInfo.resolveView ? SDL_GPU_STOREOP_RESOLVE_AND_STORE : SDL_GPU_STOREOP_STORE,
        .resolve_texture = passInfo.resolveView ? passInfo.resolveView->handle : nullptr,
    };
    const SDL_GPUDepthStencilTargetInfo depthTarget{
        .texture = passInfo.depthView->handle,
        .clear_depth = passInfo.clearDepthValue,
        .load_op = passInfo.clearDepth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD,
        .store_op = SDL_GPU_STOREOP_STORE,
        .stencil_load_op = SDL_GPU_LOADOP_DONT_CARE,
        .stencil_store_op = SDL_GPU_STOREOP_DONT_CARE,
    };

    SDL_PushGPUDebugGroup(cmd.cmd(), render_pass_label(i));
    SDL_GPURenderPass* pass = cmd.begin_render_pass(&colorTarget, 1, &depthTarget);
    const PassContext ctx{
        .cmd = cmd.cmd(),
        .pass = pass,
        .uniforms = uniforms,
        .targetWidth = passInfo.targetSize.width,
        .targetHeight = passInfo.targetSize.height,
    };
    render_pass_impl(ctx, renderPasses, i, interpolatedFrame, debugFrame);
    SDL_EndGPURenderPass(pass);
    SDL_PopGPUDebugGroup(cmd.cmd());

    if (finalize && i == renderPasses.size() - 1) {
      depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView, passInfo.msaaSamples, depthMapping);
    }

    if (passInfo.resolveTarget) {
      const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
      gpu::Texture resolveSource = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView;
      if (passInfo.snapshotColorResolveSource && passInfo.resolveSourceSnapshot) {
        gpu::copy_texture_to_texture(cmd, *passInfo.copySourceTexture,
                                     static_cast<uint32_t>(passInfo.resolveSnapshotRect.x),
                                     static_cast<uint32_t>(passInfo.resolveSnapshotRect.y),
                                     *passInfo.resolveSourceSnapshot->texture, 0, 0,
                                     static_cast<uint32_t>(passInfo.resolveSnapshotRect.width),
                                     static_cast<uint32_t>(passInfo.resolveSnapshotRect.height));
        resolveSource = passInfo.resolveSourceSnapshot->texture;
      }
      if (isDepth && passInfo.msaaSamples > 1) {
        Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
      }
      const tex_copy_conv::ConvRequest convReq{
          .fmt = passInfo.resolveFormat,
          .src = resolveSource,
          .uniformRange = passInfo.resolveUniformRange,
          .dst = passInfo.resolveTarget,
          .sampleFilter = passInfo.resolveLinearSampling
                              ? tex_copy_conv::SampleFilter::Linear
                              : tex_copy_conv::SampleFilter::Nearest,
          .forceOpaqueAlpha = passInfo.resolveForceOpaqueAlpha,
      };
      if (passInfo.resolveNeedsConversion) {
        tex_copy_conv::run(cmd, uniforms, convReq);
      } else if (passInfo.resolveNeedsShaderSampling) {
        tex_copy_conv::blit(cmd, uniforms, convReq);
      } else {
        const uint32_t originX = static_cast<uint32_t>(
            passInfo.resolveRect.x - (passInfo.snapshotColorResolveSource ? passInfo.resolveSnapshotRect.x : 0));
        const uint32_t originY = static_cast<uint32_t>(
            passInfo.resolveRect.y - (passInfo.snapshotColorResolveSource ? passInfo.resolveSnapshotRect.y : 0));
        gpu::copy_texture_to_texture(cmd, *resolveSource, originX, originY, *passInfo.resolveTarget->texture, 0, 0,
                                     static_cast<uint32_t>(passInfo.resolveRect.width),
                                     static_cast<uint32_t>(passInfo.resolveRect.height));
      }
      for (const auto& patch : passInfo.patches) {
        if (!patch.dst || !patch.native) {
          continue;
        }
        tex_copy_conv::blit(cmd, uniforms,
                            tex_copy_conv::ConvRequest{
                                .fmt = GX_TF_RGBA8,
                                .src = passInfo.resolveTarget->texture,
                                .uniformRange = patch.uniformRange,
                                .dst = patch.native,
                                .sampleFilter = tex_copy_conv::SampleFilter::Linear,
                            });
        gpu::copy_texture_to_texture(cmd, *patch.native->texture, 0, 0, *patch.dst->texture, patch.x, patch.y,
                                     patch.width, patch.height);
      }
    }
  }
  if (finalize && recycle) {
    recycle_render_passes(renderPasses);
  }

#if defined(AURORA_GFX_DEBUG_GROUPS)
  if (finalize && !debugFrame.groups.empty()) {
    for (auto it = debugFrame.groups.rbegin(); it != debugFrame.groups.rend(); ++it) {
      Log.warn("Debug group was not popped at end of frame: {}", *it);
    }
    debugFrame.groups.clear();
  }

  if (finalize && debugFrame.markers.size() > 0) {
    debugFrame.markers.clear();
  }
#endif
}

void seal_frame(SealedFrame& out) noexcept {
  out.data().depthMapping = depth_peek::capture_frame_mapping();
  out.data().uniforms = g_batchUniforms;
  ZoneScoped;
  // The encode that could still have been holding these has completed: the
  // producer joins the worker's DONE phase before it seals another frame.
  g_retiredBindGroups.clear();
  auto& passes = out.data().passes;
  // The previous cycle already recycled these, so this normally just hands the empty vector, its
  // capacity included, back to the producer.
  recycle_render_passes(passes);
  passes.swap(g_renderPasses);
#ifdef AURORA_GFX_DEBUG_GROUPS
  // Marker indices and unmatched-group warnings belong to these detached passes.
  // The next producer frame must not modify strings still read by this encoder.
  auto& debug = out.data().debug;
  debug.groups.clear();
  debug.markers.clear();
  debug.groups.swap(g_debugFrame.groups);
  debug.markers.swap(g_debugFrame.markers);
#endif
  g_currentRenderPass = UINT32_MAX;
}

void render(SealedFrame& frame, gpu::Encoder& cmd, int32_t interpolatedFrame, bool finalize, bool recycle) {
  render_impl(frame.data().passes, cmd, frame.data().uniforms, interpolatedFrame, finalize, frame.data().debug,
              frame.data().depthMapping, recycle);
}

void recycle_sealed_passes(SealedFrame& frame) noexcept { recycle_render_passes(frame.data().passes); }

void render(gpu::Encoder& cmd, int32_t interpolatedFrame, bool finalize) {
  render_impl(g_renderPasses, cmd, g_batchUniforms, interpolatedFrame, finalize, g_debugFrame,
              depth_peek::capture_frame_mapping());
  if (finalize) {
    g_currentRenderPass = UINT32_MAX;
    expire_bind_group_cache();
  }
}

void after_submit() noexcept {
  depth_peek::after_submit();
  efb_ram::after_submit();
}

static void render_pass_impl(const PassContext& ctx, const std::vector<RenderPass>& renderPasses, u32 idx,
                             int32_t interpolatedFrame, DebugFrameData& debugFrame) {
  // Per-invocation, not per-process: two encoders can be recording at once.
  gx::DrawEncodeState encodeState{};
  const auto& emptyTextures = *gx::g_emptyTextureBindings;
  encodeState.boundTextureBindGroup = &emptyTextures;
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif

  // Vertex data and storage arrays are bound for the whole pass; every GX draw pulls from them.
  const std::array storageBuffers{g_vertexBuffer->handle, g_storageBuffer->handle};
  SDL_BindGPUVertexStorageBuffers(ctx.pass, 0, storageBuffers.data(), storageBuffers.size());
  SDL_BindGPUFragmentSamplers(ctx.pass, 0, emptyTextures.raw.data(), emptyTextures.count);

  for (const auto& cmd : renderPasses[idx].commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        SDL_PopGPUDebugGroup(ctx.cmd);
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        SDL_PushGPUDebugGroup(ctx.cmd, cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      // WebGPU requires 0 <= minDepth <= maxDepth <= 1. vp.znear/vp.zfar are in GX's own distance
      // terms (0 = near); under UseReversedZ the host depth-buffer storage direction is flipped
      // (near = 1, far = 0), so this range has to be remapped through 1-x the same way the
      // projection matrix, depth compare function, and clear value all are - a plain min/max clamp
      // (the previous code here) maps a *restricted* range (e.g. a viewport deliberately narrowed
      // to force something to draw "in front of everything") to the wrong end of the buffer: what
      // should land near the near-storage-extreme (1.0) instead lands near the far-storage-extreme
      // (0.0), so anything else drawn afterward at its true depth wins the compare test and the
      // "in front" geometry silently vanishes. A full [0,1] viewport is unaffected either way,
      // which is why this only broke specific elements, not the whole scene. Matches upstream
      // aurora's apply_viewport (lib/gfx/encoding.cpp) exactly.
      const float minDepth = gx::UseReversedZ ? 1.0f - vp.zfar : vp.znear;
      const float maxDepth = gx::UseReversedZ ? 1.0f - vp.znear : vp.zfar;
      const SDL_GPUViewport viewport{
          .x = vp.left, .y = vp.top, .w = vp.width, .h = vp.height, .min_depth = minDepth, .max_depth = maxDepth};
      SDL_SetGPUViewport(ctx.pass, &viewport);
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      const auto& size = renderPasses[idx].targetSize;
      const auto left = std::clamp(sc.x, 0, static_cast<int32_t>(size.width));
      const auto top = std::clamp(sc.y, 0, static_cast<int32_t>(size.height));
      const auto right =
          std::clamp(sc.x + sc.width, left, static_cast<int32_t>(size.width));
      const auto bottom =
          std::clamp(sc.y + sc.height, top, static_cast<int32_t>(size.height));
      const SDL_Rect rect{left, top, right - left, bottom - top};
      SDL_SetGPUScissor(ctx.pass, &rect);
    } break;
    case CommandType::Draw: {
      const auto& draw = cmd.data.draw;
      switch (draw.type) {
      case ShaderType::GX: {
        const gfx::Range* uniformOverride = nullptr;
        if (interpolatedFrame >= 0 &&
            static_cast<size_t>(interpolatedFrame) < draw.gx.interpolatedUniformRanges.size() &&
            draw.gx.interpolatedUniformRanges[interpolatedFrame].size != 0) {
          uniformOverride = &draw.gx.interpolatedUniformRanges[interpolatedFrame];
        }
        gx::render(draw.gx, ctx, encodeState, renderPasses[idx].requireReadyPipelines, uniformOverride);
      } break;
      case ShaderType::Clear:
        clear::render(draw.clear, ctx, encodeState.currentPipeline);
        break;
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      SDL_InsertGPUDebugLabel(ctx.cmd, debugFrame.markers[cmd.data.debugMarkerIndex].c_str());
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    SDL_PopGPUDebugGroup(ctx.cmd);
  }
#endif
}

bool bind_pipeline(PipelineRef ref, const PassContext& ctx, PipelineRef& currentPipeline, bool requireReady) {
  if (ref == currentPipeline) {
    return true;
  }
  gpu::GraphicsPipeline pipeline;
  bool pipelineReady;
  if (!skip_unready_pipelines()) {
    pipelineReady = wait_pipeline(ref, pipeline);
  } else if (requireReady) {
    // The pass resolves into a persistent texture (a one-shot bake such as MKW's minimap), so a
    // skipped draw would never be re-issued. These run behind loads, not mid-race.
    pipelineReady = wait_pipeline_for_persistent_pass(ref, pipeline);
  } else {
    pipelineReady = try_pipeline(ref, pipeline);
  }
  if (!pipelineReady || !pipeline) {
    return false;
  }
  SDL_BindGPUGraphicsPipeline(ctx.pass, pipeline->handle);
  currentPipeline = ref;
  return true;
}

static inline Range push(ByteBuffer& target, const uint8_t* data, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(alignment);
  } else {
    target.append(data, length);
    if (padding > 0) {
      target.append_zeroes(padding);
    }
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
static inline Range map(ByteBuffer& target, size_t length, size_t alignment) {
  size_t padding = 0;
  if (alignment != 0) {
    const size_t remainder = length % alignment;
    if (remainder != 0) {
      padding = alignment - remainder;
    }
  }
  auto begin = target.size();
  if (length == 0) {
    length = alignment;
    target.append_zeroes(length);
  } else {
    // The caller fills [0, length); only the alignment padding needs clearing,
    // otherwise it hands the next frame's readers whatever was there before.
    target.append_uninitialized(length);
    target.append_zeroes(padding);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length + padding)};
}
Range push_verts(const uint8_t* data, size_t length) { return push(g_verts, data, length, 0); }
Range push_indices(const uint8_t* data, size_t length) { return push(g_indices, data, length, 0); }
Range push_offset_indices(const uint16_t* src, size_t count, uint16_t offset) {
  if (count == 0) return push(g_indices, nullptr, 0, 0);
  // push(g_indices, data, count * 2, 0) without the intermediate buffer: no alignment, so no padding.
  const size_t length = count * sizeof(uint16_t);
  const auto begin = g_indices.size();
  g_indices.append_uninitialized(length);
  uint16_t* dst = reinterpret_cast<uint16_t*>(g_indices.data() + begin);
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<uint16_t>(src[i] + offset);
  }
  return {static_cast<uint32_t>(begin), static_cast<uint32_t>(length)};
}
Range push_uniform(const uint8_t* data, size_t length) { return push(g_uniforms, data, length, UniformAlignment); }
Range push_storage(const uint8_t* data, size_t length) { return push(g_storage, data, length, StorageAlignment); }
Range push_texture_data(const uint8_t* data, size_t length, u32 bytesPerRow, u32 rowsPerImage) {
  // For CopyBufferToTexture, we need an alignment of 256 per row (see Dawn kTextureBytesPerRowAlignment)
  const auto copyBytesPerRow = AURORA_ALIGN(bytesPerRow, 256);
  const auto range = map(g_textureUpload, copyBytesPerRow * rowsPerImage, 0);
  u8* dst = g_textureUpload.data() + range.offset;
  for (u32 i = 0; i < rowsPerImage; ++i) {
    memcpy(dst, data, bytesPerRow);
    data += bytesPerRow;
    dst += copyBytesPerRow;
  }
  return range;
}
std::pair<ByteBuffer, Range> map_verts(size_t length) {
  const auto range = map(g_verts, length, 4);
  return {ByteBuffer{g_verts.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_indices(size_t length) {
  const auto range = map(g_indices, length, 4);
  return {ByteBuffer{g_indices.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> map_uniform(size_t length) {
  const auto range = map(g_uniforms, length, UniformAlignment);
  return {ByteBuffer{g_uniforms.data() + range.offset, range.size}, range};
}
std::pair<ByteBuffer, Range> copy_uniform(Range source) {
  const auto destination = map(g_uniforms, source.size, UniformAlignment);
  std::memcpy(g_uniforms.data() + destination.offset, g_uniforms.data() + source.offset, source.size);
  return {ByteBuffer{g_uniforms.data() + destination.offset, destination.size}, destination};
}
const uint8_t* staged_bytes(int buffer, const Range& range) noexcept {
  const ByteBuffer* buffers[] = {&g_verts, &g_uniforms, &g_indices, &g_storage};
  const ByteBuffer& target = *buffers[buffer & 3];
  return range.offset + range.size <= target.size() ? target.data() + range.offset : nullptr;
}
std::pair<ByteBuffer, Range> map_storage(size_t length) {
  const auto range = map(g_storage, length, StorageAlignment);
  return {ByteBuffer{g_storage.data() + range.offset, range.size}, range};
}

TextureBindingsRef texture_bindings_ref(TextureBindings&& bindings) {
  const auto id = hash_texture_bindings(bindings);
  const auto it = g_cachedBindGroups.find(id);
  if (it == g_cachedBindGroups.end()) {
    g_cachedBindGroups.emplace(id, CachedBindGroup{
                                       .bindings = std::make_unique<TextureBindings>(std::move(bindings)),
                                       .lastUsedFrame = g_frameIndex,
                                   });
  } else {
    it->second.lastUsedFrame = g_frameIndex;
  }
  return id;
}

std::pair<TextureBindingsRef, const TextureBindings*> find_texture_bindings(const TextureBindingsRaw& raw,
                                                                           uint32_t count) {
  const auto id = hash_texture_bindings(raw, count);
  const auto it = g_cachedBindGroups.find(id);
  if (it == g_cachedBindGroups.end()) {
    return {id, nullptr};
  }
  it->second.lastUsedFrame = g_frameIndex;  // as texture_bindings_ref for bindings it already holds
  return {id, it->second.bindings.get()};
}

const TextureBindings& find_texture_bindings(TextureBindingsRef id) {
  const auto it = g_cachedBindGroups.find(id);
  CHECK(it != g_cachedBindGroups.end(), "find_texture_bindings: failed to locate {:x}", id);
  return *it->second.bindings;
}

const gpu::Sampler& sampler_ref(const SDL_GPUSamplerCreateInfo& descriptor) {
  const auto id = hash_sampler(descriptor);
  auto it = g_cachedSamplers.find(id);
  if (it == g_cachedSamplers.end()) {
    it = g_cachedSamplers.try_emplace(id, gpu::create_sampler(descriptor)).first;
  }
  return it->second;
}

uint32_t align_uniform(uint32_t value) { return AURORA_ALIGN(value, UniformAlignment); }

void insert_debug_marker(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  auto idx = g_debugFrame.markers.size();
  g_debugFrame.markers.emplace_back(std::move(label));
  push_command(CommandType::DebugMarker, {.debugMarkerIndex = idx});
#endif
}

} // namespace aurora::gfx

void aurora::gfx::push_debug_group(std::string label) {
#if defined(AURORA_GFX_DEBUG_GROUPS)
  g_debugFrame.groups.push_back(std::move(label));
#endif
}
void aurora_push_debug_group(const char* label) {
#ifdef AURORA_GFX_DEBUG_GROUPS
  aurora::gfx::g_debugFrame.groups.emplace_back(label);
#endif
}
void aurora_pop_debug_group() {
#ifdef AURORA_GFX_DEBUG_GROUPS
  if (aurora::gfx::g_debugFrame.groups.empty()) {
    aurora::gfx::Log.error("Debug group stack underflowed!");
    return;
  }

  aurora::gfx::g_debugFrame.groups.pop_back();
#endif
}

const AuroraStats* aurora_get_stats() { return &aurora::gfx::g_stats; }
