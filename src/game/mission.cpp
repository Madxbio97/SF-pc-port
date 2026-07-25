#include "sf/game/mission.hpp"

#include "sf/assets/hog_archive.hpp"
#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/localization.hpp"

#include <array>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace sf::game {
namespace {

constexpr std::array missions{
    MissionDefinition{0U, "Georgia Street", "SUBWAY", "SUBWAY.OVL",
                      "SOL/SUBWAY.STR", "EOL/SUBWAY.STR", 0},
    MissionDefinition{1U, "Destroyed subway", "SUBWAY2", "SUBWAY2.OVL", "",
                      "EOL/SUBWAY2.STR", 1},
    MissionDefinition{2U, "Main subway line", "SUBWAY3", "SUBWAY3.OVL", "",
                      "EOL/SUBWAY3.STR", 2},
    MissionDefinition{3U, "Washington Park", "PARK", "PARK.OVL", "SOL/PARK.STR",
                      "", 3},
    MissionDefinition{4U, "Freedom Memorial", "PARK2", "PARK2.OVL",
                      "SOL/PARK2.STR", "EOL/PARK2.STR", 4},
    MissionDefinition{5U, "Expo Center Reception", "MUSEUM", "MUSEUM.OVL",
                      "SOL/MUSEUM.STR", "EOL/MUSEUM.STR", 5},
    MissionDefinition{6U, "Expo Center Dinorama", "MUSEUM2", "MUSEUM2.OVL", "",
                      "", 6},
    MissionDefinition{7U, "Rhoemer's Base", "BASEEXT", "BASEEXT.OVL",
                      "SOL/BASEEXT.STR", "", 7, 0U},
    MissionDefinition{8U, "Base Bunker", "BUNKER", "BASEEXT.OVL", "", "", 8,
                      1U},
    MissionDefinition{9U, "Base Tower", "CHOPPER", "CHOPPER.OVL",
                      "SOL/CHOPPER.STR", "EOL/CHOPPER.STR", 9, 0U},
    MissionDefinition{10U, "Base Escape", "BASEEXT2", "BASEEXT.OVL", "",
                      "EOL/BASEEXT2.STR", 10, 2U},
    MissionDefinition{11U, "Rhoemer's Stronghold", "CHURCH", "LEVSPEC.OVL",
                      "SOL/CHURCH.STR", "EOL/CHURCH.STR", 11},
    MissionDefinition{12U, "Stronghold lower level", "CHURCH2", "LEVSPEC.OVL",
                      "", "", 12, 1U},
    MissionDefinition{13U, "Stronghold catacombs", "CATACOMB", "CATACOMB.OVL",
                      "SOL/CATACOMB.STR", "EOL/CATACOMB.STR", 13, 0U},
    MissionDefinition{14U, "PHARCOM warehouses", "WHOUSE", "WHOUSE.OVL",
                      "SOL/WHOUSE.STR", "", 14},
    MissionDefinition{15U, "PHARCOM elite guards", "WHOUSE2", "WHOUSE.OVL",
                      "SOL/WHOUSE2.STR", "EOL/WHOUSE2.STR", 15, 1U},
    MissionDefinition{16U, "Warehouse 76", "INWHOUSE", "WHOUSE.OVL", "",
                      "EOL/INWHOUSE.STR", 16, 2U},
    MissionDefinition{17U, "Silo access tunnels", "CAVE", "CAVE.OVL", "",
                      "EOL/CAVE.STR", 17, 0U},
    MissionDefinition{18U, "Tunnel blackout", "CAVE2", "WHOUSE.OVL", "",
                      "EOL/CAVE2.STR", 18, 1U, "CAVE.OVL"},
    MissionDefinition{19U, "Missile Silo", "SILO", "WHOUSE.OVL", "",
                      "EOL/SILO.STR", 19, 2U, "CAVE.OVL"},
};
constexpr std::array<std::string_view, 1U> subway_scripted_movies{
    "SOL/INTRO.STR"};
constexpr std::array<std::string_view, 1U> museum_scripted_movies{
    "CUT/MUSEUM.STR"};
constexpr std::array<std::string_view, 1U> museum2_scripted_movies{
    "CUT/MUSEUM2.STR"};
constexpr std::array<std::string_view, 1U> church2_scripted_movies{
    "CUT/CHURCH2.STR"};
constexpr std::array<std::string_view, 2U> catacomb_scripted_movies{
    "CUT/CATACOMB.STR", "CUT/CAT2.STR"};
constexpr std::array<std::string_view, 1U> warehouse_scripted_movies{
    "CUT/WHOUSE.STR"};
constexpr std::array<std::string_view, 2U> silo_scripted_movies{
    "CUT/SILO.STR", "CUT/SILO2.STR"};

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
    throw core::Error{core::ErrorCode::invalid_format, "Truncated DLF header"};
  }
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::vector<std::byte> copyBytes(std::span<const std::byte> bytes) {
  return {bytes.begin(), bytes.end()};
}

