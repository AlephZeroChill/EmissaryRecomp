#pragma once
#include "memory.h"
#include "hz_perf.h"
#include "ppc_runtime.h"
#include "system_bridge.h"
#include "game_graphics_options.h"
#include "runtime_log.h"
#include "ppc_interpreter.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <tuple>
#include <type_traits>

inline void InvokeIndirectCpu(uint32_t target, CpuContext* ctx);

// Guest code patching (Gecko codes, Syriinge hooks): every translated function opens with MKW_GUEST_CODE_GUARD,
// one byte per function (indexed by the translator's guest_code_guard table); a function whose guest code changed at
// run time runs in the interpreter instead (guest_code_patch.cpp).
// The translator opens each public entry as a wrapper: the guard, then a tail call of the function's body
// (func_X_body). The table is hidden (one rip-relative compare, no GOT load) and RunPatched takes ctx first (it stays
// in its argument register), so a wrapper is a compare, a conditional tail jump to the body, and a two-instruction
// patched arm.
extern "C" __attribute__((visibility("hidden"))) uint8_t g_mkwGuestCodePatched[];
namespace GuestCodePatch {
void RunPatched(CpuContext* ctx, uint32_t address);
void (*ContinuationAt(uint32_t address))(CpuContext*);  // guest_code_patch.h: the code variant entered at address
}
#define MKW_GUEST_CODE_GUARD(index, address)                    \
    do {                                                        \
        if (__builtin_expect(g_mkwGuestCodePatched[index], 0)) { \
            GuestCodePatch::RunPatched(ctx, address);           \
            return;                                             \
        }                                                       \
    } while (false)

// A hand-written body for a translated function, declared at file scope: `MKW_NATIVE_BODY(0x801EC1B8u, Native);`.
// The translator scans runtime/src for it and makes func_X's wrapper tail-call `symbol` instead of func_X_body (after
// the guard above), so static, direct, indirect and interpreter calls all reach the native at the translated call's
// cost. func_X_body stays: the native's fallback for inputs it does not handle, and its differential-test reference.
#define MKW_NATIVE_BODY(address, symbol) extern "C" void symbol(CpuContext* ctx)

// Native hooks with guest fallback: a function the translator lists (TranslatedBuildShardEmitter, nativeHook) calls
// g_mkwNativeHook[slot] after its guard, so a mod's patch of the function still wins. A hook returning true handled
// the call as the guest code would have: it may use the stack below r1 and the volatile registers the translated
// function writes, and leaves the non-volatile registers and the caller's frames as they were; false (or no hook)
// runs the translated body. Slot 0: nw4r::g3d::DrawResMdlDirectly (hle/g3d_native.cpp).
using MkwNativeHookFn = bool (*)(CpuContext*);
extern "C" __attribute__((visibility("hidden"))) MkwNativeHookFn g_mkwNativeHook[4];
#define MKW_NATIVE_HOOK(slot)                                                     \
    do {                                                                          \
        if (const MkwNativeHookFn mkwHook = g_mkwNativeHook[slot];                \
            __builtin_expect(mkwHook != nullptr, 0) && mkwHook(ctx)) {            \
            return;                                                               \
        }                                                                         \
    } while (false)

// Persistent per-thread CPU context used across translated function calls.
CpuContext& GetPersistentCpuContext();
void InitializePersistentCpuContext();

enum class FunctionKind : uint8_t {
    BaseTranslated = 0,
    ModTranslated = 1,
    Native = 2,
};

inline constexpr uint32_t kPpcAllNonvolatileFprMask = 0xffffc000u;
inline constexpr uint32_t kBaseTranslatedFunctionPriority = 0;
inline constexpr uint32_t kModTranslatedFunctionPriorityBase = 100;
inline constexpr uint32_t kNativeFunctionPriority = 10000;

#include "isa/ppc_isa_cr.h"

struct TranslatedFunctionInfo {
    uint32_t address = 0;
    const char* name = "";
    uint64_t moduleId = 0;
    uint32_t priority = 0;
    uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    void* entryPoint = nullptr; // Raw typed function pointer for indirect-call resolution
    void (*rawCpuInvoker)(CpuContext*) = nullptr;
    bool mustRemainDynamicallyDispatchable = true;
    FunctionKind kind = FunctionKind::BaseTranslated;
};

struct RawDispatchRecord {
    uint32_t address = 0;
    void (*entry)(CpuContext*) = nullptr;
    uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    bool preserveNonvolatileGprs = false;
};

// Translator-owned indirect dispatch is split by the high address byte and
// then by 4 KiB guest pages. Each populated segment stores only the contiguous
// page range that contains verified function entry points. The final search is
// bounded to at most the 1024 aligned instruction addresses in one guest page.
struct StaticIndirectDispatchPage {
    uint32_t firstEntry = 0;
    uint16_t entryCount = 0;
    // Explicit tail padding to an 8-byte stride. The emitter writes it as a
    // literal third initializer, so it is part of the generated-data contract.
    uint16_t reserved = 0;
};

struct StaticIndirectDispatchSegment {
    const StaticIndirectDispatchPage* pages = nullptr;
    uint16_t firstPage = 0;
    uint16_t pageCount = 0;
};

