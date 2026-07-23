#include "sf/psx/function_map.hpp"

#include "sf/core/error.hpp"

#include <cstdint>
#include <limits>
#include <map>

namespace sf::psx {
namespace {

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

} // namespace

std::vector<FunctionCandidate> discoverFunctionCandidates(
    std::span<const std::byte> text,
    std::uint32_t load_address,
    std::uint32_t entry_point) {
    if (text.size() % sizeof(std::uint32_t) != 0) {
        throw core::Error{core::ErrorCode::invalid_argument, "MIPS text size is not word-aligned"};
    }
    if (text.size() > std::numeric_limits<std::uint32_t>::max() - load_address) {
        throw core::Error{core::ErrorCode::invalid_argument, "MIPS text range overflows the address space"};
    }
    const auto text_end = load_address + static_cast<std::uint32_t>(text.size());
    if (entry_point < load_address || entry_point >= text_end || entry_point % 4U != 0) {
        throw core::Error{core::ErrorCode::invalid_argument, "Entry point is outside the MIPS text range"};
    }

    std::map<std::uint32_t, std::size_t> candidates;
    candidates.emplace(entry_point, 0);
    for (std::size_t offset = 0; offset < text.size(); offset += 4) {
        const auto instruction = readLe32(text, offset);
        constexpr std::uint32_t opcode_mask = 0xFC000000U;
        constexpr std::uint32_t jal_opcode = 0x0C000000U;
        if ((instruction & opcode_mask) != jal_opcode) {
            continue;
        }

        const auto pc = load_address + static_cast<std::uint32_t>(offset);
        const auto target = ((pc + 4U) & 0xF0000000U) | ((instruction & 0x03FFFFFFU) << 2U);
        if (target >= load_address && target < text_end && target % 4U == 0) {
            ++candidates[target];
        }
    }

    std::vector<FunctionCandidate> result;
    result.reserve(candidates.size());
    for (const auto& [address, call_count] : candidates) {
        result.push_back(FunctionCandidate{address, call_count});
    }
    return result;
}

} // namespace sf::psx
