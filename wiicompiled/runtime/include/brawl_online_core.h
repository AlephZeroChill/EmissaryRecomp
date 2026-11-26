#pragma once
// Brawl Online's core in-process (brawl_online_bridge.h): the runtime's entry points into it. On when
// BRAWL_ONLINE_CORE names the user folder (Dolphin's layout: Config/Dolphin.ini, Online/...); it
// replaces BrawlRollback's prototype loop and BrawlOnline's IPC.

#include <cstdint>
#include <string>

struct CpuContext;
struct PADStatus;

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
// The same as a look between the game's reads (no harness poll counted).
void HarnessPeekPads(void* gc_pads);
// The local ports FrameInput::Latch just read for the frame a retrace starts (`frame`: a FrameInput::Frame): the core
// takes each GameCube port's read as Dolphin's SI reads (the instant it stands for, the host decode), and in a
// rollback's re-run hands back the reads latched before it.
void HostPadsRead(void* frame);
// Input suggestions (the core's Gprb::Suggest): while the game's thread waits for the next field (det_clock.h's wait
// hook), a look at the local player's controller about once a millisecond, which the core sends the other players
// when it changes what the next frame's input would be. False when none is wanted (then the wait is not sliced).
bool SampleForSuggestions();
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

// Dolphin's controls (brawl_online_bridge.h DolphinControls*): with the core on and the user folder's
// Config/GCPadNew.ini there, each GameCube port is what Dolphin plugs into it (Dolphin.ini SIDevice0-3):
// a standard controller reads as Dolphin's Controllers settings have it (any device, expressions,
// sticks, modifiers), the GameCube adapter as the runtime reads its pads, anything else as no controller.
// `statuses`: the four ports as aurora's PADRead and the runtime's bindings left them. The game's thread. `peek`: a
// look between the game's reads (input suggestions: Dolphin's input channel for them, nothing loaded again).
void ApplyDolphinControls(PADStatus* statuses, bool peek = false);
// A port Dolphin's controls drive: its Rumble/Motor takes the game's motor command (true: done).
bool DolphinControlsRumble(int32_t chan, bool on);
bool DolphinControlsActive();
int DolphinControlsSiDevice(int port);
std::string DolphinControlsDevice(int port);
// Dolphin's controls bind this keyboard key (Dolphin's name for it, "ESCAPE").
bool DolphinControlsUseKey(const char* key);
// Dolphin's Controllers window over the game: known (the launcher passed BRAWL_ONLINE_DOLPHIN), open now, open it.
bool DolphinControllersAvailable();
bool DolphinControllersShowing();
bool OpenDolphinControllers();

}  // namespace BrawlOnlineCore
