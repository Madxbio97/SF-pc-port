#include "sf/game/gameplay.hpp"

#include "sf/core/error.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace sf::game {
namespace {

constexpr double maximum_ground_step = 160.0;
constexpr double minimum_floor_normal = 0.60;
constexpr double maximum_wall_normal_y = 0.45;
constexpr double player_radius = 58.0;
constexpr double player_height = 390.0;
constexpr double maximum_target_distance = npc_maximum_sight_distance;
constexpr double actor_target_radius = 145.0;
constexpr double actor_target_height = 185.0;
// TERRO.HMD places the grounded head around 275 units above the actor origin.
// The slightly forgiving retail-sized volume keeps manual PS1-style aiming
// stable without allowing the chest centre to count as a head shot.
constexpr double actor_head_radius = 85.0;
constexpr double actor_head_height = 275.0;
constexpr double minimum_world_segment_hit = 0.001;
constexpr double target_visibility_limit = 0.98;
constexpr double camera_wall_clearance = 18.0;
// PsyCross clips gameplay geometry at roughly 34 native units. Keep a small
// safety margin in first person so a complete close wall can never land
// wholly behind that plane and reveal the room beyond it.
constexpr double first_person_camera_near_clearance = 38.0;
constexpr double minimum_camera_view_distance = 40.0;
constexpr double camera_release_per_update = 60.0;
constexpr double scripted_ingress_run_distance =
    657.86 / npc_updates_per_second;
constexpr unsigned int scripted_fence_climb_updates = 46U;
constexpr double scripted_fence_climb_height = 360.0;
constexpr double native_actor_zone_padding = 900.0;
constexpr double native_stationary_actor_zone_padding = 1600.0;
constexpr unsigned int opening_car_flight_updates = 44U;
constexpr std::uint16_t opening_cbdc_source = 172U;
constexpr std::uint16_t opening_terrorist_source = 184U;
constexpr std::uint16_t opening_police_car_source = 57U;
constexpr std::uint32_t moving_elevator_class = 0x0bU;
constexpr std::array<std::uint16_t, 2U> legacy_elevator_sources{62U, 342U};
constexpr std::uint16_t opening_native_cbdc_first_slot = 0xff00U;
constexpr std::uint16_t opening_native_terrorist_first_slot = 0xff10U;
constexpr std::array<std::uint32_t, 2U> opening_cbdc_path_pointers{
    0x801a4a6cU, // descriptor 6, lane nearest the subway wall
    0x801a431cU, // descriptor 6, lane nearest the police car
};
constexpr std::array<std::uint32_t, 2U> opening_cbdc_dynamic_slot_offsets{
    2U, // slot 352
    1U, // slot 351
};
constexpr std::uint32_t opening_initial_dynamic_terrorist_path_pointer =
    0x801a4934U;
constexpr std::int32_t opening_guest_slot_unbound = -1;
constexpr std::int32_t opening_guest_slot_retired = -2;

struct GrenadePresentationPoint {
  double x{};
  double y{};
  double z{};
};

std::optional<GrenadePresentationPoint> retailGrenadePresentationPoint(
    const LegacyGrenadeTrajectoryBridgeState &trajectory,
    const LegacyNativePoint &retail_position) noexcept {
  const auto origin_x = static_cast<double>(trajectory.origin.x);
  const auto origin_y = static_cast<double>(trajectory.origin.y);
  const auto origin_z = static_cast<double>(trajectory.origin.z);
  const auto horizontal_x =
      static_cast<double>(trajectory.target.x) - origin_x;
  const auto horizontal_z =
      static_cast<double>(trajectory.target.z) - origin_z;
  const auto horizontal_distance = std::hypot(horizontal_x, horizontal_z);
  if (horizontal_distance < 1.0) {
    return std::nullopt;
  }

  const auto vertical_distance =
      static_cast<double>(trajectory.target.y) - origin_y;
  const auto direct_angle = std::atan2(vertical_distance, horizontal_distance);
  const auto charge = std::clamp(
      static_cast<double>(trajectory.strength_q12) / 4096.0, 0.0, 1.0);
  const auto launch_angle =
      std::lerp(direct_angle, std::numbers::pi * 0.5, charge);
  const auto cosine = std::cos(launch_angle);
  const auto tangent = std::tan(launch_angle);
  constexpr auto gravity = 10518.0 / 4096.0;
  const auto denominator =
      2.0 * cosine * cosine *
      (horizontal_distance * tangent - vertical_distance);
  if (!std::isfinite(denominator) || denominator <= 0.001) {
    return std::nullopt;
  }
  const auto speed_squared =
      gravity * horizontal_distance * horizontal_distance / denominator;
  if (!std::isfinite(speed_squared) || speed_squared <= 0.0) {
    return std::nullopt;
  }

  const auto direction_x = horizontal_x / horizontal_distance;
  const auto direction_z = horizontal_z / horizontal_distance;
  const auto from_origin_x =
      static_cast<double>(retail_position.x) - origin_x;
  const auto from_origin_z =
      static_cast<double>(retail_position.z) - origin_z;
  const auto travelled = std::clamp(from_origin_x * direction_x +
                                        from_origin_z * direction_z,
                                    0.0, horizontal_distance);
  const auto ballistic_y =
      origin_y + travelled * tangent -
      gravity * travelled * travelled /
          (2.0 * speed_squared * cosine * cosine);
  if (!std::isfinite(ballistic_y)) {
    return std::nullopt;
  }
  // Mission rendering is down-positive while the retail solver is
  // up-positive. Horizontal motion remains the exact guest collision path.
  return GrenadePresentationPoint{static_cast<double>(retail_position.x),
                                  -ballistic_y,
                                  static_cast<double>(retail_position.z)};
}

double fallbackGrenadePresentationY(const LegacyNativePoint &retail_position,
                                    std::uint8_t age) noexcept {
  constexpr auto retail_lifetime = 60.0;
  constexpr auto apex_height = 420.0;
  const auto time =
      std::clamp(static_cast<double>(age) / retail_lifetime, 0.0, 1.0);
  const auto lift = 4.0 * apex_height * time * (1.0 - time);
  return -static_cast<double>(retail_position.y) - lift;
}

std::uint64_t
legacyGuestIdentity(const LegacyObjectBridgeState &guest) noexcept {
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

bool legacyRetiredDynamicObject(const LegacyObjectBridgeState &guest) noexcept {
  // FUN_80065fa0 retires a recycled slot by clearing record+0x2c only;
  // every other field may retain the previous lifetime until repopulation.
  return guest.path_pointer == 0U;
}

constexpr std::int16_t legacyDamageReaction(WeaponDamageKind kind) noexcept {
  // Retail health event 0x0e takes the weapon-table reaction/death code,
  // not the native damage-kind enum. These are the exact category values
  // used by the USA v1.1 weapon records at 0x8010c396.
  switch (kind) {
  case WeaponDamageKind::ballistic:
    return 0x0f;
  case WeaponDamageKind::pellet:
  case WeaponDamageKind::explosive:
    return 0x11;
  case WeaponDamageKind::electrical:
  case WeaponDamageKind::fire:
    return 0x10;
  case WeaponDamageKind::gas:
    return 0x12;
  case WeaponDamageKind::none:
    return 0;
  }
  return 0;
}

bool openingEncounterHostileSlot(const NpcState &state) noexcept {
  return !state.scripted_intro_agent &&
         state.disposition == NpcDisposition::hostile &&
         state.scripted_opening_lane < opening_encounter_lanes.size();
}

bool openingEncounterHostile(const NpcState &state) noexcept {
  return state.scripted_opening_combat && openingEncounterHostileSlot(state);
}

void setLegacyBridgedBehavior(NpcState &state, NpcBehavior behavior) noexcept {
  if (state.behavior == behavior) {
    return;
  }
  state.behavior = behavior;
  state.state_updates = 0U;
  state.animation_tick = 0U;
}

void setLegacyBridgedLocomotion(NpcState &state,
                                NpcLocomotion locomotion) noexcept {
  if (state.locomotion == locomotion) {
    return;
  }
  state.locomotion = locomotion;
  state.locomotion_animation_tick = 0U;
}

void resetLegacyBridgedPresentation(NpcState &state) noexcept {
  state.behavior = NpcBehavior::idle;
  state.locomotion = NpcLocomotion::stationary;
  state.combat_phase = NpcCombatPhase::acquire;
  state.movement_distance = 0.0;
  state.fire_animation_updates = 0U;
  state.scripted_ingress = false;
  state.scripted_climbing = false;
  state.scripted_kneeling = false;
  state.scripted_low_locomotion = false;
  state.legacy_presentation_valid = false;
  state.legacy_presentation_code = 0U;
  state.legacy_presentation_mode = 0U;
  state.locomotion_animation_tick = 0U;
  state.animation_tick = 0U;
}

void syncLegacyHmdBones(SceneObject &object,
                        const LegacyObjectBridgeState &guest,
                        bool contact_space_fallback,
                        bool force_native_pose_fallback = false) noexcept {
  if (guest.bone_matrix_count == 0U || force_native_pose_fallback) {
    object.legacy_hmd_bone_count = 0U;
    object.legacy_hmd_root_space = !contact_space_fallback;
    return;
  }
  const auto count = std::min<std::size_t>(guest.bone_matrix_count,
                                           object.legacy_hmd_bones.size());
  for (std::size_t part = 0U; part < count; ++part) {
    const auto &source = guest.bone_matrices[part];
    auto &target = object.legacy_hmd_bones[part];
    target.rotation = source.rotation;
    target.x = source.translation.x;
    target.y = -source.translation.y;
    target.z = source.translation.z;
  }
  object.legacy_hmd_bone_count = static_cast<std::uint8_t>(count);
  object.legacy_hmd_root_space = false;
}

void syncLegacyHmdLighting(SceneObject &object,
                           const LegacyObjectBridgeState &guest) noexcept {
  object.legacy_hmd_back_color_q12 = guest.hmd_back_color_q12;
  object.legacy_hmd_back_color_valid = guest.hmd_back_color_valid;
}

void syncLegacyGroundContact(NpcState &state,
                             const LegacyObjectBridgeState &guest) noexcept {
  state.legacy_ground_contact_valid = guest.ground_contact_valid;
  if (!guest.ground_contact_valid) {
    return;
  }
  state.legacy_ground_y = static_cast<double>(guest.ground_contact_y);
}

NpcBehavior legacyBridgedBehavior(const LegacyObjectBridgeState &guest,
                                  bool moving) noexcept {
  if (guest.health <= 0) {
    return guest.health < 0 ? NpcBehavior::dead : NpcBehavior::dying;
  }
  if (moving && (!guest.has_target || guest.ai_fire_latch == 0U)) {
    return NpcBehavior::pursue;
  }
  return guest.has_target ? NpcBehavior::attack
         : moving         ? NpcBehavior::pursue
                          : NpcBehavior::idle;
}

bool solidMissionObject(std::uint32_t class_id) noexcept {
  switch (class_id) {
  case 0x11U: // gas pipe
  case 0x2cU: // police cars
  case 0x2fU: // pipe sections
  case 0x49U: // locked street gate
  case 0x4fU: // weapon crate
  case 0x50U: // weapon crate
  case 0x54U: // security gate
    return true;
  default:
    return false;
  }
}

std::int32_t signedHeadingDelta(std::int32_t from, std::int32_t to) noexcept {
  auto delta = normalizeHeading(static_cast<std::int64_t>(to) - from);
  if (delta > heading_angle_units / 2) {
    delta -= heading_angle_units;
  }
  return delta;
}

std::int16_t fixedRotation(double value) noexcept {
  return static_cast<std::int16_t>(
      std::clamp(std::lround(value * 4096.0),
                 static_cast<long>(std::numeric_limits<std::int16_t>::min()),
                 static_cast<long>(std::numeric_limits<std::int16_t>::max())));
}

std::array<std::int16_t, 9U> headingRotation(std::int32_t heading) noexcept {
  const auto basis = headingBasis(heading);
  return {
      fixedRotation(basis.right.x),
      0,
      fixedRotation(basis.forward.x),
      0,
      4096,
      0,
      fixedRotation(basis.right.z),
      0,
      fixedRotation(basis.forward.z),
  };
}

std::array<std::int16_t, 9U>
tiltedVehicleRotation(const std::array<std::int16_t, 9U> &base, double pitch,
                      double roll) noexcept {
  const auto cp = std::cos(pitch);
  const auto sp = std::sin(pitch);
  const auto cr = std::cos(roll);
  const auto sr = std::sin(roll);
  const std::array<double, 9U> local{
      cr, -sr * cp, sr * sp, sr, cr * cp, -cr * sp, 0.0, sp, cp,
  };
  std::array<std::int16_t, 9U> result{};
  for (std::size_t row = 0U; row < 3U; ++row) {
    for (std::size_t column = 0U; column < 3U; ++column) {
      auto value = 0.0;
      for (std::size_t component = 0U; component < 3U; ++component) {
        value += static_cast<double>(base[row * 3U + component]) / 4096.0 *
                 local[component * 3U + column];
      }
      result[row * 3U + column] = fixedRotation(value);
    }
  }
  return result;
}

assets::EmdBounds findBounds(const assets::EmdScene &scene) {
  int minimum_x = std::numeric_limits<int>::max();
  int minimum_y = std::numeric_limits<int>::max();
  int minimum_z = std::numeric_limits<int>::max();
  int maximum_x = std::numeric_limits<int>::min();
  int maximum_y = std::numeric_limits<int>::min();
  int maximum_z = std::numeric_limits<int>::min();
  for (const auto &section : scene.sections()) {
    for (const auto &vertex : section.vertices) {
      minimum_x = std::min(minimum_x, static_cast<int>(vertex.x));
      minimum_y = std::min(minimum_y, static_cast<int>(vertex.y));
      minimum_z = std::min(minimum_z, static_cast<int>(vertex.z));
      maximum_x = std::max(maximum_x, static_cast<int>(vertex.x));
      maximum_y = std::max(maximum_y, static_cast<int>(vertex.y));
      maximum_z = std::max(maximum_z, static_cast<int>(vertex.z));
    }
  }
  if (minimum_x == std::numeric_limits<int>::max()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "World model has no vertices"};
  }
  return assets::EmdBounds{
      static_cast<std::int16_t>(minimum_x),
      static_cast<std::int16_t>(minimum_y),
      static_cast<std::int16_t>(minimum_z),
      static_cast<std::int16_t>(maximum_x),
      static_cast<std::int16_t>(maximum_y),
      static_cast<std::int16_t>(maximum_z),
  };
}

bool containsXZ(const assets::EmdBounds &bounds, double x, double z) {
  return x >= bounds.minimum_x && x <= bounds.maximum_x &&
         z >= bounds.minimum_z && z <= bounds.maximum_z;
}

std::string modelStem(std::string_view name) {
  const auto extension = name.find_last_of('.');
  std::string result{name.substr(0, extension)};
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return result;
}

constexpr std::uint32_t police_lightbar_class = 0x34U;
constexpr unsigned int explosion_frame_count = 12U;
constexpr unsigned int fire_frame_count = 16U;
constexpr unsigned int breath_frame_count = 16U;
constexpr unsigned int vapor_frame_count = 8U;

ObjectFireEmitter makeFireEmitter(const assets::GmdModel &emitter,
                                  const assets::HogArchive &special_effects) {
  if (emitter.vertices().size() != 8U) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "CFIRE emitter volume is invalid"};
  }
  const auto frame_name = [](std::string_view prefix, unsigned int frame,
                             std::size_t digits) {
    auto number = std::to_string(frame);
    std::string result{prefix};
    result.append(digits - number.size(), '0');
    result += number;
    result += ".TIM";
    return result;
  };
  std::vector<assets::TimImage> explosion_frames;
  explosion_frames.reserve(explosion_frame_count);
  for (unsigned int frame = 0; frame < explosion_frame_count; ++frame) {
    auto image = assets::TimImage::parse(
        special_effects.file(frame_name("EXPL", frame, 3U)));
    if (image.mode() != assets::TimPixelMode::indexed8 ||
        image.displayWidth() != 32U || image.displayHeight() != 32U ||
        !image.clut()) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "CFIRE EXPL frame is not a paletted 32x32 TIM"};
    }
    explosion_frames.push_back(std::move(image));
  }
  std::vector<assets::TimImage> fire_frames;
  fire_frames.reserve(fire_frame_count);
  for (unsigned int frame = 0; frame < fire_frame_count; ++frame) {
    auto image = assets::TimImage::parse(
        special_effects.file(frame_name("FIRE", frame, 4U)));
    if (image.mode() != assets::TimPixelMode::indexed8 ||
        image.displayWidth() != 32U || image.displayHeight() != 64U ||
        !image.clut() ||
        image.clut()->words != explosion_frames.front().clut()->words) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "FIRE frame is not a paletted 32x64 SPFX TIM"};
    }
    fire_frames.push_back(std::move(image));
  }
  const auto load_particle_family = [&special_effects, &frame_name](
                                        std::string_view prefix,
                                        unsigned int frame_count,
                                        std::size_t digits, std::uint32_t width,
                                        std::uint32_t height,
                                        const assets::TimImage &palette) {
    std::vector<assets::TimImage> frames;
    frames.reserve(frame_count);
    for (unsigned int frame = 0; frame < frame_count; ++frame) {
      auto image = assets::TimImage::parse(
          special_effects.file(frame_name(prefix, frame, digits)));
      if (image.mode() != assets::TimPixelMode::indexed8 ||
          image.displayWidth() != width || image.displayHeight() != height ||
          !image.clut() || image.clut()->words != palette.clut()->words) {
        throw core::Error{core::ErrorCode::invalid_format,
                          std::string{prefix} +
                              " frame does not match its retail SPFX family"};
      }
      frames.push_back(std::move(image));
    }
    return frames;
  };
  auto breath_frames = load_particle_family("BRETH", breath_frame_count, 2U,
                                            16U, 16U, explosion_frames.front());
  auto vapor_frames = load_particle_family("VAPOR", vapor_frame_count, 3U, 32U,
                                           32U, explosion_frames.front());
  return ObjectFireEmitter{std::move(explosion_frames), std::move(fire_frames),
                           std::move(breath_frames), std::move(vapor_frames)};
}

struct Point3 {
  double x;
  double y;
  double z;
};

Point3 point(const assets::EmdVertex &vertex) {
  return Point3{
      static_cast<double>(vertex.x),
      static_cast<double>(vertex.y),
      static_cast<double>(vertex.z),
  };
}

bool segmentOverlapsBounds(const Point3 &from, const Point3 &to,
                           const assets::EmdBounds &bounds) noexcept {
  constexpr double tolerance = 0.001;
  return std::max(from.x, to.x) + tolerance >= bounds.minimum_x &&
         std::min(from.x, to.x) - tolerance <= bounds.maximum_x &&
         std::max(from.y, to.y) + tolerance >= bounds.minimum_y &&
         std::min(from.y, to.y) - tolerance <= bounds.maximum_y &&
         std::max(from.z, to.z) + tolerance >= bounds.minimum_z &&
         std::min(from.z, to.z) - tolerance <= bounds.maximum_z;
}

std::optional<double>
segmentTriangleIntersection(const Point3 &from, const Point3 &to,
                            const Point3 &first, const Point3 &second,
                            const Point3 &third) noexcept {
  const Point3 direction{to.x - from.x, to.y - from.y, to.z - from.z};
  const Point3 edge1{
      second.x - first.x,
      second.y - first.y,
      second.z - first.z,
  };
  const Point3 edge2{
      third.x - first.x,
      third.y - first.y,
      third.z - first.z,
  };
  const Point3 perpendicular{
      direction.y * edge2.z - direction.z * edge2.y,
      direction.z * edge2.x - direction.x * edge2.z,
      direction.x * edge2.y - direction.y * edge2.x,
  };
  const auto determinant = edge1.x * perpendicular.x +
                           edge1.y * perpendicular.y +
                           edge1.z * perpendicular.z;
  constexpr double parallel_epsilon = 0.000000001;
  if (std::abs(determinant) <= parallel_epsilon) {
    return std::nullopt;
  }

  const auto inverse_determinant = 1.0 / determinant;
  const Point3 from_first{
      from.x - first.x,
      from.y - first.y,
      from.z - first.z,
  };
  const auto u =
      (from_first.x * perpendicular.x + from_first.y * perpendicular.y +
       from_first.z * perpendicular.z) *
      inverse_determinant;
  constexpr double barycentric_tolerance = 0.000001;
  if (u < -barycentric_tolerance || u > 1.0 + barycentric_tolerance) {
    return std::nullopt;
  }

  const Point3 cross{
      from_first.y * edge1.z - from_first.z * edge1.y,
      from_first.z * edge1.x - from_first.x * edge1.z,
      from_first.x * edge1.y - from_first.y * edge1.x,
  };
  const auto v =
      (direction.x * cross.x + direction.y * cross.y + direction.z * cross.z) *
      inverse_determinant;
  if (v < -barycentric_tolerance || u + v > 1.0 + barycentric_tolerance) {
    return std::nullopt;
  }

  const auto amount =
      (edge2.x * cross.x + edge2.y * cross.y + edge2.z * cross.z) *
      inverse_determinant;
  if (amount <= minimum_world_segment_hit || amount > 1.0) {
    return std::nullopt;
  }
  return amount;
}

bool triangleHeight(const Point3 &first, const Point3 &second,
                    const Point3 &third, double x, double z, double &height) {
  const auto edge1_x = second.x - first.x;
  const auto edge1_y = second.y - first.y;
  const auto edge1_z = second.z - first.z;
  const auto edge2_x = third.x - first.x;
  const auto edge2_y = third.y - first.y;
  const auto edge2_z = third.z - first.z;
  const auto normal_x = edge1_y * edge2_z - edge1_z * edge2_y;
  const auto normal_y = edge1_z * edge2_x - edge1_x * edge2_z;
  const auto normal_z = edge1_x * edge2_y - edge1_y * edge2_x;
  const auto normal_length = std::sqrt(
      normal_x * normal_x + normal_y * normal_y + normal_z * normal_z);
  if (normal_length <= 0.0001 ||
      normal_y / normal_length < minimum_floor_normal) {
    return false;
  }

  const auto denominator = (second.z - third.z) * (first.x - third.x) +
                           (third.x - second.x) * (first.z - third.z);
  if (std::abs(denominator) <= 0.0001) {
    return false;
  }
  const auto first_weight = ((second.z - third.z) * (x - third.x) +
                             (third.x - second.x) * (z - third.z)) /
                            denominator;
  const auto second_weight = ((third.z - first.z) * (x - third.x) +
                              (first.x - third.x) * (z - third.z)) /
                             denominator;
  const auto third_weight = 1.0 - first_weight - second_weight;
  constexpr double edge_tolerance = -0.001;
  if (first_weight < edge_tolerance || second_weight < edge_tolerance ||
      third_weight < edge_tolerance) {
    return false;
  }
  height = first_weight * first.y + second_weight * second.y +
           third_weight * third.y;
  return true;
}

double squaredDistanceToSegment(double x, double z, const Point3 &first,
                                const Point3 &second) {
  const auto delta_x = second.x - first.x;
  const auto delta_z = second.z - first.z;
  const auto length_squared = delta_x * delta_x + delta_z * delta_z;
  if (length_squared <= 0.0001) {
    const auto point_x = x - first.x;
    const auto point_z = z - first.z;
    return point_x * point_x + point_z * point_z;
  }
  const auto amount = std::clamp(
      ((x - first.x) * delta_x + (z - first.z) * delta_z) / length_squared, 0.0,
      1.0);
  const auto nearest_x = first.x + amount * delta_x;
  const auto nearest_z = first.z + amount * delta_z;
  const auto point_x = x - nearest_x;
  const auto point_z = z - nearest_z;
  return point_x * point_x + point_z * point_z;
}

Point3
transformObjectPoint(double x, double y, double z,
                     const assets::MissionTransform &transform) noexcept {
  const auto component = [&](std::size_t row) {
    return (static_cast<double>(transform.rotation[row * 3U]) * x +
            static_cast<double>(transform.rotation[row * 3U + 1U]) * y +
            static_cast<double>(transform.rotation[row * 3U + 2U]) * z) /
           4096.0;
  };
  return Point3{
      static_cast<double>(transform.x) + component(0U),
      -static_cast<double>(transform.y) + component(1U),
      static_cast<double>(transform.z) + component(2U),
  };
}

assets::MissionTransform
legacyObjectTransform(const LegacyObjectBridgeState &object) noexcept {
  return assets::MissionTransform{
      object.guest_rotation,
      object.position.x,
      -object.position.y,
      object.position.z,
  };
}

std::optional<double>
movingPlatformSurfaceY(double x, double z, const assets::EmdBounds &bounds,
                       const assets::MissionTransform &transform) noexcept {
  const auto origin = transformObjectPoint(0.0, 0.0, 0.0, transform);
  const auto delta_x = x - origin.x;
  const auto delta_z = z - origin.z;
  const auto right_x = static_cast<double>(transform.rotation[0]) / 4096.0;
  const auto right_z = static_cast<double>(transform.rotation[6]) / 4096.0;
  const auto forward_x = static_cast<double>(transform.rotation[2]) / 4096.0;
  const auto forward_z = static_cast<double>(transform.rotation[8]) / 4096.0;
  const auto local_x = delta_x * right_x + delta_z * right_z;
  const auto local_z = delta_x * forward_x + delta_z * forward_z;
  if (local_x < static_cast<double>(bounds.minimum_x) ||
      local_x > static_cast<double>(bounds.maximum_x) ||
      local_z < static_cast<double>(bounds.minimum_z) ||
      local_z > static_cast<double>(bounds.maximum_z)) {
    return std::nullopt;
  }
  // Native Y grows downward, so the smallest local Y is the walkable top.
  return transformObjectPoint(0.0, static_cast<double>(bounds.minimum_y), 0.0,
                              transform)
      .y;
}

std::optional<Point3>
movingPlatformRiderDelta(const PlayerState &rider,
                         const assets::EmdBounds &bounds,
                         const assets::MissionTransform &previous,
                         const assets::MissionTransform &current) noexcept {
  if (!rider.grounded) {
    return std::nullopt;
  }
  const auto surface =
      movingPlatformSurfaceY(rider.x, rider.z, bounds, previous);
  if (!surface || std::abs(rider.y - *surface) > maximum_ground_step) {
    return std::nullopt;
  }
  const auto before = transformObjectPoint(0.0, 0.0, 0.0, previous);
  const auto after = transformObjectPoint(0.0, 0.0, 0.0, current);
  const Point3 delta{after.x - before.x, after.y - before.y,
                     after.z - before.z};
  if (delta.x == 0.0 && delta.y == 0.0 && delta.z == 0.0) {
    return std::nullopt;
  }
  return delta;
}

std::optional<double>
rayBoundsIntersection(const ActorAimRay &ray,
                      const assets::EmdBounds &local_bounds,
                      const assets::MissionTransform &transform,
                      double maximum_distance) noexcept {
  auto minimum = Point3{
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(),
  };
  auto maximum = Point3{
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest(),
  };
  for (unsigned int corner = 0U; corner < 8U; ++corner) {
    const auto point = transformObjectPoint(
        (corner & 1U) != 0U ? local_bounds.maximum_x : local_bounds.minimum_x,
        (corner & 2U) != 0U ? local_bounds.maximum_y : local_bounds.minimum_y,
        (corner & 4U) != 0U ? local_bounds.maximum_z : local_bounds.minimum_z,
        transform);
    minimum.x = std::min(minimum.x, point.x);
    minimum.y = std::min(minimum.y, point.y);
    minimum.z = std::min(minimum.z, point.z);
    maximum.x = std::max(maximum.x, point.x);
    maximum.y = std::max(maximum.y, point.y);
    maximum.z = std::max(maximum.z, point.z);
  }

  // Windows and lamp sprites are genuinely planar in the source GMD. Give
  // their shot volume the same small tolerance used by the PS1 object EC.
  constexpr double minimum_thickness = 24.0;
  const auto expand = [](double &first, double &second) {
    if (second - first < minimum_thickness) {
      const auto centre = (first + second) * 0.5;
      first = centre - minimum_thickness * 0.5;
      second = centre + minimum_thickness * 0.5;
    }
  };
  expand(minimum.x, maximum.x);
  expand(minimum.y, maximum.y);
  expand(minimum.z, maximum.z);

  const auto direction_length = std::sqrt(ray.direction_x * ray.direction_x +
                                          ray.direction_y * ray.direction_y +
                                          ray.direction_z * ray.direction_z);
  if (direction_length <= 0.0001) {
    return std::nullopt;
  }
  const std::array origin{ray.origin_x, ray.origin_y, ray.origin_z};
  const std::array direction{
      ray.direction_x / direction_length,
      ray.direction_y / direction_length,
      ray.direction_z / direction_length,
  };
  const std::array lower{minimum.x, minimum.y, minimum.z};
  const std::array upper{maximum.x, maximum.y, maximum.z};
  auto entry = 0.0;
  auto exit = maximum_distance;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    if (std::abs(direction[axis]) <= 0.0000001) {
      if (origin[axis] < lower[axis] || origin[axis] > upper[axis]) {
        return std::nullopt;
      }
      continue;
    }
    auto first = (lower[axis] - origin[axis]) / direction[axis];
    auto second = (upper[axis] - origin[axis]) / direction[axis];
    if (first > second) {
      std::swap(first, second);
    }
    entry = std::max(entry, first);
    exit = std::min(exit, second);
    if (entry > exit) {
      return std::nullopt;
    }
  }
  return entry > 0.0 && entry <= maximum_distance ? std::optional<double>{entry}
                                                  : std::nullopt;
}

std::optional<std::int32_t>
retailWeaponStepsToTarget(const PlayerInventory &source, WeaponId target,
                          std::int8_t preferred_direction) noexcept {
  if (source.current() == target) {
    return std::int32_t{};
  }
  const auto distance = [&source, target](bool next) {
    auto inventory = source;
    for (std::size_t step = 1U; step <= legacy_inventory_weapon_count; ++step) {
      const auto changed =
          next ? inventory.selectNext() : inventory.selectPrevious();
      if (!changed) {
        break;
      }
      if (inventory.current() == target) {
        return std::optional<std::size_t>{step};
      }
    }
    return std::optional<std::size_t>{};
  };
  const auto next_distance = distance(true);
  const auto previous_distance = distance(false);
  if (!next_distance && !previous_distance) {
    return std::nullopt;
  }
  if (!next_distance) {
    return -static_cast<std::int32_t>(*previous_distance);
  }
  if (!previous_distance) {
    return static_cast<std::int32_t>(*next_distance);
  }
  if (*next_distance == *previous_distance) {
    const auto tie_distance = static_cast<std::int32_t>(*next_distance);
    return preferred_direction < 0 ? -tie_distance : tie_distance;
  }
  return *next_distance < *previous_distance
             ? static_cast<std::int32_t>(*next_distance)
             : -static_cast<std::int32_t>(*previous_distance);
}

} // namespace

