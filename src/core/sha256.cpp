#include "sf/core/sha256.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace sf::core {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
    0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
    0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
    0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (~x & z);
}

constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

void processBlock(std::array<std::uint32_t, 8>& state, std::span<const std::byte, 64> block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        const auto offset = i * 4;
        words[i] = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
                   (std::to_integer<std::uint32_t>(block[offset + 1]) << 16U) |
                   (std::to_integer<std::uint32_t>(block[offset + 2]) << 8U) |
                   std::to_integer<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const auto s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^
                        (words[i - 15] >> 3U);
        const auto s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^
                        (words[i - 2] >> 10U);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto upper_e = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto temp1 = h + upper_e + choose(e, f, g) + round_constants[i] + words[i];
        const auto upper_a = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto temp2 = upper_a + majority(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

} // namespace

Sha256Digest sha256(std::span<const std::byte> data) {
    std::array<std::uint32_t, 8> state{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };

    std::size_t offset = 0;
    while (data.size() - offset >= 64) {
        processBlock(state, std::span<const std::byte, 64>{data.data() + offset, 64});
        offset += 64;
    }

    std::array<std::byte, 128> tail{};
    const auto remainder = data.size() - offset;
    for (std::size_t i = 0; i < remainder; ++i) {
        tail[i] = data[offset + i];
    }
    tail[remainder] = std::byte{0x80};

    const std::size_t padded_size = remainder < 56 ? 64 : 128;
    const auto bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[padded_size - 1 - i] = static_cast<std::byte>(bit_length >> (i * 8U));
    }
    processBlock(state, std::span<const std::byte, 64>{tail.data(), 64});
    if (padded_size == 128) {
        processBlock(state, std::span<const std::byte, 64>{tail.data() + 64, 64});
    }

    Sha256Digest digest{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        digest[i * 4] = static_cast<std::byte>(state[i] >> 24U);
        digest[i * 4 + 1] = static_cast<std::byte>(state[i] >> 16U);
        digest[i * 4 + 2] = static_cast<std::byte>(state[i] >> 8U);
        digest[i * 4 + 3] = static_cast<std::byte>(state[i]);
    }
    return digest;
}

std::string toHex(const Sha256Digest& digest) {
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const auto value : digest) {
        result << std::setw(2) << std::to_integer<unsigned int>(value);
    }
    return result.str();
}

} // namespace sf::core
