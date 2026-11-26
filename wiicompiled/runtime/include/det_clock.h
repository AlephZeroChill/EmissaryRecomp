#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

// Deterministic ("lockstep") timing, the base of network play. Copies of the game on different
// devices stay in step only if every guest-visible clock advances identically on all of them, so in
// this mode the time base, VI retraces, audio DMA, OS alarms and sleep timers run on one virtual
// timeline instead of the host's steady clock:
//  * while guest code runs, virtual time stands still apart from a small step per mftb read, so a
//    loop polling the time base still sees it move;
//  * when no guest thread can run, the scheduler moves it straight to the next due event, after
//    waiting for real time to reach that instant.
// Real time only paces the timeline; no guest-visible decision depends on it. The wall-clock pumps
// that deliver retraces, alarms and audio blocks from the middle of GX work or host frame waits are
// off in this mode, since where they land in the guest's execution depends on host speed.
namespace DetClock {

inline constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();

namespace detail {
extern bool g_enabled;
} // namespace detail

inline bool Enabled() noexcept { return detail::g_enabled; }
// Turns the mode on. Must happen before the guest starts.
void Enable() noexcept;

// Virtual time-base ticks since boot (60.75 MHz).
uint64_t Now() noexcept;
// Now(), then the per-read step: for the guest's own time-base reads.
uint64_t ReadForGuest() noexcept;
// Moves the timeline forward to `ticks` (never back).
void AdvanceTo(uint64_t ticks) noexcept;
// Sleeps until real time reaches the virtual instant `ticks`. A device that falls more than a few
// frames behind (a slow scene, a debugger) re-anchors instead of fast-forwarding to catch up.
// `frameBoundary`: `ticks` is the next VI retrace, the frame's own pacing point. Only there does lateness count for
// TakeMaxBehind: the timeline stands still while guest code runs, so an event due in the middle of a frame (audio DMA,
// an alarm) is always found "late" by the frame's own work, which is not a frame shown late.
void PaceTo(uint64_t ticks, bool frameBoundary = false);
// Forgets the real-time anchor, so the next PaceTo starts a fresh one (after a long host wait).
void ResetPacing() noexcept;
// Replayed frames advance guest time without consuming another wall-clock frame.
void SetRollbackResimulating(bool active) noexcept;
bool RollbackResimulating() noexcept;
uint64_t PacingNanoseconds() noexcept;

// A network session's time sync and frame pacing (Brawl Online's Engine, as Dolphin's CoreTiming throttle does it;
// docs/design/brawl-online-core.md §10.2). All from the game's thread.
// The timeline runs at `factor` times real time (1.0 = as is): the session's slow-down or speed-up when this machine
// is ahead of or behind the other. The schedule goes on from where it is (no jump).
void SetSpeed(double factor) noexcept;
double Speed() noexcept;
// The schedule moves `amount` earlier: the frames up to it are due at once (the session catching up a frame).
void AdvanceSchedule(std::chrono::nanoseconds amount) noexcept;
// A rollback is about to load: the presented frame after its re-run continues the schedule from where it was due now,
// the re-run's time coming out of that frame's slack (CoreTiming::BeginRollbackBurst).
void BeginRollbackBurst() noexcept;
// While waiting at the loop top for the game's loads: no pacing; paced from the present again after.
void SetLoadWait(bool waiting) noexcept;
// The real time the timeline's current instant is due at; false while it is not paced (no anchor yet, a re-run, a
// load wait).
bool ScheduledRealTime(std::chrono::steady_clock::time_point* out) noexcept;
// How far behind its schedule the pacing was at worst since the last call.
std::chrono::nanoseconds TakeMaxBehind() noexcept;
// Frame pacing of a running match: on, a pacing wait may end early by `lead()` (the time a coming rollback's re-run
// will take, 0 when none comes), so the rollback runs in the wait instead of after it (Dolphin's paced sleep lead).
using LeadFn = std::chrono::nanoseconds (*)();
void SetPresentPacing(bool on, LeadFn lead) noexcept;
// A running match's frame pacing is on (SetPresentPacing) and this is not a rollback's re-run or a load wait.
bool MatchPacing() noexcept;
// Something to do while the pacing waits (Brawl Online's input suggestions look at the controllers): a wait asks
// the hook first, and while it returns true sleeps in slices of about a millisecond, asking it before each.
// nullptr: none. Guest thread.
using WaitHook = bool (*)();
void SetWaitHook(WaitHook hook) noexcept;
// Pacing counters since the start, for the session's logs: re-anchors after falling too far behind, schedule advances.
struct PacingStats {
    uint64_t reanchors = 0;
    uint64_t advances = 0;
    uint64_t leadWakes = 0;     // waits a coming rollback ended early
    uint64_t bursts = 0;
    double speed = 1.0;
    double speedMin = 1.0, speedMax = 1.0;  // since ResetSpeedRange
    uint64_t resimTicks = 0;  // the timeline's ticks rollback re-runs used (no real time of their own)
};
PacingStats Stats() noexcept;
void ResetSpeedRange() noexcept;

// Now() as a steady_clock time point on the virtual timeline, for code that keeps its deadlines as
// time points. Never mix these with real Clock::now() values.
std::chrono::steady_clock::time_point NowTimePoint() noexcept;
// The first tick at which NowTimePoint() reaches `point` (the inverse of NowTimePoint, rounded up).
uint64_t TicksAtOrAfter(std::chrono::steady_clock::time_point point) noexcept;

} // namespace DetClock
