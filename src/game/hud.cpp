#include "sf/game/hud.hpp"

#include "sf/game/localization.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sf::game {
namespace {

constexpr WeaponIconDefinition
weaponIcon(std::string_view first = {}, std::string_view second = {},
           std::string_view third = {}) noexcept {
  return WeaponIconDefinition{{first, second, third}};
}

constexpr std::array<std::string_view, 2U> pistol_9mm_pickup_layers{
    "PISTOL2A.TIM", "PISTOL2B.TIM"};
constexpr std::array<std::string_view, 2U> unused_357_pickup_layers{
    "PISTOL3A.TIM", "PISTOL3B.TIM"};
constexpr std::array<std::string_view, 2U> flamethrower_pickup_layers{
    "FLAKA.TIM", "FLAKB.TIM"};
constexpr std::array<std::string_view, 1U> armor_pickup_layers{"VEST2.TIM"};

constexpr std::array weapon_definitions{
    WeaponDefinition{WeaponId::unarmed, "No Weapon", weaponIcon(), 0U, 0U,
                     false, false},
    WeaponDefinition{WeaponId::silenced_9mm, "Silenced 9mm",
                     weaponIcon("PISTOL1A.TIM", "PISTOL1B.TIM"), 15U, 5U,
                     true, true},
    WeaponDefinition{WeaponId::pistol_9mm, "9mm", weaponIcon(), 15U, 5U,
                     true, true},
    WeaponDefinition{WeaponId::unused_357, ".357", weaponIcon(), 0U, 0U,
                     false, false},
    WeaponDefinition{WeaponId::pistol_45, ".45",
                     weaponIcon("PISTOL4A.TIM", "PISTOL4B.TIM"), 10U, 5U,
                     true, true},
    WeaponDefinition{WeaponId::g_18, "G-18",
                     weaponIcon("PISTOL5A.TIM", "PISTOL5B.TIM"), 33U, 5U,
                     true, true},
    WeaponDefinition{WeaponId::combat_shotgun, "Combat Shotgun",
                     weaponIcon("SHOT1A.TIM", "SHOT1B.TIM", "SHOT1C.TIM"),
                     30U, 0U, true, true},
    WeaponDefinition{WeaponId::shotgun, "Shotgun",
                     weaponIcon("SHOT2A.TIM", "SHOT2B.TIM", "SHOT2C.TIM"),
                     25U, 0U, true, true},
    WeaponDefinition{WeaponId::pk_102, "PK-102",
                     weaponIcon("AS1A.TIM", "AS1B.TIM", "AS1C.TIM"), 30U,
                     5U, true, true},
    WeaponDefinition{WeaponId::m_16, "M-16",
                     weaponIcon("AS2A.TIM", "AS2B.TIM", "AS2C.TIM"), 30U,
                     5U, true, true},
    WeaponDefinition{WeaponId::biz_2, "BIZ-2",
                     weaponIcon("AS3A.TIM", "AS3B.TIM", "AS3C.TIM"), 66U,
                     5U, true, true},
    WeaponDefinition{WeaponId::hk_5, "HK-5",
                     weaponIcon("AS4A.TIM", "AS4B.TIM"), 32U, 5U, true,
                     true},
    WeaponDefinition{WeaponId::nightvision_rifle, "Nightvision Rifle",
                     weaponIcon("SNIPER1A.TIM", "SNIPER1B.TIM",
                                "SNIPER1C.TIM"),
                     10U, 2U, true, true},
    WeaponDefinition{WeaponId::sniper_rifle, "Sniper Rifle",
                     weaponIcon("SUPER1A.TIM", "SUPER1B.TIM", "SUPER1C.TIM"),
                     10U, 2U, true, true},
    WeaponDefinition{WeaponId::taser, "Taser",
                     weaponIcon("TASERA.TIM", "TASERB.TIM"), 1U, 0U, false,
                     false},
    // Native item slot 15 deliberately points at HUD group zero. FLAKA/B are
    // present in INTRFACE.HOG, but are not assigned to this weapon by the
    // original SCUS_942.40 item table.
    WeaponDefinition{WeaponId::flamethrower, "Flamethrower", weaponIcon(),
                     90U, 5U, true, true},
    WeaponDefinition{WeaponId::m_79, "M-79",
                     weaponIcon("GRENLANA.TIM", "GRENLANB.TIM",
                                "GRENLANC.TIM"),
                     15U, 0U, true, true},
    WeaponDefinition{WeaponId::k3g4, "K3G4",
                     weaponIcon("G3A.TIM", "G3B.TIM"), 20U, 5U, true, true},
    WeaponDefinition{WeaponId::virus_scanner, "Virus Scanner",
                     weaponIcon("SNIFFER.TIM"), 0U, 0U, false, false},
    WeaponDefinition{WeaponId::fragmentation_grenade, "Grenade",
                     weaponIcon("GRENADEA.TIM", "GRENADEB.TIM"), 10U, 0U,
                     true, true},
    WeaponDefinition{WeaponId::gas_grenade, "Gas Grenade",
                     weaponIcon("GASGRENA.TIM", "GASGRENB.TIM"), 10U, 0U,
                     true, true},
    WeaponDefinition{WeaponId::flashlight, "Flashlight",
                     weaponIcon("FLASHLTA.TIM", "FLASHLTB.TIM"), 0U, 0U,
                     false, false},
    WeaponDefinition{WeaponId::chopper_gun, "Chopper Gun", weaponIcon(), 90U,
                     5U, true, true},
    WeaponDefinition{WeaponId::key_card, "Keycard",
                     weaponIcon("KEYCARDA.TIM", "KEYCARDB.TIM"), 0U, 0U,
                     false, false},
    WeaponDefinition{WeaponId::c4_explosives, "C4 Explosives",
                     weaponIcon("DEVICE.TIM"), 0U, 0U, false, false},
    WeaponDefinition{WeaponId::viral_antigen, "Viral Antigen",
                     weaponIcon("ANTIDOTA.TIM", "ANTIDOTB.TIM"), 0U, 0U,
                     false, false},
};

// Original SCUS_942.40 tables at 0x8010c6e0 and 0x8010c6c4.
constexpr std::array<std::uint8_t, weapon_slot_count> next_weapon{
    1U,  25U, 0U, 0U,  1U,  6U,  7U,  8U,  11U, 4U, 5U,  9U,  13U,
    17U, 16U, 0U, 20U, 10U, 21U, 12U, 19U, 14U, 0U, 18U, 23U, 24U,
};
constexpr std::array<std::uint8_t, weapon_slot_count> previous_weapon{
    1U,  4U,  0U, 0U,  9U,  10U, 5U,  6U,  7U,  11U, 17U, 8U,  19U,
    12U, 21U, 0U, 14U, 13U, 23U, 20U, 16U, 18U, 0U,  24U, 25U, 1U,
};

constexpr std::size_t indexOf(WeaponId id) noexcept {
  return static_cast<std::size_t>(id);
}

constexpr bool definitionsMatchSlots() noexcept {
  if (weapon_definitions.size() != weapon_slot_count) {
    return false;
  }
  for (std::size_t index = 0U; index < weapon_definitions.size(); ++index) {
    if (indexOf(weapon_definitions[index].id) != index) {
      return false;
    }
  }
  return true;
}

constexpr bool linksStayInRange(
    const std::array<std::uint8_t, weapon_slot_count> &links) noexcept {
  for (const auto link : links) {
    if (link >= weapon_slot_count) {
      return false;
    }
  }
  return true;
}

static_assert(definitionsMatchSlots());
static_assert(linksStayInRange(next_weapon));
static_assert(linksStayInRange(previous_weapon));

constexpr std::array<OriginalHudGlyph, 10> hud_digit_glyphs{{
    {0U, 0U, 5U}, {8U, 0U, 5U}, {16U, 0U, 5U}, {24U, 0U, 5U},
    {0U, 8U, 5U}, {8U, 8U, 5U}, {16U, 8U, 5U}, {24U, 8U, 5U},
    {0U, 16U, 5U}, {8U, 16U, 5U},
}};

constexpr std::array<OriginalHudGlyph, 26> hud_lower_glyphs{{
    {8U, 32U, 5U}, {16U, 32U, 4U}, {24U, 32U, 4U}, {0U, 40U, 5U},
    {8U, 40U, 4U}, {16U, 40U, 4U}, {24U, 40U, 5U}, {0U, 48U, 5U},
    {8U, 48U, 1U}, {16U, 48U, 4U}, {24U, 48U, 4U}, {0U, 56U, 4U},
    {8U, 56U, 6U}, {16U, 56U, 5U}, {24U, 56U, 5U}, {32U, 0U, 4U},
    {40U, 0U, 5U}, {48U, 0U, 4U}, {56U, 0U, 4U}, {32U, 8U, 5U},
    {40U, 8U, 5U}, {48U, 8U, 5U}, {56U, 8U, 8U}, {32U, 16U, 5U},
    {40U, 16U, 5U}, {48U, 16U, 4U},
}};

constexpr std::array<OriginalHudGlyph, 26> hud_upper_glyphs{{
    {56U, 16U, 6U}, {32U, 24U, 5U}, {40U, 24U, 5U}, {48U, 24U, 6U},
    {56U, 24U, 4U}, {32U, 32U, 4U}, {40U, 32U, 5U}, {48U, 32U, 5U},
    {56U, 32U, 1U}, {32U, 40U, 4U}, {40U, 40U, 5U}, {48U, 40U, 4U},
    {56U, 40U, 7U}, {32U, 48U, 6U}, {40U, 48U, 5U}, {48U, 48U, 5U},
    {56U, 48U, 6U}, {32U, 56U, 5U}, {40U, 56U, 5U}, {48U, 56U, 5U},
    {56U, 56U, 5U}, {64U, 0U, 6U}, {72U, 0U, 9U}, {88U, 0U, 6U},
    {96U, 0U, 6U}, {64U, 8U, 5U},
}};

// SCES-01913 ViT Co. glyph metadata. The translation keeps the original
// one-byte text pipeline, replaces several Latin cells with Cyrillic, and
// adds 30 glyphs addressed by byte values 0xdf..0xfc.
constexpr std::array<OriginalHudGlyph, 26> vit_lower_glyphs{{
    {8U, 32U, 5U},  {16U, 32U, 4U}, {24U, 32U, 4U}, {0U, 40U, 5U},
    {8U, 40U, 4U},  {16U, 40U, 4U}, {24U, 40U, 5U}, {0U, 48U, 5U},
    {8U, 48U, 6U},  {16U, 48U, 6U}, {24U, 48U, 4U}, {0U, 56U, 5U},
    {8U, 56U, 6U},  {16U, 56U, 5U}, {24U, 56U, 5U}, {32U, 0U, 4U},
    {40U, 0U, 5U},  {48U, 0U, 4U},  {56U, 0U, 5U},  {32U, 8U, 5U},
    {40U, 8U, 4U},  {48U, 8U, 5U},  {56U, 8U, 6U},  {32U, 16U, 5U},
    {40U, 16U, 4U}, {48U, 16U, 4U},
}};

constexpr std::array<OriginalHudGlyph, 26> vit_upper_glyphs{{
    {56U, 16U, 6U}, {32U, 24U, 5U}, {40U, 24U, 5U}, {48U, 24U, 6U},
    {56U, 24U, 4U}, {32U, 32U, 4U}, {40U, 32U, 5U}, {48U, 32U, 5U},
    {56U, 32U, 1U}, {32U, 40U, 4U}, {40U, 40U, 5U}, {48U, 40U, 4U},
    {56U, 40U, 7U}, {32U, 48U, 6U}, {40U, 48U, 5U}, {48U, 48U, 5U},
    {56U, 48U, 6U}, {32U, 56U, 5U}, {40U, 56U, 5U}, {48U, 56U, 5U},
    {56U, 56U, 5U}, {64U, 0U, 6U},  {72U, 0U, 8U},  {88U, 0U, 6U},
    {96U, 0U, 6U},  {64U, 8U, 5U},
}};

constexpr std::array<OriginalHudGlyph, 30> vit_extended_glyphs{{
    {64U, 24U, 6U}, {72U, 24U, 5U}, {80U, 24U, 6U}, {88U, 24U, 5U},
    {96U, 24U, 4U}, {64U, 32U, 5U}, {72U, 32U, 5U}, {80U, 32U, 5U},
    {88U, 32U, 5U}, {96U, 32U, 4U}, {64U, 40U, 4U}, {72U, 40U, 5U},
    {80U, 40U, 5U}, {88U, 40U, 8U}, {96U, 40U, 4U}, {64U, 48U, 5U},
    {72U, 48U, 5U}, {80U, 48U, 5U}, {88U, 48U, 3U}, {96U, 48U, 5U},
    {64U, 56U, 4U}, {72U, 56U, 4U}, {80U, 56U, 5U}, {88U, 56U, 6U},
    {96U, 56U, 7U}, {64U, 64U, 4U}, {72U, 64U, 4U}, {80U, 64U, 4U},
    {88U, 64U, 4U}, {96U, 64U, 5U},
}};

} // namespace