struct StaticIndirectDispatchTable {
    const char* profileName = nullptr;
    const StaticIndirectDispatchSegment* segments = nullptr; // Exactly 256 high-byte segments.
    const RawDispatchRecord* entries = nullptr;
    size_t entryCount = 0;
};

inline const RawDispatchRecord* FindStaticIndirectDispatchEntry(
    const StaticIndirectDispatchTable* table,
    uint32_t address) noexcept {
    if (!table || !table->segments || !table->entries || table->entryCount == 0) {
        return nullptr;
    }

    const auto& segment = table->segments[address >> 24];
    const uint32_t pageNumber = (address >> 12) & 0x0FFFu;
    if (!segment.pages || pageNumber < segment.firstPage) {
        return nullptr;
    }
    const uint32_t relativePage = pageNumber - segment.firstPage;
    if (relativePage >= segment.pageCount) {
        return nullptr;
    }

    const auto& page = segment.pages[relativePage];
    const size_t first = page.firstEntry;
    const size_t count = page.entryCount;
    if (first > table->entryCount || count > table->entryCount - first) {
        return nullptr;
    }

    size_t lower = first;
    size_t upper = first + count;
    while (lower < upper) {
        const size_t middle = lower + (upper - lower) / 2;
        const uint32_t candidate = table->entries[middle].address;
        if (candidate < address) {
            lower = middle + 1;
        } else {
            upper = middle;
        }
    }
    return lower < first + count && table->entries[lower].address == address
        ? &table->entries[lower]
        : nullptr;
}

void RegisterStaticIndirectDispatchTable(const StaticIndirectDispatchTable* table);
inline std::atomic<const StaticIndirectDispatchTable*> g_publishedStaticIndirectDispatchTable{nullptr};

class StaticIndirectDispatchTableRegistrar {
public:
    explicit StaticIndirectDispatchTableRegistrar(const StaticIndirectDispatchTable* table) {
        RegisterStaticIndirectDispatchTable(table);
    }
};

inline unsigned PpcLowestSetBitIndex(uint32_t value) noexcept {
    return static_cast<unsigned>(__builtin_ctz(value));
}

class PpcNonvolatileFprGuard {
public:
    explicit PpcNonvolatileFprGuard(CpuContext* cpu, uint32_t mask = kPpcAllNonvolatileFprMask) noexcept
        : cpu_(cpu), mask_(mask & kPpcAllNonvolatileFprMask) {
        if (!cpu_ || mask_ == 0) {
            return;
        }
        // Real masks are a single contiguous register run - f14..f31 for the
        // conservative default and f23..f31 or similar for a measured writer -
        // so resolve the run once and copy it as one straight-line block instead
        // of testing 18 mask bits. Any other mask keeps the per-bit loop, which
        // saves and restores exactly the same registers.
        const uint32_t lowest = mask_ & (0u - mask_);
        const uint32_t aboveRun = mask_ + lowest;
        if ((aboveRun & (aboveRun - 1u)) == 0u) {
            const unsigned first = PpcLowestSetBitIndex(mask_);
            const unsigned end = aboveRun == 0u ? 32u : PpcLowestSetBitIndex(aboveRun);
            firstSlot_ = static_cast<uint8_t>(first - 14u);
            slotCount_ = static_cast<uint8_t>(end - first);
            const size_t count = slotCount_;
            for (size_t i = 0; i < count; ++i) {
                saved_[firstSlot_ + i] = cpu_->fpr[first + i];
            }
            return;
        }
        for (size_t i = 0; i < saved_.size(); ++i) {
            if ((mask_ & (1u << (14 + i))) == 0) {
                continue;
            }
            saved_[i] = cpu_->fpr[14 + i];
        }
    }

    ~PpcNonvolatileFprGuard() noexcept {
        if (!cpu_ || mask_ == 0) {
            return;
        }
        if (slotCount_ != 0) {
            const unsigned first = 14u + firstSlot_;
            const size_t count = slotCount_;
            for (size_t i = 0; i < count; ++i) {
                cpu_->fpr[first + i] = saved_[firstSlot_ + i];
            }
            return;
        }
        for (size_t i = 0; i < saved_.size(); ++i) {
            if ((mask_ & (1u << (14 + i))) == 0) {
                continue;
            }
            cpu_->fpr[14 + i] = saved_[i];
        }
    }

    PpcNonvolatileFprGuard(const PpcNonvolatileFprGuard&) = delete;
    PpcNonvolatileFprGuard& operator=(const PpcNonvolatileFprGuard&) = delete;

private:
    CpuContext* cpu_ = nullptr;
    uint32_t mask_ = 0;
    // Contiguous-run description of mask_. slotCount_ == 0 means the mask is not
    // a single run, so the destructor mirrors the constructor's per-bit loop.
    uint8_t firstSlot_ = 0;
    uint8_t slotCount_ = 0;
    // Every element selected by mask_ is written before it is read. Leaving the
    // remaining slots uninitialized avoids zero-filling all 18 values around a
    // call that may preserve only one or two architectural FPRs.
    std::array<PPC_FPR, 18> saved_;
};

class PpcNonvolatileGprGuard {
public:
    explicit PpcNonvolatileGprGuard(CpuContext* cpu, bool enabled = true) noexcept
        : cpu_(enabled ? cpu : nullptr) {
        if (!cpu_) {
            return;
        }
        for (size_t i = 0; i < saved_.size(); ++i) {
            saved_[i] = cpu_->gpr[14 + i];
        }
    }

