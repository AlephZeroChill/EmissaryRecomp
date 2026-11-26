#include "frame_input.h"
#include "brawl_online_ipc.h"
#include "brawl_online_core.h"

#include "det_clock.h"
#include "input_bindings.h"
#include "settings_overlay.h"

// hle/input/pad.cpp: the local GameCube ports as the game would read them now (Peek: changing nothing).
void PAD_HLE_SampleLocalPads(FrameInput::GcPad* pads);
bool PAD_HLE_PeekLocalPads(FrameInput::GcPad* pads);
void PAD_HLE_PumpHostEvents();

namespace FrameInput {
namespace {

Frame g_frame{};
Source g_source = nullptr;

bool IsKpad(WiiRemoteInput::Kind kind) {
    return kind == WiiRemoteInput::Kind::Remote || kind == WiiRemoteInput::Kind::RemoteWithNunchuk ||
           kind == WiiRemoteInput::Kind::RemoteWithClassic;
}

} // namespace

bool Active() {
    return DetClock::Enabled();
}

Frame SampleLocal() {
    Frame frame{};
    const bool blocked = InputBindings::InputBlocked();
    GcPad pads[4]{};
    PAD_HLE_SampleLocalPads(pads);
    BrawlOnline::SamplePads(pads);
    for (uint32_t chan = 0; chan < frame.size(); ++chan) {
        frame[chan].pad = pads[chan];
        if (blocked) frame[chan].pad = {frame[chan].pad.connected};  // the overlay owns input: at rest
        Remote& remote = frame[chan];
        remote.kind = WiiRemoteInput::EffectiveKind(chan);
        remote.connected = IsKpad(remote.kind) && WiiRemoteInput::ReadKpadSample(chan, remote.sample);
        if (!remote.connected) {
            const GcPad pad = remote.pad;
            remote = {};
            remote.pad = pad;
            continue;
        }
        // The startup cover waits for A or 2 from any remote, controller or keyboard mapping.
        if (settings_overlay::StartupScreenVisible() && (remote.sample.hold & (0x0800u | 0x0100u)) != 0) {
            settings_overlay::NotifyStrapInputAccepted();
        }
        if (blocked) {
            // The settings overlay owns input: buttons and sticks at rest, the remote still there.
            const WiiRemoteInput::KpadSample held = remote.sample;
            remote.sample = {};
            for (int i = 0; i < 3; ++i) {
                remote.sample.acc[i] = held.acc[i];
                remote.sample.nunchukAcc[i] = held.nunchukAcc[i];
            }
            remote.sample.hasNunchuk = held.hasNunchuk;
            remote.sample.hasClassic = held.hasClassic;
        }
    }
    BrawlOnlineCore::NoteLocalPads(&frame);
    return frame;
}

bool PeekLocal(GcPad* pads) {
    if (!PAD_HLE_PeekLocalPads(pads)) return false;
    BrawlOnline::PeekPads(pads);
    if (InputBindings::InputBlocked()) {
        for (uint32_t chan = 0; chan < 4; ++chan) pads[chan] = {pads[chan].connected};  // the overlay owns input
    }
    return true;
}

void Latch(uint32_t retraceCount) {
    if (g_source != nullptr) {
        g_source(retraceCount, g_frame);
    } else {
        g_frame = SampleLocal();
        // Brawl Online's core takes these reads as Dolphin's SI reads (in a rollback's re-run: the ones before it).
        BrawlOnlineCore::HostPadsRead(&g_frame);
    }
}

void Relatch() {
    if (g_source != nullptr) return;
    PAD_HLE_PumpHostEvents();  // the keyboard as it is now, not at the last present
    GcPad pads[4]{};
    if (!PeekLocal(pads)) return;
    for (uint32_t chan = 0; chan < g_frame.size(); ++chan) g_frame[chan].pad = pads[chan];
    BrawlOnlineCore::HostPadsRead(&g_frame);
}

const Remote& Get(uint32_t chan) {
    static const Remote kNone{};
    return chan < g_frame.size() ? g_frame[chan] : kNone;
}

void SetSource(Source source) {
    g_source = source;
}

} // namespace FrameInput
