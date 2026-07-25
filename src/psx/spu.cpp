#include "sf/psx/spu.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace sf::psx {
namespace {

constexpr std::uint32_t voice_register_span = 0x10U;
constexpr std::uint32_t voice_register_end = 0x180U;
constexpr std::size_t voice_register_count = 8U;

constexpr std::uint32_t register_main_volume_left = 0x180U;
constexpr std::uint32_t register_main_volume_right = 0x182U;
constexpr std::uint32_t register_reverb_volume_left = 0x184U;
constexpr std::uint32_t register_reverb_volume_right = 0x186U;
constexpr std::uint32_t register_key_on_low = 0x188U;
constexpr std::uint32_t register_key_on_high = 0x18aU;
constexpr std::uint32_t register_key_off_low = 0x18cU;
constexpr std::uint32_t register_key_off_high = 0x18eU;
constexpr std::uint32_t register_pitch_modulation_low = 0x190U;
constexpr std::uint32_t register_pitch_modulation_high = 0x192U;
constexpr std::uint32_t register_noise_mode_low = 0x194U;
constexpr std::uint32_t register_noise_mode_high = 0x196U;
constexpr std::uint32_t register_reverb_on_low = 0x198U;
constexpr std::uint32_t register_reverb_on_high = 0x19aU;
constexpr std::uint32_t register_endx_low = 0x19cU;
constexpr std::uint32_t register_endx_high = 0x19eU;
constexpr std::uint32_t register_reverb_base = 0x1a2U;
constexpr std::uint32_t register_irq_address = 0x1a4U;
constexpr std::uint32_t register_transfer_address = 0x1a6U;
constexpr std::uint32_t register_transfer_fifo = 0x1a8U;
constexpr std::uint32_t register_control = 0x1aaU;
constexpr std::uint32_t register_transfer_control = 0x1acU;
constexpr std::uint32_t register_status = 0x1aeU;
constexpr std::uint32_t register_cd_volume_left = 0x1b0U;
constexpr std::uint32_t register_cd_volume_right = 0x1b2U;
constexpr std::uint32_t register_reverb_config_begin = 0x1c0U;
constexpr std::uint32_t register_current_main_volume_left = 0x1b8U;
constexpr std::uint32_t register_current_main_volume_right = 0x1baU;
constexpr std::uint32_t register_current_voice_volume_begin = 0x200U;
constexpr std::uint32_t current_voice_volume_span = 4U;

constexpr std::size_t voice_volume_left = 0U;
constexpr std::size_t voice_volume_right = 1U;
constexpr std::size_t voice_pitch = 2U;
constexpr std::size_t voice_start_address = 3U;
constexpr std::size_t voice_adsr_low = 4U;
constexpr std::size_t voice_adsr_high = 5U;
constexpr std::size_t voice_current_adsr = 6U;
constexpr std::size_t voice_repeat_address = 7U;

constexpr std::uint16_t control_cd_audio_enable = 1U << 0U;
constexpr std::uint16_t control_cd_reverb_enable = 1U << 2U;
constexpr std::uint16_t control_transfer_mode_mask = 3U << 4U;
constexpr std::uint16_t control_irq_enable = 1U << 6U;
constexpr std::uint16_t control_reverb_enable = 1U << 7U;
constexpr std::uint16_t control_unmute = 1U << 14U;
constexpr std::uint16_t control_spu_enable = 1U << 15U;

constexpr std::uint16_t status_irq = 1U << 6U;
constexpr std::uint16_t status_dma_request = 1U << 7U;
constexpr std::uint16_t status_dma_read_request = 1U << 8U;
constexpr std::uint16_t status_dma_write_request = 1U << 9U;
constexpr std::uint16_t status_transfer_busy = 1U << 10U;

constexpr std::uint8_t adpcm_end = 1U << 0U;
constexpr std::uint8_t adpcm_repeat = 1U << 1U;
constexpr std::uint8_t adpcm_loop_start = 1U << 2U;
constexpr std::uint8_t adpcm_flag_mask = 0x07U;
constexpr std::uint32_t adpcm_block_size = 16U;
constexpr std::uint32_t ram_mask =
    static_cast<std::uint32_t>(SpuState::ram_size - 1U);
constexpr std::uint32_t pitch_one = 0x1000U;
constexpr std::uint32_t pitch_mask = 0x3fffU;
constexpr std::uint32_t voice_mask = (1U << SpuState::voice_count) - 1U;
constexpr std::uint32_t maximum_envelope_period = 1U << 22U;
constexpr std::uint32_t noise_fraction = 1U << 16U;
constexpr std::uint32_t reverb_address_mask =
    static_cast<std::uint32_t>((SpuState::ram_size - 1U) / 2U);

constexpr std::array<std::int32_t, 20U> reverb_resample_coefficients{{
    -0x0001, 0x0002,  -0x000a, 0x0023,  -0x0067, 0x010a,  -0x0268,
    0x0534,  -0x0b90, 0x2806,  0x2806,  -0x0b90, 0x0534,  -0x0268,
    0x010a,  -0x0067, 0x0023,  -0x000a, 0x0002,  -0x0001,
}};

constexpr std::array<std::array<std::int32_t, 2U>, 5U> adpcm_coefficients{{
    {{0, 0}},
    {{60, 0}},
    {{115, -52}},
    {{98, -55}},
    {{122, -60}},
}};

// DuckStation's hardware noise generator (originally measured by Dr Hell).
constexpr std::array<std::uint8_t, 64U> noise_wave_add{{
    1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1,
    1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1,
}};
constexpr std::array<std::uint32_t, 4U> noise_frequency_add{{
    0U,
    84U,
    140U,
    180U,
}};

// PlayStation SPU interpolation ROM, shared with DuckStation's hardware path.
constexpr std::array<std::int16_t, 0x200> gaussian_table{{
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
    -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001,
    0x0002, 0x0002, 0x0002, 0x0003, 0x0003, 0x0003, 0x0004, 0x0004, 0x0005,
    0x0005, 0x0006, 0x0007, 0x0007, 0x0008, 0x0009, 0x0009, 0x000a, 0x000b,
    0x000c, 0x000d, 0x000e, 0x000f, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015,
    0x0016, 0x0018, 0x0019, 0x001b, 0x001c, 0x001e, 0x0020, 0x0021, 0x0023,
    0x0025, 0x0027, 0x0029, 0x002c, 0x002e, 0x0030, 0x0033, 0x0035, 0x0038,
    0x003a, 0x003d, 0x0040, 0x0043, 0x0046, 0x0049, 0x004d, 0x0050, 0x0054,
    0x0057, 0x005b, 0x005f, 0x0063, 0x0067, 0x006b, 0x006f, 0x0074, 0x0078,
    0x007d, 0x0082, 0x0087, 0x008c, 0x0091, 0x0096, 0x009c, 0x00a1, 0x00a7,
    0x00ad, 0x00b3, 0x00ba, 0x00c0, 0x00c7, 0x00cd, 0x00d4, 0x00db, 0x00e3,
    0x00ea, 0x00f2, 0x00fa, 0x0101, 0x010a, 0x0112, 0x011b, 0x0123, 0x012c,
    0x0135, 0x013f, 0x0148, 0x0152, 0x015c, 0x0166, 0x0171, 0x017b, 0x0186,
    0x0191, 0x019c, 0x01a8, 0x01b4, 0x01c0, 0x01cc, 0x01d9, 0x01e5, 0x01f2,
    0x0200, 0x020d, 0x021b, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
    0x0283, 0x0293, 0x02a3, 0x02b4, 0x02c4, 0x02d6, 0x02e7, 0x02f9, 0x030b,
    0x031d, 0x0330, 0x0343, 0x0356, 0x036a, 0x037e, 0x0392, 0x03a7, 0x03bc,
    0x03d1, 0x03e7, 0x03fc, 0x0413, 0x042a, 0x0441, 0x0458, 0x0470, 0x0488,
    0x04a0, 0x04b9, 0x04d2, 0x04ec, 0x0506, 0x0520, 0x053b, 0x0556, 0x0572,
    0x058e, 0x05aa, 0x05c7, 0x05e4, 0x0601, 0x061f, 0x063e, 0x065c, 0x067c,
    0x069b, 0x06bb, 0x06dc, 0x06fd, 0x071e, 0x0740, 0x0762, 0x0784, 0x07a7,
    0x07cb, 0x07ef, 0x0813, 0x0838, 0x085d, 0x0883, 0x08a9, 0x08d0, 0x08f7,
    0x091e, 0x0946, 0x096f, 0x0998, 0x09c1, 0x09eb, 0x0a16, 0x0a40, 0x0a6c,
    0x0a98, 0x0ac4, 0x0af1, 0x0b1e, 0x0b4c, 0x0b7a, 0x0ba9, 0x0bd8, 0x0c07,
    0x0c38, 0x0c68, 0x0c99, 0x0ccb, 0x0cfd, 0x0d30, 0x0d63, 0x0d97, 0x0dcb,
    0x0e00, 0x0e35, 0x0e6b, 0x0ea1, 0x0ed7, 0x0f0f, 0x0f46, 0x0f7f, 0x0fb7,
    0x0ff1, 0x102a, 0x1065, 0x109f, 0x10db, 0x1116, 0x1153, 0x118f, 0x11cd,
    0x120b, 0x1249, 0x1288, 0x12c7, 0x1307, 0x1347, 0x1388, 0x13c9, 0x140b,
    0x144d, 0x1490, 0x14d4, 0x1517, 0x155c, 0x15a0, 0x15e6, 0x162c, 0x1672,
    0x16b9, 0x1700, 0x1747, 0x1790, 0x17d8, 0x1821, 0x186b, 0x18b5, 0x1900,
    0x194b, 0x1996, 0x19e2, 0x1a2e, 0x1a7b, 0x1ac8, 0x1b16, 0x1b64, 0x1bb3,
    0x1c02, 0x1c51, 0x1ca1, 0x1cf1, 0x1d42, 0x1d93, 0x1de5, 0x1e37, 0x1e89,
    0x1edc, 0x1f2f, 0x1f82, 0x1fd6, 0x202a, 0x207f, 0x20d4, 0x2129, 0x217f,
    0x21d5, 0x222c, 0x2282, 0x22da, 0x2331, 0x2389, 0x23e1, 0x2439, 0x2492,
    0x24eb, 0x2545, 0x259e, 0x25f8, 0x2653, 0x26ad, 0x2708, 0x2763, 0x27be,
    0x281a, 0x2876, 0x28d2, 0x292e, 0x298b, 0x29e7, 0x2a44, 0x2aa1, 0x2aff,
    0x2b5c, 0x2bba, 0x2c18, 0x2c76, 0x2cd4, 0x2d33, 0x2d91, 0x2df0, 0x2e4f,
    0x2eae, 0x2f0d, 0x2f6c, 0x2fcc, 0x302b, 0x308b, 0x30ea, 0x314a, 0x31aa,
    0x3209, 0x3269, 0x32c9, 0x3329, 0x3389, 0x33e9, 0x3449, 0x34a9, 0x3509,
    0x3569, 0x35c9, 0x3629, 0x3689, 0x36e8, 0x3748, 0x37a8, 0x3807, 0x3867,
    0x38c6, 0x3926, 0x3985, 0x39e4, 0x3a43, 0x3aa2, 0x3b00, 0x3b5f, 0x3bbd,
    0x3c1b, 0x3c79, 0x3cd7, 0x3d35, 0x3d92, 0x3def, 0x3e4c, 0x3ea9, 0x3f05,
    0x3f62, 0x3fbd, 0x4019, 0x4074, 0x40d0, 0x412a, 0x4185, 0x41df, 0x4239,
    0x4292, 0x42eb, 0x4344, 0x439c, 0x43f4, 0x444c, 0x44a3, 0x44fa, 0x4550,
    0x45a6, 0x45fc, 0x4651, 0x46a6, 0x46fa, 0x474e, 0x47a1, 0x47f4, 0x4846,
    0x4898, 0x48e9, 0x493a, 0x498a, 0x49d9, 0x4a29, 0x4a77, 0x4ac5, 0x4b13,
    0x4b5f, 0x4bac, 0x4bf7, 0x4c42, 0x4c8d, 0x4cd7, 0x4d20, 0x4d68, 0x4db0,
    0x4df7, 0x4e3e, 0x4e84, 0x4ec9, 0x4f0e, 0x4f52, 0x4f95, 0x4fd7, 0x5019,
    0x505a, 0x509a, 0x50da, 0x5118, 0x5156, 0x5194, 0x51d0, 0x520c, 0x5247,
    0x5281, 0x52ba, 0x52f3, 0x532a, 0x5361, 0x5397, 0x53cc, 0x5401, 0x5434,
    0x5467, 0x5499, 0x54ca, 0x54fa, 0x5529, 0x5558, 0x5585, 0x55b2, 0x55de,
    0x5609, 0x5632, 0x565b, 0x5684, 0x56ab, 0x56d1, 0x56f6, 0x571b, 0x573e,
    0x5761, 0x5782, 0x57a3, 0x57c3, 0x57e2, 0x57ff, 0x581c, 0x5838, 0x5853,
    0x586d, 0x5886, 0x589e, 0x58b5, 0x58cb, 0x58e0, 0x58f4, 0x5907, 0x5919,
    0x592a, 0x593a, 0x5949, 0x5958, 0x5965, 0x5971, 0x597c, 0x5986, 0x598f,
    0x5997, 0x599e, 0x59a4, 0x59a9, 0x59ad, 0x59b0, 0x59b2, 0x59b3,
}};

constexpr std::size_t registerIndex(std::uint32_t offset) noexcept {
  return static_cast<std::size_t>(offset / sizeof(std::uint16_t));
}

constexpr std::size_t voiceRegisterIndex(std::size_t voice,
                                         std::size_t voice_register) noexcept {
  return voice * voice_register_count + voice_register;
}

constexpr std::uint16_t transferMode(std::uint16_t control) noexcept {
  return static_cast<std::uint16_t>((control & control_transfer_mode_mask) >>
                                    4U);
}

constexpr bool validPhase(SpuAdsrPhase phase) noexcept {
  switch (phase) {
  case SpuAdsrPhase::off:
  case SpuAdsrPhase::attack:
  case SpuAdsrPhase::decay:
  case SpuAdsrPhase::sustain:
  case SpuAdsrPhase::release:
    return true;
  default:
    return false;
  }
}

} // namespace

