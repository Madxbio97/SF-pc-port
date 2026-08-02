#include "sf/assets/emd_scene.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <utility>

namespace sf::assets {
namespace {

constexpr std::size_t header_size = 0xa0;
constexpr std::size_t section_header_size = 0x2c;
constexpr std::size_t polygon_size = 16;
constexpr std::size_t vertex_size = 8;
// Retail only scans the 31 section-offset words at 0x04..0x7c. Header words
// 0x80 and 0x84 reference static/animated linked-light metadata.
constexpr std::size_t section_table_offset = 0x04;
constexpr std::size_t section_table_end = 0x80;
constexpr std::size_t maximum_sections =
    (section_table_end - section_table_offset) / sizeof(std::uint32_t);

std::uint16_t readLe16(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 2) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Truncated EMD 16-bit value"};
  }
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint16_t>(bytes[offset]) |
      (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 4) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Truncated EMD 32-bit value"};
  }
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::int16_t readSignedLe16(std::span<const std::byte> bytes,
                            std::size_t offset) {
  return static_cast<std::int16_t>(readLe16(bytes, offset));
}

EmdUv decodeUv(std::uint16_t packed) {
  return EmdUv{
      static_cast<std::uint8_t>(packed),
      static_cast<std::uint8_t>(packed >> 8U),
  };
}

bool degenerateTriangle(const EmdVertex &first, const EmdVertex &second,
                        const EmdVertex &third) noexcept {
  const auto first_x = static_cast<std::int64_t>(second.x) - first.x;
  const auto first_y = static_cast<std::int64_t>(second.y) - first.y;
  const auto first_z = static_cast<std::int64_t>(second.z) - first.z;
  const auto second_x = static_cast<std::int64_t>(third.x) - first.x;
  const auto second_y = static_cast<std::int64_t>(third.y) - first.y;
  const auto second_z = static_cast<std::int64_t>(third.z) - first.z;
  const auto cross_x = first_y * second_z - first_z * second_y;
  const auto cross_y = first_z * second_x - first_x * second_z;
  const auto cross_z = first_x * second_y - first_y * second_x;
  return cross_x == 0 && cross_y == 0 && cross_z == 0;
}

std::uint16_t vertexIndex(std::uint32_t packed, unsigned int shift) {
  const auto byte_offset = static_cast<std::uint8_t>(packed >> shift);
  if (byte_offset % 3U != 0) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "EMD polygon has an unaligned vertex index"};
  }
  return static_cast<std::uint16_t>(byte_offset / 3U);
}

EmdPolygon parsePolygon(std::span<const std::byte> bytes, std::size_t offset) {
  const auto word0 = readLe32(bytes, offset);
  const auto word1 = readLe32(bytes, offset + 4);
  const auto word2 = readLe32(bytes, offset + 8);

  EmdPolygon polygon;
  polygon.quad = static_cast<std::int32_t>(word0) < 0;
  // Bit 31 describes quad topology; it is not a material/visibility bit.
  // Retail collision boxes therefore encode 0x80000000 for quads and zero
  // for triangles. Only the remaining compact material bits make a polygon
  // renderable.
  polygon.renderable = (word0 & 0x7fffffffU) != 0U;
  polygon.vertex_indices[0] = vertexIndex(word1, 24U);
  polygon.vertex_indices[1] = vertexIndex(word2, 16U);
  polygon.vertex_indices[2] = vertexIndex(word2, 24U);
  polygon.vertex_indices[3] = polygon.quad ? vertexIndex(word2, 0U) : 0;
  polygon.clut =
      static_cast<std::uint16_t>(((word0 >> 16U) & 0x7c0U) | 0x7830U);
  polygon.texture_page = static_cast<std::uint16_t>((word1 >> 16U) & 0xffU);

  const auto base_uv = static_cast<std::uint16_t>(word0);
  if (!polygon.quad) {
    polygon.uv[0] = decodeUv(base_uv);
    polygon.uv[1] = decodeUv(static_cast<std::uint16_t>(word1));
    polygon.uv[2] = decodeUv(static_cast<std::uint16_t>(word2));
    return polygon;
  }

  const auto delta_u =
      static_cast<std::uint16_t>((word2 & 0x800U) != 0 ? 0x0fU : 0x1fU);
  const auto delta_v =
      static_cast<std::uint16_t>((word2 & 0x100U) != 0 ? 0x1f00U : 0x3f00U);
  if ((word2 & 0x200U) == 0) {
    polygon.uv[0] = decodeUv(base_uv);
    polygon.uv[1] = decodeUv(static_cast<std::uint16_t>(base_uv + delta_u));
    polygon.uv[2] = decodeUv(static_cast<std::uint16_t>(base_uv + delta_v));
    polygon.uv[3] =
        decodeUv(static_cast<std::uint16_t>(base_uv + delta_v + delta_u));
  } else {
    polygon.uv[0] = decodeUv(static_cast<std::uint16_t>(base_uv + delta_u));
    polygon.uv[1] = decodeUv(base_uv);
    polygon.uv[2] =
        decodeUv(static_cast<std::uint16_t>(base_uv + delta_v + delta_u));
    polygon.uv[3] = decodeUv(static_cast<std::uint16_t>(base_uv + delta_v));
  }
  return polygon;
}

