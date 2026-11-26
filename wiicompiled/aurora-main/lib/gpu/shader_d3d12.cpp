#include "shader_d3d12.hpp"

#include "../internal.hpp"
#include "gpu.hpp"

#include <spirv_hlsl.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>

#include <fmt/format.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <xxhash.h>

namespace aurora::gpu::shader::d3d12 {
static Module Log("aurora::gpu::shader");

namespace {
// d3dcompiler_47.dll is part of Windows 10 and 11 (System32), so DXBC needs nothing shipped with the game.
pD3DCompile load_fxc() {
  static pD3DCompile compile = [] {
    HMODULE module = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
      module = LoadLibraryW(L"d3dcompiler_47.dll");
    }
    auto* proc = module != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(module, "D3DCompile")) : nullptr;
    if (proc == nullptr) {
      Log.error("d3dcompiler_47.dll is missing: Direct3D 12 can't compile shaders");
    }
    return proc;
  }();
  return compile;
}

// DXC (dxcompiler.dll, which signs its DXIL with dxil.dll) from the program's folder, for AURORA_GFX_FLAGS=dxc.
DxcCreateInstanceProc load_dxc() {
  static DxcCreateInstanceProc create = [] {
    HMODULE module =
        LoadLibraryExW(L"dxcompiler.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto* proc =
        module != nullptr ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(module, "DxcCreateInstance")) : nullptr;
    if (proc == nullptr) {
      Log.warn("dxcompiler.dll is not beside the program: Direct3D 12 shaders are DXBC");
    }
    return proc;
  }();
  return create;
}

struct Totals {
  std::atomic<uint64_t> shaders{0};
  std::atomic<uint64_t> crossNs{0};
  std::atomic<uint64_t> compileNs{0};
  std::atomic<uint64_t> maxCompileNs{0};
};
Totals g_totals;

uint64_t elapsed_ns(std::chrono::steady_clock::time_point since) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

void log_hlsl(const std::string& hlsl) {
  int line = 1;
  size_t start = 0;
  while (start < hlsl.size()) {
    const auto end = hlsl.find('\n', start);
    Log.error("{:4} {}", line++,
              std::string_view{hlsl}.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
}

// AURORA_SHADER_DUMP=<dir> writes the HLSL too (<n>_<label>.hlsl), beside shader.cpp's GLSL.
void dump_hlsl(const char* label, const std::string& hlsl) {
  static const char* dir = std::getenv("AURORA_SHADER_DUMP");
  if (dir == nullptr) {
    return;
  }
  static std::atomic<uint32_t> counter{0};
  std::string name{label};
  for (auto& c : name) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
      c = '_';
    }
  }
  const auto path = fmt::format("{}/{:05}_{}.hlsl", dir, counter.fetch_add(1), name);
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(hlsl.data(), 1, hlsl.size(), f);
    std::fclose(f);
  }
}

// AURORA_GFX_FLAGS has `dxc` (gpu.cpp lists the flags).
bool dxc_flag() {
  const char* env = std::getenv("AURORA_GFX_FLAGS");
  std::string_view rest = env != nullptr ? env : "";
  while (!rest.empty()) {
    const auto comma = rest.find(',');
    auto item = rest.substr(0, comma);
    while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
    while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
    if (item == "dxc") {
      return true;
    }
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  }
  return false;
}

bool use_dxil() {
  static const bool dxil = [] {
    if (g_device == nullptr || (SDL_GetGPUShaderFormats(g_device) & SDL_GPU_SHADERFORMAT_DXIL) == 0) {
      return false;
    }
    return dxc_flag() && load_dxc() != nullptr;
  }();
  return dxil;
}

std::string to_hlsl(const std::vector<uint32_t>& spirv, uint32_t shaderModel, const char* label) {
  try {
    spirv_cross::CompilerHLSL compiler(spirv.data(), spirv.size());
    spirv_cross::CompilerHLSL::Options options;
    options.shader_model = shaderModel;
    compiler.set_hlsl_options(options);
    return compiler.compile();
  } catch (const std::exception& e) {
    Log.error("Shader {}: SPIR-V -> HLSL failed: {}", label, e.what());
    return {};
  }
}

const char* fxc_profile(bool compute, SDL_GPUShaderStage stage) {
  return compute ? "cs_5_1" : stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vs_5_1" : "ps_5_1";
}

