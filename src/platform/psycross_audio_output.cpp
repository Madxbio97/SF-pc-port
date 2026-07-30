#include "psycross_audio_output.hpp"

#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/psx/vab_decoder.hpp"

#include <AL/alc.h>
#include <PsyX/PsyX_globals.h>
#include <SDL.h>
#include <psx/libspu.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

namespace sf::platform::detail {
namespace {

std::size_t context_references{};
bool context_owned{};

const char *sourceStateName(ALint state) noexcept {
  switch (state) {
  case AL_INITIAL:
    return "initial";
  case AL_PLAYING:
    return "playing";
  case AL_PAUSED:
    return "paused";
  case AL_STOPPED:
    return "stopped";
  default:
    return "unknown";
  }
}

void requireAl(const char *operation) {
  const auto error = alGetError();
  if (error != AL_NO_ERROR) {
    throw core::Error{core::ErrorCode::io, std::string{operation} +
                                               " (OpenAL error " +
                                               std::to_string(error) + ')'};
  }
}

} // namespace

bool psyCrossAudioDiagnosticsEnabled() noexcept {
  static const auto enabled = [] {
    const auto *value = SDL_getenv("SF_AUDIO_DIAGNOSTICS");
    // Keep the one-second clock/SPU/sink trace enabled in public builds.  The
    // log is bounded by presentation time rather than callback count and is
    // the only reliable way to distinguish guest under-production, host
    // scheduling stalls and device underruns after a long play session.
    return value == nullptr || value[0] == '\0' || std::strcmp(value, "0") != 0;
  }();
  return enabled;
}

bool callbackBufferRequested() noexcept {
  static const auto requested = [] {
    const auto *value = SDL_getenv("SF_OPENAL_CALLBACK_BUFFER");
    // OpenAL Soft's callback buffer is the primary continuous sink. It keeps
    // the device source alive across host scheduling jitter and avoids the
    // stop/unqueue/restart races inherent to short AL buffer queues. Keep the
    // queued implementation as a compatibility fallback and an explicit
    // diagnostic override.
    return value == nullptr || value[0] == '\0' || std::strcmp(value, "0") != 0;
  }();
  return requested;
}

PsyCrossAudioContext::PsyCrossAudioContext() {
  if (context_references == 0U) {
    context_owned = alcGetCurrentContext() == nullptr;
    if (context_owned) {
      SpuInit();
    }
    if (alcGetCurrentContext() == nullptr) {
      if (context_owned) {
        SpuQuit();
      }
      context_owned = false;
      throw core::Error{core::ErrorCode::io, "Cannot initialize audio output"};
    }
  }
  ++context_references;
}

PsyCrossAudioContext::~PsyCrossAudioContext() {
  if (context_references == 0U) {
    return;
  }
  --context_references;
  if (context_references == 0U && context_owned) {
    SpuQuit();
    context_owned = false;
  }
}

PsyCrossAudioOutput::PsyCrossAudioOutput(std::size_t minimum_start_buffers,
                                         std::string diagnostic_name,
                                         PsyCrossAudioStreamKind stream_kind)
    : diagnostic_name_(std::move(diagnostic_name)), stream_kind_(stream_kind),
      minimum_start_frames_(std::clamp<std::size_t>(minimum_start_buffers, 1U,
                                                    maximum_queued_buffers) *
                            frames_per_buffer),
      start_policy_(std::clamp<std::size_t>(
          stream_kind == PsyCrossAudioStreamKind::continuous
              ? std::max<std::size_t>(minimum_start_buffers, 8U)
              : minimum_start_buffers,
          1U, maximum_queued_buffers)) {
  static_assert(sizeof(psx::SpuPcmFrame) == 2U * sizeof(std::int16_t));
  static_assert(std::is_trivially_copyable_v<psx::SpuPcmFrame>);

  buffers_.reserve(maximum_queued_buffers);
  available_.reserve(maximum_queued_buffers);
  staged_frames_.reserve(maximum_staged_frames);
  upload_scratch_.reserve(frames_per_buffer);

  alGetError();
  alGenSources(1, &source_);
  try {
    requireAl("Cannot create gameplay audio source");
    alSourcei(source_, AL_SOURCE_RELATIVE, AL_TRUE);
    requireAl("Cannot configure gameplay audio source");
    alSourcef(source_, AL_GAIN, gain_policy_.gain());
    requireAl("Cannot configure gameplay audio gain");

    // The callback buffer consumes the SPSC ring directly on OpenAL's device
    // thread. Variable callback sizes are harmless because producer overflow
    // remains in FIFO staging and is copied into the ring by update()/flush().
    // No resampling, time stretching or newest-tail replacement is involved.
    if (stream_kind_ == PsyCrossAudioStreamKind::continuous &&
        callbackBufferRequested() &&
        alIsExtensionPresent("AL_SOFT_callback_buffer") == AL_TRUE) {
      buffer_callback_ = reinterpret_cast<LPALBUFFERCALLBACKSOFT>(
          alGetProcAddress("alBufferCallbackSOFT"));
      if (buffer_callback_ != nullptr) {
        alGenBuffers(1, &callback_buffer_);
        requireAl("Cannot create callback audio buffer");
        buffer_callback_(callback_buffer_, AL_FORMAT_STEREO16,
                         static_cast<ALsizei>(psx::Spu::sample_rate),
                         &PsyCrossAudioOutput::streamCallback, this);
        requireAl("Cannot configure callback audio buffer");
        alSourcei(source_, AL_BUFFER, static_cast<ALint>(callback_buffer_));
        requireAl("Cannot attach callback audio buffer");
      }
    }

    auto *context = alcGetCurrentContext();
    auto *device = context != nullptr ? alcGetContextsDevice(context) : nullptr;
    ALCint frequency{};
    ALCint refresh{};
    ALCint synchronous{};
    if (device != nullptr) {
      alcGetIntegerv(device, ALC_FREQUENCY, 1, &frequency);
      alcGetIntegerv(device, ALC_REFRESH, 1, &refresh);
      alcGetIntegerv(device, ALC_SYNC, 1, &synchronous);
    }
    if (buffer_callback_ != nullptr && refresh > 0) {
      // One complete device update is the smallest useful prebuffer. It keeps
      // callback requests whole while removing the old fixed eight-block
      // (~23 ms) delay that became obvious at 120/144/240 Hz presentation.
      const auto device_update_frames =
          (static_cast<std::size_t>(psx::Spu::sample_rate) +
           static_cast<std::size_t>(refresh) - 1U) /
          static_cast<std::size_t>(refresh);
      minimum_start_frames_ =
          std::max(minimum_start_frames_, device_update_frames);
    }
    const auto *device_name = device != nullptr
                                  ? alcGetString(device, ALC_DEVICE_SPECIFIER)
                                  : nullptr;
    PsyX_Log_Info(
        "[AudioDiag][open] role=%s source=%u device=\"%s\" "
        "device_hz=%d refresh=%d sync=%d sample_hz=%u block_frames=%zu "
        "start_buffers=%zu start_frames=%zu max_buffers=%zu sink=%s "
        "ring_frames=%zu callback_opt_in=%u\n",
        diagnostic_name_.c_str(), static_cast<unsigned int>(source_),
        device_name != nullptr ? device_name : "unknown", frequency, refresh,
        synchronous, psx::Spu::sample_rate, frames_per_buffer,
        std::clamp<std::size_t>(minimum_start_buffers, 1U,
                                maximum_queued_buffers),
        minimum_start_frames_, maximum_queued_buffers,
        buffer_callback_ != nullptr ? "callback" : "queued",
        stream_frames_.capacity(), callbackBufferRequested() ? 1U : 0U);
  } catch (...) {
    if (callback_buffer_ != 0U) {
      alSourcei(source_, AL_BUFFER, 0);
      alDeleteBuffers(1, &callback_buffer_);
      callback_buffer_ = 0U;
    }
    if (source_ != 0U) {
      alDeleteSources(1, &source_);
      source_ = 0U;
    }
    throw;
  }
}

PsyCrossAudioOutput::~PsyCrossAudioOutput() {
  if (source_ == 0U) {
    return;
  }
  reset("destroy");
  if (callback_buffer_ != 0U) {
    alSourcei(source_, AL_BUFFER, 0);
    alDeleteBuffers(1, &callback_buffer_);
    callback_buffer_ = 0U;
  }
  alDeleteSources(1, &source_);
  if (!buffers_.empty()) {
    alDeleteBuffers(static_cast<ALsizei>(buffers_.size()), buffers_.data());
  }
}

void PsyCrossAudioOutput::queue(std::span<const psx::SpuPcmFrame> frames) {
  if (frames.empty()) {
    return;
  }
  if (frames.size() >
      static_cast<std::size_t>(std::numeric_limits<ALsizei>::max()) /
          sizeof(psx::SpuPcmFrame)) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Gameplay audio block is too large"};
  }

