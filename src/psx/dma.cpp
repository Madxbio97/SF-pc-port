#include "sf/psx/dma.hpp"

namespace sf::psx {
namespace {

constexpr std::uint32_t madr_mask = 0x00ffffffU;
constexpr std::uint32_t channel_chcr_mask = 0x71770703U;
constexpr std::uint32_t otc_chcr_mask = 0x51000000U;
constexpr std::uint32_t dpcr_mask = 0x0fffffffU;
constexpr std::uint32_t dicr_control_mask = 0x00ff807fU;
constexpr std::uint32_t dicr_flag_mask = 0x7f000000U;
constexpr std::uint32_t dicr_master_enable = 1U << 23U;
constexpr std::uint32_t dicr_force_irq = 1U << 15U;
constexpr std::uint32_t dicr_master_flag = 1U << 31U;
constexpr std::uint32_t chcr_busy = 1U << 24U;
constexpr std::uint32_t chcr_trigger = 1U << 28U;

constexpr std::uint32_t channelEnableBit(std::size_t channel) noexcept {
    return 1U << (static_cast<std::uint32_t>(channel) * 4U + 3U);
}

constexpr std::uint32_t channelIrqEnableBit(std::size_t channel) noexcept {
    return 1U << (static_cast<std::uint32_t>(channel) + 16U);
}

constexpr std::uint32_t channelIrqFlagBit(std::size_t channel) noexcept {
    return 1U << (static_cast<std::uint32_t>(channel) + 24U);
}

constexpr std::uint64_t decodedBlockSize(std::uint32_t value) noexcept {
    const auto block = static_cast<std::uint16_t>(value);
    return block == 0U
        ? std::uint64_t{1U} << 16U
        : static_cast<std::uint64_t>(block);
}

} // namespace

DmaController::DmaController() noexcept {
    reset();
}

void DmaController::reset() noexcept {
    state_ = {};
    state_.dpcr = dpcr_reset;
}

std::optional<std::size_t> DmaController::channelIndex(
    DmaChannel channel) noexcept {
    const auto index = static_cast<std::size_t>(channel);
    if (index >= channel_count) {
        return std::nullopt;
    }
    return index;
}

std::uint32_t DmaController::chcrWriteMask(std::size_t index) noexcept {
    return index == static_cast<std::size_t>(DmaChannel::otc)
        ? otc_chcr_mask
        : channel_chcr_mask;
}

DmaSyncMode DmaController::decodeSyncMode(std::uint32_t chcr) noexcept {
    return static_cast<DmaSyncMode>((chcr >> 9U) & 0x3U);
}

bool DmaController::readRegister(
    std::uint32_t offset,
    std::uint32_t& value) const noexcept {
    if ((offset & 0x3U) != 0U || offset >= register_span) {
        return false;
    }

    if (offset == 0x70U) {
        value = state_.dpcr;
        return true;
    }
    if (offset == 0x74U) {
        value = dicr();
        return true;
    }

    const auto index = static_cast<std::size_t>(offset / 0x10U);
    if (index >= channel_count) {
        return false;
    }
    switch (offset & 0x0fU) {
    case 0x00U:
        value = state_.channels[index].madr;
        return true;
    case 0x04U:
        value = state_.channels[index].bcr;
        return true;
    case 0x08U:
        value = state_.channels[index].chcr;
        return true;
    default:
        return false;
    }
}

bool DmaController::writeRegister(
    std::uint32_t offset,
    std::uint32_t value,
    std::uint32_t write_mask) noexcept {
    if ((offset & 0x3U) != 0U || offset >= register_span) {
        return false;
    }

    if (offset == 0x70U) {
        const auto previous = state_.dpcr;
        state_.dpcr = ((state_.dpcr & ~write_mask) |
                       (value & write_mask)) & dpcr_mask;
        for (std::size_t index = 0U; index < channel_count; ++index) {
            const auto enable = channelEnableBit(index);
            if ((previous & enable) != 0U && (state_.dpcr & enable) == 0U) {
                if (state_.channels[index].scheduled_token != 0U &&
                    decodeSyncMode(state_.channels[index].chcr) ==
                        DmaSyncMode::manual) {
                    state_.channels[index].chcr |= chcr_trigger;
                }
                state_.channels[index].scheduled_token = 0U;
            }
        }
        return true;
    }
    if (offset == 0x74U) {
        const auto control_mask = write_mask & dicr_control_mask;
        state_.dicr = (state_.dicr & ~control_mask) |
            (value & control_mask);
        state_.dicr &= ~(value & write_mask & dicr_flag_mask);
        return true;
    }

    const auto index = static_cast<std::size_t>(offset / 0x10U);
    if (index >= channel_count) {
        return false;
    }
    auto& channel = state_.channels[index];
    switch (offset & 0x0fU) {
    case 0x00U:
        channel.madr = ((channel.madr & ~write_mask) |
                        (value & write_mask)) & madr_mask;
        return true;
    case 0x04U:
        channel.bcr = (channel.bcr & ~write_mask) | (value & write_mask);
        return true;
    case 0x08U: {
        const auto merged = (channel.chcr & ~write_mask) |
            (value & write_mask);
        const auto next = merged & chcrWriteMask(index);
        if (channel.chcr != next) {
            channel.scheduled_token = 0U;
        }
        channel.chcr = next;
        return true;
    }
    default:
        return false;
    }
}

std::uint32_t DmaController::madr(DmaChannel channel) const noexcept {
    const auto index = channelIndex(channel);
    return index.has_value() ? state_.channels[*index].madr : 0U;
}

std::uint32_t DmaController::bcr(DmaChannel channel) const noexcept {
    const auto index = channelIndex(channel);
    return index.has_value() ? state_.channels[*index].bcr : 0U;
}

std::uint32_t DmaController::chcr(DmaChannel channel) const noexcept {
    const auto index = channelIndex(channel);
    return index.has_value() ? state_.channels[*index].chcr : 0U;
}

DmaSyncMode DmaController::syncMode(DmaChannel channel) const noexcept {
    return decodeSyncMode(chcr(channel));
}

bool DmaController::channelEnabled(DmaChannel channel) const noexcept {
    const auto index = channelIndex(channel);
    return index.has_value() &&
        (state_.dpcr & channelEnableBit(*index)) != 0U;
}

bool DmaController::channelStartable(DmaChannel channel) const noexcept {
    const auto index = channelIndex(channel);
    if (!index.has_value()) {
        return false;
    }
    const auto& state = state_.channels[*index];
    if (state.scheduled_token != 0U ||
        (state_.dpcr & channelEnableBit(*index)) == 0U ||
        (state.chcr & chcr_busy) == 0U) {
        return false;
    }

    switch (decodeSyncMode(state.chcr)) {
    case DmaSyncMode::manual:
        return (state.chcr & chcr_trigger) != 0U;
    case DmaSyncMode::request:
    case DmaSyncMode::linked_list:
        return true;
    case DmaSyncMode::reserved:
        return false;
    }
    return false;
}

std::optional<std::uint64_t> DmaController::estimatedWordCount(
    DmaChannel channel) const noexcept {
    const auto index = channelIndex(channel);
    if (!index.has_value()) {
        return std::nullopt;
    }
    const auto& state = state_.channels[*index];
    switch (decodeSyncMode(state.chcr)) {
    case DmaSyncMode::manual:
        return decodedBlockSize(state.bcr);
    case DmaSyncMode::request:
        return decodedBlockSize(state.bcr) * decodedBlockSize(state.bcr >> 16U);
    case DmaSyncMode::linked_list:
    case DmaSyncMode::reserved:
        return std::nullopt;
    }
    return std::nullopt;
}

bool DmaController::setMadr(
    DmaChannel channel,
    std::uint32_t value) noexcept {
    const auto index = channelIndex(channel);
    if (!index.has_value()) {
        return false;
    }
    state_.channels[*index].madr = value & madr_mask;
    return true;
}

bool DmaController::setBcr(
    DmaChannel channel,
    std::uint32_t value) noexcept {
    const auto index = channelIndex(channel);
    if (!index.has_value()) {
        return false;
    }
    state_.channels[*index].bcr = value;
    return true;
}

bool DmaController::markScheduled(
    DmaChannel channel,
    std::uint64_t token) noexcept {
    const auto index = channelIndex(channel);
    if (!index.has_value() || token == 0U || !channelStartable(channel)) {
        return false;
    }
    auto& state = state_.channels[*index];
    state.chcr &= ~chcr_trigger;
    state.scheduled_token = token;
    return true;
}

std::uint64_t DmaController::scheduledToken(DmaChannel channel) const noexcept {
    const auto index = channelIndex(channel);
    return index.has_value() ? state_.channels[*index].scheduled_token : 0U;
}

bool DmaController::cancelScheduled(
    DmaChannel channel,
    std::uint64_t token) noexcept {
    const auto index = channelIndex(channel);
    if (!index.has_value() || token == 0U ||
        state_.channels[*index].scheduled_token != token) {
        return false;
    }
    auto& state = state_.channels[*index];
    state.scheduled_token = 0U;
    if (decodeSyncMode(state.chcr) == DmaSyncMode::manual) {
        state.chcr |= chcr_trigger;
    }
    return true;
}

bool DmaController::complete(DmaChannel channel) noexcept {
    const auto index = channelIndex(channel);
    if (!index.has_value()) {
        return false;
    }
    auto& state = state_.channels[*index];
    if ((state.chcr & chcr_busy) == 0U) {
        state.scheduled_token = 0U;
        return false;
    }

    const auto was_active = interruptLine();
    state.chcr &= ~(chcr_busy | chcr_trigger);
    state.scheduled_token = 0U;

    const auto enable = channelIrqEnableBit(*index);
    if ((state_.dicr & dicr_master_enable) != 0U &&
        (state_.dicr & enable) != 0U) {
        state_.dicr |= channelIrqFlagBit(*index);
    }
    return !was_active && interruptLine();
}

bool DmaController::interruptLine() const noexcept {
    if ((state_.dicr & dicr_force_irq) != 0U) {
        return true;
    }
    if ((state_.dicr & dicr_master_enable) == 0U) {
        return false;
    }

    const auto enables = (state_.dicr >> 16U) & 0x7fU;
    const auto flags = (state_.dicr >> 24U) & 0x7fU;
    return (enables & flags) != 0U;
}

std::uint32_t DmaController::dicr() const noexcept {
    return state_.dicr | (interruptLine() ? dicr_master_flag : 0U);
}

bool DmaController::restoreState(const DmaControllerState& state) noexcept {
    if ((state.dpcr & ~dpcr_mask) != 0U ||
        (state.dicr & ~(dicr_control_mask | dicr_flag_mask)) != 0U) {
        return false;
    }

    for (std::size_t index = 0U; index < channel_count; ++index) {
        const auto& channel = state.channels[index];
        if ((channel.madr & ~madr_mask) != 0U ||
            (channel.chcr & ~chcrWriteMask(index)) != 0U) {
            return false;
        }
        if (channel.scheduled_token != 0U &&
            ((channel.chcr & chcr_busy) == 0U ||
             (channel.chcr & chcr_trigger) != 0U ||
             (state.dpcr & channelEnableBit(index)) == 0U ||
             decodeSyncMode(channel.chcr) == DmaSyncMode::reserved)) {
            return false;
        }
    }

    state_ = state;
    return true;
}

} // namespace sf::psx
