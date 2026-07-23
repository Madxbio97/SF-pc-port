#include "sf/game/combat.hpp"
#include "sf/game/effects.hpp"
#include "sf/game/mission_scripts.hpp"
#include "sf/game/npc_ai.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{std::string{message}};
  }
}

sf::game::NpcState hostile() {
  return sf::game::NpcState{
      .active = true,
      .object = 3U,
      .disposition = sf::game::NpcDisposition::hostile,
      .weapon = sf::game::WeaponId::glock_17,
      .health = 100U,
      .maximum_health = 100U,
      .magazine = 15U,
      .reserve_ammo = 60U,
      .magazine_capacity = 15U,
  };
}

sf::game::NpcPerception visiblePlayer(double distance = 1500.0) {
  return sf::game::NpcPerception{
      .player_z = distance,
      .distance = distance,
      .player_visible = true,
  };
}

void testOriginalWeaponRecords() {
  using namespace sf::game;
  using DamagePair = std::array<std::uint16_t, 2U>;
  constexpr std::array<DamagePair, weapon_slot_count> expected_damage{{
      DamagePair{15U, 0U},    DamagePair{50U, 25U},   DamagePair{50U, 25U},
      DamagePair{15U, 0U},    DamagePair{150U, 75U},  DamagePair{50U, 25U},
      DamagePair{300U, 150U}, DamagePair{200U, 75U},  DamagePair{50U, 25U},
      DamagePair{50U, 25U},   DamagePair{75U, 35U},   DamagePair{50U, 25U},
      DamagePair{90U, 90U},   DamagePair{50U, 25U},   DamagePair{100U, 0U},
      DamagePair{50U, 50U},   DamagePair{600U, 600U}, DamagePair{50U, 25U},
      DamagePair{0U, 0U},     DamagePair{600U, 0U},   DamagePair{600U, 0U},
      DamagePair{0U, 0U},     DamagePair{75U, 75U},   DamagePair{0U, 0U},
      DamagePair{0U, 0U},     DamagePair{0U, 0U},
  }};
  for (std::size_t slot = 0; slot < expected_damage.size(); ++slot) {
    const auto &definition =
        weaponCombatDefinition(static_cast<WeaponId>(slot));
    require(definition.close_damage == expected_damage[slot][0] &&
                definition.distant_damage == expected_damage[slot][1],
            "Recovered SCUS close/distant damage table mismatch");
  }
  const auto &glock = weaponCombatDefinition(WeaponId::glock_17);
  require(glock.close_damage == 50U && glock.distant_damage == 25U &&
              glock.damageAtDistance(960.0) == 50U &&
              glock.damageAtDistance(961.0) == 25U &&
              glock.world_model == "GLOCK17",
          "Glock record must preserve both native damage fields and model");
  require(glock.fire_mode == WeaponFireMode::semi_automatic,
          "Glock must be semi-automatic");
  const auto &shotgun = weaponCombatDefinition(WeaponId::combat_shotgun);
  require(shotgun.close_damage == 300U && shotgun.distant_damage == 150U &&
              shotgun.pellet_count == 8U && shotgun.world_model == "BERELLI",
          "Combat shotgun record must preserve native damage fields and pellet "
          "pattern");
  const auto &m16 = weaponCombatDefinition(WeaponId::m_16);
  require(m16.automatic() && m16.close_damage == 50U &&
              m16.distant_damage == 25U && m16.world_model == "M16",
          "M16 record must expose automatic fire and its mission model");
  require(
      weaponCombatDefinition(WeaponId::pistol_357).close_damage == 150U &&
          weaponCombatDefinition(WeaponId::pistol_357).distant_damage == 75U &&
          weaponCombatDefinition(WeaponId::m_79).close_damage == 600U &&
          weaponCombatDefinition(WeaponId::fragmentation_grenade)
                  .close_damage == 600U,
      "Recovered SCUS damage values for heavy and explosive weapons mismatch");
  require(weaponStance(WeaponId::glock_17) == PlayerWeaponStance::sidearm &&
              weaponStance(WeaponId::m_16) == PlayerWeaponStance::long_gun,
          "Weapon stance must select matching native animation channels");
  require(
      weaponCombatDefinition(WeaponId::taser).fire_mode ==
              WeaponFireMode::continuous &&
          weaponCombatDefinition(WeaponId::flamethrower).damage_kind ==
              WeaponDamageKind::fire &&
          weaponCombatDefinition(WeaponId::m_79).fire_mode ==
              WeaponFireMode::projectile &&
          weaponCombatDefinition(WeaponId::fragmentation_grenade).fire_mode ==
              WeaponFireMode::thrown,
      "Special weapons must retain distinct beam, flame, projectile and throw "
      "modes");
  constexpr std::array<std::string_view, weapon_slot_count> retail_models{
      "",        "GLOKSIL",  "GLOCK17", "",         "COLT45",   "GLOCK18",
      "BERELLI", "ITHICA37", "AK102",   "M16",      "BIZON2",   "MP5",
      "DRAGSVD", "SUPERG",   "TASER",   "FLAMEGDF", "GRENLAUN", "G3",
      "FLASHLT", "GRENADE",  "GRENADE", "FLASHLT",  "CHNGUN",   "",
      "",        "",
  };
  constexpr std::array<unsigned int, weapon_slot_count> retail_cadence{
      0U, 4U, 12U, 0U,  8U, 1U, 18U, 15U, 2U, 2U, 2U, 2U, 20U,
      4U, 0U, 2U,  20U, 2U, 0U, 15U, 15U, 0U, 2U, 0U, 0U, 0U,
  };
  for (std::size_t slot = 0U; slot < weapon_slot_count; ++slot) {
    const auto &definition =
        weaponCombatDefinition(static_cast<WeaponId>(slot));
    require(definition.world_model == retail_models[slot] &&
                definition.fire_interval_updates == retail_cadence[slot],
            "Recovered SCUS weapon model/cadence table mismatch");
  }
}