    ~PpcNonvolatileGprGuard() noexcept {
        if (!cpu_) {
            return;
        }
        for (size_t i = 0; i < saved_.size(); ++i) {
            cpu_->gpr[14 + i] = saved_[i];
        }
    }

    PpcNonvolatileGprGuard(const PpcNonvolatileGprGuard&) = delete;
    PpcNonvolatileGprGuard& operator=(const PpcNonvolatileGprGuard&) = delete;

private:
    CpuContext* cpu_ = nullptr;
    // Left uninitialized: enabled writes all 18 slots before reading them, disabled reads none,
    // so skipping the zero-fill saves 72 bytes of stack work on the common disabled hot path.
    std::array<uint32_t, 18> saved_;
};

// Guest address of OSLoadContext, which models rfi-style context restoration
// and intentionally replaces the full guest register file instead of returning
// like a normal ABI call. It can never be guarded.
inline constexpr uint32_t kOSLoadContextAddress = 0x801D80B4u;

inline bool ShouldPreserveNonvolatileGprsForRawCpuCall(const TranslatedFunctionInfo* info) noexcept {
    return info->kind == FunctionKind::Native && info->address != kOSLoadContextAddress;
}

inline uint32_t NonvolatileFprGuardMaskFor(const TranslatedFunctionInfo* info) noexcept {
    if (!info || info->nonvolatileFprWriteMask == 0) {
        return 0;
    }
    return info->nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask;
}

template <uint32_t Target>
struct KnownTranslatedCpuCall {
    static constexpr bool kAvailable = false;
    static constexpr uint32_t kNonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    static constexpr bool kMustRemainDynamicallyDispatchable = true;
    static constexpr void (*Entry)(CpuContext*) = nullptr;
};

// Translator-emitted trait specializations, one macro shared by every generated shard.
// `addr` is the 8-digit hex entry point, `winner` the resolved symbol (mods/Retro Rewind can
// publish their own name). A specialization only exists for a single statically bound winner,
// so overridable/dynamically-dispatchable are hardcoded here rather than passed in.
#define MKW_TRANSLATED_TRAIT(addr, winner, nonvolatile_fpr_write_mask)                    \
    extern "C" void winner(CpuContext* ctx);                                              \
    template <>                                                                           \
    struct KnownTranslatedCpuCall<0x##addr##u> {                                          \
        static constexpr bool kAvailable = true;                                          \
        static constexpr uint32_t kNonvolatileFprWriteMask = nonvolatile_fpr_write_mask;  \
        static constexpr bool kMustRemainDynamicallyDispatchable = false;                 \
        static constexpr void (*Entry)(CpuContext*) = &winner;                            \
    }

template <uint32_t Target>
struct KnownNativeCpuCall {
    static constexpr bool kAvailable = false;
    static constexpr uint32_t kNonvolatileFprWriteMask = kPpcAllNonvolatileFprMask;
    static constexpr void (*Entry)(CpuContext*) = nullptr;
};

template <uint32_t Target>
struct KnownTypedNativeCpuCall {
    static constexpr bool kAvailable = false;
};

#include "native_cpu_calls.inc"

// Direct-mapped thread-local memo in front of the sorted registry lookup; since the per-site inline caches
// (indirect_call_site.h) only their slow paths reach it, with P+ Training's many polymorphic sites. 2048 entries
// (32 KiB per memo): the 512 before missed often enough to keep the generated table's search at ~0.9% of the game
// thread (P+ Training profile). Indexed by the top bits of a multiplicative hash (the low bits of the
// product only mix the low bits of the address). Must stay a power of two.
inline constexpr unsigned kIndirectDispatchCacheBits = 11;
inline constexpr size_t kIndirectDispatchCacheEntries = size_t{1} << kIndirectDispatchCacheBits;
MKW_PPC_FORCE_INLINE size_t IndirectDispatchMemoIndex(uint32_t address) {
    return ((address >> 2) * 2654435761u) >> (32 - kIndirectDispatchCacheBits);
}

// Namespace-scope `inline thread_local`, not function-local `static thread_local`, to avoid a
// thread-static init epoch check on every bctrl (same as g_currentCpuContext in ppc_runtime.h).
struct IndirectResolvedDispatchMemoEntry {
    bool valid;
    uint32_t address;
    const TranslatedFunctionInfo* info;
};
inline thread_local IndirectResolvedDispatchMemoEntry
    g_indirectResolvedDispatchMemo[kIndirectDispatchCacheEntries]{};

struct IndirectRawDispatchMemoEntry {
    bool valid;
    uint32_t address;
    const RawDispatchRecord* record;
};
inline thread_local IndirectRawDispatchMemoEntry
    g_indirectRawDispatchMemo[kIndirectDispatchCacheEntries]{};

class TranslatedFunctionRegistry {
public:
    static void Register(TranslatedFunctionInfo info);
    static void Finalize();
    static inline bool IsLookupPublished() noexcept {
        return lookupPublished_.load(std::memory_order_acquire);
    }
    static std::optional<TranslatedFunctionInfo> FindNearestByAddress(uint32_t address);
    static inline const TranslatedFunctionInfo* FindByAddressPtr(uint32_t address) {
        if (!lookupPublished_.load(std::memory_order_acquire)) {
            return FindByAddressPtrSlow(address);
        }

        auto& cached = g_indirectResolvedDispatchMemo[IndirectDispatchMemoIndex(address)];
        if (cached.valid && cached.address == address) {
            return cached.info;
        }

        const auto* info = FindByAddressPtrSlow(address);
        cached = {true, address, info};
        return info;
    }

