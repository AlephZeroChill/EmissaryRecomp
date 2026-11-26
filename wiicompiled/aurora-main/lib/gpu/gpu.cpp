#include "gpu.hpp"
#include "shader.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <aurora/render_size_limits.hpp>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_vulkan.h>

#include "../gfx/common.hpp"
#include "../internal.hpp"
#include "../window.hpp"
#include "../dolphin/vi/vi_internal.hpp"

namespace aurora::gx {
void clear_display_copy_cache() noexcept;
} // namespace aurora::gx

namespace aurora {
extern std::atomic<bool> g_presentWaitsForRefresh;  // aurora.cpp
} // namespace aurora

namespace aurora::gpu {
static Module Log("aurora::gpu");

SDL_GPUDevice* g_device = nullptr;
GraphicsConfig g_graphicsConfig;
TextureWithSampler g_frameBuffer;
TextureWithSampler g_frameBufferResolved;
TextureWithSampler g_depthBuffer;
GraphicsPipeline g_CopyPipeline;
GraphicsPipeline g_PicturePipeline;  // draw_present_picture: the copy with the picture adjustments
bool g_bcTexturesSupported = false;
bool g_safeVertexFetch = false;
uint32_t g_shaderDebug = 0;

static SDL_Window* g_window = nullptr;  // claimed for the device's swapchain
static std::string g_adapterName;
static std::string g_backendName;
static std::atomic_bool g_initialized{false};
static Texture g_presentOverrideTexture;
static Sampler g_presentOverrideSampler;

// ---- object lifetimes ----

TextureObject::~TextureObject() {
  if (handle != nullptr && g_device != nullptr) {
    SDL_ReleaseGPUTexture(g_device, handle);
  }
}
SamplerObject::~SamplerObject() {
  if (handle != nullptr && g_device != nullptr) {
    SDL_ReleaseGPUSampler(g_device, handle);
  }
}
BufferObject::~BufferObject() {
  if (handle != nullptr && g_device != nullptr) {
    SDL_ReleaseGPUBuffer(g_device, handle);
  }
}
TransferBufferObject::~TransferBufferObject() {
  if (handle != nullptr && g_device != nullptr) {
    SDL_ReleaseGPUTransferBuffer(g_device, handle);
  }
}
ShaderObject::~ShaderObject() {
  if (handle != nullptr && g_device != nullptr) {
    SDL_ReleaseGPUShader(g_device, handle);
  }
}
GraphicsPipelineObject::~GraphicsPipelineObject() {
  if (handle != nullptr && g_device != nullptr) {
    SDL_ReleaseGPUGraphicsPipeline(g_device, handle);
  }
}
ComputePipelineObject::~ComputePipelineObject() {
  if (handle != nullptr && g_device != nullptr) {
    SDL_ReleaseGPUComputePipeline(g_device, handle);
  }
}

Texture create_texture(const SDL_GPUTextureCreateInfo& info, const char* label) {
  SDL_GPUTextureCreateInfo createInfo = info;
  SDL_PropertiesID props = 0;
  if (label != nullptr) {
    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_GPU_TEXTURE_CREATE_NAME_STRING, label);
    createInfo.props = props;
  }
  SDL_GPUTexture* handle = SDL_CreateGPUTexture(g_device, &createInfo);
  if (props != 0) {
    SDL_DestroyProperties(props);
  }
  if (handle == nullptr) {
    Log.error("Failed to create texture {} ({}x{}, format {}): {}", label ? label : "", info.width, info.height,
              static_cast<int>(info.format), SDL_GetError());
    return nullptr;
  }
  auto texture = std::make_shared<TextureObject>();
  texture->handle = handle;
  texture->format = info.format;
  texture->usage = info.usage;
  texture->width = info.width;
  texture->height = info.height;
  texture->layers = info.layer_count_or_depth;
  texture->mips = info.num_levels;
  texture->samples = info.sample_count;
  return texture;
}

Texture create_texture_2d(uint32_t width, uint32_t height, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                          const char* label, uint32_t mips, SDL_GPUSampleCount samples) {
  const SDL_GPUTextureCreateInfo info{
      .type = SDL_GPU_TEXTURETYPE_2D,
      .format = format,
      .usage = usage,
      .width = width,
      .height = height,
      .layer_count_or_depth = 1,
      .num_levels = mips,
      .sample_count = samples,
  };
  return create_texture(info, label);
}

Sampler create_sampler(const SDL_GPUSamplerCreateInfo& info) {
  SDL_GPUSampler* handle = SDL_CreateGPUSampler(g_device, &info);
  if (handle == nullptr) {
    Log.error("Failed to create sampler: {}", SDL_GetError());
    return nullptr;
  }
  auto sampler = std::make_shared<SamplerObject>();
  sampler->handle = handle;
  return sampler;
}

Buffer create_buffer(SDL_GPUBufferUsageFlags usage, uint32_t size, const char* label) {
  SDL_GPUBufferCreateInfo info{.usage = usage, .size = size};
  SDL_PropertiesID props = 0;
  if (label != nullptr) {
    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_GPU_BUFFER_CREATE_NAME_STRING, label);
    info.props = props;
  }
  SDL_GPUBuffer* handle = SDL_CreateGPUBuffer(g_device, &info);
  if (props != 0) {
    SDL_DestroyProperties(props);
  }
  if (handle == nullptr) {
    Log.error("Failed to create buffer {} ({} bytes): {}", label ? label : "", size, SDL_GetError());
    return nullptr;
  }
  auto buffer = std::make_shared<BufferObject>();
  buffer->handle = handle;
  buffer->usage = usage;
  buffer->size = size;
  return buffer;
}

TransferBuffer create_transfer_buffer(bool upload, uint32_t size) {
  const SDL_GPUTransferBufferCreateInfo info{
      .usage = upload ? SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD : SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
      .size = size,
  };
  SDL_GPUTransferBuffer* handle = SDL_CreateGPUTransferBuffer(g_device, &info);
  if (handle == nullptr) {
    Log.error("Failed to create transfer buffer ({} bytes): {}", size, SDL_GetError());
    return nullptr;
  }
  auto buffer = std::make_shared<TransferBufferObject>();
  buffer->handle = handle;
  buffer->size = size;
  buffer->upload = upload;
  return buffer;
}

GraphicsPipeline create_graphics_pipeline(const SDL_GPUGraphicsPipelineCreateInfo& info, const char* label) {
  SDL_GPUGraphicsPipelineCreateInfo createInfo = info;
  SDL_PropertiesID props = 0;
  if (label != nullptr) {
    props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_GPU_GRAPHICSPIPELINE_CREATE_NAME_STRING, label);
    createInfo.props = props;
  }
  SDL_GPUGraphicsPipeline* handle = SDL_CreateGPUGraphicsPipeline(g_device, &createInfo);
  if (props != 0) {
    SDL_DestroyProperties(props);
  }
  if (handle == nullptr) {
    Log.error("Failed to create pipeline {}: {}", label ? label : "", SDL_GetError());
    return nullptr;
  }
  auto pipeline = std::make_shared<GraphicsPipelineObject>();
  pipeline->handle = handle;
  return pipeline;
}

