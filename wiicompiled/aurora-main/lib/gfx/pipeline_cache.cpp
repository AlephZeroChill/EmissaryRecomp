#include "pipeline_cache.hpp"

#include "clear.hpp"
#include "../gx/pipeline.hpp"
#include "../fs_helper.hpp"
#include "../gpu/gpu.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/format.h>
#include <aurora/profiling.hpp>

namespace aurora::gfx {
static Module Log("aurora::gfx::pipeline_cache");

constexpr uint32_t PipelineCacheSchema = 2;

struct CachedPipeline {
  gpu::GraphicsPipeline pipeline;
  uint32_t firstFrameUsed = UINT32_MAX;
};

struct PendingPipeline {
  PipelineRef hash;
  uint32_t firstFrameUsed = UINT32_MAX;
  NewPipelineCallback create;
};

struct PipelineCacheWrite {
  ShaderType type;
  PipelineRef hash;
  uint32_t configVersion;
  ByteBuffer config;
  uint32_t firstFrameUsed = UINT32_MAX;
};

static std::mutex g_pipelineMutex;
static bool g_hasPipelineThread = false;
static bool g_pipelineFrameActive = false;
static std::atomic_bool g_skipUnreadyGxPipelines = false;
static size_t g_pipelinesPerFrame = 0;
// Keep first-use compilation bounded. The render command stream remains ordered;
// it waits for a queued pipeline only when the corresponding draw is consumed.
constexpr size_t MaxQueuedPipelineBuilds = 256;
// First-use compilation works best as a short parallel burst. Leave two logical processors for the
// render and game threads, and cap large hosts to limit driver submissions and memory use.
constexpr size_t ReservedLogicalProcessors = 2;
constexpr size_t MaxPipelineWorkers = 22;
// Cached clear and GX pipelines are prewarmed using the full worker pool.
constexpr size_t MaxBackgroundPipelineWorkers = MaxPipelineWorkers;
// For synchronous pipeline fallback (OpenGL)
#ifdef NDEBUG
constexpr size_t BuildPipelinesPerFrame = 5;
#else
constexpr size_t BuildPipelinesPerFrame = 1;
#endif
static std::vector<std::thread> g_pipelineThreads;
static bool g_pipelineThreadEnd = false;
static size_t g_activeBackgroundPipelineWorkers = 0;
static std::condition_variable g_pipelineCv;
static absl::flat_hash_map<PipelineRef, CachedPipeline> g_pipelines;
static std::deque<PendingPipeline> g_priorityPipelines;
static std::deque<PendingPipeline> g_backgroundPipelines;
static absl::flat_hash_set<PipelineRef> g_pendingPipelines;

static std::FILE* g_pipelineCacheFile = nullptr;
static bool g_pipelineCacheBroken = false;
static std::thread g_pipelineCacheWriterThread;
static std::condition_variable g_pipelineCacheWriterCv;
static std::mutex g_pipelineCacheWriterMutex;
static std::deque<PipelineCacheWrite> g_pipelineCacheWriteQueue;
static absl::flat_hash_set<PipelineRef> g_pipelineCachePendingWrites;
static bool g_pipelineCacheWriterStop = false;

#if defined(__cpp_lib_atomic_ref)
static std::atomic_ref queuedPipelines{g_stats.queuedPipelines};
static std::atomic_ref createdPipelines{g_stats.createdPipelines};
#else
struct AtomicStatRef {
  uint32_t& ref;
  void operator++() { __atomic_fetch_add(&ref, 1, __ATOMIC_RELAXED); }
  void operator--() { __atomic_fetch_sub(&ref, 1, __ATOMIC_RELAXED); }
  void operator++(int) { __atomic_fetch_add(&ref, 1, __ATOMIC_RELAXED); }
  void operator--(int) { __atomic_fetch_sub(&ref, 1, __ATOMIC_RELAXED); }
  void operator=(uint32_t val) { __atomic_store_n(&ref, val, __ATOMIC_RELAXED); }
  uint32_t load() const { return __atomic_load_n(&ref, __ATOMIC_RELAXED); }
};
static AtomicStatRef queuedPipelines{g_stats.queuedPipelines};
static AtomicStatRef createdPipelines{g_stats.createdPipelines};
#endif

template <typename PipelineConfig>
static PipelineCacheWrite make_pipeline_cache_write(ShaderType type, PipelineRef hash, const PipelineConfig& config,
                                                    uint32_t firstFrameUsed) {
  static_assert(std::has_unique_object_representations_v<PipelineConfig>);

  PipelineCacheWrite write{
      .type = type,
      .hash = hash,
      .configVersion = config.version,
      .config = ByteBuffer(sizeof(config)),
      .firstFrameUsed = firstFrameUsed,
  };
  std::memcpy(write.config.data(), &config, sizeof(config));
  return write;
}

static void enqueue_pipeline_cache_write(PipelineCacheWrite write) {
  if (g_pipelineCacheBroken || g_pipelineCacheFile == nullptr) {
    return;
  }

  {
    std::lock_guard lock{g_pipelineCacheWriterMutex};
    if (!g_pipelineCachePendingWrites.insert(write.hash).second) {
      return;
    }
    g_pipelineCacheWriteQueue.emplace_back(std::move(write));
  }
  g_pipelineCacheWriterCv.notify_one();
}

template <typename Queue>
static auto find_pending_pipeline(Queue& queue, PipelineRef hash) {
  return std::find_if(queue.begin(), queue.end(), [=](const PendingPipeline& pending) { return pending.hash == hash; });
}

static PendingPipeline* touch_pending_pipeline(PipelineRef hash, bool prioritize) {
  auto priorityIt = find_pending_pipeline(g_priorityPipelines, hash);
  if (priorityIt != g_priorityPipelines.end()) {
    return &*priorityIt;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelines, hash);
  if (backgroundIt == g_backgroundPipelines.end()) {
    return nullptr;
  }

  if (!prioritize) {
    return &*backgroundIt;
  }

  g_priorityPipelines.emplace_back(std::move(*backgroundIt));
  g_backgroundPipelines.erase(backgroundIt);
  g_pipelineCv.notify_all();
  return &g_priorityPipelines.back();
}

// A persistent resolve is the last chance to complete its producing draw, so promote its queued
// build ahead of first-use work. One already taken by a worker is compiling and has no entry.
static bool promote_pending_pipeline_for_wait(PipelineRef hash) {
  auto priorityIt = find_pending_pipeline(g_priorityPipelines, hash);
  if (priorityIt != g_priorityPipelines.end()) {
    if (priorityIt != g_priorityPipelines.begin()) {
      PendingPipeline pending = std::move(*priorityIt);
      g_priorityPipelines.erase(priorityIt);
      g_priorityPipelines.emplace_front(std::move(pending));
    }
    return true;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelines, hash);
  if (backgroundIt == g_backgroundPipelines.end()) {
    return false;
  }
  PendingPipeline pending = std::move(*backgroundIt);
  g_backgroundPipelines.erase(backgroundIt);
  g_priorityPipelines.emplace_front(std::move(pending));
  return true;
}

template <typename Queue>
static bool remove_pending_pipeline(Queue& queue, PipelineRef hash) {
  const auto it = find_pending_pipeline(queue, hash);
  if (it == queue.end()) {
    return false;
  }
  queue.erase(it);
  return true;
}

template <typename PipelineConfig>
static PipelineRef find_pipeline_impl(ShaderType type, const PipelineConfig& config, NewPipelineCallback&& cb,
                                      bool persist, std::optional<uint32_t> firstFrameUsedOverride) {
  ZoneScoped;

  const PipelineRef hash = xxh3_hash(config, static_cast<HashType>(type));
  const uint32_t firstFrameUsed = firstFrameUsedOverride.value_or(current_frame());
  const bool cachePreload = firstFrameUsedOverride.has_value();
  bool notifyWorker = false;
  bool syncCreate = false;
  bool removedPending = false;
  bool notifyWaiters = false;
  std::optional<PipelineCacheWrite> cacheWrite;
  {
    std::scoped_lock guard{g_pipelineMutex};
    const bool deferGxPipeline = g_hasPipelineThread && g_pipelineFrameActive && type == ShaderType::GX;
    const bool skipUnreadyGxPipeline = deferGxPipeline && g_skipUnreadyGxPipelines.load(std::memory_order_relaxed);
    auto pipelineIt = g_pipelines.find(hash);
    if (pipelineIt != g_pipelines.end()) {
      if (persist && firstFrameUsed < pipelineIt->second.firstFrameUsed) {
        pipelineIt->second.firstFrameUsed = firstFrameUsed;
        cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
      }
    } else if (g_pendingPipelines.contains(hash)) {
      auto* pending = touch_pending_pipeline(hash, g_pipelineFrameActive);
      if (pending != nullptr && firstFrameUsed < pending->firstFrameUsed) {
        pending->firstFrameUsed = firstFrameUsed;
        if (persist) {
          cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
        }
      }
      if (g_pipelineFrameActive && !deferGxPipeline) {
        syncCreate = true;
      }
    } else {
      if (g_pipelineFrameActive && !deferGxPipeline) {
        syncCreate = true;
      } else {
        if (!cachePreload && deferGxPipeline && g_pendingPipelines.size() >= MaxQueuedPipelineBuilds &&
            !g_backgroundPipelines.empty()) {
          g_pendingPipelines.erase(g_backgroundPipelines.back().hash);
          g_backgroundPipelines.pop_back();
          --queuedPipelines;
        }

        // Keep the pipeline queued past the cap rather than compiling synchronously: the recorded draw
        // references this hash forever, and dropping it baked one-shot EFB copies incomplete.
        if (!cachePreload && g_pendingPipelines.size() >= MaxQueuedPipelineBuilds && !skipUnreadyGxPipeline) {
          syncCreate = true;
        } else {
          auto& targetQueue = deferGxPipeline ? g_priorityPipelines : g_backgroundPipelines;
          targetQueue.emplace_back(PendingPipeline{
              .hash = hash,
              .firstFrameUsed = firstFrameUsed,
              .create = std::move(cb),
          });
          g_pendingPipelines.insert(hash);
          ++queuedPipelines;
          if (persist) {
            cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
          }
          notifyWorker = true;
        }
      }
    }
  }

  if (syncCreate) {
    const auto pipeline = cb();
    {
      std::scoped_lock guard{g_pipelineMutex};
      removedPending = remove_pending_pipeline(g_priorityPipelines, hash);
      removedPending = remove_pending_pipeline(g_backgroundPipelines, hash) || removedPending;
      if (removedPending) {
        g_pendingPipelines.erase(hash);
      }
      auto [it, inserted] = g_pipelines.try_emplace(hash, CachedPipeline{
                                                              .pipeline = pipeline,
                                                              .firstFrameUsed = firstFrameUsed,
                                                          });
      if (!inserted && persist && firstFrameUsed < it->second.firstFrameUsed) {
        it->second.firstFrameUsed = firstFrameUsed;
      }
      if (inserted) {
        ++createdPipelines;
      }
      if (persist) {
        cacheWrite = make_pipeline_cache_write(type, hash, config, firstFrameUsed);
      }
      notifyWaiters = true;
    }
  }

  if (cacheWrite) {
    enqueue_pipeline_cache_write(std::move(*cacheWrite));
  }

  if (notifyWorker) {
    // Compiler workers and renderer waiters share this condition variable.
    g_pipelineCv.notify_all();
  }
  if (notifyWaiters) {
    g_pipelineCv.notify_all();
  }
  if (removedPending) {
    --queuedPipelines;
  }

  return hash;
}

// The pipeline cache is a flat file of the configurations every pipeline was built from, so the next launch can
// compile them before they are drawn. A header, then records appended as pipelines are first used; duplicates
// keep the earliest frame. Boot rewrites it compacted, in place: no rename (Horizon cannot rename over a file),
// no locking, no memory mapping. A record torn by a crash mid-append ends the read; each config also carries
// its content hash, checked on load.
namespace {
constexpr char PipelineCacheMagic[4] = {'A', 'P', 'C', 'F'};
constexpr const char* PipelineCacheFileName = "pipeline_cache.bin";
constexpr uint32_t MaxPipelineConfigSize = 64 * 1024;
struct PipelineCacheFileHeader {
  char magic[4];
  uint32_t schema;
  uint32_t reserved[2];
};
struct PipelineCacheRecordHeader {
  uint32_t type;
  uint32_t configVersion;
  uint64_t hash;
  uint32_t firstFrameUsed;
  uint32_t size;
};
} // namespace

static void pipeline_cache_abort() {
  g_pipelineCacheBroken = true;
  if (g_pipelineCacheFile != nullptr) {
    std::fclose(g_pipelineCacheFile);
    g_pipelineCacheFile = nullptr;
  }
}

static bool write_pipeline_cache_record(std::FILE* file, const PipelineCacheWrite& write) {
  const PipelineCacheRecordHeader header{
      .type = underlying(write.type),
      .configVersion = write.configVersion,
      .hash = static_cast<uint64_t>(write.hash),
      .firstFrameUsed = write.firstFrameUsed,
      .size = static_cast<uint32_t>(write.config.size()),
  };
  return std::fwrite(&header, sizeof(header), 1, file) == 1 &&
         (write.config.size() == 0 || std::fwrite(write.config.data(), write.config.size(), 1, file) == 1);
}

static std::vector<PipelineCacheWrite> read_pipeline_cache_file(const std::string& path) {
  std::vector<PipelineCacheWrite> records;
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return records;
  }
  PipelineCacheFileHeader fileHeader{};
  if (std::fread(&fileHeader, sizeof(fileHeader), 1, file) != 1 ||
      std::memcmp(fileHeader.magic, PipelineCacheMagic, sizeof(PipelineCacheMagic)) != 0 ||
      fileHeader.schema != PipelineCacheSchema) {
    std::fclose(file);
    return records;
  }
  constexpr auto MaxShaderTypeValue = std::numeric_limits<std::underlying_type_t<ShaderType>>::max();
  PipelineCacheRecordHeader header{};
  while (std::fread(&header, sizeof(header), 1, file) == 1) {
    if (header.type > MaxShaderTypeValue || header.size > MaxPipelineConfigSize) {
      break;
    }
    PipelineCacheWrite record{
        .type = static_cast<ShaderType>(header.type),
        .hash = static_cast<PipelineRef>(header.hash),
        .configVersion = header.configVersion,
        .config = ByteBuffer(header.size),
        .firstFrameUsed = header.firstFrameUsed,
    };
    if (header.size != 0 && std::fread(record.config.data(), header.size, 1, file) != 1) {
      break;
    }
    records.emplace_back(std::move(record));
  }
  std::fclose(file);
  return records;
}

