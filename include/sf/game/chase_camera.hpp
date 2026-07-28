#pragma once

#include <cstdint>

namespace sf::game {

inline constexpr std::int32_t heading_angle_units = 4096;

struct HorizontalDirection {
  double x{};
  double z{};
};

struct HorizontalBasis {
  HorizontalDirection right;
  HorizontalDirection forward;
};

struct CameraState {
  double x{};
  double y{};
  double z{};
  double target_x{};
  double target_y{};
  double target_z{};
  // GTE H value used by the native renderer. Retail stores this directly on
  // the active GsVIEW2 wrapper, so guest cameras do not need an FOV guess.
  std::int32_t projection{320};
};

struct CameraRay {
  double origin_x{};
  double origin_y{};
  double origin_z{};
  double direction_x{};
  double direction_y{};
  double direction_z{};
};

struct ChaseCameraConfiguration {
  // Retail first-level framing keeps Gabe's full 390-unit height inside the
  // native 384x240 view. Mission coordinates use positive Y down.
  double distance{672.0};
  double height{300.0};
  double target_distance{0.0};
  double target_height{185.0};
};

struct FirstPersonCameraConfiguration {
  double eye_height{50.0};
  double forward_offset{34.0};
  double sight_distance{1600.0};
};

[[nodiscard]] std::int32_t normalizeHeading(std::int64_t heading) noexcept;
[[nodiscard]] std::int32_t headingFromDirection(double x, double z) noexcept;
[[nodiscard]] HorizontalBasis headingBasis(std::int32_t heading) noexcept;
[[nodiscard]] HorizontalDirection
headingDirection(std::int32_t heading) noexcept;
// Unprojects a logical-screen offset through the same GTE projection used by
// native presentation. Positive vertical offsets point toward the lower half
// of the image (mission coordinates use positive Y down).
[[nodiscard]] CameraRay
cameraRayAtProjectionOffset(const CameraState &camera, double horizontal_offset,
                            double vertical_offset) noexcept;

class ChaseCamera final {
public:
  explicit ChaseCamera(ChaseCameraConfiguration configuration = {}) noexcept;

  [[nodiscard]] CameraState follow(double x, double y, double z,
                                   std::int32_t heading) const noexcept;

private:
  ChaseCameraConfiguration configuration_;
};

class FirstPersonCamera final {
public:
  explicit FirstPersonCamera(
      FirstPersonCameraConfiguration configuration = {}) noexcept;

  [[nodiscard]] CameraState view(double x, double y, double z,
                                 std::int32_t heading,
                                 double pitch) const noexcept;

private:
  FirstPersonCameraConfiguration configuration_;
};

// Smooths the immutable camera samples produced by the 20 Hz guest for native
// presentation.  A first-person sample keeps the newest authoritative sight
// vector (mouse look is composed separately) while its collision-limited eye
// translation is interpolated.  Aim entry/exit remains an intentional cut.
[[nodiscard]] CameraState
interpolateCameraPresentation(const CameraState &previous,
                              const CameraState &current, double amount,
                              bool first_person, bool aim_transition) noexcept;

} // namespace sf::game
