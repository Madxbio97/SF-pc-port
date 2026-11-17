#pragma once

#include "sf/game/legacy_bridge_types.hpp"

#include <array>
#include <cmath>

namespace sf::game {

struct LegacyVisibilityPoint {
  double x{};
  double y{};
  double z{};
};

// FUN_8003352c starts its shot vector at instance->motion's root; the common
// AI's DAT_8011e660 anchor is in that same skeleton space. It is not a foot
// position. The live root already follows standing/crouched animation.
// These conservative body samples confirm an existing retail positive result;
// they do not replace retail hit geometry, range, FOV or target selection.
template <typename ClearSegment>
[[nodiscard]] bool legacyPhysicalActorVisible(const LegacyNativePoint &source,
                                              const LegacyNativePoint &target,
                                              bool target_crouched,
                                              ClearSegment &&clear_segment) {
  constexpr double shoulder_radius = 58.0;
  // The ordinary original L1 camera is 0x48 above the skeleton root in both
  // stances. Lower samples stay inside the standing/crouched body envelope.
  constexpr double upper_offset = 72.0;
  const LegacyVisibilityPoint from{static_cast<double>(source.x),
                                   static_cast<double>(source.y),
                                   static_cast<double>(source.z)};
  const LegacyVisibilityPoint centre{static_cast<double>(target.x),
                                     static_cast<double>(target.y),
                                     static_cast<double>(target.z)};
  const auto dx = centre.x - from.x;
  const auto dz = centre.z - from.z;
  const auto length = std::hypot(dx, dz);
  const auto side_x =
      length > 0.0 ? -dz * shoulder_radius / length : shoulder_radius;
  const auto side_z = length > 0.0 ? dx * shoulder_radius / length : 0.0;
  const std::array points{
      centre,
      LegacyVisibilityPoint{centre.x, centre.y - upper_offset, centre.z},
      LegacyVisibilityPoint{
          centre.x, centre.y + (target_crouched ? 36.0 : 72.0), centre.z},
      LegacyVisibilityPoint{centre.x + side_x, centre.y, centre.z + side_z},
      LegacyVisibilityPoint{centre.x - side_x, centre.y, centre.z - side_z},
  };
  for (const auto &to : points) {
    if (clear_segment(from, to) &&
        ((to.x == centre.x && to.y == centre.y && to.z == centre.z) ||
         clear_segment(centre, to))) {
      return true;
    }
  }
  return false;
}

} // namespace sf::game
