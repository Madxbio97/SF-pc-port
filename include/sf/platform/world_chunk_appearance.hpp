#pragma once

#include "sf/platform/retail_depth_cue.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sf::platform {

inline constexpr double world_chunk_fade_seconds = 0.60;
inline constexpr std::size_t maximum_world_chunk_count = 0xfeU;

// Presentation-only visibility for streamed world models. Gameplay and VRAM
// residency switch at the exact retail 20 Hz boundary; newly visible geometry
// then emerges from the authored far colour at the native display rate.
class WorldChunkAppearance final {
public:
  void reset() noexcept {
    visibility_.fill(0.0);
    active_.fill(false);
    initialized_ = false;
  }

  void advance(std::span<const std::uint16_t> active_models,
               double delta_seconds) noexcept {
    std::array<bool, maximum_world_chunk_count> next_active{};
    for (const auto model : active_models) {
      if (model < next_active.size()) {
        next_active[model] = true;
      }
    }

    if (!initialized_) {
      active_ = next_active;
      for (auto model = std::size_t{}; model < active_.size(); ++model) {
        visibility_[model] = active_[model] ? 1.0 : 0.0;
      }
      initialized_ = true;
      return;
    }

    const auto elapsed =
        std::isfinite(delta_seconds) ? std::max(0.0, delta_seconds) : 0.0;
    const auto step = elapsed / world_chunk_fade_seconds;
    for (auto model = std::size_t{}; model < active_.size(); ++model) {
      if (!next_active[model]) {
        visibility_[model] = 0.0;
      } else if (!active_[model]) {
        // Start on the first native presentation frame. Holding at zero for
        // an extra frame made a 20 Hz room transaction look like a delayed
        // pop even though the following frames were interpolated.
        visibility_[model] = std::min(1.0, step);
      } else {
        visibility_[model] = std::min(1.0, visibility_[model] + step);
      }
    }
    active_ = next_active;
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
  bool initialized_{};
};

} // namespace sf::platform