    // Hot wrapper: only the direct-mapped memo probe stays inline. Inlining the
    // generated-table binary search alongside it pushed the whole function past
    // the inliner's budget and out-of-lined the probe itself, which is the part
    // every bctrl executes.
    static MKW_PPC_FORCE_INLINE const RawDispatchRecord* FindRawByAddressPtr(uint32_t address) {
        if (!lookupPublished_.load(std::memory_order_acquire)) {
            return nullptr;
        }

        auto& cached = g_indirectRawDispatchMemo[IndirectDispatchMemoIndex(address)];
        if (cached.valid && cached.address == address) {
            return cached.record;
        }

        return FindRawByAddressPtrMiss(address, cached);
    }
    // Look up a registered function by host (native) address.
    // This enables stack trace symbolication for translated functions.
    static std::optional<TranslatedFunctionInfo> FindByHostAddress(uintptr_t hostAddr);

private:
    // Memo miss: resolve against the header-inline generated table first; only dynamically
    // registered records or a not-yet-published table fall through to the slow path.
    static MKW_PPC_NO_INLINE MKW_PPC_COLD const RawDispatchRecord* FindRawByAddressPtrMiss(
        uint32_t address, IndirectRawDispatchMemoEntry& cached) {
        const RawDispatchRecord* record = nullptr;
        if (const auto* table =
                g_publishedStaticIndirectDispatchTable.load(std::memory_order_acquire)) {
            record = FindStaticIndirectDispatchEntry(table, address);
        }
        if (record == nullptr) {
            record = FindRawByAddressPtrSlow(address);
        }
        cached = {true, address, record};
        return record;
    }

    static const TranslatedFunctionInfo* FindByAddressPtrSlow(uint32_t address);
    static const RawDispatchRecord* FindRawByAddressPtrSlow(uint32_t address);

    // Finalize publishes immutable registry, generated-table, and dynamic raw
    // dispatch storage. Header-inlined cache hits are enabled only after that
    // release publication, so a pre-finalization miss can never poison them.
    inline static std::atomic_bool lookupPublished_{false};
};

// Cold, profile-owned registration data is emitted in compact table shards.
// Keeping it out of generated function bodies avoids one static constructor
// per translated function.
struct BulkTranslatedFunctionRecord {
    uint32_t address;
    const char* name;
    void (*entry)(CpuContext*);
    FunctionKind kind;
    bool preservesNonvolatileFprs;
    uint32_t nonvolatileFprWriteMask;
    uint32_t priority;
    uint64_t moduleId;
    bool mustRemainDynamicallyDispatchable;
};

void RegisterBulkTranslatedFunctions(const BulkTranslatedFunctionRecord* records, size_t count);

class BulkTranslatedFunctionRegistrar {
public:
    BulkTranslatedFunctionRegistrar(const BulkTranslatedFunctionRecord* records, size_t count) {
        RegisterBulkTranslatedFunctions(records, count);
    }
};

MKW_PPC_FORCE_INLINE bool TryDispatchResolvedCpuTarget(const TranslatedFunctionInfo* info, CpuContext* cpu) {
    if (!info || !info->rawCpuInvoker) {
        return false;
    }

    RecompMod::ScopedTranslatedExecutionAddress translatedExecution(info->address);
    PpcNonvolatileFprGuard fprGuard(cpu, NonvolatileFprGuardMaskFor(info));
    PpcNonvolatileGprGuard gprGuard(cpu, ShouldPreserveNonvolatileGprsForRawCpuCall(info));
    if (TryGetCpuContext() != cpu) {
        CpuContextScope scope(cpu);
        info->rawCpuInvoker(cpu);
        return true;
    }
    info->rawCpuInvoker(cpu);
    return true;
}

inline bool TryDispatchRawCpuTarget(const RawDispatchRecord* record, CpuContext* cpu) {
    if (!record || !record->entry) {
        return false;
    }
    RecompMod::ScopedTranslatedExecutionAddress translatedExecution(record->address);
    PpcNonvolatileGprGuard gprGuard(cpu, record->preserveNonvolatileGprs);
    PpcNonvolatileFprGuard fprGuard(cpu, record->nonvolatileFprWriteMask);
    if (TryGetCpuContext() != cpu) {
        CpuContextScope scope(cpu);
        record->entry(cpu);
    } else {
        record->entry(cpu);
    }
    return true;
}

