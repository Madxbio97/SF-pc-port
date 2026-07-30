#pragma once

#include "sf/game/chase_camera.hpp"

#include <cmath>

namespace sf::platform {

enum class PlayerCameraVisibility { opaque, translucent, hidden };

// Camera collision can place the eye inside Gabe's HMD. Fade only while the
// camera is still tracking his torso: scripted cameras which merely pass near
// the actor must never make him randomly disappear.
[[nodiscard]] inline PlayerCameraVisibility
playerCameraVisibility(const game::CameraState &camera, double player_x,
                       double player_y, double player_z) noexcept {
  const auto finite = [](double value) { return std::isfinite(value); };
  if (!finite(camera.x) || !finite(camera.y) || !finite(camera.z) ||
      !finite(camera.target_x) || !finite(camera.target_y) ||
      !finite(camera.target_z) || !finite(player_x) || !finite(player_y) ||
      !finite(player_z)) {
    return PlayerCameraVisibility::opaque;
  }

  constexpr auto torso_height = 220.0;
  const auto torso_y = player_y - torso_height;
  const auto squared_distance = [](double ax, double ay, double az, double bx,
                                   double by, double bz) {
    const auto x = ax - bx;
    const auto y = ay - by;
    const auto z = az - bz;
    return x * x + y * y + z * z;
  };
  constexpr auto maximum_tracking_distance = 320.0;
  if (squared_distance(camera.target_x, camera.target_y, camera.target_z,
                       player_x, torso_y, player_z) >
      maximum_tracking_distance * maximum_tracking_distance) {
    return PlayerCameraVisibility::opaque;
  }

  const auto camera_distance_squared = squared_distance(
      camera.x, camera.y, camera.z, player_x, torso_y, player_z);
  constexpr auto hidden_distance = 56.0;
  constexpr auto translucent_distance = 300.0;
  if (camera_distance_squared <= hidden_distance * hidden_distance) {
    return PlayerCameraVisibility::hidden;
  }
  if (camera_distance_squared <= translucent_distance * translucent_distance) {
    return PlayerCameraVisibility::translucent;
  }
  return PlayerCameraVisibility::opaque;
}

} // namespace sf::platform
