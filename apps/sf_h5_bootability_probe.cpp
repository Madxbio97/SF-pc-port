#include "sf/core/error.hpp"
#include "sf/assets/hmd_model.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/legacy_mission_image.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct MissionProbeResult {
  bool ready{};
  std::string phase;
  std::string detail;
  std::uint32_t state{};
  std::uint64_t sequence{};
  std::uint64_t gameplay_sequence{};
  std::uint64_t guest_frame{};
  std::size_t objects{};
  std::size_t gameplay_objects{};
  std::size_t resident_objects{};
  std::size_t active_models{};
  std::size_t active_objects{};
  std::size_t resident_dynamic_binding_samples{};
  std::size_t presented_dynamic_binding_samples{};
  std::size_t verified_dynamic_lifecycle_edges{};
  std::size_t neutral_room_transitions{};
  std::size_t stable_lifetime_samples{};
  std::size_t lifecycle_edges{};
};

struct DynamicBindingStats {
  std::size_t resident{};
  std::size_t presented{};
};

bool plausiblePoint(const sf::game::LegacyNativePoint &point) noexcept {
  const auto plausible = [](std::int32_t value) {
    constexpr auto limit = std::int64_t{1} << 24U;
    const auto wide = static_cast<std::int64_t>(value);
    return wide >= -limit && wide <= limit;
  };
  return plausible(point.x) && plausible(point.y) && plausible(point.z);
}

bool actorAllocated(const sf::game::LegacyObjectBridgeState &object,
                    std::uint16_t dynamic_first_slot) noexcept {
  // Class zero is the resident player. It proves that the object table is
  // readable, but must not satisfy the NPC spawn/despawn lifetime gate.
  const auto actor_class = object.class_id == 1 || object.class_id == 0x35;
  if (!actor_class || !object.resident) {
    return false;
  }
  if (object.slot < dynamic_first_slot) {
    return true;
  }
  return object.maximum_health != 0 || object.health != 0 ||
         object.attributes != 0U || object.parameter != 0 ||
         object.path_pointer != 0U || object.authored_position.x != 0 ||
         object.authored_position.y != 0 || object.authored_position.z != 0;
}

bool sameActorIdentity(const sf::game::LegacyObjectBridgeState &left,
                       const sf::game::LegacyObjectBridgeState &right) noexcept {
  return left.class_id == right.class_id &&
         left.definition == right.definition &&
         left.parameter == right.parameter &&
         left.authored_position.x == right.authored_position.x &&
         left.authored_position.y == right.authored_position.y &&
         left.authored_position.z == right.authored_position.z &&
         left.path_pointer == right.path_pointer;
}

std::optional<std::string> validatePresentationFrame(
    const sf::game::LegacyPresentationFrame &frame,
    std::uint64_t after_sequence, std::uint64_t runtime_guest_frame,
    std::size_t &resident_objects) {
  if (!sf::game::legacyPresentationFrameConsumable(frame, after_sequence) ||
      frame.guest_frame != runtime_guest_frame || !frame.renderer ||
      !frame.ui || frame.renderer->guest_frame != frame.guest_frame ||
      frame.ui->guest_frame != frame.guest_frame) {
    return "renderer/UI command frame is not coherent or fresh";
  }
  for (const auto &command : frame.commands) {
    if (command.sequence != frame.sequence) {
      return "presentation command sequence does not match its frame";
    }
  }

  const auto &render = frame.renderer->state;
  const auto &mission = frame.ui->mission;
  if (render.objects.empty() ||
      render.dynamic_first_slot > render.objects.size()) {
    return "guest object table is empty or malformed";
  }
  if (!render.player.resident || render.player.room < 0 ||
      render.player.room >= 4096 || !plausiblePoint(render.player.position)) {
    return "guest player or current room is invalid";
  }
  if (mission.player_slot < 0 ||
      static_cast<std::size_t>(mission.player_slot) >= render.objects.size()) {
    return "mission player slot is outside the guest object table";
  }
  const auto &player =
      render.objects[static_cast<std::size_t>(mission.player_slot)];
  if (player.class_id != 0 || !player.resident) {
    return "mission player slot does not identify a resident player";
  }

  resident_objects = 0U;
  for (std::size_t index = 0U; index < render.objects.size(); ++index) {
    const auto &object = render.objects[index];
    if (object.slot != index) {
      return "guest object table slot identity is malformed";
    }
    if (object.resident) {
      ++resident_objects;
      if (!plausiblePoint(object.position)) {
        return "resident guest object has an implausible position";
      }
    }
    if (object.has_target &&
        (object.target_slot < 0 ||
         static_cast<std::size_t>(object.target_slot) >=
             render.objects.size())) {
      return "resident guest object has an invalid target slot";
    }
  }
  if (resident_objects == 0U) {
    return "guest object table contains no resident objects";
  }
  return std::nullopt;
}

