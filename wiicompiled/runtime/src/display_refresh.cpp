// display_refresh.cpp - G-Sync / FreeSync detection for V-Sync's automatic setting (display_refresh.h).

#include "display_refresh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <random>
#include <thread>

#include <SDL3/SDL.h>
#include <aurora/gfx.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "runtime_log.h"

namespace DisplayRefresh {

int Judge(double vblankShare, double framesPerSecond, double refreshHz) noexcept {
    // A 60-75 Hz screen looks the same either way at 60 frames a second, and frames as fast as the refresh leave no
    // blank to wait in; an idle window (a minimized game) says nothing.
    if (refreshHz < 75.0 || framesPerSecond < 30.0 || framesPerSecond > 0.9 * refreshHz) {
        return -1;
    }
    // Fixed: about a tenth of each refresh is blank (9-13% measured on a 2560x1440 240 Hz screen). Following 60 frames
    // at 144 Hz, the scan takes 1/144 s of each 1/60 s, so the screen is blank ~60% of the time (~78% at 240 Hz).
    const double following = 1.0 - framesPerSecond / refreshHz;
    if (vblankShare > std::max(0.3, 0.6 * following + 0.05)) {
        return 1;
    }
    if (vblankShare < 0.25) {
        return 0;
    }
    return -1;
}

namespace {

std::atomic<int> g_state{-1};
std::atomic<double> g_refreshHz{0.0};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_started{false};

#if defined(_WIN32)
using D3DKMT_HANDLE = UINT;
struct OpenAdapterFromGdiDisplayName {
    WCHAR DeviceName[32];
    D3DKMT_HANDLE hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
};
struct GetScanLine {
    D3DKMT_HANDLE hAdapter;
    UINT VidPnSourceId;
    BOOLEAN InVerticalBlank;
    UINT ScanLine;
};
struct CloseAdapter {
    D3DKMT_HANDLE hAdapter;
};
using OpenFn = LONG(APIENTRY*)(OpenAdapterFromGdiDisplayName*);
using ScanFn = LONG(APIENTRY*)(GetScanLine*);
using CloseFn = LONG(APIENTRY*)(const CloseAdapter*);

void Watch(HWND window) {
    HMODULE gdi = ::GetModuleHandleW(L"gdi32.dll");
    if (gdi == nullptr) gdi = ::LoadLibraryW(L"gdi32.dll");
    const auto open = gdi ? reinterpret_cast<OpenFn>(reinterpret_cast<void*>(
                                ::GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName")))
                          : nullptr;
    const auto scan = gdi ? reinterpret_cast<ScanFn>(reinterpret_cast<void*>(::GetProcAddress(gdi, "D3DKMTGetScanLine")))
                          : nullptr;
    const auto close = gdi ? reinterpret_cast<CloseFn>(reinterpret_cast<void*>(::GetProcAddress(gdi, "D3DKMTCloseAdapter")))
                           : nullptr;
    if (open == nullptr || scan == nullptr || close == nullptr) {
        RT_LOGF(RT_TAG_CONFIG, "display: no D3DKMTGetScanLine; V-Sync's automatic setting keeps it on\n");
        return;
    }
    HMONITOR monitor = nullptr;
    OpenAdapterFromGdiDisplayName adapter{};
    bool haveAdapter = false;
    double refresh = 0;
    int pending = -1, votes = 0;
    uint64_t presentsBefore = 0;
    auto windowStart = std::chrono::steady_clock::now();
    int samples = 0, blank = 0;
    std::minstd_rand jitter(12345);
    while (!g_stop.load(std::memory_order_relaxed)) {
        // Each window: the screen the game is on now (it can be dragged to another), then a second of samples
        // a few milliseconds apart, never in step with the refresh.
        const HMONITOR now = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
        if (now != monitor || !haveAdapter) {
            if (haveAdapter) {
                const CloseAdapter c{adapter.hAdapter};
                close(&c);
                haveAdapter = false;
            }
            monitor = now;
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (monitor != nullptr && ::GetMonitorInfoW(monitor, &info)) {
                adapter = {};
                wcsncpy_s(adapter.DeviceName, 32, info.szDevice, _TRUNCATE);
                haveAdapter = open(&adapter) == 0;
                DEVMODEW mode{};
                mode.dmSize = sizeof(mode);
                refresh = ::EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) ? mode.dmDisplayFrequency : 0;
                g_refreshHz.store(refresh, std::memory_order_relaxed);
            }
            pending = -1;
            votes = 0;
            samples = blank = 0;
            windowStart = std::chrono::steady_clock::now();
            AuroraPresentTiming timing{};
            aurora_get_present_timing(&timing);
            presentsBefore = timing.totalPresentCount;
            if (!haveAdapter) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
        }
        GetScanLine line{};
        line.hAdapter = adapter.hAdapter;
        line.VidPnSourceId = adapter.VidPnSourceId;
        if (scan(&line) == 0) {
            ++samples;
            blank += line.InVerticalBlank ? 1 : 0;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(2500 + static_cast<int>(jitter() % 2000)));
        const auto elapsed = std::chrono::steady_clock::now() - windowStart;
        if (elapsed < std::chrono::seconds(1)) continue;

        AuroraPresentTiming timing{};
        aurora_get_present_timing(&timing);
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const double fps = static_cast<double>(timing.totalPresentCount - presentsBefore) / seconds;
        const double share = samples > 0 ? static_cast<double>(blank) / samples : 0.0;
        const int vote = samples >= 100 ? Judge(share, fps, refresh) : -1;
        if (vote < 0) {
            pending = -1;
            votes = 0;
        } else if (vote != pending) {
            pending = vote;
            votes = 1;
        } else if (++votes >= 2 && vote != g_state.load(std::memory_order_relaxed)) {
            g_state.store(vote, std::memory_order_relaxed);
            aurora_set_display_variable_refresh(vote);
            RT_LOGF(RT_TAG_CONFIG, "display: %ls at %.0f Hz %s (%d%% of %d samples in the vertical blank at %.0f fps)\n",
                    adapter.DeviceName, refresh, vote == 1 ? "follows the frames (G-Sync/FreeSync)" : "refreshes at a fixed rate",
                    static_cast<int>(std::lround(share * 100)), samples, fps);
        }
        // The first windows, and every one with BRAWL_LOG_VRR set (measurements).
        static const bool logEvery = std::getenv("BRAWL_LOG_VRR") != nullptr;
        static int s_windowsLogged = 0;
        if (logEvery || s_windowsLogged++ < 3) {
            RT_LOGF(RT_TAG_CONFIG, "display: %ls %.0f Hz, vblank %d%% of %d samples, %.1f fps -> %d\n", adapter.DeviceName,
                    refresh, static_cast<int>(std::lround(share * 100)), samples, fps, vote);
        }
        samples = blank = 0;
        windowStart = std::chrono::steady_clock::now();
        presentsBefore = timing.totalPresentCount;
        // The screen's mode can change (a fullscreen switch to another refresh): read it again each window.
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (monitor != nullptr && ::GetMonitorInfoW(monitor, &info) &&
            ::EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency != refresh) {
            refresh = mode.dmDisplayFrequency;
            g_refreshHz.store(refresh, std::memory_order_relaxed);
            pending = -1;
            votes = 0;
        }
    }
    if (haveAdapter) {
        const CloseAdapter c{adapter.hAdapter};
        close(&c);
    }
}
#endif

}  // namespace

void Start(SDL_Window* window) noexcept {
#if defined(_WIN32)
    if (window == nullptr || g_started.exchange(true)) return;
    HWND hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr) return;
    g_stop.store(false);
    try {
        // Detached: the process may end without Stop() (an _Exit), and it only reads the screen's state.
        std::thread([hwnd] {
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            Watch(hwnd);
        }).detach();
    } catch (...) {
    }
#else
    (void)window;
#endif
}

void Stop() noexcept { g_stop.store(true); }

int VariableRefresh() noexcept { return g_state.load(std::memory_order_relaxed); }

double RefreshHz() noexcept { return g_refreshHz.load(std::memory_order_relaxed); }

}  // namespace DisplayRefresh
