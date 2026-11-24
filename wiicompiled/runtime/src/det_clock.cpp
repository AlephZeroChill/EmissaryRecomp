#include "det_clock.h"

#include "runtime_log.h"
#include "timebase_contract.h"

#include <algorithm>
#include <atomic>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace DetClock {
namespace detail {
bool g_enabled = false;
} // namespace detail

namespace {

using Clock = std::chrono::steady_clock;

// About a microsecond of guest time per mftb read: enough for a loop that polls the time base to
// get anywhere, too little to matter for the few hundred reads a frame makes.
constexpr uint64_t kTicksPerGuestRead = 61;
// How far real time may run ahead of the timeline before pacing gives up catching up and starts
// over from the present (the backlog would otherwise play back as a burst of fast frames).
constexpr auto kMaxLag = std::chrono::milliseconds(100);

std::atomic<uint64_t> g_now{0};

// Real-time anchor of the pacing: virtual instant g_anchorTicks happens at real time g_anchorReal.
// Guest thread only.
bool g_anchored = false;
Clock::time_point g_anchorReal{};
uint64_t g_anchorTicks = 0;
bool g_resimulating = false;
uint64_t g_resimStartTicks = 0;
uint64_t g_pacingNanoseconds = 0;
// Time sync and frame pacing (det_clock.h, Brawl Online's Engine): the timeline's speed against real time, a rollback
// burst's due time, the load wait, the worst lateness since the session last asked, the paced wait's lead.
double g_speed = 1.0;
bool g_burstPending = false;
Clock::time_point g_burstDue{};
bool g_loadWait = false;
std::atomic<int64_t> g_maxBehindNs{0};
bool g_presentPacing = false;
LeadFn g_lead = nullptr;
PacingStats g_stats;

void SleepUntil(Clock::time_point deadline) {
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    // The high-resolution waitable timer lands within about half a millisecond; spin the rest so
    // frames keep an even cadence instead of the 1 ms scheduler sawtooth.
    constexpr auto kSpin = std::chrono::microseconds(500);
    struct HighResolutionTimer {
        HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_MODIFY_STATE | SYNCHRONIZE);
        ~HighResolutionTimer() {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    };
    static thread_local HighResolutionTimer timer;
    const auto now = Clock::now();
    if (deadline - now > kSpin && timer.handle != nullptr) {
        const auto wait = std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
            deadline - kSpin - now);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(wait.count(), 1);
        if (SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
            WaitForSingleObject(timer.handle, INFINITE);
        }
    }
    while (Clock::now() < deadline) {
        YieldProcessor();
    }
#else
    // clock_nanosleep is precise to well under a millisecond on Android and Linux; no spin, which
    // would only cost a phone battery.
    std::this_thread::sleep_until(deadline);
#endif
}

// The real time the virtual instant `ticks` is due at: the anchor, then the ticks since it at the timeline's speed.
Clock::time_point Due(uint64_t ticks) {
    const auto elapsed = TimeBaseContract::TicksToDuration(ticks - g_anchorTicks);
    if (g_speed == 1.0) return g_anchorReal + elapsed;
    return g_anchorReal + std::chrono::duration_cast<Clock::duration>(
                              std::chrono::duration<double, std::nano>(double(elapsed.count()) / g_speed));
}

void Anchor(Clock::time_point real, uint64_t ticks) {
    g_anchored = true;
    g_anchorReal = real;
    g_anchorTicks = ticks;
}

// Until `target` less the lead (SetPresentPacing), in slices, asking the lead again as the other player's inputs come
// in (CoreTiming::PacedSleepUntil): 1 ms OS sleeps, the last few ms as one precise sleep.
void PacedSleepUntil(Clock::time_point target) {
    constexpr auto kSlice = std::chrono::milliseconds(1);
    constexpr auto kFinal = std::chrono::milliseconds(3);
    for (;;) {
        const auto lead = g_lead();
        const auto wake = target - lead;
        const auto now = Clock::now();
        if (now >= wake) {
            if (lead.count() > 0 && now < target) ++g_stats.leadWakes;
            return;
        }
        if (wake - now <= kFinal) {
            SleepUntil(wake);
            continue;
        }
        std::this_thread::sleep_for(kSlice);
    }
}

} // namespace

void Enable() noexcept {
    if (!detail::g_enabled) {
        detail::g_enabled = true;
        RT_LOGF(RT_TAG_RUNTIME, "deterministic timing on: guest clocks run on a virtual timeline\n");
    }
}

uint64_t Now() noexcept {
    return g_now.load(std::memory_order_relaxed);
}

uint64_t ReadForGuest() noexcept {
    return g_now.fetch_add(kTicksPerGuestRead, std::memory_order_relaxed);
}

void AdvanceTo(uint64_t ticks) noexcept {
    uint64_t now = g_now.load(std::memory_order_relaxed);
    while (ticks > now && !g_now.compare_exchange_weak(now, ticks, std::memory_order_relaxed)) {
    }
}

