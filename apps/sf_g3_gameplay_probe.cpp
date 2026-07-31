#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/legacy_mission_image.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t objective_reveal_entry = 0x80017ed8U;
constexpr std::uint32_t objective_complete_entry = 0x80017c24U;
constexpr std::uint32_t mission_success_entry = 0x80017698U;
constexpr std::uint32_t mission_failure_entry = 0x80017970U;
constexpr std::uint32_t mission_failure_transition_entry = 0x80017890U;
constexpr std::uint32_t centered_notice_entry = 0x80017530U;
constexpr std::uint32_t status_notice_entry = 0x80085d04U;
constexpr std::uint64_t bootstrap_budget = 500'000'000U;
constexpr std::uint64_t common_call_budget = 5'000'000U;
constexpr std::uint32_t maximum_control_wait_updates = 2'000U;
constexpr std::uint32_t stable_control_updates = 8U;
constexpr std::size_t opening_camera_source = 35U;
constexpr std::uint32_t activate_dynamic_descriptor_entry = 0x8005fd04U;
constexpr std::uint32_t opening_dynamic_descriptor = 6U;

enum class EventKind : std::uint8_t { none, interaction, impact };

struct RouteSpec {
  std::array<std::uint16_t, 8U> rooms{};
  std::uint8_t room_count{};
  std::int16_t event_source{-1};
  EventKind event_kind{EventKind::none};
  std::uint16_t expected_event_class{};
  std::array<std::uint16_t, 2U> required_passages{
      std::numeric_limits<std::uint16_t>::max(),
      std::numeric_limits<std::uint16_t>::max()};
};

constexpr auto no_source = std::numeric_limits<std::uint16_t>::max();

// Exact BIN slots and DAT-connected routes for all twenty USA v1.1 campaign
// entries. SUBWAY3 and WHOUSE2 intentionally have no extracted deterministic
// interaction yet; their gate is the real room-stream lifecycle boundary.
constexpr std::array route_specs{
    RouteSpec{{73U, 82U}, 2U, 64, EventKind::interaction, 0x73U, {64U, 65U}},
    RouteSpec{{19U, 16U, 12U, 1U}, 4U, 205, EventKind::interaction, 0x7bU},
    RouteSpec{{2U, 1U}, 2U},
    RouteSpec{{2U, 1U}, 2U, 122, EventKind::impact, 0x15U},
    RouteSpec{{1U, 2U}, 2U, 3, EventKind::impact, 0x33U},
    RouteSpec{
        {0U, 3U, 4U, 7U, 8U, 9U, 13U}, 7U, 239, EventKind::interaction, 0x1cU},
    RouteSpec{{0U, 1U}, 2U, 0, EventKind::impact, 0x13U},
    RouteSpec{{7U, 10U, 15U}, 3U, 81, EventKind::interaction, 0x1cU},
    RouteSpec{{11U, 12U}, 2U, 302, EventKind::interaction, 0x24U},
    RouteSpec{{1U, 2U}, 2U, 5, EventKind::impact, 0x28U},
    RouteSpec{{24U, 27U}, 2U, 147, EventKind::interaction, 0x1dU},
    RouteSpec{{47U, 9U, 16U}, 3U, 69, EventKind::interaction, 0x1cU},
    RouteSpec{{25U, 52U, 49U, 34U, 9U, 40U, 43U},
              7U,
              53,
              EventKind::interaction,
              0x1cU},
    RouteSpec{{28U, 29U, 44U, 47U, 42U}, 5U, 5, EventKind::interaction, 0x6aU},
    RouteSpec{{19U, 48U, 41U, 51U}, 4U, 91, EventKind::impact, 0x15U},
    RouteSpec{{28U, 15U}, 2U},
    RouteSpec{
        {0U, 19U, 22U, 24U, 29U, 33U}, 6U, 279, EventKind::interaction, 0x1bU},
    RouteSpec{{0U, 23U, 17U}, 3U, 208, EventKind::interaction, 0x1cU},
    RouteSpec{{8U, 1U}, 2U, 45, EventKind::interaction, 0x73U, {45U, 46U}},
    RouteSpec{{0U, 1U}, 2U, 138, EventKind::interaction, 0x24U},
};
static_assert(route_specs.size() == 20U);

enum class MessageChannel : std::uint8_t { centered, status };

struct MessageEvent {
  MessageChannel channel{};
  std::string text;
  std::uint32_t duration{};

  [[nodiscard]] friend bool operator==(const MessageEvent &,
                                       const MessageEvent &) = default;
};

class MessageObserver final {
public:
  void bind(sf::game::LegacyGameplayVm &vm) {
    const auto bind = [this, &vm](std::uint32_t address,
                                  MessageChannel channel) {
      vm.bindHostCall(
          address, [this, channel](sf::game::LegacyHostCallContext &context) {
            std::string value;
            if (!context.readCString(context.argument(0U), value) ||
                value.empty()) {
              malformed_ = true;
            } else {
              events_.push_back(MessageEvent{channel, std::move(value),
                                             context.argument(1U)});
            }
            context.continueGuestInstruction();
          });
    };
    bind(centered_notice_entry, MessageChannel::centered);
    bind(status_notice_entry, MessageChannel::status);
  }

  void clear() noexcept {
    events_.clear();
    malformed_ = false;
  }

  [[nodiscard]] bool malformed() const noexcept { return malformed_; }
  [[nodiscard]] const std::vector<MessageEvent> &events() const noexcept {
    return events_;
  }

private:
  std::vector<MessageEvent> events_;
  bool malformed_{};
};

