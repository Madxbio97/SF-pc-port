#include "sf/game/dynamic_lighting.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <utility>

namespace sf::game {
namespace {

struct LightProfile {
  DynamicLightRgb color;
  double radius;
  double intensity;
};

[[nodiscard]] constexpr LightProfile profile(DynamicLightKind kind) noexcept {
  switch (kind) {
  case DynamicLightKind::street_lamp:
    return {{1.0, 0.82, 0.58}, 1400.0, 0.18};
  case DynamicLightKind::police_lightbar:
    return {{0.48, 0.58, 1.0}, 760.0, 0.10};
  case DynamicLightKind::steady_fire:
    return {{1.0, 0.36, 0.08}, 920.0, 0.16};
  case DynamicLightKind::muzzle_flash:
    // Retail weapon flashes briefly saturate nearby floor and wall vertices.
    // Keep this a single accepted-shot frame, but preserve that strong warm
    // pulse instead of the former barely visible ambient lift.
    return {{1.0, 0.78, 0.42}, 1050.0, 0.78};
  case DynamicLightKind::explosion:
    return {{1.0, 0.43, 0.10}, 2200.0, 0.34};
  case DynamicLightKind::flashlight:
    // The flashlight is a bounded vertex-light volume, not visible beam
    // geometry. Keep all channels neutral so authored material color is
    // brightened without the former blue-grey cast.
    return {{1.0, 1.0, 1.0}, 2800.0, 0.55};
  }
  return {};
}

[[nodiscard]] std::uint32_t mixBits(std::uint32_t value) noexcept {
  value ^= value >> 16U;
  value *= 0x7feb352dU;
  value ^= value >> 15U;
  value *= 0x846ca68bU;
  return value ^ (value >> 16U);
}

[[nodiscard]] LightProfile animatedProfile(DynamicLightKind kind,
                                           std::uint32_t source_id,
                                           std::uint64_t tick) noexcept {
  auto result = profile(kind);
  if (kind == DynamicLightKind::police_lightbar) {
    // Use the same two-update cadence as the retail lightbar texture copies.
    // Alternate complete red/blue pulses instead of adding a permanent blue
    // ambient wash around every police vehicle.
    const auto phase = ((tick / 2U) + source_id) & 3U;
    const auto red_phase = phase == 1U || phase == 2U;
    result.color = red_phase ? DynamicLightRgb{1.0, 0.10, 0.06}
                             : DynamicLightRgb{0.08, 0.22, 1.0};
    result.intensity = phase == 0U || phase == 2U ? 0.22 : 0.13;
  } else if (kind == DynamicLightKind::steady_fire) {
    // A deterministic 20 Hz flicker follows guest time, so rendering the same
    // simulation frame at 30, 60 or 240 Hz produces the exact same light.
    const auto phase = static_cast<std::uint32_t>(tick / 2U);
    const auto noise = mixBits(source_id ^ (phase * 0x9e3779b9U));
    const auto unit = static_cast<double>(noise & 0xffffU) / 65535.0;
    const auto pulse = 0.78 + unit * 0.34;
    result.intensity *= pulse;
    result.radius *= 0.92 + unit * 0.12;
  }
  return result;
}

[[nodiscard]] bool finite(DynamicLightPoint point) noexcept {
  return std::isfinite(point.x) && std::isfinite(point.y) &&
         std::isfinite(point.z);
}

[[nodiscard]] double squaredDistance(DynamicLightPoint first,
                                     DynamicLightPoint second) noexcept {
  const auto x = first.x - second.x;
  const auto y = first.y - second.y;
  const auto z = first.z - second.z;
  return x * x + y * y + z * z;
}

[[nodiscard]] bool persistentKind(DynamicLightKind kind) noexcept {
  return kind == DynamicLightKind::street_lamp ||
         kind == DynamicLightKind::police_lightbar ||
         kind == DynamicLightKind::steady_fire;
}

[[nodiscard]] std::optional<DynamicLight>
makePersistent(const PersistentDynamicLightState &source,
               std::uint64_t animation_tick) noexcept {
  if (!source.identity_confirmed || !source.active || !source.resident ||
      source.destroyed || !persistentKind(source.kind) ||
      !finite(source.position)) {
    return std::nullopt;
  }
  const auto light_profile =
      animatedProfile(source.kind, source.source_id, animation_tick);
  return DynamicLight{source.kind,
                      source.position,
                      light_profile.color,
                      light_profile.radius,
                      light_profile.intensity,
                      {},
                      -1.0,
                      -1.0,
                      source.source_id,
                      false,
                      false};
}

[[nodiscard]] std::optional<DynamicLight>
makeTransient(const TransientDynamicLightState &source) noexcept {
  if (!source.position_confirmed || !finite(source.position) ||
      !std::isfinite(source.scale) || source.scale <= 0.0 ||
      source.total_updates == 0U || source.remaining_updates == 0U ||
      source.remaining_updates > source.total_updates) {
    return std::nullopt;
  }

  auto kind = DynamicLightKind::muzzle_flash;
  switch (source.effect_type) {
  case GameplayEffectType::muzzle_flash:
    kind = DynamicLightKind::muzzle_flash;
    break;
  case GameplayEffectType::explosion:
    kind = DynamicLightKind::explosion;
    break;
  case GameplayEffectType::burning_fire:
    kind = DynamicLightKind::steady_fire;
    break;
  case GameplayEffectType::blood_spray:
  case GameplayEffectType::blood_decal:
    return std::nullopt;
  }

  const auto light_profile = profile(kind);
  const auto scale = std::clamp(source.scale, 0.25, 4.0);
  const auto lifetime = static_cast<double>(source.remaining_updates) /
                        static_cast<double>(source.total_updates);
  const auto fade = kind == DynamicLightKind::steady_fire
                        ? 1.0
                        : std::clamp(lifetime, 0.0, 1.0);
  return DynamicLight{kind,
                      source.position,
                      light_profile.color,
                      light_profile.radius * std::sqrt(scale),
                      light_profile.intensity * std::sqrt(scale) * fade,
                      {},
                      -1.0,
                      -1.0,
                      source.source_id,
                      true,
                      false};
}

[[nodiscard]] std::optional<DynamicLight>
makeDirectional(const DirectionalDynamicLightState &source) noexcept {
  if (source.kind != DynamicLightKind::flashlight ||
      !source.identity_confirmed || !source.enabled ||
      !finite(source.position) || !finite(source.direction)) {
    return std::nullopt;
  }
  const auto length = std::sqrt(source.direction.x * source.direction.x +
                                source.direction.y * source.direction.y +
                                source.direction.z * source.direction.z);
  if (!std::isfinite(length) || length <= 0.000001) {
    return std::nullopt;
  }
  const auto light_profile = profile(source.kind);
  return DynamicLight{source.kind,
                      source.position,
                      light_profile.color,
                      light_profile.radius,
                      light_profile.intensity,
                      {source.direction.x / length, source.direction.y / length,
                       source.direction.z / length},
                      0.9612616959,
                      0.8829475929,
                      source.source_id,
                      false,
                      true};
}

struct SelectedLight {
  DynamicLight light;
  double observer_distance_squared{};
  std::size_t source_order{};
};

[[nodiscard]] int selectionPriority(const DynamicLight &light) noexcept {
  if (light.transient) {
    return 3;
  }
  if (light.directional) {
    return 2;
  }
  return 1;
}

[[nodiscard]] bool better(const SelectedLight &candidate,
                          const SelectedLight &resident) noexcept {
  const auto candidate_priority = selectionPriority(candidate.light);
  const auto resident_priority = selectionPriority(resident.light);
  if (candidate_priority != resident_priority) {
    return candidate_priority > resident_priority;
  }
  if (candidate.observer_distance_squared !=
      resident.observer_distance_squared) {
    return candidate.observer_distance_squared <
           resident.observer_distance_squared;
  }
  return candidate.source_order < resident.source_order;
}

void consider(std::array<SelectedLight, maximum_dynamic_lights> &selected,
              std::size_t &count, DynamicLight light,
              DynamicLightPoint observer, std::size_t source_order) noexcept {
  const auto candidate = SelectedLight{
      light, squaredDistance(light.position, observer), source_order};
  if (count < selected.size()) {
    selected[count++] = candidate;
    return;
  }
  auto worst = std::size_t{};
  for (std::size_t index = 1U; index < count; ++index) {
    if (better(selected[worst], selected[index])) {
      worst = index;
    }
  }
  if (better(candidate, selected[worst])) {
    selected[worst] = candidate;
  }
}

[[nodiscard]] std::int32_t arithmeticShiftRight(std::int32_t value,
                                                unsigned shift) noexcept {
  if (shift == 0U) {
    return value;
  }
  const auto bits = std::bit_cast<std::uint32_t>(value);
  auto shifted = bits >> shift;
  if (value < 0) {
    shifted |= ~std::uint32_t{} << (32U - shift);
  }
  return std::bit_cast<std::int32_t>(shifted);
}

[[nodiscard]] std::int64_t arithmeticShiftRight(std::int64_t value,
                                                unsigned shift) noexcept {
  if (shift == 0U) {
    return value;
  }
  const auto bits = std::bit_cast<std::uint64_t>(value);
  auto shifted = bits >> shift;
  if (value < 0) {
    shifted |= ~std::uint64_t{} << (64U - shift);
  }
  return std::bit_cast<std::int64_t>(shifted);
}

[[nodiscard]] std::int64_t absolute(std::int32_t value) noexcept {
  return value < 0 ? -static_cast<std::int64_t>(value)
                   : static_cast<std::int64_t>(value);
}

[[nodiscard]] std::int32_t lowSignedWord(std::int64_t value) noexcept {
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

template <typename Integer>
[[nodiscard]] std::int16_t lowSignedHalf(Integer value) noexcept {
  return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}

[[nodiscard]] std::int64_t signExtend44(std::int64_t value) noexcept {
  constexpr auto mask = (std::uint64_t{1} << 44U) - 1U;
  auto truncated = static_cast<std::uint64_t>(value) & mask;
  if ((truncated & (std::uint64_t{1} << 43U)) != 0U) {
    truncated |= ~mask;
  }
  return std::bit_cast<std::int64_t>(truncated);
}

[[nodiscard]] std::int16_t wrappedNegate(std::int16_t value) noexcept {
  const auto bits = static_cast<std::uint16_t>(value);
  return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(0U - bits));
}

// Exact PS1 GTE UNR divider used by RTPS/RTPT. The quotient is unsigned 17.16
// and saturates when H is at least twice SZ.
[[nodiscard]] std::uint32_t
divideRetailPerspective(std::uint16_t numerator,
                        std::uint16_t denominator) noexcept {
  if (static_cast<std::uint32_t>(denominator) * 2U <= numerator) {
    return 0x1ffffU;
  }

  const auto shift = static_cast<std::uint32_t>(std::countl_zero(denominator));
  auto normalized_numerator = static_cast<std::uint32_t>(numerator) << shift;
  auto normalized_denominator = static_cast<std::uint32_t>(denominator)
                                << shift;
  const auto index = ((normalized_denominator & 0x7fffU) + 0x40U) >> 7U;
  const auto table_value = std::clamp(
      ((0x40000 / static_cast<int>(index + 0x100U) + 1) / 2) - 0x101, 0, 0xff);
  const auto estimate = 0x101 + table_value;
  const auto delta =
      (static_cast<std::int32_t>(normalized_denominator) * -estimate + 0x80) >>
      8U;
  const auto reciprocal = (estimate * (0x20000 + delta) + 0x80) >> 8U;
  const auto result = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(normalized_numerator) *
           static_cast<std::uint32_t>(reciprocal) +
       0x8000U) >>
      16U);
  return std::min(0x1ffffU, result);
}

