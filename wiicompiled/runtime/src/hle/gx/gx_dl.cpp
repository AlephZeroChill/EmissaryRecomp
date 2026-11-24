extern "C" void aurora_spm_vtx_state(uint8_t* desc26, uint8_t* fmt8x26x3);
extern "C" void GXCallDisplayListCompiled(const void* data, uint32_t nbytes, uint64_t digest);  // GXDispList.cpp
extern "C" void aurora_gx_sync();  // aurora.cpp: wait for Aurora's GX thread (no-op when it is off)
#include "gx_internal.h"
#include "runtime_log.h"
#include "gx_stream_common.h"
#include "gx_cp_decode.h"
#include "isa/big_endian.h"

#include <atomic>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <unordered_map>

// The display-list scan cache validates a cached scan against the live guest
// bytes with a 64-bit XXH3 digest (see GxDisplayListScanCache::CanReuse).
#include <xxhash.h>
#include "hz_perf.h"
#include "g3d_native.h"
#include "gx_vertex_state_overrides.h"  // last: this file's entry points touch the HLE vertex state

namespace aurora::gx::fifo {
bool in_display_list();
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
}

namespace {

// Opcode constants and the stream helpers this file shares with gx_fifo.cpp /
// gx_vertex.cpp; see gx_stream_common.h.
using namespace GxCmd;
using namespace GxStream;
using GxCpDecode::ApplyCpRegWrite;
using GxCpDecode::SameVtxAttrFmt;

// Small display lists dominate the in-race call count. Cache them as well, but
// cap both individual entries and aggregate copied command bytes so malformed
// guest input cannot turn this optimization into unbounded host allocation.
constexpr uint32_t kDlScanCacheMaxEntryBytes = 64u * 1024u;
// SPM gn3_12 calls ~10k distinct lists per frame; at 8192 the cache cleared itself every frame.
constexpr size_t kDlScanCacheMaxEntries = 65536;
constexpr size_t kDlScanCacheMaxStoredBytes = 32u * 1024u * 1024u;

// Display-list write tracking (audit F6a): re-digesting every list every call is the
// costliest step of GX__CallDisplayList, and wasted on BRRES shape lists that are written
// once at load and never touched again.
constexpr uint64_t kDlWriteGenerationUntracked = GxGuestWrite::kUntracked;

// Byte footprint of one vertex of the currently configured layout. Indexed attributes are
// sized before the matrix-attribute check here (unlike BuildScanVertexLayout), but a
// GX_INDEX16 matrix attribute is impossible on real hardware so both are unreachable.
static uint32_t CalcDLVertexSize(GXVtxFmt vtxfmt) {
    uint32_t size = 0;
    for (int attr = 0; attr < 26; ++attr) {
        const auto type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) continue;
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        if (type == GX_INDEX8) size += (attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
        else if (type == GX_INDEX16) size += ((attr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u) * 2u;
        else if (type == GX_DIRECT) {
            size += DirectAttrByteSize(static_cast<GXAttr>(attr), fmt, /*matrixAttrIsOneByte=*/true,
                                       /*fallbackComps=*/0u);
        }
    }
    return size;
}

static void ApplyAuroraVtxDesc();
static void ApplyAuroraVtxAttrFmtForDisplayList(GXVtxFmt fmt, bool mixed);
static void SyncAppliedVtxStateFromHleReal();
static void SubmitLytDrawPacket(const uint8_t* packet, uint32_t packetBytes);

static inline uint8_t ScaleLytAlpha(uint8_t alpha, uint32_t modulate) {
    const int32_t product = static_cast<int32_t>(alpha) * static_cast<int32_t>(modulate & 0xFFu);
    const int32_t magic = static_cast<int32_t>(0x80808081u);
    int32_t quotient = static_cast<int32_t>((static_cast<int64_t>(magic) * product) >> 32);
    quotient = static_cast<int32_t>(quotient + product) >> 7;
    quotient += (static_cast<uint32_t>(quotient) >> 31);
    return static_cast<uint8_t>(quotient);
}

static inline uint32_t ReadLytColor(uint32_t colorAddr, int index) {
    return Memory::Read32(colorAddr + static_cast<uint32_t>(index * 4));
}

static inline uint32_t ReadModulatedLytColor(uint32_t colorAddr, int index, uint32_t alpha) {
    const uint32_t addr = colorAddr + static_cast<uint32_t>(index * 4);
    const uint8_t r = Memory::Read8(addr);
    const uint8_t g = Memory::Read8(addr + 1);
    const uint8_t b = Memory::Read8(addr + 2);
    uint8_t a = Memory::Read8(addr + 3);
    if ((alpha & 0xFFu) != 0xFFu) {
        a = ScaleLytAlpha(a, alpha);
    }
    return (static_cast<uint32_t>(r) << 24) |
           (static_cast<uint32_t>(g) << 16) |
           (static_cast<uint32_t>(b) << 8) |
           static_cast<uint32_t>(a);
}

static inline void AppendLytTexCoords(uint8_t* packet, uint32_t& pos, uint32_t texCoordAddr,
                                      int texCoordCount, uint32_t cornerOffset) {
    if (texCoordCount <= 0) {
        return;
    }
    for (int i = 0; i < texCoordCount; ++i) {
        const uint32_t coordAddr = texCoordAddr + static_cast<uint32_t>(i * 32) + cornerOffset;
        BigEndian::AppendFloat32(packet, pos, Memory::ReadFloat32(coordAddr));
        BigEndian::AppendFloat32(packet, pos, Memory::ReadFloat32(coordAddr + 4));
    }
}

static inline void AppendLytQuadVertex(uint8_t* packet, uint32_t& pos, float x, float y,
                                       uint32_t texCoordAddr, int texCoordCount,
                                       uint32_t cornerOffset, const uint32_t* colors, int colorIndex) {
    BigEndian::AppendFloat32(packet, pos, x);
    BigEndian::AppendFloat32(packet, pos, y);
    if (colors) {
        BigEndian::Append32(packet, pos, colors[colorIndex]);
    }
    AppendLytTexCoords(packet, pos, texCoordAddr, texCoordCount, cornerOffset);
}

// The four corners of an nw4r::lyt quad, in the order GX_QUADS expects, with
// each corner's texcoord byte offset and colour index. The winding order is
// load-bearing and both emit paths below share this one copy of it.
static inline void AppendLytQuadVertices(uint8_t* packet, uint32_t& pos, float x0, float y0,
                                         float x1, float y1, uint32_t texCoordAddr,
                                         int texCoordCount, const uint32_t* colors) {
    AppendLytQuadVertex(packet, pos, x0, y0, texCoordAddr, texCoordCount, 0, colors, 0);
    AppendLytQuadVertex(packet, pos, x1, y0, texCoordAddr, texCoordCount, 8, colors, 1);
    AppendLytQuadVertex(packet, pos, x1, y1, texCoordAddr, texCoordCount, 24, colors, 3);
    AppendLytQuadVertex(packet, pos, x0, y1, texCoordAddr, texCoordCount, 16, colors, 2);
}

static bool CanSubmitLytDrawDirect(int texCoordCount, const uint32_t* colors) {
    if (texCoordCount < 0 || texCoordCount > 8) {
        return false;
    }
    const auto& posFmt = g_hleGxState.vtxAttrFmt[GX_VTXFMT0][GX_VA_POS];
    if (g_hleGxState.vtxDesc[GX_VA_POS] != GX_DIRECT ||
        posFmt.cnt != GX_POS_XY ||
        posFmt.type != GX_F32) {
        return false;
    }

    const bool hasColors = colors != nullptr;
    const auto& clrFmt = g_hleGxState.vtxAttrFmt[GX_VTXFMT0][GX_VA_CLR0];
    if (hasColors) {
        if (g_hleGxState.vtxDesc[GX_VA_CLR0] != GX_DIRECT ||
            clrFmt.cnt != GX_CLR_RGBA ||
            clrFmt.type != GX_RGBA8) {
            return false;
        }
    } else if (g_hleGxState.vtxDesc[GX_VA_CLR0] != GX_NONE) {
        return false;
    }

    if (g_hleGxState.vtxDesc[GX_VA_NRM] != GX_NONE ||
        g_hleGxState.vtxDesc[GX_VA_CLR1] != GX_NONE) {
        return false;
    }

    for (int attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7MTXIDX; ++attr) {
        if (g_hleGxState.vtxDesc[attr] != GX_NONE) {
            return false;
        }
    }

    for (int i = 0; i < 8; ++i) {
        const GXAttr attr = static_cast<GXAttr>(GX_VA_TEX0 + i);
        const auto& fmt = g_hleGxState.vtxAttrFmt[GX_VTXFMT0][attr];
        if (i < texCoordCount) {
            if (g_hleGxState.vtxDesc[attr] != GX_DIRECT ||
                fmt.cnt != GX_TEX_ST ||
                fmt.type != GX_F32) {
                return false;
            }
        } else if (g_hleGxState.vtxDesc[attr] != GX_NONE) {
            return false;
        }
    }

    return true;
}

static bool SubmitLytDrawDirect(float x0, float y0, float x1, float y1, int texCoordCount,
                                uint32_t texCoordAddr, const uint32_t* colors) {
    if (!CanSubmitLytDrawDirect(texCoordCount, colors)) {
        return false;
    }


    EnsureAuroraFrameActive();

    ApplyAuroraVtxDesc();

    ApplyAuroraVtxAttrFmtForDisplayList(GX_VTXFMT0, false);

    EnsureDefaultGxAlphaCompare();


    std::array<uint8_t, 4u * (8u + 4u + 8u * 8u)> vertices{};
    uint32_t pos = 0;
    AppendLytQuadVertices(vertices.data(), pos, x0, y0, x1, y1, texCoordAddr, texCoordCount, colors);

    if (!aurora::gx::fifo::submit_raw_draw(GX_QUADS, GX_VTXFMT0, vertices.data(), 4, pos)) {
        return false;
    }
    GXMarkFrameWork();

    SyncAppliedVtxStateFromHleReal();
    return true;
}

static inline void EmitLytDrawQuad(uint32_t posAddr, uint32_t sizeAddr, int texCoordCount,
                                   uint32_t texCoordAddr, const uint32_t* colors) {
    const float x0 = Memory::ReadFloat32(posAddr);
    const float y0 = Memory::ReadFloat32(posAddr + 4);
    // The casts pin the arithmetic to PPC single precision regardless of the
    // host's FLT_EVAL_METHOD.
    const float x1 = static_cast<float>(x0 + Memory::ReadFloat32(sizeAddr));
    const float y1 = static_cast<float>(y0 - Memory::ReadFloat32(sizeAddr + 4));

    if (SubmitLytDrawDirect(x0, y0, x1, y1, texCoordCount, texCoordAddr, colors)) {
        return;
    }

    // GX has exactly 8 texture coordinates, so nw4r::lyt cannot ask for more.
    // The fixed packet buffer below is sized for that maximum; bail rather than
    // overrun it if the guest ever hands us something else.
    if (texCoordCount > 8) {
        return;
    }

    std::array<uint8_t, 3u + 4u * (8u + 4u + 8u * 8u)> packet{};
    uint32_t pos = 0;
    packet[pos++] = GX_DRAW_QUADS_CMD | GX_VTXFMT0;
    BigEndian::Append16(packet.data(), pos, 4);
    AppendLytQuadVertices(packet.data(), pos, x0, y0, x1, y1, texCoordAddr, texCoordCount, colors);
    SubmitLytDrawPacket(packet.data(), pos);
}

static uint32_t SubmitDLVertex(const uint8_t* ptr, GXVtxFmt vtxfmt, const GXAttrType* sourceVtxDesc) {
    uint32_t offset = 0;
    for (int attr = 0; attr < 26; ++attr) {
        const auto type = sourceVtxDesc[attr]; if (type == GX_NONE) continue;
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        if (type == GX_INDEX8) {
            const uint32_t indexCount = (gxAttr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (gxAttr == GX_VA_NRM && indexCount == 3u) {
                const uint32_t indices[3] = {ptr[offset], ptr[offset + 1], ptr[offset + 2]};
                SubmitIndexedNormalNBT3(indices, fmt);
            } else {
                SubmitIndexedAttribute(gxAttr, ptr[offset]);
            }
            offset += indexCount;
        }
        else if (type == GX_INDEX16) {
            const uint32_t indexCount = (gxAttr == GX_VA_NRM) ? NormalIndexCount(fmt) : 1u;
            if (gxAttr == GX_VA_NRM && indexCount == 3u) {
                const uint32_t indices[3] = {
                    ReadBE16(ptr + offset),
                    ReadBE16(ptr + offset + 2),
                    ReadBE16(ptr + offset + 4),
                };
                SubmitIndexedNormalNBT3(indices, fmt);
            } else {
                SubmitIndexedAttribute(gxAttr, ReadBE16(ptr + offset));
            }
            offset += indexCount * 2u;
        }
        else if (type == GX_DIRECT) {
            float comps[9]{};
            switch (attr) {
            case GX_VA_PNMTXIDX: {
                const u8 idx = ptr[offset];
                offset += 1;
                GXMatrixIndex1u8(gxAttr, idx);
                break;
            }
            case GX_VA_TEX0MTXIDX: case GX_VA_TEX1MTXIDX: case GX_VA_TEX2MTXIDX:
            case GX_VA_TEX3MTXIDX: case GX_VA_TEX4MTXIDX: case GX_VA_TEX5MTXIDX:
            case GX_VA_TEX6MTXIDX: case GX_VA_TEX7MTXIDX:
                GXMatrixIndex1u8(gxAttr, ptr[offset]);
                offset += 1;
                break;
            case GX_VA_POS: {
                int count = static_cast<int>(AttrCompCount(GX_VA_POS, fmt, 0u)), compSize = GetCompSizeBytes(fmt.type);
                for (int i = 0; i < count; ++i) {
                    comps[i] = ReadStreamComp(ptr + offset, fmt.type, fmt.frac);
                    offset += compSize;
                }
                SubmitAttribute(gxAttr, comps, fmt);
                break;
            }
            case GX_VA_NRM: {
                int count = static_cast<int>(AttrCompCount(GX_VA_NRM, fmt, 0u)), compSize = GetCompSizeBytes(fmt.type);
                for (int i = 0; i < count; ++i) {
                    comps[i] = ReadStreamComp(ptr + offset, fmt.type, fmt.frac);
                    offset += compSize;
                }
                SubmitAttribute(gxAttr, comps, fmt);
                break;
            }
            case GX_VA_CLR0: case GX_VA_CLR1: {
                GXColor color{};
                DecodeColorBytes(ptr + offset, fmt.type, fmt.cnt, color);
                offset += ColorByteSize(fmt.type, fmt.cnt);
                if (fmt.cnt == GX_CLR_RGB) GXColor3u8(color.r, color.g, color.b); else GXColor4u8(color.r, color.g, color.b, color.a);
                break;
            }
            case GX_VA_TEX0: case GX_VA_TEX1: case GX_VA_TEX2: case GX_VA_TEX3: case GX_VA_TEX4: case GX_VA_TEX5: case GX_VA_TEX6: case GX_VA_TEX7: {
                // All eight GX_VA_TEXn share one component layout.
                int count = static_cast<int>(AttrCompCount(GX_VA_TEX0, fmt, 0u)), compSize = GetCompSizeBytes(fmt.type);
                for (int i = 0; i < count; ++i) {
                    comps[i] = ReadStreamComp(ptr + offset, fmt.type, fmt.frac);
                    offset += compSize;
                }
                SubmitAttribute(gxAttr, comps, fmt);
                break;
            }
            default: break;
            }
        }
    }
    return offset;
}

static void ApplyAuroraVtxStateForDlBegin(GXVtxFmt fmt) {
    // Only this publish site latches the format into the HLE state; the FIFO and
    // immediate-mode sites have already done it by the time they publish.
    g_hleGxState.currentVtxFmt = fmt;

    PublishAuroraVtxState(fmt, AuroraVtxPublishOptions{/*includeNbt=*/false,
                                                       /*fmtLoopFirst=*/0,
                                                       /*fmtLoopLast=*/25});
}


struct HleGxVertexStateSnapshot {
    GXAttrType vtxDesc[26];
    VtxAttrFmt vtxAttrFmt[8][26];
    HleGxState::VtxArray vtxArray[26];
    GXVtxFmt currentVtxFmt;
    uint64_t vtxLayoutHash;
    bool vtxLayoutHashDirty;
    // Bit per vtxAttrFmt row carried in this snapshot.
    uint32_t vtxAttrFmtRows;
    bool hasVtxDesc;
    bool hasVtxArray;
    bool hasCurrentVtxFmt;
};

constexpr uint32_t kAllVtxAttrFmtRows = 0xFFu;

static HleGxVertexStateSnapshot CaptureGxVertexState() {
    HleGxVertexStateSnapshot snapshot;
    std::memcpy(snapshot.vtxDesc, g_hleGxState.vtxDesc, sizeof(snapshot.vtxDesc));
    std::memcpy(snapshot.vtxAttrFmt, g_hleGxState.vtxAttrFmt, sizeof(snapshot.vtxAttrFmt));
    std::memcpy(snapshot.vtxArray, g_hleGxState.vtxArray, sizeof(snapshot.vtxArray));
    snapshot.currentVtxFmt = g_hleGxState.currentVtxFmt;
    snapshot.vtxLayoutHash = g_hleGxState.vtxLayoutHash;
    snapshot.vtxLayoutHashDirty = g_hleGxState.vtxLayoutHashDirty;
    snapshot.vtxAttrFmtRows = kAllVtxAttrFmtRows;
    snapshot.hasVtxDesc = true;
    snapshot.hasVtxArray = true;
    snapshot.hasCurrentVtxFmt = true;
    return snapshot;
}

static void RestoreGxVertexState(const HleGxVertexStateSnapshot& snapshot) {
    if (snapshot.hasVtxDesc) {
        std::memcpy(g_hleGxState.vtxDesc, snapshot.vtxDesc, sizeof(snapshot.vtxDesc));
    }
    if (snapshot.vtxAttrFmtRows == kAllVtxAttrFmtRows) {
        std::memcpy(g_hleGxState.vtxAttrFmt, snapshot.vtxAttrFmt, sizeof(snapshot.vtxAttrFmt));
    } else {
        for (uint32_t fmt = 0; fmt < 8; ++fmt) {
            if ((snapshot.vtxAttrFmtRows & (1u << fmt)) == 0) {
                continue;
            }
            std::memcpy(g_hleGxState.vtxAttrFmt[fmt], snapshot.vtxAttrFmt[fmt],
                        sizeof(snapshot.vtxAttrFmt[fmt]));
        }
    }
    if (snapshot.hasVtxArray) {
        std::memcpy(g_hleGxState.vtxArray, snapshot.vtxArray, sizeof(snapshot.vtxArray));
    }
    if (snapshot.hasCurrentVtxFmt) {
        g_hleGxState.currentVtxFmt = snapshot.currentVtxFmt;
    }
    g_hleGxState.vtxLayoutHash = snapshot.vtxLayoutHash;
    g_hleGxState.vtxLayoutHashDirty = snapshot.vtxLayoutHashDirty;
    // The wholesale desc/attr-fmt overwrite above bypasses the per-field change
    // checks that normally bump this, so mirrors must be told to resync.
    ++g_hleGxState.vtxStateGeneration;
}

struct ScanAttrStep {
    GXAttr attr = GX_VA_NULL;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool indexed = false;
    bool index16 = false;
};

struct ScanLayoutCacheEntry {
    // Only the indexed steps are consumed (ScanDLVerticesMaxIndices); the scan
    // has no use for the direct ones beyond their contribution to vertexSize.
    std::array<ScanAttrStep, 26> indexedSteps{};
    uint32_t indexedStepCount = 0;
    uint32_t vertexSize = 0;
    uint32_t version = 0;
    uint32_t generation = 0;
    bool valid = false;
};


struct DlCpWrite {
    uint8_t reg = 0;
    uint32_t value = 0;
};


static HleGxVertexStateSnapshot CaptureGxVertexStateForCpWrites(const std::vector<DlCpWrite>& writes) {
    HleGxVertexStateSnapshot snapshot;
    snapshot.vtxAttrFmtRows = 0;
    snapshot.hasVtxDesc = false;
    snapshot.hasVtxArray = false;
    snapshot.hasCurrentVtxFmt = false;
    for (const auto& write : writes) {
        const uint8_t reg = write.reg;
        if (reg == 0x50 || reg == 0x60) {
            snapshot.hasVtxDesc = true;
        } else if (reg >= 0x70 && reg <= 0x77) {
            snapshot.vtxAttrFmtRows |= 1u << (reg - 0x70);
        } else if (reg >= 0x80 && reg <= 0x87) {
            snapshot.vtxAttrFmtRows |= 1u << (reg - 0x80);
        } else if (reg >= 0x90 && reg <= 0x97) {
            snapshot.vtxAttrFmtRows |= 1u << (reg - 0x90);
        } else if (reg >= 0xA0 && reg <= 0xBF) {
            snapshot.hasVtxArray = true;
        }
    }
    if (snapshot.hasVtxDesc) {
        std::memcpy(snapshot.vtxDesc, g_hleGxState.vtxDesc, sizeof(snapshot.vtxDesc));
    }
    for (uint32_t fmt = 0; fmt < 8; ++fmt) {
        if ((snapshot.vtxAttrFmtRows & (1u << fmt)) == 0) {
            continue;
        }
        std::memcpy(snapshot.vtxAttrFmt[fmt], g_hleGxState.vtxAttrFmt[fmt],
                    sizeof(snapshot.vtxAttrFmt[fmt]));
    }
    if (snapshot.hasVtxArray) {
        std::memcpy(snapshot.vtxArray, g_hleGxState.vtxArray, sizeof(snapshot.vtxArray));
    }
    snapshot.vtxLayoutHash = g_hleGxState.vtxLayoutHash;
    snapshot.vtxLayoutHashDirty = g_hleGxState.vtxLayoutHashDirty;
    return snapshot;
}

struct DlScanCacheEntry {
    uint32_t listAddr = 0;
    uint32_t nbytes = 0;
    uint64_t layoutHash = 0;
    std::array<uint32_t, GX_VA_MAX_ATTR> maxIdx{};
    std::array<bool, GX_VA_MAX_ATTR> sawIdx{};
    std::array<uint32_t, GX_VA_MAX_ATTR> maxXfIdx{};
    std::array<uint32_t, GX_VA_MAX_ATTR> maxXfBytes{};
    std::array<bool, GX_VA_MAX_ATTR> sawXfIdx{};
    GXVtxFmt dlVtxFmt = GX_MAX_VTXFMT;
    bool dlVtxFmtMixed = false;
    bool dlHasNestedDl = false;
    bool dlHasArrayStateWrites = false;
    bool dlHasCpWrites = false;
    bool scanOk = false;
    // Flatten results for lists that need one. `flattenVerbatim` means the
    // flattened stream is byte-identical to the source list, so aurora can be
    // handed the guest bytes directly and no copy has to be stored.
    bool needsFlatten = false;
    bool flattenOk = false;
    bool flattenVerbatim = false;
};

struct DlScanCacheRecord {
    DlScanCacheEntry result{};
    // Answer of DisplayListMayContainDraw for these bytes
    bool mayContainDraw = false;
    uint64_t contentDigest = 0;
    uint64_t writeGeneration = kDlWriteGenerationUntracked;
    std::vector<DlCpWrite> cpWrites{};
    std::vector<uint8_t> flattened{};
};

struct DlScanCacheState {
    std::unordered_map<uint64_t, DlScanCacheRecord> entries{};
    size_t storedCommandBytes = 0;
};

static uint64_t HashScanLayoutState() {
    if (!g_hleGxState.vtxLayoutHashDirty) {
        return g_hleGxState.vtxLayoutHash;
    }
    // The same 650 values the FNV-1 mix used to take one at a time, packed and hashed in one XXH3 pass.
    // The hash is only an in-process cache key (equal layouts give equal keys either way); FNV's serial
    // multiply chain cost ~0.7 us per layout change, and the layout changes dozens of times a frame.
    std::array<uint32_t, 26 + 8 * 26 * 3> packed;
    size_t n = 0;
    for (int attr = 0; attr < 26; ++attr) {
        packed[n++] = static_cast<uint32_t>(g_hleGxState.vtxDesc[attr]);
    }
    for (int fmt = 0; fmt < 8; ++fmt) {
        for (int attr = 0; attr < 26; ++attr) {
            const auto& f = g_hleGxState.vtxAttrFmt[fmt][attr];
            packed[n++] = static_cast<uint32_t>(f.cnt);
            packed[n++] = static_cast<uint32_t>(f.type);
            packed[n++] = static_cast<uint32_t>(f.frac);
        }
    }
    const uint64_t hash = static_cast<uint64_t>(XXH3_64bits(packed.data(), sizeof(packed)));
    g_hleGxState.vtxLayoutHash = hash;
    g_hleGxState.vtxLayoutHashDirty = false;
    return g_hleGxState.vtxLayoutHash;
}

static uint64_t DlScanCacheKey(uint32_t listAddr, uint32_t nbytes, uint64_t layoutHash) {
    return GxDisplayListScanCache::MakeIdentityKey(listAddr, nbytes, layoutHash);
}

static DlScanCacheState& DlScanCache() {
    static thread_local DlScanCacheState s_cache;
    return s_cache;
}

static uint64_t DlContentDigest(const uint8_t* list, uint32_t nbytes) {
    return static_cast<uint64_t>(XXH3_64bits(list, static_cast<size_t>(nbytes)));
}


struct DlScanCacheProbe {
    const DlScanCacheRecord* record = nullptr;
    uint64_t contentDigest = 0;
    bool digestValid = false;
    uint64_t writeGeneration = kDlWriteGenerationUntracked;
};

static DlScanCacheProbe ProbeDlScanCache(const uint8_t* list, uint32_t listAddr, uint32_t nbytes,
                                         uint64_t layoutHash) {
    DlScanCacheProbe probe{};
    probe.writeGeneration = GxGuestWrite::GenerationForRange(listAddr, nbytes);

    auto& s_cache = DlScanCache();
    const uint64_t key = DlScanCacheKey(listAddr, nbytes, layoutHash);
    const auto it = s_cache.entries.find(key);
    if (it != s_cache.entries.end()) {
        auto& record = it->second;
        const auto& entry = record.result;
        const bool identityMatches = entry.listAddr == CanonicalizeGxMainRamAddress(listAddr) &&
                                     entry.nbytes == nbytes && entry.layoutHash == layoutHash;
        if (identityMatches && probe.writeGeneration != kDlWriteGenerationUntracked &&
            record.writeGeneration == probe.writeGeneration) {
            // Nothing has flushed a write over these bytes since the digest was
            // taken, so the stored digest still describes them. Skip XXH3.
            probe.record = &record;
            probe.contentDigest = record.contentDigest;
            probe.digestValid = true;
        } else {
            probe.contentDigest = DlContentDigest(list, nbytes);
            probe.digestValid = true;
            if (identityMatches &&
                GxDisplayListScanCache::CanReuse(entry.listAddr, entry.nbytes, entry.layoutHash,
                                                 record.contentDigest, listAddr, nbytes, layoutHash,
                                                 probe.contentDigest)) {
                // False alarm: the bump did not actually change this list
                record.writeGeneration = probe.writeGeneration;
                probe.record = &record;
            }
        }
    }
    return probe;
}

static size_t DlScanCacheRecordBytes(const DlScanCacheRecord& record) {
    return record.flattened.size() + record.cpWrites.size() * sizeof(DlCpWrite);
}

static void StoreDlScanCache(uint32_t listAddr, uint32_t nbytes, uint64_t layoutHash,
                             uint64_t contentDigest, uint64_t writeGeneration,
                             bool mayContainDraw, const DlScanCacheEntry& entry,
                             std::vector<DlCpWrite>&& cpWrites, const uint8_t* flattened,
                             uint32_t flattenedBytes) {
    auto& s_cache = DlScanCache();
    const uint64_t key = DlScanCacheKey(listAddr, nbytes, layoutHash);
    auto old = s_cache.entries.find(key);
    bool replacing = old != s_cache.entries.end();
    size_t replacedBytes = replacing ? DlScanCacheRecordBytes(old->second) : 0;
    const size_t incomingBytes = static_cast<size_t>(flattenedBytes) +
                                 cpWrites.size() * sizeof(DlCpWrite);
    if ((!replacing && s_cache.entries.size() >= kDlScanCacheMaxEntries) ||
        s_cache.storedCommandBytes - replacedBytes + incomingBytes > kDlScanCacheMaxStoredBytes) {
        s_cache.entries.clear();
        s_cache.storedCommandBytes = 0;
        replacing = false;
        replacedBytes = 0;
    }
    DlScanCacheRecord record{};
    record.result = entry;
    record.mayContainDraw = mayContainDraw;
    record.result.listAddr = CanonicalizeGxMainRamAddress(listAddr);
    record.result.nbytes = nbytes;
    record.result.layoutHash = layoutHash;
    record.contentDigest = contentDigest;
    // Sampled before the digest was taken, so a write racing the digest can only
    // make the next call re-digest, never make it trust a stale entry.
    record.writeGeneration = writeGeneration;
    record.cpWrites = std::move(cpWrites);
    if (flattened != nullptr && flattenedBytes != 0) {
        record.flattened.assign(flattened, flattened + flattenedBytes);
    }
    auto [it, inserted] = s_cache.entries.insert_or_assign(key, std::move(record));
    (void)inserted;
    s_cache.storedCommandBytes += DlScanCacheRecordBytes(it->second);
    if (replacing) {
        s_cache.storedCommandBytes -= replacedBytes;
    }
}

static void BuildScanVertexLayout(GXVtxFmt vtxfmt, std::array<ScanAttrStep, 26>& indexedSteps,
                                  uint32_t& indexedStepCount, uint32_t& vertexSize) {
    indexedStepCount = 0;
    vertexSize = 0;
    for (int attr = 0; attr < 26; ++attr) {
        const auto type = g_hleGxState.vtxDesc[attr];
        if (type == GX_NONE) {
            continue;
        }
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        ScanAttrStep step{};
        step.attr = gxAttr;
        step.offset = vertexSize;
        if (IsMatrixIndexAttr(gxAttr)) {
            step.size = 1;
        } else if (type == GX_INDEX8) {
            step.size = (gxAttr == GX_VA_NRM) ? NormalIndexCount(g_hleGxState.vtxAttrFmt[vtxfmt][attr]) : 1u;
            step.indexed = true;
        } else if (type == GX_INDEX16) {
            step.size = ((gxAttr == GX_VA_NRM) ? NormalIndexCount(g_hleGxState.vtxAttrFmt[vtxfmt][attr]) : 1u) * 2u;
            step.indexed = true;
            step.index16 = true;
        } else if (type == GX_DIRECT) {
            // Matrix attributes are handled above, so this reports 0 for them.
            step.size = DirectAttrByteSize(gxAttr, g_hleGxState.vtxAttrFmt[vtxfmt][attr],
                                           /*matrixAttrIsOneByte=*/false, /*fallbackComps=*/0u);
        }
        if (step.size == 0) {
            continue;
        }
        if (step.indexed) {
            indexedSteps[indexedStepCount++] = step;
        }
        // Every step contributes to the vertex stride, indexed or not: the
        // offsets the indexed steps carry are relative to the whole vertex.
        vertexSize += step.size;
    }
}

static const ScanLayoutCacheEntry& GetScanVertexLayout(GXVtxFmt vtxfmt,
                                                       std::array<ScanLayoutCacheEntry, 8>& cache,
                                                       uint32_t version,
                                                       uint32_t generation) {
    auto& entry = cache[static_cast<uint32_t>(vtxfmt)];
    if (entry.valid && entry.version == version && entry.generation == generation) {
        return entry;
    }
    BuildScanVertexLayout(vtxfmt, entry.indexedSteps, entry.indexedStepCount, entry.vertexSize);
    entry.version = version;
    entry.generation = generation;
    entry.valid = true;
    return entry;
}

static void ScanDLVerticesMaxIndices(const uint8_t* ptr, uint16_t vtxCount,
                                     const std::array<ScanAttrStep, 26>& steps, uint32_t stepCount,
                                     uint32_t vertexSize,
                                     std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx,
                                     std::array<bool, GX_VA_MAX_ATTR>& sawIdx) {
    if (stepCount == 0) {
        return;
    }
    const uint8_t* vertex = ptr;
    for (uint16_t v = 0; v < vtxCount; ++v, vertex += vertexSize) {
        for (uint32_t i = 0; i < stepCount; ++i) {
            const auto& step = steps[i];
            const int attr = static_cast<int>(step.attr);
            const uint32_t indexSize = step.index16 ? 2u : 1u;
            const uint32_t indexCount = std::max(1u, step.size / indexSize);
            for (uint32_t indexIdx = 0; indexIdx < indexCount; ++indexIdx) {
                const uint8_t* indexPtr = vertex + step.offset + indexIdx * indexSize;
                const uint32_t index = step.index16 ? ReadBE16(indexPtr) : *indexPtr;
                if (!sawIdx[attr] || index > maxIdx[attr]) {
                    maxIdx[attr] = index;
                    sawIdx[attr] = true;
                }
            }
        }
    }
}


static void AppendBytes(std::vector<uint8_t>& out, const uint8_t* data, uint32_t count) {
    out.insert(out.end(), data, data + count);
}

template <class Visitor>
static bool WalkDisplayList(const uint8_t* data, uint32_t nbytes, Visitor& visitor, int depth) {
    if (depth > Visitor::kMaxDepth) {
        return false;
    }
    uint32_t pos = 0;
    while (pos < nbytes) {
        const uint8_t* const cmdPtr = data + pos;
        const uint8_t cmd = data[pos++];
        if (cmd == GX_NOP_CMD) {
            if (!visitor.OnNop(cmd)) return false;
            continue;
        }
        if (cmd == GX_LOAD_BP_REG_CMD) {
            if (pos + 4u > nbytes) return false;
            if (!visitor.OnBpReg(cmdPtr, ReadBE32(data + pos))) return false;
            pos += 4u;
            continue;
        }
        const uint8_t opcode = cmd & GX_OPCODE_MASK_CMD;
        if (opcode == GX_LOAD_CP_REG_CMD) {
            if (pos + 5u > nbytes) return false;
            if (!visitor.OnCpReg(cmdPtr, data[pos], ReadBE32(data + pos + 1))) return false;
            pos += 5u;
            continue;
        }
        if (opcode == GX_LOAD_XF_REG_CMD) {
            // Overflow-safe spelling of "pos + 4 <= nbytes && pos + payload <= nbytes".
            if (pos > nbytes || nbytes - pos < 4u) return false;
            const uint16_t count = ReadBE16(data + pos);
            const uint32_t payloadBytes = 4u + (static_cast<uint32_t>(count) + 1u) * 4u;
            if (payloadBytes > nbytes - pos) return false;
            if (!visitor.OnXfReg(cmdPtr, payloadBytes)) return false;
            pos += payloadBytes;
            continue;
        }
        if (opcode >= GX_LOAD_INDX_A_CMD && opcode <= GX_LOAD_INDX_D_CMD) {
            if (pos + 4u > nbytes) return false;
            if (!visitor.OnIndexedXf(cmdPtr, cmd, ReadBE32(data + pos))) return false;
            pos += 4u;
            continue;
        }
        if (opcode == GX_CMD_CALL_DL_CMD) {
            if (pos + 8u > nbytes) return false;
            const uint32_t addr = ReadBE32(data + pos);
            const uint32_t size = ReadBE32(data + pos + 4);
            pos += 8u;
            if (!visitor.OnCallDisplayList(addr, size, depth)) return false;
            continue;
        }
        if (opcode == GX_CMD_INVL_VC_CMD) {
            if (!visitor.OnInvalidateVertexCache(cmd)) return false;
            continue;
        }
        if constexpr (Visitor::kHandlesDraw) {
            if (IsDrawOpcode(opcode)) {
                const GXVtxFmt vtxfmt = static_cast<GXVtxFmt>(cmd & GX_VAT_MASK_CMD);
                // Resolved before the count bounds check: the index scan latches
                // the list's vertex format here, and for the other two this is a
                // pure function of the current layout.
                const uint32_t vertexSize = visitor.DrawVertexSize(vtxfmt);
                if (pos + 2u > nbytes) return false;
                const uint16_t vtxCount = ReadBE16(data + pos);
                pos += 2u;
                uint32_t payloadBytes = static_cast<uint32_t>(vtxCount) * vertexSize;
                if (pos + payloadBytes > nbytes) return false;
                if (!visitor.OnDraw(cmdPtr, opcode, vtxfmt, vtxCount, data + pos, payloadBytes)) {
                    return false;
                }
                pos += payloadBytes;
                continue;
            }
        }
        if (!visitor.OnUnknownCommand(cmd)) return false;
    }
    return true;
}

// Does this list carry anything that can produce geometry? Anything the walker
// cannot model counts as "yes", which is why every hook that would have to look
// at a payload just fails the walk instead.
struct DlMayContainDrawVisitor {
    static constexpr int kMaxDepth = 8;
    static constexpr bool kHandlesDraw = false;
    bool* sawCall = nullptr;  // set when the walk meets a call-DL command (at any depth)

    bool OnNop(uint8_t) { return true; }
    bool OnBpReg(const uint8_t*, uint32_t) { return true; }
    // An array base/stride write means the list is setting up indexed geometry.
    bool OnCpReg(const uint8_t*, uint8_t reg, uint32_t) { return !(reg >= 0xA0 && reg <= 0xBF); }
    bool OnXfReg(const uint8_t*, uint32_t) { return true; }
    bool OnIndexedXf(const uint8_t*, uint8_t, uint32_t) { return true; }
    bool OnInvalidateVertexCache(uint8_t) { return true; }
    // Draw opcodes reach this too (kHandlesDraw is false): a draw opcode, or a
    // command byte this walker does not model, both mean "assume it draws".
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (sawCall) *sawCall = true;
        if (addr == 0 || size == 0) return true;
        const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
        if (!nested) return false;
        DlMayContainDrawVisitor child;
        child.sawCall = sawCall;
        return WalkDisplayList(nested, size, child, depth + 1);
    }
};

static bool DisplayListMayContainDraw(const uint8_t* data, uint32_t nbytes, bool* sawCall = nullptr) {
    DlMayContainDrawVisitor visitor;
    visitor.sawCall = sawCall;
    return !WalkDisplayList(data, nbytes, visitor, 0);
}

// Interpreter fallback
struct DlInterpretVisitor {
    static constexpr int kMaxDepth = 8;
    static constexpr bool kHandlesDraw = true;

    bool OnNop(uint8_t) { return true; }
    bool OnInvalidateVertexCache(uint8_t) { return true; }
    bool OnUnknownCommand(uint8_t) { return true; }
    bool OnCpReg(const uint8_t*, uint8_t reg, uint32_t value) { ApplyCpRegWrite(reg, value); return true; }
    bool OnBpReg(const uint8_t*, uint32_t bpWord) {
        GXApplyBPReg(static_cast<uint8_t>(bpWord >> 24), bpWord & 0x00FFFFFFu);
        return true;
    }
    bool OnXfReg(const uint8_t* cmdPtr, uint32_t payloadBytes) {
        GXCallDisplayList(cmdPtr, 1u + payloadBytes);
        GXMarkFrameWork();
        return true;
    }
    bool OnIndexedXf(const uint8_t* cmdPtr, uint8_t cmd, uint32_t value) {
        ApplyIndexedXfArrayForPacket(cmd, value);
        GXCallDisplayList(cmdPtr, 5);
        GXMarkFrameWork();
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (addr == 0 || size == 0) return true;
        const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
        if (!nested) return true;
        DlInterpretVisitor child;
        WalkDisplayList(nested, size, child, depth + 1);
        return true;
    }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) { return CalcDLVertexSize(vtxfmt); }
    bool OnDraw(const uint8_t*, uint8_t opcode, GXVtxFmt vtxfmt, uint16_t vtxCount,
                const uint8_t* vertices, uint32_t& payloadBytes) {
        EnsureAuroraFrameActive();
        const HleGxVertexStateSnapshot drawGuestState = CaptureGxVertexState();
        ApplyAuroraVtxStateForDlBegin(vtxfmt);
        GXBegin(OpcodeToGXPrimitive(opcode), vtxfmt, vtxCount);
        GXMarkFrameWork();
        // The per-vertex decode is what advances the cursor here, exactly as
        // before the walk was shared: report what it actually consumed rather
        // than the vtxCount * stride product the walker precomputed.
        uint32_t consumed = 0;
        for (uint16_t v = 0; v < vtxCount; ++v) {
            consumed += SubmitDLVertex(vertices + consumed, vtxfmt, drawGuestState.vtxDesc);
        }
        GXEnd();
        RestoreGxVertexState(drawGuestState);
        payloadBytes = consumed;
        return true;
    }
};

static void ParseDisplayList(const uint8_t* data, uint32_t nbytes) {
    DlInterpretVisitor visitor;
    WalkDisplayList(data, nbytes, visitor, 0);
}

struct DlIndexScanVisitor {
    static constexpr int kMaxDepth = 4;
    static constexpr bool kHandlesDraw = true;

