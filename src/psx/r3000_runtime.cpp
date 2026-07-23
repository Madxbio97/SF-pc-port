#include "sf/psx/r3000_runtime.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace sf::psx {
namespace {

constexpr std::uint32_t physical_address_mask = 0x1fffffffU;
constexpr std::uint32_t ram_mirror_end = 0x00800000U;
constexpr std::uint32_t scratchpad_address = 0x1f800000U;
constexpr std::uint32_t mmio_address = 0x1f801000U;

std::uint32_t signExtend16(std::uint32_t value) noexcept {
    const auto signed_value = static_cast<std::int16_t>(value & 0xffffU);
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(signed_value));
}

std::int32_t asSigned(std::uint32_t value) noexcept {
    return std::bit_cast<std::int32_t>(value);
}

std::uint32_t arithmeticShiftRight(std::uint32_t value, std::uint32_t amount) noexcept {
    amount &= 31U;
    if (amount == 0U) {
        return value;
    }
    const auto fill = (value & 0x80000000U) != 0U
        ? std::numeric_limits<std::uint32_t>::max() << (32U - amount)
        : 0U;
    return (value >> amount) | fill;
}

bool addOverflows(std::uint32_t left, std::uint32_t right, std::uint32_t& result) noexcept {
    const auto wide = static_cast<std::int64_t>(asSigned(left)) +
        static_cast<std::int64_t>(asSigned(right));
    if (wide < std::numeric_limits<std::int32_t>::min() ||
        wide > std::numeric_limits<std::int32_t>::max()) {
        return true;
    }
    result = static_cast<std::uint32_t>(static_cast<std::int32_t>(wide));
    return false;
}

bool subtractOverflows(
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t& result) noexcept {
    const auto wide = static_cast<std::int64_t>(asSigned(left)) -
        static_cast<std::int64_t>(asSigned(right));
    if (wide < std::numeric_limits<std::int32_t>::min() ||
        wide > std::numeric_limits<std::int32_t>::max()) {
        return true;
    }
    result = static_cast<std::uint32_t>(static_cast<std::int32_t>(wide));
    return false;
}

} // namespace

std::string_view toString(R3000StopReason reason) noexcept {
    switch (reason) {
    case R3000StopReason::running: return "running";
    case R3000StopReason::returned: return "returned";
    case R3000StopReason::instruction_budget: return "instruction budget";
    case R3000StopReason::unsupported_instruction: return "unsupported instruction";
    case R3000StopReason::memory_fault: return "memory fault";
    case R3000StopReason::alignment_fault: return "alignment fault";
    case R3000StopReason::arithmetic_overflow: return "arithmetic overflow";
    case R3000StopReason::syscall: return "syscall";
    case R3000StopReason::breakpoint: return "breakpoint";
    }
    return "unknown";
}

R3000Runtime::R3000Runtime() : ram_(ram_size) {}

void R3000Runtime::clearMemory() noexcept {
    std::ranges::fill(ram_, std::byte{0});
    std::ranges::fill(scratchpad_, std::byte{0});
    std::ranges::fill(mmio_, std::byte{0});
}

void R3000Runtime::loadExecutable(const Executable& executable) {
    clearMemory();
    const auto& header = executable.header();
    if (!loadBytes(header.text_address, executable.text())) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "PS-X EXE text does not fit emulated RAM"};
    }
    const auto stack_pointer = header.stack_address == 0U
        ? 0U
        : header.stack_address + header.stack_size;
    reset(header.initial_pc, header.initial_gp, stack_pointer);
}

bool R3000Runtime::loadBytes(
    std::uint32_t address,
    std::span<const std::byte> bytes) noexcept {
    auto candidate = address;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (memoryByte(candidate) == nullptr ||
            (candidate == std::numeric_limits<std::uint32_t>::max() && index + 1U < bytes.size())) {
            return false;
        }
        ++candidate;
    }
    for (const auto byte : bytes) {
        *memoryByte(address) = byte;
        ++address;
    }
    return true;
}

