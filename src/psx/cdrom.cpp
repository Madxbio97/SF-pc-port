#include "sf/psx/cdrom.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace sf::psx {
namespace {

constexpr std::uint8_t command_get_stat = 0x01U;
constexpr std::uint8_t command_setloc = 0x02U;
constexpr std::uint8_t command_read_n = 0x06U;
constexpr std::uint8_t command_stop = 0x08U;
constexpr std::uint8_t command_pause = 0x09U;
constexpr std::uint8_t command_init = 0x0aU;
constexpr std::uint8_t command_mute = 0x0bU;
constexpr std::uint8_t command_demute = 0x0cU;
constexpr std::uint8_t command_setfilter = 0x0dU;
constexpr std::uint8_t command_setmode = 0x0eU;
constexpr std::uint8_t command_seek_l = 0x15U;
constexpr std::uint8_t command_read_s = 0x1bU;

constexpr std::uint8_t status_error = 1U << 0U;
constexpr std::uint8_t status_motor = 1U << 1U;
constexpr std::uint8_t status_id_error = 1U << 3U;
constexpr std::uint8_t status_shell_open = 1U << 4U;
constexpr std::uint8_t status_reading = 1U << 5U;
constexpr std::uint8_t status_seeking = 1U << 6U;

constexpr std::uint8_t mode_whole_sector = 1U << 5U;
constexpr std::uint8_t mode_filter_enabled = 1U << 3U;
constexpr std::uint8_t mode_xa_adpcm_enabled = 1U << 6U;
constexpr std::uint8_t mode_double_speed = 1U << 7U;
constexpr std::uint8_t submode_audio = 1U << 2U;
constexpr std::uint8_t submode_form2 = 1U << 5U;
constexpr std::uint8_t submode_realtime = 1U << 6U;
constexpr std::uint8_t submode_end_of_file = 1U << 7U;
constexpr std::uint8_t register_constant_bits = 0xe0U;
constexpr std::uint8_t interrupt_mask = 0x1fU;
constexpr std::uint8_t interrupt_type_mask = 0x07U;
constexpr std::size_t sector_header_size = 12U;
constexpr std::size_t sector_user_end =
    sector_header_size + CdRomMedia::sector_size;
constexpr std::uint32_t absolute_lead_in_sectors = 2U * 75U;

bool byteFlag(std::uint8_t value) noexcept { return value <= 1U; }

bool validSchedule(const CdRomEventSchedule &event) noexcept {
  return event.generation != 0U && byteFlag(event.pending) &&
         ((event.pending != 0U) == (event.delay_ticks != 0U));
}

bool decodeBcd(std::uint8_t value, std::uint32_t limit,
               std::uint32_t &decoded) noexcept {
  const auto high = static_cast<std::uint32_t>(value >> 4U);
  const auto low = static_cast<std::uint32_t>(value & 0x0fU);
  if (high > 9U || low > 9U) {
    return false;
  }
  decoded = high * 10U + low;
  return decoded < limit;
}

std::uint8_t encodeBcd(std::uint32_t value) noexcept {
  value %= 100U;
  return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

bool completionCommand(std::uint8_t command) noexcept {
  switch (command) {
  case command_stop:
  case command_pause:
  case command_init:
  case command_seek_l:
    return true;
  default:
    return false;
  }
}

} // namespace

bool CdRomMedia::readRawSector(
    std::uint32_t lba,
    std::span<std::byte, raw_sector_size> destination) noexcept {
  std::fill(destination.begin(), destination.end(), std::byte{0});

  std::span<std::byte, sector_size> user_data(destination.data() + 24U,
                                              sector_size);
  if (!readDataSector(lba, user_data)) {
    std::fill(destination.begin(), destination.end(), std::byte{0});
    return false;
  }

  destination[0] = std::byte{0x00};
  std::fill(destination.begin() + 1U, destination.begin() + 11U,
            std::byte{0xff});
  destination[11] = std::byte{0x00};

  const auto absolute =
      static_cast<std::uint64_t>(lba) + absolute_lead_in_sectors;
  const auto minute = static_cast<std::uint32_t>(absolute / (60U * 75U));
  const auto second = static_cast<std::uint32_t>((absolute / 75U) % 60U);
  const auto frame = static_cast<std::uint32_t>(absolute % 75U);
  destination[12] = static_cast<std::byte>(encodeBcd(minute));
  destination[13] = static_cast<std::byte>(encodeBcd(second));
  destination[14] = static_cast<std::byte>(encodeBcd(frame));
  destination[15] = std::byte{0x02};

  // The synthetic data-only view keeps the historical neutral MODE2
  // subheader. Real 2352-byte media supplies its on-disc Form1/Form2 flags
  // through the readRawSector() override used by XA routing.
  return true;
}

CdRomController::CdRomController(CdRomMedia *media) noexcept : media_(media) {
  reset();
}

void CdRomController::setXaAudioSink(CdRomXaAudioSink *sink) noexcept {
  xa_audio_sink_ = sink;
  if (xa_audio_sink_ != nullptr) {
    xa_audio_sink_->setXaOutputMixer(state_.cd_volume_matrix);
  }
}

void CdRomController::reset() noexcept {
  const auto command_generation =
      nextGeneration(state_.command_event.generation);
  const auto sector_generation = nextGeneration(state_.sector_event.generation);
  state_ = {};
  state_.mode = 0x20U;
  state_.motor_on = media_ != nullptr ? 1U : 0U;
  state_.command_event.generation = command_generation;
  state_.sector_event.generation = sector_generation;
  if (xa_audio_sink_ != nullptr) {
    xa_audio_sink_->resetXaStream();
    xa_audio_sink_->setXaOutputMixer(state_.cd_volume_matrix);
  }
}

void CdRomController::setMedia(CdRomMedia *media) noexcept {
  if (media_ == media) {
    return;
  }
  media_ = media;
  state_.motor_on = media_ != nullptr ? 1U : 0U;
  state_.reading = 0U;
  state_.seeking = 0U;
  cancelEvent(state_.sector_event);
  clearData();
  resetXaStream();
}

bool CdRomController::readRegister(std::uint32_t offset,
                                   std::uint8_t &value) noexcept {
  switch (offset) {
  case 0U: {
    value = static_cast<std::uint8_t>(state_.index & 0x03U);
    if (state_.parameter_count == 0U) {
      value = static_cast<std::uint8_t>(value | (1U << 3U));
    }
    if (state_.parameter_count < CdRomState::fifo_capacity) {
      value = static_cast<std::uint8_t>(value | (1U << 4U));
    }
    if (state_.response_position < state_.response_count) {
      value = static_cast<std::uint8_t>(value | (1U << 5U));
    }
    if (dmaRequest()) {
      value = static_cast<std::uint8_t>(value | (1U << 6U));
    }
    if (state_.command_phase == CdRomCommandPhase::execute) {
      value = static_cast<std::uint8_t>(value | (1U << 7U));
    }
    return true;
  }
  case 1U:
    value = readResponseByte();
    return true;
  case 2U:
    value = readDataByte();
    return true;
  case 3U:
    value = static_cast<std::uint8_t>(register_constant_bits |
                                      (((state_.index & 1U) == 0U)
                                           ? state_.interrupt_enable
                                           : state_.interrupt_flags));
    return true;
  default:
    value = 0xffU;
    return false;
  }
}

bool CdRomController::writeRegister(std::uint32_t offset,
                                    std::uint8_t value) noexcept {
  if (offset != 0U) {
    switch (offset + static_cast<std::uint32_t>(state_.index) * 3U) {
    case 7U:
      state_.pending_cd_volume_matrix[0U] = value;
      return true;
    case 8U:
      state_.pending_cd_volume_matrix[1U] = value;
      return true;
    case 9U:
      state_.pending_cd_volume_matrix[3U] = value;
      return true;
    case 10U:
      state_.pending_cd_volume_matrix[2U] = value;
      return true;
    case 11U:
      state_.adpcm_muted = (value & 0x01U) != 0U ? 1U : 0U;
      if ((value & 0x20U) != 0U) {
        state_.cd_volume_matrix = state_.pending_cd_volume_matrix;
      }
      if (xa_audio_sink_ != nullptr) {
        xa_audio_sink_->setXaOutputMixer(state_.cd_volume_matrix);
      }
      return true;
    default:
      break;
    }
  }
  switch (offset) {
  case 0U:
    state_.index = static_cast<std::uint8_t>(value & 0x03U);
    return true;
  case 1U:
    if (state_.index == 0U) {
      writeCommand(value);
    }
    return true;
  case 2U:
    if (state_.index == 0U) {
      if (state_.parameter_count < CdRomState::fifo_capacity) {
        state_.parameters[state_.parameter_count] = value;
        ++state_.parameter_count;
      }
    } else if (state_.index == 1U) {
      state_.interrupt_enable =
          static_cast<std::uint8_t>(value & interrupt_mask);
    }
    return true;
  case 3U:
    if (state_.index == 0U) {
      if ((value & 0x80U) != 0U && state_.data_valid != 0U) {
        state_.data_position = state_.data_begin;
        state_.data_requested = 1U;
      } else if ((value & 0x80U) == 0U) {
        state_.data_requested = 0U;
      }
    } else if (state_.index == 1U) {
      acknowledgeInterrupt(value);
      if ((value & 0x40U) != 0U) {
        clearParameters();
      }
    }
    return true;
  default:
    return false;
  }
}

bool CdRomController::interruptLine() const noexcept {
  const auto interrupt =
      static_cast<std::uint8_t>(state_.interrupt_flags & interrupt_type_mask);
  if (interrupt == 0U || interrupt > interrupt_error) {
    return false;
  }
  const auto enable_bit = static_cast<std::uint8_t>(1U << (interrupt - 1U));
  return (state_.interrupt_enable & enable_bit) != 0U;
}

bool CdRomController::dmaRequest() const noexcept {
  return state_.data_requested != 0U && state_.data_valid != 0U &&
         state_.data_position < state_.data_end;
}

bool CdRomController::canReadDmaWords(std::uint64_t word_count) const noexcept {
  if (!dmaRequest()) {
    return false;
  }
  const auto available_bytes =
      static_cast<std::uint32_t>(state_.data_end - state_.data_position);
  return word_count <= available_bytes / sizeof(std::uint32_t);
}

bool CdRomController::readDmaWord(std::uint32_t &value) noexcept {
  value = 0U;
  if (!dmaRequest() ||
      static_cast<std::uint32_t>(state_.data_end - state_.data_position) < 4U) {
    return false;
  }

  const auto offset = static_cast<std::size_t>(state_.data_position);
  value = static_cast<std::uint32_t>(
              std::to_integer<std::uint8_t>(state_.data[offset])) |
          (static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(state_.data[offset + 1U]))
           << 8U) |
          (static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(state_.data[offset + 2U]))
           << 16U) |
          (static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(state_.data[offset + 3U]))
           << 24U);
  state_.data_position = static_cast<std::uint16_t>(state_.data_position + 4U);
  finishDataReadIfNeeded();
  return true;
}