  submitted_frames_ += frames.size();

  if (buffer_callback_ != nullptr) {
    compactStaging();
    const auto staged_count = staged_frames_.size() - staged_offset_;
    if (frames.size() >
        maximum_staged_frames - std::min(staged_count, maximum_staged_frames)) {
      throw core::Error{core::ErrorCode::io,
                        "Gameplay audio timeline exceeded its FIFO bound"};
    }
    staged_frames_.insert(staged_frames_.end(), frames.begin(), frames.end());
    fillCallbackRing();
    applyGainStep();
    startIfNeeded();
    return;
  }

  compactStaging();
  const auto staged_count = staged_frames_.size() - staged_offset_;
  if (frames.size() >
      maximum_staged_frames - std::min(staged_count, maximum_staged_frames)) {
    throw core::Error{core::ErrorCode::io,
                      "Gameplay audio timeline exceeded its FIFO bound"};
  }
  staged_frames_.insert(staged_frames_.end(), frames.begin(), frames.end());
  collectProcessed();
  uploadReadyBuffers(false);
  applyGainStep();
  startIfNeeded();
}

void PsyCrossAudioOutput::flush() {
  if (buffer_callback_ != nullptr) {
    fillCallbackRing();
    applyGainStep();
    startIfNeeded();
    return;
  }
  collectProcessed();
  uploadReadyBuffers(true);
  applyGainStep();
  startIfNeeded();
}

