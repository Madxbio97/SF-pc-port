#include "sf/psx/xa_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace sf::psx {
namespace {

constexpr std::size_t sync_size = 12U;
constexpr std::size_t mode_offset = 15U;
constexpr std::size_t subheader_offset = 16U;
constexpr std::size_t subheader_copy_offset = 20U;
constexpr std::size_t audio_payload_offset = 24U;
constexpr std::size_t sound_group_count = 18U;
constexpr std::size_t sound_group_size = 128U;
constexpr std::size_t samples_per_block = 28U;

constexpr std::uint8_t mode2_value = 2U;
constexpr std::uint8_t submode_audio = 1U << 2U;
constexpr std::uint8_t submode_form2 = 1U << 5U;
constexpr std::uint8_t coding_stereo = 1U << 0U;
constexpr std::uint8_t coding_half_rate = 1U << 2U;
constexpr std::uint8_t coding_eight_bit = 1U << 4U;
constexpr std::uint8_t coding_reserved_mask =
    (1U << 1U) | (1U << 3U) | (1U << 5U) | (1U << 7U);

constexpr std::array<std::int32_t, 4> predictor_positive{0, 60, 115, 98};
constexpr std::array<std::int32_t, 4> predictor_negative{0, 0, -52, -55};

// PlayStation CD-XA 6:7 interpolation coefficients. These are the hardware
// tables used by DuckStation rather than a host-side sample duplicator.
constexpr std::array<std::array<std::int16_t, 29>, 7> xa_zigzag_tables{{
    {{0, 0x0, 0x0, 0x0, 0x0, -0x0002, 0x000a, -0x0022, 0x0041,
      -0x0054, 0x0034, 0x0009, -0x010a, 0x0400, -0x0a78, 0x234c,
      0x6794, -0x1780, 0x0bcd, -0x0623, 0x0350, -0x016d, 0x006b,
      0x000a, -0x0010, 0x0011, -0x0008, 0x0003, -0x0001}},
    {{0, 0x0, 0x0, -0x0002, 0x0, 0x0003, -0x0013, 0x003c, -0x004b,
      0x00a2, -0x00e3, 0x0132, -0x0043, -0x0267, 0x0c9d, 0x74bb,
      -0x11b4, 0x09b8, -0x05bf, 0x0372, -0x01a8, 0x00a6, -0x001b,
      0x0005, 0x0006, -0x0008, 0x0003, -0x0001, 0x0}},
    {{0, 0x0, -0x0001, 0x0003, -0x0002, -0x0005, 0x001f, -0x004a,
      0x00b3, -0x0192, 0x02b1, -0x039e, 0x04f8, -0x05a6, 0x7939,
      -0x05a6, 0x04f8, -0x039e, 0x02b1, -0x0192, 0x00b3, -0x004a,
      0x001f, -0x0005, -0x0002, 0x0003, -0x0001, 0x0, 0x0}},
    {{0, -0x0001, 0x0003, -0x0008, 0x0006, 0x0005, -0x001b, 0x00a6,
      -0x01a8, 0x0372, -0x05bf, 0x09b8, -0x11b4, 0x74bb, 0x0c9d,
      -0x0267, -0x0043, 0x0132, -0x00e3, 0x00a2, -0x004b, 0x003c,
      -0x0013, 0x0003, 0x0, -0x0002, 0x0, 0x0, 0x0}},
    {{-0x0001, 0x0003, -0x0008, 0x0011, -0x0010, 0x000a, 0x006b,
      -0x016d, 0x0350, -0x0623, 0x0bcd, -0x1780, 0x6794, 0x234c,
      -0x0a78, 0x0400, -0x010a, 0x0009, 0x0034, -0x0054, 0x0041,
      -0x0022, 0x000a, -0x0001, 0x0, 0x0001, 0x0, 0x0, 0x0}},
    {{0x0002, -0x0008, 0x0010, -0x0023, 0x002b, 0x001a, -0x00eb,
      0x027b, -0x0548, 0x0afa, -0x16fa, 0x53e0, 0x3c07, -0x1249,
      0x080e, -0x0347, 0x015b, -0x0044, -0x0017, 0x0046, -0x0023,
      0x0011, -0x0005, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0}},
    {{-0x0005, 0x0011, -0x0023, 0x0046, -0x0017, -0x0044, 0x015b,
      -0x0347, 0x080e, -0x1249, 0x3c07, 0x53e0, -0x16fa, 0x0afa,
      -0x0548, 0x027b, -0x00eb, 0x001a, 0x002b, -0x0023, 0x0010,
      -0x0008, 0x0002, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0}},
}};

// PlayStation XA 18.9 -> 44.1 kHz interpolation coefficients. The 18.9 kHz
// path is not equivalent to duplicating samples before using the 37.8 kHz
// zig-zag filter: doing that introduces a strong high-frequency image and is
// audibly wrong for music and speech. These are the coefficients used by the
// hardware-accurate DuckStation path (originally derived from Mednafen).
constexpr std::array<std::array<std::int16_t, 25>, 7> xa_half_rate_tables{{
    {{0x0, -0x5, 0x11, -0x23, 0x46, -0x17, -0x44, 0x15b, -0x347,
      0x80e, -0x1249, 0x3c07, 0x53e0, -0x16fa, 0xafa, -0x548, 0x27b,
      -0xeb, 0x1a, 0x2b, -0x23, 0x10, -0x8, 0x2, 0x0}},
    {{0x0, -0x2, 0xa, -0x22, 0x41, -0x54, 0x34, 0x9, -0x10a, 0x400,
      -0xa78, 0x234c, 0x6794, -0x1780, 0xbcd, -0x623, 0x350, -0x16d,
      0x6b, 0xa, -0x10, 0x11, -0x8, 0x3, -0x1}},
    {{-0x2, 0x0, 0x3, -0x13, 0x3c, -0x4b, 0xa2, -0xe3, 0x132, -0x43,
      -0x267, 0xc9d, 0x74bb, -0x11b4, 0x9b8, -0x5bf, 0x372, -0x1a8,
      0xa6, -0x1b, 0x5, 0x6, -0x8, 0x3, -0x1}},
    {{-0x1, 0x3, -0x2, -0x5, 0x1f, -0x4a, 0xb3, -0x192, 0x2b1,
      -0x39e, 0x4f8, -0x5a6, 0x7939, -0x5a6, 0x4f8, -0x39e, 0x2b1,
      -0x192, 0xb3, -0x4a, 0x1f, -0x5, -0x2, 0x3, -0x1}},
    {{-0x1, 0x3, -0x8, 0x6, 0x5, -0x1b, 0xa6, -0x1a8, 0x372,
      -0x5bf, 0x9b8, -0x11b4, 0x74bb, 0xc9d, -0x267, -0x43, 0x132,
      -0xe3, 0xa2, -0x4b, 0x3c, -0x13, 0x3, 0x0, -0x2}},
    {{-0x1, 0x3, -0x8, 0x11, -0x10, 0xa, 0x6b, -0x16d, 0x350,
      -0x623, 0xbcd, -0x1780, 0x6794, 0x234c, -0xa78, 0x400, -0x10a,
      0x9, 0x34, -0x54, 0x41, -0x22, 0xa, -0x2, 0x0}},
    {{0x0, 0x2, -0x8, 0x10, -0x23, 0x2b, 0x1a, -0xeb, 0x27b,
      -0x548, 0xafa, -0x16fa, 0x53e0, 0x3c07, -0x1249, 0x80e,
      -0x347, 0x15b, -0x44, -0x17, 0x46, -0x23, 0x11, -0x5, 0x0}},
}};

[[nodiscard]] std::uint8_t byteValue(std::byte value) noexcept {
  return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] bool validSync(std::span<const std::byte> sector) noexcept {
  if (byteValue(sector[0]) != 0U || byteValue(sector[sync_size - 1U]) != 0U) {
    return false;
  }
  for (std::size_t index = 1U; index + 1U < sync_size; ++index) {
    if (byteValue(sector[index]) != 0xffU) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool supportedCoding(std::uint8_t coding) noexcept {
  return (coding & coding_reserved_mask) == 0U;
}

[[nodiscard]] std::uint32_t sampleRate(std::uint8_t coding) noexcept {
  return (coding & coding_half_rate) != 0U ? 18'900U : 37'800U;
}

[[nodiscard]] std::int32_t floorDividePowerOfTwo(std::int64_t value,
                                                 std::uint8_t shift) noexcept {
  if (shift == 0U) {
    return static_cast<std::int32_t>(value);
  }

  const auto divisor = std::int64_t{1} << shift;
  if (value >= 0) {
    return static_cast<std::int32_t>(value / divisor);
  }
  return static_cast<std::int32_t>(-((-value + divisor - 1) / divisor));
}

[[nodiscard]] std::uint32_t
readLittleEndianWord(const std::byte *data) noexcept {
  return static_cast<std::uint32_t>(byteValue(data[0])) |
         (static_cast<std::uint32_t>(byteValue(data[1])) << 8U) |
         (static_cast<std::uint32_t>(byteValue(data[2])) << 16U) |
         (static_cast<std::uint32_t>(byteValue(data[3])) << 24U);
}

void decodeBlock(
    const std::byte *group, std::size_t block, bool eight_bit,
    std::array<std::int32_t, 2> &history,
    std::array<std::int16_t, samples_per_block> &decoded) noexcept {
  const auto header = byteValue(group[4U + block]);
  auto shift = static_cast<std::uint8_t>(header & 0x0fU);
  if (shift > 12U) {
    shift = 9U;
  }

  const auto filter = static_cast<std::uint8_t>(header >> 4U);
  const auto positive =
      filter < predictor_positive.size() ? predictor_positive[filter] : 0;
  const auto negative =
      filter < predictor_negative.size() ? predictor_negative[filter] : 0;

  for (std::size_t sample_index = 0U; sample_index < samples_per_block;
       ++sample_index) {
    const auto packed = readLittleEndianWord(
        group + 16U + sample_index * sizeof(std::uint32_t));

    std::int32_t encoded{};
    std::int32_t scale{};
    if (eight_bit) {
      const auto raw = static_cast<std::uint8_t>(packed >> (block * 8U));
      encoded = raw < 0x80U ? static_cast<std::int32_t>(raw)
                            : static_cast<std::int32_t>(raw) - 0x100;
      scale = 1 << 8U;
    } else {
      const auto raw = static_cast<std::uint8_t>(
          (packed >> (block * 4U)) & static_cast<std::uint32_t>(0x0fU));
      encoded = raw < 8U ? static_cast<std::int32_t>(raw)
                         : static_cast<std::int32_t>(raw) - 16;
      scale = 1 << 12U;
    }

    const auto base = floorDividePowerOfTwo(
        static_cast<std::int64_t>(encoded) * scale, shift);
    const auto predicted_newest = floorDividePowerOfTwo(
        static_cast<std::int64_t>(history[0]) * positive, 6U);
    const auto predicted_older = floorDividePowerOfTwo(
        static_cast<std::int64_t>(history[1]) * negative, 6U);
    const auto sample = std::clamp<std::int32_t>(
        base + predicted_newest + predicted_older, -32'768, 32'767);

    history[1] = history[0];
    history[0] = sample;
    decoded[sample_index] = static_cast<std::int16_t>(sample);
  }
}

[[nodiscard]] XaSectorFormat
readFormat(std::span<const std::byte> sector) noexcept {
  XaSectorFormat format{};
  format.file = byteValue(sector[subheader_offset]);
  format.channel = byteValue(sector[subheader_offset + 1U]);
  format.submode = byteValue(sector[subheader_offset + 2U]);
  format.coding = byteValue(sector[subheader_offset + 3U]);
  format.sample_rate_hz = sampleRate(format.coding);
  format.channel_count = (format.coding & coding_stereo) != 0U ? 2U : 1U;
  format.bits_per_sample = (format.coding & coding_eight_bit) != 0U ? 8U : 4U;
  return format;
}

[[nodiscard]] std::size_t
sourceFrameCount(const XaSectorFormat &format) noexcept {
  const auto scalar_samples =
      format.bits_per_sample == 8U ? std::size_t{2016U} : std::size_t{4032U};
  return scalar_samples / format.channel_count;
}

[[nodiscard]] bool predictorInRange(std::int32_t value) noexcept {
  return value >= -32'768 && value <= 32'767;
}

[[nodiscard]] std::int16_t zigZagInterpolate(
    const std::array<std::int16_t, 32> &ring, std::size_t table_index,
    std::size_t position) noexcept {
  std::int32_t sum{};
  for (std::size_t index = 0U; index < xa_zigzag_tables[table_index].size();
       ++index) {
    const auto product = static_cast<std::int64_t>(
        ring[(position - index) & 31U]) *
        xa_zigzag_tables[table_index][index];
    sum += floorDividePowerOfTwo(product, 15U);
  }
  return static_cast<std::int16_t>(
      std::clamp(sum, -32'768, 32'767));
}

[[nodiscard]] std::int16_t halfRateInterpolate(
    const std::array<std::int16_t, 32> &ring, std::size_t table_index,
    std::size_t position) noexcept {
  std::int64_t sum{};
  for (std::size_t index = 0U;
       index < xa_half_rate_tables[table_index].size(); ++index) {
    sum += static_cast<std::int64_t>(
               ring[(position + 32U - 25U + index) & 31U]) *
           xa_half_rate_tables[table_index][index];
  }
  return static_cast<std::int16_t>(std::clamp<std::int32_t>(
      floorDividePowerOfTwo(sum, 15U), -32'768, 32'767));
}

} // namespace

void XaAudioDecoder::reset() noexcept { state_ = {}; }

XaDecodeResult
XaAudioDecoder::decodeSector(std::span<const std::byte> raw_sector,
                             std::span<SpuPcmFrame> output) noexcept {
  XaDecodeResult result{};
  if (raw_sector.size() != raw_sector_size) {
    result.status = XaDecodeStatus::invalid_sector_size;
    return result;
  }
  if (!validSync(raw_sector)) {
    result.status = XaDecodeStatus::invalid_sync;
    return result;
  }
  if (byteValue(raw_sector[mode_offset]) != mode2_value) {
    result.status = XaDecodeStatus::not_mode2;
    return result;
  }

  for (std::size_t index = 0U; index < 4U; ++index) {
    if (raw_sector[subheader_offset + index] !=
        raw_sector[subheader_copy_offset + index]) {
      result.status = XaDecodeStatus::subheader_mismatch;
      return result;
    }
  }

  result.format = readFormat(raw_sector);
  if ((result.format.submode & (submode_audio | submode_form2)) !=
      (submode_audio | submode_form2)) {
    result.status = XaDecodeStatus::not_form2_audio;
    return result;
  }
  if (!supportedCoding(result.format.coding)) {
    result.status = XaDecodeStatus::unsupported_coding;
    return result;
  }

  const auto source_frames = sourceFrameCount(result.format);
  result.frames_required =
      source_frames * output_sample_rate_hz / result.format.sample_rate_hz;
  if (output.size() < result.frames_required) {
    result.status = XaDecodeStatus::output_too_small;
    return result;
  }

  auto working_state = state_;
  if (working_state.active == 0U) {
    working_state = {};
    working_state.active = 1U;
  }
  // These fields describe the last accepted sector for snapshots and
  // diagnostics. They are not stream-reset triggers: the real CD decoder
  // keeps ADPCM predictor and resampler history across Setfilter hand-offs.
  working_state.active_file = result.format.file;
  working_state.active_channel = result.format.channel;
  working_state.active_coding = result.format.coding;

  std::array<SpuPcmFrame, 4032U> source{};
  std::array<std::int16_t, samples_per_block> first{};
  std::array<std::int16_t, samples_per_block> second{};
  std::size_t source_position{};
  const auto stereo = result.format.channel_count == 2U;
  const auto eight_bit = result.format.bits_per_sample == 8U;
  const auto block_count = eight_bit ? std::size_t{4U} : std::size_t{8U};

  for (std::size_t group_index = 0U; group_index < sound_group_count;
       ++group_index) {
    const auto *group = raw_sector.data() + audio_payload_offset +
                        group_index * sound_group_size;
    if (stereo) {
      for (std::size_t block = 0U; block < block_count; block += 2U) {
        decodeBlock(group, block, eight_bit, working_state.predictor_samples[0],
                    first);
        decodeBlock(group, block + 1U, eight_bit,
                    working_state.predictor_samples[1], second);
        for (std::size_t index = 0U; index < samples_per_block; ++index) {
          source[source_position++] = SpuPcmFrame{first[index], second[index]};
        }
      }
    } else {
      for (std::size_t block = 0U; block < block_count; ++block) {
        decodeBlock(group, block, eight_bit, working_state.predictor_samples[0],
                    first);
        for (const auto sample : first) {
          source[source_position++] = SpuPcmFrame{sample, sample};
        }
      }
    }
  }

  std::size_t output_position{};
  if (result.format.sample_rate_hz == 37'800U) {
    auto position = static_cast<std::size_t>(working_state.resample_position);
    auto sixstep = static_cast<std::size_t>(working_state.resample_sixstep);
    for (std::size_t index = 0U; index < source_position; ++index) {
      working_state.resample_ring[0][position] = source[index].left;
      working_state.resample_ring[1][position] = source[index].right;
      position = (position + 1U) & 31U;
      --sixstep;
      if (sixstep != 0U) {
        continue;
      }
      sixstep = 6U;
      for (std::size_t table = 0U; table < xa_zigzag_tables.size(); ++table) {
        output[output_position++] = SpuPcmFrame{
            zigZagInterpolate(working_state.resample_ring[0], table, position),
            zigZagInterpolate(working_state.resample_ring[1], table, position),
        };
      }
    }
    working_state.resample_position = static_cast<std::uint8_t>(position);
    working_state.resample_sixstep = static_cast<std::uint8_t>(sixstep);
  } else {
    auto position = static_cast<std::size_t>(working_state.resample_position);
    auto sixstep = static_cast<std::size_t>(working_state.resample_sixstep);
    std::size_t input_position{};
    while (input_position < source_position) {
      if (sixstep >= xa_half_rate_tables.size()) {
        sixstep -= xa_half_rate_tables.size();
        position = (position + 1U) & 31U;
        working_state.resample_ring[0][position] =
            source[input_position].left;
        working_state.resample_ring[1][position] =
            source[input_position].right;
        ++input_position;
      }
      output[output_position++] = SpuPcmFrame{
          halfRateInterpolate(working_state.resample_ring[0], sixstep,
                              position),
          halfRateInterpolate(working_state.resample_ring[1], sixstep,
                              position),
      };
      sixstep += 3U;
    }
    working_state.resample_position = static_cast<std::uint8_t>(position);
    working_state.resample_sixstep = static_cast<std::uint8_t>(sixstep);
    working_state.resample_phase = 0U;
  }
  state_ = working_state;
  result.status = XaDecodeStatus::decoded;
  result.frames_written = output_position;
  return result;
}

bool XaAudioDecoder::validateState(const XaDecoderState &state) const noexcept {
  if (state.active > 1U) {
    return false;
  }
  if (state.active == 0U) {
    return state == XaDecoderState{};
  }
  if (!supportedCoding(state.active_coding) || state.resample_phase != 0U ||
      state.resample_position >= 32U || state.resample_sixstep == 0U ||
      state.resample_sixstep > 6U) {
    return false;
  }
  for (const auto &channel : state.predictor_samples) {
    for (const auto sample : channel) {
      if (!predictorInRange(sample)) {
        return false;
      }
    }
  }
  return true;
}

bool XaAudioDecoder::restoreState(const XaDecoderState &state) noexcept {
  if (!validateState(state)) {
    return false;
  }
  state_ = state;
  return true;
}

} // namespace sf::psx
