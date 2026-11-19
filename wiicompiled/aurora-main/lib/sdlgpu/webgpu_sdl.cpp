// The WebGPU C API subset aurora calls, implemented on SDL_GPU. Linked instead of Dawn when
// AURORA_GPU_SDLGPU is set; aurora's sources are unchanged. Dawn's webgpu.h stays the API definition.
//
// Model:
//   - Every buffer keeps a CPU copy. Mapping returns it; buffers the GPU reads (vertex, index, storage) also get an
//     SDL_GPUBuffer that copies upload into. Uniform buffers stay CPU-only: draws push the bound bytes.
//   - Command encoders record closures; QueueSubmit replays them into one SDL_GPUCommandBuffer.
//   - WGSL becomes MSL through aurora_wgsl_to_msl (naga), which also returns SDL_GPU's slot for every binding.
//   - Readbacks wait for their submit's fence before MapAsync completes (synchronous, like aurora uses them).
#include <dawn/webgpu.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

extern "C" int aurora_wgsl_to_msl(const char* src, unsigned stage, const char* entry, char** out);

namespace {

void fail(const char* what) {
  std::fprintf(stderr, "[webgpu-sdl] %s: %s\n", what, SDL_GetError());
}

std::string str(WGPUStringView v) {
  if (v.data == nullptr) return {};
  return v.length == WGPU_STRLEN ? std::string(v.data) : std::string(v.data, v.length);
}

WGPUStringView view_of(const char* s) { return {s, std::strlen(s)}; }

// ---------------------------------------------------------------------------------------------------------------
// Reference counting

struct Object {
  std::atomic<int> refs{1};
  virtual ~Object() = default;
};

template <typename T>
T* retain(T* o) {
  if (o != nullptr) o->refs.fetch_add(1, std::memory_order_relaxed);
  return o;
}

template <typename T>
void drop(T* o) {
  if (o != nullptr && o->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete o;
}

// A retained pointer captured by recorded commands.
template <typename T>
struct Ref {
  T* p = nullptr;
  Ref() = default;
  explicit Ref(T* o) : p(retain(o)) {}
  Ref(const Ref& r) : p(retain(r.p)) {}
  Ref& operator=(const Ref& r) {
    if (this != &r) {
      drop(p);
      p = retain(r.p);
    }
    return *this;
  }
  ~Ref() { drop(p); }
  T* operator->() const { return p; }
  T* get() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

// ---------------------------------------------------------------------------------------------------------------
// Futures: callbacks run when the caller waits (WaitAny / ProcessEvents), or at once for AllowSpontaneous.

std::mutex g_futureMutex;
uint64_t g_nextFuture = 1;
std::map<uint64_t, std::function<void()>> g_futures;

WGPUFuture make_future(WGPUCallbackMode mode, std::function<void()> fn) {
  if (mode == WGPUCallbackMode_AllowSpontaneous) {
    fn();
    std::lock_guard lock(g_futureMutex);
    return {g_nextFuture++};
  }
  std::lock_guard lock(g_futureMutex);
  const uint64_t id = g_nextFuture++;
  g_futures.emplace(id, std::move(fn));
  return {id};
}

bool run_future(uint64_t id) {
  std::function<void()> fn;
  {
    std::lock_guard lock(g_futureMutex);
    auto it = g_futures.find(id);
    if (it == g_futures.end()) return true;  // already completed
    fn = std::move(it->second);
    g_futures.erase(it);
  }
  fn();
  return true;
}

void run_all_futures() {
  std::map<uint64_t, std::function<void()>> pending;
  {
    std::lock_guard lock(g_futureMutex);
    pending.swap(g_futures);
  }
  for (auto& [id, fn] : pending) fn();
}

// ---------------------------------------------------------------------------------------------------------------
// Formats

struct FormatInfo {
  SDL_GPUTextureFormat sdl;
  uint32_t blockBytes;
  uint32_t blockDim;  // 1 for uncompressed, 4 for BC
};

FormatInfo format_info(WGPUTextureFormat f) {
  switch (f) {
  case WGPUTextureFormat_RGBA8Unorm: return {SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 4, 1};
  case WGPUTextureFormat_RGBA8UnormSrgb: return {SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, 4, 1};
  case WGPUTextureFormat_BGRA8Unorm: return {SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, 4, 1};
  case WGPUTextureFormat_BGRA8UnormSrgb: return {SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB, 4, 1};
  case WGPUTextureFormat_R8Unorm: return {SDL_GPU_TEXTUREFORMAT_R8_UNORM, 1, 1};
  case WGPUTextureFormat_RG8Unorm: return {SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, 2, 1};
  case WGPUTextureFormat_R16Sint: return {SDL_GPU_TEXTUREFORMAT_R16_INT, 2, 1};
  case WGPUTextureFormat_R32Float: return {SDL_GPU_TEXTUREFORMAT_R32_FLOAT, 4, 1};
  case WGPUTextureFormat_R32Uint: return {SDL_GPU_TEXTUREFORMAT_R32_UINT, 4, 1};
  case WGPUTextureFormat_RGBA16Float: return {SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, 8, 1};
  case WGPUTextureFormat_Depth32Float: return {SDL_GPU_TEXTUREFORMAT_D32_FLOAT, 4, 1};
  case WGPUTextureFormat_Depth24Plus: return {SDL_GPU_TEXTUREFORMAT_D24_UNORM, 4, 1};
  case WGPUTextureFormat_Depth16Unorm: return {SDL_GPU_TEXTUREFORMAT_D16_UNORM, 2, 1};
  case WGPUTextureFormat_BC1RGBAUnorm: return {SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM, 8, 4};
  case WGPUTextureFormat_BC1RGBAUnormSrgb: return {SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM_SRGB, 8, 4};
  case WGPUTextureFormat_BC2RGBAUnorm: return {SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM, 16, 4};
  case WGPUTextureFormat_BC2RGBAUnormSrgb: return {SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM_SRGB, 16, 4};
  case WGPUTextureFormat_BC3RGBAUnorm: return {SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM, 16, 4};
  case WGPUTextureFormat_BC3RGBAUnormSrgb: return {SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM_SRGB, 16, 4};
  case WGPUTextureFormat_BC4RUnorm: return {SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM, 8, 4};
  case WGPUTextureFormat_BC5RGUnorm: return {SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM, 16, 4};
  case WGPUTextureFormat_BC6HRGBFloat: return {SDL_GPU_TEXTUREFORMAT_BC6H_RGB_FLOAT, 16, 4};
  case WGPUTextureFormat_BC6HRGBUfloat: return {SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT, 16, 4};
  case WGPUTextureFormat_BC7RGBAUnorm: return {SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM, 16, 4};
  case WGPUTextureFormat_BC7RGBAUnormSrgb: return {SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM_SRGB, 16, 4};
  default: return {SDL_GPU_TEXTUREFORMAT_INVALID, 0, 1};
  }
}

bool is_depth(WGPUTextureFormat f) {
  return f == WGPUTextureFormat_Depth32Float || f == WGPUTextureFormat_Depth24Plus ||
         f == WGPUTextureFormat_Depth16Unorm || f == WGPUTextureFormat_Depth24PlusStencil8 ||
         f == WGPUTextureFormat_Depth32FloatStencil8;
}

SDL_GPUSampleCount sample_count(uint32_t n) {
  switch (n) {
  case 2: return SDL_GPU_SAMPLECOUNT_2;
  case 4: return SDL_GPU_SAMPLECOUNT_4;
  case 8: return SDL_GPU_SAMPLECOUNT_8;
  default: return SDL_GPU_SAMPLECOUNT_1;
  }
}

SDL_GPUCompareOp compare_op(WGPUCompareFunction f) {
  switch (f) {
  case WGPUCompareFunction_Never: return SDL_GPU_COMPAREOP_NEVER;
  case WGPUCompareFunction_Less: return SDL_GPU_COMPAREOP_LESS;
  case WGPUCompareFunction_Equal: return SDL_GPU_COMPAREOP_EQUAL;
  case WGPUCompareFunction_LessEqual: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
  case WGPUCompareFunction_Greater: return SDL_GPU_COMPAREOP_GREATER;
  case WGPUCompareFunction_NotEqual: return SDL_GPU_COMPAREOP_NOT_EQUAL;
  case WGPUCompareFunction_GreaterEqual: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
  default: return SDL_GPU_COMPAREOP_ALWAYS;
  }
}

SDL_GPUBlendFactor blend_factor(WGPUBlendFactor f) {
  switch (f) {
  case WGPUBlendFactor_Zero: return SDL_GPU_BLENDFACTOR_ZERO;
  case WGPUBlendFactor_One: return SDL_GPU_BLENDFACTOR_ONE;
  case WGPUBlendFactor_Src: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
  case WGPUBlendFactor_OneMinusSrc: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
  case WGPUBlendFactor_SrcAlpha: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
  case WGPUBlendFactor_OneMinusSrcAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  case WGPUBlendFactor_Dst: return SDL_GPU_BLENDFACTOR_DST_COLOR;
  case WGPUBlendFactor_OneMinusDst: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
  case WGPUBlendFactor_DstAlpha: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
  case WGPUBlendFactor_OneMinusDstAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
  case WGPUBlendFactor_SrcAlphaSaturated: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
  case WGPUBlendFactor_Constant: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
  case WGPUBlendFactor_OneMinusConstant: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
  default: return SDL_GPU_BLENDFACTOR_ONE;
  }
}

SDL_GPUBlendOp blend_op(WGPUBlendOperation o) {
  switch (o) {
  case WGPUBlendOperation_Subtract: return SDL_GPU_BLENDOP_SUBTRACT;
  case WGPUBlendOperation_ReverseSubtract: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
  case WGPUBlendOperation_Min: return SDL_GPU_BLENDOP_MIN;
  case WGPUBlendOperation_Max: return SDL_GPU_BLENDOP_MAX;
  default: return SDL_GPU_BLENDOP_ADD;
  }
}

SDL_GPUSamplerAddressMode address_mode(WGPUAddressMode m) {
  switch (m) {
  case WGPUAddressMode_Repeat: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
  case WGPUAddressMode_MirrorRepeat: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
  default: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
  }
}

SDL_GPULoadOp load_op(WGPULoadOp op) {
  return op == WGPULoadOp_Clear ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
}

SDL_GPUStoreOp store_op(WGPUStoreOp op) {
  return op == WGPUStoreOp_Discard ? SDL_GPU_STOREOP_DONT_CARE : SDL_GPU_STOREOP_STORE;
}

// ---------------------------------------------------------------------------------------------------------------
// Translated shader stages

enum class Kind { Uniform, Storage, RwStorage, Texture, StorageTexture, RwStorageTexture, Sampler };

struct StageBind {
  uint32_t group, binding;
  Kind kind;
  uint32_t slot;
  uint32_t bytes;
};

struct StageMap {
  std::string entry;
  uint32_t samplers = 0, storageTextures = 0, storageBuffers = 0, uniformBuffers = 0;
  uint32_t rwStorageTextures = 0, rwStorageBuffers = 0;
  uint32_t workgroup[3] = {1, 1, 1};
  int sizesSlot = -1;
  std::vector<StageBind> binds;
  std::string msl;
};

bool parse_kind(const std::string& s, Kind& k) {
  static const std::pair<const char*, Kind> kinds[] = {
      {"uniform", Kind::Uniform}, {"storage", Kind::Storage}, {"rwstorage", Kind::RwStorage},
      {"texture", Kind::Texture}, {"storagetexture", Kind::StorageTexture},
      {"rwstoragetexture", Kind::RwStorageTexture}, {"sampler", Kind::Sampler}};
  for (const auto& [name, kind] : kinds) {
    if (s == name) {
      k = kind;
      return true;
    }
  }
  return false;
}

bool translate(const std::string& wgsl, unsigned stage, const std::string& entry, StageMap& out) {
  char* text = nullptr;
  const int rc = aurora_wgsl_to_msl(wgsl.c_str(), stage, entry.c_str(), &text);
  const std::string blob = text != nullptr ? text : "";
  std::free(text);
  if (rc != 0) {
    std::fprintf(stderr, "[webgpu-sdl] WGSL translation of %s failed: %s\n", entry.c_str(), blob.c_str());
    return false;
  }
  const auto split = blob.find("---\n");
  std::istringstream header(blob.substr(0, split));
  out.msl = blob.substr(split + 4);
  for (std::string line; std::getline(header, line);) {
    std::istringstream ls(line);
    std::string tag;
    ls >> tag;
    if (tag == "entry") {
      ls >> out.entry;
    } else if (tag == "counts") {
      ls >> out.samplers >> out.storageTextures >> out.storageBuffers >> out.uniformBuffers >> out.rwStorageTextures >>
          out.rwStorageBuffers;
    } else if (tag == "workgroup") {
      ls >> out.workgroup[0] >> out.workgroup[1] >> out.workgroup[2];
    } else if (tag == "bind") {
      StageBind b{};
      std::string kind;
      ls >> b.group >> b.binding >> kind >> b.slot >> b.bytes;
      if (parse_kind(kind, b.kind)) out.binds.push_back(b);
    } else if (tag == "sizes") {
      ls >> out.sizesSlot;
    }
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Objects (the WGPU*Impl names are the opaque handle types webgpu.h declares)

struct WGPUInstanceImpl : Object {};
struct WGPUAdapterImpl : Object {};
struct WGPUQuerySetImpl : Object {};
struct WGPUSharedFenceImpl : Object {};
struct WGPUSharedTextureMemoryImpl : Object {};

struct WGPUDeviceImpl : Object {
  SDL_GPUDevice* gpu = nullptr;
  WGPUQueue queue = nullptr;
  SDL_GPUSampler* defaultSampler = nullptr;
  SDL_GPUBuffer* sizesBuffer = nullptr;  // naga's runtime-array sizes; unread with bounds checks off
  SDL_Window* window = nullptr;
  bool windowClaimed = false;
  WGPULoggingCallbackInfo logging{};
  ~WGPUDeviceImpl() override;
};

struct WGPUBufferImpl : Object {
  Ref<WGPUDeviceImpl> device;
  uint64_t size = 0;
  WGPUBufferUsage usage = 0;
  std::vector<uint8_t> cpu;
  SDL_GPUBuffer* gpu = nullptr;
  ~WGPUBufferImpl() override {
    if (gpu != nullptr) SDL_ReleaseGPUBuffer(device->gpu, gpu);
  }
};

struct WGPUTextureImpl : Object {
  Ref<WGPUDeviceImpl> device;
  SDL_GPUTexture* tex = nullptr;
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  WGPUTextureUsage usage = 0;
  uint32_t width = 0, height = 0, layers = 1, mips = 1, samples = 1;
  bool swapchain = false;
  ~WGPUTextureImpl() override {
    if (tex != nullptr && !swapchain) SDL_ReleaseGPUTexture(device->gpu, tex);
  }
};

struct WGPUTextureViewImpl : Object {
  Ref<WGPUTextureImpl> texture;
  uint32_t baseMip = 0, baseLayer = 0;
};

struct WGPUSamplerImpl : Object {
  Ref<WGPUDeviceImpl> device;
  SDL_GPUSampler* sampler = nullptr;
  ~WGPUSamplerImpl() override {
    if (sampler != nullptr) SDL_ReleaseGPUSampler(device->gpu, sampler);
  }
};

struct WGPUShaderModuleImpl : Object {
  std::string wgsl;
};

struct WGPUBindGroupLayoutImpl : Object {
  std::vector<WGPUBindGroupLayoutEntry> entries;
};

struct WGPUPipelineLayoutImpl : Object {
  std::vector<Ref<WGPUBindGroupLayoutImpl>> layouts;
};

struct BindEntry {
  Ref<WGPUBufferImpl> buffer;
  uint64_t offset = 0;
  uint64_t size = 0;
  Ref<WGPUSamplerImpl> sampler;
  Ref<WGPUTextureViewImpl> view;
  int dynamicIndex = -1;  // position among the layout's dynamic-offset entries, by binding order
};

struct WGPUBindGroupImpl : Object {
  std::map<uint32_t, BindEntry> entries;
};

struct WGPURenderPipelineImpl : Object {
  Ref<WGPUDeviceImpl> device;
  SDL_GPUGraphicsPipeline* pipeline = nullptr;
  StageMap vs, fs;
  ~WGPURenderPipelineImpl() override {
    if (pipeline != nullptr) SDL_ReleaseGPUGraphicsPipeline(device->gpu, pipeline);
  }
};

struct WGPUComputePipelineImpl : Object {
  Ref<WGPUDeviceImpl> device;
  SDL_GPUComputePipeline* pipeline = nullptr;
  StageMap cs;
  ~WGPUComputePipelineImpl() override {
    if (pipeline != nullptr) SDL_ReleaseGPUComputePipeline(device->gpu, pipeline);
  }
};

struct WGPUSurfaceImpl : Object {
  Ref<WGPUDeviceImpl> device;
  WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;
  uint32_t width = 0, height = 0;
  Ref<WGPUTextureImpl> current;
};

// Replay state for one submit.
struct Replay {
  WGPUDeviceImpl* device = nullptr;
  SDL_GPUCommandBuffer* cmd = nullptr;
  SDL_GPURenderPass* render = nullptr;
  SDL_GPUCopyPass* copy = nullptr;
  SDL_GPUTexture* swapchain = nullptr;
  bool swapchainAcquired = false;
  bool skipPass = false;
  WGPURenderPipelineImpl* pipeline = nullptr;
  WGPUComputePipelineImpl* compute = nullptr;
  WGPUBindGroupImpl* groups[4] = {};
  std::vector<uint32_t> dynamicOffsets[4];
  struct Download {
    SDL_GPUTransferBuffer* transfer;
    WGPUBufferImpl* dst;
    uint64_t dstOffset;
    uint32_t rows, rowBytes, srcRowBytes;
  };
  std::vector<Download> downloads;
  std::vector<SDL_GPUTransferBuffer*> uploads;

  SDL_GPUCopyPass* copy_pass() {
    if (copy == nullptr) copy = SDL_BeginGPUCopyPass(cmd);
    return copy;
  }
  void end_copy() {
    if (copy != nullptr) {
      SDL_EndGPUCopyPass(copy);
      copy = nullptr;
    }
  }
  SDL_GPUTexture* resolve(WGPUTextureImpl* t) {
    if (!t->swapchain) return t->tex;
    if (!swapchainAcquired) {
      swapchainAcquired = true;
      Uint32 w = 0, h = 0;
      if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, device->window, &swapchain, &w, &h)) fail("acquire swapchain");
    }
    return swapchain;
  }
  // Copies `size` bytes into a fresh upload transfer buffer, released after submit.
  SDL_GPUTransferBuffer* upload(const void* data, uint32_t size) {
    const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, size, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device->gpu, &info);
    void* mapped = SDL_MapGPUTransferBuffer(device->gpu, tb, false);
    std::memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(device->gpu, tb);
    uploads.push_back(tb);
    return tb;
  }
};

using Op = std::function<void(Replay&)>;

struct WGPUCommandEncoderImpl : Object {
  Ref<WGPUDeviceImpl> device;
  std::vector<Op> ops;
};

struct WGPUCommandBufferImpl : Object {
  std::vector<Op> ops;
};

struct WGPURenderPassEncoderImpl : Object {
  Ref<WGPUCommandEncoderImpl> encoder;
};

struct WGPUComputePassEncoderImpl : Object {
  Ref<WGPUCommandEncoderImpl> encoder;
};

struct WGPUQueueImpl : Object {
  WGPUDeviceImpl* device = nullptr;  // owned by the device
  std::vector<Op> pending;           // WriteBuffer/WriteTexture uploads, run before the next submit's commands
};

WGPUDeviceImpl::~WGPUDeviceImpl() {
  delete queue;
  if (gpu != nullptr) {
    if (defaultSampler != nullptr) SDL_ReleaseGPUSampler(gpu, defaultSampler);
    if (sizesBuffer != nullptr) SDL_ReleaseGPUBuffer(gpu, sizesBuffer);
    if (windowClaimed) SDL_ReleaseWindowFromGPUDevice(gpu, window);
    SDL_DestroyGPUDevice(gpu);
  }
}

namespace {

// Draw-time binding: every resource a stage uses, from the bound groups, at the slots naga assigned.
template <bool Vertex>
void bind_graphics_stage(Replay& r, const StageMap& map) {
  std::vector<SDL_GPUTextureSamplerBinding> samplers(map.samplers, SDL_GPUTextureSamplerBinding{});
  std::vector<SDL_GPUBuffer*> storage(map.storageBuffers, r.device->sizesBuffer);
  for (const auto& b : map.binds) {
    WGPUBindGroupImpl* group = b.group < 4 ? r.groups[b.group] : nullptr;
    if (group == nullptr) continue;
    auto it = group->entries.find(b.binding);
    if (it == group->entries.end()) continue;
    const BindEntry& e = it->second;
    switch (b.kind) {
    case Kind::Uniform: {
      uint64_t offset = e.offset;
      if (e.dynamicIndex >= 0 && e.dynamicIndex < static_cast<int>(r.dynamicOffsets[b.group].size())) {
        offset += r.dynamicOffsets[b.group][e.dynamicIndex];
      }
      if (e.buffer && offset + b.bytes <= e.buffer->cpu.size()) {
        if constexpr (Vertex) {
          SDL_PushGPUVertexUniformData(r.cmd, b.slot, e.buffer->cpu.data() + offset, b.bytes);
        } else {
          SDL_PushGPUFragmentUniformData(r.cmd, b.slot, e.buffer->cpu.data() + offset, b.bytes);
        }
      }
      break;
    }
    case Kind::Storage: {
      const uint32_t index = b.slot - map.uniformBuffers;
      if (index < storage.size() && e.buffer) storage[index] = e.buffer->gpu;
      break;
    }
    case Kind::Texture:
      if (b.slot < samplers.size() && e.view) samplers[b.slot].texture = r.resolve(e.view->texture.get());
      break;
    case Kind::Sampler:
      if (b.slot < samplers.size() && e.sampler) samplers[b.slot].sampler = e.sampler->sampler;
      break;
    default:
      break;
    }
  }
  for (auto& s : samplers) {
    if (s.sampler == nullptr) s.sampler = r.device->defaultSampler;
  }
  if constexpr (Vertex) {
    if (!samplers.empty()) SDL_BindGPUVertexSamplers(r.render, 0, samplers.data(), samplers.size());
    if (!storage.empty()) SDL_BindGPUVertexStorageBuffers(r.render, 0, storage.data(), storage.size());
  } else {
    if (!samplers.empty()) SDL_BindGPUFragmentSamplers(r.render, 0, samplers.data(), samplers.size());
    if (!storage.empty()) SDL_BindGPUFragmentStorageBuffers(r.render, 0, storage.data(), storage.size());
  }
}

void bind_draw(Replay& r) {
  if (r.pipeline == nullptr) return;
  bind_graphics_stage<true>(r, r.pipeline->vs);
  bind_graphics_stage<false>(r, r.pipeline->fs);
}

void set_bind_group(Replay& r, uint32_t index, WGPUBindGroupImpl* group, const std::vector<uint32_t>& offsets) {
  if (index >= 4) return;
  r.groups[index] = group;
  r.dynamicOffsets[index] = offsets;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// C API

extern "C" {

WGPUInstance wgpuCreateInstance(WGPUInstanceDescriptor const*) {
  if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_InitSubSystem(SDL_INIT_VIDEO);
  return new WGPUInstanceImpl;
}

void wgpuInstanceAddRef(WGPUInstance i) { retain(i); }
void wgpuInstanceRelease(WGPUInstance i) { drop(i); }

WGPUSurface wgpuInstanceCreateSurface(WGPUInstance, WGPUSurfaceDescriptor const*) {
  // SDL_GPU presents to the SDL window itself; the platform layer the descriptor carries is not needed.
  return new WGPUSurfaceImpl;
}

WGPUFuture wgpuInstanceRequestAdapter(WGPUInstance, WGPURequestAdapterOptions const*,
                                      WGPURequestAdapterCallbackInfo info) {
  auto* adapter = new WGPUAdapterImpl;
  return make_future(info.mode, [info, adapter] {
    info.callback(WGPURequestAdapterStatus_Success, adapter, view_of(""), info.userdata1, info.userdata2);
  });
}

void wgpuInstanceProcessEvents(WGPUInstance) { run_all_futures(); }

WGPUWaitStatus wgpuInstanceWaitAny(WGPUInstance, size_t count, WGPUFutureWaitInfo* futures, uint64_t) {
  for (size_t i = 0; i < count; ++i) {
    futures[i].completed = run_future(futures[i].future.id);
  }
  return WGPUWaitStatus_Success;
}

void wgpuAdapterAddRef(WGPUAdapter a) { retain(a); }
void wgpuAdapterRelease(WGPUAdapter a) { drop(a); }

WGPUStatus wgpuAdapterGetInfo(WGPUAdapter, WGPUAdapterInfo* info) {
  info->vendor = view_of("SDL_GPU");
  info->architecture = view_of("");
  info->device = view_of("SDL_GPU");
  info->description = view_of("aurora WebGPU on SDL_GPU");
#if defined(__APPLE__)
  info->backendType = WGPUBackendType_Metal;
  info->adapterType = WGPUAdapterType_IntegratedGPU;
#else
  info->backendType = WGPUBackendType_Vulkan;
  info->adapterType = WGPUAdapterType_DiscreteGPU;
#endif
  info->vendorID = 0;
  info->deviceID = 0;
  info->subgroupMinSize = 32;
  info->subgroupMaxSize = 32;
  return WGPUStatus_Success;
}

void wgpuAdapterInfoFreeMembers(WGPUAdapterInfo) {}

WGPUStatus wgpuAdapterGetLimits(WGPUAdapter, WGPULimits* l) {
  WGPUChainedStruct* next = l->nextInChain;
  *l = WGPULimits{};
  l->nextInChain = next;
  l->maxTextureDimension1D = 16384;
  l->maxTextureDimension2D = 16384;
  l->maxTextureDimension3D = 2048;
  l->maxTextureArrayLayers = 2048;
  l->maxBindGroups = 4;
  l->maxBindGroupsPlusVertexBuffers = 24;
  l->maxBindingsPerBindGroup = 1000;
  l->maxDynamicUniformBuffersPerPipelineLayout = 8;
  l->maxDynamicStorageBuffersPerPipelineLayout = 4;
  l->maxSampledTexturesPerShaderStage = 16;
  l->maxSamplersPerShaderStage = 16;
  l->maxStorageBuffersPerShaderStage = 8;
  l->maxStorageTexturesPerShaderStage = 4;
  l->maxUniformBuffersPerShaderStage = 4;
  l->maxUniformBufferBindingSize = 65536;
  l->maxStorageBufferBindingSize = 1u << 30;
  l->minUniformBufferOffsetAlignment = 256;
  l->minStorageBufferOffsetAlignment = 256;
  l->maxVertexBuffers = 8;
  l->maxBufferSize = 1ull << 30;
  l->maxVertexAttributes = 16;
  l->maxVertexBufferArrayStride = 2048;
  l->maxInterStageShaderVariables = 16;
  l->maxColorAttachments = 8;
  l->maxColorAttachmentBytesPerSample = 32;
  l->maxComputeWorkgroupStorageSize = 32768;
  l->maxComputeInvocationsPerWorkgroup = 1024;
  l->maxComputeWorkgroupSizeX = 1024;
  l->maxComputeWorkgroupSizeY = 1024;
  l->maxComputeWorkgroupSizeZ = 64;
  l->maxComputeWorkgroupsPerDimension = 65535;
  return WGPUStatus_Success;
}

void wgpuAdapterGetFeatures(WGPUAdapter, WGPUSupportedFeatures* features) {
  static const WGPUFeatureName supported[] = {WGPUFeatureName_TextureCompressionBC};
  features->featureCount = 1;
  features->features = supported;
}

WGPUBool wgpuAdapterHasFeature(WGPUAdapter, WGPUFeatureName feature) {
  return feature == WGPUFeatureName_TextureCompressionBC;
}

void wgpuSupportedFeaturesFreeMembers(WGPUSupportedFeatures) {}

WGPUFuture wgpuAdapterRequestDevice(WGPUAdapter, WGPUDeviceDescriptor const*, WGPURequestDeviceCallbackInfo info) {
  auto* device = new WGPUDeviceImpl;
  device->gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
  if (device->gpu == nullptr || (SDL_GetGPUShaderFormats(device->gpu) & SDL_GPU_SHADERFORMAT_MSL) == 0) {
    fail("SDL_CreateGPUDevice (MSL)");
    drop(device);
    return make_future(info.mode, [info] {
      info.callback(WGPURequestDeviceStatus_Error, nullptr, view_of("SDL_GPU device unavailable"), info.userdata1,
                    info.userdata2);
    });
  }
  device->queue = new WGPUQueueImpl;
  device->queue->device = device;
  const SDL_GPUSamplerCreateInfo samplerInfo{};
  device->defaultSampler = SDL_CreateGPUSampler(device->gpu, &samplerInfo);
  const SDL_GPUBufferCreateInfo sizesInfo{SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
                                              SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                              SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                          256, 0};
  device->sizesBuffer = SDL_CreateGPUBuffer(device->gpu, &sizesInfo);
  int count = 0;
  if (SDL_Window** windows = SDL_GetWindows(&count)) {
    device->window = count > 0 ? windows[0] : nullptr;
    SDL_free(windows);
  }
  return make_future(info.mode, [info, device] {
    info.callback(WGPURequestDeviceStatus_Success, device, view_of(""), info.userdata1, info.userdata2);
  });
}

void wgpuDeviceAddRef(WGPUDevice d) { retain(d); }
void wgpuDeviceRelease(WGPUDevice d) { drop(d); }
WGPUQueue wgpuDeviceGetQueue(WGPUDevice d) { return retain(d->queue); }
void wgpuQueueRelease(WGPUQueue) {}  // the queue lives as long as its device

WGPUStatus wgpuDeviceGetLimits(WGPUDevice, WGPULimits* limits) { return wgpuAdapterGetLimits(nullptr, limits); }
WGPUBool wgpuDeviceHasFeature(WGPUDevice, WGPUFeatureName feature) { return wgpuAdapterHasFeature(nullptr, feature); }
void wgpuDeviceSetLoggingCallback(WGPUDevice d, WGPULoggingCallbackInfo info) { d->logging = info; }
void wgpuDevicePushErrorScope(WGPUDevice, WGPUErrorFilter) {}

WGPUFuture wgpuDevicePopErrorScope(WGPUDevice, WGPUPopErrorScopeCallbackInfo info) {
  return make_future(info.mode, [info] {
    info.callback(WGPUPopErrorScopeStatus_Success, WGPUErrorType_NoError, view_of(""), info.userdata1,
                  info.userdata2);
  });
}

// MetalFX interop (shared Metal textures and fences) is Dawn-specific: report it unavailable.
WGPUSharedTextureMemory wgpuDeviceImportSharedTextureMemory(WGPUDevice, WGPUSharedTextureMemoryDescriptor const*) {
  return nullptr;
}
WGPUSharedFence wgpuDeviceImportSharedFence(WGPUDevice, WGPUSharedFenceDescriptor const*) { return nullptr; }
WGPUTexture wgpuSharedTextureMemoryCreateTexture(WGPUSharedTextureMemory, WGPUTextureDescriptor const*) {
  return nullptr;
}
WGPUStatus wgpuSharedTextureMemoryBeginAccess(WGPUSharedTextureMemory, WGPUTexture,
                                            WGPUSharedTextureMemoryBeginAccessDescriptor const*) {
  return WGPUStatus_Error;
}
WGPUStatus wgpuSharedTextureMemoryEndAccess(WGPUSharedTextureMemory, WGPUTexture,
                                          WGPUSharedTextureMemoryEndAccessState*) {
  return WGPUStatus_Error;
}
void wgpuSharedTextureMemoryEndAccessStateFreeMembers(WGPUSharedTextureMemoryEndAccessState) {}
WGPUStatus wgpuSharedTextureMemoryGetProperties(WGPUSharedTextureMemory, WGPUSharedTextureMemoryProperties*) {
  return WGPUStatus_Error;
}
void wgpuSharedTextureMemoryRelease(WGPUSharedTextureMemory m) { drop(m); }
void wgpuSharedFenceExportInfo(WGPUSharedFence, WGPUSharedFenceExportInfo*) {}
void wgpuSharedFenceRelease(WGPUSharedFence f) { drop(f); }
void wgpuQuerySetRelease(WGPUQuerySet q) { drop(q); }

// --- Buffers ---------------------------------------------------------------------------------------------------

WGPUBuffer wgpuDeviceCreateBuffer(WGPUDevice d, WGPUBufferDescriptor const* desc) {
  auto* b = new WGPUBufferImpl;
  b->device = Ref<WGPUDeviceImpl>(d);
  b->size = desc->size;
  b->usage = desc->usage;
  b->cpu.assign(desc->size, 0);
  SDL_GPUBufferUsageFlags usage = 0;
  if (desc->usage & WGPUBufferUsage_Vertex) usage |= SDL_GPU_BUFFERUSAGE_VERTEX;
  if (desc->usage & WGPUBufferUsage_Index) usage |= SDL_GPU_BUFFERUSAGE_INDEX;
  if (desc->usage & WGPUBufferUsage_Indirect) usage |= SDL_GPU_BUFFERUSAGE_INDIRECT;
  if (desc->usage & WGPUBufferUsage_Storage) {
    usage |= SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
             SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
  }
  if (usage != 0) {
    const SDL_GPUBufferCreateInfo info{usage, static_cast<Uint32>(desc->size), 0};
    b->gpu = SDL_CreateGPUBuffer(d->gpu, &info);
    if (b->gpu == nullptr) fail("SDL_CreateGPUBuffer");
  }
  return b;
}

void wgpuBufferAddRef(WGPUBuffer b) { retain(b); }
void wgpuBufferRelease(WGPUBuffer b) { drop(b); }

void wgpuBufferDestroy(WGPUBuffer b) {
  if (b->gpu != nullptr) {
    SDL_ReleaseGPUBuffer(b->device->gpu, b->gpu);
    b->gpu = nullptr;
  }
}

WGPUFuture wgpuBufferMapAsync(WGPUBuffer, WGPUMapMode, size_t, size_t, WGPUBufferMapCallbackInfo info) {
  // Uploads copy out at submit and readbacks complete at submit, so the CPU copy is always ready.
  return make_future(info.mode, [info] {
    info.callback(WGPUMapAsyncStatus_Success, view_of(""), info.userdata1, info.userdata2);
  });
}

void* wgpuBufferGetMappedRange(WGPUBuffer b, size_t offset, size_t) { return b->cpu.data() + offset; }
void const* wgpuBufferGetConstMappedRange(WGPUBuffer b, size_t offset, size_t) { return b->cpu.data() + offset; }
void wgpuBufferUnmap(WGPUBuffer) {}

// --- Textures and samplers ---------------------------------------------------------------------------------

WGPUTexture wgpuDeviceCreateTexture(WGPUDevice d, WGPUTextureDescriptor const* desc) {
  auto* t = new WGPUTextureImpl;
  t->device = Ref<WGPUDeviceImpl>(d);
  t->format = desc->format;
  t->usage = desc->usage;
  t->width = desc->size.width;
  t->height = desc->size.height;
  t->layers = std::max(1u, desc->size.depthOrArrayLayers);
  t->mips = std::max(1u, desc->mipLevelCount);
  t->samples = std::max(1u, desc->sampleCount);
  SDL_GPUTextureUsageFlags usage = 0;
  if (desc->usage & WGPUTextureUsage_TextureBinding) usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
  if (desc->usage & WGPUTextureUsage_RenderAttachment) {
    usage |= is_depth(desc->format) ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  }
  if (desc->usage & WGPUTextureUsage_StorageBinding) {
    usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
  }
  if (usage == 0) usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  const SDL_GPUTextureCreateInfo info{
      desc->dimension == WGPUTextureDimension_3D ? SDL_GPU_TEXTURETYPE_3D
      : t->layers > 1                            ? SDL_GPU_TEXTURETYPE_2D_ARRAY
                                                 : SDL_GPU_TEXTURETYPE_2D,
      format_info(desc->format).sdl,
      usage,
      t->width,
      t->height,
      t->layers,
      t->mips,
      sample_count(t->samples),
      0};
  t->tex = SDL_CreateGPUTexture(d->gpu, &info);
  if (t->tex == nullptr) fail("SDL_CreateGPUTexture");
  return t;
}

void wgpuTextureAddRef(WGPUTexture t) { retain(t); }
void wgpuTextureRelease(WGPUTexture t) { drop(t); }

WGPUTextureView wgpuTextureCreateView(WGPUTexture t, WGPUTextureViewDescriptor const* desc) {
  auto* v = new WGPUTextureViewImpl;
  v->texture = Ref<WGPUTextureImpl>(t);
  if (desc != nullptr) {
    v->baseMip = desc->baseMipLevel;
    v->baseLayer = desc->baseArrayLayer;
  }
  return v;
}

void wgpuTextureViewAddRef(WGPUTextureView v) { retain(v); }
void wgpuTextureViewRelease(WGPUTextureView v) { drop(v); }

WGPUSampler wgpuDeviceCreateSampler(WGPUDevice d, WGPUSamplerDescriptor const* desc) {
  auto* s = new WGPUSamplerImpl;
  s->device = Ref<WGPUDeviceImpl>(d);
  SDL_GPUSamplerCreateInfo info{};
  if (desc != nullptr) {
    info.min_filter = desc->minFilter == WGPUFilterMode_Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    info.mag_filter = desc->magFilter == WGPUFilterMode_Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    info.mipmap_mode = desc->mipmapFilter == WGPUMipmapFilterMode_Linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
                                                                         : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = address_mode(desc->addressModeU);
    info.address_mode_v = address_mode(desc->addressModeV);
    info.address_mode_w = address_mode(desc->addressModeW);
    info.min_lod = desc->lodMinClamp;
    info.max_lod = desc->lodMaxClamp;
    info.max_anisotropy = desc->maxAnisotropy;
    info.enable_anisotropy = desc->maxAnisotropy > 1;
    info.enable_compare = desc->compare != WGPUCompareFunction_Undefined;
    info.compare_op = compare_op(desc->compare);
  }
  s->sampler = SDL_CreateGPUSampler(d->gpu, &info);
  if (s->sampler == nullptr) fail("SDL_CreateGPUSampler");
  return s;
}

void wgpuSamplerAddRef(WGPUSampler s) { retain(s); }
void wgpuSamplerRelease(WGPUSampler s) { drop(s); }

// --- Shaders, layouts, bind groups ------------------------------------------------------------------------------

WGPUShaderModule wgpuDeviceCreateShaderModule(WGPUDevice, WGPUShaderModuleDescriptor const* desc) {
  auto* m = new WGPUShaderModuleImpl;
  for (const WGPUChainedStruct* c = desc->nextInChain; c != nullptr; c = c->next) {
    if (c->sType == WGPUSType_ShaderSourceWGSL) {
      m->wgsl = str(reinterpret_cast<const WGPUShaderSourceWGSL*>(c)->code);
    }
  }
  return m;
}

void wgpuShaderModuleAddRef(WGPUShaderModule m) { retain(m); }
void wgpuShaderModuleRelease(WGPUShaderModule m) { drop(m); }

WGPUBindGroupLayout wgpuDeviceCreateBindGroupLayout(WGPUDevice, WGPUBindGroupLayoutDescriptor const* desc) {
  auto* l = new WGPUBindGroupLayoutImpl;
  l->entries.assign(desc->entries, desc->entries + desc->entryCount);
  std::sort(l->entries.begin(), l->entries.end(),
            [](const auto& a, const auto& b) { return a.binding < b.binding; });
  return l;
}

void wgpuBindGroupLayoutAddRef(WGPUBindGroupLayout l) { retain(l); }
void wgpuBindGroupLayoutRelease(WGPUBindGroupLayout l) { drop(l); }

WGPUPipelineLayout wgpuDeviceCreatePipelineLayout(WGPUDevice, WGPUPipelineLayoutDescriptor const* desc) {
  auto* l = new WGPUPipelineLayoutImpl;
  for (size_t i = 0; i < desc->bindGroupLayoutCount; ++i) {
    l->layouts.emplace_back(desc->bindGroupLayouts[i]);
  }
  return l;
}

void wgpuPipelineLayoutAddRef(WGPUPipelineLayout l) { retain(l); }
void wgpuPipelineLayoutRelease(WGPUPipelineLayout l) { drop(l); }

WGPUBindGroup wgpuDeviceCreateBindGroup(WGPUDevice, WGPUBindGroupDescriptor const* desc) {
  auto* g = new WGPUBindGroupImpl;
  std::map<uint32_t, int> dynamicIndex;
  if (desc->layout != nullptr) {
    int next = 0;
    for (const auto& e : desc->layout->entries) {
      if (e.buffer.hasDynamicOffset) dynamicIndex[e.binding] = next++;
    }
  }
  for (size_t i = 0; i < desc->entryCount; ++i) {
    const WGPUBindGroupEntry& src = desc->entries[i];
    BindEntry e;
    e.buffer = Ref<WGPUBufferImpl>(src.buffer);
    e.offset = src.offset;
    e.size = src.size;
    e.sampler = Ref<WGPUSamplerImpl>(src.sampler);
    e.view = Ref<WGPUTextureViewImpl>(src.textureView);
    if (auto it = dynamicIndex.find(src.binding); it != dynamicIndex.end()) e.dynamicIndex = it->second;
    g->entries[src.binding] = e;
  }
  return g;
}

void wgpuBindGroupAddRef(WGPUBindGroup g) { retain(g); }
void wgpuBindGroupRelease(WGPUBindGroup g) { drop(g); }

// --- Pipelines --------------------------------------------------------------------------------------------------

namespace {
SDL_GPUShader* create_shader(WGPUDeviceImpl* d, const StageMap& m, SDL_GPUShaderStage stage) {
  const SDL_GPUShaderCreateInfo info{m.msl.size(),
                                     reinterpret_cast<const Uint8*>(m.msl.c_str()),
                                     m.entry.c_str(),
                                     SDL_GPU_SHADERFORMAT_MSL,
                                     stage,
                                     m.samplers,
                                     m.storageTextures,
                                     m.storageBuffers,
                                     m.uniformBuffers,
                                     0};
  SDL_GPUShader* shader = SDL_CreateGPUShader(d->gpu, &info);
  if (shader == nullptr) fail("SDL_CreateGPUShader");
  return shader;
}

SDL_GPUPrimitiveType primitive_type(WGPUPrimitiveTopology t) {
  switch (t) {
  case WGPUPrimitiveTopology_TriangleStrip: return SDL_GPU_PRIMITIVETYPE_TRIANGLESTRIP;
  case WGPUPrimitiveTopology_LineList: return SDL_GPU_PRIMITIVETYPE_LINELIST;
  case WGPUPrimitiveTopology_LineStrip: return SDL_GPU_PRIMITIVETYPE_LINESTRIP;
  case WGPUPrimitiveTopology_PointList: return SDL_GPU_PRIMITIVETYPE_POINTLIST;
  default: return SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
  }
}

SDL_GPUVertexElementFormat vertex_format(WGPUVertexFormat f) {
  switch (f) {
  case WGPUVertexFormat_Float32: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
  case WGPUVertexFormat_Float32x2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
  case WGPUVertexFormat_Float32x3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
  case WGPUVertexFormat_Float32x4: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
  case WGPUVertexFormat_Unorm8x4: return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
  case WGPUVertexFormat_Uint32: return SDL_GPU_VERTEXELEMENTFORMAT_UINT;
  default: return SDL_GPU_VERTEXELEMENTFORMAT_INVALID;
  }
}
} // namespace

WGPURenderPipeline wgpuDeviceCreateRenderPipeline(WGPUDevice d, WGPURenderPipelineDescriptor const* desc) {
  auto* p = new WGPURenderPipelineImpl;
  p->device = Ref<WGPUDeviceImpl>(d);
  const std::string vsEntry = str(desc->vertex.entryPoint);
  if (!translate(desc->vertex.module->wgsl, 0, vsEntry.empty() ? "vs_main" : vsEntry, p->vs)) return p;
  const WGPUFragmentState* fragment = desc->fragment;
  if (fragment != nullptr) {
    const std::string fsEntry = str(fragment->entryPoint);
    if (!translate(fragment->module->wgsl, 1, fsEntry.empty() ? "fs_main" : fsEntry, p->fs)) return p;
  }
  SDL_GPUShader* vs = create_shader(d, p->vs, SDL_GPU_SHADERSTAGE_VERTEX);
  SDL_GPUShader* fs = fragment != nullptr ? create_shader(d, p->fs, SDL_GPU_SHADERSTAGE_FRAGMENT) : nullptr;

  std::vector<SDL_GPUVertexBufferDescription> buffers;
  std::vector<SDL_GPUVertexAttribute> attributes;
  for (size_t i = 0; i < desc->vertex.bufferCount; ++i) {
    const WGPUVertexBufferLayout& vb = desc->vertex.buffers[i];
    buffers.push_back({static_cast<Uint32>(i), static_cast<Uint32>(vb.arrayStride),
                       vb.stepMode == WGPUVertexStepMode_Instance ? SDL_GPU_VERTEXINPUTRATE_INSTANCE
                                                                  : SDL_GPU_VERTEXINPUTRATE_VERTEX,
                       0});
    for (size_t a = 0; a < vb.attributeCount; ++a) {
      attributes.push_back({vb.attributes[a].shaderLocation, static_cast<Uint32>(i),
                            vertex_format(vb.attributes[a].format), static_cast<Uint32>(vb.attributes[a].offset)});
    }
  }

  std::vector<SDL_GPUColorTargetDescription> targets;
  if (fragment != nullptr) {
    for (size_t i = 0; i < fragment->targetCount; ++i) {
      const WGPUColorTargetState& t = fragment->targets[i];
      SDL_GPUColorTargetDescription ct{};
      ct.format = format_info(t.format).sdl;
      if (t.blend != nullptr) {
        ct.blend_state.enable_blend = true;
        ct.blend_state.src_color_blendfactor = blend_factor(t.blend->color.srcFactor);
        ct.blend_state.dst_color_blendfactor = blend_factor(t.blend->color.dstFactor);
        ct.blend_state.color_blend_op = blend_op(t.blend->color.operation);
        ct.blend_state.src_alpha_blendfactor = blend_factor(t.blend->alpha.srcFactor);
        ct.blend_state.dst_alpha_blendfactor = blend_factor(t.blend->alpha.dstFactor);
        ct.blend_state.alpha_blend_op = blend_op(t.blend->alpha.operation);
      }
      ct.blend_state.enable_color_write_mask = true;
      SDL_GPUColorComponentFlags mask = 0;
      if (t.writeMask & WGPUColorWriteMask_Red) mask |= SDL_GPU_COLORCOMPONENT_R;
      if (t.writeMask & WGPUColorWriteMask_Green) mask |= SDL_GPU_COLORCOMPONENT_G;
      if (t.writeMask & WGPUColorWriteMask_Blue) mask |= SDL_GPU_COLORCOMPONENT_B;
      if (t.writeMask & WGPUColorWriteMask_Alpha) mask |= SDL_GPU_COLORCOMPONENT_A;
      ct.blend_state.color_write_mask = mask;
      targets.push_back(ct);
    }
  }

  SDL_GPUGraphicsPipelineCreateInfo info{};
  info.vertex_shader = vs;
  info.fragment_shader = fs;
  info.vertex_input_state = {buffers.data(), static_cast<Uint32>(buffers.size()), attributes.data(),
                             static_cast<Uint32>(attributes.size())};
  info.primitive_type = primitive_type(desc->primitive.topology);
  info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
  info.rasterizer_state.cull_mode = desc->primitive.cullMode == WGPUCullMode_Front  ? SDL_GPU_CULLMODE_FRONT
                                    : desc->primitive.cullMode == WGPUCullMode_Back ? SDL_GPU_CULLMODE_BACK
                                                                                    : SDL_GPU_CULLMODE_NONE;
  info.rasterizer_state.front_face = desc->primitive.frontFace == WGPUFrontFace_CW
                                         ? SDL_GPU_FRONTFACE_CLOCKWISE
                                         : SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
  info.rasterizer_state.enable_depth_clip = !desc->primitive.unclippedDepth;
  info.multisample_state.sample_count = sample_count(desc->multisample.count);
  if (const WGPUDepthStencilState* ds = desc->depthStencil) {
    info.depth_stencil_state.enable_depth_test = true;
    info.depth_stencil_state.enable_depth_write = ds->depthWriteEnabled == WGPUOptionalBool_True;
    info.depth_stencil_state.compare_op = compare_op(ds->depthCompare);
    info.rasterizer_state.enable_depth_bias = ds->depthBias != 0 || ds->depthBiasSlopeScale != 0.f;
    info.rasterizer_state.depth_bias_constant_factor = static_cast<float>(ds->depthBias);
    info.rasterizer_state.depth_bias_slope_factor = ds->depthBiasSlopeScale;
    info.rasterizer_state.depth_bias_clamp = ds->depthBiasClamp;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = format_info(ds->format).sdl;
  }
  info.target_info.color_target_descriptions = targets.data();
  info.target_info.num_color_targets = static_cast<Uint32>(targets.size());
  p->pipeline = SDL_CreateGPUGraphicsPipeline(d->gpu, &info);
  if (p->pipeline == nullptr) fail("SDL_CreateGPUGraphicsPipeline");
  if (vs != nullptr) SDL_ReleaseGPUShader(d->gpu, vs);
  if (fs != nullptr) SDL_ReleaseGPUShader(d->gpu, fs);
  return p;
}

void wgpuRenderPipelineAddRef(WGPURenderPipeline p) { retain(p); }
void wgpuRenderPipelineRelease(WGPURenderPipeline p) { drop(p); }

WGPUComputePipeline wgpuDeviceCreateComputePipeline(WGPUDevice d, WGPUComputePipelineDescriptor const* desc) {
  auto* p = new WGPUComputePipelineImpl;
  p->device = Ref<WGPUDeviceImpl>(d);
  const std::string entry = str(desc->compute.entryPoint);
  if (!translate(desc->compute.module->wgsl, 2, entry.empty() ? "main" : entry, p->cs)) return p;
  const SDL_GPUComputePipelineCreateInfo info{p->cs.msl.size(),
                                              reinterpret_cast<const Uint8*>(p->cs.msl.c_str()),
                                              p->cs.entry.c_str(),
                                              SDL_GPU_SHADERFORMAT_MSL,
                                              p->cs.samplers,
                                              p->cs.storageTextures,
                                              p->cs.storageBuffers,
                                              p->cs.rwStorageTextures,
                                              p->cs.rwStorageBuffers,
                                              p->cs.uniformBuffers,
                                              p->cs.workgroup[0],
                                              p->cs.workgroup[1],
                                              p->cs.workgroup[2],
                                              0};
  p->pipeline = SDL_CreateGPUComputePipeline(d->gpu, &info);
  if (p->pipeline == nullptr) fail("SDL_CreateGPUComputePipeline");
  return p;
}

void wgpuComputePipelineRelease(WGPUComputePipeline p) { drop(p); }

// --- Command encoding ---------------------------------------------------------------------------------------------

WGPUCommandEncoder wgpuDeviceCreateCommandEncoder(WGPUDevice d, WGPUCommandEncoderDescriptor const*) {
  auto* e = new WGPUCommandEncoderImpl;
  e->device = Ref<WGPUDeviceImpl>(d);
  return e;
}

void wgpuCommandEncoderRelease(WGPUCommandEncoder e) { drop(e); }

WGPUCommandBuffer wgpuCommandEncoderFinish(WGPUCommandEncoder e, WGPUCommandBufferDescriptor const*) {
  auto* b = new WGPUCommandBufferImpl;
  b->ops = std::move(e->ops);
  return b;
}

void wgpuCommandBufferRelease(WGPUCommandBuffer b) { drop(b); }

void wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder e, WGPUBuffer src, uint64_t srcOffset, WGPUBuffer dst,
                                          uint64_t dstOffset, uint64_t size) {
  e->ops.push_back([src = Ref<WGPUBufferImpl>(src), dst = Ref<WGPUBufferImpl>(dst), srcOffset, dstOffset,
                    size](Replay& r) {
    if (size == 0) return;
    // The CPU copy is current for everything the CPU wrote; GPU-written storage is read back instead.
    std::memcpy(dst->cpu.data() + dstOffset, src->cpu.data() + srcOffset, size);
    if (dst->gpu != nullptr) {
      SDL_GPUTransferBuffer* tb = r.upload(src->cpu.data() + srcOffset, static_cast<uint32_t>(size));
      const SDL_GPUTransferBufferLocation from{tb, 0};
      const SDL_GPUBufferRegion to{dst->gpu, static_cast<Uint32>(dstOffset), static_cast<Uint32>(size)};
      SDL_UploadToGPUBuffer(r.copy_pass(), &from, &to, false);
    }
  });
}

void wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder e, WGPUTexelCopyBufferInfo const* source,
                                           WGPUTexelCopyTextureInfo const* destination,
                                           WGPUExtent3D const* copySize) {
  e->ops.push_back([buf = Ref<WGPUBufferImpl>(source->buffer), layout = source->layout,
                    tex = Ref<WGPUTextureImpl>(destination->texture), mip = destination->mipLevel,
                    origin = destination->origin, extent = *copySize](Replay& r) {
    const FormatInfo f = format_info(tex->format);
    const uint32_t blockRows = (extent.height + f.blockDim - 1) / f.blockDim;
    const uint32_t rowsPerImage = layout.rowsPerImage != 0 ? layout.rowsPerImage : extent.height;
    const uint32_t layerBytes = layout.bytesPerRow * ((rowsPerImage + f.blockDim - 1) / f.blockDim);
    const uint32_t bytes = layerBytes * (extent.depthOrArrayLayers - 1) + layout.bytesPerRow * blockRows;
    SDL_GPUTransferBuffer* tb = r.upload(buf->cpu.data() + layout.offset, bytes);
    const SDL_GPUTextureTransferInfo from{tb, 0, layout.bytesPerRow / f.blockBytes * f.blockDim, rowsPerImage};
    const SDL_GPUTextureRegion to{tex->tex,    mip,          origin.z, origin.x, origin.y, 0,
                                  extent.width, extent.height, extent.depthOrArrayLayers};
    SDL_UploadToGPUTexture(r.copy_pass(), &from, &to, false);
  });
}

void wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder e, WGPUTexelCopyTextureInfo const* source,
                                           WGPUTexelCopyBufferInfo const* destination,
                                           WGPUExtent3D const* copySize) {
  e->ops.push_back([tex = Ref<WGPUTextureImpl>(source->texture), mip = source->mipLevel, origin = source->origin,
                    buf = Ref<WGPUBufferImpl>(destination->buffer), layout = destination->layout,
                    extent = *copySize](Replay& r) {
    const FormatInfo f = format_info(tex->format);
    const uint32_t rowBytes = extent.width / f.blockDim * f.blockBytes;
    const uint32_t rows = (extent.height + f.blockDim - 1) / f.blockDim;
    const SDL_GPUTransferBufferCreateInfo info{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, rowBytes * rows, 0};
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(r.device->gpu, &info);
    const SDL_GPUTextureRegion from{r.resolve(tex.get()), mip, origin.z, origin.x, origin.y, 0,
                                    extent.width,          extent.height, 1};
    const SDL_GPUTextureTransferInfo to{tb, 0, extent.width, extent.height};
    SDL_DownloadFromGPUTexture(r.copy_pass(), &from, &to);
    r.downloads.push_back({tb, retain(buf.get()), layout.offset, rows, rowBytes, layout.bytesPerRow});
  });
}

void wgpuCommandEncoderCopyTextureToTexture(WGPUCommandEncoder e, WGPUTexelCopyTextureInfo const* source,
                                            WGPUTexelCopyTextureInfo const* destination,
                                            WGPUExtent3D const* copySize) {
  e->ops.push_back([src = Ref<WGPUTextureImpl>(source->texture), srcMip = source->mipLevel,
                    srcOrigin = source->origin, dst = Ref<WGPUTextureImpl>(destination->texture),
                    dstMip = destination->mipLevel, dstOrigin = destination->origin,
                    extent = *copySize](Replay& r) {
    const SDL_GPUTextureLocation from{r.resolve(src.get()), srcMip, srcOrigin.z, srcOrigin.x, srcOrigin.y, 0};
    const SDL_GPUTextureLocation to{r.resolve(dst.get()), dstMip, dstOrigin.z, dstOrigin.x, dstOrigin.y, 0};
    SDL_CopyGPUTextureToTexture(r.copy_pass(), &from, &to, extent.width, extent.height,
                                std::max(1u, extent.depthOrArrayLayers), false);
  });
}

// --- Render passes ----------------------------------------------------------------------------------------------

WGPURenderPassEncoder wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder e, WGPURenderPassDescriptor const* desc) {
  struct Color {
    Ref<WGPUTextureViewImpl> view, resolve;
    WGPULoadOp load;
    WGPUStoreOp store;
    WGPUColor clear;
    uint32_t slice;
  };
  std::vector<Color> colors;
  for (size_t i = 0; i < desc->colorAttachmentCount; ++i) {
    const WGPURenderPassColorAttachment& a = desc->colorAttachments[i];
    colors.push_back({Ref<WGPUTextureViewImpl>(a.view), Ref<WGPUTextureViewImpl>(a.resolveTarget), a.loadOp,
                      a.storeOp, a.clearValue, a.depthSlice == WGPU_DEPTH_SLICE_UNDEFINED ? 0 : a.depthSlice});
  }
  const bool hasDepth = desc->depthStencilAttachment != nullptr;
  WGPURenderPassDepthStencilAttachment depth{};
  Ref<WGPUTextureViewImpl> depthView;
  if (hasDepth) {
    depth = *desc->depthStencilAttachment;
    depthView = Ref<WGPUTextureViewImpl>(depth.view);
  }
  e->ops.push_back([colors, hasDepth, depth, depthView](Replay& r) {
    r.end_copy();
    std::vector<SDL_GPUColorTargetInfo> targets;
    r.skipPass = false;
    for (const auto& c : colors) {
      SDL_GPUColorTargetInfo t{};
      t.texture = r.resolve(c.view->texture.get());
      if (t.texture == nullptr) r.skipPass = true;
      t.mip_level = c.view->baseMip;
      t.layer_or_depth_plane = c.view->baseLayer + c.slice;
      t.clear_color = {static_cast<float>(c.clear.r), static_cast<float>(c.clear.g), static_cast<float>(c.clear.b),
                       static_cast<float>(c.clear.a)};
      t.load_op = load_op(c.load);
      t.store_op = store_op(c.store);
      if (c.resolve) {
        t.resolve_texture = r.resolve(c.resolve->texture.get());
        t.resolve_mip_level = c.resolve->baseMip;
        t.resolve_layer = c.resolve->baseLayer;
        t.store_op = c.store == WGPUStoreOp_Discard ? SDL_GPU_STOREOP_RESOLVE : SDL_GPU_STOREOP_RESOLVE_AND_STORE;
      }
      targets.push_back(t);
    }
    SDL_GPUDepthStencilTargetInfo ds{};
    if (hasDepth) {
      ds.texture = r.resolve(depthView->texture.get());
      ds.clear_depth = depth.depthClearValue;
      ds.load_op = load_op(depth.depthLoadOp);
      ds.store_op = depth.depthReadOnly ? SDL_GPU_STOREOP_STORE : store_op(depth.depthStoreOp);
      ds.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
      ds.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    }
    if (r.skipPass) return;
    r.render = SDL_BeginGPURenderPass(r.cmd, targets.data(), static_cast<Uint32>(targets.size()),
                                      hasDepth ? &ds : nullptr);
    r.pipeline = nullptr;
    for (auto& g : r.groups) g = nullptr;
  });
  auto* p = new WGPURenderPassEncoderImpl;
  p->encoder = Ref<WGPUCommandEncoderImpl>(e);
  return p;
}

void wgpuRenderPassEncoderRelease(WGPURenderPassEncoder p) { drop(p); }

void wgpuRenderPassEncoderEnd(WGPURenderPassEncoder p) {
  p->encoder->ops.push_back([](Replay& r) {
    if (r.render != nullptr) {
      SDL_EndGPURenderPass(r.render);
      r.render = nullptr;
    }
  });
}

void wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder p, WGPURenderPipeline pipeline) {
  p->encoder->ops.push_back([pipe = Ref<WGPURenderPipelineImpl>(pipeline)](Replay& r) {
    if (r.render == nullptr || pipe->pipeline == nullptr) return;
    SDL_BindGPUGraphicsPipeline(r.render, pipe->pipeline);
    r.pipeline = pipe.get();
  });
}

void wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder p, uint32_t index, WGPUBindGroup group,
                                       size_t dynamicOffsetCount, uint32_t const* dynamicOffsets) {
  std::vector<uint32_t> offsets(dynamicOffsets, dynamicOffsets + dynamicOffsetCount);
  p->encoder->ops.push_back([index, g = Ref<WGPUBindGroupImpl>(group), offsets](Replay& r) {
    set_bind_group(r, index, g.get(), offsets);
  });
}

void wgpuRenderPassEncoderSetIndexBuffer(WGPURenderPassEncoder p, WGPUBuffer buffer, WGPUIndexFormat format,
                                         uint64_t offset, uint64_t) {
  p->encoder->ops.push_back([b = Ref<WGPUBufferImpl>(buffer), format, offset](Replay& r) {
    if (r.render == nullptr || b->gpu == nullptr) return;
    const SDL_GPUBufferBinding binding{b->gpu, static_cast<Uint32>(offset)};
    SDL_BindGPUIndexBuffer(r.render, &binding,
                           format == WGPUIndexFormat_Uint32 ? SDL_GPU_INDEXELEMENTSIZE_32BIT
                                                            : SDL_GPU_INDEXELEMENTSIZE_16BIT);
  });
}

void wgpuRenderPassEncoderSetVertexBuffer(WGPURenderPassEncoder p, uint32_t slot, WGPUBuffer buffer, uint64_t offset,
                                          uint64_t) {
  p->encoder->ops.push_back([slot, b = Ref<WGPUBufferImpl>(buffer), offset](Replay& r) {
    if (r.render == nullptr || !b || b->gpu == nullptr) return;
    const SDL_GPUBufferBinding binding{b->gpu, static_cast<Uint32>(offset)};
    SDL_BindGPUVertexBuffers(r.render, slot, &binding, 1);
  });
}

void wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder p, float x, float y, float width, float height,
                                      float minDepth, float maxDepth) {
  p->encoder->ops.push_back([=](Replay& r) {
    if (r.render == nullptr) return;
    const SDL_GPUViewport vp{x, y, width, height, minDepth, maxDepth};
    SDL_SetGPUViewport(r.render, &vp);
  });
}

void wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder p, uint32_t x, uint32_t y, uint32_t width,
                                         uint32_t height) {
  p->encoder->ops.push_back([=](Replay& r) {
    if (r.render == nullptr) return;
    const SDL_Rect rect{static_cast<int>(x), static_cast<int>(y), static_cast<int>(width), static_cast<int>(height)};
    SDL_SetGPUScissor(r.render, &rect);
  });
}

void wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder p, WGPUColor const* color) {
  const SDL_FColor c{static_cast<float>(color->r), static_cast<float>(color->g), static_cast<float>(color->b),
                     static_cast<float>(color->a)};
  p->encoder->ops.push_back([c](Replay& r) {
    if (r.render != nullptr) SDL_SetGPUBlendConstants(r.render, c);
  });
}

void wgpuRenderPassEncoderDraw(WGPURenderPassEncoder p, uint32_t vertexCount, uint32_t instanceCount,
                               uint32_t firstVertex, uint32_t firstInstance) {
  p->encoder->ops.push_back([=](Replay& r) {
    if (r.render == nullptr || r.pipeline == nullptr) return;
    bind_draw(r);
    SDL_DrawGPUPrimitives(r.render, vertexCount, instanceCount, firstVertex, firstInstance);
  });
}

