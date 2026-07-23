#include "sf/psx/event_scheduler.hpp"

#include <limits>

namespace sf::psx {

void EventScheduler::reset() noexcept {
  state_ = {};
  state_.next_token = 1U;
}

bool EventScheduler::before(const MachineEvent &left,
                            const MachineEvent &right) noexcept {
  return left.deadline < right.deadline ||
         (left.deadline == right.deadline && left.token < right.token);
}

std::uint64_t EventScheduler::scheduleAfter(std::uint64_t delay,
                                            MachineEventType type,
                                            std::uint8_t index,
                                            std::uint64_t payload) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto deadline =
      delay > maximum - state_.now ? maximum : state_.now + delay;
  return scheduleAt(deadline, type, index, payload);
}

std::uint64_t EventScheduler::scheduleAt(std::uint64_t deadline,
                                         MachineEventType type,
                                         std::uint8_t index,
                                         std::uint64_t payload) noexcept {
  if (deadline < state_.now ||
      state_.event_count >= EventSchedulerState::capacity ||
      state_.next_token == 0U ||
      state_.next_token == std::numeric_limits<std::uint64_t>::max()) {
    return 0U;
  }

  const MachineEvent event{
      deadline, state_.next_token++, payload, type, index,
  };
  auto insertion = static_cast<std::size_t>(state_.event_count);
  while (insertion > 0U && before(event, state_.events[insertion - 1U])) {
    state_.events[insertion] = state_.events[insertion - 1U];
    --insertion;
  }
  state_.events[insertion] = event;
  ++state_.event_count;
  return event.token;
}

bool EventScheduler::popNextDue(std::uint64_t through,
                                MachineEvent &event) noexcept {
  if (state_.event_count == 0U || state_.events[0].deadline > through) {
    return false;
  }
  event = state_.events[0];
  for (std::size_t index = 1U; index < state_.event_count; ++index) {
    state_.events[index - 1U] = state_.events[index];
  }
  --state_.event_count;
  state_.events[state_.event_count] = {};
  return true;
}

bool EventScheduler::cancel(std::uint64_t token) noexcept {
  if (token == 0U) {
    return false;
  }
  for (std::size_t index = 0U; index < state_.event_count; ++index) {
    if (state_.events[index].token != token) {
      continue;
    }
    for (std::size_t next = index + 1U; next < state_.event_count; ++next) {
      state_.events[next - 1U] = state_.events[next];
    }
    --state_.event_count;
    state_.events[state_.event_count] = {};
    return true;
  }
  return false;
}

void EventScheduler::advanceTo(std::uint64_t tick) noexcept {
  if (tick >= state_.now) {
    state_.now = tick;
  }
}

bool EventScheduler::restoreState(const EventSchedulerState &state) noexcept {
  if (state.event_count > EventSchedulerState::capacity ||
      state.next_token == 0U ||
      state.next_token == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  for (std::size_t index = 0U; index < state.event_count; ++index) {
    const auto &event = state.events[index];
    if (event.deadline < state.now || event.token == 0U ||
        event.token >= state.next_token ||
        (index != 0U && before(event, state.events[index - 1U]))) {
      return false;
    }
    for (std::size_t previous = 0U; previous < index; ++previous) {
      if (state.events[previous].token == event.token) {
        return false;
      }
    }
  }
  state_ = state;
  return true;
}

} // namespace sf::psx
