// Brawl Online's core in-process, the core side (brawl_online_bridge.h): the engine and the host the
// core runs on (BrawlOnline::Engine, BrawlOnline::Host), over the runtime side's functions, and the
// main loop's hooks into the core. Only the core's headers here (brawlonline.net core/).

#include "brawl_online_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "BrawlOnline/Compat.h"
#include "BrawlOnline/Engine.h"
#include "BrawlOnline/Host.h"
#include "BrawlOnline/Online/GameAlive.h"
#include "BrawlOnline/Online/GameBridge.h"
#include "BrawlOnline/Online/OnlineClient.h"
#include "BrawlOnline/Online/OnlineSession.h"
#include "BrawlOnline/Rollback/GameplayRollback.h"
#include "BrawlOnline/Rollback/GameplaySession.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Core/Core.h"
#include "Core/Rollback/GameplayOnlineBackend.h"
#include "Core/Rollback/ReplayPlayback.h"
#include "Core/System.h"

namespace B = BrawlOnlineBridge;

namespace {

u32 ReadBE32(u32 addr) {
    const u8* p = B::GuestPointer(addr, 4);
    return p ? (u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | u32(p[3])) : 0;
}
void WriteBE32(u32 addr, u32 v) {
    if (u8* p = B::GuestPointer(addr, 4)) {
        p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v);
        B::MarkDirty(addr, 4);
    }
}

#ifndef MKW_RELEASE_VERSION
#define MKW_RELEASE_VERSION "dev"  // a build without --release-version (tools/build.py)
#endif

// ---- The engine ----------------------------------------------------------------------------------
class RecompEngine final : public BrawlOnline::Engine {
public:
    // Who this player runs, in mm's hello and tickets and the game reports (Brawl Online's
    // `engine` / `engineVersion`; docs/design/brawl-online-core.md §10, "Which engine").
    BrawlOnline::EngineIdentity Identity() const override { return {"emissary", MKW_RELEASE_VERSION}; }

    B::Action action = B::Action::Normal;
    u32 call_target = 0;

    // Brawl's main loop at the session's hooks.
    void ContinueFrame() override { action = B::Action::Continue; }
    void SkipFrame() override {
        B::SetGpr(25, 1);
        action = B::Action::Skip;
    }
    void BackToLoopTop() override {}
    void FinishFrameEnd() override { WriteBE32(B::Gpr(23) + 0x100, B::Gpr(0)); }
    void CallFromLoopTop(u32 function, std::initializer_list<u32> args) override {
        int r = 3;
        for (const u32 a : args) B::SetGpr(r++, a);
        B::SetLr(BrawlOnline::LOOP_TOP);
        call_target = function;
        action = B::Action::Call;
    }
    u32 Gpr(int index) const override { return B::Gpr(index); }
    void SaveGuestRegs(BrawlOnline::GuestRegs* out) const override { B::SaveRegs(out); }
    void RestoreGuestRegs(const BrawlOnline::GuestRegs& in) override { B::RestoreRegs(&in); }
    bool CpuRunning() const override { return true; }

