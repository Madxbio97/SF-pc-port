#include "sf/core/error.hpp"
#include "sf/game/agent_mission_rules.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace {

constexpr std::uint16_t weapon_mask = 0x00ffU;
constexpr std::uint16_t grenade_mask = sf::game::agent_marcos_frag_grenade_bit |
                                       sf::game::agent_marcos_gas_grenade_bit;
constexpr std::uint16_t kravitch_agent_weapon = 7U;
constexpr std::uint16_t marcos_agent_weapon = 4U;
constexpr std::uint16_t gabrek_agent_weapon = 9U;
constexpr std::uint16_t chapel_agent_weapon = 7U;
constexpr std::uint16_t common_npc_class = 1U;
constexpr std::uint32_t common_npc_handler = 0x80061874U;

// USA v1.1 DAT-authored identities. Keep these local to the ROM gate:
// production matching remains owned by the legacy VM.
constexpr sf::game::AgentMissionNpcIdentity kravitch_identity{
    0U, 174U, 53U, 70, 0xc102U, -1487, 2140, 6675};
constexpr sf::game::AgentMissionNpcIdentity marcos_identity{
    3U, 48U, 11U, 63, 0x4104U, 5802, 0, 15845};
constexpr sf::game::LegacyNativePoint kravitch_live_bridge_position{
    -1495, -2140, 6679};
constexpr sf::game::LegacyNativePoint marcos_live_bridge_position{5825, 0,
                                                                  15855};
constexpr sf::game::LegacyNativePoint gabrek_live_bridge_position{-817, 0,
                                                                  -7044};

sf::game::LegacyNativePoint expectedBridgePosition(
    const sf::game::AgentMissionNpcIdentity &identity) noexcept {
  const auto exact_kravitch =
      identity.mission == kravitch_identity.mission &&
      identity.slot == kravitch_identity.slot &&
      identity.definition == kravitch_identity.definition &&
      identity.room == kravitch_identity.room &&
      identity.retail_attributes == kravitch_identity.retail_attributes &&
      identity.authored_x == kravitch_identity.authored_x &&
      identity.authored_y == kravitch_identity.authored_y &&
      identity.authored_z == kravitch_identity.authored_z;
  const auto exact_marcos =
      identity.mission == marcos_identity.mission &&
      identity.slot == marcos_identity.slot &&
      identity.definition == marcos_identity.definition &&
      identity.room == marcos_identity.room &&
      identity.retail_attributes == marcos_identity.retail_attributes &&
      identity.authored_x == marcos_identity.authored_x &&
      identity.authored_y == marcos_identity.authored_y &&
      identity.authored_z == marcos_identity.authored_z;
  const auto exact_gabrek =
      identity.mission == sf::game::agent_gabrek_identity.mission &&
      identity.slot == sf::game::agent_gabrek_identity.slot &&
      identity.definition == sf::game::agent_gabrek_identity.definition &&
      identity.room == sf::game::agent_gabrek_identity.room &&
      identity.retail_attributes ==
          sf::game::agent_gabrek_identity.retail_attributes &&
      identity.authored_x == sf::game::agent_gabrek_identity.authored_x &&
      identity.authored_y == sf::game::agent_gabrek_identity.authored_y &&
      identity.authored_z == sf::game::agent_gabrek_identity.authored_z;
  if (exact_kravitch) {
    return kravitch_live_bridge_position;
  }
  if (exact_marcos) {
    return marcos_live_bridge_position;
  }
  if (exact_gabrek) {
    return gabrek_live_bridge_position;
  }
  return sf::game::LegacyNativePoint{identity.authored_x, identity.authored_y,
                                     identity.authored_z};
}

bool exactChapelGuardIdentity(
    const sf::game::AgentMissionNpcIdentity &identity) noexcept {
  return std::ranges::any_of(
      sf::game::agent_chapel_guard_identities,
      [&identity](const sf::game::AgentMissionNpcIdentity &expected) {
        return identity.mission == expected.mission &&
               identity.slot == expected.slot &&
               identity.definition == expected.definition &&
               identity.room == expected.room &&
               identity.retail_attributes == expected.retail_attributes &&
               identity.authored_x == expected.authored_x &&
               identity.authored_y == expected.authored_y &&
               identity.authored_z == expected.authored_z;
      });
}

bool sameBridgePosition(
    const sf::game::LegacyNativePoint &position,
    const sf::game::AgentMissionNpcIdentity &identity) noexcept {
  const auto expected = expectedBridgePosition(identity);
  return position.x == expected.x && position.y == expected.y &&
         position.z == expected.z;
}

std::optional<std::uint16_t> expectedAgentAttributes(
    const sf::game::AgentMissionNpcIdentity &identity) noexcept {
  if (identity.mission == kravitch_identity.mission) {
    return sf::game::agentKravitchAttributes(identity.retail_attributes, true);
  }
  if (identity.mission == marcos_identity.mission) {
    return sf::game::agentMarcosAttributes(identity.retail_attributes, true);
  }
  if (identity.mission == sf::game::agent_gabrek_identity.mission) {
    return sf::game::agentGabrekAttributes(identity.retail_attributes, true);
  }
  if (identity.mission ==
      sf::game::agent_chapel_guard_identities.front().mission) {
    return sf::game::agentChapelGuardAttributes(
        identity.retail_attributes, identity.retail_attributes, true);
  }
  return std::nullopt;
}

