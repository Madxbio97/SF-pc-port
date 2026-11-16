#pragma once

#include <algorithm>
#include <cstdint>

namespace sf::platform {

enum class PlayerCameraVisibility { opaque, translucent, hidden };

// USA v1.1 FUN_800d1898 returns (SZ3 * 3) >> 2 for the HMD root. The player
// branch in FUN_800d0058 makes scores 37..69 semitransparent and skips scores
// below 37. Display+0x0b bit 5 exempts scripted poses from this close fade.
// Use the presented view's root depth, including camera interpolation, rather
// than distance to a guessed torso or an unrelated camera target.
[[nodiscard]] constexpr PlayerCameraVisibility
playerCameraVisibility(std::int32_t root_camera_depth,
                       std::uint32_t pose_flags = 0U) noexcept {
  const auto depth =
      static_cast<std::uint32_t>(std::clamp(root_camera_depth, 0, 65'535));
  const auto score = (depth * 3U) >> 2U;
  if (score == 0U) {
    return PlayerCameraVisibility::hidden;
  }
  constexpr std::uint32_t close_fade_exempt = 0x20000000U;
  if ((pose_flags & close_fade_exempt) != 0U) {
    return PlayerCameraVisibility::opaque;
  }
  if (score < 37U) {
    return PlayerCameraVisibility::hidden;
  }
  if (score < 70U) {
    return PlayerCameraVisibility::translucent;
  }
  return PlayerCameraVisibility::opaque;
}

} // namespace sf::platform
