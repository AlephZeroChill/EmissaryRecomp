#pragma once
// Project+'s Gecko codes in the SDK's SI response handling (fn_801E6C38, which PADRead reaches through SI), for the
// PADRead that replaces the SDK's (hle/input/pad.cpp): the SDK's SI path never runs here, so a C2 code hooked into
// it never ran either. On Dolphin they rewrite each controller's response word before PADRead hands the status to
// the game:
// - 0x801E6D24, "Hold Start to Pause" (with "Allow Pausing When Set to Off", 0x800505C8, which writes 2 to
//   0x80584084 in a match whose pause is off): while that word is 2, START reaches the game only once it has been
//   held 50 frames (a timer per channel at 0x80584080 + 8 * chan; 0 again when START is up or the game is
//   paused). Brawl Online's Ranked has no pause because its plugin sets those timers back to 0 every match frame;
//   without the code a START press paused a recomp at once, in Ranked and in every match whose pause is off.
// - 0x801E6D1C (P+'s debug mode's controls, 0x80583FFE bit 0): outside the debug mode, a START press sets the
//   halfword at 0x805A8A0A to 4 when it is not 0.
// They are emulated as written, only while guest memory holds the code (the code handler's branch at the hooked
// instruction, to a payload identical to P+'s), so another codeset, or the code switched off, changes nothing.
// Other C2 codes in fn_801E6C38 (P+'s debug mode, 0x801E6CD8) are not run; the first time one is seen it is logged.

#include "brawl_dirty_memory.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>

#include <dolphin/pad.h>

namespace PplusSiCodes {

inline uint32_t BranchTarget(uint32_t at) {
    const uint32_t w = Memory::Read32(at);
    if ((w & 0xFC000003u) != 0x48000000u) return 0;
    uint32_t off = w & 0x03FFFFFCu;
    if (off & 0x02000000u) off |= 0xFC000000u;
    return at + off;
}

template <size_t N>
inline bool Applied(uint32_t at, const uint32_t (&payload)[N]) {
    const uint32_t target = BranchTarget(at);
    if (target < 0x80000000u || target >= 0x81800000u) return false;
    for (size_t i = 0; i < N; ++i) {
        if (Memory::Read32(target + uint32_t(i * 4)) != payload[i]) return false;
    }
    return true;
}

// The payloads up to their last instruction (the handler's branch back follows).
inline constexpr uint32_t kHoldStartToPause[] = {
    0x3D608058, 0x616B4080, 0x818B0004, 0x2C0C0002, 0x40820048, 0x818DBCB0, 0x818C0018, 0x818C003C,
    0x2C0C0001, 0x4182002C, 0x540C00C7, 0x41820024, 0x7D8B202E, 0x2C0C0032, 0x40800020, 0x398C0001,
    0x7D8B212E, 0x3D801000, 0x7C0C0050, 0x4800000C, 0x39800000, 0x7D8B212E, 0x901E0000};
inline constexpr uint32_t kDebugStart[] = {
    0x7C03202E, 0x3CC08058, 0x60C63FFE, 0xA0A60000, 0x54A507FF, 0x3CC0805B, 0x60C68A0A, 0xA0A60000,
    0x40820010, 0x540700C7, 0x41A20038, 0x48000024, 0x3CE01000, 0x7CE738F8, 0x7C003839, 0x74070408,
    0x3D000408, 0x7C074000, 0x40A20018, 0x64001000, 0x2C050000, 0x41A2000C, 0x38A00004, 0xB0A60000};

constexpr uint32_t kHoldTimers = 0x80584080u;  // u32 per channel, 8 bytes apart; the word at +4
constexpr uint32_t kDebugFlags = 0x80583FFEu;
constexpr uint32_t kStartFlag = 0x805A8A0Au;
constexpr uint32_t kPauseState = 0x805A4420u - 0x4350u;  // r13 - 0x4350: the pause's state, +0x18, +0x3C = 1 paused

inline bool Paused() {
    const uint32_t p = Memory::Read32(kPauseState);
    if (p < 0x80000000u || p >= 0x81800000u) return false;
    const uint32_t q = Memory::Read32(p + 0x18);
    return q >= 0x80000000u && q < 0x81800000u && Memory::Read32(q + 0x3C) == 1;
}

inline void Write32(uint32_t addr, uint32_t v) {
    BrawlDirtyMemory::Mark(addr, 4);
    Memory::Write32(addr, v);
}

// Every channel's status as the SDK's PADRead would have it after those codes; the order is theirs.
inline void Apply(PADStatus* statuses) {
    static bool reported = false;
    if (!reported && BranchTarget(0x801E6CD8u) != 0) {
        reported = true;
        std::fputs("[pad] a Gecko code hooks SI at 0x801E6CD8 (P+'s debug mode): this PADRead does not run it\n", stderr);
    }
    const bool debugStart = Applied(0x801E6D1Cu, kDebugStart);
    const bool hold = Applied(0x801E6D24u, kHoldStartToPause);
    if (!debugStart && !hold) return;
    for (uint32_t chan = 0; chan < PAD_CHANMAX; ++chan) {
        PADStatus& s = statuses[chan];
        const bool connected = s.err == PAD_ERR_NONE;
        bool start = connected && (s.button & PAD_BUTTON_START) != 0;
        if (debugStart && start && (Memory::Read16(kDebugFlags) & 1) == 0 && Memory::Read16(kStartFlag) != 0) {
            BrawlDirtyMemory::Mark(kStartFlag, 2);
            Memory::Write16(kStartFlag, 4);
        }
        if (!hold || Memory::Read32(kHoldTimers + 4) != 2) continue;
        const uint32_t timer = kHoldTimers + 8 * chan;
        if (Paused() || !start) {
            Write32(timer, 0);
            continue;
        }
        const uint32_t t = Memory::Read32(timer);
        if (t >= 50) continue;
        Write32(timer, t + 1);
        s.button = uint16_t(s.button & ~PAD_BUTTON_START);
    }
}

}  // namespace PplusSiCodes
