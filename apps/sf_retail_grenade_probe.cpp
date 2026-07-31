#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool controlReady(const sf::game::GameplaySession &gameplay) noexcept {
  const auto frame = gameplay.legacyPresentationFrame();
  return frame && frame->renderer && gameplay.legacyOpeningFinished() &&
         frame->renderer->state.player.resident &&
         !frame->renderer->state.player.control_locked &&
         !frame->renderer->state.camera.scripted &&
         !frame->renderer->state.camera.locked;
}

bool step(sf::game::GameplaySession &gameplay,
          const sf::game::GameplayInput &input = {}) {
  gameplay.update(input);
  return gameplay.advanceAudioFrameClock();
}

int run(const std::filesystem::path &cue, std::uint32_t mission_index) {
  auto disc = sf::game::GameDisc::open(cue);
  const auto package = sf::game::MissionPackage::load(disc, mission_index);
  auto gameplay = sf::game::GameplaySession{package};
  auto stable = std::uint32_t{};
  for (std::uint32_t update = 0U; update < 2'000U && stable < 8U; ++update) {
    if (!step(gameplay)) {
      return 2;
    }
    stable = controlReady(gameplay) ? stable + 1U : 0U;
  }
  if (stable != 8U) {
    return 2;
  }
  if (mission_index == 0U) {
    // Mission 0 starts its opening XA a few updates after control becomes
    // available. Drain the complete active -> quiet radio cycle before
    // holding aim; production correctly requires a release/re-arm when radio
    // takes ownership of first-person input.
    auto opening_radio_seen = false;
    auto quiet_updates = std::uint32_t{};
    auto radio_active = false;
    for (std::uint32_t update = 0U; update < 2'000U && quiet_updates < 8U;
         ++update) {
      if (!step(gameplay)) {
        return 14;
      }
      const auto frame = gameplay.legacyPresentationFrame();
      if (!frame || !frame->renderer) {
        return 14;
      }
      radio_active = gameplay.letterboxActive();
      opening_radio_seen = opening_radio_seen || radio_active;
      quiet_updates =
          opening_radio_seen && !radio_active ? quiet_updates + 1U : 0U;
    }
    if (!opening_radio_seen || quiet_updates != 8U) {
      return 14;
    }
  }
  if (!gameplay.activateRetailAllWeaponsCheat()) {
    return 2;
  }

  constexpr auto weapon = sf::game::WeaponId::fragmentation_grenade;
  for (std::uint32_t update = 0U;
       update < 300U && !gameplay.canEquipWeapon(weapon); ++update) {
    if (!step(gameplay)) {
      return 3;
    }
  }
  if (!gameplay.canEquipWeapon(weapon) || !gameplay.equipWeapon(weapon)) {
    return 3;
  }
  for (std::uint32_t update = 0U; update < 300U; ++update) {
    if (!step(gameplay)) {
      return 4;
    }
    if (gameplay.hud().inventory().current() == weapon) {
      break;
    }
  }
  if (gameplay.hud().inventory().current() != weapon) {
    return 4;
  }
  const auto hud_layers = sf::game::weaponDefinition(weapon).icon.layers();
  if (hud_layers.size() != 2U || hud_layers[0] != "GRENADEA.TIM" ||
      hud_layers[1] != "GRENADEB.TIM") {
    return 13;
  }

  auto preview_samples = std::uint32_t{};
  auto idle_preview_samples = std::uint32_t{};
  auto charge_preview_samples = std::uint32_t{};
  auto first_strength = std::uint16_t{};
  auto previous_strength = std::uint16_t{};
  auto strength_grew = false;
  auto projectile_seen = false;
  auto projectile_moved_horizontally = false;
  auto projectile_moved_vertically = false;
  auto projectile_followed_parabola = false;
  auto first_projectile_position = sf::game::LegacyNativePoint{};
  for (std::uint32_t update = 0U; update < 180U; ++update) {
    if (!step(gameplay, sf::game::GameplayInput{
                            .aim = update < 150U,
                            .fire_pressed = update == 20U,
                            .fire_held = update >= 20U && update < 52U,
                        })) {
      return 5;
    }
    const auto frame = gameplay.legacyPresentationFrame();
    if (!frame || !frame->renderer) {
      return 5;
    }
    const auto &renderer = frame->renderer->state;
    if (renderer.grenade_trajectory) {
      if (renderer.thrown_projectile) {
        return 6;
      }
      const auto &trajectory = *renderer.grenade_trajectory;
      if (trajectory.origin == trajectory.target ||
          trajectory.strength_q12 < 0x28fU ||
          trajectory.strength_q12 > 0xcccU) {
        return 7;
      }
      if (update < 20U) {
        if (trajectory.strength_q12 != 0x28fU) {
          return 7;
        }
        ++idle_preview_samples;
      } else if (update < 52U) {
        if (charge_preview_samples != 0U &&
            trajectory.strength_q12 < previous_strength) {
          return 7;
        }
        if (charge_preview_samples == 0U) {
          first_strength = trajectory.strength_q12;
        }
        previous_strength = trajectory.strength_q12;
        strength_grew = strength_grew || previous_strength > first_strength;
        ++charge_preview_samples;
      }
      ++preview_samples;
    }
    if (!renderer.thrown_projectile) {
      continue;
    }
    const auto &position = renderer.thrown_projectile->transform.translation;
    if (!projectile_seen) {
      first_projectile_position = position;
      projectile_seen = true;
    } else {
      projectile_moved_horizontally =
          projectile_moved_horizontally ||
          position.x != first_projectile_position.x ||
          position.z != first_projectile_position.z;
      projectile_moved_vertically = projectile_moved_vertically ||
                                    position.y != first_projectile_position.y;
    }
    if (gameplay.projectiles().size() != 1U ||
        !gameplay.projectiles().front().retail_transform ||
        gameplay.projectiles().front().weapon != weapon) {
      return 8;
    }
    const auto &presented = gameplay.projectiles().front();
    if (presented.x != static_cast<double>(position.x) ||
        presented.z != static_cast<double>(position.z)) {
      return 15;
    }
    projectile_followed_parabola =
        projectile_followed_parabola ||
        presented.y < -static_cast<double>(position.y) - 8.0;
  }
  if (gameplay.runtimeFaulted()) {
    return 9;
  }
  return preview_samples >= 24U && idle_preview_samples >= 4U &&
                 charge_preview_samples >= 12U && strength_grew &&
                 projectile_seen && projectile_moved_horizontally &&
                 projectile_moved_vertically && projectile_followed_parabola
             ? 0
             : 10;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 3) {
      return 1;
    }
    const auto mission_index =
        argc == 3 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 0U;
    return run(argv[1], mission_index);
  } catch (const sf::core::Error &error) {
    std::cerr << error.what() << '\n';
    return 11;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 12;
  }
}