void CdRomController::eventCommand(std::uint64_t generation) noexcept {
  if (!consumeEvent(state_.command_event, generation)) {
    return;
  }
  if ((state_.interrupt_flags & interrupt_type_mask) != 0U) {
    armEvent(state_.command_event, interrupt_retry_ticks);
    return;
  }

  switch (state_.command_phase) {
  case CdRomCommandPhase::execute:
    executeCommand();
    break;
  case CdRomCommandPhase::complete:
    completeCommand();
    break;
  case CdRomCommandPhase::idle:
    break;
  }
}

void CdRomController::eventSector(std::uint64_t generation) noexcept {
  if (!consumeEvent(state_.sector_event, generation) || state_.reading == 0U) {
    return;
  }
  if ((state_.interrupt_flags & interrupt_type_mask) != 0U) {
    armEvent(state_.sector_event, interrupt_retry_ticks);
    return;
  }
  const auto load_result = loadSector();
  if (load_result == SectorLoadResult::error) {
    state_.reading = 0U;
    setErrorResponse(0x80U);
    return;
  }
  if (load_result == SectorLoadResult::xa_audio) {
    armEvent(state_.sector_event, sectorDelay());
    return;
  }
  setStatusResponse(interrupt_data_ready);
}

void CdRomController::eventCommand() noexcept {
  eventCommand(state_.command_event.generation);
}

