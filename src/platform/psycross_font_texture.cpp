#include "psycross_font_texture.hpp"

#include "sf/core/error.hpp"

#include <PsyX/PsyX_render.h>
#include <psx/libgpu.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace sf::platform::detail {
namespace {

constexpr int retail_atlas_size = 128;
constexpr int hd_atlas_size = retail_atlas_size * 2;
constexpr int logical_atlas_size = retail_atlas_size;
constexpr std::uint16_t expected_base_x = 832U;

std::uint8_t expand5(std::uint16_t value) noexcept {
  const auto channel = static_cast<std::uint8_t>(value & 31U);
  return static_cast<std::uint8_t>((channel << 3U) | (channel >> 2U));
}

void copySheet(const assets::TimImage &image, std::span<std::uint8_t> rgba,
               unsigned int atlas_size) {
  if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut() ||
      image.clut()->words.size() < 256U || image.pixels().x < expected_base_x) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Font sheet is not an indexed 8-bit TIM"};
  }
  const auto &pixels = image.pixels();
  const auto relative_x =
      static_cast<unsigned int>(pixels.x - expected_base_x) * 2U;
  const auto relative_y = static_cast<unsigned int>(pixels.y);
  const auto width = static_cast<unsigned int>(image.displayWidth());
  const auto height = static_cast<unsigned int>(image.displayHeight());
  if (relative_x + width > atlas_size || relative_y + height > atlas_size) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Font sheet exceeds its native atlas"};
  }

  for (auto row = 0U; row < height; ++row) {
    for (auto column = 0U; column < width; ++column) {
      const auto packed =
          pixels.words[static_cast<std::size_t>(row) * pixels.width_words +
                       column / 2U];
      const auto palette_index = static_cast<std::uint8_t>(
          column % 2U == 0U ? packed & 0xffU : packed >> 8U);
      const auto color = image.clut()->words[palette_index];
      const auto destination =
          (static_cast<std::size_t>(relative_y + row) * atlas_size +
           relative_x + column) *
          4U;
      rgba[destination] = expand5(color);
      rgba[destination + 1U] = expand5(color >> 5U);
      rgba[destination + 2U] = expand5(color >> 10U);
      rgba[destination + 3U] = (color & 0x7fffU) == 0U ? 0U : 255U;
    }
  }
}

} // namespace

PsyCrossFontTexture::PsyCrossFontTexture(const assets::TimImage &font_a,
                                         const assets::TimImage &font_b,
                                         const assets::TimImage &font_c) {
  const auto retail_layout =
      font_a.displayWidth() == 32U && font_a.displayHeight() == 64U &&
      font_b.displayWidth() == 32U && font_b.displayHeight() == 64U &&
      font_c.displayWidth() == 42U && font_c.displayHeight() == 123U;
  const auto hd_layout =
      font_a.displayWidth() == 64U && font_a.displayHeight() == 128U &&
      font_b.displayWidth() == 64U && font_b.displayHeight() == 128U &&
      font_c.displayWidth() == 84U && font_c.displayHeight() == 246U;
  if (!retail_layout && !hd_layout) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Font pack is neither the retail 1x nor native 2x atlas"};
  }
  const auto physical_atlas_size =
      static_cast<unsigned int>(hd_layout ? hd_atlas_size : retail_atlas_size);
  std::vector<std::uint8_t> rgba(
      static_cast<std::size_t>(physical_atlas_size) * physical_atlas_size * 4U,
      0U);
  for (const auto &font :
       std::array{std::cref(font_a), std::cref(font_b), std::cref(font_c)}) {
    copySheet(font.get(), rgba, physical_atlas_size);
  }
  texture_ = GR_CreateRGBATexture(physical_atlas_size, physical_atlas_size,
                                  rgba.data());
  if (texture_ == 0U) {
    throw core::Error{core::ErrorCode::io,
                      "Failed to create the native HD font texture"};
  }
}

PsyCrossFontTexture::~PsyCrossFontTexture() {
  if (texture_ != 0U) {
    GR_DestroyTexture(texture_);
  }
}

void PsyCrossFontTexture::bind() const noexcept {
  DR_PSYX_TEX command{};
  SetPsyXTexture(&command, texture_, logical_atlas_size, logical_atlas_size);
  DrawPrim(&command);
}

void PsyCrossFontTexture::restoreVram() noexcept {
  DR_PSYX_TEX command{};
  SetPsyXTexture(&command, 0U, 0, 0);
  DrawPrim(&command);
}

ScopedPsyCrossFontTexture::ScopedPsyCrossFontTexture(
    const PsyCrossFontTexture *texture) noexcept
    : texture_(texture) {
  if (texture_ != nullptr) {
    texture_->bind();
  }
}

ScopedPsyCrossFontTexture::~ScopedPsyCrossFontTexture() {
  if (texture_ != nullptr) {
    PsyCrossFontTexture::restoreVram();
  }
}

} // namespace sf::platform::detail
