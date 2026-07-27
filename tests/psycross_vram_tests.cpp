#include "psycross_vram.hpp"

#include "sf/core/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

int main() {
  using namespace sf::platform::detail;

  if (physicalTexturePage(0U) != 6U || physicalTexturePage(5U) != 11U ||
      physicalTexturePage(6U) != 6U || physicalTexturePage(16U) != 22U) {
    std::cerr << "Physical VRAM page mapping changed\n";
    return 1;
  }

  if (extended_texture_page_count != 63U ||
      resident_texture_page_count - 6U <
          maximum_scene_texture_identities) {
    std::cerr << "Resident texture pool cannot cover a complete scene\n";
    return 9;
  }

  for (const auto available_banks : std::array<std::size_t, 2U>{1U, 2U}) {
    for (const auto requested_bank : std::array{0, 1}) {
      const auto source_bank =
          canonicalMissionTextureBank(requested_bank, available_banks);
      if (source_bank < 0 ||
          static_cast<std::size_t>(source_bank) >= available_banks ||
          (available_banks == 2U && source_bank != requested_bank)) {
        std::cerr << "Mission texture selector escaped its authored banks\n";
        return 14;
      }
    }
  }

  constexpr std::array alias_pages{32U, 61U, 62U, 63U, 94U};
  constexpr std::uint16_t source_tpage = 0x03e0U;
  for (const auto physical_page : alias_pages) {
    const auto encoded =
        encodeResidentTexturePage(source_tpage, physical_page);
    const auto alias_code = static_cast<unsigned int>(encoded >> 10U);
    if (alias_code != physical_page - psx_texture_page_count + 1U ||
        (encoded & 0x03e0U) != source_tpage) {
      std::cerr << "Host TPAGE encoding changed\n";
      return 10;
    }
    const auto extension = physical_page - psx_texture_page_count;
    const auto x = (extension & 15U) * 64U;
    const auto y = (extension >> 4U) * 256U;
    if (x + 64U > 1024U || y + 256U > 1024U) {
      std::cerr << "Host TPAGE exceeds the alias atlas\n";
      return 12;
    }
  }

  for (std::uint64_t generation = 0U; generation < 3U; ++generation) {
    for (unsigned int physical_page = 0U;
         physical_page < resident_texture_page_count; ++physical_page) {
      const auto token =
          residentTexturePageToken(generation, physical_page);
      if ((token & ((1U << resident_texture_page_token_bits) - 1U)) !=
              physical_page ||
          (token >> resident_texture_page_token_bits) != generation) {
        std::cerr << "Resident texture token aliases another slot\n";
        return 13;
      }
    }
  }

  std::vector<std::byte> empty_vlf(clut_bytes);
  if (validateVlf(empty_vlf) != 0U) {
    std::cerr << "Empty VLF page mask changed\n";
    return 2;
  }

  std::vector<std::byte> one_page_vlf(texture_page_bytes + clut_bytes);
  one_page_vlf[0] = std::byte{0x01};
  if (validateVlf(one_page_vlf) != 1U ||
      vlfPage(one_page_vlf, 1U, 0U).size() != texture_page_bytes ||
      vlfClut(one_page_vlf, 1U).size() != clut_bytes) {
    std::cerr << "Single-page VLF layout changed\n";
    return 3;
  }

  auto matching_page = std::vector<std::byte>(
      vlfPage(one_page_vlf, 1U, 0U).begin(),
      vlfPage(one_page_vlf, 1U, 0U).end());
  auto different_page = matching_page;
  different_page[17] ^= std::byte{0x5a};
  if (!texturePageMatchesVlf(one_page_vlf, 1U, 0U, matching_page) ||
      texturePageMatchesVlf(one_page_vlf, 1U, 0U, different_page) ||
      texturePageMatchesVlf(one_page_vlf, 1U, 1U, matching_page)) {
    std::cerr << "VLF/bank texture ownership classification changed\n";
    return 8;
  }

  auto malformed_rejected = false;
  try {
    static_cast<void>(validateVlf(std::vector<std::byte>(4U)));
  } catch (const sf::core::Error &) {
    malformed_rejected = true;
  }
  if (!malformed_rejected) {
    std::cerr << "Malformed VLF was accepted\n";
    return 4;
  }

  std::vector<std::uint16_t> scrolling_page(64U * 256U);
  for (std::size_t index = 0U; index < scrolling_page.size(); ++index) {
    scrolling_page[index] = static_cast<std::uint16_t>(index & 0xffffU);
  }
  const auto original = scrolling_page;
  std::vector<std::byte> copy_scratch(63U * 256U * sizeof(std::uint16_t));
  auto scrolling_bytes = std::as_writable_bytes(std::span{scrolling_page});
  copyTexturePageRectangle(scrolling_bytes, 1U, 0U, scrolling_bytes, 0U, 0U,
                           63U, 256U, copy_scratch);
  for (std::size_t row = 0U; row < 256U; ++row) {
    for (std::size_t column = 0U; column < 63U; ++column) {
      if (scrolling_page[row * 64U + column] !=
          original[row * 64U + column + 1U]) {
        std::cerr << "Overlapping SCRIM page shift lost its snapshot\n";
        return 5;
      }
    }
    if (scrolling_page[row * 64U + 63U] != original[row * 64U + 63U]) {
      std::cerr << "SCRIM page shift overwrote its uncovered column\n";
      return 6;
    }
  }

  const std::array authored_page{std::byte{0x10}, std::byte{0x20},
                                 std::byte{0x30}};
  const std::array animated_page{std::byte{0x20}, std::byte{0x30},
                                 std::byte{0x10}};
  if (texturePageNeedsAuthoredReload(authored_page, authored_page, false) ||
      !texturePageNeedsAuthoredReload(animated_page, authored_page, false) ||
      texturePageNeedsAuthoredReload(animated_page, authored_page, true)) {
    std::cerr << "Mutable SCRIM residency reload policy changed\n";
    return 7;
  }

  std::cout << "PsyCross VRAM tests passed\n";
  return 0;
}