void CdRomController::eventSector() noexcept {
  eventSector(state_.sector_event.generation);
}

bool CdRomController::validateState(const CdRomState &state) const noexcept {
  if (state.parameter_count > CdRomState::fifo_capacity ||
      state.response_count > CdRomState::fifo_capacity ||
      state.response_position > state.response_count || state.index > 3U ||
      (state.interrupt_enable & ~interrupt_mask) != 0U ||
      (state.interrupt_flags & ~interrupt_mask) != 0U ||
      state.interrupt_flags > interrupt_error || !byteFlag(state.motor_on) ||
      !byteFlag(state.reading) || !byteFlag(state.seeking) ||
      !byteFlag(state.muted) || !byteFlag(state.adpcm_muted) ||
      !byteFlag(state.setloc_pending) ||
      !byteFlag(state.xa_current_set) ||
      !byteFlag(state.data_valid) || !byteFlag(state.data_requested) ||
      !validSchedule(state.command_event) ||
      !validSchedule(state.sector_event)) {
    return false;
  }

  switch (state.command_phase) {
  case CdRomCommandPhase::idle:
    if (state.command_event.pending != 0U) {
      return false;
    }
    break;
  case CdRomCommandPhase::execute:
    if (state.command_event.pending == 0U &&
        (state.interrupt_flags & interrupt_type_mask) == 0U) {
      return false;
    }
    break;
  case CdRomCommandPhase::complete:
    if (!completionCommand(state.pending_command) ||
        (state.command_event.pending == 0U &&
         (state.interrupt_flags & interrupt_type_mask) == 0U)) {
      return false;
    }
    break;
  default:
    return false;
  }

  if (state.reading != 0U && state.seeking != 0U) {
    return false;
  }
  if (state.xa_current_set == 0U &&
      (state.xa_current_file != 0U || state.xa_current_channel != 0U)) {
    return false;
  }
  if (state.sector_event.pending != 0U && state.reading == 0U) {
    return false;
  }
  if (state.reading != 0U && state.sector_event.pending == 0U &&
      (state.interrupt_flags & interrupt_type_mask) == 0U) {
    return false;
  }
  if (state.response_count != 0U &&
      (state.interrupt_flags & interrupt_type_mask) == 0U) {
    return false;
  }

  if (state.data_valid == 0U) {
    if (state.data_begin != 0U || state.data_position != 0U ||
        state.data_end != 0U || state.data_requested != 0U) {
      return false;
    }
  } else {
    const auto data_only = state.data_begin == sector_header_size &&
                           state.data_end == sector_user_end;
    const auto whole_sector =
        state.data_begin == 0U && state.data_end == CdRomState::raw_sector_size;
    if ((!data_only && !whole_sector) ||
        state.data_position < state.data_begin ||
        state.data_position >= state.data_end) {
      return false;
    }
  }

  if (media_ == nullptr && (state.motor_on != 0U || state.reading != 0U ||
                            state.seeking != 0U || state.data_valid != 0U)) {
    return false;
  }
  return true;
}

