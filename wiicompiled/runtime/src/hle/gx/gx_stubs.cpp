#include <algorithm>
#include <chrono>
#include <cstdlib>
#include "gx_internal.h"
#include "runtime_log.h"
#include "hz_perf.h"
#include "g3d_native.h"

extern "C" void __GXSetSUTexRegs();

// ============================================================================
// FIFO Write Helpers
// ============================================================================

// g3d_native.h's recorder sees every guest FIFO byte as written (big-endian), whichever helper wrote it.
extern "C" void GX_HLE_FIFO_WriteFloat(float val) {
    u32 raw; std::memcpy(&raw, &val, 4);
    const uint8_t be[4] = {uint8_t(raw >> 24), uint8_t(raw >> 16), uint8_t(raw >> 8), uint8_t(raw)};
    g3dn::HleRec rec(g3dn::kRecFifo, be, 4);
    if (rec.skip) return;
    try { HleFifoWrite(raw, 4); } catch (...) { RT_LOGF(RT_TAG_GX, "FIFO write float failed\n"); }
}

extern "C" void GX_HLE_FIFO_Write32(uint32_t val) {
    const uint8_t be[4] = {uint8_t(val >> 24), uint8_t(val >> 16), uint8_t(val >> 8), uint8_t(val)};
    g3dn::HleRec rec(g3dn::kRecFifo, be, 4);
    if (rec.skip) return;
    HleFifoWrite(val, 4);
}
extern "C" void GX_HLE_FIFO_Write16(uint16_t val) {
    const uint8_t be[2] = {uint8_t(val >> 8), uint8_t(val)};
    g3dn::HleRec rec(g3dn::kRecFifo, be, 2);
    if (rec.skip) return;
    HleFifoWrite(static_cast<u32>(val), 2);
}
extern "C" void GX_HLE_FIFO_Write8(uint8_t val) {
    g3dn::HleRec rec(g3dn::kRecFifo, &val, 1);
    if (rec.skip) return;
    HleFifoWrite(static_cast<u32>(val), 1);
}

// Brawl's GXSetDrawSync (0x801F08D8) runs translated: its PE_TOKEN_INT/PE_TOKEN BP writes go down the gather pipe like
// any FIFO bytes, and its GXFlush half calls the HLE __GXSetDirtyState and PPCSync. Brawl's only GXReadDrawSync caller,
// a GPU watchdog (0x8003D834), ignores the token it reads through its own __peReg (0xCC001000).

extern "C" void OSWakeupThread_HLE(CpuContext* ctx);

// Brawl (RSBE01) GXFinishInterruptHandler globals: DrawDoneCB r13-0x3B5C, FinishQueue r13-0x3B68.
constexpr uint32_t kGxDrawDoneCallbackAddr = 0x805A08C4u;
constexpr uint32_t kGxFinishQueueAddr = 0x805A08B8u;

extern "C" void GX__FinishInterruptHandler_HLE() {
    try {
        uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) Memory::Write16(gd + 0x0Au, static_cast<uint16_t>(Memory::Read16(gd + 0x0Au) | 0x0008u));
        Memory::Write8(kGxDrawDoneFlagAddr, 1);
    } catch (...) {}
    // As the SDK handler: the game's draw-done callback, then wake GXWaitDrawDone sleepers. Brawl's frame
    // pacing (gfApplication's frame buffer slots) is released from that callback.
    if (CpuContext* cpu = TryGetCpuContext()) {
        if (const uint32_t cb = Memory::Read32(kGxDrawDoneCallbackAddr)) {
            CpuContext call = *cpu;
            InvokeIndirectCpu(cb, &call);
        }
        CpuContext wake = *cpu;
        wake.gpr[3] = kGxFinishQueueAddr;
        OSWakeupThread_HLE(&wake);
    }
}
PPC_NATIVE_OVERRIDE_VOID(801f0d2c, GX__FinishInterruptHandler_HLE, (), ());

// BRAWL_DRAWDONE_SITES=1: per call site (host return address as an offset into the executable, and the guest LR),
// how often GXDrawDone runs and how long it blocks, logged every 4800 calls (~10 s in a match). With Aurora's GX
// thread on, that block is the wait for the GX thread to finish decoding.
extern "C" char __executable_start __attribute__((weak));  // Horizon's linker script has none: 0 (absolute addresses)
static void NoteDrawDoneSite(const void* hostReturn, uint32_t guestLr, uint64_t nanos) {
    static const bool on = std::getenv("BRAWL_DRAWDONE_SITES") != nullptr;
    if (!on) return;
    struct Site { const void* host; uint32_t lr; uint64_t calls, nanos; };
    static Site sites[16];
    static uint64_t total = 0;
    for (auto& s : sites) {
        if (s.host == hostReturn || s.host == nullptr) { s.host = hostReturn; s.lr = guestLr; ++s.calls; s.nanos += nanos; break; }
    }
    if (++total % 4800 != 0) return;
    for (auto& s : sites) {
        if (s.host == nullptr) continue;
        RT_LOGF(RT_TAG_GX, "drawdone site +0x%lx lr 0x%08X: %llu calls, mean wait %.3f ms\n",
                static_cast<unsigned long>(static_cast<const char*>(s.host) - &__executable_start), s.lr,
                static_cast<unsigned long long>(s.calls), s.calls ? s.nanos / 1e6 / s.calls : 0.0);
        s.calls = s.nanos = 0;
    }
}

