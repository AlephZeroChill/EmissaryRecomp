#include "efb_ram_copy.hpp"

#include "efb_ram_encoder.hpp"
#include "tex_copy_conv.hpp"
#include "../internal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace aurora::gfx::efb_ram {
namespace {

Module Log("aurora::gfx::efb_ram");

using gpu::g_device;

// A CPU-consumed EFB copy this small is the lens-flare occlusion probe (4x4 Z24X8, 64 bytes).
// Resolving it synchronously stalls the producer and kills interpolation, so read it back async.
constexpr size_t kAsyncReadbackMaxBytes = 256;
// Each destination keeps its readback buffer forever. Only a handful are expected, and the cap
// stops an unexpected pattern of one-shot destinations from leaking GPU buffers.
constexpr size_t kMaxAsyncSlots = MaxAsyncReadbackSlots;
// NSMBW: texture patches (animated tiles) are read back every frame, one slot per patch position.
constexpr size_t kAsyncPatchMaxBytes = 16384;
constexpr size_t kMaxAsyncPatchSlots = 512;
// A one-shot color copy (schedule's `keep`: drawn once and sampled for long after, like Brawl's name tags over the
// fighters, rendered once when the match's HUD loads) is read back into guest RAM too, as a console's copy is, so the
// texture survives the GPU copy: after a lost device the new one decodes it from RAM (gpu::device_lost). Up to this
// size, this many destinations (the least recently copied one makes room), and once per destination in this many
// frames. AURORA_KEEP_COPIES=0: off.
constexpr size_t kAsyncKeepMaxBytes = 256 * 1024;
constexpr size_t kMaxAsyncKeepSlots = 64;
constexpr uint32_t kKeepEveryFrames = 30;

struct PendingCopy {
  void* dest = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t strideWidth = 0;  // > width: a patch into a wider texture
  GXTexFmt format = GX_TF_RGBA8;
  TextureHandle texture;
  TextureHandle nativeTexture;
};

struct Download {
  PendingCopy copy;
  gpu::TransferBuffer buffer;
  uint32_t bytesPerRow = 0;
  uint64_t bufferSize = 0;
};

// A submit's fence, shared by every readback recorded on that submit.
struct SharedFence {
  SDL_GPUFence* fence = nullptr;
  explicit SharedFence(SDL_GPUFence* fence) noexcept : fence(fence) {}
  SharedFence(const SharedFence&) = delete;
  SharedFence& operator=(const SharedFence&) = delete;
  ~SharedFence() {
    if (fence != nullptr && g_device != nullptr) {
      SDL_ReleaseGPUFence(g_device, fence);
    }
  }
};

enum class AsyncState : uint8_t {
  Idle,
  CopySubmitted,
  MapPending,
};

// One readback per destination address, reused for that destination's lifetime: the probe hits
// the same buffer every frame, so pooling avoids a per-frame allocation.
struct AsyncSlot {
  bool keep = false;          // a one-shot copy kept in RAM (kAsyncKeepMaxBytes)
  uint32_t keptFrame = 0;     // when it was last scheduled
  gpu::TransferBuffer buffer;
  TextureHandle nativeTexture;
  // The texture read back, held from encode until after_submit records its download.
  TextureHandle source;
  std::shared_ptr<SharedFence> fence;
  uint64_t bufferSize = 0;
  uint32_t bytesPerRow = 0;
  // Latched at encode time and read when the readback completes.
  void* dest = nullptr;
  GXTexFmt format = GX_TF_RGBA8;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t strideWidth = 0;
  uint32_t hostWidth = 0;
  uint32_t hostHeight = 0;
  HostPixelOrder order = HostPixelOrder::RGBA;
  AsyncState state = AsyncState::Idle;
};

std::vector<PendingCopy> g_pending;
std::vector<Download> g_downloads;

// Recorded by the producer, moved to the worker-private sealed list while the
// producer is excluded.
std::vector<PendingCopy> g_asyncPending;
std::vector<PendingCopy> g_asyncSealed;

std::mutex g_asyncMutex;
std::unordered_map<void*, AsyncSlot> g_asyncSlots;
uint32_t g_asyncMapsInFlight = 0;

// The shared blit shader clamps Y to flags.z/w; preserve the full source.
constexpr std::array<float, 12> NativeBlitUniform{
    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 64.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
};

// An internally scaled copy cannot be read back directly, so blit it down to native-resolution
// RGBA8 first.
void ensure_native_texture(PendingCopy& pending, TextureHandle* cache = nullptr) noexcept {
  if (pending.texture->size.width == pending.width && pending.texture->size.height == pending.height) {
    return;
  }
  if (pending.nativeTexture) {
    return;
  }
  if (cache != nullptr && *cache && (*cache)->size.width == pending.width &&
      (*cache)->size.height == pending.height) {
    pending.nativeTexture = *cache;
  } else {
    pending.nativeTexture = new_conv_texture(pending.width, pending.height, GX_TF_RGBA8, "GX EFB RAM native readback");
    if (cache != nullptr) {
      *cache = pending.nativeTexture;
    }
  }
}

void encode_native_blit(gpu::Encoder& enc, const PendingCopy& pending) noexcept {
  const tex_copy_conv::ConvRequest blitRequest{
      .fmt = GX_TF_RGBA8,
      .src = pending.texture->texture,
      .uniformRange = {0, sizeof(NativeBlitUniform)},
      .dst = pending.nativeTexture,
      .sampleFilter = pending.format == GX_TF_Z16 || pending.format == GX_TF_Z24X8
                          ? tex_copy_conv::SampleFilter::Nearest
                          : tex_copy_conv::SampleFilter::Linear,
  };
  tex_copy_conv::blit(enc, reinterpret_cast<const uint8_t*>(NativeBlitUniform.data()), blitRequest);
}

HostPixelOrder texture_pixel_order(const TextureHandle& texture) noexcept {
  return texture->format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ? HostPixelOrder::BGRA : HostPixelOrder::RGBA;
}

const TextureHandle& readback_texture(const PendingCopy& pending) noexcept {
  return pending.nativeTexture ? pending.nativeTexture : pending.texture;
}

// Called with g_asyncMutex held, once the slot's fence has signaled.
void complete_async_slot(AsyncSlot& slot) noexcept {
  slot.fence.reset();
  if (g_asyncMapsInFlight > 0) --g_asyncMapsInFlight;
  const auto* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(g_device, slot.buffer->handle, false));
  if (pixels == nullptr) {
    Log.warn("Async EFB RAM readback mapping failed: {}", SDL_GetError());
  } else {
    // Writes guest RAM from the frame worker while the guest may be reading it. The only
    // consumer min/maxes depth for a fade factor, so a torn tile just mixes two frames' depths.
    const size_t outputSize = encoded_size(slot.format, slot.width, slot.height);
    if (slot.strideWidth > slot.width) {
      // A patch: encode it on its own, then lay each row of blocks into the wider texture.
      std::vector<uint8_t> patch(outputSize);
      if (encode(patch.data(), outputSize, slot.format, slot.width, slot.height, pixels, slot.hostWidth,
                 slot.hostHeight, slot.bytesPerRow, slot.order)) {
        const size_t rowBytes = encoded_size(slot.format, slot.width, 1);
        const size_t pitch = encoded_size(slot.format, slot.strideWidth, 1);
        const size_t rows = rowBytes != 0 ? outputSize / rowBytes : 0;
        auto* out = static_cast<uint8_t*>(slot.dest);
        for (size_t row = 0; row < rows; ++row) {
          std::memcpy(out + row * pitch, patch.data() + row * rowBytes, rowBytes);
        }
        if (rows != 0) {
          notify_guest_write(slot.dest, (rows - 1) * pitch + rowBytes);
        }
      }
    } else {
      if (!encode(slot.dest, outputSize, slot.format, slot.width, slot.height, pixels, slot.hostWidth,
                  slot.hostHeight, slot.bytesPerRow, slot.order)) {
        Log.error("Failed to encode async EFB RAM copy format=0x{:x} size={}x{}",
                  static_cast<unsigned>(slot.format), slot.width, slot.height);
      }
      // Guest RAM written from outside the embedder, so nothing bumps its write generation and a
      // texture cached over this range would keep its digest.
      notify_guest_write(slot.dest, outputSize);
    }
    SDL_UnmapGPUTransferBuffer(g_device, slot.buffer->handle);
  }
  slot.state = AsyncState::Idle;
}

