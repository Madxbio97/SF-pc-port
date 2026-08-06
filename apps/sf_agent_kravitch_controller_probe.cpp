#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

constexpr std::uint16_t kravitch_source = 174U;
constexpr std::uint16_t native_shotgun_source = 165U;
constexpr std::uint32_t npc_initializer = 0x8005805cU;
constexpr std::uint32_t npc_initializer_word = 0x27bdffc8U;
constexpr std::uint32_t npc_initializer_next_word = 0xafb1002cU;
constexpr std::uint32_t weapon_profile_byte = 0x8010c38dU;
constexpr std::uint32_t weapon_profile_word = 0x8010c390U;
constexpr std::uint32_t weapon_cooldown_word = 0x8010c398U;
constexpr std::uint32_t weapon_profile_stride = 0x20U;
constexpr std::uint32_t route_counter_read = 0x80059ad0U;
constexpr std::uint32_t route_counter_read_word = 0x92c2004aU;
constexpr std::uint32_t route_counter_threshold = 0x80059ad8U;
constexpr std::uint32_t route_counter_threshold_word = 0x2c420029U;
constexpr std::uint32_t post_shot_cooldown = 0x800633c8U;
constexpr std::uint32_t post_shot_cooldown_word = 0xa243004cU;

std::optional<std::uint32_t>
read32(const sf::psx::Executable &exe, std::uint32_t address) noexcept {
  if (address < exe.header().text_address) {
    return std::nullopt;
  }
  const auto offset64 =
      static_cast<std::uint64_t>(address) - exe.header().text_address;
  if (offset64 + 4U > exe.text().size()) {
    return std::nullopt;
  }
  const auto offset = static_cast<std::size_t>(offset64);
  const auto byte = [&](std::size_t index) {
    return std::to_integer<std::uint32_t>(exe.text()[offset + index]);
  };
  return byte(0U) | (byte(1U) << 8U) | (byte(2U) << 16U) |
         (byte(3U) << 24U);
}

std::optional<std::uint8_t>
read8(const sf::psx::Executable &exe, std::uint32_t address) noexcept {
  if (address < exe.header().text_address) {
    return std::nullopt;
  }
  const auto offset64 =
      static_cast<std::uint64_t>(address) - exe.header().text_address;
  if (offset64 >= exe.text().size()) {
    return std::nullopt;
  }
  return std::to_integer<std::uint8_t>(
      exe.text()[static_cast<std::size_t>(offset64)]);
}

struct WeaponController {
  std::uint8_t cached_profile{};
  std::uint8_t presentation_profile{};
  std::uint8_t fire_latch{};
  std::uint8_t cadence{};
};

std::optional<WeaponController>
weaponController(const sf::psx::Executable &exe, std::uint8_t weapon) {
  const auto offset =
      static_cast<std::uint32_t>(weapon) * weapon_profile_stride;
  const auto cached = read8(exe, weapon_profile_byte + offset);
  const auto word = read32(exe, weapon_profile_word + offset);
  const auto cooldown = read32(exe, weapon_cooldown_word + offset);
  if (!cached || !word || !cooldown) {
    return std::nullopt;
  }
  const auto presentation =
      static_cast<std::uint8_t>((*word >> 3U) & 0x07U);
  const auto burst = static_cast<std::uint8_t>((*word >> 8U) & 0x07U);
  return WeaponController{
      *cached,
      presentation,
      static_cast<std::uint8_t>(burst == 0U ? 1U : 3U),
      static_cast<std::uint8_t>((*cooldown >> 26U) & 0x1fU),
  };
}

