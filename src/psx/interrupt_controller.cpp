#include "sf/psx/interrupt_controller.hpp"

namespace sf::psx {

void InterruptController::reset() noexcept {
    state_ = {};
}

void InterruptController::setLine(InterruptSource source, bool active) noexcept {
    const auto bit = static_cast<std::uint16_t>(
        1U << static_cast<std::uint8_t>(source));
    const auto was_active = (state_.input_lines & bit) != 0U;
    if (active) {
        state_.input_lines = static_cast<std::uint16_t>(state_.input_lines | bit);
        if (!was_active) {
            state_.status = static_cast<std::uint16_t>(state_.status | bit);
        }
    } else {
        state_.input_lines = static_cast<std::uint16_t>(state_.input_lines & ~bit);
    }
}

void InterruptController::pulse(InterruptSource source) noexcept {
    setLine(source, false);
    setLine(source, true);
    setLine(source, false);
}

void InterruptController::acknowledge(
    std::uint16_t value,
    std::uint16_t write_mask) noexcept {
    const auto preserved = static_cast<std::uint16_t>(~write_mask);
    const auto acknowledgement = static_cast<std::uint16_t>(preserved | value);
    state_.status = static_cast<std::uint16_t>(
        state_.status & acknowledgement & valid_bits);
}

void InterruptController::writeMask(
    std::uint16_t value,
    std::uint16_t write_mask) noexcept {
    state_.mask = static_cast<std::uint16_t>(
        ((state_.mask & ~write_mask) | (value & write_mask)) & valid_bits);
}

bool InterruptController::restoreState(const InterruptControllerState& state) noexcept {
    if (((state.status | state.mask | state.input_lines) & ~valid_bits) != 0U) {
        return false;
    }
    state_ = state;
    return true;
}

} // namespace sf::psx
