#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sf::platform {

// Pure, deterministic start/recovery policy used by the OpenAL sink. Initial
// playback waits for a small prebuffer; once playback has begun, an underrun
// restarts immediately with the first available buffer instead of inserting
// another full prebuffer-sized silence gap.
class AudioOutputStartPolicy final {
public:
  explicit AudioOutputStartPolicy(std::size_t startup_buffers) noexcept
      : startup_buffers_(std::max<std::size_t>(1U, startup_buffers)) {}

  [[nodiscard]] bool shouldStart(std::size_t queued_buffers,
                                 bool playing) noexcept {
    if (playing) {
      started_ = true;
      return false;
    }
    if (queued_buffers == 0U) {
      return false;
    }
    if (started_ || queued_buffers >= startup_buffers_) {
      started_ = true;
      return true;
    }
    return false;
  }

  void reset() noexcept { started_ = false; }
  [[nodiscard]] bool started() const noexcept { return started_; }

private:
  std::size_t startup_buffers_{};
  bool started_{};
};

// Smooths host-owned source gain without touching queued PCM. A stopped
// source adopts the target immediately, while a playing source moves by a
// bounded percentage on each presentation update.
class AudioOutputGainPolicy final {
public:
  explicit AudioOutputGainPolicy(std::uint8_t initial_percent = 100U,
                                 std::uint8_t maximum_step = 5U) noexcept
      : current_percent_(clamp(initial_percent)),
        target_percent_(current_percent_),
        maximum_step_(std::max<std::uint8_t>(1U, clamp(maximum_step))) {}

  void setTargetPercent(std::uint8_t percent) noexcept {
    target_percent_ = clamp(percent);
  }

  [[nodiscard]] std::uint8_t advance(bool playing) noexcept {
    if (!playing) {
      current_percent_ = target_percent_;
      return current_percent_;
    }
    if (current_percent_ < target_percent_) {
      const auto remaining =
          static_cast<std::uint8_t>(target_percent_ - current_percent_);
      current_percent_ = static_cast<std::uint8_t>(
          current_percent_ + std::min(maximum_step_, remaining));
    } else if (current_percent_ > target_percent_) {
      const auto remaining =
          static_cast<std::uint8_t>(current_percent_ - target_percent_);
      current_percent_ = static_cast<std::uint8_t>(
          current_percent_ - std::min(maximum_step_, remaining));
    }
    return current_percent_;
  }

  [[nodiscard]] std::uint8_t currentPercent() const noexcept {
    return current_percent_;
  }
  [[nodiscard]] std::uint8_t targetPercent() const noexcept {
    return target_percent_;
  }
  [[nodiscard]] float gain() const noexcept {
    return static_cast<float>(current_percent_) / 100.0F;
  }

private:
  [[nodiscard]] static constexpr std::uint8_t
  clamp(std::uint8_t percent) noexcept {
    return percent > 100U ? 100U : percent;
  }

  std::uint8_t current_percent_{};
  std::uint8_t target_percent_{};
  std::uint8_t maximum_step_{1U};
};

// Uses one absolute movie clock instead of sleeping for a full frame after
// every decode/upload.  The latter accumulates decoder and GPU upload time,
// making STR video drift behind XA audio over long sequences.
class MovieFrameTimingPolicy final {
public:
  explicit MovieFrameTimingPolicy(double frames_per_second) noexcept
      : frames_per_second_(frames_per_second) {}

  [[nodiscard]] bool valid() const noexcept {
    return std::isfinite(frames_per_second_) && frames_per_second_ > 0.0 &&
           frames_per_second_ <= 120.0;
  }

  [[nodiscard]] double
  frameEndSeconds(double frame_timestamp_seconds,
                  std::optional<double> next_frame_timestamp_seconds) noexcept {
    if (!valid()) {
      return 0.0;
    }
    const auto timestamp_valid = [](double timestamp) noexcept {
      return std::isfinite(timestamp) && timestamp >= 0.0;
    };
    const auto frame_step = 1.0 / frames_per_second_;
    const auto fallback = last_deadline_seconds_ + frame_step;
    if (!timestamp_origin_seconds_) {
      if (timestamp_valid(frame_timestamp_seconds)) {
        timestamp_origin_seconds_ = frame_timestamp_seconds;
      } else if (next_frame_timestamp_seconds &&
                 timestamp_valid(*next_frame_timestamp_seconds)) {
        timestamp_origin_seconds_ = *next_frame_timestamp_seconds - fallback;
      }
    }
    auto deadline = fallback;
    if (timestamp_origin_seconds_ && next_frame_timestamp_seconds &&
        timestamp_valid(*next_frame_timestamp_seconds)) {
      const auto timestamp_deadline =
          *next_frame_timestamp_seconds - *timestamp_origin_seconds_;
      constexpr auto maximum_timestamp_step_frames = 4.0;
      const auto timestamp_step = timestamp_deadline - last_deadline_seconds_;
      if (std::isfinite(timestamp_deadline) && timestamp_step > 0.0 &&
          timestamp_step <= frame_step * maximum_timestamp_step_frames) {
        deadline = timestamp_deadline;
      } else {
        // A repeated/backwards PTS or a discontinuity must not freeze the
        // last decoded image. Rebase at the deterministic fixed-rate
        // deadline so later valid timestamps can resume absolute pacing.
        timestamp_origin_seconds_ = *next_frame_timestamp_seconds - fallback;
      }
    }
    last_deadline_seconds_ = deadline;
    return deadline;
  }

private:
  double frames_per_second_{};
  std::optional<double> timestamp_origin_seconds_;
  double last_deadline_seconds_{};
};

} // namespace sf::platform