Spu::Spu()
    : state_(std::make_unique<SpuState>()),
      pcm_queue_(
          std::make_unique<std::array<SpuPcmFrame, pcm_queue_capacity>>()) {
  reset();
}

void Spu::reset() noexcept {
  *state_ = {};
  state_->noise_level = 1U;
  clearPcm();
}

bool Spu::readRegister(std::uint32_t offset, std::uint16_t &value) noexcept {
  value = 0U;
  if (offset >= register_span || (offset & 1U) != 0U) {
    return false;
  }

  if (offset < voice_register_end) {
    const auto voice = static_cast<std::size_t>(offset / voice_register_span);
    const auto voice_register = static_cast<std::size_t>(
        (offset % voice_register_span) / sizeof(std::uint16_t));
    if (voice_register == voice_current_adsr) {
      value = state_->voices[voice].envelope;
    } else {
      value = state_->registers[registerIndex(offset)];
    }
    return true;
  }

  if (offset >= register_current_voice_volume_begin) {
    value = state_->registers[registerIndex(offset)];
    return true;
  }

  switch (offset) {
  case register_endx_low:
    value = static_cast<std::uint16_t>(state_->endx & 0xffffU);
    break;
  case register_endx_high:
    value = static_cast<std::uint16_t>((state_->endx >> 16U) & 0xffU);
    break;
  case register_transfer_fifo:
    value = 0xffffU;
    break;
  case register_status:
    value = status();
    break;
  case register_current_main_volume_left:
    value = std::bit_cast<std::uint16_t>(state_->main_volume[0U].current_level);
    break;
  case register_current_main_volume_right:
    value = std::bit_cast<std::uint16_t>(state_->main_volume[1U].current_level);
    break;
  default:
    value = state_->registers[registerIndex(offset)];
    break;
  }
  return true;
}

