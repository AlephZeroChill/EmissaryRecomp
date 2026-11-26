// Dolphin's controls (core side, brawl_online_bridge.h): the controllers as set up in Dolphin's
// Controllers settings, read with Dolphin's own input code (brawlonline.net core/standalone,
// BrawlOnline/DolphinPads.h) from the core's User folder (BRAWL_ONLINE_CORE: the netplay Dolphin's),
// and Dolphin's Controllers window opened over the game (BRAWL_ONLINE_DOLPHIN: the netplay Dolphin's
// executable, which the launcher passes).

#include "brawl_online_bridge.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#if MKW_DOLPHIN_CONTROLS
#include "BrawlOnline/DolphinPads.h"
#endif

namespace BrawlOnlineBridge {

#if MKW_DOLPHIN_CONTROLS
namespace {
namespace DP = BrawlOnline::DolphinPads;

std::mutex s_mutex;
std::string s_user;
bool s_loaded = false;
std::chrono::steady_clock::time_point s_next_check{};
// The devices are read at most every 4 ms (as the runtime's own pads, virtual_remote.cpp): the core asks
// for the pads several times a frame, and relative inputs (the mouse) count their motion per read.
std::chrono::steady_clock::time_point s_next_update{};
std::chrono::steady_clock::time_point s_next_sdl_sync{};
#ifdef _WIN32
HANDLE s_window_process = nullptr;
// Dolphin's Controllers window lives only as long as the game: a job that ends it when the game's process ends, however
// it ends (closed, crash, _Exit, the launcher's kill). Left open, it kept the GameCube adapter, so the next game found no
// controller and the next window said "Insufficient permissions" (Sarah's board, card 797). Dolphin saves each change
// as it is made, and a posted WM_CLOSE doesn't close that window, so ending it loses nothing and needs no asking.
HANDLE s_window_job = nullptr;
#else
// macOS and Linux: the window runs under a small shell that ends it when the game's process is gone (the job's part),
// checking twice a second; this is the shell's pid while it runs.
pid_t s_window_watcher = 0;
#endif

std::wstring Wide(const std::string& text) {
#ifdef _WIN32
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
#else
    return std::wstring(text.begin(), text.end());
#endif
}

bool WindowShowingLocked() {
#ifdef _WIN32
    if (s_window_process == nullptr) return false;
    if (WaitForSingleObject(s_window_process, 0) == WAIT_TIMEOUT) return true;
    CloseHandle(s_window_process);
    s_window_process = nullptr;
    // Closed: what it saved counts from the next read on (DolphinControlsRead checks at once).
    s_next_check = {};
    return false;
#else
    if (s_window_watcher <= 0) return false;
    int status = 0;
    if (waitpid(s_window_watcher, &status, WNOHANG) == 0) return true;
    s_window_watcher = 0;
    s_next_check = {};
    return false;
#endif
}
}  // namespace

bool DolphinControlsLoad(const char* user_dir, void* window) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (user_dir == nullptr || *user_dir == '\0') return false;
    s_user = user_dir;
    s_loaded = true;
    const bool active = DP::Load(s_user, window);
    std::fprintf(stderr, "[brawl-online] Dolphin's controls: %s\n",
                 active ? "on (Config/GCPadNew.ini)" : "off (no Config/GCPadNew.ini)");
    for (int port = 0; port < 4 && active; ++port) {
        std::fprintf(stderr, "[brawl-online]   port %d: SIDevice %d, %s\n", port + 1, DP::SiDevice(port),
                     DP::SiDevice(port) == DP::SI_STANDARD_CONTROLLER ? DP::DefaultDevice(port).c_str() : "-");
    }
    return active;
}

bool DolphinControlsActive() {
    return DP::Active();
}

int DolphinControlsSiDevice(int port) {
    return DP::SiDevice(port);
}