static bool current_pipeline_config_version(ShaderType type, uint32_t version) {
  switch (type) {
  case ShaderType::Clear:
    return version == clear::ClearPipelineConfigVersion;
  case ShaderType::GX:
    return version == gx::GXPipelineConfigVersion;
  }
  return false;
}

// Loaded records for the boot prewarm, earliest-used first; dropped once queued.
static std::vector<PipelineCacheWrite> g_loadedPipelineRecords;

static bool prepare_pipeline_cache_file() {
  if (g_pipelineCacheBroken) {
    return false;
  }
  if (g_pipelineCacheFile != nullptr) {
    return true;
  }

  const auto path = fs_path_to_string(fs_path_from_string(g_config.pipelineCachePath) / PipelineCacheFileName);
  auto records = read_pipeline_cache_file(path);
  // One record per pipeline (its earliest use), configurations of the current version only.
  absl::flat_hash_map<std::pair<uint32_t, uint64_t>, size_t> latest;
  std::vector<PipelineCacheWrite> compacted;
  compacted.reserve(records.size());
  for (auto& record : records) {
    if (!current_pipeline_config_version(record.type, record.configVersion)) {
      continue;
    }
    const auto key = std::make_pair(static_cast<uint32_t>(underlying(record.type)), static_cast<uint64_t>(record.hash));
    const auto [it, inserted] = latest.try_emplace(key, compacted.size());
    if (inserted) {
      compacted.emplace_back(std::move(record));
    } else if (record.firstFrameUsed < compacted[it->second].firstFrameUsed) {
      compacted[it->second].firstFrameUsed = record.firstFrameUsed;
    }
  }
  std::stable_sort(compacted.begin(), compacted.end(),
                   [](const PipelineCacheWrite& a, const PipelineCacheWrite& b) {
                     return a.firstFrameUsed < b.firstFrameUsed;
                   });

  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    Log.error("Failed to open pipeline cache '{}' for writing", path);
    pipeline_cache_abort();
    return false;
  }
  PipelineCacheFileHeader header{.schema = PipelineCacheSchema};
  std::memcpy(header.magic, PipelineCacheMagic, sizeof(PipelineCacheMagic));
  bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1;
  for (const auto& record : compacted) {
    ok = ok && write_pipeline_cache_record(file, record);
  }
  if (!ok || std::fflush(file) != 0) {
    Log.error("Failed to write pipeline cache '{}'", path);
    std::fclose(file);
    pipeline_cache_abort();
    return false;
  }
  g_pipelineCacheFile = file;
  g_loadedPipelineRecords = std::move(compacted);
  return true;
}

