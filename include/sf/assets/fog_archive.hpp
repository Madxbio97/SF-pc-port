#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::assets {

struct FogEntry {
    std::string name;
    std::uint32_t start_sector{};
    std::uint32_t sector_count{};
    std::size_t offset{};
    std::size_t size{};
};

class FogArchive final {
public:
    static constexpr std::size_t sector_size = 2048;

    [[nodiscard]] static FogArchive parse(std::vector<std::byte> bytes);

    [[nodiscard]] std::uint32_t flags() const noexcept { return flags_; }
    [[nodiscard]] std::uint32_t declaredSectorCount() const noexcept {
        return declared_sector_count_;
    }
    [[nodiscard]] const std::vector<FogEntry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::span<const std::byte> file(std::string_view name) const;

private:
    FogArchive(
        std::vector<std::byte> bytes,
        std::uint32_t flags,
        std::uint32_t declared_sector_count,
        std::vector<FogEntry> entries);

    std::vector<std::byte> bytes_;
    std::uint32_t flags_{};
    std::uint32_t declared_sector_count_{};
    std::vector<FogEntry> entries_;
};

} // namespace sf::assets
