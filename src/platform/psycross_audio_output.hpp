#pragma once

#include "sf/platform/audio_output_policy.hpp"
#include "sf/psx/spu.hpp"

#include <AL/al.h>
#include <AL/alext.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::platform::detail {

[[nodiscard]] bool psyCrossAudioDiagnosticsEnabled() noexcept;

enum class PsyCrossAudioStreamKind : std::uint8_t {
  continuous,
  one_shot,
};

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
  explicit PsyCrossAudioOutput(std::size_t minimum_start_buffers = 1U,
                               std::string diagnostic_name = "gameplay",
                               PsyCrossAudioStreamKind stream_kind =
                                   PsyCrossAudioStreamKind::continuous);
  ~PsyCrossAudioOutput();

  PsyCrossAudioOutput(const PsyCrossAudioOutput &) = delete;
  PsyCrossAudioOutput &operator=(const PsyCrossAudioOutput &) = delete;

  void queue(std::span<const psx::SpuPcmFrame> frames);
  void flush();
  void update();
  void setGainPercent(std::uint8_t percent);
  void logDiagnostics(std::string_view context) const noexcept;
  void reset(std::string_view reason = "explicit") noexcept;

private:
  // 128 stereo frames are 2.9 ms at the native SPU rate. The callback sink
  // consumes a lock-free jitter ring, while the producer keeps any temporarily
  // unwritten frames in FIFO staging. No generated SPU frame is replaced by a
  // newer frame merely because the renderer or audio device stalled.
  static constexpr std::size_t frames_per_buffer = 128U;
  static constexpr std::size_t maximum_queued_buffers = 24U;
  static constexpr std::size_t maximum_stream_frames =
      psx::Spu::sample_rate / 2U;
  static constexpr std::size_t maximum_staged_frames =
      psx::Spu::pcm_queue_capacity;
  static constexpr std::size_t restart_fade_frames =
      psx::Spu::sample_rate / 200U;
  void collectProcessed();
  void fillCallbackRing();
  void uploadReadyBuffers(bool flush_partial);
  void uploadBuffer(std::span<const psx::SpuPcmFrame> frames);
  void compactStaging();
  void recycleProcessedBuffers(ALint processed);
  void applyGainStep();
  void startIfNeeded();
  [[nodiscard]] std::size_t queuedBufferCount() const;
  [[nodiscard]] static ALsizei AL_APIENTRY
  streamCallback(ALvoid *user, ALvoid *samples, ALsizei byte_count) noexcept;
  [[nodiscard]] ALsizei fillStream(ALvoid *samples,
                                   ALsizei byte_count) noexcept;

  PsyCrossAudioContext context_;
  std::string diagnostic_name_;
  PsyCrossAudioStreamKind stream_kind_{PsyCrossAudioStreamKind::continuous};
  ALuint source_{};
  ALuint callback_buffer_{};
  LPALBUFFERCALLBACKSOFT buffer_callback_{};
  AudioFrameRing<psx::SpuPcmFrame> stream_frames_{maximum_stream_frames};
  std::size_t minimum_start_frames_{};
  std::vector<ALuint> buffers_;
  std::vector<ALuint> available_;
  std::vector<psx::SpuPcmFrame> staged_frames_;
  std::vector<psx::SpuPcmFrame> upload_scratch_;
  std::size_t staged_offset_{};
  std::atomic<std::size_t> fade_in_frames_remaining_{};
  std::atomic<std::uint64_t> callback_frames_read_{};
  std::atomic<std::uint64_t> callback_silence_frames_{};
  std::atomic<std::uint64_t> callback_underruns_{};
  std::atomic<bool> callback_starved_{};
  std::uint64_t submitted_frames_{};
  std::uint64_t uploaded_frames_{};
  std::uint64_t recycled_buffers_{};
  std::uint64_t source_starts_{};
  std::uint64_t source_underruns_{};
  std::uint64_t source_resets_{};
  std::uint64_t reported_callback_underruns_{};
  bool underrun_latched_{};
  AudioOutputStartPolicy start_policy_{2U};
  AudioOutputRecyclePolicy recycle_policy_{};
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
  void reset() noexcept { output_.reset("ui-reset"); }

private:
  [[nodiscard]] std::span<const psx::SpuPcmFrame>
  cueFrames(PsyCrossUiCue cue) const noexcept;

  PsyCrossAudioOutput output_{1U, "ui", PsyCrossAudioStreamKind::one_shot};
  std::array<std::vector<psx::SpuPcmFrame>, 3U> cues_;
};

} // namespace sf::platform::detail