// Completes the readbacks whose GPU work has finished. Never blocks. Must not be called while
// g_asyncMutex is held.
void drain_async_events() noexcept {
  std::lock_guard lock{g_asyncMutex};
  if (g_asyncMapsInFlight == 0) {
    return;
  }
  for (auto& [dest, slot] : g_asyncSlots) {
    if (slot.state == AsyncState::MapPending && slot.fence && SDL_QueryGPUFence(g_device, slot.fence->fence)) {
      complete_async_slot(slot);
    }
  }
}

} // namespace

void schedule(void* dest, uint32_t width, uint32_t height, GXTexFmt format, TextureHandle texture,
              uint32_t strideWidth, bool keep) noexcept {
  if (dest == nullptr || width == 0 || height == 0 || !texture) {
    return;
  }
  if (!supports_format(format)) {
    Log.fatal("Unsupported CPU-visible EFB copy format 0x{:x}", static_cast<unsigned>(format));
  }

  PendingCopy request{
      .dest = dest,
      .width = width,
      .height = height,
      .format = format,
      .texture = std::move(texture),
  };
  const bool patch = strideWidth > width;
  request.strideWidth = patch ? strideWidth : 0;

  const size_t encodedSize = encoded_size(format, width, height);
  // Offscreen copies resolve on a separate pass list that the sealed frame's
  // native render does not replay, so they stay on the lazy path.
  bool async = !is_offscreen() && encodedSize != 0 &&
               encodedSize <= (patch ? kAsyncPatchMaxBytes : kAsyncReadbackMaxBytes);
  static const bool keepCopies = [] {
    const char* value = std::getenv("AURORA_KEEP_COPIES");
    return value == nullptr || value[0] != '0';
  }();
  if (!async && keep && keepCopies && !patch && !is_offscreen() && encodedSize != 0 &&
      encodedSize <= kAsyncKeepMaxBytes) {
    std::lock_guard lock{g_asyncMutex};
    const uint32_t frame = current_frame();
    auto it = g_asyncSlots.find(dest);
    if (it != g_asyncSlots.end()) {
      // A probe's or a patch's slot stays theirs; a kept one is read back again at most every kKeepEveryFrames.
      if (it->second.keep && frame - it->second.keptFrame >= kKeepEveryFrames) {
        it->second.keptFrame = frame;
        async = true;
      }
    } else {
      size_t kept = 0;
      auto oldest = g_asyncSlots.end();
      for (auto s = g_asyncSlots.begin(); s != g_asyncSlots.end(); ++s) {
        if (!s->second.keep) continue;
        ++kept;
        if (s->second.state == AsyncState::Idle &&
            (oldest == g_asyncSlots.end() || frame - s->second.keptFrame > frame - oldest->second.keptFrame)) {
          oldest = s;
        }
      }
      if (kept >= kMaxAsyncKeepSlots && oldest != g_asyncSlots.end()) {
        g_asyncSlots.erase(oldest);
        --kept;
      }
      if (kept < kMaxAsyncKeepSlots) {
        auto& slot = g_asyncSlots[dest];
        slot.keep = true;
        slot.keptFrame = frame;
        async = true;
      }
    }
    if (async) {
      // Its RAM is written when the readback lands, not now: the GPU copy is what the game samples until then.
      auto& list = g_asyncPending;
      if (auto p = std::find_if(list.begin(), list.end(), [dest](const PendingCopy& c) { return c.dest == dest; });
          p != list.end()) {
        *p = std::move(request);
      } else {
        list.push_back(std::move(request));
      }
      return;
    }
  }
  if (async) {
    std::lock_guard lock{g_asyncMutex};
    const auto it = g_asyncSlots.find(dest);
    if (it == g_asyncSlots.end()) {
      if (g_asyncSlots.size() >= (patch ? kMaxAsyncPatchSlots : kMaxAsyncSlots)) {
        async = false;
      } else {
        g_asyncSlots.try_emplace(dest);
        if (!patch) {
          // No readback has landed here yet. 0xff decodes to far Z, which the probe reads as
          // unobstructed so a new flare fades in; zero-filled RAM would decode as fully occluded.
          // (A patch keeps the texture's own pixels until its first readback lands.)
          std::memset(dest, 0xff, encodedSize);
          notify_guest_write(dest, encodedSize);
        }
      }
    }
  }

  auto& list = async ? g_asyncPending : g_pending;
  if (auto it = std::find_if(list.begin(), list.end(),
                             [dest](const PendingCopy& pending) { return pending.dest == dest; });
      it != list.end()) {
    *it = std::move(request);
  } else {
    list.push_back(std::move(request));
  }
}

