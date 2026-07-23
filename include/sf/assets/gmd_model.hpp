#pragma once

#include "sf/assets/emd_scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sf::assets {

struct GmdVertex {
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
};

struct GmdTriangle {
    std::array<std::uint8_t, 3> vertex_indices{};
    std::array<EmdUv, 3> uv{};
    std::uint16_t clut{};
    std::uint16_t texture_page{};
    std::uint8_t flags{};
    bool semi_transparent{};
};

class GmdModel final {
public:
    [[nodiscard]] static GmdModel parse(std::span<const std::byte> bytes);

    [[nodiscard]] const std::vector<GmdVertex>& vertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<GmdTriangle>& triangles() const noexcept { return triangles_; }
    [[nodiscard]] const EmdBounds& bounds() const noexcept { return bounds_; }
    [[nodiscard]] std::uint32_t texturePageMask() const noexcept { return texture_page_mask_; }
    // A zero compact material byte is collision-only retail geometry. Keep
    // its selector available to diagnostics, but never make presentation
    // reserve/upload a texture page which no submitted primitive can sample.
    [[nodiscard]] std::uint32_t renderableTexturePageMask() const noexcept {
        return renderable_texture_page_mask_;
    }
    [[nodiscard]] bool planar() const noexcept {
        return bounds_.minimum_x == bounds_.maximum_x ||
               bounds_.minimum_y == bounds_.maximum_y ||
               bounds_.minimum_z == bounds_.maximum_z;
    }

private:
    GmdModel(
        std::vector<GmdVertex> vertices,
        std::vector<GmdTriangle> triangles,
        EmdBounds bounds,
        std::uint32_t texture_page_mask,
        std::uint32_t renderable_texture_page_mask);

    std::vector<GmdVertex> vertices_;
    std::vector<GmdTriangle> triangles_;
    EmdBounds bounds_;
    std::uint32_t texture_page_mask_{};
    std::uint32_t renderable_texture_page_mask_{};
};

} // namespace sf::assets
