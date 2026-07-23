#include "sf/platform/stable_frame_vector.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{std::string{message}};
  }
}

struct Packet {
  std::uint32_t words[8]{};
};

void testStorageDoesNotMoveAcrossBusyFrames() {
  sf::platform::StableFrameVector<Packet> packets{"stress packets"};
  constexpr std::size_t packet_budget = 8192U;
  packets.reserve(packet_budget);
  packets.lockStorage();
  const auto *storage = packets.data();

  for (std::size_t frame = 0; frame < 256U; ++frame) {
    if (frame != 0U) {
      packets.reset();
      packets.reserve(packet_budget);
      packets.lockStorage();
    }
    for (std::size_t packet = 0; packet < packet_budget; ++packet) {
      packets.emplace_back().words[0] = static_cast<std::uint32_t>(packet);
    }
    require(packets.data() == storage && packets.storageStable(),
            "Frame packet storage moved under a stable budget");
  }
}

void testCapacityExhaustionFailsBeforeReallocation() {
  sf::platform::StableFrameVector<Packet> packets{"overflow packets"};
  packets.reserve(2U);
  packets.lockStorage();
  packets.emplace_back();
  packets.emplace_back();
  const auto *storage = packets.data();

  bool rejected = false;
  try {
    packets.emplace_back();
  } catch (const sf::core::Error &error) {
    rejected = error.code() == sf::core::ErrorCode::invalid_format;
  }
  require(rejected && packets.size() == 2U && packets.data() == storage &&
              packets.storageStable(),
          "Capacity exhaustion moved or appended an ordering-table packet");
}

void testReserveAfterLockFailsClosed() {
  sf::platform::StableFrameVector<Packet> packets{"late reserve packets"};
  packets.reserve(1U);
  packets.lockStorage();
  const auto *storage = packets.data();

  bool rejected = false;
  try {
    packets.reserve(2U);
  } catch (const sf::core::Error &error) {
    rejected = error.code() == sf::core::ErrorCode::invalid_format;
  }
  require(rejected && packets.data() == storage && packets.storageStable(),
          "Late reserve invalidated ordering-table packet storage");
}

} // namespace

int main() {
  try {
    testStorageDoesNotMoveAcrossBusyFrames();
    testCapacityExhaustionFailsBeforeReallocation();
    testReserveAfterLockFailsClosed();
  } catch (const std::exception &error) {
    std::cerr << "stable frame vector tests failed: " << error.what() << '\n';
    return 1;
  }
  std::cout << "stable frame vector tests passed\n";
  return 0;
}
