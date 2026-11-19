#include "depth_peek.hpp"

#include "../dolphin/vi/vi_internal.hpp"
#include "../gx/gx.hpp"
#include "../gpu/shader.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <string>

#include <aurora/profiling.hpp>

namespace aurora::gfx::depth_peek {
namespace {
Module Log("aurora::gfx::depth_peek");

using gpu::g_device;

using Clock = std::chrono::steady_clock;

constexpr size_t SlotCount = 3;
constexpr uint32_t WorkgroupSizeX = 8;
constexpr uint32_t WorkgroupSizeY = 8;
constexpr uint32_t DepthPeekSnapshotHz = 30;
constexpr auto SnapshotInterval = std::chrono::nanoseconds{1'000'000'000 / DepthPeekSnapshotHz};

struct Params {
  uint32_t dstWidth = 0;
  uint32_t dstHeight = 0;
  uint32_t srcWidth = 0;
  uint32_t srcHeight = 0;
  float offsetX = 0.f;
  float offsetY = 0.f;
  float scaleX = 1.f;
  float scaleY = 1.f;
};
static_assert(sizeof(Params) == 32);

struct LatestSnapshot {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint32_t> data;
};

enum class SlotState : uint8_t {
  Available,
  CopySubmitted,
  MapPending,
};

struct Slot {
  gpu::Buffer storageBuffer;
  gpu::TransferBuffer readbackBuffer;
  SDL_GPUFence* fence = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t byteSize = 0;
  SlotState state = SlotState::Available;
};

std::array<Slot, SlotCount> g_slots;
size_t g_nextSlot = 0;
gpu::ComputePipeline g_pipeline;
gpu::Sampler g_sampler;
bool g_msaaWarned = false;
bool g_snapshotRequested = false;
Clock::time_point g_nextSnapshotTime;
LatestSnapshot g_latest;
std::mutex g_mutex;

constexpr std::string_view ParamsMembers = "uvec2 dstSize; uvec2 srcSize; vec2 offset; vec2 scale;";

constexpr std::string_view ReversedZBody = R"(
uint gx_z24(float depth) {
  return min(uint(clamp(1.0 - depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}
)"sv;

constexpr std::string_view ForwardZBody = R"(
uint gx_z24(float depth) {
  return min(uint(clamp(depth, 0.0, 1.0) * 16777215.0 + 0.5), 0x00ffffffu);
}
)"sv;

constexpr std::string_view ShaderMain = R"(
  if (gid.x >= params.dstSize.x || gid.y >= params.dstSize.y) {
    return;
  }

  vec2 dstCenter = vec2(gid.xy) + vec2(0.5, 0.5);
  ivec2 srcPixel = clamp(ivec2(floor(params.offset + dstCenter * params.scale)), ivec2(0, 0),
                         ivec2(params.srcSize) - ivec2(1, 1));
  float depth = LOAD_DEPTH(src, srcPixel);
  out_z[gid.y * params.dstSize.x + gid.x] = gx_z24(depth);
)"sv;

gpu::shader::Interface shader_interface() {
  gpu::shader::Interface iface;
  iface.compute = true;
  iface.uniforms = {{"params", std::string{ParamsMembers}}};
  iface.rwStorageBuffers = {"out_z"};
  iface.textures = {{"src", gpu::shader::TextureKind::Depth}};
  iface.workgroup[0] = WorkgroupSizeX;
  iface.workgroup[1] = WorkgroupSizeY;
  iface.workgroup[2] = 1;
  return iface;
}

gpu::ComputePipeline create_pipeline(const char* label) {
  return gpu::shader::create_compute(shader_interface(), gx::UseReversedZ ? ReversedZBody : ForwardZBody, ShaderMain,
                                     label);
}

gpu::Sampler create_sampler() {
  constexpr SDL_GPUSamplerCreateInfo info{
      .min_filter = SDL_GPU_FILTER_NEAREST,
      .mag_filter = SDL_GPU_FILTER_NEAREST,
      .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST,
      .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
  };
  return gpu::create_sampler(info);
}

Params make_params(uint32_t sourceWidth, uint32_t sourceHeight, const FrameMapping& mapping) noexcept {
  const auto dstSize = mapping.logicalSize;
  Params params{
      .dstWidth = dstSize.x,
      .dstHeight = dstSize.y,
      .srcWidth = sourceWidth,
      .srcHeight = sourceHeight,
  };

  if (mapping.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return params;
  }

  const auto logicalSize = mapping.logicalSize;
  if (logicalSize.x == 0 || logicalSize.y == 0 || sourceWidth == 0 || sourceHeight == 0) {
    return params;
  }

  const bool stretch = mapping.viewportPolicy == AURORA_VIEWPORT_STRETCH ||
                       mapping.viewportPolicy == AURORA_VIEWPORT_16_9;
  const float scaleX = static_cast<float>(sourceWidth) / static_cast<float>(logicalSize.x);
  const float scaleY = static_cast<float>(sourceHeight) / static_cast<float>(logicalSize.y);
  const float scale = std::min(scaleX, scaleY);
  params.scaleX = stretch ? scaleX : scale;
  params.scaleY = stretch ? scaleY : scale;
  params.offsetX =
      stretch ? 0.f : (static_cast<float>(sourceWidth) - static_cast<float>(logicalSize.x) * scale) * 0.5f;
  params.offsetY =
      stretch ? 0.f : (static_cast<float>(sourceHeight) - static_cast<float>(logicalSize.y) * scale) * 0.5f;
  return params;
}

void release_fence(Slot& slot) noexcept {
  if (slot.fence != nullptr) {
    if (g_device != nullptr) {
      SDL_ReleaseGPUFence(g_device, slot.fence);
    }
    slot.fence = nullptr;
  }
}

bool ensure_slot(Slot& slot, uint32_t width, uint32_t height) {
  const uint64_t byteSize = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * sizeof(uint32_t);
  if (slot.storageBuffer && slot.width == width && slot.height == height && slot.byteSize == byteSize) {
    return true;
  }

  slot.storageBuffer = {};
  slot.readbackBuffer = {};
  slot.width = width;
  slot.height = height;
  slot.byteSize = byteSize;

  if (byteSize == 0 || byteSize > UINT32_MAX) {
    return false;
  }

  slot.storageBuffer = gpu::create_buffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, static_cast<uint32_t>(byteSize),
                                         "Depth Peek Storage Buffer");
  slot.readbackBuffer = gpu::create_transfer_buffer(false, static_cast<uint32_t>(byteSize));
  return slot.storageBuffer && slot.readbackBuffer;
}

Slot* find_available_slot(uint32_t width, uint32_t height) {
  for (size_t i = 0; i < g_slots.size(); ++i) {
    const size_t idx = (g_nextSlot + i) % g_slots.size();
    auto& slot = g_slots[idx];
    if (slot.state != SlotState::Available) {
      continue;
    }
    if (!ensure_slot(slot, width, height)) {
      continue;
    }
    g_nextSlot = (idx + 1) % g_slots.size();
    return &slot;
  }
  return nullptr;
}

// Harvests readbacks whose GPU work has finished. Never blocks. Called with g_mutex held.
void complete_slots() noexcept {
  for (auto& slot : g_slots) {
    if (slot.state != SlotState::MapPending || slot.fence == nullptr || !SDL_QueryGPUFence(g_device, slot.fence)) {
      continue;
    }
    release_fence(slot);
    const auto valueCount = static_cast<size_t>(slot.width) * static_cast<size_t>(slot.height);
    const auto* mapped = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(g_device, slot.readbackBuffer->handle, false));
    if (mapped != nullptr) {
      g_latest.width = slot.width;
      g_latest.height = slot.height;
      g_latest.data.assign(mapped, mapped + valueCount);
      SDL_UnmapGPUTransferBuffer(g_device, slot.readbackBuffer->handle);
    } else {
      Log.warn("Depth Peek readback mapping failed: {}", SDL_GetError());
    }
    slot.state = SlotState::Available;
  }
}
} // namespace

