#include "psycross_vram.hpp"

#include "sf/core/error.hpp"

#include <cstddef>
#include <iostream>
#include <vector>

int main() {
  using namespace sf::platform::detail;

  if (physicalTexturePage(0U) != 6U || physicalTexturePage(5U) != 11U ||
      physicalTexturePage(6U) != 6U || physicalTexturePage(16U) != 22U) {
    std::cerr << "Physical VRAM page mapping changed\n";
    return 1;
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

  std::cout << "PsyCross VRAM tests passed\n";
  return 0;
}
