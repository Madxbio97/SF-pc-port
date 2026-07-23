#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sf::assets {

struct HmdAnimationTransform {
    std::array<std::int16_t, 3> rotation{};
    std::array<std::int16_t, 3> translation{};
};

struct HmdRootMotionFrame {
    std::int8_t x{};
    std::int8_t y{};
    std::int8_t z{};
    std::int8_t reserved{};
};

struct HmdAnimationFrame {
    std::uint8_t number{};
    std::uint16_t updated_parts{};
    std::uint16_t valid_parts{};
    std::vector<HmdAnimationTransform> transforms;
};

class HmdAnimationClip final {
public:
    [[nodiscard]] static HmdAnimationClip parse(
        std::span<const std::byte> bytes,
        std::size_t part_count);

    [[nodiscard]] std::size_t partCount() const noexcept { return part_count_; }
    [[nodiscard]] std::uint8_t duration() const noexcept { return duration_; }
    [[nodiscard]] bool hasRootMotion() const noexcept { return has_root_motion_; }
    [[nodiscard]] std::span<const HmdRootMotionFrame> rootMotion() const noexcept {
        return root_motion_;
    }
    [[nodiscard]] std::uint16_t animatedParts() const noexcept { return animated_parts_; }
    [[nodiscard]] std::span<const HmdAnimationFrame> frames() const noexcept { return frames_; }
    [[nodiscard]] const HmdAnimationFrame& poseAtTick(std::uint64_t tick) const noexcept;

private:
    HmdAnimationClip(
        std::size_t part_count,
        std::uint8_t duration,
        bool has_root_motion,
        std::vector<HmdRootMotionFrame> root_motion,
        std::uint16_t animated_parts,
        std::vector<HmdAnimationFrame> frames);

    std::size_t part_count_{};
    std::uint8_t duration_{};
    bool has_root_motion_{};
    std::vector<HmdRootMotionFrame> root_motion_;
    std::uint16_t animated_parts_{};
    std::vector<HmdAnimationFrame> frames_;
};

} // namespace sf::assets
