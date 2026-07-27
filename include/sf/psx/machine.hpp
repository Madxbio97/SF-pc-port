#pragma once

#include "sf/psx/cdrom.hpp"
#include "sf/psx/dma.hpp"
#include "sf/psx/event_scheduler.hpp"
#include "sf/psx/interrupt_controller.hpp"
#include "sf/psx/r3000_runtime.hpp"
#include "sf/psx/spu.hpp"
#include "sf/psx/timers.hpp"
#include "sf/psx/xa_decoder.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace sf::psx {

struct CpuClockScale {
  std::uint32_t numerator{1U};
  std::uint32_t denominator{1U};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return numerator != 0U && denominator != 0U;
  }

  [[nodiscard]] friend constexpr bool operator==(const CpuClockScale &,
                                                  const CpuClockScale &) =
      default;
};

class DmaPort {
public:
  virtual ~DmaPort() = default;
  [[nodiscard]] virtual bool dmaRequest() const noexcept { return true; }
  [[nodiscard]] virtual bool readDmaWord(std::uint32_t &value) noexcept = 0;
  [[nodiscard]] virtual bool writeDmaWord(std::uint32_t value) noexcept = 0;
};

struct PsxMachineState {
  CpuClockScale cpu_clock_scale{};
  EventSchedulerState scheduler;
  InterruptControllerState interrupts;
  DmaControllerState dma;
  CdRomState cdrom;
  std::unique_ptr<SpuState> spu{std::make_unique<SpuState>()};
  XaDecoderState xa_decoder;
  RootTimersState timers;
  std::uint64_t pending_cpu_ticks{};
  std::uint32_t device_tick_remainder{};

  PsxMachineState() = default;
  PsxMachineState(PsxMachineState &&) noexcept = default;
  PsxMachineState &operator=(PsxMachineState &&) noexcept = default;

  PsxMachineState(const PsxMachineState &other)
      : cpu_clock_scale(other.cpu_clock_scale), scheduler(other.scheduler),
        interrupts(other.interrupts), dma(other.dma), cdrom(other.cdrom),
        spu(other.spu ? std::make_unique<SpuState>(*other.spu) : nullptr),
        xa_decoder(other.xa_decoder), timers(other.timers),
        pending_cpu_ticks(other.pending_cpu_ticks),
        device_tick_remainder(other.device_tick_remainder) {}

  PsxMachineState &operator=(const PsxMachineState &other) {
    if (this == &other) {
      return *this;
    }
    cpu_clock_scale = other.cpu_clock_scale;
    scheduler = other.scheduler;
    interrupts = other.interrupts;
    dma = other.dma;
    cdrom = other.cdrom;
    if (other.spu) {
      if (!spu) {
        spu = std::make_unique<SpuState>();
      }
      *spu = *other.spu;
    } else {
      spu.reset();
    }
    xa_decoder = other.xa_decoder;
    timers = other.timers;
    pending_cpu_ticks = other.pending_cpu_ticks;
    device_tick_remainder = other.device_tick_remainder;
    return *this;
  }
};

// Hardware layer around the interpreter. The scheduler uses CPU-domain ticks.
// Device clocks are unscaled with a serialized carry, matching DuckStation's
// overclock model: a faster CPU receives more work in the same hardware time.
class PsxMachine final : private R3000MmioBus, private CdRomXaAudioSink {
public:
  static constexpr std::uint64_t cpu_clock_hz = 33'868'800U;

  explicit PsxMachine(R3000Runtime &cpu,
                      CpuClockScale cpu_clock_scale = {});
  ~PsxMachine() override;
  PsxMachine(const PsxMachine &) = delete;
  PsxMachine &operator=(const PsxMachine &) = delete;

  void reset() noexcept;
  [[nodiscard]] R3000RunResult step() noexcept;
  void advanceTicks(std::uint64_t ticks) noexcept;
  // Commit already-accounted interpreter ticks to hardware devices without
  // consuming any additional guest CPU time.
  void synchronizeDevices() noexcept { flushPendingCpuTicks(); }
  void advanceHardwareTicks(std::uint64_t ticks) noexcept {
    advanceTicks(scaleDeviceTicks(ticks));
  }

  void attachDmaPort(DmaChannel channel, DmaPort *port) noexcept;
  void setCdRomMedia(CdRomMedia *media) noexcept;
  void serviceDmaRequests() noexcept;
  void setVBlank(bool active) noexcept;
  void pulseVBlank() noexcept;
  void setHBlank(bool active) noexcept;
  void advanceDotClocks(std::uint64_t clocks) noexcept;

