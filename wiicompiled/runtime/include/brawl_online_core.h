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

void StageCreate(CpuContext* ctx);
void StageFile(CpuContext* ctx);
void PadRead(CpuContext* ctx);
void PadThreadUpdated(CpuContext* ctx);

// The local controllers as the game is about to read them (FrameInput::SampleLocal): the test
// harness's pads instead, while one is on.
struct HarnessPad;
void HarnessPads(void* gc_pads);

}  // namespace BrawlOnlineCore
