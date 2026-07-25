#include "sf/game/retail_pause_map.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace sf::game {
namespace {

enum class RetailPauseMapProjection {
  none,
  subway,
  main_subway,
  park,
  museum,
  museum_dinorama,
  base,
  base_tower,
  stronghold,
  stronghold_lower,
  warehouse,
  warehouse_76,
  silo,
};

// Direct decode of USA v1.1 MENU.OVL 0x80146130. Repeated entries are
// repeated function pointers in the retail table; `none` entries are nulls.
constexpr std::array retail_pause_map_projections{
    RetailPauseMapProjection::subway,           // 0  0x8013df9c
    RetailPauseMapProjection::subway,           // 1  0x8013df9c
    RetailPauseMapProjection::main_subway,      // 2  0x8013e180
    RetailPauseMapProjection::park,             // 3  0x8013e1e0
    RetailPauseMapProjection::park,             // 4  0x8013e1e0
    RetailPauseMapProjection::museum,           // 5  0x8013e240
    RetailPauseMapProjection::museum_dinorama,  // 6  0x8013e2a4
    RetailPauseMapProjection::base,             // 7  0x8013e304
    RetailPauseMapProjection::none,             // 8  null
    RetailPauseMapProjection::base_tower,       // 9  0x8013e374
    RetailPauseMapProjection::base,             // 10 0x8013e304
    RetailPauseMapProjection::stronghold,       // 11 0x8013e3d8
    RetailPauseMapProjection::stronghold_lower, // 12 0x8013e43c
    RetailPauseMapProjection::none,             // 13 null
    RetailPauseMapProjection::warehouse,        // 14 0x8013e4a0
    RetailPauseMapProjection::warehouse,        // 15 0x8013e4a0
    RetailPauseMapProjection::warehouse_76,     // 16 0x8013e50c
    RetailPauseMapProjection::none,             // 17 null
    RetailPauseMapProjection::none,             // 18 null
    RetailPauseMapProjection::silo,             // 19 0x8013e570
};

[[nodiscard]] RetailPauseMapProjection
retailPauseMapProjection(std::size_t mission) noexcept {
  return mission < retail_pause_map_projections.size()
             ? retail_pause_map_projections[mission]
             : RetailPauseMapProjection::none;
}

[[nodiscard]] std::int32_t
retailSignedMultiplyHigh(std::int32_t value,
                         std::uint32_t multiplier) noexcept {
  const auto signed_multiplier = std::bit_cast<std::int32_t>(multiplier);
  return static_cast<std::int32_t>(
      (static_cast<std::int64_t>(value) * signed_multiplier) >> 32);
}

} // namespace

std::span<const RetailPauseMapRecord>
retailPauseMapRecords(std::size_t mission) noexcept {
  // USA v1.1 MENU.OVL 0x801460e0. Coordinates are authored pixel offsets
  // from the centre of MAPn.TIM; objective ordinals are zero based.
  static constexpr std::array mission_0{
      RetailPauseMapRecord{0, 0, -6, -54},  RetailPauseMapRecord{1, 0, 53, 77},
      RetailPauseMapRecord{2, 0, -16, -49}, RetailPauseMapRecord{3, 1, -11, 3},
      RetailPauseMapRecord{4, 1, 28, -14},
  };
  static constexpr std::array mission_1{
      RetailPauseMapRecord{0, 0xff, 0, 0},
      RetailPauseMapRecord{1, 1, -19, -12},
      RetailPauseMapRecord{2, 1, -32, -78},
  };
  static constexpr std::array mission_3{
      RetailPauseMapRecord{0, 0xff, 0, 0}, RetailPauseMapRecord{1, 0, 31, -14},
      RetailPauseMapRecord{2, 0, 12, -34}, RetailPauseMapRecord{3, 0, 12, -55},
      RetailPauseMapRecord{4, 0, 12, -87},
  };
  static constexpr std::array mission_5{
      RetailPauseMapRecord{0, 0, 8, -34},
      RetailPauseMapRecord{1, 0xff, 0, 0},
      RetailPauseMapRecord{2, 0, 27, 69},
  };
  static constexpr std::array mission_6{
      RetailPauseMapRecord{0, 0xff, 0, 0},
      RetailPauseMapRecord{1, 0, -26, 53},
  };
  static constexpr std::array mission_7{
      RetailPauseMapRecord{0, 0xff, 0, 0},  RetailPauseMapRecord{1, 0xff, 0, 0},
      RetailPauseMapRecord{2, 0, 79, 34},   RetailPauseMapRecord{3, 0, 52, 0},
      RetailPauseMapRecord{4, 0, -12, -67},
  };
  static constexpr std::array mission_9{
      RetailPauseMapRecord{0, 0, 33, 33},
      RetailPauseMapRecord{1, 0xff, 0, 0},
  };
  static constexpr std::array mission_12{
      RetailPauseMapRecord{0, 0xff, 0, 0},
      RetailPauseMapRecord{1, 0xff, 0, 0},
      RetailPauseMapRecord{2, 0xff, 0, 0},
      RetailPauseMapRecord{3, 0, 35, -43},
  };
  static constexpr std::array mission_14{
      RetailPauseMapRecord{0, 0xff, 0, 0},
      RetailPauseMapRecord{1, 0xff, 0, 0},
      RetailPauseMapRecord{2, 0xff, 0, 0},
      RetailPauseMapRecord{3, 0, 28, 0},
  };
  static constexpr std::array mission_16{
      RetailPauseMapRecord{0, 0, -3, 47},
  };
  static constexpr std::array mission_19{
      RetailPauseMapRecord{0, 0, 28, 21},
      RetailPauseMapRecord{1, 1, 8, 82},
      RetailPauseMapRecord{2, 0xff, 0, 0},
  };
  switch (mission) {
  case 0:
    return mission_0;
  case 1:
    return mission_1;
  case 3:
  case 4:
    return mission_3;
  case 5:
    return mission_5;
  case 6:
    return mission_6;
  case 7:
  case 10:
    return mission_7;
  case 9:
    return mission_9;
  case 12:
    return mission_12;
  case 14:
  case 15:
    return mission_14;
  case 16:
    return mission_16;
  case 19:
    return mission_19;
  default:
    return {};
  }
}