const wchar_t* dxc_profile(bool compute, SDL_GPUShaderStage stage) {
  return compute ? L"cs_6_0" : stage == SDL_GPU_SHADERSTAGE_VERTEX ? L"vs_6_0" : L"ps_6_0";
}

std::vector<uint8_t> compile_dxbc(const std::string& hlsl, bool compute, SDL_GPUShaderStage stage, const char* label) {
  const pD3DCompile compile = load_fxc();
  if (compile == nullptr) {
    return {};
  }
  ID3DBlob* code = nullptr;
  ID3DBlob* errors = nullptr;
  const HRESULT hr = compile(hlsl.data(), hlsl.size(), label, nullptr, nullptr, "main", fxc_profile(compute, stage),
                             D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
  std::vector<uint8_t> out;
  if (SUCCEEDED(hr) && code != nullptr) {
    const auto* bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
    out.assign(bytes, bytes + code->GetBufferSize());
  } else {
    Log.error("Shader {}: HLSL -> DXBC failed (0x{:08X}): {}", label, static_cast<uint32_t>(hr),
              errors != nullptr ? std::string_view(static_cast<const char*>(errors->GetBufferPointer()),
                                                   errors->GetBufferSize())
                                : std::string_view{});
    log_hlsl(hlsl);
  }
  if (code != nullptr) {
    code->Release();
  }
  if (errors != nullptr) {
    errors->Release();
  }
  return out;
}

std::vector<uint8_t> compile_dxil(const std::string& hlsl, bool compute, SDL_GPUShaderStage stage, const char* label) {
  // One compiler per thread: pipelines are built on several workers at once.
  thread_local IDxcCompiler3* compiler = [] {
    IDxcCompiler3* made = nullptr;
    if (FAILED(load_dxc()(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), reinterpret_cast<void**>(&made)))) {
      Log.error("DxcCreateInstance(IDxcCompiler3) failed");
      return static_cast<IDxcCompiler3*>(nullptr);
    }
    return made;
  }();
  if (compiler == nullptr) {
    return {};
  }
  const DxcBuffer source{.Ptr = hlsl.data(), .Size = hlsl.size(), .Encoding = DXC_CP_UTF8};
  LPCWSTR args[] = {L"-E", L"main", L"-T", dxc_profile(compute, stage), L"-O1"};
  IDxcResult* result = nullptr;
  std::vector<uint8_t> out;
  HRESULT status = E_FAIL;
  if (SUCCEEDED(compiler->Compile(&source, args, static_cast<UINT32>(std::size(args)), nullptr,
                                  __uuidof(IDxcResult), reinterpret_cast<void**>(&result))) &&
      result != nullptr) {
    result->GetStatus(&status);
    IDxcBlob* object = nullptr;
    if (SUCCEEDED(status) &&
        SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), reinterpret_cast<void**>(&object), nullptr)) &&
        object != nullptr) {
      const auto* bytes = static_cast<const uint8_t*>(object->GetBufferPointer());
      out.assign(bytes, bytes + object->GetBufferSize());
      object->Release();
    } else {
      IDxcBlobUtf8* errors = nullptr;
      result->GetOutput(DXC_OUT_ERRORS, __uuidof(IDxcBlobUtf8), reinterpret_cast<void**>(&errors), nullptr);
      Log.error("Shader {}: HLSL -> DXIL failed (0x{:08X}): {}", label, static_cast<uint32_t>(status),
                errors != nullptr ? std::string_view(errors->GetStringPointer(), errors->GetStringLength())
                                  : std::string_view{});
      if (errors != nullptr) {
        errors->Release();
      }
      log_hlsl(hlsl);
    }
    result->Release();
  } else {
    Log.error("Shader {}: DXC did not run", label);
  }
  return out;
}

// ---- the compiled shaders on disk ----
// HLSL compilers take 10-100 ms a shader (FXC 23 ms on average for Brawl's GX shaders on a 9800X3D, DXC 18), where a
// Vulkan driver with its own cache takes a few: so every shader made for Direct3D 12 is kept in
// <pipeline cache folder>/shader_cache_d3d12.bin, and a PC compiles it once. The key is the SPIR-V (which says
// everything about the shader), the target format and CacheVersion (bump it when SPIRV-Cross, its options or the
// compile flags change). Records are appended as made; a cut-off last record (a crash) is dropped when read.
constexpr char CacheMagic[8] = {'A', 'U', 'R', 'D', '3', 'D', '1', '2'};
constexpr uint32_t CacheVersion = 1;
constexpr uint64_t CacheLimitBytes = uint64_t{512} << 20;  // a file grown past this starts again

