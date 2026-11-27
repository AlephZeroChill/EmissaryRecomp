#pragma once

#include "abi_bridge.h"

// hle/gx/gx_fatal_stubs.cpp includes nothing but this header and reaches
// std::fprintf / std::snprintf / std::abort through it.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// VI Utils
void VI_HLE_ForceRetrace(CpuContext* ctx);
void VI_HLE_PollRetrace(CpuContext* ctx);
void VI_HLE_ProcessRetracesDeferred(int maxToProcess);
void VI_HLE_WaitForNextRetracePoll();
// Deterministic mode (det_clock.h): the virtual tick of the next VI retrace, DetClock::kNever
// before VIInit.
uint64_t VI_HLE_DetNextRetraceTicks();
// Single owner of the Aurora frame presentation sequence (seal, optional pace
// to the VI retrace boundary, pre-warm the next frame). paceToRetrace is true
// for the GXCopyDisp producer path and false for retrace-context presents.
void VI_HLE_PresentFrame(bool presentedXfb, bool paceToRetrace);
bool VI_HLE_IsAdvancingRetrace();
void VI_HLE_SetXfbReady(uint32_t xfbAddr); // Called by GXCopyDisp to signal EFB→XFB copy
// Rollback resimulation (brawl_rollback.cpp): while set, VI_HLE_PresentFrame ends Aurora frames without showing them
// (aurora_end_frame_unpresented): the guest and Aurora's frame boundaries are unchanged, nothing is presented.
void VI_HLE_SetPresentSuppressed(bool suppressed);
// Brawl Online's core finished replaying frames after a rollback: the next presented frame is on another timeline
// than the frame shown before it (aurora_mark_frame_discontinuous).
void VI_HLE_NoteRollback();
bool VI_HLE_PresentSuppressed();
// The display cadence since the last reset, one line (the presents, the game frames' spacing on screen, the present
// hold, the modelled screen; vi.cpp TickPacingWindow), for a match's log.
void VI_HLE_DisplayReport(char* out, size_t size, bool reset);
// Rollback (BRAWL_RESIM_LIGHT_LAST): while set, GXCopyDisp clears the EFB but leaves the display copy holding the image
// already in it, which the next present shows.
void VI_HLE_SetKeepDisplayCopy(bool keep);
bool VI_HLE_KeepDisplayCopy();void Audio_HLE_Tick(CpuContext* ctx, uint32_t deltaMicros);
void Audio_HLE_Poll(CpuContext* ctx);
// Deferred twin of Audio_HLE_Poll for the long host waits that already service
// retraces and alarms (the VI retrace pacing loop, the Aurora frame-worker wait
// callback). Runs the AI DMA tick on an isolated register file the way
// OS_HLE_ProcessAlarmsDeferred does, so it is safe to call from the middle of an
// arbitrary translated function.
void Audio_HLE_PollDeferred();
// Deterministic mode: the virtual tick at which the AI DMA block in flight completes, or
// DetClock::kNever while audio DMA is stopped.
uint64_t Audio_HLE_DetNextEventTicks();
// Brawl Online's rollback (its Engine's SetRollbackAudioWaits / AddRollbackAudioPause / SetRollbackLoadWait, as
// Dolphin's CoreTiming and SystemTimers' AudioDMACallback do them; docs/design/brawl-online-core.md §10.4). The
// gameplay-only snapshots leave the sound system out, so a sound plays on through a rollback; with the waits on (a
// session), the audio clock waits for the re-runs: the session adds each re-run pass's field to a pause, and the AI
// DMA holds that many ticks of blocks (no output, no DMA callback, so no AX frame, no voice moves, no wake-up of the
// game's sound thread), so the music and sounds advance only with the frames that are shown, and a sound only the
// corrected input starts is heard from its start. Without them every re-run's span was cut out of what plays: its
// blocks went to the output at once, unpaced, and the output queue, full, dropped the frames after. The blocks hold
// too while the session waits at the loop top for the game's loads. Turning the waits off drops a pause not yet
// taken. Guest thread.
void Audio_HLE_SetRollbackWaits(bool wait);
void Audio_HLE_AddRollbackPause(int64_t ticks);
void Audio_HLE_SetLoadWait(bool waiting);
// The output silenced while a playback runs fast (Brawl Online's Engine::SetPlaybackMuted: a spectator's or room
// watcher's catch-up to live, a seek's fast-forward): the AI blocks still run the DMA callback, their samples are
// dropped instead of queued (a muted queue would still fill at the playback's speed and play late after).
void Audio_HLE_SetOutputMuted(bool muted);
struct AudioHleStats {
    uint64_t played = 0;          // AI DMA blocks delivered (output + callback)
    uint64_t playedInReruns = 0;  // ... of them while a rollback's re-run ran
    uint64_t heldForReruns = 0;   // blocks the re-run pause held
    uint64_t heldInLoadWaits = 0; // blocks held while the session waited for loads
    uint64_t muted = 0;           // delivered blocks whose samples were dropped (muted)
    uint64_t blockTicks = 0;      // one block's ticks (the last block's)
};
AudioHleStats Audio_HLE_Stats();
bool OS_HLE_InterruptsEnabled() noexcept;
extern "C" void OS_HLE_ProcessAlarmsDeferred(int maxToProcess);
extern "C" void OS_HLE_BeginDeferredGuestCallbacks();
extern "C" void OS_HLE_EndDeferredGuestCallbacks();


// Defines and registers a faithful native reimplementation that REPLACES the translated function
// at a PPC address (not a stub; genuine not-yet-implemented entries live in hle/gx/gx_fatal_stubs.cpp
// and abort). The translator regex-parses these macro names to skip that address at build time, so
// renaming requires updating Translator.Cli/Program.cs, RuntimeNativeGuestEffectAnalyzer.cs,
// TranslatedBuildShardEmitter.cs and RuntimeNativeFunctionAbiProvider.cs together.

#define PPC_NATIVE_OVERRIDE(addr_hex, name, ret_type, arg_list, call_list) \
    extern "C" ret_type func_##addr_hex arg_list { return name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)

#define PPC_NATIVE_OVERRIDE_VOID(addr_hex, name, arg_list, call_list) \
    extern "C" void func_##addr_hex arg_list { name call_list; } \
    REGISTER_NATIVE_FUNCTION(0x##addr_hex, name)
