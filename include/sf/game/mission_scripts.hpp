#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sf::game {

struct MissionScriptActorSnapshot {
    std::uint16_t object{};
    bool hostile{};
    bool alive{};
    bool room_active{};
    bool visible_from_player{};
    double player_distance{};
};

struct MissionScriptUpdateContext {
    std::span<const MissionScriptActorSnapshot> actors;
    double player_x{};
    double player_y{};
    double player_z{};
    bool interact{};
};

enum class MissionScriptCommandType : std::uint8_t {
    respawn_actor,
    destroy_object_source,
    capture_checkpoint,
    mission_failed,
    teleport_to_source,
    start_finale,
};

struct MissionScriptCommand {
    MissionScriptCommandType type{MissionScriptCommandType::respawn_actor};
    // Actor commands use a scene-object slot. Object/event commands use the
    // native SUBWAY.BIN source index named by the command type.
    std::uint16_t object{};
};

struct GeorgiaMissionState {
    bool bank_assault_started{};
    bool cbdc_protected{};
    bool security_bypassed{};
    bool kravitch_eliminated{};
    bool communications_destroyed{};
    bool upper_bomb_tagged{};
    bool initial_objectives_complete{};
    bool finale_started{};
    bool failed{};
    bool bomb_warning_issued{};
    std::uint8_t bank_attackers_eliminated{};
    std::uint8_t scripted_events_mask{};
};

struct MissionCinematicCamera {
    double x{};
    double y{};
    double z{};
    double target_x{};
    double target_y{};
    double target_z{};
};

struct OpeningEncounterPoint {
    double x{};
    double y{};
    double z{};
};

struct OpeningEncounterLane {
    OpeningEncounterPoint cbdc;
    OpeningEncounterPoint hostile_spawn;
    OpeningEncounterPoint hostile_midpoint;
    OpeningEncounterPoint hostile_landing;
    OpeningEncounterPoint hostile_hold;
};

// Recovered from room 74's retail dynamic descriptors: descriptor 6 owns the
// two CHEMO positions and descriptor 26 owns both TERRO wall routes. The VM
// supplies each live spawn; the remaining nodes are the exact post-rail path.
inline constexpr std::array<OpeningEncounterLane, 2U> opening_encounter_lanes{
    OpeningEncounterLane{
        {3925.0, -2133.0, 1670.0},
        {3524.0, -2741.0, 8329.0},
        {3522.0, -2734.0, 7243.0},
        {3503.0, -2133.0, 7123.0},
        {3236.0, -2133.0, 4483.0},
    },
    OpeningEncounterLane{
        {5340.0, -2133.0, 3176.0},
        {4189.0, -2741.0, 8322.0},
        {3863.0, -2734.0, 6745.0},
        {3844.0, -2133.0, 6625.0},
        {2989.0, -2133.0, 4376.0},
    },
};

// SUBWAY source 35 (class 0x08) is the eye rail and linked source 36
// (class 0x09) is its aim rail. Source 35 advances at speed 30; native
// FUN_80020f48 synchronizes source 36 by eye-segment fraction. Class 0x73
// sources 64/65 only switch environment/visibility state.
class OpeningCinematicCameraRuntime final {
public:
    [[nodiscard]] MissionCinematicCamera sample(unsigned int updates) const noexcept;
};

inline constexpr std::uint8_t map_fade_release_per_frame = 15U;

// Recovered from FUN_800ca718/FUN_800ca780/FUN_800c8ee8: internal intensity
// starts at 255, releases once before the first draw, then by 15 per 20 Hz.
class MapFadeRuntime final {
public:
    void resetFromBlack() noexcept {
        intensity_ = static_cast<std::uint8_t>(0xffU - map_fade_release_per_frame);
    }
    void clear() noexcept {
        intensity_ = 0U;
    }
    void advance() noexcept;
    [[nodiscard]] std::uint8_t intensity() const noexcept { return intensity_; }

private:
    std::uint8_t intensity_{};
};

// Georgia Street's overlay recycles authored actor slots and links otherwise
// unrelated switches, doors, bombs and cutscene triggers by source index.
// This class owns that mission state; combat AI and rendering consume only its
// explicit commands and never duplicate the level's event policy.
class MissionScriptRuntime final {
public:
    explicit MissionScriptRuntime(std::size_t object_count = 0U);

    void resize(std::size_t object_count);
    void configureActor(
        std::uint16_t object,
        std::uint16_t source_index,
        bool hostile,
        std::uint32_t ai_parameter) noexcept;
    void reset() noexcept;
    void actorKilled(std::uint16_t object) noexcept;
    void objectDamaged(std::uint16_t source_index, bool destroyed) noexcept;
    void objectDestroyed(std::uint16_t source_index) noexcept;
    [[nodiscard]] std::vector<MissionScriptCommand> update(
        const MissionScriptUpdateContext& context) noexcept;

    // True means that the native scene slot may be reactivated. SUBWAY.BIN
    // marks the four transient hostile slots with ai_parameter == 1.
    [[nodiscard]] bool repeatable(std::uint16_t object) const noexcept;
    [[nodiscard]] bool initiallyDormant(std::uint16_t object) const noexcept;
    [[nodiscard]] const GeorgiaMissionState& state() const noexcept { return state_; }

private:
    struct ActorSlot {
        std::uint16_t source_index{};
        bool configured{};
        bool recyclable{};
        bool initially_dormant{};
        bool pending{};
        unsigned int cooldown{};
    };

    void queueCheckpoint() noexcept;
    void updateInitialObjectiveCompletion() noexcept;

    std::vector<ActorSlot> actors_;
    GeorgiaMissionState state_{};
    unsigned int global_spawn_cooldown_{};
    bool gate_open_pending_{};
    bool initial_doors_open_pending_{};
    bool checkpoint_pending_{};
    bool mission_failed_pending_{};
};

inline constexpr unsigned int mission_reinforcement_delay_updates = 30U;
inline constexpr unsigned int mission_reinforcement_spacing_updates = 7U;
inline constexpr std::size_t mission_reinforcement_live_cap = 6U;
inline constexpr double mission_reinforcement_hidden_distance = 1200.0;
inline constexpr std::uint8_t mission_bank_attacker_count = 5U;
inline constexpr unsigned int opening_car_explosion_update = 72U;
inline constexpr unsigned int opening_cinematic_duration_updates = 194U + 1U;

} // namespace sf::game
