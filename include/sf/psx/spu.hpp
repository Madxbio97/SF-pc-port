#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace sf::psx {

struct SpuPcmFrame {
  std::int16_t left{};
  std::int16_t right{};

  bool operator==(const SpuPcmFrame &) const = default;
};

enum class SpuAdsrPhase : std::uint8_t {
  off,
  attack,
  decay,
  sustain,
  release,
};

struct SpuVolumeEnvelopeState {
  std::uint32_t counter{};
  std::uint16_t counter_increment{};
  std::int16_t step{};
  std::uint8_t rate{};
  std::uint8_t decreasing{};
  std::uint8_t exponential{};
  std::uint8_t phase_invert{};

  bool operator==(const SpuVolumeEnvelopeState &) const = default;
};

struct SpuVolumeSweepState {
  SpuVolumeEnvelopeState envelope{};
  std::int16_t current_level{};
  std::uint8_t envelope_active{};

  bool operator==(const SpuVolumeSweepState &) const = default;
};

struct SpuVoiceState {
  static constexpr std::size_t samples_per_block = 28U;

  std::array<std::int16_t, samples_per_block> decoded_samples{};
  std::array<std::int16_t, 3U> interpolation_samples{};
  std::int32_t previous_sample{};
  std::int32_t older_sample{};
  // Post-ADSR mono sample. Voice N-1 feeds voice N's hardware pitch
  // modulation path, so this value is guest state rather than host output.
  std::int32_t last_volume{};
  std::uint32_t block_address{};
  std::uint32_t repeat_address{};
  std::uint32_t envelope_counter{};
  std::uint16_t pitch_counter{};
  std::uint16_t sample_index{};
  std::uint16_t envelope{};
  SpuAdsrPhase adsr_phase{SpuAdsrPhase::off};
  std::uint8_t block_flags{};
  std::uint8_t active{};
  std::uint8_t block_valid{};
  std::uint8_t first_block{};
  std::uint8_t ignore_loop_address{};

  bool operator==(const SpuVoiceState &) const = default;
};

// Fixed-size, pointer-free guest state. The bounded host output queue is
// deliberately excluded; restoring a state discards already-rendered audio.
struct SpuState {
  static constexpr std::size_t ram_size = 512U * 1024U;
  static constexpr std::size_t register_count = 0x130U;
  static constexpr std::size_t voice_count = 24U;
  static constexpr std::size_t cd_queue_capacity = 16384U;

  std::array<std::byte, ram_size> ram{};
  std::array<std::uint16_t, register_count> registers{};
  std::array<SpuVoiceState, voice_count> voices{};
  std::array<std::array<SpuVolumeSweepState, 2U>, voice_count> voice_volume{};
  std::array<SpuVolumeSweepState, 2U> main_volume{};
  std::array<SpuPcmFrame, cd_queue_capacity> cd_audio{};
  std::array<std::array<std::int16_t, 128U>, 2U> reverb_downsample_buffer{};
  std::array<std::array<std::int16_t, 64U>, 2U> reverb_upsample_buffer{};
  // CD controller input matrix: L->L, L->R, R->L, R->R.
  std::array<std::uint8_t, 4U> cd_input_matrix{0x80U, 0U, 0U, 0x80U};

  std::uint64_t mixed_frames{};
  std::uint64_t sample_clock{};
  std::uint32_t transfer_address{};
  std::uint32_t reverb_current_address{};
  std::uint32_t endx{};
  std::uint32_t noise_count{};
  std::uint16_t noise_level{1U};
  std::uint16_t cd_read_position{};
  std::uint16_t cd_write_position{};
  std::uint16_t cd_frame_count{};
  std::uint8_t reverb_resample_position{};
  std::uint8_t irq_latched{};
  std::uint8_t transfer_busy{};

  bool operator==(const SpuState &) const = default;
};

// Deterministic PlayStation SPU subset. Register offsets are relative to
// 0x1f801c00 and identify complete aligned halfwords.
class Spu final {
public:
  static constexpr std::size_t ram_size = SpuState::ram_size;
  static constexpr std::size_t voice_count = SpuState::voice_count;
  static constexpr std::uint32_t register_span = 0x260U;
  static constexpr std::uint32_t cpu_clock_hz = 33'868'800U;
  static constexpr std::uint32_t sample_rate = 44'100U;
  // Guest streaming/loading can complete a long CPU slice before the host
  // presentation thread pumps audio. Keep the same two-second safety window
  // used by mature PSX audio pipelines so that slice cannot overwrite the
  // beginning of the already-mixed waveform.
  static constexpr std::size_t pcm_queue_capacity = sample_rate * 2U;

  Spu();
  Spu(const Spu &) = delete;
  Spu &operator=(const Spu &) = delete;
  Spu(Spu &&) = delete;
  Spu &operator=(Spu &&) = delete;

  void reset() noexcept;

  [[nodiscard]] bool readRegister(std::uint32_t offset,
                                  std::uint16_t &value) noexcept;
  [[nodiscard]] bool writeRegister(std::uint32_t offset,
                                   std::uint16_t value) noexcept;

  // DMA channel 4 endpoint. A request is asserted for SPUCNT transfer modes
  // 2 (RAM to SPU) and 3 (SPU to RAM).
  [[nodiscard]] bool dmaRequest() const noexcept;
  [[nodiscard]] bool readDmaWord(std::uint32_t &value) noexcept;
  [[nodiscard]] bool writeDmaWord(std::uint32_t value) noexcept;

