#pragma once

#include "dolphin/gx/GXEnum.h"
#include "../internal.hpp"

namespace aurora::gx::fifo {

// Reset the CP write-deduplication state whenever the GX shadow state is reinitialized.
void reset_cp_register_cache();

// True while a recorded display list is being replayed. The game's SDK never flushes deferred state (genMode) into a
// list, so genMode copies Aurora recorded must not switch coplanar mode off (SPM mi1_01 front grass).
extern bool g_replayingDisplayList;

// Process a buffer of GX FIFO commands
uint32_t process(const uint8_t* data, uint32_t size, bool bigEndian);

// Submit already-packed direct vertex bytes against the current GX state. With the GX thread on, the vertices are
// copied into its queue and the draw is decoded there in order; the result is then true unless the arguments are empty
// (a vertex-size mismatch found there is logged and the draw dropped).
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
// A display list of draw commands and indexed XF loads only, submitted from its compiled form (cached by `key`, the
// list's own address (default: data), checked against digest, the caller's content digest of the list): true when that
// drew it exactly as process() would have; false (nothing changed) leaves it to process(). By the decoding thread:
// inline, or the GX thread for a list it got as a copy (data = the copy, key = the list's address).
bool submit_compiled_draw_list(const uint8_t* data, uint32_t size, uint64_t digest, const uint8_t* key = nullptr);
// Drops every compiled list (the cache only replaces an entry when its address is drawn again with other content).
void clear_compiled_draw_lists() noexcept;
// A display list of BP and CP register writes and NOP padding only, applied as process() would, without its renderer
// lock (inline decoding only; GXCallStateDisplayList).
void apply_state_display_list(const uint8_t* data, uint32_t size);
// The part of submit_raw_draw that reads decoded state: inline, or on the GX thread for a queued raw draw.
bool submit_raw_draw_decoded(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                             uint32_t vertexBytes);
// GXApplyBPReg's decode of one BP register word (reg << 24 | value): inline, or on the GX thread in order.
void apply_bp_word(uint32_t word);

} // namespace aurora::gx::fifo