std::optional<ActorAimHit> actorAimHit(const ActorAimRay &ray, double actor_x,
                                       double actor_y,
                                       double actor_z) noexcept {
  const auto direction_length = std::sqrt(ray.direction_x * ray.direction_x +
                                          ray.direction_y * ray.direction_y +
                                          ray.direction_z * ray.direction_z);
  if (direction_length <= 0.0001) {
    return std::nullopt;
  }
  const auto direction_x = ray.direction_x / direction_length;
  const auto direction_y = ray.direction_y / direction_length;
  const auto direction_z = ray.direction_z / direction_length;
  const auto intersect_sphere =
      [&](ActorAimZone zone, double height,
          double radius) -> std::optional<ActorAimHit> {
    const auto target_y = actor_y - height;
    const auto delta_x = actor_x - ray.origin_x;
    const auto delta_y = target_y - ray.origin_y;
    const auto delta_z = actor_z - ray.origin_z;
    const auto ray_distance =
        delta_x * direction_x + delta_y * direction_y + delta_z * direction_z;
    if (ray_distance <= 0.0) {
      return std::nullopt;
    }
    const auto distance_squared =
        delta_x * delta_x + delta_y * delta_y + delta_z * delta_z;
    const auto perpendicular_squared =
        std::max(0.0, distance_squared - ray_distance * ray_distance);
    if (perpendicular_squared > radius * radius) {
      return std::nullopt;
    }
    return ActorAimHit{zone, ray_distance, actor_x, target_y, actor_z};
  };

  // A head ray may also graze the large body sphere; the contextual head
  // zone deliberately wins, matching the original HEAD SHOT prompt.
  if (const auto head = intersect_sphere(ActorAimZone::head, actor_head_height,
                                         actor_head_radius)) {
    return head;
  }
  return intersect_sphere(ActorAimZone::body, actor_target_height,
                          actor_target_radius);
}

GameplaySession::GameplaySession(const MissionPackage &mission,
                                 LoadProgressCallback load_progress)
    : mission_(mission) {
  const auto report_load = [&](std::uint8_t percent) {
    if (load_progress) {
      load_progress(percent);
    }
  };
  report_load(2U);
  const auto &archive = mission.worldModels();
  models_.reserve(archive.entries().size());
  for (const auto &entry : archive.entries()) {
    auto scene = assets::EmdScene::parse(archive.file(entry.name));
    auto bounds = findBounds(scene);
    models_.push_back(WorldModel{entry.name, std::move(scene), bounds});
  }
  report_load(24U);

  struct ObjectResources {
    const assets::HogEntry *gmd{};
    const assets::HogEntry *emd{};
    const assets::HogEntry *hmd{};
  };
  std::unordered_map<std::string, ObjectResources> object_entries;
  for (const auto &entry : mission.objectModels().entries()) {
    if (entry.name.size() <= 4U) {
      continue;
    }
    const auto extension = entry.name.substr(entry.name.size() - 4U);
    if (extension != ".GMD" && extension != ".EMD" && extension != ".HMD") {
      continue;
    }
    auto &resources = object_entries[modelStem(entry.name)];
    if (extension == ".GMD") {
      resources.gmd = &entry;
    } else if (extension == ".EMD") {
      resources.emd = &entry;
    } else {
      resources.hmd = &entry;
    }
  }
  if (const auto scrim = std::ranges::find(mission.objectModels().entries(),
                                           std::string_view{"SCRIM.EMD"},
                                           &assets::HogEntry::name);
      scrim != mission.objectModels().entries().end()) {
    detached_scrim_ =
        assets::EmdScene::parse(mission.objectModels().file(scrim->name));
  }
  std::unordered_map<std::string, std::uint16_t> loaded_models;
  const auto load_model = [&](const std::string &stem,
                              const assets::HogEntry &resource,
                              std::uint32_t class_id) {
    const auto fire_presentation =
        legacyFireEmitterPresentation(class_id, stem);
    const auto model_key = fire_presentation
                               ? std::string{"SPFX:CFIRE"}
                               : resource.name + ":" + std::to_string(class_id);
    if (const auto loaded = loaded_models.find(model_key);
        loaded != loaded_models.end()) {
      return loaded->second;
    }
    if (object_models_.size() >= std::numeric_limits<std::uint16_t>::max()) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Too many mission object models"};
    }
    const auto resource_bytes = mission.objectModels().file(resource.name);
    auto geometry = [&]() -> ObjectGeometry {
      if (resource.name.ends_with(".HMD")) {
        return assets::HmdModel::parse(resource_bytes);
      }
      if (resource.name.ends_with(".EMD")) {
        return assets::EmdScene::parse(resource_bytes);
      }
      auto gmd = assets::GmdModel::parse(resource_bytes);
      if (!fire_presentation) {
        return gmd;
      }
      return makeFireEmitter(gmd, mission.specialEffects());
    }();
    const auto bounds = std::visit(
        [](const auto &model) -> std::optional<assets::EmdBounds> {
          using Model = std::decay_t<decltype(model)>;
          if constexpr (std::is_same_v<Model, assets::GmdModel>) {
            return model.bounds();
          } else if constexpr (std::is_same_v<Model, assets::EmdScene>) {
            return findBounds(model);
          } else {
            return std::nullopt;
          }
        },
        geometry);
    const auto model_index = static_cast<std::uint16_t>(object_models_.size());
    const auto visual_effect = [&] {
      if (class_id == police_lightbar_class && stem == "LIGHT") {
        return ObjectVisualEffect::police_lightbar;
      }
      // GLIT.GMD is the original lamp-brightness/halo sprite.  Its
      // authored planar mesh supplies size and UVs, but gameplay turns
      // it toward the viewer instead of leaving it in object space.
      if (stem == "GLIT") {
        return ObjectVisualEffect::billboard_glow;
      }
      return ObjectVisualEffect::none;
    }();
    object_models_.push_back(ObjectModel{
        resource.name,
        visual_effect,
        std::move(geometry),
        bounds,
    });
    loaded_models.emplace(model_key, model_index);
    return model_index;
  };

  const auto gabe = object_entries.find("GABE");
  if (gabe == object_entries.end() || gabe->second.hmd == nullptr) {
    throw core::Error{core::ErrorCode::not_found, "GABE.HMD is missing"};
  }
  player_model_ = load_model("GABE", *gabe->second.hmd, 0U);
  const auto player_part_count =
      std::get<assets::HmdModel>(object_models_[player_model_].geometry)
          .parts()
          .size();
  const auto load_root_motion = [&](std::string_view name) {
    const auto clip = assets::HmdAnimationClip::parse(
        mission.characterAnimations().file(name), player_part_count);
    if (clip.rootMotion().empty()) {
      throw core::Error{
          core::ErrorCode::invalid_format,
          "Player locomotion animation has no root-motion track: " +
              std::string{name},
      };
    }
    return std::vector<assets::HmdRootMotionFrame>{clip.rootMotion().begin(),
                                                   clip.rootMotion().end()};
  };
  const auto load_optional_root_motion = [&](std::string_view name) {
    const auto clip = assets::HmdAnimationClip::parse(
        mission.characterAnimations().file(name), player_part_count);
    return std::vector<assets::HmdRootMotionFrame>{clip.rootMotion().begin(),
                                                   clip.rootMotion().end()};
  };
  const auto walking_root_motion = load_root_motion("WK0.LWR");
  const auto running_root_motion = load_root_motion("RN0.LWR");
  const auto crouch_root_motion = load_optional_root_motion("CW0.LWR");
  const auto strafe_left_root_motion = load_optional_root_motion("STEPL0.LWR");
  const auto strafe_right_root_motion = load_optional_root_motion("STEPR0.LWR");
  const auto rolling_root_motion = load_optional_root_motion("STROL0.HAN");
  const auto standing_long_gun_roll_root_motion =
      load_optional_root_motion("STROL2.HAN");
  const auto kneeling_unarmed_roll_root_motion =
      load_optional_root_motion("KNROL0.HAN");
  const auto kneeling_sidearm_roll_root_motion =
      load_optional_root_motion("KNROL1.HAN");
  const auto kneeling_long_gun_roll_root_motion =
      load_optional_root_motion("KNROL2.HAN");
  player_controller_.setRootMotionTracks(
      walking_root_motion, running_root_motion, rolling_root_motion,
      crouch_root_motion);
  player_controller_.setStrafeRootMotionTracks(strafe_left_root_motion,
                                               strafe_right_root_motion);
  player_controller_.setAdditionalRollRootMotionTracks(
      standing_long_gun_roll_root_motion, kneeling_unarmed_roll_root_motion,
      kneeling_sidearm_roll_root_motion, kneeling_long_gun_roll_root_motion);
  report_load(42U);

  const auto &mission_objects = mission.objects();
  constexpr auto invalid_scene = std::numeric_limits<std::uint16_t>::max();
  legacy_object_definition_templates_.resize(
      mission_objects.definitions().size());
  const auto definition_resource =
      [&](const std::string &name) -> const assets::HogEntry * {
    const auto resources = object_entries.find(modelStem(name));
    if (resources == object_entries.end()) {
      return nullptr;
    }
    switch (legacyPresentationResourceKind(
        name, resources->second.gmd != nullptr,
        resources->second.emd != nullptr, resources->second.hmd != nullptr)) {
    case LegacyPresentationResourceKind::gmd:
      return resources->second.gmd;
    case LegacyPresentationResourceKind::emd:
      return resources->second.emd;
    case LegacyPresentationResourceKind::hmd:
      return resources->second.hmd;
    case LegacyPresentationResourceKind::none:
      return nullptr;
    }
    return nullptr;
  };
  source_to_scene_object_.assign(mission_objects.objects().size(),
                                 invalid_scene);
  for (std::size_t index = 0; index < mission_objects.objects().size();
       ++index) {
    if (index == mission_objects.playerIndex()) {
      continue;
    }
    const auto &object = mission_objects.objects()[index];
    const auto &definition = mission_objects.definition(object.type);
    if (definition.primary_model.empty()) {
      continue;
    }
    const auto stem = modelStem(definition.primary_model);
    const auto *resource = definition_resource(definition.primary_model);
    if (resource == nullptr) {
      continue;
    }
    const auto model_index = load_model(stem, *resource, definition.class_id);
    std::optional<std::uint16_t> destroyed_model;
    if (!definition.secondary_model.empty()) {
      const auto destroyed_stem = modelStem(definition.secondary_model);
      const auto *destroyed_resource =
          definition_resource(definition.secondary_model);
      if (destroyed_resource != nullptr) {
        destroyed_model = load_model(destroyed_stem, *destroyed_resource,
                                     definition.class_id);
      }
    }
    if (objects_.size() >= std::numeric_limits<std::uint16_t>::max()) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Too many mission objects"};
    }
    source_to_scene_object_[index] =
        static_cast<std::uint16_t>(objects_.size());
    objects_.push_back(SceneObject{
        model_index,
        object.transform,
        definition.class_id,
        static_cast<std::uint16_t>(index),
        destroyed_model,
        objectDamageResponse(definition.class_id, definition.primary_model),
        object.type,
    });
    auto &presentation_template =
        legacy_object_definition_templates_[object.type];
    if (!presentation_template) {
      presentation_template = objects_.back();
    }
  }

  // Recycled records can reference a BIN definition with no authored static
  // instance. Materialize its exact models and class response now so runtime
  // binding never substitutes another actor/object merely by class.
  for (std::size_t definition_index = 0U;
       definition_index < mission_objects.definitions().size();
       ++definition_index) {
    auto &presentation_template =
        legacy_object_definition_templates_[definition_index];
    if (presentation_template) {
      continue;
    }
    const auto &definition = mission_objects.definitions()[definition_index];
    if (definition.primary_model.empty()) {
      continue;
    }
    const auto primary_stem = modelStem(definition.primary_model);
    const auto *primary_resource =
        definition_resource(definition.primary_model);
    if (primary_resource == nullptr) {
      continue;
    }
    const auto model_index =
        load_model(primary_stem, *primary_resource, definition.class_id);
    auto destroyed_model = std::optional<std::uint16_t>{};
    if (!definition.secondary_model.empty()) {
      const auto secondary_stem = modelStem(definition.secondary_model);
      const auto *secondary_resource =
          definition_resource(definition.secondary_model);
      if (secondary_resource != nullptr) {
        destroyed_model = load_model(secondary_stem, *secondary_resource,
                                     definition.class_id);
      }
    }
    auto transform = assets::MissionTransform{};
    transform.rotation = {
        4096, 0, 0, 0, 4096, 0, 0, 0, 4096,
    };
    presentation_template = SceneObject{
        model_index,
        transform,
        definition.class_id,
        invalid_scene,
        destroyed_model,
        objectDamageResponse(definition.class_id, definition.primary_model),
        static_cast<std::uint32_t>(definition_index),
    };
  }

  opening_cbdc_objects_.fill(invalid_scene);
  opening_terrorist_objects_.fill(invalid_scene);

  // The street CBDC and both wall attackers are SUBWAY-only presentation
  // objects. Other overlays may use the same source indices for unrelated
  // definitions, so they must never inherit these clones or lane bindings.
  const auto clone_opening_template = [&](std::uint16_t source_index) {
    if (source_index >= source_to_scene_object_.size()) {
      return invalid_scene;
    }
    const auto source_object = source_to_scene_object_[source_index];
    if (source_object >= objects_.size() ||
        objects_.size() >= std::numeric_limits<std::uint16_t>::max()) {
      return invalid_scene;
    }
    const auto clone = static_cast<std::uint16_t>(objects_.size());
    objects_.push_back(objects_[source_object]);
    return clone;
  };
  if (mission.definition().index == 0U) {
    opening_cbdc_objects_ = {
        clone_opening_template(opening_cbdc_source),
        clone_opening_template(opening_cbdc_source),
    };
    opening_terrorist_objects_ = {
        opening_terrorist_source < source_to_scene_object_.size()
            ? source_to_scene_object_[opening_terrorist_source]
            : invalid_scene,
        clone_opening_template(opening_terrorist_source),
    };
  }

  for (std::size_t index = 0; index < weapon_slot_count; ++index) {
    const auto weapon = static_cast<WeaponId>(index);
    const auto stem = weaponCombatDefinition(weapon).world_model;
    if (stem.empty()) {
      continue;
    }
    const auto resources = object_entries.find(std::string{stem});
    if (resources == object_entries.end() || resources->second.gmd == nullptr) {
      continue;
    }
    weapon_models_[index] =
        load_model(std::string{stem}, *resources->second.gmd, 0U);
  }

  // SPFX is a global retail particle atlas. Several overlays, including
  // PARK/PARK2, emit effects without placing a class-0x30 CFIRE object.
  // Keep the atlas available independently of that optional scene marker.
  if (std::ranges::none_of(object_models_, [](const ObjectModel &model) {
        return std::holds_alternative<ObjectFireEmitter>(model.geometry);
      })) {
    const auto cfire = object_entries.find("CFIREA");
    if (cfire == object_entries.end() || cfire->second.gmd == nullptr) {
      throw core::Error{core::ErrorCode::not_found,
                        "Canonical CFIREA.GMD is missing"};
    }
    static_cast<void>(
        load_model("CFIREA", *cfire->second.gmd, legacy_cfire_a_class));
  }
  report_load(63U);

  object_health_.resize(objects_.size());
  object_spawn_health_.resize(objects_.size());
  object_destroyed_.resize(objects_.size());
  object_script_hidden_.resize(objects_.size());
  object_spawn_script_hidden_.resize(objects_.size());
  npc_states_.resize(objects_.size());
  npc_spawn_states_.resize(objects_.size());
  npc_damaged_.resize(objects_.size());
  mission_scripts_.resize(objects_.size());
  for (std::size_t index = 0; index < objects_.size(); ++index) {
    const auto &scene_object = objects_[index];
    const auto &model = object_models_[scene_object.model];
    const auto &source = mission_objects.objects()[scene_object.source_index];
    if (scene_object.damage_response != ObjectDamageResponse::none) {
      const auto maximum_health = static_cast<std::uint16_t>(
          source.maximum_health > 0 ? source.maximum_health : 1);
      const auto health = static_cast<std::uint16_t>(
          std::clamp<int>(source.health > 0 ? source.health : maximum_health, 1,
                          maximum_health));
      object_health_[index] = health;
      object_spawn_health_[index] = health;
    }
    if (!std::holds_alternative<assets::HmdModel>(model.geometry)) {
      continue;
    }
    const auto maximum_health = static_cast<std::uint16_t>(
        source.maximum_health > 0 ? source.maximum_health : 100);
    const auto health = static_cast<std::uint16_t>(std::clamp<int>(
        source.health > 0 ? source.health : maximum_health, 0, maximum_health));
    const auto native_weapon = static_cast<WeaponId>(source.attributes & 0xffU);
    const auto disposition =
        scene_object.class_id == 0x01U   ? NpcDisposition::hostile
        : scene_object.class_id == 0x35U ? NpcDisposition::ally
                                         : NpcDisposition::neutral;
    auto state = NpcState{
        .active = true,
        .object = static_cast<std::uint16_t>(index),
        .source_index = scene_object.source_index,
        .disposition = disposition,
        .weapon =
            isValidWeaponId(native_weapon) ? native_weapon : WeaponId::unarmed,
        .x = static_cast<double>(scene_object.transform.x),
        .y = -static_cast<double>(scene_object.transform.y),
        .z = static_cast<double>(scene_object.transform.z),
        .home_x = static_cast<double>(scene_object.transform.x),
        .home_y = -static_cast<double>(scene_object.transform.y),
        .home_z = static_cast<double>(scene_object.transform.z),
        .yaw = headingFromDirection(
            static_cast<double>(scene_object.transform.rotation[2]),
            static_cast<double>(scene_object.transform.rotation[8])),
        .health = health,
        .maximum_health = maximum_health,
        .armor = static_cast<std::uint16_t>(
            mission.definition().index == 0U &&
                    scene_object.source_index == 174U
                ? 250U
            : (source.attributes & 0x4000U) != 0U ? 100U
                                                  : 0U),
        .maximum_armor = static_cast<std::uint16_t>(
            mission.definition().index == 0U &&
                    scene_object.source_index == 174U
                ? 250U
            : (source.attributes & 0x4000U) != 0U ? 100U
                                                  : 0U),
        .path_data_offset = source.path_data_offset,
        .random_state =
            0x6d2b79f5U ^ (static_cast<std::uint32_t>(index) * 0x9e3779b9U),
    };
    state.patrol_points.reserve(source.patrol_path.size());
    for (const auto &waypoint : source.patrol_path) {
      state.patrol_points.push_back(NpcPatrolPoint{
          static_cast<double>(waypoint.x),
          -static_cast<double>(waypoint.y),
          static_cast<double>(waypoint.z),
      });
    }
    state.zone_min_x = state.home_x;
    state.zone_max_x = state.home_x;
    state.zone_min_z = state.home_z;
    state.zone_max_z = state.home_z;
    for (const auto &waypoint : state.patrol_points) {
      state.zone_min_x = std::min(state.zone_min_x, waypoint.x);
      state.zone_max_x = std::max(state.zone_max_x, waypoint.x);
      state.zone_min_z = std::min(state.zone_min_z, waypoint.z);
      state.zone_max_z = std::max(state.zone_max_z, waypoint.z);
    }
    const auto zone_padding = state.patrol_points.size() > 1U
                                  ? native_actor_zone_padding
                                  : native_stationary_actor_zone_padding;
    state.zone_min_x -= zone_padding;
    state.zone_max_x += zone_padding;
    state.zone_min_z -= zone_padding;
    state.zone_max_z += zone_padding;
    if (state.patrol_points.size() > 1U) {
      state.behavior = NpcBehavior::patrol;
      state.patrol_index = 1U;
      state.patrol_loops = source.patrol_path_loops;
      state.patrol_loop_start = source.patrol_loop_start;
    }
    if (mission.definition().index == 0U && scene_object.source_index == 173U) {
      state.scripted_defuser = true;
      state.behavior = NpcBehavior::idle;
      state.patrol_index = 0U;
    }
    const auto opening_lane = [index](const auto &actors) {
      for (std::size_t lane = 0U; lane < actors.size(); ++lane) {
        if (actors[lane] == index) {
          return static_cast<std::uint8_t>(lane);
        }
      }
      return std::uint8_t{0xffU};
    };
    const auto cbdc_lane = opening_lane(opening_cbdc_objects_);
    const auto terrorist_lane = opening_lane(opening_terrorist_objects_);
    if (cbdc_lane < opening_encounter_lanes.size()) {
      state.scripted_opening_lane = cbdc_lane;
      state.scripted_intro_agent = true;
      state.health = 100U;
      state.maximum_health = 100U;
    } else if (terrorist_lane < opening_encounter_lanes.size()) {
      state.scripted_opening_lane = terrorist_lane;
      state.health = 100U;
      state.maximum_health = 100U;
      if (terrorist_lane == 1U) {
        state.weapon = WeaponId::glock_17;
      }
    }
    state.scripted_opening_combat =
        state.scripted_opening_lane < opening_encounter_lanes.size();
    // The only actor whose native route is linked to the opening police
    // car is the street reinforcement that enters over the fence.
    state.scripted_ingress = mission.definition().index == 0U &&
                             source.ai_parameter == 1U &&
                             source.linked_object == 57;
    const auto &weapon_definition = weaponDefinition(state.weapon);
    state.magazine_capacity = weapon_definition.magazine_capacity;
    state.magazine = weapon_definition.magazine_capacity;
    state.reserve_ammo = static_cast<std::uint16_t>(std::min<std::uint32_t>(
        static_cast<std::uint32_t>(weapon_definition.magazine_capacity) * 4U,
        std::numeric_limits<std::uint16_t>::max()));
    state.last_known_player_x = state.x;
    state.last_known_player_z = state.z;
    object_health_[index] = state.health;
    object_spawn_health_[index] = state.health;
    npc_states_[index] = state;
    npc_spawn_states_[index] = state;
    auto mission_source_index = scene_object.source_index;
    auto mission_ai_parameter = source.ai_parameter;
    if (cbdc_lane < opening_encounter_lanes.size()) {
      mission_source_index = static_cast<std::uint16_t>(
          opening_native_cbdc_first_slot + cbdc_lane);
      mission_ai_parameter = 0U;
    } else if (terrorist_lane < opening_encounter_lanes.size()) {
      mission_source_index = static_cast<std::uint16_t>(
          opening_native_terrorist_first_slot + terrorist_lane);
      // ai_parameter is faction parity, not a recycle-generator marker.
      mission_ai_parameter = 0U;
    }
    mission_scripts_.configureActor(
        static_cast<std::uint16_t>(index), mission_source_index,
        disposition == NpcDisposition::hostile, mission_ai_parameter);
    if (mission_scripts_.initiallyDormant(static_cast<std::uint16_t>(index))) {
      npc_states_[index].active = false;
      object_script_hidden_[index] = true;
      object_spawn_script_hidden_[index] = true;
    }
  }
  report_load(82U);
  const auto &transform = mission.objects().player().transform;
  spawn_ = PlayerState{
      static_cast<double>(transform.x),
      -static_cast<double>(transform.y),
      static_cast<double>(transform.z),
      headingFromDirection(static_cast<double>(transform.rotation[2]),
                           static_cast<double>(transform.rotation[8])),
      true,
  };
  current_room_ = mission.layout().initialRoom();
  rebuildActiveModels();
  legacy_first_mission_ = std::make_unique<LegacyFirstMissionRuntime>(
      mission.definition(), mission.legacyImage());
  reset();
  report_load(100U);
}

GameplaySession::~GameplaySession() = default;

std::optional<CampaignCarryState>
GameplaySession::campaignCarryState() const noexcept {
  if (!legacy_first_mission_) {
    return std::nullopt;
  }
  const auto *mission = legacy_first_mission_->missionBridge();
  if (mission == nullptr || mission->player_health < 0 ||
      mission->player_armor < 0) {
    return std::nullopt;
  }
  CampaignCarryState state;
  state.owned_weapons =
      mission->inventory.owned_weapons & campaign_persistent_weapon_mask;
  state.owned_weapons |= std::uint32_t{1U}; // unarmed is always available
  for (std::size_t weapon = 0U; weapon < weapon_slot_count; ++weapon) {
    const auto bit = std::uint32_t{1U} << weapon;
    if ((state.owned_weapons & bit) == 0U) {
      continue;
    }
    state.magazines[weapon] = mission->inventory.magazines[weapon];
    state.reserves[weapon] = mission->inventory.reserves[weapon];
  }
  const auto requested = mission->inventory.current_weapon;
  if (requested < weapon_slot_count &&
      (state.owned_weapons & (std::uint32_t{1U} << requested)) != 0U) {
    state.current_weapon = requested;
  } else {
    const auto first_owned = std::countr_zero(state.owned_weapons);
    state.current_weapon = static_cast<std::uint8_t>(first_owned);
  }
  state.health = static_cast<std::uint16_t>(mission->player_health);
  state.armor = static_cast<std::uint16_t>(mission->player_armor);
  return validCampaignCarry(state)
             ? std::optional<CampaignCarryState>{std::move(state)}
             : std::nullopt;
}

bool GameplaySession::applyCampaignCarryState(
    const CampaignCarryState &state) noexcept {
  if (!legacy_first_mission_ || !validCampaignCarry(state) ||
      !legacy_first_mission_->applyCampaignCarryState(state)) {
    return false;
  }
  legacy_last_synced_guest_frame_.reset();
  syncLegacyGameplayBridge();
  return !legacy_runtime_faulted_;
}

bool GameplaySession::activateRetailAllWeaponsCheat() noexcept {
  if (!legacy_first_mission_ ||
      !legacy_first_mission_->activateRetailAllWeaponsCheat()) {
    return false;
  }
  // The test action publishes a new immutable sequence without advancing the
  // retail clock. Force one complete projection so HUD/model selection sees
  // the cheat-authored inventory immediately.
  legacy_last_synced_guest_frame_.reset();
  syncLegacyGameplayBridge();
  return !legacy_runtime_faulted_;
}

bool GameplaySession::setAudioVolumes(
    const GameplayAudioVolumes &volumes) noexcept {
  if (!volumes.valid() || !legacy_first_mission_) {
    return false;
  }
  return legacy_first_mission_->setRetailAudioVolumes({
      .sound_effects =
          legacyRetailAudioVolumeFromPercent(volumes.sound_effects),
      .music = legacyRetailAudioVolumeFromPercent(volumes.music),
      .voice_over = legacyRetailAudioVolumeFromPercent(volumes.voice_over),
  });
}

std::optional<GameplayAudioVolumes>
GameplaySession::audioVolumes() const noexcept {
  if (!legacy_first_mission_) {
    return std::nullopt;
  }
  const auto retail = legacy_first_mission_->retailAudioVolumes();
  if (!retail) {
    return std::nullopt;
  }
  return GameplayAudioVolumes{
      .sound_effects = legacyRetailAudioVolumeToPercent(retail->sound_effects),
      .music = legacyRetailAudioVolumeToPercent(retail->music),
      .voice_over = legacyRetailAudioVolumeToPercent(retail->voice_over),
  };
}

bool GameplaySession::advanceAudioFrameClock() noexcept {
  return legacy_first_mission_ &&
         legacy_first_mission_->advanceAudioFrameClock();
}

bool GameplaySession::advanceAudioSliceClock() noexcept {
  return legacy_first_mission_ &&
         legacy_first_mission_->advanceAudioSliceClock();
}

std::size_t
GameplaySession::takePcm(std::span<psx::SpuPcmFrame> destination) noexcept {
  return legacy_first_mission_ ? legacy_first_mission_->takePcm(destination)
                               : 0U;
}

void GameplaySession::clearPcm() noexcept {
  if (legacy_first_mission_) {
    legacy_first_mission_->clearPcm();
  }
}

std::optional<LegacyAudioDiagnostics>
GameplaySession::audioDiagnostics() const noexcept {
  return legacy_first_mission_ ? legacy_first_mission_->audioDiagnostics()
                               : std::nullopt;
}

void GameplaySession::resetLegacyWorldVertexColors() {
  legacy_world_vertex_colors_.clear();
  auto section_count = std::size_t{};
  for (const auto &model : models_) {
    section_count += model.scene.sections().size();
  }
  legacy_world_vertex_colors_.reserve(section_count);
  for (std::size_t model_index = 0U; model_index < models_.size();
       ++model_index) {
    const auto &sections = models_[model_index].scene.sections();
    for (std::size_t section_index = 0U; section_index < sections.size();
         ++section_index) {
      LegacyWorldSectionColorsBridgeState colors;
      colors.model = static_cast<std::uint16_t>(model_index);
      colors.section = static_cast<std::uint16_t>(section_index);
      colors.colors.reserve(sections[section_index].vertices.size());
      for (const auto &vertex : sections[section_index].vertices) {
        colors.colors.push_back(vertex.color);
      }
      legacy_world_vertex_colors_.push_back(std::move(colors));
    }
  }
}

