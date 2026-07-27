#pragma once

#include "sf/assets/tim_image.hpp"

#include <cstdint>

namespace sf::platform::detail {

// PsyCross exposes FONTA/B/C as one RGBA override texture with the retail
// 128x128 logical layout. Both the original 1x sheets and the Russian 2x sheets
// use the same guest UVs, so callers can switch atlases without rewriting any
// completed retail glyph packets.
class PsyCrossFontTexture final {
public:
  PsyCrossFontTexture(const assets::TimImage &font_a,
                      const assets::TimImage &font_b,
                      const assets::TimImage &font_c);
  ~PsyCrossFontTexture();

  PsyCrossFontTexture(const PsyCrossFontTexture &) = delete;
  PsyCrossFontTexture &operator=(const PsyCrossFontTexture &) = delete;

  void bind() const noexcept;
  static void restoreVram() noexcept;

private:
  unsigned int texture_{};
};

class ScopedPsyCrossFontTexture final {
public:
  explicit ScopedPsyCrossFontTexture(
      const PsyCrossFontTexture *texture) noexcept;
  ~ScopedPsyCrossFontTexture();

  ScopedPsyCrossFontTexture(const ScopedPsyCrossFontTexture &) = delete;
  ScopedPsyCrossFontTexture &
  operator=(const ScopedPsyCrossFontTexture &) = delete;

private:
  const PsyCrossFontTexture *texture_{};
};

} // namespace sf::platform::detail