SDL_GPUSampleCount sample_count(uint32_t samples) noexcept {
  switch (samples) {
  case 2:
    return SDL_GPU_SAMPLECOUNT_2;
  case 4:
    return SDL_GPU_SAMPLECOUNT_4;
  case 8:
    return SDL_GPU_SAMPLECOUNT_8;
  default:
    return SDL_GPU_SAMPLECOUNT_1;
  }
}

uint32_t sample_count_value(SDL_GPUSampleCount samples) noexcept {
  switch (samples) {
  case SDL_GPU_SAMPLECOUNT_2:
    return 2;
  case SDL_GPU_SAMPLECOUNT_4:
    return 4;
  case SDL_GPU_SAMPLECOUNT_8:
    return 8;
  default:
    return 1;
  }
}

uint32_t format_block_size(SDL_GPUTextureFormat format) noexcept {
  switch (format) {
  case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT:
  case SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT:
  case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB:
  case SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM_SRGB:
  case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB:
  case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB:
    return 4;
  default:
    return 1;
  }
}

uint32_t format_block_bytes(SDL_GPUTextureFormat format) noexcept {
  switch (format) {
  case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB:
  case SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM:
    return 8;
  case SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT:
  case SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT:
  case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM:
  case SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM_SRGB:
  case SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB:
  case SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB:
    return 16;
  default:
    return SDL_GPUTextureFormatTexelBlockSize(format);
  }
}

bool is_depth_format(SDL_GPUTextureFormat format) noexcept {
  switch (format) {
  case SDL_GPU_TEXTUREFORMAT_D16_UNORM:
  case SDL_GPU_TEXTUREFORMAT_D24_UNORM:
  case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
  case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
  case SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT:
    return true;
  default:
    return false;
  }
}

// ---- encoder ----

Encoder::Encoder() : m_cmd(SDL_AcquireGPUCommandBuffer(g_device)) {
  if (m_cmd == nullptr) {
    Log.error("Failed to acquire a command buffer: {}", SDL_GetError());
  }
}

Encoder::Encoder(Encoder&& rhs) noexcept : m_cmd(rhs.m_cmd), m_copy(rhs.m_copy) {
  rhs.m_cmd = nullptr;
  rhs.m_copy = nullptr;
}

Encoder& Encoder::operator=(Encoder&& rhs) noexcept {
  if (this != &rhs) {
    submit();
    m_cmd = rhs.m_cmd;
    m_copy = rhs.m_copy;
    rhs.m_cmd = nullptr;
    rhs.m_copy = nullptr;
  }
  return *this;
}

Encoder::~Encoder() { submit(); }

SDL_GPUCopyPass* Encoder::copy_pass() {
  if (m_copy == nullptr && m_cmd != nullptr) {
    m_copy = SDL_BeginGPUCopyPass(m_cmd);
  }
  return m_copy;
}

void Encoder::end_copy_pass() {
  if (m_copy != nullptr) {
    SDL_EndGPUCopyPass(m_copy);
    m_copy = nullptr;
  }
}

SDL_GPURenderPass* Encoder::begin_render_pass(const SDL_GPUColorTargetInfo* colors, uint32_t colorCount,
                                              const SDL_GPUDepthStencilTargetInfo* depth) {
  end_copy_pass();
  return SDL_BeginGPURenderPass(m_cmd, colors, colorCount, depth);
}

SDL_GPUComputePass* Encoder::begin_compute_pass(const SDL_GPUStorageTextureReadWriteBinding* textures,
                                                uint32_t textureCount,
                                                const SDL_GPUStorageBufferReadWriteBinding* buffers,
                                                uint32_t bufferCount) {
  end_copy_pass();
  return SDL_BeginGPUComputePass(m_cmd, textures, textureCount, buffers, bufferCount);
}

// A command buffer that fails to submit is never retired: SDL_GPU keeps everything it used (cycled transfer and
// uniform buffers, descriptor sets) bound to it for good, so each later frame allocates new ones. After a driver
// reset (VK_ERROR_DEVICE_LOST, NVIDIA event 153) every submit fails and the process grew by about 2.5 GB a second
// to over 40 GB before the driver crashed it (Brawl Online board: "WHY are the emissary agent runs eating 22 GIGABYTES
// OF RAM"). A lost device does not come back, so close the game the way its window's close button does: the
// runtime's exit path stops Brawl Online's core first and saves the SD card.
static std::atomic<uint32_t> g_failedSubmitsInARow{0};
static std::atomic<bool> g_quitForFailedSubmits{false};

static void note_submit_result(bool ok) {
  if (ok) {
    g_failedSubmitsInARow.store(0, std::memory_order_relaxed);
    return;
  }
  const char* error = SDL_GetError();
  Log.error("Failed to submit a command buffer: {}", error);
  const std::string_view text = error != nullptr ? error : "";
  const bool lost = text.find("DEVICE_LOST") != std::string_view::npos ||
                    text.find("DEVICE_REMOVED") != std::string_view::npos ||
                    text.find("DEVICE_HUNG") != std::string_view::npos ||
                    text.find("DEVICE_RESET") != std::string_view::npos;
  const uint32_t inARow = g_failedSubmitsInARow.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((lost || inARow >= 120) && !g_quitForFailedSubmits.exchange(true)) {
    Log.error("The graphics device is gone ({} failed submits in a row): closing the game", inARow);
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
  }
}

// Test hook: AURORA_TEST_DEVICE_LOST_AFTER_SUBMITS=N cancels every command buffer after the Nth submit and reports
// VK_ERROR_DEVICE_LOST, the way a driver reset does, without resetting the GPU every other program shares.
static bool test_device_lost(SDL_GPUCommandBuffer* cmd) {
  static const uint64_t after = [] {
    const char* value = std::getenv("AURORA_TEST_DEVICE_LOST_AFTER_SUBMITS");
    return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ull;
  }();
  static std::atomic<uint64_t> submits{0};
  if (after == 0 || submits.fetch_add(1, std::memory_order_relaxed) < after) {
    return false;
  }
  SDL_CancelGPUCommandBuffer(cmd);
  SDL_SetError("vkQueueSubmit VK_ERROR_DEVICE_LOST (AURORA_TEST_DEVICE_LOST_AFTER_SUBMITS)");
  return true;
}

void Encoder::submit() {
  if (m_cmd == nullptr) {
    return;
  }
  end_copy_pass();
  note_submit_result(!test_device_lost(m_cmd) && SDL_SubmitGPUCommandBuffer(m_cmd));
  m_cmd = nullptr;
}

