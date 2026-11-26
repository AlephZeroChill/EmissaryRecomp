#pragma once

// aurora's GPU layer: SDL_GPU, used directly. Objects are shared owners of SDL_GPU handles; SDL defers
// the real release until submitted work is done with an object, so dropping the last reference while a
// frame in flight still uses it is safe.

#include <aurora/aurora.h>
#include <aurora/math.hpp>

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace aurora::gpu {
extern SDL_GPUDevice* g_device;

struct TextureObject {
  SDL_GPUTexture* handle = nullptr;
  SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUTextureUsageFlags usage = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t layers = 1;
  uint32_t mips = 1;
  SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1;
  ~TextureObject();
};
using Texture = std::shared_ptr<TextureObject>;

struct SamplerObject {
  SDL_GPUSampler* handle = nullptr;
  ~SamplerObject();
};
using Sampler = std::shared_ptr<SamplerObject>;

struct BufferObject {
  SDL_GPUBuffer* handle = nullptr;
  SDL_GPUBufferUsageFlags usage = 0;
  uint32_t size = 0;
  ~BufferObject();
};
using Buffer = std::shared_ptr<BufferObject>;

struct TransferBufferObject {
  SDL_GPUTransferBuffer* handle = nullptr;
  uint32_t size = 0;
  bool upload = true;
  ~TransferBufferObject();
};
using TransferBuffer = std::shared_ptr<TransferBufferObject>;

struct ShaderObject {
  SDL_GPUShader* handle = nullptr;
  ~ShaderObject();
};
using Shader = std::shared_ptr<ShaderObject>;

struct GraphicsPipelineObject {
  SDL_GPUGraphicsPipeline* handle = nullptr;
  ~GraphicsPipelineObject();
};
using GraphicsPipeline = std::shared_ptr<GraphicsPipelineObject>;

struct ComputePipelineObject {
  SDL_GPUComputePipeline* handle = nullptr;
  ~ComputePipelineObject();
};
using ComputePipeline = std::shared_ptr<ComputePipelineObject>;

// The `props` label is attached where SDL supports debug names. Null on failure (logged).
Texture create_texture(const SDL_GPUTextureCreateInfo& info, const char* label);
Texture create_texture_2d(uint32_t width, uint32_t height, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                          const char* label, uint32_t mips = 1, SDL_GPUSampleCount samples = SDL_GPU_SAMPLECOUNT_1);
Sampler create_sampler(const SDL_GPUSamplerCreateInfo& info);
Buffer create_buffer(SDL_GPUBufferUsageFlags usage, uint32_t size, const char* label);
TransferBuffer create_transfer_buffer(bool upload, uint32_t size);
GraphicsPipeline create_graphics_pipeline(const SDL_GPUGraphicsPipelineCreateInfo& info, const char* label);

SDL_GPUSampleCount sample_count(uint32_t samples) noexcept;
uint32_t sample_count_value(SDL_GPUSampleCount samples) noexcept;
// Bytes per block and block width/height (1 for uncompressed formats).
uint32_t format_block_bytes(SDL_GPUTextureFormat format) noexcept;
uint32_t format_block_size(SDL_GPUTextureFormat format) noexcept;
bool is_depth_format(SDL_GPUTextureFormat format) noexcept;

// A command buffer and the copy pass open on it, if any. Copies open the copy pass on demand; beginning
// a render or compute pass ends it. Not thread-safe: one thread records into an encoder.
class Encoder {
public:
  Encoder();  // acquires a command buffer
  explicit Encoder(SDL_GPUCommandBuffer* cmd) noexcept : m_cmd(cmd) {}
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
  Encoder(Encoder&& rhs) noexcept;
  Encoder& operator=(Encoder&& rhs) noexcept;
  ~Encoder();  // submits if still open

  [[nodiscard]] SDL_GPUCommandBuffer* cmd() const noexcept { return m_cmd; }
  explicit operator bool() const noexcept { return m_cmd != nullptr; }

  SDL_GPUCopyPass* copy_pass();
  void end_copy_pass();
  SDL_GPURenderPass* begin_render_pass(const SDL_GPUColorTargetInfo* colors, uint32_t colorCount,
                                       const SDL_GPUDepthStencilTargetInfo* depth);
  SDL_GPUComputePass* begin_compute_pass(const SDL_GPUStorageTextureReadWriteBinding* textures, uint32_t textureCount,
                                         const SDL_GPUStorageBufferReadWriteBinding* buffers, uint32_t bufferCount);

  void submit();
  // Submits and returns a fence the caller releases with SDL_ReleaseGPUFence.
  SDL_GPUFence* submit_with_fence();
  void cancel();

private:
  SDL_GPUCommandBuffer* m_cmd = nullptr;
  SDL_GPUCopyPass* m_copy = nullptr;
};

// SDL_GPU's Vulkan backend frees finished command buffers, and everything they used (cycled transfer and uniform
// buffers, descriptor sets, pending destroys), only when a submitted command buffer asked for a swapchain image, or
// in SDL_WaitForGPUFences / SDL_WaitForGPUIdle. The presenter notes each frame it asks for one; a frame that ends
// without one (the window minimized, or every present skipped) retires finished work through a fence instead.
void note_swapchain_requested() noexcept;
void retire_finished_work_without_present();

// Copies recorded on an encoder's copy pass. Texture regions are mip 0, layer 0 unless given.
void copy_texture_to_texture(Encoder& enc, const TextureObject& src, uint32_t srcX, uint32_t srcY,
                             const TextureObject& dst, uint32_t dstX, uint32_t dstY, uint32_t width, uint32_t height,
                             uint32_t srcMip = 0, uint32_t dstMip = 0);
void upload_to_buffer(Encoder& enc, const TransferBufferObject& src, uint32_t srcOffset, const BufferObject& dst,
                      uint32_t dstOffset, uint32_t size);
// `pixelsPerRow`/`rowsPerLayer` describe the source layout in texels (blocks are expanded: give texel counts).
void upload_to_texture(Encoder& enc, const TransferBufferObject& src, uint32_t srcOffset, uint32_t pixelsPerRow,
                       uint32_t rowsPerLayer, const TextureObject& dst, uint32_t x, uint32_t y, uint32_t width,
                       uint32_t height, uint32_t mip = 0, uint32_t layer = 0);
void download_from_texture(Encoder& enc, const TextureObject& src, uint32_t x, uint32_t y, uint32_t width,
                           uint32_t height, const TransferBufferObject& dst, uint32_t dstOffset,
                           uint32_t pixelsPerRow = 0);
void download_from_buffer(Encoder& enc, const BufferObject& src, uint32_t srcOffset, const TransferBufferObject& dst,
                          uint32_t dstOffset, uint32_t size);

// Immediate uploads: their own command buffer, submitted before returning (ordered before later submits).
void write_buffer(const BufferObject& dst, uint32_t offset, const void* data, uint32_t size);
void write_texture(const TextureObject& dst, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t mip,
                   uint32_t layer, const void* data, uint32_t bytesPerRow, uint32_t rowsPerLayer);

struct GraphicsConfig {
  SDL_GPUTextureFormat surfaceFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
  SDL_GPUPresentMode presentMode = SDL_GPU_PRESENTMODE_VSYNC;
  uint32_t surfaceWidth = 0;
  uint32_t surfaceHeight = 0;
  SDL_GPUTextureFormat depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
  uint32_t msaaSamples = 1;
  uint16_t textureAnisotropy = 1;
  uint32_t maxTextureDimension2D = 16384;
};
struct TextureWithSampler {
  Texture texture;
  Sampler sampler;
};
struct PresentSource {
  Texture texture;
  Sampler sampler;
};
struct Viewport {
  float left;
  float top;
  float width;
  float height;
  float znear;
  float zfar;

  bool operator==(const Viewport& rhs) const {
    return left == rhs.left && top == rhs.top && width == rhs.width && height == rhs.height && znear == rhs.znear &&
           zfar == rhs.zfar;
  }
  bool operator!=(const Viewport& rhs) const { return !(*this == rhs); }
};

extern GraphicsConfig g_graphicsConfig;
extern TextureWithSampler g_frameBuffer;
extern TextureWithSampler g_frameBufferResolved;
extern TextureWithSampler g_depthBuffer;
extern GraphicsPipeline g_CopyPipeline;
extern bool g_bcTexturesSupported;
// Vertex fetch written for drivers that miscompile aurora's storage-buffer vertex pulling (Adreno:
// skinned models' texture coordinates and normals come out scrambled). See gx/shader.cpp.
extern bool g_safeVertexFetch;
// Shader diagnostics from AURORA_GFX_FLAGS (gx/shader.cpp apply_shader_debug).
enum ShaderDebug : uint32_t {
  ShaderDebugNoTextures = 1u << 0,
  ShaderDebugTexCoords = 1u << 1,
  ShaderDebugDumpSource = 1u << 2,
};
extern uint32_t g_shaderDebug;
// The device's name, for AuroraInfo.gpuName.
const char* adapter_name() noexcept;
// An integrated GPU (a laptop or mini PC's, sharing the CPU's memory).
bool adapter_integrated() noexcept;
const char* backend_name() noexcept;

bool initialize(AuroraBackend backend);
// The window claimed for the device's swapchain, or null while there is none.
SDL_Window* swapchain_window() noexcept;
bool has_surface() noexcept;
void shutdown();
void fail_if_device_lost() noexcept;
void release_surface() noexcept;
bool refresh_surface(bool recreate = true);
// NSMBW: V-Sync on or off; the change is applied by take_vsync_change at a frame boundary.
void request_vsync(bool enabled) noexcept;
bool vsync_enabled() noexcept;
// Applies a pending V-Sync change; true when the swapchain parameters must be set again.
bool take_vsync_change() noexcept;
bool vsync_change_pending() noexcept;
// Multisampling of the EFB (1, 2, 4 or 8 samples): request_msaa takes effect at the next frame boundary, where
// take_msaa_change applies it (clamped to what the device supports) and rebuilds the render targets.
void request_msaa(uint32_t samples) noexcept;
bool msaa_change_pending() noexcept;
bool take_msaa_change() noexcept;
// The sample count asked for (request_msaa or the config), before the device's clamp.
uint32_t requested_msaa() noexcept;
// Anisotropic filtering for the game's mipmapped, linearly filtered textures (1 = off, up to 16); read per draw.
void set_texture_anisotropy(uint16_t value) noexcept;
uint16_t texture_anisotropy() noexcept;
// Display-only adjustments of the game picture in the present pass (draw_present_picture). Sharpening 0..1
// (contrast-adaptive); brightness, contrast and vibrance 1 = as the game draws it.
struct PictureAdjust {
  float sharpening = 0.f;
  float brightness = 1.f;
  float contrast = 1.f;
  float vibrance = 1.f;
  bool neutral() const noexcept {
    return sharpening <= 0.f && brightness == 1.f && contrast == 1.f && vibrance == 1.f;
  }
};
void set_picture_adjust(const PictureAdjust& adjust) noexcept;
PictureAdjust picture_adjust() noexcept;
// The device's memory in bytes (its largest device-local heap) and whether it is an integrated GPU, from Vulkan's
// own description of the device in use (0 / false when it could not be read).
uint64_t adapter_memory_bytes() noexcept;
void apply_present_mode() noexcept;
void resize_swapchain(uint32_t width, uint32_t height, uint32_t native_width, uint32_t native_height,
                      bool force = false);
TextureWithSampler create_render_texture(uint32_t width, uint32_t height, bool multisampled);
const TextureWithSampler& present_source() noexcept;
PresentSource current_present_source() noexcept;
void set_present_source_override(Texture texture, Sampler sampler) noexcept;
void clear_present_source_override() noexcept;
// Draws `source` over the bound target with the XFB copy pipeline (a full-screen triangle).
void draw_copy(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Texture& source, const Sampler& sampler);
// draw_copy with the picture adjustments applied, over a viewport `width` x `height` pixels big (the
// sharpening's pixel spacing). Plain draw_copy while they are neutral.
void draw_present_picture(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Texture& source,
                          const Sampler& sampler, float width, float height);
Viewport calculate_present_viewport(uint32_t surface_width, uint32_t surface_height, uint32_t content_width,
                                    uint32_t content_height) noexcept;
Viewport calculate_present_viewport_for_aspect(uint32_t surface_width, uint32_t surface_height,
                                               float content_aspect) noexcept;

void cache_shutdown();
} // namespace aurora::gpu
