#pragma once

#include "sf/game/dynamic_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace sf::platform {

struct ActorShadowReceiverPlane {
  game::DynamicLightPoint point{};
  game::DynamicLightPoint normal{};
  bool wall{};
};

[[nodiscard]] inline bool
sameActorShadowPlane(const ActorShadowReceiverPlane &first,
                     const ActorShadowReceiverPlane &second) noexcept {
  const auto first_length = std::sqrt(first.normal.x * first.normal.x +
                                      first.normal.y * first.normal.y +
                                      first.normal.z * first.normal.z);
  const auto second_length = std::sqrt(second.normal.x * second.normal.x +
                                       second.normal.y * second.normal.y +
                                       second.normal.z * second.normal.z);
  if (!std::isfinite(first_length) || !std::isfinite(second_length) ||
      first_length <= 1.0e-6 || second_length <= 1.0e-6) {
    return false;
  }
  const auto normal_alignment = std::abs((first.normal.x * second.normal.x +
                                          first.normal.y * second.normal.y +
                                          first.normal.z * second.normal.z) /
                                         (first_length * second_length));
  const auto plane_separation =
      std::abs(((second.point.x - first.point.x) * first.normal.x +
                (second.point.y - first.point.y) * first.normal.y +
                (second.point.z - first.point.z) * first.normal.z) /
               first_length);
  return first.wall == second.wall && normal_alignment >= 0.94 &&
         plane_separation <= 48.0;
}

struct ActorShadowSupportState {
  ActorShadowReceiverPlane previous;
  ActorShadowReceiverPlane current;
  std::optional<ActorShadowReceiverPlane> pending;
  std::uint8_t pending_samples{};
  std::uint64_t guest_tick{};
  bool initialized{};
};

[[nodiscard]] inline ActorShadowSupportState
advanceActorShadowSupport(ActorShadowSupportState state,
                          std::optional<ActorShadowReceiverPlane> target,
                          std::uint64_t guest_tick) noexcept {
  constexpr auto support_confirmation_samples = std::uint8_t{2U};
  if (state.initialized && guest_tick == state.guest_tick) {
    return state;
  }
  if (!target) {
    if (state.initialized && guest_tick >= state.guest_tick) {
      state.previous = state.current;
      if (state.pending) {
        state.pending.reset();
        state.pending_samples = 1U;
      } else {
        state.pending_samples = static_cast<std::uint8_t>(
            std::min<unsigned int>(state.pending_samples + 1U,
                                   support_confirmation_samples));
      }
      state.guest_tick = guest_tick;
      if (state.pending_samples >= support_confirmation_samples) {
        state = {};
      }
    } else {
      state = {};
    }
    return state;
  }
  target->wall = false;
  if (!state.initialized || guest_tick < state.guest_tick) {
    state.previous = *target;
    state.current = *target;
    state.guest_tick = guest_tick;
    state.initialized = true;
    return state;
  }
  const auto alignment = state.current.normal.x * target->normal.x +
                         state.current.normal.y * target->normal.y +
                         state.current.normal.z * target->normal.z;
  if (alignment < 0.0) {
    target->normal = {-target->normal.x, -target->normal.y, -target->normal.z};
  }
  if (!sameActorShadowPlane(state.current, *target)) {
    state.previous = state.current;
    if (!state.pending || !sameActorShadowPlane(*state.pending, *target)) {
      state.pending = *target;
      state.pending_samples = 1U;
    } else {
      state.pending_samples = static_cast<std::uint8_t>(std::min<unsigned int>(
          state.pending_samples + 1U, support_confirmation_samples));
    }
    if (state.pending_samples >= support_confirmation_samples) {
      target = state.pending;
      state.previous = state.current;
      state.current = *target;
      state.pending.reset();
      state.pending_samples = 0U;
    }
    state.guest_tick = guest_tick;
    return state;
  }
  state.previous = state.current;
  state.current = *target;
  state.pending.reset();
  state.pending_samples = 0U;
  state.guest_tick = guest_tick;
  return state;
}