bool R3000Runtime::copyBytes(
    std::uint32_t address,
    std::span<std::byte> destination) const noexcept {
    auto candidate = address;
    for (std::size_t index = 0; index < destination.size(); ++index) {
        if (memoryByte(candidate) == nullptr ||
            (candidate == std::numeric_limits<std::uint32_t>::max() &&
             index + 1U < destination.size())) {
            return false;
        }
        ++candidate;
    }
    for (auto& byte : destination) {
        byte = *memoryByte(address);
        ++address;
    }
    return true;
}

bool R3000Runtime::restoreRam(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != ram_.size()) {
        return false;
    }
    std::ranges::copy(bytes, ram_.begin());
    return true;
}

bool R3000Runtime::restoreScratchpad(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != scratchpad_.size()) {
        return false;
    }
    std::ranges::copy(bytes, scratchpad_.begin());
    return true;
}

bool R3000Runtime::restoreMmio(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() != mmio_.size()) {
        return false;
    }
    std::ranges::copy(bytes, mmio_.begin());
    return true;
}

void R3000Runtime::reset(std::uint32_t pc, std::uint32_t gp, std::uint32_t sp) noexcept {
    state_ = {};
    state_.pc = pc;
    state_.next_pc = pc + 4U;
    state_.gpr[28] = gp;
    state_.gpr[29] = sp;
    clearLoadDelay();
}

void R3000Runtime::restoreCpuState(const R3000State& state) noexcept {
    state_ = state;
    state_.gpr[0] = 0U;
}

bool R3000Runtime::beginCall(
    std::uint32_t address,
    std::span<const std::uint32_t> arguments) noexcept {
    if (arguments.size() > 4U) {
        const auto extra_count = arguments.size() - 4U;
        if (extra_count >
            (std::numeric_limits<std::uint32_t>::max() - 16U) / sizeof(std::uint32_t)) {
            return false;
        }
        const auto required_bytes =
            16U + static_cast<std::uint32_t>(extra_count * sizeof(std::uint32_t));
        const auto stack_offset = state_.gpr[29] & static_cast<std::uint32_t>(ram_size - 1U);
        if (required_bytes > ram_size || (state_.gpr[29] & 3U) != 0U ||
            stack_offset > ram_size - required_bytes) {
            return false;
        }
        for (std::size_t index = 4U; index < arguments.size(); ++index) {
            const auto offset = 16U + static_cast<std::uint32_t>((index - 4U) * 4U);
            std::uint32_t existing{};
            if (!read32(state_.gpr[29] + offset, existing)) {
                return false;
            }
        }
    }

    clearLoadDelay();
    for (std::size_t index = 0; index < 4U; ++index) {
        state_.gpr[4U + index] = index < arguments.size() ? arguments[index] : 0U;
    }
    for (std::size_t index = 4U; index < arguments.size(); ++index) {
        const auto stack_offset = 16U + static_cast<std::uint32_t>((index - 4U) * 4U);
        if (!write32(state_.gpr[29] + stack_offset, arguments[index])) {
            return false;
        }
    }
    state_.gpr[31] = return_sentinel;
    state_.pc = address;
    state_.next_pc = address + 4U;
    state_.branch_pc = 0U;
    state_.branch_delay_slot = false;
    return true;
}

void R3000Runtime::completeHostCall() noexcept {
    flushLoadDelay();
    state_.pc = state_.gpr[31];
    state_.next_pc = state_.pc + 4U;
    state_.branch_pc = 0U;
    state_.branch_delay_slot = false;
}

void R3000Runtime::settleLoadDelay() noexcept {
    flushLoadDelay();
}

void R3000Runtime::setRegister(std::uint8_t reg, std::uint32_t value) noexcept {
    if (reg < state_.gpr.size()) {
        writeRegister(reg, value);
    }
}

bool R3000Runtime::physicalAddress(
    std::uint32_t address,
    std::uint32_t& physical) noexcept {
    physical = address;
    if (address >= 0x80000000U && address < 0xc0000000U) {
        physical &= physical_address_mask;
        // The scratchpad is cached-only on the R3000A and has no KSEG1 alias.
        if (address >= 0xa0000000U &&
            physical >= scratchpad_address &&
            physical < scratchpad_address + scratchpad_size) {
            return false;
        }
    } else if (address >= 0x20000000U) {
        return false;
    }
    return true;
}

