// The JIT's loads and stores against the classic interpreter, on what the differential test does not compare: rollback's
// dirty marks, what the checked store path observed, the exception of a store to an unmapped address, and the float
// conversions on every input class.
//
// Each case is a few guest instructions run once under Engine::Classic and once under Engine::Jit (threshold 0, so the
// block is compiled before it runs), from the same registers, the same guest bytes and cleared marks. Compared: every
// register, CR field value (bytewise) and FPR bit, the bytes and the dirty marks around each address the program touches, the address of the last
// store the executable-write check saw, and the exception a run ends with (its type, text and, for an access violation,
// address and length). A float load is followed by a store of the register as a scalar and as a pair, which shows the
// register's form.
//
// The memory is the Wii map (MEM1, MEM2, the locked cache, MEM1's mirrors); one 64 KiB chunk of MEM1 is registered as
// executable code, so the lean store policy sends its stores to the checked path.
#include "abi_bridge.h"
#include "brawl_dirty_memory.h"
#include "jit_test_cr.h"
#include "isa/ppc_isa_fpenv.h"
#include "memory.h"
#include "ppc_interpreter.h"
#include "../src/jit/jit_x64.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace RecompMod {
extern std::atomic<uint32_t> g_testLastExecutableWrite;  // interpreter_test_support.cpp
}

namespace {

using PpcInterpreter::Engine;

// --- encoders -------------------------------------------------------------------------------------------------------------
constexpr uint32_t D(uint32_t op, uint32_t rt, uint32_t ra, int32_t imm) {
    return (op << 26) | (rt << 21) | (ra << 16) | (static_cast<uint32_t>(imm) & 0xFFFFu);
}
constexpr uint32_t X(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t xo) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1);
}
constexpr uint32_t Psq(uint32_t op, uint32_t frt, uint32_t ra, uint32_t w, uint32_t i, int32_t disp) {
    return (op << 26) | (frt << 21) | (ra << 16) | (w << 15) | (i << 12) | (static_cast<uint32_t>(disp) & 0xFFFu);
}
constexpr uint32_t kBlr = 0x4E800020u;
constexpr uint32_t Fmr(uint32_t d, uint32_t b) { return (63u << 26) | (d << 21) | (b << 11) | (72u << 1); }
constexpr uint32_t PsMr(uint32_t d, uint32_t b) { return (4u << 26) | (d << 21) | (b << 11) | (72u << 1); }

// --- the machine --------------------------------------------------------------------------------------------------------
constexpr uint32_t kCodeBase = 0x80100000u;
constexpr uint32_t kCodeLimit = 0x80200000u;
constexpr uint32_t kScratch = 0x80340000u;  // where the observers of a float load store
constexpr uint32_t kReturn = 0x80020000u;
constexpr uint32_t kFlaggedChunk = 0x80500000u;  // registered as code: the policy flags this 64 KiB chunk
constexpr int kDisp = 8;

