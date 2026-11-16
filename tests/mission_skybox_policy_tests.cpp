#include "sf/platform/mission_skybox_policy.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void testExactMissionSelection() {
  constexpr std::array<std::uint32_t, 10U> expected{0U,  3U,  7U,  9U,  10U,
                                                    11U, 12U, 14U, 15U, 16U};
  for (std::uint32_t mission = 0U; mission < 20U; ++mission) {
    const auto *profile = sf::platform::missionSkyboxProfile(mission);
    const auto selected =
        std::find(expected.begin(), expected.end(), mission) != expected.end();
    require((profile != nullptr) == selected,
            "Skybox policy selected the wrong campaign mission");
  }
}

void testProfilesAreSafeAndPackaged() {
  for (const auto &profile : sf::platform::mission_skybox_profiles) {
    require(!profile.asset_name.empty() &&
                profile.asset_name.ends_with(std::string_view{".bmp"}),
            "Skybox profile did not name a packaged BMP asset");
    require(profile.exposure > 0.0F && profile.exposure <= 1.0F &&
                profile.horizon_band > 0.0F && profile.horizon_band < 0.5F &&
                profile.horizon_strength >= 0.0F &&
                profile.horizon_strength <= 1.0F,
            "Skybox profile contains unsafe grading parameters");
  }
}

void testRetailScrimIsReplaced() {
  for (const auto &profile : sf::platform::mission_skybox_profiles) {
    const bool retail_scrim = profile.mission_index == 0U ||
                              profile.mission_index == 11U ||
                              profile.mission_index == 12U;
    require(profile.replace_retail_scrim == retail_scrim,
            "Skybox profile does not replace the authored retail SCRIM");
  }
}

} // namespace

int main() {
  try {
    testExactMissionSelection();
    testProfilesAreSafeAndPackaged();
    testRetailScrimIsReplaced();
  } catch (const std::exception &error) {
    std::cerr << "mission skybox policy tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "mission skybox policy tests passed\n";
  return 0;
}
