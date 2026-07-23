#include "sf/assets/mission_objects.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace sf::assets {
namespace {

constexpr std::size_t header_size = 0x30;
constexpr std::size_t room_count_offset = 0x04;
constexpr std::size_t object_count_offset = 0x08;
constexpr std::size_t definition_count_offset = 0x0c;
constexpr std::size_t definition_table_offset = 0x10;
constexpr std::size_t object_table_offset = 0x14;
constexpr std::size_t player_index_offset = 0x1c;
constexpr std::size_t room_table_offset = 0x18;
constexpr std::size_t object_size = 0x4c;
constexpr std::size_t definition_size = 0x14;
constexpr std::size_t rotation_offset = 0x04;
constexpr std::size_t position_offset = 0x18;
constexpr std::size_t attributes_offset = 0x24;
constexpr std::size_t ai_parameter_offset = 0x28;
constexpr std::size_t path_data_offset = 0x2c;
constexpr std::size_t linked_object_offset = 0x30;
constexpr std::size_t maximum_health_offset = 0x3e;
constexpr std::size_t health_offset = 0x40;
constexpr std::size_t path_node_size = 12U;
constexpr std::size_t path_next_offset = 8U;
constexpr std::size_t path_marker_offset = 11U;
constexpr std::uint8_t path_end = 0xffU;
constexpr std::uint8_t path_marker = 0xcaU;

std::uint16_t readLe16(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint16_t)) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated mission-object integer"};
    }
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated mission-object integer"};
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::string readString(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset >= bytes.size()) {
        throw core::Error{core::ErrorCode::invalid_format, "Mission-object string offset is invalid"};
    }
    std::string result;
    while (offset < bytes.size() && bytes[offset] != std::byte{0}) {
        const auto character = std::to_integer<unsigned char>(bytes[offset]);
        if (character < 0x20U || character > 0x7eU) {
            throw core::Error{core::ErrorCode::invalid_format, "Mission-object model name is not ASCII"};
        }
        result.push_back(static_cast<char>(character));
        ++offset;
    }
    if (offset == bytes.size()) {
        throw core::Error{core::ErrorCode::invalid_format, "Mission-object model name is truncated"};
    }
    return result;
}

std::vector<MissionPathPoint> readPatrolPath(
    std::span<const std::byte> bytes,
    std::uint32_t path_offset,
    bool& loops,
    std::uint8_t& loop_start) {
    loops = false;
    loop_start = 0U;
    if (path_offset == 0U) {
        return {};
    }
    std::vector<MissionPathPoint> result;
    std::array<bool, 256U> visited{};
    std::array<std::int16_t, 256U> result_indices{};
    result_indices.fill(-1);
    auto node = std::uint8_t{};
    for (;;) {
        if (visited[node]) {
            // Some authored guard routes are closed loops. Preserve every
            // unique node once; gameplay reconnects the endpoints itself.
            loops = true;
            loop_start = static_cast<std::uint8_t>(std::max<std::int16_t>(
                result_indices[node], 0));
            break;
        }
        visited[node] = true;
        result_indices[node] = static_cast<std::int16_t>(result.size());
        const auto offset = static_cast<std::size_t>(path_offset) +
            static_cast<std::size_t>(node) * path_node_size;
        if (offset > bytes.size() || bytes.size() - offset < path_node_size ||
            std::to_integer<std::uint8_t>(bytes[offset + path_marker_offset]) != path_marker) {
            throw core::Error{core::ErrorCode::invalid_format, "Mission patrol path is invalid"};
        }
        result.push_back(MissionPathPoint{
            static_cast<std::int16_t>(readLe16(bytes, offset)),
            static_cast<std::int16_t>(readLe16(bytes, offset + 2U)),
            static_cast<std::int16_t>(readLe16(bytes, offset + 4U)),
        });
        const auto next = std::to_integer<std::uint8_t>(bytes[offset + path_next_offset]);
        if (next == path_end) {
            break;
        }
        node = next;
    }
    return result;
}

} // namespace

MissionObjects::MissionObjects(
    std::vector<MissionObject> objects,
    std::vector<MissionObjectDefinition> definitions,
    std::vector<std::vector<std::uint16_t>> room_objects,
    std::vector<std::vector<std::uint16_t>> object_rooms,
    std::size_t player_index)
    : objects_(std::move(objects)),
      definitions_(std::move(definitions)),
      room_objects_(std::move(room_objects)),
      object_rooms_(std::move(object_rooms)),
      player_index_(player_index) {}

