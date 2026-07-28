#include "launcher.hpp"

#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/localization.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/title.hpp"
#include "sf/platform/host.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

std::optional<int> parseInteger(std::string_view text) {
  int value{};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

bool parseResolution(std::string_view text,
                     sf::platform::GraphicsSettings &graphics) {
  const auto separator = text.find_first_of("xX");
  if (separator == std::string_view::npos) {
    return false;
  }
  const auto width = parseInteger(text.substr(0, separator));
  const auto height = parseInteger(text.substr(separator + 1));
  if (!width || !height || *width < 320 || *height < 240) {
    return false;
  }
  graphics.width = *width;
  graphics.height = *height;
  return true;
}

enum class LaunchMode {
  game,
  title_test,
  scene_test,
  platform_test,
};

struct LaunchRequest {
  LaunchMode mode{LaunchMode::game};
  std::filesystem::path cue_path;
};

std::optional<LaunchRequest>
parseLaunchRequest(const std::vector<std::string_view> &arguments) {
  if (arguments.empty()) {
    return LaunchRequest{};
  }
  if (arguments.size() == 1U && !arguments.front().starts_with("--")) {
    return LaunchRequest{LaunchMode::game,
                         std::filesystem::path{arguments.front()}};
  }
  if (arguments.size() != 2U || arguments[1].starts_with("--")) {
    return std::nullopt;
  }

  if (arguments[0] == "--game") {
    return LaunchRequest{LaunchMode::game, std::filesystem::path{arguments[1]}};
  }
  if (arguments[0] == "--title-test") {
    return LaunchRequest{LaunchMode::title_test,
                         std::filesystem::path{arguments[1]}};
  }
  if (arguments[0] == "--scene-test") {
    return LaunchRequest{LaunchMode::scene_test,
                         std::filesystem::path{arguments[1]}};
  }
  if (arguments[0] == "--platform-test") {
    return LaunchRequest{LaunchMode::platform_test,
                         std::filesystem::path{arguments[1]}};
  }
  return std::nullopt;
}

bool supportsMissionSelection(LaunchMode mode) noexcept {
  return mode == LaunchMode::game || mode == LaunchMode::title_test ||
         mode == LaunchMode::scene_test;
}

void printUsage() {
  std::cerr
      << "Usage:\n"
      << "  syphon_filter\n"
      << "  syphon_filter [graphics options] --game <game.cue>\n"
      << "  syphon_filter [graphics options] <game.cue>\n"
      << "Development modes:\n"
      << "  syphon_filter [graphics options] [--mission=1..20] --title-test "
         "<game.cue>\n"
      << "  syphon_filter [graphics options] [--mission=1..20] --scene-test "
         "<game.cue>\n"
      << "  syphon_filter [graphics options] --platform-test <game.cue>\n"
      << "Mission aliases: --mission=N --level=N (retail mission number "
         "1..20)\n"
      << "Gameplay test option: --all-weapons-test\n"
      << "Graphics options: --fullscreen --no-launcher "
         "--resolution=WIDTHxHEIGHT "
         "--msaa=0|2|4|8 --bilinear --nearest --anisotropic "
         "--no-anisotropic --ssao --no-ssao --aspect-adaptive --aspect-4-3 "
         "--vsync --no-vsync --fps-limit=0|20..1000\n";
  std::cerr << "Language options: --language=en --language=ru\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    sf::platform::GraphicsSettings graphics;
    sf::game::RetailCheatState retail_cheats;
    auto input = sf::platform::defaultKeyboardMouseBindings();
    auto language = sf::game::GameLanguage::english;
    std::error_code executable_path_error;
    const auto executable_path = std::filesystem::absolute(
        std::filesystem::path{argv[0]}, executable_path_error);
    const auto executable_directory = executable_path_error
                                          ? std::filesystem::current_path()
                                          : executable_path.parent_path();
    sf::game::setLocalizationRoot(executable_directory / "locales" / "ru-vit");
    sf::platform::loadLauncherSettings(graphics, input, language);
    bool show_launcher = true;
    std::optional<std::uint32_t> requested_mission;
    std::vector<std::string_view> arguments;
    if (argc > 1) {
      arguments.reserve(static_cast<std::size_t>(argc - 1));
    }
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument{argv[index]};
      if (argument == "--fullscreen") {
        graphics.fullscreen = true;
      } else if (argument == "--all-weapons-test") {
        retail_cheats.all_weapons = true;
      } else if (argument == "--no-launcher") {
        show_launcher = false;
      } else if (argument == "--bilinear") {
        graphics.bilinear_filtering = true;
      } else if (argument == "--nearest") {
        graphics.bilinear_filtering = false;
      } else if (argument == "--anisotropic") {
        graphics.anisotropic_filtering = true;
      } else if (argument == "--no-anisotropic") {
        graphics.anisotropic_filtering = false;
      } else if (argument == "--ssao") {
        graphics.ambient_occlusion = true;
      } else if (argument == "--no-ssao") {
        graphics.ambient_occlusion = false;
      } else if (argument == "--vsync") {
        graphics.vsync = true;
      } else if (argument == "--no-vsync") {
        graphics.vsync = false;
      } else if (argument.starts_with("--fps-limit=")) {
        const auto limit = parseInteger(
            argument.substr(std::string_view{"--fps-limit="}.size()));
        if (!limit || (*limit != 0 && (*limit < 20 || *limit > 1000))) {
          printUsage();
          return 64;
        }
        graphics.frame_limit = static_cast<std::uint32_t>(*limit);
      } else if (argument == "--language=en" || argument == "--locale=en") {
        language = sf::game::GameLanguage::english;
      } else if (argument == "--language=ru" || argument == "--locale=ru") {
        language = sf::game::GameLanguage::russian_vit;
      } else if (argument == "--widescreen" || argument == "--aspect-auto" ||
                 argument == "--aspect-adaptive") {
        graphics.aspect_ratio = sf::platform::AspectRatioMode::adaptive;
      } else if (argument == "--aspect-4-3") {
        graphics.aspect_ratio = sf::platform::AspectRatioMode::original_4_3;
      } else if (argument.starts_with("--resolution=")) {
        if (!parseResolution(
                argument.substr(std::string_view{"--resolution="}.size()),
                graphics)) {
          printUsage();
          return 64;
        }
      } else if (argument.starts_with("--msaa=")) {
        const auto samples =
            parseInteger(argument.substr(std::string_view{"--msaa="}.size()));
        if (!samples || (*samples != 0 && *samples != 2 && *samples != 4 &&
                         *samples != 8)) {
          printUsage();
          return 64;
        }
        graphics.msaa_samples = *samples;
      } else if (argument.starts_with("--mission=") ||
                 argument.starts_with("--level=")) {
        const auto separator = argument.find('=');
        const auto mission_number =
            parseInteger(argument.substr(separator + 1U));
        const auto mission_count = sf::game::missionCatalog().size();
        if (!mission_number || *mission_number < 1 ||
            static_cast<std::size_t>(*mission_number) > mission_count) {
          std::cerr << "Mission must be a retail number from 1 to "
                    << mission_count << ".\n";
          printUsage();
          return 64;
        }
        const auto mission_index =
            static_cast<std::uint32_t>(*mission_number - 1);
        if (requested_mission && *requested_mission != mission_index) {
          std::cerr << "Conflicting --mission/--level selections.\n";
          printUsage();
          return 64;
        }
        requested_mission = mission_index;
      } else if (argument == "--mission" || argument == "--level") {
        std::cerr << argument << " requires =N.\n";
        printUsage();
        return 64;
      } else {
        arguments.emplace_back(argument);
      }
    }

    const auto launch = parseLaunchRequest(arguments);
    if (!launch) {
      printUsage();
      return 64;
    }

    const auto supports_mission_selection =
        supportsMissionSelection(launch->mode);
    const auto cheats_enabled = sf::platform::retailCheatMarkerExists();
    if (cheats_enabled && supports_mission_selection) {
      retail_cheats.enableAll();
    }
    if (requested_mission && !supports_mission_selection) {
      std::cerr << "Mission selection requires --game, --title-test or "
                   "--scene-test.\n";
      printUsage();
      return 64;
    }
    if (retail_cheats.all_weapons && !supports_mission_selection) {
      std::cerr << "--all-weapons-test requires --game, --title-test or "
                   "--scene-test.\n";
      printUsage();
      return 64;
    }
    if ((requested_mission || retail_cheats.all_weapons) && !cheats_enabled) {
      const auto message =
          "Mission and inventory overrides require an empty "
          "syphon_filter_cheats file beside syphon_filter.exe.";
      std::cerr << message << '\n';
      sf::platform::showLauncherError("RESTRICTED ACCESS", message);
      return 64;
    }
    auto mission_index = requested_mission.value_or(0U);
    auto cue_path = launch->cue_path;
    if (show_launcher && !sf::platform::showGraphicsLauncher(
                             graphics, input, language, cue_path)) {
      return 0;
    }
    if (!sf::game::localizationPackAvailable(language)) {
      const auto message =
          "The selected Russian text pack is missing or incomplete.";
      std::cerr << message << '\n';
      sf::platform::showLauncherError("LANGUAGE PACK MISSING", message);
      return 64;
    }
    sf::game::setGameLanguage(language);
    if (cue_path.empty()) {
      const auto message =
          "No game image was selected. Choose the CUE file from the original "
          "Syphon Filter USA v1.1 disc image.";
      std::cerr << message << '\n';
      sf::platform::showLauncherError("DISC IMAGE REQUIRED", message);
      return 64;
    }

    auto disc = sf::game::GameDisc::open(cue_path);
    if (!disc.game()) {
      std::cerr << "Unsupported disc build\n";
      sf::platform::showLauncherError(
          "UNSUPPORTED DISC BUILD",
          "Use Syphon Filter USA v1.1 (SCUS-94240) in BIN/CUE format.");
      return 2;
    }

    std::unique_ptr<sf::platform::Host> host;
    if (launch->mode == LaunchMode::game ||
        launch->mode == LaunchMode::title_test) {
      const auto &definition = sf::game::missionDefinition(mission_index);
      const auto development_alias = launch->mode == LaunchMode::title_test;
      std::cout << "Disc verified. Starting "
                << (development_alias ? "native title test" : "campaign")
                << " at mission " << (mission_index + 1U) << ": "
                << definition.title << " [" << definition.resource_name
                << "].\n";
      auto assets = sf::game::TitleAssets::load(disc);
      auto movies = sf::game::TitleMovies::load(disc);
      auto selected_mission =
          sf::game::MissionPackage::load(disc, mission_index);
      auto mission_cue_path = disc.cuePath();
      auto supported_game_serial = std::string{disc.game()->serial};
      host = sf::platform::createPsyCrossTitleHost(
          development_alias ? "Syphon Filter PC - title test"
                            : "Syphon Filter PC",
          std::move(assets), std::move(movies), std::move(selected_mission),
          std::move(mission_cue_path), std::move(supported_game_serial),
          graphics, input, retail_cheats);
    } else if (launch->mode == LaunchMode::scene_test) {
      const auto &definition = sf::game::missionDefinition(mission_index);
      std::cout << "Disc verified. Starting native scene test at mission "
                << (mission_index + 1U) << ": " << definition.title << " ["
                << definition.resource_name << "].\n";
      host = sf::platform::createPsyCrossSceneHost(
          "Syphon Filter PC - scene test",
          sf::game::MissionPackage::load(disc, mission_index), disc.cuePath(),
          graphics, input, retail_cheats);
    } else {
      std::cout << "Disc verified. Starting PsyCross platform test; "
                   "close the window to exit.\n";
      host = sf::platform::createPsyCrossHost(
          "Syphon Filter PC - platform test", graphics);
    }
    host->run();
    return 0;
  } catch (const sf::core::Error &error) {
    std::cerr << "syphon_filter: " << error.what() << '\n';
    sf::platform::showLauncherError("STARTUP FAILED", error.what());
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "syphon_filter: unexpected error: " << error.what() << '\n';
    sf::platform::showLauncherError("UNEXPECTED ERROR", error.what());
    return 1;
  }
}