void PsyCrossAudioOutput::fillCallbackRing() {
  while (staged_offset_ < staged_frames_.size()) {
    const auto remaining = staged_frames_.size() - staged_offset_;
    const auto accepted = stream_frames_.push(
        std::span<const psx::SpuPcmFrame>{staged_frames_}.subspan(
            staged_offset_, remaining));
    if (accepted == 0U) {
      break;
    }
    staged_offset_ += accepted;
    uploaded_frames_ += accepted;
  }
  compactStaging();
}

void PsyCrossAudioOutput::uploadReadyBuffers(bool flush_partial) {
  while (queuedBufferCount() < maximum_queued_buffers) {
    const auto remaining = staged_frames_.size() - staged_offset_;
    if (remaining < frames_per_buffer && (!flush_partial || remaining == 0U)) {
      break;
    }
    const auto count = std::min(remaining, frames_per_buffer);
    uploadBuffer(std::span<const psx::SpuPcmFrame>{staged_frames_}.subspan(
        staged_offset_, count));
    staged_offset_ += count;
  }
  compactStaging();
}

void PsyCrossAudioOutput::uploadBuffer(
    std::span<const psx::SpuPcmFrame> frames) {

  ALuint buffer{};
  if (available_.empty()) {
    alGenBuffers(1, &buffer);
    try {
      requireAl("Cannot create gameplay audio buffer");
      buffers_.push_back(buffer);
    } catch (...) {
      if (buffer != 0U) {
        alDeleteBuffers(1, &buffer);
      }
      throw;
    }
  } else {
    buffer = available_.back();
    available_.pop_back();
  }

  auto upload_frames = frames;
  auto fade_remaining =
      fade_in_frames_remaining_.load(std::memory_order_relaxed);
  if (fade_remaining != 0U) {
    upload_scratch_.assign(frames.begin(), frames.end());
    for (auto &frame : upload_scratch_) {
      if (fade_remaining == 0U) {
        break;
      }
      const auto completed = restart_fade_frames - fade_remaining;
      const auto numerator = static_cast<std::int32_t>(completed + 1U);
      frame.left = static_cast<std::int16_t>(
          static_cast<std::int32_t>(frame.left) * numerator /
          static_cast<std::int32_t>(restart_fade_frames));
      frame.right = static_cast<std::int16_t>(
          static_cast<std::int32_t>(frame.right) * numerator /
          static_cast<std::int32_t>(restart_fade_frames));
      --fade_remaining;
    }
    fade_in_frames_remaining_.store(fade_remaining, std::memory_order_relaxed);
    upload_frames = std::span<const psx::SpuPcmFrame>{upload_scratch_};
  }

  const auto byte_count = static_cast<ALsizei>(upload_frames.size_bytes());
  alBufferData(buffer, AL_FORMAT_STEREO16, upload_frames.data(), byte_count,
               static_cast<ALsizei>(psx::Spu::sample_rate));
  requireAl("Cannot upload gameplay audio");
  alSourceQueueBuffers(source_, 1, &buffer);
  requireAl("Cannot queue gameplay audio");
  uploaded_frames_ += frames.size();
}