void testArmorFirstDamage() {
  auto vitals = sf::game::PlayerVitals{
      .health = 150U,
      .maximum_health = 150U,
      .armor = 20U,
      .maximum_armor = 600U,
  };
  const auto result = sf::game::applyPlayerDamage(vitals, 25U);
  require(result.armor_damage == 20U && result.health_damage == 0U,
          "One original hit must be routed entirely to armor");
  require(vitals.armor == 0U && vitals.health == 150U && !result.killed,
          "Armor break must discard the remainder of the current hit");
  const auto health_result = sf::game::applyPlayerDamage(vitals, 25U);
  require(health_result.armor_damage == 0U &&
              health_result.health_damage == 25U && vitals.health == 125U,
          "A later hit must damage health after armor has reached zero");
  vitals.armor = 600U;
  const auto gas_result = sf::game::applyPlayerDamage(vitals, 600U, true);
  require(
      gas_result.armor_damage == 0U && gas_result.health_damage == 125U &&
          vitals.armor == 600U && vitals.health == 0U && gas_result.killed,
      "Native gas damage must bypass armor and kill through remaining health");
}

void testHostileReactionAndFire() {
  require(sf::game::npc_updates_per_second == 20U &&
              sf::game::npc_reaction_updates == 7U &&
              sf::game::npc_hurt_updates == 4U &&
              sf::game::npc_alert_memory_updates == 200U &&
              sf::game::npc_death_updates == 40U &&
              sf::game::npc_reload_updates == 27U &&
              sf::game::npc_aim_settle_updates == 8U &&
              sf::game::npc_cover_hold_updates == 30U &&
              sf::game::npc_lost_sight_grace_updates == 6U &&
              sf::game::npc_search_updates == 60U,
          "Fallback NPC timers must use one native retail tick at 20 Hz");
  auto state = hostile();
  auto perception = visiblePlayer();
  static_cast<void>(sf::game::updateNpcBrain(state, perception));
  require(state.behavior == sf::game::NpcBehavior::alert,
          "Visible hostile must enter alert state");
  for (unsigned int update = 1U; update < sf::game::npc_reaction_updates;
       ++update) {
    static_cast<void>(sf::game::updateNpcBrain(state, perception));
    require(state.behavior == sf::game::NpcBehavior::alert,
            "Hostile must retain the original reaction duration at 20 Hz");
  }
  static_cast<void>(sf::game::updateNpcBrain(state, perception));
  require(state.behavior == sf::game::NpcBehavior::attack,
          "Hostile must attack after the native reaction delay");
  sf::game::NpcDecision decision;
  auto fired = false;
  for (unsigned int update = 0U; update < 40U; ++update) {
    decision = sf::game::updateNpcBrain(state, perception);
    fired = fired || decision.fire;
  }
  require(fired && state.shot_serial != 0U,
          "Aligned armed hostile must acquire, aim and fire a complete burst");
}

