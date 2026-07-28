#pragma once

#include "sf/assets/emd_scene.hpp"
#include "sf/assets/gmd_model.hpp"
#include "sf/assets/hmd_model.hpp"
#include "sf/assets/mission_objects.hpp"
#include "sf/assets/tim_image.hpp"
#include "sf/game/campaign_state.hpp"
#include "sf/game/combat.hpp"
#include "sf/game/effects.hpp"
#include "sf/game/hud.hpp"
#include "sf/game/legacy_bridge_types.hpp"
#include "sf/game/mission_scripts.hpp"
#include "sf/game/npc_ai.hpp"
#include "sf/game/player_controller.hpp"
#include "sf/psx/spu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sf::game {

class LegacyFirstMissionRuntime;
class G4CampaignTransitionProbeAccess;
struct LegacyGameplayBridgeState;
struct LegacyObjectBridgeState;
struct LegacyPresentationFrame;
struct LegacyUiCommandFrame;
class MissionPackage;

using GameplayInput = PlayerInput;

struct GameplayAudioVolumes {
  static constexpr std::uint8_t maximum = 100U;

  std::uint8_t sound_effects{maximum};
  std::uint8_t music{maximum};
  std::uint8_t voice_over{maximum};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return sound_effects <= maximum && music <= maximum &&
           voice_over <= maximum;
  }

  [[nodiscard]] friend constexpr bool
  operator==(const GameplayAudioVolumes &,
             const GameplayAudioVolumes &) noexcept = default;
};

struct WorldModel {
  std::string name;
  assets::EmdScene scene;
  assets::EmdBounds bounds;
};

struct ObjectFireEmitter {
  // The retail particle controller selects one of the SPFX animation
  // families by the halfword stored in each live particle. Keep the source
  // families separate so presentation never substitutes fire for smoke or
  // vapor when it consumes guest commands.
  std::vector<assets::TimImage> frames;
  std::vector<assets::TimImage> fire_frames;
  std::vector<assets::TimImage> breath_frames;
  std::vector<assets::TimImage> vapor_frames;
};

using ObjectGeometry = std::variant<assets::GmdModel, assets::EmdScene,
                                    assets::HmdModel, ObjectFireEmitter>;

enum class LegacyPresentationResourceKind : std::uint8_t {
  none,
  gmd,
  emd,
  hmd,
};

// BIN retains the retail .TMD name while the mission HOG contains the
// converted PC presentation. Some animated records pair that HMD with a HAN;
// HAN is animation data, not substitute geometry. Selection remains within
// the exact basename: only .TMD may fall through to an HMD when no GMD/EMD
// conversion exists.
[[nodiscard]] constexpr LegacyPresentationResourceKind
legacyPresentationResourceKind(std::string_view definition_model, bool has_gmd,
                               bool has_emd, bool has_hmd) noexcept {
  if (definition_model.ends_with(".HMD")) {
    return has_hmd ? LegacyPresentationResourceKind::hmd
                   : LegacyPresentationResourceKind::none;
  }
  if (definition_model.ends_with(".EMD")) {
    return has_emd ? LegacyPresentationResourceKind::emd
                   : LegacyPresentationResourceKind::none;
  }
  if (definition_model.ends_with(".GMD")) {
    return has_gmd ? LegacyPresentationResourceKind::gmd
                   : LegacyPresentationResourceKind::none;
  }
  if (definition_model.ends_with(".TMD")) {
    if (has_gmd) {
      return LegacyPresentationResourceKind::gmd;
    }
    if (has_emd) {
      return LegacyPresentationResourceKind::emd;
    }
    if (has_hmd) {
      return LegacyPresentationResourceKind::hmd;
    }
  }
  return LegacyPresentationResourceKind::none;
}

inline constexpr std::uint32_t legacy_common_npc_handler = 0x80061874U;

// HMD is also used by animated props. Retail's class-dispatch table is the
// exact distinction: every campaign NPC class uses FUN_80061874, while
// CHOPPER, HANS and BOMB have dedicated handlers.
[[nodiscard]] constexpr bool
legacyPresentationUsesRetailNpc(bool hmd_backed, std::uint32_t object_handler,
                                std::uint32_t ai_controller) noexcept {
  return hmd_backed && object_handler == legacy_common_npc_handler &&
         ai_controller != 0U;
}

// Guest-owned HMD actors must never fall back to a host-authored bind pose.
// If the retail pose is not complete for the actor's actual HMD, presentation
// fails closed until the guest materializes it.
[[nodiscard]] constexpr bool
legacyHmdRenderAllowed(bool guest_render_authoritative,
                       bool has_exact_guest_pose) noexcept {
  return !guest_render_authoritative || has_exact_guest_pose;
}

inline constexpr std::uint32_t legacy_hmd_rendered_this_pass = 0x40U;
inline constexpr std::uint8_t legacy_instance_dormant = 0x02U;
inline constexpr std::uint8_t legacy_item_consumed_latch = 0x20U;

// WEPCRATE/WEPCRATX are a retail state pair, not damage geometry. The item
// handler at FUN_8008cb5c latches instance byte +0x00 bit 5 after collection;
// health and the generic destroyed bit remain untouched.
[[nodiscard]] constexpr bool
legacyGuestUsesSecondaryItemModel(std::uint32_t class_id,
                                  std::uint8_t instance_flags) noexcept {
  return (class_id == 0x4fU || class_id == 0x50U) &&
         (instance_flags & legacy_item_consumed_latch) != 0U;
}

// HMDs used by story actors and bosses do not all have the common 15-part
// TERRO/CBDC skeleton. Completeness is relative to the resolved model, not to
// the maximum bridge capacity.
[[nodiscard]] constexpr bool
legacyGuestHmdPoseComplete(std::size_t available_bones,
                           std::size_t required_bones) noexcept {
  return required_bones != 0U && available_bones >= required_bones;
}

// A current bridge sample may temporarily expose only part of an HMD table
// while the guest changes display lists. Once an actor lifetime has produced
// a complete pose, retain that exact pose until the lifetime is explicitly
// hidden or retired instead of interpreting a presentation transient as a
// despawn.
[[nodiscard]] constexpr bool legacyGuestActorPoseAvailable(
    bool current_pose_complete, bool retained_pose_complete) noexcept {
  return current_pose_complete || retained_pose_complete;
}