static void pipeline_cache_writer() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline cache writer thread");
#endif

  while (true) {
    std::deque<PipelineCacheWrite> batch;
    {
      std::unique_lock lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterCv.wait(lock,
                                   [] { return g_pipelineCacheWriterStop || !g_pipelineCacheWriteQueue.empty(); });
      if (g_pipelineCacheWriterStop && g_pipelineCacheWriteQueue.empty()) {
        return;
      }
      batch.swap(g_pipelineCacheWriteQueue);
      g_pipelineCachePendingWrites.clear();
    }

    bool ok = true;
    for (const auto& write : batch) {
      ok = ok && write_pipeline_cache_record(g_pipelineCacheFile, write);
    }
    if (!ok || std::fflush(g_pipelineCacheFile) != 0) {
      Log.error("Failed to append to the pipeline cache");
      pipeline_cache_abort();
      return;
    }
  }
}

// Boot prewarm telemetry: how long the cached-recipe rebuild takes. Logged once, when the launch queue first drains.
static std::atomic_bool g_prewarmActive{false};
static std::chrono::steady_clock::time_point g_prewarmStart{};
static uint32_t g_prewarmCount = 0;

static void note_pipeline_queue_drained() {
  if (!g_prewarmActive.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_prewarmStart);
  Log.info("Pipeline prewarm finished: {} pipelines in {:.1f} s", g_prewarmCount, elapsed.count() / 1000.0);
}

