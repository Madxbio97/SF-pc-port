#include "sf/game/retail_pause_map.hpp"

#include <cstdint>
#include <iostream>

namespace {

bool samePoint(const sf::game::RetailPauseMapPoint &point, std::uint8_t page,
               std::int32_t x, std::int32_t y) noexcept {
  return point.page == page && point.x == x && point.y == y;
}

} // namespace

int main() {
  const auto subway = sf::game::retailPauseMapPlayer(0U, 0, 0, 0);
  if (!subway || !samePoint(*subway, 1U, 31, -6)) {
    std::cerr << "Retail subway projection changed\n";
    return 1;
  }

  const auto main_subway = sf::game::retailPauseMapPlayer(2U, 0, 0, 0);
  if (!main_subway || !samePoint(*main_subway, 0U, 12, 37)) {
    std::cerr << "Retail main-subway projection changed\n";
    return 2;
  }

  const auto silo = sf::game::retailPauseMapPlayer(19U, 0, 0, 0);
  if (!silo || !samePoint(*silo, 1U, -124, -19)) {
    std::cerr << "Retail silo projection changed\n";
    return 3;
  }

  if (sf::game::retailPauseMapAvailable(8U) ||
      sf::game::retailPauseMapPlayer(8U, 0, 0, 0)) {
    std::cerr << "Mission without a retail map became available\n";
    return 4;
  }

  const auto records = sf::game::retailPauseMapRecords(0U);
  if (records.size() != 5U || records.front().objective != 0U ||
      records.front().page != 0U || records.front().x != -6 ||
      records.front().y != -54) {
    std::cerr << "Retail objective table changed\n";
    return 5;
  }

  std::cout << "retail pause-map tests passed\n";
  return 0;
}
