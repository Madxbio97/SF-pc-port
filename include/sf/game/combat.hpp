#pragma once

#include "sf/game/actor_animation.hpp"
#include "sf/game/hud.hpp"

#include <cstdint>
#include <string_view>

namespace sf::game {

enum class WeaponFireMode : std::uint8_t {
    none,
    semi_automatic,
    automatic,
    continuous,
    projectile,
    thrown,
    utility,
};

enum class WeaponDamageKind : std::uint8_t {
    none,
    ballistic,
    pellet,
    electrical,
    fire,
    explosive,
    gas,
};

struct WeaponCombatDefinition {
    WeaponId id{WeaponId::unarmed};
    WeaponFireMode fire_mode{WeaponFireMode::none};
    WeaponDamageKind damage_kind{WeaponDamageKind::none};
    std::uint16_t close_damage{};
    std::uint16_t distant_damage{};
    std::uint16_t maximum_range{};
    unsigned int fire_interval_updates{};
    std::uint8_t pellet_count{1U};
    std::uint16_t spread_angle{};
    std::string_view world_model;

    [[nodiscard]] constexpr bool fires() const noexcept {
        return fire_mode != WeaponFireMode::none &&
            fire_mode != WeaponFireMode::utility;
    }
    [[nodiscard]] constexpr bool automatic() const noexcept {
        return fire_mode == WeaponFireMode::automatic ||
            fire_mode == WeaponFireMode::continuous;
    }
    [[nodiscard]] constexpr std::uint16_t damageAtDistance(
        double distance) const noexcept {
        // FUN_80026130/FUN_80033xxx select the packed close value through
        // distance 0x3c0 and the second value from 0x3c1 onward.
        return distance <= 960.0 ? close_damage : distant_damage;
    }
};

// Combat values are recovered from the 0x20-byte SCUS_942.40 item records at
// 0x8010c38c. Close damage is (LE32(record + 0x0c) >> 6) & 0x3ff; distant
// damage is LE16(record + 0x0e) & 0x3ff. Timing is expressed in retail 20 Hz
// simulation ticks.
[[nodiscard]] const WeaponCombatDefinition& weaponCombatDefinition(WeaponId id) noexcept;
[[nodiscard]] PlayerWeaponStance weaponStance(WeaponId id) noexcept;
// FUN_80045c04 resolves ordinary floor-drop identities through record+4.
// The host uses these GMD identities only for authored size metadata; visible
// floor pickups are always TIM billboards. Armour uses the VEST.GMD bounds.
[[nodiscard]] std::string_view
droppedItemWorldModel(std::uint16_t item) noexcept;

struct DamageResult {
    std::uint16_t armor_damage{};
    std::uint16_t health_damage{};
    bool killed{};
};

[[nodiscard]] DamageResult applyPlayerDamage(
    PlayerVitals& vitals,
    std::uint16_t damage,
    bool bypass_armor = false) noexcept;

} // namespace sf::game
