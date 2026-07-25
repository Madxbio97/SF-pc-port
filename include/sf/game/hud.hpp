#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sf::game {

enum class WeaponId : std::uint8_t {
  unarmed = 0,
  silenced_9mm = 1,
  pistol_9mm = 2,
  glock_17 = pistol_9mm,
  unused_357 = 3,
  unused_03 = unused_357,
  pistol_45 = 4,
  pistol_357 = pistol_45,
  g_18 = 5,
  combat_shotgun = 6,
  shotgun = 7,
  pk_102 = 8,
  m_16 = 9,
  biz_2 = 10,
  hk_5 = 11,
  nightvision_rifle = 12,
  sniper_rifle = 13,
  taser = 14,
  flamethrower = 15,
  m_79 = 16,
  k3g4 = 17,
  virus_scanner = 18,
  fragmentation_grenade = 19,
  gas_grenade = 20,
  flashlight = 21,
  chopper_gun = 22,
  key_card = 23,
  c4_explosives = 24,
  viral_antigen = 25,
};

inline constexpr std::size_t weapon_slot_count = 26U;
inline constexpr std::size_t maximum_weapon_icon_layers = 3U;
inline constexpr std::size_t weapon_menu_slot_count = 7U;

[[nodiscard]] constexpr bool isValidWeaponId(WeaponId id) noexcept {
  return static_cast<std::size_t>(id) < weapon_slot_count;
}

struct WeaponIconDefinition {
  // All populated assets are drawn together, horizontally from left to right.
  // The A/B/C suffixes identify layers, not animation frames.
  std::array<std::string_view, maximum_weapon_icon_layers> assets{};

  [[nodiscard]] constexpr std::size_t layerCount() const noexcept {
    std::size_t count = 0U;
    while (count < assets.size() && !assets[count].empty()) {
      ++count;
    }
    return count;
  }

  [[nodiscard]] constexpr std::span<const std::string_view>
  layers() const noexcept {
    return {assets.data(), layerCount()};
  }
};

struct WeaponDefinition {
  WeaponId id{};
  std::string_view name;
  WeaponIconDefinition icon;
  std::uint16_t magazine_capacity{};
  std::uint8_t reserve_magazines{};
  bool uses_ammo{};
  bool shows_ammo{};
};

struct WeaponState {
  bool owned{};
  std::uint16_t magazine{};
  std::uint16_t reserve{};
};

// Exact SCUS_942.40 metadata for the original 8-pixel gameplay font.  The
// three FONTA/B/C TIMs occupy one shared 8-bpp texture page, so the UVs are
// page-relative rather than relative to an individual TIM rectangle.
struct OriginalHudGlyph {
  std::uint8_t u{};
  std::uint8_t v{};
  std::uint8_t width{};

  [[nodiscard]] constexpr std::uint8_t height() const noexcept { return 8U; }
  [[nodiscard]] constexpr int advance() const noexcept {
    return static_cast<int>(width) + 2;
  }

  friend constexpr bool operator==(const OriginalHudGlyph &,
                                   const OriginalHudGlyph &) = default;
};

[[nodiscard]] std::optional<OriginalHudGlyph>
originalHudGlyph(char value) noexcept;
[[nodiscard]] std::optional<OriginalHudGlyph>
originalEnglishHudGlyph(char value) noexcept;
[[nodiscard]] int originalHudTextWidth(std::string_view text) noexcept;
[[nodiscard]] std::string originalAmmoText(const WeaponDefinition &definition,
                                           const WeaponState &weapon);
// FUN_80039b44 stores each weapon layer by its centre.  Convert those native
// centres to renderer-ready top-left offsets while preserving the group's
// arithmetic-mean anchor used by FUN_80039778/FUN_800398a8.
[[nodiscard]] std::array<int, maximum_weapon_icon_layers>
originalWeaponIconOffsets(std::span<const int> layer_widths) noexcept;

struct OriginalAimReticleGeometry {
  int half_width{};
  int half_height{};
  int horizontal_ray{};
  int vertical_ray{};

  friend constexpr bool operator==(const OriginalAimReticleGeometry &,
                                   const OriginalAimReticleGeometry &) = default;
};

// FUN_80041830 builds the target box and its four centre rays from the
// projected body/head bounds.  These are the native 384x240 retail extents.
[[nodiscard]] OriginalAimReticleGeometry
originalAimReticleGeometry(bool head_target) noexcept;

struct OriginalHeadshotCalloutGeometry {
  int start_x{};
  int start_y{};
  int elbow_x{};
  int elbow_y{};
  int end_x{};
  int end_y{};
  int text_x{};
  int text_y{};

  friend constexpr bool operator==(const OriginalHeadshotCalloutGeometry &,
                                   const OriginalHeadshotCalloutGeometry &) = default;
};

