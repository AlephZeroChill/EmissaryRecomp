// PowerPC (Gekko/Broadway) interpreter for guest code the static translation does not have: code a mod puts in
// memory (Gecko C0/C2 inserts, Syriinge plugins, replaced .rel modules) and any address the translation missed.
// A call to such an address (InvokeIndirectCpu, InvokeDirectCpu, InvokeIndirectJump) runs here instead of failing:
// the function is interpreted until it returns to its caller (blr to the link address it was entered with). Its calls
// into translated, native or patched code go through InvokeIndirectCpu; calls into other untranslated code continue
// in the same loop, as on the processor.
//
// Floating point uses the runtime's own helpers (ppc_isa_float.h, ppc_isa_quantized.h), so results match the
// translated code bit for bit. An FPR holds either a scalar double or a packed pair (PPC_FPR); the translator picks
// one per use, and here each register remembers which form its last write left and is converted at a use of the
// other form, the same PPC_PsFromScalarInline / PPC_PsToScalarInline conversions translated code makes.
#include "ppc_interpreter.h"
#include "brawl_rollback.h"
#include "brawl_online_ipc.h"

// Guest comparisons must preserve unordered results. P+ disables Dreamland's
// wind with infinite timer bounds: their subtraction produces a NaN, which
// must not compare equal to zero and start the hazard.
#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)
#error "The PowerPC interpreter requires strict floating-point semantics"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "abi_bridge.h"
#include "guest_code_patch.h"
#include "guest_float_abi.h"
#include "hz_perf.h"
#include "memory.h"
#include "memory_access.h"
#include "ppc_runtime.h"
#include "runtime_log.h"
#include "system_bridge.h"
#if MKW_INTERP_PROFILE
#include <optional>

#include <pthread.h>

#include "save_states.h"
#endif