bool CdRomController::restoreState(const CdRomState &state) noexcept {
  if (!validateState(state)) {
    return false;
  }
  state_ = state;
  if (xa_audio_sink_ != nullptr) {
    xa_audio_sink_->setXaOutputMixer(state_.cd_volume_matrix);
  }
  return true;
}

std::uint64_t
CdRomController::nextGeneration(std::uint64_t generation) noexcept {
  return generation == std::numeric_limits<std::uint64_t>::max()
             ? 1U
             : generation + 1U;
}

void CdRomController::armEvent(CdRomEventSchedule &event,
                               std::uint32_t delay_ticks) noexcept {
  if (event.pending != 0U && event.delay_ticks == delay_ticks) {
    return;
  }
  event.generation = nextGeneration(event.generation);
  event.delay_ticks = std::max<std::uint32_t>(1U, delay_ticks);
  event.pending = 1U;
}

void CdRomController::cancelEvent(CdRomEventSchedule &event) noexcept {
  if (event.pending != 0U) {
    event.generation = nextGeneration(event.generation);
  }
  event.delay_ticks = 0U;
  event.pending = 0U;
}

bool CdRomController::consumeEvent(CdRomEventSchedule &event,
                                   std::uint64_t generation) noexcept {
  if (event.pending == 0U || event.generation != generation) {
    return false;
  }
  event.delay_ticks = 0U;
  event.pending = 0U;
  return true;
}