template <uint32_t Target>
inline void DispatchKnownTranslatedCpuTargetStatic(CpuContext* cpu) {
    const auto invokeKnownTranslated = [&]() {
        KnownTranslatedCpuCall<Target>::Entry(cpu);
    };

    // A statically resolved, non-overridable translated call already running
    // in this CpuContext needs no registry, context, or diagnostic target
    // transition. This is the normal generated-to-generated hot path.
    if (TryGetCpuContext() == cpu) {
        if constexpr (KnownTranslatedCpuCall<Target>::kNonvolatileFprWriteMask == 0) {
            invokeKnownTranslated();
            return;
        } else {
            PpcNonvolatileFprGuard fprGuard(
                cpu, KnownTranslatedCpuCall<Target>::kNonvolatileFprWriteMask);
            invokeKnownTranslated();
            return;
        }
    }

    CpuContextScope contextScope(cpu);
    RecompMod::ScopedTranslatedExecutionAddress translatedExecution(Target);
    if constexpr (KnownTranslatedCpuCall<Target>::kNonvolatileFprWriteMask == 0) {
        invokeKnownTranslated();
        return;
    } else {
        PpcNonvolatileFprGuard fprGuard(cpu, KnownTranslatedCpuCall<Target>::kNonvolatileFprWriteMask);
        invokeKnownTranslated();
    }
}

[[noreturn]] inline void ReportMissingCpuTarget(uint32_t target, CpuContext* cpu) {
    // Centralized crash path: stderr diagnostics, heuristics, crash artifacts
    // in the run's log folder, popup, exit.
    RuntimeCrash::FatalMissingGuestTarget(target, cpu);
}

// The code variant of a mod's code without a base translation (a replacement module or plugin linked in place, as a
// build captured it: guest_code_patch.cpp), when guest memory still holds its words: runs it and returns true.
inline bool RunCodeVariantAt(uint32_t target, CpuContext* cpu) {
    const auto variant = GuestCodePatch::ContinuationAt(target);
    if (variant == nullptr) return false;
    if (TryGetCpuContext() != cpu) {
        CpuContextScope scope(cpu);
        variant(cpu);
    } else {
        variant(cpu);
    }
    return true;
}

// Guest code without a translation (code a mod placed in memory, or an address the translation missed) runs its code
// variant or in the interpreter; only a target that is not guest memory at all is fatal.
inline void RunUntranslatedOrFail(uint32_t target, CpuContext* cpu) {
    if (RunCodeVariantAt(target, cpu)) return;
    if (PpcInterpreter::Enabled() && PpcInterpreter::CanRun(target)) {
        PpcInterpreter::Run(cpu, target);
        return;
    }
    ReportMissingCpuTarget(target, cpu);
}

// Code variants only (CxxLinearCodeGenerator emits it before an indirect jump's dispatch: a bctr without recognized
// targets, or a jump table's unlisted case). A target inside the variant's own code (ownCode: start/end pairs, its
// decoded runs with small gaps closed) that is no entry of anything (the variant itself, a translated function, a code
// variant continuation) means the translation missed a jump target: a jump table cut short.
// Dispatching would run the interpreter from the middle of the function with a stale lr; stop with a report instead.
inline void CheckVariantIndirectJump(uint32_t target, CpuContext* cpu, uint32_t variant, uint32_t block,
                                     const uint32_t* ownCode, uint32_t rangeCount) {
    for (uint32_t i = 0; i < rangeCount; ++i) {
        if (target - ownCode[2 * i] >= ownCode[2 * i + 1] - ownCode[2 * i]) continue;
        if (target == variant || TranslatedFunctionRegistry::FindByAddressPtr(target) != nullptr ||
            GuestCodePatch::ContinuationAt(target) != nullptr) {
            return;
        }
        RuntimeCrash::FatalVariantJumpIntoItself(variant, block, target, cpu);
    }
}

template <uint32_t Target>
inline const TranslatedFunctionInfo* ResolveDirectCpuTargetInfo() {
    return TranslatedFunctionRegistry::FindByAddressPtr(Target);
}

template <uint32_t Target>
inline const TranslatedFunctionInfo* ResolveDirectCpuTargetInfoCached() {
    // Only latch a result once the registry is published (immutable for the process lifetime);
    // a pre-finalization miss or provisional winner must never be cached. 0 = not yet resolved,
    // 1 = negative-cache sentinel, avoiding a second ready flag on the hot path.
    static std::atomic<uintptr_t> cachedValue{0};
    constexpr uintptr_t kNegativeCache = 1;
    const uintptr_t cached = cachedValue.load(std::memory_order_acquire);
    if (cached != 0) {
        return cached == kNegativeCache
            ? nullptr
            : reinterpret_cast<const TranslatedFunctionInfo*>(cached);
    }
    if (!TranslatedFunctionRegistry::IsLookupPublished()) {
        return ResolveDirectCpuTargetInfo<Target>();
    }
    const auto* info = ResolveDirectCpuTargetInfo<Target>();
    cachedValue.store(info != nullptr
                          ? reinterpret_cast<uintptr_t>(info)
                          : kNegativeCache,
                      std::memory_order_release);
    return info;
}

// Caches the "is the published winner still base translation?" verdict for state-free fast
// paths guarding tens of thousands of call sites. Same latching rule as above: 0 = unresolved,
// 1 = negative, 2 = positive, and a verdict computed pre-publication is never stored.
template <uint32_t Target>
inline bool IsBaseTranslatedCpuTargetActive() {
    static std::atomic<uint8_t> cachedVerdict{0};
    constexpr uint8_t kNegativeVerdict = 1;
    constexpr uint8_t kPositiveVerdict = 2;
    const uint8_t cached = cachedVerdict.load(std::memory_order_acquire);
    if (cached != 0) {
        return cached == kPositiveVerdict;
    }
    const bool publishable = TranslatedFunctionRegistry::IsLookupPublished();
    const auto* info = ResolveDirectCpuTargetInfo<Target>();
    const bool active = info != nullptr && info->kind == FunctionKind::BaseTranslated;
    if (publishable) {
        cachedVerdict.store(active ? kPositiveVerdict : kNegativeVerdict,
                            std::memory_order_release);
    }
    return active;
}

