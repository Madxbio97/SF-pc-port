#include "sf/game/agent_mission_hud.hpp"
#include "sf/game/localization.hpp"

#include <array>
#include <cmath>
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

void testScalarNormalizationAndTones() {
  using namespace sf::game;
  const auto rounded = makeAgentMissionHudMeter(
      AgentMissionHudMeterKind::bomb_technician_health, {1U, 3U});
  require(rounded && rounded->percent == 33U &&
              rounded->tone == AgentMissionHudTone::warning,
          "Agent HUD did not round a valid scalar deterministically");

  const auto saturated = makeAgentMissionHudMeter(
      AgentMissionHudMeterKind::bomb_detonation, {120U, 100U});
  require(saturated && saturated->percent == 100U &&
              saturated->tone == AgentMissionHudTone::critical,
          "Agent HUD did not saturate an over-range danger sample");
  require(
      !makeAgentMissionHudMeter(AgentMissionHudMeterKind::suspicion, {1U, 0U}),
      "Agent HUD accepted a zero-range bridge sample");

  require(agentMissionHudTone(AgentMissionHudMeterKind::phagan_health, 25U) ==
                  AgentMissionHudTone::critical &&
              agentMissionHudTone(AgentMissionHudMeterKind::aramov_health,
                                  51U) == AgentMissionHudTone::friendly &&
              agentMissionHudTone(AgentMissionHudMeterKind::aramov_escape,
                                  75U) == AgentMissionHudTone::critical,
          "Agent HUD semantic thresholds are inconsistent");
}

void testRouteProgressSample() {
  using namespace sf::game;
  constexpr std::array straight{
      AgentMissionHudRoutePoint{0.0, 0.0, 0.0},
      AgentMissionHudRoutePoint{10.0, 0.0, 0.0},
  };
  const auto halfway = makeAgentMissionRouteProgressSample(
      std::span<const AgentMissionHudRoutePoint>{straight}, {0.0, 0.0, 0.0},
      {5.0, 2.0, 0.0});
  const auto before_start = makeAgentMissionRouteProgressSample(
      std::span<const AgentMissionHudRoutePoint>{straight}, {0.0, 0.0, 0.0},
      {-5.0, 0.0, 0.0});
  const auto after_end = makeAgentMissionRouteProgressSample(
      std::span<const AgentMissionHudRoutePoint>{straight}, {0.0, 0.0, 0.0},
      {15.0, 0.0, 0.0});
  require(halfway == AgentMissionHudSample{50U, 100U} &&
              before_start == AgentMissionHudSample{0U, 100U} &&
              after_end == AgentMissionHudSample{100U, 100U},
          "Route projection did not clamp and normalize forward progress");

  const auto reversed = makeAgentMissionRouteProgressSample(
      std::span<const AgentMissionHudRoutePoint>{straight}, {9.0, 0.0, 0.0},
      {2.0, 0.0, 0.0});
  require(reversed == AgentMissionHudSample{80U, 100U},
          "Route direction did not follow home to the nearest endpoint");

  constexpr std::array corner{
      AgentMissionHudRoutePoint{0.0, 0.0, 0.0},
      AgentMissionHudRoutePoint{10.0, 0.0, 0.0},
      AgentMissionHudRoutePoint{10.0, 10.0, 0.0},
  };
  const auto around_corner = makeAgentMissionRouteProgressSample(
      std::span<const AgentMissionHudRoutePoint>{corner}, {0.0, 0.0, 0.0},
      {12.0, 5.0, 0.0});
  require(around_corner == AgentMissionHudSample{75U, 100U},
          "Route projection ignored cumulative polyline distance");

  constexpr std::array one_point{AgentMissionHudRoutePoint{0.0, 0.0, 0.0}};
  constexpr std::array zero_length{AgentMissionHudRoutePoint{1.0, 2.0, 3.0},
                                   AgentMissionHudRoutePoint{1.0, 2.0, 3.0}};
  const auto invalid_number =
      std::array{AgentMissionHudRoutePoint{0.0, 0.0, 0.0},
                 AgentMissionHudRoutePoint{
                     std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}};
  require(!makeAgentMissionRouteProgressSample(
              std::span<const AgentMissionHudRoutePoint>{one_point}, {}, {}) &&
              !makeAgentMissionRouteProgressSample(
                  std::span<const AgentMissionHudRoutePoint>{zero_length}, {},
                  {}) &&
              !makeAgentMissionRouteProgressSample(
                  std::span<const AgentMissionHudRoutePoint>{invalid_number},
                  {}, {}),
          "Malformed or zero-length patrol route did not fail closed");
}