namespace PpcInterpreter {
namespace {

constexpr uint32_t kXerSo = 0x80000000u;
constexpr uint32_t kXerOv = 0x40000000u;
constexpr uint32_t kXerCa = 0x20000000u;

std::atomic<bool> g_enabled{true};
std::atomic<uint64_t> g_instructions{0};

// The last addresses interpreted code jumped to on this thread (taken branches, calls, returns), oldest first
// once wrapped: the trail behind a fault in a mod's code.
constexpr uint32_t kRecentBranches = 64;
thread_local uint32_t t_recentBranches[kRecentBranches];
thread_local uint32_t t_recentBranchCount = 0;
thread_local uint32_t t_lastPc = 0;
// The interpreter frame running the rollback-driven main loop body (BrawlRollback::RunLoop), whose calls
// BrawlRollback::LoopCall sees (and, during resimulation, may skip or run inline). Only the game thread sets it; other
// threads' frames never compare equal. Not a thread_local: Call() reads it on every interpreted call, and emulated TLS
// (Horizon) makes each thread_local access a runtime call.
std::atomic<const struct Frame*> g_rollbackLoopFrame{nullptr};

#if MKW_INTERP_PROFILE
// MKW_INTERP_PROFILE=1 builds only (a diagnostic; product builds compile none of this). With BRAWL_INTERP_PROFILE=<prefix>
// set, every interpreter entry address gets its calls, guest instructions and exclusive host ticks (rdtsc while the
// interpreter runs it, the translated, native and patched code it calls excluded), and every guest PC its executed
// instructions. ProfileDump writes them to <prefix>.<n>.<label>.txt and starts the next segment (pad scripts dump at
// each scene change and at EXIT; BRAWL_INTERP_PROFILE_FRAMES=N also dumps every N frames).
namespace prof {
struct Entry {
    uint64_t calls = 0, instructions = 0, ticks = 0;
    uint32_t firstCaller = 0;
};
constexpr uint32_t kMem1Words = 0x01800000u / 4, kMem2Words = Memory::kMem2Size / 4;  // 0x80000000.., 0x90000000..
struct Table {
    std::mutex mutex;  // guards `entries`' structure: inserts here, iteration by the dump
    std::unordered_map<uint32_t, Entry> entries;
    std::vector<uint32_t> pcs = std::vector<uint32_t>(kMem1Words + kMem2Words);
    uint64_t otherInstructions = 0;
    std::string thread;
    struct Slot {
        uint32_t entry = 0;
        Entry* stat = nullptr;
    } cache[4096];
};
std::mutex g_tablesMutex;
std::vector<Table*> g_tables;
const char* Prefix() {
    static const char* const prefix = std::getenv("BRAWL_INTERP_PROFILE");
    return prefix;
}
thread_local Table* t_table = nullptr;
thread_local Entry* t_running = nullptr;
thread_local uint64_t t_since = 0;
inline uint64_t Ticks() { return __builtin_ia32_rdtsc(); }
Table* MyTable() {
    if (t_table == nullptr) {
        t_table = new Table;
        char name[32] = "?";
        pthread_getname_np(pthread_self(), name, sizeof name);
        t_table->thread = name;
        std::lock_guard<std::mutex> lock(g_tablesMutex);
        g_tables.push_back(t_table);
    }
    return t_table;
}
Entry* Find(Table* t, uint32_t entry, uint32_t caller) {
    auto& slot = t->cache[(entry >> 2) & 4095];
    if (slot.stat != nullptr && slot.entry == entry) return slot.stat;
    std::lock_guard<std::mutex> lock(t->mutex);
    auto [it, fresh] = t->entries.try_emplace(entry);
    if (fresh) it->second.firstCaller = caller;
    slot = {entry, &it->second};
    return &it->second;
}
inline void Switch(Entry* next) {
    const uint64_t now = Ticks();
    if (t_running != nullptr) t_running->ticks += now - t_since;
    t_running = next;
    t_since = now;
}
inline void CountPc(Table* t, uint32_t pc) {
    const uint32_t mem1 = (pc - 0x80000000u) >> 2, mem2 = (pc - 0x90000000u) >> 2;
    if (mem1 < kMem1Words) ++t->pcs[mem1];
    else if (mem2 < kMem2Words) ++t->pcs[kMem1Words + mem2];
    else ++t->otherInstructions;
}
// One interpreter frame: its entry's clock runs while it lives (paused by Pause for the code it calls out to).
struct Scope {
    Table* table;
    Entry* previous;
    Entry* mine;
    const uint64_t& executed;
    Scope(uint32_t entry, uint32_t caller, const uint64_t& count)
        : table(MyTable()), previous(t_running), mine(Find(table, entry, caller)), executed(count) {
        ++mine->calls;
        Switch(mine);
    }
    ~Scope() {
        mine->instructions += executed;
        Switch(previous);
    }
};
struct Pause {
    Entry* previous;
    Pause() : previous(t_running) { Switch(nullptr); }
    ~Pause() { Switch(previous); }
};
uint64_t g_frame = 0, g_segmentStart = 0;
uint32_t g_dumps = 0;
}  // namespace prof
#define MKW_INTERP_PROFILE_PAUSE() prof::Pause interpProfilePause
#else
#define MKW_INTERP_PROFILE_PAUSE() ((void)0)
#endif

// BRAWL_TRACE_PC=<hex>,<hex>...: a debugging aid for mods; the registers whenever interpreted code reaches one of
// those addresses (the first 20 times each).
const std::vector<uint32_t>& TracePcs() {
    static const std::vector<uint32_t> pcs = [] {
        std::vector<uint32_t> out;
        for (const char* p = std::getenv("BRAWL_TRACE_PC"); p != nullptr && *p;) {
            char* end = nullptr;
            out.push_back(static_cast<uint32_t>(std::strtoul(p, &end, 16)));
            if (end == p) break;
            p = *end ? end + 1 : end;
        }
        return out;
    }();
    return pcs;
}

void TracePc(const CpuContext* c, uint32_t pc) {
    static std::unordered_map<uint64_t, int> hits;
    static std::mutex mutex;
    // BRAWL_TRACE_UNIQUE=1: once per (pc, lr, r4) instead of the first 20 hits (e.g. every character a print
    // function is asked for, whenever it happens).
    static const bool unique = std::getenv("BRAWL_TRACE_UNIQUE") != nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (unique ? hits[(uint64_t{pc} << 32) ^ (uint64_t{c->lr} << 16) ^ c->gpr[4]]++ > 0 : hits[pc]++ >= 20) return;
    std::string regs;
    char item[24];
    for (int r = 0; r < 32; ++r) {
        std::snprintf(item, sizeof(item), " r%d=%08X", r, c->gpr[r]);
        regs += item;
    }
    RT_LOGF(RT_TAG_RUNTIME, "[trace] 0x%08X lr=%08X ctr=%08X%s\n", pc, c->lr, c->ctr, regs.c_str());
}

enum class Form : uint8_t { Unknown, Scalar, Paired };

struct Frame {
    CpuContext* ctx;
    Form form[32];
    uint32_t pc;
};

inline uint32_t Bits(uint32_t w, int from, int count) { return (w >> from) & ((1u << count) - 1u); }
inline uint32_t RD(uint32_t w) { return Bits(w, 21, 5); }
inline uint32_t RA(uint32_t w) { return Bits(w, 16, 5); }
inline uint32_t RB(uint32_t w) { return Bits(w, 11, 5); }
inline uint32_t RC(uint32_t w) { return Bits(w, 6, 5); }
inline int32_t Simm(uint32_t w) { return static_cast<int16_t>(w & 0xFFFFu); }
inline uint32_t Uimm(uint32_t w) { return w & 0xFFFFu; }
inline bool Rc(uint32_t w) { return (w & 1u) != 0; }

inline uint32_t Mask(uint32_t mb, uint32_t me) {
    const uint32_t a = 0xFFFFFFFFu >> mb;
    const uint32_t b = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (a & b) : (a | b);
}

// --- FPR forms ------------------------------------------------------------------------------------------------

double Scalar(Frame& f, uint32_t r) {
    if (f.form[r] == Form::Paired) {
        return PPC_PsToScalarInline(f.ctx->fpr[r].d);
    }
    return f.ctx->fpr[r].d;
}
double Paired(Frame& f, uint32_t r) {
    if (f.form[r] == Form::Scalar) {
        return PPC_PsFromScalarInline(f.ctx->fpr[r].d);
    }
    return f.ctx->fpr[r].d;
}
void SetScalar(Frame& f, uint32_t r, double v) { f.ctx->fpr[r].d = v; f.form[r] = Form::Scalar; }
void SetPaired(Frame& f, uint32_t r, double v) { f.ctx->fpr[r].d = v; f.form[r] = Form::Paired; }

// --- integer helpers -------------------------------------------------------------------------------------------

void SetCr0(CpuContext* c, uint32_t value) {
    SetCRResident(c->cr, c->xer, 0, static_cast<int32_t>(value), 0);
}
void SetCr1FromFpscr(CpuContext* c) {
    c->cr = (c->cr & ~0x0F000000u) | ((c->fpscr >> 4) & 0x0F000000u);
}
void SetCa(CpuContext* c, bool carry) { c->xer = carry ? (c->xer | kXerCa) : (c->xer & ~kXerCa); }
bool Ca(const CpuContext* c) { return (c->xer & kXerCa) != 0; }
void SetOv(CpuContext* c, bool overflow) {
    c->xer = overflow ? (c->xer | kXerOv | kXerSo) : (c->xer & ~kXerOv);
}
uint32_t Ea(CpuContext* c, uint32_t w) { return (RA(w) ? c->gpr[RA(w)] : 0) + static_cast<uint32_t>(Simm(w)); }
uint32_t EaX(CpuContext* c, uint32_t w) { return (RA(w) ? c->gpr[RA(w)] : 0) + c->gpr[RB(w)]; }

// add with carry in, carry out and overflow: the XO-form arithmetic core.
uint32_t AddCarry(uint32_t a, uint32_t b, uint32_t carryIn, bool& carry, bool& overflow) {
    const uint64_t wide = static_cast<uint64_t>(a) + b + carryIn;
    const uint32_t result = static_cast<uint32_t>(wide);
    carry = (wide >> 32) != 0;
    overflow = ((~(a ^ b) & (a ^ result)) & 0x80000000u) != 0;
    return result;
}

[[noreturn]] void Undefined(CpuContext* c, uint32_t pc, uint32_t w, const char* what) {
    DumpRecentBranches();
    SystemBridge::DumpCpuState(c);  // where the interpreted code came from: lr, the stack's return addresses
    PPC_Undefined(pc, w, what);
}

uint32_t ReadSpr(CpuContext* c, uint32_t spr, uint32_t pc, uint32_t w) {
    switch (spr) {
    case 1: return c->xer;
    case 8: return c->lr;
    case 9: return c->ctr;
    case 26: return c->srr0;
    case 27: return c->srr1;
    case 268: return PPC_Mftb();
    case 269: return PPC_Mftbu();
    case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: return c->gqr[spr - 912];
    case 920: return c->hid2;
    case 1008: return c->hid0;
    case 1009: return c->hid1;
    default:
        RT_LOGF(RT_TAG_RUNTIME, "interpreter: mfspr %u at 0x%08X reads 0\n", spr, pc);
        (void)w;
        return 0;
    }
}

void WriteSpr(CpuContext* c, uint32_t spr, uint32_t value) {
    switch (spr) {
    case 1: c->xer = value; break;
    case 8: c->lr = value; break;
    case 9: c->ctr = value; break;
    case 26: c->srr0 = value; break;
    case 27: c->srr1 = value; break;
    case 912: case 913: case 914: case 915: case 916: case 917: case 918: case 919: c->gqr[spr - 912] = value; break;
    case 920: c->hid2 = value; break;
    case 1008: c->hid0 = value; break;
    case 1009: c->hid1 = value; break;
    default: break;  // decrementer, performance monitors, BATs: no effect in the runtime
    }
}

// Branch condition (BO/BI) with the ctr decrement.
bool BranchTaken(CpuContext* c, uint32_t bo, uint32_t bi) {
    if ((bo & 0x04u) == 0) {
        --c->ctr;
    }
    const bool ctrOk = (bo & 0x04u) != 0 || ((c->ctr != 0) != ((bo & 0x02u) != 0));
    const bool condOk = (bo & 0x10u) != 0 || ((((c->cr >> (31u - bi)) & 1u) != 0) == ((bo & 0x08u) != 0));
    return ctrOk && condOk;
}

// Match translated callers: f1 is the scalar floating-point return; retain the
// other registers' representation tags across calls. ABI volatility alone does
// not change their representation. P+ keeps a scalar multiplier in f3 across a
// posture-scale call, then consumes it with ps_muls0. Forgetting its scalar tag
// interprets double 1.0 as packed float 1.875 and changes jump velocity.
void NormalizeCallFloatReturn(Frame& f) {
    f.form[1] = Form::Scalar;
}

// The float ABI translated callers assume of a translated callee (guest_float_abi.h); null for anything without an
// entry (natives, mod overrides, interpreted code), which then gets no conversion, as from a translated caller that
// knows no ABI for it.
const MkwGuestFloatAbi* FloatAbiOf(uint32_t target) {
    const MkwGuestFloatAbi* begin = g_mkwGuestFloatAbi;
    const MkwGuestFloatAbi* end = begin + g_mkwGuestFloatAbiCount;
    const MkwGuestFloatAbi* it = std::lower_bound(begin, end, target,
        [](const MkwGuestFloatAbi& entry, uint32_t address) { return entry.address < address; });
    return it != end && it->address == target ? it : nullptr;
}

// Before a call into translated code, as a translated caller does (EmitNormalizeGuestCallFloatArguments): a register the
// callee reads as a scalar float, held here in paired form, becomes the scalar it stands for. Registers the callee does
// not read that way keep their paired form (P+ code keeps paired values in volatile FPRs across calls).
void NormalizeCallFloatArguments(Frame& f, const MkwGuestFloatAbi* abi) {
    if (abi == nullptr) return;
    for (uint32_t r = 1; r <= 13; ++r) {
        if ((abi->scalarFloatArgMask & (1u << r)) != 0 && f.form[r] == Form::Paired) SetScalar(f, r, Scalar(f, r));
    }
}

// After it (EmitNormalizeGuestCallFloatReturn): a callee that returns a paired scalar leaves f1 in paired form.
void NormalizeCalleeFloatReturn(Frame& f, const MkwGuestFloatAbi* abi) {
    if (abi != nullptr && abi->returnsPairedScalarFloat != 0) f.ctx->fpr[1].d = PPC_PsToScalarInline(f.ctx->fpr[1].d);
}

// A call (bl, bcl, blrl, bctrl). Translated, native and patched code runs through the function registry; other code
// (a mod's) returns false and is interpreted right here, as the processor would run it: code that does not return the
// usual way (bl to read its own address, a hook that branches back) must not leave a host call frame behind.
bool Call(Frame& f, uint32_t target, uint32_t returnTo) {
    f.ctx->lr = returnTo;
    const bool loopCall = &f == g_rollbackLoopFrame.load(std::memory_order_relaxed);
    if (loopCall) {
        switch (BrawlRollback::LoopCall(f.ctx, target)) {
        case BrawlRollback::LoopCallAction::Inline: return false;  // interpreted in this frame: its calls come here too
        case BrawlRollback::LoopCallAction::Skipped: NormalizeCallFloatReturn(f); return true;
        case BrawlRollback::LoopCallAction::Invoke: break;
        }
    }
    if (TranslatedFunctionRegistry::FindByAddressPtr(target) == nullptr && CanRun(target)) return false;
    const MkwGuestFloatAbi* floatAbi = FloatAbiOf(target);
    NormalizeCallFloatArguments(f, floatAbi);
    HZ_EXCLUSIVE_PAUSE();  // translated code's time is not the interpreter's
    MKW_INTERP_PROFILE_PAUSE();
    if (target == 0x80017618u)
        BrawlRollback::Run(f.ctx, [](CpuContext* ctx) { InvokeIndirectCpu(0x80017618u, ctx); });
    else if (target == 0x8002A210u)
        BrawlRollback::UpdateSystem(f.ctx, [](CpuContext* ctx) { InvokeIndirectCpu(0x8002A210u, ctx); });
    else {
        InvokeIndirectCpu(target, f.ctx);
        if (target == 0x80029438u) BrawlRollback::AnchorPads();
    }
    if (loopCall) BrawlRollback::LoopCallDone(target);
    NormalizeCalleeFloatReturn(f, floatAbi);
    NormalizeCallFloatReturn(f);
    return true;
}

// A tail call (b or bctr to a translated function): the callee returns to lr. True when that is this frame's own
// return address, so the frame is done; false when the tail caller was code interpreted inside this frame (a mod's
// function called from here), which then continues at lr. Returning from the frame there would abandon the rest of
// the function the frame was entered for.
bool TailCall(Frame& f, uint32_t target, uint32_t returnAddress) {
    const uint32_t back = f.ctx->lr & ~3u;
    const MkwGuestFloatAbi* floatAbi = FloatAbiOf(target);
    NormalizeCallFloatArguments(f, floatAbi);
    {
        HZ_EXCLUSIVE_PAUSE();
        MKW_INTERP_PROFILE_PAUSE();
        InvokeIndirectCpu(target, f.ctx);
    }
    if (&f == g_rollbackLoopFrame.load(std::memory_order_relaxed)) BrawlRollback::LoopCallDone(target);
    NormalizeCalleeFloatReturn(f, floatAbi);
    NormalizeCallFloatReturn(f);
    f.ctx->lr = back;
    return back == (returnAddress & ~3u);
}

// A tail call (b/bctr to a translated function) made in the rollback loop frame, as BrawlRollback::LoopCall decides
// (Call() does the same for calls): true when handled here, with `next` the pc to continue at (inline: the callee,
// interpreted in this frame; skipped: the caller's return address, lr). False: make the tail call as usual.
bool LoopTailCall(Frame& f, uint32_t target, uint32_t& next) {
    if (&f != g_rollbackLoopFrame.load(std::memory_order_relaxed)) return false;
    switch (BrawlRollback::LoopCall(f.ctx, target)) {
    case BrawlRollback::LoopCallAction::Inline: next = target; return true;
    case BrawlRollback::LoopCallAction::Skipped: NormalizeCallFloatReturn(f); next = f.ctx->lr & ~3u; return true;
    case BrawlRollback::LoopCallAction::Invoke: return false;
    }
    return false;
}

// TailCall into a code variant entered inside a function (GuestCodePatch::ContinuationAt): a plugin loader's "call the
// original" stub runs the replaced function's first instruction and branches to the second.
bool TailCallEntry(Frame& f, void (*entry)(CpuContext*), uint32_t returnAddress) {
    const uint32_t back = f.ctx->lr & ~3u;
    {
        HZ_EXCLUSIVE_PAUSE();
        MKW_INTERP_PROFILE_PAUSE();
        entry(f.ctx);
    }
    NormalizeCallFloatReturn(f);
    f.ctx->lr = back;
    return back == (returnAddress & ~3u);
}

}  // namespace

void DumpRecentBranches() {
    const uint32_t count = std::min(t_recentBranchCount, kRecentBranches);
    std::string trail;
    char item[12];
    for (uint32_t i = 0; i < count; ++i) {
        std::snprintf(item, sizeof(item), " %08X", t_recentBranches[(t_recentBranchCount - count + i) % kRecentBranches]);
        trail += item;
    }
    RT_LOGF(RT_TAG_RUNTIME, "interpreter: last %u branch targets on this thread, oldest first:%s\n", count, trail.c_str());
}

bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }
void SetEnabled(bool enabled) { g_enabled.store(enabled, std::memory_order_relaxed); }
uint64_t InstructionCount() { return g_instructions.load(std::memory_order_relaxed); }
// Code runs at virtual addresses; a call to a low one is a bad pointer (an unrelocated offset), reported as missing.
bool CanRun(uint32_t address) {
    return address >= 0x80000000u && (address & 3u) == 0 && Memory::Contains(address, 4);
}