std::byte* R3000Runtime::memoryByte(std::uint32_t address) noexcept {
    std::uint32_t physical{};
    if (!physicalAddress(address, physical)) {
        return nullptr;
    }

    if (physical < ram_mirror_end) {
        return &ram_[physical & static_cast<std::uint32_t>(ram_size - 1U)];
    }
    if (physical >= scratchpad_address &&
        physical < scratchpad_address + scratchpad_size) {
        return &scratchpad_[physical - scratchpad_address];
    }
    if (physical >= mmio_address && physical < mmio_address + mmio_size) {
        return &mmio_[physical - mmio_address];
    }
    return nullptr;
}

const std::byte* R3000Runtime::memoryByte(std::uint32_t address) const noexcept {
    return const_cast<R3000Runtime*>(this)->memoryByte(address);
}

bool R3000Runtime::readMmio(
    std::uint32_t address,
    R3000AccessWidth width,
    std::uint32_t& value) const noexcept {
    std::uint32_t physical{};
    return mmio_bus_ != nullptr && physicalAddress(address, physical) &&
        physical >= mmio_address && physical < mmio_address + mmio_size &&
        mmio_bus_->readMmio(physical, width, value);
}

bool R3000Runtime::writeMmio(
    std::uint32_t address,
    R3000AccessWidth width,
    std::uint32_t value) noexcept {
    std::uint32_t physical{};
    return mmio_bus_ != nullptr && physicalAddress(address, physical) &&
        physical >= mmio_address && physical < mmio_address + mmio_size &&
        mmio_bus_->writeMmio(physical, width, value);
}

bool R3000Runtime::read8(std::uint32_t address, std::uint8_t& value) const noexcept {
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        value = std::to_integer<std::uint8_t>(
            ram_[physical & static_cast<std::uint32_t>(ram_size - 1U)]);
        return true;
    }
    std::uint32_t mmio_value{};
    if (readMmio(address, R3000AccessWidth::byte, mmio_value)) {
        value = static_cast<std::uint8_t>(mmio_value);
        return true;
    }
    const auto* source = memoryByte(address);
    if (source == nullptr) {
        return false;
    }
    value = std::to_integer<std::uint8_t>(*source);
    return true;
}

bool R3000Runtime::read16(std::uint32_t address, std::uint16_t& value) const noexcept {
    if ((address & 1U) != 0U) {
        return false;
    }
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        const auto offset =
            physical & static_cast<std::uint32_t>(ram_size - 1U);
        value = static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(ram_[offset]) |
            (static_cast<std::uint16_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 1U]))
             << 8U));
        return true;
    }
    std::uint32_t mmio_value{};
    if (readMmio(address, R3000AccessWidth::halfword, mmio_value)) {
        value = static_cast<std::uint16_t>(mmio_value);
        return true;
    }
    const auto* byte0 = memoryByte(address);
    const auto* byte1 = memoryByte(address + 1U);
    if (byte0 == nullptr || byte1 == nullptr) {
        return false;
    }
    value = std::to_integer<std::uint8_t>(*byte0) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(*byte1) << 8U);
    return true;
}

bool R3000Runtime::read32(std::uint32_t address, std::uint32_t& value) const noexcept {
    if ((address & 3U) != 0U) {
        return false;
    }
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        const auto offset =
            physical & static_cast<std::uint32_t>(ram_size - 1U);
        value = std::to_integer<std::uint8_t>(ram_[offset]) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 1U]))
             << 8U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 2U]))
             << 16U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 3U]))
             << 24U);
        return true;
    }
    std::uint32_t mmio_value{};
    if (readMmio(address, R3000AccessWidth::word, mmio_value)) {
        value = mmio_value;
        return true;
    }
    const auto* byte0 = memoryByte(address);
    const auto* byte1 = memoryByte(address + 1U);
    const auto* byte2 = memoryByte(address + 2U);
    const auto* byte3 = memoryByte(address + 3U);
    if (byte0 == nullptr || byte1 == nullptr || byte2 == nullptr || byte3 == nullptr) {
        return false;
    }
    value = std::to_integer<std::uint8_t>(*byte0) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(*byte1)) << 8U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(*byte2)) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(*byte3)) << 24U);
    return true;
}

