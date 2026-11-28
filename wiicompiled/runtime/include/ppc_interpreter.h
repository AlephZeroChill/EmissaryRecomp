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

// What runs guest code. Classic decodes every instruction as it runs; Cached runs decoded blocks; Verify checks the
// cached handlers against Classic; Jit compiles the cached blocks to host code (docs/jit-design.md) and is Cached
// wherever the JIT is not built. The environment picks one (BRAWL_INTERP, BRAWL_INTERP_VERIFY, BRAWL_JIT).
enum class Engine : uint8_t { Classic, Cached, Verify, Jit };
// True when this build has a JIT backend and the host gave it executable memory.
bool JitAvailable();
// Tests: every Run entered from now on, on any thread, uses `engine` instead of the environment's choice
// (ClearEngineOverride: the environment's again). Not for use while a Run is active.
void SetEngineOverride(Engine engine);
void ClearEngineOverride();
// Tests: a block is compiled once it has been entered this many times (0: before its first run).
void SetJitThreshold(uint32_t entries);
// MKW_INTERP_PROFILE=1 builds with BRAWL_INTERP_PROFILE=<prefix> (a diagnostic; empty otherwise): write the per-entry
// and per-PC interpreter profile of the segment since the last dump to <prefix>.<n>.<label>.txt, and count a frame.
void ProfileDump(const char* label);
void ProfileFrame();

}  // namespace PpcInterpreter
