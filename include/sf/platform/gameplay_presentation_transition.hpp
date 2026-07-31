#pragma once
#include <algorithm>
namespace sf::platform {
// Native presentation runs independently of the deterministic 20 Hz guest.
// Keep letterbox/HUD motion on display time so it remains equally smooth at
// 30, 60 and 240 Hz without changing retail gameplay timing.
class GameplayPresentationTransition {
public:
  static constexpr double phase_duration_seconds = 0.16;
  static constexpr double duration_seconds = phase_duration_seconds * 2.0;
  void reset() noexcept { progress_ = 0.0; }
  void advance(bool letterbox_requested, double delta_seconds) noexcept {
    const auto safe_delta = std::clamp(delta_seconds, 0.0, duration_seconds);
    const auto step = safe_delta / duration_seconds;
    progress_ =
        std::clamp(progress_ + (letterbox_requested ? step : -step), 0.0, 1.0);
  }
  [[nodiscard]] double letterboxAmount() const noexcept {
    return eased(std::clamp((progress_ - 0.5) * 2.0, 0.0, 1.0));
  }
  [[nodiscard]] double hudVisibility() const noexcept {
    return 1.0 - eased(std::clamp(progress_ * 2.0, 0.0, 1.0));
  }

private:
  [[nodiscard]] static constexpr double eased(double value) noexcept {
    // Quintic smootherstep keeps velocity and acceleration continuous at the
    // endpoints of both ordered phases.
    return value * value * value * (value * (value * 6.0 - 15.0) + 10.0);
  }
  double progress_{};
};
} // namespace sf::platform