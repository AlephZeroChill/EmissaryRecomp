// The x86-64 feature groups each engine build needs, and the one check of them. Shared by host_cpu_baseline.cpp
// (an engine's own refusal before its code runs) and the Windows loader (engine_loader.cpp: which engine it starts,
// and its `--engine` answer to Brawl Online's launcher, which downloads engine_usa_v2.dll only when that answer
// names it), so the engine the loader picks is always one that accepts the processor.
//
// Baseline x86-64 only, no allocation, nothing from the C or C++ runtime: host_cpu_baseline.cpp runs it before the
// C runtime is up (see that file's comments).
#pragma once

#if defined(__x86_64__)

#include <cpuid.h>
#include <cstdint>

namespace host_isa {

struct Feature {
    const char* name;
    unsigned leaf;
    unsigned subleaf;
    unsigned reg;  // 0 eax, 1 ebx, 2 ecx, 3 edx
    unsigned bit;
    bool isOsXsave;
};

// x86-64 (v1): SSE2 is architectural.
inline constexpr Feature kV1[] = {
    {"SSE2", 1, 0, 3, 26, false},
};

// x86-64-v2, on top of v1.
inline constexpr Feature kV2[] = {
    {"SSE3", 1, 0, 2, 0, false},
    {"SSSE3", 1, 0, 2, 9, false},
    {"CMPXCHG16B", 1, 0, 2, 13, false},
    {"SSE4.1", 1, 0, 2, 19, false},
    {"SSE4.2", 1, 0, 2, 20, false},
    {"POPCNT", 1, 0, 2, 23, false},
    {"LAHF-SAHF", 0x80000001u, 0, 2, 0, false},
};

// x86-64-v3, on top of v2.
inline constexpr Feature kV3[] = {
    {"FMA", 1, 0, 2, 12, false},
    {"MOVBE", 1, 0, 2, 22, false},
    {"OSXSAVE", 1, 0, 2, 27, true},
    {"AVX", 1, 0, 2, 28, false},
    {"F16C", 1, 0, 2, 29, false},
    {"BMI1", 7, 0, 1, 3, false},
    {"AVX2", 7, 0, 1, 5, false},
    {"BMI2", 7, 0, 1, 8, false},
    {"LZCNT", 0x80000001u, 0, 2, 5, false},
};

inline void CpuId(unsigned leaf, unsigned subleaf, unsigned regs[4]) {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
    regs[0] = eax;
    regs[1] = ebx;
    regs[2] = ecx;
    regs[3] = edx;
}

// XGETBV through inline assembly rather than the _xgetbv intrinsic: the GNU driver gates that intrinsic behind
// -mxsave, which baseline code is compiled without. Only reached once CPUID has reported OSXSAVE.
inline uint64_t ReadXcr0() {
    unsigned eax = 0, edx = 0;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<uint64_t>(edx) << 32) | eax;
}

// Calls onMissing(name) for each feature of levels 1..level (1, 2 or 3) that the host lacks, in table order, and then,
// when they are all there and OSXSAVE is one of them, for the operating system's AVX register state. True when
// nothing is missing.
template <class OnMissing>
bool Check(int level, OnMissing&& onMissing) {
    unsigned regs[4] = {0, 0, 0, 0};
    CpuId(0, 0, regs);
    const unsigned maxBasic = regs[0];
    CpuId(0x80000000u, 0, regs);
    const unsigned maxExtended = regs[0];

    bool ok = true;
    bool haveOsXsave = false;
    auto group = [&](const Feature* begin, const Feature* end) {
        for (const Feature* f = begin; f != end; ++f) {
            const bool leafAvailable = (f->leaf & 0x80000000u) != 0 ? f->leaf <= maxExtended : f->leaf <= maxBasic;
            bool present = false;
            if (leafAvailable) {
                unsigned r[4] = {0, 0, 0, 0};
                CpuId(f->leaf, f->subleaf, r);
                present = (r[f->reg] & (1u << f->bit)) != 0;
            }
            if (present) {
                haveOsXsave = haveOsXsave || f->isOsXsave;
                continue;
            }
            onMissing(f->name);
            ok = false;
        }
    };
    group(kV1, kV1 + sizeof(kV1) / sizeof(kV1[0]));
    if (level >= 2) group(kV2, kV2 + sizeof(kV2) / sizeof(kV2[0]));
    if (level >= 3) group(kV3, kV3 + sizeof(kV3) / sizeof(kV3[0]));

    // CPUID reporting AVX is not sufficient: the OS also has to have enabled XMM and YMM state saving or every
    // VEX-encoded instruction faults. This is the same guard a compiler's own runtime feature dispatch applies.
    if (ok && haveOsXsave) {
        constexpr uint64_t kXmmAndYmmState = 0x6u;
        if ((ReadXcr0() & kXmmAndYmmState) != kXmmAndYmmState) {
            onMissing("operating system support for AVX register state (XCR0 YMM bits)");
            ok = false;
        }
    }
    return ok;
}

}  // namespace host_isa

#endif  // defined(__x86_64__)
