#include "sf/game/mission_scripts.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace sf::game {
namespace {

constexpr std::uint16_t upper_subway_bomb_source = 28U;
constexpr std::uint16_t bank_bomb_source = 29U;
constexpr std::uint16_t final_subway_bomb_source = 30U;
constexpr std::uint16_t cbdc_agent_first_source = 172U;
constexpr std::uint16_t cbdc_agent_second_source = 173U;
constexpr std::uint16_t kravitch_source = 174U;
constexpr std::uint16_t bank_reinforcement_source = 175U;
constexpr std::uint16_t closed_gate_source = 67U;
constexpr std::uint16_t security_gate_source = 68U;
constexpr std::uint16_t gate_lock_source = 140U;
constexpr std::uint16_t communications_array_source = 260U;
constexpr std::array<std::uint16_t, 3U> scripted_event_sources{257U, 258U, 259U};
constexpr std::array<std::uint16_t, 2U> initial_objective_doors{238U, 239U};
constexpr std::uint16_t upper_elevator_switch = 315U;
constexpr std::uint16_t lower_elevator_switch = 316U;
constexpr std::uint16_t power_switch = 317U;
constexpr std::uint16_t lower_station_switch = 318U;
constexpr std::uint16_t upper_station_switch = 319U;

struct NativeCameraPoint {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
};

constexpr NativeCameraPoint opening_eye_origin{2372, 3206, 5977};
constexpr std::array<NativeCameraPoint, 11U> opening_eye_nodes{
    NativeCameraPoint{2379, 3204, 5982},
    NativeCameraPoint{3132, 3078, 5750},
    NativeCameraPoint{3257, 2994, 5532},
    NativeCameraPoint{3072, 2914, 4335},
    NativeCameraPoint{3108, 2839, 3729},
    NativeCameraPoint{3276, 2747, 3218},
    NativeCameraPoint{3438, 2698, 2952},
    NativeCameraPoint{3802, 2624, 2714},
    NativeCameraPoint{4336, 2567, 2548},
    NativeCameraPoint{4804, 2492, 2640},
    NativeCameraPoint{4849, 2301, 2812},
};
constexpr NativeCameraPoint opening_aim_origin{3559, 2425, 4482};
constexpr std::array<NativeCameraPoint, 11U> opening_aim_nodes{
    NativeCameraPoint{3559, 2426, 4480},
    NativeCameraPoint{3546, 2423, 3739},
    NativeCameraPoint{3845, 2425, 3038},
    NativeCameraPoint{4751, 2300, 2849},
    NativeCameraPoint{4759, 2300, 2849},
    NativeCameraPoint{4760, 2298, 2849},
    NativeCameraPoint{4767, 2298, 2849},
    NativeCameraPoint{4775, 2298, 2849},
    NativeCameraPoint{4780, 2298, 2849},
    NativeCameraPoint{4788, 2298, 2849},
    NativeCameraPoint{4793, 2298, 2849},
};
constexpr std::int32_t opening_eye_speed = 30;

constexpr double bank_activation_x = 12616.0;
constexpr double bank_activation_z = 10750.0;
constexpr double bank_activation_radius = 2800.0;
constexpr double switch_radius = 520.0;
constexpr double upper_bomb_x = 1338.0;
constexpr double upper_bomb_z = -1403.0;
constexpr double final_bomb_x = -309.0;
constexpr double final_bomb_z = 426.0;

double horizontalDistance(
    double x,
    double z,
    double target_x,
    double target_z) noexcept {
    return std::hypot(x - target_x, z - target_z);
}

double spatialDistance(
    double x,
    double y,
    double z,
    double target_x,
    double target_y,
    double target_z) noexcept {
    return std::sqrt(
        (x - target_x) * (x - target_x) +
        (y - target_y) * (y - target_y) +
        (z - target_z) * (z - target_z));
}

std::uint32_t integerSquareRoot(std::uint32_t value) noexcept {
    auto result = std::uint32_t{};
    auto bit = std::uint32_t{1U << 30U};
    while (bit > value) {
        bit >>= 2U;
    }
    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1U) + bit;
        } else {
            result >>= 1U;
        }
        bit >>= 2U;
    }
    return result;
}

