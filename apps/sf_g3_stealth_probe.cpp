#include "sf/assets/hmd_model.hpp"
#include "sf/core/error.hpp"
#include "sf/game/chase_camera.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <bit>
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
#include <vector>

namespace {

constexpr std::uint32_t catacomb_mission = 13U;
constexpr std::uint16_t doctor_source = 96U;
constexpr std::uint16_t doctor_class = 0x65U;
constexpr std::uint16_t linked_source = 5U;
constexpr std::uint16_t linked_class = 0x6aU;
constexpr std::string_view doctor_model = "SCIFO.HMD";
constexpr std::int16_t doctor_health = 150;
constexpr std::array<std::uint16_t, 5U> authored_route{28U, 29U, 44U, 47U, 42U};

// CATACOMB.OVL registers this function with retail FUN_80066f40. The common
// AI calls it only on a transition to behavior mode 2. The callback compares
// a0 with the class-0x65 doctor slot, then runs CATACOMB's own radio/message
// path and mission-failure callback. This probe observes the boundary and
// retires its original first instruction; it never invokes the callback.
constexpr std::uint32_t doctor_detection_callback = 0x801470bcU;
constexpr std::uint32_t doctor_detection_first_instruction = 0x27bdffe8U;

constexpr std::uint64_t bootstrap_budget = 500'000'000U;
constexpr std::uint32_t maximum_control_wait_updates = 2'000U;
constexpr std::uint32_t stable_control_updates = 8U;
constexpr std::uint32_t maximum_materialization_updates = 600U;
constexpr std::uint32_t undetected_baseline_updates = 32U;
constexpr std::uint32_t maximum_follow_updates = 8'000U;
constexpr std::uint32_t maximum_exposure_updates = 2'400U;
constexpr double trailing_distance = 2'200.0;
constexpr double trailing_tolerance = 320.0;
constexpr double interaction_distance = 1'200.0;

struct Contract {
  std::uint8_t pose_parts{};
  sf::game::LegacyNativePoint linked_position;
};

struct DetectionObserver {
  std::uint32_t doctor_calls{};
  std::uint32_t other_calls{};
  std::int16_t last_argument{-1};

  void reset() noexcept {
    doctor_calls = 0U;
    other_calls = 0U;
    last_argument = -1;
  }

  void bind(sf::game::LegacyGameplayVm &vm) {
    vm.bindHostCall(
        doctor_detection_callback,
        [this](sf::game::LegacyHostCallContext &context) {
          const auto argument = static_cast<std::int16_t>(context.argument(0));
          last_argument = argument;
          if (argument == static_cast<std::int16_t>(doctor_source)) {
            ++doctor_calls;
          } else {
            ++other_calls;
          }
          context.continueGuestInstruction();
        });
  }
};

struct FrameSample {
  std::int32_t player_x{};
  std::int32_t player_y{};
  std::int32_t player_z{};
  std::int16_t player_room{-1};
  std::int16_t player_slot{-1};
  std::uint32_t completed_objectives{};
  std::uint32_t failed_objectives{};
  std::uint32_t revealed_objectives{};
  std::int16_t doctor_class_id{};
  std::int16_t doctor_health{};
  std::int32_t doctor_link{-1};
  std::uint32_t doctor_definition{};
  std::uint32_t doctor_instance{};
  std::uint32_t doctor_display{};
  std::uint32_t doctor_motion{};
  std::uint32_t doctor_target{};
  std::uint32_t doctor_ai{};
  std::uint32_t doctor_ai_flags{};
  std::uint16_t doctor_ai_state{};
  std::uint8_t doctor_ai_mode{};
  std::uint8_t doctor_route_node{};
  std::int16_t doctor_target_slot{-1};
  std::uint32_t doctor_target_flags{};
  std::int16_t doctor_target_meter{};
  std::uint32_t doctor_danger{};
  std::int32_t doctor_x{};
  std::int32_t doctor_y{};
  std::int32_t doctor_z{};
  std::int16_t doctor_forward_x{};
  std::int16_t doctor_forward_z{};
  std::uint64_t doctor_pose{};
  std::uint32_t detection_calls{};
  std::uint32_t other_detection_calls{};
  std::uint8_t doctor_bones{};
  bool doctor_resident{};
  bool doctor_simulated{};
  bool doctor_has_target{};
  bool doctor_destroyed{};
  bool mission_failure{};
  bool mission_terminal{};

