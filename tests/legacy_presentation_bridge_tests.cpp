#include "sf/game/legacy_effect_presentation_policy.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{std::string{message}};
  }
}

void testRetailDangerAggregation() {
  using sf::game::LegacyUiThreatCommand;

  std::vector<LegacyUiThreatCommand> threats{
      {.guest_slot = 7,
       .target_slot = 0,
       .health = 100,
       .ai_state = 1U,
       .danger_q12 = 0x0800U,
       .resident = true,
       .has_target = true},
  };
  require(sf::game::legacyRetailDangerPercent(threats, 0) == 50U,
          "A half-alert retail threat did not fill half the danger bar");

  // State 9 has the original 0xab8 alert floor even when its raw meter is
  // lower. This is the fast reaction visible when an enemy commits to Gabe.
  threats[0].ai_state = 9U;
  threats[0].danger_q12 = 1U;
  require(sf::game::legacyRetailDangerPercent(threats, 0) == 68U,
          "Committed retail threat did not apply the alert floor");

  threats[0].danger_q12 = 0U;
  require(sf::game::legacyRetailDangerPercent(threats, 0) == 0U,
          "Released retail threat remained latched by its stale AI state");

  threats[0].ai_state = 1U;
  threats[0].danger_q12 = 0xffffc000U;
  require(sf::game::legacyRetailDangerPercent(threats, 0) == 0U,
          "Retail controller flags leaked into the danger value");

  threats[0].ai_state = 9U;
  threats[0].danger_q12 = 1U;

  threats.push_back({.guest_slot = 8,
                     .target_slot = 0,
                     .health = 100,
                     .ai_state = 1U,
                     .danger_q12 = 0x0800U,
                     .resident = true,
                     .has_target = true});
  require(sf::game::legacyRetailDangerPercent(threats, 0) == 84U,
          "Multiple retail threats were not combined in Q12 space");

  threats[0].target_slot = 3;
  threats[1].resident = false;
  require(sf::game::legacyRetailDangerPercent(threats, 0) == 0U,
          "Non-player or non-resident threats affected the danger bar");
}

void testDroppedItemPresentationCache() {
  sf::game::LegacyDroppedItemPresentationCache cache;
  sf::game::LegacyDroppedItemBridgeState pistol;
  pistol.slot = 4U;
  pistol.room = 2U;
  pistol.item = 1U;
  pistol.transform.translation = {100, -200, 300};
  const auto same_item = [](const auto &actual, const auto &expected) {
    return actual.slot == expected.slot && actual.room == expected.room &&
           actual.item == expected.item &&
           actual.transform.translation.x == expected.transform.translation.x &&
           actual.transform.translation.y == expected.transform.translation.y &&
           actual.transform.translation.z == expected.transform.translation.z;
  };
  std::vector items{pistol};
  const auto pistol_owner_mask = std::uint32_t{1U} << pistol.slot;

  cache.reconcile(10U, pistol_owner_mask, items);
  require(items.size() == 1U && same_item(items[0], pistol),
          "Validated retail pickup was not published");

  items.clear();
  cache.reconcile(11U, pistol_owner_mask, items);
  require(items.size() == 1U && same_item(items[0], pistol),
          "One transitional allocator tick lost a live pickup");

  // Re-presenting the same immutable guest frame must not age the cache at
  // 60/120/240 Hz.
  items.clear();
  cache.reconcile(11U, pistol_owner_mask, items);
  require(items.size() == 1U && same_item(items[0], pistol),
          "Native presentation refresh aged a retail pickup");

  items.clear();
  cache.reconcile(12U, pistol_owner_mask, items);
  require(items.empty(),
          "Invalid floor descriptor outlived its one-tick grace");

  sf::game::LegacyDroppedItemBridgeState rifle = pistol;
  rifle.slot = 8U;
  rifle.item = 13U;
  const auto rifle_owner_mask = std::uint32_t{1U} << rifle.slot;
  items = {rifle};
  cache.reconcile(20U, rifle_owner_mask, items);
  require(items.size() == 1U && same_item(items[0], rifle),
          "New pickup did not replace the expired cache contents");

  auto armor = rifle;
  armor.item = 0x80U;
  items = {armor};
  cache.reconcile(20U, rifle_owner_mask, items);
  require(items.size() == 1U && same_item(items[0], armor),
          "Same-frame retail slot reuse retained the old pickup");

  items.clear();
  cache.reconcile(20U, 0U, items);
  require(items.empty(),
          "Collected pickup survived a same-frame owner clear");

  // Checkpoint/mission time can regress while the same viewer survives.
  // Old room pickups must never leak into the restarted timeline.
  items.clear();
  cache.reconcile(3U, 0U, items);
  require(items.empty(), "Regressed guest time retained a future pickup");

  items = {pistol};
  cache.reconcile(4U, pistol_owner_mask, items);
  cache.reset();
  items.clear();
  cache.reconcile(5U, 0U, items);
  require(items.empty(), "Explicit presentation reset retained a pickup");
}

