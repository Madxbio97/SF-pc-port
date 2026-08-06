#pragma once

#include "sf/platform/host.hpp"
#include "sf/game/localization.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace sf::platform {

void loadLauncherSettings(
    GraphicsSettings& graphics,
    KeyboardMouseBindings& input,
    game::GameLanguage& language) noexcept;

[[nodiscard]] bool saveLauncherControllerBindings(
    const ControllerButtonBindings &bindings) noexcept;

[[nodiscard]] bool showGraphicsLauncher(
    GraphicsSettings& settings,
    KeyboardMouseBindings& input,
    game::GameLanguage& language,
    std::filesystem::path& cue_path);

[[nodiscard]] bool retailCheatMarkerExists() noexcept;

void showLauncherError(std::string_view title, std::string_view message) noexcept;

} // namespace sf::platform