void PaceTo(uint64_t ticks, bool frameBoundary) {
    if (g_resimulating) return;
    const auto now = Clock::now();
    if (!g_anchored || ticks < g_anchorTicks || g_loadWait) {
        // (While the session waits for loads, the timeline is not paced: it keeps re-anchoring, so the frames after the
        // wait are paced from then and do not race to make up for it.)
        Anchor(now, ticks);
        return;
    }
    const auto target = Due(ticks);
    if (now >= target) {
        if (frameBoundary) {
            const int64_t behind = std::chrono::duration_cast<std::chrono::nanoseconds>(now - target).count();
            int64_t worst = g_maxBehindNs.load(std::memory_order_relaxed);
            while (behind > worst && !g_maxBehindNs.compare_exchange_weak(worst, behind, std::memory_order_relaxed)) {
            }
        }
        if (now - target > kMaxLag) {
            Anchor(now, ticks);
            ++g_stats.reanchors;
        }
        return;
    }
    const auto sleepStart = Clock::now();
    // (The lead only at the frame's own pacing point, as Dolphin's paced sleep is the first throttle after a shown frame.)
    if (frameBoundary && g_presentPacing && g_lead != nullptr) PacedSleepUntil(target);
    else SleepUntil(target);
    g_pacingNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - sleepStart).count();
}

uint64_t PacingNanoseconds() noexcept { return g_pacingNanoseconds; }

void SetRollbackResimulating(bool active) noexcept {
    if (!active && g_resimulating) g_stats.resimTicks += Now() - g_resimStartTicks;
    if (!active && g_burstPending) {
        // The presented frame after a rollback burst continues the schedule from where it was due when the burst began:
        // the re-run's ticks are skipped, the real time it took comes out of the frame's slack (CoreTiming).
        g_burstPending = false;
        g_resimulating = false;
        Anchor(g_burstDue, Now());
        return;
    }
    if (active == g_resimulating) return;
    if (active) g_resimStartTicks = Now();
    else if (g_anchored) g_anchorTicks += Now() - g_resimStartTicks;
    g_resimulating = active;
}

bool RollbackResimulating() noexcept { return g_resimulating; }

void ResetPacing() noexcept {
    // (Brawl Online's ResetThrottleToNow: a rollback burst still pending is dropped too.)
    g_burstPending = false;
    Anchor(Clock::now(), Now());
}

void SetSpeed(double factor) noexcept {
    if (!(factor > 0.0) || factor == g_speed) return;
    // The schedule goes on from the current instant's due time at the new speed (CoreTiming::UpdateSpeedLimit).
    if (g_anchored && !g_resimulating && !g_loadWait) {
        const uint64_t now = Now();
        if (now >= g_anchorTicks) Anchor(Due(now), now);
    }
    g_speed = factor;
    g_stats.speed = factor;
    g_stats.speedMin = std::min(g_stats.speedMin, factor);
    g_stats.speedMax = std::max(g_stats.speedMax, factor);
}

void ResetSpeedRange() noexcept { g_stats.speedMin = g_stats.speedMax = g_speed; }

double Speed() noexcept { return g_speed; }

void AdvanceSchedule(std::chrono::nanoseconds amount) noexcept {
    if (!g_anchored) return;
    g_anchorReal -= amount;
    // A rollback burst in progress resumes the schedule at its due time: that moves as well.
    if (g_burstPending) g_burstDue -= amount;
    ++g_stats.advances;
}

void BeginRollbackBurst() noexcept {
    if (g_burstPending || !g_anchored) return;
    // At the loop top, before the load: the virtual instant here is the end of the frame just presented, which the
    // pacing already waited for.
    g_burstDue = g_resimulating || g_loadWait ? Clock::now() : Due(Now());
    g_burstPending = true;
    ++g_stats.bursts;
}

void SetLoadWait(bool waiting) noexcept {
    if (waiting == g_loadWait) return;
    g_loadWait = waiting;
    if (!waiting && !g_resimulating) Anchor(Clock::now(), Now());
}

bool ScheduledRealTime(Clock::time_point* out) noexcept {
    if (!g_anchored || g_resimulating || g_loadWait) return false;
    *out = Due(Now());
    return true;
}

std::chrono::nanoseconds TakeMaxBehind() noexcept {
    return std::chrono::nanoseconds(g_maxBehindNs.exchange(0, std::memory_order_relaxed));
}

void SetPresentPacing(bool on, LeadFn lead) noexcept {
    g_presentPacing = on;
    g_lead = lead;
}

PacingStats Stats() noexcept { return g_stats; }

Clock::time_point NowTimePoint() noexcept {
    return Clock::time_point{TimeBaseContract::TicksToDuration(Now())};
}

uint64_t TicksAtOrAfter(Clock::time_point point) noexcept {
    const auto sinceEpoch = std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count();
    if (sinceEpoch <= 0) {
        return 0;
    }
    const uint64_t ns = static_cast<uint64_t>(sinceEpoch);
    constexpr uint64_t kNum = TimeBaseContract::kTickRatioNumerator;    // 243 ticks
    constexpr uint64_t kDen = TimeBaseContract::kTickRatioDenominator;  // per 4000 ns
    return (ns / kDen) * kNum + ((ns % kDen) * kNum + kDen - 1) / kDen;
}

} // namespace DetClock
