#pragma once

#include <cstdint>

// The main display's refresh on macOS (src/display_timing_macos.cpp), for V-Sync's display lock (hle/vi.cpp).
namespace DisplayTimingMac {

// The refresh period and the latest vertical blank, in nanoseconds (the blank on std::chrono::steady_clock's scale);
// false until the display link has reported once (it starts on the first call).
bool Read(int64_t* periodNanos, int64_t* vblankNanos) noexcept;

}  // namespace DisplayTimingMac
