#pragma once

#include "sf/disc/cue_sheet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace sf::disc {

struct DirectoryEntry {
    std::string name;
    std::uint32_t extent_lba{};
    std::uint32_t size{};
    bool is_directory{};
};

struct RawSectorFile {
    std::uint32_t sector_size{};
    std::uint32_t sector_count{};
    std::vector<std::byte> bytes;
};

class Iso9660Image final {
public:
    [[nodiscard]] static Iso9660Image open(const std::filesystem::path& cue_path);

    Iso9660Image(Iso9660Image&&) noexcept = default;
    Iso9660Image& operator=(Iso9660Image&&) noexcept = default;
    Iso9660Image(const Iso9660Image&) = delete;
    Iso9660Image& operator=(const Iso9660Image&) = delete;

    [[nodiscard]] const std::string& volumeId() const noexcept { return volume_id_; }
    [[nodiscard]] const std::filesystem::path& binaryPath() const noexcept {
        return track_.binary_path;
    }
    [[nodiscard]] std::vector<DirectoryEntry> list(const std::string& path);
    [[nodiscard]] DirectoryEntry find(const std::string& path);
    [[nodiscard]] std::vector<std::byte> readFile(const std::string& path);
    [[nodiscard]] RawSectorFile readRawSectorFile(const std::string& path);

private:
    static constexpr std::size_t logical_sector_size = 2048;

    Iso9660Image(DataTrack track, std::ifstream stream);

    [[nodiscard]] std::array<std::byte, logical_sector_size> readSector(std::uint32_t lba);
    [[nodiscard]] std::vector<std::byte> readExtent(std::uint32_t lba, std::uint32_t size);
    [[nodiscard]] std::vector<std::byte> readRawExtent(
        std::uint32_t lba,
        std::uint32_t sector_count);
    [[nodiscard]] std::vector<DirectoryEntry> readDirectory(const DirectoryEntry& directory);

    DataTrack track_;
    std::ifstream stream_;
    std::string volume_id_;
    DirectoryEntry root_;
};

} // namespace sf::disc