bool R3000Runtime::write8(std::uint32_t address, std::uint8_t value) noexcept {
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        ram_[physical & static_cast<std::uint32_t>(ram_size - 1U)] =
            static_cast<std::byte>(value);
        return true;
    }
    if (writeMmio(address, R3000AccessWidth::byte, value)) {
        return true;
    }
    auto* destination = memoryByte(address);
    if (destination == nullptr) {
        return false;
    }
    *destination = static_cast<std::byte>(value);
    return true;
}

bool R3000Runtime::write16(std::uint32_t address, std::uint16_t value) noexcept {
    if ((address & 1U) != 0U) {
        return false;
    }
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        const auto offset =
            physical & static_cast<std::uint32_t>(ram_size - 1U);
        ram_[offset] = static_cast<std::byte>(value);
        ram_[offset + 1U] = static_cast<std::byte>(value >> 8U);
        return true;
    }
    if (writeMmio(address, R3000AccessWidth::halfword, value)) {
        return true;
    }
    auto* byte0 = memoryByte(address);
    auto* byte1 = memoryByte(address + 1U);
    if (byte0 == nullptr || byte1 == nullptr) {
        return false;
    }
    *byte0 = static_cast<std::byte>(value);
    *byte1 = static_cast<std::byte>(value >> 8U);
    return true;
}

bool R3000Runtime::write32(std::uint32_t address, std::uint32_t value) noexcept {
    if ((address & 3U) != 0U) {
        return false;
    }
    std::uint32_t physical{};
    if (physicalAddress(address, physical) && physical < ram_mirror_end) {
        const auto offset =
            physical & static_cast<std::uint32_t>(ram_size - 1U);
        ram_[offset] = static_cast<std::byte>(value);
        ram_[offset + 1U] = static_cast<std::byte>(value >> 8U);
        ram_[offset + 2U] = static_cast<std::byte>(value >> 16U);
        ram_[offset + 3U] = static_cast<std::byte>(value >> 24U);
        return true;
    }
    if (writeMmio(address, R3000AccessWidth::word, value)) {
        return true;
    }
    auto* byte0 = memoryByte(address);
    auto* byte1 = memoryByte(address + 1U);
    auto* byte2 = memoryByte(address + 2U);
    auto* byte3 = memoryByte(address + 3U);
    if (byte0 == nullptr || byte1 == nullptr || byte2 == nullptr || byte3 == nullptr) {
        return false;
    }
    *byte0 = static_cast<std::byte>(value);
    *byte1 = static_cast<std::byte>(value >> 8U);
    *byte2 = static_cast<std::byte>(value >> 16U);
    *byte3 = static_cast<std::byte>(value >> 24U);
    return true;
}

void R3000Runtime::writeRegister(std::uint8_t reg, std::uint32_t value) noexcept {
    if (reg == 0U) {
        return;
    }
    state_.gpr[reg] = value;
    if (state_.load_delay.valid && state_.load_delay.reg == reg) {
        state_.load_delay.valid = false;
    }
}

void R3000Runtime::scheduleLoad(std::uint8_t reg, std::uint32_t value) noexcept {
    if (reg == 0U) {
        return;
    }
    if (state_.load_delay.valid && state_.load_delay.reg == reg) {
        state_.load_delay.valid = false;
    }
    state_.next_load_delay = R3000DelayedLoadState{reg, value, true};
}

void R3000Runtime::advanceLoadDelay() noexcept {
    if (state_.load_delay.valid && state_.load_delay.reg != 0U) {
        state_.gpr[state_.load_delay.reg] = state_.load_delay.value;
    }
    state_.load_delay = state_.next_load_delay;
    state_.next_load_delay = {};
    state_.gpr[0] = 0U;
}

void R3000Runtime::flushLoadDelay() noexcept {
    advanceLoadDelay();
    advanceLoadDelay();
    clearLoadDelay();
}

void R3000Runtime::clearLoadDelay() noexcept {
    state_.load_delay = {};
    state_.next_load_delay = {};
}

void R3000Runtime::setExternalInterrupt(bool active) noexcept {
    constexpr std::uint32_t hardware_interrupt_bit = 1U << 10U;
    if (active) {
        state_.cop0_cause |= hardware_interrupt_bit;
    } else {
        state_.cop0_cause &= ~hardware_interrupt_bit;
    }
}

