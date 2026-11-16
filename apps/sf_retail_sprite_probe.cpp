#include "sf/game/game_disc.hpp"
#include "sf/game/internal/g4_campaign_transition_probe_access.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/mission.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
using Sprite = sf::game::LegacyGuestSpriteBridgeState;
constexpr std::uint32_t regular_entry = 0x800e82b4U;
constexpr std::uint32_t fast_entry = 0x800e87b4U;
// Isolated scratch RAM, used only after the original mission is snapshotted.
// No diagnostic changes are applied to the user's running game or save data.
constexpr std::uint32_t sprite_address = 0x80002000U;
constexpr std::uint32_t ot_address = 0x80002100U;
constexpr std::uint32_t org_address = 0x80002200U;
constexpr std::uint32_t packet_address = 0x80002400U;

Sprite readSprite(const sf::psx::R3000Runtime &cpu, std::uint32_t address) {
  const auto half = [&](std::uint32_t offset) {
    std::uint16_t value{};
    if (!cpu.read16(address + offset, value))
      throw std::runtime_error{"invalid original sprite"};
    return value;
  };
  Sprite sprite;
  std::uint32_t rotation{};
  if (!cpu.read32(address, sprite.attribute) ||
      !cpu.read32(address + 32U, rotation))
    throw std::runtime_error{"invalid original sprite words"};
  sprite.rotation = std::bit_cast<std::int32_t>(rotation);
  sprite.x = std::bit_cast<std::int16_t>(half(4U));
  sprite.y = std::bit_cast<std::int16_t>(half(6U));
  sprite.width = half(8U);
  sprite.height = half(10U);
  sprite.tpage = half(12U);
  const auto uv = half(14U);
  sprite.u = static_cast<std::uint8_t>(uv);
  sprite.v = static_cast<std::uint8_t>(uv >> 8U);
  sprite.center_x = half(16U);
  sprite.center_y = half(18U);
  sprite.mapping_x = std::bit_cast<std::int16_t>(half(24U));
  sprite.mapping_y = std::bit_cast<std::int16_t>(half(26U));
  sprite.scale_x = std::bit_cast<std::int16_t>(half(28U));
  sprite.scale_y = std::bit_cast<std::int16_t>(half(30U));
  return sprite;
}

void check(sf::game::LegacyGameplayVm &vm,
           const sf::game::LegacyGameplayVmSnapshot &snapshot,
           const Sprite &sprite, bool fast, std::size_t &checked) {
  if (!vm.restoreSnapshot(snapshot))
    throw std::runtime_error{"cannot restore isolated original sprite state"};
  auto &cpu = vm.runtime();
  const auto word = [&](std::uint32_t address, std::uint32_t value) {
    if (!cpu.write32(address, value))
      throw std::runtime_error{"invalid scratch word"};
  };
  const auto half = [&](std::uint32_t offset, std::uint16_t value) {
    if (!cpu.write16(sprite_address + offset, value))
      throw std::runtime_error{"invalid scratch halfword"};
  };
  word(sprite_address, sprite.attribute);
  half(4U, static_cast<std::uint16_t>(sprite.x));
  half(6U, static_cast<std::uint16_t>(sprite.y));
  half(8U, sprite.width);
  half(10U, sprite.height);
  half(12U, sprite.tpage);
  half(14U, static_cast<std::uint16_t>(sprite.u | (sprite.v << 8U)));
  half(16U, sprite.center_x);
  half(18U, sprite.center_y);
  word(sprite_address + 20U, 0x00808080U);
  half(24U, static_cast<std::uint16_t>(sprite.mapping_x));
  half(26U, static_cast<std::uint16_t>(sprite.mapping_y));
  half(28U, static_cast<std::uint16_t>(sprite.scale_x));
  half(30U, static_cast<std::uint16_t>(sprite.scale_y));
  word(sprite_address + 32U, std::bit_cast<std::uint32_t>(sprite.rotation));
  word(ot_address, 4U);
  word(ot_address + 4U, org_address);
  word(ot_address + 8U, 0U);
  word(org_address, 0x00ffffffU);
  for (std::uint32_t offset{}; offset < 44U; offset += 4U)
    word(packet_address + offset, 0U);
  word(0x8012c8a0U, packet_address);
  if (!cpu.write16(0x8012f9f8U, 0U) || !cpu.write16(0x8012f9faU, 0U))
    throw std::runtime_error{"invalid original draw offset"};
  std::optional<std::int16_t> original_angle;
  constexpr std::uint32_t rot_matrix_entry = 0x800ebc04U;
  vm.bindHostCall(rot_matrix_entry, [&](sf::game::LegacyHostCallContext &context) {
    std::uint16_t angle{};
    if (!context.read16(context.argument(0U) + 4U, angle))
      throw std::runtime_error{"invalid original rotation input"};
    original_angle = std::bit_cast<std::int16_t>(angle);
    context.continueGuestInstruction();
  });
  const std::array arguments{sprite_address, ot_address, 0U};
  if (!vm.invoke(fast ? fast_entry : regular_entry, arguments, 100000U)
           .completed())
    throw std::runtime_error{"original sprite sorter did not return"};
  if (!vm.unbindHostCall(rot_matrix_entry))
    throw std::runtime_error{"cannot remove rotation observer"};
  const auto read = [&](std::uint32_t offset) {
    std::uint32_t value{};
    if (!cpu.read32(packet_address + offset, value))
      throw std::runtime_error{"cannot read original sprite packet"};
    return value;
  };
  const auto transform = sf::game::legacyGuestSpriteSortTransform(sprite, fast);
  const auto uv = sf::game::legacyGuestSpriteTextureCorners(
      sprite, transform.textured_quad);
  const auto hidden = (sprite.attribute & 0x80000000U) != 0U ||
                      sprite.width == 0U || sprite.height == 0U;
  const auto expected_rotation_call =
      !hidden && transform.textured_quad && sprite.rotation != 0;
  if (original_angle.has_value() != expected_rotation_call ||
      (original_angle && *original_angle !=
                             static_cast<std::int16_t>(transform.angle_units)))
    throw std::runtime_error{"GTE rotation input differs from original"};
  const auto uvWord = [](std::uint8_t u, std::uint8_t v) {
    return static_cast<std::uint32_t>(u) |
           (static_cast<std::uint32_t>(v) << 8U);
  };
  if (hidden) {
    if (read(0U) != 0U)
      throw std::runtime_error{"original hidden sprite emitted a packet"};
  } else if (transform.textured_quad) {
    if ((read(4U) >> 24U & 0xfcU) != 0x2cU ||
        (read(12U) & 0xffffU) != uvWord(uv.left, uv.top) ||
        (read(20U) & 0xffffU) != uvWord(uv.right, uv.top) ||
        (read(28U) & 0xffffU) != uvWord(uv.left, uv.bottom) ||
        (read(36U) & 0xffffU) != uvWord(uv.right, uv.bottom))
      throw std::runtime_error{
          "FT4 selection or UV flip differs from original"};
  } else {
    const auto x = static_cast<std::uint16_t>(
        sprite.x + static_cast<int>(transform.local_left));
    const auto y = static_cast<std::uint16_t>(
        sprite.y + static_cast<int>(transform.local_top));
    if ((read(8U) >> 24U & 0xfcU) != 0x64U ||
        read(12U) != (x | (static_cast<std::uint32_t>(y) << 16U)) ||
        (read(16U) & 0xffffU) != uvWord(uv.left, uv.top) ||
        read(20U) !=
            (sprite.width | (static_cast<std::uint32_t>(sprite.height) << 16U)))
      throw std::runtime_error{
          "SPRT selection, pivot or UV differs from original"};
  }
  ++checked;
}
} // namespace

