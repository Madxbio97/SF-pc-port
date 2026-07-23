#pragma once

#include <cstdint>

namespace sf::psx {

enum class InterruptSource : std::uint8_t {
    vblank = 0U,
    gpu = 1U,
    cdrom = 2U,
    dma = 3U,
    timer0 = 4U,
    timer1 = 5U,
    timer2 = 6U,
    controller = 7U,
    sio = 8U,
    spu = 9U,
    lightpen = 10U,
};

struct InterruptControllerState {
    std::uint16_t status{};
    std::uint16_t mask{};
    std::uint16_t input_lines{};
};

// PS1 interrupt controller. Device inputs are edge-latched into I_STAT; the
// CPU observes the masked aggregate through COP0 Cause.IP2.
class InterruptController final {
public:
    static constexpr std::uint16_t valid_bits = 0x07ffU;

    void reset() noexcept;
    void setLine(InterruptSource source, bool active) noexcept;
    void pulse(InterruptSource source) noexcept;
    void acknowledge(std::uint16_t value, std::uint16_t write_mask = 0xffffU) noexcept;
    void writeMask(std::uint16_t value, std::uint16_t write_mask = 0xffffU) noexcept;

    [[nodiscard]] std::uint16_t status() const noexcept { return state_.status; }
    [[nodiscard]] std::uint16_t mask() const noexcept { return state_.mask; }
    [[nodiscard]] std::uint16_t inputLines() const noexcept {
        return state_.input_lines;
    }
    [[nodiscard]] bool cpuLine() const noexcept {
        return (state_.status & state_.mask) != 0U;
    }

    [[nodiscard]] InterruptControllerState captureState() const noexcept {
        return state_;
    }
    [[nodiscard]] bool restoreState(const InterruptControllerState& state) noexcept;

private:
    InterruptControllerState state_{};
};

} // namespace sf::psx
