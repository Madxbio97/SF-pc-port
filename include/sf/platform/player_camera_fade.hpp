#pragma once

#include "sf/assets/mission_objects.hpp"

#include <algorithm>
#include <array>
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

// MissionTransform stores its root Y in the opposite convention from world
// vertices and the view matrix. Convert it exactly as transformPoint(0,0,0)
// does, before the GTE depth calculation. Passing the raw translation to
// cameraDepth makes a close actor appear distant in maps below Y=0.
[[nodiscard]] constexpr PlayerCameraVisibility
playerCameraVisibility(const assets::MissionTransform &root,
                       const std::array<std::int16_t, 3U> &view_depth_row,
                       std::int32_t view_depth_translation,
                       std::uint32_t pose_flags = 0U) noexcept {
  const auto rotated =
      (static_cast<std::int64_t>(view_depth_row[0]) * root.x -
       static_cast<std::int64_t>(view_depth_row[1]) * root.y +
       static_cast<std::int64_t>(view_depth_row[2]) * root.z) >>
      12U;
  const auto depth = static_cast<std::int32_t>(
      std::clamp<std::int64_t>(rotated + view_depth_translation, 0, 65'535));
  return playerCameraVisibility(depth, pose_flags);
}

} // namespace sf::platform