assets::HogArchive parseObjectModels(std::span<const std::byte> dlf) {
  const auto archive_offset = static_cast<std::size_t>(readLe32(dlf, 0));
  const auto archive_end = static_cast<std::size_t>(readLe32(dlf, 4));
  if (archive_offset >= archive_end || archive_end > dlf.size()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "DLF object-model archive is invalid"};
  }
  return assets::HogArchive::parse(
      copyBytes(dlf.subspan(archive_offset, archive_end - archive_offset)));
}

} // namespace

std::span<const MissionDefinition> missionCatalog() noexcept {
  return missions;
}

const MissionDefinition &missionDefinition(std::uint32_t index) {
  if (index >= missions.size()) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Mission index is outside the retail campaign"};
  }
  return missions[index];
}

std::span<const std::string_view>
missionScriptedMoviePaths(std::uint32_t index) noexcept {
  switch (index) {
  case 0U:
    return subway_scripted_movies;
  case 5U:
    return museum_scripted_movies;
  case 6U:
    return museum2_scripted_movies;
  case 12U:
    return church2_scripted_movies;
  case 13U:
    return catacomb_scripted_movies;
  case 14U:
    return warehouse_scripted_movies;
  case 19U:
    return silo_scripted_movies;
  default:
    return {};
  }
}

MissionPackage::MissionPackage(
    MissionDefinition definition, assets::MissionBriefing briefing,
    bool has_retail_briefing, assets::FogArchive archive,
    LegacyMissionImage legacy_image, DiscMovie opening_movie,
    std::vector<DiscMovie> scripted_movies, DiscMovie ending_movie,
    assets::HogArchive world_models, assets::HogArchive object_models,
    assets::HogArchive special_effects, assets::HogArchive interface_assets,
    assets::HogArchive menu_assets, assets::HogArchive character_animations,
    std::vector<assets::HogArchive> texture_banks, assets::LevelLayout layout,
    assets::MissionObjects objects, std::size_t texture_file_count)
    : definition_(definition), briefing_(std::move(briefing)),
      has_retail_briefing_(has_retail_briefing), archive_(std::move(archive)),
      legacy_image_(std::move(legacy_image)),
      opening_movie_(std::move(opening_movie)),
      scripted_movies_(std::move(scripted_movies)),
      ending_movie_(std::move(ending_movie)),
      world_models_(std::move(world_models)),
      object_models_(std::move(object_models)),
      special_effects_(std::move(special_effects)),
      interface_assets_(std::move(interface_assets)),
      menu_assets_(std::move(menu_assets)),
      character_animations_(std::move(character_animations)),
      texture_banks_(std::move(texture_banks)), layout_(std::move(layout)),
      objects_(std::move(objects)), texture_file_count_(texture_file_count) {}

const assets::HogArchive &MissionPackage::textureBank(std::size_t bank) const {
  if (bank >= texture_banks_.size()) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Invalid mission texture bank"};
  }
  return texture_banks_[bank];
}