  [[nodiscard]] friend bool operator==(const FrameSample &,
                                       const FrameSample &) noexcept = default;
};

struct ScenarioResult {
  bool completed{};
  std::string stop_phase;
  std::vector<sf::game::LegacyHostPadState> pads;
  std::vector<FrameSample> samples;
  sf::game::LegacyGameplayVmSnapshot final_snapshot;
  std::size_t route_index{};
  std::size_t room_edges{};
  std::size_t full_pose_samples{};
  std::size_t safe_follow_samples{};
  bool doctor_moved{};
  bool pose_transition{};
  bool ai_transition{};
  bool mode_two{};
  bool failure{};
  std::uint32_t doctor_detection_calls{};
  std::uint32_t other_detection_calls{};
};

void digestWord(std::uint64_t &digest, std::uint32_t value) noexcept {
  constexpr std::uint64_t prime = 1099511628211ULL;
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    digest ^= static_cast<std::uint8_t>(value >> shift);
    digest *= prime;
  }
}

std::uint64_t
poseDigest(const sf::game::LegacyObjectBridgeState &actor) noexcept {
  auto digest = std::uint64_t{1469598103934665603ULL};
  digestWord(digest, actor.bone_matrix_count);
  for (std::size_t part = 0U; part < actor.bone_matrix_count; ++part) {
    const auto &matrix = actor.bone_matrices[part];
    for (const auto component : matrix.rotation) {
      digestWord(digest, static_cast<std::uint16_t>(component));
    }
    digestWord(digest, std::bit_cast<std::uint32_t>(matrix.translation.x));
    digestWord(digest, std::bit_cast<std::uint32_t>(matrix.translation.y));
    digestWord(digest, std::bit_cast<std::uint32_t>(matrix.translation.z));
  }
  return digest;
}

bool delayedLoadEqual(const sf::psx::R3000DelayedLoadState &left,
                      const sf::psx::R3000DelayedLoadState &right) noexcept {
  return left.reg == right.reg && left.value == right.value &&
         left.valid == right.valid;
}

bool replayStateEqual(const sf::game::LegacyGameplayVmSnapshot &left,
                      const sf::game::LegacyGameplayVmSnapshot &right) {
  const auto cpu_equal =
      left.cpu.gpr == right.cpu.gpr &&
      left.cpu.gte.data == right.cpu.gte.data &&
      left.cpu.gte.control == right.cpu.gte.control &&
      left.cpu.cop0_status == right.cpu.cop0_status &&
      left.cpu.cop0_cause == right.cpu.cop0_cause &&
      left.cpu.cop0_epc == right.cpu.cop0_epc &&
      left.cpu.cop0_bad_vaddr == right.cpu.cop0_bad_vaddr &&
      left.cpu.hi == right.cpu.hi && left.cpu.lo == right.cpu.lo &&
      left.cpu.pc == right.cpu.pc && left.cpu.next_pc == right.cpu.next_pc &&
      left.cpu.branch_pc == right.cpu.branch_pc &&
      left.cpu.branch_delay_slot == right.cpu.branch_delay_slot &&
      delayedLoadEqual(left.cpu.load_delay, right.cpu.load_delay) &&
      delayedLoadEqual(left.cpu.next_load_delay, right.cpu.next_load_delay);
  if (!cpu_equal || left.ram != right.ram ||
      left.scratchpad != right.scratchpad || left.mmio != right.mmio ||
      left.machine.cpu_clock_scale != right.machine.cpu_clock_scale ||
      left.machine.scheduler.now != right.machine.scheduler.now ||
      left.machine.scheduler.next_token != right.machine.scheduler.next_token ||
      left.machine.scheduler.event_count !=
          right.machine.scheduler.event_count ||
      left.machine.pending_cpu_ticks != right.machine.pending_cpu_ticks ||
      left.machine.device_tick_remainder !=
          right.machine.device_tick_remainder) {
    return false;
  }
  for (std::size_t index = 0U; index < left.machine.scheduler.event_count;
       ++index) {
    const auto &a = left.machine.scheduler.events[index];
    const auto &b = right.machine.scheduler.events[index];
    if (a.deadline != b.deadline || a.token != b.token ||
        a.payload != b.payload || a.type != b.type || a.index != b.index) {
      return false;
    }
  }
  const auto spu_equal = left.machine.spu && right.machine.spu
                             ? *left.machine.spu == *right.machine.spu
                             : !left.machine.spu && !right.machine.spu;
  const auto virtual_cd_equal = left.virtual_cd && right.virtual_cd
                                    ? *left.virtual_cd == *right.virtual_cd
                                    : !left.virtual_cd && !right.virtual_cd;
  return left.machine.interrupts.status == right.machine.interrupts.status &&
         left.machine.interrupts.mask == right.machine.interrupts.mask &&
         left.machine.interrupts.input_lines ==
             right.machine.interrupts.input_lines &&
         left.machine.dma == right.machine.dma &&
         left.machine.cdrom == right.machine.cdrom && spu_equal &&
         left.machine.xa_decoder == right.machine.xa_decoder &&
         left.machine.timers == right.machine.timers &&
         left.video_timing_baseline == right.video_timing_baseline &&
         left.audio_frame_tick == right.audio_frame_tick &&
         left.interrupt_callbacks == right.interrupt_callbacks &&
         left.video_timing_baseline_initialized ==
             right.video_timing_baseline_initialized &&
         left.audio_frame_tick_initialized ==
             right.audio_frame_tick_initialized &&
         virtual_cd_equal;
}

