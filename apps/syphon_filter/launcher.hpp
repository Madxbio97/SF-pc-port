#pragma once

#include "sf/platform/host.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace sf::platform {

void loadLauncherSettings(
    GraphicsSettings& graphics,
    KeyboardMouseBindings& input) noexcept;

[[nodiscard]] bool showGraphicsLauncher(
    GraphicsSettings& settings,
    KeyboardMouseBindings& input,
    GameplayTestSettings& tests,
    std::filesystem::path& cue_path,
    std::uint32_t& mission_index,
    bool mission_selection_enabled);

[[nodiscard]] bool launcherCheatsEnabled() noexcept;

void showLauncherError(std::string_view title, std::string_view message) noexcept;

} // namespace sf::platform
