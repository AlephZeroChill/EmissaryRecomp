#include "gx_internal.h"
#include "gx_stream_common.h"
#include "gx_cp_decode.h"
#include "isa/big_endian.h"
#include "hz_perf.h"
#include "g3d_native.h"

// Opcode constants and the stream helpers this file shares with gx_dl.cpp /
// gx_vertex.cpp; see gx_stream_common.h.
using namespace GxCmd;
using namespace GxStream;

thread_local HleGxState g_hleGxState;  // per producer thread (a draw worker replays deferred draws: g3d_native.cpp)
thread_local bool t_gxVtxPending = false;  // gx_internal.h: the vertex-state carry
thread_local uint64_t t_gxBeginPublished = 0;  // gx_internal.h
thread_local bool t_gxBeginPublishMemoValid = false;  // gx_internal.h
extern "C" bool GX_HLE_VertexStatePending() { return t_gxVtxPending; }
bool SpmAuroraDlRecording();  // gx_init.cpp: Aurora records the guest's list
extern "C" bool GX_HLE_DisplayListRecording() { return IsDisplayListActive() || SpmAuroraDlRecording(); }
extern "C" void GX_HLE_SetVertexStatePending(bool pending) { t_gxVtxPending = pending; }

namespace aurora::gx::fifo {
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
const uint8_t* get_buffer_data();  // Aurora's producer FIFO (fifo.hpp): GX_RAW_BEGIN=2's comparison
uint32_t get_buffer_size();
}

void HleGxState::ResetVertex() {
    currentAttr = NextEnabledAttr(GX_VA_PNMTXIDX - 1);
    currentComp = 0;
}

GXAttr HleGxState::NextEnabledAttr(int startAttr) {
    for (int i = startAttr + 1; i < 26; ++i) {
        if (vtxDesc[i] != GX_NONE) {
            return static_cast<GXAttr>(i);
        }
    }
    return GX_VA_NULL;
}

int HleGxState::GetExpectedCompCount(GXAttr attr, const VtxAttrFmt& fmt) {
    // Attributes with no component layout (matrix indices, XF arrays, GX_VA_NBT)
    // count as one component here: the incremental parser advances one element
    // per raw stream item for them.
    return static_cast<int>(AttrCompCount(attr, fmt, 1u));
}

extern "C" void GXShadowSaveDirty(u32 out[2]);  // aurora GXManage.cpp: dirtyState, dirtyVAT

// The begin's first vertex store publishes the vertex state before GXBegin (below), right after GX__Begin_HLE
// published it. That second publication changes nothing when the state is the one GX__Begin_HLE published (same
// generation and format, in an active frame, gx_internal.h t_gxBeginPublished) and the dirty bits it can set -
// descriptors (8), formats (0x10) and this format's VAT bit - are all still set: its register fields are the same
// values written again (the format list adds GX_VA_POS_MTX_ARRAY..GX_VA_LIGHT_ARRAY, which have no VAT fields), and
// while the vertex state is pending, the take-back replays the begin's own logged publication to the same result (the
// replay keeps the dirty bits as they were). Every immediate-mode GXBegin (43 a frame in vanilla Training: HUD/effect
// quads) published twice; each publication is ~1,300 instructions (GXSetVtxDescv/GXSetVtxAttrFmtv).
static bool BeginPublicationCurrent(GXVtxFmt fmt) {
    if (static_cast<uint32_t>(fmt) >= GX_MAX_VTXFMT ||
        t_gxBeginPublished != (uint64_t{g_hleGxState.vtxStateGeneration} << 8 | static_cast<uint64_t>(fmt) << 1 | 1u)) {
        return false;
    }
    u32 dirty[2];
    GXShadowSaveDirty(dirty);
    return (dirty[0] & 0x18u) == 0x18u && ((dirty[1] >> fmt) & 1u) != 0;
}

static void ApplyAuroraVtxStateForRawBegin(GXVtxFmt fmt) {
    // SPM: never publish the GX_VA_NBT row. It has no VAT of its own, and publishing its default
    // (cnt 1, F32) after GX_VA_NRM turned Aurora's normals into NBT (60-byte vertices) while the
    // stream carried 36-byte ones, so every paper-character draw was dropped as truncated.
    PublishAuroraVtxState(fmt, AuroraVtxPublishOptions{/*includeNbt=*/false,
                                                       /*fmtLoopFirst=*/0,
                                                       /*fmtLoopLast=*/25});
}

// There is deliberately no indexed-aurora path here. Immediate-mode indexed
// draws are parsed incrementally, and aurora's indexed-array upload has to
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream above (GX_INDEX8/16 -> GX_DIRECT).

static uint32_t GetDirectAttrByteSizeForFifo(GXAttr attr, const VtxAttrFmt& fmt) {
    return DirectAttrByteSize(attr, fmt, /*matrixAttrIsOneByte=*/true, /*fallbackComps=*/1u);
}

static bool TryGetRawDirectFifoVertexSize(GXVtxFmt fmt, uint32_t& vertexSize) {
    vertexSize = 0;
    if (fmt >= GX_MAX_VTXFMT) {
        return false;
    }

    for (int attr = 0; attr < 26; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) {
            continue;
        }
        if (type != GX_DIRECT) {
            return false;
        }

        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const uint32_t attrBytes =
            GetDirectAttrByteSizeForFifo(gxAttr, g_hleGxState.vtxAttrFmt[fmt][attr]);
        if (attrBytes == 0) {
            return false;
        }
        vertexSize += attrBytes;
    }

    return vertexSize != 0;
}

static bool TrySubmitRawDirectFifoDraw(const uint8_t* packet, uint32_t packetBytes, GXPrimitive prim,
                                       GXVtxFmt vtxFmt, uint16_t vtxCount) {
    if (packet == nullptr || packetBytes == 0 || vtxCount == 0) {
        return false;
    }

    EnsureAuroraFrameActive();

    g_hleGxState.currentVtxFmt = vtxFmt;
    g_hleGxState.currentPrim = prim;
    g_hleGxState.vertsRemaining = vtxCount;
    g_hleGxState.inBegin = false;
    g_hleGxState.auroraBeginCalled = false;
    g_hleGxState.ResetVertex();

    ApplyAuroraVtxStateForRawBegin(vtxFmt);
    EnsureDefaultGxAlphaCompare();

    if (!aurora::gx::fifo::submit_raw_draw(prim, vtxFmt, packet + 3, vtxCount, packetBytes - 3u)) {
        return false;
    }
    GXMarkFrameWork();
    return true;
}

static void DecodeColorFromArray(uint32_t addr, GXCompType type, GXCompCnt cnt, GXColor& out) {
    // Gather exactly the bytes the shared decoder consumes. GX_RGBX8 is the one
    // format whose stream footprint (4 bytes) exceeds what is decoded (3), and
    // the pre-dedup code read only those 3 from the guest array - keep it that
    // way so a 3-byte tail at the end of an array cannot start faulting.
    const uint32_t byteCount =
        (type == GX_RGBX8) ? 3u : ColorByteSize(type, cnt);
    uint8_t bytes[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < byteCount && i < 4u; ++i) {
        bytes[i] = Memory::Read8(addr + i);
    }
    DecodeColorBytes(bytes, type, cnt, out);
}

