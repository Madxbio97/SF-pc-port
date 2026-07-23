#include "sf/psx/cdrom.hpp"
#include "sf/psx/machine.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

constexpr std::uint8_t command_read_n = 0x06U;
constexpr std::uint8_t command_mute = 0x0bU;
constexpr std::uint8_t command_demute = 0x0cU;
constexpr std::uint8_t command_setfilter = 0x0dU;
constexpr std::uint8_t command_setmode = 0x0eU;
constexpr std::uint8_t interrupt_data_ready = 1U;
constexpr std::uint8_t interrupt_acknowledge = 3U;
constexpr std::uint8_t mode_filter_enabled = 1U << 3U;
constexpr std::uint8_t mode_xa_adpcm_enabled = 1U << 6U;
constexpr std::uint32_t cdrom_base = 0x1f801800U;

using Sector =
    std::array<std::byte, sf::psx::CdRomMedia::raw_sector_size>;

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void writeLe32(std::byte *destination, std::uint32_t value) noexcept {
  destination[0] = static_cast<std::byte>(value);
  destination[1] = static_cast<std::byte>(value >> 8U);
  destination[2] = static_cast<std::byte>(value >> 16U);
  destination[3] = static_cast<std::byte>(value >> 24U);
}

void writeMode2Header(Sector &sector) {
  sector[0] = std::byte{0U};
  std::fill(sector.begin() + 1U, sector.begin() + 11U, std::byte{0xffU});
  sector[11] = std::byte{0U};
  sector[12] = std::byte{0U};
  sector[13] = std::byte{2U};
  sector[14] = std::byte{0U};
  sector[15] = std::byte{2U};
}

Sector makeXaSector(std::uint8_t file, std::uint8_t channel,
                    std::uint32_t packed_samples = 0x11111111U,
                    std::uint8_t submode = 0x64U) {
  Sector sector{};
  writeMode2Header(sector);
  constexpr std::uint8_t coding = 0x00U;
  for (std::size_t copy = 0U; copy < 2U; ++copy) {
    const auto offset = 16U + copy * 4U;
    sector[offset] = static_cast<std::byte>(file);
    sector[offset + 1U] = static_cast<std::byte>(channel);
    sector[offset + 2U] = static_cast<std::byte>(submode);
    sector[offset + 3U] = static_cast<std::byte>(coding);
  }

  constexpr std::size_t payload_offset = 24U;
  constexpr std::size_t sound_group_size = 128U;
  constexpr std::size_t sound_group_count = 18U;
  constexpr std::size_t words_per_group = 28U;
  for (std::size_t group_index = 0U; group_index < sound_group_count;
       ++group_index) {
    auto *group = sector.data() + payload_offset +
                  group_index * sound_group_size;
    std::fill_n(group + 4U, 8U, std::byte{0x0cU});
    for (std::size_t word = 0U; word < words_per_group; ++word) {
      writeLe32(group + 16U + word * sizeof(std::uint32_t), packed_samples);
    }
  }
  return sector;
}

Sector makeDataSector(std::uint8_t seed) {
  Sector sector{};
  writeMode2Header(sector);
  constexpr std::uint8_t mode2_form1_data = 0x08U;
  sector[18] = static_cast<std::byte>(mode2_form1_data);
  sector[22] = static_cast<std::byte>(mode2_form1_data);
  for (std::size_t index = 0U; index < sf::psx::CdRomMedia::sector_size;
       ++index) {
    sector[24U + index] =
        static_cast<std::byte>(seed + static_cast<std::uint8_t>(index));
  }
  return sector;
}

class FakeCdRomMedia final : public sf::psx::CdRomMedia {
public:
  explicit FakeCdRomMedia(std::vector<Sector> sectors)
      : sectors_(std::move(sectors)) {}

  [[nodiscard]] std::uint32_t sectorCount() const noexcept override {
    return static_cast<std::uint32_t>(sectors_.size());
  }

