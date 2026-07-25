#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::assets {

struct WeaponDescription {
    std::string name;
    std::string description;
    std::string clip_size;
    std::string maximum_rounds;
    std::uint8_t fire_rate{};
    std::uint8_t damage{};
};

class WeaponDescriptionTable final {
public:
    [[nodiscard]] static WeaponDescriptionTable parse(std::span<const std::byte> bytes);
    [[nodiscard]] static WeaponDescriptionTable parseRussianVit(
        std::span<const std::byte> bytes);

    [[nodiscard]] const std::vector<WeaponDescription>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] const WeaponDescription* find(std::string_view name) const noexcept;

private:
    explicit WeaponDescriptionTable(std::vector<WeaponDescription> entries);

    std::vector<WeaponDescription> entries_;
};

} // namespace sf::assets