SDL_GPUFence* Encoder::submit_with_fence() {
  if (m_cmd == nullptr) {
    return nullptr;
  }
  end_copy_pass();
  SDL_GPUFence* fence = test_device_lost(m_cmd) ? nullptr : SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd);
  note_submit_result(fence != nullptr);
  m_cmd = nullptr;
  return fence;
}

void Encoder::cancel() {
  if (m_cmd == nullptr) {
    return;
  }
  end_copy_pass();
  SDL_CancelGPUCommandBuffer(m_cmd);
  m_cmd = nullptr;
}

// Minimized on Windows, the presenter asks for no swapchain image (the client area is 0x0), so SDL's Vulkan
// backend retired nothing: about 37 MB a game frame stayed bound to finished command buffers (2.2 GB a second;
// 33 GB in 20 s), the frame worker's staging maps waited longer each frame (the game fell to 30-50 fps and its
// music stuttered), and after 15-20 s the driver lost the device (NVIDIA event 153) and crashed the game in
// nvoglv64.dll (Brawl Online board: "my prod recomp just crashed. also the music stutters when the game is in the
// background").
static std::atomic<uint64_t> g_swapchainRequests{0};

void note_swapchain_requested() noexcept { g_swapchainRequests.fetch_add(1, std::memory_order_relaxed); }

void retire_finished_work_without_present() {
  // Frame worker only. The fence is the previous such frame's: waiting for it costs nothing unless the GPU is a
  // whole frame behind, and the wait is what retires every finished command buffer.
  static SDL_GPUFence* s_fence = nullptr;
  static uint64_t s_requestsSeen = 0;
  const uint64_t requests = g_swapchainRequests.load(std::memory_order_relaxed);
  if (requests != s_requestsSeen) {
    s_requestsSeen = requests;
    if (s_fence != nullptr) {
      SDL_ReleaseGPUFence(g_device, s_fence);
      s_fence = nullptr;
    }
    return;
  }
  if (s_fence != nullptr) {
    SDL_WaitForGPUFences(g_device, true, &s_fence, 1);
    SDL_ReleaseGPUFence(g_device, s_fence);
    s_fence = nullptr;
  }
  Encoder encoder;
  s_fence = encoder.submit_with_fence();
}

void copy_texture_to_texture(Encoder& enc, const TextureObject& src, uint32_t srcX, uint32_t srcY,
                             const TextureObject& dst, uint32_t dstX, uint32_t dstY, uint32_t width, uint32_t height,
                             uint32_t srcMip, uint32_t dstMip) {
  const SDL_GPUTextureLocation from{.texture = src.handle, .mip_level = srcMip, .x = srcX, .y = srcY};
  const SDL_GPUTextureLocation to{.texture = dst.handle, .mip_level = dstMip, .x = dstX, .y = dstY};
  SDL_CopyGPUTextureToTexture(enc.copy_pass(), &from, &to, width, height, 1, false);
}

void upload_to_buffer(Encoder& enc, const TransferBufferObject& src, uint32_t srcOffset, const BufferObject& dst,
                      uint32_t dstOffset, uint32_t size) {
  const SDL_GPUTransferBufferLocation from{.transfer_buffer = src.handle, .offset = srcOffset};
  const SDL_GPUBufferRegion to{.buffer = dst.handle, .offset = dstOffset, .size = size};
  SDL_UploadToGPUBuffer(enc.copy_pass(), &from, &to, false);
}

void upload_to_texture(Encoder& enc, const TransferBufferObject& src, uint32_t srcOffset, uint32_t pixelsPerRow,
                       uint32_t rowsPerLayer, const TextureObject& dst, uint32_t x, uint32_t y, uint32_t width,
                       uint32_t height, uint32_t mip, uint32_t layer) {
  const SDL_GPUTextureTransferInfo from{
      .transfer_buffer = src.handle,
      .offset = srcOffset,
      .pixels_per_row = pixelsPerRow,
      .rows_per_layer = rowsPerLayer,
  };
  const SDL_GPUTextureRegion to{
      .texture = dst.handle,
      .mip_level = mip,
      .layer = layer,
      .x = x,
      .y = y,
      .w = width,
      .h = height,
      .d = 1,
  };
  SDL_UploadToGPUTexture(enc.copy_pass(), &from, &to, false);
}

void download_from_texture(Encoder& enc, const TextureObject& src, uint32_t x, uint32_t y, uint32_t width,
                           uint32_t height, const TransferBufferObject& dst, uint32_t dstOffset,
                           uint32_t pixelsPerRow) {
  const SDL_GPUTextureRegion from{
      .texture = src.handle,
      .x = x,
      .y = y,
      .w = width,
      .h = height,
      .d = 1,
  };
  const SDL_GPUTextureTransferInfo to{
      .transfer_buffer = dst.handle,
      .offset = dstOffset,
      .pixels_per_row = pixelsPerRow,
      .rows_per_layer = 0,
  };
  SDL_DownloadFromGPUTexture(enc.copy_pass(), &from, &to);
}

void download_from_buffer(Encoder& enc, const BufferObject& src, uint32_t srcOffset, const TransferBufferObject& dst,
                          uint32_t dstOffset, uint32_t size) {
  const SDL_GPUBufferRegion from{.buffer = src.handle, .offset = srcOffset, .size = size};
  const SDL_GPUTransferBufferLocation to{.transfer_buffer = dst.handle, .offset = dstOffset};
  SDL_DownloadFromGPUBuffer(enc.copy_pass(), &from, &to);
}

void write_buffer(const BufferObject& dst, uint32_t offset, const void* data, uint32_t size) {
  if (size == 0) {
    return;
  }
  auto staging = create_transfer_buffer(true, size);
  if (!staging) {
    return;
  }
  void* mapped = SDL_MapGPUTransferBuffer(g_device, staging->handle, false);
  std::memcpy(mapped, data, size);
  SDL_UnmapGPUTransferBuffer(g_device, staging->handle);
  Encoder enc;
  upload_to_buffer(enc, *staging, 0, dst, offset, size);
  enc.submit();
}

void write_texture(const TextureObject& dst, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t mip,
                   uint32_t layer, const void* data, uint32_t bytesPerRow, uint32_t rowsPerLayer) {
  const uint32_t size = bytesPerRow * rowsPerLayer;
  if (size == 0) {
    return;
  }
  auto staging = create_transfer_buffer(true, size);
  if (!staging) {
    return;
  }
  void* mapped = SDL_MapGPUTransferBuffer(g_device, staging->handle, false);
  std::memcpy(mapped, data, size);
  SDL_UnmapGPUTransferBuffer(g_device, staging->handle);
  const uint32_t block = format_block_size(dst.format);
  const uint32_t pixelsPerRow = bytesPerRow / format_block_bytes(dst.format) * block;
  Encoder enc;
  upload_to_texture(enc, *staging, 0, pixelsPerRow, rowsPerLayer * block, dst, x, y, width, height, mip, layer);
  enc.submit();
}

