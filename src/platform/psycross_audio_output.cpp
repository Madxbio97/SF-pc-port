#include "psycross_audio_output.hpp"

#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/psx/vab_decoder.hpp"

#include <AL/alc.h>
#include <psx/libspu.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

namespace sf::platform::detail {
namespace {

std::size_t context_references{};
bool context_owned{};

void requireAl(const char *operation) {
  const auto error = alGetError();
  if (error != AL_NO_ERROR) {
    throw core::Error{core::ErrorCode::io, std::string{operation} +
                                               " (OpenAL error " +
                                               std::to_string(error) + ')'};
  }
}

} // namespace

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

PsyCrossAudioOutput::PsyCrossAudioOutput(std::size_t minimum_start_buffers)
    : start_policy_(std::clamp<std::size_t>(minimum_start_buffers, 1U,
                                            maximum_queued_buffers)) {
  static_assert(sizeof(psx::SpuPcmFrame) == 2U * sizeof(std::int16_t));
  static_assert(std::is_trivially_copyable_v<psx::SpuPcmFrame>);

  buffers_.reserve(maximum_queued_buffers);
  available_.reserve(maximum_queued_buffers);
  staged_frames_.reserve(maximum_staged_frames);

  alGetError();
  alGenSources(1, &source_);
  try {
    requireAl("Cannot create gameplay audio source");
    alSourcei(source_, AL_SOURCE_RELATIVE, AL_TRUE);
    requireAl("Cannot configure gameplay audio source");
    alSourcef(source_, AL_GAIN, gain_policy_.gain());
    requireAl("Cannot configure gameplay audio gain");
  } catch (...) {
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
  reset();
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

  compactStaging();
  if (frames.size() >= maximum_staged_frames) {
    staged_frames_.assign(
        frames.end() - static_cast<std::ptrdiff_t>(maximum_staged_frames),
        frames.end());
    staged_offset_ = 0U;
  } else {
    const auto staged_count = staged_frames_.size() - staged_offset_;
    if (staged_count + frames.size() > maximum_staged_frames) {
      staged_offset_ += staged_count + frames.size() - maximum_staged_frames;
      compactStaging();
    }
    staged_frames_.insert(staged_frames_.end(), frames.begin(), frames.end());
  }
  collectProcessed();
  uploadReadyBuffers(false);
  applyGainStep();
  startIfNeeded();
}

void PsyCrossAudioOutput::flush() {
  collectProcessed();
  uploadReadyBuffers(true);
  applyGainStep();
  startIfNeeded();
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

  const auto byte_count = static_cast<ALsizei>(frames.size_bytes());
  alBufferData(buffer, AL_FORMAT_STEREO16, frames.data(), byte_count,
               static_cast<ALsizei>(psx::Spu::sample_rate));
  requireAl("Cannot upload gameplay audio");
  alSourceQueueBuffers(source_, 1, &buffer);
  requireAl("Cannot queue gameplay audio");
}

void PsyCrossAudioOutput::update() {
  collectProcessed();
  uploadReadyBuffers(false);
  applyGainStep();
  startIfNeeded();
}

void PsyCrossAudioOutput::setGainPercent(std::uint8_t percent) {
  gain_policy_.setTargetPercent(percent);
  applyGainStep();
}

void PsyCrossAudioOutput::reset() noexcept {
  if (source_ == 0U) {
    return;
  }
  // Silence the device before detaching a live queue. Mission/checkpoint
  // restore is a real stream discontinuity; stopping a non-zero OpenAL sample
  // at full source gain is emitted as a loud click by several backends.
  alGetError();
  alSourcef(source_, AL_GAIN, 0.0F);
  alSourceStop(source_);
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
  alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
  requireAl("Cannot query processed gameplay audio");
  while (processed-- > 0) {
    ALuint buffer{};
    alSourceUnqueueBuffers(source_, 1, &buffer);
    requireAl("Cannot recycle gameplay audio buffer");
    if (available_.size() >= available_.capacity()) {
      throw core::Error{core::ErrorCode::io,
                        "Gameplay audio recycle queue exceeded its bound"};
    }
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
  ALint state{AL_STOPPED};
  alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
  alGetSourcei(source_, AL_SOURCE_STATE, &state);
  requireAl("Cannot query gameplay audio state");
  if (start_policy_.shouldStart(queued > 0 ? static_cast<std::size_t>(queued)
                                           : 0U,
                                state == AL_PLAYING)) {
    alSourcePlay(source_);
    requireAl("Cannot start gameplay audio");
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
  output_.reset();
  output_.queue(frames);
  output_.flush();
}

} // namespace sf::platform::detail
