#pragma once

#include "sf/platform/retail_depth_cue.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sf::platform {

inline constexpr double world_chunk_fade_seconds = 1.35;
inline constexpr double world_chunk_prefetch_lead = 0.90;
inline constexpr std::size_t maximum_world_chunk_count = 0xfeU;

[[nodiscard]] inline long worldChunkSpatialDepthCueFloorQ12(
    double reveal_progress, double reveal_coordinate, double near_coordinate,
    double far_coordinate) noexcept {
  reveal_progress = std::clamp(reveal_progress, 0.0, 1.0);
  if (reveal_progress <= 0.0) {
    return retail_depth_cue_q12_one;
  }
  if (reveal_progress >= 1.0) {
    return 0L;
  }
  if (!std::isfinite(reveal_coordinate) ||
      !std::isfinite(near_coordinate) || !std::isfinite(far_coordinate) ||
      far_coordinate <= near_coordinate) {
    return static_cast<long>(std::lround(
        (1.0 - reveal_progress) * retail_depth_cue_q12_one));
  }

  const auto coordinate_span = far_coordinate - near_coordinate;
  const auto feather = std::clamp(coordinate_span * 0.22, 192.0, 768.0);
  // Reveal from the player's entry point outwards. A fixed world-space origin
  // keeps the wave stable when the camera turns during the transition.
  const auto front = std::lerp(near_coordinate - feather,
                               far_coordinate + feather, reveal_progress);
  const auto amount = std::clamp(
      ((front + feather) - reveal_coordinate) / (feather * 2.0), 0.0, 1.0);
  const auto visible = amount * amount * (3.0 - 2.0 * amount);
  return static_cast<long>(
      std::lround((1.0 - visible) * retail_depth_cue_q12_one));
}

// Presentation-only visibility for streamed world models. Gameplay and VRAM
// residency switch at the exact retail 20 Hz boundary; newly visible geometry
// then emerges from the authored far colour at the native display rate.
class WorldChunkAppearance final {
public:
  void reset() noexcept {
    visibility_.fill(0.0);
    active_.fill(false);
    warm_.fill(false);
    reveal_origin_x_.fill(0.0);
    reveal_origin_z_.fill(0.0);
    reveal_origin_valid_.fill(false);
    initialized_ = false;
  }

  void prime(std::span<const std::uint16_t> active_models,
             std::span<const std::uint16_t> warm_models = {},
             double observer_x = 0.0, double observer_z = 0.0) noexcept {
    reset();
    const auto valid_observer =
        std::isfinite(observer_x) && std::isfinite(observer_z);
    for (const auto model : active_models) {
      if (model >= active_.size()) {
        continue;
      }
      active_[model] = true;
      visibility_[model] = 1.0;
      reveal_origin_x_[model] = valid_observer ? observer_x : 0.0;
      reveal_origin_z_[model] = valid_observer ? observer_z : 0.0;
      reveal_origin_valid_[model] = true;
    }
    for (const auto model : warm_models) {
      if (model >= warm_.size() || active_[model]) {
        continue;
      }
      warm_[model] = true;
      // A discontinuity (checkpoint restore, retry or movie return) must not
      // reveal optional lookahead geometry before the first coherent scene
      // frame.  The texture streamer can admit several connected rooms in
      // one rebuild; priming all of them at the lead value made those rooms
      // appear as separate depth layers after a failure.  Active terrain is
      // restored immediately above, while lookahead resumes its normal wave
      // from the authored far colour on subsequent presentation frames.
      visibility_[model] = 0.0;
      reveal_origin_x_[model] = valid_observer ? observer_x : 0.0;
      reveal_origin_z_[model] = valid_observer ? observer_z : 0.0;
      reveal_origin_valid_[model] = true;
    }
    initialized_ = true;
  }

