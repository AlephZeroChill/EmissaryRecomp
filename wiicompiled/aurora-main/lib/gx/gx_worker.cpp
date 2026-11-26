#include "gx_worker.hpp"

#include "command_processor.hpp"
#include "fifo.hpp"
#include "gx.hpp"
#include "../gfx/common.hpp"
#include "../internal.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace aurora::gx::fifo {
// GXDispList.cpp: decode a display list's bytes, resuming after a staging split, as GXCallDisplayList does.
void process_display_list_bytes(const uint8_t* data, uint32_t nbytes, bool bigEndian);
// GXDispList.cpp: GXCallDisplayListCompiled's decode of a list's copied bytes (key: the list's own address).
void process_compiled_display_list_bytes(const uint8_t* data, uint32_t nbytes, const uint8_t* key, uint64_t digest);
} // namespace aurora::gx::fifo

namespace aurora::gx::worker {
struct Batch;
// A deferred job (gx_worker.hpp): the batches a draw worker hands over for one Job item of the producer's stream,
// run by the GX thread at that item as they come, until the worker finishes the job.
struct DeferredJob {
  std::mutex mutex;              // batches, produced, done
  std::condition_variable cv;    // a batch or the end (GX thread waits); a batch run (worker in sync waits)
  std::vector<Batch*> batches;   // handed over, not yet run, oldest first
  uint64_t produced = 0;         // batches handed over
  uint64_t consumed = 0;         // batches run
  bool done = false;
};
namespace {
enum class Kind : uint8_t { Fifo, DisplayList, RawDraw, Call, Bp, SourceVtxDesc, Job, CompiledList, StateList };
struct Item {
  Kind kind;
  bool bigEndian;
  uint8_t prim; // RawDraw
  uint8_t fmt;  // RawDraw
  uint16_t vtxCount; // RawDraw
  uint32_t offset; // Call: index into Batch::calls; Bp: the register word; SourceVtxDesc: attr << 8 | type;
                   // Job: index into Batch::jobs; CompiledList: index into Batch::compiled
  uint32_t size;
};
// A CompiledList item's list: its address (the compiled-list cache key), content digest and copied bytes.
struct CompiledListRef {
  const uint8_t* key;
  uint64_t digest;
  uint32_t offset; // into Batch::bytes
};
} // namespace
// One hand-off: the bytes of many drains and display lists, decoded in order.
struct Batch {
  std::vector<uint8_t> bytes;
  std::vector<Item> items;
  std::vector<std::function<void()>> calls;
  std::vector<DeferredJob*> jobs;
  std::vector<CompiledListRef> compiled;
  void clear() noexcept {
    bytes.clear();
    items.clear();
    calls.clear();
    jobs.clear();
    compiled.clear();
  }
};
namespace detail {
bool g_enabled = false;
AURORA_TLS_CONSTINIT thread_local bool t_onGx = false;
} // namespace detail
namespace {

using detail::g_enabled;
using detail::t_onGx;
std::thread g_thread;

// The hand-off: batches go from the producer to the GX thread through a single-producer single-consumer ring, and
// spent ones come back through another for reuse, with no lock on either side. The GX thread parks (an atomic wait on
// g_producer.wake) only when it finds the ring empty, and a producer wakes it only when it is parked (g_park.parked); a
// producer in sync() sleeps on g_doneCv only while it waits (g_producer.syncWaiting), and the GX thread signals it only
// then. Counters are monotonic: slot = count % size; g_gx.ringTail advances after its batch has run, so it is also the
// completion count.
constexpr uint64_t kRingSize = 256; // in flight; a full ring makes the producer wait (a frame hands over ~25)
constexpr uint64_t kFreeSize = 16;  // spent batches kept for reuse
std::array<Batch*, kRingSize> g_ring{};
std::array<Batch*, kFreeSize> g_freeRing{};
// The shared counters and flags, a cache line per writer, and the GX thread's park state, which changes far less often
// than its batch count, on a line of its own. Each side reads the other's on every hand-off or batch: on one line, each
// batch the GX thread finished made the producer's next flush() miss twice (perf: flush()'s own cycles).
struct alignas(64) ProducerShared {
  std::atomic<uint64_t> ringHead{0};    // batches handed over
  std::atomic<uint64_t> freeTail{0};    // spent batches reused
  std::atomic<uint32_t> wake{0};        // bumped to wake the GX thread
  std::atomic<uint32_t> syncWaiting{0}; // a producer sleeps on g_doneCv
};
struct alignas(64) GxShared {
  std::atomic<uint64_t> ringTail{0}; // batches finished
  std::atomic<uint64_t> freeHead{0}; // spent batches returned
};
struct alignas(64) GxParkState {
  std::atomic<uint32_t> parked{0}; // the GX thread is parked, or about to park, on g_producer.wake
  std::atomic<bool> idle{true};    // it has nothing to run (an idle GX thread gets work early: append)
};
ProducerShared g_producer; // written by the producer
GxShared g_gx;             // written by the GX thread
GxParkState g_park;        // written by the GX thread
uint64_t g_seenTail = 0;      // the producer's last look at g_gx.ringTail, a lower bound (producer only)
uint64_t g_seenFreeHead = 0;  // its last look at g_gx.freeHead, a lower bound (producer only)
std::atomic<bool> g_stop{false};
std::mutex g_doneMutex;           // only for g_doneCv
std::condition_variable g_doneCv; // a producer waiting for batches to finish (sync, a full ring)
std::atomic<uint64_t> g_syncWaitNanos{0}; // producer time blocked in sync() since the last take
std::atomic<uint64_t> g_sealWaitNanos{0}; // the GX thread's waits for the frame worker to seal (since the last log)
std::atomic<uint64_t> g_busyNanos{0};     // the GX thread's time running batches (since the last log)
// Of that, its waits at Job items for the draw worker (run_job), total and longest, since the last log.
std::atomic<uint64_t> g_jobWaitNanos{0};
std::atomic<uint64_t> g_jobWaitMaxNanos{0};
thread_local std::unique_ptr<Batch> g_open; // the batch being filled (each producer thread its own)
// A draw worker filling a deferred job: its batches go to the job, not the ring (attach_deferred_job).
thread_local DeferredJob* t_job = nullptr;
// Spent job batches, back from the GX thread for the draw worker to refill (one mutex: a few dozen a frame).
std::mutex g_jobPoolMutex;
std::vector<Batch*> g_jobPool;
// Spent deferred jobs, reset by the GX thread and back to the producer for its next begin_deferred_job, through a
// single-producer single-consumer ring as the free batches: no allocation or lock per job on the producer.
constexpr uint64_t kFreeJobs = 128;
std::array<DeferredJob*, kFreeJobs> g_freeJobs{};
std::atomic<uint64_t> g_freeJobsHead{0}; // returned (GX thread)
std::atomic<uint64_t> g_freeJobsTail{0}; // reused (producer)

// Large hand-offs keep wake-ups rare; an idle GX thread gets work early so it starts with the frame.
constexpr size_t kFlushBytes = 32 * 1024;
constexpr size_t kEarlyFlushBytes = 2 * 1024;

std::unique_ptr<Batch> new_batch() {
  auto batch = std::make_unique<Batch>();
  batch->bytes.reserve(256 * 1024);
  batch->items.reserve(1024);
  return batch;
}

// AURORA_GX_OLD_SYNC=1: syncs and the guest's GXFlush as before this (no inline sync, no GXFlush hand-off), for A/B runs
// of one binary. AURORA_GXFLUSH_HANDOFF=0: only the GXFlush hand-off off.
const bool g_oldSync = [] {
  const char* v = std::getenv("AURORA_GX_OLD_SYNC");
  return v != nullptr && v[0] == '1';
}();
const bool g_flushHandoff = !g_oldSync && [] {
  const char* v = std::getenv("AURORA_GXFLUSH_HANDOFF");
  return v == nullptr || v[0] != '0';
}();
uint64_t g_inlineSyncs = 0; // producer: syncs that decoded the open batch themselves (sync_inline), since the last log

// Producer: a spent batch to fill, or a new one.
std::unique_ptr<Batch> take_free() {
  if (t_job != nullptr) { // a draw worker: the job pool, not the producer's free ring (single producer)
    std::lock_guard lock(g_jobPoolMutex);
    if (!g_jobPool.empty()) {
      Batch* batch = g_jobPool.back();
      g_jobPool.pop_back();
      return std::unique_ptr<Batch>(batch);
    }
    return new_batch();
  }
  const uint64_t tail = g_producer.freeTail.load(std::memory_order_relaxed);
  if (tail == g_seenFreeHead)
    g_seenFreeHead = g_gx.freeHead.load(std::memory_order_acquire);
  if (tail != g_seenFreeHead) {
    Batch* batch = g_freeRing[tail % kFreeSize];
    g_producer.freeTail.store(tail + 1, std::memory_order_release);
    return std::unique_ptr<Batch>(batch);
  }
  return new_batch();
}

// GX thread: a spent batch back to the producer, or freed when the producer has enough.
void return_spent(Batch* batch) {
  const uint64_t head = g_gx.freeHead.load(std::memory_order_relaxed);
  if (head - g_producer.freeTail.load(std::memory_order_acquire) < kFreeSize) {
    g_freeRing[head % kFreeSize] = batch;
    g_gx.freeHead.store(head + 1, std::memory_order_release);
  } else {
    delete batch;
  }
}

// Producer: until the GX thread has finished `target` batches, or with kAllSubmitted every batch handed over, those
// handed over while it waits included (as sync() always did: guest code run by the timing pump below may submit).
// Keeps the producer's guest timing alive while it waits (the frame-worker wait callback every millisecond, as
// drain()'s frame-worker wait did); waitNanos gets the time.
constexpr uint64_t kAllSubmitted = UINT64_MAX;
void wait_for_completed(uint64_t target, uint64_t* waitNanos) {
  const auto reached = [target] {
    const uint64_t want = target == kAllSubmitted ? g_producer.ringHead.load(std::memory_order_relaxed) : target;
    return g_gx.ringTail.load(std::memory_order_seq_cst) >= want;
  };
  if (reached())
    return;
  std::unique_lock lock(g_doneMutex);
  g_producer.syncWaiting.store(1, std::memory_order_seq_cst);
  while (!reached()) {
    // Time blocked here is frame time on any host even though this thread burns no cycles: counted for
    // take_sync_wait_nanos (the guest timing pump below runs on this thread and is counted as its cycles instead).
    const auto waitStart = std::chrono::steady_clock::now();
    const bool done = g_doneCv.wait_for(lock, std::chrono::milliseconds(1), reached);
    const auto waited = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
    g_syncWaitNanos.fetch_add(waited, std::memory_order_relaxed);
    if (waitNanos != nullptr)
      *waitNanos += waited;
    if (!done) {
      // The producer's guest timing (alarms, retraces) must keep running however long it waits, as in drain().
      g_producer.syncWaiting.store(0, std::memory_order_relaxed);
      lock.unlock();
      aurora::run_frame_worker_wait_callback();
      lock.lock();
      g_producer.syncWaiting.store(1, std::memory_order_seq_cst);
    }
  }
  g_producer.syncWaiting.store(0, std::memory_order_relaxed);
}

void decode_fifo(const uint8_t* data, uint32_t size) {
  // fifo::drain's loop: process stops at a draw that no longer fits the staging batch; submit it and resume there.
  uint32_t consumed = 0;
  bool retried = false;
  while (consumed < size) {
    const auto count = fifo::process(data + consumed, size - consumed, true);
    if (count == 0 && retried)
      throw gfx::StagingCapacityError("FIFO draw does not fit after capacity submission");
    consumed += count;
    if (consumed == size)
      break;
    gfx::split_staging_batch();
    retried = true;
  }
}

// The renderer lock the GX thread holds across a batch (run_batch), null outside one.
thread_local std::unique_lock<std::recursive_mutex>* t_batchLock = nullptr;

void run_items(Batch& batch, std::unique_lock<std::recursive_mutex>& batchLock);

// GX thread, at a Job item: the job's batches in order as the draw worker hands them over, until it is done; then the
// job goes back to the producer for reuse (the worker no longer touches it once done). The renderer lock is dropped
// while waiting: the worker's producer code may take it.
void run_job(DeferredJob* job, std::unique_lock<std::recursive_mutex>& batchLock) {
  for (;;) {
    Batch* next = nullptr;
    {
      std::unique_lock lock(job->mutex);
      if (job->batches.empty() && !job->done) {
        if (batchLock.owns_lock())
          batchLock.unlock();
        const auto waitStart = std::chrono::steady_clock::now();
        job->cv.wait(lock, [job] { return !job->batches.empty() || job->done; });
        const auto waited = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
        g_jobWaitNanos.fetch_add(waited, std::memory_order_relaxed);
        if (waited > g_jobWaitMaxNanos.load(std::memory_order_relaxed))
          g_jobWaitMaxNanos.store(waited, std::memory_order_relaxed);
        if (waited > 20'000'000)
          std::fprintf(stderr, "[aurora] [gxthread] waited %.1f ms for a deferred draw job\n", waited / 1e6);
      }
      if (job->batches.empty())
        break; // done, nothing left
      next = job->batches.front();
      job->batches.erase(job->batches.begin());
    }
    if (!batchLock.owns_lock())
      batchLock.lock();
    run_items(*next, batchLock);
    next->clear();
    {
      std::lock_guard lock(g_jobPoolMutex);
      g_jobPool.push_back(next);
    }
    {
      std::lock_guard lock(job->mutex);
      ++job->consumed;
    }
    job->cv.notify_all();
  }
  // Done: nobody else touches it now. Reset for reuse, back to the producer (freed when its ring is full).
  job->produced = 0;
  job->consumed = 0;
  job->done = false;
  const uint64_t head = g_freeJobsHead.load(std::memory_order_relaxed);
  if (head - g_freeJobsTail.load(std::memory_order_acquire) < kFreeJobs) {
    g_freeJobs[head % kFreeJobs] = job;
    g_freeJobsHead.store(head + 1, std::memory_order_release);
  } else {
    delete job;
  }
}

void run_batch(Batch& batch) {
  // The renderer lock once per batch instead of once per process() call and raw draw (tens of thousands a frame in a
  // Brawl match; lock and unlock were ~15% of this thread's cycles): the decoders' own locks are then owner re-locks
  // of the recursive mutex. It is dropped where decoding waits on the frame worker, which needs it: the SEALED wait
  // here and gfx::split_staging_batch (BatchLockRelease).
  std::unique_lock batchLock(aurora::renderer_gpu_mutex(), std::defer_lock);
  t_batchLock = &batchLock;
  struct ClearBatchLock {
    ~ClearBatchLock() { t_batchLock = nullptr; }
  } clearBatchLock;
  run_items(batch, batchLock);
}

void run_items(Batch& batch, std::unique_lock<std::recursive_mutex>& batchLock) {
  for (const Item& item : batch.items) {
    if (item.kind == Kind::Job) {
      run_job(batch.jobs[item.offset], batchLock);
      continue;
    }
    // SEALED, not DONE, as fifo::drain waits before decoding: the previous frame's commands must be sealed.
    if (!aurora::frame_worker_sealed()) {
      if (batchLock.owns_lock())
        batchLock.unlock();
      g_sealWaitNanos.fetch_add(aurora::wait_for_frame_worker_sealed().count(), std::memory_order_relaxed);
    }
    if (!batchLock.owns_lock())
      batchLock.lock();
    const uint8_t* data = batch.bytes.data() + item.offset;
    try {
      if (item.kind == Kind::Call) {
        // Calls only touch decoded state and the renderer: the FIFO write buffer is the producer's alone.
        batch.calls[item.offset]();
      } else if (item.kind == Kind::Bp) {
        fifo::apply_bp_word(item.offset);
      } else if (item.kind == Kind::SourceVtxDesc) {
        g_gxState.sourceVtxDesc[item.offset >> 8] = static_cast<GXAttrType>(item.offset & 0xFFu);
      } else if (item.kind == Kind::Fifo) {
        decode_fifo(data, item.size);
      } else if (item.kind == Kind::DisplayList) {
        fifo::process_display_list_bytes(data, item.size, item.bigEndian);
      } else if (item.kind == Kind::CompiledList) {
        const CompiledListRef& list = batch.compiled[item.offset];
        fifo::process_compiled_display_list_bytes(batch.bytes.data() + list.offset, item.size, list.key, list.digest);
      } else if (item.kind == Kind::StateList) {
        fifo::apply_state_display_list(data, item.size);
      } else {
        fifo::submit_raw_draw_decoded(static_cast<GXPrimitive>(item.prim), static_cast<GXVtxFmt>(item.fmt), data,
                                      item.vtxCount, item.size);
      }
    } catch (const std::exception& e) {
      static int logged = 0;
      if (logged++ < 20)
        std::fprintf(stderr, "[aurora] [gxthread] decode failed: %s\n", e.what());
    }
  }
}

void thread_main() {
  t_onGx = true;
#if defined(__linux__)
  pthread_setname_np(pthread_self(), "aurora-gx");
#endif
  for (;;) {
    const uint64_t tail = g_gx.ringTail.load(std::memory_order_relaxed);
    if (tail == g_producer.ringHead.load(std::memory_order_acquire)) {
      if (g_stop.load(std::memory_order_acquire))
        return;
      // Park: announce it, then look again, so a producer that hands over after the look sees g_park.parked and wakes
      // us (both sides' store-then-load pairs are sequentially consistent).
      g_park.idle.store(true, std::memory_order_release);
      const uint32_t wake = g_producer.wake.load(std::memory_order_seq_cst);
      g_park.parked.store(1, std::memory_order_seq_cst);
      if (tail == g_producer.ringHead.load(std::memory_order_seq_cst) && !g_stop.load(std::memory_order_seq_cst))
        g_producer.wake.wait(wake, std::memory_order_seq_cst);
      g_park.parked.store(0, std::memory_order_relaxed);
      continue;
    }
    if (g_park.idle.load(std::memory_order_relaxed)) // a store only on change keeps the producer's copy of the line
      g_park.idle.store(false, std::memory_order_release);
    Batch* batch = g_ring[tail % kRingSize];
    const auto busyStart = std::chrono::steady_clock::now();
    run_batch(*batch);
    g_busyNanos.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - busyStart).count(),
        std::memory_order_relaxed);
    batch->clear();
    return_spent(batch);
    g_gx.ringTail.store(tail + 1, std::memory_order_seq_cst);
    if (g_producer.syncWaiting.load(std::memory_order_seq_cst) != 0) {
      { std::lock_guard lock(g_doneMutex); }  // the waiter is either before its check or inside wait_for
      g_doneCv.notify_all();
    }
  }
}

