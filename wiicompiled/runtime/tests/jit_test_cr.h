// The condition register in the JIT tests. A CR field is a 64-bit value (isa/ppc_isa_crv.h) with many values for one nibble: a
// compare stores the difference of its operands. The JIT stores exactly the value the interpreter does, so the tests compare
// the fields bytewise (Same), not by what they read as (PpcSameCpuContext): a field that differs in a single bit fails, and so
// does one that reads as another nibble. The initial states of the tests hold every kind of field value (Randomize), so that
// untouched fields are checked for being left alone and every reader is checked on every kind.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "isa/ppc_isa_context.h"

namespace JitTestCr {

// One field value from three random words:
//  0, 1  the value of a nibble (PpcCrvFromNibble: what mtcrf and the CR logic store);
//  2     the difference of two signed words, 3 of two unsigned words (what a compare stores; XER[SO] clear);
//  4     a compare's difference with SO set through XER[SO] (PpcCrvWithSo's value);
//  5     a difference of operands from the extremes of the 32-bit range (the largest magnitudes a compare stores);
//  6, 7  an arbitrary 64-bit pattern. No instruction stores one, but every reader (bc, mfcr, mcrf, the CR logic) must treat it
//        as the interpreter does, whatever it holds.
inline int64_t FieldValue(uint32_t kind, uint32_t a, uint32_t b) {
    constexpr uint32_t kExtreme[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x7FFFFFFEu};
    switch (kind & 7u) {
    case 0:
    case 1: return PpcCrvFromNibble(a);
    case 2: return PpcCrvCompare(static_cast<int32_t>(a), static_cast<int32_t>(b), 0);
    case 3: return PpcCrvCompare(a, b, 0);
    case 4: return PpcCrvWithSo((a & 1u) != 0 ? PpcCrvCompare(a, b, 0) : PpcCrvCompare(static_cast<int32_t>(a), static_cast<int32_t>(b), 0));
    case 5: {
        const uint32_t x = kExtreme[(a >> 8) & 7u], y = kExtreme[(b >> 8) & 7u];
        return (a & 1u) != 0 ? PpcCrvCompare(x, y, 0) : PpcCrvCompare(static_cast<int32_t>(x), static_cast<int32_t>(y), 0);
    }
    default: return static_cast<int64_t>((static_cast<uint64_t>(a) << 32) | b);
    }
}

// A field value that reads as `nibble`: a compare's difference when one exists (a few draws of two random words), else the
// nibble's table value. For the tests that sweep the nibbles: each is run with both representations.
template <class Next>
inline int64_t FieldReadingAs(uint32_t nibble, Next&& next) {
    for (int tries = 0; tries < 64; ++tries) {
        const uint32_t kind = 2u + (next() & 1u), a = next(), b = next();
        const int64_t v = FieldValue(kind, a, b);
        if (PpcCrvNibble(v) == (nibble & 0xFu)) return v;
    }
    return PpcCrvFromNibble(nibble);
}

// All eight fields: kinds in rotation from a random start, so that every state holds several kinds.
template <class Next>
inline void Randomize(CpuContext& c, Next&& next) {
    const uint32_t first = next();
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t kind = (next() & 1u) != 0 ? first + i : next();
        const uint32_t a = next(), b = next();
        c.crv[i] = FieldValue(kind, a, b);
    }
}

// The packed CR's nibbles as fields, nibble n of `packed` in field n counting from cr0 at the top: the table values, or
// (alternate) compare-derived ones that read the same.
template <class Next>
inline void SetPacked(CpuContext& c, uint32_t packed, bool alternate, Next&& next) {
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t nibble = (packed >> ((7u - i) * 4u)) & 0xFu;
        c.crv[i] = alternate ? FieldReadingAs(nibble, next) : PpcCrvFromNibble(nibble);
    }
}

inline bool Same(const CpuContext& a, const CpuContext& b) { return std::memcmp(a.crv, b.crv, sizeof a.crv) == 0; }

// The fields that differ, as text (empty when none); `init` is the state the run started from.
inline std::string Diff(const CpuContext& want, const CpuContext& got, const CpuContext& init) {
    std::string out;
    for (uint32_t i = 0; i < 8; ++i) {
        if (want.crv[i] == got.crv[i]) continue;
        char line[192];
        std::snprintf(line, sizeof line, "  crv[%u]: want %016llX (reads %X) got %016llX (reads %X) (init %016llX)\n", i,
                      static_cast<unsigned long long>(want.crv[i]), PpcCrvNibble(want.crv[i]),
                      static_cast<unsigned long long>(got.crv[i]), PpcCrvNibble(got.crv[i]),
                      static_cast<unsigned long long>(init.crv[i]));
        out += line;
    }
    return out;
}

}  // namespace JitTestCr