std::optional<std::uint16_t> objectRoom(const sf::game::MissionPackage &package,
                                        std::uint16_t source) noexcept {
  for (std::size_t room = 0U; room < package.objects().roomCount(); ++room) {
    const auto members = package.objects().objectsInRoom(room);
    if (std::ranges::find(members, source) != members.end()) {
      if (room > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
      }
      return static_cast<std::uint16_t>(room);
    }
  }
  return std::nullopt;
}

bool adjacent(const sf::game::MissionPackage &package, std::uint16_t left,
              std::uint16_t right) {
  const auto contains = [](const auto &models, std::uint16_t room) {
    return std::ranges::find(models, room) != models.end();
  };
  return contains(package.layout().visibility(left).active_models, right) ||
         contains(package.layout().visibility(right).active_models, left);
}

std::optional<Contract>
validateContract(const sf::game::MissionPackage &package,
                 std::string &failure) {
  const auto &definition = package.definition();
  if (definition.index != catacomb_mission ||
      definition.resource_name != "CATACOMB" ||
      definition.overlay_name != "CATACOMB.OVL" ||
      definition.selection_index !=
          static_cast<std::int32_t>(catacomb_mission)) {
    failure = "mission-identity";
    return std::nullopt;
  }
  if (package.layout().initialRoom() != authored_route.front()) {
    failure = "initial-room";
    return std::nullopt;
  }
  for (std::size_t index = 1U; index < authored_route.size(); ++index) {
    if (authored_route[index] >= package.layout().modelCount() ||
        !adjacent(package, authored_route[index - 1U], authored_route[index])) {
      failure = "authored-route-adjacency";
      return std::nullopt;
    }
  }

  const auto objects = package.objects().objects();
  if (doctor_source >= objects.size() || linked_source >= objects.size()) {
    failure = "authored-sources";
    return std::nullopt;
  }
  const auto &doctor = objects[doctor_source];
  const auto &doctor_definition = package.objects().definition(doctor.type);
  const auto &linked = objects[linked_source];
  const auto &linked_definition = package.objects().definition(linked.type);
  if (doctor_definition.class_id != doctor_class ||
      doctor_definition.primary_model != doctor_model ||
      doctor.maximum_health != doctor_health ||
      doctor.linked_object != static_cast<std::int32_t>(linked_source) ||
      linked_definition.class_id != linked_class) {
    failure = "doctor-link-identity";
    return std::nullopt;
  }
  const auto doctor_room = objectRoom(package, doctor_source);
  const auto linked_room = objectRoom(package, linked_source);
  if (!doctor_room || !linked_room || *doctor_room != authored_route[1U] ||
      *linked_room != authored_route.back()) {
    failure = "doctor-link-route";
    return std::nullopt;
  }

  try {
    const auto parts =
        sf::assets::HmdModel::parse(
            package.objectModels().file(doctor_definition.primary_model))
            .parts()
            .size();
    if (parts == 0U || parts > sf::game::legacy_actor_bone_count ||
        parts > std::numeric_limits<std::uint8_t>::max()) {
      failure = "doctor-hmd-parts";
      return std::nullopt;
    }
    return Contract{
        static_cast<std::uint8_t>(parts),
        {linked.transform.x, -linked.transform.y, linked.transform.z},
    };
  } catch (...) {
    failure = "doctor-hmd-parse";
    return std::nullopt;
  }
}

std::optional<FrameSample> sampleFrame(sf::game::LegacyGameplayVm &vm,
                                       const DetectionObserver &observer) {
  const auto bridge = vm.readBridgeState();
  const auto mission = vm.readMissionBridgeState();
  if (!bridge || !mission || doctor_source >= bridge->objects.size()) {
    return std::nullopt;
  }
  const auto &doctor = bridge->objects[doctor_source];
  return FrameSample{
      bridge->player.position.x,
      bridge->player.position.y,
      bridge->player.position.z,
      bridge->player.room,
      mission->player_slot,
      mission->completed_objectives,
      mission->failed_objectives,
      mission->revealed_objectives,
      doctor.class_id,
      doctor.health,
      doctor.linked_slot,
      doctor.definition,
      doctor.instance,
      doctor.display_node,
      doctor.motion_controller,
      doctor.target_controller,
      doctor.ai_controller,
      doctor.ai_flags,
      doctor.ai_state,
      doctor.ai_mode,
      doctor.ai_route_node,
      doctor.target_slot,
      doctor.target_flags,
      doctor.target_meter,
      doctor.danger_q12,
      doctor.position.x,
      doctor.position.y,
      doctor.position.z,
      doctor.guest_rotation[2],
      doctor.guest_rotation[8],
      poseDigest(doctor),
      observer.doctor_calls,
      observer.other_calls,
      doctor.bone_matrix_count,
      doctor.resident,
      doctor.simulated,
      doctor.has_target,
      doctor.destroyed(),
      mission->failure,
      mission->terminal,
  };
}

