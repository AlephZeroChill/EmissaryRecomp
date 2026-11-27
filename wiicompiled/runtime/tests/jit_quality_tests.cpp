// The JIT's condition register liveness (src/jit/jit_x64_cr.cpp): a compare or record form whose CR field is overwritten before
// anything reads it writes no CR, and a compare fused with its bc whose field is dead on the fall-through path writes the field
// on the taken path only. A wrong "dead" verdict is a wrong CR, which random programs can miss, so every program here is a
// directed one, run under Engine::Classic and Engine::Jit (threshold 0) from many register states and the whole contexts
// compared bytewise (the jit_int_tests way): a field a write was skipped for is overwritten before the end, so the final value
// of every field is the interpreter's exact value (jit_test_cr.h).
//
//  * writer A (every compare and record form, fcmpu) x op B (one instruction per op kind, branches and calls included)
//    x reader R: `A; B; R; K; blr` with R a bc on each bit of A's field that leaves the block (to a block that reads CR, and
//    to a plain return), mfcr, a compare of another field then a bc on A's field, a call to a function that reads CR, a
//    Generic op that reads it (mcrf, crxor), a blr, and the block's end (a 32-op block falling through to a reader); K
//    overwrites the field after R, so a write skipped wrongly shows through R;
//  * chains `A; A; A; R` and mixed-field chains;
//  * compare+bc pairs (every BO shape, every BI bit of the field and of another field) whose taken path and fall-through path
//    each read the field in the next block, or whose fall-through overwrites it.
// The counters (JitX64GetCrStats) show the optimisation fired where it must, and did not where a reader intervenes.
//
// BRAWL_JIT_NO_FUSED_FLAGS only turns the live-field counter checks off. BRAWL_JIT_FALLBACK, BRAWL_JIT_FP_HELPERS or
// BRAWL_JIT_NO_CR_LIVENESS in the environment turn the counter checks off (the native code they count is off then); the context
// comparisons stay.
#include "abi_bridge.h"
#include "jit_test_cr.h"
#include "memory.h"
#include "memory_access.h"
#include "brawl_dirty_memory.h"
#include "ppc_interpreter.h"
#include "../src/jit/jit_x64.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using PpcInterpreter::detail::JitX64CrStats;
using PpcInterpreter::detail::JitX64GetCrStats;

constexpr uint32_t kReturn = 0x80020000u;
constexpr uint32_t kCodeBase = 0x80100000u;
constexpr uint32_t kSlotBytes = 0x100u;
constexpr uint32_t kSlots = 2048u;
constexpr uint32_t kTailOffset = 0xC0u;   // a block that reads CR, reached by a jump: `mfcr r4; mtlr r28; blr`
constexpr uint32_t kCalleeOffset = 0xD0u; // a function that reads CR, reached by a call: `mfcr r4; blr`
constexpr uint32_t kBlockOps = 32u;       // a block ends after this many ops
constexpr uint32_t kScratch = 0x80180100u;  // r8 points here; loads and stores stay within 0x200 bytes of it

uint32_t g_failures = 0;
uint64_t g_runs = 0, g_programs = 0;
bool g_checkCounters = true;
bool g_checkLive = true;  // BRAWL_JIT_NO_FUSED_FLAGS turns the fused-live path off

// --- encoders ----------------------------------------------------------------------------------------------------------

constexpr uint32_t D(uint32_t op, uint32_t rt, uint32_t ra, int32_t imm) {
    return (op << 26) | (rt << 21) | (ra << 16) | (static_cast<uint32_t>(imm) & 0xFFFFu);
}
constexpr uint32_t X(uint32_t rt, uint32_t ra, uint32_t rb, uint32_t xo, uint32_t rc = 0) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}
constexpr uint32_t M(uint32_t op, uint32_t rs, uint32_t ra, uint32_t shOrRb, uint32_t mb, uint32_t me, uint32_t rc) {
    return (op << 26) | (rs << 21) | (ra << 16) | (shOrRb << 11) | (mb << 6) | (me << 1) | rc;
}
constexpr uint32_t Bc(uint32_t bo, uint32_t bi, int32_t byteDisp) {
    return (16u << 26) | (bo << 21) | (bi << 16) | (static_cast<uint32_t>(byteDisp) & 0xFFFCu);
}
constexpr uint32_t Bd(int32_t byteDisp, bool link) { return (18u << 26) | (static_cast<uint32_t>(byteDisp) & 0x03FFFFFCu) | (link ? 1u : 0u); }
constexpr uint32_t kBlr = 0x4E800020u;
constexpr uint32_t kBctr = 0x4E800420u;
constexpr uint32_t kBctrl = 0x4E800421u;
constexpr uint32_t kNop = 0x60000000u;  // ori r0,r0,0
constexpr uint32_t Bclr(uint32_t bo, uint32_t bi) { return (19u << 26) | (bo << 21) | (bi << 16) | (16u << 1); }
constexpr uint32_t Cr(uint32_t xo, uint32_t bt, uint32_t ba, uint32_t bb) { return (19u << 26) | (bt << 21) | (ba << 16) | (bb << 11) | (xo << 1); }
constexpr uint32_t Mcrf(uint32_t d, uint32_t s) { return (19u << 26) | (d << 23) | (s << 18); }
constexpr uint32_t Mtcrf(uint32_t rs, uint32_t crm) { return (31u << 26) | (rs << 21) | (crm << 12) | (144u << 1); }
constexpr uint32_t Spr(uint32_t xo, uint32_t rt, uint32_t spr) { return X(rt, spr & 31u, spr >> 5, xo); }
constexpr uint32_t Mfcr(uint32_t rt) { return X(rt, 0, 0, 19); }
constexpr uint32_t Mtlr(uint32_t rs) { return Spr(467, rs, 8); }
constexpr uint32_t Cmpwi(uint32_t f, uint32_t ra, int32_t i) { return D(11, f << 2, ra, i); }
constexpr uint32_t Cmplwi(uint32_t f, uint32_t ra, uint32_t i) { return D(10, f << 2, ra, static_cast<int32_t>(i)); }
constexpr uint32_t Cmpw(uint32_t f, uint32_t ra, uint32_t rb) { return X(f << 2, ra, rb, 0); }
constexpr uint32_t Cmplw(uint32_t f, uint32_t ra, uint32_t rb) { return X(f << 2, ra, rb, 32); }
constexpr uint32_t Fp(uint32_t op, uint32_t xo5, uint32_t t, uint32_t a, uint32_t b, uint32_t c) {
    return (op << 26) | (t << 21) | (a << 16) | (b << 11) | (c << 6) | (xo5 << 1);
}
constexpr uint32_t Fcmpu(uint32_t f, uint32_t a, uint32_t b) { return (63u << 26) | (f << 23) | (a << 16) | (b << 11); }

// --- the writers A -------------------------------------------------------------------------------------------------------

struct Writer {
    std::string name;
    uint32_t word;
    uint32_t field;  // the CR field it overwrites
};