void GameplaySession::reset() {
  current_room_ = mission_.layout().initialRoom();
  rebuildActiveModels();
  resetLegacyWorldVertexColors();
  checkpoint_legacy_world_vertex_colors_.clear();
  hud_.reset();
  aim_target_.reset();
  headshot_targeted_ = false;
  locked_target_.reset();
  last_shot_ = {};
  effects_.clear();
  legacy_expl_particles_.clear();
  legacy_park2_flamethrower_ribbons_.clear();
  legacy_world_callouts_.clear();
  taser_target_.reset();
  taser_tether_updates_ = 0U;
  effect_serial_ = 1U;
  mission_scripts_.reset();
  projectiles_.clear();
  legacy_player_grenade_trajectory_.reset();
  death_updates_ = 0U;
  mission_failed_ = false;
  // The STR is followed by the retail in-engine street briefing. The guest
  // runtime owns its camera, fade and transient wall attackers.
  mission_cinematic_phase_ = MissionCinematicPhase::intro;
  mission_cinematic_updates_ = 0U;
  map_fade_.resetFromBlack();
  mission_script_updates_ = 0U;
  finale_explosion_played_ = false;
  legacy_opening_cbdc_seen_.fill(false);
  legacy_opening_terrorist_seen_.fill(false);
  legacy_opening_cbdc_guest_slots_.fill(opening_guest_slot_unbound);
  legacy_opening_cbdc_guest_identities_.fill(0U);
  legacy_opening_terrorist_guest_slots_.fill(opening_guest_slot_unbound);
  legacy_opening_terrorist_guest_identities_.fill(0U);
  legacy_guest_slot_by_scene_object_.assign(objects_.size(), -1);
  legacy_dedicated_actor_presentations_.assign(objects_.size(), false);
  legacy_dedicated_actor_weapons_.assign(objects_.size(), std::nullopt);
  legacy_dynamic_scene_by_guest_slot_.clear();
  legacy_dynamic_identity_by_guest_slot_.clear();
  legacy_dynamic_first_slot_.reset();
  legacy_last_synced_guest_frame_.reset();
  legacy_player_presentation_.reset();
  legacy_mission_state_ = {};
  legacy_mission_objective_count_ = 0U;
  legacy_mission_parameter_count_ = 0U;
  legacy_mission_objective_texts_.clear();
  legacy_mission_parameter_texts_.clear();
  legacy_completed_objectives_ = 0U;
  legacy_failed_objectives_ = 0U;
  legacy_revealed_objectives_ = 0U;
  legacy_notified_objectives_ = 0U;
  legacy_failed_parameters_ = 0U;
  legacy_parameter_mask_ = 0U;
  legacy_ui_messages_.clear();
  legacy_ui_timer_.reset();
  legacy_mission_bridge_active_ = false;
  legacy_player_guest_motion_position_.reset();
  legacy_player_guest_rotation_.reset();
  legacy_intro_movie_requested_.reset();
  legacy_ending_movie_requested_ = false;
  legacy_failure_restart_requested_ = false;
  legacy_runtime_faulted_ = false;
  legacy_presentation_fault_detail_ = "none";
  checkpoint_pending_ = false;
  pending_equipped_weapon_.reset();
  pending_guest_weapon_requests_.clear();
  pending_guest_weapon_.reset();
  pending_guest_weapon_steps_.clear();
  guest_weapon_in_flight_direction_ = 0;
  guest_weapon_in_flight_expected_.reset();
  pending_guest_weapon_menu_ = false;
  guest_quick_weapon_pending_ = false;
  host_manual_aim_ = false;
  retail_host_aim_active_ = false;
  host_manual_aim_strafe_ = 0.0;
  host_manual_aim_body_heading_.reset();
  pending_host_aim_heading_restore_.reset();
  legacy_manual_aim_neutral_camera_.reset();
  legacy_manual_aim_neutral_player_root_ = {};
  std::fill(npc_damaged_.begin(), npc_damaged_.end(), false);
  object_health_ = object_spawn_health_;
  std::fill(object_destroyed_.begin(), object_destroyed_.end(), false);
  for (auto &object : objects_) {
    object.legacy_secondary_model_active = false;
  }
  object_script_hidden_ = object_spawn_script_hidden_;
  const auto sources = mission_.objects().objects();
  if (mission_.definition().index == 0U &&
      opening_police_car_source < sources.size() &&
      opening_police_car_source < source_to_scene_object_.size()) {
    const auto car = source_to_scene_object_[opening_police_car_source];
    if (car < objects_.size()) {
      objects_[car].transform = sources[opening_police_car_source].transform;
    }
  }
  npc_states_ = npc_spawn_states_;
  for (std::size_t index = 0U; index < npc_states_.size(); ++index) {
    if (!npc_states_[index].scripted_intro_agent) {
      continue;
    }
    npc_states_[index].active = true;
    object_script_hidden_[index] = false;
  }
  for (std::size_t index = 0; index < object_health_.size(); ++index) {
    if (object_script_hidden_[index] &&
        !npc_states_[index].scripted_intro_agent) {
      npc_states_[index].active = false;
      continue;
    }
    if (npc_states_[index].active) {
      auto &state = npc_states_[index];
      if (state.scripted_opening_lane < opening_encounter_lanes.size() &&
          state.scripted_intro_agent) {
        const auto &lane = opening_encounter_lanes[state.scripted_opening_lane];
        state.x = lane.cbdc.x;
        state.y = lane.cbdc.y;
        state.z = lane.cbdc.z;
        state.behavior = NpcBehavior::idle;
      } else if (openingEncounterHostile(state)) {
        const auto &lane = opening_encounter_lanes[state.scripted_opening_lane];
        state.x = lane.hostile_spawn.x;
        state.y = lane.hostile_spawn.y;
        state.z = lane.hostile_spawn.z;
        state.yaw = headingFromDirection(
            lane.hostile_midpoint.x - lane.hostile_spawn.x,
            lane.hostile_midpoint.z - lane.hostile_spawn.z);
        state.scripted_midpoint_x = lane.hostile_midpoint.x;
        state.scripted_midpoint_y = lane.hostile_midpoint.y;
        state.scripted_midpoint_z = lane.hostile_midpoint.z;
        state.scripted_combat_x = lane.hostile_hold.x;
        state.scripted_combat_y = lane.hostile_hold.y;
        state.scripted_combat_z = lane.hostile_hold.z;
        state.scripted_ingress = true;
        state.scripted_wall_traversed = false;
        state.scripted_climbing = false;
        state.scripted_opening_midpoint_reached = false;
        state.scripted_opening_arrived = false;
        state.scripted_climb_update = 0U;
        state.scripted_climb_duration = scripted_fence_climb_updates;
        state.scripted_climb_start_x = lane.hostile_midpoint.x;
        state.scripted_climb_start_y = lane.hostile_midpoint.y;
        state.scripted_climb_start_z = lane.hostile_midpoint.z;
        state.scripted_climb_end_x = lane.hostile_landing.x;
        state.scripted_climb_end_y = lane.hostile_landing.y;
        state.scripted_climb_end_z = lane.hostile_landing.z;
        state.locomotion = NpcLocomotion::stationary;
        state.animation_tick = 0U;
      }
      if (state.scripted_intro_agent) {
        const auto ground = findGround(state.x, state.z, state.y);
        if (ground.model < models_.size()) {
          state.y = ground.y;
        }
        state.scripted_midpoint_x = state.x;
        state.scripted_midpoint_y = state.y;
        state.scripted_midpoint_z = state.z;
        state.scripted_combat_x = state.x;
        state.scripted_combat_y = state.y;
        state.scripted_combat_z = state.z;
        state.scripted_intro_spawn_update = 0U;
        state.scripted_low_locomotion = false;
        state.scripted_intro_spawned = true;
        state.scripted_opening_midpoint_reached = false;
        state.scripted_opening_arrived = false;
      }
      object_health_[index] = npc_states_[index].health;
      updateNpcTransform(static_cast<std::uint16_t>(index));
    }
  }
  auto player = spawn_;
  const auto ground = findGround(player.x, player.z, player.y);
  if (ground.model < models_.size()) {
    player.y = ground.y;
    player.grounded = true;
  }
  player_controller_.setWeaponStance(weaponStance(hud_.inventory().current()));
  player_controller_.reset(player);
  if (legacy_first_mission_ != nullptr) {
    legacy_first_mission_->reset();
    syncLegacyGameplayBridge();
  }
  rebuildActiveObjects();
  camera_collision_initialized_ = false;
  updateCameraCollision();
  checkpoint_valid_ = false;
}

bool GameplaySession::restartCheckpoint() {
  const auto runtime_present = legacy_first_mission_ != nullptr;
  const auto runtime_ready = runtime_present && legacy_first_mission_->ready();
  const auto runtime_faulted =
      runtime_present && legacy_first_mission_->faulted();
  if (!checkpoint_valid_) {
    // Georgia Street commits its first retail checkpoint only after the
    // opening rail. Before that boundary the coherent fallback is a complete
    // mission reset, never a host-only checkpoint restore.
    if (!runtime_present || !runtime_ready || legacy_runtime_faulted_ ||
        runtime_faulted) {
      return false;
    }
    reset();
    return legacy_first_mission_->ready() &&
           !legacy_first_mission_->faulted() && !legacy_runtime_faulted_;
  }
  if (!gameplayCheckpointRestoreReady(checkpoint_valid_, runtime_present,
                                      runtime_ready, legacy_runtime_faulted_,
                                      runtime_faulted)) {
    return false;
  }
  // A retail failure reaches state 2 only after the delayed fade completes.
  // At that point LegacyFirstMissionRuntime is intentionally marked finished,
  // but its captured VM checkpoint still has to be restored together with the
  // native presentation state.
  if (!legacy_first_mission_->restoreCheckpoint()) {
    return false;
  }
  // Validate the republished immutable frame before touching the host half of
  // the checkpoint. A failed guest restore must never leave a mixed scene.
  const auto restored_frame = legacy_first_mission_->presentationFrame();
  if (!restored_frame || !legacyPresentationFrameConsumable(
                             *restored_frame, legacy_presentation_sequence_)) {
    return false;
  }
  const auto &restored_bridge = restored_frame->renderer->state;
  // gameplay_trigger_enable is a retail scheduler latch. It can legitimately
  // be clear while a room/script transaction is in flight and is not part of
  // the immutable renderer/UI presentation contract.
  if (!legacyActiveWorldModels(restored_bridge, models_.size())) {
    return false;
  }

  player_controller_ = checkpoint_player_controller_;
  current_room_ = checkpoint_room_;
  active_models_ = checkpoint_active_models_;
  legacy_world_vertex_colors_ = checkpoint_legacy_world_vertex_colors_;
  hud_ = checkpoint_hud_;
  object_health_ = checkpoint_object_health_;
  object_destroyed_ = checkpoint_object_destroyed_;
  for (auto &object : objects_) {
    object.legacy_secondary_model_active = false;
  }
  object_script_hidden_ = checkpoint_object_script_hidden_;
  npc_states_ = checkpoint_npc_states_;
  npc_damaged_ = checkpoint_npc_damaged_;
  // A later room bank may have exposed more recycled slots after this
  // checkpoint was captured. Keep every parallel scene array aligned with
  // the grown presentation pool; those post-checkpoint slots restart hidden
  // and the restored guest snapshot will bind only its actual lifetimes.
  const auto restored_scene_count = npc_states_.size();
  object_health_.resize(objects_.size(), 0U);
  object_destroyed_.resize(objects_.size(), false);
  object_script_hidden_.resize(objects_.size(), true);
  npc_states_.resize(objects_.size());
  npc_damaged_.resize(objects_.size(), false);
  for (std::size_t scene = restored_scene_count; scene < npc_states_.size();
       ++scene) {
    npc_states_[scene].object = static_cast<std::uint16_t>(scene);
    npc_states_[scene].source_index = objects_[scene].source_index;
  }
  projectiles_ = checkpoint_projectiles_;
  legacy_opening_cbdc_seen_ = checkpoint_legacy_opening_cbdc_seen_;
  legacy_opening_terrorist_seen_ = checkpoint_legacy_opening_terrorist_seen_;
  legacy_opening_cbdc_guest_slots_ =
      checkpoint_legacy_opening_cbdc_guest_slots_;
  legacy_opening_cbdc_guest_identities_ =
      checkpoint_legacy_opening_cbdc_guest_identities_;
  legacy_opening_terrorist_guest_slots_ =
      checkpoint_legacy_opening_terrorist_guest_slots_;
  legacy_opening_terrorist_guest_identities_ =
      checkpoint_legacy_opening_terrorist_guest_identities_;
  legacy_guest_slot_by_scene_object_ =
      checkpoint_legacy_guest_slot_by_scene_object_;
  legacy_dynamic_scene_by_guest_slot_ =
      checkpoint_legacy_dynamic_scene_by_guest_slot_;
  legacy_dynamic_identity_by_guest_slot_ =
      checkpoint_legacy_dynamic_identity_by_guest_slot_;
  legacy_dynamic_first_slot_ = checkpoint_legacy_dynamic_first_slot_;
  legacy_last_synced_guest_frame_ = checkpoint_legacy_last_synced_guest_frame_;
  legacy_player_presentation_ = checkpoint_legacy_player_presentation_;
  legacy_intro_movie_requested_ = checkpoint_legacy_intro_movie_requested_;
  legacy_ending_movie_requested_ = checkpoint_legacy_ending_movie_requested_;
  legacy_failure_restart_requested_ = false;
  for (std::size_t index = 0; index < npc_states_.size(); ++index) {
    if (npc_states_[index].active) {
      updateNpcTransform(static_cast<std::uint16_t>(index));
    }
  }
  aim_target_.reset();
  headshot_targeted_ = false;
  locked_target_.reset();
  last_shot_ = checkpoint_last_shot_;
  effects_ = checkpoint_effects_;
  effect_serial_ = checkpoint_effect_serial_;
  legacy_ui_messages_ = checkpoint_legacy_ui_messages_;
  legacy_ui_timer_ = checkpoint_legacy_ui_timer_;
  taser_target_.reset();
  taser_tether_updates_ = 0U;
  mission_scripts_ = checkpoint_mission_scripts_;
  mission_scripts_.resize(objects_.size());
  mission_script_updates_ = checkpoint_mission_script_updates_;
  death_updates_ = checkpoint_death_updates_;
  mission_failed_ = checkpoint_mission_failed_;
  mission_cinematic_phase_ = checkpoint_mission_cinematic_phase_;
  mission_cinematic_updates_ = checkpoint_mission_cinematic_updates_;
  map_fade_ = checkpoint_map_fade_;
  finale_explosion_played_ = checkpoint_finale_explosion_played_;
  checkpoint_pending_ = false;
  pending_equipped_weapon_ = checkpoint_pending_equipped_weapon_;
  pending_guest_weapon_requests_ = checkpoint_pending_guest_weapon_requests_;
  pending_guest_weapon_ = checkpoint_pending_guest_weapon_;
  pending_guest_weapon_steps_ = checkpoint_pending_guest_weapon_steps_;
  guest_weapon_in_flight_direction_ =
      checkpoint_guest_weapon_in_flight_direction_;
  guest_weapon_in_flight_expected_ =
      checkpoint_guest_weapon_in_flight_expected_;
  pending_guest_weapon_menu_ = checkpoint_pending_guest_weapon_menu_;
  guest_quick_weapon_pending_ = checkpoint_guest_quick_weapon_pending_;
  host_manual_aim_ = false;
  retail_host_aim_active_ = false;
  host_manual_aim_strafe_ = 0.0;
  host_manual_aim_body_heading_.reset();
  pending_host_aim_heading_restore_.reset();
  legacy_manual_aim_neutral_camera_.reset();
  legacy_manual_aim_neutral_player_root_ = {};
  // Rebuild only after the checkpoint's hidden flags, NPC lifetimes and
  // guest bindings are restored. Rebuilding earlier resurrected objects
  // which had already despawned when the checkpoint was captured.
  rebuildActiveObjects();
  syncLegacyGameplayBridge();
  if (legacy_runtime_faulted_) {
    return false;
  }
  camera_state_ = checkpoint_camera_state_;
  camera_collision_distance_ = checkpoint_camera_collision_distance_;
  camera_mode_ = checkpoint_camera_mode_;
  camera_collision_initialized_ = checkpoint_camera_collision_initialized_;
  return true;
}

void GameplaySession::captureCheckpoint() {
  if (!legacyMissionAuthoritative() || legacy_runtime_faulted_ ||
      legacy_first_mission_->faulted()) {
    checkpoint_pending_ = false;
    return;
  }
  if (!legacy_first_mission_->captureCheckpoint()) {
    // States 7/9 are a temporary streaming fence, not a failed checkpoint.
    // Retain the pending edge and retry once retail publishes a coherent
    // gameplay state. A real runtime fault is handled by the next bridge sync.
    if (legacy_first_mission_->faulted()) {
      checkpoint_pending_ = false;
    }
    return;
  }
  checkpoint_player_controller_ = player_controller_;
  checkpoint_room_ = current_room_;
  checkpoint_active_models_ = active_models_;
  checkpoint_legacy_world_vertex_colors_ = legacy_world_vertex_colors_;
  checkpoint_hud_ = hud_;
  checkpoint_last_shot_ = last_shot_;
  checkpoint_effects_ = effects_;
  checkpoint_effect_serial_ = effect_serial_;
  checkpoint_legacy_ui_messages_ = legacy_ui_messages_;
  checkpoint_legacy_ui_timer_ = legacy_ui_timer_;
  checkpoint_map_fade_ = map_fade_;
  checkpoint_camera_state_ = camera_state_;
  checkpoint_camera_collision_distance_ = camera_collision_distance_;
  checkpoint_camera_mode_ = camera_mode_;
  checkpoint_camera_collision_initialized_ = camera_collision_initialized_;
  checkpoint_death_updates_ = death_updates_;
  checkpoint_mission_failed_ = mission_failed_;
  checkpoint_mission_cinematic_phase_ = mission_cinematic_phase_;
  checkpoint_mission_cinematic_updates_ = mission_cinematic_updates_;
  checkpoint_finale_explosion_played_ = finale_explosion_played_;
  // Guest RAM and the latched PAD are in the runtime snapshot. Keep the
  // matching host-side transaction so mid-gesture restore resumes exactly.
  checkpoint_pending_equipped_weapon_ = pending_equipped_weapon_;
  checkpoint_pending_guest_weapon_requests_ = pending_guest_weapon_requests_;
  checkpoint_pending_guest_weapon_ = pending_guest_weapon_;
  checkpoint_pending_guest_weapon_steps_ = pending_guest_weapon_steps_;
  checkpoint_guest_weapon_in_flight_direction_ =
      guest_weapon_in_flight_direction_;
  checkpoint_guest_weapon_in_flight_expected_ =
      guest_weapon_in_flight_expected_;
  checkpoint_pending_guest_weapon_menu_ = pending_guest_weapon_menu_;
  checkpoint_guest_quick_weapon_pending_ = guest_quick_weapon_pending_;
  checkpoint_object_health_ = object_health_;
  checkpoint_object_destroyed_ = object_destroyed_;
  checkpoint_object_script_hidden_ = object_script_hidden_;
  checkpoint_npc_states_ = npc_states_;
  checkpoint_npc_damaged_ = npc_damaged_;
  checkpoint_mission_scripts_ = mission_scripts_;
  checkpoint_projectiles_ = projectiles_;
  checkpoint_mission_script_updates_ = mission_script_updates_;
  checkpoint_legacy_opening_cbdc_seen_ = legacy_opening_cbdc_seen_;
  checkpoint_legacy_opening_terrorist_seen_ = legacy_opening_terrorist_seen_;
  checkpoint_legacy_opening_cbdc_guest_slots_ =
      legacy_opening_cbdc_guest_slots_;
  checkpoint_legacy_opening_cbdc_guest_identities_ =
      legacy_opening_cbdc_guest_identities_;
  checkpoint_legacy_opening_terrorist_guest_slots_ =
      legacy_opening_terrorist_guest_slots_;
  checkpoint_legacy_opening_terrorist_guest_identities_ =
      legacy_opening_terrorist_guest_identities_;
  checkpoint_legacy_guest_slot_by_scene_object_ =
      legacy_guest_slot_by_scene_object_;
  checkpoint_legacy_dynamic_scene_by_guest_slot_ =
      legacy_dynamic_scene_by_guest_slot_;
  checkpoint_legacy_dynamic_identity_by_guest_slot_ =
      legacy_dynamic_identity_by_guest_slot_;
  checkpoint_legacy_dynamic_first_slot_ = legacy_dynamic_first_slot_;
  checkpoint_legacy_last_synced_guest_frame_ = legacy_last_synced_guest_frame_;
  checkpoint_legacy_player_presentation_ = legacy_player_presentation_;
  checkpoint_legacy_intro_movie_requested_ = legacy_intro_movie_requested_;
  checkpoint_legacy_ending_movie_requested_ = legacy_ending_movie_requested_;
  checkpoint_valid_ = true;
  checkpoint_pending_ = false;
}

std::vector<std::uint16_t> GameplaySession::buildActiveModels(
    std::uint16_t room, std::span<const std::uint16_t> retail_traversal) const {
  std::vector<std::uint16_t> result;
  result.reserve(mission_.layout().residentModels().size() +
                 mission_.layout().visibility(room).active_models.size() +
                 retail_traversal.size() + 1U);
  const auto add_unique = [this, &result](std::uint16_t model) {
    if (model >= models_.size()) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Active world model is invalid"};
    }
    if (std::ranges::find(result, model) == result.end()) {
      result.push_back(model);
    }
  };
  // DAT +0x78 is the retail always-resident world set.  These models stay
  // loaded across every portal/room transition; omitting them made native
  // presentation and collision disagree with the guest after a transition.
  for (const auto model : mission_.layout().residentModels()) {
    add_unique(model);
  }
  add_unique(room);
  for (const auto model : mission_.layout().visibility(room).active_models) {
    add_unique(model);
  }
  // The authored DAT visibility record has a prefetch tail immediately past
  // the current room envelope. Native widescreen presentation can expose that
  // edge before the retail 4:3 portal traversal does, so render the already
  // resident prefetch ring as well. This extends the visible world by one
  // authored streaming step without inventing adjacency or changing retail
  // simulation/trigger ownership.
  for (const auto model :
       mission_.layout().visibility(room).prefetched_models) {
    add_unique(model);
  }
  // DAT_8012c7d8 contains per-frame portal traversal depths for the retail
  // 4:3 camera, not the complete streamed terrain set. It is useful as a
  // supplemental dynamic visibility signal, but replacing the DAT room
  // envelope with it removes geometry visible to a native widescreen camera.
  for (const auto model : retail_traversal) {
    add_unique(model);
  }
  return result;
}

void GameplaySession::rebuildActiveModels() {
  active_models_ = buildActiveModels(current_room_);
  rebuildActiveObjects();
}

void GameplaySession::rebuildActiveObjects() {
  active_objects_.clear();
  for (const auto room : active_models_) {
    for (const auto source_index : mission_.objects().objectsInRoom(room)) {
      const auto scene_index = source_to_scene_object_[source_index];
      if (scene_index != std::numeric_limits<std::uint16_t>::max() &&
          legacySceneActiveAfterRoomRebuild(
              true, -1,
              scene_index < object_script_hidden_.size() &&
                  object_script_hidden_[scene_index]) &&
          std::ranges::find(active_objects_, scene_index) ==
              active_objects_.end()) {
        active_objects_.push_back(scene_index);
      }
    }
  }
  // Actors are mobile. Once a mission script or their route moves them out
  // of the room that owns their source record, stream them by their live
  // position instead of dropping them with the static room list.
  for (std::size_t index = 0U; index < npc_states_.size(); ++index) {
    const auto &state = npc_states_[index];
    if (!state.active || (index < object_script_hidden_.size() &&
                          object_script_hidden_[index])) {
      continue;
    }
    const auto inside_active_room =
        std::ranges::any_of(active_models_, [&](std::uint16_t model) {
          return model < models_.size() &&
                 containsXZ(models_[model].bounds, state.x, state.z);
        });
    const auto opening_actor =
        mission_cinematic_phase_ == MissionCinematicPhase::intro &&
        state.scripted_opening_combat;
    if ((inside_active_room || opening_actor) &&
        std::ranges::find(active_objects_, static_cast<std::uint16_t>(index)) ==
            active_objects_.end()) {
      active_objects_.push_back(static_cast<std::uint16_t>(index));
    }
  }
  // A guest-resident object owns its lifetime independently of the BIN room
  // which supplied its presentation template. Preserve those bindings when
  // a room rebuild clears the authored list; the next bridge sample will
  // remove it explicitly when retail marks it non-resident or retires it.
  for (std::size_t index = 0U;
       index < legacy_guest_slot_by_scene_object_.size() &&
       index < object_script_hidden_.size();
       ++index) {
    if (!legacySceneActiveAfterRoomRebuild(
            false, legacy_guest_slot_by_scene_object_[index],
            object_script_hidden_[index]) ||
        index >= std::numeric_limits<std::uint16_t>::max()) {
      continue;
    }
    const auto scene = static_cast<std::uint16_t>(index);
    if (std::ranges::find(active_objects_, scene) == active_objects_.end()) {
      active_objects_.push_back(scene);
    }
  }
}

GameplaySession::GroundHit
GameplaySession::findGround(double x, double z, double reference_y) const {
  GroundHit best{0.0, std::numeric_limits<std::uint16_t>::max()};
  auto best_distance = std::numeric_limits<double>::max();
  for (const auto model_index : active_models_) {
    const auto &model = models_[model_index];
    if (!containsXZ(model.bounds, x, z)) {
      continue;
    }
    for (const auto &section : model.scene.sections()) {
      if (!containsXZ(section.bounds, x, z)) {
        continue;
      }
      for (const auto &polygon : section.polygons) {
        const auto first = point(section.vertices[polygon.vertex_indices[0]]);
        const auto second = point(section.vertices[polygon.vertex_indices[1]]);
        const auto third = point(section.vertices[polygon.vertex_indices[2]]);
        double height{};
        const auto consider = [&](const Point3 &a, const Point3 &b,
                                  const Point3 &c) {
          if (!triangleHeight(a, b, c, x, z, height)) {
            return;
          }
          const auto distance = std::abs(height - reference_y);
          if (distance < best_distance) {
            best_distance = distance;
            best = GroundHit{height, model_index};
          }
        };
        consider(first, second, third);
        if (polygon.quad) {
          const auto fourth =
              point(section.vertices[polygon.vertex_indices[3]]);
          consider(second, fourth, third);
        }
      }
    }
  }
  for (const auto object_index : active_objects_) {
    if (object_index >= objects_.size() ||
        object_index >= object_script_hidden_.size() ||
        object_script_hidden_[object_index] ||
        objects_[object_index].class_id != moving_elevator_class) {
      continue;
    }
    const auto *model = displayedObjectModel(object_index);
    if (model == nullptr || !model->bounds) {
      continue;
    }
    const auto height = movingPlatformSurfaceY(
        x, z, *model->bounds, objects_[object_index].transform);
    if (!height) {
      continue;
    }
    const auto distance = std::abs(*height - reference_y);
    if (distance < best_distance) {
      best_distance = distance;
      // The room below the platform remains the streaming owner at both
      // stops; elevator motion itself is not a portal transition.
      best = GroundHit{*height, current_room_};
    }
  }
  return best;
}

bool GameplaySession::collidesWithWall(double x, double y, double z) const {
  const auto radius_squared = player_radius * player_radius;
  const auto player_top = y - player_height;
  for (const auto model_index : active_models_) {
    const auto &model = models_[model_index];
    if (x + player_radius < model.bounds.minimum_x ||
        x - player_radius > model.bounds.maximum_x ||
        z + player_radius < model.bounds.minimum_z ||
        z - player_radius > model.bounds.maximum_z) {
      continue;
    }
    for (const auto &section : model.scene.sections()) {
      if (x + player_radius < section.bounds.minimum_x ||
          x - player_radius > section.bounds.maximum_x ||
          z + player_radius < section.bounds.minimum_z ||
          z - player_radius > section.bounds.maximum_z ||
          y < section.bounds.minimum_y ||
          player_top > section.bounds.maximum_y) {
        continue;
      }
      for (const auto &polygon : section.polygons) {
        const auto first = point(section.vertices[polygon.vertex_indices[0]]);
        const auto second = point(section.vertices[polygon.vertex_indices[1]]);
        const auto third = point(section.vertices[polygon.vertex_indices[2]]);
        const auto edge1_x = second.x - first.x;
        const auto edge1_y = second.y - first.y;
        const auto edge1_z = second.z - first.z;
        const auto edge2_x = third.x - first.x;
        const auto edge2_y = third.y - first.y;
        const auto edge2_z = third.z - first.z;
        const auto normal_x = edge1_y * edge2_z - edge1_z * edge2_y;
        const auto normal_y = edge1_z * edge2_x - edge1_x * edge2_z;
        const auto normal_z = edge1_x * edge2_y - edge1_y * edge2_x;
        const auto normal_length = std::sqrt(
            normal_x * normal_x + normal_y * normal_y + normal_z * normal_z);
        if (normal_length <= 0.0001 ||
            std::abs(normal_y) / normal_length > maximum_wall_normal_y) {
          continue;
        }

        std::array<Point3, 4> vertices{first, second, third, third};
        auto vertex_count = 3U;
        if (polygon.quad) {
          vertices[3] = point(section.vertices[polygon.vertex_indices[3]]);
          vertex_count = 4U;
        }
        auto minimum_y = vertices[0].y;
        auto maximum_y = vertices[0].y;
        for (std::size_t index = 1; index < vertex_count; ++index) {
          minimum_y = std::min(minimum_y, vertices[index].y);
          maximum_y = std::max(maximum_y, vertices[index].y);
        }
        if (maximum_y < player_top || minimum_y > y) {
          continue;
        }
        for (std::size_t first_index = 0; first_index < vertex_count;
             ++first_index) {
          for (std::size_t second_index = first_index + 1;
               second_index < vertex_count; ++second_index) {
            if (squaredDistanceToSegment(x, z, vertices[first_index],
                                         vertices[second_index]) <
                radius_squared) {
              return true;
            }
          }
        }
      }
    }
  }
  for (const auto object_index : active_objects_) {
    if (object_index >= objects_.size() ||
        object_script_hidden_[object_index]) {
      continue;
    }
    const auto &object = objects_[object_index];
    // Destroyed props normally stop blocking movement. A wrecked police
    // car is different: CPC.TMD replaces CP.TMD, but its body remains a
    // solid obstacle after the opening rollover.
    if ((objectDestroyed(object_index) && object.class_id != 0x2cU) ||
        !solidMissionObject(object.class_id)) {
      continue;
    }
    const auto *model = displayedObjectModel(object_index);
    if (model == nullptr || !model->bounds) {
      continue;
    }
    const auto &bounds = *model->bounds;
    const auto object_y = -static_cast<double>(object.transform.y);
    const auto vertical_extent = static_cast<double>(std::max({
        std::abs(static_cast<int>(bounds.minimum_y)),
        std::abs(static_cast<int>(bounds.maximum_y)),
        80,
    }));
    if (y < object_y - vertical_extent - player_height ||
        player_top > object_y + vertical_extent) {
      continue;
    }
    const auto yaw =
        headingFromDirection(static_cast<double>(object.transform.rotation[2]),
                             static_cast<double>(object.transform.rotation[8]));
    const auto basis = headingBasis(yaw);
    const auto delta_x = x - static_cast<double>(object.transform.x);
    const auto delta_z = z - static_cast<double>(object.transform.z);
    const auto local_x = delta_x * basis.right.x + delta_z * basis.right.z;
    const auto local_z = delta_x * basis.forward.x + delta_z * basis.forward.z;
    if (local_x >= static_cast<double>(bounds.minimum_x) - player_radius &&
        local_x <= static_cast<double>(bounds.maximum_x) + player_radius &&
        local_z >= static_cast<double>(bounds.minimum_z) - player_radius &&
        local_z <= static_cast<double>(bounds.maximum_z) + player_radius) {
      return true;
    }
  }
  return false;
}

