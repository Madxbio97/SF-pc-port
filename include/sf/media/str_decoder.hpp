#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace sf::media {

struct MovieVideoFrame {
    int width{};
    int height{};
    double timestamp_seconds{};
    std::vector<std::uint8_t> rgba8888;
};

struct MovieAudioChunk {
    int sample_rate{};
    double timestamp_seconds{};
    std::vector<std::int16_t> stereo_samples;
};

using MovieEvent = std::variant<MovieVideoFrame, MovieAudioChunk>;

class StrDecoder final {
public:
    [[nodiscard]] static StrDecoder open(std::vector<std::byte> raw_sectors);

    ~StrDecoder();
    StrDecoder(StrDecoder&&) noexcept;
    StrDecoder& operator=(StrDecoder&&) noexcept;
    StrDecoder(const StrDecoder&) = delete;
    StrDecoder& operator=(const StrDecoder&) = delete;

    [[nodiscard]] std::optional<MovieEvent> next();
    [[nodiscard]] double framesPerSecond() const noexcept;
    [[nodiscard]] bool hasAudio() const noexcept;

private:
    struct Impl;
    explicit StrDecoder(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace sf::media
