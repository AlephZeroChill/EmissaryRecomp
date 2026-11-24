#pragma once
// Brawl Online's core in-process (brawlonline.net core/, docs/design/brawl-online-core.md §10).
//
// Two halves meet here, in plain types only: the core side (brawl_online_core_side.cpp, built with
// the core's headers and Dolphin's Common) and the runtime side (brawl_online_core.cpp, built with
// the runtime's). The core's headers and the runtime's cannot share a file (both have a `Memory`).

#include <cstddef>
#include <cstdint>

namespace BrawlOnlineBridge {

// ---- The runtime, for the core (runtime side) --------------------------------------------------
// Guest RAM: a host pointer for [addr, addr + size), or nullptr unless it is all RAM.
uint8_t* GuestPointer(uint32_t addr, uint32_t size);
uint8_t* Mem1();
uint32_t Mem1Size();
uint8_t* Mem2();
uint32_t Mem2Size();
// A host write into guest RAM (the snapshots' write tracking must see it).
void MarkDirty(uint32_t addr, uint32_t size);

// The CPU at the hook being run (the main loop's).
uint32_t Gpr(int index);
void SetGpr(int index, uint32_t value);
void SetLr(uint32_t value);
// Keeps or puts back every register under `key` (the session's saved registers around its calls).
void SaveRegs(const void* key);
void RestoreRegs(const void* key);

// The deterministic clock (the guest's time base, 60.75 MHz) and its pacing.
uint64_t TimeBase();
void SetResimulating(bool active);
void ResetPacing();
// Time sync and frame pacing (det_clock.h; the core's Engine time calls, as Dolphin's CoreTiming throttle does them).
// Times are std::chrono::steady_clock nanoseconds since its epoch (one clock in the process).
void SetSpeed(double factor);
void AdvancePacing(int64_t ns);
void BeginBurst();
void SetLoadWait(bool waiting);
bool ScheduledHostTime(int64_t* steady_ns);
int64_t TakeMaxBehindNs();
// The match's frame pacing on or off; `lead`: a pacing wait may end early by CorePacedSleepLeadNs().
void SetPresentPacing(bool on, bool lead);
// The pacing's counters since SetPresentPacing(true), one line (logged at the match's end).
void PacingReport(char* out, size_t size);

// Snapshots of guest RAM in the runtime's delta store (SnapshotStore + WriteWatch), driven by the
// core's ring: the ranges (sorted by physical address, not overlapping), then saves and restores
// by key (each save's key above the newest kept; a restore drops the newer ones).
void SnapshotRegions(const uint32_t* addrs, const uint32_t* sizes, size_t count);
void SnapshotSave(int key);
void SnapshotRestore(int key);

struct Pad {
    bool connected;
    uint16_t button;
    int8_t stickX, stickY, substickX, substickY;
    uint8_t triggerL, triggerR;
};
Pad LocalPad(int chan);

void ScreenMessage(const char* text, uint32_t ms, uint32_t argb);
void StopGame();
// The width over height the game's picture is shown at (DisplaySettings::PictureAspect; 0 = not known yet).
float DrawAspect();
// The game's thread stands still at the loop top (a replay paused or buffering: the core's
// Engine::WhileStandingStill, about 30 times a second): the window's events, the controllers read
// for LeaveButtonsHeld, and the last frame presented again with the UI drawn over it.
void StandStill();
// START or B held on a local controller (a Wii Remote's B or +) as last read (FrameInput::SampleLocal: every
// retrace, and while the game stands still; within the last 250 ms, else false): any thread.
bool LeaveButtonsHeld();
// For the core's loop watch (Engine::StallReport), from its network thread while the game's thread
// has not run a session frame for a second or more: the guest clock and which guest context runs.
void StallReport(char* out, size_t size);

// ---- The core, for the runtime (core side) -----------------------------------------------------
// What the loop does after the core's loop-top hook.
enum class Action : int { Normal, Continue, Skip, Call };

// Start-up and shutdown: the user folder (Dolphin's layout: Config/Dolphin.ini, Online/...) and a
// replay comm file (Dolphin's --replay-comm, empty for none).
void CoreStart(const char* user_dir, const char* replay_comm);
void CoreShutdown();

// The main loop's hooks (Dolphin's HLE_Misc Brawlback hooks, same addresses).
Action CoreLoopTop(uint32_t* call_target);   // 0x800171B4; Call: run call_target, then again
bool CoreFrameEnd();                         // 0x80017504; true: done, on at 0x80017508
void CoreLoopEnd();                          // 0x80017508
bool CoreDrivesLoop();                       // the pacing pcs 0x800172D0 and 0x8001730C
void CoreStageCreate();                      // __ct__7stMelee 0x809435F0
void CoreStageFile();                        // the stage file's load (0x80949BC0 / 0x80949C20)
void CorePadRead();                          // after the pad thread's read of the controllers
void CorePadThreadUpdated();                 // the pad thread's loop, after P+'s code (0x8002BA84)

// Brawl Online's test harness (BRAWL_ONLINE_HARNESS_PORT, brawl_online_harness.cpp): every poll of the
// local controllers, its pads (pad_set, pad_script) replace them.
void CoreHarnessStart();
// Winsock references for the whole run (a WSACleanup too many elsewhere closes every socket).
void CoreHoldWinsock();
void CoreWatchExit();
void CoreWatchCrashes();
// BRAWL_ONLINE_PROFILE: sample the calling thread (the game loop's).
void CoreProfileStart();
// The calling thread is the game loop's; its native stack from any thread (for the loop watch).
void CoreNoteGameThread();
void CoreGameThreadStack(char* out, size_t size);
// The guest CPU of this thread (pc, lr, ctr, GPRs) to stderr: for crash reports.
void DumpGuestCpu();
bool CoreHarnessActive();
void CoreHarnessPads(Pad* pads);

// The paced wait's lead (the core's PacedSleepLead): how long the coming rollback's re-run will take, 0 for none.
int64_t CorePacedSleepLeadNs();

// The sound hooks (the core's Gprb::Session::OnSoundAlloc / OnSoundAttached / OnSoundStop, Dolphin's HLE_Misc).
enum class SoundAlloc : int { Proceed, NoChannel, Reattach };
SoundAlloc CoreSoundAlloc(uint32_t sound_id, uint32_t handle, uint32_t* sound);
void CoreSoundAttached(uint32_t handle, uint32_t sound, uint32_t sound_id);
bool CoreSoundStop(uint32_t sound);

// Brawl Online's in-game UI (brawl_online_ui.cpp, core/ui), in the runtime's ImGui frame: each
// presented frame, `top` the height the runtime's bar covers; whether the UI has the keyboard.
void CoreUiDraw(float top);
bool CoreUiBlocksInput();
// At the core's start (CoreStart, core side): the UI's hooks, before the first message.
void CoreUiStart();

}  // namespace BrawlOnlineBridge