    static std::array<ScanLayoutCacheEntry, 8>& LayoutCache() {
        static thread_local std::array<ScanLayoutCacheEntry, 8> cache;
        return cache;
    }
    static uint32_t NextLayoutGeneration() {
        static thread_local uint32_t counter = 0;
        return ++counter;
    }

    std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx;
    std::array<bool, GX_VA_MAX_ATTR>& sawIdx;
    std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfIdx;
    std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfBytes;
    std::array<bool, GX_VA_MAX_ATTR>& sawXfIdx;
    GXVtxFmt* outFmt;
    bool* outFmtMixed;
    bool* outHasNestedDl;
    bool* outHasArrayStateWrites;
    bool* outHasCpWrites;
    std::vector<DlCpWrite>* outCpWrites;
    // Every walk - including each nested one - gets its own layout generation, so
    // a cached layout can only be reused inside the walk that built it.
    uint32_t layoutVersion = 1;
    uint32_t layoutGeneration = NextLayoutGeneration();
    const ScanLayoutCacheEntry* pendingLayout = nullptr;

    bool OnNop(uint8_t) { return true; }
    bool OnBpReg(const uint8_t*, uint32_t) { return true; }
    bool OnXfReg(const uint8_t*, uint32_t) { return true; }
    bool OnInvalidateVertexCache(uint8_t) { return true; }
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnCpReg(const uint8_t*, uint8_t reg, uint32_t value) {
        if (outHasCpWrites) *outHasCpWrites = true;
        if (reg >= 0xA0 && reg <= 0xBF && outHasArrayStateWrites) *outHasArrayStateWrites = true;
        if (outCpWrites != nullptr) outCpWrites->push_back(DlCpWrite{reg, value});
        ApplyCpRegWrite(reg, value);
        if (reg == 0x50 || reg == 0x60 || (reg >= 0x70 && reg <= 0x97)) ++layoutVersion;
        return true;
    }
    bool OnIndexedXf(const uint8_t*, uint8_t cmd, uint32_t value) {
        const int attr =
            GX_POS_MTX_ARRAY + (((cmd & GX_OPCODE_MASK_CMD) - GX_LOAD_INDX_A_CMD) / 0x08);
        const uint32_t index = value >> 16;
        const uint32_t bytes = (((value >> 12) & 0x0fu) + 1u) * 4u;
        if (attr >= 0 && attr < GX_VA_MAX_ATTR) {
            if (!sawXfIdx[attr] || index > maxXfIdx[attr]) maxXfIdx[attr] = index;
            if (bytes > maxXfBytes[attr]) maxXfBytes[attr] = bytes;
            sawXfIdx[attr] = true;
        }
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (addr == 0 || size == 0) return true;
        if (outHasNestedDl) *outHasNestedDl = true;
        try {
            const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
            if (nested) {
                DlIndexScanVisitor child{maxIdx,         sawIdx,         maxXfIdx,
                                         maxXfBytes,     sawXfIdx,       outFmt,
                                         outFmtMixed,    outHasNestedDl, outHasArrayStateWrites,
                                         outHasCpWrites, outCpWrites};
                if (!WalkDisplayList(nested, size, child, depth + 1)) return false;
            }
            ++layoutVersion;
        } catch (...) {
            return false;
        }
        return true;
    }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) {
        if (outFmt) {
            if (*outFmt == GX_MAX_VTXFMT) *outFmt = vtxfmt;
            else if (*outFmt != vtxfmt && outFmtMixed) *outFmtMixed = true;
        }
        pendingLayout = &GetScanVertexLayout(vtxfmt, LayoutCache(), layoutVersion, layoutGeneration);
        return pendingLayout->vertexSize;
    }
    bool OnDraw(const uint8_t*, uint8_t, GXVtxFmt, uint16_t vtxCount, const uint8_t* vertices,
                uint32_t&) {
        ScanDLVerticesMaxIndices(vertices, vtxCount, pendingLayout->indexedSteps,
                                 pendingLayout->indexedStepCount, pendingLayout->vertexSize, maxIdx,
                                 sawIdx);
        return true;
    }
};