std::optional<std::string> validateGameplaySession(
    const sf::game::GameplaySession &gameplay, std::size_t expected_objects,
    std::uint64_t expected_sequence,
    const sf::assets::LevelLayout &layout) {
  if (gameplay.runtimeFaulted() ||
      !gameplay.legacyRenderCommandsAuthoritative()) {
    return "production gameplay lost retail runtime authority";
  }
  if (gameplay.legacyPresentationSequence() != expected_sequence) {
    return "production presentation sequence is not exactly monotonic: " +
           std::to_string(gameplay.legacyPresentationSequence()) +
           " expected=" + std::to_string(expected_sequence);
  }
  if (gameplay.objects().size() != expected_objects) {
    return "production scene object vector shape changed";
  }
  if (gameplay.models().empty() ||
      gameplay.currentRoom() >= gameplay.models().size()) {
    return "production current room is outside the world model table";
  }

  const auto active_models = gameplay.activeModels();
  if (active_models.empty() ||
      std::ranges::find(active_models, gameplay.currentRoom()) ==
          active_models.end()) {
    return "production current room is absent from the active model set";
  }
  for (const auto model : active_models) {
    if (model >= gameplay.models().size()) {
      return "production active model index is invalid";
    }
  }
  for (const auto model : layout.residentModels()) {
    if (std::ranges::find(active_models, model) == active_models.end()) {
      return "production omitted a retail always-resident world model";
    }
  }
  for (const auto model :
       layout.visibility(gameplay.currentRoom()).active_models) {
    if (std::ranges::find(active_models, model) == active_models.end()) {
      return "production omitted a model from the DAT room visibility envelope";
    }
  }
  for (const auto object : gameplay.activeObjects()) {
    if (object >= gameplay.objects().size()) {
      return "production active object index is invalid";
    }
    if (gameplay.objects()[object].model >= gameplay.objectModels().size()) {
      return "production active object model index is invalid";
    }
  }
  return std::nullopt;
}