// Producer: open_batch calls (every item goes through it) and syncs, so far and at the last begin_deferred_job: equal
// means nothing entered the stream since that job's item (begin_deferred_job's followsJob).
thread_local uint64_t t_streamEvents = 0;
thread_local uint64_t t_streamEventsAtJob = ~uint64_t{0};

Batch& open_batch() {
  ++t_streamEvents;
  if (!g_open)
    g_open = take_free();
  return *g_open;
}

// After an item's bytes went into the open batch: large hand-offs keep wake-ups rare; an idle GX thread gets work
// early (kFlushBytes, kEarlyFlushBytes). The batch is the GX thread's once this flushes.
void flush_if_full(const Batch& batch) {
  if (batch.bytes.size() >= kFlushBytes ||
      (batch.bytes.size() >= kEarlyFlushBytes && g_park.idle.load(std::memory_order_acquire))) {
    flush();
  }
}

void append(Kind kind, const uint8_t* data, uint32_t size, bool bigEndian, uint8_t prim = 0, uint8_t fmt = 0,
            uint16_t vtxCount = 0) {
  if (size == 0)
    return;
  Batch& batch = open_batch();
  const auto offset = static_cast<uint32_t>(batch.bytes.size());
  batch.bytes.insert(batch.bytes.end(), data, data + size);
  Item* last = batch.items.empty() ? nullptr : &batch.items.back();
  if (kind == Kind::Fifo && last != nullptr && last->kind == Kind::Fifo && last->offset + last->size == offset) {
    last->size += size; // each drain ends on a command boundary, so FIFO bytes concatenate into one stream
  } else {
    batch.items.push_back({kind, bigEndian, prim, fmt, vtxCount, offset, size});
  }
  flush_if_full(batch);
}

