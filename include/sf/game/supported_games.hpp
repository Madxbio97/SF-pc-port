#pragma once

#include "sf/core/sha256.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace sf::game {

struct SupportedGame {
    std::string_view title;
    std::string_view region;
    std::string_view version;
    std::string_view serial;
    std::string_view volume_id;
    std::string_view executable_path;
    sf::core::Sha256Digest executable_sha256;
};

[[nodiscard]] std::span<const SupportedGame> supportedGames() noexcept;
[[nodiscard]] std::optional<SupportedGame> identify(
    std::string_view volume_id,
    const sf::core::Sha256Digest& executable_sha256) noexcept;

} // namespace sf::game