void SubmitIndexedAttribute(GXAttr attr, uint32_t index) {
    const auto& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];
    const auto& arr = g_hleGxState.vtxArray[attr];
    if (arr.base == 0 || arr.stride == 0) {
        float comps[9]{};
        switch (attr) {
        case GX_VA_CLR0:
        case GX_VA_CLR1:
            if (fmt.cnt == GX_CLR_RGB) {
                GXColor3u8(0, 0, 0);
            } else {
                GXColor4u8(0, 0, 0, 0xFF);
            }
            break;
        case GX_VA_POS:
        case GX_VA_NRM:
        case GX_VA_TEX0:
        case GX_VA_TEX1:
        case GX_VA_TEX2:
        case GX_VA_TEX3:
        case GX_VA_TEX4:
        case GX_VA_TEX5:
        case GX_VA_TEX6:
        case GX_VA_TEX7:
            SubmitAttribute(attr, comps, fmt);
            break;
        default:
            break;
        }
        return;
    }
    const uint32_t baseAddr = arr.base + index * arr.stride;
    float comps[9]{};
    u32 rawComps[9]{};
    switch (attr) {
    case GX_VA_POS: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_POS, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_NRM: {
        const int count = static_cast<int>(AttrCompCount(GX_VA_NRM, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    case GX_VA_CLR0:
    case GX_VA_CLR1: {
        GXColor color{};
        DecodeColorFromArray(baseAddr, fmt.type, fmt.cnt, color);
        if (fmt.cnt == GX_CLR_RGB) {
            GXColor3u8(color.r, color.g, color.b);
        } else {
            GXColor4u8(color.r, color.g, color.b, color.a);
        }
        break;
    }
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7: {
        // Every GX_VA_TEXn shares one component layout, so the constant here is
        // exact for whichever of them `attr` is.
        const int count = static_cast<int>(AttrCompCount(GX_VA_TEX0, fmt, 0u));
        const int step = GetCompSizeBytes(fmt.type);
        uint32_t addr = baseAddr;
        for (int i = 0; i < count; ++i, addr += step) {
            comps[i] = ReadArrayComp(addr, fmt.type, fmt.frac);
            rawComps[i] = ReadArrayRawComp(addr, fmt.type);
        }
        SubmitAttribute(attr, comps, fmt, rawComps);
        break;
    }
    default:
        break;
    }
}

// Mirrors the raw-integer fast paths in SubmitAttribute below: for those
// (attr, type) pairs the float component buffer is never read, so the direct
// FIFO path can skip building it.
static bool SubmitAttributeReadsFloatComps(GXAttr attr, const VtxAttrFmt& fmt) {
    switch (attr) {
    case GX_VA_POS:
    case GX_VA_NRM:
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        break;
    default:
        return true;
    }
    switch (fmt.type) {
    case GX_U8:
    case GX_S8:
    case GX_U16:
    case GX_S16:
        return false;
    default:
        return true;
    }
}

void SubmitAttribute(GXAttr attr, float* comps, const VtxAttrFmt& fmt, const u32* rawComps) {
    if (attr == GX_VA_POS) {
        float bb[3] = {comps[0], comps[1], comps[2]};
        const int n = fmt.cnt == GX_POS_XY ? 2 : 3;
        if (rawComps && fmt.type != GX_F32) {
            for (int i = 0; i < n; ++i) bb[i] = ConvertCompToFloat(rawComps[i], fmt.type, fmt.frac);
        }
        SpmBBoxPosition(bb, n);
    }
    auto r8 = [rawComps](int idx) { return static_cast<u8>(rawComps[idx]); };
    auto rs8 = [rawComps](int idx) { return static_cast<s8>(rawComps[idx]); };
    auto r16 = [rawComps](int idx) { return static_cast<u16>(rawComps[idx]); };
    auto rs16 = [rawComps](int idx) { return static_cast<s16>(rawComps[idx]); };

    switch (attr) {
    case GX_VA_POS:
        if (rawComps) {
            if (fmt.cnt == GX_POS_XY) {
                switch (fmt.type) {
                case GX_U8: GXPosition2u8(r8(0), r8(1)); return;
                case GX_S8: GXPosition2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXPosition2u16(r16(0), r16(1)); return;
                case GX_S16: GXPosition2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXPosition3u8(r8(0), r8(1), r8(2)); return;
                case GX_S8: GXPosition3s8(rs8(0), rs8(1), rs8(2)); return;
                case GX_U16: GXPosition3u16(r16(0), r16(1), r16(2)); return;
                case GX_S16: GXPosition3s16(rs16(0), rs16(1), rs16(2)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_POS_XY) {
            GXPosition2f32(comps[0], comps[1]);
        } else {
            GXPosition3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_NRM:
        if (rawComps) {
            const bool nbt = fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3;
            const int groups = nbt ? 3 : 1;
            switch (fmt.type) {
            case GX_U8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u8(r8(i * 3), r8(i * 3 + 1), r8(i * 3 + 2));
                }
                return;
            case GX_S8:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s8(rs8(i * 3), rs8(i * 3 + 1), rs8(i * 3 + 2));
                }
                return;
            case GX_U16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3u16(r16(i * 3), r16(i * 3 + 1), r16(i * 3 + 2));
                }
                return;
            case GX_S16:
                for (int i = 0; i < groups; ++i) {
                    GXNormal3s16(rs16(i * 3), rs16(i * 3 + 1), rs16(i * 3 + 2));
                }
                return;
            default: break;
            }
        }
        if (fmt.cnt == GX_NRM_NBT || fmt.cnt == GX_NRM_NBT3) {
            for (int i = 0; i < 3; ++i) {
                GXNormal3f32(comps[i * 3], comps[i * 3 + 1], comps[i * 3 + 2]);
            }
        } else {
            GXNormal3f32(comps[0], comps[1], comps[2]);
        }
        break;
    case GX_VA_CLR0:
    case GX_VA_CLR1:
        if (fmt.cnt == GX_CLR_RGB) {
            GXColor3u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]));
        } else {
            GXColor4u8(static_cast<u8>(comps[0]), static_cast<u8>(comps[1]),
                       static_cast<u8>(comps[2]), static_cast<u8>(comps[3]));
        }
        break;
    case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3:
    case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7:
        if (rawComps) {
            if (fmt.cnt == GX_TEX_S) {
                switch (fmt.type) {
                case GX_U8: GXTexCoord1u8(r8(0)); return;
                case GX_S8: GXTexCoord1s8(rs8(0)); return;
                case GX_U16: GXTexCoord1u16(r16(0)); return;
                case GX_S16: GXTexCoord1s16(rs16(0)); return;
                default: break;
                }
            } else {
                switch (fmt.type) {
                case GX_U8: GXTexCoord2u8(r8(0), r8(1)); return;
                case GX_S8: GXTexCoord2s8(rs8(0), rs8(1)); return;
                case GX_U16: GXTexCoord2u16(r16(0), r16(1)); return;
                case GX_S16: GXTexCoord2s16(rs16(0), rs16(1)); return;
                default: break;
                }
            }
        }
        if (fmt.cnt == GX_TEX_S) {
            GXTexCoord1f32(comps[0]);
        } else {
            GXTexCoord2f32(comps[0], comps[1]);
        }
        break;
    default: break;
    }
}