bool GameplaySession::tryMove(PlayerState &player, double x, double z) {
  const auto ground = findGround(x, z, player.y);
  if (ground.model >= models_.size() ||
      std::abs(ground.y - player.y) > maximum_ground_step) {
    return false;
  }
  if (collidesWithWall(x, ground.y, z)) {
    return false;
  }
  player.x = x;
  player.y = ground.y;
  player.z = z;
  player.grounded = true;
  updateCurrentRoom(ground.model, player.x, player.z);
  return true;
}

void GameplaySession::updateCurrentRoom(std::uint16_t ground_model,
                                        double player_x, double player_z) {
  if (ground_model == current_room_) {
    return;
  }
  if (std::ranges::find(active_models_, ground_model) != active_models_.end() &&
      containsXZ(models_[ground_model].bounds, player_x, player_z)) {
    current_room_ = ground_model;
    rebuildActiveModels();
    return;
  }
  for (const auto model : active_models_) {
    if (containsXZ(models_[model].bounds, player_x, player_z)) {
      current_room_ = model;
      rebuildActiveModels();
      return;
    }
  }
}

void GameplaySession::advanceAnimationClock() noexcept {
  player_controller_.advanceAnimationClock();
  map_fade_.advance();
  // GameplaySession itself is the retail 20 Hz clock. The 30 fps host only
  // repeats completed simulation frames and never advances actor state.
  for (auto &state : npc_states_) {
    if (state.active && state.behavior != NpcBehavior::dead) {
      ++state.locomotion_animation_tick;
      ++state.animation_tick;
    }
  }
}

double GameplaySession::traceWorldSegment(double from_x, double from_y,
                                          double from_z, double to_x,
                                          double to_y,
                                          double to_z) const noexcept {
  const Point3 from{from_x, from_y, from_z};
  const Point3 to{to_x, to_y, to_z};
  auto nearest = 1.0;
  for (const auto model_index : active_models_) {
    const auto &model = models_[model_index];
    if (!segmentOverlapsBounds(from, to, model.bounds)) {
      continue;
    }
    for (const auto &section : model.scene.sections()) {
      if (!segmentOverlapsBounds(from, to, section.bounds)) {
        continue;
      }
      for (const auto &polygon : section.polygons) {
        const auto first = point(section.vertices[polygon.vertex_indices[0]]);
        const auto second = point(section.vertices[polygon.vertex_indices[1]]);
        const auto third = point(section.vertices[polygon.vertex_indices[2]]);
        const auto consider = [&](const Point3 &a, const Point3 &b,
                                  const Point3 &c) {
          const auto hit = segmentTriangleIntersection(from, to, a, b, c);
          if (hit && *hit < nearest) {
            nearest = *hit;
          }
        };
        consider(first, second, third);
        if (polygon.quad) {
          const auto fourth =
              point(section.vertices[polygon.vertex_indices[3]]);
          consider(second, fourth, third);
        }
      }
    }
  }
  return nearest;
}

std::optional<double>
GameplaySession::droppedItemGroundY(double x, double z, double reference_y,
                                    std::uint16_t retail_room) const noexcept {
  const auto ground = findGround(x, z, reference_y);
  auto best =
      ground.model < models_.size() ? std::optional{ground.y} : std::nullopt;
  auto best_distance =
      best ? std::abs(*best - reference_y) : std::numeric_limits<double>::max();
  if (retail_room >= models_.size() ||
      std::ranges::find(active_models_, retail_room) != active_models_.end()) {
    return best;
  }

  // A detached pickup can remain visible across a portal after its retail
  // owner room leaves the host active set. Include that exact room in the
  // height query; otherwise the flat fallback can bury the sprite again.
  const auto &model = models_[retail_room];
  if (!containsXZ(model.bounds, x, z)) {
    return best;
  }
  for (const auto &section : model.scene.sections()) {
    if (!containsXZ(section.bounds, x, z)) {
      continue;
    }
    for (const auto &polygon : section.polygons) {
      const auto first = point(section.vertices[polygon.vertex_indices[0]]);
      const auto second = point(section.vertices[polygon.vertex_indices[1]]);
      const auto third = point(section.vertices[polygon.vertex_indices[2]]);
      const auto consider = [&](const Point3 &a, const Point3 &b,
                                const Point3 &c) {
        double height{};
        if (!triangleHeight(a, b, c, x, z, height)) {
          return;
        }
        const auto distance = std::abs(height - reference_y);
        if (distance < best_distance) {
          best_distance = distance;
          best = height;
        }
      };
      consider(first, second, third);
      if (polygon.quad) {
        const auto fourth = point(section.vertices[polygon.vertex_indices[3]]);
        consider(second, fourth, third);
      }
    }
  }
  return best;
}

bool GameplaySession::droppedItemVisibleFrom(
    double from_x, double from_y, double from_z, double to_x, double to_y,
    double to_z, std::uint16_t retail_room) const noexcept {
  // Leave a small endpoint tolerance for the pickup's supporting triangle.
  // A wall hit remains well before the endpoint and is rejected.
  if (traceWorldSegment(from_x, from_y, from_z, to_x, to_y, to_z) < 0.995) {
    return false;
  }
  if (retail_room >= models_.size() ||
      std::ranges::find(active_models_, retail_room) != active_models_.end()) {
    return true;
  }

  const Point3 from{from_x, from_y, from_z};
  const Point3 to{to_x, to_y, to_z};
  const auto &model = models_[retail_room];
  if (!segmentOverlapsBounds(from, to, model.bounds)) {
    return true;
  }
  for (const auto &section : model.scene.sections()) {
    if (!segmentOverlapsBounds(from, to, section.bounds)) {
      continue;
    }
    for (const auto &polygon : section.polygons) {
      const auto first = point(section.vertices[polygon.vertex_indices[0]]);
      const auto second = point(section.vertices[polygon.vertex_indices[1]]);
      const auto third = point(section.vertices[polygon.vertex_indices[2]]);
      const auto blocked = [&](const Point3 &a, const Point3 &b,
                               const Point3 &c) {
        const auto hit = segmentTriangleIntersection(from, to, a, b, c);
        return hit && *hit < 0.995;
      };
      if (blocked(first, second, third)) {
        return false;
      }
      if (polygon.quad) {
        const auto fourth = point(section.vertices[polygon.vertex_indices[3]]);
        if (blocked(second, fourth, third)) {
          return false;
        }
      }
    }
  }
  return true;
}

ActorAimRay GameplaySession::manualAimRay() const noexcept {
  const auto sight_camera = camera();
  const auto ray = cameraRayAtProjectionOffset(
      sight_camera, 0.0, manualAimReticleVerticalOffset());
  return ActorAimRay{
      ray.origin_x,    ray.origin_y,    ray.origin_z,
      ray.direction_x, ray.direction_y, ray.direction_z,
  };
}

void GameplaySession::spawnCombatEffect(GameplayEffectType type, double x,
                                        double y, double z, double direction_x,
                                        double direction_y, double direction_z,
                                        double scale) noexcept {
  constexpr std::size_t maximum_effects = 160U;
  if (effects_.size() >= maximum_effects) {
    effects_.erase(effects_.begin());
  }
  effects_.push_back(makeGameplayEffect(type, x, y, z, direction_x, direction_y,
                                        direction_z, scale, effect_serial_++));
}

void GameplaySession::spawnMuzzleFlash(std::optional<std::uint16_t> npc,
                                       double x, double y, double z,
                                       double direction_x, double direction_z,
                                       double scale) noexcept {
  spawnCombatEffect(GameplayEffectType::muzzle_flash, x, y, z, direction_x, 0.0,
                    direction_z, scale);
  auto &effect = effects_.back();
  effect.attachment = npc ? GameplayEffectAttachment::npc_muzzle
                          : GameplayEffectAttachment::player_muzzle;
  effect.owner_object = npc.value_or(0U);
}

void GameplaySession::spawnActorHitEffects(
    double x, double y, double z, double source_x, double source_y,
    double source_z, bool headshot, GameplayEffectAttachment attachment,
    std::uint16_t owner_object) noexcept {
  auto direction_x = x - source_x;
  auto direction_y = y - source_y;
  auto direction_z = z - source_z;
  const auto length =
      std::sqrt(direction_x * direction_x + direction_y * direction_y +
                direction_z * direction_z);
  if (length > 0.0001) {
    direction_x /= length;
    direction_y /= length;
    direction_z /= length;
  }
  const auto scale = headshot ? 1.25 : 1.0;
  const auto attach_to_owner = [&](GameplayEffect &effect) {
    effect.attachment = attachment;
    effect.owner_object = owner_object;
    if (attachment == GameplayEffectAttachment::player_body) {
      effect.attachment_offset_x = effect.x - player().x;
      effect.attachment_offset_y = effect.y - player().y;
      effect.attachment_offset_z = effect.z - player().z;
    } else if (attachment == GameplayEffectAttachment::npc_body &&
               owner_object < npc_states_.size()) {
      const auto &owner = npc_states_[owner_object];
      effect.attachment_offset_x = effect.x - owner.x;
      effect.attachment_offset_y = effect.y - owner.y;
      effect.attachment_offset_z = effect.z - owner.z;
    }
  };
  spawnCombatEffect(GameplayEffectType::blood_spray, x, y, z, direction_x,
                    direction_y, direction_z, scale);
  attach_to_owner(effects_.back());
  // BLOT.TIM is the original persistent red hit mark. The renderer keeps
  // this small overlay attached at the impact point for the same long tail.
  spawnCombatEffect(GameplayEffectType::blood_decal, x - direction_x * 3.0,
                    y - direction_y * 3.0, z - direction_z * 3.0, direction_x,
                    direction_y, direction_z, scale);
  attach_to_owner(effects_.back());
}

void GameplaySession::updateEffects() noexcept {
  for (auto &effect : effects_) {
    static_cast<void>(advanceGameplayEffect(effect));
  }
  std::erase_if(effects_, [](const GameplayEffect &effect) {
    return effect.remaining_updates == 0U;
  });
  if (taser_tether_updates_ != 0U) {
    --taser_tether_updates_;
    if (taser_tether_updates_ == 0U) {
      taser_target_.reset();
    }
  }
}

void GameplaySession::damageNpc(std::uint16_t target, std::uint16_t damage,
                                WeaponDamageKind kind, bool headshot) noexcept {
  if (target >= npc_states_.size() || target >= object_health_.size() ||
      damage == 0U || !npc_states_[target].active ||
      npc_states_[target].health == 0U) {
    return;
  }
  if (queueLegacyDamage(target, damage, kind, headshot)) {
    return;
  }

  auto &state = npc_states_[target];
  state.health = damage >= state.health
                     ? 0U
                     : static_cast<std::uint16_t>(state.health - damage);
  object_health_[target] = state.health;
  npc_damaged_[target] = true;
  if (state.health == 0U) {
    mission_scripts_.actorKilled(target);
  }
}

bool GameplaySession::tryMoveNpc(NpcState &state, double forward_distance,
                                 double strafe_distance) noexcept {
  const auto basis = headingBasis(state.yaw);
  const auto desired_x =
      basis.forward.x * forward_distance + basis.right.x * strafe_distance;
  const auto desired_z =
      basis.forward.z * forward_distance + basis.right.z * strafe_distance;
  const auto distance = std::hypot(desired_x, desired_z);
  if (distance <= 0.0001) {
    return false;
  }

  const auto desired_heading = headingFromDirection(desired_x, desired_z);
  const auto side = state.avoidance_side < 0 ? -1 : 1;
  const std::array<std::int32_t, 11> offsets{
      0,           side * 192,  -side * 192,  side * 384,
      -side * 384, side * 640,  -side * 640,  side * 896,
      -side * 896, side * 1152, -side * 1152,
  };
  for (const auto offset : offsets) {
    const auto direction = headingDirection(desired_heading + offset);
    const auto candidate_x = state.x + direction.x * distance;
    const auto candidate_z = state.z + direction.z * distance;
    const auto ground = findGround(candidate_x, candidate_z, state.y);
    if (ground.model >= models_.size() ||
        std::abs(ground.y - state.y) > maximum_ground_step ||
        collidesWithWall(candidate_x, ground.y, candidate_z)) {
      continue;
    }

    const auto actor_blocked =
        std::ranges::any_of(active_objects_, [&](std::uint16_t other_index) {
          if (other_index >= npc_states_.size()) {
            return false;
          }
          const auto &other = npc_states_[other_index];
          return &other != &state && other.active && other.health != 0U &&
                 std::hypot(other.x - candidate_x, other.z - candidate_z) <
                     player_radius * 1.35;
        });
    if (actor_blocked) {
      continue;
    }

    state.x = candidate_x;
    state.y = ground.y;
    state.z = candidate_z;
    state.movement_distance = distance;
    if (offset == 0) {
      state.blocked_updates =
          state.blocked_updates > 0U ? state.blocked_updates - 1U : 0U;
    } else {
      ++state.blocked_updates;
      state.avoidance_side = offset < 0 ? -1 : 1;
      if (state.behavior == NpcBehavior::patrol ||
          state.behavior == NpcBehavior::pursue ||
          state.behavior == NpcBehavior::take_cover ||
          (state.behavior == NpcBehavior::attack &&
           state.combat_phase == NpcCombatPhase::retreat)) {
        state.yaw = normalizeHeading(desired_heading + offset);
      }
    }
    return true;
  }

  ++state.blocked_updates;
  if (state.blocked_updates % 6U == 0U) {
    state.avoidance_side = -state.avoidance_side;
  }
  return false;
}

bool GameplaySession::tryBeginNpcClimb(NpcState &state,
                                       const NpcPatrolPoint &target,
                                       bool authored_transition) noexcept {
  const auto delta_x = target.x - state.x;
  const auto delta_z = target.z - state.z;
  const auto target_distance = std::hypot(delta_x, delta_z);
  if (target_distance < 180.0) {
    return false;
  }
  const auto direction_x = delta_x / target_distance;
  const auto direction_z = delta_z / target_distance;
  const auto maximum_landing_distance = std::min(target_distance, 760.0);
  auto barrier_distance =
      authored_transition ? std::optional<double>{0.0} : std::nullopt;
  for (auto distance = 60.0; distance <= maximum_landing_distance;
       distance += 40.0) {
    const auto landing_x = state.x + direction_x * distance;
    const auto landing_z = state.z + direction_z * distance;
    const auto ground = findGround(landing_x, landing_z, state.y);
    if (ground.model >= models_.size() ||
        std::abs(ground.y - state.y) > maximum_ground_step ||
        collidesWithWall(landing_x, ground.y, landing_z)) {
      if (!barrier_distance) {
        barrier_distance = distance;
      }
      continue;
    }
    if (!barrier_distance) {
      continue;
    }
    // Start CLIMBA only when the actor has physically reached the wall.
    // Scanning the whole segment and starting immediately was the cause of
    // the visible run -> mid-road climb discontinuity.
    if (!authored_transition && *barrier_distance > 220.0) {
      return false;
    }
    if (distance < *barrier_distance + 120.0) {
      continue;
    }
    state.scripted_climbing = true;
    state.scripted_wall_traversed = state.scripted_wall_traversed ||
                                    authored_transition ||
                                    openingEncounterHostile(state);
    state.scripted_climb_update = 0U;
    state.scripted_climb_duration = scripted_fence_climb_updates;
    state.scripted_climb_start_x = state.x;
    state.scripted_climb_start_y = state.y;
    state.scripted_climb_start_z = state.z;
    state.scripted_climb_end_x = landing_x;
    state.scripted_climb_end_y = ground.y;
    state.scripted_climb_end_z = landing_z;
    state.yaw = headingFromDirection(direction_x, direction_z);
    state.locomotion = NpcLocomotion::stationary;
    state.movement_distance = 0.0;
    state.animation_tick = 0U;
    return true;
  }
  return false;
}

void GameplaySession::updateNpcClimb(NpcState &state) noexcept {
  if (!state.scripted_climbing || state.scripted_climb_duration == 0U) {
    return;
  }
  state.scripted_climb_update =
      std::min(state.scripted_climb_update + 1U, state.scripted_climb_duration);
  const auto linear = static_cast<double>(state.scripted_climb_update) /
                      static_cast<double>(state.scripted_climb_duration);
  const auto amount = linear * linear * (3.0 - 2.0 * linear);
  const auto previous_x = state.x;
  const auto previous_z = state.z;
  state.x =
      state.scripted_climb_start_x +
      (state.scripted_climb_end_x - state.scripted_climb_start_x) * amount;
  state.z =
      state.scripted_climb_start_z +
      (state.scripted_climb_end_z - state.scripted_climb_start_z) * amount;
  state.y =
      state.scripted_climb_start_y +
      (state.scripted_climb_end_y - state.scripted_climb_start_y) * amount -
      std::sin(linear * 3.14159265358979323846) * scripted_fence_climb_height;
  state.movement_distance =
      std::hypot(state.x - previous_x, state.z - previous_z);
  state.locomotion = NpcLocomotion::stationary;
  if (state.scripted_climb_update == state.scripted_climb_duration) {
    state.x = state.scripted_climb_end_x;
    state.y = state.scripted_climb_end_y;
    state.z = state.scripted_climb_end_z;
    state.scripted_climbing = false;
    state.scripted_climb_update = 0U;
    state.blocked_updates = 0U;
    state.animation_tick = 0U;
    if (openingEncounterHostile(state) && state.scripted_wall_traversed) {
      const auto distance = std::hypot(state.scripted_combat_x - state.x,
                                       state.scripted_combat_z - state.z);
      state.scripted_ingress = distance > 70.0;
      if (!state.scripted_ingress) {
        state.x = state.scripted_combat_x;
        state.y = state.scripted_combat_y;
        state.z = state.scripted_combat_z;
        setNpcBehavior(state, NpcBehavior::attack);
        state.combat_phase = NpcCombatPhase::aim;
      }
    }
  }
}

bool GameplaySession::updateNpcScriptedIngress(NpcState &state) noexcept {
  if (!state.scripted_ingress || state.health == 0U) {
    return false;
  }
  if (state.scripted_climbing) {
    updateNpcClimb(state);
    return true;
  }
  if (openingEncounterHostile(state)) {
    const auto approaching_wall = !state.scripted_opening_midpoint_reached;
    const auto target_x =
        approaching_wall ? state.scripted_midpoint_x : state.scripted_combat_x;
    const auto target_y =
        approaching_wall ? state.scripted_midpoint_y : state.scripted_combat_y;
    const auto target_z =
        approaching_wall ? state.scripted_midpoint_z : state.scripted_combat_z;
    const auto delta_x = target_x - state.x;
    const auto delta_z = target_z - state.z;
    const auto distance = std::hypot(delta_x, delta_z);
    if (distance <= 70.0) {
      state.x = target_x;
      state.y = target_y;
      state.z = target_z;
      state.movement_distance = 0.0;
      state.locomotion = NpcLocomotion::stationary;
      if (approaching_wall) {
        state.scripted_opening_midpoint_reached = true;
        state.scripted_wall_traversed = true;
        state.scripted_climbing = true;
        state.scripted_climb_update = 0U;
        state.scripted_climb_start_x = state.x;
        state.scripted_climb_start_y = state.y;
        state.scripted_climb_start_z = state.z;
        state.yaw = headingFromDirection(state.scripted_climb_end_x - state.x,
                                         state.scripted_climb_end_z - state.z);
        state.animation_tick = 0U;
        return true;
      }
      state.scripted_ingress = false;
      state.scripted_opening_arrived = true;
      setNpcBehavior(state, NpcBehavior::attack);
      state.combat_phase = NpcCombatPhase::aim;
      return false;
    }
    const auto step = std::min(scripted_ingress_run_distance, distance);
    state.yaw = headingFromDirection(delta_x, delta_z);
    state.x += delta_x / distance * step;
    state.z += delta_z / distance * step;
    if (approaching_wall) {
      state.y += (target_y - state.y) * (step / distance);
    } else {
      const auto ground = findGround(state.x, state.z, state.y);
      if (ground.model < models_.size()) {
        state.y = ground.y;
      }
    }
    state.movement_distance = step;
    state.locomotion = NpcLocomotion::run;
    return true;
  }
  if (state.patrol_points.empty() ||
      state.patrol_index >= state.patrol_points.size()) {
    state.scripted_ingress = false;
    setNpcBehavior(state, NpcBehavior::alert);
    return false;
  }

  const auto &target = state.patrol_points[state.patrol_index];
  const auto delta_x = target.x - state.x;
  const auto delta_z = target.z - state.z;
  const auto distance = std::hypot(delta_x, delta_z);
  if (distance <= 70.0) {
    if (state.source_index == opening_terrorist_source &&
        state.scripted_opening_combat && state.patrol_index >= 1U) {
      state.scripted_ingress = false;
      state.scripted_combat_x = state.x;
      state.scripted_combat_y = state.y;
      state.scripted_combat_z = state.z;
      setNpcBehavior(state, NpcBehavior::attack);
      state.combat_phase = NpcCombatPhase::aim;
      return false;
    }
    if (state.patrol_index + 1U < state.patrol_points.size()) {
      ++state.patrol_index;
      return true;
    }
    state.scripted_ingress = false;
    if (state.scripted_opening_combat) {
      state.scripted_combat_x = state.x;
      state.scripted_combat_y = state.y;
      state.scripted_combat_z = state.z;
      setNpcBehavior(state, NpcBehavior::attack);
      state.combat_phase = NpcCombatPhase::aim;
      return false;
    }
    state.last_known_player_x = player().x;
    state.last_known_player_z = player().z;
    state.alert_memory_updates = npc_alert_memory_updates;
    setNpcBehavior(state, NpcBehavior::alert);
    return false;
  }

  const auto target_yaw = headingFromDirection(delta_x, delta_z);
  state.yaw = normalizeHeading(
      static_cast<std::int64_t>(state.yaw) +
      std::clamp(signedHeadingDelta(state.yaw, target_yaw), -120, 120));
  state.locomotion = NpcLocomotion::run;
  state.movement_distance = 0.0;

  const auto direction = headingDirection(target_yaw);
  const auto candidate_x =
      state.x + direction.x * scripted_ingress_run_distance;
  const auto candidate_z =
      state.z + direction.z * scripted_ingress_run_distance;
  const auto ground = findGround(candidate_x, candidate_z, state.y);
  const auto straight_path_blocked =
      ground.model >= models_.size() ||
      std::abs(ground.y - state.y) > maximum_ground_step ||
      collidesWithWall(candidate_x, ground.y, candidate_z);
  const auto climb_allowed = state.source_index != opening_terrorist_source ||
                             state.patrol_index == 1U;
  if (straight_path_blocked && climb_allowed &&
      tryBeginNpcClimb(state, target)) {
    return true;
  }
  static_cast<void>(tryMoveNpc(state, scripted_ingress_run_distance, 0.0));
  return true;
}

void GameplaySession::updateOpeningEncounterNpc(NpcState &state) noexcept {
  if (!state.scripted_opening_combat || state.scripted_ingress ||
      state.scripted_climbing || state.health == 0U) {
    return;
  }
  if (state.scripted_intro_agent && !state.scripted_intro_spawned) {
    return;
  }
  state.x = state.scripted_combat_x;
  state.y = state.scripted_combat_y;
  state.z = state.scripted_combat_z;
  state.movement_distance = 0.0;
  state.locomotion = NpcLocomotion::stationary;

  const NpcState *target = nullptr;
  auto target_distance = std::numeric_limits<double>::max();
  for (const auto &candidate : npc_states_) {
    if (!candidate.active || candidate.health == 0U ||
        candidate.scripted_opening_lane != state.scripted_opening_lane ||
        !npcDispositionsOppose(state.disposition, candidate.disposition)) {
      continue;
    }
    const auto distance =
        std::hypot(candidate.x - state.x, candidate.z - state.z);
    if (distance < target_distance) {
      target = &candidate;
      target_distance = distance;
    }
  }
  if (target == nullptr) {
    setNpcBehavior(state, NpcBehavior::idle);
    return;
  }

  state.yaw = headingFromDirection(target->x - state.x, target->z - state.z);
  if (state.behavior != NpcBehavior::attack) {
    setNpcBehavior(state, NpcBehavior::attack);
  }
  state.combat_phase = NpcCombatPhase::aim;
  if (state.fire_animation_updates != 0U) {
    --state.fire_animation_updates;
  }
  const auto period =
      11U + static_cast<unsigned int>(state.source_index % 3U) * 2U;
  const auto encounter_updates =
      mission_cinematic_phase_ == MissionCinematicPhase::intro
          ? mission_cinematic_updates_
          : mission_script_updates_;
  if (encounter_updates >= 12U &&
      (encounter_updates + state.source_index * 5U) % period == 0U &&
      weaponCombatDefinition(state.weapon).fires()) {
    state.fire_animation_updates = 6U;
    const auto direction = headingDirection(state.yaw);
    spawnMuzzleFlash(state.object, state.x + direction.x * 100.0,
                     state.y - 235.0, state.z + direction.z * 100.0,
                     direction.x, direction.z, 1.0);
  }
}

