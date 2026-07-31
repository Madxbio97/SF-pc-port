#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace sf::platform {

inline constexpr long retail_depth_cue_q12_one = 4096L;
inline constexpr double native_depth_cue_distance_scale = 1.35;

// Native widescreen exposes more of an authored room than the PS1 viewport.
// Scale camera depth before both retail cue equations so their original far
// colour and response remain authoritative while the horizon moves outward.
[[nodiscard]] inline double nativeDepthCueCameraZ(double camera_z) noexcept {
  return camera_z / native_depth_cue_distance_scale;
}

// Exact retail terrain depth-cue equation at 0x800d4a90. The packed low word
// is the raw GTE SZ threshold and the high word is the left shift. Retail
// consumes the per-vertex SZ value directly, applies its 3/4 scale, and then
// feeds the result to DPCS.
[[nodiscard]] inline long
retailTerrainDepthCueFactorQ12(std::uint32_t packed, double camera_z) noexcept {
  if (!std::isfinite(camera_z) || camera_z <= 0.0) {
    return 0L;
  }
  if (packed == 0U) {
    packed = 0x07ffU;
  } else if (static_cast<std::uint16_t>(packed) >= 0x1000U ||
             (packed >> 16U) > 15U) {
    // Retail skips terrain DPCS for an invalid/high authored threshold. It
    // does not substitute the default horizon used by a zero packed value.
    return 0L;
  }
  const auto threshold = static_cast<std::uint16_t>(packed);
  const auto shift = static_cast<unsigned int>(packed >> 16U);
  const auto sz3 = std::clamp<std::int64_t>(
      std::llround(camera_z), 0LL,
      static_cast<std::int64_t>(std::numeric_limits<std::uint16_t>::max()));
  const auto distance = ((sz3 * 3LL) >> 2U) - threshold;
  if (distance <= 0) {
    return 0L;
  }
  return static_cast<long>(
      std::min<std::int64_t>(distance << shift, retail_depth_cue_q12_one));
}

} // namespace sf::platform