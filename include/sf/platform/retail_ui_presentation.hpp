#pragma once

#include <algorithm>
#include <cstdint>

namespace sf::platform {

struct RetailUiPresentationSample {
  double viewport_y{};
  double viewport_height{240.0};
  double normal_hud_phase{13.0};
  std::uint8_t interface_mode{1U};
  bool available{};
};

inline constexpr double retail_ui_interpolation_speed = 1.15;

[[nodiscard]] constexpr double retailUiAbsolute(double value) noexcept {
  return value < 0.0 ? -value : value;
}

[[nodiscard]] constexpr RetailUiPresentationSample
interpolateRetailUiPresentation(const RetailUiPresentationSample &previous,
                                const RetailUiPresentationSample &current,
                                double amount) noexcept {
  auto result = current;
  amount = std::clamp(amount * retail_ui_interpolation_speed, 0.0, 1.0);
  // Authoritative state remains on the 20 Hz guest. Only adjacent authored
  // steps are blended slightly ahead of wall time; loading, reset and mode
  // discontinuities snap current.
  const auto adjacent =
      previous.available && current.available &&
      retailUiAbsolute(current.viewport_y - previous.viewport_y) <= 2.0 &&
      retailUiAbsolute(current.viewport_height - previous.viewport_height) <=
          4.0 &&
      retailUiAbsolute(current.normal_hud_phase - previous.normal_hud_phase) <=
          1.0;
  if (!adjacent) {
    return result;
  }
  const auto blend = [amount](double from, double to) {
    return from + (to - from) * amount;
  };
  result.viewport_y = blend(previous.viewport_y, current.viewport_y);
  result.viewport_height =
      blend(previous.viewport_height, current.viewport_height);
  result.normal_hud_phase =
      blend(previous.normal_hud_phase, current.normal_hud_phase);
  return result;
}

// Interpolates between the exact integer-divided values authored by
// FUN_800410d0. Integer phases therefore remain bit-for-bit retail while
// intermediate render frames move smoothly at the configured refresh rate.
[[nodiscard]] constexpr double
retailHudInterpolatedExtent(double phase, int maximum_extent) noexcept {
  constexpr auto maximum_phase = 12;
  if (maximum_extent <= 0) {
    return 0.0;
  }
  const auto clamped =
      std::clamp(phase, 0.0, static_cast<double>(maximum_phase));
  const auto lower = static_cast<int>(clamped);
  const auto upper = std::min(lower + 1, maximum_phase);
  const auto lower_value = maximum_extent * lower / maximum_phase;
  const auto upper_value = maximum_extent * upper / maximum_phase;
  const auto fraction = clamped - static_cast<double>(lower);
  return static_cast<double>(lower_value) +
         static_cast<double>(upper_value - lower_value) * fraction;
}

} // namespace sf::platform