std::uint8_t CdRomController::statusByte(bool error) const noexcept {
  std::uint8_t status{};
  if (error) {
    status = static_cast<std::uint8_t>(status | status_error);
  }
  if (state_.motor_on != 0U) {
    status = static_cast<std::uint8_t>(status | status_motor);
  }
  if (media_ == nullptr) {
    status = static_cast<std::uint8_t>(status | status_shell_open);
  }
  if (state_.reading != 0U) {
    status = static_cast<std::uint8_t>(status | status_reading);
    if ((state_.mode & 0x10U) != 0U) {
      status = static_cast<std::uint8_t>(status | status_id_error);
    }
  }
  if (state_.seeking != 0U) {
    status = static_cast<std::uint8_t>(status | status_seeking);
  }
  return status;
}

std::uint32_t CdRomController::commandCompletionDelay() const noexcept {
  switch (state_.pending_command) {
  case command_pause:
    if (state_.reading == 0U) {
      return short_completion_ticks;
    }
    return (state_.mode & mode_double_speed) != 0U ? pause_double_speed_ticks
                                                   : pause_single_speed_ticks;
  case command_stop:
    if (state_.motor_on == 0U) {
      return short_completion_ticks;
    }
    return (state_.mode & mode_double_speed) != 0U ? stop_double_speed_ticks
                                                   : stop_single_speed_ticks;
  case command_seek_l:
    return seek_completion_ticks;
  case command_init:
  default:
    return short_completion_ticks;
  }
}

std::uint32_t CdRomController::sectorDelay() const noexcept {
  return (state_.mode & mode_double_speed) != 0U ? sector_double_speed_ticks
                                                 : sector_single_speed_ticks;
}

bool CdRomController::parametersMatch(std::uint8_t count) const noexcept {
  return state_.parameter_count == count;
}

bool CdRomController::decodeSetloc(std::uint32_t &lba) const noexcept {
  lba = 0U;
  if (!parametersMatch(3U)) {
    return false;
  }
  std::uint32_t minute{};
  std::uint32_t second{};
  std::uint32_t frame{};
  if (!decodeBcd(state_.parameters[0], 100U, minute) ||
      !decodeBcd(state_.parameters[1], 60U, second) ||
      !decodeBcd(state_.parameters[2], 75U, frame)) {
    return false;
  }
  const auto absolute = minute * 60U * 75U + second * 75U + frame;
  if (absolute < absolute_lead_in_sectors) {
    return false;
  }
  lba = absolute - absolute_lead_in_sectors;
  return true;
}

void CdRomController::writeCommand(std::uint8_t command) noexcept {
  if (state_.command_phase != CdRomCommandPhase::idle) {
    return;
  }
  state_.pending_command = command;
  state_.command_phase = CdRomCommandPhase::execute;
  if ((state_.interrupt_flags & interrupt_type_mask) == 0U) {
    armEvent(state_.command_event,
             command == command_init ? init_delay_ticks : command_delay_ticks);
  }
}