void wgpuRenderPassEncoderDrawIndexed(WGPURenderPassEncoder p, uint32_t indexCount, uint32_t instanceCount,
                                      uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance) {
  p->encoder->ops.push_back([=](Replay& r) {
    if (r.render == nullptr || r.pipeline == nullptr) return;
    bind_draw(r);
    SDL_DrawGPUIndexedPrimitives(r.render, indexCount, instanceCount, firstIndex, baseVertex, firstInstance);
  });
}

void wgpuRenderPassEncoderPushDebugGroup(WGPURenderPassEncoder p, WGPUStringView label) {
  p->encoder->ops.push_back([name = str(label)](Replay& r) { SDL_PushGPUDebugGroup(r.cmd, name.c_str()); });
}

void wgpuRenderPassEncoderPopDebugGroup(WGPURenderPassEncoder p) {
  p->encoder->ops.push_back([](Replay& r) { SDL_PopGPUDebugGroup(r.cmd); });
}

// --- Compute passes ---------------------------------------------------------------------------------------------

WGPUComputePassEncoder wgpuCommandEncoderBeginComputePass(WGPUCommandEncoder e, WGPUComputePassDescriptor const*) {
  e->ops.push_back([](Replay& r) {
    r.end_copy();
    r.compute = nullptr;
    for (auto& g : r.groups) g = nullptr;
  });
  auto* p = new WGPUComputePassEncoderImpl;
  p->encoder = Ref<WGPUCommandEncoderImpl>(e);
  return p;
}

