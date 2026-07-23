#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/mission.hpp"
#include "sf/psx/spu.hpp"
#include "sf/psx/vab_decoder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <vector>

namespace {

struct AudioMetrics {
  std::uint64_t frames{};
  std::uint64_t nonzero_frames{};
  std::uint32_t peak{};
};

void measurePcm(std::span<const sf::psx::SpuPcmFrame> pcm,
                AudioMetrics &metrics) noexcept {
  metrics.frames += pcm.size();
  for (const auto &frame : pcm) {
    if (frame.left != 0 || frame.right != 0) {
      ++metrics.nonzero_frames;
    }
    const auto magnitude = [](std::int16_t sample) noexcept {
      const auto widened = static_cast<std::int32_t>(sample);
      return static_cast<std::uint32_t>(widened < 0 ? -widened : widened);
    };
    metrics.peak = std::max(
        metrics.peak, std::max(magnitude(frame.left), magnitude(frame.right)));
  }
}

std::uint64_t pcmHash(std::span<const sf::psx::SpuPcmFrame> frames) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto &frame : frames) {
    for (const auto sample : {frame.left, frame.right}) {
      const auto word = static_cast<std::uint16_t>(sample);
      hash ^= static_cast<std::uint8_t>(word);
      hash *= 1099511628211ULL;
      hash ^= static_cast<std::uint8_t>(word >> 8U);
      hash *= 1099511628211ULL;
    }
  }
  return hash;
}

bool verifyRetailMenuCues(sf::game::GameDisc &disc) {
  const auto header = disc.image().readFile("COMMON/BEEPSX.VH");
  const auto body = disc.image().readFile("COMMON/BEEPSX.VB");
  constexpr std::array<std::size_t, 3U> sound_ids{2U, 1U, 0U};
  constexpr std::array<std::uint16_t, 3U> programs{6U, 5U, 4U};
  constexpr std::array<std::uint16_t, 3U> samples{3U, 2U, 1U};
  constexpr std::array<std::uint16_t, 3U> pitches{0x0800U, 0x0800U, 0x1000U};
  constexpr std::array<std::uint64_t, 3U> expected_hashes{
      0x6fb98f4448ea13b3ULL,
      0x48f5f21505e87d93ULL,
      0x23c5fcc4532f66ffULL,
  };
  std::array<std::uint64_t, 3U> hashes{};
  for (std::size_t cue = 0U; cue < sound_ids.size(); ++cue) {
    const auto decoded = sf::psx::decodeVabSound(header, body, sound_ids[cue]);
    if (!decoded.succeeded() || decoded.frames.empty() ||
        decoded.vab_id != 5U || decoded.program != programs[cue] ||
        decoded.sample != samples[cue] || decoded.pitch != pitches[cue] ||
        decoded.volume_left != 10320U || decoded.volume_right != 10320U ||
        std::ranges::none_of(decoded.frames, [](const auto &frame) {
          return frame.left != 0 || frame.right != 0;
        })) {
      return false;
    }
    hashes[cue] = pcmHash(decoded.frames);
  }
  if (hashes != expected_hashes) {
    return false;
  }
  std::cout << "H3 retail menu cues: navigate=0x" << std::hex << hashes[0]
            << ", confirm=0x" << hashes[1] << ", cancel=0x" << hashes[2]
            << std::dec << '\n';
  return true;
}

