#include "isa/ppc_isa_quantized.h"

#include <iostream>
#include <stdexcept>

// Standalone equivalent of ppc_helpers.cpp's dispatcher; the fallback body is production code.
extern "C" MKW_GUEST_OP_CC void PPC_PsqSt(uint32_t addr, double value, uint32_t w, uint32_t i) {
    const auto gqr = TryGetCpuContext()->gqr[i & 7u];
    if (w == 0) PPC_PsqStStateFallback<0u, false>(gqr, addr, value);
    else PPC_PsqStStateFallback<1u, false>(gqr, addr, value);
}

static unsigned checks = 0;

template <uint32_t I>
static void CheckStore(uint64_t bits, uint32_t gqr) {
    auto& ctx = *TryGetCpuContext();
    ctx.gqr[I] = gqr;
    const double value = PpcBitCastToDoubleInline(bits);
    for (bool resolved : {false, true}) {
        constexpr uint32_t addr = 32, offset = 17;
        auto* host = resolved ? PsqTestMemory::Pointer(addr) - offset : nullptr;
        PsqTestMemory::bytes.fill(0xCD);
        PsqTestMemory::accesses = 0;
        PPC_PsqStSegmentInline<0, I>(&ctx, host, offset, addr, PPC_PsFromScalarInline(value));
        const auto expected = PsqTestMemory::bytes;
        const auto accesses = PsqTestMemory::accesses;
        const auto address = PsqTestMemory::address;
        const auto width = PsqTestMemory::width;
        PsqTestMemory::bytes.fill(0xCD);
        PsqTestMemory::accesses = 0;
        PPC_PsqStScalarSegmentInline<I>(&ctx, host, offset, addr, value);
        if (PsqTestMemory::bytes != expected || PsqTestMemory::accesses != accesses ||
            (accesses && (PsqTestMemory::address != address || PsqTestMemory::width != width)))
            throw std::runtime_error("scalar PSQ store changed bytes or memory route");
        ++checks;
    }
}

int main() {
    try {
        const auto saved = MkwGetHostFpControl();
        CpuContext ctx{};
        for (uint32_t ni : {0u, 4u}) for (uint32_t rounding = 0; rounding < 4; ++rounding) {
            ctx.fpscr = ni;
            CpuContextScope scope(&ctx);
#if defined(__x86_64__)
            MkwSetHostFpControl((MkwGetHostFpControl() & ~(3u << 13)) | (rounding << 13));
#elif defined(__aarch64__)
            constexpr uint32_t armRounding[] = {0, 2, 1, 3};
            MkwSetHostFpControl((MkwGetHostFpControl() & ~(3u << 22)) | (armRounding[rounding] << 22));
#endif
            constexpr uint64_t edges[] = {
                0, 0x8000000000000000ull, 1, 0x000fffffffffffffull,
                0x380fffffe0000000ull, 0x3810000000000000ull, 0x47efffffe0000000ull,
                0x3ff0000000000000ull, 0xbff0000000000000ull,
                0x7ff0000000000000ull, 0xfff0000000000000ull,
                0x7ff0000000000001ull, 0x7ff8123456789abcull, 0xfff0123456789abcull
            };
            // Float fast path, ignored load-half bits, and every valid quantized type/scale.
            for (auto bits : edges) {
                CheckStore<0>(bits, 0);
                CheckStore<7>(bits, 0x21050000u);
                for (uint32_t type : {0u, 4u, 5u, 6u, 7u}) for (uint32_t scale = 0; scale < 64; ++scale) {
                    // The existing integer quantizer has no defined NaN-to-integer conversion.
                    // NaNs are covered above and here for float stores, which this change optimizes.
                    if (type != 0u && (bits & 0x7fffffffffffffffull) > 0x7ff0000000000000ull) continue;
                    CheckStore<7>(bits, 0x2105C0F8u | (scale << 8) | type);
                }
            }
            uint64_t random = 0x123456789abcdef0ull;
            for (unsigned i = 0; i < 100000; ++i) {
                random ^= random << 13; random ^= random >> 7; random ^= random << 17;
                CheckStore<0>(random, 0);
            }
        }
        MkwRestoreHostMxcsr(saved);
        std::cout << "passed " << checks << " scalar PSQ store checks\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