void DolphinControlsRead(bool window_has_focus, Pad* pads, int* si_devices) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_loaded) return;
        // Dolphin's window saves GCPadNew.ini (and Dolphin.ini) when a setting changes and when it closes.
        const auto now = std::chrono::steady_clock::now();
        if (now >= s_next_check) {
            s_next_check = now + std::chrono::seconds(1);
            WindowShowingLocked();
            if (DP::ReloadIfChanged()) {
                std::fprintf(stderr, "[brawl-online] Dolphin's controls: loaded again (%s)\n",
                             DP::Active() ? "on" : "off");
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= s_next_update) {
        s_next_update = now + std::chrono::milliseconds(4);
        // A controller plugged in or out shows within half a second (SDL's list, not its events: the runtime has those).
        if (now >= s_next_sdl_sync) {
            s_next_sdl_sync = now + std::chrono::milliseconds(500);
            DP::SyncSdlDevices();
        }
        DP::UpdateInput(window_has_focus);
    }
    for (int port = 0; port < 4; ++port) {
        si_devices[port] = DP::SiDevice(port);
        pads[port] = {};
        if (si_devices[port] != DP::SI_STANDARD_CONTROLLER) continue;
        const GCPadStatus s = DP::GetStatus(port);
        pads[port].connected = s.isConnected;
        pads[port].button = s.button;
        pads[port].stickX = static_cast<int8_t>(static_cast<int>(s.stickX) - 128);
        pads[port].stickY = static_cast<int8_t>(static_cast<int>(s.stickY) - 128);
        pads[port].substickX = static_cast<int8_t>(static_cast<int>(s.substickX) - 128);
        pads[port].substickY = static_cast<int8_t>(static_cast<int>(s.substickY) - 128);
        pads[port].triggerL = s.triggerLeft;
        pads[port].triggerR = s.triggerRight;
        // BRAWL_LOG_DOLPHIN_PADS=1: each port's pad whenever it changes (what Dolphin's settings make of the devices).
        static const bool log = std::getenv("BRAWL_LOG_DOLPHIN_PADS") != nullptr;
        static GCPadStatus last[4]{};
        if (log && (s.isConnected != last[port].isConnected || s.button != last[port].button ||
                    s.stickX != last[port].stickX || s.stickY != last[port].stickY ||
                    s.substickX != last[port].substickX || s.substickY != last[port].substickY ||
                    s.triggerLeft != last[port].triggerLeft || s.triggerRight != last[port].triggerRight)) {
            std::fprintf(stderr,
                         "[brawl-online] Dolphin's controls: port %d connected %d buttons %04x stick %d,%d c %d,%d L %d R %d\n",
                         port + 1, s.isConnected, s.button, s.stickX, s.stickY, s.substickX, s.substickY,
                         s.triggerLeft, s.triggerRight);
            last[port] = s;
        }
    }
}

void DolphinControlsPeek(bool window_has_focus, Pad* pads, int* si_devices) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_loaded) return;
    }
    DP::UpdateInput(window_has_focus, true);
    for (int port = 0; port < 4; ++port) {
        si_devices[port] = DP::SiDevice(port);
        pads[port] = {};
        if (si_devices[port] != DP::SI_STANDARD_CONTROLLER) continue;
        const GCPadStatus s = DP::GetStatus(port, true);
        pads[port].connected = s.isConnected;
        pads[port].button = s.button;
        pads[port].stickX = static_cast<int8_t>(static_cast<int>(s.stickX) - 128);
        pads[port].stickY = static_cast<int8_t>(static_cast<int>(s.stickY) - 128);
        pads[port].substickX = static_cast<int8_t>(static_cast<int>(s.substickX) - 128);
        pads[port].substickY = static_cast<int8_t>(static_cast<int>(s.substickY) - 128);
        pads[port].triggerL = s.triggerLeft;
        pads[port].triggerR = s.triggerRight;
    }
}

void DolphinControlsRumble(int port, double strength) {
    DP::SetRumble(port, strength);
}

bool DolphinControlsUseKey(const char* key) {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_loaded || !DP::Active() || key == nullptr || *key == '\0') return false;
        path = s_user + "/Config/GCPadNew.ini";
    }
    // Dolphin names keyboard keys by word in its expressions (Buttons/Start = ESCAPE, `ESCAPE` | RETURN).
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string word = key;
    const auto partOfWord = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_'; };
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
        const size_t end = at + word.size();
        if ((at == 0 || !partOfWord(text[at - 1])) && (end >= text.size() || !partOfWord(text[end]))) return true;
    }
    return false;
}