bool Spu::writeRegister(std::uint32_t offset, std::uint16_t value) noexcept {
  if (offset >= register_span || (offset & 1U) != 0U) {
    return false;
  }

  if (offset < voice_register_end) {
    const auto voice = static_cast<std::size_t>(offset / voice_register_span);
    const auto voice_register = static_cast<std::size_t>(
        (offset % voice_register_span) / sizeof(std::uint16_t));
    state_->registers[registerIndex(offset)] = value;
    if (voice_register == voice_current_adsr) {
      state_->voices[voice].envelope =
          static_cast<std::uint16_t>(value & 0x7fffU);
      state_->registers[registerIndex(offset)] = state_->voices[voice].envelope;
    } else if (voice_register == voice_repeat_address) {
      auto &voice_state = state_->voices[voice];
      const bool ignore_loop_address =
          voice_state.active == 0U || voice_state.first_block == 0U;
      voice_state.repeat_address = ramAddress(value) & ~0x0fU;
      if (ignore_loop_address) {
        voice_state.ignore_loop_address = 1U;
      }
    } else if (voice_register == voice_volume_left ||
               voice_register == voice_volume_right) {
      const auto channel = static_cast<std::size_t>(
          voice_register == voice_volume_right ? 1U : 0U);
      resetVolumeSweep(state_->voice_volume[voice][channel], value);
      const auto current_offset =
          register_current_voice_volume_begin +
          static_cast<std::uint32_t>(voice) * current_voice_volume_span +
          static_cast<std::uint32_t>(voice_register == voice_volume_right
                                         ? sizeof(std::uint16_t)
                                         : 0U);
      state_->registers[registerIndex(current_offset)] =
          std::bit_cast<std::uint16_t>(
              state_->voice_volume[voice][channel].current_level);
    }
    return true;
  }

  if (offset >= register_current_voice_volume_begin) {
    return true;
  }

  switch (offset) {
  case register_main_volume_left:
  case register_main_volume_right: {
    state_->registers[registerIndex(offset)] = value;
    const auto channel = static_cast<std::size_t>(
        offset == register_main_volume_right ? 1U : 0U);
    resetVolumeSweep(state_->main_volume[channel], value);
    const auto current_offset = channel == 0U
                                    ? register_current_main_volume_left
                                    : register_current_main_volume_right;
    state_->registers[registerIndex(current_offset)] =
        std::bit_cast<std::uint16_t>(
            state_->main_volume[channel].current_level);
    break;
  }
  case register_key_on_low:
    state_->registers[registerIndex(offset)] = value;
    keyOn(value);
    break;
  case register_key_on_high:
    state_->registers[registerIndex(offset)] = value;
    keyOn(static_cast<std::uint32_t>(value & 0x00ffU) << 16U);
    break;
  case register_key_off_low:
    state_->registers[registerIndex(offset)] = value;
    keyOff(value);
    break;
  case register_key_off_high:
    state_->registers[registerIndex(offset)] = value;
    keyOff(static_cast<std::uint32_t>(value & 0x00ffU) << 16U);
    break;
  case register_endx_low:
  case register_endx_high:
  case register_status:
    break;
  case register_transfer_address:
    state_->registers[registerIndex(offset)] = value;
    state_->transfer_address = ramAddress(value);
    touchRam(state_->transfer_address);
    break;
  case register_transfer_fifo:
    state_->registers[registerIndex(offset)] = value;
    writeRamHalfword(value);
    break;
  case register_reverb_base:
    state_->registers[registerIndex(offset)] = value;
    state_->reverb_current_address =
        (static_cast<std::uint32_t>(value) << 2U) & reverb_address_mask;
    break;
  case register_control:
    if ((state_->registers[registerIndex(offset)] & control_spu_enable) != 0U &&
        (value & control_spu_enable) == 0U) {
      for (std::size_t voice = 0U; voice < voice_count; ++voice) {
        auto &voice_state = state_->voices[voice];
        voice_state.active = 0U;
        voice_state.block_valid = 0U;
        voice_state.envelope = 0U;
        voice_state.envelope_counter = 0U;
        voice_state.last_volume = 0;
        voice_state.first_block = 0U;
        voice_state.adsr_phase = SpuAdsrPhase::off;
        state_->registers[voiceRegisterIndex(voice, voice_current_adsr)] = 0U;
      }
    }
    state_->registers[registerIndex(offset)] = value;
    if ((value & control_irq_enable) == 0U) {
      state_->irq_latched = 0U;
    }
    break;
  case register_transfer_control:
    state_->registers[registerIndex(offset)] = value;
    break;
  case register_current_main_volume_left:
  case register_current_main_volume_right: {
    state_->registers[registerIndex(offset)] = value;
    const auto channel = static_cast<std::size_t>(
        offset == register_current_main_volume_right ? 1U : 0U);
    state_->main_volume[channel].current_level =
        std::bit_cast<std::int16_t>(value);
    break;
  }
  default:
    state_->registers[registerIndex(offset)] = value;
    break;
  }
  return true;
}

bool Spu::dmaRequest() const noexcept {
  const auto mode = transferMode(control());
  return mode == 2U || mode == 3U;
}

bool Spu::readDmaWord(std::uint32_t &value) noexcept {
  value = 0U;
  if (transferMode(control()) != 3U) {
    return false;
  }
  const auto low = readRamHalfword();
  const auto high = readRamHalfword();
  value = static_cast<std::uint32_t>(low) |
          (static_cast<std::uint32_t>(high) << 16U);
  return true;
}

bool Spu::writeDmaWord(std::uint32_t value) noexcept {
  if (transferMode(control()) != 2U) {
    return false;
  }
  writeRamHalfword(static_cast<std::uint16_t>(value & 0xffffU));
  writeRamHalfword(static_cast<std::uint16_t>(value >> 16U));
  return true;
}

bool Spu::interruptLine() const noexcept {
  return state_->irq_latched != 0U && (control() & control_irq_enable) != 0U;
}

void Spu::advanceCpuTicks(std::uint64_t ticks) noexcept {
  static_assert(cpu_clock_hz % sample_rate == 0U);
  constexpr auto ticks_per_frame = cpu_clock_hz / sample_rate;

  if (ticks == 1U) {
    state_->sample_clock += sample_rate;
    if (state_->sample_clock < cpu_clock_hz) {
      return;
    }
    state_->sample_clock -= cpu_clock_hz;
    mixFrames(1U);
    return;
  }

  auto frames = ticks / ticks_per_frame;
  const auto remaining_ticks = ticks % ticks_per_frame;
  const auto scaled_remainder =
      state_->sample_clock + remaining_ticks * sample_rate;
  frames += scaled_remainder / cpu_clock_hz;
  state_->sample_clock = scaled_remainder % cpu_clock_hz;

  if (frames == 0U) {
    return;
  }
  if (idleForFastForward()) {
    fastForwardSilentFrames(frames);
    return;
  }

  while (frames != 0U) {
    const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
        frames, std::numeric_limits<std::size_t>::max()));
    mixFrames(chunk);
    frames -= chunk;
  }
}

