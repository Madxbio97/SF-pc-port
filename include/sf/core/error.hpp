#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace sf::core {

enum class ErrorCode {
    invalid_argument,
    io,
    invalid_format,
    not_found,
    unsupported,
};

class Error final : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

} // namespace sf::core
