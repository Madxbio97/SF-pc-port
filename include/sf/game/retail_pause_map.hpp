#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace sf::game {

struct RetailPauseMapRecord {
  std::uint8_t objective{};
  std::uint8_t page{};
  std::int8_t x{};
  std::int8_t y{};
};

struct RetailPauseMapPoint {
  std::uint8_t page{};
  std::int32_t x{};
  std::int32_t y{};
};

[[nodiscard]] bool retailPauseMapAvailable(std::size_t mission) noexcept;

[[nodiscard]] std::span<const RetailPauseMapRecord>
retailPauseMapRecords(std::size_t mission) noexcept;

[[nodiscard]] std::optional<RetailPauseMapPoint>
retailPauseMapPlayer(std::size_t mission, std::int32_t x, std::int32_t y,
                     std::int32_t z) noexcept;

[[nodiscard]] std::optional<RetailPauseMapPoint>
retailPauseMapPlayerOnPage(std::size_t mission, std::uint8_t page,
                           std::int32_t x, std::int32_t y,
                           std::int32_t z) noexcept;

} // namespace sf::game
