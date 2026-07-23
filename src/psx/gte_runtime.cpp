#include "sf/psx/gte_runtime.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace sf::psx {
namespace {

constexpr std::uint32_t flag_error = 1U << 31U;
constexpr std::uint32_t flag_ir1_saturated = 1U << 24U;
constexpr std::uint32_t flag_ir2_saturated = 1U << 23U;
constexpr std::uint32_t flag_ir3_saturated = 1U << 22U;
constexpr std::uint32_t flag_color_r_saturated = 1U << 21U;
constexpr std::uint32_t flag_color_g_saturated = 1U << 20U;
constexpr std::uint32_t flag_color_b_saturated = 1U << 19U;
constexpr std::uint32_t flag_sz_otz_saturated = 1U << 18U;
constexpr std::uint32_t flag_divide_overflow = 1U << 17U;
constexpr std::uint32_t flag_mac0_overflow = 1U << 16U;
constexpr std::uint32_t flag_mac0_underflow = 1U << 15U;
constexpr std::uint32_t flag_sx_saturated = 1U << 14U;
constexpr std::uint32_t flag_sy_saturated = 1U << 13U;
constexpr std::uint32_t flag_ir0_saturated = 1U << 12U;
constexpr std::uint32_t error_source_mask = 0x7f87e000U;

std::uint32_t signExtend16(std::uint32_t value) noexcept {
    return static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int16_t>(value)));
}

std::int32_t signedWord(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}

std::int32_t signedHalf(std::uint32_t value) noexcept {
    return static_cast<std::int16_t>(value);
}

