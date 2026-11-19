#include "shader.hpp"

#include "../internal.hpp"

#include <fmt/format.h>
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace aurora::gpu::shader {
static Module Log("aurora::gpu::shader");

namespace {
constexpr std::string_view MslPrelude = R"(#include <metal_stdlib>
using namespace metal;
typedef float2 vec2;
typedef float3 vec3;
typedef float4 vec4;
typedef int2 ivec2;
typedef int3 ivec3;
typedef int4 ivec4;
typedef uint2 uvec2;
typedef uint3 uvec3;
typedef uint4 uvec4;
typedef bool2 bvec2;
typedef bool3 bvec3;
typedef bool4 bvec4;
typedef float4x4 mat4;
typedef float3x4 mat3x4;
template <typename T> inline T sel(T f, T t, bool c) { return c ? t : f; }
#define round_even rint
inline float as_float(uint v) { return as_type<float>(v); }
inline uint as_uint(float v) { return as_type<uint>(v); }
inline int as_int(uint v) { return as_type<int>(v); }
inline ivec2 as_ivec2(uvec2 v) { return as_type<int2>(v); }
inline ivec3 as_ivec3(uvec3 v) { return int3(as_type<int>(v.x), as_type<int>(v.y), as_type<int>(v.z)); }
inline ivec4 as_ivec4(uvec4 v) { return as_type<int4>(v); }
#define SAMPLE(t, uv) t.sample(t##_samp, (uv))
#define SAMPLE_BIAS(t, uv, b) t.sample(t##_samp, (uv), bias(b))
#define SAMPLE_LEVEL(t, uv, l) t.sample(t##_samp, (uv), level(l))
#define LOAD(t, c) t.read(uint2(c))
#define LOAD_DEPTH(t, c) t.read(uint2(c))
#define DIMS(t) uint2(t.get_width(), t.get_height())
#define DISCARD discard_fragment()
#define BUFREF const device uint*
#define BUFSEL(n) n
#define buf_read(p, i) (p)[i]
)";

constexpr std::string_view GlslPrelude = R"(#version 450
#define sel_def(T) T sel(T f, T t, bool c) { return c ? t : f; }
sel_def(float) sel_def(vec2) sel_def(vec3) sel_def(vec4)
sel_def(int) sel_def(ivec2) sel_def(ivec3) sel_def(ivec4)
sel_def(uint) sel_def(uvec2) sel_def(uvec3) sel_def(uvec4)
sel_def(bool)
#undef sel_def
#define round_even roundEven
uint extract_bits(uint v, uint off, uint n) { return bitfieldExtract(v, int(off), int(n)); }
float as_float(uint v) { return uintBitsToFloat(v); }
uint as_uint(float v) { return floatBitsToUint(v); }
int as_int(uint v) { return int(v); }
ivec2 as_ivec2(uvec2 v) { return ivec2(v); }
ivec3 as_ivec3(uvec3 v) { return ivec3(v); }
ivec4 as_ivec4(uvec4 v) { return ivec4(v); }
#define SAMPLE(t, uv) texture(t, (uv))
#define SAMPLE_BIAS(t, uv, b) texture(t, (uv), (b))
#define SAMPLE_LEVEL(t, uv, l) textureLod(t, (uv), (l))
#define LOAD(t, c) texelFetch(t, ivec2(c), 0)
#define LOAD_DEPTH(t, c) texelFetch(t, ivec2(c), 0).r
#define DIMS(t) uvec2(textureSize(t, 0))
#define DISCARD discard
#define BUFREF uint
#define BUFSEL(n) n##_sel
)";

std::string_view msl_texture_type(TextureKind kind) {
  switch (kind) {
  case TextureKind::Int:
    return "texture2d<int>";
  case TextureKind::Uint:
    return "texture2d<uint>";
  case TextureKind::Depth:
    return "depth2d<float>";
  case TextureKind::Float:
  default:
    return "texture2d<float>";
  }
}

