// `WiiCompiled --write-hang-dump <pid> <dump path>`: writes a minidump of process <pid> (every thread's stack and
// registers, thread times and handles; no heap) to <dump path>. The hang watchdog (brawl_online_harness.cpp,
// CoreWatchHangs) starts this program that way when the game's window stopped responding: a second process, as
// MiniDumpWriteDump in the hung one can block on a lock (the heap's, the loader's) its stuck thread holds. Header-only,
// shared by the loader executable (src/product/engine_loader.cpp, which handles it before loading the engine) and
// RuntimeMain (a build whose executable is the engine). dbghelp is loaded here, so neither links it.
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>

#include <cstring>
#include <cwchar>

namespace HangDumpHelper {

inline constexpr const char* kSwitch = "--write-hang-dump";
inline constexpr const wchar_t* kSwitchW = L"--write-hang-dump";

// argv[1] is kSwitch: the dump is written and the exit code returned (0 written; 2 bad arguments, 3 no such
// process, 4 no dbghelp, 5 the file could not be made, 6 MiniDumpWriteDump failed). -1: not this mode. The path is
// read from the wide command line (a user folder can have any characters). Needs shell32 (CommandLineToArgvW).
inline int Run(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], kSwitch) != 0) return -1;
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc < 4) return 2;
    const DWORD pid = static_cast<DWORD>(std::wcstoul(wargv[2], nullptr, 10));
    const wchar_t* path = wargv[3];
    if (pid == 0) return 2;
    const HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid);
    if (!process) return 3;
    using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    const auto write = dbghelp ? reinterpret_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!write) {
        CloseHandle(process);
        return 4;
    }
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        CloseHandle(process);
        return 5;
    }
    // Small enough for the launcher's bug report (8 MB a file): no heap, no memory near the stacks.
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithHandleData |
                                                 MiniDumpWithUnloadedModules);
    const BOOL ok = write(process, pid, file, type, nullptr, nullptr, nullptr);
    CloseHandle(file);
    CloseHandle(process);
    LocalFree(wargv);
    return ok ? 0 : 6;
}

}  // namespace HangDumpHelper
#endif