std::vector<Writer> MakeWriters(uint32_t crf) {
    std::vector<Writer> w;
    w.push_back({"cmpwi", Cmpwi(crf, 3, 5), crf});
    w.push_back({"cmpwi 0", Cmpwi(crf, 3, 0), crf});
    w.push_back({"cmplwi", Cmplwi(crf, 3, 0x8001), crf});
    w.push_back({"cmpw", Cmpw(crf, 3, 5), crf});
    w.push_back({"cmplw", Cmplw(crf, 3, 5), crf});
    w.push_back({"fcmpu", Fcmpu(crf, 1, 2), crf});
    w.push_back({"andi.", D(28, 3, 6, 0x00F0), 0});
    w.push_back({"andis.", D(29, 3, 6, 0x0100), 0});
    w.push_back({"rlwinm.", M(21, 3, 6, 3, 0, 28, 1), 0});
    w.push_back({"or.", X(3, 6, 5, 444, 1), 0});
    w.push_back({"and.", X(3, 6, 5, 28, 1), 0});
    w.push_back({"rlwnm.", M(23, 3, 6, 5, 4, 27, 1), 0});
    return w;
}

// --- the ops B: one per op kind ----------------------------------------------------------------------------------------
// `pass`: the op is one the analysis may skip over (neither reads CR nor leaves). `writes`: the CR field it overwrites, or -1.
// build(slotBase) gives the words (the tail of the program is at slotBase + kTailOffset).

struct OpB {
    std::string name;
    std::function<std::vector<uint32_t>(uint32_t slot, uint32_t at)> build;
    bool pass;
    int writes;
    bool leaves = false;  // control does not reach the code after it on every path (the reader after it is not always run)
};

