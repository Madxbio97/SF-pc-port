#pragma once

#include "sf/game/legacy_bridge_types.hpp"

#include <cstddef>

namespace sf::platform {

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
