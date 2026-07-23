#pragma once

#include "sf/psx/spu.hpp"
#include "sf/platform/audio_output_policy.hpp"

#include <AL/al.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace sf::platform::detail {

// Reference-counted owner for PsyCross's OpenAL context. Gameplay and FMV
// create independent sources but share this one process-wide context.
class PsyCrossAudioContext final {
public:
  PsyCrossAudioContext();
  ~PsyCrossAudioContext();

  PsyCrossAudioContext(const PsyCrossAudioContext &) = delete;
  PsyCrossAudioContext &operator=(const PsyCrossAudioContext &) = delete;
  PsyCrossAudioContext(PsyCrossAudioContext &&) = delete;
  PsyCrossAudioContext &operator=(PsyCrossAudioContext &&) = delete;
};

// Bounded real-time sink for emulated 44.1 kHz SPU stereo frames.
class PsyCrossAudioOutput final {
public:
  explicit PsyCrossAudioOutput(std::size_t minimum_start_buffers = 2U);
  ~PsyCrossAudioOutput();

  PsyCrossAudioOutput(const PsyCrossAudioOutput &) = delete;
  PsyCrossAudioOutput &operator=(const PsyCrossAudioOutput &) = delete;

  void queue(std::span<const psx::SpuPcmFrame> frames);
  void flush();
  void update();
  void setGainPercent(std::uint8_t percent);
  void reset() noexcept;

private:
  static constexpr std::size_t frames_per_buffer = 1024U;
  static constexpr std::size_t maximum_queued_buffers = 16U;
  static constexpr std::size_t maximum_staged_frames = psx::Spu::sample_rate;

  void collectProcessed();
  void uploadReadyBuffers(bool flush_partial);
  void uploadBuffer(std::span<const psx::SpuPcmFrame> frames);
  void compactStaging();
  void applyGainStep();
  void startIfNeeded();
  [[nodiscard]] std::size_t queuedBufferCount() const;

  PsyCrossAudioContext context_;
  ALuint source_{};
  std::vector<ALuint> buffers_;
  std::vector<ALuint> available_;
  std::vector<psx::SpuPcmFrame> staged_frames_;
  std::size_t staged_offset_{};
  AudioOutputStartPolicy start_policy_{2U};
  AudioOutputGainPolicy gain_policy_{};
};

enum class PsyCrossUiCue {
  navigate,
  confirm,
  cancel,
};

// Small native cues for native-owned title/pause UI. They deliberately use a
// separate OpenAL source so guest SPU/XA timing and volume remain untouched.
class PsyCrossUiAudio final {
public:
  explicit PsyCrossUiAudio(const std::filesystem::path &cue_path);

  void play(PsyCrossUiCue cue);
  void setVolumePercent(std::uint8_t percent) {
    output_.setGainPercent(percent);
  }
  void update() { output_.update(); }
  void reset() noexcept { output_.reset(); }

private:
  [[nodiscard]] std::span<const psx::SpuPcmFrame>
  cueFrames(PsyCrossUiCue cue) const noexcept;

  PsyCrossAudioOutput output_{1U};
  std::array<std::vector<psx::SpuPcmFrame>, 3U> cues_;
};

} // namespace sf::platform::detail
