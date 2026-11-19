#pragma once

#include <cstdint>

struct CpuContext;

// Guest code without a translation (mods' code, untranslated addresses) runs in src/ppc_interpreter.cpp.
namespace PpcInterpreter {

bool Enabled();
void SetEnabled(bool enabled);
// True when address holds guest memory an interpreter run can start at.
bool CanRun(uint32_t address);
// Logs the last addresses interpreted code jumped to on this thread: the trail behind a fault.
void DumpRecentBranches();
// Runs the guest function at entry until it returns to ctx->lr as it was on entry.
void Run(CpuContext* ctx, uint32_t entry);
uint64_t InstructionCount();
// MKW_INTERP_PROFILE=1 builds with BRAWL_INTERP_PROFILE=<prefix> (a diagnostic; empty otherwise): write the per-entry
// and per-PC interpreter profile of the segment since the last dump to <prefix>.<n>.<label>.txt, and count a frame.
void ProfileDump(const char* label);
void ProfileFrame();

}  // namespace PpcInterpreter
