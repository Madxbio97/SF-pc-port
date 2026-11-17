#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/internal/g4_campaign_transition_probe_access.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
  // Production restores a requested checkpoint before advancing the retired
  // timeline's audio clock. Use that same boundary in this headless gate.
  return !gameplay.runtimeFaulted() && (gameplay.failureRestartRequested() ||
                                        gameplay.advanceAudioFrameClock());
}

bool matchesOriginalProjectile(
    sf::game::LegacyGameplayVm &vm, std::uint32_t descriptor_pointer,
    const sf::game::GameplayProjectile &presented,
    const sf::game::LegacyThrownProjectileBridgeState &bridge) {
  auto &cpu = vm.runtime();
  std::uint32_t descriptor{}, display{}, matrix{};
  if (!cpu.read32(descriptor_pointer, descriptor) ||
      !cpu.read32(descriptor + 8U, display) ||
      !cpu.read32(display + 0x0cU, matrix) || matrix == 0U)
    return false;
  std::array<std::int32_t, 3U> position{};
  for (std::uint32_t i{}; i < position.size(); ++i) {
    std::uint32_t bits{};
    if (!cpu.read32(matrix + 0x14U + i * 4U, bits))
      return false;
    position[i] = std::bit_cast<std::int32_t>(bits);
  }
  for (std::uint32_t i{}; i < presented.rotation.size(); ++i) {
    std::uint16_t bits{};
    if (!cpu.read16(matrix + i * 2U, bits) ||
        presented.rotation[i] != std::bit_cast<std::int16_t>(bits) ||
        bridge.transform.rotation[i] != presented.rotation[i])
      return false;
  }
  return presented.x == static_cast<double>(position[0]) &&
         presented.y == static_cast<double>(position[1]) &&
         presented.z == static_cast<double>(position[2]) &&
         bridge.transform.translation ==
             sf::game::LegacyNativePoint{position[0], position[1], position[2]};
}

