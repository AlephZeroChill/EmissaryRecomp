#pragma once
#include <cstdint>
struct CpuContext;
namespace BrawlRollback {
using GuestFunction = void (*)(CpuContext*);
void Run(CpuContext* ctx, GuestFunction gameProc);
void RunLoop(CpuContext* ctx, GuestFunction gameProc);
void SeedCrossplayStage();
bool DrivesLoop();
void AnchorPads();
void UpdateSystem(CpuContext* ctx, GuestFunction update);
// A call made by the main loop body while RunLoop drives it (interpreted, 0x800171B4..0x80017508), or by a function
// it runs inline. Invoke: call it as usual; Inline: interpret the callee in the caller's frame, so its own calls come
// here too; Skipped: the call is not made (a resimulated frame's render-only work).
enum class LoopCallAction { Invoke, Inline, Skipped };
LoopCallAction LoopCall(CpuContext* ctx, uint32_t target);
void LoopCallDone(uint32_t target);
// GameplayHash (the netplay checksum: fighters, RNG, frame counters) outside a session; det_hash.cpp logs it.
uint32_t CurrentGameplayHash();
}
