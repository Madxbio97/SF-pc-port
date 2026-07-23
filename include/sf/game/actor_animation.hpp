#pragma once

#include "sf/assets/hmd_animation.hpp"
#include "sf/assets/hog_archive.hpp"
#include "sf/game/hud.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::game {

enum class ActorMotion {
    idle,
    walk,
    run,
    strafe_left,
    strafe_right,
    turn_left,
    turn_right,
    crouch_walk,
    kneel,
    roll,
    kneel_roll,
    kneel_down,
    stand_up,
    quick_turn,
    climb,
    kick_door,
};

enum class PlayerUpperAction : std::uint8_t {
    neutral,
    aim,
    fire,
    reload,
    draw,
};

// PCHAN groups Gabe's upper-body clips by the native 0/1/2 weapon stance.
enum class PlayerWeaponStance : std::uint8_t {
    unarmed,
    sidearm,
    long_gun,
};

struct PlayerAnimationRequest {
    ActorMotion motion{ActorMotion::idle};
    PlayerUpperAction upper_action{PlayerUpperAction::neutral};
    PlayerWeaponStance weapon_stance{PlayerWeaponStance::unarmed};
    WeaponId weapon{WeaponId::unarmed};
};

struct PlayerAnimationTiming {
    unsigned int fire_updates{6U};
    unsigned int reload_updates{27U};
    unsigned int draw_updates{28U};
    unsigned int kneel_down_updates{10U};
    unsigned int stand_up_updates{11U};
    unsigned int quick_turn_updates{14U};
    unsigned int interact_updates{46U};
};

enum class NpcAnimationAction : std::uint8_t {
    idle,
    walk,
    run,
    aim,
    fire,
    reload,
    kneel,
    climb,
    jump,
    fall,
    hit_left,
    hit_right,
    surrender,
    death,
    fire_death,
    electrical_death,
    dead,
    fire_dead,
    electrical_dead,
};

struct NpcAnimationRequest {
    NpcAnimationAction action{NpcAnimationAction::idle};
    PlayerWeaponStance weapon_stance{PlayerWeaponStance::long_gun};
    std::uint8_t variant{};
    WeaponId weapon{WeaponId::unarmed};
    // NPC aiming is an upper-body channel. Keeping locomotion separate avoids
    // restarting a full-body AIM/RUN clip whenever a collision blocks one step.
    ActorMotion motion{ActorMotion::idle};
};

// Gameplay and native PCHAN clips advance together at the retail 20 Hz rate.
[[nodiscard]] PlayerAnimationTiming playerAnimationTiming(
    const PlayerAnimationRequest& request) noexcept;

struct ActorPose {
    const assets::HmdAnimationFrame* lower{};
    const assets::HmdAnimationFrame* upper{};

    [[nodiscard]] const assets::HmdAnimationTransform* transform(
        std::size_t part) const noexcept;
};

[[nodiscard]] double rootMotionForwardDistance(
    std::span<const assets::HmdRootMotionFrame> track,
    std::uint64_t animation_tick,
    unsigned int updates_per_animation_frame) noexcept;
[[nodiscard]] double rootMotionPlanarDistance(
    std::span<const assets::HmdRootMotionFrame> track,
    std::uint64_t animation_tick,
    unsigned int updates_per_animation_frame) noexcept;

class ActorAnimationBank final {
public:
    ActorAnimationBank(const assets::HogArchive& archive, std::size_t part_count);

    [[nodiscard]] ActorPose playerPose(ActorMotion motion, std::uint64_t tick) const noexcept;
    [[nodiscard]] ActorPose playerPose(
        const PlayerAnimationRequest& request,
        std::uint64_t tick) const noexcept;
    [[nodiscard]] ActorPose playerPose(
        const PlayerAnimationRequest& request,
        std::uint64_t locomotion_tick,
        std::uint64_t action_tick) const noexcept;
    [[nodiscard]] bool hasPlayerAnimation(
        const PlayerAnimationRequest& request) const noexcept;
    [[nodiscard]] ActorPose enemyPose(std::uint64_t tick, std::uint64_t phase) const noexcept;
    [[nodiscard]] ActorPose enemyDeathPose(std::uint64_t tick) const noexcept;
    [[nodiscard]] ActorPose npcPose(
        const NpcAnimationRequest& request,
        std::uint64_t tick,
        std::uint64_t phase = 0U) const noexcept;
    [[nodiscard]] ActorPose npcPose(
        const NpcAnimationRequest& request,
        std::uint64_t locomotion_tick,
        std::uint64_t action_tick,
        std::uint64_t phase) const noexcept;

private:
    struct NamedClip {
        std::string name;
        assets::HmdAnimationClip animation;
    };

    [[nodiscard]] const assets::HmdAnimationClip* clip(
        std::string_view name) const noexcept;

    std::vector<NamedClip> clips_;
    mutable assets::HmdAnimationFrame procedural_recoil_;
};

} // namespace sf::game
