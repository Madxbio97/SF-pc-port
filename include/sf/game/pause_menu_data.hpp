#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sf::game {

class GameplaySession;
class MissionPackage;
struct PauseMenuData;

// The two authored DLF strings are consecutive parts of one briefing, not
// separate pages.  Wrap their combined text against the retail ACD column and
// split only after complete visible lines.
[[nodiscard]] std::vector<std::string>
paginatePauseBriefing(std::string_view directive,
                      std::string_view additional_directive);

[[nodiscard]] PauseMenuData
makePauseMenuData(const MissionPackage &mission,
                  const GameplaySession &gameplay,
                  std::uint32_t maximum_unlocked_mission);

} // namespace sf::game