std::string_view glsl_texture_type(TextureKind kind) {
  switch (kind) {
  case TextureKind::Int:
    return "isampler2D";
  case TextureKind::Uint:
    return "usampler2D";
  case TextureKind::Depth:
  case TextureKind::Float:
  default:
    return "sampler2D";
  }
}

std::string msl_varyings_struct(const Interface& iface) {
  std::string out = "struct VOut {\n  float4 pos [[position]];\n";
  for (size_t i = 0; i < iface.varyings.size(); ++i) {
    const auto& v = iface.varyings[i];
    out += fmt::format("  {} {} [[user(locn{})]]{};\n", v.type, v.name, i, v.flat ? " [[flat]]" : "");
  }
  out += "};\n";
  return out;
}

std::string assemble_msl(const Interface& iface, std::string_view helpers, std::string_view body) {
  std::string out{MslPrelude};
  out += iface.structs;
  out += '\n';
  for (const auto& u : iface.uniforms) {
    out += fmt::format("struct U_{} {{\n{}\n}};\n", u.name, u.members);
  }
  const bool vertex = !iface.compute && iface.stage == SDL_GPU_SHADERSTAGE_VERTEX;
  const bool fragment = !iface.compute && iface.stage == SDL_GPU_SHADERSTAGE_FRAGMENT;
  if (vertex || fragment) {
    out += msl_varyings_struct(iface);
  }
  if (fragment) {
    out += "struct FOut {\n";
    if (!iface.colorType.empty()) {
      out += fmt::format("  {} color [[color(0)]];\n", iface.colorType);
    }
    if (iface.writesDepth) {
      out += "  float depth [[depth(any)]];\n";
    }
    out += "};\n";
  }
  out += helpers;
  out += '\n';

  std::vector<std::string> params;
  if (vertex) {
    params.emplace_back("uint vidx [[vertex_id]]");
    params.emplace_back("uint iidx [[instance_id]]");
  } else if (fragment) {
    params.emplace_back("VOut vin [[stage_in]]");
  } else {
    params.emplace_back("uint3 gid [[thread_position_in_grid]]");
    params.emplace_back("uint3 lid [[thread_position_in_threadgroup]]");
    params.emplace_back("uint3 wgid [[threadgroup_position_in_grid]]");
  }
  uint32_t buffer = 0;
  for (const auto& u : iface.uniforms) {
    params.push_back(fmt::format("constant U_{0}& {0} [[buffer({1})]]", u.name, buffer++));
  }
  for (const auto& name : iface.storageBuffers) {
    params.push_back(fmt::format("const device uint* {} [[buffer({})]]", name, buffer++));
  }
  for (const auto& name : iface.rwStorageBuffers) {
    params.push_back(fmt::format("device uint* {} [[buffer({})]]", name, buffer++));
  }
  for (size_t i = 0; i < iface.textures.size(); ++i) {
    const auto& t = iface.textures[i];
    params.push_back(fmt::format("{} {} [[texture({})]]", msl_texture_type(t.kind), t.name, i));
    params.push_back(fmt::format("sampler {}_samp [[sampler({})]]", t.name, i));
  }
  std::string paramList;
  for (size_t i = 0; i < params.size(); ++i) {
    paramList += i == 0 ? "\n    " : ",\n    ";
    paramList += params[i];
  }

  if (vertex) {
    out += fmt::format("vertex VOut vs_main({}) {{\n  VOut vout;\n  vec4 out_pos = vec4(0.0);\n", paramList);
    out += body;
    out += "\n  vout.pos = out_pos;\n  return vout;\n}\n";
  } else if (fragment) {
    out += fmt::format("fragment FOut fs_main({}) {{\n  vec4 frag_pos = vin.pos;\n", paramList);
    if (!iface.colorType.empty()) {
      out += fmt::format("  {0} out_color = {0}(0);\n", iface.colorType);
    }
    if (iface.writesDepth) {
      out += "  float out_depth = 0.0;\n";
    }
    out += body;
    out += "\n  FOut fout;\n";
    if (!iface.colorType.empty()) {
      out += "  fout.color = out_color;\n";
    }
    if (iface.writesDepth) {
      out += "  fout.depth = out_depth;\n";
    }
    out += "  return fout;\n}\n";
  } else {
    out += fmt::format("kernel void cs_main({}) {{\n", paramList);
    out += body;
    out += "\n}\n";
  }
  return out;
}

