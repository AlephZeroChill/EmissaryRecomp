#pragma once
#include "ppc_isa_crv.h"
#include "ppc_isa_fpenv.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept;

union PPC_FPR {
    uint64_t raw;
    double d;
    struct {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        float ps1; // Low word (Least Significant)
        float ps0; // High word (Most Significant)
#else
        float ps0; // High word
        float ps1; // Low word
#endif
    } paired;
};

// PowerPC CPU Context
struct CpuContext {
    // Standard GPRs
    uint32_t gpr[32];

    // Condition Register: one value per field, cr0 first (isa/ppc_isa_crv.h). The packed word:
    // PpcGetCr / PpcSetCr.
    int64_t  crv[8];

    // Special Purpose Registers defined by standard PPC
    uint32_t lr;        // Link Register
    uint32_t ctr;       // Count Register
    uint32_t xer;       // Integer Exception Register
    uint32_t fpscr;     // Floating-Point Status and Control Register

    // Program State
    uint32_t pc;        // Program Counter (not used alot tho)

    // Floating Point Registers (Modified for Paired Single support)
    PPC_FPR  fpr[32];

    // Broadway Specific Extensions
    uint32_t gqr[8];    // Graphics Quantization Registers
    uint32_t hid0;      // HID0
    uint32_t hid1;      // HID1
    uint32_t hid2;      // HID2

    uint32_t srr0;      // Save/Restore Register 0
    uint32_t srr1;      // Save/Restore Register 1
    uint32_t msr;       // Machine State Register
};

// The packed 32-bit CR (mfcr's value, cr0 in the top nibble), and setting all eight fields from one.
inline uint32_t PpcGetCr(const CpuContext* cpu) noexcept { return PpcCrvPack(cpu->crv); }
inline void PpcSetCr(CpuContext* cpu, uint32_t cr) noexcept { PpcCrvUnpack(cr, cpu->crv); }

// Two contexts with the same registers. A CR field has many values for one 4-bit value (a compare's is the
// operands' difference), so the fields compare by what they read as, never bytewise.
inline bool PpcSameCpuContext(const CpuContext& a, const CpuContext& b) noexcept {
    for (uint32_t i = 0; i < 8; ++i)
        if (PpcCrvNibble(a.crv[i]) != PpcCrvNibble(b.crv[i])) return false;
    CpuContext x = a, y = b;
    for (uint32_t i = 0; i < 8; ++i) x.crv[i] = y.crv[i] = 0;
    return std::memcmp(&x, &y, sizeof(CpuContext)) == 0;
}

inline thread_local CpuContext* g_currentCpuContext = nullptr;

class CpuContextScope {
public:
    explicit CpuContextScope(CpuContext* ctx)
        : previous_(g_currentCpuContext)
    {
        g_currentCpuContext = ctx;

        savedMxcsr_ = MkwGetHostFpControl();
        if (ctx != nullptr)
            MkwApplyHostNiMode(ctx->fpscr);
    }

    ~CpuContextScope()
    {
        g_currentCpuContext = previous_;
        if (previous_ != nullptr)
            MkwApplyHostNiMode(previous_->fpscr);
        else
            MkwRestoreHostMxcsr(savedMxcsr_);
    }

    CpuContextScope(const CpuContextScope&) = delete;
    CpuContextScope& operator=(const CpuContextScope&) = delete;

private:
    CpuContext* previous_ = nullptr;
    uint32_t savedMxcsr_ = 0;
};

inline CpuContext* TryGetCpuContext() noexcept
{
    return g_currentCpuContext;
}

inline CpuContext* CurrentCpuContext()
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        std::cerr << "[runtime] CRITICAL: CurrentCpuContext is NULL. "
                  << "Did you forget to create a CpuContextScope?" << std::endl;
        ShowRuntimeFatalPopup("Runtime context failure",
                              "The game stopped because a translated function tried to run without a CPU context.");
        std::abort();
    }

    if (cpu->gpr[1] == 0) {
        std::cerr << "[runtime] CRITICAL: Guest Stack Pointer (r1) is NULL (0x00000000). "
                  << "The emulated program has crashed." << std::endl;
        ShowRuntimeFatalPopup("Guest execution failure",
                              "The game stopped because the guest stack pointer became null while translated code was running.");
        std::abort(); // Stop immediately so you can debug the cause.
    }
    return cpu;
}

// Condition Register Fields
#define CR_LT 0
#define CR_GT 1
#define CR_EQ 2
#define CR_SO 3

extern "C" void DumpHostStackTraceForRuntimeHelper();
void MarkFatalErrorReported();

[[noreturn]] inline void PPC_Undefined(uint32_t pc, uint32_t rawInstruction, const char* details)
{
    std::fprintf(stderr,
                 "[runtime] UNDEFINED guest instruction: pc=0x%08X raw=0x%08X %s\n",
                 pc,
                 rawInstruction,
                 details ? details : "");
    char message[256]{};
    std::snprintf(message, sizeof(message),
                  "The game stopped because it reached an unsupported guest instruction at PC 0x%08X (instruction 0x%08X).\n\n%s",
                  pc, rawInstruction, details ? details : "No additional details were provided.");
    ShowRuntimeFatalPopup("Unsupported guest instruction", message);
    std::abort();
}

// Used by generated code when the translator encounters privileged/unmodeled PPC instructions
// (e.g. rfi). This is intentionally loud so missing HLE hooks are easy to find.
#define UNDEFINED(pc, rawInstruction, details) PPC_Undefined((pc), (rawInstruction), (details))

#ifndef MKW_LEAN_FLAGS
#define MKW_LEAN_FLAGS 0  // isa/ppc_isa_cr.h: compares leave SO clear (translated code never sets XER[SO])
#endif

// A compare into a CR field (signed, unsigned), SO from XER[SO].
inline void SetCR(CpuContext* cpu, int field, int32_t a, int32_t b) {
    cpu->crv[field & 7] = PpcCrvCompare(a, b, cpu->xer);
}
inline void SetCR(CpuContext* cpu, int field, uint32_t a, uint32_t b) {
    cpu->crv[field & 7] = PpcCrvCompare(a, b, cpu->xer);
}

// A floating-point compare into a CR field: unordered (a NaN) sets SO alone.
inline void SetCRFloat(CpuContext* cpu, int field, double a, double b) {
    cpu->crv[field & 7] = PpcCrvCompareFloat(a, b);
}

// One CR bit (bit: 0 LT, 1 GT, 2 EQ, 3 SO).
inline bool GetCRBit(CpuContext* cpu, int field, int bit) {
    return PpcCrvBit(cpu->crv[field & 7], static_cast<uint32_t>(bit));
}
