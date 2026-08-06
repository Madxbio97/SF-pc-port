#pragma once

#include <algorithm>
#include <cstddef>

namespace sf::platform {

struct GameplayMessageHorizontalLayout {
  int left{};
  int width{1};

  [[nodiscard]] friend constexpr bool
  operator==(const GameplayMessageHorizontalLayout &,
             const GameplayMessageHorizontalLayout &) noexcept = default;
};

// Localized gameplay copy is centered like the retail status channel, but its
// backing should follow the widest rendered line rather than the maximum wrap
// column. The margin is only a cap for long prose.
[[nodiscard]] constexpr GameplayMessageHorizontalLayout
gameplayMessageHorizontalLayout(int viewport_width, int horizontal_margin,
                                int rendered_line_width) noexcept {
  viewport_width = std::max(1, viewport_width);
  horizontal_margin =
      std::clamp(horizontal_margin, 0, (viewport_width - 1) / 2);
  const auto maximum_width =
      std::max(1, viewport_width - horizontal_margin * 2);
  const auto width = std::clamp(rendered_line_width, 1, maximum_width);
  return {(viewport_width - width) / 2, width};
}

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
