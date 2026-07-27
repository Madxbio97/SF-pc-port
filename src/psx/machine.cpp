#include "sf/psx/machine.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace sf::psx {
namespace {

constexpr std::uint32_t interrupt_base = 0x1f801070U;
constexpr std::uint32_t dma_base = 0x1f801080U;
constexpr std::uint32_t timer_base = 0x1f801100U;
constexpr std::uint32_t cdrom_base = 0x1f801800U;
constexpr std::uint32_t spu_base = 0x1f801c00U;
constexpr std::uint32_t ram_address_mask =
    static_cast<std::uint32_t>(R3000Runtime::ram_size - 1U);
constexpr std::uint64_t maximum_dma_words = 16U * 1024U * 1024U;
constexpr std::uint64_t maximum_linked_list_nodes = 65'536U;
constexpr std::size_t spu_control_register_index = 0x1aaU / 2U;
// DuckStation also accumulates CPU work and runs timing events in slices.
// This small slice removes per-instruction SPU/timer work while keeping an
// interrupt's worst-case recognition latency far below one scanline.
constexpr std::uint64_t maximum_cpu_slice_ticks = 256U;

std::uint32_t accessBits(R3000AccessWidth width) noexcept {
  switch (width) {
  case R3000AccessWidth::byte:
    return 8U;
  case R3000AccessWidth::halfword:
    return 16U;
  case R3000AccessWidth::word:
    return 32U;
  }
  return 0U;
}

std::uint32_t laneShift(std::uint32_t address) noexcept {
  return (address & 3U) * 8U;
}

std::uint32_t laneMask(std::uint32_t address, R3000AccessWidth width) noexcept {
  const auto bits = accessBits(width);
  if (bits == 32U) {
    return 0xffffffffU;
  }
  return ((1U << bits) - 1U) << laneShift(address);
}

std::uint32_t placeLane(std::uint32_t address, R3000AccessWidth width,
                        std::uint32_t value) noexcept {
  return (value << laneShift(address)) & laneMask(address, width);
}

std::uint32_t extractLane(std::uint32_t address, R3000AccessWidth width,
                          std::uint32_t value) noexcept {
  const auto bits = accessBits(width);
  const auto shifted = value >> laneShift(address);
  return bits == 32U ? shifted : shifted & ((1U << bits) - 1U);
}

std::size_t channelIndex(DmaChannel channel) noexcept {
  return static_cast<std::size_t>(channel);
}

DmaChannel channelFromIndex(std::size_t index) noexcept {
  return static_cast<DmaChannel>(static_cast<std::uint8_t>(index));
}

bool spuInterruptLine(const SpuState &state) noexcept {
  constexpr std::uint16_t irq_enable = 1U << 6U;
  return state.irq_latched != 0U &&
         (state.registers[spu_control_register_index] & irq_enable) != 0U;
}

} // namespace

PsxMachine::PsxMachine(R3000Runtime &cpu, CpuClockScale cpu_clock_scale)
    : cpu_(cpu), cpu_clock_scale_(cpu_clock_scale) {
  if (!cpu_clock_scale_.valid()) {
    throw std::invalid_argument{"Invalid PSX CPU clock scale"};
  }
  const auto divisor =
      std::gcd(cpu_clock_scale_.numerator, cpu_clock_scale_.denominator);
  cpu_clock_scale_.numerator /= divisor;
  cpu_clock_scale_.denominator /= divisor;
  cpu_.attachMmioBus(this);
  cdrom_.setXaAudioSink(this);
  dma_ports_[channelIndex(DmaChannel::spu)] = &spu_dma_port_;
  reset();
}

PsxMachine::~PsxMachine() { cpu_.attachMmioBus(nullptr); }

void PsxMachine::reset() noexcept {
  pending_cpu_ticks_ = 0U;
  device_tick_remainder_ = 0U;
  scheduler_.reset();
  interrupts_.reset();
  dma_.reset();
  cdrom_.reset();
  spu_.reset();
  xa_decoder_.reset();
  timers_.reset();
  syncCpuInterruptLine();
}

R3000RunResult PsxMachine::step() noexcept {
  auto result = cpu_.step();
  if (result.reason == R3000StopReason::running) {
    const auto cycles = std::max<std::uint64_t>(1U, result.instructions);
    queueCpuTicks(cycles);
  }
  return result;
}

void PsxMachine::advanceTicks(std::uint64_t ticks) noexcept {
  flushPendingCpuTicks();
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto target =
      ticks > maximum - scheduler_.now() ? maximum : scheduler_.now() + ticks;

  MachineEvent event;
  while (scheduler_.popNextDue(target, event)) {
    advanceDevicesTo(event.deadline);
    dispatchEvent(event);
  }
  advanceDevicesTo(target);
  syncCpuInterruptLine();
}

