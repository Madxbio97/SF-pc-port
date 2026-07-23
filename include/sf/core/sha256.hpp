#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace sf::core {

using Sha256Digest = std::array<std::byte, 32>;

[[nodiscard]] Sha256Digest sha256(std::span<const std::byte> data);
[[nodiscard]] std::string toHex(const Sha256Digest& digest);

} // namespace sf::core