std::int32_t arithmeticShiftRight(std::int64_t value, unsigned int shift) noexcept {
    if (shift == 0U || value >= 0) {
        return static_cast<std::int32_t>(value >> shift);
    }
    const auto magnitude = static_cast<std::uint64_t>(-value);
    const auto rounding = (std::uint64_t{1U} << shift) - 1U;
    return -static_cast<std::int32_t>((magnitude + rounding) >> shift);
}

std::uint32_t absoluteBits(std::int32_t value) noexcept {
    return value < 0
        ? static_cast<std::uint32_t>(-static_cast<std::int64_t>(value))
        : static_cast<std::uint32_t>(value);
}

std::uint32_t normalizedTableKey(std::uint32_t value, unsigned int leading) noexcept {
    return leading < 24U
        ? value >> (24U - leading)
        : value << (leading - 24U);
}

// Exact software equivalents of the two 192-entry PSYQ lookup tables used by
// SquareRoot0 and FUN_800c720c. Their entries are floor(sqrt(key/64)*4096)
// and floor(sqrt(64/key)*4096), respectively, for keys 0x40..0xff.
std::uint32_t squareRootTable(std::uint32_t key) noexcept {
    return integerSquareRoot(key << 18U);
}

std::uint32_t inverseSquareRootTable(std::uint32_t key) noexcept {
    return integerSquareRoot((std::uint32_t{1U} << 30U) / key);
}

std::int32_t squareRoot0(std::uint32_t value) noexcept {
    if (value == 0U) {
        return 0;
    }
    const auto leading = std::countl_zero(value) & ~1U;
    const auto key = normalizedTableKey(value, leading);
    return static_cast<std::int32_t>(
        (squareRootTable(key) << ((31U - leading) >> 1U)) >> 12U);
}

NativeCameraPoint subtractPoint(
    const NativeCameraPoint& first,
    const NativeCameraPoint& second) noexcept {
    return NativeCameraPoint{
        first.x - second.x,
        first.y - second.y,
        first.z - second.z,
    };
}

std::uint32_t squaredLength(const NativeCameraPoint& point) noexcept {
    return static_cast<std::uint32_t>(
        static_cast<std::int64_t>(point.x) * point.x +
        static_cast<std::int64_t>(point.y) * point.y +
        static_cast<std::int64_t>(point.z) * point.z);
}

NativeCameraPoint normalize4096(NativeCameraPoint point) noexcept {
    const auto components = absoluteBits(point.x) |
        absoluteBits(point.y) | absoluteBits(point.z);
    if (components == 0U) {
        return {};
    }
    const auto leading = std::countl_zero(components);
    const auto pre_shift = 18 - static_cast<int>(leading);
    if (pre_shift > 0) {
        point.x = arithmeticShiftRight(point.x, static_cast<unsigned int>(pre_shift));
        point.y = arithmeticShiftRight(point.y, static_cast<unsigned int>(pre_shift));
        point.z = arithmeticShiftRight(point.z, static_cast<unsigned int>(pre_shift));
    }
    const auto length_squared = squaredLength(point);
    const auto length_leading = std::countl_zero(length_squared) & ~1U;
    const auto key = normalizedTableKey(length_squared, length_leading);
    const auto reciprocal = static_cast<std::int32_t>(inverseSquareRootTable(key));
    const auto post_shift = (31U - length_leading) >> 1U;
    return NativeCameraPoint{
        arithmeticShiftRight(static_cast<std::int64_t>(point.x) * reciprocal, post_shift),
        arithmeticShiftRight(static_cast<std::int64_t>(point.y) * reciprocal, post_shift),
        arithmeticShiftRight(static_cast<std::int64_t>(point.z) * reciprocal, post_shift),
    };
}

std::int32_t multiply12(std::int32_t value, std::int32_t scale) noexcept {
    const auto product = static_cast<std::int64_t>(value) * scale;
    const auto magnitude = static_cast<std::uint64_t>(product < 0 ? -product : product);
    const auto result = static_cast<std::int32_t>(magnitude >> 12U);
    return product < 0 ? -result : result;
}

NativeCameraPoint scale12(const NativeCameraPoint& point, std::int32_t scale) noexcept {
    return NativeCameraPoint{
        multiply12(point.x, scale),
        multiply12(point.y, scale),
        multiply12(point.z, scale),
    };
}

void addPoint(NativeCameraPoint& point, const NativeCameraPoint& delta) noexcept {
    point.x += delta.x;
    point.y += delta.y;
    point.z += delta.z;
}

} // namespace