bool verifyRetailVolumeRouting(const sf::game::LegacyMissionImage &image) {
  auto virtual_cd = image.createVirtualCd();
  sf::game::LegacyGameplayVm vm{image.executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(std::move(virtual_cd));
  const auto bootstrap = vm.bootstrapFirstMission();
  const auto activation = vm.invoke(0x8005fd04U, std::array{6U});
  if (!bootstrap.completed() || !activation.completed() ||
      !vm.writeHostPadState({})) {
    return false;
  }
  vm.clearPcm();
  if (!vm.tickRetailOuterFrame().completed() || !vm.advanceAudioFrameClock()) {
    return false;
  }

  const auto initial = vm.readRetailAudioVolumes();
  if (!initial) {
    std::cerr << "H3 volume initial unreadable\n";
    return false;
  }
  // The opening XA stream has not assigned its authored base volume yet.
  // Seed that exact retail route state so group 2's CD-input branch is
  // exercised independently of the later dialogue trigger.
  const auto gp = vm.runtime().state().gpr[28U];
  if (!vm.runtime().write16(gp + 0x7a0U, 100U) ||
      !vm.runtime().write16(gp + 0x7a2U, 0U)) {
    return false;
  }
  if (!vm.setRetailAudioVolumes(*initial)) {
    return false;
  }
  const auto cd_volume = [&vm] {
    const auto &registers = vm.machine().spu().state().registers;
    return std::array<std::uint16_t, 2U>{registers[0x1b0U / 2U],
                                         registers[0x1b2U / 2U]};
  };
  const auto initial_cd_volume = cd_volume();
  if (initial_cd_volume[0] == 0U || initial_cd_volume[1] == 0U) {
    std::cerr << "H3 volume initialized CD=0x" << std::hex
              << initial_cd_volume[0] << "/0x" << initial_cd_volume[1]
              << std::dec << '\n';
    return false;
  }

  const sf::game::LegacyRetailAudioVolumes no_effects{
      .sound_effects = 0U,
      .music = initial->music,
      .voice_over = initial->voice_over,
  };
  if (!vm.setRetailAudioVolumes(no_effects) ||
      vm.readRetailAudioVolumes() != no_effects ||
      cd_volume() != initial_cd_volume) {
    return false;
  }
  const sf::game::LegacyRetailAudioVolumes no_music{
      .sound_effects = initial->sound_effects,
      .music = 0U,
      .voice_over = initial->voice_over,
  };
  if (!vm.setRetailAudioVolumes(no_music) ||
      vm.readRetailAudioVolumes() != no_music ||
      cd_volume() != initial_cd_volume) {
    return false;
  }
  const sf::game::LegacyRetailAudioVolumes no_voice{
      .sound_effects = initial->sound_effects,
      .music = initial->music,
      .voice_over = 0U,
  };
  if (!vm.setRetailAudioVolumes(no_voice) ||
      vm.readRetailAudioVolumes() != no_voice ||
      cd_volume() != std::array<std::uint16_t, 2U>{0U, 0U} ||
      !vm.setRetailAudioVolumes(*initial)) {
    return false;
  }
  if (cd_volume() != initial_cd_volume) {
    return false;
  }

  const sf::game::LegacyRetailAudioVolumes invalid{
      .sound_effects = 128U,
      .music = 127U,
      .voice_over = 127U,
  };
  return !vm.setRetailAudioVolumes(invalid) &&
         vm.readRetailAudioVolumes() == initial;
}

void printSilentDiagnostics(const sf::game::LegacyMissionImage &image) {
  auto virtual_cd = image.createVirtualCd();
  sf::game::LegacyGameplayVm vm{image.executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(std::move(virtual_cd));
  const auto bootstrap = vm.bootstrapFirstMission();
  const auto activation = vm.invoke(0x8005fd04U, std::array{6U});
  if (!bootstrap.completed() || !activation.completed() ||
      !vm.writeHostPadState({})) {
    std::cerr << "H3 audio diagnostics could not reproduce bootstrap\n";
    return;
  }

  vm.clearPcm();
  auto maximum_active_voices = std::size_t{};
  for (std::uint32_t update = 0U; update <= 400U; ++update) {
    if (update != 0U && !vm.writeHostPadState({})) {
      break;
    }
    const auto frame = vm.tickRetailOuterFrame();
    if (!frame.completed()) {
      break;
    }
    if (!vm.advanceAudioFrameClock()) {
      break;
    }
    const auto &voices = vm.machine().spu().state().voices;
    maximum_active_voices = std::max(
        maximum_active_voices,
        static_cast<std::size_t>(std::ranges::count_if(
            voices, [](const auto &voice) { return voice.active != 0U; })));
  }

  const auto &spu = vm.machine().spu();
  const auto &state = spu.state();
  const auto nonzero_ram = std::ranges::count_if(
      state.ram, [](std::byte value) { return value != std::byte{}; });
  const auto active_voices = std::ranges::count_if(
      state.voices, [](const auto &voice) { return voice.active != 0U; });
  const auto reg = [&state](std::uint32_t offset) {
    return state.registers[offset / sizeof(std::uint16_t)];
  };
  const auto &dma = vm.machine().dma();
  std::cerr << "H3 silent diagnostics: control=0x" << std::hex << spu.control()
            << ", status=0x" << spu.status() << ", main-volume=0x"
            << reg(0x180U) << "/0x" << reg(0x182U) << ", key-on=0x"
            << reg(0x188U) << "/0x" << reg(0x18aU) << ", transfer=0x"
            << state.transfer_address << std::dec
            << ", ram-nonzero=" << nonzero_ram << ", active=" << active_voices
            << ", maximum-active=" << maximum_active_voices << ", dma4=0x"
            << std::hex << dma.madr(sf::psx::DmaChannel::spu) << "/0x"
            << dma.bcr(sf::psx::DmaChannel::spu) << "/0x"
            << dma.chcr(sf::psx::DmaChannel::spu) << std::dec << '\n';
}

int traceOpeningAudio(const sf::game::LegacyMissionImage &image) {
  auto virtual_cd = image.createVirtualCd();
  sf::game::LegacyGameplayVm vm{image.executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(std::move(virtual_cd));
  const auto bootstrap = vm.bootstrapFirstMission();
  const auto activation = vm.invoke(0x8005fd04U, std::array{6U});
  if (!bootstrap.completed() || !activation.completed() ||
      !vm.writeHostPadState({})) {
    std::cerr << "Opening audio trace bootstrap failed\n";
    return 11;
  }

  vm.clearPcm();
  std::uint32_t previous_active = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t previous_endx = std::numeric_limits<std::uint32_t>::max();
  std::int16_t previous_rail_health = std::numeric_limits<std::int16_t>::min();
  std::uint32_t previous_cd_lba = std::numeric_limits<std::uint32_t>::max();
  for (std::uint32_t update = 0U; update <= 400U; ++update) {
    const auto frame = vm.tickRetailOuterFrame();
    if (!frame.completed() || !vm.advanceAudioFrameClock()) {
      std::cerr << "Opening audio trace stopped at update " << update << '\n';
      return 12;
    }
    const auto bridge = vm.readBridgeState();
    if (!bridge || bridge->objects.size() <= 35U) {
      std::cerr << "Opening audio trace bridge failed\n";
      return 13;
    }
    const auto &spu = vm.machine().spu();
    const auto &state = spu.state();
    std::uint32_t active{};
    for (std::size_t voice = 0U; voice < state.voices.size(); ++voice) {
      if (state.voices[voice].active != 0U) {
        active |= static_cast<std::uint32_t>(1U << voice);
      }
    }
    const auto rail_health = bridge->objects[35U].health;
    const auto &cdrom = vm.machine().cdrom();
    std::uint32_t cd_active{};
    std::uint32_t ready_callback{};
    std::uint32_t xa_header{};
    std::uint32_t xa_base{};
    std::uint32_t stream_start{};
    static_cast<void>(vm.runtime().read32(0x8011642cU, cd_active));
    static_cast<void>(vm.runtime().read32(0x80114cc4U, ready_callback));
    static_cast<void>(vm.runtime().read32(0x801167f8U, xa_header));
    if (xa_header != 0U) {
      static_cast<void>(vm.runtime().read32(xa_header + 0x84U, xa_base));
    }
    static_cast<void>(vm.runtime().read32(0x80116878U, stream_start));
    if (update == 0U || update == 208U || update == 400U ||
        active != previous_active || state.endx != previous_endx ||
        rail_health != previous_rail_health ||
        cdrom.currentLba() != previous_cd_lba) {
      std::cout << "opening-audio: update=" << update << ", active=0x"
                << std::hex << active << ", endx=0x" << state.endx
                << ", control=0x" << spu.control() << ", pmon=0x"
                << state.registers[0x190U / 2U] << '/'
                << state.registers[0x192U / 2U] << ", noise=0x"
                << state.registers[0x194U / 2U] << '/'
                << state.registers[0x196U / 2U] << ", reverb=0x"
                << state.registers[0x198U / 2U] << '/'
                << state.registers[0x19aU / 2U] << std::dec
                << ", rail-health=" << rail_health
                << ", cd=" << cdrom.currentLba() << '/'
                << static_cast<unsigned int>(cdrom.mode()) << '/'
                << static_cast<unsigned int>(cdrom.filterFile()) << '/'
                << static_cast<unsigned int>(cdrom.filterChannel()) << '/'
                << cdrom.reading() << '/' << cd_active << "/0x" << std::hex
                << ready_callback << std::dec << ", xa=" << xa_header << '/'
                << xa_base << '/' << stream_start
                << ", cd-pcm=" << spu.queuedCdFrames() << ", voices=";
      for (std::size_t voice = 0U; voice < state.voices.size(); ++voice) {
        const auto &current = state.voices[voice];
        if (current.active == 0U) {
          continue;
        }
        std::cout << voice << ':' << current.block_address << '/'
                  << current.repeat_address << '/'
                  << static_cast<unsigned int>(current.block_flags) << '/'
                  << current.envelope << ',';
      }
      std::cout << '\n';
    }
    previous_active = active;
    previous_endx = state.endx;
    previous_rail_health = rail_health;
    previous_cd_lba = cdrom.currentLba();
    vm.clearPcm();
  }
  return 0;
}

void drainPcm(sf::game::LegacyFirstMissionRuntime &runtime,
              AudioMetrics &metrics) noexcept {
  std::array<sf::psx::SpuPcmFrame, 4096U> pcm{};
  while (const auto count = runtime.takePcm(pcm)) {
    measurePcm(std::span{pcm}.first(count), metrics);
  }
}

std::vector<sf::psx::SpuPcmFrame>
takeAllPcm(sf::game::LegacyFirstMissionRuntime &runtime) {
  std::vector<sf::psx::SpuPcmFrame> result;
  std::array<sf::psx::SpuPcmFrame, 4096U> pcm{};
  while (const auto count = runtime.takePcm(pcm)) {
    result.insert(result.end(), pcm.begin(), pcm.begin() + count);
  }
  return result;
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "H3 audio probe requires Syphon Filter USA v1.1"};
  }
  if (!verifyRetailMenuCues(disc)) {
    std::cerr << "H3 retail BEEPSX menu cue gate failed\n";
    return 11;
  }

  const auto mission = sf::game::MissionPackage::loadFirst(disc);
  if (!verifyRetailVolumeRouting(mission.legacyImage())) {
    std::cerr << "H3 retail volume routing gate failed\n";
    return 12;
  }
  auto runtime = std::make_unique<sf::game::LegacyFirstMissionRuntime>(
      mission.legacyImage());
  if (!runtime->ready() || runtime->faulted()) {
    std::cerr << "H3 audio bootstrap fault\n";
    return 2;
  }

  constexpr std::uint32_t sampled_updates = 400U;
  AudioMetrics metrics;
  const auto initial_boundary = takeAllPcm(*runtime);
  runtime->reset();
  const auto reset_boundary = takeAllPcm(*runtime);
  if (runtime->faulted() || initial_boundary.empty() ||
      reset_boundary != initial_boundary) {
    std::cerr << "H3 mission reset did not restore its initial PCM boundary\n";
    return 15;
  }

  // Briefing and pause screens deliberately advance only the retail
  // SPU/CD timer. Exercise that path long enough to cross normal XA sector
  // and stream boundaries without letting renderer/gameplay ticks hide an
  // interrupted audio clock.
  constexpr std::uint32_t audio_only_updates = 800U;
  AudioMetrics audio_only_metrics;
  for (std::uint32_t update = 0U; update < audio_only_updates; ++update) {
    if (!runtime->advanceAudioFrameClock()) {
      std::cerr << "H3 audio-only clock stopped at update " << update << '\n';
      return 16;
    }
    drainPcm(*runtime, audio_only_metrics);
  }
  constexpr auto pcm_frames_per_update =
      sf::psx::Spu::sample_rate /
      sf::game::LegacyGameplayVm::updates_per_second;
  const auto expected_audio_only_frames =
      static_cast<std::uint64_t>(audio_only_updates) * pcm_frames_per_update;
  if (runtime->faulted() ||
      audio_only_metrics.frames < expected_audio_only_frames ||
      audio_only_metrics.frames > expected_audio_only_frames + 1U ||
      audio_only_metrics.nonzero_frames == 0U) {
    std::cerr << "H3 audio-only clock lost PCM continuity: frames="
              << audio_only_metrics.frames
              << ", expected=" << expected_audio_only_frames
              << ", nonzero=" << audio_only_metrics.nonzero_frames
              << ", peak=" << audio_only_metrics.peak << '\n';
    return 17;
  }
  runtime->reset();
  if (runtime->faulted() || takeAllPcm(*runtime) != initial_boundary) {
    std::cerr << "H3 reset after audio-only playback diverged\n";
    return 18;
  }
  measurePcm(reset_boundary, metrics);
  for (std::uint32_t update = 0U; update < sampled_updates; ++update) {
    runtime->setHostPadState({});
    runtime->advanceHostUpdate();
    if (runtime->faulted() || runtime->finished()) {
      std::cerr << "H3 audio runtime stopped at update " << update << '\n';
      return 3;
    }
    drainPcm(*runtime, metrics);
  }

  constexpr auto expected_frames =
      static_cast<std::uint64_t>(sampled_updates + 1U) * pcm_frames_per_update;
  if (metrics.frames < expected_frames ||
      metrics.frames > expected_frames + pcm_frames_per_update) {
    std::cerr << "H3 audio clock cadence mismatch: " << metrics.frames
              << " frames, expected " << expected_frames << '\n';
    return 4;
  }
  if (metrics.nonzero_frames < 256U || metrics.peak < 2U) {
    std::cerr << "H3 audio gate observed only silent PCM\n";
    printSilentDiagnostics(mission.legacyImage());
    return 5;
  }
  if (!runtime->openingFinished()) {
    std::cerr << "H3 audio gate did not complete the opening XA dialogue\n";
    return 9;
  }

  if (!runtime->captureCheckpoint()) {
    std::cerr << "H3 audio checkpoint capture failed\n";
    return 6;
  }
  runtime->setHostPadState({});
  runtime->advanceHostUpdate();
  const auto expected_replay = takeAllPcm(*runtime);
  if (runtime->faulted() || !runtime->restoreCheckpoint()) {
    std::cerr << "H3 audio checkpoint restore failed\n";
    return 7;
  }
  runtime->setHostPadState({});
  runtime->advanceHostUpdate();
  const auto replayed = takeAllPcm(*runtime);
  if (runtime->faulted() || expected_replay.empty() ||
      expected_replay != replayed ||
      std::ranges::none_of(replayed, [](const auto &frame) {
        return frame.left != 0 || frame.right != 0;
      })) {
    std::cerr << "H3 audio checkpoint replay was not bit-exact and audible\n";
    return 8;
  }

  const sf::game::LegacyRetailAudioVolumes checkpoint_volumes{
      .sound_effects = 17U,
      .music = 43U,
      .voice_over = 71U,
  };
  const sf::game::LegacyRetailAudioVolumes current_host_volumes{
      .sound_effects = 23U,
      .music = 47U,
      .voice_over = 89U,
  };
  if (!runtime->setRetailAudioVolumes(checkpoint_volumes) ||
      !runtime->captureCheckpoint() ||
      !runtime->setRetailAudioVolumes(current_host_volumes) ||
      !runtime->restoreCheckpoint() ||
      runtime->retailAudioVolumes() != current_host_volumes) {
    std::cerr << "H3 host volume did not survive checkpoint restore\n";
    return 13;
  }
  runtime->reset();
  if (runtime->faulted() || !runtime->ready() ||
      runtime->retailAudioVolumes() != current_host_volumes) {
    std::cerr << "H3 host volume did not survive mission reset\n";
    return 14;
  }

  std::cout << "H3 audio gate passed: sampled-updates=" << sampled_updates
            << ", pcm-frames=" << metrics.frames
            << ", nonzero-frames=" << metrics.nonzero_frames
            << ", peak=" << metrics.peak
            << ", replay-frames=" << replayed.size() << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: sf_h3_audio_probe <game.cue> [--trace-opening]\n";
    return 1;
  }
  try {
    if (argc == 3) {
      auto disc = sf::game::GameDisc::open(std::filesystem::path{argv[1]});
      const auto mission = sf::game::MissionPackage::loadFirst(disc);
      return traceOpeningAudio(mission.legacyImage());
    }
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "H3 audio gate failed: " << error.what() << '\n';
    return 10;
  }
}