MissionObjects MissionObjects::parse(std::span<const std::byte> bytes) {
    if (bytes.size() < header_size) {
        throw core::Error{core::ErrorCode::invalid_format, "Mission-object header is truncated"};
    }
    const auto room_count = static_cast<std::size_t>(readLe32(bytes, room_count_offset));
    const auto count = static_cast<std::size_t>(readLe32(bytes, object_count_offset));
    const auto definition_count = static_cast<std::size_t>(readLe32(bytes, definition_count_offset));
    const auto definitions_offset = static_cast<std::size_t>(readLe32(bytes, definition_table_offset));
    const auto table_offset = static_cast<std::size_t>(readLe32(bytes, object_table_offset));
    const auto rooms_offset = static_cast<std::size_t>(readLe32(bytes, room_table_offset));
    const auto player_index = static_cast<std::size_t>(readLe32(bytes, player_index_offset));
    if (room_count == 0 ||
        room_count > std::numeric_limits<std::uint16_t>::max() ||
        count == 0 || definition_count == 0 || player_index >= count ||
        definitions_offset > bytes.size() ||
        definition_count > (bytes.size() - definitions_offset) / definition_size ||
        table_offset > bytes.size() || rooms_offset > bytes.size() ||
        room_count > (bytes.size() - rooms_offset) / (2U * sizeof(std::uint32_t)) ||
        count > (bytes.size() - table_offset) / object_size) {
        throw core::Error{core::ErrorCode::invalid_format, "Mission-object table is invalid"};
    }

    std::vector<MissionObjectDefinition> definitions;
    definitions.reserve(definition_count);
    for (std::size_t index = 0; index < definition_count; ++index) {
        const auto offset = definitions_offset + index * definition_size;
        definitions.push_back(MissionObjectDefinition{
            readLe32(bytes, offset),
            readString(bytes, readLe32(bytes, offset + 4U)),
            readString(bytes, readLe32(bytes, offset + 0x0cU)),
        });
    }

    std::vector<MissionObject> objects;
    objects.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = table_offset + index * object_size;
        MissionObject object;
        object.type = readLe32(bytes, offset);
        if (object.type >= definition_count) {
            throw core::Error{core::ErrorCode::invalid_format, "Mission object has an invalid definition"};
        }
        for (std::size_t component = 0; component < object.transform.rotation.size(); ++component) {
            object.transform.rotation[component] = static_cast<std::int16_t>(
                readLe16(bytes, offset + rotation_offset + component * 2U));
        }
        object.transform.x = static_cast<std::int32_t>(readLe32(bytes, offset + position_offset));
        object.transform.y = static_cast<std::int32_t>(readLe32(bytes, offset + position_offset + 4U));
        object.transform.z = static_cast<std::int32_t>(readLe32(bytes, offset + position_offset + 8U));
        object.attributes = readLe32(bytes, offset + attributes_offset);
        object.ai_parameter = readLe32(bytes, offset + ai_parameter_offset);
        object.path_data_offset = readLe32(bytes, offset + path_data_offset);
        object.linked_object = static_cast<std::int32_t>(
            readLe32(bytes, offset + linked_object_offset));
        const auto class_id = definitions[object.type].class_id;
        // Actors and class 0x73 transition controllers use the same linked
        // 12-byte node format. Class 0x73 drives portal/environment switching;
        // the cinematic camera rails are class 0x08/0x09 sources 35/36.
        if (class_id == 0x01U || class_id == 0x35U || class_id == 0x73U) {
            object.patrol_path = readPatrolPath(
                bytes,
                object.path_data_offset,
                object.patrol_path_loops,
                object.patrol_loop_start);
        }
        object.maximum_health = static_cast<std::int16_t>(
            readLe16(bytes, offset + maximum_health_offset));
        object.health = static_cast<std::int16_t>(readLe16(bytes, offset + health_offset));
        objects.push_back(object);
    }

    std::vector<std::vector<std::uint16_t>> room_objects;
    std::vector<std::vector<std::uint16_t>> object_rooms(count);
    room_objects.reserve(room_count);
    for (std::size_t room = 0; room < room_count; ++room) {
        const auto room_offset = rooms_offset + room * 2U * sizeof(std::uint32_t);
        const auto room_object_count = static_cast<std::size_t>(readLe32(bytes, room_offset));
        const auto indices_offset = static_cast<std::size_t>(readLe32(bytes, room_offset + 4U));
        if (indices_offset > bytes.size() ||
            room_object_count > (bytes.size() - indices_offset) / sizeof(std::uint32_t)) {
            throw core::Error{core::ErrorCode::invalid_format, "Mission room-object list is invalid"};
        }
        std::vector<std::uint16_t> indices;
        indices.reserve(room_object_count);
        for (std::size_t index = 0; index < room_object_count; ++index) {
            const auto object_index = readLe32(bytes, indices_offset + index * sizeof(std::uint32_t));
            if (object_index >= count || object_index > std::numeric_limits<std::uint16_t>::max()) {
                throw core::Error{core::ErrorCode::invalid_format, "Mission room has an invalid object"};
            }
            const auto checked_object = static_cast<std::uint16_t>(object_index);
            indices.push_back(checked_object);
            object_rooms[checked_object].push_back(
                static_cast<std::uint16_t>(room));
        }
        room_objects.push_back(std::move(indices));
    }
    return MissionObjects{
        std::move(objects), std::move(definitions), std::move(room_objects),
        std::move(object_rooms), player_index};
}

const MissionObjectDefinition& MissionObjects::definition(std::size_t index) const {
    if (index >= definitions_.size()) {
        throw core::Error{core::ErrorCode::invalid_argument, "Mission-object definition is invalid"};
    }
    return definitions_[index];
}

std::span<const std::uint16_t> MissionObjects::objectsInRoom(std::size_t room) const {
    if (room >= room_objects_.size()) {
        throw core::Error{core::ErrorCode::invalid_argument, "Mission room is invalid"};
    }
    return room_objects_[room];
}

std::span<const std::uint16_t>
MissionObjects::roomsContainingObject(std::size_t object) const {
    if (object >= object_rooms_.size()) {
        throw core::Error{core::ErrorCode::invalid_argument,
                          "Mission object is invalid"};
    }
    return object_rooms_[object];
}

} // namespace sf::assets