std::uint64_t PsxMachine::currentTick() const noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return pending_cpu_ticks_ > maximum - scheduler_.now()
             ? maximum
             : scheduler_.now() + pending_cpu_ticks_;
}

std::uint64_t PsxMachine::cpuTicksPerSecond() const noexcept {
  return scaleDeviceTicks(cpu_clock_hz);
}

std::optional<std::uint64_t>
PsxMachine::dmaCompletionTick(DmaChannel channel) const noexcept {
  const auto token = dma_.scheduledToken(channel);
  if (token == 0U) {
    return std::nullopt;
  }
  const auto scheduler = scheduler_.captureState();
  const auto index = channelIndex(channel);
  for (std::size_t event_index = 0U;
       event_index < scheduler.event_count; ++event_index) {
    const auto &event = scheduler.events[event_index];
    if (event.type == MachineEventType::dma_complete &&
        event.index == index && event.token == token) {
      return event.deadline;
    }
  }
  return std::nullopt;
}

bool PsxMachine::completePendingDmaTransfers() noexcept {
  // One completion can make a different channel startable, so allow a bounded
  // number of passes. DMA completion itself clears CHCR busy and therefore
  // cannot reschedule the same request indefinitely.
  for (std::size_t pass = 0U; pass < DmaController::channel_count; ++pass) {
    auto completed_any = false;
    for (std::size_t index = 0U; index < DmaController::channel_count;
         ++index) {
      const auto channel = channelFromIndex(index);
      const auto token = dma_.scheduledToken(channel);
      if (token == 0U) {
        continue;
      }
      if (!scheduler_.cancel(token)) {
        return false;
      }
      dispatchEvent(MachineEvent{
          .deadline = scheduler_.now(),
          .token = token,
          .payload = 0U,
          .type = MachineEventType::dma_complete,
          .index = static_cast<std::uint8_t>(index),
      });
      if (dma_.scheduledToken(channel) == token) {
        return false;
      }
      completed_any = true;
    }
    if (!completed_any) {
      return true;
    }
  }

  for (std::size_t index = 0U; index < DmaController::channel_count; ++index) {
    if (dma_.scheduledToken(channelFromIndex(index)) != 0U) {
      return false;
    }
  }
  return true;
}

bool PsxMachine::completeNextPendingCdRomEvent() noexcept {
  const auto command = cdrom_.commandSchedule();
  const auto sector = cdrom_.sectorSchedule();
  const auto state = scheduler_.captureState();
  for (std::size_t index = 0U; index < state.event_count; ++index) {
    auto event = state.events[index];
    const auto current_command =
        event.type == MachineEventType::cdrom_command && event.index == 0U &&
        command.pending != 0U && event.payload == command.generation;
    const auto current_sector =
        event.type == MachineEventType::cdrom_sector && event.index == 0U &&
        sector.pending != 0U && event.payload == sector.generation;
    if (!current_command && !current_sector) {
      continue;
    }
    if (!scheduler_.cancel(event.token)) {
      return false;
    }
    event.deadline = scheduler_.now();
    dispatchEvent(event);
    return true;
  }
  return false;
}

void PsxMachine::attachDmaPort(DmaChannel channel, DmaPort *port) noexcept {
  const auto index = channelIndex(channel);
  if (index >= dma_ports_.size()) {
    return;
  }
  if (port == nullptr) {
    const auto token = dma_.scheduledToken(channel);
    if (token != 0U) {
      static_cast<void>(scheduler_.cancel(token));
      static_cast<void>(dma_.cancelScheduled(channel, token));
      if (channel == DmaChannel::spu) {
        spu_.setDmaTransferBusy(false);
      }
    }
  }
  dma_ports_[index] = port;
  kickDmaChannels();
}

void PsxMachine::setCdRomMedia(CdRomMedia *media) noexcept {
  cdrom_.setMedia(media);
  syncCdRomSchedules();
  syncCdRomInterruptLine();
  kickDmaChannels();
}

void PsxMachine::serviceDmaRequests() noexcept { kickDmaChannels(); }

void PsxMachine::setVBlank(bool active) noexcept {
  routeTimerInterrupts(timers_.setVBlank(active));
  interrupts_.setLine(InterruptSource::vblank, active);
  syncCpuInterruptLine();
}

void PsxMachine::pulseVBlank() noexcept {
  setVBlank(false);
  setVBlank(true);
  setVBlank(false);
}

void PsxMachine::setHBlank(bool active) noexcept {
  routeTimerInterrupts(timers_.setHBlank(active));
  syncCpuInterruptLine();
}

void PsxMachine::advanceDotClocks(std::uint64_t clocks) noexcept {
  routeTimerInterrupts(timers_.advanceDotClocks(clocks));
  syncCpuInterruptLine();
}

