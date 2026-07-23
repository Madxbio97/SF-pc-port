#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>

namespace sf::core {

template <typename Vertex, std::size_t MaximumVertices>
struct ClippedPolygon {
    std::array<Vertex, MaximumVertices> vertices;
    std::size_t count{};
};

template <typename Vertex, std::size_t MaximumVertices, typename SignedDistance, typename Interpolate>
[[nodiscard]] ClippedPolygon<Vertex, MaximumVertices> clipConvexPolygon(
    std::span<const Vertex> input,
    SignedDistance signed_distance,
    Interpolate interpolate) {
    ClippedPolygon<Vertex, MaximumVertices> result;
    if (input.empty()) {
        return result;
    }
    const auto append = [&result](const Vertex& vertex) {
        if (result.count >= result.vertices.size()) {
            throw std::length_error{"Clipped polygon exceeds its fixed capacity"};
        }
        result.vertices[result.count++] = vertex;
    };

    auto previous = input.back();
    auto previous_distance = signed_distance(previous);
    auto previous_inside = previous_distance >= 0.0;
    for (const auto& current : input) {
        const auto current_distance = signed_distance(current);
        const auto current_inside = current_distance >= 0.0;
        if (current_inside != previous_inside) {
            if (current_distance != 0.0 && previous_distance != 0.0) {
                const auto denominator = previous_distance - current_distance;
                const auto amount = denominator == 0.0
                    ? 0.0
                    : previous_distance / denominator;
                append(interpolate(previous, current, std::clamp(amount, 0.0, 1.0)));
            }
        }
        if (current_inside) {
            append(current);
        }
        previous = current;
        previous_distance = current_distance;
        previous_inside = current_inside;
    }
    return result;
}

} // namespace sf::core