void GameplaySession::updateNpcs(bool player_fired,
                                 bool player_rolled) noexcept {
  const auto &player = player_controller_.state();
  auto danger_level = std::uint8_t{};
  auto danger_critical = false;
  for (const auto object_index : active_objects_) {
    if (object_index >= npc_states_.size()) {
      continue;
    }
    auto &state = npc_states_[object_index];
    if (!state.active) {
      continue;
    }
    state.health = object_health_[object_index];
    const auto legacy_owned_actor =
        legacy_first_mission_ != nullptr && legacy_first_mission_->ready() &&
        object_index < legacy_guest_slot_by_scene_object_.size() &&
        legacy_guest_slot_by_scene_object_[object_index] >= 0;
    if (legacy_owned_actor) {
      continue;
    }
    if (state.scripted_climbing) {
      updateNpcClimb(state);
      updateNpcTransform(object_index);
      continue;
    }
    if (updateNpcScriptedIngress(state)) {
      updateNpcTransform(object_index);
      continue;
    }
    if (state.scripted_defuser && state.health != 0U &&
        mission_scripts_.state().bank_assault_started) {
      constexpr double bank_bomb_x = 14625.0;
      constexpr double bank_bomb_z = 12479.0;
      const auto distance =
          std::hypot(bank_bomb_x - state.x, bank_bomb_z - state.z);
      if (!state.scripted_bank_staged && !state.patrol_points.empty()) {
        state.scripted_opening_combat = false;
        const auto &bank_start = state.patrol_points.front();
        state.x = bank_start.x;
        state.y = bank_start.y;
        state.z = bank_start.z;
        state.weapon = npc_spawn_states_[object_index].weapon;
        state.scripted_bank_staged = true;
      }
      if (distance > 280.0 && !mission_scripts_.state().cbdc_protected) {
        state.scripted_kneeling = false;
        state.behavior = NpcBehavior::patrol;
        state.locomotion = NpcLocomotion::walk;
        state.yaw =
            headingFromDirection(bank_bomb_x - state.x, bank_bomb_z - state.z);
        static_cast<void>(tryMoveNpc(state, 15.0, 0.0));
      } else {
        state.scripted_kneeling = true;
        state.behavior = NpcBehavior::idle;
        state.locomotion = NpcLocomotion::stationary;
      }
      updateNpcTransform(object_index);
      continue;
    }
    if (openingEncounterHostile(state) &&
        mission_cinematic_phase_ == MissionCinematicPhase::gameplay &&
        playerAlive() && npcZoneContains(state, player.x, player.z)) {
      const auto player_distance =
          std::hypot(player.x - state.x, player.z - state.z);
      const auto player_heading =
          headingFromDirection(player.x - state.x, player.z - state.z);
      const auto player_visible =
          player_distance <= npc_maximum_sight_distance &&
          std::abs(signedHeadingDelta(state.yaw, player_heading)) <=
              npc_sight_half_angle &&
          traceWorldSegment(state.x, state.y - actor_target_height, state.z,
                            player.x, player.y - actor_target_height,
                            player.z) >= target_visibility_limit;
      if (player_visible) {
        state.scripted_opening_combat = false;
        state.scripted_ingress = false;
        state.last_known_player_x = player.x;
        state.last_known_player_z = player.z;
        state.alert_memory_updates = npc_alert_memory_updates;
        setNpcBehavior(state, NpcBehavior::alert);
      }
    }
    if (state.scripted_opening_combat && state.health != 0U) {
      updateOpeningEncounterNpc(state);
      updateNpcTransform(object_index);
      continue;
    }
    auto target_is_player = state.disposition == NpcDisposition::hostile &&
                            playerAlive() &&
                            npcZoneContains(state, player.x, player.z);
    std::optional<std::uint16_t> actor_target;
    auto target_x = player.x;
    auto target_y = player.y;
    auto target_z = player.z;
    auto target_distance =
        target_is_player ? std::hypot(target_x - state.x, target_z - state.z)
                         : std::numeric_limits<double>::max();
    const auto opening_player_priority = target_is_player &&
                                         openingEncounterHostileSlot(state) &&
                                         !state.scripted_opening_combat;
    for (const auto candidate_index : active_objects_) {
      if (opening_player_priority) {
        continue;
      }
      if (candidate_index == object_index ||
          candidate_index >= npc_states_.size()) {
        continue;
      }
      const auto &candidate = npc_states_[candidate_index];
      if (!candidate.active || candidate.health == 0U ||
          !npcDispositionsOppose(state.disposition, candidate.disposition) ||
          (state.disposition == NpcDisposition::hostile &&
           !npcZoneContains(state, candidate.x, candidate.z))) {
        continue;
      }
      const auto distance =
          std::hypot(candidate.x - state.x, candidate.z - state.z);
      if (distance >= target_distance) {
        continue;
      }
      target_is_player = false;
      actor_target = candidate_index;
      target_x = candidate.x;
      target_y = candidate.y;
      target_z = candidate.z;
      target_distance = distance;
    }
    const auto has_target = target_is_player || actor_target.has_value();
    const auto delta_x = target_x - state.x;
    const auto delta_z = target_z - state.z;
    const auto target_heading = has_target && target_distance > 0.0001
                                    ? headingFromDirection(delta_x, delta_z)
                                    : state.yaw;
    const auto visible =
        has_target &&
        traceWorldSegment(state.x, state.y - actor_target_height, state.z,
                          target_x, target_y - actor_target_height,
                          target_z) >= target_visibility_limit;
    const auto target_proxy = PlayerState{
        target_x, target_y, target_z, target_heading, true,
    };
    const auto cover = npc_damaged_[object_index] && has_target
                           ? findNpcCover(state, target_proxy)
                           : std::nullopt;
    const auto ally_alerted =
        std::ranges::any_of(active_objects_, [&](std::uint16_t other_index) {
          if (other_index == object_index ||
              other_index >= npc_states_.size()) {
            return false;
          }
          const auto &other = npc_states_[other_index];
          if (!other.active || other.disposition != state.disposition ||
              other.health == 0U || other.behavior == NpcBehavior::idle ||
              other.behavior == NpcBehavior::patrol ||
              other.behavior == NpcBehavior::search) {
            return false;
          }
          return std::hypot(other.x - state.x, other.z - state.z) <=
                 npc_alert_share_distance;
        });
    const auto target_moving =
        target_is_player
            ? player_controller_.actorMotion() != ActorMotion::idle &&
                  player_controller_.actorMotion() != ActorMotion::turn_left &&
                  player_controller_.actorMotion() != ActorMotion::turn_right
            : actor_target && npc_states_[*actor_target].locomotion !=
                                  NpcLocomotion::stationary;
    const auto perception = NpcPerception{
        .player_x = target_x,
        .player_y = target_y,
        .player_z = target_z,
        .distance = target_distance,
        .signed_player_angle = signedHeadingDelta(state.yaw, target_heading),
        .player_visible = visible,
        .heard_weapon = target_is_player && player_fired,
        .ally_alerted = ally_alerted,
        .damaged = npc_damaged_[object_index],
        .target_moving = target_moving,
        .target_inside_zone = has_target,
        .target_hostile =
            actor_target &&
            npc_states_[*actor_target].disposition == NpcDisposition::hostile,
        .cover_available = cover.has_value(),
        .cover_x = cover ? cover->x : 0.0,
        .cover_y = cover ? cover->y : 0.0,
        .cover_z = cover ? cover->z : 0.0,
    };
    const auto previous_locomotion = state.locomotion;
    const auto decision = updateNpcBrain(state, perception);
    if (state.locomotion != previous_locomotion) {
      state.locomotion_animation_tick = 0U;
    }
    state.yaw = decision.desired_yaw;
    const auto exact_aim =
        target_is_player && visible && state.behavior == NpcBehavior::attack &&
        (decision.fire || state.combat_phase == NpcCombatPhase::burst) &&
        std::abs(signedHeadingDelta(state.yaw, target_heading)) <= 96;
    if (target_is_player) {
      const auto danger =
          updateNpcDanger(state, perception, exact_aim, player_rolled);
      danger_level = std::max(danger_level, danger.level);
      danger_critical = danger_critical || danger.critical;
    } else if (state.disposition == NpcDisposition::hostile) {
      // A terrorist exchanging fire with CBDC no longer has a lock on
      // Gabe. Feed an occluded perception tick so stale red DANGER drains
      // instead of remaining latched during NPC-vs-NPC combat.
      auto lost_player = perception;
      lost_player.player_visible = false;
      lost_player.heard_weapon = false;
      static_cast<void>(updateNpcDanger(state, lost_player, false, false));
    }

    if (decision.advance_patrol && !state.patrol_points.empty()) {
      if (state.patrol_loops) {
        state.patrol_index =
            state.patrol_index + 1U >= state.patrol_points.size()
                ? std::min(state.patrol_loop_start,
                           state.patrol_points.size() - 1U)
                : state.patrol_index + 1U;
      } else {
        if (state.patrol_direction > 0 &&
            state.patrol_index + 1U >= state.patrol_points.size()) {
          state.patrol_direction = -1;
        } else if (state.patrol_direction < 0 && state.patrol_index == 0U) {
          state.patrol_direction = 1;
        }
        state.patrol_index = static_cast<std::size_t>(
            static_cast<std::ptrdiff_t>(state.patrol_index) +
            state.patrol_direction);
      }
    }

    if ((std::abs(decision.forward_distance) > 0.0001 ||
         std::abs(decision.strafe_distance) > 0.0001) &&
        state.health != 0U) {
      const auto moved = tryMoveNpc(state, decision.forward_distance,
                                    decision.strafe_distance);
      if (!moved && state.blocked_updates >= 1U &&
          (state.behavior == NpcBehavior::patrol ||
           state.behavior == NpcBehavior::return_home ||
           (state.behavior == NpcBehavior::pursue && state.route_active)) &&
          !state.patrol_points.empty()) {
        const auto route_index =
            state.behavior == NpcBehavior::pursue && state.route_active
                ? state.route_index
                : std::min(state.patrol_index, state.patrol_points.size() - 1U);
        static_cast<void>(
            tryBeginNpcClimb(state, state.patrol_points[route_index]));
      }
    }

    if (decision.fire && visible && has_target) {
      const auto &weapon = weaponCombatDefinition(state.weapon);
      const auto muzzle_forward = headingDirection(state.yaw);
      if (weapon.fire_mode != WeaponFireMode::thrown &&
          state.weapon != WeaponId::flamethrower) {
        spawnMuzzleFlash(object_index, state.x + muzzle_forward.x * 100.0,
                         state.y - 235.0, state.z + muzzle_forward.z * 100.0,
                         muzzle_forward.x, muzzle_forward.z,
                         weapon.damage_kind == WeaponDamageKind::pellet ? 1.35
                                                                        : 1.0);
      }
      const auto accuracy = npcHitChance(
          state, target_distance, static_cast<double>(weapon.maximum_range),
          target_moving);
      state.random_state = state.random_state * 1664525U + 1013904223U;
      const auto roll =
          static_cast<unsigned int>((state.random_state >> 24U) % 100U);
      if (roll < accuracy) {
        const auto damage = weapon.damageAtDistance(target_distance);
        if (target_is_player && playerAlive()) {
          if (weapon.damage_kind == WeaponDamageKind::ballistic ||
              weapon.damage_kind == WeaponDamageKind::pellet) {
            spawnActorHitEffects(player.x, player.y - actor_target_height,
                                 player.z, state.x,
                                 state.y - actor_target_height, state.z, false,
                                 GameplayEffectAttachment::player_body);
          }
          auto vitals = hud_.vitals();
          static_cast<void>(applyPlayerDamage(vitals, damage));
          hud_.setVitals(vitals);
        } else if (actor_target && objectAlive(*actor_target)) {
          if (weapon.damage_kind == WeaponDamageKind::ballistic ||
              weapon.damage_kind == WeaponDamageKind::pellet) {
            spawnActorHitEffects(
                npc_states_[*actor_target].x,
                npc_states_[*actor_target].y - actor_target_height,
                npc_states_[*actor_target].z, state.x,
                state.y - actor_target_height, state.z, false,
                GameplayEffectAttachment::npc_body, *actor_target);
          }
          damageNpc(*actor_target, damage, weapon.damage_kind);
        }
      }
    }

    updateNpcTransform(object_index);
  }

  std::fill(npc_damaged_.begin(), npc_damaged_.end(), false);
  hud_.setDanger(danger_level, danger_critical);
}

void GameplaySession::updateMissionScripts(bool interact) noexcept {
  if (legacyMissionAuthoritative()) {
    return;
  }
  const auto &player_state = player();
  std::vector<MissionScriptActorSnapshot> script_actors;
  script_actors.reserve(npc_states_.size());
  for (std::size_t index = 0U; index < npc_states_.size(); ++index) {
    // Dead and temporarily hidden transient slots still participate in
    // the native population manager. The spawn state identifies actors;
    // the live state only tells whether the current incarnation exists.
    if (!npc_spawn_states_[index].active) {
      continue;
    }
    const auto &spawn = npc_spawn_states_[index];
    const auto room_active =
        std::ranges::find(active_objects_, static_cast<std::uint16_t>(index)) !=
        active_objects_.end();
    const auto distance =
        std::hypot(spawn.x - player_state.x, spawn.z - player_state.z);
    const auto visible_from_player =
        room_active && playerAlive() &&
        traceWorldSegment(player_state.x, player_state.y - actor_target_height,
                          player_state.z, spawn.x,
                          spawn.y - actor_target_height,
                          spawn.z) >= target_visibility_limit;
    script_actors.push_back(MissionScriptActorSnapshot{
        static_cast<std::uint16_t>(index),
        npc_states_[index].disposition == NpcDisposition::hostile,
        object_health_[index] != 0U,
        room_active,
        visible_from_player,
        distance,
    });
  }

  const auto context = MissionScriptUpdateContext{
      script_actors, player_state.x, player_state.y, player_state.z, interact,
  };
  for (const auto &command : mission_scripts_.update(context)) {
    switch (command.type) {
    case MissionScriptCommandType::respawn_actor:
      respawnNpc(command.object);
      break;
    case MissionScriptCommandType::destroy_object_source:
      if (command.object < source_to_scene_object_.size()) {
        const auto scene_object = source_to_scene_object_[command.object];
        if (scene_object < object_destroyed_.size()) {
          object_destroyed_[scene_object] = true;
          object_health_[scene_object] = 0U;
        }
      }
      break;
    case MissionScriptCommandType::capture_checkpoint:
      checkpoint_pending_ = true;
      break;
    case MissionScriptCommandType::mission_failed:
      mission_failed_ = true;
      mission_cinematic_phase_ = MissionCinematicPhase::gameplay;
      mission_cinematic_updates_ = 0U;
      break;
    case MissionScriptCommandType::teleport_to_source:
      teleportPlayerToSource(command.object);
      break;
    case MissionScriptCommandType::start_finale:
      mission_cinematic_phase_ = MissionCinematicPhase::finale;
      mission_cinematic_updates_ = 0U;
      finale_explosion_played_ = false;
      break;
    }
  }
}

void GameplaySession::teleportPlayerToSource(
    std::uint16_t source_index) noexcept {
  const auto &mission_objects = mission_.objects();
  const auto sources = mission_objects.objects();
  if (source_index >= sources.size()) {
    return;
  }
  for (std::size_t room = 0U;
       room < mission_objects.roomCount() && room < models_.size(); ++room) {
    const auto room_sources = mission_objects.objectsInRoom(room);
    if (std::ranges::find(room_sources, source_index) != room_sources.end()) {
      current_room_ = static_cast<std::uint16_t>(room);
      rebuildActiveModels();
      break;
    }
  }
  const auto &destination = sources[source_index];
  auto player_state = player();
  player_state.x = static_cast<double>(destination.transform.x);
  player_state.y = -static_cast<double>(destination.transform.y);
  player_state.z = static_cast<double>(destination.transform.z);
  if (destination.linked_object >= 0 &&
      static_cast<std::size_t>(destination.linked_object) < sources.size()) {
    const auto &elevator =
        sources[static_cast<std::size_t>(destination.linked_object)];
    player_state.x = static_cast<double>(elevator.transform.x);
    player_state.z = static_cast<double>(elevator.transform.z);
  }
  const auto ground =
      findGround(player_state.x, player_state.z, player_state.y);
  if (ground.model < models_.size()) {
    player_state.y = ground.y;
    player_state.grounded = true;
    updateCurrentRoom(ground.model, player_state.x, player_state.z);
  }
  player_controller_.reset(player_state);
  camera_collision_initialized_ = false;
  updateCameraCollision();
}

void GameplaySession::scriptedExplosion(std::uint16_t source_index) noexcept {
  if (source_index >= source_to_scene_object_.size()) {
    return;
  }
  const auto scene_index = source_to_scene_object_[source_index];
  if (scene_index >= objects_.size()) {
    return;
  }
  const auto &transform = objects_[scene_index].transform;
  const auto x = static_cast<double>(transform.x);
  const auto y = -static_cast<double>(transform.y);
  const auto z = static_cast<double>(transform.z);
  spawnCombatEffect(GameplayEffectType::explosion, x, y - 120.0, z, 0.0, -1.0,
                    0.0, 2.1);
  spawnCombatEffect(GameplayEffectType::explosion, x - 180.0, y - 70.0,
                    z + 90.0, -0.5, -1.0, 0.2, 1.5);
  spawnCombatEffect(GameplayEffectType::explosion, x + 170.0, y - 150.0,
                    z - 70.0, 0.4, -1.0, -0.2, 1.65);
  object_destroyed_[scene_index] = true;
  object_health_[scene_index] = 0U;
  const auto sources = mission_.objects().objects();
  for (std::size_t linked = 0U; linked < sources.size(); ++linked) {
    if (linked >= source_to_scene_object_.size()) {
      continue;
    }
    const auto attachment = source_to_scene_object_[linked];
    if (attachment >= objects_.size() ||
        objects_[attachment].class_id == 0x01U ||
        objects_[attachment].class_id == 0x35U) {
      continue;
    }
    const auto &attachment_transform = objects_[attachment].transform;
    const auto attached_by_link = sources[linked].linked_object == source_index;
    const auto attached_vehicle_visual =
        (objects_[attachment].class_id == 0x34U ||
         objects_[attachment].class_id == 0x38U) &&
        std::hypot(static_cast<double>(attachment_transform.x - transform.x),
                   static_cast<double>(attachment_transform.z - transform.z)) <=
            240.0;
    if (!attached_by_link && !attached_vehicle_visual) {
      continue;
    }
    object_destroyed_[attachment] = true;
    object_health_[attachment] = 0U;
  }
}

void GameplaySession::updateScriptedObjects() noexcept {
  if (legacyMissionAuthoritative()) {
    return;
  }
  ++mission_script_updates_;
  const auto sources = mission_.objects().objects();
  constexpr std::array<std::uint16_t, 4U> upper_train_sources{320U, 321U, 322U,
                                                              326U};
  constexpr std::array<std::uint16_t, 4U> lower_train_sources{323U, 324U, 325U,
                                                              327U};
  constexpr double train_cycle = 36000.0;
  constexpr double train_minimum_z = -13000.0;
  const auto phase = std::fmod(
      static_cast<double>(mission_script_updates_) * 144.0, train_cycle);
  const auto move_train = [&](std::span<const std::uint16_t> source_group,
                              bool reverse) {
    for (const auto source_index : source_group) {
      if (source_index >= sources.size() ||
          source_index >= source_to_scene_object_.size()) {
        continue;
      }
      const auto scene_index = source_to_scene_object_[source_index];
      if (scene_index >= objects_.size()) {
        continue;
      }
      auto z = static_cast<double>(sources[source_index].transform.z) +
               (reverse ? phase : -phase);
      while (z < train_minimum_z) {
        z += train_cycle;
      }
      while (z >= train_minimum_z + train_cycle) {
        z -= train_cycle;
      }
      objects_[scene_index].transform.z =
          static_cast<std::int32_t>(std::lround(z));
    }
  };
  move_train(upper_train_sources, false);
  move_train(lower_train_sources, true);

  if (mission_cinematic_phase_ != MissionCinematicPhase::gameplay ||
      !playerAlive()) {
    return;
  }
  const auto &player_state = player();
  const auto hit_by_train = [&](std::span<const std::uint16_t> source_group) {
    return std::ranges::any_of(source_group, [&](std::uint16_t source_index) {
      if (source_index >= source_to_scene_object_.size()) {
        return false;
      }
      const auto scene_index = source_to_scene_object_[source_index];
      if (scene_index >= objects_.size()) {
        return false;
      }
      const auto &train = objects_[scene_index].transform;
      return std::abs(player_state.x - static_cast<double>(train.x)) < 620.0 &&
             std::abs(player_state.y + static_cast<double>(train.y)) < 520.0 &&
             std::abs(player_state.z - static_cast<double>(train.z)) < 780.0;
    });
  };
  if (hit_by_train(upper_train_sources) || hit_by_train(lower_train_sources)) {
    auto vitals = hud_.vitals();
    vitals.armor = 0U;
    vitals.health = 0U;
    hud_.setVitals(vitals);
  }
}

std::optional<std::uint16_t> GameplaySession::openingSceneObjectForGuestActor(
    const LegacyObjectBridgeState &actor,
    std::uint16_t dynamic_first_slot) const noexcept {
  if (mission_.definition().index != 0U || actor.slot < dynamic_first_slot) {
    return std::nullopt;
  }
  if (actor.class_id == 0x35) {
    const auto identity = legacyGuestIdentity(actor);
    for (std::size_t lane = 0U; lane < opening_cbdc_path_pointers.size();
         ++lane) {
      if (legacy_opening_cbdc_guest_slots_[lane] ==
              static_cast<std::int32_t>(actor.slot) &&
          legacy_opening_cbdc_guest_identities_[lane] != 0U &&
          legacy_opening_cbdc_guest_identities_[lane] == identity &&
          opening_cbdc_objects_[lane] < objects_.size()) {
        return opening_cbdc_objects_[lane];
      }
    }
    for (std::size_t lane = 0U; lane < opening_cbdc_path_pointers.size();
         ++lane) {
      if (legacy_opening_cbdc_guest_slots_[lane] !=
              opening_guest_slot_unbound ||
          !actor.resident ||
          actor.slot != static_cast<std::uint32_t>(dynamic_first_slot) +
                            opening_cbdc_dynamic_slot_offsets[lane] ||
          actor.path_pointer != opening_cbdc_path_pointers[lane] ||
          opening_cbdc_objects_[lane] >= objects_.size()) {
        continue;
      }
      return opening_cbdc_objects_[lane];
    }
    return std::nullopt;
  }
  if (actor.class_id != 0x01) {
    return std::nullopt;
  }
  constexpr std::size_t dynamic_opening_lane = 1U;
  const auto scene_object = opening_terrorist_objects_[dynamic_opening_lane];
  if (scene_object >= objects_.size()) {
    return std::nullopt;
  }
  const auto bound_slot =
      legacy_opening_terrorist_guest_slots_[dynamic_opening_lane];
  if (bound_slot >= 0) {
    const auto bound_identity =
        legacy_opening_terrorist_guest_identities_[dynamic_opening_lane];
    if (bound_slot == static_cast<std::int32_t>(actor.slot) &&
        bound_identity != 0U && bound_identity == legacyGuestIdentity(actor)) {
      return scene_object;
    }
    return std::nullopt;
  }
  // Only slot 350's initial 0x801a4934 lifetime owns the dedicated opening
  // presentation. Later 0x4988/0x473c lifetimes use their 1:1 recycled pool
  // scenes even when they coexist with the original actor.
  if (bound_slot == opening_guest_slot_unbound && actor.resident &&
      actor.parameter == 1 && actor.slot == dynamic_first_slot &&
      actor.path_pointer == opening_initial_dynamic_terrorist_path_pointer) {
    return scene_object;
  }
  return std::nullopt;
}

bool GameplaySession::legacyMissionAuthoritative() const noexcept {
  return legacy_first_mission_ != nullptr && legacy_first_mission_->ready() &&
         !legacy_first_mission_->finished();
}

bool GameplaySession::legacyOpeningFinished() const noexcept {
  return legacy_first_mission_ != nullptr &&
         legacy_first_mission_->openingFinished();
}

std::shared_ptr<const LegacyPresentationFrame>
GameplaySession::legacyPresentationFrame() const noexcept {
  return legacy_first_mission_ ? legacy_first_mission_->presentationFrame()
                               : nullptr;
}

std::uint64_t GameplaySession::legacyAimRayPatchCount() const noexcept {
  return legacy_first_mission_ ? legacy_first_mission_->hostAimRayPatchCount()
                               : 0U;
}

bool GameplaySession::legacyScriptedCameraActive() const noexcept {
  if (legacy_first_mission_ == nullptr || !legacy_first_mission_->ready() ||
      !legacy_first_mission_->bridge()) {
    return false;
  }
  const auto &camera = legacy_first_mission_->bridge()->camera;
  return camera.scripted || camera.locked;
}

std::optional<std::int32_t>
GameplaySession::legacyWeaponMenuState() const noexcept {
  if (legacy_first_mission_ == nullptr) {
    return std::nullopt;
  }
  const auto *mission = legacy_first_mission_->missionBridge();
  return mission != nullptr
             ? std::optional<std::int32_t>{mission->weapon_menu_state}
             : std::nullopt;
}

bool GameplaySession::legacyWeaponMenuDirty() const noexcept {
  const auto *mission = legacy_first_mission_ != nullptr
                            ? legacy_first_mission_->missionBridge()
                            : nullptr;
  return mission != nullptr && mission->weapon_menu_dirty;
}

bool GameplaySession::legacyWeaponMenuReady() const noexcept {
  const auto *mission = legacy_first_mission_ != nullptr
                            ? legacy_first_mission_->missionBridge()
                            : nullptr;
  return mission != nullptr && mission->weapon_menu_state >= 0 &&
         mission->weapon_menu_controller_ready &&
         mission->weapon_menu_input_ready;
}

void GameplaySession::stageNativeFirstPersonAim(const GameplayInput &input) {
  // Always consume the host release edge first. A retail camera/control lock
  // may appear on that same update; it must not leave the native controller
  // stuck in first-person after RMB is released.
  if (!input.aim && host_manual_aim_) {
    const auto *release_bridge = legacyMissionAuthoritative()
                                     ? legacy_first_mission_->bridge()
                                     : nullptr;
    const auto guest_control_available =
        release_bridge != nullptr && release_bridge->player.resident &&
        !release_bridge->player.control_locked &&
        !release_bridge->camera.scripted && !release_bridge->camera.locked;
    if (host_manual_aim_body_heading_ && guest_control_available) {
      pending_host_aim_heading_restore_ = *host_manual_aim_body_heading_;
      auto body = player_controller_.state();
      body.yaw = *host_manual_aim_body_heading_;
      player_controller_.synchronizeScriptedPose(body);
      // Seed the release frame so retail builds Gabe's absolute HMD
      // bones from the preserved body heading. syncLegacyGameplayBridge
      // repeats this narrow write after the frame, because L1 teardown
      // is still allowed to touch the root while that frame executes.
      if (legacyMissionAuthoritative() &&
          !legacy_first_mission_->restoreHostPlayerHeading(body.yaw)) {
        legacy_runtime_faulted_ = true;
        mission_failed_ = true;
      }
    } else {
      // A cutscene/control takeover owns Gabe's new body heading. Never
      // restore the pre-aim yaw across that ownership boundary.
      host_manual_aim_body_heading_.reset();
      pending_host_aim_heading_restore_.reset();
    }
    player_controller_.update(PlayerInput{}, *this);
    return;
  }
  if (!input.aim || !playerAlive() || !legacyMissionAuthoritative() ||
      !legacy_first_mission_->openingFinished()) {
    return;
  }
  const auto *bridge = legacy_first_mission_->bridge();
  if (bridge == nullptr || !bridge->player.resident ||
      bridge->player.control_locked || bridge->camera.scripted ||
      bridge->camera.locked) {
    if (player_controller_.aim() == PlayerAimState::first_person) {
      player_controller_.update(PlayerInput{}, *this);
    }
    host_manual_aim_body_heading_.reset();
    pending_host_aim_heading_restore_.reset();
    return;
  }

  // The native camera consumes the composed high-resolution look stream.
  // Never write the host collision root back into the animated guest MATRIX:
  // that matrix's first-person vertical translation is pose data and caused
  // Gabe/camera to jump below the floor when aim started.
  if (playerAim() != PlayerAimState::first_person ||
      !host_manual_aim_body_heading_) {
    host_manual_aim_body_heading_ = player_controller_.state().yaw;
  }
  player_controller_.update(
      PlayerInput{
          .aim = input.aim,
          .look_yaw = input.aim ? input.look_yaw : 0.0,
          .look_pitch = input.aim ? input.look_pitch : 0.0,
      },
      *this);
}

void GameplaySession::stageLegacyHostState(const GameplayInput &input) {
  host_manual_aim_ = input.aim;
  host_manual_aim_strafe_ = input.aim ? input.strafe : 0.0;
  if (!input.aim) {
    legacy_manual_aim_neutral_camera_.reset();
    legacy_manual_aim_neutral_player_root_ = {};
  }
  if (!legacyMissionAuthoritative()) {
    retail_host_aim_active_ = false;
    return;
  }
  const auto *aim_bridge = legacy_first_mission_->bridge();
  const auto retail_aim_requested =
      input.aim && playerAim() == PlayerAimState::first_person &&
      aim_bridge != nullptr && aim_bridge->player.resident &&
      !aim_bridge->player.control_locked && !aim_bridge->camera.scripted &&
      !aim_bridge->camera.locked;
  if (retail_aim_requested != retail_host_aim_active_) {
    if (!legacy_first_mission_->applyHostFirstPersonAim(retail_aim_requested)) {
      legacy_runtime_faulted_ = true;
      mission_failed_ = true;
      return;
    }
    retail_host_aim_active_ = retail_aim_requested;
  }
  if (retail_aim_requested) {
    const auto ray = manualAimRay();
    legacy_first_mission_->setHostAimRay(LegacyHostAimRay{
        ray.origin_x,
        ray.origin_y,
        ray.origin_z,
        ray.direction_x,
        ray.direction_y,
        ray.direction_z,
    });
  } else {
    legacy_first_mission_->setHostAimRay(std::nullopt);
  }
  auto pad = legacyPadStateFromPlayerInput(input);
  const auto clear_pending_weapon = [this] {
    pending_guest_weapon_.reset();
    pending_guest_weapon_steps_.clear();
    guest_weapon_in_flight_direction_ = 0;
    guest_weapon_in_flight_expected_.reset();
    pending_guest_weapon_menu_ = false;
  };
  const auto queue_cycle = [this, &input](std::int64_t requested_steps) {
    if (requested_steps == 0) {
      return;
    }
    const auto magnitude =
        requested_steps < 0 ? -requested_steps : requested_steps;
    const auto steps = std::min<std::size_t>(
        legacy_inventory_weapon_count, static_cast<std::size_t>(magnitude));
    const auto direction =
        static_cast<std::int8_t>(requested_steps > 0 ? 1 : -1);
    constexpr auto maximum_pending_steps = legacy_inventory_weapon_count * 4U;
    const auto must_defer = input.quick_weapon || guest_quick_weapon_pending_ ||
                            !pending_guest_weapon_requests_.empty();
    if (!must_defer) {
      auto inventory = hud_.inventory();
      if (pending_guest_weapon_) {
        static_cast<void>(inventory.select(*pending_guest_weapon_));
      }
      auto changed = false;
      for (std::size_t step = 0U; step < steps; ++step) {
        if (pending_guest_weapon_steps_.size() >= maximum_pending_steps) {
          break;
        }
        const auto step_changed =
            direction > 0 ? inventory.selectNext() : inventory.selectPrevious();
        if (!step_changed) {
          break;
        }
        pending_guest_weapon_steps_.push_back(direction);
        changed = true;
      }
      if (changed) {
        pending_guest_weapon_ = inventory.current();
        pending_guest_weapon_menu_ = true;
      }
      return;
    }
    for (std::size_t step = 0U; step < steps; ++step) {
      if (pending_guest_weapon_requests_.size() >= maximum_pending_steps) {
        break;
      }
      pending_guest_weapon_requests_.push_back(GuestWeaponRequest{
          .direction = direction,
      });
    }
  };
  if (input.weapon_menu_delta != 0) {
    queue_cycle(input.weapon_menu_delta);
  } else if (input.next_weapon != input.previous_weapon) {
    queue_cycle(input.next_weapon ? 1 : -1);
  }
  if (input.direct_weapon) {
    const auto requested = static_cast<WeaponId>(*input.direct_weapon);
    const auto *state = hud_.inventory().tryState(requested);
    if (state != nullptr && state->owned &&
        retailWeaponStepsToTarget(hud_.inventory(), requested, 1)) {
      const auto must_defer =
          input.quick_weapon || guest_quick_weapon_pending_ ||
          pending_guest_weapon_ || !pending_guest_weapon_requests_.empty();
      if (!must_defer) {
        const auto steps =
            retailWeaponStepsToTarget(hud_.inventory(), requested, 1);
        if (steps && *steps != 0) {
          const auto direction = static_cast<std::int8_t>(*steps < 0 ? -1 : 1);
          for (auto step = std::abs(*steps); step > 0; --step) {
            pending_guest_weapon_steps_.push_back(direction);
          }
          pending_guest_weapon_ = requested;
          pending_guest_weapon_menu_ = false;
        }
      } else {
        for (auto request = pending_guest_weapon_requests_.begin();
             request != pending_guest_weapon_requests_.end();) {
          if (request->direct_weapon) {
            request = pending_guest_weapon_requests_.erase(request);
          } else {
            ++request;
          }
        }
        pending_guest_weapon_requests_.push_back(GuestWeaponRequest{
            .direct_weapon = requested,
        });
      }
    }
  }

  const auto *mission_bridge = legacy_first_mission_->missionBridge();
  const auto menu_closed =
      mission_bridge != nullptr && mission_bridge->weapon_menu_state == -5;
  const auto menu_prerequisites_ready =
      mission_bridge != nullptr &&
      mission_bridge->weapon_menu_controller_ready &&
      mission_bridge->weapon_menu_input_ready;
  const auto apply_retail_weapon_step = [this](std::int8_t direction) {
    // Native wheel input has no physical PSX hold interval. Advance the
    // original tape through its five-frame arming threshold, submit one
    // exact retail delta, then commit it before the guest frame dispatcher
    // can synthesize a release for the unbound PC-only action.
    for (std::uint32_t phase = 0U; phase < 5U; ++phase) {
      if (!legacy_first_mission_->applyHostWeaponMenuInput(true, 0)) {
        return false;
      }
    }
    // FUN_800405f4 names positive deltas after the 0x8010c6c4 table;
    // native UI's positive wheel convention follows the reciprocal
    // 0x8010c6e0 table, so the boundary translation reverses the sign.
    return legacy_first_mission_->applyHostWeaponMenuInput(true, -direction) &&
           legacy_first_mission_->applyHostWeaponMenuInput(false, 0);
  };
  const auto apply_retail_quick_weapon = [this] {
    return legacy_first_mission_->applyHostWeaponMenuInput(true, 0) &&
           legacy_first_mission_->applyHostWeaponMenuInput(false, 0);
  };
  if (input.quick_weapon) {
    guest_quick_weapon_pending_ = true;
  }

  const auto start_pending_weapon_transaction = [this] {
    while (!pending_guest_weapon_requests_.empty() && !pending_guest_weapon_) {
      if (pending_guest_weapon_requests_.front().direct_weapon) {
        const auto requested =
            *pending_guest_weapon_requests_.front().direct_weapon;
        pending_guest_weapon_requests_.pop_front();
        const auto *state = hud_.inventory().tryState(requested);
        const auto steps =
            state != nullptr && state->owned
                ? retailWeaponStepsToTarget(hud_.inventory(), requested, 1)
                : std::nullopt;
        if (!steps || *steps == 0) {
          continue;
        }
        const auto direction = static_cast<std::int8_t>(*steps < 0 ? -1 : 1);
        for (auto step = std::abs(*steps); step > 0; --step) {
          pending_guest_weapon_steps_.push_back(direction);
        }
        pending_guest_weapon_ = requested;
        pending_guest_weapon_menu_ = false;
        return;
      }

      auto inventory = hud_.inventory();
      auto changed = false;
      while (!pending_guest_weapon_requests_.empty() &&
             !pending_guest_weapon_requests_.front().direct_weapon) {
        const auto direction = pending_guest_weapon_requests_.front().direction;
        pending_guest_weapon_requests_.pop_front();
        const auto step_changed =
            direction > 0 ? inventory.selectNext() : inventory.selectPrevious();
        if (!step_changed) {
          continue;
        }
        pending_guest_weapon_steps_.push_back(direction);
        changed = true;
      }
      if (changed) {
        pending_guest_weapon_ = inventory.current();
        pending_guest_weapon_menu_ = true;
        return;
      }
    }
  };
  if (!pending_guest_weapon_ && !guest_quick_weapon_pending_ && menu_closed &&
      menu_prerequisites_ready) {
    start_pending_weapon_transaction();
  }

  if (guest_weapon_in_flight_direction_ != 0 &&
      guest_weapon_in_flight_expected_ &&
      hud_.inventory().current() == *guest_weapon_in_flight_expected_) {
    if (!pending_guest_weapon_steps_.empty()) {
      pending_guest_weapon_steps_.pop_front();
    }
    guest_weapon_in_flight_direction_ = 0;
    guest_weapon_in_flight_expected_.reset();
  }

  if (pending_guest_weapon_) {
    if (!menu_closed || !menu_prerequisites_ready) {
      legacy_first_mission_->setHostPadState(pad);
      return;
    }
    if (guest_weapon_in_flight_direction_ != 0) {
      legacy_first_mission_->setHostPadState(pad);
      return;
    }
    if (!pending_guest_weapon_steps_.empty()) {
      const auto direction = pending_guest_weapon_steps_.front();
      const auto current = hud_.inventory().current();
      const auto expected = direction < 0
                                ? hud_.inventory().previousAvailable(current)
                                : hud_.inventory().nextAvailable(current);
      if (expected == current) {
        clear_pending_weapon();
        legacy_first_mission_->setHostPadState(pad);
        return;
      }
      guest_weapon_in_flight_direction_ = direction;
      guest_weapon_in_flight_expected_ = expected;
      if (!apply_retail_weapon_step(direction)) {
        return;
      }
    } else {
      if (hud_.inventory().current() != *pending_guest_weapon_) {
        const auto recovery = retailWeaponStepsToTarget(
            hud_.inventory(), *pending_guest_weapon_, 1);
        if (recovery && *recovery != 0) {
          const auto direction =
              static_cast<std::int8_t>(*recovery < 0 ? -1 : 1);
          for (auto step = std::abs(*recovery); step > 0; --step) {
            pending_guest_weapon_steps_.push_back(direction);
          }
        } else {
          // Ownership may change while commands are queued. Accept
          // the coherent guest result instead of inventing a weapon.
          clear_pending_weapon();
        }
      } else {
        clear_pending_weapon();
      }
    }
  } else {
    pending_guest_weapon_steps_.clear();
    guest_weapon_in_flight_direction_ = 0;
    guest_weapon_in_flight_expected_.reset();
    pending_guest_weapon_menu_ = false;
    if (guest_quick_weapon_pending_ && menu_closed &&
        menu_prerequisites_ready) {
      // Middle click is retail's short press: it never opens the tape.
      if (!apply_retail_quick_weapon()) {
        return;
      }
      guest_quick_weapon_pending_ = false;
    }
  }
  legacy_first_mission_->setHostPadState(pad);
}

