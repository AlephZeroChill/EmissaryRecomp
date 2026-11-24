#pragma once
#include "staging_capacity.hpp"

#include "../internal.hpp"
#include "../gpu/gpu.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <array>
#include <memory>
#include <type_traits>
#include <utility>

#include <aurora/gfx.h>
#include <aurora/math.hpp>
#include <dolphin/gx/GXEnum.h>
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace aurora {
#if INTPTR_MAX == INT32_MAX
using HashType = XXH32_hash_t;
#else
using HashType = XXH64_hash_t;
#endif
static inline HashType xxh3_hash_s(const void* input, size_t len, HashType seed = 0) {
  return static_cast<HashType>(XXH3_64bits_withSeed(input, len, seed));
}
template <typename T>
static inline HashType xxh3_hash(const T& input, HashType seed = 0) {
  // Validate that the type has no padding bytes, which can easily cause
  // hash mismatches. This also disallows floats, but that's okay for us.
  static_assert(std::has_unique_object_representations_v<T>);
  return xxh3_hash_s(&input, sizeof(T), seed);
}

// Folds two 64-bit hashes. Chaining them instead drops XXH3 onto its seeded long path past 240
// bytes, and that path regenerates a 192-byte secret on every call.
static inline HashType hash_combine(HashType lhs, HashType rhs) {
  uint64_t mixed = static_cast<uint64_t>(lhs) ^ (static_cast<uint64_t>(rhs) + 0x9E3779B97F4A7C15ull +
                                                 (static_cast<uint64_t>(lhs) << 6) + (static_cast<uint64_t>(lhs) >> 2));
  mixed ^= mixed >> 33;
  mixed *= 0xFF51AFD7ED558CCDull;
  mixed ^= mixed >> 29;
  return static_cast<HashType>(mixed);
}

// Guest-RAM write tracking hooks (see aurora_set_guest_write_hooks). A digest samples the
// generation before reading the bytes, so a racing write costs an extra digest, never a skipped one.
inline constexpr uint64_t kGuestWriteUntracked = AURORA_GUEST_WRITE_UNTRACKED;
inline AuroraGuestWriteGenerationCallback g_guestWriteGenerationHook = nullptr;
inline AuroraGuestWriteNotifyCallback g_guestWriteNotifyHook = nullptr;

inline uint64_t guest_write_generation(const void* data, size_t size) noexcept {
  if (g_guestWriteGenerationHook == nullptr || data == nullptr || size == 0) {
    return kGuestWriteUntracked;
  }
  return g_guestWriteGenerationHook(data, size);
}

inline void notify_guest_write(const void* data, size_t size) noexcept {
  if (g_guestWriteNotifyHook == nullptr || data == nullptr || size == 0) {
    return;
  }
  g_guestWriteNotifyHook(data, size);
}

// True when `stored` was taken over the same untouched bytes, so its digest still describes them.
// The untracked sentinel never matches: a source aurora cannot watch is always re-digested.
inline bool guest_write_generation_matches(uint64_t stored, uint64_t current) noexcept {
  return current != kGuestWriteUntracked && stored == current;
}

class Hasher {
public:
  explicit Hasher(const XXH64_hash_t seed = 0) {
    XXH3_INITSTATE(&state);
    XXH3_64bits_reset_withSeed(&state, seed);
  }

  void update(const void* data, const size_t size) { XXH3_64bits_update(&state, data, size); }

  template <typename T>
  void update(const T& data) {
    static_assert(std::has_unique_object_representations_v<T>);
    update(&data, sizeof(T));
  }

  [[nodiscard]] XXH64_hash_t digest() const { return XXH3_64bits_digest(&state); }

private:
  XXH3_state_t state;
};

class ByteBuffer {
public:
  ByteBuffer() noexcept = default;
  explicit ByteBuffer(size_t size) noexcept
  : m_data(static_cast<uint8_t*>(calloc(1, size))), m_length(size), m_capacity(size) {}
  explicit ByteBuffer(uint8_t* data, size_t size) noexcept : m_data(data), m_capacity(size), m_owned(false) {}
  ~ByteBuffer() noexcept {
    if (m_data != nullptr && m_owned) {
      free(m_data);
    }
  }
  ByteBuffer(ByteBuffer&& rhs) noexcept
  : m_data(rhs.m_data), m_length(rhs.m_length), m_capacity(rhs.m_capacity), m_owned(rhs.m_owned) {
    rhs.m_data = nullptr;
    rhs.m_length = 0;
    rhs.m_capacity = 0;
    rhs.m_owned = true;
  }
  ByteBuffer& operator=(ByteBuffer&& rhs) noexcept {
    if (m_data != nullptr && m_owned) {
      free(m_data);
    }
    m_data = rhs.m_data;
    m_length = rhs.m_length;
    m_capacity = rhs.m_capacity;
    m_owned = rhs.m_owned;
    rhs.m_data = nullptr;
    rhs.m_length = 0;
    rhs.m_capacity = 0;
    rhs.m_owned = true;
    return *this;
  }
  ByteBuffer(ByteBuffer const&) = delete;
  ByteBuffer& operator=(ByteBuffer const&) = delete;
  operator ArrayRef<uint8_t>() const noexcept { return {m_data, m_length}; }

  [[nodiscard]] uint8_t* data() noexcept { return m_data; }
  [[nodiscard]] const uint8_t* data() const noexcept { return m_data; }
  [[nodiscard]] size_t size() const noexcept { return m_length; }
  [[nodiscard]] bool empty() const noexcept { return m_length == 0; }

  void append(const void* data, size_t size) {
    resize(m_length + size, false);
    memcpy(m_data + m_length, data, size);
    m_length += size;
  }

  template <typename T>
  void append(const T& obj) {
    append(&obj, sizeof(T));
  }

  void append_zeroes(size_t size) {
    resize(m_length + size, true);
    m_length += size;
  }

  // Extend the buffer without clearing the new bytes. Only for callers that overwrite the whole
  // region: the mapped staging buffers are megabytes of write-combine memory.
  void append_uninitialized(size_t size) {
    resize(m_length + size, false);
    m_length += size;
  }

  void release() {
    if (m_data != nullptr && m_owned) {
      free(m_data);
    }
    m_data = nullptr;
    m_length = 0;
    m_capacity = 0;
    m_owned = true;
  }

  void clear() {
    m_length = 0;
  }

  void reserve_extra(size_t size) { resize(m_length + size, true); }

  ByteBuffer clone() const {
    ByteBuffer clone{m_length};
    std::memcpy(clone.data(), m_data, m_length);
    return clone;
  }

private:
  uint8_t* m_data = nullptr;
  size_t m_length = 0;
  size_t m_capacity = 0;
  bool m_owned = true;

  // `size` is the total capacity needed. When `zeroed` is set, [m_length, size) has to read back as
  // zero on every branch; the early return used to leave the previous frame's bytes in the padding.
  void resize(size_t size, bool zeroed) {
    if (size == 0) {
      clear();
      return;
    }
    const size_t zeroBegin = m_length;
    if (m_data == nullptr) {
      if (zeroed) {
        m_data = static_cast<uint8_t*>(calloc(1, size));
      } else {
        m_data = static_cast<uint8_t*>(malloc(size));
      }
      m_owned = true;
      m_capacity = size;
      // calloc already cleared the whole allocation.
      return;
    }
    if (size > m_capacity) {
      if (!m_owned) {
        abort();
      }
      // Exponential expansion to avoid O(n^2) time complexity.
      size_t capacity = size;
      if (capacity < m_capacity * 2) {
        capacity = m_capacity * 2;
      }
      m_data = static_cast<uint8_t*>(realloc(m_data, capacity));
      m_capacity = capacity;
    }
    if (zeroed && size > zeroBegin) {
      memset(m_data + zeroBegin, 0, size - zeroBegin);
    }
  }
};
} // namespace aurora