  [[nodiscard]] bool readDataSector(
      std::uint32_t lba,
      std::span<std::byte, sector_size> destination) noexcept override {
    const auto index = static_cast<std::size_t>(lba);
    if (index >= sectors_.size()) {
      return false;
    }
    std::copy_n(sectors_[index].begin() + 24U, destination.size(),
                destination.begin());
    return true;
  }

  [[nodiscard]] bool readRawSector(
      std::uint32_t lba,
      std::span<std::byte, raw_sector_size> destination) noexcept override {
    const auto index = static_cast<std::size_t>(lba);
    if (index >= sectors_.size()) {
      return false;
    }
    std::copy(sectors_[index].begin(), sectors_[index].end(),
              destination.begin());
    ++raw_reads;
    return true;
  }

  std::size_t raw_reads{};

private:
  std::vector<Sector> sectors_;
};

struct XaSinkCall {
  std::uint8_t file{};
  std::uint8_t channel{};
  bool muted{};
};

class FakeXaAudioSink final : public sf::psx::CdRomXaAudioSink {
public:
  void consumeXaSector(
      std::span<const std::byte, sf::psx::CdRomMedia::raw_sector_size> sector,
      bool muted) noexcept override {
    if (call_count < calls.size()) {
      calls[call_count] = XaSinkCall{
          std::to_integer<std::uint8_t>(sector[16U]),
          std::to_integer<std::uint8_t>(sector[17U]), muted};
    }
    ++call_count;
  }

  void resetXaStream() noexcept override { ++reset_count; }

  void clear() noexcept {
    calls = {};
    call_count = 0U;
  }

  std::array<XaSinkCall, 8U> calls{};
  std::size_t call_count{};
  std::size_t reset_count{};
};

void selectIndex(sf::psx::CdRomController &controller, std::uint8_t index) {
  require(controller.writeRegister(0U, index),
          "Could not select a CD-ROM register bank");
}

void writeParameter(sf::psx::CdRomController &controller,
                    std::uint8_t value) {
  selectIndex(controller, 0U);
  require(controller.writeRegister(2U, value),
          "Could not write a CD-ROM command parameter");
}

void executeCommand(sf::psx::CdRomController &controller,
                    std::uint8_t command) {
  selectIndex(controller, 0U);
  require(controller.writeRegister(1U, command),
          "Could not issue a CD-ROM command");
  const auto request = controller.commandSchedule();
  require(request.pending != 0U,
          "CD-ROM command did not publish an execution event");
  controller.eventCommand(request.generation);
}

void acknowledge(sf::psx::CdRomController &controller,
                 std::uint8_t expected_interrupt) {
  require((controller.captureState().interrupt_flags & 0x07U) ==
              expected_interrupt,
          "CD-ROM raised the wrong interrupt reason");
  selectIndex(controller, 1U);
  require(controller.writeRegister(3U, expected_interrupt),
          "Could not acknowledge a CD-ROM interrupt");
  selectIndex(controller, 0U);
  require((controller.captureState().interrupt_flags & 0x07U) == 0U,
          "CD-ROM interrupt acknowledgement did not clear the reason");
}

void setMode(sf::psx::CdRomController &controller, std::uint8_t mode) {
  writeParameter(controller, mode);
  executeCommand(controller, command_setmode);
  require(controller.mode() == mode, "Setmode did not update the mode byte");
  acknowledge(controller, interrupt_acknowledge);
}

void setFilter(sf::psx::CdRomController &controller, std::uint8_t file,
               std::uint8_t channel) {
  writeParameter(controller, file);
  writeParameter(controller, channel);
  executeCommand(controller, command_setfilter);
  require(controller.filterFile() == file &&
              controller.filterChannel() == channel,
          "Setfilter did not update the XA selector");
  acknowledge(controller, interrupt_acknowledge);
}

void startRead(sf::psx::CdRomController &controller) {
  executeCommand(controller, command_read_n);
  require(controller.reading(), "ReadN did not enter the reading state");
  acknowledge(controller, interrupt_acknowledge);
  require(controller.sectorSchedule().pending != 0U,
          "ReadN acknowledgement did not schedule a sector");
}

void fireSector(sf::psx::CdRomController &controller) {
  const auto request = controller.sectorSchedule();
  require(request.pending != 0U, "CD-ROM did not publish a sector event");
  controller.eventSector(request.generation);
}