// A resident instance can be allocated long before its authored encounter.
// Simulation activation and render readiness are deliberately separate: the
// actor enters native presentation only after its lifetime has produced a
// complete retail HMD pose. A later partial transition sample may retain that
// pose; the retail render bit remains a positive lifetime/visibility override
// but never authorizes a bind-pose substitute. Instance byte +0x23 bit 1 is
// the retail dormant/hidden latch and wins over every positive signal.
[[nodiscard]] constexpr bool legacyGuestActorStreamVisible(
    bool source_in_active_dat, bool live_position_in_active_dat,
    std::uint32_t pose_flags, bool opening_actor, bool retail_simulated,
    bool retail_dormant, bool retail_pose_available) noexcept {
  const auto retail_rendered =
      (pose_flags & legacy_hmd_rendered_this_pass) != 0U;
  return !retail_dormant && retail_pose_available &&
         (retail_rendered || opening_actor ||
          (retail_simulated &&
           (source_in_active_dat || live_position_in_active_dat)));
}

// Stable identity of one retail object lifetime. Dynamic guest slots are
// recycled; instance is therefore part of the identity even when definition,
// path and authored coordinates are reused by the next lifetime.
[[nodiscard]] std::uint64_t
legacyGuestIdentity(const LegacyObjectBridgeState &guest) noexcept;

// Without a complete world-space bone table, an untouched authored root is a
// contact point. A moving retail root is instead skeleton/root space unless
// its motion controller supplies an exact ground contact.
[[nodiscard]] constexpr bool legacyHmdFallbackUsesContactSpace(
    bool has_complete_guest_pose, bool ground_contact_valid,
    bool root_matches_authored_position) noexcept {
  return !has_complete_guest_pose &&
         (ground_contact_valid || root_matches_authored_position);
}

// Retail camera mode 1 is shared by manual aim and ledge/hang presentation.
// Native first-person visibility therefore belongs to the actual host-held
// aim action, never to that ambiguous guest camera number alone.
[[nodiscard]] constexpr bool legacyManualAimPresentationActive(
    bool host_held, bool native_first_person, std::int32_t retail_camera_mode,
    bool control_locked, bool camera_scripted, bool camera_locked) noexcept {
  static_cast<void>(retail_camera_mode);
  return host_held && native_first_person && !control_locked &&
         !camera_scripted && !camera_locked;
}

// Circle is a zoom-out command only while a retail optic owns first-person
// input. Forwarding the same button to an unscoped weapon enters incompatible
// action paths (most visibly grenade aim + roll) while L1 still owns the
// camera and can corrupt the player motion root.
[[nodiscard]] constexpr bool
legacyFirstPersonCircleAllowed(WeaponId weapon) noexcept {
  return weapon == WeaponId::nightvision_rifle ||
         weapon == WeaponId::sniper_rifle;
}

// Host first-person admission is one contract shared by the native camera and
// the retail PAD bridge. A roll/recovery owns Gabe's collision root, while XA
// playback in gameplay is the retail radio-dialogue channel. Neither state may
// be interrupted by L1, and a held L1 must be released before it can re-arm.
[[nodiscard]] constexpr bool legacyFirstPersonAimInputAllowed(
    unsigned int roll_block_updates, bool action_locked,
    bool radio_conversation_active, bool release_rearm_required) noexcept {
  return roll_block_updates == 0U && !action_locked &&
         !radio_conversation_active && !release_rearm_required;
}

// Retail L1 aim changes the owner and representation of Gabe's collision root.
// Host locomotion must stay neutral from the request edge through the complete
// first-person hold; otherwise simultaneous WASD can advance the native root
// while retail is publishing a pose-space transition sample.
[[nodiscard]] constexpr bool
legacyFirstPersonLocomotionInputAllowed(bool aim_requested) noexcept {
  return !aim_requested;
}

[[nodiscard]] constexpr bool
legacyRetailNpcIsAlly(std::uint8_t ai_archetype) noexcept {
  return (ai_archetype & 1U) == 0U;
}

// PARK2's HANS/Girdeux and CHOPPER are rigid HMD actors with overlay-owned
// handlers, not common NPCs. HANS retains weapon 15 in its exact object
// attributes; CHOPPER's class owns weapon 22 even though its attributes are
// zero. BOMB uses the same rigid presentation path but has no attached weapon.
inline constexpr std::int16_t legacy_park2_hans_class = 0x3c;
inline constexpr std::uint32_t legacy_park2_hans_handler = 0x80147004U;
inline constexpr std::uint16_t legacy_park2_hans_attributes = 0x410fU;
inline constexpr std::int16_t legacy_chopper_class = 0x03;
inline constexpr std::uint16_t legacy_chopper_attributes = 0x0000U;
inline constexpr std::int16_t legacy_bomb_class = 0x2e;

enum class LegacyDedicatedHmdActor : std::uint8_t {
  none,
  park2_bomb,
  park2_hans,
  chopper,
};

[[nodiscard]] constexpr LegacyDedicatedHmdActor legacyDedicatedHmdActor(
    bool hmd_backed, std::uint32_t mission_index, std::uint16_t source_index,
    std::uint32_t definition_index, std::int16_t class_id,
    std::uint32_t object_handler, std::uint16_t attributes) noexcept {
  if (!hmd_backed || object_handler == 0U ||
      object_handler == legacy_common_npc_handler) {
    return LegacyDedicatedHmdActor::none;
  }
  if (mission_index == 4U && source_index == 4U && definition_index == 1U &&
      class_id == legacy_bomb_class && attributes == 0U) {
    return LegacyDedicatedHmdActor::park2_bomb;
  }
  if (mission_index == 4U && source_index == 9U && definition_index == 8U &&
      class_id == legacy_park2_hans_class &&
      object_handler == legacy_park2_hans_handler &&
      attributes == legacy_park2_hans_attributes) {
    return LegacyDedicatedHmdActor::park2_hans;
  }
  if (mission_index == 9U && source_index == 2U && definition_index == 2U &&
      class_id == legacy_chopper_class &&
      attributes == legacy_chopper_attributes) {
    return LegacyDedicatedHmdActor::chopper;
  }
  return LegacyDedicatedHmdActor::none;
}

[[nodiscard]] constexpr bool legacyDedicatedHmdPresentationAllowed(
    LegacyDedicatedHmdActor actor, bool alive, bool resident_presentation,
    bool retail_dormant, bool stream_visible,
    bool has_exact_guest_presentation) noexcept {
  const auto presentation_ready = has_exact_guest_presentation ||
                                  actor == LegacyDedicatedHmdActor::park2_bomb;
  return actor != LegacyDedicatedHmdActor::none && alive &&
         resident_presentation && !retail_dormant && stream_visible &&
         presentation_ready;
}

