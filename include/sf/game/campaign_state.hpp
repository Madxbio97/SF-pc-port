#pragma once

#include "sf/game/hud.hpp"

#include <array>
#include <cstdint>

namespace sf::game {

// Durable subset of the retail mission inventory. Objective items and the
// mounted chopper gun deliberately remain mission-local; ordinary weapons,
// ammunition and Gabe's vitals continue through a connected mission group.
struct CampaignCarryState {
  std::uint8_t current_weapon{};
  std::uint32_t owned_weapons{};
  std::array<std::uint16_t, weapon_slot_count> magazines{};
  std::array<std::uint16_t, weapon_slot_count> reserves{};
  std::uint16_t health{};
  std::uint16_t armor{};

  friend bool operator==(const CampaignCarryState &,
                         const CampaignCarryState &) = default;
};

inline constexpr std::uint32_t campaign_persistent_weapon_mask =
    (std::uint32_t{1U} << static_cast<unsigned>(WeaponId::chopper_gun)) - 1U;

// Retail continuation groups, expressed in zero-based mission indices:
// 1-5, 6-7, 8-11, 12-14 and 15-20.
[[nodiscard]] constexpr bool campaignMissionsShareCarry(
    std::uint32_t completed_mission, std::uint32_t next_mission) noexcept {
  if (next_mission != completed_mission + 1U) {
    return false;
  }
  return completed_mission <= 3U || completed_mission == 5U ||
         (completed_mission >= 7U && completed_mission <= 9U) ||
         (completed_mission >= 11U && completed_mission <= 12U) ||
         (completed_mission >= 14U && completed_mission <= 18U);
}

[[nodiscard]] constexpr bool
validCampaignCarry(const CampaignCarryState &state) noexcept {
  const auto current = static_cast<unsigned>(state.current_weapon);
  return (state.owned_weapons & ~campaign_persistent_weapon_mask) == 0U &&
         current < weapon_slot_count &&
         state.health <= 0x7fffU && state.armor <= 0x7fffU &&
         (state.owned_weapons & (std::uint32_t{1U} << current)) != 0U;
}

} // namespace sf::game