namespace aurora::gfx {
inline constexpr bool UseTextureBuffer = false;
inline constexpr uint64_t UniformBufferSize = 25165824;  // 24mb
inline constexpr uint64_t VertexBufferSize = 3145728;    // 3mb
inline constexpr uint64_t IndexBufferSize = 2097152;     // 2mb
inline constexpr uint64_t StorageBufferSize = 8388608;   // 8mb
inline constexpr uint64_t TextureUploadSize = 25165824;  // 24mb

extern AuroraStats g_stats;
extern uint32_t g_drawCallCount;
extern uint32_t g_mergedDrawCallCount;
// GPU copies of the frame's pushed vertex, index and storage data. Uniform data never goes to a GPU
// buffer: draws push their range of the CPU uniform arena (PassContext::uniforms) with
// SDL_PushGPU*UniformData.
extern gpu::Buffer g_vertexBuffer;
extern gpu::Buffer g_indexBuffer;
extern gpu::Buffer g_storageBuffer;

using BindGroupRef = HashType;
using PipelineRef = HashType;
using SamplerRef = HashType;
using ShaderRef = HashType;
struct Range {
  uint32_t offset = 0;
  uint32_t size = 0;

  bool operator==(const Range& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const Range& rhs) const { return !(*this == rhs); }
};

struct ClipRect {
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;

