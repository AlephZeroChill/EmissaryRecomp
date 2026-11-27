#pragma once

#include "texture.hpp"
#include <optional>

namespace aurora::gfx::texture_replacement {
void initialize() noexcept;
void shutdown() noexcept;
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept;
void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept;
// load_tlut does work (producer state of its own, keyed by object address): a TLUT load must stay on the producer
// thread then (the runtime's deferred native draws, hle/g3d_native.cpp). Constant after init.
bool tlut_mirror_active() noexcept;
// `tlut`: the TLUT the renderer decodes a palette texture with (null: the palette mirror's copy of its slot).
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj, const GXTlutObj_* tlut = nullptr) noexcept;
std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept;
// New Super Paper Bros.: the button prompt set for the controller in use (0 xbox, 1 playstation, 2 nintendo,
// -1 the game's own Wii Remote icons). Returns true once after it changed, so cached textures can be re-resolved.
void set_button_icon_style(int style) noexcept;
bool take_button_icon_style_change() noexcept;
int button_icon_style() noexcept;
} // namespace aurora::gfx::texture_replacement
