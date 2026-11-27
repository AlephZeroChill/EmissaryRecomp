#include "jit/jit_exec_memory.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#define MKW_JIT_MEMORY_HOST 1
#elif defined(_WIN32)
#include <windows.h>
#define MKW_JIT_MEMORY_HOST 1
#else
#define MKW_JIT_MEMORY_HOST 0
#endif

namespace JitExecMemory {
namespace {

#if MKW_JIT_MEMORY_HOST

size_t PageSize() {
    static const size_t size = [] {
#if defined(_WIN32)
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        return static_cast<size_t>(info.dwPageSize);
#else
        long value = sysconf(_SC_PAGESIZE);
        return value > 0 ? static_cast<size_t>(value) : static_cast<size_t>(4096);
#endif
    }();
    return size;
}

Region MapRegion(size_t bytes) {
    Region region;
    if (bytes == 0) {
        return region;
    }
    const size_t page = PageSize();
    if (bytes > SIZE_MAX - page) {
        return region;
    }
    const size_t size = (bytes + page - 1) / page * page;
#if defined(_WIN32)
    void* base = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!base) {
        return region;
    }
#else
    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return region;
    }
#endif
    region.base = base;
    region.size = size;
    return region;
}

#if defined(__linux__)
// A region of whole 2 MiB pages starting on a 2 MiB boundary, advised for transparent huge pages (the advice is a hint: a
// host that refuses it still has the mapping).
Region MapRegionHuge(size_t bytes) {
    Region region;
    constexpr size_t kHuge = Arena::kHugePageBytes;
    if (bytes == 0 || bytes > SIZE_MAX - 2 * kHuge) {
        return region;
    }
    const size_t size = (bytes + kHuge - 1) / kHuge * kHuge;
    void* raw = mmap(nullptr, size + kHuge, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        return region;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t aligned = (start + kHuge - 1) & ~(uintptr_t{kHuge} - 1);
    if (aligned > start) {
        munmap(raw, aligned - start);
    }
    const uintptr_t end = start + size + kHuge;
    if (end > aligned + size) {
        munmap(reinterpret_cast<void*>(aligned + size), end - (aligned + size));
    }
#if defined(MADV_HUGEPAGE)
    // BRAWL_JIT_NO_HUGEPAGES=1 (a diagnostic for measuring): the same aligned chunks without the advice
    static const bool noAdvice = [] { const char* v = std::getenv("BRAWL_JIT_NO_HUGEPAGES"); return v != nullptr && *v != '\0' && *v != '0'; }();
    if (!noAdvice) madvise(reinterpret_cast<void*>(aligned), size, MADV_HUGEPAGE);
#endif
    region.base = reinterpret_cast<void*>(aligned);
    region.size = size;
    return region;
}
#endif

void UnmapRegion(Region region) {
#if defined(_WIN32)
    VirtualFree(region.base, 0, MEM_RELEASE);
#else
    munmap(region.base, region.size);
#endif
}

bool Probe() {
    Region region = MapRegion(1);
    if (!region) {
        return false;
    }
    // Run one instruction to see that the mapping really executes (a policy can allow the mapping and still fault).
    // x86-64: mov eax, 42; ret. Other hosts have no probe code, so they report unavailable.
#if defined(__x86_64__) || defined(_M_X64)
    static const uint8_t code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(region.base, code, sizeof(code));
    using Fn = int (*)();
    const int result = reinterpret_cast<Fn>(region.base)();
    UnmapRegion(region);
    return result == 42;
#else
    UnmapRegion(region);
    return false;
#endif
}

#endif  // MKW_JIT_MEMORY_HOST

}  // namespace

bool Available() {
#if MKW_JIT_MEMORY_HOST
    static const bool available = Probe();
    return available;
#else
    return false;
#endif
}

Region Allocate(size_t bytes) {
#if MKW_JIT_MEMORY_HOST
    return MapRegion(bytes);
#else
    (void)bytes;
    return Region{};
#endif
}

void Free(Region region) {
#if MKW_JIT_MEMORY_HOST
    if (region.base) {
        UnmapRegion(region);
    }
#else
    (void)region;
#endif
}

Arena::Arena(size_t chunkBytes) : chunkBytes_(chunkBytes ? chunkBytes : kDefaultChunkBytes) {}

Arena::~Arena() { Reset(); }

Arena::Arena(Arena&& other) noexcept
    : chunks_(std::move(other.chunks_)),
      chunkBytes_(other.chunkBytes_),
      offset_(other.offset_),
      used_(other.used_),
      reserved_(other.reserved_) {
    other.chunks_.clear();
    other.offset_ = other.used_ = other.reserved_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        Reset();
        chunks_ = std::move(other.chunks_);
        chunkBytes_ = other.chunkBytes_;
        offset_ = other.offset_;
        used_ = other.used_;
        reserved_ = other.reserved_;
        other.chunks_.clear();
        other.offset_ = other.used_ = other.reserved_ = 0;
    }
    return *this;
}

void* Arena::Allocate(size_t bytes, size_t align) {
    if (bytes == 0 || align == 0 || (align & (align - 1)) != 0) {
        return nullptr;
    }
    // Chunks start page-aligned, so alignment up to a page holds as an offset into one.
    if (align > 4096) {
        return nullptr;
    }
    if (!chunks_.empty()) {
        const Region& tail = chunks_.back();
        const size_t start = (offset_ + align - 1) & ~(align - 1);
        if (start <= tail.size && bytes <= tail.size - start) {
            offset_ = start + bytes;
            used_ += bytes;
            return static_cast<uint8_t*>(tail.base) + start;
        }
    }
    if (bytes > SIZE_MAX - align) {
        return nullptr;
    }
    const size_t want = bytes > chunkBytes_ ? bytes : chunkBytes_;
    Region chunk;
#if defined(__linux__)
    if (want >= kHugePageBytes) chunk = MapRegionHuge(want);
    if (!chunk)
#endif
        chunk = JitExecMemory::Allocate(want);
    if (!chunk) {
        return nullptr;
    }
    chunks_.push_back(chunk);
    reserved_ += chunk.size;
    // An oversized chunk is exact, so the chunk before it may still have room: keep filling that one.
    if (bytes > chunkBytes_ && chunks_.size() >= 2) {
        std::swap(chunks_[chunks_.size() - 1], chunks_[chunks_.size() - 2]);
        used_ += bytes;
        return chunks_[chunks_.size() - 2].base;
    }
    offset_ = bytes;
    used_ += bytes;
    return chunk.base;
}

bool Arena::Shrink(void* p, size_t oldBytes, size_t newBytes) {
    if (chunks_.empty() || p == nullptr || newBytes > oldBytes) {
        return false;
    }
    const Region& tail = chunks_.back();
    auto* const at = static_cast<uint8_t*>(p);
    auto* const base = static_cast<uint8_t*>(tail.base);
    if (at < base || at >= base + tail.size || static_cast<size_t>(at - base) + oldBytes != offset_) {
        return false;
    }
    offset_ -= oldBytes - newBytes;
    used_ -= oldBytes - newBytes;
    return true;
}

void Arena::Reset() {
    for (const Region& chunk : chunks_) {
        Free(chunk);
    }
    chunks_.clear();
    offset_ = 0;
    used_ = 0;
    reserved_ = 0;
}

}  // namespace JitExecMemory