bool doctorReady(const FrameSample &sample, const Contract &contract,
                 std::uint32_t authored_definition) noexcept {
  return sample.doctor_class_id == static_cast<std::int16_t>(doctor_class) &&
         sample.doctor_definition == authored_definition &&
         sample.doctor_link == static_cast<std::int32_t>(linked_source) &&
         sample.doctor_health > 0 && !sample.doctor_destroyed &&
         sample.doctor_instance != 0U && sample.doctor_display != 0U &&
         sample.doctor_motion != 0U && sample.doctor_target != 0U &&
         sample.doctor_ai != 0U && sample.doctor_resident &&
         sample.doctor_simulated && sample.doctor_bones == contract.pose_parts;
}

double distance2d(std::int32_t first_x, std::int32_t first_z,
                  std::int32_t second_x, std::int32_t second_z) noexcept {
  return std::hypot(static_cast<double>(first_x) - second_x,
                    static_cast<double>(first_z) - second_z);
}

std::int32_t signedHeadingDelta(std::int32_t desired,
                                std::int32_t current) noexcept {
  auto delta =
      sf::game::normalizeHeading(static_cast<std::int64_t>(desired) - current);
  if (delta > sf::game::heading_angle_units / 2) {
    delta -= sf::game::heading_angle_units;
  }
  return delta;
}

sf::game::LegacyHostPadState
steerFromBridge(const sf::game::LegacyGameplayBridgeState &bridge,
                double target_x, double target_z, bool interact,
                double turn_direction,
                std::int32_t heading_offset = 0) noexcept {
  constexpr std::int32_t turn_dead_zone = 72;
  constexpr std::int32_t forward_cone = 620;
  sf::game::LegacyHostPadState pad;
  const auto delta_x = target_x - bridge.player.position.x;
  const auto delta_z = target_z - bridge.player.position.z;
  const auto desired = sf::game::normalizeHeading(
      static_cast<std::int64_t>(
          sf::game::headingFromDirection(delta_x, delta_z)) +
      heading_offset);
  const auto current = sf::game::headingFromDirection(
      static_cast<double>(bridge.player.guest_rotation[2]),
      static_cast<double>(bridge.player.guest_rotation[8]));
  const auto delta = signedHeadingDelta(desired, current);
  const auto calibrated_delta = delta * turn_direction;
  if (calibrated_delta > turn_dead_zone) {
    pad.left_x = 0xffU;
  } else if (calibrated_delta < -turn_dead_zone) {
    pad.left_x = 0x01U;
  }
  if (std::abs(delta) < forward_cone &&
      std::hypot(delta_x, delta_z) > trailing_tolerance) {
    pad.left_y = 0x01U;
  }
  if (interact) {
    pad.buttons = 0x1000U;
  }
  return pad;
}

bool tick(sf::game::LegacyGameplayVm &vm,
          const sf::game::LegacyHostPadState &pad = {}) {
  if (!vm.writeHostPadState(pad)) {
    return false;
  }
  const auto frame = vm.tickRetailOuterFrame();
  return frame.completed() && vm.advanceAudioFrameClock();
}

bool waitForActiveGameplay(sf::game::LegacyGameplayVm &vm,
                           std::uint32_t &updates) {
  auto stable = std::uint32_t{};
  while (stable < stable_control_updates &&
         updates < maximum_control_wait_updates) {
    if (!tick(vm)) {
      return false;
    }
    ++updates;
    const auto mission = vm.readMissionBridgeState();
    const auto bridge = vm.readBridgeState();
    const auto ready =
        mission && bridge && !mission->terminal && mission->player_slot >= 0 &&
        bridge->terrain_triggers_enabled && bridge->player.resident &&
        !bridge->player.control_locked && !bridge->camera.scripted &&
        !bridge->camera.locked;
    stable = ready ? stable + 1U : 0U;
  }
  return stable == stable_control_updates;
}

bool appendTick(sf::game::LegacyGameplayVm &vm, DetectionObserver &observer,
                ScenarioResult &result,
                const sf::game::LegacyHostPadState &pad) {
  result.pads.push_back(pad);
  if (!tick(vm, pad)) {
    return false;
  }
  const auto sample = sampleFrame(vm, observer);
  if (!sample) {
    return false;
  }
  result.samples.push_back(*sample);
  return true;
}