void testXaRoutingFilterAndControllerSnapshot() {
  FakeCdRomMedia media{{makeXaSector(1U, 2U, 0x11111111U),
                        makeXaSector(1U, 3U, 0x22222222U),
                        makeXaSector(1U, 2U, 0x33333333U)}};
  FakeXaAudioSink sink;
  sf::psx::CdRomController controller{&media};
  controller.setXaAudioSink(&sink);

  setMode(controller,
          static_cast<std::uint8_t>(mode_xa_adpcm_enabled |
                                    mode_filter_enabled));
  setFilter(controller, 1U, 2U);
  startRead(controller);

  fireSector(controller);
  const auto first_state = controller.captureState();
  require(sink.call_count == 1U && sink.calls[0].file == 1U &&
              sink.calls[0].channel == 2U && !sink.calls[0].muted,
          "Matching XA sector was not routed to the audio sink");
  require((first_state.interrupt_flags & 0x07U) == 0U &&
              first_state.data_valid == 0U && !controller.dmaRequest(),
          "XA audio incorrectly produced INT1 or a data FIFO");
  std::uint32_t rejected_dma_word{0xffffffffU};
  require(!controller.canReadDmaWords(1U) &&
              !controller.readDmaWord(rejected_dma_word) &&
              rejected_dma_word == 0U,
          "XA audio exposed a readable DMA3 word");
  require(first_state.current_lba == 1U &&
              first_state.sector_event.pending != 0U,
          "XA audio did not automatically schedule the next sector");

  auto checkpoint =
      std::make_unique<sf::psx::CdRomState>(controller.captureState());
  require(controller.validateState(*checkpoint),
          "Valid mid-read XA controller snapshot was rejected");

  fireSector(controller);
  auto expected =
      std::make_unique<sf::psx::CdRomState>(controller.captureState());
  require(sink.call_count == 1U && expected->current_lba == 2U &&
              expected->sector_event.pending != 0U &&
              (expected->interrupt_flags & 0x07U) == 0U,
          "Setfilter mismatch was routed or stalled the XA stream");

  auto corrupt = std::make_unique<sf::psx::CdRomState>(*checkpoint);
  corrupt->sector_event.pending = 0U;
  corrupt->sector_event.delay_ticks = 0U;
  const auto before_rejection = controller.captureState();
  require(!controller.restoreState(*corrupt) &&
              controller.captureState() == before_rejection,
          "Invalid active-read snapshot was accepted or mutated the CD-ROM");

  require(controller.restoreState(*checkpoint),
          "Valid mid-read XA controller snapshot did not restore");
  sink.clear();
  fireSector(controller);
  require(sink.call_count == 0U && controller.captureState() == *expected,
          "Restored XA controller did not replay the filtered sector");
}

void testMuteCommandsReachXaSink() {
  FakeCdRomMedia media{{makeXaSector(4U, 5U), makeXaSector(4U, 5U)}};
  FakeXaAudioSink sink;
  sf::psx::CdRomController controller{&media};
  controller.setXaAudioSink(&sink);
  setMode(controller, mode_xa_adpcm_enabled);
  startRead(controller);

  executeCommand(controller, command_mute);
  require(controller.muted(), "Mute did not update the CD-ROM mute state");
  acknowledge(controller, interrupt_acknowledge);
  fireSector(controller);
  require(sink.call_count == 1U && sink.calls[0].muted,
          "Muted XA sector did not reach the sink with muted=true");
  require(!controller.dmaRequest() &&
              (controller.captureState().interrupt_flags & 0x07U) == 0U,
          "Muted XA sector incorrectly entered the data path");

  executeCommand(controller, command_demute);
  require(!controller.muted(),
          "Demute did not update the CD-ROM mute state");
  acknowledge(controller, interrupt_acknowledge);
  sink.clear();
  fireSector(controller);
  require(sink.call_count == 1U && !sink.calls[0].muted,
          "Demuted XA sector did not reach the sink with muted=false");
}

