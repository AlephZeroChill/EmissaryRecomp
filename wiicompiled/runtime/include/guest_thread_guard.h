#pragma once

#include "runtime_log.h"

#include <atomic>
#include <cstdio>

// A guard for runtime state that only the guest CPU's host thread touches, in place of a std::mutex no other thread
// takes. Guest code and everything it calls runs on that one thread: the HLE entry points, guest threads (coroutines
// confined to it, host_context.cpp), alarms, retraces, DVD/NAND callbacks, save states (a helper coroutine), and
// Aurora's frame-wait callback (run by the waiting game thread, never the GX thread). Such a mutex protects nothing
// contended, yet each lock/unlock pays two atomic read-modify-writes, which also wait for the store buffer to drain:
// in P+ Training ~1,100 pairs a frame went to these locks, ~1.5% of the game thread (an all-thread trace of
// pthread_mutex_lock found no other thread on them). The guard keeps the lock sites as the record of what
// they protect and checks that claim instead of locking: the first thread to take it owns it, and any other thread is
// reported once, as the data race it would be. Lockable, so lock_guard and unique_lock work as with the mutex.
class GuestThreadGuard {
public:
    explicit constexpr GuestThreadGuard(const char* name) noexcept : m_name(name) {}
    GuestThreadGuard(const GuestThreadGuard&) = delete;
    GuestThreadGuard& operator=(const GuestThreadGuard&) = delete;

    void lock() noexcept {
        if (__builtin_expect(m_owner.load(std::memory_order_relaxed) != &t_tag, 0)) {
            Claim();
        }
    }
    bool try_lock() noexcept {
        lock();
        return true;
    }
    void unlock() noexcept {}

private:
    // Owns an unowned guard; on another thread, reports it once.
    __attribute__((noinline, cold)) void Claim() noexcept {
        const void* unowned = nullptr;
        if (m_owner.compare_exchange_strong(unowned, &t_tag, std::memory_order_relaxed)) {
            return;
        }
        if (!m_reported.exchange(true, std::memory_order_relaxed)) {
            RT_LOGF(RT_TAG_RUNTIME, "%s: taken on a second host thread; it guards guest-thread-only state without "
                                    "locking, so this is a data race\n", m_name);
        }
    }

    static inline thread_local char t_tag = 0;  // its address identifies the thread
    const char* m_name;
    std::atomic<const void*> m_owner{nullptr};
    std::atomic<bool> m_reported{false};
};