PsxMachineState PsxMachine::captureState() const {
  PsxMachineState state;
  state.cpu_clock_scale = cpu_clock_scale_;
  state.scheduler = scheduler_.captureState();
  state.interrupts = interrupts_.captureState();
  state.dma = dma_.captureState();
  state.cdrom = cdrom_.captureState();
  *state.spu = spu_.state();
  state.xa_decoder = xa_decoder_.captureState();
  state.timers = timers_.snapshot();
  state.pending_cpu_ticks = pending_cpu_ticks_;
  state.device_tick_remainder = device_tick_remainder_;
  return state;
}

bool PsxMachine::validateState(const PsxMachineState &state) const noexcept {
  EventScheduler scheduler;
  InterruptController interrupts;
  DmaController dma;
  CdRomController cdrom{cdrom_.media()};
  if (state.cpu_clock_scale != cpu_clock_scale_ ||
      state.pending_cpu_ticks >= maximum_cpu_slice_ticks ||
      state.device_tick_remainder >= cpu_clock_scale_.numerator ||
      !scheduler.restoreState(state.scheduler) ||
      !interrupts.restoreState(state.interrupts) ||
      !dma.restoreState(state.dma) || !cdrom.restoreState(state.cdrom) ||
      state.spu == nullptr || !spu_.validateState(*state.spu) ||
      !xa_decoder_.validateState(state.xa_decoder)) {
    return false;
  }

  std::array<bool, DmaController::channel_count> found_tokens{};
  bool found_cdrom_command{};
  bool found_cdrom_sector{};
  for (std::size_t event_index = 0U; event_index < state.scheduler.event_count;
       ++event_index) {
    const auto &event = state.scheduler.events[event_index];
    const auto logical_now =
        state.pending_cpu_ticks >
                std::numeric_limits<std::uint64_t>::max() - state.scheduler.now
            ? std::numeric_limits<std::uint64_t>::max()
            : state.scheduler.now + state.pending_cpu_ticks;
    if (event.deadline <= logical_now) {
      return false;
    }
    if (event.type == MachineEventType::dma_complete) {
      if (event.index >= DmaController::channel_count || event.payload != 0U) {
        return false;
      }
      const auto channel = channelFromIndex(event.index);
      if (dma.scheduledToken(channel) != event.token ||
          found_tokens[event.index]) {
        return false;
      }
      found_tokens[event.index] = true;
      continue;
    }
    if (event.index != 0U) {
      return false;
    }
    if (event.type == MachineEventType::cdrom_command) {
      const auto request = cdrom.commandSchedule();
      const auto delay = scaleDeviceTicks(request.delay_ticks);
      const auto latest_deadline =
          delay > std::numeric_limits<std::uint64_t>::max() -
                      state.scheduler.now
              ? std::numeric_limits<std::uint64_t>::max()
              : state.scheduler.now + delay;
      if (found_cdrom_command || request.pending == 0U ||
          request.generation != event.payload ||
          event.deadline <= state.scheduler.now ||
          event.deadline > latest_deadline) {
        return false;
      }
      found_cdrom_command = true;
      continue;
    }
    if (event.type == MachineEventType::cdrom_sector) {
      const auto request = cdrom.sectorSchedule();
      const auto delay = scaleDeviceTicks(request.delay_ticks);
      const auto latest_deadline =
          delay > std::numeric_limits<std::uint64_t>::max() -
                      state.scheduler.now
              ? std::numeric_limits<std::uint64_t>::max()
              : state.scheduler.now + delay;
      if (found_cdrom_sector || request.pending == 0U ||
          request.generation != event.payload ||
          event.deadline <= state.scheduler.now ||
          event.deadline > latest_deadline) {
        return false;
      }
      found_cdrom_sector = true;
      continue;
    }
    return false;
  }
  if ((cdrom.commandSchedule().pending != 0U) != found_cdrom_command ||
      (cdrom.sectorSchedule().pending != 0U) != found_cdrom_sector) {
    return false;
  }
  for (std::size_t index = 0U; index < DmaController::channel_count; ++index) {
    const auto token = dma.scheduledToken(channelFromIndex(index));
    if ((token != 0U) != found_tokens[index]) {
      return false;
    }
  }
  if ((state.spu->transfer_busy != 0U) !=
      found_tokens[channelIndex(DmaChannel::spu)]) {
    return false;
  }
  if (state.spu->cd_input_matrix != state.cdrom.cd_volume_matrix) {
    return false;
  }

  const auto dma_bit = static_cast<std::uint16_t>(
      1U << static_cast<std::uint8_t>(InterruptSource::dma));
  if (((interrupts.inputLines() & dma_bit) != 0U) != dma.interruptLine()) {
    return false;
  }
  const auto cdrom_bit = static_cast<std::uint16_t>(
      1U << static_cast<std::uint8_t>(InterruptSource::cdrom));
  if (((interrupts.inputLines() & cdrom_bit) != 0U) != cdrom.interruptLine()) {
    return false;
  }
  const auto spu_bit = static_cast<std::uint16_t>(
      1U << static_cast<std::uint8_t>(InterruptSource::spu));
  if (((interrupts.inputLines() & spu_bit) != 0U) !=
      spuInterruptLine(*state.spu)) {
    return false;
  }

  return true;
}