void testAtomicDeepCopy() {
  sf::game::LegacyGameplayBridgeState render;
  render.world_model_count = 4U;
  render.player.room = 1;
  render.active_world_models = {1U, 2U};
  render.resident_world_models = {0U, 3U};
  render.dynamic_first_slot = 1U;
  render.target_lock_active = true;
  render.target_hit_result = 0x80041000U;
  render.aimed_target_slot = 1;
  render.proximity_target_slot = 1;
  render.tracked_slots = {1, -1, -1, -1, -1, -1};
  render.objects.resize(2U);
  render.objects[0].resident = true;
  render.objects[0].has_target = true;
  render.objects[0].target_slot = 1;
  render.objects[0].target_flags = 1U;
  render.objects[0].target_meter = 73;
  render.objects[1].resident = true;
  render.objects[1].health = 90;
  render.objects[1].target_flags = 0x40U;
  render.objects[1].has_target = true;
  render.objects[1].target_slot = 0;
  render.objects[1].danger_q12 = 0x800U;
  render.world_callouts.push_back({1, "Head Shot", true});
  sf::game::LegacyWeaponEventBridgeState weapon_event;
  weapon_event.type = sf::game::LegacyWeaponEventType::shot;
  weapon_event.weapon = 1U;
  weapon_event.actor_slot = 0;
  weapon_event.aimed_target_slot = 1;
  weapon_event.hit_result = 0x80041000U;
  weapon_event.origin = {100, -200, 300};
  weapon_event.endpoint = {400, -500, 600};
  weapon_event.first_person = true;
  render.weapon_events.push_back(weapon_event);
  sf::game::LegacyLineParticleBridgeState line_particle;
  line_particle.first = {700, -800, 900};
  line_particle.second = {710, -810, 920};
  line_particle.controller = 3U;
  line_particle.particle = 11U;
  line_particle.source_slot = -1;
  line_particle.remaining_updates = 4;
  line_particle.kind = sf::game::LegacyLineParticleKind::moving_trail;
  line_particle.first_color = {255U, 255U, 192U};
  line_particle.second_color = {255U, 255U, 192U};
  render.line_particles.push_back(line_particle);
  sf::game::LegacyCombatParticleBridgeState combat_particle;
  combat_particle.position = {730, -830, 940};
  combat_particle.controller = 4U;
  combat_particle.particle = 12U;
  combat_particle.attached_slot = 1;
  combat_particle.source_slot = 0;
  combat_particle.remaining_updates = 3;
  combat_particle.kind =
      sf::game::LegacyCombatParticleKind::blood_impact_triangle;
  combat_particle.color = {128U, 16U, 8U};
  combat_particle.scale_byte = 6U;
  combat_particle.angle = 0x100;
  combat_particle.second_angle = 0x555;
  combat_particle.third_angle = -0x555;
  render.combat_particles.push_back(combat_particle);
  sf::game::LegacyPark2FlamethrowerRibbonBridgeState flame_ribbon;
  flame_ribbon.corners = {{{10, 20}, {41, 20}, {10, 51}, {41, 51}}};
  flame_ribbon.world_first = {100, 200, 300};
  flame_ribbon.world_second = {400, 500, 600};
  flame_ribbon.color = {0x55U, 0x66U, 0x77U};
  flame_ribbon.ordering_depth = 300U;
  flame_ribbon.slot = 4U;
  flame_ribbon.frame = 2U;
  flame_ribbon.width_shift = 2U;
  render.park2_flamethrower_ribbons.push_back(flame_ribbon);
  sf::game::LegacyGuestSpriteBridgeState guest_sprite;
  guest_sprite.tpage = 31U;
  guest_sprite.u = 64U;
  guest_sprite.v = 96U;
  guest_sprite.width = 32U;
  guest_sprite.height = 32U;
  guest_sprite.ordering_depth = 123U;
  render.guest_camera_lists_captured = true;
  render.renderer_sprite_fast_path = true;
  render.guest_sprites.push_back(guest_sprite);
  sf::game::LegacyGuestRawPacketBridgeState guest_raw;
  guest_raw.ordering_depth = 124U;
  guest_raw.word_count = 4U;
  guest_raw.opcode = 0x22U;
  guest_raw.words[0] = 0x22112233U;
  render.guest_raw_packets.push_back(guest_raw);
  sf::game::LegacyDroppedItemBridgeState dropped_item;
  dropped_item.slot = 3U;
  dropped_item.room = 2U;
  dropped_item.item = 1U;
  dropped_item.transform.rotation = {
      4096, 0, 0, 0, 4096, 0, 0, 0, 4096,
  };
  dropped_item.transform.translation = {120, -340, 560};
  render.dropped_item_floor_owner_mask =
      std::uint32_t{1U} << dropped_item.slot;
  render.dropped_items.push_back(dropped_item);
  sf::game::LegacyThrownProjectileBridgeState thrown_projectile;
  thrown_projectile.age = 7U;
  thrown_projectile.weapon = 19U;
  thrown_projectile.transform.rotation = {
      4096, 0, 0, 0, 4096, 0, 0, 0, 4096,
  };
  thrown_projectile.transform.translation = {4569, -2492, 3160};
  render.thrown_projectile = thrown_projectile;

  sf::game::LegacyMissionBridgeState ui;
  ui.player_slot = 0;
  ui.player_health = 125;
  ui.inventory.current_weapon = 3U;
  ui.objective_count = 2U;
  ui.parameter_count = 1U;
  ui.objective_texts = {"First objective", "Second objective"};
  ui.parameter_texts = {"Mission parameter"};
  ui.messages = {
      {sf::game::LegacyUiMessageChannel::centered, "Checkpoint", 60U},
      {sf::game::LegacyUiMessageChannel::status, "OBJECTIVE ADDED", 40U},
  };
  ui.messages[0].glyphs = {
      {-24, -6, 32U, 48U, 7U, 8U, {255U, 255U, 255U}},
  };
  ui.messages[1].glyphs = {
      {-17, 91, 40U, 48U, 6U, 8U, {128U, 128U, 128U}},
  };
  ui.messages[1].backdrop = sf::game::LegacyUiBackdropBridgeState{
      {{{-20, 89}, {-4, 89}, {-20, 101}, {-4, 101}}},
      {40U, 48U, 80U},
      true,
  };
  ui.timer = sf::game::LegacyUiTimerBridgeState{
      23999,
      0x0302U,
      {{-176, 50, 64U, 12U, 5U, 8U, {128U, 128U, 128U}}},
  };

  constexpr std::array edges{
      sf::game::LegacyPresentationCommandType::checkpoint_commit,
      sf::game::LegacyPresentationCommandType::play_intro_fmv,
      sf::game::LegacyPresentationCommandType::play_ending_fmv,
      sf::game::LegacyPresentationCommandType::restart_after_failure,
      sf::game::LegacyPresentationCommandType::runtime_fault,
      sf::game::LegacyPresentationCommandType::checkpoint_commit,
  };
  const auto frame =
      sf::game::buildLegacyPresentationFrame(9U, 41U, render, ui, edges);
  require(frame && frame->valid(), "Presentation frame was not built");
  require(sf::game::legacyPresentationFrameConsumable(*frame, 8U) &&
              !sf::game::legacyPresentationFrameConsumable(*frame, 9U),
          "Presentation consumer accepted a replay or rejected a fresh frame");
  require(frame->renderer->guest_frame == frame->ui->guest_frame &&
              frame->guest_frame == 41U,
          "Renderer/UI snapshots did not share one guest frame");
  require(frame->ui->target.active && frame->ui->target.target_slot == 1 &&
              frame->ui->target.aimed_target_slot == 1 &&
              frame->ui->target.proximity_target_slot == 1 &&
              frame->ui->target.hit_result == 0x80041000U &&
              frame->ui->target.headshot &&
              frame->ui->target.target_flags == 1U &&
              frame->ui->target.target_meter == 73 &&
              frame->ui->world_callouts.size() == 1U &&
              frame->ui->world_callouts[0].text == "Head Shot",
          "UI target command mismatch");
  require(frame->ui->threats.size() == 1U &&
              frame->ui->threats[0].danger_q12 == 0x800U,
          "UI threat command mismatch");
  require(frame->ui->mission.messages.size() == 2U &&
              frame->ui->mission.messages[0].channel ==
                  sf::game::LegacyUiMessageChannel::centered &&
              frame->ui->mission.messages[0].text == "Checkpoint" &&
              frame->ui->mission.messages[0].duration == 60U &&
              frame->ui->mission.messages[1].channel ==
                  sf::game::LegacyUiMessageChannel::status &&
              frame->ui->mission.messages[1].text == "OBJECTIVE ADDED" &&
              frame->ui->mission.messages[1].duration == 40U &&
              frame->ui->mission.messages[0].glyphs.size() == 1U &&
              frame->ui->mission.messages[0].glyphs[0].x == -24 &&
              frame->ui->mission.messages[1].backdrop &&
              frame->ui->mission.messages[1].backdrop->corners[3] ==
                  sf::game::LegacyProjectedPointBridgeState{-4, 101} &&
              frame->ui->mission.timer &&
              frame->ui->mission.timer->remaining_ticks == 23999 &&
              frame->ui->mission.timer->glyphs[0].x == -176,
          "Ordered retail UI-message channels were not preserved");
  require(frame->renderer->state.weapon_events.size() == 1U &&
              frame->renderer->state.weapon_events[0].type ==
                  sf::game::LegacyWeaponEventType::shot &&
              frame->renderer->state.weapon_events[0].weapon == 1U &&
              frame->renderer->state.weapon_events[0].actor_slot == 0 &&
              frame->renderer->state.weapon_events[0].aimed_target_slot == 1 &&
              frame->renderer->state.weapon_events[0].first_person &&
              frame->renderer->state.weapon_events[0].origin.y == -200 &&
              frame->renderer->state.weapon_events[0].endpoint.z == 600,
          "Weapon event snapshot mismatch");
  require(frame->renderer->state.thrown_projectile &&
              frame->renderer->state.thrown_projectile->age == 7U &&
              frame->renderer->state.thrown_projectile->weapon == 19U &&
              frame->renderer->state.thrown_projectile->transform.translation ==
                  sf::game::LegacyNativePoint{4569, -2492, 3160},
          "Thrown-projectile snapshot mismatch");
  require(frame->renderer->state.line_particles.size() == 1U &&
              frame->renderer->state.line_particles[0].particle == 11U &&
              frame->renderer->state.line_particles[0].first.y == -800 &&
              frame->renderer->state.line_particles[0].second.z == 920 &&
              frame->renderer->state.line_particles[0].first_color ==
                  sf::game::LegacyRgbBridgeState{255U, 255U, 192U},
          "LINE_G2 presentation snapshot mismatch");
  require(frame->renderer->state.combat_particles.size() == 1U &&
              frame->renderer->state.combat_particles[0].particle == 12U &&
              frame->renderer->state.combat_particles[0].position.y == -830 &&
              frame->renderer->state.combat_particles[0].attached_slot == 1 &&
              frame->renderer->state.combat_particles[0].source_slot == 0 &&
              frame->renderer->state.combat_particles[0].color ==
                  sf::game::LegacyRgbBridgeState{128U, 16U, 8U},
          "Flat combat-particle presentation snapshot mismatch");
  require(
      frame->renderer->state.park2_flamethrower_ribbons.size() == 1U &&
          frame->renderer->state.park2_flamethrower_ribbons[0].slot == 4U &&
          frame->renderer->state.park2_flamethrower_ribbons[0].frame == 2U &&
          frame->renderer->state.park2_flamethrower_ribbons[0].world_first ==
              sf::game::LegacyNativePoint{100, 200, 300} &&
          frame->renderer->state.park2_flamethrower_ribbons[0].world_second ==
              sf::game::LegacyNativePoint{400, 500, 600} &&
          frame->renderer->state.park2_flamethrower_ribbons[0].width_shift ==
              2U &&
          frame->renderer->state.park2_flamethrower_ribbons[0].corners[3] ==
              sf::game::LegacyProjectedPointBridgeState{41, 51},
      "PARK2 flame-ribbon presentation snapshot mismatch");
  require(frame->renderer->state.guest_camera_lists_captured &&
              frame->renderer->state.renderer_sprite_fast_path &&
              frame->renderer->state.guest_sprites.size() == 1U &&
              frame->renderer->state.guest_sprites[0].tpage == 31U &&
              frame->renderer->state.guest_sprites[0].u == 64U &&
              frame->renderer->state.guest_raw_packets.size() == 1U &&
              frame->renderer->state.guest_raw_packets[0].opcode == 0x22U,
          "Guest camera-packet presentation snapshot mismatch");
  require(
      frame->commands.size() == 6U &&
          frame->contains(
              sf::game::LegacyPresentationCommandType::present_renderer) &&
          frame->contains(
              sf::game::LegacyPresentationCommandType::refresh_ui) &&
          frame->contains(
              sf::game::LegacyPresentationCommandType::checkpoint_commit) &&
          frame->contains(
              sf::game::LegacyPresentationCommandType::play_intro_fmv) &&
          frame->contains(
              sf::game::LegacyPresentationCommandType::play_ending_fmv) &&
          frame->contains(
              sf::game::LegacyPresentationCommandType::restart_after_failure) &&
          !frame->contains(
              sf::game::LegacyPresentationCommandType::runtime_fault),
      "Presentation edge commands were incomplete or not unique");

  render.objects[0].target_meter = 1;
  render.active_world_models[0] = 3U;
  render.resident_world_models.clear();
  render.objects[1].danger_q12 = 0U;
  render.world_callouts[0].text = "mutated";
  render.weapon_events[0].weapon = 2U;
  render.weapon_events[0].origin.y = 0;
  render.line_particles[0].first.y = 0;
  render.line_particles[0].first_color = {0U, 0U, 0U};
  render.combat_particles[0].position.y = 0;
  render.combat_particles[0].color = {0U, 0U, 0U};
  render.park2_flamethrower_ribbons[0].corners[3] = {0, 0};
  render.guest_sprites[0].tpage = 0U;
  render.renderer_sprite_fast_path = false;
  render.guest_raw_packets[0].words[0] = 0x20112233U;
  render.dropped_item_floor_owner_mask = 0U;
  render.dropped_items[0].transform.translation.x = 999;
  render.thrown_projectile->transform.translation = {};
  ui.player_health = 1;
  ui.objective_texts[0] = "mutated objective";
  ui.messages[0].text = "mutated centered message";
  ui.messages[0].duration = 1U;
  ui.messages[0].glyphs[0].x = 0;
  ui.messages[1].channel = sf::game::LegacyUiMessageChannel::centered;
  ui.messages[1].text = "mutated status message";
  ui.messages[1].backdrop->corners[3] = {0, 0};
  ui.timer->remaining_ticks = 1;
  ui.timer->glyphs[0].x = 0;
  require(frame->renderer->state.active_world_models ==
                  std::vector<std::uint16_t>({1U, 2U}) &&
              frame->renderer->state.resident_world_models ==
                  std::vector<std::uint16_t>({0U, 3U}) &&
              frame->renderer->state.objects[0].target_meter == 73 &&
              frame->renderer->state.weapon_events[0].weapon == 1U &&
              frame->renderer->state.weapon_events[0].origin.y == -200 &&
              frame->renderer->state.dropped_items.size() == 1U &&
              frame->renderer->state.dropped_items[0].transform.translation.x ==
                  120 &&
              frame->renderer->state.dropped_items[0].transform.rotation[4] ==
                  4096 &&
              frame->renderer->state.dropped_item_floor_owner_mask ==
                  (std::uint32_t{1U} << 3U) &&
              frame->renderer->state.line_particles[0].first.y == -800 &&
              frame->renderer->state.line_particles[0].first_color ==
                  sf::game::LegacyRgbBridgeState{255U, 255U, 192U} &&
              frame->renderer->state.combat_particles[0].position.y == -830 &&
              frame->renderer->state.combat_particles[0].color ==
                  sf::game::LegacyRgbBridgeState{128U, 16U, 8U} &&
              frame->renderer->state.park2_flamethrower_ribbons[0].corners[3] ==
                  sf::game::LegacyProjectedPointBridgeState{41, 51} &&
              frame->renderer->state.renderer_sprite_fast_path &&
              frame->renderer->state.guest_sprites[0].tpage == 31U &&
              frame->renderer->state.guest_raw_packets[0].opcode == 0x22U &&
              frame->renderer->state.thrown_projectile &&
              frame->renderer->state.thrown_projectile->transform.translation ==
                  sf::game::LegacyNativePoint{4569, -2492, 3160} &&
              frame->ui->threats[0].danger_q12 == 0x800U &&
              frame->ui->world_callouts[0].text == "Head Shot" &&
              frame->ui->mission.player_health == 125 &&
              frame->ui->mission.objective_texts[0] == "First objective" &&
              frame->ui->mission.messages.size() == 2U &&
              frame->ui->mission.messages[0].channel ==
                  sf::game::LegacyUiMessageChannel::centered &&
              frame->ui->mission.messages[0].text == "Checkpoint" &&
              frame->ui->mission.messages[0].duration == 60U &&
              frame->ui->mission.messages[0].glyphs[0].x == -24 &&
              frame->ui->mission.messages[1].channel ==
                  sf::game::LegacyUiMessageChannel::status &&
              frame->ui->mission.messages[1].text == "OBJECTIVE ADDED" &&
              frame->ui->mission.messages[1].backdrop &&
              frame->ui->mission.messages[1].backdrop->corners[3] ==
                  sf::game::LegacyProjectedPointBridgeState{-4, 101} &&
              frame->ui->mission.timer &&
              frame->ui->mission.timer->remaining_ticks == 23999 &&
              frame->ui->mission.timer->glyphs[0].x == -176,
          "Published presentation frame aliased mutable source state");
}

