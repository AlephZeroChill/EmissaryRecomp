#include "platform/huge_text.h"

#if defined(__linux__) && !defined(__ANDROID__) && (defined(__x86_64__) || defined(__aarch64__))

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25  // Linux 6.1; older kernels answer EINVAL, which is fine
#endif

namespace HugeText {
namespace {

constexpr uintptr_t kHugePage = uintptr_t{2} << 20;

struct TextSegment {
    uintptr_t start = 0;
    uintptr_t end = 0;
};

// The main program is the first object dl_iterate_phdr reports. Its executable PT_LOAD is the text, whether the
// binary is PIE (dlpi_addr is the load bias) or not (bias 0). Shared libraries and the vdso are never visited.
int FindMainText(dl_phdr_info* info, size_t, void* user) {
    auto* segment = static_cast<TextSegment*>(user);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type == PT_LOAD && (header.p_flags & PF_X) != 0 && (header.p_flags & PF_W) == 0) {
            segment->start = info->dlpi_addr + header.p_vaddr;
            segment->end = info->dlpi_addr + header.p_vaddr + header.p_memsz;
        }
    }
    return 1;  // the first entry only
}

bool ThpUsable() {
    char text[128] = {};
    FILE* file = std::fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (!file) return false;
    const bool read = std::fgets(text, sizeof text, file) != nullptr;
    std::fclose(file);
    if (!read || std::strstr(text, "[never]")) return false;
    unsigned long size = 0;
    file = std::fopen("/sys/kernel/mm/transparent_hugepage/hpage_pmd_size", "r");
    if (!file) return false;
    const bool ok = std::fscanf(file, "%lu", &size) == 1;
    std::fclose(file);
    return ok && size == kHugePage;  // a 64 KiB-page aarch64 kernel has 512 MiB huge pages: not worth a copy
}

bool OnlyThread() {
    DIR* directory = opendir("/proc/self/task");
    if (!directory) return false;
    int count = 0;
    while (const dirent* entry = readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
    }
    closedir(directory);
    return count == 1;
}

// The one mapping holding [begin, end) must be a private, read-execute file mapping; anything else (a debugger's
// or a loader's own remap, a split mapping) means the layout is not what the copy assumes.
bool IsSingleFileTextMapping(uintptr_t begin, uintptr_t end) {
    FILE* file = std::fopen("/proc/self/maps", "r");
    if (!file) return false;
    char line[1024];
    bool ok = false;
    while (std::fgets(line, sizeof line, file)) {
        unsigned long lo = 0, hi = 0, offset = 0, inode = 0;
        char perms[8] = {}, device[32] = {};
        if (std::sscanf(line, "%lx-%lx %7s %lx %31s %lu", &lo, &hi, perms, &offset, device, &inode) != 6) continue;
        if (begin >= lo && begin < hi) {
            ok = end <= hi && std::strcmp(perms, "r-xp") == 0 && inode != 0;
            break;
        }
    }
    std::fclose(file);
    return ok;
}

// Sums AnonHugePages of the smaps entries that overlap [begin, end). Returns false if smaps can't be read.
bool MeasureHuge(uintptr_t begin, uintptr_t end, size_t* hugeBytes) {
    FILE* file = std::fopen("/proc/self/smaps", "r");
    if (!file) return false;
    char line[1024];
    bool inside = false;
    size_t total = 0;
    while (std::fgets(line, sizeof line, file)) {
        unsigned long lo = 0, hi = 0;
        if (std::sscanf(line, "%lx-%lx", &lo, &hi) == 2 && std::strchr(line, '-') < std::strchr(line, ' ')) {
            inside = lo < end && hi > begin;
        } else if (inside && std::strncmp(line, "AnonHugePages:", 14) == 0) {
            unsigned long kib = 0;
            if (std::sscanf(line + 14, "%lu", &kib) == 1) total += static_cast<size_t>(kib) << 10;
        }
    }
    std::fclose(file);
    *hugeBytes = total;
    return true;
}

} // namespace

