#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
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
#include <string_view>

namespace {

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

// Exact BIN slots and DAT-connected routes for the supported USA v1.1 image.
// `none` is intentional where the first active route has no switch/door; in
// those missions the required object residency edge is the script observable.
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

bool adjacent(const sf::game::MissionPackage &mission, std::uint16_t left,
              std::uint16_t right) {
  const auto contains = [](const auto &models, std::uint16_t room) {
    return std::ranges::find(models, room) != models.end();
  };
  return contains(mission.layout().visibility(left).active_models, right) ||
         contains(mission.layout().visibility(right).active_models, left);
}

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

bool sameMissionState(const sf::game::LegacyMissionBridgeState &left,
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

struct ScenarioResult {
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
  sf::game::LegacyGameplayVmSnapshot snapshot;
};

bool tick(sf::game::LegacyGameplayVm &vm) {
  if (!vm.writeHostPadState({})) {
    return false;
  }
  const auto frame = vm.tickRetailOuterFrame();
  return frame.completed() && vm.advanceAudioFrameClock();
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

ScenarioResult runScenario(sf::game::LegacyGameplayVm &vm,
                           const sf::game::MissionPackage &mission,
                           const RouteSpec &spec) {
  ScenarioResult result;
  const auto before = vm.readBridgeState();
  const auto mission_before = vm.readMissionBridgeState();
  auto room = currentRoom(vm);
  if (room) {
    result.final_room = *room;
  }
  if (!before || !mission_before || !room || *room != spec.rooms[0]) {
    return result;
  }

  auto previous_room = *room;
  for (std::size_t index = 1U; index < spec.room_count; ++index) {
    const auto requested = static_cast<std::int16_t>(spec.rooms[index]);
    const auto seed = roomSeed(mission, spec.rooms[index], *mission_before);
    if (!seed || !vm.writeHostPlayerState(*seed) ||
        !vm.synchronizeHostRoom(requested) || !tick(vm)) {
      return result;
    }
    room = currentRoom(vm);
    if (room) {
      result.final_room = *room;
    }
    if (!room || *room != spec.rooms[index] || *room == previous_room) {
      return result;
    }
    previous_room = *room;
    ++result.room_edges;
  }

  const auto after_route = vm.readBridgeState();
  const auto mission_after_route = vm.readMissionBridgeState();
  if (!after_route || !mission_after_route) {
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
    if (!immediate || !immediate_mission) {
      return result;
    }
    result.event_edges =
        lifecycleEdges(*after_route, *immediate) +
        (sameMissionState(*mission_after_route, *immediate_mission) ? 0U : 1U);
  }

  const auto settle_frames = has_passage_events ? 0U : 4U;
  const auto settle_seed =
      roomSeed(mission, spec.rooms[spec.room_count - 1U], *mission_after_route);
  if (!settle_seed) {
    return result;
  }
  const auto settle = [&]() {
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
    }
    return true;
  };
  if (!settle()) {
    return result;
  }
  const auto after = vm.readBridgeState();
  const auto mission_after = vm.readMissionBridgeState();
  room = currentRoom(vm);
  if (!after || !mission_after || !room ||
      *room != spec.rooms[spec.room_count - 1U]) {
    return result;
  }
  const auto post_event_snapshot = vm.captureSnapshot();
  if (spec.event_kind != EventKind::none && !has_passage_events &&
      result.event_edges == 0U) {
    if (!vm.restoreSnapshot(pre_event_snapshot) || !settle()) {
      return result;
    }
    const auto control = vm.readBridgeState();
    const auto control_mission = vm.readMissionBridgeState();
    if (!control || !control_mission) {
      return result;
    }
    result.event_edges =
        lifecycleEdges(*control, *after) +
        (sameMissionState(*control_mission, *mission_after) ? 0U : 1U);
    if (!vm.restoreSnapshot(post_event_snapshot)) {
      return result;
    }
  }
  result.lifecycle_edges =
      std::max(result.lifecycle_edges, lifecycleEdges(*before, *after));
  result.final_room = *room;
  result.completed =
      result.room_edges + 1U == spec.room_count &&
      result.lifecycle_edges != 0U &&
      (spec.event_kind == EventKind::none ||
       (has_passage_events
            ? result.event_completed && result.passage_events == 2U &&
                  std::ranges::all_of(result.passage_instructions,
                                      [](std::uint64_t instructions) {
                                        return instructions != 0U;
                                      })
            : result.event_completed && result.event_edges != 0U));
  result.raw_cd_sector = vm.machine().cdrom().captureState().current_lba;
  result.snapshot = vm.captureSnapshot();
  return result;
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

int runProbe(const std::filesystem::path &cue_path,
             std::optional<std::uint32_t> only_mission) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "G2.3 script probe requires Syphon Filter USA v1.1"};
  }