void testPatrolCoverReloadAndAccuracy() {
  using namespace sf::game;
  auto patrol = hostile();
  patrol.behavior = NpcBehavior::patrol;
  patrol.patrol_points = {
      NpcPatrolPoint{0.0, 0.0, 0.0},
      NpcPatrolPoint{0.0, 0.0, 300.0},
  };
  patrol.patrol_index = 1U;
  const auto patrol_decision = updateNpcBrain(patrol, NpcPerception{});
  require(std::abs(patrol_decision.forward_distance - 190.8 / 20.0) < 0.0001,
          "Authored patrol movement must preserve its world speed at 20 Hz");

  auto covered = hostile();
  covered.health = 50U;
  auto hit = visiblePlayer();
  hit.damaged = true;
  hit.cover_available = true;
  hit.cover_x = -400.0;
  static_cast<void>(updateNpcBrain(covered, hit));
  require(covered.behavior == NpcBehavior::take_cover,
          "Wounded hostile must use a reachable occluded cover point");

  auto reload = hostile();
  reload.behavior = NpcBehavior::attack;
  reload.magazine = 0U;
  static_cast<void>(updateNpcBrain(reload, visiblePlayer()));
  require(reload.behavior == NpcBehavior::reloading,
          "Empty NPC magazine must enter reload state");
  for (unsigned int update = 0U; update < npc_reload_updates; ++update) {
    static_cast<void>(updateNpcBrain(reload, visiblePlayer()));
  }
  require(reload.behavior == NpcBehavior::attack && reload.magazine == 15U &&
              reload.reserve_ammo == 45U,
          "NPC reload must refill the magazine and consume reserve ammunition");

  auto aim = hostile();
  aim.behavior = NpcBehavior::attack;
  aim.state_updates = npc_aim_settle_updates * 2U;
  const auto standing = npcHitChance(aim, 1000.0, 32000.0, false);
  const auto moving = npcHitChance(aim, 1000.0, 32000.0, true);
  require(standing <= 24U && moving < standing,
          "NPC accuracy must remain capped and lose precision against a moving "
          "Gabe");

  auto distant = hostile();
  distant.behavior = NpcBehavior::pursue;
  distant.alert_memory_updates = npc_alert_memory_updates;
  const auto chase = updateNpcBrain(distant, visiblePlayer(5000.0));
  require(distant.behavior == NpcBehavior::pursue &&
              chase.forward_distance > 0.0,
          "A distant visible Gabe must be chased instead of attacked from the "
          "spawn point");

  auto crowded = hostile();
  crowded.behavior = NpcBehavior::attack;
  crowded.state_updates = npc_aim_settle_updates;
  static_cast<void>(updateNpcBrain(crowded, visiblePlayer(250.0)));
  NpcDecision retreat;
  for (unsigned int update = 0U; update < 14U; ++update) {
    retreat = updateNpcBrain(crowded, visiblePlayer(250.0));
    crowded.yaw = retreat.desired_yaw;
  }
  require(retreat.forward_distance > 0.0 &&
              crowded.locomotion == NpcLocomotion::run &&
              std::abs(retreat.desired_yaw - 1600) < 100,
          "A retreating hostile must turn and run along the movement vector "
          "away from Gabe");

  const auto range = npcCombatRange(WeaponId::glock_17);
  require(range.minimum > 0.0 && range.preferred_maximum < 32000.0,
          "Combat spacing must not reuse the technical hitscan range");
}

void testStableCombatLocomotionAndSightMemory() {
  using namespace sf::game;
  auto state = hostile();
  state.behavior = NpcBehavior::attack;
  state.combat_phase = NpcCombatPhase::reposition;
  state.phase_updates = 1U;
  state.reposition_direction = -1;
  auto perception = visiblePlayer(1500.0);
  const auto first = updateNpcBrain(state, perception);
  require(first.strafe_distance < 0.0 &&
              npcAnimationRequest(state).action == NpcAnimationAction::aim &&
              npcAnimationRequest(state).motion == ActorMotion::strafe_left,
          "Combat repositioning must combine a stable aim upper body with "
          "strafe legs");

  state.movement_distance = 0.0;
  const auto second = updateNpcBrain(state, perception);
  require(second.strafe_distance < 0.0 &&
              npcAnimationRequest(state).action == NpcAnimationAction::aim,
          "A blocked step must not flip the actor from AIM to a full-body RUN "
          "clip");

  perception.player_visible = false;
  for (unsigned int update = 0U; update + 1U < npc_lost_sight_grace_updates;
       ++update) {
    static_cast<void>(updateNpcBrain(state, perception));
    require(state.behavior == NpcBehavior::attack,
            "One-frame wall occlusion must not restart combat pursuit");
  }
  static_cast<void>(updateNpcBrain(state, perception));
  require(state.behavior == NpcBehavior::pursue,
          "A genuinely lost target must be pursued at its last known position");
}

void testAuthoredPursuitRoute() {
  using namespace sf::game;
  auto state = hostile();
  state.behavior = NpcBehavior::pursue;
  state.alert_memory_updates = npc_alert_memory_updates;
  state.last_known_player_x = 1000.0;
  state.last_known_player_z = 1000.0;
  state.patrol_points = {
      NpcPatrolPoint{0.0, 0.0, 0.0},
      NpcPatrolPoint{0.0, 0.0, 400.0},
      NpcPatrolPoint{500.0, 0.0, 400.0},
      NpcPatrolPoint{900.0, 0.0, 900.0},
  };
  auto hidden = visiblePlayer(1400.0);
  hidden.player_x = 1000.0;
  hidden.player_z = 1000.0;
  hidden.player_visible = false;
  const auto decision = updateNpcBrain(state, hidden);
  require(state.route_active && state.route_index == 1U &&
              decision.forward_distance > 0.0,
          "Lost-target pursuit must enter the actor's authored SUBWAY.BIN node "
          "route");
}

