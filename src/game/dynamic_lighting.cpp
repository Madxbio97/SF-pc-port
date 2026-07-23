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
makePersistent(const PersistentDynamicLightState &source) noexcept {
  if (!source.identity_confirmed || !source.active || !source.resident ||
      source.destroyed || !persistentKind(source.kind) ||
      !finite(source.position)) {
    return std::nullopt;
  }
  const auto light_profile = profile(source.kind);
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

[[nodiscard]] bool better(const SelectedLight &candidate,
                          const SelectedLight &resident) noexcept {
  if (candidate.light.transient != resident.light.transient) {
    return candidate.light.transient;
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

[[nodiscard]] std::uint8_t quantize(double value) noexcept {
  if (!std::isfinite(value)) {
    return 0U;
  }
  return static_cast<std::uint8_t>(
      std::clamp<long>(std::lround(value), 0L, 255L));
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
    std::span<const DirectionalDynamicLightState> directional) noexcept {
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
    if (const auto light = makePersistent(source)) {
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

DynamicLightModulation sampleDynamicLighting(const DynamicLightFrame &frame,
                                             DynamicLightPoint point) noexcept {
  auto result = DynamicLightModulation{};
  if (!finite(point)) {
    return result;
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
    const auto radial = 1.0 - distance_squared / radius_squared;
    auto attenuation = radial * radial * light.intensity;
    if (light.directional) {
      const auto distance = std::sqrt(distance_squared);
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
    result.red += light.color.red * attenuation;
    result.green += light.color.green * attenuation;
    result.blue += light.color.blue * attenuation;
  }
  result.red = std::min(result.red, 0.55);
  result.green = std::min(result.green, 0.55);
  result.blue = std::min(result.blue, 0.55);
  return result;
}

DynamicLightVertexColor
applyDynamicLighting(DynamicLightVertexColor base,
                     DynamicLightModulation modulation) noexcept {
  constexpr double neutral_modulation = 96.0;
  const auto apply = [](std::uint8_t value, double amount) {
    if (!std::isfinite(amount) || amount <= 0.0) {
      return value;
    }
    return quantize(static_cast<double>(value) + amount * neutral_modulation);
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