std::optional<OriginalHudGlyph> originalEnglishHudGlyph(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return hud_digit_glyphs[static_cast<std::size_t>(value - '0')];
  }
  if (value >= 'a' && value <= 'z') {
    return hud_lower_glyphs[static_cast<std::size_t>(value - 'a')];
  }
  if (value >= 'A' && value <= 'Z') {
    return hud_upper_glyphs[static_cast<std::size_t>(value - 'A')];
  }
  switch (value) {
  case '!':
    return OriginalHudGlyph{8U, 24U, 1U};
  case '\"':
    return OriginalHudGlyph{24U, 24U, 3U};
  case '\'':
    return OriginalHudGlyph{24U, 24U, 1U};
  case '(':
    return OriginalHudGlyph{88U, 16U, 3U};
  case ')':
    return OriginalHudGlyph{96U, 16U, 3U};
  case ',':
    return OriginalHudGlyph{0U, 32U, 1U};
  case '-':
    return OriginalHudGlyph{4U, 32U, 3U};
  case '.':
    return OriginalHudGlyph{28U, 16U, 1U};
  case '/':
    return OriginalHudGlyph{20U, 16U, 5U};
  case ':':
    return OriginalHudGlyph{16U, 16U, 2U};
  case '?':
    return OriginalHudGlyph{0U, 24U, 4U};
  default:
    return std::nullopt;
  }
}

