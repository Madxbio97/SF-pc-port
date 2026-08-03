#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sf::game {

// Read-only snapshot used by the host audio trace. It intentionally exposes
// clocks and queue depths only; diagnostics must never mutate guest state.
struct LegacyAudioDiagnostics {
  std::uint64_t machine_tick{};
  std::uint64_t audio_frame_tick{};
  std::uint64_t spu_sample_clock{};
  std::uint64_t spu_mixed_frames{};
  std::uint64_t spu_dropped_pcm_frames{};
  std::uint32_t cd_lba{};
  std::size_t spu_pcm_frames{};
  std::size_t spu_cd_frames{};
  std::size_t active_spu_voices{};
  std::uint16_t spu_control{};
  std::uint16_t spu_status{};
  std::uint8_t cd_mode{};
  std::uint8_t cd_reading{};
  std::uint8_t cd_muted{};
  std::uint8_t cd_adpcm_muted{};
  std::uint8_t xa_stream_set{};
  std::uint8_t xa_file{};
  std::uint8_t xa_channel{};
  bool audio_frame_tick_initialized{};
};

struct LegacyNativePoint {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t z{};

  [[nodiscard]] friend bool operator==(const LegacyNativePoint &,
                                       const LegacyNativePoint &) = default;
};

inline constexpr std::size_t legacy_inventory_weapon_count = 26U;
inline constexpr std::size_t legacy_weapon_events_per_frame = 16U;
inline constexpr std::size_t legacy_effect_particle_capacity = 160U;

// SPFX family identity is part of the guest/native presentation contract, so
// keep its layout with the bridge types rather than in GameplaySession.
enum class LegacyEffectSpriteFamily : std::uint8_t {
  fire = 1U,
  explosion = 2U,
  breath = 3U,
  vapor = 4U,
};

struct LegacyEffectSpriteLayout {
  std::size_t frame_count{};
  std::uint16_t width_words{};
  std::uint16_t height{};

  [[nodiscard]] friend constexpr bool
  operator==(const LegacyEffectSpriteLayout &,
             const LegacyEffectSpriteLayout &) noexcept = default;
};

[[nodiscard]] constexpr LegacyEffectSpriteLayout
legacyEffectSpriteLayout(LegacyEffectSpriteFamily family) noexcept {
  switch (family) {
  case LegacyEffectSpriteFamily::fire:
    return {16U, 16U, 64U};
  case LegacyEffectSpriteFamily::explosion:
    return {12U, 16U, 32U};
  case LegacyEffectSpriteFamily::breath:
    return {16U, 8U, 16U};
  case LegacyEffectSpriteFamily::vapor:
    return {8U, 16U, 32U};
  }
  return {};
}

[[nodiscard]] constexpr bool
legacyEffectSpriteFrameValid(std::uint8_t family, std::uint8_t frame) noexcept {
  if (family == 0U) {
    // update7/render3 lamp and glass fragments use a mission/VLF texture,
    // not one of the four animated SPFX families.
    return frame == 0U;
  }
  const auto layout =
      legacyEffectSpriteLayout(static_cast<LegacyEffectSpriteFamily>(family));
  return layout.frame_count != 0U && frame < layout.frame_count;
}

enum class LegacyWeaponEventType : std::uint8_t {
  shot,
  thrown,
  scanner_begin,
  scanner_end,
  flashlight_toggle,
  key_card_use,
  c4_use,
  antigen_use,
};

[[nodiscard]] constexpr bool
legacyWeaponEventUsesFirstPerson(std::uint32_t aim_mode) noexcept {
  // DAT_80115e80 value 1 is the ordinary third-person targeting state. Only
  // 2 (sniper) and 3 (night vision) transfer presentation to the optic camera.
  return aim_mode == 2U || aim_mode == 3U;
}

// One accepted retail weapon/use boundary. Damage, ammunition, projectiles
// and mission callbacks have already remained guest-owned; this immutable
// edge carries only the data needed to present that exact action natively.
struct LegacyWeaponEventBridgeState {
  LegacyWeaponEventType type{};
  std::uint8_t weapon{};
  std::int16_t actor_slot{-1};
  std::int16_t aimed_target_slot{-1};
  std::uint32_t hit_result{};
  LegacyNativePoint origin;
  LegacyNativePoint endpoint;
  bool first_person{};
  bool enabled{};
};

struct LegacyCameraBridgeState {
  LegacyNativePoint eye;
  LegacyNativePoint target;
  // Unsigned halfword passed by retail to GsSetProjection/SetGeomScreen.
  // The current HLE display bootstrap can leave this stale, so native
  // presentation rebuilds it from the authoritative FOV channel below.
  std::int32_t projection{320};
  std::int32_t fov_raw{};
  std::int32_t mode{-1};
  bool scripted{};
  bool locked{};
  std::int16_t presentation_viewport_y{};
  std::int16_t presentation_viewport_height{240};
  // Exact retail letterbox ownership. This is changed only by the original
  // viewport animation, unlike XA, camera locks and scripted-camera modes.
  bool retail_letterbox_active{};

  [[nodiscard]] double fovDegrees() const noexcept {
    return static_cast<double>(fov_raw) * 360.0 / 4096.0;
  }
  [[nodiscard]] std::int32_t
  projectionForDisplayWidth(std::int32_t width) const noexcept;
};

struct LegacyFadeBridgeState {
  std::int16_t step{};
  std::uint16_t current{};
  std::uint8_t floor{};
  std::uint32_t callback{};
  bool initialized{};

