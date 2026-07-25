#include "sf/psx/spu.hpp"
#include "sf/psx/vab_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uint32_t voice_volume_left = 0x000U;
constexpr std::uint32_t voice_volume_right = 0x002U;
constexpr std::uint32_t voice_pitch = 0x004U;
constexpr std::uint32_t voice_start_address = 0x006U;
constexpr std::uint32_t voice_repeat_address = 0x00eU;
constexpr std::uint32_t main_volume_left = 0x180U;
constexpr std::uint32_t main_volume_right = 0x182U;
constexpr std::uint32_t key_on_low = 0x188U;
constexpr std::uint32_t pitch_modulation_low = 0x190U;
constexpr std::uint32_t noise_mode_low = 0x194U;
constexpr std::uint32_t endx_low = 0x19cU;
constexpr std::uint32_t irq_address = 0x1a4U;
constexpr std::uint32_t transfer_address = 0x1a6U;
constexpr std::uint32_t transfer_fifo = 0x1a8U;
constexpr std::uint32_t control = 0x1aaU;
constexpr std::uint32_t transfer_control = 0x1acU;
constexpr std::uint32_t status = 0x1aeU;
constexpr std::uint32_t cd_volume_left = 0x1b0U;
constexpr std::uint32_t cd_volume_right = 0x1b2U;
constexpr std::uint32_t current_main_volume_left = 0x1b8U;
constexpr std::uint32_t current_voice_volume_left = 0x200U;

constexpr std::uint16_t control_cd_enable = 1U << 0U;
constexpr std::uint16_t control_irq_enable = 1U << 6U;
constexpr std::uint16_t control_unmute = 1U << 14U;
constexpr std::uint16_t control_enable = 1U << 15U;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void writeLe16(std::byte *destination, std::uint16_t value) noexcept {
  destination[0] = static_cast<std::byte>(value);
  destination[1] = static_cast<std::byte>(value >> 8U);
}

void writeLe32(std::byte *destination, std::uint32_t value) noexcept {
  writeLe16(destination, static_cast<std::uint16_t>(value));
  writeLe16(destination + 2U, static_cast<std::uint16_t>(value >> 16U));
}