std::int32_t lowSignedWord(std::int64_t value) noexcept {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

void updateErrorFlag(GteState& state) noexcept {
    auto& flags = state.control[31];
    flags &= ~flag_error;
    if ((flags & error_source_mask) != 0U) {
        flags |= flag_error;
    }
}

std::int32_t clampIr(
    GteState& state,
    std::uint8_t component,
    std::int32_t value,
    bool limit_mode) noexcept {
    const auto minimum = limit_mode ? 0 : -0x8000;
    constexpr auto maximum = 0x7fff;
    if (value < minimum) {
        state.control[31] |= flag_ir1_saturated >> (component - 1U);
        return minimum;
    }
    if (value > maximum) {
        state.control[31] |= flag_ir1_saturated >> (component - 1U);
        return maximum;
    }
    return value;
}

void setMacAndIr(
    GteState& state,
    std::uint8_t component,
    std::int64_t value,
    std::uint8_t shift,
    bool limit_mode) noexcept {
    constexpr std::int64_t minimum_mac = -(std::int64_t{1} << 43U);
    constexpr std::int64_t maximum_mac = (std::int64_t{1} << 43U) - 1;
    if (value < minimum_mac) {
        state.control[31] |= 1U << (28U - component);
    } else if (value > maximum_mac) {
        state.control[31] |= 1U << (31U - component);
    }
    const auto shifted = lowSignedWord(value >> shift);
    state.data[24U + component] = std::bit_cast<std::uint32_t>(shifted);
    state.data[8U + component] = std::bit_cast<std::uint32_t>(
        clampIr(state, component, shifted, limit_mode));
}

std::int64_t signExtendMac(
    GteState& state,
    std::uint8_t component,
    std::int64_t value) noexcept {
    constexpr std::int64_t minimum_mac = -(std::int64_t{1} << 43U);
    constexpr std::int64_t maximum_mac = (std::int64_t{1} << 43U) - 1;
    if (value < minimum_mac) {
        state.control[31] |= 1U << (28U - component);
    } else if (value > maximum_mac) {
        state.control[31] |= 1U << (31U - component);
    }
    constexpr auto mask = (std::uint64_t{1} << 44U) - 1U;
    auto truncated = static_cast<std::uint64_t>(value) & mask;
    if ((truncated & (std::uint64_t{1} << 43U)) != 0U) {
        truncated |= ~mask;
    }
    return std::bit_cast<std::int64_t>(truncated);
}

std::int16_t packedHalf(std::uint32_t value, bool high) noexcept {
    return static_cast<std::int16_t>(high ? value >> 16U : value);
}

std::int16_t matrixElement(
    const GteState& state,
    std::uint8_t matrix,
    std::uint8_t row,
    std::uint8_t column) noexcept {
    if (matrix == 3U) {
        const auto red = static_cast<std::int16_t>((state.data[6] & 0xffU) << 4U);
        if (row == 0U) {
            if (column == 0U) {
                return static_cast<std::int16_t>(-red);
            }
            if (column == 1U) {
                return red;
            }
            return static_cast<std::int16_t>(state.data[8]);
        }
        const auto value = row == 1U
            ? packedHalf(state.control[1], false)
            : packedHalf(state.control[2], false);
        return value;
    }

    const auto base = static_cast<std::uint8_t>(matrix * 8U);
    const auto element = static_cast<std::uint8_t>(row * 3U + column);
    return packedHalf(
        state.control[base + element / 2U],
        (element & 1U) != 0U);
}

std::int16_t vectorElement(
    const GteState& state,
    std::uint8_t vector,
    std::uint8_t component) noexcept {
    if (vector == 3U) {
        return static_cast<std::int16_t>(state.data[9U + component]);
    }
    const auto base = static_cast<std::uint8_t>(vector * 2U);
    if (component < 2U) {
        return packedHalf(state.data[base], component == 1U);
    }
    return static_cast<std::int16_t>(state.data[base + 1U]);
}

std::int32_t translationElement(
    const GteState& state,
    std::uint8_t translation,
    std::uint8_t component) noexcept {
    switch (translation) {
    case 0U: return signedWord(state.control[5U + component]);
    case 1U: return signedWord(state.control[13U + component]);
    case 2U: return signedWord(state.control[21U + component]);
    default: return 0;
    }
}

std::uint32_t clampColor(
    GteState& state,
    std::uint8_t component,
    std::int32_t value) noexcept {
    if (value < 0) {
        state.control[31] |= flag_color_r_saturated >> component;
        return 0U;
    }
    if (value > 0xff) {
        state.control[31] |= flag_color_r_saturated >> component;
        return 0xffU;
    }
    return static_cast<std::uint32_t>(value);
}

void pushRgbFromMac(GteState& state) noexcept {
    const auto red = clampColor(state, 0U, signedWord(state.data[25]) >> 4U);
    const auto green = clampColor(state, 1U, signedWord(state.data[26]) >> 4U);
    const auto blue = clampColor(state, 2U, signedWord(state.data[27]) >> 4U);
    const auto code = state.data[6] & 0xff000000U;
    state.data[20] = state.data[21];
    state.data[21] = state.data[22];
    state.data[22] = red | (green << 8U) | (blue << 16U) | code;
}

void interpolateColor(
    GteState& state,
    const std::array<std::int64_t, 3U>& input_mac,
    std::uint8_t shift,
    bool limit_mode) noexcept {
    for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
        setMacAndIr(
            state,
            static_cast<std::uint8_t>(component + 1U),
            static_cast<std::int64_t>(
                translationElement(state, 2U, component)) * 4096 -
                input_mac[component],
            shift,
            false);
    }

    const auto interpolation = static_cast<std::int64_t>(
        signedHalf(state.data[8]));
    for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
        setMacAndIr(
            state,
            static_cast<std::uint8_t>(component + 1U),
            static_cast<std::int64_t>(
                signedHalf(state.data[9U + component])) * interpolation +
                input_mac[component],
            shift,
            limit_mode);
    }
}

void depthCueColor(
    GteState& state,
    std::uint32_t color,
    std::uint8_t shift,
    bool limit_mode) noexcept {
    std::array<std::int64_t, 3U> input_mac{};
    for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
        input_mac[component] = static_cast<std::int64_t>(
            (color >> (component * 8U)) & 0xffU) << 16U;
    }
    interpolateColor(state, input_mac, shift, limit_mode);
    pushRgbFromMac(state);
}

void setOtz(GteState& state, std::int32_t value) noexcept {
    if (value < 0) {
        state.control[31] |= flag_sz_otz_saturated;
        value = 0;
    } else if (value > 0xffff) {
        state.control[31] |= flag_sz_otz_saturated;
        value = 0xffff;
    }
    state.data[7] = static_cast<std::uint32_t>(value);
}

