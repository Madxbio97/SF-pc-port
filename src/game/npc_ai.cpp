#include "sf/game/npc_ai.hpp"

#include "sf/game/chase_camera.hpp"
#include "sf/game/combat.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sf::game {
namespace {

constexpr std::int32_t maximum_turn_per_update = 120;
constexpr std::int32_t attack_alignment = 96;
constexpr double native_walk_distance_per_update = 190.8 / npc_updates_per_second;
constexpr double native_run_distance_per_update = 657.86 / npc_updates_per_second;
constexpr double patrol_arrival_distance = 70.0;
constexpr double pursuit_arrival_distance = 95.0;
constexpr double cover_arrival_distance = 85.0;
constexpr unsigned int wounded_retreat_updates = npcUpdatesAt20Hz(120U);
constexpr unsigned int cover_peek_updates = npcUpdatesAt20Hz(30U);
constexpr unsigned int combat_aim_updates = npcUpdatesAt20Hz(10U);
constexpr unsigned int combat_burst_timeout_updates = npcUpdatesAt20Hz(120U);
constexpr unsigned int combat_recover_updates = npcUpdatesAt20Hz(28U);
constexpr unsigned int combat_reposition_updates = npcUpdatesAt20Hz(36U);
constexpr unsigned int combat_reposition_blocked_updates = npcUpdatesAt20Hz(8U);
constexpr unsigned int combat_retreat_updates = npcUpdatesAt20Hz(60U);
constexpr unsigned int danger_rise_per_update = 96U * 3U;
constexpr unsigned int danger_fall_per_update = 64U * 3U;

std::int32_t signedHeadingDelta(std::int32_t from, std::int32_t to) noexcept {
    auto delta = normalizeHeading(static_cast<std::int64_t>(to) - from);
    if (delta > heading_angle_units / 2) {
        delta -= heading_angle_units;
    }
    return delta;
}

std::int32_t turnToward(std::int32_t current, std::int32_t target) noexcept {
    const auto delta = std::clamp(
        signedHeadingDelta(current, target),
        -maximum_turn_per_update,
        maximum_turn_per_update);
    return normalizeHeading(static_cast<std::int64_t>(current) + delta);
}

bool canSeePlayer(const NpcPerception& perception) noexcept {
    return perception.target_inside_zone && perception.player_visible &&
        perception.distance <= npc_maximum_sight_distance &&
        std::abs(perception.signed_player_angle) <= npc_sight_half_angle;
}

void setCombatPhase(NpcState& state, NpcCombatPhase phase) noexcept {
    if (state.combat_phase == phase) {
        return;
    }
    state.combat_phase = phase;
    state.phase_updates = 0U;
}

std::size_t adjacentRouteIndex(
    const NpcState& state,
    std::size_t index,
    int direction) noexcept {
    if (state.patrol_points.empty()) {
        return 0U;
    }
    if (direction > 0) {
        if (index + 1U < state.patrol_points.size()) {
            return index + 1U;
        }
        return state.patrol_loops
            ? std::min(state.patrol_loop_start, state.patrol_points.size() - 1U)
            : index;
    }
    if (index > 0U) {
        return index - 1U;
    }
    return state.patrol_loops ? state.patrol_points.size() - 1U : index;
}

void initializePursuitRoute(NpcState& state) noexcept {
    if (state.patrol_points.size() < 2U) {
        state.route_active = false;
        state.route_finished = true;
        return;
    }
    auto nearest = std::size_t{};
    auto nearest_distance = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < state.patrol_points.size(); ++index) {
        const auto& point = state.patrol_points[index];
        const auto distance = std::hypot(point.x - state.x, point.z - state.z);
        if (distance < nearest_distance) {
            nearest = index;
            nearest_distance = distance;
        }
    }
    const auto forward = adjacentRouteIndex(state, nearest, 1);
    const auto backward = adjacentRouteIndex(state, nearest, -1);
    const auto target_distance = [&](std::size_t index) {
        const auto& point = state.patrol_points[index];
        return std::hypot(
            point.x - state.last_known_player_x,
            point.z - state.last_known_player_z);
    };
    if (forward != nearest && backward != nearest) {
        state.route_direction = target_distance(forward) <= target_distance(backward) ? 1 : -1;
    } else {
        state.route_direction = forward != nearest ? 1 : -1;
    }
    state.route_index = adjacentRouteIndex(state, nearest, state.route_direction);
    state.route_active = state.route_index != nearest;
    state.route_finished = !state.route_active;
}