std::optional<std::string> validateDynamicGuestBindings(
    const sf::game::GameplaySession &gameplay,
    const sf::game::LegacyGameplayBridgeState &guest,
    DynamicBindingStats &stats) {
  constexpr auto unbound_scene = std::numeric_limits<std::size_t>::max();
  const auto bindings = gameplay.legacyGuestSlotsBySceneObject();
  if (bindings.size() != gameplay.objects().size()) {
    return "guest/SceneObject binding vector shape is invalid";
  }

  auto scene_by_guest =
      std::vector<std::size_t>(guest.objects.size(), unbound_scene);
  for (std::size_t scene = 0U; scene < bindings.size(); ++scene) {
    const auto slot = bindings[scene];
    if (slot < 0) {
      continue;
    }
    if (static_cast<std::size_t>(slot) >= guest.objects.size()) {
      return "SceneObject binding references an invalid guest slot";
    }
    auto &bound_scene = scene_by_guest[static_cast<std::size_t>(slot)];
    if (bound_scene != unbound_scene) {
      return "one guest slot is bound to multiple SceneObjects";
    }
    bound_scene = scene;
  }

  const auto active_objects = gameplay.activeObjects();
  for (std::size_t slot = guest.dynamic_first_slot;
       slot < guest.objects.size(); ++slot) {
    const auto &object = guest.objects[slot];
    if (!object.resident || object.path_pointer == 0U) {
      continue;
    }
    ++stats.resident;
    const auto scene = scene_by_guest[slot];
    if (scene == unbound_scene || scene >= gameplay.objects().size() ||
        scene > std::numeric_limits<std::uint16_t>::max()) {
      return "resident dynamic guest slot has no native SceneObject binding";
    }
    const auto &native = gameplay.objects()[scene];
    if (native.class_id != static_cast<std::uint32_t>(object.class_id) ||
        !native.definition_index ||
        *native.definition_index != object.definition) {
      return "resident dynamic binding does not preserve class/definition identity";
    }

    const auto presented = object.presentation_controller == 0U ||
                           object.presentation_enabled != 0U;
    if (!presented) {
      continue;
    }
    ++stats.presented;
    const auto retail_npc =
        object.object_handler == sf::game::legacy_common_npc_handler &&
        object.ai_controller != 0U;
    const auto *actor_hmd =
        native.model < gameplay.objectModels().size()
            ? std::get_if<sf::assets::HmdModel>(
                  &gameplay.objectModels()[native.model].geometry)
            : nullptr;
    const auto actor_pose_materialized =
        actor_hmd != nullptr &&
        sf::game::legacyGuestHmdPoseComplete(object.bone_matrix_count,
                                             actor_hmd->parts().size());
    const auto native_active =
        std::ranges::find(active_objects, static_cast<std::uint16_t>(scene)) !=
        active_objects.end();
    if (retail_npc && !actor_pose_materialized && native_active) {
      return "retail NPC became active before guest pose materialization";
    }
    if (retail_npc && !native_active) {
      continue;
    }
    if (native.transform.x != object.position.x ||
        native.transform.z != object.position.z) {
      return "presented dynamic binding does not preserve retail X/Z";
    }
    if (!native_active) {
      return "presented resident dynamic binding is absent from active objects";
    }
  }
  return std::nullopt;
}

std::size_t countDynamicLifecycleEdges(
    const sf::game::LegacyGameplayBridgeState &before,
    const sf::game::LegacyGameplayBridgeState &after) noexcept {
  const auto first = std::max<std::size_t>(before.dynamic_first_slot,
                                           after.dynamic_first_slot);
  const auto count = std::min(before.objects.size(), after.objects.size());
  auto edges = std::size_t{};
  for (auto slot = first; slot < count; ++slot) {
    const auto &left = before.objects[slot];
    const auto &right = after.objects[slot];
    const auto left_allocated = left.resident && left.path_pointer != 0U;
    const auto right_allocated = right.resident && right.path_pointer != 0U;
    if (left_allocated != right_allocated ||
        (left_allocated && right_allocated &&
         (!sameActorIdentity(left, right) ||
          left.destroyed() != right.destroyed()))) {
      ++edges;
    }
  }
  return edges;
}

std::string_view
phaseName(sf::game::LegacyFirstMissionBootstrapPhase phase) noexcept {
  using Phase = sf::game::LegacyFirstMissionBootstrapPhase;
  switch (phase) {
  case Phase::reset:
    return "reset";
  case Phase::common_init:
    return "common_init";
  case Phase::pop_title:
    return "pop_title";
  case Phase::select_mission:
    return "select_mission";
  case Phase::pop_transition:
    return "pop_transition";
  case Phase::mission_init:
    return "mission_init";
  case Phase::initialize_fade:
    return "initialize_fade";
  case Phase::release_loading_ui:
    return "release_loading_ui";
  case Phase::release_loading_fade:
    return "release_loading_fade";
  case Phase::reset_loading_ui:
    return "reset_loading_ui";
  case Phase::initialize_display:
    return "initialize_display";
  case Phase::pop_loading:
    return "pop_loading";
  case Phase::start_opening:
    return "start_opening";
  case Phase::ready:
    return "ready";
  }
  return "unknown";
}

std::string executionDetail(const sf::game::LegacyGameplayVmResult &result) {
  std::ostringstream stream;
  stream << "reason=" << sf::psx::toString(result.execution.reason) << " pc=0x"
         << std::hex << result.execution.pc << std::dec
         << " instructions=" << result.execution.instructions
         << " host_calls=" << result.host_calls;
  return stream.str();
}

MissionProbeResult fail(std::string phase, std::string detail) {
  MissionProbeResult result;
  result.phase = std::move(phase);
  result.detail = std::move(detail);
  return result;
}