// ---- configuration ----

static std::vector<std::string> g_gfxFlags;

static bool gfx_flag(std::string_view flag) {
  return std::find(g_gfxFlags.begin(), g_gfxFlags.end(), flag) != g_gfxFlags.end();
}

// AURORA_GFX_FLAGS: comma-separated graphics debugging switches.
//   nobc       do not use BC texture compression
//   safefetch / nosafefetch   force the GPU compatibility vertex fetch on or off
//   notex      every texture samples as grey (is it geometry or texturing?)
//   uvvis      texture samples show their coordinates instead
//   dumpshaders  write every generated GX shader to <cache>/shaders
//   debug      create the device with SDL's GPU debug mode (backend validation)
//   immediate  require immediate presentation (diagnose swapchain contention)
static void load_gfx_flags() {
  g_gfxFlags.clear();
  const char* env = std::getenv("AURORA_GFX_FLAGS");
  if (env == nullptr) {
    return;
  }
  std::string_view rest{env};
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    auto item = rest.substr(0, comma);
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
    if (!item.empty()) {
      g_gfxFlags.emplace_back(item);
    }
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  }
}

static bool contains_ignore_case(std::string_view haystack, std::string_view needle) {
  if (needle.size() > haystack.size()) {
    return false;
  }
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    size_t j = 0;
    while (j < needle.size() && std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
                                    std::tolower(static_cast<unsigned char>(needle[j]))) {
      ++j;
    }
    if (j == needle.size()) {
      return true;
    }
  }
  return false;
}

// Qualcomm's Adreno drivers miscompile aurora's vertex pulling on some driver versions, and every Android
// GPU runs a vendor driver that games rarely exercise this way, so phones get the safe path.
static void choose_gpu_compat() {
  const bool qualcomm = contains_ignore_case(g_adapterName, "adreno") || contains_ignore_case(g_adapterName, "qualcomm");
#if defined(__ANDROID__)
  constexpr bool mobile = true;
#else
  constexpr bool mobile = false;
#endif
  const char* reason = nullptr;
  if (gfx_flag("nosafefetch")) {
    g_safeVertexFetch = false;
    reason = "off (debug flag)";
  } else if (gfx_flag("safefetch")) {
    g_safeVertexFetch = true;
    reason = "on (debug flag)";
  } else if (g_config.gpuCompat == AURORA_GPU_COMPAT_ON) {
    g_safeVertexFetch = true;
    reason = "on (setting)";
  } else if (g_config.gpuCompat == AURORA_GPU_COMPAT_OFF) {
    g_safeVertexFetch = false;
    reason = "off (setting)";
  } else {
    g_safeVertexFetch = qualcomm || mobile;
    reason = qualcomm ? "on (auto: Qualcomm Adreno)" : mobile ? "on (auto: mobile GPU)" : "off (auto)";
  }
  g_shaderDebug = (gfx_flag("notex") ? ShaderDebugNoTextures : 0u) |
                  (gfx_flag("uvvis") ? ShaderDebugTexCoords : 0u) |
                  (gfx_flag("dumpshaders") ? ShaderDebugDumpSource : 0u);
  Log.info("GPU compatibility vertex fetch {} on {}", reason, g_adapterName);
}

const char* adapter_name() noexcept { return g_adapterName.c_str(); }

namespace {
// What Vulkan says about the device in use: its type (integrated or not) and its memory. SDL_GPU passes on
// neither, so a separate instance reads them, matched to the device by name. Only the leading fields of
// VkPhysicalDeviceProperties and VkPhysicalDeviceMemoryProperties are read, at their fixed offsets (Vulkan 1.0
// ABI), so no Vulkan headers are needed.
struct VulkanDeviceFacts {
  bool read = false;
  bool integrated = false;
  uint64_t memoryBytes = 0;
};
VulkanDeviceFacts g_deviceFacts;

VulkanDeviceFacts read_vulkan_device_facts(const std::string& name) {
  VulkanDeviceFacts facts;
#if !defined(__APPLE__)
  if (name.empty() || !SDL_Vulkan_LoadLibrary(nullptr)) {
    return facts;
  }
  using PFN_GetInstanceProcAddr = void* (*)(void*, const char*);
  const auto getInstanceProc = reinterpret_cast<PFN_GetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  using PFN_CreateInstance = int32_t (*)(const void*, const void*, void**);
  using PFN_DestroyInstance = void (*)(void*, const void*);
  using PFN_EnumerateDevices = int32_t (*)(void*, uint32_t*, void**);
  using PFN_GetDeviceInfo = void (*)(void*, void*);
  const auto createInstance =
      getInstanceProc ? reinterpret_cast<PFN_CreateInstance>(getInstanceProc(nullptr, "vkCreateInstance")) : nullptr;
  if (createInstance != nullptr) {
    struct {
      uint32_t sType = 1;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO
      const void* pNext = nullptr;
      uint32_t flags = 0;
      const void* pApplicationInfo = nullptr;
      uint32_t enabledLayerCount = 0;
      const char* const* ppEnabledLayerNames = nullptr;
      uint32_t enabledExtensionCount = 0;
      const char* const* ppEnabledExtensionNames = nullptr;
    } createInfo;
    void* instance = nullptr;
    if (createInstance(&createInfo, nullptr, &instance) == 0 && instance != nullptr) {
      const auto destroy = reinterpret_cast<PFN_DestroyInstance>(getInstanceProc(instance, "vkDestroyInstance"));
      const auto enumerate =
          reinterpret_cast<PFN_EnumerateDevices>(getInstanceProc(instance, "vkEnumeratePhysicalDevices"));
      const auto properties =
          reinterpret_cast<PFN_GetDeviceInfo>(getInstanceProc(instance, "vkGetPhysicalDeviceProperties"));
      const auto memory =
          reinterpret_cast<PFN_GetDeviceInfo>(getInstanceProc(instance, "vkGetPhysicalDeviceMemoryProperties"));
      uint32_t count = 0;
      if (enumerate != nullptr && properties != nullptr && memory != nullptr &&
          enumerate(instance, &count, nullptr) == 0 && count > 0) {
        std::vector<void*> devices(count);
        if (enumerate(instance, &count, devices.data()) >= 0) {
          for (uint32_t i = 0; i < count; ++i) {
            // VkPhysicalDeviceProperties: apiVersion, driverVersion, vendorID, deviceID, deviceType, deviceName[256].
            alignas(16) unsigned char props[2048]{};
            properties(devices[i], props);
            uint32_t type = 0;
            std::memcpy(&type, props + 16, sizeof(type));
            const char* deviceName = reinterpret_cast<const char*>(props + 20);
            if (std::string_view(deviceName, strnlen(deviceName, 256)) != name) {
              continue;
            }
            // VkPhysicalDeviceMemoryProperties: memoryTypeCount, memoryTypes[32] (8 bytes each), memoryHeapCount,
            // then memoryHeaps[16] at 264 (VkDeviceSize size, flags: 16 bytes each).
            alignas(16) unsigned char mem[1024]{};
            memory(devices[i], mem);
            uint32_t heapCount = 0;
            std::memcpy(&heapCount, mem + 260, sizeof(heapCount));
            for (uint32_t h = 0; h < std::min<uint32_t>(heapCount, 16); ++h) {
              uint64_t size = 0;
              uint32_t flags = 0;
              std::memcpy(&size, mem + 264 + h * 16, sizeof(size));
              std::memcpy(&flags, mem + 264 + h * 16 + 8, sizeof(flags));
              if ((flags & 1u) != 0) {  // VK_MEMORY_HEAP_DEVICE_LOCAL_BIT
                facts.memoryBytes = std::max(facts.memoryBytes, size);
              }
            }
            facts.integrated = type == 1;  // VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
            facts.read = true;
            break;
          }
        }
      }
      if (destroy != nullptr) {
        destroy(instance, nullptr);
      }
    }
  }
  SDL_Vulkan_UnloadLibrary();
#else
  (void)name;
#endif
  return facts;
}
} // namespace

