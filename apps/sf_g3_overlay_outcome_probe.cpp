#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/mission.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t bootstrap_budget = 500'000'000U;
constexpr std::uint64_t callback_budget = 5'000'000U;
constexpr std::uint32_t activate_dynamic_descriptor_entry = 0x8005fd04U;

struct CallbackStep {
  std::uint32_t entry{};
  std::uint16_t source{};
  std::uint16_t object_event{};
  bool damage_core{};
};

struct ActorExpectation {
  std::uint16_t source{};
  std::uint8_t set_instance_state_3{};
  std::uint8_t clear_instance_state_3{};
};

struct Scenario {
  std::uint32_t mission{};
  std::string_view name;
  std::vector<CallbackStep> callbacks;
  std::uint32_t completed_objectives{};
  std::uint32_t revealed_objectives{};
  std::optional<bool> failure;
  std::optional<bool> success;
  std::vector<ActorExpectation> actors;
};

struct ProgressState {
  std::uint32_t completed_objectives{};
  std::uint32_t failed_objectives{};
  std::uint32_t revealed_objectives{};
  std::uint32_t failed_parameters{};
  bool success{};
  bool terminal{};
  bool failure{};

  [[nodiscard]] friend bool operator==(const ProgressState &,
                                       const ProgressState &) = default;
};

struct ActorState {
  std::uint16_t source{};
  std::int16_t health{};
  std::uint8_t instance_flags{};
  std::array<std::uint8_t, 4U> instance_state{};

  [[nodiscard]] friend bool operator==(const ActorState &,
                                       const ActorState &) = default;
};

struct ScenarioResult {
  bool completed{};
  std::uint64_t instructions{};
  ProgressState baseline;
  ProgressState progress;
  std::vector<ActorState> actors;
  std::vector<std::byte> ram;

  [[nodiscard]] friend bool operator==(const ScenarioResult &,
                                       const ScenarioResult &) = default;
};

ProgressState progress(const sf::game::LegacyMissionBridgeState &state) {
  return ProgressState{
      .completed_objectives = state.completed_objectives,
      .failed_objectives = state.failed_objectives,
      .revealed_objectives = state.revealed_objectives,
      .failed_parameters = state.failed_parameters,
      .success = state.success,
      .terminal = state.terminal,
      .failure = state.failure,
  };
}