bool seedScenario(sf::game::LegacyGameplayVm &vm, DetectionObserver &observer,
                  const Contract &contract, std::uint32_t definition,
                  ScenarioResult &result, FrameSample &active) {
  const auto initial = sampleFrame(vm, observer);
  if (!initial || initial->player_room !=
                      static_cast<std::int16_t>(authored_route.front())) {
    result.stop_phase = "initial-natural-room";
    return false;
  }
  result.samples.push_back(*initial);
  for (std::uint32_t update = 0U; update < maximum_materialization_updates;
       ++update) {
    if (doctorReady(result.samples.back(), contract, definition)) {
      active = result.samples.back();
      return true;
    }
    if (result.samples.back().mission_failure ||
        result.samples.back().detection_calls != 0U ||
        !appendTick(vm, observer, result, {})) {
      result.stop_phase = "natural-doctor-materialization";
      return false;
    }
  }
  result.stop_phase = "natural-doctor-not-observed";
  return false;
}

bool advanceRoute(const sf::game::MissionPackage &package, std::int16_t room,
                  std::size_t &route_index, std::size_t &room_edges) {
  if (room < 0 ||
      static_cast<std::size_t>(room) >= package.layout().modelCount()) {
    return false;
  }
  if (room == static_cast<std::int16_t>(authored_route[route_index])) {
    return true;
  }
  if (route_index + 1U < authored_route.size() &&
      room == static_cast<std::int16_t>(authored_route[route_index + 1U])) {
    ++route_index;
    ++room_edges;
    return true;
  }
  if (route_index != 0U &&
      room == static_cast<std::int16_t>(authored_route[route_index - 1U])) {
    return true;
  }
  return adjacent(package, authored_route[route_index],
                  static_cast<std::uint16_t>(room));
}

