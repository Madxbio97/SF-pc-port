#include "sf/core/error.hpp"
#include "sf/game/effects.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/mission.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

void printState(std::string_view phase,
                const sf::game::LegacyObjectBridgeState &object) {
  std::cout << phase << " slot=" << object.slot << " class=0x" << std::hex
            << static_cast<std::uint16_t>(object.class_id) << std::dec
            << " hp=" << object.health << '/' << object.maximum_health
            << " flags=0x" << std::hex
            << static_cast<unsigned int>(object.instance_flags) << std::dec
            << " state=" << static_cast<unsigned int>(object.instance_state[0])
            << '/' << static_cast<unsigned int>(object.instance_state[1]) << '/'
            << static_cast<unsigned int>(object.instance_state[2]) << '/'
            << static_cast<unsigned int>(object.instance_state[3])
            << " resident=" << object.resident
            << " simulated=" << object.simulated << " presented="
            << static_cast<unsigned int>(object.presentation_enabled) << '/'
            << static_cast<unsigned int>(object.presentation_mode) << '\n';
}

int run(const std::filesystem::path &cue, std::uint32_t mission_index) {
  auto disc = sf::game::GameDisc::open(cue);
  auto package = sf::game::MissionPackage::load(disc, mission_index);
  const auto &mission = package.definition();
  if (mission.selection_index < 0) {
    return 2;
  }
  auto virtual_cd = package.legacyImage().createVirtualCd();
  sf::game::LegacyGameplayVm vm{package.legacyImage().executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(virtual_cd);
  const auto bootstrap = vm.bootstrapMission(
      static_cast<std::uint32_t>(mission.selection_index), mission.index == 0U,
      sf::game::syphonFilterUsaV11FirstMissionBootstrapProfile(),
      sf::game::syphonFilterUsaV11RetailPlatformTailProfile(),
      sf::game::syphonFilterUsaV11FirstMissionOpeningProfile(), 500'000'000U);
  if (!bootstrap.completed() || !vm.writeHostPadState({})) {
    return 3;
  }
  for (unsigned int frame = 0; frame < 3U; ++frame) {
    if (!vm.tickNativeDrivenGameplayFrame().completed()) {
      return 4;
    }
  }
  const auto initial = vm.readBridgeState();
  const auto mission_state = vm.readMissionBridgeState();
  if (!initial || !mission_state || mission_state->player_slot < 0) {
    return 5;
  }
  const auto &objects = package.objects();
  for (std::size_t source = 0;
       source < objects.objects().size() && source < initial->objects.size();
       ++source) {
    const auto &record = objects.objects()[source];
    const auto &definition = objects.definition(record.type);
    const auto response = sf::game::objectDamageResponse(
        definition.class_id, definition.primary_model);
    const auto &before = initial->objects[source];
    if (response == sf::game::ObjectDamageResponse::none || !before.resident ||
        before.maximum_health <= 0) {
      continue;
    }
    std::cout << "model=" << definition.primary_model
              << " secondary=" << definition.secondary_model << ' ';
    printState("before", before);
    const auto snapshot = vm.captureSnapshot();
    const auto impact = vm.queueHostImpact(mission_state->player_slot,
                                           static_cast<std::int16_t>(source));
    const auto damage = vm.queueHostDamage(sf::game::LegacyHostDamageEvent{
        mission_state->player_slot,
        mission_state->player_slot,
        static_cast<std::int16_t>(source),
        std::numeric_limits<std::int16_t>::max(),
        0x0f,
    });
    auto ticked = impact.completed() && damage.completed();
    for (unsigned int frame = 0; frame < 3U && ticked; ++frame) {
      ticked = vm.tickNativeDrivenGameplayFrame().completed();
    }
    const auto after = ticked ? vm.readBridgeState() : std::nullopt;
    if (after && source < after->objects.size()) {
      printState("after", after->objects[source]);
    } else {
      std::cout << "after fault\n";
    }
    if (!vm.restoreSnapshot(snapshot) || !vm.writeHostPadState({})) {
      return 6;
    }
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "Usage: sf_retail_prop_state_probe <game.cue> <mission>\n";
    return 1;
  }
  try {
    return run(argv[1], static_cast<std::uint32_t>(std::stoul(argv[2])));
  } catch (const sf::core::Error &error) {
    std::cerr << error.what() << '\n';
    return 10;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 11;
  }
}