// Per-frame counts for the [gxthread] log line (producer only).
uint64_t g_syncs = 0, g_flushes = 0, g_bytes = 0, g_frames = 0;
// Syncs by caller (the default argument is the calling function's name, a string literal).
struct SyncReason {
  const char* why;
  uint64_t count;
  uint64_t waitNanos;
};
std::array<SyncReason, 32> g_syncReasons{};

SyncReason* note_sync_reason(const char* why) {
  for (auto& reason : g_syncReasons) {
    if (reason.why == why || reason.why == nullptr) {
      reason.why = why;
      ++reason.count;
      return &reason;
    }
  }
  return nullptr;
}
} // namespace

bool enabled() noexcept { return g_enabled; }
bool on_gx_thread() noexcept { return t_onGx; }
bool holds_batch_lock() noexcept { return t_batchLock != nullptr && t_batchLock->owns_lock(); }

BatchLockRelease::BatchLockRelease() noexcept {
  if (t_batchLock != nullptr && t_batchLock->owns_lock()) {
    t_batchLock->unlock();
    m_released = true;
  }
}
BatchLockRelease::~BatchLockRelease() {
  if (m_released)
    t_batchLock->lock();
}

void start() {
  if (g_enabled)
    return;
  // On by default (AURORA_GX_THREAD=0: decode on the producer). Pixel-identical to the producer-thread decode in
  // deterministic replays of vanilla Training; critical-path p99 -9% there.
  const char* env = std::getenv("AURORA_GX_THREAD");
  if (env != nullptr && std::strcmp(env, "0") == 0)
    return;
  g_thread = std::thread(thread_main);
  g_enabled = true;
  std::fprintf(stderr, "[aurora] [gxthread] GX decoding runs on its own thread\n");
}

