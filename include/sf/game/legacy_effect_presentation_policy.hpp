#pragma once

#include "sf/game/legacy_bridge_types.hpp"

#include <cstdint>
#include <span>

namespace sf::game {

// Exact guest GsSPRITE packets own SPFX presentation only when both the
// retail camera list and its particle bridge were captured for this frame.
[[nodiscard]] constexpr bool legacyGuestEffectsAuthoritative(
    bool guest_camera_lists_captured,
    bool legacy_effect_particles_authoritative) noexcept {
  return guest_camera_lists_captured && legacy_effect_particles_authoritative;
}

// A complete camera list owns only the EXPL particle whose stable pool index
// and family match the embedded GsSPRITE provenance. Distant live particles
// retain native fallback presentation until retail links them.
[[nodiscard]] constexpr bool legacyGuestSpriteCoversExplParticle(
    std::int16_t pool_index, std::uint8_t family,
    const LegacyGuestSpriteBridgeState &sprite,
    bool guest_camera_lists_captured,
    bool legacy_effect_particles_authoritative) noexcept {
  return legacyGuestEffectsAuthoritative(
             guest_camera_lists_captured,
             legacy_effect_particles_authoritative) &&
         pool_index >= 0 && sprite.effect_particle == pool_index &&
         sprite.effect_family == family;
}

[[nodiscard]] inline bool legacyExplParticleHasGuestSprite(
    std::int16_t pool_index, std::uint8_t family,
    std::span<const LegacyGuestSpriteBridgeState> sprites,
    bool guest_camera_lists_captured,
    bool legacy_effect_particles_authoritative) noexcept {
  for (const auto &sprite : sprites) {
    if (legacyGuestSpriteCoversExplParticle(
            pool_index, family, sprite, guest_camera_lists_captured,
            legacy_effect_particles_authoritative)) {
      return true;
    }
  }
  return false;
}

} // namespace sf::game