MissionCinematicCamera OpeningCinematicCameraRuntime::sample(
    unsigned int updates) const noexcept {
    // Event 0x12 advances once during activation. GameplaySession now calls
    // this runtime directly at the retail 20 Hz cadence.
    const auto native_tick = updates;
    if (native_tick == 0U) {
        return MissionCinematicCamera{
            static_cast<double>(opening_eye_origin.x),
            -static_cast<double>(opening_eye_origin.y),
            static_cast<double>(opening_eye_origin.z),
            static_cast<double>(opening_aim_origin.x),
            -static_cast<double>(opening_aim_origin.y),
            static_cast<double>(opening_aim_origin.z),
        };
    }

    auto eye = opening_eye_origin;
    auto aim = opening_aim_origin;
    auto node = std::size_t{};
    auto reverse = false;
    for (auto tick = 0U; tick < native_tick; ++tick) {
        const auto target = opening_eye_nodes[node];
        const auto delta = subtractPoint(target, eye);
        const auto distance = squareRoot0(squaredLength(delta));
        auto done = false;
        if (opening_eye_speed >= distance) {
            if (!reverse) {
                ++node;
                if (node >= opening_eye_nodes.size()) {
                    node = opening_eye_nodes.size() - 1U;
                    reverse = true;
                    done = true;
                }
            } else if (node == 0U) {
                reverse = false;
                done = true;
            } else {
                --node;
            }
        }
        const auto step = done
            ? delta
            : scale12(normalize4096(delta), std::min(opening_eye_speed, distance));
        addPoint(eye, step);

        const auto segment_start = !reverse
            ? (node == 0U ? 0U : node - 1U)
            : std::min(node + 1U, opening_eye_nodes.size() - 1U);
        const auto travelled = squareRoot0(squaredLength(
            subtractPoint(eye, opening_eye_nodes[segment_start])));
        const auto segment_length = squareRoot0(squaredLength(
            subtractPoint(opening_eye_nodes[node], opening_eye_nodes[segment_start])));
        aim = opening_aim_nodes[segment_start];
        if (segment_length != 0) {
            const auto aim_delta = subtractPoint(
                opening_aim_nodes[node], opening_aim_nodes[segment_start]);
            aim.x += static_cast<std::int32_t>(
                static_cast<std::int64_t>(aim_delta.x) * travelled / segment_length);
            aim.y += static_cast<std::int32_t>(
                static_cast<std::int64_t>(aim_delta.y) * travelled / segment_length);
            aim.z += static_cast<std::int32_t>(
                static_cast<std::int64_t>(aim_delta.z) * travelled / segment_length);
        }
        if (done) {
            break;
        }
    }
    return MissionCinematicCamera{
        static_cast<double>(eye.x),
        -static_cast<double>(eye.y),
        static_cast<double>(eye.z),
        static_cast<double>(aim.x),
        -static_cast<double>(aim.y),
        static_cast<double>(aim.z),
    };
}

void MapFadeRuntime::advance() noexcept {
    intensity_ = intensity_ > map_fade_release_per_frame
        ? static_cast<std::uint8_t>(intensity_ - map_fade_release_per_frame)
        : 0U;
}

MissionScriptRuntime::MissionScriptRuntime(std::size_t object_count)
    : actors_(object_count) {}

void MissionScriptRuntime::resize(std::size_t object_count) {
    actors_.resize(object_count);
}

void MissionScriptRuntime::configureActor(
    std::uint16_t object,
    std::uint16_t source_index,
    bool hostile,
    std::uint32_t ai_parameter) noexcept {
    if (object >= actors_.size()) {
        return;
    }
    auto& actor = actors_[object];
    actor.source_index = source_index;
    actor.configured = true;
    actor.recyclable = hostile && ai_parameter == 1U;
    // Room streaming controls these actors in the original. Hiding 176, 182
    // and 183 until Kravitch died removed authored NPCs from room 70.
    actor.initially_dormant = false;
}

void MissionScriptRuntime::reset() noexcept {
    for (auto& actor : actors_) {
        actor.pending = false;
        actor.cooldown = 0U;
    }
    state_ = {};
    global_spawn_cooldown_ = 0U;
    gate_open_pending_ = false;
    initial_doors_open_pending_ = false;
    checkpoint_pending_ = false;
    mission_failed_pending_ = false;
}

void MissionScriptRuntime::queueCheckpoint() noexcept {
    checkpoint_pending_ = true;
}

