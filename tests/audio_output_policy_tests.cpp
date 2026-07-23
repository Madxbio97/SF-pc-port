#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/platform/audio_output_policy.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void testStartupAndImmediateRecovery() {
  sf::platform::AudioOutputStartPolicy policy{2U};
  require(!policy.shouldStart(0U, false) && !policy.shouldStart(1U, false),
          "Audio output started before its initial prebuffer");
  require(policy.shouldStart(2U, false) && policy.started(),
          "Audio output did not start after one complete guest tick");
  require(!policy.shouldStart(2U, true),
          "Playing audio source was redundantly restarted");
  require(policy.shouldStart(1U, false),
          "Underrun recovery waited for a second prebuffer gap");

  policy.reset();
  require(!policy.started() && !policy.shouldStart(1U, false),
          "Explicit reset retained the previous recovery state");
}

void testBoundedGainRamp() {
  sf::platform::AudioOutputGainPolicy policy{100U, 5U};
  policy.setTargetPercent(0U);
  require(policy.advance(true) == 95U && policy.currentPercent() == 95U,
          "Playing source gain jumped instead of ramping down");
  require(policy.advance(true) == 90U,
          "Playing source gain did not use its fixed downward step");
  require(policy.advance(false) == 0U && policy.gain() == 0.0F,
          "Stopped source did not adopt its target immediately");

  policy.setTargetPercent(12U);
  require(policy.advance(true) == 5U && policy.advance(true) == 10U &&
              policy.advance(true) == 12U,
          "Playing source gain overshot its upward target");
  policy.setTargetPercent(255U);
  require(policy.targetPercent() == 100U && policy.advance(false) == 100U &&
              policy.gain() == 1.0F,
          "Gain target was not clamped to OpenAL's normalized range");
}

void testRetailVolumeMapping() {
  require(sf::game::legacyRetailAudioVolumeFromPercent(0U) == 0U &&
              sf::game::legacyRetailAudioVolumeFromPercent(50U) == 64U &&
              sf::game::legacyRetailAudioVolumeFromPercent(100U) == 127U &&
              sf::game::legacyRetailAudioVolumeFromPercent(255U) == 127U &&
              sf::game::legacyRetailAudioVolumeToPercent(0U) == 0U &&
              sf::game::legacyRetailAudioVolumeToPercent(64U) == 50U &&
              sf::game::legacyRetailAudioVolumeToPercent(127U) == 100U &&
              sf::game::legacyRetailAudioVolumeToPercent(255U) == 100U,
          "Pause volume did not map to the retail 0..127 range");
  const sf::game::LegacyRetailAudioVolumes volumes{
      .sound_effects = 17U,
      .music = 43U,
      .voice_over = 71U,
  };
  require(volumes.valid() &&
              volumes.groups() == std::array<std::uint8_t, 3U>{17U, 43U, 71U},
          "Retail sound groups are not ordered as SFX/Music/Voice-over");
}

void testMovieFrameTimingUsesOneAbsoluteClock() {
  sf::platform::MovieFrameTimingPolicy ntsc_str{15.0};
  require(
      ntsc_str.valid() &&
          std::abs(ntsc_str.frameEndSeconds(10.0, 10.08) - 0.08) < 0.000'001 &&
          std::abs(ntsc_str.frameEndSeconds(10.08, 10.15) - 0.15) < 0.000'001,
      "STR frame pacing ignored decoded presentation timestamps");

  sf::platform::MovieFrameTimingPolicy missing_timestamps{15.0};
  require(std::abs(missing_timestamps.frameEndSeconds(0.0, std::nullopt) -
                   (1.0 / 15.0)) < 0.000'001 &&
              std::abs(missing_timestamps.frameEndSeconds(0.0, 0.0) -
                       (2.0 / 15.0)) < 0.000'001,
          "Missing or repeated STR timestamps broke fixed-rate fallback");

  require(!sf::platform::MovieFrameTimingPolicy{0.0}.valid() &&
              !sf::platform::MovieFrameTimingPolicy{
                  std::numeric_limits<double>::infinity()}
                   .valid() &&
              !sf::platform::MovieFrameTimingPolicy{121.0}.valid(),
          "Invalid STR frame rate was accepted");

  sf::platform::MovieFrameTimingPolicy discontinuous{10.0};
  require(std::abs(discontinuous.frameEndSeconds(5.0, 5.1) - 0.1) < 0.000'001 &&
              std::abs(discontinuous.frameEndSeconds(5.1, 50.0) - 0.2) <
                  0.000'001 &&
              std::abs(discontinuous.frameEndSeconds(50.0, 50.1) - 0.3) <
                  0.000'001,
          "Large STR PTS discontinuity froze or permanently desynchronized "
          "the movie clock");

  sf::platform::MovieFrameTimingPolicy nonpositive{10.0};
  require(std::abs(nonpositive.frameEndSeconds(3.0, 3.1) - 0.1) < 0.000'001 &&
              std::abs(nonpositive.frameEndSeconds(3.1, 3.0) - 0.2) <
                  0.000'001 &&
              std::abs(nonpositive.frameEndSeconds(3.0, 3.1) - 0.3) < 0.000'001,
          "Repeated or backwards STR PTS bypassed fixed-rate recovery");

  sf::platform::MovieFrameTimingPolicy nonfinite{10.0};
  require(std::abs(nonfinite.frameEndSeconds(
                       std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity()) -
                   0.1) < 0.000'001 &&
              std::abs(nonfinite.frameEndSeconds(
                           std::numeric_limits<double>::quiet_NaN(), 7.0) -
                       0.2) < 0.000'001,
          "Non-finite STR PTS escaped deterministic fallback");
}

} // namespace

int main() {
  try {
    testStartupAndImmediateRecovery();
    testBoundedGainRamp();
    testRetailVolumeMapping();
    testMovieFrameTimingUsesOneAbsoluteClock();
  } catch (const std::exception &error) {
    std::cerr << "audio output policy tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "audio output policy tests passed\n";
  return 0;
}
