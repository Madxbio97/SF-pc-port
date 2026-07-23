#pragma once

#include "sf/game/actor_animation.hpp"
#include "sf/game/hud.hpp"

#include <cstdint>
#include <vector>

namespace sf::game {

enum class NpcDisposition : std::uint8_t {
  neutral,
  ally,
  hostile,
};

enum class NpcBehavior : std::uint8_t {
  idle,
  patrol,
  alert,
  pursue,
  return_home,
  search,
  take_cover,
  attack,
  reloading,
  hurt,
  surrender,
  dying,
  dead,
};

enum class NpcLocomotion : std::uint8_t {
  stationary,
  walk,
  run,
  backpedal,
  strafe_left,
  strafe_right,
  turn_left,
  turn_right,
};

enum class NpcCombatPhase : std::uint8_t {
  acquire,
  aim,
  burst,
  recover,
  reposition,
  retreat,
};

enum class NpcDeathKind : std::uint8_t {
  normal,
  fire,
  electrical,
};

// Retail AI +0x41 is a short firing counter. A fresh shot either starts it
// from zero or reloads it upward while an automatic burst is still active.
// Counting only the zero-to-nonzero edge drops those in-burst reloads.
[[nodiscard]] constexpr bool
legacyFireLatchBeginsShot(std::uint8_t previous,
                          std::uint8_t current) noexcept {
  return current != 0U && (previous == 0U || current > previous);
}

struct NpcPatrolPoint {
  double x{};
  double y{};
  double z{};
};

struct NpcState {
  bool active{};
  std::uint16_t object{};
  std::uint16_t source_index{};
  NpcDisposition disposition{NpcDisposition::neutral};
  NpcBehavior behavior{NpcBehavior::idle};
  WeaponId weapon{WeaponId::unarmed};
  double x{};
  double y{};
  double z{};
  double home_x{};
  double home_y{};
  double home_z{};
  double zone_min_x{};
  double zone_max_x{};
  double zone_min_z{};
  double zone_max_z{};
  double last_known_player_x{};
  double last_known_player_z{};
  std::int32_t yaw{};
  std::uint16_t health{};
  std::uint16_t maximum_health{};
  std::uint16_t armor{};
  std::uint16_t maximum_armor{};
  std::uint32_t path_data_offset{};
  std::vector<NpcPatrolPoint> patrol_points;
  bool patrol_loops{};
  std::size_t patrol_loop_start{};
  std::size_t patrol_index{};
  int patrol_direction{1};
  std::size_t route_index{};
  int route_direction{1};
  bool route_active{};
  bool route_finished{};
  bool scripted_defuser{};
  bool scripted_intro_agent{};
  bool scripted_intro_spawned{};
  unsigned int scripted_intro_spawn_update{};
  bool scripted_bank_staged{};
  bool scripted_opening_combat{};
  std::uint8_t scripted_opening_lane{0xffU};
  bool scripted_opening_midpoint_reached{};
  bool scripted_opening_arrived{};
  bool scripted_low_locomotion{};
  double scripted_midpoint_x{};
  double scripted_midpoint_y{};
  double scripted_midpoint_z{};
  double scripted_combat_x{};
  double scripted_combat_y{};
  double scripted_combat_z{};
  bool scripted_kneeling{};
  // SUBWAY actor template 184 owns the two authored street ingresses linked
  // to police car 57. Runtime instances traverse their own route root before
  // the scripted CBDC exchange takes control.
  bool scripted_ingress{};
  bool scripted_wall_traversed{};
  bool scripted_climbing{};
  unsigned int scripted_climb_update{};
  unsigned int scripted_climb_duration{};
  double scripted_climb_start_x{};
  double scripted_climb_start_y{};
  double scripted_climb_start_z{};
  double scripted_climb_end_x{};
  double scripted_climb_end_y{};
  double scripted_climb_end_z{};
  double cover_x{};
  double cover_y{};
  double cover_z{};
  bool cover_available{};
  bool cover_arrived{};
  std::uint16_t magazine{};
  std::uint16_t reserve_ammo{};
  std::uint16_t magazine_capacity{};
  NpcDeathKind death_kind{NpcDeathKind::normal};
  NpcLocomotion locomotion{NpcLocomotion::stationary};
  NpcCombatPhase combat_phase{NpcCombatPhase::acquire};
  unsigned int state_updates{};
  unsigned int phase_updates{};
  unsigned int alert_memory_updates{};
  unsigned int lost_sight_updates{};
  unsigned int fire_cooldown_updates{};
  unsigned int fire_animation_updates{};
  unsigned int cover_wait_updates{};
  unsigned int retreat_updates{};
  unsigned int blocked_updates{};
  unsigned int danger_evade_updates{};
  std::uint16_t danger_lock{};
  unsigned int burst_rounds_remaining{};
  int avoidance_side{1};
  int reposition_direction{1};
  double movement_distance{};
  std::uint32_t shot_serial{};
  std::uint32_t random_state{0x6d2b79f5U};
  bool legacy_presentation_valid{};
  std::uint8_t legacy_presentation_code{};
  std::uint8_t legacy_presentation_mode{};
  bool legacy_ground_contact_valid{};
  double legacy_ground_y{};
  // Locomotion and upper/full-body actions are independent retail channels.
  // A shot may restart the action channel without restarting moving legs.
  std::uint64_t locomotion_animation_tick{};
  std::uint64_t animation_tick{};
};

struct NpcPerception {
  double player_x{};
  double player_y{};
  double player_z{};
  double distance{};
  std::int32_t signed_player_angle{};
  bool player_visible{};
  bool heard_weapon{};
  bool ally_alerted{};
  bool damaged{};
  bool target_moving{};
  // Hostiles may only acquire and pursue targets inside the zone recovered
  // from their authored SUBWAY.BIN route.
  bool target_inside_zone{true};
  // Allies only enter the shared combat state machine for an explicitly
  // hostile actor; this keeps them incapable of treating Gabe as a target.
  bool target_hostile{};
  bool cover_available{};
  double cover_x{};
  double cover_y{};
  double cover_z{};
};

[[nodiscard]] bool npcDispositionsOppose(NpcDisposition first,
                                         NpcDisposition second) noexcept;

struct NpcDecision {
  std::int32_t desired_yaw{};
  double forward_distance{};
  double strafe_distance{};
  bool fire{};
  bool advance_patrol{};
};

struct NpcCombatRange {
  double minimum{};
  double preferred_maximum{};
};

struct NpcDangerSignal {
  std::uint8_t level{};
  bool critical{};
};

inline constexpr unsigned int npc_updates_per_second = 20U;
inline constexpr unsigned int
npcUpdatesAt20Hz(unsigned int updates_at_60_hz) noexcept {
  return updates_at_60_hz == 0U ? 0U : (updates_at_60_hz + 2U) / 3U;
}

inline constexpr unsigned int npc_reaction_updates = npcUpdatesAt20Hz(20U);
inline constexpr unsigned int npc_hurt_updates = npcUpdatesAt20Hz(12U);
inline constexpr unsigned int npc_alert_memory_updates = npcUpdatesAt20Hz(600U);
inline constexpr unsigned int npc_death_updates = npcUpdatesAt20Hz(120U);
// RELOAD is a native PCHAN clip and therefore runs for one 20 Hz tick per
// frame.
inline constexpr unsigned int npc_reload_updates = 27U;
inline constexpr unsigned int npc_aim_settle_updates = npcUpdatesAt20Hz(24U);
inline constexpr unsigned int npc_cover_hold_updates = npcUpdatesAt20Hz(90U);
inline constexpr unsigned int npc_lost_sight_grace_updates =
    npcUpdatesAt20Hz(18U);
inline constexpr unsigned int npc_search_updates = npcUpdatesAt20Hz(180U);
inline constexpr double npc_close_detection_distance = 819.0;
inline constexpr double npc_maximum_sight_distance = 32000.0;
inline constexpr double npc_alert_share_distance = 3200.0;
inline constexpr std::int32_t npc_sight_half_angle = 0x271;
inline constexpr std::uint16_t npc_danger_maximum = 0x1000U;
inline constexpr std::uint16_t npc_danger_roll_reduction = 0x600U;
inline constexpr unsigned int npc_danger_roll_evasion_updates =
    npcUpdatesAt20Hz(24U);

void setNpcBehavior(NpcState &state, NpcBehavior behavior) noexcept;
[[nodiscard]] NpcDecision
updateNpcBrain(NpcState &state, const NpcPerception &perception) noexcept;
[[nodiscard]] NpcAnimationRequest
npcAnimationRequest(const NpcState &state) noexcept;
[[nodiscard]] NpcCombatRange npcCombatRange(WeaponId weapon) noexcept;
[[nodiscard]] unsigned int npcHitChance(const NpcState &state, double distance,
                                        double maximum_range,
                                        bool target_moving) noexcept;
[[nodiscard]] NpcDangerSignal updateNpcDanger(NpcState &state,
                                              const NpcPerception &perception,
                                              bool exact_aim,
                                              bool player_rolled) noexcept;

} // namespace sf::game
