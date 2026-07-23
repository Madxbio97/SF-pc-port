#include "sf/psx/xa_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {

using Sector = std::array<std::byte, sf::psx::XaAudioDecoder::raw_sector_size>;
using PcmBuffer = std::array<sf::psx::SpuPcmFrame,
                             sf::psx::XaAudioDecoder::maximum_output_frames>;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void writeLe32(std::byte *destination, std::uint32_t value) {
  destination[0] = static_cast<std::byte>(value);
  destination[1] = static_cast<std::byte>(value >> 8U);
  destination[2] = static_cast<std::byte>(value >> 16U);
  destination[3] = static_cast<std::byte>(value >> 24U);
}

Sector makeSector(std::uint8_t coding,
                  std::uint32_t packed_samples = 0x76543210U,
                  std::uint8_t filter = 0U) {
  Sector sector{};
  for (std::size_t index = 1U; index < 11U; ++index) {
    sector[index] = std::byte{0xffU};
  }
  sector[15] = std::byte{2U};

  constexpr std::array<std::uint8_t, 3> subheader_prefix{1U, 0U, 0x64U};
  for (std::size_t copy = 0U; copy < 2U; ++copy) {
    const auto offset = 16U + copy * 4U;
    sector[offset] = static_cast<std::byte>(subheader_prefix[0]);
    sector[offset + 1U] = static_cast<std::byte>(subheader_prefix[1]);
    sector[offset + 2U] = static_cast<std::byte>(subheader_prefix[2]);
    sector[offset + 3U] = static_cast<std::byte>(coding);
  }

  constexpr std::size_t payload_offset = 24U;
  constexpr std::size_t group_size = 128U;
  constexpr std::size_t group_count = 18U;
  constexpr std::size_t words_per_group = 28U;
  const auto header = static_cast<std::byte>((filter << 4U) | 12U);
  for (std::size_t group_index = 0U; group_index < group_count; ++group_index) {
    auto *group = sector.data() + payload_offset + group_index * group_size;
    std::fill_n(group + 4U, 8U, header);
    for (std::size_t word = 0U; word < words_per_group; ++word) {
      writeLe32(group + 16U + word * sizeof(std::uint32_t), packed_samples);
    }
  }
  return sector;
}

void requireFrame(const sf::psx::SpuPcmFrame &frame, std::int16_t left,
                  std::int16_t right, const char *message) {
  require(frame == sf::psx::SpuPcmFrame{left, right}, message);
}

void testFourBitMono() {
  sf::psx::XaAudioDecoder decoder;
  const auto sector = makeSector(0x00U);
  PcmBuffer pcm{};
  const auto result = decoder.decodeSector(sector, pcm);

  require(result.succeeded(), "4-bit mono sector was rejected");
  require(result.frames_written == 4704U && result.frames_required == 4704U,
          "4-bit mono produced the wrong frame count");
  require(result.format.channel_count == 1U &&
              result.format.bits_per_sample == 4U &&
              result.format.sample_rate_hz == 37'800U,
          "4-bit mono format was decoded incorrectly");
  requireFrame(pcm[0], 0, 0, "4-bit mono first sample is wrong");
  requireFrame(pcm[30], -1, -1, "4-bit mono zig-zag warm-up is wrong");
  requireFrame(pcm[35], -2, -2, "4-bit mono interpolation is wrong");
}

void testFourBitStereo() {
  sf::psx::XaAudioDecoder decoder;
  const auto sector = makeSector(0x01U);
  PcmBuffer pcm{};
  const auto result = decoder.decodeSector(sector, pcm);

  require(result.succeeded(), "4-bit stereo sector was rejected");
  require(result.frames_written == 2352U && result.frames_required == 2352U,
          "4-bit stereo produced the wrong frame count");
  require(result.format.channel_count == 2U &&
              result.format.sample_rate_hz == 37'800U,
          "4-bit stereo format was decoded incorrectly");
  requireFrame(pcm[0], 0, -1, "4-bit stereo channel order is wrong");
  requireFrame(pcm[30], -1, -14,
               "4-bit stereo zig-zag warm-up is wrong");
  requireFrame(pcm[35], -2, -11,
               "4-bit stereo interpolation is wrong");
}

void testHalfRateMono() {
  sf::psx::XaAudioDecoder decoder;
  const auto sector = makeSector(0x04U);
  PcmBuffer pcm{};
  const auto result = decoder.decodeSector(sector, pcm);

  require(result.succeeded(), "18.9 kHz mono sector was rejected");
  std::uint64_t hash = 1469598103934665603ULL;
  for (std::size_t index = 0U; index < result.frames_written; ++index) {
    const auto word = static_cast<std::uint16_t>(pcm[index].left);
    hash ^= static_cast<std::uint8_t>(word);
    hash *= 1099511628211ULL;
    hash ^= static_cast<std::uint8_t>(word >> 8U);
    hash *= 1099511628211ULL;
  }
  require(result.frames_written == 9407U && result.frames_required == 9408U,
          "18.9 kHz mono produced the wrong frame count");
  require(result.format.sample_rate_hz == 18'900U,
          "Half-rate coding selected the wrong sample rate");
  requireFrame(pcm[68], 0, 0, "Half-rate filter warm-up is too short");
  requireFrame(pcm[69], -1, -1, "Half-rate interpolation phase is wrong");
  requireFrame(pcm[result.frames_written - 1U], 6, 6,
               "Half-rate final interpolation sample is wrong");
  require(hash == 17352770825429278310ULL,
          "Half-rate 25-tap interpolation golden vector changed");

  PcmBuffer continued_pcm{};
  const auto continued = decoder.decodeSector(sector, continued_pcm);
  require(continued.succeeded() && continued.frames_written == 9408U,
          "Half-rate interpolation continuity lost a frame");
}

