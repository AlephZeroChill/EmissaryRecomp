// The instructions Decode leaves Generic, which the JIT compiles natively (src/jit/jit_x64_generic.cpp), against the classic
// interpreter: the carry forms (addic, addic., subfc, addc, adde, subfe, addze, subfze, addme, subfme), mulhw, mulhwu, divw,
// divwu, the Rc forms of the ops that have a kind without Rc, the OE forms (compiled as the interpreter's handler), orc, nand,
// eqv, sraw, the condition register ops (crand ... cror over every bit triple, mcrf over every field pair, mtcrf over every
// mask), the cache and ordering ops that do nothing, and the float ops fsel, fnabs, fnmadd, fnmsub, fres, frsqrte, fctiw,
// fctiwz. (The loads and stores among the Generic ops are in jit_mem_tests.cpp.)
//
// An instruction is placed as `insn; probe; mflr r6; mtlr r31; blr` and run from many states under Engine::Classic and
// Engine::Jit (threshold 0): every pair of edge values in the two source registers, XER carry and summary overflow cycling (so
// every carry-in both ways), a condition register of mixed field values (jit_test_cr.h); divide by zero and INT_MIN / -1 are among the edge values. The whole
// contexts are compared bytewise (a CR field must hold the interpreter's exact value), and for the float ops the probe stores the destination as a scalar and as a pair to memory, which shows
// its form.
//
// Each program's first compile is classified by JitX64KindOps(K::Generic, ...): the instruction must have been compiled
// natively, except the ones listed as the interpreter's (OE forms), which must not (so the test notices both ways).
//
// BRAWL_JIT_GENERIC=0 or BRAWL_JIT_FALLBACK=all in the environment runs the same comparisons with the native code off (the
// native-or-not check is skipped).
#include "abi_bridge.h"
#include "isa/ppc_isa_fpenv.h"
#include "jit_test_cr.h"
#include "memory.h"
#include "ppc_interpreter.h"
#include "../src/jit/jit_x64.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using PpcInterpreter::detail::JitX64KindOps;
using PpcInterpreter::detail::K;

constexpr uint32_t kReturn = 0x80020000u;
constexpr uint32_t kCodeBase = 0x80100000u;
constexpr uint32_t kProbe = 0x80190000u;
constexpr uint32_t kBlr = 0x4E800020u;

constexpr uint32_t kEdges[] = {0u, 1u, 2u, 3u, 31u, 32u, 33u, 63u, 64u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
                               0xFFFFFFFFu, 0xFFFFFFFEu, 0x12345678u, 0xFFFF0000u, 0x0000FFFFu, 0x00008000u, 0x000000FFu,
                               0xFFFFFF80u, 0x00007FFFu, 0xFFFF8000u};
constexpr size_t kEdgeCount = sizeof(kEdges) / sizeof(kEdges[0]);

enum class Native : uint8_t { Yes, No, Either };
struct Insn {
    std::string name;
    uint32_t word;
    bool wide;
    Native native = Native::Yes;
    bool probeFpr = false;  // a float op: the probe stores f3 (scalar and paired view) after it
    bool fpStates = false;  // float operand states
};
std::vector<Insn> g_insns;

constexpr uint32_t D(uint32_t op, uint32_t rt, uint32_t ra, uint32_t imm) { return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFFu); }
constexpr uint32_t X(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t xo, uint32_t rc = 0) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}
constexpr uint32_t M(uint32_t op, uint32_t rs, uint32_t ra, uint32_t shOrRb, uint32_t mb, uint32_t me, uint32_t rc) {
    return (op << 26) | (rs << 21) | (ra << 16) | (shOrRb << 11) | (mb << 6) | (me << 1) | rc;
}
constexpr uint32_t Cr(uint32_t xo, uint32_t bt, uint32_t ba, uint32_t bb) { return (19u << 26) | (bt << 21) | (ba << 16) | (bb << 11) | (xo << 1); }
constexpr uint32_t Fa(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t c, uint32_t xo5) {
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo5 << 1);
}
constexpr uint32_t Fx(uint32_t op, uint32_t d, uint32_t a, uint32_t b, uint32_t xo10) {
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (xo10 << 1);
}
constexpr uint32_t Spr(uint32_t xo, uint32_t rt, uint32_t spr) { return X(rt, spr & 31u, spr >> 5, xo); }

char g_buf[160];
template <typename... A>
std::string Fmt(const char* f, A... a) {
    std::snprintf(g_buf, sizeof g_buf, f, a...);
    return g_buf;
}
void Add(std::string name, uint32_t word, bool wide, Native native = Native::Yes) { g_insns.push_back({std::move(name), word, wide, native}); }