std::optional<OriginalHudGlyph> originalHudGlyph(char value) noexcept {
  if (!russianLanguageActive()) {
    return originalEnglishHudGlyph(value);
  }
  const auto raw = static_cast<unsigned char>(value);
  if (raw >= 0xdfU && raw <= 0xfcU) {
    return vit_extended_glyphs[raw - 0xdfU];
  }
  if (value >= 'a' && value <= 'z') {
    return vit_lower_glyphs[static_cast<std::size_t>(value - 'a')];
  }
  if (value >= 'A' && value <= 'Z') {
    return vit_upper_glyphs[static_cast<std::size_t>(value - 'A')];
  }
  return originalEnglishHudGlyph(value);
}

int originalHudTextWidth(std::string_view text) noexcept {
  auto width = 0;
  for (const auto character : text) {
    if (character == ' ') {
      width += 4;
    } else if (const auto glyph = originalHudGlyph(character)) {
      width += glyph->advance();
    }
  }
  return width == 0 ? 0 : width - 2;
}

std::string originalAmmoText(const WeaponDefinition &definition,
                             const WeaponState &weapon) {
  if (!definition.shows_ammo) {
    return {};
  }
  const auto append_two_digits = [](std::string &output, unsigned int value) {
    output.push_back(static_cast<char>('0' + (value / 10U) % 10U));
    output.push_back(static_cast<char>('0' + value % 10U));
  };
  std::string result;
  result.reserve(6U);
  append_two_digits(result, std::min<unsigned int>(weapon.magazine, 99U));
  if (weapon.reserve != 0U) {
    result.push_back('/');
    auto reserve = std::min<unsigned int>(weapon.reserve, 999U);
    if (reserve > 99U) {
      result.push_back(static_cast<char>('0' + reserve / 100U));
      reserve %= 100U;
    }
    append_two_digits(result, reserve);
  }
  return result;
}

