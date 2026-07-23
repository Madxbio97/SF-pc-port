#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::assets {

struct HogEntry {
    std::string name;
    std::size_t offset{};
    std::size_t size{};
};

class HogArchive final {
public:
    [[nodiscard]] static HogArchive parse(std::vector<std::byte> bytes);

    [[nodiscard]] std::uint32_t identifier() const noexcept { return identifier_; }
    [[nodiscard]] const std::vector<HogEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::span<const std::byte> file(std::string_view name) const;

private:
    HogArchive(
        std::vector<std::byte> bytes,
        std::uint32_t identifier,
        std::vector<HogEntry> entries);

    std::vector<std::byte> bytes_;
    std::uint32_t identifier_{};
    std::vector<HogEntry> entries_;
};

} // namespace sf::assets