void testHostileReturnsToAuthoredZone() {
  using namespace sf::game;
  auto state = hostile();
  state.behavior = NpcBehavior::attack;
  state.x = 2400.0;
  state.home_x = 0.0;
  state.home_z = 0.0;
  auto outside = visiblePlayer(9000.0);
  outside.target_inside_zone = false;
  auto decision = updateNpcBrain(state, outside);
  require(
      state.behavior == NpcBehavior::return_home,
      "A hostile must drop combat when Gabe leaves its authored route zone");
  auto returning = false;
  for (unsigned int update = 0U; update < 40U; ++update) {
    state.yaw = decision.desired_yaw;
    decision = updateNpcBrain(state, outside);
    returning = returning || decision.forward_distance > 0.0;
  }
  require(returning && npcAnimationRequest(state).motion == ActorMotion::run,
          "An out-of-zone hostile must turn toward home and use its run "
          "locomotion");
}

void testCloseDetectionAndDisposition() {
  auto close = hostile();
  auto perception = visiblePlayer(500.0);
  perception.signed_player_angle = 2048;
  static_cast<void>(sf::game::updateNpcBrain(close, perception));
  require(close.behavior == sf::game::NpcBehavior::alert,
          "Close visible player must be noticed outside the normal sight cone");

  auto ally = hostile();
  ally.disposition = sf::game::NpcDisposition::ally;
  ally.behavior = sf::game::NpcBehavior::attack;
  const auto decision = sf::game::updateNpcBrain(ally, visiblePlayer(100.0));
  require(!decision.fire, "Allied mission actors must never attack Gabe");

  auto squad = hostile();
  auto shared_alert = visiblePlayer(5000.0);
  shared_alert.player_visible = false;
  shared_alert.ally_alerted = true;
  static_cast<void>(sf::game::updateNpcBrain(squad, shared_alert));
  require(squad.behavior == sf::game::NpcBehavior::alert,
          "An alerted hostile must share Gabe's last known position with its "
          "nearby squad");
}

void testOriginalDangerLock() {
  using namespace sf::game;
  auto near_enemy = hostile();
  near_enemy.behavior = NpcBehavior::alert;
  auto near = visiblePlayer(400.0);
  NpcDangerSignal near_signal;
  for (unsigned int update = 0U; update < 4U; ++update) {
    near_signal = updateNpcDanger(near_enemy, near, false, false);
  }

  auto far_enemy = hostile();
  far_enemy.behavior = NpcBehavior::alert;
  auto far = visiblePlayer(3000.0);
  NpcDangerSignal far_signal;
  for (unsigned int update = 0U; update < 4U; ++update) {
    far_signal = updateNpcDanger(far_enemy, far, false, false);
  }
  require(near_signal.level > far_signal.level && near_signal.level != 0U,
          "DANGER must rise more strongly for a nearby visible enemy");

  near.player_visible = false;
  const auto before_hidden = near_signal.level;
  for (unsigned int update = 0U; update < 4U; ++update) {
    near_signal = updateNpcDanger(near_enemy, near, false, false);
  }
  require(near_signal.level < before_hidden,
          "DANGER must decay after the enemy loses sight of Gabe");

  auto locked = hostile();
  locked.behavior = NpcBehavior::attack;
  locked.combat_phase = NpcCombatPhase::burst;
  const auto critical =
      updateNpcDanger(locked, visiblePlayer(1200.0), true, false);
  require(critical.critical && critical.level == 100U &&
              locked.danger_lock == npc_danger_maximum,
          "An exact hostile aim lock must fill DANGER immediately");
  const auto evaded =
      updateNpcDanger(locked, visiblePlayer(1200.0), true, true);
  require(!evaded.critical && evaded.level < critical.level &&
              locked.danger_evade_updates == npc_danger_roll_evasion_updates,
          "A roll must break exact lock and reduce DANGER");
}

