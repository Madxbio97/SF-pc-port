#pragma once

#include "sf/assets/tim_image.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace sf::platform::detail {

inline constexpr std::size_t texture_page_bytes = 64U * 256U * 2U;
inline constexpr unsigned int psx_texture_page_count = 32U;
// TPAGE bits 10..15 are host-only in this port. Code zero selects native PSX
// VRAM; codes 1..63 select the complete host alias atlas. Keeping every
// possible (logical page, texture bank) identity resident avoids order-based
// eviction of actor textures during room transitions.
inline constexpr unsigned int extended_texture_page_count = 63U;
inline constexpr unsigned int resident_texture_page_count =
    psx_texture_page_count + extended_texture_page_count;
inline constexpr unsigned int maximum_scene_texture_identities =
    psx_texture_page_count * 2U + 1U;
static_assert(resident_texture_page_count - 6U >=
              maximum_scene_texture_identities);
inline constexpr unsigned int resident_texture_page_token_bits = 7U;
static_assert((1U << resident_texture_page_token_bits) >=
              resident_texture_page_count);
inline constexpr std::size_t clut_bytes = 256U * 32U * 2U;
inline constexpr std::uint16_t mission_clut_resident_x = 0U;
inline constexpr std::uint16_t mission_clut_resident_y = 192U;
inline constexpr std::uint16_t hud_source_vram_x = 768U;
inline constexpr std::uint16_t hud_resident_vram_x = 0U;

// Retail missions may contain only VRAM.HOG. Bank one is a valid renderer
// selector only when VRAM1.HOG exists; for a single-bank mission it aliases
// the sole authored bank instead of becoming an out-of-range archive access.
[[nodiscard]] constexpr int
canonicalMissionTextureBank(int requested_bank,
                            std::size_t available_banks) noexcept {
  return available_banks == 1U && requested_bank == 1 ? 0 : requested_bank;
}

[[nodiscard]] unsigned int physicalTexturePage(unsigned int page) noexcept;
[[nodiscard]] constexpr std::uint16_t
encodeResidentTexturePage(std::uint16_t tpage,
                          unsigned int physical_page) noexcept {
  if (physical_page < psx_texture_page_count) {
    return static_cast<std::uint16_t>((tpage & 0x03e0U) | physical_page);
  }
  const auto extension = physical_page - psx_texture_page_count;
  if (extension >= extended_texture_page_count) {
    return tpage;
  }
  return static_cast<std::uint16_t>(
      (tpage & 0x03e0U) |
      static_cast<std::uint16_t>((extension + 1U) << 10U));
}
[[nodiscard]] constexpr std::uint64_t
residentTexturePageToken(std::uint64_t generation,
                         unsigned int physical_page) noexcept {
  return (generation << resident_texture_page_token_bits) | physical_page;
}
[[nodiscard]] std::uint32_t validateVlf(std::span<const std::byte> bytes);
[[nodiscard]] std::span<const std::byte>
vlfPage(std::span<const std::byte> bytes, std::uint32_t page_mask,
        unsigned int page);
[[nodiscard]] std::span<const std::byte>
vlfClut(std::span<const std::byte> bytes, std::uint32_t page_mask);
[[nodiscard]] bool
texturePageMatchesVlf(std::span<const std::byte> vlf,
                      std::uint32_t page_mask, unsigned int page,
                      std::span<const std::byte> texture_bank_page);

void uploadTexturePage(unsigned int page, std::span<const std::byte> bytes);
void uploadTexturePageAt(unsigned int physical_page,
                         std::span<const std::byte> bytes);
void readTexturePageAt(unsigned int physical_page,
                       std::span<std::uint16_t> words);
void uploadTexturePageBlockAt(unsigned int physical_page,
                              const assets::TimBlock &block,
                              unsigned int local_x, unsigned int local_y);
void copyTexturePageRectangle(std::span<const std::byte> source,
                              unsigned int source_x, unsigned int source_y,
                              std::span<std::byte> destination,
                              unsigned int destination_x,
                              unsigned int destination_y, unsigned int width,
                              unsigned int height,
                              std::span<std::byte> scratch);
[[nodiscard]] bool texturePageNeedsAuthoredReload(
    std::span<const std::byte> resident, std::span<const std::byte> authored,
    bool runtime_mutated) noexcept;
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
