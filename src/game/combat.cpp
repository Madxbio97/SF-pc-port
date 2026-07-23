#include "sf/game/combat.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace sf::game {
namespace {

// The original item record packs two 10-bit damage values across bytes 0x0c..0x0f.
// FUN_80026130 and the player targeting path use close damage through 0x3c0
// world units, then distant damage. Targeting itself uses a 32000-unit ceiling;
// shorter ranges below are reserved for native projectile/beam effects.
// The cadence field is bits 26..30 of record+0x0c. Automatic weapons retain
// their second retail burst controller; this table preserves the packed
// per-round field instead of substituting host-tuned timings.
constexpr std::array definitions{
    WeaponCombatDefinition{WeaponId::unarmed, WeaponFireMode::none,
        WeaponDamageKind::none, 15U, 0U},
    WeaponCombatDefinition{WeaponId::silenced_9mm, WeaponFireMode::semi_automatic,
        WeaponDamageKind::ballistic, 50U, 25U, 32000U, 4U, 1U, 0U, "GLOKSIL"},
    WeaponCombatDefinition{WeaponId::pistol_9mm, WeaponFireMode::semi_automatic,
        WeaponDamageKind::ballistic, 50U, 25U, 32000U, 12U, 1U, 0U, "GLOCK17"},
    WeaponCombatDefinition{WeaponId::unused_357, WeaponFireMode::none,
        WeaponDamageKind::none, 15U, 0U},
    WeaponCombatDefinition{WeaponId::pistol_45, WeaponFireMode::semi_automatic,
        WeaponDamageKind::ballistic, 150U, 75U, 32000U, 8U, 1U, 0U, "COLT45"},
    WeaponCombatDefinition{WeaponId::g_18, WeaponFireMode::automatic,
        WeaponDamageKind::ballistic, 50U, 25U, 32000U, 1U, 1U, 20U, "GLOCK18"},
    WeaponCombatDefinition{WeaponId::combat_shotgun, WeaponFireMode::semi_automatic,
        WeaponDamageKind::pellet, 300U, 150U, 6000U, 18U, 8U, 96U, "BERELLI"},
    WeaponCombatDefinition{WeaponId::shotgun, WeaponFireMode::semi_automatic,
        WeaponDamageKind::pellet, 200U, 75U, 6000U, 15U, 8U, 112U, "ITHICA37"},
    WeaponCombatDefinition{WeaponId::pk_102, WeaponFireMode::automatic,
        WeaponDamageKind::ballistic, 50U, 25U, 32000U, 2U, 1U, 28U, "AK102"},
    WeaponCombatDefinition{WeaponId::m_16, WeaponFireMode::automatic,
        WeaponDamageKind::ballistic, 50U, 25U, 32000U, 2U, 1U, 24U, "M16"},
    WeaponCombatDefinition{WeaponId::biz_2, WeaponFireMode::automatic,
        WeaponDamageKind::ballistic, 75U, 35U, 32000U, 2U, 1U, 32U, "BIZON2"},
    WeaponCombatDefinition{WeaponId::hk_5, WeaponFireMode::automatic,
        WeaponDamageKind::ballistic, 50U, 25U, 32000U, 2U, 1U, 24U, "MP5"},
    WeaponCombatDefinition{WeaponId::nightvision_rifle, WeaponFireMode::semi_automatic,
        WeaponDamageKind::ballistic, 90U, 90U, 32000U, 20U, 1U, 0U, "DRAGSVD"},
    WeaponCombatDefinition{WeaponId::sniper_rifle, WeaponFireMode::semi_automatic,
        WeaponDamageKind::ballistic, 50U, 25U, 32000U, 4U, 1U, 0U, "SUPERG"},
    WeaponCombatDefinition{WeaponId::taser, WeaponFireMode::continuous,
        WeaponDamageKind::electrical, 100U, 0U, 6000U, 0U, 1U, 0U, "TASER"},
    WeaponCombatDefinition{WeaponId::flamethrower, WeaponFireMode::continuous,
        WeaponDamageKind::fire, 50U, 50U, 2400U, 2U, 1U, 80U, "FLAMEGDF"},
    WeaponCombatDefinition{WeaponId::m_79, WeaponFireMode::projectile,
        WeaponDamageKind::explosive, 600U, 600U, 480U, 20U, 1U, 0U, "GRENLAUN"},
    WeaponCombatDefinition{WeaponId::k3g4, WeaponFireMode::automatic,
        WeaponDamageKind::ballistic, 50U, 25U, 32000U, 2U, 1U, 20U, "G3"},
    WeaponCombatDefinition{WeaponId::virus_scanner, WeaponFireMode::utility,
        WeaponDamageKind::none, 0U, 0U, 0U, 0U, 1U, 0U, "FLASHLT"},
    WeaponCombatDefinition{WeaponId::fragmentation_grenade, WeaponFireMode::thrown,
        WeaponDamageKind::explosive, 600U, 0U, 480U, 15U, 1U, 0U, "GRENADE"},
    WeaponCombatDefinition{WeaponId::gas_grenade, WeaponFireMode::thrown,
        WeaponDamageKind::gas, 600U, 0U, 480U, 15U, 1U, 0U, "GRENADE"},
    WeaponCombatDefinition{WeaponId::flashlight, WeaponFireMode::utility,
        WeaponDamageKind::none, 0U, 0U, 0U, 0U, 1U, 0U, "FLASHLT"},
    WeaponCombatDefinition{WeaponId::chopper_gun, WeaponFireMode::automatic,
        WeaponDamageKind::ballistic, 75U, 75U, 32000U, 2U, 1U, 32U, "CHNGUN"},
    WeaponCombatDefinition{WeaponId::key_card, WeaponFireMode::utility},
    WeaponCombatDefinition{WeaponId::c4_explosives, WeaponFireMode::utility},
    WeaponCombatDefinition{WeaponId::viral_antigen, WeaponFireMode::utility},
};

static_assert(definitions.size() == weapon_slot_count);

} // namespace

