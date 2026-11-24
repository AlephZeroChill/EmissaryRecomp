#pragma once

#include "common.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::gfx {

using NewPipelineCallback = std::function<gpu::GraphicsPipeline()>;

void initialize_pipeline_cache();
// Queues the cached pipelines again at `samples` (the EFB's new MSAA count), in the background.
void prewarm_for_sample_count(uint32_t samples);
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();
void set_skip_unready_pipelines(bool enabled) noexcept;
bool skip_unready_pipelines() noexcept;
uint32_t queued_pipeline_count() noexcept;

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

bool wait_pipeline(PipelineRef ref, gpu::GraphicsPipeline& pipeline);
bool try_pipeline(PipelineRef ref, gpu::GraphicsPipeline& pipeline);
// Waits indefinitely for a draw about to be committed to a persistent texture with no re-issue
// path. Separate from wait_pipeline so skip-unready mode never becomes blocking for other passes.
bool wait_pipeline_for_persistent_pass(PipelineRef ref, gpu::GraphicsPipeline& pipeline);

} // namespace aurora::gfx
