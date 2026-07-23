#include "sf/assets/level_layout.hpp"

#include "sf/core/error.hpp"

#include <limits>
#include <string>
#include <utility>

namespace sf::assets {
namespace {

constexpr std::size_t resident_offset = 0x78;
constexpr std::size_t resident_size = 16;
constexpr std::size_t model_count_offset = 0x88;
constexpr std::size_t initial_room_offset = 0x8c;
constexpr std::size_t visibility_offset = 0x90;
constexpr std::size_t visibility_entry_size = 15;
constexpr std::uint8_t prefetch_marker = 0xfe;
constexpr std::uint8_t end_marker = 0xff;

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated level-layout integer"};
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint8_t readByte(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset >= bytes.size()) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated level-layout byte"};
    }
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

void addModel(
    std::vector<std::uint16_t>& destination,
    std::uint8_t value,
    std::size_t model_count,
    std::size_t room) {
    if (value >= model_count) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "Level room " + std::to_string(room) + " references an invalid model"};
    }
    destination.push_back(value);
}

} // namespace

LevelLayout::LevelLayout(
    std::uint16_t initial_room,
    std::vector<std::uint16_t> resident_models,
    std::vector<LevelVisibility> rooms)
    : initial_room_(initial_room),
      resident_models_(std::move(resident_models)),
      rooms_(std::move(rooms)) {}

LevelLayout LevelLayout::parse(
    std::span<const std::byte> bytes,
    std::size_t expected_model_count) {
    const auto model_count = static_cast<std::size_t>(readLe32(bytes, model_count_offset));
    if (model_count == 0 || model_count >= prefetch_marker ||
        model_count != expected_model_count ||
        model_count > (std::numeric_limits<std::size_t>::max() - visibility_offset) /
            visibility_entry_size ||
        visibility_offset + model_count * visibility_entry_size > bytes.size()) {
        throw core::Error{core::ErrorCode::invalid_format, "Level-layout model count is invalid"};
    }

    const auto initial_room_value = readLe32(bytes, initial_room_offset);
    if (initial_room_value >= model_count) {
        throw core::Error{core::ErrorCode::invalid_format, "Level-layout initial room is invalid"};
    }

    std::vector<std::uint16_t> resident_models;
    bool resident_terminated = false;
    for (std::size_t index = 0; index < resident_size; ++index) {
        const auto value = readByte(bytes, resident_offset + index);
        if (value == end_marker) {
            resident_terminated = true;
            break;
        }
        if (value == prefetch_marker) {
            throw core::Error{core::ErrorCode::invalid_format, "Invalid resident model marker"};
        }
        addModel(resident_models, value, model_count, initial_room_value);
    }
    if (!resident_terminated) {
        throw core::Error{core::ErrorCode::invalid_format, "Resident model list is unterminated"};
    }

    std::vector<LevelVisibility> rooms(model_count);
    for (std::size_t room = 0; room < model_count; ++room) {
        auto& visibility = rooms[room];
        bool prefetch = false;
        bool terminated = false;
        const auto entry_offset = visibility_offset + room * visibility_entry_size;
        for (std::size_t index = 0; index < visibility_entry_size; ++index) {
            const auto value = readByte(bytes, entry_offset + index);
            if (value == end_marker) {
                terminated = true;
                break;
            }
            if (value == prefetch_marker) {
                // Retail levels may split the prefetch tail into multiple
                // groups. Native visibility only needs their union.
                prefetch = true;
                continue;
            }
            addModel(
                prefetch ? visibility.prefetched_models : visibility.active_models,
                value,
                model_count,
                room);
        }
        if (!terminated) {
            throw core::Error{
                core::ErrorCode::invalid_format,
                "Level room " + std::to_string(room) + " is unterminated"};
        }
    }

    return LevelLayout{
        static_cast<std::uint16_t>(initial_room_value),
        std::move(resident_models),
        std::move(rooms)};
}

const LevelVisibility& LevelLayout::visibility(std::size_t room) const {
    if (room >= rooms_.size()) {
        throw core::Error{core::ErrorCode::invalid_argument, "Invalid level room index"};
    }
    return rooms_[room];
}

} // namespace sf::assets
