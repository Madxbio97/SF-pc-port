#pragma once

#include "sf/game/effects.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sf::game {

inline constexpr std::size_t maximum_dynamic_lights = 32U;

struct DynamicLightPoint {
  double x{};
  double y{};
  double z{};

  [[nodiscard]] friend constexpr bool
  operator==(const DynamicLightPoint &,
             const DynamicLightPoint &) noexcept = default;
};

struct DynamicLightRgb {
  double red{};
  double green{};
  double blue{};

  [[nodiscard]] friend constexpr bool
  operator==(const DynamicLightRgb &,
             const DynamicLightRgb &) noexcept = default;
};

enum class DynamicLightKind : std::uint8_t {
  street_lamp,
  police_lightbar,
  steady_fire,
  muzzle_flash,
  explosion,
  flashlight,
};

// Persistent sources are created only after the renderer has positively
// identified an authored light object. The guest-owned active/resident and
// destroyed latches remain authoritative; this policy never revives a lamp.
struct PersistentDynamicLightState {
  DynamicLightKind kind{DynamicLightKind::street_lamp};
  DynamicLightPoint position;
  std::uint32_t source_id{};
  bool identity_confirmed{};
  bool active{};
  bool resident{};
  bool destroyed{};
};

// Attached effects must be resolved to their exact interpolated world anchor
// before reaching this module. A missing actor/muzzle anchor therefore fails
// closed instead of emitting a light at stale guest coordinates.
struct TransientDynamicLightState {
  GameplayEffectType effect_type{GameplayEffectType::muzzle_flash};
  DynamicLightPoint position;
  std::uint32_t source_id{};
  double scale{1.0};
  std::uint16_t remaining_updates{};
  std::uint16_t total_updates{};
  bool position_confirmed{};
};

struct DynamicLight {
  DynamicLightKind kind{DynamicLightKind::street_lamp};
  DynamicLightPoint position;
  DynamicLightRgb color;
  double radius{};
  double intensity{};
  DynamicLightPoint direction;
  double inner_cone_cosine{-1.0};
  double outer_cone_cosine{-1.0};
  std::uint32_t source_id{};
  bool transient{};
  bool directional{};
};

struct DirectionalDynamicLightState {
  DynamicLightKind kind{DynamicLightKind::flashlight};
  DynamicLightPoint position;
  DynamicLightPoint direction;
  std::uint32_t source_id{};
  bool identity_confirmed{};
  bool enabled{};
};

struct DynamicLightFrame {
  std::array<DynamicLight, maximum_dynamic_lights> lights{};
  std::size_t count{};

  [[nodiscard]] constexpr std::span<const DynamicLight>
  active() const noexcept {
    return std::span<const DynamicLight>{lights}.first(count);
  }
};

// Builds a bounded immutable frame. Transient combat lights have priority;
// within each class the sources nearest the camera are retained.
[[nodiscard]] DynamicLightFrame buildDynamicLightFrame(
    std::span<const PersistentDynamicLightState> persistent,
    std::span<const TransientDynamicLightState> transient,
    DynamicLightPoint observer,
    std::span<const DirectionalDynamicLightState> directional = {},
    std::uint64_t animation_tick = 0U) noexcept;

// Static level geometry already contains the retail lamp/fire contribution in
// its authored vertex colours. Reapplying reconstructed persistent sources
// washes out lamp-off rooms and other intentionally dark sections. Keep only
// genuinely dynamic events and the player flashlight for that baked pass;
// actors and movable props continue to use the complete frame.
[[nodiscard]] DynamicLightFrame
dynamicLightFrameForBakedWorld(const DynamicLightFrame &frame) noexcept;

struct DynamicLightModulation {
  double red{};
  double green{};
  double blue{};
};

// Returns additive normalized modulation. Shadow projection is a separate
// actor-only pass; existing retail fog/depth cue remains responsible for
// distance.
[[nodiscard]] DynamicLightModulation
sampleDynamicLighting(const DynamicLightFrame &frame,
                      DynamicLightPoint point) noexcept;

