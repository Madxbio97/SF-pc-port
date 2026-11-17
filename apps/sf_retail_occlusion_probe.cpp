#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/internal/g4_campaign_transition_probe_access.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Access = sf::game::G4CampaignTransitionProbeAccess;
constexpr std::uint32_t vision_entry = 0x8005a2b0U;
struct Sample {
  std::uint32_t actor{};
  sf::game::LegacyGameplayVmSnapshot state;
};
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error{message};
}

std::size_t checkVision(sf::game::GameDisc &disc, std::uint32_t index) {
  const auto package = sf::game::MissionPackage::load(disc, index);
  auto gameplay = std::make_unique<sf::game::GameplaySession>(package);
  auto *vm = Access::occlusionProbeVm(*gameplay);
  require(vm != nullptr, "mission runtime is unavailable");
  std::set<std::uint32_t> seen;
  std::vector<Sample> samples;
  vm->bindHostCall(vision_entry, [&](sf::game::LegacyHostCallContext &context) {
    const auto actor = context.argument(0U);
    if (samples.size() < 8U && seen.insert(actor).second)
      samples.push_back({actor, vm->captureSnapshot()});
    context.continueGuestInstruction();
  });
  // Neutral input only: this observes startup without driving a playthrough.
  for (unsigned tick{}; tick < 48U; ++tick) {
    gameplay->update({});
    require(!gameplay->runtimeFaulted(), "mission runtime faulted");
  }
  require(vm->unbindHostCall(vision_entry), "cannot remove vision observer");
  std::size_t checked{}, positive{}, blocked{}, native_blocked{};
  for (const auto &sample : samples) {
    for (const auto lit_retained_target : {false, true}) {
      const auto run = [&](sf::game::LegacyBallisticVisibility visibility) {
        require(vm->restoreSnapshot(sample.state),
                "cannot restore vision sample");
        if (lit_retained_target) {
          // Exercise the original nearby-flashlight acquisition branch with a
          // retained positive target flag. No gameplay tick or save uses this
          // diagnostic state; the light itself is enabled by retail code.
          require(vm->invoke(0x80024190U, std::array{1U}, 500000U).completed(),
                  "cannot enable original flashlight");
          auto &cpu = vm->runtime();
          std::uint32_t target{}, ai{}, flags{}, player_slot{};
          require(cpu.read32(sample.actor + 0x14U, target) &&
                      cpu.read32(sample.actor + 0x1cU, ai) &&
                      cpu.read32(target + 4U, flags) &&
                      cpu.read32(0x80116ab0U, player_slot) &&
                      cpu.write16(target,
                                  static_cast<std::uint16_t>(player_slot)) &&
                      cpu.write32(target + 4U, flags | 2U) &&
                      cpu.write16(ai + 0x2cU, 0U),
                  "cannot prepare retained positive target");
        }
        vm->setHostileBallisticLineOfSight(std::move(visibility));
        const auto result =
            vm->invoke(vision_entry, std::array{sample.actor}, 500000U);
        require(result.completed(), "original vision routine did not return");
        return result.return_value;
      };
      const auto original = run({});
      const std::vector reference(vm->runtime().ram().begin(),
                                  vm->runtime().ram().end());
      require(run([](const auto &, const auto &, bool) { return true; }) ==
                      original &&
                  std::ranges::equal(reference, vm->runtime().ram()),
              "clear vision changed original state or result");
      std::size_t blocked_queries{};
      const auto suppressed = run([&](const auto &, const auto &, bool) {
        ++blocked_queries;
        return false;
      });
      if (blocked_queries != 0U) {
        require(original != 0U && suppressed == 0U,
                "occluded original positive vision was not rejected");
        ++positive;
        ++blocked;
      } else {
        require(suppressed == original, "unrelated/negative vision changed");
      }
      bool queried{}, visible{};
      const auto native =
          run([&](const auto &from, const auto &to, bool crouched) {
            queried = true;
            visible =
                Access::occlusionProbeVisible(*gameplay, from, to, crouched);
            return visible;
          });
      require(native == (queried && !visible ? 0U : original),
              "native world confirmation diverged from its geometry query");
      native_blocked += queried && !visible ? 1U : 0U;
      ++checked;
    }
  }
  vm->setHostileBallisticLineOfSight({});
  std::cout << "mission=" << index << " samples=" << checked
            << " positive=" << positive << " rejected=" << blocked
            << " native-occluded=" << native_blocked
            << " clear-state-mismatches=0\n";
  return positive;
}

