#include "imgui.hpp"

#include <cstddef>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>

#include "fs_helper.hpp"
#include "internal.hpp"
#include "gpu/gpu.hpp"
#include "window.hpp"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "aurora/profiling.hpp"

namespace aurora::imgui {
static Module Log("aurora::imgui");
static float g_scale;
static std::string g_imguiLog{};
static bool g_useSdlRenderer = false;
// Set once ImGui::Render() has produced this frame's draw data. Interpolation encodes up to four
// ImGui passes per frame, and every one of them used to rebuild the draw lists from scratch.
static bool g_frameDataBuilt = false;

// An image handed to ImGui (add_texture). The caller keeps its ImTextureID for good (the runtime's menus hold theirs in
// statics), but the texture behind it is remade with the device after a loss (gpu::device_lost), so the ID is a
// number of our own, 1 + the entry's index, which render_frame_data swaps for the texture's SDL_GPUTexture (what
// ImGui 1.92.2's SDL_GPU backend binds). The pixels are kept to make it again.
struct GpuTexture {
  gpu::Texture texture;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

static std::vector<SDL_Texture*> g_sdlTextures;
static std::vector<std::unique_ptr<GpuTexture>> g_gpuTextures;

static gpu::Texture make_gpu_texture(const GpuTexture& entry) noexcept {
  auto texture = gpu::create_texture_2d(entry.width, entry.height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER, "imgui texture");
  if (texture) {
    gpu::write_texture(*texture, 0, 0, entry.width, entry.height, 0, 0, entry.rgba.data(), 4 * entry.width,
                       entry.height);
  }
  return texture;
}

// Our IDs (1 + index) in this frame's draw lists, as the textures' handles; an image whose texture failed draws
// nothing rather than reading a handle that isn't one.
static void resolve_texture_ids(ImDrawData* data) noexcept {
  if (data == nullptr || g_gpuTextures.empty()) {
    return;
  }
  for (ImDrawList* list : data->CmdLists) {
    for (ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.TexRef._TexData != nullptr || cmd.UserCallback != nullptr) {
        continue;
      }
      const auto id = static_cast<uintptr_t>(cmd.TexRef._TexID);
      if (id == 0 || id > g_gpuTextures.size()) {
        continue;
      }
      const auto& texture = g_gpuTextures[id - 1]->texture;
      if (texture) {
        cmd.TexRef._TexID = static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture->handle));
      } else {
        cmd.ElemCount = 0;
      }
    }
  }
}

void release_device_objects() noexcept {
  if (g_useSdlRenderer || ImGui::GetCurrentContext() == nullptr || ImGui::GetIO().BackendRendererUserData == nullptr) {
    return;
  }
  // The font atlas's textures are marked destroyed; ImGui asks the new backend to make them again.
  ImGui_ImplSDLGPU3_Shutdown();
  for (auto& entry : g_gpuTextures) {
    entry->texture = {};
  }
}

void restore_device_objects() noexcept {
  if (g_useSdlRenderer || ImGui::GetCurrentContext() == nullptr || ImGui::GetIO().BackendRendererUserData != nullptr) {
    return;
  }
  ImGui_ImplSDLGPU3_InitInfo info;
  info.Device = gpu::g_device;
  info.ColorTargetFormat = gpu::g_graphicsConfig.surfaceFormat;
  info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
  ImGui_ImplSDLGPU3_Init(&info);
  for (auto& entry : g_gpuTextures) {
    entry->texture = make_gpu_texture(*entry);
  }
}

static void recreate_device_objects() noexcept {
  ImGui_ImplSDLGPU3_DestroyDeviceObjects();
  ImGui_ImplSDLGPU3_CreateDeviceObjects();
}

void remove_legacy_ini_file(const char* basePath) noexcept {
  if (basePath == nullptr || *basePath == '\0') {
    return;
  }

  std::error_code ec;
  std::filesystem::remove(fs_path_from_string(basePath) / "imgui.ini", ec);
}

void create_context() noexcept {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  remove_legacy_ini_file(g_config.userPath);
  remove_legacy_ini_file(g_config.cachePath);
  g_imguiLog = std::string{g_config.cachePath} + "/imgui.log";
  io.IniFilename = nullptr;
  io.LogFilename = g_imguiLog.c_str();
  ImGui::LoadIniSettingsFromMemory("", 0);
  io.WantSaveIniSettings = false;
}

void initialize() noexcept {
  ZoneScoped;
  SDL_Renderer* renderer = window::get_sdl_renderer();
  ImGui_ImplSDL3_InitForSDLRenderer(window::get_sdl_window(), renderer);
  g_useSdlRenderer = renderer != nullptr;
  if (g_useSdlRenderer) {
    ImGui_ImplSDLRenderer3_Init(renderer);
  } else {
    ImGui_ImplSDLGPU3_InitInfo info;
    info.Device = gpu::g_device;
    info.ColorTargetFormat = gpu::g_graphicsConfig.surfaceFormat;
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&info);
  }
}

