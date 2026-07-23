#pragma once

#include "sf/core/sha256.hpp"
#include "sf/disc/iso9660.hpp"
#include "sf/game/supported_games.hpp"
#include "sf/psx/executable.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sf::game {

struct OverlayInfo {
  std::string path;
  std::uint32_t size{};
  core::Sha256Digest sha256{};
};

class GameDisc final {
public:
  [[nodiscard]] static GameDisc open(const std::filesystem::path &cue_path);

  [[nodiscard]] const std::filesystem::path &cuePath() const noexcept {
    return cue_path_;
  }
  [[nodiscard]] const disc::Iso9660Image &image() const noexcept {
    return image_;
  }
  [[nodiscard]] disc::Iso9660Image &image() noexcept { return image_; }
  [[nodiscard]] const std::string &bootPath() const noexcept {
    return boot_path_;
  }
  [[nodiscard]] const std::vector<std::byte> &executableFile() const noexcept {
    return executable_file_;
  }
  [[nodiscard]] const psx::Executable &executable() const noexcept {
    return executable_;
  }
  [[nodiscard]] const core::Sha256Digest &executableHash() const noexcept {
    return executable_hash_;
  }
  [[nodiscard]] const std::optional<SupportedGame> &game() const noexcept {
    return game_;
  }
  [[nodiscard]] std::vector<OverlayInfo> overlays();

private:
  GameDisc(std::filesystem::path cue_path, disc::Iso9660Image image,
           std::string boot_path, std::vector<std::byte> executable_file,
           psx::Executable executable, core::Sha256Digest executable_hash,
           std::optional<SupportedGame> game);

  std::filesystem::path cue_path_;
  disc::Iso9660Image image_;
  std::string boot_path_;
  std::vector<std::byte> executable_file_;
  psx::Executable executable_;
  core::Sha256Digest executable_hash_;
  std::optional<SupportedGame> game_;
};

} // namespace sf::game