    // Emulated time: the deterministic clock's time base (60.75 MHz), paced by the runtime.
    u64 GetTicks() const override { return B::TimeBase(); }
    u32 GetTicksPerSecond() const override { return 60'750'000; }
    u32 GetTicksPerField() const override { return 1'013'513; }  // 60.75 MHz / 59.94 Hz
    u64 GetFakeTimeBase() const override { return B::TimeBase(); }
    bool IsRollbackResimulating() const override { return resimulating; }
    void SetRollbackResimulating(bool r) override {
        resimulating = r;
        B::SetResimulating(r);
    }
    // The loop top's wait for the game's loads: unpaced meanwhile (the deterministic clock's load wait).
    void SetRollbackLoadWait(bool waiting) override { B::SetLoadWait(waiting); }
    // The audio clock's waits for re-runs (by the fields the session adds as pauses) and its hold in the load waits,
    // as Dolphin's AudioDMACallback: the recomp's AI DMA holds its blocks (hle_stubs.h Audio_HLE_SetRollbackWaits).
    // Without them (PPR_GPRB_RESIM_AUDIO=advance, the core's switch) the audio runs through re-runs and plays them.
    void SetRollbackAudioWaits(bool wait) override { B::SetAudioWaits(wait); }
    void AddRollbackAudioPause(s64 ticks) override { B::AddAudioPause(ticks); }
#ifdef BRAWL_ONLINE_ENGINE_PLAYBACK_MUTE
    // A playback running fast (a watcher's catch-up, a seek's fast-forward): the output silenced meanwhile.
    void SetPlaybackMuted(bool muted) override {
        const double dropped = B::SetAudioMuted(muted);
        if (muted) INFO_LOG_FMT(BRAWLBACK, "recomp audio: muted (a playback running fast)");
        else INFO_LOG_FMT(BRAWLBACK, "recomp audio: unmuted, {:.3f} s of sound dropped while muted", dropped);
    }
#endif
    // Time sync (docs/design/brawl-online-core.md §10.2): the session's speed for this machine (a little slower when
    // it is ahead of the other, faster when behind) and its catch-up frames, on the deterministic clock's schedule,
    // as Dolphin's CoreTiming throttle takes them.
    void SetRollbackSpeedAdjustment(double factor) override { B::SetSpeed(factor * TestClockSkew()); }
    // BRAWL_TEST_CLOCK_SKEW=<factor> (tests): this machine's match runs that much faster than real time before time
    // sync's own factor (1.004: 0.4% fast), as a PC whose clock or frame pacing is off would; Dolphin's equivalent is
    // its emulation speed (Core.EmulationSpeed).
    static double TestClockSkew() {
        static const double skew = [] {
            const char* e = std::getenv("BRAWL_TEST_CLOCK_SKEW");
            const double v = e ? std::strtod(e, nullptr) : 1.0;
            if (e) INFO_LOG_FMT(BRAWLBACK, "test: BRAWL_TEST_CLOCK_SKEW: the match runs at {:.4f} times real time", v);
            return v > 0.5 && v < 2.0 ? v : 1.0;
        }();
        return skew;
    }
    void AdvanceThrottle(DT amount) override {
        B::AdvancePacing(std::chrono::duration_cast<std::chrono::nanoseconds>(amount).count());
    }
    // The match's frame pacing; a pacing wait ends early by the lead (a coming rollback runs in it).
    void SetPresentPacing(bool on) override {
        if (!on && present_pacing) {
            char line[512];
            B::PacingReport(line, sizeof line);
            INFO_LOG_FMT(BRAWLBACK, "{}", line);
            B::AudioReport(line, sizeof line);
            INFO_LOG_FMT(BRAWLBACK, "{}", line);
        }
        present_pacing = on;
        B::SetPresentPacing(on, lead != nullptr);
    }
    void SetPacedSleepLead(PacedSleepLead l) override {
        lead = l;
        B::SetPresentPacing(present_pacing, l != nullptr);
    }
    void ResetThrottleToNow() override { B::ResetPacing(); }
    void BeginRollbackBurst() override { B::BeginBurst(); }
    DT TakeMaxBehindSchedule() override {
        return std::chrono::duration_cast<DT>(std::chrono::nanoseconds(B::TakeMaxBehindNs()));
    }
    std::optional<TimePoint> ScheduledHostTime() override {
        int64_t ns = 0;
        if (!B::ScheduledHostTime(&ns)) return std::nullopt;
        return TimePoint(std::chrono::duration_cast<DT>(std::chrono::nanoseconds(ns)));
    }
    void SleepUntil(TimePoint t) override { std::this_thread::sleep_until(t); }
    // The lead (the core's PacedSleepLead), asked by the pacing's waits.
    PacedSleepLead lead = nullptr;
    bool present_pacing = false;

