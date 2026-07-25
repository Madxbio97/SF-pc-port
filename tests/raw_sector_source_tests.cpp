#include "sf/core/error.hpp"
#include "sf/disc/raw_sector_source.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

void writeTrack(const std::filesystem::path& path) {
    std::vector<std::byte> bytes(3U * sf::disc::RawSectorSource::raw_sector_size);
    std::fill_n(
        bytes.begin(), sf::disc::RawSectorSource::raw_sector_size, std::byte{0x09});
    std::fill_n(
        bytes.begin() + sf::disc::RawSectorSource::raw_sector_size,
        sf::disc::RawSectorSource::raw_sector_size,
        std::byte{0x11});
    std::fill_n(
        bytes.begin() + 2U * sf::disc::RawSectorSource::raw_sector_size,
        sf::disc::RawSectorSource::raw_sector_size,
        std::byte{0x22});

    std::ofstream stream{path, std::ios::binary};
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "Cannot write temporary raw track");
}

std::byte patternedSectorValue(std::uint32_t logical_lba) {
    return static_cast<std::byte>((logical_lba * 37U + 0x31U) & 0xffU);
}

void writePatternedTrack(
    const std::filesystem::path& path,
    std::uint32_t logical_sector_count) {
    std::vector<std::byte> bytes(
        (static_cast<std::size_t>(logical_sector_count) + 1U) *
        sf::disc::RawSectorSource::raw_sector_size,
        std::byte{0x09});
    for (std::uint32_t lba = 0U; lba < logical_sector_count; ++lba) {
        std::fill_n(
            bytes.begin() + static_cast<std::ptrdiff_t>(
                                (static_cast<std::size_t>(lba) + 1U) *
                                sf::disc::RawSectorSource::raw_sector_size),
            sf::disc::RawSectorSource::raw_sector_size,
            patternedSectorValue(lba));
    }

    std::ofstream stream{path, std::ios::binary};
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "Cannot write patterned temporary raw track");
}

void requirePatternedSector(
    std::span<const std::byte> bytes,
    std::uint32_t logical_lba,
    const char* message) {
    require(
        bytes.size() == sf::disc::RawSectorSource::raw_sector_size &&
            std::ranges::all_of(bytes, [expected = patternedSectorValue(logical_lba)](
                                           std::byte value) {
                return value == expected;
            }),
        message);
}

void writeCue(
    const std::filesystem::path& path,
    const char* track_mode = "MODE2/2352") {
    std::ofstream stream{path};
    stream << "FILE \"track.bin\" BINARY\n"
           << "  TRACK 01 " << track_mode << "\n"
           << "    INDEX 01 00:00:01\n";
    require(stream.good(), "Cannot write temporary CUE");
}

void testStreamingAndBounds() {
    sf::test::TemporaryDirectory temporary{"sf_raw_sector_tests"};
    const auto cue_path = temporary.path() / "disc.cue";
    writeTrack(temporary.path() / "track.bin");
    writeCue(cue_path);

    auto source = sf::disc::RawSectorSource::open(cue_path);
    require(source.sectorCount() == 2U, "CUE INDEX was not excluded from logical LBAs");

    const auto first = source.readSector(0U);
    require(first.front() == std::byte{0x11} && first.back() == std::byte{0x11},
        "First logical raw sector came from the wrong physical offset");

    std::vector<std::byte> both(2U * sf::disc::RawSectorSource::raw_sector_size);
    source.readSectors(0U, both);
    require(both.front() == std::byte{0x11}, "Bulk raw read lost its first sector");
    require(both[sf::disc::RawSectorSource::raw_sector_size] == std::byte{0x22},
        "Bulk raw read lost its second sector");

    try {
        static_cast<void>(source.readSector(2U));
        throw std::runtime_error{"Out-of-range raw LBA was accepted"};
    } catch (const sf::core::Error& error) {
        require(error.code() == sf::core::ErrorCode::invalid_argument,
            "Out-of-range raw LBA returned the wrong error");
    }

    try {
        std::vector<std::byte> partial(17U);
        source.readSectors(0U, partial);
        throw std::runtime_error{"Partial raw-sector destination was accepted"};
    } catch (const sf::core::Error& error) {
        require(error.code() == sf::core::ErrorCode::invalid_argument,
            "Partial raw-sector read returned the wrong error");
    }
}