MissionPackage MissionPackage::load(GameDisc &disc, std::uint32_t index) {
  const auto &definition = missionDefinition(index);
  const auto resource = std::string{definition.resource_name};
  const auto archive_path = "FOG/" + resource + ".FOG";
  auto archive = assets::FogArchive::parse(disc.image().readFile(archive_path));
  auto legacy_image = LegacyMissionImage::load(disc, archive, archive_path);

  constexpr std::array required_files{
      "SLF.RFF",  "VLF.RFF",  "DLF.RFF",  "WLDEMD.HOG",
      "VRAM.HOG", "INIT.OVL", "MENU.HOG",
  };
  for (const auto *name : required_files) {
    static_cast<void>(archive.file(name));
  }
  static_cast<void>(archive.file(resource + ".BIN"));
  static_cast<void>(archive.file(resource + ".DAT"));

  auto briefing =
      assets::MissionBriefing::fallback(std::string{definition.title});
  auto has_retail_briefing = false;
  try {
    const auto briefing_overlay = definition.briefing_overlay_name.empty()
                                      ? definition.overlay_name
                                      : definition.briefing_overlay_name;
    briefing = assets::MissionBriefing::parseOverlayRecord(
        disc.image().readFile("BIN/" + std::string{briefing_overlay}),
        definition.briefing_record, definition.title);
    has_retail_briefing = true;
  } catch (const core::Error &) {
    // The overlay is authoritative. Keep DLF parsing only as a fallback
    // for compatible images whose overlay has no recoverable text table.
    try {
      briefing = assets::MissionBriefing::parseRecord(
          archive.file("DLF.RFF"), definition.briefing_record,
          definition.title);
      has_retail_briefing = true;
    } catch (const core::Error &) {
      // A malformed optional briefing must not prevent mission loading.
    }
  }
  if (const auto localized = localizedMissionBriefing(index)) {
    briefing = assets::MissionBriefing::fromFields(
        localized->location, localized->mission_title, localized->date_time,
        localized->directive, localized->additional_directive);
    has_retail_briefing = true;
  }
  std::vector<assets::HogArchive> texture_banks;
  texture_banks.push_back(
      assets::HogArchive::parse(copyBytes(archive.file("VRAM.HOG"))));
  if (const auto bank = std::ranges::find_if(
          archive.entries(),
          [](const auto &entry) { return entry.name == "VRAM1.HOG"; });
      bank != archive.entries().end()) {
    texture_banks.push_back(
        assets::HogArchive::parse(copyBytes(archive.file(bank->name))));
  }
  auto world_models =
      assets::HogArchive::parse(copyBytes(archive.file("WLDEMD.HOG")));
  auto object_models = parseObjectModels(archive.file("DLF.RFF"));
  auto special_effects =
      assets::HogArchive::parse(disc.image().readFile("COMMON/SPFX.HOG"));
  auto interface_assets =
      assets::HogArchive::parse(disc.image().readFile("COMMON/INTRFACE.HOG"));
  auto menu_assets =
      assets::HogArchive::parse(copyBytes(archive.file("MENU.HOG")));
  auto character_animations =
      assets::HogArchive::parse(disc.image().readFile("COMMON/PCHAN.HOG"));
  for (unsigned int frame = 0; frame < 8U; ++frame) {
    static_cast<void>(
        special_effects.file("EXPL00" + std::to_string(frame) + ".TIM"));
  }
  constexpr std::array required_interface_assets{
      "DANGER.TIM",   "TARGET.TIM",   "ARMOR.TIM",
      "PISTOL1A.TIM", "PISTOL1B.TIM", "TASERA.TIM",
      "TASERB.TIM",   "FLASHLTA.TIM", "FLASHLTB.TIM",
  };
  for (const auto *name : required_interface_assets) {
    static_cast<void>(interface_assets.file(name));
  }
  constexpr std::array required_menu_assets{
      "GLOKSIL.TIM", "TASER.TIM", "FLASHLT.TIM",  "MAP1.TIM",
      "MAP2.TIM",    "MAP3.TIM",  "WEAPDESC.TXT",
  };
  if (index == 0U) {
    for (const auto *name : required_menu_assets) {
      static_cast<void>(menu_assets.file(name));
    }
  }
  constexpr std::array required_animations{
      "ST0.LWR", "ST02.UPR", "WK0.LWR",    "WK0.UPR",
      "RN0.LWR", "RN0.UPR",  "IDLE13.HAN",
  };
  for (const auto *name : required_animations) {
    static_cast<void>(character_animations.file(name));
  }
  auto layout = assets::LevelLayout::parse(archive.file(resource + ".DAT"),
                                           world_models.entries().size());
  auto objects = assets::MissionObjects::parse(archive.file(resource + ".BIN"));
  const auto texture_file_count =
      std::accumulate(texture_banks.begin(), texture_banks.end(), std::size_t{},
                      [](std::size_t count, const assets::HogArchive &bank) {
                        return count + bank.entries().size();
                      });
  const auto load_movie = [&disc](std::string_view path) {
    if (path.empty()) {
      return DiscMovie{};
    }
    return DiscMovie{
        std::string{path},
        disc.image().readRawSectorFile(std::string{path}),
    };
  };
  auto opening_movie = load_movie(definition.opening_movie_path);
  std::vector<DiscMovie> scripted_movies;
  for (const auto path : missionScriptedMoviePaths(index)) {
    scripted_movies.push_back(load_movie(path));
  }
  auto ending_movie = load_movie(definition.ending_movie_path);
  return MissionPackage{
      definition,
      std::move(briefing),
      has_retail_briefing,
      std::move(archive),
      std::move(legacy_image),
      std::move(opening_movie),
      std::move(scripted_movies),
      std::move(ending_movie),
      std::move(world_models),
      std::move(object_models),
      std::move(special_effects),
      std::move(interface_assets),
      std::move(menu_assets),
      std::move(character_animations),
      std::move(texture_banks),
      std::move(layout),
      std::move(objects),
      texture_file_count,
  };
}

MissionPackage MissionPackage::loadFirst(GameDisc &disc) {
  return load(disc, 0U);
}

} // namespace sf::game