std::array<int, maximum_weapon_icon_layers>
originalWeaponIconOffsets(std::span<const int> layer_widths) noexcept {
  std::array<int, maximum_weapon_icon_layers> centers{};
  std::array<int, maximum_weapon_icon_layers> offsets{};
  const auto count = std::min(layer_widths.size(), centers.size());
  if (count == 0U) {
    return offsets;
  }

  if (count == 2U) {
    centers[0] = -layer_widths[0] / 2;
    centers[1] = layer_widths[1] / 2;
  } else if (count == 3U) {
    centers[0] = -layer_widths[0] / 2 - layer_widths[1] / 2;
    centers[1] = 0;
    centers[2] = layer_widths[1] / 2 + layer_widths[2] / 2;
  }

  auto center_sum = 0;
  for (std::size_t index = 0U; index < count; ++index) {
    center_sum += centers[index];
  }
  const auto center_mean = center_sum / static_cast<int>(count);
  for (std::size_t index = 0U; index < count; ++index) {
    offsets[index] = centers[index] - center_mean - layer_widths[index] / 2;
  }
  return offsets;
}

OriginalAimReticleGeometry
originalAimReticleGeometry(bool head_target) noexcept {
  // Recovered at native resolution from FUN_80041830 and the retail output.
  // A body lock follows the full projected torso, while HEAD SHOT contracts
  // the same four-ray box to the head volume.
  return head_target
      ? OriginalAimReticleGeometry{10, 7, 10, 7}
      : OriginalAimReticleGeometry{17, 8, 17, 9};
}

