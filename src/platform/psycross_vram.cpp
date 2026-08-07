#include "psycross_vram.hpp"

#include "sf/core/error.hpp"

#include <PsyX/PsyX_render.h>
#include <psx/libgte.h>
#include <psx/libgpu.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace sf::platform::detail {
static_assert(extended_texture_page_count == VRAM_ALIAS_PAGE_COUNT);
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

[[nodiscard]] std::vector<std::uint32_t>
packVramWords(std::span<const std::byte> bytes) {
  if ((bytes.size() & 1U) != 0U) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "VRAM payload has an odd byte count"};
  }
  // LoadImage reinterprets the buffer as a dense u16 array, so the packed
  // words must be gapless 32-bit units. Packing into u_long leaves the high
  // half of every 8-byte element zero on LP64 hosts, striping the upload.
  const auto word_count = bytes.size() / 2U;
  std::vector<std::uint32_t> result((word_count + 1U) / 2U);
  for (std::size_t index = 0; index < word_count; ++index) {
    const auto value = readLe16(bytes, index * 2U);
    result[index / 2U] |= static_cast<std::uint32_t>(value)
                          << ((index & 1U) * 16U);
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

bool texturePageMatchesVlf(std::span<const std::byte> vlf,
                           std::uint32_t page_mask, unsigned int page,
                           std::span<const std::byte> texture_bank_page) {
  if (page >= 32U || (page_mask & (1U << page)) == 0U ||
      texture_bank_page.size() != texture_page_bytes) {
    return false;
  }
  return std::ranges::equal(vlfPage(vlf, page_mask, page),
                            texture_bank_page);
}

void uploadTexturePage(unsigned int page, std::span<const std::byte> bytes) {
  if (bytes.size() != texture_page_bytes) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Mission texture page size is invalid"};
  }
  auto rect = texturePageRect(page);
  auto packed = packVramWords(bytes);
  LoadImage(&rect, reinterpret_cast<u_long *>(packed.data()));
}

void uploadTexturePageAt(unsigned int physical_page,
                         std::span<const std::byte> bytes) {
  if (physical_page >= resident_texture_page_count ||
      bytes.size() != texture_page_bytes) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Mission texture page size is invalid"};
  }
  if (physical_page >= psx_texture_page_count) {
    std::array<std::uint16_t, texture_page_bytes / sizeof(std::uint16_t)>
        words{};
    for (std::size_t index = 0U; index < words.size(); ++index) {
      words[index] = readLe16(bytes, index * sizeof(std::uint16_t));
    }
    GR_UploadVRAMAliasPage(
        static_cast<int>(physical_page - psx_texture_page_count),
        words.data());
    return;
  }
  auto rect = physicalTexturePageRect(physical_page);
  auto packed = packVramWords(bytes);
  LoadImage(&rect, reinterpret_cast<u_long *>(packed.data()));
}

void readTexturePageAt(unsigned int physical_page,
                       std::span<std::uint16_t> words) {
  if (physical_page >= resident_texture_page_count ||
      words.size() != texture_page_bytes / sizeof(std::uint16_t)) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Resident texture-page read is invalid"};
  }
  if (physical_page >= psx_texture_page_count) {
    GR_ReadVRAMAliasPage(
        static_cast<int>(physical_page - psx_texture_page_count),
        words.data());
    return;
  }
  GR_ReadVRAM(words.data(), static_cast<int>((physical_page & 15U) * 64U),
              physical_page > 15U ? 256 : 0, 64, 256);
}

void uploadTexturePageBlockAt(unsigned int physical_page,
                              const assets::TimBlock &block,
                              unsigned int local_x, unsigned int local_y) {
  constexpr auto page_width = 64U;
  constexpr auto page_height = 256U;
  if (physical_page >= resident_texture_page_count ||
      block.words.size() !=
          static_cast<std::size_t>(block.width_words) * block.height ||
      local_x + block.width_words > page_width ||
      local_y + block.height > page_height) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Texture-page block upload is invalid"};
  }
  if (physical_page < psx_texture_page_count) {
    uploadTimBlockAt(
        block,
        static_cast<std::uint16_t>((physical_page & 15U) * page_width +
                                   local_x),
        static_cast<std::uint16_t>((physical_page > 15U ? page_height : 0U) +
                                   local_y));
    return;
  }

  std::array<std::uint16_t, texture_page_bytes / sizeof(std::uint16_t)> page{};
  readTexturePageAt(physical_page, page);
  for (std::size_t row = 0U; row < block.height; ++row) {
    std::ranges::copy_n(
        block.words.begin() +
            static_cast<std::ptrdiff_t>(row * block.width_words),
        block.width_words,
        page.begin() + static_cast<std::ptrdiff_t>(
                           (local_y + row) * page_width + local_x));
  }
  GR_UploadVRAMAliasPage(
      static_cast<int>(physical_page - psx_texture_page_count), page.data());
}

void copyTexturePageRectangle(std::span<const std::byte> source,
                              unsigned int source_x, unsigned int source_y,
                              std::span<std::byte> destination,
                              unsigned int destination_x,
                              unsigned int destination_y, unsigned int width,
                              unsigned int height,
                              std::span<std::byte> scratch) {
  constexpr auto page_width = 64U;
  constexpr auto page_height = 256U;
  constexpr auto word_bytes = sizeof(std::uint16_t);
  const auto row_bytes = static_cast<std::size_t>(width) * word_bytes;
  const auto copy_bytes = row_bytes * height;
  if (source.size() != texture_page_bytes ||
      destination.size() != texture_page_bytes || width == 0U || height == 0U ||
      source_x + width > page_width || destination_x + width > page_width ||
      source_y + height > page_height || destination_y + height > page_height ||
      scratch.size() < copy_bytes) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Texture-page rectangle copy is invalid"};
  }

  // Snapshot the complete source rectangle before writing. This exactly
  // preserves PS1 MoveImage semantics for the SCRIM ring's overlapping
  // one-word horizontal shifts.
  for (std::size_t row = 0U; row < height; ++row) {
    const auto source_offset =
        ((source_y + row) * page_width + source_x) * word_bytes;
    std::memcpy(scratch.data() + row * row_bytes, source.data() + source_offset,
                row_bytes);
  }
  for (std::size_t row = 0U; row < height; ++row) {
    const auto destination_offset =
        ((destination_y + row) * page_width + destination_x) * word_bytes;
    std::memcpy(destination.data() + destination_offset,
                scratch.data() + row * row_bytes, row_bytes);
  }
}

bool texturePageNeedsAuthoredReload(std::span<const std::byte> resident,
                                    std::span<const std::byte> authored,
                                    bool runtime_mutated) noexcept {
  // A PS1 MoveImage destination is live texture state, not a corrupt cache.
  // Its bytes are expected to differ from the source archive until residency
  // is genuinely lost. Ordinary room/object-list changes must preserve it.
  return !runtime_mutated &&
         (resident.size() != authored.size() ||
          !std::equal(resident.begin(), resident.end(), authored.begin()));
}

void uploadClut(std::span<const std::byte> bytes) {
  if (bytes.size() != clut_bytes) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Mission CLUT size is invalid"};
  }
  RECT16 rect{static_cast<short>(mission_clut_resident_x),
              static_cast<short>(mission_clut_resident_y), 256, 32};
  auto packed = packVramWords(bytes);
  LoadImage(&rect, reinterpret_cast<u_long *>(packed.data()));
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