void testInvalidAndFaultFrames() {
  sf::game::LegacyGameplayBridgeState render;
  render.world_model_count = 1U;
  render.player.room = 0;
  render.active_world_models = {0U};
  render.objects.resize(1U);
  sf::game::LegacyMissionBridgeState ui;
  ui.player_slot = 2;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an invalid player slot");

  ui.player_slot = 0;
  render.active_world_models = {1U};
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an out-of-range world model");
  render.active_world_models = {0U};
  ui.objective_count = 1U;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an incoherent mission text table");
  ui.objective_count = 0U;
  sf::game::LegacyWeaponEventBridgeState invalid_weapon_event;
  invalid_weapon_event.type = sf::game::LegacyWeaponEventType::shot;
  invalid_weapon_event.weapon = 24U;
  invalid_weapon_event.actor_slot = 0;
  render.weapon_events.push_back(invalid_weapon_event);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an invalid weapon event");
  render.weapon_events.clear();
  sf::game::LegacyLineParticleBridgeState invalid_line;
  invalid_line.first = {1, 2, 3};
  invalid_line.second = {4, 5, 6};
  invalid_line.particle = 0U;
  invalid_line.remaining_updates = 1;
  invalid_line.kind = sf::game::LegacyLineParticleKind::ballistic_tracer;
  invalid_line.semi_transparent = true;
  render.line_particles.push_back(invalid_line);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a semitransparent ballistic line");
  render.line_particles[0].semi_transparent = false;
  render.line_particles[0].kind =
      static_cast<sf::game::LegacyLineParticleKind>(0xffU);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an unknown LINE_G2 kind");
  render.line_particles[0].kind =
      sf::game::LegacyLineParticleKind::ballistic_tracer;
  render.line_particles[0].controller = 0x58U;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a LINE_G2 controller outside retail "
          "allocation");
  render.line_particles.clear();

  sf::game::LegacyCombatParticleBridgeState invalid_particle;
  invalid_particle.position = {1, 2, 3};
  invalid_particle.remaining_updates = 1;
  invalid_particle.kind =
      sf::game::LegacyCombatParticleKind::blood_impact_triangle;
  render.combat_particles.push_back(invalid_particle);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a zero-scale blood triangle");
  render.combat_particles[0].scale_byte = 6U;
  render.combat_particles[0].source_slot = 1;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an out-of-range combat source slot");
  render.combat_particles[0].source_slot = -1;
  render.combat_particles[0].kind =
      static_cast<sf::game::LegacyCombatParticleKind>(0xffU);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an unknown combat-particle kind");
  render.combat_particles.clear();

  sf::game::LegacyDroppedItemBridgeState dropped_item;
  dropped_item.slot = 2U;
  dropped_item.room = 0U;
  dropped_item.item = 1U;
  dropped_item.transform.rotation = {
      4096, 0, 0, 0, 4096, 0, 0, 0, 4096,
  };
  dropped_item.transform.translation = {100, -200, 300};
  render.dropped_items.push_back(dropped_item);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a pickup without a floor owner");
  render.dropped_item_floor_owner_mask =
      std::uint32_t{1U} << dropped_item.slot;
  require(static_cast<bool>(
              sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui)),
          "Presentation bridge rejected a valid 3D pickup transform");
  render.dropped_item_floor_owner_mask |= std::uint32_t{1U} << 30U;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted owner bits outside retail capacity");
  render.dropped_item_floor_owner_mask =
      std::uint32_t{1U} << dropped_item.slot;
  render.dropped_items[0].transform.rotation = {};
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an empty pickup transform");
  render.dropped_items.clear();
  render.dropped_item_floor_owner_mask = 0U;

  sf::game::LegacyThrownProjectileBridgeState thrown_projectile;
  thrown_projectile.age = 7U;
  thrown_projectile.weapon = 19U;
  thrown_projectile.transform.rotation = {
      4096, 0, 0, 0, 4096, 0, 0, 0, 4096,
  };
  thrown_projectile.transform.translation = {100, -200, 300};
  render.thrown_projectile = thrown_projectile;
  require(static_cast<bool>(
              sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui)),
          "Presentation bridge rejected a valid thrown projectile");
  render.thrown_projectile->weapon = 21U;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a non-grenade projectile");
  render.thrown_projectile->weapon = 19U;
  render.thrown_projectile->age = 61U;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an expired thrown projectile");
  render.thrown_projectile->age = 7U;
  render.thrown_projectile->transform.rotation = {};
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an empty projectile transform");
  render.thrown_projectile.reset();

  sf::game::LegacyPark2FlamethrowerRibbonBridgeState flame_ribbon;
  flame_ribbon.corners = {{{10, 20}, {41, 20}, {10, 51}, {41, 51}}};
  flame_ribbon.world_first = {100, 200, 300};
  flame_ribbon.world_second = {400, 500, 600};
  flame_ribbon.color = {0x55U, 0x66U, 0x77U};
  flame_ribbon.ordering_depth = 300U;
  flame_ribbon.slot = 4U;
  flame_ribbon.frame = 2U;
  flame_ribbon.width_shift = 2U;
  render.park2_flamethrower_ribbons.push_back(flame_ribbon);
  require(static_cast<bool>(
              sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui)),
          "Presentation bridge rejected a valid PARK2 flame ribbon");
  render.park2_flamethrower_ribbons[0].ordering_depth = 0U;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a zero-depth PARK2 flame ribbon");
  render.park2_flamethrower_ribbons[0].ordering_depth = 300U;
  render.park2_flamethrower_ribbons[0].frame = 5U;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a frame mismatched to its PARK2 slot");
  render.park2_flamethrower_ribbons[0].frame = 2U;
  render.park2_flamethrower_ribbons[0].corners[0].x = -1025;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an unprojectable PARK2 flame corner");
  render.park2_flamethrower_ribbons[0].corners[0].x = 10;
  render.park2_flamethrower_ribbons[0].width_shift = 0U;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an invalid PARK2 width history");
  render.park2_flamethrower_ribbons[0].width_shift = 2U;
  render.park2_flamethrower_ribbons.push_back(flame_ribbon);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a duplicate PARK2 flame slot");
  render.park2_flamethrower_ribbons.clear();

  sf::game::LegacyGuestRawPacketBridgeState raw_packet;
  static_assert(sf::game::legacyGuestRawPacketNeedsDrawMode(0x22U));
  static_assert(sf::game::legacyGuestRawPacketNeedsDrawMode(0x52U));
  static_assert(!sf::game::legacyGuestRawPacketNeedsDrawMode(0x20U));
  static_assert(!sf::game::legacyGuestRawPacketNeedsDrawMode(0x50U));
  static_assert(sf::game::legacyGuestRawPacketOtIndex(0U, 4096U) == 0U);
  static_assert(sf::game::legacyGuestRawPacketOtIndex(5000U, 4096U) == 4095U);
  raw_packet.ordering_depth = 0U;
  raw_packet.word_count = 4U;
  raw_packet.opcode = 0x22U;
  raw_packet.words[0] = 0x22112233U;
  render.guest_raw_packets.push_back(raw_packet);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an uncaptured guest packet list");
  render.guest_raw_packets.clear();
  render.renderer_sprite_fast_path = true;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an uncaptured sprite-sort mode");
  render.renderer_sprite_fast_path = false;
  render.guest_raw_packets.push_back(raw_packet);
  render.guest_camera_lists_captured = true;
  require(static_cast<bool>(
              sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui)),
          "Presentation bridge rejected a valid retail raw packet");
  render.guest_raw_packets[0].word_count = 5U;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a non-retail raw packet layout");
  render.guest_raw_packets[0].word_count = 4U;
  render.guest_raw_packets[0].effect_particle = 2;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted raw particle without world provenance");
  render.guest_raw_packets[0].effect_world_position_valid = true;
  render.guest_raw_packets[0].effect_position = {100, 200, 300};
  require(static_cast<bool>(
              sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui)),
          "Presentation bridge rejected coherent raw particle provenance");
  render.guest_raw_packets.clear();

  sf::game::LegacyGuestSpriteBridgeState guest_sprite;
  guest_sprite.tpage = 0x20U;
  render.guest_sprites.push_back(guest_sprite);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a non-GPU guest sprite page");
  render.guest_sprites.clear();

  guest_sprite.tpage = 12U;
  guest_sprite.effect_particle = 4;
  guest_sprite.effect_family = 0U;
  guest_sprite.effect_frame = 0U;
  guest_sprite.effect_position = {100, 200, 300};
  render.guest_sprites.push_back(guest_sprite);
  require(static_cast<bool>(
              sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui)),
          "Presentation bridge rejected a retail lamp/glass fragment sprite");
  render.guest_sprites.clear();

  guest_sprite.tpage = 31U;
  guest_sprite.effect_particle = 4;
  guest_sprite.effect_family = 3U;
  guest_sprite.effect_frame = 16U;
  render.guest_sprites.push_back(guest_sprite);
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted invalid SPFX provenance");
  render.guest_sprites.clear();

  render.scrim.resource_present = true;
  render.scrim.visible = true;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted foreground SCRIM without a retail "
          "world transform");
  render.scrim.transform.rotation = {4096, 0, 0, 0, 4096, 0, 0, 0, 4096};
  render.scrim.transform.translation = {120, -340, 560};
  render.scrim.transform_valid = true;
  const auto scrim_frame =
      sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui);
  require(scrim_frame &&
              scrim_frame->renderer->state.scrim.transform.translation.z == 560,
          "Presentation bridge lost the retail SCRIM world transform");
  render.scrim.vram_moves_active = true;
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted an active SCRIM copy phase without "
          "the complete retail packet chain");
  render.scrim.vram_moves.assign(
      13U, sf::game::LegacyVramMoveBridgeState{0, 0, 1, 1, 1, 0});
  require(static_cast<bool>(
              sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui)),
          "Presentation bridge rejected the active thirteen-packet SCRIM "
          "copy phase");
  render.scrim.vram_moves[0] = {63, 0, 2, 1, 0, 0};
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a cross-page SCRIM copy");
  render.scrim.vram_moves[0] = {0, 0, 1, 1, 1, 0};
  render.scrim.vram_moves_active = false;
  require(static_cast<bool>(
              sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui)),
          "Presentation bridge rejected the inactive half of the retail "
          "SCRIM copy cycle");
  render.scrim = {};

  sf::game::LegacyUiMessageBridgeState invalid_message;
  invalid_message.channel = sf::game::LegacyUiMessageChannel::centered;
  invalid_message.backdrop = sf::game::LegacyUiBackdropBridgeState{};
  ui.messages = {invalid_message};
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted a status backdrop on centered text");
  ui.messages.clear();
  ui.timer = sf::game::LegacyUiTimerBridgeState{};
  require(!sf::game::buildLegacyPresentationFrame(1U, 0U, render, ui),
          "Presentation bridge accepted the retail absent-timer sentinel");
  ui.timer.reset();

  const auto fault = sf::game::buildLegacyPresentationFaultFrame(2U, 7U);
  require(fault && !fault->valid() && fault->commands.size() == 1U &&
              fault->contains(
                  sf::game::LegacyPresentationCommandType::runtime_fault) &&
              !sf::game::legacyPresentationFrameConsumable(*fault, 0U),
          "Presentation fault frame was not fail-closed");

  sf::game::LegacyPresentationFrame split_tick;
  split_tick.sequence = 3U;
  split_tick.guest_frame = 8U;
  split_tick.renderer.emplace();
  split_tick.renderer->guest_frame = 8U;
  split_tick.ui.emplace();
  split_tick.ui->guest_frame = 9U;
  split_tick.commands = {
      {sf::game::LegacyPresentationCommandType::present_renderer, 3U},
      {sf::game::LegacyPresentationCommandType::refresh_ui, 3U},
  };
  require(!sf::game::legacyPresentationFrameConsumable(split_tick, 0U),
          "Presentation consumer accepted split renderer/UI ticks");
}