  bool operator==(const ClipRect& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const ClipRect& rhs) const { return !(*this == rhs); }
};

using gpu::Viewport;

// What a draw needs to encode itself into a render pass.
struct PassContext {
  SDL_GPUCommandBuffer* cmd = nullptr;
  SDL_GPURenderPass* pass = nullptr;
  // Base of the CPU uniform arena the frame's Ranges index into.
  const uint8_t* uniforms = nullptr;
  uint32_t targetWidth = 0;
  uint32_t targetHeight = 0;

  [[nodiscard]] const void* uniform(const Range& range) const noexcept { return uniforms + range.offset; }
  // Pushes `range` of the uniform arena to uniform slot `slot` of both graphics stages.
  void push_uniform(uint32_t slot, const Range& range) const noexcept {
    SDL_PushGPUVertexUniformData(cmd, slot, uniform(range), range.size);
    SDL_PushGPUFragmentUniformData(cmd, slot, uniform(range), range.size);
  }
};

// A draw's fragment textures: one texture + sampler per slot, in slot order. Holds references so the
// GPU objects outlive every draw that recorded them.
inline constexpr uint32_t MaxBoundTextures = 8;
using TextureBindingsRaw = std::array<SDL_GPUTextureSamplerBinding, MaxBoundTextures>;
struct TextureBindings {
  std::array<gpu::Texture, MaxBoundTextures> textures;
  std::array<gpu::Sampler, MaxBoundTextures> samplers;
  TextureBindingsRaw raw{};
  uint32_t count = 0;
};

struct TextureRef;
using TextureHandle = std::shared_ptr<TextureRef>;

enum class ShaderType : uint8_t {
  Clear = 0,
  GX = 1,
};

void initialize();
void shutdown();

bool begin_frame();
bool resume_frame();
void abort_frame() noexcept;
void end_frame(gpu::Encoder& cmd);
// Milliseconds the last end_frame/end_batch spent unmapping the staging buffer, recording its upload copies, and
// mapping the next one (the frame worker's slow-frame log reports them).
inline double g_lastStagingUnmapMs = 0, g_lastStagingCopyMs = 0, g_lastStagingMapMs = 0;
void end_batch(gpu::Encoder& cmd);
uint32_t current_frame() noexcept;

// One frame's recorded render passes, detached from the guest-visible
// recording state. The contents are private to common.cpp.
struct SealedFrameData;

// Owns the recorded passes of a sealed frame, touched only by the thread that sealed it.
// seal_frame() runs with the producer excluded, and the object is reused to keep its capacity.
class SealedFrame {
public:
  SealedFrame();
  ~SealedFrame();
  SealedFrame(SealedFrame&&) noexcept;
  SealedFrame& operator=(SealedFrame&&) noexcept;
  SealedFrame(const SealedFrame&) = delete;
  SealedFrame& operator=(const SealedFrame&) = delete;

