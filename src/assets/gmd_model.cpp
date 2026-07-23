#include "sf/assets/gmd_model.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace sf::assets {
namespace {

constexpr std::size_t header_size = 0x18;
constexpr std::size_t triangle_size = 0x10;
constexpr std::uint32_t identifier = 0x7b;

std::uint16_t readLe16(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated GMD 16-bit value"};
    }
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated GMD 32-bit value"};
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::int16_t readSignedLe16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::int16_t>(readLe16(bytes, offset));
}

std::int16_t unpackSigned(std::uint32_t value, unsigned int shift, unsigned int bits) {
    const auto mask = (1U << bits) - 1U;
    const auto sign = 1U << (bits - 1U);
    auto component = static_cast<std::int32_t>((value >> shift) & mask);
    if ((static_cast<std::uint32_t>(component) & sign) != 0) {
        component -= static_cast<std::int32_t>(1U << bits);
    }
    return static_cast<std::int16_t>(component);
}

EmdUv decodeUv(std::uint16_t packed) {
    return EmdUv{
        static_cast<std::uint8_t>(packed),
        static_cast<std::uint8_t>(packed >> 8U),
    };
}

} // namespace

GmdModel::GmdModel(
    std::vector<GmdVertex> vertices,
    std::vector<GmdTriangle> triangles,
    EmdBounds bounds,
    std::uint32_t texture_page_mask,
    std::uint32_t renderable_texture_page_mask)
    : vertices_(std::move(vertices)),
      triangles_(std::move(triangles)),
      bounds_(bounds),
      texture_page_mask_(texture_page_mask),
      renderable_texture_page_mask_(renderable_texture_page_mask) {}

GmdModel GmdModel::parse(std::span<const std::byte> bytes) {
    if (bytes.size() < header_size || readLe32(bytes, 0) != identifier) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid GMD header"};
    }

    const auto triangle_count = static_cast<std::size_t>(readLe16(bytes, 4));
    const auto vertex_offset = static_cast<std::size_t>(readLe16(bytes, 6));
    const auto normal_offset = static_cast<std::size_t>(readLe16(bytes, 8));
    if (triangle_count == 0 ||
        triangle_count > (std::numeric_limits<std::size_t>::max() - header_size) / triangle_size ||
        vertex_offset != header_size + triangle_count * triangle_size ||
        normal_offset < vertex_offset || normal_offset > bytes.size() ||
        (normal_offset - vertex_offset) % sizeof(std::uint32_t) != 0) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid GMD table offsets"};
    }
    const auto vertex_count = (normal_offset - vertex_offset) / sizeof(std::uint32_t);
    if (vertex_count == 0 || vertex_count > 256U) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid GMD vertex count"};
    }

    const EmdBounds bounds{
        readSignedLe16(bytes, 0x0a),
        readSignedLe16(bytes, 0x0c),
        readSignedLe16(bytes, 0x0e),
        readSignedLe16(bytes, 0x10),
        readSignedLe16(bytes, 0x12),
        readSignedLe16(bytes, 0x14),
    };
    if (bounds.minimum_x > bounds.maximum_x ||
        bounds.minimum_y > bounds.maximum_y ||
        bounds.minimum_z > bounds.maximum_z) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid GMD bounds"};
    }

    std::vector<GmdVertex> vertices;
    vertices.reserve(vertex_count);
    for (std::size_t index = 0; index < vertex_count; ++index) {
        const auto packed = readLe32(bytes, vertex_offset + index * sizeof(std::uint32_t));
        vertices.push_back(GmdVertex{
            unpackSigned(packed, 0, 10),
            unpackSigned(packed, 10, 10),
            unpackSigned(packed, 20, 12),
        });
    }

    std::vector<GmdTriangle> triangles;
    triangles.reserve(triangle_count);
    std::uint32_t texture_page_mask{};
    std::uint32_t renderable_texture_page_mask{};
    for (std::size_t index = 0; index < triangle_count; ++index) {
        const auto offset = header_size + index * triangle_size;
        const auto word0 = readLe32(bytes, offset);
        const auto word1 = readLe32(bytes, offset + 4U);
        const auto word2 = readLe32(bytes, offset + 8U);
        GmdTriangle triangle;
        triangle.vertex_indices = {
            static_cast<std::uint8_t>(word2),
            static_cast<std::uint8_t>(word2 >> 8U),
            static_cast<std::uint8_t>(word2 >> 16U),
        };
        if (std::ranges::any_of(triangle.vertex_indices, [vertex_count](std::uint8_t value) {
                return value >= vertex_count;
            })) {
            throw core::Error{core::ErrorCode::invalid_format, "GMD vertex index is out of range"};
        }
        triangle.uv = {
            decodeUv(static_cast<std::uint16_t>(word0)),
            decodeUv(static_cast<std::uint16_t>(word1)),
            decodeUv(static_cast<std::uint16_t>(word1 >> 16U)),
        };
        triangle.clut = static_cast<std::uint16_t>(
            0x7830U | (((word0 >> 24U) & 0x7fU) << 6U));
        triangle.texture_page = static_cast<std::uint16_t>((word0 >> 16U) & 0xffU);
        triangle.flags = static_cast<std::uint8_t>((word2 >> 24U) & 0x7fU);
        triangle.semi_transparent = static_cast<std::int32_t>(word0) < 0;
        const auto texture_page_bit =
            1U << (triangle.texture_page & 0x1fU);
        texture_page_mask |= texture_page_bit;
        if (triangle.flags != 0U) {
            renderable_texture_page_mask |= texture_page_bit;
        }
        triangles.push_back(triangle);
    }
    return GmdModel{std::move(vertices), std::move(triangles), bounds,
                    texture_page_mask, renderable_texture_page_mask};
}

} // namespace sf::assets