void testWeaponEffectPresentationQueue() {
  auto first = std::make_shared<sf::game::LegacyPresentationFrame>();
  first->sequence = 1U;
  first->renderer.emplace();
  sf::game::LegacyWeaponEventBridgeState first_event;
  first_event.weapon = 1U;
  first->renderer->state.weapon_events.push_back(first_event);
  sf::game::LegacyLineParticleBridgeState one_tick_line;
  one_tick_line.controller = 3U;
  one_tick_line.particle = 7U;
  one_tick_line.source_slot = 91;
  one_tick_line.remaining_updates = 1;
  one_tick_line.first.x = 100;
  first->renderer->state.line_particles.push_back(one_tick_line);
  sf::game::LegacyCombatParticleBridgeState one_tick_particle;
  one_tick_particle.controller = 4U;
  one_tick_particle.particle = 8U;
  one_tick_particle.remaining_updates = 1;
  one_tick_particle.kind =
      sf::game::LegacyCombatParticleKind::blood_impact_triangle;
  one_tick_particle.scale_byte = 6U;
  one_tick_particle.position.x = 300;
  first->renderer->state.combat_particles.push_back(one_tick_particle);
  sf::game::LegacyGuestSpriteBridgeState one_tick_sprite;
  one_tick_sprite.source_address = 0x80002028U;
  one_tick_sprite.effect_particle = 11;
  one_tick_sprite.effect_position = {350, 450, 550};
  one_tick_sprite.width = 8U;
  one_tick_sprite.height = 8U;
  first->renderer->state.camera.target.x = 101;
  first->renderer->state.guest_sprites.push_back(one_tick_sprite);
  sf::game::LegacyGuestRawPacketBridgeState one_tick_raw;
  one_tick_raw.source_address = 0x80001000U;
  one_tick_raw.word_count = 4U;
  one_tick_raw.opcode = 0x20U;
  one_tick_raw.effect_particle = 9;
  one_tick_raw.effect_world_position_valid = true;
  one_tick_raw.effect_position = {500, 600, 700};
  first->renderer->state.camera.eye.x = 100;
  first->renderer->state.guest_raw_packets.push_back(one_tick_raw);

  auto second = std::make_shared<sf::game::LegacyPresentationFrame>();
  second->sequence = 2U;
  second->renderer.emplace();
  sf::game::LegacyWeaponEventBridgeState second_event;
  second_event.weapon = 2U;
  second->renderer->state.weapon_events.push_back(second_event);
  auto updated_line = one_tick_line;
  updated_line.remaining_updates = 2;
  updated_line.first.x = 200;
  second->renderer->state.line_particles.push_back(updated_line);
  auto updated_particle = one_tick_particle;
  updated_particle.remaining_updates = 2;
  updated_particle.position.x = 400;
  second->renderer->state.combat_particles.push_back(updated_particle);
  auto current_sprite = one_tick_sprite;
  current_sprite.source_address = 0x80002090U;
  current_sprite.effect_particle = 12;
  current_sprite.effect_position.x = 850;
  second->renderer->state.camera.target.x = 201;
  second->renderer->state.guest_sprites.push_back(current_sprite);
  auto current_raw = one_tick_raw;
  current_raw.source_address = 0x80001068U;
  current_raw.effect_particle = 10;
  current_raw.effect_position.x = 800;
  second->renderer->state.camera.eye.x = 200;
  second->renderer->state.guest_raw_packets.push_back(current_raw);

  sf::game::LegacyWeaponEffectPresentationQueue queue;
  queue.observe(first);
  queue.observe(second);
  queue.observe(second);
  require(queue.events().size() == 2U && queue.events()[0].weapon == 1U &&
              queue.events()[1].weapon == 2U && queue.lines().size() == 1U &&
              queue.lines()[0].particle == 7U &&
              queue.lines()[0].first.x == 200 &&
              queue.particles().size() == 1U &&
              queue.particles()[0].particle == 8U &&
              queue.particles()[0].position.x == 400 &&
              queue.sprites().size() == 2U &&
              queue.sprites()[0].sprite.effect_particle == 11 &&
              queue.sprites()[0].camera.target.x == 101 &&
              queue.sprites()[1].sprite.effect_particle == 12 &&
              queue.sprites()[1].camera.target.x == 201 &&
              queue.rawPackets().size() == 2U &&
              queue.rawPackets()[0].packet.effect_particle == 9 &&
              queue.rawPackets()[0].camera.eye.x == 100 &&
              queue.rawPackets()[1].packet.effect_particle == 10 &&
              queue.rawPackets()[1].camera.eye.x == 200 &&
              queue.muzzleFlashes().size() == 2U &&
              queue.muzzleFlashes()[0].source_slot == 91 &&
              queue.muzzleFlashes()[1].sequence == 2U,
          "Catch-up presentation lost or duplicated a weapon effect edge");

  queue.consumeFrame();
  require(queue.events().size() == 1U && queue.events()[0].weapon == 2U &&
              queue.lines().size() == 1U && queue.lines()[0].first.x == 200 &&
              queue.particles().size() == 1U &&
              queue.particles()[0].position.x == 400 &&
              queue.sprites().size() == 1U &&
              queue.sprites()[0].sprite.effect_position.x == 850 &&
              queue.rawPackets().size() == 1U &&
              queue.rawPackets()[0].packet.effect_position.x == 800 &&
              queue.muzzleFlashes().size() == 1U &&
              queue.muzzleFlashes()[0].sequence == 2U,
          "Latest guest effect sample did not survive a 60 Hz display frame");
  queue.consumeFrame();
  require(queue.events().size() == 1U && queue.lines().size() == 1U &&
              queue.particles().size() == 1U && queue.sprites().size() == 1U &&
              queue.rawPackets().size() == 1U &&
              queue.muzzleFlashes().size() == 1U,
          "Repeated display frame consumed the current 20 Hz effect sample");

  auto empty = std::make_shared<sf::game::LegacyPresentationFrame>();
  empty->sequence = 3U;
  empty->renderer.emplace();
  queue.observe(empty);
  require(queue.events().empty() && queue.lines().empty() &&
              queue.particles().empty() && queue.sprites().empty() &&
              queue.rawPackets().empty() && queue.muzzleFlashes().empty(),
          "A fresh empty guest sample retained expired weapon effects");

  queue.observe(first);
  require(queue.events().size() == 1U && queue.lines().size() == 1U &&
              queue.particles().size() == 1U && queue.sprites().size() == 1U &&
              queue.rawPackets().size() == 1U &&
              queue.muzzleFlashes().size() == 1U,
          "Sequence rollback retained weapon effects from a newer frame");
  queue.reset();

  const auto taser_frame = [](std::uint64_t sequence,
                              std::uint32_t packet_base,
                              std::int16_t particle_base,
                              bool include_taser) {
    auto frame = std::make_shared<sf::game::LegacyPresentationFrame>();
    frame->sequence = sequence;
    frame->renderer.emplace();
    if (!include_taser) {
      return frame;
    }
    for (std::int16_t segment = 0; segment < 2; ++segment) {
      auto packet = sf::game::LegacyGuestRawPacketBridgeState{};
      packet.source_address =
          packet_base + static_cast<std::uint32_t>(segment) * 0x68U;
      packet.word_count = 4U;
      packet.opcode = 0x50U;
      packet.effect_particle = particle_base + segment;
      packet.effect_controller = 5;
      packet.taser_segment_index = segment;
      packet.taser_segment_count = 2U;
      packet.effect_world_position_valid = true;
      packet.words[1] = static_cast<std::uint32_t>(segment + 1);
      packet.words[3] = static_cast<std::uint32_t>(segment + 2);
      frame->renderer->state.guest_raw_packets.push_back(packet);
    }
    return frame;
  };
  auto taser_first = taser_frame(10U, 0x80010000U, 20, true);
  auto normal_packet = one_tick_raw;
  normal_packet.source_address = 0x80020000U;
  normal_packet.effect_particle = 30;
  taser_first->renderer->state.guest_raw_packets.push_back(normal_packet);
  const auto taser_second =
      taser_frame(11U, 0x80011000U, 40, true);
  const auto taser_stopped = taser_frame(12U, 0U, 0, false);
  sf::game::LegacyWeaponEffectPresentationQueue taser_queue;
  taser_queue.observe(taser_first);
  taser_queue.observe(taser_second);
  require(taser_queue.rawPackets().size() == 3U &&
              std::ranges::count_if(
                  taser_queue.rawPackets(), [](const auto &entry) {
                    return sf::game::
                        legacyGuestRawPacketIsRetailTaserConductor(
                            entry.packet);
                  }) == 2 &&
              taser_queue.rawPackets()[1].packet.effect_particle >= 40,
          "Catch-up mixed two generations of the retail taser chain");
  taser_queue.observe(taser_stopped);
  require(taser_queue.rawPackets().size() == 1U &&
              !sf::game::legacyGuestRawPacketIsRetailTaserConductor(
                  taser_queue.rawPackets()[0].packet),
          "Stopped retail taser retained a stale conductor generation");

  require(queue.events().empty() && queue.lines().empty() &&
              queue.particles().empty() && queue.sprites().empty() &&
              queue.rawPackets().empty() && queue.muzzleFlashes().empty(),
          "Weapon effect presentation queue did not reset");
}