static void compile_pending_pipeline(PendingPipeline pending) {
  auto result = pending.create();
  {
    std::lock_guard lock{g_pipelineMutex};
    const auto [_, inserted] = g_pipelines.try_emplace(pending.hash, CachedPipeline{
                                                                         .pipeline = std::move(result),
                                                                         .firstFrameUsed = pending.firstFrameUsed,
                                                                     });
    g_pendingPipelines.erase(pending.hash);
    if (inserted) {
      ++createdPipelines;
    }
  }
  g_pipelineCv.notify_all();
  --queuedPipelines;
  if (queuedPipelines.load() == 0) {
    note_pipeline_queue_drained();
  }
}

static void pipeline_worker() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline compilation thread");
#endif

  while (true) {
    PendingPipeline pending;
    bool background = false;
    {
      std::unique_lock lock{g_pipelineMutex};
      g_pipelineCv.wait(lock, [] {
        return !g_priorityPipelines.empty() ||
               (!g_backgroundPipelines.empty() &&
                g_activeBackgroundPipelineWorkers < MaxBackgroundPipelineWorkers) ||
               g_pipelineThreadEnd;
      });
      if (g_pipelineThreadEnd) {
        return;
      }
      background = g_priorityPipelines.empty();
      auto& source = background ? g_backgroundPipelines : g_priorityPipelines;
      pending = std::move(source.front());
      source.pop_front();
      if (background) {
        ++g_activeBackgroundPipelineWorkers;
      }
    }
    compile_pending_pipeline(std::move(pending));
    if (background) {
      {
        std::lock_guard lock{g_pipelineMutex};
        --g_activeBackgroundPipelineWorkers;
      }
      g_pipelineCv.notify_all();
    }
  }
}