void testHurtAndDeathAnimations() {
  auto state = hostile();
  state.scripted_climbing = true;
  require(sf::game::npcAnimationRequest(state).action ==
                  sf::game::NpcAnimationAction::climb &&
              sf::game::npcAnimationRequest(state).motion ==
                  sf::game::ActorMotion::climb,
          "The authored street ingress must select CLIMBA while crossing the "
          "fence");
  state.legacy_presentation_valid = true;
  state.legacy_presentation_code = 10U;
  require(sf::game::npcAnimationRequest(state).action ==
              sf::game::NpcAnimationAction::jump,
          "The retail ingress launch state must select JP1");
  state.legacy_presentation_code = 12U;
  require(sf::game::npcAnimationRequest(state).action ==
              sf::game::NpcAnimationAction::fall,
          "The retail ingress descent state must select FALL1");
  state.scripted_climbing = false;
  state.scripted_low_locomotion = true;
  state.movement_distance = 4.0;
  const auto crouch_walk = sf::game::npcAnimationRequest(state);
  require(crouch_walk.action == sf::game::NpcAnimationAction::walk &&
              crouch_walk.motion == sf::game::ActorMotion::crouch_walk,
          "A retail low-route actor must use crouch locomotion");
  state.scripted_low_locomotion = false;
  state.movement_distance = 0.0;
  state.scripted_kneeling = true;
  state.behavior = sf::game::NpcBehavior::attack;
  require(sf::game::npcAnimationRequest(state).action ==
              sf::game::NpcAnimationAction::aim,
          "A kneeling combatant must aim without standing up");
  state.fire_animation_updates = 1U;
  const auto kneeling_fire = sf::game::npcAnimationRequest(state);
  require(kneeling_fire.action == sf::game::NpcAnimationAction::fire &&
              kneeling_fire.motion == sf::game::ActorMotion::kneel,
          "A kneeling combatant must fire from the kneeling lower-body pose");
  state.scripted_kneeling = false;
  state.fire_animation_updates = 0U;
  auto perception = visiblePlayer();
  perception.damaged = true;
  static_cast<void>(sf::game::updateNpcBrain(state, perception));
  require(state.behavior == sf::game::NpcBehavior::hurt,
          "Damage must interrupt an actor with the native hit state");
  require(sf::game::npcAnimationRequest(state).action ==
              sf::game::NpcAnimationAction::hit_left,
          "Hurt state must select a hit reaction clip");

  state.health = 0U;
  static_cast<void>(sf::game::updateNpcBrain(state, perception));
  require(state.behavior == sf::game::NpcBehavior::dying,
          "Zero health must enter the non-looping death state");
  perception.damaged = false;
  for (unsigned int update = 1U; update < sf::game::npc_death_updates;
       ++update) {
    static_cast<void>(sf::game::updateNpcBrain(state, perception));
  }
  state.animation_tick = 42U;
  static_cast<void>(sf::game::updateNpcBrain(state, perception));
  require(state.behavior == sf::game::NpcBehavior::dead &&
              state.animation_tick == 42U,
          "Death state must settle on the final lying pose without restarting "
          "the clip");
  require(sf::game::npcAnimationRequest(state).action ==
              sf::game::NpcAnimationAction::dead,
          "Settled corpses must request an explicit final death frame");

  state.death_kind = sf::game::NpcDeathKind::electrical;
  state.behavior = sf::game::NpcBehavior::dying;
  require(sf::game::npcAnimationRequest(state).action ==
              sf::game::NpcAnimationAction::electrical_death,
          "Electrical kills must use the recovered FIREDANC animation path");
}

