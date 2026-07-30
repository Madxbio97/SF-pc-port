#include "sf/game/mission_start.hpp"

#include <algorithm>
#include <cmath>

namespace sf::game {

bool MissionStartGate::update(bool confirm_down,
                              bool text_animation_complete) noexcept {
  if (phase_ == MissionStartPhase::accepted) {
    return true;
  }
  if (!text_animation_complete) {
    phase_ = MissionStartPhase::waiting_for_release;
    released_frames_ = 0U;
    return false;
  }
  if (phase_ == MissionStartPhase::waiting_for_release) {
    if (confirm_down) {
      released_frames_ = 0U;
      return false;
    }
    ++released_frames_;
    if (released_frames_ >= release_frames) {
      phase_ = MissionStartPhase::waiting_for_confirm;
    }
    return false;
  }
  if (!confirm_down) {
    return false;
  }
  phase_ = MissionStartPhase::accepted;
  return true;
}

std::uint8_t MissionStartGate::promptBrightness(std::uint32_t frame) noexcept {
  constexpr auto cycle_ticks = 2U * (prompt_fade_ticks + prompt_hold_ticks);
  const auto phase = frame % cycle_ticks;
  if (phase < prompt_fade_ticks) {
    return static_cast<std::uint8_t>(phase * prompt_peak_brightness /
                                     prompt_fade_ticks);
  }
  if (phase < prompt_fade_ticks + prompt_hold_ticks) {
    return prompt_peak_brightness;
  }
  if (phase < 2U * prompt_fade_ticks + prompt_hold_ticks) {
    const auto fade_phase = phase - prompt_fade_ticks - prompt_hold_ticks;
    return static_cast<std::uint8_t>((prompt_fade_ticks - fade_phase) *
                                     prompt_peak_brightness /
                                     prompt_fade_ticks);
  }
  return 0U;
}

std::uint8_t
MissionStartGate::fadeOutIntensity(double elapsed_seconds) noexcept {
  if (!(elapsed_seconds > 0.0)) {
    return 0U;
  }
  if (elapsed_seconds >= fade_out_seconds) {
    return 0xffU;
  }
  const auto progress =
      std::clamp(elapsed_seconds / fade_out_seconds, 0.0, 1.0);
  return static_cast<std::uint8_t>(std::lround(progress * 255.0));
}

} // namespace sf::game