void initialize() {
  g_pipeline = create_pipeline("Depth Peek Pipeline");
  g_sampler = create_sampler();
}

void shutdown() {
  testing::reset();
  g_pipeline = {};
  g_sampler = {};
  for (auto& slot : g_slots) {
    release_fence(slot);
    slot = {};
  }
}

void request_snapshot() noexcept {
  std::lock_guard lock{g_mutex};
  g_snapshotRequested = true;
}

bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
  std::lock_guard lock{g_mutex};
  if (x >= g_latest.width || y >= g_latest.height || g_latest.data.empty()) {
    return false;
  }
  z = g_latest.data[static_cast<size_t>(y) * g_latest.width + x] & 0x00ffffffu;
  return true;
}

void poll() noexcept {
  std::lock_guard lock{g_mutex};
  complete_slots();
}

FrameMapping capture_frame_mapping() noexcept {
  return {vi::configured_fb_size(), gx::g_gxState.viewportPolicy};
}

void encode_frame_snapshot(gpu::Encoder& enc, const gpu::Texture& depth, uint32_t msaaSamples,
                           const FrameMapping& mapping) noexcept {
  ZoneScoped;
  const auto now = Clock::now();
  {
    std::lock_guard lock{g_mutex};
    if (!g_snapshotRequested || now < g_nextSnapshotTime) {
      return;
    }
    g_snapshotRequested = false;
    g_nextSnapshotTime = now + SnapshotInterval;
  }

  const auto dstSize = mapping.logicalSize;
  if (!depth || !g_pipeline || dstSize.x == 0 || dstSize.y == 0 || depth->width == 0 || depth->height == 0) {
    return;
  }
  // SDL_GPU cannot sample a multisampled texture.
  if (msaaSamples > 1 || depth->samples != SDL_GPU_SAMPLECOUNT_1) {
    if (!g_msaaWarned) {
      g_msaaWarned = true;
      Log.warn("Depth Peek from multisampled EFB targets is not supported");
    }
    return;
  }

  const Params params = make_params(depth->width, depth->height, mapping);
  gpu::Buffer storageBuffer;
  {
    std::lock_guard lock{g_mutex};
    auto* slot = find_available_slot(dstSize.x, dstSize.y);
    if (slot == nullptr) {
      return;
    }
    slot->state = SlotState::CopySubmitted;
    storageBuffer = slot->storageBuffer;
  }

  const SDL_GPUStorageBufferReadWriteBinding storageBinding{
      .buffer = storageBuffer->handle,
      .cycle = false,
  };
  SDL_GPUComputePass* pass = enc.begin_compute_pass(nullptr, 0, &storageBinding, 1);
  if (pass == nullptr) {
    Log.warn("Depth Peek compute pass failed: {}", SDL_GetError());
    return;
  }
  SDL_BindGPUComputePipeline(pass, g_pipeline->handle);
  const SDL_GPUTextureSamplerBinding textureBinding{
      .texture = depth->handle,
      .sampler = g_sampler->handle,
  };
  SDL_BindGPUComputeSamplers(pass, 0, &textureBinding, 1);
  SDL_PushGPUComputeUniformData(enc.cmd(), 0, &params, sizeof(params));
  SDL_DispatchGPUCompute(pass, (dstSize.x + WorkgroupSizeX - 1) / WorkgroupSizeX,
                         (dstSize.y + WorkgroupSizeY - 1) / WorkgroupSizeY, 1);
  SDL_EndGPUComputePass(pass);
}

