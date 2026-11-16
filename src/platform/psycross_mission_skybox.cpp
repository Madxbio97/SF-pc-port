#include "psycross_mission_skybox.hpp"

#include "sf/platform/mission_skybox_policy.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_render.h>
#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace sf::platform::detail {
namespace {

struct Vector3 {
  double x{};
  double y{};
  double z{};
};

Vector3 normalize(Vector3 value) noexcept {
  const auto length =
      std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
  if (!std::isfinite(length) || length <= 0.000'001) {
    return {};
  }
  return {value.x / length, value.y / length, value.z / length};
}

Vector3 cross(const Vector3 &first, const Vector3 &second) noexcept {
  return {first.y * second.z - first.z * second.y,
          first.z * second.x - first.x * second.z,
          first.x * second.y - first.y * second.x};
}

std::filesystem::path executableDirectory() noexcept {
  if (auto *base = SDL_GetBasePath(); base != nullptr) {
    const std::filesystem::path result{base};
    SDL_free(base);
    if (!result.empty()) {
      return result;
    }
  }
  std::error_code error;
  return std::filesystem::current_path(error);
}

std::filesystem::path skyboxAssetPath(std::string_view name) noexcept {
  const auto relative = std::filesystem::path{"assets"} / "skyboxes" / name;
  const auto packaged = executableDirectory() / relative;
  std::error_code error;
  if (std::filesystem::is_regular_file(packaged, error)) {
    return packaged;
  }
  error.clear();
  const auto development = std::filesystem::current_path(error) / relative;
  return error ? packaged : development;
}

} // namespace

MissionSkybox::MissionSkybox(std::uint32_t mission_index,
                             bool enabled) noexcept
    : profile_(enabled ? missionSkyboxProfile(mission_index) : nullptr) {
  if (profile_ == nullptr) {
    return;
  }

  const auto path = skyboxAssetPath(profile_->asset_name);
  const auto path_text = path.string();
  SDL_Surface *loaded = SDL_LoadBMP(path_text.c_str());
  if (loaded == nullptr) {
    PsyX_Log_Warning("Mission skybox unavailable: %s (%s)\n", path_text.c_str(),
                     SDL_GetError());
    return;
  }
  SDL_Surface *rgba =
      SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0U);
  SDL_FreeSurface(loaded);
  if (rgba == nullptr || rgba->w <= 0 || rgba->h <= 0 ||
      rgba->w != rgba->h * 2) {
    PsyX_Log_Warning("Mission skybox must be a valid 2:1 RGBA image: %s\n",
                     path_text.c_str());
    if (rgba != nullptr) {
      SDL_FreeSurface(rgba);
    }
    return;
  }

  const auto row_bytes = static_cast<std::size_t>(rgba->w) * 4U;
  std::vector<unsigned char> pixels(row_bytes *
                                    static_cast<std::size_t>(rgba->h));
  const auto *source = static_cast<const unsigned char *>(rgba->pixels);
  for (int row = 0; row < rgba->h; ++row) {
    std::memcpy(pixels.data() + static_cast<std::size_t>(row) * row_bytes,
                source + static_cast<std::size_t>(row) *
                             static_cast<std::size_t>(rgba->pitch),
                row_bytes);
  }
  texture_ = GR_CreateSkyboxTexture(rgba->w, rgba->h, pixels.data());
  SDL_FreeSurface(rgba);
  if (texture_ == 0U) {
    PsyX_Log_Warning("Mission skybox upload failed: %s\n", path_text.c_str());
    return;
  }
  PsyX_Log_Info("Mission skybox: mission %u, %s\n", mission_index + 1U,
                path_text.c_str());
}

MissionSkybox::~MissionSkybox() {
  if (texture_ != 0U) {
    GR_DestroyTexture(texture_);
  }
}

bool MissionSkybox::replacesRetailScrim() const noexcept {
  // Missing generated assets must retain the original PS1 backdrop.
  return ready() && profile_ != nullptr && profile_->replace_retail_scrim;
}

void MissionSkybox::draw(const game::CameraState &camera,
                         std::uint8_t horizon_red, std::uint8_t horizon_green,
                         std::uint8_t horizon_blue) const noexcept {
  if (!ready() || profile_ == nullptr || camera.projection <= 0) {
    return;
  }

  const auto forward =
      normalize({camera.target_x - camera.x, camera.target_y - camera.y,
                 camera.target_z - camera.z});
  const auto right = normalize(cross(forward, {0.0, -1.0, 0.0}));
  const auto down = normalize(cross(forward, right));
  if ((forward.x == 0.0 && forward.y == 0.0 && forward.z == 0.0) ||
      (right.x == 0.0 && right.y == 0.0 && right.z == 0.0) ||
      (down.x == 0.0 && down.y == 0.0 && down.z == 0.0)) {
    return;
  }

  const GrSkyboxView view{
      static_cast<float>(right.x),
      static_cast<float>(right.y),
      static_cast<float>(right.z),
      static_cast<float>(down.x),
      static_cast<float>(down.y),
      static_cast<float>(down.z),
      static_cast<float>(forward.x),
      static_cast<float>(forward.y),
      static_cast<float>(forward.z),
      camera.projection,
      384,
      240,
      static_cast<float>(horizon_red) / 255.0F,
      static_cast<float>(horizon_green) / 255.0F,
      static_cast<float>(horizon_blue) / 255.0F,
      profile_->yaw_offset_turns,
      profile_->tint[0],
      profile_->tint[1],
      profile_->tint[2],
      profile_->exposure,
      profile_->horizon_band,
      profile_->horizon_strength,
  };
  GR_DrawSkybox(texture_, &view);
}

} // namespace sf::platform::detail
