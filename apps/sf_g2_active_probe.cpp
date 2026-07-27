#include "sf/core/error.hpp"
#include "sf/assets/hmd_model.hpp"
#include "sf/assets/level_layout.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t fnv_offset = 1469598103934665603ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

class DigestBuilder final {
public:
  template <typename T> void integral(T raw) noexcept {
    if constexpr (std::is_enum_v<T>) {
      integral(static_cast<std::underlying_type_t<T>>(raw));
    } else {
      static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
      using Unsigned = std::make_unsigned_t<T>;
      const auto value = static_cast<Unsigned>(raw);
      for (std::size_t byte_index = 0U; byte_index < sizeof(Unsigned);
           ++byte_index) {
        byte(static_cast<std::uint8_t>(value >> (byte_index * 8U)));
      }
    }
  }

  void boolean(bool value) noexcept {
    byte(static_cast<std::uint8_t>(value ? 1U : 0U));
  }

  void text(std::string_view value) noexcept {
    integral(static_cast<std::uint64_t>(value.size()));
    for (const auto character : value) {
      byte(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
  }

  [[nodiscard]] std::uint64_t finish() const noexcept { return value_; }

private:
  void byte(std::uint8_t value) noexcept {
    value_ ^= value;
    value_ *= fnv_prime;
  }

  std::uint64_t value_{fnv_offset};
};

template <typename T, std::size_t Size>
void mixIntegralArray(DigestBuilder &digest,
                      const std::array<T, Size> &values) noexcept {
  digest.integral(static_cast<std::uint64_t>(Size));
  for (const auto value : values) {
    digest.integral(value);
  }
}

void mixPoint(DigestBuilder &digest,
              const sf::game::LegacyNativePoint &point) noexcept {
  digest.integral(point.x);
  digest.integral(point.y);
  digest.integral(point.z);
}

void mixMatrix(DigestBuilder &digest,
               const sf::game::LegacyNativeMatrix &matrix) noexcept {
  mixIntegralArray(digest, matrix.rotation);
  mixPoint(digest, matrix.translation);
}

void mixObject(DigestBuilder &digest,
               const sf::game::LegacyObjectBridgeState &object) noexcept {
  digest.integral(object.slot);
  digest.integral(object.definition);
  digest.integral(object.class_id);
  digest.integral(object.attributes);
  digest.integral(object.parameter);
  digest.integral(object.linked_slot);
  digest.integral(object.maximum_health);
  digest.integral(object.health);
  mixPoint(digest, object.authored_position);
  digest.integral(object.path_pointer);
  digest.integral(object.instance);
  digest.integral(object.root_node);
  digest.integral(object.pose_flags);
  digest.integral(object.motion_controller);
  digest.integral(object.presentation_controller);
  digest.integral(object.target_controller);
  digest.integral(object.health_controller);
  digest.integral(object.ai_controller);
  digest.integral(object.instance_flags);
  mixIntegralArray(digest, object.instance_state);
  digest.integral(object.ai_flags);
  digest.integral(object.ai_fire_latch);
  digest.integral(object.ai_route_node);
  digest.integral(object.ai_previous_route_node);
  digest.integral(object.ai_route_flags);
  digest.integral(object.ai_mode);
  digest.integral(object.ai_archetype);
  digest.integral(object.ai_combat_mode);
  digest.integral(object.ai_pool_index);
  digest.integral(object.ai_state);
  digest.integral(object.target_slot);
  digest.integral(object.target_flags);
  digest.integral(object.target_meter);
  digest.integral(object.danger_q12);
  digest.integral(object.presentation_enabled);
  digest.integral(object.presentation_mode);
  digest.boolean(object.resident);
  digest.boolean(object.simulated);
  digest.boolean(object.has_target);
  mixPoint(digest, object.position);
  mixIntegralArray(digest, object.guest_rotation);
  digest.integral(static_cast<std::uint64_t>(object.bone_matrices.size()));
  for (const auto &matrix : object.bone_matrices) {
    mixMatrix(digest, matrix);
  }
  digest.integral(object.bone_matrix_count);
  digest.integral(object.ground_contact_y);
  digest.boolean(object.ground_contact_valid);
}

void mixInventory(
    DigestBuilder &digest,
    const sf::game::LegacyInventoryBridgeState &inventory) noexcept {
  digest.integral(inventory.current_weapon);
  digest.integral(inventory.owned_weapons);
  mixIntegralArray(digest, inventory.magazines);
  mixIntegralArray(digest, inventory.reserves);
}

void mixMission(DigestBuilder &digest,
                const sf::game::LegacyMissionBridgeState &mission) noexcept {
  digest.integral(mission.player_slot);
  digest.integral(mission.player_health);
  digest.integral(mission.player_armor);
  digest.integral(mission.objective_count);
  digest.integral(mission.parameter_count);
  digest.integral(static_cast<std::uint64_t>(mission.objective_texts.size()));
  for (const auto &text : mission.objective_texts) {
    digest.text(text);
  }
  digest.integral(static_cast<std::uint64_t>(mission.parameter_texts.size()));
  for (const auto &text : mission.parameter_texts) {
    digest.text(text);
  }
  digest.integral(mission.completed_objectives);
  digest.integral(mission.failed_objectives);
  digest.integral(mission.revealed_objectives);
  digest.integral(mission.notified_objectives);
  digest.integral(mission.failed_parameters);
  digest.integral(mission.parameter_mask);
  digest.integral(mission.weapon_menu_state);
  digest.boolean(mission.weapon_menu_dirty);
  digest.boolean(mission.weapon_menu_controller_ready);
  digest.boolean(mission.weapon_menu_input_ready);
  mixInventory(digest, mission.inventory);
  digest.boolean(mission.success);
  digest.boolean(mission.terminal);
  digest.boolean(mission.failure);
  digest.boolean(mission.failure_transition);
}

[[nodiscard]] std::uint64_t
presentationDigest(const sf::game::LegacyPresentationFrame &frame) noexcept {
  DigestBuilder digest;
  digest.integral(frame.sequence);
  digest.integral(frame.guest_frame);
  digest.boolean(frame.renderer.has_value());
  if (frame.renderer) {
    const auto &renderer = *frame.renderer;
    const auto &state = renderer.state;
    digest.integral(renderer.guest_frame);
    mixPoint(digest, state.camera.eye);
    mixPoint(digest, state.camera.target);
    digest.integral(state.camera.projection);
    digest.integral(state.camera.fov_raw);
    digest.integral(state.camera.mode);
    digest.boolean(state.camera.scripted);
    digest.boolean(state.camera.locked);
    digest.integral(state.fade.step);
    digest.integral(state.fade.current);
    digest.integral(state.fade.floor);
    digest.integral(state.fade.callback);
    digest.boolean(state.fade.initialized);
    mixPoint(digest, state.player.position);
    mixIntegralArray(digest, state.player.guest_rotation);
    digest.integral(state.player.room);
    digest.boolean(state.player.resident);
    digest.boolean(state.player.control_locked);
    digest.integral(state.world_model_count);
    digest.integral(
        static_cast<std::uint64_t>(state.active_world_models.size()));
    for (const auto model : state.active_world_models) {
      digest.integral(model);
    }
    digest.integral(
        static_cast<std::uint64_t>(state.resident_world_models.size()));
    for (const auto model : state.resident_world_models) {
      digest.integral(model);
    }
    digest.boolean(state.target_lock_active);
    digest.integral(state.taser_conductor_phase);
    digest.integral(state.taser_target_slot);
    digest.integral(state.target_hit_result);
    digest.integral(state.aimed_target_slot);
    digest.integral(state.proximity_target_slot);
    mixIntegralArray(digest, state.tracked_slots);
    digest.integral(state.dynamic_first_slot);
    digest.integral(static_cast<std::uint64_t>(state.objects.size()));
    for (const auto &object : state.objects) {
      mixObject(digest, object);
    }
    digest.integral(static_cast<std::uint64_t>(state.expl_particles.size()));
    for (const auto &particle : state.expl_particles) {
      mixPoint(digest, particle.position);
      digest.integral(particle.controller);
      digest.integral(particle.source_slot);
      digest.integral(particle.family);
      digest.integral(particle.scale_byte);
      digest.integral(particle.frame);
      digest.integral(particle.red);
      digest.integral(particle.green);
      digest.integral(particle.blue);
    }
    digest.integral(static_cast<std::uint64_t>(state.world_callouts.size()));
    for (const auto &callout : state.world_callouts) {
      digest.integral(callout.guest_slot);
      digest.text(callout.text);
      digest.boolean(callout.headshot);
    }
  }

  digest.boolean(frame.ui.has_value());
  if (frame.ui) {
    const auto &ui = *frame.ui;
    digest.integral(ui.guest_frame);
    mixMission(digest, ui.mission);
    digest.integral(ui.target.guest_slot);
    digest.integral(ui.target.target_slot);
    digest.integral(ui.target.aimed_target_slot);
    digest.integral(ui.target.proximity_target_slot);
    digest.integral(ui.target.target_meter);
    digest.integral(ui.target.target_flags);
    digest.integral(ui.target.hit_result);
    digest.boolean(ui.target.active);
    digest.boolean(ui.target.headshot);
    digest.integral(static_cast<std::uint64_t>(ui.threats.size()));
    for (const auto &threat : ui.threats) {
      digest.integral(threat.guest_slot);
      digest.integral(threat.target_slot);
      digest.integral(threat.health);
      digest.integral(threat.ai_state);
      digest.integral(threat.danger_q12);
      digest.boolean(threat.resident);
      digest.boolean(threat.has_target);
    }
    digest.integral(static_cast<std::uint64_t>(ui.world_callouts.size()));
    for (const auto &callout : ui.world_callouts) {
      digest.integral(callout.guest_slot);
      digest.text(callout.text);
      digest.boolean(callout.headshot);
    }
  }

  digest.integral(static_cast<std::uint64_t>(frame.commands.size()));
  for (const auto &command : frame.commands) {
    digest.integral(command.type);
    digest.integral(command.sequence);
  }
  return digest.finish();
}

struct PcmDigest {
  std::uint64_t frames{};
  std::uint64_t hash{fnv_offset};

  [[nodiscard]] friend constexpr bool
  operator==(const PcmDigest &, const PcmDigest &) noexcept = default;
};

PcmDigest takePcmDigest(sf::game::GameplaySession &gameplay) noexcept {
  PcmDigest digest;
  std::array<sf::psx::SpuPcmFrame, 4096U> buffer{};
  for (;;) {
    const auto count = gameplay.takePcm(buffer);
    digest.frames += count;
    for (std::size_t index = 0U; index < count; ++index) {
      for (const auto sample : {buffer[index].left, buffer[index].right}) {
        const auto value = static_cast<std::uint16_t>(sample);
        digest.hash ^= static_cast<std::uint8_t>(value);
        digest.hash *= fnv_prime;
        digest.hash ^= static_cast<std::uint8_t>(value >> 8U);
        digest.hash *= fnv_prime;
      }
    }
    if (count != buffer.size()) {
      return digest;
    }
  }
}

bool plausiblePoint(const sf::game::LegacyNativePoint &point) noexcept {
  constexpr auto limit = std::int64_t{1} << 24U;
  const auto plausible = [](std::int32_t value) {
    const auto wide = static_cast<std::int64_t>(value);
    return wide >= -limit && wide <= limit;
  };
  return plausible(point.x) && plausible(point.y) && plausible(point.z);
}

std::optional<std::string>
validateSession(const sf::game::GameplaySession &gameplay,
                const sf::assets::LevelLayout &layout,
                std::uint64_t after_sequence) {
  if (gameplay.runtimeFaulted() ||
      !gameplay.legacyRenderCommandsAuthoritative()) {
    return "production gameplay lost retail authority";
  }
  const auto frame = gameplay.legacyPresentationFrame();
  if (!frame) {
    return "production gameplay published no immutable frame";
  }
  if (!sf::game::legacyPresentationFrameConsumable(*frame, after_sequence) ||
      frame->sequence != gameplay.legacyPresentationSequence() ||
      frame->sequence != after_sequence + 1U || !frame->renderer ||
      !frame->ui || frame->renderer->guest_frame != frame->guest_frame ||
      frame->ui->guest_frame != frame->guest_frame) {
    return "immutable presentation frame is stale, split or non-monotonic: " +
           std::to_string(after_sequence) + "->" +
           std::to_string(frame->sequence) +
           " session=" + std::to_string(gameplay.legacyPresentationSequence()) +
           " guest=" + std::to_string(frame->guest_frame);
  }
  for (const auto &command : frame->commands) {
    if (command.sequence != frame->sequence) {
      return "presentation command sequence is incoherent";
    }
  }

  const auto &guest = frame->renderer->state;
  const auto &mission = frame->ui->mission;
  if (guest.objects.empty() ||
      guest.dynamic_first_slot > guest.objects.size() ||
      !guest.player.resident || guest.player.room < 0 ||
      !plausiblePoint(guest.player.position)) {
    return "guest player/object table is malformed";
  }
  if (mission.player_slot < 0 ||
      static_cast<std::size_t>(mission.player_slot) >= guest.objects.size() ||
      guest.objects[static_cast<std::size_t>(mission.player_slot)].class_id !=
          0 ||
      !guest.objects[static_cast<std::size_t>(mission.player_slot)].resident) {
    return "retail mission player slot is invalid";
  }

  if (gameplay.models().empty() ||
      gameplay.currentRoom() >= gameplay.models().size()) {
    return "production room is outside the world model table";
  }
  const auto active_models = gameplay.activeModels();
  const auto retail_active_models =
      sf::game::legacyActiveWorldModels(guest, gameplay.models().size());
  auto expected_active_models = std::vector<std::uint16_t>{};
  const auto add_expected = [&expected_active_models](std::uint16_t model) {
    if (std::ranges::find(expected_active_models, model) ==
        expected_active_models.end()) {
      expected_active_models.push_back(model);
    }
  };
  for (const auto model : layout.residentModels()) {
    add_expected(model);
  }
  add_expected(gameplay.currentRoom());
  for (const auto model :
       layout.visibility(gameplay.currentRoom()).active_models) {
    add_expected(model);
  }
  if (retail_active_models) {
    for (const auto model : *retail_active_models) {
      add_expected(model);
    }
  }
  if (!retail_active_models || active_models.empty() ||
      std::ranges::find(active_models, gameplay.currentRoom()) ==
          active_models.end() ||
      !std::ranges::equal(active_models, expected_active_models)) {
    return "production active models diverged from the DAT/native visibility envelope";
  }
  for (const auto model : active_models) {
    if (model >= gameplay.models().size()) {
      return "active model index is invalid";
    }
  }
  for (const auto resident : guest.resident_world_models) {
    if (std::ranges::find(active_models, resident) == active_models.end()) {
      return "retail resident world model is absent from the active set";
    }
  }
  const auto active_objects = gameplay.activeObjects();
  for (const auto scene : active_objects) {
    if (scene >= gameplay.objects().size() ||
        gameplay.objects()[scene].model >= gameplay.objectModels().size()) {
      return "active object/model index is invalid";
    }
  }

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

  for (std::size_t slot = 0U; slot < guest.objects.size(); ++slot) {
    const auto &object = guest.objects[slot];
    if (object.slot != slot) {
      return "guest object slot identity is malformed";
    }
    if (object.resident && !plausiblePoint(object.position)) {
      return "resident guest object position is implausible";
    }
    if (object.has_target && (object.target_slot < 0 ||
                              static_cast<std::size_t>(object.target_slot) >=
                                  guest.objects.size())) {
      return "guest target slot is invalid";
    }
    if (slot < guest.dynamic_first_slot || !object.resident ||
        object.path_pointer == 0U) {
      continue;
    }
    const auto scene = scene_by_guest[slot];
    if (scene == unbound_scene || scene >= gameplay.objects().size()) {
      return "resident dynamic guest object has no native binding";
    }
    const auto &native = gameplay.objects()[scene];
    if (native.class_id != static_cast<std::uint32_t>(object.class_id) ||
        !native.definition_index ||
        *native.definition_index != object.definition) {
      return "dynamic binding changed retail class/definition identity";
    }
    const auto presented = object.presentation_controller == 0U ||
                           object.presentation_enabled != 0U;
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
    if (presented && !retail_npc && !native_active) {
      return "presented resident dynamic object is not active";
    }
    if (retail_npc && !actor_pose_materialized && native_active) {
      return "retail NPC became active before guest pose materialization";
    }
  }
  return std::nullopt;
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

bool exactRetailNpcPose(
    const sf::game::GameplaySession &gameplay,
    const sf::game::LegacyPresentationFrame &frame,
    std::size_t scene) noexcept {
  if (!frame.renderer || scene >= gameplay.objects().size()) {
    return false;
  }
  const auto bindings = gameplay.legacyGuestSlotsBySceneObject();
  if (scene >= bindings.size() || bindings[scene] < 0) {
    return false;
  }
  const auto slot = static_cast<std::size_t>(bindings[scene]);
  const auto &guest_objects = frame.renderer->state.objects;
  if (slot >= guest_objects.size()) {
    return false;
  }
  const auto &guest = guest_objects[slot];
  if (guest.object_handler != sf::game::legacy_common_npc_handler ||
      guest.ai_controller == 0U) {
    return false;
  }
  const auto &native = gameplay.objects()[scene];
  const auto *hmd = native.model < gameplay.objectModels().size()
                        ? std::get_if<sf::assets::HmdModel>(
                              &gameplay.objectModels()[native.model].geometry)
                        : nullptr;
  return hmd != nullptr && sf::game::legacyGuestHmdPoseComplete(
                               guest.bone_matrix_count, hmd->parts().size());
}

std::int32_t signedHeadingDelta(std::int32_t before,
                                std::int32_t after) noexcept {
  constexpr auto circle = sf::game::heading_angle_units;
  auto delta = (after - before) % circle;
  if (delta > circle / 2) {
    delta -= circle;
  } else if (delta < -circle / 2) {
    delta += circle;
  }
  return delta;
}

struct TraceSample {
  sf::game::GameplayInput input;
  std::uint64_t frame_digest{};
  PcmDigest pcm;
};

struct ActiveMetrics {
  std::uint32_t control_wait_updates{};
  std::uint32_t active_updates{};
  std::uint32_t retail_pose_materializations{};
  std::uint32_t post_materialization_updates{};
  std::size_t room_edges{};
  std::size_t maximum_resident{};
  std::size_t maximum_active{};
  double maximum_displacement{};
  std::int32_t positive_turn{};
  std::int32_t negative_turn{};
};

struct ActiveTrace {
  std::uint64_t initial_frame_digest{};
  std::uint64_t initial_sequence{};
  std::vector<TraceSample> samples;
  ActiveMetrics metrics;
};

std::optional<std::string> validateDedicatedMissionWeapon(
    const sf::game::GameplaySession &gameplay, std::uint32_t mission_index) {
  if (mission_index != 4U) {
    return std::nullopt;
  }
  const auto hans = std::ranges::find(gameplay.objects(), 9U,
                                      &sf::game::SceneObject::source_index);
  if (hans == gameplay.objects().end()) {
    return "PARK2 HANS scene object is absent";
  }
  const auto scene = static_cast<std::uint16_t>(
      std::distance(gameplay.objects().begin(), hans));
  if (gameplay.legacyDedicatedActorWeapon(scene) !=
      sf::game::WeaponId::flamethrower) {
    return "PARK2 HANS lost the retail flamethrower attachment";
  }
  return std::nullopt;
}

class TraceCapture final {
public:
  TraceCapture(sf::game::GameplaySession &gameplay,
               const sf::assets::LevelLayout &layout, ActiveTrace &trace)
      : gameplay_(gameplay), layout_(layout), trace_(trace) {}

  std::optional<std::string> initialize() {
    gameplay_.clearPcm();
    const auto frame = gameplay_.legacyPresentationFrame();
    if (!frame) {
      return "production gameplay published no initial frame";
    }
    if (frame->sequence == 0U) {
      return "production gameplay published sequence zero";
    }
    if (const auto validation =
            validateSession(gameplay_, layout_, frame->sequence - 1U)) {
      return *validation;
    }
    trace_.initial_frame_digest = presentationDigest(*frame);
    trace_.initial_sequence = frame->sequence;
    previous_sequence_ = frame->sequence;
    previous_room_ = gameplay_.currentRoom();
    previous_exact_pose_.resize(gameplay_.objects().size());
    for (std::size_t scene = 0U; scene < previous_exact_pose_.size(); ++scene) {
      previous_exact_pose_[scene] =
          exactRetailNpcPose(gameplay_, *frame, scene);
    }
    return std::nullopt;
  }

  std::optional<std::string> step(const sf::game::GameplayInput &input) {
    gameplay_.update(input);
    if (!gameplay_.advanceAudioFrameClock()) {
      return "production audio/hardware clock stopped";
    }
    if (const auto validation =
            validateSession(gameplay_, layout_, previous_sequence_)) {
      return *validation;
    }
    const auto frame = gameplay_.legacyPresentationFrame();
    if (!frame) {
      return "production gameplay dropped its immutable frame";
    }
    trace_.samples.push_back(TraceSample{input, presentationDigest(*frame),
                                         takePcmDigest(gameplay_)});
    previous_sequence_ = frame->sequence;
    if (gameplay_.currentRoom() != previous_room_) {
      ++trace_.metrics.room_edges;
      previous_room_ = gameplay_.currentRoom();
    }
    const auto resident = static_cast<std::size_t>(std::ranges::count_if(
        frame->renderer->state.objects,
        [](const auto &object) { return object.resident; }));
    trace_.metrics.maximum_resident =
        std::max(trace_.metrics.maximum_resident, resident);
    trace_.metrics.maximum_active = std::max(trace_.metrics.maximum_active,
                                             gameplay_.activeObjects().size());
    if (previous_exact_pose_.size() != gameplay_.objects().size()) {
      previous_exact_pose_.assign(gameplay_.objects().size(), false);
    }
    const auto active = gameplay_.activeObjects();
    for (std::size_t scene = 0U; scene < previous_exact_pose_.size(); ++scene) {
      const auto exact_pose = exactRetailNpcPose(gameplay_, *frame, scene);
      const auto native_active =
          std::ranges::find(active, static_cast<std::uint16_t>(scene)) !=
          active.end();
      if (!previous_exact_pose_[scene] && exact_pose && native_active) {
        ++trace_.metrics.retail_pose_materializations;
      }
      previous_exact_pose_[scene] = exact_pose;
    }
    if (trace_.metrics.retail_pose_materializations != 0U) {
      ++trace_.metrics.post_materialization_updates;
    }
    return std::nullopt;
  }

private:
  sf::game::GameplaySession &gameplay_;
  const sf::assets::LevelLayout &layout_;
  ActiveTrace &trace_;
  std::uint64_t previous_sequence_{};
  std::uint16_t previous_room_{};
  std::vector<bool> previous_exact_pose_;
};

std::optional<std::string>
captureActiveTrace(const sf::game::MissionPackage &package,
                   ActiveTrace &trace) {
  auto gameplay = std::make_unique<sf::game::GameplaySession>(package);
  TraceCapture capture{*gameplay, package.layout(), trace};
  if (const auto initialized = capture.initialize()) {
    return *initialized;
  }

  constexpr std::uint32_t maximum_control_wait_updates = 2'000U;
  constexpr std::uint32_t stable_control_updates = 8U;
  auto stable_control = std::uint32_t{};
  while (stable_control < stable_control_updates &&
         trace.metrics.control_wait_updates < maximum_control_wait_updates) {
    if (const auto stepped = capture.step({})) {
      return "control wait: " + *stepped;
    }
    ++trace.metrics.control_wait_updates;
    stable_control = controlReady(*gameplay) ? stable_control + 1U : 0U;
  }
  if (stable_control != stable_control_updates) {
    return "retail control/camera lock did not release";
  }
  if (package.definition().index == 0U &&
      (trace.metrics.retail_pose_materializations == 0U ||
       trace.metrics.post_materialization_updates < stable_control_updates)) {
    return "retail renderer did not materialize and sustain an NPC pose";
  }

  constexpr std::uint32_t turn_updates = 24U;
  const auto initial_yaw = gameplay->player().yaw;
  for (std::uint32_t update = 0U; update < turn_updates; ++update) {
    if (const auto stepped =
            capture.step(sf::game::GameplayInput{.turn = 1.0})) {
      return "positive turn: " + *stepped;
    }
    ++trace.metrics.active_updates;
  }
  const auto positive_yaw = gameplay->player().yaw;
  trace.metrics.positive_turn = signedHeadingDelta(initial_yaw, positive_yaw);

  for (std::uint32_t update = 0U; update < turn_updates; ++update) {
    if (const auto stepped =
            capture.step(sf::game::GameplayInput{.turn = -1.0})) {
      return "negative turn: " + *stepped;
    }
    ++trace.metrics.active_updates;
  }
  const auto negative_yaw = gameplay->player().yaw;
  trace.metrics.negative_turn = signedHeadingDelta(positive_yaw, negative_yaw);
  if (trace.metrics.positive_turn == 0 || trace.metrics.negative_turn == 0 ||
      static_cast<std::int64_t>(trace.metrics.positive_turn) *
              static_cast<std::int64_t>(trace.metrics.negative_turn) >=
          0) {
    auto detail = std::string{"retail player did not respond oppositely to "
                              "left/right PAD turn: "} +
                  std::to_string(initial_yaw) + "->" +
                  std::to_string(positive_yaw) + "->" +
                  std::to_string(negative_yaw) +
                  " delta=" + std::to_string(trace.metrics.positive_turn) +
                  '/' + std::to_string(trace.metrics.negative_turn) +
                  " wait=" + std::to_string(trace.metrics.control_wait_updates);
    const auto frame = gameplay->legacyPresentationFrame();
    if (frame && frame->renderer && frame->ui) {
      const auto slot = frame->ui->mission.player_slot;
      if (slot >= 0 && static_cast<std::size_t>(slot) <
                           frame->renderer->state.objects.size()) {
        const auto &object =
            frame->renderer->state.objects[static_cast<std::size_t>(slot)];
        detail += " player=" + std::to_string(slot) +
                  " resident=" + std::to_string(object.resident) +
                  " simulated=" + std::to_string(object.simulated) +
                  " instance=" + std::to_string(object.instance) +
                  " motion=" + std::to_string(object.motion_controller) +
                  " ai=" + std::to_string(object.ai_controller) +
                  " state=" + std::to_string(object.ai_state) +
                  " position=" + std::to_string(object.position.x) + ',' +
                  std::to_string(object.position.y) + ',' +
                  std::to_string(object.position.z);
      }
    }
    return detail;
  }

  constexpr std::uint32_t direction_attempts = 8U;
  constexpr std::uint32_t movement_updates = 16U;
  constexpr std::uint32_t direction_turn_updates = 8U;
  constexpr double required_displacement = 128.0;
  auto moved = false;
  for (std::uint32_t attempt = 0U; attempt < direction_attempts && !moved;
       ++attempt) {
    const auto origin = gameplay->player();
    for (std::uint32_t update = 0U; update < movement_updates; ++update) {
      if (const auto stepped =
              capture.step(sf::game::GameplayInput{.move = 1.0, .run = true})) {
        return "locomotion: " + *stepped;
      }
      ++trace.metrics.active_updates;
      const auto current = gameplay->player();
      trace.metrics.maximum_displacement =
          std::max(trace.metrics.maximum_displacement,
                   std::hypot(current.x - origin.x, current.z - origin.z));
    }
    moved = trace.metrics.maximum_displacement >= required_displacement;
    if (moved) {
      break;
    }
    for (std::uint32_t update = 0U; update < direction_turn_updates; ++update) {
      if (const auto stepped =
              capture.step(sf::game::GameplayInput{.turn = 1.0})) {
        return "direction scan: " + *stepped;
      }
      ++trace.metrics.active_updates;
    }
  }
  if (!moved) {
    return "retail player could not move 128 units in any sampled direction";
  }

  constexpr std::uint32_t soak_updates = 48U;
  for (std::uint32_t update = 0U; update < soak_updates; ++update) {
    const auto phase = update % 16U;
    const auto locking = phase < 8U;
    const sf::game::GameplayInput input{
        .move = phase < 10U ? 1.0 : 0.0,
        .turn = phase >= 10U ? 0.65 : 0.0,
        .run = phase < 10U,
        .interact = phase == 9U,
        .target_lock = phase == 0U,
        .target_lock_held = locking,
        .target_lock_released = phase == 8U,
    };
    if (const auto stepped = capture.step(input)) {
      return "active soak: " + *stepped;
    }
    ++trace.metrics.active_updates;
  }
  if (const auto dedicated = validateDedicatedMissionWeapon(
          *gameplay, package.definition().index)) {
    return *dedicated;
  }
  return std::nullopt;
}

std::optional<std::string>
replayActiveTrace(const sf::game::MissionPackage &package,
                  const ActiveTrace &trace) {
  auto gameplay = std::make_unique<sf::game::GameplaySession>(package);
  gameplay->clearPcm();
  const auto initial = gameplay->legacyPresentationFrame();
  if (!initial) {
    return "replay published no initial frame";
  }
  if (initial->sequence == 0U) {
    return "replay published sequence zero";
  }
  if (const auto validation =
          validateSession(*gameplay, package.layout(),
                          initial->sequence - 1U)) {
    return "replay initial frame: " + *validation;
  }
  if (initial->sequence != trace.initial_sequence ||
      presentationDigest(*initial) != trace.initial_frame_digest) {
    return "replay initial immutable frame diverged";
  }

  auto previous_sequence = initial->sequence;
  for (std::size_t index = 0U; index < trace.samples.size(); ++index) {
    gameplay->update(trace.samples[index].input);
    if (!gameplay->advanceAudioFrameClock()) {
      return "replay audio/hardware clock stopped at update=" +
             std::to_string(index);
    }
    if (const auto validation = validateSession(
            *gameplay, package.layout(), previous_sequence)) {
      return "replay update=" + std::to_string(index) + " " + *validation;
    }
    const auto frame = gameplay->legacyPresentationFrame();
    if (!frame ||
        presentationDigest(*frame) != trace.samples[index].frame_digest) {
      return "replay immutable frame diverged at update=" +
             std::to_string(index);
    }
    const auto pcm = takePcmDigest(*gameplay);
    if (pcm != trace.samples[index].pcm) {
      return "replay PCM diverged at update=" + std::to_string(index) +
             " frames=" + std::to_string(pcm.frames) + '/' +
             std::to_string(trace.samples[index].pcm.frames);
    }
    previous_sequence = frame->sequence;
  }
  if (const auto dedicated = validateDedicatedMissionWeapon(
          *gameplay, package.definition().index)) {
    return *dedicated;
  }
  return std::nullopt;
}

int runProbe(const std::filesystem::path &cue_path,
             std::optional<std::uint32_t> only_mission) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{
        sf::core::ErrorCode::unsupported,
        "G2 active gameplay probe requires Syphon Filter USA v1.1"};
  }

  const auto missions = sf::game::missionCatalog();
  constexpr auto retail_mission_count = std::size_t{20U};
  if (missions.size() != retail_mission_count) {
    std::cerr << "G2 active gate requires all 20 missions: catalog="
              << missions.size() << '\n';
    return 2;
  }

  auto checked = std::size_t{};
  auto ready = std::size_t{};
  auto total_updates = std::uint64_t{};
  auto total_room_edges = std::size_t{};
  for (const auto &mission : missions) {
    if (only_mission && mission.index != *only_mission) {
      continue;
    }
    ++checked;
    try {
      const auto package = sf::game::MissionPackage::load(disc, mission.index);
      ActiveTrace trace;
      if (const auto failure = captureActiveTrace(package, trace)) {
        std::cout << "mission=" << mission.index
                  << " resource=" << mission.resource_name
                  << " result=failed phase=active detail="
                  << std::quoted(*failure) << '\n';
        continue;
      }
      if (const auto failure = replayActiveTrace(package, trace)) {
        std::cout << "mission=" << mission.index
                  << " resource=" << mission.resource_name
                  << " result=failed phase=replay detail="
                  << std::quoted(*failure) << '\n';
        continue;
      }
      ++ready;
      total_updates += trace.samples.size();
      total_room_edges += trace.metrics.room_edges;
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=ready scope=active-retail"
                << " wait=" << trace.metrics.control_wait_updates
                << " updates=" << trace.samples.size()
                << " active-updates=" << trace.metrics.active_updates
                << " pose-materializations="
                << trace.metrics.retail_pose_materializations
                << " post-pose="
                << trace.metrics.post_materialization_updates
                << " turn=" << trace.metrics.positive_turn << '/'
                << trace.metrics.negative_turn << " displacement=" << std::fixed
                << std::setprecision(1) << trace.metrics.maximum_displacement
                << std::defaultfloat
                << " room-edges=" << trace.metrics.room_edges
                << " resident=" << trace.metrics.maximum_resident
                << " active=" << trace.metrics.maximum_active
                << " replay=exact\n";
    } catch (const std::exception &error) {
      std::cout << "mission=" << mission.index
                << " resource=" << mission.resource_name
                << " result=failed phase=exception detail="
                << std::quoted(error.what()) << '\n';
    }
  }

  const auto expected = only_mission ? std::size_t{1U} : retail_mission_count;
  if (checked != expected || ready != checked) {
    std::cerr << "G2 active gameplay gate failed: ready=" << ready << '/'
              << checked << " expected=" << expected << '\n';
    return 2;
  }
  std::cout << "G2 active gameplay gate passed: scope=active-retail ready="
            << ready << '/' << checked << " updates=" << total_updates
            << " room-edges=" << total_room_edges << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: sf_g2_active_probe <game.cue> [mission-index]\n";
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
    std::cerr << "G2 active gameplay gate failed: " << error.what() << '\n';
    return 10;
  }
}
