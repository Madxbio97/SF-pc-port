#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace sf::assets {

struct EmdVertex {
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
    std::uint16_t color{};
};

struct EmdUv {
    std::uint8_t u{};
    std::uint8_t v{};
};

struct EmdPolygon {
    bool quad{};
    bool renderable{};
    // Retail EMDs retain zero-area seams. The PS1 NCLIP path rejects them;
    // PGXP must not reconstruct those records into visible stretched faces.
    bool degenerate{};
    std::array<std::uint16_t, 4> vertex_indices{};
    std::array<EmdUv, 4> uv{};
    std::uint16_t clut{};
    std::uint16_t texture_page{};
};

struct EmdBounds {
    std::int16_t minimum_x{};
    std::int16_t minimum_y{};
    std::int16_t minimum_z{};
    std::int16_t maximum_x{};
    std::int16_t maximum_y{};
    std::int16_t maximum_z{};
};

struct EmdSection {
    EmdBounds bounds;
    std::uint32_t quad_count{};
    std::vector<EmdVertex> vertices;
    std::vector<EmdPolygon> polygons;
};

[[nodiscard]] std::optional<std::uint8_t> resolveEmdTexturePageSource(
    std::uint16_t raw_texture_page,
    std::uint32_t authored_page_mask,
    std::uint32_t vlf_page_mask) noexcept;

class EmdScene final {
public:
    [[nodiscard]] static EmdScene parse(std::span<const std::byte> bytes);

    [[nodiscard]] std::uint32_t flags() const noexcept { return flags_; }
    [[nodiscard]] std::uint8_t textureBank() const noexcept {
        return static_cast<std::uint8_t>((flags_ & 0xffU) >> 4U);
    }
    [[nodiscard]] std::uint32_t texturePageMask() const noexcept { return texture_page_mask_; }
    [[nodiscard]] std::optional<std::uint32_t> resolvedTexturePageMask(
        std::uint32_t vlf_page_mask) const noexcept;
    [[nodiscard]] const std::vector<EmdSection>& sections() const noexcept { return sections_; }
    [[nodiscard]] std::size_t vertexCount() const noexcept;
    [[nodiscard]] std::size_t polygonCount() const noexcept;

private:
    EmdScene(
        std::uint32_t flags,
        std::uint32_t texture_page_mask,
        std::vector<EmdSection> sections);

    std::uint32_t flags_{};
    std::uint32_t texture_page_mask_{};
    std::vector<EmdSection> sections_;
};

} // namespace sf::assets
