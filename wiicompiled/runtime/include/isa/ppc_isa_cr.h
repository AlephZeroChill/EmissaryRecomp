#pragma once

#include "ppc_isa_config.h"

#include <cmath>
#include <cstdint>

// MKW_LEAN_FLAGS=1: compares leave the CR field's SO bit clear instead of copying XER[SO]. Translated code never
// sets SO (its only XER writes are CA updates and a few mtxer restores, checked program-wide), so the copy is dead
// work in every compare.
#ifndef MKW_LEAN_FLAGS
#define MKW_LEAN_FLAGS 0
#endif
#define MKW_CR_SO_BIT(xer) (MKW_LEAN_FLAGS ? 0u : (((xer) >> 31) & 1u))

// A compare's new CR word. MKW_OUTLINE_CR_COMPARES (isa/ppc_isa_config.h's MKW_OUTLINE_GUEST_OPS by default): the
// compares translated code materializes are out-of-line calls (src/guest_ops_outlined.cpp, MKW_GUEST_OP_CC); they are
// `const`, so a compare whose CR is never read still compiles away.
template <typename T>
inline uint32_t PpcCrCompareBody(uint32_t cr, uint32_t xer, uint32_t field, T a, T b) noexcept {
    uint32_t value = (a < b ? 0x8u : 0u) | (a > b ? 0x4u : 0u) | (a == b ? 0x2u : 0u) | MKW_CR_SO_BIT(xer);
    const uint32_t shift = (7 - field) * 4;
    return (cr & ~(0xFu << shift)) | (value << shift);
}
inline uint32_t PpcCrCompareFloatBody(uint32_t cr, uint32_t field, double a, double b) noexcept {
    uint32_t value = (std::isnan(a) || std::isnan(b)) ? 0x1u :
        ((a < b ? 0x8u : 0u) | (a > b ? 0x4u : 0u) | (a == b ? 0x2u : 0u));
    const uint32_t shift = (7 - field) * 4;
    return (cr & ~(0xFu << shift)) | (value << shift);
}
#ifndef MKW_OUTLINE_CR_COMPARES
#define MKW_OUTLINE_CR_COMPARES MKW_OUTLINE_GUEST_OPS
#endif
#if MKW_OUTLINE_CR_COMPARES
__attribute__((const, noinline)) MKW_GUEST_OP_CC uint32_t PpcCrCompareSignedOutlined(
    uint32_t cr, uint32_t xer, uint32_t field, int32_t a, int32_t b) noexcept;
__attribute__((const, noinline)) MKW_GUEST_OP_CC uint32_t PpcCrCompareUnsignedOutlined(
    uint32_t cr, uint32_t xer, uint32_t field, uint32_t a, uint32_t b) noexcept;
__attribute__((const, noinline)) MKW_GUEST_OP_CC uint32_t PpcCrCompareFloatOutlined(
    uint32_t cr, uint32_t field, double a, double b) noexcept;
// CR0 (record forms, most compares) and CR0 against zero (record forms, cmpwi/cmplwi rX,0) take no field and no zero
// operand: two argument moves fewer per call site.
__attribute__((const, noinline)) MKW_GUEST_OP_CC uint32_t PpcCr0CompareSignedOutlined(
    uint32_t cr, uint32_t xer, int32_t a, int32_t b) noexcept;
__attribute__((const, noinline)) MKW_GUEST_OP_CC uint32_t PpcCr0CompareSignedZeroOutlined(
    uint32_t cr, uint32_t xer, int32_t a) noexcept;
__attribute__((const, noinline)) MKW_GUEST_OP_CC uint32_t PpcCr0CompareUnsignedOutlined(
    uint32_t cr, uint32_t xer, uint32_t a, uint32_t b) noexcept;
__attribute__((const, noinline)) MKW_GUEST_OP_CC uint32_t PpcCr0CompareUnsignedZeroOutlined(
    uint32_t cr, uint32_t xer, uint32_t a) noexcept;
