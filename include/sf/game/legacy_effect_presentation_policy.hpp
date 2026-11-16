#pragma once

#include "sf/game/legacy_bridge_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sf::game {

struct LegacyExplParticlePresentationSample {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t z{};
  std::uint16_t controller{};
  std::int16_t source_slot{-1};
  std::uint8_t family{};
  std::uint8_t scale_byte{};
  std::uint8_t frame{};
  bool attached_explosion_sequence{};
  std::int16_t pool_index{-1};
};

struct LegacyExplParticlePresentationPosition {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t z{};

  [[nodiscard]] friend constexpr bool operator==(
      const LegacyExplParticlePresentationPosition &,
      const LegacyExplParticlePresentationPosition &) noexcept = default;
};

inline constexpr std::int64_t
    legacy_expl_particle_maximum_interpolated_displacement = 2048;

[[nodiscard]] constexpr bool legacyExplParticlePresentationContinuous(
    const LegacyExplParticlePresentationSample &previous,
    const LegacyExplParticlePresentationSample &current) noexcept {
  if (previous.pool_index < 0 || current.pool_index < 0 ||
      previous.pool_index != current.pool_index ||
      previous.controller != current.controller ||
      previous.source_slot != current.source_slot ||
      previous.family != current.family ||
      previous.attached_explosion_sequence !=
          current.attached_explosion_sequence ||
      previous.scale_byte != current.scale_byte ||
      current.frame < previous.frame) {
    return false;
  }
  const auto dx = static_cast<std::int64_t>(current.x) - previous.x;
  const auto dy = static_cast<std::int64_t>(current.y) - previous.y;
  const auto dz = static_cast<std::int64_t>(current.z) - previous.z;
  constexpr auto maximum =
      legacy_expl_particle_maximum_interpolated_displacement;
  if (dx < -maximum || dx > maximum || dy < -maximum || dy > maximum ||
      dz < -maximum || dz > maximum) {
    return false;
  }
  return dx * dx + dy * dy + dz * dz <= maximum * maximum;
}

[[nodiscard]] constexpr LegacyExplParticlePresentationPosition
interpolateLegacyExplParticlePosition(
    const LegacyExplParticlePresentationSample &previous,
    const LegacyExplParticlePresentationSample &current,
    double amount) noexcept {
  const auto current_position = LegacyExplParticlePresentationPosition{
      current.x, current.y, current.z};
  if (!legacyExplParticlePresentationContinuous(previous, current)) {
    return current_position;
  }
  amount = std::clamp(amount, 0.0, 1.0);
  const auto component = [amount](std::int32_t from, std::int32_t to) {
    const auto value = static_cast<double>(from) +
                       (static_cast<double>(to) - from) * amount;
    return static_cast<std::int32_t>(value >= 0.0 ? value + 0.5
                                                  : value - 0.5);
  };
  return {
      component(previous.x, current.x),
      component(previous.y, current.y),
      component(previous.z, current.z),
  };
}

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

// Retail CFIRE controllers use the EXPL family for their two flame particles;
// some overlays use the FIRE family directly. Either family proves that the
// authored emitter already has a live retail-owned presentation.
[[nodiscard]] constexpr bool legacyExplParticlePresentsAuthoredFire(
    LegacyEffectSpriteFamily family) noexcept {
  return family == LegacyEffectSpriteFamily::explosion ||
         family == LegacyEffectSpriteFamily::fire;
}

// EXPL density is restricted to a proven authored CFIRE owner. FIRE is itself
// an exact retail fire family and may use its high-resolution frame globally;
// PARK2 is a separate FT4 ribbon bridge and never reaches either predicate.
[[nodiscard]] constexpr bool legacyCfireUsesRetailExplFrameVolume(
    std::uint32_t, bool authored_cfire_owned,
    LegacyEffectSpriteFamily family) noexcept {
  return authored_cfire_owned &&
         family == LegacyEffectSpriteFamily::explosion;
}

[[nodiscard]] constexpr bool legacyCfireUsesRetailFireFrameVolume(
    std::uint32_t, bool,
    LegacyEffectSpriteFamily family) noexcept {
  return family == LegacyEffectSpriteFamily::fire;
}

inline constexpr std::uint8_t legacy_cfire_attached_explosion_scale = 57U;

// A missing source slot may be recovered only from the exact attached EXPL
// sequence and retail CFIRE scale. Spatial/group ownership is checked later;
// this predicate alone never grants ownership to an ordinary explosion.
[[nodiscard]] constexpr bool legacyUnboundCfireParticleCandidate(
    LegacyEffectSpriteFamily family, std::uint8_t scale_byte,
    bool attached_explosion_sequence) noexcept {
  return attached_explosion_sequence &&
         family == LegacyEffectSpriteFamily::explosion &&
         scale_byte == legacy_cfire_attached_explosion_scale;
}