void Spu::mixFrames(std::size_t frame_count) noexcept {
  for (std::size_t frame_index = 0U; frame_index < frame_count; ++frame_index) {
    const auto raw_cd_frame = popCdFrame();
    SpuPcmFrame cd_frame{};
    cd_frame.left = clampSample(
        floorDivPowerOfTwoWide(static_cast<std::int64_t>(raw_cd_frame.left) *
                                   state_->cd_input_matrix[0U],
                               7U) +
        floorDivPowerOfTwoWide(static_cast<std::int64_t>(raw_cd_frame.right) *
                                   state_->cd_input_matrix[2U],
                               7U));
    cd_frame.right = clampSample(
        floorDivPowerOfTwoWide(static_cast<std::int64_t>(raw_cd_frame.left) *
                                   state_->cd_input_matrix[1U],
                               7U) +
        floorDivPowerOfTwoWide(static_cast<std::int64_t>(raw_cd_frame.right) *
                                   state_->cd_input_matrix[3U],
                               7U));
    std::int64_t mixed_left = 0;
    std::int64_t mixed_right = 0;
    std::int64_t reverb_left = 0;
    std::int64_t reverb_right = 0;
    const auto spu_control = control();
    const bool spu_enabled = (spu_control & control_spu_enable) != 0U;
    const auto noise_voices =
        static_cast<std::uint32_t>(
            state_->registers[registerIndex(register_noise_mode_low)]) |
        (static_cast<std::uint32_t>(
             state_->registers[registerIndex(register_noise_mode_high)])
         << 16U);
    const auto pitch_modulation_voices =
        static_cast<std::uint32_t>(
            state_->registers[registerIndex(register_pitch_modulation_low)]) |
        (static_cast<std::uint32_t>(
             state_->registers[registerIndex(register_pitch_modulation_high)])
         << 16U);

    if (spu_enabled) {
      auto reverb_voices =
          static_cast<std::uint32_t>(
              state_->registers[registerIndex(register_reverb_on_low)]) |
          (static_cast<std::uint32_t>(
               state_->registers[registerIndex(register_reverb_on_high)])
           << 16U);
      for (std::size_t voice = 0U; voice < voice_count; ++voice) {
        auto &voice_state = state_->voices[voice];
        if (voice_state.active == 0U) {
          voice_state.last_volume = 0;
          reverb_voices >>= 1U;
          continue;
        }

        const bool noise_enabled = (noise_voices & (1U << voice)) != 0U;
        const auto interpolated = voiceSample(voice);
        const auto source = static_cast<std::int64_t>(
            noise_enabled ? std::bit_cast<std::int16_t>(state_->noise_level)
                          : interpolated);
        const auto enveloped =
            floorDivPowerOfTwoWide(source * voice_state.envelope, 15U);
        voice_state.last_volume = static_cast<std::int32_t>(enveloped);
        const auto left_volume = state_->voice_volume[voice][0U].current_level;
        const auto right_volume = state_->voice_volume[voice][1U].current_level;
        const auto voice_left =
            floorDivPowerOfTwoWide(enveloped * left_volume, 15U);
        const auto voice_right =
            floorDivPowerOfTwoWide(enveloped * right_volume, 15U);
        mixed_left += voice_left;
        mixed_right += voice_right;
        if ((reverb_voices & 1U) != 0U) {
          reverb_left += voice_left;
          reverb_right += voice_right;
        }
        reverb_voices >>= 1U;

        auto pitch = state_->registers[voiceRegisterIndex(voice, voice_pitch)];
        if (voice != 0U && (pitch_modulation_voices & (1U << voice)) != 0U) {
          const auto factor = static_cast<std::int64_t>(
              std::clamp(state_->voices[voice - 1U].last_volume, -0x8000,
                         0x7fff) +
              0x8000);
          const auto signed_pitch =
              static_cast<std::int64_t>(std::bit_cast<std::int16_t>(pitch));
          pitch = static_cast<std::uint16_t>(
              floorDivPowerOfTwoWide(signed_pitch * factor, 15U));
        }
        pitch = std::min<std::uint16_t>(pitch,
                                        static_cast<std::uint16_t>(pitch_mask));
        advanceVoice(voice, pitch, noise_enabled);
        advanceEnvelope(voice);
        for (std::size_t channel = 0U; channel < 2U; ++channel) {
          tickVolumeSweep(state_->voice_volume[voice][channel]);
          const auto current_offset =
              register_current_voice_volume_begin +
              static_cast<std::uint32_t>(voice) * current_voice_volume_span +
              static_cast<std::uint32_t>(channel * 2U);
          state_->registers[registerIndex(current_offset)] =
              std::bit_cast<std::uint16_t>(
                  state_->voice_volume[voice][channel].current_level);
        }
      }

      if ((spu_control & control_cd_audio_enable) != 0U) {
        const auto cd_left_volume = static_cast<std::int16_t>(
            state_->registers[registerIndex(register_cd_volume_left)]);
        const auto cd_right_volume = static_cast<std::int16_t>(
            state_->registers[registerIndex(register_cd_volume_right)]);
        mixed_left += floorDivPowerOfTwoWide(
            static_cast<std::int64_t>(cd_frame.left) * cd_left_volume, 15U);
        mixed_right += floorDivPowerOfTwoWide(
            static_cast<std::int64_t>(cd_frame.right) * cd_right_volume, 15U);
        if ((spu_control & control_cd_reverb_enable) != 0U) {
          reverb_left += floorDivPowerOfTwoWide(
              static_cast<std::int64_t>(cd_frame.left) * cd_left_volume, 15U);
          reverb_right += floorDivPowerOfTwoWide(
              static_cast<std::int64_t>(cd_frame.right) * cd_right_volume, 15U);
        }
      }

      if ((spu_control & control_unmute) == 0U) {
        mixed_left = 0;
        mixed_right = 0;
        reverb_left = 0;
        reverb_right = 0;
      }

      const auto reverb =
          processReverb(clampSample(reverb_left), clampSample(reverb_right));
      mixed_left += reverb.left;
      mixed_right += reverb.right;

      const auto main_left = state_->main_volume[0U].current_level;
      const auto main_right = state_->main_volume[1U].current_level;
      mixed_left = floorDivPowerOfTwoWide(
          static_cast<std::int64_t>(clampSample(mixed_left)) * main_left, 15U);
      mixed_right = floorDivPowerOfTwoWide(
          static_cast<std::int64_t>(clampSample(mixed_right)) * main_right,
          15U);
    }

    for (std::size_t channel = 0U; channel < 2U; ++channel) {
      tickVolumeSweep(state_->main_volume[channel]);
      const auto current_offset = channel == 0U
                                      ? register_current_main_volume_left
                                      : register_current_main_volume_right;
      state_->registers[registerIndex(current_offset)] =
          std::bit_cast<std::uint16_t>(
              state_->main_volume[channel].current_level);
    }

    // The SPU noise generator is free-running, even when no voice is
    // currently sourcing it.
    advanceNoiseFrames(1U);

    SpuPcmFrame output{};
    if (spu_enabled) {
      output.left = clampSample(mixed_left);
      output.right = clampSample(mixed_right);
    }
    pushPcmFrame(output);
    ++state_->mixed_frames;
  }
}

std::size_t Spu::pushCdAudio(std::span<const SpuPcmFrame> frames) noexcept {
  std::size_t accepted = 0U;
  while (accepted < frames.size() &&
         state_->cd_frame_count < SpuState::cd_queue_capacity) {
    const auto position = static_cast<std::size_t>(state_->cd_write_position);
    state_->cd_audio[position] = frames[accepted];
    state_->cd_write_position = static_cast<std::uint16_t>(
        (position + 1U) % SpuState::cd_queue_capacity);
    ++state_->cd_frame_count;
    ++accepted;
  }
  return accepted;
}

void Spu::clearCdAudio() noexcept {
  state_->cd_audio.fill({});
  state_->cd_read_position = 0U;
  state_->cd_write_position = 0U;
  state_->cd_frame_count = 0U;
}

std::size_t Spu::takePcm(std::span<SpuPcmFrame> destination) noexcept {
  const auto count = std::min(destination.size(), pcm_frame_count_);
  for (std::size_t index = 0U; index < count; ++index) {
    destination[index] = (*pcm_queue_)[pcm_read_position_];
    (*pcm_queue_)[pcm_read_position_] = {};
    pcm_read_position_ = (pcm_read_position_ + 1U) % pcm_queue_capacity;
  }
  pcm_frame_count_ -= count;
  return count;
}

std::size_t Spu::copyPcm(std::span<SpuPcmFrame> destination) const noexcept {
  const auto count = std::min(destination.size(), pcm_frame_count_);
  for (std::size_t index = 0U; index < count; ++index) {
    destination[index] =
        (*pcm_queue_)[(pcm_read_position_ + index) % pcm_queue_capacity];
  }
  return count;
}

bool Spu::restorePcm(std::span<const SpuPcmFrame> frames,
                     std::uint64_t dropped_frames) noexcept {
  if (frames.size() > pcm_queue_capacity) {
    return false;
  }
  pcm_queue_->fill({});
  std::copy(frames.begin(), frames.end(), pcm_queue_->begin());
  pcm_read_position_ = 0U;
  pcm_write_position_ = frames.size() % pcm_queue_capacity;
  pcm_frame_count_ = frames.size();
  dropped_pcm_frames_ = dropped_frames;
  return true;
}

void Spu::clearPcm() noexcept {
  pcm_queue_->fill({});
  pcm_read_position_ = 0U;
  pcm_write_position_ = 0U;
  pcm_frame_count_ = 0U;
  dropped_pcm_frames_ = 0U;
}

std::uint16_t Spu::control() const noexcept {
  return state_->registers[registerIndex(register_control)];
}

std::uint16_t Spu::status() const noexcept {
  auto value = static_cast<std::uint16_t>(control() & 0x003fU);
  if (state_->irq_latched != 0U) {
    value = static_cast<std::uint16_t>(value | status_irq);
  }

  if (state_->transfer_busy != 0U) {
    value = static_cast<std::uint16_t>(value | status_transfer_busy);
  }

  const auto mode = transferMode(control());
  if (mode == 2U) {
    value = static_cast<std::uint16_t>(value | status_dma_request |
                                       status_dma_write_request);
  } else if (mode == 3U) {
    value = static_cast<std::uint16_t>(value | status_dma_request |
                                       status_dma_read_request);
  }
  return value;
}