template <uint32_t Target>
inline void InvokeDirectCpu(CpuContext* ctx) {
    static_assert(Target != 0, "InvokeDirectCpu cannot target address 0");
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if constexpr (KnownNativeCpuCall<Target>::kAvailable) {
        const auto invokeKnownNative = [&]() {
            PpcNonvolatileGprGuard gprGuard(cpu);
            if (TryGetCpuContext() != cpu) {
                CpuContextScope scope(cpu);
                KnownNativeCpuCall<Target>::Entry(cpu);
                return;
            }
            KnownNativeCpuCall<Target>::Entry(cpu);
        };
        if constexpr (KnownNativeCpuCall<Target>::kNonvolatileFprWriteMask == 0) {
            invokeKnownNative();
        } else {
            PpcNonvolatileFprGuard fprGuard(cpu, KnownNativeCpuCall<Target>::kNonvolatileFprWriteMask);
            invokeKnownNative();
        }
        return;
    }

    if constexpr (KnownTypedNativeCpuCall<Target>::kAvailable) {
        PpcNonvolatileGprGuard gprGuard(cpu);
        if (TryGetCpuContext() != cpu) {
            CpuContextScope scope(cpu);
            KnownTypedNativeCpuCall<Target>::Invoke(cpu);
        } else {
            KnownTypedNativeCpuCall<Target>::Invoke(cpu);
        }
        return;
    }

    if constexpr (KnownTranslatedCpuCall<Target>::kAvailable) {
        DispatchKnownTranslatedCpuTargetStatic<Target>(cpu);
        return;
    }

    const auto* registeredInfo = ResolveDirectCpuTargetInfoCached<Target>();
    if (TryDispatchResolvedCpuTarget(registeredInfo, cpu)) {
        return;
    }

    RunUntranslatedOrFail(Target, cpu);
}

// OSDisableInterrupts / OSEnableInterrupts / OSRestoreInterrupts (~900 call sites, called thousands of times a frame):
// their published winner is always the native override (hle/os/os_interrupt.cpp, PPC_NATIVE_OVERRIDE), reached through
// the registry, DispatchKnownTranslatedCpuTargetStatic and AbiRawCpuThunk. These call it directly with what that thunk
// does: arguments from r3, the int32 result into r3. The natives read and write only guest memory and the host
// interrupt flag, never the context's registers, so no context scope or register guard is involved either way.
extern "C" int32_t OS__DisableInterrupts_HLE();
extern "C" int32_t OS__EnableInterrupts_HLE();
extern "C" int32_t OS__RestoreInterrupts_HLE(int32_t level);
template <>
inline void InvokeDirectCpu<0x801DCF10u>(CpuContext* ctx) {
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    cpu->gpr[3] = static_cast<uint32_t>(OS__DisableInterrupts_HLE());
}
template <>
inline void InvokeDirectCpu<0x801DCF24u>(CpuContext* ctx) {
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    cpu->gpr[3] = static_cast<uint32_t>(OS__EnableInterrupts_HLE());
}
template <>
inline void InvokeDirectCpu<0x801DCF38u>(CpuContext* ctx) {
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    cpu->gpr[3] = static_cast<uint32_t>(OS__RestoreInterrupts_HLE(static_cast<int32_t>(cpu->gpr[3])));
}

// OSLockMutex / OSUnlockMutex (hle/os/os_scheduler.cpp, PPC_NATIVE_OVERRIDE_VOID, always the published winner): the
// generic path (TryDispatchResolvedCpuTarget) wraps every native call in a copy-out/copy-back of r14-r31 and of the
// non-volatile FPRs (a native's default write mask), ~70 memory operations per call. These two natives write only r3
// of the context (the mutex handed to OSSleepThread / OSWakeupThread); a thread switch inside them saves and restores
// the whole register file, so the guards restore what is already there. The execution-address and context scopes are
// kept as that path makes them.
extern "C" void OSLockMutex_HLE(CpuContext* ctx);
extern "C" void OSUnlockMutex_HLE(CpuContext* ctx);
template <>
inline void InvokeDirectCpu<0x801DEBB4u>(CpuContext* ctx) {
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    RecompMod::ScopedTranslatedExecutionAddress translatedExecution(0x801DEBB4u);
    if (TryGetCpuContext() != cpu) {
        CpuContextScope scope(cpu);
        OSLockMutex_HLE(cpu);
        return;
    }
    OSLockMutex_HLE(cpu);
}
template <>
inline void InvokeDirectCpu<0x801DEC90u>(CpuContext* ctx) {
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    RecompMod::ScopedTranslatedExecutionAddress translatedExecution(0x801DEC90u);
    if (TryGetCpuContext() != cpu) {
        CpuContextScope scope(cpu);
        OSUnlockMutex_HLE(cpu);
        return;
    }
    OSUnlockMutex_HLE(cpu);
}