[[nodiscard]] constexpr std::optional<WeaponId>
legacyDedicatedHmdWeapon(LegacyDedicatedHmdActor actor) noexcept {
  if (actor == LegacyDedicatedHmdActor::park2_hans) {
    return WeaponId::flamethrower;
  }
  if (actor == LegacyDedicatedHmdActor::chopper) {
    return WeaponId::chopper_gun;
  }
  return std::nullopt;
}

enum class ObjectVisualEffect : std::uint8_t {
  none,
  police_lightbar,
  billboard_glow,
};

struct ObjectModel {
  std::string name;
  ObjectVisualEffect visual_effect{ObjectVisualEffect::none};
  ObjectGeometry geometry;
  std::optional<assets::EmdBounds> bounds;
};

struct SceneObject {
  std::uint16_t model{};
  assets::MissionTransform transform;
  std::uint32_t class_id{};
  std::uint16_t source_index{};
  std::optional<std::uint16_t> destroyed_model;
  ObjectDamageResponse damage_response{ObjectDamageResponse::none};
  // BIN definition identity is distinct from source_index: recycled guest
  // objects may use a definition which has no authored static instance.
  std::optional<std::uint32_t> definition_index;
  // Exact guest item-consumed presentation. Kept separate from destruction
  // so an opened weapon crate retains its authored collision and health.
  bool legacy_secondary_model_active{};
  // Legacy VM actor transforms are retail HMD root matrices, not contact
  // points. Their posed model-space ground offset must remain intact.
  bool legacy_hmd_root_space{};
  // Exact world-space retail part matrices. TERRO/CBDC/Gabe share the
  // recovered 15-part HMD order used by the guest display controller.
  std::array<assets::MissionTransform, 15U> legacy_hmd_bones{};
  std::uint8_t legacy_hmd_bone_count{};
  std::array<std::int16_t, 3U> legacy_hmd_back_color_q12{0x1000, 0x1000,
                                                         0x1000};
  bool legacy_hmd_back_color_valid{};
};

// Nearest authored support plane used by renderer-only actor presentation.
// The normal follows the mission's Y-down world convention and points toward
// the solid side of walkable polygons. Keeping this query in GameplaySession
// makes shadows use the same floor/elevator choice as movement and pickups.
struct ActorGroundSurface {
  double y{};
  double normal_x{};
  double normal_y{1.0};
  double normal_z{};
};

// First visible authored receiver between a posed actor vertex and its floor
// projection. It lets renderer-only shadows fold onto walls before reaching
// the support plane without exposing collision internals to presentation.
struct ActorShadowSurfaceHit {
  double x{};
  double y{};
  double z{};
  double normal_x{};
  double normal_y{};
  double normal_z{};
};

enum class ActorAimZone : std::uint8_t {
  body,
  head,
};

struct ActorAimRay {
  double origin_x{};
  double origin_y{};
  double origin_z{};
  double direction_x{};
  double direction_y{};
  double direction_z{};
};

// The unscoped PC sight sits at the centre of the lower half of the original
// 384x240 image: 120 + 60 = 180. Scoped optics retain their authored centre.
inline constexpr double manual_aim_reticle_vertical_offset = 0.0;

struct ActorAimHit {
  ActorAimZone zone{ActorAimZone::body};
  double ray_distance{};
  double target_x{};
  double target_y{};
  double target_z{};
};

// The retail targeting volume has a compact head zone above the broader body
// volume.  Keeping this pure makes contextual aiming independent of rendering.
[[nodiscard]] std::optional<ActorAimHit> actorAimHit(const ActorAimRay &ray,
                                                     double actor_x,
                                                     double actor_y,
                                                     double actor_z) noexcept;

struct GameplayShotEvent {
  bool fired{};
  WeaponId weapon{WeaponId::unarmed};
  std::optional<std::uint16_t> target;
  std::optional<std::uint16_t> object_target;
  bool headshot{};
  bool world_impact{};
  double impact_x{};
  double impact_y{};
  double impact_z{};
};

enum class ProjectilePhase : std::uint8_t {
  flying,
  explosion,
  gas_cloud,
};

enum class MissionCinematicPhase : std::uint8_t {
  intro,
  gameplay,
  finale,
  complete,
};

struct GameplayProjectile {
  bool active{};
  WeaponId weapon{WeaponId::fragmentation_grenade};
  ProjectilePhase phase{ProjectilePhase::flying};
  std::array<std::int16_t, 9U> rotation{};
  bool retail_transform{};
  double x{};
  double y{};
  double z{};
  double velocity_x{};
  double velocity_y{};
  double velocity_z{};
  double radius{};
  unsigned int remaining_updates{};
  unsigned int age_updates{};
};

struct LegacyExplParticle {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t z{};
  std::uint16_t controller{};
  std::int16_t source_slot{-1};
  LegacyEffectSpriteFamily family{LegacyEffectSpriteFamily::explosion};
  std::uint8_t scale_byte{};
  std::uint8_t frame{};
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
};

struct LegacyProjectedFlamePoint {
  std::int16_t x{};
  std::int16_t y{};
};

struct LegacyPark2FlamethrowerRibbon {
  std::array<LegacyProjectedFlamePoint, 4U> corners{};
  // Native-coordinate centres corresponding to corners 0/1 and 2/3.
  LegacyNativePoint world_first;
  LegacyNativePoint world_second;
  std::uint16_t ordering_depth{};
  std::uint8_t slot{};
  std::uint8_t frame{};
  std::uint8_t width_shift{1U};
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
};

struct LegacyWorldCallout {
  std::uint16_t object{};
  std::string text;
  bool headshot{};
};

// Pure lifecycle policy shared by production synchronization and narrow unit
// tests. Dynamic guest slots are a contiguous overlay-owned suffix; identity
// changes rebind the corresponding presentation slot without changing its
// stable native scene index.
[[nodiscard]] constexpr std::optional<std::size_t>
legacyDynamicPoolIndex(std::size_t object_count,
                       std::uint16_t dynamic_first_slot,
                       std::uint32_t guest_slot) noexcept {
  if (dynamic_first_slot > object_count || guest_slot < dynamic_first_slot ||
      static_cast<std::size_t>(guest_slot) >= object_count) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(guest_slot - dynamic_first_slot);
}

[[nodiscard]] constexpr bool legacyDynamicBindingChanged(
    std::uint64_t identity, std::uint64_t previous_identity,
    std::uint16_t scene, std::uint16_t previous_scene) noexcept {
  return identity != previous_identity || scene != previous_scene;
}

[[nodiscard]] constexpr bool legacyPresentationTemplateMatches(
    const std::optional<std::uint32_t> &definition_index,
    std::uint32_t class_id, std::uint32_t guest_definition,
    std::uint32_t guest_class) noexcept {
  return definition_index && *definition_index == guest_definition &&
         class_id == guest_class;
}

