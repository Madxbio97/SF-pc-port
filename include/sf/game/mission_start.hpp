#pragma once

#include <cstdint>

namespace sf::game {

enum class MissionStartPhase {
  waiting_for_release,
  waiting_for_confirm,
  accepted,
};

class MissionStartGate final {
public:
  static constexpr std::uint32_t release_frames = 2U;
  static constexpr std::uint32_t prompt_fade_ticks = 8U;
  static constexpr std::uint32_t prompt_hold_ticks = 24U;
  static constexpr std::uint8_t prompt_peak_brightness = 200U;
  static constexpr double fade_out_seconds = 0.25;

  [[nodiscard]] bool update(bool confirm_down,
                            bool text_animation_complete = true) noexcept;
  [[nodiscard]] MissionStartPhase phase() const noexcept { return phase_; }
  [[nodiscard]] static std::uint8_t
  promptBrightness(std::uint32_t frame) noexcept;
  [[nodiscard]] static std::uint8_t
  fadeOutIntensity(double elapsed_seconds) noexcept;

private:
  MissionStartPhase phase_{MissionStartPhase::waiting_for_release};
  std::uint32_t released_frames_{};
};

} // namespace sf::game