[[nodiscard]] std::array<std::int32_t, 3U>
applyRetailMatrixLong(const std::array<std::int16_t, 9U> &rotation,
                      const std::array<std::int32_t, 3U> &vector) noexcept {
  std::array<std::int16_t, 3U> high{};
  std::array<std::int16_t, 3U> remainder{};
  for (std::size_t component = 0U; component < 3U; ++component) {
    const auto high_word = vector[component] / 0x8000;
    const auto low_word = vector[component] - high_word * 0x8000;
    high[component] = lowSignedHalf(high_word);
    remainder[component] = lowSignedHalf(low_word);
  }

  std::array<std::int32_t, 3U> result{};
  for (std::size_t row = 0U; row < 3U; ++row) {
    auto multiply = [&](const std::array<std::int16_t, 3U> &input) {
      auto accumulator = signExtend44(
          static_cast<std::int64_t>(rotation[row * 3U]) * input[0]);
      accumulator = signExtend44(
          accumulator +
          static_cast<std::int64_t>(rotation[row * 3U + 1U]) * input[1]);
      return signExtend44(accumulator +
                          static_cast<std::int64_t>(rotation[row * 3U + 2U]) *
                              input[2]);
    };
    // ApplyMatrixLV reads MAC1..3 after both RTIR passes. IR saturation is
    // deliberately irrelevant here, including for the sf=12 remainder pass.
    const auto high_mac = lowSignedWord(multiply(high));
    const auto low_mac =
        lowSignedWord(arithmeticShiftRight(multiply(remainder), 12U));
    result[row] =
        lowSignedWord(static_cast<std::int64_t>(high_mac) * 8LL + low_mac);
  }
  return result;
}