// The retail Head Shot notification is connected to the upper target ray by
// a white two-segment callout; all coordinates are relative to the box centre.
[[nodiscard]] OriginalHeadshotCalloutGeometry
originalHeadshotCalloutGeometry() noexcept;

struct OriginalRadarGeometry {
  std::uint8_t frame{};
  int outer_half_width{};
  int outer_half_height{};
  int inner_half_width{};
  int inner_half_height{};
  int reticle_half_width{};
  int reticle_half_height{};

  friend constexpr bool operator==(const OriginalRadarGeometry &,
                                   const OriginalRadarGeometry &) = default;
};

// FUN_800410d0 grows both radar panels non-linearly over twelve frames.  Keep
// that arithmetic outside the renderer so startup and reset use one path.
[[nodiscard]] OriginalRadarGeometry
originalRadarGeometry(std::uint8_t reveal_frame) noexcept;

[[nodiscard]] const WeaponDefinition *
tryWeaponDefinition(WeaponId id) noexcept;
[[nodiscard]] const WeaponDefinition &weaponDefinition(WeaponId id);
// Floor pickups normally reuse the active HUD icon group. A handful of
// retail item slots deliberately select empty group zero even though their
// authored INTRFACE sprites are present; expose those pickup-only layers
// without changing the authoritative inventory HUD table.
[[nodiscard]] std::span<const std::string_view>
droppedItemIconLayers(std::uint16_t item) noexcept;

class PlayerInventory final {
public:
  PlayerInventory();

  void resetFirstMission() noexcept;
  void grant(WeaponId id, std::uint16_t magazine,
             std::uint16_t reserve) noexcept;
  void remove(WeaponId id) noexcept;
  [[nodiscard]] bool select(WeaponId id) noexcept;
  [[nodiscard]] bool selectNext() noexcept;
  [[nodiscard]] bool selectPrevious() noexcept;
  [[nodiscard]] WeaponId nextAvailable(WeaponId from) const noexcept;
  [[nodiscard]] WeaponId previousAvailable(WeaponId from) const noexcept;
  [[nodiscard]] std::array<WeaponId, weapon_menu_slot_count>
  weaponMenuWindow() const noexcept;
  [[nodiscard]] std::optional<WeaponId>
  quickSlot(std::size_t slot) const noexcept;
  [[nodiscard]] bool consumeRound() noexcept;
  [[nodiscard]] std::uint16_t reload() noexcept;

  [[nodiscard]] WeaponId current() const noexcept { return current_; }
  [[nodiscard]] const WeaponState *tryState(WeaponId id) const noexcept;
  [[nodiscard]] const WeaponState &state(WeaponId id) const;
  [[nodiscard]] const WeaponState &currentState() const noexcept {
    return states_[static_cast<std::size_t>(current_)];
  }
  [[nodiscard]] const WeaponDefinition &currentDefinition() const {
    return weaponDefinition(current_);
  }

private:
  [[nodiscard]] bool
  cycle(const std::array<std::uint8_t, weapon_slot_count> &links) noexcept;
  [[nodiscard]] WeaponId linkedAvailable(
      WeaponId from,
      const std::array<std::uint8_t, weapon_slot_count> &links) const noexcept;

  std::array<WeaponState, weapon_slot_count> states_{};
  WeaponId current_{WeaponId::unarmed};
};

struct PlayerVitals {
  std::uint16_t health{150U};
  std::uint16_t maximum_health{150U};
  std::uint16_t armor{600U};
  std::uint16_t maximum_armor{600U};
};

enum class PrimaryStatus : std::uint8_t {
  health,
  armor,
};

struct HudRgb {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};

  friend constexpr bool operator==(const HudRgb &, const HudRgb &) = default;
};

struct OriginalWeaponMenuGeometry {
  int strip_left{};
  int strip_top{};
  int strip_right{};
  int strip_bottom{};
  int selection_left{};
  int selection_top{};
  int selection_right{};
  int selection_bottom{};
  int frame_left{};
  int frame_right{};
  int frame_top{};
  int frame_bottom{};
  HudRgb background_color{};
  HudRgb frame_color{};

  friend constexpr bool operator==(const OriginalWeaponMenuGeometry &,
                                   const OriginalWeaponMenuGeometry &) = default;
};

// FUN_800405f4 creates the long-switch backing from two overlapping
// semi-transparent quads and two horizontal lines. Coordinates are relative
// to the retail HUD's 192x120 centre.
[[nodiscard]] OriginalWeaponMenuGeometry
originalWeaponMenuGeometry() noexcept;

[[nodiscard]] constexpr std::string_view
originalPrimaryStatusLabel(PrimaryStatus status) noexcept {
  return status == PrimaryStatus::armor ? "ARMOR" : "HEALTH";
}

struct HudInput {
  bool aiming{};
  bool next_weapon{};
  bool previous_weapon{};
  std::int32_t weapon_menu_delta{};
};

