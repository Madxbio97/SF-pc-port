#include "sf/assets/fog_archive.hpp"
#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/legacy_mission_image.hpp"
#include "sf/game/mission.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<sf::psx::SpuPcmFrame> takeAllPcm(sf::game::LegacyGameplayVm &vm) {
  std::vector<sf::psx::SpuPcmFrame> result;
  std::array<sf::psx::SpuPcmFrame, 4096U> buffer{};
  for (;;) {
    const auto count = vm.takePcm(buffer);
    result.insert(result.end(), buffer.begin(), buffer.begin() + count);
    if (count != buffer.size()) {
      return result;
    }
  }
}

std::string readGuestString(const sf::psx::R3000Runtime &runtime,
                            std::uint32_t address) {
  std::string result;
  for (std::size_t index = 0; index < 128U; ++index) {
    std::uint8_t value{};
    if (!runtime.read8(address + static_cast<std::uint32_t>(index), value) ||
        value == 0U) {
      break;
    }
    if (value < 0x20U || value > 0x7eU) {
      return {};
    }
    result.push_back(static_cast<char>(value));
  }
  return result;
}

bool replayStateEqual(const sf::game::LegacyGameplayVmSnapshot &left,
                      const sf::game::LegacyGameplayVmSnapshot &right) {
  return left.cpu.gpr == right.cpu.gpr && left.cpu.hi == right.cpu.hi &&
         left.cpu.lo == right.cpu.lo && left.cpu.pc == right.cpu.pc &&
         left.cpu.next_pc == right.cpu.next_pc && left.ram == right.ram &&
         left.scratchpad == right.scratchpad && left.mmio == right.mmio;
}

void printFrameFailure(const sf::game::LegacyRetailOuterFrameResult &frame) {
  for (std::size_t index = 0; index < frame.guest_calls.size(); ++index) {
    const auto &call = frame.guest_calls[index];
    if (!call.completed()) {
      std::cerr << " call=" << index
                << " reason=" << sf::psx::toString(call.execution.reason)
                << " pc=0x" << std::hex << call.execution.pc << std::dec;
    }
  }
  if (frame.renderer_tail && !frame.renderer_tail->completed()) {
    std::cerr << " renderer-reason="
              << sf::psx::toString(frame.renderer_tail->execution.reason)
              << " renderer-pc=0x" << std::hex
              << frame.renderer_tail->execution.pc << std::dec;
  }
  if (!frame.platform_tail.delayed_callbacks.completed()) {
    std::cerr << " tail-reason="
              << sf::psx::toString(
                     frame.platform_tail.delayed_callbacks.execution.reason)
              << " tail-pc=0x" << std::hex
              << frame.platform_tail.delayed_callbacks.execution.pc << std::dec;
  }
  std::cerr << '\n';
}

bool validateRetailEnvironment(sf::game::LegacyGameplayVm &vm,
                               std::uint32_t mission_index,
                               std::string_view phase) {
  const auto bridge = vm.readBridgeState();
  const auto expected_clear = [&] {
    if (mission_index == 9U) {
      return sf::game::LegacyRgbBridgeState{0U, 0U, 2U};
    }
    if (mission_index == 13U) {
      return sf::game::LegacyRgbBridgeState{5U, 0U, 0U};
    }
    return sf::game::LegacyRgbBridgeState{};
  }();
  constexpr sf::game::LegacyRgbBridgeState expected_back{128U, 128U, 128U};
  if (bridge && bridge->environment.clear_color == expected_clear &&
      bridge->environment.back_color == expected_back &&
      bridge->environment.fog_color == expected_clear &&
      bridge->environment.fog_dqa == 0 && bridge->environment.fog_dqb == 0 &&
      !bridge->environment.fogEnabled()) {
    return true;
  }
  std::cerr << "mission=" << mission_index << " " << phase
            << " retail environment bridge mismatch\n";
  return false;
}