void testScrimCopyPresentationQueue() {
  static_assert(sf::game::legacy_scrim_copy_cycle == 128U);
  const auto make_frame = [](std::uint64_t sequence, bool active) {
    auto frame = std::make_shared<sf::game::LegacyPresentationFrame>();
    frame->sequence = sequence;
    frame->guest_frame = sequence + 100U;
    frame->renderer.emplace();
    auto &scrim = frame->renderer->state.scrim;
    scrim.resource_present = true;
    scrim.vram_moves_active = active;
    scrim.vram_moves.assign(
        13U, sf::game::LegacyVramMoveBridgeState{0, 0, 1, 1, 1, 0});
    return frame;
  };

  sf::game::LegacyScrimCopyPresentationQueue queue;
  const auto positive_first = make_frame(1U, true);
  const auto negative = make_frame(2U, false);
  const auto positive_second = make_frame(3U, true);
  queue.observe(positive_first);
  queue.observe(negative);
  queue.observe(positive_second);
  queue.observe(positive_second);
  require(queue.phases().size() == 2U &&
              queue.phases()[0].guest_frame == 101U &&
              queue.phases()[1].guest_frame == 103U &&
              queue.phases()[0].moves.size() == 13U,
          "Catch-up queue lost or duplicated a retail SCRIM copy phase");
  queue.consumeFrame();
  require(queue.phases().empty(),
          "SCRIM copy phases survived their presentation frame");
  queue.observe(make_frame(1U, true));
  require(queue.phases().size() == 1U,
          "SCRIM queue did not recover after a sequence rollback");
  queue.reset();
  require(queue.phases().empty(), "SCRIM queue did not reset");
  require(sf::game::legacyScrimCopyPhasePosition(11U, 12U) ==
                  sf::game::LegacyScrimCopyPhasePosition::preceding_display &&
              sf::game::legacyScrimCopyPhasePosition(12U, 12U) ==
                  sf::game::LegacyScrimCopyPhasePosition::displayed_frame &&
              sf::game::legacyScrimCopyPhasePosition(13U, 12U) ==
                  sf::game::LegacyScrimCopyPhasePosition::future_frame,
          "SCRIM copy phase lost its post-display timing boundary");
}