bool has_pending(void* dest) noexcept {
  if (dest == nullptr) return !g_pending.empty() || !g_downloads.empty();
  return std::any_of(g_pending.begin(), g_pending.end(),
                     [dest](const PendingCopy& pending) { return pending.dest == dest; }) ||
         std::any_of(g_downloads.begin(), g_downloads.end(),
                     [dest](const Download& download) { return download.copy.dest == dest; });
}

bool prepare_downloads(void* dest) {
  bool found = false;
  for (auto& pending : g_pending) {
    if (dest != nullptr && pending.dest != dest) continue;
    found = true;
    ensure_native_texture(pending);
  }
  return found;
}

void encode_downloads(gpu::Encoder& enc, void* dest) noexcept {
  if (!g_downloads.empty()) {
    Log.fatal("Attempted to encode new EFB RAM downloads before completing the previous batch");
  }

  g_downloads.reserve(g_pending.size());
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    if (dest != nullptr && it->dest != dest) {
      ++it;
      continue;
    }
    PendingCopy pending = std::move(*it);
    it = g_pending.erase(it);
    if (pending.nativeTexture) {
      encode_native_blit(enc, pending);
    }
    g_downloads.push_back({.copy = std::move(pending)});
  }
}

// The caller has submitted the encoder holding the copies' blits; the downloads go on a command
// buffer of their own, submitted after it, whose fence covers them.
bool complete_downloads() noexcept {
  if (g_downloads.empty()) {
    return true;
  }
  bool success = true;
  gpu::Encoder enc;
  if (!enc) {
    g_downloads.clear();
    return false;
  }
  for (auto& download : g_downloads) {
    const auto& texture = readback_texture(download.copy);
    const uint32_t bytesPerRow = texture->size.width * 4;
    const uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * texture->size.height;
    download.buffer = gpu::create_transfer_buffer(false, static_cast<uint32_t>(bufferSize));
    if (!download.buffer) {
      continue;
    }
    download.bytesPerRow = bytesPerRow;
    download.bufferSize = bufferSize;
    gpu::download_from_texture(enc, *texture->texture, 0, 0, texture->size.width, texture->size.height,
                               *download.buffer, 0, texture->size.width);
  }
  SDL_GPUFence* fence = enc.submit_with_fence();
  if (fence == nullptr) {
    Log.error("EFB RAM readback submit failed: {}", SDL_GetError());
    g_downloads.clear();
    return false;
  }
  const bool waited = SDL_WaitForGPUFences(g_device, true, &fence, 1);
  SDL_ReleaseGPUFence(g_device, fence);
  if (!waited) {
    Log.error("EFB RAM readback wait failed: {}", SDL_GetError());
    g_downloads.clear();
    return false;
  }

  for (auto& download : g_downloads) {
    if (!download.buffer) {
      Log.error("EFB RAM readback buffer creation failed");
      success = false;
      continue;
    }
    const auto* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(g_device, download.buffer->handle, false));
    if (pixels == nullptr) {
      Log.error("EFB RAM readback mapping failed: {}", SDL_GetError());
      success = false;
      continue;
    }
    const auto& readbackTexture = readback_texture(download.copy);
    const auto order = texture_pixel_order(readbackTexture);
    const size_t outputSize = encoded_size(download.copy.format, download.copy.width, download.copy.height);
    if (!encode(download.copy.dest, outputSize, download.copy.format, download.copy.width, download.copy.height, pixels,
                readbackTexture->size.width, readbackTexture->size.height, download.bytesPerRow, order)) {
      Log.error("Failed to encode EFB RAM copy format=0x{:x} size={}x{}", static_cast<unsigned>(download.copy.format),
                download.copy.width, download.copy.height);
      success = false;
    }
    notify_guest_write(download.copy.dest, outputSize);
    SDL_UnmapGPUTransferBuffer(g_device, download.buffer->handle);
  }
  g_downloads.clear();
  return success;
}