void stop() noexcept {
  if (!g_enabled)
    return;
  sync();
  g_stop.store(true, std::memory_order_seq_cst);
  g_producer.wake.fetch_add(1, std::memory_order_seq_cst);
  g_producer.wake.notify_one();
  if (g_thread.joinable())
    g_thread.join();
  g_enabled = false;
}

void submit_fifo(const uint8_t* data, uint32_t size) { append(Kind::Fifo, data, size, true); }

void submit_display_list(const uint8_t* data, uint32_t size, bool bigEndian) {
  append(Kind::DisplayList, data, size, bigEndian);
}

void submit_compiled_display_list(const uint8_t* data, uint32_t size, uint64_t digest) {
  if (size == 0)
    return;
  // append's copy, with the item pointing at its CompiledListRef (which holds the bytes' offset), then its flush
  Batch& batch = open_batch();
  const auto offset = static_cast<uint32_t>(batch.bytes.size());
  batch.bytes.insert(batch.bytes.end(), data, data + size);
  batch.items.push_back({Kind::CompiledList, true, 0, 0, 0, static_cast<uint32_t>(batch.compiled.size()), size});
  batch.compiled.push_back({data, digest, offset});
  flush_if_full(batch);
}

void submit_state_display_list(const uint8_t* data, uint32_t size) { append(Kind::StateList, data, size, true); }