[[nodiscard]] constexpr bool
legacySceneActiveAfterRoomRebuild(bool authored_room_active,
                                  std::int32_t guest_slot,
                                  bool script_hidden) noexcept {
  return !script_hidden && (authored_room_active || guest_slot >= 0);
}

// Object textures are authored against one of the two retail VRAM banks.
// A current-room owner is authoritative; otherwise an unambiguous active
// owner/containing-room bank wins. Ambiguous or absent provenance fails closed
// to the current room bank.
[[nodiscard]] constexpr std::uint8_t
resolveTextureBankOwnership(std::uint8_t current_bank,
                            bool current_room_matches,
                            std::uint8_t active_bank_mask) noexcept {
  if (current_room_matches) {
    return current_bank;
  }
  if (active_bank_mask == 0x01U) {
    return 0U;
  }
  if (active_bank_mask == 0x02U) {
    return 1U;
  }
  return current_bank;
}

[[nodiscard]] constexpr std::uint8_t resolveAuthoredObjectTextureBank(
    std::uint8_t current_bank, bool current_is_owner,
    bool current_is_spatial_owner, std::uint8_t spatial_owner_bank_mask,
    std::uint8_t authored_owner_bank_mask) noexcept {
  if (spatial_owner_bank_mask != 0U) {
    return resolveTextureBankOwnership(current_bank, current_is_spatial_owner,
                                       spatial_owner_bank_mask);
  }
  return resolveTextureBankOwnership(current_bank, current_is_owner,
                                     authored_owner_bank_mask);
}

// HMD actors, SPFX and the weapon GMDs are resident mission resources, not
// room assets. Retail composes them into VRAM bank zero and keeps that source
// while the streamed world changes banks at portals (for example SUBWAY and
// MUSEUM). Every HMD texture reference in the 20 retail mission archives is
// backed by bank zero; the same pages in bank one are unrelated world data or
// empty. Spatially rebinding an HMD therefore makes the body disappear while
// its separately submitted weapon remains visible.
inline constexpr std::uint8_t resident_hmd_texture_bank = 0U;
inline constexpr std::uint8_t resident_weapon_texture_bank = 0U;

[[nodiscard]] constexpr std::uint8_t resolveDisplayedObjectTextureBank(
    std::uint8_t object_bank, bool hmd_backed) noexcept {
  return hmd_backed ? resident_hmd_texture_bank : object_bank;
}

// A native checkpoint is only the presentation half of one guest snapshot.
// Restoring it without a healthy runtime would combine unrelated timelines.
[[nodiscard]] constexpr bool
gameplayCheckpointRestoreReady(bool checkpoint_valid, bool runtime_present,
                               bool runtime_ready, bool host_runtime_faulted,
                               bool runtime_faulted) noexcept {
  return checkpoint_valid && runtime_present && runtime_ready &&
         !host_runtime_faulted && !runtime_faulted;
}

class GameplaySession final : private PlayerMovementResolver {
public:
  using LoadProgressCallback = std::function<void(std::uint8_t)>;

  explicit GameplaySession(const MissionPackage &mission,
                           LoadProgressCallback load_progress = {});
  ~GameplaySession();

  void update(const GameplayInput &input);
  void advanceAnimationClock() noexcept;
  void reset();
  [[nodiscard]] bool restartCheckpoint();
  [[nodiscard]] bool activateRetailAllWeaponsCheat() noexcept;
  [[nodiscard]] bool setRetailAllWeaponsCheat(bool enabled) noexcept;
  [[nodiscard]] bool setRetailHardMode(bool enabled) noexcept;
  [[nodiscard]] bool setRetailOneShotKills(bool enabled) noexcept;
  [[nodiscard]] bool setRetailWeakEnemies(bool enabled) noexcept;
  [[nodiscard]] bool activateRetailMovieTheaterCheat() noexcept;
  [[nodiscard]] std::optional<CampaignCarryState>
  campaignCarryState() const noexcept;
  [[nodiscard]] bool
  applyCampaignCarryState(const CampaignCarryState &state) noexcept;
  [[nodiscard]] bool
  setAudioVolumes(const GameplayAudioVolumes &volumes) noexcept;
  [[nodiscard]] std::optional<GameplayAudioVolumes>
  audioVolumes() const noexcept;
  [[nodiscard]] bool advanceAudioFrameClock() noexcept;
  [[nodiscard]] bool advanceAudioSliceClock() noexcept;
  [[nodiscard]] std::size_t
  takePcm(std::span<psx::SpuPcmFrame> destination) noexcept;
  void clearPcm() noexcept;
  [[nodiscard]] std::optional<LegacyAudioDiagnostics>
  audioDiagnostics() const noexcept;

