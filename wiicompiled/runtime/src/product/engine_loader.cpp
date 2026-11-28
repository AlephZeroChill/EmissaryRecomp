// Windows launcher for Brawl's engine_usa.dll and optional engine_usa_v2.dll.
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "../../include/hang_dump_helper.h"
#include "../../include/host_isa_features.h"

namespace fs = std::filesystem;

namespace {

fs::path ExeDirectory() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

} // namespace

// The usual engine needs x86-64-v3 (AVX2, FMA, BMI2...): exactly the check that engine makes of the processor itself
// (host_isa_features.h). Older processors get engine_<region>_v2.dll.
bool HostHasV3() {
    return host_isa::Check(3, [](const char*) {});
}

// The engine this processor plays with: engine_usa_v2.dll without x86-64-v3 or with BRAWL_ENGINE_V2 set, else
// engine_usa.dll. Releases ship engine_usa_v2.dll on its own (EmissaryRecomp-<tag>-Windows-OldCPU.zip), so it may
// not be in the folder.
bool WantsOldCpuEngine() {
    return std::getenv("BRAWL_ENGINE_V2") != nullptr || !HostHasV3();
}

int main(int argc, char** argv) {
    // The hang watchdog's dump of a running game (hang_dump_helper.h): no engine needed.
    if (const int dumped = HangDumpHelper::Run(argc, argv); dumped >= 0) return dumped;
    const bool oldCpu = WantsOldCpuEngine();
    const wchar_t* name = oldCpu ? L"engine_usa_v2.dll" : L"engine_usa.dll";
    // `WiiCompiled --engine`: prints the engine's file name and exits, loading nothing. Brawl Online's launcher asks
    // before every start and downloads engine_usa_v2.dll when this names it and the folder lacks it.
    if (argc >= 2 && std::strcmp(argv[1], "--engine") == 0) {
        const char* text = oldCpu ? "engine_usa_v2.dll\n" : "engine_usa.dll\n";
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        if (out == nullptr || out == INVALID_HANDLE_VALUE ||
            !WriteFile(out, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr)) {
            return 2;
        }
        return 0;
    }
    const fs::path exeDir = ExeDirectory();
    const fs::path module = exeDir / name;
    std::error_code ec;
    if (oldCpu && !fs::exists(module, ec)) {
        const std::wstring message =
            L"This processor plays with engine_usa_v2.dll (the engine for processors without AVX2), which is not in "
            L"this folder:\n" + exeDir.wstring() +
            L"\n\nBrawl Online's launcher downloads it before it starts the game. If you downloaded EmissaryRecomp "
            L"yourself, unzip EmissaryRecomp-<version>-Windows-OldCPU.zip from the same release into this folder too.";
        MessageBoxW(nullptr, message.c_str(), L"Emissary", MB_OK | MB_ICONERROR);
        return 1;
    }
    HMODULE engine = LoadLibraryExW(module.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    auto entry = engine ? reinterpret_cast<int (*)(int, char**)>(GetProcAddress(engine, "SDL_main")) : nullptr;
    if (!entry) {
        const std::wstring message = L"The game engine could not be loaded:\n" + module.wstring() +
                                     L"\n(error " + std::to_wstring(GetLastError()) + L")\n\nUnzip the whole download again.";
        MessageBoxW(nullptr, message.c_str(), L"Super Smash Bros. Brawl", MB_OK | MB_ICONERROR);
        return 1;
    }
    return entry(argc, argv);
}