void Run(CpuContext* ctx, uint32_t entry) {
    HZ_EXCLUSIVE(kHzInterpTicks);
    // Each entry is logged once; the per-thread set keeps the lock off the path of every later call, and a small
    // direct-mapped cache of recent entries keeps the set's hashing off it (0.66% of P+'s game thread).
    thread_local std::unordered_set<uint32_t> seen;
    thread_local uint32_t recentEntries[1024];
    uint32_t& recentEntry = recentEntries[(entry >> 2) & 1023u];
    if (recentEntry != entry && (recentEntry = entry, seen.insert(entry).second)) {
        static std::mutex logMutex;
        static std::unordered_set<uint32_t> logged;
        std::lock_guard<std::mutex> lock(logMutex);
        if (logged.size() < 4096 && logged.insert(entry).second) {
            RT_LOGF(RT_TAG_RUNTIME, "interpreter: running untranslated guest code at 0x%08X (called from 0x%08X)\n",
                    entry, ctx->lr);
        }
    }
    Frame f{ctx, {}, entry};
    // Callers hand FPRs over in the scalar CpuContext form (the translator's entry contract), so a float argument
    // consumed by a paired op is splatted to both lanes, as lfs/fadds leave it on the hardware.
    for (auto& form : f.form) form = Form::Scalar;
    const uint32_t returnAddress = ctx->lr;
    uint64_t executed = 0;
    const auto& tracePcs = TracePcs();
    static thread_local unsigned rollbackLoopDepth=0;
    static const bool rollbackEnabled=std::getenv("BRAWL_ROLLBACK_INPUT")!=nullptr || std::getenv("BRAWL_ONLINE_IPC")!=nullptr;
    // The per-instruction bookkeeping below touches these thread_locals on every guest instruction. Resolve each once
    // per Run: on targets built with emulated TLS (Horizon), every thread_local access is otherwise a runtime call.
    unsigned& loopDepth = rollbackLoopDepth;
    uint32_t* const recentBranches = t_recentBranches;
    uint32_t& recentBranchCount = t_recentBranchCount;
    uint32_t& lastPc = t_lastPc;
    const bool rollbackLoopFrame = loopDepth && entry == 0x800171B4u;  // only on the game thread (loopDepth)
    struct LoopFrameScope {
        bool owner;
        const Frame* saved;
        ~LoopFrameScope() { if (owner) g_rollbackLoopFrame.store(saved, std::memory_order_relaxed); }
    } loopFrameScope{rollbackLoopFrame, g_rollbackLoopFrame.load(std::memory_order_relaxed)};
    if (rollbackLoopFrame) g_rollbackLoopFrame.store(&f, std::memory_order_relaxed);
#if MKW_INTERP_PROFILE
    std::optional<prof::Scope> profile;
    if (prof::Prefix() != nullptr) profile.emplace(entry, returnAddress, executed);
    prof::Table* const profileTable = profile ? profile->table : nullptr;
#endif
    for (;;) {
        const uint32_t pc = f.pc;
#if MKW_INTERP_PROFILE
        if (profileTable != nullptr) prof::CountPc(profileTable, pc);
#endif
        if(pc==0x80949BC0u) BrawlOnline::Stage();
        if(pc==0x809435F0u) BrawlRollback::SeedCrossplayStage();
        // Brawl Online's complete main-loop boundary, including pad bookkeeping,
        // render-side game writes and the application frame counter.
        if(rollbackEnabled && pc==0x800171B4u && !loopDepth) {
            MKW_INTERP_PROFILE_PAUSE();  // the rollback session's own time; each frame's Run below is its own entry
            BrawlRollback::RunLoop(ctx,[](CpuContext* c) {
                ++rollbackLoopDepth;Run(c,0x800171B4u);--rollbackLoopDepth;
            });
            for(auto& form:f.form) form=Form::Scalar;
            continue;
        }
        if(loopDepth && pc==0x80017508u) {
            for(unsigned i=0;i<32;++i) SetScalar(f,i,Scalar(f,i));
            g_instructions.fetch_add(executed,std::memory_order_relaxed);return;
        }
        if(loopDepth && BrawlRollback::DrivesLoop()) {
            if(pc==0x800172D0u) ctx->gpr[20]=1;
            if(pc==0x8001730Cu) {
                ctx->gpr[20]=1;
                Memory::Write32(ctx->gpr[23]+0xFC,Memory::Read16(ctx->gpr[23]+0xF8));
            }
        }
        const uint32_t w = MemoryInline::FlatRead32(pc);
        ctx->pc = pc;
        ++executed;
        if (pc != lastPc + 4) recentBranches[recentBranchCount++ % kRecentBranches] = pc;
        lastPc = pc;
        if (!tracePcs.empty() && std::find(tracePcs.begin(), tracePcs.end(), pc) != tracePcs.end()) TracePc(ctx, pc);
        uint32_t next = pc + 4;
        CpuContext* const c = ctx;
        switch (w >> 26) {
        case 3: break;  // twi: traps are debug asserts
        case 4: {  // paired singles
            const uint32_t xo10 = Bits(w, 1, 10);
            const uint32_t xo6 = Bits(w, 1, 6);
            const uint32_t xo5 = Bits(w, 1, 5);
            const uint32_t d = RD(w), a = RA(w), b = RB(w), cc = RC(w);
            if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {  // psq_lx, psq_stx, psq_lux, psq_stux
                const uint32_t wBit = Bits(w, 10, 1), i = Bits(w, 7, 3);
                const uint32_t ea = ((xo6 & 32u) || a) ? c->gpr[a] + c->gpr[b] : c->gpr[b];
                if (xo6 == 6 || xo6 == 38) SetPaired(f, d, PPC_PsqL(ea, wBit, i));
                else PPC_PsqSt(ea, Paired(f, d), wBit, i);
                if (xo6 & 32u) c->gpr[a] = ea;
                break;
            }
            switch (xo10) {
            case 0: case 32: case 64: case 96: {  // ps_cmpu0, ps_cmpo0, ps_cmpu1, ps_cmpo1
                const double pa = Paired(f, a), pb = Paired(f, b);
                const bool lane1 = xo10 >= 64;
                const double x = lane1 ? PpcGetPs1Inline(pa) : PpcGetPs0Inline(pa);
                const double y = lane1 ? PpcGetPs1Inline(pb) : PpcGetPs0Inline(pb);
                SetCRFloatResident(c->cr, static_cast<int>(d >> 2), x, y);
                break;
            }
            case 40: SetPaired(f, d, PPC_PsNegInline(Paired(f, b))); break;
            case 72: if (f.form[b] == Form::Scalar) SetScalar(f, d, c->fpr[b].d); else SetPaired(f, d, c->fpr[b].d); break;
            case 136: SetPaired(f, d, PPC_PsNegInline(PPC_PsAbsInline(Paired(f, b)))); break;
            case 264: SetPaired(f, d, PPC_PsAbsInline(Paired(f, b))); break;
            case 528: SetPaired(f, d, PPC_PsMerge00Inline(Paired(f, a), Paired(f, b))); break;
            case 560: SetPaired(f, d, PPC_PsMerge01Inline(Paired(f, a), Paired(f, b))); break;
            case 592: SetPaired(f, d, PPC_PsMerge10Inline(Paired(f, a), Paired(f, b))); break;
            case 624: SetPaired(f, d, PPC_PsMerge11Inline(Paired(f, a), Paired(f, b))); break;
            case 1014: {  // dcbz_l
                const uint32_t ea = ((a ? c->gpr[a] : 0) + c->gpr[b]) & ~31u;
                for (uint32_t k = 0; k < 32; k += 4) MemoryInline::FlatWrite32(ea + k, 0);
                break;
            }
            default:
                switch (xo5) {
                case 10: SetPaired(f, d, PPC_PsSum0Inline(Paired(f, a), Paired(f, b), Paired(f, cc))); break;
                case 11: SetPaired(f, d, PPC_PsSum1Inline(Paired(f, a), Paired(f, b), Paired(f, cc))); break;
                case 12: SetPaired(f, d, PPC_PsMuls0Inline(Paired(f, a), Paired(f, cc))); break;
                case 13: SetPaired(f, d, PPC_PsMuls1Inline(Paired(f, a), Paired(f, cc))); break;
                case 14: SetPaired(f, d, PPC_PsMadds0Inline(Paired(f, a), Paired(f, cc), Paired(f, b))); break;
                case 15: SetPaired(f, d, PPC_PsMadds1Inline(Paired(f, a), Paired(f, cc), Paired(f, b))); break;
                case 18: SetPaired(f, d, PPC_PsDivInline(Paired(f, a), Paired(f, b))); break;
                case 20: SetPaired(f, d, PPC_PsSubInline(Paired(f, a), Paired(f, b))); break;
                case 21: SetPaired(f, d, PPC_PsAddInline(Paired(f, a), Paired(f, b))); break;
                case 23: SetPaired(f, d, PPC_PsSelInline(Paired(f, cc), Paired(f, a), Paired(f, b))); break;
                case 24: SetPaired(f, d, PPC_PsRes(Paired(f, b))); break;
                case 25: SetPaired(f, d, PPC_PsMulInline(Paired(f, a), Paired(f, cc))); break;
                case 26: SetPaired(f, d, PPC_PsRsqrte(Paired(f, b))); break;
                case 28: SetPaired(f, d, PPC_PsMsubInline(Paired(f, a), Paired(f, cc), Paired(f, b))); break;
                case 29: SetPaired(f, d, PPC_PsMaddInline(Paired(f, a), Paired(f, cc), Paired(f, b))); break;
                case 30: SetPaired(f, d, PPC_PsNmsubInline(Paired(f, a), Paired(f, cc), Paired(f, b))); break;
                case 31: SetPaired(f, d, PPC_PsNmaddInline(Paired(f, a), Paired(f, cc), Paired(f, b))); break;
                default: Undefined(c, pc, w, "interpreter: paired-single opcode");
                }
            }
            if (Rc(w) && xo10 != 1014) SetCr1FromFpscr(c);
            break;
        }
        case 7: c->gpr[RD(w)] = static_cast<uint32_t>(static_cast<int32_t>(c->gpr[RA(w)]) * Simm(w)); break;  // mulli
        case 8: {  // subfic
            bool carry, ov;
            c->gpr[RD(w)] = AddCarry(~c->gpr[RA(w)], static_cast<uint32_t>(Simm(w)), 1, carry, ov);
            SetCa(c, carry);
            break;
        }
        case 10: SetCRResident(c->cr, c->xer, static_cast<int>(RD(w) >> 2), c->gpr[RA(w)], Uimm(w)); break;  // cmpli
        case 11:  // cmpi
            SetCRResident(c->cr, c->xer, static_cast<int>(RD(w) >> 2), static_cast<int32_t>(c->gpr[RA(w)]), Simm(w));
            break;
        case 12: case 13: {  // addic, addic.
            bool carry, ov;
            c->gpr[RD(w)] = AddCarry(c->gpr[RA(w)], static_cast<uint32_t>(Simm(w)), 0, carry, ov);
            SetCa(c, carry);
            if ((w >> 26) == 13) SetCr0(c, c->gpr[RD(w)]);
            break;
        }
        case 14: c->gpr[RD(w)] = (RA(w) ? c->gpr[RA(w)] : 0) + static_cast<uint32_t>(Simm(w)); break;  // addi
        case 15: c->gpr[RD(w)] = (RA(w) ? c->gpr[RA(w)] : 0) + (Uimm(w) << 16); break;  // addis
        case 16: {  // bc
            const int32_t disp = static_cast<int16_t>(w & 0xFFFCu);
            const uint32_t target = (w & 2u) ? static_cast<uint32_t>(disp) : pc + static_cast<uint32_t>(disp);
            if (BranchTaken(c, RD(w), RA(w))) {
                if (!(w & 1u) || !Call(f, target, pc + 4)) next = target;
            }
            break;
        }
        case 17: RT_LOGF(RT_TAG_RUNTIME, "interpreter: sc at 0x%08X ignored\n", pc); break;
        case 18: {  // b
            int32_t disp = static_cast<int32_t>(w & 0x03FFFFFCu);
            if (disp & 0x02000000) disp -= 0x04000000;
            uint32_t target = (w & 2u) ? static_cast<uint32_t>(disp) : pc + static_cast<uint32_t>(disp);
            // ba/bla to low addresses: mods (Project+) map virtual 0 onto MEM1 with an IBAT, so they reach MEM1 code.
            if (target < 0x01800000u) target |= 0x80000000u;
            if (w & 1u) { if (!Call(f, target, pc + 4)) next = target; break; }
            // A branch to another function's start is a tail call (a Gecko hook ending a function with b 0x800018A8).
            if (TranslatedFunctionRegistry::FindByAddressPtr(target) != nullptr) {
                if (LoopTailCall(f, target, next)) break;
                if (TailCall(f, target, returnAddress)) { g_instructions.fetch_add(executed, std::memory_order_relaxed); return; }
                next = c->lr & ~3u;
                break;
            }
            if (const auto continuation = GuestCodePatch::ContinuationAt(target)) {
                if (TailCallEntry(f, continuation, returnAddress)) { g_instructions.fetch_add(executed, std::memory_order_relaxed); return; }
                next = c->lr & ~3u;
                break;
            }
            next = target;
            break;
        }
        case 19: {
            const uint32_t xo = Bits(w, 1, 10);
            switch (xo) {
            case 0: c->cr = PpcMcrfResident(c->cr, RD(w) >> 2, RA(w) >> 2); break;  // mcrf
            case 16: {  // bclr
                const uint32_t target = c->lr & ~3u;
                if (BranchTaken(c, RD(w), RA(w))) {
                    if (w & 1u) { if (!Call(f, target, pc + 4)) next = target; break; }
                    if (target == (returnAddress & ~3u)) {
                        SetScalar(f, 1, Scalar(f, 1));  // the caller takes f1 as the scalar return value
                        g_instructions.fetch_add(executed, std::memory_order_relaxed);
                        return;
                    }
                    next = target;  // a computed jump through lr inside the function
                }
                break;
            }
            case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:  // crnor..cror
                c->cr = PpcCrLogicalResident(c->cr, xo == 33 ? 0 : xo == 129 ? 1 : xo == 193 ? 2 : xo == 225 ? 3 :
                                                    xo == 257 ? 4 : xo == 289 ? 5 : xo == 417 ? 6 : 7,
                                             RD(w), RA(w), RB(w));
                break;
            case 50: next = c->srr0; c->msr = c->srr1; break;  // rfi
            case 150: break;  // isync
            case 528: {  // bcctr
                const uint32_t target = c->ctr & ~3u;
                if (BranchTaken(c, RD(w) | 0x04u, RA(w))) {
                    if (w & 1u) { if (!Call(f, target, pc + 4)) next = target; break; }
                    // bctr: a switch-table jump inside this function, or a tail call to another function.
                    if (TranslatedFunctionRegistry::FindByAddressPtr(target) != nullptr) {
                        if (LoopTailCall(f, target, next)) break;
                        if (TailCall(f, target, returnAddress)) { g_instructions.fetch_add(executed, std::memory_order_relaxed); return; }
                        next = c->lr & ~3u;
                        break;
                    }
                    next = target;
                }
                break;
            }
            default: Undefined(c, pc, w, "interpreter: opcode 19");
            }
            break;
        }
        case 20: {  // rlwimi
            const uint32_t m = Mask(Bits(w, 6, 5), Bits(w, 1, 5));
            const uint32_t r = PpcRotl32Inline(c->gpr[RD(w)], RB(w));
            c->gpr[RA(w)] = (r & m) | (c->gpr[RA(w)] & ~m);
            if (Rc(w)) SetCr0(c, c->gpr[RA(w)]);
            break;
        }
        case 21:  // rlwinm
            c->gpr[RA(w)] = PpcRotl32Inline(c->gpr[RD(w)], RB(w)) & Mask(Bits(w, 6, 5), Bits(w, 1, 5));
            if (Rc(w)) SetCr0(c, c->gpr[RA(w)]);
            break;
        case 23:  // rlwnm
            c->gpr[RA(w)] = PpcRotl32Inline(c->gpr[RD(w)], c->gpr[RB(w)] & 31u) & Mask(Bits(w, 6, 5), Bits(w, 1, 5));
            if (Rc(w)) SetCr0(c, c->gpr[RA(w)]);
            break;
        case 24: c->gpr[RA(w)] = c->gpr[RD(w)] | Uimm(w); break;
        case 25: c->gpr[RA(w)] = c->gpr[RD(w)] | (Uimm(w) << 16); break;
        case 26: c->gpr[RA(w)] = c->gpr[RD(w)] ^ Uimm(w); break;
        case 27: c->gpr[RA(w)] = c->gpr[RD(w)] ^ (Uimm(w) << 16); break;
        case 28: c->gpr[RA(w)] = c->gpr[RD(w)] & Uimm(w); SetCr0(c, c->gpr[RA(w)]); break;
        case 29: c->gpr[RA(w)] = c->gpr[RD(w)] & (Uimm(w) << 16); SetCr0(c, c->gpr[RA(w)]); break;
        case 31: {
            const uint32_t xo = Bits(w, 1, 10);
            const uint32_t d = RD(w), a = RA(w), b = RB(w);
            const bool oe = (w & 0x400u) != 0;
            bool carry = false, ov = false;
            uint32_t result = 0;
            bool writeD = false, writeA = false, carryOut = false;
            switch (xo & 0x1FFu) {  // XO-form arithmetic: OE is bit 10
            case 8: result = AddCarry(~c->gpr[a], c->gpr[b], 1, carry, ov); writeD = carryOut = true; break;    // subfc
            case 10: result = AddCarry(c->gpr[a], c->gpr[b], 0, carry, ov); writeD = carryOut = true; break;    // addc
            case 11: result = static_cast<uint32_t>((static_cast<uint64_t>(c->gpr[a]) * c->gpr[b]) >> 32); writeD = true; break;
            case 40: result = AddCarry(~c->gpr[a], c->gpr[b], 1, carry, ov); writeD = true; break;              // subf
            case 75: result = static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(c->gpr[a])) *
                                                     static_cast<int32_t>(c->gpr[b])) >> 32); writeD = true; break;  // mulhw
            case 104: result = AddCarry(~c->gpr[a], 0, 1, carry, ov); writeD = true; break;                     // neg
            case 136: result = AddCarry(~c->gpr[a], c->gpr[b], Ca(c), carry, ov); writeD = carryOut = true; break;  // subfe
            case 138: result = AddCarry(c->gpr[a], c->gpr[b], Ca(c), carry, ov); writeD = carryOut = true; break;  // adde
            case 200: result = AddCarry(~c->gpr[a], 0, Ca(c), carry, ov); writeD = carryOut = true; break;         // subfze
            case 202: result = AddCarry(c->gpr[a], 0, Ca(c), carry, ov); writeD = carryOut = true; break;          // addze
            case 232: result = AddCarry(~c->gpr[a], 0xFFFFFFFFu, Ca(c), carry, ov); writeD = carryOut = true; break;  // subfme
            case 234: result = AddCarry(c->gpr[a], 0xFFFFFFFFu, Ca(c), carry, ov); writeD = carryOut = true; break;   // addme
            case 235: {  // mullw
                const int64_t wide = static_cast<int64_t>(static_cast<int32_t>(c->gpr[a])) * static_cast<int32_t>(c->gpr[b]);
                result = static_cast<uint32_t>(wide);
                ov = wide != static_cast<int32_t>(wide);
                writeD = true;
                break;
            }
            case 266: result = AddCarry(c->gpr[a], c->gpr[b], 0, carry, ov); writeD = true; break;  // add
            case 459: result = PPC_Divwu(c->gpr[a], c->gpr[b]); ov = c->gpr[b] == 0; writeD = true; break;  // divwu
            case 491:  // divw
                result = static_cast<uint32_t>(PPC_Divw(static_cast<int32_t>(c->gpr[a]), static_cast<int32_t>(c->gpr[b])));
                ov = c->gpr[b] == 0 || (c->gpr[a] == 0x80000000u && c->gpr[b] == 0xFFFFFFFFu);
                writeD = true;
                break;
            default: break;
            }
            if (writeD) {
                c->gpr[d] = result;
                if (carryOut) SetCa(c, carry);
                if (oe) SetOv(c, ov);
                if (Rc(w)) SetCr0(c, result);
                break;
            }
            switch (xo) {
            case 0: SetCRResident(c->cr, c->xer, static_cast<int>(d >> 2), static_cast<int32_t>(c->gpr[a]), static_cast<int32_t>(c->gpr[b])); break;
            case 32: SetCRResident(c->cr, c->xer, static_cast<int>(d >> 2), c->gpr[a], c->gpr[b]); break;
            case 4: break;  // tw
            case 19: c->gpr[d] = c->cr; break;  // mfcr
            case 20: case 23: c->gpr[d] = MemoryInline::FlatRead32(EaX(c, w)); break;  // lwarx, lwzx
            case 24: result = PPC_Slw(c->gpr[d], c->gpr[b]); writeA = true; break;
            case 26: result = PPC_CntlzwInline(c->gpr[d]); writeA = true; break;
            case 28: result = c->gpr[d] & c->gpr[b]; writeA = true; break;
            case 54: case 86: case 246: case 278: case 470: case 982: case 598: case 854: break;  // cache ops, sync, eieio
            case 55: { const uint32_t ea = EaX(c, w); c->gpr[d] = MemoryInline::FlatRead32(ea); c->gpr[a] = ea; break; }
            case 60: result = c->gpr[d] & ~c->gpr[b]; writeA = true; break;
            case 83: c->gpr[d] = c->msr; break;
            case 87: c->gpr[d] = MemoryInline::FlatRead8(EaX(c, w)); break;
            case 119: { const uint32_t ea = EaX(c, w); c->gpr[d] = MemoryInline::FlatRead8(ea); c->gpr[a] = ea; break; }
            case 124: result = ~(c->gpr[d] | c->gpr[b]); writeA = true; break;
            case 144: {  // mtcrf
                const uint32_t crm = Bits(w, 12, 8);
                uint32_t m = 0;
                for (int k = 0; k < 8; ++k) if (crm & (0x80u >> k)) m |= 0xF0000000u >> (k * 4);
                c->cr = (c->cr & ~m) | (c->gpr[d] & m);
                break;
            }
            case 146: c->msr = c->gpr[d]; break;
            case 150:  // stwcx.: always succeeds (one core, no reservations lost)
                MemoryInline::FlatWrite32(EaX(c, w), c->gpr[d]);
                c->cr = (c->cr & 0x0FFFFFFFu) | 0x20000000u | ((c->xer & kXerSo) ? 0x10000000u : 0u);
                break;
            case 151: MemoryInline::FlatWrite32(EaX(c, w), c->gpr[d]); break;
            case 183: { const uint32_t ea = EaX(c, w); MemoryInline::FlatWrite32(ea, c->gpr[d]); c->gpr[a] = ea; break; }
            case 210: case 242: break;  // mtsr, mtsrin
            case 215: MemoryInline::FlatWrite8(EaX(c, w), static_cast<uint8_t>(c->gpr[d])); break;
            case 247: { const uint32_t ea = EaX(c, w); MemoryInline::FlatWrite8(ea, static_cast<uint8_t>(c->gpr[d])); c->gpr[a] = ea; break; }
            case 279: c->gpr[d] = MemoryInline::FlatRead16(EaX(c, w)); break;
            case 284: result = ~(c->gpr[d] ^ c->gpr[b]); writeA = true; break;
            case 311: { const uint32_t ea = EaX(c, w); c->gpr[d] = MemoryInline::FlatRead16(ea); c->gpr[a] = ea; break; }
            case 316: result = c->gpr[d] ^ c->gpr[b]; writeA = true; break;
            case 339: c->gpr[d] = ReadSpr(c, (b << 5) | a, pc, w); break;
            case 343: c->gpr[d] = static_cast<uint32_t>(static_cast<int16_t>(MemoryInline::FlatRead16(EaX(c, w)))); break;
            case 371: c->gpr[d] = ((b << 5) | a) == 269 ? PPC_Mftbu() : PPC_Mftb(); break;
            case 375: { const uint32_t ea = EaX(c, w); c->gpr[d] = static_cast<uint32_t>(static_cast<int16_t>(MemoryInline::FlatRead16(ea))); c->gpr[a] = ea; break; }
            case 407: MemoryInline::FlatWrite16(EaX(c, w), static_cast<uint16_t>(c->gpr[d])); break;
            case 412: result = c->gpr[d] | ~c->gpr[b]; writeA = true; break;
            case 439: { const uint32_t ea = EaX(c, w); MemoryInline::FlatWrite16(ea, static_cast<uint16_t>(c->gpr[d])); c->gpr[a] = ea; break; }
            case 444: result = c->gpr[d] | c->gpr[b]; writeA = true; break;
            case 467: WriteSpr(c, (b << 5) | a, c->gpr[d]); break;
            case 476: result = ~(c->gpr[d] & c->gpr[b]); writeA = true; break;
            case 512: {  // mcrxr
                const uint32_t shift = (7u - (d >> 2)) * 4u;
                c->cr = (c->cr & ~(0xFu << shift)) | ((c->xer >> 28) << shift);
                c->xer &= 0x0FFFFFFFu;
                break;
            }
            case 534: c->gpr[d] = __builtin_bswap32(MemoryInline::FlatRead32(EaX(c, w))); break;  // lwbrx
            case 535: SetScalar(f, d, MemoryInline::FlatReadFloat32(EaX(c, w))); break;  // lfsx
            case 536: result = PPC_Srw(c->gpr[d], c->gpr[b]); writeA = true; break;
            case 567: { const uint32_t ea = EaX(c, w); SetScalar(f, d, MemoryInline::FlatReadFloat32(ea)); c->gpr[a] = ea; break; }
            case 533: case 597: {  // lswx (XER bytes from rA|0 + rB: a P+ poke loop), lswi
                uint32_t ea = xo == 533 ? EaX(c, w) : a ? c->gpr[a] : 0;
                uint32_t n = xo == 533 ? (c->xer & 0x7Fu) : b ? b : 32, r = d - 1;
                for (uint32_t k = 0; k < n; ++k) {
                    if (k % 4 == 0) { r = (r + 1) & 31u; c->gpr[r] = 0; }
                    c->gpr[r] |= static_cast<uint32_t>(MemoryInline::FlatRead8(ea++)) << (24 - 8 * (k % 4));
                }
                break;
            }
            case 599: SetScalar(f, d, MemoryInline::FlatReadFloat64(EaX(c, w))); break;  // lfdx
            case 631: { const uint32_t ea = EaX(c, w); SetScalar(f, d, MemoryInline::FlatReadFloat64(ea)); c->gpr[a] = ea; break; }
            case 662: MemoryInline::FlatWrite32(EaX(c, w), __builtin_bswap32(c->gpr[d])); break;  // stwbrx
            case 663: MemoryInline::FlatWriteFloat32(EaX(c, w), Scalar(f, d)); break;  // stfsx
            case 695: { const uint32_t ea = EaX(c, w); MemoryInline::FlatWriteFloat32(ea, Scalar(f, d)); c->gpr[a] = ea; break; }
            case 661: case 725: {  // stswx, stswi
                uint32_t ea = xo == 661 ? EaX(c, w) : a ? c->gpr[a] : 0;
                uint32_t n = xo == 661 ? (c->xer & 0x7Fu) : b ? b : 32, r = d - 1;
                for (uint32_t k = 0; k < n; ++k) {
                    if (k % 4 == 0) r = (r + 1) & 31u;
                    MemoryInline::FlatWrite8(ea++, static_cast<uint8_t>(c->gpr[r] >> (24 - 8 * (k % 4))));
                }
                break;
            }
            case 727: MemoryInline::FlatWriteFloat64(EaX(c, w), Scalar(f, d)); break;  // stfdx
            case 759: { const uint32_t ea = EaX(c, w); MemoryInline::FlatWriteFloat64(ea, Scalar(f, d)); c->gpr[a] = ea; break; }
            case 790: c->gpr[d] = __builtin_bswap16(MemoryInline::FlatRead16(EaX(c, w))); break;  // lhbrx
            case 792: {  // sraw
                const uint32_t sh = c->gpr[b] & 63u;
                const int32_t s = static_cast<int32_t>(c->gpr[d]);
                result = PPC_Sraw(c->gpr[d], c->gpr[b]);
                SetCa(c, s < 0 && (sh > 31 ? true : (static_cast<uint32_t>(s) & ((1u << sh) - 1u)) != 0));
                writeA = true;
                break;
            }
            case 824: {  // srawi
                const uint32_t sh = b;
                const int32_t s = static_cast<int32_t>(c->gpr[d]);
                result = static_cast<uint32_t>(s >> sh);
                SetCa(c, s < 0 && sh != 0 && (static_cast<uint32_t>(s) & ((1u << sh) - 1u)) != 0);
                writeA = true;
                break;
            }
            case 918: MemoryInline::FlatWrite16(EaX(c, w), __builtin_bswap16(static_cast<uint16_t>(c->gpr[d]))); break;
            case 922: result = static_cast<uint32_t>(static_cast<int16_t>(c->gpr[d])); writeA = true; break;
            case 954: result = static_cast<uint32_t>(static_cast<int8_t>(c->gpr[d])); writeA = true; break;
            case 983: MemoryInline::FlatWrite32(EaX(c, w), static_cast<uint32_t>(c->fpr[d].raw)); break;  // stfiwx
            case 1014: {  // dcbz
                const uint32_t ea = EaX(c, w) & ~31u;
                for (uint32_t k = 0; k < 32; k += 4) MemoryInline::FlatWrite32(ea + k, 0);
                break;
            }
            default: Undefined(c, pc, w, "interpreter: opcode 31");
            }
            if (writeA) {
                c->gpr[a] = result;
                if (Rc(w)) SetCr0(c, result);
            }
            break;
        }
        case 32: c->gpr[RD(w)] = MemoryInline::FlatRead32(Ea(c, w)); break;
        case 33: { const uint32_t ea = Ea(c, w); c->gpr[RD(w)] = MemoryInline::FlatRead32(ea); c->gpr[RA(w)] = ea; break; }
        case 34: c->gpr[RD(w)] = MemoryInline::FlatRead8(Ea(c, w)); break;
        case 35: { const uint32_t ea = Ea(c, w); c->gpr[RD(w)] = MemoryInline::FlatRead8(ea); c->gpr[RA(w)] = ea; break; }
        case 36: MemoryInline::FlatWrite32(Ea(c, w), c->gpr[RD(w)]); break;
        case 37: { const uint32_t ea = Ea(c, w); MemoryInline::FlatWrite32(ea, c->gpr[RD(w)]); c->gpr[RA(w)] = ea; break; }
        case 38: MemoryInline::FlatWrite8(Ea(c, w), static_cast<uint8_t>(c->gpr[RD(w)])); break;
        case 39: { const uint32_t ea = Ea(c, w); MemoryInline::FlatWrite8(ea, static_cast<uint8_t>(c->gpr[RD(w)])); c->gpr[RA(w)] = ea; break; }
        case 40: c->gpr[RD(w)] = MemoryInline::FlatRead16(Ea(c, w)); break;
        case 41: { const uint32_t ea = Ea(c, w); c->gpr[RD(w)] = MemoryInline::FlatRead16(ea); c->gpr[RA(w)] = ea; break; }
        case 42: c->gpr[RD(w)] = static_cast<uint32_t>(static_cast<int16_t>(MemoryInline::FlatRead16(Ea(c, w)))); break;
        case 43: { const uint32_t ea = Ea(c, w); c->gpr[RD(w)] = static_cast<uint32_t>(static_cast<int16_t>(MemoryInline::FlatRead16(ea))); c->gpr[RA(w)] = ea; break; }
        case 44: MemoryInline::FlatWrite16(Ea(c, w), static_cast<uint16_t>(c->gpr[RD(w)])); break;
        case 45: { const uint32_t ea = Ea(c, w); MemoryInline::FlatWrite16(ea, static_cast<uint16_t>(c->gpr[RD(w)])); c->gpr[RA(w)] = ea; break; }
        case 46: {  // lmw
            uint32_t ea = Ea(c, w);
            for (uint32_t r = RD(w); r < 32; ++r, ea += 4) c->gpr[r] = MemoryInline::FlatRead32(ea);
            break;
        }
        case 47: {  // stmw
            uint32_t ea = Ea(c, w);
            for (uint32_t r = RD(w); r < 32; ++r, ea += 4) MemoryInline::FlatWrite32(ea, c->gpr[r]);
            break;
        }
        case 48: SetScalar(f, RD(w), MemoryInline::FlatReadFloat32(Ea(c, w))); break;
        case 49: { const uint32_t ea = Ea(c, w); SetScalar(f, RD(w), MemoryInline::FlatReadFloat32(ea)); c->gpr[RA(w)] = ea; break; }
        case 50: SetScalar(f, RD(w), MemoryInline::FlatReadFloat64(Ea(c, w))); break;
        case 51: { const uint32_t ea = Ea(c, w); SetScalar(f, RD(w), MemoryInline::FlatReadFloat64(ea)); c->gpr[RA(w)] = ea; break; }
        case 52: MemoryInline::FlatWriteFloat32(Ea(c, w), Scalar(f, RD(w))); break;
        case 53: { const uint32_t ea = Ea(c, w); MemoryInline::FlatWriteFloat32(ea, Scalar(f, RD(w))); c->gpr[RA(w)] = ea; break; }
        case 54: MemoryInline::FlatWriteFloat64(Ea(c, w), Scalar(f, RD(w))); break;
        case 55: { const uint32_t ea = Ea(c, w); MemoryInline::FlatWriteFloat64(ea, Scalar(f, RD(w))); c->gpr[RA(w)] = ea; break; }
        case 56: case 57: case 60: case 61: {  // psq_l, psq_lu, psq_st, psq_stu: 12-bit displacement
            int32_t disp = static_cast<int32_t>(w & 0xFFFu);
            if (disp & 0x800) disp -= 0x1000;
            const uint32_t op = w >> 26, a = RA(w);
            const uint32_t ea = ((op == 57 || op == 61) || a ? c->gpr[a] : 0) + static_cast<uint32_t>(disp);
            const uint32_t wBit = Bits(w, 15, 1), i = Bits(w, 12, 3);
            if (op <= 57) SetPaired(f, RD(w), PPC_PsqL(ea, wBit, i));
            else PPC_PsqSt(ea, Paired(f, RD(w)), wBit, i);
            if (op == 57 || op == 61) c->gpr[a] = ea;
            break;
        }
        case 59: {  // single-precision arithmetic
            const uint32_t d = RD(w), a = RA(w), b = RB(w), cc = RC(w);
            switch (Bits(w, 1, 5)) {
            case 18: SetScalar(f, d, PpcForceSingleValueInline(PpcFdivInline(Scalar(f, a), Scalar(f, b)))); break;
            case 20: SetScalar(f, d, PpcForceSingleValueInline(PpcFsubInline(Scalar(f, a), Scalar(f, b)))); break;
            case 21: SetScalar(f, d, PpcForceSingleValueInline(PpcFaddInline(Scalar(f, a), Scalar(f, b)))); break;
            case 24: SetScalar(f, d, PpcForceSingleValueInline(PpcApproximateReciprocalInline(Scalar(f, b)))); break;
            case 25: SetScalar(f, d, PpcFmulsInline(Scalar(f, a), Scalar(f, cc))); break;
            case 28: SetScalar(f, d, PpcFmsubsInline(Scalar(f, a), Scalar(f, cc), Scalar(f, b))); break;
            case 29: SetScalar(f, d, PpcFmaddsInline(Scalar(f, a), Scalar(f, cc), Scalar(f, b))); break;
            case 30: SetScalar(f, d, PpcFnmsubsInline(Scalar(f, a), Scalar(f, cc), Scalar(f, b))); break;
            case 31: SetScalar(f, d, PpcFnmaddsInline(Scalar(f, a), Scalar(f, cc), Scalar(f, b))); break;
            default: Undefined(c, pc, w, "interpreter: opcode 59");
            }
            if (Rc(w)) SetCr1FromFpscr(c);
            break;
        }
        case 63: {  // double-precision arithmetic, moves, compares, FPSCR
            const uint32_t d = RD(w), a = RA(w), b = RB(w), cc = RC(w);
            const uint32_t xo5 = Bits(w, 1, 5);
            if (xo5 >= 18 && xo5 != 22 && xo5 != 24 && xo5 != 26 && xo5 <= 31) {
                switch (xo5) {
                case 18: SetScalar(f, d, PpcFdivInline(Scalar(f, a), Scalar(f, b))); break;
                case 20: SetScalar(f, d, PpcFsubInline(Scalar(f, a), Scalar(f, b))); break;
                case 21: SetScalar(f, d, PpcFaddInline(Scalar(f, a), Scalar(f, b))); break;
                case 23: SetScalar(f, d, Scalar(f, a) >= 0.0 ? Scalar(f, cc) : Scalar(f, b)); break;  // fsel
                case 25: SetScalar(f, d, PpcFmulInline(Scalar(f, a), Scalar(f, cc))); break;
                case 28: SetScalar(f, d, PpcFmsubInline(Scalar(f, a), Scalar(f, cc), Scalar(f, b))); break;
                case 29: SetScalar(f, d, PpcFmaddInline(Scalar(f, a), Scalar(f, cc), Scalar(f, b))); break;
                case 30: SetScalar(f, d, PpcFnmsubInline(Scalar(f, a), Scalar(f, cc), Scalar(f, b))); break;
                case 31: SetScalar(f, d, PpcFnmaddInline(Scalar(f, a), Scalar(f, cc), Scalar(f, b))); break;
                default: Undefined(c, pc, w, "interpreter: opcode 63 A-form");
                }
                if (Rc(w)) SetCr1FromFpscr(c);
                break;
            }
            switch (Bits(w, 1, 10)) {
            case 0: case 32: SetCRFloatResident(c->cr, static_cast<int>(d >> 2), Scalar(f, a), Scalar(f, b)); break;
            case 12: SetScalar(f, d, PpcForceSingleValueInline(Scalar(f, b))); break;  // frsp
            case 14: SetScalar(f, d, PPC_Fctiw(Scalar(f, b))); break;
            case 15: SetScalar(f, d, PPC_Fctiwz(Scalar(f, b))); break;
            case 26: SetScalar(f, d, PpcApproximateReciprocalSquareRootInline(Scalar(f, b))); break;  // frsqrte
            case 38: PPC_Mtfsb1(d); break;   // mtfsb1
            case 70: PPC_Mtfsb0(d); break; // mtfsb0
            case 40: SetScalar(f, d, -Scalar(f, b)); break;  // fneg
            case 64: c->cr = PPC_Mcrfs(d >> 2, a >> 2); break;
            case 72:  // fmr: copies whichever form the source holds
                c->fpr[d] = c->fpr[b];
                f.form[d] = f.form[b];
                break;
            case 134: PPC_Mtfsfi(d >> 2, Bits(w, 12, 4)); break;
            case 136: SetScalar(f, d, -std::fabs(Scalar(f, b))); break;  // fnabs
            case 264: SetScalar(f, d, std::fabs(Scalar(f, b))); break;   // fabs
            case 583: c->fpr[d].raw = c->fpscr; f.form[d] = Form::Scalar; break;  // mffs
            case 711: PPC_Mtfsf(Bits(w, 17, 8), c->fpr[b].d); break;
            default: Undefined(c, pc, w, "interpreter: opcode 63");
            }
            if (Rc(w)) SetCr1FromFpscr(c);
            break;
        }
        default:
            Undefined(c, pc, w, "interpreter: primary opcode");
        }
        f.pc = next;
    }
}