  [[nodiscard]] std::uint64_t currentTick() const noexcept;
  [[nodiscard]] std::uint64_t cpuTicksPerSecond() const noexcept;
  [[nodiscard]] CpuClockScale cpuClockScale() const noexcept {
    return cpu_clock_scale_;
  }
  [[nodiscard]] std::optional<std::uint64_t>
  dmaCompletionTick(DmaChannel channel) const noexcept;
  // Clock-neutral guest callbacks still need MMIO DMA requests to become
  // observable before a retail busy-wait can return. Complete those scheduled
  // transfers without advancing the CPU/SPU/CD timeline.
  [[nodiscard]] bool completePendingDmaTransfers() noexcept;
  // Synchronous HLE CD reads cannot yield back to the realtime 120 Hz clock.
  // Complete one scheduled CD event in-place so their busy-waits make forward
  // progress without rendering a block of future SPU audio in one host frame.
  [[nodiscard]] bool completeNextPendingCdRomEvent() noexcept;
  [[nodiscard]] const InterruptController &interrupts() const noexcept {
    return interrupts_;
  }
  [[nodiscard]] const DmaController &dma() const noexcept { return dma_; }
  [[nodiscard]] const CdRomController &cdrom() const noexcept { return cdrom_; }
  [[nodiscard]] CdRomController &cdrom() noexcept { return cdrom_; }
  [[nodiscard]] const Spu &spu() const noexcept { return spu_; }
  [[nodiscard]] Spu &spu() noexcept { return spu_; }
  [[nodiscard]] const RootTimers &timers() const noexcept { return timers_; }
  [[nodiscard]] PsxMachineState captureState() const;
  [[nodiscard]] bool validateState(const PsxMachineState &state) const noexcept;
  [[nodiscard]] bool restoreState(const PsxMachineState &state) noexcept;

private:
  [[nodiscard]] bool readMmio(std::uint32_t physical_address,
                              R3000AccessWidth width,
                              std::uint32_t &value) noexcept override;
  [[nodiscard]] bool writeMmio(std::uint32_t physical_address,
                               R3000AccessWidth width,
                               std::uint32_t value) noexcept override;
  void consumeXaSector(
      std::span<const std::byte, CdRomMedia::raw_sector_size> sector,
      bool muted) noexcept override;
  void resetXaStream() noexcept override;
  void setXaOutputMixer(
      std::array<std::uint8_t, 4U> matrix) noexcept override;

  void advanceDevicesTo(std::uint64_t tick) noexcept;
  void flushPendingCpuTicks() noexcept;
  void queueCpuTicks(std::uint64_t ticks) noexcept;
  [[nodiscard]] std::uint64_t
  scaleDeviceTicks(std::uint64_t ticks) const noexcept;
  [[nodiscard]] std::uint64_t
  unscaleCpuTicks(std::uint64_t ticks) noexcept;
  void dispatchEvent(const MachineEvent &event) noexcept;
  void routeTimerInterrupts(RootTimers::IrqMask mask) noexcept;
  void syncCdRomSchedules() noexcept;
  void syncCdRomInterruptLine() noexcept;
  void syncDmaInterruptLine() noexcept;
  void syncSpuInterruptLine() noexcept;
  void syncCpuInterruptLine() noexcept;
  void kickDmaChannels() noexcept;
  [[nodiscard]] bool executeDmaTransfer(DmaChannel channel) noexcept;
  [[nodiscard]] bool executeLinearDma(DmaChannel channel) noexcept;
  [[nodiscard]] bool executeLinkedListDma(DmaChannel channel) noexcept;
  [[nodiscard]] bool executeOtcDma() noexcept;
  [[nodiscard]] bool linkedListTicks(std::uint64_t &ticks) const noexcept;

  R3000Runtime &cpu_;
  EventScheduler scheduler_;
  InterruptController interrupts_;
  DmaController dma_;
  CdRomController cdrom_;
  Spu spu_;
  XaAudioDecoder xa_decoder_;
  RootTimers timers_;
  CpuClockScale cpu_clock_scale_{};
  std::uint64_t pending_cpu_ticks_{};
  std::uint32_t device_tick_remainder_{};

  class SpuDmaPort final : public DmaPort {
  public:
    explicit SpuDmaPort(Spu &spu) noexcept : spu_(spu) {}

    [[nodiscard]] bool dmaRequest() const noexcept override {
      return spu_.dmaRequest();
    }
    [[nodiscard]] bool readDmaWord(std::uint32_t &value) noexcept override {
      return spu_.readDmaWord(value);
    }
    [[nodiscard]] bool writeDmaWord(std::uint32_t value) noexcept override {
      return spu_.writeDmaWord(value);
    }

  private:
    Spu &spu_;
  };

  SpuDmaPort spu_dma_port_{spu_};
  std::array<DmaPort *, DmaController::channel_count> dma_ports_{};
};

} // namespace sf::psx