void testCombatEffectsAndDestructibleClasses() {
  using namespace sf::game;
  require(legacyFireLatchBeginsShot(0U, 6U) &&
              legacyFireLatchBeginsShot(2U, 6U) &&
              !legacyFireLatchBeginsShot(6U, 5U) &&
              !legacyFireLatchBeginsShot(0U, 0U),
          "Retail automatic-fire latch reload was not treated as a new shot");
  require(
      legacyFireEmitterPresentation(legacy_cfire_a_class, "CFIREA.TMD") &&
          legacyFireEmitterPresentation(legacy_cfire_b_class, "CFIREB") &&
          legacyFireEmitterPresentation(legacy_cfire_c_class, "CFIREC.TMD") &&
          !legacyFireEmitterPresentation(legacy_cfire_a_class, "CFIREB.TMD") &&
          !legacyFireEmitterPresentation(0x24U, "CFIREA.TMD") &&
          objectDamageResponse(0x20U) == ObjectDamageResponse::shatter &&
          objectDamageResponse(0x37U) == ObjectDamageResponse::shatter &&
          objectDamageResponse(0x56U) == ObjectDamageResponse::shatter &&
          objectDamageResponse(0x13U) == ObjectDamageResponse::extinguish &&
          objectDamageResponse(0x16U) == ObjectDamageResponse::extinguish &&
          objectDamageResponse(0x33U) == ObjectDamageResponse::extinguish &&
          objectDamageResponse(0x11U, "HLITE.TMD") ==
              ObjectDamageResponse::extinguish &&
          objectDamageResponse(0x11U, "GASPIPE.TMD") ==
              ObjectDamageResponse::explosive &&
          objectDamageResponse(0x3aU) == ObjectDamageResponse::breakable &&
          objectDamageResponse(0x2eU) == ObjectDamageResponse::explosive &&
          objectDamageResponse(0x2cU) == ObjectDamageResponse::vehicle &&
          objectDamageResponse(0x4fU) == ObjectDamageResponse::none &&
          objectDamageResponse(0x50U) == ObjectDamageResponse::none &&
          objectDamageResponse(0xffU) == ObjectDamageResponse::none,
      "SUBWAY object classes must preserve retail hit/interaction callbacks");
  require(legacyGuestDestructionStateAuthoritative(
              ObjectDamageResponse::extinguish, 0, true) &&
              !legacyGuestDestructionStateAuthoritative(
                  ObjectDamageResponse::extinguish, 0, false) &&
              legacyGuestDestructionStateAuthoritative(
                  ObjectDamageResponse::shatter, 0, true) &&
              legacyGuestDestructionStateAuthoritative(
                  ObjectDamageResponse::shatter, 1, false),
          "Retail lamp destruction latch lost its class-specific lifetime");
  require(
      legacyGuestDestroyedState(ObjectDamageResponse::extinguish, true,
                                false) &&
          legacyGuestDestroyedState(ObjectDamageResponse::extinguish, false,
                                    true) &&
          !legacyGuestDestroyedState(ObjectDamageResponse::extinguish, false,
                                     false) &&
          legacyGuestDestroyedState(ObjectDamageResponse::shatter, true, false),
      "Streamed prop destruction was not monotonic");
  require(legacyGuestStaticPropPresentationAllowed(
              true, true, true, ObjectDamageResponse::extinguish) &&
              !legacyGuestStaticPropPresentationAllowed(
                  true, true, false, ObjectDamageResponse::extinguish) &&
              !legacyGuestStaticPropPresentationAllowed(
                  true, true, true, ObjectDamageResponse::none) &&
              legacyGuestStaticPropPresentationAllowed(
                  false, false, false, ObjectDamageResponse::none),
          "Destroyed lamp secondary model was hidden by the guest dormant bit");

  auto flash = makeGameplayEffect(GameplayEffectType::muzzle_flash, 1.0, 2.0,
                                  3.0, 0.0, 0.0, 1.0, 1.0, 7U);
  flash.attachment = GameplayEffectAttachment::player_muzzle;
  auto npc_flash = flash;
  npc_flash.attachment = GameplayEffectAttachment::npc_muzzle;
  require(nativeGameplayEffectPresentationAllowed(flash, false, false) &&
              !nativeGameplayEffectPresentationAllowed(flash, true, false) &&
              !nativeGameplayEffectPresentationAllowed(flash, false, true) &&
              nativeGameplayEffectPresentationAllowed(npc_flash, true, false) &&
              nativeGameplayEffectPresentationAllowed(npc_flash, true, true),
          "First-person aim hid enemy world-space muzzle effects");
  GameplayMuzzleFlashPresentationQueue muzzle_queue;
  const std::array first_flash{flash};
  muzzle_queue.observe(first_flash);
  muzzle_queue.observe({});
  require(muzzle_queue.flashes().size() == 1U &&
              muzzle_queue.flashes().front().seed == flash.seed,
          "Catch-up discarded a one-tick native muzzle flash");
  muzzle_queue.consumeFrame();
  require(muzzle_queue.flashes().empty(),
          "Consumed native muzzle flash survived an empty simulation sample");
  auto next_flash = flash;
  ++next_flash.seed;
  const std::array second_flash{next_flash};
  muzzle_queue.observe(first_flash);
  muzzle_queue.observe(second_flash);
  require(muzzle_queue.flashes().size() == 2U,
          "Catch-up collapsed two distinct native muzzle flashes");
  muzzle_queue.consumeFrame();
  require(muzzle_queue.flashes().size() == 1U &&
              muzzle_queue.flashes().front().seed == next_flash.seed,
          "Latest native muzzle flash did not survive its display interval");
  muzzle_queue.reset();
  require(muzzle_queue.flashes().empty(),
          "Native muzzle flash presentation queue did not reset");
  require(flash.remaining_updates == 1U && !advanceGameplayEffect(flash),
          "Muzzle flash must use one native 20 Hz finite lifetime");
  auto blot = makeGameplayEffect(GameplayEffectType::blood_decal, 0.0, 0.0, 0.0,
                                 0.0, 0.0, 1.0, 1.0, 9U);
  require(blot.remaining_updates == 80U && advanceGameplayEffect(blot),
          "Blood blot must persist after the short spray has finished");
  auto spray = makeGameplayEffect(GameplayEffectType::blood_spray, 0.0, 0.0,
                                  0.0, 0.0, 0.0, 1.0, 1.0, 11U);
  require(
      spray.remaining_updates == 3U,
      "Blood droplets must expire quickly instead of hanging in world space");
  auto car_fire = makeGameplayEffect(GameplayEffectType::burning_fire, 0.0, 0.0,
                                     0.0, 0.0, -1.0, 0.0, 1.0, 13U);
  for (auto update = 0U; update < 600U; ++update) {
    require(advanceGameplayEffect(car_fire),
            "The opening police-car fire must persist after the rollover");
  }
  blot.attachment = GameplayEffectAttachment::npc_body;
  blot.owner_object = 12U;
  require(blot.attachment == GameplayEffectAttachment::npc_body &&
              blot.owner_object == 12U,
          "Persistent hit marks must be attachable to their struck actor");
}

