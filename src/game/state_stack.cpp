#include "sf/game/state_stack.hpp"

#include "sf/core/error.hpp"

namespace sf::game {

StateStack::StateStack(SystemState initial_state, StateTransitionSink& sink) noexcept
    : states_{initial_state}, sink_(&sink) {}

void StateStack::push(SystemState state) {
    if (top_ >= capacity - 1) {
        throw core::Error{core::ErrorCode::invalid_argument, "System state stack overflow"};
    }
    ++top_;
    states_[top_] = state;
    sink_->changeState(state, true);
}

void StateStack::pop() {
    if (top_ == 0) {
        throw core::Error{core::ErrorCode::invalid_argument, "System state stack underflow"};
    }
    --top_;
    sink_->changeState(states_[top_], false);
}

} // namespace sf::game
