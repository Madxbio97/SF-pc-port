#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sf::assets {

enum class TimPixelMode : std::uint8_t {
    indexed4 = 0,
    indexed8 = 1,
    direct16 = 2,
    direct24 = 3,
};

struct TimBlock {
    std::uint16_t x{};
    std::uint16_t y{};
    std::uint16_t width_words{};
    std::uint16_t height{};
    std::vector<std::uint16_t> words;
};

class TimImage final {
public:
    [[nodiscard]] static TimImage parse(std::span<const std::byte> bytes);

    [[nodiscard]] TimPixelMode mode() const noexcept { return mode_; }
    [[nodiscard]] const std::optional<TimBlock>& clut() const noexcept { return clut_; }
    [[nodiscard]] const TimBlock& pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::uint16_t displayWidth() const noexcept;
    [[nodiscard]] std::uint16_t displayHeight() const noexcept { return pixels_.height; }

private:
    TimImage(TimPixelMode mode, std::optional<TimBlock> clut, TimBlock pixels);

    TimPixelMode mode_{};
    std::optional<TimBlock> clut_;
    TimBlock pixels_;
};

} // namespace sf::assets
