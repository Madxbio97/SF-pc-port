#include "sf/core/error.hpp"
#include "sf/core/file_io.hpp"
#include "test_support.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <vector>

int main() {
  const sf::test::TemporaryDirectory temporary{"sf_file_io_tests"};
  const auto file = temporary.path() / "nested" / "payload.bin";
  constexpr std::array payload{
      std::byte{0x00}, std::byte{0x11}, std::byte{0x7f},
      std::byte{0x80}, std::byte{0xff},
  };

  sf::core::writeBinaryFile(file, payload, true);
  if (sf::core::readBinaryFile(file) !=
      std::vector<std::byte>{payload.begin(), payload.end()}) {
    std::cerr << "Binary file round trip changed the payload\n";
    return 1;
  }

  auto missing_file_rejected = false;
  try {
    static_cast<void>(
        sf::core::readBinaryFile(temporary.path() / "missing.bin"));
  } catch (const sf::core::Error &) {
    missing_file_rejected = true;
  }
  if (!missing_file_rejected) {
    std::cerr << "Missing binary file was not rejected\n";
    return 2;
  }

  std::cout << "file IO tests passed\n";
  return 0;
}