std::vector<Scenario> scenarios() {
  constexpr std::array m0_story{
      CallbackStep{0x80148168U, 174U},
      CallbackStep{0x80148168U, 260U},
  };
  constexpr std::array m11_scientists{
      CallbackStep{0x80146c10U, 73U},  CallbackStep{0x80146c10U, 76U},
      CallbackStep{0x80146c10U, 216U}, CallbackStep{0x80146c10U, 217U},
      CallbackStep{0x80146c10U, 218U}, CallbackStep{0x80146c10U, 219U},
      CallbackStep{0x80146c10U, 220U}, CallbackStep{0x80146c10U, 221U},
      CallbackStep{0x80146c10U, 222U}, CallbackStep{0x80146c10U, 223U},
  };
  constexpr std::array m11_cures{
      CallbackStep{0x80146e60U, 224U}, CallbackStep{0x80146e60U, 225U},
      CallbackStep{0x80146e60U, 226U}, CallbackStep{0x80146e60U, 227U},
      CallbackStep{0x80146e60U, 228U}, CallbackStep{0x80146e60U, 229U},
  };
  constexpr std::array m12_scientists{
      CallbackStep{0x80146c10U, 50U},  CallbackStep{0x80146c10U, 52U},
      CallbackStep{0x80146c10U, 55U},  CallbackStep{0x80146c10U, 214U},
      CallbackStep{0x80146c10U, 215U}, CallbackStep{0x80146c10U, 216U},
      CallbackStep{0x80146c10U, 217U}, CallbackStep{0x80146c10U, 218U},
      CallbackStep{0x80146c10U, 219U},
  };
  constexpr std::array m12_cures{
      CallbackStep{0x80146e60U, 220U},
      CallbackStep{0x80146e60U, 221U},
      CallbackStep{0x80146e60U, 222U},
      CallbackStep{0x80146e60U, 223U},
  };

  auto result = std::vector<Scenario>{
      Scenario{0U,
               "kravitch+radio-objective",
               {m0_story.begin(), m0_story.end()},
               0x01U},
      Scenario{2U,
               "aramov-death-success",
               {{0x80148618U, 13U}},
               0x01U,
               0U,
               false,
               true},
      Scenario{3U, "girdeux-death-objective", {{0x80146dacU, 48U}}, 0x08U},
      Scenario{4U,
               "hans-death-success",
               {{0x80146ac0U, 9U}},
               0x01U,
               0U,
               false,
               true},
      Scenario{5U,
               "phagan-protected-failure",
               {{0x80146c10U, 373U}},
               0U,
               0U,
               true,
               false},
      Scenario{6U,
               "phagan-protected-failure",
               {{0x80146a70U, 183U}},
               0U,
               0U,
               true,
               false},
      Scenario{6U,
               "mara-protected-failure",
               {{0x80146a70U, 153U}},
               0U,
               0U,
               true,
               false},
      Scenario{9U,
               "chopper-death-success",
               {{0U, 2U, 0U, true}, {0U, 2U, 0x0dU, false}},
               0x02U,
               0x02U,
               false,
               true,
               {{2U, 0U, 0U}}},
      Scenario{11U,
               "all-scientists-objective",
               {m11_scientists.begin(), m11_scientists.end()},
               0x01U},
      Scenario{11U,
               "all-viro-cures-objective",
               {m11_cures.begin(), m11_cures.end()},
               0x02U,
               0U,
               false,
               std::nullopt,
               {{224U, 0x08U, 0U},
                {225U, 0x08U, 0U},
                {226U, 0x08U, 0U},
                {227U, 0x08U, 0U},
                {228U, 0x08U, 0U},
                {229U, 0x08U, 0U}}},
      Scenario{11U,
               "viro-death-failure",
               {{0x80146c10U, 224U}},
               0U,
               0U,
               true,
               false},
      Scenario{12U,
               "all-scientists-objective",
               {m12_scientists.begin(), m12_scientists.end()},
               0x01U},
      Scenario{12U,
               "all-viro-cures-objective",
               {m12_cures.begin(), m12_cures.end()},
               0x02U,
               0U,
               false,
               std::nullopt,
               {{220U, 0x08U, 0U},
                {221U, 0x08U, 0U},
                {222U, 0x08U, 0U},
                {223U, 0x08U, 0U}}},
      Scenario{12U,
               "viro-death-failure",
               {{0x80146c10U, 220U}},
               0U,
               0U,
               true,
               false},
      Scenario{13U,
               "doctor-death-failure",
               {{0x80146ca4U, 96U}},
               0U,
               0U,
               true,
               false},
      Scenario{13U,
               "phagan-death-failure",
               {{0x80146ca4U, 201U}},
               0U,
               0U,
               true,
               false},
      Scenario{
          13U, "mei-death-failure", {{0x80146ca4U, 200U}}, 0U, 0U, true, false},
      Scenario{13U,
               "phagan-authored-activation",
               {{0x80147398U, 7U}},
               0x01U,
               0x06U,
               false,
               false,
               {{201U, 0U, 0x02U}}},
      Scenario{13U,
               "mei-authored-activation",
               {{0x80147398U, 95U}},
               0x02U,
               0U,
               false,
               false,
               {{200U, 0U, 0x02U}}},
      Scenario{14U,
               "richard-death-failure",
               {{0x80147220U, 181U}},
               0U,
               0U,
               true,
               false},
      Scenario{14U,
               "richard-authored-completion",
               {{0x80147708U, 186U}},
               0x01U,
               0U,
               false,
               false,
               {{181U, 0x08U, 0U}}},
  };
  return result;
}

