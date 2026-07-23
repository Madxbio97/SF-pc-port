#pragma once

#include <array>
#include <cstdint>

namespace sf::psx {

struct GteState {
    std::array<std::uint32_t, 32> data{};
    std::array<std::uint32_t, 32> control{};
};

// Integer Geometry Transformation Engine state used by original gameplay math.
// Unsupported commands remain an explicit deterministic VM stop.
class GteRuntime final {
public:
    [[nodiscard]] static std::uint32_t readData(
        const GteState& state,
        std::uint8_t index) noexcept;
    [[nodiscard]] static std::uint32_t readControl(
        const GteState& state,
        std::uint8_t index) noexcept;
    static void writeData(
        GteState& state,
        std::uint8_t index,
        std::uint32_t value) noexcept;
    static void writeControl(
        GteState& state,
        std::uint8_t index,
        std::uint32_t value) noexcept;
    [[nodiscard]] static bool executeCommand(
        GteState& state,
        std::uint32_t instruction) noexcept;
};

} // namespace sf::psx
