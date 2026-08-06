#pragma once

#include "sf/game/agent_mission_timer.hpp"

namespace sf::game {

inline constexpr std::int32_t retail_washington_park_timer_ticks =
    20 * 60 * 20;
inline constexpr std::int32_t agent_washington_park_timer_ticks =
    15 * 60 * 20;

[[nodiscard]] constexpr std::int32_t
agentWashingtonParkTimerAdjustedTicks(std::int32_t current) noexcept {
  return agentMissionTimerAdjustedTicks(current, agent_mission_timer_rules[0]);
}

} // namespace sf::game