void MissionScriptRuntime::updateInitialObjectiveCompletion() noexcept {
    const auto complete = state_.cbdc_protected && state_.kravitch_eliminated &&
        state_.communications_destroyed;
    if (complete && !state_.initial_objectives_complete) {
        state_.initial_objectives_complete = true;
        initial_doors_open_pending_ = true;
        queueCheckpoint();
    }
}

void MissionScriptRuntime::actorKilled(std::uint16_t object) noexcept {
    if (object >= actors_.size() || !actors_[object].configured) {
        return;
    }
    auto& actor = actors_[object];
    if (actor.source_index == bank_reinforcement_source) {
        state_.bank_assault_started = true;
        state_.bank_attackers_eliminated = static_cast<std::uint8_t>(
            std::min<unsigned int>(
                state_.bank_attackers_eliminated + 1U,
                mission_bank_attacker_count));
        if (state_.bank_attackers_eliminated < mission_bank_attacker_count) {
            actor.pending = true;
            actor.cooldown = mission_reinforcement_delay_updates;
        } else {
            state_.cbdc_protected = true;
            queueCheckpoint();
            updateInitialObjectiveCompletion();
        }
    } else if (actor.recyclable) {
        actor.pending = true;
        actor.cooldown = mission_reinforcement_delay_updates;
    }

    if (actor.source_index == kravitch_source) {
        state_.kravitch_eliminated = true;
        if (state_.communications_destroyed) {
            queueCheckpoint();
        }
        updateInitialObjectiveCompletion();
    } else if (actor.source_index == cbdc_agent_first_source ||
               actor.source_index == cbdc_agent_second_source) {
        state_.failed = true;
        mission_failed_pending_ = true;
    }
}

void MissionScriptRuntime::objectDamaged(
    std::uint16_t source_index,
    bool destroyed) noexcept {
    if (source_index == final_subway_bomb_source ||
        (destroyed && (source_index == upper_subway_bomb_source ||
                       source_index == bank_bomb_source))) {
        state_.failed = true;
        mission_failed_pending_ = true;
        return;
    }
    if (source_index == upper_subway_bomb_source ||
        source_index == bank_bomb_source) {
        state_.bomb_warning_issued = true;
    }
}

void MissionScriptRuntime::objectDestroyed(std::uint16_t source_index) noexcept {
    if (source_index == gate_lock_source) {
        gate_open_pending_ = true;
    }
    if (source_index == communications_array_source) {
        state_.communications_destroyed = true;
        if (state_.kravitch_eliminated) {
            queueCheckpoint();
        }
        updateInitialObjectiveCompletion();
    }
    objectDamaged(source_index, true);
}

