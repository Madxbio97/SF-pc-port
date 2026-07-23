#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sf::assets {

struct LevelVisibility {
    std::vector<std::uint16_t> active_models;
    std::vector<std::uint16_t> prefetched_models;
};

class LevelLayout final {
public:
    [[nodiscard]] static LevelLayout parse(
        std::span<const std::byte> bytes,
        std::size_t expected_model_count);

    [[nodiscard]] std::size_t modelCount() const noexcept { return rooms_.size(); }
    [[nodiscard]] std::uint16_t initialRoom() const noexcept { return initial_room_; }
    [[nodiscard]] std::span<const std::uint16_t> residentModels() const noexcept {
        return resident_models_;
    }
    [[nodiscard]] const LevelVisibility& visibility(std::size_t room) const;

private:
    LevelLayout(
        std::uint16_t initial_room,
        std::vector<std::uint16_t> resident_models,
        std::vector<LevelVisibility> rooms);

    std::uint16_t initial_room_{};
    std::vector<std::uint16_t> resident_models_;
    std::vector<LevelVisibility> rooms_;
};

} // namespace sf::assets