bool validateFrame(const sf::game::GameplaySession &gameplay,
                   std::string_view stage) {
  const auto frame = gameplay.legacyPresentationFrame();
  if (!frame || !frame->renderer ||
      frame->sequence != gameplay.legacyPresentationSequence() ||
      frame->renderer->state.objects.size() <= native_shotgun_source) {
    std::cerr << "Agent Kravitch controller gate failed: stage=" << stage
              << " frame=absent\n";
    return false;
  }
  const auto &kravitch = frame->renderer->state.objects[kravitch_source];
  const auto &native_shotgun =
      frame->renderer->state.objects[native_shotgun_source];
  const auto valid =
      kravitch.slot == kravitch_source && kravitch.definition == 53U &&
      kravitch.class_id == 1 &&
      kravitch.object_handler == sf::game::legacy_common_npc_handler &&
      kravitch.attributes == 0xc107U && kravitch.instance != 0U &&
      kravitch.ai_controller != 0U &&
      kravitch.presentation_controller != 0U &&
      kravitch.presentation_enabled == 1U &&
      kravitch.presentation_mode == 3U &&
      native_shotgun.slot == native_shotgun_source &&
      native_shotgun.definition == 53U && native_shotgun.class_id == 1 &&
      native_shotgun.presentation_enabled == 1U &&
      native_shotgun.presentation_mode == 3U;
  if (valid) {
    return true;
  }
  std::cerr << "Agent Kravitch controller gate failed: stage=" << stage
            << " attrs=0x" << std::hex << std::setw(4)
            << std::setfill('0') << kravitch.attributes << std::dec
            << std::setfill(' ') << " instance=0x" << std::hex
            << kravitch.instance << " ai=0x" << kravitch.ai_controller
            << " presentation=0x" << kravitch.presentation_controller
            << std::dec << '/'
            << static_cast<unsigned int>(kravitch.presentation_enabled) << '/'
            << static_cast<unsigned int>(kravitch.presentation_mode)
            << " native="
            << static_cast<unsigned int>(native_shotgun.presentation_enabled)
            << '/' << static_cast<unsigned int>(native_shotgun.presentation_mode)
            << '\n';
  return false;
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{
        sf::core::ErrorCode::unsupported,
        "Agent Kravitch controller probe requires USA v1.1"};
  }
  const auto package = sf::game::MissionPackage::load(disc, 0U);
  const auto objects = package.objects().objects();
  if (objects.size() <= native_shotgun_source ||
      objects[kravitch_source].type != 53U ||
      objects[kravitch_source].attributes != 0xc102U ||
      objects[kravitch_source].transform.x != -1487 ||
      objects[kravitch_source].transform.y != 2140 ||
      objects[kravitch_source].transform.z != 6675 ||
      objects[native_shotgun_source].type != 53U ||
      objects[native_shotgun_source].attributes != 0x8107U) {
    std::cerr << "Agent Kravitch controller gate failed: DAT identity\n";
    return 2;
  }

  const auto &exe = package.legacyImage().executable();
  const auto initializer = read32(exe, npc_initializer);
  const auto initializer_next = read32(exe, npc_initializer + 4U);
  const auto glock = weaponController(exe, 2U);
  const auto shotgun = weaponController(exe, 7U);
  const auto route_read = read32(exe, route_counter_read);
  const auto route_threshold = read32(exe, route_counter_threshold);
  const auto cooldown_store = read32(exe, post_shot_cooldown);
  if (!initializer || *initializer != npc_initializer_word || !glock ||
      !initializer_next || *initializer_next != npc_initializer_next_word ||
      !shotgun || !route_read || *route_read != route_counter_read_word ||
      !route_threshold ||
      *route_threshold != route_counter_threshold_word || !cooldown_store ||
      *cooldown_store != post_shot_cooldown_word ||
      glock->cached_profile != 0x0fU ||
      shotgun->cached_profile != 0x19U ||
      glock->presentation_profile != 1U ||
      shotgun->presentation_profile != 2U || glock->fire_latch != 1U ||
      shotgun->fire_latch != 3U || shotgun->cadence != 15U) {
    std::cerr << "Agent Kravitch controller gate failed: retail controller "
                 "signature\n";
    return 2;
  }

  // Agent must be selected before bootstrap. A later record rewrite cannot
  // change the pose/burst profile already cached by FUN_8005805c.
  auto gameplay = sf::game::GameplaySession{package, true};
  if (!validateFrame(gameplay, "initial-agent")) {
    return 2;
  }
  for (std::uint32_t update = 0U; update < 4U; ++update) {
    gameplay.update({});
    if (!gameplay.advanceAudioFrameClock() ||
        !validateFrame(gameplay, "stable-agent")) {
      return 2;
    }
  }

  std::cout << "Agent Kravitch controller gate passed: attributes=0xc107"
               " presentation=1/3 cached-profile=0x19 fire-latch=3 cadence=15"
               " native-shotgun-reference=165 initial-agent=1\n";
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: sf_agent_kravitch_controller_probe <game.cue>\n";
    return 1;
  }
  try {
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "Agent Kravitch controller gate failed: " << error.what()
              << '\n';
    return 10;
  }
}