OriginalHeadshotCalloutGeometry
originalHeadshotCalloutGeometry() noexcept {
  return OriginalHeadshotCalloutGeometry{
      0, -14,
      9, -20,
      16, -20,
      8, -28,
  };
}

OriginalWeaponMenuGeometry
originalWeaponMenuGeometry() noexcept {
  // Native POLY_F4/LINE_F2 packets from FUN_800405f4. The command word
  // 0x28503028 encodes RGB 40,48,80; both quads use average blending.
  return OriginalWeaponMenuGeometry{
      -200, -90, 200, -69,
      -49, -93, 49, -66,
      -200, 200, -92, -68,
      HudRgb{40U, 48U, 80U},
      HudRgb{128U, 128U, 128U},
  };
}

OriginalRadarGeometry
originalRadarGeometry(std::uint8_t reveal_frame) noexcept {
  constexpr auto maximum_frame = std::uint8_t{12U};
  const auto frame = static_cast<int>(std::min(reveal_frame, maximum_frame));
  const auto inner_half_width = ((frame * 18) / 12) * frame / 12;
  const auto inner_half_height = ((frame * 15) / 12) * frame / 12;
  const auto reticle_half_height = std::max(0, inner_half_height - 7);
  return OriginalRadarGeometry{
      static_cast<std::uint8_t>(frame),
      frame * 2,
      frame * 20 / 12,
      inner_half_width,
      inner_half_height,
      reticle_half_height == 0 ? 0 : 9,
      reticle_half_height,
  };
}

const WeaponDefinition *tryWeaponDefinition(WeaponId id) noexcept {
  if (!isValidWeaponId(id)) {
    return nullptr;
  }
  return &weapon_definitions[indexOf(id)];
}

const WeaponDefinition &weaponDefinition(WeaponId id) {
  const auto *definition = tryWeaponDefinition(id);
  if (definition == nullptr) {
    throw std::out_of_range{"WeaponId is outside the original 26-slot table"};
  }
  return *definition;
}

std::span<const std::string_view>
droppedItemIconLayers(std::uint16_t item) noexcept {
  if (item == 0x80U) {
    return armor_pickup_layers;
  }
  if (item >= weapon_slot_count) {
    return {};
  }
  switch (static_cast<WeaponId>(item)) {
  case WeaponId::pistol_9mm:
    return pistol_9mm_pickup_layers;
  case WeaponId::unused_357:
    return unused_357_pickup_layers;
  case WeaponId::flamethrower:
    return flamethrower_pickup_layers;
  default:
    return weapon_definitions[item].icon.layers();
  }
}

PlayerInventory::PlayerInventory() { resetFirstMission(); }

void PlayerInventory::resetFirstMission() noexcept {
  states_.fill({});
  states_[indexOf(WeaponId::unarmed)].owned = true;
  grant(WeaponId::silenced_9mm, 15U, 45U);
  grant(WeaponId::taser, 1U, 0U);
  grant(WeaponId::flashlight, 0U, 0U);
  current_ = WeaponId::silenced_9mm;
}