std::string assemble_glsl(const Interface& iface, std::string_view helpers, std::string_view body) {
  std::string out{GlslPrelude};
  const bool vertex = !iface.compute && iface.stage == SDL_GPU_SHADERSTAGE_VERTEX;
  const bool fragment = !iface.compute && iface.stage == SDL_GPU_SHADERSTAGE_FRAGMENT;
  // SDL_GPU's SPIR-V sets: graphics resources in set 0 (vertex) or 2 (fragment), uniforms in the next set;
  // compute read-only resources in set 0, read-write in 1, uniforms in 2.
  const uint32_t resourceSet = iface.compute ? 0 : vertex ? 0 : 2;
  const uint32_t uniformSet = iface.compute ? 2 : resourceSet + 1;
  if (iface.compute) {
    out += fmt::format("layout(local_size_x = {}, local_size_y = {}, local_size_z = {}) in;\n", iface.workgroup[0],
                       iface.workgroup[1], iface.workgroup[2]);
  }
  out += iface.structs;
  out += '\n';
  for (size_t i = 0; i < iface.uniforms.size(); ++i) {
    const auto& u = iface.uniforms[i];
    out += fmt::format("layout(std140, set = {}, binding = {}) uniform U_{} {{\n{}\n}} {};\n", uniformSet, i, u.name,
                       u.members, u.name);
  }
  uint32_t binding = 0;
  for (const auto& t : iface.textures) {
    out += fmt::format("layout(set = {}, binding = {}) uniform {} {};\n", resourceSet, binding++,
                       glsl_texture_type(t.kind), t.name);
  }
  for (size_t i = 0; i < iface.storageBuffers.size(); ++i) {
    const auto& name = iface.storageBuffers[i];
    out += fmt::format("layout(std430, set = {}, binding = {}) readonly buffer B_{} {{ uint {}[]; }};\n", resourceSet,
                       binding++, name, name);
    out += fmt::format("const uint {}_sel = {}u;\n", name, i);
  }
  for (size_t i = 0; i < iface.rwStorageBuffers.size(); ++i) {
    const auto& name = iface.rwStorageBuffers[i];
    out += fmt::format("layout(std430, set = 1, binding = {}) buffer B_{} {{ uint {}[]; }};\n", i, name, name);
  }
  // buf_read(sel, i): read word i of the storage buffer selected by BUFSEL(name).
  out += "uint buf_read(uint p, uint i) {\n";
  for (size_t i = 0; i < iface.storageBuffers.size(); ++i) {
    out += fmt::format("  if (p == {}u) {{ return {}[i]; }}\n", i, iface.storageBuffers[i]);
  }
  out += "  return 0u;\n}\n";
  if ((vertex || fragment) && !iface.varyings.empty()) {  // GLSL has no empty interface blocks
    out += fmt::format("layout(location = 0) {} VOut {{\n", vertex ? "out" : "in");
    for (const auto& v : iface.varyings) {
      out += fmt::format("  {}{} {};\n", v.flat ? "flat " : "", v.type, v.name);
    }
    out += fmt::format("}} {};\n", vertex ? "vout" : "vin");
  }
  if (fragment && !iface.colorType.empty()) {
    out += fmt::format("layout(location = 0) out {} fs_color;\n", iface.colorType);
  }
  out += helpers;
  out += "\nvoid main() {\n";
  if (vertex) {
    out += "  uint vidx = uint(gl_VertexIndex);\n  uint iidx = uint(gl_InstanceIndex);\n  vec4 out_pos = vec4(0.0);\n";
    out += body;
    out += "\n  gl_Position = out_pos;\n";
  } else if (fragment) {
    out += "  vec4 frag_pos = gl_FragCoord;\n";
    if (!iface.colorType.empty()) {
      out += fmt::format("  {0} out_color = {0}(0);\n", iface.colorType);
    }
    if (iface.writesDepth) {
      out += "  float out_depth = 0.0;\n";
    }
    out += body;
    if (!iface.colorType.empty()) {
      out += "\n  fs_color = out_color;";
    }
    if (iface.writesDepth) {
      out += "\n  gl_FragDepth = out_depth;";
    }
    out += '\n';
  } else {
    out += "  uvec3 gid = gl_GlobalInvocationID;\n  uvec3 lid = gl_LocalInvocationID;\n  uvec3 wgid = gl_WorkGroupID;\n";
    out += body;
    out += '\n';
  }
  out += "}\n";
  return out;
}

