#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace sf::game {

enum class AgentMissionHudMeterKind : std::uint8_t {
  bomb_technician_health,
  aramov_escape,
  bomb_detonation,
  girdeux_tank_damage,
  suspicion,
  phagan_health,
  aramov_health,
};

enum class AgentMissionHudTone : std::uint8_t {
  neutral,
  friendly,
  warning,
  critical,
};

// Presentation receives only bounded scalar samples. Missing or malformed
// samples fail closed, so a transient guest/bridge read can never invent a
// mission state or alter the retail objective script.
struct AgentMissionHudSample {
  std::uint32_t current{};
  std::uint32_t maximum{};

  [[nodiscard]] friend constexpr bool
  operator==(const AgentMissionHudSample &,
             const AgentMissionHudSample &) = default;
};

struct AgentMissionHudRoutePoint {
  double x{};
  double y{};
  double z{};
};

template <typename Point, std::size_t Extent>
[[nodiscard]] std::optional<AgentMissionHudSample>
makeAgentMissionRouteProgressSample(
    std::span<const Point, Extent> route, AgentMissionHudRoutePoint home,
    AgentMissionHudRoutePoint position) noexcept {
  const auto finite = [](const auto &point) {
    return std::isfinite(static_cast<double>(point.x)) &&
           std::isfinite(static_cast<double>(point.y)) &&
           std::isfinite(static_cast<double>(point.z));
  };
  const auto distance = [](const auto &left, const auto &right) {
    return std::hypot(
        static_cast<double>(right.x) - static_cast<double>(left.x),
        static_cast<double>(right.y) - static_cast<double>(left.y),
        static_cast<double>(right.z) - static_cast<double>(left.z));
  };

  if (route.size() < 2U || !finite(home) || !finite(position)) {
    return std::nullopt;
  }
  for (const auto &point : route) {
    if (!finite(point)) {
      return std::nullopt;
    }
  }

  auto total_length = 0.0;
  for (std::size_t index = 1U; index < route.size(); ++index) {
    const auto length = distance(route[index - 1U], route[index]);
    if (!std::isfinite(length)) {
      return std::nullopt;
    }
    total_length += length;
  }
  if (!(total_length > 0.0) || !std::isfinite(total_length)) {
    return std::nullopt;
  }

  const auto home_to_front = distance(home, route.front());
  const auto home_to_back = distance(home, route.back());
  if (!std::isfinite(home_to_front) || !std::isfinite(home_to_back)) {
    return std::nullopt;
  }
  const auto starts_at_front = home_to_front <= home_to_back;

  auto best_distance = std::numeric_limits<double>::infinity();
  auto best_along = 0.0;
  auto traversed = 0.0;
  for (std::size_t index = 1U; index < route.size(); ++index) {
    const auto &from = route[index - 1U];
    const auto &to = route[index];
    const auto length = distance(from, to);
    if (length == 0.0) {
      continue;
    }
    const auto unit_x =
        (static_cast<double>(to.x) - static_cast<double>(from.x)) / length;
    const auto unit_y =
        (static_cast<double>(to.y) - static_cast<double>(from.y)) / length;
    const auto unit_z =
        (static_cast<double>(to.z) - static_cast<double>(from.z)) / length;
    const auto projected =
        std::clamp((position.x - static_cast<double>(from.x)) * unit_x +
                       (position.y - static_cast<double>(from.y)) * unit_y +
                       (position.z - static_cast<double>(from.z)) * unit_z,
                   0.0, length);
    const auto nearest = AgentMissionHudRoutePoint{
        static_cast<double>(from.x) + unit_x * projected,
        static_cast<double>(from.y) + unit_y * projected,
        static_cast<double>(from.z) + unit_z * projected};
    const auto separation = distance(position, nearest);
    if (separation < best_distance) {
      best_distance = separation;
      best_along = traversed + projected;
    }
    traversed += length;
  }
  if (!std::isfinite(best_distance)) {
    return std::nullopt;
  }

  const auto progress = starts_at_front
                            ? best_along / total_length
                            : (total_length - best_along) / total_length;
  const auto percent = std::clamp(std::lround(progress * 100.0), 0L, 100L);
  return AgentMissionHudSample{static_cast<std::uint32_t>(percent), 100U};
}

struct AgentMissionHudMeter {
  AgentMissionHudMeterKind kind{};
  std::uint8_t percent{};
  AgentMissionHudTone tone{AgentMissionHudTone::neutral};

  [[nodiscard]] friend constexpr bool
  operator==(const AgentMissionHudMeter &,
             const AgentMissionHudMeter &) = default;
};

struct AgentMissionHudState {
  std::optional<AgentMissionHudSample> bomb_technician_health;
  std::optional<AgentMissionHudSample> aramov_escape;
  std::optional<AgentMissionHudSample> bomb_detonation;
  // This is accumulated weak-point damage, not remaining tank health.
  std::optional<AgentMissionHudSample> girdeux_tank_damage;
  std::optional<AgentMissionHudSample> suspicion;
  std::optional<AgentMissionHudSample> phagan_health;
  std::optional<AgentMissionHudSample> aramov_health;
};