struct CacheKey {
  uint64_t low = 0;
  uint64_t high = 0;
  bool operator==(const CacheKey&) const = default;
};
struct CacheKeyHash {
  size_t operator()(const CacheKey& key) const noexcept { return static_cast<size_t>(key.low ^ (key.high * 31)); }
};
struct CacheRecordHeader {
  uint64_t low;
  uint64_t high;
  uint32_t size;
  uint32_t check;  // low 32 bits of the bytecode's XXH3
};

struct ShaderCache {
  std::mutex mutex;
  bool opened = false;
  std::FILE* file = nullptr;
  std::unordered_map<CacheKey, std::vector<uint8_t>, CacheKeyHash> shaders;
  uint64_t hits = 0;
};
ShaderCache g_cache;

CacheKey cache_key(const std::vector<uint32_t>& spirv, SDL_GPUShaderFormat format) {
  const uint64_t seed = (uint64_t{CacheVersion} << 32) | format;
  const XXH128_hash_t hash = XXH3_128bits_withSeed(spirv.data(), spirv.size() * sizeof(uint32_t), seed);
  return {hash.low64, hash.high64};
}

uint32_t bytecode_check(const std::vector<uint8_t>& bytes) {
  return static_cast<uint32_t>(XXH3_64bits(bytes.data(), bytes.size()));
}

// Under g_cache.mutex.
void open_cache() {
  g_cache.opened = true;
  // AURORA_D3D12_SHADER_CACHE=<file>: that file instead (a test's warm start; one process at a time).
  const char* override = std::getenv("AURORA_D3D12_SHADER_CACHE");
  if (override == nullptr && g_config.pipelineCachePath == nullptr) {
    return;
  }
  const std::string path =
      override != nullptr ? std::string(override) : std::string(g_config.pipelineCachePath) + "/shader_cache_d3d12.bin";
  std::FILE* in = std::fopen(path.c_str(), "rb");
  bool valid = false;
  long end = 0;
  if (in != nullptr) {
    char magic[8]{};
    uint32_t version = 0;
    valid = std::fread(magic, sizeof(magic), 1, in) == 1 && std::fread(&version, sizeof(version), 1, in) == 1 &&
            std::memcmp(magic, CacheMagic, sizeof(magic)) == 0 && version == CacheVersion;
    end = valid ? std::ftell(in) : 0;
    CacheRecordHeader header{};
    while (valid && std::fread(&header, sizeof(header), 1, in) == 1) {
      if (header.size == 0 || header.size > (64u << 20)) {
        break;
      }
      std::vector<uint8_t> bytes(header.size);
      if (std::fread(bytes.data(), 1, bytes.size(), in) != bytes.size() || bytecode_check(bytes) != header.check) {
        break;
      }
      g_cache.shaders.insert_or_assign(CacheKey{header.low, header.high}, std::move(bytes));
      end = std::ftell(in);
    }
    std::fclose(in);
  }
  if (valid && static_cast<uint64_t>(end) < CacheLimitBytes) {
    // Continue after the last whole record (drops a cut-off one).
    g_cache.file = std::fopen(path.c_str(), "r+b");
    if (g_cache.file != nullptr && std::fseek(g_cache.file, end, SEEK_SET) != 0) {
      std::fclose(g_cache.file);
      g_cache.file = nullptr;
    }
  }
  if (g_cache.file == nullptr) {
    g_cache.shaders.clear();
    g_cache.file = std::fopen(path.c_str(), "wb");
    const bool ok = g_cache.file != nullptr && std::fwrite(CacheMagic, sizeof(CacheMagic), 1, g_cache.file) == 1 &&
                    std::fwrite(&CacheVersion, sizeof(CacheVersion), 1, g_cache.file) == 1;
    if (!ok) {
      Log.warn("Can't write the Direct3D 12 shader cache {}: shaders are compiled at every start", path);
      if (g_cache.file != nullptr) {
        std::fclose(g_cache.file);
        g_cache.file = nullptr;
      }
    }
  }
  Log.info("Direct3D 12 shader cache: {} shaders in {}", g_cache.shaders.size(), path);
}