constexpr uint32_t kRegs[] = {0, 3, 4, 5};

// --- the instruction lists -------------------------------------------------------------------------------------------------

void AddCarryForms() {
    const int32_t imms[] = {0, 1, -1, 0x7FFF, -0x8000, 0x1234, -0x1234, 0xFF, 0x8000, 31};
    for (int32_t i : imms)
        for (uint32_t rd : kRegs)
            for (uint32_t ra : kRegs) {
                Add(Fmt("addic r%u,r%u,%d", rd, ra, i), D(12, rd, ra, i), false);
                Add(Fmt("addic. r%u,r%u,%d", rd, ra, i), D(13, rd, ra, i), false);
            }
    struct Xo { const char* name; uint32_t xo; bool twoSources; };
    const Xo xos[] = {{"subfc", 8, true}, {"addc", 10, true}, {"adde", 138, true}, {"subfe", 136, true}, {"addze", 202, false},
                      {"subfze", 200, false}, {"addme", 234, false}, {"subfme", 232, false}, {"mulhwu", 11, true},
                      {"mulhw", 75, true}, {"divwu", 459, true}, {"divw", 491, true}, {"add", 266, true}, {"subf", 40, true},
                      {"neg", 104, false}, {"mullw", 235, true}};
    for (const Xo& op : xos)
        for (uint32_t rc = 0; rc < 2; ++rc)
            for (uint32_t oe = 0; oe < 2; ++oe)
                for (uint32_t d : kRegs)
                    for (uint32_t a : kRegs)
                        for (uint32_t b : kRegs) {
                            if (!op.twoSources && b != 3) continue;  // rB is not read: one value is enough
                            const bool kind = (op.xo == 266 || op.xo == 40 || op.xo == 104 || op.xo == 235) && !rc && !oe;
                            if (kind) continue;  // a kind of its own: the int tests have it
                            // Rc forms of the carry ops and the like are native; OE forms are the interpreter's
                            Add(Fmt("%s%s%s r%u,r%u,r%u", op.name, oe ? "o" : "", rc ? "." : "", d, a, b),
                                X(d, a, op.twoSources ? b : 0, op.xo + (oe ? 512u : 0u), rc), true,
                                oe ? Native::No : Native::Yes);
                        }
}

void AddLogicalForms() {
    struct Op { const char* name; uint32_t xo; };
    const Op ops[] = {{"xor", 316}, {"andc", 60}, {"nor", 124}, {"slw", 24}, {"srw", 536}, {"sraw", 792}, {"orc", 412},
                      {"nand", 476}, {"eqv", 284}};
    for (const Op& op : ops)
        for (uint32_t rc = 0; rc < 2; ++rc) {
            if (rc == 0 && (op.xo == 316 || op.xo == 60 || op.xo == 124 || op.xo == 24 || op.xo == 536)) continue;  // kinds
            for (uint32_t s : kRegs)
                for (uint32_t a : kRegs)
                    for (uint32_t b : kRegs) Add(Fmt("%s%s r%u,r%u,r%u", op.name, rc ? "." : "", a, s, b), X(s, a, b, op.xo, rc), true);
        }
    for (uint32_t rc = 0; rc < 2; ++rc)
        for (uint32_t s : kRegs)
            for (uint32_t a : kRegs) {
                if (rc)
                    for (uint32_t sh = 0; sh < 32; ++sh) Add(Fmt("srawi. r%u,r%u,%u", a, s, sh), X(s, a, sh, 824, 1), false, Native::Yes);
                if (rc) {
                    Add(Fmt("extsb. r%u,r%u", a, s), X(s, a, 0, 954, 1), false);
                    Add(Fmt("extsh. r%u,r%u", a, s), X(s, a, 0, 922, 1), false);
                    Add(Fmt("cntlzw. r%u,r%u", a, s), X(s, a, 0, 26, 1), false);
                    for (uint32_t sh : {0u, 1u, 5u, 16u, 31u})
                        for (uint32_t mb : {0u, 8u, 31u})
                            for (uint32_t me : {0u, 23u, 31u})
                                Add(Fmt("rlwimi. r%u,r%u,%u,%u,%u", a, s, sh, mb, me), M(20, s, a, sh, mb, me, 1), true);
                }
            }
}