// --- Immediate-mode begins as raw vertex bytes (GX_RAW_BEGIN, default on) ---------------------------------------------
// A begun draw's vertex bytes reach Aurora as the bytes its encoder would write. Each guest store into a begin was
// parsed into components (the ring push, the attribute walk), converted to floats or integers and handed to Aurora's
// immediate-mode calls (GXPosition3f32, GXTexCoord2u8...), whose encoder writes them back in the vertex format Aurora
// was given - the HLE format with indexed attributes as direct ones (PublishAuroraVtxState). For the formats below that
// round trip is the identity, so the bytes are the guest's own for a direct attribute and the array element's for an
// indexed one: here a vertex's guest bytes are gathered as they are stored, and the complete vertex written to Aurora's
// FIFO with its indexed attributes expanded from their arrays. Vanilla Training: 43 begins a frame (HUD and effect
// quads), 16 stores each, ~250 instructions a store and ~220 an Aurora call before.
//   Covered: matrix indices (one byte), positions and texture coordinates of type F32 (bit patterns, never converted),
// U8, S8, U16, S16 (passed as raw integers, which the encoder clamps to the same range), direct or indexed; colours
// RGBA8 with four components and RGB8 with three, direct or indexed. Any other attribute (normals, other colour
// formats, the array pseudo-attributes), SPM's frame dump, or the draw worker: the begin takes the component path.
//   Exactness: the FIFO bytes at GXEnd are the same, and nothing reads Aurora's FIFO before GXEnd drains it; Aurora's
// attribute writer is at the vertex start at every vertex end on both paths. The HLE parse state is the component
// path's at each vertex end (vertex start, nothing buffered). Should the vertex state change during a begin (no GX call
// may come between GXBegin and the vertices; checked at each vertex end), or an indexed element lie outside guest RAM,
// the vertex's gathered bytes go to the component path one at a time (its byte-by-byte equivalence, see
// HleFifoWriteBurstChunked) and the begin goes on there. GX_RAW_BEGIN=2 checks: the component path draws, and at each
// GXEnd the bytes Aurora holds for the begin are compared with the raw path's (counts in the [vi] g3d native lines).
namespace {
enum RawPartKind : uint8_t { kRawDirect, kRawMatrix, kRawIndex8, kRawIndex16 };
struct RawPart {
    uint8_t attr = 0;
    uint8_t kind = kRawDirect;
    uint8_t guestBytes = 0;  // in the guest's stream
    uint8_t elemBytes = 0;   // an indexed attribute's array element (what Aurora gets)
    uint8_t compBytes = 0;   // a numeric indexed attribute's component size; 0: a colour (read byte by byte)
    uint8_t comps = 0;
    GXCompType type = GX_F32;
};
struct RawBegin {
    bool active = false;  // this thread's begin takes the raw path (or, check mode, is gathered alongside)
    bool check = false;   // GX_RAW_BEGIN=2: gathered and compared, not drawn
    uint8_t nParts = 0;
    uint32_t vtxBytes = 0;  // one vertex in the guest's stream
    uint32_t have = 0;      // its bytes gathered so far
    uint32_t generation = 0;
    uint32_t vertsLeft = 0;  // check mode: vertices not yet gathered
    GXVtxFmt fmt = GX_VTXFMT0;
    RawPart parts[26];
    uint8_t vtx[256];
};
thread_local RawBegin t_rawBegin;
// GX_RAW_BEGIN: 0 off, 1 on (default), 2 check. Read at startup: the draw worker's light loads read it too.
const int s_rawBeginMode = [] {
    const char* v = std::getenv("GX_RAW_BEGIN");
    return v == nullptr ? 1 : std::atoi(v);
}();
// Game thread counters, read and cleared by the stats line.
uint64_t s_rawBegins = 0, s_rawFallbacks = 0, s_rawChecked = 0, s_rawCheckBad = 0, s_rawComponentBegins = 0;
uint32_t s_rawCheckStart = 0;            // check mode: Aurora's FIFO size right after GXBegin
std::vector<uint8_t> s_rawCheckBytes;    // check mode: the raw path's bytes for the begin

int RawBeginMode() { return s_rawBeginMode; }

// The begin's raw layout, when every enabled attribute is covered (above).
bool BuildRawLayout(RawBegin& r, GXVtxFmt fmt) {
    if (fmt >= GX_MAX_VTXFMT || SpmDumpFrame()) return false;
    r.nParts = 0;
    r.vtxBytes = 0;
    for (int a = 0; a < 26; ++a) {
        const GXAttrType type = g_hleGxState.vtxDesc[a];
        if (type == GX_NONE) continue;
        const GXAttr attr = static_cast<GXAttr>(a);
        RawPart p;
        p.attr = static_cast<uint8_t>(a);
        if (IsMatrixIndexAttr(attr)) {  // one byte whatever its type, as the component path reads it
            p.kind = kRawMatrix;
            p.guestBytes = 1;
        } else {
            if (type != GX_DIRECT && type != GX_INDEX8 && type != GX_INDEX16) return false;
            const VtxAttrFmt& f = g_hleGxState.vtxAttrFmt[fmt][a];
            const bool color = attr == GX_VA_CLR0 || attr == GX_VA_CLR1;
            const bool numeric = attr == GX_VA_POS || (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7);
            if (color) {
                if (f.cnt == GX_CLR_RGBA && f.type == GX_RGBA8) p.elemBytes = 4;
                else if (f.cnt == GX_CLR_RGB && f.type == GX_RGB8) p.elemBytes = 3;
                else return false;
            } else if (numeric) {
                if (f.type != GX_F32 && f.type != GX_U8 && f.type != GX_S8 && f.type != GX_U16 && f.type != GX_S16) {
                    return false;
                }
                p.comps = static_cast<uint8_t>(AttrCompCount(attr, f, 0u));
                p.compBytes = static_cast<uint8_t>(GetCompSizeBytes(f.type));
                p.type = f.type;
                p.elemBytes = static_cast<uint8_t>(p.comps * p.compBytes);
            } else {
                return false;  // normals, the array pseudo-attributes, GX_VA_NBT
            }
            if (type == GX_DIRECT) {
                p.kind = kRawDirect;
                p.guestBytes = p.elemBytes;
            } else {
                p.kind = type == GX_INDEX8 ? kRawIndex8 : kRawIndex16;
                p.guestBytes = type == GX_INDEX8 ? 1 : 2;
            }
        }
        r.parts[r.nParts++] = p;
        r.vtxBytes += p.guestBytes;
    }
    if (r.vtxBytes == 0 || r.vtxBytes > sizeof(r.vtx)) return false;
    r.fmt = fmt;
    r.generation = g_hleGxState.vtxStateGeneration;
    r.have = 0;
    return true;
}

// Big-endian bytes to Aurora's FIFO (check mode: to the comparison buffer).
inline void RawPut(const RawBegin& r, const uint8_t* p, uint32_t n) {
    if (r.check) {
        s_rawCheckBytes.insert(s_rawCheckBytes.end(), p, p + n);
        return;
    }
    GxAuroraFifoWrite(p, n);
}

// The gathered vertex, expanded: false (nothing written) when an indexed element is not all in guest RAM.
bool RawEmitVertex(RawBegin& r) {
    uint8_t out[512];
    uint32_t n = 0;
    uint32_t at = 0;
    for (uint32_t i = 0; i < r.nParts; ++i) {
        const RawPart& p = r.parts[i];
        const uint8_t* src = r.vtx + at;
        at += p.guestBytes;
        if (p.kind == kRawDirect || p.kind == kRawMatrix) {
            std::memcpy(out + n, src, p.guestBytes);
            n += p.guestBytes;
            continue;
        }
        const uint32_t index = p.kind == kRawIndex8 ? src[0] : ReadBE16(src);
        const auto& arr = g_hleGxState.vtxArray[p.attr];
        if (arr.base == 0 || arr.stride == 0) {
            // SubmitIndexedAttribute without an array: zero components (encoded 0 in any of these types), or black
            // with an opaque alpha for RGBA8.
            std::memset(out + n, 0, p.elemBytes);
            if (p.compBytes == 0 && p.elemBytes == 4) out[n + 3] = 0xFF;
            n += p.elemBytes;
            continue;
        }
        const uint32_t addr = arr.base + index * arr.stride;
        if (!Memory::Contains(addr, p.elemBytes)) return false;
        if (p.compBytes == 0) {  // a colour: the bytes DecodeColorFromArray reads
            for (uint32_t b = 0; b < p.elemBytes; ++b) out[n + b] = Memory::Read8(addr + b);
        } else {  // the raw components SubmitIndexedAttribute reads, big-endian
            for (uint32_t c = 0; c < p.comps; ++c) {
                const u32 raw = ReadArrayRawComp(addr + c * p.compBytes, p.type);
                if (p.compBytes == 4) BigEndian::Write32(out + n + 4 * c, raw);
                else if (p.compBytes == 2) BigEndian::Write16(out + n + 2 * c, static_cast<uint16_t>(raw));
                else out[n + c] = static_cast<uint8_t>(raw);
            }
        }
        n += p.elemBytes;
    }
    if (!r.check) {
        // The component path's matrix-index hook (SPM's bounding boxes), for the position index.
        at = 0;
        for (uint32_t i = 0; i < r.nParts; ++i) {
            if (r.parts[i].attr == GX_VA_PNMTXIDX) SpmBBoxVertexMatrix(r.vtx[at]);
            at += r.parts[i].guestBytes;
        }
    }
    RawPut(r, out, n);
    return true;
}

// The begin's raw path gives up at a vertex start: the gathered bytes go to the component path, one at a time.
void RawFallBack(RawBegin& r, const uint8_t* rest, uint32_t restBytes) {
    r.active = false;
    ++s_rawFallbacks;
    g_hleGxState.ResetVertex();
    const uint32_t have = r.have;
    r.have = 0;
    for (uint32_t i = 0; i < have; ++i) HleFifoWrite(r.vtx[i], 1);
    for (uint32_t i = 0; i < restBytes; ++i) HleFifoWrite(rest[i], 1);
}

// Stream bytes into a raw begin, all consumed here (bytes past the draw go to the packet parser).
void RawBeginBytes(RawBegin& r, const uint8_t* p, uint32_t n) {
    while (n != 0) {
        const uint32_t take = std::min(n, r.vtxBytes - r.have);
        std::memcpy(r.vtx + r.have, p, take);
        r.have += take;
        p += take;
        n -= take;
        if (r.have < r.vtxBytes) return;
        // A vertex is complete.
        if (g_hleGxState.vtxStateGeneration != r.generation || g_hleGxState.currentVtxFmt != r.fmt ||
            !RawEmitVertex(r)) {
            RawFallBack(r, p, n);
            return;
        }
        r.have = 0;
        g_hleGxState.ResetVertex();
        if (g_hleGxState.vertsRemaining > 0 && --g_hleGxState.vertsRemaining == 0) {
            g_hleGxState.inBegin = false;
            g_hleGxState.auroraBeginCalled = false;
            r.active = false;
            GXEnd();
            for (uint32_t i = 0; i < n; ++i) HleFifoWrite(p[i], 1);  // past the draw: the packet parser's
            return;
        }
    }
}

// A store's bytes, big-endian as HleFifoWrite pushes them (its callers store 1, 2 or 4 bytes; 0 means 4).
inline uint32_t StoreBytes(u32 val, uint32_t sizeBytes, uint8_t (&bytes)[4]) {
    if (sizeBytes == 4 || sizeBytes == 0) {
        BigEndian::Write32(bytes, val);
        return 4;
    }
    if (sizeBytes == 2) {
        BigEndian::Write16(bytes, static_cast<uint16_t>(val));
        return 2;
    }
    bytes[0] = static_cast<uint8_t>(val);
    return 1;
}

// Check mode: the same gathering, alongside the component path (which draws): every byte it is given.
void RawCheckBytes(RawBegin& r, const uint8_t* bytes, uint32_t n) {
    for (uint32_t i = 0; i < n && r.vertsLeft != 0; ++i) {
        r.vtx[r.have++] = bytes[i];
        if (r.have < r.vtxBytes) continue;
        r.have = 0;
        --r.vertsLeft;
        if (g_hleGxState.vtxStateGeneration != r.generation || !RawEmitVertex(r)) {
            r.active = false;  // the raw path would have fallen back: nothing to compare
            return;
        }
    }
}

// Check mode, right before the component path's GXEnd: Aurora's bytes for the begin against the raw path's.
void RawCheckCompare(RawBegin& r) {
    if (!r.active || !r.check) return;
    r.active = false;
    const uint32_t size = aurora::gx::fifo::get_buffer_size();
    if (r.vertsLeft != 0 || size < s_rawCheckStart) return;
    const uint32_t n = size - s_rawCheckStart;
    ++s_rawChecked;
    const bool same = n == s_rawCheckBytes.size() &&
                      std::memcmp(aurora::gx::fifo::get_buffer_data() + s_rawCheckStart, s_rawCheckBytes.data(), n) == 0;
    if (!same && s_rawCheckBad++ < 20) {
        RT_LOGF(RT_TAG_GX, "[rawbegin] MISMATCH fmt %u: Aurora %u bytes, raw %zu bytes\n", static_cast<unsigned>(r.fmt), n,
                s_rawCheckBytes.size());
    }
}
}  // namespace