void multiplyMatrixVector(
    GteState& state,
    std::uint8_t matrix,
    std::uint8_t translation,
    const std::array<std::int16_t, 3U>& vector,
    std::uint8_t shift,
    bool limit_mode) noexcept {
    for (std::uint8_t row = 0U; row < 3U; ++row) {
        const auto component = static_cast<std::uint8_t>(row + 1U);
        const auto x_product = static_cast<std::int64_t>(
            matrixElement(state, matrix, row, 0U)) * vector[0];
        const auto y_product = static_cast<std::int64_t>(
            matrixElement(state, matrix, row, 1U)) * vector[1];
        const auto z_product = static_cast<std::int64_t>(
            matrixElement(state, matrix, row, 2U)) * vector[2];
        auto value = signExtendMac(
            state,
            component,
            static_cast<std::int64_t>(translationElement(state, translation, row)) *
                    4096 +
                x_product);
        value = signExtendMac(state, component, value + y_product);
        value = signExtendMac(state, component, value + z_product);
        setMacAndIr(state, component, value, shift, limit_mode);
    }
}

void normalColorDepthCue(
    GteState& state,
    std::uint8_t vector,
    std::uint8_t shift,
    bool limit_mode) noexcept {
    const std::array normal{
        vectorElement(state, vector, 0U),
        vectorElement(state, vector, 1U),
        vectorElement(state, vector, 2U),
    };
    multiplyMatrixVector(state, 1U, 3U, normal, shift, limit_mode);
    const std::array lit_normal{
        static_cast<std::int16_t>(state.data[9]),
        static_cast<std::int16_t>(state.data[10]),
        static_cast<std::int16_t>(state.data[11]),
    };
    multiplyMatrixVector(state, 2U, 1U, lit_normal, shift, limit_mode);

    std::array<std::int64_t, 3U> color_mac{};
    for (std::uint8_t component = 0U; component < 3U; ++component) {
        const auto color = static_cast<std::int64_t>(
            (state.data[6] >> (component * 8U)) & 0xffU);
        color_mac[component] = color *
            static_cast<std::int16_t>(state.data[9U + component]) * 16;
        setMacAndIr(
            state,
            static_cast<std::uint8_t>(component + 1U),
            static_cast<std::int64_t>(translationElement(state, 2U, component)) *
                    4096 -
                color_mac[component],
            shift,
            false);
    }
    const auto depth_cue = static_cast<std::int64_t>(
        static_cast<std::int16_t>(state.data[8]));
    for (std::uint8_t component = 0U; component < 3U; ++component) {
        setMacAndIr(
            state,
            static_cast<std::uint8_t>(component + 1U),
            static_cast<std::int64_t>(
                static_cast<std::int16_t>(state.data[9U + component])) *
                    depth_cue +
                color_mac[component],
            shift,
            limit_mode);
    }
    pushRgbFromMac(state);
}