void testSetFilterReleasesAutomaticXaLockWithoutFlushingDecoder() {
  FakeCdRomMedia media{{makeXaSector(1U, 2U), makeXaSector(1U, 3U)}};
  FakeXaAudioSink sink;
  sf::psx::CdRomController controller{&media};
  controller.setXaAudioSink(&sink);
  setMode(controller,
          static_cast<std::uint8_t>(mode_xa_adpcm_enabled |
                                    mode_filter_enabled));
  setFilter(controller, 1U, 2U);
  startRead(controller);

  fireSector(controller);
  require(sink.call_count == 1U &&
              controller.captureState().xa_current_set == 1U,
          "First filtered XA stream did not acquire the automatic lock");

  const auto resets_before_filter_change = sink.reset_count;
  setFilter(controller, 1U, 3U);
  const auto changed = controller.captureState();
  require(changed.xa_current_set == 0U &&
              changed.xa_current_file == 0U &&
              changed.xa_current_channel == 0U,
          "Setfilter retained the previous automatic XA stream lock");
  require(sink.reset_count == resets_before_filter_change,
          "Setfilter incorrectly flushed queued XA PCM or decoder history");

  fireSector(controller);
  require(sink.call_count == 2U && sink.calls[1].file == 1U &&
              sink.calls[1].channel == 3U,
          "Setfilter did not hand off immediately to the new XA stream");
}

void testAutomaticXaFileLockAndEofHandoff() {
  constexpr std::uint8_t xa_audio_eof_submode = 0xe4U;
  FakeCdRomMedia media{{makeXaSector(2U, 4U),
                        makeXaSector(2U, 5U),
                        makeXaSector(2U, 4U, 0x22222222U,
                                     xa_audio_eof_submode),
                        makeXaSector(2U, 5U)}};
  FakeXaAudioSink sink;
  sf::psx::CdRomController controller{&media};
  controller.setXaAudioSink(&sink);
  setMode(controller, mode_xa_adpcm_enabled);
  startRead(controller);

  fireSector(controller);
  auto state = controller.captureState();
  require(sink.call_count == 1U && state.xa_current_set != 0U &&
              state.xa_current_file == 2U && state.xa_current_channel == 4U,
          "Unfiltered XA stream did not lock to its first file/channel");

  fireSector(controller);
  require(sink.call_count == 1U,
          "Interleaved XA channel bypassed the active stream lock");

  fireSector(controller);
  state = controller.captureState();
  require(sink.call_count == 2U && state.xa_current_set == 0U,
          "XA EOF sector did not release the automatic stream lock");

  fireSector(controller);
  state = controller.captureState();
  require(sink.call_count == 3U && sink.calls[2].channel == 5U &&
              state.xa_current_set != 0U && state.xa_current_channel == 5U,
          "XA stream did not hand off to the channel after EOF");
}

