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

void testStartupAndStableRecovery() {
  sf::platform::AudioOutputStartPolicy policy{2U};
  require(!policy.shouldStart(0U, false) && !policy.shouldStart(1U, false),
          "Audio output started before its initial prebuffer");
  require(policy.shouldStart(2U, false) && policy.started(),
          "Audio output did not start after one complete guest tick");
  require(!policy.shouldStart(2U, true),
          "Playing audio source was redundantly restarted");
  require(!policy.shouldStart(1U, false) && policy.shouldStart(2U, false),
          "Underrun recovery restarted from an unstable one-buffer queue");

  policy.reset();
  require(!policy.started() && !policy.shouldStart(1U, false),
          "Explicit reset retained the previous recovery state");
}

void testRealtimeFrameRingWrapsWithoutBlockingOrReordering() {
  sf::platform::AudioFrameRing<int> ring{5U};
  const std::array first{1, 2, 3, 4};
  require(ring.push(first) == first.size() && ring.size() == first.size(),
          "Realtime audio ring did not accept its initial frames");

  std::array<int, 3U> head{};
  require(ring.pop(head) == head.size() &&
              head == std::array<int, 3U>{1, 2, 3},
          "Realtime audio ring reordered its initial frames");

  const std::array wrapped{5, 6, 7, 8, 9};
  require(ring.push(wrapped) == 4U && ring.size() == ring.capacity(),
          "Realtime audio ring did not bound a wrapped producer burst");
  std::array<int, 6U> tail{};
  require(ring.pop(tail) == 5U &&
              std::ranges::equal(std::span{tail}.first(5U),
                                 std::array<int, 5U>{4, 5, 6, 7, 8}),
          "Realtime audio ring lost ordering across its wrap point");

  require(ring.push(std::array{10, 11, 12}) == 3U, "Ring refill failed");
  ring.clear();
  require(ring.size() == 0U && ring.pop(tail) == 0U,
          "Realtime audio ring retained frames across a stream reset");
}

void testStoppedSourcePrebufferIsNotRecycled() {
  sf::platform::AudioOutputRecyclePolicy policy;
  require(!policy.shouldRecycle(false) && policy.prebuffering(),
          "Initial OpenAL prebuffer was treated as played data");
  policy.playbackStarted();
  require(!policy.prebuffering() && policy.shouldRecycle(true),
          "Playing OpenAL source stopped recycling completed buffers");
  require(policy.shouldDrainStoppedGeneration(false, true),
          "A stopped live generation was not eligible for recovery draining");
  require(policy.shouldRecycle(false) && policy.prebuffering(),
          "First stopped-source observation did not drain the old generation");
  require(!policy.shouldDrainStoppedGeneration(false, true),
          "Fresh recovery buffers were mistaken for the stopped generation");
  require(!policy.shouldRecycle(false),
          "Fresh recovery buffers were recycled before restart");
  policy.playbackStarted();
  require(policy.shouldRecycle(true),
          "Recovered source did not return to normal recycling");
  policy.reset();
  require(policy.prebuffering() && !policy.shouldRecycle(false),
          "Explicit reset did not protect the new prebuffer");
}

void testCatchUpDropsOnlyStalePcm() {
  sf::platform::AudioOutputCatchUpPolicy policy{1U, 3U};
  require(!policy.beginFrame(1U) && policy.retainCompletedUpdate(0U) &&
              !policy.catchingUp(),
          "Normal audio update entered catch-up mode");

  require(policy.beginFrame(4U) && policy.catchingUp(),
          "Audio catch-up did not request one output reset");
  require(!policy.retainCompletedUpdate(3U) && !policy.beginFrame(2U),
          "Historical catch-up PCM was retained or catch-up reset twice");
  require(policy.retainCompletedUpdate(2U) &&
              policy.retainCompletedUpdate(1U) && policy.catchingUp(),
          "Recovery prebuffer PCM was discarded too aggressively");
  require(policy.retainCompletedUpdate(0U) && !policy.catchingUp(),
          "Final catch-up PCM was discarded or catch-up stayed active");

  static_cast<void>(policy.beginFrame(3U));
  policy.reset();
  require(!policy.catchingUp() && policy.retainCompletedUpdate(0U),
          "Explicit audio reset retained catch-up state");

  sf::platform::AudioOutputCatchUpPolicy sliced_policy{6U};
  require(!sliced_policy.beginFrame(4U) && sliced_policy.beginFrame(7U),
          "Normal 120 Hz slices were mistaken for an audio discontinuity");
}

void testExactRetailAudioCadence() {
  sf::platform::AudioOutputCadencePolicy cadence{44'100U, 120U};
  std::size_t first_second{};
  for (auto callback = 0U; callback < 120U; ++callback) {
    const auto frames = cadence.advanceCallback();
    require(frames == (callback % 2U == 0U ? 367U : 368U),
            "44.1 kHz cadence did not alternate fractional callback frames");
    first_second += frames;
  }
  require(first_second == 44'100U,
          "Retail callback cadence drifted over one second");

  cadence.reset();
  std::size_t retail_frame{};
  for (auto callback = 0U; callback < 6U; ++callback) {
    retail_frame += cadence.advanceCallback();
  }
  require(retail_frame == 2'205U,
          "Six retail callbacks did not produce one exact 20 Hz PCM frame");

  sf::platform::AudioOutputCadencePolicy invalid{44'100U, 0U};
  require(invalid.advanceCallback() == 0U,
          "Invalid callback rate produced a PCM budget");
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
    testStartupAndStableRecovery();
    testRealtimeFrameRingWrapsWithoutBlockingOrReordering();
    testStoppedSourcePrebufferIsNotRecycled();
    testCatchUpDropsOnlyStalePcm();
    testExactRetailAudioCadence();
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