bool Spu::validateState(const SpuState &state) const noexcept {
  const auto valid_sweep = [](const SpuVolumeSweepState &sweep) noexcept {
    return sweep.envelope.counter < 0x8000U &&
           sweep.envelope.counter_increment <= 0x8000U &&
           sweep.envelope.rate <= 0x7fU && sweep.envelope.decreasing <= 1U &&
           sweep.envelope.exponential <= 1U &&
           sweep.envelope.phase_invert <= 1U && sweep.envelope_active <= 1U;
  };
  const auto reverb_base =
      (static_cast<std::uint32_t>(
           state.registers[registerIndex(register_reverb_base)])
       << 2U) &
      reverb_address_mask;
  if (state.sample_clock >= cpu_clock_hz ||
      state.transfer_address >= ram_size ||
      (state.transfer_address & 1U) != 0U ||
      state.reverb_current_address > reverb_address_mask ||
      state.reverb_current_address < reverb_base ||
      state.reverb_resample_position >= 64U ||
      (state.endx & ~voice_mask) != 0U || !validFlag(state.irq_latched) ||
      !validFlag(state.transfer_busy) ||
      state.cd_read_position >= SpuState::cd_queue_capacity ||
      state.cd_write_position >= SpuState::cd_queue_capacity ||
      state.cd_frame_count > SpuState::cd_queue_capacity) {
    return false;
  }

  const auto expected_cd_write =
      (static_cast<std::size_t>(state.cd_read_position) +
       state.cd_frame_count) %
      SpuState::cd_queue_capacity;
  if (state.cd_write_position != expected_cd_write) {
    return false;
  }
  for (std::size_t channel = 0U; channel < 2U; ++channel) {
    const auto current_offset = channel == 0U
                                    ? register_current_main_volume_left
                                    : register_current_main_volume_right;
    if (!valid_sweep(state.main_volume[channel]) ||
        state.registers[registerIndex(current_offset)] !=
            std::bit_cast<std::uint16_t>(
                state.main_volume[channel].current_level)) {
      return false;
    }
  }

  for (std::size_t voice = 0U; voice < voice_count; ++voice) {
    const auto &voice_state = state.voices[voice];
    if (!validFlag(voice_state.active) || !validFlag(voice_state.block_valid) ||
        !validFlag(voice_state.first_block) ||
        !validFlag(voice_state.ignore_loop_address) ||
        !validPhase(voice_state.adsr_phase) ||
        voice_state.block_address >= ram_size ||
        (voice_state.block_address & 0x0fU) != 0U ||
        voice_state.repeat_address >= ram_size ||
        (voice_state.repeat_address & 0x0fU) != 0U ||
        voice_state.pitch_counter >= pitch_one ||
        voice_state.sample_index >= SpuVoiceState::samples_per_block ||
        voice_state.envelope > 0x7fffU ||
        (voice_state.block_flags & ~adpcm_flag_mask) != 0U ||
        voice_state.previous_sample <
            std::numeric_limits<std::int16_t>::min() ||
        voice_state.previous_sample >
            std::numeric_limits<std::int16_t>::max() ||
        voice_state.older_sample < std::numeric_limits<std::int16_t>::min() ||
        voice_state.older_sample > std::numeric_limits<std::int16_t>::max()) {
      return false;
    }

    if ((voice_state.active == 0U) !=
            (voice_state.adsr_phase == SpuAdsrPhase::off) ||
        (voice_state.active == 0U && voice_state.block_valid != 0U) ||
        voice_state.envelope_counter >= maximum_envelope_period) {
      return false;
    }
    if (state.registers[voiceRegisterIndex(voice, voice_current_adsr)] !=
            voice_state.envelope ||
        (ramAddress(
             state.registers[voiceRegisterIndex(voice, voice_repeat_address)]) &
         ~0x0fU) != voice_state.repeat_address) {
      return false;
    }

    if (voice_state.adsr_phase == SpuAdsrPhase::off &&
        voice_state.envelope_counter != 0U) {
      return false;
    }

    const auto current_left_offset =
        register_current_voice_volume_begin +
        static_cast<std::uint32_t>(voice) * current_voice_volume_span;
    const auto current_right_offset = current_left_offset + 2U;
    if (!valid_sweep(state.voice_volume[voice][0U]) ||
        !valid_sweep(state.voice_volume[voice][1U]) ||
        state.registers[registerIndex(current_left_offset)] !=
            std::bit_cast<std::uint16_t>(
                state.voice_volume[voice][0U].current_level) ||
        state.registers[registerIndex(current_right_offset)] !=
            std::bit_cast<std::uint16_t>(
                state.voice_volume[voice][1U].current_level)) {
      return false;
    }
  }
  return true;
}

bool Spu::restoreState(const SpuState &state) noexcept {
  if (!validateState(state)) {
    return false;
  }
  *state_ = state;
  clearPcm();
  return true;
}

bool Spu::validFlag(std::uint8_t value) noexcept { return value <= 1U; }

std::uint32_t Spu::ramAddress(std::uint16_t value) noexcept {
  return (static_cast<std::uint32_t>(value) << 3U) & ram_mask;
}

std::int32_t Spu::fixedVolume(std::uint16_t value) noexcept {
  if ((value & 0x8000U) != 0U) {
    return 0;
  }
  auto volume = static_cast<std::int32_t>(value & 0x7fffU);
  if ((volume & 0x4000) != 0) {
    volume -= 0x8000;
  }
  return volume;
}

std::int16_t Spu::clampSample(std::int64_t value) noexcept {
  value = std::clamp(
      value,
      static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::min()),
      static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max()));
  return static_cast<std::int16_t>(value);
}

std::int32_t Spu::floorDivPowerOfTwo(std::int32_t value,
                                     std::uint32_t shift) noexcept {
  const auto divisor = static_cast<std::int64_t>(1) << shift;
  const auto wide_value = static_cast<std::int64_t>(value);
  if (wide_value >= 0) {
    return static_cast<std::int32_t>(wide_value / divisor);
  }
  return static_cast<std::int32_t>(-((-wide_value + divisor - 1) / divisor));
}

std::int64_t Spu::floorDivPowerOfTwoWide(std::int64_t value,
                                         std::uint32_t shift) noexcept {
  const auto divisor = static_cast<std::int64_t>(1) << shift;
  if (value >= 0) {
    return value / divisor;
  }
  return -((-value + divisor - 1) / divisor);
}

void Spu::resetVolumeSweep(SpuVolumeSweepState &sweep,
                           std::uint16_t value) noexcept {
  if ((value & 0x8000U) == 0U) {
    sweep = {};
    sweep.current_level = static_cast<std::int16_t>(fixedVolume(value) * 2);
    return;
  }

  auto &envelope = sweep.envelope;
  envelope = {};
  envelope.rate = static_cast<std::uint8_t>(value & 0x7fU);
  envelope.decreasing = static_cast<std::uint8_t>((value & 0x2000U) != 0U);
  envelope.exponential = static_cast<std::uint8_t>((value & 0x4000U) != 0U);
  envelope.phase_invert = static_cast<std::uint8_t>(
      (value & 0x1000U) != 0U &&
      !(envelope.decreasing != 0U && envelope.exponential != 0U));
  envelope.counter_increment = 0x8000U;

  const auto base_step = static_cast<std::int32_t>(
      7U - (static_cast<std::uint32_t>(envelope.rate) & 3U));
  const bool invert_step =
      ((envelope.decreasing != 0U) ^ (envelope.phase_invert != 0U)) ||
      (envelope.decreasing != 0U && envelope.exponential != 0U);
  auto step = invert_step ? ~base_step : base_step;
  if (envelope.rate < 44U) {
    step *= static_cast<std::int32_t>(1U << (11U - (envelope.rate >> 2U)));
  } else if (envelope.rate >= 48U) {
    envelope.counter_increment = static_cast<std::uint16_t>(
        envelope.counter_increment >> ((envelope.rate >> 2U) - 11U));
    if (envelope.rate != 0x7fU) {
      envelope.counter_increment =
          std::max<std::uint16_t>(envelope.counter_increment, 1U);
    }
  }
  envelope.step = static_cast<std::int16_t>(step);
  sweep.envelope_active =
      static_cast<std::uint8_t>(envelope.counter_increment != 0U);
}

void Spu::tickVolumeSweep(SpuVolumeSweepState &sweep) noexcept {
  if (sweep.envelope_active == 0U) {
    return;
  }

  auto &envelope = sweep.envelope;
  auto counter_increment =
      static_cast<std::uint32_t>(envelope.counter_increment);
  auto step = static_cast<std::int32_t>(envelope.step);
  if (envelope.exponential != 0U) {
    if (envelope.decreasing != 0U) {
      step = static_cast<std::int32_t>(floorDivPowerOfTwoWide(
          static_cast<std::int64_t>(step) * sweep.current_level, 15U));
    } else if (sweep.current_level >= 0x6000) {
      if (envelope.rate < 40U) {
        step = floorDivPowerOfTwo(step, 2U);
      } else if (envelope.rate >= 44U) {
        counter_increment >>= 2U;
      } else {
        step = floorDivPowerOfTwo(step, 1U);
        counter_increment >>= 1U;
      }
    }
  }

  envelope.counter += counter_increment;
  if ((envelope.counter & 0x8000U) == 0U) {
    return;
  }
  envelope.counter = 0U;

  auto level = static_cast<std::int32_t>(sweep.current_level) + step;
  if (envelope.decreasing == 0U) {
    const auto target = step < 0 ? std::numeric_limits<std::int16_t>::min()
                                 : std::numeric_limits<std::int16_t>::max();
    level = std::clamp(
        level,
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max()));
    sweep.current_level = static_cast<std::int16_t>(level);
    sweep.envelope_active = static_cast<std::uint8_t>(level != target);
    return;
  }

  if (envelope.phase_invert != 0U) {
    level = std::clamp(
        level,
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()), 0);
  } else {
    level = std::max(level, 0);
  }
  sweep.current_level = static_cast<std::int16_t>(level);
  sweep.envelope_active = static_cast<std::uint8_t>(level == 0);
}

void Spu::keyOn(std::uint32_t mask) noexcept {
  mask &= voice_mask;
  state_->endx &= ~mask;
  for (std::size_t voice = 0U; voice < voice_count; ++voice) {
    const auto voice_bit = static_cast<std::uint32_t>(1U << voice);
    if ((mask & voice_bit) == 0U) {
      continue;
    }

    auto &voice_state = state_->voices[voice];
    voice_state = {};
    voice_state.block_address =
        ramAddress(
            state_->registers[voiceRegisterIndex(voice, voice_start_address)]) &
        ~0x0fU;
    voice_state.repeat_address =
        ramAddress(
            state_
                ->registers[voiceRegisterIndex(voice, voice_repeat_address)]) &
        ~0x0fU;
    voice_state.envelope = 0U;
    voice_state.adsr_phase = SpuAdsrPhase::attack;
    voice_state.active = 1U;
    voice_state.first_block = 1U;
    state_->registers[voiceRegisterIndex(voice, voice_current_adsr)] =
        voice_state.envelope;
  }
}