void advancePursuitRoute(NpcState& state) noexcept {
    if (!state.route_active || state.patrol_points.empty()) {
        return;
    }
    const auto next = adjacentRouteIndex(state, state.route_index, state.route_direction);
    const auto distance_to_target = [&](std::size_t index) {
        const auto& point = state.patrol_points[index];
        return std::hypot(
            point.x - state.last_known_player_x,
            point.z - state.last_known_player_z);
    };
    if (next == state.route_index ||
        distance_to_target(next) >= distance_to_target(state.route_index)) {
        state.route_active = false;
        state.route_finished = true;
        return;
    }
    state.route_index = next;
}

ActorMotion actorMotion(NpcLocomotion locomotion) noexcept {
    switch (locomotion) {
    case NpcLocomotion::walk: return ActorMotion::walk;
    case NpcLocomotion::run: return ActorMotion::run;
    case NpcLocomotion::backpedal: return ActorMotion::walk;
    case NpcLocomotion::strafe_left: return ActorMotion::strafe_left;
    case NpcLocomotion::strafe_right: return ActorMotion::strafe_right;
    case NpcLocomotion::turn_left: return ActorMotion::turn_left;
    case NpcLocomotion::turn_right: return ActorMotion::turn_right;
    case NpcLocomotion::stationary:
    default: return ActorMotion::idle;
    }
}

} // namespace

NpcCombatRange npcCombatRange(WeaponId weapon) noexcept {
    switch (weapon) {
    case WeaponId::unarmed:
    case WeaponId::taser:
        return NpcCombatRange{180.0, 560.0};
    case WeaponId::combat_shotgun:
    case WeaponId::shotgun:
    case WeaponId::flamethrower:
        return NpcCombatRange{420.0, 1150.0};
    case WeaponId::m_79:
    case WeaponId::fragmentation_grenade:
    case WeaponId::gas_grenade:
        return NpcCombatRange{1250.0, 2800.0};
    case WeaponId::nightvision_rifle:
    case WeaponId::sniper_rifle:
        return NpcCombatRange{1700.0, 5200.0};
    case WeaponId::pk_102:
    case WeaponId::m_16:
    case WeaponId::biz_2:
    case WeaponId::hk_5:
    case WeaponId::k3g4:
    case WeaponId::chopper_gun:
        return NpcCombatRange{800.0, 2600.0};
    default:
        return NpcCombatRange{600.0, 1900.0};
    }
}

void setNpcBehavior(NpcState& state, NpcBehavior behavior) noexcept {
    if (state.behavior == behavior) {
        return;
    }
    state.behavior = behavior;
    state.state_updates = 0U;
    state.animation_tick = 0U;
    state.locomotion = NpcLocomotion::stationary;
    state.lost_sight_updates = 0U;
    if (behavior == NpcBehavior::pursue) {
        state.route_active = false;
        state.route_finished = false;
    } else {
        state.route_active = false;
        state.route_finished = false;
    }
    if (behavior == NpcBehavior::attack) {
        state.combat_phase = NpcCombatPhase::acquire;
        state.phase_updates = 0U;
        state.burst_rounds_remaining = 0U;
    }
    if (behavior != NpcBehavior::take_cover) {
        state.cover_arrived = false;
        state.cover_wait_updates = 0U;
    }
}