void testSubwayMissionScriptRuntime() {
  using namespace sf::game;
  MissionScriptRuntime runtime{9U};
  runtime.configureActor(0U, 175U, true, 1U); // finite bank slot
  runtime.configureActor(1U, 174U, true, 3U); // Kravitch
  runtime.configureActor(2U, 176U, true, 0U);
  runtime.configureActor(3U, 182U, true, 0U);
  runtime.configureActor(4U, 183U, true, 0U);
  runtime.configureActor(5U, 172U, false, 0U); // CBDC agent
  runtime.configureActor(6U, 184U, true, 1U);  // transient street slot
  runtime.configureActor(7U, 180U, true, 1U);  // transient bank slot
  runtime.configureActor(8U, 181U, true, 1U);  // transient bank slot
  require(runtime.repeatable(0U) && runtime.repeatable(6U) &&
              runtime.repeatable(7U) && runtime.repeatable(8U) &&
              !runtime.initiallyDormant(2U) && !runtime.initiallyDormant(3U) &&
              !runtime.initiallyDormant(4U) && !runtime.repeatable(1U),
          "SUBWAY ai_parameter must expose all four transient hostile slots "
          "without hiding NPCs");

  const auto update =
      [&](std::span<const MissionScriptActorSnapshot> actors = {},
          double x = 0.0, double y = 0.0, double z = 0.0,
          bool interact = false) {
        return runtime.update(
            MissionScriptUpdateContext{actors, x, y, z, interact});
      };

  runtime.actorKilled(1U);
  auto commands = update();
  require(commands.empty(),
          "Kravitch's death must not hide or recreate room-streamed actors");

  runtime.objectDestroyed(260U);
  commands = update();
  require(commands.size() == 1U &&
              commands.front().type ==
                  MissionScriptCommandType::capture_checkpoint,
          "Destroying the communications array must advance the mission "
          "checkpoint");

  runtime.objectDestroyed(140U);
  commands = update();
  require(commands.size() == 1U &&
              commands.front().type ==
                  MissionScriptCommandType::destroy_object_source &&
              commands.front().object == 67U,
          "Shooting the authored lock must open its paired first-level gate");

  runtime.actorKilled(5U);
  commands = update();
  require(commands.size() == 1U &&
              commands.front().type == MissionScriptCommandType::mission_failed,
          "A killed CBDC agent must fail the protection objective");

  runtime.reset();
  runtime.actorKilled(6U);
  const std::array dead_wave{
      MissionScriptActorSnapshot{6U, true, false, true, false, 2000.0},
  };
  for (unsigned int tick = 1U; tick < mission_reinforcement_delay_updates;
       ++tick) {
    require(runtime.update(MissionScriptUpdateContext{dead_wave}).empty(),
            "The endless street wave must retain its original respawn delay");
  }
  commands = runtime.update(MissionScriptUpdateContext{dead_wave});
  require(commands.size() == 1U &&
              commands.front().type ==
                  MissionScriptCommandType::respawn_actor &&
              commands.front().object == 6U,
          "The street reinforcement slot must respawn off-screen after every "
          "completed death");

  runtime.reset();
  for (std::uint8_t killed = 0U; killed < mission_bank_attacker_count;
       ++killed) {
    runtime.actorKilled(0U);
    if (killed + 1U < mission_bank_attacker_count) {
      for (unsigned int tick = 0U; tick < mission_reinforcement_delay_updates;
           ++tick) {
        commands = runtime.update(MissionScriptUpdateContext{std::array{
            MissionScriptActorSnapshot{0U, true, false, true, false, 2000.0}}});
      }
      require(!commands.empty() && commands.back().type ==
                                       MissionScriptCommandType::respawn_actor,
              "The finite bank wave must recycle its authored slot");
    }
  }
  require(runtime.state().cbdc_protected &&
              runtime.state().bank_attackers_eliminated ==
                  mission_bank_attacker_count,
          "Five eliminated attackers must complete CBDC protection");

  runtime.actorKilled(1U);
  runtime.objectDestroyed(260U);
  commands = update();
  require(
      runtime.state().initial_objectives_complete,
      "Bank protection, Kravitch and the radio must unlock the subway route");
  commands = runtime.update(
      MissionScriptUpdateContext{{}, 1338.0, -134.0, -1403.0, true});
  require(runtime.state().upper_bomb_tagged &&
              std::ranges::any_of(
                  commands,
                  [](const MissionScriptCommand &command) {
                    return command.type ==
                           MissionScriptCommandType::capture_checkpoint;
                  }),
          "Interacting with the upper subway bomb must tag it and checkpoint");
  commands = runtime.update(
      MissionScriptUpdateContext{{}, -309.0, 1118.0, 426.0, false});
  require(
      runtime.state().finale_started &&
          std::ranges::any_of(commands,
                              [](const MissionScriptCommand &command) {
                                return command.type ==
                                       MissionScriptCommandType::start_finale;
                              }),
      "Reaching the lower bomb after tagging must start the ending movie");

  commands = runtime.update(
      MissionScriptUpdateContext{{}, -562.0, -259.0, 4304.0, true});
  require(
      runtime.state().security_bypassed &&
          std::ranges::any_of(
              commands,
              [](const MissionScriptCommand &command) {
                return command.type ==
                           MissionScriptCommandType::destroy_object_source &&
                       command.object == 68U;
              }),
      "The recovered power switch must open its linked security gate");

  runtime.objectDamaged(30U, false);
  commands = update();
  require(
      runtime.state().failed &&
          std::ranges::any_of(commands,
                              [](const MissionScriptCommand &command) {
                                return command.type ==
                                       MissionScriptCommandType::mission_failed;
                              }),
      "Shooting the final subway bomb must fail Georgia Street immediately");
}