std::vector<OpB> MakeOps(uint32_t crf, uint32_t other) {
    std::vector<OpB> v;
    const auto one = [&](const char* name, uint32_t w, bool pass, int writes = -1) {
        v.push_back({name, [w](uint32_t, uint32_t) { return std::vector<uint32_t>{w}; }, pass, writes, false});
    };
    // integer
    one("li", D(14, 6, 0, 5), true);
    one("addi", D(14, 6, 5, 3), true);
    one("lis", D(15, 6, 0, 0x1234), true);
    one("addis", D(15, 6, 5, 0x1234), true);
    one("mulli", D(7, 6, 5, 7), true);
    one("ori", D(24, 6, 5, 0x8001), true);
    one("oris", D(25, 6, 5, 0x8001), true);
    one("xori", D(26, 6, 5, 0x8001), true);
    one("xoris", D(27, 6, 5, 0x8001), true);
    one("andi.", D(28, 6, 5, 0x00FF), true, 0);
    one("andis.", D(29, 6, 5, 0x00FF), true, 0);
    one("rlwinm", M(21, 5, 6, 4, 8, 23, 0), true);
    one("rlwinm.", M(21, 5, 6, 4, 8, 23, 1), true, 0);
    one("rlwimi", M(20, 5, 6, 4, 8, 23, 0), true);
    one("cmpwi other", Cmpwi(other, 5, 5), true, static_cast<int>(other));
    one("cmplwi other", Cmplwi(other, 5, 5), true, static_cast<int>(other));
    one("cmpw other", Cmpw(other, 5, 6), true, static_cast<int>(other));
    one("cmplw other", Cmplw(other, 5, 6), true, static_cast<int>(other));
    one("cmpwi same", Cmpwi(crf, 5, 7), true, static_cast<int>(crf));
    one("cmplw same", Cmplw(crf, 5, 6), true, static_cast<int>(crf));
    one("add", X(6, 5, 7, 266), true);
    one("subf", X(6, 5, 7, 40), true);
    one("neg", X(6, 5, 0, 104), true);
    one("mullw", X(6, 5, 7, 235), true);
    one("or", X(5, 6, 7, 444), true);
    one("or.", X(5, 6, 7, 444, 1), true, 0);
    one("and", X(5, 6, 7, 28), true);
    one("and.", X(5, 6, 7, 28, 1), true, 0);
    one("xor", X(5, 6, 7, 316), true);
    one("andc", X(5, 6, 7, 60), true);
    one("nor", X(5, 6, 7, 124), true);
    one("slw", X(5, 6, 7, 24), true);
    one("srw", X(5, 6, 7, 536), true);
    one("srawi", X(5, 6, 3, 824), true);
    one("extsb", X(5, 6, 0, 954), true);
    one("extsh", X(5, 6, 0, 922), true);
    one("cntlzw", X(5, 6, 0, 26), true);
    one("mflr", Spr(339, 6, 8), true);
    one("mtlr", Mtlr(6), true);
    one("mfctr", Spr(339, 6, 9), true);
    one("mtctr", Spr(467, 6, 9), true);
    one("mfcr", Mfcr(6), false);
    one("subfic", D(8, 6, 5, 0x1234), true);
    one("rlwnm", M(23, 5, 6, 7, 4, 27, 0), true);
    one("rlwnm.", M(23, 5, 6, 7, 4, 27, 1), true, 0);
    one("mfspr xer", Spr(339, 6, 1), false);
    one("mtspr xer", Spr(467, 6, 1), false);
    one("mtspr gqr", Spr(467, 6, 913), false);
    // loads (pass) and stores (leave: their slow path can throw)
    one("lwz", D(32, 6, 8, 0x10), true);
    one("lwzu", D(33, 6, 8, 0x10), true);
    one("lbz", D(34, 6, 8, 0x11), true);
    one("lbzu", D(35, 6, 8, 0x11), true);
    one("lhz", D(40, 6, 8, 0x12), true);
    one("lhzu", D(41, 6, 8, 0x12), true);
    one("lha", D(42, 6, 8, 0x12), true);
    one("lwzx", X(6, 8, 9, 23), true);
    one("lbzx", X(6, 8, 9, 87), true);
    one("lhzx", X(6, 8, 9, 279), true);
    one("lmw", D(46, 29, 8, 0x40), false);
    one("stw", D(36, 5, 8, 0x20), false);
    one("stwu", D(37, 5, 8, 0x20), false);
    one("stb", D(38, 5, 8, 0x21), false);
    one("stbu", D(39, 5, 8, 0x21), false);
    one("sth", D(44, 5, 8, 0x22), false);
    one("sthu", D(45, 5, 8, 0x22), false);
    one("stmw", D(47, 29, 8, 0x80), false);
    one("stwx", X(5, 8, 9, 151), false);
    one("stbx", X(5, 8, 9, 215), false);
    one("sthx", X(5, 8, 9, 407), false);
    // float
    one("lfs", D(48, 3, 8, 0x30), true);
    one("lfsu", D(49, 3, 8, 0x30), true);
    one("lfd", D(50, 3, 8, 0x38), true);
    one("lfdu", D(51, 3, 8, 0x38), true);
    one("lfsx", X(3, 8, 9, 535), true);
    one("stfs", D(52, 3, 8, 0x40), false);
    one("stfsu", D(53, 3, 8, 0x40), false);
    one("stfd", D(54, 3, 8, 0x48), false);
    one("stfdu", D(55, 3, 8, 0x48), false);
    one("stfsx", X(3, 8, 9, 663), false);
    one("psq_l", (56u << 26) | (3u << 21) | (8u << 16) | 0x30, false);
    one("psq_lu", (57u << 26) | (3u << 21) | (8u << 16) | 0x30, false);
    one("psq_st", (60u << 26) | (3u << 21) | (8u << 16) | 0x40, false);
    one("psq_stu", (61u << 26) | (3u << 21) | (8u << 16) | 0x40, false);
    one("fadds", Fp(59, 21, 3, 1, 2, 0), true);
    one("fsubs", Fp(59, 20, 3, 1, 2, 0), true);
    one("fmuls", Fp(59, 25, 3, 1, 0, 2), true);
    one("fdivs", Fp(59, 18, 3, 1, 2, 0), true);
    one("fmadds", Fp(59, 29, 3, 1, 4, 2), true);
    one("fmsubs", Fp(59, 28, 3, 1, 4, 2), true);
    one("fnmsubs", Fp(59, 30, 3, 1, 4, 2), true);
    one("fnmadds", Fp(59, 31, 3, 1, 4, 2), true);
    one("fadd", Fp(63, 21, 3, 1, 2, 0), true);
    one("fsub", Fp(63, 20, 3, 1, 2, 0), true);
    one("fmul", Fp(63, 25, 3, 1, 0, 2), true);
    one("fdiv", Fp(63, 18, 3, 1, 2, 0), true);
    one("fmadd", Fp(63, 29, 3, 1, 4, 2), true);
    one("fmsub", Fp(63, 28, 3, 1, 4, 2), true);
    one("fneg", (63u << 26) | (3u << 21) | (2u << 11) | (40u << 1), true);
    one("fabs", (63u << 26) | (3u << 21) | (2u << 11) | (264u << 1), true);
    one("fmr", (63u << 26) | (3u << 21) | (2u << 11) | (72u << 1), true);
    one("frsp", (63u << 26) | (3u << 21) | (2u << 11) | (12u << 1), true);
    one("fcmpu other", Fcmpu(other, 1, 2), true, static_cast<int>(other));
    one("fcmpu same", Fcmpu(crf, 1, 2), true, static_cast<int>(crf));
    one("ps_add", Fp(4, 21, 3, 1, 2, 0), true);
    one("ps_sub", Fp(4, 20, 3, 1, 2, 0), true);
    one("ps_mul", Fp(4, 25, 3, 1, 0, 2), true);
    one("ps_madd", Fp(4, 29, 3, 1, 4, 2), true);
    one("ps_msub", Fp(4, 28, 3, 1, 4, 2), true);
    one("ps_nmadd", Fp(4, 31, 3, 1, 4, 2), true);
    one("ps_nmsub", Fp(4, 30, 3, 1, 4, 2), true);
    one("ps_muls0", Fp(4, 12, 3, 1, 0, 2), true);
    one("ps_muls1", Fp(4, 13, 3, 1, 0, 2), true);
    one("ps_madds0", Fp(4, 14, 3, 1, 4, 2), true);
    one("ps_madds1", Fp(4, 15, 3, 1, 4, 2), true);
    one("ps_sum0", Fp(4, 10, 3, 1, 4, 2), true);
    one("ps_sum1", Fp(4, 11, 3, 1, 4, 2), true);
    one("ps_merge00", (4u << 26) | (3u << 21) | (1u << 16) | (2u << 11) | (528u << 1), true);
    one("ps_merge01", (4u << 26) | (3u << 21) | (1u << 16) | (2u << 11) | (560u << 1), true);
    one("ps_merge10", (4u << 26) | (3u << 21) | (1u << 16) | (2u << 11) | (592u << 1), true);
    one("ps_merge11", (4u << 26) | (3u << 21) | (1u << 16) | (2u << 11) | (624u << 1), true);
    one("ps_mr", (4u << 26) | (3u << 21) | (2u << 11) | (72u << 1), true);
    one("ps_neg", (4u << 26) | (3u << 21) | (2u << 11) | (40u << 1), true);
    one("ps_abs", (4u << 26) | (3u << 21) | (2u << 11) | (264u << 1), true);
    // Generic ops that jit_x64_generic.cpp lists as neither reading nor leaving (DescribeGeneric): the carry, multiply and
    // divide forms, their Rc forms (writers of cr0), the logical forms without a kind, the indexed loads
    one("addic", D(12, 6, 5, 0x77), true);
    one("addic.", D(13, 6, 5, 0x77), true, 0);
    one("subfe", X(6, 5, 7, 136), true);
    one("addze", X(6, 5, 0, 202), true);
    one("mulhw", X(6, 5, 7, 75), true);
    one("divw", X(6, 5, 7, 491), true);
    one("addc", X(6, 5, 7, 10), true);
    one("adde.", X(6, 5, 7, 138, 1), true, 0);
    one("subfc", X(6, 5, 7, 8), true);
    one("subfze", X(6, 5, 0, 200), true);
    one("addme", X(6, 5, 0, 234), true);
    one("subfme.", X(6, 5, 0, 232, 1), true, 0);
    one("mulhwu", X(6, 5, 7, 11), true);
    one("divwu.", X(6, 5, 7, 459, 1), true, 0);
    one("addo (OE: the handler)", X(6, 5, 7, 266 + 512), true);
    one("add.", X(6, 5, 7, 266, 1), true, 0);
    one("neg.", X(6, 5, 0, 104, 1), true, 0);
    one("slw.", X(5, 6, 7, 24, 1), true, 0);
    one("srawi.", X(5, 6, 3, 824, 1), true, 0);
    one("sraw", X(5, 6, 7, 792), true);
    one("extsb.", X(5, 6, 0, 954, 1), true, 0);
    one("cntlzw.", X(5, 6, 0, 26, 1), true, 0);
    one("orc", X(5, 6, 7, 412), true);
    one("nand", X(5, 6, 7, 476), true);
    one("eqv.", X(5, 6, 7, 284, 1), true, 0);
    one("rlwimi.", M(20, 5, 6, 4, 8, 23, 1), true, 0);
    one("lwzux", X(6, 8, 9, 55), true);
    one("lhax", X(6, 8, 9, 343), true);
    one("lwbrx", X(6, 8, 9, 534), true);
    one("lfdx", X(3, 8, 9, 599), true);
    one("stwux", X(5, 8, 9, 183), false);
    one("stfdx", X(3, 8, 9, 727), false);
    one("stwbrx", X(5, 8, 9, 662), false);
    one("sync", X(0, 0, 0, 598), true);
    one("fsel", Fp(63, 23, 3, 1, 4, 2), true);
    one("fnabs", (63u << 26) | (3u << 21) | (2u << 11) | (136u << 1), true);
    one("fctiwz", (63u << 26) | (3u << 21) | (2u << 11) | (15u << 1), true);
    one("crxor", Cr(193, 4 * crf + 1, 4 * crf, 4 * other), false);
    one("crorc", Cr(417, 4 * other + 2, 4 * crf + 3, 4 * crf + 1), false);
    one("mcrf from", Mcrf(other, crf), false);
    one("mcrf to", Mcrf(crf, other), false);
    one("mtcrf", Mtcrf(7, 1u << (7 - crf)), false);
    one("mtcrf all", Mtcrf(7, 0xFF), false);
    // branches and calls
    const auto tailAbs = [](uint32_t slot, uint32_t at, int words) { return static_cast<int32_t>(slot + kTailOffset - (at + 4u * words)); };
    const auto calleeAbs = [](uint32_t slot, uint32_t at, int words) { return static_cast<int32_t>(slot + kCalleeOffset - (at + 4u * words)); };
    v.push_back({"bc over (taken: next but one)", [](uint32_t, uint32_t) { return std::vector<uint32_t>{Bc(12, 2, 8), kNop}; }, false, -1, false});
    v.push_back({"bc to tail", [tailAbs](uint32_t slot, uint32_t at) { return std::vector<uint32_t>{Bc(4, 5, tailAbs(slot, at, 0))}; },
                 false, -1, false});
    v.push_back({"bdnz over", [](uint32_t, uint32_t) { return std::vector<uint32_t>{Bc(16, 0, 8), kNop}; }, false, -1, false});
    v.push_back({"bclr (lr = return)", [](uint32_t, uint32_t) { return std::vector<uint32_t>{Bclr(12, 3)}; }, false, -1, false});
    v.push_back({"b +8 (jump followed)", [](uint32_t, uint32_t) { return std::vector<uint32_t>{Bd(8, false), kNop}; }, false, -1, false});
    v.push_back({"bl callee (call)", [calleeAbs](uint32_t slot, uint32_t at) { return std::vector<uint32_t>{Bd(calleeAbs(slot, at, 0), true)}; },
                 false, -1, false});
    v.push_back({"bctrl to callee (call)", [](uint32_t slot, uint32_t) {
                     return std::vector<uint32_t>{D(15, 7, 0, static_cast<int32_t>((slot + kCalleeOffset) >> 16)),
                                                   D(24, 7, 7, static_cast<int32_t>((slot + kCalleeOffset) & 0xFFFF)), Spr(467, 7, 9), kBctrl};
                 }, false, -1, false});
    v.push_back({"bctr to tail (jump)", [](uint32_t slot, uint32_t) {
                     return std::vector<uint32_t>{D(15, 7, 0, static_cast<int32_t>((slot + kTailOffset) >> 16)),
                                                   D(24, 7, 7, static_cast<int32_t>((slot + kTailOffset) & 0xFFFF)), Spr(467, 7, 9), kBctr};
                 }, false, -1, true});
    v.push_back({"blr (return)", [](uint32_t, uint32_t) { return std::vector<uint32_t>{kBlr}; }, false, -1, true});
    return v;
}