void PsyCrossAudioOutput::update() {
  if (buffer_callback_ != nullptr) {
    fillCallbackRing();
    applyGainStep();
    startIfNeeded();
    return;
  }
  collectProcessed();
  uploadReadyBuffers(false);
  applyGainStep();
  startIfNeeded();
}

ALsizei AL_APIENTRY PsyCrossAudioOutput::streamCallback(
    ALvoid *user, ALvoid *samples, ALsizei byte_count) noexcept {
  if (user == nullptr) {
    return 0;
  }
  return static_cast<PsyCrossAudioOutput *>(user)->fillStream(samples,
                                                              byte_count);
}

ALsizei PsyCrossAudioOutput::fillStream(ALvoid *samples,
                                        ALsizei byte_count) noexcept {
  if (samples == nullptr || byte_count <= 0) {
    return 0;
  }
  std::memset(samples, 0, static_cast<std::size_t>(byte_count));
  const auto requested =
      static_cast<std::size_t>(byte_count) / sizeof(psx::SpuPcmFrame);
  if (requested == 0U) {
    return byte_count;
  }
  auto destination = std::span<psx::SpuPcmFrame>{
      static_cast<psx::SpuPcmFrame *>(samples), requested};
  const auto recovering = callback_starved_.load(std::memory_order_relaxed);
  const auto recovered =
      !recovering || stream_frames_.size() >= minimum_start_frames_;
  if (recovering && recovered) {
    callback_starved_.store(false, std::memory_order_relaxed);
  }
  const auto supplied = recovered ? stream_frames_.pop(destination) : 0U;
  callback_frames_read_.fetch_add(supplied, std::memory_order_relaxed);

  if (supplied < requested) {
    callback_silence_frames_.fetch_add(requested - supplied,
                                       std::memory_order_relaxed);
    if (!callback_starved_.exchange(true, std::memory_order_relaxed)) {
      callback_underruns_.fetch_add(1U, std::memory_order_relaxed);
    }
  } else if (!recovering) {
    callback_starved_.store(false, std::memory_order_relaxed);
  }

  auto fade_remaining =
      fade_in_frames_remaining_.load(std::memory_order_relaxed);
  for (auto index = std::size_t{}; index < supplied && fade_remaining != 0U;
       ++index) {
    const auto completed = restart_fade_frames - fade_remaining;
    const auto numerator = static_cast<std::int32_t>(completed + 1U);
    destination[index].left = static_cast<std::int16_t>(
        static_cast<std::int32_t>(destination[index].left) * numerator /
        static_cast<std::int32_t>(restart_fade_frames));
    destination[index].right = static_cast<std::int16_t>(
        static_cast<std::int32_t>(destination[index].right) * numerator /
        static_cast<std::int32_t>(restart_fade_frames));
    --fade_remaining;
  }
  fade_in_frames_remaining_.store(fade_remaining, std::memory_order_relaxed);
  return byte_count;
}

void PsyCrossAudioOutput::setGainPercent(std::uint8_t percent) {
  gain_policy_.setTargetPercent(percent);
  applyGainStep();
}