  [[nodiscard]] bool interruptLine() const noexcept;
  void setDmaTransferBusy(bool busy) noexcept {
    state_->transfer_busy = busy ? 1U : 0U;
  }

  // Both entry points advance the same 44.1 kHz mixer. advanceCpuTicks keeps
  // the fractional CPU-to-SPU clock in the snapshot.
  void advanceCpuTicks(std::uint64_t ticks) noexcept;
  void mixFrames(std::size_t frame_count) noexcept;

  // CD/XA samples are already decoded stereo PCM at 44.1 kHz. Accepted
  // frames are serialized because they affect future guest-visible output.
  [[nodiscard]] std::size_t
  pushCdAudio(std::span<const SpuPcmFrame> frames) noexcept;
  [[nodiscard]] std::size_t queuedCdFrames() const noexcept {
    return state_->cd_frame_count;
  }
  void clearCdAudio() noexcept;
  void setCdInputMixer(std::array<std::uint8_t, 4U> matrix) noexcept {
    state_->cd_input_matrix = matrix;
  }

  [[nodiscard]] std::size_t queuedPcmFrames() const noexcept {
    return pcm_frame_count_;
  }
  [[nodiscard]] std::size_t
  takePcm(std::span<SpuPcmFrame> destination) noexcept;
  [[nodiscard]] std::size_t
  copyPcm(std::span<SpuPcmFrame> destination) const noexcept;
  [[nodiscard]] bool restorePcm(std::span<const SpuPcmFrame> frames,
                                std::uint64_t dropped_frames = 0U) noexcept;
  void clearPcm() noexcept;
  [[nodiscard]] std::uint64_t droppedPcmFrames() const noexcept {
    return dropped_pcm_frames_;
  }

  [[nodiscard]] std::span<const std::byte, ram_size> ram() const noexcept {
    return state_->ram;
  }
  [[nodiscard]] std::uint32_t endx() const noexcept { return state_->endx; }
  [[nodiscard]] std::uint16_t control() const noexcept;
  [[nodiscard]] std::uint16_t status() const noexcept;

  [[nodiscard]] const SpuState &state() const noexcept { return *state_; }
  [[nodiscard]] SpuState captureState() const noexcept { return *state_; }
  [[nodiscard]] bool validateState(const SpuState &state) const noexcept;
  [[nodiscard]] bool restoreState(const SpuState &state) noexcept;

private:
  [[nodiscard]] static bool validFlag(std::uint8_t value) noexcept;
  [[nodiscard]] static std::uint32_t ramAddress(std::uint16_t value) noexcept;
  [[nodiscard]] static std::int32_t fixedVolume(std::uint16_t value) noexcept;
  [[nodiscard]] static std::int16_t clampSample(std::int64_t value) noexcept;
  [[nodiscard]] static std::int32_t
  floorDivPowerOfTwo(std::int32_t value, std::uint32_t shift) noexcept;
  [[nodiscard]] static std::int64_t
  floorDivPowerOfTwoWide(std::int64_t value, std::uint32_t shift) noexcept;
  static void resetVolumeSweep(SpuVolumeSweepState &sweep,
                               std::uint16_t value) noexcept;
  static void tickVolumeSweep(SpuVolumeSweepState &sweep) noexcept;

  void keyOn(std::uint32_t mask) noexcept;
  void keyOff(std::uint32_t mask) noexcept;
  [[nodiscard]] bool decodeBlock(std::size_t voice_index) noexcept;
  void finishBlock(std::size_t voice_index, bool noise_enabled) noexcept;
  [[nodiscard]] std::int32_t voiceSample(std::size_t voice_index) noexcept;
  void advanceVoice(std::size_t voice_index, std::uint16_t pitch,
                    bool noise_enabled) noexcept;
  void advanceEnvelope(std::size_t voice_index) noexcept;
  void advanceNoiseFrames(std::uint64_t frames) noexcept;
  [[nodiscard]] std::uint32_t
  reverbMemoryAddress(std::uint32_t address) const noexcept;
  [[nodiscard]] std::int16_t reverbRead(std::uint16_t address,
                                        std::int32_t offset = 0) const noexcept;
  void reverbWrite(std::uint16_t address, std::int16_t value) noexcept;
  [[nodiscard]] SpuPcmFrame processReverb(std::int32_t left,
                                          std::int32_t right) noexcept;

  [[nodiscard]] std::uint16_t readRamHalfword() noexcept;
  void writeRamHalfword(std::uint16_t value) noexcept;
  void touchRam(std::uint32_t address) noexcept;

  [[nodiscard]] SpuPcmFrame popCdFrame() noexcept;
  void pushPcmFrame(SpuPcmFrame frame) noexcept;
  [[nodiscard]] bool idleForFastForward() const noexcept;
  void fastForwardSilentFrames(std::uint64_t frames) noexcept;

  // Keep the device small enough to coexist with one inline snapshot on the
  // default Windows thread stack. The snapshot itself remains pointer-free.
  std::unique_ptr<SpuState> state_;
  std::unique_ptr<std::array<SpuPcmFrame, pcm_queue_capacity>> pcm_queue_;
  std::size_t pcm_read_position_{};
  std::size_t pcm_write_position_{};
  std::size_t pcm_frame_count_{};
  std::uint64_t dropped_pcm_frames_{};
};

} // namespace sf::psx
