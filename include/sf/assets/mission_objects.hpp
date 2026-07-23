#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sf::assets {

struct MissionTransform {
    std::array<std::int16_t, 9> rotation{};
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
};

struct MissionPathPoint {
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
};

struct MissionObject {
    std::uint32_t type{};
    MissionTransform transform;
    // Native SUBWAY.BIN object attributes.  The low byte identifies the
    // equipped item for actors; the remaining bits are mission/object flags.
    std::uint32_t attributes{};
    std::uint32_t ai_parameter{};
    std::uint32_t path_data_offset{};
    // Native object index linked by SUBWAY.OVL. Actors use it for protected
    // objects, switches for elevators/gates and invisible triggers for their
    // authored event target. -1 means that the object owns the event itself.
    std::int32_t linked_object{-1};
    std::vector<MissionPathPoint> patrol_path;
    bool patrol_path_loops{};
    std::uint8_t patrol_loop_start{};
    std::int16_t maximum_health{};
    std::int16_t health{};
};

struct MissionObjectDefinition {
    std::uint32_t class_id{};
    std::string primary_model;
    std::string secondary_model;
};

class MissionObjects final {
public:
    [[nodiscard]] static MissionObjects parse(std::span<const std::byte> bytes);

    [[nodiscard]] std::span<const MissionObject> objects() const noexcept { return objects_; }
    [[nodiscard]] std::span<const MissionObjectDefinition> definitions() const noexcept {
        return definitions_;
    }
    [[nodiscard]] std::size_t roomCount() const noexcept { return room_objects_.size(); }
    [[nodiscard]] std::size_t playerIndex() const noexcept { return player_index_; }
    [[nodiscard]] const MissionObject& player() const noexcept { return objects_[player_index_]; }
    [[nodiscard]] const MissionObjectDefinition& definition(std::size_t index) const;
    [[nodiscard]] std::span<const std::uint16_t> objectsInRoom(std::size_t room) const;
    [[nodiscard]] std::span<const std::uint16_t> roomsContainingObject(
        std::size_t object) const;

private:
    MissionObjects(
        std::vector<MissionObject> objects,
        std::vector<MissionObjectDefinition> definitions,
        std::vector<std::vector<std::uint16_t>> room_objects,
        std::vector<std::vector<std::uint16_t>> object_rooms,
        std::size_t player_index);

    std::vector<MissionObject> objects_;
    std::vector<MissionObjectDefinition> definitions_;
    std::vector<std::vector<std::uint16_t>> room_objects_;
    std::vector<std::vector<std::uint16_t>> object_rooms_;
    std::size_t player_index_{};
};

} // namespace sf::assets
