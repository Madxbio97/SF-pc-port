#include "sf/disc/iso9660.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>

namespace sf::disc {
namespace {

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated ISO9660 integer"};
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::string trimRight(std::string value) {
    while (!value.empty() && value.back() == ' ') {
        value.pop_back();
    }
    return value;
}

std::string normalizeComponent(std::string value) {
    if (const auto version = value.find(';'); version != std::string::npos) {
        value.resize(version);
    }
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> components;
    std::string current;
    for (const auto character : path) {
        if (character == '/' || character == '\\') {
            if (!current.empty()) {
                components.push_back(normalizeComponent(std::move(current)));
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        components.push_back(normalizeComponent(std::move(current)));
    }
    return components;
}

DirectoryEntry parseRecord(std::span<const std::byte> record) {
    if (record.size() < 34) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated ISO9660 directory record"};
    }
    const auto name_length = std::to_integer<std::size_t>(record[32]);
    if (33U + name_length > record.size()) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid ISO9660 file identifier"};
    }

    std::string name;
    name.reserve(name_length);
    for (std::size_t i = 0; i < name_length; ++i) {
        name.push_back(static_cast<char>(std::to_integer<unsigned char>(record[33 + i])));
    }

    return DirectoryEntry{
        normalizeComponent(std::move(name)),
        readLe32(record, 2),
        readLe32(record, 10),
        (std::to_integer<unsigned int>(record[25]) & 0x02U) != 0,
    };
}

} // namespace

Iso9660Image::Iso9660Image(DataTrack track, std::ifstream stream)
    : track_(std::move(track)), stream_(std::move(stream)) {}

Iso9660Image Iso9660Image::open(const std::filesystem::path& cue_path) {
    auto cue = CueSheet::load(cue_path);
    auto track = cue.dataTrack();
    std::ifstream stream{track.binary_path, std::ios::binary};
    if (!stream) {
        throw core::Error{core::ErrorCode::io, "Cannot open track binary: " + track.binary_path.string()};
    }

    Iso9660Image image{std::move(track), std::move(stream)};
    const auto descriptor = image.readSector(16);
    constexpr std::array magic{
        std::byte{'C'}, std::byte{'D'}, std::byte{'0'}, std::byte{'0'}, std::byte{'1'},
    };
    if (descriptor[0] != std::byte{1} ||
        !std::equal(magic.begin(), magic.end(), descriptor.begin() + 1) ||
        descriptor[6] != std::byte{1}) {
        throw core::Error{core::ErrorCode::invalid_format, "ISO9660 primary volume descriptor was not found"};
    }

    image.volume_id_.reserve(32);
    for (std::size_t i = 40; i < 72; ++i) {
        image.volume_id_.push_back(static_cast<char>(std::to_integer<unsigned char>(descriptor[i])));
    }
    image.volume_id_ = trimRight(std::move(image.volume_id_));

    const auto root_length = std::to_integer<std::size_t>(descriptor[156]);
    if (root_length == 0 || 156U + root_length > descriptor.size()) {
        throw core::Error{core::ErrorCode::invalid_format, "ISO9660 root record is invalid"};
    }
    image.root_ = parseRecord(std::span{descriptor}.subspan(156, root_length));
    image.root_.name = "/";
    if (!image.root_.is_directory) {
        throw core::Error{core::ErrorCode::invalid_format, "ISO9660 root record is not a directory"};
    }
    return image;
}

std::array<std::byte, Iso9660Image::logical_sector_size> Iso9660Image::readSector(std::uint32_t lba) {
    const auto physical_lba = static_cast<std::uint64_t>(track_.index_lba) + lba;
    const auto offset = physical_lba * track_.sectorSize() + track_.userDataOffset();
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw core::Error{core::ErrorCode::io, "Disc offset exceeds host stream limits"};
    }

