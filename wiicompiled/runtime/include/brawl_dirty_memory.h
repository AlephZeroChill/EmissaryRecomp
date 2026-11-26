#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// MKW_LEAN_DIRTY_MARKS=0 (lean memory only): no guest store writes the dirty map (the per-store 16-bit mark, range
// marks, the helpers below). Its one reader that matters is rollback's WriteWatch, and brawl_rollback.cpp refuses to
// track writes in such a build. For the vanilla EABI translation, which runs neither rollback (its hooks need the
// interpreted, mod-patched main loop) nor Gecko codes, the marks are ~2-3 host instructions per guest store that
// nothing reads. Natives that write or compare marks themselves (g3d_calc, mtx BRAWL_HLE_VERIFY) still do, so their
// verify modes report mark differences in such a build: verify with marks on. Default on.
#ifndef MKW_LEAN_DIRTY_MARKS
#define MKW_LEAN_DIRTY_MARKS 1
#endif

// Portable guest write tracking. Aliases share a Page allocated by the match tracker. Publication and removal happen
// at the simulation's snapshot safe point.
namespace BrawlDirtyMemory {

struct Page {
    std::atomic<uint8_t> touched{0};
    std::atomic<uint8_t> blocks[1024]{};
};

inline std::atomic<bool> active{false};
inline Page* pages[65536]{};

// Bumped before anything clears marks or rewrites guest memory without marking it (a rollback or save-state load, a
// verifier putting memory back). Between bumps a mark only goes from clear to set, so code that matched guest memory in
// the current epoch and whose marks are still clear has not been written since: the interpreter's block cache checks
// that instead of comparing the code on every entry.
inline std::atomic<uint32_t> markEpoch{1};
inline void MarksCleared() {
    markEpoch.fetch_add(1, std::memory_order_acq_rel);
}

// Rollback's WriteWatch consumes the marks of the pages it tracks (scans then clears them, writers quiescent) and
// restores those pages from snapshots; consumeSeq counts the consumes, and a snapshot keeps the count it was taken at.
// The interpreter's code caches set these hooks so that neither needs a new epoch: before a consume they retire the
// code under set marks (noting the count), and after a restore to a snapshot they retire the code written since it
// (and code built since). Without hooks, both start a new epoch.
inline std::atomic<uint32_t> consumeSeq{0};
inline void (*consumingMarks)(uint32_t seq) = nullptr;
inline void (*restoredMarks)(uint32_t seq) = nullptr;

inline void MarksConsuming() {
    const uint32_t seq = consumeSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (consumingMarks)
        consumingMarks(seq);
    else
        MarksCleared();
}

inline void MarksRestored(uint32_t snapshotSeq) {
    if (restoredMarks)
        restoredMarks(snapshotSeq);
    else
        MarksCleared();
}

// Lean memory: the 64 KiB pages (of the 1 GiB canonical window) whose marks rollback's WriteWatch consumes. Nothing
// reads the marks of any other page, so the interpreter clears those of the code it checks there itself.
inline std::atomic<uint8_t> trackedPages[1u << 14]{};

// Out-of-line helpers stay cold, so the code around a store that reaches them keeps its registers.
#if defined(_MSC_VER)
#define BRAWL_DIRTY_MEMORY_SLOW __declspec(noinline)
#else
#define BRAWL_DIRTY_MEMORY_SLOW __attribute__((noinline, cold))
#endif

// Marks every block of [address, address + size) on the tracked pages it touches.
BRAWL_DIRTY_MEMORY_SLOW inline void MarkSlow(uint32_t address, size_t size) {
    if (!active.load(std::memory_order_relaxed) || size == 0)
        return;
    while (size != 0) {
        const size_t offset = address & 65535u;
        const size_t count = size < 65536 - offset ? size : 65536 - offset;
        if (Page* page = pages[address >> 16]) {
            page->touched.store(1, std::memory_order_relaxed);
            for (size_t block = offset / 64; block <= (offset + count - 1) / 64; ++block)
                page->blocks[block].store(1, std::memory_order_relaxed);
        }
        address += uint32_t(count);
        size -= count;
    }
}

#if MKW_LEAN_MEMORY
// The lean memory model (memory_access.h) gives every guest byte one canonical offset, `address & 0x3FFFFFFF`, in a
// 1 GiB window, so its marks are one flat byte per 64 guest bytes over that window (leanBlocks, allocated by
// Memory::Init), with no page table or alias pointers. Every store marks unconditionally, tracked or not: a shift and
// one 16-bit store of 0x0101, no branch. That store also marks the following block, which covers a write of up to 64
// bytes that crosses into it; the scanner copies the extra block. WriteWatch scans and clears its pages' slices.
inline constexpr uint32_t kLeanDirtyMask = 0x3FFFFFFFu;
inline constexpr size_t kLeanDirtyBlocks = (size_t{1} << 30) / 64;
inline uint8_t* leanBlocks = nullptr; // kLeanDirtyBlocks + 1 bytes (the last block's mark spills one byte)

inline void LeanMarkBlockPair(uint32_t canonical) {
    const uint16_t both = 0x0101;
    std::memcpy(leanBlocks + (canonical >> 6), &both, sizeof(both));
}

BRAWL_DIRTY_MEMORY_SLOW inline void LeanMarkRange(uint32_t canonical, size_t size) {
    const size_t first = canonical >> 6;
    const size_t last = (size_t(canonical) + size - 1) >> 6;
    std::memset(leanBlocks + first, 1, (last < kLeanDirtyBlocks ? last : kLeanDirtyBlocks - 1) - first + 1);
}
#endif

// Flat and heap memory while a match is tracked: a write of up to 64 bytes (every guest store) marks its one or two
// blocks inline, with no call: MarkSlow's effect for it (the tracked page's touched flag and blocks; a block past the
// page's end is the next page's block 0). Out of line, the calls cost the game thread more than the marks themselves
// (MarkSlow and MarkHostSlow were ~18% of its cycles in a rollback sync test). Any call on this path, even a rarely
// taken one, gave up much of that.
inline void MarkSmall(uint32_t address, size_t size) {
    if (size == 0 || size > 64) [[unlikely]] {
        MarkSlow(address, size);
        return;
    }
    const uint32_t offset = address & 65535u;
    uint32_t last = offset + uint32_t(size) - 1;
    if (last > 65535u) [[unlikely]] {
        if (Page* next = pages[((address >> 16) + 1) & 0xFFFFu]) {
            next->touched.store(1, std::memory_order_relaxed);
            next->blocks[0].store(1, std::memory_order_relaxed);
        }
        last = 65535u;
    }
    if (Page* page = pages[address >> 16]) {
        page->touched.store(1, std::memory_order_relaxed);
        page->blocks[offset >> 6].store(1, std::memory_order_relaxed);
        page->blocks[last >> 6].store(1, std::memory_order_relaxed);
    }
}

inline void Mark(uint32_t address, size_t size) {
#if MKW_LEAN_MEMORY && !MKW_LEAN_DIRTY_MARKS
    (void)address;
    (void)size;
#elif MKW_LEAN_MEMORY
    if (size <= 64)
        LeanMarkBlockPair(address & kLeanDirtyMask);
    else if (size != 0)
        LeanMarkRange(address & kLeanDirtyMask, size);
#else
    if (active.load(std::memory_order_relaxed))
        MarkSmall(address, size);
#endif
}

// Bulk helpers holding a host backing pointer mark at the actual write, never when they take the pointer (which would
// also mark reads and miss caching).
struct HostSpan {
    uintptr_t start;
    size_t size;
    uint32_t guest;
};
inline uintptr_t flatBase = 0;
inline std::vector<HostSpan> hosts;

BRAWL_DIRTY_MEMORY_SLOW inline void MarkHostSlow(const void* pointer, size_t size) {
    if (!active.load(std::memory_order_relaxed) || size == 0)
        return;
    const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
    if (flatBase != 0 && start >= flatBase && start - flatBase <= UINT32_MAX) {
        Mark(uint32_t(start - flatBase), size);
        return;
    }
    for (const HostSpan& span : hosts) {
        const uintptr_t lo = start > span.start ? start : span.start;
        const uintptr_t hi = start + size < span.start + span.size ? start + size : span.start + span.size;
        if (lo < hi)
            Mark(span.guest + uint32_t(lo - span.start), hi - lo);
    }
}

inline void MarkHost(const void* pointer, size_t size) {
#if MKW_LEAN_MEMORY // flatBase is the lean window (Memory::Init): every guest host pointer lies inside it
    const uintptr_t offset = reinterpret_cast<uintptr_t>(pointer) - flatBase;
    if (offset < (uintptr_t{1} << 30))
        Mark(uint32_t(offset), size);
#else
    // A pointer into the flat guest window (flatBase, set by the tracker) marks inline, as Mark does; others look up
    // the registered host spans.
    if (active.load(std::memory_order_relaxed)) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(pointer);
        if (flatBase != 0 && start >= flatBase && start - flatBase <= UINT32_MAX) {
            MarkSmall(uint32_t(start - flatBase), size);
        } else {
            MarkHostSlow(pointer, size);
        }
    }
#endif
}

} // namespace BrawlDirtyMemory