void GameplaySession::syncLegacyActorCombatPresentation(
    NpcState &state, const LegacyObjectBridgeState &guest,
    bool fresh_guest_sample) noexcept {
  if (!fresh_guest_sample) {
    return;
  }

  const auto presentation_valid = guest.presentation_controller != 0U;
  const auto presentation_changed =
      state.legacy_presentation_valid != presentation_valid ||
      state.legacy_presentation_code != guest.presentation_enabled ||
      state.legacy_presentation_mode != guest.presentation_mode;
  state.legacy_presentation_valid = presentation_valid;
  state.legacy_presentation_code = guest.presentation_enabled;
  state.legacy_presentation_mode = guest.presentation_mode;
  if (presentation_changed) {
    state.animation_tick = 0U;
  }

  // These pairs are the retail first-level traversal presentations recovered
  // from the live actor controller, rather than a height-based approximation.
  const auto first_traversal_pair =
      guest.presentation_enabled == 10U && guest.presentation_mode == 34U;
  const auto second_traversal_pair =
      guest.presentation_enabled == 12U && guest.presentation_mode == 42U;
  const auto traversing_wall = mission_.definition().index == 0U &&
                               (first_traversal_pair || second_traversal_pair);
  if (state.scripted_climbing != traversing_wall) {
    state.scripted_climbing = traversing_wall;
    state.animation_tick = 0U;
  }

  if (guest.health <= 0) {
    state.scripted_climbing = false;
    state.scripted_kneeling = false;
    state.scripted_low_locomotion = false;
    setLegacyBridgedLocomotion(state, NpcLocomotion::stationary);
    setLegacyBridgedBehavior(state, guest.health < 0 ? NpcBehavior::dead
                                                     : NpcBehavior::dying);
    state.fire_animation_updates = 0U;
    state.combat_phase = NpcCombatPhase::acquire;
    return;
  }

  // +0x41 is the retail per-shot latch. The accompanying 0x10000000 AI
  // flag is sticky and therefore cannot delimit the firing animation.
  const auto firing = guest.ai_fire_latch != 0U;
  const auto previous_fire_latch =
      static_cast<std::uint8_t>(std::min(state.fire_animation_updates, 0xffU));
  const auto was_firing = previous_fire_latch != 0U;
  const auto began_firing =
      legacyFireLatchBeginsShot(previous_fire_latch, guest.ai_fire_latch);
  state.fire_animation_updates = guest.ai_fire_latch;
  state.combat_phase = firing             ? NpcCombatPhase::burst
                       : guest.has_target ? NpcCombatPhase::aim
                                          : NpcCombatPhase::acquire;
  if (firing != was_firing) {
    state.animation_tick = 0U;
  }
  if (!began_firing || !weaponCombatDefinition(state.weapon).fires()) {
    return;
  }

  ++state.shot_serial;
  state.animation_tick = 0U;
  const auto direction = headingDirection(state.yaw);
  spawnMuzzleFlash(state.object, state.x + direction.x * 100.0, state.y - 235.0,
                   state.z + direction.z * 100.0, direction.x, direction.z,
                   1.0);
}

void GameplaySession::ensureLegacyDynamicPresentationCapacity(
    std::size_t capacity) {
  if (capacity <= legacy_dynamic_objects_.size()) {
    return;
  }

  constexpr auto invalid_scene = std::numeric_limits<std::uint16_t>::max();
  auto presentation_template = invalid_scene;
  // Prefer a real HMD actor because recycled records overwhelmingly contain
  // actors. Every binding is replaced by its exact guest definition below,
  // so this object is only a hidden, pre-sized storage placeholder.
  for (const auto scene : source_to_scene_object_) {
    if (scene >= objects_.size() ||
        objects_[scene].model >= object_models_.size() ||
        !std::holds_alternative<assets::HmdModel>(
            object_models_[objects_[scene].model].geometry)) {
      continue;
    }
    presentation_template = scene;
    break;
  }
  if (presentation_template == invalid_scene) {
    for (const auto scene : source_to_scene_object_) {
      if (scene < objects_.size()) {
        presentation_template = scene;
        break;
      }
    }
  }
  if (presentation_template == invalid_scene) {
    return;
  }

  const auto available =
      static_cast<std::size_t>(invalid_scene) - objects_.size();
  capacity = std::min(capacity, legacy_dynamic_objects_.size() + available);
  while (legacy_dynamic_objects_.size() < capacity) {
    const auto scene = static_cast<std::uint16_t>(objects_.size());
    objects_.push_back(objects_[presentation_template]);
    objects_.back().legacy_hmd_bone_count = 0U;
    objects_.back().legacy_hmd_root_space = false;
    objects_.back().legacy_secondary_model_active = false;
    legacy_dynamic_objects_.push_back(scene);

    object_health_.push_back(0U);
    object_spawn_health_.push_back(0U);
    object_destroyed_.push_back(false);
    object_script_hidden_.push_back(true);
    object_spawn_script_hidden_.push_back(true);
    auto dormant = NpcState{};
    dormant.object = scene;
    dormant.source_index = objects_.back().source_index;
    npc_states_.push_back(dormant);
    npc_spawn_states_.push_back(dormant);
    npc_damaged_.push_back(false);
  }
  mission_scripts_.resize(objects_.size());
  legacy_guest_slot_by_scene_object_.resize(objects_.size(), -1);
  legacy_dedicated_actor_presentations_.resize(objects_.size(), false);
  legacy_dedicated_actor_weapons_.resize(objects_.size(), std::nullopt);
}

void GameplaySession::syncLegacyResidentObjects(
    const LegacyGameplayBridgeState &bridge, bool fresh_guest_sample) {
  // classifyTransitionRequest freezes the VM on the terminal frame before
  // this bridge runs. That final sample still owns scripted-object death,
  // actor pose and transform state even though no later guest tick is legal.
  constexpr auto invalid_scene = std::numeric_limits<std::uint16_t>::max();
  const auto dynamic_count =
      bridge.objects.size() > bridge.dynamic_first_slot
          ? bridge.objects.size() - bridge.dynamic_first_slot
          : 0U;
  ensureLegacyDynamicPresentationCapacity(dynamic_count);
  if (!legacy_dynamic_first_slot_ ||
      *legacy_dynamic_first_slot_ != bridge.dynamic_first_slot) {
    legacy_dynamic_first_slot_ = bridge.dynamic_first_slot;
    legacy_dynamic_scene_by_guest_slot_.clear();
    legacy_dynamic_identity_by_guest_slot_.clear();
    for (const auto scene : legacy_dynamic_objects_) {
      if (scene >= objects_.size()) {
        continue;
      }
      object_script_hidden_[scene] = true;
      npc_states_[scene].active = false;
      std::erase(active_objects_, scene);
    }
  }
  legacy_guest_slot_by_scene_object_.assign(objects_.size(), -1);
  legacy_dedicated_actor_presentations_.assign(objects_.size(), false);
  legacy_dedicated_actor_weapons_.assign(objects_.size(), std::nullopt);
  legacy_dynamic_scene_by_guest_slot_.resize(bridge.objects.size(),
                                             invalid_scene);
  legacy_dynamic_identity_by_guest_slot_.resize(bridge.objects.size(), 0U);
  std::vector<bool> used_scene(objects_.size());
  std::vector<bool> source_in_active_dat(source_to_scene_object_.size());
  for (const auto model : active_models_) {
    for (const auto source : mission_.objects().objectsInRoom(model)) {
      if (source < source_in_active_dat.size()) {
        source_in_active_dat[source] = true;
      }
    }
  }
  const auto configure_dynamic_scene = [this](
                                           std::uint16_t scene,
                                           const LegacyObjectBridgeState &guest,
                                           bool binding_changed) {
    if (scene >= objects_.size()) {
      return false;
    }
    constexpr auto invalid_scene = std::numeric_limits<std::uint16_t>::max();
    const SceneObject *presentation_template = nullptr;
    // A recycled guest record retains the retail object-definition index.
    // Bind it only to the exact BIN definition, including definitions
    // which have no authored static source record.
    if (guest.definition < legacy_object_definition_templates_.size()) {
      const auto &exact = legacy_object_definition_templates_[guest.definition];
      if (exact &&
          legacyPresentationTemplateMatches(
              exact->definition_index, exact->class_id, guest.definition,
              static_cast<std::uint32_t>(guest.class_id))) {
        presentation_template = &*exact;
      }
    }
    // Mission 1 has two runtime actors whose exact definitions have no
    // loadable independent presentation. Retain its recovered fallback,
    // but never reinterpret these SUBWAY source numbers in another overlay.
    if (presentation_template == nullptr && mission_.definition().index == 0U &&
        (guest.class_id == 0x01 || guest.class_id == 0x35)) {
      const auto fallback_source = guest.class_id == 0x35
                                       ? opening_cbdc_source
                                       : opening_terrorist_source;
      if (fallback_source < source_to_scene_object_.size()) {
        const auto fallback_scene = source_to_scene_object_[fallback_source];
        if (fallback_scene < objects_.size() &&
            objects_[fallback_scene].class_id ==
                static_cast<std::uint32_t>(guest.class_id)) {
          presentation_template = &objects_[fallback_scene];
        }
      }
    }
    if (presentation_template == nullptr) {
      // Fail closed: a missing model must not silently turn a unique
      // boss or destructible into the first object sharing its class.
      return false;
    }
    const auto hmd_backed =
        presentation_template->model < object_models_.size() &&
        std::holds_alternative<assets::HmdModel>(
            object_models_[presentation_template->model].geometry);
    const auto retail_actor = legacyPresentationUsesRetailNpc(
        hmd_backed, guest.object_handler, guest.ai_controller);

    auto npc_template_scene = invalid_scene;
    if (presentation_template->source_index < source_to_scene_object_.size()) {
      const auto candidate =
          source_to_scene_object_[presentation_template->source_index];
      if (candidate < npc_spawn_states_.size()) {
        npc_template_scene = candidate;
      }
    }
    const auto template_changed = !legacyPresentationTemplateMatches(
        objects_[scene].definition_index, objects_[scene].class_id,
        guest.definition, static_cast<std::uint32_t>(guest.class_id));
    if (binding_changed || template_changed) {
      objects_[scene] = *presentation_template;
      objects_[scene].definition_index = guest.definition;
      objects_[scene].legacy_hmd_bone_count = 0U;
      objects_[scene].legacy_hmd_root_space = false;
      objects_[scene].legacy_secondary_model_active = false;
      object_health_[scene] = 0U;
      object_destroyed_[scene] = false;
      npc_damaged_[scene] = false;
      if (npc_template_scene < npc_spawn_states_.size() && retail_actor) {
        npc_states_[scene] = npc_spawn_states_[npc_template_scene];
      } else {
        npc_states_[scene] = {};
      }
      npc_states_[scene].object = scene;
      npc_states_[scene].source_index = objects_[scene].source_index;
    }
    if (!retail_actor) {
      npc_states_[scene].active = false;
      return true;
    }
    auto &state = npc_states_[scene];
    state.disposition = legacyRetailNpcIsAlly(guest.ai_archetype)
                            ? NpcDisposition::ally
                            : NpcDisposition::hostile;
    state.scripted_defuser = false;
    state.scripted_intro_agent = false;
    state.scripted_intro_spawned = false;
    state.scripted_opening_combat = false;
    state.scripted_opening_lane = 0xffU;
    state.scripted_ingress = false;
    state.scripted_wall_traversed = false;
    state.scripted_climbing = false;
    const auto weapon = static_cast<WeaponId>(guest.attributes & 0xffU);
    if (isValidWeaponId(weapon)) {
      state.weapon = weapon;
      const auto &definition = weaponDefinition(weapon);
      state.magazine_capacity = definition.magazine_capacity;
      if (binding_changed) {
        state.magazine = state.magazine_capacity;
        state.reserve_ammo = static_cast<std::uint16_t>(std::min<std::uint32_t>(
            static_cast<std::uint32_t>(state.magazine_capacity) * 4U,
            std::numeric_limits<std::uint16_t>::max()));
      } else {
        state.magazine = std::min(state.magazine, state.magazine_capacity);
      }
    }
    return true;
  };
  const auto sync_scene = [this, &bridge, &source_in_active_dat, &used_scene,
                           fresh_guest_sample](
                              std::uint16_t scene,
                              const LegacyObjectBridgeState &guest) {
    if (scene >= objects_.size()) {
      return;
    }
    used_scene[scene] = true;
    syncLegacyHmdLighting(objects_[scene], guest);
    legacy_guest_slot_by_scene_object_[scene] =
        static_cast<std::int32_t>(guest.slot);
    const auto *actor_hmd =
        objects_[scene].model < object_models_.size()
            ? std::get_if<assets::HmdModel>(
                  &object_models_[objects_[scene].model].geometry)
            : nullptr;
    const auto actor_hmd_backed = actor_hmd != nullptr;
    const auto actor = legacyPresentationUsesRetailNpc(
        actor_hmd_backed, guest.object_handler, guest.ai_controller);
    const auto has_complete_guest_pose =
        actor_hmd != nullptr &&
        legacyGuestHmdPoseComplete(guest.bone_matrix_count,
                                   actor_hmd->parts().size());
    const auto resident_presentation =
        guest.resident && (guest.presentation_controller == 0U ||
                           guest.presentation_enabled != 0U);
    const auto retail_dormant =
        (guest.instance_state[3] & legacy_instance_dormant) != 0U;
    const auto source_streamed =
        objects_[scene].source_index < source_in_active_dat.size() &&
        source_in_active_dat[objects_[scene].source_index];
    const auto live_position_streamed =
        std::ranges::any_of(active_models_, [&](std::uint16_t model) {
          return model < models_.size() &&
                 containsXZ(models_[model].bounds, guest.position.x,
                            guest.position.z);
        });
    const auto opening_actor =
        mission_cinematic_phase_ == MissionCinematicPhase::intro &&
        mission_.definition().index == 0U &&
        (std::ranges::find(opening_cbdc_objects_, scene) !=
             opening_cbdc_objects_.end() ||
         std::ranges::find(opening_terrorist_objects_, scene) !=
             opening_terrorist_objects_.end());
    // Retail keeps object records resident across room transitions. Residency
    // alone is not a render command: static props belong to the active DAT
    // set, while moved/dynamic props may additionally enter through their live
    // position. Treating every resident record as visible retained the entire
    // mission's texture set and eventually exhausted native VRAM aliases.
    const auto stream_visible =
        actor ? legacyGuestActorStreamVisible(
                    source_streamed, live_position_streamed, guest.pose_flags,
                    opening_actor, guest.simulated, retail_dormant,
                    has_complete_guest_pose)
              : source_streamed || live_position_streamed;
    // Destructible lights such as LIGHT/SPOTLT set both the generic destroyed
    // latch and the dormant bit, then retain the same object record to draw
    // their authored secondary (dark/broken) model. Treating dormant as an
    // unconditional hide discarded that exact post-hit presentation.
    const auto static_presentation_allowed =
        !actor && legacyGuestStaticPropPresentationAllowed(
                      retail_dormant, guest.destroyed(),
                      objects_[scene].destroyed_model.has_value(),
                      objects_[scene].damage_response);
    const auto presented = resident_presentation && stream_visible &&
                           (!retail_dormant || static_presentation_allowed);
    const auto has_exact_guest_presentation =
        has_complete_guest_pose ||
        (guest.display_node != 0U &&
         (guest.pose_flags & legacy_hmd_rendered_this_pass) != 0U);
    const auto dedicated_actor = legacyDedicatedHmdActor(
        actor_hmd_backed, mission_.definition().index,
        objects_[scene].source_index, guest.definition, guest.class_id,
        guest.object_handler, guest.attributes);
    legacy_dedicated_actor_presentations_[scene] =
        legacyDedicatedHmdPresentationAllowed(
            dedicated_actor, guest.alive(), resident_presentation,
            retail_dormant, stream_visible, has_exact_guest_presentation);
    if (legacy_dedicated_actor_presentations_[scene]) {
      legacy_dedicated_actor_weapons_[scene] =
          legacyDedicatedHmdWeapon(dedicated_actor);
    }
    if (actor) {
      auto &state = npc_states_[scene];
      state.disposition = legacyRetailNpcIsAlly(guest.ai_archetype)
                              ? NpcDisposition::ally
                              : NpcDisposition::hostile;
      const auto retail_weapon =
          static_cast<WeaponId>(guest.attributes & 0xffU);
      state.weapon =
          isValidWeaponId(retail_weapon) ? retail_weapon : WeaponId::unarmed;
      syncLegacyGroundContact(state, guest);
      if (!presented) {
        resetLegacyBridgedPresentation(state);
        state.active = false;
        state.health =
            static_cast<std::uint16_t>(std::max<int>(guest.health, 0));
        object_health_[scene] = state.health;
        object_script_hidden_[scene] = true;
        objects_[scene].legacy_hmd_bone_count = 0U;
        objects_[scene].legacy_hmd_root_space = false;
        std::erase(active_objects_, scene);
        return;
      }
      const auto old_x = state.x;
      const auto old_z = state.z;
      state.active = true;
      state.health = static_cast<std::uint16_t>(std::clamp<int>(
          guest.health, 0, std::numeric_limits<std::uint16_t>::max()));
      state.maximum_health = static_cast<std::uint16_t>(
          std::clamp<int>(std::max<int>(guest.maximum_health, guest.health), 1,
                          std::numeric_limits<std::uint16_t>::max()));
      state.x = static_cast<double>(guest.position.x);
      state.y = static_cast<double>(guest.position.y);
      state.z = static_cast<double>(guest.position.z);
      const auto root_matches_authored_position =
          guest.position.x == guest.authored_position.x &&
          guest.position.y == guest.authored_position.y &&
          guest.position.z == guest.authored_position.z;
      const auto contact_space_fallback = legacyHmdFallbackUsesContactSpace(
          has_complete_guest_pose, guest.ground_contact_valid,
          root_matches_authored_position);
      if (contact_space_fallback) {
        state.y = static_cast<double>(guest.ground_contact_valid
                                          ? guest.ground_contact_y
                                          : guest.position.y);
      }
      syncLegacyHmdBones(objects_[scene], guest, contact_space_fallback,
                         !has_complete_guest_pose);
      state.home_x = state.x;
      state.home_y = state.y;
      state.home_z = state.z;
      state.yaw =
          headingFromDirection(static_cast<double>(guest.guest_rotation[2]),
                               static_cast<double>(guest.guest_rotation[8]));
      if (fresh_guest_sample) {
        state.movement_distance =
            std::sqrt((state.x - old_x) * (state.x - old_x) +
                      (state.z - old_z) * (state.z - old_z));
        const auto moving = state.movement_distance > 1.0;
        const auto low_pose = (guest.ai_route_flags & 0x0f00U) == 0x0400U;
        state.scripted_low_locomotion = moving && low_pose;
        state.scripted_kneeling = !moving && low_pose;
        setLegacyBridgedLocomotion(
            state, moving ? low_pose ? NpcLocomotion::walk : NpcLocomotion::run
                          : NpcLocomotion::stationary);
      }
      const auto moving = state.movement_distance > 1.0;
      setLegacyBridgedBehavior(state, legacyBridgedBehavior(guest, moving));
      syncLegacyActorCombatPresentation(state, guest, fresh_guest_sample);
      object_health_[scene] = state.health;
      object_destroyed_[scene] = false;
      objects_[scene].legacy_secondary_model_active = false;
      object_script_hidden_[scene] = false;
      auto &transform = objects_[scene].transform;
      transform.rotation = guest.guest_rotation;
      transform.x = guest.position.x;
      transform.y = static_cast<std::int32_t>(-std::lround(state.y));
      transform.z = guest.position.z;
      if (std::ranges::find(active_objects_, scene) == active_objects_.end()) {
        active_objects_.push_back(scene);
      }
      return;
    }
    if (actor_hmd_backed) {
      // HMD props and NPC records before/after their retail AI lifetime still
      // render from exact guest bones, but must not retain the constructor's
      // synthetic NPC state, weapon, targeting or native damage behavior.
      resetLegacyBridgedPresentation(npc_states_[scene]);
      npc_states_[scene].active = false;
    }
    const auto was_destroyed = object_destroyed_[scene];
    objects_[scene].legacy_secondary_model_active =
        legacyGuestUsesSecondaryItemModel(
            static_cast<std::uint32_t>(guest.class_id), guest.instance_flags);
    const auto destroyed_latched =
        (guest.instance_flags & LegacyObjectBridgeState::destroyed_latch) != 0U;
    if (objectDestructible(scene) &&
        legacyGuestDestructionStateAuthoritative(
            objects_[scene].damage_response, guest.maximum_health,
            destroyed_latched)) {
      const auto bridged_health = guest.destroyed() ? 0 : guest.health;
      object_health_[scene] = static_cast<std::uint16_t>(std::clamp<int>(
          bridged_health, 0, std::numeric_limits<std::uint16_t>::max()));
      // GASPIPE accepts generic health damage, but its retail class-11
      // handler has no death presentation or explosion callback. The silo
      // reuses class 0x11 for HLITE, whose resource-specific response above
      // does own the normal extinguished presentation.
      const auto guest_has_destroyed_presentation =
          guest.class_id != 0x11 ||
          objects_[scene].damage_response != ObjectDamageResponse::explosive;
      object_destroyed_[scene] = legacyGuestDestroyedState(
          objects_[scene].damage_response, was_destroyed,
          guest_has_destroyed_presentation && guest.destroyed());
    }
    if (guest.resident) {
      auto &resident_transform = objects_[scene].transform;
      const auto hmd_backed =
          objects_[scene].model < object_models_.size() &&
          std::holds_alternative<assets::HmdModel>(
              object_models_[objects_[scene].model].geometry);
      if (hmd_backed) {
        const auto contact_space_fallback =
            guest.bone_matrix_count == 0U && guest.ground_contact_valid;
        syncLegacyHmdBones(objects_[scene], guest, contact_space_fallback);
      }
      const auto billboard_presentation =
          objects_[scene].model < object_models_.size() &&
          object_models_[objects_[scene].model].visual_effect ==
              ObjectVisualEffect::billboard_glow;
      // GLIT's root MATRIX is overwritten by the retail render callback
      // with camera-facing scale. The native renderer billboards the
      // authored mesh itself, so retain the authored rotation/scale.
      if (!billboard_presentation) {
        resident_transform.rotation = guest.guest_rotation;
      }
      resident_transform.x = guest.position.x;
      resident_transform.y = -guest.position.y;
      resident_transform.z = guest.position.z;
    }
    object_script_hidden_[scene] = !presented;
    if (!presented) {
      std::erase(active_objects_, scene);
    } else if (std::ranges::find(active_objects_, scene) ==
               active_objects_.end()) {
      active_objects_.push_back(scene);
    }
  };

  const auto static_count =
      std::min<std::size_t>(bridge.dynamic_first_slot, bridge.objects.size());
  for (std::size_t slot = 0U; slot < static_count; ++slot) {
    if (slot >= source_to_scene_object_.size()) {
      continue;
    }
    const auto scene = source_to_scene_object_[slot];
    if (scene >= objects_.size() ||
        objects_[scene].class_id !=
            static_cast<std::uint32_t>(bridge.objects[slot].class_id)) {
      continue;
    }
    sync_scene(scene, bridge.objects[slot]);
  }

  for (std::size_t slot = bridge.dynamic_first_slot;
       slot < bridge.objects.size(); ++slot) {
    const auto &guest = bridge.objects[slot];
    if (legacyRetiredDynamicObject(guest)) {
      legacy_dynamic_identity_by_guest_slot_[slot] = 0U;
      legacy_dynamic_scene_by_guest_slot_[slot] = invalid_scene;
      continue;
    }
    if (openingSceneObjectForGuestActor(guest, bridge.dynamic_first_slot)) {
      legacy_dynamic_identity_by_guest_slot_[slot] = 0U;
      legacy_dynamic_scene_by_guest_slot_[slot] = invalid_scene;
      continue;
    }
    const auto identity = legacyGuestIdentity(guest);
    const auto previous_identity = legacy_dynamic_identity_by_guest_slot_[slot];
    const auto previous_scene = legacy_dynamic_scene_by_guest_slot_[slot];
    const auto pool_index = legacyDynamicPoolIndex(
        bridge.objects.size(), bridge.dynamic_first_slot, guest.slot);
    const auto scene =
        pool_index && *pool_index < legacy_dynamic_objects_.size()
            ? legacy_dynamic_objects_[*pool_index]
            : invalid_scene;
    const auto scene_hmd_backed =
        scene < objects_.size() &&
        objects_[scene].model < object_models_.size() &&
        std::holds_alternative<assets::HmdModel>(
            object_models_[objects_[scene].model].geometry);
    const auto same_identity_respawn =
        legacyPresentationUsesRetailNpc(scene_hmd_backed, guest.object_handler,
                                        guest.ai_controller) &&
        previous_identity == identity && previous_scene < npc_states_.size() &&
        npc_states_[previous_scene].health == 0U && guest.health > 0;
    const auto binding_changed =
        legacyDynamicBindingChanged(identity, previous_identity, scene,
                                    previous_scene) ||
        same_identity_respawn;
    if (scene == invalid_scene || scene >= objects_.size() ||
        used_scene[scene] ||
        !configure_dynamic_scene(scene, guest, binding_changed)) {
      legacy_dynamic_identity_by_guest_slot_[slot] = 0U;
      legacy_dynamic_scene_by_guest_slot_[slot] = invalid_scene;
      continue;
    }
    legacy_dynamic_identity_by_guest_slot_[slot] = identity;
    legacy_dynamic_scene_by_guest_slot_[slot] = scene;
    sync_scene(scene, guest);
  }

  for (const auto scene : legacy_dynamic_objects_) {
    if (scene >= objects_.size() || used_scene[scene]) {
      continue;
    }
    resetLegacyBridgedPresentation(npc_states_[scene]);
    npc_states_[scene].active = false;
    object_health_[scene] = 0U;
    object_destroyed_[scene] = false;
    objects_[scene].legacy_secondary_model_active = false;
    object_script_hidden_[scene] = true;
    objects_[scene].legacy_hmd_bone_count = 0U;
    objects_[scene].legacy_hmd_root_space = false;
    std::erase(active_objects_, scene);
  }

  for (std::size_t scene = 0U; scene < npc_states_.size(); ++scene) {
    if (!npc_states_[scene].active || used_scene[scene] ||
        std::ranges::find(opening_cbdc_objects_, scene) !=
            opening_cbdc_objects_.end() ||
        std::ranges::find(opening_terrorist_objects_, scene) !=
            opening_terrorist_objects_.end()) {
      continue;
    }
    resetLegacyBridgedPresentation(npc_states_[scene]);
    npc_states_[scene].active = false;
    object_script_hidden_[scene] = true;
    std::erase(active_objects_, static_cast<std::uint16_t>(scene));
  }
}

