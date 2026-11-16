#include "sf/game/dynamic_lighting.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/internal/g4_campaign_transition_probe_access.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/mission.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr std::uint32_t light_entry = 0x800d3b8cU;
constexpr std::size_t samples_per_descriptor = 2048U;
constexpr std::size_t maximum_samples = 65536U;

std::optional<std::uint32_t> number(std::string_view text) {
  std::uint32_t value{};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size()
             ? std::optional{value}
             : std::nullopt;
}
std::int16_t lowHalf(std::uint32_t value) {
  return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}
std::size_t checkMission(sf::game::GameDisc &disc,
                         const sf::game::MissionDefinition &definition,
                         std::uint32_t frames) {
  const auto package = sf::game::MissionPackage::load(disc, definition.index);
  sf::game::LegacyFirstMissionRuntime runtime{package.definition(),
                                              package.legacyImage()};
  auto *vm =
      sf::game::G4CampaignTransitionProbeAccess::lightingProbeVm(runtime);
  if (!vm)
    throw std::runtime_error{"original mission runtime is not ready"};
  std::uint32_t instruction{}, colour_instruction{};
  if (!vm->runtime().read32(light_entry, instruction) ||
      instruction != 0x05a00035U ||
      !vm->runtime().read32(0x800d3cb4U, colour_instruction) ||
      colour_instruction != 0x3c0f0101U)
    throw std::runtime_error{"unsupported original light instructions"};

  // Observe real renderer calls, with mission lights first and then the held
  // flashlight. Never inject input or alter the user's running game.
  std::map<std::array<std::uint32_t, 3>, std::size_t> seen;
  std::vector<sf::psx::R3000State> samples;
  bool overflow{};
  vm->bindHostCall(light_entry, [&](sf::game::LegacyHostCallContext &context) {
    const std::array key{context.registerValue(11U), context.registerValue(2U),
                         context.registerValue(14U)};
    if (seen[key]++ < samples_per_descriptor) {
      if (samples.size() == maximum_samples)
        overflow = true;
      else
        samples.push_back(vm->runtime().state());
    }
    context.continueGuestInstruction();
  });
  const auto advance = [&] {
    for (std::uint32_t frame{}; frame < frames; ++frame) {
      runtime.advanceHostUpdate();
      if (runtime.faulted())
        throw std::runtime_error{"original renderer faulted"};
    }
  };
  advance();
  seen.clear();
  if (!sf::game::G4CampaignTransitionProbeAccess::invokeRetailFlashlight(
          runtime, true))
    throw std::runtime_error{"original flashlight toggle failed"};
  advance();
  if (overflow || samples.empty())
    throw std::runtime_error{"original lighting sample coverage is incomplete"};

  std::size_t checked{}, unmatched{}, mismatches{}, lit{};
  std::set<std::uint32_t> projections;
  for (const auto &sample : samples) {
    // RTPT retains its three input vectors. Match the SXY/SZ pair consumed
    // by FUN_800d3b8c to its exact source, not to an estimated world ray.
    int slot = -1;
    for (int index{}; index < 3; ++index)
      if (sample.gte.data[12 + index] == sample.gpr[12] &&
          (sample.gte.data[17 + index] & 0xffffU) == sample.gpr[25]) {
        slot = index;
        break;
      }
    if (slot < 0) {
      ++unmatched;
      continue;
    }
    sf::game::PreparedRetailVertexLight light;
    light.state.extent = std::bit_cast<std::int32_t>(sample.gpr[11]);
    light.state.screen_shift = sample.gpr[2] & 0xffffU;
    light.state.depth_shift = sample.gpr[2] >> 16U;
    light.state.threshold = std::bit_cast<std::int32_t>(sample.gpr[24]);
    light.state.channel_mask = sample.gpr[14];
    for (std::size_t index{}; index < 9U; ++index)
      light.view_rotation[index] =
          lowHalf(sample.gte.control[index / 2U] >> ((index % 2U) * 16U));
    for (std::size_t index{}; index < 3U; ++index)
      light.inverse_translation[index] =
          std::bit_cast<std::int32_t>(sample.gte.control[5U + index]);
    const auto input = static_cast<std::size_t>(slot) * 2U;
    const sf::game::DynamicLightPoint point{
        static_cast<double>(lowHalf(sample.gte.data[input])),
        static_cast<double>(lowHalf(sample.gte.data[input] >> 16U)),
        static_cast<double>(lowHalf(sample.gte.data[input + 1U]))};
    const auto projection = sample.gte.control[26] & 0xffffU;
    projections.insert(projection);

    // Replay the actual instructions through the integer CPU/GTE after all
    // observations are complete. This pure colour routine writes no mission
    // state; no further game tick is run on the replayed CPU state.
    vm->runtime().restoreCpuState(sample);
    const auto reference = vm->runtime().call(light_entry, {}, 1000U);
    if (reference.reason != sf::psx::R3000StopReason::returned)
      throw std::runtime_error{"original light reference did not return"};
    const auto expected = vm->runtime().state().gpr[13];
    const std::array lights{light};
    const auto actual = sf::game::applyRetailVertexLightingPacked(
        sample.gpr[13], lights, point, static_cast<std::int32_t>(projection));
    ++checked;
    lit += expected != sample.gpr[13] ? 1U : 0U;
    if (actual != expected && mismatches++ < 3U)
      std::cerr << "mission=" << definition.index << " point=" << point.x << ','
                << point.y << ',' << point.z << " base=0x" << std::hex
                << sample.gpr[13] << " expected=0x" << expected << " actual=0x"
                << actual << std::dec << '\n';
  }
  std::cout << "mission=" << definition.index
            << " resource=" << definition.resource_name
            << " checked=" << checked << " lit=" << lit
            << " unmatched=" << unmatched << " mismatches=" << mismatches
            << " H=";
  for (const auto projection : projections)
    std::cout << projection << ',';
  std::cout << '\n';
  if (unmatched || mismatches || checked == 0U)
    throw std::runtime_error{"retail lighting parity failed"};
  return checked;
}
} // namespace

int main(int argc, char **argv) try {
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: sf_retail_lighting_probe <USA-v1.1.cue> "
                 "[mission-index] [frames-per-phase]\n";
    return 1;
  }
  const auto mission = argc >= 3 ? number(argv[2]) : std::nullopt;
  const auto frames =
      argc == 4 ? number(argv[3]) : std::optional<std::uint32_t>{30U};
  if ((argc >= 3 &&
       (!mission || *mission >= sf::game::missionCatalog().size())) ||
      !frames || *frames == 0U || *frames > 600U)
    throw std::runtime_error{"invalid mission or frame count"};
  auto disc = sf::game::GameDisc::open(argv[1]);
  std::size_t checked{}, missions{};
  for (const auto &definition : sf::game::missionCatalog()) {
    if (mission && definition.index != *mission)
      continue;
    try {
      checked += checkMission(disc, definition, *frames);
      ++missions;
    } catch (const std::exception &error) {
      std::cerr << "mission=" << definition.index << " " << error.what()
                << '\n';
      return 2;
    }
  }
  std::cout << "Lighting parity passed: missions=" << missions
            << " samples=" << checked << '\n';
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