[[nodiscard]] inline std::optional<ActorShadowReceiverPlane>
sampleActorShadowSupport(const ActorShadowSupportState &state,
                         double amount) noexcept {
  if (!state.initialized) {
    return std::nullopt;
  }
  amount = std::clamp(std::isfinite(amount) ? amount : 1.0, 0.0, 1.0);
  const auto previous_normal = state.previous.normal;
  auto current_normal = state.current.normal;
  const auto alignment = previous_normal.x * current_normal.x +
                         previous_normal.y * current_normal.y +
                         previous_normal.z * current_normal.z;
  // Polygon winding is not semantically significant for support planes.
  // Keep both samples in the same hemisphere before interpolating.
  if (alignment < 0.0) {
    current_normal = {-current_normal.x, -current_normal.y, -current_normal.z};
  }
  auto normal = game::DynamicLightPoint{
      std::lerp(previous_normal.x, current_normal.x, amount),
      std::lerp(previous_normal.y, current_normal.y, amount),
      std::lerp(previous_normal.z, current_normal.z, amount)};
  const auto normal_length = std::sqrt(
      normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
  if (!std::isfinite(normal_length) || normal_length <= 1.0e-6) {
    return std::nullopt;
  }
  normal = {normal.x / normal_length, normal.y / normal_length,
            normal.z / normal_length};
  return ActorShadowReceiverPlane{
      {std::lerp(state.previous.point.x, state.current.point.x, amount),
       std::lerp(state.previous.point.y, state.current.point.y, amount),
       std::lerp(state.previous.point.z, state.current.point.z, amount)},
      normal,
      false};
}

struct ActorShadowCachedReceiver {
  std::optional<ActorShadowReceiverPlane> stable;
  std::optional<ActorShadowReceiverPlane> pending;
  std::uint8_t pending_samples{};
  bool initialized{};
};

inline void alignActorShadowPlaneHemisphere(
    ActorShadowReceiverPlane &plane,
    const ActorShadowReceiverPlane &reference) noexcept {
  const auto alignment = plane.normal.x * reference.normal.x +
                         plane.normal.y * reference.normal.y +
                         plane.normal.z * reference.normal.z;
  if (alignment < 0.0) {
    plane.normal = {-plane.normal.x, -plane.normal.y, -plane.normal.z};
  }
}

[[nodiscard]] inline bool sameActorShadowReceiver(
    const std::optional<ActorShadowReceiverPlane> &first,
    const std::optional<ActorShadowReceiverPlane> &second) noexcept {
  if (!first || !second) {
    return !first && !second;
  }
  return sameActorShadowPlane(*first, *second);
}

inline void updateActorShadowReceiver(
    ActorShadowCachedReceiver &history,
    std::optional<ActorShadowReceiverPlane> candidate) noexcept {
  constexpr auto receiver_confirmation_samples = std::uint8_t{2U};
  if (!history.initialized) {
    history.stable = std::move(candidate);
    history.initialized = true;
    return;
  }
  // Adjacent triangles often describe the same physical receiver with
  // opposite winding.  Keep the cached plane in one normal hemisphere so its
  // projection bias cannot jump through the wall from one guest tick to the
  // next.
  if (candidate && history.stable) {
    alignActorShadowPlaneHemisphere(*candidate, *history.stable);
  } else if (candidate && history.pending) {
    alignActorShadowPlaneHemisphere(*candidate, *history.pending);
  }
  if (sameActorShadowReceiver(history.stable, candidate)) {
    history.stable = std::move(candidate);
    history.pending.reset();
    history.pending_samples = 0U;
    return;
  }
  if (!sameActorShadowReceiver(history.pending, candidate)) {
    history.pending = std::move(candidate);
    history.pending_samples = 1U;
    return;
  }
  history.pending_samples = static_cast<std::uint8_t>(std::min<unsigned int>(
      history.pending_samples + 1U, receiver_confirmation_samples));
  if (history.pending_samples >= receiver_confirmation_samples) {
    history.stable = std::move(history.pending);
    history.pending_samples = 0U;
  }
}

[[nodiscard]] inline bool
actorShadowReceiverIsWall(game::DynamicLightPoint receiver_normal,
                          game::DynamicLightPoint support_normal) noexcept {
  const auto length = [](game::DynamicLightPoint value) {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
  };
  const auto receiver_length = length(receiver_normal);
  const auto support_length = length(support_normal);
  if (!std::isfinite(receiver_length) || !std::isfinite(support_length) ||
      receiver_length <= 1.0e-6 || support_length <= 1.0e-6) {
    return false;
  }
  const auto alignment = std::abs((receiver_normal.x * support_normal.x +
                                   receiver_normal.y * support_normal.y +
                                   receiver_normal.z * support_normal.z) /
                                  (receiver_length * support_length));
  // Adjacent floor triangles may have opposite winding.  Use absolute normal
  // alignment so they remain one receiver family rather than masquerading as
  // a wall at their shared edge.
  return alignment < 0.72;
}

// Reuses the exact receiver plane found on the latest 20 Hz snapshot while
// posed vertices continue to interpolate at display refresh.  This removes a
// full mission-geometry raycast per HMD vertex per display frame.
[[nodiscard]] inline std::optional<game::DynamicLightPoint>
projectActorShadowOntoCachedPlane(
    game::DynamicLightPoint source, game::DynamicLightPoint target,
    const ActorShadowReceiverPlane &plane) noexcept {
  const auto direction = game::DynamicLightPoint{
      target.x - source.x, target.y - source.y, target.z - source.z};
  const auto denominator = direction.x * plane.normal.x +
                           direction.y * plane.normal.y +
                           direction.z * plane.normal.z;
  if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-7) {
    return std::nullopt;
  }
  const auto numerator = (plane.point.x - source.x) * plane.normal.x +
                         (plane.point.y - source.y) * plane.normal.y +
                         (plane.point.z - source.z) * plane.normal.z;
  const auto distance = numerator / denominator;
  if (!std::isfinite(distance) || distance < -0.01 || distance > 1.01) {
    return std::nullopt;
  }
  constexpr auto surface_bias = 4.0;
  return game::DynamicLightPoint{
      source.x + direction.x * std::clamp(distance, 0.0, 1.0) +
          plane.normal.x * surface_bias,
      source.y + direction.y * std::clamp(distance, 0.0, 1.0) +
          plane.normal.y * surface_bias,
      source.z + direction.z * std::clamp(distance, 0.0, 1.0) +
          plane.normal.z * surface_bias,
  };
}

} // namespace sf::platform