bool R3000Runtime::interruptPending() const noexcept {
    constexpr std::uint32_t interrupt_enable_current = 1U;
    constexpr std::uint32_t interrupt_mask = 0x0000ff00U;
    return (state_.cop0_status & interrupt_enable_current) != 0U &&
        (state_.cop0_status & state_.cop0_cause & interrupt_mask) != 0U;
}

void R3000Runtime::takeInterrupt() noexcept {
    constexpr std::uint32_t bootstrap_exception_vector = 1U << 22U;
    constexpr std::uint32_t branch_delay_bit = 1U << 31U;
    constexpr std::uint32_t exception_code_mask = 0x0000007cU;
    constexpr std::uint32_t mode_stack_mask = 0x0000003fU;

    const auto interrupted_pc = state_.pc;
    state_.cop0_epc = state_.branch_delay_slot ? state_.branch_pc : interrupted_pc;
    state_.cop0_cause &= ~(branch_delay_bit | exception_code_mask);
    if (state_.branch_delay_slot) {
        state_.cop0_cause |= branch_delay_bit;
    }
    state_.cop0_status = (state_.cop0_status & ~mode_stack_mask) |
        ((state_.cop0_status << 2U) & mode_stack_mask);
    const auto vector = (state_.cop0_status & bootstrap_exception_vector) != 0U
        ? 0xbfc00180U
        : 0x80000080U;
    flushLoadDelay();
    state_.pc = vector;
    state_.next_pc = vector + 4U;
    state_.branch_pc = 0U;
    state_.branch_delay_slot = false;
}