void GameplaySession::syncLegacyGameplayBridge() {
  // Synchronization is idempotent. A host presentation pass may revisit the
  // already-consumed immutable frame between guest ticks; replaying its edge
  // commands would be wrong, but treating that revisit as corruption used to
  // terminate otherwise healthy gameplay.
  if (legacy_first_mission_ != nullptr && !legacy_first_mission_->faulted() &&
      legacy_first_mission_->ready()) {
    const auto current = legacy_first_mission_->presentationFrame();
    if (current && current->sequence == legacy_presentation_sequence_) {
      return;
    }
  }
  legacy_mission_bridge_active_ = false;
  legacy_expl_particles_.clear();
  legacy_park2_flamethrower_ribbons_.clear();
  legacy_world_callouts_.clear();
  taser_target_.reset();
  taser_tether_updates_ = 0U;
  const auto fail_bridge = [this](std::string_view detail) {
    legacy_runtime_faulted_ = true;
    legacy_presentation_fault_detail_ = detail;
    legacy_player_presentation_.reset();
    legacy_player_guest_motion_position_.reset();
    legacy_player_guest_rotation_.reset();
    aim_target_.reset();
    locked_target_.reset();
    headshot_targeted_ = false;
    legacy_world_callouts_.clear();
    taser_target_.reset();
    taser_tether_updates_ = 0U;
    projectiles_.clear();
    legacy_player_grenade_trajectory_.reset();
    legacy_ui_messages_.clear();
    legacy_ui_timer_.reset();
    legacy_mission_objective_count_ = 0U;
    legacy_mission_parameter_count_ = 0U;
    legacy_mission_objective_texts_.clear();
    legacy_mission_parameter_texts_.clear();
    legacy_completed_objectives_ = 0U;
    legacy_failed_objectives_ = 0U;
    legacy_revealed_objectives_ = 0U;
    legacy_notified_objectives_ = 0U;
    legacy_failed_parameters_ = 0U;
    legacy_parameter_mask_ = 0U;
    hud_.setDanger(0U);
    hud_.setTargetHealth(std::nullopt);
    mission_failed_ = true;
  };
  if (legacy_first_mission_ != nullptr && legacy_first_mission_->faulted()) {
    fail_bridge("guest-runtime-fault");
    return;
  }
  // A terminal guest frame freezes the VM before the host consumes its
  // ending/restart request. Keep that final bridge readable once so success
  // and failure cannot fall through into an unsupported application state.
  if (legacy_first_mission_ == nullptr || !legacy_first_mission_->ready()) {
    legacy_player_presentation_.reset();
    return;
  }
  const auto frame = legacy_first_mission_->presentationFrame();
  if (!frame) {
    fail_bridge("missing-frame");
    return;
  }
  if (!legacyPresentationFrameConsumable(*frame,
                                         legacy_presentation_sequence_)) {
    fail_bridge(frame->sequence <= legacy_presentation_sequence_
                    ? "non-monotonic-sequence"
                    : "invalid-frame");
    return;
  }
  const auto &bridge = frame->renderer->state;
  // Keep terrain_triggers_enabled as a diagnostic sample only. Retail owns
  // the trigger scheduler and briefly clears this latch during legitimate
  // script/room transactions; native presentation neither runs nor repairs
  // those triggers.
  const auto retail_active_models =
      legacyActiveWorldModels(bridge, models_.size());
  if (!retail_active_models) {
    fail_bridge("active-world-models");
    return;
  }
  legacy_presentation_sequence_ = frame->sequence;
  legacy_runtime_faulted_ = false;
  legacy_presentation_fault_detail_ = "none";
  const auto &ui = *frame->ui;
  const auto &mission = ui.mission;
  legacy_expl_particles_.reserve(bridge.expl_particles.size());
  for (const auto &particle : bridge.expl_particles) {
    if (particle.position.y == std::numeric_limits<std::int32_t>::min()) {
      continue;
    }
    legacy_expl_particles_.push_back(LegacyExplParticle{
        particle.position.x,
        -particle.position.y,
        particle.position.z,
        particle.controller,
        particle.source_slot,
        static_cast<LegacyEffectSpriteFamily>(particle.family),
        particle.scale_byte,
        particle.frame,
        particle.red,
        particle.green,
        particle.blue,
    });
  }
  legacy_park2_flamethrower_ribbons_.reserve(
      bridge.park2_flamethrower_ribbons.size());
  for (const auto &source : bridge.park2_flamethrower_ribbons) {
    LegacyPark2FlamethrowerRibbon ribbon;
    for (std::size_t corner = 0U; corner < ribbon.corners.size(); ++corner) {
      ribbon.corners[corner] = LegacyProjectedFlamePoint{
          source.corners[corner].x,
          source.corners[corner].y,
      };
    }
    ribbon.world_first = LegacyNativePoint{
        source.world_first.x,
        -source.world_first.y,
        source.world_first.z,
    };
    ribbon.world_second = LegacyNativePoint{
        source.world_second.x,
        -source.world_second.y,
        source.world_second.z,
    };
    ribbon.ordering_depth = source.ordering_depth;
    ribbon.slot = source.slot;
    ribbon.frame = source.frame;
    ribbon.width_shift = source.width_shift;
    ribbon.red = source.color.red;
    ribbon.green = source.color.green;
    ribbon.blue = source.color.blue;
    legacy_park2_flamethrower_ribbons_.push_back(ribbon);
  }
  const auto checkpoint_committed =
      legacy_first_mission_->consumeCheckpointCommit();
  if (checkpoint_committed) {
    checkpoint_pending_ = true;
  }
  if (const auto request = legacy_first_mission_->consumeIntroMovieRequest()) {
    legacy_intro_movie_requested_ = request;
  }
  legacy_ending_movie_requested_ =
      legacy_first_mission_->consumeEndingMovieRequest() ||
      legacy_ending_movie_requested_;
  legacy_failure_restart_requested_ =
      legacy_first_mission_->consumeFailureRestartRequest() ||
      legacy_failure_restart_requested_;
  const auto guest_frame = frame->guest_frame;
  const auto fresh_guest_sample =
      !legacy_last_synced_guest_frame_ ||
      *legacy_last_synced_guest_frame_ != guest_frame;
  legacy_last_synced_guest_frame_ = guest_frame;
  if (fresh_guest_sample &&
      !mergeLegacyWorldVertexColorCache(legacy_world_vertex_colors_,
                                        bridge.world_vertex_colors,
                                        *retail_active_models)) {
    // Renderer descriptors can lead their native DAT counterpart for one
    // streaming edge. Vertex colors are an optional lighting projection: an
    // incompatible sample must retain the last coherent cache, never take
    // down gameplay or partially mutate it.
    legacy_presentation_fault_detail_ = "world-vertex-colors-deferred";
  }
  if (fresh_guest_sample) {
    if (bridge.grenade_trajectory) {
      legacy_player_grenade_trajectory_ = bridge.grenade_trajectory;
    } else if (!bridge.thrown_projectile) {
      legacy_player_grenade_trajectory_.reset();
    }
    projectiles_.clear();
    const auto append_projectile = [&](const auto &source,
                                       bool player_projectile) {
      if (!source) {
        return;
      }
      const auto &retail = *source;
      auto x = static_cast<double>(retail.transform.translation.x);
      auto y = fallbackGrenadePresentationY(retail.transform.translation,
                                             retail.age);
      auto z = static_cast<double>(retail.transform.translation.z);
      if (player_projectile && legacy_player_grenade_trajectory_) {
        if (const auto point = retailGrenadePresentationPoint(
                *legacy_player_grenade_trajectory_,
                retail.transform.translation)) {
          x = point->x;
          y = point->y;
          z = point->z;
        }
      }
      projectiles_.push_back(GameplayProjectile{
          .active = true,
          .weapon = static_cast<WeaponId>(retail.weapon),
          .phase = ProjectilePhase::flying,
          .rotation = retail.transform.rotation,
          .retail_transform = true,
          .x = x,
          .y = y,
          .z = z,
          .remaining_updates = 60U - retail.age,
          .age_updates = retail.age,
      });
    };
    append_projectile(bridge.thrown_projectile, true);
    append_projectile(bridge.enemy_thrown_projectile, false);
  }
  // Apply retail's room transaction before synchronizing object residency.
  // The renderer byte buffer is only the original 4:3 camera traversal; the
  // complete native terrain envelope remains the DAT room visibility list.
  // Rebuilding active objects after resident sync would drop every newly
  // spawned/rebound object for one frame.
  if (fresh_guest_sample && bridge.player.room >= 0) {
    const auto retail_room = static_cast<std::uint16_t>(bridge.player.room);
    const auto native_active_models =
        buildActiveModels(retail_room, *retail_active_models);
    const auto world_set_changed = active_models_ != native_active_models;
    if (current_room_ != retail_room || world_set_changed) {
      // Commit the guest-owned room and the native-safe visibility
      // envelope as one transaction before resident-object sync.
      current_room_ = retail_room;
      active_models_ = native_active_models;
      rebuildActiveObjects();
    }
  }
  syncLegacyResidentObjects(bridge, fresh_guest_sample);
  syncLegacyOpeningBridge(bridge, fresh_guest_sample);
  legacy_mission_bridge_active_ = true;
  if (fresh_guest_sample) {
    if (bridge.player.resident) {
      // LegacyGameplayVm exposes native Y to the renderer; undo that one
      // bridge conversion so MENU.OVL sees the original motion-controller
      // word, byte-for-byte equivalent to lw 4(player->motion).
      legacy_player_guest_motion_position_ = LegacyNativePoint{
          bridge.player.position.x,
          -bridge.player.position.y,
          bridge.player.position.z,
      };
      legacy_player_guest_rotation_ = bridge.player.guest_rotation;
    } else {
      legacy_player_guest_motion_position_.reset();
      legacy_player_guest_rotation_.reset();
    }
    const auto scene_for_guest =
        [this](std::int16_t guest_slot) -> std::optional<std::uint16_t> {
      if (guest_slot < 0) {
        return std::nullopt;
      }
      const auto mapped =
          std::ranges::find(legacy_guest_slot_by_scene_object_, guest_slot);
      if (mapped == legacy_guest_slot_by_scene_object_.end()) {
        return std::nullopt;
      }
      const auto scene = static_cast<std::size_t>(
          std::distance(legacy_guest_slot_by_scene_object_.begin(), mapped));
      if (scene >= objects_.size() || scene >= object_script_hidden_.size() ||
          object_script_hidden_[scene]) {
        return std::nullopt;
      }
      return static_cast<std::uint16_t>(scene);
    };
    for (const auto &event : bridge.weapon_events) {
      if (event.type != LegacyWeaponEventType::shot &&
          event.type != LegacyWeaponEventType::thrown) {
        continue;
      }
      const auto weapon = static_cast<WeaponId>(event.weapon);
      if (!isValidWeaponId(weapon)) {
        continue;
      }
      const auto endpoint_x = static_cast<double>(event.endpoint.x);
      const auto endpoint_y = -static_cast<double>(event.endpoint.y);
      const auto endpoint_z = static_cast<double>(event.endpoint.z);

      const auto scene = scene_for_guest(event.aimed_target_slot);
      const auto headshot =
          scene && event.aimed_target_slot == ui.target.aimed_target_slot &&
          ui.target.headshot;
      last_shot_ = GameplayShotEvent{
          .fired = true,
          .weapon = weapon,
          .target = scene,
          .headshot = headshot,
          .world_impact = event.hit_result != 0U && !scene,
          .impact_x = endpoint_x,
          .impact_y = endpoint_y,
          .impact_z = endpoint_z,
      };
      // Renderer-authoritative missions publish their real SPFX particles
      // from the guest pool. Do not synthesize a second muzzle, impact or
      // blood burst from the sight-ray endpoint: that endpoint is not the
      // retail collision point and moves one simulation sample ahead of the
      // interpolated actor pose.
    }
  }
  const auto native_aim_owns_body =
      host_manual_aim_ &&
      player_controller_.aim() == PlayerAimState::first_person &&
      !bridge.player.control_locked && !bridge.camera.scripted &&
      !bridge.camera.locked;
  const auto pending_restore_owns_body =
      pending_host_aim_heading_restore_.has_value() && bridge.player.resident &&
      !bridge.player.control_locked && !bridge.camera.scripted &&
      !bridge.camera.locked;
  const auto body_heading_override =
      native_aim_owns_body        ? host_manual_aim_body_heading_
      : pending_restore_owns_body ? pending_host_aim_heading_restore_
                                  : std::nullopt;
  if (fresh_guest_sample) {
    const auto neutral_manual_aim_sample =
        host_manual_aim_ &&
        player_controller_.aim() == PlayerAimState::first_person &&
        std::abs(host_manual_aim_strafe_) <= 0.0001 &&
        !bridge.player.control_locked && !bridge.camera.scripted &&
        !bridge.camera.locked;
    if (neutral_manual_aim_sample) {
      // Physical L1 is retained while idle. Its guest camera is the exact
      // center reference from which L1+L2/R2 translate the whole view for a
      // corner peek; the player collision root intentionally remains still.
      legacy_manual_aim_neutral_camera_ = bridge.camera;
      legacy_manual_aim_neutral_player_root_ = bridge.player.position;
    } else if (!host_manual_aim_ || bridge.player.control_locked ||
               bridge.camera.scripted || bridge.camera.locked) {
      legacy_manual_aim_neutral_camera_.reset();
      legacy_manual_aim_neutral_player_root_ = {};
    }
    if (mission.player_slot >= 0 &&
        static_cast<std::size_t>(mission.player_slot) < bridge.objects.size()) {
      const auto &guest =
          bridge.objects[static_cast<std::size_t>(mission.player_slot)];
      const auto *player_hmd =
          std::get_if<assets::HmdModel>(&playerModel().geometry);
      const auto exact_guest_pose =
          guest.class_id == 0 && guest.resident && player_hmd != nullptr &&
          legacyGuestHmdPoseComplete(guest.bone_matrix_count,
                                     player_hmd->parts().size());
      if (exact_guest_pose) {
        SceneObject presentation;
        presentation.model = player_model_;
        presentation.class_id = 0U;
        presentation.transform.rotation = guest.guest_rotation;
        if (body_heading_override) {
          presentation.transform.rotation =
              headingRotation(*body_heading_override);
        }
        presentation.transform.x = guest.position.x;
        presentation.transform.y = -guest.position.y;
        presentation.transform.z = guest.position.z;
        // Player traversal is not a missing-table transient: ROM samples for
        // climb 10/34 and hang 5/18 carry all 15 final world-space matrices.
        // Preserve those retail poses directly instead of substituting a
        // generic CLIMBA clip from control/camera heuristics.
        syncLegacyHmdBones(presentation, guest, false);
        syncLegacyHmdLighting(presentation, guest);
        legacy_player_presentation_.emplace(std::move(presentation));
      } else {
        legacy_player_presentation_.reset();
      }
    } else {
      legacy_player_presentation_.reset();
    }
    if (bridge.player.resident) {
      auto scripted_player = player();
      scripted_player.x = static_cast<double>(bridge.player.position.x);
      scripted_player.y = static_cast<double>(bridge.player.position.y);
      scripted_player.z = static_cast<double>(bridge.player.position.z);
      scripted_player.yaw = headingFromDirection(
          static_cast<double>(bridge.player.guest_rotation[2]),
          static_cast<double>(bridge.player.guest_rotation[8]));
      if (body_heading_override) {
        scripted_player.yaw = *body_heading_override;
      }
      if (mission.player_slot >= 0 &&
          static_cast<std::size_t>(mission.player_slot) <
              bridge.objects.size()) {
        scripted_player.grounded =
            bridge.objects[static_cast<std::size_t>(mission.player_slot)]
                .ground_contact_valid;
      }
      if (bridge.player.control_locked) {
        player_controller_.synchronizeScriptedPose(scripted_player);
      } else if (host_manual_aim_ && !bridge.camera.scripted &&
                 !bridge.camera.locked) {
        // Retail L2/R2 owns exact collision and lateral root motion. This is
        // the motion-controller position (not the pose-space HMD root which
        // caused the old floor drop); body_heading_override above preserves
        // the independent mouse-aim heading.
        player_controller_.synchronizeScriptedPose(scripted_player);
      } else {
        // The final 1->0 sample is still guest-owned; reset the native
        // action state at that exact root before returning control.
        player_controller_.reset(scripted_player);
      }
      camera_collision_initialized_ = false;
      updateCameraCollision();
    }
    auto &inventory = hud_.inventory();
    for (std::size_t index = 0U; index < legacy_inventory_weapon_count;
         ++index) {
      const auto id = static_cast<WeaponId>(index);
      if ((mission.inventory.owned_weapons & (std::uint32_t{1U} << index)) !=
          0U) {
        inventory.grant(id, mission.inventory.magazines[index],
                        mission.inventory.reserves[index]);
      } else {
        inventory.remove(id);
      }
    }
    const auto current =
        static_cast<WeaponId>(mission.inventory.current_weapon);
    if (isValidWeaponId(current) &&
        (mission.inventory.owned_weapons &
         (std::uint32_t{1U} << mission.inventory.current_weapon)) != 0U) {
      static_cast<void>(inventory.select(current));
    }
  }
  if (fresh_guest_sample) {
    // Retail has already applied its reveal, convergence, expiry and fade to
    // these packets. Keeping host notices here would run a second, guessed
    // state machine and expose the complete source string on its first tick.
    legacy_ui_messages_ = mission.messages;
    legacy_ui_timer_ = mission.timer;
  }
  legacy_mission_objective_count_ = mission.objective_count;
  legacy_mission_parameter_count_ = mission.parameter_count;
  if (legacy_mission_objective_texts_ != mission.objective_texts) {
    legacy_mission_objective_texts_ = mission.objective_texts;
  }
  if (legacy_mission_parameter_texts_ != mission.parameter_texts) {
    legacy_mission_parameter_texts_ = mission.parameter_texts;
  }
  legacy_completed_objectives_ = mission.completed_objectives;
  legacy_failed_objectives_ = mission.failed_objectives;
  legacy_revealed_objectives_ = mission.revealed_objectives;
  legacy_notified_objectives_ = mission.notified_objectives;
  legacy_failed_parameters_ = mission.failed_parameters;
  legacy_parameter_mask_ = mission.parameter_mask;

  auto vitals = hud_.vitals();
  vitals.health = static_cast<std::uint16_t>(std::clamp<int>(
      mission.player_health, 0, std::numeric_limits<std::uint16_t>::max()));
  vitals.armor = static_cast<std::uint16_t>(std::clamp<int>(
      mission.player_armor, 0, std::numeric_limits<std::uint16_t>::max()));
  vitals.maximum_health = std::max(vitals.maximum_health, vitals.health);
  vitals.maximum_armor = std::max(vitals.maximum_armor, vitals.armor);
  hud_.setVitals(vitals);

  const auto completed = mission.completed_objectives;
  legacy_mission_state_ = {};
  const auto combined_target_complete = (completed & 0x01U) != 0U;
  legacy_mission_state_.cbdc_protected = (completed & 0x02U) != 0U;
  legacy_mission_state_.security_bypassed = (completed & 0x04U) != 0U;
  legacy_mission_state_.upper_bomb_tagged = (completed & 0x08U) != 0U;
  legacy_mission_state_.kravitch_eliminated = combined_target_complete;
  legacy_mission_state_.communications_destroyed = combined_target_complete;
  legacy_mission_state_.initial_objectives_complete =
      (completed & 0x03U) == 0x03U;
  legacy_mission_state_.finale_started = mission.success || mission.terminal;
  legacy_mission_state_.failed = mission.failure;
  if (mission.failure || mission.failure_transition ||
      legacy_failure_restart_requested_) {
    mission_failed_ = true;
    mission_cinematic_phase_ = MissionCinematicPhase::gameplay;
    mission_cinematic_updates_ = 0U;
  } else if (mission.terminal && mission.success &&
             legacy_ending_movie_requested_) {
    mission_cinematic_phase_ = MissionCinematicPhase::complete;
    mission_cinematic_updates_ = 0U;
  }
  syncLegacyUiProjection(bridge, ui);
  if (!host_manual_aim_ && pending_host_aim_heading_restore_) {
    if (!pending_restore_owns_body) {
      pending_host_aim_heading_restore_.reset();
      host_manual_aim_body_heading_.reset();
      return;
    }
    // The release frame has already run. Writing now prevents retail's
    // just-finished L1 teardown from overwriting the preserved body yaw;
    // the current immutable presentation was overridden above and the
    // next guest frame starts from this narrow rotation-only repair.
    if (!legacy_first_mission_->restoreHostPlayerHeading(
            *pending_host_aim_heading_restore_)) {
      fail_bridge("host-heading-restore");
      return;
    }
    pending_host_aim_heading_restore_.reset();
    host_manual_aim_body_heading_.reset();
  }
}