void PsyCrossAudioOutput::logDiagnostics(
    std::string_view context) const noexcept {
  ALint state{AL_STOPPED};
  ALint queued{};
  ALint processed{};
  ALint sample_offset{};
  ALfloat second_offset{};
  ALfloat gain{};
  ALfloat pitch{};
  alGetError();
  alGetSourcei(source_, AL_SOURCE_STATE, &state);
  alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
  alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
  alGetSourcei(source_, AL_SAMPLE_OFFSET, &sample_offset);
  alGetSourcef(source_, AL_SEC_OFFSET, &second_offset);
  alGetSourcef(source_, AL_GAIN, &gain);
  alGetSourcef(source_, AL_PITCH, &pitch);
  const auto error = alGetError();
  const auto producer_staged = staged_frames_.size() >= staged_offset_
                                   ? staged_frames_.size() - staged_offset_
                                   : 0U;
  const auto staged =
      producer_staged +
      (buffer_callback_ != nullptr ? stream_frames_.size() : 0U);
  PsyX_Log_Info(
      "[AudioDiag][host] role=%s context=%.*s valid=%u al_error=%d "
      "sink=%s state=%s(%d) queued=%d processed=%d sample_offset=%d "
      "sec_offset=%.6f staged_frames=%zu buffers=%zu free=%zu "
      "prebuffer=%u gain=%.3f target_gain=%u pitch=%.3f submitted=%llu "
      "uploaded=%llu producer_staged=%zu recycled=%llu starts=%llu "
      "underruns=%llu "
      "resets=%llu callback_read=%llu callback_silence=%llu "
      "callback_starvations=%llu\n",
      diagnostic_name_.c_str(), static_cast<int>(context.size()),
      context.data(), error == AL_NO_ERROR ? 1U : 0U, static_cast<int>(error),
      buffer_callback_ != nullptr ? "callback" : "queued",
      sourceStateName(state), static_cast<int>(state), static_cast<int>(queued),
      static_cast<int>(processed), static_cast<int>(sample_offset),
      static_cast<double>(second_offset), staged, buffers_.size(),
      available_.size(), recycle_policy_.prebuffering() ? 1U : 0U,
      static_cast<double>(gain),
      static_cast<unsigned int>(gain_policy_.targetPercent()),
      static_cast<double>(pitch),
      static_cast<unsigned long long>(submitted_frames_),
      static_cast<unsigned long long>(uploaded_frames_), producer_staged,
      static_cast<unsigned long long>(recycled_buffers_),
      static_cast<unsigned long long>(source_starts_),
      static_cast<unsigned long long>(source_underruns_),
      static_cast<unsigned long long>(source_resets_),
      static_cast<unsigned long long>(
          callback_frames_read_.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          callback_silence_frames_.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          callback_underruns_.load(std::memory_order_relaxed)));
}

void PsyCrossAudioOutput::reset(std::string_view reason) noexcept {
  if (source_ == 0U) {
    return;
  }
  ++source_resets_;
  PsyX_Log_Info("[AudioDiag][reset] role=%s reason=%.*s sequence=%llu\n",
                diagnostic_name_.c_str(), static_cast<int>(reason.size()),
                reason.data(), static_cast<unsigned long long>(source_resets_));
  // Silence the device before detaching a live queue. Mission/checkpoint
  // restore is a real stream discontinuity; stopping a non-zero OpenAL sample
  // at full source gain is emitted as a loud click by several backends.
  alGetError();
  alSourcef(source_, AL_GAIN, 0.0F);
  alSourceStop(source_);
  if (buffer_callback_ != nullptr) {
    // alSourceStop synchronizes OpenAL Soft's callback before the producer
    // publishes the new empty generation. The callback buffer remains
    // attached and can be restarted without reallocating device objects.
    stream_frames_.clear();
    staged_frames_.clear();
    staged_offset_ = 0U;
    fade_in_frames_remaining_.store(restart_fade_frames,
                                    std::memory_order_relaxed);
    callback_starved_.store(false, std::memory_order_relaxed);
    start_policy_.reset();
    alGetError();
    return;
  }
  ALint queued{};
  alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
  while (queued-- > 0) {
    ALuint buffer{};
    alSourceUnqueueBuffers(source_, 1, &buffer);
    if (alGetError() != AL_NO_ERROR) {
      break;
    }
    if (available_.size() < available_.capacity()) {
      available_.push_back(buffer);
    }
  }
  staged_frames_.clear();
  staged_offset_ = 0U;
  fade_in_frames_remaining_.store(restart_fade_frames,
                                  std::memory_order_relaxed);
  underrun_latched_ = false;
  recycle_policy_.reset();
  start_policy_.reset();
  alGetError();
}