  [[nodiscard]] double blackOpacity() const noexcept;
};

struct LegacyPlayerBridgeState {
  LegacyNativePoint position;
  std::array<std::int16_t, 9U> guest_rotation{};
  std::int16_t room{-1};
  bool resident{};
  bool control_locked{};
};

// The exact processed PAD sample consumed by the retail gameplay tick.
// buttons is the byte-swapped internal mask produced by FUN_800d7aec
// (L2/R2/L1/R1 are bits 0/1/2/3 respectively).
struct LegacyPadBridgeState {
  std::uint16_t buttons{};
  std::uint8_t left_x{0x80U};
  std::uint8_t left_y{0x80U};
  std::uint8_t right_x{0x80U};
  std::uint8_t right_y{0x80U};
};

inline constexpr std::size_t legacy_actor_bone_count = 15U;
inline constexpr std::size_t legacy_tracked_target_count = 6U;

struct LegacyNativeMatrix {
  std::array<std::int16_t, 9U> rotation{};
  LegacyNativePoint translation;
};

struct LegacyObjectBridgeState {
  std::uint32_t slot{};
  std::uint32_t definition{};
  std::int16_t class_id{};
  // Exact common/overlay handler selected by the retail class table. This is
  // the authoritative discriminator between NPC HMDs and animated HMD props.
  std::uint32_t object_handler{};
  std::uint16_t attributes{};
  std::int32_t parameter{};
  std::int32_t linked_slot{-1};
  std::int16_t maximum_health{};
  std::int16_t health{};
  // Retail identity for recycled dynamic slots. Unlike slot, these values
  // remain tied to the currently authored actor/object instance.
  LegacyNativePoint authored_position;
  std::uint32_t path_pointer{};
  std::uint32_t instance{};
  std::uint32_t root_node{};
  // First word of the retail display descriptor at instance+0x08. The
  // descriptor itself is allocated during object construction; this child
  // becomes non-zero only while the renderer owns a live display node.
  std::uint32_t display_node{};
  std::uint32_t pose_flags{};
  // FUN_800d0058 loads these signed Q12 channels into the GTE background
  // colour before submitting this HMD. Zero is the authored tunnel blackout;
  // 0x1000 is the maximum white NCDT result.
  std::array<std::int16_t, 3U> hmd_back_color_q12{0x1000, 0x1000, 0x1000};
  bool hmd_back_color_valid{};
  std::uint32_t motion_controller{};
  std::uint32_t presentation_controller{};
  std::uint32_t target_controller{};
  std::uint32_t health_controller{};
  std::uint32_t ai_controller{};
  // Retail generic-object death handlers latch destroyed/hidden at
  // instance+0x00 bit 7 even when the object-record health remains positive.
  static constexpr std::uint8_t destroyed_latch = 0x80U;
  std::uint8_t instance_flags{};
  std::array<std::uint8_t, 4U> instance_state{};
  std::uint32_t ai_flags{};
  std::uint8_t ai_fire_latch{};
  std::uint8_t ai_route_node{};
  std::uint8_t ai_previous_route_node{};
  std::uint16_t ai_route_flags{};
  std::uint8_t ai_mode{};
  std::uint8_t ai_archetype{};
  std::uint8_t ai_combat_mode{};
  std::uint8_t ai_pool_index{};
  std::uint16_t ai_state{};
  std::int16_t target_slot{-1};
  std::uint32_t target_flags{};
  // Retail TARGET meter (target controller +0x58), expressed as 0..100.
  // This is aim/hit confidence, not the selected object's health.
  std::int16_t target_meter{};
  // Retail per-object DANGER contribution (target controller +0xd4).
  // The low value is Q12 after masking the controller flag bits.
  std::uint32_t danger_q12{};
  std::uint8_t presentation_enabled{};
  std::uint8_t presentation_mode{};
  bool resident{};
  bool simulated{};
  bool has_target{};
  LegacyNativePoint position;
  // Row-major 12-bit fixed-point PSX rotation. Runtime MATRIX translation is
  // already in the native coordinate convention used by the renderer.
  std::array<std::int16_t, 9U> guest_rotation{};
  // Final retail world-space part matrices from display+0x18. When present,
  // the native renderer can draw the guest pose directly without replaying a
  // guessed PCHAN clip over an already animated guest root.
  std::array<LegacyNativeMatrix, legacy_actor_bone_count> bone_matrices{};
  std::uint8_t bone_matrix_count{};
  std::int32_t ground_contact_y{};
  bool ground_contact_valid{};

  [[nodiscard]] bool destroyed() const noexcept {
    return health <= 0 || (instance_flags & destroyed_latch) != 0U;
  }
  [[nodiscard]] bool alive() const noexcept { return resident && !destroyed(); }
};

struct LegacyExplParticleBridgeState {
  // Retail effect particles keep authored Y (opposite the runtime root-node
  // MATRIX convention); GameplaySession performs that single sign change.
  LegacyNativePoint position;
  std::uint16_t controller{};
  std::int16_t source_slot{-1};
  std::uint8_t family{};
  std::uint8_t scale_byte{};
  std::uint8_t frame{};
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
  // Stable EXPL pool identity. Camera-list GsSPRITE provenance uses the same
  // index, allowing presentation to suppress only the exact represented
  // particle without changing the bridge ABI/layout.
  std::int16_t pool_index{-1};
};

