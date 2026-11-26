#pragma once

// Direct3D 12 shaders: SDL_GPU's D3D12 backend takes DXBC or DXIL only, and aurora's GX shaders are made at run
// time as Vulkan GLSL (shader.cpp). Their SPIR-V goes on through SPIRV-Cross to HLSL, whose registers already
// follow SDL_GPU's D3D12 layout (a SPIR-V descriptor set is the register space, a binding the register), and the
// HLSL to DXBC with Windows' own d3dcompiler_47.dll, or to DXIL with DXC when AURORA_GFX_FLAGS has `dxc` and
// dxcompiler.dll + dxil.dll are beside the program.

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

namespace aurora::gpu::shader::d3d12 {
// The format create() makes for this device (DXBC or DXIL), or 0 when the device takes neither.
SDL_GPUShaderFormat format() noexcept;

// SPIR-V words (entry point `main`) -> bytecode in format(), entry point `main`. Logs and returns empty on failure.
std::vector<uint8_t> compile(const std::vector<uint32_t>& spirv, bool compute, SDL_GPUShaderStage stage,
                             const char* label);
} // namespace aurora::gpu::shader::d3d12
