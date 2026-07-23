#include "sf/assets/hmd_animation.hpp"

#include "sf/core/error.hpp"

#include <limits>
#include <utility>

namespace sf::assets {
namespace {

constexpr std::uint8_t root_motion_marker = 0xeaU;
constexpr std::uint8_t end_frame = 0xfcU;

std::uint8_t readByte(std::span<const std::byte> bytes, std::size_t& cursor) {
    if (cursor >= bytes.size()) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated HMD animation"};
    }
    return std::to_integer<std::uint8_t>(bytes[cursor++]);
}

std::int16_t signExtend(std::uint32_t value, unsigned int bits) {
    const auto mask = (std::uint32_t{1} << bits) - 1U;
    const auto sign = std::uint32_t{1} << (bits - 1U);
    value &= mask;
    const auto result = (value ^ sign) - sign;
    return static_cast<std::int16_t>(result);
}

std::array<std::int16_t, 3> decodeRotation(
    std::span<const std::byte> bytes,
    std::size_t& cursor,
    bool& absolute) {
    const auto first = readByte(bytes, cursor);
    absolute = false;
    if ((first & 0x80U) == 0U) {
        const auto packed = (static_cast<std::uint32_t>(first) << 8U) |
            readByte(bytes, cursor);
        return {
            signExtend(packed >> 10U, 5U),
            signExtend(packed >> 5U, 5U),
            signExtend(packed, 5U),
        };
    }
    if ((first & 0x40U) == 0U) {
        if ((first & 0x20U) == 0U) {
            const auto packed = (static_cast<std::uint32_t>(first & 0x1fU) << 16U) |
                (static_cast<std::uint32_t>(readByte(bytes, cursor)) << 8U) |
                readByte(bytes, cursor);
            return {
                signExtend(packed >> 14U, 7U),
                signExtend(packed >> 7U, 7U),
                signExtend(packed, 7U),
            };
        }
        absolute = true;
        const auto x = (static_cast<std::uint32_t>(first & 0x1fU) << 8U) |
            readByte(bytes, cursor);
        const auto y = (static_cast<std::uint32_t>(readByte(bytes, cursor)) << 8U) |
            readByte(bytes, cursor);
        const auto z = (static_cast<std::uint32_t>(readByte(bytes, cursor)) << 8U) |
            readByte(bytes, cursor);
        return {
            signExtend(x, 13U),
            signExtend(y, 16U),
            signExtend(z, 16U),
        };
    }
    const auto packed = (static_cast<std::uint32_t>(first & 0x3fU) << 24U) |
        (static_cast<std::uint32_t>(readByte(bytes, cursor)) << 16U) |
        (static_cast<std::uint32_t>(readByte(bytes, cursor)) << 8U) |
        readByte(bytes, cursor);
    return {
        signExtend(packed >> 20U, 10U),
        signExtend(packed >> 10U, 10U),
        signExtend(packed, 10U),
    };
}

std::array<std::int16_t, 3> decodeTranslation(
    std::span<const std::byte> bytes,
    std::size_t& cursor) {
    return {
        signExtend(readByte(bytes, cursor), 8U),
        static_cast<std::int16_t>(-signExtend(readByte(bytes, cursor), 8U)),
        signExtend(readByte(bytes, cursor), 8U),
    };
}

std::int8_t signedByte(std::byte value) {
    return static_cast<std::int8_t>(signExtend(std::to_integer<std::uint8_t>(value), 8U));
}

std::size_t animationOffset(
    std::span<const std::byte> bytes,
    bool& has_root_motion,
    std::vector<HmdRootMotionFrame>& root_motion) {
    has_root_motion = !bytes.empty() &&
        std::to_integer<std::uint8_t>(bytes.front()) == root_motion_marker;
    if (!has_root_motion) {
        return 0U;
    }
    if (bytes.size() < 4U) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated HMD root-motion header"};
    }
    const auto offset = static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[1])) |
        (static_cast<std::size_t>(std::to_integer<std::uint8_t>(bytes[2])) << 8U);
    if (std::to_integer<std::uint8_t>(bytes[3]) != 0U ||
        offset < 8U || offset >= bytes.size() ||
        std::to_integer<std::uint8_t>(bytes[offset - 4U]) != 0xefU ||
        std::to_integer<std::uint8_t>(bytes[offset - 3U]) != 0xefU ||
        (offset - 8U) % 4U != 0U) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD root-motion boundary"};
    }
    root_motion.reserve((offset - 8U) / 4U);
    // Native PCHAN prefixes store one signed {x, y, z, pad} root record for
    // every decoded pose, followed by the EF EF boundary and mirrored offset.
    for (std::size_t cursor = 4U; cursor < offset - 4U; cursor += 4U) {
        root_motion.push_back(HmdRootMotionFrame{
            signedByte(bytes[cursor]),
            signedByte(bytes[cursor + 1U]),
            signedByte(bytes[cursor + 2U]),
            signedByte(bytes[cursor + 3U]),
        });
    }
    return offset;
}

} // namespace

