#pragma once

#include "sf/assets/tim_image.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sf::platform::detail {

inline constexpr std::size_t texture_page_bytes = 64U * 256U * 2U;
inline constexpr std::size_t clut_bytes = 256U * 32U * 2U;
inline constexpr std::uint16_t mission_clut_resident_x = 0U;
inline constexpr std::uint16_t mission_clut_resident_y = 192U;
inline constexpr std::uint16_t hud_source_vram_x = 768U;
inline constexpr std::uint16_t hud_resident_vram_x = 0U;

[[nodiscard]] unsigned int physicalTexturePage(unsigned int page) noexcept;
[[nodiscard]] std::uint32_t validateVlf(std::span<const std::byte> bytes);
[[nodiscard]] std::span<const std::byte>
vlfPage(std::span<const std::byte> bytes, std::uint32_t page_mask,
        unsigned int page);
[[nodiscard]] std::span<const std::byte>
vlfClut(std::span<const std::byte> bytes, std::uint32_t page_mask);

void uploadTexturePage(unsigned int page, std::span<const std::byte> bytes);
void uploadTexturePageAt(unsigned int physical_page,
                         std::span<const std::byte> bytes);
void uploadClut(std::span<const std::byte> bytes);

[[nodiscard]] int texturePageMode(assets::TimPixelMode mode) noexcept;
[[nodiscard]] unsigned int
timTexturePage(const assets::TimImage &image) noexcept;
void uploadTimBlockAt(const assets::TimBlock &block,
                      std::uint16_t destination_x, std::uint16_t destination_y);
void uploadTimBlock(const assets::TimBlock &block);

[[nodiscard]] constexpr std::uint16_t
hudResidentX(std::uint16_t source_x) noexcept {
  return static_cast<std::uint16_t>(hud_resident_vram_x + source_x -
                                    hud_source_vram_x);
}

void uploadHudPixels(const assets::TimBlock &block);

} // namespace sf::platform::detail