void submit_raw_draw(uint8_t prim, uint8_t fmt, const uint8_t* vertices, uint16_t vtxCount, uint32_t vertexBytes) {
  append(Kind::RawDraw, vertices, vertexBytes, true, prim, fmt, vtxCount);
}

void submit_call(std::function<void()> fn) {
  fifo::drain(); // the producer's pending FIFO bytes go first
  Batch& batch = open_batch();
  batch.items.push_back({Kind::Call, false, 0, 0, 0, static_cast<uint32_t>(batch.calls.size()), 0});
  batch.calls.push_back(std::move(fn));
}

void submit_bp(uint32_t word) {
  // No drain: inline, GXApplyBPReg decodes the register at once, ahead of FIFO bytes not yet drained, so it goes
  // after everything drained so far and before those.
  Batch& batch = open_batch();
  batch.items.push_back({Kind::Bp, false, 0, 0, 0, word, 0});
}

void submit_source_vtx_desc(uint8_t attr, uint8_t type) {
  // No drain, as submit_bp: inline, GXSetSourceVtxDesc writes decoded state at once.
  Batch& batch = open_batch();
  batch.items.push_back({Kind::SourceVtxDesc, false, 0, 0, 0, static_cast<uint32_t>(attr) << 8 | type, 0});
}

