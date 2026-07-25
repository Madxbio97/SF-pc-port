#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sf::platform {

// Single-producer/single-consumer PCM ring used between the emulation thread
// and OpenAL Soft's realtime callback. The storage is allocated once; neither
// push nor pop allocates, blocks, or takes a lock. A short producer stall can
// therefore become an exact interval of silence without stopping/restarting
// the device source or replaying stale buffers afterwards.
template <typename Frame> class AudioFrameRing final {
public:
  explicit AudioFrameRing(std::size_t capacity)
      : frames_(std::max<std::size_t>(capacity, 1U)) {}

  [[nodiscard]] std::size_t push(std::span<const Frame> source) noexcept {
    const auto write = write_position_.load(std::memory_order_relaxed);
    const auto read = read_position_.load(std::memory_order_acquire);
    const auto occupied = static_cast<std::size_t>(write - read);
    const auto writable = frames_.size() - std::min(occupied, frames_.size());
    const auto count = std::min(source.size(), writable);
    if (count == 0U) {
      return 0U;
    }
    const auto offset = static_cast<std::size_t>(write % frames_.size());
    const auto first = std::min(count, frames_.size() - offset);
    std::copy_n(source.begin(), first, frames_.begin() +
                                        static_cast<std::ptrdiff_t>(offset));
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(first),
                count - first, frames_.begin());
    write_position_.store(write + count, std::memory_order_release);
    return count;
  }

  [[nodiscard]] std::size_t pop(std::span<Frame> destination) noexcept {
    const auto read = read_position_.load(std::memory_order_relaxed);
    const auto write = write_position_.load(std::memory_order_acquire);
    const auto readable = static_cast<std::size_t>(write - read);
    const auto count = std::min(destination.size(), readable);
    if (count == 0U) {
      return 0U;
    }
    const auto offset = static_cast<std::size_t>(read % frames_.size());
    const auto first = std::min(count, frames_.size() - offset);
    std::copy_n(frames_.begin() + static_cast<std::ptrdiff_t>(offset), first,
                destination.begin());
    std::copy_n(frames_.begin(), count - first,
                destination.begin() + static_cast<std::ptrdiff_t>(first));
    read_position_.store(read + count, std::memory_order_release);
    return count;
  }

  void clear() noexcept {
    read_position_.store(write_position_.load(std::memory_order_acquire),
                         std::memory_order_release);
  }

  [[nodiscard]] std::size_t size() const noexcept {
    const auto write = write_position_.load(std::memory_order_acquire);
    const auto read = read_position_.load(std::memory_order_acquire);
    return std::min(static_cast<std::size_t>(write - read), frames_.size());
  }
  [[nodiscard]] std::size_t capacity() const noexcept { return frames_.size(); }

private:
  std::vector<Frame> frames_;
  std::atomic<std::uint64_t> read_position_{};
  std::atomic<std::uint64_t> write_position_{};
};

// Pure, deterministic start/recovery policy used by the OpenAL sink. Initial
// playback waits for a small prebuffer. Recovery uses the same threshold: an
// immediate one-buffer restart repeatedly underruns when the renderer submits
// audio less often than the device consumes it and is heard as crackling.
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
    if (queued_buffers >= startup_buffers_) {
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

// OpenAL reports every buffer attached to a stopped source as processed. A
// recovery queue therefore needs one explicit drain of the exhausted playback
// generation followed by a protected prebuffer phase; otherwise every fresh
// block is recycled before the source can reach its restart threshold.
class AudioOutputRecyclePolicy final {
public:
  [[nodiscard]] bool shouldRecycle(bool playing) noexcept {
    if (playing) {
      prebuffering_ = false;
      return true;
    }
    if (prebuffering_) {
      return false;
    }
    prebuffering_ = true;
    return true;
  }

  void playbackStarted() noexcept { prebuffering_ = false; }
  void reset() noexcept { prebuffering_ = true; }
  [[nodiscard]] bool prebuffering() const noexcept { return prebuffering_; }
  [[nodiscard]] bool
  shouldDrainStoppedGeneration(bool playing,
                               bool has_processed_buffers) const noexcept {
    return !playing && has_processed_buffers && !prebuffering_;
  }

private:
  bool prebuffering_{true};
};

// Keeps a short, current recovery window after a renderer stall. Retaining
// only one 120 Hz slice cannot satisfy OpenAL's recovery prebuffer and turns a
// transient stall into a longer silence; three slices cover the eight
// 128-frame buffers used by the gameplay source without preserving a stale
// historical tail.
class AudioOutputCatchUpPolicy final {
public:
  explicit AudioOutputCatchUpPolicy(std::size_t maximum_coherent_updates = 1U,
                                    std::size_t recovery_updates = 3U) noexcept
      : maximum_coherent_updates_(
            std::max<std::size_t>(1U, maximum_coherent_updates)),
        recovery_updates_(std::max<std::size_t>(1U, recovery_updates)) {}

  // Returns true once when stale guest PCM must be discarded. The live host
  // queue remains attached so a long frame does not manufacture an underrun.
  [[nodiscard]] bool beginFrame(std::size_t pending_updates) noexcept {
    if (pending_updates <= maximum_coherent_updates_) {
      return false;
    }
    const auto reset_output = !catching_up_;
    catching_up_ = true;
    return reset_output;
  }

  // Returns true for the newest recovery window. Older catch-up blocks are
  // deliberately discarded, while the final few slices can immediately meet
  // the source's recovery prebuffer.
  [[nodiscard]] bool
  retainCompletedUpdate(std::size_t pending_updates) noexcept {
    if (!catching_up_) {
      return true;
    }
    if (pending_updates >= recovery_updates_) {
      return false;
    }
    if (pending_updates == 0U) {
      catching_up_ = false;
    }
    return true;
  }

  void reset() noexcept { catching_up_ = false; }
  [[nodiscard]] bool catchingUp() const noexcept { return catching_up_; }

private:
  std::size_t maximum_coherent_updates_{1U};
  std::size_t recovery_updates_{3U};
  bool catching_up_{};
};

// Converts the retail callback cadence into an exact host sample budget.
// Guest CPU work may advance hardware by a variable number of cycles inside a
// fixed 20 Hz update. The realtime sink must nevertheless submit exactly one
// second of PCM per second instead of accumulating that excess as latency.
class AudioOutputCadencePolicy final {
public:
  constexpr AudioOutputCadencePolicy(std::uint32_t sample_rate,
                                     std::uint32_t callback_hz) noexcept
      : whole_frames_(callback_hz == 0U ? 0U : sample_rate / callback_hz),
        remainder_per_callback_(callback_hz == 0U ? 0U
                                                  : sample_rate % callback_hz),
        callback_hz_(callback_hz) {}

  [[nodiscard]] constexpr std::size_t advanceCallback() noexcept {
    if (callback_hz_ == 0U) {
      return 0U;
    }
    remainder_ += remainder_per_callback_;
    const auto carried_frames = remainder_ / callback_hz_;
    remainder_ %= callback_hz_;
    return static_cast<std::size_t>(whole_frames_ + carried_frames);
  }

  constexpr void reset() noexcept { remainder_ = 0U; }

private:
  std::uint32_t whole_frames_{};
  std::uint32_t remainder_per_callback_{};
  std::uint32_t callback_hz_{};
  std::uint32_t remainder_{};
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