void CdRomController::executeCommand() noexcept {
  const auto command = state_.pending_command;
  const auto fail_parameter_count = [this]() noexcept {
    clearParameters();
    state_.command_phase = CdRomCommandPhase::idle;
    setErrorResponse(0x20U);
  };

  switch (command) {
  case command_get_stat:
    if (!parametersMatch(0U)) {
      fail_parameter_count();
      return;
    }
    state_.command_phase = CdRomCommandPhase::idle;
    setStatusResponse(interrupt_acknowledge);
    return;

  case command_setloc: {
    std::uint32_t lba{};
    if (!parametersMatch(3U)) {
      fail_parameter_count();
      return;
    }
    if (!decodeSetloc(lba)) {
      clearParameters();
      state_.command_phase = CdRomCommandPhase::idle;
      setErrorResponse(0x10U);
      return;
    }
    state_.target_lba = lba;
    state_.setloc_pending = 1U;
    clearParameters();
    state_.command_phase = CdRomCommandPhase::idle;
    setStatusResponse(interrupt_acknowledge);
    return;
  }

  case command_read_n:
  case command_read_s:
    if (!parametersMatch(0U)) {
      fail_parameter_count();
      return;
    }
    if (media_ == nullptr) {
      state_.command_phase = CdRomCommandPhase::idle;
      setErrorResponse(0x80U);
      return;
    }
    if (state_.setloc_pending != 0U) {
      state_.current_lba = state_.target_lba;
      state_.setloc_pending = 0U;
    }
    cancelEvent(state_.sector_event);
    clearData();
    resetXaStream();
    state_.motor_on = 1U;
    state_.seeking = 0U;
    state_.reading = 1U;
    state_.command_phase = CdRomCommandPhase::idle;
    setStatusResponse(interrupt_acknowledge);
    return;

  case command_stop:
    if (!parametersMatch(0U)) {
      fail_parameter_count();
      return;
    }
    state_.reading = 0U;
    state_.seeking = 0U;
    cancelEvent(state_.sector_event);
    clearData();
    resetXaStream();
    state_.command_phase = CdRomCommandPhase::complete;
    setStatusResponse(interrupt_acknowledge);
    return;

  case command_pause:
    if (!parametersMatch(0U)) {
      fail_parameter_count();
      return;
    }
    state_.seeking = 0U;
    cancelEvent(state_.sector_event);
    resetXaStream();
    state_.command_phase = CdRomCommandPhase::complete;
    setStatusResponse(interrupt_acknowledge);
    return;

  case command_init:
    if (!parametersMatch(0U)) {
      fail_parameter_count();
      return;
    }
    state_.mode = 0x20U;
    state_.filter_file = 0U;
    state_.filter_channel = 0U;
    state_.reading = 0U;
    state_.seeking = 0U;
    state_.muted = 0U;
    state_.adpcm_muted = 0U;
    state_.setloc_pending = 0U;
    state_.motor_on = media_ != nullptr ? 1U : 0U;
    state_.current_lba = 0U;
    state_.target_lba = 0U;
    cancelEvent(state_.sector_event);
    clearData();
    resetXaStream();
    if (xa_audio_sink_ != nullptr) {
      xa_audio_sink_->setXaOutputMixer(state_.cd_volume_matrix);
    }
    state_.command_phase = CdRomCommandPhase::complete;
    setStatusResponse(interrupt_acknowledge);
    return;

  case command_mute:
    if (!parametersMatch(0U)) {
      fail_parameter_count();
      return;
    }
    state_.muted = 1U;
    state_.command_phase = CdRomCommandPhase::idle;
    setStatusResponse(interrupt_acknowledge);
    return;

  case command_demute:
    if (!parametersMatch(0U)) {
      fail_parameter_count();
      return;
    }
    state_.muted = 0U;
    state_.command_phase = CdRomCommandPhase::idle;
    setStatusResponse(interrupt_acknowledge);
    return;

  case command_setfilter:
    if (!parametersMatch(2U)) {
      fail_parameter_count();
      return;
    }
    state_.filter_file = state_.parameters[0];
    state_.filter_channel = state_.parameters[1];
    // Setfilter selects a new XA stream immediately.  The controller's
    // automatic file/channel lock must not keep the previously selected
    // stream pinned until its EOF, but queued PCM and decoder history remain
    // intact (matching DuckStation/hardware behaviour).
    state_.xa_current_file = 0U;
    state_.xa_current_channel = 0U;
    state_.xa_current_set = 0U;
    clearParameters();
    state_.command_phase = CdRomCommandPhase::idle;
    setStatusResponse(interrupt_acknowledge);
    return;

  case command_setmode:
    if (!parametersMatch(1U)) {
      fail_parameter_count();
      return;
    }
    state_.mode = state_.parameters[0];
    clearParameters();
    state_.command_phase = CdRomCommandPhase::idle;
    setStatusResponse(interrupt_acknowledge);
    return;

  case command_seek_l:
    if (!parametersMatch(0U)) {
      fail_parameter_count();
      return;
    }
    if (media_ == nullptr) {
      state_.command_phase = CdRomCommandPhase::idle;
      setErrorResponse(0x80U);
      return;
    }
    state_.reading = 0U;
    state_.seeking = 1U;
    state_.motor_on = 1U;
    cancelEvent(state_.sector_event);
    clearData();
    resetXaStream();
    state_.command_phase = CdRomCommandPhase::complete;
    setStatusResponse(interrupt_acknowledge);
    return;

  default:
    clearParameters();
    state_.command_phase = CdRomCommandPhase::idle;
    setErrorResponse(0x40U);
    return;
  }
}

