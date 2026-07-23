#include "sf/assets/hmd_model.hpp"

#include "sf/core/error.hpp"

#include <limits>
#include <utility>

namespace sf::assets {
namespace {

constexpr std::size_t header_size = 0x24;
constexpr std::size_t triangle_size = 0x10;
constexpr std::size_t part_header_size = 0x44;
constexpr std::size_t vertex_size = 8;
constexpr std::size_t bounds_size = 0x20;
constexpr std::uint32_t identifier = 0x48000000;
constexpr std::uint32_t supported_flags = 1;

std::uint16_t readLe16(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2U) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated HMD 16-bit value"};
    }
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated HMD 32-bit value"};
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::int16_t readSignedLe16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::int16_t>(readLe16(bytes, offset));
}

std::int32_t readSignedLe32(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::int32_t>(readLe32(bytes, offset));
}

std::string readFixedString(
    std::span<const std::byte> bytes,
    std::size_t offset,
    std::size_t length) {
    if (offset > bytes.size() || bytes.size() - offset < length) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated HMD name"};
    }
    std::string result;
    result.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        const auto character = std::to_integer<unsigned char>(bytes[offset + index]);
        if (character == 0U) {
            break;
        }
        if (character < 0x20U || character > 0x7eU) {
            throw core::Error{core::ErrorCode::invalid_format, "HMD name is not ASCII"};
        }
        result.push_back(static_cast<char>(character));
    }
    if (result.empty()) {
        throw core::Error{core::ErrorCode::invalid_format, "HMD name is empty"};
    }
    return result;
}

HmdVertex readVertex(std::span<const std::byte> bytes, std::size_t offset) {
    return HmdVertex{
        readSignedLe16(bytes, offset),
        readSignedLe16(bytes, offset + 2U),
        readSignedLe16(bytes, offset + 4U),
    };
}

EmdUv decodeUv(std::uint16_t packed) {
    return EmdUv{
        static_cast<std::uint8_t>(packed),
        static_cast<std::uint8_t>(packed >> 8U),
    };
}

void validateHierarchy(std::span<const HmdPart> parts) {
    std::size_t root_count{};
    for (const auto& part : parts) {
        if (part.parent == -1) {
            ++root_count;
        } else if (part.parent < 0 || static_cast<std::size_t>(part.parent) >= parts.size()) {
            throw core::Error{core::ErrorCode::invalid_format, "HMD parent index is invalid"};
        }
    }
    if (root_count != 1U) {
        throw core::Error{core::ErrorCode::invalid_format, "HMD must have one root part"};
    }

    std::vector<std::uint8_t> states(parts.size());
    for (std::size_t start = 0; start < parts.size(); ++start) {
        auto current = start;
        while (states[current] == 0U) {
            states[current] = 1U;
            const auto parent = parts[current].parent;
            if (parent < 0) {
                break;
            }
            current = static_cast<std::size_t>(parent);
        }
        if (states[current] == 1U && parts[current].parent >= 0) {
            throw core::Error{core::ErrorCode::invalid_format, "HMD hierarchy contains a cycle"};
        }
        current = start;
        while (states[current] == 1U) {
            states[current] = 2U;
            const auto parent = parts[current].parent;
            if (parent < 0) {
                break;
            }
            current = static_cast<std::size_t>(parent);
        }
    }
}

} // namespace

HmdModel::HmdModel(
    std::uint32_t flags,
    std::string name,
    std::vector<HmdPart> parts,
    std::vector<HmdVertex> vertices,
    std::vector<std::uint16_t> vertex_parts,
    std::vector<HmdVertex> normals,
    std::vector<HmdTriangle> triangles,
    std::uint32_t texture_page_mask)
    : flags_(flags),
      name_(std::move(name)),
      parts_(std::move(parts)),
      vertices_(std::move(vertices)),
      vertex_parts_(std::move(vertex_parts)),
      normals_(std::move(normals)),
      triangles_(std::move(triangles)),
      texture_page_mask_(texture_page_mask) {}

