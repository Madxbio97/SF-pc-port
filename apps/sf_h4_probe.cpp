#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/mission.hpp"
#include "sf/platform/player_input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <string_view>

namespace {

std::optional<std::array<std::int16_t, 4U>> scopePacketBounds(
    std::span<const sf::game::LegacyGuestRawPacketBridgeState> packets) {
  auto minimum_x = std::numeric_limits<std::int16_t>::max();
  auto minimum_y = std::numeric_limits<std::int16_t>::max();
  auto maximum_x = std::numeric_limits<std::int16_t>::min();
  auto maximum_y = std::numeric_limits<std::int16_t>::min();
  auto found = false;
  const auto include = [&](std::uint32_t word) {
    const auto x = static_cast<std::int16_t>(word & 0xffffU);
    const auto y = static_cast<std::int16_t>(word >> 16U);
    minimum_x = std::min(minimum_x, x);
    minimum_y = std::min(minimum_y, y);
    maximum_x = std::max(maximum_x, x);
    maximum_y = std::max(maximum_y, y);
    found = true;
  };
  for (const auto &packet : packets) {
    if (!sf::game::legacyGuestRawPacketIsRetailScopeOverlay(packet)) {
      continue;
    }
    const auto base_opcode = packet.opcode & 0xfdU;
    if (packet.word_count == 2U && base_opcode == 0x68U) {
      include(packet.words[1]);
    } else if (packet.word_count == 3U && base_opcode == 0x40U) {
      include(packet.words[1]);
      include(packet.words[2]);
    } else if (packet.word_count == 4U && base_opcode == 0x20U) {
      include(packet.words[1]);
      include(packet.words[2]);
      include(packet.words[3]);
    } else if (packet.word_count == 4U && base_opcode == 0x50U) {
      include(packet.words[1]);
      include(packet.words[3]);
    } else if (packet.word_count == 5U && base_opcode == 0x28U) {
      include(packet.words[1]);
      include(packet.words[2]);
      include(packet.words[3]);
      include(packet.words[4]);
    } else if (packet.word_count == 6U && base_opcode == 0x30U) {
      include(packet.words[1]);
      include(packet.words[3]);
      include(packet.words[5]);
    }
  }
  return found ? std::optional{std::array{minimum_x, minimum_y, maximum_x,
                                          maximum_y}}
               : std::nullopt;
}

int runScopeStressProbe(sf::game::GameDisc &disc) {
  const auto mission = sf::game::MissionPackage::load(disc, 18U);
  auto gameplay = std::make_unique<sf::game::GameplaySession>(mission);
  if (!gameplay->activateRetailAllWeaponsCheat()) {
    std::cerr << "scope stress: all-weapons activation failed\n";
    return 50;
  }
  for (std::uint32_t update = 0U; update < 256U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .direct_weapon =
            update == 0U
                ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(
                      sf::game::WeaponId::nightvision_rifle)}
                : std::nullopt,
    });
    if (gameplay->runtimeFaulted()) {
      std::cerr << "scope stress: selection fault reason="
                << gameplay->runtimeFaultReason()
                << " detail=" << gameplay->runtimeFaultDetail() << '\n';
      return 51;
    }
    if (gameplay->hud().inventory().current() ==
            sf::game::WeaponId::nightvision_rifle &&
        gameplay->legacyWeaponMenuState() == -5) {
      break;
    }
  }
  for (std::uint32_t update = 0U; update < 4'500U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .aim = true,
        .roll = update == 240U,
        .interact = update == 120U,
    });
    if (gameplay->runtimeFaulted()) {
      std::cerr << "scope stress: aim fault update=" << update
                << " reason=" << gameplay->runtimeFaultReason()
                << " detail=" << gameplay->runtimeFaultDetail() << '\n';
      return 52;
    }
  }
  const auto frame = gameplay->legacyPresentationFrame();
  const auto scope_packets =
      frame && frame->renderer
          ? std::ranges::count_if(
                frame->renderer->state.guest_raw_packets,
                &sf::game::legacyGuestRawPacketIsRetailScopeOverlay)
          : 0;
  const auto scope_bounds =
      frame && frame->renderer
          ? scopePacketBounds(frame->renderer->state.guest_raw_packets)
          : std::nullopt;
  std::cout << "scope stress passed: guest-frame="
            << (frame ? frame->guest_frame : 0U)
            << " sequence=" << (frame ? frame->sequence : 0U) << " ui="
            << (frame && frame->ui
                    ? static_cast<unsigned>(frame->ui->mission.interface_mode)
                    : 0U)
            << " aim="
            << (frame && frame->ui
                    ? static_cast<unsigned>(
                          frame->ui->mission.first_person_aim_mode)
                    : 0U)
            << " nv="
            << (frame && frame->renderer
                    ? frame->renderer->state.environment.nightvision_enabled
                    : false)
            << " scope-packets=" << scope_packets;
  if (scope_bounds) {
    std::cout << " guest-bounds=" << (*scope_bounds)[0] << ','
              << (*scope_bounds)[1] << ".." << (*scope_bounds)[2] << ','
              << (*scope_bounds)[3];
  }
  std::cout << '\n';
  if (!frame || !frame->ui || !frame->renderer ||
      frame->ui->mission.interface_mode != 3U ||
      frame->ui->mission.first_person_aim_mode != 3U ||
      !frame->renderer->state.environment.nightvision_enabled ||
      scope_packets == 0 || !scope_bounds ||
      *scope_bounds != std::array<std::int16_t, 4U>{-155, -55, 155, 52}) {
    std::cerr << "scope stress: incomplete retail SVD presentation\n";
    return 53;
  }
  return 0;
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{
        sf::core::ErrorCode::unsupported,
        "H4 presentation probe requires Syphon Filter USA v1.1"};
  }

  const auto mission = sf::game::MissionPackage::loadFirst(disc);
  auto runtime = std::make_unique<sf::game::LegacyFirstMissionRuntime>(
      mission.legacyImage());
  if (!runtime->ready() || runtime->faulted() ||
      !runtime->presentationFrame()) {
    std::cerr << "H4 presentation bootstrap fault\n";
    return 2;
  }

  const auto checkpoint_frame = runtime->presentationFrame();
  if (!checkpoint_frame ||
      !sf::game::legacyPresentationFrameConsumable(*checkpoint_frame, 0U) ||
      !runtime->captureCheckpoint()) {
    std::cerr << "H4 checkpoint capture gate failed\n";
    return 3;
  }
  runtime->setHostPadState({});
  runtime->advanceHostUpdate();
  const auto advanced_frame = runtime->presentationFrame();
  if (runtime->faulted() || !advanced_frame ||
      !sf::game::legacyPresentationFrameConsumable(
          *advanced_frame, checkpoint_frame->sequence) ||
      !runtime->restoreCheckpoint()) {
    std::cerr << "H4 checkpoint restore gate failed\n";
    return 4;
  }
  const auto restored_frame = runtime->presentationFrame();
  if (!restored_frame ||
      restored_frame->guest_frame != checkpoint_frame->guest_frame ||
      !sf::game::legacyPresentationFrameConsumable(*restored_frame,
                                                   advanced_frame->sequence)) {
    std::cerr << "H4 restored presentation frame is incoherent\n";
    return 5;
  }
  runtime->reset();
  const auto reset_frame = runtime->presentationFrame();
  if (runtime->faulted() || !reset_frame || reset_frame->guest_frame != 0U ||
      !sf::game::legacyPresentationFrameConsumable(*reset_frame,
                                                   restored_frame->sequence)) {
    std::cerr << "H4 reset presentation frame is incoherent\n";
    return 6;
  }

  constexpr std::uint32_t frame_count = 8U;
  auto previous_sequence = std::uint64_t{};
  auto maximum_objects = std::size_t{};
  auto posed_actors = std::size_t{};
  for (std::uint32_t sample = 0U; sample <= frame_count; ++sample) {
    const auto frame = runtime->presentationFrame();
    if (!frame ||
        !sf::game::legacyPresentationFrameConsumable(*frame,
                                                     previous_sequence) ||
        frame->guest_frame != runtime->guestFrame()) {
      std::cerr << "H4 presentation frame contract failed at sample " << sample
                << '\n';
      return 7;
    }

    const auto &render = frame->renderer->state;
    const auto &ui = frame->ui->mission;
    if (render.dynamic_first_slot > render.objects.size() ||
        ui.player_slot < 0 ||
        static_cast<std::size_t>(ui.player_slot) >= render.objects.size()) {
      std::cerr << "H4 renderer/UI frame is incoherent at sample " << sample
                << '\n';
      return 8;
    }
    maximum_objects = std::max(maximum_objects, render.objects.size());
    posed_actors =
        std::max(posed_actors, static_cast<std::size_t>(std::ranges::count_if(
                                   render.objects, [](const auto &object) {
                                     return object.resident &&
                                            object.bone_matrix_count != 0U;
                                   })));
    previous_sequence = frame->sequence;

    if (sample != frame_count) {
      runtime->setHostPadState({});
      runtime->advanceHostUpdate();
      if (runtime->faulted()) {
        std::cerr << "H4 presentation runtime fault at sample " << sample
                  << '\n';
        return 9;
      }
    }
  }

  if (maximum_objects == 0U || posed_actors == 0U) {
    std::cerr << "H4 ROM gate observed no renderer objects/guest bone poses\n";
    return 10;
  }

  // Probe the raw retail input boundary before GameplaySession translates
  // native controls. This deliberately exercises the same 20 Hz outer-frame
  // tick and reports both the processed PAD sample and the collision root.
  // It distinguishes a missing host binding from a button combination that
  // retail itself does not accept in its L1 presentation state.
  for (std::uint32_t update = 0U;
       update < 1'000U && !runtime->openingFinished(); ++update) {
    runtime->setHostPadState({});
    runtime->advanceHostUpdate();
    if (runtime->faulted()) {
      std::cerr << "H4 raw PAD probe faulted during the opening rail\n";
      return 42;
    }
  }
  if (!runtime->openingFinished() || !runtime->captureCheckpoint()) {
    std::cerr << "H4 raw PAD probe could not capture a playable checkpoint\n";
    return 43;
  }
  for (std::uint32_t settle = 0U; settle < 64U; ++settle) {
    runtime->setHostPadState({});
    runtime->advanceHostUpdate();
  }
  if (runtime->faulted() || !runtime->captureCheckpoint()) {
    std::cerr << "H4 raw PAD probe could not settle the playable checkpoint\n";
    return 43;
  }
  const auto run_raw_pad = [&runtime](std::string_view label,
                                      sf::game::LegacyHostPadState pad) {
    const auto *before = runtime->bridge();
    const auto start = before != nullptr ? before->player.position
                                         : sf::game::LegacyNativePoint{};
    const auto camera_start = before != nullptr
                                  ? before->camera
                                  : sf::game::LegacyCameraBridgeState{};
    for (std::uint32_t tick = 0U; tick < 8U; ++tick) {
      runtime->setHostPadState(pad);
      runtime->advanceHostUpdate();
    }
    const auto *after = runtime->bridge();
    if (after == nullptr) {
      std::cerr << "H4 raw PAD " << label << ": bridge unavailable\n";
      return;
    }
    std::cerr << "H4 raw PAD " << label << ": processed=0x" << std::hex
              << after->pad.buttons << std::dec << " axes=("
              << static_cast<unsigned>(after->pad.left_x) << ','
              << static_cast<unsigned>(after->pad.left_y) << ") root=("
              << start.x << ',' << start.y << ',' << start.z << ")->("
              << after->player.position.x << ',' << after->player.position.y
              << ',' << after->player.position.z << ") camera-eye=("
              << camera_start.eye.x << ',' << camera_start.eye.y << ','
              << camera_start.eye.z << ")->(" << after->camera.eye.x << ','
              << after->camera.eye.y << ',' << after->camera.eye.z
              << ") target=(" << camera_start.target.x << ','
              << camera_start.target.y << ',' << camera_start.target.z << ")->("
              << after->camera.target.x << ',' << after->camera.target.y << ','
              << after->camera.target.z << ")\n";
  };
  const auto raw_scenario = [&runtime,
                             &run_raw_pad](std::string_view label,
                                           sf::game::LegacyHostPadState pad) {
    if (!runtime->restoreCheckpoint()) {
      std::cerr << "H4 raw PAD " << label << ": restore failed\n";
      return false;
    }
    run_raw_pad(label, pad);
    return !runtime->faulted();
  };
  constexpr std::uint16_t raw_l2 = 0x0100U;
  constexpr std::uint16_t raw_r2 = 0x0200U;
  constexpr std::uint16_t raw_l1 = 0x0400U;
  const auto raw_aim_direction = [&runtime](std::string_view label,
                                            std::uint8_t left_x,
                                            std::uint8_t left_y) {
    if (!runtime->restoreCheckpoint()) {
      return false;
    }
    for (std::uint32_t tick = 0U; tick < 24U; ++tick) {
      runtime->setHostPadState(
          {.buttons = raw_l1, .left_x = 0x80U, .left_y = 0x80U});
      runtime->advanceHostUpdate();
    }
    const auto *before = runtime->bridge();
    if (before == nullptr) {
      return false;
    }
    const auto camera_start = before->camera;
    for (std::uint32_t tick = 0U; tick < 64U; ++tick) {
      runtime->setHostPadState(
          {.buttons = raw_l1, .left_x = left_x, .left_y = left_y});
      runtime->advanceHostUpdate();
    }
    const auto *after = runtime->bridge();
    if (after == nullptr) {
      return false;
    }
    const auto heading = [](const sf::game::LegacyCameraBridgeState &camera) {
      return sf::game::headingFromDirection(
          static_cast<double>(camera.target.x - camera.eye.x),
          static_cast<double>(camera.target.z - camera.eye.z));
    };
    const auto pitch = [](const sf::game::LegacyCameraBridgeState &camera) {
      const auto x = static_cast<double>(camera.target.x - camera.eye.x);
      const auto y = static_cast<double>(camera.target.y - camera.eye.y);
      const auto z = static_cast<double>(camera.target.z - camera.eye.z);
      constexpr auto units_per_radian =
          static_cast<double>(sf::game::heading_angle_units) /
          (2.0 * std::numbers::pi);
      return std::atan2(y, std::hypot(x, z)) * units_per_radian;
    };
    auto heading_delta = sf::game::normalizeHeading(
        static_cast<std::int64_t>(heading(after->camera)) -
        heading(camera_start) + sf::game::heading_angle_units / 2);
    heading_delta -= sf::game::heading_angle_units / 2;
    std::cerr << "H4 raw aim " << label << ": heading=" << heading(camera_start)
              << "->" << heading(after->camera) << " delta=" << heading_delta
              << " pitch=" << pitch(camera_start) << "->"
              << pitch(after->camera)
              << " delta=" << pitch(after->camera) - pitch(camera_start)
              << '\n';
    return !runtime->faulted();
  };
  if (!raw_scenario("chase analog-left", {.left_x = 0x00U}) ||
      !raw_scenario("chase analog-right", {.left_x = 0xffU}) ||
      !raw_scenario("L2 neutral", {.buttons = raw_l2}) ||
      !raw_scenario("R2 neutral", {.buttons = raw_r2}) ||
      !raw_scenario("L1 neutral", {.buttons = raw_l1}) ||
      !raw_scenario("L1+L2 neutral", {.buttons = raw_l1 | raw_l2}) ||
      !raw_scenario("L1+R2 neutral", {.buttons = raw_l1 | raw_r2}) ||
      !raw_scenario("L1 analog-left", {.buttons = raw_l1, .left_x = 0x00U}) ||
      !raw_scenario("L1 analog-right", {.buttons = raw_l1, .left_x = 0xffU}) ||
      !raw_scenario("L1+L2 analog-left",
                    {.buttons = raw_l1 | raw_l2, .left_x = 0x00U}) ||
      !raw_scenario("L1+R2 analog-right",
                    {.buttons = raw_l1 | raw_r2, .left_x = 0xffU}) ||
      !raw_aim_direction("left", 0x00U, 0x80U) ||
      !raw_aim_direction("right", 0xffU, 0x80U) ||
      !raw_aim_direction("up", 0x80U, 0x00U) ||
      !raw_aim_direction("down", 0x80U, 0xffU)) {
    std::cerr << "H4 raw PAD scenario faulted\n";
    return 44;
  }
  runtime.reset();

  // Destroyed Subway begins directly in retail gameplay and contains the
  // early vending-machine/ledge traversal. Drive only original PAD inputs
  // and report the exact player display/controller changes; no host pose is
  // written into this VM.
  {
    const auto traversal_mission = sf::game::MissionPackage::load(disc, 1U);
    auto traversal = std::make_unique<sf::game::LegacyFirstMissionRuntime>(
        traversal_mission.definition(), traversal_mission.legacyImage());
    if (!traversal->ready() || traversal->faulted()) {
      std::cerr << "H4 traversal diagnostic bootstrap failed\n";
      return 45;
    }
    auto stable_control = std::uint32_t{};
    for (std::uint32_t settle = 0U; settle < 700U && stable_control < 4U;
         ++settle) {
      traversal->setHostPadState({});
      traversal->advanceHostUpdate();
      const auto *bridge = traversal->bridge();
      stable_control = bridge != nullptr && bridge->player.resident &&
                               !bridge->player.control_locked &&
                               !bridge->camera.scripted &&
                               !bridge->camera.locked
                           ? stable_control + 1U
                           : 0U;
    }
    if (stable_control < 4U || !traversal->captureCheckpoint()) {
      std::cerr << "H4 traversal diagnostic never reached stable control\n";
      return 45;
    }
    struct TraversalSample {
      std::uint32_t root_node{};
      std::uint32_t display_node{};
      std::uint32_t pose_flags{};
      std::uint32_t presentation_controller{};
      std::uint8_t presentation_enabled{};
      std::uint8_t presentation_mode{};
      std::uint8_t bone_count{};
      bool resident{};
      bool control_locked{};

      [[nodiscard]] bool operator==(const TraversalSample &) const = default;
    };
    auto traversal_edges = std::uint32_t{};
    auto exact_climb_pose_seen = false;
    auto exact_hang_pose_seen = false;
    constexpr std::array<std::string_view, 5U> traversal_scenarios{
        "triangle-forward", "cross-forward", "square-forward", "triangle-left",
        "triangle-right"};
    for (std::size_t scenario = 0U; scenario < traversal_scenarios.size();
         ++scenario) {
      if (!traversal->restoreCheckpoint()) {
        std::cerr << "H4 traversal diagnostic checkpoint restore failed\n";
        return 46;
      }
      auto previous = std::optional<TraversalSample>{};
      for (std::uint32_t tick = 0U; tick < 240U; ++tick) {
        auto pad = sf::game::LegacyHostPadState{};
        pad.left_y = 0x00U;
        if (scenario >= 3U && tick < 28U) {
          pad.left_y = 0x80U;
          pad.left_x = scenario == 3U ? 0x00U : 0xffU;
        }
        if (scenario == 0U || scenario >= 3U) {
          pad.buttons = tick % 20U < 3U ? 0x1000U : 0U;
        } else if (scenario == 1U) {
          pad.buttons = 0x4000U;
        } else {
          pad.buttons = tick % 20U < 3U ? 0x8000U : 0U;
        }
        traversal->setHostPadState(pad);
        traversal->advanceHostUpdate();
        const auto *frame = traversal->presentationFrame().get();
        if (traversal->faulted() || frame == nullptr || !frame->renderer ||
            !frame->ui) {
          std::cerr << "H4 traversal diagnostic faulted at tick=" << tick
                    << '\n';
          return 46;
        }
        const auto &render = frame->renderer->state;
        const auto slot = frame->ui->mission.player_slot;
        if (slot < 0 ||
            static_cast<std::size_t>(slot) >= render.objects.size()) {
          std::cerr << "H4 traversal diagnostic lost player slot\n";
          return 47;
        }
        const auto &player = render.objects[static_cast<std::size_t>(slot)];
        const auto current = TraversalSample{
            player.root_node,
            player.display_node,
            player.pose_flags,
            player.presentation_controller,
            player.presentation_enabled,
            player.presentation_mode,
            player.bone_matrix_count,
            player.resident,
            render.player.control_locked,
        };
        exact_climb_pose_seen =
            exact_climb_pose_seen ||
            (player.presentation_enabled == 10U &&
             player.presentation_mode == 34U && player.resident &&
             player.bone_matrix_count == 15U && !render.player.control_locked);
        exact_hang_pose_seen =
            exact_hang_pose_seen ||
            (player.presentation_enabled == 5U &&
             player.presentation_mode == 18U && render.camera.mode == 1 &&
             !render.camera.scripted && !render.camera.locked &&
             player.resident && player.bone_matrix_count == 15U &&
             !render.player.control_locked);
        if (!previous || current != *previous) {
          ++traversal_edges;
          auto bone_min = sf::game::LegacyNativePoint{
              std::numeric_limits<std::int32_t>::max(),
              std::numeric_limits<std::int32_t>::max(),
              std::numeric_limits<std::int32_t>::max()};
          auto bone_max = sf::game::LegacyNativePoint{
              std::numeric_limits<std::int32_t>::min(),
              std::numeric_limits<std::int32_t>::min(),
              std::numeric_limits<std::int32_t>::min()};
          for (std::size_t bone = 0U; bone < player.bone_matrix_count; ++bone) {
            const auto &translation = player.bone_matrices[bone].translation;
            bone_min.x = std::min(bone_min.x, translation.x);
            bone_min.y = std::min(bone_min.y, translation.y);
            bone_min.z = std::min(bone_min.z, translation.z);
            bone_max.x = std::max(bone_max.x, translation.x);
            bone_max.y = std::max(bone_max.y, translation.y);
            bone_max.z = std::max(bone_max.z, translation.z);
          }
          std::cerr << "H4 traversal sample: scenario="
                    << traversal_scenarios[scenario] << " tick=" << tick
                    << " slot=" << slot << " bits=0x" << std::hex
                    << render.pad.buttons << " flags=0x" << player.pose_flags
                    << " root-node=0x" << player.root_node << " display=0x"
                    << player.display_node << " controller=0x"
                    << player.presentation_controller << std::dec
                    << " presentation="
                    << static_cast<unsigned>(player.presentation_enabled) << '/'
                    << static_cast<unsigned>(player.presentation_mode)
                    << " resident=" << player.resident << " bones="
                    << static_cast<unsigned>(player.bone_matrix_count)
                    << " lock=" << render.player.control_locked << " root=("
                    << render.player.position.x << ','
                    << render.player.position.y << ','
                    << render.player.position.z << ") display-root=("
                    << player.position.x << ',' << player.position.y << ','
                    << player.position.z << ") bone-range=(" << bone_min.x
                    << ',' << bone_min.y << ',' << bone_min.z << ")->("
                    << bone_max.x << ',' << bone_max.y << ',' << bone_max.z
                    << ") camera=" << render.camera.mode << '/'
                    << render.camera.scripted << '/' << render.camera.locked
                    << " eye=(" << render.camera.eye.x << ','
                    << render.camera.eye.y << ',' << render.camera.eye.z
                    << ") target=(" << render.camera.target.x << ','
                    << render.camera.target.y << ',' << render.camera.target.z
                    << ")\n";
          previous = current;
        }
      }
    }
    if (!exact_climb_pose_seen || !exact_hang_pose_seen) {
      std::cerr << "H4 traversal diagnostic missed exact retail poses: climb="
                << exact_climb_pose_seen << " hang=" << exact_hang_pose_seen
                << '\n';
      return 48;
    }
    std::cerr << "H4 traversal diagnostic edges=" << traversal_edges << '\n';
  }

  // Exercise the shipping GameplaySession seam after the opening rail. This
  // catches regressions that isolated SDL, HUD and VM tests cannot see.
  auto gameplay = std::make_unique<sf::game::GameplaySession>(mission);
  constexpr std::uint32_t control_settle_updates = 1'000U;
  auto opening_finished = false;
  auto opening_npc_combat_seen = false;
  for (std::uint32_t update = 0U; update < control_settle_updates; ++update) {
    gameplay->update({});
    gameplay->advanceAnimationClock();
    if (gameplay->runtimeFaulted()) {
      std::cerr << "H4 control gate faulted while waiting for opening rail: "
                << gameplay->runtimeFaultReason() << " update=" << update
                << '\n';
      return 11;
    }
    if (const auto frame = gameplay->legacyPresentationFrame();
        frame && frame->renderer) {
      opening_npc_combat_seen =
          opening_npc_combat_seen ||
          std::ranges::any_of(frame->renderer->state.combat_particles,
                              [](const auto &particle) {
                                return particle.kind ==
                                       sf::game::LegacyCombatParticleKind::
                                           blood_impact_triangle;
                              });
    }
    if (gameplay->legacyOpeningFinished()) {
      opening_finished = true;
      break;
    }
  }
  if (!opening_finished) {
    std::cerr << "H4 control gate never finished the opening rail\n";
    return 37;
  }
  if (!opening_npc_combat_seen) {
    std::cerr << "H4 full-start gate never crossed mission 0's opening NPC "
                 "combat event\n";
    return 40;
  }

  const auto player_before_aim = gameplay->player();
  auto aim_ready = false;
  for (std::uint32_t update = 0U; update < 64U; ++update) {
    gameplay->update(sf::game::GameplayInput{.aim = true});
    gameplay->advanceAnimationClock();
    if (gameplay->player().yaw != player_before_aim.yaw) {
      std::cerr << "H4 aim entry rotated Gabe for one guest frame\n";
      return 39;
    }
    if (gameplay->runtimeFaulted()) {
      std::cerr << "H4 control gate faulted while waiting for manual aim\n";
      return 11;
    }
    if (gameplay->playerAim() == sf::game::PlayerAimState::first_person &&
        gameplay->hud().aiming()) {
      aim_ready = true;
      break;
    }
  }
  if (!aim_ready) {
    std::cerr << "H4 control gate never entered coherent first-person aim\n";
    return 12;
  }

  const auto aim_origin = gameplay->player();
  if (std::abs(aim_origin.x - player_before_aim.x) > 0.001 ||
      std::abs(aim_origin.y - player_before_aim.y) > 2.0 ||
      std::abs(aim_origin.z - player_before_aim.z) > 0.001 ||
      aim_origin.yaw != player_before_aim.yaw ||
      aim_origin.grounded != player_before_aim.grounded) {
    std::cerr
        << "H4 entering first-person changed Gabe's collision root: before=("
        << player_before_aim.x << ',' << player_before_aim.y << ','
        << player_before_aim.z << ") after=(" << aim_origin.x << ','
        << aim_origin.y << ',' << aim_origin.z << ")\n";
    return 36;
  }
  const auto aim_camera_origin = gameplay->camera();
  auto horizontal_sight_moved = false;
  auto vertical_sight_moved = false;
  for (std::uint32_t update = 0U; update < 32U; ++update) {
    const auto phase = update % 4U;
    const auto move = phase == 0U ? 1.0 : phase == 1U ? -1.0 : 0.0;
    const auto turn = phase == 2U ? 1.0 : phase == 3U ? -1.0 : 0.0;
    gameplay->update(sf::game::GameplayInput{
        .move = move,
        .turn = turn,
        .aim = true,
        .look_yaw = turn * sf::platform::retail_first_person_yaw_units_per_tick,
        .look_pitch =
            -move * sf::platform::retail_first_person_pitch_units_per_tick,
    });
    gameplay->advanceAnimationClock();
    if (gameplay->runtimeFaulted() ||
        gameplay->playerAim() != sf::game::PlayerAimState::first_person) {
      std::cerr << "H4 four-way aim left first-person or faulted\n";
      return 35;
    }
    const auto player = gameplay->player();
    const auto camera = gameplay->camera();
    horizontal_sight_moved =
        horizontal_sight_moved ||
        std::abs((camera.target_x - camera.x) -
                 (aim_camera_origin.target_x - aim_camera_origin.x)) > 0.001 ||
        std::abs((camera.target_z - camera.z) -
                 (aim_camera_origin.target_z - aim_camera_origin.z)) > 0.001;
    vertical_sight_moved =
        vertical_sight_moved ||
        std::abs((camera.target_y - camera.y) -
                 (aim_camera_origin.target_y - aim_camera_origin.y)) > 0.001;
    if (std::abs(player.x - aim_origin.x) > 0.001 ||
        std::abs(player.y - aim_origin.y) > 64.0 ||
        std::abs(player.z - aim_origin.z) > 0.001 ||
        player.yaw != aim_origin.yaw ||
        player.grounded != aim_origin.grounded) {
      std::cerr << "H4 WASD moved Gabe's root during four-way aim: origin=("
                << aim_origin.x << ',' << aim_origin.y << ',' << aim_origin.z
                << ") current=(" << player.x << ',' << player.y << ','
                << player.z << ")\n";
      return 36;
    }
  }
  if (!horizontal_sight_moved || !vertical_sight_moved) {
    std::cerr << "H4 four-way aim missed an axis: horizontal="
              << horizontal_sight_moved << " vertical=" << vertical_sight_moved
              << '\n';
    return 36;
  }

  const auto strafe_origin = gameplay->player();
  const auto strafe_camera_origin = gameplay->camera();
  for (std::uint32_t update = 0U; update < 8U; ++update) {
    gameplay->update(sf::game::GameplayInput{.aim = true, .strafe = -1.0});
    gameplay->advanceAnimationClock();
  }
  const auto strafe_left = gameplay->player();
  const auto strafe_left_camera = gameplay->camera();
  const auto left_frame = gameplay->legacyPresentationFrame();
  for (std::uint32_t update = 0U; update < 8U; ++update) {
    gameplay->update(sf::game::GameplayInput{.aim = true});
    gameplay->advanceAnimationClock();
  }
  for (std::uint32_t update = 0U; update < 8U; ++update) {
    gameplay->update(sf::game::GameplayInput{.aim = true, .strafe = 1.0});
    gameplay->advanceAnimationClock();
  }
  const auto strafe_right = gameplay->player();
  const auto strafe_right_camera = gameplay->camera();
  const auto right_frame = gameplay->legacyPresentationFrame();
  std::cerr << "H4 aim-strafe diagnostic: left root=(" << strafe_origin.x << ','
            << strafe_origin.y << ',' << strafe_origin.z << ")->("
            << strafe_left.x << ',' << strafe_left.y << ',' << strafe_left.z
            << ") right->(" << strafe_right.x << ',' << strafe_right.y << ','
            << strafe_right.z << ")\n";
  const auto dump_pad = [](std::string_view label, const auto &frame) {
    if (!frame || !frame->renderer || !frame->ui) {
      return;
    }
    const auto &state = frame->renderer->state;
    const auto slot = frame->ui->mission.player_slot;
    std::cerr << "H4 aim-strafe PAD " << label << ": bits=0x" << std::hex
              << state.pad.buttons << std::dec << " axes=("
              << static_cast<unsigned>(state.pad.left_x) << ','
              << static_cast<unsigned>(state.pad.left_y) << ")";
    if (slot >= 0 && static_cast<std::size_t>(slot) < state.objects.size()) {
      const auto &player = state.objects[static_cast<std::size_t>(slot)];
      std::cerr << " slot=" << slot << " flags=0x" << std::hex
                << player.pose_flags << " handler=0x" << player.object_handler
                << " motion=0x" << player.motion_controller
                << " presentation=0x" << player.presentation_controller
                << std::dec << ':'
                << static_cast<unsigned>(player.presentation_enabled) << '/'
                << static_cast<unsigned>(player.presentation_mode)
                << " bones=" << static_cast<unsigned>(player.bone_matrix_count)
                << " lock=" << state.player.control_locked << " root=("
                << state.player.position.x << ',' << state.player.position.y
                << ',' << state.player.position.z << ')';
    }
    std::cerr << '\n';
  };
  dump_pad("L1+L2 peek", left_frame);
  dump_pad("L1+R2 peek", right_frame);
  const auto sight_direction_matches = [](const auto &first,
                                          const auto &second) {
    return std::abs((first.target_x - first.x) -
                    (second.target_x - second.x)) <= 1.0 &&
           std::abs((first.target_y - first.y) -
                    (second.target_y - second.y)) <= 1.0 &&
           std::abs((first.target_z - first.z) -
                    (second.target_z - second.z)) <= 1.0;
  };
  const auto root_stayed = [](const auto &first, const auto &second) {
    return std::abs(first.x - second.x) <= 0.001 &&
           std::abs(first.y - second.y) <= 2.0 &&
           std::abs(first.z - second.z) <= 0.001;
  };
  const auto pad_matches = [](const auto &frame, std::uint16_t buttons) {
    return frame && frame->renderer &&
           frame->renderer->state.pad.buttons == buttons;
  };
  if (!root_stayed(strafe_origin, strafe_left) ||
      !root_stayed(strafe_origin, strafe_right) ||
      !pad_matches(left_frame, 0x0005U) || !pad_matches(right_frame, 0x0006U) ||
      !sight_direction_matches(strafe_camera_origin, strafe_left_camera) ||
      !sight_direction_matches(strafe_camera_origin, strafe_right_camera) ||
      std::hypot(strafe_left_camera.x - strafe_right_camera.x,
                 strafe_left_camera.z - strafe_right_camera.z) < 80.0) {
    std::cerr << "H4 retail L1+L2/R2 corner peek contract failed\n";
    return 41;
  }

  gameplay->update(sf::game::GameplayInput{
      .aim = true,
      .look_yaw = 192.0,
      .look_pitch = -128.0,
  });
  const auto moved_aim_camera = gameplay->camera();
  const auto moved_aim_player = gameplay->player();
  const auto mouse_aim_origin = strafe_right;
  const auto sight_moved =
      std::abs((moved_aim_camera.target_x - moved_aim_camera.x) -
               (aim_camera_origin.target_x - aim_camera_origin.x)) > 1.0 ||
      std::abs((moved_aim_camera.target_y - moved_aim_camera.y) -
               (aim_camera_origin.target_y - aim_camera_origin.y)) > 1.0 ||
      std::abs((moved_aim_camera.target_z - moved_aim_camera.z) -
               (aim_camera_origin.target_z - aim_camera_origin.z)) > 1.0;
  if (!sight_moved) {
    std::cerr << "H4 native mouse aim did not move freely\n";
    return 36;
  }
  if (moved_aim_player.yaw != mouse_aim_origin.yaw ||
      std::abs(moved_aim_player.x - mouse_aim_origin.x) > 0.001 ||
      std::abs(moved_aim_player.y - mouse_aim_origin.y) > 2.0 ||
      std::abs(moved_aim_player.z - mouse_aim_origin.z) > 0.001) {
    std::cerr << "H4 mouse sight rotation changed Gabe's body transform\n";
    return 39;
  }
  const auto aim_patch_count_before_fire = gameplay->legacyAimRayPatchCount();
  const auto fire_weapon = gameplay->hud().inventory().current();
  const auto fire_ammo_before = gameplay->hud().inventory().currentState();
  auto manual_shot_event_seen = false;
  for (std::uint32_t update = 0U; update < 16U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .aim = true,
        .fire_pressed = update == 0U,
        .fire_held = update == 0U,
    });
    if (const auto frame = gameplay->legacyPresentationFrame();
        frame && frame->renderer) {
      manual_shot_event_seen =
          manual_shot_event_seen ||
          std::ranges::any_of(
              frame->renderer->state.weapon_events, [](const auto &event) {
                return event.type == sf::game::LegacyWeaponEventType::shot;
              });
    }
    if (gameplay->player().yaw != aim_origin.yaw) {
      std::cerr << "H4 neutral/fire aim rotated Gabe for one guest frame\n";
      return 39;
    }
  }
  const auto fire_ammo_after = gameplay->hud().inventory().currentState();
  if (gameplay->legacyAimRayPatchCount() <= aim_patch_count_before_fire ||
      !manual_shot_event_seen ||
      fire_ammo_after.magazine + 1U != fire_ammo_before.magazine) {
    std::cerr << "H4 retail fire path did not consume the native sight ray, "
                 "publish its accepted shot, and spend one round"
              << ": weapon=" << static_cast<unsigned int>(fire_weapon)
              << ", ammo=" << fire_ammo_before.magazine << "->"
              << fire_ammo_after.magazine
              << ", reserve=" << fire_ammo_before.reserve << "->"
              << fire_ammo_after.reserve << '\n';
    return 40;
  }
  const auto held_aim_camera = gameplay->camera();
  const auto held_aim_player = gameplay->player();
  if (std::abs((held_aim_camera.target_x - held_aim_camera.x) -
               (moved_aim_camera.target_x - moved_aim_camera.x)) > 0.001 ||
      std::abs((held_aim_camera.target_y - held_aim_camera.y) -
               (moved_aim_camera.target_y - moved_aim_camera.y)) > 0.001 ||
      std::abs((held_aim_camera.target_z - held_aim_camera.z) -
               (moved_aim_camera.target_z - moved_aim_camera.z)) > 0.001) {
    std::cerr << "H4 native mouse aim recentered on neutral/fire frames\n";
    return 36;
  }
  if (held_aim_player.yaw != aim_origin.yaw ||
      std::abs(held_aim_player.x - aim_origin.x) > 0.001 ||
      std::abs(held_aim_player.y - aim_origin.y) > 64.0 ||
      std::abs(held_aim_player.z - aim_origin.z) > 0.001) {
    std::cerr << "H4 neutral/fire aim changed Gabe's body transform\n";
    return 39;
  }

  auto chase_restored = false;
  for (std::uint32_t update = 0U; update < 32U; ++update) {
    gameplay->update({});
    if (gameplay->player().yaw != aim_origin.yaw) {
      std::cerr << "H4 aim release rotated Gabe for one guest frame\n";
      return 39;
    }
    if (gameplay->playerAim() == sf::game::PlayerAimState::chase &&
        !gameplay->hud().aiming()) {
      chase_restored = true;
      break;
    }
  }
  if (!chase_restored) {
    std::cerr << "H4 control gate did not leave first-person aim coherently\n";
    return 13;
  }
  gameplay->update({});
  if (gameplay->runtimeFaulted() || gameplay->player().yaw != aim_origin.yaw) {
    std::cerr << "H4 post-release guest frame restored the old PS1 aim yaw\n";
    return 39;
  }
  const auto released_player = gameplay->player();
  if (std::abs(released_player.x - aim_origin.x) > 0.001 ||
      std::abs(released_player.z - aim_origin.z) > 0.001 ||
      std::abs(released_player.y - aim_origin.y) > 128.0 ||
      released_player.yaw != aim_origin.yaw ||
      (aim_origin.grounded && !released_player.grounded)) {
    std::cerr << "H4 first-person exit left the collision envelope: origin=("
              << aim_origin.x << ',' << aim_origin.y << ',' << aim_origin.z
              << ") released=(" << released_player.x << ',' << released_player.y
              << ',' << released_player.z << ")\n";
    return 38;
  }
  const auto released_yaw = released_player.yaw;
  auto chase_turn_restored = false;
  for (std::uint32_t update = 0U; update < 32U; ++update) {
    gameplay->update(sf::game::GameplayInput{.turn = 1.0});
    chase_turn_restored = gameplay->player().yaw != released_yaw;
    if (chase_turn_restored) {
      break;
    }
  }
  if (!chase_turn_restored) {
    std::cerr << "H4 chase controls did not resume after mouse aim\n";
    return 38;
  }

  const auto quick_before = gameplay->hud().inventory().current();
  const auto quick_expected =
      gameplay->hud().inventory().previousAvailable(quick_before);
  auto quick_changed = false;
  for (std::uint32_t update = 0U; update < 96U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .quick_weapon = update == 0U,
        .strafe = update == 0U ? 1.0 : 0.0,
    });
    if (gameplay->hud().weaponMenuFrames() != 0U) {
      std::cerr << "H4 quick weapon tap incorrectly opened the weapon tape\n";
      return 14;
    }
    if (gameplay->hud().inventory().current() != quick_before) {
      quick_changed = gameplay->hud().inventory().current() == quick_expected &&
                      gameplay->hud().weaponSwitchFrames() != 0U;
      break;
    }
  }
  if (!quick_changed) {
    std::cerr << "H4 quick weapon tap did not select the retail previous weapon"
              << ": before=" << static_cast<int>(quick_before)
              << ", expected=" << static_cast<int>(quick_expected)
              << ", actual="
              << static_cast<int>(gameplay->hud().inventory().current())
              << ", menu-state="
              << gameplay->legacyWeaponMenuState().value_or(999)
              << ", dirty=" << gameplay->legacyWeaponMenuDirty()
              << ", ready=" << gameplay->legacyWeaponMenuReady()
              << ", menu-frames=" << gameplay->hud().weaponMenuFrames() << '\n';
    return 15;
  }

  auto cycle_last_state = std::int32_t{999};
  auto cycle_last_dirty = false;
  auto cycle_last_ready = false;
  auto cycle_last_weapon = gameplay->hud().inventory().current();
  const auto cycle_weapon = [&](std::int32_t direction,
                                sf::game::WeaponId expected) {
    auto menu_seen = false;
    for (std::uint32_t update = 0U; update < 128U; ++update) {
      gameplay->update(sf::game::GameplayInput{
          .weapon_menu_delta = update == 0U ? direction : 0,
      });
      cycle_last_state = gameplay->legacyWeaponMenuState().value_or(999);
      cycle_last_dirty = gameplay->legacyWeaponMenuDirty();
      cycle_last_ready = gameplay->legacyWeaponMenuReady();
      cycle_last_weapon = gameplay->hud().inventory().current();
      menu_seen = menu_seen || gameplay->hud().weaponMenuFrames() != 0U;
      if (gameplay->hud().inventory().current() == expected) {
        return menu_seen && gameplay->hud().weaponSwitchFrames() != 0U &&
               gameplay->hud().weaponMenuWindow()[3] == expected;
      }
    }
    return false;
  };
  const auto next_expected = gameplay->hud().inventory().nextAvailable(
      gameplay->hud().inventory().current());
  if (!cycle_weapon(1, next_expected)) {
    std::cerr << "H4 positive wheel cycle/tape contract failed: phase="
              << cycle_last_state << ", dirty=" << cycle_last_dirty
              << ", ready=" << cycle_last_ready
              << ", weapon=" << static_cast<int>(cycle_last_weapon)
              << ", expected=" << static_cast<int>(next_expected) << '\n';
    return 16;
  }
  const auto previous_expected = gameplay->hud().inventory().previousAvailable(
      gameplay->hud().inventory().current());
  if (!cycle_weapon(-1, previous_expected)) {
    std::cerr << "H4 negative wheel cycle/tape contract failed\n";
    return 17;
  }

  for (std::uint32_t update = 0U; update < 32U; ++update) {
    gameplay->update({});
  }
  const auto burst_start = gameplay->hud().inventory().current();
  auto burst_inventory = gameplay->hud().inventory();
  static_cast<void>(burst_inventory.selectNext());
  static_cast<void>(burst_inventory.selectNext());
  static_cast<void>(burst_inventory.selectPrevious());
  const auto burst_expected = burst_inventory.current();
  auto burst_passed = false;
  auto burst_menu_seen = false;
  for (std::uint32_t update = 0U; update < 128U; ++update) {
    const auto delta = update < 2U ? 1 : (update == 2U ? -1 : 0);
    gameplay->update(sf::game::GameplayInput{.weapon_menu_delta = delta});
    burst_menu_seen =
        burst_menu_seen || gameplay->hud().weaponMenuFrames() != 0U;
    if (update >= 2U &&
        gameplay->hud().inventory().current() == burst_expected) {
      burst_passed = burst_menu_seen &&
                     gameplay->hud().weaponSwitchFrames() != 0U &&
                     gameplay->hud().weaponMenuWindow()[3] == burst_expected;
      break;
    }
  }
  if (!burst_passed) {
    std::cerr << "H4 continuous/reversed wheel burst contract failed: actual="
              << static_cast<int>(gameplay->hud().inventory().current())
              << ", expected=" << static_cast<int>(burst_expected)
              << ", start=" << static_cast<int>(burst_start)
              << ", menu-seen=" << burst_menu_seen << ", menu-state="
              << gameplay->legacyWeaponMenuState().value_or(999)
              << ", dirty=" << gameplay->legacyWeaponMenuDirty()
              << ", ready=" << gameplay->legacyWeaponMenuReady() << '\n';
    return 18;
  }

  for (std::uint32_t update = 0U; update < 32U; ++update) {
    gameplay->update({});
  }
  auto multi_inventory = gameplay->hud().inventory();
  std::array<sf::game::WeaponId, 3U> multi_expected{};
  for (auto &expected : multi_expected) {
    static_cast<void>(multi_inventory.selectNext());
    expected = multi_inventory.current();
  }
  auto multi_observed = gameplay->hud().inventory().current();
  std::size_t multi_step = 0U;
  std::uint32_t multi_failure_update = 0U;
  auto multi_passed = false;
  std::optional<sf::game::WeaponId> multi_unexpected;
  std::array<std::int32_t, 12U> multi_menu_states{};
  for (std::uint32_t update = 0U; update < 192U; ++update) {
    if (update < multi_menu_states.size()) {
      multi_menu_states[update] =
          gameplay->legacyWeaponMenuState().value_or(999);
    }
    gameplay->update(sf::game::GameplayInput{
        .weapon_menu_delta = update == 0U ? 3 : 0,
    });
    const auto current = gameplay->hud().inventory().current();
    if (current == multi_observed) {
      continue;
    }
    if (multi_step >= multi_expected.size() ||
        current != multi_expected[multi_step]) {
      multi_unexpected = current;
      multi_failure_update = update;
      break;
    }
    multi_observed = current;
    ++multi_step;
    if (multi_step == multi_expected.size()) {
      multi_passed = gameplay->hud().weaponMenuFrames() != 0U &&
                     gameplay->hud().weaponSwitchFrames() != 0U &&
                     gameplay->hud().weaponMenuWindow()[3] == current;
      break;
    }
  }
  if (!multi_passed) {
    std::cerr << "H4 multi-notch wheel direction contract failed at step "
              << multi_step << ", start="
              << static_cast<int>(gameplay->hud().inventory().previousAvailable(
                     multi_expected[0]))
              << ", expected=" << static_cast<int>(multi_expected[multi_step])
              << ", sequence=" << static_cast<int>(multi_expected[0]) << '/'
              << static_cast<int>(multi_expected[1]) << '/'
              << static_cast<int>(multi_expected[2])
              << ", update=" << multi_failure_update;
    if (multi_unexpected) {
      std::cerr << ", current=" << static_cast<int>(*multi_unexpected);
    }
    std::cerr << ", menu=";
    for (std::size_t index = 0U; index < multi_menu_states.size(); ++index) {
      std::cerr << (index == 0U ? "" : "/") << multi_menu_states[index];
    }
    std::cerr << '\n';
    return 19;
  }

  for (std::uint32_t update = 0U; update < 32U; ++update) {
    gameplay->update({});
  }
  const auto net_zero_weapon = gameplay->hud().inventory().current();
  auto net_zero_menu_seen = false;
  for (std::uint32_t update = 0U; update < 128U; ++update) {
    const auto delta = update == 0U ? 1 : (update == 1U ? -1 : 0);
    gameplay->update(sf::game::GameplayInput{.weapon_menu_delta = delta});
    net_zero_menu_seen =
        net_zero_menu_seen || gameplay->hud().weaponMenuFrames() != 0U;
  }
  if (!net_zero_menu_seen ||
      gameplay->hud().inventory().current() != net_zero_weapon) {
    std::cerr << "H4 unacknowledged/reversed wheel contract failed\n";
    return 20;
  }

  const auto boundary_wheel_expected =
      gameplay->hud().inventory().nextAvailable(
          gameplay->hud().inventory().current());
  if (!cycle_weapon(1, boundary_wheel_expected)) {
    std::cerr << "H4 release-boundary wheel setup failed\n";
    return 21;
  }
  const auto boundary_quick_expected =
      gameplay->hud().inventory().previousAvailable(boundary_wheel_expected);
  auto boundary_quick_passed = false;
  for (std::uint32_t update = 0U; update < 192U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .quick_weapon = update == 0U,
    });
    if (gameplay->hud().inventory().current() == boundary_quick_expected &&
        gameplay->legacyWeaponMenuState() == -5) {
      boundary_quick_passed = true;
      break;
    }
  }
  if (!boundary_quick_passed) {
    std::cerr << "H4 release-boundary quick weapon contract failed\n";
    return 22;
  }

  const auto quick_wheel_start = gameplay->hud().inventory().current();
  auto quick_wheel_changed = false;
  auto quick_wheel_menu_seen = false;
  auto quick_wheel_passed = false;
  for (std::uint32_t update = 0U; update < 192U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .quick_weapon = update == 0U,
        .weapon_menu_delta = update == 1U ? 1 : 0,
    });
    quick_wheel_changed =
        quick_wheel_changed ||
        gameplay->hud().inventory().current() != quick_wheel_start;
    quick_wheel_menu_seen =
        quick_wheel_menu_seen || gameplay->hud().weaponMenuFrames() != 0U;
    if (quick_wheel_changed && quick_wheel_menu_seen &&
        gameplay->hud().inventory().current() == quick_wheel_start &&
        gameplay->legacyWeaponMenuState() == -5) {
      quick_wheel_passed = true;
      break;
    }
  }
  if (!quick_wheel_passed) {
    std::cerr << "H4 quick-then-wheel ordering contract failed\n";
    return 23;
  }

  const auto quick_direct_target = gameplay->hud().inventory().current();
  auto quick_direct_changed = false;
  auto quick_direct_passed = false;
  for (std::uint32_t update = 0U; update < 192U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .quick_weapon = update == 0U,
        .direct_weapon =
            update == 1U
                ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(
                      quick_direct_target)}
                : std::nullopt,
    });
    quick_direct_changed =
        quick_direct_changed ||
        gameplay->hud().inventory().current() != quick_direct_target;
    if (quick_direct_changed &&
        gameplay->hud().inventory().current() == quick_direct_target &&
        gameplay->legacyWeaponMenuState() == -5) {
      quick_direct_passed = true;
      break;
    }
  }
  if (!quick_direct_passed) {
    std::cerr << "H4 quick-then-direct ordering contract failed\n";
    return 24;
  }

  const auto direct_boundary_weapon =
      gameplay->hud().inventory().nextAvailable(quick_direct_target);
  if (!cycle_weapon(1, direct_boundary_weapon)) {
    std::cerr << "H4 direct release-boundary setup failed\n";
    return 25;
  }
  auto direct_boundary_passed = false;
  for (std::uint32_t update = 0U; update < 192U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .direct_weapon =
            update == 0U
                ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(
                      quick_direct_target)}
                : std::nullopt,
    });
    if (gameplay->hud().inventory().current() == quick_direct_target &&
        gameplay->legacyWeaponMenuState() == -5) {
      direct_boundary_passed = true;
      break;
    }
  }
  if (!direct_boundary_passed) {
    std::cerr << "H4 release-boundary direct weapon contract failed\n";
    return 26;
  }

  const auto sniper_state =
      gameplay->hud().inventory().tryState(sf::game::WeaponId::sniper_rifle);
  if (sniper_state == nullptr || !sniper_state->owned) {
    std::cerr << "H4 retail bootstrap did not expose the sniper rifle\n";
    return 27;
  }
  auto sniper_selected =
      gameplay->hud().inventory().current() == sf::game::WeaponId::sniper_rifle;
  for (std::uint32_t update = 0U; !sniper_selected && update < 256U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .direct_weapon =
            update == 0U
                ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(
                      sf::game::WeaponId::sniper_rifle)}
                : std::nullopt,
    });
    if (gameplay->runtimeFaulted()) {
      std::cerr << "H4 sniper selection faulted the retail runtime\n";
      return 28;
    }
    sniper_selected = gameplay->hud().inventory().current() ==
                          sf::game::WeaponId::sniper_rifle &&
                      gameplay->legacyWeaponMenuState() == -5;
  }
  if (!sniper_selected) {
    std::cerr << "H4 direct sniper selection never completed\n";
    return 29;
  }

  const auto sniper_aim_patch_before = gameplay->legacyAimRayPatchCount();
  auto sniper_aim_ready = false;
  for (std::uint32_t update = 0U; update < 96U; ++update) {
    gameplay->update(sf::game::GameplayInput{.aim = true});
    if (gameplay->runtimeFaulted()) {
      std::cerr << "H4 sniper manual aim faulted the retail runtime\n";
      return 30;
    }
    if (gameplay->playerAim() == sf::game::PlayerAimState::first_person) {
      sniper_aim_ready = true;
      break;
    }
  }
  if (!sniper_aim_ready) {
    std::cerr << "H4 sniper never entered manual aim\n";
    return 31;
  }
  for (std::uint32_t update = 0U; update < 48U; ++update) {
    gameplay->update(sf::game::GameplayInput{.aim = true});
    if (gameplay->runtimeFaulted()) {
      std::cerr << "H4 sniper scope-open animation faulted\n";
      return 30;
    }
  }
  if (gameplay->legacyAimRayPatchCount() <= sniper_aim_patch_before) {
    std::cerr << "H4 sniper scope did not consume the native sight ray\n";
    return 30;
  }
  const auto sniper_fov_before =
      gameplay->legacyPresentationFrame()->renderer->state.camera.fov_raw;
  const auto sniper_zoom_before =
      gameplay->legacyPresentationFrame()->ui->mission.scope_zoom_raw;
  const auto sniper_projection_before =
      gameplay->legacyPresentationFrame()->renderer->state.camera.projection;
  for (std::uint32_t update = 0U; update < 24U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .aim = true,
        .interact = true,
    });
    if (gameplay->runtimeFaulted()) {
      std::cerr << "H4 sniper zoom-in faulted the retail runtime: reason="
                << gameplay->runtimeFaultReason()
                << " detail=" << gameplay->runtimeFaultDetail()
                << " update=" << update << '\n';
      return 30;
    }
  }
  const auto sniper_fov_after =
      gameplay->legacyPresentationFrame()->renderer->state.camera.fov_raw;
  const auto sniper_zoom_after =
      gameplay->legacyPresentationFrame()->ui->mission.scope_zoom_raw;
  const auto sniper_projection_after =
      gameplay->legacyPresentationFrame()->renderer->state.camera.projection;
  const auto sniper_pad_after =
      gameplay->legacyPresentationFrame()->renderer->state.pad.buttons;
  const auto sniper_interface_mode =
      gameplay->legacyPresentationFrame()->ui->mission.interface_mode;
  const auto sniper_aim_mode =
      gameplay->legacyPresentationFrame()->ui->mission.first_person_aim_mode;
  const auto sniper_scope_packets = std::ranges::count_if(
      gameplay->legacyPresentationFrame()->renderer->state.guest_raw_packets,
      &sf::game::legacyGuestRawPacketIsRetailScopeOverlay);
  const auto sniper_native_projection_after = gameplay->camera().projection;
  for (std::uint32_t update = 0U; update < 24U; ++update) {
    gameplay->update(sf::game::GameplayInput{
        .aim = true,
        .roll = true,
    });
    if (gameplay->runtimeFaulted()) {
      std::cerr << "H4 sniper zoom-out faulted the retail runtime: reason="
                << gameplay->runtimeFaultReason()
                << " detail=" << gameplay->runtimeFaultDetail()
                << " update=" << update << '\n';
      return 30;
    }
  }
  const auto sniper_zoom_out =
      gameplay->legacyPresentationFrame()->ui->mission.scope_zoom_raw;
  const auto sniper_native_projection_out = gameplay->camera().projection;
  std::cout << "H4 sniper zoom diagnostic: fov=" << sniper_fov_before << "->"
            << sniper_fov_after << " zoom=" << sniper_zoom_before << "->"
            << sniper_zoom_after << " projection=" << sniper_projection_before
            << "->" << sniper_projection_after << " pad=0x" << std::hex
            << sniper_pad_after << std::dec
            << " ui=" << static_cast<unsigned>(sniper_interface_mode)
            << " aim=" << static_cast<unsigned>(sniper_aim_mode)
            << " labels=" << gameplay->legacyUiMessages().size()
            << " scope-packets=" << sniper_scope_packets
            << " native-projection=" << sniper_native_projection_after << "->"
            << sniper_native_projection_out << " zoom-out=" << sniper_zoom_out
            << '\n';
  if (sniper_zoom_after >= sniper_zoom_before ||
      sniper_zoom_out <= sniper_zoom_after ||
      sniper_native_projection_after <= sniper_native_projection_out) {
    std::cerr << "H4 sniper retail zoom-in/zoom-out transition mismatch\n";
    return 30;
  }
  for (const auto &message : gameplay->legacyUiMessages()) {
    if (!message.text.empty()) {
      std::cout << "H4 sniper label: " << message.text
                << " glyphs=" << message.glyphs.size() << '\n';
    }
  }
  const auto aim_camera_before = gameplay->camera();
  const auto aim_pitch_slope = [](const sf::game::CameraState &camera) {
    const auto horizontal =
        std::hypot(camera.target_x - camera.x, camera.target_z - camera.z);
    return horizontal > 0.001 ? (camera.target_y - camera.y) / horizontal : 0.0;
  };
  const auto aim_pitch_before = aim_pitch_slope(aim_camera_before);
  gameplay->update(sf::game::GameplayInput{
      .aim = true,
      .look_yaw = 48.0,
      .look_pitch = -48.0,
  });
  auto sniper_aim_moved = false;
  auto minimum_aim_pitch = aim_pitch_before;
  auto maximum_aim_pitch = aim_pitch_before;
  auto horizontal_aim_moved = false;
  for (std::uint32_t update = 0U; update < 32U; ++update) {
    if (update != 0U) {
      gameplay->update(sf::game::GameplayInput{.aim = true});
    }
    if (gameplay->runtimeFaulted()) {
      std::cerr << "H4 sniper look input faulted the retail runtime\n";
      return 32;
    }
    const auto camera = gameplay->camera();
    const auto pitch = aim_pitch_slope(camera);
    minimum_aim_pitch = std::min(minimum_aim_pitch, pitch);
    maximum_aim_pitch = std::max(maximum_aim_pitch, pitch);
    horizontal_aim_moved =
        horizontal_aim_moved ||
        std::abs(camera.target_x - aim_camera_before.target_x) > 1.0 ||
        std::abs(camera.target_z - aim_camera_before.target_z) > 1.0;
    sniper_aim_moved =
        gameplay->playerAim() == sf::game::PlayerAimState::first_person &&
        minimum_aim_pitch < aim_pitch_before - 0.001 && horizontal_aim_moved;
    if (sniper_aim_moved) {
      break;
    }
  }
  if (!sniper_aim_moved) {
    std::cerr << "H4 sniper sight did not respond in the non-inverted direction"
              << ": pitch=" << aim_pitch_before << " -> [" << minimum_aim_pitch
              << ',' << maximum_aim_pitch
              << "], horizontal=" << horizontal_aim_moved << '\n';
    return 33;
  }
  for (std::uint32_t update = 0U; update < 32U; ++update) {
    gameplay->update({});
  }

  const auto nightvision_mission = sf::game::MissionPackage::load(disc, 18U);
  auto nightvision_gameplay =
      std::make_unique<sf::game::GameplaySession>(nightvision_mission);
  if (!nightvision_gameplay->activateRetailAllWeaponsCheat()) {
    std::cerr << "H4 could not enable retail all-weapons state\n";
    return 34;
  }
  const auto nightvision_state =
      nightvision_gameplay->hud().inventory().tryState(
          sf::game::WeaponId::nightvision_rifle);
  if (nightvision_state == nullptr || !nightvision_state->owned) {
    std::cerr << "H4 retail bootstrap did not expose the nightvision rifle\n";
    return 34;
  }
  auto nightvision_selected = false;
  for (std::uint32_t update = 0U; update < 256U; ++update) {
    nightvision_gameplay->update(sf::game::GameplayInput{
        .direct_weapon =
            update == 0U
                ? std::optional<std::uint8_t>{static_cast<std::uint8_t>(
                      sf::game::WeaponId::nightvision_rifle)}
                : std::nullopt,
    });
    if (nightvision_gameplay->runtimeFaulted()) {
      std::cerr
          << "H4 nightvision selection faulted the retail runtime: reason="
          << nightvision_gameplay->runtimeFaultReason()
          << " detail=" << nightvision_gameplay->runtimeFaultDetail() << '\n';
      return 34;
    }
    nightvision_selected = nightvision_gameplay->hud().inventory().current() ==
                               sf::game::WeaponId::nightvision_rifle &&
                           nightvision_gameplay->legacyWeaponMenuState() == -5;
    if (nightvision_selected) {
      break;
    }
  }
  if (!nightvision_selected) {
    std::cerr << "H4 direct nightvision selection never completed\n";
    return 34;
  }
  for (std::uint32_t update = 0U; update < 192U; ++update) {
    nightvision_gameplay->update(sf::game::GameplayInput{.aim = true});
    if (nightvision_gameplay->runtimeFaulted()) {
      std::cerr << "H4 nightvision aim faulted the retail runtime: reason="
                << nightvision_gameplay->runtimeFaultReason()
                << " detail=" << nightvision_gameplay->runtimeFaultDetail()
                << " update=" << update << '\n';
      return 34;
    }
  }

  const auto glock_state =
      gameplay->hud().inventory().tryState(sf::game::WeaponId::glock_17);
  if (glock_state != nullptr && glock_state->owned &&
      (gameplay->canEquipWeapon(sf::game::WeaponId::glock_17) ||
       gameplay->equipWeapon(sf::game::WeaponId::glock_17))) {
    std::cerr << "H4 unreachable direct weapon was not rejected\n";
    return 34;
  }

  std::cout << "H4 presentation gate passed: frames=" << frame_count + 1U
            << ", sequence=" << previous_sequence
            << ", objects=" << maximum_objects
            << ", posed-actors=" << posed_actors
            << ", "
               "controls=retail-four-way+mouse/quick/wheel-burst/multi-notch/"
               "net-zero/"
               "ordering/sniper\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: sf_h4_probe <game.cue> [scope-stress]\n";
    return 1;
  }
  try {
    if (argc == 3 && std::string_view{argv[2]} == "scope-stress") {
      auto disc = sf::game::GameDisc::open(std::filesystem::path{argv[1]});
      return runScopeStressProbe(disc);
    }
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "H4 presentation gate failed: " << error.what() << '\n';
    return 10;
  }
}
