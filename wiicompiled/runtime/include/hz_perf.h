// hz_perf.h - Horizon-only hot-path counters, printed by the main-thread sampler (platform/horizon.c).
#pragma once

// MKW_HZ_PERF: compile the counters in (default: Horizon builds only). MKW_HZ_PERF=1 on another host reads the cycle
// counter instead, to measure the counters' own instruction cost there (g_hzPerf is then defined in abi_bridge.cpp).
// MKW_HZ_PERF_HOT (default: MKW_HZ_PERF): the per-call ones, on paths every frame takes tens of thousands of times:
// each FIFO write (HZ_HOT_PERF_ADD + HZ_HOT_PHASE: two counter reads), each HLE thunk and interpreter entry
// (HZ_EXCLUSIVE: an exclusive-time switch with two), each indirect call (HZ_EXCLUSIVE_PAUSE_IF_ACTIVE: a test that also
// keeps the call from being a tail jump), each display list (gx_dl.cpp spans). A timing build turns only these off
// (CMake MKW_HZ_PERF_HOT_COUNTERS=OFF -> MKW_HZ_PERF_HOT=0); the per-frame phases and counts stay, and the sampler
// prints zeros for the hot slots.
#ifndef MKW_HZ_PERF
#if defined(__SWITCH__)
#define MKW_HZ_PERF 1
#else
#define MKW_HZ_PERF 0
#endif
#endif
#ifndef MKW_HZ_PERF_HOT
#define MKW_HZ_PERF_HOT MKW_HZ_PERF
#endif

