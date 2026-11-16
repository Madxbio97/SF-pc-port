#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace sf::platform {

enum class MissionSkyboxTexture : std::uint8_t {
  dc_night,
  park_storm,
  kazakhstan_night,
  stronghold_dawn,
  almaty_industrial,
};

struct MissionSkyboxProfile {
  std::uint32_t mission_index{};
  MissionSkyboxTexture texture{};
  std::string_view asset_name;
  float yaw_offset_turns{};
  std::array<float, 3U> tint{1.0F, 1.0F, 1.0F};
  float exposure{1.0F};
  float horizon_band{0.16F};
  float horizon_strength{0.78F};
  bool replace_retail_scrim{};
};

// Mission indices are zero-based. Connected missions deliberately share one
// panorama while yaw, grading and exposure preserve their own time and mood.
inline constexpr std::array<MissionSkyboxProfile, 10U> mission_skybox_profiles{{
    {0U,
     MissionSkyboxTexture::dc_night,
     "dc_night.bmp",
     0.04F,
     {0.92F, 0.95F, 1.00F},
     0.78F,
     0.17F,
     1.00F,
     true},
    {3U,
     MissionSkyboxTexture::park_storm,
     "park_storm.bmp",
     0.19F,
     {0.82F, 0.94F, 0.90F},
     0.72F,
     0.20F,
     1.00F,
     false},
    {7U,
     MissionSkyboxTexture::kazakhstan_night,
     "kazakhstan_night.bmp",
     0.02F,
     {0.78F, 0.90F, 1.00F},
     0.76F,
     0.18F,
     1.00F,
     false},
    {9U,
     MissionSkyboxTexture::kazakhstan_night,
     "kazakhstan_night.bmp",
     0.31F,
     {0.86F, 0.94F, 1.00F},
     0.86F,
     0.16F,
     1.00F,
     false},
    {10U,
     MissionSkyboxTexture::kazakhstan_night,
     "kazakhstan_night.bmp",
     0.58F,
     {0.90F, 0.88F, 0.86F},
     0.80F,
     0.19F,
     1.00F,
     false},
    {11U,
     MissionSkyboxTexture::stronghold_dawn,
     "stronghold_dawn.bmp",
     0.08F,
     {0.88F, 0.92F, 1.00F},
     0.80F,
     0.28F,
     1.00F,
     true},
    {12U,
     MissionSkyboxTexture::stronghold_dawn,
     "stronghold_dawn.bmp",
     0.47F,
     {1.00F, 0.96F, 0.93F},
     0.92F,
     0.26F,
     1.00F,
     true},
    {14U,
     MissionSkyboxTexture::almaty_industrial,
     "almaty_industrial.bmp",
     0.11F,
     {0.82F, 0.91F, 0.90F},
     0.72F,
     0.19F,
     1.00F,
     false},
    {15U,
     MissionSkyboxTexture::almaty_industrial,
     "almaty_industrial.bmp",
     0.38F,
     {0.78F, 0.88F, 0.88F},
     0.68F,
     0.20F,
     1.00F,
     false},
    {16U,
     MissionSkyboxTexture::almaty_industrial,
     "almaty_industrial.bmp",
     0.66F,
     {1.00F, 0.88F, 0.76F},
     0.84F,
     0.18F,
     1.00F,
     false},
}};

[[nodiscard]] constexpr const MissionSkyboxProfile *
missionSkyboxProfile(std::uint32_t mission_index) noexcept {
  for (const auto &profile : mission_skybox_profiles) {
    if (profile.mission_index == mission_index) {
      return &profile;
    }
  }
  return nullptr;
}

} // namespace sf::platform