void testPersistentWorldVertexColorCache() {
  using sf::game::LegacyWorldSectionColorsBridgeState;
  const std::vector authored{
      LegacyWorldSectionColorsBridgeState{0U, 0U, {0x7fffU, 0x6318U}},
      LegacyWorldSectionColorsBridgeState{1U, 0U, {0x4210U}},
  };
  auto cache = authored;
  const std::array darkened{
      LegacyWorldSectionColorsBridgeState{0U, 0U, {0x2108U, 0x1084U}},
  };
  require(sf::game::mergeLegacyWorldVertexColorCache(cache, darkened) &&
              cache[0].colors == darkened[0].colors,
          "Guest lamp-off colors did not enter the persistent cache");

  require(sf::game::mergeLegacyWorldVertexColorCache(cache, {}) &&
              cache[0].colors == darkened[0].colors,
          "Stream-out restored authored lamp-lit vertex colors");
  const std::array adjacent{
      LegacyWorldSectionColorsBridgeState{1U, 0U, {0x001fU}},
  };
  require(sf::game::mergeLegacyWorldVertexColorCache(cache, adjacent) &&
              cache[0].colors == darkened[0].colors &&
              cache[1].colors == adjacent[0].colors,
          "An adjacent streamed section overwrote the extinguished light");

  const auto before_invalid = cache;
  const std::array malformed{
      LegacyWorldSectionColorsBridgeState{0U, 0U, {0x7fffU, 0x7fffU}},
      LegacyWorldSectionColorsBridgeState{9U, 0U, {0x7fffU}},
  };
  require(!sf::game::mergeLegacyWorldVertexColorCache(cache, malformed) &&
              cache == before_invalid,
          "Malformed world-color update partially mutated the cache");

  const std::array streamed_updates{
      LegacyWorldSectionColorsBridgeState{0U, 0U, {0x2108U, 0x1084U}},
      LegacyWorldSectionColorsBridgeState{1U, 0U, {0x1111U, 0x2222U}},
  };
  const std::array<std::uint16_t, 1U> required_model{0U};
  require(sf::game::mergeLegacyWorldVertexColorCache(cache, streamed_updates,
                                                     required_model) &&
              cache[0].colors == streamed_updates[0].colors &&
              cache[1].colors == adjacent[0].colors,
          "A stale optional stream descriptor invalidated a coherent color "
          "transaction");
  const auto before_required_mismatch = cache;
  const std::array<std::uint16_t, 2U> both_required{0U, 1U};
  require(!sf::game::mergeLegacyWorldVertexColorCache(cache, streamed_updates,
                                                      both_required) &&
              cache == before_required_mismatch,
          "A required world-color topology mismatch partially mutated the "
          "cache");

  const auto checkpoint = cache;
  cache = authored;
  cache = checkpoint;
  require(cache == checkpoint && cache[0].colors == darkened[0].colors,
          "Checkpoint restore lost persistent lamp-off colors");
  cache = authored;
  require(cache == authored,
          "Mission reset did not restore authored world colors");
}

