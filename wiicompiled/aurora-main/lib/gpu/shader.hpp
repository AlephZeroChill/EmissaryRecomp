#pragma once

// aurora's shaders are written once, in a C-like subset that compiles as both MSL and Vulkan GLSL after a
// per-language prelude. assemble() wraps a body in the entry point and resource declarations for the
// device's language, with resources placed in SDL_GPU's slots for that stage.
//
// The dialect:
//  - Types: float int uint bool, vec2-4 ivec2-4 uvec2-4 bvec2-4, mat4 mat3x4. Constructors and
//    swizzles as in GLSL. Every variable is declared with its type and initialized (nothing is
//    zero-initialized implicitly).
//  - Functions available on both: abs min max clamp mix floor fract sqrt exp2 dot normalize length.
//    Prelude helpers: sel(f, t, cond) (cond a scalar bool), round_even(x), extract_bits(v, off, n) (uint),
//    as_float(uint), as_int(uint), as_ivec2/3/4(uvec), as_uint(float).
//  - Shift a vector by a scalar or by a vector of the same signedness.
//  - Textures, by declared name: SAMPLE(t, uv), SAMPLE_BIAS(t, uv, b), SAMPLE_LEVEL(t, uv, lod),
//    LOAD(t, ivec2) (mip 0, returns vec4 or ivec4), LOAD_DEPTH(t, ivec2) (float), DIMS(t) (uvec2).
//  - DISCARD; ends a fragment invocation.
//  - Helper functions (`helpers`) may not touch resources, uniforms or stage variables; only the body can.
//    A helper that reads a storage buffer takes it as a BUFREF parameter, reads it with buf_read(p, i), and
//    the body passes BUFSEL(name).
//
// Stage variables in the body:
//  - vertex:   reads `vidx`, `iidx` (uint); writes `out_pos` (vec4) and `vout.<varying>`.
//  - fragment: reads `frag_pos` (vec4: window x, y, depth, 1/w) and `vin.<varying>`; writes `out_color`
//              (vec4, or ivec4/uvec4 per colorType) and, when writesDepth, `out_depth`.
//  - compute:  reads `gid`, `lid`, `wgid` (uvec3).

#include "gpu.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aurora::gpu::shader {
enum class Language { MSL, GLSL };
Language language() noexcept;

enum class TextureKind { Float, Int, Uint, Depth };

struct Uniform {
  std::string name;     // instance name used in the body
  std::string members;  // "type name; type name[n]; ..." in declaration order (std140-compatible layout)
};
struct TextureBinding {
  std::string name;
  TextureKind kind = TextureKind::Float;
};
struct Varying {
  std::string type;
  std::string name;
  bool flat = false;
};

struct Interface {
  SDL_GPUShaderStage stage = SDL_GPU_SHADERSTAGE_VERTEX;
  bool compute = false;
  // Struct declarations (`struct X { ... };`) used by uniforms or the body, emitted first.
  std::string structs;
  std::vector<Uniform> uniforms;              // slot order
  std::vector<std::string> storageBuffers;    // read-only uint arrays, slot order
  std::vector<std::string> rwStorageBuffers;  // compute only: read-write uint arrays
  std::vector<TextureBinding> textures;       // sampled, each paired with its own sampler, slot order
  std::vector<Varying> varyings;              // vertex outputs / fragment inputs, location order
  std::string colorType = "vec4";             // fragment output type; empty for none
  bool writesDepth = false;
  uint32_t workgroup[3] = {1, 1, 1};
};

// Full source for the device's language (or `lang`).
std::string assemble(const Interface& iface, std::string_view helpers, std::string_view body);
std::string assemble(Language lang, const Interface& iface, std::string_view helpers, std::string_view body);

// Compiles the assembled shader. Logs the source and returns null on failure.
Shader create(const Interface& iface, std::string_view helpers, std::string_view body, const char* label);
ComputePipeline create_compute(const Interface& iface, std::string_view helpers, std::string_view body,
                               const char* label);
} // namespace aurora::gpu::shader