extern "C" void GX__DrawDone_HLE() {
    HZ_PHASE(kHzDrawDoneTicks);
    try { Memory::Write8(kGxDrawDoneFlagAddr, 0); } catch (...) {}
    const auto start = std::chrono::steady_clock::now();
    GXDrawDone();
    const CpuContext* cpu = TryGetCpuContext();
    NoteDrawDoneSite(__builtin_return_address(0), cpu ? cpu->lr : 0,
                     static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - start).count()));
    GX__FinishInterruptHandler_HLE();
}
PPC_NATIVE_OVERRIDE_VOID(801f0a30, GX__DrawDone_HLE, (), ());

extern "C" void GX__PixModeSync_HLE() {
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
    GXPixModeSync();
}
PPC_NATIVE_OVERRIDE_VOID(801f0af0, GX__PixModeSync_HLE, (), ());

// ============================================================================
// Hardware Revision / Thread Query - No-ops
// ============================================================================

extern "C" void __GX__InitRevisionBits_HLE() {}
PPC_NATIVE_OVERRIDE_VOID(801ed7d8, __GX__InitRevisionBits_HLE, (), ());

// ============================================================================
// Texture State Management - Aurora handles internally
// ============================================================================

extern "C" void __GX__SetSUTexRegs_HLE() {
    __GXSetSUTexRegs();
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(801f3294, __GX__SetSUTexRegs_HLE, (), ());

extern "C" void __GX__SetTmemConfig_HLE(uint32_t mode) {
    // TMEM layout configuration - Aurora manages internally
    (void)mode;
}
PPC_NATIVE_OVERRIDE_VOID(801F33FC, __GX__SetTmemConfig_HLE, (uint32_t mode), (mode));

extern "C" void __GX__FlushTextureState_HLE() {
    // BP texture state flush - Aurora handles via API
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(801f3b78, __GX__FlushTextureState_HLE, (), ());

// ============================================================================
// Copy Configuration - No-ops for features Aurora doesn't use
// ============================================================================

extern "C" void GX__SetDispCopyFrame2Field_HLE(uint32_t f) {
    GXSetDispCopyFrame2Field(f);
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFCFFFu) | ((f & 3u) << 12));
            Memory::Write32(gd + 0x24Cu, Memory::Read32(gd + 0x24Cu) & 0xFFFFCFFFu);
        }
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(801f16e4, GX__SetDispCopyFrame2Field_HLE, (uint32_t f), (f));

extern "C" void GX__SetCopyClamp_HLE(uint32_t c) {
    GXSetCopyClamp(static_cast<GXFBClamp>(c));
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) {
            const uint32_t clamp = c & 3u;
            Memory::Write32(gd + 0x23Cu, (Memory::Read32(gd + 0x23Cu) & 0xFFFFFFFCu) | clamp);
            Memory::Write32(gd + 0x24Cu, (Memory::Read32(gd + 0x24Cu) & 0xFFFFFFFCu) | clamp);
        }
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(801f1704, GX__SetCopyClamp_HLE, (uint32_t c), (c));

extern "C" void GX__ClearBoundingBox_HLE() {
    SpmBBoxClear();
    GXClearBoundingBox();
    try {
        const uint32_t gd = Memory::Read32(kGXDataPtrAddr);
        if (gd) Memory::Write16(gd + 2, 0);
    } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(801f1fb8, GX__ClearBoundingBox_HLE, (), ());

// GXReadBoundingBox (SPM 0x8028E26C, this SDK's order: left, top, right, bottom).
extern "C" void GX__ReadBoundingBox_SPM(uint32_t leftPtr, uint32_t topPtr, uint32_t rightPtr, uint32_t bottomPtr) {
    uint16_t l = 0, t = 0, r = 0, b = 0;
    SpmBBoxRead(l, t, r, b);
    {   // union with Aurora's CPU-projected box, which also sees display-list and FIFO draws
        u16 al = 1023, ar = 0, at = 1023, ab = 0;
        ::GXReadBoundingBox(&al, &ar, &at, &ab);
        if (al <= ar && at <= ab) {
            if (l > r || t > b) { l = al; r = ar; t = at; b = ab; }
            else { l = std::min<uint16_t>(l, al); r = std::max<uint16_t>(r, ar); t = std::min<uint16_t>(t, at); b = std::max<uint16_t>(b, ab); }
        }
    }
    try {
        if (leftPtr) Memory::Write16(leftPtr, l);
        if (topPtr) Memory::Write16(topPtr, t);
        if (rightPtr) Memory::Write16(rightPtr, r);
        if (bottomPtr) Memory::Write16(bottomPtr, b);
    } catch (const Memory::AccessViolation&) {
    }
}

// ============================================================================
// FIFO/State Management - No-ops
// ============================================================================

extern "C" void GX__SetDirtyState_HLE() {
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write32(gd + 0x5FCu, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(801f0e10, GX__SetDirtyState_HLE, (), ());

extern "C" void GX__ResetWriteGatherPipe_HLE() {
    // WPAR reset - not needed on host
}
PPC_NATIVE_OVERRIDE_VOID(801f058c, GX__ResetWriteGatherPipe_HLE, (), ());
