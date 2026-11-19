// insn_count.cpp - per-call user-instruction counts (NAT_INSN=1); see insn_count.h.

#include "insn_count.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__linux__) && defined(__x86_64__)
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#define NAT_INSN_COUNTER 1
#else
#define NAT_INSN_COUNTER 0
#endif

namespace InsnCount {

bool EnvOn() {
    const char* value = std::getenv("NAT_INSN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

namespace {

constexpr int kMaxSites = 128;
constexpr int kNameBytes = 48;

struct SiteCounts {
    char name[kNameBytes] = {};
    std::atomic<uint64_t> calls[2]{};
    std::atomic<uint64_t> instructions[2]{};
};
SiteCounts g_sites[kMaxSites];
std::atomic<int> g_siteCount{0};
std::mutex g_siteMutex;
std::atomic<int> g_openErrno{0};  // the first failed perf_event_open's errno, reported once

#if NAT_INSN_COUNTER
// One counter per thread: retired instructions in user mode, this thread only (no inherit), pinned so it is never
// multiplexed away. rdpmc reads it without a syscall when the kernel allows (cap_user_rdpmc), else read().
struct ThreadCounter {
    int fd = -1;
    perf_event_mmap_page* page = nullptr;
    bool tried = false;
    uint64_t overhead = 0;  // the instructions two back-to-back Read() calls count between their counter reads
};
thread_local ThreadCounter t_counter;

inline uint64_t Rdpmc(uint32_t counter) {
    uint32_t lo, hi;
    // lfence: the read neither runs ahead of the instructions before it nor lets later ones run ahead of it.
    asm volatile("lfence\n\trdpmc\n\tlfence" : "=a"(lo), "=d"(hi) : "c"(counter) : "memory");
    return (uint64_t{hi} << 32) | lo;
}

uint64_t ReadCounter(const ThreadCounter& c) {
    const volatile perf_event_mmap_page* const pc = c.page;
    for (;;) {
        const uint32_t seq = pc->lock;
        asm volatile("" ::: "memory");
        const uint32_t index = pc->index;
        const int64_t offset = pc->offset;
        uint64_t count;
        if (pc->cap_user_rdpmc && index != 0) {
            const uint32_t width = pc->pmc_width;
            uint64_t raw = Rdpmc(index - 1);
            raw <<= 64 - width;
            count = static_cast<uint64_t>(offset + (static_cast<int64_t>(raw) >> (64 - width)));
        } else {
            uint64_t value = 0;
            if (read(c.fd, &value, sizeof(value)) != static_cast<ssize_t>(sizeof(value))) return 0;
            return value;
        }
        asm volatile("" ::: "memory");
        if (pc->lock == seq) return count;
    }
}

void Open(ThreadCounter& c) {
    c.tried = true;
    perf_event_attr attr{};
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.pinned = 1;
    const int fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
    if (fd < 0) {
        int expected = 0;
        g_openErrno.compare_exchange_strong(expected, errno == 0 ? -1 : errno);
        return;
    }
    void* const page = mmap(nullptr, static_cast<size_t>(sysconf(_SC_PAGESIZE)), PROT_READ, MAP_SHARED, fd, 0);
    if (page == MAP_FAILED) {
        int expected = 0;
        g_openErrno.compare_exchange_strong(expected, errno == 0 ? -1 : errno);
        close(fd);
        return;
    }
    c.fd = fd;
    c.page = static_cast<perf_event_mmap_page*>(page);
}
#endif

}  // namespace

// Out of line (and kept so), so a measurement's two reads run the same code as the calibration's: the overhead
// subtracted below is two back-to-back calls of this very function, as callers make them.
[[gnu::noinline]] uint64_t Read() {
#if NAT_INSN_COUNTER
    ThreadCounter& c = t_counter;
    if (!c.tried) [[unlikely]] {
        Open(c);
        if (c.page != nullptr) {  // the read path's own cost, through this function (c.tried is set now)
            uint64_t best = UINT64_MAX;
            for (int i = 0; i < 256; ++i) {
                const uint64_t a = Read();
                const uint64_t b = Read();
                best = std::min(best, b - a);
            }
            c.overhead = best;
        }
    }
    return c.page != nullptr ? ReadCounter(c) : 0;
#else
    return 0;
#endif
}

uint64_t Since(uint64_t start) {
    const uint64_t now = Read();
#if NAT_INSN_COUNTER
    const uint64_t delta = now - start;
    return delta > t_counter.overhead ? delta - t_counter.overhead : 0;
#else
    (void)start;
    return now;
#endif
}

int Site(const char* name) {
    std::lock_guard<std::mutex> lock(g_siteMutex);
    const int count = g_siteCount.load(std::memory_order_relaxed);
    for (int i = 0; i < count; ++i)
        if (std::strncmp(g_sites[i].name, name, kNameBytes - 1) == 0) return i;
    if (count == kMaxSites) return -1;
    std::snprintf(g_sites[count].name, kNameBytes, "%s", name);
    g_siteCount.store(count + 1, std::memory_order_release);
    return count;
}

void Add(int site, Column column, uint64_t instructions) {
    if (site < 0) return;
    g_sites[site].calls[column].fetch_add(1, std::memory_order_relaxed);
    g_sites[site].instructions[column].fetch_add(instructions, std::memory_order_relaxed);
}

void Report(double frames) {
    if (!On() || frames <= 0) return;
    static std::atomic<bool> warned{false};
    if (const int error = g_openErrno.load(std::memory_order_relaxed); error != 0 && !warned.exchange(true))
        std::fprintf(stderr, "[insn] counter unavailable on some thread (perf_event_open: %s); its counts are 0. "
                     "Start the game with CAP_PERFMON (scripts/WiiCompiled-insn).\n",
                     error > 0 ? std::strerror(error) : "mmap failed");
    std::fprintf(stderr, "[insn] window frames=%.0f\n", frames);  // every window, so a run's frames add up
    const int count = g_siteCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        SiteCounts& s = g_sites[i];
        uint64_t calls[2], insns[2];
        for (int k = 0; k < 2; ++k) {
            calls[k] = s.calls[k].exchange(0, std::memory_order_relaxed);
            insns[k] = s.instructions[k].exchange(0, std::memory_order_relaxed);
        }
        if (calls[0] == 0 && calls[1] == 0) continue;
        const auto perCall = [](uint64_t n, uint64_t c) { return c != 0 ? static_cast<double>(n) / c : 0.0; };
        // Raw sums first (insn-sum.py adds them up over a run), then the window's readable form. The saving is per
        // frame over the window and only meaningful when both columns counted the same calls (a verify mode).
        std::fprintf(stderr,
                     "[insn] frames=%.0f site=%s calls=%llu,%llu insns=%llu,%llu | %.2f calls/frame, original %.0f "
                     "native %.0f per call, saving %.4fM/frame\n",
                     frames, s.name, static_cast<unsigned long long>(calls[0]),
                     static_cast<unsigned long long>(calls[1]), static_cast<unsigned long long>(insns[0]),
                     static_cast<unsigned long long>(insns[1]), static_cast<double>(std::max(calls[0], calls[1])) / frames,
                     perCall(insns[0], calls[0]), perCall(insns[1], calls[1]),
                     calls[0] == calls[1] ? (static_cast<double>(insns[0]) - static_cast<double>(insns[1])) / frames / 1e6
                                          : 0.0);
    }
}

}  // namespace InsnCount
