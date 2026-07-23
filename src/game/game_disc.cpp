#include "sf/game/game_disc.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace sf::game {
namespace {

std::string parseBootPath(std::span<const std::byte> system_configuration) {
  std::string text;
  text.reserve(system_configuration.size());
  for (const auto value : system_configuration) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
  }

  const auto boot = text.find("BOOT");
  const auto separator =
      boot == std::string::npos ? std::string::npos : text.find('=', boot + 4);
  const auto cdrom = separator == std::string::npos
                         ? std::string::npos
                         : text.find(':', separator + 1);
  if (cdrom == std::string::npos) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "SYSTEM.CNF has no BOOT path"};
  }

  auto end = text.find_first_of("\r\n", cdrom + 1);
  if (end == std::string::npos) {
    end = text.size();
  }
  auto path = text.substr(cdrom + 1, end - cdrom - 1);
  while (!path.empty() &&
         std::isspace(static_cast<unsigned char>(path.front())) != 0) {
    path.erase(path.begin());
  }
  while (!path.empty() &&
         std::isspace(static_cast<unsigned char>(path.back())) != 0) {
    path.pop_back();
  }
  while (!path.empty() && (path.front() == '\\' || path.front() == '/')) {
    path.erase(path.begin());
  }
  if (const auto version = path.find(';'); version != std::string::npos) {
    path.resize(version);
  }
  std::ranges::transform(path, path.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  if (path.empty()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "SYSTEM.CNF BOOT path is empty"};
  }
  return path;
}

} // namespace

GameDisc::GameDisc(std::filesystem::path cue_path, disc::Iso9660Image image,
                   std::string boot_path,
                   std::vector<std::byte> executable_file,
                   psx::Executable executable,
                   core::Sha256Digest executable_hash,
                   std::optional<SupportedGame> game)
    : cue_path_(std::move(cue_path)), image_(std::move(image)),
      boot_path_(std::move(boot_path)),
      executable_file_(std::move(executable_file)),
      executable_(std::move(executable)), executable_hash_(executable_hash),
      game_(std::move(game)) {}

GameDisc GameDisc::open(const std::filesystem::path &cue_path) {
  auto image = disc::Iso9660Image::open(cue_path);
  const auto system_configuration = image.readFile("SYSTEM.CNF");
  auto boot_path = parseBootPath(system_configuration);
  auto executable_file = image.readFile(boot_path);
  auto executable_hash = core::sha256(executable_file);
  auto executable = psx::Executable::parse(executable_file);
  auto game = identify(image.volumeId(), executable_hash);
  return GameDisc{
      cue_path,
      std::move(image),
      std::move(boot_path),
      std::move(executable_file),
      std::move(executable),
      executable_hash,
      std::move(game),
  };
}

std::vector<OverlayInfo> GameDisc::overlays() {
  std::vector<OverlayInfo> result;
  for (const auto &entry : image_.list("BIN")) {
    constexpr std::string_view extension = ".OVL";
    if (entry.is_directory || entry.name.size() < extension.size() ||
        entry.name.compare(entry.name.size() - extension.size(),
                           extension.size(), extension) != 0) {
      continue;
    }
    const auto path = "BIN/" + entry.name;
    const auto bytes = image_.readFile(path);
    result.push_back(OverlayInfo{path, entry.size, core::sha256(bytes)});
  }
  return result;
}

} // namespace sf::game
