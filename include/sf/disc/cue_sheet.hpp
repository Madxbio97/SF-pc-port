#pragma once

#include <cstdint>
#include <filesystem>

namespace sf::disc {

enum class TrackMode {
    mode1_2048,
    mode1_2352,
    mode2_2352,
};

struct DataTrack {
    std::filesystem::path binary_path;
    TrackMode mode{};
    std::uint32_t index_lba{};

    [[nodiscard]] std::uint32_t sectorSize() const noexcept;
    [[nodiscard]] std::uint32_t userDataOffset() const noexcept;
};

class CueSheet final {
public:
    [[nodiscard]] static CueSheet load(const std::filesystem::path& cue_path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const DataTrack& dataTrack() const noexcept { return data_track_; }

private:
    std::filesystem::path path_;
    DataTrack data_track_;
};

} // namespace sf::disc
