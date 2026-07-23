#include "sf/assets/fog_archive.hpp"
#include "sf/assets/hmd_model.hpp"
#include "sf/assets/mission_briefing.hpp"
#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

bool hasExtension(std::string_view name, std::string_view extension) {
  return name.size() >= extension.size() &&
         name.substr(name.size() - extension.size()) == extension;
}

bool isGameplayOverlay(std::string_view name) {
  static constexpr std::string_view infrastructure_overlays[] = {
      "INIT.OVL", "MENU.OVL", "MOVIE.OVL", "TCACHE.OVL", "TITLE.OVL",
  };
  return hasExtension(name, ".OVL") &&
         std::ranges::find(infrastructure_overlays, name) ==
             std::ranges::end(infrastructure_overlays);
}

std::string modelStem(std::string_view name) {
  const auto extension = name.find_last_of('.');
  std::string result{name.substr(0, extension)};
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  return result;
}

std::set<std::string> discFiles(sf::game::GameDisc &disc,
                                std::string_view directory) {
  std::set<std::string> result;
  for (const auto &entry : disc.image().list(std::string{directory})) {
    if (!entry.is_directory) {
      result.insert(entry.name);
    }
  }
  return result;
}

std::string moviePath(const std::set<std::string> &files,
                      std::string_view directory, std::string_view resource) {
  const auto name = std::string{resource} + ".STR";
  return files.contains(name) ? std::string{directory} + '/' + name
                              : std::string{};
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "H5 catalog probe requires Syphon Filter USA v1.1"};
  }

  auto fog_files = disc.image().list("FOG");
  std::erase_if(fog_files, [](const auto &entry) {
    return entry.is_directory || !hasExtension(entry.name, ".FOG");
  });
  const auto sol_files = discFiles(disc, "SOL");
  const auto eol_files = discFiles(disc, "EOL");
  const auto bin_files = discFiles(disc, "BIN");
  const auto catalog = sf::game::missionCatalog();
  std::set<std::string> overlays;
  std::set<std::string> catalog_fog_files;
  std::size_t primary_definitions{};
  std::size_t secondary_definitions{};
  std::size_t hmd_definitions{};
  std::size_t hmd_instances{};
  std::size_t tmd_hmd_aliases{};

  std::cout << "resource,overlay,title,location,opening,ending\n";
  for (const auto &mission : catalog) {
    const auto fog_name = std::string{mission.resource_name} + ".FOG";
    const auto archive_path = "FOG/" + fog_name;
    catalog_fog_files.insert(fog_name);
    const auto archive =
        sf::assets::FogArchive::parse(disc.image().readFile(archive_path));
    const auto package = sf::game::MissionPackage::load(disc, mission.index);

    struct ObjectResources {
      const sf::assets::HogEntry *gmd{};
      const sf::assets::HogEntry *emd{};
      const sf::assets::HogEntry *hmd{};
    };
    std::unordered_map<std::string, ObjectResources> object_resources;
    for (const auto &entry : package.objectModels().entries()) {
      if (entry.name.size() <= 4U) {
        continue;
      }
      const auto extension = entry.name.substr(entry.name.size() - 4U);
      auto &resources = object_resources[modelStem(entry.name)];
      if (extension == ".GMD") {
        resources.gmd = &entry;
      } else if (extension == ".EMD") {
        resources.emd = &entry;
      } else if (extension == ".HMD") {
        resources.hmd = &entry;
      }
    }
    const auto resolve_model = [&](std::string_view name) {
      const auto resources = object_resources.find(modelStem(name));
      if (resources == object_resources.end()) {
        return std::pair{sf::game::LegacyPresentationResourceKind::none,
                         static_cast<const sf::assets::HogEntry *>(nullptr)};
      }
      const auto kind = sf::game::legacyPresentationResourceKind(
          name, resources->second.gmd != nullptr,
          resources->second.emd != nullptr, resources->second.hmd != nullptr);
      const auto *entry = [&]() -> const sf::assets::HogEntry * {
        switch (kind) {
        case sf::game::LegacyPresentationResourceKind::gmd:
          return resources->second.gmd;
        case sf::game::LegacyPresentationResourceKind::emd:
          return resources->second.emd;
        case sf::game::LegacyPresentationResourceKind::hmd:
          return resources->second.hmd;
        case sf::game::LegacyPresentationResourceKind::none:
          return nullptr;
        }
        return nullptr;
      }();
      return std::pair{kind, entry};
    };
    std::vector<sf::game::LegacyPresentationResourceKind> primary_kinds;
    primary_kinds.reserve(package.objects().definitions().size());
    for (const auto &definition : package.objects().definitions()) {
      auto primary_kind = sf::game::LegacyPresentationResourceKind::none;
      if (!definition.primary_model.empty()) {
        ++primary_definitions;
        const auto [kind, resource] = resolve_model(definition.primary_model);
        if (resource == nullptr) {
          throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                                archive_path +
                                    " cannot resolve exact primary model " +
                                    definition.primary_model};
        }
        primary_kind = kind;
        if (kind == sf::game::LegacyPresentationResourceKind::hmd) {
          ++hmd_definitions;
          if (definition.primary_model.ends_with(".TMD")) {
            ++tmd_hmd_aliases;
          }
          const auto model = sf::assets::HmdModel::parse(
              package.objectModels().file(resource->name));
          if (model.parts().size() > sf::game::legacy_actor_bone_count) {
            throw sf::core::Error{
                sf::core::ErrorCode::invalid_format,
                archive_path +
                    " HMD exceeds the retail bone bridge: " + resource->name};
          }
        }
      }
      primary_kinds.push_back(primary_kind);
      if (!definition.secondary_model.empty()) {
        ++secondary_definitions;
        const auto [kind, resource] = resolve_model(definition.secondary_model);
        static_cast<void>(kind);
        if (resource == nullptr) {
          throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                                archive_path +
                                    " cannot resolve exact secondary model " +
                                    definition.secondary_model};
        }
      }
    }
    for (const auto &object : package.objects().objects()) {
      if (object.type >= primary_kinds.size()) {
        throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                              archive_path + " has invalid object type"};
      }
      hmd_instances += primary_kinds[object.type] ==
                               sf::game::LegacyPresentationResourceKind::hmd
                           ? 1U
                           : 0U;
    }

    auto mapped_overlay_embedded = false;
    for (const auto &entry : archive.entries()) {
      if (!isGameplayOverlay(entry.name)) {
        continue;
      }
      if (!bin_files.contains(entry.name)) {
        throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                              archive_path + " references missing BIN/" +
                                  entry.name};
      }
      const auto embedded_overlay = archive.file(entry.name);
      const auto root_overlay = disc.image().readFile("BIN/" + entry.name);
      if (embedded_overlay.size() < root_overlay.size() ||
          !std::equal(root_overlay.begin(), root_overlay.end(),
                      embedded_overlay.begin())) {
        throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                              archive_path + " overlay differs from BIN/" +
                                  entry.name};
      }
      mapped_overlay_embedded =
          mapped_overlay_embedded || entry.name == mission.overlay_name;
    }

    if (!bin_files.contains(std::string{mission.overlay_name})) {
      throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                            archive_path + " maps to missing BIN/" +
                                std::string{mission.overlay_name}};
    }
    const auto exact_overlay = std::string{mission.resource_name} + ".OVL";
    if (bin_files.contains(exact_overlay)) {
      if (mission.overlay_name != exact_overlay) {
        throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                              archive_path + " ignores its exact overlay"};
      }
    } else if (!mapped_overlay_embedded) {
      throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                            archive_path +
                                " shared overlay mapping is not embedded"};
    }

    auto briefing_title = std::string{};
    auto location = std::string{};
    try {
      const auto briefing = sf::assets::MissionBriefing::parseRecord(
          archive.file("DLF.RFF"), mission.briefing_record, mission.title);
      briefing_title = briefing.missionTitle();
      location = briefing.location();
    } catch (const sf::core::Error &error) {
      // Continuation levels carry gameplay DLF data but no briefing block.
      std::cerr << mission.resource_name << ": " << error.what() << '\n';
    }
    if (briefing_title != mission.title || location.empty()) {
      throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                            std::string{mission.resource_name} +
                                " briefing/catalog mismatch title='" +
                                briefing_title + "' location='" + location +
                                "'"};
    }
    const auto opening = moviePath(sol_files, "SOL", mission.resource_name);
    const auto ending = moviePath(eol_files, "EOL", mission.resource_name);
    if (opening != mission.opening_movie_path ||
        ending != mission.ending_movie_path) {
      throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                            archive_path + " movie mapping mismatch"};
    }
    overlays.insert(std::string{mission.overlay_name});

    std::cout << std::quoted(std::string{mission.resource_name}) << ','
              << std::quoted(std::string{mission.overlay_name}) << ','
              << std::quoted(std::string{mission.title}) << ','
              << std::quoted(location) << ',' << std::quoted(opening) << ','
              << std::quoted(ending) << '\n';
  }

  const auto all_fog_files =
      std::ranges::all_of(fog_files, [&](const auto &entry) {
        return catalog_fog_files.contains(entry.name);
      });
  constexpr std::size_t expected_primary_definitions = 416U;
  constexpr std::size_t expected_hmd_definitions = 46U;
  constexpr std::size_t expected_hmd_instances = 670U;
  constexpr std::size_t expected_tmd_hmd_aliases = 4U;
  if (fog_files.size() != catalog.size() || !all_fog_files ||
      overlays.size() != 13U ||
      primary_definitions != expected_primary_definitions ||
      hmd_definitions != expected_hmd_definitions ||
      hmd_instances != expected_hmd_instances ||
      tmd_hmd_aliases != expected_tmd_hmd_aliases) {
    throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                          "H5 catalog gate does not cover the retail campaign"};
  }
  std::cout << "H5 catalog gate passed: missions=" << catalog.size()
            << ", overlays=" << overlays.size()
            << ", primary-models=" << primary_definitions
            << ", secondary-models=" << secondary_definitions
            << ", hmd-definitions=" << hmd_definitions
            << ", hmd-instances=" << hmd_instances
            << ", tmd-hmd-aliases=" << tmd_hmd_aliases << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: sf_h5_catalog_probe <game.cue>\n";
    return 1;
  }
  try {
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "H5 catalog gate failed: " << error.what() << '\n';
    return 10;
  }
}