int GxRawPacketMode() { return RawBeginMode(); }
uint64_t g_gxLightPackets = 0;
uint64_t g_gxBeginPublishSkips = 0;
uint64_t g_gxTexMtxPackets = 0;
static uint64_t s_rwLogDedupes = 0;  // GxCarryLogPublish (below): publications not logged twice (game thread)
static std::atomic<uint64_t> s_texMtxChecked{0}, s_texMtxCheckBad{0};  // check mode
void GxNoteTexMtxPacketCheck(bool same) {
    s_texMtxChecked.fetch_add(1, std::memory_order_relaxed);
    if (!same && s_texMtxCheckBad.fetch_add(1, std::memory_order_relaxed) < 20) {
        RT_LOGF(RT_TAG_GX, "[rawbegin] texture matrix packet MISMATCH\n");
    }
}
namespace {
std::atomic<uint64_t> s_lightChecked{0}, s_lightCheckBad{0};  // check mode: both producer threads load lights
}  // namespace
void GxNoteLightPacketCheck(bool same) {
    s_lightChecked.fetch_add(1, std::memory_order_relaxed);
    if (!same && s_lightCheckBad.fetch_add(1, std::memory_order_relaxed) < 20) {
        RT_LOGF(RT_TAG_GX, "[rawbegin] light packet MISMATCH\n");
    }
}

// The stats line (g3d_native.cpp G3dNativeLogStats): per frame.
extern "C" void GX_HLE_LogRawBeginStats(double frames) {
    const uint64_t lightChecked = s_lightChecked.exchange(0, std::memory_order_relaxed);
    const uint64_t texMtxChecked = s_texMtxChecked.exchange(0, std::memory_order_relaxed);
    if (frames <= 0 || (s_rawBegins | s_rawComponentBegins | s_rawChecked | g_gxLightPackets | lightChecked |
                        g_gxTexMtxPackets | texMtxChecked) == 0) {
        return;
    }
    RT_LOGF(RT_TAG_VI, "gx raw begins (GX_RAW_BEGIN=%d): %.1f/frame raw, %.2f/frame fell back, %.1f/frame component "
            "path; checked %.1f/frame, %llu mismatches; light packets %.1f/frame (game thread), checked %.1f/frame, "
            "%llu mismatches; begin publications in place %.1f/frame, carry-log repeats %.1f/frame; texture matrix "
            "packets %.1f/frame, checked %.1f/frame, %llu mismatches\n",
            s_rawBeginMode, s_rawBegins / frames, s_rawFallbacks / frames, s_rawComponentBegins / frames,
            s_rawChecked / frames, static_cast<unsigned long long>(s_rawCheckBad), g_gxLightPackets / frames,
            lightChecked / frames, static_cast<unsigned long long>(s_lightCheckBad.load(std::memory_order_relaxed)),
            g_gxBeginPublishSkips / frames, s_rwLogDedupes / frames, g_gxTexMtxPackets / frames,
            texMtxChecked / frames, static_cast<unsigned long long>(s_texMtxCheckBad.load(std::memory_order_relaxed)));
    s_rawBegins = s_rawFallbacks = s_rawComponentBegins = s_rawChecked = g_gxLightPackets = 0;
    g_gxBeginPublishSkips = s_rwLogDedupes = g_gxTexMtxPackets = 0;
}