void testNonAudioDataKeepsInt1AndDmaPath() {
  constexpr std::uint8_t seed = 0x31U;
  FakeCdRomMedia media{{makeDataSector(seed), makeDataSector(0x71U)}};
  sf::psx::CdRomController controller{&media};

  selectIndex(controller, 1U);
  require(controller.writeRegister(2U, 0x01U),
          "Could not enable CD-ROM INT1");
  selectIndex(controller, 0U);
  setMode(controller, mode_xa_adpcm_enabled);
  startRead(controller);
  fireSector(controller);

  const auto loaded = controller.captureState();
  require((loaded.interrupt_flags & 0x07U) == interrupt_data_ready &&
              controller.interruptLine() && loaded.data_valid != 0U &&
              loaded.data_requested == 0U && !controller.dmaRequest(),
          "Non-audio MODE2 sector lost its INT1/data-FIFO behavior");

  selectIndex(controller, 0U);
  require(controller.writeRegister(3U, 0x80U) && controller.dmaRequest() &&
              controller.canReadDmaWords(
                  sf::psx::CdRomMedia::sector_size / sizeof(std::uint32_t)),
          "Non-audio sector did not expose the DMA3 FIFO");

  std::uint32_t first_word{};
  auto expected_first_word = static_cast<std::uint32_t>(seed);
  expected_first_word += static_cast<std::uint32_t>(seed + 1U) * 0x100U;
  expected_first_word += static_cast<std::uint32_t>(seed + 2U) * 0x10000U;
  expected_first_word += static_cast<std::uint32_t>(seed + 3U) * 0x1000000U;
  require(controller.readDmaWord(first_word) &&
              first_word == expected_first_word,
          "DMA3 read the wrong MODE2 user-data bytes");

  auto checkpoint =
      std::make_unique<sf::psx::CdRomState>(controller.captureState());
  require(controller.validateState(*checkpoint),
          "Valid mid-DMA CD-ROM snapshot was rejected");
  std::uint32_t expected_second_word{};
  require(controller.readDmaWord(expected_second_word),
          "Could not advance the DMA FIFO after its snapshot");
  require(controller.restoreState(*checkpoint),
          "Valid mid-DMA CD-ROM snapshot did not restore");
  std::uint32_t replayed_second_word{};
  require(controller.readDmaWord(replayed_second_word) &&
              replayed_second_word == expected_second_word,
          "Restored DMA FIFO did not replay deterministically");

  acknowledge(controller, interrupt_data_ready);
  require(controller.sectorSchedule().pending != 0U,
          "INT1 acknowledgement did not resume the data-sector stream");
}

void testCdInputVolumeMatrixRegisters() {
  sf::psx::CdRomController controller;
  selectIndex(controller, 2U);
  require(controller.writeRegister(1U, 0x40U) &&
              controller.writeRegister(2U, 0x20U) &&
              controller.writeRegister(3U, 0x30U),
          "Could not write pending CD volume matrix bank 2");
  selectIndex(controller, 3U);
  require(controller.writeRegister(1U, 0x10U) &&
              controller.writeRegister(2U, 0x21U),
          "Could not write/apply CD volume matrix bank 3");
  const auto applied = controller.captureState();
  require(applied.pending_cd_volume_matrix ==
              std::array<std::uint8_t, 4U>{0x40U, 0x20U, 0x10U, 0x30U} &&
              applied.cd_volume_matrix == applied.pending_cd_volume_matrix &&
              applied.adpcm_muted == 1U && controller.validateState(applied),
          "CD volume apply/mute register semantics are incorrect");

  require(controller.writeRegister(2U, 0U),
          "Could not clear CD ADPCM mute bit");
  const auto unmuted = controller.captureState();
  require(unmuted.adpcm_muted == 0U &&
              unmuted.cd_volume_matrix == applied.cd_volume_matrix,
          "CD ADPCM unmute changed the applied volume matrix");
}

void machineWrite(sf::psx::R3000Runtime &runtime, std::uint32_t address,
                  std::uint8_t value, const char *message) {
  require(runtime.write8(address, value), message);
}

void machineAcknowledge(sf::psx::R3000Runtime &runtime,
                        std::uint8_t interrupt) {
  machineWrite(runtime, cdrom_base, 1U,
               "Could not select machine CD-ROM index 1");
  machineWrite(runtime, cdrom_base + 3U, interrupt,
               "Could not acknowledge the machine CD-ROM interrupt");
  machineWrite(runtime, cdrom_base, 0U,
               "Could not restore machine CD-ROM index 0");
}

void machineExecuteCommand(sf::psx::R3000Runtime &runtime,
                           sf::psx::PsxMachine &machine,
                           std::uint8_t command) {
  machineWrite(runtime, cdrom_base + 1U, command,
               "Could not issue a machine CD-ROM command");
  machine.advanceTicks(sf::psx::CdRomController::command_delay_ticks);
  require((machine.cdrom().captureState().interrupt_flags & 0x07U) ==
              interrupt_acknowledge,
          "Machine CD-ROM command did not produce INT3");
}

void machineSetMode(sf::psx::R3000Runtime &runtime,
                    sf::psx::PsxMachine &machine, std::uint8_t mode) {
  machineWrite(runtime, cdrom_base + 2U, mode,
               "Could not write machine Setmode parameter");
  machineExecuteCommand(runtime, machine, command_setmode);
  machineAcknowledge(runtime, interrupt_acknowledge);
}

