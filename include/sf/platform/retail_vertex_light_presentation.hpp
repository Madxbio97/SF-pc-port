#pragma once

#include "sf/game/legacy_bridge_types.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace sf::platform {

inline void interpolateRetailVertexLights(
    std::span<const game::LegacyVertexLightBridgeState> previous,
    std::span<const game::LegacyVertexLightBridgeState> current, double amount,
    std::vector<game::LegacyVertexLightBridgeState> &result) {
  amount = std::clamp(amount, 0.0, 1.0);
  result.assign(current.begin(), current.end());
  const auto interpolate_rotation = [amount](std::int16_t from,
                                              std::int16_t to) {
    return static_cast<std::int16_t>(std::clamp<long>(
        std::lround(std::lerp(static_cast<double>(from),
                              static_cast<double>(to), amount)),
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
  };
  const auto interpolate_translation = [amount](std::int32_t from,
                                                 std::int32_t to) {
    return static_cast<std::int32_t>(std::clamp<long long>(
        std::llround(std::lerp(static_cast<double>(from),
                               static_cast<double>(to), amount)),
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
  };
  for (auto &light : result) {
    const auto old = std::find_if(previous.begin(), previous.end(),
                                  [&](const auto &candidate) {
                                    return candidate.source == light.source;
                                  });
    if (old == previous.end() || ((old->flags ^ light.flags) & 1U) != 0U) {
      continue;
    }
    for (std::size_t index = 0U; index < light.matrix.rotation.size();
         ++index) {
      light.matrix.rotation[index] = interpolate_rotation(
          old->matrix.rotation[index], light.matrix.rotation[index]);
    }
    light.matrix.translation.x = interpolate_translation(
        old->matrix.translation.x, light.matrix.translation.x);
    light.matrix.translation.y = interpolate_translation(
        old->matrix.translation.y, light.matrix.translation.y);
    light.matrix.translation.z = interpolate_translation(
        old->matrix.translation.z, light.matrix.translation.z);
  }
}

[[nodiscard]] inline std::uint64_t retailVertexLightPresentationSignature(
    std::span<const game::LegacyVertexLightBridgeState> lights,
    std::int32_t projection) noexcept {
  auto hash = std::uint64_t{14695981039346656037ULL};
  const auto mix = [&hash]<typename T>(T value) {
    using Unsigned = std::make_unsigned_t<T>;
    const auto bits = static_cast<Unsigned>(value);
    for (std::size_t byte = 0U; byte < sizeof(bits); ++byte) {
      hash ^= static_cast<std::uint8_t>(bits >> (byte * 8U));
      hash *= 1099511628211ULL;
    }
  };
  if (!lights.empty()) {
    mix(std::max(projection, 1));
  }
  mix(static_cast<std::uint64_t>(lights.size()));
  for (const auto &light : lights) {
    mix(light.source);
    mix(light.flags);
    for (const auto value : light.matrix.rotation) {
      mix(value);
    }
    mix(light.matrix.translation.x);
    mix(light.matrix.translation.y);
    mix(light.matrix.translation.z);
    mix(light.shape);
    mix(light.screen_shift);
    mix(light.depth_shift);
    mix(light.threshold);
    mix(light.channel_mask);
  }
  return hash;
}

} // namespace sf::platform