MissionProbeResult probeMission(sf::game::GameDisc &disc,
                                const sf::game::MissionDefinition &mission) {
  auto current_phase = std::string{"package_load"};
  try {
    auto package = sf::game::MissionPackage::load(disc, mission.index);
    const auto &image = package.legacyImage();

    current_phase = "vm_create";
    auto virtual_cd = image.createVirtualCd();
    sf::game::LegacyGameplayVm vm{image.executable()};
    vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
    vm.bindSyphonFilterUsaV11VirtualCdCalls(virtual_cd);

    if (mission.selection_index < 0) {
      return fail("mission_selection", "mission has no retail selection index");
    }

    current_phase = "bootstrap";
    constexpr std::uint64_t bootstrap_instruction_budget = 500'000'000U;
    const auto bootstrap = vm.bootstrapMission(
        static_cast<std::uint32_t>(mission.selection_index),
        mission.index == 0U,
        sf::game::syphonFilterUsaV11FirstMissionBootstrapProfile(),
        sf::game::syphonFilterUsaV11RetailPlatformTailProfile(),
        sf::game::syphonFilterUsaV11FirstMissionOpeningProfile(),
        bootstrap_instruction_budget);
    if (!bootstrap.completed()) {
      auto detail = executionDetail(bootstrap.execution);
      const auto &state = vm.runtime().state();
      std::ostringstream registers;
      registers << detail << " bridge_fault=" << bootstrap.bridge_fault
                << " ra=0x" << std::hex << state.gpr[31] << " sp=0x"
                << state.gpr[29] << " a0=0x" << state.gpr[4] << " a1=0x"
                << state.gpr[5] << " a2=0x" << state.gpr[6] << " a3=0x"
                << state.gpr[7] << std::dec;
      return fail("bootstrap/" + std::string{phaseName(bootstrap.phase)},
                  registers.str());
    }

    current_phase = "virtual_cd_mount";
    const auto expected_archive =
        "FOG/" + std::string{mission.resource_name} + ".FOG";
    if (!virtual_cd->mountedArchive() ||
        *virtual_cd->mountedArchive() != expected_archive) {
      return fail(current_phase, "retail archive is not mounted");
    }

    current_phase = "host_pad_write";
    if (!vm.writeHostPadState({})) {
      return fail(current_phase, "guest pad bridge rejected neutral state");
    }

    current_phase = "retail_first_frame";
    vm.clearPcm();
    const auto frame = vm.tickRetailOuterFrame();
    if (!frame.completed()) {
      for (std::size_t index = 0; index < frame.guest_calls.size(); ++index) {
        if (!frame.guest_calls[index].completed()) {
          return fail(current_phase + "/guest_call_" + std::to_string(index),
                      executionDetail(frame.guest_calls[index]));
        }
      }
      if (frame.renderer_tail && !frame.renderer_tail->completed()) {
        return fail(current_phase + "/renderer_tail",
                    executionDetail(*frame.renderer_tail));
      }
      if (!frame.platform_tail.delayed_callbacks.completed()) {
        return fail(current_phase + "/delayed_callbacks",
                    executionDetail(frame.platform_tail.delayed_callbacks));
      }
      return fail(
          current_phase,
          "unsupported_state=" + std::to_string(frame.unsupported_state) +
              " bridge_fault=" + std::to_string(frame.bridge_fault) +
              " state=" + std::to_string(frame.state_before) + '/' +
              std::to_string(frame.state_after));
    }

    current_phase = "audio_frame_clock";
    if (!vm.advanceAudioFrameClock()) {
      return fail(current_phase, "SPU frame clock rejected the guest frame");
    }

    current_phase = "production_runtime_bootstrap";
    auto runtime = std::make_unique<sf::game::LegacyFirstMissionRuntime>(
        package.definition(), image);
    const auto initial_frame = runtime->presentationFrame();
    if (!runtime->ready() || runtime->faulted() || !initial_frame ||
        !sf::game::legacyPresentationFrameConsumable(*initial_frame, 0U)) {
      return fail(current_phase,
                  "ready=" + std::to_string(runtime->ready()) +
                      " faulted=" + std::to_string(runtime->faulted()) +
                      " frame=" + std::to_string(initial_frame != nullptr));
    }

    current_phase = "production_runtime_checkpoint";
    if (!runtime->captureCheckpoint()) {
      return fail(current_phase, "initial checkpoint capture failed");
    }

    current_phase = "production_runtime_continuity";
    constexpr std::uint32_t retail_tick_count = 8U;
    auto previous_frame = initial_frame;
    auto previous_objects = initial_frame->renderer->state.objects;
    auto raw_frames =
        std::vector<std::shared_ptr<const sf::game::LegacyPresentationFrame>>{};
    raw_frames.reserve(static_cast<std::size_t>(retail_tick_count) + 1U);
    const auto object_count = previous_objects.size();
    const auto dynamic_first_slot =
        initial_frame->renderer->state.dynamic_first_slot;
    auto maximum_resident_objects = std::size_t{};
    auto stable_lifetime_samples = std::size_t{};
    auto lifecycle_edges = std::size_t{};

    for (std::uint32_t tick = 0U; tick <= retail_tick_count; ++tick) {
      const auto observed = runtime->presentationFrame();
      auto resident_objects = std::size_t{};
      if (runtime->faulted() || !observed) {
        return fail(current_phase,
                    "runtime fault/no frame at tick=" + std::to_string(tick));
      }
      if (const auto validation = validatePresentationFrame(
              *observed, tick == 0U ? 0U : previous_frame->sequence,
              runtime->guestFrame(), resident_objects)) {
        return fail(current_phase,
                    "tick=" + std::to_string(tick) + " " + *validation);
      }
      if (tick != 0U &&
          observed->guest_frame != previous_frame->guest_frame + 1U) {
        return fail(current_phase,
                    "retail guest frame did not advance exactly once at tick=" +
                        std::to_string(tick));
      }
      raw_frames.push_back(observed);

      const auto &render = observed->renderer->state;
      if (render.objects.size() != object_count ||
          render.dynamic_first_slot != dynamic_first_slot) {
        return fail(current_phase,
                    "guest object table shape changed at tick=" +
                        std::to_string(tick));
      }
      maximum_resident_objects =
          std::max(maximum_resident_objects, resident_objects);

      if (tick != 0U) {
        for (std::size_t index = 0U; index < render.objects.size(); ++index) {
          const auto &before = previous_objects[index];
          const auto &after = render.objects[index];
          const auto before_allocated =
              actorAllocated(before, dynamic_first_slot);
          const auto after_allocated = actorAllocated(after, dynamic_first_slot);
          const auto same_identity = sameActorIdentity(before, after);
          if (before_allocated && after_allocated && same_identity) {
            ++stable_lifetime_samples;
          }
          if (before_allocated != after_allocated ||
              (before_allocated && after_allocated && !same_identity) ||
              (before_allocated && after_allocated &&
               before.destroyed() != after.destroyed())) {
            ++lifecycle_edges;
          }
        }
      }

      previous_frame = observed;
      previous_objects = render.objects;
      if (tick != retail_tick_count) {
        runtime->setHostPadState({});
        runtime->advanceHostUpdate();
      }
    }

    const auto runtime_sequence = previous_frame->sequence;
    const auto runtime_guest_frame = previous_frame->guest_frame;

    // GameplaySession owns its own retail VM. Release the continuity VM first
    // so this all-mission gate also exercises the production memory topology.
    runtime.reset();
    current_phase = "production_gameplay_bootstrap";
    auto gameplay = std::make_unique<sf::game::GameplaySession>(package);
    const auto gameplay_object_count = gameplay->objects().size();
    auto expected_gameplay_sequence =
        gameplay->legacyPresentationSequence();
    auto resident_dynamic_binding_samples = std::size_t{};
    auto presented_dynamic_binding_samples = std::size_t{};
    auto verified_dynamic_lifecycle_edges = std::size_t{};
    auto neutral_room_transitions = std::size_t{};
    auto previous_room = gameplay->currentRoom();
    if (expected_gameplay_sequence == 0U) {
      return fail(current_phase,
                  "production gameplay published no initial frame");
    }
    if (const auto validation = validateGameplaySession(
            *gameplay, gameplay_object_count, expected_gameplay_sequence,
            package.layout())) {
      return fail(current_phase, *validation);
    }
    auto initial_binding_stats = DynamicBindingStats{};
    if (raw_frames.empty() || !raw_frames.front()->renderer) {
      return fail(current_phase, "raw frame history is unavailable");
    }
    if (const auto validation = validateDynamicGuestBindings(
            *gameplay, raw_frames.front()->renderer->state,
            initial_binding_stats)) {
      return fail(current_phase, *validation);
    }
    resident_dynamic_binding_samples += initial_binding_stats.resident;
    presented_dynamic_binding_samples += initial_binding_stats.presented;

    current_phase = "production_gameplay_continuity";
    for (std::uint32_t tick = 1U; tick <= retail_tick_count; ++tick) {
      if (expected_gameplay_sequence ==
          std::numeric_limits<std::uint64_t>::max()) {
        return fail(current_phase, "production sequence overflowed");
      }
      ++expected_gameplay_sequence;
      gameplay->update({});
      if (gameplay->currentRoom() != previous_room) {
        ++neutral_room_transitions;
        previous_room = gameplay->currentRoom();
      }
      if (const auto validation = validateGameplaySession(
              *gameplay, gameplay_object_count,
              expected_gameplay_sequence,
              package.layout())) {
        return fail(current_phase,
                    "tick=" + std::to_string(tick) + " " + *validation);
      }
      if (tick >= raw_frames.size() || !raw_frames[tick]->renderer) {
        return fail(current_phase,
                    "raw frame history is incomplete at tick=" +
                        std::to_string(tick));
      }
      auto binding_stats = DynamicBindingStats{};
      if (const auto validation = validateDynamicGuestBindings(
              *gameplay, raw_frames[tick]->renderer->state, binding_stats)) {
        return fail(current_phase,
                    "tick=" + std::to_string(tick) + " " + *validation);
      }
      resident_dynamic_binding_samples += binding_stats.resident;
      presented_dynamic_binding_samples += binding_stats.presented;
      verified_dynamic_lifecycle_edges += countDynamicLifecycleEdges(
          raw_frames[tick - 1U]->renderer->state,
          raw_frames[tick]->renderer->state);
    }

    current_phase = "production_gameplay_restart_checkpoint";
    ++expected_gameplay_sequence;
    if (!gameplay->restartCheckpoint()) {
      return fail(current_phase, "production checkpoint restore failed");
    }
    if (const auto validation = validateGameplaySession(
            *gameplay, gameplay_object_count, expected_gameplay_sequence,
            package.layout())) {
      return fail(current_phase, *validation);
    }
    const auto restart_raw_tick = mission.index == 0U ? 0U : 1U;
    auto restart_binding_stats = DynamicBindingStats{};
    if (restart_raw_tick >= raw_frames.size() ||
        !raw_frames[restart_raw_tick]->renderer) {
      return fail(current_phase, "restart reference frame is unavailable");
    }
    if (const auto validation = validateDynamicGuestBindings(
            *gameplay, raw_frames[restart_raw_tick]->renderer->state,
            restart_binding_stats)) {
      return fail(current_phase, *validation);
    }
    ++expected_gameplay_sequence;
    gameplay->update({});
    if (const auto validation = validateGameplaySession(
            *gameplay, gameplay_object_count, expected_gameplay_sequence,
            package.layout())) {
      return fail(current_phase, "post-restart update: " + *validation);
    }
    auto restarted_update_binding_stats = DynamicBindingStats{};
    if (const auto validation = validateDynamicGuestBindings(
            *gameplay, raw_frames[restart_raw_tick + 1U]->renderer->state,
            restarted_update_binding_stats)) {
      return fail(current_phase, "post-restart update: " + *validation);
    }

    current_phase = "production_gameplay_reset";
    ++expected_gameplay_sequence;
    gameplay->reset();
    if (const auto validation = validateGameplaySession(
            *gameplay, gameplay_object_count, expected_gameplay_sequence,
            package.layout())) {
      return fail(current_phase, *validation);
    }
    auto reset_binding_stats = DynamicBindingStats{};
    if (const auto validation = validateDynamicGuestBindings(
            *gameplay, raw_frames.front()->renderer->state,
            reset_binding_stats)) {
      return fail(current_phase, *validation);
    }
    ++expected_gameplay_sequence;
    gameplay->update({});
    if (const auto validation = validateGameplaySession(
            *gameplay, gameplay_object_count, expected_gameplay_sequence,
            package.layout())) {
      return fail(current_phase, "post-reset update: " + *validation);
    }
    auto reset_update_binding_stats = DynamicBindingStats{};
    if (const auto validation = validateDynamicGuestBindings(
            *gameplay, raw_frames[1U]->renderer->state,
            reset_update_binding_stats)) {
      return fail(current_phase, "post-reset update: " + *validation);
    }

    MissionProbeResult result;
    result.ready = true;
    result.phase = "ready";
    result.state = frame.state_after;
    result.sequence = runtime_sequence;
    result.gameplay_sequence = expected_gameplay_sequence;
    result.guest_frame = runtime_guest_frame;
    result.objects = object_count;
    result.gameplay_objects = gameplay_object_count;
    result.resident_objects = maximum_resident_objects;
    result.active_models = gameplay->activeModels().size();
    result.active_objects = gameplay->activeObjects().size();
    result.resident_dynamic_binding_samples =
        resident_dynamic_binding_samples;
    result.presented_dynamic_binding_samples =
        presented_dynamic_binding_samples;
    result.verified_dynamic_lifecycle_edges =
        verified_dynamic_lifecycle_edges;
    result.neutral_room_transitions = neutral_room_transitions;
    result.stable_lifetime_samples = stable_lifetime_samples;
    result.lifecycle_edges = lifecycle_edges;
    return result;
  } catch (const std::exception &error) {
    return fail(std::move(current_phase), error.what());
  }
}

