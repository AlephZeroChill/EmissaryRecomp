#pragma once

#include <cstdint>

// The float ABI translated callers assume of a translated function, as the translator inferred it (translate-recursive
// writes generated/guest_float_abi.txt after every emission; the shard emitter turns it into this table, sorted by
// address, only the functions the build compiles that have a fact). A translated caller holding one of the function's
// scalar float arguments in paired form converts it before the call (CxxLinearCodeGenerator
// EmitNormalizeGuestCallFloatArguments), and converts f1 after it when the function returns a paired scalar
// (EmitNormalizeGuestCallFloatReturn). The interpreter's calls into translated code (ppc_interpreter.cpp) do the same
// from this table.
struct MkwGuestFloatAbi {
    uint32_t address;
    uint16_t scalarFloatArgMask;       // bit n: fn (f1-f13) is read as a scalar float
    uint16_t returnsPairedScalarFloat;  // 1: f1 comes back in paired form
};

extern "C" const uint32_t g_mkwGuestFloatAbiCount;
extern "C" const MkwGuestFloatAbi g_mkwGuestFloatAbi[];