bool PsxMachine::restoreState(const PsxMachineState &state) noexcept {
  if (!validateState(state)) {
    return false;
  }

  static_cast<void>(scheduler_.restoreState(state.scheduler));
  static_cast<void>(interrupts_.restoreState(state.interrupts));
  static_cast<void>(dma_.restoreState(state.dma));
  static_cast<void>(cdrom_.restoreState(state.cdrom));
  static_cast<void>(spu_.restoreState(*state.spu));
  static_cast<void>(xa_decoder_.restoreState(state.xa_decoder));
  timers_.restoreState(state.timers);
  pending_cpu_ticks_ = state.pending_cpu_ticks;
  device_tick_remainder_ = state.device_tick_remainder;
  syncCpuInterruptLine();
  return true;
}

void PsxMachine::consumeXaSector(
    std::span<const std::byte, CdRomMedia::raw_sector_size> sector,
    bool muted) noexcept {
  // DuckStation/hardware-compatible whole-sector admission. Decoding a sector
  // while the previous one is still buffered mutates XA predictor and
  // interpolation history even though its PCM cannot be consumed; that is a
  // direct source of recurring clicks and broken speech/music continuity.
  constexpr std::size_t xa_fifo_low_watermark = 10U;
  if (spu_.queuedCdFrames() > xa_fifo_low_watermark) {
    return;
  }
  std::array<SpuPcmFrame, XaAudioDecoder::maximum_output_frames> pcm{};
  const auto decoded = xa_decoder_.decodeSector(sector, pcm);
  if (!decoded.succeeded() || muted) {
    return;
  }
  static_cast<void>(
      spu_.pushCdAudio(std::span<const SpuPcmFrame>{pcm}.first(
          decoded.frames_written)));
}

void PsxMachine::resetXaStream() noexcept {
  xa_decoder_.reset();
  spu_.clearCdAudio();
}

void PsxMachine::setXaOutputMixer(
    std::array<std::uint8_t, 4U> matrix) noexcept {
  spu_.setCdInputMixer(matrix);
}

bool PsxMachine::readMmio(std::uint32_t physical_address,
                          R3000AccessWidth width,
                          std::uint32_t &value) noexcept {
  flushPendingCpuTicks();
  if (physical_address >= spu_base &&
      physical_address < spu_base + Spu::register_span) {
    const auto byte_count = accessBits(width) / 8U;
    const auto first_offset = physical_address - spu_base;
    if (first_offset + byte_count > Spu::register_span) {
      return false;
    }
    value = 0U;
    for (std::uint32_t index = 0U; index < byte_count; ++index) {
      const auto byte_offset = first_offset + index;
      std::uint16_t halfword{};
      if (!spu_.readRegister(byte_offset & ~1U, halfword)) {
        return false;
      }
      const auto byte = static_cast<std::uint8_t>(
          halfword >> ((byte_offset & 1U) * 8U));
      value |= static_cast<std::uint32_t>(byte) << (index * 8U);
    }
    syncSpuInterruptLine();
    return true;
  }

  if (physical_address >= cdrom_base &&
      physical_address < cdrom_base + CdRomController::register_span) {
    const auto byte_count = accessBits(width) / 8U;
    const auto first_offset = physical_address - cdrom_base;
    const auto fifo_read = first_offset == 1U || first_offset == 2U;
    if (!fifo_read &&
        first_offset + byte_count > CdRomController::register_span) {
      return false;
    }
    value = 0U;
    for (std::uint32_t index = 0U; index < byte_count; ++index) {
      const auto offset = fifo_read ? first_offset : first_offset + index;
      std::uint8_t byte{};
      if (offset >= CdRomController::register_span ||
          !cdrom_.readRegister(offset, byte)) {
        return false;
      }
      value |= static_cast<std::uint32_t>(byte) << (index * 8U);
    }
    syncCdRomSchedules();
    syncCdRomInterruptLine();
    kickDmaChannels();
    return true;
  }

  const auto aligned = physical_address & ~3U;
  std::uint32_t register_value{};

  if (aligned == interrupt_base) {
    register_value = interrupts_.status();
  } else if (aligned == interrupt_base + 4U) {
    register_value = interrupts_.mask();
  } else if (aligned >= dma_base &&
             aligned < dma_base + DmaController::register_span) {
    if (!dma_.readRegister(aligned - dma_base, register_value)) {
      return false;
    }
  } else if (aligned >= timer_base && aligned < timer_base + 0x30U) {
    const auto timer_offset = aligned - timer_base;
    const auto channel = static_cast<std::size_t>(timer_offset / 0x10U);
    switch (timer_offset & 0x0fU) {
    case 0x00U:
      register_value = timers_.readCounter(channel);
      break;
    case 0x04U:
      register_value = timers_.readMode(channel);
      break;
    case 0x08U:
      register_value = timers_.readTarget(channel);
      break;
    default:
      return false;
    }
  } else {
    return false;
  }

  value = extractLane(physical_address, width, register_value);
  return true;
}

