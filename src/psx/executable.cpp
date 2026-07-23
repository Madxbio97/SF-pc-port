#include "sf/psx/executable.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace sf::psx {
namespace {

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw core::Error{core::ErrorCode::invalid_format, "Truncated PS-X EXE header"};
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

bool isKseg0(std::uint32_t address) {
    return address >= 0x80000000U && address < 0xA0000000U;
}

} // namespace

Executable Executable::parse(std::span<const std::byte> file) {
    constexpr std::string_view signature = "PS-X EXE";
    if (file.size() < file_header_size ||
        !std::equal(signature.begin(), signature.end(), file.begin(), [](char expected, std::byte actual) {
            return static_cast<unsigned char>(expected) == std::to_integer<unsigned char>(actual);
        })) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid PS-X EXE signature"};
    }

    Executable result;
    result.header_ = ExecutableHeader{
        readLe32(file, 0x10), readLe32(file, 0x14), readLe32(file, 0x18), readLe32(file, 0x1c),
        readLe32(file, 0x20), readLe32(file, 0x24), readLe32(file, 0x28), readLe32(file, 0x2c),
        readLe32(file, 0x30), readLe32(file, 0x34),
    };

    if (!isKseg0(result.header_.text_address) || !isKseg0(result.header_.initial_pc)) {
        throw core::Error{core::ErrorCode::invalid_format, "PS-X EXE addresses are outside KSEG0"};
    }
    if (result.header_.text_size > file.size() - file_header_size) {
        throw core::Error{core::ErrorCode::invalid_format, "PS-X EXE text segment is truncated"};
    }
    const auto text_end = static_cast<std::uint64_t>(result.header_.text_address) + result.header_.text_size;
    if (result.header_.initial_pc < result.header_.text_address || result.header_.initial_pc >= text_end) {
        throw core::Error{core::ErrorCode::invalid_format, "PS-X EXE entry point is outside the text segment"};
    }

    result.storage_.assign(file.begin(), file.end());
    return result;
}

} // namespace sf::psx
