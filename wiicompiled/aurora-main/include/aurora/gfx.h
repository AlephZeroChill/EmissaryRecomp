#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stddef.h"
#include "stdint.h"
#endif

#ifndef NDEBUG
#define AURORA_GFX_DEBUG_GROUPS
#endif

void aurora_push_debug_group(const char* label);
void aurora_pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
  uint32_t presentedFrameCount;
  uint32_t interpolatedFrameCount;
} AuroraStats;

typedef struct {
  uint64_t totalPresentCount;
  uint32_t sampleCount;
  double framesPerSecond;
  double averageFrameTimeMs;
  double p95FrameTimeMs;
  double jitterMs;
  // framesPerSecond with duplicated presentation slots scaled out, so this is the rate of frames
  // that carried new motion. Equal to framesPerSecond when every slot replayed real interpolation.
  double effectiveFramesPerSecond;
} AuroraPresentTiming;

const AuroraStats* aurora_get_stats();
void aurora_get_present_timing(AuroraPresentTiming* timing);

// Interpolation health: the per-frame fields describe the last sealed frame, the counters
// accumulate since it was configured. This answers "output FPS dropped but the game held 60".
typedef struct {
  uint32_t targetFps;          // configured target, 0 when interpolation is off
  uint32_t targetSamples;      // slots the pacing controller currently aims for
  uint32_t activeSamples;      // slots latched for the latest sealed frame
  uint32_t candidates;         // perspective draws in the latest sealed frame
  uint32_t matchable;          // candidates whose identity also existed last frame
  uint32_t matches;            // draws matched to the previous frame
  uint32_t eligible;           // latest frame inserted interpolated slots
  uint32_t replaySafe;         // latest frame could replay its command stream
  uint64_t framesSealed;
  uint64_t framesLowMatch;
  uint64_t framesReplayUnsafe;
  uint64_t slotReductions;
  uint64_t lateSealDrops;
  // Mode and Predict counters (AURORA_FRAME_INTERPOLATION_PREDICT), since start: draws carried forward and held,
  // draws and frames that glided back from an overshoot, the largest glide (view-space units), frames shown
  // without inserted frames because a rollback had just rewritten them.
  uint32_t mode;
  uint64_t predictDraws;
  uint64_t predictHeld;
  uint64_t glideDraws;
  uint64_t glideFrames;
  float glideLargest;
  uint64_t discontinuousFrames;
  uint64_t joltFrames;  // frames shown as drawn because the camera jolted (screen shake)
  // Cost and latency, since start: CPU time of the seal's interpolation work and of encoding every slot, the
  // frames encoded, presentation jobs dropped for a newer frame, and from a frame being ready to its own image's
  // present call (sum, count, largest).
  uint64_t sealNanos;
  uint64_t encodeNanos;
  uint64_t encodedFrames;
  uint64_t supersededJobs;
  uint64_t frameLatencyNanos;
  uint64_t frameLatencyCount;
  uint64_t frameLatencyMaxNanos;
} AuroraFrameInterpolationDiagnostics;

void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics);

// Generates transform-interpolated perspective frames between consecutive 60 Hz logical frames.
// Supported targets are 0 (off), 120, 180 and 240. Guest simulation and VI timing are unchanged.
void aurora_set_frame_interpolation_fps(uint32_t targetFps);
// How the inserted frames are made. INTERPOLATE: between the previous and the newest game frame, which is shown
// after them (most of a frame later). PREDICT: the newest game frame is shown as soon as it is ready and the
// inserted frames carry each draw forward by its last frame's motion.
enum { AURORA_FRAME_INTERPOLATION_INTERPOLATE = 0, AURORA_FRAME_INTERPOLATION_PREDICT = 1 };
void aurora_set_frame_interpolation_mode(uint32_t mode);
uint32_t aurora_get_frame_interpolation_mode();
// The frame being recorded follows a rollback (its previous frame was on another timeline).
void aurora_mark_frame_discontinuous();
// Like aurora_set_present_schedule, for a producer that presents each frame as soon as it is ready (no retrace to
// pace to): `readyNanos` is when the frame being sealed was ready, `intervalNanos` the game frame. Interpolate's
// slots start a short lead after it, Predict shows the frame at once and its inserted frames over the interval.
void aurora_set_seal_present_schedule(uint64_t readyNanos, uint64_t intervalNanos);
// While true (a game's loading screens), late frames don't lower the interpolated frame rate.
void aurora_set_frame_pacing_exempt(bool exempt);
// NSMBW: V-Sync on (Fifo) or off (Immediate, else Mailbox); off by default.
void aurora_set_vsync(bool enabled);
bool aurora_get_vsync();
// Multisample anti-aliasing of the game's picture: 1 (off), 2, 4 or 8 samples per pixel, applied at the next frame
// boundary and lowered to what the GPU takes. The EFB is resolved before every copy and display.
void aurora_set_msaa(uint32_t samples);
uint32_t aurora_get_msaa();  // the count in use
// Anisotropic filtering of the game's mipmapped, linearly filtered textures: 1 (off) to 16. Takes effect at once.
void aurora_set_texture_anisotropy(uint32_t maxAnisotropy);
uint32_t aurora_get_texture_anisotropy();
// Display only: the game picture as it is presented (not the menus over it). Sharpening 0..1 (contrast-adaptive);
// brightness, contrast and vibrance 1 = as the game draws it.
void aurora_set_picture_adjustments(float sharpening, float brightness, float contrast, float vibrance);
uint32_t aurora_get_frame_interpolation_fps();

// Newly encountered GX pipelines compile on the bounded worker queue. Draws whose pipeline is not
// ready are skipped rather than stalling submission, and pick it up once compilation finishes.
void aurora_set_skip_unready_pipelines(bool enabled);
bool aurora_get_skip_unready_pipelines();
uint32_t aurora_get_queued_pipeline_count();

// Controls whether display copies bypass the Wii's vertical copy filter.
void aurora_set_disable_copy_filter(bool disabled);
bool aurora_get_disable_copy_filter();

// Guest-RAM write tracking. `generation` changes whenever guest RAM covering a host range was
// written (or returns AURORA_GUEST_WRITE_UNTRACKED); `notify` reports writes aurora made itself.
#define AURORA_GUEST_WRITE_UNTRACKED UINT64_MAX
typedef uint64_t (*AuroraGuestWriteGenerationCallback)(const void* hostPtr, size_t size);
typedef void (*AuroraGuestWriteNotifyCallback)(const void* hostPtr, size_t size);
void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation,
                                  AuroraGuestWriteNotifyCallback notify);

#ifdef __cplusplus
}
#endif

#endif
