#pragma once

#include "sf/game/controller_bindings.hpp"
#include "sf/game/retail_cheats.hpp"
#include "sf/platform/player_input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
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

enum class ControllerProtocol {
  automatic,
  xinput,
  direct_input,
  raw_input,
};

using ControllerButtonBindings = game::ControllerButtonBindings;
inline constexpr auto controller_action_binding_count =
    game::controller_action_count;
using ControllerSettingsCommitCallback =
    std::function<bool(const ControllerButtonBindings &, bool vibration)>;

struct GraphicsSettings {
  int width{1280};
  int height{720};
  int msaa_samples{};
  bool bilinear_filtering{true};
  bool trilinear_filtering{true};
  bool anisotropic_filtering{true};
  bool smaa{true};
  bool volumetric_fog{};
  AspectRatioMode aspect_ratio{AspectRatioMode::adaptive};
  bool vsync{true};
  std::uint32_t frame_limit{60U};
  bool fullscreen{};
  ControllerProtocol controller_protocol{ControllerProtocol::automatic};
  ControllerButtonBindings controller_bindings;
  bool controller_vibration{true};
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
    game::RetailCheatState cheats = {},
    ControllerSettingsCommitCallback controller_settings_commit = {});

[[nodiscard]] std::unique_ptr<Host> createPsyCrossSceneHost(
    std::string title, game::MissionPackage mission,
    std::filesystem::path cue_path, GraphicsSettings graphics = {},
    KeyboardMouseBindings input = defaultKeyboardMouseBindings(),
    game::RetailCheatState cheats = {},
    ControllerSettingsCommitCallback controller_settings_commit = {});

} // namespace sf::platform
