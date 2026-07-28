#pragma once

#include "sf/assets/hmd_animation.hpp"
#include "sf/game/actor_animation.hpp"
#include "sf/game/chase_camera.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sf::game {

// Shared by the deterministic controller and the display-rate mouse lead.
// This leaves a small margin around the vertical camera singularity.
inline constexpr double maximum_first_person_aim_pitch = 1000.0;

struct PlayerInput {
  // Keep this compatibility prefix in the same order as the original
  // GameplayInput aggregate used by the platform layer.
  double move{};
  double turn{};
  bool run{};
  bool aim{};
  bool next_weapon{};
  bool previous_weapon{};
  bool quick_weapon{};

  double strafe{};
  // Mouse/controller look is supplied in PS1 heading-angle units. Keeping
  // device scaling outside the controller makes replay updates deterministic.
  double look_yaw{};
  double look_pitch{};
  bool fire_pressed{};
  bool fire_held{};
  bool roll{};
  bool reload{};
  bool kneel{};
  bool interact{};
  bool target_lock{};
  bool target_lock_held{};
  bool target_lock_released{};
  bool quick_turn{};
  std::int32_t weapon_menu_delta{};
  std::optional<std::uint8_t> direct_weapon;
  // Dedicated retail L1+L2/R2 corner-peek channel. First-person movement is
  // blocked; keeping this separate prevents rejected A/D locomotion from being
  // reinterpreted as a camera-only peek.
  double aim_peek{};
};

struct PlayerState {
  double x{};
  double y{};
  double z{};
  std::int32_t yaw{};
  bool grounded{};
};

enum class PlayerLocomotionState : std::uint8_t {
  idle,
  walking,
  running,
  strafing,
  crouch_walking,
};

enum class PlayerStanceState : std::uint8_t {
  standing,
  kneeling,
};

// Action is deliberately separate from locomotion so fire/reload can blend an
// upper channel while rolls and stance transitions temporarily own the body.
enum class PlayerActionState : std::uint8_t {
  ready,
  firing,
  rolling,
  reloading,
  weapon_switching,
  kneeling_down,
  standing_up,
  quick_turning,
  interacting,
};

enum class PlayerAimState : std::uint8_t {
  chase,
  first_person,
};

enum class PlayerWeaponSwitchState : std::uint8_t {
  none,
  next,
  previous,
  direct,
};

enum class PlayerCameraMode : std::uint8_t {
  chase,
  first_person_aim,
};

enum class PlayerRollDirection : std::uint8_t {
  forward,
  left,
  right,
};

struct PlayerCameraIntent {
  PlayerCameraMode mode{PlayerCameraMode::chase};
  double anchor_x{};
  double anchor_y{};
  double anchor_z{};
  std::int32_t heading{};
  double pitch{};
};

class PlayerMovementResolver {
public:
  virtual ~PlayerMovementResolver() = default;

  [[nodiscard]] virtual bool tryMove(PlayerState &player, double desired_x,
                                     double desired_z) = 0;
};

class PlayerController final {
public:
  static constexpr unsigned int updates_per_animation_frame = 1U;
  static constexpr std::int32_t turn_units_per_update = 72;
  static constexpr unsigned int minimum_roll_updates = 10U;
  static constexpr unsigned int fire_action_updates = 6U;
  static constexpr unsigned int reload_action_updates = 27U;
  static constexpr unsigned int weapon_switch_action_updates = 28U;
  static constexpr unsigned int stance_action_updates = 10U;
  static constexpr unsigned int stand_action_updates = 11U;
  static constexpr unsigned int quick_turn_action_updates = 14U;
  static constexpr unsigned int interact_action_updates = 46U;
  // Matches the collision resolver's largest legal step. First-person bridge
  // samples outside this range are pose/transition offsets, not world roots.
  static constexpr double maximum_first_person_root_height_step = 160.0;

  explicit PlayerController(
      ChaseCameraConfiguration chase_camera = {},
      FirstPersonCameraConfiguration aim_camera = {}) noexcept;

  void setRootMotionTracks(
      std::span<const assets::HmdRootMotionFrame> walking,
      std::span<const assets::HmdRootMotionFrame> running,
      std::span<const assets::HmdRootMotionFrame> rolling = {},
      std::span<const assets::HmdRootMotionFrame> crouch_walking = {});
  void
  setStrafeRootMotionTracks(std::span<const assets::HmdRootMotionFrame> left,
                            std::span<const assets::HmdRootMotionFrame> right);
  void setAdditionalRollRootMotionTracks(
      std::span<const assets::HmdRootMotionFrame> standing_long_gun,
      std::span<const assets::HmdRootMotionFrame> kneeling_unarmed,
      std::span<const assets::HmdRootMotionFrame> kneeling_sidearm,
      std::span<const assets::HmdRootMotionFrame> kneeling_long_gun);
  void setWeaponStance(PlayerWeaponStance stance) noexcept {
    weapon_stance_ = stance;
  }
  void setAnimationTiming(PlayerAnimationTiming timing) noexcept {
    animation_timing_ = timing;
  }
  void reset(const PlayerState &spawn) noexcept;
  // Scripted retail frames own the root transform while preserving the
  // native presentation state until the lock's final falling-edge sample.
  void synchronizeScriptedPose(const PlayerState &pose) noexcept;
  // Accept the retail collision root without clearing held first-person
  // locomotion or restarting its root-motion phase.
  void synchronizeFirstPersonRoot(const PlayerState &pose) noexcept;
  void update(const PlayerInput &input, PlayerMovementResolver &movement);
  void advanceAnimationClock() noexcept;

