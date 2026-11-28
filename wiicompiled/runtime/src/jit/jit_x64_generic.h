// What the groups of the x86-64 backend share beyond jit_x64.h: the instructions Decode leaves as K::Generic (the interpreter's
// Exec runs them), their facts (what the analyses may assume of each), and the entry points that give them native code. Private
// to src/jit.
#pragma once

#include "jit_x64.h"

namespace PpcInterpreter::detail::x64 {

// What an analysis may assume of an instruction word Decode left Generic, whether it gets native code or the interpreter's
// handler runs it (the facts hold for Exec's behaviour, which both reproduce):
//  * cr: CrPass (reads no CR bit, leaves the block never, writes no CR field), CrWriter (overwrites field crField in full, and
//    otherwise as CrPass), CrBarrier (anything else: reads CR, may leave the block, may throw, or is unknown).
//  * form: FormKeep (writes no Frame::form byte), FormScalar (sets form[formReg] to Scalar and no other), FormBarrier (anything
//    else, or unknown).
// Words that are not listed are barriers for both: the safe answer.
struct GenericFacts {
    enum Cr : uint8_t { CrPass, CrWriter, CrBarrier };
    enum Form : uint8_t { FormKeep, FormScalar, FormBarrier };
    Cr cr = CrBarrier;
    Form form = FormBarrier;
    uint8_t crField = 0;
    uint8_t formReg = 0;
};
GenericFacts DescribeGeneric(uint32_t word);

// Native code for the Generic op at `index` (jit_x64_generic.cpp), or 0 with nothing emitted: the core then calls the
// interpreter's handler. BRAWL_JIT_GENERIC=0 makes it always 0.
uint32_t EmitGeneric(BlockEmitter& e, uint32_t index);
bool GenericNativeEnabled();

// BRAWL_JIT_GENERIC_STATS=1: per (primary opcode, extended opcode) counts of the Generic ops compiled and executed, native or
// not, printed at exit. The core emits the execution counter before each Generic op.
bool GenericStatsOn();
uint64_t* GenericRunCounter(uint32_t word);

// The other groups' parts of it.
uint32_t EmitIntOp(BlockEmitter& e, const Op& o);          // jit_x64_int.cpp: one op of the Int group, as EmitInt (not a fused pair)
uint32_t EmitMemGeneric(BlockEmitter& e, uint32_t word);   // jit_x64_mem.cpp: the Generic loads and stores
uint32_t EmitFpGeneric(BlockEmitter& e, uint32_t word);    // jit_x64_fp.cpp: the Generic float ops

// The floating point form analysis (jit_x64_forms.cpp) classifies every kind: the kinds a block may hold are the ops' kinds and
// the Generic words' facts above.

}  // namespace PpcInterpreter::detail::x64
