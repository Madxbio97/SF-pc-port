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

[[nodiscard]] constexpr bool legacyExplParticleOwnedByGuestSlot(
    std::int16_t particle_source_slot,
    std::int32_t scene_guest_slot) noexcept {
  return scene_guest_slot >= 0 && particle_source_slot == scene_guest_slot;
}

[[nodiscard]] constexpr bool legacyDistantFireEmitterAllowed(
    bool bridge_authoritative, bool scene_active,
    bool authored_owner_active, bool authored_owner_warm,
    bool authored_initially_hidden, std::int32_t scene_guest_slot,
    bool has_live_owned_particle) noexcept {
  const auto lifecycle_presented =
      (authored_owner_active && scene_active) ||
      (authored_owner_warm && !authored_initially_hidden);
  return bridge_authoritative && lifecycle_presented && scene_guest_slot >= 0 &&
         !has_live_owned_particle;
}

} // namespace sf::game