// The readback is recorded on its own command buffer submitted after the frame's, so its fence
// covers exactly the copy whose result is mapped.
void after_submit() noexcept {
  std::lock_guard lock{g_mutex};
  complete_slots();
  for (auto& slot : g_slots) {
    if (slot.state != SlotState::CopySubmitted) {
      continue;
    }
    gpu::Encoder enc;
    if (!enc) {
      slot.state = SlotState::Available;
      continue;
    }
    gpu::download_from_buffer(enc, *slot.storageBuffer, 0, *slot.readbackBuffer, 0,
                              static_cast<uint32_t>(slot.byteSize));
    slot.fence = enc.submit_with_fence();
    slot.state = slot.fence != nullptr ? SlotState::MapPending : SlotState::Available;
  }
}

namespace testing {
void reset() noexcept {
  std::lock_guard lock{g_mutex};
  g_snapshotRequested = false;
  g_nextSlot = 0;
  g_nextSnapshotTime = {};
  g_latest = {};
  for (auto& slot : g_slots) {
    release_fence(slot);
    slot.state = SlotState::Available;
  }
}

bool snapshot_requested() noexcept {
  std::lock_guard lock{g_mutex};
  return g_snapshotRequested;
}

void set_latest(uint32_t width, uint32_t height, const std::vector<uint32_t>& data) {
  std::lock_guard lock{g_mutex};
  g_latest.width = width;
  g_latest.height = height;
  g_latest.data = data;
}
} // namespace testing

} // namespace aurora::gfx::depth_peek