EmdSection parseSection(std::span<const std::byte> bytes,
                        std::size_t section_offset, std::size_t section_end) {
  if (section_offset > section_end || section_end > bytes.size() ||
      section_end - section_offset < section_header_size) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "EMD section bounds are invalid"};
  }

  const auto polygon_count =
      static_cast<std::size_t>(readLe16(bytes, section_offset + 4));
  const auto vertex_count =
      static_cast<std::size_t>(readLe16(bytes, section_offset + 6));
  const auto vertex_relative_offset =
      static_cast<std::size_t>(readLe32(bytes, section_offset + 0x24));
  const auto polygon_offset = section_offset + section_header_size;
  const auto vertex_offset = section_offset + vertex_relative_offset;
  if (polygon_count > (section_end - polygon_offset) / polygon_size ||
      vertex_relative_offset > section_end - section_offset ||
      vertex_relative_offset <
          section_header_size + polygon_count * polygon_size ||
      vertex_count > (section_end - vertex_offset) / vertex_size) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "EMD section payload is truncated"};
  }

  EmdSection section;
  section.bounds = EmdBounds{
      readSignedLe16(bytes, section_offset + 0x08),
      readSignedLe16(bytes, section_offset + 0x10),
      readSignedLe16(bytes, section_offset + 0x0a),
      readSignedLe16(bytes, section_offset + 0x0c),
      readSignedLe16(bytes, section_offset + 0x12),
      readSignedLe16(bytes, section_offset + 0x0e),
  };
  section.quad_count = readLe32(bytes, section_offset + 0x14);
  if (section.bounds.minimum_x > section.bounds.maximum_x ||
      section.bounds.minimum_y > section.bounds.maximum_y ||
      section.bounds.minimum_z > section.bounds.maximum_z ||
      section.quad_count > polygon_count) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "EMD section metadata is invalid"};
  }
  section.polygons.reserve(polygon_count);
  for (std::size_t index = 0; index < polygon_count; ++index) {
    auto polygon = parsePolygon(bytes, polygon_offset + index * polygon_size);
    const auto used_vertices = polygon.quad ? 4U : 3U;
    if (std::ranges::any_of(polygon.vertex_indices.begin(),
                            polygon.vertex_indices.begin() + used_vertices,
                            [vertex_count](std::uint16_t value) {
                              return value >= vertex_count;
                            })) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "EMD polygon vertex index is out of range"};
    }
    section.polygons.push_back(polygon);
  }

  section.vertices.reserve(vertex_count);
  for (std::size_t index = 0; index < vertex_count; ++index) {
    const auto offset = vertex_offset + index * vertex_size;
    section.vertices.push_back(EmdVertex{
        readSignedLe16(bytes, offset),
        readSignedLe16(bytes, offset + 2),
        readSignedLe16(bytes, offset + 4),
        readLe16(bytes, offset + 6),
    });
  }
  for (auto &polygon : section.polygons) {
    // Retail NCLIP evaluates the first three vertices for triangles and
    // quads alike. Preserve that exact rejection rule before PGXP projection.
    polygon.degenerate = degenerateTriangle(
        section.vertices[polygon.vertex_indices[0]],
        section.vertices[polygon.vertex_indices[1]],
        section.vertices[polygon.vertex_indices[2]]);
  }
  return section;
}

} // namespace

