#pragma once

#include "sf/game/legacy_bridge_types.hpp"
#include "sf/game/legacy_virtual_cd.hpp"
#include "sf/psx/executable.hpp"
#include "sf/psx/machine.hpp"
#include "sf/psx/r3000_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sf::game {

enum class LegacyGameplayBridgeReadFault {
  none,
  invalid_snapshot,
  effect_controller_mode,
  effect_attached_slot,
  effect_source_slot,
  effect_packet_opcode,
  effect_position_overflow,
  park2_flame_profile,
  park2_flame_overlay,
  park2_flame_link,
  park2_flame_material,
  park2_flame_opcode,
  park2_flame_projection,
  park2_flame_world,
};

enum class LegacyGameplayBridgeReadStage {
  none,
  globals,
  dynamic_lights,
  world,
  dropped_items,
  camera_packets,
  objects,
  attached_text,
  effects,
  park2_flame,
};

[[nodiscard]] constexpr std::string_view legacyGameplayBridgeReadStageName(
    LegacyGameplayBridgeReadStage stage) noexcept {
  switch (stage) {
  case LegacyGameplayBridgeReadStage::none:
    return "none";
  case LegacyGameplayBridgeReadStage::globals:
    return "globals";
  case LegacyGameplayBridgeReadStage::dynamic_lights:
    return "dynamic-lights";
  case LegacyGameplayBridgeReadStage::world:
    return "world";
  case LegacyGameplayBridgeReadStage::dropped_items:
    return "dropped-items";
  case LegacyGameplayBridgeReadStage::camera_packets:
    return "camera-packets";
  case LegacyGameplayBridgeReadStage::objects:
    return "objects";
  case LegacyGameplayBridgeReadStage::attached_text:
    return "attached-text";
  case LegacyGameplayBridgeReadStage::effects:
    return "effects";
  case LegacyGameplayBridgeReadStage::park2_flame:
    return "park2-flame";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view legacyGameplayBridgeReadFaultName(
    LegacyGameplayBridgeReadFault fault) noexcept {
  switch (fault) {
  case LegacyGameplayBridgeReadFault::none:
    return "none";
  case LegacyGameplayBridgeReadFault::invalid_snapshot:
    return "invalid-renderer-snapshot";
  case LegacyGameplayBridgeReadFault::effect_controller_mode:
    return "invalid-effect-controller-mode";
  case LegacyGameplayBridgeReadFault::effect_attached_slot:
    return "invalid-effect-attached-slot";
  case LegacyGameplayBridgeReadFault::effect_source_slot:
    return "invalid-effect-source-slot";
  case LegacyGameplayBridgeReadFault::effect_packet_opcode:
    return "invalid-effect-packet-opcode";
  case LegacyGameplayBridgeReadFault::effect_position_overflow:
    return "invalid-effect-position";
  case LegacyGameplayBridgeReadFault::park2_flame_profile:
    return "invalid-park2-flame-profile";
  case LegacyGameplayBridgeReadFault::park2_flame_overlay:
    return "invalid-park2-flame-overlay";
  case LegacyGameplayBridgeReadFault::park2_flame_link:
    return "invalid-park2-flame-link";
  case LegacyGameplayBridgeReadFault::park2_flame_material:
    return "invalid-park2-flame-material";
  case LegacyGameplayBridgeReadFault::park2_flame_opcode:
    return "invalid-park2-flame-opcode";
  case LegacyGameplayBridgeReadFault::park2_flame_projection:
    return "invalid-park2-flame-projection";
  case LegacyGameplayBridgeReadFault::park2_flame_world:
    return "invalid-park2-flame-world";
  }
  return "unknown-renderer-bridge-fault";
}

class LegacyGameplayVm;

class LegacyHostCallContext final {
public:
  [[nodiscard]] std::uint32_t pc() const noexcept;
  [[nodiscard]] std::uint32_t returnAddress() const noexcept;
  [[nodiscard]] std::uint32_t registerValue(std::size_t index) const noexcept;
  [[nodiscard]] std::uint32_t argument(std::size_t index) const noexcept;
  void setRegister(std::size_t index, std::uint32_t value) noexcept;
  void setReturnValue(std::uint32_t value) noexcept;
  void rejectHostCall() noexcept { accepted_ = false; }
  // Observe/patch the boundary, then retire the original guest instruction at
  // this PC instead of replacing the whole call with an HLE return.
  void continueGuestInstruction() noexcept {
    continue_guest_instruction_ = true;
  }

  [[nodiscard]] bool read8(std::uint32_t address,
                           std::uint8_t &value) const noexcept;
  [[nodiscard]] bool read16(std::uint32_t address,
                            std::uint16_t &value) const noexcept;
  [[nodiscard]] bool read32(std::uint32_t address,
                            std::uint32_t &value) const noexcept;
  [[nodiscard]] bool readBytes(std::uint32_t address,
                               std::span<std::byte> destination) const noexcept;
  [[nodiscard]] bool readCString(std::uint32_t address, std::string &value,
                                 std::size_t maximum_size = 256U) const;
  [[nodiscard]] bool write8(std::uint32_t address, std::uint8_t value) noexcept;
  [[nodiscard]] bool write16(std::uint32_t address,
                             std::uint16_t value) noexcept;
  [[nodiscard]] bool write32(std::uint32_t address,
                             std::uint32_t value) noexcept;
  [[nodiscard]] bool writeBytes(std::uint32_t address,
                                std::span<const std::byte> bytes) noexcept;

private:
  friend class LegacyGameplayVm;
  explicit LegacyHostCallContext(psx::R3000Runtime &runtime) noexcept
      : runtime_(runtime) {}

  psx::R3000Runtime &runtime_;
  bool accepted_{true};
  bool continue_guest_instruction_{};
};

using LegacyHostCall = std::function<void(LegacyHostCallContext &)>;

struct LegacyGameplayVmResult {
  psx::R3000RunResult execution;
  std::uint32_t return_value{};
  std::uint64_t host_calls{};
  std::optional<std::uint32_t> host_boundary;

  [[nodiscard]] bool completed() const noexcept {
    return execution.reason == psx::R3000StopReason::returned;
  }
  [[nodiscard]] bool stoppedAtHostBoundary() const noexcept {
    return host_boundary.has_value();
  }
};

struct LegacyRetailFrameProfile {
  std::uint32_t frame_entry{0x80014ff8U};
};

[[nodiscard]] constexpr LegacyRetailFrameProfile
syphonFilterUsaV11RetailFrameProfile() noexcept {
  return {};
}

// Headless slice of the retail platform tail. The original function also
// builds and submits GPU primitives; the native renderer owns that work.
struct LegacyRetailPlatformTailProfile {
  std::uint32_t delayed_callbacks_entry{0x800c8bb0U};
  std::uint32_t fade_step{0x801164d8U};
  std::uint32_t fade_current{0x801164daU};
  std::uint32_t fade_callback{0x801164e0U};
  std::uint32_t fade_initialized{0x801164edU};
  std::uint32_t fade_floor_rgb{0x800d37f4U};
  std::uint32_t fade_callback_dispatch_entry{0x800ca6ecU};
};

[[nodiscard]] constexpr LegacyRetailPlatformTailProfile
syphonFilterUsaV11RetailPlatformTailProfile() noexcept {
  return {};
}

struct LegacyRetailPlatformTailResult {
  LegacyGameplayVmResult delayed_callbacks;
  std::optional<LegacyGameplayVmResult> fade_callback;
  bool bridge_fault{};

  [[nodiscard]] bool completed() const noexcept;
};

struct LegacyRetailOuterFrameProfile {
  std::uint32_t system_clock{0x801169a4U};
  std::uint32_t current_state{0x80115c78U};
  std::uint32_t presented_state{0x80115c6cU};
  std::uint32_t gameplay_clock{0x801163b4U};
  std::uint32_t gameplay_frame{0x80116a88U};
  std::uint32_t vblank_counter{0x8010f378U};
  std::uint32_t renderer_vblank_interval{0x80116484U};
  std::uint32_t player_pointer{0x80116b9cU};
  std::uint32_t display_flags{0x8012d97aU};
  std::uint32_t input_entry{0x800d837cU};
  std::uint32_t gameplay_entry{0x80014ff8U};
  std::uint32_t player_frame_entry{0x800489f8U};
  std::uint32_t loading_player_frame_entry{0x80048a70U};
  std::uint32_t loading_stream_frame_entry{0x8008294cU};
  std::uint32_t loading_overlay_frame_entry{0x80015dc8U};
  std::uint32_t state7_frame_entry{0x80145accU};
  std::uint32_t stream_resume_entry{0x80082724U};
  std::uint32_t renderer_frame_entry{0x800c973cU};
  std::uint32_t pop_state_entry{0x80016094U};
};

[[nodiscard]] constexpr LegacyRetailOuterFrameProfile
syphonFilterUsaV11RetailOuterFrameProfile() noexcept {
  return {};
}

// State 2 is a synchronous preamble in System_RunStateMachine, not a frame.
// Retail drains this stack transition before incrementing the system clock or
// polling PAD, then executes the resulting application state in the same loop.
struct LegacyRetailState2TransitionProfile {
  std::uint32_t current_state{0x80115c78U};
  std::uint32_t state_depth{0x80115c74U};
  std::uint32_t transition{0x80115c7cU};
  std::uint32_t pop_state_entry{0x80016094U};
  std::uint32_t push_state_entry{0x80016020U};
  std::uint32_t common_init_entry{0x80014d6cU};
  std::uint32_t maximum_dispatches{10U};
};

[[nodiscard]] constexpr LegacyRetailState2TransitionProfile
syphonFilterUsaV11RetailState2TransitionProfile() noexcept {
  return {};
}

struct LegacyRetailState2TransitionResult {
  std::vector<LegacyGameplayVmResult> guest_calls;
  std::uint32_t dispatches{};
  std::uint32_t final_state{};
  bool bridge_fault{};
  bool dispatch_limit_reached{};

  [[nodiscard]] bool completed() const noexcept {
    return !bridge_fault && !dispatch_limit_reached && final_state != 2U &&
           std::ranges::all_of(guest_calls, &LegacyGameplayVmResult::completed);
  }
};

struct LegacyRetailOuterFrameResult {
  std::vector<LegacyGameplayVmResult> guest_calls;
  LegacyRetailPlatformTailResult platform_tail;
  std::optional<LegacyGameplayVmResult> renderer_tail;
  std::uint32_t state_before{};
  std::uint32_t state_after{};
  bool bridge_fault{};
  bool unsupported_state{};
  bool tail_skipped{};
  std::string_view bridge_fault_stage{};

  [[nodiscard]] bool completed() const noexcept;
};

struct LegacyFirstMissionOpeningProfile {
  std::uint32_t delayed_callback_control_entry{0x800c8a9cU};
  std::uint32_t skipped_movie_callback{0x800173ecU};
  std::uint32_t fade_reset_entry{0x800ca718U};
  std::uint32_t fade_start_entry{0x800ca780U};
  std::uint32_t fade_completion_callback{0x8001625cU};
  std::uint32_t mission_event_entry{0x80015364U};
  std::uint32_t camera_event_id{0x12U};
  std::uint32_t camera_event_priority{5U};
  std::uint32_t camera_source{35U};
};

[[nodiscard]] constexpr LegacyFirstMissionOpeningProfile
syphonFilterUsaV11FirstMissionOpeningProfile() noexcept {
  return {};
}

struct LegacyFirstMissionOpeningResult {
  LegacyGameplayVmResult remove_movie_callback;
  LegacyGameplayVmResult fade_reset;
  LegacyGameplayVmResult fade_start;
  LegacyGameplayVmResult camera_event;

  [[nodiscard]] bool completed() const noexcept;
};

enum class LegacyFirstMissionBootstrapPhase : std::uint8_t {
  reset,
  common_init,
  pop_title,
  select_mission,
  pop_transition,
  mission_init,
  initialize_fade,
  release_loading_ui,
  release_loading_fade,
  reset_loading_ui,
  initialize_display,
  pop_loading,
  enable_gameplay_triggers,
  start_opening,
  ready,
};

struct LegacyFirstMissionBootstrapProfile {
  std::uint32_t global_pointer{0x80115c68U};
  std::uint32_t stack_pointer{0x801fff00U};
  std::uint32_t common_init_entry{0x80014d6cU};
  std::uint32_t pop_state_entry{0x80016094U};
  std::uint32_t select_mission_entry{0x80014d24U};
  std::uint32_t loading_ui_handle{0x8015469cU};
  std::uint32_t loading_fade_handle{0x80115f16U};
  std::uint32_t loading_ui_find_entry{0x80086ea0U};
  std::uint32_t loading_ui_release_entry{0x80086018U};
  std::uint32_t loading_fade_release_entry{0x80084c30U};
  std::uint32_t loading_ui_reset_entry{0x8003b030U};
  std::uint32_t display_memory_query_entry{0x800de3fcU};
  std::uint32_t display_init_entry{0x800cb000U};
  std::uint32_t display_owner{0x801169c4U};
  // FUN_8001629c is the retail graphics-reset boundary. Native presentation
  // replaces its GPU work, but gameplay still consumes the software latch it
  // raises before any terrain opcode-0x10 volume may dispatch event 0x12/13.
  std::uint32_t gameplay_trigger_enable{0x80116962U};
};

[[nodiscard]] constexpr LegacyFirstMissionBootstrapProfile
syphonFilterUsaV11FirstMissionBootstrapProfile() noexcept {
  return {};
}

struct LegacyFirstMissionBootstrapResult {
  LegacyFirstMissionBootstrapPhase phase{
      LegacyFirstMissionBootstrapPhase::reset};
  LegacyGameplayVmResult execution;
  bool bridge_fault{};

  [[nodiscard]] bool completed() const noexcept {
    return phase == LegacyFirstMissionBootstrapPhase::ready && !bridge_fault;
  }
};

struct LegacyOverlayInstructionWord {
  std::uint32_t address{};
  std::uint32_t expected{};
};

struct LegacyPark2FlamethrowerBridgeProfile {
  bool enabled{};
  std::uint32_t packet_pool{0x8014ab20U};
  std::uint32_t packet_stride{0x30U};
  std::uint32_t packet_count{72U};
  std::uint32_t state_pool{0x8014bd48U};
  std::uint32_t state_stride{0x0cU};
  std::uint32_t width_history_pool{0x8014b8c8U};
  std::uint32_t width_history_stride{0x10U};
  // These words bind the packet pool, stride, count, EXPL descriptor lookup,
  // projected FT4 opcode and ring wrap to the exact USA v1.1 PARK2 overlay.
  std::array<LegacyOverlayInstructionWord, 24U> validation_words{{
      {0x80147638U, 0x3c108015U}, {0x8014763cU, 0x2610ab20U},
      {0x8014767cU, 0x24420058U}, {0x80147688U, 0x8c63c260U},
      {0x801476a0U, 0x0c031f63U}, {0x801476acU, 0x2a220048U},
      {0x801476b4U, 0x26100030U}, {0x801482d0U, 0x3c032c00U},
      {0x801482e4U, 0x34420002U}, {0x801482f8U, 0x2a620048U},
      {0x80148474U, 0x3c038015U}, {0x80148478U, 0x8c63a3e8U},
      {0x80148480U, 0x2484bd48U}, {0x80148d14U, 0x28420048U},
      {0x80147b48U, 0x2442bd48U}, {0x80147b4cU, 0x02c28021U},
      {0x80147dfcU, 0x2442bd54U}, {0x80147e00U, 0x02c22021U},
      {0x8014804cU, 0x96020004U}, {0x80148064U, 0x96020006U},
      {0x8014807cU, 0x96020008U}, {0x80148130U, 0x8c22b8c8U},
      {0x80148148U, 0x8c22b8ccU}, {0x80148160U, 0x8c22b8d0U},
  }};
};

struct LegacyGameplayBridgeProfile {
  struct ScrimProfile {
    bool enabled{};
    std::uint32_t resource_state{0x80119234U};
    std::uint32_t copy_state{0x80119230U};
    std::uint32_t model_instance{0x8011922cU};
    std::uint32_t copy_nodes{0x80130d38U};
    std::uint32_t copy_node_stride{0x20U};
    std::uint32_t copy_node_count{13U};
    std::array<std::pair<std::uint32_t, std::uint32_t>, 3U> validation_words{{
        {0x80022024U, 0x27bdffe8U},
        {0x80022120U, 0x3c028012U},
        {0x8002234cU, 0x27bdffd8U},
    }};
  };

  std::uint32_t camera_controller_pointer{0x80115d84U};
  std::uint32_t camera_mode{0x801191ecU};
  std::uint32_t camera_lock{0x801169e0U};
  // Offsets within the camera render context reached through
  // camera_controller_pointer -> controller -> context.
  std::uint32_t renderer_flags_offset{0x06U};
  // FUN_800c84f4 reads this byte once for the complete sprite list: 1 uses
  // GsSortFastSprite, while 0 uses GsSortSprite with authored mapping,
  // rotation and scale.
  std::uint32_t renderer_sprite_fast_path_offset{0x09U};
  std::uint32_t renderer_clear_rgb_offset{0x10U};
  std::uint32_t renderer_back_rgb_offset{0xe0U};
  std::uint32_t renderer_fog_dqa_offset{0xe4U};
  std::uint32_t renderer_fog_dqb_offset{0xe8U};
  std::uint32_t renderer_fog_rgb_offset{0xecU};
  std::uint32_t renderer_sprite_list_offset{0x90U};
  std::uint32_t renderer_line_list_offset{0x94U};
  std::uint32_t renderer_raw_packet_list_offset{0x98U};
  // Retail HUD/optic geometry is owned by the independent 2D render context
  // created by FUN_8003a31c. Scope modes link their POLY_F4/LINE packets here,
  // not into the world camera reached through camera_controller_pointer.
  std::uint32_t interface_renderer_pointer{0x80116998U};
  // FUN_800c973c seeds the active value from terrain_depth_cue before each
  // camera and FUN_800d3100 can replace it while drawing one dark-frame
  // object. The active copy is diagnostic only: it is not camera atmosphere.
  std::uint32_t active_terrain_depth_cue{0x80116450U};
  std::uint32_t terrain_depth_cue{0x80116458U};
  std::uint32_t renderer_display_flags{0x8012d97aU};
  // FUN_800c9140's independent atmosphere/backdrop pass. The descriptor is
  // mission data; +0x14 selects its material and +0x18..+0x1a its TILE RGB.
  std::uint32_t screen_filter_enabled{0x8011646eU};
  std::uint32_t screen_filter_descriptor{0x80116a78U};
  // FUN_800cfc9c selects this clear colour while camera flag 0x10 owns the
  // night-vision multi-pass presentation.
  std::uint32_t nightvision_clear_color{0x80116b28U};
  std::uint32_t processed_pad0{0x80122478U};
  std::uint32_t player_pointer{0x80116b9cU};
  std::uint32_t current_weapon{0x80115fb8U};
  std::uint32_t aim_mode{0x80115e80U};
  std::uint32_t gameplay_frame{0x80116a88U};
  std::uint32_t grenade_charge_frame{0x80127da0U};
  std::uint32_t grenade_input_pending{0x80127d98U};
  std::uint32_t aim_target{0x80119550U};
  std::uint32_t player_control_lock{0x80115e28U};
  std::uint32_t current_room{0x80116946U};
  std::uint32_t world_layout_pointer{0x80116a60U};
  std::uint32_t world_model_descriptors{0x80116994U};
  std::uint32_t world_model_descriptor_stride{0x3cU};
  std::uint32_t world_model_count{0x801169b0U};
  std::uint32_t object_activation_distance{0x80116b54U};
  std::uint32_t gameplay_trigger_enable{0x80116962U};
  std::uint32_t world_visibility_bytes{0x8012c7d8U};
  std::uint32_t fade_step{0x801164d8U};
  std::uint32_t fade_current{0x801164daU};
  std::uint32_t fade_callback{0x801164e0U};
  std::uint32_t fade_initialized{0x801164edU};
  std::uint32_t fade_floor_rgb{0x800d37f4U};
  std::uint32_t object_records_pointer{0x80115cccU};
  std::uint32_t object_count{0x80116a5cU};
  std::uint32_t object_definition_count{0x80116b14U};
  std::uint32_t object_definitions_pointer{0x80116b98U};
  std::uint32_t object_handler_table{0x801028a4U};
  std::uint32_t dynamic_first_slot{0x801168c4U};
  std::uint32_t target_lock_active{0x80116b7cU};
  // Base of the retail flashlight's 0x40-byte vertex-light source. +0 is its
  // active list-node handle; never sample this address as a boolean byte.
  std::uint32_t flashlight_enabled{0x8012f9b8U};
  std::uint32_t dynamic_light_list{0x80116464U};
  std::uint32_t maximum_vertex_lights{legacy_vertex_light_capacity};
  std::uint32_t taser_conductor_phase{0x80116ae6U};
  std::uint32_t taser_target_slot{0x801169a0U};
  std::uint32_t target_hit_result{0x80115e90U};
  std::uint32_t aimed_target_slot{0x80115e94U};
  std::uint32_t proximity_target_slot{0x80115e96U};
  std::uint32_t headshot_text_handle{0x80115e98U};
  std::uint32_t active_text_list{0x801160d8U};
  std::uint32_t text_object_pool{0x80120a98U};
  std::uint32_t text_object_stride{0x1cU};
  std::uint32_t text_object_capacity{40U};
  std::uint32_t headshot_text_pointer{0x8011628cU};
  std::uint32_t primary_story_text_pointer{0x80116934U};
  std::uint32_t primary_story_target_slot{0x80116aaeU};
  std::uint32_t secondary_story_text_pointer{0x80116970U};
  std::uint32_t secondary_story_target_slot{0x80116b00U};
  std::uint32_t tracked_slots{0x8011ba00U};
  // FUN_80045b10/FUN_80045f84 own thirty detachable display slots. Once an
  // owner word is a non-negative room index, descriptor+0x0c points at that
  // slot's 0x24-byte MATRIX and +0x16 is its inventory selector.
  std::uint32_t dropped_item_owners{0x8012b828U};
  std::uint32_t dropped_item_descriptors{0x80127ce8U};
  std::uint32_t dropped_item_matrices{0x8011c97cU};
  std::uint32_t dropped_item_matrix_stride{0x24U};
  std::uint32_t dropped_item_capacity{30U};
  // FUN_80027584 selects one of two fixed descriptors. The first belongs to
  // Gabe, the second to every non-player thrower. +8 owns the transient
  // display object whose +0 link is non-zero only while in flight.
  std::uint32_t player_thrown_projectile_pointer{0x801169d8U};
  std::uint32_t enemy_thrown_projectile_pointer{0x801169dcU};
  // Some overlay death paths retire the record before the common drop pass.
  // Run these retail helpers only at the pre-render boundary, while their
  // actor/display ownership is still coherent.
  std::uint32_t dropped_item_attach_entry{0x80045c04U};
  std::uint32_t dropped_item_detach_entry{0x80045f84U};
  std::uint32_t effect_particle_pool{0x80137740U};
  std::uint32_t effect_controller_pool_pointer{0x80116920U};
  std::uint32_t effect_controller_count{0x801166dcU};
  // Non-zero when the supported executable provides FUN_800db9c0(MATRIX*).
  // The bridge mirrors that coordinate composition natively and read-only;
  // it must never execute guest code while publishing an immutable frame.
  std::uint32_t bone_matrix_resolver_entry{0x800db9c0U};
  std::uint32_t effect_particle_capacity{160U};
  // FUN_8004bbb0 allocates at most 0x58 controllers (Georgia Street's
  // branch); lower-memory levels select 0x28 or 0x50.
  std::uint32_t maximum_effect_controllers{0x58U};
  // FUN_80054fbc adds these wind/current terms to moving LINE_G2 particles
  // when controller flag 0x4000 is active.
  std::uint32_t effect_world_motion_x{0x8012fa20U};
  std::uint32_t effect_world_motion_z{0x8012fa28U};
  // FUN_800511a0 selects the authored horizontal impact plane used by
  // PARK.OVL's update1/render5 rain controller.
  std::uint32_t effect_mission_index{0x80130c88U};
  std::uint32_t effect_floor_probe_y{0x80116b58U};
  std::uint32_t effect_floor_counts{0x8010c6fcU};
  std::uint32_t effect_floor_thresholds{0x8010c714U};
  std::uint32_t maximum_objects{2048U};
  std::uint32_t maximum_definitions{1024U};
  std::uint32_t maximum_object_class{0x7fU};
  std::uint32_t maximum_text_nodes{40U};
  std::uint32_t maximum_world_callouts{5U};
  std::uint32_t maximum_world_models{0xfeU};
  std::uint32_t maximum_world_sections{31U};
  std::uint32_t maximum_world_section_vertices{1024U};
  std::uint32_t maximum_world_vertex_colors{131072U};
  std::uint32_t maximum_resident_world_models{16U};
  std::uint32_t maximum_guest_sprites{512U};
  std::uint32_t maximum_guest_lines{512U};
  std::uint32_t maximum_guest_raw_packets{1024U};
  ScrimProfile scrim;
  LegacyPark2FlamethrowerBridgeProfile park2_flamethrower;
};

[[nodiscard]] constexpr LegacyGameplayBridgeProfile
syphonFilterUsaV11GameplayBridgeProfile() noexcept {
  return {};
}

// Host player state retained for isolated diagnostics. Production manual aim
// uses the narrower locomotion bridge below so it cannot overwrite animated
// pose height, rotation, or vitals. Coordinates use the renderer convention
// (positive Y up).
struct LegacyHostPlayerState {
  LegacyNativePoint position;
  std::int32_t yaw{};
  std::int16_t health{150};
  std::int16_t armor{600};
  LegacyNativePoint previous_position;
  bool has_previous_position{};
};

// Collision-resolved root written back before the next retail tick. The guest
// motion controller owns world height; the animated MATRIX keeps its pose Y.
struct LegacyHostPlayerLocomotion {
  LegacyNativePoint position;
  LegacyNativePoint previous_position;
  bool has_previous_position{};
};

struct LegacyHostPadState {
  // Active-high standard PlayStation button bits.
  std::uint16_t buttons{};
  std::uint8_t left_x{0x80U};
  std::uint8_t left_y{0x80U};
  std::uint8_t right_x{0x80U};
  std::uint8_t right_y{0x80U};
};

// Native sight ray in renderer coordinates (positive Y down). The USA v1.1
// boundary converts it to the guest's ray descriptor immediately before the
// original collision/headshot scan, so damage and scripts stay retail-owned.
struct LegacyHostAimRay {
  double origin_x{};
  double origin_y{};
  double origin_z{};
  double direction_x{};
  double direction_y{};
  double direction_z{};
};

struct LegacyHostAimRayProfile {
  // Enter FUN_8003a3c8 after the caller's JAL delay slot has completed. The
  // second manual-shot caller only writes descriptor+4 in that delay slot, so
  // intercepting the JAL itself observes a stale endpoint pointer. Restrict
  // the callee hook to the exact scoped and unscoped retail first-person
  // callers; unrelated AI/world rays retain their guest-owned direction.
  std::uint32_t collision_scan_entry{0x8003a3c8U};
  std::array<std::uint32_t, 3U> accepted_return_addresses{
      0x8003a8a4U,
      0x8003a99cU,
      0x8003aa34U,
  };
  std::uint32_t origin_pointer_offset{};
  std::uint32_t endpoint_pointer_offset{4U};
  std::int32_t ray_length{0x1900};
};

[[nodiscard]] constexpr LegacyHostAimRayProfile
syphonFilterUsaV11HostAimRayProfile() noexcept {
  return {};
}

struct LegacyEnemyCloseAimProfile {
  // FUN_80062220 normally feeds Gabe's lagged tracking point to the generic
  // weapon controller. At point-blank range that PS1-era lead can put an
  // entire burst behind him. The hook substitutes the target point already
  // built by retail at sp+0x10, while the original JAL/delay slot and damage
  // pipeline continue unchanged.
  std::uint32_t boundary{0x80062858U};
  std::uint32_t instruction{0x0c01fa12U};
  std::uint32_t delay_instruction{0x24a50010U};
  std::int32_t close_distance{0x333};
};

[[nodiscard]] constexpr LegacyEnemyCloseAimProfile
syphonFilterUsaV11EnemyCloseAimProfile() noexcept {
  return {};
}

struct LegacyWeaponEventHookBoundary {
  std::uint32_t address{};
  std::uint32_t instruction{};
  std::uint32_t delay_instruction{};
  LegacyWeaponEventType type{};
};

struct LegacyWeaponEventHookProfile {
  // Accepted player-state boundaries in SCUS_942.40. Hooks pass the original
  // JAL and delay slot through unchanged; both words are validated first.
  std::array<LegacyWeaponEventHookBoundary, 7U> boundaries{{
      {0x800261d8U, 0x0c011961U, 0x02402021U, LegacyWeaponEventType::shot},
      {0x80026554U, 0x0c011961U, 0x02402021U, LegacyWeaponEventType::thrown},
      {0x80025fc4U, 0x0c00a3cfU, 0x2405003dU,
       LegacyWeaponEventType::scanner_begin},
      {0x800264dcU, 0x0c00a3cfU, 0x2405000dU,
       LegacyWeaponEventType::scanner_end},
      {0x800264a8U, 0x0c009064U, 0x00000000U,
       LegacyWeaponEventType::flashlight_toggle},
      {0x8008d6a0U, 0x0c009481U, 0x24050022U,
       LegacyWeaponEventType::key_card_use},
      {0x800905dcU, 0x0c009481U, 0x24050022U, LegacyWeaponEventType::c4_use},
  }};
  std::uint32_t player_pointer{0x80116b9cU};
  std::uint32_t current_weapon{0x80115fb8U};
  std::uint32_t aim_mode{0x80115e80U};
  std::uint32_t hit_result{0x80115e90U};
  std::uint32_t aimed_target_slot{0x80115e94U};
  std::uint32_t ray_origin{0x80119550U};
  std::uint32_t ray_endpoint{0x80119560U};
  std::uint32_t maximum_events{legacy_weapon_events_per_frame};
};

[[nodiscard]] constexpr LegacyWeaponEventHookProfile
syphonFilterUsaV11WeaponEventHookProfile() noexcept {
  return {};
}

struct LegacyUiMessageHookBoundary {
  std::uint32_t address{};
  std::array<std::uint32_t, 4U> instructions{};
  LegacyUiMessageChannel channel{LegacyUiMessageChannel::status};
};

// Read-only hooks at the retail text builders. The original instructions and
// glyph allocator still execute in guest code; the host only preserves source
// strings which retail immediately compiles into transient glyph packets.
struct LegacyGameplayTextHookProfile {
  std::array<LegacyUiMessageHookBoundary, 2U> message_boundaries{{
      {0x80017530U,
       {0x27bdffe0U, 0xafb10014U, 0x00808821U, 0xafb20018U},
       LegacyUiMessageChannel::centered},
      {0x80085d04U,
       {0x27bdffd8U, 0x00801021U, 0xafb00018U, 0x00a08021U},
       LegacyUiMessageChannel::status},
  }};
  std::uint32_t attached_text_entry{0x80085eb0U};
  std::array<std::uint32_t, 4U> attached_text_instructions{
      0x27bdffc8U,
      0xafb10024U,
      0x00a08821U,
      0xafb20028U,
  };
  std::uint32_t active_text_list{0x801160d8U};
  std::uint32_t text_pool_cursor{0x801160d4U};
  std::uint32_t text_object_pool{0x80120a98U};
  std::uint32_t text_object_stride{0x1cU};
  std::uint32_t text_object_capacity{40U};
  std::uint32_t maximum_messages_per_frame{64U};
  std::uint32_t maximum_text_size{256U};
};

[[nodiscard]] constexpr LegacyGameplayTextHookProfile
syphonFilterUsaV11GameplayTextHookProfile() noexcept {
  return {};
}

struct LegacyHostDamageEvent {
  std::int16_t attacker_slot{-1};
  std::int16_t owner_slot{-1};
  std::int16_t target_slot{-1};
  std::int16_t damage{};
  std::int16_t damage_type{};
};

struct LegacyNativeMissionBridgeProfile {
  std::uint32_t player_pointer{0x80116b9cU};
  std::uint32_t object_records_pointer{0x80115cccU};
  std::uint32_t object_count{0x80116a5cU};
  std::uint32_t processed_pad0{0x80122478U};
  std::uint32_t raw_pad0{0x80122658U};
  std::uint32_t raw_pad1{0x8012267aU};
  std::uint32_t current_room{0x80116946U};
  // FUN_80082ec0 releases the asynchronous stream queue after the renderer
  // has paused it. Portal-driven retail gameplay reaches the same boundary
  // before FUN_800820d4 performs its blocking room drain.
  std::uint32_t stream_unlock_entry{0x80082ec0U};
  std::uint32_t room_change_entry{0x800820d4U};
  std::uint32_t damage_entry{0x80069cb0U};
  std::uint32_t event_entry{0x80015364U};
  // FUN_800405f4 is the retail weapon-tape state machine. Native mouse
  // wheel/middle-button input enters here so selection, animation, sounds and
  // inventory ownership remain guest-authored without pretending to be an
  // unrelated physical controller button.
  std::uint32_t weapon_menu_input_entry{0x800405f4U};
  // FUN_8002fa48 is the retail first-person entry/exit wrapper. It selects
  // the weapon-specific optic (normal sniper, SVD nightvision or detector)
  // and owns the matching camera/UI transition state.
  std::uint32_t first_person_aim_input_entry{0x8002fa48U};
  std::uint32_t mission_progress_pointer{0x8011699cU};
  // cc8 is the terminal latch. cc9 is also raised by FUN_80017890 while a
  // failed mission enters its transition, so the persistent b24/b25 outcome
  // bytes are the authoritative failure/success discriminator.
  std::uint32_t mission_terminal_latch{0x80115cc8U};
  std::uint32_t mission_success_latch{0x80115cc9U};
  std::uint32_t mission_transition_latch{0x80115ccaU};
  std::uint32_t mission_failure_flag{0x80116b24U};
  std::uint32_t mission_completed_flag{0x80116b25U};
  std::uint32_t inventory_current_weapon{0x80115fb8U};
  std::uint32_t weapon_menu_state{0x80115f50U};
  std::uint32_t weapon_menu_dirty{0x80115f74U};
  std::uint32_t weapon_menu_controller_ready{0x80115f3cU};
  // Exact FUN_8002f5d8 presentation mode: 0 chase, 2 normal sniper,
  // 3 nightvision rifle, 4 viral detector.
  std::uint32_t first_person_aim_mode{0x80115e80U};
  // FUN_8002fb1c constructs the normal sniper's independent camera at the
  // fixed DAT_8013c730 object. SVD instead animates the primary camera reached
  // through PTR_DAT_80115d84. Both store the live projection at +0xc0c.
  std::uint32_t scope_camera_controller_pointer{0x80115d84U};
  std::uint32_t sniper_scope_camera_controller{0x8013c730U};
  std::uint32_t scope_zoom_offset{0xc0cU};
  std::uint32_t weapon_menu_input_ready{0x80116984U};
  std::uint32_t inventory_owned_weapons{0x80115fbcU};
  std::uint32_t inventory_ammo_table{0x8012f0b0U};
  // Retail FONT/TEXT state. Slot 6 is the status stack; slots 0..5 contain
  // independent centered text. The status backdrop is the exact POLY_F4
  // packet built by FUN_800865ec.
  std::uint32_t text_slot_table{0x80120ef8U};
  std::uint32_t text_slot_stride{0x14U};
  std::uint32_t text_slot_count{7U};
  std::uint32_t text_object_pool{0x80120a98U};
  std::uint32_t text_object_stride{0x1cU};
  std::uint32_t text_object_capacity{40U};
  std::uint32_t message_glyph_pool{0x8011f5f8U};
  std::uint32_t timer_glyph_pool{0x8011bfd8U};
  std::uint32_t glyph_stride{0x2cU};
  std::uint32_t message_glyph_capacity{120U};
  std::uint32_t timer_glyph_capacity{8U};
  std::uint32_t status_backdrop_tag{0x80121074U};
  std::uint32_t status_backdrop_color_code{0x80121080U};
  std::uint32_t status_backdrop_vertices{0x80121084U};
  std::uint32_t mission_timer_remaining{0x80116690U};
  std::uint32_t mission_timer_handle{0x80115f22U};
  std::uint32_t maximum_objects{2048U};
};

[[nodiscard]] constexpr LegacyNativeMissionBridgeProfile
syphonFilterUsaV11NativeMissionBridgeProfile() noexcept {
  return {};
}

struct LegacyRetailAudioProfile {
  std::uint32_t reset_callback_entry{0x800e4184U};
  std::uint32_t interrupt_callback_entry{0x800e41b4U};
  std::uint32_t expected_tick_callback{0x800f6574U};
  std::uint32_t set_group_volume_entry{0x8006b824U};
  std::uint32_t group_volume_address{0x80116020U};
  std::uint32_t callback_hz{120U};
  std::uint8_t timer_irq{6U};
};

[[nodiscard]] constexpr LegacyRetailAudioProfile
syphonFilterUsaV11RetailAudioProfile() noexcept {
  return {};
}

// Retail MENU.OVL presents Sound FX, Music and Voice-over in this order and
// maps them to groups 0, 1 and 2. Group 2 reaches the SPU CD-input volume and
// therefore controls XA dialogue; groups 0 and 1 are applied to their active
// VAB voices.
struct LegacyRetailAudioVolumes {
  static constexpr std::uint8_t maximum = 0x7fU;
  static constexpr std::size_t group_count = 3U;

  std::uint8_t sound_effects{maximum};
  std::uint8_t music{maximum};
  std::uint8_t voice_over{maximum};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return sound_effects <= maximum && music <= maximum &&
           voice_over <= maximum;
  }

  [[nodiscard]] constexpr std::array<std::uint8_t, group_count>
  groups() const noexcept {
    return {sound_effects, music, voice_over};
  }

  [[nodiscard]] friend constexpr bool
  operator==(const LegacyRetailAudioVolumes &,
             const LegacyRetailAudioVolumes &) noexcept = default;
};

[[nodiscard]] constexpr std::uint8_t
legacyRetailAudioVolumeFromPercent(std::uint8_t percent) noexcept {
  const auto clamped = percent > 100U ? 100U : percent;
  return static_cast<std::uint8_t>(
      (static_cast<std::uint32_t>(clamped) * LegacyRetailAudioVolumes::maximum +
       50U) /
      100U);
}

[[nodiscard]] constexpr std::uint8_t
legacyRetailAudioVolumeToPercent(std::uint8_t volume) noexcept {
  const auto clamped = volume > LegacyRetailAudioVolumes::maximum
                           ? LegacyRetailAudioVolumes::maximum
                           : volume;
  return static_cast<std::uint8_t>((static_cast<std::uint32_t>(clamped) * 100U +
                                    LegacyRetailAudioVolumes::maximum / 2U) /
                                   LegacyRetailAudioVolumes::maximum);
}

// Frame-boundary state used by mission checkpoints and deterministic replay.
// Host bindings and the CD catalog remain configuration; mutable CD transport
// state is serialized whenever the virtual-CD binding is active.
struct LegacyGameplayVmSnapshot {
  static constexpr std::size_t interrupt_callback_count = 11U;

  psx::R3000State cpu;
  psx::PsxMachineState machine;
  std::vector<std::byte> ram;
  std::array<std::byte, psx::R3000Runtime::scratchpad_size> scratchpad{};
  std::array<std::byte, psx::R3000Runtime::mmio_size> mmio{};
  std::uint32_t video_timing_baseline{};
  std::uint64_t audio_frame_tick{};
  std::array<std::uint32_t, interrupt_callback_count> interrupt_callbacks{};
  struct AttachedTextSource {
    std::uint32_t text_object{};
    std::string text;
    // Retail stores the additive source-string checksum in TEXT+0x15 and
    // includes it in every handle.  Pool addresses are reused, so the object
    // pointer alone is not a stable identity.
    std::uint8_t text_checksum{};

    [[nodiscard]] friend bool operator==(const AttachedTextSource &,
                                         const AttachedTextSource &) = default;
  };
  // A common NPC can be retired by its mission overlay before the retail
  // renderer reaches FUN_80045c04. Preserve only the exact live guest
  // identity and authored drop bits needed to replay that missed retail edge
  // on the following renderer boundary.
  struct PendingActorDrop {
    std::uint32_t record_slot{};
    std::uint32_t instance{};
    std::uint16_t attributes{};

    [[nodiscard]] friend bool operator==(const PendingActorDrop &,
                                         const PendingActorDrop &) = default;
  };
  std::vector<AttachedTextSource> attached_text_sources;
  std::vector<LegacyUiMessageBridgeState> ui_messages;
  std::vector<PendingActorDrop> pending_actor_drops;
  bool video_timing_baseline_initialized{};
  bool audio_frame_tick_initialized{};
  std::optional<LegacyVirtualCd::Snapshot> virtual_cd;
};

// Addresses used by the retail mission scheduler. They are shared by every
// level in the supported executable; the loaded overlay supplies callbacks.
struct LegacyMissionRuntimeProfile {
  std::uint32_t frame_event_entry{0x80015364U};
  std::uint32_t delayed_callbacks_entry{0x80016994U};
  std::uint32_t queue_drain_entry{0x800156dcU};
  std::uint32_t pending_queue_count{0x80116c68U};
  std::uint32_t ready_queue_count{0x8011775cU};
  std::uint32_t ready_queue_entries{0x80117760U};
  std::uint32_t dynamic_event_table_pointer{0x80130c8cU};
  std::uint32_t static_event_table{0x80102ae0U};
  std::uint32_t special_object_handler_pointer{0x8010330cU};
  std::uint32_t object_records_pointer{0x80115cccU};
  std::uint32_t object_count{0x80116a5cU};
  std::uint32_t object_definitions_pointer{0x80116b98U};
  std::uint32_t object_definition_count{0x80116b14U};
  std::uint32_t object_handler_table{0x801028a4U};
  std::uint32_t maximum_ready_events{100U};
  std::uint32_t maximum_objects{2048U};
  std::uint32_t maximum_definitions{1024U};
  std::uint32_t maximum_object_class{0x7fU};
};

[[nodiscard]] constexpr LegacyMissionRuntimeProfile
syphonFilterUsaV11MissionProfile() noexcept {
  return {};
}

struct LegacyMissionTickResult {
  LegacyGameplayVmResult frame_event;
  LegacyGameplayVmResult delayed_callbacks;
  LegacyGameplayVmResult queue_drain;
  std::vector<LegacyGameplayVmResult> dispatched_events;
  std::uint32_t ready_events{};
  bool bridge_fault{};

  [[nodiscard]] bool completed() const noexcept;
  [[nodiscard]] std::uint64_t instructions() const noexcept;
  [[nodiscard]] std::uint64_t hostCalls() const noexcept;
};

// Boundary between original mission code and the native renderer/platform.
// The VM owns original RAM state; native systems exchange only explicit values.
class LegacyGameplayVm final {
public:
  static constexpr std::uint32_t updates_per_second = 20U;

  explicit LegacyGameplayVm(const psx::Executable &executable,
                            psx::CpuClockScale cpu_clock_scale = {});

  [[nodiscard]] bool loadOverlay(std::uint32_t address,
                                 std::span<const std::byte> bytes) noexcept;
  void bindHostCall(std::uint32_t address, LegacyHostCall call);
  void bindPsxBiosRandomCalls();
  void bindPsxLibcStringCalls();
  void bindPsxVideoTimingCall();
  void bindPsxCriticalSectionCalls();
  void bindPsxGpuSubmissionCall();
  void bindSyphonFilterUsaV11VirtualCdCalls(
      std::shared_ptr<LegacyVirtualCd> virtual_cd);
  void bindSyphonFilterUsaV11PlatformCalls();
  void bindSyphonFilterUsaV11BootstrapPlatformCalls();
  void bindSyphonFilterUsaV11HostAimRayHook(
      const LegacyHostAimRayProfile &profile =
          syphonFilterUsaV11HostAimRayProfile());
  void bindSyphonFilterUsaV11EnemyCloseAimHook(
      const LegacyEnemyCloseAimProfile &profile =
          syphonFilterUsaV11EnemyCloseAimProfile());
  void bindSyphonFilterUsaV11WeaponEventHooks(
      const LegacyWeaponEventHookProfile &profile =
          syphonFilterUsaV11WeaponEventHookProfile());
  void bindSyphonFilterUsaV11GameplayTextHooks(
      const LegacyGameplayTextHookProfile &profile =
          syphonFilterUsaV11GameplayTextHookProfile());
  void clearWeaponEvents() noexcept { weapon_events_.clear(); }
  void clearUiMessages() noexcept { ui_messages_.clear(); }
  [[nodiscard]] std::span<const LegacyWeaponEventBridgeState>
  weaponEvents() const noexcept {
    return weapon_events_;
  }
  [[nodiscard]] bool unbindHostCall(std::uint32_t address) noexcept;
  void clearHostCalls() noexcept;
  [[nodiscard]] LegacyGameplayVmSnapshot captureSnapshot() const;
  [[nodiscard]] bool
  restoreSnapshot(const LegacyGameplayVmSnapshot &snapshot) noexcept;
  // Complete a 20 Hz retail frame with an exact deterministic CPU/SPU time.
  // clearPcm() establishes the first frame boundary after bootstrap or a
  // native transition; snapshots retain this clock anchor.
  [[nodiscard]] bool
  advanceAudioFrameClock(const LegacyRetailAudioProfile &profile =
                             syphonFilterUsaV11RetailAudioProfile()) noexcept;
  // Advances one retail 120 Hz timer/SPU slice. The realtime host uses this
  // smaller boundary so PCM reaches the device without a full 50 ms frame of
  // latency; offline probes may continue to use advanceAudioFrameClock().
  [[nodiscard]] bool
  advanceAudioSliceClock(const LegacyRetailAudioProfile &profile =
                             syphonFilterUsaV11RetailAudioProfile()) noexcept;
  [[nodiscard]] bool
  setRetailAudioVolumes(const LegacyRetailAudioVolumes &volumes,
                        const LegacyRetailAudioProfile &profile =
                            syphonFilterUsaV11RetailAudioProfile(),
                        std::uint64_t execution_budget = 1'000'000U) noexcept;
  [[nodiscard]] std::optional<LegacyRetailAudioVolumes> readRetailAudioVolumes(
      const LegacyRetailAudioProfile &profile =
          syphonFilterUsaV11RetailAudioProfile()) const noexcept;
  [[nodiscard]] std::size_t
  takePcm(std::span<psx::SpuPcmFrame> destination) noexcept;
  void clearPcm() noexcept;
  [[nodiscard]] LegacyAudioDiagnostics audioDiagnostics() const noexcept;
  [[nodiscard]] std::optional<LegacyGameplayBridgeState>
  readBridgeState(const LegacyGameplayBridgeProfile &profile =
                      syphonFilterUsaV11GameplayBridgeProfile());
  [[nodiscard]] LegacyGameplayBridgeReadFault
  lastBridgeReadFault() const noexcept {
    return last_bridge_read_fault_;
  }
  [[nodiscard]] LegacyGameplayBridgeReadStage
  lastBridgeReadStage() const noexcept {
    return last_bridge_read_stage_;
  }
  // Development probes use the following writers to compare isolated retail
  // functions. They are deliberately absent from LegacyFirstMissionRuntime.
  [[nodiscard]] bool writeHostPlayerState(
      const LegacyHostPlayerState &state,
      const LegacyNativeMissionBridgeProfile &profile =
          syphonFilterUsaV11NativeMissionBridgeProfile()) noexcept;
  [[nodiscard]] bool writeHostPlayerLocomotion(
      const LegacyHostPlayerLocomotion &state,
      const LegacyNativeMissionBridgeProfile &profile =
          syphonFilterUsaV11NativeMissionBridgeProfile()) noexcept;
  [[nodiscard]] bool writeHostPlayerVitals(
      std::int16_t health, std::int16_t armor,
      const LegacyNativeMissionBridgeProfile &profile =
          syphonFilterUsaV11NativeMissionBridgeProfile()) noexcept;
  [[nodiscard]] bool writeHostPlayerHeading(
      std::int32_t yaw,
      const LegacyNativeMissionBridgeProfile &profile =
          syphonFilterUsaV11NativeMissionBridgeProfile()) noexcept;
  void setHostAimRay(std::optional<LegacyHostAimRay> ray) noexcept;
  [[nodiscard]] std::uint64_t hostAimRayPatchCount() const noexcept {
    return host_aim_ray_patch_count_;
  }
  [[nodiscard]] std::uint64_t enemyCloseAimPatchCount() const noexcept {
    return enemy_close_aim_patch_count_;
  }
  [[nodiscard]] bool writeHostPadState(
      const LegacyHostPadState &state,
      const LegacyNativeMissionBridgeProfile &profile =
          syphonFilterUsaV11NativeMissionBridgeProfile()) noexcept;
  [[nodiscard]] bool writeHostInventoryState(
      const LegacyInventoryBridgeState &state,
      const LegacyNativeMissionBridgeProfile &profile =
          syphonFilterUsaV11NativeMissionBridgeProfile()) noexcept;
  [[nodiscard]] bool setRetailHardMode(bool enabled) noexcept;
  [[nodiscard]] bool setRetailOneShotKills(bool enabled) noexcept;
  [[nodiscard]] bool
  weakenRetailEnemySlots(std::span<const std::uint32_t> slots,
                         const LegacyGameplayBridgeProfile &profile =
                             syphonFilterUsaV11GameplayBridgeProfile()) noexcept;
  [[nodiscard]] bool
  synchronizeHostRoom(std::int16_t room,
                      const LegacyNativeMissionBridgeProfile &profile =
                          syphonFilterUsaV11NativeMissionBridgeProfile(),
                      std::uint64_t execution_budget = 5'000'000U) noexcept;
  [[nodiscard]] LegacyGameplayVmResult
  queueHostDamage(const LegacyHostDamageEvent &event,
                  const LegacyNativeMissionBridgeProfile &profile =
                      syphonFilterUsaV11NativeMissionBridgeProfile(),
                  std::uint64_t execution_budget = 1'000'000U);
  [[nodiscard]] LegacyGameplayVmResult
  queueHostImpact(std::int16_t attacker_slot, std::int16_t target_slot,
                  const LegacyNativeMissionBridgeProfile &profile =
                      syphonFilterUsaV11NativeMissionBridgeProfile(),
                  std::uint64_t execution_budget = 1'000'000U);
  [[nodiscard]] LegacyGameplayVmResult
  queueHostInteraction(std::int16_t target_slot,
                       const LegacyNativeMissionBridgeProfile &profile =
                           syphonFilterUsaV11NativeMissionBridgeProfile(),
                       std::uint64_t execution_budget = 1'000'000U);
  [[nodiscard]] LegacyGameplayVmResult invokeRetailWeaponMenuInput(
      bool held, std::int32_t delta,
      const LegacyNativeMissionBridgeProfile &profile =
          syphonFilterUsaV11NativeMissionBridgeProfile(),
      std::uint64_t execution_budget = 1'000'000U);
  [[nodiscard]] LegacyGameplayVmResult
  invokeRetailFirstPersonAim(bool active,
                             const LegacyNativeMissionBridgeProfile &profile =
                                 syphonFilterUsaV11NativeMissionBridgeProfile(),
                             std::uint64_t execution_budget = 5'000'000U);
  [[nodiscard]] std::optional<LegacyMissionBridgeState> readMissionBridgeState(
      const LegacyNativeMissionBridgeProfile &profile =
          syphonFilterUsaV11NativeMissionBridgeProfile()) const noexcept;
  [[nodiscard]] LegacyGameplayVmResult
  invoke(std::uint32_t address, std::span<const std::uint32_t> arguments = {},
         std::uint64_t execution_budget = 1'000'000U);
  // Continue from the current CPU state without replacing PC, RA or the
  // active call frame. This is the execution path used by a continuous
  // guest loop after its initial entry has been established.
  [[nodiscard]] LegacyGameplayVmResult
  resumeCurrentPc(std::uint64_t execution_budget = 1'000'000U);
  // Stop before dispatching the host call at boundary_address. A later
  // resumeCurrentPc() observes the same PC and dispatches that call normally.
  [[nodiscard]] LegacyGameplayVmResult
  runCurrentPcUntilHostBoundary(std::uint32_t boundary_address,
                                std::uint64_t execution_budget = 1'000'000U);
  [[nodiscard]] LegacyGameplayVmResult
  tickRetailFrame(const LegacyRetailFrameProfile &profile =
                      syphonFilterUsaV11RetailFrameProfile(),
                  std::uint64_t execution_budget = 50'000'000U);
  [[nodiscard]] LegacyRetailPlatformTailResult
  tickRetailPlatformTail(bool advance_delayed_callbacks,
                         const LegacyRetailPlatformTailProfile &profile =
                             syphonFilterUsaV11RetailPlatformTailProfile(),
                         std::uint64_t execution_budget = 5'000'000U);
  [[nodiscard]] LegacyRetailState2TransitionResult
  dispatchRetailState2Transition(
      const LegacyRetailState2TransitionProfile &profile =
          syphonFilterUsaV11RetailState2TransitionProfile(),
      std::uint64_t execution_budget = 50'000'000U);
  [[nodiscard]] LegacyRetailOuterFrameResult
  tickRetailOuterFrame(const LegacyRetailOuterFrameProfile &profile =
                           syphonFilterUsaV11RetailOuterFrameProfile(),
                       const LegacyRetailPlatformTailProfile &tail_profile =
                           syphonFilterUsaV11RetailPlatformTailProfile(),
                       std::uint64_t execution_budget = 50'000'000U);
  // Offline diagnostic scheduler used by legacy probes. Shipping gameplay
  // always calls tickRetailOuterFrame().
  [[nodiscard]] LegacyRetailOuterFrameResult tickNativeDrivenGameplayFrame(
      const LegacyRetailOuterFrameProfile &profile =
          syphonFilterUsaV11RetailOuterFrameProfile(),
      const LegacyRetailPlatformTailProfile &tail_profile =
          syphonFilterUsaV11RetailPlatformTailProfile(),
      std::uint64_t execution_budget = 50'000'000U);
  [[nodiscard]] LegacyFirstMissionOpeningResult
  startFirstMissionOpeningWithoutMovie(
      const LegacyFirstMissionOpeningProfile &profile =
          syphonFilterUsaV11FirstMissionOpeningProfile(),
      std::uint64_t execution_budget = 50'000'000U);
  [[nodiscard]] LegacyFirstMissionBootstrapResult bootstrapFirstMission(
      const LegacyFirstMissionBootstrapProfile &profile =
          syphonFilterUsaV11FirstMissionBootstrapProfile(),
      const LegacyRetailPlatformTailProfile &tail_profile =
          syphonFilterUsaV11RetailPlatformTailProfile(),
      const LegacyFirstMissionOpeningProfile &opening_profile =
          syphonFilterUsaV11FirstMissionOpeningProfile(),
      std::uint64_t execution_budget = 50'000'000U);
  // Boots any retail campaign entry through its FOG/overlay load. Mission 0
  // may additionally request the Georgia-specific post-FMV opening setup.
  [[nodiscard]] LegacyFirstMissionBootstrapResult
  bootstrapMission(std::uint32_t mission_selection_index,
                   bool start_first_mission_opening,
                   const LegacyFirstMissionBootstrapProfile &profile =
                       syphonFilterUsaV11FirstMissionBootstrapProfile(),
                   const LegacyRetailPlatformTailProfile &tail_profile =
                       syphonFilterUsaV11RetailPlatformTailProfile(),
                   const LegacyFirstMissionOpeningProfile &opening_profile =
                       syphonFilterUsaV11FirstMissionOpeningProfile(),
                   std::uint64_t execution_budget = 50'000'000U);
  // Scheduler/event dispatch slice used for diagnostics. Camera, actors and
  // scripted objects require tickRetailFrame(), which executes the whole frame.
  [[nodiscard]] LegacyMissionTickResult
  tickMission(const LegacyMissionRuntimeProfile &profile =
                  syphonFilterUsaV11MissionProfile(),
              std::uint64_t per_call_execution_budget = 5'000'000U);

  [[nodiscard]] const psx::R3000Runtime &runtime() const noexcept {
    return runtime_;
  }
  [[nodiscard]] psx::R3000Runtime &runtime() noexcept { return runtime_; }
  [[nodiscard]] const psx::PsxMachine &machine() const noexcept {
    return machine_;
  }
  [[nodiscard]] psx::PsxMachine &machine() noexcept { return machine_; }

private:
  [[nodiscard]] LegacyGameplayVmResult
  invokeFrameCall(std::uint32_t address,
                  std::span<const std::uint32_t> arguments,
                  std::uint64_t execution_budget);
  [[nodiscard]] bool advanceAudioClockCallbacks(
      const LegacyRetailAudioProfile &profile,
      std::uint32_t callback_count) noexcept;
  [[nodiscard]] bool finalizeDeadActorDropsBeforeRenderer(
      const LegacyGameplayBridgeProfile &profile,
      std::uint64_t execution_budget) noexcept;
  [[nodiscard]] LegacyGameplayVmResult
  runExecutionPump(std::optional<std::uint32_t> host_boundary,
                   std::uint64_t execution_budget,
                   bool advance_guest_clock = true);
  [[nodiscard]] bool
  issueCdRomCommand(std::uint8_t command,
                    std::span<const std::uint8_t> parameters,
                    bool wait_for_completion = false) noexcept;
  [[nodiscard]] bool transferCdRomSectors(std::uint32_t sector,
                                          std::uint32_t sector_count,
                                          std::uint32_t destination,
                                          std::uint8_t mode) noexcept;
  [[nodiscard]] bool
  waitForCdRomInterrupt(std::uint8_t expected_interrupt) noexcept;
  [[nodiscard]] bool
  acknowledgeCdRomInterrupt(std::uint8_t expected_interrupt) noexcept;
  [[nodiscard]] bool dispatchCdRomReadyCallback();
  [[nodiscard]] LegacyHostCall *findHostCall(std::uint32_t address) noexcept;
  [[nodiscard]] const LegacyHostCall *
  findHostCall(std::uint32_t address) const noexcept;
  void recoverCdRomTransfer() noexcept;

  psx::R3000Runtime runtime_;
  psx::PsxMachine machine_;
  std::unordered_map<std::uint32_t, LegacyHostCall> host_calls_;
  std::vector<LegacyHostCall *> ram_host_calls_;
  std::shared_ptr<LegacyVirtualCd> virtual_cd_;
  std::uint32_t executable_initial_pc_{};
  std::uint32_t video_timing_baseline_{};
  std::uint64_t audio_frame_tick_{};
  std::array<std::uint32_t, LegacyGameplayVmSnapshot::interrupt_callback_count>
      interrupt_callbacks_{};
  std::optional<LegacyHostAimRay> host_aim_ray_;
  std::vector<LegacyWeaponEventBridgeState> weapon_events_;
  mutable std::vector<LegacyGameplayVmSnapshot::AttachedTextSource>
      attached_text_sources_;
  std::vector<LegacyUiMessageBridgeState> ui_messages_;
  std::vector<LegacyGameplayVmSnapshot::PendingActorDrop> pending_actor_drops_;
  LegacyGameplayBridgeReadFault last_bridge_read_fault_{
      LegacyGameplayBridgeReadFault::none};
  LegacyGameplayBridgeReadStage last_bridge_read_stage_{
      LegacyGameplayBridgeReadStage::none};
  std::uint64_t host_aim_ray_patch_count_{};
  std::uint64_t enemy_close_aim_patch_count_{};
  bool video_timing_baseline_initialized_{};
  bool audio_frame_tick_initialized_{};
};

} // namespace sf::game
