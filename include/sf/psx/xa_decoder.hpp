#pragma once

#include "sf/psx/spu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sf::psx {

enum class XaDecodeStatus : std::uint8_t {
  decoded,
  invalid_sector_size,
  invalid_sync,
  not_mode2,
  subheader_mismatch,
  not_form2_audio,
  unsupported_coding,
  output_too_small,
};

struct XaSectorFormat {
  std::uint32_t sample_rate_hz{};
  std::uint8_t file{};
  std::uint8_t channel{};
  std::uint8_t submode{};
  std::uint8_t coding{};
  std::uint8_t channel_count{};
  std::uint8_t bits_per_sample{};

  bool operator==(const XaSectorFormat &) const = default;
};

struct XaDecodeResult {
  XaDecodeStatus status{XaDecodeStatus::invalid_sector_size};
  XaSectorFormat format{};
  std::size_t frames_written{};
  std::size_t frames_required{};

  [[nodiscard]] bool succeeded() const noexcept {
    return status == XaDecodeStatus::decoded;
  }
};

// Pointer-free mutable state. Predictor and resampler history are reset only
// by the CD controller's explicit stream reset; Setfilter/file/channel/coding
// hand-offs remain continuous, as on hardware and in DuckStation.
struct XaDecoderState {
  // [left/right][newest/older]
  std::array<std::array<std::int32_t, 2>, 2> predictor_samples{};
  // Hardware XA 37.8 -> 44.1 kHz zig-zag interpolation history.
  std::array<std::array<std::int16_t, 32>, 2> resample_ring{};
  std::uint32_t resample_phase{};
  std::uint8_t resample_position{};
  std::uint8_t resample_sixstep{6U};
  std::uint8_t active_file{};
  std::uint8_t active_channel{};
  std::uint8_t active_coding{};
  std::uint8_t active{};

  bool operator==(const XaDecoderState &) const = default;
};

// Deterministic decoder for one 2352-byte CD-ROM XA Mode 2 Form 2 audio
// sector. Output is signed stereo PCM at the native SPU rate (44.1 kHz).
class XaAudioDecoder final {
public:
  static constexpr std::size_t raw_sector_size = 2352U;
  static constexpr std::uint32_t output_sample_rate_hz = 44'100U;
  static constexpr std::size_t maximum_output_frames = 9408U;

  XaAudioDecoder() noexcept { reset(); }

  void reset() noexcept;

  [[nodiscard]] XaDecodeResult
  decodeSector(std::span<const std::byte> raw_sector,
               std::span<SpuPcmFrame> output) noexcept;

  [[nodiscard]] XaDecoderState captureState() const noexcept { return state_; }
  [[nodiscard]] bool validateState(const XaDecoderState &state) const noexcept;
  [[nodiscard]] bool restoreState(const XaDecoderState &state) noexcept;

private:
  XaDecoderState state_{};
};

} // namespace sf::psx
