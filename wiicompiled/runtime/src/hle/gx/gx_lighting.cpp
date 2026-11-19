// gx_lighting.cpp - Lighting and Channel Control
#include "gx_internal.h"
#include "runtime_log.h"
bool SpmAuroraDlRecording();  // gx_init.cpp: material lists are recorded at map load

#include <cstdio>
#include <cstdlib>

namespace {

// GXLightObj is an opaque 64-byte guest object. The Revolution SDK leaves
// three words at the front unused, then stores the exact 13 XF light words
// loaded by GXLoadLightObjImm. Aurora's host-side GXLightObj implementation is
// compact (it starts at color), so a guest pointer must never be passed to the
// host implementation directly.
constexpr uint32_t kLightObjSize = 0x40;
constexpr uint32_t kColorOffset = 0x0c;
constexpr uint32_t kAttnAOffset = 0x10;
constexpr uint32_t kAttnKOffset = 0x1c;
constexpr uint32_t kPositionOffset = 0x28;
constexpr uint32_t kDirectionOffset = 0x34;

bool ValidateGuestLightObj(uint32_t addr, const char* operation) {
    if (addr != 0 && Memory::Contains(addr, kLightObjSize)) {
        return true;
    }
    RT_LOGF(RT_TAG_GX, "%s: invalid GXLightObj @0x%08X\n", operation, addr);
    std::fflush(stderr);
    return false;
}

float ReadGuestLightFloat(uint32_t addr, uint32_t offset) {
    return Memory::ReadFloat32(addr + offset);
}

void InitializeHostLightFromGuest(GXLightObj& host, uint32_t guestAddr) {
    GXInitLightColor(&host, DecodeGxColor(Memory::Read32(guestAddr + kColorOffset)));
    GXInitLightAttn(
        &host,
        ReadGuestLightFloat(guestAddr, kAttnAOffset + 0),
        ReadGuestLightFloat(guestAddr, kAttnAOffset + 4),
        ReadGuestLightFloat(guestAddr, kAttnAOffset + 8),
        ReadGuestLightFloat(guestAddr, kAttnKOffset + 0),
        ReadGuestLightFloat(guestAddr, kAttnKOffset + 4),
        ReadGuestLightFloat(guestAddr, kAttnKOffset + 8));
    GXInitLightPos(
        &host,
        ReadGuestLightFloat(guestAddr, kPositionOffset + 0),
        ReadGuestLightFloat(guestAddr, kPositionOffset + 4),
        ReadGuestLightFloat(guestAddr, kPositionOffset + 8));

    // GXInitLightDir negates its arguments before storing them. The guest
    // object already contains those stored XF direction components.
    GXInitLightDir(
        &host,
        -ReadGuestLightFloat(guestAddr, kDirectionOffset + 0),
        -ReadGuestLightFloat(guestAddr, kDirectionOffset + 4),
        -ReadGuestLightFloat(guestAddr, kDirectionOffset + 8));
}

} // namespace

// ============================================================================
// Light Object Load
// ============================================================================

// The XF packet GXLoadLightObjImm writes for the guest object, built from its bytes: 0x10, the register word (light
// idx's 16 words at 0x600 + 0x10 * idx), three zero words, then the object's 13 words from +0x0C (colour, cosine and
// distance attenuation, position, direction) - big-endian in guest memory as in the FIFO. The component path reads
// them into a host GXLightObj (the colour word split into bytes and packed back; the floats by their bits; the
// direction negated once here and once by GXInitLightDir) and writes them back in that order, so the bytes are the
// same; only for a light id of one bit (GXLoadLightObjImm's log2 of it is then that bit's index). Each load was 13
// guest reads, four Aurora setters and 17 FIFO writes (the count per frame is on the [vi] gx raw begins line).
static bool BuildLightPacket(uint32_t la, uint32_t lid, uint8_t (&packet)[69]) {
    if (lid == 0 || (lid & (lid - 1)) != 0) return false;
    const auto* src = static_cast<const uint8_t*>(GuestToHostPtr(la + kColorOffset, 52));
    if (src == nullptr) return false;
    const uint32_t reg = (0x600u + 0x10u * static_cast<uint32_t>(__builtin_ctz(lid))) | (0xFu << 16);
    packet[0] = 0x10;
    packet[1] = static_cast<uint8_t>(reg >> 24);
    packet[2] = static_cast<uint8_t>(reg >> 16);
    packet[3] = static_cast<uint8_t>(reg >> 8);
    packet[4] = static_cast<uint8_t>(reg);
    std::memset(packet + 5, 0, 12);
    std::memcpy(packet + 17, src, 52);
    return true;
}

extern "C" void GX__LoadLightObjImm_HLE(uint32_t la, uint32_t lid) {
    G3D_REC_HLE(g3dn::kRecLoadLightObj, la, lid);
    if (!ValidateGuestLightObj(la, "GXLoadLightObjImm")) return;
    uint8_t packet[69];
    const int mode = GxRawPacketMode();
    const bool built = mode != 0 && BuildLightPacket(la, lid, packet);
    if (built && mode == 1) {
        GxAuroraFifoWrite(packet, sizeof packet);  // this thread's FIFO (the draw worker's: its job's)
        if (!t_gxDrawWorker) ++g_gxLightPackets;
        return;
    }
    GXLightObj host{};
    InitializeHostLightFromGuest(host, la);
    GXLoadLightObjImm(&host, static_cast<GXLightID>(lid));
    namespace d = aurora::gx::fifo::detail;
    if (built && !d::sInDisplayList) {  // GX_RAW_BEGIN=2: the packet against the bytes just written
        GxNoteLightPacketCheck(d::sBufferSize >= sizeof packet &&
                               std::memcmp(d::sBufferData + d::sBufferSize - sizeof packet, packet, sizeof packet) == 0);
    }
}
PPC_NATIVE_OVERRIDE_VOID(801F2418, GX__LoadLightObjImm_HLE, (uint32_t la, uint32_t lid), (la, lid));

// ============================================================================
// Channel Control
// ============================================================================

extern "C" void GX__SetChanAmbColor_HLE(uint32_t c, uint32_t cp) {
    EnsureAuroraFrameActive();
    GXColor color = DecodeGxColor(Memory::Read32(cp));
    GXSetChanAmbColor((GXChannelID)c, color);
}
PPC_NATIVE_OVERRIDE_VOID(801f2494, GX__SetChanAmbColor_HLE, (uint32_t c, uint32_t cp), (c, cp));

extern "C" void GX__SetChanMatColor_HLE(uint32_t c, uint32_t cp) {
    EnsureAuroraFrameActive();
    GXColor color = DecodeGxColor(Memory::Read32(cp));
    GXSetChanMatColor((GXChannelID)c, color);
}
PPC_NATIVE_OVERRIDE_VOID(801F256C, GX__SetChanMatColor_HLE, (uint32_t c, uint32_t cp), (c, cp));

extern "C" void GX__SetNumChans_HLE(uint32_t n) { GXSetNumChans((u8)n); }
PPC_NATIVE_OVERRIDE_VOID(801f2644, GX__SetNumChans_HLE, (uint32_t n), (n));

extern "C" void GX__SetChanCtrl_HLE(uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af) {
    GXSetChanCtrl((GXChannelID)ch, en!=0, (GXColorSrc)as, (GXColorSrc)ms, lm, (GXDiffuseFn)df, (GXAttnFn)af);
}
PPC_NATIVE_OVERRIDE_VOID(801F2668, GX__SetChanCtrl_HLE, (uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af), (ch, en, as, ms, lm, df, af));