int main(int argc, char **argv) try {
  if (argc != 2) {
    std::cerr << "Usage: sf_retail_sprite_probe <USA-v1.1.cue>\n";
    return 1;
  }
  auto disc = sf::game::GameDisc::open(argv[1]);
  std::size_t checked{}, calls{}, missions{};
  for (const auto &definition : sf::game::missionCatalog()) {
    const auto package = sf::game::MissionPackage::load(disc, definition.index);
    sf::game::LegacyFirstMissionRuntime runtime{package.definition(),
                                                package.legacyImage()};
    auto *vm =
        sf::game::G4CampaignTransitionProbeAccess::presentationProbeVm(runtime);
    if (!vm)
      throw std::runtime_error{"original mission runtime is not ready"};
    std::vector<Sprite> samples;
    std::set<std::array<std::uint32_t, 3U>> seen;
    std::size_t mission_calls{};
    for (const auto entry : {regular_entry, fast_entry})
      vm->bindHostCall(entry,
                       [&, entry](sf::game::LegacyHostCallContext &context) {
                         ++mission_calls;
                         const auto address = context.argument(0U);
                         auto sprite = readSprite(vm->runtime(), address);
                         sprite.renderer_fast_path = entry == fast_entry;
                         const std::array key{address, sprite.attribute, entry};
                         if (seen.insert(key).second)
                           samples.push_back(sprite);
                         context.continueGuestInstruction();
                       });
    for (unsigned frame{}; frame < 12U; ++frame) {
      runtime.advanceHostUpdate();
      if (runtime.faulted())
        throw std::runtime_error{"original presentation faulted"};
    }
    for (const auto entry : {regular_entry, fast_entry})
      if (!vm->unbindHostCall(entry))
        throw std::runtime_error{"cannot remove sprite observer"};
    if (mission_calls == 0U || samples.empty())
      throw std::runtime_error{"original sprite observation coverage is empty"};
    const auto snapshot = vm->captureSnapshot();
    const auto before = checked;
    for (const auto &sprite : samples)
      check(*vm, snapshot, sprite, sprite.renderer_fast_path, checked);

    if (definition.index == 0U) {
      Sprite sprite;
      sprite.x = 40;
      sprite.y = 30;
      sprite.width = 96U;
      sprite.height = 64U;
      sprite.u = 16U;
      sprite.v = 32U;
      sprite.mapping_x = 48;
      sprite.mapping_y = 32;
      for (const auto flags : {0U, 0x00400000U, 0x00800000U, 0x00c00000U})
        for (const auto shortcut : {0U, 0x08000000U})
          for (const auto visibility : {0U, 0x80000000U})
            for (const auto scale : {4096, 2048, -4096})
              for (const auto angle : {0, 359, -359, 360, -360, 737280}) {
                sprite.attribute = flags | shortcut | visibility;
                sprite.scale_x = static_cast<std::int16_t>(scale);
                sprite.scale_y = 4096;
                sprite.rotation = angle;
                for (const auto fast : {false, true})
                  check(*vm, snapshot, sprite, fast, checked);
              }
      sprite.attribute = 0U;
      sprite.width = 0U;
      check(*vm, snapshot, sprite, false, checked);
      sprite.width = 96U;
      sprite.height = 0U;
      check(*vm, snapshot, sprite, true, checked);
    }
    calls += mission_calls;
    ++missions;
    std::cout << "mission=" << definition.index
              << " resource=" << definition.resource_name
              << " calls=" << mission_calls << " unique=" << samples.size()
              << " checked=" << checked - before << " mismatches=0\n";
  }
  std::cout << "Sprite parity passed: missions=" << missions
            << " original_calls=" << calls << " packets=" << checked << "\n";
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 2;
}