bool prepareFirstFrame(sf::game::LegacyGameplayVm &vm,
                       std::uint32_t mission_index) {
  if (mission_index == 0U) {
    const auto activation = vm.invoke(activate_dynamic_descriptor_entry,
                                      std::array{6U}, callback_budget);
    if (!activation.completed()) {
      return false;
    }
  }
  if (!vm.writeHostPadState({})) {
    return false;
  }
  const auto frame = vm.tickRetailOuterFrame();
  return frame.completed() && vm.advanceAudioFrameClock();
}

ScenarioResult runScenario(sf::game::LegacyGameplayVm &vm,
                           const Scenario &scenario) {
  ScenarioResult result;
  const auto baseline = vm.readMissionBridgeState();
  if (!baseline) {
    return result;
  }
  result.baseline = progress(*baseline);
  result.completed = true;
  for (const auto &callback : scenario.callbacks) {
    const auto invocation = [&] {
      if (callback.damage_core) {
        const auto mission = vm.readMissionBridgeState();
        const auto bridge = vm.readBridgeState();
        if (!mission || !bridge || callback.source >= bridge->objects.size()) {
          return sf::game::LegacyGameplayVmResult{};
        }
        const auto &actor = bridge->objects[callback.source];
        std::uint16_t hit_part{1U};
        if (actor.health_controller != 0U) {
          static_cast<void>(
              vm.runtime().read16(actor.health_controller + 2U, hit_part));
        }
        constexpr std::uint32_t damage_core_entry = 0x80068770U;
        const std::array arguments{
            static_cast<std::uint32_t>(callback.source),
            0xffffffffU,
            static_cast<std::uint32_t>(mission->player_slot),
            0x7fffU,
            static_cast<std::uint32_t>(hit_part),
            0U,
            0x8011e670U,
            0x8011e670U,
        };
        return vm.invoke(damage_core_entry, arguments, callback_budget);
      }
      if (callback.object_event != 0U) {
        constexpr std::uint32_t event_address = 0x80117e60U;
        const auto bridge = vm.readBridgeState();
        if (!bridge || callback.source >= bridge->objects.size() ||
            bridge->objects[callback.source].object_handler == 0U) {
          return sf::game::LegacyGameplayVmResult{};
        }
        auto written = true;
        for (std::uint32_t offset = 0U; offset < 0x1cU; offset += 4U) {
          written = written && vm.runtime().write32(event_address + offset, 0U);
        }
        written = written &&
                  vm.runtime().write16(event_address, callback.object_event) &&
                  vm.runtime().write32(event_address + 4U, callback.source) &&
                  vm.runtime().write32(event_address + 8U, callback.source);
        if (!written) {
          return sf::game::LegacyGameplayVmResult{};
        }
        return vm.invoke(bridge->objects[callback.source].object_handler,
                         std::array{event_address}, callback_budget);
      }
      return vm.invoke(callback.entry,
                       std::array{std::uint32_t{callback.source}},
                       callback_budget);
    }();
    result.instructions += invocation.execution.instructions;
    if (!invocation.completed()) {
      result.completed = false;
      break;
    }
  }
  const auto mission = vm.readMissionBridgeState();
  const auto bridge = vm.readBridgeState();
  if (!mission || !bridge) {
    result.completed = false;
  } else {
    result.progress = progress(*mission);
    result.actors.reserve(scenario.actors.size());
    for (const auto &expected : scenario.actors) {
      if (expected.source >= bridge->objects.size()) {
        result.completed = false;
        break;
      }
      const auto &actor = bridge->objects[expected.source];
      result.actors.push_back(ActorState{
          .source = expected.source,
          .health = actor.health,
          .instance_flags = actor.instance_flags,
          .instance_state = actor.instance_state,
      });
    }
  }
  result.ram = vm.captureSnapshot().ram;
  return result;
}

