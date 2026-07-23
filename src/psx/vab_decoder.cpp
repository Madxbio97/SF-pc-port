#include "sf/psx/vab_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace sf::psx {
namespace {

constexpr std::size_t vab_header_size = 0x20U;
constexpr std::size_t program_table_offset = 0x20U;
constexpr std::size_t program_entry_size = 16U;
constexpr std::size_t program_entry_count = 128U;
constexpr std::size_t tone_table_offset =
    program_table_offset + program_entry_count * program_entry_size;
constexpr std::size_t tones_per_program = 16U;
constexpr std::size_t tone_entry_size = 32U;
constexpr std::size_t sample_size_entry_count = 256U;
constexpr std::size_t sound_descriptor_size = 24U;

constexpr std::uint32_t voice_volume_left = 0x000U;
constexpr std::uint32_t voice_volume_right = 0x002U;
constexpr std::uint32_t voice_pitch = 0x004U;
constexpr std::uint32_t voice_start_address = 0x006U;
constexpr std::uint32_t voice_adsr_low = 0x008U;
constexpr std::uint32_t voice_adsr_high = 0x00aU;
constexpr std::uint32_t voice_repeat_address = 0x00eU;
constexpr std::uint32_t main_volume_left = 0x180U;
constexpr std::uint32_t main_volume_right = 0x182U;
constexpr std::uint32_t key_on_low = 0x188U;
constexpr std::uint32_t transfer_address = 0x1a6U;
constexpr std::uint32_t control = 0x1aaU;

constexpr std::uint16_t control_unmute = 1U << 14U;
constexpr std::uint16_t control_enable = 1U << 15U;

// PsyQ's SsPitchFromNote lookup tables. The first covers semitones and the
// second 1/128th-semitone fine steps. Keeping the original integer tables and
// rounding is important: floating-point pow() is close, but not bit-exact at
// every pitch and produces a different SPU register in edge cases.
constexpr std::array<std::uint16_t, 12U> semitone_pitch{
    0x8000U, 0x879cU, 0x8facU, 0x9837U, 0xa145U, 0xaadcU,
    0xb504U, 0xbfc8U, 0xcb2fU, 0xd744U, 0xe411U, 0xf1a1U,
};
constexpr std::array<std::uint16_t, 128U> fine_pitch{
    0x8000U, 0x800eU, 0x801dU, 0x802cU, 0x803bU, 0x804aU, 0x8058U,
    0x8067U, 0x8076U, 0x8085U, 0x8094U, 0x80a3U, 0x80b1U, 0x80c0U,
    0x80cfU, 0x80deU, 0x80edU, 0x80fcU, 0x810bU, 0x811aU, 0x8129U,
    0x8138U, 0x8146U, 0x8155U, 0x8164U, 0x8173U, 0x8182U, 0x8191U,
    0x81a0U, 0x81afU, 0x81beU, 0x81cdU, 0x81dcU, 0x81ebU, 0x81faU,
    0x8209U, 0x8218U, 0x8227U, 0x8236U, 0x8245U, 0x8254U, 0x8263U,
    0x8272U, 0x8282U, 0x8291U, 0x82a0U, 0x82afU, 0x82beU, 0x82cdU,
    0x82dcU, 0x82ebU, 0x82faU, 0x830aU, 0x8319U, 0x8328U, 0x8337U,
    0x8346U, 0x8355U, 0x8364U, 0x8374U, 0x8383U, 0x8392U, 0x83a1U,
    0x83b0U, 0x83c0U, 0x83cfU, 0x83deU, 0x83edU, 0x83fdU, 0x840cU,
    0x841bU, 0x842aU, 0x843aU, 0x8449U, 0x8458U, 0x8468U, 0x8477U,
    0x8486U, 0x8495U, 0x84a5U, 0x84b4U, 0x84c3U, 0x84d3U, 0x84e2U,
    0x84f1U, 0x8501U, 0x8510U, 0x8520U, 0x852fU, 0x853eU, 0x854eU,
    0x855dU, 0x856dU, 0x857cU, 0x858bU, 0x859bU, 0x85aaU, 0x85baU,
    0x85c9U, 0x85d9U, 0x85e8U, 0x85f8U, 0x8607U, 0x8617U, 0x8626U,
    0x8636U, 0x8645U, 0x8655U, 0x8664U, 0x8674U, 0x8683U, 0x8693U,
    0x86a2U, 0x86b2U, 0x86c1U, 0x86d1U, 0x86e0U, 0x86f0U, 0x8700U,
    0x870fU, 0x871fU, 0x872eU, 0x873eU, 0x874eU, 0x875dU, 0x876dU,
    0x877dU, 0x878cU,
};

[[nodiscard]] std::uint8_t byteValue(std::byte value) noexcept {
  return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] std::uint16_t readLe16(
    std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(byteValue(bytes[offset])) |
         static_cast<std::uint16_t>(byteValue(bytes[offset + 1U]) << 8U);
}

[[nodiscard]] std::uint32_t readLe32(
    std::span<const std::byte> bytes, std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(readLe16(bytes, offset)) |
         (static_cast<std::uint32_t>(readLe16(bytes, offset + 2U)) << 16U);
}

[[nodiscard]] bool isBeepHeader(
    std::span<const std::byte> header) noexcept {
  constexpr std::array magic{std::byte{'B'}, std::byte{'E'}, std::byte{'E'},
                             std::byte{'P'}};
  return header.size() >= vab_header_size &&
         std::equal(magic.begin(), magic.end(), header.begin());
}

[[nodiscard]] std::uint16_t calculatePitch(std::uint8_t note,
                                           std::uint8_t fine,
                                           std::uint8_t center,
                                           std::uint8_t shift) noexcept {
  auto fine_total = static_cast<std::int32_t>(fine) + shift;
  auto semitone = static_cast<std::int32_t>(note) - center;
  if (fine_total >= 128) {
    semitone += fine_total / 128;
    fine_total %= 128;
  }

  auto octave = semitone / 12 - 2;
  auto semitone_index = semitone % 12;
  if (semitone_index < 0) {
    semitone_index += 12;
    --octave;
  }
  if (octave >= 0) {
    return 0x3fffU;
  }

  const auto product =
      static_cast<std::uint32_t>(semitone_pitch[semitone_index]) *
      fine_pitch[static_cast<std::size_t>(fine_total)];
  const auto base = product >> 16U;
  const auto shift_right = static_cast<std::uint32_t>(-octave);
  const auto rounded = base + (1U << (shift_right - 1U));
  return static_cast<std::uint16_t>(rounded >> shift_right);
}

[[nodiscard]] std::uint16_t calculateVolume(
    std::uint8_t sound_volume, std::uint8_t bank_volume,
    std::uint8_t program_volume, std::uint8_t tone_volume) noexcept {
  constexpr std::uint32_t maximum_volume = 0x3fffU;
  constexpr std::uint32_t psyq_scale = 0x3f01U; // 127 * 127
  const auto first =
      static_cast<std::uint32_t>(sound_volume) * bank_volume * maximum_volume /
      psyq_scale;
  return static_cast<std::uint16_t>(
      first * program_volume * tone_volume / psyq_scale);
}

[[nodiscard]] bool uploadSample(Spu &spu,
                                std::span<const std::byte> sample) noexcept {
  if (sample.empty() || (sample.size() & 15U) != 0U ||
      sample.size() > Spu::ram_size) {
    return false;
  }
  if (!spu.writeRegister(transfer_address, 0U) ||
      !spu.writeRegister(control, 2U << 4U)) {
    return false;
  }
  for (std::size_t offset = 0U; offset < sample.size(); offset += 4U) {
    const auto word = static_cast<std::uint32_t>(byteValue(sample[offset])) |
                      (static_cast<std::uint32_t>(
                           byteValue(sample[offset + 1U]))
                       << 8U) |
                      (static_cast<std::uint32_t>(
                           byteValue(sample[offset + 2U]))
                       << 16U) |
                      (static_cast<std::uint32_t>(
                           byteValue(sample[offset + 3U]))
                       << 24U);
    if (!spu.writeDmaWord(word)) {
      return false;
    }
  }
  return true;
}

} // namespace

VabDecodeResult decodeVabSound(std::span<const std::byte> header,
                               std::span<const std::byte> body,
                               std::size_t sound_index) {
  VabDecodeResult result{};
  if (!isBeepHeader(header)) {
    return result;
  }

  const auto descriptor_offset =
      static_cast<std::size_t>(readLe32(header, 4U));
  const auto sound_count = static_cast<std::size_t>(byteValue(header[12U]));
  const auto program_count =
      static_cast<std::size_t>(readLe16(header, 0x12U));
  const auto sample_count =
      static_cast<std::size_t>(readLe16(header, 0x16U));
  const auto tone_bytes = program_count * tones_per_program * tone_entry_size;
  const auto size_table_offset = tone_table_offset + tone_bytes;
  if (program_count == 0U || program_count > program_entry_count ||
      sample_count == 0U || sample_count >= sample_size_entry_count ||
      sound_index >= sound_count ||
      descriptor_offset > header.size() ||
      sound_count >
          (header.size() - descriptor_offset) / sound_descriptor_size ||
      size_table_offset > header.size() ||
      sample_size_entry_count * sizeof(std::uint16_t) >
          header.size() - size_table_offset) {
    return result;
  }

  const auto descriptor =
      header.subspan(descriptor_offset + sound_index * sound_descriptor_size,
                     sound_descriptor_size);
  if ((readLe16(descriptor, 0U) & 0x1fU) != 0U) {
    result.status = VabDecodeStatus::unsupported_sound;
    return result;
  }

  const auto program = static_cast<std::size_t>(byteValue(descriptor[5U]));
  const auto note = byteValue(descriptor[7U]);
  const auto fine = byteValue(descriptor[8U]);
  if (program >= program_count) {
    result.status = VabDecodeStatus::invalid_sound;
    return result;
  }

  const auto program_entry =
      header.subspan(program_table_offset + program * program_entry_size,
                     program_entry_size);
  const auto tone_count = std::min<std::size_t>(
      byteValue(program_entry[0U]), tones_per_program);
  std::span<const std::byte> selected_tone;
  for (std::size_t tone = 0U; tone < tone_count; ++tone) {
    const auto candidate = header.subspan(
        tone_table_offset +
            (program * tones_per_program + tone) * tone_entry_size,
        tone_entry_size);
    if (note >= byteValue(candidate[6U]) &&
        note <= byteValue(candidate[7U]) && readLe16(candidate, 22U) != 0U) {
      selected_tone = candidate;
      break;
    }
  }
  if (selected_tone.empty()) {
    result.status = VabDecodeStatus::invalid_sound;
    return result;
  }

  const auto sample_number =
      static_cast<std::size_t>(readLe16(selected_tone, 22U));
  if (sample_number == 0U || sample_number > sample_count) {
    result.status = VabDecodeStatus::invalid_sample;
    return result;
  }
  std::size_t sample_offset{};
  for (std::size_t sample = 1U; sample < sample_number; ++sample) {
    sample_offset +=
        static_cast<std::size_t>(readLe16(
            header,
            size_table_offset + sample * sizeof(std::uint16_t))) *
        8U;
  }
  const auto sample_size =
      static_cast<std::size_t>(readLe16(
          header,
          size_table_offset + sample_number * sizeof(std::uint16_t))) *
      8U;
  if (sample_size == 0U || sample_offset > body.size() ||
      sample_size > body.size() - sample_offset) {
    result.status = VabDecodeStatus::invalid_sample;
    return result;
  }

  const auto pitch = calculatePitch(
      note, fine, byteValue(selected_tone[4U]),
      byteValue(selected_tone[5U]));
  if (pitch == 0U || byteValue(descriptor[10U]) != 0U ||
      byteValue(selected_tone[3U]) != 0x40U) {
    result.status = VabDecodeStatus::unsupported_sound;
    return result;
  }

  Spu spu;
  if (!uploadSample(spu, body.subspan(sample_offset, sample_size))) {
    result.status = VabDecodeStatus::invalid_sample;
    return result;
  }
  const auto volume = calculateVolume(
      byteValue(descriptor[9U]), byteValue(header[0x18U]),
      byteValue(program_entry[1U]), byteValue(selected_tone[2U]));
  const auto configured =
      spu.writeRegister(voice_volume_left, volume) &&
      spu.writeRegister(voice_volume_right, volume) &&
      spu.writeRegister(voice_pitch, pitch) &&
      spu.writeRegister(voice_start_address, 0U) &&
      spu.writeRegister(voice_repeat_address, 0U) &&
      spu.writeRegister(voice_adsr_low, readLe16(selected_tone, 14U)) &&
      spu.writeRegister(voice_adsr_high, readLe16(selected_tone, 16U)) &&
      spu.writeRegister(main_volume_left, 0x3fffU) &&
      spu.writeRegister(main_volume_right, 0x3fffU) &&
      spu.writeRegister(control, control_enable | control_unmute) &&
      spu.writeRegister(key_on_low, 1U);
  if (!configured) {
    result.status = VabDecodeStatus::invalid_sample;
    return result;
  }

  const auto source_frames = sample_size / 16U * 28U;
  const auto maximum_frames = std::min<std::size_t>(
      (source_frames * 0x1000U + pitch - 1U) / pitch + 64U,
      Spu::pcm_queue_capacity);
  while (spu.state().voices[0].active != 0U &&
         result.frames.size() < maximum_frames) {
    spu.mixFrames(1U);
    std::array<SpuPcmFrame, 1U> frame{};
    if (spu.takePcm(frame) != 1U) {
      result.status = VabDecodeStatus::invalid_sample;
      result.frames.clear();
      return result;
    }
    result.frames.push_back(frame[0]);
  }
  if (result.frames.empty() || spu.state().voices[0].active != 0U) {
    result.status = VabDecodeStatus::invalid_sample;
    result.frames.clear();
    return result;
  }

  result.status = VabDecodeStatus::decoded;
  result.vab_id = readLe16(header, 8U);
  result.program = static_cast<std::uint16_t>(program);
  result.sample = static_cast<std::uint16_t>(sample_number);
  result.pitch = pitch;
  result.volume_left = volume;
  result.volume_right = volume;
  return result;
}

} // namespace sf::psx
