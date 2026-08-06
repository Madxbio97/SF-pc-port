#include "sf/assets/hmd_model.hpp"
#include "sf/core/error.hpp"
#include "sf/game/chase_camera.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t bootstrap_budget = 500'000'000U;
constexpr std::uint64_t common_call_budget = 5'000'000U;
constexpr std::uint32_t maximum_control_wait_updates = 2'000U;
constexpr std::uint32_t stable_control_updates = 8U;
constexpr std::uint32_t opening_dynamic_descriptor_entry = 0x8005fd04U;
constexpr std::uint32_t opening_dynamic_descriptor = 6U;
constexpr std::size_t opening_camera_source = 35U;
constexpr std::size_t retail_mission_count = 20U;
constexpr std::size_t maximum_candidate_attempts = 8U;
constexpr std::uint32_t maximum_navigation_updates = 1'600U;
constexpr std::uint32_t navigation_stuck_updates = 24U;

enum class ActorKind : std::uint8_t {
  common_hmd,
  retail_rigid_boss,
};

struct RigidBossSpec {
  std::uint32_t mission{};
  std::uint16_t source{};
  std::uint16_t class_id{};
  std::string_view model;
};

// PARK2 and CHOPPER author no common HMD combat actor. Their exact retail
// enemies use dedicated class handlers and rigid TMD presentation, so testing
// an arbitrary HMD (or declaring the mission actor-free) would be dishonest.
constexpr std::array rigid_boss_specs{
    RigidBossSpec{4U, 9U, 0x3cU, "HANS.TMD"},
    RigidBossSpec{9U, 2U, 0x03U, "CHOPPER.TMD"},
};

struct PreferredActorSpec {
  std::uint32_t mission{};
  std::uint16_t source{};
};

constexpr std::array preferred_actor_specs{
    PreferredActorSpec{0U, 179U},  PreferredActorSpec{1U, 210U},
    PreferredActorSpec{2U, 17U},   PreferredActorSpec{6U, 167U},
    PreferredActorSpec{8U, 222U},  PreferredActorSpec{11U, 196U},
    PreferredActorSpec{12U, 142U}, PreferredActorSpec{15U, 109U},
    PreferredActorSpec{16U, 245U}, PreferredActorSpec{17U, 17U},
    PreferredActorSpec{18U, 11U},  PreferredActorSpec{19U, 83U},
};

struct ActorCandidate {
  std::uint16_t source{};
  std::uint16_t room{};
  std::uint8_t pose_parts{};
  std::uint32_t rank{};
  ActorKind kind{ActorKind::common_hmd};
};

struct ActorSample {
  std::uint32_t definition{};
  std::int16_t class_id{};
  std::int16_t health{};
  std::uint32_t path_pointer{};
  std::uint32_t instance{};
  std::uint32_t root_node{};
  std::uint32_t display_node{};
  std::uint32_t pose_flags{};
  std::uint32_t motion_controller{};
  std::uint32_t presentation_controller{};
  std::uint32_t target_controller{};
  std::uint32_t health_controller{};
  std::uint32_t ai_controller{};
  std::uint32_t ai_flags{};
  std::uint16_t ai_state{};
  std::uint8_t ai_fire_latch{};
  std::uint8_t ai_route_node{};
  std::uint8_t ai_previous_route_node{};
  std::uint8_t ai_mode{};
  std::uint8_t ai_combat_mode{};
  std::int16_t target_slot{-1};
  std::uint32_t target_flags{};
  std::int16_t target_meter{};
  std::uint32_t danger_q12{};
  std::uint8_t instance_flags{};
  std::array<std::uint8_t, 4U> instance_state{};
  std::uint8_t presentation_enabled{};
  std::uint8_t presentation_mode{};
  sf::game::LegacyNativePoint position;
  std::array<std::int16_t, 9U> guest_rotation{};
  std::uint64_t pose_digest{};
  std::uint64_t behavior_digest{};
  std::uint8_t bone_matrix_count{};
  bool resident{};
  bool simulated{};
  bool has_target{};
  bool destroyed{};

  [[nodiscard]] friend bool operator==(const ActorSample &left,
                                       const ActorSample &right) noexcept {
    return left.definition == right.definition &&
           left.class_id == right.class_id && left.health == right.health &&
           left.path_pointer == right.path_pointer &&
           left.instance == right.instance &&
           left.root_node == right.root_node &&
           left.display_node == right.display_node &&
           left.pose_flags == right.pose_flags &&
           left.motion_controller == right.motion_controller &&
           left.presentation_controller == right.presentation_controller &&
           left.target_controller == right.target_controller &&
           left.health_controller == right.health_controller &&
           left.ai_controller == right.ai_controller &&
           left.ai_flags == right.ai_flags && left.ai_state == right.ai_state &&
           left.ai_fire_latch == right.ai_fire_latch &&
           left.ai_route_node == right.ai_route_node &&
           left.ai_previous_route_node == right.ai_previous_route_node &&
           left.ai_mode == right.ai_mode &&
           left.ai_combat_mode == right.ai_combat_mode &&
           left.target_slot == right.target_slot &&
           left.target_flags == right.target_flags &&
           left.target_meter == right.target_meter &&
           left.danger_q12 == right.danger_q12 &&
           left.instance_flags == right.instance_flags &&
           left.instance_state == right.instance_state &&
           left.presentation_enabled == right.presentation_enabled &&
           left.presentation_mode == right.presentation_mode &&
           left.position.x == right.position.x &&
           left.position.y == right.position.y &&
           left.position.z == right.position.z &&
           left.guest_rotation == right.guest_rotation &&
           left.pose_digest == right.pose_digest &&
           left.behavior_digest == right.behavior_digest &&
           left.bone_matrix_count == right.bone_matrix_count &&
           left.resident == right.resident &&
           left.simulated == right.simulated &&
           left.has_target == right.has_target &&
           left.destroyed == right.destroyed;
  }
};

struct ScenarioResult {
  bool completed{};
  std::string stop_phase;
  std::uint16_t source{};
  std::uint16_t room{};
  sf::game::LegacyNativePoint player_position;
  std::int16_t player_room{-1};
  std::vector<ActorSample> samples;
  std::uint64_t impact_instructions{};
  std::uint64_t nonlethal_instructions{};
  std::uint64_t lethal_instructions{};
  std::size_t full_pose_samples{};
  std::size_t rigid_presentation_samples{};
  std::size_t navigation_updates{};
  std::size_t natural_room_edges{};
  std::int16_t initial_player_health{};
  std::int16_t minimum_player_health{};
  std::size_t enemy_fire_samples{};
  bool stealth_observed{};
  bool ai_transition{};
  bool target_transition{};
  bool target_acquired{};
  bool pose_transition{};
  bool behavior_transition{};
  bool nonlethal_damage{};
  bool death_observed{};
  bool drop_expected{};
  bool drop_observed{};
  sf::game::LegacyGameplayVmSnapshot final_snapshot;
};