void machineStartRead(sf::psx::R3000Runtime &runtime,
                      sf::psx::PsxMachine &machine) {
  machineExecuteCommand(runtime, machine, command_read_n);
  machineAcknowledge(runtime, interrupt_acknowledge);
  require(machine.cdrom().sectorSchedule().pending != 0U,
          "Machine ReadN did not schedule the first sector");
}

void testMachineXaDecoderSpuQueueAndSnapshot() {
  auto runtime = std::make_unique<sf::psx::R3000Runtime>();
  auto machine = std::make_unique<sf::psx::PsxMachine>(*runtime);
  FakeCdRomMedia media{{makeXaSector(7U, 1U, 0x11111111U),
                        makeXaSector(7U, 1U, 0x22222222U),
                        makeXaSector(7U, 1U, 0x33333333U)}};
  machine->setCdRomMedia(&media);

  machineSetMode(*runtime, *machine, mode_xa_adpcm_enabled);
  machineStartRead(*runtime, *machine);
  machine->advanceTicks(sf::psx::CdRomController::sector_single_speed_ticks);
  require(machine->spu().queuedCdFrames() == 4704U &&
              machine->cdrom().currentLba() == 1U &&
              (machine->cdrom().captureState().interrupt_flags & 0x07U) == 0U,
          "Machine XA sink did not decode the first sector into SPU CD PCM");

  auto checkpoint = std::make_unique<sf::psx::PsxMachineState>();
  *checkpoint = machine->captureState();
  require(machine->validateState(*checkpoint),
          "Machine rejected its mid-XA snapshot");

  machine->advanceTicks(sf::psx::CdRomController::sector_single_speed_ticks);
  auto expected = std::make_unique<sf::psx::PsxMachineState>();
  *expected = machine->captureState();
  require(expected->spu != nullptr && checkpoint->spu != nullptr &&
              expected->cdrom.current_lba == 2U &&
              expected->spu->cd_frame_count <
                  checkpoint->spu->cd_frame_count &&
              expected->xa_decoder == checkpoint->xa_decoder,
          "Busy XA FIFO decoded a sector and corrupted predictor history");

  machine->advanceTicks(1234U);
  require(machine->restoreState(*checkpoint) &&
              machine->spu().queuedCdFrames() ==
                  checkpoint->spu->cd_frame_count &&
              machine->cdrom().currentLba() == checkpoint->cdrom.current_lba,
          "Machine snapshot did not restore the XA decoder/CD queue boundary");
  machine->advanceTicks(sf::psx::CdRomController::sector_single_speed_ticks);
  auto replayed = std::make_unique<sf::psx::PsxMachineState>();
  *replayed = machine->captureState();
  require(replayed->spu != nullptr && expected->spu != nullptr &&
              replayed->cdrom == expected->cdrom &&
              *replayed->spu == *expected->spu &&
              replayed->xa_decoder == expected->xa_decoder,
          "Restored machine did not replay XA decode and SPU queue exactly");

  machine->spu().mixFrames(machine->spu().queuedCdFrames());
  const auto decoder_before_admission = machine->captureState().xa_decoder;
  machine->advanceTicks(sf::psx::CdRomController::sector_single_speed_ticks);
  require(machine->cdrom().currentLba() == 3U &&
              machine->spu().queuedCdFrames() != 0U &&
              machine->captureState().xa_decoder != decoder_before_admission,
          "Drained XA FIFO did not admit the following complete sector");
}

} // namespace

int main() {
  try {
    testXaRoutingFilterAndControllerSnapshot();
    testMuteCommandsReachXaSink();
    testSetFilterReleasesAutomaticXaLockWithoutFlushingDecoder();
    testAutomaticXaFileLockAndEofHandoff();
    testNonAudioDataKeepsInt1AndDmaPath();
    testCdInputVolumeMatrixRegisters();
    testMachineXaDecoderSpuQueueAndSnapshot();
    std::cout << "CD/XA routing tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "CD/XA routing tests failed: " << error.what() << '\n';
    return 1;
  }
}