void CdRomController::completeCommand() noexcept {
  const auto command = state_.pending_command;
  state_.command_phase = CdRomCommandPhase::idle;

  switch (command) {
  case command_stop:
    state_.reading = 0U;
    state_.seeking = 0U;
    state_.motor_on = 0U;
    state_.current_lba = 0U;
    break;
  case command_pause:
    state_.reading = 0U;
    state_.seeking = 0U;
    break;
  case command_init:
    break;
  case command_seek_l:
    if (media_ == nullptr) {
      state_.seeking = 0U;
      state_.pending_command = 0U;
      setErrorResponse(0x80U);
      return;
    }
    if (state_.setloc_pending != 0U) {
      state_.current_lba = state_.target_lba;
      state_.setloc_pending = 0U;
    }
    state_.seeking = 0U;
    break;
  default:
    state_.pending_command = 0U;
    setErrorResponse(0x40U);
    return;
  }

  state_.pending_command = 0U;
  setStatusResponse(interrupt_complete);
}

void CdRomController::acknowledgeInterrupt(std::uint8_t value) noexcept {
  state_.interrupt_flags = static_cast<std::uint8_t>(state_.interrupt_flags &
                                                     ~(value & interrupt_mask));
  if ((state_.interrupt_flags & interrupt_type_mask) == 0U) {
    clearResponse();
    scheduleUnblockedWork();
  }
}

void CdRomController::scheduleUnblockedWork() noexcept {
  if ((state_.interrupt_flags & interrupt_type_mask) != 0U) {
    return;
  }
  if (state_.command_phase == CdRomCommandPhase::execute) {
    armEvent(state_.command_event, state_.pending_command == command_init
                                       ? init_delay_ticks
                                       : command_delay_ticks);
    return;
  }
  if (state_.command_phase == CdRomCommandPhase::complete) {
    armEvent(state_.command_event, commandCompletionDelay());
    return;
  }
  if (state_.reading != 0U) {
    armEvent(state_.sector_event, sectorDelay());
  }
}

void CdRomController::clearParameters() noexcept {
  state_.parameters.fill(0U);
  state_.parameter_count = 0U;
}

void CdRomController::clearResponse() noexcept {
  state_.response.fill(0U);
  state_.response_position = 0U;
  state_.response_count = 0U;
}

void CdRomController::clearData() noexcept {
  state_.data.fill(std::byte{0});
  state_.data_begin = 0U;
  state_.data_position = 0U;
  state_.data_end = 0U;
  state_.data_valid = 0U;
  state_.data_requested = 0U;
}

void CdRomController::resetXaStream() noexcept {
  state_.xa_current_file = 0U;
  state_.xa_current_channel = 0U;
  state_.xa_current_set = 0U;
  if (xa_audio_sink_ != nullptr) {
    xa_audio_sink_->resetXaStream();
  }
}

void CdRomController::setResponse(
    std::uint8_t interrupt, std::span<const std::uint8_t> bytes) noexcept {
  clearResponse();
  const auto count = std::min(bytes.size(), CdRomState::fifo_capacity);
  for (std::size_t index = 0U; index < count; ++index) {
    state_.response[index] = bytes[index];
  }
  state_.response_count = static_cast<std::uint8_t>(count);
  state_.interrupt_flags =
      static_cast<std::uint8_t>(interrupt & interrupt_mask);
}

void CdRomController::setStatusResponse(std::uint8_t interrupt) noexcept {
  const std::array response{statusByte()};
  setResponse(interrupt, response);
}

void CdRomController::setErrorResponse(std::uint8_t code) noexcept {
  const std::array response{statusByte(true), code};
  setResponse(interrupt_error, response);
}