// Indirect jump (bctr without link) - used for switch tables.
inline void InvokeIndirectJump(uint32_t target, CpuContext* ctx) {
    // Use the caller's context, not the persistent global context!
    // This is essential for tail calls via bctr where the 'this' pointer (r3)
    // must be passed correctly to the target function.
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (TryDispatchRawCpuTarget(TranslatedFunctionRegistry::FindRawByAddressPtr(target), cpu)) {
        return;
    }

    // Cold fallback for dynamically registered or signature-only targets.
    const auto* info = TranslatedFunctionRegistry::FindByAddressPtr(target);
    if (info) {
        if (TryDispatchResolvedCpuTarget(info, cpu)) {
            return;
        }
    }
    
    // A tail jump into untranslated code (a mod's) runs its code variant or in the interpreter like a call would.
    if (RunCodeVariantAt(target, cpu)) return;
    if (PpcInterpreter::Enabled() && PpcInterpreter::CanRun(target) &&
        TranslatedFunctionRegistry::FindByAddressPtr(target & ~3u) == nullptr) {
        PpcInterpreter::Run(cpu, target);
        return;
    }
    // If not registered, this is likely a switch table jump to an intra-function label.
    // The translator should have recognized this pattern and generated proper switch code.
    RT_LOG(RT_TAG_RUNTIME) << "InvokeIndirectJump: target 0x"
              << std::hex << target << std::dec
              << " is not a registered function.\n"
              << "This is likely a switch statement jump table that the translator\n"
              << "failed to recognize. The function containing this bctr needs\n"
              << "proper switch pattern recognition." << std::endl;
    RT_LOG(RT_TAG_RUNTIME) << "Indirect jump context: pc=0x" << std::hex << cpu->pc
              << " lr=0x" << cpu->lr << " ctr=0x" << cpu->ctr
              << " r1=0x" << cpu->gpr[1] << " r3=0x" << cpu->gpr[3]
              << " r29=0x" << cpu->gpr[29] << " r30=0x" << cpu->gpr[30]
              << " r31=0x" << cpu->gpr[31] << std::dec << std::endl;
    DumpHostStackTraceForRuntimeHelper();
    std::fflush(stderr);
    std::ostringstream message;
    message << "The game stopped because an indirect jump targeted guest address 0x"
            << std::hex << target
            << ", but that address is not a registered translated function.\n\n"
            << "The translator may have missed a switch/jump-table target.";
    // Same artifact set as RuntimeCrash::FatalMissingGuestTarget, which is this
    // path's sibling for indirect *calls*: MarkFatalErrorReported below stops the
    // atexit reporter, so the crash log has to be written here.
    RuntimeCrash::WriteCrashArtifacts("missing_jump_target", message.str(), &target);
    SetRuntimeExitCode(EXIT_FAILURE);
    ShowRuntimeFatalPopup("Missing indirect jump target", message.str());
    MarkFatalErrorReported();
    std::exit(EXIT_FAILURE);
}

inline void InvokeIndirectCpu(uint32_t target, CpuContext* ctx) {
    HZ_EXCLUSIVE_PAUSE_IF_ACTIVE();  // HLE calling guest code: that time is the guest's, not the HLE function's
    CpuContext* cpu = ctx ? ctx : &GetPersistentCpuContext();
    if (target == 0) {
        ReportMissingCpuTarget(target, cpu);
    }
    if (TryDispatchRawCpuTarget(TranslatedFunctionRegistry::FindRawByAddressPtr(target), cpu)) {
        return;
    }
    if (TryDispatchResolvedCpuTarget(TranslatedFunctionRegistry::FindByAddressPtr(target), cpu)) {
        return;
    }
    RunUntranslatedOrFail(target, cpu);
}

template <typename Signature>
class AbiTrampoline;

template <typename Ret, typename... Args>
class AbiTrampoline<Ret(Args...)> {
public:
    using TargetFn = Ret (*)(Args...);

    AbiTrampoline(uint32_t address,
                  const char* name,
                  TargetFn target,
                  FunctionKind kind = FunctionKind::BaseTranslated,
                  bool preservesNonvolatileFprs = false,
                  uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask,
                  uint32_t priority = 0,
                  uint64_t moduleId = 0,
                  void (*rawCpuInvoker)(CpuContext*) = nullptr,
                  bool mustRemainDynamicallyDispatchable = true)
    {
        TranslatedFunctionInfo info;
        info.address = address;
        info.name = name ? name : "";
        info.moduleId = moduleId;
        info.priority = priority;
        const bool effectivePreservesNonvolatileFprs =
            preservesNonvolatileFprs || kind == FunctionKind::Native;
        info.nonvolatileFprWriteMask = effectivePreservesNonvolatileFprs ? 0 : (nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask);
        info.entryPoint = reinterpret_cast<void*>(target);
        info.rawCpuInvoker = rawCpuInvoker;
        info.mustRemainDynamicallyDispatchable = mustRemainDynamicallyDispatchable;
        info.kind = kind;
        TranslatedFunctionRegistry::Register(std::move(info));
    }

    static std::tuple<Args...> BuildArgs(CpuContext* cpu) {
        size_t gprIndex = 0;
        size_t fprIndex = 0;
        return std::tuple<Args...>{LoadArgument<Args>(cpu, gprIndex, fprIndex)...};
    }

