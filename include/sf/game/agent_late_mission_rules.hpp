#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace sf::game {

inline constexpr std::uint16_t agent_elite_guard_frag_grenade_bit = 0x1000U;
inline constexpr std::uint16_t agent_elite_guard_gas_grenade_bit = 0x2000U;
inline constexpr std::uint8_t agent_elite_guard_grenade_counter_floor = 0x0cU;

[[nodiscard]] constexpr bool
agentEliteGuardGrenadeCadenceEligible(std::uint16_t mission,
                                      std::uint16_t attributes, bool common_npc,
                                      bool active, bool alive) noexcept {
  return mission == 15U && common_npc && active && alive &&
         (attributes & agent_elite_guard_frag_grenade_bit) != 0U &&
         (attributes & agent_elite_guard_gas_grenade_bit) == 0U;
}

[[nodiscard]] constexpr std::uint8_t
agentEliteGuardGrenadeDecisionCounter(std::uint8_t retail_counter,
                                      bool eligible) noexcept {
  return eligible && retail_counter < agent_elite_guard_grenade_counter_floor
             ? agent_elite_guard_grenade_counter_floor
             : retail_counter;
}

inline constexpr std::uint16_t agent_tunnel_blackout_mission = 18U;
inline constexpr std::uint32_t agent_flashlight_target_memory_frames = 100U;

[[nodiscard]] constexpr std::uint32_t agentEnemyTargetMemoryFrames(
    std::uint16_t mission, bool agent_mode, bool flashlight_active,
    std::uint32_t retail_frames = 40U,
    std::uint32_t agent_frames = 80U,
    std::uint32_t flashlight_frames =
        agent_flashlight_target_memory_frames) noexcept {
  if (!agent_mode) {
    return retail_frames;
  }
  return mission == agent_tunnel_blackout_mission && flashlight_active &&
                 flashlight_frames > agent_frames
             ? flashlight_frames
             : agent_frames;
}

struct AgentCbdcFriendlyFireIdentity {
  std::uint16_t mission{};
  std::uint16_t source{};
  std::uint16_t definition{};
  std::uint16_t object_class{};

  [[nodiscard]] friend constexpr bool
  operator==(const AgentCbdcFriendlyFireIdentity &,
             const AgentCbdcFriendlyFireIdentity &) = default;
};

inline constexpr std::array agent_cbdc_friendly_fire_identities{
    AgentCbdcFriendlyFireIdentity{3U, 7U, 32U, 0x35U},
    AgentCbdcFriendlyFireIdentity{3U, 8U, 32U, 0x35U},
    AgentCbdcFriendlyFireIdentity{3U, 16U, 9U, 0x35U},
    AgentCbdcFriendlyFireIdentity{3U, 17U, 11U, 0x35U},
    AgentCbdcFriendlyFireIdentity{3U, 18U, 57U, 0x35U},
    AgentCbdcFriendlyFireIdentity{3U, 19U, 19U, 0x35U},
};

[[nodiscard]] constexpr bool agentCbdcFriendlyFireTarget(
    std::uint16_t mission, std::uint16_t source, std::uint16_t definition,
    std::uint16_t object_class) noexcept {
  for (const auto &identity : agent_cbdc_friendly_fire_identities) {
    if (identity == AgentCbdcFriendlyFireIdentity{
                        mission, source, definition, object_class}) {
      return true;
    }
  }
  return false;
}

inline constexpr std::int32_t agent_cbdc_friendly_fire_penalty_ticks =
    30 * 20;

[[nodiscard]] constexpr std::int32_t agentCbdcFriendlyFireAdjustedTicks(
    std::int32_t current, std::uint32_t penalties = 1U) noexcept {
  if (current <= 0 || penalties == 0U) {
    return current;
  }
  constexpr auto penalty =
      static_cast<std::uint64_t>(agent_cbdc_friendly_fire_penalty_ticks);
  const auto total = penalty * penalties;
  return total >= static_cast<std::uint64_t>(current)
             ? 0
             : current - static_cast<std::int32_t>(total);
}

} // namespace sf::game