    // Snapshots: the region set in the runtime's delta store, in Dolphin's ring (RollbackManager:
    // 8 slots, a load `back` saves before the newest frees the ones after it).
    static constexpr int SLOTS = 8;
    void BeginRegionMode(const Ranges& ranges, const Ranges& exclude) override {
        std::vector<std::pair<u64, u64>> spans;  // [start, end) effective
        for (const auto& [a, n] : ranges) spans.emplace_back(a, u64(a) + n);
        for (const auto& [ha, hn] : exclude) {
            const u64 hs = ha, he = u64(ha) + hn;
            std::vector<std::pair<u64, u64>> out;
            for (const auto& [st, e] : spans) {
                if (e <= hs || st >= he) { out.emplace_back(st, e); continue; }
                if (st < hs) out.emplace_back(st, hs);
                if (e > he) out.emplace_back(he, e);
            }
            spans = std::move(out);
        }
        // Physical order, merged where they touch (the write tracker wants neither overlap nor
        // disorder).
        std::sort(spans.begin(), spans.end(), [](const auto& x, const auto& y) {
            return (x.first & 0x3FFFFFFF) < (y.first & 0x3FFFFFFF);
        });
        std::vector<u32> addrs, sizes;
        size_t total = 0;
        for (const auto& [st, e] : spans) {
            if (e <= st || !B::GuestPointer(u32(st), u32(e - st))) continue;
            if (!addrs.empty() && (addrs.back() & 0x3FFFFFFF) + sizes.back() >= (u32(st) & 0x3FFFFFFF) &&
                (addrs.back() >> 28) == (u32(st) >> 28)) {
                const u32 end = std::max(addrs.back() + sizes.back(), u32(e));
                total += end - (addrs.back() + sizes.back());
                sizes.back() = end - addrs.back();
                continue;
            }
            addrs.push_back(u32(st));
            sizes.push_back(u32(e - st));
            total += size_t(e - st);
        }
        B::SnapshotRegions(addrs.data(), sizes.data(), addrs.size());
        have_regions = !addrs.empty();
        ring_next = ring_count = 0;
        next_key = 1;
        std::fprintf(stderr, "[brawl-online] region set: %zu ranges, %zu bytes\n", addrs.size(), total);
    }
    void EndRegionMode() override {
        B::SnapshotRegions(nullptr, nullptr, 0);
        have_regions = false;
        ring_next = ring_count = 0;
    }
    void ToggleFrameSave() override {}
    void SaveFrame(s64 tag) override {
        if (!have_regions) return;
        const int key = next_key++;
        B::SnapshotSave(key);
        slots[size_t(ring_next)] = {tag, key};
        ring_next = (ring_next + 1) % SLOTS;
        ring_count = std::min(ring_count + 1, SLOTS);
        ++saves;
    }
    bool LoadFrame(int back) override {
        if (!have_regions || back < 1 || back >= ring_count) return false;
        const int target = ((ring_next - 1 - back) % SLOTS + SLOTS) % SLOTS;
        B::SnapshotRestore(slots[size_t(target)].key);
        next_key = slots[size_t(target)].key + 1;
        ring_next = (target + 1) % SLOTS;
        ring_count -= back;
        ++loads;
        return true;
    }
    int SlotsBackForTag(s64 tag) const override {
        if (tag == NO_TAG) return -1;
        for (int back = 1; back < ring_count; ++back)
            if (slots[size_t(((ring_next - 1 - back) % SLOTS + SLOTS) % SLOTS)].tag == tag) return back;
        return -1;
    }
    int RingCount() const override { return ring_count; }
    std::optional<PhysRange> MailboxRegion() const override { return mailbox; }
    void SetMailboxRegion(u32 virt_addr, u32 size_bytes) override {
        if (size_bytes == 0) mailbox.reset();
        else mailbox = PhysRange{virt_addr & 0x1FFFFFFF, (virt_addr & 0x1FFFFFFF) + size_bytes};
    }
    std::vector<std::pair<std::string, double>> SnapshotStats() const override {
        return {{"save_count", double(saves)}, {"load_count", double(loads)}, {"partial_granules", 0}};
    }
    std::vector<std::pair<u32, u64>> PartialGranuleSkips() const override { return {}; }
#ifdef BRAWL_ONLINE_ENGINE_STANDING_STILL  // a playback paused or buffering (a core before it has none)
    // The runtime presents and handles the window's events on the game's thread: while the core holds
    // it at the loop top, the last frame and the UI are shown again from here.
    void WhileStandingStill() override { B::StandStill(); }
#endif
#ifdef BRAWL_ONLINE_ENGINE_STALL_REPORT  // the core's loop watch (a core before it has none)
    std::string StallReport() const override {
        char text[512];
        B::StallReport(text, sizeof text);
        char stack[2048];
        B::CoreGameThreadStack(stack, sizeof stack);
        return *stack ? std::string(text) + "; " + stack : std::string(text);
    }
#endif

private:
    struct Slot { s64 tag = NO_TAG; int key = 0; };
    bool resimulating = false;
    bool have_regions = false;
    std::array<Slot, SLOTS> slots;
    int ring_next = 0, ring_count = 0, next_key = 1;
    u64 saves = 0, loads = 0;
    std::optional<PhysRange> mailbox;
};

