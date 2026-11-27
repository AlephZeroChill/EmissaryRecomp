#pragma once

#include <cstddef>
#include <cstdint>

// The executable's translated code is ~144 MiB of text; on 4 KiB pages the instruction TLB walks page tables
// constantly. On Linux, RemapExecutableText() copies the executable's 2 MiB-aligned text span into an anonymous
// mapping that asks for transparent huge pages and moves it over the original (huge_text.cpp has the details).
// Every other host compiles it to a no-op that reports why.
namespace HugeText {

struct Result {
    uintptr_t start = 0;      // first remapped byte (2 MiB aligned); 0 when nothing was remapped
    size_t bytes = 0;         // remapped length, a multiple of 2 MiB; 0 when nothing was remapped
    size_t hugeBytes = 0;     // AnonHugePages inside the span; filled only when BRAWL_HUGE_TEXT=verify
    bool verified = false;    // hugeBytes was measured
    const char* reason = "";  // why nothing was remapped (a literal), empty on success
};

// BRAWL_HUGE_TEXT=0 turns it off, =verify also measures the result in /proc/self/smaps. Call once, before any other
// thread exists (it declines when it finds more than one). Never throws and never leaves the text unmapped: any
// failure keeps the original mapping.
Result RemapExecutableText() noexcept;

} // namespace HugeText