void testOutputFailureIsAtomic() {
  sf::psx::XaAudioDecoder decoder;
  const auto history_sector = makeSector(0x00U, 0x11111111U, 2U);
  PcmBuffer pcm{};
  require(decoder.decodeSector(history_sector, pcm).succeeded(),
          "Could not seed XA predictor state");
  const auto before = decoder.captureState();

  const auto sector = makeSector(0x00U);
  const auto result = decoder.decodeSector(
      sector, std::span<sf::psx::SpuPcmFrame>{pcm}.first(4703U));
  require(result.status == sf::psx::XaDecodeStatus::output_too_small,
          "Short XA output buffer returned the wrong status");
  require(result.frames_written == 0U && result.frames_required == 4704U,
          "Short XA output buffer returned the wrong bounds");
  require(decoder.captureState() == before,
          "Short XA output buffer mutated decoder state");
}

void testFileChannelHandoffPreservesDecoderHistory() {
  const auto first = makeSector(0x00U, 0x11111111U, 2U);
  const auto continued = makeSector(0x00U, 0x22222222U, 3U);
  auto handed_off = continued;
  handed_off[16U] = handed_off[20U] = std::byte{9U};
  handed_off[17U] = handed_off[21U] = std::byte{7U};

  sf::psx::XaAudioDecoder reference;
  sf::psx::XaAudioDecoder changed_selector;
  PcmBuffer discarded{};
  require(reference.decodeSector(first, discarded).succeeded() &&
              changed_selector.decodeSector(first, discarded).succeeded(),
          "Could not seed XA hand-off predictor history");

  PcmBuffer expected{};
  PcmBuffer actual{};
  const auto expected_result = reference.decodeSector(continued, expected);
  const auto actual_result =
      changed_selector.decodeSector(handed_off, actual);
  require(expected_result.succeeded() && actual_result.succeeded() &&
              expected_result.frames_written == actual_result.frames_written &&
              std::equal(expected.begin(),
                         expected.begin() + expected_result.frames_written,
                         actual.begin()),
          "XA file/channel hand-off reset predictor or resampler history");
  require(changed_selector.captureState().active_file == 9U &&
              changed_selector.captureState().active_channel == 7U,
          "XA snapshot did not record the accepted hand-off selector");
}

void testSubheaderAndCodingErrors() {
  sf::psx::XaAudioDecoder decoder;
  PcmBuffer pcm{};

  auto mismatched = makeSector(0x00U);
  mismatched[20] = std::byte{2U};
  require(decoder.decodeSector(mismatched, pcm).status ==
              sf::psx::XaDecodeStatus::subheader_mismatch,
          "Duplicated XA subheader mismatch was accepted");

  const auto reserved_coding = makeSector(0x02U);
  require(decoder.decodeSector(reserved_coding, pcm).status ==
              sf::psx::XaDecodeStatus::unsupported_coding,
          "Reserved XA coding bits were accepted");

  auto not_form2 = makeSector(0x00U);
  not_form2[18] = std::byte{0x44U};
  not_form2[22] = std::byte{0x44U};
  require(decoder.decodeSector(not_form2, pcm).status ==
              sf::psx::XaDecodeStatus::not_form2_audio,
          "Non-Form2 XA sector was accepted as audio");
}

void testSnapshotReplay() {
  const auto first_sector = makeSector(0x00U, 0x11111111U, 2U);
  const auto next_sector = makeSector(0x00U, 0x22222222U, 3U);
  sf::psx::XaAudioDecoder original;
  PcmBuffer discarded{};
  require(original.decodeSector(first_sector, discarded).succeeded(),
          "Could not establish snapshot predictor history");
  const auto snapshot = original.captureState();

  PcmBuffer expected{};
  const auto expected_result = original.decodeSector(next_sector, expected);
  require(expected_result.succeeded(), "Could not decode post-snapshot sector");

  sf::psx::XaAudioDecoder restored;
  require(restored.restoreState(snapshot), "Valid XA snapshot was rejected");
  PcmBuffer replayed{};
  const auto replayed_result = restored.decodeSector(next_sector, replayed);
  require(
      replayed_result.status == expected_result.status &&
          replayed_result.format == expected_result.format &&
          replayed_result.frames_written == expected_result.frames_written &&
          replayed_result.frames_required == expected_result.frames_required,
      "Restored XA result differs from original execution");
  require(std::equal(expected.begin(),
                     expected.begin() + expected_result.frames_written,
                     replayed.begin()),
          "Restored XA PCM differs from original execution");
  require(restored.captureState() == original.captureState(),
          "Restored XA state diverged after replay");
}

} // namespace

int main() {
  try {
    testFourBitMono();
    testFourBitStereo();
    testHalfRateMono();
    testOutputFailureIsAtomic();
    testFileChannelHandoffPreservesDecoderHistory();
    testSubheaderAndCodingErrors();
    testSnapshotReplay();
    std::cout << "XA decoder tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "XA decoder tests failed: " << error.what() << '\n';
    return 1;
  }
}