bool PsxMachine::writeMmio(std::uint32_t physical_address,
                           R3000AccessWidth width,
                           std::uint32_t value) noexcept {
  flushPendingCpuTicks();
  if (physical_address >= spu_base &&
      physical_address < spu_base + Spu::register_span) {
    const auto byte_count = accessBits(width) / 8U;
    const auto first_offset = physical_address - spu_base;
    if (first_offset + byte_count > Spu::register_span) {
      return false;
    }

    if (width == R3000AccessWidth::byte) {
      const auto register_offset = first_offset & ~1U;
      std::uint16_t previous{};
      if (!spu_.readRegister(register_offset, previous)) {
        return false;
      }
      const auto shift = (first_offset & 1U) * 8U;
      const auto mask = static_cast<std::uint16_t>(0xffU << shift);
      const auto merged = static_cast<std::uint16_t>(
          (previous & ~mask) |
          ((static_cast<std::uint16_t>(value) << shift) & mask));
      if (!spu_.writeRegister(register_offset, merged)) {
        return false;
      }
    } else {
      if ((first_offset & 1U) != 0U ||
          !spu_.writeRegister(first_offset,
                              static_cast<std::uint16_t>(value))) {
        return false;
      }
      if (width == R3000AccessWidth::word &&
          !spu_.writeRegister(
              first_offset + 2U,
              static_cast<std::uint16_t>(value >> 16U))) {
        return false;
      }
    }

    syncSpuInterruptLine();
    kickDmaChannels();
    syncCpuInterruptLine();
    return true;
  }

  if (physical_address >= cdrom_base &&
      physical_address < cdrom_base + CdRomController::register_span) {
    const auto byte_count = accessBits(width) / 8U;
    const auto first_offset = physical_address - cdrom_base;
    const auto parameter_write = first_offset == 2U;
    if (!parameter_write &&
        first_offset + byte_count > CdRomController::register_span) {
      return false;
    }
    for (std::uint32_t index = 0U; index < byte_count; ++index) {
      const auto offset = parameter_write ? first_offset : first_offset + index;
      if (offset >= CdRomController::register_span ||
          !cdrom_.writeRegister(
              offset, static_cast<std::uint8_t>(value >> (index * 8U)))) {
        return false;
      }
    }
    syncCdRomSchedules();
    syncCdRomInterruptLine();
    kickDmaChannels();
    syncCpuInterruptLine();
    return true;
  }

  const auto aligned = physical_address & ~3U;
  const auto write_mask = laneMask(physical_address, width);
  const auto placed_value = placeLane(physical_address, width, value);

  if (aligned == interrupt_base) {
    interrupts_.acknowledge(static_cast<std::uint16_t>(placed_value),
                            static_cast<std::uint16_t>(write_mask));
  } else if (aligned == interrupt_base + 4U) {
    interrupts_.writeMask(static_cast<std::uint16_t>(placed_value),
                          static_cast<std::uint16_t>(write_mask));
  } else if (aligned >= dma_base &&
             aligned < dma_base + DmaController::register_span) {
    std::array<std::uint64_t, DmaController::channel_count> previous_tokens{};
    for (std::size_t index = 0U; index < previous_tokens.size(); ++index) {
      previous_tokens[index] = dma_.scheduledToken(channelFromIndex(index));
    }
    if (!dma_.writeRegister(aligned - dma_base, placed_value, write_mask)) {
      return false;
    }
    for (std::size_t index = 0U; index < previous_tokens.size(); ++index) {
      const auto token = previous_tokens[index];
      if (token != 0U &&
          dma_.scheduledToken(channelFromIndex(index)) != token) {
        static_cast<void>(scheduler_.cancel(token));
        if (channelFromIndex(index) == DmaChannel::spu) {
          spu_.setDmaTransferBusy(false);
        }
      }
    }
    syncDmaInterruptLine();
    kickDmaChannels();
  } else if (aligned >= timer_base && aligned < timer_base + 0x30U) {
    const auto timer_offset = aligned - timer_base;
    const auto channel = static_cast<std::size_t>(timer_offset / 0x10U);
    std::uint16_t previous{};
    switch (timer_offset & 0x0fU) {
    case 0x00U:
      previous = timers_.readCounter(channel);
      break;
    case 0x04U:
      previous = timers_.peekMode(channel);
      break;
    case 0x08U:
      previous = timers_.readTarget(channel);
      break;
    default:
      return false;
    }
    const auto merged = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(previous) & ~write_mask) |
        (placed_value & write_mask));
    switch (timer_offset & 0x0fU) {
    case 0x00U:
      timers_.writeCounter(channel, merged);
      break;
    case 0x04U:
      timers_.writeMode(channel, merged);
      break;
    case 0x08U:
      timers_.writeTarget(channel, merged);
      break;
    default:
      return false;
    }
  } else {
    return false;
  }

  syncCpuInterruptLine();
  return true;
}