static bool ScanDisplayListMaxIndices(const uint8_t* data, uint32_t nbytes,
                                      std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx,
                                      std::array<bool, GX_VA_MAX_ATTR>& sawIdx,
                                      std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfIdx,
                                      std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfBytes,
                                      std::array<bool, GX_VA_MAX_ATTR>& sawXfIdx,
                                      GXVtxFmt* outFmt, bool* outFmtMixed, bool* outHasNestedDl,
                                      bool* outHasArrayStateWrites, bool* outHasCpWrites,
                                      std::vector<DlCpWrite>* outCpWrites) {
    DlIndexScanVisitor visitor{maxIdx,         sawIdx,         maxXfIdx,
                               maxXfBytes,     sawXfIdx,       outFmt,
                               outFmtMixed,    outHasNestedDl, outHasArrayStateWrites,
                               outHasCpWrites, outCpWrites};
    return WalkDisplayList(data, nbytes, visitor, 0);
}

// Inlines nested lists into one flat command stream aurora can consume directly.
// `outVerbatim` stays true only while every consumed command is appended
// unchanged, i.e. the flattened stream is a byte-for-byte copy of the source
// list. Callers use that to hand aurora the guest bytes directly.
struct DlFlattenVisitor {
    static constexpr int kMaxDepth = 8;
    static constexpr bool kHandlesDraw = true;