int runProbe(const std::filesystem::path &cue_path,
             std::optional<std::uint32_t> only_mission) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{
        sf::core::ErrorCode::unsupported,
        "G2 retail continuity probe requires Syphon Filter USA v1.1"};
  }

  constexpr auto retail_mission_count = std::size_t{20U};
  const auto missions = sf::game::missionCatalog();
  if (missions.size() != retail_mission_count) {
    std::cerr << "G2 retail continuity gate requires all 20 missions: catalog="
              << missions.size() << '\n';
    return 2;
  }

  std::size_t checked{};
  std::size_t ready{};
  for (const auto &mission : missions) {
    if (only_mission && mission.index != *only_mission) {
      continue;
    }
    ++checked;
    const auto result = probeMission(disc, mission);
    if (result.ready) {
      ++ready;
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=ready scope=neutral-smoke phase=" << result.phase
                << " state=" << result.state << " sequence=" << result.sequence
                << " gameplay-sequence=" << result.gameplay_sequence
                << " guest-frame=" << result.guest_frame
                << " objects=" << result.objects
                << " gameplay-objects=" << result.gameplay_objects
                << " resident=" << result.resident_objects
                << " active-models=" << result.active_models
                << " active-objects=" << result.active_objects
                << " dynamic-bindings="
                << result.resident_dynamic_binding_samples
                << " presented-dynamic-bindings="
                << result.presented_dynamic_binding_samples
                << " verified-dynamic-lifecycle-edges="
                << result.verified_dynamic_lifecycle_edges
                << " neutral-room-transitions="
                << result.neutral_room_transitions
                << " stable-lifetimes=" << result.stable_lifetime_samples
                << " lifecycle-edges=" << result.lifecycle_edges
                << '\n';
    } else {
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=failed phase=" << result.phase
                << " detail=" << std::quoted(result.detail) << '\n';
    }
  }

  const auto expected = only_mission ? std::size_t{1U} : retail_mission_count;
  if (checked != expected || ready != checked) {
    std::cerr << "G2 retail continuity gate failed: ready=" << ready << '/'
              << checked << " expected=" << expected
              << '\n';
    return 2;
  }
  std::cout << "G2 retail continuity gate passed: scope=neutral-smoke ready="
            << ready << '/' << checked << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: sf_h5_bootability_probe <game.cue> [mission-index]\n";
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
    std::cerr << "G2 retail continuity gate failed: " << error.what() << '\n';
    return 10;
  }
}