std::optional<std::uint8_t>
resolveEmdTexturePageSource(std::uint16_t raw_texture_page,
                            std::uint32_t authored_page_mask,
                            std::uint32_t vlf_page_mask) noexcept {
  const auto direct = static_cast<std::uint8_t>(raw_texture_page & 0x1fU);
  const auto low_nibble = static_cast<std::uint8_t>(direct & 0x0fU);
  const auto has_shifted = low_nibble >= 6U && low_nibble < 12U;
  const auto shifted =
      static_cast<std::uint8_t>(direct - (has_shifted ? 6U : 0U));
  const auto resolve_from =
      [direct, shifted,
       has_shifted](std::uint32_t mask) -> std::optional<std::uint8_t> {
    const auto direct_match = (mask & (1U << direct)) != 0U;
    const auto shifted_match = has_shifted && (mask & (1U << shifted)) != 0U;
    if (direct_match == shifted_match) {
      return std::nullopt;
    }
    return direct_match ? direct : shifted;
  };

  const auto authored = resolve_from(authored_page_mask);
  if (authored) {
    return authored;
  }
  if (has_shifted && (authored_page_mask & (1U << direct)) != 0U &&
      (authored_page_mask & (1U << shifted)) != 0U) {
    return std::nullopt;
  }
  return resolve_from(vlf_page_mask);
}

EmdScene::EmdScene(std::uint32_t flags, std::uint32_t texture_page_mask,
                   std::vector<EmdSection> sections)
    : flags_(flags), texture_page_mask_(texture_page_mask),
      sections_(std::move(sections)) {}

EmdScene EmdScene::parse(std::span<const std::byte> bytes) {
  if (bytes.size() < header_size) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "EMD header is truncated"};
  }

  std::vector<std::size_t> offsets;
  offsets.reserve(maximum_sections);
  for (std::size_t index = 0; index < maximum_sections; ++index) {
    const auto value =
        readLe32(bytes, section_table_offset + index * sizeof(std::uint32_t));
    if (value == std::numeric_limits<std::uint32_t>::max()) {
      break;
    }
    if (value < header_size || value >= bytes.size() ||
        (!offsets.empty() && value <= offsets.back())) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "EMD section offsets are invalid"};
    }
    offsets.push_back(value);
  }
  if (offsets.empty()) {
    throw core::Error{core::ErrorCode::invalid_format, "EMD has no sections"};
  }

  std::vector<EmdSection> sections;
  sections.reserve(offsets.size());
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    const auto end =
        index + 1U < offsets.size() ? offsets[index + 1U] : bytes.size();
    sections.push_back(parseSection(bytes, offsets[index], end));
  }
  return EmdScene{readLe32(bytes, 0), readLe32(bytes, 0x88),
                  std::move(sections)};
}

std::optional<std::uint32_t>
EmdScene::resolvedTexturePageMask(std::uint32_t vlf_page_mask) const noexcept {
  auto result = texture_page_mask_;
  for (const auto &section : sections_) {
    for (const auto &polygon : section.polygons) {
      if (!polygon.renderable || polygon.degenerate) {
        continue;
      }
      const auto page = resolveEmdTexturePageSource(
          polygon.texture_page, texture_page_mask_, vlf_page_mask);
      if (!page) {
        return std::nullopt;
      }
      result |= 1U << *page;
    }
  }
  return result;
}

std::size_t EmdScene::vertexCount() const noexcept {
  return std::accumulate(sections_.begin(), sections_.end(), std::size_t{},
                         [](std::size_t total, const EmdSection &section) {
                           return total + section.vertices.size();
                         });
}

std::size_t EmdScene::polygonCount() const noexcept {
  return std::accumulate(sections_.begin(), sections_.end(), std::size_t{},
                         [](std::size_t total, const EmdSection &section) {
                           return total + section.polygons.size();
                         });
}

} // namespace sf::assets
