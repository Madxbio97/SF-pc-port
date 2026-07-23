#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sf::psx {

enum class MachineEventType : std::uint8_t {
  dma_complete,
  cdrom_command,
  cdrom_sector,
};

struct MachineEvent {
  std::uint64_t deadline{};
  std::uint64_t token{};
  std::uint64_t payload{};
  MachineEventType type{MachineEventType::dma_complete};
  std::uint8_t index{};
};

struct EventSchedulerState {
  static constexpr std::size_t capacity = 32U;

  std::uint64_t now{};
  std::uint64_t next_token{1U};
  std::array<MachineEvent, capacity> events{};
  std::uint8_t event_count{};
};

// Small fixed-capacity scheduler. Equal-deadline events are dispatched in
// insertion order, making snapshots and replays independent of host timing.
class EventScheduler final {
public:
  void reset() noexcept;
  [[nodiscard]] std::uint64_t
  scheduleAfter(std::uint64_t delay, MachineEventType type, std::uint8_t index,
                std::uint64_t payload = 0U) noexcept;
  [[nodiscard]] std::uint64_t scheduleAt(std::uint64_t deadline,
                                         MachineEventType type,
                                         std::uint8_t index,
                                         std::uint64_t payload = 0U) noexcept;
  [[nodiscard]] bool popNextDue(std::uint64_t through,
                                MachineEvent &event) noexcept;
  [[nodiscard]] bool cancel(std::uint64_t token) noexcept;
  void advanceTo(std::uint64_t tick) noexcept;

  [[nodiscard]] std::uint64_t now() const noexcept { return state_.now; }
  [[nodiscard]] std::size_t pendingEvents() const noexcept {
    return state_.event_count;
  }
  [[nodiscard]] bool hasDue(std::uint64_t through) const noexcept {
    return state_.event_count != 0U && state_.events[0].deadline <= through;
  }
  [[nodiscard]] EventSchedulerState captureState() const noexcept {
    return state_;
  }
  [[nodiscard]] bool restoreState(const EventSchedulerState &state) noexcept;

private:
  [[nodiscard]] static bool before(const MachineEvent &left,
                                   const MachineEvent &right) noexcept;

  EventSchedulerState state_{};
};

} // namespace sf::psx
