#pragma once

#include "sf/assets/tim_image.hpp"

#include <cstdint>

namespace sf::platform::detail {

// Russian FONTA/B/C are stored at twice the retail pixel density.  PsyCross
// exposes an RGBA override texture whose logical dimensions can remain 128x128;
// existing UVs and screen-space metrics therefore keep their exact layout
// while each logical font pixel is backed by four source texels.
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
