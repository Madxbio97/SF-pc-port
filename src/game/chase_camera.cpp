#include "sf/game/chase_camera.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace sf::game {

std::int32_t normalizeHeading(std::int64_t heading) noexcept {
  heading %= heading_angle_units;
  if (heading < 0) {
    heading += heading_angle_units;
  }
  return static_cast<std::int32_t>(heading);
}

std::int32_t headingFromDirection(double x, double z) noexcept {
  const auto radians = std::atan2(x, z);
  return normalizeHeading(static_cast<std::int64_t>(
      std::lround(radians * static_cast<double>(heading_angle_units) /
                  (2.0 * std::numbers::pi))));
}

HorizontalBasis headingBasis(std::int32_t heading) noexcept {
  const auto radians =
      static_cast<double>(normalizeHeading(heading)) *
      (2.0 * std::numbers::pi / static_cast<double>(heading_angle_units));
  const auto sine = std::sin(radians);
  const auto cosine = std::cos(radians);
  return HorizontalBasis{
      HorizontalDirection{cosine, -sine},
      HorizontalDirection{sine, cosine},
  };
}

HorizontalDirection headingDirection(std::int32_t heading) noexcept {
  return headingBasis(heading).forward;
}

CameraRay cameraRayAtProjectionOffset(const CameraState &camera,
                                      double horizontal_offset,
                                      double vertical_offset) noexcept {
  const auto normalize = [](double x, double y, double z) {
    const auto length = std::hypot(x, y, z);
    if (!std::isfinite(length) || length <= 0.000001) {
      return std::array{0.0, 0.0, 1.0};
    }
    return std::array{x / length, y / length, z / length};
  };
  const auto cross = [](const std::array<double, 3U> &first,
                        const std::array<double, 3U> &second) {
    return std::array{
        first[1] * second[2] - first[2] * second[1],
        first[2] * second[0] - first[0] * second[2],
        first[0] * second[1] - first[1] * second[0],
    };
  };

  const auto forward =
      normalize(camera.target_x - camera.x, camera.target_y - camera.y,
                camera.target_z - camera.z);
  const auto right_cross = cross(forward, std::array{0.0, -1.0, 0.0});
  const auto right = normalize(right_cross[0], right_cross[1], right_cross[2]);
  const auto down = cross(forward, right);
  const auto projection =
      std::max(1.0, std::abs(static_cast<double>(camera.projection)));
  const auto direction =
      normalize(forward[0] + right[0] * horizontal_offset / projection +
                    down[0] * vertical_offset / projection,
                forward[1] + right[1] * horizontal_offset / projection +
                    down[1] * vertical_offset / projection,
                forward[2] + right[2] * horizontal_offset / projection +
                    down[2] * vertical_offset / projection);
  return CameraRay{
      camera.x, camera.y, camera.z, direction[0], direction[1], direction[2],
  };
}

ChaseCamera::ChaseCamera(ChaseCameraConfiguration configuration) noexcept
    : configuration_(configuration) {}

CameraState ChaseCamera::follow(double x, double y, double z,
                                std::int32_t heading) const noexcept {
  const auto forward = headingBasis(heading).forward;
  return CameraState{
      x - forward.x * configuration_.distance,
      y - configuration_.height,
      z - forward.z * configuration_.distance,
      x + forward.x * configuration_.target_distance,
      y - configuration_.target_height,
      z + forward.z * configuration_.target_distance,
  };
}

FirstPersonCamera::FirstPersonCamera(
    FirstPersonCameraConfiguration configuration) noexcept
    : configuration_(configuration) {}

CameraState FirstPersonCamera::view(double x, double y, double z,
                                    std::int32_t heading,
                                    double pitch) const noexcept {
  const auto horizontal = headingDirection(heading);
  const auto pitch_radians = pitch * (2.0 * std::numbers::pi /
                                      static_cast<double>(heading_angle_units));
  const auto pitch_cosine = std::cos(pitch_radians);
  const auto pitch_sine = std::sin(pitch_radians);
  const auto eye_x = x + horizontal.x * configuration_.forward_offset;
  const auto eye_y = y - configuration_.eye_height;
  const auto eye_z = z + horizontal.z * configuration_.forward_offset;
  return CameraState{
      eye_x,
      eye_y,
      eye_z,
      eye_x + horizontal.x * pitch_cosine * configuration_.sight_distance,
      eye_y + pitch_sine * configuration_.sight_distance,
      eye_z + horizontal.z * pitch_cosine * configuration_.sight_distance,
  };
}

CameraState interpolateCameraPresentation(const CameraState &previous,
                                          const CameraState &current,
                                          double amount, bool first_person,
                                          bool aim_transition) noexcept {
  if (aim_transition) {
    return current;
  }

  amount = std::clamp(amount, 0.0, 1.0);
  const auto component = [amount](double from, double to) {
    return std::lerp(from, to, amount);
  };
  auto result = current;
  result.x = component(previous.x, current.x);
  result.y = component(previous.y, current.y);
  result.z = component(previous.z, current.z);
  result.projection = static_cast<std::int32_t>(
      std::llround(component(previous.projection, current.projection)));

  if (first_person) {
    // Lean/peek translates the retail eye and target together.  Retain
    // the newest sight vector so interpolation cannot add input latency or
    // fight the per-presentation mouse-look correction.
    result.target_x = result.x + current.target_x - current.x;
    result.target_y = result.y + current.target_y - current.y;
    result.target_z = result.z + current.target_z - current.z;
  } else {
    result.target_x = component(previous.target_x, current.target_x);
    result.target_y = component(previous.target_y, current.target_y);
    result.target_z = component(previous.target_z, current.target_z);
  }
  return result;
}

} // namespace sf::game
