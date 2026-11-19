// gx_copy.cpp - Framebuffer Copy Operations
#include "gx_internal.h"
#include "runtime_log.h"

#include "settings_overlay.h"

#include <aurora/aurora.h>
#include <dolphin/gx/GXAurora.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <map>
#include <mutex>
#include <vector>
#include "hz_perf.h"

extern "C" void aurora_gx_order_point();  // aurora.cpp: drain GX work in order without waiting for the GX thread
// GXFrameBuffer.cpp: the next GXCopyDisp clears the EFB but keeps the image already in the display copy.
extern "C" void aurora_gx_keep_display_copy_once();

namespace {
// Copy destinations stay GPU-only until an explicit/lazy readback. Track the
// guest ranges that own those results so a later data-cache flush over a reused
// allocation can retire the stale GPU texture before it is considered by a
// subsequent GXLoadTexObj. There is at most one live range per destination;
// rewriting the destination replaces its previous extent.
std::mutex g_efbCopyDestinationsMutex;
std::map<uint32_t, uint32_t> g_efbCopyDestinations;
uint32_t g_largestEfbCopyDestination = 0;
// The 4 KB pages that hold a destination, a bit per page (canonical address >> 12, modulo 128K pages: an alias only
// costs a locked check), so the range checks below take the lock only for ranges touching such a page. Data-cache ops
// and DMA writes (locked-cache stores in nw4r g3d's matrix work) run thousands of times a frame and copy destinations
// are a handful of textures: ~1% of the game thread in P+ online play went to this lock. Written under the lock: set
// when a destination is added (the game thread, GX__CopyTex_HLE), recomputed when destinations are retired, each word
// stored once with its final bits, so a reader sees a superset of the destinations' pages until they are retired; after
// an addition only another thread's concurrent check can miss it, which with the lock was a matter of timing as well.
constexpr uint32_t kEfbCopyPageBits = 1u << 17;
std::array<std::atomic<uint64_t>, kEfbCopyPageBits / 64> g_efbCopyPages{};

void MarkEfbCopyPages(std::array<uint64_t, kEfbCopyPageBits / 64>& words, uint64_t start, uint64_t size) {
    const uint64_t first = start >> 12, last = (start + size - 1) >> 12;
    for (uint64_t page = first; page <= last && page - first < kEfbCopyPageBits; ++page) {
        const uint32_t bit = static_cast<uint32_t>(page) & (kEfbCopyPageBits - 1);
        words[bit >> 6] |= 1ull << (bit & 63);
    }
}

bool TouchesEfbCopyPage(uint64_t start, uint64_t end) {
    const uint64_t first = start >> 12, last = (end - 1) >> 12;
    if (last - first >= kEfbCopyPageBits) {
        return true;
    }
    for (uint64_t page = first; page <= last; ++page) {
        const uint32_t bit = static_cast<uint32_t>(page) & (kEfbCopyPageBits - 1);
        if (g_efbCopyPages[bit >> 6].load(std::memory_order_acquire) & (1ull << (bit & 63))) {
            return true;
        }
    }
    return false;
}

void RememberEfbCopyDestination(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
    const uint64_t start = CanonicalizeGxMainRamAddress(addr);
    g_efbCopyDestinations[static_cast<uint32_t>(start)] = size;
    // A conservative monotonic maximum lets invalidation jump directly to
    // the only map interval that could overlap instead of walking every copy
    // destination on every DCStoreRange.
    g_largestEfbCopyDestination = std::max(g_largestEfbCopyDestination, size);
    const uint64_t first = start >> 12, last = (start + size - 1) >> 12;
    for (uint64_t page = first; page <= last && page - first < kEfbCopyPageBits; ++page) {
        const uint32_t bit = static_cast<uint32_t>(page) & (kEfbCopyPageBits - 1);
        auto& word = g_efbCopyPages[bit >> 6];
        word.store(word.load(std::memory_order_relaxed) | (1ull << (bit & 63)), std::memory_order_release);
    }
}

} // namespace

// A data-cache op over a GXCopyTex destination is the game about to read the copy with its CPU (SPM's offscreen
// effects: doors, the 3D flip, character switches copy the screen, then DCFlushRange it and move the pixels into
// their own buffers). Copies are GPU-only, so download any that are still pending into RAM first.
void DownloadEfbCopiesForRange(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }
    const uint64_t start = CanonicalizeGxMainRamAddress(addr);
    const uint64_t end = start + size;
    if (!TouchesEfbCopyPage(start, end)) {
        return;
    }
    std::vector<uint32_t> overlapping;
    {
        std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
        const uint64_t earliest = start > g_largestEfbCopyDestination ? start - g_largestEfbCopyDestination : 0;
        for (auto it = g_efbCopyDestinations.lower_bound(static_cast<uint32_t>(earliest));
             it != g_efbCopyDestinations.end() && static_cast<uint64_t>(it->first) < end; ++it) {
            if (static_cast<uint64_t>(it->first) + it->second > start) overlapping.push_back(it->first);
        }
    }
    for (const uint32_t copyAddr : overlapping) {
        aurora_flush_efb_copy_to_ram(GuestToHostPtr(copyAddr));  // no-op unless still pending
    }
}