static void build_synchronous_pipelines_for_frame() {
  while (g_pipelinesPerFrame < BuildPipelinesPerFrame) {
    PendingPipeline pending;
    {
      std::lock_guard lock{g_pipelineMutex};
      if (g_priorityPipelines.empty() && g_backgroundPipelines.empty()) {
        return;
      }
      auto& source = !g_priorityPipelines.empty() ? g_priorityPipelines : g_backgroundPipelines;
      pending = std::move(source.front());
      source.pop_front();
    }
    compile_pending_pipeline(std::move(pending));
    ++g_pipelinesPerFrame;
  }
}

static size_t pipeline_worker_count() {
  const size_t logicalProcessors = std::thread::hardware_concurrency();
  if (logicalProcessors == 0) {
    return 1;
  }
  const size_t availableWorkers =
      logicalProcessors > ReservedLogicalProcessors ? logicalProcessors - ReservedLogicalProcessors : 1;
  return std::clamp(availableWorkers, size_t{1}, MaxPipelineWorkers);
}

template <typename PipelineConfig, typename CreateFn>
static void load_pipeline_cache_entries(ShaderType type, uint32_t configVersion, CreateFn&& create) {
  for (const auto& record : g_loadedPipelineRecords) {
    if (record.type != type || record.configVersion != configVersion ||
        record.config.size() != sizeof(PipelineConfig)) {
      continue;
    }
    if (xxh3_hash_s(record.config.data(), record.config.size(), static_cast<HashType>(type)) != record.hash) {
      Log.warn("Skipping cached pipeline configuration with mismatched content hash");
      continue;
    }
    PipelineConfig config;
    std::memcpy(&config, record.config.data(), sizeof(config));
    if (config.version != configVersion) {
      continue;
    }
    if constexpr (std::is_same_v<PipelineConfig, gx::PipelineConfig>) {
      if (!gx::valid_pipeline_config(config)) {
        Log.warn("Skipping invalid cached GX pipeline configuration");
        continue;
      }
    }

    find_pipeline_impl(type, config, [=] { return create(config); }, false, record.firstFrameUsed);
  }
}