void cancel() noexcept {
  g_pending.clear();
  g_downloads.clear();
  g_asyncPending.clear();
}

void seal_async_downloads() noexcept {
  {
    std::lock_guard lock{g_asyncMutex};
    for (auto& pending : g_asyncPending) {
      const auto it = g_asyncSlots.find(pending.dest);
      ensure_native_texture(pending, it != g_asyncSlots.end() ? &it->second.nativeTexture : nullptr);
    }
  }
  g_asyncSealed = std::move(g_asyncPending);
  g_asyncPending.clear();
}

void encode_async_downloads(gpu::Encoder& enc) noexcept {
  if (g_asyncSealed.empty()) {
    return;
  }
  // Retire the previous frame's map first so its slot is free here, otherwise a single-slot
  // destination could only be sampled every other frame.
  drain_async_events();
  std::lock_guard lock{g_asyncMutex};
  for (auto& pending : g_asyncSealed) {
    const auto it = g_asyncSlots.find(pending.dest);
    if (it == g_asyncSlots.end()) {
      continue;
    }
    auto& slot = it->second;
    // Only one readback per destination may be outstanding and the previous map still owns the pooled
    // buffer. Dropping this request just leaves the guest on a slightly older probe.
    if (slot.state != AsyncState::Idle) {
      continue;
    }
    if (pending.nativeTexture) {
      encode_native_blit(enc, pending);
    }
    const auto& texture = readback_texture(pending);
    const uint32_t bytesPerRow = texture->size.width * 4;
    const uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * texture->size.height;
    if (!slot.buffer || slot.bufferSize != bufferSize) {
      slot.buffer = gpu::create_transfer_buffer(false, static_cast<uint32_t>(bufferSize));
      slot.bufferSize = bufferSize;
    }
    if (!slot.buffer) {
      continue;
    }
    slot.source = texture;
    slot.bytesPerRow = bytesPerRow;
    slot.dest = pending.dest;
    slot.format = pending.format;
    slot.width = pending.width;
    slot.height = pending.height;
    slot.strideWidth = pending.strideWidth;
    slot.hostWidth = texture->size.width;
    slot.hostHeight = texture->size.height;
    slot.order = texture_pixel_order(texture);
    slot.state = AsyncState::CopySubmitted;
  }
  g_asyncSealed.clear();
}