void shutdown() noexcept {
  ZoneScoped;
  // Startup can fail before either backend initializes. A context alone does
  // not mean its renderer/platform backend owns resources to release.
  if (ImGui::GetCurrentContext() != nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData != nullptr) {
      if (g_useSdlRenderer) {
        ImGui_ImplSDLRenderer3_Shutdown();
      } else {
        ImGui_ImplSDLGPU3_Shutdown();
      }
    }
    if (io.BackendPlatformUserData != nullptr) {
      ImGui_ImplSDL3_Shutdown();
    }
    ImGui::DestroyContext();
  }
  for (const auto& texture : g_sdlTextures) {
    SDL_DestroyTexture(texture);
  }
  g_sdlTextures.clear();
  g_gpuTextures.clear();
  g_useSdlRenderer = false;
  g_scale = 0.f;
  g_frameDataBuilt = false;
}

void process_event(const SDL_Event& event) noexcept {
  auto renderEvent = event;
  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      SDL_ConvertEventToRenderCoordinates(renderer, &renderEvent);
    }
  }
  ImGui_ImplSDL3_ProcessEvent(&renderEvent);
}

bool wants_capture_event(const SDL_Event& event) noexcept {
  if (ImGui::GetCurrentContext() == nullptr) {
    return false;
  }

  const ImGuiIO& io = ImGui::GetIO();
  switch (event.type) {
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
  case SDL_EVENT_MOUSE_WHEEL:
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
    return io.WantCaptureMouse;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
  case SDL_EVENT_TEXT_INPUT:
    return io.WantCaptureKeyboard || io.WantTextInput;
  default:
    return false;
  }
}

void new_frame(const AuroraWindowSize& size) noexcept {
  ZoneScoped;
  ImVec2 framebufferScale{
      size.width > 0 ? static_cast<float>(size.native_fb_width) / static_cast<float>(size.width) : 1.0f,
      size.height > 0 ? static_cast<float>(size.native_fb_height) / static_cast<float>(size.height) : 1.0f,
  };
  ImVec2 displaySize{static_cast<float>(size.width), static_cast<float>(size.height)};

  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      float renderScaleX = 1.0f;
      float renderScaleY = 1.0f;
      SDL_GetRenderScale(renderer, &renderScaleX, &renderScaleY);
      if (renderScaleX > 0.0f && renderScaleY > 0.0f &&
          (std::fabs(renderScaleX - 1.0f) > 0.0001f || std::fabs(renderScaleY - 1.0f) > 0.0001f)) {
        int outputWidth = static_cast<int>(size.native_fb_width);
        int outputHeight = static_cast<int>(size.native_fb_height);
        SDL_GetRenderOutputSize(renderer, &outputWidth, &outputHeight);
        displaySize = {
            static_cast<float>(outputWidth) / renderScaleX,
            static_cast<float>(outputHeight) / renderScaleY,
        };
        framebufferScale = {renderScaleX, renderScaleY};
      }
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    g_scale = size.scale;
  } else {
    if (g_scale != size.scale) {
      if (g_scale > 0.f) {
        Log.info("Display scale {} -> {}: ImGui's device objects recreated", g_scale, size.scale);
        recreate_device_objects();
      }
      g_scale = size.scale;
    }
    ImGui_ImplSDLGPU3_NewFrame();
  }
  ImGui_ImplSDL3_NewFrame();

  ImGuiIO& io = ImGui::GetIO();
  io.DisplayFramebufferScale = framebufferScale;
  ImGui::GetIO().DisplaySize = displaySize;
  ImGui::NewFrame();
  g_frameDataBuilt = false;
}

void render_frame_data() noexcept {
  ZoneScoped;
  if (g_frameDataBuilt) {
    return;
  }
  ImGui::Render();
  auto* data = ImGui::GetDrawData();
  data->FramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
  if (!g_useSdlRenderer) {
    resolve_texture_ids(data);
  }
  g_frameDataBuilt = true;
}

void prepare(SDL_GPUCommandBuffer* cmd) noexcept {
  ZoneScoped;
  render_frame_data();
  if (g_useSdlRenderer) {
    return;
  }
  ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);
}

void render(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) noexcept {
  ZoneScoped;
  render_frame_data();

  auto* data = ImGui::GetDrawData();
  if (g_useSdlRenderer) {
    SDL_Renderer* renderer = window::get_sdl_renderer();
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(data, renderer);
    SDL_RenderPresent(renderer);
  } else {
    SDL_PushGPUDebugGroup(cmd, "Aurora: Dear Imgui");
    ImGui_ImplSDLGPU3_RenderDrawData(data, cmd, pass);
    SDL_PopGPUDebugGroup(cmd);
  }
}

ImTextureID add_texture(uint32_t width, uint32_t height, const uint8_t* data) noexcept {
  if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    SDL_UpdateTexture(texture, nullptr, data, width * 4);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    g_sdlTextures.push_back(texture);
    return reinterpret_cast<ImTextureID>(texture);
  }
  auto entry = std::make_unique<GpuTexture>();
  entry->width = width;
  entry->height = height;
  entry->rgba.assign(data, data + size_t{width} * height * 4);
  entry->texture = make_gpu_texture(*entry);
  if (!entry->texture) {
    return ImTextureID{};
  }
  g_gpuTextures.push_back(std::move(entry));
  return static_cast<ImTextureID>(g_gpuTextures.size());
}
} // namespace aurora::imgui

// C bindings
extern "C" {
ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8) {
  return aurora::imgui::add_texture(width, height, static_cast<const uint8_t*>(rgba8));
}
}