static void load_pipeline_cache() {
  if (!prepare_pipeline_cache_file()) {
    return;
  }
  load_pipeline_cache_entries<clear::PipelineConfig>(ShaderType::Clear, clear::ClearPipelineConfigVersion,
                                                     clear::create_pipeline);
  load_pipeline_cache_entries<gx::PipelineConfig>(ShaderType::GX, gx::GXPipelineConfigVersion,
                                                  gx::create_pipeline);
  std::vector<PipelineCacheWrite>().swap(g_loadedPipelineRecords);
  const auto queued = queuedPipelines.load();
  if (queued > 0) {
    g_prewarmCount = queued;
    g_prewarmStart = std::chrono::steady_clock::now();
    g_prewarmActive.store(true, std::memory_order_release);
  }
}

static void start_pipeline_cache_writer() {
  if (!prepare_pipeline_cache_file()) {
    return;
  }

  g_pipelineCacheWriterStop = false;
  g_pipelineCacheWriterThread = std::thread(pipeline_cache_writer);
}

static void stop_pipeline_cache_writer() {
  if (g_pipelineCacheWriterThread.joinable()) {
    {
      std::lock_guard lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterStop = true;
    }
    g_pipelineCacheWriterCv.notify_one();
    g_pipelineCacheWriterThread.join();
  } else {
    g_pipelineCacheWriterStop = false;
  }

  g_pipelineCacheWriteQueue.clear();
  g_pipelineCachePendingWrites.clear();
}

template <>
PipelineRef find_pipeline(ShaderType type, const clear::PipelineConfig& config, NewPipelineCallback&& cb) {
  return find_pipeline_impl(type, config, std::move(cb), true, std::nullopt);
}

template <>
PipelineRef find_pipeline(ShaderType type, const gx::PipelineConfig& config, NewPipelineCallback&& cb) {
  return find_pipeline_impl(type, config, std::move(cb), true, std::nullopt);
}

void initialize_pipeline_cache() {
  g_pipelineCacheBroken = false;
  g_pipelineCacheWriterStop = false;
  g_pipelineFrameActive = false;
  g_pipelineThreadEnd = false;
  g_activeBackgroundPipelineWorkers = 0;

  {
    g_hasPipelineThread = true;
    const size_t workerCount = pipeline_worker_count();
    g_pipelineThreads.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
      g_pipelineThreads.emplace_back(pipeline_worker);
    }
    Log.info("Enabled {} priority pipeline compilation workers ({} background prewarm)",
             workerCount, MaxBackgroundPipelineWorkers);
  }

  load_pipeline_cache();
  if (!g_pipelineCacheBroken) {
    start_pipeline_cache_writer();
  }
}