void PlayerInventory::grant(WeaponId id, std::uint16_t magazine,
                            std::uint16_t reserve) noexcept {
  const auto *definition = tryWeaponDefinition(id);
  if (definition == nullptr) {
    return;
  }
  auto &item = states_[indexOf(id)];
  item.owned = true;
  if (!definition->uses_ammo) {
    item.magazine = definition->magazine_capacity == 0U
                        ? 0U
                        : std::min(magazine, definition->magazine_capacity);
    item.reserve = 0U;
    return;
  }
  item.magazine = std::min(magazine, definition->magazine_capacity);
  const auto maximum_reserve =
      static_cast<std::uint32_t>(definition->magazine_capacity) *
      definition->reserve_magazines;
  item.reserve = static_cast<std::uint16_t>(
      std::min<std::uint32_t>(reserve, maximum_reserve));
}

void PlayerInventory::remove(WeaponId id) noexcept {
  if (!isValidWeaponId(id) || id == WeaponId::unarmed) {
    return;
  }
  states_[indexOf(id)] = {};
  if (current_ == id) {
    current_ = WeaponId::unarmed;
    static_cast<void>(selectNext());
  }
}

bool PlayerInventory::select(WeaponId id) noexcept {
  if (!isValidWeaponId(id) || !states_[indexOf(id)].owned || current_ == id) {
    return false;
  }
  current_ = id;
  return true;
}

WeaponId PlayerInventory::linkedAvailable(
    WeaponId from,
    const std::array<std::uint8_t, weapon_slot_count> &links) const noexcept {
  if (!isValidWeaponId(from)) {
    from = WeaponId::unarmed;
  }
  auto candidate = from;
  for (std::size_t checked = 0; checked < weapon_slot_count; ++checked) {
    const auto linked_index = links[indexOf(candidate)];
    if (linked_index >= weapon_slot_count) {
      return from;
    }
    candidate = static_cast<WeaponId>(linked_index);
    if (candidate == from) {
      return from;
    }
    if (states_[indexOf(candidate)].owned) {
      return candidate;
    }
  }
  return from;
}

bool PlayerInventory::cycle(
    const std::array<std::uint8_t, weapon_slot_count> &links) noexcept {
  if (!isValidWeaponId(current_)) {
    current_ = WeaponId::unarmed;
  }
  const auto candidate = linkedAvailable(current_, links);
  if (candidate == current_) {
    return false;
  }
  current_ = candidate;
  return true;
}

bool PlayerInventory::selectNext() noexcept { return cycle(next_weapon); }

bool PlayerInventory::selectPrevious() noexcept {
  return cycle(previous_weapon);
}

WeaponId PlayerInventory::nextAvailable(WeaponId from) const noexcept {
  return linkedAvailable(from, next_weapon);
}

WeaponId PlayerInventory::previousAvailable(WeaponId from) const noexcept {
  return linkedAvailable(from, previous_weapon);
}

std::array<WeaponId, weapon_menu_slot_count>
PlayerInventory::weaponMenuWindow() const noexcept {
  std::array<WeaponId, weapon_menu_slot_count> result{};
  auto weapon = current_;
  for (std::size_t step = 0; step < weapon_menu_slot_count / 2U; ++step) {
    weapon = nextAvailable(weapon);
  }
  for (auto &slot : result) {
    slot = weapon;
    weapon = previousAvailable(weapon);
  }
  return result;
}

std::optional<WeaponId>
PlayerInventory::quickSlot(std::size_t slot) const noexcept {
  // PC quick slots enumerate usable inventory in the same native slot order
  // as the pause menu.  Unarmed and mission-only key items are deliberately
  // excluded; their lack of an icon also keeps this mapping deterministic.
  std::size_t owned_index = 0U;
  for (std::size_t index = 1U; index < weapon_slot_count; ++index) {
    const auto id = static_cast<WeaponId>(index);
    const auto& definition = weapon_definitions[index];
    if (!states_[index].owned || definition.icon.layerCount() == 0U) {
      continue;
    }
    if (owned_index++ == slot) {
      return id;
    }
  }
  return std::nullopt;
}

bool PlayerInventory::consumeRound() noexcept {
  const auto &definition = currentDefinition();
  if (!definition.uses_ammo) {
    return true;
  }
  auto &item = states_[indexOf(current_)];
  if (item.magazine == 0U) {
    return false;
  }
  --item.magazine;
  return true;
}