NpcDecision updateNpcBrain(
    NpcState& state,
    const NpcPerception& perception) noexcept {
    NpcDecision decision{.desired_yaw = state.yaw};
    if (!state.active) {
        return decision;
    }

    state.movement_distance = 0.0;
    state.locomotion = NpcLocomotion::stationary;
    ++state.state_updates;
    if (state.behavior == NpcBehavior::attack) {
        ++state.phase_updates;
    }
    if (state.fire_cooldown_updates > 0U) {
        --state.fire_cooldown_updates;
    }
    if (state.fire_animation_updates > 0U) {
        --state.fire_animation_updates;
    }
    if (state.alert_memory_updates > 0U) {
        --state.alert_memory_updates;
    }
    if (state.retreat_updates > 0U) {
        --state.retreat_updates;
    }

    if (state.health == 0U) {
        if (state.behavior != NpcBehavior::dying && state.behavior != NpcBehavior::dead) {
            setNpcBehavior(state, NpcBehavior::dying);
        } else if (state.behavior == NpcBehavior::dying &&
                   state.state_updates >= npc_death_updates) {
            // Preserve the final DETH frame instead of restarting the fall.
            state.behavior = NpcBehavior::dead;
            state.state_updates = 0U;
        }
        return decision;
    }

    if (perception.damaged) {
        state.last_known_player_x = perception.player_x;
        state.last_known_player_z = perception.player_z;
        state.alert_memory_updates = npc_alert_memory_updates;
        if (perception.cover_available &&
            state.health * 3U <= state.maximum_health * 2U) {
            state.cover_x = perception.cover_x;
            state.cover_y = perception.cover_y;
            state.cover_z = perception.cover_z;
            state.cover_available = true;
            state.cover_arrived = false;
            state.cover_wait_updates = 0U;
            setNpcBehavior(state, NpcBehavior::take_cover);
        } else {
            if (state.health * 3U <= state.maximum_health * 2U) {
                state.retreat_updates = wounded_retreat_updates;
            }
            setNpcBehavior(state, NpcBehavior::hurt);
        }
    }
    if (state.disposition == NpcDisposition::neutral ||
        (state.disposition == NpcDisposition::ally && !perception.target_hostile)) {
        if (state.behavior == NpcBehavior::hurt && state.state_updates >= npc_hurt_updates) {
            setNpcBehavior(state, NpcBehavior::idle);
        }
        return decision;
    }

    if (state.disposition == NpcDisposition::hostile &&
        !perception.target_inside_zone) {
        state.alert_memory_updates = 0U;
        if (state.behavior != NpcBehavior::idle &&
            state.behavior != NpcBehavior::patrol &&
            state.behavior != NpcBehavior::return_home) {
            setNpcBehavior(state, NpcBehavior::return_home);
        }
    }

    const auto sees_player = canSeePlayer(perception);
    const auto notices_close_player = perception.target_inside_zone &&
        perception.player_visible &&
        perception.distance <= npc_close_detection_distance;
    const auto hears_player = perception.target_inside_zone &&
        perception.heard_weapon && perception.distance <= 8000.0;
    if (perception.target_inside_zone &&
        (sees_player || notices_close_player || hears_player || perception.ally_alerted)) {
        state.last_known_player_x = perception.player_x;
        state.last_known_player_z = perception.player_z;
        state.alert_memory_updates = npc_alert_memory_updates;
        if (state.behavior == NpcBehavior::idle || state.behavior == NpcBehavior::patrol ||
            state.behavior == NpcBehavior::search) {
            setNpcBehavior(state, NpcBehavior::alert);
        }
    }

    const auto desired_yaw = headingFromDirection(
        state.last_known_player_x - state.x,
        state.last_known_player_z - state.z);
    decision.desired_yaw = turnToward(state.yaw, desired_yaw);
    const auto& weapon = weaponCombatDefinition(state.weapon);
    const auto combat_range = npcCombatRange(state.weapon);
    const auto preferred_maximum = weapon.maximum_range == 0U
        ? combat_range.preferred_maximum
        : std::min(combat_range.preferred_maximum, static_cast<double>(weapon.maximum_range));

    switch (state.behavior) {
    case NpcBehavior::idle:
        if (state.patrol_points.size() > 1U) {
            setNpcBehavior(state, NpcBehavior::patrol);
        } else {
            decision.desired_yaw = state.yaw;
        }
        break;
    case NpcBehavior::patrol: {
        if (state.patrol_points.empty()) {
            setNpcBehavior(state, NpcBehavior::idle);
            break;
        }
        const auto& waypoint = state.patrol_points[
            std::min(state.patrol_index, state.patrol_points.size() - 1U)];
        const auto delta_x = waypoint.x - state.x;
        const auto delta_z = waypoint.z - state.z;
        const auto distance = std::hypot(delta_x, delta_z);
        const auto waypoint_yaw = headingFromDirection(delta_x, delta_z);
        decision.desired_yaw = turnToward(state.yaw, waypoint_yaw);
        if (distance <= patrol_arrival_distance) {
            decision.advance_patrol = true;
        } else if (std::abs(signedHeadingDelta(state.yaw, waypoint_yaw)) <=
                   attack_alignment * 2) {
            decision.forward_distance = native_walk_distance_per_update;
            state.locomotion = NpcLocomotion::walk;
        }
        break;
    }
    case NpcBehavior::alert:
        if (state.state_updates >= npc_reaction_updates) {
            setNpcBehavior(
                state,
                perception.player_visible && perception.distance <= preferred_maximum * 0.9
                    ? NpcBehavior::attack
                    : NpcBehavior::pursue);
        }
        break;
    case NpcBehavior::pursue: {
        if (perception.player_visible) {
            state.route_active = false;
            state.route_finished = false;
        } else if (!state.route_active && !state.route_finished) {
            initializePursuitRoute(state);
        }
        auto target_x = state.last_known_player_x;
        auto target_z = state.last_known_player_z;
        if (perception.player_visible) {
            target_x = perception.player_x;
            target_z = perception.player_z;
        } else if (state.route_active) {
            const auto& point = state.patrol_points[state.route_index];
            target_x = point.x;
            target_z = point.z;
        }
        const auto delta_x = target_x - state.x;
        const auto delta_z = target_z - state.z;
        const auto target_distance = std::hypot(delta_x, delta_z);
        const auto target_yaw = headingFromDirection(delta_x, delta_z);
        decision.desired_yaw = turnToward(state.yaw, target_yaw);
        if (perception.player_visible && perception.distance <= preferred_maximum * 0.9 &&
            state.retreat_updates == 0U) {
            setNpcBehavior(state, NpcBehavior::attack);
        } else if (!perception.player_visible && target_distance <= pursuit_arrival_distance) {
            if (state.route_active) {
                advancePursuitRoute(state);
            } else {
                setNpcBehavior(state, NpcBehavior::search);
            }
        } else if (state.alert_memory_updates == 0U) {
            setNpcBehavior(state, NpcBehavior::search);
        } else if (std::abs(signedHeadingDelta(state.yaw, target_yaw)) <=
                   attack_alignment * 3) {
            decision.forward_distance = native_run_distance_per_update;
            state.locomotion = NpcLocomotion::run;
        }
        break;
    }
    case NpcBehavior::return_home: {
        const auto delta_x = state.home_x - state.x;
        const auto delta_z = state.home_z - state.z;
        const auto distance = std::hypot(delta_x, delta_z);
        if (distance <= patrol_arrival_distance) {
            setNpcBehavior(
                state,
                state.patrol_points.size() > 1U
                    ? NpcBehavior::patrol
                    : NpcBehavior::idle);
            break;
        }
        const auto home_yaw = headingFromDirection(delta_x, delta_z);
        decision.desired_yaw = turnToward(state.yaw, home_yaw);
        if (std::abs(signedHeadingDelta(state.yaw, home_yaw)) <=
            attack_alignment * 3) {
            decision.forward_distance = native_run_distance_per_update;
            state.locomotion = NpcLocomotion::run;
        }
        break;
    }
    case NpcBehavior::search: {
        if (perception.player_visible) {
            setNpcBehavior(state, NpcBehavior::alert);
            break;
        }
        if (state.state_updates == npc_search_updates / 2U) {
            state.avoidance_side = -state.avoidance_side;
        }
        const auto search_yaw = normalizeHeading(
            static_cast<std::int64_t>(state.yaw) + state.avoidance_side * 256);
        decision.desired_yaw = turnToward(state.yaw, search_yaw);
        state.locomotion = state.avoidance_side < 0
            ? NpcLocomotion::turn_left
            : NpcLocomotion::turn_right;
        if (state.state_updates >= npc_search_updates) {
            setNpcBehavior(
                state,
                state.patrol_points.size() > 1U ? NpcBehavior::patrol : NpcBehavior::idle);
        }
        break;
    }
    case NpcBehavior::take_cover: {
        const auto delta_x = state.cover_x - state.x;
        const auto delta_z = state.cover_z - state.z;
        const auto distance = std::hypot(delta_x, delta_z);
        const auto cover_yaw = headingFromDirection(delta_x, delta_z);
        decision.desired_yaw = turnToward(state.yaw, cover_yaw);
        if (distance > cover_arrival_distance) {
            decision.forward_distance = native_run_distance_per_update;
            state.locomotion = NpcLocomotion::run;
        } else {
            state.cover_arrived = true;
            ++state.cover_wait_updates;
            decision.desired_yaw = turnToward(state.yaw, desired_yaw);
            if (state.magazine_capacity != 0U && state.magazine == 0U &&
                state.reserve_ammo != 0U) {
                setNpcBehavior(state, NpcBehavior::reloading);
            } else if (perception.player_visible &&
                       state.cover_wait_updates >= cover_peek_updates) {
                setNpcBehavior(state, NpcBehavior::attack);
            } else if (state.cover_wait_updates >= npc_cover_hold_updates) {
                setNpcBehavior(state, NpcBehavior::pursue);
            }
        }
        break;
    }
    case NpcBehavior::attack:
        if (!perception.player_visible) {
            ++state.lost_sight_updates;
            if (state.lost_sight_updates >= npc_lost_sight_grace_updates) {
                setNpcBehavior(state, NpcBehavior::pursue);
            }
            break;
        }
        state.lost_sight_updates = 0U;
        if (perception.distance > preferred_maximum * 1.25) {
            setNpcBehavior(state, NpcBehavior::pursue);
            break;
        }
        if (state.magazine_capacity != 0U && state.magazine == 0U) {
            if (state.reserve_ammo != 0U) {
                setNpcBehavior(state, NpcBehavior::reloading);
            }
            break;
        }
        if (state.retreat_updates != 0U &&
            state.combat_phase != NpcCombatPhase::retreat) {
            setCombatPhase(state, NpcCombatPhase::retreat);
        }
        switch (state.combat_phase) {
        case NpcCombatPhase::acquire:
            if (perception.distance < combat_range.minimum) {
                setCombatPhase(state, NpcCombatPhase::retreat);
            } else if (state.phase_updates >= npc_aim_settle_updates) {
                setCombatPhase(state, NpcCombatPhase::aim);
            }
            break;
        case NpcCombatPhase::aim:
            if (perception.distance < combat_range.minimum) {
                setCombatPhase(state, NpcCombatPhase::retreat);
            } else if (perception.distance > preferred_maximum) {
                decision.forward_distance = native_walk_distance_per_update;
                state.locomotion = NpcLocomotion::walk;
            } else if (state.phase_updates >= combat_aim_updates && weapon.fires()) {
                state.random_state = state.random_state * 1664525U + 1013904223U;
                state.burst_rounds_remaining = weapon.automatic()
                    ? 3U + ((state.random_state >> 29U) & 3U)
                    : 1U;
                setCombatPhase(state, NpcCombatPhase::burst);
            }
            break;
        case NpcCombatPhase::burst:
            if (std::abs(signedHeadingDelta(state.yaw, desired_yaw)) <= attack_alignment &&
                state.fire_cooldown_updates == 0U && state.burst_rounds_remaining != 0U) {
                decision.fire = true;
                ++state.shot_serial;
                --state.burst_rounds_remaining;
                if (state.magazine_capacity != 0U && state.magazine != 0U) {
                    --state.magazine;
                }
                state.fire_cooldown_updates = std::max(
                    weapon.fire_interval_updates,
                    weapon.automatic() ? 1U : 4U);
                state.fire_animation_updates = playerAnimationTiming(PlayerAnimationRequest{
                    ActorMotion::idle,
                    PlayerUpperAction::fire,
                    weaponStance(state.weapon),
                    state.weapon,
                }).fire_updates;
                state.animation_tick = 0U;
                if (state.burst_rounds_remaining == 0U ||
                    (state.magazine_capacity != 0U && state.magazine == 0U)) {
                    setCombatPhase(state, NpcCombatPhase::recover);
                }
            } else if (state.phase_updates >= combat_burst_timeout_updates) {
                setCombatPhase(state, NpcCombatPhase::recover);
            }
            break;
        case NpcCombatPhase::recover:
            if (perception.distance < combat_range.minimum) {
                setCombatPhase(state, NpcCombatPhase::retreat);
            } else if (state.phase_updates >= combat_recover_updates) {
                state.reposition_direction = ((state.object + state.shot_serial) & 1U) == 0U
                    ? -1
                    : 1;
                setCombatPhase(state, NpcCombatPhase::reposition);
            }
            break;
        case NpcCombatPhase::reposition:
            if (perception.distance < combat_range.minimum) {
                setCombatPhase(state, NpcCombatPhase::retreat);
            } else if (state.phase_updates >= combat_reposition_updates) {
                setCombatPhase(state, NpcCombatPhase::aim);
            } else {
                decision.strafe_distance =
                    state.reposition_direction * native_walk_distance_per_update;
                state.locomotion = state.reposition_direction < 0
                    ? NpcLocomotion::strafe_left
                    : NpcLocomotion::strafe_right;
                if (state.blocked_updates == combat_reposition_blocked_updates) {
                    state.reposition_direction = -state.reposition_direction;
                }
            }
            break;
        case NpcCombatPhase::retreat:
            if ((state.retreat_updates == 0U &&
                 perception.distance >= combat_range.minimum * 1.35) ||
                state.phase_updates >= combat_retreat_updates) {
                setCombatPhase(state, NpcCombatPhase::aim);
            } else {
                decision.desired_yaw = turnToward(
                    state.yaw,
                    normalizeHeading(static_cast<std::int64_t>(desired_yaw) +
                                     heading_angle_units / 2));
                decision.forward_distance = native_run_distance_per_update;
                state.locomotion = NpcLocomotion::run;
            }
            break;
        }
        break;
    case NpcBehavior::reloading:
        if (state.state_updates >= playerAnimationTiming(PlayerAnimationRequest{
                ActorMotion::idle,
                PlayerUpperAction::reload,
                weaponStance(state.weapon),
                state.weapon,
            }).reload_updates) {
            const auto rounds = std::min(state.magazine_capacity, state.reserve_ammo);
            state.magazine = rounds;
            state.reserve_ammo = static_cast<std::uint16_t>(state.reserve_ammo - rounds);
            setNpcBehavior(state, NpcBehavior::attack);
        }
        break;
    case NpcBehavior::hurt:
        if (state.state_updates >= npc_hurt_updates) {
            setNpcBehavior(state, NpcBehavior::alert);
        }
        break;
    case NpcBehavior::surrender:
    case NpcBehavior::dying:
    case NpcBehavior::dead:
        decision.desired_yaw = state.yaw;
        break;
    }
    return decision;
}

