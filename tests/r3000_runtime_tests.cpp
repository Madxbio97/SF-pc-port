#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/legacy_virtual_cd.hpp"
#include "sf/game/player_controller.hpp"
#include "sf/psx/executable.hpp"
#include "sf/psx/gte_runtime.hpp"
#include "sf/psx/machine.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t code_address = 0x80010000U;
constexpr std::uint32_t legacy_common_npc_handler = 0x80061874U;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

constexpr std::uint32_t encodeR(std::uint32_t rs, std::uint32_t rt,
                                std::uint32_t rd, std::uint32_t shift,
                                std::uint32_t function) noexcept {
  return (rs << 21U) | (rt << 16U) | (rd << 11U) | (shift << 6U) | function;
}

constexpr std::uint32_t encodeI(std::uint32_t opcode, std::uint32_t rs,
                                std::uint32_t rt,
                                std::uint16_t immediate) noexcept {
  return (opcode << 26U) | (rs << 21U) | (rt << 16U) | immediate;
}

constexpr std::uint32_t encodeJ(std::uint32_t opcode,
                                std::uint32_t address) noexcept {
  return (opcode << 26U) | ((address >> 2U) & 0x03ffffffU);
}

constexpr std::uint32_t
encodeCop0Transfer(std::uint32_t operation, std::uint32_t rt,
                   std::uint32_t register_index) noexcept {
  return (0x10U << 26U) | (operation << 21U) | (rt << 16U) |
         (register_index << 11U);
}

constexpr std::uint32_t
encodeCop2Transfer(std::uint32_t operation, std::uint32_t rt,
                   std::uint32_t data_register) noexcept {
  return (0x12U << 26U) | (operation << 21U) | (rt << 16U) |
         (data_register << 11U);
}

void writeLe32(std::span<std::byte> bytes, std::size_t offset,
               std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
  bytes[offset + 2U] = static_cast<std::byte>(value >> 16U);
  bytes[offset + 3U] = static_cast<std::byte>(value >> 24U);
}

template <std::size_t Size>
std::vector<std::byte>
instructionBytes(const std::array<std::uint32_t, Size> &words) {
  std::vector<std::byte> bytes(words.size() * sizeof(std::uint32_t));
  for (std::size_t index = 0; index < words.size(); ++index) {
    writeLe32(bytes, index * sizeof(std::uint32_t), words[index]);
  }
  return bytes;
}

template <std::size_t Size>
void loadCode(sf::psx::R3000Runtime &runtime,
              const std::array<std::uint32_t, Size> &words) {
  const auto bytes = instructionBytes(words);
  require(runtime.loadBytes(code_address, bytes),
          "Could not load synthetic R3000 code");
}

void testBranchDelay() {
  sf::psx::R3000Runtime runtime;
  constexpr std::array words{
      encodeI(0x09U, 0U, 8U, 1U),      encodeI(0x04U, 8U, 8U, 2U),
      encodeI(0x09U, 0U, 2U, 7U),      encodeI(0x09U, 0U, 2U, 99U),
      encodeR(31U, 0U, 0U, 0U, 0x08U), encodeI(0x09U, 0U, 3U, 5U),
  };
  loadCode(runtime, words);
  const auto result = runtime.call(code_address);
  require(result.reason == sf::psx::R3000StopReason::returned,
          "Branch-delay program did not return");
  require(result.instructions == 5U,
          "Branch delay executed the wrong instruction count");
  require(runtime.state().gpr[2] == 7U,
          "Taken branch did not execute its delay slot");
  require(runtime.state().gpr[3] == 5U, "JR did not execute its delay slot");
}

void testLoadDelay() {
  sf::psx::R3000Runtime runtime;
  constexpr std::array words{
      encodeI(0x0fU, 0U, 8U, 0x8001U), encodeI(0x23U, 8U, 2U, 0x0200U),
      encodeI(0x09U, 2U, 3U, 1U),      encodeR(2U, 0U, 4U, 0U, 0x21U),
      encodeI(0x23U, 8U, 5U, 0x0200U), encodeI(0x09U, 0U, 5U, 9U),
      encodeR(31U, 0U, 0U, 0U, 0x08U), 0U,
  };
  loadCode(runtime, words);
  require(runtime.write32(0x80010200U, 0x12345678U),
          "Could not seed load-delay data");
  const auto result = runtime.call(code_address);
  require(result.reason == sf::psx::R3000StopReason::returned,
          "Load-delay program did not return");
  require(runtime.state().gpr[3] == 1U,
          "Load result was visible one instruction too early");
  require(runtime.state().gpr[4] == 0x12345678U,
          "Delayed load never became visible");
  require(runtime.state().gpr[5] == 9U,
          "Register write did not cancel an incoming load");

  constexpr std::array return_load_words{
      encodeI(0x0fU, 0U, 8U, 0x8001U),
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      encodeI(0x23U, 8U, 2U, 0x0200U),
  };
  loadCode(runtime, return_load_words);
  const auto return_load_result = runtime.call(code_address);
  require(return_load_result.reason == sf::psx::R3000StopReason::returned &&
              runtime.state().gpr[2] == 0x12345678U,
          "Load in a JR delay slot was lost at the host return boundary");

  constexpr std::array stop_load_words{
      encodeI(0x0fU, 0U, 8U, 0x8001U),
      encodeI(0x23U, 8U, 2U, 0x0200U),
      0xffffffffU,
  };
  loadCode(runtime, stop_load_words);
  const auto stop_load_result = runtime.call(code_address);
  require(
      stop_load_result.reason ==
              sf::psx::R3000StopReason::unsupported_instruction &&
          runtime.state().gpr[2] == 0x12345678U,
      "Pending load was lost when the following instruction stopped execution");
}

void testMultiplyAndDivide() {
  sf::psx::R3000Runtime runtime;
  constexpr std::array multiply_words{
      encodeR(4U, 5U, 0U, 0U, 0x18U),
      encodeR(0U, 0U, 2U, 0U, 0x12U),
      encodeR(0U, 0U, 3U, 0U, 0x10U),
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  loadCode(runtime, multiply_words);
  const std::array multiply_arguments{static_cast<std::uint32_t>(-3), 7U};
  auto result = runtime.call(code_address, multiply_arguments);
  require(result.reason == sf::psx::R3000StopReason::returned,
          "Signed multiply program did not return");
  require(runtime.state().gpr[2] == static_cast<std::uint32_t>(-21) &&
              runtime.state().gpr[3] == 0xffffffffU,
          "Signed multiply HI/LO mismatch");

  constexpr std::array divide_words{
      encodeR(4U, 5U, 0U, 0U, 0x1aU),
      encodeR(0U, 0U, 2U, 0U, 0x12U),
      encodeR(0U, 0U, 3U, 0U, 0x10U),
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  loadCode(runtime, divide_words);
  const std::array divide_arguments{static_cast<std::uint32_t>(-8), 3U};
  result = runtime.call(code_address, divide_arguments);
  require(result.reason == sf::psx::R3000StopReason::returned,
          "Signed divide program did not return");
  require(runtime.state().gpr[2] == static_cast<std::uint32_t>(-2) &&
              runtime.state().gpr[3] == static_cast<std::uint32_t>(-2),
          "Signed divide quotient/remainder mismatch");
}

void testCop0Status() {
  sf::psx::R3000Runtime runtime;
  constexpr std::array words{
      encodeI(0x0fU, 0U, 8U, 0x4000U), encodeI(0x0dU, 8U, 8U, 0x1234U),
      encodeCop0Transfer(4U, 8U, 12U), encodeCop0Transfer(0U, 2U, 12U),
      encodeI(0x09U, 2U, 3U, 1U),      encodeR(2U, 0U, 4U, 0U, 0x21U),
      encodeR(31U, 0U, 0U, 0U, 0x08U), 0U,
  };
  loadCode(runtime, words);
  const auto result = runtime.call(code_address);
  require(result.reason == sf::psx::R3000StopReason::returned,
          "COP0 Status transfer program did not return");
  require(runtime.state().cop0_status == 0x40001234U &&
              runtime.state().gpr[2] == 0x40001234U &&
              runtime.state().gpr[3] == 1U &&
              runtime.state().gpr[4] == 0x40001234U,
          "COP0 Status MFC0/MTC0 or load delay mismatch");

  constexpr std::array cause_words{
      encodeCop0Transfer(0U, 2U, 13U),
      0U,
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  loadCode(runtime, cause_words);
  const auto cause_result = runtime.call(code_address);
  require(cause_result.reason == sf::psx::R3000StopReason::returned &&
              runtime.state().gpr[2] == 0U,
          "COP0 Cause register transfer mismatch");

  constexpr std::array unsupported_words{
      encodeCop0Transfer(0U, 2U, 7U),
  };
  loadCode(runtime, unsupported_words);
  const auto unsupported = runtime.call(code_address);
  require(unsupported.reason ==
              sf::psx::R3000StopReason::unsupported_instruction,
          "Unsupported COP0 register transfer did not stop deterministically");
}

class RecordingDmaPort final : public sf::psx::DmaPort {
public:
  [[nodiscard]] bool readDmaWord(std::uint32_t &value) noexcept override {
    if (read_index_ >= read_words.size()) {
      return false;
    }
    value = read_words[read_index_++];
    return true;
  }

  [[nodiscard]] bool writeDmaWord(std::uint32_t value) noexcept override {
    written_words.push_back(value);
    return true;
  }

  std::vector<std::uint32_t> read_words;
  std::vector<std::uint32_t> written_words;

private:
  std::size_t read_index_{};
};

class PatternCdRomMedia final : public sf::psx::CdRomMedia {
public:
  [[nodiscard]] std::uint32_t sectorCount() const noexcept override {
    return sector_count;
  }

  [[nodiscard]] bool readDataSector(
      std::uint32_t lba,
      std::span<std::byte, sector_size> destination) noexcept override {
    if (lba >= sector_count) {
      return false;
    }
    for (std::size_t index = 0U; index < destination.size(); ++index) {
      destination[index] = static_cast<std::byte>(
          (static_cast<std::size_t>(lba) * 17U + index) & 0xffU);
    }
    ++reads;
    return true;
  }

  static constexpr std::uint32_t sector_count = 8U;
  std::uint32_t reads{};
};

bool sameCpuState(const sf::psx::R3000State &left,
                  const sf::psx::R3000State &right) noexcept {
  const auto same_load = [](const sf::psx::R3000DelayedLoadState &a,
                            const sf::psx::R3000DelayedLoadState &b) {
    return a.reg == b.reg && a.value == b.value && a.valid == b.valid;
  };
  return left.gpr == right.gpr && left.gte.data == right.gte.data &&
         left.gte.control == right.gte.control &&
         left.cop0_status == right.cop0_status &&
         left.cop0_cause == right.cop0_cause &&
         left.cop0_epc == right.cop0_epc &&
         left.cop0_bad_vaddr == right.cop0_bad_vaddr && left.hi == right.hi &&
         left.lo == right.lo && left.pc == right.pc &&
         left.next_pc == right.next_pc && left.branch_pc == right.branch_pc &&
         left.branch_delay_slot == right.branch_delay_slot &&
         same_load(left.load_delay, right.load_delay) &&
         same_load(left.next_load_delay, right.next_load_delay);
}

bool sameMachineTimeline(const sf::psx::PsxMachineState &left,
                         const sf::psx::PsxMachineState &right) noexcept {
  if (left.cpu_clock_scale != right.cpu_clock_scale ||
      left.scheduler.now != right.scheduler.now ||
      left.scheduler.next_token != right.scheduler.next_token ||
      left.scheduler.event_count != right.scheduler.event_count ||
      left.pending_cpu_ticks != right.pending_cpu_ticks ||
      left.device_tick_remainder != right.device_tick_remainder ||
      left.interrupts.status != right.interrupts.status ||
      left.interrupts.mask != right.interrupts.mask ||
      left.interrupts.input_lines != right.interrupts.input_lines ||
      left.dma != right.dma || left.cdrom != right.cdrom ||
      left.xa_decoder != right.xa_decoder || left.timers != right.timers) {
    return false;
  }
  for (std::size_t index = 0U; index < left.scheduler.events.size(); ++index) {
    const auto &a = left.scheduler.events[index];
    const auto &b = right.scheduler.events[index];
    if (a.deadline != b.deadline || a.token != b.token ||
        a.payload != b.payload || a.type != b.type || a.index != b.index) {
      return false;
    }
  }
  return true;
}

bool sameMachineState(const sf::psx::PsxMachineState &left,
                      const sf::psx::PsxMachineState &right) noexcept {
  if (left.cpu_clock_scale != right.cpu_clock_scale ||
      left.scheduler.now != right.scheduler.now ||
      left.scheduler.next_token != right.scheduler.next_token ||
      left.scheduler.event_count != right.scheduler.event_count ||
      left.pending_cpu_ticks != right.pending_cpu_ticks ||
      left.device_tick_remainder != right.device_tick_remainder ||
      left.interrupts.status != right.interrupts.status ||
      left.interrupts.mask != right.interrupts.mask ||
      left.interrupts.input_lines != right.interrupts.input_lines ||
      left.dma != right.dma || left.cdrom != right.cdrom ||
      static_cast<bool>(left.spu) != static_cast<bool>(right.spu) ||
      (left.spu && *left.spu != *right.spu) ||
      left.xa_decoder != right.xa_decoder || left.timers != right.timers) {
    return false;
  }
  for (std::size_t index = 0U; index < sf::psx::EventSchedulerState::capacity;
       ++index) {
    const auto &left_event = left.scheduler.events[index];
    const auto &right_event = right.scheduler.events[index];
    if (left_event.deadline != right_event.deadline ||
        left_event.token != right_event.token ||
        left_event.payload != right_event.payload ||
        left_event.type != right_event.type ||
        left_event.index != right_event.index) {
      return false;
    }
  }
  return true;
}

std::uint64_t
logicalMachineTick(const sf::psx::PsxMachineState &state) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return state.pending_cpu_ticks > maximum - state.scheduler.now
             ? maximum
             : state.scheduler.now + state.pending_cpu_ticks;
}

void testMachineInterrupts() {
  constexpr std::uint32_t i_stat = 0x1f801070U;
  constexpr std::uint32_t i_mask = 0x1f801074U;
  constexpr std::uint32_t cause_ip2 = 1U << 10U;
  constexpr std::uint32_t cause_branch_delay = 1U << 31U;

  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  std::uint16_t value{};
  require(runtime.read16(i_stat, value) && value == 0U &&
              runtime.read16(i_mask, value) && value == 0U,
          "Machine interrupt controller did not reset cleanly");

  machine.setVBlank(true);
  require(runtime.read16(i_stat, value) && value == 1U &&
              (runtime.state().cop0_cause & cause_ip2) == 0U,
          "VBlank edge was not latched independently of I_MASK");
  require(runtime.write16(i_mask, 1U) &&
              (runtime.state().cop0_cause & cause_ip2) != 0U,
          "I_MASK did not drive COP0 Cause.IP2");
  require(runtime.write16(i_stat, 0xfffeU) && runtime.read16(i_stat, value) &&
              value == 0U && (runtime.state().cop0_cause & cause_ip2) == 0U,
          "I_STAT acknowledgement did not clear VBlank/Cause.IP2");

  machine.setVBlank(true);
  require(runtime.read16(i_stat, value) && value == 0U,
          "Level-held VBlank incorrectly retriggered without an edge");
  machine.setVBlank(false);
  machine.setVBlank(true);
  require(runtime.read16(i_stat, value) && value == 1U,
          "Second VBlank rising edge was not latched");
  machine.setVBlank(false);
  require(runtime.write16(i_stat, 0xfffeU),
          "Could not acknowledge the second VBlank edge");

  constexpr std::array branch_words{
      encodeI(0x04U, 0U, 0U, 1U),
      0U,
      0U,
  };
  loadCode(runtime, branch_words);
  constexpr std::array handler_words{0x42000010U}; // RFE
  const auto handler = instructionBytes(handler_words);
  require(runtime.loadBytes(0x80000080U, handler),
          "Could not load the synthetic interrupt handler");
  runtime.reset(code_address);
  auto cpu_state = runtime.state();
  cpu_state.cop0_status = cause_ip2 | 1U;
  runtime.restoreCpuState(cpu_state);

  const auto branch = machine.step();
  require(branch.reason == sf::psx::R3000StopReason::running &&
              branch.instructions == 1U,
          "Machine did not execute the branch before the interrupt");
  machine.pulseVBlank();
  const auto exception = machine.step();
  require(exception.reason == sf::psx::R3000StopReason::running &&
              exception.instructions == 0U &&
              runtime.state().pc == 0x80000080U &&
              runtime.state().cop0_epc == code_address &&
              (runtime.state().cop0_cause & cause_branch_delay) != 0U &&
              (runtime.state().cop0_status & 1U) == 0U &&
              (runtime.state().cop0_status & 4U) != 0U,
          "CPU interrupt entry lost EPC/BD or the COP0 mode stack");

  require(runtime.write16(i_stat, 0xfffeU),
          "Could not acknowledge VBlank before RFE");
  const auto rfe = machine.step();
  require(rfe.reason == sf::psx::R3000StopReason::running &&
              rfe.instructions == 1U &&
              (runtime.state().cop0_status & 1U) != 0U,
          "RFE did not restore the current interrupt-enable bit");
}

void testMachineTimer2() {
  constexpr std::uint32_t i_stat = 0x1f801070U;
  constexpr std::uint32_t timer2_counter = 0x1f801120U;
  constexpr std::uint32_t timer2_mode = 0x1f801124U;
  constexpr std::uint32_t timer2_target = 0x1f801128U;
  constexpr std::uint16_t timer2_irq = 1U << 6U;

  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  require(runtime.write16(timer2_target, 0x44e8U) &&
              runtime.write16(timer2_mode, 0x0258U),
          "Could not configure Timer2");

  machine.advanceTicks(141'119U);
  std::uint16_t value{};
  require(runtime.read16(timer2_counter, value) && value == 0x44e7U &&
              runtime.read16(i_stat, value) && (value & timer2_irq) == 0U,
          "Timer2 fired before its divided target deadline");
  machine.advanceTicks(1U);
  require(machine.currentTick() == 141'120U &&
              runtime.read16(timer2_counter, value) && value == 0U &&
              runtime.read16(i_stat, value) && (value & timer2_irq) != 0U,
          "Timer2 did not fire exactly at 141120 CPU ticks");

  std::uint16_t first_mode{};
  std::uint16_t second_mode{};
  require(runtime.read16(timer2_mode, first_mode) &&
              runtime.read16(timer2_mode, second_mode) &&
              (first_mode & 0x0c00U) == 0x0c00U &&
              (second_mode & 0x0c00U) == 0x0400U,
          "Timer2 reached-target flag was not read-to-clear");
  require(runtime.write16(i_stat, 0xffbfU), "Could not acknowledge Timer2");
  machine.advanceTicks(141'120U);
  require(runtime.read16(i_stat, value) && (value & timer2_irq) != 0U,
          "Timer2 repeat mode did not raise a second interrupt");

  sf::psx::R3000Runtime fast_runtime;
  sf::psx::PsxMachine fast_machine{fast_runtime};
  require(fast_runtime.write16(0x1f801108U, 1U) &&
              fast_runtime.write16(0x1f801104U, 0x0058U),
          "Could not configure the timer fast-forward case");
  fast_machine.advanceTicks(std::numeric_limits<std::uint64_t>::max());
  require(fast_runtime.read16(0x1f801100U, value) && value == 0U &&
              fast_runtime.read16(i_stat, value) && (value & (1U << 4U)) != 0U,
          "Large timer advance did not fast-forward deterministically");
}

void testMachineDmaCancellation() {
  constexpr std::uint32_t gpu_dma = 0x1f8010a0U;
  constexpr std::uint32_t dpcr = 0x1f8010f0U;

  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  RecordingDmaPort gpu;
  machine.attachDmaPort(sf::psx::DmaChannel::gpu, &gpu);
  require(runtime.write32(0x3000U, 0x12345678U) &&
              runtime.write32(dpcr, machine.dma().dpcr() | 0x00000800U) &&
              runtime.write32(gpu_dma, 0x3000U) &&
              runtime.write32(gpu_dma + 4U, 1U) &&
              runtime.write32(gpu_dma + 8U, 0x11000001U),
          "Could not start cancellable DMA");

  for (std::size_t iteration = 0U; iteration < 40U; ++iteration) {
    machine.attachDmaPort(sf::psx::DmaChannel::gpu, nullptr);
    const auto cancelled = machine.captureState();
    require(cancelled.scheduler.event_count == 0U &&
                machine.validateState(cancelled),
            "Cancelled DMA left a stale scheduler event");
    machine.attachDmaPort(sf::psx::DmaChannel::gpu, &gpu);
  }
  machine.advanceTicks(1U);
  require(gpu.written_words == std::vector<std::uint32_t>{0x12345678U},
          "Repeated DMA cancellation exhausted or corrupted the scheduler");
}

void testMachineLinearDmaAndSnapshot() {
  constexpr std::uint32_t i_stat = 0x1f801070U;
  constexpr std::uint32_t gpu_dma = 0x1f8010a0U;
  constexpr std::uint32_t dpcr = 0x1f8010f0U;
  constexpr std::uint32_t dicr = 0x1f8010f4U;
  constexpr std::array payloads{0x10203040U, 0x55667788U, 0x90abcdefU};

  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  RecordingDmaPort gpu;
  machine.attachDmaPort(sf::psx::DmaChannel::gpu, &gpu);
  for (std::size_t index = 0U; index < payloads.size(); ++index) {
    require(runtime.write32(0x3000U + static_cast<std::uint32_t>(index * 4U),
                            payloads[index]),
            "Could not seed linear GPU DMA payload");
  }
  require(runtime.write32(dpcr, machine.dma().dpcr() | 0x00000800U) &&
              runtime.write32(dicr, 0x00840000U) &&
              runtime.write32(gpu_dma, 0x3000U) &&
              runtime.write32(gpu_dma + 4U, 3U) &&
              runtime.write32(gpu_dma + 8U, 0x11000001U),
          "Could not start linear GPU DMA");

  machine.advanceTicks(2U);
  const auto checkpoint = machine.captureState();
  require(gpu.written_words.empty() && machine.currentTick() == 2U,
          "Linear DMA completed before its word deadline");
  machine.advanceTicks(1U);
  require(
      gpu.written_words ==
              std::vector<std::uint32_t>(payloads.begin(), payloads.end()) &&
          (machine.dma().chcr(sf::psx::DmaChannel::gpu) & 0x01000000U) == 0U &&
          (machine.dma().dicr() & 0x84000000U) == 0x84000000U,
      "Linear GPU DMA completion or DICR flags mismatch");
  std::uint16_t interrupt_status{};
  require(runtime.read16(i_stat, interrupt_status) &&
              (interrupt_status & (1U << 3U)) != 0U,
          "DMA completion did not latch I_STAT.DMA");
  std::uint32_t dma_interrupt_control{};
  require(runtime.write32(dicr, 0x04840000U) &&
              runtime.read32(dicr, dma_interrupt_control) &&
              dma_interrupt_control == 0x00840000U &&
              runtime.read16(i_stat, interrupt_status) &&
              (interrupt_status & (1U << 3U)) != 0U &&
              runtime.write16(i_stat, 0xfff7U),
          "DICR W1C incorrectly acknowledged the separate I_STAT latch");

  gpu.written_words.clear();
  require(machine.restoreState(checkpoint) && machine.currentTick() == 2U,
          "Machine rejected a valid pending-DMA snapshot");
  machine.advanceTicks(1U);
  require(gpu.written_words == std::vector<std::uint32_t>(payloads.begin(),
                                                          payloads.end()) &&
              (machine.dma().dicr() & 0x84000000U) == 0x84000000U,
          "Restored scheduler/device state did not replay deterministically");
}

void testMachineOtcDma() {
  constexpr std::uint32_t otc_dma = 0x1f8010e0U;
  constexpr std::uint32_t dpcr = 0x1f8010f0U;

  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  require(runtime.write32(dpcr, machine.dma().dpcr() | 0x08000000U) &&
              runtime.write32(otc_dma, 0x100cU) &&
              runtime.write32(otc_dma + 4U, 4U) &&
              runtime.write32(otc_dma + 8U, 0x11000002U),
          "Could not start OTC DMA");
  machine.advanceTicks(3U);
  std::uint32_t value{};
  require(runtime.read32(0x100cU, value) && value == 0U,
          "OTC DMA completed before four ticks");
  machine.advanceTicks(1U);
  constexpr std::array expected{
      0x00ffffffU,
      0x00001000U,
      0x00001004U,
      0x00001008U,
  };
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    require(runtime.read32(0x1000U + static_cast<std::uint32_t>(index * 4U),
                           value) &&
                value == expected[index],
            "OTC DMA produced an invalid ordering-table chain");
  }
  require((machine.dma().chcr(sf::psx::DmaChannel::otc) & 0x01000000U) == 0U,
          "OTC DMA left its busy bit set");
}

void testMachineLinkedListDma() {
  constexpr std::uint32_t gpu_dma = 0x1f8010a0U;
  constexpr std::uint32_t dpcr = 0x1f8010f0U;
  constexpr std::array payloads{0xa1a2a3a4U, 0xb1b2b3b4U, 0xc1c2c3c4U};

  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  RecordingDmaPort gpu;
  machine.attachDmaPort(sf::psx::DmaChannel::gpu, &gpu);
  require(runtime.write32(0x1000U, 0x02002000U) &&
              runtime.write32(0x1004U, payloads[0]) &&
              runtime.write32(0x1008U, payloads[1]) &&
              runtime.write32(0x2000U, 0x01800000U) &&
              runtime.write32(0x2004U, payloads[2]) &&
              runtime.write32(dpcr, machine.dma().dpcr() | 0x00000800U) &&
              runtime.write32(gpu_dma, 0x1000U) &&
              runtime.write32(gpu_dma + 8U, 0x01000401U),
          "Could not start linked-list GPU DMA");
  machine.advanceTicks(30U);
  require(gpu.written_words.empty(),
          "Linked-list GPU DMA completed before its 31-tick deadline");
  machine.advanceTicks(1U);
  require(
      gpu.written_words ==
              std::vector<std::uint32_t>(payloads.begin(), payloads.end()) &&
          (machine.dma().chcr(sf::psx::DmaChannel::gpu) & 0x01000000U) == 0U,
      "Linked-list GPU DMA payload order or completion mismatch");
}

void testMachineCdRomDmaAndSnapshot() {
  constexpr std::uint32_t i_stat = 0x1f801070U;
  constexpr std::uint32_t cdrom = 0x1f801800U;
  constexpr std::uint32_t cdrom_dma = 0x1f8010b0U;
  constexpr std::uint32_t dpcr = 0x1f8010f0U;
  constexpr std::uint32_t destination = 0x00004000U;
  constexpr std::uint16_t cdrom_irq = 1U << 2U;
  constexpr std::uint32_t source_lba = 2U;

  sf::psx::R3000Runtime runtime;
  sf::psx::PsxMachine machine{runtime};
  PatternCdRomMedia media;
  machine.setCdRomMedia(&media);

  const auto select_index = [&](std::uint8_t index) {
    return runtime.write8(cdrom, index);
  };
  const auto acknowledge = [&](std::uint8_t expected_interrupt) {
    std::uint8_t flags{};
    std::uint16_t status{};
    return select_index(1U) && runtime.read8(cdrom + 3U, flags) &&
           (flags & 0x07U) == expected_interrupt &&
           runtime.write8(cdrom + 3U, expected_interrupt) &&
           runtime.write16(i_stat, 0xfffbU) && runtime.read16(i_stat, status) &&
           (status & cdrom_irq) == 0U && select_index(0U);
  };
  const auto issue_command = [&](std::uint8_t command) {
    if (!runtime.write8(cdrom + 1U, command)) {
      return false;
    }
    machine.advanceTicks(sf::psx::CdRomController::command_delay_ticks);
    std::uint16_t status{};
    return runtime.read16(i_stat, status) && (status & cdrom_irq) != 0U;
  };

  require(select_index(1U) && runtime.write8(cdrom + 2U, 0x04U) &&
              select_index(0U),
          "Could not enable CD-ROM INT3");

  // LBA 2 is absolute 00:02:02. The controller consumes BCD MSF.
  require(runtime.write8(cdrom + 2U, 0x00U) &&
              runtime.write8(cdrom + 2U, 0x02U) &&
              runtime.write8(cdrom + 2U, 0x02U) && issue_command(0x02U) &&
              acknowledge(3U),
          "CD-ROM Setloc did not produce an acknowledge interrupt");
  require(runtime.write8(cdrom + 2U, 0xa0U) && issue_command(0x0eU) &&
              acknowledge(3U) && machine.cdrom().mode() == 0xa0U,
          "CD-ROM Setmode did not enable raw double-speed reads");
  require(issue_command(0x06U) && acknowledge(3U),
          "CD-ROM ReadN did not produce its command acknowledge");

  machine.advanceTicks(sf::psx::CdRomController::sector_double_speed_ticks);
  std::uint16_t interrupt_status{};
  require(runtime.read16(i_stat, interrupt_status) &&
              (interrupt_status & cdrom_irq) == 0U,
          "Disabled CD-ROM INT1 incorrectly raised I_STAT");
  require(select_index(1U) && runtime.write8(cdrom + 2U, 0x01U) &&
              runtime.read16(i_stat, interrupt_status) &&
              (interrupt_status & cdrom_irq) != 0U,
          "CD-ROM sector did not raise enabled INT1 through I_STAT");
  const auto active_irq_checkpoint = machine.captureState();
  require(acknowledge(1U) && machine.restoreState(active_irq_checkpoint) &&
              runtime.read16(i_stat, interrupt_status) &&
              (interrupt_status & cdrom_irq) != 0U &&
              (machine.interrupts().inputLines() & cdrom_irq) != 0U &&
              (machine.cdrom().captureState().interrupt_flags & 0x07U) == 1U,
          "Active CD-ROM IRQ snapshot did not restore IF/I_STAT/input line");
  require(select_index(0U) && runtime.write8(cdrom + 3U, 0x80U),
          "Could not request the CD-ROM data FIFO");

  std::array<std::uint8_t, 12U> mode2_header{};
  for (auto &byte : mode2_header) {
    require(runtime.read8(cdrom + 2U, byte),
            "Could not consume the MODE2 sector header through the CPU FIFO");
  }
  constexpr std::array<std::uint8_t, 12U> expected_header{
      0x00U, 0x02U, 0x02U, 0x02U, 0x00U, 0x00U,
      0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
  };
  require(mode2_header == expected_header && acknowledge(1U),
          "CD-ROM MODE2 header or INT1 acknowledgement mismatch");

  const auto checkpoint = machine.captureState();
  const std::vector<std::byte> ram_checkpoint(runtime.ram().begin(),
                                              runtime.ram().end());
  std::size_t sector_event_index = sf::psx::EventSchedulerState::capacity;
  for (std::size_t index = 0U; index < checkpoint.scheduler.event_count;
       ++index) {
    if (checkpoint.scheduler.events[index].type ==
        sf::psx::MachineEventType::cdrom_sector) {
      sector_event_index = index;
      break;
    }
  }
  require(sector_event_index < checkpoint.scheduler.event_count,
          "Mid-read CD-ROM snapshot did not retain the next sector event");

  const auto unmodified = machine.captureState();
  auto corrupt_payload = checkpoint;
  ++corrupt_payload.scheduler.events[sector_event_index].payload;
  require(
      !machine.restoreState(corrupt_payload) &&
          sameMachineState(unmodified, machine.captureState()),
      "Machine accepted a stale CD-ROM event payload or mutated on rejection");
  auto corrupt_generation = checkpoint;
  ++corrupt_generation.cdrom.sector_event.generation;
  require(!machine.restoreState(corrupt_generation) &&
              sameMachineState(unmodified, machine.captureState()),
          "Machine accepted a mismatched CD-ROM generation or mutated on "
          "rejection");
  auto corrupt_deadline = checkpoint;
  corrupt_deadline.scheduler.events[sector_event_index].deadline =
      checkpoint.scheduler.now + checkpoint.cdrom.sector_event.delay_ticks + 1U;
  require(!machine.restoreState(corrupt_deadline) &&
              sameMachineState(unmodified, machine.captureState()),
          "Machine accepted a delayed CD-ROM event or mutated on rejection");
  auto dangling_read = checkpoint.cdrom;
  dangling_read.sector_event.pending = 0U;
  dangling_read.sector_event.delay_ticks = 0U;
  require(!machine.cdrom().validateState(dangling_read),
          "CD-ROM accepted an active read without IRQ or sector event");
  auto impossible_interrupt = checkpoint.cdrom;
  impossible_interrupt.interrupt_flags = 7U;
  require(!machine.cdrom().validateState(impossible_interrupt),
          "CD-ROM accepted an impossible interrupt reason");

  const auto available_words = static_cast<std::uint32_t>(
      (checkpoint.cdrom.data_end - checkpoint.cdrom.data_position) /
      sizeof(std::uint32_t));
  const auto oversized_words = available_words + 1U;
  std::vector<std::byte> rejected_destination(
      static_cast<std::size_t>(oversized_words) * sizeof(std::uint32_t),
      std::byte{0xa5});
  require(oversized_words <= 0xffffU &&
              runtime.loadBytes(destination, rejected_destination),
          "Could not seed RAM for the rejected CD-ROM DMA test");
  const auto cdrom_before_rejection = machine.cdrom().captureState();
  require(runtime.write32(dpcr, machine.dma().dpcr() | 0x00008000U) &&
              runtime.write32(cdrom_dma, destination) &&
              runtime.write32(cdrom_dma + 4U, 0x00010000U | oversized_words) &&
              runtime.write32(cdrom_dma + 8U, 0x11000000U),
          "Could not start oversized CD-ROM DMA request");
  machine.advanceTicks(oversized_words);
  std::vector<std::byte> rejected_result(rejected_destination.size());
  require(runtime.copyBytes(destination, rejected_result) &&
              rejected_result == rejected_destination &&
              machine.cdrom().captureState() == cdrom_before_rejection &&
              machine.dma().madr(sf::psx::DmaChannel::cdrom) == destination &&
              (machine.dma().chcr(sf::psx::DmaChannel::cdrom) & 0x01000000U) ==
                  0U &&
              machine.dma().scheduledToken(sf::psx::DmaChannel::cdrom) == 0U,
          "Rejected CD-ROM DMA mutated data or left the channel busy");

  require(runtime.write32(cdrom_dma, destination) &&
              runtime.write32(cdrom_dma + 4U, 0x00010200U) &&
              runtime.write32(cdrom_dma + 8U, 0x11000000U) &&
              machine.dma().scheduledToken(sf::psx::DmaChannel::cdrom) != 0U,
          "Could not schedule CD-ROM DMA for execution preflight test");
  machine.setCdRomMedia(nullptr);
  machine.advanceTicks(512U);
  require(
      runtime.copyBytes(destination, rejected_result) &&
          rejected_result == rejected_destination &&
          machine.dma().madr(sf::psx::DmaChannel::cdrom) == destination &&
          (machine.dma().chcr(sf::psx::DmaChannel::cdrom) & 0x01000000U) ==
              0U &&
          machine.dma().scheduledToken(sf::psx::DmaChannel::cdrom) == 0U,
      "CD-ROM DMA execution preflight mutated RAM or left the channel busy");
  machine.setCdRomMedia(&media);
  require(runtime.restoreRam(ram_checkpoint) &&
              machine.restoreState(checkpoint),
          "Could not restore CD-ROM state after DMA preflight rejection tests");

  const auto atomic_dma_tick = machine.currentTick();
  const auto atomic_dma_mixed_frames = machine.spu().state().mixed_frames;
  require(runtime.write32(dpcr, machine.dma().dpcr() | 0x00008000U) &&
              runtime.write32(cdrom_dma, destination) &&
              runtime.write32(cdrom_dma + 4U, 0x00010200U) &&
              runtime.write32(cdrom_dma + 8U, 0x11000000U) &&
              machine.dma().scheduledToken(sf::psx::DmaChannel::cdrom) != 0U &&
              machine.completePendingDmaTransfers() &&
              machine.currentTick() == atomic_dma_tick &&
              machine.spu().state().mixed_frames == atomic_dma_mixed_frames &&
              (machine.dma().chcr(sf::psx::DmaChannel::cdrom) & 0x01000000U) ==
                  0U &&
              machine.dma().scheduledToken(sf::psx::DmaChannel::cdrom) == 0U,
          "Clock-neutral CD-ROM DMA did not complete without advancing SPU");
  require(runtime.restoreRam(ram_checkpoint) &&
              machine.restoreState(checkpoint),
          "Could not restore CD-ROM state after clock-neutral DMA test");

  const auto run_dma = [&]() {
    if (!runtime.write32(dpcr, machine.dma().dpcr() | 0x00008000U) ||
        !runtime.write32(cdrom_dma, destination) ||
        !runtime.write32(cdrom_dma + 4U, 0x00010200U) ||
        !runtime.write32(cdrom_dma + 8U, 0x11000000U)) {
      return false;
    }
    machine.advanceTicks(511U);
    if ((machine.dma().chcr(sf::psx::DmaChannel::cdrom) & 0x01000000U) == 0U) {
      return false;
    }
    machine.advanceTicks(1U);
    return (machine.dma().chcr(sf::psx::DmaChannel::cdrom) & 0x01000000U) ==
               0U &&
           machine.dma().madr(sf::psx::DmaChannel::cdrom) ==
               destination + sf::psx::CdRomMedia::sector_size;
  };

  std::vector<std::byte> first_transfer(sf::psx::CdRomMedia::sector_size);
  require(run_dma() && runtime.copyBytes(destination, first_transfer),
          "CD-ROM DMA3 did not complete its 2048-byte request");
  for (std::size_t index = 0U; index < first_transfer.size(); ++index) {
    const auto expected =
        static_cast<std::byte>((source_lba * 17U + index) & 0xffU);
    require(first_transfer[index] == expected,
            "CD-ROM DMA3 copied the wrong user-data byte");
  }

  require(runtime.write32(destination, 0xdeadbeefU),
          "Could not mutate RAM before CD-ROM snapshot restore");
  machine.advanceTicks(17U);
  require(runtime.restoreRam(ram_checkpoint) &&
              machine.restoreState(checkpoint),
          "Machine rejected a valid mid-read CD-ROM snapshot");
  std::vector<std::byte> replay_transfer(sf::psx::CdRomMedia::sector_size);
  require(run_dma() && runtime.copyBytes(destination, replay_transfer) &&
              replay_transfer == first_transfer && media.reads == 1U,
          "Restored CD-ROM FIFO/DMA state did not replay deterministically");
}

void testCop2LeadingCount() {
  sf::psx::R3000Runtime runtime;
  constexpr std::array words{
      encodeCop2Transfer(4U, 4U, 30U),
      encodeCop2Transfer(0U, 2U, 31U),
      0U,
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  loadCode(runtime, words);
  const std::array positive_argument{0x10U};
  auto result = runtime.call(code_address, positive_argument);
  require(result.reason == sf::psx::R3000StopReason::returned &&
              runtime.state().gpr[2] == 27U,
          "GTE LZCS/LZCR positive count mismatch");
  const std::array negative_argument{0xffffffffU};
  result = runtime.call(code_address, negative_argument);
  require(result.reason == sf::psx::R3000StopReason::returned &&
              runtime.state().gpr[2] == 32U,
          "GTE LZCS/LZCR negative count mismatch");
}

void testGteGameplayMath() {
  sf::psx::GteState state;
  sf::psx::GteRuntime::writeData(state, 9U, static_cast<std::uint32_t>(-3));
  sf::psx::GteRuntime::writeData(state, 10U, 200U);
  sf::psx::GteRuntime::writeData(state, 11U, 0xffff8000U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4aa00428U),
          "GTE SQR command was rejected");
  require(sf::psx::GteRuntime::readData(state, 25U) == 9U &&
              sf::psx::GteRuntime::readData(state, 26U) == 40'000U &&
              sf::psx::GteRuntime::readData(state, 27U) == 0x40000000U,
          "GTE SQR MAC result mismatch");
  require(sf::psx::GteRuntime::readData(state, 9U) == 9U &&
              sf::psx::GteRuntime::readData(state, 10U) == 0x7fffU &&
              sf::psx::GteRuntime::readData(state, 11U) == 0x7fffU &&
              (sf::psx::GteRuntime::readControl(state, 31U) & 0x80c00000U) ==
                  0x80c00000U,
          "GTE SQR saturation mismatch");

  sf::psx::GteRuntime::writeData(state, 6U, 0x5a000000U);
  sf::psx::GteRuntime::writeData(state, 8U, 0x1000U);
  sf::psx::GteRuntime::writeData(state, 9U, 2U);
  sf::psx::GteRuntime::writeData(state, 10U, 4U);
  sf::psx::GteRuntime::writeData(state, 11U, 8U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a90003dU),
          "GTE GPF command was rejected");
  require(sf::psx::GteRuntime::readData(state, 25U) == 0x2000U &&
              sf::psx::GteRuntime::readData(state, 26U) == 0x4000U &&
              sf::psx::GteRuntime::readData(state, 27U) == 0x8000U &&
              sf::psx::GteRuntime::readData(state, 22U) == 0x5affffffU,
          "GTE GPF result or RGB FIFO mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 6U, 0x5a302010U);
  sf::psx::GteRuntime::writeData(state, 8U, 0x0800U);
  sf::psx::GteRuntime::writeControl(state, 21U, 0x0500U);
  sf::psx::GteRuntime::writeControl(state, 22U, 0x0600U);
  sf::psx::GteRuntime::writeControl(state, 23U, 0x0700U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a780010U) &&
              sf::psx::GteRuntime::readData(state, 25U) == 0x0300U &&
              sf::psx::GteRuntime::readData(state, 26U) == 0x0400U &&
              sf::psx::GteRuntime::readData(state, 27U) == 0x0500U &&
              sf::psx::GteRuntime::readData(state, 22U) == 0x5a504030U,
          "GTE DPCS interpolation result mismatch");

  state = {};
  sf::psx::GteRuntime::writeControl(state, 8U, 0x1000U);
  sf::psx::GteRuntime::writeControl(state, 10U, 0x1000U);
  sf::psx::GteRuntime::writeControl(state, 12U, 0x1000U);
  sf::psx::GteRuntime::writeControl(state, 16U, 0x1000U);
  sf::psx::GteRuntime::writeControl(state, 18U, 0x1000U);
  sf::psx::GteRuntime::writeControl(state, 20U, 0x1000U);
  sf::psx::GteRuntime::writeData(state, 0U, 0x08001000U);
  sf::psx::GteRuntime::writeData(state, 1U, 0x0400U);
  sf::psx::GteRuntime::writeData(state, 6U, 0xaac08040U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4ae80413U) &&
              sf::psx::GteRuntime::readData(state, 25U) == 0x400U &&
              sf::psx::GteRuntime::readData(state, 26U) == 0x400U &&
              sf::psx::GteRuntime::readData(state, 27U) == 0x300U &&
              sf::psx::GteRuntime::readData(state, 22U) == 0xaa304040U,
          "GTE NCDS lighting/depth-cue result mismatch");
  sf::psx::GteRuntime::writeData(state, 2U, 0x04000800U);
  sf::psx::GteRuntime::writeData(state, 3U, 0x0200U);
  sf::psx::GteRuntime::writeData(state, 4U, 0x02000400U);
  sf::psx::GteRuntime::writeData(state, 5U, 0x0100U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4af80416U) &&
              sf::psx::GteRuntime::readData(state, 20U) == 0xaa304040U &&
              sf::psx::GteRuntime::readData(state, 21U) == 0xaa182020U &&
              sf::psx::GteRuntime::readData(state, 22U) == 0xaa0c1010U,
          "GTE NCDT triple lighting/depth-cue FIFO mismatch");

  sf::psx::GteRuntime::writeData(state, 12U, 1U);
  sf::psx::GteRuntime::writeData(state, 13U, 2U);
  sf::psx::GteRuntime::writeData(state, 14U, 3U);
  sf::psx::GteRuntime::writeData(state, 15U, 4U);
  require(sf::psx::GteRuntime::readData(state, 12U) == 2U &&
              sf::psx::GteRuntime::readData(state, 13U) == 3U &&
              sf::psx::GteRuntime::readData(state, 14U) == 4U &&
              sf::psx::GteRuntime::readData(state, 15U) == 4U,
          "GTE SXY FIFO register alias mismatch");

  state = {};
  sf::psx::GteRuntime::writeControl(state, 0U, 1U);
  sf::psx::GteRuntime::writeControl(state, 1U, 0U);
  sf::psx::GteRuntime::writeControl(state, 2U, 1U);
  sf::psx::GteRuntime::writeControl(state, 3U, 0U);
  sf::psx::GteRuntime::writeControl(state, 4U, 1U);
  sf::psx::GteRuntime::writeData(state, 9U, 2U);
  sf::psx::GteRuntime::writeData(state, 10U, 3U);
  sf::psx::GteRuntime::writeData(state, 11U, 4U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a41e012U) &&
              sf::psx::GteRuntime::readData(state, 25U) == 2U &&
              sf::psx::GteRuntime::readData(state, 26U) == 3U &&
              sf::psx::GteRuntime::readData(state, 27U) == 4U,
          "GTE MVMVA identity result mismatch");

  sf::psx::GteRuntime::writeData(state, 9U, 1U);
  sf::psx::GteRuntime::writeData(state, 10U, 2U);
  sf::psx::GteRuntime::writeData(state, 11U, 4U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a00000cU) &&
              sf::psx::GteRuntime::readData(state, 25U) == 2U &&
              sf::psx::GteRuntime::readData(state, 26U) == 0xfffffffdU &&
              sf::psx::GteRuntime::readData(state, 27U) == 1U,
          "GTE OP result mismatch");

  state = {};
  sf::psx::GteRuntime::writeControl(state, 0U, 0x1000U);
  sf::psx::GteRuntime::writeControl(state, 1U, 0U);
  sf::psx::GteRuntime::writeControl(state, 2U, 0x1000U);
  sf::psx::GteRuntime::writeControl(state, 3U, 0U);
  sf::psx::GteRuntime::writeControl(state, 4U, 0x1000U);
  sf::psx::GteRuntime::writeControl(state, 26U, 100U);
  sf::psx::GteRuntime::writeData(
      state, 0U,
      100U |
          (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-50)) << 16U));
  sf::psx::GteRuntime::writeData(state, 1U, 1000U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a180001U) &&
              sf::psx::GteRuntime::readData(state, 25U) == 100U &&
              sf::psx::GteRuntime::readData(state, 26U) == 0xffffffceU &&
              sf::psx::GteRuntime::readData(state, 27U) == 1000U &&
              sf::psx::GteRuntime::readData(state, 19U) == 1000U &&
              sf::psx::GteRuntime::readData(state, 14U) == 0xfffa000aU &&
              sf::psx::GteRuntime::readData(state, 24U) == 0U &&
              sf::psx::GteRuntime::readData(state, 8U) == 0U &&
              sf::psx::GteRuntime::readControl(state, 31U) == 0U,
          "GTE RTPS result mismatch");

  sf::psx::GteRuntime::writeData(state, 12U, 0U);
  sf::psx::GteRuntime::writeData(state, 13U, 1U);
  sf::psx::GteRuntime::writeData(state, 14U, 1U << 16U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4b400006U) &&
              sf::psx::GteRuntime::readData(state, 24U) == 1U,
          "GTE NCLIP result mismatch");

  sf::psx::R3000Runtime runtime;
  constexpr std::array transfer_words{
      encodeI(0x0fU, 0U, 8U, 0x8001U), encodeI(0x32U, 8U, 9U, 0x0200U), 0U,
      encodeI(0x3aU, 8U, 9U, 0x0204U), encodeR(31U, 0U, 0U, 0U, 0x08U), 0U,
  };
  loadCode(runtime, transfer_words);
  require(runtime.write32(0x80010200U, 0xffff8001U),
          "Could not seed LWC2 data");
  const auto transfer_result = runtime.call(code_address);
  std::uint32_t transferred{};
  require(transfer_result.reason == sf::psx::R3000StopReason::returned &&
              runtime.read32(0x80010204U, transferred) &&
              transferred == 0xffff8001U,
          "LWC2/SWC2 transfer mismatch");
}

void testGteRetailColorAndDepthCommands() {
  constexpr std::uint32_t all_color_saturation_flags = 0x81f80000U;
  sf::psx::GteState state;

  sf::psx::GteRuntime::writeData(state, 6U, 0x7a000000U);
  sf::psx::GteRuntime::writeData(state, 8U, 0x0800U);
  sf::psx::GteRuntime::writeData(state, 9U, 0x0100U);
  sf::psx::GteRuntime::writeData(state, 10U, 0x0200U);
  sf::psx::GteRuntime::writeData(state, 11U, 0x0300U);
  sf::psx::GteRuntime::writeControl(state, 21U, 0x0500U);
  sf::psx::GteRuntime::writeControl(state, 22U, 0x0600U);
  sf::psx::GteRuntime::writeControl(state, 23U, 0x0700U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a080011U) &&
              sf::psx::GteRuntime::readData(state, 25U) == 0x0300U &&
              sf::psx::GteRuntime::readData(state, 26U) == 0x0400U &&
              sf::psx::GteRuntime::readData(state, 27U) == 0x0500U &&
              sf::psx::GteRuntime::readData(state, 9U) == 0x0300U &&
              sf::psx::GteRuntime::readData(state, 10U) == 0x0400U &&
              sf::psx::GteRuntime::readData(state, 11U) == 0x0500U &&
              sf::psx::GteRuntime::readData(state, 22U) == 0x7a504030U &&
              sf::psx::GteRuntime::readControl(state, 31U) == 0U,
          "GTE INTPL interpolation result mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 6U, 0x7b000000U);
  sf::psx::GteRuntime::writeData(state, 8U, 0U);
  sf::psx::GteRuntime::writeData(state, 9U, 0xffffffffU);
  sf::psx::GteRuntime::writeData(state, 10U, 0xfffffffeU);
  sf::psx::GteRuntime::writeData(state, 11U, 0xfffffffdU);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a080411U) &&
              sf::psx::GteRuntime::readData(state, 25U) == 0xffffffffU &&
              sf::psx::GteRuntime::readData(state, 26U) == 0xfffffffeU &&
              sf::psx::GteRuntime::readData(state, 27U) == 0xfffffffdU &&
              sf::psx::GteRuntime::readData(state, 9U) == 0U &&
              sf::psx::GteRuntime::readData(state, 10U) == 0U &&
              sf::psx::GteRuntime::readData(state, 11U) == 0U &&
              sf::psx::GteRuntime::readData(state, 22U) == 0x7b000000U &&
              sf::psx::GteRuntime::readControl(state, 31U) ==
                  all_color_saturation_flags,
          "GTE INTPL LM/color saturation flags mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 6U, 0x6c302010U);
  sf::psx::GteRuntime::writeData(state, 8U, 0x0800U);
  sf::psx::GteRuntime::writeData(state, 9U, 0x1000U);
  sf::psx::GteRuntime::writeData(state, 10U, 0x0800U);
  sf::psx::GteRuntime::writeData(state, 11U, 0x0400U);
  sf::psx::GteRuntime::writeControl(state, 21U, 0x0400U);
  sf::psx::GteRuntime::writeControl(state, 22U, 0x0500U);
  sf::psx::GteRuntime::writeControl(state, 23U, 0x0600U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a080029U) &&
              sf::psx::GteRuntime::readData(state, 25U) == 0x0280U &&
              sf::psx::GteRuntime::readData(state, 26U) == 0x0300U &&
              sf::psx::GteRuntime::readData(state, 27U) == 0x0360U &&
              sf::psx::GteRuntime::readData(state, 22U) == 0x6c363028U &&
              sf::psx::GteRuntime::readControl(state, 31U) == 0U,
          "GTE DCPL interpolation result mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 6U, 0x66030201U);
  sf::psx::GteRuntime::writeData(state, 8U, 0U);
  sf::psx::GteRuntime::writeData(state, 9U, 0xffffffffU);
  sf::psx::GteRuntime::writeData(state, 10U, 0xffffffffU);
  sf::psx::GteRuntime::writeData(state, 11U, 0xffffffffU);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a000429U) &&
              sf::psx::GteRuntime::readData(state, 25U) == 0xfffffff0U &&
              sf::psx::GteRuntime::readData(state, 26U) == 0xffffffe0U &&
              sf::psx::GteRuntime::readData(state, 27U) == 0xffffffd0U &&
              sf::psx::GteRuntime::readData(state, 9U) == 0U &&
              sf::psx::GteRuntime::readData(state, 10U) == 0U &&
              sf::psx::GteRuntime::readData(state, 11U) == 0U &&
              sf::psx::GteRuntime::readData(state, 22U) == 0x66000000U &&
              sf::psx::GteRuntime::readControl(state, 31U) ==
                  all_color_saturation_flags,
          "GTE DCPL LM/color saturation flags mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 6U, 0x55000000U);
  sf::psx::GteRuntime::writeData(state, 8U, 0x0800U);
  sf::psx::GteRuntime::writeData(state, 20U, 0xaa0c0804U);
  sf::psx::GteRuntime::writeData(state, 21U, 0xbb181410U);
  sf::psx::GteRuntime::writeData(state, 22U, 0xcc24201cU);
  sf::psx::GteRuntime::writeControl(state, 21U, 0x0400U);
  sf::psx::GteRuntime::writeControl(state, 22U, 0x0500U);
  sf::psx::GteRuntime::writeControl(state, 23U, 0x0600U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a08002aU) &&
              sf::psx::GteRuntime::readData(state, 20U) == 0x55362c22U &&
              sf::psx::GteRuntime::readData(state, 21U) == 0x553c3228U &&
              sf::psx::GteRuntime::readData(state, 22U) == 0x5542382eU &&
              sf::psx::GteRuntime::readData(state, 25U) == 0x02e0U &&
              sf::psx::GteRuntime::readData(state, 26U) == 0x0380U &&
              sf::psx::GteRuntime::readData(state, 27U) == 0x0420U &&
              sf::psx::GteRuntime::readControl(state, 31U) == 0U,
          "GTE DPCT RGB FIFO order/result mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 6U, 0x5d000000U);
  sf::psx::GteRuntime::writeData(state, 8U, 0x1000U);
  sf::psx::GteRuntime::writeData(state, 20U, 0x00ffffffU);
  sf::psx::GteRuntime::writeData(state, 21U, 0x00ffffffU);
  sf::psx::GteRuntime::writeData(state, 22U, 0x00ffffffU);
  sf::psx::GteRuntime::writeControl(state, 21U, 0x7fffffffU);
  sf::psx::GteRuntime::writeControl(state, 22U, 0x7fffffffU);
  sf::psx::GteRuntime::writeControl(state, 23U, 0x7fffffffU);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a08042aU) &&
              sf::psx::GteRuntime::readData(state, 20U) == 0x5dffffffU &&
              sf::psx::GteRuntime::readData(state, 21U) == 0x5dffffffU &&
              sf::psx::GteRuntime::readData(state, 22U) == 0x5dffffffU &&
              sf::psx::GteRuntime::readControl(state, 31U) ==
                  all_color_saturation_flags,
          "GTE DPCT IR/RGB saturation flags mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 17U, 100U);
  sf::psx::GteRuntime::writeData(state, 18U, 200U);
  sf::psx::GteRuntime::writeData(state, 19U, 300U);
  sf::psx::GteRuntime::writeControl(state, 29U, 0x1000U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a00002dU) &&
              sf::psx::GteRuntime::readData(state, 24U) == 0x00258000U &&
              sf::psx::GteRuntime::readData(state, 7U) == 600U &&
              sf::psx::GteRuntime::readControl(state, 31U) == 0U,
          "GTE AVSZ3 average depth result mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 17U, 0xffffU);
  sf::psx::GteRuntime::writeData(state, 18U, 0xffffU);
  sf::psx::GteRuntime::writeData(state, 19U, 0xffffU);
  sf::psx::GteRuntime::writeControl(state, 29U, 0x7fffU);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a00002dU) &&
              sf::psx::GteRuntime::readData(state, 24U) == 0x7ffb8003U &&
              sf::psx::GteRuntime::readData(state, 7U) == 0xffffU &&
              sf::psx::GteRuntime::readControl(state, 31U) == 0x80050000U,
          "GTE AVSZ3 MAC0/OTZ positive saturation flags mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 16U, 100U);
  sf::psx::GteRuntime::writeData(state, 17U, 200U);
  sf::psx::GteRuntime::writeData(state, 18U, 300U);
  sf::psx::GteRuntime::writeData(state, 19U, 400U);
  sf::psx::GteRuntime::writeControl(state, 30U, 0x0400U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a00002eU) &&
              sf::psx::GteRuntime::readData(state, 24U) == 0x000fa000U &&
              sf::psx::GteRuntime::readData(state, 7U) == 250U &&
              sf::psx::GteRuntime::readControl(state, 31U) == 0U,
          "GTE AVSZ4 average depth result mismatch");

  state = {};
  sf::psx::GteRuntime::writeData(state, 16U, 0xffffU);
  sf::psx::GteRuntime::writeData(state, 17U, 0xffffU);
  sf::psx::GteRuntime::writeData(state, 18U, 0xffffU);
  sf::psx::GteRuntime::writeData(state, 19U, 0xffffU);
  sf::psx::GteRuntime::writeControl(state, 30U, 0xffff8000U);
  require(sf::psx::GteRuntime::executeCommand(state, 0x4a00002eU) &&
              sf::psx::GteRuntime::readData(state, 24U) == 0x00020000U &&
              sf::psx::GteRuntime::readData(state, 7U) == 0U &&
              sf::psx::GteRuntime::readControl(state, 31U) == 0x80048000U,
          "GTE AVSZ4 MAC0/OTZ negative saturation flags mismatch");
}

void testMemoryMap() {
  sf::psx::R3000Runtime runtime;
  require(runtime.write32(0x80000100U, 0x89abcdefU), "KSEG0 RAM write failed");
  std::uint32_t value{};
  require(runtime.read32(0x00000100U, value) && value == 0x89abcdefU,
          "KUSEG RAM alias mismatch");
  require(runtime.read32(0xa0000100U, value) && value == 0x89abcdefU,
          "KSEG1 RAM alias mismatch");
  require(runtime.read32(0x80200100U, value) && value == 0x89abcdefU,
          "Two-megabyte RAM mirror mismatch");
  require(runtime.write32(0x9f800000U, 0x10203040U) &&
              runtime.read32(0x1f800000U, value) && value == 0x10203040U,
          "Scratchpad alias mismatch");
  require(!runtime.read32(0xbf800000U, value) &&
              !runtime.write32(0xbf800000U, 0U),
          "Uncached KSEG1 incorrectly exposed the cached-only scratchpad");
  require(runtime.write32(0x1f8010f0U, 0x12345678U) &&
              runtime.read32(0x9f8010f0U, value) && value == 0x12345678U &&
              runtime.read32(0xbf8010f0U, value) && value == 0x12345678U,
          "MMIO shadow alias mismatch");
  std::uint8_t mmio_byte{};
  require(runtime.read8(0x1f8010f1U, mmio_byte) && mmio_byte == 0x56U,
          "MMIO shadow is not byte-addressable little-endian memory");
  std::uint16_t unaligned_value{};
  require(!runtime.write16(0x1f8010f1U, 0xabcdU) &&
              !runtime.read16(0x1f8010f1U, unaligned_value) &&
              !runtime.write32(0x1f8010f2U, 0xabcdef01U) &&
              runtime.read32(0x1f8010f0U, value) && value == 0x12345678U,
          "MMIO shadow accepted an unaligned access");

  require(runtime.write8(0x1f8003ffU, 0x5aU), "Scratchpad tail seed failed");
  constexpr std::array invalid_write{std::byte{0x11}, std::byte{0x22}};
  require(!runtime.loadBytes(0x1f8003ffU, invalid_write),
          "Cross-boundary block write unexpectedly succeeded");
  std::uint8_t tail{};
  require(runtime.read8(0x1f8003ffU, tail) && tail == 0x5aU,
          "Rejected block write modified memory partially");
  std::array invalid_read{std::byte{0x33}, std::byte{0x44}};
  require(!runtime.copyBytes(0x1f8003ffU, invalid_read) &&
              invalid_read[0] == std::byte{0x33} &&
              invalid_read[1] == std::byte{0x44},
          "Rejected block read modified its destination partially");

  const std::vector<std::byte> ram_snapshot(runtime.ram().begin(),
                                            runtime.ram().end());
  const std::vector<std::byte> scratchpad_snapshot(runtime.scratchpad().begin(),
                                                   runtime.scratchpad().end());
  const std::vector<std::byte> mmio_snapshot(runtime.mmio().begin(),
                                             runtime.mmio().end());
  runtime.reset(code_address, 0U, 0U);
  require(runtime.read32(0x1f8010f0U, value) && value == 0x12345678U,
          "CPU reset unexpectedly cleared the MMIO shadow");
  runtime.clearMemory();
  require(runtime.read32(0x1f8010f0U, value) && value == 0U,
          "Memory clear did not clear the MMIO shadow");
  require(runtime.restoreRam(ram_snapshot) &&
              runtime.restoreScratchpad(scratchpad_snapshot) &&
              runtime.restoreMmio(mmio_snapshot),
          "Memory bridge could not restore a complete snapshot");
  require(runtime.read32(0x80000100U, value) && value == 0x89abcdefU &&
              runtime.read32(0x1f800000U, value) && value == 0x10203040U &&
              runtime.read32(0x1f8010f0U, value) && value == 0x12345678U,
          "Memory bridge restored different contents");
  require(!runtime.restoreRam(
              std::span{ram_snapshot}.first(ram_snapshot.size() - 1U)),
          "RAM bridge accepted a truncated snapshot");
  require(!runtime.restoreMmio(
              std::span{mmio_snapshot}.first(mmio_snapshot.size() - 1U)),
          "MMIO bridge accepted a truncated snapshot");

  sf::psx::R3000State cpu_snapshot{};
  cpu_snapshot.gpr[0] = 99U;
  cpu_snapshot.gpr[2] = 42U;
  cpu_snapshot.cop0_status = 0x40000000U;
  cpu_snapshot.pc = code_address;
  cpu_snapshot.next_pc = code_address + 4U;
  runtime.restoreCpuState(cpu_snapshot);
  require(runtime.state().gpr[0] == 0U && runtime.state().gpr[2] == 42U &&
              runtime.state().cop0_status == 0x40000000U,
          "CPU snapshot restore did not preserve the zero-register invariant");
}

void testDeterministicStops() {
  sf::psx::R3000Runtime runtime;
  constexpr std::array unsupported_words{0xffffffffU};
  loadCode(runtime, unsupported_words);
  auto result = runtime.call(code_address);
  require(result.reason == sf::psx::R3000StopReason::unsupported_instruction &&
              result.pc == code_address,
          "Unsupported instruction did not stop deterministically");

  constexpr std::array loop_words{encodeJ(0x02U, code_address), 0U};
  loadCode(runtime, loop_words);
  result = runtime.call(code_address, {}, 6U);
  require(result.reason == sf::psx::R3000StopReason::instruction_budget &&
              result.instructions == 6U,
          "Instruction budget did not stop an infinite loop");

  constexpr std::array exact_budget_return_words{
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  loadCode(runtime, exact_budget_return_words);
  result = runtime.call(code_address, {}, exact_budget_return_words.size());
  require(
      result.reason == sf::psx::R3000StopReason::returned &&
          result.instructions == exact_budget_return_words.size(),
      "Return sentinel reached at the instruction-budget boundary was missed");

  constexpr std::array unaligned_words{
      encodeI(0x23U, 0U, 2U, 1U),
  };
  loadCode(runtime, unaligned_words);
  result = runtime.call(code_address);
  require(result.reason == sf::psx::R3000StopReason::alignment_fault,
          "Unaligned LW did not report an alignment fault");

  constexpr std::array stack_argument_words{
      encodeI(0x23U, 29U, 2U, 16U),
      0U,
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  loadCode(runtime, stack_argument_words);
  runtime.reset(code_address, 0U, 0x80011000U);
  const std::array stack_arguments{1U, 2U, 3U, 4U, 0x55667788U};
  result = runtime.call(code_address, stack_arguments);
  require(result.reason == sf::psx::R3000StopReason::returned &&
              runtime.state().gpr[2] == 0x55667788U,
          "O32 stack argument bridge mismatch");

  runtime.reset(code_address, 0U, 0x801ffff0U);
  result = runtime.call(code_address, stack_arguments);
  require(result.reason == sf::psx::R3000StopReason::memory_fault,
          "O32 bridge accepted stack arguments across the RAM mirror boundary");

  runtime.reset(code_address, 0U, 0x80000000U);
  const auto maximum_stack_arguments =
      4U + (sf::psx::R3000Runtime::ram_size - 16U) / sizeof(std::uint32_t);
  const std::vector<std::uint32_t> oversized_arguments(maximum_stack_arguments +
                                                       1U);
  require(
      !runtime.beginCall(code_address, oversized_arguments),
      "O32 bridge accepted more stack arguments than emulated RAM can contain");
}

void testLegacyVirtualCd() {
  sf::game::LegacyVirtualCd cd;
  std::vector<std::byte> root_file(2050U);
  for (std::size_t index = 0U; index < root_file.size(); ++index) {
    root_file[index] = static_cast<std::byte>(index & 0xffU);
  }
  const std::vector<std::byte> archive_file(
      sf::game::LegacyVirtualCd::sector_size, std::byte{0x42});
  const std::vector<std::byte> root_override(
      sf::game::LegacyVirtualCd::sector_size, std::byte{0x24});
  const std::vector<std::byte> empty_file;
  require(
      cd.addRootFile("BIN/INIT.OVL", root_file) &&
          cd.addRootFile("SUBWAY.OVL", root_override) &&
          cd.addRootFile("EMPTY.OVL", empty_file) &&
          cd.addArchiveFile("FOG/SUBWAY.FOG", "SUBWAY.OVL", archive_file, 7U) &&
          cd.addArchiveFile("FOG/SUBWAY.FOG", "AUTO.OVL", root_override),
      "Virtual CD catalog setup failed");
  require(
      !cd.addArchiveFile("FOG/SUBWAY.FOG", "COLLIDE.OVL", archive_file, 7U) &&
          !cd.addArchiveFile("FOG/SUBWAY.FOG", "OVERFLOW.OVL", archive_file,
                             std::numeric_limits<std::uint32_t>::max()),
      "Virtual CD accepted an invalid archive sector extent");

  const auto root_location = cd.locate("\\BIN\\INIT.OVL;1");
  const auto root_override_location = cd.locate("SUBWAY.OVL");
  require(root_location &&
              root_location->sector ==
                  sf::game::LegacyVirtualCd::root_sector_base &&
              root_location->size == root_file.size() &&
              root_override_location &&
              root_override_location->sector ==
                  sf::game::LegacyVirtualCd::root_sector_base + 2U &&
              root_override_location->size == root_override.size() &&
              !cd.locate("AUTO.OVL") && !cd.locate("MISSING.OVL"),
          "Virtual CD root location mismatch");

  std::uint32_t handle{0xdeadbeefU};
  std::uint32_t size{0xdeadbeefU};
  std::vector<std::byte> destination(sf::game::LegacyVirtualCd::sector_size,
                                     std::byte{0x7e});
  require(
      cd.open("MISSING.OVL", handle) == sf::game::LegacyCdResult::not_found &&
          handle == 0U &&
          cd.open("EMPTY.OVL", handle) ==
              sf::game::LegacyCdResult::empty_file &&
          handle == 0U &&
          cd.paddedSize(0xdeadbeefU, size) ==
              sf::game::LegacyCdResult::invalid_argument &&
          size == 0U &&
          cd.read(0xdeadbeefU, destination).result ==
              sf::game::LegacyCdResult::invalid_argument &&
          cd.rewind(0xdeadbeefU) ==
              sf::game::LegacyCdResult::invalid_argument &&
          cd.close(0xdeadbeefU) == sf::game::LegacyCdResult::invalid_argument,
      "Virtual CD failure results mismatch");

  require(cd.mount("\\FOG\\subway.fog;1") &&
              cd.mountedArchive() == "FOG/SUBWAY.FOG",
          "Virtual CD mount normalization mismatch");

  const auto archive_location = cd.locate("ANY/PATH/SUBWAY.OVL");
  const auto automatic_location = cd.locate("AUTO.OVL");
  const auto mounted_root_location = cd.locate("BIN/INIT.OVL");
  require(archive_location &&
              archive_location->sector ==
                  sf::game::LegacyVirtualCd::archive_sector_base + 7U &&
              archive_location->size == archive_file.size() &&
              automatic_location &&
              automatic_location->sector ==
                  sf::game::LegacyVirtualCd::archive_sector_base + 8U &&
              automatic_location->size == root_override.size() &&
              mounted_root_location &&
              mounted_root_location->sector == root_location->sector,
          "Virtual CD mounted archive location mismatch");

  std::vector<std::byte> raw_sector(sf::game::LegacyVirtualCd::sector_size,
                                    std::byte{0x7e});
  auto raw_read = cd.readSectors(archive_location->sector, raw_sector);
  require(raw_read.result == sf::game::LegacyCdResult::success &&
              raw_read.bytes_read == raw_sector.size() &&
              raw_read.transfer_size == raw_sector.size() &&
              std::ranges::all_of(
                  raw_sector,
                  [](std::byte value) { return value == std::byte{0x42}; }),
          "Virtual CD integer-LBA archive read mismatch");

  std::vector<std::byte> raw_archive_crossing(
      2U * sf::game::LegacyVirtualCd::sector_size, std::byte{0x7e});
  raw_read = cd.readSectors(archive_location->sector, raw_archive_crossing);
  require(raw_read.result == sf::game::LegacyCdResult::success &&
              raw_read.bytes_read == raw_archive_crossing.size() &&
              raw_read.transfer_size == raw_archive_crossing.size() &&
              std::ranges::all_of(
                  raw_archive_crossing.begin(),
                  raw_archive_crossing.begin() +
                      sf::game::LegacyVirtualCd::sector_size,
                  [](std::byte value) { return value == std::byte{0x42}; }) &&
              std::ranges::all_of(
                  raw_archive_crossing.begin() +
                      sf::game::LegacyVirtualCd::sector_size,
                  raw_archive_crossing.end(),
                  [](std::byte value) { return value == std::byte{0x24}; }),
          "Virtual CD raw read did not cross contiguous archive members");

  raw_sector.assign(sf::game::LegacyVirtualCd::sector_size, std::byte{0x7e});
  raw_read = cd.readSectors(root_location->sector + 1U, raw_sector);
  require(
      raw_read.result == sf::game::LegacyCdResult::success &&
          raw_read.bytes_read == 2U &&
          raw_read.transfer_size == raw_sector.size() &&
          std::equal(root_file.begin() + sf::game::LegacyVirtualCd::sector_size,
                     root_file.end(), raw_sector.begin()) &&
          std::all_of(raw_sector.begin() + 2U, raw_sector.end(),
                      [](std::byte value) {
                        return value ==
                               sf::game::LegacyVirtualCd::sector_padding;
                      }),
      "Virtual CD raw tail padding mismatch");

  std::vector<std::byte> raw_out_of_bounds(
      2U * sf::game::LegacyVirtualCd::sector_size, std::byte{0x7e});
  raw_read = cd.readSectors(root_location->sector + 1U, raw_out_of_bounds);
  require(raw_read.result == sf::game::LegacyCdResult::invalid_argument &&
              raw_read.bytes_read == 0U && raw_read.transfer_size == 0U &&
              std::ranges::all_of(
                  raw_out_of_bounds,
                  [](std::byte value) { return value == std::byte{0x7e}; }),
          "Virtual CD raw read crossed an extent boundary");

  std::vector<std::byte> raw_unaligned(
      sf::game::LegacyVirtualCd::sector_size + 1U, std::byte{0x7e});
  raw_read = cd.readSectors(archive_location->sector, raw_unaligned);
  require(raw_read.result == sf::game::LegacyCdResult::invalid_argument &&
              std::ranges::all_of(
                  raw_unaligned,
                  [](std::byte value) { return value == std::byte{0x7e}; }),
          "Virtual CD accepted an unaligned raw read");

  require(cd.open("\\BIN\\INIT.OVL;1", handle) ==
              sf::game::LegacyCdResult::success,
          "Virtual CD root fallback failed");
  require(cd.paddedSize(handle, size) == sf::game::LegacyCdResult::success &&
              size == 2U * sf::game::LegacyVirtualCd::sector_size,
          "Virtual CD padded size mismatch");
  destination.assign(size, std::byte{0x7e});
  auto read = cd.read(handle, destination);
  require(
      read.result == sf::game::LegacyCdResult::success &&
          read.bytes_read == root_file.size() && read.transfer_size == size &&
          std::equal(root_file.begin(), root_file.end(), destination.begin()) &&
          destination[root_file.size()] ==
              sf::game::LegacyVirtualCd::sector_padding &&
          destination.back() == sf::game::LegacyVirtualCd::sector_padding,
      "Virtual CD sector transfer mismatch");
  read = cd.read(handle, destination);
  require(read.result == sf::game::LegacyCdResult::success &&
              read.bytes_read == 0U && read.transfer_size == 0U,
          "Virtual CD EOF mismatch");
  require(cd.rewind(handle) == sf::game::LegacyCdResult::success,
          "Virtual CD rewind failed");

  std::vector<std::byte> non_sector_request(3000U, std::byte{0x7e});
  read = cd.read(handle, non_sector_request);
  require(
      read.result == sf::game::LegacyCdResult::success &&
          read.bytes_read == sf::game::LegacyVirtualCd::sector_size &&
          read.transfer_size == sf::game::LegacyVirtualCd::sector_size &&
          std::equal(root_file.begin(),
                     root_file.begin() + sf::game::LegacyVirtualCd::sector_size,
                     non_sector_request.begin()) &&
          std::all_of(non_sector_request.begin() +
                          sf::game::LegacyVirtualCd::sector_size,
                      non_sector_request.end(),
                      [](std::byte value) { return value == std::byte{0x7e}; }),
      "Virtual CD non-sector request was not rounded down");

  std::vector<std::byte> one_sector(sf::game::LegacyVirtualCd::sector_size,
                                    std::byte{0x7e});
  read = cd.read(handle, one_sector);
  require(
      read.result == sf::game::LegacyCdResult::success &&
          read.bytes_read == 2U &&
          read.transfer_size == sf::game::LegacyVirtualCd::sector_size &&
          std::equal(root_file.begin() + sf::game::LegacyVirtualCd::sector_size,
                     root_file.end(), one_sector.begin()) &&
          std::all_of(one_sector.begin() + 2U, one_sector.end(),
                      [](std::byte value) {
                        return value ==
                               sf::game::LegacyVirtualCd::sector_padding;
                      }),
      "Virtual CD tail transfer mismatch");

  require(cd.rewind(handle) == sf::game::LegacyCdResult::success,
          "Virtual CD second rewind failed");
  std::vector<std::byte> sub_sector_request(
      sf::game::LegacyVirtualCd::sector_size - 1U, std::byte{0x7e});
  read = cd.read(handle, sub_sector_request);
  require(read.result == sf::game::LegacyCdResult::success &&
              read.bytes_read == 0U && read.transfer_size == 0U &&
              std::ranges::all_of(
                  sub_sector_request,
                  [](std::byte value) { return value == std::byte{0x7e}; }),
          "Virtual CD sub-sector request mismatch");
  read = cd.read(handle, one_sector);
  require(
      read.bytes_read == one_sector.size() &&
          std::equal(one_sector.begin(), one_sector.end(), root_file.begin()),
      "Virtual CD zero-sector request advanced the cursor");
  require(cd.close(handle) == sf::game::LegacyCdResult::success &&
              cd.close(handle) == sf::game::LegacyCdResult::invalid_argument,
          "Virtual CD close mismatch");

  require(cd.open("ANY/PATH/SUBWAY.OVL", handle) ==
              sf::game::LegacyCdResult::success,
          "Virtual CD mounted basename lookup failed");
  destination.resize(sf::game::LegacyVirtualCd::sector_size);
  read = cd.read(handle, destination);
  require(read.bytes_read == destination.size() &&
              std::ranges::all_of(
                  destination,
                  [](std::byte value) { return value == std::byte{0x42}; }),
          "Virtual CD mounted file contents mismatch");
  require(cd.close(handle) == sf::game::LegacyCdResult::success,
          "Virtual CD mounted handle close failed");

  require(!cd.mount("FOG/MISSING.FOG") && !cd.mountedArchive(),
          "Virtual CD failed mount retained the previous archive");
  raw_sector.assign(sf::game::LegacyVirtualCd::sector_size, std::byte{0x7e});
  raw_read = cd.readSectors(archive_location->sector, raw_sector);
  require(raw_read.result == sf::game::LegacyCdResult::invalid_argument &&
              std::ranges::all_of(
                  raw_sector,
                  [](std::byte value) { return value == std::byte{0x7e}; }),
          "Virtual CD exposed an unmounted archive extent");
  require(cd.open("SUBWAY.OVL", handle) == sf::game::LegacyCdResult::success,
          "Virtual CD root lookup after failed mount failed");
  destination.assign(sf::game::LegacyVirtualCd::sector_size, std::byte{0x7e});
  read = cd.read(handle, destination);
  require(read.bytes_read == destination.size() &&
              std::ranges::all_of(
                  destination,
                  [](std::byte value) { return value == std::byte{0x24}; }) &&
              cd.close(handle) == sf::game::LegacyCdResult::success,
          "Virtual CD archive/root precedence mismatch");

  require(cd.mount("FOG/SUBWAY.FOG"), "Virtual CD remount failed");
  cd.unmount();
  require(!cd.mountedArchive() && cd.open("ANY/PATH/SUBWAY.OVL", handle) ==
                                      sf::game::LegacyCdResult::not_found,
          "Virtual CD unmount mismatch");

  std::array<std::uint32_t, sf::game::LegacyVirtualCd::maximum_open_files>
      handles{};
  for (auto &open_handle : handles) {
    require(cd.open("BIN/INIT.OVL", open_handle) ==
                sf::game::LegacyCdResult::success,
            "Virtual CD handle allocation failed");
  }
  require(cd.open("BIN/INIT.OVL", handle) ==
              sf::game::LegacyCdResult::no_free_handle,
          "Virtual CD accepted too many handles");
  for (const auto open_handle : handles) {
    require(cd.close(open_handle) == sf::game::LegacyCdResult::success,
            "Virtual CD handle cleanup failed");
  }

  sf::game::LegacyVirtualCd checkpoint_cd;
  require(checkpoint_cd.addRootFile("BIN/INIT.OVL", root_file) &&
              checkpoint_cd.addArchiveFile("FOG/SUBWAY.FOG", "SUBWAY.OVL",
                                           archive_file, 7U) &&
              checkpoint_cd.mount("FOG/SUBWAY.FOG"),
          "Virtual CD checkpoint catalog setup failed");
  std::uint32_t checkpoint_root_handle{};
  std::uint32_t checkpoint_archive_handle{};
  require(checkpoint_cd.open("BIN/INIT.OVL", checkpoint_root_handle) ==
                  sf::game::LegacyCdResult::success &&
              checkpoint_cd.open("SUBWAY.OVL", checkpoint_archive_handle) ==
                  sf::game::LegacyCdResult::success,
          "Virtual CD checkpoint handle setup failed");
  std::vector<std::byte> checkpoint_buffer(
      sf::game::LegacyVirtualCd::sector_size, std::byte{0x7e});
  read = checkpoint_cd.read(checkpoint_root_handle, checkpoint_buffer);
  require(read.result == sf::game::LegacyCdResult::success &&
              read.bytes_read == checkpoint_buffer.size(),
          "Virtual CD checkpoint cursor seed failed");
  constexpr auto checkpoint_raw_sector =
      sf::game::LegacyVirtualCd::archive_sector_base + 7U;
  checkpoint_cd.setCurrentRawSector(checkpoint_raw_sector);
  const auto cd_snapshot = checkpoint_cd.captureSnapshot();

  static_cast<void>(
      checkpoint_cd.read(checkpoint_root_handle, checkpoint_buffer));
  static_cast<void>(
      checkpoint_cd.read(checkpoint_archive_handle, checkpoint_buffer));
  require(checkpoint_cd.close(checkpoint_root_handle) ==
              sf::game::LegacyCdResult::success,
          "Virtual CD checkpoint mutation failed");
  checkpoint_cd.unmount();
  checkpoint_cd.setCurrentRawSector(9U);
  require(checkpoint_cd.restoreSnapshot(cd_snapshot) &&
              checkpoint_cd.mountedArchive() == "FOG/SUBWAY.FOG" &&
              checkpoint_cd.currentRawSector() == checkpoint_raw_sector,
          "Virtual CD checkpoint mount/raw-sector restore mismatch");

  checkpoint_buffer.assign(sf::game::LegacyVirtualCd::sector_size,
                           std::byte{0x7e});
  read = checkpoint_cd.read(checkpoint_root_handle, checkpoint_buffer);
  require(read.result == sf::game::LegacyCdResult::success &&
              read.bytes_read == 2U &&
              read.transfer_size == checkpoint_buffer.size() &&
              checkpoint_buffer[0] ==
                  root_file[sf::game::LegacyVirtualCd::sector_size] &&
              checkpoint_buffer[1] == root_file.back() &&
              std::ranges::all_of(
                  checkpoint_buffer.begin() + 2U, checkpoint_buffer.end(),
                  [](std::byte value) {
                    return value == sf::game::LegacyVirtualCd::sector_padding;
                  }),
          "Virtual CD checkpoint file cursor restore mismatch");
  read = checkpoint_cd.read(checkpoint_archive_handle, checkpoint_buffer);
  require(read.result == sf::game::LegacyCdResult::success &&
              read.bytes_read == checkpoint_buffer.size() &&
              std::ranges::all_of(
                  checkpoint_buffer,
                  [](std::byte value) { return value == std::byte{0x42}; }),
          "Virtual CD checkpoint open-handle restore mismatch");
  sf::game::LegacyVirtualCd other_cd;
  require(!other_cd.restoreSnapshot(cd_snapshot),
          "Virtual CD accepted a checkpoint from another catalog instance");
}

void testLegacyGameplayVmBoundary() {
  constexpr std::array initial_words{
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  const auto initial_code = instructionBytes(initial_words);
  std::vector<std::byte> executable_bytes(2048U + initial_code.size());
  constexpr std::string_view signature{"PS-X EXE"};
  std::ranges::transform(signature, executable_bytes.begin(), [](char value) {
    return static_cast<std::byte>(value);
  });
  writeLe32(executable_bytes, 0x10U, code_address);
  writeLe32(executable_bytes, 0x18U, code_address);
  writeLe32(executable_bytes, 0x1cU,
            static_cast<std::uint32_t>(initial_code.size()));
  std::ranges::copy(initial_code, executable_bytes.begin() + 2048);

  const auto executable = sf::psx::Executable::parse(executable_bytes);
  sf::game::LegacyGameplayVm vm{executable};
  constexpr std::uint32_t overlay_address = 0x80020000U;
  constexpr std::array overlay_words{
      encodeI(0x09U, 4U, 2U, 1U),
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  const auto overlay = instructionBytes(overlay_words);
  require(vm.loadOverlay(overlay_address, overlay),
          "Legacy VM overlay load failed");
  const std::array arguments{41U};
  const auto result = vm.invoke(overlay_address, arguments);
  require(result.completed() && result.return_value == 42U,
          "Legacy VM did not execute an overlay function");

  std::uint32_t pass_through_calls{};
  vm.bindHostCall(
      overlay_address,
      [&pass_through_calls](sf::game::LegacyHostCallContext &context) {
        ++pass_through_calls;
        context.continueGuestInstruction();
      });
  const auto pass_through_result = vm.invoke(overlay_address, arguments);
  require(
      pass_through_result.completed() &&
          pass_through_result.return_value == 42U &&
          pass_through_result.host_calls == 1U && pass_through_calls == 1U,
      "Pass-through host hook did not execute the original guest instruction");
  require(vm.unbindHostCall(overlay_address),
          "Could not remove the pass-through host hook");

  constexpr auto retail_aim_profile =
      sf::game::syphonFilterUsaV11HostAimRayProfile();
  static_assert(
      retail_aim_profile.collision_scan_entry == 0x8003a3c8U &&
      retail_aim_profile.accepted_return_addresses[0] == 0x8003a8a4U &&
      retail_aim_profile.accepted_return_addresses[1] == 0x8003a99cU &&
      retail_aim_profile.accepted_return_addresses[2] == 0x8003aa34U &&
      retail_aim_profile.ray_length == 0x1900);
  auto aim_profile = sf::game::syphonFilterUsaV11HostAimRayProfile();
  constexpr std::uint32_t first_aim_caller = overlay_address + 0x20U;
  constexpr std::uint32_t second_aim_caller = overlay_address + 0x40U;
  constexpr std::uint32_t third_aim_caller = overlay_address + 0x60U;
  constexpr std::array first_aim_caller_words{
      encodeR(31U, 0U, 8U, 0U, 0x21U), encodeJ(0x03U, overlay_address), 0U,
      encodeR(8U, 0U, 31U, 0U, 0x21U), encodeR(31U, 0U, 0U, 0U, 0x08U), 0U,
  };
  constexpr std::array second_aim_caller_words{
      encodeR(31U, 0U, 8U, 0U, 0x21U), encodeJ(0x03U, overlay_address),
      encodeI(0x2bU, 4U, 5U, 4U),      encodeR(8U, 0U, 31U, 0U, 0x21U),
      encodeR(31U, 0U, 0U, 0U, 0x08U), 0U,
  };
  require(vm.loadOverlay(first_aim_caller,
                         instructionBytes(first_aim_caller_words)) &&
              vm.loadOverlay(second_aim_caller,
                             instructionBytes(second_aim_caller_words)) &&
              vm.loadOverlay(third_aim_caller,
                             instructionBytes(first_aim_caller_words)),
          "Legacy aim caller overlays failed to load");
  aim_profile.collision_scan_entry = overlay_address;
  aim_profile.accepted_return_addresses = {
      first_aim_caller + 12U,
      second_aim_caller + 12U,
      third_aim_caller + 12U,
  };
  constexpr std::uint32_t aim_descriptor = 0x801fe000U;
  constexpr std::uint32_t aim_origin = 0x801fe020U;
  constexpr std::uint32_t aim_endpoint = 0x801fe030U;
  require(vm.runtime().write32(aim_descriptor, aim_origin) &&
              vm.runtime().write32(aim_descriptor + 4U, aim_endpoint),
          "Could not prepare the retail aim descriptor fixture");
  vm.setHostAimRay(sf::game::LegacyHostAimRay{
      11.0,
      22.0,
      33.0,
      1.0,
      2.0,
      2.0,
  });
  vm.bindSyphonFilterUsaV11HostAimRayHook(aim_profile);
  const auto invoke_aim_caller = [&](std::uint32_t caller,
                                     std::uint64_t expected_patch_count) {
    const auto aimed_result =
        vm.invoke(caller, std::array{aim_descriptor, aim_endpoint});
    std::array<std::uint32_t, 6U> aimed_words{};
    require(aimed_result.completed() &&
                aimed_result.return_value == aim_descriptor + 1U &&
                vm.hostAimRayPatchCount() == expected_patch_count &&
                vm.runtime().read32(aim_origin, aimed_words[0]) &&
                vm.runtime().read32(aim_origin + 4U, aimed_words[1]) &&
                vm.runtime().read32(aim_origin + 8U, aimed_words[2]) &&
                vm.runtime().read32(aim_endpoint, aimed_words[3]) &&
                vm.runtime().read32(aim_endpoint + 4U, aimed_words[4]) &&
                vm.runtime().read32(aim_endpoint + 8U, aimed_words[5]) &&
                std::bit_cast<std::int32_t>(aimed_words[0]) == 11 &&
                std::bit_cast<std::int32_t>(aimed_words[1]) == -22 &&
                std::bit_cast<std::int32_t>(aimed_words[2]) == 33 &&
                std::bit_cast<std::int32_t>(aimed_words[3]) == 2144 &&
                std::bit_cast<std::int32_t>(aimed_words[4]) == -4289 &&
                std::bit_cast<std::int32_t>(aimed_words[5]) == 4300,
            "Native sight ray did not patch the retail origin and endpoint");
  };
  invoke_aim_caller(first_aim_caller, 1U);
  require(vm.runtime().write32(aim_descriptor + 4U, 0xdeadbeefU),
          "Could not prepare the stale endpoint-pointer fixture");
  invoke_aim_caller(second_aim_caller, 2U);
  invoke_aim_caller(third_aim_caller, 3U);

  constexpr std::uint32_t unrelated_endpoint_sentinel = 0x12345678U;
  require(vm.runtime().write32(aim_endpoint, unrelated_endpoint_sentinel),
          "Could not prepare the unrelated collision-ray fixture");
  const auto unrelated_result =
      vm.invoke(overlay_address, std::array{aim_descriptor});
  std::uint32_t endpoint_after_unrelated_call{};
  require(
      unrelated_result.completed() && vm.hostAimRayPatchCount() == 3U &&
          vm.runtime().read32(aim_endpoint, endpoint_after_unrelated_call) &&
          endpoint_after_unrelated_call == unrelated_endpoint_sentinel,
      "Native sight ray modified an unrelated retail collision call");

  const auto malformed_result =
      vm.invoke(first_aim_caller, std::array{0x801ffffcU, aim_endpoint});
  require(malformed_result.completed() && vm.hostAimRayPatchCount() == 3U,
          "Native sight ray accepted a malformed retail descriptor");
  constexpr std::uint32_t endpoint_sentinel = 0x76543210U;
  require(vm.runtime().write32(aim_endpoint, endpoint_sentinel),
          "Could not prepare the absent host-ray fixture");
  vm.setHostAimRay(std::nullopt);
  const auto absent_result =
      vm.invoke(second_aim_caller, std::array{aim_descriptor, aim_endpoint});
  std::uint32_t endpoint_after_absent_ray{};
  require(absent_result.completed() && vm.hostAimRayPatchCount() == 3U &&
              vm.runtime().read32(aim_endpoint, endpoint_after_absent_ray) &&
              endpoint_after_absent_ray == endpoint_sentinel,
          "Absent native sight ray modified the retail descriptor");
  require(vm.unbindHostCall(overlay_address),
          "Could not remove the retail aim-ray hook");

  constexpr std::uint32_t close_aim_caller = 0x80020400U;
  constexpr std::uint32_t close_aim_callee = 0x80020480U;
  constexpr std::array close_aim_caller_words{
      encodeI(0x09U, 29U, 29U, static_cast<std::uint16_t>(-0x30)),
      encodeI(0x0fU, 0U, 9U, 0U),
      encodeI(0x0dU, 9U, 9U, 500U),
      encodeI(0x2bU, 29U, 9U, 0x10U),
      encodeI(0x0fU, 0U, 9U, 0U),
      encodeI(0x0dU, 9U, 9U, 100U),
      encodeI(0x2bU, 29U, 9U, 0x18U),
      encodeR(31U, 0U, 8U, 0U, 0x21U),
      encodeJ(0x03U, close_aim_callee),
      encodeI(0x09U, 5U, 5U, 0x10U),
      encodeR(8U, 0U, 31U, 0U, 0x21U),
      encodeI(0x09U, 29U, 29U, 0x30U),
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  constexpr std::array close_aim_callee_words{
      encodeI(0x23U, 5U, 2U, 0U),
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  require(vm.loadOverlay(close_aim_caller,
                         instructionBytes(close_aim_caller_words)) &&
              vm.loadOverlay(close_aim_callee,
                             instructionBytes(close_aim_callee_words)),
          "Could not load the close enemy-aim fixture");
  auto close_aim_profile = sf::game::syphonFilterUsaV11EnemyCloseAimProfile();
  close_aim_profile.boundary = close_aim_caller + 0x20U;
  close_aim_profile.instruction = encodeJ(0x03U, close_aim_callee);
  constexpr std::uint32_t shooter = 0x801fd000U;
  constexpr std::uint32_t shooter_instance = 0x801fd100U;
  constexpr std::uint32_t shooter_matrix = 0x801fd200U;
  constexpr std::uint32_t lagged_target = 0x801fd300U;
  require(vm.runtime().write32(shooter + 8U, shooter_instance) &&
              vm.runtime().write32(shooter_instance + 0x0cU, shooter_matrix) &&
              vm.runtime().write32(shooter_matrix + 0x14U, 100U) &&
              vm.runtime().write32(shooter_matrix + 0x1cU, 100U) &&
              vm.runtime().write32(lagged_target, 0x1234U),
          "Could not seed the close enemy-aim fixture");
  vm.bindSyphonFilterUsaV11EnemyCloseAimHook(close_aim_profile);
  vm.runtime().setRegister(29U, 0x801fc000U);
  const auto close_aim =
      vm.invoke(close_aim_caller, std::array{shooter, lagged_target - 0x10U});
  require(close_aim.completed(), "Point-blank enemy aim did not complete");
  require(vm.enemyCloseAimPatchCount() == 1U,
          "Point-blank enemy aim hook did not patch the target");
  require(close_aim.return_value == 500U,
          "Point-blank enemy aim retained the lagged target");

  require(vm.runtime().write32(shooter_matrix + 0x14U,
                               std::bit_cast<std::uint32_t>(-1000)),
          "Could not move the enemy outside close range");
  vm.runtime().setRegister(29U, 0x801fc000U);
  const auto distant_aim =
      vm.invoke(close_aim_caller, std::array{shooter, lagged_target - 0x10U});
  require(distant_aim.completed() && distant_aim.return_value == 0x1234U &&
              vm.enemyCloseAimPatchCount() == 1U,
          "Distant enemy aim lost the original tracking point");
  require(vm.unbindHostCall(close_aim_profile.boundary),
          "Could not remove the close enemy-aim hook");

  constexpr auto retail_weapon_events =
      sf::game::syphonFilterUsaV11WeaponEventHookProfile();
  static_assert(!sf::game::legacyWeaponEventUsesFirstPerson(0U) &&
                !sf::game::legacyWeaponEventUsesFirstPerson(1U) &&
                sf::game::legacyWeaponEventUsesFirstPerson(2U) &&
                sf::game::legacyWeaponEventUsesFirstPerson(3U));
  static_assert(retail_weapon_events.boundaries[0].address == 0x800261d8U &&
                retail_weapon_events.boundaries[1].address == 0x80026554U &&
                retail_weapon_events.boundaries[2].address == 0x80025fc4U &&
                retail_weapon_events.boundaries[3].address == 0x800264dcU &&
                retail_weapon_events.boundaries[4].address == 0x800264a8U &&
                retail_weapon_events.boundaries[5].address == 0x8008d6a0U &&
                retail_weapon_events.boundaries[6].address == 0x800905dcU &&
                retail_weapon_events.boundaries[0].instruction == 0x0c011961U &&
                retail_weapon_events.boundaries[0].delay_instruction ==
                    0x02402021U);
  auto weapon_event_profile = retail_weapon_events;
  weapon_event_profile.boundaries[0] = {
      overlay_address,
      overlay_words[0],
      overlay_words[1],
      sf::game::LegacyWeaponEventType::shot,
  };
  constexpr std::uint32_t weapon_player_pointer = 0x801fd000U;
  constexpr std::uint32_t weapon_player = 0x801fd100U;
  constexpr std::uint32_t weapon_current = 0x801fd010U;
  constexpr std::uint32_t weapon_aim_mode = 0x801fd014U;
  constexpr std::uint32_t weapon_hit_result = 0x801fd018U;
  constexpr std::uint32_t weapon_aimed_slot = 0x801fd01cU;
  constexpr std::uint32_t weapon_ray_origin = 0x801fd020U;
  constexpr std::uint32_t weapon_ray_endpoint = 0x801fd030U;
  weapon_event_profile.player_pointer = weapon_player_pointer;
  weapon_event_profile.current_weapon = weapon_current;
  weapon_event_profile.aim_mode = weapon_aim_mode;
  weapon_event_profile.hit_result = weapon_hit_result;
  weapon_event_profile.aimed_target_slot = weapon_aimed_slot;
  weapon_event_profile.ray_origin = weapon_ray_origin;
  weapon_event_profile.ray_endpoint = weapon_ray_endpoint;
  weapon_event_profile.maximum_events = 2U;
  require(vm.runtime().write32(weapon_player_pointer, weapon_player) &&
              vm.runtime().write16(weapon_player + 2U, 7U) &&
              vm.runtime().write32(weapon_current, 1U) &&
              vm.runtime().write32(weapon_aim_mode, 2U) &&
              vm.runtime().write32(weapon_hit_result, 0x80041000U) &&
              vm.runtime().write16(weapon_aimed_slot, 9U) &&
              vm.runtime().write32(weapon_ray_origin, 100U) &&
              vm.runtime().write32(weapon_ray_origin + 4U,
                                   std::bit_cast<std::uint32_t>(-200)) &&
              vm.runtime().write32(weapon_ray_origin + 8U, 300U) &&
              vm.runtime().write32(weapon_ray_endpoint, 400U) &&
              vm.runtime().write32(weapon_ray_endpoint + 4U,
                                   std::bit_cast<std::uint32_t>(-500)) &&
              vm.runtime().write32(weapon_ray_endpoint + 8U, 600U),
          "Could not prepare retail weapon-event fixture");
  vm.bindSyphonFilterUsaV11WeaponEventHooks(weapon_event_profile);
  const auto weapon_event_result =
      vm.invoke(overlay_address, std::array{weapon_player});
  require(weapon_event_result.completed() &&
              weapon_event_result.return_value == weapon_player + 1U &&
              vm.weaponEvents().size() == 1U,
          "Retail shot hook did not preserve the guest instruction");
  const auto &shot_event = vm.weaponEvents().front();
  require(shot_event.type == sf::game::LegacyWeaponEventType::shot &&
              shot_event.weapon == 1U && shot_event.actor_slot == 7 &&
              shot_event.aimed_target_slot == 9 &&
              shot_event.hit_result == 0x80041000U && shot_event.first_person &&
              shot_event.origin.x == 100 && shot_event.origin.y == -200 &&
              shot_event.origin.z == 300 && shot_event.endpoint.x == 400 &&
              shot_event.endpoint.y == -500 && shot_event.endpoint.z == 600,
          "Retail shot hook exported the wrong immutable event");
  vm.clearWeaponEvents();

  weapon_event_profile.boundaries[0].type =
      sf::game::LegacyWeaponEventType::c4_use;
  require(vm.runtime().write32(weapon_current, 25U),
          "Could not select antigen for the item-use fixture");
  vm.bindSyphonFilterUsaV11WeaponEventHooks(weapon_event_profile);
  const auto antigen_result =
      vm.invoke(overlay_address, std::array{weapon_player});
  require(antigen_result.completed() && vm.weaponEvents().size() == 1U &&
              vm.weaponEvents().front().type ==
                  sf::game::LegacyWeaponEventType::antigen_use,
          "Shared retail mission-item boundary did not distinguish antigen");
  vm.clearWeaponEvents();

  require(vm.runtime().write32(overlay_address, 0U),
          "Could not corrupt weapon hook descriptor fixture");
  const auto rejected_weapon_event =
      vm.invoke(overlay_address, std::array{weapon_player});
  require(rejected_weapon_event.completed() && vm.weaponEvents().empty(),
          "Weapon observer corrupted guest execution on an unknown opcode");
  require(vm.loadOverlay(overlay_address, overlay),
          "Could not restore weapon hook descriptor fixture");
  for (const auto &boundary : weapon_event_profile.boundaries) {
    static_cast<void>(vm.unbindHostCall(boundary.address));
  }

  constexpr std::uint32_t interrupt_host_call = 0x80021000U;
  constexpr std::array interrupt_handler_words{
      encodeI(0x0fU, 0U, 8U, 0x1f80U),
      encodeI(0x0dU, 8U, 8U, 0x1070U),
      encodeI(0x09U, 0U, 9U, 0xfffeU),
      encodeI(0x2bU, 8U, 9U, 0U),
      encodeCop0Transfer(0U, 26U, 14U),
      0U,
      0x42000010U,
      encodeR(26U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  const auto interrupt_handler = instructionBytes(interrupt_handler_words);
  require(vm.runtime().loadBytes(0x80000080U, interrupt_handler) &&
              vm.runtime().write16(0x1f801074U, 1U),
          "Could not install the legacy VM interrupt-ordering fixture");
  auto interrupt_cpu = vm.runtime().state();
  interrupt_cpu.cop0_status = (1U << 10U) | 1U;
  vm.runtime().restoreCpuState(interrupt_cpu);
  bool host_call_saw_interrupt{};
  vm.bindHostCall(interrupt_host_call,
                  [&](sf::game::LegacyHostCallContext &context) {
                    host_call_saw_interrupt =
                        vm.runtime().state().cop0_epc == interrupt_host_call;
                    context.setReturnValue(77U);
                  });
  vm.machine().pulseVBlank();
  const auto interrupted_host_result = vm.invoke(interrupt_host_call);
  require(interrupted_host_result.completed() &&
              interrupted_host_result.return_value == 77U &&
              host_call_saw_interrupt,
          "Legacy HLE hook ran before a pending hardware interrupt");
  require(vm.unbindHostCall(interrupt_host_call),
          "Could not remove the interrupt-ordering host hook");

  vm.clearPcm();
  const auto audio_frame_start = vm.machine().currentTick();
  require(vm.advanceAudioFrameClock(),
          "Legacy VM audio frame clock rejected the retail profile");
  constexpr auto audio_ticks_per_frame =
      sf::psx::PsxMachine::cpu_clock_hz /
      sf::game::LegacyGameplayVm::updates_per_second;
  constexpr auto pcm_frames_per_update =
      sf::psx::Spu::sample_rate /
      sf::game::LegacyGameplayVm::updates_per_second;
  std::vector<sf::psx::SpuPcmFrame> pcm(pcm_frames_per_update);
  require(vm.machine().currentTick() ==
                  audio_frame_start + audio_ticks_per_frame &&
              vm.takePcm(pcm) == pcm.size(),
          "Legacy VM 20 Hz audio clock did not emit exactly 2205 frames");

  vm.clearPcm();
  const auto sliced_audio_start = vm.machine().currentTick();
  auto sliced_audio_ok = true;
  for (auto slice = 0U; slice < 6U; ++slice) {
    sliced_audio_ok = sliced_audio_ok && vm.advanceAudioSliceClock();
  }
  std::ranges::fill(pcm, sf::psx::SpuPcmFrame{});
  require(sliced_audio_ok &&
              vm.machine().currentTick() ==
                  sliced_audio_start + audio_ticks_per_frame &&
              vm.takePcm(pcm) == pcm.size(),
          "Six low-latency SPU slices diverged from one retail audio frame");

  sf::game::LegacyGameplayVm overclocked_vm{executable,
                                            sf::psx::CpuClockScale{2U, 1U}};
  overclocked_vm.clearPcm();
  const auto overclocked_audio_start = overclocked_vm.machine().currentTick();
  std::ranges::fill(pcm, sf::psx::SpuPcmFrame{});
  require(overclocked_vm.machine().cpuTicksPerSecond() ==
                  sf::psx::PsxMachine::cpu_clock_hz * 2U &&
              overclocked_vm.advanceAudioFrameClock() &&
              overclocked_vm.machine().currentTick() ==
                  overclocked_audio_start + audio_ticks_per_frame * 2U &&
              overclocked_vm.takePcm(pcm) == pcm.size(),
          "200% guest CPU clock changed the fixed 20 Hz/44.1 kHz cadence");
  auto incompatible_clock_snapshot = overclocked_vm.captureSnapshot();
  incompatible_clock_snapshot.machine.cpu_clock_scale = {1U, 1U};
  require(!overclocked_vm.restoreSnapshot(incompatible_clock_snapshot),
          "VM accepted a snapshot from a different guest CPU clock");

  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  const auto vblank_callbacks = vm.invoke(0x800e4248U);
  require(vblank_callbacks.completed() && vblank_callbacks.return_value == 0U,
          "VSyncCallbacks escaped the native platform frame boundary");
  auto audio_profile = sf::game::syphonFilterUsaV11RetailAudioProfile();
  constexpr std::uint32_t audio_callback = 0x80022000U;
  audio_profile.expected_tick_callback = audio_callback;
  std::uint32_t audio_callback_count{};
  vm.bindHostCall(
      audio_callback,
      [&audio_callback_count](sf::game::LegacyHostCallContext &context) {
        ++audio_callback_count;
        context.setReturnValue(0U);
      });
  const std::array register_audio_callback{
      static_cast<std::uint32_t>(audio_profile.timer_irq), audio_callback};
  const auto registered = vm.invoke(audio_profile.interrupt_callback_entry,
                                    register_audio_callback);
  require(registered.completed() && registered.return_value == 0U,
          "InterruptCallback did not register the retail audio timer");
  vm.clearPcm();
  require(vm.advanceAudioFrameClock(audio_profile) &&
              audio_callback_count == 6U,
          "Retail 120 Hz audio callback did not run six times per frame");

  // A ready-sector callback executes inside the 120 Hz scheduler. It must not
  // advance CD/SPU hardware a second time merely because guest instructions
  // are needed to acknowledge the sector. That old double clock generated
  // excess PCM at every streamed room transition and eventually forced the
  // frontend to drop complete sounds.
  constexpr std::uint32_t cd_ready_callback_address = 0x80114cc4U;
  constexpr std::uint32_t cd_ready_callback = 0x80022020U;
  std::uint32_t cd_ready_callback_count{};
  vm.bindHostCall(
      cd_ready_callback,
      [&cd_ready_callback_count](sf::game::LegacyHostCallContext &context) {
        ++cd_ready_callback_count;
        context.setReturnValue(0U);
      });
  auto ready_cdrom = vm.machine().cdrom().captureState();
  ready_cdrom.interrupt_flags = 1U;
  ready_cdrom.response_position = 0U;
  ready_cdrom.response_count = 1U;
  ready_cdrom.response[0] = 2U;
  require(vm.runtime().write32(cd_ready_callback_address, cd_ready_callback) &&
              vm.machine().cdrom().restoreState(ready_cdrom),
          "Could not seed a retail CD ready callback");
  vm.clearPcm();
  const auto ready_callback_tick = vm.machine().currentTick();
  std::ranges::fill(pcm, sf::psx::SpuPcmFrame{});
  require(vm.advanceAudioFrameClock(audio_profile) &&
              cd_ready_callback_count == 1U &&
              vm.machine().currentTick() ==
                  ready_callback_tick + audio_ticks_per_frame &&
              vm.takePcm(pcm) == pcm.size(),
          "CD ready callback advanced the hardware/audio timeline twice");
  require(vm.unbindHostCall(cd_ready_callback),
          "Could not remove the CD ready callback fixture");

  // Non-data CD interrupts belong to the guest CD handler, not to the audio
  // scheduler. In retail gameplay an INT2/INT3 can remain latched while a room
  // stream changes state. It must neither be acknowledged here nor terminate
  // the independent 120 Hz SPU clock.
  auto command_cdrom = vm.machine().cdrom().captureState();
  command_cdrom.interrupt_flags = 2U;
  command_cdrom.response_position = 0U;
  command_cdrom.response_count = 1U;
  command_cdrom.response[0] = 2U;
  require(vm.machine().cdrom().restoreState(command_cdrom) &&
              vm.advanceAudioSliceClock(audio_profile),
          "A non-data CD interrupt stopped the audio clock");
  const auto retained_command_cdrom = vm.machine().cdrom().captureState();
  require((retained_command_cdrom.interrupt_flags & 0x07U) == 2U &&
              retained_command_cdrom.response_position == 0U &&
              retained_command_cdrom.response_count == 1U &&
              retained_command_cdrom.response[0] == 2U &&
              audio_callback_count == 13U,
          "A non-data CD interrupt stopped or was consumed by the audio clock");
  constexpr std::uint32_t cdrom_register_base = 0x1f801800U;
  require(vm.runtime().write8(cdrom_register_base, 1U) &&
              vm.runtime().write8(cdrom_register_base + 3U, 0x1fU) &&
              vm.runtime().write8(cdrom_register_base, 0U),
          "Could not acknowledge the command-complete interrupt fixture");
  const auto acknowledged_command_cdrom = vm.machine().cdrom().captureState();
  require((acknowledged_command_cdrom.interrupt_flags & 0x07U) == 0U &&
              acknowledged_command_cdrom.response_position == 0U &&
              acknowledged_command_cdrom.response_count == 0U,
          "Guest CD interrupt acknowledgement left response state latched");

  // Streaming can consume more than one retail frame of guest CPU time before
  // returning to the native outer loop. Real timer IRQs latch one pending bit;
  // replaying every elapsed edge as an immediate callback makes sequenced
  // music race or restart after a room/chunk transition.
  const auto overdue_start = vm.machine().currentTick();
  vm.machine().advanceTicks(audio_ticks_per_frame * 2U);
  const auto overdue_target = overdue_start + audio_ticks_per_frame * 2U;
  require(
      vm.advanceAudioFrameClock(audio_profile) &&
          vm.machine().currentTick() >= overdue_target &&
          vm.machine().currentTick() <= overdue_target + 16U &&
          audio_callback_count == 14U,
      "Overdue retail audio IRQs were not coalesced at a streaming boundary");

  vm.clearPcm();
  vm.machine().spu().mixFrames(32U);
  const auto host_pcm_snapshot = vm.captureSnapshot();
  require(vm.machine().spu().queuedPcmFrames() == 32U &&
              vm.restoreSnapshot(host_pcm_snapshot) &&
              vm.machine().spu().queuedPcmFrames() == 0U,
          "Gameplay snapshot replayed already-rendered host PCM");

  const auto callback_snapshot = vm.captureSnapshot();
  const std::array unregister_audio_callback{
      static_cast<std::uint32_t>(audio_profile.timer_irq), 0U};
  const auto unregistered = vm.invoke(audio_profile.interrupt_callback_entry,
                                      unregister_audio_callback);
  require(unregistered.completed() &&
              unregistered.return_value == audio_callback &&
              vm.advanceAudioFrameClock(audio_profile) &&
              audio_callback_count == 14U,
          "InterruptCallback did not return and remove the previous callback");
  require(vm.restoreSnapshot(callback_snapshot) &&
              vm.advanceAudioFrameClock(audio_profile) &&
              audio_callback_count == 20U,
          "Audio callback registration did not survive snapshot restore");
  const auto reset_callbacks = vm.invoke(audio_profile.reset_callback_entry);
  require(reset_callbacks.completed() &&
              vm.advanceAudioFrameClock(audio_profile) &&
              audio_callback_count == 20U,
          "ResetCallback did not clear the retail audio timer");
  vm.clearPcm();

  constexpr std::uint32_t volume_setter = 0x80022040U;
  constexpr std::uint32_t volume_table = 0x80007000U;
  constexpr std::uint32_t spu_voice_registers = 0x1f801c00U;
  audio_profile.set_group_volume_entry = volume_setter;
  audio_profile.group_volume_address = volume_table;
  vm.bindHostCall(volume_setter, [=](sf::game::LegacyHostCallContext &context) {
    const auto group = context.argument(0);
    const auto volume = context.argument(1);
    if (group >= sf::game::LegacyRetailAudioVolumes::group_count ||
        !context.write8(volume_table + group,
                        static_cast<std::uint8_t>(volume)) ||
        !context.write16(spu_voice_registers + group * 0x10U,
                         static_cast<std::uint16_t>(volume * 0x0101U))) {
      context.rejectHostCall();
      return;
    }
    context.setReturnValue(0U);
  });
  auto atomic_audio_cpu = vm.runtime().state();
  atomic_audio_cpu.pc = code_address;
  atomic_audio_cpu.next_pc = code_address + 4U;
  atomic_audio_cpu.branch_pc = code_address;
  atomic_audio_cpu.branch_delay_slot = false;
  atomic_audio_cpu.load_delay = {7U, 0x11223344U, true};
  atomic_audio_cpu.next_load_delay = {8U, 0x55667788U, true};
  vm.runtime().restoreCpuState(atomic_audio_cpu);
  const auto deferred_clock_seed = vm.invoke(overlay_address, arguments);
  require(deferred_clock_seed.completed(),
          "Could not seed deferred CPU ticks before the audio overlay");
  vm.runtime().restoreCpuState(atomic_audio_cpu);
  vm.clearPcm();
  const auto pending_tick_snapshot = vm.captureSnapshot();
  require(pending_tick_snapshot.machine.pending_cpu_ticks != 0U &&
              pending_tick_snapshot.audio_frame_tick ==
                  logicalMachineTick(pending_tick_snapshot.machine) &&
              vm.restoreSnapshot(pending_tick_snapshot),
          "VM rejected its own audio snapshot with deferred CPU ticks");
  const auto audio_overlay_before = vm.captureSnapshot();
  require(audio_overlay_before.machine.pending_cpu_ticks != 0U,
          "Audio overlay regression did not start from a live CPU slice");
  const sf::game::LegacyRetailAudioVolumes overlay_volumes{17U, 43U, 71U};
  require(vm.setRetailAudioVolumes(overlay_volumes, audio_profile),
          "Atomic retail audio overlay rejected valid volumes");
  const auto audio_overlay_after = vm.captureSnapshot();
  const auto read_volume = [&vm](std::uint32_t group) {
    std::uint8_t value{};
    require(vm.runtime().read8(volume_table + group, value),
            "Could not read atomic audio volume fixture");
    return value;
  };
  require(
      sameCpuState(audio_overlay_before.cpu, audio_overlay_after.cpu) &&
          logicalMachineTick(audio_overlay_before.machine) ==
              logicalMachineTick(audio_overlay_after.machine) &&
          audio_overlay_after.machine.pending_cpu_ticks == 0U &&
          read_volume(0U) == overlay_volumes.sound_effects &&
          read_volume(1U) == overlay_volumes.music &&
          read_volume(2U) == overlay_volumes.voice_over &&
          audio_overlay_before.machine.spu && audio_overlay_after.machine.spu &&
          *audio_overlay_before.machine.spu != *audio_overlay_after.machine.spu,
      "Retail audio overlay advanced or corrupted the guest timeline");

  const auto audio_rollback_before = vm.captureSnapshot();
  vm.bindHostCall(volume_setter, [=](sf::game::LegacyHostCallContext &context) {
    const auto group = context.argument(0);
    static_cast<void>(context.write8(
        volume_table + group, static_cast<std::uint8_t>(context.argument(1))));
    if (group == 1U) {
      context.rejectHostCall();
      return;
    }
    context.setReturnValue(0U);
  });
  require(!vm.setRetailAudioVolumes({3U, 5U, 7U}, audio_profile),
          "Failing retail audio overlay was accepted");
  const auto audio_rollback_after = vm.captureSnapshot();
  require(sameCpuState(audio_rollback_before.cpu, audio_rollback_after.cpu) &&
              sameMachineState(audio_rollback_before.machine,
                               audio_rollback_after.machine) &&
              audio_rollback_before.ram == audio_rollback_after.ram &&
              audio_rollback_before.scratchpad ==
                  audio_rollback_after.scratchpad &&
              audio_rollback_before.mmio == audio_rollback_after.mmio,
          "Failed retail audio overlay partially mutated the guest");

  constexpr std::uint32_t dma_source = 0x00006000U;
  constexpr std::uint32_t dma4_base = 0x1f8010c0U;
  constexpr std::uint32_t dma_dpcr = 0x1f8010f0U;
  constexpr std::uint32_t spu_transfer_address = 0x1f801da6U;
  constexpr std::uint32_t spu_control = 0x1f801daaU;
  require(vm.runtime().write32(dma_source, 0x44332211U) &&
              vm.runtime().write16(spu_transfer_address, 0U) &&
              vm.runtime().write16(spu_control, 2U << 4U) &&
              vm.runtime().write32(dma_dpcr,
                                   vm.machine().dma().dpcr() | (1U << 19U)) &&
              vm.runtime().write32(dma4_base, dma_source) &&
              vm.runtime().write32(dma4_base + 4U, 0x00010001U) &&
              vm.runtime().write32(dma4_base + 8U, 0x01000201U) &&
              vm.machine().dmaCompletionTick(sf::psx::DmaChannel::spu),
          "Could not schedule the BIOS event DMA4 fixture");
  const auto invoke_bios_b0 = [&vm](std::uint32_t call) {
    auto cpu = vm.runtime().state();
    cpu.gpr[9] = call;
    vm.runtime().restoreCpuState(cpu);
    return vm.invoke(0x000000b0U, std::array{1U});
  };
  const auto pending_event = invoke_bios_b0(0x0bU);
  require(pending_event.completed() && pending_event.return_value == 0U &&
              vm.machine().dmaCompletionTick(sf::psx::DmaChannel::spu),
          "BIOS TestEvent reported a pending SPU DMA as complete");
  const auto completed_event = invoke_bios_b0(0x0aU);
  require(completed_event.completed() && completed_event.return_value == 1U &&
              !vm.machine().dmaCompletionTick(sf::psx::DmaChannel::spu) &&
              vm.machine().spu().ram()[0] == std::byte{0x11U} &&
              vm.machine().spu().ram()[3] == std::byte{0x44U},
          "BIOS WaitEvent did not wait for the scheduled SPU DMA");

  constexpr std::uint32_t bone_resolver_address = 0x80020100U;
  constexpr std::array bone_resolver_words{
      encodeI(0x23U, 4U, 8U, 0x14U),
      0U,
      encodeI(0x09U, 8U, 8U, 1U),
      encodeI(0x2bU, 4U, 8U, 0x14U),
      encodeI(0x09U, 0U, 2U, 0U),
      encodeR(31U, 0U, 0U, 0U, 0x08U),
      0U,
  };
  const auto bone_resolver = instructionBytes(bone_resolver_words);
  require(vm.loadOverlay(bone_resolver_address, bone_resolver),
          "Legacy VM bone resolver overlay load failed");

  auto snapshot_cpu = vm.runtime().state();
  snapshot_cpu.cop0_status = 0x40001234U;
  vm.runtime().restoreCpuState(snapshot_cpu);
  require(vm.runtime().write32(0x80010100U, 0x12345678U) &&
              vm.runtime().write32(0x1f800100U, 0x89abcdefU) &&
              vm.runtime().write32(0x1f8010f0U, 0x0badc0deU),
          "Legacy VM snapshot seed failed");
  const auto snapshot = vm.captureSnapshot();
  vm.runtime().reset(0x80030000U, 0x11111111U, 0x8001fff0U);
  require(vm.runtime().write32(0x80010100U, 0U) &&
              vm.runtime().write32(0x1f800100U, 0U) &&
              vm.runtime().write32(0x1f8010f0U, 0U) &&
              vm.restoreSnapshot(snapshot),
          "Legacy VM snapshot restore failed");
  std::uint32_t restored_ram{};
  std::uint32_t restored_scratchpad{};
  std::uint32_t restored_mmio{};
  require(vm.runtime().read32(0x80010100U, restored_ram) &&
              vm.runtime().read32(0x1f800100U, restored_scratchpad) &&
              vm.runtime().read32(0x1f8010f0U, restored_mmio) &&
              restored_ram == 0x12345678U &&
              restored_scratchpad == 0x89abcdefU &&
              restored_mmio == 0x0badc0deU &&
              vm.runtime().state().pc == snapshot.cpu.pc &&
              vm.runtime().state().gpr[28] == snapshot.cpu.gpr[28] &&
              vm.runtime().state().gpr[29] == snapshot.cpu.gpr[29] &&
              vm.runtime().state().cop0_status == snapshot.cpu.cop0_status,
          "Legacy VM snapshot state mismatch");
  auto invalid_snapshot = snapshot;
  invalid_snapshot.machine.scheduler.next_token = 0U;
  require(vm.runtime().write32(0x80010100U, 0xfeedfaceU) &&
              !vm.restoreSnapshot(invalid_snapshot) &&
              vm.runtime().read32(0x80010100U, restored_ram) &&
              restored_ram == 0xfeedfaceU && vm.restoreSnapshot(snapshot),
          "Rejected machine snapshot partially mutated the legacy VM");
  invalid_snapshot = snapshot;
  invalid_snapshot.machine.scheduler.next_token =
      std::numeric_limits<std::uint64_t>::max();
  require(!vm.restoreSnapshot(invalid_snapshot),
          "Machine snapshot accepted an exhausted scheduler token space");
  invalid_snapshot = snapshot;
  invalid_snapshot.audio_frame_tick =
      invalid_snapshot.machine.scheduler.now + 1U;
  require(!vm.restoreSnapshot(invalid_snapshot),
          "Legacy VM accepted a future audio frame anchor");
  invalid_snapshot = snapshot;
  invalid_snapshot.interrupt_callbacks[audio_profile.timer_irq] = 1U;
  require(!vm.restoreSnapshot(invalid_snapshot),
          "Legacy VM accepted an invalid interrupt callback address");

  require(vm.runtime().write32(0x80010100U, 0xdecafbadU),
          "Could not seed atomic CPU snapshot rejection");
  const auto rejection_cpu = vm.runtime().state();
  const auto rejection_machine = vm.machine().captureState();
  const auto reject_cpu_snapshot = [&](auto corrupt, const char *message) {
    auto candidate = snapshot;
    corrupt(candidate.cpu);
    std::uint32_t preserved_ram{};
    require(
        !vm.restoreSnapshot(candidate) &&
            vm.runtime().read32(0x80010100U, preserved_ram) &&
            preserved_ram == 0xdecafbadU &&
            sameCpuState(vm.runtime().state(), rejection_cpu) &&
            sameMachineState(vm.machine().captureState(), rejection_machine),
        message);
  };
  reject_cpu_snapshot([](auto &cpu) { cpu.pc |= 2U; },
                      "Misaligned snapshot PC mutated the legacy VM");
  reject_cpu_snapshot([](auto &cpu) { cpu.next_pc |= 2U; },
                      "Misaligned snapshot next-PC mutated the legacy VM");
  reject_cpu_snapshot([](auto &cpu) { cpu.branch_pc |= 2U; },
                      "Misaligned snapshot branch-PC mutated the legacy VM");
  reject_cpu_snapshot(
      [](auto &cpu) {
        cpu.load_delay = sf::psx::R3000DelayedLoadState{32U, 1U, true};
      },
      "Out-of-range delayed-load register mutated the legacy VM");
  reject_cpu_snapshot(
      [](auto &cpu) {
        cpu.next_load_delay = sf::psx::R3000DelayedLoadState{255U, 2U, true};
      },
      "Out-of-range next-load register mutated the legacy VM");
  reject_cpu_snapshot(
      [](auto &cpu) {
        cpu.next_load_delay = sf::psx::R3000DelayedLoadState{32U, 0U, false};
      },
      "Dormant out-of-range load register mutated the legacy VM");
  reject_cpu_snapshot([](auto &cpu) { cpu.gpr[0] = 1U; },
                      "Nonzero snapshot r0 mutated the legacy VM");
  require(vm.restoreSnapshot(snapshot),
          "Valid snapshot was rejected after CPU validation cases");

  auto bridge_profile = sf::game::LegacyGameplayBridgeProfile{};
  bridge_profile.camera_controller_pointer = 0x80032000U;
  bridge_profile.camera_mode = 0x80032020U;
  bridge_profile.camera_lock = 0x80032024U;
  bridge_profile.presentation_viewport_pointer = 0x800320fcU;
  bridge_profile.player_control_lock = 0x80032028U;
  bridge_profile.current_room = 0x8003202cU;
  bridge_profile.world_layout_pointer = 0x80032088U;
  bridge_profile.world_model_descriptors = 0x800320e4U;
  bridge_profile.world_model_count = 0x8003208cU;
  bridge_profile.world_visibility_bytes = 0x80032090U;
  bridge_profile.active_terrain_depth_cue = 0x800320f0U;
  bridge_profile.terrain_depth_cue = 0x800320f4U;
  bridge_profile.renderer_display_flags = 0x800320f8U;
  bridge_profile.screen_filter_enabled = 0x8004a400U;
  bridge_profile.screen_filter_descriptor = 0x8004a404U;
  bridge_profile.nightvision_clear_color = 0x8004a408U;
  bridge_profile.fade_step = 0x80032004U;
  bridge_profile.fade_current = 0x80032006U;
  bridge_profile.fade_callback = 0x80032008U;
  bridge_profile.fade_initialized = 0x8003200cU;
  bridge_profile.fade_floor_rgb = 0x8003200dU;
  bridge_profile.object_records_pointer = 0x80032010U;
  bridge_profile.object_count = 0x80032014U;
  bridge_profile.object_definitions_pointer = 0x80032018U;
  bridge_profile.object_definition_count = 0x8003201cU;
  bridge_profile.object_handler_table = 0x80032100U;
  bridge_profile.dynamic_first_slot = 0x80032038U;
  bridge_profile.target_lock_active = 0x8003203aU;
  bridge_profile.virus_scanner_target = 0x8004a420U;
  bridge_profile.virus_scanner_target_slot = 0x8004a42cU;
  bridge_profile.flashlight_enabled = 0x8004a2c0U;
  bridge_profile.dynamic_light_list = 0x800320e0U;
  bridge_profile.taser_conductor_phase = 0x80032084U;
  bridge_profile.taser_target_slot = 0x80032086U;
  bridge_profile.tracked_slots = 0x80032040U;
  bridge_profile.target_hit_result = 0x80032060U;
  bridge_profile.aimed_target_slot = 0x80032064U;
  bridge_profile.proximity_target_slot = 0x80032066U;
  bridge_profile.headshot_text_handle = 0x80032068U;
  bridge_profile.active_text_list = 0x8003206cU;
  bridge_profile.text_object_pool = 0x80049100U;
  bridge_profile.text_object_stride = 0x1cU;
  bridge_profile.text_object_capacity = 4U;
  bridge_profile.headshot_text_pointer = 0x80032070U;
  bridge_profile.primary_story_text_pointer = 0x80032074U;
  bridge_profile.primary_story_target_slot = 0x80032078U;
  bridge_profile.secondary_story_text_pointer = 0x8003207cU;
  bridge_profile.secondary_story_target_slot = 0x80032080U;
  bridge_profile.effect_controller_pool_pointer = 0x80032030U;
  bridge_profile.effect_controller_count = 0x80032034U;
  bridge_profile.bone_matrix_resolver_entry = bone_resolver_address;
  bridge_profile.effect_particle_pool = 0x80040000U;
  bridge_profile.effect_particle_capacity = 4U;
  bridge_profile.player_thrown_projectile_pointer = 0x8004a500U;
  bridge_profile.enemy_thrown_projectile_pointer = 0x8004a504U;
  auto native_bridge_profile = sf::game::LegacyNativeMissionBridgeProfile{};
  native_bridge_profile.player_pointer = 0x80032084U;
  native_bridge_profile.object_records_pointer =
      bridge_profile.object_records_pointer;
  native_bridge_profile.object_count = bridge_profile.object_count;
  constexpr std::uint32_t camera_controller = 0x80033000U;
  constexpr std::uint32_t camera_object = 0x80034000U;
  constexpr std::uint32_t camera_matrix = 0x80035000U;
  constexpr std::uint32_t bridge_object_records = 0x80036000U;
  constexpr std::uint32_t bridge_object_definitions = 0x80037000U;
  constexpr std::uint32_t actor_instance = 0x80038000U;
  constexpr std::uint32_t actor_node = 0x80039000U;
  constexpr std::uint32_t actor_matrix = 0x8003a000U;
  constexpr std::uint32_t actor_motion = 0x8003b000U;
  constexpr std::uint32_t player_instance = 0x8003c000U;
  constexpr std::uint32_t player_node = 0x8003d000U;
  constexpr std::uint32_t player_matrix = 0x8003e000U;
  constexpr std::uint32_t player_bone_table = 0x8003f000U;
  constexpr std::uint32_t player_bone_matrix = 0x8003f100U;
  constexpr std::uint32_t player_motion = 0x8003f200U;
  constexpr std::uint32_t player_health = 0x8003f300U;
  constexpr std::uint32_t effect_controllers = 0x80041000U;
  constexpr std::uint32_t recycled_actor_instance = 0x80042000U;
  constexpr std::uint32_t recycled_actor_node = 0x80043000U;
  constexpr std::uint32_t recycled_actor_matrix = 0x80044000U;
  constexpr std::uint32_t recycled_actor_bone_table = 0x80045000U;
  constexpr std::uint32_t recycled_actor_bone_matrix = 0x80045100U;
  constexpr std::uint32_t recycled_actor_bone_coordinate = 0x80045140U;
  constexpr std::uint32_t recycled_actor_parent_bone_matrix = 0x80045180U;
  constexpr std::uint32_t recycled_actor_parent_bone_coordinate = 0x800451c0U;
  constexpr std::uint32_t recycled_actor_motion = 0x80046000U;
  constexpr std::uint32_t recycled_actor_presentation = 0x80047000U;
  constexpr std::uint32_t recycled_actor_target = 0x80047100U;
  constexpr std::uint32_t recycled_actor_ai = 0x80047200U;
  constexpr std::uint32_t recycled_actor_first_path = 0x80048000U;
  constexpr std::uint32_t recycled_actor_second_path = 0x80048100U;
  constexpr std::uint32_t attached_text_node = 0x80049000U;
  constexpr std::uint32_t attached_text_object = 0x80049100U;
  constexpr std::uint32_t headshot_text = 0x80049200U;
  constexpr std::uint32_t bridge_world_layout = 0x80049400U;
  constexpr std::uint32_t guest_sprite_node = 0x8004a100U;
  const std::uint32_t guest_sprite =
      bridge_profile.effect_particle_pool + 0x28U;
  constexpr std::uint32_t guest_line_node = 0x8004a180U;
  constexpr std::uint32_t guest_line = 0x8004a1a0U;
  constexpr std::uint32_t guest_raw_node = 0x8004a200U;
  constexpr std::uint32_t flashlight_light_node = 0x8004a300U;
  constexpr std::uint32_t flashlight_matrix = 0x8004a340U;
  constexpr std::uint32_t screen_filter_descriptor = 0x8004a440U;
  constexpr std::uint32_t actor_light_state = 0x8004a480U;
  constexpr std::uint32_t recycled_actor_light_state = 0x8004a4a0U;
  constexpr std::uint32_t player_light_state = 0x8004a4c0U;
  constexpr std::uint32_t presentation_viewport = 0x8004a680U;
  constexpr std::uint32_t world_descriptors = 0x8004b000U;
  constexpr std::uint32_t guest_world_model = 0x8004b200U;
  constexpr std::uint32_t guest_world_resource = 0x8004b240U;
  constexpr std::uint32_t guest_world_payload = 0x8004b280U;
  constexpr std::uint32_t guest_world_section = 0x8004b400U;
  constexpr std::uint32_t guest_world_vertices = guest_world_section + 0x2cU;
  const std::uint32_t guest_raw =
      bridge_profile.effect_particle_pool + 0x68U + 0x28U;
  require(
      vm.runtime().write32(bridge_profile.camera_controller_pointer,
                           camera_controller) &&
          vm.runtime().write32(bridge_profile.camera_mode, 0x0bU) &&
          vm.runtime().write32(bridge_profile.camera_lock, 1U) &&
          vm.runtime().write32(bridge_profile.presentation_viewport_pointer,
                               presentation_viewport) &&
          vm.runtime().write16(
              presentation_viewport +
                  bridge_profile.presentation_viewport_y_offset,
              0U) &&
          vm.runtime().write16(
              presentation_viewport +
                  bridge_profile.presentation_viewport_height_offset,
              240U) &&
          vm.runtime().write32(bridge_profile.player_control_lock, 1U) &&
          vm.runtime().write16(bridge_profile.current_room, 0xffffU) &&
          vm.runtime().write32(bridge_profile.dynamic_light_list,
                               flashlight_light_node) &&
          vm.runtime().write32(flashlight_light_node,
                               bridge_profile.flashlight_enabled) &&
          vm.runtime().write32(flashlight_light_node + 4U, 0U) &&
          vm.runtime().write32(flashlight_light_node + 8U, 0U) &&
          vm.runtime().write32(bridge_profile.flashlight_enabled,
                               flashlight_light_node) &&
          vm.runtime().write32(bridge_profile.flashlight_enabled + 4U, 1U) &&
          vm.runtime().write32(bridge_profile.flashlight_enabled + 8U,
                               flashlight_matrix) &&
          vm.runtime().write32(bridge_profile.flashlight_enabled + 0x0cU,
                               0x50U) &&
          vm.runtime().write32(bridge_profile.flashlight_enabled + 0x10U,
                               0x0eU) &&
          vm.runtime().write32(bridge_profile.flashlight_enabled + 0x14U, 6U) &&
          vm.runtime().write32(bridge_profile.flashlight_enabled + 0x18U, 0U) &&
          vm.runtime().write32(bridge_profile.flashlight_enabled + 0x1cU,
                               0x00ffffffU) &&
          vm.runtime().write16(flashlight_matrix, 4096U) &&
          vm.runtime().write16(flashlight_matrix + 8U, 4096U) &&
          vm.runtime().write16(flashlight_matrix + 16U, 4096U) &&
          vm.runtime().write32(flashlight_matrix + 0x14U, 101U) &&
          vm.runtime().write32(flashlight_matrix + 0x18U, 202U) &&
          vm.runtime().write32(flashlight_matrix + 0x1cU, 303U) &&
          vm.runtime().write32(bridge_profile.world_model_descriptors,
                               world_descriptors) &&
          vm.runtime().write32(world_descriptors, guest_world_model) &&
          vm.runtime().write32(world_descriptors + 1U * 0x3cU,
                               guest_world_model) &&
          // Model 2 is loaded but absent from resident/current/visibility.
          // Native widescreen can still present it, so its live lamp colors
          // must be captured before the player enters that zone.
          vm.runtime().write32(world_descriptors + 2U * 0x3cU,
                               guest_world_model) &&
          vm.runtime().write32(world_descriptors + 3U * 0x3cU,
                               guest_world_model) &&
          vm.runtime().write32(world_descriptors + 4U * 0x3cU,
                               guest_world_model) &&
          vm.runtime().write32(guest_world_model + 0x10U,
                               guest_world_resource) &&
          vm.runtime().write32(guest_world_resource + 0x20U,
                               guest_world_payload) &&
          vm.runtime().write32(guest_world_payload + 4U, guest_world_section) &&
          vm.runtime().write32(guest_world_payload + 8U, 0xffffffffU) &&
          vm.runtime().write16(guest_world_section + 6U, 3U) &&
          vm.runtime().write32(guest_world_section + 0x24U, 0x2cU) &&
          vm.runtime().write16(guest_world_vertices + 6U, 0x0421U) &&
          vm.runtime().write16(guest_world_vertices + 14U, 0x0842U) &&
          vm.runtime().write16(guest_world_vertices + 22U, 0x0c63U) &&
          vm.runtime().write32(bridge_profile.world_layout_pointer,
                               bridge_world_layout) &&
          vm.runtime().write32(bridge_profile.world_model_count, 5U) &&
          vm.runtime().write8(bridge_world_layout + 0x78U, 0U) &&
          vm.runtime().write8(bridge_world_layout + 0x79U, 4U) &&
          vm.runtime().write8(bridge_world_layout + 0x7aU, 0xffU) &&
          vm.runtime().write8(bridge_profile.world_visibility_bytes, 0U) &&
          vm.runtime().write8(bridge_profile.world_visibility_bytes + 1U, 3U) &&
          vm.runtime().write8(bridge_profile.world_visibility_bytes + 2U, 0U) &&
          vm.runtime().write8(bridge_profile.world_visibility_bytes + 3U, 1U) &&
          vm.runtime().write8(bridge_profile.world_visibility_bytes + 4U, 0U) &&
          vm.runtime().write32(camera_controller, camera_object) &&
          vm.runtime().write32(camera_object, camera_matrix) &&
          vm.runtime().write16(camera_object + 4U, 444U) &&
          vm.runtime().write8(
              camera_object + bridge_profile.renderer_clear_rgb_offset, 7U) &&
          vm.runtime().write8(camera_object +
                                  bridge_profile.renderer_clear_rgb_offset + 1U,
                              11U) &&
          vm.runtime().write8(camera_object +
                                  bridge_profile.renderer_clear_rgb_offset + 2U,
                              13U) &&
          vm.runtime().write8(
              camera_object + bridge_profile.renderer_back_rgb_offset, 23U) &&
          vm.runtime().write8(camera_object +
                                  bridge_profile.renderer_back_rgb_offset + 1U,
                              29U) &&
          vm.runtime().write8(camera_object +
                                  bridge_profile.renderer_back_rgb_offset + 2U,
                              31U) &&
          vm.runtime().write32(
              camera_object + bridge_profile.renderer_fog_dqa_offset,
              std::bit_cast<std::uint32_t>(std::int32_t{-123})) &&
          vm.runtime().write32(camera_object +
                                   bridge_profile.renderer_fog_dqb_offset,
                               0x12345000U) &&
          vm.runtime().write8(
              camera_object + bridge_profile.renderer_fog_rgb_offset, 41U) &&
          vm.runtime().write8(camera_object +
                                  bridge_profile.renderer_fog_rgb_offset + 1U,
                              43U) &&
          vm.runtime().write8(camera_object +
                                  bridge_profile.renderer_fog_rgb_offset + 2U,
                              47U) &&
          vm.runtime().write32(bridge_profile.active_terrain_depth_cue,
                               (5U << 16U) | 0x0800U) &&
          vm.runtime().write32(bridge_profile.terrain_depth_cue,
                               (2U << 16U) | 0x0321U) &&
          vm.runtime().write16(bridge_profile.renderer_display_flags,
                               0x00a4U) &&
          vm.runtime().write8(bridge_profile.screen_filter_enabled, 1U) &&
          vm.runtime().write32(bridge_profile.screen_filter_descriptor,
                               screen_filter_descriptor) &&
          vm.runtime().write32(screen_filter_descriptor + 0x14U, 0U) &&
          vm.runtime().write8(screen_filter_descriptor + 0x18U, 47U) &&
          vm.runtime().write8(screen_filter_descriptor + 0x19U, 112U) &&
          vm.runtime().write8(screen_filter_descriptor + 0x1aU, 77U) &&
          vm.runtime().write32(bridge_profile.nightvision_clear_color,
                               0xff00ff00U) &&
          vm.runtime().write16(
              camera_object + bridge_profile.renderer_flags_offset, 5U) &&
          vm.runtime().write8(
              camera_object + bridge_profile.renderer_sprite_fast_path_offset,
              1U) &&
          vm.runtime().write32(camera_object +
                                   bridge_profile.renderer_sprite_list_offset,
                               guest_sprite_node) &&
          vm.runtime().write32(camera_object +
                                   bridge_profile.renderer_line_list_offset,
                               guest_line_node) &&
          vm.runtime().write32(
              camera_object + bridge_profile.renderer_raw_packet_list_offset,
              guest_raw_node) &&
          vm.runtime().write32(guest_sprite_node, guest_sprite) &&
          vm.runtime().write32(guest_sprite_node + 8U, 0U) &&
          vm.runtime().write32(guest_sprite, 0x22000000U) &&
          vm.runtime().write16(guest_sprite + 0x04U, 17U) &&
          vm.runtime().write16(guest_sprite + 0x06U, 29U) &&
          vm.runtime().write16(guest_sprite + 0x08U, 32U) &&
          vm.runtime().write16(guest_sprite + 0x0aU, 24U) &&
          vm.runtime().write16(guest_sprite + 0x0cU, 31U) &&
          vm.runtime().write8(guest_sprite + 0x0eU, 64U) &&
          vm.runtime().write8(guest_sprite + 0x0fU, 96U) &&
          vm.runtime().write16(guest_sprite + 0x10U, 16U) &&
          vm.runtime().write16(guest_sprite + 0x12U, 12U) &&
          vm.runtime().write8(guest_sprite + 0x14U, 101U) &&
          vm.runtime().write8(guest_sprite + 0x15U, 102U) &&
          vm.runtime().write8(guest_sprite + 0x16U, 103U) &&
          vm.runtime().write16(guest_sprite + 0x18U, 2U) &&
          vm.runtime().write16(guest_sprite + 0x1aU, 3U) &&
          vm.runtime().write16(guest_sprite + 0x1cU, 4096U) &&
          vm.runtime().write16(guest_sprite + 0x1eU, 2048U) &&
          vm.runtime().write32(guest_sprite + 0x20U, 0x120U) &&
          vm.runtime().write32(guest_sprite + 0x24U, 77U) &&
          vm.runtime().write32(guest_line_node, guest_line) &&
          vm.runtime().write32(guest_line_node + 8U, 0U) &&
          vm.runtime().write32(guest_line, 0x40000000U) &&
          vm.runtime().write16(guest_line + 0x04U, 11U) &&
          vm.runtime().write16(guest_line + 0x06U, 12U) &&
          vm.runtime().write16(guest_line + 0x08U, 21U) &&
          vm.runtime().write16(guest_line + 0x0aU, 22U) &&
          vm.runtime().write8(guest_line + 0x0cU, 1U) &&
          vm.runtime().write8(guest_line + 0x0dU, 2U) &&
          vm.runtime().write8(guest_line + 0x0eU, 3U) &&
          vm.runtime().write8(guest_line + 0x0fU, 4U) &&
          vm.runtime().write8(guest_line + 0x10U, 5U) &&
          vm.runtime().write8(guest_line + 0x11U, 6U) &&
          vm.runtime().write32(guest_raw_node, guest_raw) &&
          vm.runtime().write32(guest_raw_node + 8U, 0U) &&
          vm.runtime().write32(guest_raw + 0x04U, 0U) &&
          vm.runtime().write32(guest_raw + 0x08U, 0x06000000U) &&
          vm.runtime().write32(guest_raw + 0x0cU, 0x30112233U) &&
          vm.runtime().write32(guest_raw + 0x10U, 0x00020001U) &&
          vm.runtime().write32(guest_raw + 0x14U, 0x00445566U) &&
          vm.runtime().write32(guest_raw + 0x18U, 0x00040003U) &&
          vm.runtime().write32(guest_raw + 0x1cU, 0x00778899U) &&
          vm.runtime().write32(guest_raw + 0x20U, 0x00060005U) &&
          vm.runtime().write32(camera_matrix + 0x14U, 100U) &&
          vm.runtime().write32(camera_matrix + 0x18U,
                               std::bit_cast<std::uint32_t>(-200)) &&
          vm.runtime().write32(camera_matrix + 0x1cU, 300U) &&
          vm.runtime().write32(camera_controller + 0x444U, 400U) &&
          vm.runtime().write32(camera_controller + 0x448U,
                               std::bit_cast<std::uint32_t>(-500)) &&
          vm.runtime().write32(camera_controller + 0x44cU, 600U) &&
          vm.runtime().write32(camera_controller + 0xbdcU, 827U) &&
          vm.runtime().write16(
              bridge_profile.fade_step,
              std::bit_cast<std::uint16_t>(std::int16_t{-8})) &&
          vm.runtime().write16(bridge_profile.fade_current, 135U) &&
          vm.runtime().write32(bridge_profile.fade_callback, 0x80123456U) &&
          vm.runtime().write8(bridge_profile.fade_initialized, 1U) &&
          vm.runtime().write8(bridge_profile.fade_floor_rgb, 15U) &&
          vm.runtime().write32(bridge_profile.object_records_pointer,
                               bridge_object_records) &&
          vm.runtime().write32(bridge_profile.object_count, 3U) &&
          vm.runtime().write32(bridge_profile.object_definition_count, 64U) &&
          vm.runtime().write32(bridge_profile.object_definitions_pointer,
                               bridge_object_definitions) &&
          vm.runtime().write32(bridge_profile.object_handler_table,
                               0x80060000U) &&
          vm.runtime().write32(bridge_profile.object_handler_table + 4U,
                               legacy_common_npc_handler) &&
          vm.runtime().write32(bridge_profile.object_handler_table + 0x35U * 4U,
                               legacy_common_npc_handler) &&
          vm.runtime().write16(bridge_profile.dynamic_first_slot, 1U) &&
          vm.runtime().write8(bridge_profile.target_lock_active, 1U) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target, 1234U) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target + 4U,
                               std::bit_cast<std::uint32_t>(-567)) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target + 8U,
                               890U) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target_slot, 1U) &&
          vm.runtime().write16(bridge_profile.taser_conductor_phase, 2U) &&
          vm.runtime().write16(bridge_profile.taser_target_slot, 1U) &&
          vm.runtime().write32(bridge_profile.target_hit_result, 0x8004a000U) &&
          vm.runtime().write16(bridge_profile.aimed_target_slot, 0U) &&
          vm.runtime().write16(bridge_profile.proximity_target_slot, 0U) &&
          vm.runtime().write16(bridge_profile.headshot_text_handle, 0x0700U) &&
          vm.runtime().write32(bridge_profile.active_text_list,
                               attached_text_node) &&
          vm.runtime().write32(bridge_profile.headshot_text_pointer,
                               headshot_text) &&
          vm.runtime().write16(bridge_profile.primary_story_target_slot,
                               0xffffU) &&
          vm.runtime().write16(bridge_profile.secondary_story_target_slot,
                               0xffffU) &&
          vm.runtime().write32(attached_text_node, attached_text_object) &&
          vm.runtime().write32(attached_text_node + 8U, 0U) &&
          vm.runtime().write8(attached_text_object + 0x14U, 0x03U) &&
          vm.runtime().write8(attached_text_object + 0x15U, 7U) &&
          vm.runtime().write16(attached_text_object + 0x16U, 1U) &&
          vm.runtime().write32(headshot_text, 0x64616548U) &&
          vm.runtime().write32(headshot_text + 4U, 0x6f685320U) &&
          vm.runtime().write16(headshot_text + 8U, 0x0074U) &&
          vm.runtime().write16(bridge_profile.tracked_slots, 1U) &&
          vm.runtime().write16(bridge_profile.tracked_slots + 2U, 0U) &&
          vm.runtime().write16(bridge_profile.tracked_slots + 4U, 0xffffU) &&
          vm.runtime().write16(bridge_profile.tracked_slots + 6U, 0xffffU) &&
          vm.runtime().write16(bridge_profile.tracked_slots + 8U, 0xffffU) &&
          vm.runtime().write16(bridge_profile.tracked_slots + 10U, 0xffffU) &&
          vm.runtime().write32(bridge_profile.effect_controller_pool_pointer,
                               effect_controllers) &&
          vm.runtime().write16(bridge_profile.effect_controller_count, 4U),
      "Could not seed legacy camera/fade bridge");
  constexpr std::uint32_t second_record = bridge_object_records + 0x4cU;
  constexpr std::uint32_t third_record = bridge_object_records + 0x98U;
  require(
      vm.runtime().write32(bridge_object_records, 13U) &&
          vm.runtime().write16(bridge_object_records + 0x24U, 0x21U) &&
          vm.runtime().write32(bridge_object_records + 0x28U, 7U) &&
          vm.runtime().write32(bridge_object_records + 0x30U, 53U) &&
          vm.runtime().write32(bridge_object_records + 0x34U, actor_instance) &&
          vm.runtime().write16(bridge_object_records + 0x3eU, 100U) &&
          vm.runtime().write16(bridge_object_records + 0x40U, 75U) &&
          vm.runtime().write16(bridge_object_definitions + 13U * 0x14U,
                               0x35U) &&
          vm.runtime().write8(actor_instance, 0x40U) &&
          vm.runtime().write32(actor_instance + 8U, actor_node) &&
          vm.runtime().write32(actor_instance + 0x0cU, actor_motion) &&
          vm.runtime().write32(actor_motion + 0x12cU, 0x80000002U) &&
          vm.runtime().write32(actor_node + 0x0cU, actor_matrix) &&
          vm.runtime().write32(actor_node + 0x1cU, actor_light_state) &&
          vm.runtime().write16(actor_light_state + 4U, 300U) &&
          vm.runtime().write16(actor_light_state + 6U, 400U) &&
          vm.runtime().write16(actor_light_state + 8U, 500U) &&
          vm.runtime().write16(actor_matrix, 4096U) &&
          vm.runtime().write16(actor_matrix + 8U, 4096U) &&
          vm.runtime().write16(actor_matrix + 0x10U, 4096U) &&
          vm.runtime().write32(actor_matrix + 0x14U,
                               std::bit_cast<std::uint32_t>(-10)) &&
          vm.runtime().write32(actor_matrix + 0x18U, 20U) &&
          vm.runtime().write32(actor_matrix + 0x1cU, 30U) &&
          vm.runtime().write32(second_record, 53U) &&
          vm.runtime().write16(second_record + 0x24U, 0x40U) &&
          vm.runtime().write32(
              second_record + 0x28U,
              std::bit_cast<std::uint32_t>(std::int32_t{-2})) &&
          vm.runtime().write32(
              second_record + 0x30U,
              std::bit_cast<std::uint32_t>(std::int32_t{-1})) &&
          // A recycled dynamic actor starts retired: FUN_80065fa0 clears the
          // path pointer but deliberately leaves this runtime cache intact.
          vm.runtime().write32(second_record + 0x2cU, 0U) &&
          vm.runtime().write32(second_record + 0x34U,
                               recycled_actor_instance) &&
          vm.runtime().write16(second_record + 0x3eU, 100U) &&
          vm.runtime().write16(second_record + 0x40U, 75U) &&
          vm.runtime().write16(bridge_object_definitions + 53U * 0x14U, 1U) &&
          vm.runtime().write32(recycled_actor_instance + 8U,
                               recycled_actor_node) &&
          vm.runtime().write32(recycled_actor_instance + 0x0cU,
                               recycled_actor_motion) &&
          vm.runtime().write32(recycled_actor_instance + 0x10U,
                               recycled_actor_presentation) &&
          vm.runtime().write32(recycled_actor_instance + 0x14U,
                               recycled_actor_target) &&
          vm.runtime().write32(recycled_actor_instance + 0x1cU,
                               recycled_actor_ai) &&
          vm.runtime().write32(recycled_actor_node + 8U, 0x40U) &&
          vm.runtime().write32(recycled_actor_node + 0x0cU,
                               recycled_actor_matrix) &&
          vm.runtime().write32(recycled_actor_node + 0x18U,
                               recycled_actor_bone_table) &&
          vm.runtime().write32(recycled_actor_node + 0x1cU,
                               recycled_actor_light_state) &&
          vm.runtime().write16(recycled_actor_light_state + 4U, 600U) &&
          vm.runtime().write16(recycled_actor_light_state + 6U, 700U) &&
          vm.runtime().write16(recycled_actor_light_state + 8U, 800U) &&
          vm.runtime().write16(recycled_actor_matrix, 4096U) &&
          vm.runtime().write16(recycled_actor_matrix + 8U, 4096U) &&
          vm.runtime().write16(recycled_actor_matrix + 0x10U, 4096U) &&
          vm.runtime().write32(recycled_actor_matrix + 0x14U, 701U) &&
          vm.runtime().write32(recycled_actor_matrix + 0x18U, 702U) &&
          vm.runtime().write32(recycled_actor_matrix + 0x1cU, 703U) &&
          vm.runtime().write16(recycled_actor_bone_matrix, 4096U) &&
          vm.runtime().write16(recycled_actor_bone_matrix + 8U, 4096U) &&
          vm.runtime().write16(recycled_actor_bone_matrix + 0x10U, 4096U) &&
          vm.runtime().write32(recycled_actor_bone_matrix + 0x14U, 744U) &&
          vm.runtime().write32(recycled_actor_bone_matrix + 0x18U, 755U) &&
          vm.runtime().write32(recycled_actor_bone_matrix + 0x1cU, 766U) &&
          // The display cache still contains the previous frame above. Its
          // retail coordinate hierarchy carries the current local pose.
          vm.runtime().write32(recycled_actor_bone_matrix + 0x20U,
                               recycled_actor_bone_coordinate) &&
          vm.runtime().write16(recycled_actor_bone_coordinate, 4096U) &&
          vm.runtime().write16(recycled_actor_bone_coordinate + 8U, 4096U) &&
          vm.runtime().write16(recycled_actor_bone_coordinate + 0x10U, 4096U) &&
          vm.runtime().write32(recycled_actor_bone_coordinate + 0x14U, 744U) &&
          vm.runtime().write32(recycled_actor_bone_coordinate + 0x18U, 735U) &&
          vm.runtime().write32(recycled_actor_bone_coordinate + 0x1cU, 741U) &&
          vm.runtime().write32(recycled_actor_bone_coordinate + 0x20U,
                               recycled_actor_parent_bone_matrix) &&
          vm.runtime().write8(recycled_actor_bone_coordinate + 0x2cU, 1U) &&
          vm.runtime().write32(recycled_actor_parent_bone_matrix + 0x20U,
                               recycled_actor_parent_bone_coordinate) &&
          vm.runtime().write16(recycled_actor_parent_bone_coordinate, 4096U) &&
          vm.runtime().write16(recycled_actor_parent_bone_coordinate + 8U,
                               4096U) &&
          vm.runtime().write16(recycled_actor_parent_bone_coordinate + 0x10U,
                               4096U) &&
          vm.runtime().write32(recycled_actor_parent_bone_coordinate + 0x14U,
                               15U) &&
          vm.runtime().write32(recycled_actor_parent_bone_coordinate + 0x18U,
                               20U) &&
          vm.runtime().write32(recycled_actor_parent_bone_coordinate + 0x1cU,
                               25U) &&
          vm.runtime().write32(recycled_actor_parent_bone_coordinate + 0x20U,
                               0U) &&
          vm.runtime().write8(recycled_actor_parent_bone_coordinate + 0x2cU,
                              1U) &&
          vm.runtime().write32(recycled_actor_motion + 0x12cU, 0xffffff8aU) &&
          vm.runtime().write8(recycled_actor_presentation + 8U, 1U) &&
          vm.runtime().write8(recycled_actor_presentation + 9U, 3U) &&
          vm.runtime().write16(recycled_actor_target, 0U) &&
          vm.runtime().write32(recycled_actor_target + 4U, 0x20U) &&
          vm.runtime().write16(recycled_actor_target + 0x58U, 73U) &&
          vm.runtime().write32(recycled_actor_target + 0xd4U, 0x800U) &&
          vm.runtime().write32(recycled_actor_ai + 0x20U, 0x200U) &&
          vm.runtime().write8(recycled_actor_ai + 0x43U, 0U) &&
          vm.runtime().write16(recycled_actor_first_path + 6U, 0x0400U) &&
          vm.runtime().write16(recycled_actor_second_path + 6U, 0x0100U) &&
          vm.runtime().write32(third_record, 54U) &&
          // This synthetic class-0 record sits beyond dynamic_first_slot;
          // every allocated recycled record, regardless of class, carries
          // the non-zero root/path token installed by FUN_8005f204.
          vm.runtime().write32(third_record + 0x2cU, 0x80048200U) &&
          vm.runtime().write32(third_record + 0x30U, 0xffffffffU) &&
          vm.runtime().write32(third_record + 0x34U, player_instance) &&
          vm.runtime().write16(third_record + 0x3eU, 150U) &&
          vm.runtime().write16(third_record + 0x40U, 150U) &&
          vm.runtime().write16(bridge_object_definitions + 54U * 0x14U, 0U) &&
          vm.runtime().write32(player_instance + 8U, player_node) &&
          vm.runtime().write32(player_instance + 0x0cU, player_motion) &&
          vm.runtime().write32(player_motion, 911U) &&
          vm.runtime().write32(player_motion + 4U, std::bit_cast<std::uint32_t>(
                                                       std::int32_t{-922})) &&
          vm.runtime().write32(player_motion + 8U, 933U) &&
          vm.runtime().write32(player_node + 8U, 0x40U) &&
          vm.runtime().write32(player_node + 0x0cU, player_matrix) &&
          vm.runtime().write32(player_node + 0x18U, player_bone_table) &&
          vm.runtime().write32(player_node + 0x1cU, player_light_state) &&
          vm.runtime().write16(player_light_state + 4U, 900U) &&
          vm.runtime().write16(player_light_state + 6U, 1000U) &&
          vm.runtime().write16(player_light_state + 8U, 1100U) &&
          vm.runtime().write16(player_matrix + 4U, 4096U) &&
          vm.runtime().write32(player_matrix + 0x14U, 111U) &&
          vm.runtime().write32(player_matrix + 0x18U, 222U) &&
          vm.runtime().write32(player_matrix + 0x1cU, 333U) &&
          vm.runtime().write16(player_bone_matrix, 4096U) &&
          vm.runtime().write16(player_bone_matrix + 8U, 4096U) &&
          vm.runtime().write16(player_bone_matrix + 0x10U, 4096U) &&
          vm.runtime().write32(player_bone_matrix + 0x14U, 444U) &&
          vm.runtime().write32(player_bone_matrix + 0x18U, 555U) &&
          vm.runtime().write32(player_bone_matrix + 0x1cU, 666U) &&
          // Controller 0: source-57 attached EXPL, frames 0..7.
          vm.runtime().write16(effect_controllers + 0x1cU, 2U) &&
          vm.runtime().write16(effect_controllers + 0x1eU, 0U) &&
          vm.runtime().write16(effect_controllers + 0x20U, 57U) &&
          vm.runtime().write8(effect_controllers + 0x23U, 96U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool, 1000U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool + 4U,
                               2000U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool + 8U,
                               3000U) &&
          vm.runtime().write8(bridge_profile.effect_particle_pool + 0x20U,
                              128U) &&
          vm.runtime().write8(bridge_profile.effect_particle_pool + 0x21U,
                              96U) &&
          vm.runtime().write8(bridge_profile.effect_particle_pool + 0x22U,
                              64U) &&
          vm.runtime().write16(bridge_profile.effect_particle_pool + 0x24U,
                               5U) &&
          vm.runtime().write16(bridge_profile.effect_particle_pool + 0x26U,
                               0xffffU) &&
          vm.runtime().write16(bridge_profile.effect_particle_pool + 0x60U,
                               10U) &&
          vm.runtime().write16(bridge_profile.effect_particle_pool + 0x62U,
                               9U) &&
          vm.runtime().write16(bridge_profile.effect_particle_pool + 0x64U,
                               7U) &&
          // Controller 1: full FIRE0000..0015 family.
          vm.runtime().write16(effect_controllers + 0x34U + 0x1cU, 1U) &&
          vm.runtime().write16(effect_controllers + 0x34U + 0x1eU, 1U) &&
          vm.runtime().write16(effect_controllers + 0x34U + 0x20U, 0xffffU) &&
          vm.runtime().write8(effect_controllers + 0x34U + 0x23U, 72U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool + 0x68U,
                               4000U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool + 0x68U + 4U,
                               5000U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool + 0x68U + 8U,
                               6000U) &&
          vm.runtime().write8(
              bridge_profile.effect_particle_pool + 0x68U + 0x20U, 80U) &&
          vm.runtime().write8(
              bridge_profile.effect_particle_pool + 0x68U + 0x21U, 70U) &&
          vm.runtime().write8(
              bridge_profile.effect_particle_pool + 0x68U + 0x22U, 60U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x68U + 0x24U, 4U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x68U + 0x26U, 0xffffU) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x68U + 0x60U, 8U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x68U + 0x62U, 1U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x68U + 0x64U, 15U) &&
          // Controller 2: BRETH00..15 smoke emitted by shocked actors.
          vm.runtime().write16(effect_controllers + 0x68U + 0x1cU, 3U) &&
          vm.runtime().write16(effect_controllers + 0x68U + 0x1eU, 2U) &&
          vm.runtime().write16(effect_controllers + 0x68U + 0x20U, 1U) &&
          vm.runtime().write8(effect_controllers + 0x68U + 0x23U, 48U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool + 0xd0U,
                               7000U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool + 0xd0U + 4U,
                               8000U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool + 0xd0U + 8U,
                               9000U) &&
          vm.runtime().write8(
              bridge_profile.effect_particle_pool + 0xd0U + 0x20U, 64U) &&
          vm.runtime().write8(
              bridge_profile.effect_particle_pool + 0xd0U + 0x21U, 64U) &&
          vm.runtime().write8(
              bridge_profile.effect_particle_pool + 0xd0U + 0x22U, 64U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0xd0U + 0x24U, 6U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0xd0U + 0x26U, 0xffffU) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0xd0U + 0x60U, 12U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0xd0U + 0x62U, 3U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0xd0U + 0x64U, 15U) &&
          // Controller 3: VAPOR000..007 environmental plume.
          // A negative-state controller takes its family from particle
          // +0x62, not this deliberately mismatched controller field.
          vm.runtime().write32(effect_controllers + 0x9cU, 0x80000000U) &&
          vm.runtime().write16(effect_controllers + 0x9cU + 0x1cU, 1U) &&
          vm.runtime().write16(effect_controllers + 0x9cU + 0x1eU, 3U) &&
          vm.runtime().write16(effect_controllers + 0x9cU + 0x20U, 0xffffU) &&
          vm.runtime().write8(effect_controllers + 0x9cU + 0x23U, 32U) &&
          vm.runtime().write32(bridge_profile.effect_particle_pool + 0x138U,
                               10000U) &&
          vm.runtime().write32(
              bridge_profile.effect_particle_pool + 0x138U + 4U, 11000U) &&
          vm.runtime().write32(
              bridge_profile.effect_particle_pool + 0x138U + 8U, 12000U) &&
          vm.runtime().write8(
              bridge_profile.effect_particle_pool + 0x138U + 0x20U, 96U) &&
          vm.runtime().write8(
              bridge_profile.effect_particle_pool + 0x138U + 0x21U, 96U) &&
          vm.runtime().write8(
              bridge_profile.effect_particle_pool + 0x138U + 0x22U, 96U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x138U + 0x24U, 3U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x138U + 0x26U, 0xffffU) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x138U + 0x60U, 7U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x138U + 0x62U, 4U) &&
          vm.runtime().write16(
              bridge_profile.effect_particle_pool + 0x138U + 0x64U, 7U),
      "Could not seed legacy object bridge");
  for (std::size_t part = 0U; part < sf::game::legacy_actor_bone_count;
       ++part) {
    require(vm.runtime().write32(player_bone_table +
                                     static_cast<std::uint32_t>(part * 4U),
                                 player_bone_matrix) &&
                vm.runtime().write32(recycled_actor_bone_table +
                                         static_cast<std::uint32_t>(part * 4U),
                                     recycled_actor_bone_matrix),
            "Could not seed legacy actor bone tables");
  }

  constexpr auto retail_text_hooks =
      sf::game::syphonFilterUsaV11GameplayTextHookProfile();
  static_assert(
      retail_text_hooks.message_boundaries[0].address == 0x80017530U &&
      retail_text_hooks.message_boundaries[0].instructions[0] == 0x27bdffe0U &&
      retail_text_hooks.message_boundaries[0].channel ==
          sf::game::LegacyUiMessageChannel::centered &&
      retail_text_hooks.message_boundaries[1].address == 0x80085d04U &&
      retail_text_hooks.message_boundaries[1].instructions[0] == 0x27bdffd8U &&
      retail_text_hooks.message_boundaries[1].channel ==
          sf::game::LegacyUiMessageChannel::status &&
      retail_text_hooks.message_boundaries[2].address == 0x8008582cU &&
      retail_text_hooks.message_boundaries[2].instructions ==
          std::array<std::uint32_t, 4U>{0x27bdffc8U, 0xafb20028U, 0x00a09021U,
                                        0xafb3002cU} &&
      retail_text_hooks.message_boundaries[2].channel ==
          sf::game::LegacyUiMessageChannel::centered &&
      retail_text_hooks.message_boundaries[2].text_argument == 1U &&
      retail_text_hooks.message_boundaries[2].duration_argument == 2U &&
      retail_text_hooks.message_boundaries[2].channel_from_slot &&
      retail_text_hooks.message_boundaries[2].accepted_return_address ==
          0x80044fdcU &&
      retail_text_hooks.message_boundaries[2].force_gameplay_layout &&
      retail_text_hooks.attached_text_entry == 0x80085eb0U &&
      retail_text_hooks.attached_text_instructions[0] == 0x27bdffc8U);
  constexpr std::uint32_t centered_text_hook = 0x80020200U;
  constexpr std::uint32_t status_text_hook = 0x80020240U;
  constexpr std::uint32_t attached_text_hook = 0x80020280U;
  constexpr std::uint32_t scanner_text_hook = 0x800202c0U;
  constexpr std::uint32_t scanner_text_caller = 0x80020300U;
  constexpr std::array text_hook_words{
      encodeI(0x09U, 4U, 2U, 1U),      0U, 0U, 0U,
      encodeR(31U, 0U, 0U, 0U, 0x08U), 0U,
  };
  constexpr std::array scanner_text_caller_words{
      encodeR(31U, 0U, 8U, 0U, 0x21U), encodeJ(0x03U, scanner_text_hook), 0U,
      encodeR(8U, 0U, 31U, 0U, 0x21U), encodeR(31U, 0U, 0U, 0U, 0x08U),   0U,
  };
  const auto text_hook_code = instructionBytes(text_hook_words);
  const auto scanner_text_caller_code =
      instructionBytes(scanner_text_caller_words);
  require(vm.loadOverlay(centered_text_hook, text_hook_code) &&
              vm.loadOverlay(status_text_hook, text_hook_code) &&
              vm.loadOverlay(attached_text_hook, text_hook_code) &&
              vm.loadOverlay(scanner_text_hook, text_hook_code) &&
              vm.loadOverlay(scanner_text_caller, scanner_text_caller_code),
          "Could not install retail gameplay-text hook fixtures");
  auto text_hook_profile = retail_text_hooks;
  const std::array<std::uint32_t, 4U> text_hook_prefix{
      text_hook_words[0],
      text_hook_words[1],
      text_hook_words[2],
      text_hook_words[3],
  };
  text_hook_profile.message_boundaries[0] = {
      centered_text_hook,
      text_hook_prefix,
      sf::game::LegacyUiMessageChannel::centered,
  };
  text_hook_profile.message_boundaries[1] = {
      status_text_hook,
      text_hook_prefix,
      sf::game::LegacyUiMessageChannel::status,
  };
  text_hook_profile.message_boundaries[2] = {
      scanner_text_hook,
      text_hook_prefix,
      sf::game::LegacyUiMessageChannel::centered,
      1U,
      2U,
      true,
      scanner_text_caller + 0x0cU,
      true,
  };
  text_hook_profile.attached_text_entry = attached_text_hook;
  text_hook_profile.attached_text_instructions = text_hook_prefix;
  text_hook_profile.active_text_list = bridge_profile.active_text_list;
  text_hook_profile.text_pool_cursor = 0x80049300U;
  text_hook_profile.text_object_pool = bridge_profile.text_object_pool;
  text_hook_profile.text_object_stride = bridge_profile.text_object_stride;
  text_hook_profile.text_object_capacity = bridge_profile.text_object_capacity;
  text_hook_profile.maximum_messages_per_frame = 4U;
  text_hook_profile.maximum_text_size = 64U;

  constexpr std::uint32_t centered_text_address = 0x80010600U;
  constexpr std::uint32_t status_text_address = 0x80010700U;
  constexpr std::uint32_t generic_text_address = 0x80010800U;
  constexpr std::uint32_t second_generic_text_address = 0x80010900U;
  constexpr std::uint32_t duplicate_text_address = 0x80010a00U;
  constexpr std::uint32_t scanner_text_address = 0x80010b00U;
  const auto write_guest_text = [&vm](std::uint32_t address,
                                      std::string_view text) {
    for (std::size_t index = 0U; index < text.size(); ++index) {
      if (!vm.runtime().write8(address + static_cast<std::uint32_t>(index),
                               static_cast<std::uint8_t>(text[index]))) {
        return false;
      }
    }
    return vm.runtime().write8(
        address + static_cast<std::uint32_t>(text.size()), 0U);
  };
  require(write_guest_text(centered_text_address, "Checkpoint") &&
              write_guest_text(status_text_address, "Objective Updated") &&
              write_guest_text(generic_text_address, "Open Crate") &&
              write_guest_text(second_generic_text_address, "Use Keycard") &&
              write_guest_text(duplicate_text_address, "Wrong Replacement") &&
              write_guest_text(scanner_text_address, "Plant beacon\non body") &&
              vm.runtime().write8(text_hook_profile.text_pool_cursor, 1U),
          "Could not seed gameplay-text source strings");
  vm.bindSyphonFilterUsaV11GameplayTextHooks(text_hook_profile);
  const auto centered_text_result =
      vm.invoke(centered_text_hook, std::array{centered_text_address, 73U});
  const auto status_text_result =
      vm.invoke(status_text_hook, std::array{status_text_address, 41U});
  require(centered_text_result.completed() &&
              centered_text_result.return_value == centered_text_address + 1U &&
              status_text_result.completed() &&
              status_text_result.return_value == status_text_address + 1U,
          "Gameplay message hooks did not preserve guest execution");

  const auto generic_text_result =
      vm.invoke(attached_text_hook,
                std::array{0U, generic_text_address, 0xffffffffU, 0U});
  constexpr std::uint32_t generic_text_node = attached_text_node + 0x20U;
  constexpr std::uint32_t generic_text_object = attached_text_object + 0x1cU;
  require(
      generic_text_result.completed() &&
          generic_text_result.return_value == 1U &&
          vm.runtime().write32(attached_text_node + 8U, generic_text_node) &&
          vm.runtime().write32(generic_text_node, generic_text_object) &&
          vm.runtime().write32(generic_text_node + 8U, 0U) &&
          vm.runtime().write8(generic_text_object + 0x14U, 0x03U) &&
          // FUN_8008507c writes the additive source-string checksum here.
          vm.runtime().write8(generic_text_object + 0x15U, 161U) &&
          vm.runtime().write16(generic_text_object + 0x16U, 0U),
      "Attached-text hook did not retain the predicted retail pool object");

  const auto gameplay_text_snapshot = vm.captureSnapshot();
  require(
      gameplay_text_snapshot.ui_messages.size() == 2U &&
          gameplay_text_snapshot.ui_messages[0].channel ==
              sf::game::LegacyUiMessageChannel::centered &&
          gameplay_text_snapshot.ui_messages[0].text == "Checkpoint" &&
          gameplay_text_snapshot.ui_messages[0].duration == 73U &&
          gameplay_text_snapshot.ui_messages[1].channel ==
              sf::game::LegacyUiMessageChannel::status &&
          gameplay_text_snapshot.ui_messages[1].text == "Objective Updated" &&
          gameplay_text_snapshot.ui_messages[1].duration == 41U &&
          gameplay_text_snapshot.attached_text_sources.size() == 1U &&
          gameplay_text_snapshot.attached_text_sources[0].text_object ==
              generic_text_object &&
          gameplay_text_snapshot.attached_text_sources[0].text_checksum ==
              161U &&
          gameplay_text_snapshot.attached_text_sources[0].text == "Open Crate",
      "Gameplay-text hooks exported different text, timing or pool identity");

  vm.clearUiMessages();
  const auto second_generic_result =
      vm.invoke(attached_text_hook,
                std::array{2U, second_generic_text_address, 0xffffffffU, 0U});
  const auto mutated_text_state = vm.captureSnapshot();
  require(second_generic_result.completed() &&
              mutated_text_state.ui_messages.empty() &&
              mutated_text_state.attached_text_sources.size() == 2U &&
              vm.restoreSnapshot(gameplay_text_snapshot),
          "Gameplay-text checkpoint mutation or restore failed");
  const auto restored_text_state = vm.captureSnapshot();
  require(restored_text_state.ui_messages ==
                  gameplay_text_snapshot.ui_messages &&
              restored_text_state.attached_text_sources.size() == 1U &&
              restored_text_state.attached_text_sources[0].text_object ==
                  generic_text_object &&
              restored_text_state.attached_text_sources[0].text == "Open Crate",
          "Gameplay-text checkpoint did not restore exact host text state");

  const auto duplicate_text_result =
      vm.invoke(attached_text_hook,
                std::array{0U, duplicate_text_address, 0xffffffffU, 0U});
  const auto duplicate_text_state = vm.captureSnapshot();
  require(duplicate_text_result.completed() &&
              duplicate_text_state.attached_text_sources.size() == 1U &&
              duplicate_text_state.attached_text_sources[0].text ==
                  "Open Crate",
          "Duplicate attached-text call overwrote the live retail label");

  constexpr std::uint32_t malformed_text_address = 0x10000000U;
  const auto malformed_text_result =
      vm.invoke(centered_text_hook, std::array{malformed_text_address, 99U});
  require(malformed_text_result.completed() &&
              vm.captureSnapshot().ui_messages.size() == 2U,
          "Gameplay message observer faulted retail on an unreadable string");
  require(vm.runtime().write32(status_text_hook, 0U),
          "Could not corrupt gameplay-text opcode fixture");
  const auto mismatched_text_result =
      vm.invoke(status_text_hook, std::array{status_text_address, 99U});
  require(mismatched_text_result.completed() &&
              vm.captureSnapshot().ui_messages.size() == 2U &&
              vm.loadOverlay(status_text_hook, text_hook_code),
          "Gameplay message observer faulted retail on an unknown opcode");

  vm.clearUiMessages();
  const std::array scanner_text_arguments{2U, scanner_text_address, 0xffffffffU,
                                          0U};
  const auto scanner_text_result =
      vm.invoke(scanner_text_caller, scanner_text_arguments);
  const auto scanner_text_state = vm.captureSnapshot();
  require(scanner_text_result.completed() &&
              scanner_text_result.return_value == 3U &&
              scanner_text_state.ui_messages.size() == 1U &&
              scanner_text_state.ui_messages[0].channel ==
                  sf::game::LegacyUiMessageChannel::centered &&
              scanner_text_state.ui_messages[0].text ==
                  "Plant beacon\non body" &&
              scanner_text_state.ui_messages[0].force_gameplay_layout &&
              scanner_text_state.ui_messages[0].duration == 0xffffffffU,
          "Scanner message hook lost its source, slot channel or duration");
  const auto rejected_scanner_text_result =
      vm.invoke(scanner_text_hook, scanner_text_arguments);
  require(rejected_scanner_text_result.completed() &&
              rejected_scanner_text_result.return_value == 3U &&
              vm.captureSnapshot().ui_messages.size() == 1U,
          "Scanner message hook accepted an unrelated retail caller");
  const auto scanner_status_result = vm.invoke(
      scanner_text_caller, std::array{6U, scanner_text_address, 29U, 0U});
  const auto scanner_status_state = vm.captureSnapshot();
  require(scanner_status_result.completed() &&
              scanner_status_result.return_value == 7U &&
              scanner_status_state.ui_messages.size() == 2U &&
              scanner_status_state.ui_messages[1].channel ==
                  sf::game::LegacyUiMessageChannel::status &&
              scanner_status_state.ui_messages[1].duration == 29U,
          "Slot-derived gameplay message channel was not preserved");
  const auto invalid_scanner_slot_result = vm.invoke(
      scanner_text_caller, std::array{7U, scanner_text_address, 17U, 0U});
  require(invalid_scanner_slot_result.completed() &&
              invalid_scanner_slot_result.return_value == 8U &&
              vm.captureSnapshot().ui_messages.size() == 2U,
          "Gameplay message hook accepted an invalid retail text slot");
  const auto aliased_scanner_slot_result = vm.invoke(
      scanner_text_caller, std::array{0x100U, scanner_text_address, 17U, 0U});
  require(aliased_scanner_slot_result.completed() &&
              aliased_scanner_slot_result.return_value == 0x101U &&
              vm.captureSnapshot().ui_messages.size() == 2U,
          "Gameplay message hook truncated an invalid slot into slot zero");
  vm.clearUiMessages();

  constexpr std::uint32_t thrown_projectile_descriptor = 0x8004a540U;
  constexpr std::uint32_t thrown_projectile_object = 0x8004a580U;
  constexpr std::uint32_t thrown_projectile_matrix =
      thrown_projectile_object + 0x1cU;
  constexpr std::uint32_t enemy_projectile_descriptor = 0x8004a5c0U;
  constexpr std::uint32_t enemy_projectile_object = 0x8004a600U;
  constexpr std::uint32_t enemy_projectile_matrix =
      enemy_projectile_object + 0x1cU;
  require(
      vm.runtime().write32(bridge_profile.player_thrown_projectile_pointer,
                           thrown_projectile_descriptor) &&
          vm.runtime().write8(thrown_projectile_descriptor + 1U, 7U) &&
          vm.runtime().write32(thrown_projectile_descriptor + 4U, 19U) &&
          vm.runtime().write32(thrown_projectile_descriptor + 8U,
                               thrown_projectile_object) &&
          // Display+0 is an opaque non-zero retail ownership token. It is
          // only a liveness test and is not necessarily a readable pointer.
          vm.runtime().write32(thrown_projectile_object, 1U) &&
          vm.runtime().write16(thrown_projectile_matrix, 4096U) &&
          vm.runtime().write16(thrown_projectile_matrix + 8U, 4096U) &&
          vm.runtime().write16(thrown_projectile_matrix + 0x10U, 4096U) &&
          vm.runtime().write32(thrown_projectile_matrix + 0x14U, 4569U) &&
          vm.runtime().write32(
              thrown_projectile_matrix + 0x18U,
              std::bit_cast<std::uint32_t>(std::int32_t{-2492})) &&
          vm.runtime().write32(thrown_projectile_matrix + 0x1cU, 3160U) &&
          vm.runtime().write32(bridge_profile.enemy_thrown_projectile_pointer,
                               enemy_projectile_descriptor) &&
          vm.runtime().write8(enemy_projectile_descriptor + 1U, 9U) &&
          vm.runtime().write32(enemy_projectile_descriptor + 4U, 20U) &&
          vm.runtime().write32(enemy_projectile_descriptor + 8U,
                               enemy_projectile_object) &&
          vm.runtime().write32(enemy_projectile_object, 1U) &&
          vm.runtime().write16(enemy_projectile_matrix, 4096U) &&
          vm.runtime().write16(enemy_projectile_matrix + 8U, 4096U) &&
          vm.runtime().write16(enemy_projectile_matrix + 0x10U, 4096U) &&
          vm.runtime().write32(enemy_projectile_matrix + 0x14U, 4660U) &&
          vm.runtime().write32(
              enemy_projectile_matrix + 0x18U,
              std::bit_cast<std::uint32_t>(std::int32_t{-2500})) &&
          vm.runtime().write32(enemy_projectile_matrix + 0x1cU, 3200U),
      "Could not seed the retail player/enemy in-flight grenade descriptors");
  const auto bridge = vm.readBridgeState(bridge_profile);
  require(
      bridge && bridge->camera.eye.x == 100 && bridge->camera.eye.y == -200 &&
          bridge->camera.eye.z == 300 && bridge->camera.target.x == 400 &&
          bridge->camera.target.y == 500 && bridge->camera.target.z == 600 &&
          bridge->camera.projection == 444 &&
          bridge->camera.projectionForDisplayWidth(384) == 261 &&
          bridge->camera.fov_raw == 827 && bridge->camera.mode == 0x0b &&
          bridge->camera.scripted && bridge->camera.locked &&
          bridge->camera.presentation_viewport_y == 0 &&
          bridge->camera.presentation_viewport_height == 240 &&
          !bridge->camera.retail_letterbox_active &&
          bridge->environment.clear_color ==
              sf::game::LegacyRgbBridgeState{7U, 11U, 13U} &&
          bridge->environment.back_color ==
              sf::game::LegacyRgbBridgeState{23U, 29U, 31U} &&
          bridge->environment.fog_color ==
              sf::game::LegacyRgbBridgeState{41U, 43U, 47U} &&
          bridge->environment.fog_dqa == -123 &&
          bridge->environment.fog_dqb == 0x12345000 &&
          bridge->environment.active_terrain_depth_cue ==
              ((5U << 16U) | 0x0800U) &&
          bridge->environment.terrain_depth_cue == ((2U << 16U) | 0x0321U) &&
          bridge->environment.renderer_display_flags == 0x00a4U &&
          bridge->environment.renderer_flags == 5U &&
          !bridge->environment.renderer_darkness_enabled &&
          bridge->environment.effectiveTerrainDepthCue() ==
              ((2U << 16U) | 0x0321U) &&
          bridge->environment.background_enabled &&
          bridge->environment.screen_filter_enabled &&
          bridge->environment.screen_filter_material == 0U &&
          bridge->environment.screen_filter_color ==
              sf::game::LegacyRgbBridgeState{47U, 112U, 77U} &&
          !bridge->environment.nightvision_enabled &&
          bridge->environment.nightvision_clear_color ==
              sf::game::LegacyRgbBridgeState{0U, 255U, 0U} &&
          bridge->renderer_sprite_fast_path && bridge->thrown_projectile &&
          bridge->thrown_projectile->age == 7U &&
          bridge->thrown_projectile->weapon == 19U &&
          bridge->thrown_projectile->transform.translation ==
              sf::game::LegacyNativePoint{4569, -2492, 3160} &&
          bridge->enemy_thrown_projectile &&
          bridge->enemy_thrown_projectile->age == 9U &&
          bridge->enemy_thrown_projectile->weapon == 20U &&
          bridge->enemy_thrown_projectile->transform.translation ==
              sf::game::LegacyNativePoint{4660, -2500, 3200} &&
          bridge->guest_camera_lists_captured &&
          bridge->guest_sprites.size() == 1U &&
          bridge->guest_sprites[0].tpage == 31U &&
          bridge->guest_sprites[0].u == 64U &&
          bridge->guest_sprites[0].v == 96U &&
          bridge->guest_sprites[0].ordering_depth == 77U &&
          bridge->guest_sprites[0].effect_particle == 0 &&
          bridge->guest_sprites[0].effect_family == 2U &&
          bridge->guest_sprites[0].effect_frame == 4U &&
          bridge->guest_sprites[0].effect_position.x == 1000 &&
          bridge->guest_sprites[0].effect_position.y == 2000 &&
          bridge->guest_sprites[0].effect_position.z == 3000 &&
          bridge->guest_lines.size() == 1U &&
          bridge->guest_lines[0].first ==
              sf::game::LegacyProjectedPointBridgeState{11, 12} &&
          bridge->guest_lines[0].second_color ==
              sf::game::LegacyRgbBridgeState{4U, 5U, 6U} &&
          bridge->guest_raw_packets.size() == 1U &&
          bridge->guest_raw_packets[0].word_count == 6U &&
          bridge->guest_raw_packets[0].opcode == 0x30U &&
          bridge->guest_raw_packets[0].ordering_depth == 0U &&
          bridge->guest_raw_packets[0].effect_particle == 1 &&
          bridge->guest_raw_packets[0].effect_world_position_valid &&
          bridge->guest_raw_packets[0].effect_position.x == 4000 &&
          bridge->guest_raw_packets[0].effect_position.y == 5000 &&
          bridge->guest_raw_packets[0].effect_position.z == 6000 &&
          bridge->environment.fogEnabled() && bridge->fade.step == -8 &&
          bridge->fade.current == 135U && bridge->fade.initialized &&
          bridge->fade.blackOpacity() == 0.5 && bridge->player.control_locked &&
          bridge->player.room == -1 && bridge->world_model_count == 5U &&
          bridge->flashlight_enabled && bridge->vertex_lights.size() == 1U &&
          bridge->vertex_lights[0].source ==
              bridge_profile.flashlight_enabled &&
          bridge->vertex_lights[0].flags == 1U &&
          bridge->vertex_lights[0].matrix.translation.x == 101 &&
          bridge->vertex_lights[0].matrix.translation.y == 202 &&
          bridge->vertex_lights[0].matrix.translation.z == 303 &&
          bridge->vertex_lights[0].shape == 0x50 &&
          bridge->vertex_lights[0].screen_shift == 0x0eU &&
          bridge->vertex_lights[0].depth_shift == 6U &&
          bridge->vertex_lights[0].channel_mask == 0x00ffffffU &&
          bridge->world_vertex_colors.size() == 5U &&
          bridge->world_vertex_colors[0].model == 1U &&
          bridge->world_vertex_colors[1].model == 3U &&
          bridge->world_vertex_colors[2].model == 0U &&
          bridge->world_vertex_colors[3].model == 4U &&
          bridge->world_vertex_colors[0].section == 0U &&
          bridge->world_vertex_colors[0].colors ==
              std::vector<std::uint16_t>({0x0421U, 0x0842U, 0x0c63U}) &&
          bridge->world_vertex_colors.back().model == 2U &&
          bridge->world_vertex_colors.back().colors ==
              std::vector<std::uint16_t>({0x0421U, 0x0842U, 0x0c63U}) &&
          bridge->active_world_models == std::vector<std::uint16_t>({1U, 3U}) &&
          bridge->resident_world_models ==
              std::vector<std::uint16_t>({0U, 4U}) &&
          bridge->player.resident && bridge->player.position.x == 911 &&
          bridge->player.position.y == 922 &&
          bridge->player.position.z == 933 &&
          bridge->player.guest_rotation[2] == 4096 &&
          bridge->target_lock_active && bridge->taser_conductor_phase == 2U &&
          bridge->virus_scanner_target_valid &&
          bridge->virus_scanner_target.x == 1234 &&
          bridge->virus_scanner_target.y == -567 &&
          bridge->virus_scanner_target.z == 890 &&
          bridge->virus_scanner_target_slot == 1 &&
          bridge->taser_target_slot == 1 && bridge->taserConductorActive() &&
          bridge->target_hit_result == 0x8004a000U &&
          bridge->aimed_target_slot == 0 &&
          bridge->proximity_target_slot == 0 &&
          bridge->world_callouts.size() == 2U &&
          bridge->world_callouts[0].guest_slot == 1 &&
          bridge->world_callouts[0].headshot &&
          bridge->world_callouts[0].text == "Head Shot" &&
          bridge->world_callouts[1].guest_slot == 0 &&
          !bridge->world_callouts[1].headshot &&
          bridge->world_callouts[1].text == "Open Crate" &&
          bridge->tracked_slots[0] == 1 && bridge->tracked_slots[1] == 0 &&
          bridge->tracked_slots[2] == -1 && bridge->tracked_slots[3] == -1 &&
          bridge->tracked_slots[4] == -1 && bridge->tracked_slots[5] == -1 &&
          bridge->dynamic_first_slot == 1U && bridge->objects.size() == 3U &&
          bridge->objects[0].class_id == 0x35 &&
          bridge->objects[0].object_handler == legacy_common_npc_handler &&
          bridge->objects[0].attributes == 0x21U &&
          bridge->objects[0].parameter == 7 &&
          bridge->objects[0].linked_slot == 53 && bridge->objects[0].alive() &&
          bridge->objects[0].instance_flags == 0x40U &&
          !bridge->objects[0].destroyed() &&
          bridge->objects[0].position.x == -10 &&
          bridge->objects[0].position.y == 20 &&
          bridge->objects[0].position.z == 30 &&
          bridge->objects[0].guest_rotation[0] == 4096 &&
          bridge->objects[0].hmd_back_color_valid &&
          bridge->objects[0].hmd_back_color_q12 ==
              std::array<std::int16_t, 3U>{300, 400, 500} &&
          !bridge->objects[0].ground_contact_valid &&
          bridge->objects[1].class_id == 1 &&
          bridge->objects[1].object_handler == 0U &&
          bridge->objects[1].attributes == 0x40U &&
          bridge->objects[1].parameter == -2 &&
          bridge->objects[1].linked_slot == -1 &&
          bridge->objects[1].health == 75 && !bridge->objects[1].resident &&
          !bridge->objects[1].alive() && !bridge->objects[1].simulated &&
          !bridge->objects[1].has_target &&
          bridge->objects[1].target_flags == 0U &&
          bridge->objects[1].target_meter == 0 &&
          bridge->objects[1].danger_q12 == 0U &&
          bridge->objects[1].bone_matrix_count == 0U &&
          !bridge->objects[1].ground_contact_valid &&
          bridge->objects[2].class_id == 0 &&
          bridge->objects[2].object_handler == 0x80060000U &&
          bridge->objects[2].bone_matrix_count ==
              sf::game::legacy_actor_bone_count &&
          bridge->objects[2].bone_matrices[0].translation.x == 444 &&
          bridge->objects[2].bone_matrices[0].translation.y == 555 &&
          bridge->objects[2].bone_matrices[0].translation.z == 666 &&
          bridge->objects[2].hmd_back_color_valid &&
          bridge->objects[2].hmd_back_color_q12 ==
              std::array<std::int16_t, 3U>{900, 1000, 1100} &&
          bridge->expl_particles.size() == 4U &&
          bridge->expl_particles[0].pool_index == 0 &&
          bridge->expl_particles[0].controller == 0U &&
          bridge->expl_particles[0].source_slot == 57 &&
          bridge->expl_particles[0].family == 2U &&
          bridge->expl_particles[0].scale_byte == 96U &&
          bridge->expl_particles[0].frame == 4U &&
          bridge->expl_particles[1].pool_index == 1 &&
          bridge->expl_particles[1].controller == 1U &&
          bridge->expl_particles[1].source_slot == -1 &&
          bridge->expl_particles[1].family == 1U &&
          bridge->expl_particles[1].scale_byte == 72U &&
          bridge->expl_particles[1].frame == 8U &&
          bridge->expl_particles[2].pool_index == 2 &&
          bridge->expl_particles[2].controller == 2U &&
          bridge->expl_particles[2].source_slot == 1 &&
          bridge->expl_particles[2].family == 3U &&
          bridge->expl_particles[2].scale_byte == 48U &&
          bridge->expl_particles[2].frame == 8U &&
          bridge->expl_particles[3].pool_index == 3 &&
          bridge->expl_particles[3].controller == 3U &&
          bridge->expl_particles[3].source_slot == -1 &&
          bridge->expl_particles[3].family == 4U &&
          bridge->expl_particles[3].scale_byte == 32U &&
          bridge->expl_particles[3].frame == 4U &&
          bridge->park2_flamethrower_ribbons.empty(),
      "Legacy typed gameplay bridge mismatch");

  require(
      vm.runtime().write16(presentation_viewport +
                               bridge_profile.presentation_viewport_y_offset,
                           40U) &&
          vm.runtime().write16(
              presentation_viewport +
                  bridge_profile.presentation_viewport_height_offset,
              160U),
      "Could not seed the retail radio viewport");
  const auto radio_viewport_bridge = vm.readBridgeState(bridge_profile);
  require(radio_viewport_bridge &&
              radio_viewport_bridge->camera.presentation_viewport_y == 40 &&
              radio_viewport_bridge->camera.presentation_viewport_height ==
                  160 &&
              radio_viewport_bridge->camera.retail_letterbox_active,
          "The exact retail radio viewport did not own letterbox state");
  require(
      vm.runtime().write16(presentation_viewport +
                               bridge_profile.presentation_viewport_y_offset,
                           0U) &&
          vm.runtime().write16(
              presentation_viewport +
                  bridge_profile.presentation_viewport_height_offset,
              240U),
      "Could not restore the gameplay viewport");

  require(
      vm.runtime().write32(bridge_profile.virus_scanner_target, 0U) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target + 4U, 0U) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target + 8U, 0U) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target_slot, 99U),
      "Could not seed cleared virus-scanner target state");
  const auto cleared_scanner_target = vm.readBridgeState(bridge_profile);
  require(cleared_scanner_target &&
              !cleared_scanner_target->virus_scanner_target_valid &&
              cleared_scanner_target->virus_scanner_target_slot == -1,
          "A stale scanner slot survived cleared retail coordinates");
  require(vm.runtime().write32(bridge_profile.virus_scanner_target, 1U),
          "Could not seed transient invalid virus-scanner target state");
  const auto transient_scanner_target = vm.readBridgeState(bridge_profile);
  require(transient_scanner_target &&
              !transient_scanner_target->virus_scanner_target_valid &&
              transient_scanner_target->virus_scanner_target_slot == -1,
          "A transient invalid scanner slot faulted the gameplay bridge");
  require(
      vm.runtime().write32(bridge_profile.virus_scanner_target, 1234U) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target + 4U,
                               std::bit_cast<std::uint32_t>(-567)) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target + 8U,
                               890U) &&
          vm.runtime().write32(bridge_profile.virus_scanner_target_slot, 1U),
      "Could not restore virus-scanner target state");

  auto park2_bridge_profile = bridge_profile;
  auto &park2_flame = park2_bridge_profile.park2_flamethrower;
  park2_flame.enabled = true;
  constexpr std::uint32_t park2_validation_words = 0x80052000U;
  constexpr std::uint32_t park2_packet_pool = 0x80053000U;
  constexpr std::uint32_t park2_packet_node = 0x80054000U;
  constexpr std::uint32_t park2_next_packet = park2_packet_pool + 0x30U;
  constexpr std::uint32_t park2_next_packet_node = park2_packet_node + 4U;
  constexpr std::uint32_t park2_current_object = 0x80055000U;
  constexpr std::uint32_t park2_next_object = 0x80055100U;
  park2_flame.packet_pool = park2_packet_pool;
  for (std::size_t index = 0U; index < park2_flame.validation_words.size();
       ++index) {
    auto &validation = park2_flame.validation_words[index];
    validation.address =
        park2_validation_words + static_cast<std::uint32_t>(index * 4U);
    require(vm.runtime().write32(validation.address, validation.expected),
            "Could not seed PARK2 overlay validation word");
  }
  const auto pack_projected = [](std::int16_t x, std::int16_t y) {
    return static_cast<std::uint32_t>(
        static_cast<std::uint16_t>(x) |
        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16U));
  };
  require(
      vm.runtime().write32(park2_flame.state_pool, park2_current_object) &&
          vm.runtime().write16(park2_flame.state_pool + 4U, 4U) &&
          vm.runtime().write16(park2_flame.state_pool + 6U,
                               static_cast<std::uint16_t>(-6)) &&
          vm.runtime().write16(park2_flame.state_pool + 8U, 8U) &&
          vm.runtime().write32(park2_flame.state_pool +
                                   park2_flame.state_stride,
                               park2_next_object) &&
          vm.runtime().write32(park2_current_object + 0x14U, 100U) &&
          vm.runtime().write32(park2_current_object + 0x18U, 200U) &&
          vm.runtime().write32(park2_current_object + 0x1cU, 300U) &&
          vm.runtime().write32(park2_next_object + 0x14U, 120U) &&
          vm.runtime().write32(park2_next_object + 0x18U, 220U) &&
          vm.runtime().write32(park2_next_object + 0x1cU, 320U) &&
          vm.runtime().write32(park2_flame.width_history_pool, 1U) &&
          vm.runtime().write32(park2_packet_pool, park2_packet_node) &&
          vm.runtime().write32(park2_packet_node, park2_packet_pool) &&
          vm.runtime().write32(park2_packet_pool + 0x04U, 300U) &&
          // addPrim may replace the low 24 tag bits; the POLY_FT4 word count
          // in the high byte remains the immutable packet-layout guard.
          vm.runtime().write32(park2_packet_pool + 0x08U, 0x09123456U) &&
          vm.runtime().write32(park2_packet_pool + 0x0cU, 0x2e776655U) &&
          vm.runtime().write32(park2_packet_pool + 0x10U,
                               pack_projected(10, 20)) &&
          vm.runtime().write16(park2_packet_pool + 0x14U, 0x0040U) &&
          vm.runtime().write16(park2_packet_pool + 0x16U, 0x7ff0U) &&
          vm.runtime().write32(park2_packet_pool + 0x18U,
                               pack_projected(41, 20)) &&
          vm.runtime().write16(park2_packet_pool + 0x1cU, 0x005fU) &&
          vm.runtime().write16(park2_packet_pool + 0x1eU, 0x00bcU) &&
          vm.runtime().write32(park2_packet_pool + 0x20U,
                               pack_projected(10, 51)) &&
          vm.runtime().write16(park2_packet_pool + 0x24U, 0x1f40U) &&
          vm.runtime().write32(park2_packet_pool + 0x28U,
                               pack_projected(41, 51)) &&
          vm.runtime().write16(park2_packet_pool + 0x2cU, 0x1f5fU),
      "Could not seed the PARK2 Girdeux POLY_FT4 packet");
  const auto park2_bridge = vm.readBridgeState(park2_bridge_profile);
  require(
      park2_bridge && park2_bridge->park2_flamethrower_ribbons.size() == 1U &&
          park2_bridge->park2_flamethrower_ribbons[0].corners[0] ==
              sf::game::LegacyProjectedPointBridgeState{10, 20} &&
          park2_bridge->park2_flamethrower_ribbons[0].corners[3] ==
              sf::game::LegacyProjectedPointBridgeState{41, 51} &&
          park2_bridge->park2_flamethrower_ribbons[0].color ==
              sf::game::LegacyRgbBridgeState{0x55U, 0x66U, 0x77U} &&
          park2_bridge->park2_flamethrower_ribbons[0].ordering_depth == 300U &&
          park2_bridge->park2_flamethrower_ribbons[0].slot == 0U &&
          park2_bridge->park2_flamethrower_ribbons[0].frame == 2U &&
          park2_bridge->park2_flamethrower_ribbons[0].world_first ==
              sf::game::LegacyNativePoint{120, 220, 320} &&
          park2_bridge->park2_flamethrower_ribbons[0].world_second ==
              sf::game::LegacyNativePoint{102, 203, 304} &&
          park2_bridge->park2_flamethrower_ribbons[0].width_shift == 2U,
      "PARK2 Girdeux flame-ribbon bridge mismatch");
  require(vm.runtime().write16(
              park2_flame.state_pool + park2_flame.state_stride + 4U, 10U) &&
              vm.runtime().write16(park2_flame.state_pool +
                                       park2_flame.state_stride + 6U,
                                   12U) &&
              vm.runtime().write16(park2_flame.state_pool +
                                       park2_flame.state_stride + 8U,
                                   static_cast<std::uint16_t>(-14)) &&
              vm.runtime().write32(park2_next_packet, park2_next_packet_node) &&
              vm.runtime().write32(park2_next_packet_node, park2_next_packet) &&
              vm.runtime().write32(park2_next_packet + 0x04U, 310U) &&
              vm.runtime().write32(park2_next_packet + 0x08U, 0x09123456U) &&
              vm.runtime().write32(park2_next_packet + 0x0cU, 0x2e776655U) &&
              vm.runtime().write32(park2_next_packet + 0x10U,
                                   pack_projected(42, 21)) &&
              vm.runtime().write16(park2_next_packet + 0x14U, 0x0060U) &&
              vm.runtime().write16(park2_next_packet + 0x16U, 0x7ff0U) &&
              vm.runtime().write32(park2_next_packet + 0x18U,
                                   pack_projected(73, 21)) &&
              vm.runtime().write16(park2_next_packet + 0x1cU, 0x007fU) &&
              vm.runtime().write16(park2_next_packet + 0x1eU, 0x00bcU) &&
              vm.runtime().write32(park2_next_packet + 0x20U,
                                   pack_projected(42, 52)) &&
              vm.runtime().write16(park2_next_packet + 0x24U, 0x1f60U) &&
              vm.runtime().write32(park2_next_packet + 0x28U,
                                   pack_projected(73, 52)) &&
              vm.runtime().write16(park2_next_packet + 0x2cU, 0x1f7fU),
          "Could not seed the active PARK2 successor packet");
  const auto park2_active_successor = vm.readBridgeState(park2_bridge_profile);
  require(
      park2_active_successor &&
          park2_active_successor->park2_flamethrower_ribbons.size() == 2U &&
          park2_active_successor->park2_flamethrower_ribbons[0].world_first ==
              sf::game::LegacyNativePoint{110, 232, 334} &&
          park2_active_successor->park2_flamethrower_ribbons[1].world_first ==
              sf::game::LegacyNativePoint{110, 232, 334} &&
          park2_active_successor->park2_flamethrower_ribbons[1].world_second ==
              sf::game::LegacyNativePoint{125, 214, 313} &&
          vm.runtime().write32(park2_next_packet, 0U) &&
          vm.runtime().write16(
              park2_flame.state_pool + park2_flame.state_stride + 4U, 0U) &&
          vm.runtime().write16(
              park2_flame.state_pool + park2_flame.state_stride + 6U, 0U) &&
          vm.runtime().write16(
              park2_flame.state_pool + park2_flame.state_stride + 8U, 0U),
      "PARK2 bridge did not rewind the post-step active successor");
  require(vm.runtime().write32(park2_next_object + 0x14U, 400U) &&
              vm.runtime().write32(park2_next_object + 0x18U, 500U) &&
              vm.runtime().write32(park2_next_object + 0x1cU, 600U),
          "Could not seed the PARK2 stretched neighbour");
  const auto park2_clamped_bridge = vm.readBridgeState(park2_bridge_profile);
  require(park2_clamped_bridge &&
              park2_clamped_bridge->park2_flamethrower_ribbons.size() == 1U &&
              park2_clamped_bridge->park2_flamethrower_ribbons[0].world_first ==
                  sf::game::LegacyNativePoint{172, 270, 369} &&
              vm.runtime().write32(park2_next_object + 0x14U, 120U) &&
              vm.runtime().write32(park2_next_object + 0x18U, 220U) &&
              vm.runtime().write32(park2_next_object + 0x1cU, 320U),
          "PARK2 bridge did not preserve the retail neighbour clamp");
  require(vm.runtime().write32(
              park2_flame.state_pool + park2_flame.state_stride, 0U),
          "Could not unlink the PARK2 neighbour state");
  const auto park2_fallback_bridge = vm.readBridgeState(park2_bridge_profile);
  require(
      park2_fallback_bridge &&
          park2_fallback_bridge->park2_flamethrower_ribbons.size() == 1U &&
          park2_fallback_bridge->park2_flamethrower_ribbons[0].world_first ==
              sf::game::LegacyNativePoint{96, 194, 292} &&
          vm.runtime().write32(park2_flame.state_pool +
                                   park2_flame.state_stride,
                               park2_next_object),
      "PARK2 bridge lost the retail unlinked-neighbour fallback");
  require(vm.runtime().write32(park2_packet_pool + 0x04U, 0U),
          "Could not seed the PARK2 transient zero-depth packet");
  const auto park2_transient_bridge = vm.readBridgeState(park2_bridge_profile);
  require(park2_transient_bridge &&
              park2_transient_bridge->park2_flamethrower_ribbons.empty() &&
              vm.runtime().write32(park2_packet_pool + 0x04U, 300U),
          "PARK2 bridge rejected a linked packet before OT depth commit");
  require(
      vm.runtime().write32(park2_packet_pool + 0x10U, pack_projected(1024, 20)),
      "Could not seed the PARK2 clipped packet");
  const auto park2_clipped_bridge = vm.readBridgeState(park2_bridge_profile);
  require(park2_clipped_bridge &&
              park2_clipped_bridge->park2_flamethrower_ribbons.empty() &&
              vm.runtime().write32(park2_packet_pool + 0x10U,
                                   pack_projected(10, 20)),
          "PARK2 bridge rejected a GTE-clipped ribbon corner");
  require(vm.runtime().write16(park2_packet_pool + 0x14U, 0x0041U) &&
              !vm.readBridgeState(park2_bridge_profile) &&
              vm.runtime().write16(park2_packet_pool + 0x14U, 0x0040U),
          "PARK2 bridge accepted a damaged EXPL material");
  require(vm.runtime().write32(park2_flame.validation_words[0].address,
                               park2_flame.validation_words[0].expected ^ 1U) &&
              !vm.readBridgeState(park2_bridge_profile) &&
              vm.runtime().write32(park2_flame.validation_words[0].address,
                                   park2_flame.validation_words[0].expected),
          "PARK2 bridge accepted a mismatched overlay descriptor");

  auto line_bridge_profile = bridge_profile;
  line_bridge_profile.effect_controller_pool_pointer = 0x80032200U;
  line_bridge_profile.effect_controller_count = 0x80032204U;
  line_bridge_profile.effect_particle_pool = 0x80050000U;
  line_bridge_profile.effect_particle_capacity = 2U;
  constexpr std::uint32_t line_controllers = 0x80051000U;
  constexpr std::uint32_t second_line_controller = line_controllers + 0x34U;
  constexpr std::uint32_t second_line_particle = 0x80050000U + 0x68U;
  require(
      vm.runtime().write32(line_bridge_profile.effect_controller_pool_pointer,
                           line_controllers) &&
          vm.runtime().write16(line_bridge_profile.effect_controller_count,
                               2U) &&
          // FUN_800558c0: opaque ballistic LINE_G2, compact endpoint at
          // particle +0x50.
          vm.runtime().write16(line_controllers + 0x1eU, 0U) &&
          vm.runtime().write16(line_controllers + 0x20U, 0xffffU) &&
          vm.runtime().write32(line_controllers + 0x2cU, 4U) &&
          vm.runtime().write32(line_controllers + 0x30U, 3U) &&
          vm.runtime().write32(line_bridge_profile.effect_particle_pool,
                               1000U) &&
          vm.runtime().write32(line_bridge_profile.effect_particle_pool + 4U,
                               2000U) &&
          vm.runtime().write32(line_bridge_profile.effect_particle_pool + 8U,
                               3000U) &&
          vm.runtime().write16(line_bridge_profile.effect_particle_pool + 0x24U,
                               5U) &&
          vm.runtime().write16(line_bridge_profile.effect_particle_pool + 0x26U,
                               0xffffU) &&
          vm.runtime().write32(line_bridge_profile.effect_particle_pool + 0x34U,
                               0x500096c8U) &&
          vm.runtime().write32(line_bridge_profile.effect_particle_pool + 0x3cU,
                               0x000096c8U) &&
          vm.runtime().write16(
              line_bridge_profile.effect_particle_pool + 0x50U,
              std::bit_cast<std::uint16_t>(std::int16_t{-100})) &&
          vm.runtime().write16(line_bridge_profile.effect_particle_pool + 0x52U,
                               200U) &&
          vm.runtime().write16(
              line_bridge_profile.effect_particle_pool + 0x54U,
              std::bit_cast<std::uint16_t>(std::int16_t{-300})) &&
          // FUN_80055298: the exact opaque M79 child trail. Current world
          // point is packet xy0; subtracting its attached-object motion and
          // particle step recovers packet xy1.
          vm.runtime().write32(second_line_controller + 4U, 0x20020U) &&
          vm.runtime().write16(second_line_controller + 0x14U, 2U) &&
          vm.runtime().write16(second_line_controller + 0x1eU, 1U) &&
          vm.runtime().write16(second_line_controller + 0x20U, 0xffffU) &&
          vm.runtime().write8(second_line_controller + 0x23U, 3U) &&
          vm.runtime().write32(second_line_controller + 0x2cU, 7U) &&
          vm.runtime().write32(second_line_controller + 0x30U, 3U) &&
          vm.runtime().write32(second_line_particle, 4000U) &&
          vm.runtime().write32(second_line_particle + 4U, 5000U) &&
          vm.runtime().write32(second_line_particle + 8U, 6000U) &&
          vm.runtime().write32(second_line_particle + 0x10U, 10U) &&
          vm.runtime().write32(second_line_particle + 0x14U, 4096U) &&
          vm.runtime().write32(second_line_particle + 0x18U, 30U) &&
          vm.runtime().write16(second_line_particle + 0x24U, 4U) &&
          vm.runtime().write16(second_line_particle + 0x26U, 0xffffU) &&
          vm.runtime().write32(second_line_particle + 0x34U, 0x50c0ffffU) &&
          vm.runtime().write32(second_line_particle + 0x3cU, 0x00c0ffffU) &&
          vm.runtime().write32(player_motion + 0x10U, 8192U) &&
          vm.runtime().write32(
              player_motion + 0x14U,
              std::bit_cast<std::uint32_t>(std::int32_t{-4096})) &&
          vm.runtime().write32(player_motion + 0x18U, 12288U),
      "Could not seed retail LINE_G2 bridge fixtures");
  const auto line_bridge = vm.readBridgeState(line_bridge_profile);
  require(line_bridge && line_bridge->expl_particles.empty() &&
              line_bridge->line_particles.size() == 2U &&
              line_bridge->line_particles[0].kind ==
                  sf::game::LegacyLineParticleKind::ballistic_tracer &&
              line_bridge->line_particles[0].first.x == -100 &&
              line_bridge->line_particles[0].first.y == 200 &&
              line_bridge->line_particles[0].first.z == -300 &&
              line_bridge->line_particles[0].second.x == 1000 &&
              line_bridge->line_particles[0].second.y == 2000 &&
              line_bridge->line_particles[0].second.z == 3000 &&
              line_bridge->line_particles[0].first_color ==
                  sf::game::LegacyRgbBridgeState{200U, 150U, 0U} &&
              !line_bridge->line_particles[0].semi_transparent &&
              line_bridge->line_particles[1].kind ==
                  sf::game::LegacyLineParticleKind::moving_trail &&
              line_bridge->line_particles[1].first.x == 4000 &&
              line_bridge->line_particles[1].first.y == 5000 &&
              line_bridge->line_particles[1].first.z == 6000 &&
              line_bridge->line_particles[1].second.x == 3988 &&
              line_bridge->line_particles[1].second.y == 5000 &&
              line_bridge->line_particles[1].second.z == 5967 &&
              line_bridge->line_particles[1].first_color ==
                  sf::game::LegacyRgbBridgeState{255U, 255U, 192U} &&
              line_bridge->line_particles[1].second_color ==
                  sf::game::LegacyRgbBridgeState{255U, 255U, 192U} &&
              !line_bridge->line_particles[1].semi_transparent &&
              !line_bridge->line_particles[1].raw_packet_authoritative,
          "Legacy LINE_G2 endpoints or packet colors mismatch");
  require(vm.runtime().write32(second_line_controller + 4U, 0x20060U),
          "Could not seed bouncing update7 controller");
  const auto bouncing_line_bridge = vm.readBridgeState(line_bridge_profile);
  require(
      bouncing_line_bridge &&
          bouncing_line_bridge->line_particles.size() == 2U &&
          bouncing_line_bridge->line_particles[1].raw_packet_authoritative &&
          vm.runtime().write32(second_line_controller + 4U, 0x20020U),
      "Bouncing update7 line lost exact raw-packet ownership");
  require(vm.runtime().write16(second_line_controller + 0x14U, 3U) &&
              !vm.readBridgeState(line_bridge_profile) &&
              vm.runtime().write16(second_line_controller + 0x14U, 2U),
          "Legacy bridge accepted an invalid attached LINE_G2 object");
  require(
      vm.runtime().write16(
          line_bridge_profile.effect_controller_count,
          static_cast<std::uint16_t>(
              line_bridge_profile.maximum_effect_controllers + 1U)) &&
          !vm.readBridgeState(line_bridge_profile) &&
          vm.runtime().write16(line_bridge_profile.effect_controller_count, 2U),
      "Legacy bridge accepted a controller count above retail allocation");
  require(vm.runtime().write32(second_line_controller + 0x2cU, 8U) &&
              vm.runtime().write32(second_line_controller + 0x30U, 5U),
          "Could not seed the non-rendered retail projectile controller");
  const auto skipped_projectile = vm.readBridgeState(line_bridge_profile);
  require(skipped_projectile &&
              skipped_projectile->line_particles.size() == 1U &&
              vm.runtime().write32(second_line_controller + 0x2cU, 7U) &&
              vm.runtime().write32(second_line_controller + 0x30U, 3U),
          "Legacy bridge rendered retail mode8/render5 as a line");
  require(
      vm.runtime().write32(line_bridge_profile.effect_particle_pool + 0x34U,
                           0x510096c8U) &&
          !vm.readBridgeState(line_bridge_profile) &&
          vm.runtime().write32(line_bridge_profile.effect_particle_pool + 0x34U,
                               0x500096c8U),
      "Legacy bridge accepted a damaged LINE_G2 opcode");

  auto combat_bridge_profile = bridge_profile;
  combat_bridge_profile.effect_controller_pool_pointer = 0x80051300U;
  combat_bridge_profile.effect_controller_count = 0x80051304U;
  combat_bridge_profile.effect_particle_pool = 0x80051600U;
  combat_bridge_profile.effect_particle_capacity = 2U;
  constexpr std::uint32_t combat_controllers = 0x80051400U;
  constexpr std::uint32_t blood_controller = combat_controllers + 0x34U;
  constexpr std::uint32_t ejected_particle = 0x80051600U;
  constexpr std::uint32_t blood_particle = ejected_particle + 0x68U;
  require(
      vm.runtime().write32(combat_bridge_profile.effect_controller_pool_pointer,
                           combat_controllers) &&
          vm.runtime().write16(combat_bridge_profile.effect_controller_count,
                               2U) &&
          // FUN_80055acc has already advanced this particle when the native
          // bridge reads it. The bridge must recover the LINE_F2 state that
          // the retail renderer submitted earlier in the same guest tick.
          // FUN_8004bbb0 leaves controller+0x14 untouched, and
          // FUN_8004e1f0 does not author it for update6/render2. Preserve the
          // retail allocator poison seen on mission 0's first shot.
          vm.runtime().write16(combat_controllers + 0x14U, 0x0c0cU) &&
          vm.runtime().write16(combat_controllers + 0x1eU, 0U) &&
          vm.runtime().write16(combat_controllers + 0x20U, 0U) &&
          vm.runtime().write32(combat_controllers + 0x2cU, 6U) &&
          vm.runtime().write32(combat_controllers + 0x30U, 2U) &&
          vm.runtime().write32(ejected_particle, 1050U) &&
          vm.runtime().write32(ejected_particle + 4U, 2001U) &&
          vm.runtime().write32(ejected_particle + 8U, 3070U) &&
          vm.runtime().write32(ejected_particle + 0x10U, 50U) &&
          vm.runtime().write32(ejected_particle + 0x14U, 4096U) &&
          vm.runtime().write32(ejected_particle + 0x18U, 70U) &&
          vm.runtime().write16(ejected_particle + 0x24U, 5U) &&
          vm.runtime().write16(ejected_particle + 0x26U, 0xffffU) &&
          vm.runtime().write32(ejected_particle + 0x34U, 0x40112233U) &&
          vm.runtime().write16(ejected_particle + 0x62U, 0x0120U) &&
          vm.runtime().write16(ejected_particle + 0x64U, 0x0020U) &&
          // FUN_8005554c projects mode9 from its current world centre. Its
          // three guest angles and flat POLY_F3 material are immutable input
          // to the native projection.
          // FUN_8004ec5c's player-attached mode9 path marks +0x14 meaningful
          // with bit 0x20000. Detached mode9 controllers do not initialize it.
          vm.runtime().write32(blood_controller + 4U, 0x20228U) &&
          vm.runtime().write16(blood_controller + 0x14U, 1U) &&
          vm.runtime().write16(blood_controller + 0x1eU, 1U) &&
          vm.runtime().write16(blood_controller + 0x20U, 2U) &&
          vm.runtime().write8(blood_controller + 0x23U, 6U) &&
          vm.runtime().write32(blood_controller + 0x2cU, 9U) &&
          vm.runtime().write32(blood_controller + 0x30U, 0U) &&
          vm.runtime().write32(blood_particle, std::bit_cast<std::uint32_t>(
                                                   std::int32_t{-400})) &&
          vm.runtime().write32(blood_particle + 4U, 500U) &&
          vm.runtime().write32(blood_particle + 8U, 600U) &&
          vm.runtime().write32(blood_particle + 0x20U, 0x00604020U) &&
          vm.runtime().write16(blood_particle + 0x24U, 4U) &&
          vm.runtime().write16(blood_particle + 0x26U, 0xffffU) &&
          vm.runtime().write32(blood_particle + 0x34U, 0x22604020U) &&
          vm.runtime().write16(blood_particle + 0x56U, 0x0555U) &&
          vm.runtime().write16(
              blood_particle + 0x5cU,
              std::bit_cast<std::uint16_t>(std::int16_t{-0x0555})) &&
          vm.runtime().write16(
              blood_particle + 0x62U,
              std::bit_cast<std::uint16_t>(std::int16_t{-0x0100})),
      "Could not seed retail flat combat-particle fixtures");
  const auto combat_bridge = vm.readBridgeState(combat_bridge_profile);
  require(combat_bridge && combat_bridge->combat_particles.size() == 2U &&
              combat_bridge->combat_particles[0].kind ==
                  sf::game::LegacyCombatParticleKind::ejected_shot_line &&
              combat_bridge->combat_particles[0].controller == 0U &&
              combat_bridge->combat_particles[0].particle == 0U &&
              combat_bridge->combat_particles[0].attached_slot == -1 &&
              combat_bridge->combat_particles[0].source_slot == 0 &&
              combat_bridge->combat_particles[0].position.x == 1000 &&
              combat_bridge->combat_particles[0].position.y == 2000 &&
              combat_bridge->combat_particles[0].position.z == 3000 &&
              combat_bridge->combat_particles[0].angle == 0x0100 &&
              combat_bridge->combat_particles[0].second_angle == 0x0020 &&
              combat_bridge->combat_particles[0].color ==
                  sf::game::LegacyRgbBridgeState{0x33U, 0x22U, 0x11U} &&
              !combat_bridge->combat_particles[0].semi_transparent &&
              combat_bridge->combat_particles[1].kind ==
                  sf::game::LegacyCombatParticleKind::blood_impact_triangle &&
              combat_bridge->combat_particles[1].position.x == -400 &&
              combat_bridge->combat_particles[1].position.y == 500 &&
              combat_bridge->combat_particles[1].position.z == 600 &&
              combat_bridge->combat_particles[1].attached_slot == 1 &&
              combat_bridge->combat_particles[1].source_slot == 2 &&
              combat_bridge->combat_particles[1].scale_byte == 6U &&
              combat_bridge->combat_particles[1].angle == -0x0100 &&
              combat_bridge->combat_particles[1].second_angle == 0x0555 &&
              combat_bridge->combat_particles[1].third_angle == -0x0555 &&
              combat_bridge->combat_particles[1].color ==
                  sf::game::LegacyRgbBridgeState{0x20U, 0x40U, 0x60U} &&
              combat_bridge->combat_particles[1].semi_transparent,
          "Retail flat combat-particle bridge mismatch");
  require(vm.runtime().write32(blood_controller + 4U, 0x430U) &&
              vm.runtime().write16(blood_controller + 0x14U, 0x0c0cU),
          "Could not seed detached retail mode9 allocator poison");
  const auto detached_blood_bridge = vm.readBridgeState(combat_bridge_profile);
  require(detached_blood_bridge &&
              detached_blood_bridge->combat_particles.size() == 2U &&
              detached_blood_bridge->combat_particles[1].attached_slot == -1 &&
              vm.runtime().write32(blood_controller + 4U, 0x10000U) &&
              vm.runtime().write16(blood_controller + 0x14U, 0x0c0cU),
          "Detached retail mode9 treated uninitialized +0x14 as an actor");
  const auto unrelated_flag_blood_bridge =
      vm.readBridgeState(combat_bridge_profile);
  require(unrelated_flag_blood_bridge &&
              unrelated_flag_blood_bridge->combat_particles.size() == 2U &&
              unrelated_flag_blood_bridge->combat_particles[1].attached_slot ==
                  -1 &&
              vm.runtime().write32(blood_controller + 4U, 0x20228U) &&
              vm.runtime().write16(blood_controller + 0x14U, 1U),
          "Mode9 flag 0x10000 was mistaken for an authored attachment");
  require(
      vm.runtime().write32(ejected_particle + 0x34U, 0x41112233U) &&
          !vm.readBridgeState(combat_bridge_profile) &&
          vm.lastBridgeReadFault() ==
              sf::game::LegacyGameplayBridgeReadFault::effect_packet_opcode &&
          vm.runtime().write32(ejected_particle + 0x34U, 0x40112233U),
      "Legacy bridge accepted a damaged LINE_F2 opcode");
  require(vm.runtime().write16(blood_controller + 0x20U, 3U) &&
              !vm.readBridgeState(combat_bridge_profile) &&
              vm.lastBridgeReadFault() ==
                  sf::game::LegacyGameplayBridgeReadFault::effect_source_slot &&
              vm.runtime().write16(blood_controller + 0x20U, 2U),
          "Legacy bridge accepted an invalid combat-particle source slot");
  require(
      vm.runtime().write16(blood_controller + 0x14U, 3U) &&
          !vm.readBridgeState(combat_bridge_profile) &&
          vm.lastBridgeReadFault() ==
              sf::game::LegacyGameplayBridgeReadFault::effect_attached_slot &&
          vm.runtime().write16(blood_controller + 0x14U, 1U),
      "Legacy bridge accepted an invalid authored mode9 attachment");
  require(
      vm.runtime().write32(combat_controllers + 0x30U, 3U) &&
          !vm.readBridgeState(combat_bridge_profile) &&
          vm.lastBridgeReadFault() ==
              sf::game::LegacyGameplayBridgeReadFault::effect_controller_mode &&
          vm.runtime().write32(combat_controllers + 0x30U, 2U),
      "Legacy bridge accepted a mismatched update6 renderer");
  require(vm.runtime().write32(ejected_particle,
                               std::bit_cast<std::uint32_t>(
                                   std::numeric_limits<std::int32_t>::min())) &&
              vm.runtime().write32(ejected_particle + 0x10U, 1U) &&
              !vm.readBridgeState(combat_bridge_profile) &&
              vm.lastBridgeReadFault() ==
                  sf::game::LegacyGameplayBridgeReadFault::
                      effect_position_overflow &&
              vm.runtime().write32(ejected_particle, 1050U) &&
              vm.runtime().write32(ejected_particle + 0x10U, 50U),
          "Legacy bridge accepted an overflowing submitted LINE_F2 position");

  const auto renderer_flags_address =
      camera_object + bridge_profile.renderer_flags_offset;
  require(vm.runtime().write16(renderer_flags_address, 0x15U),
          "Could not seed the retail night-vision camera flag");
  const auto nightvision_environment = vm.readBridgeState(bridge_profile);
  require(nightvision_environment &&
              nightvision_environment->environment.nightvision_enabled &&
              nightvision_environment->environment.screen_filter_color ==
                  sf::game::LegacyRgbBridgeState{0U, 112U, 0U} &&
              vm.runtime().write16(renderer_flags_address, 5U),
          "Legacy bridge lost the retail night-vision environment state");
  require(vm.runtime().write16(renderer_flags_address, 0xfffeU),
          "Could not seed the disabled retail background flag");
  const auto disabled_background = vm.readBridgeState(bridge_profile);
  require(disabled_background &&
              !disabled_background->environment.background_enabled &&
              vm.runtime().write16(renderer_flags_address, 5U),
          "Legacy bridge did not decode the retail background-enable bit");
  const auto renderer_sprite_fast_path_address =
      camera_object + bridge_profile.renderer_sprite_fast_path_offset;
  require(vm.runtime().write8(renderer_sprite_fast_path_address, 2U) &&
              !vm.readBridgeState(bridge_profile) &&
              vm.runtime().write8(renderer_sprite_fast_path_address, 1U),
          "Legacy bridge accepted an invalid retail sprite-sort mode");
  require(
      vm.runtime().write8(renderer_sprite_fast_path_address, 0U) &&
          vm.runtime().write16(bridge_profile.renderer_display_flags, 0x00a5U),
      "Could not seed the retail dark-frame environment");
  const auto dark_environment = vm.readBridgeState(bridge_profile);
  require(
      dark_environment &&
          dark_environment->environment.renderer_darkness_enabled &&
          dark_environment->environment.effectiveTerrainDepthCue() ==
              dark_environment->environment.terrain_depth_cue &&
          vm.runtime().write8(renderer_sprite_fast_path_address, 1U) &&
          vm.runtime().write16(bridge_profile.renderer_display_flags, 0x00a4U),
      "Legacy bridge leaked a per-object dark-frame cue into camera fog");
  const auto fog_dqa_address =
      camera_object + bridge_profile.renderer_fog_dqa_offset;
  require(vm.runtime().write32(fog_dqa_address, 0x00010000U) &&
              !vm.readBridgeState(bridge_profile) &&
              vm.runtime().write32(
                  fog_dqa_address,
                  std::bit_cast<std::uint32_t>(std::int32_t{-123})),
          "Legacy bridge accepted a DQA value outside the GTE register");
  require(vm.runtime().write32(fog_dqa_address, 0U),
          "Could not seed the zero-DQA environment fixture");
  const auto no_fog_environment = vm.readBridgeState(bridge_profile);
  require(no_fog_environment &&
              no_fog_environment->environment.fog_dqb == 0x12345000 &&
              !no_fog_environment->environment.fogEnabled() &&
              vm.runtime().write32(
                  fog_dqa_address,
                  std::bit_cast<std::uint32_t>(std::int32_t{-123})),
          "Legacy bridge did not fail closed for zero DQA");
  require(vm.runtime().write32(bridge_profile.terrain_depth_cue,
                               (16U << 16U) | 0x0321U) &&
              !vm.readBridgeState(bridge_profile) &&
              vm.runtime().write32(bridge_profile.terrain_depth_cue,
                                   (2U << 16U) | 0x0321U),
          "Legacy bridge accepted an unsafe terrain depth-cue shift");
  require(vm.runtime().write32(bridge_profile.active_terrain_depth_cue,
                               (16U << 16U) | 0x0800U) &&
              !vm.readBridgeState(bridge_profile) &&
              vm.runtime().write32(bridge_profile.active_terrain_depth_cue,
                                   (5U << 16U) | 0x0800U),
          "Legacy bridge accepted an unsafe active depth-cue shift");
  require(sf::game::validateLegacyWorldModelSets(*bridge, 5U),
          "Legacy bridge rejected valid world model sets");
  auto duplicate_world_model = *bridge;
  duplicate_world_model.active_world_models.push_back(
      duplicate_world_model.active_world_models.front());
  require(!sf::game::validateLegacyWorldModelSets(duplicate_world_model, 5U),
          "Legacy bridge accepted a duplicate active world model");
  auto out_of_range_world_model = *bridge;
  out_of_range_world_model.resident_world_models.push_back(5U);
  require(!sf::game::validateLegacyWorldModelSets(out_of_range_world_model, 5U),
          "Legacy bridge accepted an out-of-range resident world model");
  require(vm.runtime().write32(world_descriptors + 2U * 0x3cU, 0x801ffff8U),
          "Could not seed an unloading optional world descriptor");
  const auto unloading_optional = vm.readBridgeState(bridge_profile);
  require(
      vm.runtime().write32(world_descriptors + 2U * 0x3cU, guest_world_model),
      "Could not restore the optional world descriptor");
  require(unloading_optional &&
              std::ranges::none_of(
                  unloading_optional->world_vertex_colors,
                  [](const auto &colors) { return colors.model == 2U; }),
          "Legacy bridge rejected or partially captured an unloading optional "
          "world model");

  require(vm.runtime().write32(world_descriptors + 1U * 0x3cU, 0x801ffff8U),
          "Could not seed a malformed required world descriptor");
  const auto malformed_required = vm.readBridgeState(bridge_profile);
  require(
      vm.runtime().write32(world_descriptors + 1U * 0x3cU, guest_world_model),
      "Could not restore the required world descriptor");
  require(malformed_required &&
              std::ranges::none_of(
                  malformed_required->world_vertex_colors,
                  [](const auto &colors) { return colors.model == 1U; }) &&
              malformed_required->active_world_models ==
                  std::vector<std::uint16_t>({1U, 3U}),
          "Legacy bridge invalidated guest visibility while an active world's "
          "auxiliary color payload was being recycled");

  auto bounded_world_colors_profile = bridge_profile;
  bounded_world_colors_profile.maximum_world_vertex_colors = 13U;
  const auto bounded_world_colors =
      vm.readBridgeState(bounded_world_colors_profile);
  require(bounded_world_colors &&
              bounded_world_colors->world_vertex_colors.size() == 4U &&
              std::ranges::none_of(
                  bounded_world_colors->world_vertex_colors,
                  [](const auto &colors) { return colors.model == 2U; }),
          "Optional world colors exhausted the required camera-set budget");
  auto unreadable_visibility_profile = bridge_profile;
  unreadable_visibility_profile.world_visibility_bytes = 0x801ffffeU;
  require(!vm.readBridgeState(unreadable_visibility_profile),
          "Legacy bridge accepted a truncated world visibility byte array");
  require(vm.runtime().write8(bridge_world_layout + 0x79U, 0U),
          "Could not seed an authored duplicate resident world model");
  const auto deduplicated_resident = vm.readBridgeState(bridge_profile);
  require(deduplicated_resident &&
              deduplicated_resident->resident_world_models ==
                  std::vector<std::uint16_t>({0U}) &&
              vm.runtime().write8(bridge_world_layout + 0x79U, 4U),
          "Legacy bridge did not normalize a duplicate resident model");
  require(vm.runtime().write8(bridge_world_layout + 0x7aU, 5U) &&
              !vm.readBridgeState(bridge_profile) &&
              vm.runtime().write8(bridge_world_layout + 0x7aU, 0xffU),
          "Legacy bridge accepted an unterminated/out-of-range resident set");
  require(vm.runtime().write32(bridge_profile.world_model_count, 0U) &&
              !vm.readBridgeState(bridge_profile) &&
              vm.runtime().write32(bridge_profile.world_model_count, 5U),
          "Legacy bridge accepted an empty retail world-model table");
  require(vm.runtime().write16(bridge_profile.current_room, 5U) &&
              !vm.readBridgeState(bridge_profile) &&
              vm.runtime().write16(bridge_profile.current_room, 0xffffU),
          "Legacy bridge accepted an out-of-range retail room");
  require(vm.runtime().write16(bridge_profile.dynamic_first_slot, 4U) &&
              !vm.readBridgeState(bridge_profile) &&
              vm.runtime().write16(bridge_profile.dynamic_first_slot, 1U),
          "Legacy bridge accepted a dynamic pool boundary past object_count");
  require(
      vm.runtime().write16(bridge_object_definitions + 54U * 0x14U, 0x80U) &&
          !vm.readBridgeState(bridge_profile) &&
          vm.runtime().write16(bridge_object_definitions + 54U * 0x14U, 0U),
      "Legacy bridge indexed past the retail object-handler table");
  const auto yaw_round_trip_snapshot = vm.captureSnapshot();
  require(vm.runtime().write32(native_bridge_profile.player_pointer,
                               player_instance) &&
              vm.runtime().write32(player_instance + 0x18U, player_health),
          "Could not seed the host player yaw round-trip fixture");
  for (const auto yaw : std::array<std::int32_t, 2U>{512, 1024}) {
    require(vm.writeHostPlayerState(
                sf::game::LegacyHostPlayerState{
                    .position = {911, 922, 933},
                    .yaw = yaw,
                    .health = 150,
                    .armor = 600,
                    .previous_position = {911, 922, 933},
                    .has_previous_position = true,
                },
                native_bridge_profile),
            "Host player yaw writer rejected the synthetic retail player");
    const auto rewritten = vm.readBridgeState(bridge_profile);
    require(rewritten && rewritten->player.resident &&
                sf::game::headingFromDirection(
                    static_cast<double>(rewritten->player.guest_rotation[2]),
                    static_cast<double>(rewritten->player.guest_rotation[8])) ==
                    yaw,
            "Host player yaw did not survive the guest matrix round trip");
  }
  require(vm.runtime().write32(player_matrix + 0x14U, 111U) &&
              vm.runtime().write32(player_matrix + 0x18U, 222U) &&
              vm.runtime().write32(player_matrix + 0x1cU, 333U) &&
              vm.runtime().write16(player_matrix, 2345U) &&
              vm.runtime().write16(player_health + 6U, 321U) &&
              vm.runtime().write16(player_health + 8U, 123U) &&
              vm.runtime().write16(third_record + 0x40U, 124U) &&
              vm.writeHostPlayerLocomotion(
                  sf::game::LegacyHostPlayerLocomotion{
                      .position = {101, 202, 303},
                      .previous_position = {91, 192, 293},
                      .has_previous_position = true,
                  },
                  native_bridge_profile),
          "Narrow first-person locomotion writer rejected the retail player");
  std::array<std::uint32_t, 9U> locomotion_words{};
  require(vm.runtime().read32(player_motion, locomotion_words[0]) &&
              vm.runtime().read32(player_motion + 4U, locomotion_words[1]) &&
              vm.runtime().read32(player_motion + 8U, locomotion_words[2]) &&
              vm.runtime().read32(player_motion + 0x40U, locomotion_words[3]) &&
              vm.runtime().read32(player_motion + 0x44U, locomotion_words[4]) &&
              vm.runtime().read32(player_motion + 0x48U, locomotion_words[5]) &&
              vm.runtime().read32(player_matrix + 0x14U, locomotion_words[6]) &&
              vm.runtime().read32(player_matrix + 0x18U, locomotion_words[7]) &&
              vm.runtime().read32(player_matrix + 0x1cU, locomotion_words[8]),
          "Could not inspect the narrow first-person locomotion write");
  std::array<std::uint16_t, 4U> preserved_player_words{};
  require(
      vm.runtime().read16(player_matrix, preserved_player_words[0]) &&
          vm.runtime().read16(player_health + 6U, preserved_player_words[1]) &&
          vm.runtime().read16(player_health + 8U, preserved_player_words[2]) &&
          vm.runtime().read16(third_record + 0x40U, preserved_player_words[3]),
      "Could not inspect pose/vitals after first-person locomotion");
  const auto signed_word = [](std::uint32_t value) {
    return std::bit_cast<std::int32_t>(value);
  };
  require(signed_word(locomotion_words[0]) == 101 &&
              signed_word(locomotion_words[1]) == -202 &&
              signed_word(locomotion_words[2]) == 303 &&
              signed_word(locomotion_words[3]) == 91 &&
              signed_word(locomotion_words[4]) == -192 &&
              signed_word(locomotion_words[5]) == 293 &&
              signed_word(locomotion_words[6]) == 101 &&
              signed_word(locomotion_words[7]) == 222 &&
              signed_word(locomotion_words[8]) == 303 &&
              preserved_player_words ==
                  std::array<std::uint16_t, 4U>{2345U, 321U, 123U, 124U},
          "First-person locomotion corrupted animated pose height, rotation, "
          "or vitals");
  constexpr std::array<std::int16_t, 9U> tilted_player_rotation{
      4096, 0, 0, 0, 3547, -2048, 0, 2048, 3547,
  };
  auto tilted_seeded = true;
  for (std::uint32_t component = 0U; component < tilted_player_rotation.size();
       ++component) {
    tilted_seeded =
        tilted_seeded &&
        vm.runtime().write16(
            player_matrix + component * 2U,
            std::bit_cast<std::uint16_t>(tilted_player_rotation[component]));
  }
  require(tilted_seeded &&
              vm.writeHostPlayerHeading(1024, native_bridge_profile),
          "Narrow host heading writer rejected the tilted player root");
  std::array<std::int16_t, 9U> rotated_tilt{};
  auto tilted_read = true;
  for (std::uint32_t component = 0U; component < rotated_tilt.size();
       ++component) {
    std::uint16_t bits{};
    tilted_read = tilted_read &&
                  vm.runtime().read16(player_matrix + component * 2U, bits);
    rotated_tilt[component] = std::bit_cast<std::int16_t>(bits);
  }
  require(tilted_read && rotated_tilt[3] == tilted_player_rotation[3] &&
              rotated_tilt[4] == tilted_player_rotation[4] &&
              rotated_tilt[5] == tilted_player_rotation[5] &&
              sf::game::headingFromDirection(
                  static_cast<double>(rotated_tilt[2]),
                  static_cast<double>(rotated_tilt[8])) == 1024,
          "Narrow heading restore destroyed player root tilt/roll");
  require(vm.restoreSnapshot(yaw_round_trip_snapshot),
          "Could not restore the VM after the host player yaw round trip");
  require(vm.runtime().write8(actor_instance, 0x80U),
          "Could not seed retail destroyed-instance latch");
  const auto bridge_with_destroyed_latch = vm.readBridgeState(bridge_profile);
  require(bridge_with_destroyed_latch &&
              bridge_with_destroyed_latch->objects[0].resident &&
              bridge_with_destroyed_latch->objects[0].health == 75 &&
              bridge_with_destroyed_latch->objects[0].destroyed() &&
              !bridge_with_destroyed_latch->objects[0].alive(),
          "Legacy destroyed-instance latch was ignored");
  require(vm.runtime().write8(actor_instance, 0x40U),
          "Could not restore live instance flags");
  require(vm.runtime().write16(bridge_profile.taser_conductor_phase, 3U),
          "Could not seed retail taser shutdown phase");
  const auto bridge_with_stopped_taser = vm.readBridgeState(bridge_profile);
  require(bridge_with_stopped_taser &&
              bridge_with_stopped_taser->taser_conductor_phase == 3U &&
              bridge_with_stopped_taser->taser_target_slot == 1 &&
              !bridge_with_stopped_taser->taserConductorActive(),
          "Legacy taser shutdown phase remained active");
  require(vm.runtime().write16(bridge_profile.taser_conductor_phase, 2U),
          "Could not restore active taser conductor phase");
  require(vm.runtime().write32(actor_motion + 0x12cU, 0xffffff8aU),
          "Could not seed packed legacy ground contact");
  const auto bridge_with_contact = vm.readBridgeState(bridge_profile);
  require(bridge_with_contact &&
              bridge_with_contact->objects[0].ground_contact_valid &&
              bridge_with_contact->objects[0].ground_contact_y == 120,
          "Legacy packed ground-contact flags/sentinel mismatch");

  require(
      vm.runtime().write32(second_record + 0x2cU, recycled_actor_first_path),
      "Could not activate recycled legacy actor lifetime");
  const auto active_recycled_actor = vm.readBridgeState(bridge_profile);
  require(
      active_recycled_actor && active_recycled_actor->objects[1].resident &&
          active_recycled_actor->objects[1].object_handler ==
              legacy_common_npc_handler &&
          active_recycled_actor->objects[1].alive() &&
          active_recycled_actor->objects[1].simulated &&
          active_recycled_actor->objects[1].has_target &&
          active_recycled_actor->objects[1].target_slot == 0 &&
          active_recycled_actor->objects[1].target_flags == 0x20U &&
          active_recycled_actor->objects[1].target_meter == 73 &&
          active_recycled_actor->objects[1].danger_q12 == 0x800U &&
          active_recycled_actor->objects[1].presentation_enabled == 1U &&
          active_recycled_actor->objects[1].position.x == 701 &&
          active_recycled_actor->objects[1].position.y == 702 &&
          active_recycled_actor->objects[1].position.z == 703 &&
          active_recycled_actor->objects[1].ai_route_flags == 0x0400U &&
          active_recycled_actor->objects[1].bone_matrix_count ==
              sf::game::legacy_actor_bone_count &&
          active_recycled_actor->objects[1].bone_matrices[0].translation.x ==
              744 &&
          active_recycled_actor->objects[1].ground_contact_valid &&
          active_recycled_actor->objects[1].ground_contact_y == 120,
      "Legacy recycled actor activation did not restore exact runtime pose");

  constexpr std::uint32_t partial_bone_count = 3U;
  require(vm.runtime().write32(
              recycled_actor_bone_table + partial_bone_count * 4U, 0U),
          "Could not terminate the variable-size HMD pose table");
  const auto partial_actor_pose = vm.readBridgeState(bridge_profile);
  require(partial_actor_pose &&
              partial_actor_pose->objects[1].bone_matrix_count ==
                  partial_bone_count &&
              partial_actor_pose->objects[1]
                      .bone_matrices[partial_bone_count - 1U]
                      .translation.x == 744 &&
              vm.runtime().write32(recycled_actor_bone_table +
                                       partial_bone_count * 4U,
                                   recycled_actor_bone_matrix),
          "Legacy bridge rejected a variable-size HMD pose table");

  require(vm.runtime().write16(recycled_actor_target, 0xffffU) &&
              vm.runtime().write32(recycled_actor_target + 4U, 0x09U),
          "Could not seed target-controller flags without a current target");
  const auto actor_without_target = vm.readBridgeState(bridge_profile);
  require(
      actor_without_target && !actor_without_target->objects[1].has_target &&
          actor_without_target->objects[1].target_slot == -1 &&
          actor_without_target->objects[1].target_flags == 0x09U &&
          actor_without_target->objects[1].target_meter == 73 &&
          actor_without_target->objects[1].danger_q12 == 0x800U,
      "Legacy bridge conflated target-controller flags with target presence");
  require(vm.runtime().write16(recycled_actor_target, 0U) &&
              vm.runtime().write32(recycled_actor_target + 4U, 0x20U),
          "Could not restore active target-controller state");

  require(vm.runtime().write32(recycled_actor_node + 8U, 0U),
          "Could not clear retail HMD-rendered pose flag");
  auto no_bone_resolver_profile = bridge_profile;
  no_bone_resolver_profile.bone_matrix_resolver_entry = 0U;
  const auto actor_with_stale_bone_cache =
      vm.readBridgeState(no_bone_resolver_profile);
  require(
      actor_with_stale_bone_cache &&
          actor_with_stale_bone_cache->objects[1].resident &&
          actor_with_stale_bone_cache->objects[1].bone_matrix_count == 0U,
      "Legacy bridge exported a readable but not freshly rendered bone cache");
  const auto bridge_read_before = vm.captureSnapshot();
  const auto actor_with_materialized_bones = vm.readBridgeState(bridge_profile);
  const auto bridge_read_after = vm.captureSnapshot();
  std::uint32_t unchanged_bone_cache{};
  require(
      actor_with_materialized_bones &&
          actor_with_materialized_bones->objects[1].bone_matrix_count ==
              sf::game::legacy_actor_bone_count &&
          actor_with_materialized_bones->objects[1]
                  .bone_matrices[0]
                  .translation.x == 759,
      "Legacy bridge did not materialize the non-rendered retail bone pose");
  require(
      actor_with_materialized_bones->objects[1]
                  .bone_matrices[0]
                  .translation.y == 755 &&
          actor_with_materialized_bones->objects[1]
                  .bone_matrices[0]
                  .translation.z == 766 &&
          bridge_read_before.ram == bridge_read_after.ram &&
          bridge_read_before.scratchpad == bridge_read_after.scratchpad &&
          bridge_read_before.mmio == bridge_read_after.mmio &&
          bridge_read_before.cpu.gpr == bridge_read_after.cpu.gpr &&
          bridge_read_before.cpu.gte.data == bridge_read_after.cpu.gte.data &&
          bridge_read_before.cpu.gte.control ==
              bridge_read_after.cpu.gte.control &&
          bridge_read_before.cpu.pc == bridge_read_after.cpu.pc &&
          bridge_read_before.cpu.next_pc == bridge_read_after.cpu.next_pc &&
          bridge_read_before.machine.scheduler.now ==
              bridge_read_after.machine.scheduler.now &&
          bridge_read_before.machine.spu && bridge_read_after.machine.spu &&
          *bridge_read_before.machine.spu == *bridge_read_after.machine.spu &&
          vm.runtime().read32(recycled_actor_bone_matrix + 0x14U,
                              unchanged_bone_cache) &&
          unchanged_bone_cache == 744U,
      "Legacy bridge materialized a pose by mutating the guest VM");

  require(vm.runtime().write32(recycled_actor_node + 8U, 0x40U) &&
              vm.runtime().write16(second_record + 0x40U, 0U) &&
              vm.runtime().write32(recycled_actor_matrix + 0x14U, 711U),
          "Could not seed dynamic actor death presentation");
  const auto dying_recycled_actor = vm.readBridgeState(bridge_profile);
  require(dying_recycled_actor && dying_recycled_actor->objects[1].resident &&
              !dying_recycled_actor->objects[1].alive() &&
              dying_recycled_actor->objects[1].position.x == 711 &&
              dying_recycled_actor->objects[1].bone_matrix_count ==
                  sf::game::legacy_actor_bone_count,
          "Dynamic actor was retired before FUN_80065fa0 cleared its path");

  require(vm.runtime().write32(second_record + 0x2cU, 0U),
          "Could not retire dynamic actor lifetime");
  const auto retired_recycled_actor = vm.readBridgeState(bridge_profile);
  require(retired_recycled_actor &&
              !retired_recycled_actor->objects[1].resident &&
              !retired_recycled_actor->objects[1].simulated &&
              !retired_recycled_actor->objects[1].has_target &&
              retired_recycled_actor->objects[1].target_flags == 0U &&
              retired_recycled_actor->objects[1].target_meter == 0 &&
              retired_recycled_actor->objects[1].danger_q12 == 0U &&
              retired_recycled_actor->objects[1].position.x == 0 &&
              retired_recycled_actor->objects[1].bone_matrix_count == 0U &&
              !retired_recycled_actor->objects[1].ground_contact_valid,
          "Retired dynamic actor leaked stale root/animation/ground state");

  require(vm.runtime().write32(second_record, 0xffffffffU) &&
              vm.runtime().write32(second_record + 0x34U, 0xffffffffU),
          "Could not poison the retired dynamic actor cache");
  const auto retired_actor_with_stale_overlay_cache =
      vm.readBridgeState(bridge_profile);
  require(retired_actor_with_stale_overlay_cache &&
              retired_actor_with_stale_overlay_cache->objects[1].definition ==
                  0xffffffffU &&
              retired_actor_with_stale_overlay_cache->objects[1].class_id ==
                  -1 &&
              !retired_actor_with_stale_overlay_cache->objects[1].resident &&
              !retired_actor_with_stale_overlay_cache->objects[1].simulated &&
              !retired_actor_with_stale_overlay_cache->objects[1].has_target,
          "Retired dynamic actor dereferenced stale overlay-owned caches");
  require(
      vm.runtime().write32(second_record + 0x2cU, recycled_actor_first_path) &&
          !vm.readBridgeState(bridge_profile) &&
          vm.runtime().write32(second_record + 0x2cU, 0U),
      "Active dynamic actor accepted stale overlay-owned caches");
  require(
      vm.runtime().write32(second_record, 53U) &&
          vm.runtime().write32(second_record + 0x34U, recycled_actor_instance),
      "Could not restore the recycled actor cache");

  require(
      vm.runtime().write32(second_record + 0x2cU, recycled_actor_second_path) &&
          vm.runtime().write16(second_record + 0x40U, 90U) &&
          vm.runtime().write32(recycled_actor_matrix + 0x14U, 901U) &&
          vm.runtime().write32(recycled_actor_bone_matrix + 0x14U, 944U),
      "Could not seed recycled actor respawn");
  const auto respawned_recycled_actor = vm.readBridgeState(bridge_profile);
  require(
      respawned_recycled_actor &&
          respawned_recycled_actor->objects[1].resident &&
          respawned_recycled_actor->objects[1].alive() &&
          respawned_recycled_actor->objects[1].simulated &&
          respawned_recycled_actor->objects[1].position.x == 901 &&
          respawned_recycled_actor->objects[1].ai_route_flags == 0x0100U &&
          respawned_recycled_actor->objects[1].bone_matrix_count ==
              sf::game::legacy_actor_bone_count &&
          respawned_recycled_actor->objects[1].bone_matrices[0].translation.x ==
              944,
      "Replenishing dynamic actor did not rebind after retirement");

  auto virtual_cd = std::make_shared<sf::game::LegacyVirtualCd>();
  const std::vector<std::byte> virtual_cd_file(
      sf::game::LegacyVirtualCd::sector_size, std::byte{0x5a});
  require(virtual_cd->addRootFile("BIN/INIT.OVL", virtual_cd_file),
          "Legacy VM virtual CD catalog setup failed");
  constexpr std::array virtual_cd_path{
      std::byte{'B'}, std::byte{'I'}, std::byte{'N'}, std::byte{'/'},
      std::byte{'I'}, std::byte{'N'}, std::byte{'I'}, std::byte{'T'},
      std::byte{'.'}, std::byte{'O'}, std::byte{'V'}, std::byte{'L'},
      std::byte{},
  };
  constexpr std::uint32_t virtual_cd_path_address = 0x80010400U;
  constexpr std::uint32_t virtual_cd_handle_address = 0x80010500U;
  constexpr std::uint32_t virtual_cd_size_address = 0x80010504U;
  constexpr std::uint32_t virtual_cd_read_size_address = 0x80010508U;
  constexpr std::uint32_t virtual_cd_destination = 0x80011000U;
  require(vm.runtime().loadBytes(virtual_cd_path_address, virtual_cd_path),
          "Legacy VM virtual CD path seed failed");
  vm.bindSyphonFilterUsaV11VirtualCdCalls(virtual_cd);
  const std::array open_arguments{virtual_cd_path_address,
                                  virtual_cd_handle_address};
  const auto open_result = vm.invoke(0x800deef4U, open_arguments, 1U);
  std::uint32_t virtual_cd_handle{};
  require(
      open_result.completed() && open_result.return_value == 0U &&
          vm.runtime().read32(virtual_cd_handle_address, virtual_cd_handle) &&
          virtual_cd_handle != 0U,
      "Legacy VM virtual CD open HLE mismatch");
  const std::array size_arguments{virtual_cd_handle, virtual_cd_size_address};
  const auto size_result = vm.invoke(0x800df148U, size_arguments, 1U);
  std::uint32_t virtual_cd_size{};
  require(size_result.completed() && size_result.return_value == 0U &&
              vm.runtime().read32(virtual_cd_size_address, virtual_cd_size) &&
              virtual_cd_size == sf::game::LegacyVirtualCd::sector_size,
          "Legacy VM virtual CD size HLE mismatch");
  constexpr std::uint32_t virtual_cd_position_address = 0x80010520U;
  constexpr std::array<std::byte, 4U> virtual_cd_position{
      std::byte{0x00}, std::byte{0x02}, std::byte{0x10}, std::byte{0x00}};
  constexpr std::uint32_t cdrom_base = 0x1f801800U;
  require(vm.runtime().loadBytes(virtual_cd_position_address,
                                 virtual_cd_position) &&
              vm.runtime().write8(cdrom_base, 0U) &&
              vm.runtime().write8(cdrom_base + 1U, 0x01U),
          "Could not seed a pending CD-ROM interrupt for Setloc recovery");
  vm.machine().advanceTicks(sf::psx::CdRomController::command_delay_ticks);
  const std::array setloc_arguments{2U, virtual_cd_position_address};
  const auto rejected_setloc = vm.invoke(0x800ed5c0U, setloc_arguments, 1U);
  const auto raw_sector_after_rejection = virtual_cd->currentRawSector();
  const auto recovered_setloc = vm.invoke(0x800ed5c0U, setloc_arguments, 1U);
  require(
      rejected_setloc.completed() && rejected_setloc.return_value == 0U &&
          raw_sector_after_rejection == 0U && recovered_setloc.completed() &&
          recovered_setloc.return_value == 1U &&
          virtual_cd->currentRawSector() == 10U &&
          vm.machine().cdrom().captureState().interrupt_flags == 0U,
      "Virtual-CD Setloc committed early or failed to recover the controller");
  constexpr std::uint32_t vm_snapshot_raw_sector = 0x00045678U;
  virtual_cd->setCurrentRawSector(vm_snapshot_raw_sector);
  const auto vm_cd_snapshot = vm.captureSnapshot();
  const std::array read_arguments{
      virtual_cd_handle,
      virtual_cd_destination,
      sf::game::LegacyVirtualCd::sector_size,
      virtual_cd_read_size_address,
  };
  const auto virtual_cd_read_tick = vm.machine().currentTick();
  const auto virtual_cd_read_mixed_frames =
      vm.machine().spu().state().mixed_frames;
  const auto read_result = vm.invoke(0x800df198U, read_arguments, 1U);
  std::vector<std::byte> virtual_cd_output(
      sf::game::LegacyVirtualCd::sector_size);
  std::uint32_t virtual_cd_read_size{};
  require(
      read_result.completed() && read_result.return_value == 0U &&
          vm.runtime().copyBytes(virtual_cd_destination, virtual_cd_output) &&
          vm.runtime().read32(virtual_cd_read_size_address,
                              virtual_cd_read_size) &&
          virtual_cd_read_size == virtual_cd_output.size() &&
          vm.machine().currentTick() == virtual_cd_read_tick + 1U &&
          vm.machine().spu().state().mixed_frames ==
              virtual_cd_read_mixed_frames &&
          std::ranges::all_of(
              virtual_cd_output,
              [](std::byte value) { return value == std::byte{0x5a}; }),
      "Legacy VM virtual CD read HLE changed data or advanced SPU time");
  const std::array close_arguments{virtual_cd_handle_address};
  const auto close_result = vm.invoke(0x800df3b0U, close_arguments, 1U);
  require(
      close_result.completed() && close_result.return_value == 0U &&
          vm.runtime().read32(virtual_cd_handle_address, virtual_cd_handle) &&
          virtual_cd_handle == 0U,
      "Legacy VM virtual CD close HLE mismatch");
  virtual_cd->setCurrentRawSector(0U);
  require(
      vm.restoreSnapshot(vm_cd_snapshot) &&
          virtual_cd->currentRawSector() == vm_snapshot_raw_sector &&
          vm.runtime().read32(virtual_cd_handle_address, virtual_cd_handle) &&
          virtual_cd_handle != 0U,
      "Legacy VM virtual CD snapshot restore mismatch");
  const auto replay_read_result = vm.invoke(0x800df198U, read_arguments, 1U);
  virtual_cd_output.assign(virtual_cd_output.size(), std::byte{0x00});
  require(
      replay_read_result.completed() && replay_read_result.return_value == 0U &&
          vm.runtime().copyBytes(virtual_cd_destination, virtual_cd_output) &&
          std::ranges::all_of(
              virtual_cd_output,
              [](std::byte value) { return value == std::byte{0x5a}; }),
      "Legacy VM virtual CD replay diverged after snapshot restore");
  const auto replay_close_result = vm.invoke(0x800df3b0U, close_arguments, 1U);
  require(replay_close_result.completed() &&
              replay_close_result.return_value == 0U,
          "Legacy VM restored virtual CD handle could not be closed");
  vm.clearHostCalls();
  const auto cleared_text_state = vm.captureSnapshot();
  require(cleared_text_state.attached_text_sources.empty() &&
              cleared_text_state.ui_messages.empty() &&
              !vm.restoreSnapshot(vm_cd_snapshot),
          "Host-call clear retained text state or accepted unbound CD state");

  constexpr std::uint32_t host_call_address = 0x80030000U;
  constexpr std::array host_call_words{
      encodeR(31U, 0U, 16U, 0U, 0x21U), encodeI(0x0fU, 0U, 8U, 0x8001U),
      encodeI(0x09U, 0U, 4U, 40U),      encodeJ(0x03U, host_call_address),
      encodeI(0x23U, 8U, 2U, 0x0200U),  encodeI(0x09U, 2U, 2U, 1U),
      encodeR(16U, 0U, 0U, 0U, 0x08U),  0U,
  };
  const auto host_call_code = instructionBytes(host_call_words);
  require(vm.loadOverlay(overlay_address, host_call_code),
          "Legacy VM host-call program load failed");
  require(vm.runtime().write32(0x80010200U, 0xdeadbeefU),
          "Legacy VM host-call load-delay seed failed");
  std::uint32_t observed_calls{};
  vm.bindHostCall(host_call_address,
                  [&observed_calls](sf::game::LegacyHostCallContext &context) {
                    require(context.pc() == host_call_address,
                            "Host call observed the wrong PC");
                    require(context.argument(0) == 40U,
                            "Host call received the wrong argument");
                    context.setReturnValue(41U);
                    require(context.write32(0x80010300U, 0xcafebabeU),
                            "Host call could not write through the RAM bridge");
                    ++observed_calls;
                  });
  const auto host_result = vm.invoke(overlay_address);
  require(host_result.completed() && host_result.return_value == 42U &&
              host_result.host_calls == 1U && observed_calls == 1U,
          "Legacy VM host-call dispatch mismatch");
  std::uint32_t host_memory{};
  require(vm.runtime().read32(0x80010300U, host_memory) &&
              host_memory == 0xcafebabeU,
          "Host call RAM write was not retained");
  require(vm.unbindHostCall(host_call_address),
          "Legacy VM host call was not removable");
  vm.clearHostCalls();

  auto weapon_input_profile =
      sf::game::syphonFilterUsaV11NativeMissionBridgeProfile();
  weapon_input_profile.weapon_menu_input_entry = host_call_address;
  std::uint32_t weapon_input_held{};
  std::uint32_t weapon_input_delta{};
  vm.bindHostCall(host_call_address,
                  [&weapon_input_held, &weapon_input_delta](
                      sf::game::LegacyHostCallContext &context) {
                    weapon_input_held = context.argument(0);
                    weapon_input_delta = context.argument(1);
                  });
  const auto weapon_input =
      vm.invokeRetailWeaponMenuInput(true, -3, weapon_input_profile, 1U);
  require(weapon_input.completed() && weapon_input.host_calls == 1U &&
              weapon_input_held == 1U &&
              weapon_input_delta == std::bit_cast<std::uint32_t>(-3),
          "Retail weapon-menu host input argument bridge mismatch");
  vm.clearHostCalls();

  vm.bindHostCall(host_call_address,
                  [](sf::game::LegacyHostCallContext &context) {
                    context.rejectHostCall();
                  });
  const auto rejected_host_result = vm.invoke(host_call_address, {}, 1U);
  require(rejected_host_result.execution.reason ==
                  sf::psx::R3000StopReason::unsupported_instruction &&
              rejected_host_result.execution.pc == host_call_address &&
              rejected_host_result.host_calls == 1U,
          "Rejected host call did not stop at the explicit HLE boundary");
  vm.clearHostCalls();

  vm.bindPsxBiosRandomCalls();
  require(vm.runtime().write32(0xa0009010U, 1U),
          "Could not seed BIOS random state");
  const auto random_result = vm.invoke(0xbfc02200U);
  std::uint32_t random_seed{};
  const auto expected_seed = 1U * 0x41c64e6dU + 0x3039U;
  require(random_result.completed() && random_result.host_calls == 1U &&
              random_result.return_value ==
                  ((expected_seed >> 16U) & 0x7fffU) &&
              vm.runtime().read32(0xa0009010U, random_seed) &&
              random_seed == expected_seed,
          "BIOS random HLE mismatch");
  const std::array seeded_value{0x12345678U};
  const auto seed_result = vm.invoke(0xbfc02230U, seeded_value);
  require(seed_result.completed() &&
              vm.runtime().read32(0xa0009010U, random_seed) &&
              random_seed == seeded_value[0],
          "BIOS random seed HLE mismatch");
  vm.clearHostCalls();

  vm.bindPsxLibcStringCalls();
  constexpr std::array string_bytes{
      std::byte{'G'}, std::byte{'a'}, std::byte{'b'},
      std::byte{'e'}, std::byte{},
  };
  require(vm.runtime().loadBytes(0x80010400U, string_bytes),
          "Could not seed libc string HLE");
  const std::array string_argument{0x80010400U};
  const auto string_length = vm.invoke(0x800ec8a4U, string_argument, 1U);
  require(string_length.completed() && string_length.return_value == 4U,
          "libc strlen HLE mismatch");
  vm.clearHostCalls();

  vm.bindPsxVideoTimingCall();
  require(vm.runtime().write32(0x8010f378U, 123U),
          "Could not seed VSync HLE counter");
  const std::array query_mode{0xffffffffU};
  const auto vsync_query = vm.invoke(0x800e3f54U, query_mode);
  const std::array elapsed_mode{1U};
  const auto vsync_elapsed = vm.invoke(0x800e3f54U, elapsed_mode);
  const std::array wait_mode{0U};
  const auto vsync_wait = vm.invoke(0x800e3f54U, wait_mode);
  const auto vsync_query_after_wait = vm.invoke(0x800e3f54U, query_mode);
  std::uint32_t retrace_counter{};
  require(vsync_query.completed() && vsync_query.return_value == 123U &&
              vsync_elapsed.completed() && vsync_elapsed.return_value == 0U &&
              vsync_wait.completed() && vsync_wait.return_value == 0U &&
              vsync_query_after_wait.completed() &&
              vsync_query_after_wait.return_value == 124U &&
              vm.runtime().read32(0x8010f378U, retrace_counter) &&
              retrace_counter == 124U,
          "VSync HLE mismatch");
  vm.clearHostCalls();

  auto outer_profile = sf::game::syphonFilterUsaV11RetailOuterFrameProfile();
  outer_profile.system_clock = 0x80030200U;
  outer_profile.current_state = 0x80030204U;
  outer_profile.gameplay_clock = 0x80030208U;
  outer_profile.gameplay_frame = 0x8003020cU;
  outer_profile.vblank_counter = 0x80030210U;
  outer_profile.renderer_vblank_interval = 0x80030214U;
  outer_profile.player_pointer = 0x80030218U;
  outer_profile.presented_state = 0x8003022cU;
  outer_profile.input_entry = 0x80031000U;
  outer_profile.gameplay_entry = 0x80031004U;
  outer_profile.player_frame_entry = 0x80031008U;
  outer_profile.renderer_frame_entry = 0x8003100cU;
  std::uint32_t gameplay_calls{};
  std::uint32_t player_calls{};
  std::uint32_t renderer_calls{};
  bool renderer_arguments_match{};
  vm.bindHostCall(outer_profile.input_entry,
                  [state = outer_profile.current_state](
                      sf::game::LegacyHostCallContext &context) {
                    static_cast<void>(context.write32(state, 0U));
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(outer_profile.gameplay_entry,
                  [&gameplay_calls](sf::game::LegacyHostCallContext &context) {
                    ++gameplay_calls;
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(outer_profile.player_frame_entry,
                  [&player_calls](sf::game::LegacyHostCallContext &context) {
                    ++player_calls;
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(outer_profile.renderer_frame_entry,
                  [&renderer_calls, &renderer_arguments_match](
                      sf::game::LegacyHostCallContext &context) {
                    ++renderer_calls;
                    renderer_arguments_match =
                        context.argument(0) == 1U && context.argument(1) == 42U;
                    context.setReturnValue(0U);
                  });
  require(
      vm.runtime().write32(outer_profile.system_clock, 0U) &&
          vm.runtime().write32(outer_profile.current_state, 7U) &&
          vm.runtime().write32(outer_profile.gameplay_clock, 0U) &&
          vm.runtime().write32(outer_profile.gameplay_frame, 42U) &&
          vm.runtime().write32(outer_profile.vblank_counter, 0U) &&
          vm.runtime().write16(outer_profile.renderer_vblank_interval, 2U) &&
          vm.runtime().write32(outer_profile.player_pointer, 0x80012000U),
      "Could not seed retail outer-frame ordering test");
  const auto outer_frame_tick = vm.machine().currentTick();
  const auto outer_frame = vm.tickRetailOuterFrame(
      outer_profile, sf::game::syphonFilterUsaV11RetailPlatformTailProfile(),
      1U);
  std::uint32_t system_clock{};
  std::uint32_t gameplay_clock{};
  std::uint32_t renderer_vblanks{};
  require(
      outer_frame.completed() && outer_frame.state_before == 0U &&
          outer_frame.state_after == 0U && gameplay_calls == 1U &&
          player_calls == 1U && renderer_calls == 1U &&
          renderer_arguments_match &&
          vm.runtime().read32(outer_profile.system_clock, system_clock) &&
          system_clock == 1U &&
          vm.runtime().read32(outer_profile.gameplay_clock, gameplay_clock) &&
          gameplay_clock == 1U &&
          vm.runtime().read32(outer_profile.vblank_counter, renderer_vblanks) &&
          renderer_vblanks == 2U &&
          vm.machine().currentTick() == outer_frame_tick,
      "Retail outer frame did not sample input state/render cadence in retail "
      "order or consumed hardware time outside the fixed 20 Hz boundary");
  vm.clearHostCalls();

  outer_profile.display_flags = 0x8003021cU;
  outer_profile.loading_player_frame_entry = 0x80031010U;
  outer_profile.loading_stream_frame_entry = 0x80031014U;
  outer_profile.loading_overlay_frame_entry = 0x80031018U;
  outer_profile.state7_frame_entry = 0x8003101cU;
  outer_profile.stream_resume_entry = 0x80031020U;
  auto streaming_tail_profile =
      sf::game::syphonFilterUsaV11RetailPlatformTailProfile();
  streaming_tail_profile.delayed_callbacks_entry = 0x80031024U;
  streaming_tail_profile.fade_step = 0x80030220U;
  streaming_tail_profile.fade_current = 0x80030222U;
  streaming_tail_profile.fade_callback = 0x80030224U;
  streaming_tail_profile.fade_initialized = 0x80030228U;
  streaming_tail_profile.fade_floor_rgb = 0x80030229U;
  streaming_tail_profile.fade_callback_dispatch_entry = 0x80031028U;

  std::vector<std::uint32_t> state7_order;
  vm.bindHostCall(outer_profile.input_entry,
                  [&state7_order](sf::game::LegacyHostCallContext &context) {
                    state7_order.push_back(1U);
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(outer_profile.state7_frame_entry,
                  [&state7_order](sf::game::LegacyHostCallContext &context) {
                    state7_order.push_back(2U);
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(outer_profile.loading_stream_frame_entry,
                  [&state7_order](sf::game::LegacyHostCallContext &context) {
                    require(
                        context.argument(0) == 0U,
                        "Retail state 7 stream frame lost its zero argument");
                    state7_order.push_back(3U);
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(streaming_tail_profile.delayed_callbacks_entry,
                  [&state7_order](sf::game::LegacyHostCallContext &context) {
                    require(
                        context.argument(0) == 0U,
                        "Streaming platform tail advanced delayed callbacks");
                    state7_order.push_back(4U);
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(outer_profile.renderer_frame_entry,
                  [&state7_order](sf::game::LegacyHostCallContext &context) {
                    state7_order.push_back(99U);
                    context.setReturnValue(0U);
                  });
  require(
      vm.runtime().write32(outer_profile.system_clock, 0U) &&
          vm.runtime().write32(outer_profile.current_state, 7U) &&
          vm.runtime().write32(outer_profile.presented_state, 7U) &&
          vm.runtime().write32(outer_profile.vblank_counter, 0U) &&
          vm.runtime().write16(outer_profile.renderer_vblank_interval, 3U) &&
          vm.runtime().write16(streaming_tail_profile.fade_step, 0U) &&
          vm.runtime().write16(streaming_tail_profile.fade_current, 0U) &&
          vm.runtime().write32(streaming_tail_profile.fade_callback, 0U) &&
          vm.runtime().write8(streaming_tail_profile.fade_initialized, 0U) &&
          vm.runtime().write8(streaming_tail_profile.fade_floor_rgb, 0U),
      "Could not seed retail state-7 streaming frame");
  const auto state7_tick = vm.machine().currentTick();
  const auto state7_mixed_frames = vm.machine().spu().state().mixed_frames;
  const auto state7_frame =
      vm.tickRetailOuterFrame(outer_profile, streaming_tail_profile, 1U);
  require(
      state7_frame.completed() && state7_frame.state_before == 7U &&
          state7_frame.state_after == 7U && !state7_frame.renderer_tail &&
          state7_frame.platform_tail.completed() &&
          state7_order == std::vector<std::uint32_t>{1U, 2U, 3U, 4U} &&
          vm.machine().currentTick() == state7_tick &&
          vm.machine().spu().state().mixed_frames == state7_mixed_frames,
      "Retail state 7 did not run input, loader and platform tail in order");
  vm.clearHostCalls();

  std::vector<std::uint32_t> state7_exit_order;
  vm.bindHostCall(
      outer_profile.input_entry,
      [&state7_exit_order](sf::game::LegacyHostCallContext &context) {
        state7_exit_order.push_back(1U);
        context.setReturnValue(0U);
      });
  vm.bindHostCall(outer_profile.state7_frame_entry,
                  [&state7_exit_order,
                   outer_profile](sf::game::LegacyHostCallContext &context) {
                    state7_exit_order.push_back(2U);
                    static_cast<void>(
                        context.write32(outer_profile.current_state, 9U));
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(
      outer_profile.stream_resume_entry,
      [&state7_exit_order,
       outer_profile](sf::game::LegacyHostCallContext &context) {
        std::uint32_t temporary_state{};
        require(
            context.read32(outer_profile.current_state, temporary_state) &&
                temporary_state == 0U,
            "Retail state-7 resume did not observe temporary gameplay state");
        state7_exit_order.push_back(3U);
        context.setReturnValue(0U);
      });
  vm.bindHostCall(
      streaming_tail_profile.delayed_callbacks_entry,
      [&state7_exit_order,
       outer_profile](sf::game::LegacyHostCallContext &context) {
        std::uint32_t restored_state{};
        require(context.read32(outer_profile.current_state, restored_state) &&
                    restored_state == 9U,
                "Retail state-7 resume did not restore the loader state");
        state7_exit_order.push_back(4U);
        context.setReturnValue(0U);
      });
  vm.bindHostCall(
      outer_profile.renderer_frame_entry,
      [&state7_exit_order](sf::game::LegacyHostCallContext &context) {
        state7_exit_order.push_back(99U);
        context.setReturnValue(0U);
      });
  require(vm.runtime().write32(outer_profile.current_state, 7U),
          "Could not seed retail state-7 exit frame");
  const auto state7_exit =
      vm.tickRetailOuterFrame(outer_profile, streaming_tail_profile, 1U);
  require(
      state7_exit.completed() && state7_exit.state_before == 7U &&
          state7_exit.state_after == 9U && !state7_exit.renderer_tail &&
          state7_exit.tail_skipped && !state7_exit.platform_tail.completed() &&
          state7_exit_order == std::vector<std::uint32_t>{1U, 2U, 3U},
      "Retail state-7 exit did not resume under state 0, restore state 9 and "
      "skip the stale presentation tail");
  vm.clearHostCalls();

  std::vector<std::uint32_t> state9_order;
  bool state9_display_masked{};
  vm.bindHostCall(outer_profile.input_entry,
                  [&state9_order](sf::game::LegacyHostCallContext &context) {
                    state9_order.push_back(1U);
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(outer_profile.loading_player_frame_entry,
                  [&state9_order](sf::game::LegacyHostCallContext &context) {
                    require(context.argument(0) == 0x80012000U,
                            "Retail state 9 lost its player pointer");
                    state9_order.push_back(2U);
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(outer_profile.loading_stream_frame_entry,
                  [&state9_order, &state9_display_masked,
                   outer_profile](sf::game::LegacyHostCallContext &context) {
                    std::uint16_t flags{};
                    state9_display_masked =
                        context.read16(outer_profile.display_flags, flags) &&
                        (flags & 2U) == 0U;
                    require(
                        context.argument(0) == 0U,
                        "Retail state 9 stream frame lost its zero argument");
                    state9_order.push_back(3U);
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(
      outer_profile.loading_overlay_frame_entry,
      [&state9_order, outer_profile](sf::game::LegacyHostCallContext &context) {
        state9_order.push_back(4U);
        static_cast<void>(context.write32(outer_profile.current_state, 0U));
        context.setReturnValue(0U);
      });
  vm.bindHostCall(outer_profile.renderer_frame_entry,
                  [&state9_order](sf::game::LegacyHostCallContext &context) {
                    require(
                        context.argument(0) == 1U && context.argument(1) == 42U,
                        "Retail state-9 gameplay edge lost renderer arguments");
                    state9_order.push_back(5U);
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(streaming_tail_profile.delayed_callbacks_entry,
                  [&state9_order](sf::game::LegacyHostCallContext &context) {
                    state9_order.push_back(99U);
                    context.setReturnValue(0U);
                  });
  require(
      vm.runtime().write32(outer_profile.current_state, 9U) &&
          vm.runtime().write32(outer_profile.player_pointer, 0x80012000U) &&
          vm.runtime().write32(outer_profile.gameplay_frame, 42U) &&
          vm.runtime().write32(outer_profile.vblank_counter, 0U) &&
          vm.runtime().write16(outer_profile.renderer_vblank_interval, 3U) &&
          vm.runtime().write16(outer_profile.display_flags, 0xffffU),
      "Could not seed retail state-9 streaming frame");
  const auto state9_frame =
      vm.tickRetailOuterFrame(outer_profile, streaming_tail_profile, 1U);
  std::uint16_t state9_flags{};
  require(state9_frame.completed() && state9_frame.state_before == 9U &&
              state9_frame.state_after == 0U && !state9_frame.renderer_tail &&
              state9_frame.tail_skipped &&
              !state9_frame.platform_tail.delayed_callbacks.completed() &&
              state9_display_masked &&
              vm.runtime().read16(outer_profile.display_flags, state9_flags) &&
              state9_flags == 0xfffdU &&
              state9_order == std::vector<std::uint32_t>{1U, 2U, 3U, 4U},
          "Retail state 9 did not run loader calls before skipping the stale "
          "presentation tail");
  vm.clearHostCalls();

  auto state2_profile =
      sf::game::syphonFilterUsaV11RetailState2TransitionProfile();
  state2_profile.current_state = 0x80030300U;
  state2_profile.state_depth = 0x80030304U;
  state2_profile.transition = 0x80030308U;
  state2_profile.pop_state_entry = 0x80031100U;
  state2_profile.push_state_entry = 0x80031104U;
  state2_profile.common_init_entry = 0x80031108U;
  state2_profile.maximum_dispatches = 3U;
  std::vector<std::uint32_t> state2_call_order;
  vm.bindHostCall(
      state2_profile.pop_state_entry,
      [&state2_call_order,
       state2_profile](sf::game::LegacyHostCallContext &context) {
        std::uint32_t depth{};
        require(context.read32(state2_profile.state_depth, depth) &&
                    depth == 2U,
                "Retail state-2 pop observed the wrong stack depth");
        state2_call_order.push_back(1U);
        static_cast<void>(context.write32(state2_profile.state_depth, 1U));
        static_cast<void>(context.write32(state2_profile.current_state, 0U));
        context.setReturnValue(0U);
      });
  vm.bindHostCall(
      state2_profile.common_init_entry,
      [&state2_call_order](sf::game::LegacyHostCallContext &context) {
        require(context.argument(0) == 3U,
                "Retail state-2 dispatcher lost transition 3");
        state2_call_order.push_back(2U);
        context.setReturnValue(0U);
      });
  require(vm.runtime().write32(state2_profile.current_state, 2U) &&
              vm.runtime().write32(state2_profile.state_depth, 2U) &&
              vm.runtime().write8(state2_profile.transition, 3U) &&
              vm.runtime().write8(state2_profile.transition + 1U, 0xa5U) &&
              vm.runtime().write8(state2_profile.transition + 2U, 0x5aU) &&
              vm.runtime().write8(state2_profile.transition + 3U, 0xc3U),
          "Could not seed retail state-2 transition");
  const auto state2_transition =
      vm.dispatchRetailState2Transition(state2_profile, 1U);
  require(state2_transition.completed() && state2_transition.dispatches == 1U &&
              state2_transition.final_state == 0U &&
              state2_call_order == std::vector<std::uint32_t>{1U, 2U},
          "Retail state-2 preamble did not pop then restore before PAD");
  std::uint8_t transition_neighbor1{};
  std::uint8_t transition_neighbor2{};
  std::uint8_t transition_neighbor3{};
  require(vm.runtime().read8(state2_profile.transition + 1U,
                             transition_neighbor1) &&
              vm.runtime().read8(state2_profile.transition + 2U,
                                 transition_neighbor2) &&
              vm.runtime().read8(state2_profile.transition + 3U,
                                 transition_neighbor3) &&
              transition_neighbor1 == 0xa5U && transition_neighbor2 == 0x5aU &&
              transition_neighbor3 == 0xc3U,
          "Retail state-2 dispatcher touched bytes adjacent to transition");
  vm.clearHostCalls();

  std::uint32_t transition2_pop_calls{};
  std::uint32_t transition2_common_calls{};
  std::uint32_t transition2_common_argument{};
  vm.bindHostCall(state2_profile.pop_state_entry,
                  [&transition2_pop_calls,
                   state2_profile](sf::game::LegacyHostCallContext &context) {
                    ++transition2_pop_calls;
                    static_cast<void>(
                        context.write32(state2_profile.current_state, 0U));
                    context.setReturnValue(0U);
                  });
  vm.bindHostCall(state2_profile.common_init_entry,
                  [&transition2_common_calls, &transition2_common_argument,
                   state2_profile](sf::game::LegacyHostCallContext &context) {
                    ++transition2_common_calls;
                    transition2_common_argument = context.argument(0);
                    static_cast<void>(
                        context.write32(state2_profile.current_state, 0U));
                    context.setReturnValue(0U);
                  });
  require(vm.runtime().write32(state2_profile.current_state, 2U) &&
              vm.runtime().write32(state2_profile.state_depth, 7U) &&
              vm.runtime().write8(state2_profile.transition, 2U),
          "Could not seed retail state-2 reset transition");
  const auto state2_reset =
      vm.dispatchRetailState2Transition(state2_profile, 1U);
  std::uint32_t reset_depth{};
  std::uint8_t reset_transition{};
  require(state2_reset.completed() && state2_reset.dispatches == 1U &&
              state2_reset.final_state == 0U && transition2_pop_calls == 0U &&
              transition2_common_calls == 1U &&
              transition2_common_argument == 4U &&
              vm.runtime().read32(state2_profile.state_depth, reset_depth) &&
              reset_depth == 1U &&
              vm.runtime().read8(state2_profile.transition, reset_transition) &&
              reset_transition == 4U &&
              vm.runtime().read8(state2_profile.transition + 1U,
                                 transition_neighbor1) &&
              vm.runtime().read8(state2_profile.transition + 2U,
                                 transition_neighbor2) &&
              vm.runtime().read8(state2_profile.transition + 3U,
                                 transition_neighbor3) &&
              transition_neighbor1 == 0xa5U && transition_neighbor2 == 0x5aU &&
              transition_neighbor3 == 0xc3U,
          "Retail transition 2 did not reset depth and dispatch transition 4");
  vm.clearHostCalls();

  std::uint32_t transition5_pop_calls{};
  std::uint32_t transition5_push_calls{};
  std::uint32_t transition5_common_calls{};
  std::uint32_t transition5_push_argument{};
  vm.bindHostCall(
      state2_profile.pop_state_entry,
      [&transition5_pop_calls,
       state2_profile](sf::game::LegacyHostCallContext &context) {
        ++transition5_pop_calls;
        static_cast<void>(context.write32(state2_profile.state_depth, 1U));
        static_cast<void>(context.write32(state2_profile.current_state, 0U));
        context.setReturnValue(0U);
      });
  vm.bindHostCall(
      state2_profile.push_state_entry,
      [&transition5_push_calls, &transition5_push_argument,
       state2_profile](sf::game::LegacyHostCallContext &context) {
        ++transition5_push_calls;
        transition5_push_argument = context.argument(0);
        static_cast<void>(context.write32(state2_profile.state_depth, 2U));
        static_cast<void>(context.write32(state2_profile.current_state, 4U));
        context.setReturnValue(0U);
      });
  vm.bindHostCall(
      state2_profile.common_init_entry,
      [&transition5_common_calls](sf::game::LegacyHostCallContext &context) {
        ++transition5_common_calls;
        context.setReturnValue(0U);
      });
  require(vm.runtime().write32(state2_profile.current_state, 2U) &&
              vm.runtime().write32(state2_profile.state_depth, 2U) &&
              vm.runtime().write8(state2_profile.transition, 5U),
          "Could not seed retail state-2 title transition");
  const auto state2_title =
      vm.dispatchRetailState2Transition(state2_profile, 1U);
  require(state2_title.completed() && state2_title.dispatches == 1U &&
              state2_title.final_state == 4U && transition5_pop_calls == 1U &&
              transition5_push_calls == 1U && transition5_push_argument == 4U &&
              transition5_common_calls == 0U,
          "Retail transition 5 did not pop then push state 4");
  vm.clearHostCalls();

  sf::game::LegacyMissionBridgeState active_state2_mission;
  sf::game::LegacyMissionBridgeState failed_state2_mission;
  sf::game::LegacyMissionBridgeState successful_state2_mission;
  failed_state2_mission.terminal = true;
  failed_state2_mission.failure = true;
  successful_state2_mission.terminal = true;
  successful_state2_mission.success = true;
  require(
      sf::game::legacyRetailState2DispatchAllowed(2U, active_state2_mission) &&
          sf::game::legacyRetailState2DispatchAllowed(
              2U, successful_state2_mission) &&
          !sf::game::legacyRetailState2DispatchAllowed(2U,
                                                       failed_state2_mission) &&
          !sf::game::legacyRetailState2DispatchAllowed(0U,
                                                       active_state2_mission),
      "Terminal failure was allowed through the state-2 dispatcher");
  require(!sf::game::legacyRetailStreamingState(0U) &&
              !sf::game::legacyRetailStreamingState(5U) &&
              sf::game::legacyRetailStreamingState(7U) &&
              sf::game::legacyRetailStreamingState(9U) &&
              sf::game::legacyRuntimeCheckpointCaptureAllowed(0U) &&
              sf::game::legacyRuntimeCheckpointCaptureAllowed(5U) &&
              !sf::game::legacyRuntimeCheckpointCaptureAllowed(2U) &&
              !sf::game::legacyRuntimeCheckpointCaptureAllowed(3U) &&
              !sf::game::legacyRuntimeCheckpointCaptureAllowed(4U) &&
              !sf::game::legacyRuntimeCheckpointCaptureAllowed(7U) &&
              !sf::game::legacyRuntimeCheckpointCaptureAllowed(9U),
          "Retail checkpoint transaction states were misclassified");

  sf::game::LegacyGameplayBridgeState coherent_renderer;
  coherent_renderer.world_model_count = 1U;
  coherent_renderer.player.room = 0;
  coherent_renderer.active_world_models = {0U};
  coherent_renderer.dynamic_first_slot = 1U;
  coherent_renderer.objects.resize(1U);
  coherent_renderer.objects[0].slot = 0U;
  coherent_renderer.objects[0].definition = 17U;
  coherent_renderer.objects[0].class_id = 1;
  coherent_renderer.objects[0].resident = true;
  coherent_renderer.objects[0].position = {101, 202, 303};
  sf::game::LegacyMissionBridgeState coherent_ui;
  coherent_ui.player_slot = 0;
  coherent_ui.completed_objectives = 0x05U;
  coherent_renderer.scrim.resource_present = true;
  coherent_renderer.scrim.vram_moves_active = true;
  coherent_renderer.scrim.vram_moves.assign(
      13U, sf::game::LegacyVramMoveBridgeState{0, 0, 1, 1, 1, 0});
  const auto coherent_source = sf::game::buildLegacyPresentationFrame(
      70U, 40U, coherent_renderer, coherent_ui);
  constexpr std::array streaming_edge{
      sf::game::LegacyPresentationCommandType::checkpoint_commit};
  const auto coherent_republish = sf::game::republishLegacyCoherentPresentation(
      coherent_source, 70U, 41U, streaming_edge);
  const auto coherent_republish_again =
      sf::game::republishLegacyCoherentPresentation(coherent_republish, 71U,
                                                    42U);
  require(
      coherent_source && coherent_republish && coherent_republish_again &&
          coherent_republish->sequence == 71U &&
          coherent_republish->guest_frame == 41U &&
          coherent_republish_again->sequence == 72U &&
          coherent_republish_again->guest_frame == 42U &&
          coherent_republish->renderer->state.objects[0].definition == 17U &&
          coherent_republish->renderer->state.objects[0].position.x == 101 &&
          !coherent_republish->renderer->state.scrim.vram_moves_active &&
          coherent_republish->renderer->state.scrim.vram_moves.size() == 13U &&
          coherent_republish->ui->mission.completed_objectives == 0x05U &&
          coherent_republish->contains(
              sf::game::LegacyPresentationCommandType::checkpoint_commit) &&
          sf::game::legacyPresentationFrameConsumable(
              *coherent_republish, coherent_source->sequence) &&
          sf::game::legacyPresentationFrameConsumable(
              *coherent_republish_again, coherent_republish->sequence),
      "Streaming republish did not preserve one coherent frame and monotonic "
      "sequence");

  constexpr std::uint8_t failure_restart_request = 1U << 2U;
  std::uint8_t pending_transition = failure_restart_request;
  sf::game::LegacyGameplayBridgeState transition_renderer;
  transition_renderer.world_model_count = 1U;
  transition_renderer.player.room = 0;
  transition_renderer.active_world_models = {0U};
  transition_renderer.objects.resize(1U);
  sf::game::LegacyMissionBridgeState transition_ui;
  transition_ui.player_slot = 0;
  transition_ui.terminal = true;
  transition_ui.failure = true;
  const auto stale_transition_frame = sf::game::buildLegacyPresentationFrame(
      41U, 17U, transition_renderer, transition_ui);
  require(
      stale_transition_frame &&
          !sf::game::consumeLegacyPresentationTransitionRequest(
              pending_transition, failure_restart_request,
              stale_transition_frame,
              sf::game::LegacyPresentationCommandType::restart_after_failure) &&
          pending_transition == failure_restart_request,
      "Stale presentation frame consumed the pending failure restart");
  constexpr std::array failure_edge{
      sf::game::LegacyPresentationCommandType::restart_after_failure};
  const auto published_transition_frame =
      sf::game::buildLegacyPresentationFrame(42U, 17U, transition_renderer,
                                             transition_ui, failure_edge);
  require(
      published_transition_frame &&
          sf::game::legacyPresentationFrameConsumable(
              *published_transition_frame, stale_transition_frame->sequence) &&
          sf::game::consumeLegacyPresentationTransitionRequest(
              pending_transition, failure_restart_request,
              published_transition_frame,
              sf::game::LegacyPresentationCommandType::restart_after_failure) &&
          pending_transition == 0U &&
          !sf::game::consumeLegacyPresentationTransitionRequest(
              pending_transition, failure_restart_request,
              published_transition_frame,
              sf::game::LegacyPresentationCommandType::restart_after_failure),
      "Published failure command was not consumed exactly once");
  vm.bindPsxCriticalSectionCalls();
  const auto enter_critical = vm.invoke(0x800e3f34U, {}, 1U);
  const auto exit_critical = vm.invoke(0x800e3f44U, {}, 1U);
  require(enter_critical.completed() && enter_critical.return_value == 1U &&
              exit_critical.completed() && exit_critical.return_value == 0U,
          "Critical-section HLE mismatch");
  vm.clearHostCalls();

  vm.bindSyphonFilterUsaV11PlatformCalls();
  auto retail_profile = sf::game::syphonFilterUsaV11RetailFrameProfile();
  retail_profile.frame_entry = 0x800e6e74U;
  const auto retail_frame = vm.tickRetailFrame(retail_profile, 1U);
  require(retail_frame.completed() && retail_frame.host_calls == 1U &&
              retail_frame.return_value == 0U,
          "Retail frame GPU boundary mismatch");
  vm.clearHostCalls();

  auto tail_profile = sf::game::syphonFilterUsaV11RetailPlatformTailProfile();
  tail_profile.delayed_callbacks_entry = 0x80030000U;
  tail_profile.fade_callback_dispatch_entry = 0x80030004U;
  tail_profile.fade_step = 0x80030100U;
  tail_profile.fade_current = 0x80030102U;
  tail_profile.fade_callback = 0x80030104U;
  tail_profile.fade_initialized = 0x80030108U;
  tail_profile.fade_floor_rgb = 0x80030109U;
  std::uint32_t delayed_argument{};
  std::uint32_t fade_callback_calls{};
  vm.bindHostCall(
      tail_profile.delayed_callbacks_entry,
      [&delayed_argument](sf::game::LegacyHostCallContext &context) {
        delayed_argument = context.argument(0);
        context.setReturnValue(0U);
      });
  vm.bindHostCall(
      tail_profile.fade_callback_dispatch_entry,
      [&fade_callback_calls](sf::game::LegacyHostCallContext &context) {
        ++fade_callback_calls;
        context.setReturnValue(0U);
      });
  require(
      vm.runtime().write16(tail_profile.fade_step,
                           std::bit_cast<std::uint16_t>(std::int16_t{-7})) &&
          vm.runtime().write16(tail_profile.fade_current, 0xffU) &&
          vm.runtime().write32(tail_profile.fade_callback, 0x8001625cU) &&
          vm.runtime().write8(tail_profile.fade_initialized, 1U) &&
          vm.runtime().write8(tail_profile.fade_floor_rgb, 15U),
      "Could not seed retail headless tail");
  const auto initialized_tail =
      vm.tickRetailPlatformTail(false, tail_profile, 1U);
  std::uint16_t fade_current{};
  std::uint8_t fade_initialized{};
  require(
      initialized_tail.completed() && delayed_argument == 0U &&
          vm.runtime().read16(tail_profile.fade_current, fade_current) &&
          fade_current == 15U &&
          vm.runtime().read8(tail_profile.fade_initialized, fade_initialized) &&
          fade_initialized == 0U,
      "Retail headless tail did not initialize fade state");
  require(vm.runtime().write16(tail_profile.fade_current, 0xffU),
          "Could not restart retail fade");
  const auto fading_tail = vm.tickRetailPlatformTail(true, tail_profile, 1U);
  require(fading_tail.completed() && delayed_argument == 1U &&
              vm.runtime().read16(tail_profile.fade_current, fade_current) &&
              fade_current == 248U && fade_callback_calls == 0U,
          "Retail headless tail fade step mismatch");
  require(vm.runtime().write16(tail_profile.fade_current, 15U),
          "Could not finish retail fade");
  const auto completed_tail = vm.tickRetailPlatformTail(true, tail_profile, 1U);
  require(completed_tail.completed() && completed_tail.fade_callback &&
              fade_callback_calls == 1U,
          "Retail headless tail did not dispatch fade callback");
  vm.clearHostCalls();

  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  constexpr std::array expected_neutral_pad{
      std::byte{0x00}, std::byte{0x73}, std::byte{0xff}, std::byte{0xff},
      std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
  };
  const auto native_profile =
      sf::game::syphonFilterUsaV11NativeMissionBridgeProfile();
  std::array<std::byte, expected_neutral_pad.size()> neutral_pad{};
  std::uint8_t second_pad_status{};
  std::uint32_t left_x{};
  std::uint32_t left_y{};
  std::uint32_t right_x{};
  std::uint32_t right_y{};
  require(
      vm.writeHostPadState(sf::game::LegacyHostPadState{}) &&
          vm.runtime().copyBytes(native_profile.raw_pad0, neutral_pad) &&
          neutral_pad == expected_neutral_pad &&
          vm.runtime().read8(native_profile.raw_pad1, second_pad_status) &&
          second_pad_status == 0xffU &&
          vm.runtime().read32(native_profile.processed_pad0 + 0x1cU, left_x) &&
          vm.runtime().read32(native_profile.processed_pad0 + 0x24U, left_y) &&
          vm.runtime().read32(native_profile.processed_pad0 + 0x2cU, right_x) &&
          vm.runtime().read32(native_profile.processed_pad0 + 0x34U, right_y) &&
          left_x == 0U && left_y == 0U && right_x == 0U && right_y == 0U,
      "Retail neutral analog-pad packet/derived axes mismatch");
  const sf::game::LegacyHostPadState directional_pad{
      .left_x = 0x10U,
      .left_y = 0x20U,
      .right_x = 0xe0U,
      .right_y = 0xf0U,
  };
  require(
      vm.writeHostPadState(directional_pad) &&
          vm.runtime().read32(native_profile.processed_pad0 + 0x1cU, left_x) &&
          vm.runtime().read32(native_profile.processed_pad0 + 0x24U, left_y) &&
          vm.runtime().read32(native_profile.processed_pad0 + 0x2cU, right_x) &&
          vm.runtime().read32(native_profile.processed_pad0 + 0x34U, right_y) &&
          left_x == 0xffffff90U && left_y == 0x60U && right_x == 0x60U &&
          right_y == 0xffffff90U,
      "Retail processed-pad left/right analog order mismatch");
  constexpr std::uint16_t triangle_button = 0x1000U;
  constexpr std::array expected_triangle_pad{
      std::byte{0x00}, std::byte{0x73}, std::byte{0xff}, std::byte{0xef},
      std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
  };
  std::array<std::byte, expected_triangle_pad.size()> triangle_pad{};
  std::uint16_t processed_buttons{};
  std::uint32_t processed_face_vertical{};
  require(vm.writeHostPadState(sf::game::LegacyHostPadState{
              .buttons = triangle_button,
          }) &&
              vm.runtime().copyBytes(native_profile.raw_pad0, triangle_pad) &&
              vm.runtime().read16(native_profile.processed_pad0 + 4U,
                                  processed_buttons) &&
              vm.runtime().read32(native_profile.processed_pad0 + 0x14U,
                                  processed_face_vertical) &&
              triangle_pad == expected_triangle_pad &&
              static_cast<std::uint16_t>(
                  ~(static_cast<std::uint16_t>(triangle_pad[2]) * 0x100U +
                    static_cast<std::uint16_t>(triangle_pad[3]))) == 0x0010U &&
              processed_buttons == 0x0010U && processed_face_vertical == 0x7fU,
          "Host interaction must match the retail serial PAD mask layout");

  auto room_profile = native_profile;
  room_profile.current_room = 0x80030220U;
  room_profile.room_change_entry = 0x80031024U;
  room_profile.stream_unlock_entry = 0x80031028U;
  std::uint32_t stream_unlock_calls{};
  std::uint32_t room_change_calls{};
  std::uint32_t requested_room{};
  vm.bindHostCall(
      room_profile.stream_unlock_entry,
      [&stream_unlock_calls](sf::game::LegacyHostCallContext &context) {
        ++stream_unlock_calls;
        context.setReturnValue(0U);
      });
  vm.bindHostCall(
      room_profile.room_change_entry,
      [&room_change_calls, &requested_room, room = room_profile.current_room](
          sf::game::LegacyHostCallContext &context) {
        ++room_change_calls;
        requested_room = context.argument(0);
        static_cast<void>(
            context.write16(room, static_cast<std::uint16_t>(requested_room)));
        context.setReturnValue(0U);
      });
  std::uint16_t synchronized_room{};
  require(
      vm.runtime().write16(room_profile.current_room, 7U) &&
          vm.synchronizeHostRoom(7, room_profile, 1U) &&
          stream_unlock_calls == 0U && room_change_calls == 0U &&
          vm.synchronizeHostRoom(11, room_profile, 1U) &&
          stream_unlock_calls == 1U && room_change_calls == 1U &&
          requested_room == 11U &&
          vm.runtime().read16(room_profile.current_room, synchronized_room) &&
          synchronized_room == 11U,
      "Native room bridge did not use the exact retail room-change boundary");
  require(vm.unbindHostCall(room_profile.stream_unlock_entry),
          "Could not remove retail stream-unlock test binding");
  require(vm.unbindHostCall(room_profile.room_change_entry),
          "Could not remove retail room-change test binding");

  auto stream_cd = std::make_shared<sf::game::LegacyVirtualCd>();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(stream_cd);
  constexpr std::uint32_t stream_transfer_pointer = 0x8011609cU;
  constexpr std::uint32_t raw_read_entry = 0x800f0384U;
  constexpr std::uint32_t raw_sync_entry = 0x800f0520U;
  constexpr std::uint32_t cd_control_entry = 0x800ed5c0U;
  const std::array failed_raw_arguments{1U, 0x801f2000U, 0U};
  const auto failed_raw_read =
      vm.invoke(raw_read_entry, failed_raw_arguments, 1U);
  const std::array get_status_arguments{1U, 0U};
  const auto recovered_get_status =
      vm.invoke(cd_control_entry, get_status_arguments, 1U);
  const auto recovered_cdrom = vm.machine().cdrom().captureState();
  require(failed_raw_read.completed() && failed_raw_read.return_value == 0U &&
              recovered_get_status.completed() &&
              recovered_get_status.return_value == 1U &&
              recovered_cdrom.interrupt_flags == 0U &&
              recovered_cdrom.reading == 0U &&
              recovered_cdrom.command_event.pending == 0U &&
              recovered_cdrom.sector_event.pending == 0U,
          "Failed virtual-CD transfer poisoned later controller commands");
  const std::array stream_sync_arguments{1U, 0U};
  const std::array direct_sync_arguments{0U, 0U};
  require(vm.runtime().write32(stream_transfer_pointer, 0U),
          "Could not seed idle virtual-CD stream state");
  const auto idle_stream_sync =
      vm.invoke(raw_sync_entry, stream_sync_arguments, 1U);
  require(vm.runtime().write32(stream_transfer_pointer, 0x801f1000U),
          "Could not seed active virtual-CD stream state");
  const auto active_stream_sync =
      vm.invoke(raw_sync_entry, stream_sync_arguments, 1U);
  const auto direct_raw_sync =
      vm.invoke(raw_sync_entry, direct_sync_arguments, 1U);
  require(idle_stream_sync.completed() && idle_stream_sync.return_value == 2U &&
              active_stream_sync.completed() &&
              active_stream_sync.return_value == 0U &&
              direct_raw_sync.completed() && direct_raw_sync.return_value == 0U,
          "Virtual-CD sync did not preserve retail idle/active stream polling");
  vm.bindSyphonFilterUsaV11VirtualCdCalls(nullptr);

  auto impact_profile = native_profile;
  impact_profile.event_entry = 0x80031020U;
  std::array<std::uint32_t, 8U> impact_arguments{};
  vm.bindHostCall(
      impact_profile.event_entry,
      [&impact_arguments](sf::game::LegacyHostCallContext &context) {
        for (std::size_t index = 0U; index < impact_arguments.size(); ++index) {
          impact_arguments[index] = context.argument(index);
        }
        context.setReturnValue(0U);
      });
  const auto impact = vm.queueHostImpact(-2, 140, impact_profile, 1U);
  require(impact.completed() &&
              impact_arguments ==
                  std::array<std::uint32_t, 8U>{0x0dU, 4U, 0xfffffffeU, 140U,
                                                0U, 0U, 0U, 0U},
          "Retail host impact event ordering/arguments mismatch");
  require(vm.unbindHostCall(impact_profile.event_entry),
          "Could not remove retail host impact test binding");

  auto inventory_profile = native_profile;
  inventory_profile.player_pointer = 0x801f0000U;
  inventory_profile.object_records_pointer = 0x801f0004U;
  inventory_profile.object_count = 0x801f0008U;
  inventory_profile.inventory_current_weapon = 0x801f000cU;
  inventory_profile.inventory_owned_weapons = 0x801f0010U;
  inventory_profile.inventory_ammo_table = 0x801f0100U;
  constexpr std::uint32_t inventory_player = 0x801f0200U;
  constexpr std::uint32_t inventory_record = 0x801f0300U;
  constexpr std::uint32_t inventory_motion = 0x801f0400U;
  constexpr std::uint32_t inventory_health = 0x801f0600U;
  constexpr std::uint32_t inventory_root = 0x801f0800U;
  constexpr std::uint32_t inventory_matrix = 0x801f0900U;
  require(
      vm.runtime().write32(inventory_profile.player_pointer,
                           inventory_player) &&
          vm.runtime().write32(inventory_profile.object_records_pointer,
                               inventory_record) &&
          vm.runtime().write32(inventory_profile.object_count, 1U) &&
          vm.runtime().write32(inventory_record + 0x34U, inventory_player) &&
          vm.runtime().write32(inventory_player + 8U, inventory_root) &&
          vm.runtime().write32(inventory_player + 0x0cU, inventory_motion) &&
          vm.runtime().write32(inventory_player + 0x18U, inventory_health) &&
          vm.runtime().write32(inventory_root + 0x0cU, inventory_matrix),
      "Could not seed retail inventory bridge player");
  sf::game::LegacyInventoryBridgeState inventory_state;
  inventory_state.current_weapon = 8U;
  inventory_state.owned_weapons = (std::uint32_t{1U} << 0U) |
                                  (std::uint32_t{1U} << 8U) |
                                  (std::uint32_t{1U} << 19U);
  for (std::size_t weapon = 0U;
       weapon < sf::game::legacy_inventory_weapon_count; ++weapon) {
    inventory_state.magazines[weapon] =
        static_cast<std::uint16_t>(10U + weapon);
    inventory_state.reserves[weapon] =
        static_cast<std::uint16_t>(100U + weapon);
  }
  std::uint32_t inventory_current{};
  std::uint32_t inventory_owned{};
  std::uint8_t inventory_record_current{};
  std::uint16_t inventory_reserve_8{};
  std::uint16_t inventory_magazine_8{};
  std::uint16_t inventory_reserve_25{};
  std::uint16_t inventory_magazine_25{};
  require(
      vm.writeHostInventoryState(inventory_state, inventory_profile) &&
          vm.runtime().read32(inventory_profile.inventory_current_weapon,
                              inventory_current) &&
          vm.runtime().read32(inventory_profile.inventory_owned_weapons,
                              inventory_owned) &&
          vm.runtime().read8(inventory_record + 0x24U,
                             inventory_record_current) &&
          vm.runtime().read16(inventory_profile.inventory_ammo_table + 8U * 4U,
                              inventory_reserve_8) &&
          vm.runtime().read16(inventory_profile.inventory_ammo_table + 8U * 4U +
                                  2U,
                              inventory_magazine_8) &&
          vm.runtime().read16(inventory_profile.inventory_ammo_table + 25U * 4U,
                              inventory_reserve_25) &&
          vm.runtime().read16(inventory_profile.inventory_ammo_table +
                                  25U * 4U + 2U,
                              inventory_magazine_25) &&
          inventory_current == 8U && inventory_record_current == 8U &&
          inventory_owned == inventory_state.owned_weapons &&
          inventory_reserve_8 == 108U && inventory_magazine_8 == 18U &&
          inventory_reserve_25 == 125U && inventory_magazine_25 == 35U,
      "Retail inventory current/owned/reserve/magazine layout mismatch");

  auto mission_bridge_profile = inventory_profile;
  mission_bridge_profile.mission_progress_pointer = 0x801f1000U;
  mission_bridge_profile.mission_terminal_latch = 0x801f1004U;
  mission_bridge_profile.mission_success_latch = 0x801f1005U;
  mission_bridge_profile.mission_transition_latch = 0x801f1006U;
  mission_bridge_profile.mission_failure_flag = 0x801f1007U;
  mission_bridge_profile.mission_completed_flag = 0x801f1008U;
  mission_bridge_profile.weapon_menu_state = 0x801f1010U;
  mission_bridge_profile.weapon_menu_dirty = 0x801f1014U;
  mission_bridge_profile.weapon_menu_controller_ready = 0x801f1018U;
  mission_bridge_profile.weapon_menu_input_ready = 0x801f101cU;
  mission_bridge_profile.first_person_aim_mode = 0x801f1020U;
  mission_bridge_profile.scope_camera_controller_pointer = 0x801f1024U;
  mission_bridge_profile.scope_zoom_offset = 0x20U;
  mission_bridge_profile.text_slot_table = 0x801f1500U;
  mission_bridge_profile.text_object_pool = 0x801f1600U;
  mission_bridge_profile.message_glyph_pool = 0x801f1b00U;
  mission_bridge_profile.timer_glyph_pool = 0x801f3000U;
  mission_bridge_profile.status_backdrop_tag = 0x801f3200U;
  mission_bridge_profile.status_backdrop_color_code = 0x801f320cU;
  mission_bridge_profile.status_backdrop_vertices = 0x801f3210U;
  mission_bridge_profile.mission_timer_remaining = 0x801f3240U;
  mission_bridge_profile.mission_timer_handle = 0x801f3244U;
  constexpr std::uint32_t mission_progress = 0x801f1100U;
  constexpr std::uint32_t objective_text_table = 0x801f1200U;
  constexpr std::uint32_t parameter_text_table = 0x801f1240U;
  constexpr std::array objective_text_addresses{
      0x801f1300U,
      0x801f1340U,
      0x801f1380U,
      0x801f13c0U,
  };
  constexpr std::array parameter_text_addresses{
      0x801f1400U,
      0x801f1440U,
  };
  constexpr std::array<std::string_view, objective_text_addresses.size()>
      objective_texts{
          "Eliminate target",
          "Protect agents",
          "Disable security",
          "Tag bomb",
      };
  constexpr std::array<std::string_view, parameter_text_addresses.size()>
      parameter_texts{
          "Do not eliminate agents",
          "Avoid damaging bombs",
      };
  constexpr std::uint32_t status_text_node = 0x801f3300U;
  const auto status_text_object = mission_bridge_profile.text_object_pool +
                                  mission_bridge_profile.text_object_stride;
  const auto timer_text_object = mission_bridge_profile.text_object_pool +
                                 mission_bridge_profile.text_object_stride * 2U;
  constexpr std::uint32_t pickup_text_address = 0x80010b00U;
  const auto write_guest_string = [&vm](std::uint32_t address,
                                        std::string_view text) noexcept {
    for (std::size_t index = 0U; index < text.size(); ++index) {
      if (!vm.runtime().write8(address + static_cast<std::uint32_t>(index),
                               static_cast<std::uint8_t>(text[index]))) {
        return false;
      }
    }
    return vm.runtime().write8(
        address + static_cast<std::uint32_t>(text.size()), 0U);
  };
  for (std::size_t index = 0U; index < objective_texts.size(); ++index) {
    require(vm.runtime().write32(objective_text_table +
                                     static_cast<std::uint32_t>(index * 4U),
                                 objective_text_addresses[index]) &&
                write_guest_string(objective_text_addresses[index],
                                   objective_texts[index]),
            "Could not seed retail objective text table");
  }
  for (std::size_t index = 0U; index < parameter_texts.size(); ++index) {
    require(vm.runtime().write32(parameter_text_table +
                                     static_cast<std::uint32_t>(index * 4U),
                                 parameter_text_addresses[index]) &&
                write_guest_string(parameter_text_addresses[index],
                                   parameter_texts[index]),
            "Could not seed retail parameter text table");
  }
  vm.bindSyphonFilterUsaV11GameplayTextHooks(text_hook_profile);
  vm.clearUiMessages();
  require(write_guest_text(pickup_text_address, "9mm taken") &&
              vm.invoke(status_text_hook, std::array{pickup_text_address, 41U})
                  .completed(),
          "Could not observe the retail pickup-notification source");
  const auto pickup_source_snapshot = vm.captureSnapshot();
  require(pickup_source_snapshot.ui_messages.size() == 1U &&
              pickup_source_snapshot.ui_messages[0].channel ==
                  sf::game::LegacyUiMessageChannel::status &&
              pickup_source_snapshot.ui_messages[0].text == "9mm taken",
          "Retail pickup-notification source was not retained");
  require(
      vm.runtime().write32(mission_bridge_profile.mission_progress_pointer,
                           mission_progress) &&
          vm.runtime().write32(mission_progress, 4U) &&
          vm.runtime().write32(mission_progress + 4U, objective_text_table) &&
          vm.runtime().write32(mission_progress + 8U, 2U) &&
          vm.runtime().write32(mission_progress + 0x0cU,
                               parameter_text_table) &&
          vm.runtime().write32(mission_progress + 0x10U, 0x05U) &&
          vm.runtime().write32(mission_progress + 0x14U, 0x08U) &&
          vm.runtime().write32(mission_progress + 0x18U, 0x0fU) &&
          vm.runtime().write32(mission_progress + 0x1cU, 0x03U) &&
          vm.runtime().write32(mission_progress + 0x20U, 0x01U) &&
          vm.runtime().write32(mission_progress + 0x24U, 0x02U) &&
          vm.runtime().write8(mission_bridge_profile.mission_terminal_latch,
                              1U) &&
          vm.runtime().write8(mission_bridge_profile.mission_success_latch,
                              0U) &&
          vm.runtime().write8(mission_bridge_profile.mission_transition_latch,
                              0U) &&
          vm.runtime().write8(mission_bridge_profile.mission_failure_flag,
                              1U) &&
          vm.runtime().write8(mission_bridge_profile.mission_completed_flag,
                              0U) &&
          vm.runtime().write32(mission_bridge_profile.weapon_menu_state, 0U) &&
          vm.runtime().write8(mission_bridge_profile.weapon_menu_dirty, 1U) &&
          vm.runtime().write32(
              mission_bridge_profile.weapon_menu_controller_ready, 1U) &&
          vm.runtime().write32(mission_bridge_profile.weapon_menu_input_ready,
                               1U) &&
          vm.runtime().write32(mission_bridge_profile.first_person_aim_mode,
                               0U) &&
          vm.runtime().write32(
              mission_bridge_profile.scope_camera_controller_pointer,
              0x801f3300U) &&
          vm.runtime().write32(
              0x801f3300U + mission_bridge_profile.scope_zoom_offset, 320U) &&
          // Completed retail status state: two differently colored reveal
          // packets plus the glyph-tight POLY_F4 built by FUN_800865ec.
          vm.runtime().write32(
              mission_bridge_profile.text_slot_table +
                  6U * mission_bridge_profile.text_slot_stride + 0x10U,
              status_text_node) &&
          vm.runtime().write32(status_text_node, status_text_object) &&
          vm.runtime().write32(status_text_node + 8U, 0U) &&
          vm.runtime().write32(status_text_object,
                               mission_bridge_profile.message_glyph_pool) &&
          vm.runtime().write16(status_text_object + 0x0cU, 2U) &&
          // Additive checksum of the source string "9mm taken".
          vm.runtime().write8(status_text_object + 0x15U, 70U) &&
          vm.runtime().write32(status_text_object + 0x18U, 0U) &&
          vm.runtime().write16(mission_bridge_profile.message_glyph_pool + 4U,
                               static_cast<std::uint16_t>(-17)) &&
          vm.runtime().write16(mission_bridge_profile.message_glyph_pool + 6U,
                               91U) &&
          vm.runtime().write16(mission_bridge_profile.message_glyph_pool + 8U,
                               7U) &&
          vm.runtime().write16(
              mission_bridge_profile.message_glyph_pool + 0x0aU, 8U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool + 0x0eU,
                              33U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool + 0x0fU,
                              44U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool + 0x14U,
                              255U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool + 0x15U,
                              255U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool + 0x16U,
                              255U) &&
          vm.runtime().write16(mission_bridge_profile.message_glyph_pool +
                                   mission_bridge_profile.glyph_stride + 4U,
                               static_cast<std::uint16_t>(-9)) &&
          vm.runtime().write16(mission_bridge_profile.message_glyph_pool +
                                   mission_bridge_profile.glyph_stride + 6U,
                               91U) &&
          vm.runtime().write16(mission_bridge_profile.message_glyph_pool +
                                   mission_bridge_profile.glyph_stride + 8U,
                               6U) &&
          vm.runtime().write16(mission_bridge_profile.message_glyph_pool +
                                   mission_bridge_profile.glyph_stride + 0x0aU,
                               8U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool +
                                  mission_bridge_profile.glyph_stride + 0x0eU,
                              40U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool +
                                  mission_bridge_profile.glyph_stride + 0x0fU,
                              44U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool +
                                  mission_bridge_profile.glyph_stride + 0x14U,
                              128U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool +
                                  mission_bridge_profile.glyph_stride + 0x15U,
                              128U) &&
          vm.runtime().write8(mission_bridge_profile.message_glyph_pool +
                                  mission_bridge_profile.glyph_stride + 0x16U,
                              128U) &&
          vm.runtime().write32(mission_bridge_profile.status_backdrop_tag,
                               0x05000000U) &&
          vm.runtime().write32(
              mission_bridge_profile.status_backdrop_color_code, 0x2a503028U) &&
          vm.runtime().write32(mission_bridge_profile.status_backdrop_vertices,
                               0x0059ffecU) &&
          vm.runtime().write32(mission_bridge_profile.status_backdrop_vertices +
                                   4U,
                               0x0059fffeU) &&
          vm.runtime().write32(mission_bridge_profile.status_backdrop_vertices +
                                   8U,
                               0x0063ffecU) &&
          vm.runtime().write32(mission_bridge_profile.status_backdrop_vertices +
                                   12U,
                               0x0063fffeU) &&
          // PARK timer: generation-tagged handle and exact eight-glyph block.
          vm.runtime().write32(timer_text_object,
                               mission_bridge_profile.timer_glyph_pool) &&
          vm.runtime().write16(timer_text_object + 0x0cU, 8U) &&
          vm.runtime().write8(timer_text_object + 0x15U, 3U) &&
          vm.runtime().write16(mission_bridge_profile.mission_timer_handle,
                               0x0302U) &&
          vm.runtime().write32(mission_bridge_profile.mission_timer_remaining,
                               23999U),
      "Could not seed retail mission bridge state");
  for (std::uint32_t index = 0U;
       index < mission_bridge_profile.timer_glyph_capacity; ++index) {
    const auto glyph = mission_bridge_profile.timer_glyph_pool +
                       index * mission_bridge_profile.glyph_stride;
    require(vm.runtime().write16(
                glyph + 4U, static_cast<std::uint16_t>(-176 + index * 6)) &&
                vm.runtime().write16(glyph + 6U, 50U) &&
                vm.runtime().write16(glyph + 8U, 5U) &&
                vm.runtime().write16(glyph + 0x0aU, 8U) &&
                vm.runtime().write8(glyph + 0x0eU,
                                    static_cast<std::uint8_t>(64U + index)) &&
                vm.runtime().write8(glyph + 0x0fU, 12U) &&
                vm.runtime().write8(glyph + 0x14U, 128U) &&
                vm.runtime().write8(glyph + 0x15U, 128U) &&
                vm.runtime().write8(glyph + 0x16U, 128U),
            "Could not seed exact retail timer glyph packet");
  }
  const auto failed_before_transition =
      vm.readMissionBridgeState(mission_bridge_profile);
  require(failed_before_transition &&
              failed_before_transition->messages.size() == 1U &&
              failed_before_transition->messages[0].text == "9mm taken",
          "Retail pickup-notification text was not associated with its glyphs");
  vm.clearUiMessages();
  const auto persistent_pickup =
      vm.readMissionBridgeState(mission_bridge_profile);
  require(
      persistent_pickup && persistent_pickup->messages.size() == 1U &&
          persistent_pickup->messages[0].text == "9mm taken",
      "Retail pickup-notification source did not survive its creation frame");
  constexpr std::uint32_t colliding_status_text_address = 0x80010b00U;
  require(write_guest_text(colliding_status_text_address, "New prompt :") &&
              vm.invoke(status_text_hook,
                        std::array{colliding_status_text_address, 41U})
                  .completed(),
          "Could not submit the colliding retail status source");
  const auto collision_replacement =
      vm.readMissionBridgeState(mission_bridge_profile);
  require(collision_replacement &&
              collision_replacement->messages.size() == 1U &&
              collision_replacement->messages[0].text == "New prompt :",
          "A stale equal-checksum source replaced the current status text");
  vm.clearUiMessages();
  require(write_guest_text(colliding_status_text_address, "9mm taken") &&
              vm.invoke(status_text_hook,
                        std::array{colliding_status_text_address, 41U})
                  .completed(),
          "Could not resubmit the pickup source after the collision probe");
  const auto restored_pickup_source =
      vm.readMissionBridgeState(mission_bridge_profile);
  require(restored_pickup_source &&
              restored_pickup_source->messages.size() == 1U &&
              restored_pickup_source->messages[0].text == "9mm taken",
          "Could not restore the pickup source after the collision probe");
  vm.clearUiMessages();
  require(
      failed_before_transition && failed_before_transition->failure &&
          failed_before_transition->terminal &&
          !failed_before_transition->success &&
          !failed_before_transition->failure_transition &&
          failed_before_transition->objective_count == 4U &&
          failed_before_transition->parameter_count == 2U &&
          failed_before_transition->objective_texts ==
              std::vector<std::string>{"Eliminate target", "Protect agents",
                                       "Disable security", "Tag bomb"} &&
          failed_before_transition->parameter_texts ==
              std::vector<std::string>{"Do not eliminate agents",
                                       "Avoid damaging bombs"} &&
          failed_before_transition->completed_objectives == 0x05U &&
          failed_before_transition->failed_objectives == 0x08U &&
          failed_before_transition->failed_parameters == 0x01U &&
          failed_before_transition->weapon_menu_state == 0 &&
          failed_before_transition->weapon_menu_dirty &&
          failed_before_transition->weapon_menu_controller_ready &&
          failed_before_transition->weapon_menu_input_ready &&
          failed_before_transition->messages.size() == 1U &&
          failed_before_transition->messages[0].channel ==
              sf::game::LegacyUiMessageChannel::status &&
          failed_before_transition->messages[0].text == "9mm taken" &&
          failed_before_transition->messages[0].glyphs.size() == 2U &&
          failed_before_transition->messages[0].glyphs[0].x == -17 &&
          failed_before_transition->messages[0].glyphs[0].color ==
              sf::game::LegacyRgbBridgeState{255U, 255U, 255U} &&
          failed_before_transition->messages[0].glyphs[1].x == -9 &&
          failed_before_transition->messages[0].glyphs[1].color ==
              sf::game::LegacyRgbBridgeState{128U, 128U, 128U} &&
          failed_before_transition->messages[0].backdrop &&
          failed_before_transition->messages[0].backdrop->color ==
              sf::game::LegacyRgbBridgeState{40U, 48U, 80U} &&
          failed_before_transition->messages[0].backdrop->corners[3] ==
              sf::game::LegacyProjectedPointBridgeState{-2, 99} &&
          failed_before_transition->messages[0].backdrop->semi_transparent &&
          failed_before_transition->timer &&
          failed_before_transition->timer->handle == 0x0302U &&
          failed_before_transition->timer->remaining_ticks == 23999 &&
          failed_before_transition->timer->glyphs.size() == 8U &&
          failed_before_transition->timer->glyphs.front().x == -176 &&
          failed_before_transition->timer->glyphs.back().x == -134,
      "Retail failure latch must not bypass the delayed transition");
  require(
      vm.runtime().write8(mission_bridge_profile.mission_transition_latch, 1U),
      "Could not seed retail mission transition latch");
  const auto failed_after_transition =
      vm.readMissionBridgeState(mission_bridge_profile);
  require(failed_after_transition &&
              failed_after_transition->failure_transition,
          "Retail failure transition latch mismatch");
  require(
      vm.runtime().write8(mission_bridge_profile.mission_success_latch, 1U) &&
          vm.runtime().write8(mission_bridge_profile.mission_failure_flag,
                              0U) &&
          vm.runtime().write8(mission_bridge_profile.mission_completed_flag,
                              1U),
      "Could not seed retail mission completion flags");
  const auto succeeded = vm.readMissionBridgeState(mission_bridge_profile);
  require(succeeded && succeeded->success && succeeded->terminal &&
              !succeeded->failure && !succeeded->failure_transition,
          "Retail mission success/failure latches are reversed");
  require(
      vm.runtime().write32(mission_progress, 33U) &&
          !vm.readMissionBridgeState(mission_bridge_profile),
      "Mission bridge accepted more entries than its 32-bit masks can hold");
  require(vm.runtime().write32(mission_progress, 4U) &&
              vm.runtime().write32(objective_text_table, 0x801fffffU) &&
              !vm.readMissionBridgeState(mission_bridge_profile),
          "Mission bridge accepted an empty/out-of-range guest C-string");

  constexpr std::array bzero_seed{
      std::byte{0x12},
      std::byte{0x34},
      std::byte{0x56},
      std::byte{0x78},
  };
  constexpr std::array<std::byte, bzero_seed.size()> expected_zeroes{};
  std::array<std::byte, bzero_seed.size()> zeroed_bytes{};
  require(vm.runtime().loadBytes(0x80010700U, bzero_seed),
          "Could not seed BIOS bzero HLE");
  vm.runtime().setRegister(9U, 0x28U);
  constexpr std::array bzero_arguments{0x80010700U, 4U};
  const auto bzero_result = vm.invoke(0x000000a0U, bzero_arguments, 1U);
  require(bzero_result.completed() &&
              bzero_result.return_value == 0x80010700U &&
              vm.runtime().copyBytes(0x80010700U, zeroed_bytes) &&
              zeroed_bytes == expected_zeroes,
          "BIOS A0:28 bzero HLE mismatch");

  constexpr std::array bios_string{
      std::byte{'S'}, std::byte{'U'}, std::byte{'B'}, std::byte{'W'},
      std::byte{'A'}, std::byte{'Y'}, std::byte{'.'}, std::byte{'H'},
      std::byte{'O'}, std::byte{'G'}, std::byte{},
  };
  require(vm.runtime().loadBytes(0x80010500U, bios_string),
          "Could not seed BIOS strchr HLE");
  vm.runtime().setRegister(9U, 0x1eU);
  constexpr std::array strchr_arguments{0x80010500U,
                                        static_cast<std::uint32_t>('.')};
  const auto strchr_result = vm.invoke(0x000000a0U, strchr_arguments, 1U);
  require(strchr_result.completed() &&
              strchr_result.return_value == 0x80010506U,
          "BIOS A0:1e strchr HLE mismatch");
  constexpr std::array bios_suffix{
      std::byte{'/'}, std::byte{'V'}, std::byte{'R'}, std::byte{'A'},
      std::byte{'M'}, std::byte{'0'}, std::byte{'.'}, std::byte{'H'},
      std::byte{'O'}, std::byte{'G'}, std::byte{},
  };
  require(vm.runtime().loadBytes(0x80010600U, bios_suffix),
          "Could not seed BIOS strcat source");
  vm.runtime().setRegister(9U, 0x15U);
  constexpr std::array strcat_arguments{0x80010500U, 0x80010600U};
  const auto strcat_result = vm.invoke(0x000000a0U, strcat_arguments, 1U);
  constexpr std::array expected_concatenated{
      std::byte{'S'}, std::byte{'U'}, std::byte{'B'}, std::byte{'W'},
      std::byte{'A'}, std::byte{'Y'}, std::byte{'.'}, std::byte{'H'},
      std::byte{'O'}, std::byte{'G'}, std::byte{'/'}, std::byte{'V'},
      std::byte{'R'}, std::byte{'A'}, std::byte{'M'}, std::byte{'0'},
      std::byte{'.'}, std::byte{'H'}, std::byte{'O'}, std::byte{'G'},
      std::byte{},
  };
  std::array<std::byte, expected_concatenated.size()> concatenated_bytes{};
  require(strcat_result.completed() &&
              strcat_result.return_value == 0x80010500U &&
              vm.runtime().copyBytes(0x80010500U, concatenated_bytes) &&
              concatenated_bytes == expected_concatenated,
          "BIOS A0:15 strcat HLE mismatch");
  require(vm.runtime().write32(0xa0009010U, 1U),
          "Could not seed BIOS A0 rand state");
  vm.runtime().setRegister(9U, 0x2fU);
  const auto bios_rand_result = vm.invoke(0x000000a0U, {}, 1U);
  constexpr auto expected_bios_seed = 1U * 0x41c64e6dU + 0x3039U;
  std::uint32_t bios_seed{};
  require(bios_rand_result.completed() &&
              bios_rand_result.return_value ==
                  ((expected_bios_seed >> 16U) & 0x7fffU) &&
              vm.runtime().read32(0xa0009010U, bios_seed) &&
              bios_seed == expected_bios_seed,
          "BIOS A0:2f rand HLE mismatch");
  vm.clearHostCalls();

  auto mission_profile = sf::game::syphonFilterUsaV11MissionProfile();
  mission_profile.frame_event_entry = 0x80030010U;
  mission_profile.delayed_callbacks_entry = 0x80030020U;
  mission_profile.queue_drain_entry = 0x80030030U;
  mission_profile.pending_queue_count = 0x80031000U;
  mission_profile.ready_queue_count = 0x80031004U;
  mission_profile.ready_queue_entries = 0x80031100U;
  mission_profile.dynamic_event_table_pointer = 0x80031200U;
  mission_profile.static_event_table = 0x80031300U;
  mission_profile.special_object_handler_pointer = 0x80031204U;
  mission_profile.object_records_pointer = 0x80031208U;
  mission_profile.object_count = 0x80031210U;
  mission_profile.object_definitions_pointer = 0x8003120cU;
  mission_profile.object_definition_count = 0x80031214U;
  mission_profile.object_handler_table = 0x80031400U;
  constexpr std::uint32_t dynamic_event_table = 0x80031500U;
  constexpr std::uint32_t object_records = 0x80031600U;
  constexpr std::uint32_t object_definitions = 0x80031800U;
  constexpr std::uint32_t mission_handler = 0x80030040U;
  constexpr std::uint32_t event_stride = 0x1cU;
  constexpr std::array destinations{0xffffU, 0xfffeU, 0x29aU, 2U, 3U};
  for (std::uint32_t index = 0U; index < destinations.size(); ++index) {
    const auto event =
        mission_profile.ready_queue_entries + index * event_stride;
    require(
        vm.runtime().write16(event, static_cast<std::uint16_t>(index + 1U)) &&
            vm.runtime().write32(event + 8U, destinations[index]),
        "Could not seed a legacy mission event");
  }
  require(
      vm.runtime().write32(mission_profile.ready_queue_count,
                           static_cast<std::uint32_t>(destinations.size())) &&
          vm.runtime().write32(mission_profile.static_event_table + 12U,
                               mission_handler) &&
          vm.runtime().write32(mission_profile.dynamic_event_table_pointer,
                               dynamic_event_table) &&
          vm.runtime().write32(dynamic_event_table + 2U * 12U + 4U,
                               mission_handler) &&
          vm.runtime().write32(mission_profile.special_object_handler_pointer,
                               mission_handler) &&
          vm.runtime().write32(mission_profile.object_records_pointer,
                               object_records) &&
          vm.runtime().write32(mission_profile.object_count, 4U) &&
          vm.runtime().write32(mission_profile.object_definitions_pointer,
                               object_definitions) &&
          vm.runtime().write32(mission_profile.object_definition_count, 5U) &&
          vm.runtime().write32(object_records + 2U * 0x4cU, 3U) &&
          vm.runtime().write16(object_definitions + 3U * 0x14U, 1U) &&
          vm.runtime().write32(mission_profile.object_handler_table + 1U * 4U,
                               mission_handler) &&
          vm.runtime().write32(object_records + 3U * 0x4cU, 4U) &&
          vm.runtime().write16(object_definitions + 4U * 0x14U, 8U) &&
          vm.runtime().write32(mission_profile.object_handler_table + 8U * 4U,
                               0U),
      "Could not seed legacy mission dispatch tables");

  auto frame_called = false;
  auto callbacks_called = false;
  auto queue_called = false;
  std::vector<std::uint32_t> dispatched_addresses;
  vm.bindHostCall(
      mission_profile.frame_event_entry,
      [&frame_called](sf::game::LegacyHostCallContext &context) {
        constexpr std::array expected{4U, 5U, 0xfffeU, 0xfffeU, 0U, 0U, 0U, 0U};
        for (std::size_t index = 0U; index < expected.size(); ++index) {
          require(context.argument(index) == expected[index],
                  "Legacy mission frame event argument mismatch");
        }
        frame_called = true;
      });
  vm.bindHostCall(mission_profile.delayed_callbacks_entry,
                  [&callbacks_called](sf::game::LegacyHostCallContext &) {
                    callbacks_called = true;
                  });
  vm.bindHostCall(
      mission_profile.queue_drain_entry,
      [&queue_called,
       &mission_profile](sf::game::LegacyHostCallContext &context) {
        require(context.argument(0) == mission_profile.pending_queue_count &&
                    context.argument(1) == mission_profile.ready_queue_count,
                "Legacy mission queue arguments mismatch");
        queue_called = true;
      });
  vm.bindHostCall(
      mission_handler,
      [&dispatched_addresses](sf::game::LegacyHostCallContext &context) {
        dispatched_addresses.push_back(context.argument(0));
      });
  const auto mission_result = vm.tickMission(mission_profile);
  require(mission_result.completed() && frame_called && callbacks_called &&
              queue_called,
          "Legacy mission tick did not complete");
  require(mission_result.ready_events == destinations.size() &&
              mission_result.dispatched_events.size() == 4U &&
              mission_result.hostCalls() == 7U,
          "Legacy mission dispatch count mismatch");
  require(dispatched_addresses ==
              std::vector<std::uint32_t>{
                  mission_profile.ready_queue_entries,
                  mission_profile.ready_queue_entries + event_stride,
                  mission_profile.ready_queue_entries + 2U * event_stride,
                  mission_profile.ready_queue_entries + 3U * event_stride,
              },
          "Legacy mission dispatched the wrong event records");
  vm.clearHostCalls();
  require(sf::game::LegacyGameplayVm::updates_per_second == 20U,
          "Legacy VM cadence boundary mismatch");

  sf::game::LegacyMissionBridgeState failed_mission;
  failed_mission.terminal = true;
  failed_mission.failure = true;
  require(!sf::game::legacyMissionFailureRestartReady(0U, failed_mission),
          "Retail failure must retain the 0xc8-unit delay before fade");
  failed_mission.failure_transition = true;
  require(!sf::game::legacyMissionFailureRestartReady(0U, failed_mission) &&
              sf::game::legacyMissionFailureRestartReady(2U, failed_mission),
          "Retail failure fade must finish before requesting restart");
  failed_mission.failure = false;
  failed_mission.success = true;
  require(!sf::game::legacyMissionFailureRestartReady(2U, failed_mission),
          "Retail success must not request a checkpoint restart");
  require(sf::game::legacyRetailTerminalMovieBoundary(3U, failed_mission) &&
              sf::game::legacyRetailTerminalMovieBoundary(4U, failed_mission) &&
              !sf::game::legacyRetailTerminalMovieBoundary(5U, failed_mission),
          "Retail terminal movie-loader boundary mismatch");

  const auto success_transition = sf::game::classifyLegacyMissionTransition(
      19U, 0U, 3U, failed_mission, false);
  require(success_transition.request_ending_movie &&
              success_transition.finished &&
              !success_transition.request_failure_restart,
          "Retail success transition did not request native EOL playback");
  const auto movie_loader_success = sf::game::classifyLegacyMissionTransition(
      19U, 0U, 4U, failed_mission, false);
  require(movie_loader_success.request_ending_movie &&
              movie_loader_success.finished &&
              !movie_loader_success.request_failure_restart,
          "Retail state-4 success loader did not request native EOL playback");
  failed_mission.success = false;
  failed_mission.failure = true;
  const auto failure_transition = sf::game::classifyLegacyMissionTransition(
      4U, 0U, 2U, failed_mission, false);
  require(failure_transition.request_failure_restart &&
              failure_transition.finished &&
              !failure_transition.request_ending_movie,
          "Retail failure transition did not request checkpoint restart");
  sf::game::LegacyMissionBridgeState active_mission;
  const auto intro_loading = sf::game::classifyLegacyMissionTransition(
      0U, 0U, 9U, active_mission, false, 1U);
  const auto intro_ready = sf::game::classifyLegacyMissionTransition(
      0U, 9U, 0U, active_mission, intro_loading.movie_loader_pending, 1U);
  const auto non_georgia_state9 = sf::game::classifyLegacyMissionTransition(
      1U, 0U, 9U, active_mission, false);
  const auto museum2_loading = sf::game::classifyLegacyMissionTransition(
      6U, 0U, 9U, active_mission, false, 1U);
  const auto museum2_ready = sf::game::classifyLegacyMissionTransition(
      6U, 9U, 0U, active_mission, museum2_loading.movie_loader_pending, 1U);
  require(intro_loading.movie_loader_pending &&
              intro_ready.request_intro_movie &&
              !intro_ready.movie_loader_pending &&
              !non_georgia_state9.request_intro_movie &&
              !non_georgia_state9.movie_loader_pending &&
              museum2_loading.movie_loader_pending &&
              museum2_ready.request_intro_movie &&
              !museum2_ready.movie_loader_pending,
          "Retail state-9 native INTRO handoff mismatch");

  active_mission.completed_objectives = 0x08U;
  const auto ending_loading = sf::game::classifyLegacyMissionTransition(
      0U, 0U, 9U, active_mission, false, 1U);
  const auto ending_ready = sf::game::classifyLegacyMissionTransition(
      0U, 9U, 0U, active_mission, ending_loading.movie_loader_pending, 1U);
  require(ending_loading.movie_loader_pending &&
              ending_ready.request_ending_movie && ending_ready.finished &&
              !ending_ready.request_intro_movie &&
              !ending_ready.movie_loader_pending,
          "Retail SUBWAY source-30 MOVIE handoff was mistaken for INTRO");
}

void testLegacyGameplayVmContinuousPump() {
  constexpr std::array loop_words{
      encodeI(0x09U, 8U, 8U, 1U),
      encodeJ(0x02U, code_address),
      0U,
  };
  const auto initial_code = instructionBytes(loop_words);
  std::vector<std::byte> executable_bytes(2048U + initial_code.size());
  constexpr std::string_view signature{"PS-X EXE"};
  std::ranges::transform(signature, executable_bytes.begin(), [](char value) {
    return static_cast<std::byte>(value);
  });
  writeLe32(executable_bytes, 0x10U, code_address);
  writeLe32(executable_bytes, 0x18U, code_address);
  writeLe32(executable_bytes, 0x1cU,
            static_cast<std::uint32_t>(initial_code.size()));
  std::ranges::copy(initial_code, executable_bytes.begin() + 2048);

  const auto executable = sf::psx::Executable::parse(executable_bytes);
  sf::game::LegacyGameplayVm vm{executable};
  const auto first_slice = vm.resumeCurrentPc(2U);
  const auto first_slice_pc = vm.runtime().state().pc;
  const auto second_slice = vm.resumeCurrentPc(2U);
  require(first_slice.execution.reason ==
                  sf::psx::R3000StopReason::instruction_budget &&
              first_slice.execution.instructions == 2U &&
              first_slice_pc == code_address + 8U &&
              second_slice.execution.reason ==
                  sf::psx::R3000StopReason::instruction_budget &&
              second_slice.execution.instructions == 2U &&
              vm.runtime().state().gpr[8] == 2U &&
              vm.runtime().state().pc == code_address + 4U,
          "Legacy VM resume reset or diverged from the active guest loop");

  constexpr std::uint32_t caller_address = 0x80022000U;
  constexpr std::uint32_t boundary_address = 0x80023000U;
  constexpr std::array caller_words{
      encodeJ(0x03U, boundary_address),    0U, encodeI(0x09U, 0U, 16U, 1U),
      encodeJ(0x02U, caller_address + 8U), 0U,
  };
  const auto caller = instructionBytes(caller_words);
  require(vm.loadOverlay(caller_address, caller),
          "Could not load the continuous guest boundary fixture");
  vm.runtime().reset(caller_address);
  std::uint32_t boundary_calls{};
  vm.bindHostCall(boundary_address,
                  [&boundary_calls](sf::game::LegacyHostCallContext &context) {
                    ++boundary_calls;
                    context.setReturnValue(0x12345678U);
                  });

  const auto boundary = vm.runCurrentPcUntilHostBoundary(boundary_address, 8U);
  require(boundary.stoppedAtHostBoundary() &&
              boundary.host_boundary == boundary_address &&
              boundary.execution.reason == sf::psx::R3000StopReason::running &&
              boundary.execution.instructions == 2U &&
              boundary.host_calls == 0U && boundary_calls == 0U &&
              vm.runtime().state().pc == boundary_address,
          "Legacy VM continuous pump crossed its host boundary");

  const auto dispatched = vm.resumeCurrentPc(1U);
  require(dispatched.execution.reason ==
                  sf::psx::R3000StopReason::instruction_budget &&
              dispatched.execution.instructions == 0U &&
              dispatched.host_calls == 1U && boundary_calls == 1U &&
              dispatched.return_value == 0x12345678U &&
              vm.runtime().state().pc == caller_address + 8U,
          "Legacy VM resume did not dispatch the preserved boundary call");
}

void testGuestPadBridge() {
  sf::game::PlayerInput input;
  input.move = 1.0;
  input.turn = -0.5;
  input.aim = true;
  input.next_weapon = true;
  input.strafe = -1.0;
  input.aim_peek = -1.0;
  input.look_yaw = 96.0;
  input.look_pitch = -96.0;
  input.fire_held = true;
  input.roll = true;
  input.reload = true;
  input.kneel = true;
  input.target_lock_held = true;

  const auto pad = sf::game::legacyPadStateFromPlayerInput(input);
  constexpr std::uint16_t expected_buttons =
      0x0100U | 0x0400U | 0x1000U | 0x2000U | 0x4000U | 0x8000U;
  require(pad.buttons == expected_buttons,
          "Manual aim lost retail strafe/crouch or leaked a forbidden PAD "
          "action");
  require(pad.left_x == 128U && pad.left_y == 128U && pad.right_x == 128U &&
              pad.right_y == 128U,
          "Host first-person locomotion leaked into the retail sight axes");

  input = {};
  input.aim = true;
  input.look_pitch = -96.0;
  const auto native_aim_up = sf::game::legacyPadStateFromPlayerInput(input);
  input.look_pitch = 96.0;
  const auto native_aim_down = sf::game::legacyPadStateFromPlayerInput(input);
  input.look_pitch = 0.0;
  input.move = 1.0;
  const auto keyboard_aim_up = sf::game::legacyPadStateFromPlayerInput(input);
  input.move = -1.0;
  const auto keyboard_aim_down = sf::game::legacyPadStateFromPlayerInput(input);
  require(native_aim_up.left_y == 0x80U && native_aim_down.left_y == 0x80U &&
              keyboard_aim_up.left_y == 0x80U &&
              keyboard_aim_down.left_y == 0x80U,
          "Host mouse or W/S locomotion leaked into the guest PAD");

  input = {};
  input.aim = true;
  input.roll = true;
  const auto scope_zoom_out = sf::game::legacyPadStateFromPlayerInput(input);
  input.roll = false;
  input.quick_turn = true;
  const auto scoped_quick_turn = sf::game::legacyPadStateFromPlayerInput(input);
  require((scope_zoom_out.buttons & 0x2400U) == 0x2400U &&
              (scoped_quick_turn.buttons & 0x2000U) == 0U,
          "Scoped Circle zoom-out or quick-turn isolation mismatch");

  input = {};
  input.aim = true;
  input.aim_peek = -1.0;
  const auto aim_strafe_left = sf::game::legacyPadStateFromPlayerInput(input);
  input.aim_peek = 1.0;
  const auto aim_strafe_right = sf::game::legacyPadStateFromPlayerInput(input);
  input.aim_peek = 0.0;
  input.strafe = 1.0;
  const auto aim_move_right = sf::game::legacyPadStateFromPlayerInput(input);
  input.strafe = 0.0;
  input.turn = -1.0;
  const auto aim_turn_left = sf::game::legacyPadStateFromPlayerInput(input);
  input.turn = 1.0;
  const auto aim_turn_right = sf::game::legacyPadStateFromPlayerInput(input);
  require((aim_strafe_left.buttons & 0x0300U) == 0x0100U &&
              (aim_strafe_right.buttons & 0x0300U) == 0x0200U &&
              (aim_strafe_left.buttons & 0x0400U) != 0U &&
              (aim_strafe_right.buttons & 0x0400U) != 0U &&
              (aim_turn_left.buttons & 0x0400U) != 0U &&
              (aim_turn_right.buttons & 0x0400U) != 0U &&
              (aim_move_right.buttons & 0x0300U) == 0U &&
              aim_turn_left.left_x == 0x80U && aim_turn_right.left_x == 0x80U,
          "Manual-aim peek or host locomotion PAD isolation mismatch");

  input = {};
  input.target_lock_held = true;
  const auto chase_target_lock = sf::game::legacyPadStateFromPlayerInput(input);
  input.aim = true;
  const auto manual_aim_target_lock =
      sf::game::legacyPadStateFromPlayerInput(input);
  require((chase_target_lock.buttons & 0x0800U) != 0U &&
              (manual_aim_target_lock.buttons & 0x0800U) == 0U,
          "Retail auto-lock was not isolated from direct manual aim");

  input = {};
  input.move = 1.0;
  input.strafe = 1.0;
  input.run = true;
  const auto diagonal_run = sf::game::legacyPadStateFromPlayerInput(input);
  require(diagonal_run.left_y == 0x01U &&
              (diagonal_run.buttons & 0x0300U) == 0x0200U,
          "Forward run and right strafe did not reach the retail PAD "
          "simultaneously");

  constexpr std::uint16_t latched_target_lock_and_fire = 0x0800U | 0x8000U;
  const auto manual_aim_transition = sf::game::legacyManualAimTransitionButtons(
      manual_aim_target_lock.buttons, latched_target_lock_and_fire, true);
  const auto chase_transition = sf::game::legacyManualAimTransitionButtons(
      chase_target_lock.buttons, latched_target_lock_and_fire, false);
  require((manual_aim_transition & 0x0400U) != 0U &&
              (manual_aim_transition & 0x0800U) == 0U &&
              (manual_aim_transition & 0x8000U) != 0U &&
              (chase_transition & 0x0800U) != 0U,
          "Latched auto-lock was not atomically retired by manual aim");

  input = {};
  input.run = true;
  input.move = -1.0;
  input.strafe = 1.0;
  input.quick_turn = true;
  const auto quick_turn = sf::game::legacyPadStateFromPlayerInput(input);
  require(quick_turn.left_y == 0xffU && (quick_turn.buttons & 0x0200U) != 0U &&
              (quick_turn.buttons & 0x2000U) != 0U,
          "Guest PAD bridge quick-turn mapping mismatch");

  input = {};
  input.direct_weapon = std::uint8_t{4U};
  const auto direct_weapon = sf::game::legacyPadStateFromPlayerInput(input);
  require((direct_weapon.buttons & 0x0001U) == 0U,
          "Direct weapon UI command leaked into an untracked PAD Select pulse");

  input = {};
  input.quick_weapon = true;
  const auto quick_weapon = sf::game::legacyPadStateFromPlayerInput(input);
  require(quick_weapon.buttons == 0U,
          "Native quick-weapon command leaked into the physical PAD bridge");
  input.strafe = -1.0;
  const auto quick_weapon_strafe_left =
      sf::game::legacyPadStateFromPlayerInput(input);
  input.strafe = 1.0;
  const auto quick_weapon_strafe_right =
      sf::game::legacyPadStateFromPlayerInput(input);
  require(quick_weapon_strafe_left.buttons == 0x0100U &&
              quick_weapon_strafe_right.buttons == 0x0200U,
          "Native quick-weapon command suppressed an independent strafe");
}

} // namespace

int main() {
  try {
    testBranchDelay();
    testLoadDelay();
    testMultiplyAndDivide();
    testCop0Status();
    testMachineInterrupts();
    testMachineTimer2();
    testMachineDmaCancellation();
    testMachineLinearDmaAndSnapshot();
    testMachineOtcDma();
    testMachineLinkedListDma();
    testMachineCdRomDmaAndSnapshot();
    testCop2LeadingCount();
    testGteGameplayMath();
    testGteRetailColorAndDepthCommands();
    testMemoryMap();
    testDeterministicStops();
    testLegacyVirtualCd();
    testGuestPadBridge();
    testLegacyGameplayVmBoundary();
    testLegacyGameplayVmContinuousPump();
    std::cout << "R3000 runtime tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "R3000 runtime test failure: " << error.what() << '\n';
    return 1;
  }
}