struct LegacyRgbBridgeState {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};

  [[nodiscard]] friend constexpr bool
  operator==(const LegacyRgbBridgeState &,
             const LegacyRgbBridgeState &) noexcept = default;
};

struct LegacyProjectedPointBridgeState {
  std::int16_t x{};
  std::int16_t y{};

  [[nodiscard]] friend constexpr bool
  operator==(const LegacyProjectedPointBridgeState &,
             const LegacyProjectedPointBridgeState &) noexcept = default;
};

// Exact guest-owned camera auxiliary lists consumed by FUN_800c84f4. These
// commands have already been projected by retail; native presentation must not
// rebuild them from guessed world-space emitters.
struct LegacyGuestSpriteBridgeState {
  std::uint32_t source_address{};
  std::uint32_t attribute{};
  std::int16_t x{};
  std::int16_t y{};
  std::uint16_t width{};
  std::uint16_t height{};
  std::uint16_t tpage{};
  std::uint8_t u{};
  std::uint8_t v{};
  std::uint16_t center_x{};
  std::uint16_t center_y{};
  LegacyRgbBridgeState color;
  std::int16_t mapping_x{};
  std::int16_t mapping_y{};
  std::int16_t scale_x{};
  std::int16_t scale_y{};
  std::int32_t rotation{};
  std::uint32_t ordering_depth{};
  // Sprites can come from either the world-camera renderer or the independent
  // interface renderer. Preserve the exact renderer-wide GsSortSprite mode.
  bool renderer_fast_path{};
  // FUN_80040ba8 owns thirteen SCP-bearing sprites in the interface list.
  // This provenance lets only that authored scope UI survive native aim.
  bool retail_scope_overlay{};
  // FUN_800540dc embeds this GsSPRITE at particle+0x28. Provenance is
  // populated only when that exact pool identity and controller chain are
  // coherent; non-particle sprites remain -1/0/0.
  std::int16_t effect_particle{-1};
  std::uint8_t effect_family{};
  std::uint8_t effect_frame{};
  LegacyNativePoint effect_position;
  // Lamp halos are authored as update7/render3 particle sprites. They emit
  // light and therefore must bypass the guest RGB modulation which normally
  // shades glass fragments and other camera-list sprites.
  bool force_fullbright{};
};

[[nodiscard]] constexpr bool
legacyLampHaloSourceClass(std::int16_t class_id) noexcept {
  switch (class_id) {
  case 0x13: // PRLIT/AHALT/BARLITE/ARMYLT/LABLT/FLATLT/CAVLIT
  case 0x15: // GLIT/YLIT
  case 0x16: // SPOTLT
  case 0x33: // BARLIT/LAMPY
  case 0x34: // LIGHT/POOLT
  case 0x46: // SUBLIT
  case 0x47: // MET/RNLT
    return true;
  default:
    return false;
  }
}

struct LegacyGuestSpriteSortTransform {
  double local_left{};
  double local_top{};
  double local_right{};
  double local_bottom{};
  double scale_x{1.0};
  double scale_y{1.0};
  double angle_units{};
};

// GsSortFastSprite reads only x/y/w/h. The regular GsSortSprite path applies
// mapping origin, Q12 scale and authored rotation. Selection is the camera
// render-context byte +0x09, never a bit in GsSPRITE.attribute.
[[nodiscard]] constexpr LegacyGuestSpriteSortTransform
legacyGuestSpriteSortTransform(const LegacyGuestSpriteBridgeState &sprite,
                               bool renderer_fast_path) noexcept {
  if (renderer_fast_path) {
    return LegacyGuestSpriteSortTransform{
        0.0,
        0.0,
        static_cast<double>(sprite.width),
        static_cast<double>(sprite.height),
        1.0,
        1.0,
        0.0,
    };
  }
  const auto left = -static_cast<double>(sprite.mapping_x);
  const auto top = -static_cast<double>(sprite.mapping_y);
  return LegacyGuestSpriteSortTransform{
      left,
      top,
      left + sprite.width,
      top + sprite.height,
      static_cast<double>(sprite.scale_x) / 4096.0,
      static_cast<double>(sprite.scale_y) / 4096.0,
      static_cast<double>(sprite.rotation) / 360.0,
  };
}

inline constexpr std::uint32_t legacy_retail_scope_sprite_stride = 0x2cU;
inline constexpr std::uint32_t legacy_retail_scope_vertical_sprite_count = 5U;
inline constexpr std::uint32_t legacy_retail_scope_horizontal_sprite_count =
    8U;

[[nodiscard]] constexpr bool legacyGuestSpriteIsRetailScopeOverlayAddress(
    std::uint32_t source, std::uint32_t vertical_begin,
    std::uint32_t horizontal_begin) noexcept {
  const auto in_array = [source](std::uint32_t begin, std::uint32_t count) {
    if (begin == 0U || source < begin) {
      return false;
    }
    const auto delta = source - begin;
    return delta % legacy_retail_scope_sprite_stride == 0U &&
           delta / legacy_retail_scope_sprite_stride < count;
  };
  return in_array(vertical_begin, legacy_retail_scope_vertical_sprite_count) ||
         in_array(horizontal_begin,
                  legacy_retail_scope_horizontal_sprite_count);
}

struct LegacyGuestLineBridgeState {
  std::uint32_t attribute{};
  LegacyProjectedPointBridgeState first;
  LegacyProjectedPointBridgeState second;
  LegacyRgbBridgeState first_color;
  LegacyRgbBridgeState second_color;
};

