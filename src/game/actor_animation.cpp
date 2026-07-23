#include "sf/game/actor_animation.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <cmath>
#include <string>
#include <string_view>

namespace sf::game {
namespace {

std::string normalize(std::string_view name) {
    std::string result{name};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

bool isAnimation(std::string_view name) {
    return name.ends_with(".HAN") || name.ends_with(".LWR") || name.ends_with(".UPR");
}

void validatePair(
    const assets::HmdAnimationClip& lower,
    const assets::HmdAnimationClip& upper,
    std::size_t part_count) {
    const auto expected = part_count == 16U
        ? std::numeric_limits<std::uint16_t>::max()
        : static_cast<std::uint16_t>((std::uint32_t{1} << part_count) - 1U);
    if (lower.partCount() != part_count || upper.partCount() != part_count ||
        (lower.animatedParts() & upper.animatedParts()) != 0U ||
        (lower.animatedParts() | upper.animatedParts()) != expected) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid split actor animation"};
    }
}

std::string_view lowerClipName(ActorMotion motion) noexcept {
    switch (motion) {
    case ActorMotion::walk:
        return "WK0.LWR";
    case ActorMotion::run:
        return "RN0.LWR";
    case ActorMotion::strafe_left:
        return "STEPL0.LWR";
    case ActorMotion::strafe_right:
        return "STEPR0.LWR";
    case ActorMotion::turn_left:
        return "TURNL0.LWR";
    case ActorMotion::turn_right:
        return "TURNR0.LWR";
    case ActorMotion::crouch_walk:
        return "CW0.LWR";
    case ActorMotion::kneel:
    case ActorMotion::kneel_roll:
        return "KN0.LWR";
    case ActorMotion::kneel_down:
    case ActorMotion::stand_up:
    case ActorMotion::quick_turn:
    case ActorMotion::climb:
    case ActorMotion::kick_door:
    case ActorMotion::roll:
    case ActorMotion::idle:
    default:
        return "ST0.LWR";
    }
}

std::string_view neutralUpperClipName(ActorMotion motion) noexcept {
    switch (motion) {
    case ActorMotion::walk:
        return "WK0.UPR";
    case ActorMotion::run:
        return "RN0.UPR";
    case ActorMotion::crouch_walk:
        return "CW0.UPR";
    case ActorMotion::idle:
    case ActorMotion::strafe_left:
    case ActorMotion::strafe_right:
    case ActorMotion::turn_left:
    case ActorMotion::turn_right:
    case ActorMotion::kneel:
    case ActorMotion::roll:
    case ActorMotion::kneel_roll:
    case ActorMotion::kneel_down:
    case ActorMotion::stand_up:
    case ActorMotion::quick_turn:
    case ActorMotion::climb:
    case ActorMotion::kick_door:
    default:
        return "ST02.UPR";
    }
}

enum class AnimationContext : std::size_t {
    standing,
    walking,
    running,
    crouch_walking,
    kneeling,
};

AnimationContext animationContext(ActorMotion motion) noexcept {
    switch (motion) {
    case ActorMotion::walk:
        return AnimationContext::walking;
    case ActorMotion::run:
        return AnimationContext::running;
    case ActorMotion::crouch_walk:
        return AnimationContext::crouch_walking;
    case ActorMotion::kneel:
    case ActorMotion::kneel_roll:
        return AnimationContext::kneeling;
    case ActorMotion::idle:
    case ActorMotion::strafe_left:
    case ActorMotion::strafe_right:
    case ActorMotion::turn_left:
    case ActorMotion::turn_right:
    case ActorMotion::roll:
    case ActorMotion::kneel_down:
    case ActorMotion::stand_up:
    case ActorMotion::quick_turn:
    case ActorMotion::climb:
    case ActorMotion::kick_door:
    default:
        return AnimationContext::standing;
    }
}

std::size_t stanceIndex(PlayerWeaponStance stance) noexcept {
    switch (stance) {
    case PlayerWeaponStance::sidearm:
        return 1U;
    case PlayerWeaponStance::long_gun:
        return 2U;
    case PlayerWeaponStance::unarmed:
    default:
        return 0U;
    }
}

std::string_view requestedUpperClipName(const PlayerAnimationRequest& request) noexcept {
    using StanceClips = std::array<std::string_view, 3>;
    constexpr std::array aim_clips{
        StanceClips{"", "ST1AIM.UPR", "ST2AIM.UPR"},
        StanceClips{"", "WK1AIM.UPR", "WK2AIM.UPR"},
        StanceClips{"", "RN1AIM.UPR", "RN2AIM.UPR"},
        StanceClips{"", "CW1AIM.UPR", "CW2AIM.UPR"},
        StanceClips{"", "KN1AIM.UPR", "KN2AIM.UPR"},
    };
    constexpr std::array fire_clips{
        StanceClips{"", "", "ST2FIRE.UPR"},
        StanceClips{"", "", ""},
        StanceClips{"", "", ""},
        StanceClips{"", "", ""},
        StanceClips{"", "", "KN2FIRE.UPR"},
    };
    constexpr std::array reload_clips{
        StanceClips{"", "STRLD1B.UPR", "ST2RLD.UPR"},
        StanceClips{"", "WK1RLD.UPR", "WK2RLD.UPR"},
        StanceClips{"", "RN1RLD.UPR", "RN2RLD.UPR"},
        StanceClips{"", "CW1RLD.UPR", "CW2RLD.UPR"},
        StanceClips{"", "KN1RLD.UPR", "KN2RLD.UPR"},
    };
    constexpr std::array draw_clips{
        StanceClips{"STDRW0.UPR", "SWIT0_1.UPR", "STDRW2.UPR"},
        StanceClips{"WKDRW0.UPR", "SWIT0_1.UPR", "WKDRW2.UPR"},
        StanceClips{"RNDRW0.UPR", "SWIT0_1.UPR", "RNDRW2.UPR"},
        StanceClips{"CWDRW0.UPR", "CWDRW1.UPR", "CWDRW2.UPR"},
        StanceClips{"KNDRW0.UPR", "KNDRW1.UPR", "KNDRW2.UPR"},
    };
    const auto context = static_cast<std::size_t>(animationContext(request.motion));
    const auto stance = stanceIndex(request.weapon_stance);
    const auto thrown_weapon = request.weapon == WeaponId::fragmentation_grenade ||
        request.weapon == WeaponId::gas_grenade;
    switch (request.upper_action) {
    case PlayerUpperAction::aim:
        return aim_clips[context][stance];
    case PlayerUpperAction::fire:
        if (thrown_weapon) {
            constexpr std::array throw_clips{
                std::string_view{"STLOB.UPR"},
                std::string_view{"WKLOB.UPR"},
                std::string_view{"RNLOB.UPR"},
                std::string_view{"STLOB.UPR"},
                std::string_view{"STLOB.UPR"},
            };
            return throw_clips[context];
        }
        return fire_clips[context][stance];
    case PlayerUpperAction::reload:
        return reload_clips[context][stance];
    case PlayerUpperAction::draw:
        return draw_clips[context][stance];
    case PlayerUpperAction::neutral:
    default:
        if (request.weapon_stance == PlayerWeaponStance::long_gun) {
            switch (request.motion) {
            case ActorMotion::walk: return "WK2.UPR";
            case ActorMotion::run: return "RN2.UPR";
            case ActorMotion::crouch_walk: return "CW2.UPR";
            default: break;
            }
        }
        return neutralUpperClipName(request.motion);
    }
}

std::string_view rollClipName(const PlayerAnimationRequest& request) noexcept {
    const auto long_gun = request.weapon_stance == PlayerWeaponStance::long_gun;
    if (request.motion == ActorMotion::kneel_roll) {
        if (long_gun) {
            return "KNROL2.HAN";
        }
        return request.weapon_stance == PlayerWeaponStance::sidearm
            ? "KNROL1.HAN"
            : "KNROL0.HAN";
    }
    return long_gun ? "STROL2.HAN" : "STROL0.HAN";
}

std::string_view fullBodyClipName(const PlayerAnimationRequest& request) noexcept {
    const auto suffix = request.weapon_stance == PlayerWeaponStance::long_gun ? '2' : '0';
    switch (request.motion) {
    case ActorMotion::kneel_down:
        return suffix == '2' ? "STKN2.HAN" : "STKN0.HAN";
    case ActorMotion::stand_up:
        return suffix == '2' ? "KNST2.HAN" : "KNST0.HAN";
    case ActorMotion::quick_turn:
        return suffix == '2' ? "STRNTRN2.HAN" : "STRNTRN0.HAN";
    case ActorMotion::climb:
        return "CLIMBA.HAN";
    case ActorMotion::kick_door:
        return "KIKDR.HAN";
    default:
        return {};
    }
}

void addRecoilRotation(
    assets::HmdAnimationFrame& frame,
    std::size_t part,
    std::size_t component,
    int amount) noexcept {
    if (part >= frame.transforms.size() || component >= 3U ||
        (frame.valid_parts & (std::uint16_t{1} << part)) == 0U) {
        return;
    }
    auto& value = frame.transforms[part].rotation[component];
    value = static_cast<std::int16_t>(std::clamp(
        static_cast<int>(value) + amount,
        static_cast<int>(std::numeric_limits<std::int16_t>::min()),
        static_cast<int>(std::numeric_limits<std::int16_t>::max())));
}

} // namespace

PlayerAnimationTiming playerAnimationTiming(
    const PlayerAnimationRequest& request) noexcept {
    using StanceFrames = std::array<unsigned int, 3>;
    // Frame counts come directly from the matching PCHAN upper/full-body clips.
    constexpr std::array reload_frames{
        StanceFrames{27U, 27U, 15U},
        StanceFrames{25U, 25U, 25U},
        StanceFrames{14U, 14U, 14U},
        StanceFrames{19U, 19U, 20U},
        StanceFrames{11U, 11U, 15U},
    };
    constexpr std::array draw_frames{
        StanceFrames{8U, 28U, 12U},
        StanceFrames{25U, 28U, 25U},
        StanceFrames{14U, 28U, 14U},
        StanceFrames{19U, 19U, 19U},
        StanceFrames{12U, 12U, 7U},
    };
    const auto context = static_cast<std::size_t>(animationContext(request.motion));
    const auto stance = stanceIndex(request.weapon_stance);
    return PlayerAnimationTiming{
        6U,
        reload_frames[context][stance],
        draw_frames[context][stance],
        10U,
        11U,
        14U,
        46U,
    };
}

double rootMotionForwardDistance(
    std::span<const assets::HmdRootMotionFrame> track,
    std::uint64_t animation_tick,
    unsigned int updates_per_animation_frame) noexcept {
    if (track.empty() || updates_per_animation_frame == 0U) {
        return 0.0;
    }
    return static_cast<double>(track[animation_tick % track.size()].z) /
        static_cast<double>(updates_per_animation_frame);
}

double rootMotionPlanarDistance(
    std::span<const assets::HmdRootMotionFrame> track,
    std::uint64_t animation_tick,
    unsigned int updates_per_animation_frame) noexcept {
    if (track.empty() || updates_per_animation_frame == 0U) {
        return 0.0;
    }
    const auto& frame = track[animation_tick % track.size()];
    return std::hypot(
        static_cast<double>(frame.x),
        static_cast<double>(frame.z)) /
        static_cast<double>(updates_per_animation_frame);
}

const assets::HmdAnimationTransform* ActorPose::transform(std::size_t part) const noexcept {
    if (lower != nullptr && part < lower->transforms.size() &&
        (lower->valid_parts & (std::uint16_t{1} << part)) != 0U) {
        return &lower->transforms[part];
    }
    if (upper != nullptr && part < upper->transforms.size() &&
        (upper->valid_parts & (std::uint16_t{1} << part)) != 0U) {
        return &upper->transforms[part];
    }
    return nullptr;
}

ActorAnimationBank::ActorAnimationBank(
    const assets::HogArchive& archive,
    std::size_t part_count) {
    clips_.reserve(archive.entries().size());
    for (const auto& entry : archive.entries()) {
        auto name = normalize(entry.name);
        if (!isAnimation(name)) {
            continue;
        }
        clips_.push_back(NamedClip{
            std::move(name),
            assets::HmdAnimationClip::parse(archive.file(entry.name), part_count),
        });
    }

    constexpr std::array required{
        "ST0.LWR",
        "ST02.UPR",
        "WK0.LWR",
        "WK0.UPR",
        "RN0.LWR",
        "RN0.UPR",
        "IDLE13.HAN",
    };
    for (const auto name : required) {
        if (clip(name) == nullptr) {
            throw core::Error{
                core::ErrorCode::not_found,
                "Required actor animation is missing: " + std::string{name},
            };
        }
    }

    validatePair(*clip("ST0.LWR"), *clip("ST02.UPR"), part_count);
    validatePair(*clip("WK0.LWR"), *clip("WK0.UPR"), part_count);
    validatePair(*clip("RN0.LWR"), *clip("RN0.UPR"), part_count);
    const auto expected = part_count == 16U
        ? std::numeric_limits<std::uint16_t>::max()
        : static_cast<std::uint16_t>((std::uint32_t{1} << part_count) - 1U);
    if (clip("IDLE13.HAN")->animatedParts() != expected) {
        throw core::Error{core::ErrorCode::invalid_format, "Incomplete enemy idle animation"};
    }
}

const assets::HmdAnimationClip* ActorAnimationBank::clip(std::string_view name) const noexcept {
    const auto match = std::ranges::find_if(clips_, [name](const NamedClip& candidate) {
        return candidate.name == name;
    });
    return match == clips_.end() ? nullptr : &match->animation;
}

ActorPose ActorAnimationBank::playerPose(ActorMotion motion, std::uint64_t tick) const noexcept {
    return playerPose(PlayerAnimationRequest{.motion = motion}, tick);
}

bool ActorAnimationBank::hasPlayerAnimation(
    const PlayerAnimationRequest& request) const noexcept {
    if (request.motion == ActorMotion::roll || request.motion == ActorMotion::kneel_roll) {
        return clip(rollClipName(request)) != nullptr;
    }
    if (clip(lowerClipName(request.motion)) == nullptr) {
        return false;
    }
    if (request.motion == ActorMotion::kneel_down ||
        request.motion == ActorMotion::stand_up ||
        request.motion == ActorMotion::quick_turn ||
        request.motion == ActorMotion::climb ||
        request.motion == ActorMotion::kick_door) {
        return clip(fullBodyClipName(request)) != nullptr;
    }
    if (request.upper_action == PlayerUpperAction::neutral) {
        return clip(requestedUpperClipName(request)) != nullptr;
    }
    const auto requested = requestedUpperClipName(request);
    if (request.upper_action == PlayerUpperAction::fire && clip(requested) == nullptr) {
        auto aim_request = request;
        aim_request.upper_action = PlayerUpperAction::aim;
        return clip(requestedUpperClipName(aim_request)) != nullptr;
    }
    return clip(requested) != nullptr;
}

ActorPose ActorAnimationBank::playerPose(
    const PlayerAnimationRequest& request,
    std::uint64_t tick) const noexcept {
    return playerPose(request, tick, tick);
}

ActorPose ActorAnimationBank::playerPose(
    const PlayerAnimationRequest& request,
    std::uint64_t locomotion_tick,
    std::uint64_t action_tick) const noexcept {
    if (request.motion == ActorMotion::roll || request.motion == ActorMotion::kneel_roll) {
        const auto* full_body = clip(rollClipName(request));
        if (full_body == nullptr) {
            full_body = clip(request.motion == ActorMotion::kneel_roll
                ? "KNROL0.HAN"
                : "STROL0.HAN");
        }
        if (full_body != nullptr) {
            return ActorPose{&full_body->poseAtTick(action_tick), nullptr};
        }
    }

    if (request.motion == ActorMotion::kneel_down ||
        request.motion == ActorMotion::stand_up ||
        request.motion == ActorMotion::quick_turn ||
        request.motion == ActorMotion::climb ||
        request.motion == ActorMotion::kick_door) {
        const auto* full_body = clip(fullBodyClipName(request));
        if (full_body != nullptr) {
            return ActorPose{&full_body->poseAtTick(action_tick), nullptr};
        }
    }

    auto* lower = clip(lowerClipName(request.motion));
    if (lower == nullptr) {
        lower = clip("ST0.LWR");
    }

    const assets::HmdAnimationClip* upper{};
    const assets::HmdAnimationFrame* upper_pose{};
    auto procedural_fire = false;
    if (request.motion == ActorMotion::kneel &&
        request.upper_action == PlayerUpperAction::neutral) {
        // KN0.LWR intentionally contains legs only. Retail keeps the upper
        // channel left by the completed stand-to-kneel clip; substituting the
        // standing ST02 upper pose produces the visible sideways snap/lean.
        const auto* transition = clip(
            request.weapon_stance == PlayerWeaponStance::long_gun
                ? "STKN2.HAN"
                : "STKN0.HAN");
        if (transition != nullptr && !transition->frames().empty()) {
            upper_pose = &transition->frames().back();
        }
    }
    if (upper_pose == nullptr && request.upper_action != PlayerUpperAction::neutral) {
        const auto requested_name = requestedUpperClipName(request);
        upper = clip(requested_name);
        if (upper == nullptr && request.upper_action == PlayerUpperAction::fire) {
            auto aim_request = request;
            aim_request.upper_action = PlayerUpperAction::aim;
            upper = clip(requestedUpperClipName(aim_request));
            procedural_fire = upper != nullptr;
        }
        if (upper == nullptr && request.upper_action == PlayerUpperAction::draw) {
            auto unarmed_draw = request;
            unarmed_draw.weapon_stance = PlayerWeaponStance::unarmed;
            upper = clip(requestedUpperClipName(unarmed_draw));
        }
    }
    if (upper_pose == nullptr && upper == nullptr) {
        auto neutral = request;
        neutral.upper_action = PlayerUpperAction::neutral;
        upper = clip(requestedUpperClipName(neutral));
    }
    if (upper_pose == nullptr && upper == nullptr) {
        upper = clip("ST02.UPR");
    }

    if (upper_pose == nullptr && upper != nullptr) {
        upper_pose = &upper->poseAtTick(action_tick);
    }
    if (procedural_fire && upper_pose != nullptr) {
        // PCHAN has no dedicated pistol or moving-fire clip, so apply a short
        // procedural recoil over the matching native aiming pose.
        constexpr std::array recoil{96, 72, 48, 24, 8, 0};
        const auto kick = recoil[std::min<std::size_t>(
            static_cast<std::size_t>(action_tick), recoil.size() - 1U)];
        procedural_recoil_ = *upper_pose;
        addRecoilRotation(procedural_recoil_, 5U, 0U, -kick);
        addRecoilRotation(procedural_recoil_, 11U, 0U, -kick);
        addRecoilRotation(procedural_recoil_, 3U, 0U, -kick / 2);
        addRecoilRotation(procedural_recoil_, 9U, 0U, -kick / 2);
        addRecoilRotation(procedural_recoil_, 13U, 0U, kick / 4);
        upper_pose = &procedural_recoil_;
    }

    return ActorPose{
        lower == nullptr ? nullptr : &lower->poseAtTick(locomotion_tick),
        upper_pose,
    };
}

ActorPose ActorAnimationBank::enemyPose(std::uint64_t tick, std::uint64_t phase) const noexcept {
    const auto* enemy_idle = clip("IDLE13.HAN");
    return ActorPose{&enemy_idle->poseAtTick(tick + phase), nullptr};
}

ActorPose ActorAnimationBank::enemyDeathPose(std::uint64_t tick) const noexcept {
    const auto* death = clip("DETH1A.HAN");
    if (death == nullptr || death->frames().empty()) {
        return enemyPose(tick, 0U);
    }
    const auto frame = std::min<std::size_t>(
        static_cast<std::size_t>(tick),
        death->frames().size() - 1U);
    return ActorPose{&death->frames()[frame], nullptr};
}

ActorPose ActorAnimationBank::npcPose(
    const NpcAnimationRequest& request,
    std::uint64_t tick,
    std::uint64_t phase) const noexcept {
    return npcPose(request, tick, tick, phase);
}

ActorPose ActorAnimationBank::npcPose(
    const NpcAnimationRequest& request,
    std::uint64_t locomotion_tick,
    std::uint64_t action_tick,
    std::uint64_t phase) const noexcept {
    const auto full_body = [&](std::string_view name, bool loop) -> ActorPose {
        const auto* animation = clip(name);
        if (animation == nullptr || animation->frames().empty()) {
            return enemyPose(locomotion_tick, phase);
        }
        const auto frame = loop
            ? static_cast<std::size_t>(
                (action_tick + phase) % animation->frames().size())
            : std::min<std::size_t>(
                static_cast<std::size_t>(action_tick),
                animation->frames().size() - 1U);
        return ActorPose{&animation->frames()[frame], nullptr};
    };
    const auto final_full_body = [&](std::string_view name) -> ActorPose {
        const auto* animation = clip(name);
        if (animation == nullptr || animation->frames().empty()) {
            return enemyPose(locomotion_tick, phase);
        }
        return ActorPose{&animation->frames().back(), nullptr};
    };
    switch (request.action) {
    case NpcAnimationAction::walk:
        return playerPose(PlayerAnimationRequest{
            request.motion,
            PlayerUpperAction::neutral,
            request.weapon_stance,
            request.weapon,
        }, locomotion_tick + phase, action_tick);
    case NpcAnimationAction::run:
        return playerPose(PlayerAnimationRequest{
            request.motion,
            PlayerUpperAction::neutral,
            request.weapon_stance,
            request.weapon,
        }, locomotion_tick + phase, action_tick);
    case NpcAnimationAction::aim:
        return playerPose(PlayerAnimationRequest{
            request.motion,
            PlayerUpperAction::aim,
            request.weapon_stance,
            request.weapon,
        }, locomotion_tick + phase, action_tick);
    case NpcAnimationAction::fire:
        return playerPose(PlayerAnimationRequest{
            request.motion,
            PlayerUpperAction::fire,
            request.weapon_stance,
            request.weapon,
        }, locomotion_tick + phase, action_tick);
    case NpcAnimationAction::reload:
        return playerPose(PlayerAnimationRequest{
            ActorMotion::idle,
            PlayerUpperAction::reload,
            request.weapon_stance,
            request.weapon,
        }, locomotion_tick + phase, action_tick);
    case NpcAnimationAction::kneel:
        return playerPose(PlayerAnimationRequest{
            ActorMotion::kneel,
            PlayerUpperAction::neutral,
            request.weapon_stance,
            request.weapon,
        }, locomotion_tick + phase, action_tick);
    case NpcAnimationAction::climb:
        return playerPose(PlayerAnimationRequest{
            ActorMotion::climb,
            PlayerUpperAction::neutral,
            request.weapon_stance,
            request.weapon,
        }, action_tick, action_tick);
    case NpcAnimationAction::jump:
        return full_body("JP1.HAN", false);
    case NpcAnimationAction::fall:
        return full_body("FALL1.HAN", false);
    case NpcAnimationAction::hit_left:
        return full_body("SHIML.HAN", false);
    case NpcAnimationAction::hit_right:
        return full_body("SHIMR.HAN", false);
    case NpcAnimationAction::surrender:
        return full_body("SUREND1.HAN", false);
    case NpcAnimationAction::fire_death:
        return full_body("DETHFIR0.HAN", false);
    case NpcAnimationAction::electrical_death:
        return full_body("FIREDANC.HAN", true);
    case NpcAnimationAction::death:
        // Object index is not a retail death-direction selector. Using it as
        // one chose acrobatic variants arbitrarily and left some corpses with
        // raised legs. DETH1A is the neutral grounded fall used as fallback.
        return full_body("DETH1A.HAN", false);
    case NpcAnimationAction::dead:
        return final_full_body("DETH1A.HAN");
    case NpcAnimationAction::fire_dead:
        return final_full_body("DETHFIR0.HAN");
    case NpcAnimationAction::electrical_dead:
        return final_full_body("DETHFIR0.HAN");
    case NpcAnimationAction::idle:
    default:
        return playerPose(PlayerAnimationRequest{
            request.motion,
            PlayerUpperAction::neutral,
            request.weapon_stance,
            request.weapon,
        }, locomotion_tick + phase, action_tick);
    }
}

} // namespace sf::game