ScenarioResult runUndetectedFollow(sf::game::LegacyGameplayVm &vm,
                                   const sf::game::MissionPackage &package,
                                   DetectionObserver &observer,
                                   const Contract &contract,
                                   double turn_direction) {
  ScenarioResult result;
  observer.reset();
  const auto definition = package.objects().objects()[doctor_source].type;
  FrameSample baseline;
  if (!seedScenario(vm, observer, contract, definition, result, baseline)) {
    return result;
  }
  result.route_index = 0U;
  for (std::uint32_t update = 0U; update < undetected_baseline_updates;
       ++update) {
    if (!appendTick(vm, observer, result, {})) {
      result.stop_phase = "undetected-baseline-tick";
      return result;
    }
    const auto &sample = result.samples.back();
    if (!doctorReady(sample, contract, definition)) {
      result.stop_phase = "undetected-baseline-doctor";
      return result;
    }
    if (sample.detection_calls != 0U) {
      result.stop_phase = "undetected-baseline-callback";
      return result;
    }
    if (sample.mission_failure) {
      result.stop_phase = "undetected-baseline-failure";
      return result;
    }
  }

  const auto follow_start = result.samples.back();
  auto previous_player_x = follow_start.player_x;
  auto previous_player_z = follow_start.player_z;
  auto stagnant_updates = std::uint32_t{};
  auto cover_stage = std::uint8_t{};
  for (std::uint32_t update = 0U; update < maximum_follow_updates; ++update) {
    const auto bridge = vm.readBridgeState();
    if (!bridge || doctor_source >= bridge->objects.size()) {
      result.stop_phase = "natural-follow-bridge";
      return result;
    }
    const auto &doctor = bridge->objects[doctor_source];
    const auto forward_length =
        std::hypot(static_cast<double>(doctor.guest_rotation[2]),
                   static_cast<double>(doctor.guest_rotation[8]));
    if (forward_length < 1.0) {
      result.stop_phase = "doctor-heading";
      return result;
    }
    const auto forward_x = doctor.guest_rotation[2] / forward_length;
    const auto forward_z = doctor.guest_rotation[8] / forward_length;
    auto target_x = doctor.position.x - forward_x * trailing_distance;
    auto target_z = doctor.position.z - forward_z * trailing_distance;
    auto hold_for_lead = false;
    if (result.route_index == 1U && cover_stage < 3U) {
      constexpr std::array<std::array<double, 2U>, 2U> cover_waypoints{{
          {-4'050.0, -1'350.0},
          {-6'050.0, -1'350.0},
      }};
      if (cover_stage < cover_waypoints.size()) {
        target_x = cover_waypoints[cover_stage][0U];
        target_z = cover_waypoints[cover_stage][1U];
        if (distance2d(bridge->player.position.x, bridge->player.position.z,
                       static_cast<std::int32_t>(target_x),
                       static_cast<std::int32_t>(target_z)) <= 220.0) {
          ++cover_stage;
        }
      } else if (doctor.position.x > -8'000) {
        hold_for_lead = true;
      } else {
        cover_stage = 3U;
      }
    }
    const auto linked_distance =
        distance2d(bridge->player.position.x, bridge->player.position.z,
                   contract.linked_position.x, contract.linked_position.z);
    const auto interact = result.route_index + 1U == authored_route.size() &&
                          linked_distance <= interaction_distance &&
                          (update % 16U) == 0U;
    const auto player_motion =
        distance2d(bridge->player.position.x, bridge->player.position.z,
                   previous_player_x, previous_player_z);
    stagnant_updates = player_motion < 4.0 ? stagnant_updates + 1U : 0U;
    previous_player_x = bridge->player.position.x;
    previous_player_z = bridge->player.position.z;
    auto heading_offset = std::int32_t{};
    if (stagnant_updates > 40U) {
      constexpr std::array<std::int32_t, 4U> detour_offsets{768, -768, 1024,
                                                            -1024};
      const auto phase = static_cast<std::size_t>(
          ((stagnant_updates - 41U) / 90U) % detour_offsets.size());
      heading_offset = detour_offsets[phase];
    }
    const auto pad =
        hold_for_lead ? sf::game::LegacyHostPadState{}
                      : steerFromBridge(*bridge, target_x, target_z, interact,
                                        turn_direction, heading_offset);
    if (!appendTick(vm, observer, result, pad)) {
      result.stop_phase = "natural-follow-tick";
      return result;
    }
    const auto &sample = result.samples.back();
    if (!advanceRoute(package, sample.player_room, result.route_index,
                      result.room_edges)) {
      result.stop_phase = "natural-route-diverged";
      return result;
    }
    if (sample.detection_calls != 0U) {
      result.stop_phase = "stealth-callback-during-follow";
      return result;
    }
    if (sample.mission_failure) {
      result.stop_phase = "mission-failure-during-follow";
      return result;
    }
    if (doctorReady(sample, contract, definition)) {
      ++result.full_pose_samples;
      const auto separation = distance2d(sample.player_x, sample.player_z,
                                         sample.doctor_x, sample.doctor_z);
      if (separation >= 1'200.0 && separation <= 5'500.0) {
        ++result.safe_follow_samples;
      }
    }
    result.doctor_moved =
        result.doctor_moved ||
        distance2d(sample.doctor_x, sample.doctor_z, follow_start.doctor_x,
                   follow_start.doctor_z) > 256.0;
    result.pose_transition = result.pose_transition ||
                             sample.doctor_pose != follow_start.doctor_pose;
    result.ai_transition =
        result.ai_transition ||
        sample.doctor_ai_state != follow_start.doctor_ai_state ||
        sample.doctor_ai_flags != follow_start.doctor_ai_flags ||
        sample.doctor_route_node != follow_start.doctor_route_node;
    if (result.room_edges >= 1U && result.doctor_moved &&
        result.pose_transition && result.ai_transition &&
        result.safe_follow_samples >= 16U) {
      result.completed = true;
      result.stop_phase = "ready";
      result.doctor_detection_calls = observer.doctor_calls;
      result.other_detection_calls = observer.other_calls;
      result.final_snapshot = vm.captureSnapshot();
      return result;
    }
  }
  result.stop_phase = "natural-route-not-observed";
  return result;
}

