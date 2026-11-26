#include "brawl_dirty_memory.h"
#pragma once

// Per-granule write-generation counters let GX cache consumers skip re-digesting unchanged
// ranges. Assumes GPU visibility only happens via DCStoreRange/DCFlushRange/DCInvalidateRange,
// so hooking those plus DMA writers is a complete notification. Header-only: hot, must not
// link-depend on the GX HLE.

#include "memory.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Canonical MEM1/MEM2 physical address for any of the guest's cached, uncached
// or physical aliases. Lives here rather than in the GX HLE so the tracking
// table, its notifiers and the GX caches all agree on one address space.
inline uint32_t CanonicalizeGxMainRamAddress(uint32_t addr) noexcept {
    if (addr < 0x01800000u) {
        return addr;
    }
    if (addr >= Memory::kMem2PhysicalBase && addr < Memory::kMem2PhysicalEnd) {
        return addr;
    }
    if (addr >= 0x80000000u && addr < 0x81800000u) {
        return addr - 0x80000000u;
    }
    if (addr >= Memory::kMem2CachedBase && addr < Memory::kMem2CachedEnd) {
        return addr - 0x80000000u;
    }
    if (addr >= 0xC0000000u && addr < 0xC1800000u) {
        return addr - 0xC0000000u;
    }
    if (addr >= Memory::kMem2UncachedBase && addr < Memory::kMem2UncachedEnd) {
        return addr - 0xC0000000u;
    }
    return addr;
}