[[nodiscard]] bool roundedGuestCoordinate(double value,
                                          std::int32_t &result) noexcept {
  if (!std::isfinite(value) ||
      value < static_cast<double>(std::numeric_limits<std::int32_t>::min()) ||
      value > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
    return false;
  }
  result = static_cast<std::int32_t>(std::llround(value));
  return true;
}

struct RetailProjection {
  std::int16_t x{};
  std::int16_t y{};
  std::uint16_t depth{};
};

[[nodiscard]] std::optional<RetailProjection>
projectIntoRetailLight(const RetailVertexLightState &light,
                       DynamicLightPoint world_point,
                       std::int32_t projection) noexcept {
  if (projection <= 0 || projection > 0xffff || light.extent <= 0 ||
      light.threshold < 0 || light.screen_shift > 31U ||
      light.depth_shift > 31U || (light.channel_mask & 0xff000000U) != 0U) {
    return std::nullopt;
  }

  std::int32_t guest_x{};
  std::int32_t guest_y{};
  std::int32_t guest_z{};
  if (!roundedGuestCoordinate(world_point.x, guest_x) ||
      !roundedGuestCoordinate(world_point.y, guest_y) ||
      !roundedGuestCoordinate(world_point.z, guest_z)) {
    return std::nullopt;
  }

  // GsSetView2 transposes the source rotation and separately computes
  // -ApplyMatrixLV(R^T, t). RTPT then performs a second Q12 multiply/add.
  // Keeping those two rounding points is observable at light boundaries.
  std::array<std::int16_t, 9U> view_rotation{};
  for (std::size_t row = 0U; row < 3U; ++row) {
    for (std::size_t column = 0U; column < 3U; ++column) {
      auto value = light.matrix.rotation[column * 3U + row];
      // FUN_800c973c reverses the raw X/Z columns for the attached light
      // before GsSetView2 builds the inverse view matrix.
      if ((light.flags & 1U) != 0U && (row == 0U || row == 2U)) {
        value = wrappedNegate(value);
      }
      view_rotation[row * 3U + column] = value;
    }
  }

  auto inverse_translation =
      applyRetailMatrixLong(view_rotation, light.matrix.translation);
  for (auto &component : inverse_translation) {
    component = std::bit_cast<std::int32_t>(
        0U - std::bit_cast<std::uint32_t>(component));
  }

  const std::array<std::int16_t, 3U> vertex{
      lowSignedHalf(guest_x),
      // EMD vertices and the renderer MATRIX already share retail's Y-down
      // space. Player/camera bridge adapters perform their own conversions;
      // flipping this value again sends the light cone away from the level.
      lowSignedHalf(guest_y),
      lowSignedHalf(guest_z),
  };
  std::array<std::int32_t, 3U> local{};
  for (std::size_t row = 0U; row < 3U; ++row) {
    auto accumulator = signExtend44(
        static_cast<std::int64_t>(inverse_translation[row]) * 4096LL +
        static_cast<std::int64_t>(view_rotation[row * 3U]) * vertex[0]);
    accumulator = signExtend44(
        accumulator +
        static_cast<std::int64_t>(view_rotation[row * 3U + 1U]) * vertex[1]);
    accumulator = signExtend44(
        accumulator +
        static_cast<std::int64_t>(view_rotation[row * 3U + 2U]) * vertex[2]);
    local[row] = lowSignedWord(arithmeticShiftRight(accumulator, 12U));
  }

  const auto depth_value = std::clamp(local[2], 0, 0xffff);
  if (depth_value == 0) {
    return std::nullopt;
  }

  const auto depth = static_cast<std::uint16_t>(depth_value);
  const auto quotient =
      divideRetailPerspective(static_cast<std::uint16_t>(projection), depth);
  const auto project_axis = [&](std::int32_t coordinate) {
    const auto ir = std::clamp(coordinate, -0x8000, 0x7fff);
    const auto screen = static_cast<std::int64_t>(quotient) * ir;
    return std::clamp(lowSignedWord(arithmeticShiftRight(screen, 16U)), -1024,
                      1023);
  };
  return RetailProjection{static_cast<std::int16_t>(project_axis(local[0])),
                          static_cast<std::int16_t>(project_axis(local[1])),
                          depth};
}

[[nodiscard]] std::uint32_t
applyRetailProjectedLight(std::uint32_t packed_color,
                          const RetailVertexLightState &light,
                          RetailProjection projected) noexcept {
  if (std::bit_cast<std::int32_t>(packed_color) < 0 || projected.depth == 0U) {
    return packed_color;
  }

  auto shift = light.screen_shift;
  std::int64_t depth_term{};
  if (projected.depth >= 256U) {
    depth_term =
        static_cast<std::int64_t>(projected.depth) >> light.depth_shift;
  } else {
    shift += projected.depth < 128U ? 2U : 1U;
    // MIPS variable shifts use the low five bits of the shift amount.
    shift &= 31U;
    // FUN_800d3b8c reuses t9 for this adjusted shift in the near-SZ path.
    depth_term = static_cast<std::int64_t>(shift) >> light.depth_shift;
  }

  const auto packed_xy =
      static_cast<std::uint32_t>(static_cast<std::uint16_t>(projected.x)) |
      (static_cast<std::uint32_t>(static_cast<std::uint16_t>(projected.y))
       << 16U);
  const auto signed_xy = std::bit_cast<std::int32_t>(packed_xy);
  const auto scaled_y = absolute(arithmeticShiftRight(signed_xy, shift));
  const auto shifted_x = std::bit_cast<std::int32_t>(packed_xy << 16U);
  const auto scaled_x = absolute(
      arithmeticShiftRight(arithmeticShiftRight(shifted_x, shift), 1U));
  const auto value = static_cast<std::int64_t>(light.extent) - scaled_x -
                     depth_term - scaled_y;
  if (value < 0 || value < light.threshold) {
    return packed_color;
  }

  const auto intensity =
      static_cast<std::uint32_t>(std::min<std::int64_t>(value * 2LL, 255LL));
  const auto addition =
      (intensity | (intensity << 8U) | (intensity << 16U)) & light.channel_mask;
  auto result = packed_color & 0xff000000U;
  for (unsigned shift_bits = 0U; shift_bits < 24U; shift_bits += 8U) {
    const auto base = (packed_color >> shift_bits) & 0xffU;
    const auto add = (addition >> shift_bits) & 0xffU;
    result |= std::min(base + add, 255U) << shift_bits;
  }
  return result;
}

} // namespace

