#include "sf/game/agent_late_mission_rules.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{std::string{message}};
  }
}

void testEliteGuardGrenadeCadence() {
  using namespace sf::game;
  constexpr auto frag = agent_elite_guard_frag_grenade_bit;
  constexpr auto gas = agent_elite_guard_gas_grenade_bit;
  require(agentEliteGuardGrenadeCadenceEligible(15U, frag, true, true, true),
          "An active authored frag user was rejected");
  require(
      !agentEliteGuardGrenadeCadenceEligible(14U, frag, true, true, true) &&
          !agentEliteGuardGrenadeCadenceEligible(16U, frag, true, true, true) &&
          !agentEliteGuardGrenadeCadenceEligible(15U, 0U, true, true, true) &&
          !agentEliteGuardGrenadeCadenceEligible(15U, gas, true, true, true) &&
          !agentEliteGuardGrenadeCadenceEligible(
              15U, static_cast<std::uint16_t>(frag | gas), true, true, true) &&
          !agentEliteGuardGrenadeCadenceEligible(15U, frag, false, true,
                                                 true) &&
          !agentEliteGuardGrenadeCadenceEligible(15U, frag, true, false,
                                                 true) &&
          !agentEliteGuardGrenadeCadenceEligible(15U, frag, true, true, false),
      "Elite cadence leaked to an ineligible actor or grenade type");
  require(agentEliteGuardGrenadeDecisionCounter(0U, true) == 0x0cU &&
              agentEliteGuardGrenadeDecisionCounter(0x0bU, true) == 0x0cU &&
              agentEliteGuardGrenadeDecisionCounter(0x0cU, true) == 0x0cU &&
              agentEliteGuardGrenadeDecisionCounter(0x3cU, true) == 0x3cU &&
              agentEliteGuardGrenadeDecisionCounter(0U, false) == 0U,
          "Elite grenade cadence floor is not monotonic and fail-closed");
}

void testTunnelBlackoutTargetMemory() {
  using namespace sf::game;
  require(agentEnemyTargetMemoryFrames(18U, true, true) == 100U,
          "A validated Tunnel Blackout flashlight lost its memory window");
  require(agentEnemyTargetMemoryFrames(17U, true, true) == 80U &&
              agentEnemyTargetMemoryFrames(19U, true, true) == 80U &&
              agentEnemyTargetMemoryFrames(18U, true, false) == 80U &&
              agentEnemyTargetMemoryFrames(18U, false, true) == 40U,
          "Target memory leaked across mission, mode or flashlight state");
  require(agentEnemyTargetMemoryFrames(18U, true, true, 20U, 60U, 60U) ==
              60U,
          "A non-increasing flashlight limit changed the Agent baseline");
}

void testCbdcFriendlyFirePenalty() {
  using namespace sf::game;
  require(agent_cbdc_friendly_fire_identities.size() == 6U,
          "CBDC identity coverage is incomplete");
  for (const auto &identity : agent_cbdc_friendly_fire_identities) {
    require(agentCbdcFriendlyFireTarget(
                identity.mission, identity.source, identity.definition,
                identity.object_class),
            "An exact CBDC identity was rejected");
  }
  require(!agentCbdcFriendlyFireTarget(2U, 7U, 32U, 0x35U) &&
              !agentCbdcFriendlyFireTarget(3U, 6U, 32U, 0x35U) &&
              !agentCbdcFriendlyFireTarget(3U, 7U, 31U, 0x35U) &&
              !agentCbdcFriendlyFireTarget(3U, 7U, 32U, 1U),
          "CBDC penalty accepted an inexact identity");
  require(agent_cbdc_friendly_fire_penalty_ticks == 600 &&
              agentCbdcFriendlyFireAdjustedTicks(18000) == 17400 &&
              agentCbdcFriendlyFireAdjustedTicks(18000, 2U) == 16800 &&
              agentCbdcFriendlyFireAdjustedTicks(600) == 0 &&
              agentCbdcFriendlyFireAdjustedTicks(200) == 0 &&
              agentCbdcFriendlyFireAdjustedTicks(0) == 0 &&
              agentCbdcFriendlyFireAdjustedTicks(-1) == -1 &&
              agentCbdcFriendlyFireAdjustedTicks(1200, 0U) == 1200 &&
              agentCbdcFriendlyFireAdjustedTicks(
                  1200, std::numeric_limits<std::uint32_t>::max()) == 0,
          "CBDC timer penalty did not clamp or accumulate safely");
}

} // namespace

int main() {
  try {
    testEliteGuardGrenadeCadence();
    testTunnelBlackoutTargetMemory();
    testCbdcFriendlyFirePenalty();
    std::cout << "Agent late mission rule tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "Agent late mission rule tests failed: " << error.what()
              << '\n';
    return EXIT_FAILURE;
  }
}
