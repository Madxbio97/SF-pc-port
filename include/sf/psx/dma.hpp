#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace sf::psx {

enum class DmaChannel : std::uint8_t {
    mdec_in = 0U,
    mdec_out = 1U,
    gpu = 2U,
    cdrom = 3U,
    spu = 4U,
    pio = 5U,
    otc = 6U,
};

enum class DmaSyncMode : std::uint8_t {
    manual = 0U,
    request = 1U,
    linked_list = 2U,
    reserved = 3U,
};

struct DmaChannelState {
    std::uint32_t madr{};
    std::uint32_t bcr{};
    std::uint32_t chcr{};
    std::uint64_t scheduled_token{};

    bool operator==(const DmaChannelState&) const = default;
};

struct DmaControllerState {
    std::array<DmaChannelState, 7U> channels{};
    std::uint32_t dpcr{0x07654321U};
    std::uint32_t dicr{};

    bool operator==(const DmaControllerState&) const = default;
};

// Register-level PS1 DMA controller. Transfer endpoints and RAM movement are
// owned by the machine; this class only owns guest-visible DMA state.
class DmaController final {
public:
    static constexpr std::size_t channel_count = 7U;
    static constexpr std::uint32_t register_span = 0x78U;
    static constexpr std::uint32_t dpcr_reset = 0x07654321U;

    DmaController() noexcept;

    void reset() noexcept;

    // Offsets are relative to 0x1f801080 and must identify a complete aligned
    // register. PsxMachine applies byte/halfword lane masks before calling.
    [[nodiscard]] bool readRegister(
        std::uint32_t offset,
        std::uint32_t& value) const noexcept;
    [[nodiscard]] bool writeRegister(
        std::uint32_t offset,
        std::uint32_t value,
        std::uint32_t write_mask = 0xffffffffU) noexcept;

    [[nodiscard]] std::uint32_t madr(DmaChannel channel) const noexcept;
    [[nodiscard]] std::uint32_t bcr(DmaChannel channel) const noexcept;
    [[nodiscard]] std::uint32_t chcr(DmaChannel channel) const noexcept;
    [[nodiscard]] DmaSyncMode syncMode(DmaChannel channel) const noexcept;
    [[nodiscard]] bool channelEnabled(DmaChannel channel) const noexcept;
    [[nodiscard]] bool channelStartable(DmaChannel channel) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> estimatedWordCount(
        DmaChannel channel) const noexcept;

    [[nodiscard]] bool setMadr(
        DmaChannel channel,
        std::uint32_t value) noexcept;
    [[nodiscard]] bool setBcr(
        DmaChannel channel,
        std::uint32_t value) noexcept;

    [[nodiscard]] bool markScheduled(
        DmaChannel channel,
        std::uint64_t token) noexcept;
    [[nodiscard]] std::uint64_t scheduledToken(DmaChannel channel) const noexcept;
    [[nodiscard]] bool cancelScheduled(
        DmaChannel channel,
        std::uint64_t token) noexcept;

    // Returns true only when completion transitions the derived DMA IRQ output
    // from inactive to active.
    [[nodiscard]] bool complete(DmaChannel channel) noexcept;
    [[nodiscard]] bool interruptLine() const noexcept;

    [[nodiscard]] std::uint32_t dpcr() const noexcept { return state_.dpcr; }
    [[nodiscard]] std::uint32_t dicr() const noexcept;
    [[nodiscard]] DmaControllerState captureState() const noexcept {
        return state_;
    }
    [[nodiscard]] bool restoreState(const DmaControllerState& state) noexcept;

private:
    [[nodiscard]] static std::optional<std::size_t> channelIndex(
        DmaChannel channel) noexcept;
    [[nodiscard]] static std::uint32_t chcrWriteMask(std::size_t index) noexcept;
    [[nodiscard]] static DmaSyncMode decodeSyncMode(std::uint32_t chcr) noexcept;

    DmaControllerState state_{};
};

} // namespace sf::psx
