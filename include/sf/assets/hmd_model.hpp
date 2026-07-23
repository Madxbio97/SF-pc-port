#pragma once

#include "sf/assets/emd_scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sf::assets {

struct HmdVertex {
    std::int16_t x{};
    std::int16_t y{};
    std::int16_t z{};
};

struct HmdTransform {
    std::array<std::int16_t, 9> rotation{};
    std::array<std::int16_t, 3> translation{};
};

struct HmdBounds {
    std::array<std::int32_t, 3> minimum{};
    std::array<std::int32_t, 3> maximum{};
};

struct HmdPart {
    std::string name;
    HmdTransform local_transform;
    std::int16_t parent{-1};
    std::uint16_t hierarchy_flags{};
    std::uint16_t declared_vertex_count{};
    std::uint16_t declared_normal_count{};
    std::uint32_t first_vertex{};
    std::uint32_t vertex_count{};
    std::uint32_t padded_vertex_count{};
    std::uint32_t first_normal{};
    std::uint32_t normal_count{};
    std::uint32_t padded_normal_count{};
    std::uint32_t metadata0{};
    std::uint32_t metadata1{};
    HmdBounds bounds;
};

struct HmdTriangle {
    std::array<std::uint16_t, 3> vertex_indices{};
    std::array<EmdUv, 3> uv{};
    std::uint16_t clut{};
    std::uint16_t texture_page{};
};

class HmdModel final {
public:
    [[nodiscard]] static HmdModel parse(std::span<const std::byte> bytes);

    [[nodiscard]] std::uint32_t flags() const noexcept { return flags_; }
    [[nodiscard]] bool flatLit() const noexcept { return (flags_ & 1U) != 0; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<HmdPart>& parts() const noexcept { return parts_; }
    [[nodiscard]] const std::vector<HmdVertex>& vertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<std::uint16_t>& vertexParts() const noexcept {
        return vertex_parts_;
    }
    [[nodiscard]] const std::vector<HmdVertex>& normals() const noexcept { return normals_; }
    [[nodiscard]] const std::vector<HmdTriangle>& triangles() const noexcept {
        return triangles_;
    }
    [[nodiscard]] std::uint32_t texturePageMask() const noexcept { return texture_page_mask_; }

private:
    HmdModel(
        std::uint32_t flags,
        std::string name,
        std::vector<HmdPart> parts,
        std::vector<HmdVertex> vertices,
        std::vector<std::uint16_t> vertex_parts,
        std::vector<HmdVertex> normals,
        std::vector<HmdTriangle> triangles,
        std::uint32_t texture_page_mask);

    std::uint32_t flags_{};
    std::string name_;
    std::vector<HmdPart> parts_;
    std::vector<HmdVertex> vertices_;
    std::vector<std::uint16_t> vertex_parts_;
    std::vector<HmdVertex> normals_;
    std::vector<HmdTriangle> triangles_;
    std::uint32_t texture_page_mask_{};
};

} // namespace sf::assets
