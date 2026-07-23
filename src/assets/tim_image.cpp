#include "sf/assets/tim_image.hpp"

#include "sf/core/error.hpp"

#include <limits>
#include <string>
#include <utility>

namespace sf::assets {
namespace {

std::uint16_t readLe16(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint16_t)) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated TIM integer"};
    }
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated TIM integer"};
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::pair<TimBlock, std::size_t> parseBlock(
    std::span<const std::byte> bytes,
    std::size_t offset,
    const char* label) {
    constexpr std::size_t block_header_size = 12;
    const auto block_size = static_cast<std::size_t>(readLe32(bytes, offset));
    if (block_size < block_header_size || block_size > bytes.size() - offset) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            std::string{"Invalid TIM "} + label + " block size"};
    }

    TimBlock block{
        readLe16(bytes, offset + 4),
        readLe16(bytes, offset + 6),
        readLe16(bytes, offset + 8),
        readLe16(bytes, offset + 10),
        {},
    };
    const auto word_count = static_cast<std::size_t>(block.width_words) * block.height;
    if (block.width_words == 0 || block.height == 0 ||
        word_count > (block_size - block_header_size) / 2U) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            std::string{"Invalid TIM "} + label + " dimensions"};
    }

    block.words.reserve(word_count);
    for (std::size_t index = 0; index < word_count; ++index) {
        block.words.push_back(readLe16(bytes, offset + block_header_size + index * 2U));
    }
    return {std::move(block), offset + block_size};
}

} // namespace

TimImage::TimImage(TimPixelMode mode, std::optional<TimBlock> clut, TimBlock pixels)
    : mode_(mode), clut_(std::move(clut)), pixels_(std::move(pixels)) {}

TimImage TimImage::parse(std::span<const std::byte> bytes) {
    constexpr std::uint32_t signature = 0x10U;
    constexpr std::uint32_t has_clut_flag = 0x08U;
    if (bytes.size() < 20 || readLe32(bytes, 0) != signature) {
        throw core::Error{core::ErrorCode::invalid_format, "TIM signature was not found"};
    }

    const auto flags = readLe32(bytes, 4);
    const auto raw_mode = flags & 0x07U;
    if (raw_mode > static_cast<std::uint32_t>(TimPixelMode::direct24) || (flags & ~0x0fU) != 0) {
        throw core::Error{core::ErrorCode::unsupported, "Unsupported TIM pixel flags"};
    }
    const auto mode = static_cast<TimPixelMode>(raw_mode);
    const bool has_clut = (flags & has_clut_flag) != 0;
    if ((mode == TimPixelMode::indexed4 || mode == TimPixelMode::indexed8) != has_clut) {
        throw core::Error{core::ErrorCode::invalid_format, "TIM palette flag does not match pixel mode"};
    }

    std::size_t offset = 8;
    std::optional<TimBlock> clut;
    if (has_clut) {
        auto [block, next] = parseBlock(bytes, offset, "CLUT");
        clut = std::move(block);
        offset = next;
    }
    auto [pixels, next] = parseBlock(bytes, offset, "pixel");
    static_cast<void>(next);
    return TimImage{mode, std::move(clut), std::move(pixels)};
}

std::uint16_t TimImage::displayWidth() const noexcept {
    const auto words = static_cast<std::uint32_t>(pixels_.width_words);
    std::uint32_t width = words;
    switch (mode_) {
    case TimPixelMode::indexed4:
        width *= 4U;
        break;
    case TimPixelMode::indexed8:
        width *= 2U;
        break;
    case TimPixelMode::direct16:
        break;
    case TimPixelMode::direct24:
        width = width * 2U / 3U;
        break;
    }
    return static_cast<std::uint16_t>(
        width > std::numeric_limits<std::uint16_t>::max()
            ? std::numeric_limits<std::uint16_t>::max()
            : width);
}

} // namespace sf::assets
