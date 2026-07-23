#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace sf::assets {

// Exact INIT.OVL briefing contract used by the supported retail executable.
// Coordinates are in the game's centre-origin 384x240 UI space.
struct RetailBriefingLayout final {
  static constexpr int region_x = -155;
  static constexpr int region_y = -90;
  static constexpr int region_width = 310;
  static constexpr int region_height = 170;
  static constexpr std::uint8_t red = 110U;
  static constexpr std::uint8_t green = 130U;
  static constexpr std::uint8_t blue = 200U;
  static constexpr int prompt_x = 170;
  static constexpr int prompt_y = 98;
  static constexpr std::string_view prompt = "Press %x to continue";
  static constexpr std::uint8_t cross_u = 94U;
  static constexpr std::uint8_t cross_v = 175U;
  static constexpr std::uint8_t cross_width = 10U;
  static constexpr std::uint8_t cross_height = 8U;
  static constexpr int cross_advance = 12;
  static constexpr int prompt_width = 117;
};

class MissionBriefing final {
public:
  [[nodiscard]] static MissionBriefing parse(std::span<const std::byte> dlf);
  [[nodiscard]] static MissionBriefing parseRecord(
      std::span<const std::byte> dlf,
      std::size_t record_index,
      std::string_view mission_title = {});
  [[nodiscard]] static MissionBriefing fallback(std::string title);

  [[nodiscard]] std::string_view location() const noexcept { return location_; }
  [[nodiscard]] std::string_view missionTitle() const noexcept {
    return mission_title_;
  }
  [[nodiscard]] std::string_view dateTime() const noexcept {
    return date_time_;
  }
  [[nodiscard]] std::string_view directive() const noexcept {
    return directive_;
  }
  [[nodiscard]] std::string_view additionalDirective() const noexcept {
    return additional_directive_;
  }
  [[nodiscard]] std::string retailTitle() const;
  [[nodiscard]] std::array<std::string_view, 2U>
  retailDirectives() const noexcept {
    return {directive_, additional_directive_};
  }

private:
  MissionBriefing(std::string location, std::string mission_title,
                  std::string date_time, std::string directive,
                  std::string additional_directive);

  std::string location_;
  std::string mission_title_;
  std::string date_time_;
  std::string directive_;
  std::string additional_directive_;
};

} // namespace sf::assets