#endif

__attribute__((always_inline)) inline void SetCRResident(uint32_t& cr, uint32_t xer, int field, int32_t a,
                                                         int32_t b) noexcept {
#if MKW_OUTLINE_CR_COMPARES
    if (__builtin_constant_p(field) && field == 0) {
        cr = (__builtin_constant_p(b) && b == 0) ? PpcCr0CompareSignedZeroOutlined(cr, xer, a)
                                                 : PpcCr0CompareSignedOutlined(cr, xer, a, b);
        return;
    }
    cr = PpcCrCompareSignedOutlined(cr, xer, static_cast<uint32_t>(field), a, b);
#else
    cr = PpcCrCompareBody<int32_t>(cr, xer, static_cast<uint32_t>(field), a, b);
#endif
}
__attribute__((always_inline)) inline void SetCRResident(uint32_t& cr, uint32_t xer, int field, uint32_t a,
                                                         uint32_t b) noexcept {
#if MKW_OUTLINE_CR_COMPARES
    if (__builtin_constant_p(field) && field == 0) {
        cr = (__builtin_constant_p(b) && b == 0) ? PpcCr0CompareUnsignedZeroOutlined(cr, xer, a)
                                                 : PpcCr0CompareUnsignedOutlined(cr, xer, a, b);
        return;
    }
    cr = PpcCrCompareUnsignedOutlined(cr, xer, static_cast<uint32_t>(field), a, b);
#else
    cr = PpcCrCompareBody<uint32_t>(cr, xer, static_cast<uint32_t>(field), a, b);
#endif
}
inline void SetCRFloatResident(uint32_t& cr, int field, double a, double b) noexcept {
#if MKW_OUTLINE_CR_COMPARES
    cr = PpcCrCompareFloatOutlined(cr, static_cast<uint32_t>(field), a, b);
#else
    cr = PpcCrCompareFloatBody(cr, static_cast<uint32_t>(field), a, b);
#endif
}


inline bool GetCRBitResident(uint32_t cr, int field, int bit) noexcept {
    const int shift = (7 - field) * 4 + (3 - bit);
    return ((cr >> shift) & 1u) != 0u;
}

inline uint32_t PpcCrSetBitResident(uint32_t cr, uint32_t bitIndex, uint32_t value) noexcept {
    const uint32_t mask = 1u << (31u - (bitIndex & 31u));
    return (value & 1u) != 0 ? (cr | mask) : (cr & ~mask);
}

inline uint32_t PpcCrLogicalResident(
    uint32_t cr, uint32_t op, uint32_t bt, uint32_t ba, uint32_t bb) noexcept {
    const auto readBit = [cr](uint32_t index) noexcept {
        return (cr >> (31u - (index & 31u))) & 1u;
    };
    const uint32_t a = readBit(ba);
    const uint32_t b = readBit(bb);
    uint32_t result = 0;
    switch (op & 7u) {
        case 0: result = ~(a | b) & 1u; break;
        case 1: result = a & (~b & 1u); break;
        case 2: result = a ^ b; break;
        case 3: result = ~(a & b) & 1u; break;
        case 4: result = a & b; break;
        case 5: result = ~(a ^ b) & 1u; break;
        case 6: result = a | (~b & 1u); break;  // crorc complements CRBB.
        case 7: result = a | b; break;
    }
    return PpcCrSetBitResident(cr, bt, result);
}

inline uint32_t PpcMcrfResident(uint32_t cr, uint32_t dstField, uint32_t srcField) noexcept {
    dstField &= 7u;
    srcField &= 7u;
    const uint32_t dstShift = (7u - dstField) * 4u;
    const uint32_t srcShift = (7u - srcField) * 4u;
    const uint32_t field = (cr >> srcShift) & 0xFu;
    return (cr & ~(0xFu << dstShift)) | (field << dstShift);
}
