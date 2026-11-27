#pragma once

#include "settings_overlay.h"
#include "runtime_config.h"
#include "display_settings.h"
#include "brawl_online_core.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx/GXAurora.h>

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace WindowPlacementPersistence {
inline bool sizeDirty = false;
inline bool positionDirty = false;
inline uint32_t width = 0;
inline uint32_t height = 0;
inline int32_t x = 0;
inline int32_t y = 0;
inline std::chrono::steady_clock::time_point changedAt{};

inline void Flush(bool force = false) {
    if (!sizeDirty && !positionDirty) {
        return;
    }
    constexpr auto kSaveDelay = std::chrono::milliseconds(300);
    if (!force && std::chrono::steady_clock::now() - changedAt < kSaveDelay) {
        return;
    }
    if (sizeDirty && width != 0 && height != 0) {
        RuntimeConfigFile::SetWindowSize(width, height);
    }
    if (positionDirty) {
        RuntimeConfigFile::SetWindowPosition(x, y);
    }
    sizeDirty = false;
    positionDirty = false;
}
} // namespace WindowPlacementPersistence

// The close event can be consumed while execution is inside a guest fiber
// (for example, OSSleepThread).  Running normal C++/CRT shutdown from that
// fiber re-enters runtime teardown and can fault while the fiber machinery is
// still active.  A window close is an intentional successful exit, so end the
// process directly and do not run the crash/atexit paths.
namespace SdCard { void Shutdown(); }  // hle/storage/sd_card.h

// Brawl Online's core, if on, stops first as Dolphin's does when its window closes (Core::Stop,
// then the app's Online::Client::Shutdown): a Ranked set left at once ("the game is closing"), the
// replay finished, Rooms' connection closed, game-status.json removed. On a thread of its own (this
// may be a guest fiber); the window closes anyway after 4 s.
inline void StopBrawlOnlineForWindowClose() noexcept {
    if (!BrawlOnlineCore::Enabled()) {
        return;
    }
    static std::atomic<bool> done{false};
    std::fputs("[brawl-online] the window closes: the core stops first\n", stderr);
    std::thread([] {
        BrawlOnlineCore::Shutdown();
        done = true;
    }).detach();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!done && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!done) {
        std::fputs("[brawl-online] the core's stop took over 4 s: closing anyway\n", stderr);
    }
}

// The exit code when the game closes because the graphics device was lost for good (aurora_closing_for_lost_device):
// the Brawl Online launcher tells the player why (its handle_dolphin_exit_code.ts, 0x4c).
constexpr int kExitGraphicsDeviceLost = 0x4c;

[[noreturn]] inline void ExitForAuroraWindowClose() noexcept {
    const int exitCode = aurora_closing_for_lost_device() ? kExitGraphicsDeviceLost : EXIT_SUCCESS;
    StopBrawlOnlineForWindowClose();
    settings_overlay::ReleaseControllers();
    WindowPlacementPersistence::Flush(true);
    // What the game saved on the SD card (a mod's settings, replays) goes back into UserData/SD: the atexit handler
    // that does it never runs on this path.
    SdCard::Shutdown();
#if defined(_WIN32)
    // Not ExitProcess: the engine is a DLL, and ExitProcess runs its static destructors after
    // the other threads are gone; a still-joinable std::thread then calls std::terminate and the fatal popup hangs
    // the close (issue #38).
    std::fflush(nullptr);
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(exitCode));
    std::_Exit(exitCode);
#else
    std::_Exit(exitCode);
#endif
}

// Update cached Aurora window/framebuffer dimensions based on pending events.
inline void ProcessAuroraEvents(const AuroraEvent* events) {
    if (!events) {
        return;
    }

    for (const AuroraEvent* event = events; event->type != AURORA_NONE; ++event) {
        switch (event->type) {
        case AURORA_WINDOW_MOVED:
            if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED) {
                WindowPlacementPersistence::x = event->windowPos.x;
                WindowPlacementPersistence::y = event->windowPos.y;
                WindowPlacementPersistence::positionDirty = true;
                WindowPlacementPersistence::changedAt = std::chrono::steady_clock::now();
            }
            break;
        case AURORA_WINDOW_RESIZED:
            if (aurora_get_display_mode() == AURORA_DISPLAY_MODE_WINDOWED &&
                event->windowSize.width != 0 && event->windowSize.height != 0) {
                WindowPlacementPersistence::width = event->windowSize.width;
                WindowPlacementPersistence::height = event->windowSize.height;
                WindowPlacementPersistence::sizeDirty = true;
                WindowPlacementPersistence::changedAt = std::chrono::steady_clock::now();
            }
            break;
        case AURORA_EXIT:
            ExitForAuroraWindowClose();
        default:
            break;
        }
    }

    WindowPlacementPersistence::Flush();

    settings_overlay::HandleEvents(events);
}

inline bool BeginAuroraFrame() {
    if (!aurora_begin_frame()) {
        return false;
    }
    // Apply only after begin_frame: changing viewport policy during event dispatch
    // can deadlock the asynchronous frame worker. Re-read every frame because the
    // OS can resize a window without delivering a resize event.
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    AuroraGetSurfaceSize(&surfaceWidth, &surfaceHeight);
    DisplaySettings::OnFrameBoundary(surfaceWidth, surfaceHeight);
    return true;
}

// Poll Aurora events and update cached window/framebuffer dimensions.
inline void UpdateAuroraAndProcessEvents() {
    ProcessAuroraEvents(aurora_update());
}