void writeRamBytes(sf::psx::Spu &spu, std::uint32_t byte_address,
                   std::span<const std::byte> bytes) {
  require((byte_address & 7U) == 0U,
          "SPU RAM test write address is not 8-byte aligned");
  require((bytes.size() & 3U) == 0U,
          "SPU RAM test write size is not word aligned");
  require(spu.writeRegister(transfer_address,
                            static_cast<std::uint16_t>(byte_address >> 3U)),
          "Could not set SPU transfer address");
  require(spu.writeRegister(control, 2U << 4U),
          "Could not select SPU DMA write mode");

  for (std::size_t offset = 0U; offset < bytes.size(); offset += 4U) {
    const auto word =
        std::to_integer<std::uint32_t>(bytes[offset]) |
        (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
    require(spu.writeDmaWord(word), "Could not write an SPU DMA word");
  }
}

void keyOnVoiceZero(sf::psx::Spu &spu, std::uint16_t pitch = 0U) {
  require(spu.writeRegister(voice_pitch, pitch), "Could not set voice pitch");
  require(spu.writeRegister(voice_start_address, 0U),
          "Could not set voice start address");
  require(spu.writeRegister(voice_repeat_address, 0U),
          "Could not set voice repeat address");
  require(spu.writeRegister(control, control_enable),
          "Could not enable the SPU");
  require(spu.writeRegister(key_on_low, 1U), "Could not key on voice zero");
}

void testRegisterAccess() {
  auto spu = std::make_unique<sf::psx::Spu>();
  std::uint16_t value = 0U;

  require(spu->writeRegister(voice_volume_left, 0x2345U),
          "Voice register write failed");
  require(spu->readRegister(voice_volume_left, value) && value == 0x2345U,
          "Voice register did not round-trip");
  require(spu->writeRegister(transfer_control, 0x0004U),
          "Global register write failed");
  require(spu->readRegister(transfer_control, value) && value == 0x0004U,
          "Global register did not round-trip");
  require(spu->readRegister(transfer_fifo, value) && value == 0xffffU,
          "Transfer FIFO read value is incorrect");

  value = 0xaaaaU;
  require(!spu->readRegister(1U, value) && value == 0U,
          "Odd SPU register read was accepted");
  require(!spu->writeRegister(1U, 0U), "Odd SPU register write was accepted");
  require(!spu->readRegister(sf::psx::Spu::register_span, value),
          "Out-of-range SPU register read was accepted");
  require(!spu->writeRegister(sf::psx::Spu::register_span, 0U),
          "Out-of-range SPU register write was accepted");
  require(spu->readRegister(sf::psx::Spu::register_span - 2U, value),
          "Last aligned SPU register was rejected");
}

void testDmaTransfer() {
  auto spu = std::make_unique<sf::psx::Spu>();
  constexpr std::uint16_t address_units = 0x0010U;
  constexpr std::uint32_t byte_address = address_units << 3U;

  require(spu->writeRegister(transfer_address, address_units),
          "Could not set DMA write address");
  require(spu->writeRegister(control, 2U << 4U),
          "Could not select DMA write mode");
  require(spu->dmaRequest(), "DMA write mode did not assert a request");
  require(spu->writeDmaWord(0x44332211U), "SPU DMA write failed");
  require(spu->state().transfer_address == byte_address + 4U,
          "SPU DMA write advanced the address incorrectly");
  require(std::to_integer<std::uint8_t>(spu->ram()[byte_address]) == 0x11U &&
              std::to_integer<std::uint8_t>(spu->ram()[byte_address + 1U]) ==
                  0x22U &&
              std::to_integer<std::uint8_t>(spu->ram()[byte_address + 2U]) ==
                  0x33U &&
              std::to_integer<std::uint8_t>(spu->ram()[byte_address + 3U]) ==
                  0x44U,
          "SPU DMA write was not little-endian");

  std::uint32_t word = 0xffffffffU;
  require(!spu->readDmaWord(word) && word == 0U,
          "SPU DMA read ignored the transfer mode");
  require(spu->writeRegister(transfer_address, address_units),
          "Could not rewind DMA read address");
  require(spu->writeRegister(control, 3U << 4U),
          "Could not select DMA read mode");
  require(spu->dmaRequest(), "DMA read mode did not assert a request");
  require(spu->readDmaWord(word) && word == 0x44332211U,
          "SPU DMA read or byte order is incorrect");
  require(spu->state().transfer_address == byte_address + 4U,
          "SPU DMA read advanced the address incorrectly");
  require(spu->writeRegister(control, 0U),
          "Could not stop the SPU DMA transfer");
  require(!spu->dmaRequest(), "Stopped DMA still asserted a request");
}

void testIrqLatchAndClear() {
  auto spu = std::make_unique<sf::psx::Spu>();
  constexpr std::uint16_t address_units = 0x0040U;
  std::uint16_t status_value = 0U;

  require(spu->writeRegister(irq_address, address_units),
          "Could not set SPU IRQ address");
  require(spu->writeRegister(control, control_irq_enable),
          "Could not enable SPU IRQ");
  require(spu->writeRegister(transfer_address, address_units),
          "Could not touch the SPU IRQ address");
  require(spu->interruptLine(), "SPU IRQ was not latched on RAM access");
  require(spu->readRegister(status, status_value) &&
              (status_value & (1U << 6U)) != 0U,
          "SPU status did not expose the IRQ latch");

  require(spu->writeRegister(control, 0U), "Could not disable the SPU IRQ");
  require(!spu->interruptLine(), "Disabling SPU IRQ did not clear the latch");
  require(spu->readRegister(status, status_value) &&
              (status_value & (1U << 6U)) == 0U,
          "Cleared SPU IRQ remained visible in status");
}

void testCpuTickCadence() {
  auto spu = std::make_unique<sf::psx::Spu>();
  std::array<sf::psx::SpuPcmFrame, 3U> output{};

  spu->advanceCpuTicks(767U);
  require(spu->queuedPcmFrames() == 0U,
          "SPU emitted a frame before 768 CPU ticks");
  spu->advanceCpuTicks(1U);
  require(spu->queuedPcmFrames() == 1U,
          "SPU did not emit a frame at 768 CPU ticks");
  spu->advanceCpuTicks(1536U);
  require(spu->queuedPcmFrames() == 3U,
          "SPU cadence drifted across multiple frames");
  require(spu->takePcm(output) == output.size(),
          "Could not drain cadence test output");
  require(spu->state().sample_clock == 0U,
          "SPU retained a fractional clock after exact frame periods");
}

void testIdleFastForward() {
  auto spu = std::make_unique<sf::psx::Spu>();
  constexpr auto ticks = std::numeric_limits<std::uint64_t>::max();
  constexpr std::uint64_t ticks_per_frame =
      sf::psx::Spu::cpu_clock_hz / sf::psx::Spu::sample_rate;
  constexpr auto expected_frames = ticks / ticks_per_frame;
  constexpr auto expected_clock =
      (ticks % ticks_per_frame) * sf::psx::Spu::sample_rate;

  spu->advanceCpuTicks(ticks);

  require(spu->state().mixed_frames == expected_frames,
          "Idle SPU fast-forward produced the wrong frame count");
  require(spu->state().sample_clock == expected_clock,
          "Idle SPU fast-forward produced the wrong fractional clock");
  require(spu->queuedPcmFrames() == sf::psx::Spu::pcm_queue_capacity,
          "Idle SPU fast-forward did not retain a bounded silence tail");
  require(spu->droppedPcmFrames() ==
              expected_frames - sf::psx::Spu::pcm_queue_capacity,
          "Idle SPU fast-forward reported the wrong dropped-frame count");
}

void testAdpcmFilterVectors() {
  {
    auto spu = std::make_unique<sf::psx::Spu>();
    std::array<std::byte, 16U> block{};
    block[0] = static_cast<std::byte>(0x0cU);
    block[1] = static_cast<std::byte>(0x00U);
    std::fill(block.begin() + 2, block.end(), static_cast<std::byte>(0x81U));
    writeRamBytes(*spu, 0U, block);
    keyOnVoiceZero(*spu);
    spu->mixFrames(1U);

    constexpr std::array<std::int16_t, 28U> expected{
        1, -8, 1, -8, 1, -8, 1, -8, 1, -8, 1, -8, 1, -8,
        1, -8, 1, -8, 1, -8, 1, -8, 1, -8, 1, -8, 1, -8,
    };
    require(spu->state().voices[0].decoded_samples == expected,
            "SPU ADPCM filter-zero vector mismatch");
  }

  {
    auto spu = std::make_unique<sf::psx::Spu>();
    std::array<std::byte, 16U> block{};
    block[0] = static_cast<std::byte>(0x2cU);
    block[1] = static_cast<std::byte>(0x00U);
    std::fill(block.begin() + 2, block.end(), static_cast<std::byte>(0x11U));
    writeRamBytes(*spu, 0U, block);
    keyOnVoiceZero(*spu);
    spu->mixFrames(1U);

    constexpr std::array<std::int16_t, 28U> expected{
        1, 2, 3, 4, 5, 5, 4, 3, 2, 1, 0, 0, 1, 2,
        3, 4, 5, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4,
    };
    require(spu->state().voices[0].decoded_samples == expected,
            "SPU ADPCM filter-two vector mismatch");
  }
}

void testHardwarePitchClampAndPitchModulation() {
  {
    auto spu = std::make_unique<sf::psx::Spu>();
    std::array<std::byte, 16U> block{};
    block[0] = std::byte{0x0cU};
    std::fill(block.begin() + 2U, block.end(), std::byte{0x11U});
    writeRamBytes(*spu, 0U, block);
    keyOnVoiceZero(*spu, 0x4000U);
    spu->mixFrames(1U);
    require(spu->state().voices[0].sample_index == 3U &&
                spu->state().voices[0].pitch_counter == 0x0fffU,
            "SPU wrapped an over-range pitch instead of clamping to 0x3fff");

    require(spu->writeRegister(control, 0U), "Could not disable the SPU");
    require(spu->state().voices[0].active == 0U &&
                spu->state().voices[0].envelope == 0U &&
                spu->state().voices[0].adsr_phase == sf::psx::SpuAdsrPhase::off,
            "Disabling SPUCNT did not force active voices off");
  }

  {
    auto spu = std::make_unique<sf::psx::Spu>();
    std::array<std::byte, 32U> blocks{};
    blocks[0U] = std::byte{0x0cU};
    blocks[16U] = std::byte{0x0cU};
    std::fill(blocks.begin() + 2U, blocks.begin() + 16U, std::byte{0x11U});
    std::fill(blocks.begin() + 18U, blocks.end(), std::byte{0x11U});
    writeRamBytes(*spu, 0U, blocks);

    require(spu->writeRegister(voice_pitch, 0U) &&
                spu->writeRegister(voice_start_address, 0U) &&
                spu->writeRegister(voice_repeat_address, 0U) &&
                spu->writeRegister(voice_pitch + 0x10U, 0x1000U) &&
                spu->writeRegister(voice_start_address + 0x10U, 2U) &&
                spu->writeRegister(voice_repeat_address + 0x10U, 2U) &&
                spu->writeRegister(noise_mode_low, 1U) &&
                spu->writeRegister(pitch_modulation_low, 2U) &&
                spu->writeRegister(control,
                                   static_cast<std::uint16_t>(control_enable |
                                                              control_unmute |
                                                              (0x3fU << 8U))) &&
                spu->writeRegister(key_on_low, 3U) &&
                spu->writeRegister(0x00cU, 0x7fffU) &&
                spu->writeRegister(0x01cU, 0x7fffU),
            "Could not configure SPU pitch modulation voices");

    auto state = std::make_unique<sf::psx::SpuState>();
    *state = spu->state();
    state->noise_level = 0x7fffU;
    require(spu->restoreState(*state),
            "Could not seed deterministic SPU noise state");
    spu->mixFrames(1U);
    require(spu->state().voices[0].last_volume == 0x7ffe &&
                spu->state().voices[1].sample_index == 1U &&
                spu->state().voices[1].pitch_counter == 0x0fffU,
            "Previous voice output did not modulate the following voice pitch");
  }
}

void testHardwareNoiseGeneratorCadence() {
  auto spu = std::make_unique<sf::psx::Spu>();
  require(spu->writeRegister(control, static_cast<std::uint16_t>(0x3fU << 8U)),
          "Could not select the fastest SPU noise clock");
  spu->mixFrames(4U);
  require(spu->state().noise_count == 180U && spu->state().noise_level == 31U,
          "SPU noise LFSR/counter diverged from DuckStation cadence");

  auto incremental = std::make_unique<sf::psx::Spu>();
  auto accelerated = std::make_unique<sf::psx::Spu>();
  constexpr std::uint16_t medium_noise_clock = 0x25U << 8U;
  require(incremental->writeRegister(control, medium_noise_clock) &&
              accelerated->writeRegister(control, medium_noise_clock),
          "Could not configure SPU noise fast-forward comparison");
  constexpr std::size_t frames = 10'000U;
  incremental->mixFrames(frames);
  accelerated->advanceCpuTicks(
      static_cast<std::uint64_t>(frames) *
      (sf::psx::Spu::cpu_clock_hz / sf::psx::Spu::sample_rate));
  require(
      incremental->state().noise_count == accelerated->state().noise_count &&
          incremental->state().noise_level == accelerated->state().noise_level,
      "Idle SPU fast-forward skipped or drifted the free-running noise "
      "generator");
}

void testHardwareVolumeSweeps() {
  auto spu = std::make_unique<sf::psx::Spu>();
  std::array<std::byte, 16U> block{};
  block[0U] = std::byte{0x0cU};
  std::fill(block.begin() + 2U, block.end(), std::byte{0x11U});
  writeRamBytes(*spu, 0U, block);

  require(spu->writeRegister(voice_volume_left, 0x1000U) &&
              spu->writeRegister(main_volume_left, 0x1000U),
          "Could not seed SPU fixed volume before sweep mode");
  std::uint16_t current{};
  require(spu->readRegister(current_voice_volume_left, current) &&
              current == 0x2000U &&
              spu->readRegister(current_main_volume_left, current) &&
              current == 0x2000U,
          "SPU fixed volume did not populate the current-volume registers");

  // Linear increase, rate zero: +7<<11 on the first 44.1 kHz tick.
  require(spu->writeRegister(voice_volume_left, 0x8000U) &&
              spu->writeRegister(main_volume_left, 0x8000U),
          "Could not enable SPU hardware volume sweeps");
  keyOnVoiceZero(*spu);
  spu->mixFrames(1U);
  require(spu->readRegister(current_voice_volume_left, current) &&
              current == 0x5800U &&
              spu->readRegister(current_main_volume_left, current) &&
              current == 0x5800U,
          "SPU voice/main volume sweep used the wrong first step");
  spu->mixFrames(1U);
  require(spu->readRegister(current_voice_volume_left, current) &&
              current == 0x7fffU &&
              spu->readRegister(current_main_volume_left, current) &&
              current == 0x7fffU &&
              spu->state().voice_volume[0U][0U].envelope_active == 0U &&
              spu->state().main_volume[0U].envelope_active == 0U,
          "SPU volume sweep did not clamp and stop at positive full scale");
}

void testAdpcmLoopAndEndx() {
  auto spu = std::make_unique<sf::psx::Spu>();
  std::array<std::byte, 32U> blocks{};
  blocks[0] = static_cast<std::byte>(0x0cU);
  blocks[1] = static_cast<std::byte>(0x04U);
  std::fill(blocks.begin() + 2, blocks.begin() + 16,
            static_cast<std::byte>(0x11U));
  blocks[16] = static_cast<std::byte>(0x0cU);
  blocks[17] = static_cast<std::byte>(0x03U);
  std::fill(blocks.begin() + 18, blocks.end(), static_cast<std::byte>(0x22U));
  writeRamBytes(*spu, 0U, blocks);
  keyOnVoiceZero(*spu, 0x1000U);

  spu->mixFrames(28U);
  require(spu->endx() == 0U && spu->state().voices[0].block_address == 16U &&
              spu->state().voices[0].repeat_address == 0U,
          "SPU did not retain the ADPCM loop-start address");
  spu->mixFrames(28U);
  require((spu->endx() & 1U) != 0U && spu->state().voices[0].active == 1U &&
              spu->state().voices[0].block_address == 0U,
          "SPU did not set ENDX and repeat the ADPCM loop");

  std::uint16_t endx_value = 0U;
  require(spu->readRegister(endx_low, endx_value) && endx_value == 1U,
          "ENDX register did not expose the completed voice");
  require(spu->writeRegister(key_on_low, 1U),
          "Could not restart the looped voice");
  require((spu->endx() & 1U) == 0U, "Key-on did not clear the voice ENDX bit");
}

void testAdpcmAddressMaskAndLateLoopRegisterWrite() {
  auto spu = std::make_unique<sf::psx::Spu>();
  std::array<std::byte, 48U> blocks{};
  blocks[0U] = std::byte{0x0cU};
  blocks[16U] = std::byte{0x0cU};
  blocks[17U] = std::byte{0x04U};
  blocks[32U] = std::byte{0x0cU};
  std::fill(blocks.begin() + 2U, blocks.begin() + 16U, std::byte{0x11U});
  std::fill(blocks.begin() + 18U, blocks.begin() + 32U, std::byte{0x22U});
  std::fill(blocks.begin() + 34U, blocks.end(), std::byte{0x33U});
  writeRamBytes(*spu, 0U, blocks);

  require(spu->writeRegister(voice_pitch, 0x1000U) &&
              spu->writeRegister(voice_start_address, 1U) &&
              spu->writeRegister(voice_repeat_address, 1U) &&
              spu->writeRegister(control, control_enable) &&
              spu->writeRegister(key_on_low, 1U),
          "Could not configure odd-address SPU ADPCM voice");
  require(spu->state().voices[0].block_address == 0U &&
              spu->state().voices[0].repeat_address == 0U,
          "SPU did not mask the ADPCM address-register low bit");

  spu->mixFrames(28U);
  require(spu->state().voices[0].block_address == 16U &&
              spu->state().voices[0].first_block == 0U,
          "SPU did not finish the first ADPCM block");
  require(spu->writeRegister(voice_repeat_address, 4U),
          "Could not write a late SPU repeat address");
  spu->mixFrames(1U);
  std::uint16_t repeat_register{};
  require(spu->state().voices[0].repeat_address == 32U &&
              spu->state().voices[0].ignore_loop_address == 1U &&
              spu->readRegister(voice_repeat_address, repeat_register) &&
              repeat_register == 4U,
          "Late repeat-address write was overwritten by a loop-start flag");
}

void testCdInputMixer() {
  auto spu = std::make_unique<sf::psx::Spu>();
  const std::array input{sf::psx::SpuPcmFrame{12000, -8000}};
  std::array<sf::psx::SpuPcmFrame, 1U> output{};

  require(spu->writeRegister(main_volume_left, 0x2000U) &&
              spu->writeRegister(main_volume_right, 0x2000U) &&
              spu->writeRegister(cd_volume_left, 0x4000U) &&
              spu->writeRegister(cd_volume_right, 0x4000U),
          "Could not configure the CD mixer volumes");
  require(spu->writeRegister(control, control_enable | control_unmute |
                                          control_cd_enable),
          "Could not enable the CD mixer");
  require(spu->pushCdAudio(input) == input.size(), "SPU rejected CD input PCM");
  spu->mixFrames(1U);
  require(spu->queuedCdFrames() == 0U,
          "SPU did not consume the CD input frame");
  require(spu->takePcm(output) == 1U, "SPU did not produce CD mixer output");
  require(output[0] == sf::psx::SpuPcmFrame{3000, -2000},
          "SPU CD volume or main volume scaling is incorrect");

  spu->setCdInputMixer({0U, 0x80U, 0x80U, 0U});
  require(spu->pushCdAudio(input) == input.size(),
          "SPU rejected matrix-routing CD input PCM");
  spu->mixFrames(1U);
  require(spu->takePcm(output) == 1U &&
              output[0] == sf::psx::SpuPcmFrame{-2000, 3000},
          "CD controller stereo matrix was not applied at SPU consumption");
}

void testSnapshotRestoresNextPcmExactly() {
  auto spu = std::make_unique<sf::psx::Spu>();
  std::array<std::byte, 16U> block{};
  block[0] = static_cast<std::byte>(0x1cU);
  block[1] = static_cast<std::byte>(0x00U);
  std::fill(block.begin() + 2, block.end(), static_cast<std::byte>(0x21U));
  writeRamBytes(*spu, 0U, block);

  require(spu->writeRegister(voice_volume_left, 0x2000U) &&
              spu->writeRegister(voice_volume_right, 0x2000U) &&
              spu->writeRegister(voice_pitch, 0x1800U) &&
              spu->writeRegister(voice_start_address, 0U) &&
              spu->writeRegister(voice_repeat_address, 0U) &&
              spu->writeRegister(main_volume_left, 0x3000U) &&
              spu->writeRegister(main_volume_right, 0x3000U) &&
              spu->writeRegister(cd_volume_left, 0x4000U) &&
              spu->writeRegister(cd_volume_right, 0x4000U) &&
              spu->writeRegister(control, control_enable | control_unmute |
                                              control_cd_enable) &&
              spu->writeRegister(key_on_low, 1U),
          "Could not configure snapshot audio source");

  auto cd_frames = std::make_unique<std::vector<sf::psx::SpuPcmFrame>>();
  cd_frames->reserve(40U);
  for (std::int16_t index = 0; index < 40; ++index) {
    cd_frames->push_back(sf::psx::SpuPcmFrame{
        static_cast<std::int16_t>(1000 + index * 37),
        static_cast<std::int16_t>(-700 - index * 29),
    });
  }
  require(spu->pushCdAudio(*cd_frames) == cd_frames->size(),
          "SPU rejected snapshot CD input");

  spu->mixFrames(7U);
  spu->clearPcm();
  auto snapshot = std::make_unique<sf::psx::SpuState>();
  *snapshot = spu->state();
  auto expected = std::make_unique<std::vector<sf::psx::SpuPcmFrame>>(20U);
  auto actual = std::make_unique<std::vector<sf::psx::SpuPcmFrame>>(20U);

  spu->mixFrames(expected->size());
  require(spu->takePcm(*expected) == expected->size(),
          "Could not collect pre-restore PCM");
  require(std::any_of(expected->begin(), expected->end(),
                      [](const sf::psx::SpuPcmFrame &frame) {
                        return frame.left != 0 || frame.right != 0;
                      }),
          "Snapshot test source produced only silence");

  require(spu->restoreState(*snapshot), "Could not restore valid SPU state");
  require(spu->queuedPcmFrames() == 0U,
          "Restore retained stale host PCM output");
  spu->mixFrames(actual->size());
  require(spu->takePcm(*actual) == actual->size(),
          "Could not collect post-restore PCM");
  require(*actual == *expected,
          "Restored SPU did not reproduce the next PCM bit-exactly");

  // Machine/gameplay snapshots deliberately exclude the host-facing mixer
  // queue. The explicit copy/restore API remains useful to host sinks that
  // own an unsubmitted presentation boundary, so verify its ordering and
  // overflow diagnostics independently.
  spu->mixFrames(12U);
  std::vector<sf::psx::SpuPcmFrame> pending(spu->queuedPcmFrames());
  require(spu->copyPcm(pending) == pending.size(),
          "Could not copy the pending host PCM boundary");
  std::vector<sf::psx::SpuPcmFrame> drained(pending.size());
  require(spu->takePcm(drained) == drained.size() && drained == pending,
          "Copying pending PCM consumed or reordered it");
  require(spu->restorePcm(pending, 17U) &&
              spu->queuedPcmFrames() == pending.size() &&
              spu->droppedPcmFrames() == 17U,
          "Could not restore the host PCM boundary");
  std::fill(drained.begin(), drained.end(), sf::psx::SpuPcmFrame{});
  require(spu->takePcm(drained) == drained.size() && drained == pending,
          "Restored host PCM differs from the captured boundary");
}

void testInvalidStateRejection() {
  auto spu = std::make_unique<sf::psx::Spu>();
  auto baseline = std::make_unique<sf::psx::SpuState>();
  auto invalid = std::make_unique<sf::psx::SpuState>();
  *baseline = spu->state();
  require(spu->validateState(*baseline), "Fresh SPU state was invalid");

  *invalid = *baseline;
  invalid->sample_clock = sf::psx::Spu::cpu_clock_hz;
  require(!spu->validateState(*invalid) && !spu->restoreState(*invalid),
          "Out-of-range SPU sample clock was accepted");
  require(spu->state() == *baseline,
          "Rejected sample-clock state changed the SPU");

  *invalid = *baseline;
  invalid->voices[0].active = 2U;
  require(!spu->validateState(*invalid) && !spu->restoreState(*invalid),
          "Invalid SPU voice flag was accepted");
  require(spu->state() == *baseline, "Rejected voice state changed the SPU");

  *invalid = *baseline;
  invalid->cd_frame_count = 1U;
  require(!spu->validateState(*invalid) && !spu->restoreState(*invalid),
          "Inconsistent SPU CD queue state was accepted");
  require(spu->state() == *baseline, "Rejected CD queue state changed the SPU");

  *invalid = *baseline;
  invalid->reverb_resample_position = 64U;
  require(!spu->validateState(*invalid) && !spu->restoreState(*invalid),
          "Out-of-range SPU reverb position was accepted");
  require(spu->state() == *baseline, "Rejected reverb state changed the SPU");
}

void testVabSoundUsesRetailSampleAndSpuPath() {
  constexpr std::size_t descriptor_offset = 0x7a0U;
  constexpr std::size_t tone_offset = 0x820U;
  constexpr std::size_t size_table_offset = 0xa20U;
  std::vector<std::byte> header(0xc20U);
  header[0] = std::byte{'B'};
  header[1] = std::byte{'E'};
  header[2] = std::byte{'E'};
  header[3] = std::byte{'P'};
  writeLe32(header.data() + 4U, static_cast<std::uint32_t>(descriptor_offset));
  writeLe16(header.data() + 8U, 5U);
  header[12U] = std::byte{1U};
  writeLe16(header.data() + 0x12U, 1U);
  writeLe16(header.data() + 0x14U, 1U);
  writeLe16(header.data() + 0x16U, 1U);
  header[0x18U] = std::byte{0x7fU};

  header[0x20U] = std::byte{1U};
  header[0x21U] = std::byte{0x7fU};
  header[descriptor_offset + 5U] = std::byte{0U};
  header[descriptor_offset + 7U] = std::byte{60U};
  header[descriptor_offset + 8U] = std::byte{0U};
  header[descriptor_offset + 9U] = std::byte{80U};
  header[descriptor_offset + 10U] = std::byte{0U};

  header[tone_offset + 2U] = std::byte{0x7fU};
  header[tone_offset + 3U] = std::byte{0x40U};
  header[tone_offset + 4U] = std::byte{72U};
  header[tone_offset + 6U] = std::byte{0U};
  header[tone_offset + 7U] = std::byte{0x7fU};
  writeLe16(header.data() + tone_offset + 14U, 0xb2b1U);
  writeLe16(header.data() + tone_offset + 16U, 0x80ffU);
  writeLe16(header.data() + tone_offset + 20U, 0U);
  writeLe16(header.data() + tone_offset + 22U, 1U);

  std::array<std::byte, 32U> body{};
  body[0U] = std::byte{0x00U};
  body[1U] = std::byte{0U};
  std::fill(body.begin() + 2U, body.begin() + 16U, std::byte{0x77U});
  body[16U] = std::byte{0x00U};
  body[17U] = std::byte{1U};
  std::fill(body.begin() + 18U, body.end(), std::byte{0x77U});
  writeLe16(header.data() + size_table_offset + 2U,
            static_cast<std::uint16_t>(body.size() / 8U));

  const auto decoded = sf::psx::decodeVabSound(header, body, 0U);
  require(decoded.succeeded() && decoded.vab_id == 5U &&
              decoded.program == 0U && decoded.sample == 1U &&
              decoded.pitch == 0x0800U && decoded.volume_left == 10320U &&
              decoded.volume_right == 10320U,
          "VAB descriptor did not preserve retail program/pitch/volume");
  require(decoded.frames.size() >= 56U && decoded.frames.size() <= 128U &&
              std::any_of(decoded.frames.begin(), decoded.frames.end(),
                          [](const auto &frame) {
                            return frame.left != 0 && frame.left == frame.right;
                          }),
          "VAB decoder did not render audible stereo SPU PCM");

  const auto truncated = sf::psx::decodeVabSound(
      header, std::span<const std::byte>{body}.first(16U), 0U);
  require(truncated.status == sf::psx::VabDecodeStatus::invalid_sample &&
              truncated.frames.empty(),
          "VAB decoder accepted a truncated sample body");
}

} // namespace

int main() {
  try {
    testRegisterAccess();
    testDmaTransfer();
    testIrqLatchAndClear();
    testCpuTickCadence();
    testIdleFastForward();
    testAdpcmFilterVectors();
    testHardwarePitchClampAndPitchModulation();
    testHardwareNoiseGeneratorCadence();
    testHardwareVolumeSweeps();
    testAdpcmLoopAndEndx();
    testAdpcmAddressMaskAndLateLoopRegisterWrite();
    testCdInputMixer();
    testSnapshotRestoresNextPcmExactly();
    testInvalidStateRejection();
    testVabSoundUsesRetailSampleAndSpuPath();
  } catch (const std::exception &error) {
    std::cerr << "spu_tests failed: " << error.what() << '\n';
    return 1;
  }

  std::cout << "spu_tests passed\n";
  return 0;
}