DynamicLightVertexColor retailGmdBackColorModulation(
    std::array<std::int16_t, 3U> back_color_q12) noexcept {
  const auto weighted_q12 =
      (54LL * back_color_q12[0] + 183LL * back_color_q12[1] +
       19LL * back_color_q12[2] + 128LL) >>
      8U;
  const auto intensity = static_cast<std::uint8_t>(
      std::clamp<std::int64_t>((weighted_q12 + 16LL) >> 5U, 0LL, 255LL));
  return {intensity, intensity, intensity};
}

std::optional<SceneTriangleLightSample>
sampleSceneTriangleLighting(std::array<DynamicLightPoint, 3U> vertices,
                            std::array<DynamicLightVertexColor, 3U> colors,
                            DynamicLightPoint point) noexcept {
  if (!finite(point) || !std::ranges::all_of(vertices, [](const auto &vertex) {
        return finite(vertex);
      })) {
    return std::nullopt;
  }
  const auto denominator =
      (vertices[1].z - vertices[2].z) * (vertices[0].x - vertices[2].x) +
      (vertices[2].x - vertices[1].x) * (vertices[0].z - vertices[2].z);
  if (std::abs(denominator) <= 1.0e-8) {
    return std::nullopt;
  }
  const auto first =
      ((vertices[1].z - vertices[2].z) * (point.x - vertices[2].x) +
       (vertices[2].x - vertices[1].x) * (point.z - vertices[2].z)) /
      denominator;
  const auto second =
      ((vertices[2].z - vertices[0].z) * (point.x - vertices[2].x) +
       (vertices[0].x - vertices[2].x) * (point.z - vertices[2].z)) /
      denominator;
  const auto third = 1.0 - first - second;
  constexpr auto edge_tolerance = 1.0e-6;
  if (first < -edge_tolerance || second < -edge_tolerance ||
      third < -edge_tolerance) {
    return std::nullopt;
  }
  const auto channel = [&](std::uint8_t DynamicLightVertexColor::*member) {
    return static_cast<std::uint8_t>(std::clamp(
        std::lround(first * colors[0].*member + second * colors[1].*member +
                    third * colors[2].*member),
        0L, 255L));
  };
  return SceneTriangleLightSample{
      {channel(&DynamicLightVertexColor::red),
       channel(&DynamicLightVertexColor::green),
       channel(&DynamicLightVertexColor::blue)},
      first * vertices[0].y + second * vertices[1].y + third * vertices[2].y,
  };
}

DynamicLightFrame buildDynamicLightFrame(
    std::span<const PersistentDynamicLightState> persistent,
    std::span<const TransientDynamicLightState> transient,
    DynamicLightPoint observer,
    std::span<const DirectionalDynamicLightState> directional,
    std::uint64_t animation_tick) noexcept {
  auto result = DynamicLightFrame{};
  if (!finite(observer)) {
    return result;
  }

  std::array<SelectedLight, maximum_dynamic_lights> selected{};
  auto count = std::size_t{};
  auto source_order = std::size_t{};
  for (const auto &source : transient) {
    if (const auto light = makeTransient(source)) {
      consider(selected, count, *light, observer, source_order);
    }
    ++source_order;
  }
  for (const auto &source : directional) {
    if (const auto light = makeDirectional(source)) {
      consider(selected, count, *light, observer, source_order);
    }
    ++source_order;
  }
  for (const auto &source : persistent) {
    if (const auto light = makePersistent(source, animation_tick)) {
      consider(selected, count, *light, observer, source_order);
    }
    ++source_order;
  }

  std::sort(selected.begin(), selected.begin() + count,
            [](const SelectedLight &first, const SelectedLight &second) {
              return first.source_order < second.source_order;
            });
  result.count = count;
  for (std::size_t index = 0U; index < count; ++index) {
    result.lights[index] = selected[index].light;
  }
  return result;
}

DynamicLightFrame
dynamicLightFrameForBakedWorld(const DynamicLightFrame &frame) noexcept {
  auto result = DynamicLightFrame{};
  for (const auto &light : frame.active()) {
    if (!light.transient && !light.directional) {
      continue;
    }
    if (result.count >= result.lights.size()) {
      break;
    }
    result.lights[result.count++] = light;
  }
  return result;
}

DynamicLightModulation sampleDynamicLightingImpl(
    const DynamicLightFrame &frame, DynamicLightPoint point,
    std::optional<DynamicLightPoint> surface_normal) noexcept {
  auto result = DynamicLightModulation{};
  if (!finite(point)) {
    return result;
  }
  auto normal = DynamicLightPoint{};
  if (surface_normal) {
    if (!finite(*surface_normal)) {
      return result;
    }
    const auto length = std::sqrt(surface_normal->x * surface_normal->x +
                                  surface_normal->y * surface_normal->y +
                                  surface_normal->z * surface_normal->z);
    if (!std::isfinite(length) || length <= 0.000001) {
      return result;
    }
    normal = {surface_normal->x / length, surface_normal->y / length,
              surface_normal->z / length};
  }
  for (const auto &light : frame.active()) {
    if (!finite(light.position) || !std::isfinite(light.radius) ||
        !std::isfinite(light.intensity) || light.radius <= 0.0 ||
        light.intensity <= 0.0) {
      continue;
    }
    const auto distance_squared = squaredDistance(light.position, point);
    const auto radius_squared = light.radius * light.radius;
    if (!std::isfinite(distance_squared) ||
        distance_squared >= radius_squared) {
      continue;
    }
    const auto distance = std::sqrt(distance_squared);
    const auto radial = 1.0 - distance_squared / radius_squared;
    auto attenuation = radial * radial * light.intensity;
    if (light.directional) {
      if (distance <= 0.000001 ||
          light.inner_cone_cosine <= light.outer_cone_cosine) {
        continue;
      }
      const auto inverse_distance = 1.0 / distance;
      const auto to_point = DynamicLightPoint{
          (point.x - light.position.x) * inverse_distance,
          (point.y - light.position.y) * inverse_distance,
          (point.z - light.position.z) * inverse_distance,
      };
      const auto cosine = light.direction.x * to_point.x +
                          light.direction.y * to_point.y +
                          light.direction.z * to_point.z;
      if (cosine <= light.outer_cone_cosine) {
        continue;
      }
      const auto cone =
          std::clamp((cosine - light.outer_cone_cosine) /
                         (light.inner_cone_cosine - light.outer_cone_cosine),
                     0.0, 1.0);
      attenuation *= cone * cone;
    }
    if (surface_normal && distance > 0.000001) {
      const auto inverse_distance = 1.0 / distance;
      const auto to_light = DynamicLightPoint{
          (light.position.x - point.x) * inverse_distance,
          (light.position.y - point.y) * inverse_distance,
          (light.position.z - point.z) * inverse_distance,
      };
      const auto cosine =
          normal.x * to_light.x + normal.y * to_light.y + normal.z * to_light.z;
      // Wrapped Lambert lighting prevents low-poly characters from acquiring
      // razor-sharp black facets, but fully rejects lights behind a surface.
      const auto surface = std::clamp((cosine + 0.12) / 1.12, 0.0, 1.0);
      attenuation *= surface * surface * (3.0 - 2.0 * surface);
    }
    result.red += light.color.red * attenuation;
    result.green += light.color.green * attenuation;
    result.blue += light.color.blue * attenuation;
  }
  // Keep irradiance linear here. A hard per-channel ceiling made every
  // sufficiently crowded light volume converge to the same flat value and
  // destroyed texture contrast before the authored retail colour was known.
  // The final composition owns the bounded, headroom-aware response.
  return result;
}