// --- the readers R -------------------------------------------------------------------------------------------------------
// Words that follow B. `killsAfter`: K (an overwrite of the field) follows the reader, then the return.

struct Reader {
    std::string name;
    std::function<std::vector<uint32_t>(uint32_t crf, uint32_t slot, uint32_t at, uint32_t other)> build;
    bool reads;  // the reader exposes the CR written by A (every reader here does)
    bool padToBlockEnd = false;
};

std::vector<Reader> MakeReaders() {
    std::vector<Reader> v;
    const auto toTail = [](uint32_t slot, uint32_t at, int words) { return static_cast<int32_t>(slot + kTailOffset - (at + 4u * words)); };
    const auto toCallee = [](uint32_t slot, uint32_t at, int words) { return static_cast<int32_t>(slot + kCalleeOffset - (at + 4u * words)); };
    for (uint32_t bit = 0; bit < 4; ++bit) {
        for (uint32_t bo : {12u, 4u}) {
            char name[64];
            std::snprintf(name, sizeof name, "bc %s bit %u to a reader block", bo == 12 ? "t" : "f", bit);
            v.push_back({name, [bit, bo, toTail](uint32_t crf, uint32_t slot, uint32_t at, uint32_t) {
                             return std::vector<uint32_t>{Bc(bo, 4 * crf + bit, toTail(slot, at, 0))};
                         }, true});
            std::snprintf(name, sizeof name, "bc %s bit %u over a return", bo == 12 ? "t" : "f", bit);
            v.push_back({name, [bit, bo](uint32_t crf, uint32_t, uint32_t, uint32_t) {
                             return std::vector<uint32_t>{Bc(bo, 4 * crf + bit, 8), kBlr};
                         }, true});
        }
    }
    v.push_back({"mfcr", [](uint32_t, uint32_t, uint32_t, uint32_t) { return std::vector<uint32_t>{Mfcr(4)}; }, true});
    v.push_back({"compare of another field, bc on the field", [toTail](uint32_t crf, uint32_t slot, uint32_t at, uint32_t other) {
                     return std::vector<uint32_t>{Cmpwi(other, 5, 3), Bc(12, 4 * crf + 1, toTail(slot, at, 1))};
                 }, true});
    v.push_back({"compare of another field, bc on its SO bit", [toTail](uint32_t crf, uint32_t slot, uint32_t at, uint32_t other) {
                     return std::vector<uint32_t>{Cmpwi(other, 5, 3), Bc(12, 4 * crf + 3, toTail(slot, at, 1))};
                 }, true});
    v.push_back({"blr", [](uint32_t, uint32_t, uint32_t, uint32_t) { return std::vector<uint32_t>{kBlr}; }, true});
    v.push_back({"call (bl to a function that reads CR)", [toCallee](uint32_t, uint32_t slot, uint32_t at, uint32_t) {
                     return std::vector<uint32_t>{Bd(toCallee(slot, at, 0), true)};
                 }, true});
    v.push_back({"mcrf from the field", [](uint32_t crf, uint32_t, uint32_t, uint32_t other) { return std::vector<uint32_t>{Mcrf(other, crf)}; },
                 true});
    v.push_back({"crxor on the field", [](uint32_t crf, uint32_t, uint32_t, uint32_t) {
                     return std::vector<uint32_t>{Cr(193, 4 * crf, 4 * crf + 1, 4 * crf + 2)};
                 }, true});
    v.push_back({"mtcrf another field", [](uint32_t, uint32_t, uint32_t, uint32_t other) {
                     return std::vector<uint32_t>{Mtcrf(7, 1u << (7 - other))};
                 }, true});
    v.push_back({"end of a 32-op block", [](uint32_t, uint32_t, uint32_t, uint32_t) { return std::vector<uint32_t>{}; }, true, true});
    return v;
}

// --- running --------------------------------------------------------------------------------------------------------------

CpuContext StateFor(uint64_t seed, uint32_t k) {
    CpuContext c{};
    uint64_t s = 0x9E3779B97F4A7C15ull * (seed * 2654435761ull + k + 1);
    auto next = [&] { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return static_cast<uint32_t>((s * 0x2545F4914F6CDD1Dull) >> 32); };
    static const uint32_t edge[] = {0u, 1u, 5u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 3u, 7u, 0x8001u};
    for (auto& g : c.gpr) g = (next() & 3) ? edge[next() % 9] : next();
    c.gpr[1] = 0x80180000u;
    c.gpr[8] = kScratch;
    c.gpr[9] = 8;
    c.gpr[28] = kReturn;
    JitTestCr::Randomize(c, next);
    static const uint32_t xers[] = {0u, 0x20000000u, 0x80000000u, 0xE0000000u};
    c.xer = xers[k % 4] | (next() & 0x7Fu);
    c.ctr = (k & 8) ? 1u : (next() & 0xFu);
    c.lr = kReturn;
    static const uint32_t types[] = {0u, 4u, 5u, 6u, 7u};  // 1 to 3 are reserved (the interpreter aborts)
    for (auto& q : c.gqr) q = (k & 4) ? 0u : (types[next() % 5] | ((next() & 0x3Fu) << 8) | (types[next() % 5] << 16) | ((next() & 0x3Fu) << 24));
    static const uint64_t fpEdge[] = {0x3FF0000000000000ull, 0x4000000000000000ull, 0x0ull, 0x7FF8000000000000ull, 0xBFF0000000000000ull};
    for (auto& f : c.fpr) f.raw = (next() & 3) ? fpEdge[next() % 5] : (static_cast<uint64_t>(next()) << 32 | next());
    c.fpscr = 0;
    return c;
}