void Spu::keyOff(std::uint32_t mask) noexcept {
  mask &= voice_mask;
  for (std::size_t voice = 0U; voice < voice_count; ++voice) {
    const auto voice_bit = static_cast<std::uint32_t>(1U << voice);
    auto &voice_state = state_->voices[voice];
    if ((mask & voice_bit) != 0U && voice_state.active != 0U) {
      voice_state.adsr_phase = SpuAdsrPhase::release;
      voice_state.envelope_counter = 0U;
    }
  }
}

bool Spu::decodeBlock(std::size_t voice_index) noexcept {
  auto &voice = state_->voices[voice_index];
  if (voice.active == 0U) {
    return false;
  }

  // The hardware Gaussian interpolator retains the final three decoded
  // samples of the preceding ADPCM block. Key-on zeroes this history.
  voice.interpolation_samples = {
      voice.decoded_samples[SpuVoiceState::samples_per_block - 3U],
      voice.decoded_samples[SpuVoiceState::samples_per_block - 2U],
      voice.decoded_samples[SpuVoiceState::samples_per_block - 1U],
  };

  const auto address = voice.block_address & ram_mask;
  const auto read_byte = [this, address](std::uint32_t offset) noexcept {
    const auto current_address = (address + offset) & ram_mask;
    touchRam(current_address);
    return std::to_integer<std::uint8_t>(state_->ram[current_address]);
  };

  const auto header = read_byte(0U);
  const auto flags = static_cast<std::uint8_t>(read_byte(1U) & adpcm_flag_mask);
  const auto encoded_shift = static_cast<std::uint32_t>(header & 0x0fU);
  const auto shift = encoded_shift > 12U ? 9U : encoded_shift;
  auto filter = static_cast<std::size_t>((header >> 4U) & 0x0fU);
  if (filter >= adpcm_coefficients.size()) {
    filter = 0U;
  }

  for (std::size_t index = 0U; index < SpuVoiceState::samples_per_block;
       ++index) {
    const auto packed = read_byte(2U + static_cast<std::uint32_t>(index / 2U));
    auto nibble = static_cast<std::int32_t>((index & 1U) == 0U ? packed & 0x0fU
                                                               : packed >> 4U);
    if ((nibble & 8) != 0) {
      nibble -= 16;
    }

    const auto base = floorDivPowerOfTwo(nibble * 4096, shift);
    const auto prediction =
        floorDivPowerOfTwo(
            voice.previous_sample * adpcm_coefficients[filter][0], 6U) +
        floorDivPowerOfTwo(voice.older_sample * adpcm_coefficients[filter][1],
                           6U);
    const auto decoded =
        clampSample(static_cast<std::int64_t>(base) + prediction);
    voice.decoded_samples[index] = decoded;
    voice.older_sample = voice.previous_sample;
    voice.previous_sample = decoded;
  }

  voice.block_flags = flags;
  voice.block_valid = 1U;
  if ((flags & adpcm_loop_start) != 0U && voice.ignore_loop_address == 0U) {
    voice.repeat_address = address;
    state_->registers[voiceRegisterIndex(voice_index, voice_repeat_address)] =
        static_cast<std::uint16_t>(address >> 3U);
  }
  return true;
}

void Spu::finishBlock(std::size_t voice_index, bool noise_enabled) noexcept {
  auto &voice = state_->voices[voice_index];
  voice.sample_index = 0U;
  voice.block_valid = 0U;
  voice.first_block = 0U;

  if ((voice.block_flags & adpcm_end) != 0U) {
    state_->endx |= static_cast<std::uint32_t>(1U << voice_index);
    voice.block_address = voice.repeat_address;
    if ((voice.block_flags & adpcm_repeat) == 0U && !noise_enabled) {
      voice.active = 0U;
      voice.adsr_phase = SpuAdsrPhase::off;
      voice.envelope = 0U;
      voice.envelope_counter = 0U;
      state_->registers[voiceRegisterIndex(voice_index, voice_current_adsr)] =
          0U;
    }
    return;
  }

  voice.block_address = (voice.block_address + adpcm_block_size) & ram_mask;
}

std::int32_t Spu::voiceSample(std::size_t voice_index) noexcept {
  auto &voice = state_->voices[voice_index];
  if (voice.active == 0U) {
    return 0;
  }
  if (voice.block_valid == 0U && !decodeBlock(voice_index)) {
    return 0;
  }

  const auto interpolation_index =
      static_cast<std::size_t>(voice.pitch_counter >> 4U);
  const auto sample_index = static_cast<std::size_t>(voice.sample_index);
  const auto sample_at = [&voice, sample_index](std::int32_t offset) noexcept {
    const auto index = static_cast<std::int32_t>(sample_index) + offset;
    if (index >= 0) {
      return voice.decoded_samples[static_cast<std::size_t>(index)];
    }
    return voice.interpolation_samples[static_cast<std::size_t>(index + 3)];
  };

  auto output =
      static_cast<std::int64_t>(gaussian_table[0x0ffU - interpolation_index]) *
      sample_at(-3);
  output +=
      static_cast<std::int64_t>(gaussian_table[0x1ffU - interpolation_index]) *
      sample_at(-2);
  output +=
      static_cast<std::int64_t>(gaussian_table[0x100U + interpolation_index]) *
      sample_at(-1);
  output += static_cast<std::int64_t>(gaussian_table[interpolation_index]) *
            sample_at(0);
  return static_cast<std::int32_t>(floorDivPowerOfTwoWide(output, 15U));
}

void Spu::advanceVoice(std::size_t voice_index, std::uint16_t pitch,
                       bool noise_enabled) noexcept {
  auto &voice = state_->voices[voice_index];
  if (voice.active == 0U) {
    return;
  }

  auto phase = static_cast<std::uint32_t>(voice.pitch_counter) + pitch;
  while (phase >= pitch_one && voice.active != 0U) {
    phase -= pitch_one;
    ++voice.sample_index;
    if (voice.sample_index >= SpuVoiceState::samples_per_block) {
      finishBlock(voice_index, noise_enabled);
    }
  }
  voice.pitch_counter = static_cast<std::uint16_t>(phase);
}

void Spu::advanceNoiseFrames(std::uint64_t frames) noexcept {
  if (frames == 0U) {
    return;
  }

  const auto noise_clock =
      static_cast<std::uint32_t>((control() >> 8U) & 0x3fU);
  const auto frequency = noise_frequency_add[noise_clock & 3U];
  const auto level_units =
      static_cast<std::uint32_t>(0x8000U >> (noise_clock >> 2U));
  const auto level = level_units * noise_fraction;

  const auto advance_lfsr = [this](std::uint64_t steps) noexcept {
    // The measured 16-bit recurrence has period 65535 for every state
    // except the fixed point 0xffff.
    if (state_->noise_level == 0xffffU) {
      return;
    }
    steps %= 65535U;
    while (steps-- != 0U) {
      state_->noise_level = static_cast<std::uint16_t>(
          static_cast<std::uint16_t>(state_->noise_level << 1U) |
          noise_wave_add[(state_->noise_level >> 10U) & 0x3fU]);
    }
  };

  // Normalize a state left over from a different noise-clock setting and
  // reach the tiny steady remainder cycle using the literal hardware step.
  for (std::size_t warmup = 0U; frames != 0U && warmup < 8U; ++warmup) {
    const auto previous_remainder = state_->noise_count & 0xffffU;
    auto count = static_cast<std::uint64_t>(state_->noise_count) +
                 noise_fraction + frequency;
    if ((count & 0xffffU) >= 210U) {
      count += noise_fraction;
      count -= frequency;
    }
    if (count >= level) {
      count %= level;
      advance_lfsr(1U);
    }
    state_->noise_count = static_cast<std::uint32_t>(count);
    --frames;
    if ((state_->noise_count & 0xffffU) == previous_remainder) {
      break;
    }
  }
  if (frames == 0U) {
    return;
  }

  const auto remainder = state_->noise_count & 0xffffU;
  const auto sum = remainder + frequency;
  const auto increment = static_cast<std::uint32_t>(
      sum >= noise_fraction || sum >= 210U ? 2U : 1U);
  auto quotient = state_->noise_count / noise_fraction;
  std::uint64_t lfsr_steps{};
  if (level_units == 1U) {
    lfsr_steps = frames;
    quotient = 0U;
  } else {
    const auto whole = frames / level_units;
    const auto tail = frames % level_units;
    lfsr_steps =
        whole * increment + (quotient + increment * tail) / level_units;
    quotient =
        static_cast<std::uint32_t>((quotient + increment * tail) % level_units);
  }
  state_->noise_count = quotient * noise_fraction + remainder;
  advance_lfsr(lfsr_steps);
}

std::uint32_t Spu::reverbMemoryAddress(std::uint32_t address) const noexcept {
  const auto base = (static_cast<std::uint32_t>(
                         state_->registers[registerIndex(register_reverb_base)])
                     << 2U) &
                    reverb_address_mask;
  auto offset =
      state_->reverb_current_address + (address & reverb_address_mask);
  if ((offset & (reverb_address_mask + 1U)) != 0U) {
    offset += base;
  }
  return (offset & reverb_address_mask) * 2U;
}