inline constexpr std::size_t maximum_agent_mission_hud_meters = 2U;

struct AgentMissionHudMeters {
  std::array<AgentMissionHudMeter, maximum_agent_mission_hud_meters> values{};
  std::size_t count{};

  [[nodiscard]] constexpr std::span<const AgentMissionHudMeter>
  entries() const noexcept {
    return {values.data(), count};
  }
};

[[nodiscard]] constexpr std::string_view
agentMissionHudLabel(AgentMissionHudMeterKind kind) noexcept {
  switch (kind) {
  case AgentMissionHudMeterKind::bomb_technician_health:
    return "BOMB TECH";
  case AgentMissionHudMeterKind::aramov_escape:
    return "ARAMOV ESCAPE";
  case AgentMissionHudMeterKind::bomb_detonation:
    return "BOMB DETONATION";
  case AgentMissionHudMeterKind::girdeux_tank_damage:
    return "FUEL TANK";
  case AgentMissionHudMeterKind::suspicion:
    return "SUSPICION";
  case AgentMissionHudMeterKind::phagan_health:
    return "PHAGAN";
  case AgentMissionHudMeterKind::aramov_health:
    return "ARAMOV";
  }
  return {};
}

[[nodiscard]] constexpr AgentMissionHudTone
agentMissionHudTone(AgentMissionHudMeterKind kind,
                    std::uint8_t percent) noexcept {
  switch (kind) {
  case AgentMissionHudMeterKind::bomb_technician_health:
  case AgentMissionHudMeterKind::phagan_health:
  case AgentMissionHudMeterKind::aramov_health:
    return percent <= 25U   ? AgentMissionHudTone::critical
           : percent <= 50U ? AgentMissionHudTone::warning
                            : AgentMissionHudTone::friendly;
  case AgentMissionHudMeterKind::girdeux_tank_damage:
    return percent >= 75U   ? AgentMissionHudTone::friendly
           : percent >= 50U ? AgentMissionHudTone::warning
                            : AgentMissionHudTone::neutral;
  case AgentMissionHudMeterKind::aramov_escape:
  case AgentMissionHudMeterKind::bomb_detonation:
  case AgentMissionHudMeterKind::suspicion:
    return percent >= 75U   ? AgentMissionHudTone::critical
           : percent >= 50U ? AgentMissionHudTone::warning
                            : AgentMissionHudTone::neutral;
  }
  return AgentMissionHudTone::neutral;
}

[[nodiscard]] constexpr std::optional<AgentMissionHudMeter>
makeAgentMissionHudMeter(AgentMissionHudMeterKind kind,
                         AgentMissionHudSample sample) noexcept {
  if (sample.maximum == 0U || agentMissionHudLabel(kind).empty()) {
    return std::nullopt;
  }
  const auto current = std::min(sample.current, sample.maximum);
  const auto numerator = static_cast<std::uint64_t>(current) * 100U +
                         static_cast<std::uint64_t>(sample.maximum) / 2U;
  const auto percent = static_cast<std::uint8_t>(
      std::min<std::uint64_t>(100U, numerator / sample.maximum));
  return AgentMissionHudMeter{kind, percent,
                              agentMissionHudTone(kind, percent)};
}

// Exact mission routing prevents stale values from a checkpoint or previous
// level from leaking onto another Agent mission's HUD. Retail scripts remain
// the sole owners of success, failure, actors and timers.
[[nodiscard]] constexpr AgentMissionHudMeters
makeAgentMissionHudMeters(std::uint32_t mission_index,
                          const AgentMissionHudState &state) noexcept {
  AgentMissionHudMeters result;
  const auto append = [&](AgentMissionHudMeterKind kind,
                          const std::optional<AgentMissionHudSample> &sample) {
    if (!sample || result.count >= result.values.size()) {
      return;
    }
    if (const auto meter = makeAgentMissionHudMeter(kind, *sample)) {
      result.values[result.count++] = *meter;
    }
  };

  switch (mission_index) {
  case 1U:
    append(AgentMissionHudMeterKind::bomb_technician_health,
           state.bomb_technician_health);
    break;
  case 2U:
    append(AgentMissionHudMeterKind::aramov_escape, state.aramov_escape);
    break;
  case 4U:
    append(AgentMissionHudMeterKind::bomb_detonation, state.bomb_detonation);
    append(AgentMissionHudMeterKind::girdeux_tank_damage,
           state.girdeux_tank_damage);
    break;
  case 5U:
    append(AgentMissionHudMeterKind::suspicion, state.suspicion);
    break;
  case 6U:
    append(AgentMissionHudMeterKind::phagan_health, state.phagan_health);
    append(AgentMissionHudMeterKind::aramov_health, state.aramov_health);
    break;
  default:
    break;
  }
  return result;
}

} // namespace sf::game