  auto checked = std::size_t{};
  auto passed = std::size_t{};
  for (const auto &mission : sf::game::missionCatalog()) {
    if (only_mission && mission.index != *only_mission) {
      continue;
    }
    ++checked;
    const auto package = sf::game::MissionPackage::load(disc, mission.index);
    const auto &spec = route_specs[mission.index];
    if (!validateSpec(package, spec)) {
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=failed phase=matrix"
                << " initial=" << package.layout().initialRoom()
                << " route-initial=" << spec.rooms[0] << " adjacency=";
      for (std::size_t index = 1U; index < spec.room_count; ++index) {
        std::cout << (index == 1U ? "" : ",") << spec.rooms[index - 1U] << '>'
                  << spec.rooms[index] << ':'
                  << adjacent(package, spec.rooms[index - 1U],
                              spec.rooms[index]);
      }
      std::cout << " candidates=";
      for (std::size_t room = 0U; room < package.layout().modelCount();
           ++room) {
        if (!package.objects().objectsInRoom(room).empty()) {
          std::cout << room << ':'
                    << package.objects().objectsInRoom(room).size() << ',';
        }
      }
      std::cout << " broken-links=";
      for (std::size_t index = 1U; index < spec.room_count; ++index) {
        const auto left = spec.rooms[index - 1U];
        const auto right = spec.rooms[index];
        if (adjacent(package, left, right)) {
          continue;
        }
        std::cout << left << '[';
        for (std::size_t candidate = 0U;
             candidate < package.layout().modelCount(); ++candidate) {
          if (adjacent(package, left, static_cast<std::uint16_t>(candidate))) {
            std::cout << candidate << ',';
          }
        }
        std::cout << "]/" << right << '[';
        for (std::size_t candidate = 0U;
             candidate < package.layout().modelCount(); ++candidate) {
          if (adjacent(package, right, static_cast<std::uint16_t>(candidate))) {
            std::cout << candidate << ',';
          }
        }
        std::cout << "]";
      }
      std::cout << '\n';
      continue;
    }

    const auto &image = package.legacyImage();
    auto virtual_cd = image.createVirtualCd();
    sf::game::LegacyGameplayVm vm{image.executable()};
    vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
    vm.bindSyphonFilterUsaV11VirtualCdCalls(virtual_cd);
    constexpr std::uint64_t bootstrap_budget = 500'000'000U;
    const auto bootstrap = vm.bootstrapMission(
        static_cast<std::uint32_t>(mission.selection_index),
        mission.index == 0U,
        sf::game::syphonFilterUsaV11FirstMissionBootstrapProfile(),
        sf::game::syphonFilterUsaV11RetailPlatformTailProfile(),
        sf::game::syphonFilterUsaV11FirstMissionOpeningProfile(),
        bootstrap_budget);
    if (!bootstrap.completed() || !tick(vm)) {
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=failed phase=bootstrap\n";
      continue;
    }

    const auto checkpoint = vm.captureSnapshot();
    const auto first = runScenario(vm, package, spec);
    const auto restored = vm.restoreSnapshot(checkpoint);
    const auto replay =
        restored ? runScenario(vm, package, spec) : ScenarioResult{};
    const auto exact =
        first.completed && replay.completed &&
        first.final_room == replay.final_room &&
        first.room_edges == replay.room_edges &&
        first.lifecycle_edges == replay.lifecycle_edges &&
        first.event_edges == replay.event_edges &&
        first.event_instructions == replay.event_instructions &&
        first.event_completed == replay.event_completed &&
        first.passage_events == replay.passage_events &&
        first.passage_instructions == replay.passage_instructions &&
        first.raw_cd_sector == replay.raw_cd_sector &&
        replayStateEqual(first.snapshot, replay.snapshot);
    const auto replay_state_equal =
        replayStateEqual(first.snapshot, replay.snapshot);
    if (exact) {
      ++passed;
    }
    std::cout << "mission=" << mission.index
              << " resource=" << mission.resource_name
              << " result=" << (exact ? "ready" : "failed") << " route=";
    for (std::size_t index = 0U; index < spec.room_count; ++index) {
      std::cout << (index == 0U ? "" : ">") << spec.rooms[index];
    }
    std::cout << " room-edges=" << first.room_edges
              << " room=" << first.final_room
              << " lifecycle-edges=" << first.lifecycle_edges
              << " event=" << first.event_completed
              << " event-edges=" << first.event_edges
              << " event-instructions=" << first.event_instructions
              << " passage-events=" << first.passage_events
              << " passage-instructions=" << first.passage_instructions[0]
              << '/' << first.passage_instructions[1]
              << " completed=" << first.completed << " replay=" << exact
              << " replay-completed=" << replay.completed
              << " replay-room=" << replay.final_room
              << " replay-room-edges=" << replay.room_edges
              << " replay-lifecycle-edges=" << replay.lifecycle_edges
              << " replay-event=" << replay.event_completed
              << " replay-event-edges=" << replay.event_edges
              << " replay-state=" << replay_state_equal;
    if (!exact) {
      std::cout << " room-objects=";
      for (std::size_t index = 0U; index < spec.room_count; ++index) {
        std::cout << (index == 0U ? "" : ",") << spec.rooms[index] << ':'
                  << package.objects().objectsInRoom(spec.rooms[index]).size();
      }
      std::cout << " current-neighbours=";
      for (const auto candidate :
           package.layout().visibility(first.final_room).active_models) {
        std::cout << candidate << ':'
                  << package.objects().objectsInRoom(candidate).size() << ',';
      }
    }
    std::cout << '\n';
  }

  const auto expected = only_mission ? std::size_t{1U} : route_specs.size();
  if (checked != expected || passed != checked) {
    std::cerr << "G2.3 script gate failed: passed=" << passed << '/' << checked
              << " expected=" << expected << '\n';
    return 2;
  }
  std::cout << "G2.3 script gate passed: active-routes=" << passed << '/'
            << checked << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: sf_g2_script_probe <game.cue> [mission-index]\n";
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
          mission >= route_specs.size()) {
        std::cerr << "Invalid mission index\n";
        return 1;
      }
      only_mission = mission;
    }
    return runProbe(std::filesystem::path{argv[1]}, only_mission);
  } catch (const std::exception &error) {
    std::cerr << "G2.3 script gate failed: " << error.what() << '\n';
    return 10;
  }
}