#if MKW_HZ_PERF
#include <cstdint>
extern "C" volatile unsigned long long g_hzPerf[48];
enum HzPerfSlot {
    kHzSync, kHzSyncWork, kHzPagesCompared, kHzSyncTicks, kHzPatched, kHzPatchedTicks,
    kHzAudioBlocks, kHzAudioCallbackTicks, kHzAudioTicks, kHzNoteWrite, kHzEdgeDirty, kHzFrames, kHzCondWait, kHzNanosleep, kHzInterpInstructions,
    kHzFifoWrites, kHzFifoTicks, kHzDlCalls, kHzDlTicks, kHzDlBytes,
    kHzDlProbeTicks, kHzDlApplyTicks, kHzDlAuroraTicks, kHzDlSyncTicks,
    // Frame phases, top-level ticks only (a phase entered again inside itself is not counted twice).
    kHzAudioTickTicks, kHzCopyTexTicks, kHzCopyDispTicks, kHzDrawDoneTicks, kHzPatchedTopTicks,
    // Aurora's draw path (aurora-main/lib/gx/command_processor.cpp writes these by index: keep them at 29..35).
    kHzAuDrawTicks, kHzAuUnmergedDraws, kHzAuMergedDraws, kHzAuPipelineTicks, kHzAuBindTicks, kHzAuUniformTicks,
    kHzAuArrayTicks,
    kHzGeckoTicks,  // 36: the Gecko code handler's pass at each VBI (hle/gecko.cpp)
    kHzInterpTicks,  // 37: time the interpreter itself runs, without the translated code it calls (ppc_interpreter.cpp)
    kHzHleTicks,     // 38: time in HLE functions (AbiRawCpuThunk), without the guest code they call or switch to
    kHzAudioJoinTicks,  // 39: Audio_HLE_Tick waiting for the AX mix worker (JoinMixWorker)
    kHzFallbackReads,   // 40: guest reads through MemoryInline::ReadResolvedFallback (out-of-line, memory.cpp)
    kHzFallbackWrites,  // 41: guest writes through MemoryInline::WriteResolvedFallback (out-of-line, memory.cpp)
};
inline uint64_t HzTicks() {
#if defined(__aarch64__)
    // cntpct_el0, not cntvct_el0: Horizon's kernel sets only EL0PCTEN in CNTKCTL_EL1, so an EL0 cntvct_el0 read
    // traps on hardware (2168-0002). Ryujinx doesn't trap either one.
    uint64_t t;
    asm volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t;
#else
    return __builtin_readcyclecounter();
#endif
}
#define HZ_PERF_ADD(slot, n) (g_hzPerf[slot] += (n))
#define HZ_PERF_SET(slot, n) (g_hzPerf[slot] = (n))
#define HZ_PERF_T0() const uint64_t hzT0 = HzTicks()
#define HZ_PERF_SINCE(slot) (g_hzPerf[slot] += HzTicks() - hzT0)
// Ticks of a whole scope into a slot, counted only at the outermost entry of that slot.
inline unsigned char g_hzPhaseDepth[48];
struct HzPhase {
    int slot;
    uint64_t t0;
    explicit HzPhase(int s) : slot(s), t0(g_hzPhaseDepth[s]++ == 0 ? HzTicks() : 0) {}
    ~HzPhase() {
        if (--g_hzPhaseDepth[slot] == 0) g_hzPerf[slot] += HzTicks() - t0;
    }
};
#define HZ_PHASE_CAT2(a, b) a##b
#define HZ_PHASE_CAT(a, b) HZ_PHASE_CAT2(a, b)
#define HZ_PHASE(slot) HzPhase HZ_PHASE_CAT(hzPhase, __LINE__)(slot)
// Exclusive time of a region that calls out: HzExclusive runs the clock while it lives, HzExclusivePause stops it for
// a call that belongs to someone else. They nest: each pause and each new region closes the running segment first.
inline uint64_t g_hzExclusiveStart = 0;
inline int g_hzExclusiveSlot = -1;
inline void HzExclusiveSwitch(int slot) {
    const uint64_t now = HzTicks();
    if (g_hzExclusiveSlot >= 0) g_hzPerf[g_hzExclusiveSlot] += now - g_hzExclusiveStart;
    g_hzExclusiveSlot = slot;
    g_hzExclusiveStart = now;
}
struct HzExclusive {
    int previous;
    explicit HzExclusive(int slot) : previous(g_hzExclusiveSlot) { HzExclusiveSwitch(slot); }
    ~HzExclusive() { HzExclusiveSwitch(previous); }
};
struct HzExclusivePause {
    int previous;
    HzExclusivePause() : previous(g_hzExclusiveSlot) { HzExclusiveSwitch(-1); }
    ~HzExclusivePause() { HzExclusiveSwitch(previous); }
};
// A pause that costs one load and branch when nothing is being timed: for paths every guest call takes (indirect
// dispatch) and fiber switches.
struct HzExclusivePauseIfActive {
    int previous;
    HzExclusivePauseIfActive() : previous(g_hzExclusiveSlot) {
        if (previous >= 0) HzExclusiveSwitch(-1);
    }
    ~HzExclusivePauseIfActive() {
        if (previous >= 0) HzExclusiveSwitch(previous);
    }
};
#if MKW_HZ_PERF_HOT
#define HZ_EXCLUSIVE(slot) HzExclusive HZ_PHASE_CAT(hzExclusive, __LINE__)(slot)
#define HZ_EXCLUSIVE_PAUSE() HzExclusivePause HZ_PHASE_CAT(hzExclusivePause, __LINE__)
#define HZ_EXCLUSIVE_PAUSE_IF_ACTIVE() HzExclusivePauseIfActive HZ_PHASE_CAT(hzExclusivePauseIf, __LINE__)
#define HZ_HOT_PERF_ADD(slot, n) HZ_PERF_ADD(slot, n)
#define HZ_HOT_PHASE(slot) HZ_PHASE(slot)
#endif
#else
#define HZ_PERF_ADD(slot, n) ((void)0)
#define HZ_PERF_SET(slot, n) ((void)0)
#define HZ_PERF_T0() ((void)0)
#define HZ_PERF_SINCE(slot) ((void)0)
#define HZ_PHASE(slot) ((void)0)
#endif
#if !MKW_HZ_PERF || !MKW_HZ_PERF_HOT
#define HZ_EXCLUSIVE(slot) ((void)0)
#define HZ_EXCLUSIVE_PAUSE() ((void)0)
#define HZ_EXCLUSIVE_PAUSE_IF_ACTIVE() ((void)0)
#define HZ_HOT_PERF_ADD(slot, n) ((void)0)
#define HZ_HOT_PHASE(slot) ((void)0)
#endif