std::uint16_t PlayerInventory::reload() noexcept {
  const auto &definition = currentDefinition();
  if (!definition.uses_ammo || definition.magazine_capacity == 0U) {
    return 0U;
  }
  auto &item = states_[indexOf(current_)];
  const auto missing =
      static_cast<std::uint16_t>(definition.magazine_capacity - item.magazine);
  const auto transferred = std::min(missing, item.reserve);
  item.magazine = static_cast<std::uint16_t>(item.magazine + transferred);
  item.reserve = static_cast<std::uint16_t>(item.reserve - transferred);
  return transferred;
}

const WeaponState *PlayerInventory::tryState(WeaponId id) const noexcept {
  if (!isValidWeaponId(id)) {
    return nullptr;
  }
  return &states_[indexOf(id)];
}

const WeaponState &PlayerInventory::state(WeaponId id) const {
  const auto *item = tryState(id);
  if (item == nullptr) {
    throw std::out_of_range{"WeaponId is outside the inventory table"};
  }
  return *item;
}

GameplayHud::GameplayHud() { reset(); }

void GameplayHud::reset() noexcept {
  inventory_.resetFirstMission();
  vitals_ = {};
  danger_ = 0U;
  danger_critical_ = false;
  target_health_.reset();
  aiming_ = false;
  displayed_primary_bar_ = bar_maximum;
  displayed_primary_trail_ = bar_maximum;
  displayed_danger_bar_ = 0U;
  displayed_target_bar_ = 0U;
  primary_reveal_ = 0U;
  danger_reveal_ = 0U;
  target_reveal_ = 0U;
  reveal_frame_ = 0U;
  weapon_switch_frames_ = 0U;
  weapon_menu_frames_ = 0U;
  tick_ = 0U;
}

void GameplayHud::update(const HudInput &input) noexcept {
  ++tick_;
  if (reveal_frame_ < reveal_duration) {
    ++reveal_frame_;
  }
  if (weapon_switch_frames_ > 0U) {
    --weapon_switch_frames_;
  }
  if (weapon_menu_frames_ > 0U) {
    --weapon_menu_frames_;
  }
  aiming_ = input.aiming;
  auto changed = false;
  if (input.weapon_menu_delta != 0) {
    weapon_menu_frames_ = weapon_menu_duration;
    const auto signed_steps = static_cast<std::int64_t>(input.weapon_menu_delta);
    const auto magnitude = signed_steps < 0 ? -signed_steps : signed_steps;
    const auto steps = std::min<std::size_t>(
        weapon_slot_count, static_cast<std::size_t>(magnitude));
    for (std::size_t step = 0; step < steps; ++step) {
      changed = (signed_steps > 0 ? inventory_.selectNext()
                                  : inventory_.selectPrevious()) || changed;
    }
  } else if (input.next_weapon != input.previous_weapon) {
    changed = input.next_weapon ? inventory_.selectNext()
                                : inventory_.selectPrevious();
  }
  if (changed) {
    weapon_switch_frames_ = weapon_switch_duration;
  }
  // FUN_8003db64 always draws the red health endpoint underneath the blue
  // armor endpoint.  While armor is longer it hides health completely; once
  // armor is depleted the red endpoint remains and becomes the visible bar.
  displayed_primary_bar_ = approachBar(displayed_primary_bar_, armorBar());
  displayed_primary_trail_ = approachBar(
      displayed_primary_trail_,
      std::max(displayed_primary_bar_, healthBar()));
  displayed_danger_bar_ = approachBar(displayed_danger_bar_, dangerBar());
  displayed_target_bar_ =
      approachBar(displayed_target_bar_, targetBar().value_or(0U));
  primary_reveal_ = approachReveal(primary_reveal_, true);
  danger_reveal_ = approachReveal(danger_reveal_, dangerBar() != 0U);
  target_reveal_ = approachReveal(target_reveal_, targetBar().has_value());
}

void GameplayHud::showWeaponMenu() noexcept {
  weapon_menu_frames_ = weapon_menu_duration;
}

void GameplayHud::notifyWeaponChanged() noexcept {
  weapon_switch_frames_ = weapon_switch_duration;
}