void executeNormalColorDepthCueSingle(
    GteState& state,
    std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>(
        (instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    normalColorDepthCue(state, 0U, shift, limit_mode);
    updateErrorFlag(state);
}

void executeNormalColorDepthCueTriple(
    GteState& state,
    std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>(
        (instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    for (std::uint8_t vector = 0U; vector < 3U; ++vector) {
        normalColorDepthCue(state, vector, shift, limit_mode);
    }
    updateErrorFlag(state);
}

void checkMac0Overflow(GteState& state, std::int64_t value) noexcept {
    if (value < std::numeric_limits<std::int32_t>::min()) {
        state.control[31] |= flag_mac0_underflow;
    } else if (value > std::numeric_limits<std::int32_t>::max()) {
        state.control[31] |= flag_mac0_overflow;
    }
}

void pushScreenDepth(GteState& state, std::int32_t value) noexcept {
    if (value < 0) {
        state.control[31] |= flag_sz_otz_saturated;
        value = 0;
    } else if (value > 0xffff) {
        state.control[31] |= flag_sz_otz_saturated;
        value = 0xffff;
    }
    state.data[16] = state.data[17];
    state.data[17] = state.data[18];
    state.data[18] = state.data[19];
    state.data[19] = static_cast<std::uint32_t>(value);
}

void pushScreenPosition(GteState& state, std::int32_t x, std::int32_t y) noexcept {
    if (x < -1024) {
        state.control[31] |= flag_sx_saturated;
        x = -1024;
    } else if (x > 1023) {
        state.control[31] |= flag_sx_saturated;
        x = 1023;
    }
    if (y < -1024) {
        state.control[31] |= flag_sy_saturated;
        y = -1024;
    } else if (y > 1023) {
        state.control[31] |= flag_sy_saturated;
        y = 1023;
    }
    state.data[12] = state.data[13];
    state.data[13] = state.data[14];
    state.data[14] = static_cast<std::uint16_t>(x) |
        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16U);
}

std::uint32_t dividePerspective(
    GteState& state,
    std::uint32_t numerator,
    std::uint32_t denominator) noexcept {
    if (denominator * 2U <= numerator) {
        state.control[31] |= flag_divide_overflow;
        return 0x1ffffU;
    }

    const auto shift = static_cast<std::uint32_t>(
        std::countl_zero(static_cast<std::uint16_t>(denominator)));
    numerator <<= shift;
    denominator <<= shift;
    const auto index = ((denominator & 0x7fffU) + 0x40U) >> 7U;
    const auto table_value = std::clamp(
        ((0x40000 / static_cast<int>(index + 0x100U) + 1) / 2) - 0x101,
        0,
        0xff);
    const auto estimate = 0x101 + table_value;
    const auto delta =
        (static_cast<std::int32_t>(denominator) * -estimate + 0x80) >> 8U;
    const auto reciprocal = (estimate * (0x20000 + delta) + 0x80) >> 8U;
    const auto result = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(numerator) * static_cast<std::uint32_t>(reciprocal) +
            0x8000U) >> 16U);
    return std::min(0x1ffffU, result);
}

void executeSquare(GteState& state, std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    for (std::uint8_t component = 1U; component <= 3U; ++component) {
        const auto value = static_cast<std::int64_t>(signedHalf(state.data[8U + component]));
        setMacAndIr(state, component, value * value, shift, limit_mode);
    }
    updateErrorFlag(state);
}

void executeGeneralPurposeMultiply(GteState& state, std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    const auto interpolation = static_cast<std::int64_t>(signedHalf(state.data[8]));
    for (std::uint8_t component = 1U; component <= 3U; ++component) {
        const auto value = static_cast<std::int64_t>(signedHalf(state.data[8U + component]));
        setMacAndIr(state, component, interpolation * value, shift, limit_mode);
    }
    pushRgbFromMac(state);
    updateErrorFlag(state);
}

void executeDepthCueColorSingle(
    GteState& state,
    std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>(
        (instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    depthCueColor(state, state.data[6], shift, limit_mode);
    updateErrorFlag(state);
}

void executeDepthCueColorTriple(
    GteState& state,
    std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>(
        (instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    for (std::uint8_t iteration = 0U; iteration < 3U; ++iteration) {
        depthCueColor(state, state.data[20], shift, limit_mode);
    }
    updateErrorFlag(state);
}

void executeDepthCueLight(
    GteState& state,
    std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>(
        (instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    std::array<std::int64_t, 3U> input_mac{};
    for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
        const auto color = static_cast<std::int64_t>(
            (state.data[6] >> (component * 8U)) & 0xffU);
        input_mac[component] = color *
            static_cast<std::int64_t>(signedHalf(state.data[9U + component])) *
            16;
    }
    interpolateColor(state, input_mac, shift, limit_mode);
    pushRgbFromMac(state);
    updateErrorFlag(state);
}

void executeInterpolate(
    GteState& state,
    std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>(
        (instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    std::array<std::int64_t, 3U> input_mac{};
    for (std::uint8_t component = 0U; component < input_mac.size(); ++component) {
        input_mac[component] = static_cast<std::int64_t>(
            signedHalf(state.data[9U + component])) * 4096;
    }
    interpolateColor(state, input_mac, shift, limit_mode);
    pushRgbFromMac(state);
    updateErrorFlag(state);
}

void executeAverageDepth(
    GteState& state,
    std::uint8_t first_depth,
    std::uint8_t depth_count,
    std::uint8_t scale_control) noexcept {
    state.control[31] = 0U;
    auto depth_sum = std::uint32_t{};
    for (std::uint8_t index = 0U; index < depth_count; ++index) {
        depth_sum += state.data[first_depth + index] & 0xffffU;
    }
    const auto result = static_cast<std::int64_t>(
        signedHalf(state.control[scale_control])) *
        static_cast<std::int32_t>(depth_sum);
    checkMac0Overflow(state, result);
    state.data[24] = std::bit_cast<std::uint32_t>(lowSignedWord(result));
    setOtz(state, lowSignedWord(result >> 12U));
    updateErrorFlag(state);
}

void executeMatrixVectorMultiply(GteState& state, std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto matrix = static_cast<std::uint8_t>((instruction >> 17U) & 3U);
    const auto vector = static_cast<std::uint8_t>((instruction >> 15U) & 3U);
    const auto translation = static_cast<std::uint8_t>((instruction >> 13U) & 3U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    const std::array vector_value{
        vectorElement(state, vector, 0U),
        vectorElement(state, vector, 1U),
        vectorElement(state, vector, 2U),
    };

    for (std::uint8_t row = 0U; row < 3U; ++row) {
        const auto component = static_cast<std::uint8_t>(row + 1U);
        const auto x_product = static_cast<std::int64_t>(matrixElement(state, matrix, row, 0U)) *
            vector_value[0];
        const auto y_product = static_cast<std::int64_t>(matrixElement(state, matrix, row, 1U)) *
            vector_value[1];
        const auto z_product = static_cast<std::int64_t>(matrixElement(state, matrix, row, 2U)) *
            vector_value[2];

        if (translation == 2U) {
            const auto first = signExtendMac(
                state,
                component,
                static_cast<std::int64_t>(translationElement(state, translation, row)) * 4096 +
                    x_product);
            static_cast<void>(clampIr(
                state,
                component,
                lowSignedWord(first >> shift),
                false));
            const auto value = signExtendMac(state, component, y_product) + z_product;
            setMacAndIr(
                state,
                component,
                signExtendMac(state, component, value),
                shift,
                limit_mode);
            continue;
        }

        auto value = signExtendMac(
            state,
            component,
            (static_cast<std::int64_t>(translationElement(state, translation, row)) * 4096) +
                x_product);
        value = signExtendMac(state, component, value + y_product);
        value = signExtendMac(state, component, value + z_product);
        setMacAndIr(state, component, value, shift, limit_mode);
    }
    updateErrorFlag(state);
}

void executeOuterProduct(GteState& state, std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    const auto diagonal1 = static_cast<std::int64_t>(packedHalf(state.control[0], false));
    const auto diagonal2 = static_cast<std::int64_t>(packedHalf(state.control[2], false));
    const auto diagonal3 = static_cast<std::int64_t>(packedHalf(state.control[4], false));
    const auto ir1 = static_cast<std::int64_t>(signedHalf(state.data[9]));
    const auto ir2 = static_cast<std::int64_t>(signedHalf(state.data[10]));
    const auto ir3 = static_cast<std::int64_t>(signedHalf(state.data[11]));
    setMacAndIr(state, 1U, ir3 * diagonal2 - ir2 * diagonal3, shift, limit_mode);
    setMacAndIr(state, 2U, ir1 * diagonal3 - ir3 * diagonal1, shift, limit_mode);
    setMacAndIr(state, 3U, ir2 * diagonal1 - ir1 * diagonal2, shift, limit_mode);
    updateErrorFlag(state);
}

void perspectiveTransformVector(
    GteState& state,
    std::uint8_t vector_index,
    std::uint8_t shift,
    bool limit_mode,
    bool last) noexcept {
    const std::array vector{
        vectorElement(state, vector_index, 0U),
        vectorElement(state, vector_index, 1U),
        vectorElement(state, vector_index, 2U),
    };
    std::array<std::int64_t, 3> coordinate{};
    for (std::uint8_t row = 0U; row < 3U; ++row) {
        const auto component = static_cast<std::uint8_t>(row + 1U);
        auto value = signExtendMac(
            state,
            component,
            static_cast<std::int64_t>(translationElement(state, 0U, row)) * 4096 +
                static_cast<std::int64_t>(matrixElement(state, 0U, row, 0U)) * vector[0]);
        value = signExtendMac(
            state,
            component,
            value + static_cast<std::int64_t>(matrixElement(state, 0U, row, 1U)) * vector[1]);
        coordinate[row] = signExtendMac(
            state,
            component,
            value + static_cast<std::int64_t>(matrixElement(state, 0U, row, 2U)) * vector[2]);
        state.data[25U + row] = std::bit_cast<std::uint32_t>(
            lowSignedWord(coordinate[row] >> shift));
    }

    state.data[9] = std::bit_cast<std::uint32_t>(
        clampIr(state, 1U, signedWord(state.data[25]), limit_mode));
    state.data[10] = std::bit_cast<std::uint32_t>(
        clampIr(state, 2U, signedWord(state.data[26]), limit_mode));
    static_cast<void>(clampIr(
        state,
        3U,
        lowSignedWord(coordinate[2] >> 12U),
        false));
    state.data[11] = std::bit_cast<std::uint32_t>(std::clamp(
        signedWord(state.data[27]),
        limit_mode ? 0 : -0x8000,
        0x7fff));

    pushScreenDepth(state, lowSignedWord(coordinate[2] >> 12U));
    const auto quotient = dividePerspective(
        state,
        state.control[26] & 0xffffU,
        state.data[19] & 0xffffU);
    const auto screen_x = static_cast<std::int64_t>(quotient) * signedHalf(state.data[9]) +
        signedWord(state.control[24]);
    const auto screen_y = static_cast<std::int64_t>(quotient) * signedHalf(state.data[10]) +
        signedWord(state.control[25]);
    checkMac0Overflow(state, screen_x);
    checkMac0Overflow(state, screen_y);
    pushScreenPosition(
        state,
        lowSignedWord(screen_x >> 16U),
        lowSignedWord(screen_y >> 16U));

    if (last) {
        const auto depth_cue = static_cast<std::int64_t>(quotient) *
                static_cast<std::int16_t>(state.control[27]) +
            signedWord(state.control[28]);
        checkMac0Overflow(state, depth_cue);
        state.data[24] = std::bit_cast<std::uint32_t>(lowSignedWord(depth_cue));
        auto ir0 = depth_cue >> 12U;
        if (ir0 < 0) {
            state.control[31] |= flag_ir0_saturated;
            ir0 = 0;
        } else if (ir0 > 0x1000) {
            state.control[31] |= flag_ir0_saturated;
            ir0 = 0x1000;
        }
        state.data[8] = static_cast<std::uint32_t>(ir0);
    }
}

void executePerspectiveTransform(GteState& state, std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    perspectiveTransformVector(state, 0U, shift, limit_mode, true);
    updateErrorFlag(state);
}

void executeTriplePerspectiveTransform(GteState& state, std::uint32_t instruction) noexcept {
    state.control[31] = 0U;
    const auto shift = static_cast<std::uint8_t>((instruction & (1U << 19U)) != 0U ? 12U : 0U);
    const auto limit_mode = (instruction & (1U << 10U)) != 0U;
    perspectiveTransformVector(state, 0U, shift, limit_mode, false);
    perspectiveTransformVector(state, 1U, shift, limit_mode, false);
    perspectiveTransformVector(state, 2U, shift, limit_mode, true);
    updateErrorFlag(state);
}

void executeNormalClip(GteState& state) noexcept {
    state.control[31] = 0U;
    const auto sx0 = static_cast<std::int64_t>(packedHalf(state.data[12], false));
    const auto sy0 = static_cast<std::int64_t>(packedHalf(state.data[12], true));
    const auto sx1 = static_cast<std::int64_t>(packedHalf(state.data[13], false));
    const auto sy1 = static_cast<std::int64_t>(packedHalf(state.data[13], true));
    const auto sx2 = static_cast<std::int64_t>(packedHalf(state.data[14], false));
    const auto sy2 = static_cast<std::int64_t>(packedHalf(state.data[14], true));
    const auto area = sx0 * sy1 + sx1 * sy2 + sx2 * sy0 -
        sx0 * sy2 - sx1 * sy0 - sx2 * sy1;
    checkMac0Overflow(state, area);
    state.data[24] = std::bit_cast<std::uint32_t>(lowSignedWord(area));
    updateErrorFlag(state);
}

} // namespace

std::uint32_t GteRuntime::readData(const GteState& state, std::uint8_t index) noexcept {
    if (index == 15U) {
        return state.data[14];
    }
    if (index == 28U || index == 29U) {
        const auto component = [&state](std::uint8_t ir) {
            return static_cast<std::uint32_t>(std::clamp(signedHalf(state.data[ir]) / 0x80, 0, 0x1f));
        };
        return component(9U) | (component(10U) << 5U) | (component(11U) << 10U);
    }
    return state.data[index & 31U];
}

std::uint32_t GteRuntime::readControl(const GteState& state, std::uint8_t index) noexcept {
    return state.control[index & 31U];
}

void GteRuntime::writeData(
    GteState& state,
    std::uint8_t index,
    std::uint32_t value) noexcept {
    index &= 31U;
    switch (index) {
    case 1U:
    case 3U:
    case 5U:
    case 8U:
    case 9U:
    case 10U:
    case 11U:
        state.data[index] = signExtend16(value);
        break;
    case 7U:
    case 16U:
    case 17U:
    case 18U:
    case 19U:
        state.data[index] = value & 0xffffU;
        break;
    case 15U:
        state.data[12] = state.data[13];
        state.data[13] = state.data[14];
        state.data[14] = value;
        break;
    case 28U:
        state.data[28] = value & 0x7fffU;
        state.data[9] = signExtend16((value & 0x1fU) * 0x80U);
        state.data[10] = signExtend16(((value >> 5U) & 0x1fU) * 0x80U);
        state.data[11] = signExtend16(((value >> 10U) & 0x1fU) * 0x80U);
        break;
    case 30U: {
        state.data[30] = value;
        const auto sign_mask = (value & 0x80000000U) != 0U ? 0xffffffffU : 0U;
        state.data[31] = static_cast<std::uint32_t>(std::countl_zero(value ^ sign_mask));
        break;
    }
    case 29U:
    case 31U:
        break;
    default:
        state.data[index] = value;
        break;
    }
}

void GteRuntime::writeControl(
    GteState& state,
    std::uint8_t index,
    std::uint32_t value) noexcept {
    index &= 31U;
    switch (index) {
    case 4U:
    case 12U:
    case 20U:
    case 26U:
    case 27U:
    case 29U:
    case 30U:
        state.control[index] = signExtend16(value);
        break;
    case 31U:
        state.control[31] = value & 0x7ffff000U;
        updateErrorFlag(state);
        break;
    default:
        state.control[index] = value;
        break;
    }
}

bool GteRuntime::executeCommand(GteState& state, std::uint32_t instruction) noexcept {
    switch (instruction & 0x3fU) {
    case 0x01U:
        executePerspectiveTransform(state, instruction);
        return true;
    case 0x06U:
        executeNormalClip(state);
        return true;
    case 0x0cU:
        executeOuterProduct(state, instruction);
        return true;
    case 0x10U:
        executeDepthCueColorSingle(state, instruction);
        return true;
    case 0x11U:
        executeInterpolate(state, instruction);
        return true;
    case 0x12U:
        executeMatrixVectorMultiply(state, instruction);
        return true;
    case 0x13U:
        executeNormalColorDepthCueSingle(state, instruction);
        return true;
    case 0x16U:
        executeNormalColorDepthCueTriple(state, instruction);
        return true;
    case 0x28U:
        executeSquare(state, instruction);
        return true;
    case 0x29U:
        executeDepthCueLight(state, instruction);
        return true;
    case 0x2aU:
        executeDepthCueColorTriple(state, instruction);
        return true;
    case 0x2dU:
        executeAverageDepth(state, 17U, 3U, 29U);
        return true;
    case 0x2eU:
        executeAverageDepth(state, 16U, 4U, 30U);
        return true;
    case 0x30U:
        executeTriplePerspectiveTransform(state, instruction);
        return true;
    case 0x3dU:
        executeGeneralPurposeMultiply(state, instruction);
        return true;
    default:
        return false;
    }
}

} // namespace sf::psx