void flush() noexcept {
  if (!g_open || g_open->items.empty())
    return;
  if (t_job != nullptr) { // a draw worker: to its job
    {
      std::lock_guard lock(t_job->mutex);
      t_job->batches.push_back(g_open.release());
      ++t_job->produced;
    }
    t_job->cv.notify_all();
    return;
  }
  g_bytes += g_open->bytes.size();
  ++g_flushes;
  const uint64_t head = g_producer.ringHead.load(std::memory_order_relaxed);
  if (head - g_seenTail >= kRingSize) UNLIKELY {
    g_seenTail = g_gx.ringTail.load(std::memory_order_acquire);
    if (head - g_seenTail >= kRingSize) {
      wait_for_completed(head - kRingSize + 1, nullptr); // a full ring: the GX thread is that far behind
      g_seenTail = g_gx.ringTail.load(std::memory_order_acquire);
    }
  }
  g_ring[head % kRingSize] = g_open.release();
  g_producer.ringHead.store(head + 1, std::memory_order_seq_cst);
  if (g_park.parked.load(std::memory_order_seq_cst) != 0) {
    g_producer.wake.fetch_add(1, std::memory_order_seq_cst);
    g_producer.wake.notify_one();
  }
  // AURORA_GX_SYNC_EVERY=1 (diagnostic): wait for every hand-off, so decoding still runs on the GX thread but never
  // overlaps the producer. Separates "decoding on another thread" problems from ordering races.
  static const bool syncEvery = [] {
    const char* v = std::getenv("AURORA_GX_SYNC_EVERY");
    return v != nullptr && v[0] == '1';
  }();
  static thread_local bool t_inSyncEvery = false;
  if (syncEvery && !t_inSyncEvery) {
    t_inSyncEvery = true;
    sync("sync_every");  // its own flush() finds nothing open; it waits like any sync, servicing guest timing
    t_inSyncEvery = false;
  }
}

