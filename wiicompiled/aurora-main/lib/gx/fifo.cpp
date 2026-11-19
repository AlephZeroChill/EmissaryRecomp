#include "fifo.hpp"
#include "command_processor.hpp"
#include "gx_worker.hpp"
#include "../gfx/common.hpp"
#include "../internal.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "aurora/profiling.hpp"

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

namespace detail {
thread_local uint8_t* sBufferData = nullptr;
thread_local uint32_t sBufferSize = 0;
thread_local uint32_t sBufferCapacity = 0;
thread_local bool sInDisplayList = false;
thread_local uint8_t* sDlBuffer = nullptr;
thread_local uint32_t sDlSize = 0;
thread_local uint32_t sDlWritePos = 0;
} // namespace detail

void init() {
  constexpr uint32_t initialCapacity = 64 * 1024;
  reset_cp_register_cache();
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
}

void write_data_grow(const void* data, uint32_t length) {
  uint32_t needed = detail::sBufferSize + length;
  uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
  detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCap;
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  uint32_t bytesWritten = detail::sDlWritePos;
  uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

// How much of the producer's frame is spent blocked before it may decode the next batch of GX commands.
static void note_drain_wait(uint64_t nanos) noexcept {
  ZoneScopedN("FIFO drain wait");
  TracyPlot("aurora: fifoDrainWaitUs", static_cast<int64_t>(nanos / 1000));
}

void drain() {
  // GX thread on (gx_worker.hpp): the producer hands its bytes over in order and returns; the GX thread decodes them
  // (after its own SEALED wait).
  if (worker::producer()) {
    if (detail::sBufferSize != 0) {
      worker::submit_fifo(detail::sBufferData, detail::sBufferSize);
      detail::sBufferSize = 0;
    }
    return;
  }
  // SEALED, not DONE.
  const auto waited = aurora::wait_for_frame_worker_sealed();
  if (waited.count() > 0) UNLIKELY {
    note_drain_wait(static_cast<uint64_t>(waited.count()));
  }
  if (detail::sBufferSize == 0) {
    return;
  }
  uint32_t consumed = 0;
  bool retried = false;
  while (consumed < detail::sBufferSize) {
    const auto count = process(detail::sBufferData + consumed, detail::sBufferSize - consumed, true);
    if (count == 0 && retried)
      throw gfx::StagingCapacityError("FIFO draw does not fit after capacity submission");
    consumed += count;
    if (consumed == detail::sBufferSize) break;
    // process returned with its renderer lock released. No recursive drain.
    gfx::split_staging_batch();
    retried = true;
  }
  detail::sBufferSize = 0;
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }
void clear_buffer() {
  detail::sBufferSize = 0;
}

} // namespace aurora::gx::fifo
