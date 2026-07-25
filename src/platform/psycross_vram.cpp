#include "psycross_vram.hpp"

#include "sf/core/error.hpp"

#include <psx/libgte.h>
#include <psx/libgpu.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace sf::platform::detail {
namespace {

[[nodiscard]] std::uint16_t readLe16(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint16_t>(bytes[offset]) |
      (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

[[nodiscard]] std::uint32_t readLe32(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] std::vector<u_long>
packVramWords(std::span<const std::byte> bytes) {
  if ((bytes.size() & 1U) != 0U) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "VRAM payload has an odd byte count"};
  }
  const auto word_count = bytes.size() / 2U;
  std::vector<u_long> result((word_count + 1U) / 2U);
  for (std::size_t index = 0; index < word_count; ++index) {
    const auto value = readLe16(bytes, index * 2U);
    result[index / 2U] |= static_cast<u_long>(value) << ((index & 1U) * 16U);
  }
  return result;
}

[[nodiscard]] RECT16 texturePageRect(unsigned int page) noexcept {
  const auto physical_page = physicalTexturePage(page);
  return RECT16{
      static_cast<short>((physical_page & 15U) * 64U),
      static_cast<short>(physical_page > 15U ? 256 : 0),
      64,
      256,
  };
}

[[nodiscard]] RECT16
physicalTexturePageRect(unsigned int physical_page) noexcept {
  return RECT16{
      static_cast<short>((physical_page & 15U) * 64U),
      static_cast<short>(physical_page > 15U ? 256 : 0),
      64,
      256,
  };
}

} // namespace

unsigned int physicalTexturePage(unsigned int page) noexcept {
  return (page & 15U) < 6U ? page + 6U : page;
}

std::uint32_t validateVlf(std::span<const std::byte> bytes) {
  if (bytes.size() < 4U) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "VLF texture map is truncated"};
  }
  const auto page_mask = readLe32(bytes, 0U);
  const auto page_count = static_cast<std::size_t>(std::popcount(page_mask));
  const auto expected_size = page_count * texture_page_bytes + clut_bytes;
  if (bytes.size() != expected_size) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "VLF texture map size is inconsistent"};
  }
  return page_mask;
}

std::span<const std::byte> vlfPage(std::span<const std::byte> bytes,
                                   std::uint32_t page_mask, unsigned int page) {
  if (page >= 32U || (page_mask & (1U << page)) == 0U) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "VLF texture page is not present"};
  }
  const auto preceding = page == 0U ? 0U : page_mask & ((1U << page) - 1U);
  const auto offset =
      static_cast<std::size_t>(std::popcount(preceding)) * texture_page_bytes;
  return bytes.subspan(offset, texture_page_bytes);
}

std::span<const std::byte> vlfClut(std::span<const std::byte> bytes,
                                   std::uint32_t page_mask) {
  const auto offset =
      static_cast<std::size_t>(std::popcount(page_mask)) * texture_page_bytes;
  return bytes.subspan(offset, clut_bytes);
}

void uploadTexturePage(unsigned int page, std::span<const std::byte> bytes) {
  if (bytes.size() != texture_page_bytes) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Mission texture page size is invalid"};
  }
  auto rect = texturePageRect(page);
  auto packed = packVramWords(bytes);
  LoadImage(&rect, packed.data());
}

void uploadTexturePageAt(unsigned int physical_page,
                         std::span<const std::byte> bytes) {
  if (physical_page >= 32U || bytes.size() != texture_page_bytes) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Mission texture page size is invalid"};
  }
  auto rect = physicalTexturePageRect(physical_page);
  auto packed = packVramWords(bytes);
  LoadImage(&rect, packed.data());
}

void uploadClut(std::span<const std::byte> bytes) {
  if (bytes.size() != clut_bytes) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Mission CLUT size is invalid"};
  }
  RECT16 rect{static_cast<short>(mission_clut_resident_x),
              static_cast<short>(mission_clut_resident_y), 256, 32};
  auto packed = packVramWords(bytes);
  LoadImage(&rect, packed.data());
}

int texturePageMode(assets::TimPixelMode mode) noexcept {
  switch (mode) {
  case assets::TimPixelMode::indexed4:
    return 0;
  case assets::TimPixelMode::indexed8:
    return 1;
  case assets::TimPixelMode::direct16:
    return 2;
  case assets::TimPixelMode::direct24:
    return 3;
  }
  return 2;
}

unsigned int timTexturePage(const assets::TimImage &image) noexcept {
  const auto &pixels = image.pixels();
  return static_cast<unsigned int>(pixels.x / 64U) +
         (static_cast<unsigned int>(pixels.y / 256U) * 16U);
}

void uploadTimBlockAt(const assets::TimBlock &block,
                      std::uint16_t destination_x,
                      std::uint16_t destination_y) {
  const auto checked = [](std::uint16_t value) {
    if (value > static_cast<std::uint16_t>(std::numeric_limits<short>::max())) {
      throw core::Error{core::ErrorCode::unsupported,
                        "Effect TIM VRAM coordinate exceeds PsyCross range"};
    }
    return static_cast<short>(value);
  };
  RECT16 rect{checked(destination_x), checked(destination_y),
              checked(block.width_words), checked(block.height)};
  LoadImage(&rect, reinterpret_cast<u_long *>(
                       const_cast<std::uint16_t *>(block.words.data())));
}

void uploadTimBlock(const assets::TimBlock &block) {
  uploadTimBlockAt(block, block.x, block.y);
}

void uploadHudPixels(const assets::TimBlock &block) {
  uploadTimBlockAt(block, hudResidentX(block.x), block.y);
}

} // namespace sf::platform::detail