bool replayStateEqual(const sf::game::LegacyGameplayVmSnapshot &left,
                      const sf::game::LegacyGameplayVmSnapshot &right) {
  const auto delayed_load_equal = [](const sf::psx::R3000DelayedLoadState &a,
                                     const sf::psx::R3000DelayedLoadState &b) {
    return a.reg == b.reg && a.value == b.value && a.valid == b.valid;
  };
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
      delayed_load_equal(left.cpu.load_delay, right.cpu.load_delay) &&
      delayed_load_equal(left.cpu.next_load_delay, right.cpu.next_load_delay);
  const auto scheduler_equal = [&] {
    if (left.machine.cpu_clock_scale != right.machine.cpu_clock_scale ||
        left.machine.scheduler.now != right.machine.scheduler.now ||
        left.machine.scheduler.next_token !=
            right.machine.scheduler.next_token ||
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
    return true;
  }();
  const auto interrupts_equal =
      left.machine.interrupts.status == right.machine.interrupts.status &&
      left.machine.interrupts.mask == right.machine.interrupts.mask &&
      left.machine.interrupts.input_lines ==
          right.machine.interrupts.input_lines;
  const auto spu_equal = left.machine.spu && right.machine.spu
                             ? *left.machine.spu == *right.machine.spu
                             : !left.machine.spu && !right.machine.spu;
  const auto virtual_cd_equal = left.virtual_cd && right.virtual_cd
                                    ? *left.virtual_cd == *right.virtual_cd
                                    : !left.virtual_cd && !right.virtual_cd;
  return cpu_equal && left.ram == right.ram &&
         left.scratchpad == right.scratchpad && left.mmio == right.mmio &&
         scheduler_equal && interrupts_equal &&
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

bool missionStateEqual(const sf::game::LegacyMissionBridgeState &left,
                       const sf::game::LegacyMissionBridgeState &right) {
  return left.player_slot == right.player_slot &&
         left.player_health == right.player_health &&
         left.player_armor == right.player_armor &&
         left.objective_count == right.objective_count &&
         left.parameter_count == right.parameter_count &&
         left.objective_texts == right.objective_texts &&
         left.parameter_texts == right.parameter_texts &&
         left.completed_objectives == right.completed_objectives &&
         left.failed_objectives == right.failed_objectives &&
         left.revealed_objectives == right.revealed_objectives &&
         left.notified_objectives == right.notified_objectives &&
         left.failed_parameters == right.failed_parameters &&
         left.parameter_mask == right.parameter_mask &&
         left.weapon_menu_state == right.weapon_menu_state &&
         left.weapon_menu_dirty == right.weapon_menu_dirty &&
         left.weapon_menu_controller_ready ==
             right.weapon_menu_controller_ready &&
         left.weapon_menu_input_ready == right.weapon_menu_input_ready &&
         left.inventory.current_weapon == right.inventory.current_weapon &&
         left.inventory.owned_weapons == right.inventory.owned_weapons &&
         left.inventory.magazines == right.inventory.magazines &&
         left.inventory.reserves == right.inventory.reserves &&
         left.success == right.success && left.terminal == right.terminal &&
         left.failure == right.failure &&
         left.failure_transition == right.failure_transition &&
         left.messages == right.messages && left.timer == right.timer;
}

bool missionProgressEqual(const sf::game::LegacyMissionBridgeState &left,
                          const sf::game::LegacyMissionBridgeState &right) {
  return left.completed_objectives == right.completed_objectives &&
         left.failed_objectives == right.failed_objectives &&
         left.revealed_objectives == right.revealed_objectives &&
         left.notified_objectives == right.notified_objectives &&
         left.failed_parameters == right.failed_parameters &&
         left.parameter_mask == right.parameter_mask &&
         left.success == right.success && left.terminal == right.terminal &&
         left.failure == right.failure;
}

std::uint32_t entryMask(std::uint32_t count) noexcept {
  if (count >= 32U) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  return count == 0U ? 0U : (1U << count) - 1U;
}

bool missionInvariants(const sf::game::LegacyMissionBridgeState &state) {
  if (state.objective_count == 0U ||
      state.objective_count > sf::game::legacy_mission_entry_limit ||
      state.parameter_count > sf::game::legacy_mission_entry_limit ||
      state.objective_texts.size() != state.objective_count ||
      state.parameter_texts.size() != state.parameter_count ||
      std::ranges::any_of(state.objective_texts,
                          [](const auto &text) { return text.empty(); }) ||
      std::ranges::any_of(state.parameter_texts,
                          [](const auto &text) { return text.empty(); })) {
    return false;
  }
  const auto objective_mask = entryMask(state.objective_count);
  const auto parameter_mask = entryMask(state.parameter_count);
  if (((state.completed_objectives | state.failed_objectives |
        state.revealed_objectives | state.notified_objectives) &
       ~objective_mask) != 0U ||
      ((state.failed_parameters | state.parameter_mask) & ~parameter_mask) !=
          0U ||
      (state.notified_objectives & ~state.revealed_objectives) != 0U ||
      (state.success && state.failure) ||
      ((state.success || state.failure) && !state.terminal) ||
      (state.failure_transition && !state.failure)) {
    return false;
  }
  return true;
}

std::uint64_t
guestIdentity(const sf::game::LegacyObjectBridgeState &guest) noexcept {
  auto value =
      static_cast<std::uint64_t>(guest.definition) |
      (static_cast<std::uint64_t>(static_cast<std::uint16_t>(guest.class_id))
       << 32U);
  const auto mix = [&value](std::uint32_t component) {
    value ^= static_cast<std::uint64_t>(component) + 0x9e3779b97f4a7c15ULL +
             (value << 6U) + (value >> 2U);
  };
  mix(static_cast<std::uint32_t>(guest.authored_position.x));
  mix(static_cast<std::uint32_t>(guest.authored_position.y));
  mix(static_cast<std::uint32_t>(guest.authored_position.z));
  mix(guest.path_pointer);
  mix(static_cast<std::uint32_t>(guest.attributes));
  mix(static_cast<std::uint32_t>(guest.parameter));
  mix(static_cast<std::uint32_t>(guest.linked_slot));
  return value == 0U ? 1U : value;
}

struct LifetimeStats {
  std::size_t observations{};
  std::size_t active_observations{};
  std::size_t identity_edges{};
  std::size_t spawns{};
  std::size_t despawns{};
  std::size_t reuses{};
  bool valid{true};

  [[nodiscard]] friend bool operator==(const LifetimeStats &,
                                       const LifetimeStats &) = default;
};

struct ActorCoverageStats {
  std::size_t live_npc_samples{};
  std::size_t presented_npc_pose_samples{};
  std::array<std::size_t, 4U> special_appearances{};
  std::array<std::size_t, 4U> special_presented_poses{};
  bool valid{true};

  [[nodiscard]] friend bool operator==(const ActorCoverageStats &,
                                       const ActorCoverageStats &) = default;
};

class ActorCoverageTracker final {
public:
  void observe(const sf::game::LegacyGameplayBridgeState &state) noexcept {
    constexpr std::array<std::int16_t, 4U> special_classes{0x02, 0x4c, 0x5c,
                                                           0x65};
    for (const auto &object : state.objects) {
      const auto presented =
          object.resident && (object.presentation_controller == 0U ||
                              object.presentation_enabled != 0U);
      const auto full_hmd_pose =
          object.bone_matrix_count == sf::game::legacy_actor_bone_count;
      const auto retail_npc =
          object.object_handler == sf::game::legacy_common_npc_handler &&
          object.ai_controller != 0U;
      if (retail_npc && object.alive()) {
        ++stats_.live_npc_samples;
        if (presented && full_hmd_pose) {
          ++stats_.presented_npc_pose_samples;
        }
      }

      const auto special = std::ranges::find(special_classes, object.class_id);
      if (special == special_classes.end() ||
          object.object_handler != sf::game::legacy_common_npc_handler ||
          (!object.resident && !object.simulated)) {
        continue;
      }
      const auto index = static_cast<std::size_t>(
          std::distance(special_classes.begin(), special));
      ++stats_.special_appearances[index];
      if (presented && full_hmd_pose) {
        ++stats_.special_presented_poses[index];
      }
    }
  }

  [[nodiscard]] const ActorCoverageStats &stats() const noexcept {
    return stats_;
  }

private:
  ActorCoverageStats stats_;
};

class LifetimeTracker final {
public:
  bool observe(const sf::game::LegacyGameplayBridgeState &state) {
    if (!state.terrain_triggers_enabled) {
      stats_.valid = false;
      return false;
    }
    if (!initialized_) {
      current_.resize(state.objects.size());
      last_.resize(state.objects.size());
      dynamic_first_slot_ = state.dynamic_first_slot;
      initialized_ = true;
    }
    if (state.objects.size() != current_.size() ||
        state.dynamic_first_slot != dynamic_first_slot_ ||
        dynamic_first_slot_ > state.objects.size()) {
      stats_.valid = false;
      return false;
    }

    std::unordered_set<std::uint64_t> active_identities;
    for (std::size_t slot = 0U; slot < state.objects.size(); ++slot) {
      const auto &object = state.objects[slot];
      if (object.slot != slot) {
        stats_.valid = false;
        return false;
      }
      if (slot < dynamic_first_slot_) {
        continue;
      }

      auto identity = std::uint64_t{};
      if (object.path_pointer == 0U) {
        if (object.resident || object.simulated) {
          stats_.valid = false;
          return false;
        }
      } else {
        identity = guestIdentity(object);
        ++stats_.active_observations;
        if (!active_identities.insert(identity).second) {
          stats_.valid = false;
          return false;
        }
      }

      if (stats_.observations != 0U && current_[slot] != identity) {
        ++stats_.identity_edges;
        if (current_[slot] != 0U && identity == 0U) {
          ++stats_.despawns;
        } else if (current_[slot] == 0U && identity != 0U) {
          ++stats_.spawns;
          if (last_[slot] != 0U && last_[slot] != identity) {
            ++stats_.reuses;
          }
        } else if (current_[slot] != 0U && identity != 0U) {
          ++stats_.reuses;
        }
      }
      current_[slot] = identity;
      if (identity != 0U) {
        last_[slot] = identity;
      }
    }
    ++stats_.observations;
    return true;
  }

  [[nodiscard]] const LifetimeStats &stats() const noexcept { return stats_; }

private:
  std::vector<std::uint64_t> current_;
  std::vector<std::uint64_t> last_;
  std::size_t dynamic_first_slot_{};
  LifetimeStats stats_;
  bool initialized_{};
};

bool sameLifecycle(const sf::game::LegacyObjectBridgeState &left,
                   const sf::game::LegacyObjectBridgeState &right) {
  return left.definition == right.definition &&
         left.class_id == right.class_id &&
         left.path_pointer == right.path_pointer &&
         left.instance == right.instance && left.root_node == right.root_node &&
         left.motion_controller == right.motion_controller &&
         left.presentation_controller == right.presentation_controller &&
         left.presentation_enabled == right.presentation_enabled &&
         left.presentation_mode == right.presentation_mode &&
         left.instance_flags == right.instance_flags &&
         left.instance_state == right.instance_state &&
         left.resident == right.resident && left.simulated == right.simulated &&
         left.health == right.health && left.position.x == right.position.x &&
         left.position.y == right.position.y &&
         left.position.z == right.position.z;
}

std::size_t lifecycleEdges(const sf::game::LegacyGameplayBridgeState &before,
                           const sf::game::LegacyGameplayBridgeState &after) {
  const auto count = std::min(before.objects.size(), after.objects.size());
  auto edges = std::size_t{};
  for (std::size_t slot = 0U; slot < count; ++slot) {
    if (before.objects[slot].class_id == 0 &&
        after.objects[slot].class_id == 0) {
      continue;
    }
    edges += sameLifecycle(before.objects[slot], after.objects[slot]) ? 0U : 1U;
  }
  return edges + (before.objects.size() == after.objects.size() ? 0U : 1U);
}

bool adjacent(const sf::game::MissionPackage &mission, std::uint16_t left,
              std::uint16_t right) {
  const auto contains = [](const auto &models, std::uint16_t room) {
    return std::ranges::find(models, room) != models.end();
  };
  return contains(mission.layout().visibility(left).active_models, right) ||
         contains(mission.layout().visibility(right).active_models, left);
}

bool validateSpec(const sf::game::MissionPackage &mission,
                  const RouteSpec &spec) {
  if (spec.room_count < 2U || spec.room_count > spec.rooms.size() ||
      spec.rooms[0] != mission.layout().initialRoom()) {
    return false;
  }
  for (std::size_t index = 1U; index < spec.room_count; ++index) {
    if (spec.rooms[index] >= mission.layout().modelCount() ||
        !adjacent(mission, spec.rooms[index - 1U], spec.rooms[index])) {
      return false;
    }
  }
  for (const auto source : spec.required_passages) {
    if (source == no_source) {
      continue;
    }
    if (source >= mission.objects().objects().size()) {
      return false;
    }
    const auto &object = mission.objects().objects()[source];
    if (mission.objects().definition(object.type).class_id != 0x73U ||
        object.patrol_path.empty()) {
      return false;
    }
  }
  return true;
}

bool tick(sf::game::LegacyGameplayVm &vm) {
  if (!vm.writeHostPadState({})) {
    return false;
  }
  const auto frame = vm.tickRetailOuterFrame();
  return frame.completed() && vm.advanceAudioFrameClock();
}

bool prepareFirstVisibleFrame(sf::game::LegacyGameplayVm &vm,
                              std::uint32_t mission_index) {
  // This is the exact production LegacyFirstMissionRuntime boundary. The host
  // skips SUBWAY's frontend callback, so production activates retail dynamic
  // descriptor 6 before advancing the queued first visible frame.
  if (mission_index == 0U) {
    constexpr std::array activation_arguments{opening_dynamic_descriptor};
    const auto activation = vm.invoke(activate_dynamic_descriptor_entry,
                                      activation_arguments, common_call_budget);
    if (!activation.completed()) {
      return false;
    }
  }
  return tick(vm);
}

std::optional<std::uint16_t> currentRoom(sf::game::LegacyGameplayVm &vm);

struct NaturalLifetimeResult {
  bool completed{};
  std::uint8_t stop_phase{};
  LifetimeStats lifetime;
  ActorCoverageStats actors;
  sf::game::LegacyGameplayVmSnapshot snapshot;
};

NaturalLifetimeResult runNaturalLifetimeScenario(sf::game::LegacyGameplayVm &vm,
                                                 std::uint32_t mission_index) {
  NaturalLifetimeResult result;
  LifetimeTracker tracker;
  ActorCoverageTracker actors;
  if (!prepareFirstVisibleFrame(vm, mission_index)) {
    result.stop_phase = 1U;
    return result;
  }
  const auto initial = vm.readBridgeState();
  if (!initial || !tracker.observe(*initial)) {
    result.stop_phase = 2U;
    return result;
  }
  actors.observe(*initial);
  constexpr std::uint32_t neutral_updates = 8U;
  for (std::uint32_t update = 0U; update < neutral_updates; ++update) {
    if (!tick(vm)) {
      result.stop_phase = 3U;
      return result;
    }
    const auto bridge = vm.readBridgeState();
    if (!bridge || !tracker.observe(*bridge)) {
      result.stop_phase = 4U;
      return result;
    }
    actors.observe(*bridge);
  }
  result.lifetime = tracker.stats();
  result.actors = actors.stats();
  result.completed = result.lifetime.valid &&
                     result.lifetime.observations == neutral_updates + 1U &&
                     result.actors.valid;
  result.snapshot = vm.captureSnapshot();
  return result;
}

bool naturalLifetimeResultEqual(const NaturalLifetimeResult &left,
                                const NaturalLifetimeResult &right) {
  return left.completed && right.completed && left.lifetime == right.lifetime &&
         left.actors == right.actors &&
         replayStateEqual(left.snapshot, right.snapshot);
}

bool naturalLifetimeMatches(std::uint32_t mission_index,
                            const LifetimeStats &lifetime) noexcept {
  if (!lifetime.valid || lifetime.observations != 9U) {
    return false;
  }
  switch (mission_index) {
  case 0U:
    return lifetime.identity_edges == 2U && lifetime.active_observations == 9U;
  case 14U:
    return lifetime.identity_edges == 3U && lifetime.active_observations == 10U;
  case 17U:
    return lifetime.identity_edges == 2U && lifetime.active_observations == 9U;
  default:
    return lifetime.identity_edges == 0U && lifetime.active_observations == 0U;
  }
}

std::string_view naturalLifetimeCoverage(std::uint32_t mission_index) noexcept {
  return mission_index == 0U || mission_index == 14U || mission_index == 17U
             ? "natural-dynamic-short"
             : "no-short-natural-dynamic";
}

struct StartVisualResult {
  bool completed{};
  std::int16_t room{-1};
  std::int32_t projection{};
  std::int32_t fov_raw{};
  std::array<std::int32_t, 6U> camera{};
  std::uint16_t fade_current{};
  std::uint8_t fade_floor{};
  std::vector<std::uint16_t> active_models;
  sf::game::LegacyGameplayVmSnapshot snapshot;
};

StartVisualResult
runStartVisualScenario(sf::game::LegacyGameplayVm &vm,
                       const sf::game::MissionPackage &mission_package) {
  StartVisualResult result;
  const auto bridge = vm.readBridgeState();
  const auto mission = vm.readMissionBridgeState();
  const auto room = currentRoom(vm);
  if (!bridge || !mission || !room || !missionInvariants(*mission) ||
      mission->terminal || bridge->player.room < 0 ||
      static_cast<std::uint16_t>(bridge->player.room) != *room) {
    return result;
  }
  if (!sf::game::validateLegacyWorldModelSets(
          *bridge, mission_package.layout().modelCount())) {
    return result;
  }

  result.room = bridge->player.room;
  result.projection = bridge->camera.projectionForDisplayWidth(320);
  result.fov_raw = bridge->camera.fov_raw;
  result.camera = {bridge->camera.eye.x,    bridge->camera.eye.y,
                   bridge->camera.eye.z,    bridge->camera.target.x,
                   bridge->camera.target.y, bridge->camera.target.z};
  result.fade_current = bridge->fade.current;
  result.fade_floor = bridge->fade.floor;
  result.active_models = bridge->active_world_models;
  const auto camera_has_extent =
      bridge->camera.eye.x != bridge->camera.target.x ||
      bridge->camera.eye.y != bridge->camera.target.y ||
      bridge->camera.eye.z != bridge->camera.target.z;
  const auto current_visible = std::ranges::find(result.active_models, *room) !=
                               result.active_models.end();
  result.completed = camera_has_extent && result.projection > 0 &&
                     result.projection < 0xffff && result.fov_raw > 0 &&
                     result.fov_raw < 2048 &&
                     bridge->fade.blackOpacity() == 0.0 &&
                     !result.active_models.empty() && current_visible;
  result.snapshot = vm.captureSnapshot();
  return result;
}

bool startVisualResultEqual(const StartVisualResult &left,
                            const StartVisualResult &right) {
  return left.completed && right.completed && left.room == right.room &&
         left.projection == right.projection && left.fov_raw == right.fov_raw &&
         left.camera == right.camera &&
         left.fade_current == right.fade_current &&
         left.fade_floor == right.fade_floor &&
         left.active_models == right.active_models &&
         replayStateEqual(left.snapshot, right.snapshot);
}

struct ProductionActorResult {
  bool completed{};
  std::uint32_t control_wait{};
  std::size_t expected_npc_samples{};
  std::size_t actual_npc_samples{};
  std::size_t variable_hmd_pose_samples{};
  std::array<std::size_t, 3U> special_prop_samples{};
  std::vector<std::uint64_t> frame_digests;

  [[nodiscard]] friend bool operator==(const ProductionActorResult &,
                                       const ProductionActorResult &) = default;
};

bool productionControlReady(
    const sf::game::GameplaySession &gameplay) noexcept {
  const auto frame = gameplay.legacyPresentationFrame();
  if (!frame || !frame->renderer || !gameplay.legacyOpeningFinished()) {
    return false;
  }
  const auto &state = frame->renderer->state;
  return state.player.resident && !state.player.control_locked &&
         !state.camera.scripted && !state.camera.locked;
}

bool observeProductionActors(const sf::game::MissionPackage &mission,
                             const sf::game::GameplaySession &gameplay,
                             ProductionActorResult &result) {
  const auto frame = gameplay.legacyPresentationFrame();
  if (gameplay.runtimeFaulted() || !frame || !frame->renderer ||
      frame->sequence != gameplay.legacyPresentationSequence()) {
    return false;
  }
  const auto &guest = frame->renderer->state;
  const auto bindings = gameplay.legacyGuestSlotsBySceneObject();
  if (bindings.size() != gameplay.objects().size()) {
    return false;
  }

  auto digest = std::uint64_t{1469598103934665603ULL};
  const auto mix = [&digest](std::uint64_t value) {
    digest ^= value;
    digest *= 1099511628211ULL;
  };
  mix(frame->sequence);
  mix(frame->guest_frame);
  constexpr std::array<std::int16_t, 3U> special_props{0x03, 0x3c, 0x59};
  for (std::size_t scene = 0U; scene < bindings.size(); ++scene) {
    const auto slot = bindings[scene];
    if (scene > std::numeric_limits<std::uint16_t>::max() ||
        scene >= gameplay.objects().size()) {
      return false;
    }
    const auto actual_npc =
        gameplay.npcState(static_cast<std::uint16_t>(scene)) != nullptr;
    auto expected_npc = false;
    if (slot >= 0) {
      if (static_cast<std::size_t>(slot) >= guest.objects.size()) {
        return false;
      }
      const auto &object = guest.objects[static_cast<std::size_t>(slot)];
      const auto &scene_object = gameplay.objects()[scene];
      if (scene_object.model >= gameplay.objectModels().size()) {
        return false;
      }
      const auto retail_dormant =
          (object.instance_state[3] & sf::game::legacy_instance_dormant) != 0U;
      const auto *native_model =
          gameplay.displayedObjectModel(static_cast<std::uint16_t>(scene));
      const auto *hmd_model = std::get_if<sf::assets::HmdModel>(
          &gameplay.objectModels()[scene_object.model].geometry);
      const auto hmd_backed = hmd_model != nullptr;
      const auto actor = sf::game::legacyPresentationUsesRetailNpc(
          hmd_backed, object.object_handler, object.ai_controller);
      const auto static_presentation_allowed =
          !actor && sf::game::legacyGuestStaticPropPresentationAllowed(
                        retail_dormant, object.destroyed(),
                        scene_object.destroyed_model.has_value(),
                        scene_object.damage_response);
      if (retail_dormant && native_model != nullptr &&
          !static_presentation_allowed) {
        return false;
      }
      const auto exact_guest_pose =
          hmd_model != nullptr &&
          sf::game::legacyGuestHmdPoseComplete(object.bone_matrix_count,
                                               hmd_model->parts().size());
      const auto presented =
          object.resident && (object.presentation_controller == 0U ||
                              object.presentation_enabled != 0U);
      const auto source_streamed =
          std::ranges::any_of(gameplay.activeModels(), [&](std::uint16_t room) {
            const auto sources = mission.objects().objectsInRoom(room);
            return std::ranges::find(sources, scene_object.source_index) !=
                   sources.end();
          });
      const auto live_position_streamed =
          std::ranges::any_of(gameplay.activeModels(), [&](std::uint16_t room) {
            if (room >= gameplay.models().size()) {
              return false;
            }
            const auto &bounds = gameplay.models()[room].bounds;
            return object.position.x >= bounds.minimum_x &&
                   object.position.x <= bounds.maximum_x &&
                   object.position.z >= bounds.minimum_z &&
                   object.position.z <= bounds.maximum_z;
          });
      const auto stream_visible = sf::game::legacyGuestActorStreamVisible(
          source_streamed, live_position_streamed, object.pose_flags, false,
          object.simulated, retail_dormant, exact_guest_pose);
      expected_npc =
          hmd_backed &&
          object.object_handler == sf::game::legacy_common_npc_handler &&
          object.ai_controller != 0U && presented && stream_visible;
      if (hmd_backed && native_model != nullptr &&
          (!exact_guest_pose ||
           scene_object.legacy_hmd_bone_count < hmd_model->parts().size())) {
        return false;
      }
      if (expected_npc &&
          (scene_object.legacy_hmd_bone_count != object.bone_matrix_count ||
           scene_object.legacy_hmd_bone_count < hmd_model->parts().size() ||
           scene_object.legacy_hmd_root_space)) {
        return false;
      }
      if (expected_npc &&
          object.bone_matrix_count != sf::game::legacy_actor_bone_count) {
        ++result.variable_hmd_pose_samples;
      }
      const auto special = std::ranges::find(special_props, object.class_id);
      if (special != special_props.end() && native_model != nullptr) {
        const auto index = static_cast<std::size_t>(
            std::distance(special_props.begin(), special));
        ++result.special_prop_samples[index];
        if (actual_npc) {
          return false;
        }
      }
      mix(object.object_handler);
      mix(object.ai_controller);
      mix(object.bone_matrix_count);
      mix(static_cast<std::uint16_t>(object.class_id));
      mix(presented);
    }
    if (expected_npc) {
      ++result.expected_npc_samples;
    }
    if (actual_npc) {
      ++result.actual_npc_samples;
    }
    if (actual_npc != expected_npc) {
      return false;
    }
    mix(static_cast<std::uint32_t>(slot));
    mix(expected_npc);
    mix(actual_npc);
  }
  result.frame_digests.push_back(digest);
  return true;
}

ProductionActorResult
runProductionActorScenario(const sf::game::MissionPackage &mission) {
  ProductionActorResult result;
  auto gameplay = std::make_unique<sf::game::GameplaySession>(mission);
  const auto live_volumes = gameplay->audioVolumes();
  if (!live_volumes || !gameplay->setAudioVolumes(*live_volumes)) {
    std::cerr << "production retail audio-volume apply failed\n";
    return result;
  }
  auto stable_control = std::uint32_t{};
  while (stable_control < stable_control_updates &&
         result.control_wait < maximum_control_wait_updates) {
    gameplay->update({});
    if (!gameplay->advanceAudioFrameClock()) {
      std::cerr << "production audio/hardware clock stopped\n";
      return result;
    }
    ++result.control_wait;
    if (gameplay->runtimeFaulted()) {
      std::cerr << "production runtime fault: "
                << gameplay->runtimeFaultReason() << '\n';
      return result;
    }
    stable_control =
        productionControlReady(*gameplay) ? stable_control + 1U : 0U;
  }
  if (stable_control != stable_control_updates) {
    return result;
  }
  constexpr std::uint32_t factual_frames = 8U;
  for (std::uint32_t frame = 0U; frame < factual_frames; ++frame) {
    gameplay->update({});
    if (!gameplay->advanceAudioFrameClock()) {
      std::cerr << "production factual-frame audio/hardware clock stopped\n";
      return result;
    }
    if (!observeProductionActors(mission, *gameplay, result)) {
      return result;
    }
  }
  result.completed = true;
  return result;
}

bool runProductionRendererStress(const sf::game::MissionPackage &mission,
                                 std::uint32_t updates) {
  auto gameplay = std::make_unique<sf::game::GameplaySession>(mission);
  for (std::uint32_t update = 0U; update < updates; ++update) {
    gameplay->update({});
    if (!gameplay->advanceAudioFrameClock()) {
      std::cerr << "production renderer stress audio/hardware clock stopped: "
                << "update=" << update + 1U << '\n';
      return false;
    }
    if (gameplay->runtimeFaulted()) {
      const auto frame = gameplay->legacyPresentationFrame();
      std::cerr << "production renderer stress fault: update=" << update + 1U
                << " reason=" << gameplay->runtimeFaultReason()
                << " detail=" << gameplay->runtimeFaultDetail()
                << " guest-frame=" << (frame ? frame->guest_frame : 0U)
                << " sequence=" << gameplay->legacyPresentationSequence()
                << '\n';
      return false;
    }
  }
  return true;
}

struct ActiveGameplayWaitResult {
  bool completed{};
  std::uint32_t updates{};
};

ActiveGameplayWaitResult waitForActiveGameplay(sf::game::LegacyGameplayVm &vm,
                                               std::uint32_t mission_index) {
  ActiveGameplayWaitResult result;
  auto stable_control = std::uint32_t{};
  while (stable_control < stable_control_updates &&
         result.updates < maximum_control_wait_updates) {
    if (!vm.writeHostPadState({})) {
      return result;
    }
    const auto frame = vm.tickRetailOuterFrame();
    if (!frame.completed() || !vm.advanceAudioFrameClock()) {
      return result;
    }
    ++result.updates;

    if (frame.state_after != 0U && frame.state_after != 5U) {
      stable_control = 0U;
      continue;
    }
    const auto mission = vm.readMissionBridgeState();
    const auto bridge = vm.readBridgeState();
    const auto opening_finished =
        mission_index != 0U ||
        (bridge && opening_camera_source < bridge->objects.size() &&
         bridge->objects[opening_camera_source].health <= 0);
    const auto control_ready =
        mission && bridge && missionInvariants(*mission) &&
        !mission->terminal && opening_finished && bridge->player.resident &&
        !bridge->player.control_locked && !bridge->camera.scripted &&
        !bridge->camera.locked;
    stable_control = control_ready ? stable_control + 1U : 0U;
  }
  result.completed = stable_control == stable_control_updates;
  return result;
}

std::optional<std::uint16_t> currentRoom(sf::game::LegacyGameplayVm &vm) {
  std::uint16_t room{};
  if (!vm.runtime().read16(
          sf::game::syphonFilterUsaV11NativeMissionBridgeProfile().current_room,
          room)) {
    return std::nullopt;
  }
  return room;
}

std::optional<sf::game::LegacyHostPlayerState>
roomSeed(const sf::game::MissionPackage &mission, std::uint16_t room,
         const sf::game::LegacyMissionBridgeState &state) {
  const auto sources = mission.objects().objectsInRoom(room);
  if (sources.empty()) {
    return std::nullopt;
  }
  const auto &transform =
      mission.objects().objects()[sources.front()].transform;
  const sf::game::LegacyNativePoint point{transform.x, -transform.y,
                                          transform.z};
  return sf::game::LegacyHostPlayerState{
      point, 0, state.player_health, state.player_armor, point, true};
}

struct RouteResult {
  bool completed{};
  std::uint16_t final_room{};
  std::size_t room_edges{};
  std::size_t lifecycle_edges{};
  std::size_t event_edges{};
  std::uint64_t event_instructions{};
  bool event_completed{};
  std::size_t passage_events{};
  std::array<std::uint64_t, 2U> passage_instructions{};
  std::uint32_t raw_cd_sector{};
  LifetimeStats lifetime;
  ActorCoverageStats actors;
  std::vector<MessageEvent> messages;
  sf::game::LegacyGameplayVmSnapshot snapshot;
};

RouteResult runRouteScenario(sf::game::LegacyGameplayVm &vm,
                             const sf::game::MissionPackage &mission,
                             const RouteSpec &spec, MessageObserver &observer) {
  RouteResult result;
  observer.clear();
  auto mission_before = vm.readMissionBridgeState();
  auto room = currentRoom(vm);
  if (!mission_before || !room || !missionInvariants(*mission_before)) {
    return result;
  }
  if (*room != spec.rooms[0]) {
    const auto seed = roomSeed(mission, spec.rooms[0], *mission_before);
    if (!seed || !vm.writeHostPlayerState(*seed) ||
        !vm.synchronizeHostRoom(static_cast<std::int16_t>(spec.rooms[0])) ||
        !tick(vm)) {
      return result;
    }
    mission_before = vm.readMissionBridgeState();
    room = currentRoom(vm);
  }
  const auto before = vm.readBridgeState();
  if (room) {
    result.final_room = *room;
  }
  if (!before || !mission_before || !room || *room != spec.rooms[0] ||
      !missionInvariants(*mission_before)) {
    return result;
  }

  LifetimeTracker lifetime;
  ActorCoverageTracker actors;
  if (!lifetime.observe(*before)) {
    return result;
  }
  actors.observe(*before);

  auto previous_room = *room;
  for (std::size_t index = 1U; index < spec.room_count; ++index) {
    const auto requested = static_cast<std::int16_t>(spec.rooms[index]);
    const auto seed = roomSeed(mission, spec.rooms[index], *mission_before);
    if (!seed || !vm.writeHostPlayerState(*seed) ||
        !vm.synchronizeHostRoom(requested) || !tick(vm)) {
      return result;
    }
    room = currentRoom(vm);
    const auto bridge = vm.readBridgeState();
    if (room) {
      result.final_room = *room;
    }
    if (!room || !bridge || *room != spec.rooms[index] ||
        *room == previous_room || !lifetime.observe(*bridge)) {
      return result;
    }
    actors.observe(*bridge);
    previous_room = *room;
    ++result.room_edges;
  }

  const auto after_route = vm.readBridgeState();
  const auto mission_after_route = vm.readMissionBridgeState();
  if (!after_route || !mission_after_route ||
      !missionInvariants(*mission_after_route) ||
      mission_after_route->objective_texts != mission_before->objective_texts ||
      mission_after_route->parameter_texts != mission_before->parameter_texts) {
    return result;
  }
  result.lifecycle_edges = lifecycleEdges(*before, *after_route);

  const auto pre_event_snapshot = vm.captureSnapshot();
  const auto has_passage_events = spec.required_passages[0] != no_source;
  if (has_passage_events) {
    result.event_completed = true;
    for (std::size_t index = 0U; index < spec.required_passages.size();
         ++index) {
      const auto source = spec.required_passages[index];
      if (source == no_source) {
        continue;
      }
      const auto event =
          vm.queueHostInteraction(static_cast<std::int16_t>(source));
      if (!event.completed()) {
        result.event_completed = false;
        return result;
      }
      result.passage_instructions[index] = event.execution.instructions;
      result.event_instructions += event.execution.instructions;
      ++result.passage_events;
      const auto bridge = vm.readBridgeState();
      if (!bridge || !lifetime.observe(*bridge)) {
        return result;
      }
      actors.observe(*bridge);
    }
  } else if (spec.event_kind != EventKind::none) {
    if (spec.event_source < 0 || static_cast<std::size_t>(spec.event_source) >=
                                     after_route->objects.size()) {
      return result;
    }
    const auto &event_object =
        after_route->objects[static_cast<std::size_t>(spec.event_source)];
    if (static_cast<std::uint16_t>(event_object.class_id) !=
        spec.expected_event_class) {
      return result;
    }
    sf::game::LegacyGameplayVmResult event;
    if (spec.event_kind == EventKind::interaction) {
      event = vm.queueHostInteraction(spec.event_source);
    } else {
      event = vm.queueHostImpact(mission_after_route->player_slot,
                                 spec.event_source);
    }
    result.event_completed = event.completed();
    result.event_instructions = event.execution.instructions;
    if (!result.event_completed) {
      return result;
    }
    const auto immediate = vm.readBridgeState();
    const auto immediate_mission = vm.readMissionBridgeState();
    if (!immediate || !immediate_mission || !lifetime.observe(*immediate)) {
      return result;
    }
    actors.observe(*immediate);
    result.event_edges =
        lifecycleEdges(*after_route, *immediate) +
        (missionProgressEqual(*mission_after_route, *immediate_mission) ? 0U
                                                                        : 1U);
  }

  const auto settle_frames = has_passage_events ? 0U : 4U;
  const auto settle_seed =
      roomSeed(mission, spec.rooms[spec.room_count - 1U], *mission_after_route);
  if (!settle_seed) {
    return result;
  }
  const auto settle = [&](bool track_lifetime) {
    for (std::uint32_t frame = 0U; frame < settle_frames; ++frame) {
      if (!vm.writeHostPlayerState(*settle_seed) || !tick(vm)) {
        return false;
      }
      room = currentRoom(vm);
      if (!room) {
        return false;
      }
      if (*room != spec.rooms[spec.room_count - 1U] &&
          !vm.synchronizeHostRoom(
              static_cast<std::int16_t>(spec.rooms[spec.room_count - 1U]))) {
        return false;
      }
      const auto bridge = vm.readBridgeState();
      if (!bridge || (track_lifetime && !lifetime.observe(*bridge))) {
        return false;
      }
      if (track_lifetime) {
        actors.observe(*bridge);
      }
    }
    return true;
  };
  if (!settle(true)) {
    return result;
  }
  const auto after = vm.readBridgeState();
  const auto mission_after = vm.readMissionBridgeState();
  room = currentRoom(vm);
  if (!after || !mission_after || !room ||
      *room != spec.rooms[spec.room_count - 1U] ||
      !missionInvariants(*mission_after)) {
    return result;
  }
  const auto post_event_snapshot = vm.captureSnapshot();
  if (spec.event_kind != EventKind::none && !has_passage_events &&
      result.event_edges == 0U) {
    if (!vm.restoreSnapshot(pre_event_snapshot) || !settle(false)) {
      return result;
    }
    const auto control = vm.readBridgeState();
    const auto control_mission = vm.readMissionBridgeState();
    if (!control || !control_mission) {
      return result;
    }
    result.event_edges =
        lifecycleEdges(*control, *after) +
        (missionProgressEqual(*control_mission, *mission_after) ? 0U : 1U);
    if (!vm.restoreSnapshot(post_event_snapshot)) {
      return result;
    }
  }
  result.lifecycle_edges =
      std::max(result.lifecycle_edges, lifecycleEdges(*before, *after));
  result.final_room = *room;
  result.lifetime = lifetime.stats();
  result.actors = actors.stats();
  result.completed =
      result.room_edges + 1U == spec.room_count &&
      result.lifecycle_edges != 0U && result.lifetime.valid &&
      result.lifetime.observations >= spec.room_count && result.actors.valid &&
      !observer.malformed() &&
      (spec.event_kind == EventKind::none ||
       (has_passage_events
            ? result.event_completed && result.passage_events == 2U &&
                  std::ranges::all_of(result.passage_instructions,
                                      [](std::uint64_t instructions) {
                                        return instructions != 0U;
                                      })
            : result.event_completed && result.event_edges != 0U));
  result.raw_cd_sector = vm.machine().cdrom().captureState().current_lba;
  result.messages = observer.events();
  result.snapshot = vm.captureSnapshot();
  return result;
}

bool routeResultEqual(const RouteResult &left, const RouteResult &right) {
  return left.completed && right.completed &&
         left.final_room == right.final_room &&
         left.room_edges == right.room_edges &&
         left.lifecycle_edges == right.lifecycle_edges &&
         left.event_edges == right.event_edges &&
         left.event_instructions == right.event_instructions &&
         left.event_completed == right.event_completed &&
         left.passage_events == right.passage_events &&
         left.passage_instructions == right.passage_instructions &&
         left.raw_cd_sector == right.raw_cd_sector &&
         left.lifetime == right.lifetime && left.actors == right.actors &&
         left.messages == right.messages &&
         replayStateEqual(left.snapshot, right.snapshot);
}

bool hasMessage(const std::vector<MessageEvent> &events, MessageChannel channel,
                std::string_view exact = {}) {
  return std::ranges::any_of(events, [channel, exact](const auto &event) {
    return event.channel == channel && (exact.empty() || event.text == exact);
  });
}

struct ObjectiveResult {
  bool completed{};
  std::uint32_t objective{};
  bool reveal_edge{};
  bool complete_edge{};
  std::uint64_t reveal_instructions{};
  std::uint64_t complete_instructions{};
  sf::game::LegacyMissionBridgeState final_state;
  std::vector<MessageEvent> messages;
  sf::game::LegacyGameplayVmSnapshot snapshot;
};

ObjectiveResult runObjectiveScenario(sf::game::LegacyGameplayVm &vm,
                                     MessageObserver &observer) {
  ObjectiveResult result;
  observer.clear();
  const auto before = vm.readMissionBridgeState();
  if (!before || !missionInvariants(*before) || before->terminal) {
    return result;
  }

  const auto objective_mask = entryMask(before->objective_count);
  const auto available = objective_mask & ~before->completed_objectives;
  if (available == 0U) {
    return result;
  }
  for (std::uint32_t index = 0U; index < before->objective_count; ++index) {
    if ((available & (1U << index)) != 0U) {
      result.objective = index;
      break;
    }
  }
  const auto bit = 1U << result.objective;

  const std::array reveal_arguments{result.objective, 0xffffffffU};
  const auto reveal =
      vm.invoke(objective_reveal_entry, reveal_arguments, common_call_budget);
  const auto revealed = vm.readMissionBridgeState();
  if (!reveal.completed() || !revealed || !missionInvariants(*revealed)) {
    return result;
  }
  result.reveal_instructions = reveal.execution.instructions;
  result.reveal_edge = (before->revealed_objectives & bit) != 0U ||
                       ((revealed->revealed_objectives & bit) != 0U &&
                        (revealed->notified_objectives & bit) != 0U);

  const std::array complete_arguments{result.objective};
  const auto complete = vm.invoke(objective_complete_entry, complete_arguments,
                                  common_call_budget);
  const auto after = vm.readMissionBridgeState();
  if (!complete.completed() || !after || !missionInvariants(*after)) {
    return result;
  }
  result.complete_instructions = complete.execution.instructions;
  result.complete_edge = (before->completed_objectives & bit) == 0U &&
                         (after->completed_objectives & bit) != 0U;
  result.final_state = *after;
  result.messages = observer.events();
  result.completed =
      result.reveal_edge && result.complete_edge && !observer.malformed() &&
      after->objective_count == before->objective_count &&
      after->parameter_count == before->parameter_count &&
      after->objective_texts == before->objective_texts &&
      after->parameter_texts == before->parameter_texts &&
      hasMessage(result.messages, MessageChannel::status) &&
      hasMessage(result.messages, MessageChannel::centered, "Checkpoint");
  result.snapshot = vm.captureSnapshot();
  return result;
}

bool objectiveResultEqual(const ObjectiveResult &left,
                          const ObjectiveResult &right) {
  return left.completed && right.completed &&
         left.objective == right.objective &&
         left.reveal_edge == right.reveal_edge &&
         left.complete_edge == right.complete_edge &&
         left.reveal_instructions == right.reveal_instructions &&
         left.complete_instructions == right.complete_instructions &&
         missionStateEqual(left.final_state, right.final_state) &&
         left.messages == right.messages &&
         replayStateEqual(left.snapshot, right.snapshot);
}

struct TransitionTrace {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> states;
  sf::game::LegacyMissionTransitionDecision decision;
  sf::game::LegacyMissionBridgeState final_state;
  sf::game::LegacyMissionBridgeState last_state;
  sf::psx::R3000StopReason stop_reason{sf::psx::R3000StopReason::running};
  std::uint32_t stop_pc{};
  std::uint32_t final_application_state{};
  std::uint8_t stop_phase{};
  bool completed{};
};

TransitionTrace runTransition(sf::game::LegacyGameplayVm &vm,
                              std::uint32_t mission_index,
                              bool expect_failure) {
  TransitionTrace result;
  auto movie_loader_pending = false;
  const auto scripted_movie_count =
      sf::game::missionScriptedMoviePaths(mission_index).size();
  auto coherent_mission = vm.readMissionBridgeState();
  if (!coherent_mission || !missionInvariants(*coherent_mission)) {
    result.stop_phase = 1U;
    return result;
  }
  result.last_state = *coherent_mission;
  constexpr std::uint32_t maximum_frames = 320U;
  for (std::uint32_t frame_index = 0U; frame_index < maximum_frames;
       ++frame_index) {
    std::uint32_t application_state{};
    constexpr auto outer_profile =
        sf::game::syphonFilterUsaV11RetailOuterFrameProfile();
    if (!vm.runtime().read32(outer_profile.current_state, application_state)) {
      result.stop_phase = 9U;
      return result;
    }
    if (application_state == 2U) {
      const auto transition_mission = vm.readMissionBridgeState();
      if (!transition_mission || !missionInvariants(*transition_mission)) {
        result.stop_phase = 10U;
        return result;
      }
      if (!sf::game::legacyRetailState2DispatchAllowed(application_state,
                                                       *transition_mission)) {
        result.decision = sf::game::classifyLegacyMissionTransition(
            mission_index, application_state, application_state,
            *transition_mission, movie_loader_pending, scripted_movie_count);
        result.final_state = *transition_mission;
        result.last_state = *transition_mission;
        result.final_application_state = application_state;
        result.stop_phase = 7U;
        result.completed =
            expect_failure && result.decision.request_failure_restart &&
            !result.decision.request_ending_movie &&
            transition_mission->failure && transition_mission->terminal &&
            !transition_mission->success;
        return result;
      }
      const auto transition = vm.dispatchRetailState2Transition();
      result.final_application_state = transition.final_state;
      if (!transition.completed()) {
        result.stop_phase = 11U;
        return result;
      }
      if (sf::game::legacyRetailTerminalMovieBoundary(transition.final_state,
                                                      *transition_mission)) {
        result.states.emplace_back(application_state, transition.final_state);
        result.decision = sf::game::classifyLegacyMissionTransition(
            mission_index, application_state, transition.final_state,
            *transition_mission, movie_loader_pending, scripted_movie_count);
        result.final_state = *transition_mission;
        result.last_state = *transition_mission;
        result.stop_phase = 7U;
        result.completed = result.decision.request_ending_movie &&
                           !result.decision.request_failure_restart &&
                           transition_mission->success &&
                           transition_mission->terminal &&
                           !transition_mission->failure;
        return result;
      }
    }
    if (!vm.writeHostPadState({})) {
      result.stop_phase = 2U;
      return result;
    }
    const auto frame = vm.tickRetailOuterFrame();
    if (!frame.completed()) {
      result.stop_phase = 3U;
      result.final_application_state = frame.state_before;
      const auto record = [&](const sf::game::LegacyGameplayVmResult &call) {
        if (!call.completed() &&
            result.stop_reason == sf::psx::R3000StopReason::running) {
          result.stop_reason = call.execution.reason;
          result.stop_pc = call.execution.pc;
        }
      };
      for (const auto &call : frame.guest_calls) {
        record(call);
      }
      if (frame.renderer_tail) {
        record(*frame.renderer_tail);
      }
      record(frame.platform_tail.delayed_callbacks);
      if (frame.platform_tail.fade_callback) {
        record(*frame.platform_tail.fade_callback);
      }
      return result;
    }
    if (!vm.advanceAudioFrameClock()) {
      result.stop_phase = 4U;
      return result;
    }
    result.states.emplace_back(frame.state_before, frame.state_after);
    // The common retail success path enters the movie loader through state 4.
    // Its overlay teardown makes the bridge unreadable on that same boundary,
    // so classify it against the last coherent terminal mission sample.
    if (!expect_failure && frame.state_after == 4U) {
      result.decision = sf::game::classifyLegacyMissionTransition(
          mission_index, frame.state_before, frame.state_after,
          *coherent_mission, movie_loader_pending, scripted_movie_count);
      result.final_state = *coherent_mission;
      result.last_state = *coherent_mission;
      result.final_application_state = frame.state_after;
      result.stop_phase = 7U;
      result.completed = result.decision.request_ending_movie &&
                         !result.decision.request_failure_restart &&
                         coherent_mission->success &&
                         coherent_mission->terminal &&
                         !coherent_mission->failure;
      return result;
    }
    if (!sf::game::legacyRetailStreamingState(frame.state_after)) {
      const auto mission = vm.readMissionBridgeState();
      if (!mission) {
        result.stop_phase = 5U;
        return result;
      }
      result.last_state = *mission;
      if (!missionInvariants(*mission)) {
        result.stop_phase = 6U;
        return result;
      }
      coherent_mission = *mission;
    }
    result.decision = sf::game::classifyLegacyMissionTransition(
        mission_index, frame.state_before, frame.state_after, *coherent_mission,
        movie_loader_pending, scripted_movie_count);
    movie_loader_pending = result.decision.movie_loader_pending;
    if (!result.decision.finished) {
      continue;
    }
    result.final_state = *coherent_mission;
    result.last_state = *coherent_mission;
    result.final_application_state = frame.state_after;
    result.stop_phase = 7U;
    result.completed =
        expect_failure
            ? result.decision.request_failure_restart &&
                  !result.decision.request_ending_movie &&
                  coherent_mission->failure && coherent_mission->terminal &&
                  !coherent_mission->success && frame.state_after == 2U
            : result.decision.request_ending_movie &&
                  !result.decision.request_failure_restart &&
                  coherent_mission->success && coherent_mission->terminal &&
                  !coherent_mission->failure && frame.state_after == 3U;
    return result;
  }
  result.stop_phase = 8U;
  return result;
}

struct OutcomeResult {
  bool completed{};
  bool failure{};
  bool reason_edge{};
  bool transition_latch{};
  std::uint32_t selected_entry{};
  std::uint64_t outcome_instructions{};
  std::uint64_t transition_instructions{};
  TransitionTrace transition;
  std::vector<MessageEvent> messages;
  sf::game::LegacyGameplayVmSnapshot snapshot;
};

OutcomeResult runFailureScenario(sf::game::LegacyGameplayVm &vm,
                                 std::uint32_t mission_index,
                                 MessageObserver &observer) {
  OutcomeResult result;
  result.failure = true;
  observer.clear();
  const auto before = vm.readMissionBridgeState();
  if (!before || !missionInvariants(*before) || before->terminal) {
    return result;
  }

  const auto fail_objective = before->parameter_count == 0U;
  if (fail_objective && before->objective_count == 0U) {
    return result;
  }
  result.selected_entry = 0U;
  constexpr auto no_sound = 0xffffffffU;
  const std::array failure_arguments{result.selected_entry, no_sound,
                                     fail_objective ? 1U : 0U};
  const auto failure =
      vm.invoke(mission_failure_entry, failure_arguments, common_call_budget);
  const auto failed = vm.readMissionBridgeState();
  if (!failure.completed() || !failed || !missionInvariants(*failed)) {
    return result;
  }
  result.outcome_instructions = failure.execution.instructions;
  const auto failed_mask =
      fail_objective ? failed->failed_objectives : failed->failed_parameters;
  const auto before_mask =
      fail_objective ? before->failed_objectives : before->failed_parameters;
  result.reason_edge = (before_mask & 1U) == 0U && (failed_mask & 1U) != 0U;

  const auto transition =
      vm.invoke(mission_failure_transition_entry, {}, common_call_budget);
  const auto transition_state = vm.readMissionBridgeState();
  if (!transition.completed() || !transition_state ||
      !missionInvariants(*transition_state)) {
    return result;
  }
  result.transition_instructions = transition.execution.instructions;
  result.transition_latch = transition_state->failure_transition;
  result.transition = runTransition(vm, mission_index, true);
  result.messages = observer.events();
  result.completed = result.reason_edge && result.transition_latch &&
                     result.transition.completed && !observer.malformed() &&
                     hasMessage(result.messages, MessageChannel::centered);
  result.snapshot = vm.captureSnapshot();
  return result;
}

OutcomeResult runSuccessScenario(sf::game::LegacyGameplayVm &vm,
                                 std::uint32_t mission_index,
                                 MessageObserver &observer) {
  OutcomeResult result;
  observer.clear();
  const auto before = vm.readMissionBridgeState();
  if (!before || !missionInvariants(*before) || before->terminal) {
    return result;
  }

  constexpr std::array success_arguments{1U};
  const auto success =
      vm.invoke(mission_success_entry, success_arguments, common_call_budget);
  const auto accepted = vm.readMissionBridgeState();
  if (!success.completed() || success.return_value != 1U || !accepted ||
      !missionInvariants(*accepted) || !accepted->success ||
      !accepted->terminal || accepted->failure) {
    return result;
  }
  result.outcome_instructions = success.execution.instructions;
  result.transition = runTransition(vm, mission_index, false);
  result.messages = observer.events();
  result.completed = result.transition.completed && !observer.malformed();
  result.snapshot = vm.captureSnapshot();
  return result;
}

bool outcomeResultEqual(const OutcomeResult &left, const OutcomeResult &right) {
  return left.completed && right.completed && left.failure == right.failure &&
         left.reason_edge == right.reason_edge &&
         left.transition_latch == right.transition_latch &&
         left.selected_entry == right.selected_entry &&
         left.outcome_instructions == right.outcome_instructions &&
         left.transition_instructions == right.transition_instructions &&
         left.transition.states == right.transition.states &&
         left.transition.decision.movie_loader_pending ==
             right.transition.decision.movie_loader_pending &&
         left.transition.decision.request_intro_movie ==
             right.transition.decision.request_intro_movie &&
         left.transition.decision.request_ending_movie ==
             right.transition.decision.request_ending_movie &&
         left.transition.decision.request_failure_restart ==
             right.transition.decision.request_failure_restart &&
         left.transition.decision.finished ==
             right.transition.decision.finished &&
         left.transition.final_application_state ==
             right.transition.final_application_state &&
         missionStateEqual(left.transition.final_state,
                           right.transition.final_state) &&
         left.messages == right.messages &&
         replayStateEqual(left.snapshot, right.snapshot);
}

std::string_view eventCoverage(const RouteSpec &spec) noexcept {
  if (spec.required_passages[0] != no_source) {
    return "authored-passage";
  }
  switch (spec.event_kind) {
  case EventKind::interaction:
    return "authored-interaction";
  case EventKind::impact:
    return "authored-impact";
  case EventKind::none:
    return "route-lifecycle-only";
  }
  return "invalid";
}

int runProbe(const std::filesystem::path &cue_path,
             std::optional<std::uint32_t> only_mission,
             std::uint32_t renderer_stress_updates) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{
        sf::core::ErrorCode::unsupported,
        "G3.1 gameplay probe requires Syphon Filter USA v1.1"};
  }
  if (sf::game::missionCatalog().size() != route_specs.size()) {
    throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                          "G3.1 coverage matrix is not all-20"};
  }

  auto checked = std::size_t{};
  auto passed = std::size_t{};
  auto authored_event_coverage = std::size_t{};
  auto natural_dynamic_coverage = std::size_t{};
  auto lifetime_spawns = std::size_t{};
  auto lifetime_despawns = std::size_t{};
  auto lifetime_reuses = std::size_t{};
  auto live_npc_samples = std::size_t{};
  auto presented_npc_pose_samples = std::size_t{};
  auto special_appearances = std::array<std::size_t, 4U>{};
  auto special_presented_poses = std::array<std::size_t, 4U>{};
  auto production_npc_samples = std::size_t{};
  auto production_special_prop_samples = std::array<std::size_t, 3U>{};
  for (const auto &mission : sf::game::missionCatalog()) {
    if (only_mission && mission.index != *only_mission) {
      continue;
    }
    ++checked;
    if (mission.index >= route_specs.size() || mission.selection_index < 0) {
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=failed phase=coverage-matrix\n";
      continue;
    }

    const auto package = sf::game::MissionPackage::load(disc, mission.index);
    const auto &spec = route_specs[mission.index];
    if (!validateSpec(package, spec)) {
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=failed phase=route-matrix\n";
      continue;
    }
    if (renderer_stress_updates != 0U) {
      const auto passed_stress =
          runProductionRendererStress(package, renderer_stress_updates);
      std::cout << "mission=" << mission.index
                << " renderer-stress=" << renderer_stress_updates
                << " result=" << (passed_stress ? "passed" : "failed") << '\n';
      return passed_stress ? 0 : 2;
    }
    if (spec.event_kind != EventKind::none) {
      ++authored_event_coverage;
    }

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
    auto natural_first = NaturalLifetimeResult{};
    auto natural_replay = NaturalLifetimeResult{};
    auto natural_exact = false;
    auto restore_for_active = false;
    auto bootstrap_checkpoint =
        std::optional<sf::game::LegacyGameplayVmSnapshot>{};
    if (bootstrap.completed()) {
      bootstrap_checkpoint.emplace(vm.captureSnapshot());
      natural_first = runNaturalLifetimeScenario(vm, mission.index);
      if (vm.restoreSnapshot(*bootstrap_checkpoint)) {
        natural_replay = runNaturalLifetimeScenario(vm, mission.index);
        natural_exact =
            naturalLifetimeResultEqual(natural_first, natural_replay);
      }
      restore_for_active = vm.restoreSnapshot(*bootstrap_checkpoint);
    }
    const auto natural_tier_ready =
        natural_exact &&
        naturalLifetimeMatches(mission.index, natural_first.lifetime);
    const auto first_visible =
        restore_for_active && prepareFirstVisibleFrame(vm, mission.index);
    const auto active_wait = first_visible
                                 ? waitForActiveGameplay(vm, mission.index)
                                 : ActiveGameplayWaitResult{};
    auto park_ui_bridge_ready = mission.index != 3U;
    if (active_wait.completed && mission.index == 3U) {
      struct ParkUiTrace {
        bool completed{};
        bool timer_call{};
        bool timer_bridge{};
        bool message_call{};
        bool message_bridge{};
        std::uint32_t timer_setup_updates{};
        std::uint16_t raw_timer_handle{0xffffU};
        std::uint32_t raw_timer_glyphs{};
        std::uint16_t raw_timer_glyph_count{};
        std::int16_t timer_first_x{};
        std::int16_t timer_first_y{};
        std::int32_t timer_before{};
        std::int32_t timer_after{};
        std::vector<sf::game::LegacyUiGlyphBridgeState> message_before;
        std::vector<sf::game::LegacyUiGlyphBridgeState> message_after;
        std::optional<sf::game::LegacyUiBackdropBridgeState> backdrop;

        [[nodiscard]] bool operator==(const ParkUiTrace &) const = default;
      };
      const auto ui_checkpoint = vm.captureSnapshot();
      const auto trace_park_ui = [&vm]() {
        ParkUiTrace trace;
        constexpr std::uint32_t timer_setup_entry = 0x80040154U;
        constexpr std::uint32_t status_text_entry = 0x80085d04U;
        constexpr std::uint32_t probe_text = 0x801ff000U;
        constexpr std::string_view text = "PARK UI";
        for (std::size_t index = 0U; index < text.size(); ++index) {
          if (!vm.runtime().write8(probe_text +
                                       static_cast<std::uint32_t>(index),
                                   static_cast<std::uint8_t>(text[index]))) {
            return trace;
          }
        }
        if (!vm.runtime().write8(
                probe_text + static_cast<std::uint32_t>(text.size()), 0U)) {
          return trace;
        }
        const auto timer =
            vm.invoke(timer_setup_entry, std::array{0x4b0U, 0U, 0x80146a64U},
                      common_call_budget);
        trace.timer_call = timer.completed();
        auto timer_state = vm.readMissionBridgeState();
        while (trace.timer_call && timer_state && !timer_state->timer &&
               trace.timer_setup_updates < 24U) {
          if (!tick(vm)) {
            return trace;
          }
          ++trace.timer_setup_updates;
          timer_state = vm.readMissionBridgeState();
        }
        static_cast<void>(
            vm.runtime().read16(0x80115f22U, trace.raw_timer_handle));
        if (trace.raw_timer_handle != 0xffffU) {
          const auto object =
              0x80120a98U + (trace.raw_timer_handle & 0xffU) * 0x1cU;
          static_cast<void>(
              vm.runtime().read32(object, trace.raw_timer_glyphs));
          static_cast<void>(
              vm.runtime().read16(object + 0x0cU, trace.raw_timer_glyph_count));
        }
        trace.timer_bridge = timer_state && timer_state->timer.has_value();
        if (!trace.timer_call || !trace.timer_bridge) {
          return trace;
        }
        const auto message = vm.invoke(
            status_text_entry, std::array{probe_text, 0U}, common_call_budget);
        trace.message_call = message.completed();
        const auto before = vm.readMissionBridgeState();
        trace.message_bridge = before.has_value();
        if (!timer.completed() || !message.completed() || !before ||
            !before->timer || before->timer->glyphs.size() != 8U) {
          return trace;
        }
        trace.timer_before = before->timer->remaining_ticks;
        trace.timer_first_x = before->timer->glyphs.front().x;
        trace.timer_first_y = before->timer->glyphs.front().y;
        for (const auto &candidate : before->messages) {
          if (candidate.channel == sf::game::LegacyUiMessageChannel::status) {
            trace.message_before.insert(trace.message_before.end(),
                                        candidate.glyphs.begin(),
                                        candidate.glyphs.end());
          }
          if (candidate.backdrop) {
            trace.backdrop = candidate.backdrop;
          }
        }
        if (!tick(vm) || !tick(vm)) {
          return trace;
        }
        const auto after = vm.readMissionBridgeState();
        if (!after || !after->timer) {
          return trace;
        }
        trace.timer_after = after->timer->remaining_ticks;
        for (const auto &candidate : after->messages) {
          if (candidate.channel == sf::game::LegacyUiMessageChannel::status) {
            trace.message_after.insert(trace.message_after.end(),
                                       candidate.glyphs.begin(),
                                       candidate.glyphs.end());
          }
          if (candidate.backdrop) {
            trace.backdrop = candidate.backdrop;
          }
        }
        trace.completed = trace.timer_before > 0 &&
                          trace.timer_after == trace.timer_before - 2 &&
                          !trace.message_after.empty() && trace.backdrop &&
                          trace.message_before != trace.message_after;
        return trace;
      };
      const auto first_ui = trace_park_ui();
      const auto replay_restored = vm.restoreSnapshot(ui_checkpoint);
      const auto replay_ui = replay_restored ? trace_park_ui() : ParkUiTrace{};
      const auto final_restored = vm.restoreSnapshot(ui_checkpoint);
      park_ui_bridge_ready =
          first_ui.completed && replay_ui == first_ui && final_restored;
      std::cout << "PARK-retail-ui timer=" << first_ui.timer_before << "->"
                << first_ui.timer_after << " timer-glyphs=8 message-glyphs="
                << first_ui.message_before.size() << " transition="
                << (first_ui.message_before != first_ui.message_after)
                << " backdrop=" << first_ui.backdrop.has_value()
                << " replay=" << (replay_ui == first_ui)
                << " stages=" << first_ui.timer_call << '/'
                << first_ui.timer_bridge << '/' << first_ui.message_call << '/'
                << first_ui.message_bridge << " raw=0x" << std::hex
                << first_ui.raw_timer_handle << "/0x"
                << first_ui.raw_timer_glyphs << '/' << std::dec
                << first_ui.raw_timer_glyph_count
                << " timer-first=" << first_ui.timer_first_x << ','
                << first_ui.timer_first_y
                << " setup-updates=" << first_ui.timer_setup_updates << '\n';
    }
    const auto initial_mission =
        active_wait.completed ? vm.readMissionBridgeState() : std::nullopt;
    const auto initial_bridge =
        active_wait.completed ? vm.readBridgeState() : std::nullopt;
    if (!natural_tier_ready || !active_wait.completed || !initial_mission ||
        !initial_bridge || !missionInvariants(*initial_mission) ||
        initial_mission->terminal ||
        !initial_bridge->terrain_triggers_enabled) {
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=failed phase=bootstrap-or-active-gameplay"
                << " natural-replay=" << natural_exact << " natural-stop="
                << static_cast<unsigned int>(natural_first.stop_phase) << '/'
                << static_cast<unsigned int>(natural_replay.stop_phase)
                << " natural-edges=" << natural_first.lifetime.identity_edges
                << " natural-active="
                << natural_first.lifetime.active_observations
                << " actor-valid=" << natural_first.actors.valid << '/'
                << natural_replay.actors.valid
                << " actor-live/pose=" << natural_first.actors.live_npc_samples
                << '/' << natural_first.actors.presented_npc_pose_samples
                << " terrain-triggers="
                << (initial_bridge && initial_bridge->terrain_triggers_enabled)
                << " control-wait=" << active_wait.updates << '\n';
      continue;
    }
    if (mission.index == 0U || mission.index == 14U || mission.index == 17U) {
      ++natural_dynamic_coverage;
    }

    const auto visual_checkpoint = vm.captureSnapshot();
    const auto start_visual_first = runStartVisualScenario(vm, package);
    const auto visual_restore = vm.restoreSnapshot(visual_checkpoint);
    const auto start_visual_replay = visual_restore
                                         ? runStartVisualScenario(vm, package)
                                         : StartVisualResult{};
    const auto start_visual_exact =
        startVisualResultEqual(start_visual_first, start_visual_replay);
    const auto restore_after_visual = vm.restoreSnapshot(visual_checkpoint);
    if (!start_visual_exact || !restore_after_visual) {
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=failed phase=start-visual"
                << " replay=" << start_visual_exact
                << " room=" << start_visual_first.room
                << " projection=" << start_visual_first.projection
                << " fov=" << start_visual_first.fov_raw
                << " fade=" << start_visual_first.fade_current << '/'
                << static_cast<unsigned int>(start_visual_first.fade_floor)
                << " world-models=" << start_visual_first.active_models.size()
                << '\n';
      continue;
    }

    const auto production_actor_first = runProductionActorScenario(package);
    const auto production_actor_replay = runProductionActorScenario(package);
    const auto production_actor_exact =
        production_actor_first.completed &&
        production_actor_first == production_actor_replay;
    if (!production_actor_exact) {
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=failed phase=production-actor"
                << " first/replay=" << production_actor_first.completed << '/'
                << production_actor_replay.completed
                << " control-wait=" << production_actor_first.control_wait
                << " npc=" << production_actor_first.expected_npc_samples << '/'
                << production_actor_first.actual_npc_samples << '\n';
      continue;
    }
    production_npc_samples += production_actor_first.expected_npc_samples;
    for (std::size_t index = 0U; index < production_special_prop_samples.size();
         ++index) {
      production_special_prop_samples[index] +=
          production_actor_first.special_prop_samples[index];
    }

    MessageObserver observer;
    observer.bind(vm);
    const auto checkpoint = vm.captureSnapshot();

    const auto route_first = runRouteScenario(vm, package, spec, observer);
    const auto route_restore = vm.restoreSnapshot(checkpoint);
    const auto route_replay =
        route_restore ? runRouteScenario(vm, package, spec, observer)
                      : RouteResult{};
    const auto route_exact = routeResultEqual(route_first, route_replay);
    const auto restore_after_route = vm.restoreSnapshot(checkpoint);

    const auto objective_first = restore_after_route
                                     ? runObjectiveScenario(vm, observer)
                                     : ObjectiveResult{};
    const auto objective_restore = vm.restoreSnapshot(checkpoint);
    const auto objective_replay = objective_restore
                                      ? runObjectiveScenario(vm, observer)
                                      : ObjectiveResult{};
    const auto objective_exact =
        objectiveResultEqual(objective_first, objective_replay);
    const auto restore_after_objective = vm.restoreSnapshot(checkpoint);

    const auto failure_first =
        restore_after_objective
            ? runFailureScenario(vm, mission.index, observer)
            : OutcomeResult{};
    const auto failure_restore = vm.restoreSnapshot(checkpoint);
    const auto failure_replay =
        failure_restore ? runFailureScenario(vm, mission.index, observer)
                        : OutcomeResult{};
    const auto failure_exact =
        outcomeResultEqual(failure_first, failure_replay);
    const auto restore_after_failure = vm.restoreSnapshot(checkpoint);

    const auto success_first =
        restore_after_failure ? runSuccessScenario(vm, mission.index, observer)
                              : OutcomeResult{};
    const auto success_restore = vm.restoreSnapshot(checkpoint);
    const auto success_replay =
        success_restore ? runSuccessScenario(vm, mission.index, observer)
                        : OutcomeResult{};
    const auto success_exact =
        outcomeResultEqual(success_first, success_replay);

    const auto ready = natural_tier_ready && park_ui_bridge_ready &&
                       start_visual_exact && production_actor_exact &&
                       route_exact && objective_exact && failure_exact &&
                       success_exact;
    lifetime_spawns +=
        natural_first.lifetime.spawns + route_first.lifetime.spawns;
    lifetime_despawns +=
        natural_first.lifetime.despawns + route_first.lifetime.despawns;
    lifetime_reuses +=
        natural_first.lifetime.reuses + route_first.lifetime.reuses;
    live_npc_samples += natural_first.actors.live_npc_samples +
                        route_first.actors.live_npc_samples;
    presented_npc_pose_samples +=
        natural_first.actors.presented_npc_pose_samples +
        route_first.actors.presented_npc_pose_samples;
    for (std::size_t index = 0U; index < special_appearances.size(); ++index) {
      special_appearances[index] +=
          natural_first.actors.special_appearances[index] +
          route_first.actors.special_appearances[index];
      special_presented_poses[index] +=
          natural_first.actors.special_presented_poses[index] +
          route_first.actors.special_presented_poses[index];
    }
    if (ready) {
      ++passed;
    }

    std::cout
        << "mission=" << mission.index << " resource=" << mission.resource_name
        << " result=" << (ready ? "ready" : "failed")
        << " event-coverage=" << eventCoverage(spec)
        << " lifetime-coverage=" << naturalLifetimeCoverage(mission.index)
        << " natural-replay=" << natural_exact
        << " natural-edges=" << natural_first.lifetime.identity_edges
        << " natural-active=" << natural_first.lifetime.active_observations
        << " natural-spawns=" << natural_first.lifetime.spawns
        << " natural-despawns=" << natural_first.lifetime.despawns
        << " natural-reuses=" << natural_first.lifetime.reuses
        << " live-npc-samples="
        << natural_first.actors.live_npc_samples +
               route_first.actors.live_npc_samples
        << " npc-pose-samples="
        << natural_first.actors.presented_npc_pose_samples +
               route_first.actors.presented_npc_pose_samples
        << " start-visual-replay=" << start_visual_exact
        << " start-world-models=" << start_visual_first.active_models.size()
        << " start-projection=" << start_visual_first.projection
        << " production-actor-replay=" << production_actor_exact
        << " production-npc=" << production_actor_first.expected_npc_samples
        << " control-wait=" << active_wait.updates
        << " objective-coverage=common-guest@0x" << std::hex
        << objective_reveal_entry << "+0x" << objective_complete_entry
        << " failure-coverage=common-guest@0x" << mission_failure_entry << "+0x"
        << mission_failure_transition_entry
        << " success-coverage=common-entry@0x" << mission_success_entry
        << std::dec << " objectives=" << initial_mission->objective_count
        << " parameters=" << initial_mission->parameter_count
        << " route-replay=" << route_exact
        << " lifecycle-edges=" << route_first.lifecycle_edges
        << " identity-edges=" << route_first.lifetime.identity_edges
        << " spawns=" << route_first.lifetime.spawns
        << " despawns=" << route_first.lifetime.despawns
        << " reuses=" << route_first.lifetime.reuses
        << " event-edges=" << route_first.event_edges
        << " objective-replay=" << objective_exact
        << " objective=" << objective_first.objective
        << " objective-messages=" << objective_first.messages.size()
        << " failure-replay=" << failure_exact
        << " failure-frames=" << failure_first.transition.states.size()
        << " failure-messages=" << failure_first.messages.size()
        << " success-replay=" << success_exact
        << " success-frames=" << success_first.transition.states.size() << '\n';
    if (!ready) {
      std::cout << "  route-diagnostic first=" << route_first.completed
                << " replay=" << route_replay.completed << " state="
                << replayStateEqual(route_first.snapshot, route_replay.snapshot)
                << " lifetime="
                << (route_first.lifetime == route_replay.lifetime)
                << " messages="
                << (route_first.messages == route_replay.messages)
                << " first-room/edges=" << route_first.final_room << '/'
                << route_first.room_edges << '/' << route_first.lifecycle_edges
                << '/' << route_first.event_edges
                << " replay-room/edges=" << route_replay.final_room << '/'
                << route_replay.room_edges << '/'
                << route_replay.lifecycle_edges << '/'
                << route_replay.event_edges << '\n';
      std::cout
          << "  success-diagnostic first=" << success_first.completed
          << " replay=" << success_replay.completed
          << " transition=" << success_first.transition.completed << '/'
          << success_replay.transition.completed
          << " final-state=" << success_first.transition.final_application_state
          << '/' << success_replay.transition.final_application_state
          << " decision="
          << success_first.transition.decision.request_ending_movie << '/'
          << success_replay.transition.decision.request_ending_movie
          << " trace="
          << (success_first.transition.states ==
              success_replay.transition.states)
          << " mission="
          << missionStateEqual(success_first.transition.final_state,
                               success_replay.transition.final_state)
          << " messages=" << (success_first.messages == success_replay.messages)
          << " state="
          << replayStateEqual(success_first.snapshot, success_replay.snapshot)
          << " outcome-instructions=" << success_first.outcome_instructions
          << '/' << success_replay.outcome_instructions << " stop="
          << static_cast<unsigned int>(success_first.transition.stop_phase)
          << '/' << sf::psx::toString(success_first.transition.stop_reason)
          << "/0x" << std::hex << success_first.transition.stop_pc << std::dec
          << " last=" << success_first.transition.last_state.terminal << '/'
          << success_first.transition.last_state.success << '/'
          << success_first.transition.last_state.failure << '/'
          << success_first.transition.last_state.failure_transition
          << " masks=0x" << std::hex
          << success_first.transition.last_state.completed_objectives << "/0x"
          << success_first.transition.last_state.failed_objectives << "/0x"
          << success_first.transition.last_state.revealed_objectives << "/0x"
          << success_first.transition.last_state.notified_objectives << std::dec
          << " cpu=" << success_first.snapshot.cpu.pc
          << "/t1=" << success_first.snapshot.cpu.gpr[9]
          << "/a0=" << success_first.snapshot.cpu.gpr[4]
          << "/a1=" << success_first.snapshot.cpu.gpr[5]
          << "/a2=" << success_first.snapshot.cpu.gpr[6]
          << "/a3=" << success_first.snapshot.cpu.gpr[7]
          << "/ra=" << success_first.snapshot.cpu.gpr[31] << " states=";
      for (const auto &[before, after] : success_first.transition.states) {
        std::cout << before << '>' << after << ',';
      }
      std::cout << '\n';
    }
  }

  const auto expected = only_mission ? std::size_t{1U} : route_specs.size();
  const auto expected_authored_events =
      only_mission
          ? (route_specs[*only_mission].event_kind == EventKind::none ? 0U : 1U)
          : 18U;
  const auto missing_special_pose =
      std::ranges::any_of(special_presented_poses,
                          [](std::size_t samples) { return samples == 0U; });
  const auto missing_special_prop =
      std::ranges::any_of(production_special_prop_samples,
                          [](std::size_t samples) { return samples == 0U; });
  if (checked != expected || passed != checked ||
      authored_event_coverage != expected_authored_events ||
      (!only_mission &&
       (natural_dynamic_coverage != 3U || lifetime_spawns == 0U ||
        lifetime_despawns == 0U || lifetime_reuses == 0U ||
        live_npc_samples == 0U || presented_npc_pose_samples == 0U ||
        production_npc_samples == 0U || missing_special_pose ||
        missing_special_prop))) {
    std::cerr << "G3.1 gameplay gate failed: passed=" << passed << '/'
              << checked << " expected=" << expected
              << " authored-events=" << authored_event_coverage << '/'
              << expected_authored_events
              << " natural-dynamic=" << natural_dynamic_coverage << "/3"
              << " lifetime-spawn/despawn/reuse=" << lifetime_spawns << '/'
              << lifetime_despawns << '/' << lifetime_reuses
              << " actor-live/pose=" << live_npc_samples << '/'
              << presented_npc_pose_samples
              << " production-npc=" << production_npc_samples
              << " missing-special-pose=" << missing_special_pose
              << " missing-special-prop=" << missing_special_prop << '\n';
    return 2;
  }
  std::cout << "G3.1 gameplay gate passed: missions=" << passed << '/'
            << checked << " authored-events=" << authored_event_coverage << '/'
            << expected_authored_events << " success-common-entry=" << checked
            << " natural-dynamic=" << natural_dynamic_coverage
            << " lifetime-spawn/despawn/reuse=" << lifetime_spawns << '/'
            << lifetime_despawns << '/' << lifetime_reuses
            << " actor-live/pose=" << live_npc_samples << '/'
            << presented_npc_pose_samples
            << " production-npc=" << production_npc_samples
            << " special-appear/pose=";
  for (std::size_t index = 0U; index < special_appearances.size(); ++index) {
    std::cout << special_appearances[index] << '/'
              << special_presented_poses[index] << ',';
  }
  std::cout << " production-special-props=";
  for (const auto samples : production_special_prop_samples) {
    std::cout << samples << ',';
  }
  std::cout << " natural-success=0 (fail-closed coverage matrix)\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: sf_g3_gameplay_probe <game.cue> [mission-index] "
                 "[renderer-stress-updates]\n";
    return 1;
  }
  try {
    auto only_mission = std::optional<std::uint32_t>{};
    if (argc >= 3) {
      auto mission = std::uint32_t{};
      const auto text = std::string_view{argv[2]};
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), mission);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
          mission >= route_specs.size()) {
        std::cerr << "Invalid mission index\n";
        return 1;
      }
      only_mission = mission;
    }
    auto renderer_stress_updates = std::uint32_t{};
    if (argc == 4) {
      const auto text = std::string_view{argv[3]};
      const auto parsed = std::from_chars(
          text.data(), text.data() + text.size(), renderer_stress_updates);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
          renderer_stress_updates == 0U) {
        std::cerr << "Invalid renderer stress update count\n";
        return 1;
      }
    }
    return runProbe(std::filesystem::path{argv[1]}, only_mission,
                    renderer_stress_updates);
  } catch (const std::exception &error) {
    std::cerr << "G3.1 gameplay gate failed: " << error.what() << '\n';
    return 10;
  }
}