void FillScratch(uint64_t seed) {
    uint64_t s = seed * 0x9E3779B97F4A7C15ull + 1;
    for (uint32_t i = 0; i < 0x200; i += 4) {
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        Memory::Write32(kScratch - 0x40 + i, static_cast<uint32_t>((s * 0x2545F4914F6CDD1Dull) >> 32));
    }
}

CpuContext RunOne(PpcInterpreter::Engine engine, uint32_t entry, const CpuContext& init, uint64_t seed) {
    PpcInterpreter::SetEngineOverride(engine);
    FillScratch(seed);
    CpuContext ctx = init;
    ctx.pc = 0;
    {
        CpuContextScope scope(&ctx);
        PpcInterpreter::Run(&ctx, entry);
    }
    ctx.pc = 0;
    return ctx;
}

struct Slot {
    uint32_t base;
};
uint32_t g_slot = 0;
Slot NextSlot() { return Slot{kCodeBase + (g_slot++ % kSlots) * kSlotBytes}; }

void WriteTail(uint32_t base) {
    Memory::Write32(base + kTailOffset, Mfcr(4));
    Memory::Write32(base + kTailOffset + 4, Mtlr(28));
    Memory::Write32(base + kTailOffset + 8, kBlr);
    Memory::Write32(base + kCalleeOffset, Mfcr(4));
    Memory::Write32(base + kCalleeOffset + 4, kBlr);
}

// Runs words (with the tail) from many states, comparing; returns the counters' change across the first JIT run.
JitX64CrStats RunProgram(const std::string& name, const std::vector<uint32_t>& words, uint32_t states = 12) {
    const Slot slot = NextSlot();
    for (uint32_t i = 0; i < words.size(); ++i) Memory::Write32(slot.base + 4 * i, words[i]);
    WriteTail(slot.base);
    ++g_programs;
    if (std::getenv("JIT_QUALITY_TRACE") != nullptr) {
        std::fprintf(stderr, "program %llu: %s:", static_cast<unsigned long long>(g_programs), name.c_str());
        for (uint32_t w : words) std::fprintf(stderr, " %08X", w);
        std::fprintf(stderr, "\n");
    }
    JitX64CrStats delta{};
    for (uint32_t k = 0; k < states; ++k) {
        const CpuContext init = StateFor(g_programs, k);
        const CpuContext want = RunOne(PpcInterpreter::Engine::Classic, slot.base, init, g_programs * 31 + k);
        const JitX64CrStats before = JitX64GetCrStats();
        const CpuContext got = RunOne(PpcInterpreter::Engine::Jit, slot.base, init, g_programs * 31 + k);
        const JitX64CrStats after = JitX64GetCrStats();
        if (k == 0)
            delta = {after.blocksAnalysed - before.blocksAnalysed, after.writesSkipped - before.writesSkipped,
                     after.fusedFlagsOnly - before.fusedFlagsOnly, after.compactWrites - before.compactWrites,
                     after.fusedLive - before.fusedLive};
        ++g_runs;
        if (std::memcmp(&want, &got, sizeof want) != 0) {
            if (++g_failures <= 20) {
                std::fprintf(stderr, "jit quality: %s differs for state %u\n", name.c_str(), k);
                std::fprintf(stderr, "  program:");
                for (uint32_t w : words) std::fprintf(stderr, " %08X", w);
                std::fprintf(stderr, "\n");
                for (int g = 0; g < 32; ++g)
                    if (want.gpr[g] != got.gpr[g])
                        std::fprintf(stderr, "  r%d: want %08X got %08X (init %08X)\n", g, want.gpr[g], got.gpr[g], init.gpr[g]);
                std::fputs(JitTestCr::Diff(want, got, init).c_str(), stderr);
                if (want.xer != got.xer) std::fprintf(stderr, "  xer: want %08X got %08X\n", want.xer, got.xer);
                if (want.ctr != got.ctr) std::fprintf(stderr, "  ctr: want %08X got %08X\n", want.ctr, got.ctr);
                if (want.lr != got.lr) std::fprintf(stderr, "  lr: want %08X got %08X\n", want.lr, got.lr);
            }
        }
    }
    return delta;
}