DynamicLightModulation sampleDynamicLighting(const DynamicLightFrame &frame,
                                             DynamicLightPoint point) noexcept {
  return sampleDynamicLightingImpl(frame, point, std::nullopt);
}

DynamicShadowProjection
selectDynamicShadowProjection(const DynamicLightFrame &frame,
                              DynamicLightPoint actor_anchor,
                              DynamicLightPoint ground_normal) noexcept {
  auto result = DynamicShadowProjection{};
  if (!finite(actor_anchor) || !finite(ground_normal)) {
    return result;
  }
  const auto normal_length = std::sqrt(ground_normal.x * ground_normal.x +
                                       ground_normal.y * ground_normal.y +
                                       ground_normal.z * ground_normal.z);
  if (!std::isfinite(normal_length) || normal_length <= 0.000001) {
    return result;
  }
  const auto normal = DynamicLightPoint{ground_normal.x / normal_length,
                                        ground_normal.y / normal_length,
                                        ground_normal.z / normal_length};

  struct ShadowInfluence {
    DynamicLightPoint ray;
    double score{};
    std::uint32_t source_id{};
    DynamicLightKind kind{DynamicLightKind::street_lamp};
  };
  std::array<ShadowInfluence, maximum_dynamic_lights> influences{};
  auto influence_count = std::size_t{};
  auto strongest_score = 0.0;
  for (const auto &light : frame.active()) {
    // A single projected silhouette cannot represent several short-lived
    // combat shadows. Muzzle flashes/explosions and the actor-owned
    // flashlight used to yank the common ray by tens of degrees for one 20 Hz
    // update. Keep them in illumination, but let only persistent radial scene
    // sources participate in the stable key-light solution.
    if (light.transient || light.directional || !finite(light.position)) {
      continue;
    }
    const auto stable_profile = profile(light.kind);
    if (!std::isfinite(stable_profile.radius) ||
        !std::isfinite(stable_profile.intensity) ||
        stable_profile.radius <= 0.0 || stable_profile.intensity <= 0.0) {
      continue;
    }
    const auto delta = DynamicLightPoint{actor_anchor.x - light.position.x,
                                         actor_anchor.y - light.position.y,
                                         actor_anchor.z - light.position.z};
    const auto distance_squared =
        delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
    if (!std::isfinite(distance_squared) ||
        distance_squared >= stable_profile.radius * stable_profile.radius) {
      continue;
    }
    const auto distance = std::sqrt(distance_squared);
    if (distance <= 0.000001) {
      continue;
    }
    const auto ray = DynamicLightPoint{delta.x / distance, delta.y / distance,
                                       delta.z / distance};
    auto attenuation = 1.0 - distance_squared / (stable_profile.radius *
                                                 stable_profile.radius);
    attenuation = attenuation * attenuation * stable_profile.intensity;
    const auto ground_alignment =
        ray.x * normal.x + ray.y * normal.y + ray.z * normal.z;
    constexpr auto minimum_ground_alignment = 0.16;
    if (ground_alignment <= minimum_ground_alignment) {
      continue;
    }
    // Shadow weight deliberately uses the non-animated profile. Police and
    // fire color/intensity animation remains visible on geometry without
    // rotating or pulsing the actor silhouette every other guest update.
    const auto luminance = stable_profile.color.red * 0.2126 +
                           stable_profile.color.green * 0.7152 +
                           stable_profile.color.blue * 0.0722;
    const auto score = attenuation * std::max(luminance, 0.0) *
                       (0.35 + ground_alignment * 0.65);
    if (!std::isfinite(score) || score <= 0.0) {
      continue;
    }
    influences[influence_count++] =
        ShadowInfluence{ray, score, light.source_id, light.kind};
    strongest_score = std::max(strongest_score, score);
  }

  if (influence_count == 0U || !std::isfinite(strongest_score) ||
      strongest_score <= 0.000001) {
    return result;
  }

  // Sort before accumulation so the result is bit-stable even if resident
  // object enumeration changes. A soft relative-score gate prevents dozens
  // of barely relevant lamps from washing out the local key direction while
  // avoiding a hard top-N boundary when two sources exchange dominance.
  std::ranges::sort(std::span{influences}.first(influence_count), {},
                    [](const ShadowInfluence &influence) {
                      return std::pair{influence.source_id, influence.kind};
                    });
  constexpr auto key_weight = 0.05;
  auto weighted_ray = DynamicLightPoint{
      result.ray_direction.x * key_weight,
      result.ray_direction.y * key_weight,
      result.ray_direction.z * key_weight,
  };
  auto total_weight = key_weight;
  for (const auto &influence : std::span{influences}.first(influence_count)) {
    const auto relative = influence.score / strongest_score;
    const auto gate = std::clamp((relative - 0.08) / 0.22, 0.0, 1.0);
    const auto smooth_gate = gate * gate * (3.0 - 2.0 * gate);
    const auto weight = influence.score * smooth_gate;
    weighted_ray.x += influence.ray.x * weight;
    weighted_ray.y += influence.ray.y * weight;
    weighted_ray.z += influence.ray.z * weight;
    total_weight += weight;
  }
  if (!std::isfinite(total_weight) || total_weight <= 0.000001) {
    return result;
  }
  weighted_ray = {weighted_ray.x / total_weight, weighted_ray.y / total_weight,
                  weighted_ray.z / total_weight};
  auto normal_component = weighted_ray.x * normal.x +
                          weighted_ray.y * normal.y + weighted_ray.z * normal.z;
  if (!std::isfinite(normal_component) || normal_component <= 0.000001) {
    return result;
  }
  auto tangent = DynamicLightPoint{
      weighted_ray.x - normal.x * normal_component,
      weighted_ray.y - normal.y * normal_component,
      weighted_ray.z - normal.z * normal_component,
  };
  const auto tangent_length = std::sqrt(
      tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
  // A shadow may lean, but never become several character-heights long.
  constexpr auto maximum_tangent_ratio = 0.85;
  const auto maximum_tangent = normal_component * maximum_tangent_ratio;
  if (std::isfinite(tangent_length) && tangent_length > maximum_tangent &&
      tangent_length > 0.000001) {
    const auto scale = maximum_tangent / tangent_length;
    tangent = {tangent.x * scale, tangent.y * scale, tangent.z * scale};
  }
  auto bounded_ray = DynamicLightPoint{
      normal.x * normal_component + tangent.x,
      normal.y * normal_component + tangent.y,
      normal.z * normal_component + tangent.z,
  };
  const auto bounded_length =
      std::sqrt(bounded_ray.x * bounded_ray.x + bounded_ray.y * bounded_ray.y +
                bounded_ray.z * bounded_ray.z);
  if (!std::isfinite(bounded_length) || bounded_length <= 0.000001) {
    return result;
  }
  result.ray_direction = {bounded_ray.x / bounded_length,
                          bounded_ray.y / bounded_length,
                          bounded_ray.z / bounded_length};
  // Darkness follows the strongest local key rather than the number of
  // resident sources. Walking into a room containing many lamps no longer
  // makes the silhouette abruptly darker or amplifies light flicker.
  result.darkness = std::clamp(0.18 + strongest_score * 0.85, 0.18, 0.42);
  result.source_driven = true;
  return result;
}

DynamicShadowProjectionState
advanceDynamicShadowProjection(const DynamicShadowProjectionState &state,
                               const DynamicShadowProjection &target,
                               std::uint64_t guest_tick) noexcept {
  const auto valid_projection = [](const DynamicShadowProjection &projection) {
    const auto length_squared =
        projection.ray_direction.x * projection.ray_direction.x +
        projection.ray_direction.y * projection.ray_direction.y +
        projection.ray_direction.z * projection.ray_direction.z;
    return finite(projection.ray_direction) &&
           std::isfinite(projection.darkness) &&
           std::isfinite(length_squared) && length_squared > 0.000001;
  };
  auto stable_target =
      valid_projection(target) ? target : DynamicShadowProjection{};
  if (!state.initialized || guest_tick < state.guest_tick ||
      !valid_projection(state.current)) {
    return DynamicShadowProjectionState{stable_target, stable_target,
                                        guest_tick, true};
  }
  if (guest_tick == state.guest_tick) {
    return state;
  }

  const auto elapsed =
      std::min<std::uint64_t>(guest_tick - state.guest_tick, 20U);
  constexpr auto direction_response_per_update = 0.32;
  constexpr auto darkness_response_per_update = 0.40;
  const auto direction_response =
      1.0 - std::pow(1.0 - direction_response_per_update,
                     static_cast<double>(elapsed));
  const auto darkness_response =
      1.0 - std::pow(1.0 - darkness_response_per_update,
                     static_cast<double>(elapsed));
  auto direction = DynamicLightPoint{
      std::lerp(state.current.ray_direction.x, stable_target.ray_direction.x,
                direction_response),
      std::lerp(state.current.ray_direction.y, stable_target.ray_direction.y,
                direction_response),
      std::lerp(state.current.ray_direction.z, stable_target.ray_direction.z,
                direction_response),
  };
  const auto direction_length =
      std::sqrt(direction.x * direction.x + direction.y * direction.y +
                direction.z * direction.z);
  if (!std::isfinite(direction_length) || direction_length <= 0.000001) {
    direction = stable_target.ray_direction;
  } else {
    direction = {direction.x / direction_length, direction.y / direction_length,
                 direction.z / direction_length};
  }
  const auto current = DynamicShadowProjection{
      direction,
      std::lerp(state.current.darkness, stable_target.darkness,
                darkness_response),
      stable_target.source_driven,
  };
  return DynamicShadowProjectionState{state.current, current, guest_tick, true};
}

DynamicShadowProjection
sampleDynamicShadowProjection(const DynamicShadowProjectionState &state,
                              double amount) noexcept {
  if (!state.initialized || !std::isfinite(amount)) {
    return DynamicShadowProjection{};
  }
  const auto interpolation = std::clamp(amount, 0.0, 1.0);
  auto direction = DynamicLightPoint{
      std::lerp(state.previous.ray_direction.x, state.current.ray_direction.x,
                interpolation),
      std::lerp(state.previous.ray_direction.y, state.current.ray_direction.y,
                interpolation),
      std::lerp(state.previous.ray_direction.z, state.current.ray_direction.z,
                interpolation),
  };
  const auto direction_length =
      std::sqrt(direction.x * direction.x + direction.y * direction.y +
                direction.z * direction.z);
  if (!std::isfinite(direction_length) || direction_length <= 0.000001) {
    return DynamicShadowProjection{};
  }
  direction = {direction.x / direction_length, direction.y / direction_length,
               direction.z / direction_length};
  return DynamicShadowProjection{
      direction,
      std::lerp(state.previous.darkness, state.current.darkness, interpolation),
      interpolation < 1.0 ? state.previous.source_driven
                          : state.current.source_driven,
  };
}

std::optional<DynamicLightPoint>
projectDynamicShadowPoint(DynamicLightPoint vertex,
                          DynamicLightPoint ground_point,
                          DynamicLightPoint ground_normal,
                          const DynamicShadowProjection &projection) noexcept {
  if (!finite(vertex) || !finite(ground_point) || !finite(ground_normal) ||
      !finite(projection.ray_direction)) {
    return std::nullopt;
  }
  const auto normal_length = std::sqrt(ground_normal.x * ground_normal.x +
                                       ground_normal.y * ground_normal.y +
                                       ground_normal.z * ground_normal.z);
  const auto ray_length =
      std::sqrt(projection.ray_direction.x * projection.ray_direction.x +
                projection.ray_direction.y * projection.ray_direction.y +
                projection.ray_direction.z * projection.ray_direction.z);
  if (!std::isfinite(normal_length) || !std::isfinite(ray_length) ||
      normal_length <= 0.000001 || ray_length <= 0.000001) {
    return std::nullopt;
  }
  const auto normal = DynamicLightPoint{ground_normal.x / normal_length,
                                        ground_normal.y / normal_length,
                                        ground_normal.z / normal_length};
  const auto ray = DynamicLightPoint{
      projection.ray_direction.x / ray_length,
      projection.ray_direction.y / ray_length,
      projection.ray_direction.z / ray_length,
  };
  const auto denominator =
      ray.x * normal.x + ray.y * normal.y + ray.z * normal.z;
  if (!std::isfinite(denominator) || denominator <= 0.08) {
    return std::nullopt;
  }
  const auto plane_distance = (ground_point.x - vertex.x) * normal.x +
                              (ground_point.y - vertex.y) * normal.y +
                              (ground_point.z - vertex.z) * normal.z;
  auto distance = plane_distance / denominator;
  if (!std::isfinite(distance)) {
    return std::nullopt;
  }
  // Malformed poses and low grazing lights cannot produce an unbounded quad.
  distance = std::clamp(distance, 0.0, 960.0);
  auto tangent = DynamicLightPoint{ray.x - normal.x * denominator,
                                   ray.y - normal.y * denominator,
                                   ray.z - normal.z * denominator};
  auto tangent_offset = DynamicLightPoint{
      tangent.x * distance, tangent.y * distance, tangent.z * distance};
  const auto tangent_offset_length =
      std::sqrt(tangent_offset.x * tangent_offset.x +
                tangent_offset.y * tangent_offset.y +
                tangent_offset.z * tangent_offset.z);
  constexpr auto maximum_stretch_ratio = 0.85;
  const auto maximum_tangent_offset =
      std::abs(plane_distance) * maximum_stretch_ratio;
  if (std::isfinite(tangent_offset_length) &&
      tangent_offset_length > maximum_tangent_offset &&
      tangent_offset_length > 0.000001) {
    const auto scale = maximum_tangent_offset / tangent_offset_length;
    tangent_offset = {tangent_offset.x * scale, tangent_offset.y * scale,
                      tangent_offset.z * scale};
  }
  constexpr auto surface_bias = 3.0;
  return DynamicLightPoint{
      vertex.x + normal.x * plane_distance + tangent_offset.x -
          normal.x * surface_bias,
      vertex.y + normal.y * plane_distance + tangent_offset.y -
          normal.y * surface_bias,
      vertex.z + normal.z * plane_distance + tangent_offset.z -
          normal.z * surface_bias,
  };
}

DynamicLightModulation
sampleDynamicLighting(const DynamicLightFrame &frame, DynamicLightPoint point,
                      DynamicLightPoint surface_normal) noexcept {
  return sampleDynamicLightingImpl(frame, point, surface_normal);
}

DynamicLightVertexColor
applyDynamicLighting(DynamicLightVertexColor base,
                     DynamicLightModulation modulation) noexcept {
  if (!std::isfinite(modulation.red) || !std::isfinite(modulation.green) ||
      !std::isfinite(modulation.blue) || modulation.red < 0.0 ||
      modulation.green < 0.0 || modulation.blue < 0.0) {
    return base;
  }

  // Match the old response slope around neutral PS1 modulation (128) while
  // approaching only a bounded fraction of the remaining channel headroom.
  // This soft knee lets overlapping lights grow monotonically without
  // clipping already bright retail colours to featureless white.
  constexpr double neutral_modulation_delta = 96.0;
  constexpr double neutral_headroom = 255.0 - 128.0;
  constexpr double linear_response =
      neutral_modulation_delta / neutral_headroom;
  constexpr double maximum_headroom_fraction = 0.72;
  // Dynamic sources sit on top of the authored PS1 vertex lighting. Keep that
  // layer deliberately restrained so lamps, fire and muzzle flashes preserve
  // local contrast instead of lifting an entire surface toward white.
  constexpr double native_dynamic_light_exposure = 0.68;
  // Retail encodes intentional darkness in the primitive modulation itself.
  // An additive light which always targets white turns black alcoves and
  // lamp-off sections into bright grey, and also leaks visibly through the
  // deliberately coarse PS1 occlusion. Use the retail luminance as a shared
  // exposure mask: neutral/brighter geometry keeps the full native response,
  // while authored dark regions retain their original contrast and hue.
  constexpr double neutral_retail_luminance = 128.0;
  const auto retail_luminance = (54.0 * static_cast<double>(base.red) +
                                 183.0 * static_cast<double>(base.green) +
                                 19.0 * static_cast<double>(base.blue)) /
                                256.0;
  const auto authored_exposure =
      std::clamp(retail_luminance / neutral_retail_luminance, 0.0, 1.0);
  const auto apply = [authored_exposure](std::uint8_t value, double amount) {
    if (amount <= 0.0 || value == 255U) {
      return value;
    }
    const auto response =
        -maximum_headroom_fraction *
        std::expm1(-amount * native_dynamic_light_exposure * linear_response /
                   maximum_headroom_fraction);
    const auto headroom = 255.0 - static_cast<double>(value);
    const auto delta = static_cast<unsigned>(
        std::floor(headroom * response * authored_exposure));
    return static_cast<std::uint8_t>(static_cast<unsigned>(value) + delta);
  };
  return DynamicLightVertexColor{apply(base.red, modulation.red),
                                 apply(base.green, modulation.green),
                                 apply(base.blue, modulation.blue)};
}

std::optional<RetailVertexLightRay>
retailVertexLightRay(const RetailVertexLightState &light) noexcept {
  const auto direction_sign = (light.flags & 1U) != 0U ? -1.0 : 1.0;
  auto direction = DynamicLightPoint{
      direction_sign * static_cast<double>(light.matrix.rotation[2]),
      direction_sign * static_cast<double>(light.matrix.rotation[5]),
      direction_sign * static_cast<double>(light.matrix.rotation[8]),
  };
  const auto length =
      std::sqrt(direction.x * direction.x + direction.y * direction.y +
                direction.z * direction.z);
  if (!std::isfinite(length) || length <= 0.000001) {
    return std::nullopt;
  }
  direction = {direction.x / length, direction.y / length,
               direction.z / length};
  const auto origin = DynamicLightPoint{
      static_cast<double>(light.matrix.translation[0]),
      static_cast<double>(light.matrix.translation[1]),
      static_cast<double>(light.matrix.translation[2]),
  };
  if (!finite(origin)) {
    return std::nullopt;
  }
  return RetailVertexLightRay{origin, direction};
}

std::uint32_t applyRetailVertexLightingPacked(
    std::uint32_t packed_color, std::span<const RetailVertexLightState> lights,
    DynamicLightPoint world_point, std::int32_t projection) noexcept {
  if (lights.size() > maximum_retail_vertex_lights || projection <= 0 ||
      !finite(world_point)) {
    return packed_color;
  }
  auto result = packed_color;
  for (const auto &light : lights) {
    if (const auto projected =
            projectIntoRetailLight(light, world_point, projection)) {
      result = applyRetailProjectedLight(result, light, *projected);
    }
  }
  return result;
}

DynamicLightVertexColor
applyRetailVertexLighting(DynamicLightVertexColor base,
                          std::span<const RetailVertexLightState> lights,
                          DynamicLightPoint world_point,
                          std::int32_t projection) noexcept {
  const auto packed = static_cast<std::uint32_t>(base.red) |
                      (static_cast<std::uint32_t>(base.green) << 8U) |
                      (static_cast<std::uint32_t>(base.blue) << 16U);
  const auto lit =
      applyRetailVertexLightingPacked(packed, lights, world_point, projection);
  return DynamicLightVertexColor{static_cast<std::uint8_t>(lit),
                                 static_cast<std::uint8_t>(lit >> 8U),
                                 static_cast<std::uint8_t>(lit >> 16U)};
}

bool dynamicLightSegmentIntersectsBounds(
    DynamicLightPoint origin, DynamicLightPoint direction,
    double maximum_distance, const DynamicLightBounds &bounds) noexcept {
  if (!finite(origin) || !finite(direction) || !finite(bounds.minimum) ||
      !finite(bounds.maximum) || !std::isfinite(maximum_distance) ||
      maximum_distance <= 0.0 || bounds.minimum.x > bounds.maximum.x ||
      bounds.minimum.y > bounds.maximum.y ||
      bounds.minimum.z > bounds.maximum.z) {
    return false;
  }
  const auto direction_length =
      std::sqrt(direction.x * direction.x + direction.y * direction.y +
                direction.z * direction.z);
  if (!std::isfinite(direction_length) || direction_length <= 0.000001) {
    return false;
  }
  direction = {direction.x / direction_length, direction.y / direction_length,
               direction.z / direction_length};
  auto nearest = 0.0;
  auto farthest = maximum_distance;
  const auto intersect_axis = [&](double ray_origin, double ray_direction,
                                  double minimum, double maximum) {
    if (std::abs(ray_direction) <= 0.000001) {
      return ray_origin >= minimum && ray_origin <= maximum;
    }
    auto first = (minimum - ray_origin) / ray_direction;
    auto second = (maximum - ray_origin) / ray_direction;
    if (first > second) {
      std::swap(first, second);
    }
    nearest = std::max(nearest, first);
    farthest = std::min(farthest, second);
    return nearest <= farthest;
  };
  return intersect_axis(origin.x, direction.x, bounds.minimum.x,
                        bounds.maximum.x) &&
         intersect_axis(origin.y, direction.y, bounds.minimum.y,
                        bounds.maximum.y) &&
         intersect_axis(origin.z, direction.z, bounds.minimum.z,
                        bounds.maximum.z);
}

std::optional<DynamicLightSurfaceHit>
dynamicLightSurfaceHit(DynamicLightPoint origin, DynamicLightPoint direction,
                       const DynamicLightSurfaceTriangle &triangle,
                       double maximum_distance) noexcept {
  if (!finite(origin) || !finite(direction) || !finite(triangle.first) ||
      !finite(triangle.second) || !finite(triangle.third) ||
      !std::isfinite(maximum_distance) || maximum_distance <= 0.0) {
    return std::nullopt;
  }
  const auto direction_length =
      std::sqrt(direction.x * direction.x + direction.y * direction.y +
                direction.z * direction.z);
  if (!std::isfinite(direction_length) || direction_length <= 0.000001) {
    return std::nullopt;
  }
  direction = {direction.x / direction_length, direction.y / direction_length,
               direction.z / direction_length};
  const auto edge1 = DynamicLightPoint{
      triangle.second.x - triangle.first.x,
      triangle.second.y - triangle.first.y,
      triangle.second.z - triangle.first.z,
  };
  const auto edge2 = DynamicLightPoint{
      triangle.third.x - triangle.first.x,
      triangle.third.y - triangle.first.y,
      triangle.third.z - triangle.first.z,
  };
  const auto cross = [](DynamicLightPoint first, DynamicLightPoint second) {
    return DynamicLightPoint{
        first.y * second.z - first.z * second.y,
        first.z * second.x - first.x * second.z,
        first.x * second.y - first.y * second.x,
    };
  };
  const auto dot = [](DynamicLightPoint first, DynamicLightPoint second) {
    return first.x * second.x + first.y * second.y + first.z * second.z;
  };
  const auto perpendicular = cross(direction, edge2);
  const auto determinant = dot(edge1, perpendicular);
  if (!std::isfinite(determinant) || std::abs(determinant) <= 0.0000001) {
    return std::nullopt;
  }
  const auto inverse_determinant = 1.0 / determinant;
  const auto from_first = DynamicLightPoint{
      origin.x - triangle.first.x,
      origin.y - triangle.first.y,
      origin.z - triangle.first.z,
  };
  const auto u = dot(from_first, perpendicular) * inverse_determinant;
  if (u < 0.0 || u > 1.0) {
    return std::nullopt;
  }
  const auto side = cross(from_first, edge1);
  const auto v = dot(direction, side) * inverse_determinant;
  if (v < 0.0 || u + v > 1.0) {
    return std::nullopt;
  }
  const auto distance = dot(edge2, side) * inverse_determinant;
  if (!std::isfinite(distance) || distance <= 0.5 ||
      distance > maximum_distance) {
    return std::nullopt;
  }
  auto normal = cross(edge1, edge2);
  const auto normal_length = std::sqrt(
      normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
  if (!std::isfinite(normal_length) || normal_length <= 0.000001) {
    return std::nullopt;
  }
  normal = {normal.x / normal_length, normal.y / normal_length,
            normal.z / normal_length};
  if (dot(normal, direction) > 0.0) {
    normal = {-normal.x, -normal.y, -normal.z};
  }
  return DynamicLightSurfaceHit{
      {origin.x + direction.x * distance, origin.y + direction.y * distance,
       origin.z + direction.z * distance},
      normal,
      distance,
  };
}

} // namespace sf::game