// The frame's encoder (with any native blits) is submitted; record this frame's readbacks on one
// command buffer after it, whose fence tells drain_async_events when the slots may be mapped.
void after_submit() noexcept {
  {
    std::lock_guard lock{g_asyncMutex};
    std::vector<AsyncSlot*> recorded;
    std::optional<gpu::Encoder> enc;
    for (auto& [dest, slot] : g_asyncSlots) {
      if (slot.state != AsyncState::CopySubmitted) {
        continue;
      }
      if (!enc) {
        enc.emplace();
      }
      if (!*enc || !slot.source) {
        slot.source.reset();
        slot.state = AsyncState::Idle;
        continue;
      }
      gpu::download_from_texture(*enc, *slot.source->texture, 0, 0, slot.hostWidth, slot.hostHeight, *slot.buffer, 0,
                                 slot.hostWidth);
      slot.source.reset();
      recorded.push_back(&slot);
    }
    if (enc && *enc) {
      SDL_GPUFence* fence = enc->submit_with_fence();
      const auto shared = fence != nullptr ? std::make_shared<SharedFence>(fence) : nullptr;
      for (auto* slot : recorded) {
        if (!shared) {
          slot->state = AsyncState::Idle;
          continue;
        }
        slot->fence = shared;
        slot->state = AsyncState::MapPending;
        ++g_asyncMapsInFlight;
      }
    }
  }

  // Nothing else harvests completed readbacks on this thread, so a finished one would sit with its
  // slot busy. Querying the fences does not block.
  drain_async_events();
}

void abort_async() noexcept { g_asyncSealed.clear(); }

void shutdown() noexcept {
  cancel();
  g_asyncSealed.clear();
  decltype(g_asyncSlots) retiredSlots;
  {
    std::lock_guard lock{g_asyncMutex};
    retiredSlots.swap(g_asyncSlots);
    g_asyncMapsInFlight = 0;
  }
}

} // namespace aurora::gfx::efb_ram
