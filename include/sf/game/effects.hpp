#pragma once

#include "sf/assets/mission_objects.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace sf::game {

struct EffectPoint {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t z{};

  friend bool operator==(const EffectPoint &, const EffectPoint &) = default;
};

// Class 0x30 starts its EXPL particles at the authored object origin. The
// effects system raises the native Y coordinate by 0x70 before projection;
// viewer Y is inverted, hence the negative bias here.
[[nodiscard]] EffectPoint
cfireSpawnPoint(const assets::MissionTransform &transform) noexcept;

struct EffectVramRect {
  std::int16_t x{};
  std::int16_t y{};
  std::int16_t width{};
  std::int16_t height{};

  friend bool operator==(const EffectVramRect &,
                         const EffectVramRect &) = default;
};

struct EffectTextureCopy {
  EffectVramRect source;
  std::int16_t destination_x{};
  std::int16_t destination_y{};

  friend bool operator==(const EffectTextureCopy &,
                         const EffectTextureCopy &) = default;
};

struct PoliceLightbarFrame {
  EffectTextureCopy blue;
  EffectTextureCopy red;

  friend bool operator==(const PoliceLightbarFrame &,
                         const PoliceLightbarFrame &) = default;
};

// LIGHT.GMD is opaque geometry. The original effect animates its red and blue
// texture cells every two retail 20 Hz gameplay ticks rather than drawing glow
// quads.
[[nodiscard]] const PoliceLightbarFrame &
policeLightbarFrame(std::uint64_t gameplay_tick) noexcept;

enum class ObjectDamageResponse : std::uint8_t {
  none,
  shatter,
  extinguish,
  breakable,
  explosive,
  vehicle,
};

inline constexpr std::uint32_t legacy_cfire_a_class = 0x30U;
inline constexpr std::uint32_t legacy_cfire_b_class = 0x25U;
inline constexpr std::uint32_t legacy_cfire_c_class = 0x27U;

[[nodiscard]] bool
legacyFireEmitterPresentation(std::uint32_t class_id,
                              std::string_view model_name) noexcept;

// Retail overlays dispatch shot callbacks by the class stored in the mission
// object definition. A small number of overlays reuse a class entry for a
// different resource, so model_name participates in those exact identities.
[[nodiscard]] ObjectDamageResponse
objectDamageResponse(std::uint32_t class_id,
                     std::string_view model_name = {}) noexcept;

// Retail death callbacks can clear their health controller before the generic
// object record is sampled. instance+0 bit 7 is the common destroyed latch
// used by glass, lights and other destructible props, so it remains
// authoritative even when the live health record has already gone away.
[[nodiscard]] constexpr bool
legacyGuestDestructionStateAuthoritative(ObjectDamageResponse response,
                                         std::int16_t maximum_health,
                                         bool destroyed_latched) noexcept {
  return maximum_health > 0 ||
         (response != ObjectDamageResponse::none && destroyed_latched);
}

// A streamed prop may temporarily lose its health controller and be rebuilt
// with a live-looking generic record. Destruction is monotonic for the current
// retail object identity; dynamic-slot rebinding resets the presentation state
// separately when a genuinely new identity occupies the slot.
[[nodiscard]] constexpr bool
legacyGuestDestroyedState(ObjectDamageResponse response,
                          bool previously_destroyed,
                          bool guest_destroyed) noexcept {
  return guest_destroyed ||
         (response != ObjectDamageResponse::none && previously_destroyed);
}

// Destructible static props commonly enter the dormant state at the same
// instant as their death callback. A secondary model remains drawable in that
// state; the dormant bit suppresses only objects with no post-destruction
// presentation.
[[nodiscard]] constexpr bool legacyGuestStaticPropPresentationAllowed(
    bool dormant, bool destroyed, bool has_secondary_model,
    ObjectDamageResponse response) noexcept {
  return !dormant || (destroyed && has_secondary_model &&
                      response != ObjectDamageResponse::none);
}

enum class GameplayEffectType : std::uint8_t {
  muzzle_flash,
  blood_spray,
  blood_decal,
  explosion,
  burning_fire,
};

enum class GameplayEffectAttachment : std::uint8_t {
  world,
  player_muzzle,
  npc_muzzle,
  player_body,
  npc_body,
};

struct GameplayEffect {
  GameplayEffectType type{GameplayEffectType::muzzle_flash};
  double x{};
  double y{};
  double z{};
  double direction_x{};
  double direction_y{};
  double direction_z{};
  double scale{1.0};
  std::uint16_t remaining_updates{};
  std::uint16_t total_updates{};
  std::uint32_t seed{};
  GameplayEffectAttachment attachment{GameplayEffectAttachment::world};
  std::uint16_t owner_object{};
  double attachment_offset_x{};
  double attachment_offset_y{};
  double attachment_offset_z{};
};

// Native muzzle effects also live for a single 20 Hz update. Keep every new
// flash sampled during catch-up until a host frame has presented it, mirroring
// the queue used for retail weapon/effect edges.
class GameplayMuzzleFlashPresentationQueue final {
public:
  void observe(std::span<const GameplayEffect> effects);

  [[nodiscard]] std::span<const GameplayEffect> flashes() const noexcept {
    return pending_flashes_;
  }

  void consumeFrame() noexcept;
  void reset() noexcept;

private:
  std::vector<GameplayEffect> pending_flashes_;
  std::vector<GameplayEffect> latest_flashes_;
  bool frame_consumed_{true};
};

// Retail camera lists own their blood, impact and player-shot presentation.
// Enemy fire has no corresponding weapon edge in the player-only hook, so its
// actor-attached native muzzle effect remains the one intentional supplement.
// First-person aim hides only Gabe's own barrel, which sits in front of the
// optic camera. Enemy fire remains a world-space effect and must stay visible.
[[nodiscard]] constexpr bool
nativeGameplayEffectPresentationAllowed(const GameplayEffect &effect,
                                        bool guest_effects_authoritative,
                                        bool first_person_aim) noexcept {
  if (effect.type == GameplayEffectType::muzzle_flash && first_person_aim &&
      effect.attachment == GameplayEffectAttachment::player_muzzle) {
    return false;
  }
  return !guest_effects_authoritative ||
         (effect.type == GameplayEffectType::muzzle_flash &&
          effect.attachment == GameplayEffectAttachment::npc_muzzle);
}

[[nodiscard]] GameplayEffect
makeGameplayEffect(GameplayEffectType type, double x, double y, double z,
                   double direction_x, double direction_y, double direction_z,
                   double scale, std::uint32_t seed) noexcept;

// Returns false once the effect has reached its native finite lifetime.
[[nodiscard]] bool advanceGameplayEffect(GameplayEffect &effect) noexcept;

} // namespace sf::game