std::uint8_t CdRomController::readResponseByte() noexcept {
  if (state_.response_position >= state_.response_count) {
    return 0U;
  }
  const auto value = state_.response[state_.response_position];
  ++state_.response_position;
  if (state_.response_position == state_.response_count) {
    state_.response_position = 0U;
    state_.response_count = 0U;
    state_.response.fill(0U);
  }
  return value;
}

std::uint8_t CdRomController::readDataByte() noexcept {
  if (!dmaRequest()) {
    return 0U;
  }
  const auto value = std::to_integer<std::uint8_t>(
      state_.data[static_cast<std::size_t>(state_.data_position)]);
  ++state_.data_position;
  finishDataReadIfNeeded();
  return value;
}

void CdRomController::finishDataReadIfNeeded() noexcept {
  if (state_.data_valid != 0U && state_.data_position >= state_.data_end) {
    clearData();
  }
}

CdRomController::SectorLoadResult CdRomController::loadSector() noexcept {
  clearData();
  if (media_ == nullptr || state_.current_lba >= media_->sectorCount()) {
    return SectorLoadResult::error;
  }

  std::array<std::byte, CdRomMedia::raw_sector_size> raw_sector{};
  if (!media_->readRawSector(state_.current_lba, raw_sector)) {
    clearData();
    return SectorLoadResult::error;
  }
  ++state_.current_lba;

  const auto duplicated_subheader = std::equal(
      raw_sector.begin() + 16U, raw_sector.begin() + 20U,
      raw_sector.begin() + 20U, raw_sector.begin() + 24U);
  const auto file = std::to_integer<std::uint8_t>(raw_sector[16U]);
  const auto channel = std::to_integer<std::uint8_t>(raw_sector[17U]);
  const auto submode = std::to_integer<std::uint8_t>(raw_sector[18U]);
  const auto xa_audio =
      (state_.mode & mode_xa_adpcm_enabled) != 0U &&
      raw_sector[15U] == std::byte{0x02} && duplicated_subheader &&
      (submode & (submode_audio | submode_form2 | submode_realtime)) ==
          (submode_audio | submode_form2 | submode_realtime);
  if (xa_audio) {
    const auto matches_filter =
        (state_.mode & mode_filter_enabled) == 0U ||
        (file == state_.filter_file && channel == state_.filter_channel);
    if (matches_filter) {
      auto matches_current = true;
      if (state_.xa_current_set == 0U) {
        // A handful of discs contain junk channel 255 sectors. Hardware-like
        // automatic selection ignores those unless software explicitly asks
        // for channel 255 through Setfilter.
        if (channel == 0xffU &&
            ((state_.mode & mode_filter_enabled) == 0U ||
             state_.filter_channel != 0xffU)) {
          matches_current = false;
        } else {
          state_.xa_current_file = file;
          state_.xa_current_channel = channel;
          state_.xa_current_set = 1U;
        }
      } else {
        matches_current = file == state_.xa_current_file &&
                          channel == state_.xa_current_channel;
      }

      if (matches_current) {
        // EOF belongs to this stream, but releases the automatic file lock for
        // the following sector.
        if ((submode & submode_end_of_file) != 0U) {
          state_.xa_current_file = 0U;
          state_.xa_current_channel = 0U;
          state_.xa_current_set = 0U;
        }
        if (xa_audio_sink_ != nullptr) {
          xa_audio_sink_->consumeXaSector(
              raw_sector,
              state_.muted != 0U || state_.adpcm_muted != 0U);
        }
      }
    }
    return SectorLoadResult::xa_audio;
  }

  std::copy_n(raw_sector.begin() + 12U, state_.data.size(),
              state_.data.begin());

  if ((state_.mode & mode_whole_sector) != 0U) {
    state_.data_begin = 0U;
    state_.data_end = static_cast<std::uint16_t>(CdRomState::raw_sector_size);
  } else {
    state_.data_begin = static_cast<std::uint16_t>(sector_header_size);
    state_.data_end = static_cast<std::uint16_t>(sector_user_end);
  }
  state_.data_position = state_.data_begin;
  state_.data_valid = 1U;
  state_.data_requested = 0U;
  return SectorLoadResult::data;
}

} // namespace sf::psx
