#pragma once

#include "../internal.hpp"

#include <cstring>

namespace aurora::gx::fifo {

// A thread_local with a constant initializer, declared so: other translation units then access it directly instead
// of through a TLS wrapper call that checks for a dynamic initializer (every GX_WRITE_* touches the buffer below).
#ifndef AURORA_TLS_CONSTINIT
#if defined(__clang__)
#define AURORA_TLS_CONSTINIT [[clang::require_constant_initialization]]
#elif defined(__GNUC__) && __GNUC__ >= 10
#define AURORA_TLS_CONSTINIT __constinit
#else
#define AURORA_TLS_CONSTINIT
#endif
#endif

namespace detail {
// Per producer thread: the game thread's, and a draw worker's (gx_worker.hpp deferred jobs) while it fills a job.
AURORA_TLS_CONSTINIT extern thread_local uint8_t* sBufferData;
AURORA_TLS_CONSTINIT extern thread_local uint32_t sBufferSize;
AURORA_TLS_CONSTINIT extern thread_local uint32_t sBufferCapacity;
AURORA_TLS_CONSTINIT extern thread_local bool sInDisplayList;
AURORA_TLS_CONSTINIT extern thread_local uint8_t* sDlBuffer;
AURORA_TLS_CONSTINIT extern thread_local uint32_t sDlSize;
AURORA_TLS_CONSTINIT extern thread_local uint32_t sDlWritePos;
} // namespace detail

void init();

// Out-of-line slow path: grows internal buffer then appends data
void write_data_grow(const void* data, uint32_t length);

inline void write_data(const void* data, const uint32_t length) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize + length <= detail::sBufferCapacity)
        LIKELY {
          std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
          detail::sBufferSize += length;
          return;
        }
      write_data_grow(data, length);
    }
  else if (detail::sDlWritePos + length <= detail::sDlSize) {
    std::memcpy(detail::sDlBuffer + detail::sDlWritePos, data, length);
    detail::sDlWritePos += length;
  }
}

inline void write_u8(const uint8_t val) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize < detail::sBufferCapacity)
        LIKELY {
          detail::sBufferData[detail::sBufferSize++] = val;
          return;
        }
      write_data_grow(&val, 1);
    }
  else if (detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = val;
  }
}

inline void write_u16(const uint16_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u32(const uint32_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u64(const uint64_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_f32(const float val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

// Display list recording
void begin_display_list(uint8_t* buf, uint32_t size);
uint32_t end_display_list();
bool in_display_list();

// Drain the internal FIFO buffer through the command processor
void drain();

// Internal buffer inspection
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
void clear_buffer();

} // namespace aurora::gx::fifo