inline constexpr std::size_t legacy_guest_raw_packet_words = 6U;

[[nodiscard]] constexpr bool
legacyGuestRawPacketNeedsDrawMode(std::uint8_t opcode) noexcept {
  return (opcode & 0x02U) != 0U;
}

static_assert(legacyGuestRawPacketNeedsDrawMode(0x22U));
static_assert(legacyGuestRawPacketNeedsDrawMode(0x52U));
static_assert(!legacyGuestRawPacketNeedsDrawMode(0x20U));
static_assert(!legacyGuestRawPacketNeedsDrawMode(0x50U));

[[nodiscard]] constexpr std::uint32_t
legacyGuestRawPacketOtIndex(std::uint32_t depth,
                            std::uint32_t ot_length) noexcept {
  return ot_length == 0U ? 0U : depth < ot_length ? depth : ot_length - 1U;
}

static_assert(legacyGuestRawPacketOtIndex(0U, 4096U) == 0U);
static_assert(legacyGuestRawPacketOtIndex(4096U, 4096U) == 4095U);

struct LegacyGuestRawPacketBridgeState {
  std::uint32_t source_address{};
  std::uint32_t ordering_depth{};
  std::uint8_t word_count{};
  std::uint8_t opcode{};
  std::array<std::uint32_t, legacy_guest_raw_packet_words> words{};
  std::int16_t effect_particle{-1};
  bool effect_world_position_valid{};
  LegacyNativePoint effect_position;
};

struct LegacyGuestRawPacketAddressRange {
  std::uint32_t begin{};
  std::uint32_t stride{};
  std::uint32_t count{};

  [[nodiscard]] constexpr bool contains(std::uint32_t source) const noexcept {
    return stride != 0U && source >= begin &&
           source < begin + stride * count &&
           (source - begin) % stride == 0U;
  }
};

// Fixed INTERFACE raw arrays used by FUN_80041830/FUN_800426a0 and the
// viral-detector controller. Only linked entries are active in a frame.
inline constexpr LegacyGuestRawPacketAddressRange
    legacy_retail_scope_line_packets{0x8011c138U, 0x18U, 26U};
inline constexpr LegacyGuestRawPacketAddressRange
    legacy_retail_scope_quad_packets{0x8011c498U, 0x24U, 16U};
inline constexpr LegacyGuestRawPacketAddressRange
    legacy_retail_scope_triangle_packets{0x8011c840U, 0x1cU, 2U};
inline constexpr LegacyGuestRawPacketAddressRange
    legacy_virus_scanner_line_packets{0x8011c138U, 0x18U, 28U};
inline constexpr LegacyGuestRawPacketAddressRange
    legacy_virus_scanner_target_dot_packets{0x80135df8U, 0x24U, 1U};
inline constexpr std::array<LegacyGuestRawPacketAddressRange, 4U>
    legacy_fixed_retail_optic_packet_ranges{
        legacy_virus_scanner_line_packets,
        legacy_retail_scope_quad_packets,
        legacy_retail_scope_triangle_packets,
        legacy_virus_scanner_target_dot_packets,
    };

[[nodiscard]] constexpr bool legacyGuestRawPacketIsRetailScopeOverlay(
    const LegacyGuestRawPacketBridgeState &packet) noexcept {
  // FUN_80041830/FUN_800426a0 share these fixed raw-packet arrays. Their
  // active counts differ by optic, but these are the complete retail bounds.
  return legacy_retail_scope_line_packets.contains(packet.source_address) ||
         legacy_retail_scope_quad_packets.contains(packet.source_address) ||
         legacy_retail_scope_triangle_packets.contains(packet.source_address);
}

[[nodiscard]] constexpr bool legacyGuestRawPacketIsVirusScannerOverlay(
    const LegacyGuestRawPacketBridgeState &packet) noexcept {
  return legacy_virus_scanner_line_packets.contains(packet.source_address) ||
         legacy_virus_scanner_target_dot_packets.contains(
             packet.source_address);
}

[[nodiscard]] constexpr bool legacyGuestRawPacketIsRetailOpticOverlay(
    const LegacyGuestRawPacketBridgeState &packet) noexcept {
  return legacyGuestRawPacketIsRetailScopeOverlay(packet) ||
         legacyGuestRawPacketIsVirusScannerOverlay(packet);
}

[[nodiscard]] constexpr bool legacyGuestSpriteUsesWorldDepth(
    const LegacyGuestSpriteBridgeState &sprite) noexcept {
  return sprite.effect_particle >= 0;
}

[[nodiscard]] constexpr bool legacyGuestRawPacketUsesWorldDepth(
    const LegacyGuestRawPacketBridgeState &packet) noexcept {
  return packet.effect_particle >= 0 && packet.effect_world_position_valid;
}

[[nodiscard]] constexpr bool legacyGuestCameraItemVisibleWithNativeFirstPerson(
    bool first_person_aim, bool uses_world_depth) noexcept {
  // Native presentation owns the complete first-person HUD and optic. Guest
  // camera lists contain the same already-projected reticle plus world effects.
  // Only explicit effect-pool provenance may survive into the world pass.
  return !first_person_aim || uses_world_depth;
}

