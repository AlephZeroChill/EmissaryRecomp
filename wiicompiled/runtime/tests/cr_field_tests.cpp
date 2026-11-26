// CpuContext's per-field condition register (isa/ppc_isa_crv.h) against the packed 32-bit forms
// (isa/ppc_isa_cr.h) it replaced: every nibble, compares at their edges and at random, float compares
// with NaN, the CR logical operations, and whole-word round trips.
#include "isa/ppc_isa_cr.h"
#include "isa/ppc_isa_crv.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>

static void require(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        std::abort();
    }
}

template <class T> static void CheckCompare(T a, T b, uint32_t xer) {
    const uint32_t packed = (PpcCrCompareBody(0u, xer, 0u, a, b) >> 28) & 0xFu;
    const int64_t v = PpcCrvCompare(a, b, xer);
    require(PpcCrvNibble(v) == packed, "integer compare field");
    for (uint32_t bit = 0; bit < 4; ++bit)
        require(PpcCrvBit(v, bit) == (((packed >> (3u - bit)) & 1u) != 0), "integer compare bit");
}

static void CheckFloat(double a, double b) {
    const uint32_t packed = (PpcCrCompareFloatBody(0u, 0u, a, b) >> 28) & 0xFu;
    require(PpcCrvNibble(PpcCrvCompareFloat(a, b)) == packed, "float compare field");
}

int main() {
    for (uint32_t n = 0; n < 16; ++n) {
        const int64_t v = PpcCrvEncode(n);
        require(PpcCrvNibble(v) == n, "nibble round trip");
        for (uint32_t bit = 0; bit < 4; ++bit) {
            require(PpcCrvBit(v, bit) == (((n >> (3u - bit)) & 1u) != 0), "nibble bit");
            const uint32_t mask = 0x8u >> bit;
            require(PpcCrvNibble(PpcCrvWithBit(v, bit, true)) == (n | mask), "set bit");
            require(PpcCrvNibble(PpcCrvWithBit(v, bit, false)) == (n & ~mask), "clear bit");
        }
    }

    const int32_t signedEdges[] = {0, 1, -1, 2, -2, 0x7FFFFFFF, -0x7FFFFFFF - 1, 0x7FFFFFFE, -0x7FFFFFFF, 80, -80};
    const uint32_t unsignedEdges[] = {0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0xFFFFFFFEu, 80u};
    for (uint32_t xer : {0u, 0x80000000u, 0x20000000u, 0xA0000000u}) {
        for (int32_t a : signedEdges)
            for (int32_t b : signedEdges) CheckCompare(a, b, xer);
        for (uint32_t a : unsignedEdges)
            for (uint32_t b : unsignedEdges) CheckCompare(a, b, xer);
    }
    std::mt19937_64 rng(12345);
    for (int i = 0; i < 2000000; ++i) {
        const uint64_t r = rng();
        const uint32_t xer = (rng() & 1) ? 0x80000000u : 0u;
        CheckCompare(static_cast<int32_t>(r), static_cast<int32_t>(r >> 32), xer);
        CheckCompare(static_cast<uint32_t>(r), static_cast<uint32_t>(r >> 32), xer);
        // Near-equal operands, where a wrong EQ or GT would show.
        CheckCompare(static_cast<int32_t>(r), static_cast<int32_t>(r) + static_cast<int32_t>(rng() % 3) - 1, xer);
        CheckCompare(static_cast<uint32_t>(r), static_cast<uint32_t>(r) + static_cast<uint32_t>(rng() % 3) - 1u, xer);
    }

    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    const double floats[] = {0.0, -0.0, 1.0, -1.0, 0.5, inf, -inf, nan, 1e-310, -1e-310};
    for (double a : floats)
        for (double b : floats) CheckFloat(a, b);

    for (int i = 0; i < 200000; ++i) {
        const uint32_t cr = static_cast<uint32_t>(rng());
        int64_t f[8];
        PpcCrvUnpack(cr, f);
        require(PpcCrvPack(f) == cr, "word round trip");
        int64_t g[8];
        PpcCrView(g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]) = cr;
        require(static_cast<uint32_t>(PpcCrView(g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7])) == cr, "view");
        const uint32_t op = static_cast<uint32_t>(rng()) & 7u, bt = static_cast<uint32_t>(rng()) & 31u,
                       ba = static_cast<uint32_t>(rng()) & 31u, bb = static_cast<uint32_t>(rng()) & 31u;
        const uint32_t want = PpcCrLogicalResident(cr, op, bt, ba, bb);
        f[bt / 4] = PpcCrvWithBit(f[bt / 4], bt % 4, PpcCrvLogic(op, PpcCrvBit(f[ba / 4], ba % 4), PpcCrvBit(f[bb / 4], bb % 4)));
        require(PpcCrvPack(f) == want, "cr logical");
    }
    std::cout << "cr field tests: ok\n";
    return 0;
}
