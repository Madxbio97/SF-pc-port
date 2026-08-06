#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/agent_late_mission_rules.hpp"
#include "sf/game/agent_mission_rules.hpp"
#include "sf/game/agent_park_timer.hpp"
#include "sf/game/legacy_virtual_cd.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace sf::game {
namespace {

constexpr std::uint32_t cd_lead_in_sectors = 150U;
constexpr std::uint32_t agent_guest_ram_begin = 0x80000000U;
constexpr std::uint32_t agent_guest_ram_end = 0x80200000U;

bool validGuestRamRange(std::uint32_t address, std::uint32_t size) noexcept {
  return size != 0U && size <= agent_guest_ram_end - agent_guest_ram_begin &&
         address >= agent_guest_ram_begin &&
         address <= agent_guest_ram_end - size;
}

bool guestFrameReached(std::uint32_t frame, std::uint32_t target) noexcept {
  return std::bit_cast<std::int32_t>(frame - target) >= 0;
}

std::uint8_t legacyTextChecksum(std::string_view text) noexcept {
  auto checksum = std::uint8_t{};
  for (const auto character : text) {
    checksum = static_cast<std::uint8_t>(checksum +
                                         static_cast<unsigned char>(character));
  }
  return checksum;
}

bool advanceRetailRendererVblank(
    psx::R3000Runtime &runtime,
    const LegacyRetailOuterFrameProfile &profile) noexcept {
  std::uint16_t interval_bits{};
  std::uint32_t counter{};
  if (!runtime.read16(profile.renderer_vblank_interval, interval_bits) ||
      !runtime.read32(profile.vblank_counter, counter)) {
    return false;
  }
  const auto interval = std::bit_cast<std::int16_t>(interval_bits);
  if (interval <= 0) {
    return false;
  }
  return runtime.write32(profile.vblank_counter,
                         counter + static_cast<std::uint32_t>(interval));
}

std::uint8_t encodeBcd(std::uint32_t value) noexcept {
  return static_cast<std::uint8_t>(((value / 10U) << 4U) | (value % 10U));
}

std::optional<std::uint32_t> decodeBcd(std::uint8_t value) noexcept {
  const auto high = static_cast<std::uint32_t>(value >> 4U);
  const auto low = static_cast<std::uint32_t>(value & 0x0fU);
  if (high > 9U || low > 9U) {
    return std::nullopt;
  }
  return high * 10U + low;
}

bool encodeCdPosition(std::uint32_t sector,
                      std::array<std::byte, 4U> &position) noexcept {
  const auto absolute = static_cast<std::uint64_t>(sector) + cd_lead_in_sectors;
  const auto minute = absolute / (60U * 75U);
  if (minute > 99U) {
    return false;
  }
  const auto remainder = absolute % (60U * 75U);
  position = {
      static_cast<std::byte>(encodeBcd(static_cast<std::uint32_t>(minute))),
      static_cast<std::byte>(
          encodeBcd(static_cast<std::uint32_t>(remainder / 75U))),
      static_cast<std::byte>(
          encodeBcd(static_cast<std::uint32_t>(remainder % 75U))),
      std::byte{0},
  };
  return true;
}

std::optional<std::uint32_t>
decodeCdPosition(std::span<const std::byte, 4U> position) noexcept {
  const auto minute = decodeBcd(std::to_integer<std::uint8_t>(position[0]));
  const auto second = decodeBcd(std::to_integer<std::uint8_t>(position[1]));
  const auto sector = decodeBcd(std::to_integer<std::uint8_t>(position[2]));
  if (!minute || !second || !sector || *second >= 60U || *sector >= 75U) {
    return std::nullopt;
  }
  const auto absolute = (*minute * 60U + *second) * 75U + *sector;
  if (absolute < cd_lead_in_sectors) {
    return std::nullopt;
  }
  return absolute - cd_lead_in_sectors;
}

struct LegacyPlayerGuestAddresses {
  std::uint32_t instance{};
  std::uint32_t slot{};
  std::uint32_t record{};
  std::uint32_t motion{};
  std::uint32_t matrix{};
  std::uint32_t health{};
};

std::optional<LegacyPlayerGuestAddresses>
resolveLegacyPlayer(const psx::R3000Runtime &runtime,
                    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  constexpr std::uint32_t object_record_stride = 0x4cU;
  constexpr std::uint32_t object_instance_offset = 0x34U;
  constexpr std::uint32_t instance_node_offset = 8U;
  constexpr std::uint32_t instance_motion_offset = 0x0cU;
  constexpr std::uint32_t instance_health_offset = 0x18U;
  constexpr std::uint32_t node_matrix_offset = 0x0cU;

  std::uint32_t player{};
  std::uint32_t records{};
  std::uint32_t count{};
  if (!runtime.read32(profile.player_pointer, player) || player == 0U ||
      !runtime.read32(profile.object_records_pointer, records) ||
      records == 0U || !runtime.read32(profile.object_count, count) ||
      count > profile.maximum_objects) {
    return std::nullopt;
  }

  LegacyPlayerGuestAddresses result;
  result.instance = player;
  auto found = false;
  for (std::uint32_t slot = 0U; slot < count; ++slot) {
    const auto offset = static_cast<std::uint64_t>(slot) * object_record_stride;
    const auto record64 = static_cast<std::uint64_t>(records) + offset;
    if (record64 > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    const auto record = static_cast<std::uint32_t>(record64);
    std::uint32_t instance{};
    if (!runtime.read32(record + object_instance_offset, instance)) {
      return std::nullopt;
    }
    if (instance == player) {
      result.slot = slot;
      result.record = record;
      found = true;
      break;
    }
  }
  if (!found ||
      !runtime.read32(player + instance_motion_offset, result.motion) ||
      result.motion == 0U ||
      !runtime.read32(player + instance_health_offset, result.health) ||
      result.health == 0U) {
    return std::nullopt;
  }

  std::uint32_t root_node{};
  if (!runtime.read32(player + instance_node_offset, root_node) ||
      root_node == 0U ||
      !runtime.read32(root_node + node_matrix_offset, result.matrix) ||
      result.matrix == 0U) {
    return std::nullopt;
  }
  return result;
}

std::uint32_t guestWord(std::int32_t value) noexcept {
  return std::bit_cast<std::uint32_t>(value);
}

std::uint16_t guestHalf(std::int16_t value) noexcept {
  return std::bit_cast<std::uint16_t>(value);
}

bool writeLegacyPlayerHeading(psx::R3000Runtime &runtime, std::uint32_t matrix,
                              std::int32_t heading) noexcept {
  constexpr std::int32_t fixed_one = 4096;
  constexpr std::int32_t angle_units = 4096;
  auto yaw = heading % angle_units;
  if (yaw < 0) {
    yaw += angle_units;
  }
  const auto radians =
      static_cast<double>(yaw) *
      (2.0 * std::numbers::pi / static_cast<double>(angle_units));
  const auto fixed = [](double value) {
    return static_cast<std::int16_t>(
        std::clamp<long>(std::lround(value * fixed_one),
                         std::numeric_limits<std::int16_t>::min(),
                         std::numeric_limits<std::int16_t>::max()));
  };
  const auto cosine = fixed(std::cos(radians));
  const auto sine = fixed(std::sin(radians));
  const std::array<std::int16_t, 9U> rotation{
      cosine,
      0,
      sine,
      0,
      static_cast<std::int16_t>(fixed_one),
      0,
      static_cast<std::int16_t>(-sine),
      0,
      cosine,
  };
  for (std::uint32_t component = 0U; component < rotation.size(); ++component) {
    if (!runtime.write16(matrix + component * 2U,
                         guestHalf(rotation[component]))) {
      return false;
    }
  }
  return true;
}

bool rotateLegacyPlayerHeading(psx::R3000Runtime &runtime, std::uint32_t matrix,
                               std::int32_t heading) noexcept {
  constexpr std::int32_t angle_units = 4096;
  std::array<std::int16_t, 9U> current{};
  for (std::uint32_t component = 0U; component < current.size(); ++component) {
    std::uint16_t bits{};
    if (!runtime.read16(matrix + component * 2U, bits)) {
      return false;
    }
    current[component] = std::bit_cast<std::int16_t>(bits);
  }
  if (std::hypot(static_cast<double>(current[2]),
                 static_cast<double>(current[8])) <= 0.5) {
    return writeLegacyPlayerHeading(runtime, matrix, heading);
  }

  auto yaw = heading % angle_units;
  if (yaw < 0) {
    yaw += angle_units;
  }
  const auto target =
      static_cast<double>(yaw) *
      (2.0 * std::numbers::pi / static_cast<double>(angle_units));
  const auto existing = std::atan2(static_cast<double>(current[2]),
                                   static_cast<double>(current[8]));
  const auto cosine = std::cos(target - existing);
  const auto sine = std::sin(target - existing);
  const auto fixed = [](double value) {
    return static_cast<std::int16_t>(std::clamp<long>(
        std::lround(value), std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
  };
  auto rotated = current;
  for (std::size_t column = 0U; column < 3U; ++column) {
    const auto first = static_cast<double>(current[column]);
    const auto third = static_cast<double>(current[6U + column]);
    rotated[column] = fixed(cosine * first + sine * third);
    rotated[6U + column] = fixed(-sine * first + cosine * third);
  }
  for (std::uint32_t component = 0U; component < rotated.size(); ++component) {
    if (!runtime.write16(matrix + component * 2U,
                         guestHalf(rotated[component]))) {
      return false;
    }
  }
  return true;
}

std::uint32_t guestArgument(std::int16_t value) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

bool validInterruptCallback(std::uint32_t address) noexcept {
  if (address == 0U) {
    return true;
  }
  constexpr std::uint32_t physical_address_mask = 0x1fffffffU;
  const auto physical = address & physical_address_mask;
  return (address & 3U) == 0U && physical < psx::R3000Runtime::ram_size;
}

bool validDelayedLoad(const psx::R3000DelayedLoadState &load) noexcept {
  return load.reg < 32U && (!load.valid || load.reg != 0U);
}

bool validCpuSnapshot(const psx::R3000State &cpu) noexcept {
  constexpr std::uint32_t instruction_alignment_mask = 3U;
  return cpu.gpr[0] == 0U && (cpu.pc & instruction_alignment_mask) == 0U &&
         (cpu.next_pc & instruction_alignment_mask) == 0U &&
         (cpu.branch_pc & instruction_alignment_mask) == 0U &&
         validDelayedLoad(cpu.load_delay) &&
         validDelayedLoad(cpu.next_load_delay);
}

bool sameSchedulerState(const psx::EventSchedulerState &left,
                        const psx::EventSchedulerState &right) noexcept {
  if (left.now != right.now || left.next_token != right.next_token ||
      left.event_count != right.event_count) {
    return false;
  }
  for (std::size_t index = 0U; index < left.events.size(); ++index) {
    const auto &a = left.events[index];
    const auto &b = right.events[index];
    if (a.deadline != b.deadline || a.token != b.token ||
        a.payload != b.payload || a.type != b.type || a.index != b.index) {
      return false;
    }
  }
  return true;
}

// Host-owned settings may change retail RAM and SPU volume state, but applying
// them while paused/restoring must never advance or otherwise mutate the PSX
// machine timeline.
bool sameMachineTimeline(const psx::PsxMachineState &left,
                         const psx::PsxMachineState &right) noexcept {
  return sameSchedulerState(left.scheduler, right.scheduler) &&
         left.cpu_clock_scale == right.cpu_clock_scale &&
         left.pending_cpu_ticks == right.pending_cpu_ticks &&
         left.device_tick_remainder == right.device_tick_remainder &&
         left.interrupts.status == right.interrupts.status &&
         left.interrupts.mask == right.interrupts.mask &&
         left.interrupts.input_lines == right.interrupts.input_lines &&
         left.dma == right.dma && left.cdrom == right.cdrom &&
         left.xa_decoder == right.xa_decoder && left.timers == right.timers;
}

std::uint64_t logicalMachineTick(const psx::PsxMachineState &state) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return state.pending_cpu_ticks > maximum - state.scheduler.now
             ? maximum
             : state.scheduler.now + state.pending_cpu_ticks;
}

} // namespace

std::uint32_t LegacyHostCallContext::pc() const noexcept {
  return runtime_.state().pc;
}

std::uint32_t LegacyHostCallContext::returnAddress() const noexcept {
  return runtime_.state().gpr[31];
}

std::uint32_t
LegacyHostCallContext::registerValue(std::size_t index) const noexcept {
  return index < runtime_.state().gpr.size() ? runtime_.state().gpr[index] : 0U;
}

std::uint32_t
LegacyHostCallContext::argument(std::size_t index) const noexcept {
  if (index < 4U) {
    return registerValue(4U + index);
  }
  constexpr auto maximum_stack_index =
      (std::numeric_limits<std::uint32_t>::max() - 16U) / 4U + 4U;
  if (index > maximum_stack_index) {
    return 0U;
  }
  const auto offset = 16U + static_cast<std::uint32_t>((index - 4U) * 4U);
  std::uint32_t value{};
  return runtime_.read32(registerValue(29U) + offset, value) ? value : 0U;
}

void LegacyHostCallContext::setRegister(std::size_t index,
                                        std::uint32_t value) noexcept {
  if (index < runtime_.state().gpr.size()) {
    runtime_.setRegister(static_cast<std::uint8_t>(index), value);
  }
}

void LegacyHostCallContext::setReturnValue(std::uint32_t value) noexcept {
  runtime_.setRegister(2U, value);
}

bool LegacyHostCallContext::read8(std::uint32_t address,
                                  std::uint8_t &value) const noexcept {
  return runtime_.read8(address, value);
}

bool LegacyHostCallContext::read16(std::uint32_t address,
                                   std::uint16_t &value) const noexcept {
  return runtime_.read16(address, value);
}

bool LegacyHostCallContext::read32(std::uint32_t address,
                                   std::uint32_t &value) const noexcept {
  return runtime_.read32(address, value);
}

bool LegacyHostCallContext::readBytes(
    std::uint32_t address, std::span<std::byte> destination) const noexcept {
  return runtime_.copyBytes(address, destination);
}

bool LegacyHostCallContext::readCString(std::uint32_t address,
                                        std::string &value,
                                        std::size_t maximum_size) const {
  value.clear();
  value.reserve(maximum_size);
  for (std::size_t index = 0U; index < maximum_size; ++index) {
    std::uint8_t character{};
    if (!read8(address + static_cast<std::uint32_t>(index), character)) {
      value.clear();
      return false;
    }
    if (character == 0U) {
      return true;
    }
    value.push_back(static_cast<char>(character));
  }
  value.clear();
  return false;
}

bool LegacyHostCallContext::write8(std::uint32_t address,
                                   std::uint8_t value) noexcept {
  return runtime_.write8(address, value);
}

bool LegacyHostCallContext::write16(std::uint32_t address,
                                    std::uint16_t value) noexcept {
  return runtime_.write16(address, value);
}

bool LegacyHostCallContext::write32(std::uint32_t address,
                                    std::uint32_t value) noexcept {
  return runtime_.write32(address, value);
}

bool LegacyHostCallContext::writeBytes(
    std::uint32_t address, std::span<const std::byte> bytes) noexcept {
  return runtime_.loadBytes(address, bytes);
}

LegacyGameplayVm::LegacyGameplayVm(const psx::Executable &executable,
                                   psx::CpuClockScale cpu_clock_scale)
    : machine_(runtime_, cpu_clock_scale),
      ram_host_calls_(psx::R3000Runtime::ram_size / sizeof(std::uint32_t)),
      executable_initial_pc_(executable.header().initial_pc) {
  host_calls_.reserve(128U);
  runtime_.loadExecutable(executable);
  bindAgentDifficultyDamageHook();
  bindSyphonFilterUsaV11AgentMissionNpcSpawnHook();
  bindSyphonFilterUsaV11AgentAramovSpeedHook();
}

bool LegacyGameplayVm::loadOverlay(std::uint32_t address,
                                   std::span<const std::byte> bytes) noexcept {
  return runtime_.loadBytes(address, bytes);
}

bool LegacyGameplayVm::waitForCdRomInterrupt(
    std::uint8_t expected_interrupt) noexcept {
  constexpr std::size_t maximum_device_events = 16U;
  for (std::size_t attempt = 0U; attempt < maximum_device_events; ++attempt) {
    const auto state = machine_.cdrom().captureState();
    const auto interrupt =
        static_cast<std::uint8_t>(state.interrupt_flags & 0x07U);
    if (interrupt != 0U) {
      return interrupt == expected_interrupt;
    }

    const auto command = machine_.cdrom().commandSchedule();
    const auto sector = machine_.cdrom().sectorSchedule();
    if (command.pending == 0U && sector.pending == 0U) {
      return false;
    }
    // This is a synchronous HLE boundary: the guest cannot yield while it
    // waits, whereas realtime SPU/CD time is owned by the independent 120 Hz
    // scheduler. Advancing all hardware by the emulated seek/sector delay here
    // rendered up to hundreds of milliseconds of future PCM during a room
    // transition. The output then played that stale block before newly fired
    // voices, which sounded like lag and missing samples. Complete only the
    // next CD event; do not move the global hardware/audio clock.
    if (!machine_.completeNextPendingCdRomEvent()) {
      return false;
    }
  }
  return false;
}

bool LegacyGameplayVm::acknowledgeCdRomInterrupt(
    std::uint8_t expected_interrupt) noexcept {
  constexpr std::uint32_t cdrom_base = 0x1f801800U;
  const auto state = machine_.cdrom().captureState();
  if ((state.interrupt_flags & 0x07U) != expected_interrupt ||
      !runtime_.write8(cdrom_base, 0U)) {
    return false;
  }

  for (std::size_t response = 0U; response < psx::CdRomState::fifo_capacity;
       ++response) {
    std::uint8_t status{};
    if (!runtime_.read8(cdrom_base, status)) {
      return false;
    }
    if ((status & (1U << 5U)) == 0U) {
      break;
    }
    std::uint8_t ignored{};
    if (!runtime_.read8(cdrom_base + 1U, ignored)) {
      return false;
    }
  }

  if (!runtime_.write8(cdrom_base, 1U) ||
      !runtime_.write8(cdrom_base + 3U, 0x1fU) ||
      !runtime_.write8(cdrom_base, 0U) ||
      !runtime_.write16(0x1f801070U, 0xfffbU)) {
    return false;
  }
  return (machine_.cdrom().captureState().interrupt_flags & 0x07U) == 0U;
}

bool LegacyGameplayVm::dispatchCdRomReadyCallback() {
  constexpr std::uint8_t data_ready_interrupt = 1U;
  constexpr std::uint32_t ready_callback_address = 0x80114cc4U;
  constexpr std::uint32_t ready_result_address = 0x80125450U;
  constexpr std::uint32_t ready_state_address = 0x80114f9dU;

  const auto cdrom = machine_.cdrom().captureState();
  const auto interrupt =
      static_cast<std::uint8_t>(cdrom.interrupt_flags & 0x07U);
  if (interrupt == 0U) {
    return true;
  }
  if (interrupt != data_ready_interrupt) {
    // This helper owns only the retail data-ready (INT1) fast path. Command
    // acknowledgement/completion/error IRQs are ordinary CD-ROM state and are
    // consumed by the guest's CD interrupt handler. Treating one of those
    // latched IRQs as an audio-clock failure made the independent 120 Hz SPU
    // scheduler abort during normal room streaming, even though neither the
    // timer callback nor the audio timeline was invalid. Leave unrelated IRQs
    // untouched and continue clocking the SPU.
    return true;
  }

  std::array<std::byte, 8U> result{};
  const auto response_count = std::min<std::size_t>(
      result.size(), cdrom.response_count - cdrom.response_position);
  for (std::size_t index = 0U; index < response_count; ++index) {
    result[index] =
        static_cast<std::byte>(cdrom.response[cdrom.response_position + index]);
  }

  std::uint32_t callback{};
  if (!runtime_.read32(ready_callback_address, callback) ||
      !acknowledgeCdRomInterrupt(data_ready_interrupt) ||
      !runtime_.loadBytes(ready_result_address, result) ||
      !runtime_.write8(ready_state_address, data_ready_interrupt)) {
    return false;
  }
  // The native 120 Hz scheduler owns hardware time. Running this HLE callback
  // through invoke() advanced CPU, CD and SPU clocks a second time during
  // streaming, producing large bursts of future PCM which the frontend then
  // had to discard. Execute it atomically like the other retail frame/audio
  // callbacks: instructions still run, but only the scheduler advances the
  // emulated hardware timeline.
  if (callback == 0U) {
    return true;
  }
  const auto callback_result = invokeFrameCall(
      callback,
      std::array{static_cast<std::uint32_t>(data_ready_interrupt),
                 ready_result_address},
      5'000'000U);
  return callback_result.completed();
}

void LegacyGameplayVm::recoverCdRomTransfer() noexcept {
  constexpr std::uint32_t cdrom_base = 0x1f801800U;
  constexpr std::uint32_t cdrom_dma_chcr = 0x1f8010b8U;
  constexpr std::uint32_t interrupt_status = 0x1f801070U;

  static_cast<void>(runtime_.write32(cdrom_dma_chcr, 0U));
  machine_.cdrom().reset();
  static_cast<void>(runtime_.write8(cdrom_base, 0U));
  static_cast<void>(runtime_.write16(interrupt_status, 0xfffbU));
}

bool LegacyGameplayVm::issueCdRomCommand(
    std::uint8_t command, std::span<const std::uint8_t> parameters,
    bool wait_for_completion) noexcept {
  constexpr std::uint32_t cdrom_base = 0x1f801800U;
  if (parameters.size() > psx::CdRomState::fifo_capacity ||
      (machine_.cdrom().captureState().interrupt_flags & 0x07U) != 0U ||
      !runtime_.write8(cdrom_base, 1U) ||
      !runtime_.write8(cdrom_base + 3U, 0x40U) ||
      !runtime_.write8(cdrom_base, 0U)) {
    return false;
  }
  for (const auto parameter : parameters) {
    if (!runtime_.write8(cdrom_base + 2U, parameter)) {
      return false;
    }
  }
  if (!runtime_.write8(cdrom_base + 1U, command) ||
      !waitForCdRomInterrupt(3U) || !acknowledgeCdRomInterrupt(3U)) {
    return false;
  }
  return !wait_for_completion ||
         (waitForCdRomInterrupt(2U) && acknowledgeCdRomInterrupt(2U));
}

bool LegacyGameplayVm::transferCdRomSectors(std::uint32_t sector,
                                            std::uint32_t sector_count,
                                            std::uint32_t destination,
                                            std::uint8_t mode) noexcept {
  constexpr std::uint32_t cdrom_base = 0x1f801800U;
  constexpr std::uint32_t dma3_madr = 0x1f8010b0U;
  constexpr std::uint32_t dma3_bcr = 0x1f8010b4U;
  constexpr std::uint32_t dma3_chcr = 0x1f8010b8U;
  constexpr std::uint32_t dma_dpcr = 0x1f8010f0U;
  constexpr std::uint32_t dma3_enable = 1U << 15U;
  constexpr std::uint32_t dma3_control = 0x11000000U;
  constexpr std::uint32_t data_words = LegacyVirtualCd::sector_size / 4U;
  constexpr std::uint32_t raw_header_size = 12U;

  if (sector_count == 0U) {
    return true;
  }
  if (destination == 0U || sector_count > psx::R3000Runtime::ram_size /
                                              LegacyVirtualCd::sector_size) {
    return false;
  }
  const auto last_byte =
      static_cast<std::uint64_t>(destination) +
      static_cast<std::uint64_t>(sector_count) * LegacyVirtualCd::sector_size -
      1U;
  if (last_byte > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  std::array<std::byte, 4U> encoded_position{};
  if (!encodeCdPosition(sector, encoded_position)) {
    return false;
  }
  const std::array location_parameters{
      std::to_integer<std::uint8_t>(encoded_position[0]),
      std::to_integer<std::uint8_t>(encoded_position[1]),
      std::to_integer<std::uint8_t>(encoded_position[2]),
  };
  const std::array mode_parameters{mode};
  const auto fail = [this]() noexcept {
    recoverCdRomTransfer();
    return false;
  };
  if (!issueCdRomCommand(0x02U, location_parameters) ||
      !issueCdRomCommand(0x0eU, mode_parameters) ||
      !issueCdRomCommand(0x06U, {})) {
    return fail();
  }

  for (std::uint32_t index = 0U; index < sector_count; ++index) {
    if (!waitForCdRomInterrupt(1U) || !runtime_.write8(cdrom_base, 0U) ||
        !runtime_.write8(cdrom_base + 3U, 0x80U)) {
      return fail();
    }

    if ((mode & 0x20U) != 0U) {
      std::array<std::uint8_t, raw_header_size> header{};
      for (auto &byte : header) {
        if (!runtime_.read8(cdrom_base + 2U, byte)) {
          return fail();
        }
      }
      std::array<std::byte, 4U> expected_position{};
      if (!encodeCdPosition(sector + index, expected_position) ||
          header[0] != std::to_integer<std::uint8_t>(expected_position[0]) ||
          header[1] != std::to_integer<std::uint8_t>(expected_position[1]) ||
          header[2] != std::to_integer<std::uint8_t>(expected_position[2]) ||
          header[3] != 0x02U) {
        return fail();
      }
    }

    std::uint32_t dpcr{};
    if (!runtime_.read32(dma_dpcr, dpcr) ||
        !runtime_.write32(dma_dpcr, dpcr | dma3_enable) ||
        !runtime_.write32(dma3_madr,
                          destination + index * LegacyVirtualCd::sector_size) ||
        !runtime_.write32(dma3_bcr, 0x00010000U | data_words) ||
        !runtime_.write32(dma3_chcr, dma3_control)) {
      return fail();
    }
    if (!machine_.completePendingDmaTransfers()) {
      return fail();
    }
    std::uint32_t channel_control{};
    if (!runtime_.read32(dma3_chcr, channel_control) ||
        (channel_control & (1U << 24U)) != 0U ||
        !runtime_.write8(cdrom_base, 0U) ||
        !runtime_.write8(cdrom_base + 3U, 0U) ||
        !acknowledgeCdRomInterrupt(1U)) {
      return fail();
    }
  }

  if (!issueCdRomCommand(0x09U, {}, true)) {
    return fail();
  }
  return true;
}

bool LegacyMissionTickResult::completed() const noexcept {
  return !bridge_fault && frame_event.completed() &&
         delayed_callbacks.completed() && queue_drain.completed() &&
         std::ranges::all_of(dispatched_events,
                             &LegacyGameplayVmResult::completed);
}

bool LegacyRetailPlatformTailResult::completed() const noexcept {
  return !bridge_fault && delayed_callbacks.completed() &&
         (!fade_callback || fade_callback->completed());
}

bool LegacyRetailOuterFrameResult::completed() const noexcept {
  const auto tail_completed =
      tail_skipped ||
      (renderer_tail ? renderer_tail->completed() : platform_tail.completed());
  return !bridge_fault && !unsupported_state && tail_completed &&
         std::ranges::all_of(guest_calls, &LegacyGameplayVmResult::completed);
}

bool LegacyFirstMissionOpeningResult::completed() const noexcept {
  return remove_movie_callback.completed() && fade_reset.completed() &&
         fade_start.completed() && camera_event.completed();
}

std::uint64_t LegacyMissionTickResult::instructions() const noexcept {
  auto total = frame_event.execution.instructions +
               delayed_callbacks.execution.instructions +
               queue_drain.execution.instructions;
  for (const auto &event : dispatched_events) {
    total += event.execution.instructions;
  }
  return total;
}

std::uint64_t LegacyMissionTickResult::hostCalls() const noexcept {
  auto total = frame_event.host_calls + delayed_callbacks.host_calls +
               queue_drain.host_calls;
  for (const auto &event : dispatched_events) {
    total += event.host_calls;
  }
  return total;
}

void LegacyGameplayVm::bindHostCall(std::uint32_t address,
                                    LegacyHostCall call) {
  if (call) {
    const auto [entry, inserted] =
        host_calls_.insert_or_assign(address, std::move(call));
    static_cast<void>(inserted);
    if (address >= 0x80000000U && address < 0x80200000U &&
        (address & 3U) == 0U) {
      ram_host_calls_[(address - 0x80000000U) / sizeof(std::uint32_t)] =
          &entry->second;
    }
  } else {
    static_cast<void>(unbindHostCall(address));
  }
}

LegacyHostCall *LegacyGameplayVm::findHostCall(std::uint32_t address) noexcept {
  if (address >= 0x80000000U && address < 0x80200000U && (address & 3U) == 0U) {
    return ram_host_calls_[(address - 0x80000000U) / sizeof(std::uint32_t)];
  }
  const auto entry = host_calls_.find(address);
  return entry == host_calls_.end() ? nullptr : &entry->second;
}

const LegacyHostCall *
LegacyGameplayVm::findHostCall(std::uint32_t address) const noexcept {
  return const_cast<LegacyGameplayVm *>(this)->findHostCall(address);
}

void LegacyGameplayVm::bindPsxBiosRandomCalls() {
  constexpr std::uint32_t random_address = 0xbfc02200U;
  constexpr std::uint32_t seed_random_address = 0xbfc02230U;
  constexpr std::uint32_t random_seed_address = 0xa0009010U;
  bindHostCall(random_address, [](LegacyHostCallContext &context) {
    std::uint32_t seed{};
    if (!context.read32(random_seed_address, seed)) {
      context.setReturnValue(0U);
      return;
    }
    seed = seed * 0x41c64e6dU + 0x3039U;
    static_cast<void>(context.write32(random_seed_address, seed));
    context.setReturnValue((seed >> 16U) & 0x7fffU);
  });
  bindHostCall(seed_random_address, [](LegacyHostCallContext &context) {
    static_cast<void>(
        context.write32(random_seed_address, context.argument(0)));
  });
}

void LegacyGameplayVm::bindPsxLibcStringCalls() {
  constexpr std::uint32_t strcmp_address = 0x800ec884U;
  constexpr std::uint32_t strcpy_address = 0x800ec894U;
  constexpr std::uint32_t strlen_address = 0x800ec8a4U;
  constexpr std::uint32_t memcpy_address = 0x800ec8d4U;
  constexpr std::uint32_t memset_address = 0x800ec8e4U;
  constexpr std::uint32_t sprintf_address = 0x800ec924U;

  bindHostCall(strcmp_address, [](LegacyHostCallContext &context) {
    const auto left_address = context.argument(0);
    const auto right_address = context.argument(1);
    for (std::uint32_t index = 0U; index < psx::R3000Runtime::ram_size;
         ++index) {
      std::uint8_t left{};
      std::uint8_t right{};
      if (!context.read8(left_address + index, left) ||
          !context.read8(right_address + index, right)) {
        context.setReturnValue(0U);
        return;
      }
      if (left != right || left == 0U) {
        context.setReturnValue(
            std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(left) -
                                         static_cast<std::int32_t>(right)));
        return;
      }
    }
    context.setReturnValue(0U);
  });
  bindHostCall(strcpy_address, [](LegacyHostCallContext &context) {
    const auto destination = context.argument(0);
    const auto source = context.argument(1);
    for (std::uint32_t index = 0U; index < psx::R3000Runtime::ram_size;
         ++index) {
      std::uint8_t value{};
      if (!context.read8(source + index, value) ||
          !context.write8(destination + index, value)) {
        context.setReturnValue(0U);
        return;
      }
      if (value == 0U) {
        context.setReturnValue(destination);
        return;
      }
    }
    context.setReturnValue(0U);
  });
  bindHostCall(strlen_address, [](LegacyHostCallContext &context) {
    const auto string_address = context.argument(0);
    for (std::uint32_t length = 0U; length < psx::R3000Runtime::ram_size;
         ++length) {
      std::uint8_t value{};
      if (!context.read8(string_address + length, value)) {
        context.setReturnValue(0U);
        return;
      }
      if (value == 0U) {
        context.setReturnValue(length);
        return;
      }
    }
    context.setReturnValue(0U);
  });
  bindHostCall(memcpy_address, [](LegacyHostCallContext &context) {
    const auto destination = context.argument(0);
    const auto source = context.argument(1);
    const auto size = context.argument(2);
    if (size > psx::R3000Runtime::ram_size) {
      context.setReturnValue(0U);
      return;
    }
    std::vector<std::byte> bytes(size);
    if (!context.readBytes(source, bytes) ||
        !context.writeBytes(destination, bytes)) {
      context.setReturnValue(0U);
      return;
    }
    context.setReturnValue(destination);
  });
  bindHostCall(memset_address, [](LegacyHostCallContext &context) {
    const auto destination = context.argument(0);
    const auto size = context.argument(2);
    if (size > psx::R3000Runtime::ram_size) {
      context.setReturnValue(0U);
      return;
    }
    const std::vector<std::byte> bytes(
        size, static_cast<std::byte>(context.argument(1) & 0xffU));
    context.setReturnValue(context.writeBytes(destination, bytes) ? destination
                                                                  : 0U);
  });
  bindHostCall(sprintf_address, [](LegacyHostCallContext &context) {
    const auto destination = context.argument(0);
    std::string format;
    if (destination == 0U ||
        !context.readCString(context.argument(1), format, 1024U)) {
      context.setReturnValue(0U);
      return;
    }

    std::string output;
    output.reserve(format.size() + 64U);
    auto argument_index = std::size_t{2U};
    auto valid = true;
    for (std::size_t index = 0U; index < format.size() && valid; ++index) {
      if (format[index] != '%') {
        output.push_back(format[index]);
        continue;
      }
      ++index;
      if (index >= format.size()) {
        valid = false;
        break;
      }
      if (format[index] == '%') {
        output.push_back('%');
        continue;
      }

      const auto zero_padded = format[index] == '0';
      if (zero_padded) {
        ++index;
      }
      std::size_t width{};
      while (index < format.size() && format[index] >= '0' &&
             format[index] <= '9') {
        width = width * 10U + static_cast<std::size_t>(format[index] - '0');
        ++index;
      }
      if (index < format.size() &&
          (format[index] == 'l' || format[index] == 'h')) {
        ++index;
      }
      if (index >= format.size()) {
        valid = false;
        break;
      }

      std::string value;
      const auto argument = context.argument(argument_index++);
      if (format[index] == 's') {
        valid = context.readCString(argument, value, 4096U);
      } else if (format[index] == 'c') {
        value.push_back(static_cast<char>(argument & 0xffU));
      } else if (format[index] == 'd' || format[index] == 'i' ||
                 format[index] == 'u' || format[index] == 'x' ||
                 format[index] == 'X') {
        std::array<char, 32> buffer{};
        std::to_chars_result conversion{};
        if (format[index] == 'd' || format[index] == 'i') {
          conversion =
              std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                            std::bit_cast<std::int32_t>(argument));
        } else {
          const auto base = format[index] == 'u' ? 10 : 16;
          conversion = std::to_chars(
              buffer.data(), buffer.data() + buffer.size(), argument, base);
        }
        valid = conversion.ec == std::errc{};
        if (valid) {
          value.assign(buffer.data(), conversion.ptr);
          if (format[index] == 'X') {
            std::ranges::transform(
                value, value.begin(), [](unsigned char character) {
                  return static_cast<char>(std::toupper(character));
                });
          }
        }
      } else {
        valid = false;
      }
      if (width > value.size()) {
        output.append(width - value.size(), zero_padded ? '0' : ' ');
      }
      output += value;
    }
    if (!valid || output.size() >= psx::R3000Runtime::ram_size) {
      context.setReturnValue(0U);
      return;
    }
    std::vector<std::byte> encoded(output.size() + 1U);
    std::ranges::transform(output, encoded.begin(), [](char character) {
      return static_cast<std::byte>(character);
    });
    encoded.back() = std::byte{};
    context.setReturnValue(context.writeBytes(destination, encoded)
                               ? static_cast<std::uint32_t>(output.size())
                               : 0U);
  });
}

void LegacyGameplayVm::bindPsxVideoTimingCall() {
  constexpr std::uint32_t vsync_address = 0x800e3f54U;
  constexpr std::uint32_t retrace_counter_address = 0x8010f378U;
  bindHostCall(vsync_address, [this](LegacyHostCallContext &context) {
    std::uint32_t counter{};
    if (!context.read32(retrace_counter_address, counter)) {
      context.rejectHostCall();
      return;
    }
    const auto mode = std::bit_cast<std::int32_t>(context.argument(0));
    if (mode < 0) {
      // VSync(-1) is a pure query. Presented guest frames advance the
      // software VBlank counter explicitly at the renderer boundary.
      context.setReturnValue(counter);
      return;
    }

    if (!video_timing_baseline_initialized_) {
      video_timing_baseline_ = counter;
      video_timing_baseline_initialized_ = true;
    }
    const auto elapsed = counter - video_timing_baseline_;
    if (mode == 1) {
      context.setReturnValue(elapsed);
      return;
    }

    const auto wait = mode > 1 ? static_cast<std::uint32_t>(mode) : 1U;
    if (elapsed < wait) {
      counter += wait - elapsed;
      if (!context.write32(retrace_counter_address, counter)) {
        context.rejectHostCall();
        return;
      }
    }
    video_timing_baseline_ = counter;
    context.setReturnValue(elapsed);
  });
}

void LegacyGameplayVm::bindPsxCriticalSectionCalls() {
  // Guest execution is single-threaded and platform callbacks only enter at a
  // frame boundary, so the PsyQ syscall pair is already atomic on the host.
  constexpr std::uint32_t enter_critical_section_address = 0x800e3f34U;
  constexpr std::uint32_t exit_critical_section_address = 0x800e3f44U;
  bindHostCall(
      enter_critical_section_address,
      [](LegacyHostCallContext &context) { context.setReturnValue(1U); });
  bindHostCall(
      exit_critical_section_address,
      [](LegacyHostCallContext &context) { context.setReturnValue(0U); });
}

void LegacyGameplayVm::bindPsxGpuSubmissionCall() {
  // The guest has already built its ordering table and GPU packet chain when
  // it reaches this boundary. Native rendering consumes that state separately;
  // submitting the chain to PSX DMA/GPU hardware is intentionally host-owned.
  constexpr std::uint32_t submit_ordering_table_address = 0x800e6e74U;
  bindHostCall(
      submit_ordering_table_address,
      [](LegacyHostCallContext &context) { context.setReturnValue(0U); });
}

void LegacyGameplayVm::bindSyphonFilterUsaV11VirtualCdCalls(
    std::shared_ptr<LegacyVirtualCd> virtual_cd) {
  constexpr std::uint32_t search_address = 0x800deb50U;
  constexpr std::uint32_t unmount_address = 0x800dec48U;
  constexpr std::uint32_t mount_address = 0x800dec7cU;
  constexpr std::uint32_t open_address = 0x800deef4U;
  constexpr std::uint32_t padded_size_address = 0x800df148U;
  constexpr std::uint32_t read_address = 0x800df198U;
  constexpr std::uint32_t rewind_address = 0x800df32cU;
  constexpr std::uint32_t close_address = 0x800df3b0U;
  constexpr std::uint32_t control_address = 0x800ed5c0U;
  constexpr std::uint32_t control_fast_address = 0x800ed6fcU;
  constexpr std::uint32_t raw_read_address = 0x800f0384U;
  constexpr std::uint32_t raw_sync_address = 0x800f0520U;

  if (!virtual_cd) {
    static_cast<void>(unbindHostCall(search_address));
    static_cast<void>(unbindHostCall(unmount_address));
    static_cast<void>(unbindHostCall(mount_address));
    static_cast<void>(unbindHostCall(open_address));
    static_cast<void>(unbindHostCall(padded_size_address));
    static_cast<void>(unbindHostCall(read_address));
    static_cast<void>(unbindHostCall(rewind_address));
    static_cast<void>(unbindHostCall(close_address));
    static_cast<void>(unbindHostCall(control_address));
    static_cast<void>(unbindHostCall(control_fast_address));
    static_cast<void>(unbindHostCall(raw_read_address));
    static_cast<void>(unbindHostCall(raw_sync_address));
    machine_.setCdRomMedia(nullptr);
    virtual_cd_.reset();
    return;
  }

  virtual_cd_ = virtual_cd;
  machine_.setCdRomMedia(virtual_cd.get());
  static_cast<void>(runtime_.write8(0x1f801800U, 1U));
  static_cast<void>(runtime_.write8(0x1f801802U, 0x1fU));
  static_cast<void>(runtime_.write8(0x1f801800U, 0U));
  bindHostCall(open_address, [virtual_cd](LegacyHostCallContext &context) {
    std::string path;
    std::uint32_t handle{};
    const auto output = context.argument(1);
    auto result = LegacyCdResult::invalid_argument;
    std::uint32_t previous_output{};
    if (output != 0U && context.read32(output, previous_output) &&
        context.readCString(context.argument(0), path)) {
      result = virtual_cd->open(path, handle);
      static_cast<void>(context.write32(output, handle));
    }
    context.setReturnValue(static_cast<std::uint32_t>(result));
  });
  bindHostCall(
      padded_size_address, [virtual_cd](LegacyHostCallContext &context) {
        std::uint32_t size{};
        const auto output = context.argument(1);
        auto result = output == 0U
                          ? LegacyCdResult::invalid_argument
                          : virtual_cd->paddedSize(context.argument(0), size);
        if (output != 0U && !context.write32(output, size)) {
          result = LegacyCdResult::invalid_argument;
        }
        context.setReturnValue(static_cast<std::uint32_t>(result));
      });
  bindHostCall(read_address, [this,
                              virtual_cd](LegacyHostCallContext &context) {
    const auto destination = context.argument(1);
    const auto requested = context.argument(2);
    const auto output = context.argument(3);
    if (destination == 0U || output == 0U ||
        requested > psx::R3000Runtime::ram_size) {
      if (output != 0U) {
        static_cast<void>(context.write32(output, 0U));
      }
      context.setReturnValue(
          static_cast<std::uint32_t>(LegacyCdResult::invalid_argument));
      return;
    }
    std::uint32_t previous_output{};
    if (!context.read32(output, previous_output)) {
      context.setReturnValue(
          static_cast<std::uint32_t>(LegacyCdResult::invalid_argument));
      return;
    }
    const auto plan = virtual_cd->planRead(context.argument(0), requested);
    auto result = plan.result;
    if (result == LegacyCdResult::success && plan.transfer_size != 0U) {
      const auto sectors = plan.transfer_size / LegacyVirtualCd::sector_size;
      if (!transferCdRomSectors(plan.sector, sectors, destination, 0xa0U) ||
          virtual_cd->commitRead(context.argument(0), plan.sector,
                                 plan.bytes_read) != LegacyCdResult::success) {
        result = LegacyCdResult::invalid_argument;
      }
    }
    const auto bytes_read =
        result == LegacyCdResult::success ? plan.bytes_read : 0U;
    if (!context.write32(output, bytes_read)) {
      result = LegacyCdResult::invalid_argument;
    }
    context.setReturnValue(static_cast<std::uint32_t>(result));
  });
  bindHostCall(rewind_address, [virtual_cd](LegacyHostCallContext &context) {
    context.setReturnValue(
        static_cast<std::uint32_t>(virtual_cd->rewind(context.argument(0))));
  });
  bindHostCall(close_address, [virtual_cd](LegacyHostCallContext &context) {
    std::uint32_t handle{};
    const auto pointer = context.argument(0);
    auto result = pointer != 0U && context.read32(pointer, handle)
                      ? virtual_cd->close(handle)
                      : LegacyCdResult::invalid_argument;
    if (result == LegacyCdResult::success && !context.write32(pointer, 0U)) {
      result = LegacyCdResult::invalid_argument;
    }
    context.setReturnValue(static_cast<std::uint32_t>(result));
  });
  bindHostCall(mount_address, [virtual_cd](LegacyHostCallContext &context) {
    std::string path;
    const auto mounted = context.readCString(context.argument(0), path) &&
                         virtual_cd->mount(path);
    context.setReturnValue(mounted ? 1U : 0U);
  });
  bindHostCall(unmount_address, [virtual_cd](LegacyHostCallContext &context) {
    virtual_cd->unmount();
    context.setReturnValue(0U);
  });
  bindHostCall(search_address, [virtual_cd](LegacyHostCallContext &context) {
    constexpr std::size_t cdl_file_size = 24U;
    const auto output = context.argument(0);
    std::string path;
    std::array<std::byte, cdl_file_size> previous{};
    if (output == 0U || !context.readBytes(output, previous) ||
        !context.readCString(context.argument(1), path)) {
      context.setReturnValue(0U);
      return;
    }
    const auto location = virtual_cd->locate(path);
    std::array<std::byte, cdl_file_size> file{};
    std::array<std::byte, 4U> position{};
    if (!location || !encodeCdPosition(location->sector, position)) {
      context.setReturnValue(0U);
      return;
    }
    std::ranges::copy(position, file.begin());
    for (std::size_t index = 0U; index < sizeof(location->size); ++index) {
      file[4U + index] = static_cast<std::byte>(location->size >> (index * 8U));
    }
    if (!context.writeBytes(output, file)) {
      context.setReturnValue(0U);
      return;
    }
    context.setReturnValue(output);
  });
  const auto control = [this, virtual_cd](LegacyHostCallContext &context) {
    constexpr std::uint32_t set_location_command = 2U;
    constexpr std::uint32_t read_command = 6U;
    constexpr std::uint32_t read_stream_command = 0x1bU;
    const auto command = static_cast<std::uint8_t>(context.argument(0));
    const auto implicit_set_location =
        (command == read_command || command == read_stream_command) &&
        context.argument(1) != 0U;
    std::array<std::uint8_t, 3U> parameters{};
    std::size_t parameter_count{};
    std::optional<std::uint32_t> pending_raw_sector;
    if (command == set_location_command || implicit_set_location) {
      std::array<std::byte, 4U> position{};
      if (context.argument(1) == 0U ||
          !context.readBytes(context.argument(1), position)) {
        context.setReturnValue(0U);
        return;
      }
      const auto sector = decodeCdPosition(position);
      if (!sector) {
        context.setReturnValue(0U);
        return;
      }
      pending_raw_sector = *sector;
      for (std::size_t index = 0U; index < 3U; ++index) {
        parameters[index] = std::to_integer<std::uint8_t>(position[index]);
      }
      parameter_count = 3U;
    } else if (command == 0x0dU) {
      if (context.argument(1) == 0U ||
          !context.read8(context.argument(1), parameters[0]) ||
          !context.read8(context.argument(1) + 1U, parameters[1])) {
        context.setReturnValue(0U);
        return;
      }
      parameter_count = 2U;
    } else if (command == 0x0eU) {
      if (context.argument(1) == 0U ||
          !context.read8(context.argument(1), parameters[0])) {
        context.setReturnValue(0U);
        return;
      }
      parameter_count = 1U;
    }
    if (implicit_set_location &&
        !issueCdRomCommand(set_location_command, parameters)) {
      recoverCdRomTransfer();
      context.setReturnValue(0U);
      return;
    }
    const auto command_parameters =
        implicit_set_location
            ? std::span<const std::uint8_t>{}
            : std::span<const std::uint8_t>{parameters}.first(parameter_count);
    const auto needs_completion = command == 0x08U || command == 0x09U ||
                                  command == 0x0aU || command == 0x15U;
    const auto completed =
        issueCdRomCommand(command, command_parameters, needs_completion);
    if (completed && pending_raw_sector) {
      virtual_cd->setCurrentRawSector(*pending_raw_sector);
    } else if (!completed) {
      recoverCdRomTransfer();
    }
    static_cast<void>(context.write8(0x80114cddU, command));
    static_cast<void>(context.write8(0x80114f9cU, 2U));
    static_cast<void>(context.write8(0x80114f9dU, 0U));
    context.setReturnValue(completed ? 1U : 0U);
  };
  bindHostCall(control_address, control);
  bindHostCall(control_fast_address, control);
  bindHostCall(raw_read_address, [this,
                                  virtual_cd](LegacyHostCallContext &context) {
    const auto sectors = context.argument(0);
    const auto destination = context.argument(1);
    if (destination == 0U ||
        sectors > psx::R3000Runtime::ram_size / LegacyVirtualCd::sector_size) {
      context.setReturnValue(0U);
      return;
    }
    const auto mode = static_cast<std::uint8_t>(context.argument(2) | 0x20U);
    if (!transferCdRomSectors(virtual_cd->currentRawSector(), sectors,
                              destination, mode)) {
      context.setReturnValue(0U);
      return;
    }
    virtual_cd->advanceRawSectors(sectors);
    static_cast<void>(context.write32(0x80114fe8U, 0U));
    static_cast<void>(context.write32(0x80114fecU, destination));
    static_cast<void>(
        context.write32(0x80114ff4U, context.argument(2) | 0x20U));
    static_cast<void>(context.write32(0x8011500cU, 0U));
    context.setReturnValue(1U);
  });
  bindHostCall(raw_sync_address, [](LegacyHostCallContext &context) {
    const auto output = context.argument(1);
    if (output != 0U) {
      for (std::uint32_t index = 0U; index < 8U; ++index) {
        static_cast<void>(context.write8(output + index, 0U));
      }
    }
    // FUN_8008294c polls mode 1 once while the stream queue is idle and
    // again after FUN_800f0384 has installed DAT_8011609c. The first poll
    // must report idle (2); the second completes the synchronous virtual
    // transfer (0). Returning 0 for both enters retail's 0x78-VBlank
    // hardware timeout loop inside a single guest call.
    std::uint32_t stream_transfer{};
    const auto stream_poll = context.argument(0) != 0U;
    if (stream_poll && !context.read32(0x8011609cU, stream_transfer)) {
      context.rejectHostCall();
      return;
    }
    context.setReturnValue(stream_poll && stream_transfer == 0U ? 2U : 0U);
  });
}

void LegacyGameplayVm::bindSyphonFilterUsaV11PlatformCalls() {
  bindPsxBiosRandomCalls();
  bindPsxLibcStringCalls();
  bindPsxVideoTimingCall();
  bindPsxCriticalSectionCalls();
  bindPsxGpuSubmissionCall();
}

void LegacyGameplayVm::bindSyphonFilterUsaV11Park2FlameLosHook(
    const LegacyPark2FlameLosProfile &profile) {
  bindHostCall(
      profile.event_entry, [this, profile](LegacyHostCallContext &context) {
        if (!park2_flame_line_of_sight_clear_.has_value() ||
            *park2_flame_line_of_sight_clear_) {
          context.continueGuestInstruction();
          return;
        }

        const auto return_address = context.returnAddress();
        std::uint32_t call_instruction{};
        std::uint32_t delay_instruction{};
        std::uint32_t current_player{};
        if (return_address < 8U ||
            !context.read32(return_address - 8U, call_instruction) ||
            !context.read32(return_address - 4U, delay_instruction) ||
            !context.read32(profile.current_player_slot, current_player) ||
            !legacyPark2FlameEventSuppressed(
                park2_flame_line_of_sight_clear_, return_address,
                call_instruction, delay_instruction, context.argument(0U),
                context.argument(1U), context.argument(2U),
                context.argument(3U), current_player, profile)) {
          context.continueGuestInstruction();
          return;
        }

        // HLE-return to the exact PARK2 continuation, skipping only the blocked
        // proximity event. The retail flame animation and state machine
        // continue.
        context.setReturnValue(0U);
      });
}

void LegacyGameplayVm::bindSyphonFilterUsaV11BootstrapPlatformCalls() {
  bindSyphonFilterUsaV11PlatformCalls();
  bindSyphonFilterUsaV11Park2FlameLosHook();
  bindSyphonFilterUsaV11HostAimRayHook();
  bindSyphonFilterUsaV11AgentEnemyAimHooks();
  bindSyphonFilterUsaV11AgentGrenadeAwarenessHook();
  bindSyphonFilterUsaV11WeaponEventHooks();
  bindSyphonFilterUsaV11GameplayTextHooks();
  const auto bind_zero_result = [this](std::uint32_t address) {
    bindHostCall(address, [](LegacyHostCallContext &context) {
      context.setReturnValue(0U);
    });
  };
  const auto bind_success_result = [this](std::uint32_t address) {
    bindHostCall(address, [](LegacyHostCallContext &context) {
      context.setReturnValue(1U);
    });
  };

  // GPU/interrupt/pad entry points below only submit work to PSX hardware.
  // Guest-side resource parsing and state construction remain untouched.
  constexpr std::array zero_result_calls{
      0x800e4c84U, // ResetGraph
      0x800e5000U, // DrawSync
      0x800e5184U, // ClearImage
      0x800e52acU, // LoadImage
      0x800e7ca4U, // BreakDraw
      0x800e41e4U, // DMACallback; skipped STR playback owns no host DMA
      0x800e4214U, // VSyncCallback
      0x800e4248U, // VSyncCallbacks; native frame tail owns callback dispatch
      0x800ec2a4U, // _patch_gte BIOS exception hook
      0x80141828U, // COMMON memory-card IRQ/C0 table patch
      0x801418bcU, // COMMON B0/ChangeClearPAD table patch
      0x8013e8f4U, // Skipped MOVIE overlay finalization; no STR playback
      0x800ec84cU, // BIOS B0:3f debug console alias
      0x800ec914U, // BIOS B0:3f debug console alias
  };
  for (const auto address : zero_result_calls) {
    bind_zero_result(address);
  }
  const auto audio_profile = syphonFilterUsaV11RetailAudioProfile();
  bindHostCall(audio_profile.reset_callback_entry,
               [this](LegacyHostCallContext &context) {
                 interrupt_callbacks_.fill(0U);
                 context.setReturnValue(0U);
               });
  bindHostCall(audio_profile.interrupt_callback_entry,
               [this](LegacyHostCallContext &context) {
                 const auto interrupt = context.argument(0);
                 const auto callback = context.argument(1);
                 if (interrupt >= interrupt_callbacks_.size() ||
                     !validInterruptCallback(callback)) {
                   context.rejectHostCall();
                   return;
                 }
                 const auto index = static_cast<std::size_t>(interrupt);
                 const auto previous = interrupt_callbacks_[index];
                 interrupt_callbacks_[index] = callback;
                 context.setReturnValue(previous);
               });
  constexpr std::array success_result_calls{
      0x800ff454U, // PadStartCom
  };
  for (const auto address : success_result_calls) {
    bind_success_result(address);
  }
  constexpr std::uint32_t clear_ordering_table_reverse_address = 0x800e54ecU;
  bindHostCall(
      clear_ordering_table_reverse_address, [](LegacyHostCallContext &context) {
        constexpr std::uint32_t ordering_table_entry_size = 4U;
        const auto ordering_table = context.argument(0);
        const auto entries = context.argument(1);
        if (entries > psx::R3000Runtime::ram_size / ordering_table_entry_size) {
          context.rejectHostCall();
          return;
        }
        if (entries != 0U) {
          const auto last_entry = static_cast<std::uint64_t>(ordering_table) +
                                  static_cast<std::uint64_t>(entries - 1U) *
                                      ordering_table_entry_size;
          if (last_entry > std::numeric_limits<std::uint32_t>::max()) {
            context.rejectHostCall();
            return;
          }
        }
        for (std::uint32_t index = 0U; index < entries; ++index) {
          const auto link = index == 0U
                                ? 0x00ffffffU
                                : (ordering_table +
                                   (index - 1U) * ordering_table_entry_size) &
                                      0x00ffffffU;
          if (!context.write32(
                  ordering_table + index * ordering_table_entry_size, link)) {
            context.rejectHostCall();
            return;
          }
        }
        context.setReturnValue(0U);
      });
  constexpr std::uint32_t cd_init_address = 0x800eececU;
  bindHostCall(cd_init_address, [this](LegacyHostCallContext &context) {
    static_cast<void>(runtime_.write8(0x1f801800U, 1U));
    static_cast<void>(runtime_.write8(0x1f801802U, 0x1fU));
    static_cast<void>(runtime_.write8(0x1f801803U, 0x1fU));
    static_cast<void>(runtime_.write8(0x1f801800U, 0U));
    if (!issueCdRomCommand(0x01U, {}) || !issueCdRomCommand(0x0aU, {}, true) ||
        !issueCdRomCommand(0x0cU, {})) {
      context.rejectHostCall();
      return;
    }
    // Preserve libcd's software mirror after the hardware handshake.
    static_cast<void>(context.write8(0x80114cdcU, 0U));
    static_cast<void>(context.write8(0x80114cddU, 0U));
    static_cast<void>(context.write32(0x80114cc0U, 0U));
    static_cast<void>(context.write32(0x80114cc4U, 0U));
    static_cast<void>(context.write32(0x80114cccU, 0U));
    static_cast<void>(context.write32(0x80114cd0U, 0U));
    static_cast<void>(context.write8(0x80114f9cU, 2U));
    static_cast<void>(context.write8(0x80114f9dU, 0U));
    static_cast<void>(context.write8(0x80114f9eU, 0U));
    context.setReturnValue(0U);
  });
  constexpr std::uint32_t cd_control_address = 0x800ee648U;
  bindHostCall(cd_control_address, [this](LegacyHostCallContext &context) {
    const auto command = static_cast<std::uint8_t>(context.argument(0));
    std::array<std::uint8_t, 3U> parameters{};
    std::size_t parameter_count{};
    if (command == 0x02U) {
      parameter_count = 3U;
    } else if (command == 0x0dU) {
      parameter_count = 2U;
    } else if (command == 0x0eU) {
      parameter_count = 1U;
    }
    if (parameter_count != 0U) {
      if (context.argument(1) == 0U) {
        context.rejectHostCall();
        return;
      }
      for (std::size_t index = 0U; index < parameter_count; ++index) {
        if (!context.read8(context.argument(1) +
                               static_cast<std::uint32_t>(index),
                           parameters[index])) {
          context.rejectHostCall();
          return;
        }
      }
    }
    const auto needs_completion = command == 0x08U || command == 0x09U ||
                                  command == 0x0aU || command == 0x15U;
    if (!issueCdRomCommand(
            command,
            std::span<const std::uint8_t>{parameters}.first(parameter_count),
            needs_completion)) {
      context.rejectHostCall();
      return;
    }
    static_cast<void>(context.write8(0x80114cddU, command));
    static_cast<void>(context.write8(0x80114f9cU, 2U));
    static_cast<void>(context.write8(0x80114f9dU, 0U));
    context.setReturnValue(0U);
  });
  constexpr std::uint32_t cd_sync_address = 0x800f0520U;
  bindHostCall(cd_sync_address, [](LegacyHostCallContext &context) {
    constexpr std::uint32_t command_complete = 2U;
    const auto result = context.argument(1);
    if (result != 0U) {
      for (std::uint32_t index = 0U; index < 8U; ++index) {
        static_cast<void>(context.write8(result + index, 0U));
      }
    }
    context.setReturnValue(command_complete);
  });
  constexpr std::uint32_t assertion_failure_address = 0x800ddc34U;
  bindHostCall(assertion_failure_address, [](LegacyHostCallContext &context) {
    context.rejectHostCall();
  });
  constexpr std::uint32_t bios_a0_vector = 0x000000a0U;
  bindHostCall(bios_a0_vector, [](LegacyHostCallContext &context) {
    constexpr std::uint32_t strcat_call = 0x15U;
    constexpr std::uint32_t strchr_call = 0x1eU;
    constexpr std::uint32_t bzero_call = 0x28U;
    constexpr std::uint32_t random_call = 0x2fU;
    constexpr std::uint32_t initialize_heap_call = 0x39U;
    constexpr std::uint32_t initialize_memory_card_devices_call = 0x70U;
    constexpr std::uint32_t random_seed_address = 0xa0009010U;
    const auto call = context.registerValue(9U);
    if (call == strcat_call) {
      const auto destination = context.argument(0);
      const auto source = context.argument(1);
      std::uint32_t destination_length{};
      for (; destination_length < psx::R3000Runtime::ram_size;
           ++destination_length) {
        std::uint8_t value{};
        if (!context.read8(destination + destination_length, value)) {
          context.rejectHostCall();
          return;
        }
        if (value == 0U) {
          break;
        }
      }
      if (destination_length == psx::R3000Runtime::ram_size) {
        context.rejectHostCall();
        return;
      }
      for (std::uint32_t index = 0U; index < psx::R3000Runtime::ram_size;
           ++index) {
        std::uint8_t value{};
        if (!context.read8(source + index, value) ||
            !context.write8(destination + destination_length + index, value)) {
          context.rejectHostCall();
          return;
        }
        if (value == 0U) {
          context.setReturnValue(destination);
          return;
        }
      }
      context.rejectHostCall();
      return;
    }
    if (call == strchr_call) {
      const auto string = context.argument(0);
      const auto character = static_cast<std::uint8_t>(context.argument(1));
      for (std::uint32_t index = 0U; index < psx::R3000Runtime::ram_size;
           ++index) {
        std::uint8_t value{};
        if (!context.read8(string + index, value)) {
          context.rejectHostCall();
          return;
        }
        if (value == character) {
          context.setReturnValue(string + index);
          return;
        }
        if (value == 0U) {
          context.setReturnValue(0U);
          return;
        }
      }
      context.rejectHostCall();
      return;
    }
    if (call == bzero_call) {
      const auto destination = context.argument(0);
      const auto length = context.argument(1);
      if (length > psx::R3000Runtime::ram_size) {
        context.rejectHostCall();
        return;
      }
      const std::vector<std::byte> zeroes(length);
      if (!context.writeBytes(destination, zeroes)) {
        context.rejectHostCall();
        return;
      }
      context.setReturnValue(destination);
      return;
    }
    if (call == random_call) {
      std::uint32_t seed{};
      if (!context.read32(random_seed_address, seed)) {
        context.rejectHostCall();
        return;
      }
      seed = seed * 0x41c64e6dU + 0x3039U;
      if (!context.write32(random_seed_address, seed)) {
        context.rejectHostCall();
        return;
      }
      context.setReturnValue((seed >> 16U) & 0x7fffU);
      return;
    }
    if (call == initialize_heap_call) {
      // The retail executable replaces allocation with its own arena
      // immediately after boot; only the BIOS bookkeeping call remains.
      context.setReturnValue(0U);
      return;
    }
    if (call != initialize_memory_card_devices_call) {
      context.rejectHostCall();
      return;
    }
    static_cast<void>(context.write32(0x00009f20U, 0U));
    static_cast<void>(context.write32(0x00009f24U, 0U));
    context.setReturnValue(0U);
  });
  constexpr std::uint32_t bios_b0_vector = 0x000000b0U;
  bindHostCall(bios_b0_vector, [this](LegacyHostCallContext &context) {
    constexpr std::uint32_t deliver_event_call = 0x07U;
    constexpr std::uint32_t open_event_call = 0x08U;
    constexpr std::uint32_t close_event_call = 0x09U;
    constexpr std::uint32_t wait_event_call = 0x0aU;
    constexpr std::uint32_t test_event_call = 0x0bU;
    constexpr std::uint32_t enable_event_call = 0x0cU;
    constexpr std::uint32_t disable_event_call = 0x0dU;
    constexpr std::uint32_t initialize_memory_card_call = 0x4aU;
    constexpr std::uint32_t start_memory_card_call = 0x4bU;
    constexpr std::uint32_t change_clear_pad_call = 0x5bU;
    const auto call = context.registerValue(9U);
    if (call == open_event_call) {
      // DMA4 is completed deterministically by PsxMachine. A single enabled
      // event handle is sufficient until the BIOS event table is emulated.
      context.setReturnValue(1U);
    } else if (call == wait_event_call) {
      if (const auto deadline =
              machine_.dmaCompletionTick(psx::DmaChannel::spu)) {
        const auto now = machine_.currentTick();
        if (*deadline > now) {
          machine_.advanceTicks(*deadline - now);
        }
      }
      context.setReturnValue(
          machine_.dma().scheduledToken(psx::DmaChannel::spu) == 0U ? 1U : 0U);
    } else if (call == test_event_call) {
      context.setReturnValue(
          machine_.dma().scheduledToken(psx::DmaChannel::spu) == 0U ? 1U : 0U);
    } else if (call == deliver_event_call || call == close_event_call ||
               call == enable_event_call || call == disable_event_call) {
      context.setReturnValue(1U);
    } else if (call == initialize_memory_card_call) {
      static_cast<void>(context.write8(0x00007264U, 0U));
      static_cast<void>(context.write8(0x00007568U, 1U));
      static_cast<void>(context.write8(0x00007569U, 1U));
      static_cast<void>(context.write32(0x000074b8U, context.argument(0)));
      context.setReturnValue(0U);
    } else if (call == start_memory_card_call) {
      static_cast<void>(context.write32(0x00008914U, 1U));
      static_cast<void>(context.write32(0x0000860cU, 0U));
      static_cast<void>(context.write32(0x000074bcU, 1U));
      context.setReturnValue(1U);
    } else if (call == change_clear_pad_call) {
      std::uint32_t previous{};
      static_cast<void>(context.read32(0x00008914U, previous));
      static_cast<void>(context.write32(0x00008914U, context.argument(0)));
      context.setReturnValue(previous);
    } else {
      context.rejectHostCall();
    }
  });
  constexpr std::uint32_t gpu_control_write_address = 0x800e6d60U;
  constexpr std::uint32_t gpu_control_shadow_address = 0x80125310U;
  bindHostCall(gpu_control_write_address, [](LegacyHostCallContext &context) {
    const auto command = context.argument(0);
    static_cast<void>(
        context.write8(gpu_control_shadow_address + (command >> 24U),
                       static_cast<std::uint8_t>(command)));
    context.setReturnValue(0U);
  });
}

void LegacyGameplayVm::bindAgentDifficultyDamageHook(
    const LegacyNativeMissionBridgeProfile &profile) {
  bindHostCall(profile.damage_entry, [this,
                                      profile](LegacyHostCallContext &context) {
    // This is an instruction-boundary patch, not an HLE replacement: the
    // retail damage routine must still execute for guest and host hits.
    context.continueGuestInstruction();
    if (!agent_difficulty_) {
      return;
    }

    constexpr std::size_t a3_register = 7U;
    std::uint32_t player{};
    std::uint16_t player_slot_bits{};
    if (!context.read32(profile.player_pointer, player) ||
        !validGuestRamRange(player, 4U) ||
        !context.read16(player + 2U, player_slot_bits)) {
      return;
    }

    const auto player_slot = std::bit_cast<std::int16_t>(player_slot_bits);
    const auto attacker_slot = std::bit_cast<std::int16_t>(
        static_cast<std::uint16_t>(context.argument(0U)));
    const auto owner_slot = std::bit_cast<std::int16_t>(
        static_cast<std::uint16_t>(context.argument(1U)));
    const auto target_slot = std::bit_cast<std::int16_t>(
        static_cast<std::uint16_t>(context.argument(2U)));
    const auto damage = std::bit_cast<std::int16_t>(
        static_cast<std::uint16_t>(context.argument(3U)));

    // CBDC personnel keep retail damage and reactions. Agent only records an
    // exact player-owned friendly-fire edge here; timer mutation is deferred
    // to the safe post-frame maintenance boundary.
    if (damage > 0 &&
        (attacker_slot == player_slot || owner_slot == player_slot) &&
        target_slot >= 0) {
      constexpr std::uint32_t object_record_stride = 0x4cU;
      constexpr std::uint32_t object_definition_stride = 0x14U;
      constexpr std::uint32_t object_instance_offset = 0x34U;
      constexpr std::uint32_t instance_slot_offset = 2U;
      std::uint16_t mission_bits{};
      std::uint32_t records{};
      std::uint32_t count{};
      std::uint32_t definitions{};
      std::uint32_t definition_count{};
      if (context.read16(profile.mission_index, mission_bits) &&
          mission_bits == 3U &&
          context.read32(profile.object_records_pointer, records) &&
          context.read32(profile.object_count, count) &&
          context.read32(profile.object_definitions_pointer, definitions) &&
          context.read32(profile.object_definition_count, definition_count) &&
          count <= profile.maximum_objects &&
          definition_count <= profile.maximum_definitions &&
          static_cast<std::uint32_t>(target_slot) < count) {
        const auto record64 = static_cast<std::uint64_t>(records) +
                              static_cast<std::uint64_t>(
                                  static_cast<std::uint16_t>(target_slot)) *
                                  object_record_stride;
        if (record64 <= std::numeric_limits<std::uint32_t>::max()) {
          const auto record = static_cast<std::uint32_t>(record64);
          std::uint32_t definition{};
          std::uint32_t instance{};
          std::uint16_t live_slot_bits{};
          if (validGuestRamRange(record, object_record_stride) &&
              context.read32(record, definition) &&
              definition < definition_count &&
              definition <= std::numeric_limits<std::uint16_t>::max() &&
              context.read32(record + object_instance_offset, instance) &&
              validGuestRamRange(instance, instance_slot_offset + 2U) &&
              context.read16(instance + instance_slot_offset, live_slot_bits) &&
              std::bit_cast<std::int16_t>(live_slot_bits) == target_slot) {
            const auto definition64 = static_cast<std::uint64_t>(definitions) +
                                      static_cast<std::uint64_t>(definition) *
                                          object_definition_stride;
            if (definition64 <= std::numeric_limits<std::uint32_t>::max()) {
              const auto definition_address =
                  static_cast<std::uint32_t>(definition64);
              std::uint16_t object_class{};
              std::uint32_t gameplay_frame{};
              if (validGuestRamRange(definition_address,
                                     object_definition_stride) &&
                  context.read16(definition_address, object_class) &&
                  agentCbdcFriendlyFireTarget(
                      mission_bits, static_cast<std::uint16_t>(target_slot),
                      static_cast<std::uint16_t>(definition), object_class) &&
                  context.read32(profile.gameplay_frame, gameplay_frame) &&
                  (!agent_cbdc_friendly_fire_frame_ ||
                   *agent_cbdc_friendly_fire_frame_ != gameplay_frame)) {
                agent_cbdc_friendly_fire_frame_ = gameplay_frame;
                if (agent_cbdc_friendly_fire_pending_penalties_ !=
                    std::numeric_limits<std::uint32_t>::max()) {
                  ++agent_cbdc_friendly_fire_pending_penalties_;
                }
              }
            }
          }
        }
      }
    }
    if (target_slot != player_slot || damage <= 0) {
      return;
    }

    constexpr std::uint32_t object_record_stride = 0x4cU;
    std::uint32_t records{};
    std::uint32_t count{};
    const auto tables_valid =
        context.read32(profile.object_records_pointer, records) &&
        context.read32(profile.object_count, count) &&
        count <= profile.maximum_objects &&
        validGuestRamRange(records, object_record_stride);
    struct SniperSource {
      std::int16_t slot{-1};
      std::uint32_t instance{};
      std::uint32_t ai_controller{};
      std::uint8_t weapon{};
    };
    const auto sniper_source =
        [&](std::int16_t source_slot) -> std::optional<SniperSource> {
      constexpr std::uint32_t object_weapon_offset = 0x24U;
      constexpr std::uint32_t object_instance_offset = 0x34U;
      constexpr std::uint32_t instance_slot_offset = 2U;
      constexpr std::uint32_t instance_ai_offset = 0x1cU;
      constexpr std::uint8_t svd_weapon = 12U;
      constexpr std::uint8_t sniper_weapon = 13U;
      if (!tables_valid || source_slot < 0 ||
          static_cast<std::uint32_t>(source_slot) >= count) {
        return std::nullopt;
      }
      const auto source_record64 =
          static_cast<std::uint64_t>(records) +
          static_cast<std::uint64_t>(static_cast<std::uint16_t>(source_slot)) *
              object_record_stride;
      if (source_record64 > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
      }
      const auto source_record = static_cast<std::uint32_t>(source_record64);
      std::uint32_t source_instance{};
      std::uint16_t shooter_slot_bits{};
      if (!validGuestRamRange(source_record, object_record_stride) ||
          !context.read32(source_record + object_instance_offset,
                          source_instance) ||
          !validGuestRamRange(source_instance, instance_slot_offset + 2U) ||
          !context.read16(source_instance + instance_slot_offset,
                          shooter_slot_bits)) {
        return std::nullopt;
      }
      const auto shooter_slot = std::bit_cast<std::int16_t>(shooter_slot_bits);
      if (shooter_slot < 0 || shooter_slot == player_slot ||
          static_cast<std::uint32_t>(shooter_slot) >= count) {
        return std::nullopt;
      }
      const auto shooter_record64 =
          static_cast<std::uint64_t>(records) +
          static_cast<std::uint64_t>(static_cast<std::uint16_t>(shooter_slot)) *
              object_record_stride;
      if (shooter_record64 > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
      }
      const auto shooter_record = static_cast<std::uint32_t>(shooter_record64);
      std::uint8_t weapon{};
      std::uint32_t shooter_instance{};
      std::uint32_t shooter_ai{};
      std::uint16_t identity_slot_bits{};
      if (!validGuestRamRange(shooter_record, object_record_stride) ||
          !context.read8(shooter_record + object_weapon_offset, weapon) ||
          (weapon != svd_weapon && weapon != sniper_weapon) ||
          !context.read32(shooter_record + object_instance_offset,
                          shooter_instance) ||
          !validGuestRamRange(shooter_instance, instance_ai_offset + 4U) ||
          !context.read16(shooter_instance + instance_slot_offset,
                          identity_slot_bits) ||
          std::bit_cast<std::int16_t>(identity_slot_bits) != shooter_slot ||
          !context.read32(shooter_instance + instance_ai_offset, shooter_ai) ||
          shooter_ai == 0U) {
        return std::nullopt;
      }
      return SniperSource{shooter_slot, shooter_instance, shooter_ai, weapon};
    };
    const auto matches_active = [&](const SniperSource &candidate) {
      return candidate.slot == agent_headshot_shooter_slot_ &&
             candidate.instance == agent_headshot_shooter_instance_ &&
             candidate.ai_controller == agent_headshot_shooter_ai_controller_ &&
             candidate.weapon == agent_headshot_weapon_;
    };

    auto shooter = sniper_source(attacker_slot);
    std::uint32_t gameplay_frame{};
    const auto damage_type = std::bit_cast<std::int16_t>(
        static_cast<std::uint16_t>(context.argument(4U)));
    constexpr std::int16_t direct_sniper_damage_type = 0x000f;
    constexpr std::int16_t weapon_collision_damage_type = 0x0892;
    const auto ballistic_sniper_hit =
        damage_type == direct_sniper_damage_type ||
        damage_type == weapon_collision_damage_type;
    if (ballistic_sniper_hit && (!shooter || !matches_active(*shooter))) {
      if (const auto owner = sniper_source(owner_slot);
          owner && matches_active(*owner)) {
        shooter = owner;
      }
    }
    if (ballistic_sniper_hit && shooter && matches_active(*shooter) &&
        context.read32(profile.gameplay_frame, gameplay_frame) &&
        guestFrameReached(gameplay_frame, agent_headshot_ready_frame_)) {
      context.setRegister(
          a3_register, guestArgument(std::numeric_limits<std::int16_t>::max()));
      if (shooter->slot >= 0 && static_cast<std::size_t>(shooter->slot) <
                                    agent_headshot_engagements_.size()) {
        auto &engagement = agent_headshot_engagements_[static_cast<std::size_t>(
            shooter->slot)];
        if (engagement.instance == shooter->instance &&
            engagement.ai_controller == shooter->ai_controller &&
            engagement.weapon == shooter->weapon) {
          engagement.consumed = true;
        }
      }
      agent_headshot_shooter_slot_ = -1;
      agent_headshot_shooter_instance_ = 0U;
      agent_headshot_shooter_ai_controller_ = 0U;
      agent_headshot_weapon_ = 0U;
      agent_headshot_ready_frame_ = 0U;
      return;
    }

    const auto increased = static_cast<std::int32_t>(damage) +
                           (static_cast<std::int32_t>(damage) + 3) / 4;
    const auto saturated = std::min(
        increased,
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max()));
    context.setRegister(a3_register,
                        guestArgument(static_cast<std::int16_t>(saturated)));
  });
}

void LegacyGameplayVm::bindSyphonFilterUsaV11AgentMissionNpcSpawnHook(
    const LegacyNativeMissionBridgeProfile &profile) {
  constexpr std::uint32_t spawn_attributes_boundary = 0x8005f468U;
  constexpr std::uint32_t spawn_attributes_instruction = 0xac430024U;
  constexpr std::uint32_t mission_index_address = 0x80130c88U;
  constexpr std::uint32_t object_record_stride = 0x4cU;
  bindHostCall(spawn_attributes_boundary, [this, profile](
                                              LegacyHostCallContext &context) {
    context.continueGuestInstruction();
    std::uint32_t instruction{};
    std::uint16_t mission_index{};
    if (!agent_difficulty_ || !context.read32(context.pc(), instruction) ||
        instruction != spawn_attributes_instruction ||
        !context.read16(mission_index_address, mission_index)) {
      return;
    }

    enum class SpawnRuleKind : std::uint8_t {
      kravitch,
      marcos,
      gabrek,
      chapel_guard,
    };
    struct SpawnRule {
      std::uint32_t mission;
      std::uint32_t slot;
      std::uint32_t definition;
      std::optional<LegacyNativePoint> authored_position;
      SpawnRuleKind kind;
      std::uint16_t retail_attributes{};
    };
    std::optional<SpawnRule> rule;
    const auto slot = context.registerValue(16U); // s0
    if (mission_index == 0U && slot == 174U) {
      // Kravitch's live record is transformed during mission bootstrap, so its
      // coordinates no longer match the authored source values here. Mission,
      // slot, definition and the exact record pointer uniquely identify him.
      rule = SpawnRule{0U, 174U, 53U, std::nullopt, SpawnRuleKind::kravitch};
    } else if (mission_index == 3U && slot == 48U) {
      rule = SpawnRule{3U, 48U, 11U, LegacyNativePoint{5802, 0, 15845},
                       SpawnRuleKind::marcos};
    } else if (mission_index == agent_gabrek_identity.mission &&
               slot == agent_gabrek_identity.slot) {
      rule = SpawnRule{agent_gabrek_identity.mission,
                       agent_gabrek_identity.slot,
                       agent_gabrek_identity.definition,
                       LegacyNativePoint{agent_gabrek_identity.authored_x,
                                         agent_gabrek_identity.authored_y,
                                         agent_gabrek_identity.authored_z},
                       SpawnRuleKind::gabrek,
                       agent_gabrek_identity.retail_attributes};
    } else if (mission_index == 12U) {
      for (const auto &identity : agent_chapel_guard_identities) {
        if (slot != identity.slot) {
          continue;
        }
        rule = SpawnRule{identity.mission,
                         identity.slot,
                         identity.definition,
                         LegacyNativePoint{identity.authored_x,
                                           identity.authored_y,
                                           identity.authored_z},
                         SpawnRuleKind::chapel_guard,
                         identity.retail_attributes};
        break;
      }
    }
    if (!rule) {
      return;
    }

    std::uint32_t records{};
    std::uint32_t definition{};
    if (!context.read32(profile.object_records_pointer, records)) {
      return;
    }
    const auto expected_record64 =
        static_cast<std::uint64_t>(records) +
        static_cast<std::uint64_t>(rule->slot) * object_record_stride;
    if (expected_record64 > std::numeric_limits<std::uint32_t>::max()) {
      return;
    }
    const auto record = context.registerValue(2U); // v0
    if (record != static_cast<std::uint32_t>(expected_record64) ||
        !validGuestRamRange(record, object_record_stride) ||
        !context.read32(record, definition) || definition != rule->definition) {
      return;
    }
    if (rule->authored_position) {
      std::uint32_t authored_x{};
      std::uint32_t authored_y{};
      std::uint32_t authored_z{};
      if (!context.read32(record + 0x18U, authored_x) ||
          !context.read32(record + 0x1cU, authored_y) ||
          !context.read32(record + 0x20U, authored_z) ||
          std::bit_cast<std::int32_t>(authored_x) !=
              rule->authored_position->x ||
          std::bit_cast<std::int32_t>(authored_y) !=
              rule->authored_position->y ||
          std::bit_cast<std::int32_t>(authored_z) !=
              rule->authored_position->z) {
        return;
      }
    }

    const auto attributes =
        static_cast<std::uint16_t>(context.registerValue(3U)); // v1
    auto adjusted = attributes;
    switch (rule->kind) {
    case SpawnRuleKind::kravitch:
      adjusted = agentKravitchAttributes(attributes, true);
      break;
    case SpawnRuleKind::marcos:
      adjusted = agentMarcosAttributes(attributes, true);
      break;
    case SpawnRuleKind::gabrek:
      adjusted = agentGabrekAttributes(attributes, true);
      break;
    case SpawnRuleKind::chapel_guard:
      adjusted =
          agentChapelGuardAttributes(attributes, rule->retail_attributes, true);
      break;
    }
    context.setRegister(3U, adjusted);
  });

  constexpr std::uint32_t npc_init_entry = 0x8005805cU;
  constexpr std::uint32_t npc_init_entry_instruction = 0x27bdffc8U;
  constexpr std::uint32_t npc_init_entry_next_instruction = 0xafb1002cU;
  bindHostCall(npc_init_entry, [this, profile](
                                             LegacyHostCallContext &context) {
    // Patch the exact record before FUN_8005805c reads and caches its weapon
    // descriptor, then retire the original retail prologue instruction.
    context.continueGuestInstruction();

    constexpr std::uint32_t kravitch_slot = 174U;
    constexpr std::uint32_t kravitch_definition = 53U;
    std::uint32_t boundary_word{};
    std::uint32_t delay_word{};
    std::uint16_t mission_index{};
    std::uint16_t live_slot{};
    std::uint32_t records{};
    std::uint32_t count{};
    const auto instance = context.argument(0U); // a0
    if (!agent_difficulty_ ||
        !context.read32(npc_init_entry, boundary_word) ||
        boundary_word != npc_init_entry_instruction ||
        !context.read32(npc_init_entry + 4U, delay_word) ||
        delay_word != npc_init_entry_next_instruction ||
        !context.read16(profile.mission_index, mission_index) ||
        mission_index != 0U || !validGuestRamRange(instance, 4U) ||
        !context.read16(instance + 2U, live_slot) ||
        live_slot != kravitch_slot ||
        !context.read32(profile.object_records_pointer, records) ||
        !context.read32(profile.object_count, count) ||
        count <= kravitch_slot || count > profile.maximum_objects) {
      return;
    }

    const auto record64 = static_cast<std::uint64_t>(records) +
                          static_cast<std::uint64_t>(kravitch_slot) *
                              object_record_stride;
    if (record64 > std::numeric_limits<std::uint32_t>::max()) {
      return;
    }
    const auto record = static_cast<std::uint32_t>(record64);
    std::uint32_t definition{};
    std::uint16_t attributes{};
    std::uint32_t owner{};
    if (!validGuestRamRange(record, object_record_stride) ||
        !context.read32(record, definition) ||
        definition != kravitch_definition ||
        !context.read16(record + 0x24U, attributes) ||
        (attributes != 0xc102U && attributes != 0xc107U &&
         attributes != 0xc109U) ||
        !context.read32(record + 0x34U, owner) || owner != instance) {
      return;
    }

    const auto adjusted = agentKravitchAttributes(attributes, true);
    if (adjusted != attributes) {
      static_cast<void>(context.write16(record + 0x24U, adjusted));
    }
  });
}

void LegacyGameplayVm::bindSyphonFilterUsaV11AgentAramovSpeedHook() {
  constexpr std::uint32_t locomotion_boundary = 0x8007157cU;
  constexpr std::uint32_t locomotion_instruction = 0x0c01bf12U;
  constexpr std::size_t actor_register = 16U; // s0
  constexpr std::uint32_t mission_index_address = 0x80130c88U;
  constexpr std::uint32_t object_records_pointer = 0x80115cccU;
  constexpr std::uint32_t object_count_address = 0x80116a5cU;
  constexpr std::uint32_t object_definitions_pointer = 0x80116b98U;
  constexpr std::uint32_t definition_count_address = 0x80116b14U;
  constexpr std::uint32_t object_record_stride = 0x4cU;
  constexpr std::uint32_t object_definition_stride = 0x14U;
  constexpr std::uint32_t aramov_slot = 13U;
  constexpr std::uint32_t aramov_definition = 6U;
  constexpr std::uint16_t aramov_class = 2U;

  bindHostCall(locomotion_boundary, [this](LegacyHostCallContext &context) {
    // FUN_80071384 has merged animation root motion and scripted locomotion.
    // Scale Mara's final horizontal delta before retail collision integration.
    context.continueGuestInstruction();
    std::uint32_t instruction{};
    std::uint16_t mission_index{};
    if (!agent_difficulty_ || !context.read32(context.pc(), instruction) ||
        instruction != locomotion_instruction ||
        !context.read16(mission_index_address, mission_index) ||
        mission_index != 2U) {
      return;
    }

    const auto actor = context.registerValue(actor_register);
    std::uint16_t instance_slot{};
    std::uint32_t records{};
    std::uint32_t count{};
    std::uint32_t definitions{};
    std::uint32_t definition_count{};
    if (actor == 0U || !validGuestRamRange(actor, 0x10U) ||
        !context.read16(actor + 2U, instance_slot) ||
        instance_slot != aramov_slot ||
        !context.read32(object_records_pointer, records) ||
        !context.read32(object_count_address, count) || count <= aramov_slot ||
        count > 2048U ||
        !context.read32(object_definitions_pointer, definitions) ||
        !context.read32(definition_count_address, definition_count) ||
        definition_count <= aramov_definition || definition_count > 1024U) {
      return;
    }

    const auto record64 = static_cast<std::uint64_t>(records) +
                          aramov_slot * object_record_stride;
    const auto definition64 = static_cast<std::uint64_t>(definitions) +
                              aramov_definition * object_definition_stride;
    if (record64 > std::numeric_limits<std::uint32_t>::max() ||
        definition64 > std::numeric_limits<std::uint32_t>::max()) {
      return;
    }
    const auto record = static_cast<std::uint32_t>(record64);
    const auto definition_address = static_cast<std::uint32_t>(definition64);
    std::uint32_t definition{};
    std::uint32_t record_instance{};
    std::uint32_t authored_x{};
    std::uint32_t authored_y{};
    std::uint32_t authored_z{};
    std::uint32_t motion{};
    std::uint32_t delta_x{};
    std::uint32_t delta_z{};
    std::uint16_t attributes{};
    std::uint16_t class_id{};
    if (!validGuestRamRange(record, object_record_stride) ||
        !context.read32(record, definition) ||
        definition != aramov_definition ||
        !context.read32(record + 0x18U, authored_x) ||
        !context.read32(record + 0x1cU, authored_y) ||
        !context.read32(record + 0x20U, authored_z) ||
        std::bit_cast<std::int32_t>(authored_x) != -2749 ||
        std::bit_cast<std::int32_t>(authored_y) != 10 ||
        std::bit_cast<std::int32_t>(authored_z) != 9958 ||
        !context.read16(record + 0x24U, attributes) ||
        (attributes & 0x00ffU) != 2U ||
        !context.read32(record + 0x34U, record_instance) ||
        record_instance != actor ||
        !validGuestRamRange(definition_address, object_definition_stride) ||
        !context.read16(definition_address, class_id) ||
        class_id != aramov_class || !context.read32(actor + 0x0cU, motion) ||
        !validGuestRamRange(motion, 0x5cU) ||
        !context.read32(motion + 0x50U, delta_x) ||
        !context.read32(motion + 0x58U, delta_z)) {
      return;
    }

    const auto adjusted_x =
        agentAramovRootMotionDelta(std::bit_cast<std::int32_t>(delta_x));
    const auto adjusted_z =
        agentAramovRootMotionDelta(std::bit_cast<std::int32_t>(delta_z));
    if (!context.write32(motion + 0x50U,
                         std::bit_cast<std::uint32_t>(adjusted_x))) {
      return;
    }
    if (!context.write32(motion + 0x58U,
                         std::bit_cast<std::uint32_t>(adjusted_z))) {
      // Keep the horizontal pair coherent if the second guest write fails.
      if (!context.write32(motion + 0x50U, delta_x)) {
        return;
      }
    }
  });
}

void LegacyGameplayVm::bindSyphonFilterUsaV11AgentEnemyAimHooks(
    const LegacyAgentEnemyAimProfile &profile) {
  if (profile.agent_accuracy_boundary != 0U) {
    bindHostCall(
        profile.agent_accuracy_boundary,
        [this, profile](LegacyHostCallContext &context) {
          // Hard's branch has already added six to a0. Agent adds a
          // small extra bias before retail multiplies the same aim
          // coefficient; the original mult still executes.
          context.continueGuestInstruction();
          std::uint32_t instruction{};
          if (!agent_difficulty_ || profile.agent_accuracy_bonus <= 0 ||
              !context.read32(context.pc(), instruction) ||
              instruction != profile.agent_accuracy_instruction) {
            return;
          }
          const auto bonus =
              static_cast<std::uint32_t>(profile.agent_accuracy_bonus);
          const auto coefficient = context.argument(0U);
          if (coefficient > std::numeric_limits<std::uint32_t>::max() - bonus) {
            return;
          }
          context.setRegister(4U, coefficient + bonus);
        });
  }

  if (profile.agent_target_memory_boundary != 0U) {
    bindHostCall(
        profile.agent_target_memory_boundary,
        [this, profile](LegacyHostCallContext &context) {
          // The original SLTIU still executes. For the Agent-only extension,
          // values in [retail, Agent) are moved just inside retail's window.
          context.continueGuestInstruction();
          constexpr std::size_t age_register = 2U;    // v0
          constexpr std::size_t actor_register = 16U; // s0
          constexpr std::uint32_t actor_ai_controller_offset = 0x1cU;
          constexpr std::uint32_t ai_archetype_offset = 0x47U;
          std::uint32_t instruction{};
          if (!agent_difficulty_ || profile.retail_target_memory_frames == 0U ||
              profile.agent_target_memory_frames <=
                  profile.retail_target_memory_frames ||
              !context.read32(context.pc(), instruction) ||
              instruction != profile.agent_target_memory_instruction) {
            return;
          }

          const auto actor = context.registerValue(actor_register);
          std::uint32_t target{};
          std::uint32_t player{};
          std::uint32_t ai{};
          std::uint16_t target_slot_bits{};
          std::uint16_t player_slot_bits{};
          std::uint8_t archetype{};
          if (!validGuestRamRange(actor, actor_ai_controller_offset + 4U) ||
              !context.read32(actor + profile.actor_target_controller_offset,
                              target) ||
              !context.read32(actor + actor_ai_controller_offset, ai) ||
              !validGuestRamRange(ai, ai_archetype_offset + 1U) ||
              !context.read8(ai + ai_archetype_offset, archetype) ||
              (archetype & 1U) == 0U ||
              !validGuestRamRange(target, profile.target_slot_offset + 2U) ||
              !context.read16(target + profile.target_slot_offset,
                              target_slot_bits) ||
              !context.read32(profile.player_pointer, player) ||
              !validGuestRamRange(player, profile.instance_slot_offset + 2U) ||
              !context.read16(player + profile.instance_slot_offset,
                              player_slot_bits) ||
              target_slot_bits != player_slot_bits) {
            return;
          }

          std::uint16_t mission{};
          const auto mission_valid =
              context.read16(profile.mission_index, mission);
          const auto flashlight_active = [&]() {
            constexpr std::uint32_t light_node_size = 0x0cU;
            constexpr std::uint32_t light_next_offset = 8U;
            constexpr std::size_t light_capacity = 4U;
            if (!mission_valid || mission != profile.tunnel_blackout_mission ||
                profile.maximum_vertex_lights == 0U ||
                profile.maximum_vertex_lights > light_capacity) {
              return false;
            }
            std::uint32_t source_handle{};
            std::uint32_t node{};
            if (!context.read32(profile.flashlight_source, source_handle) ||
                source_handle == 0U ||
                !context.read32(profile.dynamic_light_list, node)) {
              return false;
            }
            std::array<std::uint32_t, light_capacity> seen_nodes{};
            std::array<std::uint32_t, light_capacity> seen_sources{};
            auto count = std::size_t{};
            auto found = false;
            while (node != 0U) {
              if (count >= profile.maximum_vertex_lights || (node & 3U) != 0U ||
                  !validGuestRamRange(node, light_node_size)) {
                return false;
              }
              for (std::size_t previous = 0U; previous < count; ++previous) {
                if (seen_nodes[previous] == node) {
                  return false;
                }
              }
              std::uint32_t source{};
              std::uint32_t next{};
              std::uint32_t backlink{};
              if (!context.read32(node, source) ||
                  !context.read32(node + light_next_offset, next) ||
                  (source & 3U) != 0U || !validGuestRamRange(source, 4U) ||
                  !context.read32(source, backlink) || backlink != node) {
                return false;
              }
              for (std::size_t previous = 0U; previous < count; ++previous) {
                if (seen_sources[previous] == source) {
                  return false;
                }
              }
              seen_nodes[count] = node;
              seen_sources[count] = source;
              ++count;
              found = found || (source == profile.flashlight_source &&
                                source_handle == node);
              node = next;
            }
            return found;
          }();
          const auto memory_frames = agentEnemyTargetMemoryFrames(
              mission, true, flashlight_active,
              profile.retail_target_memory_frames,
              profile.agent_target_memory_frames,
              profile.flashlight_target_memory_frames);
          const auto age = context.registerValue(age_register);
          if (age >= profile.retail_target_memory_frames &&
              age < memory_frames) {
            context.setRegister(age_register,
                                profile.retail_target_memory_frames - 1U);
          }
        });
  }

  if (profile.kravitch_post_shot_boundary != 0U) {
    bindHostCall(
        profile.kravitch_post_shot_boundary,
        [this, profile](LegacyHostCallContext &context) {
          // Keep FUN_800630c0's SB and all retail firing/LOS decisions. Only
          // shorten the pause it already produced for the exact live Agent
          // Kravitch, then make the next retail route decision eligible.
          context.continueGuestInstruction();
          constexpr std::size_t cooldown_register = 3U; // v1
          constexpr std::size_t instance_register = 16U; // s0
          constexpr std::size_t weapon_register = 17U;   // s1
          constexpr std::size_t ai_register = 18U;       // s2
          constexpr std::uint16_t kravitch_slot = 174U;
          constexpr std::uint32_t kravitch_definition = 53U;
          constexpr std::uint8_t shotgun = 7U;
          constexpr std::uint32_t object_record_stride = 0x4cU;
          constexpr std::uint32_t object_definition_stride = 0x14U;
          constexpr std::uint32_t instance_slot_offset = 0x02U;
          constexpr std::uint32_t instance_target_offset = 0x14U;
          constexpr std::uint32_t instance_health_offset = 0x18U;
          constexpr std::uint32_t instance_ai_offset = 0x1cU;
          constexpr std::uint32_t health_value_offset = 0x08U;
          constexpr std::uint32_t target_slot_offset = 0U;
          constexpr std::uint32_t target_flags_offset = 0x04U;
          constexpr std::uint32_t target_invalid_flag = 0x04U;
          constexpr std::uint32_t ai_decision_counter_offset = 0x4aU;
          constexpr std::uint32_t ai_combat_mode_offset = 0x48U;
          constexpr std::uint32_t record_attributes_offset = 0x24U;
          constexpr std::uint32_t record_owner_offset = 0x34U;

          std::uint32_t instruction{};
          std::uint16_t mission{};
          if (!agent_difficulty_ ||
              !context.read32(context.pc(), instruction) ||
              instruction != profile.kravitch_post_shot_instruction ||
              !context.read16(profile.mission_index, mission) || mission != 0U ||
              context.registerValue(weapon_register) != shotgun) {
            return;
          }

          const auto instance = context.registerValue(instance_register);
          const auto ai = context.registerValue(ai_register);
          std::uint16_t instance_slot{};
          std::uint32_t instance_target{};
          std::uint32_t health{};
          std::uint32_t instance_ai{};
          std::uint16_t health_bits{};
          std::uint8_t combat_mode{};
          if (!validGuestRamRange(instance, instance_ai_offset + 4U) ||
              !context.read16(instance + instance_slot_offset, instance_slot) ||
              instance_slot != kravitch_slot ||
              !context.read32(instance + instance_target_offset,
                              instance_target) ||
              !context.read32(instance + instance_health_offset, health) ||
              !context.read32(instance + instance_ai_offset, instance_ai) ||
              instance_ai != ai ||
              !validGuestRamRange(health, health_value_offset + 2U) ||
              !context.read16(health + health_value_offset, health_bits) ||
              std::bit_cast<std::int16_t>(health_bits) <= 0 ||
              !validGuestRamRange(ai, ai_decision_counter_offset + 1U) ||
              !context.read8(ai + ai_combat_mode_offset, combat_mode) ||
              combat_mode != 2U) {
            return;
          }

          std::uint32_t player{};
          std::uint16_t player_slot{};
          std::uint16_t target_slot{};
          std::uint32_t target_flags{};
          if (!validGuestRamRange(instance_target, target_flags_offset + 4U) ||
              !context.read16(instance_target + target_slot_offset,
                              target_slot) ||
              !context.read32(instance_target + target_flags_offset,
                              target_flags) ||
              (target_flags & target_invalid_flag) != 0U ||
              !context.read32(profile.player_pointer, player) ||
              !validGuestRamRange(player, instance_slot_offset + 2U) ||
              !context.read16(player + instance_slot_offset, player_slot) ||
              target_slot != player_slot) {
            return;
          }

          std::uint32_t records{};
          std::uint32_t count{};
          std::uint32_t definitions{};
          std::uint32_t definition_count{};
          if (!context.read32(profile.object_records_pointer, records) ||
              !context.read32(profile.object_count, count) ||
              !context.read32(profile.object_definitions_pointer,
                              definitions) ||
              !context.read32(profile.object_definition_count,
                              definition_count) ||
              count <= kravitch_slot || count > profile.maximum_objects ||
              definition_count <= kravitch_definition ||
              definition_count > profile.maximum_definitions) {
            return;
          }
          const auto record64 = static_cast<std::uint64_t>(records) +
                                static_cast<std::uint64_t>(kravitch_slot) *
                                    object_record_stride;
          const auto definition64 =
              static_cast<std::uint64_t>(definitions) +
              static_cast<std::uint64_t>(kravitch_definition) *
                  object_definition_stride;
          if (record64 > std::numeric_limits<std::uint32_t>::max() ||
              definition64 > std::numeric_limits<std::uint32_t>::max()) {
            return;
          }
          const auto record = static_cast<std::uint32_t>(record64);
          const auto definition_address =
              static_cast<std::uint32_t>(definition64);
          std::uint32_t definition{};
          std::uint16_t attributes{};
          std::uint32_t owner{};
          std::uint16_t object_class{};
          std::uint32_t handler{};
          if (!validGuestRamRange(record, object_record_stride) ||
              !context.read32(record, definition) ||
              definition != kravitch_definition ||
              !context.read16(record + record_attributes_offset, attributes) ||
              attributes != 0xc107U ||
              !context.read32(record + record_owner_offset, owner) ||
              owner != instance ||
              !validGuestRamRange(definition_address,
                                  object_definition_stride) ||
              !context.read16(definition_address, object_class) ||
              object_class != 1U ||
              !context.read32(profile.object_handler_table + 4U, handler) ||
              handler != profile.common_npc_handler) {
            return;
          }

          const auto retail_value = context.registerValue(cooldown_register);
          const auto retail_cooldown =
              static_cast<std::uint8_t>(retail_value & 0xffU);
          const auto cooldown =
              agentKravitchPostShotCooldown(retail_cooldown, true);
          context.setRegister(cooldown_register,
                              (retail_value & 0xffffff00U) | cooldown);

          std::uint8_t decision_counter{};
          if (!context.read8(ai + ai_decision_counter_offset,
                             decision_counter)) {
            return;
          }
          const auto primed =
              agentKravitchPostShotDecisionCounter(decision_counter, true);
          if (primed != decision_counter) {
            static_cast<void>(context.write8(ai + ai_decision_counter_offset,
                                             primed));
          }
        });
  }
}

void LegacyGameplayVm::bindSyphonFilterUsaV11AgentGrenadeAwarenessHook(
    const LegacyAgentGrenadeAwarenessProfile &profile) {
  for (const auto boundary : profile.boundaries) {
    bindHostCall(boundary, [this, profile](LegacyHostCallContext &context) {
      // Preserve both retail functions wholesale. In the extended Agent-only
      // band, feed their original SLTI a value just inside the 0xa00 threshold;
      // guest code then alerts the NPC and owns route choice and locomotion.
      context.continueGuestInstruction();
      std::uint32_t instruction{};
      if (!agent_difficulty_ || profile.retail_distance <= 0 ||
          profile.agent_distance <= profile.retail_distance ||
          !context.read32(context.pc(), instruction) ||
          instruction != profile.instruction) {
        return;
      }

      const auto distance =
          std::bit_cast<std::int32_t>(context.registerValue(2U));
      if (distance >= profile.retail_distance &&
          distance < profile.agent_distance) {
        context.setRegister(
            2U, static_cast<std::uint32_t>(profile.retail_distance - 1));
      }
    });
  }

  if (profile.route_return_boundary == 0U) {
    return;
  }
  bindHostCall(profile.route_return_boundary, [this,
                                               profile](LegacyHostCallContext
                                                            &context) {
    // Retail chooses the first route edge by distance to a 0x780 ring
    // around a player/grenade midpoint. The wider Agent alert band makes
    // that heuristic visibly pull distant enemies toward the live grenade.
    // Keep every retail controller and animation, but reject a first edge
    // unless it has a positive away component and increases XZ distance.
    context.continueGuestInstruction();
    constexpr std::size_t return_value_register = 2U;
    constexpr std::size_t target_point_register = 20U;  // s4
    constexpr std::size_t standoff_register = 21U;      // s5
    constexpr std::size_t controller_register = 22U;    // s6
    constexpr std::size_t current_route_register = 23U; // s7
    constexpr std::size_t stack_register = 29U;
    constexpr std::size_t return_address_register = 31U;
    constexpr std::uint8_t player_grenade_danger = 1U;
    constexpr std::uint32_t route_actor_stack_offset = 0x58U;
    constexpr std::uint32_t route_table_stack_offset = 0x60U;
    constexpr std::uint32_t controller_route_offset = 0x43U;
    static constexpr std::uint32_t route_stride = 0x0cU;
    static constexpr std::uint32_t route_flags_offset = 0x06U;
    static constexpr std::uint32_t route_neighbours_offset = 0x08U;
    constexpr std::size_t route_neighbour_count = 3U;

    std::uint32_t instruction{};
    std::uint8_t danger_mask{};
    if (!agent_difficulty_ || !context.read32(context.pc(), instruction) ||
        instruction != profile.route_return_instruction ||
        !context.read8(profile.danger_mask, danger_mask) ||
        !validGuestRamRange(context.registerValue(stack_register),
                            route_table_stack_offset + 4U)) {
      return;
    }

    const auto stack = context.registerValue(stack_register);
    const auto controller = context.registerValue(controller_register);
    std::uint32_t route_table{};
    std::uint8_t current_route{};
    std::uint8_t previous_route{};
    if (!validGuestRamRange(stack, route_table_stack_offset + 4U) ||
        !validGuestRamRange(controller, controller_route_offset + 2U) ||
        !context.read32(stack + route_table_stack_offset, route_table) ||
        !context.read8(controller + controller_route_offset, current_route) ||
        !context.read8(controller + controller_route_offset + 1U,
                       previous_route)) {
      return;
    }

    struct RouteNode {
      std::int32_t x{};
      std::int32_t z{};
      std::uint16_t flags{};
      std::array<std::int8_t, route_neighbour_count> neighbours{};
    };
    const auto read_route_node =
        [&](std::uint8_t index) -> std::optional<RouteNode> {
      const auto address64 = static_cast<std::uint64_t>(route_table) +
                             static_cast<std::uint64_t>(index) * route_stride;
      if (address64 > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
      }
      const auto address = static_cast<std::uint32_t>(address64);
      if (!validGuestRamRange(address, route_stride)) {
        return std::nullopt;
      }

      std::uint16_t x_bits{};
      std::uint16_t z_bits{};
      RouteNode node;
      if (!context.read16(address, x_bits) ||
          !context.read16(address + 4U, z_bits) ||
          !context.read16(address + route_flags_offset, node.flags)) {
        return std::nullopt;
      }
      node.x = std::bit_cast<std::int16_t>(x_bits);
      node.z = std::bit_cast<std::int16_t>(z_bits);
      for (std::size_t neighbour = 0U; neighbour < node.neighbours.size();
           ++neighbour) {
        std::uint8_t bits{};
        if (!context.read8(address + route_neighbours_offset +
                               static_cast<std::uint32_t>(neighbour),
                           bits)) {
          return std::nullopt;
        }
        node.neighbours[neighbour] = std::bit_cast<std::int8_t>(bits);
      }
      return node;
    };

    const auto current = read_route_node(current_route);
    if (!current) {
      return;
    }

    // A live grenade always owns this decision. Preserve the earlier
    // Agent safety correction before considering weapon spacing/roles.
    if ((danger_mask & player_grenade_danger) != 0U) {
      constexpr std::uint32_t projectile_position_offset = 0x0cU;
      constexpr std::uint32_t projectile_live_offset = 0U;
      constexpr std::uint16_t route_type_mask = 0x0f00U;
      constexpr std::uint16_t blocked_route_type = 1U;
      constexpr std::uint16_t disabled_route_type = 7U;
      if (profile.retail_standoff_distance <= 0 ||
          context.registerValue(standoff_register) !=
              static_cast<std::uint32_t>(profile.retail_standoff_distance)) {
        return;
      }

      std::uint32_t projectile{};
      std::uint32_t grenade_x_bits{};
      std::uint32_t grenade_z_bits{};
      std::uint8_t projectile_live{};
      if (!context.read32(profile.player_projectile_pointer, projectile) ||
          !validGuestRamRange(projectile, projectile_position_offset + 12U) ||
          !context.read8(projectile + projectile_live_offset,
                         projectile_live) ||
          !context.read32(projectile + projectile_position_offset,
                          grenade_x_bits) ||
          !context.read32(projectile + projectile_position_offset + 8U,
                          grenade_z_bits) ||
          projectile_live != 0U) {
        return;
      }

      const auto grenade_x = std::bit_cast<std::int32_t>(grenade_x_bits);
      const auto grenade_z = std::bit_cast<std::int32_t>(grenade_z_bits);
      const auto distance_squared = [&](const RouteNode &node) {
        const auto dx = static_cast<std::int64_t>(node.x) - grenade_x;
        const auto dz = static_cast<std::int64_t>(node.z) - grenade_z;
        return static_cast<std::uint64_t>(dx * dx) +
               static_cast<std::uint64_t>(dz * dz);
      };
      const auto away_score = [&](const RouteNode &node) {
        const auto away_x = static_cast<std::int64_t>(current->x) - grenade_x;
        const auto away_z = static_cast<std::int64_t>(current->z) - grenade_z;
        const auto step_x = static_cast<std::int64_t>(node.x) - current->x;
        const auto step_z = static_cast<std::int64_t>(node.z) - current->z;
        return away_x * step_x + away_z * step_z;
      };
      const auto current_distance = distance_squared(*current);
      const auto current_type =
          static_cast<std::uint16_t>((current->flags & route_type_mask) >> 8U);
      const auto route_allowed = [&](const RouteNode &node) {
        const auto type =
            static_cast<std::uint16_t>((node.flags & route_type_mask) >> 8U);
        return type != blocked_route_type &&
               (current_type != disabled_route_type ||
                type != disabled_route_type);
      };
      const auto safe_route = [&](std::uint8_t route) {
        if (route == current_route) {
          return true;
        }
        const auto node = read_route_node(route);
        return node && route_allowed(*node) && away_score(*node) >= 0 &&
               distance_squared(*node) > current_distance;
      };

      const auto selected = context.registerValue(return_value_register);
      if (selected <= std::numeric_limits<std::uint8_t>::max() &&
          selected != current_route &&
          safe_route(static_cast<std::uint8_t>(selected))) {
        return;
      }

      auto best_route = current_route;
      auto best_distance = current_distance;
      auto best_score = std::int64_t{};
      for (const auto neighbour : current->neighbours) {
        if (neighbour < 0) {
          continue;
        }
        const auto candidate_route = static_cast<std::uint8_t>(neighbour);
        const auto candidate = read_route_node(candidate_route);
        if (!candidate) {
          continue;
        }
        const auto score = away_score(*candidate);
        const auto distance = distance_squared(*candidate);
        if (candidate_route == current_route || !route_allowed(*candidate) ||
            score < 0 || distance <= current_distance) {
          continue;
        }
        const auto candidate_is_previous = candidate_route == previous_route;
        const auto best_is_previous = best_route == previous_route;
        if (best_route == current_route ||
            (!candidate_is_previous && best_is_previous) ||
            (candidate_is_previous == best_is_previous &&
             (distance > best_distance ||
              (distance == best_distance && score > best_score)))) {
          best_route = candidate_route;
          best_distance = distance;
          best_score = score;
        }
      }
      context.setReturnValue(best_route);
      return;
    }

    // The second caller of this epilogue is a reversal probe. Changing
    // its return value would create false 180-degree turns.
    if (profile.route_selection_return_address == 0U ||
        context.registerValue(return_address_register) !=
            profile.route_selection_return_address ||
        context.registerValue(current_route_register) != current_route) {
      return;
    }
    const auto retail_standoff = context.registerValue(standoff_register);
    if (retail_standoff == 0U || retail_standoff == 0x03c0U ||
        retail_standoff == 0x0780U || retail_standoff == 0x7d00U) {
      return;
    }

    constexpr std::uint32_t actor_target_offset = 0x14U;
    constexpr std::uint32_t actor_health_offset = 0x18U;
    constexpr std::uint32_t actor_ai_offset = 0x1cU;
    constexpr std::uint32_t actor_slot_offset = 0x02U;
    constexpr std::uint32_t health_value_offset = 0x08U;
    constexpr std::uint32_t ai_flags_offset = 0x20U;
    constexpr std::uint32_t ai_fire_latch_offset = 0x41U;
    constexpr std::uint32_t ai_archetype_offset = 0x47U;
    constexpr std::uint32_t ai_combat_mode_offset = 0x48U;
    constexpr std::uint32_t target_slot_offset = 0U;
    constexpr std::uint32_t target_flags_offset = 0x04U;
    constexpr std::uint32_t object_record_stride = 0x4cU;
    constexpr std::uint32_t object_weapon_offset = 0x24U;
    constexpr std::uint32_t object_instance_offset = 0x34U;
    constexpr std::uint32_t object_definition_stride = 0x14U;
    constexpr std::uint32_t active_combat_flag = 0x00000200U;
    constexpr std::uint32_t scripted_combat_flags =
        0x00000400U | 0x00001000U | 0x00040000U | 0x00080000U | 0x20000000U;
    constexpr std::uint32_t target_invalid_flag = 0x04U;
    constexpr std::uint16_t authored_route_mask = 0x0f0fU;

    std::uint32_t actor{};
    std::uint32_t target{};
    std::uint32_t health{};
    std::uint32_t actor_ai{};
    std::uint32_t player{};
    std::uint16_t actor_slot_bits{};
    std::uint16_t player_slot_bits{};
    std::uint16_t target_slot_bits{};
    std::uint16_t health_bits{};
    std::uint32_t target_flags{};
    std::uint32_t ai_flags{};
    std::uint8_t fire_latch{};
    std::uint8_t archetype{};
    std::uint8_t combat_mode{};
    if (!context.read32(stack + route_actor_stack_offset, actor) ||
        !validGuestRamRange(actor, actor_ai_offset + 4U) ||
        !context.read16(actor + actor_slot_offset, actor_slot_bits) ||
        !context.read32(actor + actor_target_offset, target) ||
        !context.read32(actor + actor_health_offset, health) ||
        !context.read32(actor + actor_ai_offset, actor_ai) ||
        actor_ai != controller ||
        !validGuestRamRange(target, target_flags_offset + 4U) ||
        !validGuestRamRange(health, health_value_offset + 2U) ||
        !validGuestRamRange(controller, ai_combat_mode_offset + 1U) ||
        !context.read16(target + target_slot_offset, target_slot_bits) ||
        !context.read32(target + target_flags_offset, target_flags) ||
        !context.read16(health + health_value_offset, health_bits) ||
        std::bit_cast<std::int16_t>(health_bits) <= 0 ||
        !context.read32(controller + ai_flags_offset, ai_flags) ||
        !context.read8(controller + ai_fire_latch_offset, fire_latch) ||
        !context.read8(controller + ai_archetype_offset, archetype) ||
        !context.read8(controller + ai_combat_mode_offset, combat_mode) ||
        (archetype & 1U) == 0U || combat_mode != 2U ||
        (ai_flags & active_combat_flag) == 0U ||
        (ai_flags & scripted_combat_flags) != 0U ||
        (target_flags & target_invalid_flag) != 0U ||
        !context.read32(profile.player_pointer, player) ||
        !validGuestRamRange(player, actor_slot_offset + 2U) ||
        !context.read16(player + actor_slot_offset, player_slot_bits) ||
        target_slot_bits != player_slot_bits) {
      return;
    }

    const auto actor_slot = static_cast<std::uint32_t>(actor_slot_bits);
    std::uint32_t records{};
    std::uint32_t definition_count{};
    std::uint32_t definitions{};
    if (!context.read32(profile.object_records_pointer, records) ||
        !context.read32(profile.object_definition_count, definition_count) ||
        !context.read32(profile.object_definitions_pointer, definitions) ||
        records == 0U || definitions == 0U || definition_count == 0U ||
        definition_count > 4096U) {
      return;
    }
    const auto record64 =
        static_cast<std::uint64_t>(records) +
        static_cast<std::uint64_t>(actor_slot) * object_record_stride;
    if (record64 > std::numeric_limits<std::uint32_t>::max()) {
      return;
    }
    const auto record = static_cast<std::uint32_t>(record64);
    std::uint32_t definition{};
    std::uint32_t record_instance{};
    std::uint8_t weapon{};
    if (!validGuestRamRange(record, object_record_stride) ||
        !context.read32(record, definition) || definition >= definition_count ||
        !context.read8(record + object_weapon_offset, weapon) ||
        !context.read32(record + object_instance_offset, record_instance) ||
        record_instance != actor) {
      return;
    }
    const auto definition64 =
        static_cast<std::uint64_t>(definitions) +
        static_cast<std::uint64_t>(definition) * object_definition_stride;
    if (definition64 > std::numeric_limits<std::uint32_t>::max()) {
      return;
    }
    const auto definition_address = static_cast<std::uint32_t>(definition64);
    std::uint16_t class_bits{};
    if (!validGuestRamRange(definition_address, object_definition_stride) ||
        !context.read16(definition_address, class_bits)) {
      return;
    }
    const auto object_class = std::bit_cast<std::int16_t>(class_bits);
    if (object_class < 0 || object_class > 255) {
      return;
    }
    std::uint32_t handler{};
    if (!context.read32(profile.object_handler_table +
                            static_cast<std::uint32_t>(object_class) * 4U,
                        handler) ||
        handler != profile.common_npc_handler) {
      return;
    }

    const auto preferred_distance = [&]() -> std::optional<std::int32_t> {
      if (weapon >= 1U && weapon <= 5U) {
        return profile.pistol_distance;
      }
      if (weapon == 6U || weapon == 7U || weapon == 14U || weapon == 15U) {
        return profile.shotgun_distance;
      }
      if ((weapon >= 8U && weapon <= 11U) || weapon == 17U) {
        return profile.automatic_distance;
      }
      if (weapon == 12U || weapon == 13U || weapon == 16U) {
        return profile.sniper_distance;
      }
      return std::nullopt;
    }();
    if (!preferred_distance || *preferred_distance <= 0 ||
        profile.tactical_distance_band < 0 ||
        profile.tactical_minimum_improvement < 0 ||
        profile.flank_distance_tolerance < 0 ||
        profile.flank_minimum_step < 0 ||
        (current->flags & authored_route_mask) != 0U) {
      return;
    }

    const auto target_point = context.registerValue(target_point_register);
    std::uint32_t target_x_bits{};
    std::uint32_t target_z_bits{};
    if (!validGuestRamRange(target_point, 12U) ||
        !context.read32(target_point, target_x_bits) ||
        !context.read32(target_point + 8U, target_z_bits)) {
      return;
    }
    const auto target_x = std::bit_cast<std::int32_t>(target_x_bits);
    const auto target_z = std::bit_cast<std::int32_t>(target_z_bits);
    const auto route_is_direct = [&](std::uint8_t route) {
      if (route == current_route) {
        return true;
      }
      return std::ranges::find(current->neighbours,
                               std::bit_cast<std::int8_t>(route)) !=
             current->neighbours.end();
    };
    const auto tactical_node =
        [&](std::uint8_t route) -> std::optional<RouteNode> {
      if (!route_is_direct(route) ||
          (route != current_route && route == previous_route)) {
        return std::nullopt;
      }
      const auto node = read_route_node(route);
      if (!node || (node->flags & authored_route_mask) != 0U) {
        return std::nullopt;
      }
      return node;
    };
    const auto distance_to_target = [&](const RouteNode &node) {
      const auto dx = static_cast<double>(node.x) - target_x;
      const auto dz = static_cast<double>(node.z) - target_z;
      return static_cast<std::uint64_t>(std::llround(std::hypot(dx, dz)));
    };
    const auto distance_error = [&](const RouteNode &node) {
      const auto distance = distance_to_target(node);
      const auto preferred = static_cast<std::uint64_t>(*preferred_distance);
      return distance > preferred ? distance - preferred : preferred - distance;
    };

    const auto selected_bits = context.registerValue(return_value_register);
    const auto selected_route =
        selected_bits <= std::numeric_limits<std::uint8_t>::max()
            ? std::optional{static_cast<std::uint8_t>(selected_bits)}
            : std::nullopt;
    if (!selected_route) {
      return;
    }
    const auto selected_node = tactical_node(*selected_route);
    if (!selected_node) {
      return;
    }
    const auto current_error = distance_error(*current);
    const auto band =
        static_cast<std::uint64_t>(profile.tactical_distance_band);
    const auto baseline_error = distance_error(*selected_node);

    if (current_error > band) {
      auto best_route = current_route;
      auto best_error = current_error;
      for (const auto neighbour : current->neighbours) {
        if (neighbour < 0) {
          continue;
        }
        const auto candidate_route = static_cast<std::uint8_t>(neighbour);
        const auto candidate = tactical_node(candidate_route);
        if (!candidate) {
          continue;
        }
        const auto error = distance_error(*candidate);
        if (error < best_error) {
          best_route = candidate_route;
          best_error = error;
        }
      }
      const auto improvement =
          static_cast<std::uint64_t>(profile.tactical_minimum_improvement);
      if (best_error + improvement <= baseline_error) {
        context.setReturnValue(best_route);
      }
      return;
    }

    // Stable stateless roles: suppress / left flank / right flank / hold.
    // Dynamic slot reuse changes definition/path and therefore assignment.
    const auto role = static_cast<std::uint32_t>(
        (actor_slot + definition + (route_table >> 4U)) & 3U);
    if (role == 0U) {
      constexpr std::uint32_t weapon_in_range_flag = 0x00002000U;
      if ((ai_flags & weapon_in_range_flag) != 0U && fire_latch != 0U) {
        context.setReturnValue(current_route);
      }
      return;
    }
    if (role == 3U) {
      context.setReturnValue(current_route);
      return;
    }

    const auto radial_x = static_cast<std::int64_t>(target_x) - current->x;
    const auto radial_z = static_cast<std::int64_t>(target_z) - current->z;
    const auto side = role == 1U ? std::int64_t{1} : std::int64_t{-1};
    const auto allowed_error =
        baseline_error +
        static_cast<std::uint64_t>(profile.flank_distance_tolerance);
    const auto minimum_lateral =
        static_cast<std::int64_t>(profile.flank_minimum_step) *
        static_cast<std::int64_t>(
            std::max<std::uint64_t>(distance_to_target(*current), 1U));
    auto best_route = *selected_route;
    auto best_lateral = std::numeric_limits<std::int64_t>::min();
    auto best_error = std::numeric_limits<std::uint64_t>::max();
    for (const auto neighbour : current->neighbours) {
      if (neighbour < 0) {
        continue;
      }
      const auto candidate_route = static_cast<std::uint8_t>(neighbour);
      const auto candidate = tactical_node(candidate_route);
      if (!candidate) {
        continue;
      }
      const auto error = distance_error(*candidate);
      const auto step_x = static_cast<std::int64_t>(candidate->x) - current->x;
      const auto step_z = static_cast<std::int64_t>(candidate->z) - current->z;
      const auto lateral = side * (radial_x * step_z - radial_z * step_x);
      if (error > allowed_error || lateral < minimum_lateral) {
        continue;
      }
      if (lateral > best_lateral ||
          (lateral == best_lateral && error < best_error)) {
        best_route = candidate_route;
        best_lateral = lateral;
        best_error = error;
      }
    }
    if (best_lateral != std::numeric_limits<std::int64_t>::min()) {
      context.setReturnValue(best_route);
    }
  });
}

void LegacyGameplayVm::bindSyphonFilterUsaV11HostAimRayHook(
    const LegacyHostAimRayProfile &profile) {
  bindHostCall(
      profile.collision_scan_entry,
      [this, profile](LegacyHostCallContext &context) {
        // The retail caller's JAL and delay slot have already run. Observe the
        // completed descriptor at the callee entry, then pass the original
        // collision/headshot scan through unchanged.
        context.continueGuestInstruction();
        if (std::ranges::find(profile.accepted_return_addresses,
                              context.registerValue(31U)) ==
                profile.accepted_return_addresses.end() ||
            !host_aim_ray_ || profile.ray_length <= 0) {
          return;
        }

        constexpr std::uint32_t guest_ram_begin = 0x80000000U;
        constexpr std::uint32_t guest_ram_end = 0x80200000U;
        constexpr std::uint32_t vector_size = 3U * sizeof(std::uint32_t);
        const auto valid_word = [](std::uint32_t address) {
          return (address & 3U) == 0U && address >= guest_ram_begin &&
                 address <= guest_ram_end - sizeof(std::uint32_t);
        };
        const auto valid_vector = [](std::uint32_t address) {
          return (address & 3U) == 0U && address >= guest_ram_begin &&
                 address <= guest_ram_end - vector_size;
        };
        const auto descriptor = context.argument(0);
        if (!valid_word(descriptor)) {
          return;
        }
        const auto descriptor_field =
            [descriptor](std::uint32_t offset) -> std::optional<std::uint32_t> {
          if ((offset & 3U) != 0U ||
              offset > guest_ram_end - descriptor - sizeof(std::uint32_t)) {
            return std::nullopt;
          }
          return descriptor + offset;
        };
        const auto origin_field =
            descriptor_field(profile.origin_pointer_offset);
        const auto endpoint_field =
            descriptor_field(profile.endpoint_pointer_offset);
        if (!origin_field || !endpoint_field) {
          return;
        }
        std::uint32_t origin_address{};
        std::uint32_t endpoint_address{};
        if (!context.read32(*origin_field, origin_address) ||
            !context.read32(*endpoint_field, endpoint_address) ||
            !valid_vector(origin_address) || !valid_vector(endpoint_address)) {
          return;
        }
        const auto origin_end = static_cast<std::uint64_t>(origin_address) +
                                static_cast<std::uint64_t>(vector_size);
        const auto endpoint_end = static_cast<std::uint64_t>(endpoint_address) +
                                  static_cast<std::uint64_t>(vector_size);
        if (static_cast<std::uint64_t>(origin_address) < endpoint_end &&
            static_cast<std::uint64_t>(endpoint_address) < origin_end) {
          return;
        }

        const auto &ray = *host_aim_ray_;
        const auto direction_length =
            std::hypot(ray.direction_x, ray.direction_y, ray.direction_z);
        if (!std::isfinite(ray.origin_x) || !std::isfinite(ray.origin_y) ||
            !std::isfinite(ray.origin_z) || !std::isfinite(direction_length) ||
            direction_length <= 0.000001) {
          return;
        }
        const auto guest_word =
            [](double value) -> std::optional<std::uint32_t> {
          if (!std::isfinite(value) ||
              value < static_cast<double>(
                          std::numeric_limits<std::int32_t>::min()) ||
              value > static_cast<double>(
                          std::numeric_limits<std::int32_t>::max())) {
            return std::nullopt;
          }
          return std::bit_cast<std::uint32_t>(
              static_cast<std::int32_t>(std::lround(value)));
        };
        const auto scale =
            static_cast<double>(profile.ray_length) / direction_length;
        const std::array guest_values{
            guest_word(ray.origin_x),
            guest_word(-ray.origin_y),
            guest_word(ray.origin_z),
            guest_word(ray.origin_x + ray.direction_x * scale),
            guest_word(-ray.origin_y - ray.direction_y * scale),
            guest_word(ray.origin_z + ray.direction_z * scale),
        };
        if (std::ranges::any_of(guest_values,
                                [](const auto &value) { return !value; })) {
          return;
        }
        const auto patched =
            context.write32(origin_address, *guest_values[0]) &&
            context.write32(origin_address + 4U, *guest_values[1]) &&
            context.write32(origin_address + 8U, *guest_values[2]) &&
            context.write32(endpoint_address, *guest_values[3]) &&
            context.write32(endpoint_address + 4U, *guest_values[4]) &&
            context.write32(endpoint_address + 8U, *guest_values[5]);
        if (patched) {
          ++host_aim_ray_patch_count_;
        }
      });
}

void LegacyGameplayVm::bindSyphonFilterUsaV11WeaponEventHooks(
    const LegacyWeaponEventHookProfile &profile) {
  for (const auto boundary : profile.boundaries) {
    bindHostCall(boundary.address, [this, profile,
                                    boundary](LegacyHostCallContext &context) {
      // Observe only the accepted retail edge. The original JAL, delay slot,
      // RA and complete weapon/mission state machine still execute in guest.
      context.continueGuestInstruction();

      std::uint32_t instruction{};
      std::uint32_t delay_instruction{};
      if (context.pc() > std::numeric_limits<std::uint32_t>::max() - 4U ||
          !context.read32(context.pc(), instruction) ||
          !context.read32(context.pc() + 4U, delay_instruction) ||
          instruction != boundary.instruction ||
          delay_instruction != boundary.delay_instruction ||
          profile.maximum_events == 0U ||
          profile.maximum_events > legacy_weapon_events_per_frame) {
        return;
      }
      if (weapon_events_.size() >= profile.maximum_events) {
        return;
      }

      constexpr std::uint32_t guest_ram_begin = 0x80000000U;
      constexpr std::uint32_t guest_ram_end = 0x80200000U;
      std::uint32_t player{};
      std::uint32_t weapon_word{};
      std::uint32_t aim_mode{};
      std::uint16_t actor_slot_bits{};
      std::uint16_t aimed_slot_bits{};
      if (!context.read32(profile.player_pointer, player) ||
          player < guest_ram_begin || player > guest_ram_end - 4U ||
          !context.read16(player + 2U, actor_slot_bits) ||
          !context.read32(profile.current_weapon, weapon_word) ||
          !context.read32(profile.aim_mode, aim_mode) ||
          !context.read16(profile.aimed_target_slot, aimed_slot_bits)) {
        return;
      }

      const auto weapon = static_cast<std::uint8_t>(weapon_word);
      if (weapon_word >= legacy_inventory_weapon_count) {
        return;
      }
      if ((boundary.type == LegacyWeaponEventType::shot ||
           boundary.type == LegacyWeaponEventType::thrown) &&
          context.argument(0) != player &&
          context.registerValue(18U) != player) {
        return;
      }

      auto type = boundary.type;
      switch (type) {
      case LegacyWeaponEventType::shot:
        if (weapon == 0U || weapon == 3U || (weapon >= 18U && weapon != 22U)) {
          return;
        }
        break;
      case LegacyWeaponEventType::thrown:
        if (weapon != 19U && weapon != 20U) {
          return;
        }
        break;
      case LegacyWeaponEventType::scanner_begin:
      case LegacyWeaponEventType::scanner_end:
        if (weapon != 18U) {
          return;
        }
        break;
      case LegacyWeaponEventType::flashlight_toggle:
        if (weapon != 21U) {
          return;
        }
        break;
      case LegacyWeaponEventType::key_card_use:
        if (weapon != 23U) {
          return;
        }
        break;
      case LegacyWeaponEventType::c4_use:
        if (weapon == 25U) {
          type = LegacyWeaponEventType::antigen_use;
        } else if (weapon != 24U) {
          return;
        }
        break;
      case LegacyWeaponEventType::antigen_use:
        return;
      }

      const auto read_signed32 = [&context](std::uint32_t address,
                                            std::int32_t &value) {
        std::uint32_t bits{};
        if (!context.read32(address, bits)) {
          return false;
        }
        value = std::bit_cast<std::int32_t>(bits);
        return true;
      };
      LegacyWeaponEventBridgeState event;
      event.type = type;
      event.weapon = weapon;
      event.actor_slot = std::bit_cast<std::int16_t>(actor_slot_bits);
      event.aimed_target_slot = std::bit_cast<std::int16_t>(aimed_slot_bits);
      event.first_person = legacyWeaponEventUsesFirstPerson(aim_mode);
      event.enabled = type == LegacyWeaponEventType::flashlight_toggle &&
                      context.argument(0) != 0U;
      if (event.actor_slot < 0 ||
          !context.read32(profile.hit_result, event.hit_result) ||
          !read_signed32(profile.ray_origin, event.origin.x) ||
          !read_signed32(profile.ray_origin + 4U, event.origin.y) ||
          !read_signed32(profile.ray_origin + 8U, event.origin.z) ||
          !read_signed32(profile.ray_endpoint, event.endpoint.x) ||
          !read_signed32(profile.ray_endpoint + 4U, event.endpoint.y) ||
          !read_signed32(profile.ray_endpoint + 8U, event.endpoint.z)) {
        return;
      }
      weapon_events_.push_back(event);
    });
  }

  bindHostCall(profile.impact_boundary, [this, profile](
                                            LegacyHostCallContext &context) {
    context.continueGuestInstruction();

    constexpr auto signature_bytes = static_cast<std::uint32_t>(
        (std::tuple_size_v<decltype(profile.impact_instructions)> - 1U) * 4U);
    if (context.pc() >
        std::numeric_limits<std::uint32_t>::max() - signature_bytes) {
      return;
    }
    for (std::size_t word = 0U; word < profile.impact_instructions.size();
         ++word) {
      std::uint32_t instruction{};
      if (!context.read32(context.pc() + static_cast<std::uint32_t>(word * 4U),
                          instruction) ||
          instruction != profile.impact_instructions[word]) {
        return;
      }
    }

    const auto shooter_slot = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(context.argument(0U)));
    const auto edge = std::find_if(
        weapon_events_.rbegin(), weapon_events_.rend(),
        [shooter_slot](const LegacyWeaponEventBridgeState &candidate) {
          return candidate.type == LegacyWeaponEventType::shot &&
                 candidate.actor_slot == shooter_slot &&
                 candidate.impact_count < candidate.impacts.size();
        });
    if (edge == weapon_events_.rend()) {
      return;
    }

    const auto read_signed32 = [&context](std::uint32_t address,
                                          std::int32_t &value) {
      std::uint32_t bits{};
      if (!context.read32(address, bits)) {
        return false;
      }
      value = std::bit_cast<std::int32_t>(bits);
      return true;
    };
    LegacyWeaponImpactBridgeState impact;
    const auto position = context.argument(3U);
    const auto vector = context.argument(4U);
    if (!read_signed32(position, impact.position.x) ||
        !read_signed32(position + 4U, impact.position.y) ||
        !read_signed32(position + 8U, impact.position.z) ||
        !read_signed32(vector, impact.vector.x) ||
        !read_signed32(vector + 4U, impact.vector.y) ||
        !read_signed32(vector + 8U, impact.vector.z) ||
        !context.read32(profile.hit_result, impact.hit_result)) {
      return;
    }

    const auto target_controller = context.argument(1U);
    impact.world = target_controller == 0U;
    impact.effect_kind = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(context.argument(2U)));
    if (!impact.world) {
      std::uint16_t target_slot{};
      if (!context.read16(target_controller + 2U, target_slot)) {
        return;
      }
      impact.target_slot = std::bit_cast<std::int16_t>(target_slot);
      if (impact.target_slot < 0) {
        return;
      }
    }
    edge->impacts[edge->impact_count++] = impact;
  });
}

void LegacyGameplayVm::bindSyphonFilterUsaV11GameplayTextHooks(
    const LegacyGameplayTextHookProfile &profile) {
  const auto profile_valid = profile.maximum_messages_per_frame != 0U &&
                             profile.maximum_messages_per_frame <= 256U &&
                             profile.maximum_text_size != 0U &&
                             profile.maximum_text_size <= 4096U &&
                             profile.text_object_capacity != 0U &&
                             profile.text_object_capacity <= 256U &&
                             profile.text_object_stride >= 0x18U;
  for (const auto &boundary : profile.message_boundaries) {
    bindHostCall(boundary.address, [this, profile, boundary, profile_valid](
                                       LegacyHostCallContext &context) {
      // Observe the retail builder without replacing it. Glyph parsing,
      // allocation, sound/UI timing and the return value remain guest-
      // authored.
      context.continueGuestInstruction();
      if (!profile_valid || boundary.text_argument >= 4U ||
          boundary.duration_argument >= 4U ||
          (boundary.accepted_return_address != 0U &&
           context.registerValue(31U) != boundary.accepted_return_address)) {
        return;
      }
      for (std::size_t word = 0U; word < boundary.instructions.size(); ++word) {
        std::uint32_t instruction{};
        if (!context.read32(boundary.address +
                                static_cast<std::uint32_t>(word * 4U),
                            instruction) ||
            instruction != boundary.instructions[word]) {
          return;
        }
      }
      if (ui_messages_.size() >= profile.maximum_messages_per_frame) {
        return;
      }
      auto channel = boundary.channel;
      if (boundary.channel_from_slot) {
        constexpr std::uint32_t status_text_slot = 6U;
        const auto slot = context.argument(0U);
        if (slot > status_text_slot) {
          return;
        }
        channel = slot == status_text_slot ? LegacyUiMessageChannel::status
                                           : LegacyUiMessageChannel::centered;
      }
      std::string text;
      if (!context.readCString(context.argument(boundary.text_argument), text,
                               profile.maximum_text_size) ||
          text.empty()) {
        return;
      }
      ui_messages_.push_back(LegacyUiMessageBridgeState{
          .channel = channel,
          .text = std::move(text),
          .duration = context.argument(boundary.duration_argument),
          .force_gameplay_layout = boundary.force_gameplay_layout});
    });
  }

  bindHostCall(
      profile.attached_text_entry,
      [this, profile, profile_valid](LegacyHostCallContext &context) {
        // FUN_80085eb0 discards its source string after compiling the glyph
        // packets. Predict its read-only allocator choice and retain the
        // string against that exact TEXT object; the original function still
        // performs every allocation and attachment.
        context.continueGuestInstruction();
        if (!profile_valid) {
          return;
        }
        for (std::size_t word = 0U;
             word < profile.attached_text_instructions.size(); ++word) {
          std::uint32_t instruction{};
          if (!context.read32(profile.attached_text_entry +
                                  static_cast<std::uint32_t>(word * 4U),
                              instruction) ||
              instruction != profile.attached_text_instructions[word]) {
            return;
          }
        }

        const auto attached_slot = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(context.argument(0U)));
        std::uint32_t node{};
        if (!context.read32(profile.active_text_list, node)) {
          return;
        }
        std::vector<std::uint32_t> visited;
        visited.reserve(profile.text_object_capacity);
        while (node != 0U) {
          if (visited.size() >= profile.text_object_capacity ||
              std::ranges::find(visited, node) != visited.end()) {
            return;
          }
          visited.push_back(node);
          std::uint32_t object{};
          std::uint32_t next{};
          std::uint8_t flags{};
          std::uint16_t slot_bits{};
          if (!context.read32(node, object) ||
              !context.read32(node + 8U, next) ||
              !context.read8(object + 0x14U, flags) ||
              !context.read16(object + 0x16U, slot_bits)) {
            return;
          }
          if ((flags & 0x02U) != 0U &&
              std::bit_cast<std::int16_t>(slot_bits) == attached_slot) {
            return;
          }
          node = next;
        }

        std::uint8_t cursor{};
        if (!context.read8(profile.text_pool_cursor, cursor) ||
            cursor >= profile.text_object_capacity) {
          return;
        }
        auto index = static_cast<std::uint32_t>(cursor);
        std::optional<std::uint32_t> selected_object;
        for (std::uint32_t attempt = 0U; attempt < profile.text_object_capacity;
             ++attempt) {
          const auto object64 =
              static_cast<std::uint64_t>(profile.text_object_pool) +
              static_cast<std::uint64_t>(index) * profile.text_object_stride;
          if (object64 > std::numeric_limits<std::uint32_t>::max()) {
            return;
          }
          const auto object = static_cast<std::uint32_t>(object64);
          std::uint8_t flags{};
          if (!context.read8(object + 0x14U, flags)) {
            return;
          }
          if ((flags & 0x01U) == 0U) {
            selected_object = object;
            break;
          }
          index = (index + 1U) % profile.text_object_capacity;
        }
        if (!selected_object) {
          return;
        }

        std::string text;
        if (!context.readCString(context.argument(1U), text,
                                 profile.maximum_text_size) ||
            text.empty()) {
          return;
        }
        const auto text_checksum = legacyTextChecksum(text);
        const auto cached = std::ranges::find_if(
            attached_text_sources_, [selected_object](const auto &entry) {
              return entry.text_object == *selected_object;
            });
        if (cached != attached_text_sources_.end()) {
          cached->text = std::move(text);
          cached->text_checksum = text_checksum;
        } else {
          attached_text_sources_.push_back(
              LegacyGameplayVmSnapshot::AttachedTextSource{
                  *selected_object, std::move(text), text_checksum});
        }
      });
}

bool LegacyGameplayVm::unbindHostCall(std::uint32_t address) noexcept {
  if (address >= 0x80000000U && address < 0x80200000U && (address & 3U) == 0U) {
    ram_host_calls_[(address - 0x80000000U) / sizeof(std::uint32_t)] = nullptr;
  }
  return host_calls_.erase(address) != 0U;
}

void LegacyGameplayVm::clearHostCalls() noexcept {
  std::ranges::fill(ram_host_calls_, nullptr);
  host_calls_.clear();
  host_aim_ray_.reset();
  agent_cbdc_friendly_fire_frame_.reset();
  agent_cbdc_friendly_fire_pending_penalties_ = 0U;
  weapon_events_.clear();
  attached_text_sources_.clear();
  ui_messages_.clear();
  host_aim_ray_patch_count_ = 0U;

  clearAgentHeadshotThreat();
  interrupt_callbacks_.fill(0U);
  machine_.setCdRomMedia(nullptr);
  virtual_cd_.reset();
}

double LegacyFadeBridgeState::blackOpacity() const noexcept {
  if (floor == 0xffU) {
    return 0.0;
  }
  const auto clamped = std::clamp<std::uint16_t>(current, floor, 0xffU);
  return static_cast<double>(clamped - floor) /
         static_cast<double>(0xffU - floor);
}

std::int32_t LegacyCameraBridgeState::projectionForDisplayWidth(
    std::int32_t width) const noexcept {
  constexpr std::int32_t angle_units = 4096;
  constexpr std::int64_t fixed_one = 4096;
  if (width <= 0 || fov_raw >= angle_units / 2) {
    return static_cast<std::uint16_t>(projection);
  }

  // Retail FUN_800cacf0 halves the full horizontal angle, evaluates the
  // PsyQ Q12 sine/cosine pair, divides them to Q12 tangent, then divides
  // half the logical display width by that tangent. Integer divisions
  // truncate toward zero and FUN_800cad98 stores only the low halfword.
  const auto half_angle = fov_raw / 2;
  const auto radians =
      static_cast<double>(half_angle) *
      (2.0 * std::numbers::pi / static_cast<double>(angle_units));
  const auto sine_q12 = static_cast<std::int64_t>(
      std::lround(std::sin(radians) * static_cast<double>(fixed_one)));
  const auto cosine_q12 = static_cast<std::int64_t>(
      std::lround(std::cos(radians) * static_cast<double>(fixed_one)));
  if (cosine_q12 == 0) {
    return 0xffff;
  }
  const auto tangent_q12 = (sine_q12 * fixed_one) / cosine_q12;
  if (tangent_q12 == 0) {
    return 0xffff;
  }
  const auto projected =
      (static_cast<std::int64_t>(width / 2) * fixed_one) / tangent_q12;
  return static_cast<std::uint16_t>(projected);
}

bool validateLegacyWorldModelSets(const LegacyGameplayBridgeState &state,
                                  std::size_t expected_model_count) noexcept {
  if (expected_model_count == 0U || expected_model_count >= 0xfeU ||
      state.world_model_count != expected_model_count ||
      state.player.room < -1 ||
      (state.player.room >= 0 &&
       static_cast<std::size_t>(state.player.room) >= expected_model_count)) {
    return false;
  }

  std::array<bool, 0xfeU> seen{};
  const auto valid_set = [&](std::span<const std::uint16_t> source) {
    seen.fill(false);
    for (const auto model : source) {
      if (model >= expected_model_count || seen[model]) {
        return false;
      }
      seen[model] = true;
    }
    return true;
  };
  return valid_set(state.resident_world_models) &&
         valid_set(state.active_world_models);
}

bool LegacyGameplayVm::finalizeDeadActorDropsBeforeRenderer(
    const LegacyGameplayBridgeProfile &profile,
    std::uint64_t execution_budget) noexcept {
  try {
    constexpr std::uint32_t object_record_stride = 0x4cU;
    constexpr std::uint32_t object_attributes_offset = 0x24U;
    constexpr std::uint32_t object_instance_offset = 0x34U;
    constexpr std::uint32_t object_definition_stride = 0x14U;
    constexpr std::uint32_t instance_definition_slot_offset = 2U;
    constexpr std::uint32_t instance_display_node_offset = 8U;
    constexpr std::uint32_t instance_health_controller_offset = 0x18U;
    constexpr std::uint32_t health_value_offset = 8U;
    constexpr auto vacant_owner = std::numeric_limits<std::uint32_t>::max();
    if (profile.dropped_item_capacity != 30U ||
        profile.dropped_item_attach_entry == 0U ||
        profile.dropped_item_detach_entry == 0U) {
      return false;
    }

    // FUN_800478d0 performs this exact guarded pass before its drop snapshot.
    // Do it before the renderer, never after its packet lists are finalized.
    for (std::uint32_t slot = 0U; slot < profile.dropped_item_capacity;
         ++slot) {
      std::uint32_t owner{};
      if (!runtime_.read32(profile.dropped_item_owners + slot * 4U, owner)) {
        return false;
      }
      if (owner == vacant_owner || (owner & 0x80000000U) == 0U) {
        continue;
      }

      std::uint32_t health_controller{};
      std::uint16_t health_bits{};
      if (!runtime_.read32(owner + instance_health_controller_offset,
                           health_controller) ||
          !runtime_.read16(health_controller + health_value_offset,
                           health_bits)) {
        return false;
      }
      if (std::bit_cast<std::int16_t>(health_bits) >= 1) {
        continue;
      }
      if (!invokeFrameCall(profile.dropped_item_detach_entry,
                           std::array{owner, 1U}, execution_budget)
               .completed()) {
        return false;
      }
    }

    // A few overlay paths can retire a newly killed actor before its normal
    // attachment edge. Recover it while the record, instance and display node
    // are still live, then let the original retail pair author the pickup.
    std::uint32_t object_records{};
    std::uint32_t object_definitions{};
    std::uint32_t object_count_bits{};
    std::uint32_t definition_count_bits{};
    if (!runtime_.read32(profile.object_records_pointer, object_records) ||
        !runtime_.read32(profile.object_definitions_pointer,
                         object_definitions) ||
        !runtime_.read32(profile.object_count, object_count_bits) ||
        !runtime_.read32(profile.object_definition_count,
                         definition_count_bits)) {
      return false;
    }
    const auto object_count = std::bit_cast<std::int32_t>(object_count_bits);
    const auto definition_count =
        std::bit_cast<std::int32_t>(definition_count_bits);
    if (object_count < 0 || definition_count < 0 ||
        static_cast<std::uint32_t>(object_count) > profile.maximum_objects ||
        static_cast<std::uint32_t>(definition_count) >
            profile.maximum_definitions) {
      return false;
    }

    const auto remember_live_drop = [this](std::uint32_t slot,
                                           std::uint32_t instance,
                                           std::uint16_t attributes) {
      const auto found = std::ranges::find_if(
          pending_actor_drops_, [slot](const auto &candidate) {
            return candidate.record_slot == slot;
          });
      const LegacyGameplayVmSnapshot::PendingActorDrop candidate{slot, instance,
                                                                 attributes};
      if (found == pending_actor_drops_.end()) {
        pending_actor_drops_.push_back(candidate);
      } else {
        *found = candidate;
      }
    };
    const auto actor_already_attached = [this, &profile](std::uint32_t instance,
                                                         bool &read_ok) {
      read_ok = true;
      for (std::uint32_t item_slot = 0U;
           item_slot < profile.dropped_item_capacity; ++item_slot) {
        std::uint32_t owner{};
        if (!runtime_.read32(profile.dropped_item_owners + item_slot * 4U,
                             owner)) {
          read_ok = false;
          return false;
        }
        if (owner == instance) {
          return true;
        }
      }
      return false;
    };

    for (std::uint32_t slot = 0U;
         slot < static_cast<std::uint32_t>(object_count); ++slot) {
      const auto record = object_records + slot * object_record_stride;
      std::uint32_t definition{};
      std::uint32_t instance{};
      std::uint16_t attributes{};
      if (!runtime_.read32(record, definition) ||
          !runtime_.read16(record + object_attributes_offset, attributes) ||
          !runtime_.read32(record + object_instance_offset, instance)) {
        return false;
      }
      const auto pending = std::ranges::find_if(
          pending_actor_drops_, [slot](const auto &candidate) {
            return candidate.record_slot == slot;
          });
      if (pending != pending_actor_drops_.end() && instance != 0U &&
          pending->instance != instance) {
        pending_actor_drops_.erase(pending);
      }
      if (definition >= static_cast<std::uint32_t>(definition_count) ||
          instance == 0U ||
          ((attributes & 0x00ffU) == 0U && (attributes & 0x7000U) == 0U)) {
        continue;
      }
      std::uint16_t class_bits{};
      if (!runtime_.read16(object_definitions +
                               definition * object_definition_stride,
                           class_bits)) {
        return false;
      }
      const auto class_id = std::bit_cast<std::int16_t>(class_bits);
      if (class_id != 0x01 && class_id != 0x35) {
        continue;
      }
      std::uint32_t health_controller{};
      std::uint32_t display_node{};
      std::uint16_t health_bits{};
      if (!runtime_.read32(instance + instance_health_controller_offset,
                           health_controller) ||
          !runtime_.read32(instance + instance_display_node_offset,
                           display_node) ||
          health_controller == 0U || display_node == 0U ||
          !runtime_.read16(health_controller + health_value_offset,
                           health_bits)) {
        continue;
      }
      if (std::bit_cast<std::int16_t>(health_bits) >= 1) {
        remember_live_drop(slot, instance, attributes);
        continue;
      }

      // A live record still owns the retail death presentation. Do not
      // manufacture an attach/detach edge merely because health reached
      // zero: that puts the pickup at the pre-fall root and leaves weapons
      // floating on ledges. The guarded owner pass above handles the exact
      // retail attachment edge. Keep the cached identity only as recovery
      // for overlays that actually retire the record before that edge.
      continue;
    }

    // Some mission overlays clear record +0x34 (and occasionally the item
    // bits at +0x24) in their death handler before the renderer-side attach
    // pass.  The live-frame cache above lets us replay the exact missed
    // FUN_80045c04/FUN_80045f84 pair without manufacturing a host pickup.
    for (std::size_t index = 0U; index < pending_actor_drops_.size();) {
      const auto candidate = pending_actor_drops_[index];
      if (candidate.record_slot >= static_cast<std::uint32_t>(object_count)) {
        pending_actor_drops_.erase(pending_actor_drops_.begin() +
                                   static_cast<std::ptrdiff_t>(index));
        continue;
      }

      const auto record =
          object_records + candidate.record_slot * object_record_stride;
      std::uint32_t current_instance{};
      std::uint16_t current_attributes{};
      if (!runtime_.read32(record + object_instance_offset, current_instance) ||
          !runtime_.read16(record + object_attributes_offset,
                           current_attributes)) {
        return false;
      }
      if (current_instance != 0U && current_instance != candidate.instance) {
        pending_actor_drops_.erase(pending_actor_drops_.begin() +
                                   static_cast<std::ptrdiff_t>(index));
        continue;
      }
      const auto current_drop_bits_active =
          (current_attributes & 0x00ffU) != 0U ||
          (current_attributes & 0x7000U) != 0U;
      if (current_instance == candidate.instance && current_drop_bits_active) {
        // The actor is still owned by the overlay. Its normal death/update
        // path must decide when the held weapon reaches the ground.
        ++index;
        continue;
      }

      std::uint16_t instance_slot_bits{};
      std::uint32_t health_controller{};
      std::uint32_t display_node{};
      std::uint16_t health_bits{};
      if (!runtime_.read16(candidate.instance + instance_definition_slot_offset,
                           instance_slot_bits) ||
          instance_slot_bits != candidate.record_slot ||
          !runtime_.read32(candidate.instance +
                               instance_health_controller_offset,
                           health_controller) ||
          !runtime_.read32(candidate.instance + instance_display_node_offset,
                           display_node) ||
          health_controller == 0U || display_node == 0U ||
          !runtime_.read16(health_controller + health_value_offset,
                           health_bits)) {
        pending_actor_drops_.erase(pending_actor_drops_.begin() +
                                   static_cast<std::ptrdiff_t>(index));
        continue;
      }
      if (std::bit_cast<std::int16_t>(health_bits) >= 1) {
        ++index;
        continue;
      }

      bool owner_read_ok{};
      const auto already_attached =
          actor_already_attached(candidate.instance, owner_read_ok);
      if (!owner_read_ok) {
        return false;
      }
      auto attach_complete = true;
      if (!already_attached) {
        if (current_attributes != candidate.attributes &&
            !runtime_.write16(record + object_attributes_offset,
                              candidate.attributes)) {
          return false;
        }
        attach_complete =
            invokeFrameCall(profile.dropped_item_attach_entry,
                            std::array{candidate.instance}, execution_budget)
                .completed();
        if (current_attributes != candidate.attributes &&
            !runtime_.write16(record + object_attributes_offset,
                              current_attributes)) {
          return false;
        }
      }
      if (!attach_complete ||
          !invokeFrameCall(profile.dropped_item_detach_entry,
                           std::array{candidate.instance, 1U}, execution_budget)
               .completed()) {
        return false;
      }
      pending_actor_drops_.erase(pending_actor_drops_.begin() +
                                 static_cast<std::ptrdiff_t>(index));
    }
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<LegacyGameplayBridgeState>
LegacyGameplayVm::readBridgeState(const LegacyGameplayBridgeProfile &profile) {
  // Until the complete immutable bridge has been validated, expose a useful
  // fail-closed reason to the production runtime instead of collapsing every
  // malformed guest field into an undifferentiated return-to-title fault.
  last_bridge_read_fault_ = LegacyGameplayBridgeReadFault::invalid_snapshot;
  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::globals;
  constexpr std::uint32_t camera_target_x_offset = 0x444U;
  constexpr std::uint32_t camera_target_y_offset = 0x448U;
  constexpr std::uint32_t camera_target_z_offset = 0x44cU;
  constexpr std::uint32_t camera_projection_offset = 4U;
  constexpr std::uint32_t camera_fov_offset = 0xbdcU;
  constexpr std::uint32_t matrix_translation_x_offset = 0x14U;
  constexpr std::uint32_t matrix_translation_y_offset = 0x18U;
  constexpr std::uint32_t matrix_translation_z_offset = 0x1cU;
  constexpr std::uint32_t object_record_stride = 0x4cU;
  constexpr std::uint32_t object_authored_x_offset = 0x18U;
  constexpr std::uint32_t object_authored_y_offset = 0x1cU;
  constexpr std::uint32_t object_authored_z_offset = 0x20U;
  constexpr std::uint32_t object_attributes_offset = 0x24U;
  constexpr std::uint32_t object_parameter_offset = 0x28U;
  constexpr std::uint32_t object_path_pointer_offset = 0x2cU;
  constexpr std::uint32_t object_linked_slot_offset = 0x30U;
  constexpr std::uint32_t object_instance_offset = 0x34U;
  constexpr std::uint32_t object_maximum_health_offset = 0x3eU;
  constexpr std::uint32_t object_health_offset = 0x40U;
  constexpr std::uint32_t object_definition_stride = 0x14U;
  constexpr std::uint32_t instance_flags_offset = 0U;
  constexpr std::uint32_t instance_node_offset = 8U;
  constexpr std::uint32_t instance_motion_offset = 0x0cU;
  constexpr std::uint32_t instance_presentation_offset = 0x10U;
  constexpr std::uint32_t instance_target_offset = 0x14U;
  constexpr std::uint32_t instance_health_offset = 0x18U;
  constexpr std::uint32_t instance_ai_offset = 0x1cU;
  constexpr std::uint32_t instance_state_offset = 0x20U;
  constexpr std::uint32_t node_pose_flags_offset = 8U;
  // FUN_800d0058 clears this bit before each HMD render pass and
  // FUN_800cfe64 sets it only after the display's bone table was emitted.
  // The allocated display+0x18 table may otherwise retain an older pose.
  constexpr std::uint32_t node_hmd_rendered_this_pass = 0x40U;
  constexpr std::uint32_t node_matrix_offset = 0x0cU;
  constexpr std::uint32_t display_bone_matrices_offset = 0x18U;
  constexpr std::uint32_t display_light_state_offset = 0x1cU;
  constexpr std::uint32_t light_back_color_offset = 4U;
  constexpr std::uint32_t motion_position_x_offset = 0U;
  constexpr std::uint32_t motion_position_y_offset = 4U;
  constexpr std::uint32_t motion_position_z_offset = 8U;
  constexpr std::uint32_t motion_ground_contact_y_offset = 0x12cU;
  constexpr std::uint32_t presentation_enabled_offset = 8U;
  constexpr std::uint32_t presentation_mode_offset = 9U;
  constexpr std::uint32_t target_slot_offset = 0U;
  constexpr std::uint32_t target_flags_offset = 4U;
  constexpr std::uint32_t target_meter_offset = 0x58U;
  constexpr std::uint32_t target_danger_offset = 0xd4U;
  constexpr std::uint32_t ai_flags_offset = 0x20U;
  constexpr std::uint32_t ai_fire_latch_offset = 0x41U;
  constexpr std::uint32_t ai_route_node_offset = 0x43U;
  constexpr std::uint32_t ai_previous_route_node_offset = 0x44U;
  constexpr std::uint32_t route_node_stride = 0x0cU;
  constexpr std::uint32_t route_node_flags_offset = 6U;
  constexpr std::uint32_t ai_mode_offset = 0x46U;
  constexpr std::uint32_t ai_archetype_offset = 0x47U;
  constexpr std::uint32_t ai_combat_mode_offset = 0x48U;
  constexpr std::uint32_t ai_pool_index_offset = 0x4dU;
  constexpr std::uint32_t ai_state_offset = 0x52U;
  constexpr std::uint32_t resident_world_models_offset = 0x78U;
  constexpr std::uint8_t world_model_prefetch_marker = 0xfeU;
  constexpr std::uint8_t world_model_end_marker = 0xffU;

  const auto address =
      [](std::uint32_t base,
         std::uint64_t offset) -> std::optional<std::uint32_t> {
    const auto result = static_cast<std::uint64_t>(base) + offset;
    if (result > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(result);
  };
  const auto readable_ram_pointer = [](std::uint32_t pointer,
                                       std::size_t size) {
    constexpr std::uint64_t ram_begin = 0x80000000ULL;
    constexpr std::uint64_t ram_end = 0x80200000ULL;
    const auto end = static_cast<std::uint64_t>(pointer) + size;
    return (pointer & 3U) == 0U && pointer >= ram_begin && end <= ram_end;
  };
  const auto read_signed16 = [this](std::uint32_t source, std::int16_t &value) {
    std::uint16_t bits{};
    if (!runtime_.read16(source, bits)) {
      return false;
    }
    value = std::bit_cast<std::int16_t>(bits);
    return true;
  };
  const auto read_signed32 = [this](std::uint32_t source, std::int32_t &value) {
    std::uint32_t bits{};
    if (!runtime_.read32(source, bits)) {
      return false;
    }
    value = std::bit_cast<std::int32_t>(bits);
    return true;
  };
  const auto read_rgb = [this, &address](std::uint32_t source,
                                         std::uint32_t offset,
                                         LegacyRgbBridgeState &value) {
    const auto red = address(source, offset);
    const auto green = address(source, static_cast<std::uint64_t>(offset) + 1U);
    const auto blue = address(source, static_cast<std::uint64_t>(offset) + 2U);
    return red && green && blue && runtime_.read8(*red, value.red) &&
           runtime_.read8(*green, value.green) &&
           runtime_.read8(*blue, value.blue);
  };
  const auto read_pointer_string =
      [this](std::uint32_t pointer_address) -> std::optional<std::string> {
    constexpr std::uint32_t ram_begin = 0x80000000U;
    constexpr std::uint64_t ram_end = 0x80200000ULL;
    constexpr std::size_t maximum_length = 96U;
    std::uint32_t pointer{};
    if (!runtime_.read32(pointer_address, pointer) || pointer < ram_begin ||
        pointer >= ram_end) {
      return std::nullopt;
    }
    std::string result;
    result.reserve(maximum_length);
    for (std::size_t index = 0U; index < maximum_length; ++index) {
      const auto address = static_cast<std::uint64_t>(pointer) + index;
      if (address >= ram_end) {
        return std::nullopt;
      }
      std::uint8_t character{};
      if (!runtime_.read8(static_cast<std::uint32_t>(address), character)) {
        return std::nullopt;
      }
      if (character == 0U) {
        return result.empty() ? std::nullopt
                              : std::optional<std::string>{std::move(result)};
      }
      if (character < 0x20U || character >= 0x7fU) {
        return std::nullopt;
      }
      result.push_back(static_cast<char>(character));
    }
    return std::nullopt;
  };
  const auto read_matrix_point = [&](std::uint32_t matrix,
                                     LegacyNativePoint &point) {
    const auto x_address = address(matrix, matrix_translation_x_offset);
    const auto y_address = address(matrix, matrix_translation_y_offset);
    const auto z_address = address(matrix, matrix_translation_z_offset);
    if (!x_address || !y_address || !z_address ||
        !read_signed32(*x_address, point.x) ||
        !read_signed32(*y_address, point.y) ||
        !read_signed32(*z_address, point.z)) {
      return false;
    }
    return true;
  };
  const auto read_matrix = [&](std::uint32_t source,
                               LegacyNativeMatrix &matrix) {
    if (!read_matrix_point(source, matrix.translation)) {
      return false;
    }
    for (std::uint32_t component = 0U; component < matrix.rotation.size();
         ++component) {
      const auto component_address = address(source, component * 2U);
      if (!component_address ||
          !read_signed16(*component_address, matrix.rotation[component])) {
        return false;
      }
    }
    return true;
  };

  // FUN_800db41c materializes a display MATRIX from the local coordinate
  // stored at MATRIX+0x20, recursively composing its parent with
  // CompMatrixLV. Calling that guest helper from this immutable bridge used
  // to advance the CPU, device timers and SPU while also dirtying guest RAM.
  // Mirror the exact fixed-point composition without touching VM state.
  struct ResolvedMatrix {
    std::uint32_t address{};
    LegacyNativeMatrix value;
  };
  std::vector<ResolvedMatrix> resolved_matrices;
  std::vector<std::uint32_t> resolving_matrices;
  const auto arithmetic_shift_12 = [](std::int64_t value) {
    constexpr std::int64_t divisor = 1LL << 12U;
    return value >= 0 ? value / divisor : -((-value + divisor - 1LL) / divisor);
  };
  const auto saturate_ir = [](std::int64_t value) {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(
        value, std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
  };
  const auto wrap_translation = [](std::int64_t value) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
  };
  const auto compose_matrix = [&](const LegacyNativeMatrix &parent,
                                  const LegacyNativeMatrix &local) {
    LegacyNativeMatrix result;
    for (std::size_t row = 0U; row < 3U; ++row) {
      for (std::size_t column = 0U; column < 3U; ++column) {
        std::int64_t accumulator{};
        for (std::size_t inner = 0U; inner < 3U; ++inner) {
          accumulator +=
              static_cast<std::int64_t>(parent.rotation[row * 3U + inner]) *
              local.rotation[inner * 3U + column];
        }
        result.rotation[row * 3U + column] =
            saturate_ir(arithmetic_shift_12(accumulator));
      }
    }

    const std::array local_translation{local.translation.x, local.translation.y,
                                       local.translation.z};
    const std::array parent_translation{
        parent.translation.x, parent.translation.y, parent.translation.z};
    std::array<std::int32_t, 3U> high{};
    std::array<std::int32_t, 3U> remainder{};
    for (std::size_t component = 0U; component < 3U; ++component) {
      high[component] = local_translation[component] / 0x8000;
      remainder[component] =
          local_translation[component] - high[component] * 0x8000;
    }
    std::array<std::int32_t *, 3U> output_translation{
        &result.translation.x, &result.translation.y, &result.translation.z};
    for (std::size_t row = 0U; row < 3U; ++row) {
      std::int64_t high_accumulator{};
      std::int64_t remainder_accumulator{};
      for (std::size_t column = 0U; column < 3U; ++column) {
        const auto rotation =
            static_cast<std::int64_t>(parent.rotation[row * 3U + column]);
        high_accumulator += rotation * high[column];
        remainder_accumulator += rotation * remainder[column];
      }
      const auto high_ir = saturate_ir(high_accumulator);
      const auto remainder_ir =
          saturate_ir(arithmetic_shift_12(remainder_accumulator));
      *output_translation[row] =
          wrap_translation(static_cast<std::int64_t>(high_ir) * 8LL +
                           remainder_ir + parent_translation[row]);
    }
    return result;
  };
  const auto resolve_matrix = [&](auto &&self, std::uint32_t output,
                                  LegacyNativeMatrix &result) -> bool {
    if (const auto cached = std::ranges::find(resolved_matrices, output,
                                              &ResolvedMatrix::address);
        cached != resolved_matrices.end()) {
      result = cached->value;
      return true;
    }
    if (resolving_matrices.size() >= 64U ||
        std::ranges::find(resolving_matrices, output) !=
            resolving_matrices.end() ||
        !readable_ram_pointer(output, 0x24U)) {
      return false;
    }

    resolving_matrices.push_back(output);
    std::uint32_t coordinate{};
    std::uint32_t parent_output{};
    LegacyNativeMatrix local;
    auto complete = runtime_.read32(output + 0x20U, coordinate) &&
                    readable_ram_pointer(coordinate, 0x24U) &&
                    read_matrix(coordinate, local) &&
                    runtime_.read32(coordinate + 0x20U, parent_output);
    if (complete && parent_output == 0U) {
      result = local;
    } else if (complete) {
      LegacyNativeMatrix parent;
      complete = self(self, parent_output, parent);
      if (complete) {
        result = compose_matrix(parent, local);
      }
    }
    resolving_matrices.pop_back();
    if (!complete) {
      return false;
    }
    resolved_matrices.push_back({output, result});
    return true;
  };

  LegacyGameplayBridgeState state;
  std::uint32_t player_control_lock{};
  std::uint16_t current_room_bits{};
  std::uint8_t target_lock_active{};
  std::uint8_t aim_miss{};
  std::int32_t virus_scanner_target_slot{-1};
  std::uint32_t flashlight_handle{};
  std::uint16_t headshot_text_handle{};
  std::int16_t primary_story_target_slot{-1};
  std::int16_t secondary_story_target_slot{-1};
  if (!runtime_.read16(profile.dynamic_first_slot, state.dynamic_first_slot) ||
      !runtime_.read16(profile.processed_pad0 + 4U, state.pad.buttons) ||
      !runtime_.read8(profile.processed_pad0 + 6U, state.pad.right_x) ||
      !runtime_.read8(profile.processed_pad0 + 7U, state.pad.right_y) ||
      !runtime_.read8(profile.processed_pad0 + 8U, state.pad.left_x) ||
      !runtime_.read8(profile.processed_pad0 + 9U, state.pad.left_y) ||
      !runtime_.read32(profile.player_control_lock, player_control_lock) ||
      !runtime_.read16(profile.current_room, current_room_bits) ||
      !runtime_.read8(profile.target_lock_active, target_lock_active) ||
      !read_signed32(profile.aim_target, state.aim_target.x) ||
      !read_signed32(profile.aim_target + 4U, state.aim_target.y) ||
      !read_signed32(profile.aim_target + 8U, state.aim_target.z) ||
      !runtime_.read8(profile.aim_miss, aim_miss) ||
      !read_signed32(profile.virus_scanner_target,
                     state.virus_scanner_target.x) ||
      !read_signed32(profile.virus_scanner_target + 4U,
                     state.virus_scanner_target.y) ||
      !read_signed32(profile.virus_scanner_target + 8U,
                     state.virus_scanner_target.z) ||
      !read_signed32(profile.virus_scanner_target_slot,
                     virus_scanner_target_slot) ||
      !runtime_.read32(profile.flashlight_enabled, flashlight_handle) ||
      !runtime_.read16(profile.taser_conductor_phase,
                       state.taser_conductor_phase) ||
      !read_signed16(profile.taser_target_slot, state.taser_target_slot) ||
      !runtime_.read32(profile.target_hit_result, state.target_hit_result) ||
      !read_signed16(profile.aimed_target_slot, state.aimed_target_slot) ||
      !read_signed16(profile.proximity_target_slot,
                     state.proximity_target_slot) ||
      !runtime_.read16(profile.headshot_text_handle, headshot_text_handle) ||
      !read_signed16(profile.primary_story_target_slot,
                     primary_story_target_slot) ||
      !read_signed16(profile.secondary_story_target_slot,
                     secondary_story_target_slot)) {
    return std::nullopt;
  }
  state.player.control_locked = player_control_lock != 0U;
  state.player.room = std::bit_cast<std::int16_t>(current_room_bits);
  state.target_lock_active = target_lock_active != 0U;
  state.aim_target_valid = aim_miss == 0U;

  // FUN_800cd734 owns this intrusive list. A source is active only while its
  // +0 handle names the node that actually contains it; sampling the low byte
  // of DAT_8012f9b8 used to randomly disable the flashlight for ...00 handles.
  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::dynamic_lights;
  if (profile.maximum_vertex_lights == 0U ||
      profile.maximum_vertex_lights > legacy_vertex_light_capacity) {
    return std::nullopt;
  }
  std::uint32_t light_node{};
  if (!runtime_.read32(profile.dynamic_light_list, light_node)) {
    return std::nullopt;
  }
  state.vertex_lights.reserve(profile.maximum_vertex_lights);
  while (light_node != 0U) {
    if (state.vertex_lights.size() >= profile.maximum_vertex_lights ||
        !readable_ram_pointer(light_node, 0x0cU)) {
      return std::nullopt;
    }
    std::uint32_t source{};
    std::uint32_t next{};
    if (!runtime_.read32(light_node, source) ||
        !runtime_.read32(light_node + 8U, next) ||
        !readable_ram_pointer(source, 0x40U) ||
        std::ranges::any_of(state.vertex_lights,
                            [source](const auto &resident) {
                              return resident.source == source;
                            })) {
      return std::nullopt;
    }

    std::uint32_t source_handle{};
    std::uint32_t matrix_pointer{};
    LegacyVertexLightBridgeState light;
    light.source = source;
    if (!runtime_.read32(source, source_handle) ||
        source_handle != light_node ||
        !runtime_.read32(source + 4U, light.flags) ||
        !runtime_.read32(source + 8U, matrix_pointer) ||
        !readable_ram_pointer(matrix_pointer, 0x20U) ||
        !read_matrix(matrix_pointer, light.matrix) ||
        !read_signed32(source + 0x0cU, light.shape) ||
        !runtime_.read32(source + 0x10U, light.screen_shift) ||
        !runtime_.read32(source + 0x14U, light.depth_shift) ||
        !read_signed32(source + 0x18U, light.threshold) ||
        !runtime_.read32(source + 0x1cU, light.channel_mask) ||
        light.screen_shift > 31U || light.depth_shift > 31U ||
        light.shape < 0 || light.threshold < 0 ||
        (light.channel_mask & 0xff000000U) != 0U) {
      return std::nullopt;
    }
    state.vertex_lights.push_back(light);
    light_node = next;
  }
  const auto flashlight =
      std::ranges::find(state.vertex_lights, profile.flashlight_enabled,
                        &LegacyVertexLightBridgeState::source);
  state.flashlight_enabled = flashlight != state.vertex_lights.end();
  if ((flashlight_handle != 0U) != state.flashlight_enabled) {
    return std::nullopt;
  }

  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::world;
  std::uint32_t world_layout{};
  std::uint32_t world_model_count_bits{};
  std::uint32_t object_activation_distance_bits{};
  std::uint8_t gameplay_trigger_enable{};
  if (!runtime_.read32(profile.world_layout_pointer, world_layout) ||
      !runtime_.read32(profile.world_model_count, world_model_count_bits) ||
      !runtime_.read32(profile.object_activation_distance,
                       object_activation_distance_bits) ||
      !runtime_.read8(profile.gameplay_trigger_enable,
                      gameplay_trigger_enable)) {
    return std::nullopt;
  }
  const auto world_model_count =
      std::bit_cast<std::int32_t>(world_model_count_bits);
  if (world_model_count <= 0 ||
      world_model_count >= world_model_prefetch_marker ||
      profile.maximum_world_models == 0U ||
      profile.maximum_world_models > world_model_prefetch_marker ||
      static_cast<std::uint32_t>(world_model_count) >
          profile.maximum_world_models ||
      profile.maximum_resident_world_models == 0U ||
      profile.maximum_resident_world_models > 256U || state.player.room < -1 ||
      (state.player.room >= 0 && state.player.room >= world_model_count)) {
    return std::nullopt;
  }
  state.world_model_count = static_cast<std::uint16_t>(world_model_count);
  state.object_activation_distance =
      std::bit_cast<std::int32_t>(object_activation_distance_bits);
  state.terrain_triggers_enabled = gameplay_trigger_enable != 0U;
  const auto resident_begin =
      address(world_layout, resident_world_models_offset);
  if (!resident_begin ||
      !readable_ram_pointer(*resident_begin,
                            profile.maximum_resident_world_models) ||
      !readable_ram_pointer(profile.world_visibility_bytes,
                            state.world_model_count)) {
    return std::nullopt;
  }
  bool resident_terminated = false;
  for (std::uint32_t index = 0U; index < profile.maximum_resident_world_models;
       ++index) {
    std::uint8_t model{};
    if (!runtime_.read8(*resident_begin + index, model)) {
      return std::nullopt;
    }
    if (model == world_model_end_marker) {
      resident_terminated = true;
      break;
    }
    if (model == world_model_prefetch_marker ||
        model >= state.world_model_count) {
      return std::nullopt;
    }
    // Retail DATs can repeat a resident entry (WHOUSE repeats model 56).
    // The bridge exposes a set, so normalize that authored duplication while
    // keeping marker/range validation fail-closed.
    if (std::ranges::find(state.resident_world_models, model) ==
        state.resident_world_models.end()) {
      state.resident_world_models.push_back(model);
    }
  }
  if (!resident_terminated) {
    return std::nullopt;
  }
  state.active_world_models.reserve(state.world_model_count);
  for (std::uint16_t model = 0U; model < state.world_model_count; ++model) {
    std::uint8_t visibility{};
    if (!runtime_.read8(profile.world_visibility_bytes + model, visibility)) {
      return std::nullopt;
    }
    if (visibility != 0U) {
      state.active_world_models.push_back(model);
    }
  }
  if (!validateLegacyWorldModelSets(state, state.world_model_count) ||
      profile.world_model_descriptor_stride < 0x14U ||
      profile.maximum_world_sections != 31U ||
      profile.maximum_world_section_vertices == 0U ||
      profile.maximum_world_vertex_colors == 0U) {
    return std::nullopt;
  }

  constexpr std::uint32_t decal_active_offset = 0x20U;
  constexpr std::uint32_t decal_owner_offset = 0x24U;
  constexpr std::uint32_t decal_material_offset = 0x28U;
  if (profile.world_decal_stride != 0x38U ||
      profile.world_decal_capacity != legacy_world_decal_capacity) {
    return std::nullopt;
  }
  state.world_decals.reserve(profile.world_decal_capacity);
  for (std::uint32_t slot = 0U; slot < profile.world_decal_capacity; ++slot) {
    const auto record =
        address(profile.world_decal_pool,
                static_cast<std::uint64_t>(slot) * profile.world_decal_stride);
    std::uint32_t active{};
    std::int32_t owner{};
    if (!record || !runtime_.read32(*record + decal_active_offset, active) ||
        active > 1U) {
      return std::nullopt;
    }
    if (active == 0U) {
      continue;
    }
    if (!read_signed32(*record + decal_owner_offset, owner)) {
      return std::nullopt;
    }
    LegacyWorldDecalBridgeState decal;
    decal.owner = owner;
    decal.slot = static_cast<std::uint8_t>(slot);
    for (std::uint32_t vertex = 0U; vertex < decal.vertices.size(); ++vertex) {
      const auto source = *record + vertex * 8U;
      std::int16_t x{};
      std::int16_t y{};
      std::int16_t z{};
      if (!read_signed16(source, x) || !read_signed16(source + 2U, y) ||
          !read_signed16(source + 4U, z)) {
        return std::nullopt;
      }
      decal.vertices[vertex] = {x, y, z};
    }
    for (std::uint32_t word = 0U; word < decal.material_words.size(); ++word) {
      if (!runtime_.read32(*record + decal_material_offset + word * 4U,
                           decal.material_words[word])) {
        return std::nullopt;
      }
    }
    state.world_decals.push_back(decal);
  }

  // FUN_8002ba08/FUN_800c6ac0 do not create a radial lamp. They mutate the
  // relocated BGR555 color at section vertex+6. The native widescreen DAT
  // envelope can expose a model before it enters retail's 4:3 camera set, so
  // capture every descriptor whose payload is already loaded. Limiting this
  // to resident/visible models left authored lamp-lit colors in the host cache
  // until the player entered that zone. Null/unloaded descriptors still retain
  // their last immutable host color and are sampled as soon as retail loads
  // them.
  std::uint32_t world_descriptors{};
  const auto descriptor_bytes =
      static_cast<std::uint64_t>(state.world_model_count) *
      profile.world_model_descriptor_stride;
  const auto world_descriptors_available =
      runtime_.read32(profile.world_model_descriptors, world_descriptors) &&
      descriptor_bytes <= std::numeric_limits<std::size_t>::max() &&
      readable_ram_pointer(world_descriptors,
                           static_cast<std::size_t>(descriptor_bytes));
  auto captured_vertex_colors = std::size_t{};
  const auto ram = runtime_.ram();
  const auto read_ram16 = [&ram](std::uint32_t source,
                                 std::uint16_t &value) noexcept {
    const auto segment = source & 0xe0000000U;
    if (segment != 0U && segment != 0x80000000U && segment != 0xa0000000U) {
      return false;
    }
    const auto physical = source & 0x1fffffffU;
    if (physical >= ram.size() || ram.size() - physical < 2U) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(ram[physical]) |
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(ram[physical + 1U]))
         << 8U));
    return true;
  };
  const auto capture_world_model = [&](std::uint16_t model_index) {
    const auto descriptor =
        address(world_descriptors, static_cast<std::uint64_t>(model_index) *
                                       profile.world_model_descriptor_stride);
    std::uint32_t model{};
    std::uint32_t model_resource{};
    std::uint32_t payload{};
    if (!descriptor || !runtime_.read32(*descriptor, model)) {
      return;
    }
    // The descriptor array covers the whole mission; streamed-out models have
    // a null object and simply contribute no colors to this immutable frame.
    if (model == 0U) {
      return;
    }
    if (!readable_ram_pointer(model, 0x14U) ||
        !runtime_.read32(model + 0x10U, model_resource) ||
        !readable_ram_pointer(model_resource, 0x24U) ||
        !runtime_.read32(model_resource + 0x20U, payload)) {
      return;
    }
    // A visibility/residency byte can lead the asynchronous retail streamer.
    // Its model descriptor then exists while +0x20 still carries the unloaded
    // sentinel. No guest vertex colors exist yet, so retain authored colors.
    if (payload == 0U || payload == std::numeric_limits<std::uint32_t>::max()) {
      return;
    }
    // BUNKER keeps this relocated resource through the uncached physical RAM
    // alias while the other overlays use KSEG0. R3000 maps both to the same
    // 2 MiB; canonicalize before applying the bridge's host-pointer bounds.
    if (payload < 0x00200000U) {
      payload |= 0x80000000U;
    }
    if (!readable_ram_pointer(payload, 0x88U)) {
      return;
    }
    std::vector<LegacyWorldSectionColorsBridgeState> model_colors;
    model_colors.reserve(profile.maximum_world_sections);
    auto model_vertex_colors = std::size_t{};
    for (std::uint32_t section_index = 0U;
         section_index < profile.maximum_world_sections; ++section_index) {
      std::uint32_t section{};
      if (!runtime_.read32(payload + 4U + section_index * 4U, section)) {
        return;
      }
      if (section == std::numeric_limits<std::uint32_t>::max()) {
        break;
      }
      if (section < 0x00200000U) {
        section |= 0x80000000U;
      }
      std::uint16_t vertex_count{};
      std::uint32_t vertex_relative_offset{};
      if (!readable_ram_pointer(section, 0x2cU) ||
          !runtime_.read16(section + 6U, vertex_count) || vertex_count == 0U ||
          vertex_count > profile.maximum_world_section_vertices ||
          !runtime_.read32(section + 0x24U, vertex_relative_offset)) {
        return;
      }
      const auto vertex_base = address(section, vertex_relative_offset);
      const auto vertex_bytes = static_cast<std::uint64_t>(vertex_count) * 8U;
      if (!vertex_base ||
          vertex_bytes > std::numeric_limits<std::size_t>::max() ||
          !readable_ram_pointer(*vertex_base,
                                static_cast<std::size_t>(vertex_bytes)) ||
          vertex_count > profile.maximum_world_vertex_colors ||
          model_vertex_colors >
              profile.maximum_world_vertex_colors - vertex_count) {
        return;
      }
      LegacyWorldSectionColorsBridgeState colors;
      colors.model = model_index;
      colors.section = static_cast<std::uint16_t>(section_index);
      colors.colors.resize(vertex_count);
      for (std::uint32_t vertex = 0U; vertex < vertex_count; ++vertex) {
        if (!read_ram16(*vertex_base + vertex * 8U + 6U,
                        colors.colors[vertex])) {
          return;
        }
      }
      model_vertex_colors += vertex_count;
      model_colors.push_back(std::move(colors));
    }
    // Prioritize the validated retail camera set. Optional preloaded models
    // are deterministic best-effort additions and must never exhaust the
    // bounded immutable frame or partially replace their previous cache.
    if (captured_vertex_colors >
        profile.maximum_world_vertex_colors - model_vertex_colors) {
      return;
    }
    captured_vertex_colors += model_vertex_colors;
    for (auto &colors : model_colors) {
      state.world_vertex_colors.push_back(std::move(colors));
    }
  };
  // World visibility and residency above are authoritative. Live per-vertex
  // colors are an auxiliary presentation cache: the retail streamer can keep
  // a model visible while recycling its relocated color payload. Skipping that
  // transient payload preserves the last immutable host colors and must not
  // invalidate the complete guest renderer/UI snapshot.
  if (world_descriptors_available) {
    std::array<bool, 0xfeU> prioritized{};
    const auto capture_set = [&](std::span<const std::uint16_t> source) {
      for (const auto model_index : source) {
        if (!prioritized[model_index]) {
          prioritized[model_index] = true;
          capture_world_model(model_index);
        }
      }
    };

    // Required camera-visible models are captured first. Resident models are
    // preloaded resources, so their colors are useful but must not consume the
    // bounded frame before the live retail traversal has been preserved.
    capture_set(state.active_world_models);
    if (state.player.room >= 0) {
      const auto room = static_cast<std::uint16_t>(state.player.room);
      capture_set(std::span<const std::uint16_t>{&room, 1U});
    }
    capture_set(state.resident_world_models);
    for (std::uint16_t model_index = 0U; model_index < state.world_model_count;
         ++model_index) {
      if (!prioritized[model_index]) {
        capture_world_model(model_index);
      }
    }
  }
  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::dropped_items;
  for (std::size_t index = 0U; index < state.tracked_slots.size(); ++index) {
    const auto slot_address = address(profile.tracked_slots, index * 2U);
    if (!slot_address ||
        !read_signed16(*slot_address, state.tracked_slots[index])) {
      return std::nullopt;
    }
  }
  if (profile.dropped_item_capacity != 30U ||
      profile.dropped_item_matrix_stride != 0x24U) {
    return std::nullopt;
  }
  state.dropped_item_floor_owner_mask = 0U;
  state.dropped_items.reserve(profile.dropped_item_capacity);
  for (std::uint32_t slot = 0U; slot < profile.dropped_item_capacity; ++slot) {
    const auto owner_address = address(profile.dropped_item_owners, slot * 4U);
    const auto descriptor_address =
        address(profile.dropped_item_descriptors, slot * 4U);
    std::uint32_t owner{};
    std::uint32_t descriptor{};
    if (!owner_address || !descriptor_address ||
        !runtime_.read32(*owner_address, owner) ||
        !runtime_.read32(*descriptor_address, descriptor)) {
      return std::nullopt;
    }
    // Attached equipment stores an actor pointer (KSEG0); 0xffffffff is the
    // vacant sentinel. Only a non-negative in-range room owner is a floor
    // pickup. Publish that ownership before descriptor validation so the
    // presentation cache can distinguish allocator churn from collection.
    if ((owner & 0x80000000U) != 0U || owner >= state.world_model_count) {
      continue;
    }
    state.dropped_item_floor_owner_mask |= std::uint32_t{1U} << slot;
    std::uint16_t item{};
    std::uint32_t matrix{};
    LegacyNativeMatrix transform;
    // Descriptor+0x0c is the live retail bridge. The allocator can expose a
    // non-negative owner while recycling a detached descriptor; that slot is
    // not a drawable pickup until its fixed MATRIX and selector agree again.
    // Treat this bounded auxiliary pool independently: one transitional slot
    // must not invalidate the camera, world, actors and UI captured in the
    // same retail frame.
    const auto descriptor_matrix = address(descriptor, 0x0cU);
    const auto expected_matrix = address(
        profile.dropped_item_matrices,
        static_cast<std::uint64_t>(slot) * profile.dropped_item_matrix_stride);
    if (!readable_ram_pointer(descriptor, 0x18U) || !descriptor_matrix ||
        !expected_matrix || !runtime_.read32(*descriptor_matrix, matrix) ||
        matrix != *expected_matrix || !readable_ram_pointer(matrix, 0x20U) ||
        !runtime_.read16(descriptor + 0x16U, item) ||
        !read_matrix(matrix, transform) ||
        (item >= legacy_inventory_weapon_count && item != 0x80U)) {
      continue;
    }
    state.dropped_items.push_back(LegacyDroppedItemBridgeState{
        static_cast<std::uint8_t>(slot), static_cast<std::uint16_t>(owner),
        item, transform});
  }
  const auto read_thrown_projectile =
      [&](std::uint32_t pointer_address, std::uint32_t &descriptor,
          std::optional<LegacyThrownProjectileBridgeState> &output) {
        if (!runtime_.read32(pointer_address, descriptor)) {
          return false;
        }
        if (!readable_ram_pointer(descriptor, 0x10U)) {
          return true;
        }
        std::uint8_t age{};
        std::uint32_t weapon{};
        std::uint32_t render_object{};
        if (!runtime_.read8(descriptor + 1U, age) ||
            !runtime_.read32(descriptor + 4U, weapon) ||
            !runtime_.read32(descriptor + 8U, render_object)) {
          return false;
        }
        std::uint32_t renderer_link{};
        constexpr std::uint32_t object_matrix_offset = 0x1cU;
        if (readable_ram_pointer(render_object, object_matrix_offset + 0x20U) &&
            runtime_.read32(render_object, renderer_link) &&
            renderer_link != 0U && age <= 60U &&
            (weapon == 19U || weapon == 20U)) {
          LegacyThrownProjectileBridgeState projectile;
          projectile.age = age;
          projectile.weapon = static_cast<std::uint8_t>(weapon);
          if (!read_matrix(render_object + object_matrix_offset,
                           projectile.transform)) {
            return false;
          }
          output = std::move(projectile);
        }
        return true;
      };
  std::uint32_t projectile_descriptor{};
  std::uint32_t enemy_projectile_descriptor{};
  if (!read_thrown_projectile(profile.player_thrown_projectile_pointer,
                              projectile_descriptor, state.thrown_projectile) ||
      !read_thrown_projectile(profile.enemy_thrown_projectile_pointer,
                              enemy_projectile_descriptor,
                              state.enemy_thrown_projectile)) {
    return std::nullopt;
  }
  // Retail keeps the dotted ballistic guide alive for the entire grenade aim
  // mode. FUN_80025dfc opens its charge window by clearing DAT_80127d98 and
  // latching DAT_80127da0; FUN_80026608 then clamps the elapsed clock to
  // 0x28f..0xccc. Before charging, use that same minimum strength so entering
  // aim already presents a stable parabola. Never infer a second charge timer
  // on the host frame rate.
  std::uint32_t current_weapon{};
  std::uint32_t aim_mode{};
  std::uint32_t gameplay_frame{};
  std::uint32_t charge_frame{};
  std::uint32_t player{};
  std::uint8_t grenade_input_pending{};
  std::uint8_t projectile_ready{};
  if (!runtime_.read32(profile.current_weapon, current_weapon) ||
      !runtime_.read32(profile.aim_mode, aim_mode) ||
      !runtime_.read32(profile.gameplay_frame, gameplay_frame) ||
      !runtime_.read32(profile.grenade_charge_frame, charge_frame) ||
      !runtime_.read8(profile.grenade_input_pending, grenade_input_pending) ||
      !runtime_.read32(profile.player_pointer, player)) {
    return std::nullopt;
  }
  state.grenade_input_ready = grenade_input_pending != 0U;
  const auto grenade_weapon = current_weapon == 19U || current_weapon == 20U;
  const auto charge_held = (state.pad.buttons & 0x0080U) != 0U;
  if (grenade_weapon && aim_mode != 0U && !state.thrown_projectile &&
      readable_ram_pointer(projectile_descriptor, 1U) &&
      runtime_.read8(projectile_descriptor, projectile_ready) &&
      projectile_ready != 0U && readable_ram_pointer(player, 0x0cU)) {
    std::uint32_t player_display{};
    std::uint32_t part_matrices{};
    std::uint32_t hand_matrix{};
    LegacyGrenadeTrajectoryBridgeState trajectory;
    std::int32_t hand_y{};
    const auto coherent_origin =
        runtime_.read32(player + 8U, player_display) &&
        readable_ram_pointer(player_display, 0x1cU) &&
        runtime_.read32(player_display + 0x18U, part_matrices) &&
        readable_ram_pointer(part_matrices, 0x24U) &&
        runtime_.read32(part_matrices + 0x20U, hand_matrix) &&
        readable_ram_pointer(hand_matrix, 0x20U) &&
        read_signed32(hand_matrix + 0x14U, trajectory.origin.x) &&
        read_signed32(hand_matrix + 0x18U, hand_y) &&
        read_signed32(hand_matrix + 0x1cU, trajectory.origin.z) &&
        hand_y != std::numeric_limits<std::int32_t>::min() &&
        read_signed32(profile.aim_target, trajectory.target.x) &&
        read_signed32(profile.aim_target + 4U, trajectory.target.y) &&
        read_signed32(profile.aim_target + 8U, trajectory.target.z);
    if (coherent_origin) {
      trajectory.origin.y = -hand_y;
      constexpr std::uint32_t minimum_strength = 0x28fU;
      constexpr std::uint32_t maximum_strength = 0xcccU;
      constexpr std::uint32_t full_charge_frames = 0x78U;
      const auto charge_active = charge_held && grenade_input_pending == 0U;
      const auto elapsed =
          charge_active
              ? static_cast<std::uint64_t>(gameplay_frame - charge_frame)
              : 0U;
      const auto scaled = elapsed * 0x1000U / full_charge_frames;
      trajectory.strength_q12 =
          static_cast<std::uint16_t>(std::clamp<std::uint64_t>(
              scaled, minimum_strength, maximum_strength));
      if (trajectory.origin != trajectory.target) {
        state.grenade_trajectory = trajectory;
      }
    }
  }
  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::camera_packets;
  std::uint32_t camera_controller{};
  std::uint32_t camera_object{};
  std::uint32_t camera_matrix{};
  if (!runtime_.read32(profile.camera_controller_pointer, camera_controller) ||
      camera_controller == 0U ||
      !runtime_.read32(camera_controller, camera_object) ||
      camera_object == 0U) {
    return std::nullopt;
  }
  const auto fog_dqa = address(camera_object, profile.renderer_fog_dqa_offset);
  const auto fog_dqb = address(camera_object, profile.renderer_fog_dqb_offset);
  const auto renderer_flags =
      address(camera_object, profile.renderer_flags_offset);
  const auto renderer_sprite_fast_path =
      address(camera_object, profile.renderer_sprite_fast_path_offset);
  std::uint16_t renderer_flags_value{};
  std::uint16_t renderer_display_flags_value{};
  std::uint8_t renderer_sprite_fast_path_value{};
  std::uint8_t screen_filter_enabled_value{};
  std::uint32_t screen_filter_descriptor{};
  std::uint32_t nightvision_clear_reference{};
  std::uint32_t nightvision_clear_color{};
  if (!read_rgb(camera_object, profile.renderer_clear_rgb_offset,
                state.environment.clear_color) ||
      !read_rgb(camera_object, profile.renderer_back_rgb_offset,
                state.environment.back_color) ||
      !read_rgb(camera_object, profile.renderer_fog_rgb_offset,
                state.environment.fog_color) ||
      !fog_dqa || !fog_dqb || !renderer_flags || !renderer_sprite_fast_path ||
      !read_signed32(*fog_dqa, state.environment.fog_dqa) ||
      !read_signed32(*fog_dqb, state.environment.fog_dqb) ||
      !runtime_.read32(profile.active_terrain_depth_cue,
                       state.environment.active_terrain_depth_cue) ||
      !runtime_.read32(profile.terrain_depth_cue,
                       state.environment.terrain_depth_cue) ||
      !runtime_.read16(profile.renderer_display_flags,
                       renderer_display_flags_value) ||
      !runtime_.read16(*renderer_flags, renderer_flags_value) ||
      !runtime_.read8(*renderer_sprite_fast_path,
                      renderer_sprite_fast_path_value) ||
      !runtime_.read8(profile.screen_filter_enabled,
                      screen_filter_enabled_value) ||
      !runtime_.read32(profile.screen_filter_descriptor,
                       screen_filter_descriptor) ||
      !runtime_.read32(profile.nightvision_clear_reference,
                       nightvision_clear_reference) ||
      !runtime_.read32(profile.nightvision_clear_color,
                       nightvision_clear_color) ||
      renderer_sprite_fast_path_value > 1U ||
      (state.environment.active_terrain_depth_cue >> 16U) > 15U ||
      (state.environment.terrain_depth_cue >> 16U) > 15U ||
      state.environment.fog_dqa < std::numeric_limits<std::int16_t>::min() ||
      state.environment.fog_dqa > std::numeric_limits<std::int16_t>::max()) {
    return std::nullopt;
  }
  state.environment.renderer_flags = renderer_flags_value;
  state.environment.background_enabled = (renderer_flags_value & 1U) != 0U;
  state.environment.renderer_display_flags = renderer_display_flags_value;
  state.environment.renderer_darkness_enabled =
      renderer_sprite_fast_path_value == 0U &&
      (renderer_display_flags_value & 1U) != 0U;
  state.environment.nightvision_enabled = (renderer_flags_value & 0x10U) != 0U;
  state.environment.nightvision_clear_override_enabled =
      state.environment.nightvision_enabled &&
      nightvision_clear_color == nightvision_clear_reference;
  state.environment.nightvision_clear_color = LegacyRgbBridgeState{
      static_cast<std::uint8_t>(nightvision_clear_color),
      static_cast<std::uint8_t>(nightvision_clear_color >> 8U),
      static_cast<std::uint8_t>(nightvision_clear_color >> 16U),
  };
  if (screen_filter_enabled_value != 0U) {
    if (screen_filter_descriptor == 0U ||
        !readable_ram_pointer(screen_filter_descriptor, 0x1bU) ||
        !runtime_.read32(screen_filter_descriptor + 0x14U,
                         state.environment.screen_filter_material) ||
        !read_rgb(screen_filter_descriptor, 0x18U,
                  state.environment.screen_filter_color)) {
      return std::nullopt;
    }
    // FUN_800c9140 emits the scene-covering TILE only for materials 0..3.
    // Selector four is a draw-mode/object-only secondary path and returns
    // before constructing the TILE.
    state.environment.screen_filter_enabled =
        state.environment.screen_filter_material != 4U;
    if (state.environment.nightvision_enabled) {
      const auto maximum =
          std::max({state.environment.screen_filter_color.red,
                    state.environment.screen_filter_color.green,
                    state.environment.screen_filter_color.blue});
      state.environment.screen_filter_color = {0U, maximum, 0U};
    }
  }
  state.renderer_sprite_fast_path = renderer_sprite_fast_path_value != 0U;

  const auto &scrim = profile.scrim;
  if (scrim.enabled) {
    std::uint8_t resource_state{};
    std::uint32_t copy_state{};
    std::uint32_t model_instance{};
    if (scrim.copy_node_count != 13U || scrim.copy_node_stride != 0x20U) {
      return std::nullopt;
    }
    for (const auto &[validation_address, expected] : scrim.validation_words) {
      std::uint32_t instruction{};
      if (!runtime_.read32(validation_address, instruction) ||
          instruction != expected) {
        return std::nullopt;
      }
    }
    if (!runtime_.read8(scrim.resource_state, resource_state) ||
        !runtime_.read32(scrim.copy_state, copy_state) ||
        !runtime_.read32(scrim.model_instance, model_instance)) {
      return std::nullopt;
    }
    const auto signed_resource_state =
        std::bit_cast<std::int8_t>(resource_state);
    if (signed_resource_state < -1) {
      return std::nullopt;
    }
    state.scrim.resource_present = signed_resource_state != -1;
    state.scrim.visible = state.scrim.resource_present &&
                          signed_resource_state > 0 && model_instance != 0U;
    const auto signed_copy_state = std::bit_cast<std::int32_t>(copy_state);
    state.scrim.vram_moves_active = signed_copy_state > 0;
    if ((state.scrim.resource_present && model_instance == 0U) ||
        (!state.scrim.resource_present &&
         (model_instance != 0U || copy_state != 0U))) {
      return std::nullopt;
    }
    if (state.scrim.visible) {
      const auto coordinate_address = address(model_instance, 0x0cU);
      std::uint32_t coordinate{};
      std::uint32_t parent{};
      if (!coordinate_address ||
          !runtime_.read32(*coordinate_address, coordinate) ||
          !readable_ram_pointer(coordinate, 0x24U) ||
          !read_matrix(coordinate, state.scrim.transform) ||
          !runtime_.read32(coordinate + 0x20U, parent) || parent != 0U) {
        return std::nullopt;
      }
      state.scrim.transform_valid = true;
    }
    if (copy_state != 0U) {
      state.scrim.vram_moves.reserve(scrim.copy_node_count);
      for (std::uint32_t index = 0U; index < scrim.copy_node_count; ++index) {
        const auto node64 =
            static_cast<std::uint64_t>(scrim.copy_nodes) +
            static_cast<std::uint64_t>(index) * scrim.copy_node_stride;
        if (node64 > std::numeric_limits<std::uint32_t>::max()) {
          return std::nullopt;
        }
        const auto node = static_cast<std::uint32_t>(node64);
        std::uint32_t tag{};
        std::uint32_t cache_command{};
        std::uint32_t opcode{};
        std::uint32_t source{};
        std::uint32_t destination{};
        std::uint32_t extent{};
        if (!runtime_.read32(node + 0x08U, tag) ||
            !runtime_.read32(node + 0x0cU, cache_command) ||
            !runtime_.read32(node + 0x10U, opcode) ||
            !runtime_.read32(node + 0x14U, source) ||
            !runtime_.read32(node + 0x18U, destination) ||
            !runtime_.read32(node + 0x1cU, extent) || (tag >> 24U) != 5U ||
            cache_command != 0x01000000U || opcode != 0x80000000U) {
          return std::nullopt;
        }
        const auto half = [](std::uint32_t word, unsigned int shift) {
          return std::bit_cast<std::int16_t>(
              static_cast<std::uint16_t>(word >> shift));
        };
        LegacyVramMoveBridgeState move{
            half(source, 0U),  half(source, 16U),     half(extent, 0U),
            half(extent, 16U), half(destination, 0U), half(destination, 16U),
        };
        if (move.source_x < 0 || move.source_y < 0 || move.width <= 0 ||
            move.height <= 0 || move.destination_x < 0 ||
            move.destination_y < 0 || move.source_x + move.width > 1024 ||
            move.destination_x + move.width > 1024 ||
            move.source_y + move.height > 512 ||
            move.destination_y + move.height > 512) {
          return std::nullopt;
        }
        state.scrim.vram_moves.push_back(move);
      }
    }
  }

  // FUN_800c84f4 consumes three intrusive {item, opaque, next} lists from the
  // camera render context. Preserve their projected retail packets verbatim;
  // this includes glass fragments, weapon sprites, weather and overlay-owned
  // effects which have no stable world-space reconstruction on the host.
  constexpr std::uint32_t guest_list_next_offset = 8U;
  const auto read_list = [&](std::uint32_t head, std::uint32_t maximum,
                             const auto &read_item) {
    std::vector<std::uint32_t> visited;
    visited.reserve(maximum);
    for (auto node = head; node != 0U;) {
      if (visited.size() == maximum ||
          std::ranges::find(visited, node) != visited.end() ||
          !readable_ram_pointer(node, 12U)) {
        return false;
      }
      visited.push_back(node);
      std::uint32_t item{};
      std::uint32_t next{};
      if (!runtime_.read32(node, item) || item == 0U ||
          !runtime_.read32(node + guest_list_next_offset, next) ||
          !read_item(item)) {
        return false;
      }
      node = next;
    }
    return true;
  };
  const auto sprite_head_address =
      address(camera_object, profile.renderer_sprite_list_offset);
  const auto line_head_address =
      address(camera_object, profile.renderer_line_list_offset);
  const auto raw_head_address =
      address(camera_object, profile.renderer_raw_packet_list_offset);
  std::uint32_t sprite_head{};
  std::uint32_t line_head{};
  std::uint32_t raw_head{};
  std::uint32_t interface_renderer{};
  std::uint32_t interface_sprite_head{};
  std::uint32_t interface_raw_head{};
  std::uint32_t retail_scope_vertical_sprites{};
  std::uint32_t retail_scope_horizontal_sprites{};
  std::uint8_t interface_sprite_fast_path{};
  if (!sprite_head_address || !line_head_address || !raw_head_address ||
      profile.maximum_guest_sprites == 0U ||
      profile.maximum_guest_lines == 0U ||
      profile.maximum_guest_raw_packets == 0U ||
      !runtime_.read32(*sprite_head_address, sprite_head) ||
      !runtime_.read32(*line_head_address, line_head) ||
      !runtime_.read32(*raw_head_address, raw_head) ||
      !runtime_.read32(profile.interface_renderer_pointer,
                       interface_renderer) ||
      !runtime_.read32(profile.retail_scope_vertical_sprites_pointer,
                       retail_scope_vertical_sprites) ||
      !runtime_.read32(profile.retail_scope_horizontal_sprites_pointer,
                       retail_scope_horizontal_sprites)) {
    return std::nullopt;
  }
  if (interface_renderer != 0U) {
    const auto interface_raw_head_address =
        address(interface_renderer, profile.renderer_raw_packet_list_offset);
    if (!interface_raw_head_address ||
        !runtime_.read32(*interface_raw_head_address, interface_raw_head)) {
      return std::nullopt;
    }
    // The interface context's sprite fields are not initialized during the
    // mission bootstrap. They become authoritative only once retail enables
    // the SVD camera pass; raw optic packets remain valid independently.
    if (state.environment.nightvision_enabled) {
      const auto interface_sprite_head_address =
          address(interface_renderer, profile.renderer_sprite_list_offset);
      const auto interface_fast_path_address =
          address(interface_renderer, profile.renderer_sprite_fast_path_offset);
      if (!interface_sprite_head_address || !interface_fast_path_address ||
          !runtime_.read32(*interface_sprite_head_address,
                           interface_sprite_head) ||
          !runtime_.read8(*interface_fast_path_address,
                          interface_sprite_fast_path)) {
        return std::nullopt;
      }
    }
  }

  state.guest_sprites.reserve(profile.maximum_guest_sprites +
                              legacy_retail_scope_vertical_sprite_count +
                              legacy_retail_scope_horizontal_sprite_count);
  const auto read_sprite = [&](std::uint32_t item, bool renderer_fast_path,
                               bool retail_scope_overlay) {
    constexpr std::uint32_t sprite_size = 0x24U;
    constexpr std::uint32_t effect_particle_stride = 0x68U;
    constexpr std::uint32_t effect_particle_sprite_offset = 0x28U;
    if (!readable_ram_pointer(item, sprite_size + 4U)) {
      return false;
    }
    LegacyGuestSpriteBridgeState sprite;
    sprite.source_address = item;
    std::uint16_t x{};
    std::uint16_t y{};
    std::uint16_t mapping_x{};
    std::uint16_t mapping_y{};
    std::uint16_t scale_x{};
    std::uint16_t scale_y{};
    std::uint32_t rotation{};
    if (!runtime_.read32(item, sprite.attribute) ||
        !runtime_.read16(item + 0x04U, x) ||
        !runtime_.read16(item + 0x06U, y) ||
        !runtime_.read16(item + 0x08U, sprite.width) ||
        !runtime_.read16(item + 0x0aU, sprite.height) ||
        !runtime_.read16(item + 0x0cU, sprite.tpage) ||
        !runtime_.read8(item + 0x0eU, sprite.u) ||
        !runtime_.read8(item + 0x0fU, sprite.v) ||
        !runtime_.read16(item + 0x10U, sprite.center_x) ||
        !runtime_.read16(item + 0x12U, sprite.center_y) ||
        !runtime_.read8(item + 0x14U, sprite.color.red) ||
        !runtime_.read8(item + 0x15U, sprite.color.green) ||
        !runtime_.read8(item + 0x16U, sprite.color.blue) ||
        !runtime_.read16(item + 0x18U, mapping_x) ||
        !runtime_.read16(item + 0x1aU, mapping_y) ||
        !runtime_.read16(item + 0x1cU, scale_x) ||
        !runtime_.read16(item + 0x1eU, scale_y) ||
        !runtime_.read32(item + 0x20U, rotation) ||
        !runtime_.read32(item + sprite_size, sprite.ordering_depth)) {
      return false;
    }
    sprite.x = std::bit_cast<std::int16_t>(x);
    sprite.y = std::bit_cast<std::int16_t>(y);
    sprite.mapping_x = std::bit_cast<std::int16_t>(mapping_x);
    sprite.mapping_y = std::bit_cast<std::int16_t>(mapping_y);
    sprite.scale_x = std::bit_cast<std::int16_t>(scale_x);
    sprite.scale_y = std::bit_cast<std::int16_t>(scale_y);
    sprite.rotation = std::bit_cast<std::int32_t>(rotation);
    sprite.renderer_fast_path = renderer_fast_path;
    sprite.retail_scope_overlay = retail_scope_overlay;
    const auto first_particle_sprite =
        address(profile.effect_particle_pool, effect_particle_sprite_offset);
    if (first_particle_sprite && item >= *first_particle_sprite) {
      const auto delta = item - *first_particle_sprite;
      const auto particle = delta / effect_particle_stride;
      if (delta % effect_particle_stride == 0U &&
          particle < profile.effect_particle_capacity &&
          particle <= static_cast<std::uint32_t>(
                          std::numeric_limits<std::int16_t>::max())) {
        sprite.effect_particle = static_cast<std::int16_t>(particle);
      }
    }
    // Retail itself skips non-GPU texture pages at this list boundary.
    if (sprite.tpage < 0x20U) {
      state.guest_sprites.push_back(sprite);
    }
    return true;
  };
  const auto read_camera_sprite = [&](std::uint32_t item) {
    return read_sprite(item, renderer_sprite_fast_path_value != 0U, false);
  };
  if (!read_list(sprite_head, profile.maximum_guest_sprites,
                 read_camera_sprite)) {
    return std::nullopt;
  }
  const auto read_interface_scope_sprite = [&](std::uint32_t item) {
    if (!legacyGuestSpriteIsRetailScopeOverlayAddress(
            item, retail_scope_vertical_sprites,
            retail_scope_horizontal_sprites)) {
      return true;
    }
    return read_sprite(item, interface_sprite_fast_path == 1U, true);
  };
  state.guest_lines.reserve(profile.maximum_guest_lines);
  const auto read_line = [&](std::uint32_t item) {
    if (!readable_ram_pointer(item, 0x14U)) {
      return false;
    }
    LegacyGuestLineBridgeState line;
    std::uint16_t x0{};
    std::uint16_t y0{};
    std::uint16_t x1{};
    std::uint16_t y1{};
    if (!runtime_.read32(item, line.attribute) ||
        !runtime_.read16(item + 0x04U, x0) ||
        !runtime_.read16(item + 0x06U, y0) ||
        !runtime_.read16(item + 0x08U, x1) ||
        !runtime_.read16(item + 0x0aU, y1) ||
        !runtime_.read8(item + 0x0cU, line.first_color.red) ||
        !runtime_.read8(item + 0x0dU, line.first_color.green) ||
        !runtime_.read8(item + 0x0eU, line.first_color.blue) ||
        !runtime_.read8(item + 0x0fU, line.second_color.red) ||
        !runtime_.read8(item + 0x10U, line.second_color.green) ||
        !runtime_.read8(item + 0x11U, line.second_color.blue)) {
      return false;
    }
    line.first = {std::bit_cast<std::int16_t>(x0),
                  std::bit_cast<std::int16_t>(y0)};
    line.second = {std::bit_cast<std::int16_t>(x1),
                   std::bit_cast<std::int16_t>(y1)};
    state.guest_lines.push_back(line);
    return true;
  };
  if (!read_list(line_head, profile.maximum_guest_lines, read_line)) {
    return std::nullopt;
  }
  state.guest_raw_packets.reserve(profile.maximum_guest_raw_packets);
  const auto read_raw_packet = [&](std::uint32_t item) {
    constexpr std::uint32_t effect_particle_stride = 0x68U;
    constexpr std::uint32_t effect_particle_packet_offset = 0x28U;
    if (!readable_ram_pointer(item, 0x10U)) {
      return false;
    }
    LegacyGuestRawPacketBridgeState packet;
    packet.source_address = item;
    std::uint32_t tag{};
    if (!runtime_.read32(item + 0x04U, packet.ordering_depth) ||
        !runtime_.read32(item + 0x08U, tag) ||
        !runtime_.read32(item + 0x0cU, packet.words[0])) {
      return false;
    }
    packet.word_count = static_cast<std::uint8_t>(tag >> 24U);
    packet.opcode = static_cast<std::uint8_t>(packet.words[0] >> 24U);
    const auto first_particle_packet =
        address(profile.effect_particle_pool, effect_particle_packet_offset);
    if (first_particle_packet && item >= *first_particle_packet) {
      const auto delta = item - *first_particle_packet;
      const auto particle = delta / effect_particle_stride;
      if (delta % effect_particle_stride == 0U &&
          particle < profile.effect_particle_capacity &&
          particle <= static_cast<std::uint32_t>(
                          std::numeric_limits<std::int16_t>::max())) {
        packet.effect_particle = static_cast<std::int16_t>(particle);
      }
    }
    const auto base_opcode = static_cast<std::uint8_t>(packet.opcode & 0xfdU);
    const auto accepted = (packet.word_count == 2U && base_opcode == 0x68U) ||
                          (packet.word_count == 3U && base_opcode == 0x40U) ||
                          (packet.word_count == 4U &&
                           (base_opcode == 0x20U || base_opcode == 0x50U)) ||
                          (packet.word_count == 5U && base_opcode == 0x28U) ||
                          (packet.word_count == 6U && base_opcode == 0x30U);
    // Match retail: unsupported raw nodes remain linked but are not sorted.
    if (!accepted || packet.opcode == 0U || (packet.opcode & 0x80U) != 0U) {
      return true;
    }
    if (!readable_ram_pointer(item + 0x0cU,
                              static_cast<std::size_t>(packet.word_count) *
                                  sizeof(std::uint32_t))) {
      return false;
    }
    for (std::size_t word = 1U; word < packet.word_count; ++word) {
      if (!runtime_.read32(item + 0x0cU + static_cast<std::uint32_t>(word * 4U),
                           packet.words[word])) {
        return false;
      }
    }
    if (state.guest_raw_packets.size() >= profile.maximum_guest_raw_packets) {
      return false;
    }
    state.guest_raw_packets.push_back(packet);
    return true;
  };
  if (!read_list(raw_head, profile.maximum_guest_raw_packets,
                 read_raw_packet)) {
    return std::nullopt;
  }
  // Modes 2..4 are the three retail first-person optics. The interface packet
  // pool keeps stale non-zero links outside those modes, so link state alone
  // cannot identify an active scope overlay.
  const auto optic_active = aim_mode >= 2U && aim_mode <= 4U;
  const auto packet_matches_optic_mode =
      [aim_mode](const LegacyGuestRawPacketBridgeState &packet) {
        return aim_mode == 4U
                   ? legacyGuestRawPacketIsVirusScannerOverlay(packet)
                   : legacyGuestRawPacketIsRetailScopeOverlay(packet);
      };
  const auto read_interface_scope_packet = [&](std::uint32_t item) {
    const auto candidate = LegacyGuestRawPacketBridgeState{
        .source_address = item,
    };
    return !packet_matches_optic_mode(candidate) || read_raw_packet(item);
  };
  // The interface context is normally distinct from the world camera. Avoid
  // reading the same intrusive list twice on exceptional/loading frames where
  // both globals temporarily identify the same object. Only the fixed optic
  // sprite/raw arrays cross this boundary; the rest of retail UI is already
  // represented by the dedicated immutable UI bridge and would otherwise be
  // drawn twice.
  if (optic_active && interface_renderer != 0U &&
      interface_renderer != camera_object &&
      ((state.environment.nightvision_enabled &&
        !read_list(interface_sprite_head, profile.maximum_guest_sprites,
                   read_interface_scope_sprite)) ||
       !read_list(interface_raw_head, profile.maximum_guest_raw_packets,
                  read_interface_scope_packet))) {
    return std::nullopt;
  }
  // Fixed optic packets are intrusive nodes. A transient or partially
  // reconstructed interface list must not drop authored scope geometry: scan
  // the known arrays as a fallback, accept only linked entries, and dedupe the
  // normal list traversal. This is also what preserves the six semitransparent
  // mode-2 POLY_F4 dim quads at 0x8011c5b8..0x8011c66c.
  if (optic_active && interface_renderer != 0U) {
    const auto read_active_fixed_optic_range = [&](const auto &range) {
      for (std::uint32_t index = 0U; index < range.count; ++index) {
        const auto item = range.begin + index * range.stride;
        std::uint32_t link{};
        if (!runtime_.read32(item, link)) {
          return false;
        }
        if (link == 0U || std::ranges::any_of(state.guest_raw_packets,
                                              [item](const auto &packet) {
                                                return packet.source_address ==
                                                       item;
                                              })) {
          continue;
        }
        if (!read_raw_packet(item)) {
          return false;
        }
      }
      return true;
    };
    const auto fixed_ranges_valid =
        aim_mode == 4U
            ? read_active_fixed_optic_range(
                  legacy_virus_scanner_line_packets) &&
                  read_active_fixed_optic_range(
                      legacy_virus_scanner_target_dot_packets)
            : read_active_fixed_optic_range(legacy_retail_scope_line_packets) &&
                  read_active_fixed_optic_range(
                      legacy_retail_scope_quad_packets) &&
                  read_active_fixed_optic_range(
                      legacy_retail_scope_triangle_packets);
    if (!fixed_ranges_valid) {
      return std::nullopt;
    }
  }
  state.guest_camera_lists_captured = true;
  const auto camera_projection =
      address(camera_object, camera_projection_offset);
  std::uint16_t projection{};
  if (!camera_projection || !runtime_.read16(*camera_projection, projection) ||
      !runtime_.read32(camera_object, camera_matrix) || camera_matrix == 0U ||
      !read_matrix_point(camera_matrix, state.camera.eye)) {
    return std::nullopt;
  }
  state.camera.projection = projection;
  const auto target_x = address(camera_controller, camera_target_x_offset);
  const auto target_y = address(camera_controller, camera_target_y_offset);
  const auto target_z = address(camera_controller, camera_target_z_offset);
  const auto fov = address(camera_controller, camera_fov_offset);
  std::int32_t guest_target_y{};
  std::uint32_t camera_mode_bits{};
  std::uint32_t camera_lock{};
  if (!target_x || !target_y || !target_z || !fov ||
      !read_signed32(*target_x, state.camera.target.x) ||
      !read_signed32(*target_y, guest_target_y) ||
      !read_signed32(*target_z, state.camera.target.z) ||
      !read_signed32(*fov, state.camera.fov_raw) ||
      !runtime_.read32(profile.camera_mode, camera_mode_bits) ||
      !runtime_.read32(profile.camera_lock, camera_lock) ||
      guest_target_y == std::numeric_limits<std::int32_t>::min()) {
    return std::nullopt;
  }
  state.camera.target.y = -guest_target_y;
  state.camera.mode = std::bit_cast<std::int32_t>(camera_mode_bits);
  state.camera.scripted = state.camera.mode == 0x0b;
  state.camera.locked = camera_lock != 0U;

  std::uint32_t presentation_viewport{};
  if (!runtime_.read32(profile.presentation_viewport_pointer,
                       presentation_viewport) ||
      !readable_ram_pointer(presentation_viewport, 8U)) {
    return std::nullopt;
  }
  const auto presentation_viewport_y =
      address(presentation_viewport, profile.presentation_viewport_y_offset);
  const auto presentation_viewport_height = address(
      presentation_viewport, profile.presentation_viewport_height_offset);
  if (!presentation_viewport_y || !presentation_viewport_height ||
      !read_signed16(*presentation_viewport_y,
                     state.camera.presentation_viewport_y) ||
      !read_signed16(*presentation_viewport_height,
                     state.camera.presentation_viewport_height) ||
      state.camera.presentation_viewport_height <= 0 ||
      state.camera.presentation_viewport_height > 240) {
    return std::nullopt;
  }
  // The display swap retargets this RECT between framebuffer pages by adding
  // or subtracting 240 from y. Publish logical screen-space y; treating the
  // physical page offset as a bar height blacks out every other guest frame.
  constexpr auto retail_framebuffer_height = std::int16_t{240};
  if (state.camera.presentation_viewport_y >= retail_framebuffer_height) {
    state.camera.presentation_viewport_y -= retail_framebuffer_height;
  } else if (state.camera.presentation_viewport_y < 0) {
    state.camera.presentation_viewport_y += retail_framebuffer_height;
  }
  if (state.camera.presentation_viewport_y < 0 ||
      state.camera.presentation_viewport_y > 40) {
    return std::nullopt;
  }
  state.camera.retail_letterbox_active =
      state.camera.presentation_viewport_height < 240;

  std::uint16_t fade_step{};
  std::uint8_t fade_initialized{};
  if (!runtime_.read16(profile.fade_step, fade_step) ||
      !runtime_.read16(profile.fade_current, state.fade.current) ||
      !runtime_.read32(profile.fade_callback, state.fade.callback) ||
      !runtime_.read8(profile.fade_initialized, fade_initialized) ||
      !runtime_.read8(profile.fade_floor_rgb, state.fade.floor)) {
    return std::nullopt;
  }
  state.fade.step = std::bit_cast<std::int16_t>(fade_step);
  state.fade.initialized = fade_initialized != 0U;

  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::objects;
  std::uint32_t object_records{};
  std::uint32_t object_definitions{};
  std::uint32_t object_count_bits{};
  std::uint32_t object_definition_count_bits{};
  if (!runtime_.read32(profile.object_records_pointer, object_records) ||
      !runtime_.read32(profile.object_definitions_pointer,
                       object_definitions) ||
      !runtime_.read32(profile.object_count, object_count_bits) ||
      !runtime_.read32(profile.object_definition_count,
                       object_definition_count_bits)) {
    return std::nullopt;
  }
  const auto object_count = std::bit_cast<std::int32_t>(object_count_bits);
  const auto object_definition_count =
      std::bit_cast<std::int32_t>(object_definition_count_bits);
  if (object_count < 0 ||
      static_cast<std::uint32_t>(object_count) > profile.maximum_objects ||
      object_definition_count < 0 ||
      static_cast<std::uint32_t>(object_definition_count) >
          profile.maximum_definitions ||
      state.dynamic_first_slot > static_cast<std::uint32_t>(object_count) ||
      (object_count != 0 && object_records == 0U) ||
      (object_definition_count != 0 && object_definitions == 0U)) {
    return std::nullopt;
  }
  const auto virus_scanner_target_coordinates_valid =
      state.virus_scanner_target.x != 0 || state.virus_scanner_target.y != 0 ||
      state.virus_scanner_target.z != 0;
  if (virus_scanner_target_coordinates_valid &&
      virus_scanner_target_slot >= 0 &&
      virus_scanner_target_slot < object_count) {
    state.virus_scanner_target_slot = virus_scanner_target_slot;
    state.virus_scanner_target_valid = true;
  }
  // FUN_8003ce88 clears only the three coordinates. The slot word is stale
  // after leaving scanner range and must never keep a carrier selected alone.
  const auto read_hmd_wound_vertices = [this, &address, &readable_ram_pointer,
                                        &profile](
                                           LegacyObjectBridgeState &object) {
    object.hmd_wound_vertex_count = 0U;
    if (!object.resident || object.root_node == 0U) {
      return;
    }

    constexpr std::uint32_t display_model_offset = 0x10U;
    constexpr std::uint32_t model_payload_offset = 0x20U;
    constexpr std::uint32_t hmd_wound_table_offset = 0x10U;
    constexpr std::uint32_t hmd_geometry_end_offset = 0x14U;
    constexpr std::uint32_t hmd_header_size = 0x24U;
    constexpr std::uint32_t hmd_part_header_size = 0x44U;
    constexpr std::uint32_t hmd_vertex_size = 8U;
    constexpr std::uint32_t wound_record_stride = 0x3cU;
    constexpr std::uint32_t wound_record_node_offset = 0U;
    constexpr std::uint32_t wound_record_count_offset = 0x0cU;
    constexpr std::uint32_t wound_record_vertices_offset = 0x10U;
    constexpr std::uint32_t maximum_hmd_parts = 256U;

    const auto model_address = address(object.root_node, display_model_offset);
    std::uint32_t model{};
    if (!model_address || !runtime_.read32(*model_address, model) ||
        model == 0U) {
      return;
    }
    const auto payload_address = address(model, model_payload_offset);
    std::uint32_t payload{};
    if (!payload_address || !runtime_.read32(*payload_address, payload) ||
        !readable_ram_pointer(payload, hmd_header_size)) {
      return;
    }

    std::uint32_t flags{};
    std::uint32_t part_count{};
    std::uint32_t triangle_words{};
    std::uint32_t geometry_end{};
    const auto geometry_end_address = address(payload, hmd_geometry_end_offset);
    if (!geometry_end_address || !runtime_.read32(payload, flags) ||
        !runtime_.read32(payload + 4U, part_count) ||
        !runtime_.read32(payload + 8U, triangle_words) ||
        !runtime_.read32(*geometry_end_address, geometry_end) ||
        (flags & ~1U) != 0x48000000U || part_count == 0U ||
        part_count > maximum_hmd_parts || triangle_words == 0U ||
        (triangle_words & 3U) != 0U) {
      return;
    }

    std::uint32_t wound_table{};
    if (object.class_id == 0) {
      wound_table = profile.player_hmd_wound_table;
    } else {
      const auto table_address = address(payload, hmd_wound_table_offset);
      if (!table_address || !runtime_.read32(*table_address, wound_table)) {
        return;
      }
    }
    if (!readable_ram_pointer(wound_table, 8U)) {
      return;
    }

    std::uint32_t record_count{};
    std::uint32_t records{};
    if (!runtime_.read32(wound_table, record_count) ||
        !runtime_.read32(wound_table + 4U, records) || record_count == 0U ||
        record_count > profile.maximum_objects) {
      return;
    }
    const auto record_bytes =
        static_cast<std::uint64_t>(record_count) * wound_record_stride;
    if (record_bytes > std::numeric_limits<std::size_t>::max() ||
        !readable_ram_pointer(records,
                              static_cast<std::size_t>(record_bytes))) {
      return;
    }

    auto matching_record = std::uint32_t{};
    for (std::uint32_t index = 0U; index < record_count; ++index) {
      const auto record = address(records, static_cast<std::uint64_t>(index) *
                                               wound_record_stride);
      std::uint32_t record_node{};
      if (!record ||
          !runtime_.read32(*record + wound_record_node_offset, record_node)) {
        return;
      }
      if (record_node == object.root_node) {
        matching_record = *record;
        break;
      }
    }
    if (matching_record == 0U) {
      return;
    }

    std::uint32_t wound_count{};
    if (!runtime_.read32(matching_record + wound_record_count_offset,
                         wound_count) ||
        wound_count > legacy_hmd_wound_vertex_capacity) {
      return;
    }
    std::array<std::uint32_t, legacy_hmd_wound_vertex_capacity>
        wound_pointers{};
    for (std::uint32_t index = 0U; index < wound_count; ++index) {
      if (!runtime_.read32(matching_record + wound_record_vertices_offset +
                               index * 4U,
                           wound_pointers[index])) {
        return;
      }
    }

    const auto geometry_offset =
        static_cast<std::uint64_t>(hmd_header_size) +
        static_cast<std::uint64_t>(triangle_words) * 4U;
    if (geometry_offset > geometry_end ||
        !readable_ram_pointer(payload, geometry_end)) {
      return;
    }
    const auto geometry_begin =
        static_cast<std::uint64_t>(payload) + geometry_offset;
    const auto geometry_finish =
        static_cast<std::uint64_t>(payload) + geometry_end;
    auto cursor = geometry_begin;
    auto first_vertex = std::uint64_t{};
    std::array<std::uint16_t, legacy_hmd_wound_vertex_capacity> vertices{};
    auto vertex_count = std::size_t{};

    for (std::uint32_t part = 0U; part < part_count; ++part) {
      if (cursor > geometry_finish ||
          geometry_finish - cursor < hmd_part_header_size ||
          cursor > std::numeric_limits<std::uint32_t>::max()) {
        return;
      }
      const auto part_address = static_cast<std::uint32_t>(cursor);
      std::uint32_t part_size{};
      std::uint32_t vertex_triplets{};
      std::uint32_t normal_triplets{};
      std::uint16_t declared_vertices{};
      std::uint16_t declared_normals{};
      std::uint32_t normal_offset{};
      if (!runtime_.read32(part_address, part_size) ||
          !runtime_.read32(part_address + 8U, vertex_triplets) ||
          !runtime_.read32(part_address + 0x0cU, normal_triplets) ||
          !runtime_.read16(part_address + 0x34U, declared_vertices) ||
          !runtime_.read16(part_address + 0x36U, declared_normals) ||
          !runtime_.read32(part_address + 0x40U, normal_offset)) {
        return;
      }
      const auto padded_vertices =
          static_cast<std::uint64_t>(vertex_triplets) * 3U;
      const auto padded_normals =
          static_cast<std::uint64_t>(normal_triplets) * 3U;
      const auto expected_part_size =
          static_cast<std::uint64_t>(hmd_part_header_size) +
          (padded_vertices + padded_normals) * hmd_vertex_size;
      const auto relative_part = cursor - payload;
      const auto expected_normal_offset = relative_part + hmd_part_header_size +
                                          padded_vertices * hmd_vertex_size;
      if (expected_part_size != part_size ||
          part_size > geometry_finish - cursor ||
          declared_vertices > padded_vertices ||
          declared_normals > padded_normals ||
          expected_normal_offset != normal_offset ||
          first_vertex + declared_vertices >
              std::numeric_limits<std::uint16_t>::max()) {
        return;
      }

      const auto normal_begin =
          static_cast<std::uint64_t>(payload) + normal_offset;
      // FUN_800d2bf0/FUN_800d2dd4 index this normal table with the
      // part's authored +0x34 vertex count; +0x36 is not the search bound.
      const auto normal_end =
          normal_begin +
          static_cast<std::uint64_t>(declared_vertices) * hmd_vertex_size;
      if (normal_begin < cursor || normal_end > cursor + part_size) {
        return;
      }
      for (std::uint32_t wound = 0U; wound < wound_count; ++wound) {
        const auto pointer = static_cast<std::uint64_t>(wound_pointers[wound]);
        if (pointer < normal_begin || pointer >= normal_end ||
            (pointer - normal_begin) % hmd_vertex_size != 0U) {
          continue;
        }
        const auto local_vertex = (pointer - normal_begin) / hmd_vertex_size;
        if (local_vertex >= declared_vertices) {
          continue;
        }
        const auto global_vertex =
            static_cast<std::uint16_t>(first_vertex + local_vertex);
        if (std::ranges::find(vertices.begin(), vertices.begin() + vertex_count,
                              global_vertex) ==
                vertices.begin() + vertex_count &&
            vertex_count < vertices.size()) {
          vertices[vertex_count++] = global_vertex;
        }
      }
      first_vertex += declared_vertices;
      cursor += part_size;
    }
    if (cursor != geometry_finish) {
      return;
    }
    object.hmd_wound_vertices = vertices;
    object.hmd_wound_vertex_count = static_cast<std::uint8_t>(vertex_count);
  };
  state.objects.reserve(static_cast<std::size_t>(object_count));
  for (std::uint32_t slot = 0U; slot < static_cast<std::uint32_t>(object_count);
       ++slot) {
    const auto record =
        address(object_records,
                static_cast<std::uint64_t>(slot) * object_record_stride);
    if (!record) {
      return std::nullopt;
    }
    LegacyObjectBridgeState object;
    object.slot = slot;
    const auto authored_x_address = address(*record, object_authored_x_offset);
    const auto authored_y_address = address(*record, object_authored_y_offset);
    const auto authored_z_address = address(*record, object_authored_z_offset);
    const auto attributes_address = address(*record, object_attributes_offset);
    const auto parameter_address = address(*record, object_parameter_offset);
    const auto path_pointer_address =
        address(*record, object_path_pointer_offset);
    const auto linked_slot_address =
        address(*record, object_linked_slot_offset);
    const auto instance_address = address(*record, object_instance_offset);
    const auto maximum_health_address =
        address(*record, object_maximum_health_offset);
    const auto health_address = address(*record, object_health_offset);
    if (!authored_x_address || !authored_y_address || !authored_z_address ||
        !attributes_address || !parameter_address || !path_pointer_address ||
        !linked_slot_address || !instance_address || !maximum_health_address ||
        !health_address || !runtime_.read32(*record, object.definition) ||
        !read_signed32(*authored_x_address, object.authored_position.x) ||
        !read_signed32(*authored_y_address, object.authored_position.y) ||
        !read_signed32(*authored_z_address, object.authored_position.z) ||
        !runtime_.read16(*attributes_address, object.attributes) ||
        !read_signed32(*parameter_address, object.parameter) ||
        !runtime_.read32(*path_pointer_address, object.path_pointer) ||
        !read_signed32(*linked_slot_address, object.linked_slot) ||
        !runtime_.read32(*instance_address, object.instance) ||
        !read_signed16(*maximum_health_address, object.maximum_health) ||
        !read_signed16(*health_address, object.health)) {
      return std::nullopt;
    }
    // FUN_80065fa0 retires a recycled slot by clearing only record+0x2c.
    // Its instance, display root, controller pointers, health and matrices
    // may all remain intact until the slot is populated again by
    // FUN_8005f204. Do not export those stale caches as another resident
    // object. Static records legitimately have no path, and a dying
    // dynamic object remains present until this exact retirement write.
    const auto recycled_slot = slot >= state.dynamic_first_slot;
    const auto active_lifetime = !recycled_slot || object.path_pointer != 0U;
    // A room drain may replace the definition table before clearing every
    // recycled record. Once record+0x2c is zero, every other field belongs to
    // the retired lifetime and is allowed to reference the previous overlay.
    // Preserve a readable class when it is still available, but never reject
    // an otherwise valid frame or dereference freed instance/controller state
    // for an inactive slot. Active/static records remain strictly validated.
    if (object.definition <
        static_cast<std::uint32_t>(object_definition_count)) {
      const auto definition = address(
          object_definitions, static_cast<std::uint64_t>(object.definition) *
                                  object_definition_stride);
      if (!definition || !read_signed16(*definition, object.class_id)) {
        return std::nullopt;
      }
    } else {
      if (active_lifetime) {
        return std::nullopt;
      }
      object.class_id = -1;
    }
    if (!active_lifetime) {
      state.objects.push_back(object);
      continue;
    }
    if (object.class_id < 0 || static_cast<std::uint32_t>(object.class_id) >
                                   profile.maximum_object_class) {
      return std::nullopt;
    }
    const auto handler_address =
        address(profile.object_handler_table,
                static_cast<std::uint64_t>(
                    static_cast<std::uint16_t>(object.class_id)) *
                    sizeof(std::uint32_t));
    if (!handler_address ||
        !runtime_.read32(*handler_address, object.object_handler)) {
      return std::nullopt;
    }
    if (object.instance != 0U) {
      const auto instance_flags_address =
          address(object.instance, instance_flags_offset);
      const auto node_address = address(object.instance, instance_node_offset);
      const auto motion_address =
          address(object.instance, instance_motion_offset);
      const auto presentation_address =
          address(object.instance, instance_presentation_offset);
      const auto target_address =
          address(object.instance, instance_target_offset);
      const auto health_controller_address =
          address(object.instance, instance_health_offset);
      const auto ai_address = address(object.instance, instance_ai_offset);
      if (!instance_flags_address || !node_address || !motion_address ||
          !presentation_address || !target_address ||
          !health_controller_address || !ai_address ||
          !runtime_.read8(*instance_flags_address, object.instance_flags) ||
          !runtime_.read32(*node_address, object.root_node) ||
          !runtime_.read32(*motion_address, object.motion_controller) ||
          !runtime_.read32(*presentation_address,
                           object.presentation_controller) ||
          !runtime_.read32(*target_address, object.target_controller) ||
          !runtime_.read32(*health_controller_address,
                           object.health_controller) ||
          !runtime_.read32(*ai_address, object.ai_controller)) {
        return std::nullopt;
      }
      for (std::uint32_t index = 0U; index < object.instance_state.size();
           ++index) {
        const auto state_address =
            address(object.instance, instance_state_offset + index);
        if (!state_address ||
            !runtime_.read8(*state_address, object.instance_state[index])) {
          return std::nullopt;
        }
      }
      object.resident = active_lifetime && object.root_node != 0U;
      if (object.resident) {
        if (!runtime_.read32(object.root_node, object.display_node)) {
          return std::nullopt;
        }
        const auto pose_flags_address =
            address(object.root_node, node_pose_flags_offset);
        const auto matrix_address =
            address(object.root_node, node_matrix_offset);
        std::uint32_t matrix{};
        if (!pose_flags_address || !matrix_address ||
            !runtime_.read32(*pose_flags_address, object.pose_flags) ||
            !runtime_.read32(*matrix_address, matrix) || matrix == 0U ||
            !read_matrix_point(matrix, object.position)) {
          return std::nullopt;
        }
        const auto light_state_address =
            address(object.root_node, display_light_state_offset);
        std::uint32_t light_state{};
        if (!light_state_address ||
            !runtime_.read32(*light_state_address, light_state)) {
          return std::nullopt;
        }
        if (light_state != 0U &&
            readable_ram_pointer(
                light_state + light_back_color_offset,
                static_cast<std::uint32_t>(object.hmd_back_color_q12.size() *
                                           sizeof(std::int16_t)))) {
          auto complete = true;
          for (std::uint32_t channel = 0U;
               channel < object.hmd_back_color_q12.size(); ++channel) {
            complete = complete &&
                       read_signed16(light_state + light_back_color_offset +
                                         channel * sizeof(std::int16_t),
                                     object.hmd_back_color_q12[channel]);
          }
          object.hmd_back_color_valid = complete;
        }
        read_hmd_wound_vertices(object);
        for (std::uint32_t component = 0U;
             component < object.guest_rotation.size(); ++component) {
          const auto component_address = address(matrix, component * 2U);
          if (!component_address ||
              !read_signed16(*component_address,
                             object.guest_rotation[component])) {
            return std::nullopt;
          }
        }
        if (object.class_id == 0) {
          state.player.position = object.position;
          if (object.motion_controller != 0U) {
            const auto motion_x =
                address(object.motion_controller, motion_position_x_offset);
            const auto motion_y =
                address(object.motion_controller, motion_position_y_offset);
            const auto motion_z =
                address(object.motion_controller, motion_position_z_offset);
            std::int32_t guest_motion_y{};
            if (!motion_x || !motion_y || !motion_z ||
                !read_signed32(*motion_x, state.player.position.x) ||
                !read_signed32(*motion_y, guest_motion_y) ||
                !read_signed32(*motion_z, state.player.position.z) ||
                guest_motion_y == std::numeric_limits<std::int32_t>::min()) {
              return std::nullopt;
            }
            state.player.position.y = -guest_motion_y;
          }
          state.player.guest_rotation = object.guest_rotation;
          state.player.resident = true;
        }
        {
          const auto table_address =
              address(object.root_node, display_bone_matrices_offset);
          std::uint32_t table{};
          auto complete = table_address &&
                          runtime_.read32(*table_address, table) &&
                          readable_ram_pointer(table, sizeof(std::uint32_t));
          std::array<std::uint32_t, legacy_actor_bone_count> matrix_pointers{};
          auto matrix_count = std::size_t{};
          for (std::size_t part = 0U; complete && part < matrix_pointers.size();
               ++part) {
            const auto pointer_address = address(table, part * 4U);
            if (!pointer_address ||
                !runtime_.read32(*pointer_address, matrix_pointers[part]) ||
                matrix_pointers[part] == 0U ||
                !readable_ram_pointer(matrix_pointers[part], 0x20U)) {
              break;
            }
            ++matrix_count;
          }
          complete = complete && matrix_count != 0U;
          const auto rendered_this_pass =
              (object.pose_flags & node_hmd_rendered_this_pass) != 0U;
          if (complete && !rendered_this_pass) {
            complete = profile.bone_matrix_resolver_entry != 0U;
            for (std::size_t part = 0U; complete && part < matrix_count;
                 ++part) {
              complete = resolve_matrix(resolve_matrix, matrix_pointers[part],
                                        object.bone_matrices[part]);
            }
          }
          if (rendered_this_pass) {
            for (std::size_t part = 0U; complete && part < matrix_count;
                 ++part) {
              complete = read_matrix(matrix_pointers[part],
                                     object.bone_matrices[part]);
            }
          }
          if (complete) {
            object.bone_matrix_count = static_cast<std::uint8_t>(matrix_count);
          }
          if (object.motion_controller != 0U) {
            const auto contact_address = address(
                object.motion_controller, motion_ground_contact_y_offset);
            std::uint32_t packed_contact_y{};
            if (contact_address &&
                runtime_.read32(*contact_address, packed_contact_y)) {
              // Retail motion Y values reserve their low two bits
              // for contact flags. 0x80000000 after unpacking is
              // the no-floor sentinel (commonly stored as 0x80000002).
              const auto unpacked_bits = packed_contact_y & ~3U;
              const auto guest_contact_y =
                  std::bit_cast<std::int32_t>(unpacked_bits);
              if (guest_contact_y != std::numeric_limits<std::int32_t>::min()) {
                object.ground_contact_y = -guest_contact_y;
                object.ground_contact_valid = true;
              }
            }
          }
        }
      }
      if (active_lifetime && object.presentation_controller != 0U) {
        const auto enabled_address = address(object.presentation_controller,
                                             presentation_enabled_offset);
        const auto mode_address =
            address(object.presentation_controller, presentation_mode_offset);
        if (!enabled_address || !mode_address ||
            !runtime_.read8(*enabled_address, object.presentation_enabled) ||
            !runtime_.read8(*mode_address, object.presentation_mode)) {
          return std::nullopt;
        }
      }
      if (active_lifetime && object.target_controller != 0U) {
        const auto slot_address =
            address(object.target_controller, target_slot_offset);
        const auto flags_address =
            address(object.target_controller, target_flags_offset);
        const auto meter_address =
            address(object.target_controller, target_meter_offset);
        const auto danger_address =
            address(object.target_controller, target_danger_offset);
        if (!slot_address || !flags_address || !meter_address ||
            !danger_address ||
            !read_signed16(*slot_address, object.target_slot) ||
            !runtime_.read32(*flags_address, object.target_flags) ||
            !read_signed16(*meter_address, object.target_meter) ||
            !runtime_.read32(*danger_address, object.danger_q12)) {
          return std::nullopt;
        }
        // FUN_80065fa0 leaves the target-controller allocation intact
        // after the actor has died. Its first halfword can therefore
        // contain a recycled pool value until the slot is populated
        // again; it is not a live combat target.
        object.has_target = object.health > 0 && object.target_slot >= 0 &&
                            static_cast<std::uint32_t>(object.target_slot) <
                                static_cast<std::uint32_t>(object_count);
        if (!object.has_target) {
          object.target_slot = -1;
        }
      }
      if (active_lifetime && object.ai_controller != 0U) {
        const auto ai_flags_address =
            address(object.ai_controller, ai_flags_offset);
        const auto fire_latch_address =
            address(object.ai_controller, ai_fire_latch_offset);
        const auto route_address =
            address(object.ai_controller, ai_route_node_offset);
        const auto previous_route_address =
            address(object.ai_controller, ai_previous_route_node_offset);
        const auto mode_address = address(object.ai_controller, ai_mode_offset);
        const auto archetype_address =
            address(object.ai_controller, ai_archetype_offset);
        const auto combat_address =
            address(object.ai_controller, ai_combat_mode_offset);
        const auto pool_address =
            address(object.ai_controller, ai_pool_index_offset);
        const auto state_address =
            address(object.ai_controller, ai_state_offset);
        if (!ai_flags_address || !fire_latch_address || !route_address ||
            !previous_route_address || !mode_address || !archetype_address ||
            !combat_address || !pool_address || !state_address ||
            !runtime_.read32(*ai_flags_address, object.ai_flags) ||
            !runtime_.read8(*fire_latch_address, object.ai_fire_latch) ||
            !runtime_.read8(*route_address, object.ai_route_node) ||
            !runtime_.read8(*previous_route_address,
                            object.ai_previous_route_node) ||
            !runtime_.read8(*mode_address, object.ai_mode) ||
            !runtime_.read8(*archetype_address, object.ai_archetype) ||
            !runtime_.read8(*combat_address, object.ai_combat_mode) ||
            !runtime_.read8(*pool_address, object.ai_pool_index) ||
            !runtime_.read16(*state_address, object.ai_state)) {
          return std::nullopt;
        }
        object.simulated = (object.ai_flags & 0x200U) != 0U;
        if (object.path_pointer != 0U && object.ai_route_node != 0xffU) {
          const auto route_flags_address =
              address(object.path_pointer,
                      static_cast<std::uint64_t>(object.ai_route_node) *
                              route_node_stride +
                          route_node_flags_offset);
          if (!route_flags_address ||
              !runtime_.read16(*route_flags_address, object.ai_route_flags)) {
            return std::nullopt;
          }
        }
      }
    }
    state.objects.push_back(object);
  }

  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::attached_text;
  // DAT_801160d8 is the retail text-system list. Its nodes are wrappers
  // (object at +0, next at +8); attached text objects carry flag 0x02 and the
  // guest object slot at +0x16. Only strings whose source pointer is retained
  // by retail are exported, so presentation never guesses mission labels.
  std::uint32_t text_node{};
  if (!runtime_.read32(profile.active_text_list, text_node) ||
      profile.maximum_text_nodes == 0U || profile.maximum_text_nodes > 256U ||
      profile.maximum_world_callouts > profile.maximum_text_nodes) {
    return std::nullopt;
  }
  std::vector<std::uint32_t> visited_text_nodes;
  visited_text_nodes.reserve(profile.maximum_text_nodes);
  while (text_node != 0U) {
    if (visited_text_nodes.size() >= profile.maximum_text_nodes ||
        !readable_ram_pointer(text_node, 12U) ||
        std::ranges::find(visited_text_nodes, text_node) !=
            visited_text_nodes.end()) {
      return std::nullopt;
    }
    visited_text_nodes.push_back(text_node);
    std::uint32_t text_object{};
    std::uint32_t next{};
    if (!runtime_.read32(text_node, text_object) ||
        !runtime_.read32(text_node + 8U, next) ||
        !readable_ram_pointer(text_object, 0x18U)) {
      return std::nullopt;
    }
    std::uint8_t flags{};
    std::int16_t attached_slot{-1};
    if (!runtime_.read8(text_object + 0x14U, flags) ||
        !read_signed16(text_object + 0x16U, attached_slot)) {
      return std::nullopt;
    }
    if ((flags & 0x02U) != 0U && attached_slot >= 0 &&
        static_cast<std::size_t>(attached_slot) < state.objects.size() &&
        state.world_callouts.size() < profile.maximum_world_callouts) {
      auto is_headshot = false;
      if (headshot_text_handle != 0xffffU &&
          profile.text_object_stride >= 0x18U &&
          profile.text_object_capacity != 0U &&
          text_object >= profile.text_object_pool) {
        const auto offset = text_object - profile.text_object_pool;
        const auto index = offset / profile.text_object_stride;
        std::uint8_t generation{};
        if (offset % profile.text_object_stride == 0U &&
            index < profile.text_object_capacity && index <= 0xffU &&
            runtime_.read8(text_object + 0x15U, generation)) {
          const auto handle = static_cast<std::uint16_t>(
              index | (static_cast<std::uint32_t>(generation) << 8U));
          is_headshot = handle == headshot_text_handle;
        }
      }
      std::uint8_t text_checksum{};
      const auto has_text_checksum =
          runtime_.read8(text_object + 0x15U, text_checksum);
      std::optional<std::string> text;
      const auto cached = std::ranges::find_if(
          attached_text_sources_,
          [text_object, text_checksum, has_text_checksum](const auto &entry) {
            return has_text_checksum && entry.text_object == text_object &&
                   entry.text_checksum == text_checksum;
          });
      if (cached != attached_text_sources_.end() && !cached->text.empty()) {
        text = cached->text;
      } else if (is_headshot) {
        text = read_pointer_string(profile.headshot_text_pointer);
      } else if (attached_slot == primary_story_target_slot) {
        text = read_pointer_string(profile.primary_story_text_pointer);
      } else if (attached_slot == secondary_story_target_slot) {
        text = read_pointer_string(profile.secondary_story_text_pointer);
      }
      if (text) {
        state.world_callouts.push_back(LegacyWorldCalloutBridgeState{
            attached_slot, std::move(*text), is_headshot});
      }
    }
    text_node = next;
  }

  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::effects;
  // FUN_8004c0e8 owns a fixed 160-entry effect-particle pool. Each live
  // particle belongs to one 0x34-byte controller chain. Normal animated
  // controllers store their SPFX family at controller +0x1c; only a
  // negative-state controller selects the per-particle +0x62 field.
  constexpr std::uint32_t effect_controller_stride = 0x34U;
  constexpr std::uint32_t effect_controller_state_offset = 0U;
  constexpr std::uint32_t effect_controller_flags_offset = 4U;
  constexpr std::uint32_t effect_controller_attached_slot_offset = 0x14U;
  constexpr std::uint32_t effect_controller_gravity_offset = 0x1aU;
  constexpr std::uint32_t effect_controller_family_offset = 0x1cU;
  constexpr std::uint32_t effect_controller_head_offset = 0x1eU;
  constexpr std::uint32_t effect_controller_source_offset = 0x20U;
  constexpr std::uint32_t effect_controller_scale_offset = 0x23U;
  constexpr std::uint32_t effect_controller_update_mode_offset = 0x2cU;
  constexpr std::uint32_t effect_controller_render_mode_offset = 0x30U;
  constexpr std::uint32_t effect_particle_stride = 0x68U;
  constexpr std::uint32_t effect_particle_x_offset = 0U;
  constexpr std::uint32_t effect_particle_y_offset = 4U;
  constexpr std::uint32_t effect_particle_z_offset = 8U;
  constexpr std::uint32_t effect_particle_velocity_x_offset = 0x10U;
  constexpr std::uint32_t effect_particle_velocity_y_offset = 0x14U;
  constexpr std::uint32_t effect_particle_velocity_z_offset = 0x18U;
  constexpr std::uint32_t effect_particle_red_offset = 0x20U;
  constexpr std::uint32_t effect_particle_green_offset = 0x21U;
  constexpr std::uint32_t effect_particle_blue_offset = 0x22U;
  constexpr std::uint32_t effect_particle_lifetime_offset = 0x24U;
  constexpr std::uint32_t effect_particle_next_offset = 0x26U;
  constexpr std::uint32_t effect_particle_line_first_color_offset = 0x34U;
  constexpr std::uint32_t effect_particle_line_second_color_offset = 0x3cU;
  constexpr std::uint32_t effect_particle_line_second_x_offset = 0x50U;
  constexpr std::uint32_t effect_particle_line_second_y_offset = 0x52U;
  constexpr std::uint32_t effect_particle_line_second_z_offset = 0x54U;
  constexpr std::uint32_t effect_particle_second_angle_offset = 0x56U;
  constexpr std::uint32_t effect_particle_third_angle_offset = 0x5cU;
  constexpr std::uint32_t effect_particle_total_lifetime_offset = 0x60U;
  constexpr std::uint32_t effect_particle_family_offset = 0x62U;
  constexpr std::uint32_t effect_particle_maximum_frame_offset = 0x64U;
  constexpr std::uint32_t attached_motion_x_offset = 0x10U;
  constexpr std::uint32_t attached_motion_y_offset = 0x14U;
  constexpr std::uint32_t attached_motion_z_offset = 0x18U;
  constexpr std::int16_t fire_family = 1;
  constexpr std::int16_t fire_maximum_frame = 15;
  constexpr std::int16_t explosion_family = 2;
  constexpr std::int16_t attached_explosion_maximum_frame = 7;
  constexpr std::int16_t free_explosion_maximum_frame = 11;
  constexpr std::int16_t breath_family = 3;
  constexpr std::int16_t breath_maximum_frame = 15;
  constexpr std::int16_t vapor_family = 4;
  constexpr std::int16_t vapor_maximum_frame = 7;
  constexpr std::uint32_t park_rain_update_mode = 1U;
  constexpr std::uint32_t ballistic_update_mode = 4U;
  constexpr std::uint32_t taser_conductor_update_mode = 5U;
  constexpr std::uint32_t ejected_shot_update_mode = 6U;
  constexpr std::uint32_t moving_trail_update_mode = 7U;
  constexpr std::uint32_t blood_impact_update_mode = 9U;
  constexpr std::uint32_t blood_impact_render_mode = 0U;
  constexpr std::uint32_t ejected_shot_render_mode = 2U;
  constexpr std::uint32_t line_render_mode = 3U;
  constexpr std::uint32_t park_rain_render_mode = 5U;
  constexpr std::uint32_t maximum_update_mode = 9U;
  constexpr std::uint32_t maximum_render_mode = 5U;

  struct EffectControllerDescriptor {
    std::int32_t state{};
    std::uint32_t flags{};
    std::int16_t attached_slot{-1};
    std::int16_t gravity{};
    std::int16_t family{};
    std::int16_t source{-1};
    std::uint8_t scale{};
    std::uint32_t update_mode{};
    std::uint32_t render_mode{};
    LegacyNativePoint attached_motion;
  };

  std::uint32_t effect_controllers{};
  std::uint16_t effect_controller_count{};
  if (!runtime_.read32(profile.effect_controller_pool_pointer,
                       effect_controllers) ||
      !runtime_.read16(profile.effect_controller_count,
                       effect_controller_count) ||
      effect_controller_count > profile.maximum_effect_controllers ||
      profile.effect_particle_capacity >
          static_cast<std::uint32_t>(
              std::numeric_limits<std::int16_t>::max()) ||
      (effect_controller_count != 0U && effect_controllers == 0U)) {
    return std::nullopt;
  }

  std::vector<EffectControllerDescriptor> controller_descriptors(
      effect_controller_count);
  std::vector<std::uint16_t> particle_controller(
      profile.effect_particle_capacity, 0U);
  std::vector<std::uint16_t> particle_chain_index(
      profile.effect_particle_capacity, 0U);
  std::vector<std::uint16_t> controller_chain_count(effect_controller_count,
                                                    0U);
  std::vector<bool> particle_linked(profile.effect_particle_capacity, false);
  for (std::uint32_t controller_index = 0U;
       controller_index < effect_controller_count; ++controller_index) {
    const auto controller_address = address(
        effect_controllers, static_cast<std::uint64_t>(controller_index) *
                                effect_controller_stride);
    if (!controller_address) {
      return std::nullopt;
    }
    const auto controller_field = [&](std::uint32_t offset) {
      return address(*controller_address, offset);
    };
    const auto state_address = controller_field(effect_controller_state_offset);
    const auto flags_address = controller_field(effect_controller_flags_offset);
    const auto attached_slot_address =
        controller_field(effect_controller_attached_slot_offset);
    const auto gravity_address =
        controller_field(effect_controller_gravity_offset);
    const auto family_address =
        controller_field(effect_controller_family_offset);
    const auto head_address = controller_field(effect_controller_head_offset);
    const auto source_address =
        controller_field(effect_controller_source_offset);
    const auto scale_address = controller_field(effect_controller_scale_offset);
    const auto update_mode_address =
        controller_field(effect_controller_update_mode_offset);
    const auto render_mode_address =
        controller_field(effect_controller_render_mode_offset);
    auto &controller = controller_descriptors[controller_index];
    std::int16_t particle{};
    if (!state_address || !flags_address || !attached_slot_address ||
        !gravity_address || !family_address || !head_address ||
        !source_address || !scale_address || !update_mode_address ||
        !render_mode_address ||
        !read_signed32(*state_address, controller.state) ||
        !runtime_.read32(*flags_address, controller.flags) ||
        !read_signed16(*attached_slot_address, controller.attached_slot) ||
        !read_signed16(*gravity_address, controller.gravity) ||
        !read_signed16(*family_address, controller.family) ||
        !read_signed16(*head_address, particle) ||
        !read_signed16(*source_address, controller.source) ||
        !runtime_.read8(*scale_address, controller.scale) ||
        !runtime_.read32(*update_mode_address, controller.update_mode) ||
        !runtime_.read32(*render_mode_address, controller.render_mode)) {
      return std::nullopt;
    }
    if (particle >= 0 && (controller.update_mode > maximum_update_mode ||
                          controller.render_mode > maximum_render_mode)) {
      return std::nullopt;
    }
    if (particle >= 0 &&
        ((controller.update_mode == ejected_shot_update_mode &&
          controller.render_mode != ejected_shot_render_mode) ||
         (controller.update_mode == blood_impact_update_mode &&
          controller.render_mode != blood_impact_render_mode))) {
      last_bridge_read_fault_ =
          LegacyGameplayBridgeReadFault::effect_controller_mode;
      return std::nullopt;
    }
    if (particle >= 0 && controller.update_mode == moving_trail_update_mode &&
        (controller.flags & 0x20000U) != 0U) {
      if (controller.attached_slot < 0 ||
          controller.attached_slot >= object_count) {
        return std::nullopt;
      }
      const auto record = address(
          object_records, static_cast<std::uint64_t>(controller.attached_slot) *
                              object_record_stride);
      const auto instance_address =
          record ? address(*record, object_instance_offset) : std::nullopt;
      std::uint32_t instance{};
      if (!instance_address || !runtime_.read32(*instance_address, instance) ||
          instance == 0U) {
        return std::nullopt;
      }
      const auto motion_address = address(instance, instance_motion_offset);
      std::uint32_t motion{};
      if (!motion_address || !runtime_.read32(*motion_address, motion) ||
          motion == 0U) {
        return std::nullopt;
      }
      const auto motion_x_address = address(motion, attached_motion_x_offset);
      const auto motion_y_address = address(motion, attached_motion_y_offset);
      const auto motion_z_address = address(motion, attached_motion_z_offset);
      if (!motion_x_address || !motion_y_address || !motion_z_address ||
          !read_signed32(*motion_x_address, controller.attached_motion.x) ||
          !read_signed32(*motion_y_address, controller.attached_motion.y) ||
          !read_signed32(*motion_z_address, controller.attached_motion.z)) {
        return std::nullopt;
      }
    }
    std::uint32_t chain_length = 0U;
    for (; particle >= 0; ++chain_length) {
      const auto particle_index = static_cast<std::uint32_t>(particle);
      if (chain_length >= profile.effect_particle_capacity ||
          particle_index >= profile.effect_particle_capacity ||
          particle_linked[particle_index]) {
        return std::nullopt;
      }
      particle_linked[particle_index] = true;
      particle_controller[particle_index] =
          static_cast<std::uint16_t>(controller_index);
      particle_chain_index[particle_index] =
          static_cast<std::uint16_t>(chain_length);
      const auto particle_address = address(
          profile.effect_particle_pool,
          static_cast<std::uint64_t>(particle_index) * effect_particle_stride);
      const auto next_address =
          particle_address
              ? address(*particle_address, effect_particle_next_offset)
              : std::nullopt;
      if (!next_address || !read_signed16(*next_address, particle)) {
        return std::nullopt;
      }
    }
    controller_chain_count[controller_index] =
        static_cast<std::uint16_t>(chain_length);
  }

  const auto packet_color = [](std::uint32_t packet) {
    return LegacyRgbBridgeState{
        static_cast<std::uint8_t>(packet),
        static_cast<std::uint8_t>(packet >> 8U),
        static_cast<std::uint8_t>(packet >> 16U),
    };
  };
  const auto floor_shift = [](std::int64_t value, unsigned int bits) {
    const auto divisor = std::int64_t{1} << bits;
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
  };
  const auto recover_damped_velocity =
      [&floor_shift](std::int32_t value) -> std::optional<std::int32_t> {
    const auto estimate = static_cast<std::int64_t>(value) * 16 / 15;
    for (auto candidate = estimate - 8; candidate <= estimate + 8;
         ++candidate) {
      if (candidate < std::numeric_limits<std::int32_t>::min() ||
          candidate > std::numeric_limits<std::int32_t>::max()) {
        continue;
      }
      if (candidate - floor_shift(candidate + 1, 4U) == value) {
        return static_cast<std::int32_t>(candidate);
      }
    }
    return std::nullopt;
  };
  const auto read_park_rain_floor = [&]() -> std::optional<std::int32_t> {
    constexpr std::int16_t retail_mission_count = 21;
    constexpr std::uint32_t thresholds_per_mission_bytes = 0x0eU;
    constexpr std::uint8_t maximum_thresholds = 7U;
    std::int16_t mission{};
    std::int16_t probe_y{};
    if (!read_signed16(profile.effect_mission_index, mission) ||
        !read_signed16(profile.effect_floor_probe_y, probe_y)) {
      return std::nullopt;
    }
    if (mission >= 0 && mission < retail_mission_count) {
      std::uint8_t count{};
      const auto count_address = address(profile.effect_floor_counts,
                                         static_cast<std::uint32_t>(mission));
      const auto threshold_base = address(profile.effect_floor_thresholds,
                                          static_cast<std::uint32_t>(mission) *
                                              thresholds_per_mission_bytes);
      if (!count_address || !threshold_base ||
          !runtime_.read8(*count_address, count) ||
          count > maximum_thresholds) {
        return std::nullopt;
      }
      for (std::uint8_t index = 0U; index < count; ++index) {
        const auto threshold_address =
            address(*threshold_base, static_cast<std::uint32_t>(index) * 2U);
        std::int16_t threshold{};
        if (!threshold_address ||
            !read_signed16(*threshold_address, threshold)) {
          return std::nullopt;
        }
        if (static_cast<std::int32_t>(threshold) - 0x80 < probe_y) {
          return static_cast<std::int32_t>(threshold) - 0x60;
        }
      }
    }
    return static_cast<std::int32_t>(probe_y);
  };

  state.expl_particles.reserve(profile.effect_particle_capacity);
  state.line_particles.reserve(profile.effect_particle_capacity);
  state.combat_particles.reserve(profile.effect_particle_capacity);
  for (std::uint32_t particle_index = 0U;
       particle_index < profile.effect_particle_capacity; ++particle_index) {
    if (!particle_linked[particle_index]) {
      continue;
    }
    const auto controller_index = particle_controller[particle_index];
    if (controller_index >= controller_descriptors.size()) {
      return std::nullopt;
    }
    const auto &controller = controller_descriptors[controller_index];
    const auto light_source_slot = [&](std::int16_t slot) {
      return slot >= 0 &&
             static_cast<std::size_t>(slot) < state.objects.size() &&
             legacyLampHaloSourceClass(
                 state.objects[static_cast<std::size_t>(slot)].class_id);
    };
    const auto lamp_halo = light_source_slot(controller.source) ||
                           light_source_slot(controller.attached_slot);
    const auto particle_address = address(
        profile.effect_particle_pool,
        static_cast<std::uint64_t>(particle_index) * effect_particle_stride);
    if (!particle_address) {
      return std::nullopt;
    }
    const auto field = [&](std::uint32_t offset) {
      return address(*particle_address, offset);
    };
    const auto x_address = field(effect_particle_x_offset);
    const auto y_address = field(effect_particle_y_offset);
    const auto z_address = field(effect_particle_z_offset);
    const auto lifetime_address = field(effect_particle_lifetime_offset);
    std::int16_t lifetime{};
    if (!x_address || !y_address || !z_address || !lifetime_address ||
        !read_signed16(*lifetime_address, lifetime)) {
      return std::nullopt;
    }

    // Every embedded particle camera item is authored around the current
    // pool position, not around its controller's source object. In
    // particular, update7/render3 lamp and glass fragments use a GsSPRITE at
    // particle+0x28 but are not members of the EXPL texture families below.
    // Capture their exact moving centre before any family-specific branch can
    // continue, otherwise native camera reprojection incorrectly anchors them
    // at the zero-initialized world origin.
    const auto has_particle_sprite =
        std::ranges::any_of(state.guest_sprites, [&](const auto &sprite) {
          return sprite.effect_particle ==
                 static_cast<std::int16_t>(particle_index);
        });
    const auto has_particle_packet =
        std::ranges::any_of(state.guest_raw_packets, [&](const auto &packet) {
          return packet.effect_particle ==
                 static_cast<std::int16_t>(particle_index);
        });
    if (has_particle_sprite || has_particle_packet) {
      LegacyNativePoint particle_position;
      if (!read_signed32(*x_address, particle_position.x) ||
          !read_signed32(*y_address, particle_position.y) ||
          !read_signed32(*z_address, particle_position.z) ||
          particle_position.y == std::numeric_limits<std::int32_t>::min()) {
        return std::nullopt;
      }
      for (auto &sprite : state.guest_sprites) {
        if (sprite.effect_particle ==
            static_cast<std::int16_t>(particle_index)) {
          sprite.effect_position = particle_position;
          sprite.force_fullbright = lamp_halo;
        }
      }
      for (auto &packet : state.guest_raw_packets) {
        if (packet.effect_particle ==
            static_cast<std::int16_t>(particle_index)) {
          packet.effect_controller =
              static_cast<std::int16_t>(controller_index);
          packet.effect_position = particle_position;
          packet.effect_world_position_valid = true;
          const auto base_opcode =
              static_cast<std::uint8_t>(packet.opcode & 0xfdU);
          if (controller.update_mode == taser_conductor_update_mode &&
              controller.render_mode == line_render_mode &&
              packet.word_count == 4U && base_opcode == 0x50U) {
            packet.taser_segment_index =
                static_cast<std::int16_t>(particle_chain_index[particle_index]);
            packet.taser_segment_count =
                controller_chain_count[controller_index];
          }
        }
      }
    }

    // PARK.OVL's update1/render5 controller owns 80 rain particles. Its
    // camera-list packet is already projected at 20 Hz, but native terrain is
    // rendered through the interpolated camera. Recover the exact world line
    // now so presentation can reproject it and depth-test it against walls.
    const auto rain_packet =
        std::ranges::find_if(state.guest_raw_packets, [&](const auto &packet) {
          return packet.effect_particle ==
                     static_cast<std::int16_t>(particle_index) &&
                 packet.word_count == 4U && packet.opcode == 0x52U;
        });
    const auto park_rain_particle =
        controller.update_mode == park_rain_update_mode &&
        controller.render_mode == park_rain_render_mode &&
        rain_packet != state.guest_raw_packets.end();
    if (park_rain_particle) {
      constexpr std::uint32_t offscreen_endpoint = 0x04000400U;
      if (rain_packet->words[1] == offscreen_endpoint &&
          rain_packet->words[3] == offscreen_endpoint) {
        continue;
      }
      LegacyLineParticleBridgeState line;
      if (!read_signed32(*x_address, line.first.x) ||
          !read_signed32(*y_address, line.first.y) ||
          !read_signed32(*z_address, line.first.z) || lifetime < 0) {
        return std::nullopt;
      }
      line.second = line.first;
      line.controller = controller_index;
      line.particle = static_cast<std::uint16_t>(particle_index);
      line.source_slot = controller.source;
      line.remaining_updates = lifetime;
      line.first_color = packet_color(rain_packet->words[0]);
      line.second_color = packet_color(rain_packet->words[2]);
      line.semi_transparent = true;

      const auto floor = read_park_rain_floor();
      if (!floor || line.first.y < *floor) {
        return std::nullopt;
      }
      if (line.first.y > *floor) {
        std::int32_t world_motion_x{};
        std::int32_t world_motion_z{};
        if (!read_signed32(profile.effect_world_motion_x, world_motion_x) ||
            !read_signed32(profile.effect_world_motion_z, world_motion_z)) {
          return std::nullopt;
        }
        const auto gravity_high = static_cast<std::uint8_t>(
            static_cast<std::uint16_t>(controller.gravity) >> 8U);
        const auto fall =
            static_cast<std::int32_t>(std::bit_cast<std::int8_t>(gravity_high));
        if (fall >= 0) {
          return std::nullopt;
        }
        const auto previous_x = static_cast<std::int64_t>(line.first.x) -
                                floor_shift(world_motion_x, 3U);
        const auto previous_y = static_cast<std::int64_t>(line.first.y) - fall;
        const auto previous_z = static_cast<std::int64_t>(line.first.z) -
                                floor_shift(world_motion_z, 3U);
        if (previous_x < std::numeric_limits<std::int32_t>::min() ||
            previous_x > std::numeric_limits<std::int32_t>::max() ||
            previous_y < std::numeric_limits<std::int32_t>::min() ||
            previous_y > std::numeric_limits<std::int32_t>::max() ||
            previous_z < std::numeric_limits<std::int32_t>::min() ||
            previous_z > std::numeric_limits<std::int32_t>::max()) {
          return std::nullopt;
        }
        line.second = LegacyNativePoint{
            static_cast<std::int32_t>(previous_x),
            static_cast<std::int32_t>(previous_y),
            static_cast<std::int32_t>(previous_z),
        };
        line.kind = LegacyLineParticleKind::rain_streak;
      } else {
        const auto first_x = static_cast<std::int16_t>(rain_packet->words[1]);
        const auto first_y =
            static_cast<std::int16_t>(rain_packet->words[1] >> 16U);
        const auto second_x = static_cast<std::int16_t>(rain_packet->words[3]);
        const auto second_y =
            static_cast<std::int16_t>(rain_packet->words[3] >> 16U);
        const auto width = std::abs(static_cast<std::int32_t>(second_x) -
                                    static_cast<std::int32_t>(first_x));
        if (width == 0) {
          continue;
        }
        if (first_y != second_y || (width & 1) != 0 ||
            width / 2 > std::numeric_limits<std::uint8_t>::max()) {
          return std::nullopt;
        }
        // The custom builder has already clamped Y to the authored mission
        // floor. Preserve its exact projected expansion in native space.
        line.kind = LegacyLineParticleKind::rain_splash;
        line.screen_half_width = static_cast<std::uint8_t>(width / 2);
      }
      state.line_particles.push_back(line);
      continue;
    }

    const auto line_particle =
        controller.render_mode == line_render_mode &&
        (controller.update_mode == ballistic_update_mode ||
         controller.update_mode == moving_trail_update_mode);
    if (line_particle) {
      const auto first_color_address =
          field(effect_particle_line_first_color_offset);
      const auto second_color_address =
          field(effect_particle_line_second_color_offset);
      std::uint32_t first_color_packet{};
      std::uint32_t second_color_packet{};
      LegacyLineParticleBridgeState line;
      if (!first_color_address || !second_color_address ||
          !read_signed32(*x_address, line.first.x) ||
          !read_signed32(*y_address, line.first.y) ||
          !read_signed32(*z_address, line.first.z) ||
          !runtime_.read32(*first_color_address, first_color_packet) ||
          !runtime_.read32(*second_color_address, second_color_packet)) {
        return std::nullopt;
      }
      const auto opcode = static_cast<std::uint8_t>(first_color_packet >> 24U);
      if ((controller.update_mode == ballistic_update_mode &&
           (opcode != 0x50U || lifetime <= 0)) ||
          (controller.update_mode == moving_trail_update_mode &&
           ((opcode != 0x50U && opcode != 0x52U) || lifetime == 0))) {
        return std::nullopt;
      }
      line.controller = controller_index;
      line.particle = static_cast<std::uint16_t>(particle_index);
      line.source_slot = controller.source;
      line.remaining_updates = lifetime;
      line.first_color = packet_color(first_color_packet);
      line.second_color = packet_color(second_color_packet);
      line.semi_transparent = opcode == 0x52U;

      if (controller.update_mode == ballistic_update_mode) {
        const auto second_x_address =
            field(effect_particle_line_second_x_offset);
        const auto second_y_address =
            field(effect_particle_line_second_y_offset);
        const auto second_z_address =
            field(effect_particle_line_second_z_offset);
        std::int16_t second_x{};
        std::int16_t second_y{};
        std::int16_t second_z{};
        if (!second_x_address || !second_y_address || !second_z_address ||
            !read_signed16(*second_x_address, second_x) ||
            !read_signed16(*second_y_address, second_y) ||
            !read_signed16(*second_z_address, second_z)) {
          return std::nullopt;
        }
        line.second = line.first;
        line.first = LegacyNativePoint{second_x, second_y, second_z};
        line.kind = LegacyLineParticleKind::ballistic_tracer;
      } else {
        const auto velocity_x_address =
            field(effect_particle_velocity_x_offset);
        const auto velocity_y_address =
            field(effect_particle_velocity_y_offset);
        const auto velocity_z_address =
            field(effect_particle_velocity_z_offset);
        std::int32_t velocity_x{};
        std::int32_t velocity_y{};
        std::int32_t velocity_z{};
        if (!velocity_x_address || !velocity_y_address || !velocity_z_address ||
            !read_signed32(*velocity_x_address, velocity_x) ||
            !read_signed32(*velocity_y_address, velocity_y) ||
            !read_signed32(*velocity_z_address, velocity_z)) {
          return std::nullopt;
        }
        if ((controller.flags & 0x10U) != 0U) {
          const auto recovered_x = recover_damped_velocity(velocity_x);
          const auto recovered_z = recover_damped_velocity(velocity_z);
          if (!recovered_x || !recovered_z) {
            return std::nullopt;
          }
          velocity_x = *recovered_x;
          velocity_z = *recovered_z;
        }
        std::int64_t step_x = velocity_x;
        std::int64_t step_z = velocity_z;
        auto step_y =
            (static_cast<std::int64_t>(velocity_y) - controller.gravity) / 4096;
        if ((controller.flags & 0x20000U) != 0U) {
          step_x += floor_shift(controller.attached_motion.x, 12U);
          step_y += floor_shift(controller.attached_motion.y, 12U);
          step_z += floor_shift(controller.attached_motion.z, 12U);
        }
        if ((controller.flags & 0x4000U) != 0U) {
          std::int32_t world_motion_x{};
          std::int32_t world_motion_z{};
          if (!read_signed32(profile.effect_world_motion_x, world_motion_x) ||
              !read_signed32(profile.effect_world_motion_z, world_motion_z)) {
            return std::nullopt;
          }
          step_x += floor_shift(world_motion_x, 5U);
          step_z += floor_shift(world_motion_z, 5U);
        }
        const auto previous_x =
            static_cast<std::int64_t>(line.first.x) - step_x;
        const auto previous_y =
            static_cast<std::int64_t>(line.first.y) - step_y;
        const auto previous_z =
            static_cast<std::int64_t>(line.first.z) - step_z;
        if (previous_x < std::numeric_limits<std::int32_t>::min() ||
            previous_x > std::numeric_limits<std::int32_t>::max() ||
            previous_y < std::numeric_limits<std::int32_t>::min() ||
            previous_y > std::numeric_limits<std::int32_t>::max() ||
            previous_z < std::numeric_limits<std::int32_t>::min() ||
            previous_z > std::numeric_limits<std::int32_t>::max()) {
          return std::nullopt;
        }
        line.second = LegacyNativePoint{
            static_cast<std::int32_t>(previous_x),
            static_cast<std::int32_t>(previous_y),
            static_cast<std::int32_t>(previous_z),
        };
        line.kind = LegacyLineParticleKind::moving_trail;
        line.raw_packet_authoritative = (controller.flags & 0x40U) != 0U;
      }
      state.line_particles.push_back(line);
      continue;
    }

    const auto ejected_shot_particle =
        controller.update_mode == ejected_shot_update_mode &&
        controller.render_mode == ejected_shot_render_mode;
    const auto blood_impact_particle =
        controller.update_mode == blood_impact_update_mode &&
        controller.render_mode == blood_impact_render_mode;
    if (ejected_shot_particle || blood_impact_particle) {
      if (controller.source < -1 ||
          (controller.source >= 0 && controller.source >= object_count)) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::effect_source_slot;
        return std::nullopt;
      }
      if (lifetime <= 0) {
        return std::nullopt;
      }
      // The retail controller allocator initializes +0x20 (source), but
      // deliberately leaves +0x14 untouched. FUN_8004e1f0 never writes that
      // field for update6/render2, so the first mission's first cartridge
      // carries the allocator's 0x0c0c poison there. It is a world-space
      // LINE_F2 and has no attachment. FUN_80054fbc consumes +0x14 only when
      // controller flag 0x20000 is set; 0x10000 has unrelated semantics and
      // must not turn the same allocator poison into an object attachment.
      constexpr std::uint32_t actor_attachment_flags = 0x20000U;
      const auto attachment_authored =
          blood_impact_particle &&
          (controller.flags & actor_attachment_flags) != 0U;
      if (attachment_authored && (controller.attached_slot < 0 ||
                                  controller.attached_slot >= object_count)) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::effect_attached_slot;
        return std::nullopt;
      }
      LegacyCombatParticleBridgeState particle;
      particle.controller = controller_index;
      particle.particle = static_cast<std::uint16_t>(particle_index);
      particle.attached_slot =
          attachment_authored ? controller.attached_slot : std::int16_t{-1};
      particle.source_slot = controller.source;
      particle.remaining_updates = lifetime;
      particle.scale_byte = controller.scale;
      if (!read_signed32(*x_address, particle.position.x) ||
          !read_signed32(*y_address, particle.position.y) ||
          !read_signed32(*z_address, particle.position.z)) {
        return std::nullopt;
      }

      if (ejected_shot_particle) {
        const auto packet_address =
            field(effect_particle_line_first_color_offset);
        const auto velocity_x_address =
            field(effect_particle_velocity_x_offset);
        const auto velocity_y_address =
            field(effect_particle_velocity_y_offset);
        const auto velocity_z_address =
            field(effect_particle_velocity_z_offset);
        const auto angle_address = field(effect_particle_family_offset);
        const auto angle_step_address =
            field(effect_particle_maximum_frame_offset);
        std::uint32_t packet{};
        std::int32_t velocity_x{};
        std::int32_t velocity_y{};
        std::int32_t velocity_z{};
        std::uint16_t angle{};
        std::uint16_t angle_step{};
        if (!packet_address || !velocity_x_address || !velocity_y_address ||
            !velocity_z_address || !angle_address || !angle_step_address ||
            !runtime_.read32(*packet_address, packet) ||
            !read_signed32(*velocity_x_address, velocity_x) ||
            !read_signed32(*velocity_y_address, velocity_y) ||
            !read_signed32(*velocity_z_address, velocity_z) ||
            !runtime_.read16(*angle_address, angle) ||
            !runtime_.read16(*angle_step_address, angle_step) ||
            static_cast<std::uint8_t>(packet >> 24U) != 0x40U) {
          last_bridge_read_fault_ =
              LegacyGameplayBridgeReadFault::effect_packet_opcode;
          return std::nullopt;
        }
        const auto previous_x =
            static_cast<std::int64_t>(particle.position.x) - velocity_x;
        const auto previous_y = static_cast<std::int64_t>(particle.position.y) -
                                floor_shift(velocity_y, 12U);
        const auto previous_z =
            static_cast<std::int64_t>(particle.position.z) - velocity_z;
        if (previous_x < std::numeric_limits<std::int32_t>::min() ||
            previous_x > std::numeric_limits<std::int32_t>::max() ||
            previous_y < std::numeric_limits<std::int32_t>::min() ||
            previous_y > std::numeric_limits<std::int32_t>::max() ||
            previous_z < std::numeric_limits<std::int32_t>::min() ||
            previous_z > std::numeric_limits<std::int32_t>::max()) {
          last_bridge_read_fault_ =
              LegacyGameplayBridgeReadFault::effect_position_overflow;
          return std::nullopt;
        }
        particle.position = LegacyNativePoint{
            static_cast<std::int32_t>(previous_x),
            static_cast<std::int32_t>(previous_y),
            static_cast<std::int32_t>(previous_z),
        };
        particle.angle = std::bit_cast<std::int16_t>(
            static_cast<std::uint16_t>(angle - angle_step));
        particle.second_angle = std::bit_cast<std::int16_t>(angle_step);
        particle.kind = LegacyCombatParticleKind::ejected_shot_line;
        particle.color = packet_color(packet);
      } else {
        const auto color_address = field(effect_particle_red_offset);
        const auto packet_address =
            field(effect_particle_line_first_color_offset);
        const auto angle_address = field(effect_particle_family_offset);
        const auto second_angle_address =
            field(effect_particle_second_angle_offset);
        const auto third_angle_address =
            field(effect_particle_third_angle_offset);
        std::uint32_t color{};
        std::uint32_t packet{};
        if (!color_address || !packet_address || !angle_address ||
            !second_angle_address || !third_angle_address ||
            !runtime_.read32(*color_address, color) ||
            !runtime_.read32(*packet_address, packet) ||
            !read_signed16(*angle_address, particle.angle) ||
            !read_signed16(*second_angle_address, particle.second_angle) ||
            !read_signed16(*third_angle_address, particle.third_angle)) {
          return std::nullopt;
        }
        const auto opcode = static_cast<std::uint8_t>(packet >> 24U);
        if (opcode != 0x20U && opcode != 0x22U) {
          last_bridge_read_fault_ =
              LegacyGameplayBridgeReadFault::effect_packet_opcode;
          return std::nullopt;
        }
        particle.kind = LegacyCombatParticleKind::blood_impact_triangle;
        particle.color = packet_color(color);
        particle.semi_transparent = opcode == 0x22U;
      }
      state.combat_particles.push_back(particle);
      continue;
    }

    if (controller.scale == 0U) {
      continue;
    }
    const auto red_address = field(effect_particle_red_offset);
    const auto green_address = field(effect_particle_green_offset);
    const auto blue_address = field(effect_particle_blue_offset);
    const auto total_lifetime_address =
        field(effect_particle_total_lifetime_offset);
    const auto family_address = field(effect_particle_family_offset);
    const auto maximum_frame_address =
        field(effect_particle_maximum_frame_offset);
    std::int16_t total_lifetime{};
    std::int16_t family = controller.family;
    std::int16_t maximum_frame{};
    if (!red_address || !green_address || !blue_address ||
        !total_lifetime_address || !family_address || !maximum_frame_address ||
        !read_signed16(*total_lifetime_address, total_lifetime) ||
        (controller.state < 0 && !read_signed16(*family_address, family)) ||
        !read_signed16(*maximum_frame_address, maximum_frame)) {
      return std::nullopt;
    }
    const auto supported_fire =
        family == fire_family && maximum_frame == fire_maximum_frame;
    const auto supported_explosion =
        family == explosion_family &&
        (maximum_frame == attached_explosion_maximum_frame ||
         maximum_frame == free_explosion_maximum_frame);
    const auto supported_breath =
        family == breath_family && maximum_frame == breath_maximum_frame;
    const auto supported_vapor =
        family == vapor_family && maximum_frame == vapor_maximum_frame;
    if (lifetime <= 0 || total_lifetime <= 0 ||
        (!supported_fire && !supported_explosion && !supported_breath &&
         !supported_vapor)) {
      continue;
    }
    LegacyExplParticleBridgeState particle;
    if (!read_signed32(*x_address, particle.position.x) ||
        !read_signed32(*y_address, particle.position.y) ||
        !read_signed32(*z_address, particle.position.z) ||
        !runtime_.read8(*red_address, particle.red) ||
        !runtime_.read8(*green_address, particle.green) ||
        !runtime_.read8(*blue_address, particle.blue)) {
      return std::nullopt;
    }
    particle.pool_index = static_cast<std::int16_t>(particle_index);
    const auto frame =
        maximum_frame - maximum_frame * static_cast<std::int32_t>(lifetime) /
                            static_cast<std::int32_t>(total_lifetime);
    particle.controller = controller_index;
    particle.source_slot = controller.source;
    particle.family = static_cast<std::uint8_t>(family);
    particle.scale_byte = controller.scale;
    particle.frame = static_cast<std::uint8_t>(
        std::clamp<std::int32_t>(frame, 0, maximum_frame));
    for (auto &sprite : state.guest_sprites) {
      if (sprite.effect_particle == static_cast<std::int16_t>(particle_index)) {
        sprite.effect_family = particle.family;
        sprite.effect_frame = particle.frame;
        sprite.effect_position = particle.position;
      }
    }
    state.expl_particles.push_back(particle);
  }

  if (std::ranges::any_of(state.guest_raw_packets, [](const auto &packet) {
        return packet.effect_particle >= 0 &&
               !packet.effect_world_position_valid;
      })) {
    return std::nullopt;
  }

  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::park2_flame;
  const auto &park2_flame = profile.park2_flamethrower;
  if (park2_flame.enabled) {
    constexpr std::uint32_t retail_packet_stride = 0x30U;
    constexpr std::uint32_t retail_packet_count = 72U;
    constexpr std::uint32_t retail_state_stride = 0x0cU;
    constexpr std::uint32_t retail_width_history_stride = 0x10U;
    constexpr std::uint32_t packet_word_count = 0x09000000U;
    constexpr std::uint32_t packet_word_count_mask = 0xff000000U;
    constexpr std::uint8_t flat_textured_quad = 0x2cU;
    constexpr std::uint8_t semitrans_flat_textured_quad = 0x2eU;
    constexpr std::uint16_t retail_expl_clut = 0x7ff0U;
    constexpr std::uint16_t retail_expl_tpage = 0x00bcU;
    constexpr std::uint32_t offscreen_xy = 0x04000400U;
    constexpr std::array<std::uint8_t, 4U> frame_minimum_u{64U, 96U, 0U, 32U};
    constexpr std::array<std::uint8_t, 4U> frame_minimum_v{0U, 0U, 32U, 32U};

    if (park2_flame.packet_stride != retail_packet_stride ||
        park2_flame.packet_count != retail_packet_count ||
        park2_flame.state_pool != 0x8014bd48U ||
        park2_flame.state_stride != retail_state_stride ||
        park2_flame.width_history_pool != 0x8014b8c8U ||
        park2_flame.width_history_stride != retail_width_history_stride ||
        !readable_ram_pointer(
            park2_flame.packet_pool,
            static_cast<std::size_t>(park2_flame.packet_count) *
                park2_flame.packet_stride) ||
        !readable_ram_pointer(
            park2_flame.state_pool,
            static_cast<std::size_t>(park2_flame.packet_count) *
                park2_flame.state_stride) ||
        !readable_ram_pointer(
            park2_flame.width_history_pool,
            static_cast<std::size_t>(park2_flame.packet_count) *
                park2_flame.width_history_stride)) {
      last_bridge_read_fault_ =
          LegacyGameplayBridgeReadFault::park2_flame_profile;
      return std::nullopt;
    }
    for (const auto &validation : park2_flame.validation_words) {
      std::uint32_t word{};
      if (!readable_ram_pointer(validation.address, sizeof(word)) ||
          !runtime_.read32(validation.address, word) ||
          word != validation.expected) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_overlay;
        return std::nullopt;
      }
    }

    state.park2_flamethrower_ribbons.reserve(park2_flame.packet_count);
    for (std::uint32_t slot = 0U; slot < park2_flame.packet_count; ++slot) {
      const auto packet =
          address(park2_flame.packet_pool,
                  static_cast<std::uint64_t>(slot) * park2_flame.packet_stride);
      if (!packet) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_profile;
        return std::nullopt;
      }
      const auto field = [&address, packet](std::uint32_t offset) {
        return address(*packet, offset);
      };
      const auto packet_tag_address = field(0x08U);
      const auto color_code_address = field(0x0cU);
      const auto uv0_address = field(0x14U);
      const auto clut_address = field(0x16U);
      const auto uv1_address = field(0x1cU);
      const auto tpage_address = field(0x1eU);
      const auto uv2_address = field(0x24U);
      const auto uv3_address = field(0x2cU);
      std::uint32_t link{};
      std::uint32_t packet_tag{};
      std::uint32_t color_code{};
      std::uint16_t uv0{};
      std::uint16_t clut{};
      std::uint16_t uv1{};
      std::uint16_t tpage{};
      std::uint16_t uv2{};
      std::uint16_t uv3{};
      if (!packet_tag_address || !color_code_address || !uv0_address ||
          !clut_address || !uv1_address || !tpage_address || !uv2_address ||
          !uv3_address || !runtime_.read32(*packet, link)) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_profile;
        return std::nullopt;
      }
      if (link == 0U) {
        continue;
      }
      std::uint32_t linked_packet{};
      if (!readable_ram_pointer(link, sizeof(linked_packet)) ||
          !runtime_.read32(link, linked_packet) || linked_packet != *packet ||
          !runtime_.read32(*packet_tag_address, packet_tag) ||
          !runtime_.read32(*color_code_address, color_code) ||
          !runtime_.read16(*uv0_address, uv0) ||
          !runtime_.read16(*clut_address, clut) ||
          !runtime_.read16(*uv1_address, uv1) ||
          !runtime_.read16(*tpage_address, tpage) ||
          !runtime_.read16(*uv2_address, uv2) ||
          !runtime_.read16(*uv3_address, uv3) ||
          (packet_tag & packet_word_count_mask) != packet_word_count) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_link;
        return std::nullopt;
      }

      const auto material = static_cast<std::size_t>(slot & 3U);
      const auto minimum_u = frame_minimum_u[material];
      const auto minimum_v = frame_minimum_v[material];
      const auto maximum_u = static_cast<std::uint8_t>(minimum_u + 31U);
      const auto maximum_v = static_cast<std::uint8_t>(minimum_v + 31U);
      const auto pack_uv = [](std::uint8_t u, std::uint8_t v) {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(u) |
            (static_cast<std::uint16_t>(v) << 8U));
      };
      if (uv0 != pack_uv(minimum_u, minimum_v) ||
          uv1 != pack_uv(maximum_u, minimum_v) ||
          uv2 != pack_uv(minimum_u, maximum_v) ||
          uv3 != pack_uv(maximum_u, maximum_v) || clut != retail_expl_clut ||
          tpage != retail_expl_tpage) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_material;
        return std::nullopt;
      }

      const auto opcode = static_cast<std::uint8_t>(color_code >> 24U);
      if (opcode == flat_textured_quad) {
        // FUN_8014844c has linked this slot, but FUN_80147a8c has not yet
        // projected it into a drawable semitransparent packet.
        continue;
      }
      if (opcode != semitrans_flat_textured_quad) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_opcode;
        return std::nullopt;
      }

      const auto depth_address = field(0x04U);
      constexpr std::array<std::uint32_t, 4U> xy_offsets{0x10U, 0x18U, 0x20U,
                                                         0x28U};
      std::uint32_t ordering_depth{};
      std::array<std::uint32_t, 4U> packed_xy{};
      if (!depth_address || !runtime_.read32(*depth_address, ordering_depth)) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_projection;
        return std::nullopt;
      }
      for (std::size_t corner = 0U; corner < packed_xy.size(); ++corner) {
        const auto xy_address = field(xy_offsets[corner]);
        if (!xy_address || !runtime_.read32(*xy_address, packed_xy[corner])) {
          last_bridge_read_fault_ =
              LegacyGameplayBridgeReadFault::park2_flame_projection;
          return std::nullopt;
        }
      }
      if (std::ranges::all_of(
              packed_xy, [](std::uint32_t xy) { return xy == offscreen_xy; })) {
        continue;
      }
      if (ordering_depth == 0U || ordering_depth >= 4096U) {
        // The overlay switches a linked FT4 to opcode 0x2e before its OT
        // depth is committed, and clears the depth again while recycling the
        // ring slot. Both states are valid but not drawable this frame.
        continue;
      }

      LegacyPark2FlamethrowerRibbonBridgeState ribbon;
      auto drawable_coordinates = true;
      for (std::size_t corner = 0U; corner < packed_xy.size(); ++corner) {
        const auto x = std::bit_cast<std::int16_t>(
            static_cast<std::uint16_t>(packed_xy[corner]));
        const auto y = std::bit_cast<std::int16_t>(
            static_cast<std::uint16_t>(packed_xy[corner] >> 16U));
        if (x < -1024 || x > 1023 || y < -1024 || y > 1023) {
          // GTE projection saturates individual corners while a ribbon
          // crosses the viewport boundary. Retail leaves that linked packet
          // in the ring, but it must not enter native presentation.
          drawable_coordinates = false;
          break;
        }
        ribbon.corners[corner] = LegacyProjectedPointBridgeState{x, y};
      }
      if (!drawable_coordinates) {
        continue;
      }

      const auto current_state =
          address(park2_flame.state_pool,
                  static_cast<std::uint64_t>(slot) * park2_flame.state_stride);
      const auto next_slot = (slot + 1U) % park2_flame.packet_count;
      const auto next_state = address(park2_flame.state_pool,
                                      static_cast<std::uint64_t>(next_slot) *
                                          park2_flame.state_stride);
      const auto width_history = address(park2_flame.width_history_pool,
                                         static_cast<std::uint64_t>(slot) *
                                             park2_flame.width_history_stride);
      const auto read_state_offset =
          [&](std::uint32_t state, std::uint32_t offset, std::int16_t &value) {
            const auto source = address(state, offset);
            return source && read_signed16(*source, value);
          };
      const auto read_object_position = [&](std::uint32_t object,
                                            LegacyNativePoint &position) {
        const auto x = address(object, 0x14U);
        const auto y = address(object, 0x18U);
        const auto z = address(object, 0x1cU);
        return x && y && z && readable_ram_pointer(object, 0x20U) &&
               read_signed32(*x, position.x) && read_signed32(*y, position.y) &&
               read_signed32(*z, position.z);
      };
      const auto checked_point =
          [](std::int64_t x, std::int64_t y,
             std::int64_t z) -> std::optional<LegacyNativePoint> {
        constexpr auto minimum =
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
        constexpr auto maximum =
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
        if (x < minimum || x > maximum || y < minimum || y > maximum ||
            z < minimum || z > maximum) {
          return std::nullopt;
        }
        return LegacyNativePoint{static_cast<std::int32_t>(x),
                                 static_cast<std::int32_t>(y),
                                 static_cast<std::int32_t>(z)};
      };
      std::uint32_t current_object{};
      std::uint32_t next_object{};
      LegacyNativePoint current_base;
      LegacyNativePoint next_base;
      std::int16_t current_x_offset{};
      std::int16_t current_y_offset{};
      std::int16_t current_z_offset{};
      std::int16_t next_x_offset{};
      std::int16_t next_y_offset{};
      std::int16_t next_z_offset{};
      std::int32_t width_x{};
      std::int32_t width_y{};
      std::int32_t width_z{};
      if (!current_state || !next_state || !width_history ||
          !runtime_.read32(*current_state, current_object) ||
          current_object == 0U ||
          !read_object_position(current_object, current_base) ||
          !read_state_offset(*current_state, 4U, current_x_offset) ||
          !read_state_offset(*current_state, 6U, current_y_offset) ||
          !read_state_offset(*current_state, 8U, current_z_offset) ||
          !runtime_.read32(*next_state, next_object) ||
          !read_signed32(*width_history, width_x) ||
          !read_signed32(*width_history + 4U, width_y) ||
          !read_signed32(*width_history + 8U, width_z)) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_world;
        return std::nullopt;
      }

      // The bridge samples RAM after FUN_80147a8c has completed the ring. An
      // active slot's object therefore already contains base + the full local
      // step used by its packet. Rewind that committed step to recover the
      // exact current base which retail projected this frame.
      const auto current_before = checked_point(
          static_cast<std::int64_t>(current_base.x) - current_x_offset,
          static_cast<std::int64_t>(current_base.y) + current_y_offset,
          static_cast<std::int64_t>(current_base.z) - current_z_offset);
      if (!current_before) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_world;
        return std::nullopt;
      }

      // FUN_80147a8c projects the next/fallback point first. At the ring wrap
      // it applies slot zero's full local offset and the authored -2 Y bias;
      // an unlinked next state falls back to the pre-step current base point.
      auto world_first = current_before;
      if (next_object != 0U) {
        if (!read_object_position(next_object, next_base) ||
            !read_state_offset(*next_state, 4U, next_x_offset) ||
            !read_state_offset(*next_state, 6U, next_y_offset) ||
            !read_state_offset(*next_state, 8U, next_z_offset)) {
          last_bridge_read_fault_ =
              LegacyGameplayBridgeReadFault::park2_flame_world;
          return std::nullopt;
        }
        world_first = next_base;
        if (next_slot == 0U) {
          // Slot zero was processed before slot 71. Its final position is the
          // base seen by the wrap path before this extra step and -2 Y bias.
          world_first = checked_point(
              static_cast<std::int64_t>(next_base.x) + next_x_offset,
              static_cast<std::int64_t>(next_base.y) + 2 - next_y_offset,
              static_cast<std::int64_t>(next_base.z) + next_z_offset);
        } else {
          // A normal successor is processed later. If its packet is active,
          // the sampled object has since advanced and must be rewound. An
          // inactive successor still holds the base observed by this slot.
          const auto next_packet = address(
              park2_flame.packet_pool, static_cast<std::uint64_t>(next_slot) *
                                           park2_flame.packet_stride);
          std::uint32_t next_link{};
          if (!next_packet || !runtime_.read32(*next_packet, next_link)) {
            last_bridge_read_fault_ =
                LegacyGameplayBridgeReadFault::park2_flame_world;
            return std::nullopt;
          }
          if (next_link != 0U) {
            world_first = checked_point(
                static_cast<std::int64_t>(next_base.x) - next_x_offset,
                static_cast<std::int64_t>(next_base.y) + next_y_offset,
                static_cast<std::int64_t>(next_base.z) - next_z_offset);
          }
        }
      }
      if (world_first) {
        const auto absolute = [](std::int64_t value) {
          return value < 0 ? -value : value;
        };
        const auto delta_x =
            static_cast<std::int64_t>(world_first->x) - current_before->x;
        const auto delta_y =
            static_cast<std::int64_t>(world_first->y) - current_before->y;
        const auto delta_z =
            static_cast<std::int64_t>(world_first->z) - current_before->z;
        // 0x80147e44..0x80147f80 prevents a newly linked neighbour from
        // stretching one FT4 across the whole chain. Retail compares each
        // current local offset with one eighth of the absolute separation;
        // if a local offset is still smaller than that envelope in any axis,
        // it keeps only one quarter of the signed separation (MIPS
        // truncation toward zero).
        const auto clamp_neighbour =
            absolute(current_x_offset) < absolute(delta_x) / 8 ||
            absolute(current_y_offset) < absolute(delta_y) / 8 ||
            absolute(current_z_offset) < absolute(delta_z) / 8;
        if (clamp_neighbour) {
          world_first = checked_point(
              static_cast<std::int64_t>(current_before->x) + delta_x / 4,
              static_cast<std::int64_t>(current_before->y) + delta_y / 4,
              static_cast<std::int64_t>(current_before->z) + delta_z / 4);
        }
      }
      // Retail projects pre-step base + offset + arithmetic(offset >> 1).
      // The sampled object already contains pre-step base + offset, leaving
      // only the half-step. Store guest Y for gameplay's usual inversion.
      const auto world_second =
          checked_point(static_cast<std::int64_t>(current_base.x) +
                            floor_shift(current_x_offset, 1U),
                        static_cast<std::int64_t>(current_base.y) -
                            floor_shift(current_y_offset, 1U),
                        static_cast<std::int64_t>(current_base.z) +
                            floor_shift(current_z_offset, 1U));
      if (!world_first || !world_second) {
        last_bridge_read_fault_ =
            LegacyGameplayBridgeReadFault::park2_flame_world;
        return std::nullopt;
      }

      ribbon.color = LegacyRgbBridgeState{
          static_cast<std::uint8_t>(color_code),
          static_cast<std::uint8_t>(color_code >> 8U),
          static_cast<std::uint8_t>(color_code >> 16U),
      };
      ribbon.ordering_depth = static_cast<std::uint16_t>(ordering_depth);
      ribbon.slot = static_cast<std::uint8_t>(slot);
      ribbon.frame = static_cast<std::uint8_t>(2U + material);
      ribbon.world_first = *world_first;
      ribbon.world_second = *world_second;
      ribbon.width_shift =
          width_x == 0 && width_y == 0 && width_z == 0 ? 1U : 2U;
      state.park2_flamethrower_ribbons.push_back(ribbon);
    }
  }
  state.weapon_events = weapon_events_;
  last_bridge_read_fault_ = LegacyGameplayBridgeReadFault::none;
  last_bridge_read_stage_ = LegacyGameplayBridgeReadStage::none;
  return state;
}

bool LegacyGameplayVm::writeHostPlayerState(
    const LegacyHostPlayerState &state,
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  constexpr std::uint32_t matrix_translation_x_offset = 0x14U;
  constexpr std::uint32_t matrix_translation_y_offset = 0x18U;
  constexpr std::uint32_t matrix_translation_z_offset = 0x1cU;
  constexpr std::uint32_t motion_position_x_offset = 0U;
  constexpr std::uint32_t motion_position_y_offset = 4U;
  constexpr std::uint32_t motion_position_z_offset = 8U;
  constexpr std::uint32_t motion_cached_position_offset = 0x40U;
  constexpr std::uint32_t health_armor_offset = 6U;
  constexpr std::uint32_t health_value_offset = 8U;
  constexpr std::uint32_t object_health_offset = 0x40U;

  const auto &cached_position =
      state.has_previous_position ? state.previous_position : state.position;
  if (state.position.y == std::numeric_limits<std::int32_t>::min() ||
      cached_position.y == std::numeric_limits<std::int32_t>::min()) {
    return false;
  }
  const auto player = resolveLegacyPlayer(runtime_, profile);
  if (!player) {
    return false;
  }

  const auto guest_y = -state.position.y;
  const auto cached_guest_y = -cached_position.y;
  const std::array motion_positions{
      std::pair{motion_position_x_offset, state.position.x},
      std::pair{motion_position_y_offset, guest_y},
      std::pair{motion_position_z_offset, state.position.z},
      std::pair{motion_cached_position_offset + motion_position_x_offset,
                cached_position.x},
      std::pair{motion_cached_position_offset + motion_position_y_offset,
                cached_guest_y},
      std::pair{motion_cached_position_offset + motion_position_z_offset,
                cached_position.z},
  };
  for (const auto [offset, value] : motion_positions) {
    if (!runtime_.write32(player->motion + offset, guestWord(value))) {
      return false;
    }
  }
  if (!runtime_.write32(player->matrix + matrix_translation_x_offset,
                        guestWord(state.position.x)) ||
      !runtime_.write32(player->matrix + matrix_translation_y_offset,
                        guestWord(state.position.y)) ||
      !runtime_.write32(player->matrix + matrix_translation_z_offset,
                        guestWord(state.position.z))) {
    return false;
  }

  if (!writeLegacyPlayerHeading(runtime_, player->matrix, state.yaw)) {
    return false;
  }

  return runtime_.write16(player->health + health_armor_offset,
                          guestHalf(state.armor)) &&
         runtime_.write16(player->health + health_value_offset,
                          guestHalf(state.health)) &&
         runtime_.write16(player->record + object_health_offset,
                          guestHalf(state.health));
}

bool LegacyGameplayVm::writeHostPlayerLocomotion(
    const LegacyHostPlayerLocomotion &state,
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  constexpr std::uint32_t matrix_translation_x_offset = 0x14U;
  constexpr std::uint32_t matrix_translation_z_offset = 0x1cU;
  constexpr std::uint32_t motion_position_x_offset = 0U;
  constexpr std::uint32_t motion_position_y_offset = 4U;
  constexpr std::uint32_t motion_position_z_offset = 8U;
  constexpr std::uint32_t motion_cached_position_offset = 0x40U;

  const auto &cached_position =
      state.has_previous_position ? state.previous_position : state.position;
  if (state.position.y == std::numeric_limits<std::int32_t>::min() ||
      cached_position.y == std::numeric_limits<std::int32_t>::min()) {
    return false;
  }
  const auto player = resolveLegacyPlayer(runtime_, profile);
  if (!player) {
    return false;
  }

  // Motion coordinates drive collision and the immutable presentation bridge.
  // MATRIX X/Z must follow immediately so the rendered actor cannot lag one
  // retail tick behind. MATRIX Y is deliberately pose-owned: replacing it
  // with the collision root makes first-person Gabe/camera fall below ground.
  const auto guest_y = -state.position.y;
  const auto cached_guest_y = -cached_position.y;
  const std::array motion_positions{
      std::pair{motion_position_x_offset, state.position.x},
      std::pair{motion_position_y_offset, guest_y},
      std::pair{motion_position_z_offset, state.position.z},
      std::pair{motion_cached_position_offset + motion_position_x_offset,
                cached_position.x},
      std::pair{motion_cached_position_offset + motion_position_y_offset,
                cached_guest_y},
      std::pair{motion_cached_position_offset + motion_position_z_offset,
                cached_position.z},
  };
  for (const auto [offset, value] : motion_positions) {
    if (!runtime_.write32(player->motion + offset, guestWord(value))) {
      return false;
    }
  }
  return runtime_.write32(player->matrix + matrix_translation_x_offset,
                          guestWord(state.position.x)) &&
         runtime_.write32(player->matrix + matrix_translation_z_offset,
                          guestWord(state.position.z));
}

bool LegacyGameplayVm::writeHostPlayerVitals(
    std::int16_t health, std::int16_t armor,
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  constexpr std::uint32_t health_armor_offset = 6U;
  constexpr std::uint32_t health_value_offset = 8U;
  constexpr std::uint32_t object_health_offset = 0x40U;
  const auto player = resolveLegacyPlayer(runtime_, profile);
  return player &&
         runtime_.write16(player->health + health_armor_offset,
                          guestHalf(armor)) &&
         runtime_.write16(player->health + health_value_offset,
                          guestHalf(health)) &&
         runtime_.write16(player->record + object_health_offset,
                          guestHalf(health));
}

bool LegacyGameplayVm::writeHostPlayerHeading(
    std::int32_t yaw,
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  const auto player = resolveLegacyPlayer(runtime_, profile);
  return player && rotateLegacyPlayerHeading(runtime_, player->matrix, yaw);
}

void LegacyGameplayVm::setHostAimRay(
    std::optional<LegacyHostAimRay> ray) noexcept {
  host_aim_ray_ = std::move(ray);
}

bool LegacyGameplayVm::writeHostPadState(
    const LegacyHostPadState &state,
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  constexpr std::uint8_t analog_pad_type = 7U;
  constexpr std::uint32_t buttons_offset = 4U;
  constexpr std::uint32_t right_x_offset = 6U;
  constexpr std::uint32_t right_y_offset = 7U;
  constexpr std::uint32_t left_x_offset = 8U;
  constexpr std::uint32_t left_y_offset = 9U;
  constexpr std::uint32_t face_horizontal_offset = 0x0cU;
  constexpr std::uint32_t face_vertical_offset = 0x14U;
  constexpr std::uint32_t left_analog_x_offset = 0x1cU;
  constexpr std::uint32_t left_analog_y_offset = 0x24U;
  constexpr std::uint32_t right_analog_x_offset = 0x2cU;
  constexpr std::uint32_t right_analog_y_offset = 0x34U;
  constexpr std::uint16_t triangle = 0x1000U;
  constexpr std::uint16_t circle = 0x2000U;
  constexpr std::uint16_t cross = 0x4000U;
  constexpr std::uint16_t square = 0x8000U;
  const auto axis_x = [](std::uint8_t value) {
    // DualShock reports 0x80 at rest. The old 0x7f origin injected +1 into
    // every neutral horizontal axis and made an untouched pad drift.
    return static_cast<std::int32_t>(value) - 0x80;
  };
  const auto axis_y = [](std::uint8_t value) {
    return 0x80 - static_cast<std::int32_t>(value);
  };
  const auto face_horizontal = ((state.buttons & circle) != 0U ? 0x7f : 0) -
                               ((state.buttons & square) != 0U ? 0x7f : 0);
  const auto face_vertical = ((state.buttons & triangle) != 0U ? 0x7f : 0) -
                             ((state.buttons & cross) != 0U ? 0x7f : 0);
  const auto active_low_buttons = static_cast<std::uint16_t>(~state.buttons);
  if (!runtime_.write8(profile.raw_pad0, 0U) ||
      !runtime_.write8(profile.raw_pad0 + 1U, 0x73U) ||
      // The serial packet stores the conventional Select/D-pad byte first.
      // Retail FUN_800d7aec reconstructs that byte as the high half of its
      // internal mask, so its processed word is byte-swapped relative to the
      // host convention (for example Triangle 0x1000 becomes 0x0010).
      !runtime_.write8(profile.raw_pad0 + 2U,
                       static_cast<std::uint8_t>(active_low_buttons & 0xffU)) ||
      !runtime_.write8(profile.raw_pad0 + 3U,
                       static_cast<std::uint8_t>(active_low_buttons >> 8U)) ||
      !runtime_.write8(profile.raw_pad0 + 4U, state.right_x) ||
      !runtime_.write8(profile.raw_pad0 + 5U, state.right_y) ||
      !runtime_.write8(profile.raw_pad0 + 6U, state.left_x) ||
      !runtime_.write8(profile.raw_pad0 + 7U, state.left_y) ||
      !runtime_.write8(profile.raw_pad1, 0xffU) ||
      !runtime_.write8(profile.processed_pad0, 0U) ||
      !runtime_.write8(profile.processed_pad0 + 1U, analog_pad_type) ||
      !runtime_.write8(profile.processed_pad0 + 2U, 1U) ||
      !runtime_.write8(profile.processed_pad0 + 3U, 0U) ||
      !runtime_.write16(profile.processed_pad0 + buttons_offset,
                        static_cast<std::uint16_t>((state.buttons << 8U) |
                                                   (state.buttons >> 8U))) ||
      !runtime_.write8(profile.processed_pad0 + right_x_offset,
                       state.right_x) ||
      !runtime_.write8(profile.processed_pad0 + right_y_offset,
                       state.right_y) ||
      !runtime_.write8(profile.processed_pad0 + left_x_offset, state.left_x) ||
      !runtime_.write8(profile.processed_pad0 + left_y_offset, state.left_y)) {
    return false;
  }
  for (std::uint32_t offset = 0x0aU; offset < 0x3cU; ++offset) {
    if (!runtime_.write8(profile.processed_pad0 + offset, 0U)) {
      return false;
    }
  }
  return runtime_.write32(profile.processed_pad0 + face_horizontal_offset,
                          guestWord(face_horizontal)) &&
         runtime_.write32(profile.processed_pad0 + face_vertical_offset,
                          guestWord(face_vertical)) &&
         runtime_.write32(profile.processed_pad0 + left_analog_x_offset,
                          guestWord(axis_x(state.left_x))) &&
         runtime_.write32(profile.processed_pad0 + left_analog_y_offset,
                          guestWord(axis_y(state.left_y))) &&
         runtime_.write32(profile.processed_pad0 + right_analog_x_offset,
                          guestWord(axis_x(state.right_x))) &&
         runtime_.write32(profile.processed_pad0 + right_analog_y_offset,
                          guestWord(axis_y(state.right_y)));
}

bool LegacyGameplayVm::writeHostInventoryState(
    const LegacyInventoryBridgeState &state,
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  constexpr std::uint32_t equipped_weapon_offset = 0x24U;
  // FUN_80045554 stores reserve first and the loaded magazine second in each
  // four-byte weapon entry. Native firing is authoritative between VM ticks,
  // so copy it in before the guest applies retail pickup/crate changes.
  const auto player = resolveLegacyPlayer(runtime_, profile);
  if (!player ||
      !runtime_.write32(profile.inventory_current_weapon,
                        state.current_weapon) ||
      !runtime_.write8(player->record + equipped_weapon_offset,
                       state.current_weapon) ||
      !runtime_.write32(profile.inventory_owned_weapons, state.owned_weapons)) {
    return false;
  }
  for (std::size_t weapon = 0U; weapon < legacy_inventory_weapon_count;
       ++weapon) {
    const auto entry =
        profile.inventory_ammo_table + static_cast<std::uint32_t>(weapon * 4U);
    if (!runtime_.write16(entry, state.reserves[weapon]) ||
        !runtime_.write16(entry + 2U, state.magazines[weapon])) {
      return false;
    }
  }
  return true;
}

bool LegacyGameplayVm::setRetailHardMode(bool enabled) noexcept {
  // Retail title code stores this byte before mission bootstrap. Enemy aim and
  // reaction routines read it directly on every update.
  constexpr std::uint32_t hard_mode_flag = 0x801168d0U;
  return runtime_.write8(hard_mode_flag, enabled ? 1U : 0U);
}

bool LegacyGameplayVm::setAgentDifficulty(bool enabled) noexcept {
  agent_difficulty_ = enabled;
  if (!enabled) {
    clearAgentHeadshotThreat();
    agent_cbdc_friendly_fire_frame_.reset();
    agent_cbdc_friendly_fire_pending_penalties_ = 0U;
  }
  return true;
}

bool LegacyGameplayVm::applyAgentMissionNpcOverrides(
    std::uint32_t mission_index, bool enabled,
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  if (mission_index == 15U) {
    if (!enabled) {
      return true;
    }
    constexpr std::uint32_t object_record_stride = 0x4cU;
    constexpr std::uint32_t object_definition_stride = 0x14U;
    constexpr std::uint32_t object_instance_offset = 0x34U;
    constexpr std::uint32_t instance_slot_offset = 2U;
    constexpr std::uint32_t instance_health_offset = 0x18U;
    constexpr std::uint32_t instance_ai_offset = 0x1cU;
    constexpr std::uint32_t health_value_offset = 8U;
    constexpr std::uint32_t grenade_counter_offset = 0x4aU;
    constexpr std::uint32_t common_npc_handler = 0x80061874U;
    std::uint32_t records{};
    std::uint32_t count{};
    std::uint32_t definitions{};
    std::uint32_t definition_count{};
    std::uint32_t handler{};
    if (!runtime_.read32(profile.object_records_pointer, records) ||
        !runtime_.read32(profile.object_count, count) ||
        !runtime_.read32(profile.object_definitions_pointer, definitions) ||
        !runtime_.read32(profile.object_definition_count, definition_count) ||
        !runtime_.read32(profile.object_handler_table + 4U, handler) ||
        count > profile.maximum_objects ||
        definition_count > profile.maximum_definitions ||
        handler != common_npc_handler) {
      return true;
    }
    for (std::uint32_t slot = 0U; slot < count; ++slot) {
      const auto record64 =
          static_cast<std::uint64_t>(records) +
          static_cast<std::uint64_t>(slot) * object_record_stride;
      if (record64 > std::numeric_limits<std::uint32_t>::max()) {
        continue;
      }
      const auto record = static_cast<std::uint32_t>(record64);
      std::uint32_t definition{};
      std::uint16_t attributes{};
      std::uint32_t instance{};
      if (!validGuestRamRange(record, object_record_stride) ||
          !runtime_.read32(record, definition) ||
          definition >= definition_count ||
          !runtime_.read16(record + 0x24U, attributes) ||
          !runtime_.read32(record + object_instance_offset, instance)) {
        continue;
      }
      const auto definition64 =
          static_cast<std::uint64_t>(definitions) +
          static_cast<std::uint64_t>(definition) * object_definition_stride;
      if (definition64 > std::numeric_limits<std::uint32_t>::max()) {
        continue;
      }
      std::uint16_t object_class{};
      std::uint16_t live_slot{};
      std::uint32_t health_controller{};
      std::uint32_t ai{};
      std::uint16_t health_bits{};
      std::uint8_t grenade_counter{};
      if (!validGuestRamRange(static_cast<std::uint32_t>(definition64),
                              object_definition_stride) ||
          !runtime_.read16(static_cast<std::uint32_t>(definition64),
                           object_class) ||
          !validGuestRamRange(instance, instance_ai_offset + 4U) ||
          !runtime_.read16(instance + instance_slot_offset, live_slot) ||
          live_slot != slot ||
          !runtime_.read32(instance + instance_health_offset,
                           health_controller) ||
          !runtime_.read32(instance + instance_ai_offset, ai) ||
          !validGuestRamRange(health_controller, health_value_offset + 2U) ||
          !runtime_.read16(health_controller + health_value_offset,
                           health_bits) ||
          !validGuestRamRange(ai, grenade_counter_offset + 1U) ||
          !runtime_.read8(ai + grenade_counter_offset, grenade_counter)) {
        continue;
      }
      const auto eligible = agentEliteGuardGrenadeCadenceEligible(
          15U, attributes, object_class == 1U, true,
          std::bit_cast<std::int16_t>(health_bits) > 0);
      const auto adjusted =
          agentEliteGuardGrenadeDecisionCounter(grenade_counter, eligible);
      if (adjusted != grenade_counter &&
          !runtime_.write8(ai + grenade_counter_offset, adjusted)) {
        return false;
      }
    }
    return true;
  }
  if (mission_index == agent_gabrek_identity.mission ||
      mission_index == agent_chapel_guard_identities.front().mission) {
    constexpr std::uint32_t object_record_stride = 0x4cU;
    constexpr std::uint32_t object_definition_stride = 0x14U;
    constexpr std::uint32_t common_npc_handler = 0x80061874U;
    std::uint32_t records{};
    std::uint32_t count{};
    std::uint32_t definitions{};
    std::uint32_t definition_count{};
    std::uint32_t handler{};
    if (!runtime_.read32(profile.object_records_pointer, records) ||
        !runtime_.read32(profile.object_count, count) ||
        !runtime_.read32(profile.object_definitions_pointer, definitions) ||
        !runtime_.read32(profile.object_definition_count, definition_count) ||
        !runtime_.read32(profile.object_handler_table + 4U, handler) ||
        count > profile.maximum_objects ||
        definition_count > profile.maximum_definitions ||
        handler != common_npc_handler) {
      // Mission and room tables are replaced transactionally. Retry after the
      // next coherent outer frame instead of touching a partial table.
      return true;
    }

    const auto apply_identity = [&](const AgentMissionNpcIdentity &identity,
                                    bool gabrek) {
      if (identity.mission != mission_index || count <= identity.slot ||
          definition_count <= identity.definition) {
        return true;
      }
      const auto record64 = static_cast<std::uint64_t>(records) +
                            identity.slot * object_record_stride;
      const auto definition64 = static_cast<std::uint64_t>(definitions) +
                                identity.definition * object_definition_stride;
      if (record64 > std::numeric_limits<std::uint32_t>::max() ||
          definition64 > std::numeric_limits<std::uint32_t>::max()) {
        return true;
      }
      const auto record = static_cast<std::uint32_t>(record64);
      const auto definition_address = static_cast<std::uint32_t>(definition64);
      std::uint32_t definition{};
      std::uint16_t attributes{};
      std::uint16_t class_id{};
      if (!validGuestRamRange(record, object_record_stride) ||
          !runtime_.read32(record, definition) ||
          definition != identity.definition ||
          !runtime_.read16(record + 0x24U, attributes) ||
          !validGuestRamRange(definition_address, object_definition_stride) ||
          !runtime_.read16(definition_address, class_id) || class_id != 1U) {
        return true;
      }

      if (gabrek) {
        // BASEEXT bootstrap moves Gabrek from his DAT root before maintained
        // overrides run. Spawn matching above remains on the authored tuple.
        constexpr LegacyNativePoint expected_position{-817, 0, -7044};
        std::uint32_t authored_x{};
        std::uint32_t authored_y{};
        std::uint32_t authored_z{};
        if (!runtime_.read32(record + 0x18U, authored_x) ||
            !runtime_.read32(record + 0x1cU, authored_y) ||
            !runtime_.read32(record + 0x20U, authored_z) ||
            std::bit_cast<std::int32_t>(authored_x) != expected_position.x ||
            std::bit_cast<std::int32_t>(authored_y) != expected_position.y ||
            std::bit_cast<std::int32_t>(authored_z) != expected_position.z) {
          return true;
        }
      } else {
        constexpr std::uint32_t object_instance_offset = 0x34U;
        constexpr std::uint32_t instance_slot_offset = 2U;
        std::uint32_t instance{};
        std::uint16_t instance_slot{};
        if (!agentChapelGuardMaintainedAttributesEligible(
                attributes, identity.retail_attributes) ||
            !runtime_.read32(record + object_instance_offset, instance) ||
            instance == 0U ||
            !validGuestRamRange(instance, instance_slot_offset + 2U) ||
            !runtime_.read16(instance + instance_slot_offset, instance_slot) ||
            instance_slot != identity.slot) {
          return true;
        }
      }

      const auto adjusted =
          gabrek ? agentGabrekAttributes(attributes, enabled)
                 : agentChapelGuardAttributes(
                       attributes, identity.retail_attributes, enabled);
      return adjusted == attributes ||
             runtime_.write16(record + 0x24U, adjusted);
    };

    if (mission_index == agent_gabrek_identity.mission) {
      return apply_identity(agent_gabrek_identity, true);
    }
    for (const auto &identity : agent_chapel_guard_identities) {
      if (!apply_identity(identity, false)) {
        return false;
      }
    }
    return true;
  }

  struct Rule {
    std::uint32_t slot;
    std::uint32_t definition;
    LegacyNativePoint authored_position;
  };
  std::optional<Rule> rule;
  if (mission_index == 0U) {
    // SUBWAY bootstrap transforms Kravitch's authored DAT root before the
    // maintained override sees the live record.
    rule = Rule{174U, 53U, LegacyNativePoint{-1495, -2140, 6679}};
  } else if (mission_index == 3U) {
    // PARK bootstrap likewise moves Marcos away from his authored DAT root.
    // The spawn hook still matches the authored tuple before this transform.
    rule = Rule{48U, 11U, LegacyNativePoint{5825, 0, 15855}};
  } else {
    return true;
  }

  constexpr std::uint32_t object_record_stride = 0x4cU;
  constexpr std::uint32_t object_definition_stride = 0x14U;
  constexpr std::uint32_t common_npc_handler = 0x80061874U;
  std::uint32_t records{};
  std::uint32_t count{};
  std::uint32_t definitions{};
  std::uint32_t definition_count{};
  if (!runtime_.read32(profile.object_records_pointer, records) ||
      !runtime_.read32(profile.object_count, count) ||
      !runtime_.read32(profile.object_definitions_pointer, definitions) ||
      !runtime_.read32(profile.object_definition_count, definition_count) ||
      count <= rule->slot || count > profile.maximum_objects ||
      definition_count <= rule->definition ||
      definition_count > profile.maximum_definitions) {
    // Room streaming swaps these tables transactionally. The immutable
    // bridge will report persistent corruption; an override simply retries.
    return true;
  }

  const auto record64 =
      static_cast<std::uint64_t>(records) + rule->slot * object_record_stride;
  const auto definition64 = static_cast<std::uint64_t>(definitions) +
                            rule->definition * object_definition_stride;
  if (record64 > std::numeric_limits<std::uint32_t>::max() ||
      definition64 > std::numeric_limits<std::uint32_t>::max()) {
    return true;
  }
  const auto record = static_cast<std::uint32_t>(record64);
  const auto definition_address = static_cast<std::uint32_t>(definition64);
  std::uint32_t definition{};
  std::uint32_t authored_x{};
  std::uint32_t authored_y{};
  std::uint32_t authored_z{};
  std::uint16_t attributes{};
  std::uint16_t class_id{};
  std::uint32_t handler{};
  if (!validGuestRamRange(record, object_record_stride) ||
      !runtime_.read32(record, definition) || definition != rule->definition ||
      !runtime_.read32(record + 0x18U, authored_x) ||
      !runtime_.read32(record + 0x1cU, authored_y) ||
      !runtime_.read32(record + 0x20U, authored_z) ||
      std::bit_cast<std::int32_t>(authored_x) != rule->authored_position.x ||
      std::bit_cast<std::int32_t>(authored_y) != rule->authored_position.y ||
      std::bit_cast<std::int32_t>(authored_z) != rule->authored_position.z ||
      !runtime_.read16(record + 0x24U, attributes) ||
      !validGuestRamRange(definition_address, object_definition_stride) ||
      !runtime_.read16(definition_address, class_id) || class_id != 1U ||
      !runtime_.read32(profile.object_handler_table + 4U, handler) ||
      handler != common_npc_handler) {
    return true;
  }

  const auto adjusted = mission_index == 0U
                            ? agentKravitchAttributes(attributes, enabled)
                            : agentMarcosAttributes(attributes, enabled);
  if (adjusted != attributes && !runtime_.write16(record + 0x24U, adjusted)) {
    return false;
  }
  if (mission_index != 3U || !enabled) {
    return true;
  }

  // FUN_8005d088 increments ai+0x4a and makes a retail grenade decision at
  // 0x3d. Raising only Marcos's post-reset floor keeps the original random
  // choice, animation, projectile ownership and safety gates, while reducing
  // the average delay between his ordinary-frag attempts.
  constexpr std::uint32_t record_instance_offset = 0x34U;
  constexpr std::uint32_t instance_slot_offset = 2U;
  constexpr std::uint32_t instance_ai_offset = 0x1cU;
  constexpr std::uint32_t grenade_counter_offset = 0x4aU;
  std::uint32_t instance{};
  std::uint16_t instance_slot{};
  std::uint32_t ai{};
  std::uint8_t grenade_counter{};
  if (!runtime_.read32(record + record_instance_offset, instance) ||
      !validGuestRamRange(instance, instance_ai_offset + 4U) ||
      !runtime_.read16(instance + instance_slot_offset, instance_slot) ||
      instance_slot != rule->slot ||
      !runtime_.read32(instance + instance_ai_offset, ai) ||
      !validGuestRamRange(ai, grenade_counter_offset + 1U) ||
      !runtime_.read8(ai + grenade_counter_offset, grenade_counter)) {
    // The actor is not live yet or room streaming is swapping ownership.
    // The maintained override retries on the next coherent outer frame.
    return true;
  }
  const auto accelerated =
      agentMarcosGrenadeDecisionCounter(grenade_counter, true);
  return accelerated == grenade_counter ||
         runtime_.write8(ai + grenade_counter_offset, accelerated);
}

bool LegacyGameplayVm::applyAgentMissionTimer(
    std::uint32_t mission_index,
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  constexpr std::uint32_t pending_seconds_address = 0x8011669cU;
  constexpr std::uint32_t expiry_callback_address = 0x801166a4U;
  constexpr std::uint32_t active_expiry_callback_address = 0x80116698U;
  constexpr std::uint32_t active_timer_setter = 0x8004027cU;
  constexpr std::uint64_t execution_budget = 1'000'000U;

  if (!agent_difficulty_) {
    agent_cbdc_friendly_fire_frame_.reset();
    agent_cbdc_friendly_fire_pending_penalties_ = 0U;
    return true;
  }
  if (mission_index != 3U &&
      agent_cbdc_friendly_fire_pending_penalties_ != 0U) {
    agent_cbdc_friendly_fire_frame_.reset();
    agent_cbdc_friendly_fire_pending_penalties_ = 0U;
  }
  const auto *rule = agentMissionTimerRule(mission_index);
  if (rule == nullptr) {
    return true;
  }
  const auto pending_penalties =
      mission_index == 3U ? agent_cbdc_friendly_fire_pending_penalties_ : 0U;
  std::uint16_t handle{};
  std::uint32_t pending_seconds{};
  std::uint32_t pending_expiry_callback{};
  std::uint32_t active_expiry_callback{};
  std::uint32_t remaining_bits{};
  if (!runtime_.read16(profile.mission_timer_handle, handle) ||
      !runtime_.read32(pending_seconds_address, pending_seconds) ||
      !runtime_.read32(expiry_callback_address, pending_expiry_callback) ||
      !runtime_.read32(active_expiry_callback_address,
                       active_expiry_callback) ||
      !runtime_.read32(profile.mission_timer_remaining, remaining_bits)) {
    return false;
  }
  if (handle == 0xffffU) {
    agent_cbdc_friendly_fire_pending_penalties_ = 0U;
    agent_cbdc_friendly_fire_frame_.reset();
    // FUN_80040154 delays creation by 0x14 ticks. Change its pending seconds
    // only for the exact mission callback and retail duration. The retail
    // callback still creates and owns the timer, HUD and failure transition.
    return pending_expiry_callback != rule->expiry_callback ||
           pending_seconds != rule->retail_seconds ||
           runtime_.write32(pending_seconds_address, rule->agent_seconds);
  }
  if (active_expiry_callback != rule->expiry_callback) {
    agent_cbdc_friendly_fire_pending_penalties_ = 0U;
    agent_cbdc_friendly_fire_frame_.reset();
    return true;
  }

  const auto current = std::bit_cast<std::int32_t>(remaining_bits);
  auto adjusted = agentMissionTimerAdjustedTicks(current, *rule);
  adjusted = agentCbdcFriendlyFireAdjustedTicks(adjusted, pending_penalties);
  if (adjusted == current) {
    agent_cbdc_friendly_fire_pending_penalties_ = 0U;
    return true;
  }
  try {
    const std::array arguments{std::bit_cast<std::uint32_t>(adjusted)};
    const auto completed =
        invoke(active_timer_setter, arguments, execution_budget).completed();
    if (completed) {
      agent_cbdc_friendly_fire_pending_penalties_ = 0U;
    }
    return completed;
  } catch (...) {
    return false;
  }
}

bool LegacyGameplayVm::applyAgentWashingtonParkTimer(
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  return applyAgentMissionTimer(3U, profile);
}

LegacyGameplayVmResult
LegacyGameplayVm::invokeRetailPark2BombFailure(std::uint64_t execution_budget) {
  // PARK2 FUN_80146ac0 calls the common story-failure wrapper with parameter
  // zero and sound 0x6b when its class-0x2e BOMB actor dies. Re-enter that
  // exact resident boundary for the Agent detonation meter.
  constexpr std::uint32_t story_failure_entry = 0x80091b9cU;
  constexpr std::array arguments{0U, 0x6bU, 0U};
  return invoke(story_failure_entry, arguments, execution_budget);
}

bool LegacyGameplayVm::updateAgentGrenadeAwareness(
    const LegacyGameplayBridgeState &state,
    const LegacyAgentGrenadeAwarenessProfile &profile,
    std::uint64_t execution_budget) noexcept {
  constexpr std::uint8_t player_grenade_danger = 1U;
  if (!agent_difficulty_ || !state.thrown_projectile) {
    return true;
  }
  const auto &projectile = *state.thrown_projectile;
  if ((projectile.weapon != 19U && projectile.weapon != 20U) ||
      projectile.age > 60U || profile.alert_entry == 0U) {
    return true;
  }

  std::uint8_t danger_mask{};
  if (!runtime_.read8(profile.danger_mask, danger_mask)) {
    return false;
  }
  if ((danger_mask & player_grenade_danger) != 0U) {
    return true;
  }

  try {
    const std::array arguments{0U}; // FUN_800591fc: player grenade owner
    return invoke(profile.alert_entry, arguments, execution_budget).completed();
  } catch (...) {
    return false;
  }
}

bool LegacyGameplayVm::updateAgentHeadshotThreat(
    const LegacyGameplayBridgeState &state, std::int16_t player_slot,
    const LegacyNativeMissionBridgeProfile &profile) noexcept {
  if (!agent_difficulty_) {
    clearAgentHeadshotThreat();
    return true;
  }

  std::uint32_t gameplay_frame{};
  if (player_slot < 0 ||
      !runtime_.read32(profile.gameplay_frame, gameplay_frame)) {
    clearAgentHeadshotThreat();
    return false;
  }

  constexpr std::uint32_t common_npc_handler = 0x80061874U;
  constexpr std::uint32_t active_combat_flag = 0x200U;
  constexpr std::uint32_t invalid_target_flag = 0x04U;
  constexpr std::uint32_t danger_value_mask = 0x3fffU;
  constexpr std::uint8_t dormant_instance_flag = 0x02U;
  constexpr std::uint8_t svd_weapon = 12U;
  constexpr std::uint8_t sniper_weapon = 13U;
  constexpr std::uint32_t warning_frames = 20U;
  auto current_engagements =
      std::vector<AgentHeadshotEngagement>(state.objects.size());
  const auto tracked_by_retail = [&state](std::uint32_t slot) {
    return std::ranges::find(
               state.tracked_slots,
               static_cast<std::int16_t>(static_cast<std::uint16_t>(slot))) !=
           state.tracked_slots.end();
  };
  const auto eligible = [&](const LegacyObjectBridgeState &object) {
    const auto weapon = static_cast<std::uint8_t>(object.attributes);
    return object.slot < current_engagements.size() &&
           object.slot != static_cast<std::uint32_t>(player_slot) &&
           object.instance != 0U && !object.destroyed() && object.has_target &&
           tracked_by_retail(object.slot) &&
           object.object_handler == common_npc_handler &&
           object.target_controller != 0U && object.ai_controller != 0U &&
           object.target_slot == player_slot && object.simulated &&
           (object.danger_q12 & danger_value_mask) != 0U &&
           (object.ai_flags & active_combat_flag) != 0U &&
           (object.instance_state[3] & dormant_instance_flag) == 0U &&
           (object.target_flags & invalid_target_flag) == 0U &&
           (weapon == svd_weapon || weapon == sniper_weapon);
  };
  const auto same_identity = [](const AgentHeadshotEngagement &engagement,
                                const LegacyObjectBridgeState &object,
                                std::uint8_t weapon) {
    return engagement.instance == object.instance &&
           engagement.ai_controller == object.ai_controller &&
           engagement.weapon == weapon;
  };

  for (const auto &object : state.objects) {
    if (!eligible(object)) {
      continue;
    }
    const auto weapon = static_cast<std::uint8_t>(object.attributes);
    auto engagement = AgentHeadshotEngagement{
        object.instance,
        object.ai_controller,
        weapon,
        false,
    };
    if (object.slot < agent_headshot_engagements_.size()) {
      const auto &previous = agent_headshot_engagements_[object.slot];
      if (same_identity(previous, object, weapon)) {
        engagement.consumed = previous.consumed;
      }
    }
    current_engagements[object.slot] = engagement;
  }

  const auto clear_active = [this] {
    agent_headshot_shooter_slot_ = -1;
    agent_headshot_shooter_instance_ = 0U;
    agent_headshot_shooter_ai_controller_ = 0U;
    agent_headshot_weapon_ = 0U;
    agent_headshot_ready_frame_ = 0U;
  };
  if (agent_headshot_shooter_slot_ >= 0) {
    const auto slot = static_cast<std::size_t>(agent_headshot_shooter_slot_);
    const auto active = slot < current_engagements.size()
                            ? &current_engagements[slot]
                            : nullptr;
    if (active == nullptr || active->consumed ||
        active->instance != agent_headshot_shooter_instance_ ||
        active->ai_controller != agent_headshot_shooter_ai_controller_ ||
        active->weapon != agent_headshot_weapon_) {
      clear_active();
    }
  }

  if (agent_headshot_shooter_slot_ < 0) {
    for (const auto &object : state.objects) {
      if (!eligible(object)) {
        continue;
      }
      const auto &engagement = current_engagements[object.slot];
      if (engagement.consumed) {
        continue;
      }
      agent_headshot_shooter_slot_ = static_cast<std::int16_t>(object.slot);
      agent_headshot_shooter_instance_ = engagement.instance;
      agent_headshot_shooter_ai_controller_ = engagement.ai_controller;
      agent_headshot_weapon_ = engagement.weapon;
      agent_headshot_ready_frame_ = gameplay_frame + warning_frames;
      break;
    }
  }

  agent_headshot_engagements_ = std::move(current_engagements);
  return true;
}

void LegacyGameplayVm::clearAgentHeadshotThreat() noexcept {
  agent_headshot_engagements_.clear();
  agent_headshot_shooter_slot_ = -1;
  agent_headshot_shooter_instance_ = 0U;
  agent_headshot_shooter_ai_controller_ = 0U;
  agent_headshot_weapon_ = 0U;
  agent_headshot_ready_frame_ = 0U;
}

bool LegacyGameplayVm::setRetailOneShotKills(bool enabled) noexcept {
  // MENU.OVL's 9mm super-ammo action toggles this resident byte. The common
  // damage path consumes it for every player firearm, despite the historical
  // cheat being entered while the Silenced 9mm is highlighted.
  constexpr std::uint32_t super_ammo_flag = 0x801168d1U;
  return runtime_.write8(super_ammo_flag, enabled ? 1U : 0U);
}

bool LegacyGameplayVm::weakenRetailEnemySlots(
    std::span<const std::uint32_t> slots,
    const LegacyGameplayBridgeProfile &profile) noexcept {
  constexpr std::uint32_t object_record_stride = 0x4cU;
  constexpr std::uint32_t object_health_offset = 0x40U;
  std::uint32_t records{};
  std::uint32_t count_bits{};
  if (!runtime_.read32(profile.object_records_pointer, records) ||
      !runtime_.read32(profile.object_count, count_bits) || records == 0U) {
    return false;
  }
  const auto count = std::bit_cast<std::int32_t>(count_bits);
  if (count < 0 ||
      static_cast<std::uint32_t>(count) > profile.maximum_objects) {
    return false;
  }
  for (const auto slot : slots) {
    if (slot >= static_cast<std::uint32_t>(count) ||
        !runtime_.write16(
            records + slot * object_record_stride + object_health_offset, 1U)) {
      return false;
    }
  }
  return true;
}

bool LegacyGameplayVm::synchronizeHostRoom(
    std::int16_t room, const LegacyNativeMissionBridgeProfile &profile,
    std::uint64_t execution_budget) noexcept {
  try {
    std::uint16_t current_bits{};
    if (!runtime_.read16(profile.current_room, current_bits)) {
      return false;
    }
    if (std::bit_cast<std::int16_t>(current_bits) == room) {
      return true;
    }
    if (profile.stream_unlock_entry != 0U &&
        !invoke(profile.stream_unlock_entry, {}, execution_budget)
             .completed()) {
      return false;
    }
    const std::array arguments{guestArgument(room)};
    return invoke(profile.room_change_entry, arguments, execution_budget)
        .completed();
  } catch (...) {
    return false;
  }
}

LegacyGameplayVmResult LegacyGameplayVm::queueHostDamage(
    const LegacyHostDamageEvent &event,
    const LegacyNativeMissionBridgeProfile &profile,
    std::uint64_t execution_budget) {
  const std::array arguments{
      guestArgument(event.attacker_slot), guestArgument(event.owner_slot),
      guestArgument(event.target_slot),   guestArgument(event.damage),
      guestArgument(event.damage_type),
  };
  return invoke(profile.damage_entry, arguments, execution_budget);
}

LegacyGameplayVmResult LegacyGameplayVm::queueHostImpact(
    std::int16_t attacker_slot, std::int16_t target_slot,
    const LegacyNativeMissionBridgeProfile &profile,
    std::uint64_t execution_budget) {
  // Retail weapon collision posts event 0x0d before applying health damage.
  // Static handlers consume this event directly (LOCK uses it to enqueue the
  // linked GATE event), while actor health continues through FUN_80069cb0.
  const std::array arguments{
      0x0dU,
      4U,
      guestArgument(attacker_slot),
      guestArgument(target_slot),
      0U,
      0U,
      0U,
      0U,
  };
  return invoke(profile.event_entry, arguments, execution_budget);
}

LegacyGameplayVmResult LegacyGameplayVm::queueHostInteraction(
    std::int16_t target_slot, const LegacyNativeMissionBridgeProfile &profile,
    std::uint64_t execution_budget) {
  const auto target = guestArgument(target_slot);
  const std::array arguments{
      0x12U, 3U, target, target, 0U, 0U, 0U, 0U,
  };
  return invoke(profile.event_entry, arguments, execution_budget);
}

LegacyGameplayVmResult LegacyGameplayVm::invokeRetailWeaponMenuInput(
    bool held, std::int32_t delta,
    const LegacyNativeMissionBridgeProfile &profile,
    std::uint64_t execution_budget) {
  const std::array arguments{
      held ? 1U : 0U,
      std::bit_cast<std::uint32_t>(delta),
  };
  return invoke(profile.weapon_menu_input_entry, arguments, execution_budget);
}

LegacyGameplayVmResult LegacyGameplayVm::invokeRetailFirstPersonAim(
    bool active, const LegacyNativeMissionBridgeProfile &profile,
    std::uint64_t execution_budget) {
  const std::array arguments{active ? 1U : 0U};
  return invoke(profile.first_person_aim_input_entry, arguments,
                execution_budget);
}

std::optional<LegacyMissionBridgeState>
LegacyGameplayVm::readMissionBridgeState(
    const LegacyNativeMissionBridgeProfile &profile) const noexcept {
  constexpr std::uint32_t health_armor_offset = 6U;
  constexpr std::uint32_t health_value_offset = 8U;
  constexpr std::uint32_t objective_count_offset = 0U;
  constexpr std::uint32_t objective_text_table_offset = 4U;
  constexpr std::uint32_t parameter_count_offset = 8U;
  constexpr std::uint32_t parameter_text_table_offset = 0x0cU;
  constexpr std::uint32_t completed_objectives_offset = 0x10U;
  constexpr std::uint32_t failed_objectives_offset = 0x14U;
  constexpr std::uint32_t revealed_objectives_offset = 0x18U;
  constexpr std::uint32_t notified_objectives_offset = 0x1cU;
  constexpr std::uint32_t failed_parameters_offset = 0x20U;
  constexpr std::uint32_t parameter_mask_offset = 0x24U;
  constexpr std::uint32_t ram_begin = 0x80000000U;
  constexpr std::uint64_t ram_end = 0x80200000ULL;
  constexpr std::size_t maximum_text_length = 256U;

  const auto readable_ram_range = [](std::uint32_t address,
                                     std::size_t size) noexcept {
    const auto end = static_cast<std::uint64_t>(address) + size;
    return address >= ram_begin && end <= ram_end;
  };
  const auto read_text_table = [this, &readable_ram_range](
                                   std::uint32_t table, std::uint32_t count,
                                   std::vector<std::string> &texts) noexcept {
    if (count > legacy_mission_entry_limit) {
      return false;
    }
    texts.clear();
    if (count == 0U) {
      return true;
    }
    const auto table_size = static_cast<std::size_t>(count) * 4U;
    if ((table & 3U) != 0U || !readable_ram_range(table, table_size)) {
      return false;
    }
    try {
      texts.reserve(count);
      for (std::uint32_t index = 0U; index < count; ++index) {
        std::uint32_t pointer{};
        if (!runtime_.read32(table + index * 4U, pointer) ||
            !readable_ram_range(pointer, 1U)) {
          return false;
        }
        std::string text;
        text.reserve(maximum_text_length);
        auto terminated = false;
        for (std::size_t offset = 0U; offset < maximum_text_length; ++offset) {
          const auto address = static_cast<std::uint64_t>(pointer) + offset;
          if (address >= ram_end) {
            return false;
          }
          std::uint8_t character{};
          if (!runtime_.read8(static_cast<std::uint32_t>(address), character)) {
            return false;
          }
          if (character == 0U) {
            terminated = true;
            break;
          }
          if (character != '\n' && character != '\r' && character != '\t' &&
              (character < 0x20U || character > 0x7eU)) {
            return false;
          }
          text.push_back(static_cast<char>(character));
        }
        if (!terminated || text.empty()) {
          return false;
        }
        texts.push_back(std::move(text));
      }
    } catch (...) {
      return false;
    }
    return texts.size() == count;
  };

  const auto player = resolveLegacyPlayer(runtime_, profile);
  std::uint32_t progress{};
  if (!player || !runtime_.read32(profile.mission_progress_pointer, progress) ||
      progress == 0U) {
    return std::nullopt;
  }

  LegacyMissionBridgeState state;
  state.player_slot = static_cast<std::int16_t>(player->slot);
  std::uint16_t health{};
  std::uint16_t armor{};
  std::uint8_t terminal{};
  std::uint8_t transition_started{};
  std::uint8_t transition{};
  std::uint8_t failure{};
  std::uint8_t completed{};
  std::uint32_t current_weapon{};
  std::uint32_t weapon_menu_state{};
  std::uint8_t weapon_menu_dirty{};
  std::uint32_t normal_hud_phase_bits{};
  std::uint32_t weapon_menu_controller_ready{};
  std::uint32_t first_person_aim_mode{};
  std::uint32_t scope_camera_controller{};
  std::uint32_t scope_zoom_bits{};
  std::uint32_t weapon_menu_input_ready{};
  std::uint32_t objective_text_table{};
  std::uint32_t parameter_text_table{};
  const auto read_scope_camera = [&] {
    if (first_person_aim_mode == 2U) {
      scope_camera_controller = profile.sniper_scope_camera_controller;
      return true;
    }
    return runtime_.read32(profile.scope_camera_controller_pointer,
                           scope_camera_controller);
  };
  if (!runtime_.read16(player->health + health_value_offset, health) ||
      !runtime_.read16(player->health + health_armor_offset, armor) ||
      !runtime_.read32(progress + objective_count_offset,
                       state.objective_count) ||
      !runtime_.read32(progress + objective_text_table_offset,
                       objective_text_table) ||
      !runtime_.read32(progress + parameter_count_offset,
                       state.parameter_count) ||
      !runtime_.read32(progress + parameter_text_table_offset,
                       parameter_text_table) ||
      !runtime_.read32(progress + completed_objectives_offset,
                       state.completed_objectives) ||
      !runtime_.read32(progress + failed_objectives_offset,
                       state.failed_objectives) ||
      !runtime_.read32(progress + revealed_objectives_offset,
                       state.revealed_objectives) ||
      !runtime_.read32(progress + notified_objectives_offset,
                       state.notified_objectives) ||
      !runtime_.read32(progress + failed_parameters_offset,
                       state.failed_parameters) ||
      !runtime_.read32(progress + parameter_mask_offset,
                       state.parameter_mask) ||
      !runtime_.read8(profile.mission_terminal_latch, terminal) ||
      !runtime_.read8(profile.mission_success_latch, transition_started) ||
      !runtime_.read8(profile.mission_failure_flag, failure) ||
      !runtime_.read8(profile.mission_completed_flag, completed) ||
      !runtime_.read32(profile.inventory_current_weapon, current_weapon) ||
      !runtime_.read32(profile.weapon_menu_state, weapon_menu_state) ||
      !runtime_.read8(profile.weapon_menu_dirty, weapon_menu_dirty) ||
      !runtime_.read32(profile.normal_hud_phase, normal_hud_phase_bits) ||
      !runtime_.read32(profile.weapon_menu_controller_ready,
                       weapon_menu_controller_ready) ||
      !runtime_.read32(profile.first_person_aim_mode, first_person_aim_mode) ||
      !read_scope_camera() || scope_camera_controller == 0U ||
      !runtime_.read32(scope_camera_controller + profile.scope_zoom_offset,
                       scope_zoom_bits) ||
      !runtime_.read32(profile.weapon_menu_input_ready,
                       weapon_menu_input_ready) ||
      !runtime_.read32(profile.inventory_owned_weapons,
                       state.inventory.owned_weapons) ||
      !runtime_.read8(profile.mission_transition_latch, transition)) {
    return std::nullopt;
  }
  if (!read_text_table(objective_text_table, state.objective_count,
                       state.objective_texts) ||
      !read_text_table(parameter_text_table, state.parameter_count,
                       state.parameter_texts)) {
    return std::nullopt;
  }
  state.inventory.current_weapon = static_cast<std::uint8_t>(current_weapon);
  state.weapon_menu_state = std::bit_cast<std::int32_t>(weapon_menu_state);
  state.weapon_menu_dirty = weapon_menu_dirty != 0U;
  state.normal_hud_phase = std::bit_cast<std::int32_t>(normal_hud_phase_bits);
  if (state.normal_hud_phase < -1 || state.normal_hud_phase > 13) {
    return std::nullopt;
  }
  state.interface_mode =
      static_cast<std::uint8_t>(weapon_menu_controller_ready);
  state.first_person_aim_mode =
      static_cast<std::uint8_t>(first_person_aim_mode);
  state.scope_zoom_raw = std::bit_cast<std::int32_t>(scope_zoom_bits);
  state.weapon_menu_controller_ready = weapon_menu_controller_ready == 1U;
  state.weapon_menu_input_ready = weapon_menu_input_ready == 1U;
  for (std::size_t weapon = 0U; weapon < legacy_inventory_weapon_count;
       ++weapon) {
    const auto entry =
        profile.inventory_ammo_table + static_cast<std::uint32_t>(weapon * 4U);
    if (!runtime_.read16(entry, state.inventory.reserves[weapon]) ||
        !runtime_.read16(entry + 2U, state.inventory.magazines[weapon])) {
      return std::nullopt;
    }
  }
  state.player_health = std::bit_cast<std::int16_t>(health);
  state.player_armor = std::bit_cast<std::int16_t>(armor);
  state.terminal = terminal != 0U;
  state.success = state.terminal && transition_started != 0U &&
                  completed != 0U && failure == 0U;
  state.failure = state.terminal && failure != 0U && !state.success;
  state.failure_transition = state.failure && transition != 0U;

  // Sample the completed retail FONT/TEXT state, not the text-builder edge.
  // FUN_800869ec has already advanced reveal/fade and written the live glyph
  // packets by this point, so these values encode the exact 20 Hz animation.
  const auto read_glyphs =
      [this, &readable_ram_range,
       &profile](std::uint32_t text_object, std::uint32_t glyph_pool,
                 std::uint32_t glyph_capacity,
                 std::vector<LegacyUiGlyphBridgeState> &glyphs) noexcept {
        std::uint32_t glyph_pointer{};
        std::uint16_t glyph_count{};
        if (!runtime_.read32(text_object, glyph_pointer) ||
            !runtime_.read16(text_object + 0x0cU, glyph_count) ||
            profile.glyph_stride < 0x17U || glyph_count > glyph_capacity) {
          return false;
        }
        glyphs.clear();
        if (glyph_count == 0U) {
          return true;
        }
        const auto pool_size =
            static_cast<std::uint64_t>(glyph_capacity) * profile.glyph_stride;
        const auto offset =
            static_cast<std::uint64_t>(glyph_pointer) - glyph_pool;
        if (glyph_pointer < glyph_pool || offset % profile.glyph_stride != 0U ||
            offset + static_cast<std::uint64_t>(glyph_count) *
                         profile.glyph_stride >
                pool_size ||
            !readable_ram_range(glyph_pointer,
                                static_cast<std::size_t>(glyph_count) *
                                    profile.glyph_stride)) {
          return false;
        }
        try {
          glyphs.reserve(glyph_count);
          for (std::uint32_t index = 0U; index < glyph_count; ++index) {
            const auto address = glyph_pointer + index * profile.glyph_stride;
            std::uint16_t x{};
            std::uint16_t y{};
            std::uint16_t width{};
            std::uint16_t height{};
            LegacyUiGlyphBridgeState glyph;
            if (!runtime_.read16(address + 4U, x) ||
                !runtime_.read16(address + 6U, y) ||
                !runtime_.read16(address + 8U, width) ||
                !runtime_.read16(address + 0x0aU, height) || width > 0xffU ||
                height > 0xffU || !runtime_.read8(address + 0x0eU, glyph.u) ||
                !runtime_.read8(address + 0x0fU, glyph.v) ||
                !runtime_.read8(address + 0x14U, glyph.color.red) ||
                !runtime_.read8(address + 0x15U, glyph.color.green) ||
                !runtime_.read8(address + 0x16U, glyph.color.blue)) {
              return false;
            }
            glyph.x = std::bit_cast<std::int16_t>(x);
            glyph.y = std::bit_cast<std::int16_t>(y);
            glyph.width = static_cast<std::uint8_t>(width);
            glyph.height = static_cast<std::uint8_t>(height);
            glyphs.push_back(glyph);
          }
        } catch (...) {
          return false;
        }
        return true;
      };

  const auto valid_text_object = [&profile](std::uint32_t object) noexcept {
    if (object < profile.text_object_pool ||
        profile.text_object_stride < 0x1cU ||
        profile.text_object_capacity == 0U) {
      return false;
    }
    const auto offset = object - profile.text_object_pool;
    return offset % profile.text_object_stride == 0U &&
           offset / profile.text_object_stride < profile.text_object_capacity;
  };

  if (profile.text_slot_count != 7U || profile.text_slot_stride < 0x14U ||
      profile.text_object_capacity > 256U ||
      profile.message_glyph_capacity > 1024U) {
    return std::nullopt;
  }
  std::vector<std::uint32_t> visited_nodes;
  std::vector<std::uint32_t> visited_objects;
  try {
    visited_nodes.reserve(profile.text_object_capacity);
    visited_objects.reserve(profile.text_object_capacity);
    state.messages.reserve(profile.text_object_capacity);
  } catch (...) {
    return std::nullopt;
  }
  // The retail centered/status builders compile their source strings into
  // glyph packets and retain only an additive byte checksum in TEXT+0x15.
  // Keep each observed builder call single-use while associating the live
  // packets below. Without this bridge the Russian presentation sees an
  // empty source and replays English glyph UVs through the Cyrillic atlas
  // (for example "9mm TAKEN" becomes pseudo-Cyrillic garbage).
  std::vector<bool> used_ui_message_sources(ui_messages_.size(), false);
  for (std::uint32_t slot = 0U; slot < profile.text_slot_count; ++slot) {
    const auto slot_address =
        profile.text_slot_table + slot * profile.text_slot_stride;
    std::uint32_t node{};
    if (!runtime_.read32(slot_address + 0x10U, node)) {
      return std::nullopt;
    }
    while (node != 0U) {
      if (visited_nodes.size() >= profile.text_object_capacity ||
          !readable_ram_range(node, 12U) ||
          std::ranges::find(visited_nodes, node) != visited_nodes.end()) {
        return std::nullopt;
      }
      visited_nodes.push_back(node);
      std::uint32_t object{};
      std::uint32_t next_node{};
      if (!runtime_.read32(node, object) ||
          !runtime_.read32(node + 8U, next_node)) {
        return std::nullopt;
      }
      while (object != 0U) {
        if (!valid_text_object(object) ||
            visited_objects.size() >= profile.text_object_capacity ||
            std::ranges::find(visited_objects, object) !=
                visited_objects.end()) {
          return std::nullopt;
        }
        visited_objects.push_back(object);
        LegacyUiMessageBridgeState message;
        message.channel = slot == 6U ? LegacyUiMessageChannel::status
                                     : LegacyUiMessageChannel::centered;
        if (!read_glyphs(object, profile.message_glyph_pool,
                         profile.message_glyph_capacity, message.glyphs)) {
          return std::nullopt;
        }
        std::uint8_t text_checksum{};
        if (!runtime_.read8(object + 0x15U, text_checksum)) {
          return std::nullopt;
        }
        // A TEXT object is recycled frequently and its generation field is
        // only an additive 8-bit source checksum. A new builder call must win
        // over the persistent cache; otherwise an equal-sum historical string
        // is incorrectly attached to the new glyph packet and English bytes
        // are replayed through the Russian atlas.
        for (std::size_t index = 0U; index < ui_messages_.size(); ++index) {
          const auto &candidate = ui_messages_[index];
          if (used_ui_message_sources[index] ||
              candidate.channel != message.channel ||
              legacyTextChecksum(candidate.text) != text_checksum) {
            continue;
          }
          message.text = candidate.text;
          used_ui_message_sources[index] = true;
          const auto cached = std::ranges::find_if(
              attached_text_sources_, [object](const auto &entry) {
                return entry.text_object == object;
              });
          if (cached != attached_text_sources_.end()) {
            cached->text = candidate.text;
            cached->text_checksum = text_checksum;
          } else if (attached_text_sources_.size() <
                     profile.text_object_capacity) {
            attached_text_sources_.push_back(
                LegacyGameplayVmSnapshot::AttachedTextSource{
                    object, candidate.text, text_checksum});
          }
          break;
        }
        if (message.text.empty()) {
          const auto source = std::ranges::find_if(
              attached_text_sources_,
              [object, text_checksum](const auto &entry) {
                return entry.text_object == object &&
                       entry.text_checksum == text_checksum;
              });
          if (source != attached_text_sources_.end()) {
            message.text = source->text;
          }
        }
        state.messages.push_back(std::move(message));
        if (!runtime_.read32(object + 0x18U, object)) {
          return std::nullopt;
        }
      }
      node = next_node;
    }
  }

  // Slot 6 owns one union backdrop packet. Attach it to the first status
  // message; presentation draws all backdrops before glyphs, preserving the
  // retail primitive ordering without manufacturing a fixed rectangle.
  const auto first_status =
      std::ranges::find_if(state.messages, [](const auto &message) {
        return message.channel == LegacyUiMessageChannel::status;
      });
  if (first_status != state.messages.end()) {
    std::uint32_t tag{};
    std::uint32_t color_code{};
    if (!runtime_.read32(profile.status_backdrop_tag, tag) ||
        !runtime_.read32(profile.status_backdrop_color_code, color_code)) {
      return std::nullopt;
    }
    if (tag != 0U) {
      LegacyUiBackdropBridgeState backdrop;
      backdrop.color =
          LegacyRgbBridgeState{static_cast<std::uint8_t>(color_code),
                               static_cast<std::uint8_t>(color_code >> 8U),
                               static_cast<std::uint8_t>(color_code >> 16U)};
      backdrop.semi_transparent =
          (static_cast<std::uint8_t>(color_code >> 24U) & 0x02U) != 0U;
      for (std::uint32_t corner = 0U; corner < 4U; ++corner) {
        std::uint32_t xy{};
        if (!runtime_.read32(profile.status_backdrop_vertices + corner * 4U,
                             xy)) {
          return std::nullopt;
        }
        backdrop.corners[corner] = LegacyProjectedPointBridgeState{
            std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(xy)),
            std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(xy >> 16U))};
      }
      first_status->backdrop = backdrop;
    }
  }

  std::uint16_t timer_handle{};
  std::uint32_t timer_remaining{};
  if (!runtime_.read16(profile.mission_timer_handle, timer_handle) ||
      !runtime_.read32(profile.mission_timer_remaining, timer_remaining)) {
    return std::nullopt;
  }
  if (timer_handle != 0xffffU) {
    const auto index = static_cast<std::uint32_t>(timer_handle & 0xffU);
    const auto timer_object =
        profile.text_object_pool + index * profile.text_object_stride;
    std::uint8_t generation{};
    if (index >= profile.text_object_capacity ||
        !runtime_.read8(timer_object + 0x15U, generation) ||
        generation != static_cast<std::uint8_t>(timer_handle >> 8U)) {
      return std::nullopt;
    }
    LegacyUiTimerBridgeState timer;
    timer.remaining_ticks = std::bit_cast<std::int32_t>(timer_remaining);
    timer.handle = timer_handle;
    if (!read_glyphs(timer_object, profile.timer_glyph_pool,
                     profile.timer_glyph_capacity, timer.glyphs)) {
      return std::nullopt;
    }
    state.timer = std::move(timer);
  }
  return state;
}

LegacyGameplayVmSnapshot LegacyGameplayVm::captureSnapshot() const {
  LegacyGameplayVmSnapshot snapshot;
  snapshot.cpu = runtime_.state();
  snapshot.machine = machine_.captureState();
  snapshot.ram.assign(runtime_.ram().begin(), runtime_.ram().end());
  std::ranges::copy(runtime_.scratchpad(), snapshot.scratchpad.begin());
  std::ranges::copy(runtime_.mmio(), snapshot.mmio.begin());
  snapshot.video_timing_baseline = video_timing_baseline_;
  snapshot.audio_frame_tick = audio_frame_tick_;
  snapshot.interrupt_callbacks = interrupt_callbacks_;
  snapshot.attached_text_sources = attached_text_sources_;
  snapshot.ui_messages = ui_messages_;
  snapshot.pending_actor_drops = pending_actor_drops_;
  snapshot.agent_cbdc_friendly_fire_frame = agent_cbdc_friendly_fire_frame_;
  snapshot.agent_cbdc_friendly_fire_pending_penalties =
      agent_cbdc_friendly_fire_pending_penalties_;
  snapshot.video_timing_baseline_initialized =
      video_timing_baseline_initialized_;
  snapshot.audio_frame_tick_initialized = audio_frame_tick_initialized_;
  if (virtual_cd_) {
    snapshot.virtual_cd = virtual_cd_->captureSnapshot();
  }
  return snapshot;
}

bool LegacyGameplayVm::restoreSnapshot(
    const LegacyGameplayVmSnapshot &snapshot) noexcept {
  const auto valid_attached_text_sources = [&snapshot] {
    if (snapshot.attached_text_sources.size() > 256U) {
      return false;
    }
    for (std::size_t index = 0U; index < snapshot.attached_text_sources.size();
         ++index) {
      const auto &entry = snapshot.attached_text_sources[index];
      if ((entry.text_object & 3U) != 0U || entry.text_object < 0x80000000U ||
          entry.text_object > 0x801fffe4U || entry.text.empty() ||
          entry.text.size() > 4096U) {
        return false;
      }
      for (std::size_t previous = 0U; previous < index; ++previous) {
        if (snapshot.attached_text_sources[previous].text_object ==
            entry.text_object) {
          return false;
        }
      }
    }
    return true;
  };
  const auto valid_ui_messages = [&snapshot] {
    if (snapshot.ui_messages.size() > 256U) {
      return false;
    }
    return std::ranges::all_of(snapshot.ui_messages, [](const auto &message) {
      const auto valid_channel =
          message.channel == LegacyUiMessageChannel::centered ||
          message.channel == LegacyUiMessageChannel::status;
      return valid_channel && !message.text.empty() &&
             message.text.size() <= 4096U;
    });
  };
  const auto valid_pending_actor_drops = [&snapshot] {
    if (snapshot.pending_actor_drops.size() > 2048U) {
      return false;
    }
    for (std::size_t index = 0U; index < snapshot.pending_actor_drops.size();
         ++index) {
      const auto &candidate = snapshot.pending_actor_drops[index];
      if (candidate.record_slot >= 2048U || candidate.instance < 0x80000000U ||
          candidate.instance > 0x801ffffcU || (candidate.instance & 3U) != 0U ||
          ((candidate.attributes & 0x00ffU) == 0U &&
           (candidate.attributes & 0x7000U) == 0U)) {
        return false;
      }
      for (std::size_t previous = 0U; previous < index; ++previous) {
        if (snapshot.pending_actor_drops[previous].record_slot ==
            candidate.record_slot) {
          return false;
        }
      }
    }
    return true;
  };
  if (snapshot.ram.size() != psx::R3000Runtime::ram_size ||
      snapshot.virtual_cd.has_value() != static_cast<bool>(virtual_cd_) ||
      !valid_attached_text_sources() || !valid_ui_messages() ||
      !valid_pending_actor_drops() || !validCpuSnapshot(snapshot.cpu) ||
      !std::ranges::all_of(snapshot.interrupt_callbacks,
                           validInterruptCallback) ||
      (snapshot.audio_frame_tick_initialized
           ? snapshot.audio_frame_tick > logicalMachineTick(snapshot.machine)
           : snapshot.audio_frame_tick != 0U) ||
      !machine_.validateState(snapshot.machine)) {
    return false;
  }
  if ((virtual_cd_ && !virtual_cd_->restoreSnapshot(*snapshot.virtual_cd)) ||
      !runtime_.restoreRam(snapshot.ram) ||
      !runtime_.restoreScratchpad(snapshot.scratchpad) ||
      !runtime_.restoreMmio(snapshot.mmio)) {
    return false;
  }
  runtime_.restoreCpuState(snapshot.cpu);
  // Host PCM has already been submitted to the audio device and is not guest
  // state. PsxMachine::restoreState() restores the SPU voices/CD FIFO but
  // deliberately clears this presentation queue, matching a save-state load
  // in hardware emulators and preventing pre-checkpoint audio from replaying.
  if (!machine_.restoreState(snapshot.machine)) {
    return false;
  }
  video_timing_baseline_ = snapshot.video_timing_baseline;
  audio_frame_tick_ = snapshot.audio_frame_tick;
  interrupt_callbacks_ = snapshot.interrupt_callbacks;
  video_timing_baseline_initialized_ =
      snapshot.video_timing_baseline_initialized;
  audio_frame_tick_initialized_ = snapshot.audio_frame_tick_initialized;
  attached_text_sources_ = snapshot.attached_text_sources;
  ui_messages_ = snapshot.ui_messages;
  pending_actor_drops_ = snapshot.pending_actor_drops;
  agent_cbdc_friendly_fire_frame_ = snapshot.agent_cbdc_friendly_fire_frame;
  agent_cbdc_friendly_fire_pending_penalties_ =
      snapshot.agent_cbdc_friendly_fire_pending_penalties;
  weapon_events_.clear();
  clearAgentHeadshotThreat();
  return true;
}

bool LegacyGameplayVm::advanceAudioClockCallbacks(
    const LegacyRetailAudioProfile &profile,
    std::uint32_t callback_count) noexcept {
  if (callback_count == 0U || profile.callback_hz == 0U ||
      machine_.cpuTicksPerSecond() % profile.callback_hz != 0U ||
      profile.timer_irq >= interrupt_callbacks_.size()) {
    return false;
  }

  try {
    const auto ticks_per_callback =
        machine_.cpuTicksPerSecond() / profile.callback_hz;
    if (!audio_frame_tick_initialized_) {
      audio_frame_tick_ = machine_.currentTick();
      audio_frame_tick_initialized_ = true;
    }

    const auto dispatch_callback = [&]() {
      if (!dispatchCdRomReadyCallback()) {
        return false;
      }

      const auto callback = interrupt_callbacks_[profile.timer_irq];
      if (callback != 0U) {
        if (profile.expected_tick_callback != 0U &&
            callback != profile.expected_tick_callback) {
          return false;
        }
        const auto result = invokeFrameCall(callback, {}, 5'000'000U);
        if (!result.completed()) {
          return false;
        }
      }
      return true;
    };

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto frame_ticks =
        static_cast<std::uint64_t>(callback_count) * ticks_per_callback;
    const auto frame_deadline = audio_frame_tick_ > maximum - frame_ticks
                                    ? maximum
                                    : audio_frame_tick_ + frame_ticks;

    while (audio_frame_tick_ < frame_deadline) {
      const auto now = machine_.currentTick();
      const auto next = audio_frame_tick_ > maximum - ticks_per_callback
                            ? maximum
                            : audio_frame_tick_ + ticks_per_callback;
      if (now >= next) {
        // A PSX timer IRQ is level-latched, not an unbounded callback queue.
        // Streaming work can advance several 120 Hz periods before the HLE
        // boundary is reached; coalesce those missed edges into one pending
        // callback and re-anchor at the latest elapsed cadence boundary.
        const auto elapsed_callbacks =
            (now - audio_frame_tick_) / ticks_per_callback;
        audio_frame_tick_ =
            elapsed_callbacks == 0U
                ? next
                : audio_frame_tick_ + elapsed_callbacks * ticks_per_callback;
        if (!dispatch_callback()) {
          return false;
        }
        continue;
      }

      machine_.advanceTicks(next - now);
      audio_frame_tick_ = next;
      if (!dispatch_callback()) {
        return false;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool LegacyGameplayVm::advanceAudioFrameClock(
    const LegacyRetailAudioProfile &profile) noexcept {
  if (profile.callback_hz == 0U ||
      profile.callback_hz % updates_per_second != 0U) {
    return false;
  }
  return advanceAudioClockCallbacks(profile,
                                    profile.callback_hz / updates_per_second);
}

bool LegacyGameplayVm::advanceAudioSliceClock(
    const LegacyRetailAudioProfile &profile) noexcept {
  return advanceAudioClockCallbacks(profile, 1U);
}

bool LegacyGameplayVm::stopRetailXa(const LegacyRetailAudioProfile &profile,
                                    std::uint64_t execution_budget) noexcept {
  if (profile.stop_xa_entry == 0U || execution_budget == 0U) {
    return false;
  }
  try {
    // FUN_800c6058 is the resident retail stop path: clear the XA state,
    // issue CdlPause, mute CD input and discard the queued stream. CdRom/SPU
    // state remains the acknowledgement authority for presentation.
    return invokeFrameCall(profile.stop_xa_entry, {}, execution_budget)
        .completed();
  } catch (...) {
    return false;
  }
}

bool LegacyGameplayVm::setRetailAudioVolumes(
    const LegacyRetailAudioVolumes &volumes,
    const LegacyRetailAudioProfile &profile,
    std::uint64_t execution_budget) noexcept {
  if (!volumes.valid() || profile.set_group_volume_entry == 0U ||
      profile.group_volume_address == 0U || execution_budget == 0U) {
    return false;
  }

  std::optional<LegacyGameplayVmSnapshot> before;
  std::vector<LegacyWeaponEventBridgeState> saved_weapon_events;
  std::vector<LegacyGameplayVmSnapshot::AttachedTextSource>
      saved_attached_text_sources;
  std::vector<LegacyUiMessageBridgeState> saved_ui_messages;
  const auto rollback = [this, &before, &saved_weapon_events,
                         &saved_attached_text_sources,
                         &saved_ui_messages]() noexcept {
    const auto restored = before && restoreSnapshot(*before);
    weapon_events_ = std::move(saved_weapon_events);
    attached_text_sources_ = std::move(saved_attached_text_sources);
    ui_messages_ = std::move(saved_ui_messages);
    return restored;
  };
  try {
    saved_weapon_events = weapon_events_;
    saved_attached_text_sources = attached_text_sources_;
    saved_ui_messages = ui_messages_;
    // A completed retail callback may leave already-accounted CPU ticks in
    // the machine's device slice. SPU MMIO synchronizes that slice, so anchor
    // the atomic overlay after the same synchronization. This changes no
    // logical guest time and prevents the volume write from looking like a
    // timeline mutation.
    machine_.synchronizeDevices();
    before.emplace(captureSnapshot());

    const auto groups = volumes.groups();
    for (std::size_t group = 0U; group < groups.size(); ++group) {
      const std::array<std::uint32_t, 2U> arguments{
          static_cast<std::uint32_t>(group), groups[group]};
      if (!runtime_.beginCall(profile.set_group_volume_entry, arguments)) {
        static_cast<void>(rollback());
        return false;
      }
      const auto result =
          runExecutionPump(std::nullopt, execution_budget, false);
      if (!result.completed()) {
        static_cast<void>(rollback());
        return false;
      }
    }

    const auto after_machine = machine_.captureState();
    if (!sameMachineTimeline(before->machine, after_machine)) {
      static_cast<void>(rollback());
      return false;
    }

    // The setters are an atomic host overlay: retain their retail RAM/SPU
    // volume effects, but restore the interrupted CPU and compatibility
    // shadows exactly. No scheduler tick or asynchronous callback is consumed.
    runtime_.restoreCpuState(before->cpu);
    if (!runtime_.restoreScratchpad(before->scratchpad) ||
        !runtime_.restoreMmio(before->mmio)) {
      static_cast<void>(rollback());
      return false;
    }
    weapon_events_ = std::move(saved_weapon_events);
    attached_text_sources_ = std::move(saved_attached_text_sources);
    ui_messages_ = std::move(saved_ui_messages);
    return true;
  } catch (...) {
    static_cast<void>(rollback());
    return false;
  }
}

std::optional<LegacyRetailAudioVolumes>
LegacyGameplayVm::readRetailAudioVolumes(
    const LegacyRetailAudioProfile &profile) const noexcept {
  if (profile.group_volume_address == 0U) {
    return std::nullopt;
  }
  LegacyRetailAudioVolumes result;
  auto groups = result.groups();
  for (std::size_t group = 0U; group < groups.size(); ++group) {
    if (!runtime_.read8(profile.group_volume_address +
                            static_cast<std::uint32_t>(group),
                        groups[group])) {
      return std::nullopt;
    }
  }
  result.sound_effects = groups[0];
  result.music = groups[1];
  result.voice_over = groups[2];
  return result.valid() ? std::optional{result} : std::nullopt;
}

std::size_t
LegacyGameplayVm::takePcm(std::span<psx::SpuPcmFrame> destination) noexcept {
  return machine_.spu().takePcm(destination);
}

void LegacyGameplayVm::clearPcm() noexcept {
  machine_.spu().clearPcm();
  audio_frame_tick_ = machine_.currentTick();
  audio_frame_tick_initialized_ = true;
}

LegacyAudioDiagnostics LegacyGameplayVm::audioDiagnostics() const noexcept {
  const auto &spu = machine_.spu();
  const auto &spu_state = spu.state();
  const auto cd_state = machine_.cdrom().captureState();
  auto active_voices = std::size_t{};
  for (const auto &voice : spu_state.voices) {
    active_voices += voice.active != 0U ? 1U : 0U;
  }
  return LegacyAudioDiagnostics{
      .machine_tick = machine_.currentTick(),
      .audio_frame_tick = audio_frame_tick_,
      .spu_sample_clock = spu_state.sample_clock,
      .spu_mixed_frames = spu_state.mixed_frames,
      .spu_dropped_pcm_frames = spu.droppedPcmFrames(),
      .cd_lba = cd_state.current_lba,
      .spu_pcm_frames = spu.queuedPcmFrames(),
      .spu_cd_frames = spu.queuedCdFrames(),
      .active_spu_voices = active_voices,
      .spu_control = spu.control(),
      .spu_status = spu.status(),
      .cd_mode = cd_state.mode,
      .cd_reading = cd_state.reading,
      .cd_muted = cd_state.muted,
      .cd_adpcm_muted = cd_state.adpcm_muted,
      .xa_stream_set = cd_state.xa_current_set,
      .xa_file = cd_state.xa_current_file,
      .xa_channel = cd_state.xa_current_channel,
      .audio_frame_tick_initialized = audio_frame_tick_initialized_,
  };
}

LegacyGameplayVmResult
LegacyGameplayVm::invoke(std::uint32_t address,
                         std::span<const std::uint32_t> arguments,
                         std::uint64_t execution_budget) {
  if (!runtime_.beginCall(address, arguments)) {
    return LegacyGameplayVmResult{
        {psx::R3000StopReason::memory_fault, 0U, address, 0U},
        runtime_.state().gpr[2],
        0U,
        std::nullopt,
    };
  }
  return runExecutionPump(std::nullopt, execution_budget);
}

LegacyGameplayVmResult
LegacyGameplayVm::invokeFrameCall(std::uint32_t address,
                                  std::span<const std::uint32_t> arguments,
                                  std::uint64_t execution_budget) {
  if (!runtime_.beginCall(address, arguments)) {
    return LegacyGameplayVmResult{
        {psx::R3000StopReason::memory_fault, 0U, address, 0U},
        runtime_.state().gpr[2],
        0U,
        std::nullopt,
    };
  }
  // Native presentation fixes the retail simulation at 20 Hz. Running a
  // decoded frame's functions atomically prevents their host instruction
  // count from consuming extra emulated hardware time (the old reason for
  // the 6x CPU clock). The realtime host advances SPU/CD/timers separately
  // in exact 120 Hz slices; the offline frame helper groups six such slices.
  return runExecutionPump(std::nullopt, execution_budget, false);
}

LegacyGameplayVmResult
LegacyGameplayVm::resumeCurrentPc(std::uint64_t execution_budget) {
  return runExecutionPump(std::nullopt, execution_budget);
}

LegacyGameplayVmResult LegacyGameplayVm::runCurrentPcUntilHostBoundary(
    std::uint32_t boundary_address, std::uint64_t execution_budget) {
  return runExecutionPump(boundary_address, execution_budget);
}

LegacyGameplayVmResult
LegacyGameplayVm::runExecutionPump(std::optional<std::uint32_t> host_boundary,
                                   std::uint64_t execution_budget,
                                   bool advance_guest_clock) {
  std::uint64_t instructions{};
  std::uint64_t host_calls{};
  const auto boundary_result = [&]() {
    return LegacyGameplayVmResult{
        {
            psx::R3000StopReason::running,
            instructions,
            runtime_.state().pc,
            0U,
        },
        runtime_.state().gpr[2],
        host_calls,
        host_boundary,
    };
  };
  const auto service_clock_neutral_dma = [&]() {
    return advance_guest_clock || machine_.completePendingDmaTransfers();
  };

  for (std::uint64_t operation = 0; operation < execution_budget; ++operation) {
    if (runtime_.atReturnSentinel()) {
      runtime_.settleLoadDelay();
      return LegacyGameplayVmResult{
          {psx::R3000StopReason::returned, instructions, runtime_.state().pc,
           0U},
          runtime_.state().gpr[2],
          host_calls,
          std::nullopt,
      };
    }

    if (advance_guest_clock && runtime_.interruptPending()) {
      auto execution = machine_.step();
      if (execution.reason != psx::R3000StopReason::running) {
        execution.instructions = instructions;
        return LegacyGameplayVmResult{
            execution,
            runtime_.state().gpr[2],
            host_calls,
            std::nullopt,
        };
      }
      instructions += execution.instructions;
      continue;
    }

    if (host_boundary && runtime_.state().pc == *host_boundary &&
        findHostCall(*host_boundary) != nullptr) {
      return boundary_result();
    }

    if (auto *hook = findHostCall(runtime_.state().pc); hook != nullptr) {
      LegacyHostCallContext context{runtime_};
      (*hook)(context);
      ++host_calls;
      if (!context.accepted_) {
        return LegacyGameplayVmResult{
            {
                psx::R3000StopReason::unsupported_instruction,
                instructions,
                runtime_.state().pc,
                0U,
            },
            runtime_.state().gpr[2],
            host_calls,
            std::nullopt,
        };
      }
      if (context.continue_guest_instruction_) {
        auto execution =
            advance_guest_clock ? machine_.step() : runtime_.step();
        if (execution.reason != psx::R3000StopReason::running) {
          execution.instructions = instructions;
          return LegacyGameplayVmResult{
              execution,
              runtime_.state().gpr[2],
              host_calls,
              std::nullopt,
          };
        }
        instructions += execution.instructions;
        if (!service_clock_neutral_dma()) {
          return LegacyGameplayVmResult{
              {psx::R3000StopReason::memory_fault, instructions,
               runtime_.state().pc, 0U},
              runtime_.state().gpr[2],
              host_calls,
              std::nullopt,
          };
        }
        continue;
      }
      runtime_.completeHostCall();
      if (advance_guest_clock) {
        machine_.advanceTicks(1U);
      }
      if (!service_clock_neutral_dma()) {
        return LegacyGameplayVmResult{
            {psx::R3000StopReason::memory_fault, instructions,
             runtime_.state().pc, 0U},
            runtime_.state().gpr[2],
            host_calls,
            std::nullopt,
        };
      }
      continue;
    }

    auto execution = advance_guest_clock ? machine_.step() : runtime_.step();
    if (execution.reason != psx::R3000StopReason::running) {
      execution.instructions = instructions;
      return LegacyGameplayVmResult{
          execution,
          runtime_.state().gpr[2],
          host_calls,
          std::nullopt,
      };
    }
    instructions += execution.instructions;
    if (!service_clock_neutral_dma()) {
      return LegacyGameplayVmResult{
          {psx::R3000StopReason::memory_fault, instructions,
           runtime_.state().pc, 0U},
          runtime_.state().gpr[2],
          host_calls,
          std::nullopt,
      };
    }
  }

  if (runtime_.atReturnSentinel()) {
    runtime_.settleLoadDelay();
    return LegacyGameplayVmResult{
        {psx::R3000StopReason::returned, instructions, runtime_.state().pc, 0U},
        runtime_.state().gpr[2],
        host_calls,
        std::nullopt,
    };
  }
  if (host_boundary && !runtime_.interruptPending() &&
      runtime_.state().pc == *host_boundary &&
      findHostCall(*host_boundary) != nullptr) {
    return boundary_result();
  }

  return LegacyGameplayVmResult{
      {psx::R3000StopReason::instruction_budget, instructions,
       runtime_.state().pc, 0U},
      runtime_.state().gpr[2],
      host_calls,
      std::nullopt,
  };
}

LegacyGameplayVmResult
LegacyGameplayVm::tickRetailFrame(const LegacyRetailFrameProfile &profile,
                                  std::uint64_t execution_budget) {
  // A retail frame is executed between two host-owned 20 Hz boundaries.
  // Charging its interpreter instruction count to PsxMachine advances SPU/CD
  // a second time and turns heavy streaming frames into seconds of queued
  // future PCM. Keep the frame atomic; the independent 120 Hz scheduler is
  // the sole owner of hardware time during realtime gameplay.
  return invokeFrameCall(profile.frame_entry, {}, execution_budget);
}

LegacyRetailPlatformTailResult LegacyGameplayVm::tickRetailPlatformTail(
    bool advance_delayed_callbacks,
    const LegacyRetailPlatformTailProfile &profile,
    std::uint64_t execution_budget) {
  LegacyRetailPlatformTailResult result;
  const std::array callback_arguments{
      advance_delayed_callbacks ? 1U : 0U,
  };
  // This tail is part of the same fixed retail frame as the state-machine
  // body. In particular, room streaming can execute a large delayed-callback
  // batch here. Running it through invoke() double-clocked SPU/CD by the
  // batch's instruction count and accumulated an ever-growing audio delay.
  result.delayed_callbacks = invokeFrameCall(
      profile.delayed_callbacks_entry, callback_arguments, execution_budget);
  if (!result.delayed_callbacks.completed()) {
    return result;
  }

  std::uint8_t initialized{};
  std::uint8_t floor{};
  std::uint16_t current{};
  std::uint16_t step_bits{};
  std::uint32_t callback{};
  if (!runtime_.read8(profile.fade_initialized, initialized) ||
      !runtime_.read8(profile.fade_floor_rgb, floor) ||
      !runtime_.read16(profile.fade_current, current) ||
      !runtime_.read16(profile.fade_step, step_bits) ||
      !runtime_.read32(profile.fade_callback, callback)) {
    result.bridge_fault = true;
    return result;
  }

  // FUN_800c8ee8 performs this software state update before constructing
  // the fullscreen primitive. Reproduce only that part at the renderer
  // boundary so mission callbacks and timing remain guest-authored.
  if (initialized != 0U) {
    if (!runtime_.write8(profile.fade_initialized, 0U) ||
        !runtime_.write16(profile.fade_current, floor)) {
      result.bridge_fault = true;
    }
    return result;
  }

  const auto step = std::bit_cast<std::int16_t>(step_bits);
  const auto at_endpoint =
      (current == 0xffU && step > 0) || (current == floor && step < 0);
  if (at_endpoint) {
    if (callback != 0U) {
      result.fade_callback = invokeFrameCall(
          profile.fade_callback_dispatch_entry, {}, execution_budget);
    }
    return result;
  }

  const auto next = std::clamp<std::int32_t>(
      static_cast<std::int32_t>(current) + step, floor, 0xff);
  if (!runtime_.write16(profile.fade_current,
                        static_cast<std::uint16_t>(next))) {
    result.bridge_fault = true;
  }
  return result;
}

LegacyRetailState2TransitionResult
LegacyGameplayVm::dispatchRetailState2Transition(
    const LegacyRetailState2TransitionProfile &profile,
    std::uint64_t execution_budget) {
  LegacyRetailState2TransitionResult result;
  if (profile.maximum_dispatches == 0U) {
    result.dispatch_limit_reached = true;
    return result;
  }

  const auto call = [&](std::uint32_t address,
                        std::span<const std::uint32_t> arguments = {}) {
    result.guest_calls.push_back(
        invokeFrameCall(address, arguments, execution_budget));
    return result.guest_calls.back().completed();
  };

  while (result.dispatches < profile.maximum_dispatches) {
    if (!runtime_.read32(profile.current_state, result.final_state)) {
      result.bridge_fault = true;
      return result;
    }
    if (result.final_state != 2U) {
      return result;
    }

    std::uint8_t transition{};
    if (!runtime_.read8(profile.transition, transition)) {
      result.bridge_fault = true;
      return result;
    }
    if (transition == 2U) {
      transition = 4U;
      if (!runtime_.write32(profile.state_depth, 1U) ||
          !runtime_.write8(profile.transition, transition)) {
        result.bridge_fault = true;
        return result;
      }
    } else if (!call(profile.pop_state_entry)) {
      return result;
    }

    const std::array transition_argument{
        static_cast<std::uint32_t>(transition)};
    constexpr std::array push_title_argument{4U};
    const auto dispatched =
        transition == 5U ? call(profile.push_state_entry, push_title_argument)
                         : call(profile.common_init_entry, transition_argument);
    ++result.dispatches;
    if (!dispatched) {
      return result;
    }
  }

  if (!runtime_.read32(profile.current_state, result.final_state)) {
    result.bridge_fault = true;
  } else {
    result.dispatch_limit_reached = result.final_state == 2U;
  }
  return result;
}

LegacyRetailOuterFrameResult LegacyGameplayVm::tickRetailOuterFrame(
    const LegacyRetailOuterFrameProfile &profile,
    const LegacyRetailPlatformTailProfile &tail_profile,
    std::uint64_t execution_budget) {
  LegacyRetailOuterFrameResult result;
  const auto increment = [this](std::uint32_t address) {
    std::uint32_t value{};
    return runtime_.read32(address, value) &&
           runtime_.write32(address, value + 1U);
  };
  const auto call = [&](std::uint32_t address,
                        std::span<const std::uint32_t> arguments) {
    result.guest_calls.push_back(
        invokeFrameCall(address, arguments, execution_budget));
    return result.guest_calls.back().completed();
  };
  const auto call_without_arguments = [&](std::uint32_t address) {
    return call(address, {});
  };

  if (!increment(profile.system_clock)) {
    result.bridge_fault = true;
    result.bridge_fault_stage = "system-clock";
    return result;
  }
  if (!call_without_arguments(profile.input_entry)) {
    return result;
  }
  // Retail samples the application state after pad processing. Input may
  // enter pause/loading, so branching on the pre-input state would execute
  // one extra gameplay/player frame during that transition.
  if (!runtime_.read32(profile.current_state, result.state_before)) {
    result.bridge_fault = true;
    result.bridge_fault_stage = "state-before";
    return result;
  }

  if (result.state_before == 0U || result.state_before == 5U) {
    if (!call_without_arguments(profile.gameplay_entry)) {
      return result;
    }
    if (!increment(profile.gameplay_clock)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "gameplay-clock";
      return result;
    }
    std::uint32_t player{};
    if (!runtime_.read32(profile.player_pointer, player)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "player-pointer";
      return result;
    }
    const std::array player_arguments{player};
    if (!call(profile.player_frame_entry, player_arguments)) {
      return result;
    }
  } else if (result.state_before == 7U) {
    if (!call_without_arguments(profile.state7_frame_entry)) {
      return result;
    }
    std::uint32_t updated_state{};
    if (!runtime_.read32(profile.current_state, updated_state)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "state7-updated-state";
      return result;
    }
    constexpr std::array zero_argument{0U};
    if (updated_state == 7U) {
      if (!call(profile.loading_stream_frame_entry, zero_argument)) {
        return result;
      }
    } else {
      if (!runtime_.write32(profile.current_state, 0U)) {
        result.bridge_fault = true;
        result.bridge_fault_stage = "state7-temporary-state";
        return result;
      }
      if (!call_without_arguments(profile.stream_resume_entry)) {
        return result;
      }
      if (!runtime_.write32(profile.current_state, updated_state)) {
        result.bridge_fault = true;
        result.bridge_fault_stage = "state7-restored-state";
        return result;
      }
    }
  } else if (result.state_before == 9U) {
    std::uint32_t player{};
    std::uint16_t display_flags{};
    if (!runtime_.read32(profile.player_pointer, player) ||
        !runtime_.read16(profile.display_flags, display_flags)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "state9-player-display";
      return result;
    }
    const std::array player_arguments{player};
    constexpr std::array zero_argument{0U};
    if (!call(profile.loading_player_frame_entry, player_arguments)) {
      return result;
    }
    if (!runtime_.write16(profile.display_flags,
                          static_cast<std::uint16_t>(display_flags & ~2U))) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "state9-display-flags";
      return result;
    }
    if (!call(profile.loading_stream_frame_entry, zero_argument) ||
        !call_without_arguments(profile.loading_overlay_frame_entry)) {
      return result;
    }
  } else if (result.state_before == 6U) {
    // State 6 has no body in System_RunStateMachine. Its common tail below
    // observes the state change and immediately pops the retail state stack.
  } else {
    result.unsupported_state = true;
    return result;
  }

  if (!runtime_.read32(profile.current_state, result.state_after)) {
    result.bridge_fault = true;
    result.bridge_fault_stage = "state-after";
    return result;
  }

  // Exact state-change branch from System_RunStateMachine. A transition frame
  // does not submit a renderer list. State 6 is the transient used by the SVD
  // optic path: retail pops it here and resumes the underlying gameplay state
  // on the following outer iteration.
  std::uint32_t presented_state{};
  if (!runtime_.read32(profile.presented_state, presented_state)) {
    result.bridge_fault = true;
    result.bridge_fault_stage = "presented-state";
    return result;
  }
  if (presented_state != result.state_after) {
    if (result.state_after == 6U) {
      if (!call_without_arguments(profile.pop_state_entry) ||
          !runtime_.read32(profile.current_state, result.state_after)) {
        return result;
      }
    } else if (!runtime_.write32(profile.presented_state, result.state_after)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "presented-state-update";
      return result;
    }

    std::uint32_t player{};
    if (!runtime_.read32(profile.player_pointer, player)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "transition-player";
      return result;
    }
    if (player != 0U) {
      std::uint32_t display{};
      if (!runtime_.read32(player + 8U, display)) {
        result.bridge_fault = true;
        result.bridge_fault_stage = "transition-display";
        return result;
      }
      if (display != 0U) {
        std::uint8_t flags{};
        if (!runtime_.read8(display + 10U, flags) ||
            !runtime_.write32(display + 0x28U, 0U) ||
            !runtime_.write8(display + 10U,
                             static_cast<std::uint8_t>(flags & ~2U))) {
          result.bridge_fault = true;
          result.bridge_fault_stage = "transition-display-reset";
          return result;
        }
      }
    }
    result.tail_skipped = true;
    return result;
  }
  const auto gameplay_state =
      result.state_after == 0U || result.state_after == 5U;
  if (gameplay_state) {
    std::uint32_t gameplay_frame{};
    if (!finalizeDeadActorDropsBeforeRenderer(
            syphonFilterUsaV11GameplayBridgeProfile(), execution_budget)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "dead-actor-drops";
      return result;
    }
    if (!runtime_.read32(profile.gameplay_frame, gameplay_frame)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "gameplay-frame";
      return result;
    }
    if (!advanceRetailRendererVblank(runtime_, profile)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "renderer-vblank";
      return result;
    }
    // Retail calls FUN_800c973c(1, DAT_80116a88) after the scheduler and
    // player frame. Besides submitting primitives, its terrain traversal
    // fills the room visibility bytes registered by FUN_80080930. The next
    // event 0x11 consumes those bytes and drains newly visible objects into
    // event 0x0a. Running only the software fade/callback tail leaves that
    // array empty and strands dynamically spawned actors outside the event
    // loop.
    const std::array renderer_arguments{1U, gameplay_frame};
    result.renderer_tail = invokeFrameCall(
        profile.renderer_frame_entry, renderer_arguments, execution_budget);
    if (result.renderer_tail->completed() &&
        !runtime_.read32(profile.current_state, result.state_after)) {
      result.bridge_fault = true;
      result.bridge_fault_stage = "renderer-state-after";
    }
  } else {
    result.platform_tail =
        tickRetailPlatformTail(false, tail_profile, execution_budget);
  }
  return result;
}

LegacyRetailOuterFrameResult LegacyGameplayVm::tickNativeDrivenGameplayFrame(
    const LegacyRetailOuterFrameProfile &profile,
    const LegacyRetailPlatformTailProfile &tail_profile,
    std::uint64_t execution_budget) {
  LegacyRetailOuterFrameResult result;
  const auto increment = [this](std::uint32_t address) {
    std::uint32_t value{};
    return runtime_.read32(address, value) &&
           runtime_.write32(address, value + 1U);
  };
  const auto call = [&](std::uint32_t address,
                        std::span<const std::uint32_t> arguments = {}) {
    result.guest_calls.push_back(invoke(address, arguments, execution_budget));
    return result.guest_calls.back().completed();
  };

  if (!runtime_.read32(profile.current_state, result.state_before)) {
    result.bridge_fault = true;
    return result;
  }
  if (result.state_before != 0U && result.state_before != 5U) {
    result.unsupported_state = true;
    return result;
  }
  if (!increment(profile.system_clock) || !call(profile.gameplay_entry) ||
      !increment(profile.gameplay_clock)) {
    result.bridge_fault =
        result.guest_calls.empty() || result.guest_calls.back().completed();
    return result;
  }
  std::uint32_t player{};
  if (!runtime_.read32(profile.player_pointer, player) || player == 0U) {
    result.bridge_fault = true;
    return result;
  }
  const std::array player_arguments{player};
  if (!call(profile.player_frame_entry, player_arguments) ||
      !runtime_.read32(profile.current_state, result.state_after)) {
    result.bridge_fault = result.guest_calls.back().completed();
    return result;
  }

  // Host input replaces only the retail pad poll. The guest still owns its
  // state stack: scripted 5 -> 7 -> 9 transitions must survive this frame or
  // their callbacks are pushed again until the retail stack overflows.
  const auto gameplay_state =
      result.state_after == 0U || result.state_after == 5U;
  if (gameplay_state) {
    std::uint32_t gameplay_frame{};
    if (!finalizeDeadActorDropsBeforeRenderer(
            syphonFilterUsaV11GameplayBridgeProfile(), execution_budget) ||
        !runtime_.read32(profile.gameplay_frame, gameplay_frame) ||
        !advanceRetailRendererVblank(runtime_, profile)) {
      result.bridge_fault = true;
      return result;
    }
    const std::array renderer_arguments{1U, gameplay_frame};
    result.renderer_tail = invoke(profile.renderer_frame_entry,
                                  renderer_arguments, execution_budget);
    if (result.renderer_tail->completed() &&
        !runtime_.read32(profile.current_state, result.state_after)) {
      result.bridge_fault = true;
    }
  } else {
    result.platform_tail =
        tickRetailPlatformTail(false, tail_profile, execution_budget);
  }
  return result;
}

LegacyFirstMissionOpeningResult
LegacyGameplayVm::startFirstMissionOpeningWithoutMovie(
    const LegacyFirstMissionOpeningProfile &profile,
    std::uint64_t execution_budget) {
  LegacyFirstMissionOpeningResult result;
  const std::array remove_arguments{
      profile.skipped_movie_callback,
      0U,
      0U,
  };
  result.remove_movie_callback = invoke(profile.delayed_callback_control_entry,
                                        remove_arguments, execution_budget);
  if (!result.remove_movie_callback.completed()) {
    return result;
  }

  result.fade_reset = invoke(profile.fade_reset_entry, {}, execution_budget);
  if (!result.fade_reset.completed()) {
    return result;
  }
  constexpr std::uint32_t fade_out_direction = 0xffffffffU;
  constexpr std::uint32_t fade_rate = 7U;
  const std::array fade_arguments{
      fade_out_direction,
      fade_rate,
      profile.fade_completion_callback,
  };
  result.fade_start =
      invoke(profile.fade_start_entry, fade_arguments, execution_budget);
  if (!result.fade_start.completed()) {
    return result;
  }

  const std::array event_arguments{
      profile.camera_event_id,
      profile.camera_event_priority,
      profile.camera_source,
      profile.camera_source,
      0U,
      0U,
      0U,
      0U,
  };
  result.camera_event =
      invoke(profile.mission_event_entry, event_arguments, execution_budget);
  return result;
}

LegacyFirstMissionBootstrapResult LegacyGameplayVm::bootstrapFirstMission(
    const LegacyFirstMissionBootstrapProfile &profile,
    const LegacyRetailPlatformTailProfile &tail_profile,
    const LegacyFirstMissionOpeningProfile &opening_profile,
    std::uint64_t execution_budget) {
  return bootstrapMission(0U, true, profile, tail_profile, opening_profile,
                          execution_budget);
}

LegacyFirstMissionBootstrapResult LegacyGameplayVm::bootstrapMission(
    std::uint32_t mission_selection_index, bool start_first_mission_opening,
    const LegacyFirstMissionBootstrapProfile &profile,
    const LegacyRetailPlatformTailProfile &tail_profile,
    const LegacyFirstMissionOpeningProfile &opening_profile,
    std::uint64_t execution_budget) {
  LegacyFirstMissionBootstrapResult result;
  agent_cbdc_friendly_fire_frame_.reset();
  agent_cbdc_friendly_fire_pending_penalties_ = 0U;
  interrupt_callbacks_.fill(0U);
  runtime_.reset(executable_initial_pc_, profile.global_pointer,
                 profile.stack_pointer);

  const auto run = [&](LegacyFirstMissionBootstrapPhase phase,
                       std::uint32_t address,
                       std::span<const std::uint32_t> arguments) {
    result.phase = phase;
    result.execution = invoke(address, arguments, execution_budget);
    return result.execution.completed();
  };
  constexpr std::array common_arguments{1U};
  if (!run(LegacyFirstMissionBootstrapPhase::common_init,
           profile.common_init_entry, common_arguments) ||
      !run(LegacyFirstMissionBootstrapPhase::pop_title, profile.pop_state_entry,
           {})) {
    return result;
  }
  const std::array mission_arguments{mission_selection_index};
  if (!run(LegacyFirstMissionBootstrapPhase::select_mission,
           profile.select_mission_entry, mission_arguments) ||
      !run(LegacyFirstMissionBootstrapPhase::pop_transition,
           profile.pop_state_entry, {})) {
    return result;
  }
  constexpr std::array mission_init_arguments{4U};
  if (!run(LegacyFirstMissionBootstrapPhase::mission_init,
           profile.common_init_entry, mission_init_arguments)) {
    return result;
  }

  result.phase = LegacyFirstMissionBootstrapPhase::initialize_fade;
  const auto loading_tail =
      tickRetailPlatformTail(false, tail_profile, execution_budget);
  result.execution = loading_tail.delayed_callbacks;
  if (!loading_tail.completed()) {
    result.bridge_fault = loading_tail.bridge_fault;
    return result;
  }

  std::uint32_t loading_ui{};
  std::uint8_t loading_fade{};
  if (!runtime_.read32(profile.loading_ui_handle, loading_ui) ||
      !runtime_.read8(profile.loading_fade_handle, loading_fade)) {
    result.bridge_fault = true;
    return result;
  }
  const std::array loading_ui_find_arguments{loading_ui,
                                             profile.global_pointer};
  if (!run(LegacyFirstMissionBootstrapPhase::release_loading_ui,
           profile.loading_ui_find_entry, loading_ui_find_arguments)) {
    return result;
  }
  const std::array loading_ui_release_arguments{result.execution.return_value};
  if (!run(LegacyFirstMissionBootstrapPhase::release_loading_ui,
           profile.loading_ui_release_entry, loading_ui_release_arguments)) {
    return result;
  }
  const std::array loading_fade_arguments{
      static_cast<std::uint32_t>(loading_fade)};
  if (!run(LegacyFirstMissionBootstrapPhase::release_loading_fade,
           profile.loading_fade_release_entry, loading_fade_arguments) ||
      !run(LegacyFirstMissionBootstrapPhase::reset_loading_ui,
           profile.loading_ui_reset_entry, {})) {
    return result;
  }
  if (!runtime_.write32(profile.loading_ui_handle, 0xffffffffU) ||
      !runtime_.write8(profile.loading_fade_handle, 0xffU)) {
    result.bridge_fault = true;
    return result;
  }

  if (!run(LegacyFirstMissionBootstrapPhase::initialize_display,
           profile.display_memory_query_entry, {})) {
    return result;
  }
  const std::array display_arguments{result.execution.return_value};
  if (!run(LegacyFirstMissionBootstrapPhase::initialize_display,
           profile.display_init_entry, display_arguments)) {
    return result;
  }
  if (!runtime_.write32(profile.display_owner, 0U)) {
    result.bridge_fault = true;
    return result;
  }
  if (!run(LegacyFirstMissionBootstrapPhase::pop_loading,
           profile.pop_state_entry, {})) {
    return result;
  }

  // The native renderer owns FUN_8001629c's GPU reset, so the abbreviated
  // bootstrap never executes the original store to DAT_80116962. Preserve
  // that exact gameplay-visible side effect at the same post-loading edge;
  // otherwise every authored terrain trigger is silently disabled and
  // dormant actors such as Kravitch can never receive their retail event 12.
  result.phase = LegacyFirstMissionBootstrapPhase::enable_gameplay_triggers;
  if (!runtime_.write8(profile.gameplay_trigger_enable, 1U)) {
    result.bridge_fault = true;
    return result;
  }

  if (!start_first_mission_opening) {
    result.phase = LegacyFirstMissionBootstrapPhase::ready;
    return result;
  }

  result.phase = LegacyFirstMissionBootstrapPhase::start_opening;
  const auto opening =
      startFirstMissionOpeningWithoutMovie(opening_profile, execution_budget);
  if (!opening.completed()) {
    constexpr auto failed = [](const LegacyGameplayVmResult &value) {
      return !value.completed();
    };
    const std::array opening_calls{
        opening.remove_movie_callback,
        opening.fade_reset,
        opening.fade_start,
        opening.camera_event,
    };
    if (const auto call = std::ranges::find_if(opening_calls, failed);
        call != opening_calls.end()) {
      result.execution = *call;
    }
    return result;
  }
  result.execution = opening.camera_event;
  result.phase = LegacyFirstMissionBootstrapPhase::ready;
  return result;
}

LegacyMissionTickResult
LegacyGameplayVm::tickMission(const LegacyMissionRuntimeProfile &profile,
                              std::uint64_t per_call_execution_budget) {
  LegacyMissionTickResult result;
  constexpr std::array frame_event_arguments{
      4U, 5U, 0xfffeU, 0xfffeU, 0U, 0U, 0U, 0U,
  };
  result.frame_event = invoke(profile.frame_event_entry, frame_event_arguments,
                              per_call_execution_budget);
  if (!result.frame_event.completed()) {
    return result;
  }

  result.delayed_callbacks =
      invoke(profile.delayed_callbacks_entry, {}, per_call_execution_budget);
  if (!result.delayed_callbacks.completed()) {
    return result;
  }

  const std::array queue_arguments{
      profile.pending_queue_count,
      profile.ready_queue_count,
  };
  result.queue_drain = invoke(profile.queue_drain_entry, queue_arguments,
                              per_call_execution_budget);
  if (!result.queue_drain.completed()) {
    return result;
  }

  if (!runtime_.read32(profile.ready_queue_count, result.ready_events)) {
    result.bridge_fault = true;
    return result;
  }
  if (result.ready_events == 0U || (result.ready_events & 0x80000000U) != 0U) {
    return result;
  }
  if (result.ready_events > profile.maximum_ready_events) {
    result.bridge_fault = true;
    return result;
  }
  result.dispatched_events.reserve(result.ready_events);

  constexpr std::uint32_t event_stride = 0x1cU;
  constexpr std::uint32_t event_destination_offset = 8U;
  constexpr std::uint32_t event_table_stride = 12U;
  constexpr std::uint32_t object_record_stride = 0x4cU;
  constexpr std::uint32_t object_definition_stride = 0x14U;
  for (std::uint32_t index = 0U; index < result.ready_events; ++index) {
    std::uint32_t current_ready_events{};
    if (!runtime_.read32(profile.ready_queue_count, current_ready_events)) {
      result.bridge_fault = true;
      return result;
    }
    if (current_ready_events == 0U) {
      break;
    }

    const auto event_address =
        profile.ready_queue_entries + index * event_stride;
    std::uint16_t event_id{};
    std::uint32_t destination{};
    if (!runtime_.read16(event_address, event_id) ||
        !runtime_.read32(event_address + event_destination_offset,
                         destination)) {
      result.bridge_fault = true;
      return result;
    }

    std::uint32_t handler{};
    auto nullable_handler = false;
    if (destination == 0xffffU) {
      if (!runtime_.read32(profile.static_event_table +
                               event_id * event_table_stride,
                           handler)) {
        result.bridge_fault = true;
        return result;
      }
    } else if (destination - 0xfffdU < 2U) {
      std::uint32_t dynamic_table{};
      if (!runtime_.read32(profile.dynamic_event_table_pointer,
                           dynamic_table) ||
          !runtime_.read32(dynamic_table + event_id * event_table_stride + 4U,
                           handler)) {
        result.bridge_fault = true;
        return result;
      }
    } else if (destination == 0x29aU) {
      nullable_handler = true;
      if (!runtime_.read32(profile.special_object_handler_pointer, handler)) {
        result.bridge_fault = true;
        return result;
      }
    } else {
      nullable_handler = true;
      std::uint32_t object_records{};
      std::uint32_t object_definitions{};
      std::uint32_t definition_index{};
      std::uint16_t class_id{};
      if (!runtime_.read32(profile.object_records_pointer, object_records) ||
          !runtime_.read32(profile.object_definitions_pointer,
                           object_definitions) ||
          !runtime_.read32(object_records + destination * object_record_stride,
                           definition_index) ||
          !runtime_.read16(object_definitions +
                               definition_index * object_definition_stride,
                           class_id)) {
        result.bridge_fault = true;
        return result;
      }
      auto class_index = static_cast<std::uint32_t>(class_id);
      if ((class_id & 0x8000U) != 0U) {
        class_index |= 0xffff0000U;
      }
      if (!runtime_.read32(profile.object_handler_table + class_index * 4U,
                           handler)) {
        result.bridge_fault = true;
        return result;
      }
    }

    if (nullable_handler && handler == 0U) {
      continue;
    }
    result.dispatched_events.push_back(invoke(
        handler, std::span{&event_address, 1U}, per_call_execution_budget));
    if (!result.dispatched_events.back().completed()) {
      return result;
    }
  }
  return result;
}

} // namespace sf::game