// PARK2's Girdeux overlay owns a 72-entry POLY_FT4 ribbon. Preserve both the
// exact projected packet and the two world-space centres which produced it so
// native presentation can reproject the flame through its interpolated camera.
struct LegacyPark2FlamethrowerRibbonBridgeState {
  std::array<LegacyProjectedPointBridgeState, 4U> corners{};
  // Guest-coordinate world centres. world_first produced corners 0/1 and
  // world_second produced corners 2/3; native presentation negates Y once.
  LegacyNativePoint world_first;
  LegacyNativePoint world_second;
  LegacyRgbBridgeState color;
  std::uint16_t ordering_depth{};
  std::uint8_t slot{};
  // Exact SPFX EXPL frame index. PARK2 rotates EXPL002..EXPL005 by slot.
  std::uint8_t frame{};
  // FUN_80147a8c forms its screen-space perpendicular with an arithmetic
  // right shift by one for a fresh segment and by two once history exists.
  std::uint8_t width_shift{1U};
};

enum class LegacyLineParticleKind : std::uint8_t {
  rain_streak = 1U,
  rain_splash = 2U,
  ballistic_tracer = 4U,
  moving_trail = 7U,
};

// Exact world endpoints and packet colors of retail SPFX LINE_G2 particles.
// FUN_800558c0 owns ballistic tracers; FUN_80055298 owns moving trails,
// including the eight children emitted by an M79 shot.
struct LegacyLineParticleBridgeState {
  LegacyNativePoint first;
  LegacyNativePoint second;
  std::uint16_t controller{};
  std::uint16_t particle{};
  std::int16_t source_slot{-1};
  std::int16_t remaining_updates{};
  LegacyLineParticleKind kind{LegacyLineParticleKind::ballistic_tracer};
  LegacyRgbBridgeState first_color;
  LegacyRgbBridgeState second_color;
  bool semi_transparent{};
  // FUN_80055298 projects both endpoints before and after the retail physics
  // step. A bouncing update7 particle can clamp its position and replace its
  // velocity during that step, so its previous world endpoint is no longer
  // recoverable from the post-update pool state. Preserve the exact tagged
  // guest LINE_G2 for those particles instead of reconstructing it.
  bool raw_packet_authoritative{};
  // PARK update1/render5 expands a ground splash in projected pixels while
  // its centre remains an exact world point. Native presentation converts
  // this half-width back through the active camera so it stays on the floor.
  std::uint8_t screen_half_width{};
};

// Raw camera packets and semantic world lines are captured from the same
// immutable guest tick. A matching pool identity means the raw projected
// packet is normally suppressed and the world line reprojected by the native
// camera. Even bouncing update7 lines retain two usable world endpoints. The
// post-bounce endpoint is preferable to a screen-space packet because only
// the former can be correctly clipped by the native world depth buffer.
[[nodiscard]] constexpr bool legacyGuestRawPacketHasWorldLine(
    const LegacyGuestRawPacketBridgeState &packet,
    std::span<const LegacyLineParticleBridgeState> current_lines) noexcept {
  if (packet.effect_particle < 0) {
    return false;
  }
  const auto particle = static_cast<std::uint16_t>(packet.effect_particle);
  for (const auto &line : current_lines) {
    if (line.particle == particle) {
      return true;
    }
  }
  return false;
}

enum class LegacyCombatParticleKind : std::uint8_t {
  // FUN_8004e1f0 creates this update6/render2 LINE_F2 at the weapon part.
  // Despite its shot origin, retail uses it as a short, flat-colored moving
  // line rather than a textured muzzle-flash sprite.
  ejected_shot_line = 6U,
  // FUN_8004ec5c creates this update9/render0 flat-colored POLY_F3 at the
  // exact collision point for the secondary blood/impact family.
  blood_impact_triangle = 9U,
};

// World-space source and appearance inputs for retail flat combat particles.
// The guest packet contains PS1-projected vertices, so native presentation
// consumes these pre-projection fields and applies its own camera exactly once.
struct LegacyCombatParticleBridgeState {
  LegacyNativePoint position;
  std::uint16_t controller{};
  std::uint16_t particle{};
  std::int16_t attached_slot{-1};
  std::int16_t source_slot{-1};
  std::int16_t remaining_updates{};
  LegacyCombatParticleKind kind{LegacyCombatParticleKind::ejected_shot_line};
  LegacyRgbBridgeState color;
  std::uint8_t scale_byte{};
  // update6: submitted angle, angular step, unused.
  // update9: first angle and the second/third triangle angle offsets.
  std::int16_t angle{};
  std::int16_t second_angle{};
  std::int16_t third_angle{};
  bool semi_transparent{};
};

// update6/render2 and update9/render0 packets are also present in the raw
// camera list. Once native presentation reconstructs the semantic particle,
// suppress only the packet with the same pool identity from this exact guest
// tick; retained catch-up particles may refer to a subsequently reused slot.
[[nodiscard]] constexpr bool legacyGuestRawPacketHasWorldCombatParticle(
    const LegacyGuestRawPacketBridgeState &packet,
    std::span<const LegacyCombatParticleBridgeState>
        current_particles) noexcept {
  if (packet.effect_particle < 0) {
    return false;
  }
  const auto particle = static_cast<std::uint16_t>(packet.effect_particle);
  for (const auto &current : current_particles) {
    if (current.particle == particle) {
      return true;
    }
  }
  return false;
}

// Retail FUN_80085eb0 text which is attached to one guest object. The guest
// owns both lifetime and attachment; native presentation only resolves the
// object slot and draws the already-active command.
struct LegacyWorldCalloutBridgeState {
  std::int16_t guest_slot{-1};
  std::string text;
  bool headshot{};
};