void GameplaySession::syncLegacyUiProjection(
    const LegacyGameplayBridgeState &bridge, const LegacyUiCommandFrame &ui) {
  aim_target_.reset();
  locked_target_.reset();
  headshot_targeted_ = false;
  legacy_world_callouts_.clear();
  taser_target_.reset();
  taser_tether_updates_ = 0U;
  hud_.setDanger(0U);
  hud_.setTargetHealth(std::nullopt);

  if (!legacy_mission_bridge_active_) {
    return;
  }

  const auto &mission = ui.mission;
  const auto presentation_scene_for_guest =
      [this, &bridge](std::int16_t guest_slot) -> std::optional<std::uint16_t> {
    if (guest_slot < 0 ||
        static_cast<std::size_t>(guest_slot) >= bridge.objects.size()) {
      return std::nullopt;
    }
    const auto &guest = bridge.objects[static_cast<std::size_t>(guest_slot)];
    if (!guest.resident) {
      return std::nullopt;
    }
    const auto mapped =
        std::ranges::find(legacy_guest_slot_by_scene_object_,
                          static_cast<std::int32_t>(guest_slot));
    if (mapped == legacy_guest_slot_by_scene_object_.end()) {
      return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(
        std::distance(legacy_guest_slot_by_scene_object_.begin(), mapped));
    if (index >= objects_.size() || index >= object_script_hidden_.size() ||
        object_script_hidden_[index] ||
        std::ranges::find(active_objects_, static_cast<std::uint16_t>(index)) ==
            active_objects_.end()) {
      return std::nullopt;
    }
    return static_cast<std::uint16_t>(index);
  };
  const auto scene_for_guest =
      [&bridge, &presentation_scene_for_guest](
          std::int16_t guest_slot) -> std::optional<std::uint16_t> {
    const auto scene = presentation_scene_for_guest(guest_slot);
    if (!scene || guest_slot < 0 ||
        static_cast<std::size_t>(guest_slot) >= bridge.objects.size() ||
        bridge.objects[static_cast<std::size_t>(guest_slot)].health <= 0) {
      return std::nullopt;
    }
    return scene;
  };

  if (bridge.taserConductorActive()) {
    if (const auto scene = scene_for_guest(bridge.taser_target_slot)) {
      // The guest advances the complete taser state machine. Keep the
      // existing native wire presentation alive for this exact sample.
      taser_target_ = scene;
      taser_tether_updates_ = 1U;
    }
  }

  for (const auto &callout : ui.world_callouts) {
    const auto scene = presentation_scene_for_guest(callout.guest_slot);
    if (!scene || callout.text.empty()) {
      continue;
    }
    legacy_world_callouts_.push_back(
        LegacyWorldCallout{*scene, callout.text, callout.headshot});
  }

  if (const auto scene = scene_for_guest(ui.target.aimed_target_slot)) {
    aim_target_ = scene;
    headshot_targeted_ = ui.target.hit_result != 0U && ui.target.headshot;
  }

  if (ui.target.active && (ui.target.target_flags & 0x01U) != 0U) {
    if (const auto scene = scene_for_guest(ui.target.target_slot)) {
      // A first-person ray hit and the retail R1 lock are independent.
      // Once lock-on is active its selected actor owns the frame anchor;
      // retaining the incidental ray hit makes the reticle slide onto a
      // different object while the camera correctly follows the enemy.
      aim_target_ = scene;
      locked_target_ = scene;
      headshot_targeted_ = headshot_targeted_ &&
                           ui.target.target_slot == ui.target.aimed_target_slot;
      hud_.setTargetHealth(static_cast<std::uint8_t>(
          std::clamp<int>(ui.target.target_meter, 0, 100)));
    }
  }

  // ui.threats is copied directly from the retail tracked-target list.  Do
  // not gate it through the native scene/NPC mirrors: streaming and scripted
  // actors make those mirrors intentionally incomplete on several missions.
  const auto danger =
      legacyRetailDangerPercent(ui.threats, mission.player_slot);
  hud_.setDanger(danger, danger == 100U);
}

bool GameplaySession::queueLegacyDamage(std::uint16_t scene_object,
                                        std::uint16_t damage,
                                        WeaponDamageKind kind,
                                        bool headshot) noexcept {
  static_cast<void>(scene_object);
  static_cast<void>(damage);
  static_cast<void>(kind);
  static_cast<void>(headshot);
  // H4 removed host-authored combat injection. Physical fire input reaches
  // the retail combat code through PAD; the host never queues damage.
  return false;
}

void GameplaySession::syncLegacyOpeningBridge(
    const LegacyGameplayBridgeState &bridge, bool fresh_guest_sample) {
  if (mission_.definition().index != 0U) {
    return;
  }
  std::array<bool, 2U> cbdc_seen{};
  std::array<bool, 2U> terrorist_seen{};
  if (opening_terrorist_source < bridge.dynamic_first_slot &&
      opening_terrorist_source < bridge.objects.size()) {
    const auto &guest = bridge.objects[opening_terrorist_source];
    const auto presented =
        guest.resident && (guest.presentation_controller == 0U ||
                           guest.presentation_enabled != 0U);
    if (presented) {
      terrorist_seen[0] = true;
      legacy_opening_terrorist_seen_[0] = true;
    }
  }
  for (std::uint32_t guest_slot = bridge.dynamic_first_slot;
       guest_slot < bridge.objects.size(); ++guest_slot) {
    const auto &guest = bridge.objects[guest_slot];
    const auto scene_object =
        openingSceneObjectForGuestActor(guest, bridge.dynamic_first_slot);
    if (!scene_object || *scene_object >= npc_states_.size()) {
      continue;
    }
    if (*scene_object < legacy_guest_slot_by_scene_object_.size()) {
      legacy_guest_slot_by_scene_object_[*scene_object] =
          static_cast<std::int32_t>(guest.slot);
    }

    auto &state = npc_states_[*scene_object];
    syncLegacyGroundContact(state, guest);
    const auto lane = static_cast<std::size_t>(state.scripted_opening_lane);
    if (lane >= opening_encounter_lanes.size()) {
      continue;
    }
    if (state.scripted_intro_agent) {
      cbdc_seen[lane] = true;
      legacy_opening_cbdc_seen_[lane] = true;
      if (legacy_opening_cbdc_guest_slots_[lane] ==
          opening_guest_slot_unbound) {
        legacy_opening_cbdc_guest_slots_[lane] =
            static_cast<std::int32_t>(guest.slot);
        legacy_opening_cbdc_guest_identities_[lane] =
            legacyGuestIdentity(guest);
      }
    } else {
      terrorist_seen[lane] = true;
      legacy_opening_terrorist_seen_[lane] = true;
      if (legacy_opening_terrorist_guest_slots_[lane] ==
          opening_guest_slot_unbound) {
        legacy_opening_terrorist_guest_slots_[lane] =
            static_cast<std::int32_t>(guest.slot);
        legacy_opening_terrorist_guest_identities_[lane] =
            legacyGuestIdentity(guest);
      }
    }

    const auto *actor_hmd =
        objects_[*scene_object].model < object_models_.size()
            ? std::get_if<assets::HmdModel>(
                  &object_models_[objects_[*scene_object].model].geometry)
            : nullptr;
    const auto has_complete_guest_pose =
        actor_hmd != nullptr &&
        legacyGuestHmdPoseComplete(guest.bone_matrix_count,
                                   actor_hmd->parts().size());
    const auto presented =
        guest.resident &&
        (guest.presentation_controller == 0U ||
         guest.presentation_enabled != 0U) &&
        (guest.instance_state[3] & legacy_instance_dormant) == 0U &&
        has_complete_guest_pose;
    if (!presented) {
      resetLegacyBridgedPresentation(state);
      state.active = false;
      state.health = static_cast<std::uint16_t>(std::max<int>(guest.health, 0));
      object_health_[*scene_object] = state.health;
      object_script_hidden_[*scene_object] = true;
      std::erase(active_objects_, *scene_object);
      continue;
    }

    const auto previous_x = state.x;
    const auto previous_z = state.z;
    state.active = true;
    state.health = static_cast<std::uint16_t>(std::clamp<int>(
        guest.health, 0, std::numeric_limits<std::uint16_t>::max()));
    state.maximum_health = static_cast<std::uint16_t>(
        std::clamp<int>(std::max<int>(guest.maximum_health, guest.health), 1,
                        std::numeric_limits<std::uint16_t>::max()));
    state.x = static_cast<double>(guest.position.x);
    state.y = static_cast<double>(guest.position.y);
    state.z = static_cast<double>(guest.position.z);
    const auto root_matches_authored_position =
        guest.position.x == guest.authored_position.x &&
        guest.position.y == guest.authored_position.y &&
        guest.position.z == guest.authored_position.z;
    const auto contact_space_fallback = legacyHmdFallbackUsesContactSpace(
        has_complete_guest_pose, guest.ground_contact_valid,
        root_matches_authored_position);
    if (contact_space_fallback) {
      state.y = static_cast<double>(guest.ground_contact_valid
                                        ? guest.ground_contact_y
                                        : guest.position.y);
    }
    state.home_x = state.x;
    state.home_y = state.y;
    state.home_z = state.z;
    state.yaw =
        headingFromDirection(static_cast<double>(guest.guest_rotation[2]),
                             static_cast<double>(guest.guest_rotation[8]));
    if (fresh_guest_sample) {
      state.movement_distance =
          std::sqrt((state.x - previous_x) * (state.x - previous_x) +
                    (state.z - previous_z) * (state.z - previous_z));
      const auto moving = state.movement_distance > 1.0;
      const auto low_pose = (guest.ai_route_flags & 0x0f00U) == 0x0400U;
      state.scripted_low_locomotion = moving && low_pose;
      state.scripted_kneeling = !moving && low_pose;
      setLegacyBridgedLocomotion(state, moving ? state.scripted_low_locomotion
                                                     ? NpcLocomotion::walk
                                                     : NpcLocomotion::run
                                               : NpcLocomotion::stationary);
    }
    const auto moving = state.movement_distance > 1.0;
    state.scripted_intro_spawned = true;
    state.scripted_opening_combat = true;
    state.scripted_ingress = false;
    setLegacyBridgedBehavior(state, legacyBridgedBehavior(guest, moving));
    syncLegacyActorCombatPresentation(state, guest, fresh_guest_sample);
    if (fresh_guest_sample) {
      state.scripted_wall_traversed = !state.scripted_climbing;
      state.scripted_opening_arrived = !moving && !state.scripted_climbing;
    }
    if (guest.has_target && guest.target_slot == 83) {
      state.last_known_player_x = player().x;
      state.last_known_player_z = player().z;
      state.alert_memory_updates = npc_alert_memory_updates;
    }

    syncLegacyHmdBones(objects_[*scene_object], guest, contact_space_fallback,
                       !has_complete_guest_pose);
    auto &transform = objects_[*scene_object].transform;
    transform.rotation = guest.guest_rotation;
    transform.x = guest.position.x;
    transform.y = static_cast<std::int32_t>(-std::lround(state.y));
    transform.z = guest.position.z;
    object_health_[*scene_object] = state.health;
    object_destroyed_[*scene_object] = false;
    object_script_hidden_[*scene_object] = false;
    if (std::ranges::find(active_objects_, *scene_object) ==
        active_objects_.end()) {
      active_objects_.push_back(*scene_object);
    }
    const auto &route = opening_encounter_lanes[lane];
    state.zone_min_x =
        std::min({state.x, route.hostile_midpoint.x, route.hostile_landing.x,
                  route.hostile_hold.x, player().x}) -
        native_stationary_actor_zone_padding;
    state.zone_max_x =
        std::max({state.x, route.hostile_midpoint.x, route.hostile_landing.x,
                  route.hostile_hold.x, player().x}) +
        native_stationary_actor_zone_padding;
    state.zone_min_z =
        std::min({state.z, route.hostile_midpoint.z, route.hostile_landing.z,
                  route.hostile_hold.z, player().z}) -
        native_stationary_actor_zone_padding;
    state.zone_max_z =
        std::max({state.z, route.hostile_midpoint.z, route.hostile_landing.z,
                  route.hostile_hold.z, player().z}) +
        native_stationary_actor_zone_padding;
  }

  const auto hide_actor = [this](std::uint16_t object) {
    if (object >= npc_states_.size()) {
      return;
    }
    resetLegacyBridgedPresentation(npc_states_[object]);
    npc_states_[object].active = false;
    object_script_hidden_[object] = true;
    objects_[object].legacy_hmd_bone_count = 0U;
    objects_[object].legacy_hmd_root_space = false;
    std::erase(active_objects_, object);
  };
  const auto binding_replaced = [&bridge](std::int32_t slot,
                                          std::uint64_t identity) {
    if (slot < 0) {
      return false;
    }
    if (static_cast<std::size_t>(slot) >= bridge.objects.size()) {
      return true;
    }
    const auto &guest = bridge.objects[static_cast<std::size_t>(slot)];
    return !legacyRetiredDynamicObject(guest) &&
           legacyGuestIdentity(guest) != identity;
  };
  for (std::size_t lane = 0U; lane < opening_encounter_lanes.size(); ++lane) {
    if (!cbdc_seen[lane]) {
      // A vacant pool record may later reload the same authored actor;
      // preserve that binding. A non-vacant different identity retires
      // it and owns its separate generic presentation slot.
      if (binding_replaced(legacy_opening_cbdc_guest_slots_[lane],
                           legacy_opening_cbdc_guest_identities_[lane])) {
        legacy_opening_cbdc_guest_slots_[lane] = opening_guest_slot_retired;
        legacy_opening_cbdc_guest_identities_[lane] = 0U;
      }
      hide_actor(opening_cbdc_objects_[lane]);
    }
    if (!terrorist_seen[lane]) {
      if (lane == 1U &&
          binding_replaced(legacy_opening_terrorist_guest_slots_[lane],
                           legacy_opening_terrorist_guest_identities_[lane])) {
        legacy_opening_terrorist_guest_slots_[lane] =
            opening_guest_slot_retired;
        legacy_opening_terrorist_guest_identities_[lane] = 0U;
      }
      hide_actor(opening_terrorist_objects_[lane]);
    }
  }
}

void GameplaySession::updateCinematic() {
  ++mission_cinematic_updates_;
  if (legacy_first_mission_ == nullptr) {
    legacy_runtime_faulted_ = true;
    mission_failed_ = true;
    return;
  }
  if (mission_cinematic_phase_ != MissionCinematicPhase::intro) {
    return;
  }

  if (legacy_first_mission_->ready() && !legacy_first_mission_->finished()) {
    legacy_first_mission_->advanceHostUpdate();
    syncLegacyGameplayBridge();
  }
  if (legacy_first_mission_->ready() &&
      legacy_first_mission_->openingFinished()) {
    mission_cinematic_phase_ = MissionCinematicPhase::gameplay;
    mission_cinematic_updates_ = 0U;
    camera_collision_initialized_ = false;
    updateCameraCollision();
    captureCheckpoint();
  }
}

void GameplaySession::activateNpc(std::uint16_t object) noexcept {
  if (object >= npc_states_.size() || object >= npc_spawn_states_.size()) {
    return;
  }
  object_script_hidden_[object] = false;
  npc_states_[object] = npc_spawn_states_[object];
  npc_states_[object].last_known_player_x = player().x;
  npc_states_[object].last_known_player_z = player().z;
  npc_states_[object].alert_memory_updates = npc_alert_memory_updates;
  setNpcBehavior(npc_states_[object], NpcBehavior::alert);
  object_health_[object] = npc_states_[object].maximum_health;
  object_destroyed_[object] = false;
  npc_damaged_[object] = false;
  updateNpcTransform(object);
}

void GameplaySession::respawnNpc(std::uint16_t object) noexcept {
  if (!mission_scripts_.repeatable(object)) {
    return;
  }
  activateNpc(object);
}

std::optional<NpcPatrolPoint>
GameplaySession::findNpcCover(const NpcState &state,
                              const PlayerState &player) const noexcept {
  const auto away_heading =
      headingFromDirection(state.x - player.x, state.z - player.z);
  constexpr std::array<std::int32_t, 8> angle_offsets{
      512, -512, 1024, -1024, 1536, -1536, 0, 2048,
  };
  constexpr std::array<double, 3> distances{280.0, 440.0, 620.0};
  for (const auto radius : distances) {
    for (const auto angle : angle_offsets) {
      const auto direction = headingDirection(away_heading + angle);
      const auto x = state.x + direction.x * radius;
      const auto z = state.z + direction.z * radius;
      const auto ground = findGround(x, z, state.y);
      if (ground.model >= models_.size() ||
          std::abs(ground.y - state.y) > maximum_ground_step ||
          collidesWithWall(x, ground.y, z)) {
        continue;
      }
      const auto hidden_from_player =
          traceWorldSegment(player.x, player.y - actor_target_height, player.z,
                            x, ground.y - actor_target_height, z) < 0.92;
      const auto reachable =
          traceWorldSegment(state.x, state.y - 60.0, state.z, x,
                            ground.y - 60.0, z) >= target_visibility_limit;
      if (hidden_from_player && reachable) {
        return NpcPatrolPoint{x, ground.y, z};
      }
    }
  }
  return std::nullopt;
}

void GameplaySession::updateNpcTransform(std::uint16_t object) noexcept {
  if (object >= objects_.size() || object >= npc_states_.size() ||
      !npc_states_[object].active) {
    return;
  }
  const auto &state = npc_states_[object];
  const auto basis = headingBasis(state.yaw);
  auto &transform = objects_[object].transform;
  transform.rotation = {
      fixedRotation(basis.right.x),
      0,
      fixedRotation(basis.forward.x),
      0,
      4096,
      0,
      fixedRotation(basis.right.z),
      0,
      fixedRotation(basis.forward.z),
  };
  transform.x = static_cast<std::int32_t>(std::lround(state.x));
  transform.y = static_cast<std::int32_t>(std::lround(-state.y));
  transform.z = static_cast<std::int32_t>(std::lround(state.z));
}

bool GameplaySession::isHostileActor(std::uint16_t object) const noexcept {
  return object < npc_states_.size() && npc_states_[object].active &&
         npc_states_[object].disposition == NpcDisposition::hostile;
}

bool GameplaySession::npcZoneContains(const NpcState &state, double x,
                                      double z) const noexcept {
  return x >= state.zone_min_x && x <= state.zone_max_x &&
         z >= state.zone_min_z && z <= state.zone_max_z;
}

void GameplaySession::update(const GameplayInput &input) {
  updateEffects();
  last_shot_ = {};
  if (legacy_first_mission_ == nullptr) {
    legacy_runtime_faulted_ = true;
    mission_failed_ = true;
    return;
  }

  const auto guest_weapon_before_update = hud_.inventory().current();
  stageNativeFirstPersonAim(input);
  stageLegacyHostState(input);
  const auto native_cinematic =
      mission_cinematic_phase_ == MissionCinematicPhase::intro ||
      mission_cinematic_phase_ == MissionCinematicPhase::finale;
  if (native_cinematic) {
    updateCinematic();
    aim_target_.reset();
    locked_target_.reset();
    headshot_targeted_ = false;
    last_shot_ = {};
    hud_.setDanger(0U);
    hud_.setTargetHealth(std::nullopt);
    hud_.update(HudInput{});
    return;
  }

  if (legacy_first_mission_->ready() && !legacy_first_mission_->finished()) {
    legacy_first_mission_->advanceHostUpdate();
    syncLegacyGameplayBridge();
  }
  if (legacyMissionAuthoritative() && checkpoint_pending_ && playerAlive()) {
    captureCheckpoint();
  }

  hud_.update(HudInput{
      .aiming = playerAim() == PlayerAimState::first_person,
  });
  if (pending_guest_weapon_menu_ || input.weapon_menu_delta != 0 ||
      input.next_weapon != input.previous_weapon) {
    hud_.showWeaponMenu();
  }
  if (hud_.inventory().current() != guest_weapon_before_update) {
    hud_.notifyWeaponChanged();
  }
}

const ObjectModel *GameplaySession::weaponModel(WeaponId id) const noexcept {
  if (!isValidWeaponId(id)) {
    return nullptr;
  }
  const auto model = weapon_models_[static_cast<std::size_t>(id)];
  return model && *model < object_models_.size() ? &object_models_[*model]
                                                 : nullptr;
}

const ObjectModel *
GameplaySession::displayedObjectModel(std::uint16_t index) const noexcept {
  if (index >= objects_.size() ||
      (index < object_script_hidden_.size() && object_script_hidden_[index])) {
    return nullptr;
  }
  const auto &object = objects_[index];
  if (!objectDestroyed(index) && !object.legacy_secondary_model_active) {
    return object.model < object_models_.size() ? &object_models_[object.model]
                                                : nullptr;
  }
  return object.destroyed_model &&
                 *object.destroyed_model < object_models_.size()
             ? &object_models_[*object.destroyed_model]
             : nullptr;
}

const NpcState *GameplaySession::npcState(std::uint16_t index) const noexcept {
  return index < npc_states_.size() && npc_states_[index].active
             ? &npc_states_[index]
             : nullptr;
}

bool GameplaySession::legacyDedicatedActorPresentation(
    std::uint16_t index) const noexcept {
  if (index >= legacy_dedicated_actor_presentations_.size() ||
      !legacy_dedicated_actor_presentations_[index] ||
      index >= objects_.size() ||
      (index < object_script_hidden_.size() && object_script_hidden_[index]) ||
      std::ranges::find(active_objects_, index) == active_objects_.end()) {
    return false;
  }
  const auto &object = objects_[index];
  return object.model < object_models_.size() &&
         std::holds_alternative<assets::HmdModel>(
             object_models_[object.model].geometry);
}

std::optional<WeaponId> GameplaySession::legacyDedicatedActorWeapon(
    std::uint16_t index) const noexcept {
  if (index >= legacy_dedicated_actor_weapons_.size() ||
      !legacy_dedicated_actor_weapons_[index] ||
      !legacyDedicatedActorPresentation(index)) {
    return std::nullopt;
  }
  if (weaponModel(*legacy_dedicated_actor_weapons_[index]) == nullptr) {
    return std::nullopt;
  }
  return legacy_dedicated_actor_weapons_[index];
}

std::uint8_t GameplaySession::textureBankAt(double x, double z) const noexcept {
  if (current_room_ >= models_.size()) {
    return 0U;
  }

  const auto current_bank = models_[current_room_].scene.textureBank();
  const auto current_contains = containsXZ(models_[current_room_].bounds, x, z);
  auto containing_bank_mask = std::uint8_t{};
  for (const auto model : active_models_) {
    if (model >= models_.size() || !containsXZ(models_[model].bounds, x, z)) {
      continue;
    }
    const auto bank = models_[model].scene.textureBank();
    if (bank < 2U) {
      containing_bank_mask |= static_cast<std::uint8_t>(1U << bank);
    }
  }
  return resolveTextureBankOwnership(current_bank, current_contains,
                                     containing_bank_mask);
}

std::uint8_t
GameplaySession::objectTextureBank(std::uint16_t index) const noexcept {
  if (current_room_ >= models_.size()) {
    return 0U;
  }
  const auto current_bank = models_[current_room_].scene.textureBank();
  if (index >= objects_.size()) {
    return current_bank;
  }

  const auto &object = objects_[index];
  if (const auto *state = npcState(index)) {
    return textureBankAt(state->x, state->z);
  }

  const auto sources = mission_.objects().objects();
  const auto source_valid = object.source_index < sources.size();
  const auto recycled = std::ranges::find(legacy_dynamic_objects_, index) !=
                        legacy_dynamic_objects_.end();
  const auto moved =
      source_valid &&
      (object.transform.x != sources[object.source_index].transform.x ||
       object.transform.z != sources[object.source_index].transform.z);
  if (!source_valid || recycled || moved) {
    return textureBankAt(static_cast<double>(object.transform.x),
                         static_cast<double>(object.transform.z));
  }

  auto active_owner_bank_mask = std::uint8_t{};
  auto spatial_owner_bank_mask = std::uint8_t{};
  auto current_is_owner = false;
  auto current_is_spatial_owner = false;
  const auto authored_owners =
      mission_.objects().roomsContainingObject(object.source_index);
  for (const auto room : authored_owners) {
    if (std::ranges::find(active_models_, room) == active_models_.end()) {
      continue;
    }
    if (room >= models_.size() || room >= mission_.objects().roomCount()) {
      continue;
    }
    current_is_owner |= room == current_room_;
    const auto bank = models_[room].scene.textureBank();
    if (bank < 2U) {
      active_owner_bank_mask |= static_cast<std::uint8_t>(1U << bank);
      if (containsXZ(models_[room].bounds, object.transform.x,
                     object.transform.z)) {
        spatial_owner_bank_mask |= static_cast<std::uint8_t>(1U << bank);
        current_is_spatial_owner |= room == current_room_;
      }
    }
  }
  // Portal display lists intentionally share static objects. The current DAT
  // may therefore name both banks while only one owner's bounds contains the
  // authored object. Prefer that spatial owner; selecting the camera room's
  // bank is what put unrelated metro textures on otherwise correct models.
  if (spatial_owner_bank_mask != 0U) {
    return resolveTextureBankOwnership(current_bank, current_is_spatial_owner,
                                       spatial_owner_bank_mask);
  }
  return resolveTextureBankOwnership(current_bank, current_is_owner,
                                     active_owner_bank_mask);
}

NpcAnimationRequest
GameplaySession::npcAnimation(std::uint16_t index) const noexcept {
  const auto *state = npcState(index);
  return state == nullptr ? NpcAnimationRequest{} : npcAnimationRequest(*state);
}

bool GameplaySession::canEquipWeapon(WeaponId id) const noexcept {
  if (!isValidWeaponId(id)) {
    return false;
  }
  const auto *state = hud_.inventory().tryState(id);
  if (state == nullptr || !state->owned) {
    return false;
  }
  if (legacy_first_mission_ == nullptr) {
    return true;
  }
  if (!legacyMissionAuthoritative()) {
    return false;
  }
  const auto *mission = legacy_first_mission_->missionBridge();
  const auto weapon_fsm_active =
      !pending_guest_weapon_requests_.empty() || pending_guest_weapon_ ||
      !pending_guest_weapon_steps_.empty() ||
      guest_weapon_in_flight_direction_ != 0 ||
      guest_weapon_in_flight_expected_ || guest_quick_weapon_pending_ ||
      mission == nullptr || mission->weapon_menu_state != -5 ||
      mission->weapon_menu_dirty;
  return !weapon_fsm_active &&
         retailWeaponStepsToTarget(hud_.inventory(), id, 1).has_value();
}

bool GameplaySession::equipWeapon(WeaponId id) noexcept {
  if (!canEquipWeapon(id)) {
    return false;
  }
  if (legacy_first_mission_ != nullptr) {
    if (hud_.inventory().current() == id) {
      return true;
    }
    pending_guest_weapon_requests_.push_back(GuestWeaponRequest{
        .direct_weapon = id,
    });
    return true;
  }
  if (hud_.inventory().current() == id) {
    return true;
  }
  if (!hud_.selectWeapon(id)) {
    return false;
  }
  pending_equipped_weapon_ = id;
  return true;
}

PlayerAnimationRequest GameplaySession::playerAnimation() const noexcept {
  if (mission_cinematic_phase_ == MissionCinematicPhase::intro) {
    return PlayerAnimationRequest{
        mission_cinematic_updates_ < 10U ? ActorMotion::kneel_down
                                         : ActorMotion::kneel,
        PlayerUpperAction::neutral,
        weaponStance(hud_.inventory().current()),
        hud_.inventory().current(),
    };
  }
  auto upper_action = PlayerUpperAction::neutral;
  switch (player_controller_.action()) {
  case PlayerActionState::firing:
    upper_action = PlayerUpperAction::fire;
    break;
  case PlayerActionState::reloading:
    upper_action = PlayerUpperAction::reload;
    break;
  case PlayerActionState::weapon_switching:
    upper_action = PlayerUpperAction::draw;
    break;
  default:
    if (playerAim() == PlayerAimState::first_person || locked_target_) {
      upper_action = PlayerUpperAction::aim;
    }
    break;
  }
  return PlayerAnimationRequest{
      player_controller_.actorMotion(),
      upper_action,
      weaponStance(hud_.inventory().current()),
      hud_.inventory().current(),
  };
}

PlayerAimState GameplaySession::playerAim() const noexcept {
  if (legacy_first_mission_ != nullptr && legacy_first_mission_->ready() &&
      legacy_first_mission_->bridge()) {
    const auto &bridge = *legacy_first_mission_->bridge();
    // Camera mode 1 is not an aim discriminator: retail also publishes it
    // during the exact ledge/hang presentation (5/18). Hiding Gabe therefore
    // requires the host aim action as well as the native first-person state.
    return legacyManualAimPresentationActive(
               host_manual_aim_,
               player_controller_.aim() == PlayerAimState::first_person,
               bridge.camera.mode, bridge.player.control_locked,
               bridge.camera.scripted, bridge.camera.locked)
               ? PlayerAimState::first_person
               : PlayerAimState::chase;
  }
  return player_controller_.aim();
}

void GameplaySession::updateCameraCollision() noexcept {
  const auto desired = player_controller_.camera();
  const auto &player = player_controller_.state();
  const auto mode = player_controller_.cameraIntent().mode;
  const auto first_person = mode == PlayerCameraMode::first_person_aim;
  const Point3 anchor =
      first_person
          ? Point3{player.x, desired.y, player.z}
          : Point3{desired.target_x, desired.target_y, desired.target_z};
  const auto distance =
      std::sqrt((desired.x - anchor.x) * (desired.x - anchor.x) +
                (desired.y - anchor.y) * (desired.y - anchor.y) +
                (desired.z - anchor.z) * (desired.z - anchor.z));
  if (distance <= 0.0001) {
    camera_state_ = desired;
    camera_collision_initialized_ = false;
    return;
  }
  const auto hit = traceWorldSegment(anchor.x, anchor.y, anchor.z, desired.x,
                                     desired.y, desired.z);
  const auto minimum_distance =
      std::min(minimum_camera_view_distance, distance);
  const auto unobstructed_distance =
      hit >= 1.0 ? distance
                 : std::clamp(hit * distance - camera_wall_clearance,
                              minimum_distance, distance);
  if (!camera_collision_initialized_ || mode != camera_mode_ || first_person) {
    camera_collision_distance_ = unobstructed_distance;
  } else if (unobstructed_distance < camera_collision_distance_) {
    camera_collision_distance_ = unobstructed_distance;
  } else {
    camera_collision_distance_ =
        std::min(unobstructed_distance,
                 camera_collision_distance_ + camera_release_per_update);
  }
  const auto amount = camera_collision_distance_ / distance;
  camera_state_ = desired;
  camera_state_.x = anchor.x + (desired.x - anchor.x) * amount;
  camera_state_.y = anchor.y + (desired.y - anchor.y) * amount;
  camera_state_.z = anchor.z + (desired.z - anchor.z) * amount;
  camera_mode_ = mode;
  camera_collision_initialized_ = true;
}

CameraState GameplaySession::camera() const noexcept {
  if (legacy_first_mission_ != nullptr && legacy_first_mission_->ready() &&
      legacy_first_mission_->bridge()) {
    const auto &bridge = *legacy_first_mission_->bridge();
    const auto &camera = bridge.camera;
    if (legacyManualAimPresentationActive(
            host_manual_aim_,
            player_controller_.aim() == PlayerAimState::first_person,
            camera.mode, bridge.player.control_locked, camera.scripted,
            camera.locked)) {
      auto native = player_controller_.camera();
      if (std::abs(host_manual_aim_strafe_) > 0.0001 &&
          legacy_manual_aim_neutral_camera_) {
        const auto &neutral = *legacy_manual_aim_neutral_camera_;
        const auto &root = bridge.player.position;
        const auto &neutral_root = legacy_manual_aim_neutral_player_root_;
        const auto basis = headingBasis(player_controller_.state().yaw);
        const auto lateral_delta =
            [right_x = basis.right.x,
             right_z = basis.right.z](const LegacyNativePoint &current,
                                      const LegacyNativePoint &current_root,
                                      const LegacyNativePoint &center,
                                      const LegacyNativePoint &center_root) {
              const auto x = static_cast<double>(current.x - current_root.x -
                                                 center.x + center_root.x);
              const auto z = static_cast<double>(current.z - current_root.z -
                                                 center.z + center_root.z);
              return x * right_x + z * right_z;
            };
        const auto eye_lateral =
            lateral_delta(camera.eye, root, neutral.eye, neutral_root);
        const auto target_lateral =
            lateral_delta(camera.target, root, neutral.target, neutral_root);
        const auto lateral = (eye_lateral + target_lateral) * 0.5;
        native.x += basis.right.x * lateral;
        native.z += basis.right.z * lateral;
        native.target_x += basis.right.x * lateral;
        native.target_z += basis.right.z * lateral;
      }
      // FirstPersonCamera intentionally moves the eye forward from Gabe's
      // collision root. The legacy-authoritative return path used to bypass
      // camera_state_'s wall constraint, allowing that eye to cross a thin
      // wall even though Gabe himself remained blocked. Clamp the final eye
      // (including retail strafe) and translate the target by the same amount
      // so aim direction and the retail shot ray stay identical.
      const auto &player = player_controller_.state();
      const Point3 eye_anchor{player.x, native.y, player.z};
      const auto eye_delta_x = native.x - eye_anchor.x;
      const auto eye_delta_y = native.y - eye_anchor.y;
      const auto eye_delta_z = native.z - eye_anchor.z;
      const auto eye_distance =
          std::sqrt(eye_delta_x * eye_delta_x + eye_delta_y * eye_delta_y +
                    eye_delta_z * eye_delta_z);
      if (eye_distance > 0.0001) {
        const auto hit =
            traceWorldSegment(eye_anchor.x, eye_anchor.y, eye_anchor.z,
                              native.x, native.y, native.z);
        if (hit < 1.0) {
          const auto distance = std::clamp(
              hit * eye_distance - camera_wall_clearance, 0.0, eye_distance);
          const auto amount = distance / eye_distance;
          const auto old_x = native.x;
          const auto old_y = native.y;
          const auto old_z = native.z;
          native.x = eye_anchor.x + eye_delta_x * amount;
          native.y = eye_anchor.y + eye_delta_y * amount;
          native.z = eye_anchor.z + eye_delta_z * amount;
          native.target_x += native.x - old_x;
          native.target_y += native.y - old_y;
          native.target_z += native.z - old_z;
        }
      }
      const auto sight_x = native.target_x - native.x;
      const auto sight_y = native.target_y - native.y;
      const auto sight_z = native.target_z - native.z;
      const auto sight_length =
          std::sqrt(sight_x * sight_x + sight_y * sight_y + sight_z * sight_z);
      if (sight_length > 0.0001) {
        const auto direction_x = sight_x / sight_length;
        const auto direction_y = sight_y / sight_length;
        const auto direction_z = sight_z / sight_length;
        const auto guard_x =
            native.x + direction_x * first_person_camera_near_clearance;
        const auto guard_y =
            native.y + direction_y * first_person_camera_near_clearance;
        const auto guard_z =
            native.z + direction_z * first_person_camera_near_clearance;
        const auto hit = traceWorldSegment(native.x, native.y, native.z,
                                           guard_x, guard_y, guard_z);
        if (hit < 1.0) {
          const auto retreat =
              first_person_camera_near_clearance * (1.0 - hit) + 2.0;
          native.x -= direction_x * retreat;
          native.y -= direction_y * retreat;
          native.z -= direction_z * retreat;
          native.target_x -= direction_x * retreat;
          native.target_y -= direction_y * retreat;
          native.target_z -= direction_z * retreat;
        }
      }
      // Mode 2 renders through the independent DAT_8013c730 camera, while
      // mode 3 animates the primary controller. The world bridge necessarily
      // exposes the latter camera, so take the optic's exact live FOV channel
      // from the mission bridge for both sniper variants. This preserves the
      // retail Triangle/Circle animation instead of freezing native view at
      // the pre-scope projection.
      const auto *mission = legacy_first_mission_->missionBridge();
      if (mission != nullptr &&
          (mission->first_person_aim_mode == 2U ||
           mission->first_person_aim_mode == 3U) &&
          mission->scope_zoom_raw > 0 && mission->scope_zoom_raw < 2048) {
        auto optic_camera = camera;
        optic_camera.fov_raw = mission->scope_zoom_raw;
        native.projection = optic_camera.projectionForDisplayWidth(384);
      } else {
        native.projection = camera.projectionForDisplayWidth(384);
      }
      return native;
    }
    return CameraState{
        static_cast<double>(camera.eye.x),
        static_cast<double>(camera.eye.y),
        static_cast<double>(camera.eye.z),
        static_cast<double>(camera.target.x),
        static_cast<double>(camera.target.y),
        static_cast<double>(camera.target.z),
        camera.projectionForDisplayWidth(384),
    };
  }
  if (mission_cinematic_phase_ == MissionCinematicPhase::intro) {
    if (legacy_first_mission_ != nullptr && legacy_first_mission_->ready() &&
        legacy_first_mission_->bridge()) {
      const auto &camera = legacy_first_mission_->bridge()->camera;
      return CameraState{
          static_cast<double>(camera.eye.x),
          static_cast<double>(camera.eye.y),
          static_cast<double>(camera.eye.z),
          static_cast<double>(camera.target.x),
          static_cast<double>(camera.target.y),
          static_cast<double>(camera.target.z),
          camera.projectionForDisplayWidth(384),
      };
    }
    const auto sampled = opening_camera_.sample(mission_cinematic_updates_);
    return CameraState{
        sampled.x,        sampled.y,        sampled.z,
        sampled.target_x, sampled.target_y, sampled.target_z,
    };
  }
  if (legacyScriptedCameraActive()) {
    const auto &camera = legacy_first_mission_->bridge()->camera;
    return CameraState{
        static_cast<double>(camera.eye.x),
        static_cast<double>(camera.eye.y),
        static_cast<double>(camera.eye.z),
        static_cast<double>(camera.target.x),
        static_cast<double>(camera.target.y),
        static_cast<double>(camera.target.z),
        camera.projectionForDisplayWidth(384),
    };
  }
  if (mission_cinematic_phase_ == MissionCinematicPhase::finale ||
      mission_cinematic_phase_ == MissionCinematicPhase::complete) {
    constexpr double bomb_x = -309.0;
    constexpr double bomb_y = 1118.0;
    constexpr double bomb_z = 426.0;
    const auto progress =
        mission_cinematic_phase_ == MissionCinematicPhase::complete
            ? 1.0
            : std::clamp(static_cast<double>(mission_cinematic_updates_) / 70.0,
                         0.0, 1.0);
    const auto angle = progress * 2.35;
    const auto radius = 1750.0 - progress * 450.0;
    return CameraState{
        bomb_x + std::cos(angle) * radius,
        bomb_y - 920.0,
        bomb_z + std::sin(angle) * radius,
        bomb_x,
        bomb_y - 160.0,
        bomb_z,
    };
  }
  return camera_state_;
}

double GameplaySession::manualAimReticleVerticalOffset() const noexcept {
  const auto weapon = hud_.inventory().current();
  return weapon == WeaponId::nightvision_rifle ||
                 weapon == WeaponId::sniper_rifle
             ? 0.0
             : manual_aim_reticle_vertical_offset;
}

std::uint8_t GameplaySession::mapFade() const noexcept {
  if (legacy_runtime_faulted_) {
    return 0xffU;
  }
  if (legacy_first_mission_ != nullptr && legacy_first_mission_->ready() &&
      legacy_first_mission_->bridge()) {
    const auto &fade = legacy_first_mission_->bridge()->fade;
    constexpr auto opening_profile =
        syphonFilterUsaV11FirstMissionOpeningProfile();
    // Native loading and mission-start screens have already completed before
    // the world renderer takes ownership. Retail starts that first world
    // frame under an almost opaque subtractive fade (248/255); replaying it
    // hides a valid PC frame. Suppress only this identified opening callback,
    // while leaving failure, checkpoint and scripted transition fades exact.
    if (fade.callback == opening_profile.fade_completion_callback &&
        fade.step < 0) {
      return 0U;
    }
    const auto intensity = std::lround(fade.blackOpacity() * 255.0);
    return static_cast<std::uint8_t>(std::clamp(intensity, 0L, 255L));
  }
  return map_fade_.intensity();
}

std::string_view GameplaySession::runtimeFaultReason() const noexcept {
  if (!legacy_runtime_faulted_) {
    return "none";
  }
  if (legacy_first_mission_ == nullptr) {
    return "missing-guest-runtime";
  }
  if (legacy_first_mission_->faultReason() ==
      LegacyRuntimeFaultReason::renderer_bridge) {
    return legacyGameplayBridgeReadFaultName(
        legacy_first_mission_->rendererBridgeFault());
  }
  if (legacy_first_mission_->faulted()) {
    return legacyRuntimeFaultReasonName(legacy_first_mission_->faultReason());
  }
  return "gameplay-presentation-contract";
}

std::string_view GameplaySession::runtimeFaultDetail() const noexcept {
  if (!legacy_runtime_faulted_ || legacy_first_mission_ == nullptr) {
    return "none";
  }
  if (legacy_first_mission_->faultReason() ==
      LegacyRuntimeFaultReason::execution) {
    return legacy_first_mission_->faultDetail();
  }
  if (legacy_first_mission_->faultReason() !=
      LegacyRuntimeFaultReason::renderer_bridge) {
    return legacy_first_mission_->faulted() ? std::string_view{"none"}
                                            : legacy_presentation_fault_detail_;
  }
  return legacyGameplayBridgeReadStageName(
      legacy_first_mission_->rendererBridgeStage());
}

} // namespace sf::game
