// display_timing_macos.cpp - the main display's refresh period and latest vertical blank on macOS, for V-Sync's
// display lock in hle/vi.cpp (Windows reads the same from DWM). A CoreVideo display link calls back at each refresh with
// the time of the frame on screen; this keeps the newest. Built on its own (no unity batch, no precompiled header):
// Apple's headers and the Wii SDK's (dolphin/types.h) don't mix.

#if defined(__APPLE__)

#include "display_timing_macos.h"

#include <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>

#include <atomic>
#include <mutex>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // CVDisplayLink: still how a non-AppKit program gets it

namespace DisplayTimingMac {
namespace {

std::atomic<int64_t> g_period{0};  // nanoseconds
std::atomic<int64_t> g_vblank{0};  // steady_clock nanoseconds (mach absolute time, as libc++'s steady_clock counts)
std::once_flag g_started;
CVDisplayLinkRef g_link = nullptr;

int64_t HostToNanos(uint64_t host) noexcept {
    static const mach_timebase_info_data_t base = [] {
        mach_timebase_info_data_t b{};
        mach_timebase_info(&b);
        return b;
    }();
    return static_cast<int64_t>(static_cast<__uint128_t>(host) * base.numer / base.denom);
}

CVReturn OnRefresh(CVDisplayLinkRef, const CVTimeStamp* now, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*,
                   void*) {
    if (now != nullptr && (now->flags & kCVTimeStampHostTimeValid) != 0) {
        if (now->videoTimeScale > 0 && now->videoRefreshPeriod > 0) {
            g_period.store(static_cast<int64_t>(static_cast<double>(now->videoRefreshPeriod) * 1e9 /
                                                static_cast<double>(now->videoTimeScale)),
                           std::memory_order_relaxed);
        }
        g_vblank.store(HostToNanos(now->hostTime), std::memory_order_release);
    }
    return kCVReturnSuccess;
}

void Start() noexcept {
    if (CVDisplayLinkCreateWithCGDisplay(CGMainDisplayID(), &g_link) != kCVReturnSuccess || g_link == nullptr) {
        g_link = nullptr;
        return;
    }
    if (CVDisplayLinkSetOutputCallback(g_link, &OnRefresh, nullptr) != kCVReturnSuccess ||
        CVDisplayLinkStart(g_link) != kCVReturnSuccess) {
        CVDisplayLinkRelease(g_link);
        g_link = nullptr;
    }
}

}  // namespace

bool Read(int64_t* periodNanos, int64_t* vblankNanos) noexcept {
    std::call_once(g_started, Start);
    const int64_t vblank = g_vblank.load(std::memory_order_acquire);
    const int64_t period = g_period.load(std::memory_order_relaxed);
    if (vblank == 0 || period <= 0) return false;
    *periodNanos = period;
    *vblankNanos = vblank;
    return true;
}

}  // namespace DisplayTimingMac

#pragma clang diagnostic pop

#endif