bool matchesExpectation(const Scenario &scenario,
                        const ScenarioResult &result) {
  const auto completed_delta = result.progress.completed_objectives &
                               ~result.baseline.completed_objectives;
  const auto expected_terminal = (scenario.failure && *scenario.failure) ||
                                 (scenario.success && *scenario.success);
  if (!result.completed || completed_delta != scenario.completed_objectives ||
      (result.progress.revealed_objectives & scenario.revealed_objectives) !=
          scenario.revealed_objectives ||
      (scenario.failure && result.progress.failure != *scenario.failure) ||
      (scenario.success && result.progress.success != *scenario.success) ||
      ((scenario.failure || scenario.success) &&
       result.progress.terminal != expected_terminal) ||
      (result.progress.failure && result.progress.success)) {
    return false;
  }
  for (std::size_t index = 0U; index < scenario.actors.size(); ++index) {
    const auto &expected = scenario.actors[index];
    const auto &actual = result.actors[index];
    if ((actual.instance_state[3] & expected.set_instance_state_3) !=
            expected.set_instance_state_3 ||
        (actual.instance_state[3] & expected.clear_instance_state_3) != 0U) {
      return false;
    }
  }
  return true;
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "G3 overlay outcome probe requires USA v1.1"};
  }

  const auto cases = scenarios();
  auto passed = std::size_t{};
  for (const auto &mission : sf::game::missionCatalog()) {
    std::vector<const Scenario *> mission_cases;
    for (const auto &scenario : cases) {
      if (scenario.mission == mission.index) {
        mission_cases.push_back(&scenario);
      }
    }
    if (mission_cases.empty()) {
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
    if (!bootstrap.completed() || !prepareFirstFrame(vm, mission.index)) {
      std::cerr << "G3 overlay outcome gate failed: mission=" << mission.index
                << " bootstrap/first-frame\n";
      return 2;
    }
    const auto checkpoint = vm.captureSnapshot();
    for (const auto *scenario : mission_cases) {
      if (!vm.restoreSnapshot(checkpoint)) {
        std::cerr << "G3 overlay outcome gate failed: mission=" << mission.index
                  << " scenario=" << scenario->name << " restore\n";
        return 2;
      }
      const auto before = vm.readMissionBridgeState();
      if (!before || before->failure || before->success ||
          (before->completed_objectives & scenario->completed_objectives) !=
              0U) {
        std::cerr << "G3 overlay outcome gate failed: mission=" << mission.index
                  << " scenario=" << scenario->name << " invalid baseline\n";
        return 2;
      }
      const auto first = runScenario(vm, *scenario);
      const auto restored = vm.restoreSnapshot(checkpoint);
      const auto replay =
          restored ? runScenario(vm, *scenario) : ScenarioResult{};
      const auto exact = first == replay;
      const auto expected = matchesExpectation(*scenario, first);
      std::cout << "mission=" << mission.index << " scenario=" << scenario->name
                << " callbacks=" << scenario->callbacks.size()
                << " completed=" << first.completed << " exact-replay=" << exact
                << " expected=" << expected << " objectives=0x" << std::hex
                << first.progress.completed_objectives << "/revealed=0x"
                << first.progress.revealed_objectives << std::dec
                << " failure=" << first.progress.failure
                << " success=" << first.progress.success
                << " instructions=" << first.instructions << '\n';
      if (!exact || !expected) {
        for (const auto &actor : first.actors) {
          std::cerr << "  actor=" << actor.source << " health=" << actor.health
                    << " flags=0x" << std::hex
                    << static_cast<unsigned int>(actor.instance_flags)
                    << " state3=0x"
                    << static_cast<unsigned int>(actor.instance_state[3])
                    << std::dec << '\n';
        }
        std::cerr << "G3 overlay outcome gate failed: mission=" << mission.index
                  << " scenario=" << scenario->name << '\n';
        return 2;
      }
      ++passed;
    }
  }

  if (passed != cases.size()) {
    std::cerr << "G3 overlay outcome gate failed: coverage=" << passed << '/'
              << cases.size() << '\n';
    return 2;
  }
  std::cout << "G3 overlay outcome gate passed: scenarios=" << passed
            << " deterministic-authored-callback replay\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: sf_g3_overlay_outcome_probe <game.cue>\n";
    return 1;
  }
  try {
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "G3 overlay outcome gate failed: " << error.what() << '\n';
    return 10;
  }
}
