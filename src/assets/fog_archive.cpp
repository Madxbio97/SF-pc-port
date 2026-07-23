#include "sf/assets/fog_archive.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_set>
#include <utility>

namespace sf::assets {
namespace {

constexpr std::size_t header_size = 16;
constexpr std::size_t entry_size = 24;
constexpr std::size_t name_size = 16;

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated FOG integer"};
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

std::string readName(std::span<const std::byte> bytes, std::size_t offset) {
    std::string result;
    result.reserve(name_size);
    for (std::size_t index = 0; index < name_size; ++index) {
        const auto character = std::to_integer<unsigned char>(bytes[offset + index]);
        if (character == 0U) {
            break;
        }
        if (character < 0x20U || character > 0x7eU) {
            throw core::Error{core::ErrorCode::invalid_format, "FOG file name is not ASCII"};
        }
        result.push_back(static_cast<char>(character));
    }
    if (result.empty()) {
        throw core::Error{core::ErrorCode::invalid_format, "FOG contains an empty file name"};
    }
    return result;
}

} // namespace

FogArchive::FogArchive(
    std::vector<std::byte> bytes,
    std::uint32_t flags,
    std::uint32_t declared_sector_count,
    std::vector<FogEntry> entries)
    : bytes_(std::move(bytes)),
      flags_(flags),
      declared_sector_count_(declared_sector_count),
      entries_(std::move(entries)) {}

FogArchive FogArchive::parse(std::vector<std::byte> bytes) {
    if (bytes.size() < sector_size) {
        throw core::Error{core::ErrorCode::invalid_format, "FOG header sector is truncated"};
    }

    const auto view = std::span<const std::byte>{bytes};
    const auto flags = readLe32(view, 0);
    const auto declared_sector_count = readLe32(view, 4);
    if (readLe32(view, 8) != 0 || readLe32(view, 12) != 0) {
        throw core::Error{core::ErrorCode::invalid_format, "FOG reserved header fields are invalid"};
    }
    if (declared_sector_count <= 1U ||
        declared_sector_count > bytes.size() / sector_size) {
        throw core::Error{core::ErrorCode::invalid_format, "FOG declared size is invalid"};
    }

    std::vector<FogEntry> entries;
    std::unordered_set<std::string> names;
    std::uint64_t maximum_end = 1;
    for (std::size_t offset = header_size; offset + entry_size <= sector_size;
         offset += entry_size) {
        const auto first = std::to_integer<unsigned char>(bytes[offset]);
        if (first == 0U || first == 0xcdU) {
            break;
        }

        auto name = readName(view, offset);
        const auto start_sector = readLe32(view, offset + name_size);
        const auto sector_count = readLe32(view, offset + name_size + sizeof(std::uint32_t));
        if (start_sector == 0U && sector_count == 0U) {
            continue;
        }
        const auto end_sector = static_cast<std::uint64_t>(start_sector) + sector_count;
        if (start_sector < 1U || sector_count == 0U ||
            end_sector > declared_sector_count) {
            throw core::Error{core::ErrorCode::invalid_format,
                "FOG entry extent is invalid: " + name + " start=" +
                    std::to_string(start_sector) + " count=" +
                    std::to_string(sector_count) + " archive=" +
                    std::to_string(declared_sector_count)};
        }
        const auto overlaps = std::ranges::any_of(entries, [&](const FogEntry& entry) {
            const auto entry_end = static_cast<std::uint64_t>(entry.start_sector) +
                entry.sector_count;
            return start_sector < entry_end && entry.start_sector < end_sector;
        });
        if (overlaps) {
            throw core::Error{core::ErrorCode::invalid_format,
                "FOG entry extents overlap"};
        }
        if (!names.insert(normalize(name)).second) {
            throw core::Error{core::ErrorCode::invalid_format, "FOG contains duplicate file names"};
        }

        const auto data_offset = static_cast<std::size_t>(start_sector) * sector_size;
        const auto data_size = static_cast<std::size_t>(sector_count) * sector_size;
        entries.push_back(FogEntry{
            std::move(name), start_sector, sector_count, data_offset, data_size});
        maximum_end = std::max(maximum_end, end_sector);
    }
    if (entries.empty() || maximum_end != declared_sector_count) {
        throw core::Error{core::ErrorCode::invalid_format, "FOG file table is incomplete"};
    }

    return FogArchive{
        std::move(bytes), flags, declared_sector_count, std::move(entries)};
}

std::span<const std::byte> FogArchive::file(std::string_view name) const {
    const auto normalized = normalize(name);
    const auto match = std::ranges::find_if(entries_, [&](const FogEntry& entry) {
        return normalize(entry.name) == normalized;
    });
    if (match == entries_.end()) {
        throw core::Error{core::ErrorCode::not_found, "File not found in FOG: " + std::string{name}};
    }
    return std::span<const std::byte>{bytes_}.subspan(match->offset, match->size);
}

} // namespace sf::assets