// A sync that finds the GX thread with nothing queued and a small open batch decodes that batch here: handing it over
// would cost a wake-up of the parked GX thread and then of this one, ~15-25 us on a desktop host. Both draw-dones of a P+
// Training frame found just that, a few FIFO bytes with the GX thread idle. Same code, same order, so a
// draw-done still returns only after everything submitted before it was decoded. The ring keeps the two threads apart:
// the GX thread decodes only batches it takes from the ring, and none is handed over meanwhile; the acquire load that
// finds the ring empty orders the GX thread's decoding before this one, and the next hand-off's release orders this one
// before its next batch. Not with a deferred job in the batch: the GX thread runs jobs (run_job, the job pools).
constexpr size_t kInlineSyncBytes = 8 * 1024;
static bool sync_inline() noexcept {
  if (g_oldSync || t_job != nullptr || !g_open || g_open->items.empty() || g_open->bytes.size() > kInlineSyncBytes)
    return false;
  for (const Item& item : g_open->items)
    if (item.kind == Kind::Job)
      return false;
  if (g_gx.ringTail.load(std::memory_order_acquire) != g_producer.ringHead.load(std::memory_order_relaxed))
    return false;
  t_onGx = true; // decoding: the API calls a batch makes run in place, as on the GX thread
  run_batch(*g_open);
  t_onGx = false;
  g_open->clear();
  ++g_inlineSyncs;
  return true;
}