void PsyCrossAudioOutput::compactStaging() {
  if (staged_offset_ == 0U) {
    return;
  }
  if (staged_offset_ == staged_frames_.size()) {
    staged_frames_.clear();
    staged_offset_ = 0U;
    return;
  }
  if (staged_offset_ >= frames_per_buffer &&
      staged_offset_ * 2U >= staged_frames_.size()) {
    staged_frames_.erase(staged_frames_.begin(),
                         staged_frames_.begin() +
                             static_cast<std::ptrdiff_t>(staged_offset_));
    staged_offset_ = 0U;
  }
}

void PsyCrossAudioOutput::collectProcessed() {
  ALint processed{};
  ALint state{AL_STOPPED};
  alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
  alGetSourcei(source_, AL_SOURCE_STATE, &state);
  requireAl("Cannot query processed gameplay audio");
  if (!recycle_policy_.shouldRecycle(state == AL_PLAYING)) {
    // OpenAL defines every buffer attached to a stopped source as processed.
    // Recycling those freshly queued buffers on every producer call prevents
    // the recovery queue from ever reaching its start threshold. Preserve
    // them until alSourcePlay() turns the source live again.
    return;
  }
  recycleProcessedBuffers(processed);
}

void PsyCrossAudioOutput::recycleProcessedBuffers(ALint processed) {
  while (processed-- > 0) {
    ALuint buffer{};
    alSourceUnqueueBuffers(source_, 1, &buffer);
    requireAl("Cannot recycle gameplay audio buffer");
    if (available_.size() >= available_.capacity()) {
      throw core::Error{core::ErrorCode::io,
                        "Gameplay audio recycle queue exceeded its bound"};
    }
    ++recycled_buffers_;
    available_.push_back(buffer);
  }
}

void PsyCrossAudioOutput::applyGainStep() {
  ALint state{AL_STOPPED};
  alGetSourcei(source_, AL_SOURCE_STATE, &state);
  requireAl("Cannot query gameplay audio gain state");
  static_cast<void>(gain_policy_.advance(state == AL_PLAYING));
  alSourcef(source_, AL_GAIN, gain_policy_.gain());
  requireAl("Cannot update gameplay audio gain");
}

void PsyCrossAudioOutput::startIfNeeded() {
  ALint queued{};
  ALint processed{};
  ALint state{AL_STOPPED};
  if (buffer_callback_ != nullptr) {
    alGetSourcei(source_, AL_SOURCE_STATE, &state);
    requireAl("Cannot query callback gameplay audio state");
    const auto starvation_count =
        callback_underruns_.load(std::memory_order_relaxed);
    if (starvation_count != reported_callback_underruns_) {
      reported_callback_underruns_ = starvation_count;
      PsyX_Log_Warning(
          "[AudioDiag][starvation] role=%s sequence=%llu ring=%zu "
          "silence_frames=%llu; continuous source retained\n",
          diagnostic_name_.c_str(),
          static_cast<unsigned long long>(starvation_count),
          stream_frames_.size(),
          static_cast<unsigned long long>(
              callback_silence_frames_.load(std::memory_order_relaxed)));
    }
    if (state != AL_PLAYING && stream_frames_.size() >= minimum_start_frames_) {
      alSourcePlay(source_);
      requireAl("Cannot start callback gameplay audio");
      ++source_starts_;
      PsyX_Log_Info(
          "[AudioDiag][start] role=%s sequence=%llu ring=%zu sink=callback\n",
          diagnostic_name_.c_str(),
          static_cast<unsigned long long>(source_starts_),
          stream_frames_.size());
    }
    return;
  }
  alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
  alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
  alGetSourcei(source_, AL_SOURCE_STATE, &state);
  requireAl("Cannot query gameplay audio state");
  if (state == AL_PLAYING) {
    underrun_latched_ = false;
  } else if (stream_kind_ != PsyCrossAudioStreamKind::one_shot &&
             start_policy_.started() && !underrun_latched_) {
    underrun_latched_ = true;
    ++source_underruns_;
    PsyX_Log_Warning(
        "[AudioDiag][underrun] role=%s sequence=%llu state=%s queued=%d "
        "staged=%zu submitted=%llu uploaded=%llu\n",
        diagnostic_name_.c_str(),
        static_cast<unsigned long long>(source_underruns_),
        sourceStateName(state), static_cast<int>(queued),
        staged_frames_.size() - staged_offset_,
        static_cast<unsigned long long>(submitted_frames_),
        static_cast<unsigned long long>(uploaded_frames_));
  }

  // The source can consume its final queued buffer after collectProcessed()
  // but before this state query. Replaying a stopped source at that point
  // restarts every already-processed buffer from the beginning, which sounds
  // like a freeze and permanently holds fresh PCM in staging. Drain that
  // exhausted generation and replace it with current samples before restart.
  // If collectProcessed() has already entered recovery prebuffering, the
  // attached buffers are new and must not be mistaken for processed audio.
  if (start_policy_.started() && recycle_policy_.shouldDrainStoppedGeneration(
                                     state == AL_PLAYING, processed > 0)) {
    recycleProcessedBuffers(processed);
    recycle_policy_.reset();
    uploadReadyBuffers(false);
    alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
    alGetSourcei(source_, AL_SOURCE_STATE, &state);
    requireAl("Cannot refresh gameplay audio recovery state");
  }
  if (start_policy_.shouldStart(queued > 0 ? static_cast<std::size_t>(queued)
                                           : 0U,
                                state == AL_PLAYING)) {
    alSourcePlay(source_);
    requireAl("Cannot start gameplay audio");
    ++source_starts_;
    underrun_latched_ = false;
    recycle_policy_.playbackStarted();
    PsyX_Log_Info(
        "[AudioDiag][start] role=%s sequence=%llu queued=%d staged=%zu\n",
        diagnostic_name_.c_str(),
        static_cast<unsigned long long>(source_starts_),
        static_cast<int>(queued), staged_frames_.size() - staged_offset_);
  }
}

