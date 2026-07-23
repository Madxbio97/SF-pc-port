#include "sf/psx/timers.hpp"

#include <algorithm>

namespace sf::psx {
namespace {

constexpr std::uint16_t mode_sync_enable = 1U << 0U;
constexpr std::uint16_t mode_sync_mask = 3U << 1U;
constexpr std::uint16_t mode_reset_at_target = 1U << 3U;
constexpr std::uint16_t mode_irq_at_target = 1U << 4U;
constexpr std::uint16_t mode_irq_at_maximum = 1U << 5U;
constexpr std::uint16_t mode_irq_repeat = 1U << 6U;
constexpr std::uint16_t mode_irq_toggle = 1U << 7U;
constexpr std::uint16_t mode_clock_mask = 3U << 8U;
constexpr std::uint16_t mode_irq_request = 1U << 10U;
constexpr std::uint16_t mode_reached_target = 1U << 11U;
constexpr std::uint16_t mode_reached_maximum = 1U << 12U;
constexpr std::uint16_t mode_write_mask = 0xe3ffU;
constexpr std::uint16_t counter_maximum = 0xffffU;
constexpr std::uint32_t counter_range = 0x10000U;

bool validChannel(std::size_t channel) noexcept {
    return channel < RootTimers::timer_count;
}

std::uint16_t syncMode(const RootTimerState& timer) noexcept {
    return static_cast<std::uint16_t>((timer.mode & mode_sync_mask) >> 1U);
}

std::uint16_t clockSource(const RootTimerState& timer) noexcept {
    return static_cast<std::uint16_t>((timer.mode & mode_clock_mask) >> 8U);
}

std::uint32_t ticksUntilValue(
    std::uint16_t counter,
    std::uint16_t value) noexcept {
    if (value > counter) {
        return static_cast<std::uint32_t>(value) - counter;
    }
    return counter_range - static_cast<std::uint32_t>(counter) + value;
}

void restorePulse(RootTimerState& timer) noexcept {
    if (!timer.pulse_restore_pending) {
        return;
    }
    timer.mode |= mode_irq_request;
    timer.pulse_restore_pending = false;
}

} // namespace

RootTimers::RootTimers() noexcept {
    reset();
}

void RootTimers::reset() noexcept {
    state_ = {};
}

void RootTimers::restoreState(const RootTimersState& state) noexcept {
    state_ = state;
    for (auto& timer : state_.timers) {
        timer.divider_remainder = static_cast<std::uint8_t>(
            timer.divider_remainder & 7U);
    }
}

std::uint16_t RootTimers::readCounter(std::size_t channel) const noexcept {
    return validChannel(channel) ? state_.timers[channel].counter : 0U;
}

std::uint16_t RootTimers::readMode(std::size_t channel) noexcept {
    if (!validChannel(channel)) {
        return 0U;
    }
    auto& timer = state_.timers[channel];
    const auto value = timer.mode;
    timer.mode &= static_cast<std::uint16_t>(
        ~(mode_reached_target | mode_reached_maximum));
    return value;
}

std::uint16_t RootTimers::peekMode(std::size_t channel) const noexcept {
    return validChannel(channel) ? state_.timers[channel].mode : 0U;
}

std::uint16_t RootTimers::readTarget(std::size_t channel) const noexcept {
    return validChannel(channel) ? state_.timers[channel].target : 0U;
}

void RootTimers::writeCounter(std::size_t channel, std::uint16_t value) noexcept {
    if (validChannel(channel)) {
        state_.timers[channel].counter = value;
    }
}

void RootTimers::writeMode(std::size_t channel, std::uint16_t value) noexcept {
    if (!validChannel(channel)) {
        return;
    }

    auto& timer = state_.timers[channel];
    timer.counter = 0U;
    timer.mode = static_cast<std::uint16_t>(
        (value & mode_write_mask) | mode_irq_request);
    timer.divider_remainder = 0U;
    timer.one_shot_fired = false;
    timer.pulse_restore_pending = false;
    timer.sync_released = false;
}

void RootTimers::writeTarget(std::size_t channel, std::uint16_t value) noexcept {
    if (validChannel(channel)) {
        state_.timers[channel].target = value;
    }
}

RootTimers::IrqMask RootTimers::advanceCpuCycles(std::uint64_t cycles) noexcept {
    if (cycles == 0U) {
        return 0U;
    }

    restorePendingPulses();
    IrqMask interrupts = 0U;

    // The interpreter advances one CPU cycle per instruction. Keep this hot
    // path division-free while preserving the general path's event timing.
    if (cycles == 1U) {
        const auto timer0_clock = clockSource(state_.timers[0]);
        if ((timer0_clock == 0U || timer0_clock == 2U) && shouldCount(0U)) {
            interrupts = static_cast<IrqMask>(
                interrupts | advanceTimerOne(0U));
        }

        const auto timer1_clock = clockSource(state_.timers[1]);
        if ((timer1_clock == 0U || timer1_clock == 2U) && shouldCount(1U)) {
            interrupts = static_cast<IrqMask>(
                interrupts | advanceTimerOne(1U));
        }

        auto& timer2 = state_.timers[2];
        if (!shouldCount(2U)) {
            return interrupts;
        }
        const auto timer2_clock = clockSource(timer2);
        if (timer2_clock == 0U || timer2_clock == 1U) {
            return static_cast<IrqMask>(interrupts | advanceTimerOne(2U));
        }
        ++timer2.divider_remainder;
        if (timer2.divider_remainder == 8U) {
            timer2.divider_remainder = 0U;
            interrupts = static_cast<IrqMask>(
                interrupts | advanceTimerOne(2U));
        }
        return interrupts;
    }

    const auto timer0_clock = clockSource(state_.timers[0]);
    if ((timer0_clock == 0U || timer0_clock == 2U) && shouldCount(0U)) {
        interrupts = static_cast<IrqMask>(interrupts | advanceTimer(0U, cycles));
    }

    const auto timer1_clock = clockSource(state_.timers[1]);
    if ((timer1_clock == 0U || timer1_clock == 2U) && shouldCount(1U)) {
        interrupts = static_cast<IrqMask>(interrupts | advanceTimer(1U, cycles));
    }

    auto& timer2 = state_.timers[2];
    if (!shouldCount(2U)) {
        return interrupts;
    }

    const auto timer2_clock = clockSource(timer2);
    if (timer2_clock == 0U || timer2_clock == 1U) {
        interrupts = static_cast<IrqMask>(interrupts | advanceTimer(2U, cycles));
        return interrupts;
    }

    auto timer2_ticks = cycles / 8U;
    const auto sub_ticks = static_cast<std::uint8_t>(cycles % 8U);
    const auto remainder = static_cast<std::uint8_t>(
        timer2.divider_remainder + sub_ticks);
    timer2_ticks += remainder / 8U;
    timer2.divider_remainder = static_cast<std::uint8_t>(remainder % 8U);
    interrupts = static_cast<IrqMask>(
        interrupts | advanceTimer(2U, timer2_ticks));
    return interrupts;
}

RootTimers::IrqMask RootTimers::advanceDotClocks(std::uint64_t clocks) noexcept {
    if (clocks == 0U) {
        return 0U;
    }

    restorePendingPulses();
    const auto timer0_clock = clockSource(state_.timers[0]);
    if ((timer0_clock == 1U || timer0_clock == 3U) && shouldCount(0U)) {
        return advanceTimer(0U, clocks);
    }
    return 0U;
}

RootTimers::IrqMask RootTimers::setHBlank(bool active) noexcept {
    auto& timer0 = state_.timers[0];
    if (timer0.gate_active == active) {
        return 0U;
    }

    restorePendingPulses();
    const auto rising_edge = !timer0.gate_active && active;
    applyGate(0U, active);

    const auto timer1_clock = clockSource(state_.timers[1]);
    if (rising_edge && (timer1_clock == 1U || timer1_clock == 3U) &&
        shouldCount(1U)) {
        return advanceTimer(1U, 1U);
    }
    return 0U;
}

RootTimers::IrqMask RootTimers::setVBlank(bool active) noexcept {
    auto& timer1 = state_.timers[1];
    if (timer1.gate_active == active) {
        return 0U;
    }
    restorePendingPulses();
    applyGate(1U, active);
    return 0U;
}

bool RootTimers::shouldCount(std::size_t channel) const noexcept {
    const auto& timer = state_.timers[channel];
    if ((timer.mode & mode_sync_enable) == 0U) {
        return true;
    }

    const auto sync_mode = syncMode(timer);
    if (channel == 2U) {
        return sync_mode == 1U || sync_mode == 2U;
    }

    switch (sync_mode) {
    case 0U: return !timer.gate_active;
    case 1U: return true;
    case 2U: return timer.gate_active;
    case 3U: return timer.gate_active || timer.sync_released;
    default: return false;
    }
}

RootTimers::IrqMask RootTimers::advanceTimerOne(
    std::size_t channel) noexcept {
    auto& timer = state_.timers[channel];
    restorePulse(timer);

    const auto reaches_target =
        ticksUntilValue(timer.counter, timer.target) == 1U;
    const auto reaches_maximum =
        ticksUntilValue(timer.counter, counter_maximum) == 1U;
    if (reaches_target) {
        timer.mode |= mode_reached_target;
    }
    if (reaches_maximum) {
        timer.mode |= mode_reached_maximum;
    }

    if (reaches_target && (timer.mode & mode_reset_at_target) != 0U) {
        timer.counter = 0U;
    } else {
        timer.counter = static_cast<std::uint16_t>(timer.counter + 1U);
    }

    const auto target_irq =
        reaches_target && (timer.mode & mode_irq_at_target) != 0U;
    const auto maximum_irq =
        reaches_maximum && (timer.mode & mode_irq_at_maximum) != 0U;
    const auto interrupt_events =
        timer.target == counter_maximum && target_irq && maximum_irq
            ? 1U
            : static_cast<std::uint32_t>(target_irq) +
                  static_cast<std::uint32_t>(maximum_irq);
    return raiseInterruptEvents(channel, interrupt_events);
}

RootTimers::IrqMask RootTimers::advanceTimer(
    std::size_t channel,
    std::uint64_t ticks) noexcept {
    auto& timer = state_.timers[channel];
    if (ticks == 0U) {
        return 0U;
    }
    restorePulse(timer);

    std::uint64_t target_events{};
    std::uint64_t maximum_events{};
    const auto first_target = static_cast<std::uint64_t>(
        ticksUntilValue(timer.counter, timer.target));
    const auto first_maximum = static_cast<std::uint64_t>(
        ticksUntilValue(timer.counter, counter_maximum));

    if ((timer.mode & mode_reset_at_target) == 0U) {
        if (ticks >= first_target) {
            target_events = 1U + (ticks - first_target) / counter_range;
        }
        if (ticks >= first_maximum) {
            maximum_events = 1U + (ticks - first_maximum) / counter_range;
        }
        const auto advanced = static_cast<std::uint32_t>(timer.counter) +
            static_cast<std::uint32_t>(ticks % counter_range);
        timer.counter = static_cast<std::uint16_t>(advanced & counter_maximum);
    } else if (ticks < first_target) {
        maximum_events = ticks >= first_maximum ? 1U : 0U;
        const auto advanced = static_cast<std::uint32_t>(timer.counter) +
            static_cast<std::uint32_t>(ticks % counter_range);
        timer.counter = static_cast<std::uint16_t>(advanced & counter_maximum);
    } else {
        target_events = 1U;
        maximum_events = first_maximum <= first_target ? 1U : 0U;
        const auto remaining = ticks - first_target;
        const auto period = timer.target == 0U
            ? static_cast<std::uint64_t>(counter_range)
            : static_cast<std::uint64_t>(timer.target);
        const auto complete_periods = remaining / period;
        const auto remainder = remaining % period;
        target_events += complete_periods;
        timer.counter = static_cast<std::uint16_t>(remainder);

        if (timer.target == 0U) {
            maximum_events += complete_periods +
                (remainder >= counter_maximum ? 1U : 0U);
        } else if (timer.target == counter_maximum) {
            maximum_events = target_events;
        }
    }

    if (target_events != 0U) {
        timer.mode |= mode_reached_target;
    }
    if (maximum_events != 0U) {
        timer.mode |= mode_reached_maximum;
    }

    const auto target_irq_events = (timer.mode & mode_irq_at_target) != 0U
        ? target_events
        : 0U;
    const auto maximum_irq_events = (timer.mode & mode_irq_at_maximum) != 0U
        ? maximum_events
        : 0U;
    const auto interrupt_events = timer.target == counter_maximum &&
            target_irq_events != 0U && maximum_irq_events != 0U
        ? std::max(target_irq_events, maximum_irq_events)
        : target_irq_events + maximum_irq_events;
    return raiseInterruptEvents(channel, interrupt_events);
}

RootTimers::IrqMask RootTimers::raiseInterruptEvents(
    std::size_t channel,
    std::uint64_t event_count) noexcept {
    auto& timer = state_.timers[channel];
    if (event_count == 0U) {
        return 0U;
    }
    const auto repeats = (timer.mode & mode_irq_repeat) != 0U;
    if (!repeats && timer.one_shot_fired) {
        return 0U;
    }
    timer.one_shot_fired = true;

    if (!repeats) {
        event_count = 1U;
    }

    const auto mask = static_cast<IrqMask>(1U << channel);
    if ((timer.mode & mode_irq_toggle) != 0U) {
        const auto request_was_high = (timer.mode & mode_irq_request) != 0U;
        const auto raises_irq = request_was_high || event_count > 1U;
        if ((event_count & 1U) != 0U) {
            timer.mode ^= mode_irq_request;
        }
        timer.pulse_restore_pending = false;
        return raises_irq ? mask : 0U;
    }

    // Pulse mode drives the internal active-low request line low and back high
    // within the timer event. The interrupt controller latches that edge, while
    // a subsequent MODE read observes the inactive (high) state.
    timer.mode &= static_cast<std::uint16_t>(~mode_irq_request);
    timer.mode |= mode_irq_request;
    timer.pulse_restore_pending = false;
    return mask;
}

void RootTimers::applyGate(std::size_t channel, bool active) noexcept {
    auto& timer = state_.timers[channel];
    const auto rising_edge = !timer.gate_active && active;
    const auto falling_edge = timer.gate_active && !active;
    timer.gate_active = active;

    if ((timer.mode & mode_sync_enable) == 0U) {
        return;
    }

    switch (syncMode(timer)) {
    case 1U:
        if (falling_edge) {
            timer.counter = 0U;
        }
        break;
    case 2U:
        if (rising_edge) {
            timer.counter = 0U;
        }
        break;
    case 3U:
        if (falling_edge) {
            timer.sync_released = true;
        }
        break;
    default: break;
    }
}

void RootTimers::restorePendingPulses() noexcept {
    for (auto& timer : state_.timers) {
        restorePulse(timer);
    }
}

} // namespace sf::psx