int g_failures = 0;
uint64_t g_cases = 0;
uint64_t g_threw = 0, g_observedCodeWrite = 0, g_marked = 0;  // what the cases reached, so a test that stopped reaching it is noticed
uint32_t g_codeNext = kCodeBase;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint32_t Next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<uint32_t>((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    uint64_t Next64() { return (uint64_t{Next()} << 32) | Next(); }
};

struct Snap {
    CpuContext ctx{};
    bool threw = false;
    std::string what;
    uint32_t violationAddress = 0;
    uint64_t violationLength = 0;
    std::vector<uint8_t> bytes, marks;
    uint32_t lastExecutableWrite = 0;
};

// What a case compares around: the guest addresses it touches (a window of 256 bytes below to 256 above each, and the
// marks of the blocks there).
struct Case {
    std::string name;
    std::vector<uint32_t> words;
    CpuContext init{};
    std::vector<uint32_t> windows;
    bool compareBytes = true;       // false where the addresses have no backing (a store that must throw)
    std::function<void()> prepare;  // the guest bytes the run starts from (deterministic)
};

uint32_t Canonical(uint32_t address) { return address & MemoryInline::kLeanMask; }

CpuContext BaseContext(uint64_t seed) {
    Rng r(seed);
    CpuContext c{};
    for (auto& g : c.gpr) g = r.Next();
    for (auto& f : c.fpr) f.raw = r.Next64();
    c.gpr[1] = 0x80031800u;
    c.gpr[4] = kDisp;
    c.gpr[6] = kScratch;
    JitTestCr::Randomize(c, [&] { return r.Next(); });
    c.xer = r.Next() & 0xE000007Fu;
    c.ctr = r.Next();
    c.lr = kReturn;
    c.gqr[0] = 0;
    return c;
}

// The classic interpreter and the JIT, one run each from the same state.
Snap RunOne(Engine engine, const Case& c, uint32_t entry) {
    if (c.prepare) c.prepare();
    for (uint32_t w : c.windows) {
        const uint32_t first = (Canonical(w) >> 6) - 8, last = (Canonical(w) >> 6) + 12;
        std::memset(BrawlDirtyMemory::leanBlocks + first, 0, last - first + 1);
    }
    RecompMod::g_testLastExecutableWrite.store(0);
    PpcInterpreter::SetEngineOverride(engine);
    PpcInterpreter::SetJitThreshold(0);
    Snap s;
    CpuContext ctx = c.init;
    ctx.pc = 0;
    {
        CpuContextScope scope(&ctx);
        try {
            PpcInterpreter::Run(&ctx, entry);
        } catch (const Memory::AccessViolation& e) {
            s.threw = true;
            s.what = e.what();
            s.violationAddress = e.address();
            s.violationLength = e.length();
        } catch (const std::exception& e) {
            s.threw = true;
            s.what = e.what();
        } catch (...) {
            s.threw = true;
            s.what = "unknown exception";
        }
    }
    ctx.pc = 0;
    s.ctx = ctx;
    for (uint32_t w : c.windows) {
        if (c.compareBytes) {
            const uint8_t* p = MemoryInline::LeanHost((w & ~63u) - 256u);
            s.bytes.insert(s.bytes.end(), p, p + 576);
        }
        const uint8_t* m = BrawlDirtyMemory::leanBlocks + (Canonical(w) >> 6) - 8;
        s.marks.insert(s.marks.end(), m, m + 21);
    }
    s.lastExecutableWrite = RecompMod::g_testLastExecutableWrite.load();
    return s;
}

std::string Diff(const Snap& a, const Snap& b) {
    std::string out;
    char line[256];
    const auto word = [&](const char* name, uint64_t x, uint64_t y) {
        if (x == y) return;
        std::snprintf(line, sizeof line, "    %s: classic=%llX jit=%llX\n", name, static_cast<unsigned long long>(x),
                      static_cast<unsigned long long>(y));
        out += line;
    };
    if (a.threw != b.threw) word("threw", a.threw, b.threw);
    if (a.what != b.what) out += "    exception text: '" + a.what + "' vs '" + b.what + "'\n";
    word("violation address", a.violationAddress, b.violationAddress);
    word("violation length", a.violationLength, b.violationLength);
    for (int i = 0; i < 32; ++i) {
        char n[16];
        std::snprintf(n, sizeof n, "r%d", i);
        word(n, a.ctx.gpr[i], b.ctx.gpr[i]);
        std::snprintf(n, sizeof n, "f%d", i);
        word(n, a.ctx.fpr[i].raw, b.ctx.fpr[i].raw);
    }
    for (uint32_t i = 0; i < 8; ++i) {  // bytewise (stwcx. stores CR0's exact value; no other field may change)
        char n[16];
        std::snprintf(n, sizeof n, "crv[%u]", i);
        word(n, static_cast<uint64_t>(a.ctx.crv[i]), static_cast<uint64_t>(b.ctx.crv[i]));
    }
    word("lr", a.ctx.lr, b.ctx.lr);
    word("ctr", a.ctx.ctr, b.ctx.ctr);
    word("last executable write", a.lastExecutableWrite, b.lastExecutableWrite);
    for (size_t i = 0; i < a.bytes.size() && i < b.bytes.size(); ++i)
        if (a.bytes[i] != b.bytes[i]) {
            std::snprintf(line, sizeof line, "    byte %zu (window %zu +%zu): classic=%02X jit=%02X\n", i, i / 576, i % 576,
                          a.bytes[i], b.bytes[i]);
            out += line;
            if (out.size() > 1500) break;
        }
    for (size_t i = 0; i < a.marks.size() && i < b.marks.size(); ++i)
        if (a.marks[i] != b.marks[i]) {
            std::snprintf(line, sizeof line, "    mark %zu (window %zu block %zu): classic=%u jit=%u\n", i, i / 21, i % 21,
                          a.marks[i], b.marks[i]);
            out += line;
        }
    return out;
}

void Run(const Case& c) {
    // addresses recycle: a new program replaces the old one's blocks
    if (g_codeNext + 4u * c.words.size() + 16 >= kCodeLimit) g_codeNext = kCodeBase;
    const uint32_t entry = g_codeNext;
    g_codeNext += 4u * static_cast<uint32_t>(c.words.size()) + 0x20u;
    g_codeNext = (g_codeNext + 31u) & ~31u;
    for (size_t i = 0; i < c.words.size(); ++i) Memory::Write32(entry + 4u * static_cast<uint32_t>(i), c.words[i]);
    const Snap classic = RunOne(Engine::Classic, c, entry);
    const Snap jit = RunOne(Engine::Jit, c, entry);
    ++g_cases;
    g_threw += classic.threw;
    g_observedCodeWrite += classic.lastExecutableWrite != 0;
    for (uint8_t m : classic.marks)
        if (m != 0) {
            ++g_marked;
            break;
        }
    const std::string diff = Diff(classic, jit);
    if (!diff.empty()) {
        if (++g_failures <= 12) {
            std::printf("FAIL %s\n", c.name.c_str());
            for (size_t i = 0; i < c.words.size(); ++i) std::printf("    %08X", c.words[i]);
            std::printf("\n%s", diff.c_str());
        }
    }
}

// Random bytes around an address, with `word` (and `second` after it) planted at it.
std::function<void()> Fill(uint32_t address, uint64_t seed, uint32_t word, uint32_t second = 0x5A5AA5A5u) {
    return [=] {
        Rng r(seed);
        uint8_t* p = MemoryInline::LeanHost((address & ~63u) - 256u);
        for (int i = 0; i < 576; ++i) p[i] = static_cast<uint8_t>(r.Next());
        const uint32_t be[2] = {__builtin_bswap32(word), __builtin_bswap32(second)};
        std::memcpy(MemoryInline::LeanHost(address), be, 8);
        uint8_t* s = MemoryInline::LeanHost(kScratch - 64);
        for (int i = 0; i < 192; ++i) s[i] = 0x33;
    };
}

// --- the addresses ------------------------------------------------------------------------------------------------------
struct Place {
    const char* name;
    uint32_t base;
    bool flagged;
};
const Place kPlaces[] = {
    {"mem1", 0x80300000u, false},
    {"mem1-chunk-edge", 0x80310000u, false},          // a store crossing it moves from one unflagged chunk to the next
    {"mem2", 0x90100000u, false},
    {"mem1-uncached", 0xC0320000u, false},
    {"mem1-physical", 0x00330000u, false},
    {"mem1-mirror", 0x86300000u, false},
    {"locked-cache", 0xE0001000u, false},
    {"code-chunk", kFlaggedChunk + 0x1000u, true},    // inside the registered code chunk, its pages without code
    {"code-chunk-start", kFlaggedChunk, true},
    {"code-chunk-end", kFlaggedChunk + 0x10000u, true},  // below it: unflagged, above: flagged
};
// Offsets from a place's base: a block's start, middle and end, the straddles, and a chunk's edge for every width.
const int kDeltas[] = {-132, -129, -64, -9, -8, -4, -3, -2, -1, 0, 1, 2, 3, 4, 7, 8, 28, 29, 31, 32, 33, 56, 57, 58, 59, 60,
                       61, 62, 63, 64, 65, 120, 121, 123, 124, 125, 127, 128};

// --- stores ---------------------------------------------------------------------------------------------------------------
struct StoreKind {
    const char* name;
    uint32_t (*encode)(uint32_t value, uint32_t base);
    bool update;
    bool isFloat, isPsq;
    uint32_t width;  // bytes the program writes at most
};
const StoreKind kStores[] = {
    {"stw", [](uint32_t d, uint32_t a) { return D(36, d, a, kDisp); }, false, false, false, 4},
    {"stwu", [](uint32_t d, uint32_t a) { return D(37, d, a, kDisp); }, true, false, false, 4},
    {"stb", [](uint32_t d, uint32_t a) { return D(38, d, a, kDisp); }, false, false, false, 1},
    {"stbu", [](uint32_t d, uint32_t a) { return D(39, d, a, kDisp); }, true, false, false, 1},
    {"sth", [](uint32_t d, uint32_t a) { return D(44, d, a, kDisp); }, false, false, false, 2},
    {"sthu", [](uint32_t d, uint32_t a) { return D(45, d, a, kDisp); }, true, false, false, 2},
    {"stwx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 151); }, false, false, false, 4},
    {"stbx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 215); }, false, false, false, 1},
    {"sthx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 407); }, false, false, false, 2},
    {"stfs", [](uint32_t d, uint32_t a) { return D(52, d, a, kDisp); }, false, true, false, 4},
    {"stfsu", [](uint32_t d, uint32_t a) { return D(53, d, a, kDisp); }, true, true, false, 4},
    {"stfsx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 663); }, false, true, false, 4},
    {"stfd", [](uint32_t d, uint32_t a) { return D(54, d, a, kDisp); }, false, true, false, 8},
    {"stfdu", [](uint32_t d, uint32_t a) { return D(55, d, a, kDisp); }, true, true, false, 8},
    {"psq_st w0", [](uint32_t d, uint32_t a) { return Psq(60, d, a, 0, 0, kDisp); }, false, true, true, 8},
    {"psq_stu w0", [](uint32_t d, uint32_t a) { return Psq(61, d, a, 0, 0, kDisp); }, true, true, true, 8},
    {"psq_st w1", [](uint32_t d, uint32_t a) { return Psq(60, d, a, 1, 0, kDisp); }, false, true, true, 4},
    {"psq_stu w1", [](uint32_t d, uint32_t a) { return Psq(61, d, a, 1, 0, kDisp); }, true, true, true, 4},
    {"psq_st q1 w0", [](uint32_t d, uint32_t a) { return Psq(60, d, a, 0, 1, kDisp); }, false, true, true, 8},
    // the forms Decode leaves Generic, which jit_x64_generic.cpp and the memory group give native code
    {"stwux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 183); }, true, false, false, 4},
    {"stbux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 247); }, true, false, false, 1},
    {"sthux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 439); }, true, false, false, 2},
    {"stwbrx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 662); }, false, false, false, 4},
    {"sthbrx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 918); }, false, false, false, 2},
    {"stwcx.", [](uint32_t d, uint32_t a) { return X(d, a, 4, 150) | 1u; }, false, false, false, 4},
    {"stwx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 151); }, false, false, false, 4},
    {"stbx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 215); }, false, false, false, 1},
    {"sthx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 407); }, false, false, false, 2},
    {"stwbrx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 662); }, false, false, false, 4},
    {"stfsux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 695); }, true, true, false, 4},
    {"stfdx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 727); }, false, true, false, 8},
    {"stfdux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 759); }, true, true, false, 8},
    {"stfsx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 663); }, false, true, false, 4},
    {"stfdx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 727); }, false, true, false, 8},
    {"stfiwx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 983); }, false, true, false, 4},
};

// A store of every kind to every place and delta, with the register it stores in its forms.
void IntegerStores() {
    for (const StoreKind& k : kStores) {
        if (k.isFloat) continue;
        for (const Place& p : kPlaces) {
            for (int delta : kDeltas) {
                const uint32_t ea = p.base + static_cast<uint32_t>(delta);
                Case c;
                c.name = std::string(k.name) + " " + p.name + " " + std::to_string(delta);
                c.init = BaseContext(1000 + delta);
                c.init.gpr[3] = ea - kDisp;
                c.words = {k.encode(5, 3), kBlr};
                c.windows = {ea};
                c.prepare = Fill(ea, 77, 0xA1B2C3D4u);
                Run(c);
            }
        }
    }
}

void MultipleStores() {
    for (uint32_t first : {31u, 30u, 29u, 28u, 20u, 10u, 5u, 1u, 0u}) {
        for (const Place& p : kPlaces) {
            for (int delta : kDeltas) {
                const uint32_t ea = p.base + static_cast<uint32_t>(delta);
                for (int store = 0; store < 2; ++store) {
                    Case c;
                    c.name = std::string(store ? "stmw r" : "lmw r") + std::to_string(first) + " " + p.name + " " +
                             std::to_string(delta);
                    c.init = BaseContext(2000 + delta);
                    c.init.gpr[3] = ea - kDisp;
                    c.words = {D(store ? 47 : 46, first, 3, kDisp), kBlr};
                    c.windows = {ea};
                    c.prepare = Fill(ea, 78, 0x01020304u);
                    Run(c);
                }
            }
        }
    }
}

// Doubles whose single narrowing takes each arm: the window 874..896 of the exponent, its edges, zeros, NaNs and
// infinities, ordinary values, and random bits.
std::vector<uint64_t> StoreValues() {
    std::vector<uint64_t> v;
    Rng r(4242);
    for (uint64_t sign : {uint64_t{0}, uint64_t{1} << 63})
        for (uint64_t exp : {0ull, 1ull, 872ull, 873ull, 874ull, 875ull, 880ull, 889ull, 895ull, 896ull, 897ull, 898ull, 1023ull,
                             1150ull, 1151ull, 1152ull, 1153ull, 2046ull, 2047ull})
            for (uint64_t frac : {0ull, 1ull, 0x8000000000000ull, 0xFFFFFFFFFFFFFull, 0x1FFFFFull, 0x1FFFFFFull, 0x123456789ABCDull,
                                  0x4000000000000ull})
                v.push_back(sign | (exp << 52) | frac);
    for (int i = 0; i < 40; ++i) v.push_back(r.Next64());
    return v;
}

// The register the store reads, made Unknown (as the context has it), Scalar (fmr) or Paired (ps_mr) in the program.
void FloatStores() {
    const std::vector<uint64_t> values = StoreValues();
    for (const StoreKind& k : kStores) {
        if (!k.isFloat) continue;
        for (int form = 0; form < 3; ++form) {
            size_t n = 0;
            for (uint64_t bits : values) {
                // every value to MEM1; the places and deltas with a few values each
                const Place& p = kPlaces[n % (sizeof kPlaces / sizeof *kPlaces)];
                const int delta = kDeltas[(n * 7) % (sizeof kDeltas / sizeof *kDeltas)];
                ++n;
                const uint32_t ea = p.base + static_cast<uint32_t>(delta);
                for (uint32_t gqr : {0u, 0x00040004u, 0x3D043D04u, 0x00070007u}) {
                    if (!k.isPsq && gqr != 0) continue;
                    for (uint32_t fpscr : {0u, 4u}) {
                        Case c;
                        c.name = std::string(k.name) + " form " + std::to_string(form) + " " + p.name + " " +
                                 std::to_string(delta) + " value " + std::to_string(bits) + " gqr " + std::to_string(gqr);
                        c.init = BaseContext(3000 + n);
                        c.init.gpr[3] = ea - kDisp;
                        c.init.fpr[5].raw = bits;
                        c.init.gqr[0] = gqr;
                        c.init.fpscr = fpscr;
                        if (form == 1) c.words.push_back(Fmr(5, 5));
                        if (form == 2) c.words.push_back(PsMr(5, 5));
                        c.words.push_back(k.encode(5, 3));
                        c.words.push_back(kBlr);
                        c.windows = {ea};
                        c.prepare = Fill(ea, 79, 0x0, 0x0);
                        Run(c);
                    }
                }
            }
        }
    }
}

// --- an unmapped store: the exception, and rA not updated ---------------------------------------------------------------
void UnmappedStores() {
    for (const StoreKind& k : kStores) {
        for (uint32_t ea : {0x84000010u, 0xF0000000u, 0x00000008u + 0x30000000u, 0xCD007000u + 0x01000000u}) {
            Case c;
            c.name = std::string(k.name) + " unmapped " + std::to_string(ea);
            c.init = BaseContext(5000 + ea);
            c.init.gpr[3] = ea - kDisp;
            c.init.fpr[5].raw = 0x3FF0000000000000ull;
            c.words = {k.encode(5, 3), kBlr};
            c.windows = {};
            c.compareBytes = false;
            Run(c);
        }
    }
    for (uint32_t ea : {0x84000010u, 0xF0000000u}) {
        Case c;
        c.name = "stmw unmapped " + std::to_string(ea);
        c.init = BaseContext(5100 + ea);
        c.init.gpr[3] = ea - kDisp;
        c.words = {D(47, 29, 3, kDisp), kBlr};
        c.compareBytes = false;
        Run(c);
    }
}

// --- loads ----------------------------------------------------------------------------------------------------------------
struct LoadKind {
    const char* name;
    uint32_t (*encode)(uint32_t dest, uint32_t base);
    bool update, isFloat, isPsq, isDouble;
};
const LoadKind kLoads[] = {
    {"lwz", [](uint32_t d, uint32_t a) { return D(32, d, a, kDisp); }, false, false, false, false},
    {"lwzu", [](uint32_t d, uint32_t a) { return D(33, d, a, kDisp); }, true, false, false, false},
    {"lbz", [](uint32_t d, uint32_t a) { return D(34, d, a, kDisp); }, false, false, false, false},
    {"lbzu", [](uint32_t d, uint32_t a) { return D(35, d, a, kDisp); }, true, false, false, false},
    {"lhz", [](uint32_t d, uint32_t a) { return D(40, d, a, kDisp); }, false, false, false, false},
    {"lhzu", [](uint32_t d, uint32_t a) { return D(41, d, a, kDisp); }, true, false, false, false},
    {"lha", [](uint32_t d, uint32_t a) { return D(42, d, a, kDisp); }, false, false, false, false},
    {"lwzx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 23); }, false, false, false, false},
    {"lbzx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 87); }, false, false, false, false},
    {"lhzx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 279); }, false, false, false, false},
    {"lfs", [](uint32_t d, uint32_t a) { return D(48, d, a, kDisp); }, false, true, false, false},
    {"lfsu", [](uint32_t d, uint32_t a) { return D(49, d, a, kDisp); }, true, true, false, false},
    {"lfsx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 535); }, false, true, false, false},
    {"lfd", [](uint32_t d, uint32_t a) { return D(50, d, a, kDisp); }, false, true, false, true},
    {"lfdu", [](uint32_t d, uint32_t a) { return D(51, d, a, kDisp); }, true, true, false, true},
    {"psq_l w0", [](uint32_t d, uint32_t a) { return Psq(56, d, a, 0, 0, kDisp); }, false, true, true, true},
    {"psq_lu w0", [](uint32_t d, uint32_t a) { return Psq(57, d, a, 0, 0, kDisp); }, true, true, true, true},
    {"psq_l w1", [](uint32_t d, uint32_t a) { return Psq(56, d, a, 1, 0, kDisp); }, false, true, true, false},
    {"psq_lu w1", [](uint32_t d, uint32_t a) { return Psq(57, d, a, 1, 0, kDisp); }, true, true, true, false},
    {"psq_l q1 w0", [](uint32_t d, uint32_t a) { return Psq(56, d, a, 0, 1, kDisp); }, false, true, true, true},
    // the forms Decode leaves Generic
    {"lwzux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 55); }, true, false, false, false},
    {"lbzux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 119); }, true, false, false, false},
    {"lhzux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 311); }, true, false, false, false},
    {"lhax", [](uint32_t d, uint32_t a) { return X(d, a, 4, 343); }, false, false, false, false},
    {"lhaux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 375); }, true, false, false, false},
    {"lhau", [](uint32_t d, uint32_t a) { return D(43, d, a, kDisp); }, true, false, false, false},
    {"lwbrx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 534); }, false, false, false, false},
    {"lhbrx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 790); }, false, false, false, false},
    {"lwarx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 20); }, false, false, false, false},
    {"lwzx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 23); }, false, false, false, false},
    {"lbzx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 87); }, false, false, false, false},
    {"lhzx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 279); }, false, false, false, false},
    {"lhax ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 343); }, false, false, false, false},
    {"lwbrx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 534); }, false, false, false, false},
    {"lfsux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 567); }, true, true, false, false},
    {"lfdx", [](uint32_t d, uint32_t a) { return X(d, a, 4, 599); }, false, true, false, true},
    {"lfdux", [](uint32_t d, uint32_t a) { return X(d, a, 4, 631); }, true, true, false, true},
    {"lfsx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 535); }, false, true, false, false},
    {"lfdx ra=0", [](uint32_t d, uint32_t a) { return X(d, 0, a, 599); }, false, true, false, true},
};

void IntegerLoads() {
    for (const LoadKind& k : kLoads) {
        if (k.isFloat) continue;
        for (const Place& p : kPlaces) {
            for (int delta : kDeltas) {
                const uint32_t ea = p.base + static_cast<uint32_t>(delta);
                Case c;
                c.name = std::string(k.name) + " " + p.name + " " + std::to_string(delta);
                c.init = BaseContext(6000 + delta);
                c.init.gpr[3] = ea - kDisp;
                c.words = {k.encode(5, 3), kBlr};
                c.windows = {ea};
                c.prepare = Fill(ea, 80, 0x80FF7F01u + static_cast<uint32_t>(delta));
                Run(c);
            }
        }
    }
}

// Words and doubles of every class, planted at the address; the register is then stored as a scalar and as a pair.
void FloatLoads() {
    std::vector<uint32_t> singles;
    Rng r(777);
    for (uint32_t sign : {0u, 0x80000000u})
        for (uint32_t exp : {0u, 1u, 2u, 126u, 127u, 128u, 253u, 254u, 255u})
            for (uint32_t frac : {0u, 1u, 0x400000u, 0x7FFFFFu, 0x200000u, 0x123456u, 0x400001u})
                singles.push_back(sign | (exp << 23) | frac);
    for (int i = 0; i < 30; ++i) singles.push_back(r.Next());
    const std::vector<uint64_t> doubles = StoreValues();
    for (const LoadKind& k : kLoads) {
        if (!k.isFloat) continue;
        const size_t count = k.isDouble && !k.isPsq ? doubles.size() : singles.size();
        for (size_t i = 0; i < count; ++i) {
            const Place& p = kPlaces[i % (sizeof kPlaces / sizeof *kPlaces)];
            const int delta = kDeltas[(i * 5) % (sizeof kDeltas / sizeof *kDeltas)];
            const uint32_t ea = p.base + static_cast<uint32_t>(delta);
            uint32_t first, second;
            if (k.isDouble && !k.isPsq) {
                first = static_cast<uint32_t>(doubles[i] >> 32);
                second = static_cast<uint32_t>(doubles[i]);
            } else {
                first = singles[i];
                second = singles[(i * 7 + 3) % singles.size()];
            }
            for (uint32_t gqr : {0u, 0x3F000000u, 0x00040004u, 0x00050005u, 0x00060006u, 0x00070007u, 0x3D043D04u}) {
                if (!k.isPsq && gqr != 0) continue;
                for (uint32_t fpscr : {0u, 4u}) {
                    Case c;
                    c.name = std::string(k.name) + " " + p.name + " " + std::to_string(delta) + " words " + std::to_string(first) +
                             "," + std::to_string(second) + " gqr " + std::to_string(gqr);
                    c.init = BaseContext(7000 + i);
                    c.init.gpr[3] = ea - kDisp;
                    c.init.gqr[0] = gqr;
                    c.init.fpscr = fpscr;
                    c.words = {k.encode(5, 3), D(52, 5, 6, 0) /* stfs f5,0(r6) */, D(54, 5, 6, 16) /* stfd f5,16(r6) */,
                               Psq(60, 5, 6, 0, 7, 32) /* psq_st f5,32(r6),0,7: GQR7 = 0, float */, kBlr};
                    c.init.gqr[7] = 0;
                    c.windows = {ea, kScratch};
                    c.prepare = Fill(ea, 81, first, second);
                    Run(c);
                }
            }
        }
    }
}

// A load or store in a block with others: a stack frame saved and restored (stwu, stmw, lmw), as a function does.
void Frames() {
    for (uint32_t first : {31u, 27u, 14u}) {
        for (const Place& p : kPlaces) {
            Case c;
            c.name = std::string("frame ") + p.name;
            const uint32_t sp = p.base + 0x200;
            c.init = BaseContext(8000 + first);
            c.init.gpr[1] = sp;
            const uint32_t n = 32 - first;
            c.words = {D(37, 1, 1, -static_cast<int32_t>(16 + 4 * n)),  // stwu r1,-N(r1)
                       0x7C0802A6u,                                       // mflr r0
                       D(47, first, 1, 8),                                // stmw
                       D(36, 0, 1, static_cast<int32_t>(16 + 4 * n + 4)),  // stw r0, N+4(r1)
                       D(32, 0, 1, static_cast<int32_t>(16 + 4 * n + 4)),  // lwz r0
                       D(46, first, 1, 8),                                // lmw
                       D(32, 1, 1, 0),                                    // lwz r1,0(r1)
                       0x7C0803A6u,                                       // mtlr r0
                       kBlr};
            c.windows = {sp, sp - 16 - 4 * n};
            c.prepare = Fill(sp - 64, 82, 0x12121212u);
            Run(c);
        }
    }
}

void SetCodeChunk(bool on) {
    RecompMod::g_executableWriteGuardEnabled.store(on);
    const uint32_t page = kFlaggedChunk + 0x800u;  // a 4 KiB guard page inside the chunk, at its start
    RecompMod::g_executableWriteGuardCoarsePages[page >> RecompMod::kExecutableWriteGuardCoarsePageShift].store(on);
    RecompMod::g_executableWriteGuardMidPages[page >> RecompMod::kExecutableWriteGuardMidPageShift].store(on);
    RecompMod::g_executableWriteGuardPages[page >> RecompMod::kExecutableWriteGuardPageShift].store(on);
    Memory::RefreshWritableFastPathsForExecutableRanges();
}

}  // namespace

int main() {
    Memory::Init(Memory::Config::WiiDefaults());
    TranslatedFunctionRegistry::Finalize();
    if (!PpcInterpreter::JitAvailable()) {
        std::printf("jit mem tests: no JIT backend in this build\n");
        return 0;
    }
    SetCodeChunk(true);
    if (!MemoryInline::LeanStoreNeedsPolicy(kFlaggedChunk + 0x1000u) || MemoryInline::LeanStoreNeedsPolicy(0x80300000u)) {
        std::printf("jit mem tests: the code chunk is not flagged as expected\n");
        return 2;
    }
    const auto stage = [](const char* name, void (*fn)()) {
        const uint64_t before = g_cases;
        const int failed = g_failures;
        fn();
        std::printf("  %-28s %7llu cases%s\n", name, static_cast<unsigned long long>(g_cases - before),
                    g_failures != failed ? "  FAILED" : "");
    };
    stage("integer stores", IntegerStores);
    stage("stmw and lmw", MultipleStores);
    stage("float stores", FloatStores);
    stage("unmapped stores", UnmappedStores);
    stage("integer loads", IntegerLoads);
    stage("float and quantized loads", FloatLoads);
    stage("frames", Frames);
    PpcInterpreter::ClearEngineOverride();
    SetCodeChunk(false);
    std::printf("  reached: %llu cases ended in an exception, %llu with a store into guarded code, %llu with dirty marks set\n",
                static_cast<unsigned long long>(g_threw), static_cast<unsigned long long>(g_observedCodeWrite),
                static_cast<unsigned long long>(g_marked));
    if (g_threw == 0 || g_observedCodeWrite == 0 || g_marked == 0) {
        std::printf("jit mem tests: a path the cases are for was never reached\n");
        ++g_failures;
    }
    // The Generic forms in the tables (indexed update, byte-reversed, algebraic, rA = 0, float indexed, stwcx., stfiwx) must have
    // been compiled natively, none as a call to the interpreter's handler (unless the environment turns the native code off).
    if (std::getenv("BRAWL_JIT_FALLBACK") == nullptr && std::getenv("BRAWL_JIT_GENERIC") == nullptr) {
        const uint64_t native = PpcInterpreter::detail::JitX64KindOps(PpcInterpreter::detail::K::Generic, true);
        const uint64_t fallback = PpcInterpreter::detail::JitX64KindOps(PpcInterpreter::detail::K::Generic, false);
        std::printf("  generic loads and stores: %llu compiled natively, %llu as the interpreter's handler\n",
                    static_cast<unsigned long long>(native), static_cast<unsigned long long>(fallback));
        if (native < 300 || fallback != 0) {
            std::printf("jit mem tests: the Generic loads and stores were not all compiled natively\n");
            ++g_failures;
        }
    }
    std::printf("jit mem tests: %llu cases, %d failures\n", static_cast<unsigned long long>(g_cases), g_failures);
    return g_failures == 0 ? 0 : 1;
}