namespace GxGuestWrite {

// Granularity matches the GX resource caches' occupancy maps: 64 KiB, so a
// cacheable display list or texture spans only a handful of counters. A false
// bump only costs one re-digest, which is exactly the untracked behaviour.
inline constexpr uint32_t kGranuleShift = 16; // 64 KiB per granule
inline constexpr uint64_t kTrackedSpan = static_cast<uint64_t>(Memory::kMem2PhysicalEnd);
inline constexpr size_t kGranuleCount = static_cast<size_t>(kTrackedSpan >> kGranuleShift);

// Folded value used for "this range is not covered by the granule map", which
// forces the consumer to recompute its digest on every call.
inline constexpr uint64_t kUntracked = ~0ull;

// The 4 KiB pages of guest RAM some consumer has taken a generation over (the getters below mark theirs), or is going to
// on another thread: the guest thread marks a texture's or palette's bytes when it hands Aurora the object
// (NoteTrackedRange, gx_texture.cpp), and the display lists a draw worker replays when it records the draw (their
// FineGenerationForRange, gx_dl.cpp GX_HLE_ApplyListVertexState). Sticky: a page is never unmarked. A write whose
// range has no marked page cannot touch any range a consumer trusts, so the guest thread's DMA and data-cache
// notification (gx_objects.cpp GxNotifyGuestRamDmaWrite) skips its counter bumps there (AnyTracked) - the locked
// increments that were most of its cost on the per-model matrix flushes. Exact for a consumer that marks on the
// notifying thread (program order), and for one on another thread when the guest thread marked its range before the
// hand-off its reads follow (a write after the mark then bumps; one before it is visible to the reads).
inline constexpr uint32_t kTrackedPageShift = 12;
inline constexpr size_t kTrackedPageCount = static_cast<size_t>(kTrackedSpan >> kTrackedPageShift);
inline std::array<std::atomic<uint64_t>, (kTrackedPageCount + 63) / 64> g_trackedPages{};

// [start, end): canonical, start < end <= kTrackedSpan. Pages already marked cost a load per 64.
inline void MarkTrackedPages(uint64_t start, uint64_t end) noexcept {
    const size_t first = static_cast<size_t>(start >> kTrackedPageShift);
    const size_t last = static_cast<size_t>((end - 1) >> kTrackedPageShift);
    const size_t lastWord = last >> 6;
    uint64_t mask = ~0ull << (first & 63);
    for (size_t w = first >> 6;; ++w, mask = ~0ull) {
        if (w == lastWord) {
            mask &= ~0ull >> (63 - (last & 63));
        }
        if ((g_trackedPages[w].load(std::memory_order_relaxed) & mask) != mask) {
            g_trackedPages[w].fetch_or(mask, std::memory_order_relaxed);
        }
        if (w == lastWord) {
            return;
        }
    }
}

// Marks [addr, addr + size) (any alias) for a consumer that will take its generation on another thread.
inline void NoteTrackedRange(uint32_t addr, uint32_t size) noexcept {
    if (size == 0) {
        return;
    }
    const uint64_t start = static_cast<uint64_t>(CanonicalizeGxMainRamAddress(addr));
    if (start >= kTrackedSpan) {
        return;
    }
    const uint64_t end = start + static_cast<uint64_t>(size);
    MarkTrackedPages(start, end < kTrackedSpan ? end : kTrackedSpan);
}

// Whether NotifyWrite(addr, size) could bump a counter some consumer depends on: a marked page among those it bumps.
inline bool AnyTracked(uint32_t addr, uint32_t size) noexcept {
    if (size == 0) {
        return false;
    }
    const uint64_t start = static_cast<uint64_t>(CanonicalizeGxMainRamAddress(addr));
    if (start >= kTrackedSpan) {
        return false;  // NotifyWrite bumps nothing there
    }
    const uint64_t end = start + static_cast<uint64_t>(size);
    const size_t first = static_cast<size_t>(start >> kTrackedPageShift);
    const size_t last = static_cast<size_t>(((end < kTrackedSpan ? end : kTrackedSpan) - 1) >> kTrackedPageShift);
    const size_t lastWord = last >> 6;
    uint64_t mask = ~0ull << (first & 63);
    for (size_t w = first >> 6;; ++w, mask = ~0ull) {
        if (w == lastWord) {
            mask &= ~0ull >> (63 - (last & 63));
        }
        if ((g_trackedPages[w].load(std::memory_order_relaxed) & mask) != 0) {
            return true;
        }
        if (w == lastWord) {
            return false;
        }
    }
}

// Shared (not thread_local) because notifications come from the OS HLE and from
// aurora's readback callbacks while the caches themselves are per guest thread.
// A bump is a release (after the guest bytes it announces) and each fold ends with
// an acquire fence (before the consumer reads those bytes): otherwise a weakly
// ordered CPU (ARM: the Switch) can show a consumer the new generation with the old
// bytes, and it caches the old digest under the new generation until the next write.
// On x86 both compile to what relaxed did (lock xadd; no fence instruction).
inline std::array<std::atomic<uint32_t>, kGranuleCount> g_generations{};

// Monotone fold of every granule counter covering [addr, addr + nbytes).
// Counters only ever increase, so the sum changes whenever any covered granule
// is bumped and can never alias back to a previously observed value.
inline uint64_t GenerationForRange(uint32_t addr, uint32_t nbytes) noexcept {
    if (nbytes == 0) {
        return kUntracked;
    }
    const uint64_t start = static_cast<uint64_t>(CanonicalizeGxMainRamAddress(addr));
    const uint64_t end = start + static_cast<uint64_t>(nbytes);
    if (end <= start || end > kTrackedSpan) {
        return kUntracked;
    }
    MarkTrackedPages(start, end);  // before the counters are read
    const size_t firstGranule = static_cast<size_t>(start >> kGranuleShift);
    const size_t lastGranule = static_cast<size_t>((end - 1) >> kGranuleShift);
    uint64_t folded = 0;
    for (size_t granule = firstGranule; granule <= lastGranule; ++granule) {
        folded += g_generations[granule].load(std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_acquire);  // the bytes read after this are at least this new
    // Never collide with the "untracked" sentinel.
    return folded == kUntracked ? folded - 1u : folded;
}

// The same counters at 4 KiB, for a consumer whose ranges share 64 KiB granules with memory rewritten every frame: the
// deferred native draw's display-list mirror (gx_dl.cpp GX_HLE_ApplyListVertexState), whose model shape lists sit
// next to the material lists animation rewrites and flushes each frame, and the display-list scan cache
// (gx_dl.cpp ProbeDlScanCache). Bumped by the same notifications.
inline constexpr uint32_t kFineGranuleShift = 12; // 4 KiB per granule
inline constexpr size_t kFineGranuleCount = static_cast<size_t>(kTrackedSpan >> kFineGranuleShift);
inline std::array<std::atomic<uint32_t>, kFineGranuleCount> g_fineGenerations{};

// GenerationForRange on the fine counters.
inline uint64_t FineGenerationForRange(uint32_t addr, uint32_t nbytes) noexcept {
    if (nbytes == 0) {
        return kUntracked;
    }
    const uint64_t start = static_cast<uint64_t>(CanonicalizeGxMainRamAddress(addr));
    const uint64_t end = start + static_cast<uint64_t>(nbytes);
    if (end <= start || end > kTrackedSpan) {
        return kUntracked;
    }
    MarkTrackedPages(start, end);  // before the counters are read
    const size_t firstGranule = static_cast<size_t>(start >> kFineGranuleShift);
    const size_t lastGranule = static_cast<size_t>((end - 1) >> kFineGranuleShift);
    uint64_t folded = 0;
    for (size_t granule = firstGranule; granule <= lastGranule; ++granule) {
        folded += g_fineGenerations[granule].load(std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_acquire);  // as GenerationForRange
    return folded == kUntracked ? folded - 1u : folded;
}

// Bumps every granule covering [addr, addr + size). Deliberately branch-light:
// this runs on every DC range op, including the ones that only ever touch
// memory no GX cache has ever looked at.
inline void NotifyWrite(uint32_t addr, uint32_t size) noexcept {
    BrawlDirtyMemory::Mark(addr,size);
    if (size == 0) {
        return;
    }
    const uint64_t start = static_cast<uint64_t>(CanonicalizeGxMainRamAddress(addr));
    const uint64_t end = start + static_cast<uint64_t>(size);
    if (end <= start || start >= kTrackedSpan) {
        return;
    }
    const uint64_t clampedEnd = end < kTrackedSpan ? end : kTrackedSpan;
    const size_t firstGranule = static_cast<size_t>(start >> kGranuleShift);
    const size_t lastGranule = static_cast<size_t>((clampedEnd - 1) >> kGranuleShift);
    for (size_t granule = firstGranule; granule <= lastGranule; ++granule) {
        g_generations[granule].fetch_add(1u, std::memory_order_release);
    }
    const size_t firstFine = static_cast<size_t>(start >> kFineGranuleShift);
    const size_t lastFine = static_cast<size_t>((clampedEnd - 1) >> kFineGranuleShift);
    for (size_t granule = firstFine; granule <= lastFine; ++granule) {
        g_fineGenerations[granule].fetch_add(1u, std::memory_order_release);
    }
}

// The skip contract shared by every consumer: a stored digest may be trusted
// only when the range is tracked at all and no notification has landed on it
// since the digest was taken.
inline bool CanSkipDigest(uint64_t storedGeneration, uint64_t currentGeneration) noexcept {
    return currentGeneration != kUntracked && storedGeneration == currentGeneration;
}

// Host-to-guest inversion for one contiguous guest RAM alias. Aurora holds host
// pointers only, so the generation and notification hooks it calls have to map
// them back before they can touch the table.
inline bool HostRangeToGuest(const void* hostBase, uint64_t hostSize, uint32_t guestBase,
                             const void* hostPtr, size_t size, uint32_t& outAddr) noexcept {
    if (hostBase == nullptr || hostPtr == nullptr || size == 0) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(hostBase);
    const uintptr_t ptr = reinterpret_cast<uintptr_t>(hostPtr);
    if (ptr < base) {
        return false;
    }
    const uint64_t offset = static_cast<uint64_t>(ptr - base);
    if (offset >= hostSize || static_cast<uint64_t>(size) > hostSize - offset) {
        return false;
    }
    outAddr = guestBase + static_cast<uint32_t>(offset);
    return true;
}

// NoteTrackedRange for a host pointer into guest RAM (an Aurora object's data), its size clamped to the bank; nothing
// where aurora's hooks cannot map it either (they then answer kUntracked). False when nothing was marked. Defined in
// gx_guest_write_hooks.cpp.
bool NoteTrackedHostRange(const void* hostPtr, size_t size) noexcept;

// Installs the generation/notification hooks into aurora. Called once after
// aurora_initialize; until then (and if guest RAM cannot be resolved) aurora
// digests its source bytes on every validation, which is the old behaviour.
// Defined in gx_guest_write_hooks.cpp.
void InstallAuroraHooks();

} // namespace GxGuestWrite
