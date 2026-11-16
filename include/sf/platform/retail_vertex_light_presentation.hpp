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

// World polygons often reference the same retail vertex. Reuse its result
// during one section pass while keeping distinct indices and authored colours
// separate. A new section pass samples new light matrices and guest colours.
template <typename Colour> class RetailSectionVertexLightCache final {
  struct Entry {
    Colour colour{};
    std::uint32_t generation{};
  };
  std::vector<Entry> entries_;
  std::uint32_t generation_{};

public:
  void beginSection(std::size_t vertex_count) {
    if (entries_.size() < vertex_count)
      entries_.resize(vertex_count);
    if (++generation_ == 0U) {
      for (auto &entry : entries_)
        entry.generation = 0U;
      generation_ = 1U;
    }
  }

  template <typename Compute>
  const Colour &resolve(std::size_t index, Compute &&compute) {
    auto &entry = entries_[index];
    if (entry.generation != generation_) {
      entry.colour = compute();
      entry.generation = generation_;
    }
    return entry.colour;
  }
};

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
    const auto blend_matrix = [&](const auto &from, auto &to) {
      for (std::size_t index = 0U; index < to.rotation.size(); ++index) {
        to.rotation[index] =
            interpolate_rotation(from.rotation[index], to.rotation[index]);
      }
      to.translation.x =
          interpolate_translation(from.translation.x, to.translation.x);
      to.translation.y =
          interpolate_translation(from.translation.y, to.translation.y);
      to.translation.z =
          interpolate_translation(from.translation.z, to.translation.z);
    };
    blend_matrix(old->matrix, light.matrix);
    if (std::ranges::any_of(old->view.rotation,
                            [](auto x) { return x != 0; }) &&
        std::ranges::any_of(light.view.rotation,
                            [](auto x) { return x != 0; })) {
      blend_matrix(old->view, light.view);
    }
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
    for (const auto value : light.view.rotation) {
      mix(value);
    }
    mix(light.view.translation.x);
    mix(light.view.translation.y);
    mix(light.view.translation.z);
    mix(light.shape);
    mix(light.screen_shift);
    mix(light.depth_shift);
    mix(light.threshold);
    mix(light.channel_mask);
  }
  return hash;
}

} // namespace sf::platform