// Smoke, gas and explosions carry their animation, tint and blend mode in
// the retail quad. A light halo alone may be represented entirely by a volume.
[[nodiscard]] constexpr bool
legacyVolumeReplacesHalo(const LegacyGuestSpriteBridgeState &sprite, bool steam,
                         bool volume_submitted) noexcept {
  return volume_submitted && sprite.force_fullbright &&
         sprite.effect_family == 0U && !steam;
}

// Some missions assign CFIRE guest slots only when their authored room becomes
// presentation-visible. Texture residency must lead that handoff on every
// connected resource route. This is resource-only and never grants render
// ownership, activation or particle lifetime.
[[nodiscard]] constexpr bool legacyCfireSpritePreloadAllowed(
    std::uint32_t, bool bridge_authoritative,
    bool cfire_geometry, bool destroyed, bool initially_hidden,
    bool scene_active, bool authored_owner_on_connected_route) noexcept {
  return bridge_authoritative && cfire_geometry && !destroyed &&
         (!initially_hidden || scene_active) &&
         authored_owner_on_connected_route;
}

[[nodiscard]] constexpr bool legacyDistantFireEmitterAllowed(
    bool bridge_authoritative, bool scene_active, bool authored_owner_presented,
    std::int32_t scene_guest_slot, bool has_live_owned_particle) noexcept {
  return bridge_authoritative && scene_active && authored_owner_presented &&
         scene_guest_slot >= 0 && !has_live_owned_particle;
}

struct LegacyHaloPresentationDescriptor {
  double red{1.0};
  double green{1.0};
  double blue{1.0};
  double radius_scale{1.0};
  double density{0.5};
  double emission{0.75};
  double light_radius_scale{1.0};
  double light_intensity_scale{0.75};
};

// The update7/render3 lamp path is explicitly full-bright. Its captured
// GsSPRITE RGB can already contain the retail far-colour/depth cue, so feeding
// that packet colour back into the native volume changes the authored CLUT hue
// as the lamp recedes. The retail fallback uses neutral modulation for the
// same reason; keep both presentation paths identical.
[[nodiscard]] constexpr LegacyRgbBridgeState legacyHaloBackColor(
    LegacyRgbBridgeState packet_color, bool force_fullbright) noexcept {
  return force_fullbright ? LegacyRgbBridgeState{128U, 128U, 128U}
                          : packet_color;
}

// Emissive volumes cannot be mixed with the far colour: additive blending
// would inject the fog chroma into their hue. Fade only their energy once the
// authored depth cue is already substantial, reaching zero inside opaque fog.
// Q12 zero/one correspond to no fog/full fog in the retail GTE path.
[[nodiscard]] constexpr double
legacyHaloFogVisibility(std::int32_t depth_cue_q12) noexcept {
  constexpr auto q12_one = 4096.0;
  constexpr auto fade_onset = 0.40;
  constexpr auto fade_end = 0.96;
  const auto fog = std::clamp(static_cast<double>(depth_cue_q12) / q12_one,
                              0.0, 1.0);
  const auto amount =
      std::clamp((fog - fade_onset) / (fade_end - fade_onset), 0.0, 1.0);
  const auto smooth = amount * amount * (3.0 - 2.0 * amount);
  return 1.0 - smooth;
}

// PS1 halo hue comes from its CLUT while GsSPRITE/GMD RGB supplies authored
// back-colour modulation. Keep hue normalized and carry brightness through
// the emission/light multipliers so neutral lamps remain neutral.
[[nodiscard]] constexpr LegacyHaloPresentationDescriptor
legacyHaloPresentationDescriptor(
    LegacyRgbBridgeState clut_color,
    LegacyRgbBridgeState back_color = {128U, 128U, 128U}) noexcept {
  if (back_color.red == 0U && back_color.green == 0U &&
      back_color.blue == 0U) {
    back_color = {128U, 128U, 128U};
  }
  const auto modulated = [](std::uint8_t palette, std::uint8_t back) {
    return std::clamp(static_cast<double>(palette) *
                          static_cast<double>(back) /
                          128.0,
                      0.0, 255.0);
  };
  auto red = modulated(clut_color.red, back_color.red);
  auto green = modulated(clut_color.green, back_color.green);
  auto blue = modulated(clut_color.blue, back_color.blue);
  auto peak = std::max({red, green, blue});
  if (peak <= 0.0) {
    red = green = blue = peak = 128.0;
  }
  const auto energy = std::clamp(peak / 248.0, 0.30, 1.0);
  return {red / peak,
          green / peak,
          blue / peak,
          1.05 + energy * 0.18,
          0.46 + energy * 0.16,
          0.62 + energy * 0.30,
          0.88 + energy * 0.24,
          0.55 + energy * 0.35};
}