// Why this is safe. The span [a, b) is whole 2 MiB pages inside the executable's text, so no other segment shares a
// page with it. Steps, each recoverable:
//   1. mmap a scratch anonymous region and trim it to a 2 MiB-aligned block of the span's length; madvise it for huge
//      pages. Failure: unmap, keep running on the original mapping.
//   2. memcpy the text into it (the original is a readable file mapping), then mprotect it read-execute. No thread
//      other than this one exists (checked), and text is never written, so the copy is the bytes that will run.
//   3. One mremap(MREMAP_FIXED) moves the scratch block over [a, b). The kernel replaces the destination under the
//      address-space lock, so there is no instant at which [a, b) is unmapped, and it either happens whole or
//      returns an error with the original untouched. This routine itself lives in the span, which is fine: the
//      syscall is in libc, and on return the program counter lies in the new mapping, which holds identical bytes.
//   Afterwards MADV_COLLAPSE (where the kernel has it) turns any small pages into huge ones now instead of waiting
//   for khugepaged. Nothing after the mremap can fail in a way that matters.
// Cost: the copied text is private anonymous memory, about the size of the resident text, where the file mapping
// was shared page cache; perf and gdb also lose file-backed symbolisation of the range (BRAWL_HUGE_TEXT=0 keeps it).
Result RemapExecutableText() noexcept {
    Result result;
    const char* mode = std::getenv("BRAWL_HUGE_TEXT");
    if (mode && std::strcmp(mode, "0") == 0) { result.reason = "disabled by BRAWL_HUGE_TEXT=0"; return result; }
    const bool verify = mode && std::strcmp(mode, "verify") == 0;

    TextSegment text;
    dl_iterate_phdr(FindMainText, &text);
    if (text.start == 0 || text.end <= text.start) { result.reason = "executable text not found"; return result; }
    const uintptr_t a = (text.start + kHugePage - 1) & ~(kHugePage - 1);
    const uintptr_t b = text.end & ~(kHugePage - 1);
    if (b <= a) { result.reason = "text has no whole 2 MiB span"; return result; }
    if (!ThpUsable()) { result.reason = "transparent huge pages unavailable"; return result; }
    if (!OnlyThread()) { result.reason = "other threads exist"; return result; }
    if (!IsSingleFileTextMapping(a, b)) { result.reason = "text is not one file mapping"; return result; }

    const size_t length = b - a;
    void* raw = mmap(nullptr, length + kHugePage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) { result.reason = "mmap failed"; return result; }
    const uintptr_t rawAddress = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t block = (rawAddress + kHugePage - 1) & ~(kHugePage - 1);
    if (block > rawAddress) munmap(raw, block - rawAddress);
    const uintptr_t tail = block + length;
    if (rawAddress + length + kHugePage > tail) munmap(reinterpret_cast<void*>(tail), rawAddress + length + kHugePage - tail);
    void* copy = reinterpret_cast<void*>(block);

    if (madvise(copy, length, MADV_HUGEPAGE) != 0) { munmap(copy, length); result.reason = "madvise(MADV_HUGEPAGE) failed"; return result; }
    std::memcpy(copy, reinterpret_cast<const void*>(a), length);
    if (mprotect(copy, length, PROT_READ | PROT_EXEC) != 0) { munmap(copy, length); result.reason = "mprotect failed"; return result; }
    if (mremap(copy, length, length, MREMAP_MAYMOVE | MREMAP_FIXED, reinterpret_cast<void*>(a)) == MAP_FAILED) {
        munmap(copy, length);
        result.reason = "mremap failed";
        return result;
    }
    madvise(reinterpret_cast<void*>(a), length, MADV_COLLAPSE);

    result.start = a;
    result.bytes = length;
    if (verify) result.verified = MeasureHuge(a, b, &result.hugeBytes);
    return result;
}

} // namespace HugeText

#else

namespace HugeText {
Result RemapExecutableText() noexcept {
    Result result;
    result.reason = "not supported on this host";
    return result;
}
} // namespace HugeText

#endif