bool npcDispositionsOppose(
    NpcDisposition first,
    NpcDisposition second) noexcept {
    return (first == NpcDisposition::hostile && second == NpcDisposition::ally) ||
        (first == NpcDisposition::ally && second == NpcDisposition::hostile);
}

NpcAnimationRequest npcAnimationRequest(const NpcState& state) noexcept {
    if (state.scripted_climbing && state.health != 0U) {
        const auto action = state.legacy_presentation_valid &&
                state.legacy_presentation_code == 10U
            ? NpcAnimationAction::jump
            : state.legacy_presentation_valid &&
                    state.legacy_presentation_code == 12U
                ? NpcAnimationAction::fall
                : NpcAnimationAction::climb;
        return NpcAnimationRequest{
            action,
            weaponStance(state.weapon),
            static_cast<std::uint8_t>(state.object & 3U),
            state.weapon,
            action == NpcAnimationAction::climb
                ? ActorMotion::climb
                : ActorMotion::idle,
        };
    }
    if (state.scripted_ingress && state.health != 0U) {
        return NpcAnimationRequest{
            NpcAnimationAction::run,
            weaponStance(state.weapon),
            static_cast<std::uint8_t>(state.object & 3U),
            state.weapon,
            ActorMotion::run,
        };
    }
    if (state.scripted_low_locomotion && state.health != 0U &&
        state.movement_distance > 0.0) {
        return NpcAnimationRequest{
            NpcAnimationAction::walk,
            weaponStance(state.weapon),
            static_cast<std::uint8_t>(state.object & 3U),
            state.weapon,
            ActorMotion::crouch_walk,
        };
    }
    if (state.scripted_intro_agent && state.scripted_intro_spawned &&
        !state.scripted_opening_arrived && state.health != 0U &&
        state.movement_distance > 0.0) {
        return NpcAnimationRequest{
            NpcAnimationAction::run,
            weaponStance(state.weapon),
            static_cast<std::uint8_t>(state.object & 3U),
            state.weapon,
            ActorMotion::run,
        };
    }
    if (state.scripted_kneeling && state.health != 0U) {
        const auto action = state.behavior == NpcBehavior::attack
            ? state.fire_animation_updates > 0U
                ? NpcAnimationAction::fire
                : NpcAnimationAction::aim
            : NpcAnimationAction::kneel;
        return NpcAnimationRequest{
            action,
            weaponStance(state.weapon),
            static_cast<std::uint8_t>(state.object & 3U),
            state.weapon,
            ActorMotion::kneel,
        };
    }
    auto action = NpcAnimationAction::idle;
    auto motion = actorMotion(state.locomotion);
    switch (state.behavior) {
    case NpcBehavior::patrol:
        action = NpcAnimationAction::walk;
        motion = ActorMotion::walk;
        break;
    case NpcBehavior::pursue:
    case NpcBehavior::return_home:
    case NpcBehavior::take_cover:
        action = state.cover_arrived ? NpcAnimationAction::aim : NpcAnimationAction::run;
        if (!state.cover_arrived) {
            motion = ActorMotion::run;
        }
        break;
    case NpcBehavior::alert:
    case NpcBehavior::search:
        action = state.weapon == WeaponId::unarmed
            ? NpcAnimationAction::idle
            : NpcAnimationAction::aim;
        break;
    case NpcBehavior::attack:
        if (state.combat_phase == NpcCombatPhase::retreat) {
            action = NpcAnimationAction::run;
            motion = ActorMotion::run;
        } else {
            action = state.fire_animation_updates > 0U
                ? NpcAnimationAction::fire
                : NpcAnimationAction::aim;
        }
        break;
    case NpcBehavior::reloading: action = NpcAnimationAction::reload; break;
    case NpcBehavior::hurt:
        action = (state.shot_serial & 1U) == 0U
            ? NpcAnimationAction::hit_left
            : NpcAnimationAction::hit_right;
        break;
    case NpcBehavior::surrender: action = NpcAnimationAction::surrender; break;
    case NpcBehavior::dying:
        switch (state.death_kind) {
        case NpcDeathKind::normal: action = NpcAnimationAction::death; break;
        case NpcDeathKind::fire: action = NpcAnimationAction::fire_death; break;
        case NpcDeathKind::electrical:
            action = NpcAnimationAction::electrical_death;
            break;
        }
        break;
    case NpcBehavior::dead:
        switch (state.death_kind) {
        case NpcDeathKind::normal: action = NpcAnimationAction::dead; break;
        case NpcDeathKind::fire: action = NpcAnimationAction::fire_dead; break;
        case NpcDeathKind::electrical:
            action = NpcAnimationAction::electrical_dead;
            break;
        }
        break;
    case NpcBehavior::idle:
    default: action = NpcAnimationAction::idle; break;
    }
    return NpcAnimationRequest{
        action,
        weaponStance(state.weapon),
        static_cast<std::uint8_t>(state.object & 3U),
        state.weapon,
        motion,
    };
}

