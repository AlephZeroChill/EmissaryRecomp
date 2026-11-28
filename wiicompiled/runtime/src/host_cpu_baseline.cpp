// Check the selected x86 instruction floor before C++ dynamic initialization.
// This translation unit uses baseline x86-64, outside unity builds and PCH, so
// it can report missing CPU features before optimized code executes. Keep it
// free of runtime-wide headers and allocating library facilities.

#if defined(__x86_64__)

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <cpuid.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "../include/host_isa_features.h"

namespace {

// The selected compiler baseline (host_isa_features.h names each level's features).
#if defined(MKW_HOST_ISA_V1)
constexpr int kHostIsaLevel = 1;
#elif defined(MKW_HOST_ISA_V2)
constexpr int kHostIsaLevel = 2;
#else
constexpr int kHostIsaLevel = 3;
#endif

// Fixed-capacity text accumulation: no allocation, no exceptions, nothing that
// could route through code this file is trying to stay ahead of.
struct TextBuffer {
    char data[1024] = {};
    size_t used = 0;

    void Append(const char* text) {
        if (text == nullptr) {
            return;
        }
        while (*text != '\0' && used + 1 < sizeof(data)) {
            data[used++] = *text++;
        }
        data[used] = '\0';
    }
};

// True when the host can run this build. Otherwise `missing` holds the absent
// feature names, comma separated.
bool CollectMissingBaselineFeatures(TextBuffer& missing) {
    bool first = true;
    return host_isa::Check(kHostIsaLevel, [&](const char* name) {
        if (!first) {
            missing.Append(", ");
        }
        missing.Append(name);
        first = false;
    });
}

// stdio is NOT usable from a .CRT$XIC initializer - the UCRT has not stood it
// up yet, and fprintf(stderr, ...) faults there. Verified on this toolchain:
// WriteFile on the raw standard-error handle and MessageBoxA both work, printf
// does not. Anything added to this reporting path has to respect that.
//
// The POSIX path runs from an __attribute__((constructor)) instead, ahead of libc's own startup
// guarantees; ::write() on the raw fd is the same kind of allocation-free, libc-init-independent
// primitive as WriteFile is on Windows, so the same restriction is honored here.
void WriteStdErrEarly(const char* text) {
#if defined(_WIN32)
    const HANDLE handle = ::GetStdHandle(STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    DWORD written = 0;
    ::WriteFile(handle, text, static_cast<DWORD>(length), &written, nullptr);
#else
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    (void)::write(STDERR_FILENO, text, length);
#endif
}

[[noreturn]] void ReportUnsupportedCpu(const char* missing) {
    TextBuffer message;
#if defined(MKW_HOST_ISA_V1)
    message.Append("This build needs the baseline x86-64 instruction set (SSE2).\n\nMissing on this machine: ");
    message.Append(missing);
#elif defined(MKW_HOST_ISA_V2)
    message.Append("This build needs a processor that supports the "
                   "x86-64-v2 instruction set.\n\nMissing on this machine: ");
    message.Append(missing);
#else
    message.Append(
        "This build needs a processor that supports AVX2 and the rest of the "
        "x86-64-v3 instruction set.\n\nMissing on this machine: ");
    message.Append(missing);
    message.Append(
        "\n\nx86-64-v3 covers Intel Core processors from Haswell (4th "
        "generation, 2013) onward and AMD processors from Excavator (2015) onward.");
#endif

    // The tag matches RT_TAG_RUNTIME in runtime_log.h. It is spelled out here
    // because this translation unit must not include runtime-wide headers (see
    // the file comment): runtime_log.h pulls in <iostream> and memory.h, and
    // this code runs before any C++ dynamic initializer.
    WriteStdErrEarly("[runtime] ");
    WriteStdErrEarly(message.data);
    WriteStdErrEarly("\n");

#if defined(_WIN32)
#if MKW_BRAWL_ONLINE_CORE
    ::MessageBoxA(nullptr, message.data, "Emissary - Unsupported Processor",
#else
    ::MessageBoxA(nullptr, message.data, "WiiCompiled - Unsupported Processor",
#endif
                  MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
    // Leave through the OS rather than exit(): the C++ dynamic initializers
    // have not run yet, so there is no constructed program state to unwind and
    // the teardown path itself lives in AVX2 translation units.
    ::ExitProcess(1u);
#else
    // Same reasoning as the Windows path above: no C++ dynamic initializer has run yet, so
    // _exit() (skips atexit/global destructors, unlike exit()) is the correct way out.
    ::_exit(1);
#endif
}

}  // namespace

extern "C" int MkwHostCpuBaselineInit() {
    TextBuffer missing;
    if (!CollectMissingBaselineFeatures(missing)) {
        ReportUnsupportedCpu(missing.data);
    }
    return 0;
}

// Priorities 0-100 are reserved for the implementation; 101 is the earliest a
// user constructor can request, which puts this ahead of every default-priority
// constructor in the image.
__attribute__((constructor(101))) static void MkwHostCpuBaselineCtor() {
    MkwHostCpuBaselineInit();
}

#else  // !defined(__x86_64__)

extern "C" int MkwHostCpuBaselineInit() {
    return 0;
}

#endif  // defined(__x86_64__)