HmdModel HmdModel::parse(std::span<const std::byte> bytes) {
    if (bytes.size() < header_size) {
        throw core::Error{core::ErrorCode::invalid_format, "HMD header is truncated"};
    }
    const auto flags = readLe32(bytes, 0);
    if ((flags & ~supported_flags) != identifier) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD identifier or flags"};
    }
    const auto part_count = static_cast<std::size_t>(readLe32(bytes, 4));
    const auto triangle_words = static_cast<std::size_t>(readLe32(bytes, 8));
    const auto geometry_end = static_cast<std::size_t>(readLe32(bytes, 0x14));
    if (part_count == 0U || part_count > static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max()) ||
        triangle_words == 0U || triangle_words % 4U != 0U ||
        readLe32(bytes, 0x0c) != 0U || readLe32(bytes, 0x10) != 0U ||
        readLe32(bytes, 0x18) != 0U ||
        triangle_words > (std::numeric_limits<std::size_t>::max() - header_size) / 4U) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD header fields"};
    }
    const auto triangle_count = triangle_words / 4U;
    const auto geometry_offset = header_size + triangle_words * 4U;
    if (geometry_offset > geometry_end || geometry_end > bytes.size() ||
        part_count > (bytes.size() - geometry_end) / bounds_size ||
        geometry_end + part_count * bounds_size != bytes.size()) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD table boundaries"};
    }

    auto cursor = geometry_offset;
    std::vector<HmdPart> parts;
    std::vector<HmdVertex> vertices;
    std::vector<std::uint16_t> vertex_parts;
    std::vector<HmdVertex> normals;
    parts.reserve(part_count);
    for (std::size_t part_index = 0; part_index < part_count; ++part_index) {
        if (cursor > geometry_end || geometry_end - cursor < part_header_size) {
            throw core::Error{core::ErrorCode::invalid_format, "HMD part header is truncated"};
        }
        const auto part_size = static_cast<std::size_t>(readLe32(bytes, cursor));
        const auto part_triangle_count = static_cast<std::size_t>(readLe32(bytes, cursor + 4U));
        const auto vertex_triplets = static_cast<std::size_t>(readLe32(bytes, cursor + 8U));
        const auto normal_triplets = static_cast<std::size_t>(readLe32(bytes, cursor + 0x0cU));
        if (part_triangle_count != triangle_count ||
            vertex_triplets > std::numeric_limits<std::size_t>::max() / 3U ||
            normal_triplets > std::numeric_limits<std::size_t>::max() / 3U) {
            throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD part counts"};
        }
        const auto vertex_count = vertex_triplets * 3U;
        const auto normal_count = normal_triplets * 3U;
        if (vertex_count > (std::numeric_limits<std::size_t>::max() - part_header_size) / vertex_size ||
            normal_count > (std::numeric_limits<std::size_t>::max() -
                part_header_size - vertex_count * vertex_size) / vertex_size) {
            throw core::Error{core::ErrorCode::invalid_format, "HMD part size overflows"};
        }
        const auto expected_part_size =
            part_header_size + (vertex_count + normal_count) * vertex_size;
        if (part_size != expected_part_size || part_size > geometry_end - cursor) {
            throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD part size"};
        }
        if (vertex_count > std::numeric_limits<std::uint32_t>::max() ||
            normal_count > std::numeric_limits<std::uint32_t>::max()) {
            throw core::Error{core::ErrorCode::invalid_format, "HMD padded table is too large"};
        }
        HmdPart part;
        part.name = readFixedString(bytes, cursor + 0x28U, 8U);
        for (std::size_t component = 0; component < part.local_transform.rotation.size(); ++component) {
            part.local_transform.rotation[component] =
                readSignedLe16(bytes, cursor + 0x10U + component * 2U);
        }
        for (std::size_t component = 0; component < part.local_transform.translation.size(); ++component) {
            part.local_transform.translation[component] =
                readSignedLe16(bytes, cursor + 0x22U + component * 2U);
        }
        part.declared_vertex_count = readLe16(bytes, cursor + 0x34U);
        part.declared_normal_count = readLe16(bytes, cursor + 0x36U);
        if (part.declared_vertex_count > vertex_count || part.declared_normal_count > normal_count) {
            throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD declared vertex counts"};
        }
        if (vertices.size() > std::numeric_limits<std::uint16_t>::max() -
                part.declared_vertex_count ||
            normals.size() > std::numeric_limits<std::uint32_t>::max() -
                part.declared_normal_count ||
            vertices.size() > std::numeric_limits<std::uint32_t>::max() -
                part.declared_vertex_count) {
            throw core::Error{core::ErrorCode::invalid_format, "HMD vertex table is too large"};
        }
        part.parent = readSignedLe16(bytes, cursor + 0x38U);
        part.hierarchy_flags = readLe16(bytes, cursor + 0x3aU);
        part.metadata0 = readLe32(bytes, cursor + 0x30U);
        part.metadata1 = readLe32(bytes, cursor + 0x3cU);
        part.first_vertex = static_cast<std::uint32_t>(vertices.size());
        part.vertex_count = part.declared_vertex_count;
        part.padded_vertex_count = static_cast<std::uint32_t>(vertex_count);
        part.first_normal = static_cast<std::uint32_t>(normals.size());
        part.normal_count = part.declared_normal_count;
        part.padded_normal_count = static_cast<std::uint32_t>(normal_count);

        const auto vertices_offset = cursor + part_header_size;
        const auto normals_offset = vertices_offset + vertex_count * vertex_size;
        if (readLe32(bytes, cursor + 0x40U) != normals_offset) {
            throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD normal-table offset"};
        }
        vertices.reserve(vertices.size() + part.declared_vertex_count);
        vertex_parts.reserve(vertex_parts.size() + part.declared_vertex_count);
        for (std::size_t index = 0; index < part.declared_vertex_count; ++index) {
            vertices.push_back(readVertex(bytes, vertices_offset + index * vertex_size));
            vertex_parts.push_back(static_cast<std::uint16_t>(part_index));
        }
        normals.reserve(normals.size() + part.declared_normal_count);
        for (std::size_t index = 0; index < part.declared_normal_count; ++index) {
            normals.push_back(readVertex(bytes, normals_offset + index * vertex_size));
        }
        parts.push_back(std::move(part));
        cursor += part_size;
    }
    if (cursor != geometry_end) {
        throw core::Error{core::ErrorCode::invalid_format, "HMD part table does not reach its boundary"};
    }

    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto offset = geometry_end + index * bounds_size;
        auto& bounds = parts[index].bounds;
        for (std::size_t component = 0; component < 3U; ++component) {
            bounds.minimum[component] = readSignedLe32(bytes, offset + component * 4U);
            bounds.maximum[component] = readSignedLe32(bytes, offset + 0x10U + component * 4U);
            if (bounds.minimum[component] > bounds.maximum[component]) {
                throw core::Error{core::ErrorCode::invalid_format, "Invalid HMD part bounds"};
            }
        }
    }
    validateHierarchy(parts);

    const auto vertex_stride = (flags & 1U) != 0U ? 8U : 12U;
    std::vector<HmdTriangle> triangles;
    triangles.reserve(triangle_count);
    std::uint32_t texture_page_mask{};
    for (std::size_t index = 0; index < triangle_count; ++index) {
        const auto offset = header_size + index * triangle_size;
        const auto word0 = readLe32(bytes, offset);
        const auto word1 = readLe32(bytes, offset + 4U);
        const auto word2 = readLe32(bytes, offset + 8U);
        const auto word3 = readLe32(bytes, offset + 0x0cU);
        const std::array encoded_indices{
            static_cast<std::uint16_t>(word2 >> 16U),
            static_cast<std::uint16_t>(word3),
            static_cast<std::uint16_t>(word3 >> 16U),
        };
        HmdTriangle triangle;
        for (std::size_t corner = 0; corner < triangle.vertex_indices.size(); ++corner) {
            if (encoded_indices[corner] % vertex_stride != 0U) {
                throw core::Error{core::ErrorCode::invalid_format, "Misaligned HMD vertex index"};
            }
            const auto vertex_index = encoded_indices[corner] / vertex_stride;
            if (vertex_index >= vertices.size()) {
                throw core::Error{core::ErrorCode::invalid_format, "HMD vertex index is out of range"};
            }
            triangle.vertex_indices[corner] = static_cast<std::uint16_t>(vertex_index);
        }
        triangle.uv = {
            decodeUv(static_cast<std::uint16_t>(word0)),
            decodeUv(static_cast<std::uint16_t>(word1)),
            decodeUv(static_cast<std::uint16_t>(word2)),
        };
        triangle.clut = static_cast<std::uint16_t>(word0 >> 16U);
        triangle.texture_page = static_cast<std::uint16_t>(word1 >> 16U);
        texture_page_mask |= 1U << (triangle.texture_page & 0x1fU);
        triangles.push_back(triangle);
    }

    return HmdModel{
        flags,
        readFixedString(bytes, 0x1cU, 8U),
        std::move(parts),
        std::move(vertices),
        std::move(vertex_parts),
        std::move(normals),
        std::move(triangles),
        texture_page_mask,
    };
}

} // namespace sf::assets
