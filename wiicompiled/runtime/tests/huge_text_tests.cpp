// The executable's text moved onto transparent huge pages (src/platform/huge_text.cpp). The test executable carries a
// text filler larger than 2 MiB so the routine has a whole aligned span to remap.
#include "../src/platform/huge_text.cpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))

#if defined(__x86_64__)
#define FILLER_NOP ".rept 6291456\n nop\n .endr\n"            // 6 MiB of one-byte nops
#define FILLER_RESULT "movl $42, %eax\n ret\n"
#else
#define FILLER_NOP ".rept 1572864\n nop\n .endr\n"            // 6 MiB of four-byte nops
#define FILLER_RESULT "mov w0, #42\n ret\n"
#endif

extern "C" int huge_text_filler();
asm(".text\n .balign 4096\n .globl huge_text_filler\n .type huge_text_filler,@function\nhuge_text_filler:\n"
    FILLER_NOP FILLER_RESULT ".size huge_text_filler, .-huge_text_filler\n");

namespace {

void Require(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

uint64_t Checksum(uintptr_t begin, size_t length) {
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(begin);
    for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// True when the mapping holding address has a file behind it.
bool FileBacked(uintptr_t address) {
    FILE* file = std::fopen("/proc/self/maps", "r");
    Require(file != nullptr, "open /proc/self/maps");
    char line[1024];
    bool backed = false;
    while (std::fgets(line, sizeof line, file)) {
        unsigned long lo = 0, hi = 0, offset = 0, inode = 0;
        char perms[8], device[32];
        if (std::sscanf(line, "%lx-%lx %7s %lx %31s %lu", &lo, &hi, perms, &offset, device, &inode) != 6) continue;
        if (address >= lo && address < hi) { backed = inode != 0; break; }
    }
    std::fclose(file);
    return backed;
}

} // namespace

using namespace HugeText;

int main() {
    const auto filler = reinterpret_cast<uintptr_t>(&huge_text_filler);
    Require(huge_text_filler() == 42, "filler runs before the remap");
    Require(FileBacked(filler), "text starts file-backed");

    // Off switch: nothing changes.
    setenv("BRAWL_HUGE_TEXT", "0", 1);
    HugeText::Result off = HugeText::RemapExecutableText();
    Require(off.bytes == 0, "BRAWL_HUGE_TEXT=0 remaps nothing");
    Require(FileBacked(filler), "BRAWL_HUGE_TEXT=0 leaves the file mapping");
    std::printf("off switch: %s\n", off.reason);

    setenv("BRAWL_HUGE_TEXT", "verify", 1);
    TextSegment segment;
    dl_iterate_phdr(FindMainText, &segment);
    const uintptr_t a = (segment.start + kHugePage - 1) & ~(kHugePage - 1);
    const uintptr_t b = segment.end & ~(kHugePage - 1);
    Require(b > a, "test text has a whole 2 MiB span");
    const uint64_t before = Checksum(a, b - a);

    const HugeText::Result result = HugeText::RemapExecutableText();
    if (result.bytes == 0) {
        // THP disabled, other threads, or a restricted kernel: the original mapping must be intact.
        std::printf("not remapped: %s\n", result.reason);
        Require(FileBacked(filler), "a declined remap leaves the file mapping");
        Require(huge_text_filler() == 42, "filler runs after a declined remap");
        return 0;
    }
    Require(result.start == a && result.bytes == b - a, "remapped exactly the aligned span");
    Require(Checksum(a, b - a) == before, "bytes unchanged");
    Require(huge_text_filler() == 42, "filler runs after the remap");
    Require(!FileBacked(a) && !FileBacked(b - 1), "span is anonymous");
    Require(FileBacked(segment.start) || a == segment.start, "text outside the span is untouched");
    Require(result.verified, "smaps measured");
    std::printf("remapped %zu MiB, %zu MiB on huge pages%s\n", result.bytes >> 20, result.hugeBytes >> 20,
                result.hugeBytes ? "" : " (THP not granted: allowed)");

    // A second call after the remap finds an anonymous mapping and declines.
    Require(HugeText::RemapExecutableText().bytes == 0, "second call declines");
    Require(huge_text_filler() == 42, "filler runs after the second call");
    std::printf("PASS\n");
    return 0;
}

#else
int main() { std::printf("huge text: not supported on this host, skipped\n"); return 0; }
#endif