void testOpeningCameraAndMapFadeRuntime() {
  using namespace sf::game;
  OpeningCinematicCameraRuntime camera;
  const auto initial = camera.sample(0U);
  require(initial.x == 2372.0 && initial.y == -3206.0 && initial.z == 5977.0 &&
              initial.target_x == 3559.0 && initial.target_y == -2425.0 &&
              initial.target_z == 4482.0,
          "Opening camera must start at the linked SUBWAY 35/36 transforms");
  const auto first_tick = camera.sample(1U);
  require(
      first_tick.x == 2378.0 && first_tick.y == -3205.0 &&
          first_tick.z == 5981.0 && first_tick.target_x == 3559.0 &&
          first_tick.target_y == -2426.0 && first_tick.target_z == 4480.0,
      "Opening event 0x12 must expose its immediate first fixed-point step");
  const auto moving = camera.sample(10U);
  require(
      moving.x > initial.x && moving.z < initial.z &&
          moving.target_z < initial.target_z,
      "Opening aim must follow the eye's recovered current-segment fraction");
  const auto second_node = camera.sample(28U);
  require(second_node.x == 3131.0 && second_node.y == -3079.0 &&
              second_node.z == 5751.0 && second_node.target_x == 3547.0 &&
              second_node.target_y == -2423.0 && second_node.target_z == 3737.0,
          "Opening camera must reproduce native tick 28 fixed-point residuals");
  const auto final = camera.sample(1000U);
  require(
      final.x == 4849.0 && final.y == -2301.0 && final.z == 2812.0 &&
          final.target_x == 4793.0 && final.target_y == -2298.0 &&
          final.target_z == 2849.0,
      "Opening camera must clamp to the recovered fixed-point terminal state");
  require(opening_cinematic_duration_updates == 195U,
          "Opening camera must return to gameplay after 194 native ticks");

  MapFadeRuntime fade;
  fade.resetFromBlack();
  require(fade.intensity() == 240U,
          "Map fade must expose the native first post-release draw intensity");
  fade.advance();
  require(fade.intensity() == 225U,
          "Map fade must release by 15 on each recovered 20 Hz frame");
  for (unsigned int frame = 1U; frame < 16U; ++frame) {
    fade.advance();
  }
  require(fade.intensity() == 0U,
          "Map fade must clamp cleanly at the normal game brightness");
}

} // namespace

int main() {
  try {
    testOriginalWeaponRecords();
    testArmorFirstDamage();
    testHostileReactionAndFire();
    testPatrolCoverReloadAndAccuracy();
    testStableCombatLocomotionAndSightMemory();
    testAuthoredPursuitRoute();
    testHostileReturnsToAuthoredZone();
    testCloseDetectionAndDisposition();
    testOriginalDangerLock();
    testHurtAndDeathAnimations();
    testCombatEffectsAndDestructibleClasses();
    testSubwayMissionScriptRuntime();
    testOpeningCameraAndMapFadeRuntime();
    std::cout << "combat/AI tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "combat/AI test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
