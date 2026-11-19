#pragma once

#include <aurora/event.h>

union SDL_Event;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;

namespace aurora::imgui {
void create_context() noexcept;
void initialize() noexcept;
void shutdown() noexcept;

void process_event(const SDL_Event& event) noexcept;
bool wants_capture_event(const SDL_Event& event) noexcept;
void new_frame(const AuroraWindowSize& size) noexcept;
// Build this frame's ImGui draw data. Idempotent for the rest of the frame: the lists are built
// once and every presentation slot replays them. Reset by new_frame.
void render_frame_data() noexcept;
// Uploads this frame's vertex/index data on `cmd` in a copy pass of its own. Call it on the same command
// buffer before beginning the render pass that render() records into (no pass may be open on `cmd`);
// call it once per render().
void prepare(SDL_GPUCommandBuffer* cmd) noexcept;
// Records the UI into `pass`, a render pass on `cmd` targeting the swapchain format.
void render(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) noexcept;
} // namespace aurora::imgui