std::int16_t Spu::reverbRead(std::uint16_t address,
                             std::int32_t offset) const noexcept {
  const auto scaled = (static_cast<std::uint32_t>(address) << 2U) +
                      static_cast<std::uint32_t>(offset);
  const auto real_address = reverbMemoryAddress(scaled);
  const auto bits = static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(state_->ram[real_address]) |
      (static_cast<std::uint16_t>(
           std::to_integer<std::uint8_t>(state_->ram[real_address + 1U]))
       << 8U));
  return std::bit_cast<std::int16_t>(bits);
}

void Spu::reverbWrite(std::uint16_t address, std::int16_t value) noexcept {
  const auto real_address =
      reverbMemoryAddress(static_cast<std::uint32_t>(address) << 2U);
  const auto bits = std::bit_cast<std::uint16_t>(value);
  state_->ram[real_address] = static_cast<std::byte>(bits);
  state_->ram[real_address + 1U] = static_cast<std::byte>(bits >> 8U);
}

SpuPcmFrame Spu::processReverb(std::int32_t left, std::int32_t right) noexcept {
  const auto position =
      static_cast<std::size_t>(state_->reverb_resample_position);
  state_->reverb_downsample_buffer[0U][position] =
      state_->reverb_downsample_buffer[0U][position | 0x40U] =
          clampSample(left);
  state_->reverb_downsample_buffer[1U][position] =
      state_->reverb_downsample_buffer[1U][position | 0x40U] =
          clampSample(right);

  const auto config = [this](std::size_t index) noexcept {
    return state_->registers[registerIndex(
        register_reverb_config_begin +
        static_cast<std::uint32_t>(index * sizeof(std::uint16_t)))];
  };
  const auto signed_config = [&config](std::size_t index) noexcept {
    return std::bit_cast<std::int16_t>(config(index));
  };
  const auto shifted = [](std::int64_t value, std::uint32_t amount) noexcept {
    return floorDivPowerOfTwoWide(value, amount);
  };
  const auto negative = [](std::int32_t value) noexcept {
    return value == std::numeric_limits<std::int16_t>::min()
               ? static_cast<std::int32_t>(
                     std::numeric_limits<std::int16_t>::max())
               : -value;
  };

  std::array<std::int32_t, 2U> output{};
  if ((position & 1U) != 0U) {
    std::array<std::int32_t, 2U> downsampled{};
    for (std::size_t channel = 0U; channel < 2U; ++channel) {
      const auto start = (position - 38U) & 0x3fU;
      std::int64_t sum = static_cast<std::int64_t>(0x4000) *
                         state_->reverb_downsample_buffer[channel][start + 19U];
      for (std::size_t tap = 0U; tap < reverb_resample_coefficients.size();
           ++tap) {
        sum += static_cast<std::int64_t>(reverb_resample_coefficients[tap]) *
               state_->reverb_downsample_buffer[channel][start + tap * 2U];
      }
      downsampled[channel] = clampSample(shifted(sum, 15U));
    }

    const auto control_value = control();
    const auto reverb_enabled = (control_value & control_reverb_enable) != 0U;
    const auto iir_alpha = static_cast<std::int32_t>(signed_config(2U));
    const auto iir_adjust = [iir_alpha](std::int16_t sample) noexcept {
      if (iir_alpha == std::numeric_limits<std::int16_t>::min()) {
        return sample == std::numeric_limits<std::int16_t>::min()
                   ? std::int64_t{0}
                   : static_cast<std::int64_t>(sample) * -65536;
      }
      return static_cast<std::int64_t>(sample) * (32768 - iir_alpha);
    };

    for (std::size_t channel = 0U; channel < 2U; ++channel) {
      if (reverb_enabled) {
        const auto input_a = clampSample(shifted(
            shifted(
                static_cast<std::int64_t>(reverbRead(config(16U + channel))) *
                    signed_config(7U),
                14U) +
                shifted(static_cast<std::int64_t>(downsampled[channel]) *
                            signed_config(30U + channel),
                        14U),
            1U));
        const auto input_b = clampSample(shifted(
            shifted(static_cast<std::int64_t>(
                        reverbRead(config(24U + (channel ^ 1U)))) *
                        signed_config(7U),
                    14U) +
                shifted(static_cast<std::int64_t>(downsampled[channel]) *
                            signed_config(30U + channel),
                        14U),
            1U));
        const auto iir_a = clampSample(shifted(
            shifted(static_cast<std::int64_t>(input_a) * iir_alpha, 14U) +
                shifted(iir_adjust(reverbRead(config(10U + channel), -1)), 14U),
            1U));
        const auto iir_b = clampSample(shifted(
            shifted(static_cast<std::int64_t>(input_b) * iir_alpha, 14U) +
                shifted(iir_adjust(reverbRead(config(18U + channel), -1)), 14U),
            1U));
        reverbWrite(config(10U + channel), iir_a);
        reverbWrite(config(18U + channel), iir_b);
      }

      auto accumulator =
          shifted(static_cast<std::int64_t>(reverbRead(config(12U + channel))) *
                      signed_config(3U),
                  14U);
      accumulator +=
          shifted(static_cast<std::int64_t>(reverbRead(config(14U + channel))) *
                      signed_config(4U),
                  14U);
      accumulator +=
          shifted(static_cast<std::int64_t>(reverbRead(config(20U + channel))) *
                      signed_config(5U),
                  14U);
      accumulator +=
          shifted(static_cast<std::int64_t>(reverbRead(config(22U + channel))) *
                      signed_config(6U),
                  14U);

      const auto feedback_a = reverbRead(
          static_cast<std::uint16_t>(config(26U + channel) - config(0U)));
      const auto feedback_b = reverbRead(
          static_cast<std::uint16_t>(config(28U + channel) - config(1U)));
      const auto mix_a = clampSample(
          shifted(accumulator + shifted(static_cast<std::int64_t>(feedback_a) *
                                            negative(signed_config(8U)),
                                        14U),
                  1U));
      const auto mix_b = clampSample(
          static_cast<std::int64_t>(feedback_a) +
          shifted(shifted(static_cast<std::int64_t>(mix_a) * signed_config(8U),
                          14U) +
                      shifted(static_cast<std::int64_t>(feedback_b) *
                                  negative(signed_config(9U)),
                              14U),
                  1U));
      const auto upsampled = clampSample(
          static_cast<std::int64_t>(feedback_b) +
          shifted(static_cast<std::int64_t>(mix_b) * signed_config(9U), 15U));
      const auto upsample_position = position >> 1U;
      state_->reverb_upsample_buffer[channel][upsample_position] =
          state_->reverb_upsample_buffer[channel][upsample_position | 0x20U] =
              upsampled;
      if (reverb_enabled) {
        reverbWrite(config(26U + channel), mix_a);
        reverbWrite(config(28U + channel), mix_b);
      }
    }

    const auto base =
        (static_cast<std::uint32_t>(
             state_->registers[registerIndex(register_reverb_base)])
         << 2U) &
        reverb_address_mask;
    state_->reverb_current_address =
        (state_->reverb_current_address + 1U) & reverb_address_mask;
    if (state_->reverb_current_address == 0U) {
      state_->reverb_current_address = base;
    }

    for (std::size_t channel = 0U; channel < 2U; ++channel) {
      const auto start = ((position >> 1U) - 19U) & 0x1fU;
      std::int64_t sum{};
      for (std::size_t tap = 0U; tap < reverb_resample_coefficients.size();
           ++tap) {
        sum += static_cast<std::int64_t>(reverb_resample_coefficients[tap]) *
               state_->reverb_upsample_buffer[channel][start + tap];
      }
      output[channel] = clampSample(shifted(sum, 14U));
    }
  } else {
    const auto index = (((position >> 1U) - 19U) & 0x1fU) + 9U;
    output[0U] = state_->reverb_upsample_buffer[0U][index];
    output[1U] = state_->reverb_upsample_buffer[1U][index];
  }

  state_->reverb_resample_position =
      static_cast<std::uint8_t>((position + 1U) & 0x3fU);
  const auto left_volume = std::bit_cast<std::int16_t>(
      state_->registers[registerIndex(register_reverb_volume_left)]);
  const auto right_volume = std::bit_cast<std::int16_t>(
      state_->registers[registerIndex(register_reverb_volume_right)]);
  return SpuPcmFrame{
      clampSample(
          shifted(static_cast<std::int64_t>(output[0U]) * left_volume, 15U)),
      clampSample(
          shifted(static_cast<std::int64_t>(output[1U]) * right_volume, 15U)),
  };
}