bool EfbCopyPagesTouched(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return false;
    }
    const uint64_t start = CanonicalizeGxMainRamAddress(addr);
    return TouchesEfbCopyPage(start, start + size);
}

void InvalidateEfbCopyDestinationsForRange(uint32_t addr, uint32_t size) {
    if (size == 0) {
        return;
    }

    const uint64_t dirtyStart = CanonicalizeGxMainRamAddress(addr);
    const uint64_t dirtyEnd = dirtyStart + size;
    if (!TouchesEfbCopyPage(dirtyStart, dirtyEnd)) {
        return;
    }
    std::vector<uint32_t> retired;
    {
        std::lock_guard<std::mutex> guard(g_efbCopyDestinationsMutex);
        const uint64_t earliestCandidate =
            dirtyStart > g_largestEfbCopyDestination ? dirtyStart - g_largestEfbCopyDestination : 0;
        for (auto it = g_efbCopyDestinations.lower_bound(static_cast<uint32_t>(earliestCandidate));
             it != g_efbCopyDestinations.end() && static_cast<uint64_t>(it->first) < dirtyEnd;) {
            const uint64_t copyStart = it->first;
            const uint64_t copyEnd = copyStart + it->second;
            if (dirtyEnd <= copyStart || dirtyStart >= copyEnd) {
                ++it;
                continue;
            }
            retired.push_back(it->first);
            it = g_efbCopyDestinations.erase(it);
        }
        if (!retired.empty()) {
            // The pages of what is left (a handful of destinations), each word stored once: its bits only clear.
            static std::array<uint64_t, kEfbCopyPageBits / 64> words;
            words.fill(0);
            for (const auto& [copyStart, copySize] : g_efbCopyDestinations) {
                MarkEfbCopyPages(words, copyStart, copySize);
            }
            for (size_t i = 0; i < words.size(); ++i) {
                if (g_efbCopyPages[i].load(std::memory_order_relaxed) != words[i]) {
                    g_efbCopyPages[i].store(words[i], std::memory_order_release);
                }
            }
        }
    }

    // Preserve FIFO ordering: the destroy command is emitted before any later
    // texture load that can consume the freshly flushed RAM bytes.
    for (const uint32_t copyAddr : retired) {
        GXDestroyCopyTex(GuestToHostPtr(copyAddr));
    }
}

// ============================================================================
// Display Copy Source/Destination
// ============================================================================