// `val` is a raw big-endian bit pattern: the FIFO stream is type-agnostic, and
// the float entry point converts before it gets here.
// The vertex-state carry (gx_internal.h): FIFO bytes take the state back only at the commands that read or write it -
// CP register writes, indexed XF loads, display-list calls, and a draw that reads parts this thread has not rewritten
// since it went pending (GxCarryCanDraw) - not for BP/XF writes or a begun draw's vertex bytes.
void HleFifoWrite(u32 val, uint32_t sizeBytes) {
    HZ_HOT_PERF_ADD(kHzFifoWrites, 1);
    HZ_HOT_PHASE(kHzFifoTicks);
    const bool recordOnly = IsDisplayListActive();
    if (recordOnly) {
        WriteDisplayListData(val, sizeBytes);
        return;
    }
    // A begin on the raw path (see RawBegin): its stores are gathered there. Only for the begin it was set up for
    // (GXBegin starts each begin with auroraBeginCalled clear, so a stale raw state is never used).
    if (RawBegin& rb = t_rawBegin; rb.active && g_hleGxState.inBegin && g_hleGxState.auroraBeginCalled) {
        uint8_t bytes[4];
        const uint32_t n = StoreBytes(val, sizeBytes, bytes);
        if (!rb.check) {
            RawBeginBytes(rb, bytes, n);
            return;
        }
        RawCheckBytes(rb, bytes, n);  // check mode: and the component path below
    }

    auto resetFifoBuffer = [&]() {
        g_hleGxState.fifoReadOffset = 0;
        g_hleGxState.fifoByteCount = 0;
    };

    auto compactFifoBuffer = [&]() {
        if (g_hleGxState.fifoByteCount == 0) {
            g_hleGxState.fifoReadOffset = 0;
            return;
        }
        if (g_hleGxState.fifoReadOffset == 0) {
            return;
        }
        std::memmove(g_hleGxState.fifoBytes.data(),
                     g_hleGxState.fifoBytes.data() + g_hleGxState.fifoReadOffset,
                     g_hleGxState.fifoByteCount);
        g_hleGxState.fifoReadOffset = 0;
    };

    auto fifoData = [&]() -> uint8_t* {
        return g_hleGxState.fifoBytes.data() + g_hleGxState.fifoReadOffset;
    };

    auto pushBytes = [&](u32 value, uint32_t count) {
        if (count == 0) return;
        if (count > 4) count = 4;
        if (g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount + count > g_hleGxState.fifoBytes.size()) {
            compactFifoBuffer();
            if (g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount + count > g_hleGxState.fifoBytes.size()) {
                resetFifoBuffer();
            }
        }
        const size_t writeOffset = g_hleGxState.fifoReadOffset + g_hleGxState.fifoByteCount;
        switch (count) {
        case 4:
            BigEndian::Write32(g_hleGxState.fifoBytes.data() + writeOffset, value);
            break;
        case 2:
            BigEndian::Write16(g_hleGxState.fifoBytes.data() + writeOffset,
                               static_cast<uint16_t>(value));
            break;
        default:
            g_hleGxState.fifoBytes[writeOffset] = static_cast<uint8_t>(value & 0xFF);
            break;
        }
        g_hleGxState.fifoByteCount += count;
    };

    auto consumeBytes = [&](uint32_t count, u32& out) -> bool {
        if (count == 0 || g_hleGxState.fifoByteCount < count) return false;
        u32 value = 0;
        const uint32_t foldCount = (count > 4) ? 4 : count;
        uint8_t* data = fifoData();
        for (uint32_t i = 0; i < foldCount; ++i) {
            value = (value << 8) | data[i];
        }
        g_hleGxState.fifoReadOffset += count;
        g_hleGxState.fifoByteCount -= count;
        if (g_hleGxState.fifoByteCount == 0) {
            g_hleGxState.fifoReadOffset = 0;
        }
        out = value;
        return true;
    };

    pushBytes(val, sizeBytes == 0 ? 4 : sizeBytes);

    // Parse raw FIFO command packets written directly to the gather pipe
    // (e.g. NW4R/G3D paths that do not call the GXBegin wrapper function).
    while (!g_hleGxState.inBegin) {
        if (g_hleGxState.fifoByteCount < 1) {
            break;
        }

        uint8_t* data = fifoData();
        const uint8_t cmd = data[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        u32 sink = 0;

        if (cmd == GX_NOP_CMD || opcode == GX_CMD_INVL_VC_CMD) {
            if (!consumeBytes(1, sink)) break;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t bpWord = ReadBE32(data + 1);
            GXApplyBPReg(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
            if (!consumeBytes(5, sink)) break;
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 6) break;
            const uint8_t reg = data[1];
            const uint32_t cpValue = ReadBE32(data + 2);
            GX_HLE_MaterializeIfPending(kTakeBackFifoCp);
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            if (!consumeBytes(6, sink)) break;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint16_t countWords = ReadBE16(data + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (g_hleGxState.fifoByteCount < packetBytes) break;
            if (SpmDumpFrame()) {
                float v[4] = {};
                for (int k = 0; k < 4 && k <= countWords; ++k) { const uint32_t bits = ReadBE32(data + 5 + 4 * k); std::memcpy(&v[k], &bits, 4); }
                RT_LOGF(RT_TAG_GX, "[drawdump] fifo xf 0x%04X x%u: %.3f %.3f %.3f %.3f\n", ReadBE16(data + 3), countWords + 1u,
                        v[0], v[1], v[2], v[3]);
            }
            GXCallDisplayList(data, packetBytes);
            GXMarkFrameWork();
            if (!consumeBytes(packetBytes, sink)) break;
            continue;
        }

        if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
            if (g_hleGxState.fifoByteCount < 5) break;
            const uint32_t xfValue = ReadBE32(data + 1);
            GX_HLE_MaterializeIfPending(kTakeBackFifoIndexed);  // reads the matrix arrays
            if (SpmDumpFrame()) {
                const auto& arr = g_hleGxState.vtxArray[GX_POS_MTX_ARRAY + (((cmd & GX_OPCODE_MASK_CMD) - GX_LOAD_INDX_A_CMD) / 0x08)];
                const uint32_t src = arr.base + (xfValue >> 16) * arr.stride;
                float m[12] = {};
                for (int k = 0; k < 12; ++k) { const uint32_t bits = Memory::Read32(src + 4 * k); std::memcpy(&m[k], &bits, 4); }
                RT_LOGF(RT_TAG_GX, "[drawdump] fifo indexed xf cmd %02X idx %u len %u dst 0x%03X from 0x%08X: %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f\n",
                        cmd, xfValue >> 16, ((xfValue >> 12) & 0xFu) + 1u, xfValue & 0xFFFu, src,
                        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            }
            ApplyIndexedXfArrayForPacket(cmd, xfValue);
            GXCallDisplayList(data, 5);
            GXMarkFrameWork();
            if (!consumeBytes(5, sink)) break;
            continue;
        }

        if (opcode == GX_CMD_CALL_DL_CMD) {
            if (g_hleGxState.fifoByteCount < 9) break;
            const uint32_t listAddr = ReadBE32(data + 1);
            const uint32_t listSize = ReadBE32(data + 5);
            if (!consumeBytes(9, sink)) break;
            if (listAddr != 0 && listSize > 0) {
                GX_HLE_MaterializeIfPending(kTakeBackFifoList);  // called by name: not through the hooked thunk
                GX__CallDisplayList_HLE(listAddr, listSize);
            }
            continue;
        }

        if (IsDrawOpcode(opcode)) {
            if (g_hleGxState.fifoByteCount < 3) break;
            const uint16_t vtxCount = ReadBE16(data + 1);
            const GXVtxFmt vtxFmt = static_cast<GXVtxFmt>(cmd & GX_VAT_MASK_CMD);
            const GXPrimitive prim = OpcodeToGXPrimitive(cmd);
            if (t_gxVtxPending && !g3dn::g_recOn) {
                if (!GxCarryCanDraw(vtxFmt)) {
                    GX_HLE_MaterializeVertexStateSlow(kTakeBackFifoDraw);
                } else if (GxCarryRewriting()) {
                    GxCarryNoteCurFmt();  // the draw sets the current format
                }
            }
            uint32_t rawVertexSize = 0;
            if (TryGetRawDirectFifoVertexSize(vtxFmt, rawVertexSize)) {
                const uint32_t packetBytes = 3u + static_cast<uint32_t>(vtxCount) * rawVertexSize;
                if (g_hleGxState.fifoByteCount < packetBytes) {
                    break;
                }
                if (TrySubmitRawDirectFifoDraw(data, packetBytes, prim, vtxFmt, vtxCount)) {
                    GxCarryCountDraw();
                    if (!consumeBytes(packetBytes, sink)) break;
                    continue;
                }
            }
            if (!consumeBytes(3, sink)) break;

            GxCarryCountDraw();
            g_hleGxState.currentVtxFmt = vtxFmt;
            g_hleGxState.currentPrim = prim;
            g_hleGxState.vertsRemaining = vtxCount;
            g_hleGxState.inBegin = true;
            g_hleGxState.auroraBeginCalled = false;
            g_hleGxState.ResetVertex();

            break;
        }

        // Unknown FIFO command byte outside a begin packet; discard it so stream parsing can recover.
        if (!consumeBytes(1, sink)) break;
    }

    if (!g_hleGxState.inBegin) {
        return;
    }

    if (!recordOnly && !g_hleGxState.auroraBeginCalled) {
        EnsureAuroraFrameActive();
        if (!BeginPublicationCurrent(g_hleGxState.currentVtxFmt)) {
            ApplyAuroraVtxStateForRawBegin(g_hleGxState.currentVtxFmt);
        }
        t_gxBeginPublished = 0;
        EnsureDefaultGxAlphaCompare();
        GXBegin(g_hleGxState.currentPrim, g_hleGxState.currentVtxFmt, static_cast<u16>(g_hleGxState.vertsRemaining));
        g_hleGxState.auroraBeginCalled = true;
        GXMarkFrameWork();
        // The raw path (RawBegin) for this begin: the stream bytes buffered so far are its first. Game thread only.
        RawBegin& rb = t_rawBegin;
        rb.active = false;
        const int rawMode = RawBeginMode();
        if (rawMode != 0 && !t_gxDrawWorker && BuildRawLayout(rb, g_hleGxState.currentVtxFmt)) {
            rb.active = true;
            rb.check = rawMode == 2;
            const uint8_t* buffered = fifoData();
            const uint32_t n = static_cast<uint32_t>(g_hleGxState.fifoByteCount);
            if (!rb.check) {
                ++s_rawBegins;
                uint8_t first[sizeof(g_hleGxState.fifoBytes)];
                std::memcpy(first, buffered, n);
                resetFifoBuffer();
                RawBeginBytes(rb, first, n);
                return;
            }
            rb.vertsLeft = g_hleGxState.vertsRemaining;
            s_rawCheckBytes.clear();
            s_rawCheckStart = aurora::gx::fifo::get_buffer_size();
            RawCheckBytes(rb, buffered, n);  // and the component path parses them below
        } else if (!t_gxDrawWorker) {
            ++s_rawComponentBegins;
        }
    }

    auto finishVertexIfNeeded = [&](GXAttr prevAttr) {
        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(prevAttr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
            if (g_hleGxState.vertsRemaining > 0) {
                g_hleGxState.vertsRemaining--;
                if (g_hleGxState.vertsRemaining == 0) {
                    g_hleGxState.inBegin = false;
                    g_hleGxState.auroraBeginCalled = false;
                    if (!recordOnly) {
                        RawCheckCompare(t_rawBegin);  // GX_RAW_BEGIN=2
                        GXEnd();
                    }
                }
            }
        }
    };

    while (true) {
        if (!g_hleGxState.inBegin) {
            break;
        }
        GXAttr attr = g_hleGxState.currentAttr;
        if (attr == GX_VA_NULL) {
            resetFifoBuffer();
            return;
        }

        GXAttrType inputType = g_hleGxState.vtxDesc[attr];
        const VtxAttrFmt& fmt = g_hleGxState.vtxAttrFmt[g_hleGxState.currentVtxFmt][attr];

        if (IsMatrixIndexAttr(attr)) {
            if (g_hleGxState.fifoByteCount < 1) break;
            u32 raw = 0;
            if (!consumeBytes(1, raw)) break;
            if (!recordOnly) {
                if (attr == GX_VA_PNMTXIDX) SpmBBoxVertexMatrix(raw);
                GXMatrixIndex1u8(attr, static_cast<u8>(raw));
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        if (inputType == GX_DIRECT) {
            if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
                const uint32_t colorSize = ColorByteSize(fmt.type, fmt.cnt);

                if (g_hleGxState.fifoByteCount < colorSize) break;

                uint8_t colorBytes[4] = {0, 0, 0, 0};
                uint8_t* data = fifoData();
                for (uint32_t i = 0; i < colorSize && i < 4; ++i) {
                    colorBytes[i] = data[i];
                }
                g_hleGxState.fifoReadOffset += colorSize;
                g_hleGxState.fifoByteCount -= colorSize;
                if (g_hleGxState.fifoByteCount == 0) {
                    g_hleGxState.fifoReadOffset = 0;
                }

                GXColor color{};
                DecodeColorBytes(colorBytes, fmt.type, fmt.cnt, color);

                if (!recordOnly) {
                    if (fmt.cnt == GX_CLR_RGB) {
                        GXColor3u8(color.r, color.g, color.b);
                    } else {
                        GXColor4u8(color.r, color.g, color.b, color.a);
                    }
                }
                finishVertexIfNeeded(attr);
                continue;
            }
            
            const int compSize = GetCompSizeBytes(fmt.type);
            if (compSize <= 0 || g_hleGxState.fifoByteCount < static_cast<size_t>(compSize)) break;
            u32 raw = 0;
            if (!consumeBytes(static_cast<uint32_t>(compSize), raw)) break;
            constexpr int kMaxComps = 9;
            if (g_hleGxState.currentComp < kMaxComps) {
                if (SubmitAttributeReadsFloatComps(attr, fmt)) {
                    g_hleGxState.compBuffer[g_hleGxState.currentComp] =
                        ConvertCompToFloat(raw, fmt.type, fmt.frac);
                }
                g_hleGxState.rawCompBuffer[g_hleGxState.currentComp] = raw;
            }
            g_hleGxState.currentComp++;
            const int expected = g_hleGxState.GetExpectedCompCount(attr, fmt);
            if (g_hleGxState.currentComp >= expected) {
                if (!recordOnly) {
                    SubmitAttribute(attr, g_hleGxState.compBuffer, fmt, g_hleGxState.rawCompBuffer);
                }
                finishVertexIfNeeded(attr);
            }
            continue;
        }

        if (inputType == GX_INDEX8 || inputType == GX_INDEX16) {
            const uint32_t idxSize = (inputType == GX_INDEX8) ? 1u : 2u;
            const uint32_t indexCount = (attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (g_hleGxState.fifoByteCount < idxSize * indexCount) break;
            u32 raw[3]{};
            for (uint32_t i = 0; i < indexCount; ++i) {
                if (!consumeBytes(idxSize, raw[i])) break;
            }
            if (!recordOnly) {
                if (attr == GX_VA_NRM && indexCount == 3u) {
                    SubmitIndexedNormalNBT3(raw, fmt);
                } else {
                    SubmitIndexedAttribute(attr, raw[0]);
                }
            }
            finishVertexIfNeeded(attr);
            continue;
        }

        g_hleGxState.currentAttr = g_hleGxState.NextEnabledAttr(attr);
        g_hleGxState.currentComp = 0;
        if (g_hleGxState.currentAttr == GX_VA_NULL) {
            g_hleGxState.ResetVertex();
        }
        break;
    }
}

// Must behave exactly like GX_HLE_FIFO_Write32/16/8 run byte by byte. The ring is not
// bulk-appended ahead of a single parse because the parser can re-enter GX HLE (nested display
// lists, XF/draw calls), which would misorder that work relative to the stream.
static void HleFifoWriteBurstChunked(const uint8_t* data, uint32_t sizeBytes) {
    uint32_t offset = 0;
    for (; offset + 4u <= sizeBytes; offset += 4u) {
        HleFifoWrite(ReadBE32(data + offset), 4);
    }
    if (offset + 2u <= sizeBytes) {
        HleFifoWrite(static_cast<u32>(ReadBE16(data + offset)), 2);
        offset += 2u;
    }
    if (offset < sizeBytes) {
        HleFifoWrite(static_cast<u32>(data[offset]), 1);
    }
}

// Applies complete register-load packets straight to the parser's own entry points, skipping the
// serialize/ring/re-parse round trip; returns bytes consumed, caller hands the rest to
// HleFifoWriteBurstChunked. Safe (per gd_fifo_hle.cpp's GdCanApplyDirect) only on a packet
// boundary with nothing buffered, outside a GXBegin packet and outside display-list recording;
// any other opcode or a truncated packet just stops the walk.
static uint32_t ApplyFifoPacketsDirect(const uint8_t* data, uint32_t sizeBytes) {
    uint32_t offset = 0;

    while (offset < sizeBytes) {
        // Re-tested per packet, not once per burst: nothing currently re-enters GX HLE mid-walk,
        // but if it ever does, breaking here just hands the remainder to the ring.
        if (IsDisplayListActive() || g_hleGxState.inBegin || g_hleGxState.fifoByteCount != 0) {
            break;
        }

        const uint8_t* packet = data + offset;
        const uint32_t avail = sizeBytes - offset;
        const uint8_t cmd = packet[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;

        if (cmd == GX_NOP_CMD) {
            offset += 1u;
            continue;
        }

        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (avail < 5u) break;
            const uint32_t bpWord = ReadBE32(packet + 1);
            GXApplyBPReg(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
            offset += 5u;
            continue;
        }

        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (avail < 6u) break;
            const uint8_t reg = packet[1];
            const uint32_t cpValue = ReadBE32(packet + 2);
            // Same function the parser calls, so the CP registers it does not
            // decode (0x30/0x40 among them) are dropped here identically.
            GX_HLE_MaterializeIfPending(kTakeBackFifoCp);
            GxCpDecode::ApplyCpRegWrite(reg, cpValue);
            offset += 6u;
            continue;
        }

        if (opcode == GX_LOAD_XF_REG_CMD) {
            if (avail < 5u) break;
            const uint16_t countWords = ReadBE16(packet + 1);
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(countWords) + 1u) * 4u;
            if (avail < packetBytes) break;
            GXCallDisplayList(packet, packetBytes);
            GXMarkFrameWork();
            offset += packetBytes;
            continue;
        }

        break;
    }

    return offset;
}

// Display-list recording just copies bytes into the guest list buffer and advances the shadow
// cursor, so a burst is one logical append as long as it doesn't cross the list end; a burst that
// would wrap falls back to the per-write path instead. Returns false to signal that fallback.
static bool WriteDisplayListBurst(const uint8_t* data, uint32_t sizeBytes) {
    GxDisplayListState& dl = g_dlRecordState;
    if (t_gxDrawWorker || !dl.active || dl.base == 0 || dl.size == 0 || dl.writePtr == 0) {
        // Same guard WriteDisplayListData applies; let the per-write path
        // reproduce its (silent) drop.
        return false;
    }

    const uint32_t end = dl.base + dl.size;
    if (dl.writePtr > end || (end - dl.writePtr) < sizeBytes) {
        return false;
    }

    // Guest-visible bytes are written through the ordinary store path, so
    // unaligned cursors, MMIO policy and executable-write guards all behave as
    // they do for a single store. The cursor and count advance per element so a
    // failed write in the middle leaves exactly the completed prefix recorded.
    try {
        uint32_t offset = 0;
        for (; offset + 4u <= sizeBytes; offset += 4u) {
            Memory::Write32(dl.writePtr, ReadBE32(data + offset));
            dl.writePtr += 4u;
            dl.count += 4u;
        }
        if (offset + 2u <= sizeBytes) {
            Memory::Write16(dl.writePtr, ReadBE16(data + offset));
            dl.writePtr += 2u;
            dl.count += 2u;
            offset += 2u;
        }
        if (offset < sizeBytes) {
            Memory::Write8(dl.writePtr, data[offset]);
            dl.writePtr += 1u;
            dl.count += 1u;
        }
    } catch (const Memory::AccessViolation&) {
    }
    return true;
}

extern "C" void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t sizeBytes) {
    if (data == nullptr || sizeBytes == 0) {
        return;
    }
    g3dn::HleRec rec(g3dn::kRecFifo, data, sizeBytes);
    if (rec.skip) return;
    // The vertex-state carry: taken back at the commands that need it (ApplyFifoPacketsDirect, HleFifoWrite).

    if (IsDisplayListActive() && WriteDisplayListBurst(data, sizeBytes)) {
        return;
    }

    const uint32_t applied = ApplyFifoPacketsDirect(data, sizeBytes);
    if (applied < sizeBytes) {
        HleFifoWriteBurstChunked(data + applied, sizeBytes - applied);
    }
}

// The native nw4r g3d path's GD bytes (hle/g3d_native.cpp): whole BP, CP, XF and indexed-XF packets, each applied as
// the parser applies it (BP: GXApplyBPReg; CP: the HLE shadow; indexed XF: its source array bound, then the packet to
// Aurora; XF: the packet to Aurora), except that a run of XF and indexed-XF packets goes to Aurora in one
// GXCallDisplayList. Decoding them together is decoding them one by one: Aurora's FIFO is drained before the run,
// and nothing between them adds to it but the indexed loads' own array bindings (bound before the run; a second
// load from the same array starts a new run). Recorded as the same FIFO bytes. Outside the parser's direct
// conditions (see ApplyFifoPacketsDirect), or at any other command, it is GX_HLE_FIFO_WriteBurst.
extern "C" void GX_HLE_FIFO_WritePackets_G3D(const uint8_t* data, uint32_t sizeBytes) {
    if (data == nullptr || sizeBytes == 0) {
        return;
    }
    g3dn::HleRec rec(g3dn::g_recJob ? g3dn::kRecFifoG3D : g3dn::kRecFifo, data, sizeBytes);
    if (rec.skip) return;
    // The vertex-state carry: taken back at the commands that need it (below, ApplyFifoPacketsDirect, HleFifoWrite).
    if (IsDisplayListActive() || g_hleGxState.inBegin || g_hleGxState.fifoByteCount != 0) {
        const uint32_t applied = ApplyFifoPacketsDirect(data, sizeBytes);
        if (applied < sizeBytes) HleFifoWriteBurstChunked(data + applied, sizeBytes - applied);
        return;
    }
    uint32_t offset = 0, runStart = 0, runArrays = 0;
    bool runOpen = false;
    auto flushRun = [&] {
        if (runOpen && offset > runStart) {
            GXCallDisplayList(data + runStart, offset - runStart);
            GXMarkFrameWork();
        }
        runOpen = false;
        runArrays = 0;
    };
    while (offset < sizeBytes) {
        const uint8_t* packet = data + offset;
        const uint32_t avail = sizeBytes - offset;
        const uint8_t cmd = packet[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        if (cmd == GX_LOAD_BP_REG_CMD && avail >= 5u) {
            flushRun();
            const uint32_t bpWord = ReadBE32(packet + 1);
            GXApplyBPReg(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
            offset += 5u;
            continue;
        }
        if (opcode == GX_LOAD_CP_REG_CMD && avail >= 6u) {
            flushRun();
            GX_HLE_MaterializeIfPending(kTakeBackFifoCp);
            GxCpDecode::ApplyCpRegWrite(packet[1], ReadBE32(packet + 2));
            offset += 6u;
            continue;
        }
        if (opcode == GX_LOAD_XF_REG_CMD && avail >= 5u) {
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(ReadBE16(packet + 1)) + 1u) * 4u;
            if (avail < packetBytes) break;
            if (!runOpen) { runOpen = true; runStart = offset; }
            offset += packetBytes;
            continue;
        }
        if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD && avail >= 5u) {
            const uint32_t arrayBit = 1u << ((opcode - GX_LOAD_INDX_A_CMD) / 0x08);
            if (runArrays & arrayBit) flushRun();
            GX_HLE_MaterializeIfPending(kTakeBackFifoIndexed);  // reads the matrix arrays
            ApplyIndexedXfArrayForPacket(cmd, ReadBE32(packet + 1));
            runArrays |= arrayBit;
            if (!runOpen) { runOpen = true; runStart = offset; }
            offset += 5u;
            continue;
        }
        break;
    }
    flushRun();
    if (offset < sizeBytes) HleFifoWriteBurstChunked(data + offset, sizeBytes - offset);
}

// --- Deferred native draws (hle/g3d_native.cpp, G3D_NATIVE_ASYNC=2; see gx_dl.cpp) ----------------------------------

// The vertex-state effect of FIFO bytes on this thread's HLE state - their CP writes, and those of the lists they call
// (GX_HLE_ApplyListVertexState) - without submitting anything. *drawFmtMask gets the formats the called lists draw
// with. False at a command it does not skip (a draw: its vertex bytes need the parse) or a truncated packet.
extern "C" bool GX_HLE_ApplyListVertexState(uint32_t listAddr, uint32_t nbytes, uint32_t* drawFmtMask);
extern "C" bool GX_HLE_ApplyFifoVertexState(const uint8_t* data, uint32_t sizeBytes, uint32_t* drawFmtMask) {
    uint32_t offset = 0;
    while (offset < sizeBytes) {
        const uint8_t* packet = data + offset;
        const uint32_t avail = sizeBytes - offset;
        const uint8_t cmd = packet[0];
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        if (cmd == GX_NOP_CMD) {
            offset += 1u;
        } else if (cmd == GX_LOAD_BP_REG_CMD) {
            if (avail < 5u) return false;
            offset += 5u;
        } else if (opcode == GX_LOAD_CP_REG_CMD) {
            if (avail < 6u) return false;
            GxCpDecode::ApplyCpRegWrite(packet[1], ReadBE32(packet + 2));
            offset += 6u;
        } else if (opcode == GX_LOAD_XF_REG_CMD) {
            if (avail < 5u) return false;
            const uint32_t packetBytes = 1u + 4u + (static_cast<uint32_t>(ReadBE16(packet + 1)) + 1u) * 4u;
            if (avail < packetBytes) return false;
            offset += packetBytes;
        } else if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
            if (avail < 5u) return false;
            offset += 5u;
        } else if (opcode == GX_CMD_CALL_DL_CMD) {
            if (avail < 9u) return false;
            uint32_t mask = 0;
            if (!GX_HLE_ApplyListVertexState(ReadBE32(packet + 1), ReadBE32(packet + 5), &mask)) return false;
            *drawFmtMask |= mask;
            offset += 9u;
        } else {
            return false;
        }
    }
    return true;
}

// The vertex part of this thread's HLE state, to hand a draw worker the game thread's state at a recording.
namespace {
struct HleVertexState {
    GXAttrType vtxDesc[26];
    VtxAttrFmt vtxAttrFmt[8][26];
    uint64_t vtxLayoutHash;
    bool vtxLayoutHashDirty;
    uint32_t vtxStateGeneration;
    HleGxState::VtxArray vtxArray[26];
    GXVtxFmt currentVtxFmt;
};
}  // namespace
extern "C" size_t GX_HLE_VertexStateBytes() { return sizeof(HleVertexState); }
extern "C" void GX_HLE_SaveVertexState(void* out) {
    auto& s = *static_cast<HleVertexState*>(out);
    std::memcpy(s.vtxDesc, g_hleGxState.vtxDesc, sizeof(s.vtxDesc));
    std::memcpy(s.vtxAttrFmt, g_hleGxState.vtxAttrFmt, sizeof(s.vtxAttrFmt));
    s.vtxLayoutHash = g_hleGxState.vtxLayoutHash;
    s.vtxLayoutHashDirty = g_hleGxState.vtxLayoutHashDirty;
    s.vtxStateGeneration = g_hleGxState.vtxStateGeneration;
    std::memcpy(s.vtxArray, g_hleGxState.vtxArray, sizeof(s.vtxArray));
    s.currentVtxFmt = g_hleGxState.currentVtxFmt;
}
extern "C" void GX_HLE_LoadVertexState(const void* in) {
    const auto& s = *static_cast<const HleVertexState*>(in);
    t_gxBeginPublished = 0;  // the generation comes back with the state: no longer a record of what was published
    t_gxBeginPublishMemoValid = false;
    std::memcpy(g_hleGxState.vtxDesc, s.vtxDesc, sizeof(s.vtxDesc));
    std::memcpy(g_hleGxState.vtxAttrFmt, s.vtxAttrFmt, sizeof(s.vtxAttrFmt));
    g_hleGxState.vtxLayoutHash = s.vtxLayoutHash;
    g_hleGxState.vtxLayoutHashDirty = s.vtxLayoutHashDirty;
    g_hleGxState.vtxStateGeneration = s.vtxStateGeneration;
    std::memcpy(g_hleGxState.vtxArray, s.vtxArray, sizeof(s.vtxArray));
    g_hleGxState.currentVtxFmt = s.currentVtxFmt;
}

// --- The vertex-state carry's rewrite tracking (gx_internal.h GxCarryRewriting) -------------------------------------
// Game thread only: it alone has the vertex state pending. While pending, its vertex setters write its stale copy and
// note here what they rewrote, with the Aurora calls they made (whose results depend on the shadow they ran on). The
// first rewrite keeps the stale state as it was (the state the first pending job started from). At the take-back
// (g3d_native.cpp MaterializeVertexState): this thread's state is saved, the stale one loaded, the jobs' end state
// rebuilt on it as without rewrites; then the logged Aurora calls are replayed on that (the shadow's dirty bits kept:
// the calls set them when they ran), and the rewritten parts are put back from the saved state.
extern "C" void GXShadowSaveDirty(u32 out[2]);       // aurora GXManage.cpp
extern "C" void GXShadowLoadDirty(const u32 in[2]);
namespace {
constexpr uint32_t kAllDescs = (1u << 26) - 1u;
struct VtxRewrites {
    uint32_t desc = 0;                // attributes whose descriptor was rewritten
    uint32_t fmt[GX_MAX_VTXFMT] = {}; // per format, attributes whose format was rewritten
    bool curFmt = false;              // the current format (set by a draw)
    bool stale = false;               // s_rwStale holds the stale state
    uint8_t canDrawKnown = 0;         // per format: GxCarryCanDraw's answer cached (until the next rewrite)
    uint8_t canDrawOk = 0;
};
VtxRewrites s_rw;
HleVertexState s_rwStale;
enum : uint8_t { kLogClear, kLogDesc, kLogFmt, kLogPublish };
struct RewriteLogEntry {
    uint8_t kind = 0, fmt = 0, attr = 0, type = 0, cnt = 0, frac = 0;
    uint16_t nDesc = 0, nFmt = 0;  // kLogPublish: its lists in s_rwLogDescs / s_rwLogFmts from descAt / fmtAt
    uint32_t descAt = 0, fmtAt = 0;
};
std::vector<RewriteLogEntry> s_rwLog;
std::vector<GXVtxDescList> s_rwLogDescs;
std::vector<GXVtxAttrFmtList> s_rwLogFmts;
std::vector<uint8_t> s_rwLogDescRewritten, s_rwLogFmtRewritten;  // per list entry: from a rewritten part
constexpr size_t kRewriteLogMax = 2048;  // a full log takes the state back (bounds the replay)
uint64_t s_rwDraws = 0;                  // draws that needed no take-back (counted for the carry stats)
}  // namespace

bool GxCarryRewriting() {
    if (!t_gxVtxPending || g3dn::g_recOn) return false;
    if (s_rwLog.size() >= kRewriteLogMax) {
        GX_HLE_MaterializeVertexStateSlow(kTakeBackLogFull);
        return false;
    }
    if (!s_rw.stale) {
        GX_HLE_SaveVertexState(&s_rwStale);
        s_rw.stale = true;
    }
    return true;
}
void GxCarryNoteDesc(uint32_t attrMask) {
    s_rw.desc |= attrMask & kAllDescs;
    s_rw.canDrawKnown = 0;
}
void GxCarryNoteFmt(uint32_t fmt, uint32_t attrMask) {
    if (fmt >= GX_MAX_VTXFMT) return;
    s_rw.fmt[fmt] |= attrMask & kAllDescs;
    s_rw.canDrawKnown &= static_cast<uint8_t>(~(1u << fmt));
}
void GxCarryNoteCurFmt() { s_rw.curFmt = true; }
void GxCarryCountDraw() {
    if (t_gxVtxPending) ++s_rwDraws;  // still pending at the draw: it needed no take-back
}
void GxCarryLogClear() {
    RewriteLogEntry e;
    e.kind = kLogClear;
    s_rwLog.push_back(e);
}
void GxCarryLogDesc(uint32_t attr, uint32_t type) {
    RewriteLogEntry e;
    e.kind = kLogDesc;
    e.attr = static_cast<uint8_t>(attr);
    e.type = static_cast<uint8_t>(type);
    s_rwLog.push_back(e);
}
void GxCarryLogFmt(uint32_t fmt, uint32_t attr, uint32_t cnt, uint32_t type, uint32_t frac) {
    RewriteLogEntry e;
    e.kind = kLogFmt;
    e.fmt = static_cast<uint8_t>(fmt);
    e.attr = static_cast<uint8_t>(attr);
    e.cnt = static_cast<uint8_t>(cnt);
    e.type = static_cast<uint8_t>(type);
    e.frac = static_cast<uint8_t>(frac);
    s_rwLog.push_back(e);
}
void GxCarryLogPublish(GXVtxFmt fmt, const GXVtxDescList* descs, int n, const GXVtxAttrFmtList* fmts, int m) {
    // The same publication as the log's last entry (same format and lists, each entry from a rewritten part or not as
    // there): its replay (GX_HLE_VtxRewritesFinish) repeats the previous one on the state that one left, a fixed point
    // (same field values; the replay keeps the dirty bits as they were), so it is not logged again. Effects quads
    // publish the same state per particle.
    if (!s_rwLog.empty() && s_rwLog.back().kind == kLogPublish) {
        const RewriteLogEntry& last = s_rwLog.back();
        bool same = last.fmt == static_cast<uint8_t>(fmt) && last.nDesc == n && last.nFmt == m;
        for (int i = 0; same && i < n; ++i) {
            const GXVtxDescList& a = s_rwLogDescs[last.descAt + i];
            const uint32_t attr = descs[i].attr;
            same = a.attr == descs[i].attr && a.type == descs[i].type &&
                   s_rwLogDescRewritten[last.descAt + i] == (attr < 26 && ((s_rw.desc >> attr) & 1u));
        }
        for (int i = 0; same && i < m; ++i) {
            const GXVtxAttrFmtList& a = s_rwLogFmts[last.fmtAt + i];
            const uint32_t attr = fmts[i].attr;
            same = a.attr == fmts[i].attr && a.cnt == fmts[i].cnt && a.type == fmts[i].type && a.frac == fmts[i].frac &&
                   s_rwLogFmtRewritten[last.fmtAt + i] ==
                       (fmt < GX_MAX_VTXFMT && attr < 26 && ((s_rw.fmt[fmt] >> attr) & 1u));
        }
        if (same) {
            ++s_rwLogDedupes;
            return;
        }
    }
    RewriteLogEntry e;
    e.kind = kLogPublish;
    e.fmt = static_cast<uint8_t>(fmt);
    e.nDesc = static_cast<uint16_t>(n);
    e.nFmt = static_cast<uint16_t>(m);
    e.descAt = static_cast<uint32_t>(s_rwLogDescs.size());
    e.fmtAt = static_cast<uint32_t>(s_rwLogFmts.size());
    for (int i = 0; i < n; ++i) {
        const uint32_t attr = descs[i].attr;
        s_rwLogDescs.push_back(descs[i]);
        s_rwLogDescRewritten.push_back(attr < 26 && ((s_rw.desc >> attr) & 1u));
    }
    for (int i = 0; i < m; ++i) {
        const uint32_t attr = fmts[i].attr;
        s_rwLogFmts.push_back(fmts[i]);
        s_rwLogFmtRewritten.push_back(fmt < GX_MAX_VTXFMT && attr < 26 && ((s_rw.fmt[fmt] >> attr) & 1u));
    }
    s_rwLog.push_back(e);
}
bool GxCarryCanDraw(uint32_t fmt) {
    if (fmt >= GX_MAX_VTXFMT || s_rw.desc != kAllDescs) return false;
    const uint8_t bit = static_cast<uint8_t>(1u << fmt);
    if (s_rw.canDrawKnown & bit) return (s_rw.canDrawOk & bit) != 0;
    bool ok = true;
    for (int attr = 0; attr < 26 && ok; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) continue;
        if (type == GX_INDEX8 || type == GX_INDEX16) ok = false;  // the arrays are not tracked
        else if (!IsMatrixIndexAttr(static_cast<GXAttr>(attr)) && !((s_rw.fmt[fmt] >> attr) & 1u)) ok = false;
    }
    s_rw.canDrawKnown |= bit;
    if (ok) s_rw.canDrawOk |= bit; else s_rw.canDrawOk &= static_cast<uint8_t>(~bit);
    return ok;
}

