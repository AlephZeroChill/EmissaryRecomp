// insn_count.h - per-call user-instruction counts (NAT_INSN=1).
//
// Why: timing-lane instruction totals differ by up to 0.09M a frame between two runs of the same binary (NOTE2), so
// a change worth less than ~0.1M a frame cannot be measured from them. This counts individual calls instead, on the
// calling thread's own retired-user-instruction counter (perf_event_open, read with rdpmc): exact for a given path and
// independent of timing.
//
// Use: one site per measured thing, then the instructions of each measured call into one of its two columns.
//   static const int kSite = InsnCount::Site("calcview");
//   const uint64_t t = InsnCount::Read();
//   body(ctx);
//   InsnCount::Add(kSite, InsnCount::kOriginal, InsnCount::Since(t));
// A verify mode runs a native and its original from the same state, so both columns come from the same calls and
// their difference is the native's exact saving on those calls (g3d_calc.cpp Verify, mtx.cpp VerifyOne and
// axfx_native.cpp Verify count every call they compare). InsnCount::Scope counts a region into one column. Report()
// runs with each fps-log window (g3d_calc.cpp G3dCalcLogStats) and logs one "[insn]" line per site; scripts/insn-sum.py
// totals the lines of a run.
//
// The counter needs CAP_PERFMON under perf_event_paranoid = 4: scripts/WiiCompiled-insn starts the game with
// it (nat-run.sh's insn mode). Without it, or off Linux x86-64, Read() returns 0 and Report() says why, once.
#pragma once

#include <cstdint>

namespace InsnCount {

enum Column : int { kOriginal = 0, kNative = 1 };

bool EnvOn();  // NAT_INSN=1
// A function-local static, so a static initializer in any file can ask (no cross-file initialization order).
inline bool On() {
    static const bool on = EnvOn();
    return on;
}

// This thread's retired user instructions so far (0 when the counter is unavailable).
uint64_t Read();
// The instructions since `start` (a Read()), less the counter read's own cost.
uint64_t Since(uint64_t start);
// A site's id by name (the same name gives the same id); -1 once the table is full (Add ignores it).
int Site(const char* name);
// One call's instructions (a Since()) into a site's column.
void Add(int site, Column column, uint64_t instructions);
// Logs every site with calls since the last report ("[insn] ..."), then starts a new window.
void Report(double frames);

// A region's instructions into one column. A site of -1 (unregistered) costs nothing: no counter reads, so an
// optional site inside a counted call does not inflate that call's count (a pair of reads and the add are ~150
// instructions).
struct Scope {
    int site;
    Column column;
    uint64_t start;
    Scope(int s, Column c) : site(s), column(c), start(s >= 0 && On() ? Read() : 0) {}
    ~Scope() {
        if (site >= 0 && On()) Add(site, column, Since(start));
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace InsnCount