  [[nodiscard]] const PlayerState &player() const noexcept {
    return player_controller_.state();
  }
  [[nodiscard]] std::int32_t playerModelHeading() const noexcept {
    return player_controller_.modelHeading();
  }
  [[nodiscard]] std::uint64_t playerAnimationTick() const noexcept {
    return player_controller_.animationTick();
  }
  [[nodiscard]] std::uint64_t playerActionAnimationTick() const noexcept {
    return player_controller_.actionAnimationTick();
  }
  [[nodiscard]] std::uint64_t playerPresentationAnimationTick() const noexcept {
    return player_controller_.action() == PlayerActionState::ready
               ? player_controller_.animationTick()
               : player_controller_.actionAnimationTick();
  }
  [[nodiscard]] PlayerAnimationRequest playerAnimation() const noexcept;
  [[nodiscard]] PlayerActionState playerAction() const noexcept {
    return player_controller_.action();
  }
  [[nodiscard]] PlayerAimState playerAim() const noexcept;
  [[nodiscard]] CameraState camera() const noexcept;
  [[nodiscard]] double manualAimReticleVerticalOffset() const noexcept;
  [[nodiscard]] std::uint16_t currentRoom() const noexcept {
    return current_room_;
  }
  [[nodiscard]] std::span<const std::uint16_t> activeModels() const noexcept {
    return active_models_;
  }
  [[nodiscard]] const std::vector<WorldModel> &models() const noexcept {
    return models_;
  }
  [[nodiscard]] std::span<const LegacyWorldSectionColorsBridgeState>
  legacyWorldVertexColors() const noexcept {
    return legacy_world_vertex_colors_;
  }
  [[nodiscard]] const std::vector<ObjectModel> &objectModels() const noexcept {
    return object_models_;
  }
  [[nodiscard]] const assets::EmdScene *detachedScrimModel() const noexcept {
    return detached_scrim_ ? &*detached_scrim_ : nullptr;
  }
  [[nodiscard]] const ObjectModel &playerModel() const noexcept {
    return object_models_[player_model_];
  }
  [[nodiscard]] const ObjectModel *weaponModel(WeaponId id) const noexcept;
  [[nodiscard]] const std::vector<SceneObject> &objects() const noexcept {
    return objects_;
  }
  [[nodiscard]] std::span<const std::uint16_t> activeObjects() const noexcept {
    return active_objects_;
  }
  [[nodiscard]] std::uint8_t textureBankAt(double x, double z) const noexcept;
  [[nodiscard]] std::uint8_t
  objectTextureBank(std::uint16_t index) const noexcept;
  [[nodiscard]] std::uint8_t
  displayedObjectTextureBank(std::uint16_t index) const noexcept;
  [[nodiscard]] const GameplayHud &hud() const noexcept { return hud_; }
  [[nodiscard]] bool canEquipWeapon(WeaponId id) const noexcept;
  [[nodiscard]] bool equipWeapon(WeaponId id) noexcept;
  [[nodiscard]] std::optional<WeaponId>
  quickWeapon(std::size_t slot) const noexcept {
    return hud_.inventory().quickSlot(slot);
  }
  [[nodiscard]] std::optional<std::uint16_t> aimTarget() const noexcept {
    return aim_target_;
  }
  [[nodiscard]] bool targetLocked() const noexcept {
    return locked_target_.has_value();
  }
  [[nodiscard]] bool headshotTargeted() const noexcept {
    return headshot_targeted_;
  }
  [[nodiscard]] std::span<const LegacyWorldCallout>
  legacyWorldCallouts() const noexcept {
    return legacy_world_callouts_;
  }
  [[nodiscard]] std::span<const LegacyUiMessageBridgeState>
  legacyUiMessages() const noexcept {
    return legacy_ui_messages_;
  }
  [[nodiscard]] const std::optional<LegacyUiTimerBridgeState> &
  legacyUiTimer() const noexcept {
    return legacy_ui_timer_;
  }
  [[nodiscard]] const GameplayShotEvent &lastShot() const noexcept {
    return last_shot_;
  }
  [[nodiscard]] std::span<const GameplayEffect> effects() const noexcept {
    return effects_;
  }
  [[nodiscard]] std::span<const LegacyExplParticle>
  legacyExplParticles() const noexcept {
    return legacy_expl_particles_;
  }
  [[nodiscard]] std::span<const LegacyPark2FlamethrowerRibbon>
  legacyPark2FlamethrowerRibbons() const noexcept {
    return legacy_park2_flamethrower_ribbons_;
  }
  [[nodiscard]] bool legacyEffectParticlesAuthoritative() const noexcept {
    return legacy_mission_bridge_active_;
  }
  // Exact signed words read from player->motion + {0,4,8}. MENU.OVL feeds
  // these guest coordinates directly into its per-mission map projection.
  [[nodiscard]] const std::optional<LegacyNativePoint> &
  legacyPlayerGuestMotionPosition() const noexcept {
    return legacy_player_guest_motion_position_;
  }
  [[nodiscard]] const std::optional<std::array<std::int16_t, 9U>> &
  legacyPlayerGuestRotation() const noexcept {
    return legacy_player_guest_rotation_;
  }
  [[nodiscard]] bool legacyRenderCommandsAuthoritative() const noexcept {
    // A bridge fault does not transfer presentation authority back to
    // native animation; the frame is dropped and the scene exits.
    return legacy_first_mission_ != nullptr;
  }
  [[nodiscard]] bool runtimeFaulted() const noexcept {
    return legacy_runtime_faulted_;
  }
  [[nodiscard]] std::string_view runtimeFaultReason() const noexcept;
  [[nodiscard]] std::string_view runtimeFaultDetail() const noexcept;
  [[nodiscard]] bool legacyOpeningFinished() const noexcept;
  [[nodiscard]] std::shared_ptr<const LegacyPresentationFrame>
  legacyPresentationFrame() const noexcept;
  [[nodiscard]] std::uint64_t legacyPresentationSequence() const noexcept {
    return legacy_presentation_sequence_;
  }
  [[nodiscard]] std::uint64_t legacyAimRayPatchCount() const noexcept;
  // Read-only bridge identity used by production diagnostics: element N is
  // the retail object-record slot currently presented by SceneObject N, or
  // -1 when the native scene has no guest owner on this frame.
  [[nodiscard]] std::span<const std::int32_t>
  legacyGuestSlotsBySceneObject() const noexcept {
    return legacy_guest_slot_by_scene_object_;
  }
  [[nodiscard]] std::optional<std::uint16_t> taserTarget() const noexcept {
    return taser_tether_updates_ != 0U ? taser_target_ : std::nullopt;
  }
  [[nodiscard]] std::span<const GameplayProjectile>
  projectiles() const noexcept {
    return projectiles_;
  }
  [[nodiscard]] std::uint16_t objectHealth(std::uint16_t index) const noexcept {
    return index < object_health_.size() ? object_health_[index] : 0U;
  }
  [[nodiscard]] bool objectAlive(std::uint16_t index) const noexcept {
    return objectHealth(index) != 0U;
  }
  [[nodiscard]] bool objectDestroyed(std::uint16_t index) const noexcept {
    return index < object_destroyed_.size() && object_destroyed_[index];
  }
  [[nodiscard]] bool objectDestructible(std::uint16_t index) const noexcept {
    return index < objects_.size() &&
           objects_[index].damage_response != ObjectDamageResponse::none;
  }
  [[nodiscard]] const ObjectModel *
  displayedObjectModel(std::uint16_t index) const noexcept;
  [[nodiscard]] const NpcState *npcState(std::uint16_t index) const noexcept;
  [[nodiscard]] bool
  legacyDedicatedActorPresentation(std::uint16_t index) const noexcept;
  // Exact attached weapon for a presented overlay-owned HMD actor. Common
  // NPC weapons remain owned exclusively by NpcState.
  [[nodiscard]] std::optional<WeaponId>
  legacyDedicatedActorWeapon(std::uint16_t index) const noexcept;
  [[nodiscard]] NpcAnimationRequest
  npcAnimation(std::uint16_t index) const noexcept;
  [[nodiscard]] bool playerAlive() const noexcept {
    return hud_.vitals().health != 0U;
  }
  [[nodiscard]] bool missionFailed() const noexcept { return mission_failed_; }
  [[nodiscard]] bool failureRestartRequested() const noexcept {
    return legacy_failure_restart_requested_;
  }
  [[nodiscard]] bool missionComplete() const noexcept {
    return mission_cinematic_phase_ == MissionCinematicPhase::complete;
  }
  [[nodiscard]] std::optional<std::size_t>
  consumeScriptedIntroMovieRequest() noexcept {
    const auto requested = legacy_intro_movie_requested_;
    legacy_intro_movie_requested_.reset();
    return requested;
  }
  [[nodiscard]] bool consumeEndingMovieRequest() noexcept {
    const auto requested = legacy_ending_movie_requested_;
    legacy_ending_movie_requested_ = false;
    return requested;
  }
  [[nodiscard]] bool legacyScriptedCameraActive() const noexcept;
  [[nodiscard]] std::optional<std::int32_t>
  legacyWeaponMenuState() const noexcept;
  [[nodiscard]] bool legacyWeaponMenuDirty() const noexcept;
  [[nodiscard]] bool legacyWeaponMenuReady() const noexcept;
  [[nodiscard]] const SceneObject *legacyPlayerPresentation() const noexcept {
    return legacy_player_presentation_ ? &*legacy_player_presentation_
                                       : nullptr;
  }
  [[nodiscard]] bool cinematic() const noexcept {
    return mission_cinematic_phase_ == MissionCinematicPhase::intro ||
           mission_cinematic_phase_ == MissionCinematicPhase::finale ||
           legacyScriptedCameraActive();
  }
  [[nodiscard]] std::uint8_t mapFade() const noexcept;
  [[nodiscard]] std::uint32_t missionObjectiveCount() const noexcept {
    return legacy_mission_objective_count_;
  }
  [[nodiscard]] std::uint32_t missionParameterCount() const noexcept {
    return legacy_mission_parameter_count_;
  }
  [[nodiscard]] const std::vector<std::string> &
  missionObjectiveTexts() const noexcept {
    return legacy_mission_objective_texts_;
  }
  [[nodiscard]] const std::vector<std::string> &
  missionParameterTexts() const noexcept {
    return legacy_mission_parameter_texts_;
  }
  [[nodiscard]] std::uint32_t completedObjectiveMask() const noexcept {
    return legacy_completed_objectives_;
  }
  [[nodiscard]] std::uint32_t failedObjectiveMask() const noexcept {
    return legacy_failed_objectives_;
  }
  [[nodiscard]] std::uint32_t revealedObjectiveMask() const noexcept {
    return legacy_revealed_objectives_;
  }
  [[nodiscard]] std::uint32_t missionParameterMask() const noexcept {
    return legacy_parameter_mask_;
  }
  [[nodiscard]] std::uint32_t failedParameterMask() const noexcept {
    return legacy_failed_parameters_;
  }
  [[nodiscard]] std::uint64_t playerDeathAnimationTick() const noexcept {
    return death_updates_;
  }
  // Presentation helpers for retail world-space pickups.  The detached guest
  // MATRIX is deliberately below the actor contact point; resolve the actual
  // terrain instead of applying a flat-map offset.  Visibility uses the same
  // authored collision mesh as gameplay so a HUD-derived pickup icon cannot
  // leak through a wall merely because it is submitted in the HUD pass.
  [[nodiscard]] std::optional<double>
  droppedItemGroundY(double x, double z, double reference_y,
                     std::uint16_t retail_room) const noexcept;
  [[nodiscard]] std::optional<ActorGroundSurface>
  actorGroundSurface(double x, double z, double reference_y) const noexcept;
  [[nodiscard]] std::optional<ActorShadowSurfaceHit>
  actorShadowSurface(double from_x, double from_y, double from_z, double to_x,
                     double to_y, double to_z) const noexcept;
  [[nodiscard]] bool
  droppedItemVisibleFrom(double from_x, double from_y, double from_z,
                         double to_x, double to_y, double to_z,
                         std::uint16_t retail_room) const noexcept;

private:
  friend class G4CampaignTransitionProbeAccess;