class GameplayHud final {
public:
  static constexpr std::uint8_t bar_maximum = 50U;
  static constexpr std::uint8_t bar_animation_step = 5U;
  static constexpr std::uint8_t bar_reveal_maximum = 53U;
  static constexpr std::uint8_t bar_reveal_step = 8U;
  static constexpr std::uint8_t reveal_duration = 12U;
  static constexpr std::uint8_t weapon_switch_duration = 18U;
  static constexpr std::uint8_t weapon_menu_duration = weapon_switch_duration;

  GameplayHud();

  void reset() noexcept;
  void update(const HudInput &input) noexcept;
  // Guest gameplay owns inventory selection. These presentation-only hooks
  // replay the native HUD timing without cycling that authoritative inventory
  // a second time.
  void showWeaponMenu() noexcept;
  void notifyWeaponChanged() noexcept;
  [[nodiscard]] bool selectWeapon(WeaponId id) noexcept;
  void setVitals(PlayerVitals vitals) noexcept;
  void setDanger(std::uint8_t danger, bool critical = false) noexcept;
  void setTargetHealth(std::optional<std::uint8_t> health) noexcept;

  [[nodiscard]] const PlayerInventory &inventory() const noexcept {
    return inventory_;
  }
  [[nodiscard]] PlayerInventory &inventory() noexcept { return inventory_; }
  [[nodiscard]] const PlayerVitals &vitals() const noexcept { return vitals_; }
  [[nodiscard]] PrimaryStatus primaryStatus() const noexcept;
  [[nodiscard]] std::uint8_t armorBar() const noexcept;
  [[nodiscard]] std::uint8_t healthBar() const noexcept;
  [[nodiscard]] HudRgb healthBarColor() const noexcept;
  [[nodiscard]] std::uint8_t primaryBar() const noexcept;
  [[nodiscard]] std::uint8_t dangerBar() const noexcept;
  [[nodiscard]] bool dangerCritical() const noexcept { return danger_critical_; }
  [[nodiscard]] std::optional<std::uint8_t> targetBar() const noexcept;
  [[nodiscard]] std::uint8_t displayedPrimaryBar() const noexcept {
    return displayed_primary_bar_;
  }
  [[nodiscard]] std::uint8_t displayedPrimaryTrail() const noexcept {
    return displayed_primary_trail_;
  }
  [[nodiscard]] std::uint8_t displayedDangerBar() const noexcept {
    return displayed_danger_bar_;
  }
  [[nodiscard]] std::uint8_t displayedTargetBar() const noexcept {
    return displayed_target_bar_;
  }
  [[nodiscard]] std::uint8_t primaryReveal() const noexcept {
    return primary_reveal_;
  }
  [[nodiscard]] std::uint8_t dangerReveal() const noexcept {
    return danger_reveal_;
  }
  [[nodiscard]] std::uint8_t targetReveal() const noexcept {
    return target_reveal_;
  }
  [[nodiscard]] std::uint8_t revealFrame() const noexcept {
    return reveal_frame_;
  }
  [[nodiscard]] bool aiming() const noexcept { return aiming_; }
  [[nodiscard]] std::uint8_t weaponSwitchFrames() const noexcept {
    return weapon_switch_frames_;
  }
  [[nodiscard]] std::uint8_t weaponMenuFrames() const noexcept {
    return weapon_menu_frames_;
  }
  [[nodiscard]] std::array<WeaponId, weapon_menu_slot_count>
  weaponMenuWindow() const noexcept {
    return inventory_.weaponMenuWindow();
  }
  [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }

private:
  [[nodiscard]] static std::uint8_t scaleBar(std::uint16_t value,
                                             std::uint16_t maximum) noexcept;
  [[nodiscard]] static std::uint8_t approachBar(std::uint8_t displayed,
                                                 std::uint8_t target) noexcept;
  [[nodiscard]] static std::uint8_t approachReveal(std::uint8_t displayed,
                                                    bool visible) noexcept;

  PlayerInventory inventory_;
  PlayerVitals vitals_{};
  std::uint8_t danger_{};
  bool danger_critical_{};
  std::optional<std::uint8_t> target_health_;
  bool aiming_{};
  std::uint8_t displayed_primary_bar_{bar_maximum};
  std::uint8_t displayed_primary_trail_{bar_maximum};
  std::uint8_t displayed_danger_bar_{};
  std::uint8_t displayed_target_bar_{};
  std::uint8_t primary_reveal_{};
  std::uint8_t danger_reveal_{};
  std::uint8_t target_reveal_{};
  std::uint8_t reveal_frame_{};
  std::uint8_t weapon_switch_frames_{};
  std::uint8_t weapon_menu_frames_{};
  std::uint64_t tick_{};
};

} // namespace sf::game