void testExactMissionRouting() {
  using namespace sf::game;
  AgentMissionHudState state;
  state.bomb_technician_health = AgentMissionHudSample{125U, 250U};
  state.aramov_escape = AgentMissionHudSample{40U, 100U};
  state.bomb_detonation = AgentMissionHudSample{62U, 100U};
  state.suspicion = AgentMissionHudSample{25U, 100U};
  state.phagan_health = AgentMissionHudSample{10U, 20U};
  state.aramov_health = AgentMissionHudSample{75U, 100U};

  require(makeAgentMissionHudMeters(0U, state).count == 0U &&
              makeAgentMissionHudMeters(3U, state).count == 0U &&
              makeAgentMissionHudMeters(7U, state).count == 0U,
          "Agent HUD leaked a stale meter into another mission");

  const auto sapper = makeAgentMissionHudMeters(1U, state);
  require(sapper.count == 0U,
          "Destroyed Subway retained the redundant CBDC health meter");

  const auto chase = makeAgentMissionHudMeters(2U, state);
  require(chase.count == 1U &&
              chase.values[0].kind == AgentMissionHudMeterKind::aramov_escape,
          "Main Subway Line did not select Aramov escape progress");

  const auto memorial = makeAgentMissionHudMeters(4U, state);
  require(memorial.count == 1U &&
              memorial.values[0].kind ==
                  AgentMissionHudMeterKind::bomb_detonation,
          "Freedom Memorial retained the redundant fuel-tank meter");

  const auto reception = makeAgentMissionHudMeters(5U, state);
  require(reception.count == 0U,
          "Expo Reception retained the redundant suspicion meter");

  const auto dinorama = makeAgentMissionHudMeters(6U, state);
  require(
      dinorama.count == 2U &&
          dinorama.values[0].kind == AgentMissionHudMeterKind::phagan_health &&
          dinorama.values[1].kind == AgentMissionHudMeterKind::aramov_health,
      "Dinorama protected-character meters are incomplete");

  state.phagan_health = AgentMissionHudSample{10U, 0U};
  const auto partial = makeAgentMissionHudMeters(6U, state);
  require(partial.count == 1U &&
              partial.values[0].kind == AgentMissionHudMeterKind::aramov_health,
          "A malformed protected-character sample did not fail closed");
}

void testLocalizedLabels() {
  using namespace sf::game;
  struct LocalizedLabel {
    std::string_view english;
    std::u8string_view russian;
  };
  constexpr std::array labels{
      LocalizedLabel{"BOMB TECH", u8"\u0421\u0410\u041f\u0401\u0420"},
      LocalizedLabel{"ARAMOV ESCAPE",
                     u8"\u041f\u041e\u0411\u0415\u0413 \u0410\u0420\u0410\u041c\u041e\u0412\u041e\u0419"},
      LocalizedLabel{"BOMB DETONATION",
                     u8"\u0414\u0415\u0422\u041e\u041d\u0410\u0426\u0418\u042f \u0411\u041e\u041c\u0411\u042b"},
      LocalizedLabel{"SUSPICION",
                     u8"\u041f\u041e\u0414\u041e\u0417\u0420\u0415\u041d\u0418\u0415"},
      LocalizedLabel{"PHAGAN", u8"\u0424\u042d\u0419\u0413\u0410\u041d"},
      LocalizedLabel{"ARAMOV", u8"\u0410\u0420\u0410\u041c\u041e\u0412\u0410"},
  };
  setGameLanguage(GameLanguage::english);
  for (const auto &[english, russian] : labels) {
    require(localizeTextCopy(english) == english,
            "English Agent HUD label was unexpectedly rewritten");
  }

  setGameLanguage(GameLanguage::russian_vit);
  for (const auto &[english, russian] : labels) {
    require(localizeTextCopy(english) == encodeVitText(russian),
            "Russian Agent HUD label does not match the authored text");
  }
  setGameLanguage(GameLanguage::english);
}

} // namespace

int main() {
  try {
    testScalarNormalizationAndTones();
    testRouteProgressSample();
    testExactMissionRouting();
    testLocalizedLabels();
    std::cout << "Agent mission HUD tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    sf::game::setGameLanguage(sf::game::GameLanguage::english);
    std::cerr << "Agent mission HUD tests failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