HmdAnimationClip::HmdAnimationClip(
    std::size_t part_count,
    std::uint8_t duration,
    bool has_root_motion,
    std::vector<HmdRootMotionFrame> root_motion,
    std::uint16_t animated_parts,
    std::vector<HmdAnimationFrame> frames)
    : part_count_(part_count),
      duration_(duration),
      has_root_motion_(has_root_motion),
      root_motion_(std::move(root_motion)),
      animated_parts_(animated_parts),
      frames_(std::move(frames)) {}

HmdAnimationClip HmdAnimationClip::parse(
    std::span<const std::byte> bytes,
    std::size_t part_count) {
    if (part_count == 0U || part_count > 16U || bytes.empty()) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD animation dimensions"};
    }

    bool has_root_motion{};
    std::vector<HmdRootMotionFrame> root_motion;
    auto cursor = animationOffset(bytes, has_root_motion, root_motion);
    std::vector<HmdAnimationFrame> frames;
    std::vector<HmdAnimationTransform> state(part_count);
    std::uint16_t valid_parts{};
    std::uint16_t animated_parts{};
    std::uint8_t previous_frame{};
    auto terminated = false;
    const auto supported_mask = part_count == 16U
        ? std::numeric_limits<std::uint16_t>::max()
        : static_cast<std::uint16_t>((std::uint32_t{1} << part_count) - 1U);

    while (cursor < bytes.size()) {
        const auto marker = readByte(bytes, cursor);
        if (marker != 0xfaU) {
            throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD animation frame marker"};
        }
        const auto number = readByte(bytes, cursor);
        if (number == end_frame) {
            terminated = true;
            break;
        }
        if (number == 0U || number != static_cast<unsigned int>(previous_frame) + 1U) {
            throw core::Error{core::ErrorCode::invalid_format, "HMD animation frames are not contiguous"};
        }
        const auto mask = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(readByte(bytes, cursor)) << 8U) |
            readByte(bytes, cursor));
        if ((mask & static_cast<std::uint16_t>(~supported_mask)) != 0U) {
            throw core::Error{core::ErrorCode::invalid_format, "HMD animation references an invalid part"};
        }

        for (std::size_t part = 0; part < part_count; ++part) {
            const auto bit = static_cast<std::uint16_t>(std::uint16_t{1} << part);
            if ((mask & bit) == 0U) {
                continue;
            }
            bool absolute{};
            const auto rotation = decodeRotation(bytes, cursor, absolute);
            if (number == 1U && !absolute) {
                throw core::Error{core::ErrorCode::invalid_format, "HMD animation has a relative first key"};
            }
            auto& transform = state[part];
            if (absolute) {
                transform.rotation = rotation;
            } else {
                for (std::size_t component = 0; component < rotation.size(); ++component) {
                    transform.rotation[component] = static_cast<std::int16_t>(
                        static_cast<std::int32_t>(transform.rotation[component]) +
                        rotation[component]);
                }
            }
            if (number == 1U) {
                transform.translation = decodeTranslation(bytes, cursor);
            } else if (part == 0U) {
                // Later part-zero bytes are root-motion metadata. The original
                // decoder advances past them without replacing matrix.t.
                static_cast<void>(decodeTranslation(bytes, cursor));
            }
            valid_parts = static_cast<std::uint16_t>(valid_parts | bit);
        }
        animated_parts = static_cast<std::uint16_t>(animated_parts | mask);
        frames.push_back(HmdAnimationFrame{
            number,
            mask,
            valid_parts,
            state,
        });
        previous_frame = number;
    }
    if (!terminated || frames.empty()) {
        throw core::Error{core::ErrorCode::invalid_format, "Unterminated HMD animation"};
    }
    if (has_root_motion && root_motion.size() != frames.size()) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "HMD root-motion track does not match its animation",
        };
    }
    return HmdAnimationClip{
        part_count,
        previous_frame,
        has_root_motion,
        std::move(root_motion),
        animated_parts,
        std::move(frames),
    };
}

const HmdAnimationFrame& HmdAnimationClip::poseAtTick(std::uint64_t tick) const noexcept {
    return frames_[tick % frames_.size()];
}

} // namespace sf::assets