// g3d_native.cpp's take-back, around rebuilding the jobs' end state.
extern "C" bool GX_HLE_VtxRewritesPending() { return s_rw.stale; }
extern "C" void GX_HLE_VtxRewritesBegin(void* saved) {
    GX_HLE_SaveVertexState(saved);
    GX_HLE_LoadVertexState(&s_rwStale);
}
extern "C" void GX_HLE_VtxRewritesFinish(const void* saved) {
    u32 dirty[2];
    GXShadowSaveDirty(dirty);
    GXVtxDescList descs[27];
    GXVtxAttrFmtList fmts[27];
    for (const RewriteLogEntry& e : s_rwLog) {
        switch (e.kind) {
        case kLogClear: GXClearVtxDesc(); break;
        case kLogDesc: GXSetVtxDesc(static_cast<GXAttr>(e.attr), static_cast<GXAttrType>(e.type)); break;
        case kLogFmt:
            GXSetVtxAttrFmt(static_cast<GXVtxFmt>(e.fmt), static_cast<GXAttr>(e.attr), static_cast<GXCompCnt>(e.cnt),
                            static_cast<GXCompType>(e.type), e.frac);
            break;
        case kLogPublish: {
            // Entries from parts not rewritten when the publish ran: what PublishAuroraVtxState takes from the state
            // the jobs left (descriptors with INDEX8/16 as DIRECT).
            for (uint32_t i = 0; i < e.nDesc; ++i) {
                descs[i] = s_rwLogDescs[e.descAt + i];
                if (!s_rwLogDescRewritten[e.descAt + i]) {
                    GXAttrType type = g_hleGxState.vtxDesc[descs[i].attr];
                    if (type == GX_INDEX8 || type == GX_INDEX16) type = GX_DIRECT;
                    descs[i].type = type;
                }
            }
            descs[e.nDesc] = GXVtxDescList{GX_VA_NULL, GX_NONE};
            GXSetVtxDescv(descs);
            if (e.nFmt > 0) {
                for (uint32_t i = 0; i < e.nFmt; ++i) {
                    fmts[i] = s_rwLogFmts[e.fmtAt + i];
                    if (!s_rwLogFmtRewritten[e.fmtAt + i]) {
                        const VtxAttrFmt& f = g_hleGxState.vtxAttrFmt[e.fmt][fmts[i].attr];
                        fmts[i] = GXVtxAttrFmtList{fmts[i].attr, f.cnt, f.type, f.frac};
                    }
                }
                fmts[e.nFmt] = GXVtxAttrFmtList{GX_VA_NULL, GX_POS_XY, GX_U8, 0};
                GXSetVtxAttrFmtv(static_cast<GXVtxFmt>(e.fmt), fmts);
            }
            break;
        }
        }
    }
    GXShadowLoadDirty(dirty);
    const auto& s = *static_cast<const HleVertexState*>(saved);
    for (int attr = 0; attr < 26; ++attr) {
        if ((s_rw.desc >> attr) & 1u) g_hleGxState.vtxDesc[attr] = s.vtxDesc[attr];
    }
    for (int fmt = 0; fmt < GX_MAX_VTXFMT; ++fmt) {
        for (int attr = 0; attr < 26; ++attr) {
            if ((s_rw.fmt[fmt] >> attr) & 1u) g_hleGxState.vtxAttrFmt[fmt][attr] = s.vtxAttrFmt[fmt][attr];
        }
    }
    if (s_rw.curFmt) g_hleGxState.currentVtxFmt = s.currentVtxFmt;
    g_hleGxState.InvalidateVtxLayoutHash();
    s_rw = VtxRewrites{};
    s_rwLog.clear();
    s_rwLogDescs.clear();
    s_rwLogFmts.clear();
    s_rwLogDescRewritten.clear();
    s_rwLogFmtRewritten.clear();
}
extern "C" uint64_t GX_HLE_TakeVtxRewriteDraws() {
    const uint64_t n = s_rwDraws;
    s_rwDraws = 0;
    return n;
}

// Nothing in flight a deferral could reorder: no display list being recorded (by the HLE or by Aurora), no GXBegin
// packet or FIFO bytes pending.
extern "C" bool GX_HLE_IdleForDeferral() {
    return !GX_HLE_DisplayListRecording() && !g_hleGxState.inBegin && g_hleGxState.fifoByteCount == 0;
}