    std::vector<uint8_t>& out;
    bool skipArrayState;
    bool* outVerbatim;

    bool OnNop(uint8_t cmd) { out.push_back(cmd); return true; }
    bool OnInvalidateVertexCache(uint8_t cmd) { out.push_back(cmd); return true; }
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnBpReg(const uint8_t* cmdPtr, uint32_t) { AppendBytes(out, cmdPtr, 5); return true; }
    bool OnIndexedXf(const uint8_t* cmdPtr, uint8_t, uint32_t) { AppendBytes(out, cmdPtr, 5); return true; }
    bool OnXfReg(const uint8_t* cmdPtr, uint32_t payloadBytes) {
        AppendBytes(out, cmdPtr, 1u + payloadBytes);
        return true;
    }
    // Command byte + the 2-byte vertex count + the vertex payload.
    bool OnDraw(const uint8_t* cmdPtr, uint8_t, GXVtxFmt, uint16_t, const uint8_t*,
                uint32_t& payloadBytes) {
        AppendBytes(out, cmdPtr, 3u + payloadBytes);
        return true;
    }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) { return CalcDLVertexSize(vtxfmt); }
    bool OnCpReg(const uint8_t* cmdPtr, uint8_t reg, uint32_t value) {
        const bool arrayBaseOrStride = (reg >= 0xA0 && reg <= 0xBF);
        if (!skipArrayState || !arrayBaseOrStride) {
            AppendBytes(out, cmdPtr, 6);
        } else if (outVerbatim != nullptr) {
            *outVerbatim = false;
        }
        ApplyCpRegWrite(reg, value);
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int depth) {
        if (outVerbatim != nullptr) *outVerbatim = false;
        if (addr == 0 || size == 0) return true;
        const uint8_t* nested = static_cast<const uint8_t*>(GuestToHostPtr(addr, size));
        if (!nested) return true;
        DlFlattenVisitor child{out, skipArrayState, outVerbatim};
        return WalkDisplayList(nested, size, child, depth + 1);
    }
};

static bool FlattenDisplayListForAurora(const uint8_t* data, uint32_t nbytes,
                                        std::vector<uint8_t>& out, bool skipArrayState,
                                        bool* outVerbatim = nullptr) {
    DlFlattenVisitor visitor{out, skipArrayState, outVerbatim};
    return WalkDisplayList(data, nbytes, visitor, 0);
}

static std::vector<uint8_t>& FlattenDisplayListScratch() {
    static thread_local std::vector<uint8_t> s_scratch;
    return s_scratch;
}

// The applied-state mirror is per producer thread (a draw worker filling deferred jobs has its own; g3d_native.cpp).
static thread_local bool s_appliedVtxDescValid = false;
static thread_local std::array<GXAttrType, 26> s_appliedVtxDesc{};
static thread_local std::array<GXAttrType, 26> s_appliedSourceVtxDesc{};
static thread_local bool s_appliedVtxAttrFmtValid = false;
static thread_local VtxAttrFmt s_appliedVtxAttrFmt[8][26]{};

struct AppliedArrayState {
    const void* data = nullptr;
    uint32_t size = 0;
    uint8_t stride = 0;
    bool valid = false;
};

static thread_local std::array<AppliedArrayState, GX_VA_MAX_ATTR> s_appliedArrays{};

// True only while the applied-state mirror is a verbatim copy of HLE vertex state at
// generation s_appliedVtxStateGeneration; any HLE-side mutation bumps that generation, so the
// memoized republish below can never observe a stale match.
static thread_local bool s_appliedVtxStateMatchesHle = false;
static thread_local uint32_t s_appliedVtxStateGeneration = 0;

static void InvalidateAppliedAuroraState() {
    s_appliedVtxDescValid = false;
    s_appliedVtxAttrFmtValid = false;
    s_appliedVtxStateMatchesHle = false;
    for (auto& array : s_appliedArrays) {
        array.valid = false;
    }
}

static void SyncAppliedVtxStateFromHleReal() {
    // skip the republish only when the mirror is already an exact copy of
    // HLE state
    if (s_appliedVtxStateMatchesHle && s_appliedVtxDescValid && s_appliedVtxAttrFmtValid &&
        s_appliedVtxStateGeneration == g_hleGxState.vtxStateGeneration) {
        return;
    }
    // Both mirrors are dense arrays of the same trivially copyable element types
    // as their HLE counterparts, so publish them with three memcpys instead of
    // 234 individual element assignments.
    static_assert(sizeof(s_appliedVtxDesc) == sizeof(g_hleGxState.vtxDesc));
    static_assert(sizeof(s_appliedSourceVtxDesc) == sizeof(g_hleGxState.vtxDesc));
    static_assert(sizeof(s_appliedVtxAttrFmt) == sizeof(g_hleGxState.vtxAttrFmt));
    std::memcpy(s_appliedVtxDesc.data(), g_hleGxState.vtxDesc, sizeof(g_hleGxState.vtxDesc));
    std::memcpy(s_appliedSourceVtxDesc.data(), g_hleGxState.vtxDesc, sizeof(g_hleGxState.vtxDesc));
    s_appliedVtxDescValid = true;

    std::memcpy(s_appliedVtxAttrFmt, g_hleGxState.vtxAttrFmt, sizeof(g_hleGxState.vtxAttrFmt));
    s_appliedVtxAttrFmtValid = true;

    s_appliedVtxStateMatchesHle = true;
    s_appliedVtxStateGeneration = g_hleGxState.vtxStateGeneration;
}

static void ApplyAuroraArrayIfChanged(GXAttr attr, const void* data, uint32_t size, uint8_t stride) {
    auto& applied = s_appliedArrays[attr];
    if (applied.valid && applied.data == data && applied.size == size && applied.stride == stride) {
        return;
    }
    GXSetArray(attr, data, size, stride);
    applied.data = data;
    applied.size = size;
    applied.stride = stride;
    applied.valid = true;
}

// Aurora's live vertex descriptor for one attribute, read straight back out of
// its CP shadow registers (GXGetVtxDesc is the exact inverse of GXSetVtxDesc,
// GX_VA_NRM included). This is the ground truth the applied-state mirror is
// checked against below; it is a switch plus a shift/mask, no state change and
// no FIFO traffic.
static bool AuroraVtxDescMatchesLive(GXAttr attr, GXAttrType expected) {
    GXAttrType live = GX_NONE;
    GXGetVtxDesc(attr, &live);
    return live == expected;
}


static void ApplyAuroraVtxDesc() {
    const bool hadValidState = s_appliedVtxDescValid;
    if (!hadValidState) {
        s_appliedVtxStateMatchesHle = false;
    }
    for (int attr = 0; attr < 26; ++attr) {
        if (attr == GX_VA_NBT) continue;
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const GXAttrType type = g_hleGxState.vtxDesc[attr];
        bool needsPublish = !hadValidState || s_appliedVtxDesc[attr] != type;
        // GXGetVtxDesc only decodes GX_VA_PNMTXIDX..GX_VA_TEX7; the XF array
        // pseudo-attributes above GX_VA_TEX7 have no CP descriptor bits, so
        // probing them would always report GX_NONE and republish a no-op.
        if (!needsPublish && !IsMatrixIndexAttr(gxAttr) && gxAttr <= GX_VA_TEX7 &&
            (type == GX_INDEX8 || type == GX_INDEX16)) {
            needsPublish = !AuroraVtxDescMatchesLive(gxAttr, type);
        }
        if (needsPublish) {
            GXSetVtxDesc(gxAttr, type);
            s_appliedVtxDesc[attr] = type;
        }
        if (!hadValidState || s_appliedSourceVtxDesc[attr] != type) {
            GXSetSourceVtxDesc(gxAttr, type);
            s_appliedSourceVtxDesc[attr] = type;
        }
    }
    s_appliedVtxDescValid = true;
}

// Size of one element of the guest array an indexed attribute reads from. An
// NBT3 normal is indexed per 3-component group, not per 9-component triple.
static uint32_t GetIndexedAttrElementSizeBytes(GXAttr attr, const VtxAttrFmt& fmt) {
    if (attr == GX_VA_NRM && fmt.cnt == GX_NRM_NBT3) {
        return 3u * static_cast<uint32_t>(GetCompSizeBytes(fmt.type));
    }
    return DirectAttrByteSize(attr, fmt, /*matrixAttrIsOneByte=*/false, /*fallbackComps=*/0u);
}

static void ApplyAuroraVtxAttrFmt(GXVtxFmt fmt) {
    const bool hadValidState = s_appliedVtxAttrFmtValid;
    // The row is byte-identical to what was last applied: the loop below would skip every attribute. One compare
    // instead of 24 per display list (6% of the main thread in the menus); any difference, padding included,
    // takes the exact per-attribute path.
    if (hadValidState && std::memcmp(s_appliedVtxAttrFmt[fmt], g_hleGxState.vtxAttrFmt[fmt],
                                     sizeof(s_appliedVtxAttrFmt[fmt])) == 0) {
        return;
    }
    // Rows other than `fmt` keep their contents and every write below copies
    // g_hleGxState, so an already-matching mirror still matches. Only the rebuild
    // from an invalidated mirror leaves the untouched rows divergent.
    if (!hadValidState) {
        s_appliedVtxStateMatchesHle = false;
    }
    for (int attr = 0; attr < 26; ++attr) {
        if (attr == GX_VA_NBT) continue;
        if (IsMatrixIndexAttr(static_cast<GXAttr>(attr))) continue;
        const auto& f = g_hleGxState.vtxAttrFmt[fmt][attr];
        if (hadValidState && SameVtxAttrFmt(s_appliedVtxAttrFmt[fmt][attr], f)) {
            continue;
        }
        GXSetVtxAttrFmt(fmt, static_cast<GXAttr>(attr), f.cnt, f.type, f.frac);
        s_appliedVtxAttrFmt[fmt][attr] = f;
    }
    // Mirror the whole row (the never-applied NBT and matrix-index entries too, as the full republish does), so an
    // unchanged row matches the compare above next time.
    std::memcpy(s_appliedVtxAttrFmt[fmt], g_hleGxState.vtxAttrFmt[fmt], sizeof(s_appliedVtxAttrFmt[fmt]));
    s_appliedVtxAttrFmtValid = true;
}

static void ApplyAuroraVtxAttrFmtAll() {
    for (int fmt = 0; fmt < 8; ++fmt) {
        ApplyAuroraVtxAttrFmt(static_cast<GXVtxFmt>(fmt));
    }
}

static void ApplyAuroraVtxAttrFmtForDisplayList(GXVtxFmt fmt, bool mixed) {
    if (!mixed && fmt < GX_MAX_VTXFMT) {
        ApplyAuroraVtxAttrFmt(fmt);
        return;
    }
    ApplyAuroraVtxAttrFmtAll();
}

static void SubmitLytDrawPacket(const uint8_t* packet, uint32_t packetBytes) {
    if (packet == nullptr || packetBytes == 0) {
        return;
    }
    EnsureAuroraFrameActive();
    ApplyAuroraVtxDesc();
    ApplyAuroraVtxAttrFmtForDisplayList(GX_VTXFMT0, false);
    EnsureDefaultGxAlphaCompare();

    GXCallDisplayList(packet, packetBytes);
    GXMarkFrameWork();
    SyncAppliedVtxStateFromHleReal();
}

static bool ApplyAuroraArraysForDisplayList(const std::array<uint32_t, GX_VA_MAX_ATTR>& maxIdx,
                                            const std::array<bool, GX_VA_MAX_ATTR>& sawIdx,
                                            GXVtxFmt vtxfmt,
                                            bool vtxfmtMixed) {
    static thread_local std::array<std::array<std::vector<uint8_t>, 2>, GX_VA_MAX_ATTR> s_packedArrays;
    static thread_local std::array<uint32_t, GX_VA_MAX_ATTR> s_packedArrayCursor{};
    bool ok = true;
    if (vtxfmt >= GX_MAX_VTXFMT) {
        vtxfmt = g_hleGxState.currentVtxFmt;
    }
    for (int attr = 0; attr < 26; ++attr) {
        const GXAttrType type = g_hleGxState.vtxDesc[attr]; if (type != GX_INDEX8 && type != GX_INDEX16) continue;
        if (!sawIdx[attr]) continue;
        const auto& arr = g_hleGxState.vtxArray[attr];
        if (arr.base == 0 || arr.stride == 0) { ok = false; continue; }
        const uint32_t count = maxIdx[attr] + 1u;
        const GXAttr gxAttr = static_cast<GXAttr>(attr);
        const auto& fmt = g_hleGxState.vtxAttrFmt[vtxfmt][attr];
        const uint32_t elemSize = GetIndexedAttrElementSizeBytes(gxAttr, fmt);
        if (elemSize == 0) {
            ok = false;
            continue;
        }
        const uint32_t span = (count > 0) ? (static_cast<uint32_t>((count - 1u) * arr.stride) + elemSize) : 0u;
        const bool needsPack = arr.stride > 0xffu;
        if (!Memory::Contains(arr.base, span)) {
            ok = false;
            continue;
        }
        const uint8_t* hostPtr = static_cast<const uint8_t*>(GuestToHostPtr(arr.base, span));
        if (!hostPtr) {
            ok = false;
            continue;
        }
        // Aurora's storage-fetch shaders consume guest big-endian bytes directly
        // and honor the array stride. Keep the backing pointer stable so per-frame
        // storage uploads can be cached instead of repacking every display-list call.
        if (!needsPack) {
            ApplyAuroraArrayIfChanged(gxAttr, hostPtr, span, static_cast<uint8_t>(arr.stride));
            continue;
        }
        // Only two packed buffers per attribute alternate, and Aurora reads the array through this pointer when it
        // decodes: with its GX thread on, that may be after this returns, so it must be done with the older one.
        aurora_gx_sync();
        auto& packed = s_packedArrays[attr][++s_packedArrayCursor[attr] & 1u];
        if (!PackIndexedAttrData(hostPtr, arr.stride, count, elemSize, packed)) {
            ok = false;
            continue;
        }
        ApplyAuroraArrayIfChanged(gxAttr, packed.data(), static_cast<uint32_t>(packed.size()), static_cast<uint8_t>(elemSize));
    }
    return ok;
}