    template <typename T>
    static std::remove_reference_t<T> LoadArgument(CpuContext* cpu, size_t& gprIndex, size_t& fprIndex) {
        using CleanT = std::remove_reference_t<T>;
        if constexpr (std::is_floating_point_v<CleanT>) {
            if (fprIndex >= 13) {
                throw std::out_of_range("FPR argument overflow");
            }
            return static_cast<CleanT>(cpu->fpr[1 + fprIndex++].d);
        } else {
            if (gprIndex >= 8) {
                size_t stackArgIndex = gprIndex - 8;
                uint32_t sp = cpu->gpr[1];
                uint32_t argAddr = sp + 8 + static_cast<uint32_t>(stackArgIndex * 4);
                gprIndex++;
                try {
                    return static_cast<CleanT>(Memory::Read32(argAddr));
                } catch (const Memory::AccessViolation&) {
                    RT_LOG(RT_TAG_RUNTIME) << "Stack parameter read failed at 0x" << std::hex << argAddr
                              << " (SP=0x" << sp << ", argIndex=" << std::dec << (gprIndex - 1) << ")" << std::endl;
                    throw;
                }
            }
            return static_cast<CleanT>(cpu->gpr[3 + gprIndex++]);
        }
    }

    static void InvokeCpu(TargetFn target, CpuContext* cpu) {
        auto args = BuildArgs(cpu);
        if constexpr (std::is_void_v<Ret>) {
            std::apply(target, args);
        } else {
            auto result = std::apply(target, args);
            if constexpr (std::is_floating_point_v<Ret>) {
                cpu->fpr[1].d = static_cast<double>(result);
            } else {
                cpu->gpr[3] = static_cast<uint32_t>(result);
            }
        }
    }
};

template <auto Target>
struct AbiRawCpuThunk {
    static void Invoke(CpuContext* cpu) {
        HZ_EXCLUSIVE(kHzHleTicks);
        if constexpr (std::is_invocable_v<decltype(Target), CpuContext*>) {
            (void)Target(cpu);
        } else {
            AbiTrampoline<std::remove_pointer_t<decltype(Target)>>::InvokeCpu(Target, cpu);
        }
    }
};

template <typename Ret>
class AbiTrampoline<Ret(CpuContext*)> {
public:
    using TargetFn = Ret (*)(CpuContext*);

    AbiTrampoline(uint32_t address,
                  const char* name,
                  TargetFn target,
                  FunctionKind kind = FunctionKind::BaseTranslated,
                  bool preservesNonvolatileFprs = false,
                  uint32_t nonvolatileFprWriteMask = kPpcAllNonvolatileFprMask,
                  uint32_t priority = 0,
                  uint64_t moduleId = 0,
                  void (*rawCpuInvoker)(CpuContext*) = nullptr,
                  bool mustRemainDynamicallyDispatchable = true)
    {
        TranslatedFunctionInfo info;
        info.address = address;
        info.name = name ? name : "";
        info.moduleId = moduleId;
        info.priority = priority;
        // Deliberately *not* mirroring the typed specialization's
        // `|| kind == FunctionKind::Native`. A CpuContext* native receives the
        // whole guest register file and can legitimately write f14-f31 through
        // it, so it stays conservative until the individual address has been
        // measured clean and its registration opts out below.
        info.nonvolatileFprWriteMask = preservesNonvolatileFprs ? 0 : (nonvolatileFprWriteMask & kPpcAllNonvolatileFprMask);
        info.entryPoint = reinterpret_cast<void*>(target);
        info.rawCpuInvoker = rawCpuInvoker ? rawCpuInvoker : reinterpret_cast<void (*)(CpuContext*)>(target);
        info.mustRemainDynamicallyDispatchable = mustRemainDynamicallyDispatchable;
        info.kind = kind;
        TranslatedFunctionRegistry::Register(std::move(info));
    }
};

#define MKW_DETAIL_CAT(a, b) a##b
#define MKW_DETAIL_MAKE_UNIQUE(a, b) MKW_DETAIL_CAT(a, b)

// Three hand-written registration macros. Generated code never registers per function; every
// translated function reaches the registry via the bulk BulkTranslatedFunctionRecord tables
// in the translator's *_registration TUs.
#define REGISTER_TRANSLATED_FUNCTION(address, fn) \
    static AbiTrampoline<decltype(fn)> MKW_DETAIL_MAKE_UNIQUE(_abi_trampoline_, __COUNTER__)(address, #fn, fn, FunctionKind::BaseTranslated, false, kPpcAllNonvolatileFprMask, 0, 0, nullptr, KnownTranslatedCpuCall<address>::kMustRemainDynamicallyDispatchable)

#define REGISTER_NATIVE_FUNCTION(address, fn) \
    static AbiTrampoline<decltype(fn)> MKW_DETAIL_MAKE_UNIQUE(_abi_native_trampoline_, __COUNTER__)(address, #fn, fn, FunctionKind::Native, false, kPpcAllNonvolatileFprMask, 0, 0, &AbiRawCpuThunk<&fn>::Invoke)

#define REGISTER_NATIVE_FUNCTION_AS(address, fn, pretty_name) \
    static AbiTrampoline<decltype(fn)> MKW_DETAIL_MAKE_UNIQUE(_abi_native_trampoline_named_, __COUNTER__)(address, pretty_name, fn, FunctionKind::Native, false, kPpcAllNonvolatileFprMask, 0, 0, &AbiRawCpuThunk<&fn>::Invoke)

#include "indirect_call_site.h"