bool cached(const CacheKey& key, std::vector<uint8_t>& out) {
  std::lock_guard lock(g_cache.mutex);
  if (!g_cache.opened) {
    open_cache();
  }
  const auto it = g_cache.shaders.find(key);
  if (it == g_cache.shaders.end()) {
    return false;
  }
  out = it->second;
  ++g_cache.hits;
  return true;
}

void remember(const CacheKey& key, const std::vector<uint8_t>& bytes) {
  std::lock_guard lock(g_cache.mutex);
  if (!g_cache.shaders.try_emplace(key, bytes).second || g_cache.file == nullptr) {
    return;
  }
  const CacheRecordHeader header{key.low, key.high, static_cast<uint32_t>(bytes.size()), bytecode_check(bytes)};
  const bool ok = std::fwrite(&header, sizeof(header), 1, g_cache.file) == 1 &&
                  std::fwrite(bytes.data(), 1, bytes.size(), g_cache.file) == bytes.size() &&
                  std::fflush(g_cache.file) == 0;
  if (!ok) {
    Log.warn("Writing the Direct3D 12 shader cache failed: no more shaders are kept");
    std::fclose(g_cache.file);
    g_cache.file = nullptr;
  }
}
} // namespace

SDL_GPUShaderFormat format() noexcept {
  if (g_device == nullptr) {
    return 0;
  }
  const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(g_device);
  if ((formats & (SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL)) != 0) {
    return 0;  // the device runs shader.cpp's own output
  }
  if (use_dxil()) {
    return SDL_GPU_SHADERFORMAT_DXIL;
  }
  return (formats & SDL_GPU_SHADERFORMAT_DXBC) != 0 ? SDL_GPU_SHADERFORMAT_DXBC : 0;
}

std::vector<uint8_t> compile(const std::vector<uint32_t>& spirv, bool compute, SDL_GPUShaderStage stage,
                             const char* label) {
  const SDL_GPUShaderFormat target = format();
  const bool dxil = target == SDL_GPU_SHADERFORMAT_DXIL;
  const CacheKey key = cache_key(spirv, target);
  if (std::vector<uint8_t> bytes; cached(key, bytes)) {
    return bytes;
  }
  auto start = std::chrono::steady_clock::now();
  const std::string hlsl = to_hlsl(spirv, dxil ? 60 : 51, label);
  const uint64_t crossNs = elapsed_ns(start);
  if (hlsl.empty()) {
    return {};
  }
  dump_hlsl(label, hlsl);
  start = std::chrono::steady_clock::now();
  auto out = dxil ? compile_dxil(hlsl, compute, stage, label) : compile_dxbc(hlsl, compute, stage, label);
  const uint64_t compileNs = elapsed_ns(start);
  if (!out.empty()) {
    remember(key, out);
  }

  // How long the HLSL compiler takes is the first-use cost Vulkan doesn't have: a summary every 100 shaders.
  const uint64_t n = g_totals.shaders.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint64_t cross = g_totals.crossNs.fetch_add(crossNs, std::memory_order_relaxed) + crossNs;
  const uint64_t total = g_totals.compileNs.fetch_add(compileNs, std::memory_order_relaxed) + compileNs;
  uint64_t max = g_totals.maxCompileNs.load(std::memory_order_relaxed);
  while (compileNs > max && !g_totals.maxCompileNs.compare_exchange_weak(max, compileNs, std::memory_order_relaxed)) {
  }
  if (n == 1 || n % 100 == 0) {
    uint64_t hits = 0;
    {
      std::lock_guard lock(g_cache.mutex);
      hits = g_cache.hits;
    }
    Log.info("{} shaders compiled to {} ({} from the cache): SPIRV-Cross {:.2f} ms each, {} {:.2f} ms each (longest "
             "{:.1f} ms)",
             n, dxil ? "DXIL" : "DXBC", hits, cross / 1e6 / n, dxil ? "DXC" : "FXC", total / 1e6 / n,
             g_totals.maxCompileNs.load(std::memory_order_relaxed) / 1e6);
  }
  return out;
}
} // namespace aurora::gpu::shader::d3d12
