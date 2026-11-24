#pragma once
// Brawl Online's core in-process (brawl_online_bridge.h): the runtime's entry points into it. On when
// BRAWL_ONLINE_CORE names the user folder (Dolphin's layout: Config/Dolphin.ini, Online/...); it
// replaces BrawlRollback's prototype loop and BrawlOnline's IPC.

#include <cstdint>

struct CpuContext;

namespace BrawlOnlineCore {

using Pass = void (*)(CpuContext*);

bool Enabled();
// Before the game's entry point runs.
void Start();
void Shutdown();

// The main loop (interpreted while the core is on): one pass from the loop top (0x800171B4) to its
// end (0x80017508), the core deciding at the loop top whether it runs, is skipped, or a guest call
// runs first.
void RunLoop(CpuContext* ctx, Pass pass);
// The interpreter, inside a pass: true at 0x80017504 when the frame end is done (go on at
// 0x80017508); the loop end; whether the core paces the loop (0x800172D0, 0x8001730C).
bool FrameEnd(CpuContext* ctx);
void LoopEnd(CpuContext* ctx);
bool DrivesLoop();

// Brawl Online's sound hooks (Dolphin's HLE_Misc BrawlbackSkipResim*SoundAllocHook, GprbSoundAttachHook and
// GprbSoundStopHook): the interpreter at one of their pcs in detail_SetupSound (the three `bl detail_Alloc*Sound`,
// 0x801C9AAC/0x801C9B54/0x801C9BFC, and its success path 0x801C9C70) or at BasicSound::Stop (0x801BC684). Returns
// the pc to go on at: `pc` itself to run the instruction there as usual.
uint32_t SoundHook(CpuContext* ctx, uint32_t pc);

void StageCreate(CpuContext* ctx);
void StageFile(CpuContext* ctx);
void PadRead(CpuContext* ctx);
void PadThreadUpdated(CpuContext* ctx);

// The local controllers as the game is about to read them (FrameInput::SampleLocal): the test
// harness's pads instead, while one is on.
struct HarnessPad;
void HarnessPads(void* gc_pads);
// The local controllers as FrameInput::SampleLocal just read them (`frame`: a FrameInput::Frame), every
// retrace and while the core holds the game still: START or B (a Wii Remote's B or +) held, for the core's
// Host::LeaveButtonsHeld (a room's game watched) from any thread.
void NoteLocalPads(const void* frame);

// Brawl Online's in-game UI (brawlonline.net core/ui: on-screen messages, chat, the replay overlay),
// drawn in the runtime's ImGui frame (settings_overlay::Draw, once per presented frame, the frame
// open). `top`: the height the runtime's own bar covers at the top of the window (0: none).
void DrawUi(float top);
// The UI has the keyboard (the chat box): the game gets no keyboard or controller input meanwhile.
bool UiBlocksInput();

}  // namespace BrawlOnlineCore
