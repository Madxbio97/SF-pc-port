#include "sf/assets/hog_archive.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace sf::assets {
namespace {

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated HOG integer"};
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::string normalize(std::string_view name) {
    std::string result{name};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

} // namespace

HogArchive::HogArchive(
    std::vector<std::byte> bytes,
    std::uint32_t identifier,
    std::vector<HogEntry> entries)
    : bytes_(std::move(bytes)), identifier_(identifier), entries_(std::move(entries)) {}

HogArchive HogArchive::parse(std::vector<std::byte> bytes) {
    constexpr std::size_t fixed_header_size = 20;
    if (bytes.size() < fixed_header_size) {
        throw core::Error{core::ErrorCode::invalid_format, "HOG header is truncated"};
    }

    const auto view = std::span<const std::byte>{bytes};
    const auto identifier = readLe32(view, 0);
    const auto count = readLe32(view, 4);
    const auto names_offset = readLe32(view, 12);
    const auto data_offset = readLe32(view, 16);
    if (count == 0) {
        throw core::Error{core::ErrorCode::invalid_format, "HOG has no entries"};
    }
    if (count > (std::numeric_limits<std::size_t>::max() - fixed_header_size) / 4U) {
        throw core::Error{core::ErrorCode::invalid_format, "HOG entry count is too large"};
    }

    const auto offset_table_end = fixed_header_size + static_cast<std::size_t>(count) * 4U;
    if (offset_table_end > names_offset || names_offset >= data_offset || data_offset > bytes.size()) {
        throw core::Error{core::ErrorCode::invalid_format, "HOG header offsets are invalid"};
    }

    std::vector<std::uint32_t> relative_offsets;
    relative_offsets.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto value = readLe32(view, fixed_header_size + index * 4U);
        if ((index == 0 && value != 0) ||
            (index > 0 && value < relative_offsets.back()) ||
            value >= bytes.size() - data_offset) {
            throw core::Error{core::ErrorCode::invalid_format, "HOG data offsets are invalid"};
        }
        relative_offsets.push_back(value);
    }

    std::vector<std::string> names;
    names.reserve(count);
    auto cursor = static_cast<std::size_t>(names_offset);
    while (names.size() < count) {
        if (cursor >= data_offset) {
            throw core::Error{core::ErrorCode::invalid_format, "HOG name table is truncated"};
        }
        const auto start = cursor;
        while (cursor < data_offset && bytes[cursor] != std::byte{0}) {
            ++cursor;
        }
        if (cursor == data_offset || cursor == start) {
            throw core::Error{core::ErrorCode::invalid_format, "HOG contains an invalid file name"};
        }
        std::string name;
        name.reserve(cursor - start);
        for (auto index = start; index < cursor; ++index) {
            const auto character = std::to_integer<unsigned char>(bytes[index]);
            if (character < 0x20U || character > 0x7eU) {
                throw core::Error{core::ErrorCode::invalid_format, "HOG file name is not ASCII"};
            }
            name.push_back(static_cast<char>(character));
        }
        names.push_back(std::move(name));
        ++cursor;
    }

    std::vector<HogEntry> entries;
    entries.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto begin = static_cast<std::size_t>(data_offset) + relative_offsets[index];
        const auto end = index + 1 < count
            ? static_cast<std::size_t>(data_offset) + relative_offsets[index + 1]
            : bytes.size();
        entries.push_back(HogEntry{std::move(names[index]), begin, end - begin});
    }
    return HogArchive{std::move(bytes), identifier, std::move(entries)};
}

std::span<const std::byte> HogArchive::file(std::string_view name) const {
    const auto normalized = normalize(name);
    const auto match = std::ranges::find_if(entries_, [&](const HogEntry& entry) {
        return normalize(entry.name) == normalized;
    });
    if (match == entries_.end()) {
        throw core::Error{core::ErrorCode::not_found, "File not found in HOG: " + std::string{name}};
    }
    return std::span<const std::byte>{bytes_}.subspan(match->offset, match->size);
}

} // namespace sf::assets
