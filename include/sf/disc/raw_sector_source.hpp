#pragma once

#include "sf/disc/cue_sheet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace sf::disc {

// Seekable, bounded access to the original 2352-byte MODE2 sectors. Unlike
// Iso9660Image::readRawSectorFile(), this source never loads a complete XA
// stream into memory.
class RawSectorSource final {
public:
    static constexpr std::size_t raw_sector_size = 2352;
    using Sector = std::array<std::byte, raw_sector_size>;

    [[nodiscard]] static RawSectorSource open(const std::filesystem::path& cue_path);

    RawSectorSource(RawSectorSource&&) noexcept = default;
    RawSectorSource& operator=(RawSectorSource&&) noexcept = default;
    RawSectorSource(const RawSectorSource&) = delete;
    RawSectorSource& operator=(const RawSectorSource&) = delete;

    [[nodiscard]] std::uint32_t sectorCount() const noexcept { return sector_count_; }
    [[nodiscard]] const std::filesystem::path& binaryPath() const noexcept {
        return track_.binary_path;
    }

    [[nodiscard]] Sector readSector(std::uint32_t lba);
    void readSectors(std::uint32_t first_lba, std::span<std::byte> destination);

private:
    RawSectorSource(DataTrack track, std::ifstream stream, std::uint32_t sector_count);

    [[nodiscard]] std::uint64_t byteOffset(std::uint32_t lba) const;
    void readTrackSectors(std::uint32_t first_lba, std::span<std::byte> destination);
    void refillReadAhead(std::uint32_t first_lba);
    [[nodiscard]] bool readAheadContains(std::uint32_t lba) const noexcept;

    static constexpr std::size_t read_ahead_sector_count = 64U;

    DataTrack track_;
    std::ifstream stream_;
    std::uint32_t sector_count_{};
    std::vector<std::byte> read_ahead_;
    std::uint32_t read_ahead_first_lba_{};
    std::uint32_t read_ahead_count_{};
    std::uint32_t next_stream_lba_{};
    bool stream_position_valid_{};
};

} // namespace sf::disc
