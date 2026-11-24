# FetchContent PATCH_COMMAND for imgui v1.92.2b (run in its source directory): the SDL_GPU backend's
# ImGui_ImplSDLGPU3_DestroyDeviceObjects() releases the texture transfer buffer and sets it to null but keeps
# TexTransferBufferSize, so after aurora recreates the device objects (a display scale change: the window moved
# to a monitor of another DPI, or Windows reporting scale 0) the next texture upload (the font atlas, made again)
# found the size big enough, mapped the null buffer and memcpy'd into address 0: aurora's render thread crashed
# (seen in Brawl Online's room tests, 0xc0000005 in ucrtbase memcpy under ImGui_ImplSDLGPU3_UpdateTexture).
# The size goes with the buffer now, and an upload whose buffer cannot be mapped waits for the next frame.
set(file backends/imgui_impl_sdlgpu3.cpp)
file(READ "${file}" text)

set(release "if (bd->TexTransferBuffer)  { SDL_ReleaseGPUTransferBuffer(v->Device, bd->TexTransferBuffer); bd->TexTransferBuffer = nullptr; }")
string(FIND "${text}" "${release}" at)
if (at EQUAL -1)
  message(FATAL_ERROR "imgui-sdlgpu3-transfer-buffer.cmake: DestroyDeviceObjects' release not found in ${file}")
endif ()
string(REPLACE "${release}" "${release}\n    bd->TexTransferBufferSize = 0; // aurora: the size goes with the buffer" text "${text}")

set(map "void* texture_ptr = SDL_MapGPUTransferBuffer(v->Device, bd->TexTransferBuffer, true);")
string(FIND "${text}" "${map}" at)
if (at EQUAL -1)
  message(FATAL_ERROR "imgui-sdlgpu3-transfer-buffer.cmake: UpdateTexture's map not found in ${file}")
endif ()
string(REPLACE "${map}" "${map}\n            if (texture_ptr == nullptr) return; // aurora: nothing mapped: upload next frame" text "${text}")

file(WRITE "${file}" "${text}")