const char* entry_name(const Interface& iface) {
  if (iface.compute) {
    return "cs_main";
  }
  return iface.stage == SDL_GPU_SHADERSTAGE_VERTEX ? "vs_main" : "fs_main";
}

uint32_t count_buffers(const std::vector<std::string>& names) { return static_cast<uint32_t>(names.size()); }

// AURORA_SHADER_DUMP=<dir>: write every assembled shader there (<n>_<label>.metal / .glsl) for offline checks.
void dump_source(const char* label, Language lang, const std::string& source) {
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
  const auto path = fmt::format("{}/{:05}_{}.{}", dir, counter.fetch_add(1), name, lang == Language::MSL ? "metal" : "glsl");
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(source.data(), 1, source.size(), f);
    std::fclose(f);
  }
}

void log_source(const char* label, const std::string& source) {
  Log.error("Shader {} failed to compile: {}", label, SDL_GetError());
  int line = 1;
  size_t start = 0;
  while (start < source.size()) {
    const auto end = source.find('\n', start);
    const auto text = std::string_view{source}.substr(start, end == std::string::npos ? std::string::npos : end - start);
    Log.error("{:4} {}", line++, text);
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
}
} // namespace

namespace {
// Vulkan GLSL -> SPIR-V words with glslang (entry point `main`). Logs the source and returns empty on failure.
std::vector<uint32_t> compile_spirv(glslang_stage_t stage, const std::string& source, const char* label) {
  static const bool initialized = glslang_initialize_process() != 0;
  if (!initialized) {
    Log.error("Shader {}: glslang failed to initialize", label);
    return {};
  }
  const glslang_input_t input{
      .language = GLSLANG_SOURCE_GLSL,
      .stage = stage,
      .client = GLSLANG_CLIENT_VULKAN,
      .client_version = GLSLANG_TARGET_VULKAN_1_1,
      .target_language = GLSLANG_TARGET_SPV,
      .target_language_version = GLSLANG_TARGET_SPV_1_3,
      .code = source.c_str(),
      .default_version = 450,
      .default_profile = GLSLANG_CORE_PROFILE,
      .force_default_version_and_profile = false,
      .forward_compatible = false,
      .messages = GLSLANG_MSG_DEFAULT_BIT,
      .resource = glslang_default_resource(),
  };
  std::vector<uint32_t> words;
  glslang_shader_t* shader = glslang_shader_create(&input);
  glslang_program_t* program = nullptr;
  if (!glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input)) {
    Log.error("Shader {}: GLSL error:\n{}", label, glslang_shader_get_info_log(shader));
    log_source(label, source);
  } else {
    program = glslang_program_create();
    glslang_program_add_shader(program, shader);
    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
      Log.error("Shader {}: GLSL link error:\n{}", label, glslang_program_get_info_log(program));
      log_source(label, source);
    } else {
      glslang_program_SPIRV_generate(program, stage);
      words.resize(glslang_program_SPIRV_get_size(program));
      glslang_program_SPIRV_get(program, words.data());
    }
  }
  if (program != nullptr) {
    glslang_program_delete(program);
  }
  glslang_shader_delete(shader);
  return words;
}

glslang_stage_t glslang_stage(const Interface& iface) {
  if (iface.compute) {
    return GLSLANG_STAGE_COMPUTE;
  }
  return iface.stage == SDL_GPU_SHADERSTAGE_VERTEX ? GLSLANG_STAGE_VERTEX : GLSLANG_STAGE_FRAGMENT;
}
} // namespace