  struct GuestWeaponRequest {
    std::optional<WeaponId> direct_weapon;
    std::int8_t direction{};
  };

  struct GroundHit {
    double y{};
    std::uint16_t model{};
    double normal_x{};
    double normal_y{1.0};
    double normal_z{};
  };

  [[nodiscard]] GroundHit findGround(double x, double z,
                                     double reference_y) const;
  [[nodiscard]] bool collidesWithWall(double x, double y, double z) const;
  [[nodiscard]] bool tryMove(PlayerState &player, double x, double z) override;
  [[nodiscard]] ActorAimRay manualAimRay() const noexcept;
  [[nodiscard]] double traceWorldSegment(double from_x, double from_y,
                                         double from_z, double to_x,
                                         double to_y,
                                         double to_z) const noexcept;
  void spawnCombatEffect(GameplayEffectType type, double x, double y, double z,
                         double direction_x, double direction_y,
                         double direction_z, double scale = 1.0) noexcept;
  void spawnMuzzleFlash(std::optional<std::uint16_t> npc, double x, double y,
                        double z, double direction_x, double direction_z,
                        double scale) noexcept;
  void spawnActorHitEffects(double x, double y, double z, double source_x,
                            double source_y, double source_z, bool headshot,
                            GameplayEffectAttachment attachment,
                            std::uint16_t owner_object = 0U) noexcept;
  void updateEffects() noexcept;
  void damageNpc(std::uint16_t target, std::uint16_t damage,
                 WeaponDamageKind kind, bool headshot = false) noexcept;
  void updateNpcs(bool player_fired, bool player_rolled) noexcept;
  void updateMissionScripts(bool interact) noexcept;
  void updateScriptedObjects() noexcept;
  void updateCinematic();
  [[nodiscard]] bool legacyMissionAuthoritative() const noexcept;
  void stageNativeFirstPersonAim(const GameplayInput &input);
  void stageLegacyHostState(const GameplayInput &input);
  [[nodiscard]] GameplayInput
  admittedFirstPersonAimInput(const GameplayInput &input) noexcept;
  [[nodiscard]] bool legacyRadioConversationActive() const noexcept;
  void syncLegacyGameplayBridge();
  void syncLegacyUiProjection(const LegacyGameplayBridgeState &bridge,
                              const LegacyUiCommandFrame &ui);
  void syncLegacyActorCombatPresentation(NpcState &state,
                                         const LegacyObjectBridgeState &guest,
                                         bool fresh_guest_sample) noexcept;
  void syncLegacyResidentObjects(const LegacyGameplayBridgeState &bridge,
                                 bool fresh_guest_sample);
  void ensureLegacyDynamicPresentationCapacity(std::size_t capacity);
  void syncLegacyOpeningBridge(const LegacyGameplayBridgeState &bridge,
                               bool fresh_guest_sample);
  [[nodiscard]] bool queueLegacyDamage(std::uint16_t scene_object,
                                       std::uint16_t damage,
                                       WeaponDamageKind kind,
                                       bool headshot = false) noexcept;
  [[nodiscard]] std::optional<std::uint16_t> openingSceneObjectForGuestActor(
      const LegacyObjectBridgeState &actor,
      std::uint16_t dynamic_first_slot) const noexcept;
  void teleportPlayerToSource(std::uint16_t source_index) noexcept;
  void scriptedExplosion(std::uint16_t source_index) noexcept;
  void activateNpc(std::uint16_t object) noexcept;
  void respawnNpc(std::uint16_t object) noexcept;
  [[nodiscard]] bool tryMoveNpc(NpcState &state, double forward_distance,
                                double strafe_distance) noexcept;
  [[nodiscard]] bool
  tryBeginNpcClimb(NpcState &state, const NpcPatrolPoint &target,
                   bool authored_transition = false) noexcept;
  void updateNpcClimb(NpcState &state) noexcept;
  [[nodiscard]] bool updateNpcScriptedIngress(NpcState &state) noexcept;
  void updateOpeningEncounterNpc(NpcState &state) noexcept;
  void updateNpcTransform(std::uint16_t object) noexcept;
  [[nodiscard]] bool isHostileActor(std::uint16_t object) const noexcept;
  [[nodiscard]] bool npcZoneContains(const NpcState &state, double x,
                                     double z) const noexcept;
  [[nodiscard]] std::optional<NpcPatrolPoint>
  findNpcCover(const NpcState &state, const PlayerState &player) const noexcept;
  void updateCameraCollision() noexcept;
  [[nodiscard]] std::vector<std::uint16_t>
  buildActiveModels(std::uint16_t room,
                    std::span<const std::uint16_t> retail_traversal = {}) const;
  void rebuildActiveModels();
  void rebuildActiveObjects();
  void updateCurrentRoom(std::uint16_t ground_model, double player_x,
                         double player_z);
  void resetLegacyWorldVertexColors();
  void captureCheckpoint();