// ---- The host ------------------------------------------------------------------------------------
class RecompHost final : public BrawlOnline::Host {
public:
    u8* GuestPointer(u32 addr, u32 size) override { return B::GuestPointer(addr, size); }
    u8* Mem1() override { return B::Mem1(); }
    u32 Mem1Size() const override { return B::Mem1Size(); }
    u8* Mem2() override { return B::Mem2(); }
    u32 Mem2Size() const override { return B::Mem2Size(); }
    void MarkDirty(u32 addr, u32 size) override { B::MarkDirty(addr, size); }
    void OnScreenMessage(std::string_view text, u32 ms, u32 argb) override {
        const std::string t(text);
        INFO_LOG_FMT(BRAWLBACK, "[screen] {}", t);
        B::ScreenMessage(t.c_str(), ms, argb);
    }
    GCPadStatus LocalPad(int chan) override {
        const B::Pad p = B::LocalPad(chan);
        GCPadStatus s{};
        s.button = p.button;
        s.stickX = u8(128 + p.stickX);
        s.stickY = u8(128 + p.stickY);
        s.substickX = u8(128 + p.substickX);
        s.substickY = u8(128 + p.substickY);
        s.triggerLeft = p.triggerL;
        s.triggerRight = p.triggerR;
        s.isConnected = p.connected;
        return s;
    }
    // A room's game watched (RoomWatch, the Rooms thread): the controllers are only read on the game's
    // thread, which reads them while the watch stands still (B::StandStill).
#ifdef BRAWL_ONLINE_HOST_LEAVE_BUTTONS_HELD  // a core before it has none
    bool LeaveButtonsHeld() override { return B::LeaveButtonsHeld(); }
#endif
    void StopGame() override { B::StopGame(); }
    bool HarnessActive() const override { return BrawlOnlineBridge::CoreHarnessActive(); }
    // The shape the picture is shown at: the game draws for it (GameBridge::PutAspect; 0 = as the game draws).
    float DrawAspectRatio() const override { return B::DrawAspect(); }
    void QueueHostJob(std::function<void()> job) override { std::thread(std::move(job)).detach(); }
};

RecompEngine s_engine;
RecompHost s_host;
bool s_started = false;

const Core::CPUThreadGuard& Guard() {
    static Core::CPUThreadGuard guard(Core::System::GetInstance());
    return guard;
}

}  // namespace