bool adapter_integrated() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
  return true;  // Apple silicon: one memory for CPU and GPU
#else
  return g_deviceFacts.integrated;
#endif
}

uint64_t adapter_memory_bytes() noexcept { return g_deviceFacts.memoryBytes; }

const char* backend_name() noexcept { return g_backendName.c_str(); }

namespace {
struct RenderTargetSize {
  uint32_t width;
  uint32_t height;
};

RenderTargetSize clamp_render_target_size(uint32_t width, uint32_t height) noexcept {
  const uint32_t maxDimension = g_graphicsConfig.maxTextureDimension2D;
  if (width == 0 || height == 0 || maxDimension == 0 || (width <= maxDimension && height <= maxDimension)) {
    return {width, height};
  }
  // Keep the requested aspect while fitting both axes inside the maximum 2D texture size.
  if (width > maxDimension) {
    height = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(height) * maxDimension) / width));
    width = maxDimension;
  }
  if (height > maxDimension) {
    width = std::max(1u, static_cast<uint32_t>((static_cast<uint64_t>(width) * maxDimension) / height));
    height = maxDimension;
  }
  return {width, height};
}

RenderTargetSize clamp_frame_buffer_size(uint32_t width, uint32_t height) noexcept {
  const auto adapterClamped = clamp_render_target_size(width, height);
  const auto budgeted = render_size_limits::fit_framebuffer_to_budget(adapterClamped.width, adapterClamped.height,
                                                                      g_graphicsConfig.maxTextureDimension2D);
  return {budgeted.width, budgeted.height};
}

// NSMBW: V-Sync is a setting, off by default. Off, the guest paces itself and presents never wait
// (Immediate, else Mailbox). On, the runtime runs the game's VI clock at a whole fraction of the display's
// refresh (runtime/src/hle/vi.cpp), so presents land one per k vblanks and never queue up.
std::atomic<bool> g_vsyncEnabled{false};
std::atomic<int> g_vsyncRequest{-1};

SDL_GPUPresentMode best_present_mode() {
  const auto supports = [](SDL_GPUPresentMode mode) {
    return g_window != nullptr && SDL_WindowSupportsGPUPresentMode(g_device, g_window, mode);
  };
  if (gfx_flag("immediate")) {
    ASSERT(!g_vsyncEnabled.load(std::memory_order_acquire), "Immediate presentation requires V-Sync off");
    ASSERT(supports(SDL_GPU_PRESENTMODE_IMMEDIATE), "Swapchain does not support requested Immediate presentation");
    return SDL_GPU_PRESENTMODE_IMMEDIATE;
  }
  if (g_vsyncEnabled.load(std::memory_order_acquire)) {
    // Mailbox: no tearing, and a frame waiting for the screen is replaced by a newer one instead of queueing behind
    // it, so a hitch's catch-up frames never pile up as delay (Fifo's queue holds up to the swapchain's images).
    // AURORA_GFX_FLAGS=fifo keeps Fifo (measurements).
    if (!gfx_flag("fifo") && supports(SDL_GPU_PRESENTMODE_MAILBOX)) {
      return SDL_GPU_PRESENTMODE_MAILBOX;
    }
    return SDL_GPU_PRESENTMODE_VSYNC;
  }
  if (supports(SDL_GPU_PRESENTMODE_IMMEDIATE)) {
    return SDL_GPU_PRESENTMODE_IMMEDIATE;
  }
  if (supports(SDL_GPU_PRESENTMODE_MAILBOX)) {
    return SDL_GPU_PRESENTMODE_MAILBOX;
  }
  Log.warn("Swapchain supports neither Immediate nor Mailbox; presentation is capped at the display refresh rate.");
  return SDL_GPU_PRESENTMODE_VSYNC;
}

Sampler linear_clamp_sampler() {
  constexpr SDL_GPUSamplerCreateInfo info{
      .min_filter = SDL_GPU_FILTER_LINEAR,
      .mag_filter = SDL_GPU_FILTER_LINEAR,
      .mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR,
      .address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE,
      .min_lod = 0.f,
      .max_lod = 1000.f,
  };
  return create_sampler(info);
}
} // namespace

TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled) {
  const auto size = clamp_render_target_size(width, height);
  if (width == 0 || height == 0) {
    Log.fatal("Invalid render texture size! {}x{}, multisampled {}", width, height, multisampled);
  }
  const uint32_t samples = multisampled ? g_graphicsConfig.msaaSamples : 1;
  // SDL_GPU cannot sample a multisampled texture; it is resolved into a single-sampled one first.
  const SDL_GPUTextureUsageFlags usage =
      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | (samples == 1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0);
  return {
      .texture = create_texture_2d(size.width, size.height, g_graphicsConfig.surfaceFormat, usage, "Render texture", 1,
                                   sample_count(samples)),
      .sampler = linear_clamp_sampler(),
  };
}

static TextureWithSampler create_depth_texture(uint32_t width, uint32_t height) {
  const auto size = clamp_render_target_size(width, height);
  const uint32_t samples = g_graphicsConfig.msaaSamples;
  const SDL_GPUTextureUsageFlags usage =
      SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | (samples == 1 ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0);
  return {
      .texture = create_texture_2d(size.width, size.height, g_graphicsConfig.depthFormat, usage, "Depth texture", 1,
                                   sample_count(samples)),
      .sampler = linear_clamp_sampler(),
  };
}

const TextureWithSampler& present_source() noexcept {
  return g_graphicsConfig.msaaSamples > 1 ? g_frameBufferResolved : g_frameBuffer;
}

PresentSource current_present_source() noexcept {
  if (g_presentOverrideTexture) {
    return {.texture = g_presentOverrideTexture, .sampler = g_presentOverrideSampler};
  }
  return {.texture = present_source().texture, .sampler = present_source().sampler};
}