void testNativeFirstPersonOwnsEveryGuestScreenPrimitive() {
  require(sf::game::legacyGuestEffectsAuthoritative(true, true) &&
              !sf::game::legacyGuestEffectsAuthoritative(true, false) &&
              !sf::game::legacyGuestEffectsAuthoritative(false, true),
          "Guest SPFX ownership did not require a complete retail frame");

  require(!sf::game::legacyGuestCameraItemVisibleWithNativeFirstPerson(true,
                                                                       false) &&
              sf::game::legacyGuestCameraItemVisibleWithNativeFirstPerson(
                  true, true) &&
              sf::game::legacyGuestCameraItemVisibleWithNativeFirstPerson(
                  false, false),
          "First-person guest camera ownership is not provenance-only");

  constexpr auto vertical_scope_sprites = 0x80130000U;
  constexpr auto horizontal_scope_sprites = 0x80130100U;
  require(sf::game::legacyGuestSpriteIsRetailScopeOverlayAddress(
              vertical_scope_sprites + 4U * 0x2cU, vertical_scope_sprites,
              horizontal_scope_sprites) &&
              sf::game::legacyGuestSpriteIsRetailScopeOverlayAddress(
                  horizontal_scope_sprites + 7U * 0x2cU,
                  vertical_scope_sprites, horizontal_scope_sprites) &&
              !sf::game::legacyGuestSpriteIsRetailScopeOverlayAddress(
                  horizontal_scope_sprites + 8U * 0x2cU,
                  vertical_scope_sprites, horizontal_scope_sprites),
          "Retail SCP sprite arrays lost their exact 5/8 by 0x2c bounds");

  const auto packet_at = [](std::uint32_t source) {
    return sf::game::LegacyGuestRawPacketBridgeState{
        .source_address = source,
    };
  };
  require(sf::game::legacyGuestRawPacketIsRetailScopeOverlay(
              packet_at(0x8011c5b8U)) &&
              sf::game::legacyGuestRawPacketIsRetailScopeOverlay(
                  packet_at(0x8011c66cU)) &&
              !sf::game::legacyGuestRawPacketIsRetailScopeOverlay(
                  packet_at(0x8011c678U)) &&
              sf::game::legacyGuestRawPacketNeedsDrawMode(0x2aU),
          "Sniper's six authored average-blended dim quads lost provenance");
  require(sf::game::legacyGuestRawPacketIsVirusScannerOverlay(
              packet_at(0x8011c138U + 27U * 0x18U)) &&
              !sf::game::legacyGuestRawPacketIsRetailScopeOverlay(
                  packet_at(0x8011c138U + 27U * 0x18U)) &&
              sf::game::legacyGuestRawPacketIsVirusScannerOverlay(
                  packet_at(0x80135df8U)) &&
              sf::game::legacyGuestRawPacketIsRetailOpticOverlay(
                  packet_at(0x80135df8U)) &&
              !sf::game::legacyGuestRawPacketIsRetailOpticOverlay(
                  packet_at(0x80135e1cU)),
          "Viral detector's 28-line sight or pulsing target dot lost its "
          "fixed-packet ownership");

  sf::game::LegacyGuestSpriteBridgeState particle_sprite;
  particle_sprite.effect_particle = 4;
  particle_sprite.effect_family = 1U;
  particle_sprite.effect_frame = 2U;
  require(sf::game::legacyGuestSpriteUsesWorldDepth(particle_sprite),
          "SPFX sprite lost depth-tested world ownership");
  const std::array particle_sprites{particle_sprite};
  require(
      sf::game::legacyGuestSpriteCoversExplParticle(4, 1U, particle_sprite,
                                                    true, true) &&
          sf::game::legacyExplParticleHasGuestSprite(4, 1U, particle_sprites,
                                                     true, true) &&
          !sf::game::legacyExplParticleHasGuestSprite(5, 1U, particle_sprites,
                                                      true, true) &&
          !sf::game::legacyExplParticleHasGuestSprite(4, 2U, particle_sprites,
                                                      true, true) &&
          !sf::game::legacyExplParticleHasGuestSprite(4, 1U, particle_sprites,
                                                      false, true) &&
          !sf::game::legacyExplParticleHasGuestSprite(4, 1U, particle_sprites,
                                                      true, false) &&
          !sf::game::legacyExplParticleHasGuestSprite(-1, 1U, particle_sprites,
                                                      true, true),
      "Guest SPFX ownership suppressed an unlinked distant EXPL particle");
  particle_sprite.effect_frame = 9U;
  require(sf::game::legacyGuestSpriteCoversExplParticle(4, 1U, particle_sprite,
                                                        true, true),
          "EXPL slot reuse incorrectly depended on the animated frame");
  require(
      sf::game::legacyExplParticleOwnedByGuestSlot(7, 7) &&
          !sf::game::legacyExplParticleOwnedByGuestSlot(7, 8) &&
          !sf::game::legacyExplParticleOwnedByGuestSlot(-1, -1),
      "Distant fire ownership accepted an unrelated or unbound guest slot");
  require(
      sf::game::legacyDistantFireEmitterAllowed(true, true, true, false, false,
                                                7, false) &&
          sf::game::legacyDistantFireEmitterAllowed(true, true, true, false,
                                                    true, 7, false) &&
          sf::game::legacyDistantFireEmitterAllowed(true, false, false, true,
                                                    false, 7, false) &&
          !sf::game::legacyDistantFireEmitterAllowed(false, true, true, false,
                                                     false, 7, false) &&
          !sf::game::legacyDistantFireEmitterAllowed(true, false, true, false,
                                                     false, 7, false) &&
          !sf::game::legacyDistantFireEmitterAllowed(true, false, false, true,
                                                     true, 7, false) &&
          !sf::game::legacyDistantFireEmitterAllowed(true, true, false, false,
                                                     false, 7, false) &&
          !sf::game::legacyDistantFireEmitterAllowed(true, true, true, false,
                                                     false, -1, false) &&
          !sf::game::legacyDistantFireEmitterAllowed(true, true, true, false,
                                                     false, 7, true),
      "Authored CFIRE fallback lost the guest-resident no-EXPL case or escaped "
      "its script activation and retail ownership guards");

  sf::game::LegacyGuestRawPacketBridgeState particle_packet;
  particle_packet.effect_particle = 3;
  require(!sf::game::legacyGuestRawPacketUsesWorldDepth(particle_packet),
          "Anchorless raw SPFX packet acquired world ownership");
  particle_packet.effect_world_position_valid = true;
  particle_packet.effect_position = {100, 200, 300};
  require(sf::game::legacyGuestRawPacketUsesWorldDepth(particle_packet),
          "Raw SPFX packet lost depth-tested world ownership");

  sf::game::LegacyCombatParticleBridgeState combat_particle;
  combat_particle.particle = 3U;
  const std::array combat_particles{combat_particle};
  require(sf::game::legacyGuestRawPacketHasWorldCombatParticle(
              particle_packet, combat_particles),
          "Semantic combat particle retained its duplicate raw packet");
  combat_particle.particle = 7U;
  const std::array unrelated_particles{combat_particle};
  require(!sf::game::legacyGuestRawPacketHasWorldCombatParticle(
              particle_packet, unrelated_particles),
          "Unrelated raw packet was suppressed by a combat particle");

  sf::game::LegacyLineParticleBridgeState line_particle;
  line_particle.particle = 3U;
  line_particle.kind = sf::game::LegacyLineParticleKind::moving_trail;
  const std::array reconstructed_lines{line_particle};
  require(sf::game::legacyGuestRawPacketHasWorldLine(particle_packet,
                                                     reconstructed_lines),
          "Reconstructable world line retained its duplicate raw packet");
  line_particle.raw_packet_authoritative = true;
  const std::array bouncing_lines{line_particle};
  require(sf::game::legacyGuestRawPacketHasWorldLine(particle_packet,
                                                     bouncing_lines),
          "Bouncing update7 line retained a depth-free raw packet");
}

