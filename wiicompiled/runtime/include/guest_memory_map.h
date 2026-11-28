// The guest's memory map: the RAM banks, and every guest address range that reaches each, as Dolphin decodes them
// (Core/HW/Memmap.cpp's banks, PowerPC/MMU.cpp's physical decode, and the BATs Boot_BS2Emu.cpp's SetupBAT gives a Wii
// title: 0x80000000 and 0xC0000000 onto physical 0, 0x90000000 and 0xD0000000 onto physical 0x10000000, 256 MiB each).
// The one table: Memory's regions (Memory::GetPointer, its page table, the interpreter) and the lean window's mappings
// (memory.cpp), the executable-write check, rollback's dirty-mark aliases and the snapshots derive from it. The
// translated code's own access path (memory_access.h) is not built from it: the lean window's mask and the flat
// guest view put the same bytes behind the same addresses.
//
// MEM1: Dolphin backs NextPowerOf2(24 MiB) = 32 MiB and masks a physical address below 0x08000000 with 0x01FFFFFF, so
// 0x80000000-0x87FFFFFF (and the uncached 0xC0000000-0xC7FFFFFF) are four copies of all 32 MiB, the 24-32 MiB tail
// included, while the game is told it has 24 MiB. MEM2: physical 0x10000000, 64 MiB. Locked cache: 256 KiB at
// 0xE0000000. Physical addresses (below 0x80000000) are unreachable on Dolphin with translation on; the recomp keeps
// MEM1's and MEM2's physical views, as it always had them (the lean window folds them onto the same bytes anyway).
// MEM2's bank runs 16 MiB past the 64 MiB the game sees: the recomp's own area, the translated modules' homes
// (hle/brawl_modules.cpp) and the runtime FST, which Dolphin does not have. It shares MEM2's backing, so every view of
// MEM2 is one contiguous host range.
#pragma once

#include <cstdint>

namespace GuestMemoryMap {

enum class Bank : uint8_t { Mem1, Mem2, LockedCache };

struct BankInfo {
    Bank bank;
    const char* name;
    uint32_t primary;      // its cached view's start: where code runs and executable ranges are registered
    uint32_t size;         // backed bytes
    uint32_t guestSize;    // the bytes the game is told about (Dolphin's GetRamSizeReal / GetExRamSizeReal)
    uint32_t privateFrom;  // bank offsets from here on are the recomp's own, with no Dolphin counterpart
};

inline constexpr BankInfo kBanks[] = {
    {Bank::Mem1, "MEM1", 0x80000000u, 0x02000000u, 0x01800000u, 0x02000000u},
    {Bank::Mem2, "MEM2", 0x90000000u, 0x05000000u, 0x04000000u, 0x04000000u},
    {Bank::LockedCache, "LOCKED_CACHE", 0xE0000000u, 0x00040000u, 0x00040000u, 0x00040000u},
};

constexpr const BankInfo& Info(Bank bank) { return kBanks[static_cast<int>(bank)]; }

// A guest address range [start, end) that reaches a bank: address `a` is bank offset (a - start) % the bank's size, so
// a view longer than its bank repeats it (MEM1's mirrors).
struct View {
    uint32_t start;
    uint32_t end;
    Bank bank;
    const char* name;
};

// The most used first (Memory's region lookup scans in this order).
inline constexpr View kViews[] = {
    {0x80000000u, 0x88000000u, Bank::Mem1, "MEM1"},
    {0x90000000u, 0x95000000u, Bank::Mem2, "MEM2"},
    {0xC0000000u, 0xC8000000u, Bank::Mem1, "MEM1_UNCACHED"},
    {0xD0000000u, 0xD5000000u, Bank::Mem2, "MEM2_UNCACHED"},
    {0xE0000000u, 0xE0040000u, Bank::LockedCache, "LOCKED_CACHE"},
    {0x00000000u, 0x08000000u, Bank::Mem1, "MEM1_PHYS"},
    {0x10000000u, 0x15000000u, Bank::Mem2, "MEM2_PHYS"},
};

// Where an address lands: its bank (null outside every view) and the offset in it.
struct Location {
    const BankInfo* bank = nullptr;
    uint32_t offset = 0;
};

constexpr Location Decode(uint32_t address) {
    for (const View& view : kViews)
        if (address >= view.start && address < view.end)
            return {&Info(view.bank), (address - view.start) % Info(view.bank).size};
    return {};
}

// The same bytes' address in their bank's primary view; an address outside every view is its own.
constexpr uint32_t Canonical(uint32_t address) {
    const Location at = Decode(address);
    return at.bank != nullptr ? at.bank->primary + at.offset : address;
}

// Every address that reaches `offset` of `bank`: f(address), view by view.
template <typename F>
constexpr void ForEachAlias(Bank bank, uint32_t offset, F&& f) {
    for (const View& view : kViews) {
        if (view.bank != bank) continue;
        for (uint64_t at = uint64_t{view.start} + offset; at < view.end; at += Info(bank).size)
            f(static_cast<uint32_t>(at));
    }
}

// Every bank-sized copy of every view, in table order: f(view, copy index, start address, bytes). Memory's regions are
// these, each one contiguous host range in every memory model.
template <typename F>
constexpr void ForEachCopy(F&& f) {
    for (const View& view : kViews) {
        uint32_t index = 0;
        for (uint64_t at = view.start; at < view.end; at += Info(view.bank).size, ++index)
            f(view, index, static_cast<uint32_t>(at), Info(view.bank).size);
    }
}

namespace detail {
constexpr bool WellFormed() {
    for (int b = 0; b < static_cast<int>(sizeof(kBanks) / sizeof(kBanks[0])); ++b) {
        const BankInfo& bank = kBanks[b];
        if (static_cast<int>(bank.bank) != b || bank.size % 0x10000u != 0 || bank.guestSize > bank.size ||
            bank.privateFrom < bank.guestSize || bank.privateFrom > bank.size)
            return false;
        const Location primary = Decode(bank.primary);
        if (primary.bank != &bank || primary.offset != 0) return false;
    }
    for (const View& view : kViews) {
        // Whole copies, 64 KiB aligned (the lean window's backing chunks, the flat build's allocation granule).
        if (view.start % 0x10000u != 0 || view.end <= view.start ||
            (uint64_t{view.end} - view.start) % Info(view.bank).size != 0)
            return false;
        for (const View& other : kViews)
            if (&other != &view && view.start < other.end && other.start < view.end) return false;
    }
    return true;
}
static_assert(WellFormed(), "guest memory map: banks and views must be whole, aligned and disjoint");
static_assert(Decode(0x8348B5C8u).offset == 0x0148B5C8u, "MEM1's mirrors repeat it every 32 MiB");
static_assert(Canonical(0xC7F00000u) == 0x81F00000u, "the uncached mirrors reach MEM1's tail");
}  // namespace detail

}  // namespace GuestMemoryMap
