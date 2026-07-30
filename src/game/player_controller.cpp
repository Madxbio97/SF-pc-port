#include "sf/game/player_controller.hpp"

#include <algorithm>
#include <cmath>

namespace sf::game {
namespace {

constexpr double input_dead_zone = 0.0001;
// Equivalent response after three former 60 Hz host slices. PlayerController
// now advances once per retail 20 Hz simulation tick.
constexpr double chase_position_follow = 0.626752;
constexpr double chase_target_follow = 0.737856;

} // namespace

PlayerController::PlayerController(
    ChaseCameraConfiguration chase_camera,
    FirstPersonCameraConfiguration aim_camera) noexcept
    : camera_rig_(chase_camera), aim_camera_rig_(aim_camera) {
  updateCamera();
}

void PlayerController::setRootMotionTracks(
    std::span<const assets::HmdRootMotionFrame> walking,
    std::span<const assets::HmdRootMotionFrame> running,
    std::span<const assets::HmdRootMotionFrame> rolling,
    std::span<const assets::HmdRootMotionFrame> crouch_walking) {
  walking_root_motion_.assign(walking.begin(), walking.end());
  running_root_motion_.assign(running.begin(), running.end());
  rolling_root_motion_.assign(rolling.begin(), rolling.end());
  crouch_root_motion_.assign(crouch_walking.begin(), crouch_walking.end());
}

void PlayerController::setStrafeRootMotionTracks(
    std::span<const assets::HmdRootMotionFrame> left,
    std::span<const assets::HmdRootMotionFrame> right) {
  strafe_left_root_motion_.assign(left.begin(), left.end());
  strafe_right_root_motion_.assign(right.begin(), right.end());
}

void PlayerController::setAdditionalRollRootMotionTracks(
    std::span<const assets::HmdRootMotionFrame> standing_long_gun,
    std::span<const assets::HmdRootMotionFrame> kneeling_unarmed,
    std::span<const assets::HmdRootMotionFrame> kneeling_sidearm,
    std::span<const assets::HmdRootMotionFrame> kneeling_long_gun) {
  standing_long_gun_roll_root_motion_.assign(standing_long_gun.begin(),
                                             standing_long_gun.end());
  kneeling_unarmed_roll_root_motion_.assign(kneeling_unarmed.begin(),
                                            kneeling_unarmed.end());
  kneeling_sidearm_roll_root_motion_.assign(kneeling_sidearm.begin(),
                                            kneeling_sidearm.end());
  kneeling_long_gun_roll_root_motion_.assign(kneeling_long_gun.begin(),
                                             kneeling_long_gun.end());
}

const std::vector<assets::HmdRootMotionFrame> &
PlayerController::rollingRootMotion() const noexcept {
  if (stance_ == PlayerStanceState::kneeling) {
    switch (weapon_stance_) {
    case PlayerWeaponStance::sidearm:
      if (!kneeling_sidearm_roll_root_motion_.empty()) {
        return kneeling_sidearm_roll_root_motion_;
      }
      break;
    case PlayerWeaponStance::long_gun:
      if (!kneeling_long_gun_roll_root_motion_.empty()) {
        return kneeling_long_gun_roll_root_motion_;
      }
      break;
    case PlayerWeaponStance::unarmed:
    default:
      if (!kneeling_unarmed_roll_root_motion_.empty()) {
        return kneeling_unarmed_roll_root_motion_;
      }
      break;
    }
  } else if (weapon_stance_ == PlayerWeaponStance::long_gun &&
             !standing_long_gun_roll_root_motion_.empty()) {
    return standing_long_gun_roll_root_motion_;
  }
  return rolling_root_motion_;
}

unsigned int PlayerController::rollDurationUpdates() const noexcept {
  const auto &root_motion = rollingRootMotion();
  return root_motion.empty()
             ? minimum_roll_updates
             : std::max(minimum_roll_updates,
                        static_cast<unsigned int>(root_motion.size()) *
                            updates_per_animation_frame);
}

void PlayerController::reset(const PlayerState &spawn) noexcept {
  state_ = spawn;
  locomotion_ = PlayerLocomotionState::idle;
  action_ = PlayerActionState::ready;
  stance_ = PlayerStanceState::standing;
  aim_ = PlayerAimState::chase;
  weapon_switch_ = PlayerWeaponSwitchState::none;
  animation_tick_ = 0U;
  action_animation_tick_ = 0U;
  animation_phase_ = 0U;
  action_updates_remaining_ = 0U;
  roll_direction_ = PlayerRollDirection::forward;
  motion_move_ = 0.0;
  motion_strafe_ = 0.0;
  motion_turn_ = 0.0;
  aim_heading_ = state_.yaw;
  camera_pitch_ = 0.0;
  direct_weapon_.reset();
  camera_mode_ = PlayerCameraMode::chase;
  camera_initialized_ = false;
  updateCamera();
}

void PlayerController::synchronizeScriptedPose(
    const PlayerState &pose) noexcept {
  state_ = pose;
  state_.yaw = normalizeHeading(state_.yaw);
  motion_move_ = 0.0;
  motion_strafe_ = 0.0;
  motion_turn_ = 0.0;
  if (aim_ != PlayerAimState::first_person) {
    aim_heading_ = state_.yaw;
  }
  updateCamera();
}

void PlayerController::synchronizeFirstPersonRoot(
    const PlayerState &pose) noexcept {
  // Retail can publish first-person transition samples before its motion
  // controller has resolved a floor contact. Their motion Y belongs to the
  // temporary aim pose, not to Gabe's world root. Accept planar collision and
  // heading immediately, but retain the last grounded height until the bridge
  // supplies an authoritative contact-space Y.
  const auto stable_y = state_.y;
  const auto stable_grounded = state_.grounded;
  state_.x = pose.x;
  state_.z = pose.z;
  state_.yaw = pose.yaw;
  const auto valid_grounded_height =
      pose.grounded &&
      (!stable_grounded ||
       std::abs(pose.y - stable_y) <= maximum_first_person_root_height_step);
  if (valid_grounded_height) {
    state_.y = pose.y;
    state_.grounded = true;
  } else {
    state_.y = stable_y;
    state_.grounded = stable_grounded;
  }
  state_.yaw = normalizeHeading(state_.yaw);
  updateCamera();
}

bool PlayerController::tryMove(PlayerMovementResolver &movement,
                               double direction_x, double direction_z,
                               double distance) {
  const auto delta_x = direction_x * distance;
  const auto delta_z = direction_z * distance;
  auto moved = movement.tryMove(state_, state_.x + delta_x, state_.z + delta_z);
  const auto diagonal = std::abs(delta_x) > input_dead_zone &&
                        std::abs(delta_z) > input_dead_zone;
  if (!moved && diagonal) {
    moved = movement.tryMove(state_, state_.x + delta_x, state_.z);
  }
  if (!moved && diagonal) {
    moved = movement.tryMove(state_, state_.x, state_.z + delta_z);
  }
  return moved;
}

void PlayerController::beginAction(PlayerActionState action,
                                   unsigned int duration) noexcept {
  action_ = action;
  action_updates_remaining_ = duration;
  action_animation_tick_ = 0U;
}

void PlayerController::updateAction(const PlayerInput &input) noexcept {
  if (action_updates_remaining_ > 0U) {
    --action_updates_remaining_;
    if (action_updates_remaining_ == 0U) {
      const auto completed_action = action_;
      action_ = PlayerActionState::ready;
      action_animation_tick_ = 0U;
      if (completed_action == PlayerActionState::weapon_switching) {
        weapon_switch_ = PlayerWeaponSwitchState::none;
      }
    }
  }
  if (actionLocked()) {
    return;
  }
  if (action_ == PlayerActionState::reloading ||
      action_ == PlayerActionState::weapon_switching) {
    return;
  }
  if (input.roll) {
    const auto horizontal = std::abs(input.strafe) > std::abs(input.move);
    if (horizontal) {
      roll_direction_ = input.strafe < 0.0 ? PlayerRollDirection::left
                                           : PlayerRollDirection::right;
    } else {
      // Retail PCHAN has no backward roll and the action always advances
      // along the native clip's local forward axis when not strafing.
      roll_direction_ = PlayerRollDirection::forward;
    }
    beginAction(PlayerActionState::rolling, rollDurationUpdates());
  } else if (input.quick_turn) {
    state_.yaw = normalizeHeading(static_cast<std::int64_t>(state_.yaw) + 2048);
    beginAction(PlayerActionState::quick_turning,
                animation_timing_.quick_turn_updates);
  } else if (input.kneel) {
    if (stance_ == PlayerStanceState::standing) {
      stance_ = PlayerStanceState::kneeling;
      beginAction(PlayerActionState::kneeling_down,
                  animation_timing_.kneel_down_updates);
    } else {
      stance_ = PlayerStanceState::standing;
      beginAction(PlayerActionState::standing_up,
                  animation_timing_.stand_up_updates);
    }
  } else if (input.reload) {
    beginAction(PlayerActionState::reloading, animation_timing_.reload_updates);
  } else if (input.interact) {
    beginAction(PlayerActionState::interacting,
                animation_timing_.interact_updates);
  } else if (input.fire_pressed || input.fire_held) {
    beginAction(PlayerActionState::firing, animation_timing_.fire_updates);
  } else if (weapon_switch_ != PlayerWeaponSwitchState::none) {
    if (action_ != PlayerActionState::weapon_switching) {
      beginAction(PlayerActionState::weapon_switching,
                  animation_timing_.draw_updates);
    }
  }
}

void PlayerController::setLocomotion(
    PlayerLocomotionState locomotion) noexcept {
  if (locomotion_ == locomotion) {
    return;
  }
  locomotion_ = locomotion;
  animation_tick_ = 0U;
  animation_phase_ = 0U;
}

void PlayerController::update(const PlayerInput &input,
                              PlayerMovementResolver &movement) {
  const auto previous_actor_motion = actorMotion();
  const auto was_manual_aim = aim_ == PlayerAimState::first_person;
  direct_weapon_ = input.direct_weapon;
  if (direct_weapon_) {
    weapon_switch_ = PlayerWeaponSwitchState::direct;
  } else if (input.weapon_menu_delta != 0) {
    weapon_switch_ = input.weapon_menu_delta > 0
                         ? PlayerWeaponSwitchState::next
                         : PlayerWeaponSwitchState::previous;
  } else if (input.quick_weapon || input.next_weapon != input.previous_weapon) {
    weapon_switch_ = input.quick_weapon || input.next_weapon
                         ? PlayerWeaponSwitchState::next
                         : PlayerWeaponSwitchState::previous;
  }
  updateAction(input);

  const auto rolling = action_ == PlayerActionState::rolling;
  const auto movement_locked = actionLocked();
  const auto stance_transition =
      action_ == PlayerActionState::kneeling_down ||
      action_ == PlayerActionState::standing_up;
  const auto manual_aim = input.aim && !actionLocksManualAim();
  if (manual_aim && !was_manual_aim) {
    aim_heading_ = state_.yaw;
  }
  // First-person aim receives one already-composed look stream (lossless
  // relative mouse plus right-stick rate). Body yaw and collision root remain
  // fixed until aim is released.
  const auto turn = manual_aim ? 0.0 : std::clamp(input.turn, -1.0, 1.0);
  const auto look_turn = std::clamp(
      input.look_yaw / static_cast<double>(turn_units_per_update), -1.0, 1.0);
  motion_turn_ = movement_locked || manual_aim
                     ? 0.0
                     : (std::abs(turn) > input_dead_zone ? turn : look_turn);
  if (!movement_locked && std::abs(turn) > input_dead_zone) {
    const auto turn_delta = static_cast<std::int64_t>(
        std::lround(turn * static_cast<double>(turn_units_per_update)));
    state_.yaw =
        normalizeHeading(static_cast<std::int64_t>(state_.yaw) + turn_delta);
  }
  if ((!movement_locked || stance_transition) &&
      std::abs(input.look_yaw) > input_dead_zone) {
    auto &heading = manual_aim ? aim_heading_ : state_.yaw;
    heading = normalizeHeading(
        static_cast<std::int64_t>(heading) +
        static_cast<std::int64_t>(std::lround(input.look_yaw)));
  }

  // Leaving manual aim commits the final sight heading to Gabe's body.  The
  // old path discarded aim_heading_ here, so chase view resumed from the yaw
  // captured before aiming and made Gabe visibly snap to the wrong direction.
  if (was_manual_aim && !manual_aim) {
    state_.yaw = aim_heading_;
  }

  // This is the low-level movement gate.  Keep it independent of the platform
  // bindings and guest PAD bridge so no simultaneous aim+WASD edge can reach
  // collision resolution, even if a caller forgets to sanitize its input.
  const auto movement_allowed = !movement_locked && !manual_aim;
  const auto move = movement_allowed ? std::clamp(input.move, -1.0, 1.0) : 0.0;
  const auto strafe =
      movement_allowed ? std::clamp(input.strafe, -1.0, 1.0) : 0.0;
  motion_move_ = move;
  motion_strafe_ = strafe;
  const auto has_forward_motion = std::abs(move) > input_dead_zone;
  const auto has_strafe_motion = std::abs(strafe) > input_dead_zone;
  auto requested_locomotion = PlayerLocomotionState::idle;
  if (has_forward_motion || has_strafe_motion) {
    if (stance_ == PlayerStanceState::kneeling) {
      requested_locomotion = PlayerLocomotionState::crouch_walking;
    } else if (has_strafe_motion && !has_forward_motion) {
      requested_locomotion = PlayerLocomotionState::strafing;
    } else if (input.run && move > 0.0 && !input.aim) {
      requested_locomotion = PlayerLocomotionState::running;
    } else {
      requested_locomotion = PlayerLocomotionState::walking;
    }
  }
  setLocomotion(requested_locomotion);

  if (requested_locomotion != PlayerLocomotionState::idle) {
    const auto &root_motion =
        requested_locomotion == PlayerLocomotionState::running
            ? running_root_motion_
        : requested_locomotion == PlayerLocomotionState::crouch_walking &&
                !crouch_root_motion_.empty()
            ? crouch_root_motion_
            : walking_root_motion_;
    const auto &strafe_root_motion =
        strafe < 0.0 ? strafe_left_root_motion_ : strafe_right_root_motion_;
    const auto native_distance =
        requested_locomotion == PlayerLocomotionState::strafing &&
                !strafe_root_motion.empty()
            ? rootMotionPlanarDistance(strafe_root_motion, animation_tick_,
                                       updates_per_animation_frame)
            : rootMotionForwardDistance(root_motion, animation_tick_,
                                        updates_per_animation_frame);
    const auto movement_heading = manual_aim ? aim_heading_ : state_.yaw;
    const auto basis = headingBasis(movement_heading);
    auto direction_x = basis.forward.x * move + basis.right.x * strafe;
    auto direction_z = basis.forward.z * move + basis.right.z * strafe;
    const auto direction_length = std::hypot(direction_x, direction_z);
    if (direction_length > 1.0) {
      direction_x /= direction_length;
      direction_z /= direction_length;
    }
    if (!tryMove(movement, direction_x, direction_z, native_distance)) {
      setLocomotion(PlayerLocomotionState::idle);
    }
  }

  if (rolling) {
    const auto &roll_root_motion = rollingRootMotion();
    const auto basis = headingBasis(state_.yaw);
    auto direction = basis.forward;
    switch (roll_direction_) {
    case PlayerRollDirection::left:
      direction.x = -basis.right.x;
      direction.z = -basis.right.z;
      break;
    case PlayerRollDirection::right:
      direction = basis.right;
      break;
    case PlayerRollDirection::forward:
    default:
      break;
    }
    const auto distance = roll_root_motion.empty()
                              ? 18.0
                              : rootMotionForwardDistance(
                                    roll_root_motion, action_animation_tick_,
                                    updates_per_animation_frame);
    static_cast<void>(tryMove(movement, direction.x, direction.z, distance));
    setLocomotion(PlayerLocomotionState::idle);
  }

  aim_ = manual_aim ? PlayerAimState::first_person : PlayerAimState::chase;
  if (aim_ == PlayerAimState::first_person) {
    camera_pitch_ = std::clamp(camera_pitch_ + input.look_pitch,
                               -maximum_first_person_aim_pitch,
                               maximum_first_person_aim_pitch);
  } else {
    aim_heading_ = state_.yaw;
    camera_pitch_ = 0.0;
  }

  if (actorMotion() != previous_actor_motion) {
    animation_tick_ = 0U;
    animation_phase_ = 0U;
  }
  updateCamera();
}

void PlayerController::advanceAnimationClock() noexcept {
  animation_phase_ = (animation_phase_ + 1U) % updates_per_animation_frame;
  if (animation_phase_ == 0U) {
    ++animation_tick_;
    if (action_ != PlayerActionState::ready) {
      ++action_animation_tick_;
    }
  }
}

ActorMotion PlayerController::actorMotion() const noexcept {
  switch (action_) {
  case PlayerActionState::rolling:
    return stance_ == PlayerStanceState::kneeling ? ActorMotion::kneel_roll
                                                  : ActorMotion::roll;
  case PlayerActionState::kneeling_down:
    return ActorMotion::kneel_down;
  case PlayerActionState::standing_up:
    return ActorMotion::stand_up;
  case PlayerActionState::quick_turning:
    return ActorMotion::quick_turn;
  case PlayerActionState::interacting:
    // PCHAN has contextual CLIMBA/KIKDR full-body clips, but no generic
    // use clip. CLIMBA is the safe locomotion-preserving fallback until a
    // mission interaction identifies a door explicitly.
    return ActorMotion::climb;
  default:
    break;
  }
  if (stance_ == PlayerStanceState::kneeling) {
    return locomotion_ == PlayerLocomotionState::crouch_walking
               ? ActorMotion::crouch_walk
               : ActorMotion::kneel;
  }
  switch (locomotion_) {
  case PlayerLocomotionState::walking:
    return ActorMotion::walk;
  case PlayerLocomotionState::strafing:
    return motion_strafe_ < 0.0 ? ActorMotion::strafe_left
                                : ActorMotion::strafe_right;
  case PlayerLocomotionState::crouch_walking:
    return ActorMotion::crouch_walk;
  case PlayerLocomotionState::running:
    return ActorMotion::run;
  case PlayerLocomotionState::idle:
  default:
    if (motion_turn_ < -input_dead_zone) {
      return ActorMotion::turn_left;
    }
    if (motion_turn_ > input_dead_zone) {
      return ActorMotion::turn_right;
    }
    return ActorMotion::idle;
  }
}

std::int32_t PlayerController::modelHeading() const noexcept {
  if (action_ != PlayerActionState::rolling) {
    return state_.yaw;
  }
  switch (roll_direction_) {
  case PlayerRollDirection::left:
    return normalizeHeading(static_cast<std::int64_t>(state_.yaw) - 1024);
  case PlayerRollDirection::right:
    return normalizeHeading(static_cast<std::int64_t>(state_.yaw) + 1024);
  case PlayerRollDirection::forward:
  default:
    return state_.yaw;
  }
}

void PlayerController::updateCamera() noexcept {
  const auto mode = aim_ == PlayerAimState::first_person
                        ? PlayerCameraMode::first_person_aim
                        : PlayerCameraMode::chase;
  const auto chase_heading = modelHeading();
  const auto desired =
      aim_ == PlayerAimState::first_person
          ? aim_camera_rig_.view(state_.x, state_.y, state_.z, aim_heading_,
                                 camera_pitch_)
          : camera_rig_.follow(state_.x, state_.y, state_.z, chase_heading);
  const auto follow_body_immediately =
      action_ == PlayerActionState::rolling ||
      action_ == PlayerActionState::quick_turning ||
      std::abs(motion_turn_) > input_dead_zone;
  if (!camera_initialized_ || mode != camera_mode_ ||
      mode == PlayerCameraMode::first_person_aim || follow_body_immediately) {
    camera_ = desired;
    camera_mode_ = mode;
    camera_initialized_ = true;
    return;
  }

  const auto follow = [](double current, double target, double amount) {
    return current + (target - current) * amount;
  };
  auto camera_x = follow(camera_.x, desired.x, chase_position_follow);
  auto camera_z = follow(camera_.z, desired.z, chase_position_follow);
  const auto forward = headingDirection(chase_heading);
  const auto behind =
      (camera_x - state_.x) * forward.x + (camera_z - state_.z) * forward.z;
  if (behind >= 0.0) {
    // A quick turn must never leave the camera looking at Gabe from the front.
    camera_x = desired.x;
    camera_z = desired.z;
  }
  camera_ = CameraState{
      camera_x,
      follow(camera_.y, desired.y, chase_position_follow),
      camera_z,
      follow(camera_.target_x, desired.target_x, chase_target_follow),
      follow(camera_.target_y, desired.target_y, chase_target_follow),
      follow(camera_.target_z, desired.target_z, chase_target_follow),
  };
  camera_mode_ = mode;
}

PlayerCameraIntent PlayerController::cameraIntent() const noexcept {
  return PlayerCameraIntent{
      aim_ == PlayerAimState::first_person ? PlayerCameraMode::first_person_aim
                                           : PlayerCameraMode::chase,
      state_.x,
      state_.y,
      state_.z,
      aim_ == PlayerAimState::first_person ? aim_heading_ : modelHeading(),
      camera_pitch_,
  };
}

} // namespace sf::game