void PsxMachine::advanceDevicesTo(std::uint64_t tick) noexcept {
  const auto elapsed = unscaleCpuTicks(tick - scheduler_.now());
  routeTimerInterrupts(timers_.advanceCpuCycles(elapsed));
  const auto mixed_frames = spu_.state().mixed_frames;
  spu_.advanceCpuTicks(elapsed);
  if (spu_.state().mixed_frames != mixed_frames) {
    interrupts_.setLine(InterruptSource::spu, spu_.interruptLine());
  }
  scheduler_.advanceTo(tick);
}

void PsxMachine::flushPendingCpuTicks() noexcept {
  if (pending_cpu_ticks_ == 0U) {
    return;
  }
  const auto ticks = pending_cpu_ticks_;
  pending_cpu_ticks_ = 0U;
  advanceTicks(ticks);
}

void PsxMachine::queueCpuTicks(std::uint64_t ticks) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  pending_cpu_ticks_ =
      ticks > maximum - pending_cpu_ticks_ ? maximum
                                           : pending_cpu_ticks_ + ticks;
  const auto target = currentTick();
  if (pending_cpu_ticks_ >= maximum_cpu_slice_ticks ||
      scheduler_.hasDue(target)) {
    flushPendingCpuTicks();
  }
}

std::uint64_t PsxMachine::scaleDeviceTicks(std::uint64_t ticks) const noexcept {
  if (cpu_clock_scale_.numerator == cpu_clock_scale_.denominator) {
    return ticks;
  }
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (ticks > maximum / cpu_clock_scale_.numerator) {
    return maximum;
  }
  const auto scaled = ticks * cpu_clock_scale_.numerator;
  const auto rounding = cpu_clock_scale_.denominator - 1U;
  return scaled > maximum - rounding
             ? maximum
             : (scaled + rounding) / cpu_clock_scale_.denominator;
}

std::uint64_t PsxMachine::unscaleCpuTicks(std::uint64_t ticks) noexcept {
  if (cpu_clock_scale_.numerator == cpu_clock_scale_.denominator) {
    return ticks;
  }
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (ticks > (maximum - device_tick_remainder_) /
                  cpu_clock_scale_.denominator) {
    device_tick_remainder_ = 0U;
    return maximum;
  }
  const auto scaled = ticks * cpu_clock_scale_.denominator +
                      device_tick_remainder_;
  const auto result = scaled / cpu_clock_scale_.numerator;
  device_tick_remainder_ = static_cast<std::uint32_t>(
      scaled % cpu_clock_scale_.numerator);
  return result;
}

void PsxMachine::dispatchEvent(const MachineEvent &event) noexcept {
  if (event.type == MachineEventType::cdrom_command) {
    if (event.index == 0U) {
      cdrom_.eventCommand(event.payload);
      syncCdRomSchedules();
      syncCdRomInterruptLine();
      kickDmaChannels();
    }
    return;
  }
  if (event.type == MachineEventType::cdrom_sector) {
    if (event.index == 0U) {
      cdrom_.eventSector(event.payload);
      syncCdRomSchedules();
      syncCdRomInterruptLine();
      kickDmaChannels();
    }
    return;
  }
  if (event.type != MachineEventType::dma_complete ||
      event.index >= DmaController::channel_count) {
    return;
  }
  const auto channel = channelFromIndex(event.index);
  if (dma_.scheduledToken(channel) != event.token) {
    return;
  }
  if (!executeDmaTransfer(channel)) {
    static_cast<void>(dma_.cancelScheduled(channel, event.token));
    if (channel == DmaChannel::spu) {
      spu_.setDmaTransferBusy(false);
    }
    syncSpuInterruptLine();
    const auto index = channelIndex(channel);
    if (channel == DmaChannel::cdrom && index < dma_ports_.size() &&
        dma_ports_[index] == nullptr) {
      static_cast<void>(dma_.complete(channel));
      syncDmaInterruptLine();
      kickDmaChannels();
    }
    return;
  }
  static_cast<void>(dma_.complete(channel));
  if (channel == DmaChannel::spu) {
    spu_.setDmaTransferBusy(false);
  }
  syncSpuInterruptLine();
  syncDmaInterruptLine();
  kickDmaChannels();
}

