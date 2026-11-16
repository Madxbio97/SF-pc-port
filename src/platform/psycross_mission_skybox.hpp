#pragma once

#include "sf/game/chase_camera.hpp"

#include <cstdint>

namespace sf::platform {
struct MissionSkyboxProfile;
}

namespace sf::platform::detail {

class MissionSkybox final {
public:
  explicit MissionSkybox(std::uint32_t mission_index,
                         bool enabled = false) noexcept;
  ~MissionSkybox();

  MissionSkybox(const MissionSkybox &) = delete;
  MissionSkybox &operator=(const MissionSkybox &) = delete;

  [[nodiscard]] bool ready() const noexcept { return texture_ != 0U; }
  [[nodiscard]] bool replacesRetailScrim() const noexcept;
  void draw(const game::CameraState &camera, std::uint8_t horizon_red,
            std::uint8_t horizon_green,
            std::uint8_t horizon_blue) const noexcept;

private:
  const MissionSkyboxProfile *profile_{};
  unsigned int texture_{};
};

} // namespace sf::platform::detail