void DolphinControlsDevice(int port, char* out, size_t size) {
    if (size == 0) return;
    std::snprintf(out, size, "%s", DP::DefaultDevice(port).c_str());
}

bool DolphinControllersAvailable() {
    const char* exe = std::getenv("BRAWL_ONLINE_DOLPHIN");
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_loaded && exe != nullptr && *exe != '\0';
}

bool DolphinControllersShowing() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return WindowShowingLocked();
}

bool DolphinControllersOpen() {
    const char* exe = std::getenv("BRAWL_ONLINE_DOLPHIN");
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_loaded || exe == nullptr || *exe == '\0' || WindowShowingLocked()) return false;
#ifdef _WIN32
    // As the launcher's "configure controllers" starts it: the netplay User folder, the Controllers window only.
    std::wstring command = L"\"" + Wide(exe) + L"\" -u \"" + Wide(s_user) + L"\" --controllers";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (s_window_job == nullptr) {
        s_window_job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (s_window_job != nullptr &&
            !SetInformationJobObject(s_window_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(s_window_job);
            s_window_job = nullptr;
        }
    }
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup,
                        &process)) {
        std::fprintf(stderr, "[brawl-online] Dolphin's Controllers window: could not start %s (error %lu)\n", exe,
                     GetLastError());
        return false;
    }
    if (s_window_job == nullptr || !AssignProcessToJobObject(s_window_job, process.hProcess)) {
        std::fprintf(stderr, "[brawl-online] Dolphin's Controllers window: not tied to the game (error %lu)\n",
                     GetLastError());
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    s_window_process = process.hProcess;
    std::fprintf(stderr, "[brawl-online] Dolphin's Controllers window: started (pid %lu)\n", process.dwProcessId);
    return true;
#else
    // As on Windows, and tied to the game: the shell starts Dolphin ($2 -u $3 --controllers) and ends it once the
    // game ($1) is gone, however the game ended; it ends by itself when Dolphin's window is closed.
    static const char kWatcher[] =
        "\"$2\" -u \"$3\" --controllers & d=$!\n"
        "trap 'kill $d 2>/dev/null; exit 0' TERM\n"
        "while kill -0 \"$1\" 2>/dev/null && kill -0 $d 2>/dev/null; do sleep 0.5; done\n"
        "kill $d 2>/dev/null\n"
        "wait $d 2>/dev/null\n";
    const std::string game = std::to_string(static_cast<long>(getpid()));
    const std::string user = s_user;
    char* const argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(kWatcher),
                          const_cast<char*>("sh"), const_cast<char*>(game.c_str()), const_cast<char*>(exe),
                          const_cast<char*>(user.c_str()), nullptr};
    pid_t pid = 0;
    const int error = posix_spawn(&pid, "/bin/sh", nullptr, nullptr, argv, environ);
    if (error != 0) {
        std::fprintf(stderr, "[brawl-online] Dolphin's Controllers window: could not start %s (error %d)\n", exe, error);
        return false;
    }
    s_window_watcher = pid;
    std::fprintf(stderr, "[brawl-online] Dolphin's Controllers window: started (watcher pid %ld)\n",
                 static_cast<long>(pid));
    return true;
#endif
}

#else  // a core without Dolphin's input library: the runtime keeps its own controls

bool DolphinControlsLoad(const char*, void*) { return false; }
bool DolphinControlsActive() { return false; }
int DolphinControlsSiDevice(int) { return 0; }
void DolphinControlsRead(bool, Pad*, int*) {}
void DolphinControlsRumble(int, double) {}
bool DolphinControlsUseKey(const char*) { return false; }
void DolphinControlsDevice(int, char* out, size_t size) { if (size) *out = '\0'; }
bool DolphinControllersAvailable() { return false; }
bool DolphinControllersShowing() { return false; }
bool DolphinControllersOpen() { return false; }

#endif

}  // namespace BrawlOnlineBridge