  [[nodiscard]] SealedFrameData& data() const noexcept { return *m_data; }

private:
  std::unique_ptr<SealedFrameData> m_data;
};

// Detach the recorded passes of the frame that just ended into `out`. Must be
// called with the renderer GPU mutex held; see SealedFrame.
void seal_frame(SealedFrame& out) noexcept;

// Encode a sealed frame. Never touches the producer-visible recording state,
// so this may run concurrently with the producer's FIFO drains.
void render(SealedFrame& frame, gpu::Encoder& cmd, int32_t interpolatedFrame = -1, bool finalize = true,
            bool recycle = true);
// Recycles a sealed frame's passes after render(frame, cmd, -1, true, false) and the replays that followed it.
void recycle_sealed_passes(SealedFrame& frame) noexcept;

// Encode the frame that is still being recorded. Only for the synchronous
// EFB-readback split path, which runs on the producer thread.
void render(gpu::Encoder& cmd, int32_t interpolatedFrame = -1, bool finalize = true);

// Sweep bind groups that have not been used for a while. Mutates the cache the producer inserts
// into, so it may only run with the producer excluded, inside the seal prologue.
void expire_bind_group_cache() noexcept;
void after_submit() noexcept;
void map_staging_buffer();
// `persistentCopy` marks resolves whose destination outlives the frame with no re-issue path, so
// the pass waits for its pipelines instead of dropping draws that would be baked in permanently.
// NSMBW: after `source` (a resolve target) is resolved this frame, copy it, scaled to width x height,
// into `dst` at (x, y). False when it can't (no such resolve, or dst isn't RGBA8).
bool add_resolve_patch(const TextureHandle& source, TextureHandle dst, uint32_t x, uint32_t y, uint32_t width,
                       uint32_t height);
void resolve_pass(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat = GX_TF_RGBA8,
                  const Vec4<float>* sourceRectPixels = nullptr, bool halfScale = false,
                  const std::array<u32, 3>* copyFilterCoefficients = nullptr, bool forceOpaqueAlpha = false,
                  float copyFilterRowStride = 1.0f, bool clampTop = false, bool clampBottom = false,
                  bool persistentCopy = false);

void begin_offscreen(uint32_t width, uint32_t height);
void end_offscreen();
bool is_offscreen() noexcept;
uint32_t get_sample_count() noexcept;
void clear_caches() noexcept;

namespace tex_palette_conv {
struct ConvRequest;
} // namespace tex_palette_conv
void queue_palette_conv(tex_palette_conv::ConvRequest req);

Range push_verts(const uint8_t* data, size_t length);
template <typename T>
static Range push_verts(ArrayRef<T> data) {
  return push_verts(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T));
}
Range push_indices(const uint8_t* data, size_t length);
template <typename T>
static Range push_indices(ArrayRef<T> data) {
  return push_indices(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T));
}
// push_indices of `count` indices src[i] + offset, written straight into the index staging (same range).
Range push_offset_indices(const uint16_t* src, size_t count, uint16_t offset);
Range push_uniform(const uint8_t* data, size_t length);
template <typename T>
static Range push_uniform(const T& data) {
  return push_uniform(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_storage(const uint8_t* data, size_t length);
// Diagnostics (AURORA_DRAW_DUMP): the bytes of `range` in the current staging buffer `buffer` (0 vertices, 1 uniforms,
// 2 indices, 3 storage), or null if out of range.
const uint8_t* staged_bytes(int buffer, const Range& range) noexcept;
template <typename T>
static Range push_storage(ArrayRef<T> data) {
  return push_storage(reinterpret_cast<const uint8_t*>(data.data()), data.size() * sizeof(T));
}
template <typename T>
static Range push_storage(const T& data) {
  return push_storage(reinterpret_cast<const uint8_t*>(&data), sizeof(T));
}
Range push_texture_data(const uint8_t* data, size_t length, uint32_t bytesPerRow, uint32_t rowsPerImage);
std::pair<ByteBuffer, Range> map_verts(size_t length);
std::pair<ByteBuffer, Range> map_indices(size_t length);
std::pair<ByteBuffer, Range> map_uniform(size_t length);
std::pair<ByteBuffer, Range> copy_uniform(Range source);
std::pair<ByteBuffer, Range> map_storage(size_t length);

template <typename State>
const State& get_state();
template <typename DrawData>
void push_draw_command(DrawData data);
template <typename DrawData>
DrawData* get_last_draw_command();

template <typename PipelineConfig>
PipelineRef pipeline_ref(const PipelineConfig& config);
// `currentPipeline` is the caller's per-pass dedupe slot; as a file-static, two concurrent encoders
// skipped each other's SetPipeline. `requireReady` refuses the skip-unready shortcut for bakes.
bool bind_pipeline(PipelineRef ref, const PassContext& ctx, PipelineRef& currentPipeline, bool requireReady = false);

// Interns a set of texture bindings for the frame; the returned pointer stays valid while the frame that
// recorded it is encoded (entries retire like the bind groups they replace).
using TextureBindingsRef = HashType;
TextureBindingsRef texture_bindings_ref(TextureBindings&& bindings);
const TextureBindings& find_texture_bindings(TextureBindingsRef id);
// The interned bindings whose raw handles are raw[0, count), if any (refreshed for this frame, as texture_bindings_ref
// does), with the id texture_bindings_ref gives them: a caller that finds them takes no texture or sampler reference.
std::pair<TextureBindingsRef, const TextureBindings*> find_texture_bindings(const TextureBindingsRaw& raw,
                                                                           uint32_t count);

const gpu::Sampler& sampler_ref(const SDL_GPUSamplerCreateInfo& descriptor);

uint32_t align_uniform(uint32_t value);
uint64_t staging_uniform_bytes(uint64_t bytes);
uint64_t staging_storage_bytes(uint64_t bytes);
// Admission does not allocate. A false result requires a producer-side split.
// Oversized operations fail before mutating the current draw/pass.
bool staging_has_space(const StagingSizes& demand);
void ensure_staging_space(const StagingSizes& demand);
void split_staging_batch();
uint64_t staging_epoch() noexcept;
StagingSizes staging_usage() noexcept;
StagingSizes staging_high_water() noexcept;
uint64_t staging_split_count() noexcept;
// Internal integration-test seam: never increases the physical allocation.
void set_staging_capacity_limits_for_testing(const StagingSizes& limits);


Vec2<uint32_t> get_render_target_size() noexcept;
// Same value as get_render_target_size() outside a render pass, but never
// touches the frame worker's render-pass list, so it is safe off-thread.
Vec2<uint32_t> get_frame_buffer_size() noexcept;
void set_viewport(const Viewport& viewport) noexcept;
void set_scissor(const ClipRect& scissor) noexcept;

void push_debug_group(std::string label);
void insert_debug_marker(std::string label);
} // namespace aurora::gfx