namespace BrawlOnlineBridge {

void CoreStart(const char* user_dir, const char* replay_comm) {
    if (s_started) return;
    s_started = true;
    B::CoreHoldWinsock();
    B::CoreWatchExit();
    B::CoreWatchCrashes();
    BrawlOnline::SetEngine(&s_engine);
    BrawlOnline::SetHost(&s_host);
    CoreUiStart();
    BrawlOnline::InitCompat(user_dir);
    Common::Log::LogManager::Init();
    INFO_LOG_FMT(BRAWLBACK, "Brawl Online core in EmissaryRecomp {}, user folder {}", MKW_RELEASE_VERSION, user_dir);
    Gprb::RegisterOnlineBackend();
    Online::Session::SelectConfigured();
    Online::GameBridge::Reset();
    if (replay_comm && *replay_comm) Gprb::Replay::Playback::SetCommFile(replay_comm);
    B::CoreHarnessStart();
}

void CoreShutdown() {
    if (!s_started) return;
    BrawlOnline::NotifyStopping();
    Gprb::Replay::Playback::Shutdown();
    // The recomp's window is both Dolphin's game and its app: closing it stops the game (above,
    // Core::Stop's Stopping) and quits, as ~MainWindow does: Rooms' connection closed and
    // game-status.json removed (the launcher's "is the game open"), Ranked's leave and reports
    // still to send given their 2 s.
    Online::Client::Shutdown();
}

Action CoreLoopTop(uint32_t* call_target) {
    CoreProfileStart();
    CoreNoteGameThread();
    auto& system = Core::System::GetInstance();
    s_engine.action = Action::Normal;
    Online::GameAlive::Beat();
    if (!Gprb::Session::IsRunning()) Gprb::CpuTraceOnLoopTop(system);
    Gprb::PadsOnLoopTop(system);
    Gprb::PadsWatchdog(Guard());
    if (!Gprb::Session::OnLoopTop(Guard())) return Action::Normal;
    *call_target = s_engine.call_target;
    return s_engine.action == Action::Normal ? Action::Continue : s_engine.action;
}

bool CoreFrameEnd() {
    if (Gprb::Session::OnFrameEnd(Guard())) return true;
    Online::GameBridge::OnFrameEnd(Guard());
    WriteBE32(B::Gpr(23) + 0x100, B::Gpr(0));
    Gprb::FrameTraceOnFrameEnd(Core::System::GetInstance(), B::Gpr(24), false);
    return true;
}

int64_t CorePacedSleepLeadNs() {
    const auto lead = s_engine.lead;
    return lead ? std::chrono::duration_cast<std::chrono::nanoseconds>(lead()).count() : 0;
}

SoundAlloc CoreSoundAlloc(uint32_t sound_id, uint32_t handle, uint32_t* sound) {
    const auto d = Gprb::Session::OnSoundAlloc(Guard(), sound_id, handle);
    switch (d.action) {
    case Gprb::Session::SoundAllocAction::Reattach:
        *sound = d.sound;
        return SoundAlloc::Reattach;
    case Gprb::Session::SoundAllocAction::NoChannel:
        return SoundAlloc::NoChannel;
    case Gprb::Session::SoundAllocAction::Proceed:
        break;
    }
    return SoundAlloc::Proceed;
}
void CoreSoundAttached(uint32_t handle, uint32_t sound, uint32_t sound_id) {
    Gprb::Session::OnSoundAttached(Guard(), handle, sound, sound_id);
}
bool CoreSoundStop(uint32_t sound) { return Gprb::Session::OnSoundStop(Guard(), sound); }

void CoreLoopEnd() { Gprb::Session::OnLoopEnd(Guard()); }
bool CoreDrivesLoop() { return Gprb::Session::DrivesLoop(); }
void CoreStageCreate() { Gprb::Session::OnStageCreate(Guard()); }
void CoreStageFile() { Gprb::Session::OnStageFileLoad(Guard()); }
void CorePadRead() { Gprb::PadsOnPadRead(); }
void CorePadThreadUpdated() { Gprb::PadsOnPadThreadUpdated(Core::System::GetInstance()); }

}  // namespace BrawlOnlineBridge