void wgpuComputePassEncoderRelease(WGPUComputePassEncoder p) { drop(p); }
void wgpuComputePassEncoderEnd(WGPUComputePassEncoder) {}

void wgpuComputePassEncoderSetPipeline(WGPUComputePassEncoder p, WGPUComputePipeline pipeline) {
  p->encoder->ops.push_back(
      [pipe = Ref<WGPUComputePipelineImpl>(pipeline)](Replay& r) { r.compute = pipe.get(); });
}

void wgpuComputePassEncoderSetBindGroup(WGPUComputePassEncoder p, uint32_t index, WGPUBindGroup group,
                                        size_t dynamicOffsetCount, uint32_t const* dynamicOffsets) {
  std::vector<uint32_t> offsets(dynamicOffsets, dynamicOffsets + dynamicOffsetCount);
  p->encoder->ops.push_back([index, g = Ref<WGPUBindGroupImpl>(group), offsets](Replay& r) {
    set_bind_group(r, index, g.get(), offsets);
  });
}

void wgpuComputePassEncoderDispatchWorkgroups(WGPUComputePassEncoder p, uint32_t x, uint32_t y, uint32_t z) {
  p->encoder->ops.push_back([=](Replay& r) {
    WGPUComputePipelineImpl* pipe = r.compute;
    if (pipe == nullptr || pipe->pipeline == nullptr) return;
    const StageMap& m = pipe->cs;
    // SDL takes read-write storage at pass begin, so each dispatch is its own compute pass.
    std::vector<SDL_GPUStorageBufferReadWriteBinding> rw(m.rwStorageBuffers, SDL_GPUStorageBufferReadWriteBinding{});
    std::vector<SDL_GPUBuffer*> ro(m.storageBuffers, r.device->sizesBuffer);
    std::vector<SDL_GPUTextureSamplerBinding> samplers(m.samplers, SDL_GPUTextureSamplerBinding{});
    const uint32_t roBase = m.uniformBuffers, rwBase = m.uniformBuffers + m.storageBuffers;
    for (auto& b : rw) b.buffer = r.device->sizesBuffer;
    struct Push {
      uint32_t slot;
      const uint8_t* data;
      uint32_t bytes;
    };
    std::vector<Push> pushes;
    for (const auto& b : m.binds) {
      WGPUBindGroupImpl* group = b.group < 4 ? r.groups[b.group] : nullptr;
      if (group == nullptr) continue;
      auto it = group->entries.find(b.binding);
      if (it == group->entries.end()) continue;
      const BindEntry& e = it->second;
      switch (b.kind) {
      case Kind::Uniform: {
        uint64_t offset = e.offset;
        if (e.dynamicIndex >= 0 && e.dynamicIndex < static_cast<int>(r.dynamicOffsets[b.group].size())) {
          offset += r.dynamicOffsets[b.group][e.dynamicIndex];
        }
        if (e.buffer && offset + b.bytes <= e.buffer->cpu.size()) {
          pushes.push_back({b.slot, e.buffer->cpu.data() + offset, b.bytes});
        }
        break;
      }
      case Kind::Storage:
        if (b.slot >= roBase && b.slot - roBase < ro.size() && e.buffer) ro[b.slot - roBase] = e.buffer->gpu;
        break;
      case Kind::RwStorage:
        if (b.slot >= rwBase && b.slot - rwBase < rw.size() && e.buffer) rw[b.slot - rwBase].buffer = e.buffer->gpu;
        break;
      case Kind::Texture:
        if (b.slot < samplers.size() && e.view) samplers[b.slot].texture = r.resolve(e.view->texture.get());
        break;
      case Kind::Sampler:
        if (b.slot < samplers.size() && e.sampler) samplers[b.slot].sampler = e.sampler->sampler;
        break;
      default:
        break;
      }
    }
    for (auto& s : samplers) {
      if (s.sampler == nullptr) s.sampler = r.device->defaultSampler;
    }
    r.end_copy();
    for (const auto& push : pushes) SDL_PushGPUComputeUniformData(r.cmd, push.slot, push.data, push.bytes);
    SDL_GPUComputePass* pass =
        SDL_BeginGPUComputePass(r.cmd, nullptr, 0, rw.data(), static_cast<Uint32>(rw.size()));
    SDL_BindGPUComputePipeline(pass, pipe->pipeline);
    if (!samplers.empty()) SDL_BindGPUComputeSamplers(pass, 0, samplers.data(), samplers.size());
    if (!ro.empty()) SDL_BindGPUComputeStorageBuffers(pass, 0, ro.data(), ro.size());
    SDL_DispatchGPUCompute(pass, x, y, z);
    SDL_EndGPUComputePass(pass);
  });
}