// Exact retail screen-text presentation packets. The guest text system owns
// reveal, fade, wrapping, alignment and lifetime; native presentation draws
// only the immutable glyph geometry/color sampled after the completed 20 Hz
// guest update.
enum class LegacyUiMessageChannel : std::uint8_t {
  centered,
  status,
};

struct LegacyUiGlyphBridgeState {
  std::int16_t x{};
  std::int16_t y{};
  std::uint8_t u{};
  std::uint8_t v{};
  std::uint8_t width{};
  std::uint8_t height{};
  LegacyRgbBridgeState color;

  [[nodiscard]] friend constexpr bool
  operator==(const LegacyUiGlyphBridgeState &,
             const LegacyUiGlyphBridgeState &) noexcept = default;
};

struct LegacyUiBackdropBridgeState {
  std::array<LegacyProjectedPointBridgeState, 4U> corners{};
  LegacyRgbBridgeState color;
  bool semi_transparent{};

  [[nodiscard]] friend constexpr bool
  operator==(const LegacyUiBackdropBridgeState &,
             const LegacyUiBackdropBridgeState &) noexcept = default;
};

struct LegacyUiMessageBridgeState {
  LegacyUiMessageChannel channel{LegacyUiMessageChannel::status};
  // Retained for diagnostics, save states, and presentation-only substitution
  // of recognized controller prompt tokens. Timing and layout remain driven
  // by the guest's glyph/backdrop packets.
  std::string text;
  std::uint32_t duration{};
  // The virus-scanner result shares the optic TEXT allocator but is ordinary
  // gameplay copy. Keep it out of the untouched English optic-glyph route.
  bool force_gameplay_layout{};
  std::vector<LegacyUiGlyphBridgeState> glyphs;
  std::optional<LegacyUiBackdropBridgeState> backdrop;

  [[nodiscard]] friend bool
  operator==(const LegacyUiMessageBridgeState &,
             const LegacyUiMessageBridgeState &) = default;
};

struct LegacyUiTimerBridgeState {
  std::int32_t remaining_ticks{};
  std::uint16_t handle{0xffffU};
  std::vector<LegacyUiGlyphBridgeState> glyphs;

  [[nodiscard]] friend bool
  operator==(const LegacyUiTimerBridgeState &,
             const LegacyUiTimerBridgeState &) = default;
};

// Immutable environment fields from the active retail camera render context.
// clear_color is the authored horizon/backdrop. fog_color and DQA/DQB are the
// exact GsFOGPARAM, while terrain_depth_cue is the authored retail DPCS setup
// consumed by the opaque world pass. The renderer can temporarily replace it
// with a close-range darkness envelope; that state must cross the bridge too.
struct LegacyEnvironmentBridgeState {
  LegacyRgbBridgeState clear_color;
  LegacyRgbBridgeState back_color;
  LegacyRgbBridgeState fog_color;
  // FUN_800c9140 appends a screen-covering semitransparent TILE at OT depth
  // zero. The descriptor supplies both its authored RGB and the exact PS1
  // ABR material used by the rain/frost atmosphere grade.
  LegacyRgbBridgeState screen_filter_color;
  LegacyRgbBridgeState nightvision_clear_color;
  std::int32_t fog_dqa{};
  std::int32_t fog_dqb{};
  // Retail terrain DPCS control: low 16 bits are the depth threshold and
  // high 16 bits are the left shift. Zero selects the retail 0x07ff default.
  std::uint32_t terrain_depth_cue{};
  // DAT_80116450 is the renderer's live copy. It is retained for diagnostics;
  // per-object overrides are not safe to apply to the whole native scene.
  std::uint32_t active_terrain_depth_cue{};
  std::uint32_t screen_filter_material{};
  std::uint16_t renderer_display_flags{};
  std::uint16_t renderer_flags{};
  // FUN_800d3100 can select this DPCS envelope while drawing an individual
  // dark-frame object. Keep the observation for diagnostics, but never apply
  // that transient object override to the complete native camera pass.
  bool renderer_darkness_enabled{};
  bool screen_filter_enabled{};
  bool nightvision_enabled{};
  // Retail overrides ClearImage only for the alternate HMD colour. Green SVD
  // keeps the camera's authored clear and receives its cast from the TILE.
  bool nightvision_clear_override_enabled{};
  bool background_enabled{};

  static constexpr std::uint32_t renderer_darkness_depth_cue = 0x00040100U;

  [[nodiscard]] constexpr std::uint32_t
  effectiveTerrainDepthCue() const noexcept {
    return terrain_depth_cue;
  }

  // CAVE2's immutable camera setup is the retail blackout discriminator. Its
  // authored live vertex colors remain the fine-grained light map; this state
  // supplies the dark ambient base which the flashlight/NV passes brighten.
  [[nodiscard]] constexpr bool blackoutEnabled() const noexcept {
    return background_enabled && terrain_depth_cue == 0x00021f40U &&
           fog_dqa == 0 && clear_color == LegacyRgbBridgeState{} &&
           fog_color == LegacyRgbBridgeState{};
  }

  // A lone/stale DQB must not turn an unreadable or disabled depth-cue setup
  // into a full-screen color wash. Retail SetFogNearFar always supplies DQA.
  [[nodiscard]] bool fogEnabled() const noexcept { return fog_dqa != 0; }
};