bool fail(std::string_view stage,
          const sf::game::AgentMissionNpcIdentity &identity,
          std::string_view detail) {
  std::cerr << "Agent weapon gate failed: mission=" << identity.mission
            << " source=" << identity.slot << " stage=" << stage << ' '
            << detail << '\n';
  return false;
}

bool validatePackageIdentity(
    const sf::game::MissionPackage &package,
    const sf::game::AgentMissionNpcIdentity &identity) {
  const auto objects = package.objects().objects();
  if (identity.slot >= objects.size()) {
    return fail("DAT", identity, "object slot is absent");
  }
  const auto &object = objects[identity.slot];
  if (object.type != identity.definition) {
    return fail("DAT", identity, "definition differs from USA v1.1");
  }
  const auto &definition = package.objects().definition(object.type);
  if (definition.class_id != common_npc_class) {
    return fail("DAT", identity, "definition is not a common NPC");
  }
  if (object.transform.x != identity.authored_x ||
      object.transform.y != identity.authored_y ||
      object.transform.z != identity.authored_z) {
    return fail("DAT", identity, "authored position differs from USA v1.1");
  }
  if (object.attributes != identity.retail_attributes) {
    return fail("DAT", identity, "retail attributes differ from USA v1.1");
  }
  const auto rooms = package.objects().roomsContainingObject(identity.slot);
  if (std::ranges::find(rooms, static_cast<std::uint16_t>(identity.room)) ==
      rooms.end()) {
    return fail("DAT", identity, "authored room differs from USA v1.1");
  }
  return true;
}

bool validateBridgeIdentity(const sf::game::LegacyGameplayBridgeState &bridge,
                            const sf::game::AgentMissionNpcIdentity &identity,
                            std::uint16_t expected_attributes,
                            std::string_view stage) {
  if (identity.slot >= bridge.objects.size()) {
    return fail(stage, identity, "bridge object slot is absent");
  }
  const auto &actor = bridge.objects[identity.slot];
  const auto chapel_guard = exactChapelGuardIdentity(identity);
  const auto chapel_live_identity =
      !chapel_guard ||
      (identity.slot < bridge.dynamic_first_slot &&
       actor.object_handler == common_npc_handler &&
       actor.instance != 0U);
  if (actor.slot != identity.slot || actor.definition != identity.definition ||
      actor.class_id != common_npc_class ||
      (!chapel_guard && !sameBridgePosition(actor.authored_position, identity)) ||
      !chapel_live_identity) {
    const auto expected_position = expectedBridgePosition(identity);
    const auto recycled_slot = identity.slot >= bridge.dynamic_first_slot;
    const auto active_lifetime =
        !recycled_slot || actor.path_pointer != 0U;
    std::cerr << "Agent weapon gate failed: mission=" << identity.mission
              << " source=" << identity.slot << " stage=" << stage
              << " definition=" << actor.definition
              << " expected_definition=" << identity.definition
              << " class=" << actor.class_id
              << " expected_class=" << common_npc_class << " live_xyz=("
              << actor.authored_position.x << ',' << actor.authored_position.y
              << ',' << actor.authored_position.z << ") expected_live_xyz=("
              << expected_position.x << ',' << expected_position.y << ','
              << expected_position.z << ") dynamic_first="
              << bridge.dynamic_first_slot << " path=0x" << std::hex
              << actor.path_pointer << " handler=0x" << actor.object_handler
              << " instance=0x" << actor.instance << " attributes=0x"
              << actor.attributes << std::dec
              << " recycled=" << recycled_slot
              << " active_lifetime=" << active_lifetime
              << " resident=" << actor.resident
              << " simulated=" << actor.simulated
              << " health=" << actor.health << '\n';
    return false;
  }
  if (actor.attributes != expected_attributes) {
    std::cerr << "Agent weapon gate failed: mission=" << identity.mission
              << " source=" << identity.slot << " stage=" << stage
              << " attributes=0x" << std::hex << std::setw(4)
              << std::setfill('0') << actor.attributes << " expected=0x"
              << std::setw(4) << expected_attributes << std::dec
              << std::setfill(' ') << '\n';
    return false;
  }
  return true;
}