// Sample only palette entries referenced by this indexed sprite. Luminance
// weighting rejects the transparent/dark fringe while retaining the authored
// hue of the hot centre.
[[nodiscard]] inline std::optional<LegacyRgbBridgeState>
legacyIndexedSpriteClutColor(std::span<const std::byte> page_bytes,
                             std::span<const std::byte> clut_bytes,
                             std::size_t clut_base_word,
                             unsigned int texture_mode, unsigned int u,
                             unsigned int v, unsigned int width,
                             unsigned int height) noexcept {
  constexpr auto page_width_words = 64U;
  constexpr auto page_height = 256U;
  constexpr auto word_bytes = 2U;
  if ((texture_mode != 0U && texture_mode != 1U) || width == 0U ||
      height == 0U || u >= 256U || v >= page_height ||
      page_bytes.size() < page_width_words * page_height * word_bytes) {
    return std::nullopt;
  }
  const auto pixels_per_word = texture_mode == 0U ? 4U : 2U;
  const auto palette_entries = texture_mode == 0U ? 16U : 256U;
  if (clut_base_word + palette_entries > clut_bytes.size() / word_bytes) {
    return std::nullopt;
  }
  const auto read_word = [](std::span<const std::byte> bytes,
                            std::size_t word) {
    const auto offset = word * word_bytes;
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(
             std::to_integer<std::uint8_t>(bytes[offset + 1U]))
         << 8U));
  };
  const auto bounded_width = std::min(width, 256U - u);
  const auto bounded_height = std::min(height, page_height - v);
  const auto stride = std::max(
      1U, std::max(bounded_width, bounded_height) / 24U);
  std::uint64_t red_sum{};
  std::uint64_t green_sum{};
  std::uint64_t blue_sum{};
  std::uint64_t weight_sum{};
  for (auto local_y = 0U; local_y < bounded_height; local_y += stride) {
    for (auto local_x = 0U; local_x < bounded_width; local_x += stride) {
      const auto pixel_x = u + local_x;
      const auto packed = read_word(
          page_bytes,
          (v + local_y) * page_width_words + pixel_x / pixels_per_word);
      const auto shift =
          (pixel_x % pixels_per_word) * (texture_mode == 0U ? 4U : 8U);
      const auto index = static_cast<unsigned int>(
          (packed >> shift) & (texture_mode == 0U ? 0x0fU : 0xffU));
      const auto color = read_word(clut_bytes, clut_base_word + index);
      if ((color & 0x7fffU) == 0U) {
        continue;
      }
      const auto decode = [](std::uint16_t five_bit) {
        return static_cast<std::uint32_t>((five_bit * 255U + 15U) / 31U);
      };
      const auto red = decode(color & 0x1fU);
      const auto green = decode((color >> 5U) & 0x1fU);
      const auto blue = decode((color >> 10U) & 0x1fU);
      const auto weight = std::max(
          1U, (red * 54U + green * 183U + blue * 19U) / 256U);
      red_sum += static_cast<std::uint64_t>(red) * weight;
      green_sum += static_cast<std::uint64_t>(green) * weight;
      blue_sum += static_cast<std::uint64_t>(blue) * weight;
      weight_sum += weight;
    }
  }
  if (weight_sum == 0U) {
    return std::nullopt;
  }
  const auto channel = [weight_sum](std::uint64_t sum) {
    return static_cast<std::uint8_t>(
        std::min<std::uint64_t>((sum + weight_sum / 2U) / weight_sum, 255U));
  };
  return LegacyRgbBridgeState{channel(red_sum), channel(green_sum),
                              channel(blue_sum)};
}

struct LegacyCfireVolumeTuning {
  double radius_scale{1.0};
  double density_scale{1.0};
  double emission_scale{1.0};
  double light_radius_scale{1.0};
  double light_intensity_scale{1.0};
};

// Volume and matching dynamic-light presentation remain mission-neutral. Any
// duplicate CFIRE presentation must be resolved by ownership, not dimming.
[[nodiscard]] constexpr LegacyCfireVolumeTuning
legacyCfireVolumeTuning(std::uint32_t, bool) noexcept {
  return {};
}

} // namespace sf::game