static bool ApplyAuroraIndexedXFArraysForDisplayList(const std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfIdx,
                                                     const std::array<uint32_t, GX_VA_MAX_ATTR>& maxXfBytes,
                                                     const std::array<bool, GX_VA_MAX_ATTR>& sawXfIdx) {
    bool ok = true;
    for (int attr = GX_POS_MTX_ARRAY; attr <= GX_LIGHT_ARRAY; ++attr) {
        if (!sawXfIdx[attr]) {
            continue;
        }
        const auto& arr = g_hleGxState.vtxArray[attr];
        const uint32_t bytes = maxXfBytes[attr];
        if (arr.base == 0 || arr.stride == 0 || bytes == 0) {
            ok = false;
            continue;
        }
        const uint32_t span = maxXfIdx[attr] * arr.stride + bytes;
        if (!Memory::Contains(arr.base, span)) {
            ok = false;
            continue;
        }
        const uint8_t* hostPtr = static_cast<const uint8_t*>(GuestToHostPtr(arr.base, span));
        if (!hostPtr) {
            ok = false;
            continue;
        }
        ApplyAuroraArrayIfChanged(static_cast<GXAttr>(attr), hostPtr, span, static_cast<uint8_t>(arr.stride));
    }
    return ok;
}

} // namespace

void ForgetAppliedAuroraArray(int attr) {
    if (attr >= 0 && attr < GX_VA_MAX_ATTR) s_appliedArrays[attr].valid = false;
}

extern "C" void GxNotifyDisplayListMemoryWrite(uint32_t addr, uint32_t size) {
    GxGuestWrite::NotifyWrite(addr, size);
}

