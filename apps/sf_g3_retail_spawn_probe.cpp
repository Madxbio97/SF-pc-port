#include "sf/assets/hmd_model.hpp"
#include "sf/core/error.hpp"
#include "sf/game/chase_camera.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace {

constexpr std::uint16_t kravitch_source = 174U;
constexpr std::uint16_t kravitch_room = 70U;
constexpr std::uint16_t kravitch_geometry_room = 71U;
constexpr std::int32_t kravitch_trigger_x_min = 1'325;
constexpr std::int32_t kravitch_trigger_x_max = 1'773;
constexpr std::int32_t kravitch_trigger_y_min = 2'141;
constexpr std::int32_t kravitch_trigger_y_max = 2'368;
constexpr std::int32_t kravitch_trigger_z_min = 6'180;
constexpr std::int32_t kravitch_trigger_z_max = 6'546;
constexpr std::uint32_t maximum_control_wait_updates = 2'000U;
constexpr std::uint32_t stable_control_updates = 8U;
constexpr std::uint32_t maximum_waypoint_updates = 900U;

struct Waypoint {
  std::string_view name;
  double x{};
  double z{};
  std::optional<std::uint32_t> entered_room;
  double radius{120.0};
  bool accept_room_entry{};
};

// Exact portal-side and destination-side positions recovered from SUBWAY.DAT.
// The probe reaches them through retail PAD locomotion; it never writes the
// player root, forces a room, or invokes a guest event directly.
constexpr std::array route_to_kravitch{
    Waypoint{"73-to-81 portal", 2'977.0, 4'495.0, 81U, 150.0, true},
    Waypoint{"room 81", 3'633.0, 3'836.0, 81U, 180.0, false},
    Waypoint{"81-to-82 portal", 2'425.0, 5'137.0, 82U, 150.0, true},
    Waypoint{"room 82", 3'318.0, 5'272.0, 82U, 180.0, false},
    Waypoint{"82-to-81 portal", 2'977.0, 4'495.0, 81U, 150.0, true},
    Waypoint{"81-to-67 portal", 1'766.0, 3'410.0, 67U, 150.0, true},
    Waypoint{"room 67 central aisle", 2'400.0, 4'050.0, 67U, 150.0, false},
    Waypoint{"room 67 west aisle", 2'100.0, 4'050.0, 67U, 120.0, false},
    Waypoint{"67-to-69 portal", 1'750.0, 4'050.0, 69U, 100.0, true},
    Waypoint{"room 69 south entry", 1'650.0, 4'050.0, 69U, 100.0, false},
    Waypoint{"room 69 north", 1'400.0, 5'154.0, 69U, 150.0, false},
    Waypoint{"69-to-68 portal", 1'133.0, 6'810.0, 68U, 150.0, true},
    Waypoint{"room 68", 1'449.0, 6'062.0, 68U, 180.0, false},
    Waypoint{"room 68 north aisle", 1'500.0, 6'800.0, 68U, 150.0, false},
    Waypoint{"room 70 east entry", 800.0, 6'800.0, kravitch_room, 170.0, true},
    Waypoint{"room 70 east aisle", 100.0, 6'800.0, kravitch_room, 170.0, false},
    Waypoint{"room 70 north entry", 300.0, 6'450.0, kravitch_room, 170.0, true},
    Waypoint{"68-to-70 portal", -302.0, 5'606.0, kravitch_room, 150.0, true},
    Waypoint{"70-to-71 portal", -650.0, 6'650.0, kravitch_geometry_room, 180.0,
             true},
    Waypoint{"Kravitch encounter", -1'100.0, 6'675.0, kravitch_geometry_room,
             180.0, false},
};

std::int32_t signedHeadingDelta(std::int32_t from, std::int32_t to) noexcept {
  auto delta = (to - from) % sf::game::heading_angle_units;
  if (delta > sf::game::heading_angle_units / 2) {
    delta -= sf::game::heading_angle_units;
  } else if (delta < -sf::game::heading_angle_units / 2) {
    delta += sf::game::heading_angle_units;
  }
  return delta;
}

bool controlReady(const sf::game::GameplaySession &gameplay) noexcept {
  const auto frame = gameplay.legacyPresentationFrame();
  if (!frame || !frame->renderer || !gameplay.legacyOpeningFinished()) {
    return false;
  }
  const auto &state = frame->renderer->state;
  return state.player.resident && !state.player.control_locked &&
         !state.camera.scripted && !state.camera.locked;
}

std::optional<std::uint16_t>
sceneForSource(const sf::game::GameplaySession &gameplay,
               std::uint16_t source) noexcept {
  const auto object =
      std::ranges::find_if(gameplay.objects(), [source](const auto &candidate) {
        return candidate.source_index == source;
      });
  if (object == gameplay.objects().end()) {
    return std::nullopt;
  }
  const auto index = static_cast<std::size_t>(
      std::distance(gameplay.objects().begin(), object));
  if (index > std::numeric_limits<std::uint16_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(index);
}

struct SpawnObservation {
  bool valid{true};
  bool appeared{};
  bool appeared_before_room{};
  bool incomplete_pose{};
  bool root_at_player{};
  bool terrain_triggers_enabled{};
  bool authored_dormancy_observed{};
  bool trigger_volume_entered{};
  bool dormancy_cleared_after_trigger{};
  std::uint32_t first_guest_slot{std::numeric_limits<std::uint32_t>::max()};
  std::uint64_t first_sequence{};
  std::size_t exact_pose_samples{};
  struct RetailTrace {
    std::int32_t scene_binding{-1};
    std::uint32_t instance{};
    std::uint32_t root_node{};
    std::uint32_t display_node{};
    std::array<std::uint8_t, 4U> instance_state{};
    std::uint32_t path_pointer{};
    std::uint32_t ai_controller{};
    std::uint32_t presentation_controller{};
    std::uint8_t presentation_enabled{};
    std::uint8_t presentation_mode{};
    std::int16_t health{};
    std::uint8_t bone_matrix_count{};
    bool resident{};
    bool simulated{};
    bool terrain_triggers_enabled{};

    [[nodiscard]] friend bool operator==(const RetailTrace &,
                                         const RetailTrace &) = default;
  };
  std::optional<RetailTrace> last_trace;
};

void traceKravitchState(const sf::game::GameplaySession &gameplay,
                        std::uint16_t scene, SpawnObservation &observation) {
  const auto frame = gameplay.legacyPresentationFrame();
  if (!frame || !frame->renderer || !frame->ui ||
      kravitch_source >= frame->renderer->state.objects.size()) {
    return;
  }
  const auto bindings = gameplay.legacyGuestSlotsBySceneObject();
  const auto &guest = frame->renderer->state.objects[kravitch_source];
  const auto trace = SpawnObservation::RetailTrace{
      .scene_binding = scene < bindings.size() ? bindings[scene] : -1,
      .instance = guest.instance,
      .root_node = guest.root_node,
      .display_node = guest.display_node,
      .instance_state = guest.instance_state,
      .path_pointer = guest.path_pointer,
      .ai_controller = guest.ai_controller,
      .presentation_controller = guest.presentation_controller,
      .presentation_enabled = guest.presentation_enabled,
      .presentation_mode = guest.presentation_mode,
      .health = guest.health,
      .bone_matrix_count = guest.bone_matrix_count,
      .resident = guest.resident,
      .simulated = guest.simulated,
      .terrain_triggers_enabled =
          frame->renderer->state.terrain_triggers_enabled,
  };
  if (observation.last_trace == trace) {
    return;
  }
  observation.last_trace = trace;
  std::cout << "kravitch-state seq=" << frame->sequence
            << " room=" << gameplay.currentRoom()
            << " bind=" << trace.scene_binding << " resident=" << trace.resident
            << " simulated=" << trace.simulated << " health=" << trace.health
            << " instance=0x" << std::hex << trace.instance << " root=0x"
            << trace.root_node << "->0x" << trace.display_node
            << " state=" << static_cast<unsigned int>(trace.instance_state[0])
            << ',' << static_cast<unsigned int>(trace.instance_state[1]) << ','
            << static_cast<unsigned int>(trace.instance_state[2]) << ','
            << static_cast<unsigned int>(trace.instance_state[3]) << " path=0x"
            << trace.path_pointer << " ai=0x" << trace.ai_controller
            << " present=0x" << trace.presentation_controller << std::dec << '/'
            << static_cast<unsigned int>(trace.presentation_enabled) << '/'
            << static_cast<unsigned int>(trace.presentation_mode)
            << " bones=" << static_cast<unsigned int>(trace.bone_matrix_count)
            << " terrain-triggers=" << trace.terrain_triggers_enabled << '\n';
}

bool observeKravitch(const sf::game::GameplaySession &gameplay,
                     std::uint16_t scene, SpawnObservation &observation,
                     std::string &failure) {
  if (gameplay.runtimeFaulted()) {
    failure = "guest renderer/UI bridge fault";
    observation.valid = false;
    return false;
  }
  const auto frame = gameplay.legacyPresentationFrame();
  if (!frame || !frame->renderer ||
      frame->sequence != gameplay.legacyPresentationSequence()) {
    failure = "production presentation frame is absent or incoherent";
    observation.valid = false;
    return false;
  }
  traceKravitchState(gameplay, scene, observation);
  const auto &renderer = frame->renderer->state;
  if (!renderer.terrain_triggers_enabled) {
    failure = "retail terrain-trigger latch dropped during the natural route";
    observation.valid = false;
    return false;
  }
  observation.terrain_triggers_enabled = true;
  const auto guest_y = -renderer.player.position.y;
  if (gameplay.currentRoom() == 68U &&
      renderer.player.position.x >= kravitch_trigger_x_min &&
      renderer.player.position.x <= kravitch_trigger_x_max &&
      guest_y >= kravitch_trigger_y_min && guest_y <= kravitch_trigger_y_max &&
      renderer.player.position.z >= kravitch_trigger_z_min &&
      renderer.player.position.z <= kravitch_trigger_z_max) {
    observation.trigger_volume_entered = true;
  }
  const auto &authored = renderer.objects[kravitch_source];
  if ((authored.instance_state[3] & sf::game::legacy_instance_dormant) != 0U) {
    observation.authored_dormancy_observed = true;
  } else if (observation.trigger_volume_entered) {
    observation.dormancy_cleared_after_trigger = true;
  }
  const auto bindings = gameplay.legacyGuestSlotsBySceneObject();
  if (scene >= bindings.size() || scene >= gameplay.objects().size()) {
    failure = "Kravitch presentation scene has no guest binding slot";
    observation.valid = false;
    return false;
  }
  const auto active = std::ranges::find(gameplay.activeObjects(), scene) !=
                      gameplay.activeObjects().end();
  const auto npc = gameplay.npcState(scene);
  if (active != (npc != nullptr)) {
    failure = "Kravitch active-object and NPC lifetimes diverged";
    observation.valid = false;
    return false;
  }
  if (!active) {
    return true;
  }

  observation.appeared = true;
  if (!observation.trigger_volume_entered) {
    observation.appeared_before_room = true;
    failure = "Kravitch appeared before Gabe entered the authored trigger";
    observation.valid = false;
    return false;
  }
  const auto encounter_streamed =
      std::ranges::any_of(gameplay.activeModels(), [](std::uint16_t room) {
        return room == kravitch_room || room == kravitch_geometry_room;
      });
  if (!encounter_streamed) {
    observation.appeared_before_room = true;
    failure = "Kravitch appeared before the retail DAT encounter envelope";
    observation.valid = false;
    return false;
  }
  const auto guest_slot = bindings[scene];
  if (guest_slot < 0 ||
      static_cast<std::size_t>(guest_slot) >= renderer.objects.size()) {
    failure = "visible Kravitch has no valid retail object owner";
    observation.valid = false;
    return false;
  }
  const auto &guest = renderer.objects[static_cast<std::size_t>(guest_slot)];
  const auto &native = gameplay.objects()[scene];
  if (native.model >= gameplay.objectModels().size()) {
    failure = "Kravitch presentation model index is invalid";
    observation.valid = false;
    return false;
  }
  const auto *hmd = std::get_if<sf::assets::HmdModel>(
      &gameplay.objectModels()[native.model].geometry);
  const auto exact_pose = hmd != nullptr &&
                          sf::game::legacyGuestHmdPoseComplete(
                              guest.bone_matrix_count, hmd->parts().size()) &&
                          native.legacy_hmd_bone_count >= hmd->parts().size() &&
                          !native.legacy_hmd_root_space;
  if (!exact_pose) {
    observation.incomplete_pose = true;
    failure =
        "Kravitch entered native presentation without an exact retail pose";
    observation.valid = false;
    return false;
  }
  const auto copied_pose =
      std::ranges::all_of(std::views::iota(std::size_t{}, hmd->parts().size()),
                          [&](std::size_t part) {
                            const auto &retail = guest.bone_matrices[part];
                            const auto &presented =
                                native.legacy_hmd_bones[part];
                            return presented.rotation == retail.rotation &&
                                   presented.x == retail.translation.x &&
                                   presented.y == -retail.translation.y &&
                                   presented.z == retail.translation.z;
                          });
  if (!copied_pose) {
    failure = "Kravitch native pose differs from the complete retail matrices";
    observation.valid = false;
    return false;
  }
  if (!guest.resident || !guest.simulated || guest.health <= 0 ||
      guest.object_handler != sf::game::legacy_common_npc_handler ||
      guest.ai_controller == 0U || native.transform.x != guest.position.x ||
      -native.transform.y != guest.position.y ||
      native.transform.z != guest.position.z) {
    failure = "Kravitch retail identity/transform is not production coherent";
    observation.valid = false;
    return false;
  }
  const auto distance =
      std::hypot(static_cast<double>(guest.position.x) - gameplay.player().x,
                 static_cast<double>(guest.position.z) - gameplay.player().z);
  if (!observation.first_sequence) {
    observation.first_sequence = frame->sequence;
    observation.first_guest_slot = guest.slot;
    observation.root_at_player = distance < 96.0;
    if (observation.root_at_player) {
      failure = "Kravitch materialized on Gabe's root";
      observation.valid = false;
      return false;
    }
  } else if (guest.slot != observation.first_guest_slot) {
    failure = "Kravitch guest identity changed during first presentation";
    observation.valid = false;
    return false;
  }
  ++observation.exact_pose_samples;
  return true;
}

bool step(sf::game::GameplaySession &gameplay,
          const sf::game::GameplayInput &input, std::uint16_t scene,
          SpawnObservation &observation, std::string &failure) {
  gameplay.update(input);
  if (!gameplay.advanceAudioFrameClock()) {
    failure = "production audio/hardware clock stopped";
    return false;
  }
  return observeKravitch(gameplay, scene, observation, failure);
}

bool reachWaypoint(sf::game::GameplaySession &gameplay,
                   const Waypoint &waypoint, std::uint16_t scene,
                   SpawnObservation &observation, std::string &failure,
                   std::uint32_t &total_updates, double turn_direction) {
  auto best_distance = std::numeric_limits<double>::infinity();
  auto stagnant_updates = std::uint32_t{};
  auto previous_room = gameplay.currentRoom();
  for (std::uint32_t update = 0U; update < maximum_waypoint_updates; ++update) {
    const auto &player = gameplay.player();
    const auto delta_x = waypoint.x - player.x;
    const auto delta_z = waypoint.z - player.z;
    const auto distance = std::hypot(delta_x, delta_z);
    const auto entered = waypoint.accept_room_entry && waypoint.entered_room &&
                         gameplay.currentRoom() == *waypoint.entered_room;
    const auto reached =
        waypoint.accept_room_entry ? entered : distance <= waypoint.radius;
    if (reached) {
      std::cout << "waypoint=" << waypoint.name
                << " result=reached room=" << gameplay.currentRoom()
                << " position=" << player.x << ',' << player.y << ','
                << player.z << " updates=" << update << '\n';
      return true;
    }

    if (distance + 12.0 < best_distance) {
      best_distance = distance;
      stagnant_updates = 0U;
    } else {
      ++stagnant_updates;
    }
    auto desired = sf::game::headingFromDirection(delta_x, delta_z);
    if (stagnant_updates > 60U) {
      constexpr std::array<std::int32_t, 4U> detour_offsets{768, -768, 1024,
                                                            -1024};
      const auto phase = static_cast<std::size_t>(
          ((stagnant_updates - 61U) / 90U) % detour_offsets.size());
      desired = sf::game::normalizeHeading(static_cast<std::int64_t>(desired) +
                                           detour_offsets[phase]);
    }
    const auto heading_delta = signedHeadingDelta(player.yaw, desired);
    auto input = sf::game::GameplayInput{};
    input.turn =
        turn_direction *
        std::clamp(static_cast<double>(heading_delta) / 384.0, -1.0, 1.0);
    input.move = std::abs(heading_delta) < 640 ? 1.0 : 0.0;
    input.run = true;
    // A physical Triangle pulse handles authored doors without bypassing the
    // guest event queue. The alternating steering offset above follows a wall
    // in both directions if the direct segment is blocked.
    input.interact = update % 32U == 4U;
    if (!step(gameplay, input, scene, observation, failure)) {
      return false;
    }
    ++total_updates;
    if (gameplay.currentRoom() != previous_room) {
      std::cout << "room-edge=" << previous_room << "->"
                << gameplay.currentRoom() << " waypoint=" << waypoint.name
                << '\n';
      previous_room = gameplay.currentRoom();
    }
  }
  const auto &player = gameplay.player();
  failure =
      "PAD route stalled at " + std::string{waypoint.name} +
      " room=" + std::to_string(gameplay.currentRoom()) +
      " position=" + std::to_string(player.x) + ',' + std::to_string(player.y) +
      ',' + std::to_string(player.z) + " yaw=" + std::to_string(player.yaw) +
      " desired=" +
      std::to_string(sf::game::headingFromDirection(waypoint.x - player.x,
                                                    waypoint.z - player.z)) +
      " distance=" +
      std::to_string(std::hypot(waypoint.x - player.x, waypoint.z - player.z));
  return false;
}

std::size_t
liveRetailHostileCount(const sf::game::GameplaySession &gameplay) noexcept {
  const auto frame = gameplay.legacyPresentationFrame();
  if (!frame || !frame->renderer) {
    return 0U;
  }
  return static_cast<std::size_t>(std::ranges::count_if(
      frame->renderer->state.objects, [](const auto &object) {
        return object.class_id == 1 && object.simulated && object.health > 0;
      }));
}

bool clearOpeningHostilesWithPad(sf::game::GameplaySession &gameplay,
                                 std::uint16_t scene,
                                 SpawnObservation &observation,
                                 std::string &failure,
                                 std::uint32_t &total_updates) {
  constexpr std::uint32_t maximum_combat_updates = 600U;
  constexpr std::uint32_t stable_clear_updates = 12U;
  auto clear_updates = std::uint32_t{};
  const auto initial_hostiles = liveRetailHostileCount(gameplay);
  for (std::uint32_t update = 0U; update < maximum_combat_updates; ++update) {
    const auto hostile_count = liveRetailHostileCount(gameplay);
    clear_updates = hostile_count == 0U ? clear_updates + 1U : 0U;
    if (clear_updates == stable_clear_updates) {
      std::cout << "opening-combat=cleared initial=" << initial_hostiles
                << " updates=" << update << '\n';
      return initial_hostiles != 0U;
    }
    const auto target_cycle = update % 72U;
    const auto input = sf::game::GameplayInput{
        .fire_pressed = target_cycle > 4U && update % 6U == 1U,
        .fire_held = target_cycle > 4U,
        .target_lock = target_cycle == 0U,
        .target_lock_held = target_cycle < 64U,
        .target_lock_released = target_cycle == 64U,
    };
    if (!step(gameplay, input, scene, observation, failure)) {
      return false;
    }
    ++total_updates;
  }
  failure = "physical PAD combat did not clear the opening hostiles";
  return false;
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "G3 retail spawn probe requires USA v1.1"};
  }
  const auto package = sf::game::MissionPackage::load(disc, 0U);
  auto gameplay = sf::game::GameplaySession{package};
  const auto scene = sceneForSource(gameplay, kravitch_source);
  if (!scene) {
    std::cerr << "G3 retail spawn gate failed: Kravitch source is absent\n";
    return 2;
  }

  auto observation = SpawnObservation{};
  auto failure = std::string{};
  if (!observeKravitch(gameplay, *scene, observation, failure)) {
    std::cerr << "G3 retail spawn gate failed: " << failure << '\n';
    return 2;
  }
  auto stable_control = std::uint32_t{};
  auto control_updates = std::uint32_t{};
  while (stable_control < stable_control_updates &&
         control_updates < maximum_control_wait_updates) {
    if (!step(gameplay, {}, *scene, observation, failure)) {
      std::cerr << "G3 retail spawn gate failed during opening: " << failure
                << '\n';
      return 2;
    }
    ++control_updates;
    stable_control = controlReady(gameplay) ? stable_control + 1U : 0U;
  }
  if (stable_control != stable_control_updates) {
    std::cerr << "G3 retail spawn gate failed: control did not release\n";
    return 2;
  }

  const auto calibration_yaw = gameplay.player().yaw;
  auto calibration_delta = std::int32_t{};
  for (std::uint32_t update = 0U; update < 8U && calibration_delta == 0;
       ++update) {
    if (!step(gameplay, sf::game::GameplayInput{.turn = 1.0}, *scene,
              observation, failure)) {
      std::cerr << "G3 retail spawn gate failed during turn calibration: "
                << failure << '\n';
      return 2;
    }
    ++control_updates;
    calibration_delta =
        signedHeadingDelta(calibration_yaw, gameplay.player().yaw);
  }
  if (calibration_delta == 0) {
    std::cerr << "G3 retail spawn gate failed: retail turn calibration did "
                 "not rotate Gabe\n";
    return 2;
  }
  const auto turn_direction = calibration_delta > 0 ? 1.0 : -1.0;
  std::cout << "turn-calibration=" << calibration_delta
            << " direction=" << turn_direction << '\n';

  auto route_updates = std::uint32_t{};
  if (!clearOpeningHostilesWithPad(gameplay, *scene, observation, failure,
                                   route_updates)) {
    std::cerr << "G3 retail spawn gate failed: " << failure << '\n';
    return 2;
  }
  for (const auto &waypoint : route_to_kravitch) {
    if (!reachWaypoint(gameplay, waypoint, *scene, observation, failure,
                       route_updates, turn_direction)) {
      std::cerr << "G3 retail spawn gate failed: " << failure << '\n';
      return 2;
    }
  }

  constexpr std::uint32_t encounter_settle_updates = 80U;
  for (std::uint32_t update = 0U; update < encounter_settle_updates; ++update) {
    const auto input = sf::game::GameplayInput{
        .fire_pressed = update % 6U == 1U,
        .fire_held = update < 48U,
        .target_lock = update == 0U,
        .target_lock_held = update < 48U,
    };
    if (!step(gameplay, input, *scene, observation, failure)) {
      std::cerr << "G3 retail spawn gate failed during encounter: " << failure
                << '\n';
      return 2;
    }
    ++route_updates;
  }
  if (!observation.terrain_triggers_enabled) {
    std::cerr << "G3 retail spawn gate failed: authored terrain triggers never "
                 "became enabled\n";
    return 2;
  }
  if (!observation.authored_dormancy_observed ||
      !observation.trigger_volume_entered ||
      !observation.dormancy_cleared_after_trigger) {
    std::cerr << "G3 retail spawn gate failed: Kravitch did not follow the "
                 "authored dormant-to-triggered lifecycle\n";
    return 2;
  }
  if (!observation.appeared || observation.exact_pose_samples < 2U) {
    std::cerr
        << "G3 retail spawn gate failed: Kravitch never produced a stable "
           "exact retail pose\n";
    return 2;
  }

  std::cout << "G3 retail spawn gate passed: control-updates="
            << control_updates << " route-updates=" << route_updates
            << " room=" << gameplay.currentRoom()
            << " first-sequence=" << observation.first_sequence
            << " guest-slot=" << observation.first_guest_slot
            << " exact-pose-samples=" << observation.exact_pose_samples << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: sf_g3_retail_spawn_probe <game.cue>\n";
    return 1;
  }
  try {
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "G3 retail spawn gate failed: " << error.what() << '\n';
    return 10;
  }
}
