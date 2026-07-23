#include "sf/assets/hmd_model.hpp"
#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr auto any_link = std::numeric_limits<std::int32_t>::min();

struct ActorSpec {
  std::uint32_t mission{};
  std::uint16_t source{};
  std::uint16_t class_id{};
  std::string_view model;
  std::int16_t maximum_health{};
  std::int32_t linked_source{any_link};
  bool dormant_at_start{};
};

// Exact authored identities which own campaign-specific AI, stealth, escort,
// boss, cure or failure logic. Generic soldiers are deliberately absent.
constexpr std::array actor_specs{
    ActorSpec{0U, 172U, 0x35U, "CHEMO.HMD", 150, -1},
    ActorSpec{0U, 173U, 0x35U, "CHEMO.HMD", 150, 29},
    ActorSpec{0U, 174U, 0x01U, "TERRO.HMD", 100, -1, true},
    ActorSpec{0U, 260U, 0x05U, "RADIO.TMD", 10, -1},
    ActorSpec{1U, 209U, 0x35U, "CHEMO.HMD", 350, 0},
    ActorSpec{2U, 13U, 0x02U, "MARA.HMD", 100},
    ActorSpec{3U, 7U, 0x35U, "CHEMO.HMD", 300, 8},
    ActorSpec{3U, 8U, 0x35U, "CHEMO.HMD", 300, -1},
    ActorSpec{3U, 16U, 0x35U, "CHEMO.HMD", 300, 0},
    ActorSpec{3U, 17U, 0x35U, "CHEMO.HMD", 300, 1},
    ActorSpec{3U, 18U, 0x35U, "CHEMO.HMD", 300, 2},
    ActorSpec{3U, 19U, 0x35U, "CHEMO.HMD", 300, 3},
    ActorSpec{3U, 48U, 0x01U, "LEADMAN.HMD", 100, -1, true},
    ActorSpec{4U, 9U, 0x3cU, "HANS.TMD", 100, -1},
    ActorSpec{5U, 108U, 0x01U, "BENTON.HMD", 100, 239, true},
    ActorSpec{5U, 373U, 0x4cU, "PHAGEN.HMD", 100, -1},
    ActorSpec{6U, 153U, 0x01U, "MARA.HMD", 100, 183},
    ActorSpec{6U, 183U, 0x4cU, "PHAGEN.HMD", 20, -1},
    ActorSpec{7U, 98U, 0x01U, "VLADI.HMD", 150, 81},
    ActorSpec{9U, 2U, 0x03U, "CHOPPER.TMD", 1000, -1},
    ActorSpec{11U, 73U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 76U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 216U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 217U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 218U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 219U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 220U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 221U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 222U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 223U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{11U, 224U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{11U, 225U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{11U, 226U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{11U, 227U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{11U, 228U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{11U, 229U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{12U, 50U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{12U, 52U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{12U, 55U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{12U, 214U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{12U, 215U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{12U, 216U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{12U, 217U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{12U, 218U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{12U, 219U, 0x65U, "SCIFO.HMD", 150},
    ActorSpec{12U, 220U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{12U, 221U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{12U, 222U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{12U, 223U, 0x5cU, "VIRO.HMD", 20},
    ActorSpec{13U, 96U, 0x65U, "SCIFO.HMD", 150, 5},
    ActorSpec{13U, 200U, 0x35U, "MEI.HMD", 500, -1, true},
    ActorSpec{13U, 201U, 0x4cU, "PHAGENSM.HMD", 500, 4, true},
    ActorSpec{14U, 181U, 0x5cU, "RICHARD.HMD", 100, 234},
    ActorSpec{17U, 200U, 0x01U, "SOELITE.HMD", 200, -1},
    ActorSpec{19U, 79U, 0x01U, "KANE.HMD", 200, -1},
};

struct TriggerSpec {
  std::uint32_t mission{};
  std::uint16_t source{};
  std::uint16_t class_id{};
  std::int32_t linked_source{any_link};
};

// Overlay-owned activation sources. Their callbacks, not a host visibility
// shortcut, decide when the linked special actor becomes live.
constexpr std::array trigger_specs{
    TriggerSpec{1U, 205U, 0x7bU, 62},   TriggerSpec{3U, 122U, 0x15U, 1},
    TriggerSpec{4U, 3U, 0x33U, 28},     TriggerSpec{5U, 239U, 0x1cU, 236},
    TriggerSpec{6U, 0U, 0x13U, 1},      TriggerSpec{7U, 81U, 0x1cU, 62},
    TriggerSpec{8U, 302U, 0x24U, 330},  TriggerSpec{9U, 5U, 0x28U, any_link},
    TriggerSpec{10U, 147U, 0x1dU, 148}, TriggerSpec{11U, 69U, 0x1cU, 65},
    TriggerSpec{12U, 53U, 0x1cU, 59},   TriggerSpec{13U, 7U, 0x22U, -1},
    TriggerSpec{13U, 95U, 0x22U, -1},   TriggerSpec{14U, 186U, 0x5fU, 181},
    TriggerSpec{14U, 234U, 0x22U, -1},  TriggerSpec{15U, 16U, 0x59U, -1},
    TriggerSpec{15U, 17U, 0x59U, -1},   TriggerSpec{15U, 18U, 0x59U, -1},
    TriggerSpec{16U, 279U, 0x1bU, 235}, TriggerSpec{17U, 208U, 0x1cU, 38},
    TriggerSpec{18U, 45U, 0x73U, -1},   TriggerSpec{18U, 46U, 0x73U, -1},
    TriggerSpec{19U, 138U, 0x24U, 49},
};

struct ActorSnapshot {
  std::uint32_t definition{};
  std::int16_t class_id{};
  std::int32_t linked_slot{};
  std::int16_t health{};
  std::uint32_t instance{};
  std::uint32_t display_node{};
  std::array<std::uint8_t, 4U> instance_state{};
  std::uint8_t bone_matrix_count{};
  bool resident{};
  bool simulated{};

  [[nodiscard]] friend bool operator==(const ActorSnapshot &,
                                       const ActorSnapshot &) = default;
};

ActorSnapshot snapshot(const sf::game::LegacyObjectBridgeState &actor) {
  return ActorSnapshot{
      .definition = actor.definition,
      .class_id = actor.class_id,
      .linked_slot = actor.linked_slot,
      .health = actor.health,
      .instance = actor.instance,
      .display_node = actor.display_node,
      .instance_state = actor.instance_state,
      .bone_matrix_count = actor.bone_matrix_count,
      .resident = actor.resident,
      .simulated = actor.simulated,
  };
}

std::span<const ActorSpec> missionActors(std::uint32_t mission) {
  const auto begin =
      std::ranges::find_if(actor_specs, [mission](const auto &spec) {
        return spec.mission == mission;
      });
  if (begin == actor_specs.end()) {
    return {};
  }
  const auto end =
      std::find_if(begin, actor_specs.end(), [mission](const auto &spec) {
        return spec.mission != mission;
      });
  return {begin, end};
}

std::span<const TriggerSpec> missionTriggers(std::uint32_t mission) {
  const auto begin =
      std::ranges::find_if(trigger_specs, [mission](const auto &spec) {
        return spec.mission == mission;
      });
  if (begin == trigger_specs.end()) {
    return {};
  }
  const auto end =
      std::find_if(begin, trigger_specs.end(), [mission](const auto &spec) {
        return spec.mission != mission;
      });
  return {begin, end};
}

std::optional<std::size_t> hmdPartCount(const sf::game::MissionPackage &package,
                                        const ActorSpec &spec) {
  if (!spec.model.ends_with(".HMD")) {
    return std::nullopt;
  }
  return sf::assets::HmdModel::parse(package.objectModels().file(spec.model))
      .parts()
      .size();
}

bool validatePackageActor(const sf::game::MissionPackage &package,
                          const ActorSpec &spec, std::string_view &failure) {
  const auto objects = package.objects().objects();
  if (spec.source >= objects.size()) {
    failure = "source is absent";
    return false;
  }
  const auto &object = objects[spec.source];
  const auto &definition = package.objects().definition(object.type);
  if (definition.class_id != spec.class_id ||
      definition.primary_model != spec.model ||
      object.maximum_health != spec.maximum_health ||
      (spec.linked_source != any_link &&
       object.linked_object != spec.linked_source)) {
    failure = "authored identity differs from the exact USA v1.1 matrix";
    return false;
  }
  return true;
}

bool validatePackageTrigger(const sf::game::MissionPackage &package,
                            const TriggerSpec &spec,
                            std::string_view &failure) {
  const auto objects = package.objects().objects();
  if (spec.source >= objects.size()) {
    failure = "activation source is absent";
    return false;
  }
  const auto &object = objects[spec.source];
  const auto &definition = package.objects().definition(object.type);
  if (definition.class_id != spec.class_id ||
      (spec.linked_source != any_link &&
       object.linked_object != spec.linked_source)) {
    failure = "activation source/link differs from the retail matrix";
    return false;
  }
  return true;
}

bool validateRuntimeActor(const sf::game::MissionPackage &package,
                          const sf::game::LegacyGameplayBridgeState &bridge,
                          const ActorSpec &spec, std::string_view &failure) {
  if (spec.source >= bridge.objects.size()) {
    failure = "guest object record is absent";
    return false;
  }
  const auto &authored = package.objects().objects()[spec.source];
  const auto &actor = bridge.objects[spec.source];
  if (actor.definition != authored.type || actor.class_id != spec.class_id ||
      (spec.linked_source != any_link &&
       actor.linked_slot != spec.linked_source)) {
    failure = "guest identity/link does not match DAT";
    return false;
  }
  if (spec.dormant_at_start &&
      (((actor.instance_state[3] & sf::game::legacy_instance_dormant) == 0U) ||
       actor.simulated || actor.display_node != 0U ||
       actor.bone_matrix_count != 0U)) {
    failure = "neutral guest ticks activated a dormant actor before its "
              "authored callback";
    return false;
  }
  const auto part_count = hmdPartCount(package, spec);
  if (part_count && actor.bone_matrix_count != 0U &&
      actor.bone_matrix_count != *part_count) {
    failure = "guest HMD pose has a partial bone set";
    return false;
  }
  if (part_count && actor.simulated && actor.display_node != 0U &&
      actor.presentation_enabled != 0U &&
      actor.bone_matrix_count != *part_count) {
    failure = "live special actor would enter native presentation in a T-pose";
    return false;
  }
  return true;
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "G3 special-actor probe requires USA v1.1"};
  }

  auto checked_actors = std::size_t{};
  auto checked_triggers = std::size_t{};
  auto full_pose_samples = std::size_t{};
  constexpr std::uint32_t neutral_updates = 12U;
  for (const auto &mission : sf::game::missionCatalog()) {
    const auto package = sf::game::MissionPackage::load(disc, mission.index);
    const auto actors = missionActors(mission.index);
    const auto triggers = missionTriggers(mission.index);
    auto failure = std::string_view{};
    for (const auto &spec : actors) {
      if (!validatePackageActor(package, spec, failure)) {
        std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                  << " source=" << spec.source << " " << failure << '\n';
        return 2;
      }
    }
    for (const auto &spec : triggers) {
      if (!validatePackageTrigger(package, spec, failure)) {
        std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                  << " source=" << spec.source << " " << failure << '\n';
        return 2;
      }
    }

    auto runtime = std::make_unique<sf::game::LegacyFirstMissionRuntime>(
        mission, package.legacyImage());
    const auto *initial = runtime->bridge();
    if (!runtime->ready() || runtime->faulted() || initial == nullptr ||
        !initial->terrain_triggers_enabled) {
      std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                << " retail trigger/runtime bootstrap is not ready\n";
      return 2;
    }
    std::vector<ActorSnapshot> checkpoint_states;
    checkpoint_states.reserve(actors.size());
    for (const auto &spec : actors) {
      if (!validateRuntimeActor(package, *initial, spec, failure)) {
        const auto &actual = initial->objects[spec.source];
        std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                  << " source=" << spec.source << " " << failure
                  << " actual(def/class/hp/link)=" << actual.definition << "/0x"
                  << std::hex << static_cast<std::uint16_t>(actual.class_id)
                  << std::dec << '/' << actual.maximum_health << '/'
                  << actual.linked_slot << " expected="
                  << package.objects().objects()[spec.source].type << "/0x"
                  << std::hex << spec.class_id << std::dec << '/'
                  << spec.maximum_health << '/' << spec.linked_source << '\n';
        return 2;
      }
      checkpoint_states.push_back(snapshot(initial->objects[spec.source]));
    }
    if (!runtime->captureCheckpoint()) {
      std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                << " checkpoint capture failed\n";
      return 2;
    }
    for (std::uint32_t update = 0U; update < neutral_updates; ++update) {
      runtime->setHostPadState({});
      runtime->advanceHostUpdate();
      const auto *bridge = runtime->bridge();
      if (runtime->faulted() || bridge == nullptr ||
          !bridge->terrain_triggers_enabled) {
        std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                  << " neutral retail tick faulted\n";
        return 2;
      }
      for (const auto &spec : actors) {
        if (!validateRuntimeActor(package, *bridge, spec, failure)) {
          std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                    << " source=" << spec.source << " " << failure << '\n';
          return 2;
        }
        full_pose_samples +=
            bridge->objects[spec.source].bone_matrix_count != 0U ? 1U : 0U;
      }
    }
    if (!runtime->restoreCheckpoint()) {
      std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                << " checkpoint restore failed\n";
      return 2;
    }
    const auto *restored = runtime->bridge();
    if (restored == nullptr || !restored->terrain_triggers_enabled) {
      std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                << " restored bridge is invalid\n";
      return 2;
    }
    for (std::size_t index = 0U; index < actors.size(); ++index) {
      if (snapshot(restored->objects[actors[index].source]) !=
          checkpoint_states[index]) {
        std::cerr << "G3 special-actor gate failed: mission=" << mission.index
                  << " source=" << actors[index].source
                  << " checkpoint changed actor lifecycle\n";
        return 2;
      }
    }
    checked_actors += actors.size();
    checked_triggers += triggers.size();
    std::cout << "mission=" << mission.index
              << " triggers=enabled special-actors=" << actors.size()
              << " activation-sources=" << triggers.size() << '\n';
  }

  if (checked_actors != actor_specs.size() ||
      checked_triggers != trigger_specs.size() || full_pose_samples == 0U) {
    std::cerr << "G3 special-actor gate failed: incomplete coverage actors="
              << checked_actors << '/' << actor_specs.size()
              << " triggers=" << checked_triggers << '/' << trigger_specs.size()
              << " pose-samples=" << full_pose_samples << '\n';
    return 2;
  }
  std::cout << "G3 special-actor gate passed: missions=20 actors="
            << checked_actors << " activation-sources=" << checked_triggers
            << " full-pose-samples=" << full_pose_samples << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: sf_g3_special_actor_probe <game.cue>\n";
    return 1;
  }
  try {
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "G3 special-actor gate failed: " << error.what() << '\n';
    return 10;
  }
}