Language language() noexcept {
  if (g_device != nullptr && (SDL_GetGPUShaderFormats(g_device) & SDL_GPU_SHADERFORMAT_MSL) != 0) {
    return Language::MSL;
  }
  return Language::GLSL;
}

std::string assemble(Language lang, const Interface& iface, std::string_view helpers, std::string_view body) {
  return lang == Language::MSL ? assemble_msl(iface, helpers, body) : assemble_glsl(iface, helpers, body);
}

std::string assemble(const Interface& iface, std::string_view helpers, std::string_view body) {
  return assemble(language(), iface, helpers, body);
}

Shader create(const Interface& iface, std::string_view helpers, std::string_view body, const char* label) {
  const Language lang = language();
  const auto source = assemble(lang, iface, helpers, body);
  dump_source(label, lang, source);
  std::vector<uint32_t> spirv;
  if (lang == Language::GLSL) {
    spirv = compile_spirv(glslang_stage(iface), source, label);
    if (spirv.empty()) {
      return nullptr;
    }
  }
  const SDL_GPUShaderCreateInfo info{
      .code_size = lang == Language::GLSL ? spirv.size() * sizeof(uint32_t) : source.size(),
      .code = lang == Language::GLSL ? reinterpret_cast<const Uint8*>(spirv.data())
                                     : reinterpret_cast<const Uint8*>(source.c_str()),
      .entrypoint = lang == Language::GLSL ? "main" : entry_name(iface),
      .format = lang == Language::GLSL ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_MSL,
      .stage = iface.stage,
      .num_samplers = static_cast<Uint32>(iface.textures.size()),
      .num_storage_textures = 0,
      .num_storage_buffers = count_buffers(iface.storageBuffers),
      .num_uniform_buffers = static_cast<Uint32>(iface.uniforms.size()),
  };
  SDL_GPUShader* handle = SDL_CreateGPUShader(g_device, &info);
  if (handle == nullptr) {
    log_source(label, source);
    return nullptr;
  }
  auto shader = std::make_shared<ShaderObject>();
  shader->handle = handle;
  return shader;
}

ComputePipeline create_compute(const Interface& iface, std::string_view helpers, std::string_view body,
                               const char* label) {
  const Language lang = language();
  Interface compute = iface;
  compute.compute = true;
  const auto source = assemble(lang, compute, helpers, body);
  dump_source(label, lang, source);
  std::vector<uint32_t> spirv;
  if (lang == Language::GLSL) {
    spirv = compile_spirv(GLSLANG_STAGE_COMPUTE, source, label);
    if (spirv.empty()) {
      return nullptr;
    }
  }
  const SDL_GPUComputePipelineCreateInfo info{
      .code_size = lang == Language::GLSL ? spirv.size() * sizeof(uint32_t) : source.size(),
      .code = lang == Language::GLSL ? reinterpret_cast<const Uint8*>(spirv.data())
                                     : reinterpret_cast<const Uint8*>(source.c_str()),
      .entrypoint = lang == Language::GLSL ? "main" : "cs_main",
      .format = lang == Language::GLSL ? SDL_GPU_SHADERFORMAT_SPIRV : SDL_GPU_SHADERFORMAT_MSL,
      .num_samplers = static_cast<Uint32>(compute.textures.size()),
      .num_readonly_storage_textures = 0,
      .num_readonly_storage_buffers = count_buffers(compute.storageBuffers),
      .num_readwrite_storage_textures = 0,
      .num_readwrite_storage_buffers = count_buffers(compute.rwStorageBuffers),
      .num_uniform_buffers = static_cast<Uint32>(compute.uniforms.size()),
      .threadcount_x = compute.workgroup[0],
      .threadcount_y = compute.workgroup[1],
      .threadcount_z = compute.workgroup[2],
  };
  SDL_GPUComputePipeline* handle = SDL_CreateGPUComputePipeline(g_device, &info);
  if (handle == nullptr) {
    log_source(label, source);
    return nullptr;
  }
  auto pipeline = std::make_shared<ComputePipelineObject>();
  pipeline->handle = handle;
  return pipeline;
}
} // namespace aurora::gpu::shader