std::vector<MissionScriptCommand> MissionScriptRuntime::update(
    const MissionScriptUpdateContext& context) noexcept {
    if (global_spawn_cooldown_ != 0U) {
        --global_spawn_cooldown_;
    }
    for (auto& actor : actors_) {
        if (actor.pending && actor.cooldown != 0U) {
            --actor.cooldown;
        }
    }

    if (!state_.bank_assault_started && horizontalDistance(
            context.player_x,
            context.player_z,
            bank_activation_x,
            bank_activation_z) <= bank_activation_radius) {
        state_.bank_assault_started = true;
    }

    std::vector<MissionScriptCommand> commands;
    if (gate_open_pending_) {
        commands.push_back({MissionScriptCommandType::destroy_object_source, closed_gate_source});
        gate_open_pending_ = false;
    }
    if (initial_doors_open_pending_) {
        for (const auto source : initial_objective_doors) {
            commands.push_back({MissionScriptCommandType::destroy_object_source, source});
        }
        initial_doors_open_pending_ = false;
    }

    if (context.interact && !state_.security_bypassed && spatialDistance(
            context.player_x, context.player_y, context.player_z,
            -562.0, -259.0, 4304.0) <= switch_radius) {
        state_.security_bypassed = true;
        commands.push_back({MissionScriptCommandType::destroy_object_source, power_switch});
        commands.push_back({MissionScriptCommandType::destroy_object_source, security_gate_source});
        queueCheckpoint();
    }
    if (context.interact) {
        if (spatialDistance(
                context.player_x, context.player_y, context.player_z,
                128.0, -2285.0, 5131.0) <= switch_radius) {
            commands.push_back({MissionScriptCommandType::teleport_to_source, lower_elevator_switch});
        } else if (spatialDistance(
                context.player_x, context.player_y, context.player_z,
                429.0, -245.0, 4770.0) <= switch_radius) {
            commands.push_back({MissionScriptCommandType::teleport_to_source, upper_elevator_switch});
        } else if (spatialDistance(
                context.player_x, context.player_y, context.player_z,
                -2236.0, 989.0, -9937.0) <= switch_radius) {
            commands.push_back({MissionScriptCommandType::teleport_to_source, upper_station_switch});
        } else if (spatialDistance(
                context.player_x, context.player_y, context.player_z,
                -1927.0, -252.0, -9550.0) <= switch_radius) {
            commands.push_back({MissionScriptCommandType::teleport_to_source, lower_station_switch});
        }
    }
    if (context.interact && !state_.upper_bomb_tagged &&
        state_.initial_objectives_complete && horizontalDistance(
            context.player_x, context.player_z, upper_bomb_x, upper_bomb_z) <= 650.0) {
        state_.upper_bomb_tagged = true;
        queueCheckpoint();
    }
    if (!state_.finale_started && state_.upper_bomb_tagged && horizontalDistance(
            context.player_x, context.player_z, final_bomb_x, final_bomb_z) <= 900.0) {
        state_.finale_started = true;
        commands.push_back({MissionScriptCommandType::start_finale, final_subway_bomb_source});
    }

    constexpr std::array<std::array<double, 2U>, 3U> scripted_event_positions{
        std::array{1513.0, 4660.0},
        std::array{-75.0, -1544.0},
        std::array{-75.0, -8791.0},
    };
    for (std::size_t index = 0U; index < scripted_event_sources.size(); ++index) {
        const auto mask = static_cast<std::uint8_t>(1U << index);
        const auto requires_bomb_tag = index != 0U;
        if ((state_.scripted_events_mask & mask) != 0U ||
            (requires_bomb_tag && !state_.upper_bomb_tagged) ||
            horizontalDistance(
                context.player_x,
                context.player_z,
                scripted_event_positions[index][0],
                scripted_event_positions[index][1]) > 760.0) {
            continue;
        }
        state_.scripted_events_mask = static_cast<std::uint8_t>(
            state_.scripted_events_mask | mask);
        // Class 0x5d is the native radio/message trigger. It does not take
        // ownership of the gameplay camera; audio is intentionally deferred.
        break;
    }

    if (checkpoint_pending_) {
        commands.push_back({MissionScriptCommandType::capture_checkpoint, 0U});
        checkpoint_pending_ = false;
    }
    if (mission_failed_pending_) {
        commands.push_back({MissionScriptCommandType::mission_failed, 0U});
        mission_failed_pending_ = false;
    }

    const auto live_hostiles = static_cast<std::size_t>(std::ranges::count_if(
        context.actors,
        [](const MissionScriptActorSnapshot& actor) {
            return actor.hostile && actor.alive && actor.room_active;
        }));
    if (global_spawn_cooldown_ != 0U) {
        return commands;
    }

    const auto candidate = std::ranges::find_if(
        context.actors,
        [&](const MissionScriptActorSnapshot& actor) {
            if (actor.object >= actors_.size()) {
                return false;
            }
            const auto& slot = actors_[actor.object];
            if (!slot.recyclable || !slot.pending || slot.cooldown != 0U ||
                actor.alive || !actor.room_active ||
                (live_hostiles >= mission_reinforcement_live_cap &&
                 slot.source_index != bank_reinforcement_source)) {
                return false;
            }
            if (slot.source_index == bank_reinforcement_source) {
                // The finite bank attackers enter through the authored doorway
                // while Gabe is covering the technician; requiring the slot to
                // leave the whole view can deadlock that objective.
                return actor.player_distance >= 480.0;
            }
            return !actor.visible_from_player &&
                actor.player_distance >= mission_reinforcement_hidden_distance;
        });
    if (candidate != context.actors.end()) {
        actors_[candidate->object].pending = false;
        global_spawn_cooldown_ = mission_reinforcement_spacing_updates;
        commands.push_back({MissionScriptCommandType::respawn_actor, candidate->object});
    }
    return commands;
}

bool MissionScriptRuntime::repeatable(std::uint16_t object) const noexcept {
    return object < actors_.size() && actors_[object].recyclable;
}

bool MissionScriptRuntime::initiallyDormant(std::uint16_t object) const noexcept {
    return object < actors_.size() && actors_[object].initially_dormant;
}

} // namespace sf::game