const WeaponCombatDefinition& weaponCombatDefinition(WeaponId id) noexcept {
    if (!isValidWeaponId(id)) {
        return definitions.front();
    }
    return definitions[static_cast<std::size_t>(id)];
}

PlayerWeaponStance weaponStance(WeaponId weapon) noexcept {
    switch (weapon) {
    case WeaponId::silenced_9mm:
    case WeaponId::pistol_9mm:
    case WeaponId::pistol_45:
    case WeaponId::g_18:
    case WeaponId::taser:
    case WeaponId::virus_scanner:
    case WeaponId::fragmentation_grenade:
    case WeaponId::gas_grenade:
    case WeaponId::flashlight:
        return PlayerWeaponStance::sidearm;
    case WeaponId::combat_shotgun:
    case WeaponId::shotgun:
    case WeaponId::pk_102:
    case WeaponId::m_16:
    case WeaponId::biz_2:
    case WeaponId::hk_5:
    case WeaponId::nightvision_rifle:
    case WeaponId::sniper_rifle:
    case WeaponId::flamethrower:
    case WeaponId::m_79:
    case WeaponId::k3g4:
    case WeaponId::chopper_gun:
        return PlayerWeaponStance::long_gun;
    case WeaponId::unarmed:
    case WeaponId::unused_357:
    case WeaponId::key_card:
    case WeaponId::c4_explosives:
    case WeaponId::viral_antigen:
    default:
        return PlayerWeaponStance::unarmed;
    }
}

DamageResult applyPlayerDamage(
    PlayerVitals& vitals,
    std::uint16_t damage,
    bool bypass_armor) noexcept {
    DamageResult result;
    // FUN_80068770 routes one complete hit either to armor or to health. If
    // armor reaches zero, the remainder of that same hit is discarded.
    if (!bypass_armor && vitals.armor != 0U) {
        result.armor_damage = std::min(vitals.armor, damage);
        vitals.armor = static_cast<std::uint16_t>(vitals.armor - result.armor_damage);
    } else {
        result.health_damage = std::min(vitals.health, damage);
        vitals.health = static_cast<std::uint16_t>(vitals.health - result.health_damage);
    }
    result.killed = vitals.health == 0U;
    return result;
}

} // namespace sf::game