void set_present_source_override(Texture texture, Sampler sampler) noexcept {
  g_presentOverrideTexture = std::move(texture);
  g_presentOverrideSampler = std::move(sampler);
}

void clear_present_source_override() noexcept {
  g_presentOverrideTexture = {};
  g_presentOverrideSampler = {};
}

Viewport calculate_present_viewport_for_aspect(uint32_t surface_width, uint32_t surface_height,
                                               float content_aspect) noexcept {
  if (surface_width == 0 || surface_height == 0 || !(content_aspect > 0.f)) {
    return {};
  }
  uint32_t viewport_width = surface_width;
  uint32_t viewport_height = std::min<uint32_t>(
      surface_height, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_width) *
                                                                               static_cast<double>(1.f / content_aspect)))));
  if (viewport_height == surface_height) {
    viewport_width = std::min<uint32_t>(
        surface_width, std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(static_cast<double>(viewport_height) *
                                                                                static_cast<double>(content_aspect)))));
  }
  return {
      .left = static_cast<float>((surface_width - viewport_width) / 2),
      .top = static_cast<float>((surface_height - viewport_height) / 2),
      .width = static_cast<float>(viewport_width),
      .height = static_cast<float>(viewport_height),
      .znear = 0.f,
      .zfar = 1.f,
  };
}

Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept {
  if (content_width == 0 || content_height == 0) {
    return {};
  }
  return calculate_present_viewport_for_aspect(surface_width, surface_height,
                                               static_cast<float>(content_width) / static_cast<float>(content_height));
}

// EFB -> XFB copy: a full-screen triangle sampling the source.
static void create_copy_pipeline() {
  shader::Interface vs{.stage = SDL_GPU_SHADERSTAGE_VERTEX, .varyings = {{"vec2", "uv"}}};
  constexpr std::string_view vsBody = R"(
  vec2 p = vidx == 0u ? vec2(-1.0, 1.0) : (vidx == 1u ? vec2(-1.0, -3.0) : vec2(3.0, 1.0));
  vec2 uv = vidx == 0u ? vec2(0.0, 0.0) : (vidx == 1u ? vec2(0.0, 2.0) : vec2(2.0, 0.0));
  out_pos = vec4(p, 0.0, 1.0);
  vout.uv = uv;)";
  shader::Interface fs{
      .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
      .textures = {{"efb_texture"}},
      .varyings = {{"vec2", "uv"}},
  };
  constexpr std::string_view fsBody = R"(
  vec4 color = SAMPLE(efb_texture, vin.uv);
  out_color = vec4(color.rgb, 1.0);)";
  const auto vertex = shader::create(vs, {}, vsBody, "XFB copy vertex");
  const auto fragment = shader::create(fs, {}, fsBody, "XFB copy fragment");
  if (!vertex || !fragment) {
    return;
  }
  const SDL_GPUColorTargetDescription colorTarget{.format = g_graphicsConfig.surfaceFormat};
  const SDL_GPUGraphicsPipelineCreateInfo info{
      .vertex_shader = vertex->handle,
      .fragment_shader = fragment->handle,
      .primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST,
      .rasterizer_state = {.fill_mode = SDL_GPU_FILLMODE_FILL, .cull_mode = SDL_GPU_CULLMODE_NONE},
      .target_info = {.color_target_descriptions = &colorTarget, .num_color_targets = 1},
  };
  g_CopyPipeline = create_graphics_pipeline(info, "XFB copy pipeline");

  // The same copy with the player's picture adjustments: contrast-adaptive sharpening over the four nearest
  // neighbours (on the coarser of the picture's and the screen's pixel grids), then brightness, contrast and
  // vibrance (saturation that lifts dull colours more than vivid ones), in the game's own gamma space.
  shader::Interface pictureFs{
      .stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
      .uniforms = {{"adjust", "vec4 params; vec4 texel;"}},
      .textures = {{"efb_texture"}},
      .varyings = {{"vec2", "uv"}},
  };
  constexpr std::string_view pictureBody = R"(
  vec3 e = SAMPLE(efb_texture, vin.uv).rgb;
  vec3 color = e;
  float sharpening = adjust.params.x;
  if (sharpening > 0.0) {
    vec2 dx = vec2(adjust.texel.x, 0.0);
    vec2 dy = vec2(0.0, adjust.texel.y);
    vec3 b = SAMPLE(efb_texture, vin.uv - dy).rgb;
    vec3 d = SAMPLE(efb_texture, vin.uv - dx).rgb;
    vec3 f = SAMPLE(efb_texture, vin.uv + dx).rgb;
    vec3 h = SAMPLE(efb_texture, vin.uv + dy).rgb;
    vec3 mn = min(min(min(b, d), min(e, f)), h);
    vec3 mx = max(max(max(b, d), max(e, f)), h);
    vec3 amp = sqrt(clamp(min(mn, vec3(1.0) - mx) / max(mx, vec3(0.0001)), vec3(0.0), vec3(1.0)));
    vec3 w = amp * (-1.0 / mix(8.0, 5.0, sharpening));
    color = clamp((b * w + d * w + f * w + h * w + e) / (vec3(1.0) + 4.0 * w), vec3(0.0), vec3(1.0));
  }
  color = color * adjust.params.y;
  color = (color - vec3(0.5)) * adjust.params.z + vec3(0.5);
  float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
  float saturation = clamp(max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b)), 0.0, 1.0);
  float vibrance = adjust.params.w - 1.0;
  float amount = sel(1.0 + vibrance * (1.0 - saturation), 1.0 + vibrance, vibrance < 0.0);
  color = mix(vec3(luma), color, amount);
  out_color = vec4(clamp(color, vec3(0.0), vec3(1.0)), 1.0);)";
  const auto pictureFragment = shader::create(pictureFs, {}, pictureBody, "Picture adjust fragment");
  if (!pictureFragment) {
    return;
  }
  SDL_GPUGraphicsPipelineCreateInfo pictureInfo = info;
  pictureInfo.fragment_shader = pictureFragment->handle;
  g_PicturePipeline = create_graphics_pipeline(pictureInfo, "Picture adjust pipeline");
}

