#pragma once

#include <algorithm>
#include <cstddef>

namespace sf::platform {

// Map the guest's currently submitted typewriter packets onto a replacement
// string.  The guest remains authoritative for lifetime and reveal progress;
// presentation only changes wording/layout.  Keeping this independent of the
// selected language prevents English native messages from appearing fully
// formed while localized messages reveal one glyph at a time.
[[nodiscard]] constexpr std::size_t gameplayMessageVisibleGlyphCount(
    std::size_t source_glyph_count, std::size_t presented_source_glyphs,
    std::size_t presentation_glyph_count) noexcept {
  if (source_glyph_count == 0U || presentation_glyph_count == 0U) {
    return 0U;
  }
  presented_source_glyphs =
      std::min(source_glyph_count, presented_source_glyphs);
  return std::min(presentation_glyph_count,
                  (presented_source_glyphs * presentation_glyph_count +
                   source_glyph_count - 1U) /
                      source_glyph_count);
}

} // namespace sf::platform