bool validateAgentWeapon(const sf::game::LegacyGameplayBridgeState &bridge,
                         const sf::game::AgentMissionNpcIdentity &identity) {
  const auto attributes = bridge.objects[identity.slot].attributes;
  if (identity.mission == kravitch_identity.mission) {
    if ((attributes & weapon_mask) != kravitch_agent_weapon) {
      return fail("Agent", identity, "Kravitch is not armed with shotgun");
    }
    return true;
  }
  if (identity.mission == marcos_identity.mission) {
    if ((attributes & weapon_mask) != marcos_agent_weapon ||
        (attributes & grenade_mask) !=
            sf::game::agent_marcos_frag_grenade_bit) {
      return fail("Agent", identity,
                  "Marcos is not .45 plus ordinary-frag only");
    }
    return true;
  }
  if (identity.mission == sf::game::agent_gabrek_identity.mission) {
    if ((attributes & weapon_mask) != gabrek_agent_weapon ||
        (attributes & grenade_mask) !=
            sf::game::agent_marcos_frag_grenade_bit) {
      return fail("Agent", identity,
                  "Gabrek is not M-16 plus ordinary-frag only");
    }
    return true;
  }
  if (identity.mission ==
      sf::game::agent_chapel_guard_identities.front().mission) {
    if ((attributes & weapon_mask) != chapel_agent_weapon) {
      return fail("Agent", identity, "chapel guard is not armed with shotgun");
    }
    return true;
  }
  return fail("Agent", identity, "unsupported mission policy");
}

bool republishFrame(sf::game::LegacyFirstMissionRuntime &runtime,
                    std::uint16_t mission, std::string_view stage) {
  const auto previous_frame = runtime.presentationFrame();
  const auto previous_guest_frame = runtime.guestFrame();
  runtime.setHostPadState({});
  runtime.advanceHostUpdate();
  if (runtime.faulted() || runtime.finished() || runtime.bridge() == nullptr ||
      runtime.guestFrame() <= previous_guest_frame ||
      runtime.presentationFrame() == previous_frame) {
    std::cerr << "Agent weapon gate failed: mission=" << mission
              << " stage=" << stage
              << " headless presentation re-publish failed"
              << " detail=" << runtime.faultDetail() << '\n';
    return false;
  }
  return true;
}

bool validateMission(
    sf::game::GameDisc &disc, std::uint16_t mission,
    std::span<const sf::game::AgentMissionNpcIdentity> identities) {
  const auto package = sf::game::MissionPackage::load(disc, mission);
  for (const auto &identity : identities) {
    if (identity.mission != mission ||
        !validatePackageIdentity(package, identity)) {
      return false;
    }
  }

  auto runtime = std::make_unique<sf::game::LegacyFirstMissionRuntime>(
      package.definition(), package.legacyImage());
  if (!runtime->ready() || runtime->faulted() || runtime->bridge() == nullptr) {
    std::cerr << "Agent weapon gate failed: mission=" << mission
              << " retail runtime bootstrap failed"
              << " detail=" << runtime->faultDetail() << '\n';
    return false;
  }
  auto retail_identities_valid = true;
  for (const auto &identity : identities) {
    if (!validateBridgeIdentity(*runtime->bridge(), identity,
                                identity.retail_attributes, "Retail")) {
      // CHURCH2 has three guards. Keep sampling so one ROM-gate run prints
      // every actual live tuple instead of stopping at the first mismatch.
      retail_identities_valid = false;
    }
  }
  if (!retail_identities_valid) {
    return false;
  }

  if (!runtime->setAgentDifficulty(true) ||
      !republishFrame(*runtime, mission, "Agent")) {
    return false;
  }
  for (const auto &identity : identities) {
    const auto expected = expectedAgentAttributes(identity);
    if (!expected.has_value()) {
      return fail("Agent", identity, "unsupported mission policy");
    }
    if (!validateBridgeIdentity(*runtime->bridge(), identity, *expected,
                                "Agent") ||
        !validateAgentWeapon(*runtime->bridge(), identity)) {
      return false;
    }
  }

  if (!runtime->setAgentDifficulty(false) ||
      !republishFrame(*runtime, mission, "Restored")) {
    return false;
  }
  for (const auto &identity : identities) {
    if (!validateBridgeIdentity(*runtime->bridge(), identity,
                                identity.retail_attributes, "Restored")) {
      return false;
    }
  }

  std::cout << "mission=" << mission << " actors=" << identities.size()
            << " retail-agent-retail=coherent\n";
  return true;
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "Agent weapon probe requires USA v1.1"};
  }

  const auto kravitch = std::span{&kravitch_identity, 1U};
  const auto marcos = std::span{&marcos_identity, 1U};
  const auto gabrek = std::span{&sf::game::agent_gabrek_identity, 1U};
  if (!validateMission(disc, kravitch_identity.mission, kravitch) ||
      !validateMission(disc, marcos_identity.mission, marcos) ||
      !validateMission(disc, sf::game::agent_gabrek_identity.mission, gabrek) ||
      !validateMission(disc,
                       sf::game::agent_chapel_guard_identities.front().mission,
                       std::span{sf::game::agent_chapel_guard_identities})) {
    return 2;
  }
  std::cout << "Agent weapon gate passed: missions=4 actors=6\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: sf_agent_mission_weapon_probe <game.cue>\n";
    return 1;
  }
  try {
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "Agent weapon gate failed: " << error.what() << '\n';
    return 10;
  }
}