    std::array<std::byte, logical_sector_size> sector{};
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.read(reinterpret_cast<char*>(sector.data()), static_cast<std::streamsize>(sector.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(sector.size())) {
        throw core::Error{core::ErrorCode::io, "Unexpected end of track while reading LBA " + std::to_string(lba)};
    }
    return sector;
}

std::vector<std::byte> Iso9660Image::readExtent(std::uint32_t lba, std::uint32_t size) {
    std::vector<std::byte> result(size);
    std::size_t written = 0;
    std::uint32_t sector_index = 0;
    while (written < result.size()) {
        const auto sector = readSector(lba + sector_index);
        const auto count = std::min(sector.size(), result.size() - written);
        std::copy_n(sector.begin(), count, result.begin() + static_cast<std::ptrdiff_t>(written));
        written += count;
        ++sector_index;
    }
    return result;
}

std::vector<std::byte> Iso9660Image::readRawExtent(
    std::uint32_t lba,
    std::uint32_t sector_count) {
    const auto sector_size = static_cast<std::uint64_t>(track_.sectorSize());
    const auto physical_lba = static_cast<std::uint64_t>(track_.index_lba) + lba;
    const auto offset = physical_lba * sector_size;
    const auto byte_count = static_cast<std::uint64_t>(sector_count) * sector_size;
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        byte_count > std::numeric_limits<std::size_t>::max()) {
        throw core::Error{core::ErrorCode::io, "Raw disc extent exceeds host stream limits"};
    }

    std::vector<std::byte> result(static_cast<std::size_t>(byte_count));
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.read(
        reinterpret_cast<char*>(result.data()),
        static_cast<std::streamsize>(byte_count));
    if (stream_.gcount() != static_cast<std::streamsize>(byte_count)) {
        throw core::Error{core::ErrorCode::io, "Unexpected end of track in raw file extent"};
    }
    return result;
}

std::vector<DirectoryEntry> Iso9660Image::readDirectory(const DirectoryEntry& directory) {
    if (!directory.is_directory) {
        throw core::Error{core::ErrorCode::invalid_argument, directory.name + " is not a directory"};
    }
    const auto bytes = readExtent(directory.extent_lba, directory.size);
    std::vector<DirectoryEntry> entries;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto record_length = std::to_integer<std::size_t>(bytes[offset]);
        if (record_length == 0) {
            offset = ((offset / logical_sector_size) + 1) * logical_sector_size;
            continue;
        }
        if (record_length > bytes.size() - offset) {
            throw core::Error{core::ErrorCode::invalid_format, "Directory record extends beyond its extent"};
        }
        const auto record = std::span{bytes}.subspan(offset, record_length);
        const auto name_length = std::to_integer<std::size_t>(record[32]);
        const bool dot_entry = name_length == 1 &&
            (record[33] == std::byte{0} || record[33] == std::byte{1});
        if (!dot_entry) {
            entries.push_back(parseRecord(record));
        }
        offset += record_length;
    }
    return entries;
}

std::vector<DirectoryEntry> Iso9660Image::list(const std::string& path) {
    const auto entry = path.empty() || path == "/" ? root_ : find(path);
    return readDirectory(entry);
}

DirectoryEntry Iso9660Image::find(const std::string& path) {
    const auto components = splitPath(path);
    auto current = root_;
    for (const auto& component : components) {
        const auto entries = readDirectory(current);
        const auto match = std::ranges::find(entries, component, &DirectoryEntry::name);
        if (match == entries.end()) {
            throw core::Error{core::ErrorCode::not_found, "File not found in disc image: " + path};
        }
        current = *match;
    }
    return current;
}

std::vector<std::byte> Iso9660Image::readFile(const std::string& path) {
    const auto entry = find(path);
    if (entry.is_directory) {
        throw core::Error{core::ErrorCode::invalid_argument, path + " is a directory"};
    }
    return readExtent(entry.extent_lba, entry.size);
}

RawSectorFile Iso9660Image::readRawSectorFile(const std::string& path) {
    const auto entry = find(path);
    if (entry.is_directory) {
        throw core::Error{core::ErrorCode::invalid_argument, path + " is a directory"};
    }
    const auto sector_count = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(entry.size) + logical_sector_size - 1U) /
        logical_sector_size);
    return RawSectorFile{
        track_.sectorSize(),
        sector_count,
        readRawExtent(entry.extent_lba, sector_count),
    };
}

} // namespace sf::disc