// Exact GPU-to-GPU copy authored by the retail SCRIM controller. Coordinates
// are PSX VRAM word/pixel coordinates and are relocated only by the native
// texture-residency layer which owns the corresponding logical pages.
struct LegacyVramMoveBridgeState {
  std::int16_t source_x{};
  std::int16_t source_y{};
  std::int16_t width{};
  std::int16_t height{};
  std::int16_t destination_x{};
  std::int16_t destination_y{};

  [[nodiscard]] friend bool
  operator==(const LegacyVramMoveBridgeState &,
             const LegacyVramMoveBridgeState &) noexcept = default;
};

// The authored 13-packet ring shifts two paired 64-word strips by one word
// per positive phase. Both strips return exactly after 2*64 phases.
inline constexpr std::uint64_t legacy_scrim_copy_cycle = 128U;

struct LegacyScrimBridgeState {
  bool resource_present{};
  bool visible{};
  // Final MATRIX owned by the live SCRIM display coordinate. Retail updates
  // this after the camera each frame; native presentation must not invent a
  // camera-facing transform for the detached environment model.
  LegacyNativeMatrix transform;
  bool transform_valid{};
  // The retail controller keeps the thirteen DR_MOVE packets linked only
  // during the positive half of its signed state.  The negative half removes
  // those packets; it does not execute the copies in reverse.
  bool vram_moves_active{};
  std::vector<LegacyVramMoveBridgeState> vram_moves;
};

inline constexpr std::size_t legacy_vertex_light_capacity = 4U;

// One source linked through the retail FUN_800cd734 light list. The MATRIX
// and shaping fields are consumed verbatim by FUN_800c973c/FUN_800d3b8c;
// they are not a native radius, cone or RGB intensity approximation.
struct LegacyVertexLightBridgeState {
  std::uint32_t source{};
  std::uint32_t flags{};
  LegacyNativeMatrix matrix;
  std::int32_t shape{};
  std::uint32_t screen_shift{};
  std::uint32_t depth_shift{};
  std::int32_t threshold{};
  std::uint32_t channel_mask{};

  [[nodiscard]] friend bool
  operator==(const LegacyVertexLightBridgeState &,
             const LegacyVertexLightBridgeState &) noexcept = default;
};

// Current BGR555 colors from the relocated world EMD. Retail lamp toggles and
// animated light groups mutate these guest vertices in place; replaying the
// immutable colors is the only exact native representation of that state.
struct LegacyWorldSectionColorsBridgeState {
  std::uint16_t model{};
  std::uint16_t section{};
  std::vector<std::uint16_t> colors;

  [[nodiscard]] friend bool
  operator==(const LegacyWorldSectionColorsBridgeState &,
             const LegacyWorldSectionColorsBridgeState &) = default;
};

// FUN_80045f84 detaches an enemy's carried item into this fixed 30-slot
// retail pool.  Weapons retain their native inventory id; 0x80 is the
// separately-authored armour pickup.
struct LegacyDroppedItemBridgeState {
  std::uint8_t slot{};
  std::uint16_t room{};
  std::uint16_t item{};
  // FUN_80045f84 moves the display into the fixed pickup MATRIX pool. Type
  // 0x11 is a camera-facing pickup sprite, but retail links it through the
  // primary world list so its OT depth is shared with Gabe and the level.
  // Preserve the complete transform used to project and sort that sprite.
  LegacyNativeMatrix transform;

  [[nodiscard]] friend bool
  operator==(const LegacyDroppedItemBridgeState &,
             const LegacyDroppedItemBridgeState &) noexcept = default;
};

// FUN_80027584 links the fixed player projectile display through object+0 and
// FUN_80026808 advances its MATRIX at object+0x1c until the retail callback
// removes that link. Preserve that exact transform instead of reconstructing
// a second host trajectory from the fire edge.
struct LegacyThrownProjectileBridgeState {
  std::uint8_t age{};
  std::uint8_t weapon{};
  LegacyNativeMatrix transform;
};

// Read-only preview of FUN_80026608 -> FUN_80027584 while the player holds a
// grenade throw. Retail grows the launch angle from this exact charge clock;
// the native renderer samples the resulting parabola as short green stitches.
struct LegacyGrenadeTrajectoryBridgeState {
  std::uint16_t strength_q12{};
  LegacyNativePoint origin;
  LegacyNativePoint target;
};

