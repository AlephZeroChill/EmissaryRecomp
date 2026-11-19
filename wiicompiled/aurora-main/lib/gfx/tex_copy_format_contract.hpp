#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace aurora::gfx::tex_copy_conv::detail {

// GX_TF_Z16 selects the depth RA8 copy format: the encoder writes alpha (255) then the high-Z
// byte, so sampling as IA8 gives high-Z as intensity. Middle and low depth bytes are not stored.
constexpr std::array<std::uint8_t, 4> z16_ra8_as_ia8_rgba(std::uint8_t high) noexcept {
  return {high, high, high, 0xff};
}

// Fragment body (gpu::shader dialect), run after the depth copy prefix has produced `depth_bytes`.
inline constexpr std::string_view Z16FragmentShader = R"(
  float i = float(depth_bytes.r) / 255.0;
  out_color = vec4(i, i, i, 1.0);)";

} // namespace aurora::gfx::tex_copy_conv::detail