void Spu::advanceEnvelope(std::size_t voice_index) noexcept {
  auto &voice = state_->voices[voice_index];
  if (voice.adsr_phase == SpuAdsrPhase::off || voice.active == 0U) {
    return;
  }

  const auto adsr_low =
      state_->registers[voiceRegisterIndex(voice_index, voice_adsr_low)];
  const auto adsr_high =
      state_->registers[voiceRegisterIndex(voice_index, voice_adsr_high)];

  struct Envelope {
    std::uint8_t rate{};
    std::uint8_t rate_mask{};
    bool decreasing{};
    bool exponential{};
  } envelope;
  auto target = std::int32_t{};
  switch (voice.adsr_phase) {
  case SpuAdsrPhase::attack:
    envelope = Envelope{static_cast<std::uint8_t>((adsr_low >> 8U) & 0x7fU),
                        0x7fU, false, (adsr_low & 0x8000U) != 0U};
    target = 0x7fff;
    break;
  case SpuAdsrPhase::decay:
    envelope =
        Envelope{static_cast<std::uint8_t>(((adsr_low >> 4U) & 0x0fU) << 2U),
                 0x7cU, true, true};
    target = std::min<std::int32_t>(
        (static_cast<std::int32_t>(adsr_low & 0x0fU) + 1) * 0x800, 0x7fff);
    break;
  case SpuAdsrPhase::sustain:
    envelope =
        Envelope{static_cast<std::uint8_t>((adsr_high >> 6U) & 0x7fU), 0x7fU,
                 (adsr_high & 0x4000U) != 0U, (adsr_high & 0x8000U) != 0U};
    break;
  case SpuAdsrPhase::release:
    envelope = Envelope{static_cast<std::uint8_t>((adsr_high & 0x1fU) << 2U),
                        0x7cU, true, (adsr_high & 0x0020U) != 0U};
    break;
  case SpuAdsrPhase::off:
    return;
  }

  auto counter_increment = std::uint32_t{0x8000U};
  const auto base_step = static_cast<std::int32_t>(
      7U - (static_cast<std::uint32_t>(envelope.rate) & 3U));
  auto step = envelope.decreasing ? ~base_step : base_step;
  if (envelope.rate < 44U) {
    step *= static_cast<std::int32_t>(std::uint32_t{1U}
                                      << (11U - (envelope.rate >> 2U)));
  } else if (envelope.rate >= 48U) {
    counter_increment >>= (envelope.rate >> 2U) - 11U;
    // All rate bits set is the hardware's never-tick special case.
    if ((envelope.rate & envelope.rate_mask) != envelope.rate_mask) {
      counter_increment = std::max(counter_increment, 1U);
    }
  }

  if (envelope.exponential) {
    if (envelope.decreasing) {
      step = static_cast<std::int32_t>(floorDivPowerOfTwoWide(
          static_cast<std::int64_t>(step) * voice.envelope, 15U));
    } else if (voice.envelope >= 0x6000U) {
      if (envelope.rate < 40U) {
        step /= 4;
      } else if (envelope.rate >= 44U) {
        counter_increment >>= 2U;
      } else {
        step /= 2;
        counter_increment >>= 1U;
      }
    }
  }

  voice.envelope_counter += counter_increment;
  if ((voice.envelope_counter & 0x8000U) == 0U) {
    return;
  }
  voice.envelope_counter = 0U;

  auto current = static_cast<std::int32_t>(voice.envelope);
  current += step;
  current = envelope.decreasing ? std::max(current, 0)
                                : std::clamp(current, -0x8000, 0x7fff);
  voice.envelope = static_cast<std::uint16_t>(current);
  const auto commitEnvelope = [this, voice_index, &voice]() noexcept {
    state_->registers[voiceRegisterIndex(voice_index, voice_current_adsr)] =
        voice.envelope;
  };
  commitEnvelope();

  switch (voice.adsr_phase) {
  case SpuAdsrPhase::attack:
    if (voice.envelope >= target) {
      voice.envelope = static_cast<std::uint16_t>(target);
      commitEnvelope();
      voice.adsr_phase = SpuAdsrPhase::decay;
      voice.envelope_counter = 0U;
    }
    break;
  case SpuAdsrPhase::decay:
    if (voice.envelope <= target) {
      voice.adsr_phase = SpuAdsrPhase::sustain;
      voice.envelope_counter = 0U;
    }
    break;
  case SpuAdsrPhase::sustain:
    break;
  case SpuAdsrPhase::release:
    if (voice.envelope == 0U) {
      voice.active = 0U;
      voice.block_valid = 0U;
      voice.first_block = 0U;
      voice.adsr_phase = SpuAdsrPhase::off;
      voice.envelope_counter = 0U;
    }
    break;
  case SpuAdsrPhase::off:
    break;
  }
}

std::uint16_t Spu::readRamHalfword() noexcept {
  const auto address = state_->transfer_address & ram_mask;
  touchRam(address);
  touchRam((address + 1U) & ram_mask);
  const auto low = std::to_integer<std::uint8_t>(state_->ram[address]);
  const auto high =
      std::to_integer<std::uint8_t>(state_->ram[(address + 1U) & ram_mask]);
  state_->transfer_address = (address + 2U) & ram_mask;
  touchRam(state_->transfer_address);
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(low) |
                                    (static_cast<std::uint16_t>(high) << 8U));
}

void Spu::writeRamHalfword(std::uint16_t value) noexcept {
  const auto address = state_->transfer_address & ram_mask;
  touchRam(address);
  touchRam((address + 1U) & ram_mask);
  state_->ram[address] = static_cast<std::byte>(value & 0x00ffU);
  state_->ram[(address + 1U) & ram_mask] = static_cast<std::byte>(value >> 8U);
  state_->transfer_address = (address + 2U) & ram_mask;
  touchRam(state_->transfer_address);
}

void Spu::touchRam(std::uint32_t address) noexcept {
  if ((control() & control_irq_enable) == 0U) {
    return;
  }
  const auto irq_address =
      ramAddress(state_->registers[registerIndex(register_irq_address)]);
  if ((address & ~7U) == irq_address) {
    state_->irq_latched = 1U;
  }
}

SpuPcmFrame Spu::popCdFrame() noexcept {
  if (state_->cd_frame_count == 0U) {
    return {};
  }
  const auto position = static_cast<std::size_t>(state_->cd_read_position);
  const auto frame = state_->cd_audio[position];
  state_->cd_audio[position] = {};
  state_->cd_read_position =
      static_cast<std::uint16_t>((position + 1U) % SpuState::cd_queue_capacity);
  --state_->cd_frame_count;
  return frame;
}

void Spu::pushPcmFrame(SpuPcmFrame frame) noexcept {
  if (pcm_frame_count_ == pcm_queue_capacity) {
    (*pcm_queue_)[pcm_read_position_] = {};
    pcm_read_position_ = (pcm_read_position_ + 1U) % pcm_queue_capacity;
    --pcm_frame_count_;
    ++dropped_pcm_frames_;
  }
  (*pcm_queue_)[pcm_write_position_] = frame;
  pcm_write_position_ = (pcm_write_position_ + 1U) % pcm_queue_capacity;
  ++pcm_frame_count_;
}

bool Spu::idleForFastForward() const noexcept {
  if (state_->cd_frame_count != 0U ||
      state_->main_volume[0U].envelope_active != 0U ||
      state_->main_volume[1U].envelope_active != 0U ||
      state_->reverb_current_address != 0U ||
      state_->reverb_resample_position != 0U ||
      state_->registers[registerIndex(register_reverb_volume_left)] != 0U ||
      state_->registers[registerIndex(register_reverb_volume_right)] != 0U ||
      state_->registers[registerIndex(register_reverb_base)] != 0U) {
    return false;
  }
  for (auto offset = register_reverb_config_begin;
       offset < register_reverb_config_begin + 32U * 2U; offset += 2U) {
    if (state_->registers[registerIndex(offset)] != 0U) {
      return false;
    }
  }
  const auto reverb_buffer_nonzero = [](const auto &channels) {
    return std::ranges::any_of(channels, [](const auto &channel) {
      return std::ranges::any_of(
          channel, [](std::int16_t sample) { return sample != 0; });
    });
  };
  if (reverb_buffer_nonzero(state_->reverb_downsample_buffer) ||
      reverb_buffer_nonzero(state_->reverb_upsample_buffer)) {
    return false;
  }
  return std::ranges::none_of(state_->voices, [](const SpuVoiceState &voice) {
    return voice.active != 0U;
  });
}

void Spu::fastForwardSilentFrames(std::uint64_t frames) noexcept {
  advanceNoiseFrames(frames);
  state_->mixed_frames += frames;
  if (frames < pcm_queue_capacity) {
    for (std::uint64_t index = 0U; index < frames; ++index) {
      pushPcmFrame({});
    }
    return;
  }

  dropped_pcm_frames_ += static_cast<std::uint64_t>(pcm_frame_count_) + frames -
                         pcm_queue_capacity;
  pcm_queue_->fill({});
  pcm_read_position_ = 0U;
  pcm_write_position_ = 0U;
  pcm_frame_count_ = pcm_queue_capacity;
}

} // namespace sf::psx