void AddCrOps() {
    // crnor crandc crxor crnand crand creqv crorc cror
    const uint32_t xos[] = {33, 129, 193, 225, 257, 289, 417, 449};
    const uint32_t sample[] = {0, 1, 5, 13, 17, 22, 30, 31};
    for (uint32_t xo : xos) {
        const bool full = xo == 193 || xo == 417;  // every triple for two of them, a sample of the rest
        for (uint32_t bt = 0; bt < 32; ++bt)
            for (uint32_t ba = 0; ba < 32; ++ba) {
                if (!full && ba % 4 != 1 && ba != bt) continue;
                for (uint32_t bb = 0; bb < 32; ++bb) {
                    if (!full) {
                        bool keep = bb == ba || bb == bt;
                        for (uint32_t s : sample) keep |= s == bb;
                        if (!keep) continue;
                    }
                    Add(Fmt("cr op xo %u bt %u ba %u bb %u", xo, bt, ba, bb), Cr(xo, bt, ba, bb), false);
                }
            }
    }
    for (uint32_t bf = 0; bf < 8; ++bf)
        for (uint32_t bfa = 0; bfa < 8; ++bfa) Add(Fmt("mcrf cr%u,cr%u", bf, bfa), (19u << 26) | (bf << 23) | (bfa << 18), false);
    for (uint32_t crm = 0; crm < 256; ++crm)
        for (uint32_t r : {0u, 3u, 4u}) Add(Fmt("mtcrf 0x%02X,r%u", crm, r), (31u << 26) | (r << 21) | (crm << 12) | (144u << 1), false);
    // ordering and cache hints, which Exec ignores
    for (uint32_t xo : {598u, 854u, 54u, 86u, 246u, 278u, 470u, 982u})
        Add(Fmt("cache/ordering xo %u", xo), X(0, 3, 4, xo), false);
    Add("isync", (19u << 26) | (150u << 1), false);
}

void AddFloatForms() {
    const auto f = [](std::string name, uint32_t word) {
        Insn i{std::move(name), word, false};
        i.probeFpr = true;
        i.fpStates = true;
        g_insns.push_back(std::move(i));
    };
    for (uint32_t d : {3u, 4u})
        for (uint32_t a : {1u, 3u}) {
            f(Fmt("fsel f%u,f%u,f2,f4", d, a), Fa(63, d, a, 4, 2, 23));
            f(Fmt("fsel f%u,f%u,f%u,f%u", d, a, a, a), Fa(63, d, a, a, a, 23));
            f(Fmt("fnmadd f%u,f%u,f2,f4", d, a), Fa(63, d, a, 4, 2, 31));
            f(Fmt("fnmsub f%u,f%u,f2,f4", d, a), Fa(63, d, a, 4, 2, 30));
            f(Fmt("fnmadd f%u,f%u,f%u,f%u", d, a, a, a), Fa(63, d, a, a, a, 31));
            f(Fmt("fnabs f%u,f%u", d, a), Fx(63, d, 0, a, 136));
            f(Fmt("fres f%u,f%u", d, a), Fa(59, d, 0, a, 0, 24));
            f(Fmt("frsqrte f%u,f%u", d, a), Fx(63, d, 0, a, 26));
            f(Fmt("fctiw f%u,f%u", d, a), Fx(63, d, 0, a, 14));
            f(Fmt("fctiwz f%u,f%u", d, a), Fx(63, d, 0, a, 15));
        }
    // Rc forms of the float ops are the interpreter's
    g_insns.push_back({"fsel. f3,f1,f2,f4", Fa(63, 3, 1, 4, 2, 23) | 1u, false, Native::No, true, true});
    g_insns.push_back({"fnabs. f3,f1", Fx(63, 3, 0, 1, 136) | 1u, false, Native::No, true, true});
}

// --- the float operand tables ---------------------------------------------------------------------------------------------------

std::vector<uint64_t> g_doubles, g_pairs;

void BuildTables() {
    const uint64_t mags[] = {
        0x0000000000000000ull, 0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x3690000000000000ull,
        0x3800000000000000ull, 0x380FFFFFE0000000ull, 0x3810000000000000ull, 0x3E70000000000000ull, 0x3FE0000000000000ull,
        0x3FF0000000000000ull, 0x3FF0000000000001ull, 0x3FF0000010000000ull, 0x3FF8000000000000ull, 0x4000000000000000ull,
        0x4008000000000000ull, 0x40091EB851EB851Full, 0x41E0000000000000ull, 0x41DFFFFFFFC00000ull, 0x41E00000001FFFFFull,
        0x43E0000000000000ull, 0x47EFFFFFE0000000ull, 0x47F0000000000000ull, 0x7FDFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull,
        0x7FF0000000000000ull, 0x4330000000000000ull, 0x4059000000000000ull,
    };
    for (uint64_t m : mags) {
        g_doubles.push_back(m);
        g_doubles.push_back(m | 0x8000000000000000ull);
    }
    for (uint64_t n : {0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF8000000001234ull, 0x7FF0000000000001ull})
        g_doubles.push_back(n);
    const uint32_t singles[] = {0x00000000u, 0x3F800000u, 0xBF800000u, 0x40490FDBu, 0x7F7FFFFFu, 0x00800000u, 0x7F800000u, 0x7FC00000u};
    for (uint32_t a : singles)
        for (uint32_t b : singles) g_pairs.push_back((static_cast<uint64_t>(a) << 32) | b);
}