// --- Queue ------------------------------------------------------------------------------------------------------

void wgpuQueueWriteBuffer(WGPUQueue q, WGPUBuffer buffer, uint64_t offset, void const* data, size_t size) {
  std::memcpy(buffer->cpu.data() + offset, data, size);
  if (buffer->gpu == nullptr || size == 0) return;
  std::vector<uint8_t> bytes(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  q->pending.push_back([b = Ref<WGPUBufferImpl>(buffer), offset, bytes = std::move(bytes)](Replay& r) {
    SDL_GPUTransferBuffer* tb = r.upload(bytes.data(), static_cast<uint32_t>(bytes.size()));
    const SDL_GPUTransferBufferLocation from{tb, 0};
    const SDL_GPUBufferRegion to{b->gpu, static_cast<Uint32>(offset), static_cast<Uint32>(bytes.size())};
    SDL_UploadToGPUBuffer(r.copy_pass(), &from, &to, false);
  });
}

void wgpuQueueWriteTexture(WGPUQueue q, WGPUTexelCopyTextureInfo const* destination, void const* data,
                           size_t dataSize, WGPUTexelCopyBufferLayout const* layout, WGPUExtent3D const* size) {
  std::vector<uint8_t> bytes(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + dataSize);
  q->pending.push_back([tex = Ref<WGPUTextureImpl>(destination->texture), mip = destination->mipLevel,
                        origin = destination->origin, layout = *layout, extent = *size,
                        bytes = std::move(bytes)](Replay& r) {
    const FormatInfo f = format_info(tex->format);
    const uint32_t rowsPerImage = layout.rowsPerImage != 0 ? layout.rowsPerImage : extent.height;
    SDL_GPUTransferBuffer* tb = r.upload(bytes.data() + layout.offset,
                                         static_cast<uint32_t>(bytes.size() - layout.offset));
    const SDL_GPUTextureTransferInfo from{tb, 0, layout.bytesPerRow / f.blockBytes * f.blockDim, rowsPerImage};
    const SDL_GPUTextureRegion to{tex->tex,    mip,          origin.z, origin.x, origin.y, 0,
                                  extent.width, extent.height, std::max(1u, extent.depthOrArrayLayers)};
    SDL_UploadToGPUTexture(r.copy_pass(), &from, &to, false);
  });
}

void wgpuQueueSubmit(WGPUQueue q, size_t commandCount, WGPUCommandBuffer const* commands) {
  WGPUDeviceImpl* d = q->device;
  Replay r;
  r.device = d;
  r.cmd = SDL_AcquireGPUCommandBuffer(d->gpu);
  if (r.cmd == nullptr) {
    fail("SDL_AcquireGPUCommandBuffer");
    return;
  }
  std::vector<Op> pending;
  pending.swap(q->pending);
  for (auto& op : pending) op(r);
  for (size_t i = 0; i < commandCount; ++i) {
    for (auto& op : commands[i]->ops) op(r);
  }
  r.end_copy();
  if (r.render != nullptr) SDL_EndGPURenderPass(r.render);
  if (r.downloads.empty()) {
    if (!SDL_SubmitGPUCommandBuffer(r.cmd)) fail("SDL_SubmitGPUCommandBuffer");
  } else {
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(r.cmd);
    if (fence != nullptr) {
      SDL_WaitForGPUFences(d->gpu, true, &fence, 1);
      SDL_ReleaseGPUFence(d->gpu, fence);
    }
    for (auto& dl : r.downloads) {
      const auto* src = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(d->gpu, dl.transfer, false));
      for (uint32_t row = 0; row < dl.rows; ++row) {
        const uint64_t dst = dl.dstOffset + static_cast<uint64_t>(row) * dl.srcRowBytes;
        if (dst + dl.rowBytes <= dl.dst->cpu.size()) {
          std::memcpy(dl.dst->cpu.data() + dst, src + static_cast<size_t>(row) * dl.rowBytes, dl.rowBytes);
        }
      }
      SDL_UnmapGPUTransferBuffer(d->gpu, dl.transfer);
      SDL_ReleaseGPUTransferBuffer(d->gpu, dl.transfer);
      drop(dl.dst);
    }
  }
  for (SDL_GPUTransferBuffer* tb : r.uploads) SDL_ReleaseGPUTransferBuffer(d->gpu, tb);
}

