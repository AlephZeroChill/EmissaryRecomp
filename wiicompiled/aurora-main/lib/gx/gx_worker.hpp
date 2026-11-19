#pragma once

#include <cstdint>
#include <functional>
#include <utility>

// The GX thread: decodes the game's GX command stream on its own core while the game thread runs ahead, the way the
// Wii's GPU command processor consumed the FIFO alongside the CPU (Dolphin's dual-core mode). The game observes GPU
// results only at sync points - draw done, EFB/display readbacks, copies, frame boundaries - where sync() waits for
// everything submitted so far. Everything else the producer submits keeps its order: FIFO bytes from drain() and
// display lists (copied) are decoded by fifo::process on the GX thread in submission order.
//
// On with AURORA_GX_THREAD=1 (read once, at start()). Off: every entry point here is a no-op and decoding stays inline.
namespace aurora::gx::worker {

// Async decoding is on. Constant after start().
bool enabled() noexcept;
// The calling thread is the GX thread.
bool on_gx_thread() noexcept;
// True when the caller must hand work to the GX thread instead of decoding inline: async and not on the GX thread.
inline bool producer() noexcept { return enabled() && !on_gx_thread(); }
void flush() noexcept;

void start();
void stop() noexcept;

// Producer side. Bytes are copied into the open batch.
void submit_fifo(const uint8_t* data, uint32_t size);
void submit_display_list(const uint8_t* data, uint32_t size, bool bigEndian);
// GXCallDisplayListCompiled's list (big-endian; digest = the caller's content digest): copied, and on the GX thread
// submitted from its compiled form, keyed by `data` (the list's own address), as inline decoding does
// (fifo::submit_compiled_draw_list), or else decoded as submit_display_list's.
void submit_compiled_display_list(const uint8_t* data, uint32_t size, uint64_t digest);
// GXCallStateDisplayList's list (big-endian BP/CP writes and NOPs only): copied, and applied on the GX thread as inline
// decoding applies it (fifo::apply_state_display_list).
void submit_state_display_list(const uint8_t* data, uint32_t size);
// A raw direct-FIFO draw (fifo::submit_raw_draw): its vertices copied, decoded by submit_raw_draw_decoded.
void submit_raw_draw(uint8_t prim, uint8_t fmt, const uint8_t* vertices, uint16_t vtxCount, uint32_t vertexBytes);
// Drains the producer's FIFO bytes, then queues fn to run on the GX thread after them.
void submit_call(std::function<void()> fn);
// GXApplyBPReg from the producer: one BP register word, decoded after everything submitted so far and ahead of the
// producer's FIFO bytes not yet drained (where the inline path decodes it).
void submit_bp(uint32_t word);
// GXSetSourceVtxDesc from the producer: g_gxState.sourceVtxDesc[attr] = type, ordered as submit_bp.
void submit_source_vtx_desc(uint8_t attr, uint8_t type);

// Work that reads or writes decoded state (g_gxState) or the renderer and returns nothing the caller needs: queued in
// order when the GX thread is on (captures must be values: it runs later), inline otherwise. fn must not write FIFO
// bytes (GX_WRITE_*) or touch the __gx shadow: both belong to the producer; do that outside fn.
template <class F>
void run_ordered(F&& fn) {
  if (producer()) {
    submit_call(std::function<void()>(std::forward<F>(fn)));
  } else {
    fn();
  }
}
// Hand the open batch to the GX thread.
void flush() noexcept;
// The calling thread holds the renderer lock across the batch it decodes (the GX thread, or a producer decoding a
// batch in place): decoders need not take it again.
bool holds_batch_lock() noexcept;
// For its lifetime, drops the renderer lock the GX thread holds across a batch, for code that waits on the frame
// worker (which needs that lock to finish): gfx::split_staging_batch. A no-op on other threads and outside a batch.
class BatchLockRelease {
public:
  BatchLockRelease() noexcept;
  ~BatchLockRelease();
  BatchLockRelease(const BatchLockRelease&) = delete;
  BatchLockRelease& operator=(const BatchLockRelease&) = delete;

private:
  bool m_released = false;
};
// Flush and wait until the GX thread has run everything submitted so far. Keeps the producer's guest timing alive
// while it waits (the frame-worker wait callback), as drain()'s frame-worker wait did. No-op unless producer().
void sync(const char* why = __builtin_FUNCTION()) noexcept;
// Once per frame (end_frame): every 600 frames logs syncs, hand-offs and bytes per frame.
void note_frame() noexcept;
// Producer: the guest's GXFlush, the SDK's push of the write-gather pipe to the GPU: the work submitted so far goes to
// the GX thread now, without waiting, rather than at the next sync (AURORA_GXFLUSH_HANDOFF=0: off).
void flush_point() noexcept;
// Deferred jobs: a stretch of the producer's command stream filled later by another thread (runtime hle/g3d_native.cpp:
// a native model draw recorded on the game thread and replayed by a draw worker). begin_deferred_job (producer) puts a
// Job item in the stream, after everything submitted so far; the GX thread waits at it for the job's batches. On the
// draw worker, between attach_deferred_job and finish_deferred_job, every producer entry point fills the job instead
// of the producer's stream (its own FIFO buffer and __gx; see fifo.hpp, __gx.h), and its sync() waits only for the
// job's batches handed over so far. Null when the GX thread is off or the caller is not the producer.
struct DeferredJob;
// followsJob (optional): set when nothing entered the stream since the previous begin_deferred_job's item (no item, no
// sync), so the GX thread's decoded state at this job is the one the previous job left.
DeferredJob* begin_deferred_job(bool* followsJob = nullptr);
void attach_deferred_job(DeferredJob* job) noexcept;
void finish_deferred_job() noexcept;
// Producer time spent blocked in sync() since the last call (nanoseconds), then reset. A frame's critical path is its
// game-thread work plus this wait, since waiting for the GX thread delays the frame although it costs no cycles.
uint64_t take_sync_wait_nanos() noexcept;

} // namespace aurora::gx::worker