  [[nodiscard]] const PlayerState &state() const noexcept { return state_; }
  [[nodiscard]] PlayerLocomotionState locomotion() const noexcept {
    return locomotion_;
  }
  [[nodiscard]] PlayerActionState action() const noexcept { return action_; }
  [[nodiscard]] PlayerStanceState stance() const noexcept { return stance_; }
  [[nodiscard]] PlayerAimState aim() const noexcept { return aim_; }
  [[nodiscard]] PlayerWeaponSwitchState weaponSwitch() const noexcept {
    return weapon_switch_;
  }
  [[nodiscard]] ActorMotion actorMotion() const noexcept;
  [[nodiscard]] std::uint64_t animationTick() const noexcept {
    return animation_tick_;
  }
  [[nodiscard]] std::uint64_t actionAnimationTick() const noexcept {
    return action_animation_tick_;
  }
  [[nodiscard]] PlayerRollDirection rollDirection() const noexcept {
    return roll_direction_;
  }
  // The native roll clips always tumble along their local forward axis.
  // Side rolls rotate Gabe's body; chase camera uses the same visual heading.
  [[nodiscard]] std::int32_t modelHeading() const noexcept;
  [[nodiscard]] std::int32_t aimHeading() const noexcept {
    return aim_heading_;
  }
  [[nodiscard]] bool actionLocked() const noexcept {
    return action_ == PlayerActionState::rolling ||
           action_ == PlayerActionState::kneeling_down ||
           action_ == PlayerActionState::standing_up ||
           action_ == PlayerActionState::quick_turning ||
           action_ == PlayerActionState::interacting;
  }
  [[nodiscard]] unsigned int rollDurationUpdates() const noexcept;
  [[nodiscard]] CameraState camera() const noexcept { return camera_; }
  [[nodiscard]] PlayerCameraIntent cameraIntent() const noexcept;
  [[nodiscard]] std::optional<std::uint8_t> directWeapon() const noexcept {
    return direct_weapon_;
  }

private:
  void setLocomotion(PlayerLocomotionState locomotion) noexcept;
  [[nodiscard]] bool tryMove(PlayerMovementResolver &movement,
                             double direction_x, double direction_z,
                             double distance);
  void beginAction(PlayerActionState action, unsigned int duration) noexcept;
  void updateAction(const PlayerInput &input) noexcept;
  void updateCamera() noexcept;
  [[nodiscard]] const std::vector<assets::HmdRootMotionFrame> &
  rollingRootMotion() const noexcept;

  PlayerState state_{};
  PlayerLocomotionState locomotion_{PlayerLocomotionState::idle};
  PlayerActionState action_{PlayerActionState::ready};
  PlayerStanceState stance_{PlayerStanceState::standing};
  PlayerAimState aim_{PlayerAimState::chase};
  PlayerWeaponSwitchState weapon_switch_{PlayerWeaponSwitchState::none};
  std::vector<assets::HmdRootMotionFrame> walking_root_motion_;
  std::vector<assets::HmdRootMotionFrame> running_root_motion_;
  std::vector<assets::HmdRootMotionFrame> rolling_root_motion_;
  std::vector<assets::HmdRootMotionFrame> standing_long_gun_roll_root_motion_;
  std::vector<assets::HmdRootMotionFrame> kneeling_unarmed_roll_root_motion_;
  std::vector<assets::HmdRootMotionFrame> kneeling_sidearm_roll_root_motion_;
  std::vector<assets::HmdRootMotionFrame> kneeling_long_gun_roll_root_motion_;
  std::vector<assets::HmdRootMotionFrame> crouch_root_motion_;
  std::vector<assets::HmdRootMotionFrame> strafe_left_root_motion_;
  std::vector<assets::HmdRootMotionFrame> strafe_right_root_motion_;
  std::uint64_t animation_tick_{};
  std::uint64_t action_animation_tick_{};
  unsigned int animation_phase_{};
  unsigned int action_updates_remaining_{};
  PlayerRollDirection roll_direction_{PlayerRollDirection::forward};
  double motion_move_{};
  double motion_strafe_{};
  double motion_turn_{};
  // Mouse aim owns a camera heading, never the collision/model heading.
  // Keeping both angles separate prevents a first-person sweep from
  // turning Gabe around when the chase camera is restored.
  std::int32_t aim_heading_{};
  double camera_pitch_{};
  std::optional<std::uint8_t> direct_weapon_;
  PlayerWeaponStance weapon_stance_{PlayerWeaponStance::unarmed};
  PlayerAnimationTiming animation_timing_{};
  ChaseCamera camera_rig_;
  FirstPersonCamera aim_camera_rig_;
  CameraState camera_{};
  PlayerCameraMode camera_mode_{PlayerCameraMode::chase};
  bool camera_initialized_{};
};

} // namespace sf::game