void shutdown_pipeline_cache() {
  if (g_hasPipelineThread) {
    {
      std::lock_guard lock{g_pipelineMutex};
      g_pipelineThreadEnd = true;
    }
    g_pipelineCv.notify_all();
    for (auto& thread : g_pipelineThreads) {
      thread.join();
    }
  }
  g_pipelineThreads.clear();
  g_hasPipelineThread = false;
  g_activeBackgroundPipelineWorkers = 0;

  stop_pipeline_cache_writer();
  pipeline_cache_abort();
  g_pipelineCacheBroken = false;
  g_pipelineFrameActive = false;
  g_pipelinesPerFrame = 0;
  g_pipelines.clear();
  g_priorityPipelines.clear();
  g_backgroundPipelines.clear();
  g_pendingPipelines.clear();

  queuedPipelines = 0;
  createdPipelines = 0;
}

void begin_pipeline_frame() {
  g_pipelineFrameActive = true;
  if (!g_hasPipelineThread) {
    g_pipelinesPerFrame = 0;
  }
}

void end_pipeline_frame() {
  g_pipelineFrameActive = false;
  if (!g_hasPipelineThread) {
    build_synchronous_pipelines_for_frame();
  }
}

void set_skip_unready_pipelines(bool enabled) noexcept {
  g_skipUnreadyGxPipelines.store(enabled, std::memory_order_relaxed);
}

bool skip_unready_pipelines() noexcept { return g_skipUnreadyGxPipelines.load(std::memory_order_relaxed); }

uint32_t queued_pipeline_count() noexcept {
#if defined(__cpp_lib_atomic_ref)
  return queuedPipelines.load(std::memory_order_relaxed);
#else
  return queuedPipelines.load();
#endif
}

bool try_pipeline(PipelineRef ref, gpu::GraphicsPipeline& pipeline) {
  std::scoped_lock lock{g_pipelineMutex};
  const auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end()) {
    return false;
  }
  pipeline = it->second.pipeline;
  return true;
}

static bool wait_pipeline_impl(PipelineRef ref, gpu::GraphicsPipeline& pipeline, bool fatalIfMissing,
                               bool persistentPass) {
  std::unique_lock lock{g_pipelineMutex};
  if (!g_pipelines.contains(ref) && g_pendingPipelines.contains(ref)) {
    ZoneScopedN("wait_pipeline");
    const auto finished = [ref] {
      return g_pipelines.contains(ref) || !g_pendingPipelines.contains(ref) || g_pipelineThreadEnd;
    };
    if (persistentPass) {
      if (promote_pending_pipeline_for_wait(ref)) {
        g_pipelineCv.notify_all();
      }
      // A persistent resolve is the last chance to produce its texture, so timing out here corrupts it
      // permanently. Only a pass marked requireReadyPipelines takes this path.
    }
    g_pipelineCv.wait(lock, finished);
  }
  const auto it = g_pipelines.find(ref);
  if (it == g_pipelines.end()) {
    lock.unlock();
    if (fatalIfMissing) {
      Log.fatal("Pipeline 0x{:x} is unavailable at its ordered draw boundary", static_cast<uint64_t>(ref));
    }
    Log.error("Pipeline 0x{:x} is unavailable for a persistent resolve pass; dropping its draw",
              static_cast<uint64_t>(ref));
    return false;
  }
  pipeline = it->second.pipeline;
  return true;
}

bool wait_pipeline(PipelineRef ref, gpu::GraphicsPipeline& pipeline) {
  return wait_pipeline_impl(ref, pipeline, true, false);
}

bool wait_pipeline_for_persistent_pass(PipelineRef ref, gpu::GraphicsPipeline& pipeline) {
  return wait_pipeline_impl(ref, pipeline, false, true);
}

} // namespace aurora::gfx

void aurora_set_skip_unready_pipelines(const bool enabled) { aurora::gfx::set_skip_unready_pipelines(enabled); }

bool aurora_get_skip_unready_pipelines() { return aurora::gfx::skip_unready_pipelines(); }

uint32_t aurora_get_queued_pipeline_count() { return aurora::gfx::queued_pipeline_count(); }