extern "C" void GX__SetDispCopySrc_HLE(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    GXSetDispCopySrc((u16)l, (u16)t, (u16)w, (u16)h);
}
PPC_NATIVE_OVERRIDE_VOID(801f1524, GX__SetDispCopySrc_HLE, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetDispCopyDst_HLE(uint32_t w, uint32_t h) { GXSetDispCopyDst((u16)w, (u16)h); }
PPC_NATIVE_OVERRIDE_VOID(801f15a4, GX__SetDispCopyDst_HLE, (uint32_t w, uint32_t h), (w, h));

// ============================================================================
// Texture Copy Source/Destination
// ============================================================================

extern "C" void GX__SetTexCopySrc_HLE(uint32_t l, uint32_t t, uint32_t w, uint32_t h) {
    GXSetTexCopySrc((u16)l, (u16)t, (u16)w, (u16)h);
    g_texCopyState.srcLeft=(u16)l; g_texCopyState.srcTop=(u16)t;
    g_texCopyState.srcWidth=(u16)w; g_texCopyState.srcHeight=(u16)h;
}
PPC_NATIVE_OVERRIDE_VOID(801f1564, GX__SetTexCopySrc_HLE, (uint32_t l, uint32_t t, uint32_t w, uint32_t h), (l, t, w, h));

extern "C" void GX__SetTexCopyDst_HLE(uint32_t w, uint32_t h, uint32_t f, uint32_t m) {
    GXSetTexCopyDst((u16)w, (u16)h, (GXTexFmt)f, (GXBool)m);
    g_texCopyState.dstWidth=(u16)w; g_texCopyState.dstHeight=(u16)h;
    g_texCopyState.dstFormat=f; g_texCopyState.dstMipmap=m;
}
PPC_NATIVE_OVERRIDE_VOID(801f15c8, GX__SetTexCopyDst_HLE, (uint32_t w, uint32_t h, uint32_t f, uint32_t m), (w, h, f, m));

extern "C" void GX__SetCopyFilter_HLE(uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa) {
    uint8_t sp[12][2]={}, vfb[7]={};
    if(spa) std::memcpy(sp, GuestToHostPtr(spa, 24), 24);
    if(vfa) std::memcpy(vfb, GuestToHostPtr(vfa, 7), 7);
    GXSetCopyFilter((GXBool)aa, sp, (GXBool)vf, vfb);
}
PPC_NATIVE_OVERRIDE_VOID(801f1b2c, GX__SetCopyFilter_HLE, (uint32_t aa, uint32_t spa, uint32_t vf, uint32_t vfa), (aa, spa, vf, vfa));

extern "C" void GX__SetDispCopyGamma_HLE(uint32_t g) { GXSetDispCopyGamma((GXGamma)g); }
PPC_NATIVE_OVERRIDE_VOID(801f1d10, GX__SetDispCopyGamma_HLE, (uint32_t g), (g));

// ============================================================================
// Copy Execution
// ============================================================================

extern "C" void GX__CopyDisp_HLE(uint32_t da, uint32_t c) {
    HZ_PHASE(kHzCopyDispTicks);
    EnsureAuroraFrameActive();
    // GX copies are FIFO-ordered on hardware. Drain submitted draws before
    // resolving the EFB so high-level copies see the same contents. An ordering point, not GXDrawDone: with Aurora's
    // GX thread on the copy is queued behind those draws, so waiting for them here would only stall this thread.
    aurora_gx_order_point();
    if (VI_HLE_KeepDisplayCopy()) aurora_gx_keep_display_copy_once();  // rollback: show the previous image again
    GXCopyDisp(GuestToHostPtr(da), (GXBool)c);
    // No second GXDrawDone here: the frame-worker wait below is for the DONE
    // phase, which strictly subsumes the drain this call would perform.
    ++g_gxFrameCount;
    VI_HLE_SetXfbReady(da);
    // Present immediately so post-copy draws don't leak into this frame. Join at the DONE phase
    // (not the cheaper SEALED phase GXDrawDone waits for) because ImGui's draw lists, owned by
    // Aurora's render worker, replay during encode; aurora_end_frame would join here anyway.
    aurora_wait_for_frame_worker();
    settings_overlay::Draw();
    // Seal, pace to the VI retrace boundary (Aurora renders the sealed frame
    // during the wait), and pre-warm the next frame.
    VI_HLE_PresentFrame(/*presentedXfb=*/true, /*paceToRetrace=*/true);
}

PPC_NATIVE_OVERRIDE_VOID(801f1d24, GX__CopyDisp_HLE, (uint32_t da, uint32_t c), (da, c));


extern "C" void GX__CopyTex_HLE(uint32_t da, uint32_t c) {
    HZ_PHASE(kHzCopyTexTicks);
    EnsureAuroraFrameActive();
    // Match GX FIFO ordering: texture copies observe all prior draws (an ordering point; see GX__CopyDisp_HLE). A copy
    // the CPU reads back waits where it is read (aurora_flush_efb_copy_to_ram syncs).
    aurora_gx_order_point();
    const uint16_t rawSrcLeft = g_texCopyState.srcLeft;
    const uint16_t rawSrcTop = g_texCopyState.srcTop;
    const uint16_t rawSrcWidth = g_texCopyState.srcWidth;
    const uint16_t rawSrcHeight = g_texCopyState.srcHeight;

    // Keep the source in guest EFB coordinates. Aurora maps it to the scaled
    // EFB exactly once, matching Dolphin's ConvertEFBRectangle path.
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
    // EFB copies stay GPU-only except probe-sized ones (e.g. the 4x4 lens-flare depth probe),
    // which Aurora reads back asynchronously via efb_ram::schedule and land in guest RAM a frame
    // later. RISK: copies above the probe threshold, or on the offscreen list, are not
    // auto-downloaded, so guest reads see stale RAM; call aurora_flush_efb_copies_to_ram if a
    // copy needs reading back.
    // NSMBW_LOG_COPY_FRAMES: per frame, the texture copies made (size, format, source, destination),
    // logged with the time whenever that list changes from the previous frame's.
    if (static const bool logFrames = std::getenv("NSMBW_LOG_COPY_FRAMES") != nullptr; logFrames) {
        static uint32_t frame = ~0u;
        static std::string current, previous;
        if (frame != static_cast<uint32_t>(g_gxFrameCount)) {
            if (current != previous) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count();
                std::fprintf(stderr, "[copyframe] t=%lld frame %u:%s\n", static_cast<long long>(ms), frame,
                             current.c_str());
                previous = current;
            }
            current.clear();
            frame = static_cast<uint32_t>(g_gxFrameCount);
        }
        char item[96];
        std::snprintf(item, sizeof(item), " %ux%u/%u@%u,%u>%08X", g_texCopyState.dstWidth, g_texCopyState.dstHeight,
                      g_texCopyState.dstFormat, rawSrcLeft, rawSrcTop, da);
        current += item;
    }
    // NSMBW_LOG_COPIES: each distinct texture copy (destination size, format, source rectangle) once,
    // with the guest code that asked for it (profiling).
    if (static const bool logCopies = std::getenv("NSMBW_LOG_COPIES") != nullptr; logCopies) {
        static std::vector<uint64_t> seen;
        const uint64_t key = (uint64_t(g_texCopyState.dstWidth) << 48) | (uint64_t(g_texCopyState.dstHeight) << 32) |
                             (uint64_t(rawSrcWidth) << 16) | rawSrcHeight;
        if (std::find(seen.begin(), seen.end(), key) == seen.end() && seen.size() < 64) {
            seen.push_back(key);
            uint32_t lr = 0, ra[4] = {};
            if (const CpuContext* cpu = TryGetCpuContext()) {
                lr = cpu->lr;
                uint32_t sp = cpu->gpr[1];
                for (int d = 0; d < 4 && sp != 0; ++d) {
                    uint32_t back = 0;
                    if (!Memory::TryRead32(sp, back) || back == 0) break;
                    Memory::TryRead32(back + 4, ra[d]);
                    sp = back;
                }
            }
            std::fprintf(stderr, "[copytex] dst %ux%u fmt %u src %u,%u %ux%u dest %08X lr %08X %08X %08X %08X %08X\n",
                         g_texCopyState.dstWidth, g_texCopyState.dstHeight, g_texCopyState.dstFormat, rawSrcLeft,
                         rawSrcTop, rawSrcWidth, rawSrcHeight, da, lr, ra[0], ra[1], ra[2], ra[3]);
        }
    }
    GXCopyTex(GuestToHostPtr(da), (GXBool)c);
    // The extent the copy writes: a destination bigger than the source is a patch into a larger
    // texture (aurora sizes its copy the same way), not a claim on that whole texture's memory.
    const uint32_t copiedWidth = std::max<uint32_t>(rawSrcWidth >> (g_texCopyState.dstMipmap ? 1 : 0), 1);
    const uint32_t copiedHeight = std::max<uint32_t>(rawSrcHeight >> (g_texCopyState.dstMipmap ? 1 : 0), 1);
    RememberEfbCopyDestination(
        da, GXGetTexBufferSize(std::min<uint32_t>(g_texCopyState.dstWidth, copiedWidth),
                               std::min<uint32_t>(g_texCopyState.dstHeight, copiedHeight),
                               g_texCopyState.dstFormat, GX_FALSE, 0));
    GXSetTexCopySrc(rawSrcLeft, rawSrcTop, rawSrcWidth, rawSrcHeight);
}
PPC_NATIVE_OVERRIDE_VOID(801f1e60, GX__CopyTex_HLE, (uint32_t da, uint32_t c), (da, c));

// GXSetDispCopyYScale (SPM 0x8028DC64) / GXGetYScaleFactor (0x8028DA34): SPM calls these directly
// (NSMBW's EGG inlined them). Translated, they read the SDK's own copy state, which the native copy
// hooks never fill, and programmed a bogus vertical XFB scale (every column smeared top to bottom).
extern "C" uint32_t GX__SetDispCopyYScale_HLE(float vscale) { return GXSetDispCopyYScale(vscale); }
PPC_NATIVE_OVERRIDE(801F19E8, GX__SetDispCopyYScale_HLE, uint32_t, (float vscale), (vscale));

extern "C" float GX__GetYScaleFactor_HLE(uint32_t efbHeight, uint32_t xfbHeight) {
    if (efbHeight == 0 || xfbHeight == 0 || xfbHeight > 1024) return 1.0f;
    return GXGetYScaleFactor(static_cast<u16>(efbHeight), static_cast<u16>(xfbHeight));
}
PPC_NATIVE_OVERRIDE(801F17B8, GX__GetYScaleFactor_HLE, float, (uint32_t efbHeight, uint32_t xfbHeight), (efbHeight, xfbHeight));
