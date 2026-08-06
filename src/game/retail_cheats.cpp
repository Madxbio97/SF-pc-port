#include "sf/game/retail_cheats.hpp"

#include <algorithm>
#include <array>

namespace sf::game {
namespace {

constexpr std::uint16_t select_button = 0x0001U;
constexpr std::uint16_t start_button = 0x0008U;
constexpr std::uint16_t right_button = 0x0020U;
constexpr std::uint16_t left_button = 0x0080U;
constexpr std::uint16_t l2_button = 0x0100U;
constexpr std::uint16_t r2_button = 0x0200U;
constexpr std::uint16_t l1_button = 0x0400U;
constexpr std::uint16_t r1_button = 0x0800U;
constexpr std::uint16_t circle_button = 0x2000U;
constexpr std::uint16_t cross_button = 0x4000U;
constexpr std::uint16_t square_button = 0x8000U;
constexpr std::uint16_t digital_buttons =
    0xffffU; // PAD buttons occupy the complete 16-bit retail sample.

struct CheatChord {
  RetailCheat cheat;
  RetailPauseCheatContext context;
  std::uint16_t buttons;
};

// Start is ignored after it opens the ACD. This preserves the original
// input sequence even when a controller reports Start for one extra sample.
[[nodiscard]] constexpr bool chordHeld(std::uint16_t held,
                                       std::uint16_t chord) noexcept {
  const auto allowed = static_cast<std::uint16_t>(chord | start_button);
  return (held & chord) == chord &&
         (held & digital_buttons & static_cast<std::uint16_t>(~allowed)) == 0U;
}

constexpr std::array pause_chords{
    // USA v1.1.
    CheatChord{RetailCheat::all_weapons,
               RetailPauseCheatContext::weapons_section,
               right_button | l2_button | r2_button | square_button |
                   circle_button | cross_button},
    CheatChord{RetailCheat::one_shot_kills,
               RetailPauseCheatContext::silenced_9mm,
               left_button | select_button | square_button | cross_button |
                   l1_button | r2_button},
    CheatChord{RetailCheat::stage_select,
               RetailPauseCheatContext::select_mission,
               left_button | l1_button | r1_button | select_button |
                   square_button | cross_button},
    CheatChord{RetailCheat::weak_enemies, RetailPauseCheatContext::map,
               right_button | l1_button | r1_button | cross_button},
    CheatChord{RetailCheat::movie_theater, RetailPauseCheatContext::map,
               right_button | l2_button | r1_button | cross_button},
    // PAL aliases published for the European release.
    CheatChord{RetailCheat::all_weapons,
               RetailPauseCheatContext::weapons_section,
               select_button | l1_button | l2_button | r2_button |
                   circle_button | cross_button},
    CheatChord{RetailCheat::stage_select,
               RetailPauseCheatContext::select_mission,
               l1_button | l2_button | r1_button | square_button |
                   circle_button | cross_button},
    CheatChord{RetailCheat::one_shot_kills,
               RetailPauseCheatContext::objectives,
               right_button | l1_button | r1_button | r2_button |
                   circle_button | cross_button},
};

} // namespace

bool RetailCheatState::enabled(RetailCheat cheat) const noexcept {
  switch (cheat) {
  case RetailCheat::all_weapons:
    return all_weapons;
  case RetailCheat::one_shot_kills:
    return one_shot_kills;
  case RetailCheat::stage_select:
    return stage_select;
  case RetailCheat::weak_enemies:
    return weak_enemies;
  case RetailCheat::movie_theater:
    return movie_theater;
  }
  return false;
}

void RetailCheatState::set(RetailCheat cheat, bool enabled_value) noexcept {
  switch (cheat) {
  case RetailCheat::all_weapons:
    all_weapons = enabled_value;
    break;
  case RetailCheat::one_shot_kills:
    one_shot_kills = enabled_value;
    break;
  case RetailCheat::stage_select:
    stage_select = enabled_value;
    break;
  case RetailCheat::weak_enemies:
    weak_enemies = enabled_value;
    break;
  case RetailCheat::movie_theater:
    movie_theater = enabled_value;
    break;
  }
}

void RetailCheatState::enableAll() noexcept {
  all_weapons = true;
  one_shot_kills = true;
  stage_select = true;
  weak_enemies = true;
  movie_theater = true;
}

RetailCheat retailCheatAt(std::size_t index) noexcept {
  constexpr std::array cheats{
      RetailCheat::all_weapons, RetailCheat::one_shot_kills,
      RetailCheat::stage_select, RetailCheat::weak_enemies,
      RetailCheat::movie_theater,
  };
  return cheats[std::min(index, cheats.size() - 1U)];
}

const char *retailCheatDisplayName(RetailCheat cheat) noexcept {
  switch (cheat) {
  case RetailCheat::all_weapons:
    return "All Weapons + Infinite Ammo";
  case RetailCheat::one_shot_kills:
    return "One-Shot Kills";
  case RetailCheat::stage_select:
    return "Stage Select";
  case RetailCheat::weak_enemies:
    return "Weak Enemies";
  case RetailCheat::movie_theater:
    return "Movie Theater";
  }
  return "Unknown";
}

std::optional<RetailCheat>
detectRetailPauseCheat(std::uint16_t held_buttons,
                       RetailPauseCheatContext context) noexcept {
  for (const auto &chord : pause_chords) {
    if (chord.context == context && chordHeld(held_buttons, chord.buttons)) {
      return chord.cheat;
    }
  }
  return std::nullopt;
}

const char *retailCheatName(RetailCheat cheat) noexcept {
  switch (cheat) {
  case RetailCheat::all_weapons:
    return "all-weapons-infinite-ammo";
  case RetailCheat::one_shot_kills:
    return "one-shot-kills";
  case RetailCheat::stage_select:
    return "stage-select";
  case RetailCheat::weak_enemies:
    return "weak-enemies";
  case RetailCheat::movie_theater:
    return "movie-theater";
  }
  return "unknown";
}

} // namespace sf::game