struct LegacyGameplayBridgeState {
  LegacyCameraBridgeState camera;
  LegacyFadeBridgeState fade;
  LegacyEnvironmentBridgeState environment;
  LegacyScrimBridgeState scrim;
  LegacyPlayerBridgeState player;
  LegacyPadBridgeState pad;
  // Exact retail renderer traversal for this guest frame. FUN_800c973c writes
  // one portal-depth/visibility score per world model through the buffer
  // registered by FUN_80080930. Non-zero entries belong to the original 4:3
  // camera traversal; they are not the complete streamed terrain set. The DAT
  // +0x78 resident list is exported separately. LegacyPresentationFrame
  // deep-copies both vectors before publishing.
  std::uint16_t world_model_count{};
  std::int32_t object_activation_distance{};
  bool terrain_triggers_enabled{};
  std::vector<std::uint16_t> active_world_models;
  std::vector<std::uint16_t> resident_world_models;
  bool target_lock_active{};
  LegacyNativePoint virus_scanner_target;
  std::int32_t virus_scanner_target_slot{-1};
  bool virus_scanner_target_valid{};
  // True only when the DAT_8012f9b8 source owns a validated node in the retail
  // dynamic-light list. DAT_8012f9b8 itself is a 0x40-byte source, not a byte
  // latch; its first word is the allocator/list handle.
  bool flashlight_enabled{};
  std::vector<LegacyVertexLightBridgeState> vertex_lights;
  std::vector<LegacyWorldSectionColorsBridgeState> world_vertex_colors;
  // FUN_8004d278 raises 1 while launching the conductor, its callback raises
  // 2 while the target is being shocked, and FUN_8004d0cc raises 3 on stop.
  std::uint16_t taser_conductor_phase{};
  // Signed guest object-record slot latched by FUN_80046a74.
  std::int16_t taser_target_slot{-1};
  // FUN_8002fd18 writes these independently from the lock-on controller.
  // They are the authoritative first-person ray hit and nearby plot target.
  std::uint32_t target_hit_result{};
  std::int16_t aimed_target_slot{-1};
  std::int16_t proximity_target_slot{-1};
  // Actual contents of DAT_8011ba00. Each signed value is a guest object
  // record slot; -1 is the vacant-entry sentinel.
  std::array<std::int16_t, legacy_tracked_target_count> tracked_slots{};
  std::uint16_t dynamic_first_slot{};
  std::vector<LegacyObjectBridgeState> objects;
  std::vector<LegacyDroppedItemBridgeState> dropped_items;
  std::optional<LegacyGrenadeTrajectoryBridgeState> grenade_trajectory;
  std::optional<LegacyThrownProjectileBridgeState> thrown_projectile;
  std::optional<LegacyThrownProjectileBridgeState> enemy_thrown_projectile;
  std::vector<LegacyExplParticleBridgeState> expl_particles;
  std::vector<LegacyLineParticleBridgeState> line_particles;
  std::vector<LegacyCombatParticleBridgeState> combat_particles;
  std::vector<LegacyPark2FlamethrowerRibbonBridgeState>
      park2_flamethrower_ribbons;
  std::vector<LegacyGuestSpriteBridgeState> guest_sprites;
  std::vector<LegacyGuestLineBridgeState> guest_lines;
  std::vector<LegacyGuestRawPacketBridgeState> guest_raw_packets;
  // Exact camera render-context byte +0x09 consumed by FUN_800c84f4. The
  // selection belongs to the whole immutable list, not to GsSPRITE.attribute.
  // Fast sorting ignores mx/my, scale and rotation.
  bool renderer_sprite_fast_path{};
  // True only after all three FUN_800c84f4 auxiliary lists were captured
  // coherently from the same guest frame. Empty vectors are then authoritative
  // (the retail frame genuinely submitted no auxiliary primitives).
  bool guest_camera_lists_captured{};
  std::vector<LegacyWorldCalloutBridgeState> world_callouts;
  std::vector<LegacyWeaponEventBridgeState> weapon_events;

  [[nodiscard]] bool taserConductorActive() const noexcept {
    return (taser_conductor_phase == 1U || taser_conductor_phase == 2U) &&
           taser_target_slot >= 0;
  }
};

inline constexpr std::size_t legacy_mission_entry_limit = 32U;

struct LegacyInventoryBridgeState {
  std::uint8_t current_weapon{};
  std::uint32_t owned_weapons{};
  std::array<std::uint16_t, legacy_inventory_weapon_count> magazines{};
  std::array<std::uint16_t, legacy_inventory_weapon_count> reserves{};
};

struct LegacyMissionBridgeState {
  std::int16_t player_slot{-1};
  std::int16_t player_health{};
  std::int16_t player_armor{};
  std::uint32_t objective_count{};
  std::uint32_t parameter_count{};
  // The retail mission overlay owns two count + C-string-pointer tables at
  // progress +0/+4 and +8/+0xc. Keep an immutable host copy so native pause
  // UI never substitutes mission-specific text.
  std::vector<std::string> objective_texts;
  std::vector<std::string> parameter_texts;
  std::uint32_t completed_objectives{};
  std::uint32_t failed_objectives{};
  std::uint32_t revealed_objectives{};
  std::uint32_t notified_objectives{};
  std::uint32_t failed_parameters{};
  std::uint32_t parameter_mask{};
  // FUN_800405f4 uses -5 for closed quick-switch, -4..-1 for opening, and
  // consumes directional long-menu pulses only once the phase reaches zero.
  std::int32_t weapon_menu_state{-5};
  bool weapon_menu_dirty{};
  std::uint8_t interface_mode{};
  // FUN_800410d0 advances the normal gameplay HUD through this exact retail
  // phase: -1 is detached, 0..12 are authored geometry, and 13 is settled.
  std::int32_t normal_hud_phase{13};
  std::uint8_t first_person_aim_mode{};
  std::int32_t scope_zoom_raw{};
  bool weapon_menu_controller_ready{};
  bool weapon_menu_input_ready{};
  LegacyInventoryBridgeState inventory;
  bool success{};
  bool terminal{};
  bool failure{};
  bool failure_transition{};
  std::vector<LegacyUiMessageBridgeState> messages;
  std::optional<LegacyUiTimerBridgeState> timer;
};

// Validates the immutable retail renderer handoff without constructing a
// synthetic display list. Resident models are resource state; active models
// are the guest camera traversal, and both sets must be independently valid.
[[nodiscard]] bool
validateLegacyWorldModelSets(const LegacyGameplayBridgeState &state,
                             std::size_t expected_model_count) noexcept;
} // namespace sf::game
