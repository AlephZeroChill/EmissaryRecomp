// Native code for the integer arithmetic, logic, rotates, compares and spr moves (Li ... Mtspr) of the x86-64 backend,
// written against the emitter API in jit_x64.h. Every function here reproduces the matching case of RunOp
// (ppc_interpreter.cpp) bit for bit; the Decode comments there say what d, a, b and imm hold per kind.
//
// Conventions: eax is the result register, ecx and edx are scratch, and no
// value lives in a host register across ops. Memory operands are used directly where that is shorter. A compare makes its CR
// field's value as the interpreter does (PpcCrvCompare: the 64-bit difference of the extended operands, BlockEmitter::CrStore
// adds XER[SO]) and stores it with one move; XER[CA] is updated without branches.
#include "jit_x64.h"
#include "jit_x64_generic.h"

#include <cstddef>



namespace PpcInterpreter::detail::x64 {

namespace {

using namespace Xbyak::util;

constexpr uint32_t kXerCa = 0x20000000u;

// The same register's address as a byte or word operand (the guest word is little-endian in the context, so the low byte
// and halfword sit at the register's own address).
Xbyak::Address ByteOf(BlockEmitter& e, const Xbyak::Address& m) { return e.a.byte[m.getRegExp()]; }
Xbyak::Address WordOf(BlockEmitter& e, const Xbyak::Address& m) { return e.a.word[m.getRegExp()]; }
Xbyak::Address CtxDword(BlockEmitter& e, size_t offset) { return e.a.dword[kCtxBase + CtxDisp(offset)]; }

// --- small emit helpers ----------------------------------------------------------------------------------------------

// dst = src, through eax (nothing when they are the same register).
void EmitMove(BlockEmitter& e, uint32_t dst, uint32_t src) {
    if (dst == src) return;
    e.a.mov(eax, e.Gpr(src));
    e.a.mov(e.Gpr(dst), eax);
}

// The words of the guest special registers the sprs mfspr and mtspr reach (the table of ReadSpr and WriteSpr), or
// false for a spr without one.
bool SprWord(BlockEmitter& e, uint32_t spr, Xbyak::Address& out) {
    switch (spr) {
    case 1: out = e.Xer(); return true;
    case 8: out = e.Lr(); return true;
    case 9: out = e.Ctr(); return true;
    case 26: out = CtxDword(e, offsetof(CpuContext, srr0)); return true;
    case 27: out = CtxDword(e, offsetof(CpuContext, srr1)); return true;
    case 920: out = CtxDword(e, offsetof(CpuContext, hid2)); return true;
    case 1008: out = CtxDword(e, offsetof(CpuContext, hid0)); return true;
    case 1009: out = CtxDword(e, offsetof(CpuContext, hid1)); return true;
    default: break;
    }
    if (spr >= 912 && spr <= 919) { out = e.Gqr(spr - 912); return true; }
    return false;
}

// --- CR fields -------------------------------------------------------------------------------------------------------

// CR0 from the value in eax (SetCr0: a signed compare against zero, whose difference is the value itself).
void EmitSetCr0FromEax(BlockEmitter& e) {
    e.a.movsxd(rax, eax);
    e.CrStore(0);
}

// A compare or record form whose CR field no later op of the block reads (BlockEmitter::CrDead) writes nothing: a compare
// is then no code at all, and a record form is its plain form.
bool CrDeadNote(BlockEmitter& e, uint32_t field) {
    if (!e.CrDead(e.index, field)) return false;
    BlockEmitter::NoteCr(BlockEmitter::CrEvent::WriteSkipped);
    return true;
}

// --- logical operations ------------------------------------------------------------------------------------------------

enum class Logic { And, Or, Xor };

void LogicRegMem(BlockEmitter& e, Logic op, const Xbyak::Reg32& r, const Xbyak::Address& m) {
    switch (op) {
    case Logic::And: e.a.and_(r, m); break;
    case Logic::Or: e.a.or_(r, m); break;
    case Logic::Xor: e.a.xor_(r, m); break;
    }
}
void LogicMemReg(BlockEmitter& e, Logic op, const Xbyak::Address& m, const Xbyak::Reg32& r) {
    switch (op) {
    case Logic::And: e.a.and_(m, r); break;
    case Logic::Or: e.a.or_(m, r); break;
    case Logic::Xor: e.a.xor_(m, r); break;
    }
}
void LogicRegImm(BlockEmitter& e, Logic op, const Xbyak::Reg32& r, uint32_t imm) {
    switch (op) {
    case Logic::And: e.a.and_(r, imm); break;
    case Logic::Or: e.a.or_(r, imm); break;
    case Logic::Xor: e.a.xor_(r, imm); break;
    }
}
void LogicMemImm(BlockEmitter& e, Logic op, const Xbyak::Address& m, uint32_t imm) {
    switch (op) {
    case Logic::And: e.a.and_(m, imm); break;
    case Logic::Or: e.a.or_(m, imm); break;
    case Logic::Xor: e.a.xor_(m, imm); break;
    }
}

// r[dst] = r[src] op imm (ori, andi., xori and their shifted forms). The Rc forms compute in eax and set CR0.
void EmitLogicImm(BlockEmitter& e, Logic op, uint32_t dst, uint32_t src, uint32_t imm, bool rc) {
    if (!rc) {
        if (op != Logic::And && imm == 0) { EmitMove(e, dst, src); return; }
        if (dst == src) { LogicMemImm(e, op, e.Gpr(dst), imm); return; }
    }
    e.a.mov(eax, e.Gpr(src));
    LogicRegImm(e, op, eax, imm);
    e.a.mov(e.Gpr(dst), eax);
    if (rc) EmitSetCr0FromEax(e);
}

// r[dst] = r[s1] op r[s2], with every aliasing of the three registers; Rc: also CR0.
void EmitLogicReg(BlockEmitter& e, Logic op, uint32_t dst, uint32_t s1, uint32_t s2, bool rc) {
    if (!rc) {
        if (s1 == s2) {
            if (op == Logic::Xor) e.a.mov(e.Gpr(dst), 0u);
            else EmitMove(e, dst, s1);
            return;
        }
        if (dst == s1 || dst == s2) {
            e.a.mov(eax, e.Gpr(dst == s1 ? s2 : s1));
            LogicMemReg(e, op, e.Gpr(dst), eax);
            return;
        }
    }
    e.a.mov(eax, e.Gpr(s1));
    LogicRegMem(e, op, eax, e.Gpr(s2));
    e.a.mov(e.Gpr(dst), eax);
    if (rc) EmitSetCr0FromEax(e);
}

// --- rotates -------------------------------------------------------------------------------------------------------------

// The result of rlwinm (rotate left by sh, and with mask) of r[src], left in eax.
void EmitRlwinmToEax(BlockEmitter& e, uint32_t src, uint32_t sh, uint32_t mask) {
    auto& c = e.a;
    if (mask == 0) { c.xor_(eax, eax); return; }
    if (sh == 0 && mask == 0xFFu) { c.movzx(eax, ByteOf(e, e.Gpr(src))); return; }
    if (sh == 0 && mask == 0xFFFFu) { c.movzx(eax, WordOf(e, e.Gpr(src))); return; }
    c.mov(eax, e.Gpr(src));
    if (sh != 0 && mask == (0xFFFFFFFFu << sh)) { c.shl(eax, static_cast<uint8_t>(sh)); return; }  // slwi
    if (sh != 0 && mask == (0xFFFFFFFFu >> (32u - sh))) { c.shr(eax, static_cast<uint8_t>(32u - sh)); return; }  // srwi
    if (sh != 0) c.rol(eax, static_cast<uint8_t>(sh));
    if (mask != 0xFFFFFFFFu) c.and_(eax, mask);
}

void EmitRlwinm(BlockEmitter& e, const Op& o, bool rc) {
    const uint32_t src = o.d, dst = o.a, sh = o.b, mask = o.imm;
    if (!rc) {
        if (mask == 0) { e.a.mov(e.Gpr(dst), 0u); return; }
        if (src == dst) {  // in place on the register's memory
            if (mask == 0xFFFFFFFFu) { if (sh != 0) e.a.rol(e.Gpr(dst), static_cast<uint8_t>(sh)); return; }
            if (sh == 0) { e.a.and_(e.Gpr(dst), mask); return; }
            if (mask == (0xFFFFFFFFu << sh)) { e.a.shl(e.Gpr(dst), static_cast<uint8_t>(sh)); return; }
            if (mask == (0xFFFFFFFFu >> (32u - sh))) { e.a.shr(e.Gpr(dst), static_cast<uint8_t>(32u - sh)); return; }
        } else if (sh == 0 && mask == 0xFFFFFFFFu) {
            EmitMove(e, dst, src);
            return;
        }
    }
    EmitRlwinmToEax(e, src, sh, mask);
    e.a.mov(e.Gpr(dst), eax);
    if (rc) EmitSetCr0FromEax(e);
}

// r[a] = (rotl(r[d], sh) & mask) | (r[a] & ~mask)
void EmitRlwimi(BlockEmitter& e, const Op& o) {
    const uint32_t src = o.d, dst = o.a, sh = o.b, mask = o.imm;
    if (mask == 0) return;
    if (mask == 0xFFFFFFFFu) {
        if (sh == 0) { EmitMove(e, dst, src); return; }
        e.a.mov(eax, e.Gpr(src));
        e.a.rol(eax, static_cast<uint8_t>(sh));
        e.a.mov(e.Gpr(dst), eax);
        return;
    }
    // eax is read before the destination changes, so src == dst is fine.
    if (sh == 0 && mask == 0xFFu) {
        e.a.movzx(eax, ByteOf(e, e.Gpr(src)));  // insert the low byte
        e.a.and_(e.Gpr(dst), ~mask);
        e.a.or_(e.Gpr(dst), eax);
        return;
    }
    e.a.mov(eax, e.Gpr(src));
    if (sh != 0) e.a.rol(eax, static_cast<uint8_t>(sh));
    e.a.and_(eax, mask);
    e.a.and_(e.Gpr(dst), ~mask);
    e.a.or_(e.Gpr(dst), eax);
}

// r[a] = rotl(r[d], r[b] & 31) & mask
void EmitRlwnm(BlockEmitter& e, const Op& o, bool rc) {
    auto& c = e.a;
    c.mov(ecx, e.Gpr(o.b));
    c.mov(eax, e.Gpr(o.d));
    c.rol(eax, cl);  // the count is taken mod 32 by the hardware, as & 31u
    if (o.imm != 0xFFFFFFFFu) c.and_(eax, o.imm);
    c.mov(e.Gpr(o.a), eax);
    if (rc) EmitSetCr0FromEax(e);
}

// --- arithmetic ------------------------------------------------------------------------------------------------------------

// r[d] = r[a] + imm
void EmitAddImm(BlockEmitter& e, uint32_t d, uint32_t a, uint32_t imm) {
    if (imm == 0) { EmitMove(e, d, a); return; }
    if (d == a) { e.a.add(e.Gpr(d), imm); return; }
    e.a.mov(eax, e.Gpr(a));
    e.a.add(eax, imm);
    e.a.mov(e.Gpr(d), eax);
}

void EmitAdd(BlockEmitter& e, const Op& o) {
    if (o.d == o.a || o.d == o.b) {
        e.a.mov(eax, e.Gpr(o.d == o.a ? o.b : o.a));
        e.a.add(e.Gpr(o.d), eax);
        return;
    }
    e.a.mov(eax, e.Gpr(o.a));
    e.a.add(eax, e.Gpr(o.b));
    e.a.mov(e.Gpr(o.d), eax);
}

// r[d] = r[b] - r[a]
void EmitSubf(BlockEmitter& e, const Op& o) {
    if (o.a == o.b) { e.a.mov(e.Gpr(o.d), 0u); return; }
    if (o.d == o.b) {
        e.a.mov(eax, e.Gpr(o.a));
        e.a.sub(e.Gpr(o.d), eax);
        return;
    }
    e.a.mov(eax, e.Gpr(o.b));
    e.a.sub(eax, e.Gpr(o.a));
    e.a.mov(e.Gpr(o.d), eax);
}

// Sets XER[CA] when ecx is all ones and clears it when ecx is 0 (nothing else in ecx is allowed).
void EmitSetCaFromMask(BlockEmitter& e) {
    e.a.and_(ecx, kXerCa);
    e.a.and_(e.Xer(), ~kXerCa);
    e.a.or_(e.Xer(), ecx);
}

// r[a] = (int32)r[d] >> sh; XER[CA] = r[d] is negative and a 1 bit was shifted out. CA is "sign and any of the low sh
// bits": with x = r & (0x80000000 | (1 << sh) - 1), that is x > 0x80000000.
void EmitSrawi(BlockEmitter& e, const Op& o) {
    auto& c = e.a;
    const uint32_t sh = o.b;
    if (sh == 0) {
        EmitMove(e, o.a, o.d);
        c.and_(e.Xer(), ~kXerCa);
        return;
    }
    const uint32_t keep = 0x80000000u | ((1u << sh) - 1u);
    c.mov(eax, e.Gpr(o.d));
    c.mov(ecx, eax);
    if (keep != 0xFFFFFFFFu) c.and_(ecx, keep);
    c.mov(edx, 0x80000000u);
    c.cmp(edx, ecx);  // CF: 0x80000000 < x
    c.sbb(ecx, ecx);
    c.sar(eax, static_cast<uint8_t>(sh));
    c.mov(e.Gpr(o.a), eax);
    EmitSetCaFromMask(e);
}

// r[d] = imm - r[a]; XER[CA] = no borrow (imm >= r[a] unsigned), as AddCarry(~a, imm, 1).
void EmitSubfic(BlockEmitter& e, const Op& o) {
    auto& c = e.a;
    c.mov(edx, o.imm);
    c.sub(edx, e.Gpr(o.a));  // CF: borrow
    c.mov(e.Gpr(o.d), edx);
    c.cmc();
    c.sbb(ecx, ecx);
    EmitSetCaFromMask(e);
}

// r[a] = r[d] << (r[b] & 63) as 64 bits, low half: counts 32..63 give 0, as PPC_Slw and PPC_Srw.
void EmitShiftReg(BlockEmitter& e, const Op& o, bool left) {
    auto& c = e.a;
    c.mov(ecx, e.Gpr(o.b));
    c.mov(eax, e.Gpr(o.d));  // zero-extends into rax
#if defined(__BMI2__)
    if (left) c.shlx(rax, rax, rcx);
    else c.shrx(rax, rax, rcx);
#else
    if (left) c.shl(rax, cl);
    else c.shr(rax, cl);
#endif
    c.mov(e.Gpr(o.a), eax);
}

void EmitCntlzw(BlockEmitter& e, const Op& o) {
    auto& c = e.a;
#if defined(__LZCNT__)
    c.lzcnt(eax, e.Gpr(o.d));
#else
    // bsr leaves its destination undefined for 0: pick 63 then, and 63 ^ 31 = 32 while 31 - bsr = bsr ^ 31 otherwise.
    c.mov(edx, 63u);
    c.bsr(eax, e.Gpr(o.d));
    c.cmovz(eax, edx);
    c.xor_(eax, 31u);
#endif
    c.mov(e.Gpr(o.a), eax);
}

}  // namespace

uint32_t EmitIntOp(BlockEmitter& e, const Op& o) {
    auto& c = e.a;
    switch (o.kind) {
    case K::Li:
    case K::Lis: c.mov(e.Gpr(o.d), o.imm); return 1;
    case K::Addi:
    case K::Addis: EmitAddImm(e, o.d, o.a, o.imm); return 1;
    case K::Mulli:
        c.imul(eax, e.Gpr(o.a), static_cast<int>(o.imm));
        c.mov(e.Gpr(o.d), eax);
        return 1;
    case K::Ori:
    case K::Oris: EmitLogicImm(e, Logic::Or, o.a, o.d, o.imm, false); return 1;
    case K::Xori:
    case K::Xoris: EmitLogicImm(e, Logic::Xor, o.a, o.d, o.imm, false); return 1;
    case K::AndiRc:
    case K::AndisRc: EmitLogicImm(e, Logic::And, o.a, o.d, o.imm, !CrDeadNote(e, 0)); return 1;
    case K::Rlwinm: EmitRlwinm(e, o, false); return 1;
    case K::RlwinmRc: EmitRlwinm(e, o, !CrDeadNote(e, 0)); return 1;
    case K::Rlwimi: EmitRlwimi(e, o); return 1;
    case K::Cmpwi:
        if (CrDeadNote(e, o.d >> 2)) return 1;
        c.movsxd(rax, e.Gpr(o.a));
        e.CrDiffImm(true, o.imm, false);
        e.CrStore(o.d >> 2);
        return 1;
    case K::Cmplwi:
        if (CrDeadNote(e, o.d >> 2)) return 1;
        c.mov(eax, e.Gpr(o.a));
        e.CrDiffImm(false, o.imm, false);
        e.CrStore(o.d >> 2);
        return 1;
    case K::Cmpw:
    case K::Cmplw: {
        if (CrDeadNote(e, o.d >> 2)) return 1;
        const bool isSigned = o.kind == K::Cmpw;
        if (isSigned) c.movsxd(rax, e.Gpr(o.a));
        else c.mov(eax, e.Gpr(o.a));
        e.CrDiffGpr(isSigned, o.b);
        e.CrStore(o.d >> 2);
        return 1;
    }
    case K::Add: EmitAdd(e, o); return 1;
    case K::Subf: EmitSubf(e, o); return 1;
    case K::Neg:
        if (o.d == o.a) { c.neg(e.Gpr(o.d)); return 1; }
        c.mov(eax, e.Gpr(o.a));
        c.neg(eax);
        c.mov(e.Gpr(o.d), eax);
        return 1;
    case K::Mullw:
        c.mov(eax, e.Gpr(o.a));
        c.imul(eax, e.Gpr(o.b));
        c.mov(e.Gpr(o.d), eax);
        return 1;
    case K::Or: EmitLogicReg(e, Logic::Or, o.a, o.d, o.b, false); return 1;
    case K::OrRc: EmitLogicReg(e, Logic::Or, o.a, o.d, o.b, !CrDeadNote(e, 0)); return 1;
    case K::And: EmitLogicReg(e, Logic::And, o.a, o.d, o.b, false); return 1;
    case K::AndRc: EmitLogicReg(e, Logic::And, o.a, o.d, o.b, !CrDeadNote(e, 0)); return 1;
    case K::Xor: EmitLogicReg(e, Logic::Xor, o.a, o.d, o.b, false); return 1;
    case K::Andc:  // r[a] = r[d] & ~r[b]
        if (o.d == o.b) { c.mov(e.Gpr(o.a), 0u); return 1; }
        c.mov(eax, e.Gpr(o.b));
        c.not_(eax);
        c.and_(eax, e.Gpr(o.d));
        c.mov(e.Gpr(o.a), eax);
        return 1;
    case K::Nor:  // r[a] = ~(r[d] | r[b])
        if (o.d == o.b) {
            if (o.a == o.d) { c.not_(e.Gpr(o.a)); return 1; }
            c.mov(eax, e.Gpr(o.d));
        } else {
            c.mov(eax, e.Gpr(o.d));
            c.or_(eax, e.Gpr(o.b));
        }
        c.not_(eax);
        c.mov(e.Gpr(o.a), eax);
        return 1;
    case K::Slw: EmitShiftReg(e, o, true); return 1;
    case K::Srw: EmitShiftReg(e, o, false); return 1;
    case K::Srawi: EmitSrawi(e, o); return 1;
    case K::Extsb:
        c.movsx(eax, ByteOf(e, e.Gpr(o.d)));
        c.mov(e.Gpr(o.a), eax);
        return 1;
    case K::Extsh:
        c.movsx(eax, WordOf(e, e.Gpr(o.d)));
        c.mov(e.Gpr(o.a), eax);
        return 1;
    case K::Cntlzw: EmitCntlzw(e, o); return 1;
    case K::Mflr:
        c.mov(eax, e.Lr());
        c.mov(e.Gpr(o.d), eax);
        return 1;
    case K::Mtlr:
        c.mov(eax, e.Gpr(o.d));
        c.mov(e.Lr(), eax);
        return 1;
    case K::Mfctr:
        c.mov(eax, e.Ctr());
        c.mov(e.Gpr(o.d), eax);
        return 1;
    case K::Mtctr:
        c.mov(eax, e.Gpr(o.d));
        c.mov(e.Ctr(), eax);
        return 1;
    case K::Mfcr:
        e.CrPack();
        c.mov(e.Gpr(o.d), eax);
        return 1;
    case K::Subfic: EmitSubfic(e, o); return 1;
    case K::Rlwnm: EmitRlwnm(e, o, false); return 1;
    case K::RlwnmRc: EmitRlwnm(e, o, !CrDeadNote(e, 0)); return 1;
    case K::Mfspr: {
        Xbyak::Address word = e.Lr();
        if (!SprWord(e, o.imm, word)) return 0;  // an unknown spr logs: the handler's
        c.mov(eax, word);
        c.mov(e.Gpr(o.d), eax);
        return 1;
    }
    case K::Mtspr: {
        Xbyak::Address word = e.Lr();
        if (!SprWord(e, o.imm, word)) return 1;  // WriteSpr ignores the rest (decrementer, performance monitors, BATs)
        c.mov(eax, e.Gpr(o.d));
        c.mov(word, eax);
        return 1;
    }
    default: break;
    }
    return 0;
}

uint32_t EmitInt(BlockEmitter& e, uint32_t index) {
    (void)index;
    return EmitIntOp(e, e.CurrentOp());
}

}  // namespace PpcInterpreter::detail::x64