void PsxMachine::syncCdRomSchedules() noexcept {
  auto command_found = false;
  auto sector_found = false;
  const auto command = cdrom_.commandSchedule();
  const auto sector = cdrom_.sectorSchedule();
  const auto scheduler_state = scheduler_.captureState();
  for (std::size_t index = 0U; index < scheduler_state.event_count; ++index) {
    const auto &event = scheduler_state.events[index];
    if (event.type == MachineEventType::cdrom_command) {
      const auto current = command.pending != 0U && event.index == 0U &&
                           event.payload == command.generation &&
                           !command_found;
      if (current) {
        command_found = true;
      } else {
        static_cast<void>(scheduler_.cancel(event.token));
      }
    } else if (event.type == MachineEventType::cdrom_sector) {
      const auto current = sector.pending != 0U && event.index == 0U &&
                           event.payload == sector.generation && !sector_found;
      if (current) {
        sector_found = true;
      } else {
        static_cast<void>(scheduler_.cancel(event.token));
      }
    }
  }
  if (command.pending != 0U && !command_found) {
    static_cast<void>(scheduler_.scheduleAfter(
        scaleDeviceTicks(command.delay_ticks),
                                               MachineEventType::cdrom_command,
                                               0U, command.generation));
  }
  if (sector.pending != 0U && !sector_found) {
    static_cast<void>(scheduler_.scheduleAfter(
        scaleDeviceTicks(sector.delay_ticks),
                                               MachineEventType::cdrom_sector,
                                               0U, sector.generation));
  }
}

void PsxMachine::syncCdRomInterruptLine() noexcept {
  interrupts_.setLine(InterruptSource::cdrom, cdrom_.interruptLine());
  syncCpuInterruptLine();
}

void PsxMachine::routeTimerInterrupts(RootTimers::IrqMask mask) noexcept {
  constexpr std::array sources{
      InterruptSource::timer0,
      InterruptSource::timer1,
      InterruptSource::timer2,
  };
  for (std::size_t index = 0U; index < sources.size(); ++index) {
    if ((mask & (1U << index)) != 0U) {
      interrupts_.pulse(sources[index]);
    }
  }
}

void PsxMachine::syncDmaInterruptLine() noexcept {
  interrupts_.setLine(InterruptSource::dma, dma_.interruptLine());
  syncCpuInterruptLine();
}

void PsxMachine::syncSpuInterruptLine() noexcept {
  interrupts_.setLine(InterruptSource::spu, spu_.interruptLine());
  syncCpuInterruptLine();
}

void PsxMachine::syncCpuInterruptLine() noexcept {
  cpu_.setExternalInterrupt(interrupts_.cpuLine());
}

void PsxMachine::kickDmaChannels() noexcept {
  for (std::size_t index = 0U; index < DmaController::channel_count; ++index) {
    const auto channel = channelFromIndex(index);
    if (!dma_.channelStartable(channel)) {
      continue;
    }

    const auto is_otc = channel == DmaChannel::otc;
    auto *port = dma_ports_[index];
    const auto internal_cdrom = channel == DmaChannel::cdrom && port == nullptr;
    if (internal_cdrom && (dma_.chcr(channel) & 1U) != 0U) {
      static_cast<void>(dma_.complete(channel));
      syncDmaInterruptLine();
      continue;
    }
    if (!is_otc) {
      const auto request =
          port != nullptr ? port->dmaRequest()
                          : channel == DmaChannel::cdrom && cdrom_.dmaRequest();
      if (!request) {
        continue;
      }
    }

    std::uint64_t delay{};
    if (dma_.syncMode(channel) == DmaSyncMode::linked_list) {
      if (channel != DmaChannel::gpu || !linkedListTicks(delay)) {
        if (internal_cdrom) {
          static_cast<void>(dma_.complete(channel));
          syncDmaInterruptLine();
        }
        continue;
      }
    } else {
      const auto words = dma_.estimatedWordCount(channel);
      if (!words.has_value() || *words > maximum_dma_words) {
        if (internal_cdrom) {
          static_cast<void>(dma_.complete(channel));
          syncDmaInterruptLine();
        }
        continue;
      }
      if (internal_cdrom && !cdrom_.canReadDmaWords(*words)) {
        static_cast<void>(dma_.complete(channel));
        syncDmaInterruptLine();
        continue;
      }
      delay = channel == DmaChannel::spu ? *words * 4U : *words;
    }
    delay = scaleDeviceTicks(std::max<std::uint64_t>(1U, delay));
    const auto token =
        scheduler_.scheduleAfter(delay, MachineEventType::dma_complete,
                                 static_cast<std::uint8_t>(index));
    if (token != 0U) {
      if (!dma_.markScheduled(channel, token)) {
        static_cast<void>(scheduler_.cancel(token));
        if (internal_cdrom) {
          static_cast<void>(dma_.complete(channel));
          syncDmaInterruptLine();
        }
      } else if (channel == DmaChannel::spu) {
        spu_.setDmaTransferBusy(true);
      }
    } else if (internal_cdrom) {
      static_cast<void>(dma_.complete(channel));
      syncDmaInterruptLine();
    }
  }
}

