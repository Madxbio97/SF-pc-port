#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace sf::game {

struct AgentMissionTimerRule {
  std::uint32_t mission_index{};
  std::uint32_t expiry_callback{};
  std::uint32_t retail_seconds{};
  std::uint32_t agent_seconds{};
};

inline constexpr std::uint32_t agent_timer_ticks_per_second = 20U;
inline constexpr std::uint32_t agent_base_escape_timer_callback = 0x80148824U;
inline constexpr std::int32_t retail_base_escape_timer_ticks = 3 * 60 * 20;
inline constexpr std::int32_t agent_base_escape_timer_ticks = 144 * 20;
inline constexpr std::uint32_t agent_warehouse_76_timer_callback =
    0x80146eb0U;
inline constexpr std::int32_t retail_warehouse_76_timer_ticks = 15 * 60 * 20;
inline constexpr std::int32_t agent_warehouse_76_timer_ticks = 12 * 60 * 20;

inline constexpr std::array agent_mission_timer_rules{
    AgentMissionTimerRule{3U, 0x80146a64U, 20U * 60U, 15U * 60U},
    AgentMissionTimerRule{10U, agent_base_escape_timer_callback, 3U * 60U,
                          144U},
    AgentMissionTimerRule{16U, agent_warehouse_76_timer_callback, 15U * 60U,
                          12U * 60U},
};

[[nodiscard]] constexpr const AgentMissionTimerRule *
agentMissionTimerRule(std::uint32_t mission_index) noexcept {
  for (const auto &rule : agent_mission_timer_rules) {
    if (rule.mission_index == mission_index) {
      return &rule;
    }
  }
  return nullptr;
}

[[nodiscard]] constexpr std::int32_t agentMissionTimerAdjustedTicks(
    std::int32_t current, const AgentMissionTimerRule &rule) noexcept {
  const auto retail_ticks = static_cast<std::int32_t>(
      rule.retail_seconds * agent_timer_ticks_per_second);
  const auto agent_ticks = static_cast<std::int32_t>(
      rule.agent_seconds * agent_timer_ticks_per_second);
  if (current <= agent_ticks) {
    return current;
  }
  const auto reduced = current - (retail_ticks - agent_ticks);
  return reduced > agent_ticks ? agent_ticks : reduced;
}

[[nodiscard]] constexpr std::int32_t
agentBaseEscapeTimerAdjustedTicks(std::int32_t current) noexcept {
  return agentMissionTimerAdjustedTicks(current, agent_mission_timer_rules[1]);
}

[[nodiscard]] constexpr std::int32_t
agentWarehouse76TimerAdjustedTicks(std::int32_t current) noexcept {
  return agentMissionTimerAdjustedTicks(current, agent_mission_timer_rules[2]);
}

[[nodiscard]] constexpr std::string_view agentMissionParameterText(
    std::uint32_t mission_index, bool agent_mode,
    std::string_view retail_text) noexcept {
  constexpr std::string_view warehouse_retail =
      "Get out before the building collapses in 15 minutes";
  constexpr std::string_view warehouse_agent =
      "Get out before the building collapses in 12 minutes";
  return agent_mode && mission_index == 16U && retail_text == warehouse_retail
             ? warehouse_agent
             : retail_text;
}

} // namespace sf::game