ScenarioResult runNaturalDetection(sf::game::LegacyGameplayVm &vm,
                                   const sf::game::MissionPackage &package,
                                   DetectionObserver &observer,
                                   const Contract &contract,
                                   double turn_direction) {
  ScenarioResult result;
  observer.reset();
  const auto definition = package.objects().objects()[doctor_source].type;
  FrameSample baseline;
  if (!seedScenario(vm, observer, contract, definition, result, baseline)) {
    return result;
  }
  for (std::uint32_t update = 0U; update < 8U; ++update) {
    if (!appendTick(vm, observer, result, {}) ||
        result.samples.back().mission_failure ||
        result.samples.back().detection_calls != 0U) {
      result.stop_phase = "detection-baseline";
      return result;
    }
  }
  baseline = result.samples.back();

  for (std::uint32_t update = 0U; update < maximum_exposure_updates; ++update) {
    const auto bridge = vm.readBridgeState();
    if (!bridge || doctor_source >= bridge->objects.size()) {
      result.stop_phase = "natural-exposure-bridge";
      return result;
    }
    const auto &doctor = bridge->objects[doctor_source];
    const auto forward_length =
        std::hypot(static_cast<double>(doctor.guest_rotation[2]),
                   static_cast<double>(doctor.guest_rotation[8]));
    if (forward_length < 1.0) {
      result.stop_phase = "doctor-heading";
      return result;
    }
    // Move Gabe into the doctor's forward view using only the retail pad.
    // A small alternating lateral lead avoids stopping behind collision.
    const auto forward_x = doctor.guest_rotation[2] / forward_length;
    const auto forward_z = doctor.guest_rotation[8] / forward_length;
    const auto side = ((update / 48U) & 1U) == 0U ? 1.0 : -1.0;
    const auto target_x =
        doctor.position.x + forward_x * 480.0 - forward_z * 220.0 * side;
    const auto target_z =
        doctor.position.z + forward_z * 480.0 + forward_x * 220.0 * side;
    const auto pad =
        steerFromBridge(*bridge, target_x, target_z, false, turn_direction);
    if (!appendTick(vm, observer, result, pad)) {
      result.stop_phase = "natural-exposure-tick";
      return result;
    }
    const auto &sample = result.samples.back();
    result.mode_two = result.mode_two || sample.doctor_ai_mode == 2U;
    result.ai_transition =
        result.ai_transition ||
        sample.doctor_ai_mode != baseline.doctor_ai_mode ||
        sample.doctor_ai_state != baseline.doctor_ai_state ||
        sample.doctor_ai_flags != baseline.doctor_ai_flags ||
        sample.doctor_has_target != baseline.doctor_has_target;
    result.failure = result.failure || sample.mission_failure;
    if (observer.doctor_calls != 0U && result.failure) {
      result.doctor_detection_calls = observer.doctor_calls;
      result.other_detection_calls = observer.other_calls;
      result.completed =
          result.ai_transition &&
          observer.last_argument == static_cast<std::int16_t>(doctor_source);
      result.stop_phase =
          result.completed ? "ready" : "retail-detection-transition";
      if (result.completed) {
        result.final_snapshot = vm.captureSnapshot();
      }
      return result;
    }
  }
  result.stop_phase = "natural-detection-not-observed";
  return result;
}

