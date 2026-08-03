#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace sf::platform {

// Retail SVD keeps five 20 Hz packet snapshots. The writer advances after
// publishing the current slot; the compositor retains (next_write + 1) through
// (next_write + 3). Native presentation frames must not advance this ring.
template <typename Snapshot> class RetailOpticHistory {
public:
  static constexpr std::size_t slot_count = 5U;
  static constexpr std::size_t echo_count = 3U;

  struct Entry {
    std::uint64_t guest_frame;
    Snapshot snapshot;
  };

  [[nodiscard]] bool
  needsObservation(std::uint64_t guest_frame) const noexcept {
    return !last_guest_frame_ || *last_guest_frame_ != guest_frame;
  }

  void reset() noexcept {
    for (auto &slot : slots_) {
      slot.reset();
    }
    next_write_ = 0U;
    last_guest_frame_.reset();
  }

  [[nodiscard]] bool observe(std::uint64_t guest_frame, Snapshot snapshot) {
    if (last_guest_frame_ && *last_guest_frame_ == guest_frame) {
      return false;
    }
    slots_[next_write_] = Entry{guest_frame, std::move(snapshot)};
    next_write_ = (next_write_ + 1U) % slot_count;
    last_guest_frame_ = guest_frame;
    return true;
  }

  [[nodiscard]] std::array<const Entry *, echo_count>
  retainedEchoes() const noexcept {
    auto result = std::array<const Entry *, echo_count>{};
    for (auto echo = std::size_t{}; echo < echo_count; ++echo) {
      const auto slot = (next_write_ + echo + 1U) % slot_count;
      if (slots_[slot]) {
        result[echo] = &*slots_[slot];
      }
    }
    return result;
  }

  // Capacity planning happens before the new 20 Hz capture is published.
  // Summing every resident slot is deliberately conservative: advancing the
  // retail ring can make an older, previously non-retained slot visible to
  // the three-echo compositor during the same presentation frame.
  template <typename Weight>
  [[nodiscard]] std::size_t storedWeight(Weight &&weight) const noexcept {
    auto result = std::size_t{};
    for (const auto &slot : slots_) {
      if (slot) {
        result += static_cast<std::size_t>(weight(slot->snapshot));
      }
    }
    return result;
  }

  [[nodiscard]] constexpr std::size_t nextWriteSlot() const noexcept {
    return next_write_;
  }

private:
  std::array<std::optional<Entry>, slot_count> slots_{};
  std::size_t next_write_{};
  std::optional<std::uint64_t> last_guest_frame_{};
};

[[nodiscard]] constexpr int
retailOpticEchoDepth(int stored_otz, std::size_t echo_index) noexcept {
  // Retail stores four byte-offset units per OT slot, subtracts eight units
  // per echo, clamps to four, then shifts right by two. RotTransPers3 already
  // returns that final OTZ on the host, so the equivalent bias is two slots.
  const auto step = static_cast<int>(echo_index + 1U) * 2;
  return stored_otz - step < 1 ? 1 : stored_otz - step;
}

} // namespace sf::platform