void sync(const char* why) noexcept {
  if (!producer())
    return;
  if (t_job != nullptr) {
    // A draw worker: everything it handed over run (the GX thread runs the job at its Job item, after everything the
    // producer submitted before it). No guest timing pump: this is not the guest's thread.
    fifo::drain();
    flush();
    std::unique_lock lock(t_job->mutex);
    t_job->cv.wait(lock, [] { return t_job->consumed == t_job->produced; });
    return;
  }
  ++g_syncs;
  ++t_streamEvents;  // frame boundaries and readbacks sync: never count a job after one as following the previous
  SyncReason* reason = note_sync_reason(why);
  if (sync_inline())
    return;
  flush();
  wait_for_completed(kAllSubmitted, reason != nullptr ? &reason->waitNanos : nullptr);
}

void flush_point() noexcept {
  if (!g_flushHandoff || !producer() || t_job != nullptr)
    return;
  fifo::drain();
  flush();
}

uint64_t take_sync_wait_nanos() noexcept { return g_syncWaitNanos.exchange(0, std::memory_order_relaxed); }

DeferredJob* begin_deferred_job(bool* followsJob) {
  if (!producer() || t_job != nullptr)
    return nullptr;
  fifo::drain(); // the producer's pending FIFO bytes go first
  if (followsJob != nullptr)
    *followsJob = t_streamEvents == t_streamEventsAtJob;
  DeferredJob* job;
  const uint64_t tail = g_freeJobsTail.load(std::memory_order_relaxed);
  if (tail != g_freeJobsHead.load(std::memory_order_acquire)) {
    job = g_freeJobs[tail % kFreeJobs];
    g_freeJobsTail.store(tail + 1, std::memory_order_release);
  } else {
    job = new DeferredJob();
  }
  Batch& batch = open_batch();
  batch.items.push_back({Kind::Job, false, 0, 0, 0, static_cast<uint32_t>(batch.jobs.size()), 0});
  batch.jobs.push_back(job);
  t_streamEventsAtJob = t_streamEvents;
  return job;
}

void attach_deferred_job(DeferredJob* job) noexcept { t_job = job; }

void finish_deferred_job() noexcept {
  DeferredJob* job = t_job;
  if (job == nullptr)
    return;
  fifo::drain();
  flush();
  t_job = nullptr;
  // Notified under the lock: once it is released the GX thread may free the job, so nothing touches it after.
  std::lock_guard lock(job->mutex);
  job->done = true;
  job->cv.notify_all();
}

void note_frame() noexcept {
  if (!g_enabled || ++g_frames % 600 != 0)
    return;
  std::string reasons;
  for (auto& reason : g_syncReasons) {
    if (reason.why != nullptr && reason.count != 0) {
      char part[128];
      std::snprintf(part, sizeof(part), " %s %.1f (%.3f ms)", reason.why, reason.count / 600.0,
                    reason.waitNanos / 600.0 / 1e6);
      reasons += part;
    }
    reason.count = 0;
    reason.waitNanos = 0;
  }
  std::fprintf(stderr,
               "[aurora] [gxthread] per frame: %.1f syncs (%.1f decoded in place), %.1f hand-offs, %.1f KB; GX thread "
               "busy %.3f ms (of it waiting for draw jobs %.3f ms, longest %.3f ms), waiting for seal %.3f ms; syncs "
               "by caller (wait):%s\n",
               g_syncs / 600.0, g_inlineSyncs / 600.0, g_flushes / 600.0, g_bytes / 600.0 / 1024.0,
               g_busyNanos.exchange(0) / 600.0 / 1e6, g_jobWaitNanos.exchange(0) / 600.0 / 1e6,
               g_jobWaitMaxNanos.exchange(0) / 1e6, g_sealWaitNanos.exchange(0) / 600.0 / 1e6, reasons.c_str());
  g_syncs = g_flushes = g_bytes = g_inlineSyncs = 0;
}

} // namespace aurora::gx::worker
