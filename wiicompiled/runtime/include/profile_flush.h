#pragma once
#include <cstdio>

#if defined(MKW_PGO_GENERATE)
extern "C" int __llvm_profile_write_file(void);
#endif

// Replay harnesses and the online core terminate without running atexit handlers.
// Keep instrumentation conditionals outside callers so their PGO hashes stay stable.
inline void FlushTrainingProfile() {
#if defined(MKW_PGO_GENERATE)
    if (__llvm_profile_write_file() != 0)
        std::fputs("[profile] failed to write training profile\n", stderr);
#endif
}