struct MissionResult {
  bool completed{};
  bool dormant_contract{};
  std::string stop_phase;
  std::size_t authored_actor_candidates{};
  std::size_t attempted_scenarios{};
  ActorCandidate candidate;
  ScenarioResult first;
  ScenarioResult replay;
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

std::uint64_t
behaviorDigest(const sf::game::LegacyObjectBridgeState &actor) noexcept {
  auto digest = std::uint64_t{1469598103934665603ULL};
  digestWord(digest, actor.instance_flags);
  for (const auto state : actor.instance_state) {
    digestWord(digest, state);
  }
  digestWord(digest, actor.pose_flags);
  digestWord(digest, actor.presentation_enabled);
  digestWord(digest, actor.presentation_mode);
  digestWord(digest, actor.ai_flags);
  digestWord(digest, actor.ai_state);
  digestWord(digest, actor.ai_mode);
  digestWord(digest, actor.ai_combat_mode);
  for (const auto component : actor.guest_rotation) {
    digestWord(digest, static_cast<std::uint16_t>(component));
  }
  digestWord(digest, std::bit_cast<std::uint32_t>(actor.position.x));
  digestWord(digest, std::bit_cast<std::uint32_t>(actor.position.y));
  digestWord(digest, std::bit_cast<std::uint32_t>(actor.position.z));
  return digest;
}

ActorSample
sampleActor(const sf::game::LegacyObjectBridgeState &actor) noexcept {
  return ActorSample{
      .definition = actor.definition,
      .class_id = actor.class_id,
      .health = actor.health,
      .path_pointer = actor.path_pointer,
      .instance = actor.instance,
      .root_node = actor.root_node,
      .display_node = actor.display_node,
      .pose_flags = actor.pose_flags,
      .motion_controller = actor.motion_controller,
      .presentation_controller = actor.presentation_controller,
      .target_controller = actor.target_controller,
      .health_controller = actor.health_controller,
      .ai_controller = actor.ai_controller,
      .ai_flags = actor.ai_flags,
      .ai_state = actor.ai_state,
      .ai_fire_latch = actor.ai_fire_latch,
      .ai_route_node = actor.ai_route_node,
      .ai_previous_route_node = actor.ai_previous_route_node,
      .ai_mode = actor.ai_mode,
      .ai_combat_mode = actor.ai_combat_mode,
      .target_slot = actor.target_slot,
      .target_flags = actor.target_flags,
      .target_meter = actor.target_meter,
      .danger_q12 = actor.danger_q12,
      .instance_flags = actor.instance_flags,
      .instance_state = actor.instance_state,
      .presentation_enabled = actor.presentation_enabled,
      .presentation_mode = actor.presentation_mode,
      .position = actor.position,
      .guest_rotation = actor.guest_rotation,
      .pose_digest = poseDigest(actor),
      .behavior_digest = behaviorDigest(actor),
      .bone_matrix_count = actor.bone_matrix_count,
      .resident = actor.resident,
      .simulated = actor.simulated,
      .has_target = actor.has_target,
      .destroyed = actor.destroyed(),
  };
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
         left.agent_cbdc_friendly_fire_frame ==
             right.agent_cbdc_friendly_fire_frame &&
         left.agent_cbdc_friendly_fire_pending_penalties ==
             right.agent_cbdc_friendly_fire_pending_penalties &&
         left.video_timing_baseline_initialized ==
             right.video_timing_baseline_initialized &&
         left.audio_frame_tick_initialized ==
             right.audio_frame_tick_initialized &&
         virtual_cd_equal;
}

bool tick(sf::game::LegacyGameplayVm &vm,
          const sf::game::LegacyHostPadState &pad = {}) {
  if (!vm.writeHostPadState(pad)) {
    return false;
  }
  const auto frame = vm.tickRetailOuterFrame();
  return frame.completed() && vm.advanceAudioFrameClock();
}

bool prepareFirstVisibleFrame(sf::game::LegacyGameplayVm &vm,
                              std::uint32_t mission_index) {
  // Production performs this SUBWAY-only dynamic display activation after
  // skipping the frontend movie callback. It does not spawn an actor or send
  // an object event; it merely reaches the first normal retail frame.
  if (mission_index == 0U) {
    constexpr std::array arguments{opening_dynamic_descriptor};
    const auto activation = vm.invoke(opening_dynamic_descriptor_entry,
                                      arguments, common_call_budget);
    if (!activation.completed()) {
      return false;
    }
  }
  return tick(vm);
}

bool waitForActiveGameplay(sf::game::LegacyGameplayVm &vm,
                           std::uint32_t mission_index,
                           std::uint32_t &updates) {
  auto stable_control = std::uint32_t{};
  while (stable_control < stable_control_updates &&
         updates < maximum_control_wait_updates) {
    if (!tick(vm)) {
      return false;
    }
    ++updates;
    const auto mission = vm.readMissionBridgeState();
    const auto bridge = vm.readBridgeState();
    const auto opening_finished =
        mission_index != 0U ||
        (bridge && opening_camera_source < bridge->objects.size() &&
         bridge->objects[opening_camera_source].health <= 0);
    const auto ready =
        mission && bridge && !mission->terminal && mission->player_slot >= 0 &&
        opening_finished && bridge->terrain_triggers_enabled &&
        bridge->player.resident && !bridge->player.control_locked &&
        !bridge->camera.scripted && !bridge->camera.locked;
    stable_control = ready ? stable_control + 1U : 0U;
  }
  return stable_control == stable_control_updates;
}

std::optional<std::uint16_t> objectRoom(const sf::game::MissionPackage &package,
                                        std::uint16_t source) noexcept {
  for (std::size_t room = 0U; room < package.objects().roomCount(); ++room) {
    if (std::ranges::find(package.objects().objectsInRoom(room), source) !=
        package.objects().objectsInRoom(room).end()) {
      if (room > std::numeric_limits<std::uint16_t>::max()) {
        return std::nullopt;
      }
      return static_cast<std::uint16_t>(room);
    }
  }
  return std::nullopt;
}

std::optional<std::uint8_t>
objectPoseParts(const sf::game::MissionPackage &package, std::uint16_t source) {
  const auto objects = package.objects().objects();
  if (source >= objects.size()) {
    return std::nullopt;
  }
  const auto &definition = package.objects().definition(objects[source].type);
  if (!std::string_view{definition.primary_model}.ends_with(".HMD")) {
    return std::nullopt;
  }
  try {
    const auto count =
        sf::assets::HmdModel::parse(
            package.objectModels().file(definition.primary_model))
            .parts()
            .size();
    if (count == 0U || count > sf::game::legacy_actor_bone_count ||
        count > std::numeric_limits<std::uint8_t>::max()) {
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(count);
  } catch (...) {
    return std::nullopt;
  }
}

bool adjacentRoom(const sf::game::MissionPackage &package, std::uint16_t left,
                  std::uint16_t right) {
  const auto contains = [](const auto &rooms, std::uint16_t room) {
    return std::ranges::find(rooms, room) != rooms.end();
  };
  return contains(package.layout().visibility(left).active_models, right) ||
         contains(package.layout().visibility(right).active_models, left);
}

std::optional<std::vector<std::uint16_t>>
shortestRoomPath(const sf::game::MissionPackage &package, std::uint16_t origin,
                 std::uint16_t destination) {
  const auto room_count = package.layout().modelCount();
  if (origin >= room_count || destination >= room_count) {
    return std::nullopt;
  }
  if (origin == destination) {
    return std::vector<std::uint16_t>{origin};
  }
  constexpr auto absent = std::numeric_limits<std::uint16_t>::max();
  auto previous = std::vector<std::uint16_t>(room_count, absent);
  auto queue = std::deque<std::uint16_t>{origin};
  previous[origin] = origin;
  while (!queue.empty() && previous[destination] == absent) {
    const auto room = queue.front();
    queue.pop_front();
    for (std::size_t candidate = 0U; candidate < room_count; ++candidate) {
      const auto candidate16 = static_cast<std::uint16_t>(candidate);
      if (previous[candidate] != absent ||
          !adjacentRoom(package, room, candidate16)) {
        continue;
      }
      previous[candidate] = room;
      queue.push_back(candidate16);
    }
  }
  if (previous[destination] == absent) {
    return std::nullopt;
  }
  auto reverse = std::vector<std::uint16_t>{destination};
  while (reverse.back() != origin) {
    reverse.push_back(previous[reverse.back()]);
  }
  std::ranges::reverse(reverse);
  return reverse;
}

std::optional<RigidBossSpec>
rigidBossSpec(std::uint32_t mission_index) noexcept {
  const auto found = std::ranges::find(rigid_boss_specs, mission_index,
                                       &RigidBossSpec::mission);
  return found == rigid_boss_specs.end() ? std::nullopt : std::optional{*found};
}

std::vector<ActorCandidate>
actorCandidates(const sf::game::MissionPackage &package,
                const sf::game::LegacyGameplayBridgeState &bridge,
                std::int16_t player_slot, std::uint32_t mission_index) {
  const auto objects = package.objects().objects();
  const auto count = std::min(objects.size(), bridge.objects.size());
  auto candidates = std::vector<ActorCandidate>{};
  if (bridge.player.room < 0) {
    return candidates;
  }
  const auto origin_room = static_cast<std::uint16_t>(bridge.player.room);
  const auto boss = rigidBossSpec(mission_index);
  const auto preferred = std::ranges::find(preferred_actor_specs, mission_index,
                                           &PreferredActorSpec::mission);
  for (std::size_t source = 0U; source < count; ++source) {
    const auto &guest = bridge.objects[source];
    if (source == static_cast<std::size_t>(player_slot) ||
        guest.object_handler == 0U || guest.maximum_health <= 1) {
      continue;
    }
    const auto source16 = static_cast<std::uint16_t>(source);
    const auto room = objectRoom(package, source16);
    if (!room) {
      continue;
    }
    const auto &definition = package.objects().definition(objects[source].type);
    auto kind = ActorKind::common_hmd;
    auto pose_parts = objectPoseParts(package, source16);
    if (boss) {
      if (source16 != boss->source || definition.class_id != boss->class_id ||
          definition.primary_model != boss->model || pose_parts) {
        continue;
      }
      kind = ActorKind::retail_rigid_boss;
    } else if (!pose_parts ||
               guest.object_handler != sf::game::legacy_common_npc_handler) {
      continue;
    }
    const auto route = shortestRoomPath(package, origin_room, *room);
    if (!route) {
      continue;
    }
    auto rank = std::uint32_t{};
    rank += preferred == preferred_actor_specs.end() ||
                    preferred->source == source16
                ? 0U
                : 2'000'000U;
    const auto already_present = guest.instance != 0U &&
                                 guest.root_node != 0U &&
                                 guest.display_node != 0U && guest.resident &&
                                 (kind == ActorKind::retail_rigid_boss ||
                                  guest.bone_matrix_count == *pose_parts);
    rank += already_present ? 0U : 1'000'000U;
    rank += static_cast<std::uint32_t>(route->size() - 1U) * 20'000U;
    // Generic class-1 soldiers are preferable: unlike story/protected actors,
    // their death callback cannot intentionally terminate the mission.
    rank += guest.class_id == 1 ? 0U : 10'000U;
    rank += guest.linked_slot < 0 ? 0U : 2'000U;
    rank += objects[source].patrol_path.empty() ? 100U : 0U;
    rank += (guest.instance_state[3] & sf::game::legacy_instance_dormant) == 0U
                ? 0U
                : 5'000U;
    rank += static_cast<std::uint32_t>(source);
    candidates.push_back(ActorCandidate{
        source16,
        *room,
        pose_parts.value_or(0U),
        rank,
        kind,
    });
  }
  std::ranges::sort(candidates, {}, &ActorCandidate::rank);
  if (preferred != preferred_actor_specs.end()) {
    std::erase_if(candidates, [preferred](const auto &candidate) {
      return candidate.source != preferred->source;
    });
  }
  return candidates;
}

bool activeActor(const sf::game::LegacyObjectBridgeState &actor,
                 const ActorCandidate &candidate) noexcept {
  const auto common =
      actor.object_handler != 0U && actor.alive() && actor.resident &&
      actor.instance != 0U && actor.root_node != 0U &&
      actor.display_node != 0U && actor.motion_controller != 0U &&
      actor.presentation_controller != 0U && actor.health_controller != 0U;
  if (!common) {
    return false;
  }
  if (candidate.kind == ActorKind::retail_rigid_boss) {
    return actor.presentation_enabled != 0U;
  }
  return actor.simulated && actor.ai_controller != 0U &&
         actor.target_controller != 0U &&
         actor.object_handler == sf::game::legacy_common_npc_handler &&
         actor.bone_matrix_count == candidate.pose_parts;
}

bool aiStateDifferent(const ActorSample &left,
                      const ActorSample &right) noexcept {
  return left.ai_flags != right.ai_flags || left.ai_state != right.ai_state ||
         left.ai_fire_latch != right.ai_fire_latch ||
         left.ai_route_node != right.ai_route_node ||
         left.ai_previous_route_node != right.ai_previous_route_node ||
         left.ai_mode != right.ai_mode ||
         left.ai_combat_mode != right.ai_combat_mode;
}

bool targetStateDifferent(const ActorSample &left,
                          const ActorSample &right) noexcept {
  return left.has_target != right.has_target ||
         left.target_slot != right.target_slot ||
         left.target_flags != right.target_flags ||
         left.target_meter != right.target_meter ||
         left.danger_q12 != right.danger_q12;
}

struct NavigationState {
  sf::game::LegacyNativePoint previous_position;
  std::int16_t previous_room{-1};
  std::uint32_t stationary_updates{};
  std::uint32_t recovery_updates{};
  std::uint32_t recovery_attempts{};
  bool initialized{};
};

double horizontalDistance(const sf::game::LegacyNativePoint &left,
                          const sf::game::LegacyNativePoint &right) noexcept {
  return std::hypot(static_cast<double>(left.x) - right.x,
                    static_cast<double>(left.z) - right.z);
}

std::optional<sf::game::LegacyNativePoint>
navigationTarget(const sf::game::MissionPackage &package,
                 const sf::game::LegacyGameplayBridgeState &bridge,
                 const ActorCandidate &candidate) {
  if (bridge.player.room < 0) {
    return std::nullopt;
  }
  const auto room = static_cast<std::uint16_t>(bridge.player.room);
  if (room == candidate.room) {
    const auto &transform =
        package.objects().objects()[candidate.source].transform;
    return sf::game::LegacyNativePoint{transform.x, -transform.y, transform.z};
  }
  const auto path = shortestRoomPath(package, room, candidate.room);
  if (!path || path->size() < 2U) {
    return std::nullopt;
  }
  const auto next_room = (*path)[1U];
  const auto members = package.objects().objectsInRoom(next_room);
  if (members.empty()) {
    return std::nullopt;
  }
  auto best = std::optional<sf::game::LegacyNativePoint>{};
  auto best_distance = std::numeric_limits<double>::max();
  for (const auto source : members) {
    const auto &transform = package.objects().objects()[source].transform;
    const auto point =
        sf::game::LegacyNativePoint{transform.x, -transform.y, transform.z};
    const auto distance = horizontalDistance(bridge.player.position, point);
    if (distance < best_distance) {
      best_distance = distance;
      best = point;
    }
  }
  return best;
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

std::optional<sf::game::LegacyHostPadState>
navigationPad(const sf::game::MissionPackage &package,
              const sf::game::LegacyGameplayBridgeState &bridge,
              const ActorCandidate &candidate, NavigationState &state,
              std::size_t &room_edges, double turn_direction) {
  constexpr double movement_epsilon = 8.0;
  constexpr std::int32_t turn_dead_zone = 64;
  constexpr std::int32_t forward_cone = 640;
  constexpr std::uint32_t recovery_duration = 90U;
  const auto target = navigationTarget(package, bridge, candidate);
  if (!target || !bridge.player.resident || bridge.player.room < 0) {
    return std::nullopt;
  }
  if (!state.initialized) {
    state.previous_position = bridge.player.position;
    state.previous_room = bridge.player.room;
    state.initialized = true;
  } else {
    const auto moved =
        horizontalDistance(state.previous_position, bridge.player.position);
    state.stationary_updates =
        moved > movement_epsilon ? 0U : state.stationary_updates + 1U;
    if (bridge.player.room != state.previous_room) {
      ++room_edges;
      state.stationary_updates = 0U;
    }
    state.previous_position = bridge.player.position;
    state.previous_room = bridge.player.room;
  }

  sf::game::LegacyHostPadState pad;
  if (state.recovery_updates != 0U ||
      state.stationary_updates >= navigation_stuck_updates) {
    if (state.recovery_updates == 0U) {
      state.recovery_updates = recovery_duration;
      state.stationary_updates = 0U;
      ++state.recovery_attempts;
    }
    const auto turn_right = ((static_cast<std::uint32_t>(candidate.source) +
                              state.recovery_attempts) &
                             1U) == 0U;
    pad.left_x = turn_right ? 0xffU : 0x01U;
    pad.left_y = 0x01U;
    if ((state.recovery_updates % 8U) == 0U) {
      pad.buttons = 0x1000U;
    }
    --state.recovery_updates;
    return pad;
  }

  const auto desired = sf::game::headingFromDirection(
      static_cast<double>(target->x) - bridge.player.position.x,
      static_cast<double>(target->z) - bridge.player.position.z);
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
  // Keep walking through a next-room anchor until the retail room detector
  // confirms the portal edge. Stopping on the closest object in that room can
  // leave Gabe touching the threshold while the authored volume stays cold.
  if (std::abs(delta) < forward_cone) {
    pad.left_y = 0x01U;
  }
  if (state.stationary_updates > 8U && (state.stationary_updates % 16U) == 0U) {
    pad.buttons = 0x1000U;
  }
  return pad;
}

bool appendActorSample(ScenarioResult &result,
                       const sf::game::LegacyGameplayBridgeState &bridge,
                       const ActorCandidate &candidate) {
  if (candidate.source >= bridge.objects.size()) {
    result.stop_phase = "actor-record-lost";
    return false;
  }
  const auto &actor = bridge.objects[candidate.source];
  result.samples.push_back(sampleActor(actor));
  if (candidate.kind == ActorKind::common_hmd) {
    result.full_pose_samples +=
        actor.bone_matrix_count == candidate.pose_parts ? 1U : 0U;
  } else if (actor.instance != 0U && actor.root_node != 0U &&
             actor.display_node != 0U && actor.resident &&
             (actor.presentation_controller != 0U ||
              actor.bone_matrix_count != 0U)) {
    ++result.rigid_presentation_samples;
  }
  return true;
}

ScenarioResult runScenario(sf::game::LegacyGameplayVm &vm,
                           const sf::game::MissionPackage &package,
                           const ActorCandidate &candidate,
                           double turn_direction) {
  auto result = ScenarioResult{};
  result.source = candidate.source;
  result.room = candidate.room;
  const auto mission = vm.readMissionBridgeState();
  const auto initial_bridge = vm.readBridgeState();
  if (!mission || !initial_bridge || mission->player_slot < 0 ||
      mission->terminal || !initial_bridge->player.resident) {
    result.stop_phase = "mission-state";
    return result;
  }
  result.player_position = initial_bridge->player.position;
  result.initial_player_health = mission->player_health;
  result.minimum_player_health = mission->player_health;
  const auto &authored = package.objects().objects()[candidate.source];

  auto stealth_sample = std::optional<ActorSample>{};
  NavigationState navigation;
  for (std::uint32_t update = 0U; update < maximum_navigation_updates;
       ++update) {
    auto bridge = vm.readBridgeState();
    if (!bridge || !bridge->terrain_triggers_enabled ||
        !appendActorSample(result, *bridge, candidate)) {
      if (result.stop_phase.empty()) {
        result.stop_phase = "natural-navigation-bridge";
      }
      return result;
    }
    const auto &actor = bridge->objects[candidate.source];
    result.player_position = bridge->player.position;
    result.player_room = bridge->player.room;
    if (actor.definition != authored.type ||
        actor.class_id !=
            static_cast<std::int16_t>(
                package.objects().definition(authored.type).class_id)) {
      result.stop_phase = "actor-identity";
      return result;
    }
    if (activeActor(actor, candidate)) {
      stealth_sample = result.samples.back();
      break;
    }
    const auto mission_during_navigation = vm.readMissionBridgeState();
    if (mission_during_navigation) {
      result.minimum_player_health = std::min(
          result.minimum_player_health, mission_during_navigation->player_health);
      result.enemy_fire_samples += static_cast<std::size_t>(std::ranges::count_if(
          bridge->objects, [&](const auto &object) {
            return object.resident && object.health > 0 && object.has_target &&
                   object.target_slot == mission->player_slot &&
                   object.ai_fire_latch != 0U;
          }));
    }
    const auto pad = navigationPad(package, *bridge, candidate, navigation,
                                   result.natural_room_edges, turn_direction);
    if (!mission_during_navigation || mission_during_navigation->terminal ||
        !pad || !tick(vm, *pad)) {
      result.stop_phase = "natural-navigation-tick";
      return result;
    }
    ++result.navigation_updates;
  }
  if (!stealth_sample) {
    const auto dormant_bridge = vm.readBridgeState();
    if (candidate.kind == ActorKind::retail_rigid_boss && dormant_bridge &&
        candidate.source < dormant_bridge->objects.size()) {
      const auto &actor = dormant_bridge->objects[candidate.source];
      if (actor.object_handler != 0U && actor.instance != 0U &&
          actor.ai_controller != 0U && actor.resident && actor.health > 0 &&
          !actor.simulated && actor.display_node == 0U &&
          actor.bone_matrix_count != 0U) {
        result.completed = true;
        result.stop_phase = "dormant-rigid-ready";
        result.final_snapshot = vm.captureSnapshot();
        return result;
      }
    }
    const auto activation = vm.queueHostImpact(
        mission->player_slot, static_cast<std::int16_t>(candidate.source));
    result.impact_instructions = activation.execution.instructions;
    if (!activation.completed() || result.impact_instructions == 0U) {
      result.stop_phase = "retail-impact-activation";
      return result;
    }
    constexpr std::uint32_t activation_updates = 64U;
    for (std::uint32_t update = 0U; update < activation_updates; ++update) {
      const auto bridge = vm.readBridgeState();
      if (!bridge || !appendActorSample(result, *bridge, candidate)) {
        if (result.stop_phase.empty()) {
          result.stop_phase = "impact-activation-bridge";
        }
        return result;
      }
      if (activeActor(bridge->objects[candidate.source], candidate)) {
        stealth_sample = result.samples.back();
        break;
      }
      if (!tick(vm)) {
        result.stop_phase = "impact-activation-tick";
        return result;
      }
    }
    if (!stealth_sample) {
      result.stop_phase = "retail-actor-not-observed";
      return result;
    }
  }
  result.stealth_observed = true;

  // Keep two neutral retail frames between materialization and the combat
  // stimulus. This captures the guest awareness state without asserting that
  // every authored guard starts outside detection range. CATACOMB's actual
  // undetected-following contract has its own dedicated stealth gate.
  constexpr std::uint32_t stealth_updates = 2U;
  for (std::uint32_t update = 0U; update < stealth_updates; ++update) {
    if (!tick(vm)) {
      result.stop_phase = "stealth-tick";
      return result;
    }
    const auto bridge = vm.readBridgeState();
    if (!bridge || !appendActorSample(result, *bridge, candidate) ||
        !activeActor(bridge->objects[candidate.source], candidate)) {
      if (result.stop_phase.empty()) {
        result.stop_phase = "stealth-hold";
      }
      return result;
    }
  }

  // Test-only combat stimulus. queueHostImpact dispatches retail event 0x0d;
  // it is deliberately not the forbidden visibility/spawn event 6.
  if (result.impact_instructions == 0U) {
    const auto impact = vm.queueHostImpact(
        mission->player_slot, static_cast<std::int16_t>(candidate.source));
    result.impact_instructions = impact.execution.instructions;
    if (!impact.completed() || result.impact_instructions == 0U) {
      result.stop_phase = "guest-impact";
      return result;
    }
  }
  auto bridge = vm.readBridgeState();
  if (!bridge || !appendActorSample(result, *bridge, candidate)) {
    if (result.stop_phase.empty()) {
      result.stop_phase = "impact-bridge";
    }
    return result;
  }

  if (candidate.kind == ActorKind::retail_rigid_boss) {
    constexpr std::uint32_t rigid_reaction_updates = 32U;
    for (std::uint32_t update = 0U; update < rigid_reaction_updates; ++update) {
      if (!tick(vm)) {
        result.stop_phase = "rigid-reaction-tick";
        return result;
      }
      bridge = vm.readBridgeState();
      if (!bridge || !appendActorSample(result, *bridge, candidate)) {
        result.stop_phase = "rigid-reaction-bridge";
        return result;
      }
      result.behavior_transition =
          result.behavior_transition || result.samples.back().behavior_digest !=
                                            stealth_sample->behavior_digest;
      result.pose_transition =
          result.pose_transition ||
          result.samples.back().pose_digest != stealth_sample->pose_digest;
    }
    const auto &actor = bridge->objects[candidate.source];
    result.completed = result.impact_instructions != 0U && actor.alive() &&
                       actor.resident && actor.instance != 0U &&
                       actor.display_node != 0U &&
                       result.rigid_presentation_samples >= 3U &&
                       (result.behavior_transition || result.pose_transition);
    result.stop_phase = result.completed ? "ready" : "rigid-handler-reaction";
    if (result.completed) {
      result.final_snapshot = vm.captureSnapshot();
    }
    return result;
  }

  auto before_damage = bridge->objects[candidate.source].health;
  if (before_damage <= 1) {
    result.stop_phase = "nonlethal-health-precondition";
    return result;
  }
  const auto nonlethal = vm.queueHostDamage(sf::game::LegacyHostDamageEvent{
      mission->player_slot,
      mission->player_slot,
      static_cast<std::int16_t>(candidate.source),
      1,
      0x0f,
  });
  result.nonlethal_instructions = nonlethal.execution.instructions;
  if (!nonlethal.completed() || result.nonlethal_instructions == 0U) {
    result.stop_phase = "guest-nonlethal-damage";
    return result;
  }

  const auto reaction_updates =
      candidate.kind == ActorKind::common_hmd ? 24U : 96U;
  for (std::uint32_t update = 0U; update < reaction_updates; ++update) {
    if (!tick(vm)) {
      result.stop_phase = "reaction-tick";
      return result;
    }
    bridge = vm.readBridgeState();
    if (!bridge || !appendActorSample(result, *bridge, candidate)) {
      if (result.stop_phase.empty()) {
        result.stop_phase = "reaction-bridge";
      }
      return result;
    }
    const auto &actor = bridge->objects[candidate.source];
    result.nonlethal_damage =
        result.nonlethal_damage ||
        (actor.health > 0 && actor.health < before_damage);
    result.target_acquired =
        result.target_acquired ||
        (actor.has_target && actor.target_slot == mission->player_slot);
    result.ai_transition =
        result.ai_transition ||
        aiStateDifferent(*stealth_sample, result.samples.back());
    result.target_transition =
        result.target_transition ||
        targetStateDifferent(*stealth_sample, result.samples.back());
    result.behavior_transition =
        result.behavior_transition || result.samples.back().behavior_digest !=
                                          stealth_sample->behavior_digest;
    result.pose_transition =
        result.pose_transition ||
        (candidate.kind == ActorKind::common_hmd
             ? result.samples.back().pose_digest != stealth_sample->pose_digest
             : result.behavior_transition);
    const auto common_reaction =
        result.target_acquired && result.ai_transition &&
        result.target_transition && result.pose_transition;
    const auto boss_reaction = result.behavior_transition &&
                               result.pose_transition &&
                               result.rigid_presentation_samples >= 3U;
    if (result.nonlethal_damage &&
        (candidate.kind == ActorKind::common_hmd ? common_reaction
                                                 : boss_reaction)) {
      break;
    }
  }
  const auto reaction_complete =
      candidate.kind == ActorKind::common_hmd
          ? result.target_acquired && result.ai_transition &&
                result.target_transition && result.pose_transition
          : result.behavior_transition && result.pose_transition &&
                result.rigid_presentation_samples >= 3U;
  if (!result.nonlethal_damage || !reaction_complete) {
    result.stop_phase = "guest-combat-reaction";
    return result;
  }

  bridge = vm.readBridgeState();
  if (!bridge || candidate.source >= bridge->objects.size() ||
      bridge->objects[candidate.source].destroyed()) {
    result.stop_phase = "lethal-health-precondition";
    return result;
  }
  const auto drop_attributes = bridge->objects[candidate.source].attributes;
  auto expected_weapon = std::optional<std::uint16_t>{};
  if ((drop_attributes & 0x00ffU) != 0U) {
    expected_weapon =
        static_cast<std::uint16_t>(drop_attributes & 0x00ffU);
  } else if ((drop_attributes & 0x3000U) == 0x1000U) {
    expected_weapon = std::uint16_t{0x13U};
  } else if ((drop_attributes & 0x3000U) == 0x2000U) {
    expected_weapon = std::uint16_t{0x14U};
  }
  const auto expected_armour = (drop_attributes & 0x4000U) != 0U;
  result.drop_expected = expected_weapon.has_value() || expected_armour;
  const auto lethal = vm.queueHostDamage(sf::game::LegacyHostDamageEvent{
      mission->player_slot,
      mission->player_slot,
      static_cast<std::int16_t>(candidate.source),
      std::numeric_limits<std::int16_t>::max(),
      0x0f,
  });
  result.lethal_instructions = lethal.execution.instructions;
  if (!lethal.completed() || result.lethal_instructions == 0U) {
    result.stop_phase = "guest-lethal-damage";
    return result;
  }

  const auto has_item = [](const sf::game::LegacyGameplayBridgeState &state,
                           std::uint16_t item) {
    return std::ranges::any_of(
        state.dropped_items,
        [item](const auto &drop) { return drop.item == item; });
  };
  // Let the normal outer frame own both halves of the lifecycle. Its
  // pre-render boundary completes the exact retail attach/detach transition;
  // the probe must not mutate finalized renderer and item pools afterward.
  constexpr std::uint32_t death_updates = 192U;
  for (std::uint32_t update = 0U; update < death_updates; ++update) {
    if (!tick(vm)) {
      result.stop_phase = "death-tick";
      return result;
    }
    bridge = vm.readBridgeState();
    if (!bridge || !appendActorSample(result, *bridge, candidate)) {
      if (result.stop_phase.empty()) {
        result.stop_phase = "death-bridge";
      }
      return result;
    }
    result.death_observed =
        result.death_observed || bridge->objects[candidate.source].destroyed();
    result.drop_observed =
        result.drop_observed ||
        ((!expected_weapon || has_item(*bridge, *expected_weapon)) &&
         (!expected_armour || has_item(*bridge, 0x80U)));
    if (result.death_observed &&
        (!result.drop_expected || result.drop_observed)) {
      break;
    }
  }
  if (!result.death_observed) {
    result.stop_phase = "guest-death-lifecycle";
    return result;
  }
  if (result.drop_expected) {
    if (!result.drop_observed) {
      result.stop_phase = "retail-drop-publication";
      return result;
    }
  }
  const auto presentation_complete =
      candidate.kind == ActorKind::common_hmd
          ? result.full_pose_samples >= 3U && result.pose_transition
          : result.rigid_presentation_samples >= 3U &&
                result.behavior_transition;
  const auto authored_behavior_complete =
      candidate.kind == ActorKind::common_hmd
          ? result.ai_transition && result.target_transition &&
                result.target_acquired
          : result.behavior_transition;
  result.completed = presentation_complete && result.stealth_observed &&
                     authored_behavior_complete && result.nonlethal_damage &&
                     result.death_observed &&
                     (!result.drop_expected || result.drop_observed);
  if (!result.completed) {
    result.stop_phase = "coverage-invariants";
    return result;
  }
  result.stop_phase = "ready";
  result.final_snapshot = vm.captureSnapshot();
  return result;
}

bool scenarioEqual(const ScenarioResult &left, const ScenarioResult &right) {
  return left.completed && right.completed && left.source == right.source &&
         left.room == right.room &&
         left.player_position.x == right.player_position.x &&
         left.player_position.y == right.player_position.y &&
         left.player_position.z == right.player_position.z &&
         left.player_room == right.player_room &&
         left.samples == right.samples &&
         left.impact_instructions == right.impact_instructions &&
         left.nonlethal_instructions == right.nonlethal_instructions &&
         left.lethal_instructions == right.lethal_instructions &&
         left.full_pose_samples == right.full_pose_samples &&
         left.rigid_presentation_samples == right.rigid_presentation_samples &&
         left.navigation_updates == right.navigation_updates &&
         left.natural_room_edges == right.natural_room_edges &&
         left.initial_player_health == right.initial_player_health &&
         left.minimum_player_health == right.minimum_player_health &&
         left.enemy_fire_samples == right.enemy_fire_samples &&
         left.stealth_observed == right.stealth_observed &&
         left.ai_transition == right.ai_transition &&
         left.target_transition == right.target_transition &&
         left.target_acquired == right.target_acquired &&
         left.pose_transition == right.pose_transition &&
         left.behavior_transition == right.behavior_transition &&
         left.nonlethal_damage == right.nonlethal_damage &&
         left.death_observed == right.death_observed &&
         left.drop_expected == right.drop_expected &&
         left.drop_observed == right.drop_observed &&
         replayStateEqual(left.final_snapshot, right.final_snapshot);
}

MissionResult probeMission(sf::game::GameDisc &disc,
                           const sf::game::MissionDefinition &mission) {
  auto result = MissionResult{};
  const auto package = sf::game::MissionPackage::load(disc, mission.index);
  if (mission.selection_index < 0) {
    result.stop_phase = "mission-selection";
    return result;
  }

  const auto &image = package.legacyImage();
  auto virtual_cd = image.createVirtualCd();
  sf::game::LegacyGameplayVm vm{image.executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(virtual_cd);
  const auto bootstrap = vm.bootstrapMission(
      static_cast<std::uint32_t>(mission.selection_index), mission.index == 0U,
      sf::game::syphonFilterUsaV11FirstMissionBootstrapProfile(),
      sf::game::syphonFilterUsaV11RetailPlatformTailProfile(),
      sf::game::syphonFilterUsaV11FirstMissionOpeningProfile(),
      bootstrap_budget);
  if (!bootstrap.completed() || !prepareFirstVisibleFrame(vm, mission.index)) {
    result.stop_phase = "bootstrap";
    return result;
  }

  auto control_updates = std::uint32_t{};
  if (!waitForActiveGameplay(vm, mission.index, control_updates)) {
    result.stop_phase = "active-gameplay";
    return result;
  }
  const auto mission_state = vm.readMissionBridgeState();
  const auto bridge = vm.readBridgeState();
  if (!mission_state || !bridge || mission_state->player_slot < 0 ||
      mission_state->terminal || !bridge->terrain_triggers_enabled) {
    result.stop_phase = "active-bridge";
    return result;
  }
  const auto calibration_snapshot = vm.captureSnapshot();
  const auto before_heading = sf::game::headingFromDirection(
      static_cast<double>(bridge->player.guest_rotation[2]),
      static_cast<double>(bridge->player.guest_rotation[8]));
  sf::game::LegacyHostPadState calibration_pad;
  calibration_pad.left_x = 0xffU;
  auto calibration_delta = std::int32_t{};
  for (std::uint32_t update = 0U; update < 16U && calibration_delta == 0;
       ++update) {
    if (!tick(vm, calibration_pad)) {
      result.stop_phase = "turn-calibration-tick";
      return result;
    }
    const auto calibration_bridge = vm.readBridgeState();
    if (calibration_bridge) {
      const auto after_heading = sf::game::headingFromDirection(
          static_cast<double>(calibration_bridge->player.guest_rotation[2]),
          static_cast<double>(calibration_bridge->player.guest_rotation[8]));
      calibration_delta = signedHeadingDelta(after_heading, before_heading);
    }
  }
  if (!vm.restoreSnapshot(calibration_snapshot) || calibration_delta == 0) {
    result.stop_phase = "turn-calibration-restore";
    return result;
  }
  const auto turn_direction = calibration_delta > 0 ? 1.0 : -1.0;

  auto candidates = actorCandidates(package, *bridge,
                                    mission_state->player_slot, mission.index);
  result.authored_actor_candidates = candidates.size();
  if (candidates.empty()) {
    // Fail closed: a mission is never silently classified actor-free merely
    // because the guest handler/definition bridge regressed.
    result.stop_phase = "no-retail-actor-candidates";
    return result;
  }

  if (candidates.size() == 1U) {
    const auto &candidate = candidates.front();
    const auto &actor = bridge->objects[candidate.source];
    const auto &authored = package.objects().objects()[candidate.source];
    const auto dormant =
        (actor.instance_state[3] & sf::game::legacy_instance_dormant) != 0U;
    const auto common_dormant =
        candidate.kind == ActorKind::common_hmd &&
        actor.object_handler == sf::game::legacy_common_npc_handler &&
        actor.bone_matrix_count == 0U;
    const auto rigid_dormant = candidate.kind == ActorKind::retail_rigid_boss &&
                               actor.object_handler != 0U &&
                               actor.bone_matrix_count != 0U;
    if (actor.definition == authored.type && actor.instance != 0U &&
        actor.ai_controller != 0U && actor.resident && actor.health > 0 &&
        !actor.simulated && actor.display_node == 0U &&
        (common_dormant || rigid_dormant)) {
      const auto contract_snapshot = vm.captureSnapshot();
      if (!tick(vm) || !tick(vm) || !vm.restoreSnapshot(contract_snapshot) ||
          !replayStateEqual(contract_snapshot, vm.captureSnapshot())) {
        result.stop_phase = "dormant-checkpoint-replay";
        return result;
      }
      const auto restored = vm.readBridgeState();
      if (!restored || candidate.source >= restored->objects.size() ||
          sampleActor(restored->objects[candidate.source]) !=
              sampleActor(actor)) {
        result.stop_phase = "dormant-actor-replay";
        return result;
      }
      result.completed = true;
      result.dormant_contract = true;
      result.candidate = candidate;
      result.first.source = candidate.source;
      result.first.room = candidate.room;
      result.first.samples.push_back(sampleActor(actor));
      result.stop_phase = dormant ? "dormant-ready" : "inactive-ready";
      return result;
    }
  }

  const auto checkpoint = vm.captureSnapshot();
  const auto attempts = std::min(candidates.size(), maximum_candidate_attempts);
  for (std::size_t candidate_index = 0U; candidate_index < attempts;
       ++candidate_index) {
    const auto &candidate = candidates[candidate_index];
    ++result.attempted_scenarios;
    if (!vm.restoreSnapshot(checkpoint)) {
      result.stop_phase = "checkpoint-restore-before-first";
      return result;
    }
    auto first = runScenario(vm, package, candidate, turn_direction);
    if (!first.completed) {
      result.stop_phase = first.stop_phase;
      result.candidate = candidate;
      result.first = std::move(first);
      continue;
    }
    if (!vm.restoreSnapshot(checkpoint)) {
      result.stop_phase = "checkpoint-restore-before-replay";
      return result;
    }
    auto replay = runScenario(vm, package, candidate, turn_direction);
    if (!scenarioEqual(first, replay)) {
      result.stop_phase =
          replay.completed ? "checkpoint-replay-diverged" : replay.stop_phase;
      continue;
    }
    result.candidate = candidate;
    result.first = std::move(first);
    result.replay = std::move(replay);
    result.completed = true;
    result.dormant_contract = result.first.stop_phase == "dormant-rigid-ready";
    result.stop_phase = "ready";
    return result;
  }
  return result;
}

int runProbe(const std::filesystem::path &cue_path,
             std::optional<std::uint32_t> only_mission) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "G3.4 AI/combat probe requires USA v1.1"};
  }
  const auto missions = sf::game::missionCatalog();
  if (missions.size() != retail_mission_count) {
    std::cerr << "G3.4 AI/combat gate requires all 20 missions: catalog="
              << missions.size() << '\n';
    return 2;
  }

  auto checked = std::size_t{};
  auto passed = std::size_t{};
  auto candidate_total = std::size_t{};
  auto pose_samples = std::size_t{};
  auto rigid_presentation_samples = std::size_t{};
  auto dormant_contracts = std::size_t{};
  for (const auto &mission : missions) {
    if (only_mission && mission.index != *only_mission) {
      continue;
    }
    ++checked;
    auto result = probeMission(disc, mission);
    candidate_total += result.authored_actor_candidates;
    if (result.completed) {
      ++passed;
      dormant_contracts += result.dormant_contract ? 1U : 0U;
      pose_samples += result.first.full_pose_samples;
      rigid_presentation_samples += result.first.rigid_presentation_samples;
    }
    std::cout << "mission=" << mission.index
              << " resource=" << mission.resource_name
              << " result=" << (result.completed ? "ready" : "failed")
              << " phase=" << result.stop_phase
              << " actors=" << result.authored_actor_candidates
              << " attempts=" << result.attempted_scenarios;
    if (result.completed && result.dormant_contract) {
      std::cout << " representative=" << result.candidate.source
                << " room=" << result.candidate.room << " kind="
                << (result.candidate.kind == ActorKind::common_hmd
                        ? "inactive-common-ai"
                        : "inactive-rigid-boss")
                << " controller=ready";
    } else if (result.completed) {
      std::cout << " representative=" << result.candidate.source
                << " room=" << result.candidate.room << " pose-parts="
                << static_cast<unsigned int>(result.candidate.pose_parts)
                << " kind="
                << (result.candidate.kind == ActorKind::common_hmd
                        ? "common-hmd"
                        : "retail-rigid-boss")
                << " samples=" << result.first.samples.size()
                << " full-pose=" << result.first.full_pose_samples
                << " rigid-presentation="
                << result.first.rigid_presentation_samples
                << " navigation=" << result.first.navigation_updates
                << " room-edges=" << result.first.natural_room_edges
                << " player-hp=" << result.first.initial_player_health << "->"
                << result.first.minimum_player_health
                << " enemy-fire-samples=" << result.first.enemy_fire_samples
                << " impact/damage/death-instructions="
                << result.first.impact_instructions << '/'
                << result.first.nonlethal_instructions << '/'
                << result.first.lethal_instructions << " drop="
                << result.first.drop_observed << " replay=exact";
    } else if (!result.first.samples.empty()) {
      const auto &sample = result.first.samples.back();
      std::cout << " last-source=" << result.candidate.source
                << " room=" << result.candidate.room
                << " nav=" << result.first.navigation_updates
                << " player-room=" << result.first.player_room << " player=("
                << result.first.player_position.x << ','
                << result.first.player_position.z << ") instance=0x" << std::hex
                << sample.instance << " display=0x" << sample.display_node
                << " ai=0x" << sample.ai_controller << " target=0x"
                << sample.target_controller << std::dec
                << " simulated=" << sample.simulated
                << " resident=" << sample.resident
                << " bones=" << static_cast<unsigned>(sample.bone_matrix_count)
                << " hp=" << sample.health
                << " player-hp=" << result.first.initial_player_health << "->"
                << result.first.minimum_player_health
                << " enemy-fire-samples=" << result.first.enemy_fire_samples
                << " damage=" << result.first.nonlethal_damage
                << " acquired=" << result.first.target_acquired
                << " ai-transition=" << result.first.ai_transition
                << " target-transition=" << result.first.target_transition
                << " pose-transition=" << result.first.pose_transition;
    }
    std::cout << '\n';
  }

  const auto expected = only_mission ? std::size_t{1U} : retail_mission_count;
  if (checked != expected || passed != checked || candidate_total == 0U ||
      pose_samples + rigid_presentation_samples + dormant_contracts == 0U) {
    std::cerr << "G3.4 AI/combat gate failed: passed=" << passed << '/'
              << checked << " expected=" << expected
              << " actor-candidates=" << candidate_total
              << " pose-samples=" << pose_samples
              << " rigid-presentation-samples=" << rigid_presentation_samples
              << " inactive-contracts=" << dormant_contracts << '\n';
    return 2;
  }
  std::cout << "G3.4 AI/combat gate passed: missions=" << passed << '/'
            << checked << " actor-candidates=" << candidate_total
            << " pose-samples=" << pose_samples
            << " rigid-presentation-samples=" << rigid_presentation_samples
            << " inactive-contracts=" << dormant_contracts
            << " coverage=live-guest-combat+retail-rigid-boss"
               "+inactive-controller"
               " checkpoint-replay=exact host-spawn=0"
               " event6=0 teleport=0 direct-ram=0\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: sf_g3_ai_combat_probe <game.cue> [mission-index]\n";
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
          mission >= retail_mission_count) {
        std::cerr << "Invalid mission index\n";
        return 1;
      }
      only_mission = mission;
    }
    return runProbe(std::filesystem::path{argv[1]}, only_mission);
  } catch (const std::exception &error) {
    std::cerr << "G3.4 AI/combat gate failed: " << error.what() << '\n';
    return 10;
  }
}