WGPUFuture wgpuQueueOnSubmittedWorkDone(WGPUQueue q, WGPUQueueWorkDoneCallbackInfo info) {
  WGPUDeviceImpl* d = q->device;
  return make_future(info.mode, [info, d] {
    SDL_WaitForGPUIdle(d->gpu);
    info.callback(WGPUQueueWorkDoneStatus_Success, view_of(""), info.userdata1, info.userdata2);
  });
}

// --- Surface ----------------------------------------------------------------------------------------------------

void wgpuSurfaceAddRef(WGPUSurface s) { retain(s); }
void wgpuSurfaceRelease(WGPUSurface s) { drop(s); }

WGPUStatus wgpuSurfaceGetCapabilities(WGPUSurface, WGPUAdapter, WGPUSurfaceCapabilities* caps) {
  static const WGPUTextureFormat formats[] = {WGPUTextureFormat_BGRA8Unorm};
  static const WGPUPresentMode modes[] = {WGPUPresentMode_Fifo, WGPUPresentMode_Immediate, WGPUPresentMode_Mailbox};
  static const WGPUCompositeAlphaMode alphas[] = {WGPUCompositeAlphaMode_Opaque, WGPUCompositeAlphaMode_Auto};
  caps->usages = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
  caps->formatCount = 1;
  caps->formats = formats;
  caps->presentModeCount = 3;
  caps->presentModes = modes;
  caps->alphaModeCount = 2;
  caps->alphaModes = alphas;
  return WGPUStatus_Success;
}