bool replayScenario(sf::game::LegacyGameplayVm &vm, DetectionObserver &observer,
                    const ScenarioResult &expected, std::string &failure) {
  observer.reset();
  const auto initial = sampleFrame(vm, observer);
  if (!initial || expected.samples.size() != expected.pads.size() + 1U ||
      *initial != expected.samples.front()) {
    failure = "checkpoint-replay-initial";
    return false;
  }
  for (std::size_t index = 0U; index < expected.pads.size(); ++index) {
    if (!tick(vm, expected.pads[index])) {
      failure = "checkpoint-replay-tick";
      return false;
    }
    const auto sample = sampleFrame(vm, observer);
    if (!sample || *sample != expected.samples[index + 1U]) {
      failure = "checkpoint-replay-frame";
      return false;
    }
  }
  if (observer.doctor_calls != expected.doctor_detection_calls ||
      observer.other_calls != expected.other_detection_calls ||
      !replayStateEqual(vm.captureSnapshot(), expected.final_snapshot)) {
    failure = "checkpoint-replay-state";
    return false;
  }
  return true;
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "G3 stealth probe requires USA v1.1"};
  }
  const auto &mission = sf::game::missionDefinition(catacomb_mission);
  auto package = sf::game::MissionPackage::load(disc, catacomb_mission);
  auto failure = std::string{};
  const auto contract = validateContract(package, failure);
  if (!contract) {
    std::cerr << "G3 stealth gate failed: phase=" << failure << '\n';
    return 2;
  }

  const auto &image = package.legacyImage();
  auto virtual_cd = image.createVirtualCd();
  sf::game::LegacyGameplayVm vm{image.executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(virtual_cd);
  const auto bootstrap = vm.bootstrapMission(
      static_cast<std::uint32_t>(mission.selection_index), false,
      sf::game::syphonFilterUsaV11FirstMissionBootstrapProfile(),
      sf::game::syphonFilterUsaV11RetailPlatformTailProfile(),
      sf::game::syphonFilterUsaV11FirstMissionOpeningProfile(),
      bootstrap_budget);
  if (!bootstrap.completed() || !tick(vm)) {
    std::cerr << "G3 stealth gate failed: phase=bootstrap\n";
    return 2;
  }
  auto control_updates = std::uint32_t{};
  if (!waitForActiveGameplay(vm, control_updates)) {
    std::cerr << "G3 stealth gate failed: phase=active-gameplay\n";
    return 2;
  }

  const auto calibration_snapshot = vm.captureSnapshot();
  const auto calibration_before = vm.readBridgeState();
  sf::game::LegacyHostPadState calibration_pad;
  calibration_pad.left_x = 0xffU;
  if (!calibration_before) {
    std::cerr << "G3 stealth gate failed: phase=turn-calibration-tick\n";
    return 2;
  }
  auto calibration_after = calibration_before;
  auto calibration_delta = std::int32_t{};
  const auto before_heading = sf::game::headingFromDirection(
      static_cast<double>(calibration_before->player.guest_rotation[2]),
      static_cast<double>(calibration_before->player.guest_rotation[8]));
  for (std::uint32_t update = 0U; update < 16U && calibration_delta == 0;
       ++update) {
    if (!tick(vm, calibration_pad)) {
      std::cerr << "G3 stealth gate failed: phase=turn-calibration-tick\n";
      return 2;
    }
    calibration_after = vm.readBridgeState();
    if (calibration_after) {
      const auto after_heading = sf::game::headingFromDirection(
          static_cast<double>(calibration_after->player.guest_rotation[2]),
          static_cast<double>(calibration_after->player.guest_rotation[8]));
      calibration_delta = signedHeadingDelta(after_heading, before_heading);
    }
  }
  if (!calibration_after || !vm.restoreSnapshot(calibration_snapshot)) {
    std::cerr << "G3 stealth gate failed: phase=turn-calibration-restore\n";
    return 2;
  }
  if (calibration_delta == 0) {
    std::cerr << "G3 stealth gate failed: phase=turn-calibration-motion\n";
    return 2;
  }
  const auto turn_direction = calibration_delta > 0 ? 1.0 : -1.0;

  std::uint32_t callback_instruction{};
  if (!vm.runtime().read32(doctor_detection_callback, callback_instruction) ||
      callback_instruction != doctor_detection_first_instruction) {
    std::cerr << "G3 stealth gate failed: phase=retail-callback-opcode\n";
    return 2;
  }
  DetectionObserver observer;
  observer.bind(vm);
  const auto checkpoint = vm.captureSnapshot();

  auto undetected =
      runUndetectedFollow(vm, package, observer, *contract, turn_direction);
  if (!undetected.completed) {
    std::cerr << "G3 stealth gate failed: phase=" << undetected.stop_phase
              << " route=" << undetected.route_index + 1U << '/'
              << authored_route.size();
    if (!undetected.samples.empty()) {
      const auto &sample = undetected.samples.back();
      std::cerr << " target=" << sample.doctor_target_slot
                << " room=" << sample.player_room
                << " ai-mode=" << static_cast<unsigned>(sample.doctor_ai_mode)
                << " ai-state=" << sample.doctor_ai_state
                << " callbacks=" << sample.detection_calls
                << " failure=" << sample.mission_failure
                << " updates=" << undetected.pads.size() << " player=("
                << sample.player_x << ',' << sample.player_z << ") doctor=("
                << sample.doctor_x << ',' << sample.doctor_z << ") distance="
                << static_cast<std::int32_t>(
                       distance2d(sample.player_x, sample.player_z,
                                  sample.doctor_x, sample.doctor_z));
    }
    std::cerr << " (fail-closed: no direct room/outcome callback fallback)\n";
    return 2;
  }
  if (!vm.restoreSnapshot(checkpoint) ||
      !replayScenario(vm, observer, undetected, failure)) {
    std::cerr << "G3 stealth gate failed: phase="
              << (failure.empty() ? "undetected-checkpoint-restore" : failure)
              << '\n';
    return 2;
  }

  if (!vm.restoreSnapshot(checkpoint)) {
    std::cerr << "G3 stealth gate failed: phase=detection-checkpoint-restore\n";
    return 2;
  }
  auto detected =
      runNaturalDetection(vm, package, observer, *contract, turn_direction);
  if (!detected.completed) {
    std::cerr << "G3 stealth gate failed: phase=" << detected.stop_phase
              << " callback-calls=" << detected.doctor_detection_calls
              << " mode2-sampled=" << detected.mode_two
              << " ai-transition=" << detected.ai_transition
              << " failure=" << detected.failure
              << " callback-arg=" << observer.last_argument
              << " (fail-closed: retail failure was not synthesized)\n";
    return 2;
  }
  if (!vm.restoreSnapshot(checkpoint) ||
      !replayScenario(vm, observer, detected, failure)) {
    std::cerr << "G3 stealth gate failed: phase="
              << (failure.empty() ? "detection-checkpoint-restore" : failure)
              << '\n';
    return 2;
  }
  if (!vm.unbindHostCall(doctor_detection_callback)) {
    std::cerr << "G3 stealth gate failed: phase=observer-unbind\n";
    return 2;
  }

  std::cout << "G3 stealth gate passed: mission=13 source=96 link=5 "
            << "route-topology=" << authored_route.size() << '/'
            << authored_route.size()
            << " natural-prefix-edges=" << undetected.room_edges
            << " follow-samples=" << undetected.safe_follow_samples
            << " pose-samples=" << undetected.full_pose_samples
            << " detection-calls=" << detected.doctor_detection_calls
            << " coverage=natural-pad-follow+retail-detection-failure"
               " checkpoint-replay=exact host-spawn=0 event6=0"
               " direct-outcome=0\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: sf_g3_stealth_probe <game.cue>\n";
    return 1;
  }
  try {
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "G3 stealth gate failed: " << error.what() << '\n';
    return 10;
  }
}
