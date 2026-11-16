#include "settings.hpp"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class EnvironmentGuard {
public:
  explicit EnvironmentGuard(const wchar_t *name) : name_(name) {
    const auto required = GetEnvironmentVariableW(name_, nullptr, 0U);
    if (required != 0U) {
      value_.resize(required);
      const auto length = GetEnvironmentVariableW(name_, value_.data(), required);
      value_.resize(length);
      existed_ = true;
    }
  }

  ~EnvironmentGuard() {
    SetEnvironmentVariableW(name_, existed_ ? value_.c_str() : nullptr);
  }

private:
  const wchar_t *name_{};
  std::wstring value_;
  bool existed_{};
};

} // namespace

int main() {
  namespace settings = sf::platform::launcher_settings;
  try {
    EnvironmentGuard environment{L"LOCALAPPDATA"};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      (L"sf-launcher-settings-" + std::to_wstring(nonce));
    require(SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()) != FALSE,
            "Could not redirect LOCALAPPDATA");

    sf::platform::GraphicsSettings expected{};
    expected.width = 2560;
    expected.height = 1440;
    expected.msaa_samples = 4;
    expected.smaa = false;
    expected.mission_skyboxes = true;
    expected.fullscreen = true;
    expected.frame_limit = 240U;
    expected.controller_protocol = sf::platform::ControllerProtocol::raw_input;
    expected.controller_bindings.stick_layout =
        sf::game::ControllerStickLayout::original_one_stick;
    expected.controller_vibration = false;
    const auto input = sf::platform::defaultKeyboardMouseBindings();
    const auto cue = root / L"ROM" / L"Syphon Filter (USA).cue";

    require(settings::saveSettingsFile(
                expected, input, sf::game::GameLanguage::russian_vit, cue),
            "First launcher settings save failed");
    require(settings::saveSettingsFile(
                expected, input, sf::game::GameLanguage::russian_vit, cue),
            "Saving over an existing launcher.ini failed");

    sf::platform::GraphicsSettings loaded{};
    auto loaded_input = sf::platform::defaultKeyboardMouseBindings();
    auto language = sf::game::GameLanguage::english;
    settings::loadSettingsFile(loaded, loaded_input, language);
    require(loaded.width == expected.width && loaded.height == expected.height,
            "Resolution did not round-trip");
    require(loaded.controller_protocol == expected.controller_protocol &&
                loaded.controller_bindings == expected.controller_bindings &&
                !loaded.controller_vibration,
            "Controller settings did not round-trip");
    require(loaded.mission_skyboxes == expected.mission_skyboxes,
            "Mission skybox toggle did not round-trip");
    require(loaded_input == input, "Input bindings did not round-trip");
    require(language == sf::game::GameLanguage::russian_vit,
            "Language did not round-trip");
    require(settings::loadGameImagePath() == cue,
            "Unicode CUE path did not round-trip");

    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(!error, "Could not remove the test directory");
    std::cout << "launcher settings tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "launcher settings tests failed: " << error.what() << '\n';
    return 1;
  }
}
