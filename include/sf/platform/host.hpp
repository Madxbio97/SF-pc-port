#pragma once

#include "sf/platform/player_input.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace sf::game {
class MissionPackage;
class TitleAssets;
class TitleMovies;
} // namespace sf::game

namespace sf::platform {

enum class AspectRatioMode {
  original_4_3,
  adaptive,
};

struct GraphicsSettings {
  int width{1280};
  int height{720};
  int msaa_samples{4};
  bool bilinear_filtering{true};
  bool anisotropic_filtering{true};
  AspectRatioMode aspect_ratio{AspectRatioMode::adaptive};
  bool vsync{true};
  std::uint32_t frame_limit{60U};
  bool fullscreen{};
};

struct GameplayTestSettings {
  bool retail_all_weapons{};
  bool mission_selection_unlocked{};
};

class Host {
public:
  virtual ~Host() = default;
  Host(const Host &) = delete;
  Host &operator=(const Host &) = delete;

  virtual void run() = 0;

protected:
  Host() = default;
};

[[nodiscard]] std::unique_ptr<Host>
createPsyCrossHost(std::string title, GraphicsSettings graphics = {});

[[nodiscard]] std::unique_ptr<Host> createPsyCrossTitleHost(
    std::string title, game::TitleAssets assets, game::TitleMovies movies,
    game::MissionPackage initial_mission, std::filesystem::path cue_path,
    std::string supported_game_serial, GraphicsSettings graphics = {},
    KeyboardMouseBindings input = defaultKeyboardMouseBindings(),
    GameplayTestSettings tests = {});

[[nodiscard]] std::unique_ptr<Host> createPsyCrossSceneHost(
    std::string title, game::MissionPackage mission,
    std::filesystem::path cue_path, GraphicsSettings graphics = {},
    KeyboardMouseBindings input = defaultKeyboardMouseBindings(),
    GameplayTestSettings tests = {});

} // namespace sf::platform
