#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sf::game {

using SystemState = std::uint32_t;

class StateTransitionSink {
public:
    virtual ~StateTransitionSink() = default;
    virtual void changeState(SystemState state, bool entering) = 0;

protected:
    StateTransitionSink() = default;
};

// Native recovery of the state-stack behavior at 0x80016020/0x80016094.
class StateStack final {
public:
    static constexpr std::size_t capacity = 11;

    StateStack(SystemState initial_state, StateTransitionSink& sink) noexcept;

    void push(SystemState state);
    void pop();

    [[nodiscard]] SystemState current() const noexcept { return states_[top_]; }
    [[nodiscard]] std::size_t depth() const noexcept { return top_ + 1; }

private:
    std::array<SystemState, capacity> states_{};
    std::size_t top_{};
    StateTransitionSink* sink_{};
};

} // namespace sf::game