std::size_t PsyCrossAudioOutput::queuedBufferCount() const {
  ALint queued{};
  alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
  requireAl("Cannot query gameplay audio queue");
  return queued > 0 ? static_cast<std::size_t>(queued) : 0U;
}

PsyCrossUiAudio::PsyCrossUiAudio(const std::filesystem::path &cue_path) {
  auto disc = game::GameDisc::open(cue_path);
  const auto header = disc.image().readFile("COMMON/BEEPSX.VH");
  const auto body = disc.image().readFile("COMMON/BEEPSX.VB");
  constexpr std::array<std::size_t, 3U> retail_sound_ids{2U, 1U, 0U};
  for (std::size_t cue = 0U; cue < cues_.size(); ++cue) {
    auto decoded = psx::decodeVabSound(header, body, retail_sound_ids[cue]);
    if (!decoded.succeeded() || decoded.frames.empty()) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Cannot decode retail COMMON/BEEPSX menu cue " +
                            std::to_string(retail_sound_ids[cue])};
    }
    cues_[cue] = std::move(decoded.frames);
  }
  constexpr auto retail_voice_preview_sound_id = std::size_t{4U};
  auto preview =
      psx::decodeVabSound(header, body, retail_voice_preview_sound_id);
  if (!preview.succeeded() || preview.frames.empty()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Cannot decode retail OPTIONS voice preview"};
  }
  voice_preview_ = std::move(preview.frames);
}

std::span<const psx::SpuPcmFrame>
PsyCrossUiAudio::cueFrames(PsyCrossUiCue cue) const noexcept {
  const auto index = static_cast<std::size_t>(cue);
  return index < cues_.size() ? std::span<const psx::SpuPcmFrame>{cues_[index]}
                              : std::span<const psx::SpuPcmFrame>{};
}

void PsyCrossUiAudio::play(PsyCrossUiCue cue) {
  const auto frames = cueFrames(cue);
  if (frames.empty()) {
    return;
  }
  // UI feedback must be immediate; a fresh key-on replaces an unfinished
  // native cue instead of serialising it behind stale menu navigation.
  output_.reset("ui-retrigger");
  output_.queue(frames);
  output_.flush();
}

void PsyCrossUiAudio::playVoicePreview() {
  if (voice_preview_.empty()) {
    return;
  }
  // OPTIONS retriggers Gabe's authored test voice on every slider step.
  voice_preview_output_.reset("voice-preview-retrigger");
  voice_preview_output_.queue(voice_preview_);
  voice_preview_output_.flush();
}

} // namespace sf::platform::detail