  void advance(std::span<const std::uint16_t> active_models,
               double delta_seconds,
               std::span<const std::uint16_t> warm_models = {},
               double observer_x = 0.0, double observer_z = 0.0) noexcept {
    std::array<bool, maximum_world_chunk_count> next_active{};
    for (const auto model : active_models) {
      if (model < next_active.size()) {
        next_active[model] = true;
      }
    }
    std::array<bool, maximum_world_chunk_count> next_warm{};
    for (const auto model : warm_models) {
      if (model < next_warm.size()) {
        next_warm[model] = true;
      }
    }

    const auto capture_reveal_origin = [&](std::size_t model) noexcept {
      const auto valid_observer =
          std::isfinite(observer_x) && std::isfinite(observer_z);
      reveal_origin_x_[model] = valid_observer ? observer_x : 0.0;
      reveal_origin_z_[model] = valid_observer ? observer_z : 0.0;
      reveal_origin_valid_[model] = true;
    };

    if (!initialized_) {
      active_ = next_active;
      for (auto model = std::size_t{}; model < active_.size(); ++model) {
        warm_[model] = next_warm[model] && !active_[model];
        visibility_[model] = active_[model] ? 1.0 : 0.0;
        if (active_[model] || warm_[model]) {
          capture_reveal_origin(model);
        }
      }
      initialized_ = true;
      return;
    }

    const auto elapsed =
        std::isfinite(delta_seconds) ? std::max(0.0, delta_seconds) : 0.0;
    const auto step = elapsed / world_chunk_fade_seconds;
    for (auto model = std::size_t{}; model < active_.size(); ++model) {
      const auto begins_warm = next_warm[model] && !next_active[model] &&
                               !warm_[model] && !active_[model];
      const auto begins_active = next_active[model] && !active_[model] &&
                                 !warm_[model];
      if (begins_warm || begins_active) {
        capture_reveal_origin(model);
      }
      if (begins_warm) {
        // The first prefetched presentation frame is fully hidden. Starting
        // at the lead value reproduced the same hard edge as a cold switch.
        visibility_[model] = 0.0;
        continue;
      }
      const auto target = next_active[model]
                              ? 1.0
                              : next_warm[model]
                                    ? world_chunk_prefetch_lead
                                    : 0.0;
      if (visibility_[model] < target) {
        visibility_[model] =
            std::min(target, visibility_[model] + step);
      } else {
        visibility_[model] =
            std::max(target, visibility_[model] - step);
      }
    }
    active_ = next_active;
    for (auto model = std::size_t{}; model < warm_.size(); ++model) {
      warm_[model] = next_warm[model] && !active_[model];
    }
  }

  [[nodiscard]] bool isActive(std::uint16_t model) const noexcept {
    return initialized_ && model < active_.size() && active_[model];
  }

  [[nodiscard]] bool isWarm(std::uint16_t model) const noexcept {
    return initialized_ && model < warm_.size() && warm_[model];
  }

  [[nodiscard]] double revealProgress(std::uint16_t model) const noexcept {
    return isActive(model) || isWarm(model)
               ? std::clamp(visibility_[model], 0.0, 1.0)
               : 0.0;
  }

  [[nodiscard]] double revealCoordinate(std::uint16_t model, double x,
                                        double z) const noexcept {
    if (!initialized_ || model >= reveal_origin_valid_.size() ||
        !reveal_origin_valid_[model] || !std::isfinite(x) ||
        !std::isfinite(z)) {
      return 0.0;
    }
    // Radial distance has no sign to invert when a mission camera starts a
    // portal transition facing sideways or back towards the previous room.
    return std::hypot(x - reveal_origin_x_[model],
                      z - reveal_origin_z_[model]);
  }

  [[nodiscard]] long depthCueFloorQ12(std::uint16_t model) const noexcept {
    if (!initialized_ || model >= visibility_.size() || !active_[model]) {
      return 0L;
    }
    const auto visibility = std::clamp(visibility_[model], 0.0, 1.0);
    // Quintic smootherstep has zero velocity and acceleration at both ends,
    // so streamed terrain neither snaps out of the far colour nor visibly
    // brakes as it reaches its authored lighting.
    const auto smooth_visibility =
        visibility * visibility * visibility *
        (visibility * (visibility * 6.0 - 15.0) + 10.0);
    return static_cast<long>(
        std::lround((1.0 - smooth_visibility) *
                    static_cast<double>(retail_depth_cue_q12_one)));
  }

private:
  std::array<double, maximum_world_chunk_count> visibility_{};
  std::array<bool, maximum_world_chunk_count> active_{};
  std::array<bool, maximum_world_chunk_count> warm_{};
  std::array<double, maximum_world_chunk_count> reveal_origin_x_{};
  std::array<double, maximum_world_chunk_count> reveal_origin_z_{};
  std::array<bool, maximum_world_chunk_count> reveal_origin_valid_{};
  bool initialized_{};
};

} // namespace sf::platform
