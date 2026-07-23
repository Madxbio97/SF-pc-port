#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sf::psx {

struct ExecutableHeader {
    std::uint32_t initial_pc{};
    std::uint32_t initial_gp{};
    std::uint32_t text_address{};
    std::uint32_t text_size{};
    std::uint32_t data_address{};
    std::uint32_t data_size{};
    std::uint32_t bss_address{};
    std::uint32_t bss_size{};
    std::uint32_t stack_address{};
    std::uint32_t stack_size{};
};

class Executable final {
public:
    [[nodiscard]] static Executable parse(std::span<const std::byte> file);

    [[nodiscard]] const ExecutableHeader& header() const noexcept { return header_; }
    [[nodiscard]] std::span<const std::byte> text() const noexcept {
        return std::span{storage_}.subspan(file_header_size, header_.text_size);
    }

private:
    static constexpr std::size_t file_header_size = 2048;

    ExecutableHeader header_;
    std::vector<std::byte> storage_;
};

} // namespace sf::psx
