#include "sf/core/file_io.hpp"

#include "sf/core/error.hpp"

#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

namespace sf::core {

std::vector<std::byte> readBinaryFile(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) {
    throw Error{ErrorCode::io, "Cannot open binary file: " + path.string()};
  }
  const auto end = input.tellg();
  if (end < 0 ||
      static_cast<std::uint64_t>(end) >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw Error{ErrorCode::io, "Binary file is too large: " + path.string()};
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input) {
    throw Error{ErrorCode::io, "Failed to read binary file: " + path.string()};
  }
  return bytes;
}

void writeBinaryFile(const std::filesystem::path &path,
                     std::span<const std::byte> bytes,
                     bool create_parent_directories) {
  if (create_parent_directories && !path.parent_path().empty()) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      throw Error{ErrorCode::io,
                  "Cannot create binary file directory: " +
                      path.parent_path().string()};
    }
  }
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) {
    throw Error{ErrorCode::io, "Cannot create binary file: " + path.string()};
  }
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw Error{ErrorCode::io, "Failed to write binary file: " + path.string()};
  }
}

} // namespace sf::core
