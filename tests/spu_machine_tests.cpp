#include "sf/psx/machine.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

constexpr std::uint32_t i_stat = 0x1f801070U;
constexpr std::uint32_t spu_dma = 0x1f8010c0U;
constexpr std::uint32_t dpcr = 0x1f8010f0U;
constexpr std::uint32_t dicr = 0x1f8010f4U;
constexpr std::uint32_t spu = 0x1f801c00U;
constexpr std::uint32_t spu_irq_address = spu + 0x1a4U;
constexpr std::uint32_t spu_transfer_address = spu + 0x1a6U;
constexpr std::uint32_t spu_control = spu + 0x1aaU;
constexpr std::uint32_t spu_status = spu + 0x1aeU;

constexpr std::uint32_t dma4_dpcr_enable = 1U << 19U;
constexpr std::uint32_t dma4_dicr_enable = (1U << 23U) | (1U << 20U);
constexpr std::uint32_t dma4_dicr_flag = 1U << 28U;
constexpr std::uint32_t dicr_master_flag = 1U << 31U;
constexpr std::uint16_t dma_interrupt = 1U << 3U;
constexpr std::uint16_t spu_interrupt = 1U << 9U;

constexpr std::uint32_t dma_from_ram = 0x01000201U;
constexpr std::uint32_t dma_to_ram = 0x01000200U;
constexpr std::uint16_t spu_dma_write_mode = 2U << 4U;
constexpr std::uint16_t spu_dma_read_mode = 3U << 4U;
constexpr std::uint16_t spu_irq_enable = 1U << 6U;
constexpr std::uint16_t spu_status_dma_read_request = 1U << 8U;
constexpr std::uint16_t spu_status_dma_write_request = 1U << 9U;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
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
      (left.spu && *left.spu != *right.spu) || left.timers != right.timers) {
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

std::unique_ptr<sf::psx::PsxMachineState>
captureState(const sf::psx::PsxMachine &machine) {
  return std::make_unique<sf::psx::PsxMachineState>(machine.captureState());
}

void enableDma4(sf::psx::R3000Runtime &runtime,
                const sf::psx::PsxMachine &machine) {
  require(runtime.write32(dpcr, machine.dma().dpcr() | dma4_dpcr_enable) &&
              runtime.write32(dicr, dma4_dicr_enable),
          "Could not enable SPU DMA4 and its completion interrupt");
}

void startDma4(sf::psx::R3000Runtime &runtime, std::uint32_t address,
               std::uint16_t word_count, std::uint32_t control) {
  require(runtime.write32(spu_dma, address) &&
              runtime.write32(spu_dma + 4U, 0x00010000U | word_count) &&
              runtime.write32(spu_dma + 8U, control),
          "Could not program SPU DMA4");
}

void testSpuMmioAndBidirectionalDma() {
  auto runtime = std::make_unique<sf::psx::R3000Runtime>();
  auto machine = std::make_unique<sf::psx::PsxMachine>(*runtime);

  std::uint16_t halfword{};
  require(runtime->write16(spu + 0x04U, 0x2345U) &&
              runtime->read16(spu + 0x04U, halfword) && halfword == 0x2345U,
          "R3000 SH/LH did not reach the SPU register window");

  constexpr std::uint32_t source = 0x00002000U;
  constexpr std::uint32_t destination = 0x00003000U;
  constexpr std::uint16_t word_count = 3U;
  constexpr std::uint16_t transfer_address_units = 0x0020U;
  constexpr std::uint32_t transfer_address =
      static_cast<std::uint32_t>(transfer_address_units) * 8U;
  constexpr std::array<std::uint32_t, word_count> payload{
      0x11223344U,
      0x89abcdefU,
      0x76543210U,
  };
  constexpr std::array<std::byte, payload.size() * sizeof(std::uint32_t)>
      expected_bytes{
          std::byte{0x44U}, std::byte{0x33U}, std::byte{0x22U},
          std::byte{0x11U}, std::byte{0xefU}, std::byte{0xcdU},
          std::byte{0xabU}, std::byte{0x89U}, std::byte{0x10U},
          std::byte{0x32U}, std::byte{0x54U}, std::byte{0x76U},
      };

  for (std::size_t index = 0U; index < payload.size(); ++index) {
    require(runtime->write32(source + static_cast<std::uint32_t>(index * 4U),
                             payload[index]),
            "Could not seed the RAM-to-SPU DMA payload");
  }

  // IRQ address 0x21 points at byte 0x108, the first halfword of word three.
  require(
      runtime->write16(spu_transfer_address, transfer_address_units) &&
          runtime->write16(spu_irq_address, transfer_address_units + 1U) &&
          runtime->write16(spu_control, spu_dma_write_mode | spu_irq_enable),
      "Could not configure SPU transfer and IRQ registers");
  require(runtime->read16(spu_status, halfword) &&
              (halfword & 0x07a0U) == 0x02a0U &&
              (halfword & spu_status_dma_read_request) == 0U,
          "Idle SPUSTAT did not expose only the DMA-write request");

  enableDma4(*runtime, *machine);
  startDma4(*runtime, source, word_count, dma_from_ram);
  const auto write_completion =
      machine->dmaCompletionTick(sf::psx::DmaChannel::spu);
  require(machine->dma().scheduledToken(sf::psx::DmaChannel::spu) != 0U &&
              write_completion &&
              *write_completion == machine->currentTick() + word_count * 4U &&
              runtime->read16(spu_status, halfword) &&
              (halfword & 0x06a0U) == 0x06a0U,
          "RAM-to-SPU DMA4 did not publish its busy scheduler deadline");

  machine->advanceTicks(word_count * 4U - 1U);
  std::uint32_t register_value{};
  require(runtime->read32(spu_dma, register_value) &&
              register_value == source &&
              runtime->read32(spu_dma + 8U, register_value) &&
              (register_value & 0x01000000U) != 0U,
          "SPU DMA4 completed before its four-ticks-per-word deadline");
  for (std::size_t index = 0U; index < expected_bytes.size(); ++index) {
    require(machine->spu().state().ram[transfer_address + index] ==
                std::byte{0U},
            "RAM-to-SPU DMA changed SPU RAM before its deadline");
  }

  machine->advanceTicks(1U);
  for (std::size_t index = 0U; index < expected_bytes.size(); ++index) {
    require(machine->spu().state().ram[transfer_address + index] ==
                expected_bytes[index],
            "RAM-to-SPU DMA copied an incorrect little-endian byte");
  }

  std::uint16_t interrupt_status{};
  require(runtime->read32(spu_dma, register_value) &&
              register_value == source + payload.size() * 4U &&
              runtime->read32(spu_dma + 8U, register_value) &&
              register_value == 0x00000201U &&
              runtime->read32(dicr, register_value) &&
              (register_value &
               (dicr_master_flag | dma4_dicr_flag | dma4_dicr_enable)) ==
                  (dicr_master_flag | dma4_dicr_flag | dma4_dicr_enable) &&
              runtime->read16(i_stat, interrupt_status) &&
              (interrupt_status & (dma_interrupt | spu_interrupt)) ==
                  (dma_interrupt | spu_interrupt) &&
              (machine->interrupts().inputLines() & spu_interrupt) != 0U,
          "RAM-to-SPU DMA completion registers or IRQ routing mismatch");
  require(!machine->dmaCompletionTick(sf::psx::DmaChannel::spu) &&
              runtime->read16(spu_status, halfword) &&
              (halfword & (1U << 10U)) == 0U,
          "Completed RAM-to-SPU DMA remained busy");

  require(runtime->write16(spu_control, spu_dma_write_mode) &&
              runtime->read16(spu_status, halfword) &&
              (halfword & (1U << 6U)) == 0U &&
              (machine->interrupts().inputLines() & spu_interrupt) == 0U &&
              runtime->read16(i_stat, interrupt_status) &&
              (interrupt_status & spu_interrupt) != 0U,
          "Clearing SPUCNT.IRQ did not lower IRQ9 while preserving I_STAT");

  require(runtime->write32(dicr, dma4_dicr_enable | dma4_dicr_flag) &&
              runtime->read32(dicr, register_value) &&
              register_value == dma4_dicr_enable &&
              runtime->write16(i_stat, static_cast<std::uint16_t>(
                                           ~(dma_interrupt | spu_interrupt))) &&
              runtime->read16(i_stat, interrupt_status) &&
              (interrupt_status & (dma_interrupt | spu_interrupt)) == 0U,
          "DICR W1C or separate I_STAT acknowledgement behaved incorrectly");

  for (std::size_t index = 0U; index < payload.size(); ++index) {
    require(
        runtime->write32(destination + static_cast<std::uint32_t>(index * 4U),
                         0xdeadbeefU),
        "Could not seed the SPU-to-RAM DMA destination");
  }
  require(runtime->write16(spu_transfer_address, transfer_address_units) &&
              runtime->write16(spu_control, spu_dma_read_mode) &&
              runtime->read16(spu_status, halfword) &&
              (halfword & 0x07b0U) == 0x01b0U &&
              (halfword & spu_status_dma_write_request) == 0U,
          "Could not configure SPU-to-RAM transfer mode");
  startDma4(*runtime, destination, word_count, dma_to_ram);
  require(runtime->read16(spu_status, halfword) &&
              (halfword & 0x05b0U) == 0x05b0U,
          "Active SPU-to-RAM DMA did not set SPUSTAT busy");

  machine->advanceTicks(word_count * 4U - 1U);
  for (std::size_t index = 0U; index < payload.size(); ++index) {
    require(
        runtime->read32(destination + static_cast<std::uint32_t>(index * 4U),
                        register_value) &&
            register_value == 0xdeadbeefU,
        "SPU-to-RAM DMA changed RAM before its deadline");
  }

  machine->advanceTicks(1U);
  for (std::size_t index = 0U; index < payload.size(); ++index) {
    require(
        runtime->read32(destination + static_cast<std::uint32_t>(index * 4U),
                        register_value) &&
            register_value == payload[index],
        "SPU-to-RAM DMA produced an incorrect word");
  }
  require(runtime->read32(spu_dma, register_value) &&
              register_value == destination + payload.size() * 4U &&
              runtime->read32(spu_dma + 8U, register_value) &&
              register_value == 0x00000200U &&
              runtime->read32(dicr, register_value) &&
              (register_value &
               (dicr_master_flag | dma4_dicr_flag | dma4_dicr_enable)) ==
                  (dicr_master_flag | dma4_dicr_flag | dma4_dicr_enable),
          "SPU-to-RAM DMA MADR/CHCR/DICR completion state mismatch");
}

void testPendingSpuDmaSnapshot() {
  auto runtime = std::make_unique<sf::psx::R3000Runtime>();
  auto machine = std::make_unique<sf::psx::PsxMachine>(*runtime);

  constexpr std::uint32_t source = 0x00006000U;
  constexpr std::uint16_t word_count = 2U;
  constexpr std::array<std::uint32_t, word_count> payload{
      0x0badc0deU,
      0xfeedfaceU,
  };
  for (std::size_t index = 0U; index < payload.size(); ++index) {
    require(runtime->write32(source + static_cast<std::uint32_t>(index * 4U),
                             payload[index]),
            "Could not seed the snapshot DMA payload");
  }

  require(runtime->write16(spu_transfer_address, 0x0040U) &&
              runtime->write16(spu_control, spu_dma_write_mode),
          "Could not configure snapshot DMA transfer state");
  enableDma4(*runtime, *machine);
  startDma4(*runtime, source, word_count, dma_from_ram);
  machine->advanceTicks(5U);

  auto checkpoint = captureState(*machine);
  require(
      checkpoint->scheduler.event_count == 1U &&
          checkpoint->dma
                  .channels[static_cast<std::size_t>(sf::psx::DmaChannel::spu)]
                  .scheduled_token != 0U,
      "Pending SPU DMA snapshot lost its completion event/token");

  auto invalid = std::make_unique<sf::psx::PsxMachineState>(*checkpoint);
  invalid->interrupts.input_lines = static_cast<std::uint16_t>(
      invalid->interrupts.input_lines ^ spu_interrupt);
  require(!machine->validateState(*invalid) && !machine->restoreState(*invalid),
          "Machine accepted a snapshot with a mismatched SPU IRQ9 input");
  auto after_rejection = captureState(*machine);
  require(sameMachineState(*checkpoint, *after_rejection),
          "Rejected SPU snapshot partially mutated the machine");

  machine->advanceTicks(3U);
  auto expected = captureState(*machine);
  require(machine->restoreState(*checkpoint),
          "Machine rejected a valid pending SPU DMA snapshot");
  machine->advanceTicks(3U);
  auto replayed = captureState(*machine);
  require(sameMachineState(*expected, *replayed),
          "Restored pending SPU DMA did not replay deterministically");
}

} // namespace

int main() {
  try {
    testSpuMmioAndBidirectionalDma();
    testPendingSpuDmaSnapshot();
    std::cout << "SPU machine tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "SPU machine tests failed: " << error.what() << '\n';
    return 1;
  }
}