// Surface-aware variant used by scene geometry. The supplied normal is
// expected to face the visible side of the polygon. It keeps point sources
// from illuminating the back of walls while retaining a small wrapped edge
// response on the deliberately low-poly retail meshes.
[[nodiscard]] DynamicLightModulation
sampleDynamicLighting(const DynamicLightFrame &frame, DynamicLightPoint point,
                      DynamicLightPoint surface_normal) noexcept;

struct DynamicLightVertexColor {
  std::uint8_t red{128U};
  std::uint8_t green{128U};
  std::uint8_t blue{128U};

  [[nodiscard]] friend constexpr bool
  operator==(const DynamicLightVertexColor &,
             const DynamicLightVertexColor &) noexcept = default;
};

// GMD display controllers store their scene back-light in signed Q12. The
// textured GPU primitive uses 128 (not 255) as neutral modulation, therefore
// Q12 1.0 maps to 128 and values below/above it darken/brighten the model.
// Retail GMD consumes one intensity rather than HMD's three independent RGB
// channels, so use the fixed Rec. 709 integer luminance weights.
[[nodiscard]] DynamicLightVertexColor retailGmdBackColorModulation(
    std::array<std::int16_t, 3U> back_color_q12) noexcept;

struct SceneTriangleLightSample {
  DynamicLightVertexColor color;
  double surface_y{};
};

// Interpolates the authored/live scene color at a point projected onto a
// triangle in X/Z. Vertical walls fail closed; callers choose the nearest
// horizontal surface when floors overlap.
[[nodiscard]] std::optional<SceneTriangleLightSample>
sampleSceneTriangleLighting(std::array<DynamicLightPoint, 3U> vertices,
                            std::array<DynamicLightVertexColor, 3U> colors,
                            DynamicLightPoint point) noexcept;

// The retail renderer keeps at most four FUN_800cd6d8 light records in the
// intrusive list rooted at DAT_80116464. Unlike the native enhancement lights
// above, these records do not describe a radial RGB source. FUN_800d40a4
// projects every vertex into each light's Q12 matrix and FUN_800d3b8c shapes
// the authored primitive color with integer screen/depth falloff.
inline constexpr std::size_t maximum_retail_vertex_lights = 4U;

struct RetailVertexLightMatrix {
  // Row-major PS1 Q12 rotation. Translation remains in guest coordinates.
  std::array<std::int16_t, 9U> rotation{};
  std::array<std::int32_t, 3U> translation{};
};

struct RetailVertexLightState {
  RetailVertexLightMatrix matrix;
  std::uint32_t flags{};
  std::int32_t extent{};
  std::uint32_t screen_shift{};
  std::uint32_t depth_shift{};
  std::int32_t threshold{};
  std::uint32_t channel_mask{0x00ffffffU};
};

struct RetailVertexLightRay {
  DynamicLightPoint origin;
  DynamicLightPoint direction;
};

// Returns the exact native-space axis represented by the retail light MATRIX.
// Attached lights (flag bit zero) are reversed on X/Z by FUN_800c973c before
// GsSetView2, so their illuminated half-space follows local -Z. This is also
// the authoritative axis for the held flashlight's bounded vertex light.
[[nodiscard]] std::optional<RetailVertexLightRay>
retailVertexLightRay(const RetailVertexLightState &light) noexcept;

// Applies the original FUN_800d3b8c/FUN_800d3cb4 color path. packed_color is
// PS1 R|G<<8|B<<16|code<<24. A code byte with bit 7 set is deliberately left
// untouched, matching the retail signed-color guard. EMD world vertices and
// the renderer matrices already share the guest Y-down coordinate space;
// projection is the guest GTE H value. Invalid inputs and malformed records
// fail closed.
[[nodiscard]] std::uint32_t applyRetailVertexLightingPacked(
    std::uint32_t packed_color, std::span<const RetailVertexLightState> lights,
    DynamicLightPoint world_point, std::int32_t projection) noexcept;

[[nodiscard]] DynamicLightVertexColor
applyRetailVertexLighting(DynamicLightVertexColor base,
                          std::span<const RetailVertexLightState> lights,
                          DynamicLightPoint world_point,
                          std::int32_t projection) noexcept;