// SPM records each list into one scratch buffer and copies it elsewhere, so a list Aurora encoded is
// recognized by content: walk the register commands until a GX_LOAD_AURORA (0x50) command shows up.
// A draw or an unknown byte ends the walk (those lists keep the runtime's own path).
[[maybe_unused]] static bool ListHasAuroraCommand(const uint8_t* data, uint32_t n) {
    uint32_t pos = 0;
    while (pos < n) {
        const uint8_t cmd = data[pos++];
        switch (cmd & 0xF8) {
        case 0x00: if (cmd != 0) return false; break;          // NOP
        case 0x50: return cmd == 0x50;                          // GX_LOAD_AURORA
        case 0x60: if (cmd != 0x61) return false; pos += 4; break;  // BP
        case 0x08: pos += 5; break;                             // CP
        case 0x10: {                                            // XF
            if (pos + 2 > n) return false;
            const uint32_t count = (static_cast<uint32_t>(data[pos]) << 8) | data[pos + 1];
            pos += 4 + (count + 1) * 4;
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38: pos += 4; break;  // indexed XF loads
        case 0x40: pos += 8; break;                             // call DL
        case 0x48: break;                                       // invalidate vertex cache
        default: return false;                                  // draw or unknown
        }
    }
    return false;
}

// Horizon perf spans inside GX__CallDisplayList_HLE: the ticks since the last mark go to a slot.
#if MKW_HZ_PERF && MKW_HZ_PERF_HOT
#define HZ_DL_MARK(var) const uint64_t var = HzTicks()
#define HZ_DL_SPAN(slot, since) HZ_PERF_ADD(slot, HzTicks() - (since))
#else
#define HZ_DL_MARK(var) ((void)0)
#define HZ_DL_SPAN(slot, since) ((void)0)
#endif

extern "C" void GX__CallDisplayList_HLE(uint32_t listAddr, uint32_t nbytes) {
    GX_HLE_SyncGuestProjection();
    if (nbytes == 0 || listAddr == 0) return;
    // Recorded as the FIFO command GXFastCallDisplayList writes, so both ways of calling a list compare equal.
    const uint8_t g3dRecCall[9] = {0x40, uint8_t(listAddr >> 24), uint8_t(listAddr >> 16), uint8_t(listAddr >> 8),
                                   uint8_t(listAddr), uint8_t(nbytes >> 24), uint8_t(nbytes >> 16),
                                   uint8_t(nbytes >> 8), uint8_t(nbytes)};
    g3dn::HleRec g3dRec(g3dn::kRecFifo, g3dRecCall, sizeof(g3dRecCall));
    if (g3dRec.skip) return;
    HZ_PERF_ADD(kHzDlCalls, 1);
    HZ_PERF_ADD(kHzDlBytes, nbytes);
    HZ_PHASE(kHzDlTicks);
    try {
        const uint8_t* list = static_cast<const uint8_t*>(GuestToHostPtr(listAddr, nbytes));
        if (!list) return;
        if (const uint8_t* hostDl = nullptr; true) {
            uint32_t hostSize = 0;
            if (SpmHostDisplayList(list, nbytes, hostDl, hostSize)) {
                g_boundTexMaps.fill(BoundTexInfo{});
                EnsureAuroraFrameActive();
                GXMarkFrameWork();
                GXCallDisplayList(hostDl, hostSize);
                return;
            }
        }

        HZ_DL_MARK(hzProbe);
        const bool allowScanCache = nbytes <= kDlScanCacheMaxEntryBytes;
        const uint64_t scanLayoutHash = allowScanCache ? HashScanLayoutState() : 0;
        // The digest is computed lazily inside the probe: an entry whose covering
        // write-tracking granules have not moved since it was stored keeps its
        // recorded digest and skips the XXH3 pass over the whole list entirely.
        const DlScanCacheProbe probe =
            allowScanCache ? ProbeDlScanCache(list, listAddr, nbytes, scanLayoutHash)
                           : DlScanCacheProbe{};
        const DlScanCacheRecord* cached = probe.record;

        // The scan a cached record came from already walked the list for this,
        // so only a miss pays for DisplayListMayContainDraw. Records are stored
        // only for lists ListHasAuroraCommand rejected, and a hit means the bytes
        // are the ones that record was taken from, so a hit is never an Aurora list.
        const bool auroraList = cached != nullptr ? false : ListHasAuroraCommand(list, nbytes);
        HZ_DL_SPAN(kHzDlProbeTicks, hzProbe);
        if (auroraList) g_boundTexMaps.fill(BoundTexInfo{});
        { static int s_callLog = 0;
          if (s_callLog++ < 60) RT_LOGF(RT_TAG_GX, "[spmdl] call 0x%08X %u bytes aurora=%d\n", listAddr, nbytes, auroraList ? 1 : 0); }  // the list rebinds textures behind the cache
        if (SpmDumpFrame()) {  // the list's bytes and the position-matrix array it indexes
            std::string hex;
            for (uint32_t i = 0; i < nbytes && i < 256; ++i) { char b[4]; std::snprintf(b, sizeof b, "%02X", list[i]); hex += b; }
            RT_LOGF(RT_TAG_GX, "[drawdump] calldl 0x%08X %u: %s\n", listAddr, nbytes, hex.c_str());
            const uint32_t mb = g_hleGxState.vtxArray[GX_POS_MTX_ARRAY].base;
            for (uint32_t k = 0; mb && k < 2; ++k) {
                float m[12];
                for (int j = 0; j < 12; ++j) { const uint32_t bits = Memory::Read32(mb + k * 48 + j * 4); std::memcpy(&m[j], &bits, 4); }
                RT_LOGF(RT_TAG_GX, "[drawdump]   mtx[%u] %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f | %.3f %.3f %.3f %.2f\n", k,
                        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            }
        }
        bool walkSawCall = false;
        const bool mayContainDraw = auroraList ? false
            : (cached != nullptr) ? cached->mayContainDraw : DisplayListMayContainDraw(list, nbytes, &walkSawCall);
        if (!mayContainDraw) {
            // Register-only lists are ~60% of calls in a match. Record the answer so the next call with these
            // bytes is a hit and skips both walks (ListHasAuroraCommand, DisplayListMayContainDraw). Not for
            // Aurora lists (they keep the per-call texture reset above) or lists calling other lists (the
            // digest covers only the outer list), the same rule the draw-list records follow.
            if (cached == nullptr && !auroraList && allowScanCache && !walkSawCall) {
                const uint64_t contentDigest =
                    probe.digestValid ? probe.contentDigest : DlContentDigest(list, nbytes);
                StoreDlScanCache(listAddr, nbytes, scanLayoutHash, contentDigest, probe.writeGeneration,
                                 /*mayContainDraw=*/false, DlScanCacheEntry{}, {}, nullptr, 0u);
            }
            EnsureAuroraFrameActive();
            GXMarkFrameWork();
            GXCallDisplayList(list, nbytes);
            // The republish is required even for register-only lists (verified by
            // experiment: dropping it corrupts in-race geometry decode). It is
            // instead memoized inside SyncAppliedVtxStateFromHleReal, so a run of
            // register-only lists between draws pays for at most one mirror copy.
            SyncAppliedVtxStateFromHleReal();
            return;
        }
        // The per-attribute index ranges are read in place from the cached record, or from a fresh
        // entry built only on a miss: copying them (and zeroing the locals they were copied into)
        // was ~0.7 KB of stores per call on the common, cached path.
        std::optional<DlScanCacheEntry> scanned;
        const DlScanCacheEntry* scan = nullptr;
        GXVtxFmt dlVtxFmt = GX_MAX_VTXFMT; bool dlVtxFmtMixed = false;
        bool dlHasNestedDl = false;
        bool dlHasArrayStateWrites = false;
        bool dlHasCpWrites = false;

        // Built only on the paths that restore it: default-constructing this ~2.5 KB snapshot on every call
        // (10k calls a frame in SPM gn3_12) cost ~0.45 ms/frame, and the cached path rarely needs it.
        std::optional<HleGxVertexStateSnapshot> stateBeforeScan;

        bool scanOk = false;
        bool needsFlatten = false;
        bool flattenOk = false;
        const uint8_t* auroraStream = nullptr;
        uint32_t auroraStreamBytes = 0;
        if (cached != nullptr) {
            const auto& entry = cached->result;
            scan = &entry;
            scanOk = entry.scanOk;
            dlVtxFmt = entry.dlVtxFmt;
            dlVtxFmtMixed = entry.dlVtxFmtMixed;
            dlHasNestedDl = entry.dlHasNestedDl;
            dlHasArrayStateWrites = entry.dlHasArrayStateWrites;
            dlHasCpWrites = entry.dlHasCpWrites;
            needsFlatten = entry.needsFlatten;
            flattenOk = entry.flattenOk;
            // A scan/flatten only mutates HLE vertex state through the CP writes
            // embedded in the list. Replaying the recorded writes in stream order
            // lands g_hleGxState exactly where the uncached path would have left it.
            if (dlHasCpWrites) {
                stateBeforeScan = CaptureGxVertexStateForCpWrites(cached->cpWrites);
            }
            for (const auto& write : cached->cpWrites) {
                ApplyCpRegWrite(write.reg, write.value);
            }
            if (needsFlatten && flattenOk) {
                if (entry.flattenVerbatim) {
                    auroraStream = list;
                    auroraStreamBytes = nbytes;
                } else {
                    auroraStream = cached->flattened.data();
                    auroraStreamBytes = static_cast<uint32_t>(cached->flattened.size());
                }
            }
        } else {
            std::vector<DlCpWrite> cpWrites{};
            // The scan discovers the list's CP writes as it walks, so this path
            // cannot narrow the snapshot the way the cached one does.
            stateBeforeScan = CaptureGxVertexState();
            DlScanCacheEntry& fresh = scanned.emplace();
            scan = &fresh;
            scanOk = ScanDisplayListMaxIndices(list, nbytes, fresh.maxIdx, fresh.sawIdx, fresh.maxXfIdx,
                                               fresh.maxXfBytes, fresh.sawXfIdx,
                                               &dlVtxFmt, &dlVtxFmtMixed, &dlHasNestedDl,
                                               &dlHasArrayStateWrites, &dlHasCpWrites, &cpWrites);
            needsFlatten = dlHasNestedDl || dlHasArrayStateWrites || dlHasCpWrites;
            bool flattenVerbatim = true;
            auto& flattened = FlattenDisplayListScratch();
            if (needsFlatten) {
                RestoreGxVertexState(*stateBeforeScan);
                flattened.clear();
                flattenOk = FlattenDisplayListForAurora(list, nbytes, flattened, dlHasArrayStateWrites,
                                                        &flattenVerbatim);
                if (flattenOk) {
                    if (flattenVerbatim) {
                        auroraStream = list;
                        auroraStreamBytes = nbytes;
                    } else {
                        auroraStream = flattened.data();
                        auroraStreamBytes = static_cast<uint32_t>(flattened.size());
                    }
                }
            }
            // Nested lists stay uncached: the digest only covers the outer list, so a
            // rewritten callee would go undetected. Everything else is cacheable since the
            // scan/flatten are pure functions of the list bytes and the layout-hash cache key.
            if (allowScanCache && scanOk && !dlHasNestedDl && (!needsFlatten || flattenOk)) {
                DlScanCacheEntry& entry = fresh;  // its index ranges are already filled in by the scan
                entry.listAddr = CanonicalizeGxMainRamAddress(listAddr);
                entry.nbytes = nbytes;
                entry.layoutHash = scanLayoutHash;
                entry.dlVtxFmt = dlVtxFmt;
                entry.dlVtxFmtMixed = dlVtxFmtMixed;
                entry.dlHasNestedDl = dlHasNestedDl;
                entry.dlHasArrayStateWrites = dlHasArrayStateWrites;
                entry.dlHasCpWrites = dlHasCpWrites;
                entry.scanOk = scanOk;
                entry.needsFlatten = needsFlatten;
                entry.flattenOk = flattenOk;
                entry.flattenVerbatim = flattenVerbatim;
                const bool storeFlattened = needsFlatten && flattenOk && !flattenVerbatim;

                const uint64_t contentDigest =
                    probe.digestValid ? probe.contentDigest : DlContentDigest(list, nbytes);
                StoreDlScanCache(listAddr, nbytes, scanLayoutHash, contentDigest,
                                 probe.writeGeneration, mayContainDraw, entry, std::move(cpWrites),
                                 storeFlattened ? flattened.data() : nullptr,
                                 storeFlattened ? static_cast<uint32_t>(flattened.size()) : 0u);
            }
        }

        if (needsFlatten) {
            bool flattenApplyOk = flattenOk && scanOk && auroraStream != nullptr && auroraStreamBytes != 0;
            HZ_DL_MARK(hzApply);
            if (flattenApplyOk) {
                ApplyAuroraVtxDesc();
                ApplyAuroraVtxAttrFmtForDisplayList(dlVtxFmt, dlVtxFmtMixed);
                const bool flattenArraysOk =
                    ApplyAuroraArraysForDisplayList(scan->maxIdx, scan->sawIdx, dlVtxFmt, dlVtxFmtMixed);
                const bool flattenXfOk =
                    ApplyAuroraIndexedXFArraysForDisplayList(scan->maxXfIdx, scan->maxXfBytes, scan->sawXfIdx);
                flattenApplyOk = flattenArraysOk && flattenXfOk;
            }
            HZ_DL_SPAN(kHzDlApplyTicks, hzApply);
            if (flattenApplyOk) {
                HZ_DL_MARK(hzAurora);
                EnsureAuroraFrameActive();
                GXMarkFrameWork();
                GXCallDisplayList(auroraStream, auroraStreamBytes);
                HZ_DL_SPAN(kHzDlAuroraTicks, hzAurora);
                // No flip back to GX_DIRECT here
                HZ_DL_MARK(hzSync);
                SyncAppliedVtxStateFromHleReal();
                HZ_DL_SPAN(kHzDlSyncTicks, hzSync);
                return;
            }

        } else {
            bool applyOk = scanOk;
            bool arraysOk = false;
            bool xfOk = false;
            HZ_DL_MARK(hzApply);
            if (scanOk) {
                ApplyAuroraVtxDesc();
                ApplyAuroraVtxAttrFmtForDisplayList(dlVtxFmt, dlVtxFmtMixed);
                arraysOk = ApplyAuroraArraysForDisplayList(scan->maxIdx, scan->sawIdx, dlVtxFmt, dlVtxFmtMixed);
                xfOk = ApplyAuroraIndexedXFArraysForDisplayList(scan->maxXfIdx, scan->maxXfBytes, scan->sawXfIdx);
                applyOk = arraysOk && xfOk;
            }
            HZ_DL_SPAN(kHzDlApplyTicks, hzApply);
            if (applyOk) {
                HZ_DL_MARK(hzAurora);
                EnsureAuroraFrameActive();
                GXMarkFrameWork();
                // The native g3d path's lists (a shape's draws): Aurora may submit them from a compiled form, keyed
                // by the list's content digest (GXCallDisplayListCompiled falls back to decoding the bytes).
                if (g3dn::g_nativeDraw && probe.digestValid)
                    GXCallDisplayListCompiled(list, nbytes, probe.contentDigest);
                else
                    GXCallDisplayList(list, nbytes);
                HZ_DL_SPAN(kHzDlAuroraTicks, hzAurora);

                HZ_DL_MARK(hzSync);
                SyncAppliedVtxStateFromHleReal();
                HZ_DL_SPAN(kHzDlSyncTicks, hzSync);
                return;
            }

        }

        if (stateBeforeScan) {
            RestoreGxVertexState(*stateBeforeScan);
        }
        EnsureAuroraFrameActive();
        GXMarkFrameWork();
        InvalidateAppliedAuroraState();
        ParseDisplayList(list, nbytes);
        InvalidateAppliedAuroraState();
    } catch (...) {}
}

PPC_NATIVE_OVERRIDE_VOID(801F4EAC, GX__CallDisplayList_HLE, (uint32_t listAddr, uint32_t nbytes), (listAddr, nbytes));

// The native nw4r g3d path's shape lists (hle/g3d_native.cpp): a shape's pre-primitive list (its vertex descriptor,
// formats and array bases, as CP writes) and its primitive list (its draws and, for a skinned shape, the matrix loads
// between them). What GX__CallDisplayList_HLE does on a scan-cache hit, from a record of the list's own: found by
// address, valid while the list's write generation is the one it was made under and the vertex layout its scan read
// is unchanged (the scan cache keys on a hash of the whole layout; a scan reads only the descriptor and the formats
// of the list's draws: nothing for a list without draws, the descriptor and one format's row for a list of draws in
// one format without CP writes, the whole layout's hash otherwise). So without the probe (layout hash, identity key
// and lookup, the SPM host-list check: a hit means the bytes are the ones the record came from) and, when the apply
// cannot fail (nothing indexed to apply: a pre-primitive list), without the vertex-state snapshot a list with CP
// writes keeps for its fallback. A miss is the HLE call; the scan cache's record for those bytes, when it stored one,
// is then copied.
extern "C" void GXCallStateDisplayList(const void* data, uint32_t nbytes);  // Aurora GXDispList.cpp

namespace {
// True for a list of BP and CP writes and NOP padding only (what GXCallStateDisplayList takes).
bool ListIsStateOnly(const uint8_t* data, uint32_t n) {
    uint32_t pos = 0;
    while (pos < n) {
        const uint8_t cmd = data[pos];
        if (cmd == GX_NOP_CMD) {
            ++pos;
        } else if (cmd == GX_LOAD_BP_REG_CMD) {
            pos += 5;
        } else if ((cmd & GX_OPCODE_MASK_CMD) == GX_LOAD_CP_REG_CMD) {
            pos += 6;
        } else {
            return false;
        }
    }
    return pos == n;
}

enum class ShapeLayout : uint8_t { kNone, kFormat, kHash };
struct G3dShapeList {
    uint32_t nbytes = 0;
    uint64_t generation = kDlWriteGenerationUntracked;
    ShapeLayout layout = ShapeLayout::kHash;
    uint64_t layoutHash = 0;                                     // kHash
    GXVtxFmt fmt = GX_VTXFMT0;                                              // kFormat: the format of its draws,
    decltype(g_hleGxState.vtxDesc) vtxDesc{};                               //   the descriptor
    std::remove_cvref_t<decltype(g_hleGxState.vtxAttrFmt[0])> fmtRow{};     //   and that format's row, as recorded
    uint64_t contentDigest = 0;
    bool mayContainDraw = false;
    bool applyCannotFail = false;
    bool streamStateOnly = false;  // the flattened stream holds BP and CP writes only (GXCallStateDisplayList)
    DlScanCacheEntry scan{};
    std::vector<DlCpWrite> cpWrites;
    std::vector<uint8_t> flattened;
};

bool ShapeLayoutMatches(const G3dShapeList& r) {
    switch (r.layout) {
    case ShapeLayout::kNone:
        return true;
    case ShapeLayout::kFormat:
        return std::memcmp(&r.vtxDesc, &g_hleGxState.vtxDesc, sizeof(r.vtxDesc)) == 0 &&
               std::memcmp(&r.fmtRow, &g_hleGxState.vtxAttrFmt[r.fmt], sizeof(r.fmtRow)) == 0;
    case ShapeLayout::kHash:
        break;
    }
    return HashScanLayoutState() == r.layoutHash;
}
// Per producer thread (the game thread, a draw worker: g3d_native.cpp), as the scan cache.
thread_local std::unordered_map<uint32_t, G3dShapeList> s_g3dShapeLists;
// GX__ClearShapeDisplayLists_G3D may run on any thread (scene reports); each drawing thread drops its records (and
// GX__CallRegisterDisplayList_G3D its list classifications).
std::atomic<uint32_t> s_g3dShapeListClears{0};
thread_local uint32_t s_g3dShapeListClearsSeen = 0;

void RecordG3dShapeList(const uint8_t* list, uint32_t listAddr, uint32_t nbytes, uint64_t generation,
                        uint64_t layoutHash) {
    const DlScanCacheProbe probe = ProbeDlScanCache(list, listAddr, nbytes, layoutHash);
    if (probe.record == nullptr || probe.record->writeGeneration != generation) return;
    const DlScanCacheRecord& from = *probe.record;
    G3dShapeList& r = s_g3dShapeLists[listAddr];
    r.nbytes = nbytes;
    r.generation = generation;
    r.contentDigest = from.contentDigest;
    r.mayContainDraw = from.mayContainDraw;
    r.scan = from.result;
    r.cpWrites = from.cpWrites;
    r.flattened = from.flattened;
    const auto& s = r.scan;
    // The layout the scan read (see above). A list without CP writes leaves the layout as the HLE call found it.
    r.layoutHash = layoutHash;
    if (!from.mayContainDraw || s.dlVtxFmt >= GX_MAX_VTXFMT) {
        r.layout = ShapeLayout::kNone;
    } else if (!s.dlVtxFmtMixed && !s.dlHasCpWrites && !s.dlHasNestedDl) {
        r.layout = ShapeLayout::kFormat;
        r.fmt = s.dlVtxFmt;
        std::memcpy(&r.vtxDesc, &g_hleGxState.vtxDesc, sizeof(r.vtxDesc));
        std::memcpy(&r.fmtRow, &g_hleGxState.vtxAttrFmt[r.fmt], sizeof(r.fmtRow));
    } else {
        r.layout = ShapeLayout::kHash;
    }
    bool indexed = false;
    for (int attr = 0; attr < GX_VA_MAX_ATTR; ++attr) indexed = indexed || s.sawIdx[attr] || s.sawXfIdx[attr];
    const bool streamOk = !s.needsFlatten || (s.flattenOk && (s.flattenVerbatim ? nbytes != 0 : !r.flattened.empty()));
    r.applyCannotFail = !indexed && s.scanOk && streamOk;
    r.streamStateOnly = s.needsFlatten && s.flattenOk &&
                        (s.flattenVerbatim ? ListIsStateOnly(list, nbytes)
                                           : ListIsStateOnly(r.flattened.data(), static_cast<uint32_t>(r.flattened.size())));
}
} // namespace

extern "C" void GX__ClearShapeDisplayLists_G3D() {
    s_g3dShapeListClears.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void GX__CallShapeDisplayList_G3D(uint32_t listAddr, uint32_t nbytes) {
    GX_HLE_SyncGuestProjection();
    if (nbytes == 0 || listAddr == 0) return;
    const uint8_t g3dRecCall[9] = {0x40, uint8_t(listAddr >> 24), uint8_t(listAddr >> 16), uint8_t(listAddr >> 8),
                                   uint8_t(listAddr), uint8_t(nbytes >> 24), uint8_t(nbytes >> 16),
                                   uint8_t(nbytes >> 8), uint8_t(nbytes)};
    g3dn::HleRec g3dRec(g3dn::g_recJob ? g3dn::kRecShapeDL : g3dn::kRecFifo, g3dRecCall, sizeof(g3dRecCall));
    if (g3dRec.skip) return;
    if (const uint32_t clears = s_g3dShapeListClears.load(std::memory_order_relaxed);
        clears != s_g3dShapeListClearsSeen) {
        s_g3dShapeLists.clear();
        s_g3dShapeListClearsSeen = clears;
    }
    try {
        const uint8_t* list = static_cast<const uint8_t*>(GuestToHostPtr(listAddr, nbytes));
        if (!list) return;
        const uint64_t generation = GxGuestWrite::GenerationForRange(listAddr, nbytes);
        if (generation == kDlWriteGenerationUntracked || nbytes > kDlScanCacheMaxEntryBytes || SpmDumpFrame()) {
            GX__CallDisplayList_HLE(listAddr, nbytes);
            return;
        }
        const auto it = s_g3dShapeLists.find(listAddr);
        if (it == s_g3dShapeLists.end() || it->second.nbytes != nbytes || it->second.generation != generation ||
            !ShapeLayoutMatches(it->second)) {
            const uint64_t layoutHash = HashScanLayoutState();  // the key the HLE call stores its record under
            GX__CallDisplayList_HLE(listAddr, nbytes);
            RecordG3dShapeList(list, listAddr, nbytes, generation, layoutHash);
            return;
        }
        const G3dShapeList& r = it->second;
        HZ_PERF_ADD(kHzDlCalls, 1);
        HZ_PERF_ADD(kHzDlBytes, nbytes);
        HZ_PHASE(kHzDlTicks);
        if (!r.mayContainDraw) {
            EnsureAuroraFrameActive();
            GXMarkFrameWork();
            GXCallDisplayList(list, nbytes);
            SyncAppliedVtxStateFromHleReal();
            return;
        }
        const DlScanCacheEntry& scan = r.scan;
        std::optional<HleGxVertexStateSnapshot> stateBeforeScan;
        if (scan.dlHasCpWrites && !r.applyCannotFail) stateBeforeScan = CaptureGxVertexStateForCpWrites(r.cpWrites);
        for (const auto& write : r.cpWrites) ApplyCpRegWrite(write.reg, write.value);
        if (scan.needsFlatten) {
            const uint8_t* stream = nullptr;
            uint32_t streamBytes = 0;
            if (scan.flattenOk) {
                stream = scan.flattenVerbatim ? list : r.flattened.data();
                streamBytes = scan.flattenVerbatim ? nbytes : static_cast<uint32_t>(r.flattened.size());
            }
            bool ok = scan.flattenOk && scan.scanOk && stream != nullptr && streamBytes != 0;
            if (ok) {
                ApplyAuroraVtxDesc();
                ApplyAuroraVtxAttrFmtForDisplayList(scan.dlVtxFmt, scan.dlVtxFmtMixed);
                const bool arraysOk =
                    ApplyAuroraArraysForDisplayList(scan.maxIdx, scan.sawIdx, scan.dlVtxFmt, scan.dlVtxFmtMixed);
                const bool xfOk = ApplyAuroraIndexedXFArraysForDisplayList(scan.maxXfIdx, scan.maxXfBytes, scan.sawXfIdx);
                ok = arraysOk && xfOk;
            }
            if (ok) {
                EnsureAuroraFrameActive();
                GXMarkFrameWork();
                if (r.streamStateOnly)
                    GXCallStateDisplayList(stream, streamBytes);
                else
                    GXCallDisplayList(stream, streamBytes);
                SyncAppliedVtxStateFromHleReal();
                return;
            }
        } else {
            bool ok = scan.scanOk;
            if (ok) {
                ApplyAuroraVtxDesc();
                ApplyAuroraVtxAttrFmtForDisplayList(scan.dlVtxFmt, scan.dlVtxFmtMixed);
                const bool arraysOk =
                    ApplyAuroraArraysForDisplayList(scan.maxIdx, scan.sawIdx, scan.dlVtxFmt, scan.dlVtxFmtMixed);
                const bool xfOk = ApplyAuroraIndexedXFArraysForDisplayList(scan.maxXfIdx, scan.maxXfBytes, scan.sawXfIdx);
                ok = arraysOk && xfOk;
            }
            if (ok) {
                EnsureAuroraFrameActive();
                GXMarkFrameWork();
                if (g3dn::g_nativeDraw)
                    GXCallDisplayListCompiled(list, nbytes, r.contentDigest);
                else
                    GXCallDisplayList(list, nbytes);
                SyncAppliedVtxStateFromHleReal();
                return;
            }
        }
        // The HLE's fallback (applyCannotFail records never get here).
        if (stateBeforeScan) RestoreGxVertexState(*stateBeforeScan);
        EnsureAuroraFrameActive();
        GXMarkFrameWork();
        InvalidateAppliedAuroraState();
        ParseDisplayList(list, nbytes);
        InvalidateAppliedAuroraState();
    } catch (...) {}
}

namespace {
// What a list holds, for GX__CallRegisterDisplayList_G3D: kOther (anything GX__CallDisplayList_HLE scans for),
// kRegisters (BP and XF commands, NOP padding), kBP (BP commands and NOP padding only).
enum class RegisterList { kOther, kRegisters, kBP };
RegisterList ClassifyRegisterList(const uint8_t* data, uint32_t n) {
    uint32_t pos = 0;
    bool xf = false;
    while (pos < n) {
        const uint8_t cmd = data[pos];
        if (cmd == GX_NOP_CMD) {
            ++pos;
        } else if (cmd == GX_LOAD_BP_REG_CMD) {
            pos += 5;
        } else if ((cmd & GX_OPCODE_MASK_CMD) == GX_LOAD_XF_REG_CMD && pos + 5 <= n) {
            pos += 5 + (static_cast<uint32_t>(ReadBE16(data + pos + 1)) + 1u) * 4u;
            xf = true;
        } else {
            return RegisterList::kOther;
        }
    }
    if (pos != n) return RegisterList::kOther;
    return xf ? RegisterList::kRegisters : RegisterList::kBP;
}
} // namespace

// The native nw4r g3d path's material display lists (hle/g3d_native.cpp: TEV, pixel, TEV colour, indirect matrix,
// texgen lists: register writes only). What GX__CallDisplayList_HLE does with a register-only list, without its
// per-call layout hash and scan-cache probe: a list is classified once per write generation of its bytes (every call
// while its memory is untracked); a list of BP writes only goes to Aurora's GXCallStateDisplayList (the writes applied
// as process() applies them, without its renderer lock), one with XF writes too to GXCallDisplayList, any other list
// takes GX__CallDisplayList_HLE.
extern "C" void GX__CallRegisterDisplayList_G3D(uint32_t listAddr, uint32_t nbytes) {
    if (nbytes == 0 || listAddr == 0) return;
    const uint8_t g3dRecCall[9] = {0x40, uint8_t(listAddr >> 24), uint8_t(listAddr >> 16), uint8_t(listAddr >> 8),
                                   uint8_t(listAddr), uint8_t(nbytes >> 24), uint8_t(nbytes >> 16),
                                   uint8_t(nbytes >> 8), uint8_t(nbytes)};
    g3dn::HleRec g3dRec(g3dn::g_recJob ? g3dn::kRecRegDL : g3dn::kRecFifo, g3dRecCall, sizeof(g3dRecCall));
    if (g3dRec.skip) return;
    struct Known {
        uint32_t nbytes;
        uint64_t generation;
        RegisterList kind;
    };
    static thread_local std::unordered_map<uint32_t, Known> s_known;
    static thread_local uint32_t s_knownClearsSeen = 0;  // scene changes (GX__ClearShapeDisplayLists_G3D)
    if (const uint32_t clears = s_g3dShapeListClears.load(std::memory_order_relaxed); clears != s_knownClearsSeen) {
        s_known.clear();
        s_knownClearsSeen = clears;
    }
    try {
        const uint8_t* list = static_cast<const uint8_t*>(GuestToHostPtr(listAddr, nbytes));
        if (!list) return;
        const uint64_t generation = GxGuestWrite::GenerationForRange(listAddr, nbytes);
        const auto it = s_known.find(listAddr);
        RegisterList kind;
        if (generation == kDlWriteGenerationUntracked || it == s_known.end() || it->second.nbytes != nbytes ||
            it->second.generation != generation) {
            kind = ClassifyRegisterList(list, nbytes);
            if (kind == RegisterList::kOther) {
                GX__CallDisplayList_HLE(listAddr, nbytes);
                return;
            }
            if (generation != kDlWriteGenerationUntracked) s_known[listAddr] = Known{nbytes, generation, kind};
        } else {
            kind = it->second.kind;
        }
        EnsureAuroraFrameActive();
        GXMarkFrameWork();
        if (kind == RegisterList::kBP)
            GXCallStateDisplayList(list, nbytes);
        else
            GXCallDisplayList(list, nbytes);
        SyncAppliedVtxStateFromHleReal();
    } catch (...) {}
}

extern "C" void nw4r__lyt__detail__DrawQuad_800847c0(CpuContext* ctx) {
    GX_HLE_MaterializeIfPending();  // a named override, not a thunk: publishes the HLE vertex descriptors
    uint32_t colors[4];
    const uint32_t colorAddr = ctx->gpr[7];
    const uint32_t* colorPtr = nullptr;
    if (colorAddr != 0) {
        colors[0] = ReadLytColor(colorAddr, 0);
        colors[1] = ReadLytColor(colorAddr, 1);
        colors[2] = ReadLytColor(colorAddr, 2);
        colors[3] = ReadLytColor(colorAddr, 3);
        colorPtr = colors;
    }

    EmitLytDrawQuad(ctx->gpr[3], ctx->gpr[4], static_cast<int32_t>(ctx->gpr[5]), ctx->gpr[6], colorPtr);
}


// ---------------------------------------------------------------------------------------------------------------
// Deferred native draws (hle/g3d_native.cpp, G3D_NATIVE_ASYNC=2): a native model draw recorded on the game thread is
// replayed by a draw worker into a deferred job (aurora gx_worker.hpp) on its own HLE state (this file's applied-state
// mirror, shape records and scan cache, g_hleGxState, g_boundTexMaps are per thread). The game thread keeps its own
// HLE state as the replay leaves it: the vertex state through the helpers below; the mirrors of what Aurora holds
// (applied state, bound textures) invalidated, so its next use publishes again.
// ---------------------------------------------------------------------------------------------------------------

// The vertex-state effect of calling a display list on this thread's HLE state (its CP writes applied in order),
// without submitting anything. The writes come from the list scan once per list address, size and write generation,
// then from this thread's cache. *drawFmtMask gets the vertex formats the list draws with (bit per format; all eight
// when mixed; none without draws): the formats a replay publishes for it. False when the list cannot be scanned or
// calls other lists (the scan may have applied some writes: the caller restores its snapshot).
// The walk behind it: CP writes applied in list order, draws skipped by their vertex size (as the flatten and the
// interpreter size them: CalcDLVertexSize), nothing scanned per vertex. A nested list fails the walk.
struct DlVertexStateVisitor {
    static constexpr int kMaxDepth = 0;
    static constexpr bool kHandlesDraw = true;

    std::vector<DlCpWrite>* writes;  // the CP writes, in order
    GXVtxFmt fmt = GX_MAX_VTXFMT;  // the first format drawn; stays so without draws
    bool mixed = false;
    int sizedFmt = -1;  // the format sizedBytes holds, until a CP write changes the layout
    uint32_t sizedBytes = 0;

    bool OnNop(uint8_t) { return true; }
    bool OnBpReg(const uint8_t*, uint32_t) { return true; }
    bool OnXfReg(const uint8_t*, uint32_t) { return true; }
    bool OnIndexedXf(const uint8_t*, uint8_t, uint32_t) { return true; }
    bool OnInvalidateVertexCache(uint8_t) { return true; }
    bool OnUnknownCommand(uint8_t) { return false; }
    bool OnCpReg(const uint8_t*, uint8_t reg, uint32_t value) {
        writes->push_back(DlCpWrite{reg, value});
        ApplyCpRegWrite(reg, value);
        if (reg == 0x50 || reg == 0x60 || (reg >= 0x70 && reg <= 0x97)) sizedFmt = -1;
        return true;
    }
    bool OnCallDisplayList(uint32_t addr, uint32_t size, int) { return addr == 0 || size == 0; }
    uint32_t DrawVertexSize(GXVtxFmt vtxfmt) {
        if (fmt == GX_MAX_VTXFMT) fmt = vtxfmt;
        else if (fmt != vtxfmt) mixed = true;
        if (sizedFmt != static_cast<int>(vtxfmt)) {
            sizedBytes = CalcDLVertexSize(vtxfmt);
            sizedFmt = static_cast<int>(vtxfmt);
        }
        return sizedBytes;
    }
    bool OnDraw(const uint8_t*, uint8_t, GXVtxFmt, uint16_t, const uint8_t*, uint32_t&) { return true; }
};

// The walk's result is kept per list (this thread's) while no write notification lands on the list's bytes: the fine
// 4 KiB generations (gx_guest_write.h), as the coarse ones missed every frame. Walking a model's shape lists costs the
// game thread their bytes, cold in its cache (perf: the command-byte load).
// The cache: open addressing on the list address (linear probes from a hashed home slot), each entry holding the
// list's CP writes inline, so a hit reads one or two cache lines (a std::unordered_map lookup missed about three
// times: bucket array, node, the writes' heap block). Entries validate themselves (address, size, generation), so a
// full probe run just overwrites the home slot. Lists with more CP writes than an entry holds are walked every time
// (32: a shape's pre-draw list sets the descriptors, a format and up to ~24 array bases and strides).
//
// A hit applies the writes from an image when the list has one (ListMirrorImage): a shape's pre-draw list (~12
// writes: the two descriptor registers, a format's three VAT registers, array bases and strides) cost ~1,000
// instructions to decode again on every hit (~50 such hits a frame in vanilla Training, all in the take-back's mirror,
// which grows as the game thread outruns the draw worker).
namespace {
// What a list's writes leave, captured right after the walk applied them (ListMirrorFill): each field they set holds
// its final value. Only for a list whose descriptor and VAT registers are each written once and whose VAT writes set
// one format: then each such write sets fields no other write of the list sets, and decides alone whether it changes
// them, from the fields as they were before the list (GxCpDecode: one layout-hash invalidation per write that changes
// a field). ApplyListImage repeats that per register: compares its fields with the image, copies them, invalidates
// once if they differed; array bases (already decoded) and strides are copied (no invalidation), the last write each.
struct ListMirrorImage {
    bool vcdLo = false, vcdHi = false;
    uint8_t vat = 0;  // bit 0 VAT A, bit 1 B, bit 2 C, all of format fmt
    uint8_t fmt = 0;
    uint32_t baseMask = 0, strideMask = 0;  // a bit per attribute
    GXAttrType desc[26] = {};
    VtxAttrFmt attrs[26] = {};
    HleGxState::VtxArray arrays[26] = {};
};
static_assert(GX_VA_PNMTXIDX == 0 && GX_VA_TEX7MTXIDX == 8 && GX_VA_POS == 9 && GX_VA_CLR1 == 12 &&
              GX_VA_TEX0 == 13 && GX_VA_TEX7 == 20 && GX_VA_NBT == 25, "GxCpDecode's descriptor ranges");

// GX_LIST_IMAGE=0: every hit decodes its writes again (a same-binary A/B). GX_LIST_IMAGE_CHECK=1: every hit with an
// image also applies the writes the old way from the same state and compares the vertex state.
const bool s_listImages = [] {
    const char* v = std::getenv("GX_LIST_IMAGE");
    return v == nullptr || v[0] != '0';
}();
const bool s_listImageCheck = [] {
    const char* v = std::getenv("GX_LIST_IMAGE_CHECK");
    return v != nullptr && v[0] == '1';
}();
uint64_t s_listImageHits = 0, s_listWriteHits = 0, s_listImageChecked = 0, s_listImageBad = 0;  // game thread

struct ListMirrorEntry {
    static constexpr uint32_t kMaxWrites = 32;
    uint32_t addr = 0;  // 0: empty
    uint32_t nbytes = 0;
    uint64_t generation = 0;
    uint8_t fmtMask = 0;
    uint8_t nwrites = 0;
    bool imaged = false;              // image holds this list's
    ListMirrorImage* image = nullptr;  // allocated at the slot's first image, kept (the table is never freed)
    uint8_t regs[kMaxWrites] = {};
    uint32_t values[kMaxWrites] = {};
};

// The image of `writes`, just applied to this thread's state by a walk; false where the list cannot have one.
bool CaptureListImage(ListMirrorEntry* slot, const std::vector<DlCpWrite>& writes) {
    if (!s_listImages || writes.empty()) return false;
    bool vcdLo = false, vcdHi = false;
    uint8_t vat = 0;
    int fmt = -1;
    uint32_t baseMask = 0, strideMask = 0;
    for (const DlCpWrite& w : writes) {
        const uint8_t reg = w.reg;
        if (reg == 0x50) {
            if (vcdLo) return false;
            vcdLo = true;
        } else if (reg == 0x60) {
            if (vcdHi) return false;
            vcdHi = true;
        } else if ((reg >= 0x70 && reg <= 0x77) || (reg >= 0x80 && reg <= 0x87) || (reg >= 0x90 && reg <= 0x97)) {
            const uint8_t bit = static_cast<uint8_t>(1u << ((reg >> 4) - 7));
            if ((vat & bit) != 0 || (fmt >= 0 && fmt != (reg & 7))) return false;
            vat |= bit;
            fmt = reg & 7;
        } else if (reg >= 0xA0 && reg <= 0xAF) {
            baseMask |= 1u << ((reg - 0xA0) + GX_VA_POS);
        } else if (reg >= 0xB0 && reg <= 0xBF) {
            strideMask |= 1u << ((reg - 0xB0) + GX_VA_POS);
        }  // other registers: dropped by GxCpDecode::ApplyCpRegWrite
    }
    if (slot->image == nullptr) slot->image = new ListMirrorImage;
    ListMirrorImage& im = *slot->image;
    im.vcdLo = vcdLo;
    im.vcdHi = vcdHi;
    im.vat = vat;
    im.fmt = static_cast<uint8_t>(fmt < 0 ? 0 : fmt);
    im.baseMask = baseMask;
    im.strideMask = strideMask;
    std::memcpy(im.desc, g_hleGxState.vtxDesc, sizeof(im.desc));
    std::memcpy(im.attrs, g_hleGxState.vtxAttrFmt[im.fmt], sizeof(im.attrs));
    std::memcpy(im.arrays, g_hleGxState.vtxArray, sizeof(im.arrays));
    return true;
}

// The image's writes applied to this thread's state, as GxCpDecode::ApplyCpRegWrite applies each (fields written
// field by field, so the formats' padding bytes stay as that leaves them).
void ApplyListImage(const ListMirrorImage& im) {
    HleGxState& st = g_hleGxState;
    if (im.vcdLo) {  // VcdLo: the matrix-index, position, normal and colour descriptors; NBT none
        const bool changed = std::memcmp(st.vtxDesc, im.desc, 13 * sizeof(GXAttrType)) != 0 ||
                             st.vtxDesc[GX_VA_NBT] != GX_NONE;
        std::memcpy(st.vtxDesc, im.desc, 13 * sizeof(GXAttrType));
        st.vtxDesc[GX_VA_NBT] = GX_NONE;
        if (changed) st.InvalidateVtxLayoutHash();
    }
    if (im.vcdHi) {  // VcdHi: the texture coordinate descriptors
        const bool changed = std::memcmp(&st.vtxDesc[GX_VA_TEX0], &im.desc[GX_VA_TEX0], 8 * sizeof(GXAttrType)) != 0;
        std::memcpy(&st.vtxDesc[GX_VA_TEX0], &im.desc[GX_VA_TEX0], 8 * sizeof(GXAttrType));
        if (changed) st.InvalidateVtxLayoutHash();
    }
    if (im.vat != 0) {
        VtxAttrFmt* attrs = st.vtxAttrFmt[im.fmt];
        const auto put = [&](int attr, bool cnt, bool frac, bool& changed) {
            VtxAttrFmt& d = attrs[attr];
            const VtxAttrFmt& s = im.attrs[attr];
            if (cnt) {
                changed |= d.cnt != s.cnt || d.type != s.type;
                d.cnt = s.cnt;
                d.type = s.type;
            }
            if (frac) {
                changed |= d.frac != s.frac;
                d.frac = s.frac;
            }
        };
        if ((im.vat & 1u) != 0) {  // VatA: position, normal, both colours, texture 0
            bool changed = false;
            for (int attr = GX_VA_POS; attr <= GX_VA_TEX0; ++attr) put(attr, true, true, changed);
            if (changed) st.InvalidateVtxLayoutHash();
        }
        if ((im.vat & 2u) != 0) {  // VatB: textures 1-4 count and type, 1-3 fraction
            bool changed = false;
            for (int tex = 1; tex <= 4; ++tex) put(GX_VA_TEX0 + tex, true, tex != 4, changed);
            if (changed) st.InvalidateVtxLayoutHash();
        }
        if ((im.vat & 4u) != 0) {  // VatC: texture 4 fraction, textures 5-7
            bool changed = false;
            put(GX_VA_TEX0 + 4, false, true, changed);
            for (int tex = 5; tex <= 7; ++tex) put(GX_VA_TEX0 + tex, true, true, changed);
            if (changed) st.InvalidateVtxLayoutHash();
        }
    }
    for (uint32_t m = im.baseMask; m != 0; m &= m - 1) {
        const int attr = __builtin_ctz(m);
        st.vtxArray[attr].base = im.arrays[attr].base;
    }
    for (uint32_t m = im.strideMask; m != 0; m &= m - 1) {
        const int attr = __builtin_ctz(m);
        st.vtxArray[attr].stride = im.arrays[attr].stride;
    }
}

// GX_LIST_IMAGE_CHECK=1: the vertex fields a list's writes can touch, and the hash bookkeeping, byte for byte.
struct ListMirrorFields {
    GXAttrType desc[26];
    VtxAttrFmt attrs[GX_MAX_VTXFMT][26];
    HleGxState::VtxArray arrays[26];
    uint32_t generation;
    bool dirty;
    void Save() {
        std::memcpy(desc, g_hleGxState.vtxDesc, sizeof(desc));
        std::memcpy(attrs, g_hleGxState.vtxAttrFmt, sizeof(attrs));
        std::memcpy(arrays, g_hleGxState.vtxArray, sizeof(arrays));
        generation = g_hleGxState.vtxStateGeneration;
        dirty = g_hleGxState.vtxLayoutHashDirty;
    }
    void Load() const {
        std::memcpy(g_hleGxState.vtxDesc, desc, sizeof(desc));
        std::memcpy(g_hleGxState.vtxAttrFmt, attrs, sizeof(attrs));
        std::memcpy(g_hleGxState.vtxArray, arrays, sizeof(arrays));
        g_hleGxState.vtxStateGeneration = generation;
        g_hleGxState.vtxLayoutHashDirty = dirty;
    }
    bool Same(const ListMirrorFields& o) const {
        return std::memcmp(desc, o.desc, sizeof(desc)) == 0 && std::memcmp(attrs, o.attrs, sizeof(attrs)) == 0 &&
               std::memcmp(arrays, o.arrays, sizeof(arrays)) == 0 && generation == o.generation && dirty == o.dirty;
    }
};
constexpr uint32_t kListMirrorSlots = 2048;  // a P+ Training frame calls a few hundred distinct lists
constexpr uint32_t kListMirrorProbes = 8;

// This thread's cache (the game thread's), never freed: the entry answering for the list at `generation` (hit), else
// the slot a walk of it refills.
ListMirrorEntry* ListMirrorProbe(uint32_t listAddr, uint32_t nbytes, uint64_t generation, bool& hit) {
    static thread_local ListMirrorEntry* s_table = nullptr;
    if (s_table == nullptr) s_table = new ListMirrorEntry[kListMirrorSlots]();
    hit = false;
    const uint32_t home = ((listAddr >> 5) * 0x9E3779B1u) >> (32 - 11);
    static_assert(kListMirrorSlots == 1u << 11);
    for (uint32_t i = 0; i < kListMirrorProbes; ++i) {
        ListMirrorEntry& e = s_table[(home + i) & (kListMirrorSlots - 1)];
        if (e.addr == listAddr) {
            hit = e.nbytes == nbytes && e.generation == generation;
            return &e;  // a stale one is refilled
        }
        if (e.addr == 0) return &e;
    }
    return &s_table[home];
}

// A walk's result kept in the slot ListMirrorProbe gave for it, right after the walk applied its writes.
void ListMirrorFill(ListMirrorEntry* slot, uint32_t listAddr, uint32_t nbytes, uint64_t generation,
                    const std::vector<DlCpWrite>& writes, uint32_t fmtMask) {
    if (writes.size() <= ListMirrorEntry::kMaxWrites) {
        slot->addr = listAddr;
        slot->nbytes = nbytes;
        slot->generation = generation;
        slot->fmtMask = static_cast<uint8_t>(fmtMask);
        slot->nwrites = static_cast<uint8_t>(writes.size());
        for (size_t w = 0; w < writes.size(); ++w) {
            slot->regs[w] = writes[w].reg;
            slot->values[w] = writes[w].value;
        }
        slot->imaged = CaptureListImage(slot, writes);
    } else if (slot->addr == listAddr) {
        slot->addr = 0;  // a stale entry for this list must not answer again
    }
}
}  // namespace

extern "C" bool GX_HLE_ApplyListVertexState(uint32_t listAddr, uint32_t nbytes, uint32_t* drawFmtMask) {
    *drawFmtMask = 0;
    if (nbytes == 0 || listAddr == 0) return true;
    const uint64_t generation = GxGuestWrite::FineGenerationForRange(listAddr, nbytes);
    ListMirrorEntry* slot = nullptr;
    if (generation != kDlWriteGenerationUntracked) {
        bool hit = false;
        slot = ListMirrorProbe(listAddr, nbytes, generation, hit);
        if (hit) {
            if (slot->imaged && s_listImageCheck) {
                static thread_local ListMirrorFields s_before, s_decoded, s_imaged;
                s_before.Save();
                for (uint32_t w = 0; w < slot->nwrites; ++w) ApplyCpRegWrite(slot->regs[w], slot->values[w]);
                s_decoded.Save();
                s_before.Load();
                ApplyListImage(*slot->image);
                s_imaged.Save();
                ++s_listImageChecked;
                if (!s_imaged.Same(s_decoded) && s_listImageBad++ < 20) {
                    RT_LOGF(RT_TAG_GX, "[dlmirror] list image MISMATCH list 0x%08X size %u writes %u\n", listAddr,
                            nbytes, slot->nwrites);
                }
                ++s_listImageHits;
            } else if (slot->imaged) {
                ApplyListImage(*slot->image);
                ++s_listImageHits;
            } else {
                for (uint32_t w = 0; w < slot->nwrites; ++w) ApplyCpRegWrite(slot->regs[w], slot->values[w]);
                if (slot->nwrites != 0) ++s_listWriteHits;
            }
            *drawFmtMask = slot->fmtMask;
            return true;
        }
    }
    const uint8_t* list = static_cast<const uint8_t*>(GuestToHostPtr(listAddr, nbytes));
    if (list == nullptr) return false;
    static thread_local std::vector<DlCpWrite> s_writes;
    s_writes.clear();
    DlVertexStateVisitor visitor{&s_writes};
    if (!WalkDisplayList(list, nbytes, visitor, 0)) return false;
    *drawFmtMask = visitor.mixed ? 0xFFu : visitor.fmt < GX_MAX_VTXFMT ? 1u << visitor.fmt : 0u;
    if (slot != nullptr) ListMirrorFill(slot, listAddr, nbytes, generation, s_writes, *drawFmtMask);
    return true;
}

// The stats line (g3d_native.cpp G3dNativeLogStats): per frame. Game thread.
extern "C" void GX_HLE_LogListImageStats(double frames) {
    if (frames <= 0 || (s_listImageHits | s_listWriteHits) == 0) return;
    RT_LOGF(RT_TAG_VI, "gx list mirror: %.1f/frame cached lists with writes applied from an image, %.1f/frame decoded "
            "again (GX_LIST_IMAGE=%d); checked %.1f/frame (GX_LIST_IMAGE_CHECK=1), %llu mismatches\n",
            s_listImageHits / frames, s_listWriteHits / frames, s_listImages ? 1 : 0, s_listImageChecked / frames,
            static_cast<unsigned long long>(s_listImageBad));
    s_listImageHits = s_listWriteHits = s_listImageChecked = 0;
}

// This thread's mirrors of what Aurora holds, invalidated (a deferred draw's replay changed it elsewhere, or a draw
// worker starts a job on a stale copy): the next draw publishes vertex state, arrays and textures again.
extern "C" void GX_HLE_InvalidatePublishedState() {
    InvalidateAppliedAuroraState();
    g_boundTexMaps.fill(BoundTexInfo{});
}

// A draw worker starting a job that follows its previous one in the command stream (g3d_native.cpp): the mirrors still
// describe what the GX thread holds, but the job loads the game thread's HLE vertex state, whose generation counter is
// the game thread's and may equal the mirror's without the same contents, so the generation shortcut is dropped
// (every publish compares field by field).
extern "C" void GX_HLE_KeepPublishedStateForNewJob() {
    s_appliedVtxStateMatchesHle = false;
}

// The game thread after recording a deferred draw: Aurora's register shadow (__gx) as the replay leaves it - when the
// draw drew, the vertex descriptors and the formats it drew with from the (exact) HLE vertex state, as its last
// publishes set them (ApplyAuroraVtxDesc, ApplyAuroraVtxAttrFmt: the same attributes), with the dirty bits its list
// calls flush cleared - and the published-state mirrors invalidated. zCompLoc: the last GXSetZCompLoc the draw made,
// or -1. drawFmtMask: the formats its lists drew with (GX_HLE_ApplyListVertexState).
extern "C" void GXShadowAfterDeferredDraw(int zCompLoc);  // aurora GXManage.cpp
extern "C" void GX_HLE_AfterDeferredDraw(int zCompLoc, uint32_t drawFmtMask) {
    if (drawFmtMask != 0) {
        // The SDK's list forms: the same register shadow as one GXSetVtxDesc / GXSetVtxAttrFmt per attribute, in that
        // order (the normal-type bits follow the final descriptors either way), in one call each; the dirty bits they
        // set are cleared below.
        GXVtxDescList descs[27];
        int n = 0;
        for (int attr = 0; attr < 26; ++attr) {
            if (attr == GX_VA_NBT) continue;
            descs[n++] = GXVtxDescList{static_cast<GXAttr>(attr), g_hleGxState.vtxDesc[attr]};
        }
        descs[n] = GXVtxDescList{GX_VA_NULL, GX_NONE};
        GXSetVtxDescv(descs);
        for (int fmt = 0; fmt < GX_MAX_VTXFMT; ++fmt) {
            if (!(drawFmtMask & (1u << fmt))) continue;
            GXVtxAttrFmtList fmts[27];
            int m = 0;
            for (int attr = 0; attr < 26; ++attr) {
                if (attr == GX_VA_NBT || IsMatrixIndexAttr(static_cast<GXAttr>(attr))) continue;
                const VtxAttrFmt& f = g_hleGxState.vtxAttrFmt[fmt][attr];
                fmts[m++] = GXVtxAttrFmtList{static_cast<GXAttr>(attr), f.cnt, f.type, f.frac};
            }
            fmts[m] = GXVtxAttrFmtList{GX_VA_NULL, GX_POS_XY, GX_U8, 0};
            GXSetVtxAttrFmtv(static_cast<GXVtxFmt>(fmt), fmts);
        }
    }
    GXShadowAfterDeferredDraw(zCompLoc);
    GX_HLE_InvalidatePublishedState();
}

// GX_HLE_AfterDeferredDraw over a run of draws the take-back mirrors one after another (g3d_native.cpp
// MaterializeJobsVertexState, which saves the dirty bits first and puts them back after): one publication at the end
// instead of one per draw. What each draw's publication leaves: the descriptor fields from GXSetVtxDescv (a full list
// without GX_VA_NBT, so every field it sets it sets whatever was there), each drawn format's VAT fields from
// GXSetVtxAttrFmtv (SETVAT: a function of its arguments alone), cleared dirty bits, invalidated mirrors. So after the
// run the descriptors are the last publishing draw's and each format's fields the last draw's that drew it: the HLE
// state at those draws is kept (Note) and published once (Finish). Vanilla Training mirrors 8-21 draws a frame (as
// far as the game thread runs ahead of the draw worker); each publication saved is ~2,000 instructions
// (GXSetVtxDescv and one GXSetVtxAttrFmtv per drawn format).
namespace {
struct DeferredPublishBatch {
    bool any = false;    // a draw was noted (the mirrors are invalidated at Finish)
    bool descs = false;  // a draw drew: vtxDesc holds the last one's descriptors
    uint8_t fmtMask = 0;
    GXAttrType vtxDesc[26]{};
    VtxAttrFmt vtxAttrFmt[GX_MAX_VTXFMT][26]{};
};
DeferredPublishBatch s_publishBatch;  // game thread
}  // namespace
extern "C" void GX_HLE_DeferredPublishBegin() {
    s_publishBatch.any = s_publishBatch.descs = false;
    s_publishBatch.fmtMask = 0;
}
extern "C" void GX_HLE_DeferredPublishNote(uint32_t drawFmtMask) {
    DeferredPublishBatch& b = s_publishBatch;
    b.any = true;
    if (drawFmtMask == 0) return;
    b.descs = true;
    std::memcpy(b.vtxDesc, g_hleGxState.vtxDesc, sizeof(b.vtxDesc));
    for (int fmt = 0; fmt < GX_MAX_VTXFMT; ++fmt) {
        if (!(drawFmtMask & (1u << fmt))) continue;
        std::memcpy(b.vtxAttrFmt[fmt], g_hleGxState.vtxAttrFmt[fmt], sizeof(b.vtxAttrFmt[fmt]));
        b.fmtMask |= static_cast<uint8_t>(1u << fmt);
    }
}
// published: the run mirrored to its end (else the take-back loads the worker's state over the fields: only the
// mirrors' invalidation is owed for the draws noted).
extern "C" void GX_HLE_DeferredPublishFinish(bool published) {
    const DeferredPublishBatch& b = s_publishBatch;
    if (!b.any) return;
    if (published && b.descs) {
        GXVtxDescList descs[27];
        int n = 0;
        for (int attr = 0; attr < 26; ++attr) {
            if (attr == GX_VA_NBT) continue;
            descs[n++] = GXVtxDescList{static_cast<GXAttr>(attr), b.vtxDesc[attr]};
        }
        descs[n] = GXVtxDescList{GX_VA_NULL, GX_NONE};
        GXSetVtxDescv(descs);
        for (int fmt = 0; fmt < GX_MAX_VTXFMT; ++fmt) {
            if (!(b.fmtMask & (1u << fmt))) continue;
            GXVtxAttrFmtList fmts[27];
            int m = 0;
            for (int attr = 0; attr < 26; ++attr) {
                if (attr == GX_VA_NBT || IsMatrixIndexAttr(static_cast<GXAttr>(attr))) continue;
                const VtxAttrFmt& f = b.vtxAttrFmt[fmt][attr];
                fmts[m++] = GXVtxAttrFmtList{static_cast<GXAttr>(attr), f.cnt, f.type, f.frac};
            }
            fmts[m] = GXVtxAttrFmtList{GX_VA_NULL, GX_POS_XY, GX_U8, 0};
            GXSetVtxAttrFmtv(static_cast<GXVtxFmt>(fmt), fmts);
        }
    }
    GXShadowAfterDeferredDraw(-1);
    GX_HLE_InvalidatePublishedState();
}

extern "C" void nw4r__lyt__detail__DrawQuad_80084d20(CpuContext* ctx) {
    GX_HLE_MaterializeIfPending();  // a named override, not a thunk: publishes the HLE vertex descriptors
    uint32_t colors[4];
    const uint32_t colorAddr = ctx->gpr[7];
    const uint32_t* colorPtr = nullptr;
    if (colorAddr != 0) {
        const uint32_t alpha = ctx->gpr[8];
        colors[0] = ReadModulatedLytColor(colorAddr, 0, alpha);
        colors[1] = ReadModulatedLytColor(colorAddr, 1, alpha);
        colors[2] = ReadModulatedLytColor(colorAddr, 2, alpha);
        colors[3] = ReadModulatedLytColor(colorAddr, 3, alpha);
        colorPtr = colors;
    }

    EmitLytDrawQuad(ctx->gpr[3], ctx->gpr[4], static_cast<int32_t>(ctx->gpr[5]), ctx->gpr[6], colorPtr);
}

#include "gx_vertex_state_overrides_end.h"  // pairs with the include at the top (unity build)