std::size_t checkFlame(sf::game::GameDisc &disc) {
  const auto package = sf::game::MissionPackage::load(disc, 4U);
  auto gameplay = std::make_unique<sf::game::GameplaySession>(package);
  auto *vm = Access::occlusionProbeVm(*gameplay);
  require(vm != nullptr, "PARK2 runtime is unavailable");
  for (unsigned tick{}; tick < 12U; ++tick)
    gameplay->update({});
  if (const auto bridge = vm->readBridgeState()) {
    for (const auto &object : bridge->objects)
      if (object.class_id == 0x3c)
        std::cout << "PARK2 boss-slot=" << object.slot << " instance=0x"
                  << std::hex << object.instance << std::dec << '\n';
  }
  const auto state = vm->captureSnapshot();
  constexpr auto profile = sf::game::syphonFilterUsaV11Park2FlameLosProfile();
  std::uint32_t player_slot{};
  require(vm->runtime().read32(profile.current_player_slot, player_slot),
          "cannot read PARK2 player identity");
  std::size_t checked{};
  for (const auto return_address : profile.return_addresses) {
    std::uint32_t call{}, delay{};
    require(vm->runtime().read32(return_address - 8U, call) &&
                vm->runtime().read32(return_address - 4U, delay) &&
                call == profile.call_instruction &&
                delay == profile.delay_instruction,
            "unsupported original PARK2 contact instructions");
    const std::array words{0xafbf0014U, 0U, call,        delay,
                           0x8fbf0014U, 0U, 0x03e00008U, 0U};
    std::vector<std::byte> bytes(words.size() * 4U);
    for (std::size_t i{}; i < words.size(); ++i)
      for (unsigned shift{}; shift < 4U; ++shift)
        bytes[i * 4U + shift] =
            static_cast<std::byte>(words[i] >> (shift * 8U));
    const auto entry = return_address - 16U;
    const auto run = [&](sf::game::LegacyBallisticVisibility visibility) {
      require(vm->restoreSnapshot(state) && vm->loadOverlay(entry, bytes),
              "cannot prepare isolated original contact call");
      vm->setHostileBallisticLineOfSight(std::move(visibility));
      const std::array arguments{profile.event_type,
                                 profile.event_mode,
                                 profile.event_id,
                                 player_slot,
                                 0U,
                                 0U,
                                 0U,
                                 0U};
      const auto result = vm->invoke(entry, arguments, 500000U);
      require(result.completed(), "original contact dispatcher did not return");
      return result.return_value;
    };
    const auto original = run({});
    const std::vector reference(vm->runtime().ram().begin(),
                                vm->runtime().ram().end());
    require(run([](const auto &, const auto &, bool) { return true; }) ==
                    original &&
                std::ranges::equal(reference, vm->runtime().ram()),
            "clear flame contact changed original event dispatch");
    std::size_t queries{};
    require(run([&](const auto &, const auto &, bool) {
              ++queries;
              return false;
            }) == 0U &&
                queries == 1U,
            "original flame event ignored live cover query");
    bool queried{}, visible{};
    const auto native =
        run([&](const auto &from, const auto &to, bool crouched) {
          queried = true;
          visible =
              Access::occlusionProbeVisible(*gameplay, from, to, crouched);
          return visible;
        });
    require(queried && native == (visible ? original : 0U),
            "PARK2 native flame cover did not use live roots");
    std::cout << "PARK2 return=0x" << std::hex << return_address << std::dec
              << " native-occluded=" << !visible
              << " clear-state-mismatches=0 blocked=1\n";
    ++checked;
  }
  return checked;
}
} // namespace

int main(int argc, char **argv) try {
  if (argc != 2 && argc != 3) {
    std::cerr << "Usage: sf_retail_occlusion_probe <USA-v1.1.cue> [flame]\n";
    return 1;
  }
  auto disc = sf::game::GameDisc::open(argv[1]);
  if (argc == 3) {
    require(std::string_view{argv[2]} == "flame", "unknown probe selection");
    require(checkFlame(disc) == 2U, "flame coverage is incomplete");
    return 0;
  }
  std::size_t positive{};
  for (const auto &mission : sf::game::missionCatalog())
    positive += checkVision(disc, mission.index);
  const auto flame_calls = checkFlame(disc);
  require(positive > 0U && flame_calls == 2U,
          "occlusion coverage is incomplete");
  std::cout << "Original occlusion gate passed: missions=20 positive-vision="
            << positive << " flame-calls=" << flame_calls << '\n';
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 2;
}