// --- running ---------------------------------------------------------------------------------------------------------------------

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t Next() {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return static_cast<uint32_t>((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    uint64_t Next64() { return (uint64_t{Next()} << 32) | Next(); }
};

CpuContext StateFor(size_t i, const Insn& insn, size_t k) {
    CpuContext c{};
    Rng r(i * 1315423911ull + k + 1);
    for (auto& g : c.gpr) g = r.Next();
    const size_t x = insn.wide ? k / kEdgeCount : k % kEdgeCount, y = insn.wide ? k % kEdgeCount : (k / kEdgeCount) % kEdgeCount;
    c.gpr[3] = kEdges[x % kEdgeCount];
    c.gpr[4] = kEdges[y % kEdgeCount];
    c.gpr[5] = (k & 1) ? kEdges[(x + y) % kEdgeCount] : r.Next();
    c.gpr[0] = (k & 2) ? kEdges[(x * 3 + y) % kEdgeCount] : r.Next();
    c.gpr[1] = 0x80180000u;
    c.gpr[30] = kProbe;
    JitTestCr::Randomize(c, [&] { return r.Next(); });
    const uint32_t xers[] = {0u, 0x20000000u, 0x80000000u, 0xE0000000u};
    c.xer = xers[k % 4] | (r.Next() & 0x7Fu);
    c.ctr = r.Next();
    c.lr = kReturn;
    c.gpr[31] = kReturn;
    for (auto& q : c.gqr) q = 0;
    for (auto& fr : c.fpr) fr.raw = r.Next64();
    if (insn.fpStates) {
        // the float sources: f1, f2, f3, f4 from the edge table, as scalars and as pairs, NI mode both ways
        const uint64_t a = g_doubles[(k * 7 + 1) % g_doubles.size()], b = g_doubles[(k * 13 + 5) % g_doubles.size()];
        const uint64_t pa = g_pairs[(k * 3) % g_pairs.size()];
        c.fpr[1].raw = a;
        c.fpr[2].raw = b;
        c.fpr[3].raw = (k & 4) ? pa : g_doubles[(k * 5 + 2) % g_doubles.size()];
        c.fpr[4].raw = g_doubles[(k * 11 + 3) % g_doubles.size()];
        c.fpscr = (k & 8) ? 4u : 0u;
        c.fpscr |= (k >> 4) & 3u;  // the rounding mode, for fctiw
    }
    return c;
}

CpuContext RunOne(PpcInterpreter::Engine engine, uint32_t entry, const CpuContext& init, std::vector<uint8_t>& probe) {
    PpcInterpreter::SetEngineOverride(engine);
    CpuContext ctx = init;
    ctx.pc = 0;
    std::memset(MemoryInline::LeanHost(kProbe), 0xEE, 64);
    {
        CpuContextScope scope(&ctx);
        PpcInterpreter::Run(&ctx, entry);
    }
    ctx.pc = 0;
    probe.assign(MemoryInline::LeanHost(kProbe), MemoryInline::LeanHost(kProbe) + 64);
    return ctx;
}

}  // namespace

int main() {
    Memory::Init(0x200000);
    TranslatedFunctionRegistry::Finalize();
    if (!PpcInterpreter::JitAvailable()) {
        std::printf("jit generic: no JIT backend in this build; nothing to test\n");
        return 0;
    }
    BuildTables();
    AddCarryForms();
    AddLogicalForms();
    AddCrOps();
    AddFloatForms();
    PpcInterpreter::SetJitThreshold(0);
    const bool checkNative = std::getenv("BRAWL_JIT_FALLBACK") == nullptr && std::getenv("BRAWL_JIT_GENERIC") == nullptr;

    uint64_t runs = 0, failures = 0, nativeBad = 0, nativeOps = 0;
    constexpr uint32_t kSlots = 4096;
    for (size_t i = 0; i < g_insns.size(); ++i) {
        const Insn& insn = g_insns[i];
        const uint32_t base = kCodeBase + (static_cast<uint32_t>(i) % kSlots) * 64u;
        uint32_t n = 0;
        Memory::Write32(base + 4 * n++, insn.word);
        if (insn.probeFpr) {
            Memory::Write32(base + 4 * n++, D(52, 3, 30, 0));                                      // stfs f3,0(r30)
            Memory::Write32(base + 4 * n++, D(54, 3, 30, 8));                                      // stfd f3,8(r30)
            Memory::Write32(base + 4 * n++, (60u << 26) | (3u << 21) | (30u << 16) | (7u << 12) | 16u);  // psq_st f3,16(r30),0,7
        }
        Memory::Write32(base + 4 * n++, Spr(339, 6, 8));   // mflr r6
        Memory::Write32(base + 4 * n++, Spr(467, 31, 8));  // mtlr r31
        Memory::Write32(base + 4 * n++, kBlr);
        const size_t states = insn.fpStates ? 256 : insn.wide ? kEdgeCount * kEdgeCount : 64;
        const size_t statesUsed = insn.name.rfind("cr op", 0) == 0 ? 8 : states;
        for (size_t k = 0; k < statesUsed; ++k) {
            const CpuContext init = StateFor(i, insn, k);
            std::vector<uint8_t> wantProbe, gotProbe;
            const CpuContext want = RunOne(PpcInterpreter::Engine::Classic, base, init, wantProbe);
            const uint64_t nativeBefore = JitX64KindOps(K::Generic, true), fallbackBefore = JitX64KindOps(K::Generic, false);
            const CpuContext got = RunOne(PpcInterpreter::Engine::Jit, base, init, gotProbe);
            if (k == 0) {
                const bool wasNative = JitX64KindOps(K::Generic, true) == nativeBefore + 1;
                const bool wasFallback = JitX64KindOps(K::Generic, false) == fallbackBefore + 1;
                nativeOps += wasNative;
                if (checkNative && ((insn.native == Native::Yes && !wasNative) || (insn.native == Native::No && !wasFallback))) {
                    if (++nativeBad <= 12)
                        std::fprintf(stderr, "jit generic: %s (0x%08X) was compiled %s, expected %s\n", insn.name.c_str(), insn.word,
                                     wasNative ? "natively" : wasFallback ? "as the interpreter's handler" : "neither way",
                                     insn.native == Native::Yes ? "natively" : "as the interpreter's handler");
                }
            }
            ++runs;
            if (std::memcmp(&want, &got, sizeof want) != 0 || wantProbe != gotProbe) {
                if (++failures <= 12) {
                    std::fprintf(stderr, "jit generic: %s (0x%08X) differs for state %zu\n", insn.name.c_str(), insn.word, k);
                    for (int g = 0; g < 32; ++g)
                        if (want.gpr[g] != got.gpr[g])
                            std::fprintf(stderr, "  r%d: want %08X got %08X (init %08X)\n", g, want.gpr[g], got.gpr[g], init.gpr[g]);
                    for (int g = 0; g < 32; ++g)
                        if (want.fpr[g].raw != got.fpr[g].raw)
                            std::fprintf(stderr, "  f%d: want %016llX got %016llX\n", g, static_cast<unsigned long long>(want.fpr[g].raw),
                                         static_cast<unsigned long long>(got.fpr[g].raw));
                    std::fputs(JitTestCr::Diff(want, got, init).c_str(), stderr);
                    if (want.xer != got.xer) std::fprintf(stderr, "  xer: want %08X got %08X (init %08X)\n", want.xer, got.xer, init.xer);
                    if (want.fpscr != got.fpscr) std::fprintf(stderr, "  fpscr: want %08X got %08X\n", want.fpscr, got.fpscr);
                    for (size_t b = 0; b < wantProbe.size(); ++b)
                        if (wantProbe[b] != gotProbe[b]) std::fprintf(stderr, "  probe byte %zu: want %02X got %02X\n", b, wantProbe[b], gotProbe[b]);
                }
            }
        }
    }
    PpcInterpreter::ClearEngineOverride();
    std::printf("jit generic: %zu instructions (%llu compiled natively), %llu runs, %llu failures, %llu with the wrong kind of code\n",
                g_insns.size(), static_cast<unsigned long long>(nativeOps), static_cast<unsigned long long>(runs),
                static_cast<unsigned long long>(failures), static_cast<unsigned long long>(nativeBad));
    return failures == 0 && nativeBad == 0 ? 0 : 1;
}