#define CHECK(cond, ...)                                                                                      \
    do {                                                                                                      \
        if (!(cond)) {                                                                                        \
            if (++g_failures <= 20) {                                                                         \
                std::fprintf(stderr, "jit quality: check failed: %s: ", #cond);                               \
                std::fprintf(stderr, __VA_ARGS__);                                                            \
                std::fprintf(stderr, "\n");                                                                   \
            }                                                                                                 \
        }                                                                                                     \
    } while (0)

// B left lr other than the return address (mtlr, a call): it is put back before the reader.
bool RestoresLr(const OpB& b) { return b.name == "mtlr" || b.name.rfind("bl ", 0) == 0 || b.name.rfind("bctrl", 0) == 0; }

// A; B; R; K; blr with the tail at +kTailOffset.
std::vector<uint32_t> Compose(const Writer& a, const OpB& b, const Reader& r, uint32_t crf, uint32_t other, uint32_t slotBase,
                               bool withKill) {
    std::vector<uint32_t> w;
    w.push_back(a.word);
    for (uint32_t x : b.build(slotBase, slotBase + 4 * static_cast<uint32_t>(w.size()))) w.push_back(x);
    if (RestoresLr(b)) w.push_back(Mtlr(28));
    if (r.padToBlockEnd) {
        while (w.size() < kBlockOps) w.push_back(kNop);
        w.push_back(Mfcr(4));  // the next block reads CR
        w.push_back(Mtlr(28));
        w.push_back(kBlr);
        return w;
    }
    for (uint32_t x : r.build(crf, slotBase, slotBase + 4 * static_cast<uint32_t>(w.size()), other)) w.push_back(x);
    // the field is overwritten after the reader, on the fall-through path
    if (withKill) w.push_back(Cmpwi(crf, 5, 9));
    w.push_back(Mtlr(28));
    w.push_back(kBlr);
    return w;
}

void SweepWriterOpReader() {
    const uint32_t crf = 3, other = 5;
    const auto readers = MakeReaders();
    uint64_t counterChecks = 0;
    for (const Writer& a : MakeWriters(crf)) {
        const auto ops = MakeOps(a.field, a.field == 0 ? other : 6);
        for (const OpB& b : ops) {
            for (const Reader& r : readers) {
                const Slot probe{kCodeBase + ((g_slot) % kSlots) * kSlotBytes};
                const auto words = Compose(a, b, r, a.field, a.field == 0 ? other : 6, probe.base, true);
                RunProgram(a.name + " ; " + b.name + " ; " + r.name, words, 6);
            }
            // The counters: A; B; K(field); blr. A is skipped exactly when B neither reads CR nor leaves.
            if (g_checkCounters && !b.leaves) {
                const Slot probe{kCodeBase + ((g_slot) % kSlots) * kSlotBytes};
                std::vector<uint32_t> w{a.word};
                for (uint32_t x : b.build(probe.base, probe.base + 4)) w.push_back(x);
                if (RestoresLr(b)) w.push_back(Mtlr(28));
                w.push_back(Cmpwi(a.field, 5, 9));
                w.push_back(Mtlr(28));
                w.push_back(kBlr);
                const JitX64CrStats d = RunProgram(a.name + " ; " + b.name + " ; kill", w, 2);
                // calls and jumps away leave the first block: the compiled block holds only part of the program, so only
                // the straight-line ops are judged here
                const bool straight = b.name.find("bc ") == std::string::npos && b.name.find("bl ") == std::string::npos &&
                                      b.name.find("bctrl") == std::string::npos && b.name.find("bclr") == std::string::npos &&
                                      b.name.find("b +8") == std::string::npos;
                if (straight) {
                    ++counterChecks;
                    if (b.pass)
                        CHECK(d.writesSkipped >= 1, "%s ; %s ; kill: A not skipped (%llu)", a.name.c_str(), b.name.c_str(),
                              static_cast<unsigned long long>(d.writesSkipped));
                    else
                        CHECK(d.writesSkipped == 0, "%s ; %s ; kill: a write was skipped across a barrier (%llu)", a.name.c_str(),
                              b.name.c_str(), static_cast<unsigned long long>(d.writesSkipped));
                }
            }
        }
    }
    std::printf("jit quality: writer x op x reader sweep done (%llu counter checks)\n", static_cast<unsigned long long>(counterChecks));
}

// A; A; A; reader chains (same and mixed fields).
void Chains() {
    const auto readers = MakeReaders();
    const uint32_t f = 3, g = 5;
    const std::vector<std::vector<uint32_t>> chains = {
        {Cmpwi(f, 3, 1), Cmplwi(f, 4, 2), Cmpw(f, 5, 6)},
        {Cmpwi(f, 3, 1), Cmpwi(g, 3, 1), Cmplw(f, 5, 6)},
        {D(28, 3, 6, 1), Cmpwi(0, 4, 2), X(3, 6, 5, 444, 1)},
        {Cmpwi(0, 3, 1), D(28, 3, 6, 0xFF), Cmplwi(f, 4, 2)},
        {Cmpwi(f, 3, 1), D(14, 6, 5, 3), Cmpwi(f, 5, 5), D(24, 6, 5, 1), Cmplwi(f, 5, 5)},
        {Fcmpu(f, 1, 2), Fcmpu(f, 2, 1), Cmpwi(f, 3, 0)},
        {Cmpwi(f, 3, 1), Mfcr(4), Cmpwi(f, 3, 2), Cmpwi(f, 3, 3)},
        {Cmpwi(f, 3, 1), Bc(12, 4 * f + 2, static_cast<int32_t>(kTailOffset - 4)), Cmpwi(f, 3, 3), Cmpwi(f, 3, 4)},
    };
    for (size_t c = 0; c < chains.size(); ++c) {
        for (const Reader& r : readers) {
            if (r.padToBlockEnd) continue;
            for (int kill = 0; kill < 2; ++kill) {
                std::vector<uint32_t> w = chains[c];
                const Slot probe{kCodeBase + (g_slot % kSlots) * kSlotBytes};
                for (uint32_t x : r.build(f, probe.base, probe.base + 4 * static_cast<uint32_t>(w.size()), g)) w.push_back(x);
                if (kill) w.push_back(Cmpwi(f, 5, 9));
                w.push_back(Mtlr(28));
                w.push_back(kBlr);
                char name[96];
                std::snprintf(name, sizeof name, "chain %zu ; %s%s", c, r.name.c_str(), kill ? " ; kill" : "");
                RunProgram(name, w, 12);
            }
        }
    }
}

// Compare and bc pairs: every BO shape and BI bit, the fall-through path reading the field, overwriting it, or leaving it.
void FusedPairs() {
    const uint32_t f = 3, g = 6;
    struct Cmp {
        const char* name;
        uint32_t word;
        uint32_t field;
    };
    const Cmp cmps[] = {{"cmpwi", Cmpwi(f, 3, 5), f}, {"cmplwi", Cmplwi(f, 3, 0x8001), f}, {"cmpw", Cmpw(f, 3, 4), f},
                        {"cmplw", Cmplw(f, 3, 4), f}, {"rlwinm.", M(21, 3, 6, 3, 0, 28, 1), 0}, {"cmpwi cr0", Cmpwi(0, 3, 0), 0}};
    const uint32_t bos[] = {12, 4, 16, 18, 8, 10, 20, 24, 26, 28};  // t, f, bdnz, bdz, dnzt, dnzf (with ctr), always, ...
    uint64_t pairs = 0, fusedFired = 0, liveFired = 0;
    for (const Cmp& c : cmps) {
        for (uint32_t bo : bos) {
            for (uint32_t biField : {c.field, g}) {
                for (uint32_t bit = 0; bit < 4; ++bit) {
                    const uint32_t bi = 4 * biField + bit;
                    // 0: fall-through reads (mfcr), 1: fall-through overwrites the field, 2: fall-through overwrites another field
                    for (int fall = 0; fall < 4; ++fall) {
                        std::vector<uint32_t> w;
                        w.push_back(c.word);
                        w.push_back(Bc(bo, bi, static_cast<int32_t>(kTailOffset - 4)));  // taken: the tail reads CR (mfcr)
                        if (fall == 0) w.push_back(Mfcr(4));
                        else if (fall == 1) w.push_back(Cmpwi(c.field, 5, 9));
                        else if (fall == 2) w.push_back(Cmpwi(c.field == 0 ? 2 : 0, 5, 9));
                        else { w.push_back(Cmpwi(c.field, 5, 9)); w.push_back(Mfcr(4)); w.push_back(Cmplwi(c.field, 5, 2)); }
                        w.push_back(Mtlr(28));
                        w.push_back(kBlr);
                        char name[96];
                        std::snprintf(name, sizeof name, "pair %s bo=%u bi=%u fall=%d", c.name, bo, bi, fall);
                        const JitX64CrStats d = RunProgram(name, w, 12);
                        ++pairs;
                        if (g_checkCounters && fall == 1) {
                            // the field is overwritten on the fall-through: the pair writes it on the taken path only, except
                            // when its bc tests the field's own SO bit (read from the field in memory)
                            const bool readsSo = (bo & 0x10u) == 0 && biField == c.field && bit == 3;
                            if (!readsSo) CHECK(d.fusedFlagsOnly >= 1, "pair %s: fused flags-only path not taken", name);
                            if (d.fusedFlagsOnly >= 1) ++fusedFired;
                        }
                        if (g_checkCounters && fall == 0) CHECK(d.fusedFlagsOnly == 0, "pair %s: fall-through reads the field, yet flags-only", name);
                        if (g_checkCounters && g_checkLive) {
                            // the field is live (the fall-through reads it, or leaves with it visible): a bc that tests one of its
                            // lt, gt, eq bits branches on the compare's flags and writes the field on both paths (fusedLive);
                            // a dead field takes the flags-only path instead, and a bc on its SO bit or another field does neither
                            const bool live = fall == 0 || fall == 2;
                            const bool onFlags = (bo & 0x10u) == 0 && biField == c.field && bit != 3;
                            CHECK(d.fusedLive == (live && onFlags ? 1u : 0u), "pair %s: fused-live counter %llu, expected %u", name,
                                  static_cast<unsigned long long>(d.fusedLive), live && onFlags ? 1u : 0u);
                            if (live && onFlags) ++liveFired;
                        }
                    }
                }
            }
        }
    }
    std::printf("jit quality: %llu compare+bc pairs (the flags-only path in %llu, the live-field flags path in %llu)\n",
                static_cast<unsigned long long>(pairs), static_cast<unsigned long long>(fusedFired),
                static_cast<unsigned long long>(liveFired));
    if (g_checkCounters && g_checkLive && liveFired == 0) CHECK(false, "no fused pair took the live-field flags path");
}

// Directed cases with exact counter expectations.
void Counters() {
    if (!g_checkCounters) return;
    const uint32_t f = 3;
    const Slot s0{kCodeBase + (g_slot % kSlots) * kSlotBytes};
    auto run = [&](const char* name, std::vector<uint32_t> w) {
        w.push_back(Mtlr(28));
        w.push_back(kBlr);
        return RunProgram(name, w, 4);
    };
    (void)s0;
    {
        const JitX64CrStats d = run("two compares of one field", {Cmpwi(f, 3, 1), Cmpwi(f, 4, 2)});
        CHECK(d.writesSkipped == 1 && d.compactWrites == 1, "two compares of one field: skipped %llu, written %llu",
              static_cast<unsigned long long>(d.writesSkipped), static_cast<unsigned long long>(d.compactWrites));
    }
    {
        const JitX64CrStats d = run("compare, mfcr, compare", {Cmpwi(f, 3, 1), Mfcr(4), Cmpwi(f, 4, 2)});
        CHECK(d.writesSkipped == 0 && d.compactWrites == 2, "compare, mfcr, compare: skipped %llu, written %llu",
              static_cast<unsigned long long>(d.writesSkipped), static_cast<unsigned long long>(d.compactWrites));
    }
    {
        const JitX64CrStats d = run("compares of different fields", {Cmpwi(f, 3, 1), Cmpwi(f + 1, 4, 2)});
        CHECK(d.writesSkipped == 0, "different fields skipped %llu", static_cast<unsigned long long>(d.writesSkipped));
    }
    {
        const JitX64CrStats d = run("record form then compare of cr0", {D(28, 3, 6, 0xFF), Cmpwi(0, 4, 2)});
        CHECK(d.writesSkipped == 1, "andi. then cmpwi cr0: skipped %llu", static_cast<unsigned long long>(d.writesSkipped));
    }
    {
        const JitX64CrStats d = run("compare across a store", {Cmpwi(f, 3, 1), D(36, 5, 8, 0x20), Cmpwi(f, 4, 2)});
        CHECK(d.writesSkipped == 0, "a store is a barrier: skipped %llu", static_cast<unsigned long long>(d.writesSkipped));
    }
    {
        const JitX64CrStats d = run("compare across a load", {Cmpwi(f, 3, 1), D(32, 6, 8, 0x10), Cmpwi(f, 4, 2)});
        CHECK(d.writesSkipped == 1, "a load is not a barrier: skipped %llu", static_cast<unsigned long long>(d.writesSkipped));
    }
    {
        const JitX64CrStats d =
            run("fused pair then compare", {Cmpwi(f, 3, 1), Bc(12, 4 * f + 2, static_cast<int32_t>(kTailOffset - 4)), Cmpwi(f, 4, 2)});
        CHECK(d.fusedFlagsOnly == 1 && d.writesSkipped == 0, "fused pair, field dead: flags-only %llu, skipped %llu",
              static_cast<unsigned long long>(d.fusedFlagsOnly), static_cast<unsigned long long>(d.writesSkipped));
    }
    {
        const JitX64CrStats d = run("fused pair then mfcr", {Cmpwi(f, 3, 1), Bc(12, 4 * f + 2, static_cast<int32_t>(kTailOffset - 4)), Mfcr(4)});
        CHECK(d.fusedFlagsOnly == 0 && d.compactWrites >= 1, "fused pair, field read: flags-only %llu",
              static_cast<unsigned long long>(d.fusedFlagsOnly));
    }
    {
        const JitX64CrStats d = run("fused pair testing the field's SO bit",
                                    {Cmpwi(f, 3, 1), Bc(12, 4 * f + 3, static_cast<int32_t>(kTailOffset - 4)), Cmpwi(f, 4, 2)});
        CHECK(d.fusedFlagsOnly == 0, "SO test needs the field in memory: flags-only %llu", static_cast<unsigned long long>(d.fusedFlagsOnly));
    }
    {
        const JitX64CrStats d = run("A ; A ; A", {Cmpwi(f, 3, 1), Cmplwi(f, 4, 2), Cmpw(f, 5, 6)});
        CHECK(d.writesSkipped == 2 && d.compactWrites == 1, "chain: skipped %llu, written %llu", static_cast<unsigned long long>(d.writesSkipped),
              static_cast<unsigned long long>(d.compactWrites));
    }
}

// --- the block entry's validity test ----------------------------------------------------------------------------------------
// A block's `checked` entry tests the epoch and the dirty marks under its code with the narrowest load that covers them (a
// byte, word, dword, a masked dword, or the 8-byte window). Block B is made of segments (a b to untranslated code is followed,
// so one block holds several runs of words) laid out so that its marks are 1, 2, 3, 4 and 5 or more contiguous bytes, have
// holes, and need more than two windows. A bc always taken leaves block A for B, and after two runs the exit is a link, so B is
// entered at `checked`. Then every word of B in turn is rewritten (a guest store marks it dirty): the next run must run the new
// word, as the classic interpreter does, and the old compiled block must not.
struct Layout {
    const char* name;
    std::vector<std::pair<uint32_t, uint32_t>> segments;  // start offset from the layout's base, words
};

void EntryCheck() {
    const std::vector<Layout> layouts = {
        {"one mark", {{0x00, 10}}},
        {"two marks", {{0x30, 12}}},
        {"three marks", {{0x38, 28}}},
        {"four marks (two segments)", {{0x30, 16}, {0xA0, 12}}},
        {"five marks and more, contiguous", {{0x00, 20}, {0x50, 10}}},
        {"a hole between marks", {{0x00, 8}, {0x100, 8}}},
        {"two marks, aligned start", {{0x00, 24}}},
        {"four contiguous marks, aligned start", {{0x00, 9}, {0x40, 8}, {0x80, 7}, {0xC0, 6}}},
        {"marks at both ends of a window", {{0x08, 8}, {0x1C0, 10}}},
        {"a mark in the last byte of a window", {{0x1C0, 8}, {0x400, 6}}},
        {"three windows", {{0x00, 6}, {0x400, 6}, {0x800, 6}}},
        {"four windows", {{0x00, 5}, {0x400, 5}, {0x800, 5}, {0xC00, 5}}},
        {"six segments", {{0x00, 3}, {0x300, 3}, {0x600, 3}, {0x900, 3}, {0xC00, 3}, {0xF00, 3}}},
        {"a segment spanning a page", {{0xFE0, 14}}},
        {"a hole inside a dword (marks 0 and 2)", {{0x00, 6}, {0x80, 6}}},
        {"five contiguous marks, one segment each", {{0x00, 4}, {0x40, 4}, {0x80, 4}, {0xC0, 4}, {0x100, 4}}},
        {"marks 0 1 and 3", {{0x10, 5}, {0x50, 5}, {0xD0, 5}}},
    };
    uint32_t region = 0x80120000u;
    uint64_t rewrites = 0;
    for (const Layout& l : layouts) {
        const uint32_t base = region;
        region += 0x2000;
        const uint32_t a = base - 0x100;  // block A
        const uint32_t n = static_cast<uint32_t>(l.segments.size());
        std::vector<uint32_t> bWords;  // addresses of the addi words of B
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t start = base + l.segments[i].first, words = l.segments[i].second;
            for (uint32_t w = 0; w + 1 < words; ++w) {
                Memory::Write32(start + 4 * w, D(14, 3, 3, 1));  // addi r3,r3,1
                bWords.push_back(start + 4 * w);
            }
            const uint32_t last = start + 4 * (words - 1);
            if (i + 1 < n) Memory::Write32(last, Bd(static_cast<int32_t>(base + l.segments[i + 1].first - last), false));
            else Memory::Write32(last, kBlr);
        }
        Memory::Write32(a, D(14, 4, 4, 1));  // addi r4,r4,1
        Memory::Write32(a + 4, Bc(20, 0, static_cast<int32_t>(base + l.segments[0].first - (a + 4))));  // bc always: leaves for B
        const auto runAll = [&](const char* what) {
            CpuContext init{};
            init.lr = kReturn;
            init.gpr[1] = 0x80180000u;
            const CpuContext want = RunOne(PpcInterpreter::Engine::Classic, a, init, 1);
            const CpuContext got = RunOne(PpcInterpreter::Engine::Jit, a, init, 1);
            ++g_runs;
            if (std::memcmp(&want, &got, sizeof want) != 0 && ++g_failures <= 20)
                std::fprintf(stderr, "jit quality: entry check, layout '%s', %s: r3 want %u got %u\n", l.name, what, want.gpr[3], got.gpr[3]);
            return got.gpr[3];
        };
        // Marks stay set until something clears them (the dispatcher clears those under a block it revalidates), so a mark left
        // by an earlier store could hide a test of the wrong byte: each store below starts from clear marks around the layout.
        const auto clearMarks = [&] {
            const uint32_t from = ((a & 0x3FFFFFFFu) >> 6) - 4, to = (((base + 0x1100) & 0x3FFFFFFFu) >> 6) + 4;
            for (uint32_t m = from; m <= to; ++m) BrawlDirtyMemory::leanBlocks[m] = 0;
        };
        for (int i = 0; i < 3; ++i) runAll("warm-up");  // compiled, A linked to B
        const uint32_t clean = runAll("unchanged");
        CHECK(clean == bWords.size(), "layout '%s': r3 %u, expected %zu", l.name, clean, bWords.size());
        for (uint32_t addr : bWords) {
            const uint64_t compiledBefore = PpcInterpreter::detail::JitX64GetStats().compiled;
            clearMarks();
            Memory::Write32(addr, D(14, 3, 3, 2));  // addi r3,r3,2
            const uint32_t r3 = runAll("a word rewritten");
            CHECK(r3 == bWords.size() + 1, "layout '%s': word %08X rewritten, r3 = %u, expected %zu", l.name, addr, r3, bWords.size() + 1);
            CHECK(PpcInterpreter::detail::JitX64GetStats().compiled > compiledBefore, "layout '%s': word %08X rewritten, no recompile", l.name, addr);
            runAll("again");
            Memory::Write32(addr, D(14, 3, 3, 1));  // back
            CHECK(runAll("restored") == bWords.size(), "layout '%s': word %08X restored", l.name, addr);
            Memory::Write32(addr, D(14, 3, 3, 1));  // the same word again: marks set, nothing changed
            CHECK(runAll("same word stored") == bWords.size(), "layout '%s': word %08X stored unchanged", l.name, addr);
            clearMarks();
            if ((addr & 63u) == 0) {
                // the word starts a mark: a 4-byte store at addr - 2 (the guest's unaligned store) marks the mark before it
                // and this one, and changes the word's first two bytes (addi r3,r3,1 becomes addi r4,r3,1)
                const uint32_t prevLow = Memory::Read32(addr - 4) & 0xFFFFu;
                MemoryInline::FlatWrite32(addr - 2, (prevLow << 16) | 0x3883u);  // one store: the marks of addr - 2 and the next block
                const uint32_t r3b = runAll("a straddling store");
                CHECK(r3b == bWords.size() - 1 || r3b == bWords.size() + 0, "layout '%s': straddling store at %08X, r3 = %u", l.name, addr - 2, r3b);
                CHECK(PpcInterpreter::detail::JitX64GetStats().compiled > compiledBefore,
                      "layout '%s': straddling store at %08X, no recompile", l.name, addr - 2);
                MemoryInline::FlatWrite32(addr - 2, (prevLow << 16) | 0x3863u);
                CHECK(runAll("straddle undone") == bWords.size(), "layout '%s': straddle at %08X undone", l.name, addr - 2);
            }
            ++rewrites;
        }
    }
    std::printf("jit quality: block entry check, %zu layouts, %llu word rewrites\n", layouts.size(), static_cast<unsigned long long>(rewrites));
}

}  // namespace

