#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace sf::game {

struct VirusScannerPoint {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t z{};
};

struct VirusScannerTargetCandidate {
  std::uint16_t scene_object{};
  std::int32_t guest_slot{-1};
  std::uint32_t class_id{};
  VirusScannerPoint position{};
};

struct VirusScannerTargetRequest {
  bool valid{};
  std::int32_t guest_slot{-1};
};

[[nodiscard]] constexpr std::int64_t
scannerAxisDistance(std::int32_t lhs, std::int32_t rhs) noexcept {
  const auto difference = static_cast<std::int64_t>(lhs) - rhs;
  return difference < 0 ? -difference : difference;
}
// GRGLO/GDF marker transforms already use native scene coordinates. Unlike
// the retail target bridge they must not accept a mirrored guest-space Y: a
// neighbouring marker on the opposite side of the map would become a false
// scanner return. Reject each full-width delta before squaring so hostile or
// stale int32 coordinates cannot overflow int64.
[[nodiscard]] constexpr std::optional<std::int64_t>
virusScannerDirectDistanceSquared(VirusScannerPoint candidate,
                                  VirusScannerPoint target,
                                  std::int64_t maximum_distance = 128) noexcept {
  constexpr auto kMaximumSafeDistance = std::int64_t{3037000499};
  if (maximum_distance <= 0 || maximum_distance > kMaximumSafeDistance) {
    return std::nullopt;
  }
  const auto dx = scannerAxisDistance(candidate.x, target.x);
  const auto dy = scannerAxisDistance(candidate.y, target.y);
  const auto dz = scannerAxisDistance(candidate.z, target.z);
  if (dx >= maximum_distance || dy >= maximum_distance ||
      dz >= maximum_distance) {
    return std::nullopt;
  }
  const auto limit = maximum_distance * maximum_distance;
  auto total = std::int64_t{};
  for (const auto distance : {dx, dy, dz}) {
    const auto square = distance * distance;
    if (square >= limit - total) {
      return std::nullopt;
    }
    total += square;
  }
  return total;
}

// CandidateAt returns optional<VirusScannerTargetCandidate>. Keeping the
// selector accessor-based avoids a render-frame allocation in GameplaySession
// while giving tests and production one exact slot/coordinate policy.
template <typename CandidateAt>
[[nodiscard]] std::optional<std::uint16_t> selectVirusScannerTarget(
    VirusScannerTargetRequest request, std::size_t candidate_count,
    CandidateAt &&candidate_at, std::uint32_t target_class) noexcept {
  // FUN_8003ce88/FUN_8003d0d0 carry the selected guest object-record slot.
  // Retail never searches by coordinates when that slot is absent or stale:
  // doing so can reveal a different corpse through a neighbouring container.
  if (!request.valid || request.guest_slot < 0) {
    return std::nullopt;
  }
  for (auto index = std::size_t{}; index < candidate_count; ++index) {
    const auto candidate = candidate_at(index);
    if (candidate && candidate->guest_slot == request.guest_slot &&
        candidate->class_id == target_class) {
      return candidate->scene_object;
    }
  }
  return std::nullopt;
}

// FUN_8002bf74 accepts the first matching class-0x6f object whose retail
// distance is strictly below 0x80. Candidate order therefore remains
// authoritative; this is not a nearest-neighbour query.
template <typename CandidateAt>
[[nodiscard]] std::optional<std::uint16_t> selectVirusScannerMarker(
    VirusScannerPoint target, std::size_t candidate_count,
    CandidateAt &&candidate_at, std::int64_t maximum_distance = 128) noexcept {
  for (auto index = std::size_t{}; index < candidate_count; ++index) {
    const auto candidate = candidate_at(index);
    if (!candidate) {
      continue;
    }
    if (virusScannerDirectDistanceSquared(candidate->position, target,
                                          maximum_distance)) {
      return candidate->scene_object;
    }
  }
  return std::nullopt;
}

} // namespace sf::game