bool GameplayHud::selectWeapon(WeaponId id) noexcept {
  if (!inventory_.select(id)) {
    return false;
  }
  weapon_switch_frames_ = weapon_switch_duration;
  return true;
}

void GameplayHud::setVitals(PlayerVitals vitals) noexcept {
  vitals.maximum_health = std::max<std::uint16_t>(vitals.maximum_health, 1U);
  vitals.maximum_armor = std::max<std::uint16_t>(vitals.maximum_armor, 1U);
  vitals.health = std::min(vitals.health, vitals.maximum_health);
  vitals.armor = std::min(vitals.armor, vitals.maximum_armor);
  vitals_ = vitals;
}

void GameplayHud::setDanger(std::uint8_t danger, bool critical) noexcept {
  danger_ = std::min(danger, static_cast<std::uint8_t>(100U));
  danger_critical_ = critical && danger_ == 100U;
  // danger_q12 is already the retail actor-controller interpolation. Running
  // it through the generic native status-bar easing a second time delayed
  // both acquisition and decay by up to ten more guest ticks.
  displayed_danger_bar_ = dangerBar();
}

void GameplayHud::setTargetHealth(std::optional<std::uint8_t> health) noexcept {
  if (health) {
    *health = std::min(*health, static_cast<std::uint8_t>(100U));
  }
  target_health_ = health;
}

PrimaryStatus GameplayHud::primaryStatus() const noexcept {
  return vitals_.armor == 0U ? PrimaryStatus::health : PrimaryStatus::armor;
}

std::uint8_t GameplayHud::scaleBar(std::uint16_t value,
                                   std::uint16_t maximum) noexcept {
  if (maximum == 0U) {
    return 0U;
  }
  const auto scaled = static_cast<std::uint32_t>(value) * bar_maximum / maximum;
  return static_cast<std::uint8_t>(
      std::min<std::uint32_t>(scaled, bar_maximum));
}

std::uint8_t GameplayHud::approachBar(std::uint8_t displayed,
                                      std::uint8_t target) noexcept {
  if (displayed < target) {
    return static_cast<std::uint8_t>(
        std::min<unsigned int>(displayed + bar_animation_step, target));
  }
  if (displayed > target) {
    return static_cast<std::uint8_t>(
        std::max<int>(static_cast<int>(displayed) - bar_animation_step, target));
  }
  return displayed;
}

std::uint8_t GameplayHud::approachReveal(std::uint8_t displayed,
                                         bool visible) noexcept {
  const auto target = visible ? bar_reveal_maximum : std::uint8_t{};
  if (displayed < target) {
    return static_cast<std::uint8_t>(
        std::min<unsigned int>(displayed + bar_reveal_step, target));
  }
  if (displayed > target) {
    return static_cast<std::uint8_t>(
        std::max<int>(static_cast<int>(displayed) - bar_reveal_step, target));
  }
  return displayed;
}

std::uint8_t GameplayHud::primaryBar() const noexcept {
  return primaryStatus() == PrimaryStatus::armor
             ? armorBar()
             : healthBar();
}

std::uint8_t GameplayHud::armorBar() const noexcept {
  return scaleBar(vitals_.armor, vitals_.maximum_armor);
}

std::uint8_t GameplayHud::healthBar() const noexcept {
  return scaleBar(vitals_.health, vitals_.maximum_health);
}

HudRgb GameplayHud::healthBarColor() const noexcept {
  if (vitals_.armor != 0U) {
    return {255U, 100U, 100U};
  }

  // Exact 16-step output of the retail rcos-based critical-health pulse.
  constexpr std::array<std::uint8_t, 16> pulse{
      255U, 245U, 217U, 175U, 127U, 78U, 36U, 8U,
      0U,   8U,   36U,  78U,  127U, 175U, 217U, 245U,
  };
  return {pulse[static_cast<std::size_t>(tick_ & 15U)], 0U, 0U};
}

std::uint8_t GameplayHud::dangerBar() const noexcept {
  return scaleBar(danger_, 100U);
}

std::optional<std::uint8_t> GameplayHud::targetBar() const noexcept {
  if (!target_health_) {
    return std::nullopt;
  }
  return scaleBar(*target_health_, 100U);
}

} // namespace sf::game