void testRetailGuestSpriteSortSelection() {
  sf::game::LegacyGuestSpriteBridgeState sprite;
  // Bit 27 used to be mistaken for the renderer-wide fast-sort selector.
  sprite.attribute = 0x08000000U;
  sprite.width = 96U;
  sprite.height = 64U;
  sprite.mapping_x = 48;
  sprite.mapping_y = 32;
  sprite.scale_x = 2048;
  sprite.scale_y = 8192;
  sprite.rotation = 360;

  const auto regular = sf::game::legacyGuestSpriteSortTransform(sprite, false);
  require(regular.local_left == -48.0 && regular.local_top == -32.0 &&
              regular.local_right == 48.0 && regular.local_bottom == 32.0 &&
              regular.scale_x == 0.5 && regular.scale_y == 2.0 &&
              regular.angle_units == 1.0,
          "Regular GsSPRITE path ignored authored transform fields");

  const auto fast = sf::game::legacyGuestSpriteSortTransform(sprite, true);
  require(fast.local_left == 0.0 && fast.local_top == 0.0 &&
              fast.local_right == 96.0 && fast.local_bottom == 64.0 &&
              fast.scale_x == 1.0 && fast.scale_y == 1.0 &&
              fast.angle_units == 0.0,
          "Fast GsSPRITE path did not use the exact x/y/w/h rectangle");

  sprite.attribute = 0U;
  require(sf::game::legacyGuestSpriteSortTransform(sprite, true).local_right ==
              fast.local_right,
          "GsSPRITE attribute incorrectly selected the renderer fast path");
}

} // namespace

int main() {
  try {
    testRetailDangerAggregation();
    testDroppedItemPresentationCache();
    testAtomicDeepCopy();
    testInvalidAndFaultFrames();
    testWeaponEffectPresentationQueue();
    testScrimCopyPresentationQueue();
    testPersistentWorldVertexColorCache();
    testNativeFirstPersonOwnsEveryGuestScreenPrimitive();
    testRetailGuestSpriteSortSelection();
    std::cout << "legacy presentation bridge tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "legacy presentation bridge tests failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
}
