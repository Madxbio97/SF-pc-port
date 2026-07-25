#include "ui_export.hpp"

#include "sf/assets/hog_archive.hpp"
#include "sf/assets/tim_image.hpp"
#include "sf/core/file_io.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/mission.hpp"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace sf::tool {
namespace {

std::size_t exportTimArchive(const assets::HogArchive &archive,
                             const std::filesystem::path &directory) {
  std::size_t exported{};
  for (const auto &entry : archive.entries()) {
    const auto name = std::filesystem::path{entry.name}.filename();
    if (name.string() != entry.name ||
        !std::string_view{entry.name}.ends_with(".TIM")) {
      continue;
    }
    const auto bytes = archive.file(entry.name);
    static_cast<void>(assets::TimImage::parse(bytes));
    core::writeBinaryFile(directory / name, bytes, true);
    ++exported;
  }
  return exported;
}

} // namespace

int exportUiAssets(const char *cue_path, const char *output_path) {
  auto disc = game::GameDisc::open(std::filesystem::path{cue_path});
  const auto output = std::filesystem::path{output_path};
  const auto first = game::MissionPackage::loadFirst(disc);
  auto total =
      exportTimArchive(first.interfaceAssets(), output / "common" / "intrface");
  for (const auto &definition : game::missionCatalog()) {
    const auto mission = game::MissionPackage::load(disc, definition.index);
    total += exportTimArchive(mission.menuAssets(),
                              output / "menu" / definition.resource_name);
  }
  std::cout << "Exported " << total << " original UI TIM assets to "
            << output.string() << '\n';
  return 0;
}

} // namespace sf::tool