NpcDangerSignal updateNpcDanger(
    NpcState& state,
    const NpcPerception& perception,
    bool exact_aim,
    bool player_rolled) noexcept {
    if (player_rolled) {
        state.danger_evade_updates = npc_danger_roll_evasion_updates;
        state.danger_lock = static_cast<std::uint16_t>(
            state.danger_lock > npc_danger_roll_reduction
                ? state.danger_lock - npc_danger_roll_reduction
                : 0U);
    } else if (state.danger_evade_updates > 0U) {
        --state.danger_evade_updates;
    }

    auto target = 0U;
    const auto alive_hostile = state.active && state.health != 0U &&
        state.disposition == NpcDisposition::hostile;
    const auto tracks_player = perception.player_visible &&
        (perception.distance <= npc_close_detection_distance ||
         std::abs(perception.signed_player_angle) <= npc_sight_half_angle ||
         (state.behavior != NpcBehavior::idle && state.behavior != NpcBehavior::patrol));
    if (alive_hostile && tracks_player) {
        // The retail HUD combines the strongest actor's 0..0x1000 aim value.
        // Its broad proximity band is 0xc80 world units; aim acquisition then
        // raises that value toward the exact-lock endpoint.
        const auto proximity = std::clamp(
            1.0 - perception.distance / npc_alert_share_distance,
            0.0,
            1.0);
        target = static_cast<unsigned int>(std::lround(proximity * 3000.0));
        if (state.behavior == NpcBehavior::attack) {
            switch (state.combat_phase) {
            case NpcCombatPhase::acquire: {
                const auto progress = std::min(state.phase_updates, npc_aim_settle_updates);
                target = std::max(
                    target,
                    1536U + progress * 1024U / npc_aim_settle_updates);
                break;
            }
            case NpcCombatPhase::aim: {
                const auto progress = std::min(state.phase_updates, combat_aim_updates);
                target = std::max(
                    target,
                    2560U + progress * 1280U / combat_aim_updates);
                break;
            }
            case NpcCombatPhase::burst:
                target = exact_aim ? npc_danger_maximum : 3840U;
                break;
            case NpcCombatPhase::recover:
            case NpcCombatPhase::reposition:
                target = std::max(target, 2048U);
                break;
            case NpcCombatPhase::retreat:
                break;
            }
        }
    }

    if (state.danger_evade_updates != 0U) {
        target = std::min(target, 2048U);
        exact_aim = false;
    }
    target = std::min<unsigned int>(target, npc_danger_maximum);
    if (exact_aim && target == npc_danger_maximum) {
        state.danger_lock = npc_danger_maximum;
    } else if (state.danger_lock < target) {
        state.danger_lock = static_cast<std::uint16_t>(std::min<unsigned int>(
            state.danger_lock + danger_rise_per_update,
            target));
    } else if (state.danger_lock > target) {
        state.danger_lock = static_cast<std::uint16_t>(
            state.danger_lock > target + danger_fall_per_update
                ? state.danger_lock - danger_fall_per_update
                : target);
    }

    return NpcDangerSignal{
        static_cast<std::uint8_t>(std::min<unsigned int>(
            (static_cast<unsigned int>(state.danger_lock) * 100U +
             npc_danger_maximum / 2U) /
                npc_danger_maximum,
            100U)),
        exact_aim && state.danger_lock == npc_danger_maximum,
    };
}

unsigned int npcHitChance(
    const NpcState& state,
    double distance,
    double maximum_range,
    bool target_moving) noexcept {
    const auto distance_ratio = maximum_range <= 0.0
        ? 1.0
        : std::clamp(distance / maximum_range, 0.0, 1.0);
    auto chance = 24.0 - distance_ratio * 14.0;
    if (target_moving) {
        chance -= 6.0;
    }
    if (state.behavior == NpcBehavior::attack &&
        (state.combat_phase == NpcCombatPhase::acquire ||
         state.combat_phase == NpcCombatPhase::aim)) {
        const auto settle = static_cast<double>(state.phase_updates) /
            static_cast<double>(npc_aim_settle_updates + combat_aim_updates);
        chance *= std::clamp(settle, 0.25, 1.0);
    }
    return static_cast<unsigned int>(std::clamp(chance, 4.0, 24.0));
}

} // namespace sf::game
