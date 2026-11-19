#include "gx.hpp"
#include "__gx.h"

#include "../../gx/command_processor.hpp"
#include "../../gx/fifo.hpp"
#include "../../gx/gx_worker.hpp"
#include "../../gfx/common.hpp"

#include <cstring>

static __GXData_struct sSavedGXData;

// process() stops at a draw that no longer fits the staging batch and returns where it stopped; submit the batch and
// resume there, as fifo::drain does. Ignoring that return dropped the rest of the list (SPM: back walls and floors of
// big map display lists never drew).
static void process_display_list(const u8* data, u32 nbytes, bool bigEndian) {
  struct ReplayScope {
    bool prev = aurora::gx::fifo::g_replayingDisplayList;
    ReplayScope() { aurora::gx::fifo::g_replayingDisplayList = true; }
    ~ReplayScope() { aurora::gx::fifo::g_replayingDisplayList = prev; }
  } replayScope;
  u32 consumed = 0;
  bool retried = false;
  while (consumed < nbytes) {
    const u32 count = aurora::gx::fifo::process(data + consumed, nbytes - consumed, bigEndian);
    if (count == 0 && retried)
      throw aurora::gfx::StagingCapacityError("display list draw does not fit after capacity submission");
    consumed += count;
    if (consumed >= nbytes) break;
    aurora::gfx::split_staging_batch();
    retried = true;
  }
}

namespace aurora::gx::fifo {
// The GX thread's entry (gx_worker.cpp): a display list's copied bytes, decoded as GXCallDisplayList does inline.
void process_display_list_bytes(const uint8_t* data, uint32_t nbytes, bool bigEndian) {
  process_display_list(data, nbytes, bigEndian);
}
// The GX thread's entry for GXCallDisplayListCompiled's lists: the list's copied bytes, submitted from the compiled form
// cached for its address (key) when that draws it as process() would, else decoded, as GXCallDisplayListCompiled does
// inline.
void process_compiled_display_list_bytes(const uint8_t* data, uint32_t nbytes, const uint8_t* key, uint64_t digest) {
  if (submit_compiled_draw_list(data, nbytes, digest, key)) {
    return;
  }
  process_display_list(data, nbytes, true);
}
} // namespace aurora::gx::fifo

extern "C" {
bool GXDisplayListIsEmpty() {
  return aurora::gx::fifo::in_display_list() && aurora::gx::fifo::detail::sDlWritePos == 0 &&
         __gx->dirtyState == 0;
}

void GXBeginDisplayList(void* list, u32 size) {
  CHECK(!aurora::gx::fifo::in_display_list(), "Display list began twice!");

  // Flush any pending dirty state before recording
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Save current shadow register state if requested
  if (__gx->dlSaveContext != 0) {
    std::memcpy(&sSavedGXData, __gx, sizeof(sSavedGXData));
  }

  __gx->inDispList = 1;

  // Redirect FIFO writes to the user-provided buffer
  aurora::gx::fifo::begin_display_list(static_cast<u8*>(list), size);
}

u32 GXEndDisplayList() {
  // Flush any pending dirty state into the display list
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // End FIFO redirection and get the byte count (ROUNDUP32)
  u32 bytesWritten = aurora::gx::fifo::end_display_list();

  // Restore saved shadow register state
  if (__gx->dlSaveContext != 0) {
    std::memcpy(__gx, &sSavedGXData, sizeof(*__gx));
  }

  __gx->inDispList = 0;

  return bytesWritten;
}

void GXCallDisplayList(const void* data, u32 nbytes) {
  // Flush any pending dirty state before calling
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Flush pending primitives
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }

  // Nested display-list calls add bytes instead of changing live renderer state.
  if (aurora::gx::fifo::in_display_list()) {
    aurora::gx::fifo::write_data(data, nbytes);
    return;
  }

  // Decode the display list immediately while its borrowed resources are valid. With the GX thread on, its bytes are
  // copied into the queue instead (the list's memory may be reused once this returns).
  aurora::gx::fifo::drain();
  if (aurora::gx::worker::producer()) {
    aurora::gx::worker::submit_display_list(static_cast<const u8*>(data), nbytes, true);
    return;
  }
  process_display_list(static_cast<const u8*>(data), nbytes, true);
}

// GXCallDisplayList for the native nw4r g3d path's lists with draws (runtime hle/gx/gx_dl.cpp): the same, except that
// the list's draw commands may be submitted from a compiled form (fifo::submit_compiled_draw_list, keyed by digest,
// the list's content digest) instead of being decoded, when that gives the same result.
void GXCallDisplayListCompiled(const void* data, u32 nbytes, uint64_t digest) {
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }
  if (aurora::gx::fifo::in_display_list()) {
    aurora::gx::fifo::write_data(data, nbytes);
    return;
  }
  aurora::gx::fifo::drain();
  if (aurora::gx::worker::producer()) {
    aurora::gx::worker::submit_compiled_display_list(static_cast<const u8*>(data), nbytes, digest);
    return;
  }
  if (aurora::gx::fifo::submit_compiled_draw_list(static_cast<const u8*>(data), nbytes, digest)) {
    return;
  }
  process_display_list(static_cast<const u8*>(data), nbytes, true);
}

// Drops GXCallDisplayListCompiled's compiled lists (the runtime, on a scene change); any thread.
void GXClearCompiledDisplayLists() { aurora::gx::fifo::clear_compiled_draw_lists(); }

// GXCallDisplayList for a list the caller knows holds only BP and CP register writes and NOP padding (the native nw4r
// g3d path's material lists and shape vertex formats, runtime hle/gx/gx_dl.cpp): the same prelude, then the writes
// applied without process()'s renderer lock and dispatch (fifo::apply_state_display_list).
void GXCallStateDisplayList(const void* data, u32 nbytes) {
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }
  if (aurora::gx::fifo::in_display_list()) {
    aurora::gx::fifo::write_data(data, nbytes);
    return;
  }
  aurora::gx::fifo::drain();
  if (aurora::gx::worker::producer()) {
    aurora::gx::worker::submit_state_display_list(static_cast<const u8*>(data), nbytes);
    return;
  }
  aurora::gx::fifo::apply_state_display_list(static_cast<const u8*>(data), nbytes);
}

void GXCallDisplayListLE(const void* data, u32 nbytes) {
  // Flush any pending dirty state before calling
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Flush pending primitives
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }

  // Decode little-endian lists separately after finishing the normal FIFO work.
  aurora::gx::fifo::drain();
  if (aurora::gx::worker::producer()) {
    aurora::gx::worker::submit_display_list(static_cast<const u8*>(data), nbytes, false);
    return;
  }

  // Process the display list through the command processor (little-endian)
  process_display_list(static_cast<const u8*>(data), nbytes, false);
}
}