bool retailPauseMapAvailable(std::size_t mission) noexcept {
  return retailPauseMapProjection(mission) != RetailPauseMapProjection::none;
}

std::optional<RetailPauseMapPoint>
retailPauseMapPlayer(std::size_t mission, std::int32_t x, std::int32_t y,
                     std::int32_t z) noexcept {
  const auto sign_x = x >> 31;
  const auto sign_y = y >> 31;
  const auto sign_z = z >> 31;
  switch (retailPauseMapProjection(mission)) {
  case RetailPauseMapProjection::subway:
    if (y >= 0x79f && x >= -0xb21) {
      return RetailPauseMapPoint{
          0,
          ((retailSignedMultiplyHigh(z, 0x939a85c5U) + z) >> 6) - sign_z - 61,
          ((retailSignedMultiplyHigh(x, 0x939a85c5U) + x) >> 6) - sign_x - 53,
      };
    }
    if (y < 0x407) {
      if (y > 0) {
        return RetailPauseMapPoint{
            1,
            (retailSignedMultiplyHigh(x, 0x094f2095U) >> 2) - sign_x - 23,
            sign_z - (retailSignedMultiplyHigh(z, 0x76b981dbU) >> 6) - 4,
        };
      }
      return RetailPauseMapPoint{
          1,
          (retailSignedMultiplyHigh(x, 0x22b63cbfU) >> 4) - sign_x + 31,
          sign_z - ((retailSignedMultiplyHigh(z, 0xea0ea0ebU) + z) >> 7) - 6,
      };
    }
    if (static_cast<std::uint32_t>(y - 0x407) < 0x398U) {
      return RetailPauseMapPoint{
          2,
          (retailSignedMultiplyHigh(x, 0x3e0f83e1U) >> 3) - sign_x - 37,
          sign_z - (retailSignedMultiplyHigh(z, 0x78787879U) >> 4) + 107,
      };
    }
    return std::nullopt;
  case RetailPauseMapProjection::main_subway:
    return RetailPauseMapPoint{
        0, (retailSignedMultiplyHigh(x, 0x3e007c01U) >> 8) - sign_x + 12,
        sign_z - (retailSignedMultiplyHigh(z, 0x3be7a9e3U) >> 8) + 37};
  case RetailPauseMapProjection::park:
    return RetailPauseMapPoint{
        0, (retailSignedMultiplyHigh(x, 0x75ded953U) >> 7) - sign_x - 4,
        sign_z - (retailSignedMultiplyHigh(z, 0x76b981dbU) >> 7) - 3};
  case RetailPauseMapProjection::museum:
    return RetailPauseMapPoint{
        0, ((retailSignedMultiplyHigh(x, 0xac769185U) + x) >> 6) - sign_x + 69,
        sign_z - ((retailSignedMultiplyHigh(z, 0xac769185U) + z) >> 6) - 18};
  case RetailPauseMapProjection::museum_dinorama:
    return RetailPauseMapPoint{
        0, (retailSignedMultiplyHigh(x, 0x6bca1af3U) >> 5) - sign_x + 28,
        sign_z - (retailSignedMultiplyHigh(z, 0x3531dec1U) >> 4) - 168};
  case RetailPauseMapProjection::base_tower:
    return RetailPauseMapPoint{
        0, (retailSignedMultiplyHigh(x, 0x2fa0be83U) >> 4) - sign_x - 7,
        sign_z - ((retailSignedMultiplyHigh(z, 0xb81702e1U) + z) >> 6) - 17};
  case RetailPauseMapProjection::base:
    if (y < -179) {
      return std::nullopt;
    }
    return RetailPauseMapPoint{
        0, ((retailSignedMultiplyHigh(x, 0x939a85c5U) + x) >> 6) - sign_x - 12,
        sign_z - (retailSignedMultiplyHigh(z, 0x4bda12f7U) >> 5) - 40};
  case RetailPauseMapProjection::stronghold:
    return RetailPauseMapPoint{
        0, (retailSignedMultiplyHigh(z, 0x38e38e39U) >> 5) - sign_z + 27,
        ((retailSignedMultiplyHigh(x, 0x88888889U) + x) >> 6) - sign_x - 11};
  case RetailPauseMapProjection::stronghold_lower:
    return RetailPauseMapPoint{
        0, (retailSignedMultiplyHigh(z, 0x78787879U) >> 6) - sign_z + 64,
        ((retailSignedMultiplyHigh(x, 0xf2b9d649U) + x) >> 7) - sign_x + 37};
  case RetailPauseMapProjection::warehouse:
    if (y < 0x175) {
      return std::nullopt;
    }
    return RetailPauseMapPoint{
        0, (retailSignedMultiplyHigh(x, 0x214d0215U) >> 4) - sign_x - 2,
        sign_z - (retailSignedMultiplyHigh(z, 0x1b4e81b5U) >> 4) + 14};
  case RetailPauseMapProjection::warehouse_76:
    return RetailPauseMapPoint{
        0, (retailSignedMultiplyHigh(x, 0x67b23a55U) >> 5) - sign_x - 25,
        sign_z - ((retailSignedMultiplyHigh(z, 0xac769185U) + z) >> 6) + 47};
  case RetailPauseMapProjection::silo:
    if (x >= 0x1349) {
      return RetailPauseMapPoint{
          0,
          ((retailSignedMultiplyHigh(x, 0x8d3dcb09U) + x) >> 4) - sign_x - 244,
          sign_y - ((retailSignedMultiplyHigh(y, 0x8d3dcb09U) + y) >> 4) + 41};
    }
    return RetailPauseMapPoint{
        1, sign_z - (retailSignedMultiplyHigh(z, 0x30c30c31U) >> 4) - 124,
        (retailSignedMultiplyHigh(x, 0x3159721fU) >> 4) - sign_x - 19};
  case RetailPauseMapProjection::none:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<RetailPauseMapPoint>
retailPauseMapPlayerOnPage(std::size_t mission, std::uint8_t page,
                           std::int32_t x, std::int32_t y,
                           std::int32_t z) noexcept {
  const auto sign_x = x >> 31;
  const auto sign_y = y >> 31;
  const auto sign_z = z >> 31;
  const auto projection = retailPauseMapProjection(mission);
  if (projection == RetailPauseMapProjection::subway) {
    switch (page) {
    case 0:
      return RetailPauseMapPoint{
          page,
          ((retailSignedMultiplyHigh(z, 0x939a85c5U) + z) >> 6) - sign_z - 61,
          ((retailSignedMultiplyHigh(x, 0x939a85c5U) + x) >> 6) - sign_x - 53,
      };
    case 1:
      if (y > 0) {
        return RetailPauseMapPoint{
            page,
            (retailSignedMultiplyHigh(x, 0x094f2095U) >> 2) - sign_x - 23,
            sign_z - (retailSignedMultiplyHigh(z, 0x76b981dbU) >> 6) - 4,
        };
      }
      return RetailPauseMapPoint{
          page,
          (retailSignedMultiplyHigh(x, 0x22b63cbfU) >> 4) - sign_x + 31,
          sign_z - ((retailSignedMultiplyHigh(z, 0xea0ea0ebU) + z) >> 7) - 6,
      };
    case 2:
      return RetailPauseMapPoint{
          page,
          (retailSignedMultiplyHigh(x, 0x3e0f83e1U) >> 3) - sign_x - 37,
          sign_z - (retailSignedMultiplyHigh(z, 0x78787879U) >> 4) + 107,
      };
    default:
      return std::nullopt;
    }
  }
  if (projection == RetailPauseMapProjection::silo) {
    if (page == 0U) {
      return RetailPauseMapPoint{
          page,
          ((retailSignedMultiplyHigh(x, 0x8d3dcb09U) + x) >> 4) - sign_x - 244,
          sign_y - ((retailSignedMultiplyHigh(y, 0x8d3dcb09U) + y) >> 4) + 41,
      };
    }
    if (page == 1U) {
      return RetailPauseMapPoint{
          page,
          sign_z - (retailSignedMultiplyHigh(z, 0x30c30c31U) >> 4) - 124,
          (retailSignedMultiplyHigh(x, 0x3159721fU) >> 4) - sign_x - 19,
      };
    }
    return std::nullopt;
  }
  const auto point = retailPauseMapPlayer(mission, x, y, z);
  return point && point->page == page ? point : std::nullopt;
}

} // namespace sf::game