int run(const std::filesystem::path &cue, std::uint32_t mission_index,
        sf::game::WeaponId weapon, bool campaign_transform_fixture,
        bool check_aftermath) {
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
  // Let startup radio and its release/re-arm latch finish before holding L1.
  // A control-ready actor can still have an authored radio viewport. Require
  // a quiet neutral interval in every mission, not just Georgia Street.
  auto quiet_updates = std::uint32_t{};
  for (std::uint32_t update = 0U; update < 2'000U && quiet_updates < 64U;
       ++update) {
    if (!step(gameplay))
      return 14;
    quiet_updates = controlReady(gameplay) && !gameplay.letterboxActive()
                        ? quiet_updates + 1U
                        : 0U;
  }
  if (quiet_updates != 64U)
    return 14;
  if (!gameplay.activateRetailAllWeaponsCheat()) {
    return 2;
  }

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
  const auto gas = weapon == sf::game::WeaponId::gas_grenade;
  if (hud_layers.size() != 2U ||
      hud_layers[0] != (gas ? "GASGRENA.TIM" : "GRENADEA.TIM") ||
      hud_layers[1] != (gas ? "GASGRENB.TIM" : "GRENADEB.TIM")) {
    return 13;
  }

  const auto checkpoint_frame = gameplay.legacyPresentationFrame();
  if (!checkpoint_frame || !checkpoint_frame->ui || !checkpoint_frame->renderer)
    return 18;
  if (!sf::game::G4CampaignTransitionProbeAccess::captureCheckpoint(gameplay))
    return 18;
  if (campaign_transform_fixture) {
    // Reproduce only FUN_80025dfc's proven descriptor selector write, then
    // invoke the real throw animation callback. Campaign starts can interrupt
    // wind-up with damage; ordinary PAD input has its own separate fixtures.
    auto *vm =
        sf::game::G4CampaignTransitionProbeAccess::occlusionProbeVm(gameplay);
    constexpr auto profile =
        sf::game::syphonFilterUsaV11GameplayBridgeProfile();
    std::uint32_t player{}, descriptor{};
    std::uint16_t slot{};
    std::uint8_t available{};
    if (!vm || !vm->runtime().read32(profile.player_pointer, player) ||
        !vm->runtime().read16(player + 2U, slot) ||
        !vm->runtime().read32(profile.player_thrown_projectile_pointer,
                              descriptor) ||
        !vm->runtime().read8(descriptor, available) || available == 0U ||
        !vm->runtime().write32(descriptor + 4U,
                               static_cast<std::uint32_t>(weapon)) ||
        !vm->invoke(0x80026608U, std::array{std::uint32_t{slot}}, 5'000'000U)
             .completed())
      return 20;
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
  auto projectile_samples = std::size_t{};
  auto first_projectile_position = sf::game::LegacyNativePoint{};
  auto *diagnostic_vm =
      sf::game::G4CampaignTransitionProbeAccess::occlusionProbeVm(gameplay);
  std::optional<std::uint32_t> death_update, restart_update;
  const auto maximum_updates = check_aftermath ? 320U : 180U;
  for (std::uint32_t update = 0U; update < maximum_updates; ++update) {
    if (!step(gameplay,
              sf::game::GameplayInput{
                  .aim = !campaign_transform_fixture && update < 150U,
                  .fire_pressed = !campaign_transform_fixture && update == 20U,
                  .fire_held = !campaign_transform_fixture && update >= 20U &&
                               update < 52U,
              })) {
      const auto *runtime =
          sf::game::G4CampaignTransitionProbeAccess::checkpointProbeRuntime(
              gameplay);
      std::cerr << "grenade update failed: update=" << update
                << " runtime=" << static_cast<int>(runtime->faultReason())
                << " renderer-stage="
                << static_cast<int>(runtime->rendererBridgeStage())
                << " detail=" << runtime->faultDetail()
                << " finished=" << runtime->finished()
                << " health=" << gameplay.hud().vitals().health << '\n';
      if (diagnostic_vm) {
        std::uint32_t state{};
        (void)diagnostic_vm->runtime().read32(
            sf::game::syphonFilterUsaV11RetailOuterFrameProfile().current_state,
            state);
        const auto live_mission = diagnostic_vm->readMissionBridgeState();
        std::cerr << "live-state=" << state << " pc=" << std::hex
                  << diagnostic_vm->runtime().state().pc << std::dec
                  << " mission=" << live_mission.has_value()
                  << " end=" << (live_mission && live_mission->terminal)
                  << " failure=" << (live_mission && live_mission->failure)
                  << " success=" << (live_mission && live_mission->success)
                  << '\n';
      }
      return 5;
    }
    const auto frame = gameplay.legacyPresentationFrame();
    if (!frame || !frame->renderer) {
      return 5;
    }
    const auto &renderer = frame->renderer->state;
    if (check_aftermath) {
      if (!frame->ui)
        return 21;
      const auto &mission = frame->ui->mission;
      if (mission.player_health <= 0 && !death_update)
        death_update = update;
      if (gameplay.failureRestartRequested()) {
        std::uint32_t state{};
        if (!death_update || !projectile_seen || mission.player_health > 0 ||
            mission.terminal || mission.failure || mission.success ||
            !diagnostic_vm ||
            !diagnostic_vm->runtime().read32(
                sf::game::syphonFilterUsaV11RetailOuterFrameProfile()
                    .current_state,
                state) ||
            state != 2U ||
            !frame->contains(
                sf::game::LegacyPresentationCommandType::restart_after_failure))
          return 21;
        restart_update = update;
        break;
      }
    }
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
      // The callback fixture covers the linked projectile lifetime. The
      // resulting gas/explosion and mission consequences have separate gates.
      if (campaign_transform_fixture && !check_aftermath && projectile_seen)
        break;
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
    if (gameplay.projectiles().size() !=
            1U + static_cast<std::size_t>(
                     renderer.enemy_thrown_projectile.has_value()) ||
        !gameplay.projectiles().front().retail_transform ||
        gameplay.projectiles().front().weapon != weapon) {
      return 8;
    }
    const auto &presented = gameplay.projectiles().front();
    auto *vm =
        sf::game::G4CampaignTransitionProbeAccess::occlusionProbeVm(gameplay);
    constexpr auto profile =
        sf::game::syphonFilterUsaV11GameplayBridgeProfile();
    if (!vm || !matchesOriginalProjectile(
                   *vm, profile.player_thrown_projectile_pointer, presented,
                   *renderer.thrown_projectile))
      return 15;
    if (renderer.enemy_thrown_projectile &&
        !matchesOriginalProjectile(*vm, profile.enemy_thrown_projectile_pointer,
                                   gameplay.projectiles().back(),
                                   *renderer.enemy_thrown_projectile))
      return 15;
    ++projectile_samples;
  }
  if (gameplay.runtimeFaulted()) {
    return 9;
  }
  if (check_aftermath &&
      (!death_update || !restart_update || *death_update >= *restart_update)) {
    std::cerr << "Missing original death/fade/restart sequence\n";
    return 21;
  }
  const auto before_restart_sequence = gameplay.legacyPresentationSequence();
  // Quick click uses the neutral PAD fixtures; the campaign fixture above
  // isolates the original projectile callback from combat interruptions.
  const auto quick_tap_fixture = !campaign_transform_fixture;
  // Check the real checkpoint in every campaign case.
  if (!gameplay.restartCheckpoint()) {
    const auto *runtime =
        sf::game::G4CampaignTransitionProbeAccess::checkpointProbeRuntime(
            gameplay);
    std::cerr << "grenade checkpoint restore failed: runtime="
              << static_cast<int>(runtime->faultReason()) << " renderer-stage="
              << static_cast<int>(runtime->rendererBridgeStage())
              << " detail=" << runtime->faultDetail() << '\n';
    return 19;
  }
  if (check_aftermath) {
    const auto restored = gameplay.legacyPresentationFrame();
    const auto &expected = checkpoint_frame->ui->mission;
    if (!restored || !restored->ui || !restored->renderer ||
        !sf::game::legacyPresentationFrameConsumable(*restored,
                                                     before_restart_sequence) ||
        gameplay.failureRestartRequested() || gameplay.missionFailed() ||
        !gameplay.playerAlive() ||
        restored->ui->mission.player_health != expected.player_health ||
        restored->ui->mission.inventory.current_weapon !=
            expected.inventory.current_weapon ||
        restored->ui->mission.inventory.owned_weapons !=
            expected.inventory.owned_weapons ||
        restored->ui->mission.inventory.magazines !=
            expected.inventory.magazines ||
        restored->ui->mission.inventory.reserves !=
            expected.inventory.reserves ||
        restored->renderer->state.player.position !=
            checkpoint_frame->renderer->state.player.position ||
        gameplay.consumeEndingMovieRequest())
      return 22;
    for (std::uint32_t update{}; update < 8U; ++update)
      if (!step(gameplay) || gameplay.failureRestartRequested())
        return 23;
    std::cout << "death-update=" << *death_update
              << " restart-update=" << *restart_update
              << " original-failure-flag=0 checkpoint-restored=1\n";
  }
  if (quick_tap_fixture) {
    auto quick_tap_projectile_seen = false;
    for (std::uint32_t update = 0U; update < 120U; ++update) {
      if (!step(gameplay, sf::game::GameplayInput{
                              .aim = true,
                              .fire_pressed = update == 0U,
                              .fire_held = false,
                          })) {
        return 16;
      }
      const auto frame = gameplay.legacyPresentationFrame();
      if (!frame || !frame->renderer) {
        return 16;
      }
      quick_tap_projectile_seen =
          quick_tap_projectile_seen ||
          frame->renderer->state.thrown_projectile.has_value();
    }
    if (!quick_tap_projectile_seen) {
      return 17;
    }
  }
  std::cout << "preview=" << preview_samples << " idle=" << idle_preview_samples
            << " charge=" << charge_preview_samples << " grew=" << strength_grew
            << " projectile=" << projectile_seen
            << " horizontal=" << projectile_moved_horizontally
            << " vertical=" << projectile_moved_vertically
            << " original-transform-samples=" << projectile_samples
            << " quick-tap="
            << (quick_tap_fixture ? "passed" : "separate-pad-fixture")
            << " mismatches=0" << '\n';
  const auto pad_charge_passed =
      campaign_transform_fixture ||
      (preview_samples >= 24U && idle_preview_samples >= 4U &&
       charge_preview_samples >= 12U && strength_grew);
  return pad_charge_passed && projectile_seen && projectile_samples != 0U &&
                 (campaign_transform_fixture ||
                  (projectile_moved_horizontally &&
                   projectile_moved_vertically))
             ? 0
             : 10;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 4 ||
        (argc == 4 && std::string_view{argv[3]} != "callback" &&
         std::string_view{argv[3]} != "aftermath")) {
      return 1;
    }
    // FUN_80047c0c deliberately restricts the all-weapons grant by mission.
    struct Case {
      std::uint32_t mission;
      sf::game::WeaponId weapon;
    };
    std::vector<Case> cases;
    const auto append_mission = [&](std::uint32_t mission) {
      if (mission < 5U || mission >= 14U)
        cases.push_back({mission, sf::game::WeaponId::fragmentation_grenade});
      if ((mission >= 7U && mission <= 13U) || mission == 19U)
        cases.push_back({mission, sf::game::WeaponId::gas_grenade});
    };
    if (argc == 2) {
      cases = {{0U, sf::game::WeaponId::fragmentation_grenade},
               {7U, sf::game::WeaponId::gas_grenade},
               {13U, sf::game::WeaponId::gas_grenade}};
    } else if (std::string_view{argv[2]} == "all") {
      if (argc != 3)
        return 1;
      for (const auto &mission : sf::game::missionCatalog())
        append_mission(mission.index);
    } else {
      const auto argument = std::string_view{argv[2]};
      std::uint32_t mission{};
      const auto parsed = std::from_chars(
          argument.data(), argument.data() + argument.size(), mission);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != argument.data() + argument.size() ||
          mission >= sf::game::missionCatalog().size())
        return 1;
      append_mission(mission);
    }
    if (cases.empty()) {
      std::cerr
          << "Original all-weapons grant has no grenade in this mission\n";
      return 1;
    }
    for (const auto &test : cases) {
      const auto result =
          run(argv[1], test.mission, test.weapon,
              argc == 4 || (argc == 3 && std::string_view{argv[2]} == "all"),
              argc == 4 && std::string_view{argv[3]} == "aftermath");
      std::cout << "mission=" << test.mission
                << " grenade=" << static_cast<unsigned>(test.weapon)
                << " result=" << result << std::endl;
      if (result != 0)
        return result;
    }
    std::cout << "Original grenade gate passed: cases=" << cases.size() << '\n';
    return 0;
  } catch (const sf::core::Error &error) {
    std::cerr << error.what() << '\n';
    return 11;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 12;
  }
}
