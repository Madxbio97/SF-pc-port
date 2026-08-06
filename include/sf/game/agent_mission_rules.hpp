#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>

namespace sf::game {

inline constexpr std::uint16_t agent_marcos_frag_grenade_bit = 0x1000U;
inline constexpr std::uint16_t agent_marcos_gas_grenade_bit = 0x2000U;
inline constexpr std::uint8_t agent_marcos_grenade_counter_floor = 0x24U;
inline constexpr std::uint8_t agent_kravitch_shotgun_cadence = 15U;
inline constexpr std::uint8_t agent_kravitch_reposition_counter_floor = 0x28U;

struct AgentMissionNpcIdentity {
  std::uint16_t mission{};
  std::uint16_t slot{};
  std::uint16_t definition{};
  std::int16_t room{};
  std::uint16_t retail_attributes{};
  std::int32_t authored_x{};
  std::int32_t authored_y{};
  std::int32_t authored_z{};
};

// USA v1.1 BASEEXT.DAT: source 98 is the only definition-38 VLADI.HMD
// actor, linked to source 81 and placed in room 41. Slot, definition and the
// authored position are all checked before the runtime override is applied.
inline constexpr AgentMissionNpcIdentity agent_gabrek_identity{
    7U, 98U, 38U, 41, 0xe102U, -826, 0, -7036};

// USA v1.1 CHURCH2.DAT: these three common guards occupy the room containing
// the catacomb-entry activation source (53) and its linked doorway (59), or
// the immediately adjoining room. No other CHURCH2 NPC shares these exact
// source/definition/position tuples.
inline constexpr std::array agent_chapel_guard_identities{
    AgentMissionNpcIdentity{12U, 179U, 28U, 44, 0x8308U, -3317, 3080, -6217},
    AgentMissionNpcIdentity{12U, 181U, 28U, 44, 0x8105U, -3369, 3079, -6422},
    AgentMissionNpcIdentity{12U, 182U, 28U, 43, 0x8105U, -4568, 3079, -6639},
};

[[nodiscard]] constexpr std::uint16_t
agentKravitchAttributes(std::uint16_t attributes, bool enabled) noexcept {
  constexpr auto weapon_mask = std::uint16_t{0x00ffU};
  constexpr auto glock_17 = std::uint16_t{2U};
  constexpr auto shotgun = std::uint16_t{7U};
  constexpr auto legacy_agent_m_16 = std::uint16_t{9U};
  const auto weapon = static_cast<std::uint16_t>(attributes & weapon_mask);
  if (enabled && (weapon == glock_17 || weapon == legacy_agent_m_16)) {
    return static_cast<std::uint16_t>((attributes & ~weapon_mask) | shotgun);
  }
  if (!enabled && (weapon == shotgun || weapon == legacy_agent_m_16)) {
    return static_cast<std::uint16_t>((attributes & ~weapon_mask) | glock_17);
  }
  return attributes;
}

[[nodiscard]] constexpr std::uint8_t
agentKravitchPostShotCooldown(std::uint8_t retail_cooldown,
                              bool enabled) noexcept {
  if (!enabled || retail_cooldown <= agent_kravitch_shotgun_cadence) {
    return retail_cooldown;
  }
  const auto halved = static_cast<std::uint8_t>(
      (static_cast<std::uint16_t>(retail_cooldown) + 1U) / 2U);
  return std::max(agent_kravitch_shotgun_cadence, halved);
}

[[nodiscard]] constexpr std::uint8_t
agentKravitchPostShotDecisionCounter(std::uint8_t retail_counter,
                                     bool enabled) noexcept {
  return enabled && retail_counter < agent_kravitch_reposition_counter_floor
             ? agent_kravitch_reposition_counter_floor
             : retail_counter;
}

[[nodiscard]] constexpr std::uint16_t
agentMarcosAttributes(std::uint16_t attributes, bool enabled) noexcept {
  constexpr auto grenade_mask = static_cast<std::uint16_t>(
      agent_marcos_frag_grenade_bit | agent_marcos_gas_grenade_bit);
  constexpr auto weapon_mask = std::uint16_t{0x00ffU};
  constexpr auto frag_grenade = std::uint16_t{19U};
  constexpr auto gas_grenade = std::uint16_t{20U};
  auto retail = static_cast<std::uint16_t>(attributes & ~grenade_mask);
  if ((retail & weapon_mask) == gas_grenade) {
    retail = static_cast<std::uint16_t>((retail & ~weapon_mask) | frag_grenade);
  }
  return enabled ? static_cast<std::uint16_t>(retail |
                                              agent_marcos_frag_grenade_bit)
                 : retail;
}

[[nodiscard]] constexpr std::uint8_t
agentMarcosGrenadeDecisionCounter(std::uint8_t retail_counter,
                                  bool enabled) noexcept {
  return enabled && retail_counter < agent_marcos_grenade_counter_floor
             ? agent_marcos_grenade_counter_floor
             : retail_counter;
}

[[nodiscard]] constexpr std::uint16_t
agentGabrekAttributes(std::uint16_t attributes, bool enabled) noexcept {
  constexpr auto weapon_mask = std::uint16_t{0x00ffU};
  constexpr auto grenade_mask = static_cast<std::uint16_t>(
      agent_marcos_frag_grenade_bit | agent_marcos_gas_grenade_bit);
  constexpr auto m_16 = std::uint16_t{9U};
  constexpr auto retail = agent_gabrek_identity.retail_attributes;
  constexpr auto agent =
      static_cast<std::uint16_t>(((retail & ~weapon_mask) & ~grenade_mask) |
                                 m_16 | agent_marcos_frag_grenade_bit);
  if (enabled && attributes == retail) {
    return agent;
  }
  if (!enabled && attributes == agent) {
    return retail;
  }
  return attributes;
}

[[nodiscard]] constexpr std::uint16_t
agentChapelGuardAttributes(std::uint16_t attributes,
                           std::uint16_t retail_attributes,
                           bool enabled) noexcept {
  constexpr auto weapon_mask = std::uint16_t{0x00ffU};
  constexpr auto shotgun = std::uint16_t{7U};
  const auto agent =
      static_cast<std::uint16_t>((retail_attributes & ~weapon_mask) | shotgun);
  if (enabled && attributes == retail_attributes) {
    return agent;
  }
  if (!enabled && attributes == agent) {
    return retail_attributes;
  }
  return attributes;
}

[[nodiscard]] constexpr bool agentChapelGuardMaintainedAttributesEligible(
    std::uint16_t attributes, std::uint16_t retail_attributes) noexcept {
  const auto agent =
      agentChapelGuardAttributes(retail_attributes, retail_attributes, true);
  return attributes == retail_attributes || attributes == agent;
}

[[nodiscard]] constexpr std::int32_t
agentAramovRootMotionDelta(std::int32_t retail_delta) noexcept {
  const auto scaled = static_cast<std::int64_t>(retail_delta) * 5 / 4;
  return static_cast<std::int32_t>(
      std::clamp<std::int64_t>(scaled, std::numeric_limits<std::int32_t>::min(),
                               std::numeric_limits<std::int32_t>::max()));
}

} // namespace sf::game
