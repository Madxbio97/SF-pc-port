#pragma once

#include "sf/game/hud.hpp"
#include "sf/game/legacy_bridge_types.hpp"

#include <cstddef>

namespace sf::platform {

// These utilities keep their authored English captions inside the original
// optic. The Russian font atlas reuses the same glyph UVs for Cyrillic, so
// drawing the untouched retail TEXT packets through that atlas produces
// transliterated garbage. The virus scanner uses the same packet path as the
// two rifle scopes even though it has no custom SCOPED.TIM frame.
[[nodiscard]] constexpr bool
usesRetailEnglishOpticText(game::WeaponId weapon) noexcept {
  return weapon == game::WeaponId::nightvision_rifle ||
         weapon == game::WeaponId::sniper_rifle ||
         weapon == game::WeaponId::virus_scanner;
}

[[nodiscard]] constexpr bool retailRifleScopeOverlayActive(
    bool first_person_aim, std::uint8_t interface_mode,
    std::uint8_t first_person_aim_mode) noexcept {
  return first_person_aim &&
         ((interface_mode == 2U && first_person_aim_mode == 2U) ||
          (interface_mode == 3U && first_person_aim_mode == 3U));
}

// The viral detector deliberately uses two different retail states: camera
// aim mode 4 selects first person, while INTERFACE mode 5 owns its 28-line
// sight and pulsing target dot.
[[nodiscard]] constexpr bool retailVirusScannerOverlayActive(
    bool first_person_aim, std::uint8_t interface_mode,
    std::uint8_t first_person_aim_mode) noexcept {
  return first_person_aim && interface_mode == 5U &&
         first_person_aim_mode == 4U;
}

// Retail scope captions live in the centered TEXT slots and have no backdrop.
// Classify the live packet itself instead of its optional source string:
// the bridge can legitimately observe an empty string or only the first few
// typewriter glyphs while the original scope label is being revealed.
[[nodiscard]] constexpr bool
isRetailScopeMessage(bool scoped, game::LegacyUiMessageChannel channel,
                     bool has_backdrop, std::size_t glyph_count) noexcept {
  return scoped && channel == game::LegacyUiMessageChannel::centered &&
         !has_backdrop && glyph_count != 0U;
}

[[nodiscard]] constexpr bool
useRetailEnglishScopeFont(bool scoped, bool russian_language,
                          game::LegacyUiMessageChannel channel,
                          bool has_backdrop, std::size_t glyph_count) noexcept {
  return russian_language &&
         isRetailScopeMessage(scoped, channel, has_backdrop, glyph_count);
}

} // namespace sf::platform
