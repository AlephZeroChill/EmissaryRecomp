// setenv / unsetenv for the JIT tests on hosts whose C library has only the Windows spelling.
#pragma once

#include <cstdlib>

#if defined(_WIN32)
inline int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && std::getenv(name) != nullptr) return 0;
    return _putenv_s(name, value);
}
inline int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif
