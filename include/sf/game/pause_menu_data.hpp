#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sf::game {

class GameplaySession;
class MissionPackage;
struct PauseMenuData;
enum class WeaponId : std::uint8_t;

// The two authored DLF strings are consecutive parts of one briefing, not
// separate pages.  Wrap their combined text against the retail ACD column and
// split only after complete visible lines.
[[nodiscard]] std::vector<std::string>
paginatePauseBriefing(std::string_view directive,
                      std::string_view additional_directive);

// Original pre-rendered weapon artwork authored in each mission's MENU.HOG.
[[nodiscard]] std::string_view pauseWeaponArtAsset(WeaponId id) noexcept;

[[nodiscard]] PauseMenuData
makePauseMenuData(const MissionPackage &mission,
                  const GameplaySession &gameplay,
                  std::uint32_t maximum_unlocked_mission);

} // namespace sf::game