R3000RunResult R3000Runtime::step() noexcept {
    const auto instruction_pc = state_.pc;
    if (interruptPending()) {
        takeInterrupt();
        return {R3000StopReason::running, 0U, instruction_pc, 0U};
    }
    std::uint32_t instruction{};
    if ((instruction_pc & 3U) != 0U) {
        return {R3000StopReason::alignment_fault, 0U, instruction_pc, 0U};
    }
    const auto fetch_physical = instruction_pc >= 0x80000000U &&
            instruction_pc < 0xc0000000U
        ? instruction_pc & physical_address_mask
        : instruction_pc;
    if (fetch_physical < ram_mirror_end) {
        const auto offset =
            fetch_physical & static_cast<std::uint32_t>(ram_size - 1U);
        instruction = std::to_integer<std::uint8_t>(ram_[offset]) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 1U]))
             << 8U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 2U]))
             << 16U) |
            (static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(ram_[offset + 3U]))
             << 24U);
    } else if (!read32(instruction_pc, instruction)) {
        return {R3000StopReason::memory_fault, 0U, instruction_pc, 0U};
    }

    const auto opcode = static_cast<std::uint8_t>(instruction >> 26U);
    const auto rs = static_cast<std::uint8_t>((instruction >> 21U) & 31U);
    const auto rt = static_cast<std::uint8_t>((instruction >> 16U) & 31U);
    const auto rd = static_cast<std::uint8_t>((instruction >> 11U) & 31U);
    const auto shift = static_cast<std::uint8_t>((instruction >> 6U) & 31U);
    const auto function = static_cast<std::uint8_t>(instruction & 63U);
    const auto immediate = instruction & 0xffffU;
    const auto left = state_.gpr[rs];
    const auto right = state_.gpr[rt];

    state_.branch_delay_slot = false;
    state_.pc = state_.next_pc;
    state_.next_pc += 4U;

    auto stop = R3000StopReason::running;
    auto memoryAddress = [&]() noexcept {
        return left + signExtend16(immediate);
    };
    auto branch = [&](bool taken) noexcept {
        state_.branch_pc = instruction_pc;
        state_.branch_delay_slot = true;
        if (taken) {
            state_.next_pc = instruction_pc + 4U + (signExtend16(immediate) << 2U);
        }
    };
    auto loadWord = [&](std::uint32_t address, std::uint32_t& value) noexcept {
        if ((address & 3U) != 0U) {
            stop = R3000StopReason::alignment_fault;
            return false;
        }
        if (!read32(address, value)) {
            stop = R3000StopReason::memory_fault;
            return false;
        }
        return true;
    };
    auto storeWord = [&](std::uint32_t address, std::uint32_t value) noexcept {
        if ((address & 3U) != 0U) {
            stop = R3000StopReason::alignment_fault;
            return false;
        }
        if (!write32(address, value)) {
            stop = R3000StopReason::memory_fault;
            return false;
        }
        return true;
    };

    switch (opcode) {
    case 0x00: {
        switch (function) {
        case 0x00: writeRegister(rd, right << shift); break;
        case 0x02: writeRegister(rd, right >> shift); break;
        case 0x03: writeRegister(rd, arithmeticShiftRight(right, shift)); break;
        case 0x04: writeRegister(rd, right << (left & 31U)); break;
        case 0x06: writeRegister(rd, right >> (left & 31U)); break;
        case 0x07: writeRegister(rd, arithmeticShiftRight(right, left)); break;
        case 0x08:
            state_.branch_pc = instruction_pc;
            state_.branch_delay_slot = true;
            state_.next_pc = left;
            break;
        case 0x09: {
            const auto target = left;
            writeRegister(rd, instruction_pc + 8U);
            state_.branch_pc = instruction_pc;
            state_.branch_delay_slot = true;
            state_.next_pc = target;
            break;
        }
        case 0x0c: stop = R3000StopReason::syscall; break;
        case 0x0d: stop = R3000StopReason::breakpoint; break;
        case 0x10: writeRegister(rd, state_.hi); break;
        case 0x11: state_.hi = left; break;
        case 0x12: writeRegister(rd, state_.lo); break;
        case 0x13: state_.lo = left; break;
        case 0x18: {
            const auto product = static_cast<std::int64_t>(asSigned(left)) *
                static_cast<std::int64_t>(asSigned(right));
            const auto bits = static_cast<std::uint64_t>(product);
            state_.lo = static_cast<std::uint32_t>(bits);
            state_.hi = static_cast<std::uint32_t>(bits >> 32U);
            break;
        }
        case 0x19: {
            const auto product = static_cast<std::uint64_t>(left) * right;
            state_.lo = static_cast<std::uint32_t>(product);
            state_.hi = static_cast<std::uint32_t>(product >> 32U);
            break;
        }
        case 0x1a: {
            const auto numerator = asSigned(left);
            const auto denominator = asSigned(right);
            if (denominator == 0) {
                state_.lo = numerator >= 0 ? 0xffffffffU : 1U;
                state_.hi = left;
            } else if (left == 0x80000000U && denominator == -1) {
                state_.lo = 0x80000000U;
                state_.hi = 0U;
            } else {
                state_.lo = static_cast<std::uint32_t>(numerator / denominator);
                state_.hi = static_cast<std::uint32_t>(numerator % denominator);
            }
            break;
        }
        case 0x1b:
            if (right == 0U) {
                state_.lo = 0xffffffffU;
                state_.hi = left;
            } else {
                state_.lo = left / right;
                state_.hi = left % right;
            }
            break;
        case 0x20: {
            std::uint32_t value{};
            if (addOverflows(left, right, value)) {
                stop = R3000StopReason::arithmetic_overflow;
            } else {
                writeRegister(rd, value);
            }
            break;
        }
        case 0x21: writeRegister(rd, left + right); break;
        case 0x22: {
            std::uint32_t value{};
            if (subtractOverflows(left, right, value)) {
                stop = R3000StopReason::arithmetic_overflow;
            } else {
                writeRegister(rd, value);
            }
            break;
        }
        case 0x23: writeRegister(rd, left - right); break;
        case 0x24: writeRegister(rd, left & right); break;
        case 0x25: writeRegister(rd, left | right); break;
        case 0x26: writeRegister(rd, left ^ right); break;
        case 0x27: writeRegister(rd, ~(left | right)); break;
        case 0x2a: writeRegister(rd, asSigned(left) < asSigned(right) ? 1U : 0U); break;
        case 0x2b: writeRegister(rd, left < right ? 1U : 0U); break;
        default: stop = R3000StopReason::unsupported_instruction; break;
        }
        break;
    }
    case 0x01: {
        const auto kind = rt;
        const auto greater_equal = (kind & 1U) != 0U;
        const auto link = (kind & 0x1eU) == 0x10U;
        if ((kind & 0x0eU) != 0U && !link) {
            stop = R3000StopReason::unsupported_instruction;
            break;
        }
        if (link) {
            writeRegister(31U, instruction_pc + 8U);
        }
        const auto is_negative = asSigned(left) < 0;
        branch(is_negative != greater_equal);
        break;
    }
    case 0x02:
        state_.branch_pc = instruction_pc;
        state_.branch_delay_slot = true;
        state_.next_pc = (state_.pc & 0xf0000000U) | ((instruction & 0x03ffffffU) << 2U);
        break;
    case 0x03:
        writeRegister(31U, instruction_pc + 8U);
        state_.branch_pc = instruction_pc;
        state_.branch_delay_slot = true;
        state_.next_pc = (state_.pc & 0xf0000000U) | ((instruction & 0x03ffffffU) << 2U);
        break;
    case 0x04: branch(left == right); break;
    case 0x05: branch(left != right); break;
    case 0x06: branch(asSigned(left) <= 0); break;
    case 0x07: branch(asSigned(left) > 0); break;
    case 0x08: {
        std::uint32_t value{};
        if (addOverflows(left, signExtend16(immediate), value)) {
            stop = R3000StopReason::arithmetic_overflow;
        } else {
            writeRegister(rt, value);
        }
        break;
    }
    case 0x09: writeRegister(rt, left + signExtend16(immediate)); break;
    case 0x0a:
        writeRegister(rt, asSigned(left) < asSigned(signExtend16(immediate)) ? 1U : 0U);
        break;
    case 0x0b: writeRegister(rt, left < signExtend16(immediate) ? 1U : 0U); break;
    case 0x0c: writeRegister(rt, left & immediate); break;
    case 0x0d: writeRegister(rt, left | immediate); break;
    case 0x0e: writeRegister(rt, left ^ immediate); break;
    case 0x0f: writeRegister(rt, immediate << 16U); break;
    case 0x10: {
        if (instruction == 0x42000010U) {
            state_.cop0_status = (state_.cop0_status & ~0x0fU) |
                ((state_.cop0_status >> 2U) & 0x0fU);
            break;
        }
        if ((instruction & 0x7ffU) != 0U) {
            stop = R3000StopReason::unsupported_instruction;
            break;
        }
        if (rs == 0U) {
            std::uint32_t value{};
            switch (rd) {
            case 8U: value = state_.cop0_bad_vaddr; break;
            case 12U: value = state_.cop0_status; break;
            case 13U: value = state_.cop0_cause; break;
            case 14U: value = state_.cop0_epc; break;
            default: stop = R3000StopReason::unsupported_instruction; break;
            }
            if (stop == R3000StopReason::running) {
                scheduleLoad(rt, value);
            }
        } else if (rs == 4U) {
            switch (rd) {
            case 12U: state_.cop0_status = right; break;
            case 13U:
                state_.cop0_cause = (state_.cop0_cause & ~0x00000300U) |
                    (right & 0x00000300U);
                break;
            case 14U: state_.cop0_epc = right; break;
            default: stop = R3000StopReason::unsupported_instruction; break;
            }
        } else {
            stop = R3000StopReason::unsupported_instruction;
        }
        break;
    }
    case 0x12:
        if (rs == 0U) {
            scheduleLoad(rt, GteRuntime::readData(state_.gte, rd));
        } else if (rs == 2U) {
            scheduleLoad(rt, GteRuntime::readControl(state_.gte, rd));
        } else if (rs == 4U) {
            GteRuntime::writeData(state_.gte, rd, right);
        } else if (rs == 6U) {
            GteRuntime::writeControl(state_.gte, rd, right);
        } else if ((rs & 0x10U) != 0U) {
            if (!GteRuntime::executeCommand(state_.gte, instruction)) {
                stop = R3000StopReason::unsupported_instruction;
            }
        } else {
            stop = R3000StopReason::unsupported_instruction;
        }
        break;
    case 0x20:
    case 0x24: {
        std::uint8_t value{};
        if (!read8(memoryAddress(), value)) {
            stop = R3000StopReason::memory_fault;
        } else if (opcode == 0x20) {
            scheduleLoad(rt, static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int8_t>(value))));
        } else {
            scheduleLoad(rt, value);
        }
        break;
    }
    case 0x21:
    case 0x25: {
        const auto address = memoryAddress();
        if ((address & 1U) != 0U) {
            stop = R3000StopReason::alignment_fault;
            break;
        }
        std::uint16_t value{};
        if (!read16(address, value)) {
            stop = R3000StopReason::memory_fault;
        } else if (opcode == 0x21) {
            scheduleLoad(rt, static_cast<std::uint32_t>(
                static_cast<std::int32_t>(static_cast<std::int16_t>(value))));
        } else {
            scheduleLoad(rt, value);
        }
        break;
    }
    case 0x22:
    case 0x26: {
        const auto address = memoryAddress();
        std::uint32_t aligned_value{};
        if (!loadWord(address & ~3U, aligned_value)) {
            break;
        }
        const auto existing = state_.load_delay.valid && state_.load_delay.reg == rt
            ? state_.load_delay.value
            : right;
        const auto amount = (address & 3U) * 8U;
        std::uint32_t value{};
        if (opcode == 0x22) {
            const auto mask = 0x00ffffffU >> amount;
            value = (existing & mask) | (aligned_value << (24U - amount));
        } else {
            const auto mask = 0xffffff00U << (24U - amount);
            value = (existing & mask) | (aligned_value >> amount);
        }
        scheduleLoad(rt, value);
        break;
    }
    case 0x23: {
        std::uint32_t value{};
        if (loadWord(memoryAddress(), value)) {
            scheduleLoad(rt, value);
        }
        break;
    }
    case 0x28:
        if (!write8(memoryAddress(), static_cast<std::uint8_t>(right))) {
            stop = R3000StopReason::memory_fault;
        }
        break;
    case 0x29: {
        const auto address = memoryAddress();
        if ((address & 1U) != 0U) {
            stop = R3000StopReason::alignment_fault;
        } else if (!write16(address, static_cast<std::uint16_t>(right))) {
            stop = R3000StopReason::memory_fault;
        }
        break;
    }
    case 0x2a:
    case 0x2e: {
        const auto address = memoryAddress();
        const auto aligned_address = address & ~3U;
        std::uint32_t memory_value{};
        if (!loadWord(aligned_address, memory_value)) {
            break;
        }
        const auto amount = (address & 3U) * 8U;
        std::uint32_t value{};
        if (opcode == 0x2a) {
            const auto mask = 0xffffff00U << amount;
            value = (memory_value & mask) | (right >> (24U - amount));
        } else {
            const auto mask = 0x00ffffffU >> (24U - amount);
            value = (memory_value & mask) | (right << amount);
        }
        static_cast<void>(storeWord(aligned_address, value));
        break;
    }
    case 0x2b: static_cast<void>(storeWord(memoryAddress(), right)); break;
    case 0x32: {
        std::uint32_t value{};
        if (loadWord(memoryAddress(), value)) {
            GteRuntime::writeData(state_.gte, rt, value);
        }
        break;
    }
    case 0x3a:
        static_cast<void>(storeWord(memoryAddress(), GteRuntime::readData(state_.gte, rt)));
        break;
    default: stop = R3000StopReason::unsupported_instruction; break;
    }

    if (stop != R3000StopReason::running) {
        state_.next_load_delay = {};
        advanceLoadDelay();
        return {stop, 0U, instruction_pc, instruction};
    }
    advanceLoadDelay();
    return {R3000StopReason::running, 1U, instruction_pc, instruction};
}

R3000RunResult R3000Runtime::call(
    std::uint32_t address,
    std::span<const std::uint32_t> arguments,
    std::uint64_t instruction_budget) noexcept {
    if (!beginCall(address, arguments)) {
        return {R3000StopReason::memory_fault, 0U, address, 0U};
    }

    for (std::uint64_t count = 0; count < instruction_budget; ++count) {
        if (atReturnSentinel()) {
            settleLoadDelay();
            return {R3000StopReason::returned, count, state_.pc, 0U};
        }
        auto result = step();
        if (result.reason != R3000StopReason::running) {
            result.instructions = count;
            return result;
        }
    }
    if (atReturnSentinel()) {
        settleLoadDelay();
        return {
            R3000StopReason::returned,
            instruction_budget,
            state_.pc,
            0U,
        };
    }
    return {R3000StopReason::instruction_budget, instruction_budget, state_.pc, 0U};
}

} // namespace sf::psx