int runProbe(const std::filesystem::path &cue_path,
             std::optional<std::uint32_t> only_mission) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "H5 campaign probe requires Syphon Filter USA v1.1"};
  }

  std::uint64_t replay_frames{};
  std::uint64_t replay_samples{};
  std::uint64_t internal_levels{};
  constexpr std::uint64_t bootstrap_budget = 500'000'000U;
  for (const auto &mission : sf::game::missionCatalog()) {
    if (only_mission && mission.index != *only_mission) {
      continue;
    }
    const auto archive_path =
        "FOG/" + std::string{mission.resource_name} + ".FOG";
    if (mission.selection_index < 0) {
      ++internal_levels;
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " overlay=" << mission.overlay_name
                << " transition=internal\n";
      continue;
    }
    const auto package = sf::game::MissionPackage::load(disc, mission.index);
    const auto &image = package.legacyImage();
    auto virtual_cd = image.createVirtualCd();
    sf::game::LegacyGameplayVm vm{image.executable()};
    vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
    vm.bindSyphonFilterUsaV11VirtualCdCalls(virtual_cd);

    const auto bootstrap = vm.bootstrapMission(
        static_cast<std::uint32_t>(mission.selection_index),
        mission.index == 0U,
        sf::game::syphonFilterUsaV11FirstMissionBootstrapProfile(),
        sf::game::syphonFilterUsaV11RetailPlatformTailProfile(),
        sf::game::syphonFilterUsaV11FirstMissionOpeningProfile(),
        bootstrap_budget);
    if (!bootstrap.completed()) {
      std::cerr << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " phase=" << static_cast<unsigned int>(bootstrap.phase)
                << " reason="
                << sf::psx::toString(bootstrap.execution.execution.reason)
                << " pc=0x" << std::hex << bootstrap.execution.execution.pc
                << " ra=0x" << vm.runtime().state().gpr[31] << " sp=0x"
                << vm.runtime().state().gpr[29] << " a0=0x"
                << vm.runtime().state().gpr[4] << " a1=0x"
                << vm.runtime().state().gpr[5] << " a2=0x"
                << vm.runtime().state().gpr[6] << " a3=0x"
                << vm.runtime().state().gpr[7] << " s0=0x"
                << vm.runtime().state().gpr[16] << " s1=0x"
                << vm.runtime().state().gpr[17] << std::dec;
      if (const auto path =
              readGuestString(vm.runtime(), vm.runtime().state().gpr[4]);
          !path.empty()) {
        std::cerr << " path=" << path;
      }
      if (const auto mounted = virtual_cd->mountedArchive()) {
        std::cerr << " mounted=" << *mounted;
      }
      std::cerr << '\n';
      return 2;
    }
    if (!virtual_cd->mountedArchive() ||
        *virtual_cd->mountedArchive() != archive_path ||
        !vm.writeHostPadState({})) {
      std::cerr << "mission=" << mission.index
                << " virtual-CD/bootstrap state mismatch\n";
      return 3;
    }
    if (!validateRetailEnvironment(vm, mission.index, "bootstrap")) {
      return 16;
    }

    vm.clearPcm();
    const auto checkpoint = vm.captureSnapshot();
    if (!checkpoint.virtual_cd) {
      return 4;
    }
    const auto first_frame = vm.tickRetailOuterFrame();
    if (!first_frame.completed() || !vm.advanceAudioFrameClock()) {
      std::cerr << "mission=" << mission.index
                << " first frame failed: state=" << first_frame.state_before
                << '/' << first_frame.state_after
                << " unsupported=" << first_frame.unsupported_state
                << " bridge=" << first_frame.bridge_fault;
      printFrameFailure(first_frame);
      return 5;
    }
    const auto first_pcm = takeAllPcm(vm);
    const auto first_state = vm.captureSnapshot();

    if (!vm.restoreSnapshot(checkpoint)) {
      std::cerr << "mission=" << mission.index
                << " checkpoint restore failed: attached-text="
                << checkpoint.attached_text_sources.size()
                << " ui-messages=" << checkpoint.ui_messages.size()
                << " pending-drops=" << checkpoint.pending_actor_drops.size()
                << " audio-clock=" << checkpoint.audio_frame_tick << '/'
                << checkpoint.machine.scheduler.now
                << " audio-initialized="
                << checkpoint.audio_frame_tick_initialized
                << " ram=" << checkpoint.ram.size()
                << " virtual-cd=" << checkpoint.virtual_cd.has_value()
                << '\n';
      return 6;
    }
    if (!vm.writeHostPadState({})) {
      std::cerr << "mission=" << mission.index
                << " host pad restore failed\n";
      return 6;
    }
    const auto replay_frame = vm.tickRetailOuterFrame();
    if (!replay_frame.completed() || !vm.advanceAudioFrameClock()) {
      std::cerr << "mission=" << mission.index << " replay frame failed\n";
      return 7;
    }
    const auto replay_pcm = takeAllPcm(vm);
    const auto replay_state = vm.captureSnapshot();
    if (first_frame.state_before != replay_frame.state_before ||
        first_frame.state_after != replay_frame.state_after ||
        first_pcm != replay_pcm ||
        !replayStateEqual(first_state, replay_state)) {
      std::cerr << "mission=" << mission.index << " snapshot replay diverged\n";
      return 8;
    }
    if (!validateRetailEnvironment(vm, mission.index, "frame1")) {
      return 17;
    }

    const auto transition_checkpoint = vm.captureSnapshot();
    const auto bridge_profile =
        sf::game::syphonFilterUsaV11NativeMissionBridgeProfile();
    const auto outer_profile =
        sf::game::syphonFilterUsaV11RetailOuterFrameProfile();
    const auto seed_failure =
        vm.runtime().write32(outer_profile.current_state, 2U) &&
        vm.runtime().write8(bridge_profile.mission_terminal_latch, 1U) &&
        vm.runtime().write8(bridge_profile.mission_success_latch, 0U) &&
        vm.runtime().write8(bridge_profile.mission_failure_flag, 1U) &&
        vm.runtime().write8(bridge_profile.mission_completed_flag, 0U);
    const auto failure_bridge = vm.readMissionBridgeState(bridge_profile);
    const auto failure_decision =
        failure_bridge ? sf::game::classifyLegacyMissionTransition(
                             mission.index, 0U, 2U, *failure_bridge, false)
                       : sf::game::LegacyMissionTransitionDecision{};
    if (!seed_failure || !failure_bridge || !failure_bridge->failure ||
        !failure_decision.request_failure_restart ||
        !failure_decision.finished ||
        !vm.restoreSnapshot(transition_checkpoint)) {
      std::cerr << "mission=" << mission.index
                << " failure transition latch gate failed\n";
      return 9;
    }
    const auto seed_success =
        vm.runtime().write32(outer_profile.current_state, 3U) &&
        vm.runtime().write8(bridge_profile.mission_terminal_latch, 1U) &&
        vm.runtime().write8(bridge_profile.mission_success_latch, 1U) &&
        vm.runtime().write8(bridge_profile.mission_failure_flag, 0U) &&
        vm.runtime().write8(bridge_profile.mission_completed_flag, 1U);
    const auto success_bridge = vm.readMissionBridgeState(bridge_profile);
    const auto success_decision =
        success_bridge ? sf::game::classifyLegacyMissionTransition(
                             mission.index, 0U, 3U, *success_bridge, false)
                       : sf::game::LegacyMissionTransitionDecision{};
    if (!seed_success || !success_bridge || !success_bridge->success ||
        !success_decision.request_ending_movie || !success_decision.finished ||
        !vm.restoreSnapshot(transition_checkpoint)) {
      std::cerr << "mission=" << mission.index
                << " success/EOL transition latch gate failed\n";
      return 10;
    }

    auto runtime = std::make_unique<sf::game::LegacyFirstMissionRuntime>(
        package.definition(), image);
    const auto runtime_frame = runtime->presentationFrame();
    if (!runtime->ready() || runtime->faulted() || !runtime_frame ||
        !sf::game::legacyPresentationFrameConsumable(*runtime_frame, 0U) ||
        !runtime->captureCheckpoint()) {
      std::cerr << "mission=" << mission.index
                << " presentation runtime bootstrap failed\n";
      return 11;
    }
    runtime->setHostPadState({});
    runtime->advanceHostUpdate();
    const auto advanced_runtime_frame = runtime->presentationFrame();
    if (runtime->faulted() || !advanced_runtime_frame ||
        !sf::game::legacyPresentationFrameConsumable(*advanced_runtime_frame,
                                                     runtime_frame->sequence) ||
        !runtime->restoreCheckpoint()) {
      std::cerr << "mission=" << mission.index
                << " presentation runtime checkpoint failed\n";
      return 12;
    }
    const auto restored_runtime_frame = runtime->presentationFrame();
    if (!restored_runtime_frame ||
        !sf::game::legacyPresentationFrameConsumable(
            *restored_runtime_frame, advanced_runtime_frame->sequence)) {
      std::cerr << "mission=" << mission.index
                << " presentation runtime replay regressed\n";
      return 13;
    }
    auto gameplay = std::make_unique<sf::game::GameplaySession>(package);
    const auto gameplay_sequence = gameplay->legacyPresentationSequence();
    if (gameplay->runtimeFaulted() ||
        !gameplay->legacyRenderCommandsAuthoritative() ||
        gameplay_sequence == 0U) {
      std::cerr << "mission=" << mission.index
                << " production gameplay bootstrap failed\n";
      return 14;
    }
    gameplay->update({});
    if (gameplay->runtimeFaulted() ||
        gameplay->legacyPresentationSequence() <= gameplay_sequence) {
      std::cerr << "mission=" << mission.index
                << " production gameplay frame failed\n";
      return 15;
    }

    ++replay_frames;
    replay_samples += first_pcm.size();
    std::cout << "mission=" << mission.index
              << " resource=" << mission.resource_name
              << " overlay=" << mission.overlay_name
              << " state=" << first_frame.state_before << '/'
              << first_frame.state_after << " pcm=" << first_pcm.size() << '\n';
  }

  std::cout << "H5 campaign gate passed: missions=" << replay_frames
            << ", internal=" << internal_levels
            << ", replay-frames=" << replay_frames << ", pcm=" << replay_samples
            << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: sf_h5_campaign_probe <game.cue> [mission-index]\n";
    return 1;
  }
  try {
    auto only_mission = std::optional<std::uint32_t>{};
    if (argc == 3) {
      auto mission = std::uint32_t{};
      const auto text = std::string_view{argv[2]};
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), mission);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
          mission >= sf::game::missionCatalog().size()) {
        std::cerr << "Invalid mission index\n";
        return 1;
      }
      only_mission = mission;
    }
    return runProbe(std::filesystem::path{argv[1]}, only_mission);
  } catch (const std::exception &error) {
    std::cerr << "H5 campaign gate failed: " << error.what() << '\n';
    return 10;
  }
}