bool PsxMachine::executeDmaTransfer(DmaChannel channel) noexcept {
  if (channel == DmaChannel::otc) {
    return executeOtcDma();
  }
  if (dma_.syncMode(channel) == DmaSyncMode::linked_list) {
    return executeLinkedListDma(channel);
  }
  return executeLinearDma(channel);
}

bool PsxMachine::executeLinearDma(DmaChannel channel) noexcept {
  const auto index = channelIndex(channel);
  if (index >= dma_ports_.size() ||
      (dma_ports_[index] == nullptr && channel != DmaChannel::cdrom)) {
    return false;
  }
  const auto words = dma_.estimatedWordCount(channel);
  if (!words.has_value() || *words > maximum_dma_words) {
    return false;
  }

  auto *port = dma_ports_[index];
  auto address = dma_.madr(channel) & 0x00fffffcU;
  const auto control = dma_.chcr(channel);
  const auto from_ram = (control & 1U) != 0U;
  const auto reverse = (control & 2U) != 0U;
  if (from_ram && port == nullptr) {
    return false;
  }
  if (!from_ram && channel == DmaChannel::cdrom && port == nullptr &&
      !cdrom_.canReadDmaWords(*words)) {
    return false;
  }
  for (std::uint64_t word_index = 0U; word_index < *words; ++word_index) {
    const auto ram_address = address & ram_address_mask & ~3U;
    std::uint32_t value{};
    if (from_ram) {
      if (!cpu_.read32(ram_address, value) || !port->writeDmaWord(value)) {
        return false;
      }
    } else {
      const auto read = port != nullptr ? port->readDmaWord(value)
                                        : cdrom_.readDmaWord(value);
      if (!read || !cpu_.write32(ram_address, value)) {
        return false;
      }
    }
    address =
        reverse ? (address - 4U) & 0x00ffffffU : (address + 4U) & 0x00ffffffU;
  }
  return dma_.setMadr(channel, address);
}

bool PsxMachine::executeLinkedListDma(DmaChannel channel) noexcept {
  if (channel != DmaChannel::gpu || (dma_.chcr(channel) & 1U) == 0U) {
    return false;
  }
  auto *port = dma_ports_[channelIndex(channel)];
  if (port == nullptr) {
    return false;
  }

  auto address = dma_.madr(channel) & ram_address_mask & ~3U;
  std::uint64_t transferred_words{};
  for (std::uint64_t node = 0U; node < maximum_linked_list_nodes; ++node) {
    std::uint32_t header{};
    if (!cpu_.read32(address, header)) {
      return false;
    }
    const auto word_count = static_cast<std::uint32_t>(header >> 24U);
    if (transferred_words + word_count > maximum_dma_words) {
      return false;
    }
    for (std::uint32_t index = 0U; index < word_count; ++index) {
      const auto word_address =
          (address + (index + 1U) * 4U) & ram_address_mask & ~3U;
      std::uint32_t value{};
      if (!cpu_.read32(word_address, value) || !port->writeDmaWord(value)) {
        return false;
      }
    }
    transferred_words += word_count;
    const auto next = header & 0x00ffffffU;
    if ((next & 0x00800000U) != 0U) {
      return dma_.setMadr(channel, next);
    }
    address = next & ram_address_mask & ~3U;
  }
  return false;
}

bool PsxMachine::executeOtcDma() noexcept {
  const auto words = dma_.estimatedWordCount(DmaChannel::otc);
  if (!words.has_value() || *words > maximum_dma_words) {
    return false;
  }
  auto address = dma_.madr(DmaChannel::otc) & ram_address_mask & ~3U;
  for (std::uint64_t index = 0U; index < *words; ++index) {
    const auto link = index + 1U == *words
                          ? 0x00ffffffU
                          : (address - 4U) & ram_address_mask & ~3U;
    if (!cpu_.write32(address, link)) {
      return false;
    }
    address = (address - 4U) & ram_address_mask & ~3U;
  }
  return true;
}

bool PsxMachine::linkedListTicks(std::uint64_t &ticks) const noexcept {
  auto address = dma_.madr(DmaChannel::gpu) & ram_address_mask & ~3U;
  ticks = 0U;
  std::uint64_t transferred_words{};
  for (std::uint64_t node = 0U; node < maximum_linked_list_nodes; ++node) {
    std::uint32_t header{};
    if (!cpu_.read32(address, header)) {
      return false;
    }
    const auto word_count = static_cast<std::uint64_t>(header >> 24U);
    if (transferred_words + word_count > maximum_dma_words ||
        ticks > std::numeric_limits<std::uint64_t>::max() - 14U - word_count) {
      return false;
    }
    transferred_words += word_count;
    ticks += 14U + word_count;
    const auto next = header & 0x00ffffffU;
    if ((next & 0x00800000U) != 0U) {
      return true;
    }
    address = next & ram_address_mask & ~3U;
  }
  return false;
}

} // namespace sf::psx