void draw_copy(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Texture& source, const Sampler& sampler) {
  (void)cmd;
  if (!g_CopyPipeline || !source || !sampler) {
    return;
  }
  SDL_BindGPUGraphicsPipeline(pass, g_CopyPipeline->handle);
  const SDL_GPUTextureSamplerBinding binding{.texture = source->handle, .sampler = sampler->handle};
  SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

void draw_present_picture(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Texture& source,
                          const Sampler& sampler, float width, float height) {
  const PictureAdjust adjust = picture_adjust();
  if (adjust.neutral() || !g_PicturePipeline || !source || !sampler || width < 1.f || height < 1.f) {
    draw_copy(cmd, pass, source, sampler);
    return;
  }
  struct {
    float params[4];
    float texel[4];
  } uniforms{
      {std::clamp(adjust.sharpening, 0.f, 1.f), adjust.brightness, adjust.contrast, adjust.vibrance},
      {1.f / std::min(width, static_cast<float>(source->width)),
       1.f / std::min(height, static_cast<float>(source->height)), 0.f, 0.f},
  };
  SDL_BindGPUGraphicsPipeline(pass, g_PicturePipeline->handle);
  SDL_PushGPUFragmentUniformData(cmd, 0, &uniforms, sizeof(uniforms));
  const SDL_GPUTextureSamplerBinding binding{.texture = source->handle, .sampler = sampler->handle};
  SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
  SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

static const char* sdl_driver(AuroraBackend backend) {
  switch (backend) {
  case BACKEND_METAL:
    return "metal";
  case BACKEND_VULKAN:
    return "vulkan";
  case BACKEND_D3D12:
    return "direct3d12";
  default:
    return nullptr;
  }
}

static bool claim_window() {
  SDL_Window* window = window::get_sdl_window();
  if (window == nullptr) {
    Log.error("Failed to claim the window: no window");
    return false;
  }
  if (g_window == window) {
    return true;
  }
  release_surface();
  if (!SDL_ClaimWindowForGPUDevice(g_device, window)) {
    Log.error("Failed to claim the window for the GPU device: {}", SDL_GetError());
    return false;
  }
  g_window = window;
  return true;
}

void apply_present_mode() noexcept {
  if (g_window == nullptr) {
    return;
  }
  g_graphicsConfig.presentMode = best_present_mode();
  g_presentWaitsForRefresh.store(g_graphicsConfig.presentMode == SDL_GPU_PRESENTMODE_VSYNC, std::memory_order_release);
  if (!SDL_SetGPUSwapchainParameters(g_device, g_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                     g_graphicsConfig.presentMode)) {
    Log.warn("Failed to set the present mode: {}", SDL_GetError());
  }
}

namespace {
std::atomic<uint32_t> g_msaaRequest{0};  // a sample count waiting for the next frame boundary; 0 = none
std::atomic<uint16_t> g_textureAnisotropy{16};
std::mutex g_pictureAdjustMutex;
PictureAdjust g_pictureAdjust;

uint32_t normalized_msaa(uint32_t samples) noexcept {
  return samples >= 8 ? 8 : samples >= 4 ? 4 : samples >= 2 ? 2 : 1;
}

// The largest count up to `samples` that both the color and the depth target take.
uint32_t supported_msaa(uint32_t samples) noexcept {
  samples = normalized_msaa(samples);
  while (samples > 1 && !(SDL_GPUTextureSupportsSampleCount(g_device, g_graphicsConfig.surfaceFormat,
                                                            sample_count(samples)) &&
                          SDL_GPUTextureSupportsSampleCount(g_device, g_graphicsConfig.depthFormat,
                                                            sample_count(samples)))) {
    samples /= 2;
  }
  return samples;
}
} // namespace

void request_msaa(uint32_t samples) noexcept {
  samples = normalized_msaa(samples);
  g_config.msaa = samples;
  if (g_initialized.load(std::memory_order_acquire)) {
    g_msaaRequest.store(samples, std::memory_order_release);
  }
}

bool msaa_change_pending() noexcept { return g_msaaRequest.load(std::memory_order_acquire) != 0; }

uint32_t requested_msaa() noexcept { return normalized_msaa(g_config.msaa); }

bool take_msaa_change() noexcept {
  const uint32_t request = g_msaaRequest.exchange(0, std::memory_order_acq_rel);
  if (request == 0) {
    return false;
  }
  const uint32_t samples = supported_msaa(request);
  if (samples == g_graphicsConfig.msaaSamples) {
    return false;
  }
  Log.info("MSAA {}x (asked for {}x)", samples, request);
  g_graphicsConfig.msaaSamples = samples;
  // New targets at the new count, at today's sizes; recorded passes keep the old ones alive until they are done.
  gx::clear_display_copy_cache();
  gfx::clear_caches();
  clear_present_source_override();
  if (g_frameBuffer.texture) {
    const uint32_t width = g_frameBuffer.texture->width;
    const uint32_t height = g_frameBuffer.texture->height;
    g_frameBuffer = create_render_texture(width, height, true);
    g_frameBufferResolved = create_render_texture(width, height, false);
    g_depthBuffer = create_depth_texture(width, height);
  }
  return true;
}

void set_texture_anisotropy(uint16_t value) noexcept {
  value = std::clamp<uint16_t>(value, 1, 16);
  g_config.maxTextureAnisotropy = value;
  g_textureAnisotropy.store(value, std::memory_order_relaxed);
}

uint16_t texture_anisotropy() noexcept { return g_textureAnisotropy.load(std::memory_order_relaxed); }

void set_picture_adjust(const PictureAdjust& adjust) noexcept {
  std::lock_guard lock(g_pictureAdjustMutex);
  g_pictureAdjust = adjust;
}

PictureAdjust picture_adjust() noexcept {
  std::lock_guard lock(g_pictureAdjustMutex);
  return g_pictureAdjust;
}

bool initialize(AuroraBackend auroraBackend) {
  load_gfx_flags();
  const char* driver = sdl_driver(auroraBackend);
  if (auroraBackend != BACKEND_AUTO && driver == nullptr) {
    Log.error("Backend {} is not available on SDL_GPU", static_cast<int>(auroraBackend));
    return false;
  }
  // GX shaders are generated at run time, so the device must take a language aurora can compile now:
  // MSL (Metal compiles it) or SPIR-V (glslang, lib/gpu/shader.cpp).
  constexpr SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV;
  if (g_device == nullptr) {
    g_device = SDL_CreateGPUDevice(formats, gfx_flag("debug"), driver);
    if (g_device == nullptr) {
      Log.error("Failed to create the SDL_GPU device ({}): {}", driver ? driver : "any", SDL_GetError());
      return false;
    }
  }
  g_backendName = SDL_GetGPUDeviceDriver(g_device);
  const SDL_PropertiesID props = SDL_GetGPUDeviceProperties(g_device);
  const char* name = SDL_GetStringProperty(props, SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr);
  g_adapterName = name != nullptr ? name : g_backendName;
  Log.info("Graphics device: {} on {}", g_adapterName, g_backendName);
  if (g_backendName == "vulkan") {
    g_deviceFacts = read_vulkan_device_facts(g_adapterName);
    if (g_deviceFacts.read) {
      Log.info("Graphics device: {} GPU, {} MiB of memory", g_deviceFacts.integrated ? "integrated" : "discrete",
               g_deviceFacts.memoryBytes >> 20);
    }
  }
  choose_gpu_compat();

  {
    window::SurfaceLock surfaceLock;
    if (!claim_window()) {
      return false;
    }
  }
  g_bcTexturesSupported = !gfx_flag("nobc") &&
                          SDL_GPUTextureSupportsFormat(g_device, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM,
                                                       SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER) &&
                          SDL_GPUTextureSupportsFormat(g_device, SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM,
                                                       SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER);
  const auto size = window::get_window_size();
  g_graphicsConfig = GraphicsConfig{
      .surfaceFormat = SDL_GetGPUSwapchainTextureFormat(g_device, g_window),
      .surfaceWidth = size.native_fb_width,
      .surfaceHeight = size.native_fb_height,
      .depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
      .msaaSamples = g_config.msaa,
      .textureAnisotropy = g_config.maxTextureAnisotropy,
      .maxTextureDimension2D = 16384,
  };
  g_graphicsConfig.msaaSamples = supported_msaa(g_config.msaa);
  set_texture_anisotropy(g_config.maxTextureAnisotropy);
  g_graphicsConfig.textureAnisotropy = texture_anisotropy();
  apply_present_mode();
  Log.info("Swapchain format {}, present mode {}, MSAA {}x", static_cast<int>(g_graphicsConfig.surfaceFormat),
           static_cast<int>(g_graphicsConfig.presentMode), g_graphicsConfig.msaaSamples);
  create_copy_pipeline();
  {
    window::SurfaceLock surfaceLock;
    resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height, true);
  }
  g_initialized.store(true, std::memory_order_release);
  return true;
}

void fail_if_device_lost() noexcept {}

void shutdown() {
  g_initialized.store(false, std::memory_order_release);
  if (g_device != nullptr) {
    SDL_WaitForGPUIdle(g_device);
  }
  g_CopyPipeline = {};
  g_PicturePipeline = {};
  g_frameBuffer = {};
  g_frameBufferResolved = {};
  g_depthBuffer = {};
  clear_present_source_override();
  release_surface();
  if (g_device != nullptr) {
    SDL_DestroyGPUDevice(g_device);
    g_device = nullptr;
  }
}

void release_surface() noexcept {
  if (g_window != nullptr && g_device != nullptr) {
    SDL_WaitForGPUIdle(g_device);
    SDL_ReleaseWindowFromGPUDevice(g_device, g_window);
  }
  g_window = nullptr;
}

bool refresh_surface(bool recreate) {
  if (g_device == nullptr) {
    return false;
  }
  if (!window::is_presentable()) {
    release_surface();
    return false;
  }
  if (recreate) {
    release_surface();
  }
  if (!claim_window()) {
    return false;
  }
  apply_present_mode();
  uint32_t width = g_graphicsConfig.surfaceWidth;
  uint32_t height = g_graphicsConfig.surfaceHeight;
  uint32_t native_width = width;
  uint32_t native_height = height;
  if (window::get_sdl_window() != nullptr) {
    const auto size = window::get_window_size();
    width = size.fb_width;
    height = size.fb_height;
    native_width = size.native_fb_width;
    native_height = size.native_fb_height;
  }
  if (width != 0 && height != 0) {
    resize_swapchain(width, height, native_width, native_height, true);
  }
  return true;
}

bool has_surface() noexcept { return g_window != nullptr; }

SDL_Window* swapchain_window() noexcept { return g_window; }

void request_vsync(bool enabled) noexcept {
  if (!g_initialized.load(std::memory_order_acquire)) {
    g_vsyncEnabled.store(enabled, std::memory_order_release);  // the first configuration picks it up
    return;
  }
  g_vsyncRequest.store(enabled ? 1 : 0, std::memory_order_release);
}

bool vsync_enabled() noexcept { return g_vsyncEnabled.load(std::memory_order_acquire); }

bool vsync_change_pending() noexcept { return g_vsyncRequest.load(std::memory_order_acquire) >= 0; }

bool take_vsync_change() noexcept {
  const int request = g_vsyncRequest.exchange(-1, std::memory_order_acq_rel);
  if (request < 0) {
    return false;
  }
  g_vsyncEnabled.store(request == 1, std::memory_order_release);
  if (g_window == nullptr) {
    return false;
  }
  const auto mode = best_present_mode();
  if (mode == g_graphicsConfig.presentMode) {
    return false;
  }
  Log.info("Present mode {} (V-Sync {})", static_cast<int>(mode), request == 1 ? "on" : "off");
  return true;
}

void resize_swapchain(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height, bool force) {
  if (g_window == nullptr || g_device == nullptr || width == 0 || height == 0 || native_height == 0 ||
      native_width == 0) {
    return;
  }

  uint32_t render_width = width;
  uint32_t render_height = height;
  const auto [efbWidth, efbHeight] = vi::configured_fb_size();
  // A line target sizes the render target exactly, including below 1x; otherwise the EFB workspace is the floor.
  if (efbWidth != 0 && efbHeight != 0 && !window::frame_buffer_line_target()) {
    render_width = std::max(render_width, efbWidth);
    render_height = std::max(render_height, efbHeight);
  }

  const auto requestedRenderSize = RenderTargetSize{render_width, render_height};
  const auto clampedRenderSize = clamp_frame_buffer_size(render_width, render_height);
  render_width = clampedRenderSize.width;
  render_height = clampedRenderSize.height;
  if (requestedRenderSize.width != render_width || requestedRenderSize.height != render_height) {
    Log.warn("Render target {}x{} exceeds the safe framebuffer budget (max {}, practical max {} / {} pixels); "
             "clamping to {}x{}",
             requestedRenderSize.width, requestedRenderSize.height, g_graphicsConfig.maxTextureDimension2D,
             render_size_limits::kMaxFramebufferDimension, render_size_limits::kMaxFramebufferPixels, render_width,
             render_height);
  }

  const uint32_t currentWidth = g_frameBuffer.texture ? g_frameBuffer.texture->width : 0;
  const uint32_t currentHeight = g_frameBuffer.texture ? g_frameBuffer.texture->height : 0;
  const bool sizeChanged = g_graphicsConfig.surfaceWidth != native_width ||
                           g_graphicsConfig.surfaceHeight != native_height || currentWidth != render_width ||
                           currentHeight != render_height;
  if (!force && !sizeChanged) {
    return;
  }
  if (sizeChanged) {
    gx::clear_display_copy_cache();
    gfx::clear_caches();
    clear_present_source_override();
    Log.info("Render target {}x{} for a {}x{} surface", render_width, render_height, native_width, native_height);
  }
  // The swapchain follows the window's size by itself; aurora only tracks it.
  g_graphicsConfig.surfaceWidth = native_width;
  g_graphicsConfig.surfaceHeight = native_height;
  if (!sizeChanged) {
    return;
  }
  g_frameBuffer = create_render_texture(render_width, render_height, true);
  g_frameBufferResolved = create_render_texture(render_width, render_height, false);
  g_depthBuffer = create_depth_texture(render_width, render_height);
}

void cache_shutdown() {}
} // namespace aurora::gpu