void testModeValidation() {
    sf::test::TemporaryDirectory temporary{"sf_raw_sector_tests"};
    const auto cue_path = temporary.path() / "disc.cue";
    writeTrack(temporary.path() / "track.bin");
    writeCue(cue_path, "MODE1/2352");

    try {
        static_cast<void>(sf::disc::RawSectorSource::open(cue_path));
        throw std::runtime_error{"MODE1 track was accepted for XA streaming"};
    } catch (const sf::core::Error& error) {
        require(error.code() == sf::core::ErrorCode::unsupported,
            "MODE1 track returned the wrong raw-sector error");
    }
}

void testReadAheadAndRandomReadSemantics() {
    constexpr std::uint32_t sector_count = 96U;
    sf::test::TemporaryDirectory temporary{"sf_raw_sector_tests"};
    const auto cue_path = temporary.path() / "disc.cue";
    writePatternedTrack(temporary.path() / "track.bin", sector_count);
    writeCue(cue_path);

    auto source = sf::disc::RawSectorSource::open(cue_path);
    require(source.sectorCount() == sector_count,
        "Patterned track reported the wrong logical sector count");

    // Sector-at-a-time reads exercise the read-ahead window and its refill at
    // the window boundary used by XA streaming.
    for (std::uint32_t lba = 0U; lba < sector_count; ++lba) {
        const auto sector = source.readSector(lba);
        requirePatternedSector(sector, lba,
            "Sequential cached read returned the wrong sector");
    }

    // Moving both backwards and forwards must remain an exact random-access
    // operation, regardless of which read-ahead window is currently resident.
    constexpr std::array random_order{
        95U, 0U, 63U, 64U, 17U, 79U, 1U, 65U, 31U, 94U,
    };
    for (const auto lba : random_order) {
        const auto sector = source.readSector(lba);
        requirePatternedSector(sector, lba,
            "Random cached read returned the wrong sector");
    }

    // Seed a window at zero, then begin inside its tail so the request must
    // copy cached sectors and refill once without duplicating or skipping LBA.
    static_cast<void>(source.readSector(0U));
    std::vector<std::byte> overlapping(
        12U * sf::disc::RawSectorSource::raw_sector_size);
    source.readSectors(60U, overlapping);
    for (std::uint32_t index = 0U; index < 12U; ++index) {
        requirePatternedSector(
            std::span<const std::byte>{overlapping}.subspan(
                static_cast<std::size_t>(index) *
                    sf::disc::RawSectorSource::raw_sector_size,
                sf::disc::RawSectorSource::raw_sector_size),
            60U + index,
            "Cached bulk read across a window boundary returned the wrong sector");
    }

    // Requests larger than the cache use one direct host read. Verify that
    // path and a following cached random read agree byte-for-byte.
    std::vector<std::byte> direct(
        70U * sf::disc::RawSectorSource::raw_sector_size);
    source.readSectors(10U, direct);
    for (std::uint32_t index = 0U; index < 70U; ++index) {
        requirePatternedSector(
            std::span<const std::byte>{direct}.subspan(
                static_cast<std::size_t>(index) *
                    sf::disc::RawSectorSource::raw_sector_size,
                sf::disc::RawSectorSource::raw_sector_size),
            10U + index,
            "Direct bulk raw read returned the wrong sector");
    }
    const auto after_direct = source.readSector(11U);
    requirePatternedSector(after_direct, 11U,
        "Cached read after a direct bulk read returned the wrong sector");
}

} // namespace

int main() {
    try {
        testStreamingAndBounds();
        testModeValidation();
        testReadAheadAndRandomReadSemantics();
        std::cout << "Raw-sector source tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Raw-sector source tests failed: " << error.what() << '\n';
        return 1;
    }
}
