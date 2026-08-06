#pragma once

#include "sf/game/hud.hpp"
#include "sf/game/legacy_bridge_types.hpp"

#include <cstdint>

namespace sf::game {

[[nodiscard]] constexpr std::uint8_t agentPark2BombDetonationIncrement(
    LegacyWeaponEventType type, WeaponId weapon) noexcept {
  if (type == LegacyWeaponEventType::thrown) {
    return weapon == WeaponId::fragmentation_grenade ||
                   weapon == WeaponId::gas_grenade
               ? std::uint8_t{100U}
               : std::uint8_t{};
  }
  if (type != LegacyWeaponEventType::shot) {
    return 0U;
  }

  switch (weapon) {
  case WeaponId::combat_shotgun:
  case WeaponId::shotgun:
    return 50U;
  case WeaponId::pistol_45:
    return 40U;
  case WeaponId::m_16:
    return 10U;
  case WeaponId::silenced_9mm:
  case WeaponId::pistol_9mm:
  case WeaponId::g_18:
  case WeaponId::pk_102:
  case WeaponId::biz_2:
  case WeaponId::hk_5:
  case WeaponId::nightvision_rifle:
  case WeaponId::sniper_rifle:
  case WeaponId::k3g4:
    return 2U;
  case WeaponId::m_79:
    return 100U;
  case WeaponId::taser:
  case WeaponId::flashlight:
  default:
    return 0U;
  }
}

[[nodiscard]] constexpr std::uint8_t applyAgentPark2BombDetonation(
    std::uint8_t current, LegacyWeaponEventType type,
    WeaponId weapon) noexcept {
  constexpr auto maximum = std::uint8_t{100U};
  if (current >= maximum) {
    return maximum;
  }
  const auto increment = agentPark2BombDetonationIncrement(type, weapon);
  return increment >= static_cast<std::uint8_t>(maximum - current)
             ? maximum
             : static_cast<std::uint8_t>(current + increment);
}

} // namespace sf::game
