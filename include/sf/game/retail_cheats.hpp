#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sf::game {

// Original controller cheats recovered from the retail US and PAL releases.
// Button masks use the normal host representation (one bit means held).
enum class RetailCheat : std::uint8_t {
  all_weapons,
  hard_mode,
  one_shot_kills,
  stage_select,
  weak_enemies,
  movie_theater,
};

inline constexpr std::size_t retail_cheat_count = 6U;

struct RetailCheatState {
  bool all_weapons{};
  bool hard_mode{};
  bool one_shot_kills{};
  bool stage_select{};
  bool weak_enemies{};
  bool movie_theater{};

  [[nodiscard]] bool enabled(RetailCheat cheat) const noexcept;
  void set(RetailCheat cheat, bool enabled) noexcept;
  void enableAll() noexcept;
};

[[nodiscard]] RetailCheat retailCheatAt(std::size_t index) noexcept;
[[nodiscard]] const char *retailCheatDisplayName(RetailCheat cheat) noexcept;

enum class RetailPauseCheatContext : std::uint8_t {
  none,
  map,
  objectives,
  weapons_section,
  silenced_9mm,
  select_mission,
};

[[nodiscard]] std::optional<RetailCheat>
detectRetailTitleCheat(std::uint16_t held_buttons,
                       bool new_game_selected) noexcept;

[[nodiscard]] std::optional<RetailCheat>
detectRetailPauseCheat(std::uint16_t held_buttons,
                       RetailPauseCheatContext context) noexcept;

[[nodiscard]] const char *retailCheatName(RetailCheat cheat) noexcept;

} // namespace sf::game