int main() {
    Memory::Init(0x200000);
    TranslatedFunctionRegistry::Finalize();
    if (!PpcInterpreter::JitAvailable()) {
        std::printf("jit quality: no JIT backend in this build; nothing to test\n");
        return 0;
    }
    g_checkCounters = std::getenv("BRAWL_JIT_FALLBACK") == nullptr && std::getenv("BRAWL_JIT_FP_HELPERS") == nullptr &&
                      std::getenv("BRAWL_JIT_NO_CR_LIVENESS") == nullptr;
    g_checkLive = std::getenv("BRAWL_JIT_NO_FUSED_FLAGS") == nullptr;
    PpcInterpreter::SetJitThreshold(0);
    EntryCheck();
    Counters();
    Chains();
    FusedPairs();
    SweepWriterOpReader();
    PpcInterpreter::ClearEngineOverride();
    const JitX64CrStats s = JitX64GetCrStats();
    std::printf("jit quality: %llu programs, %llu runs, %u failures; blocks analysed %llu, CR writes skipped %llu, "
                "fused flags-only %llu, compact writes %llu\n",
                static_cast<unsigned long long>(g_programs), static_cast<unsigned long long>(g_runs), g_failures,
                static_cast<unsigned long long>(s.blocksAnalysed), static_cast<unsigned long long>(s.writesSkipped),
                static_cast<unsigned long long>(s.fusedFlagsOnly), static_cast<unsigned long long>(s.compactWrites));
    return g_failures == 0 ? 0 : 1;
}