// PS1 textured primitives use 128 as neutral modulation. Dynamic illumination
// adds up to the remaining headroom and never darkens the authored base color.
// The retail luminance also acts as the exposure mask, preserving intentionally
// black/dim level sections instead of letting native lights wash through them.
[[nodiscard]] DynamicLightVertexColor
applyDynamicLighting(DynamicLightVertexColor base,
                     DynamicLightModulation modulation) noexcept;

struct DynamicShadowProjection {
  // Direction travelled by light rays in the mission's Y-down coordinates.
  DynamicLightPoint ray_direction{0.2971125411, 0.9284766909, 0.2228344058};
  double darkness{0.16};
  bool source_driven{};
};

// Presentation history for one actor shadow. Selection remains an
// authoritative 20 Hz operation; the previous/current pair is sampled at the
// display refresh rate so source changes do not turn into visible direction
// jumps. Callers keep one state per stable actor identity and discard it when
// that identity leaves the resident scene.
struct DynamicShadowProjectionState {
  DynamicShadowProjection previous;
  DynamicShadowProjection current;
  std::uint64_t guest_tick{};
  bool initialized{};
};

// Blends every eligible source which can physically cast onto the supplied
// support plane. Continuous score weights avoid hard direction changes when
// two nearby lights exchange dominance. Low/behind-floor effects are rejected
// and grazing rays are bounded, preventing an actor shadow from stretching
// across the whole room. A stable scene key light keeps actors grounded.
[[nodiscard]] DynamicShadowProjection
selectDynamicShadowProjection(const DynamicLightFrame &frame,
                              DynamicLightPoint actor_anchor,
                              DynamicLightPoint ground_normal) noexcept;

// Advances temporal shadow selection exactly once per guest update. Repeated
// calls for the same tick are idempotent; a guest clock rollback (mission
// restart/load) starts a fresh history. Brief combat flashes and authored
// light animation are already removed by selectDynamicShadowProjection, so
// this policy only damps genuine key-light transitions.
[[nodiscard]] DynamicShadowProjectionState
advanceDynamicShadowProjection(const DynamicShadowProjectionState &state,
                               const DynamicShadowProjection &target,
                               std::uint64_t guest_tick) noexcept;

// Interpolates the last two 20 Hz presentation states. amount is the existing
// host-frame interpolation fraction in [0, 1]. Invalid/uninitialized state
// fails closed to the stable scene key projection.
[[nodiscard]] DynamicShadowProjection
sampleDynamicShadowProjection(const DynamicShadowProjectionState &state,
                              double amount) noexcept;

// Projects one posed actor vertex onto an arbitrary support plane. The small
// normal offset avoids coplanar Z fighting while the dedicated depth-tested,
// non-depth-writing shadow pass keeps walls and actors in front.
[[nodiscard]] std::optional<DynamicLightPoint>
projectDynamicShadowPoint(DynamicLightPoint vertex,
                          DynamicLightPoint ground_point,
                          DynamicLightPoint ground_normal,
                          const DynamicShadowProjection &projection) noexcept;

struct DynamicLightSurfaceTriangle {
  DynamicLightPoint first;
  DynamicLightPoint second;
  DynamicLightPoint third;
};

struct DynamicLightSurfaceHit {
  DynamicLightPoint point;
  DynamicLightPoint normal;
  double distance{};
};

struct DynamicLightBounds {
  DynamicLightPoint minimum;
  DynamicLightPoint maximum;
};

// Cheap slab broadphase for the bounded flashlight segment. Invalid bounds or
// direction fail closed before the renderer visits any world polygons.
[[nodiscard]] bool dynamicLightSegmentIntersectsBounds(
    DynamicLightPoint origin, DynamicLightPoint direction,
    double maximum_distance, const DynamicLightBounds &bounds) noexcept;

// Two-sided nearest-hit primitive used by the flashlight presentation. It is
// pure and bounded so malformed/degenerate world polygons fail closed.
[[nodiscard]] std::optional<DynamicLightSurfaceHit>
dynamicLightSurfaceHit(DynamicLightPoint origin, DynamicLightPoint direction,
                       const DynamicLightSurfaceTriangle &triangle,
                       double maximum_distance) noexcept;

} // namespace sf::game