#if MKW_INTERP_PROFILE
// The segment since the last dump, to <prefix>.<n>.<label>.txt: "F frames", "T thread instructions ticks" per thread,
// "E entry calls instructions ticks translated firstCaller" per entry (translated: the address has a translation, so
// the interpreter runs it because its code was patched or redirected), "P pc instructions" per guest PC.
void ProfileDump(const char* label) {
    const char* prefix = prof::Prefix();
    if (prefix == nullptr) return;
    char path[512];
    std::snprintf(path, sizeof path, "%s.%03u.%s.txt", prefix, prof::g_dumps++, label && *label ? label : "none");
    FILE* out = std::fopen(path, "w");
    if (out == nullptr) return;
    std::fprintf(out, "F %llu\n", static_cast<unsigned long long>(prof::g_frame - prof::g_segmentStart));
    prof::g_segmentStart = prof::g_frame;
    std::unordered_map<uint32_t, prof::Entry> merged;
    std::vector<uint64_t> pcs(prof::kMem1Words + prof::kMem2Words);
    std::lock_guard<std::mutex> tablesLock(prof::g_tablesMutex);
    for (prof::Table* t : prof::g_tables) {
        std::lock_guard<std::mutex> lock(t->mutex);
        uint64_t instructions = 0, ticks = 0;
        for (auto& [entry, stat] : t->entries) {
            auto& m = merged[entry];
            m.calls += stat.calls;
            m.instructions += stat.instructions;
            m.ticks += stat.ticks;
            if (m.firstCaller == 0) m.firstCaller = stat.firstCaller;
            instructions += stat.instructions;
            ticks += stat.ticks;
            stat.calls = stat.instructions = stat.ticks = 0;
        }
        for (size_t i = 0; i < pcs.size(); ++i) {
            pcs[i] += t->pcs[i];
            t->pcs[i] = 0;
        }
        std::fprintf(out, "T %s %llu %llu other %llu\n", t->thread.c_str(), static_cast<unsigned long long>(instructions),
                     static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(t->otherInstructions));
        t->otherInstructions = 0;
    }
    for (const auto& [entry, stat] : merged) {
        if (stat.calls == 0) continue;
        std::fprintf(out, "E %08X %llu %llu %llu %d %08X\n", entry, static_cast<unsigned long long>(stat.calls),
                     static_cast<unsigned long long>(stat.instructions), static_cast<unsigned long long>(stat.ticks),
                     TranslatedFunctionRegistry::FindByAddressPtr(entry) != nullptr ? 1 : 0, stat.firstCaller);
    }
    for (size_t i = 0; i < pcs.size(); ++i) {
        if (pcs[i] == 0) continue;
        const uint32_t pc = i < prof::kMem1Words ? 0x80000000u + static_cast<uint32_t>(i) * 4
                                                 : 0x90000000u + static_cast<uint32_t>(i - prof::kMem1Words) * 4;
        std::fprintf(out, "P %08X %llu\n", pc, static_cast<unsigned long long>(pcs[i]));
    }
    std::fclose(out);
}

// Once per presented frame (GuestCodePatch::SyncFrame): a segment ends at each scene change, labelled with the scene
// it covered, and every BRAWL_INTERP_PROFILE_FRAMES frames.
void ProfileFrame() {
    if (prof::Prefix() == nullptr) return;
    ++prof::g_frame;
    static const uint64_t every = [] {
        const char* text = std::getenv("BRAWL_INTERP_PROFILE_FRAMES");
        return text ? std::strtoull(text, nullptr, 10) : 0ull;
    }();
    static std::string scene;
    std::string now = SaveStates::SceneName();
    if (now != scene) {
        ProfileDump(scene.c_str());
        scene = std::move(now);
    } else if (every != 0 && prof::g_frame - prof::g_segmentStart >= every) {
        ProfileDump(scene.c_str());
    }
}
#else
void ProfileDump(const char*) {}
void ProfileFrame() {}
#endif

}  // namespace PpcInterpreter
