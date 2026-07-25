#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace sf::test {

class TemporaryDirectory final {
public:
  explicit TemporaryDirectory(std::string_view prefix) {
    const auto seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path();
    for (std::uint64_t attempt = 0; attempt != 100U; ++attempt) {
      path_ = root / (std::string{prefix} + "_" +
                      std::to_string(seed + attempt));
      std::error_code error;
      if (std::filesystem::create_directory(path_, error)) {
        return;
      }
      if (error && error != std::errc::file_exists) {
        throw std::runtime_error{"Cannot create temporary test directory: " +
                                 path_.string()};
      }
    }
    throw std::runtime_error{"Cannot allocate a unique temporary test "
                             "directory"};
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;
  TemporaryDirectory(TemporaryDirectory &&) = delete;
  TemporaryDirectory &operator=(TemporaryDirectory &&) = delete;

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

} // namespace sf::test
