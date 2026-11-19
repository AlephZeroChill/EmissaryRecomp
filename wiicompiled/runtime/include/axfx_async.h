// axfx_async.h - the join of the AXFX callbacks' worker (hle/audio/axfx_native.cpp, AXFX_ASYNC).
//
// The reverb and delay callbacks hand their 96 samples to a worker thread. Join() waits until every job handed over is
// done and its results are in guest memory: the bus buffers, the delay lines, the reverb's low-pass words, and the
// stack words the call left for it. Call it before anything outside the callbacks reads or writes guest memory an
// effect may be processing: the DSP's reads and writes of the aux buffers (AxDspHle's JoinMix and
// DispatchCommandList), whole-memory images, hashes and restores (det_hash, save_states, brawl_rollback). The guest
// functions that own the effects join through their native bodies in axfx_native.cpp. Game thread only (the guest's
// fibers); cheap when nothing is pending.
#pragma once

namespace AxfxAsync {

void Join();

}  // namespace AxfxAsync