void wgpuSurfaceCapabilitiesFreeMembers(WGPUSurfaceCapabilities) {}

void wgpuSurfaceConfigure(WGPUSurface s, WGPUSurfaceConfiguration const* config) {
  WGPUDeviceImpl* d = config->device;
  s->device = Ref<WGPUDeviceImpl>(d);
  s->format = config->format;
  s->width = config->width;
  s->height = config->height;
  if (!d->windowClaimed && d->window != nullptr) {
    d->windowClaimed = SDL_ClaimWindowForGPUDevice(d->gpu, d->window);
    if (!d->windowClaimed) fail("SDL_ClaimWindowForGPUDevice");
  }
  if (d->windowClaimed) {
    const SDL_GPUPresentMode mode = config->presentMode == WGPUPresentMode_Immediate ? SDL_GPU_PRESENTMODE_IMMEDIATE
                                    : config->presentMode == WGPUPresentMode_Mailbox ? SDL_GPU_PRESENTMODE_MAILBOX
                                                                                     : SDL_GPU_PRESENTMODE_VSYNC;
    if (!SDL_SetGPUSwapchainParameters(d->gpu, d->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
      SDL_SetGPUSwapchainParameters(d->gpu, d->window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
    }
  }
  auto* t = new WGPUTextureImpl;
  t->device = Ref<WGPUDeviceImpl>(d);
  t->format = config->format;
  t->usage = config->usage;
  t->width = config->width;
  t->height = config->height;
  t->swapchain = true;
  s->current = Ref<WGPUTextureImpl>(t);
  drop(t);
}

void wgpuSurfaceUnconfigure(WGPUSurface s) { s->current = Ref<WGPUTextureImpl>(); }

void wgpuSurfaceGetCurrentTexture(WGPUSurface s, WGPUSurfaceTexture* out) {
  out->texture = retain(s->current.get());
  out->status = out->texture != nullptr ? WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
                                        : WGPUSurfaceGetCurrentTextureStatus_Error;
}

WGPUStatus wgpuSurfacePresent(WGPUSurface) {
  // SDL presents the swapchain texture when the command buffer that acquired it is submitted.
  return WGPUStatus_Success;
}

} // extern "C"
