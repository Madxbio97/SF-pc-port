#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sf::platform {

// Measures completed host presentations independently from the fixed 20 Hz
// guest simulation. A time window keeps the result equally stable at 30,
// 60, 120 and 240 Hz, while a gentle time-based filter avoids a flickering
// counter on variable-refresh displays.
class PresentationFrameMeter final {
public:
  static constexpr double reporting_interval_seconds = 0.5;
  static constexpr double smoothing_time_constant_seconds = 0.75;
  static constexpr double maximum_contiguous_sample_seconds = 0.5;

  void advance(double elapsed_seconds,
               std::uint32_t completed_simulation_frames = 0U) noexcept {
    if (!std::isfinite(elapsed_seconds) || elapsed_seconds <= 0.0) {
      return;
    }
    if (elapsed_seconds > maximum_contiguous_sample_seconds) {
      resetWindow();
      return;
    }

    window_seconds_ += elapsed_seconds;
    ++window_frames_;
    window_simulation_frames_ += completed_simulation_frames;
    if (window_seconds_ + 1.0e-9 < reporting_interval_seconds) {
      return;
    }

    const auto measured = static_cast<double>(window_frames_) / window_seconds_;
    const auto measured_simulation =
        static_cast<double>(window_simulation_frames_) / window_seconds_;
    if (!ready_) {
      frames_per_second_ = measured;
      simulation_frames_per_second_ = measured_simulation;
      ready_ = true;
    } else {
      const auto alpha =
          1.0 - std::exp(-window_seconds_ / smoothing_time_constant_seconds);
      frames_per_second_ += (measured - frames_per_second_) * alpha;
      simulation_frames_per_second_ +=
          (measured_simulation - simulation_frames_per_second_) * alpha;
    }
    rebuildText();
    resetWindow();
  }

  void reset() noexcept {
    resetWindow();
    frames_per_second_ = 0.0;
    simulation_frames_per_second_ = 0.0;
    ready_ = false;
    text_size_ = 0U;
    text_[0] = '\0';
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] double framesPerSecond() const noexcept {
    return frames_per_second_;
  }
  [[nodiscard]] double simulationFramesPerSecond() const noexcept {
    return simulation_frames_per_second_;
  }
  [[nodiscard]] double frameMilliseconds() const noexcept {
    return ready_ && frames_per_second_ > 0.0 ? 1000.0 / frames_per_second_
                                              : 0.0;
  }
  [[nodiscard]] std::string_view text() const noexcept {
    return {text_.data(), text_size_};
  }

private:
  void resetWindow() noexcept {
    window_seconds_ = 0.0;
    window_frames_ = 0U;
    window_simulation_frames_ = 0U;
  }

  void rebuildText() noexcept {
    auto *cursor = text_.data();
    auto *const end = text_.data() + text_.size() - 1U;
    const auto append = [&](std::string_view value) {
      const auto count = std::min<std::size_t>(
          value.size(), static_cast<std::size_t>(end - cursor));
      std::copy_n(value.data(), count, cursor);
      cursor += count;
    };
    const auto append_unsigned = [&](std::uint32_t value) {
      const auto converted = std::to_chars(cursor, end, value);
      if (converted.ec == std::errc{}) {
        cursor = converted.ptr;
      }
    };

    const auto rounded_fps = static_cast<std::uint32_t>(
        std::clamp(std::lround(frames_per_second_), 0L, 9999L));
    const auto rounded_simulation_fps = static_cast<std::uint32_t>(
        std::clamp(std::lround(simulation_frames_per_second_), 0L, 9999L));
    const auto frame_tenths = static_cast<std::uint32_t>(
        std::clamp(std::lround(frameMilliseconds() * 10.0), 0L, 99999L));
    append("FPS ");
    append_unsigned(rounded_fps);
    append("  LOGIC ");
    append_unsigned(rounded_simulation_fps);
    append("  ");
    append_unsigned(frame_tenths / 10U);
    if (cursor < end) {
      *cursor++ = '.';
    }
    append_unsigned(frame_tenths % 10U);
    append(" MS");
    *cursor = '\0';
    text_size_ = static_cast<std::size_t>(cursor - text_.data());
  }

  double window_seconds_{};
  std::uint32_t window_frames_{};
  std::uint32_t window_simulation_frames_{};
  double frames_per_second_{};
  double simulation_frames_per_second_{};
  bool ready_{};
  std::array<char, 48U> text_{};
  std::size_t text_size_{};
};

} // namespace sf::platform