  const MissionPackage &mission_;
  std::vector<WorldModel> models_;
  std::vector<std::uint16_t> active_models_;
  // Complete last-known guest BGR555 state. The guest publishes only its
  // current 4:3 streamed set, while native widescreen can retain adjacent DAT
  // models; absent sections therefore keep their last retail color instead
  // of reverting to the authored (often lamp-lit) EMD value.
  std::vector<LegacyWorldSectionColorsBridgeState> legacy_world_vertex_colors_;
  std::vector<ObjectModel> object_models_;
  std::optional<assets::EmdScene> detached_scrim_;
  std::uint16_t player_model_{};
  std::array<std::optional<std::uint16_t>, weapon_slot_count> weapon_models_{};
  std::vector<SceneObject> objects_;
  std::vector<std::optional<SceneObject>> legacy_object_definition_templates_;
  std::vector<std::uint16_t> source_to_scene_object_;
  std::vector<std::uint16_t> active_objects_;
  std::vector<std::uint16_t> object_health_;
  std::vector<std::uint16_t> object_spawn_health_;
  std::vector<bool> object_destroyed_;
  std::vector<bool> object_script_hidden_;
  std::vector<bool> object_spawn_script_hidden_;
  std::vector<NpcState> npc_states_;
  std::vector<NpcState> npc_spawn_states_;
  std::vector<bool> npc_damaged_;
  std::array<std::uint16_t, 2U> opening_cbdc_objects_{};
  std::array<std::uint16_t, 2U> opening_terrorist_objects_{};
  // One reusable native presentation slot per retail recycled object
  // record. The count and static/dynamic split are overlay data and differ
  // between missions; slots are grown once when a coherent guest bridge is
  // first observed, then rebound by the guest definition and identity.
  std::vector<std::uint16_t> legacy_dynamic_objects_;
  std::array<bool, 2U> legacy_opening_cbdc_seen_{};
  std::array<bool, 2U> legacy_opening_terrorist_seen_{};
  std::array<std::int32_t, 2U> legacy_opening_cbdc_guest_slots_{-1, -1};
  std::array<std::uint64_t, 2U> legacy_opening_cbdc_guest_identities_{};
  std::array<std::int32_t, 2U> legacy_opening_terrorist_guest_slots_{-1, -1};
  std::array<std::uint64_t, 2U> legacy_opening_terrorist_guest_identities_{};
  std::vector<std::int32_t> legacy_guest_slot_by_scene_object_;
  std::vector<bool> legacy_dedicated_actor_presentations_;
  std::vector<std::optional<WeaponId>> legacy_dedicated_actor_weapons_;
  std::vector<std::uint16_t> legacy_dynamic_scene_by_guest_slot_;
  std::vector<std::uint64_t> legacy_dynamic_identity_by_guest_slot_;
  std::optional<std::uint16_t> legacy_dynamic_first_slot_;
  std::optional<std::uint64_t> legacy_last_synced_guest_frame_;
  std::uint64_t legacy_presentation_sequence_{};
  std::unique_ptr<LegacyFirstMissionRuntime> legacy_first_mission_;
  MissionScriptRuntime mission_scripts_;
  GeorgiaMissionState legacy_mission_state_{};
  std::uint32_t legacy_mission_objective_count_{};
  std::uint32_t legacy_mission_parameter_count_{};
  std::vector<std::string> legacy_mission_objective_texts_;
  std::vector<std::string> legacy_mission_parameter_texts_;
  std::uint32_t legacy_completed_objectives_{};
  std::uint32_t legacy_failed_objectives_{};
  std::uint32_t legacy_revealed_objectives_{};
  std::uint32_t legacy_notified_objectives_{};
  std::uint32_t legacy_failed_parameters_{};
  std::uint32_t legacy_parameter_mask_{};
  std::vector<LegacyUiMessageBridgeState> legacy_ui_messages_;
  std::optional<LegacyUiTimerBridgeState> legacy_ui_timer_;
  bool legacy_mission_bridge_active_{};
  std::optional<LegacyNativePoint> legacy_player_guest_motion_position_;
  std::optional<std::array<std::int16_t, 9U>> legacy_player_guest_rotation_;
  std::optional<std::size_t> legacy_intro_movie_requested_;
  bool legacy_ending_movie_requested_{};
  bool legacy_failure_restart_requested_{};
  bool legacy_runtime_faulted_{};
  std::string_view legacy_presentation_fault_detail_{"none"};
  std::optional<SceneObject> legacy_player_presentation_;
  OpeningCinematicCameraRuntime opening_camera_;
  MapFadeRuntime map_fade_;
  PlayerState spawn_;
  PlayerController player_controller_;
  std::uint16_t current_room_{};
  GameplayHud hud_;
  std::optional<std::uint16_t> aim_target_;
  bool headshot_targeted_{};
  std::optional<std::uint16_t> locked_target_;
  GameplayShotEvent last_shot_;
  std::vector<GameplayEffect> effects_;
  std::vector<LegacyExplParticle> legacy_expl_particles_;
  std::vector<LegacyPark2FlamethrowerRibbon> legacy_park2_flamethrower_ribbons_;
  std::vector<LegacyWorldCallout> legacy_world_callouts_;
  std::optional<std::uint16_t> taser_target_;
  unsigned int taser_tether_updates_{};
  std::uint32_t effect_serial_{1U};
  std::vector<GameplayProjectile> projectiles_;
  // The retail preview disappears on the same tick that its projectile
  // descriptor becomes live. Retain the last coherent guide so native
  // presentation can place the flying grenade on that exact parabola.
  std::optional<LegacyGrenadeTrajectoryBridgeState>
      legacy_player_grenade_trajectory_;
  std::optional<WeaponId> pending_equipped_weapon_;
  std::deque<GuestWeaponRequest> pending_guest_weapon_requests_;
  std::optional<WeaponId> pending_guest_weapon_;
  std::deque<std::int8_t> pending_guest_weapon_steps_;
  std::int8_t guest_weapon_in_flight_direction_{};
  std::optional<WeaponId> guest_weapon_in_flight_expected_;
  bool pending_guest_weapon_menu_{};
  bool guest_quick_weapon_pending_{};
  bool host_manual_aim_{};
  bool retail_host_aim_active_{};
  unsigned int first_person_aim_roll_block_updates_{};
  bool first_person_aim_release_rearm_required_{};
  double host_manual_aim_strafe_{};
  std::optional<std::int32_t> host_manual_aim_body_heading_;
  std::optional<std::int32_t> pending_host_aim_heading_restore_;
  std::optional<LegacyCameraBridgeState> legacy_manual_aim_neutral_camera_;
  LegacyNativePoint legacy_manual_aim_neutral_player_root_;
  CameraState camera_state_{};
  double camera_collision_distance_{};
  PlayerCameraMode camera_mode_{PlayerCameraMode::chase};
  bool camera_collision_initialized_{};
  unsigned int death_updates_{};
  bool mission_failed_{};
  MissionCinematicPhase mission_cinematic_phase_{
      MissionCinematicPhase::gameplay};
  unsigned int mission_cinematic_updates_{};
  std::uint64_t mission_script_updates_{};
  bool finale_explosion_played_{};
  bool checkpoint_pending_{};
  bool checkpoint_valid_{};
  PlayerController checkpoint_player_controller_{};
  std::vector<std::uint16_t> checkpoint_active_models_;
  std::vector<LegacyWorldSectionColorsBridgeState>
      checkpoint_legacy_world_vertex_colors_;
  std::optional<WeaponId> checkpoint_pending_equipped_weapon_;
  std::deque<GuestWeaponRequest> checkpoint_pending_guest_weapon_requests_;
  std::optional<WeaponId> checkpoint_pending_guest_weapon_;
  std::deque<std::int8_t> checkpoint_pending_guest_weapon_steps_;
  std::int8_t checkpoint_guest_weapon_in_flight_direction_{};
  std::optional<WeaponId> checkpoint_guest_weapon_in_flight_expected_;
  bool checkpoint_pending_guest_weapon_menu_{};
  bool checkpoint_guest_quick_weapon_pending_{};
  std::uint16_t checkpoint_room_{};
  GameplayHud checkpoint_hud_{};
  GameplayShotEvent checkpoint_last_shot_{};
  std::vector<GameplayEffect> checkpoint_effects_;
  std::uint32_t checkpoint_effect_serial_{1U};
  std::vector<LegacyUiMessageBridgeState> checkpoint_legacy_ui_messages_;
  std::optional<LegacyUiTimerBridgeState> checkpoint_legacy_ui_timer_;
  MapFadeRuntime checkpoint_map_fade_;
  CameraState checkpoint_camera_state_{};
  double checkpoint_camera_collision_distance_{};
  PlayerCameraMode checkpoint_camera_mode_{PlayerCameraMode::chase};
  bool checkpoint_camera_collision_initialized_{};
  unsigned int checkpoint_death_updates_{};
  bool checkpoint_mission_failed_{};
  MissionCinematicPhase checkpoint_mission_cinematic_phase_{
      MissionCinematicPhase::gameplay};
  unsigned int checkpoint_mission_cinematic_updates_{};
  bool checkpoint_finale_explosion_played_{};
  std::vector<std::uint16_t> checkpoint_object_health_;
  std::vector<bool> checkpoint_object_destroyed_;
  std::vector<NpcState> checkpoint_npc_states_;
  std::vector<bool> checkpoint_npc_damaged_;
  std::vector<bool> checkpoint_object_script_hidden_;
  MissionScriptRuntime checkpoint_mission_scripts_;
  std::vector<GameplayProjectile> checkpoint_projectiles_;
  std::uint64_t checkpoint_mission_script_updates_{};
  std::array<bool, 2U> checkpoint_legacy_opening_cbdc_seen_{};
  std::array<bool, 2U> checkpoint_legacy_opening_terrorist_seen_{};
  std::array<std::int32_t, 2U> checkpoint_legacy_opening_cbdc_guest_slots_{-1,
                                                                           -1};
  std::array<std::uint64_t, 2U>
      checkpoint_legacy_opening_cbdc_guest_identities_{};
  std::array<std::int32_t, 2U> checkpoint_legacy_opening_terrorist_guest_slots_{
      -1, -1};
  std::array<std::uint64_t, 2U>
      checkpoint_legacy_opening_terrorist_guest_identities_{};
  std::vector<std::int32_t> checkpoint_legacy_guest_slot_by_scene_object_;
  std::vector<std::uint16_t> checkpoint_legacy_dynamic_scene_by_guest_slot_;
  std::vector<std::uint64_t> checkpoint_legacy_dynamic_identity_by_guest_slot_;
  std::optional<std::uint16_t> checkpoint_legacy_dynamic_first_slot_;
  std::optional<std::uint64_t> checkpoint_legacy_last_synced_guest_frame_;
  std::optional<SceneObject> checkpoint_legacy_player_presentation_;
  std::optional<std::size_t> checkpoint_legacy_intro_movie_requested_;
  bool checkpoint_legacy_ending_movie_requested_{};
};

} // namespace sf::game
