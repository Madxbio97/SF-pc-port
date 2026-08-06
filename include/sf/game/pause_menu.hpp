#pragma once

#include "sf/game/controller_bindings.hpp"
#include "sf/game/retail_cheats.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::game {

enum class PauseScreen {
  root,
  briefing,
  options,
  cheats,
  sound,
  controller,
  controller_bindings,
  brightness,
  screen_centering,
  mission_select,
  weapons,
  parameters,
  objectives,
  map,
  confirmation,
  notification,
};

enum class MissionEntryState {
  active,
  completed,
  failed,
};

struct MissionMenuEntry {
  std::uint32_t id{};
  std::string text;
  MissionEntryState state{MissionEntryState::active};
  bool visible{true};
};

// Guest tables are stored in bit order while MENU.OVL presents them in the
// reverse authored order. A single "." entry is the retail silent script
// sentinel and must never become a native menu row.
[[nodiscard]] std::vector<MissionMenuEntry> makeRetailMissionMenuEntries(
    std::span<const std::string> exact_texts, std::uint32_t entry_count,
    std::uint32_t visible_mask, std::uint32_t completed_mask = 0U,
    std::uint32_t failed_mask = 0U);

enum class MapMarkerKind {
  player,
  objective,
  friendly,
  hostile,
};

inline constexpr std::uint32_t all_pause_map_layers = 0xffffffffU;

struct PauseMapMarker {
  MapMarkerKind kind{MapMarkerKind::player};
  float x{};
  float y{};
  float heading{};
  bool visible{true};
  std::uint32_t objective_id{};
  std::string label;
  std::uint32_t layer{all_pause_map_layers};
};

struct PauseMapData {
  std::vector<std::string> layer_assets;
  std::vector<PauseMapMarker> markers;
  std::string current_location;
  std::size_t current_layer{};
  bool reconnaissance_available{true};
};

struct PauseMissionData {
  std::string mission_name;
  std::string date_time;
  std::string location;
  std::string area;
  std::vector<std::string> briefing_pages;
  std::vector<MissionMenuEntry> objectives;
  std::vector<MissionMenuEntry> parameters;
  PauseMapData map;
};

struct PauseWeaponData {
  std::uint32_t id{};
  std::string name;
  std::string icon_asset;
  // Canonical source text; the platform renderer applies the active locale.
  std::string description;
  std::int32_t ammo{};
  std::int32_t maximum_ammo{};
  std::uint8_t fire_rate{};
  std::uint8_t damage{};
  std::uint8_t accuracy{};
  bool available{};
  bool equipped{};
  bool equip_allowed{true};
  // Keep the authored WEAPDESC values: the retail details page presents
  // these fields verbatim instead of deriving them from the HUD counter.
  std::string clip_size;
  std::string maximum_rounds;
};

enum class ControllerPreset {
  standard,
  alternate,
  custom,
};

struct PauseSettings {
  std::uint8_t voice_volume{100};
  std::uint8_t music_volume{100};
  std::uint8_t sound_effects_volume{100};
  std::uint8_t brightness{50};
  std::int16_t screen_center_x{};
  std::int16_t screen_center_y{};
  ControllerPreset controller_preset{ControllerPreset::standard};
  bool invert_aim{};
  bool vibration{true};
  ControllerButtonBindings bindings;

  friend bool operator==(const PauseSettings &,
                         const PauseSettings &) = default;
};

struct PauseMenuData {
  PauseMissionData mission;
  std::vector<PauseWeaponData> weapons;
  std::vector<MissionMenuEntry> missions;
  std::uint32_t current_mission{};
  std::uint32_t maximum_unlocked_mission{};
  RetailCheatState cheats;
};

struct PauseMenuInput {
  bool previous{};
  bool next{};
  bool left{};
  bool right{};
  bool confirm{};
  bool cancel{};
  bool pause{};
};

enum class PauseSetting : std::uint32_t {
  voice_volume,
  music_volume,
  sound_effects_volume,
  brightness,
  screen_center_x,
  screen_center_y,
  controller_preset,
  invert_aim,
  vibration,
  bindings,
};

enum class PauseCommandType {
  none,
  resume,
  equip_weapon,
  preview_setting,
  commit_settings,
  revert_settings,
  begin_controller_binding,
  restart_checkpoint,
  restart_mission,
  select_mission,
  quit_game,
  set_retail_cheat,
};

struct PauseMenuCommand {
  PauseCommandType type{PauseCommandType::none};
  std::uint32_t subject{};
  std::int32_t value{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return type != PauseCommandType::none;
  }
};

struct PauseRect {
  std::int16_t x{};
  std::int16_t y{};
  std::int16_t width{};
  std::int16_t height{};
};

// Retail MENU.OVL works in a fixed 384x240 coordinate space. The text-window
// rectangles below are recovered from its window initialisation routine; the
// grid rectangles describe the corresponding visible ACD panels.
struct PauseAcdLayout {
  static constexpr PauseRect canvas{0, 0, 384, 240};
  static constexpr PauseRect left_grid{36, 25, 190, 184};
  static constexpr PauseRect left_content{52, 32, 165, 155};
  static constexpr PauseRect information_grid{233, 35, 116, 81};
  static constexpr PauseRect information_content{236, 40, 109, 70};
  static constexpr PauseRect section_menu{252, 134, 100, 100};
  static constexpr PauseRect hint{52, 200, 165, 10};
  // MENU.OVL draws every MAPn.TIM at its native size around (135,115).
  // The largest retail page is 118x174; aspect fitting preserves the exact
  // per-page dimensions and common centre used by authored marker offsets.
  static constexpr PauseRect map_image{76, 28, 118, 174};
  static constexpr PauseRect expanded_content{12, 8, 360, 210};
  static constexpr PauseRect expanded_map_panel{8, 8, 256, 210};
  static constexpr PauseRect expanded_map_image{14, 14, 244, 198};
  static constexpr PauseRect expanded_information_panel{268, 8, 108, 210};
  static constexpr PauseRect expanded_information_content{274, 16, 96, 194};
  static constexpr PauseRect expanded_weapon_image_panel{18, 34, 168, 170};
  static constexpr PauseRect expanded_weapon_information_panel{192, 34, 174,
                                                               170};
  static constexpr PauseRect expanded_hint{20, 224, 344, 10};

  [[nodiscard]] static constexpr PauseRect
  sectionSelection(std::size_t index) noexcept {
    return PauseRect{
        section_menu.x,
        static_cast<std::int16_t>(132 + index * 16),
        section_menu.width,
        11,
    };
  }
};

enum class PausePanelRole : std::uint8_t {
  none,
  left_content,
  right_information,
  right_sections,
  hint,
};

enum class PauseTextAlignment : std::uint8_t {
  left,
  center,
  right,
};

enum class PauseTransitionKind : std::uint8_t {
  none,
  section_selection,
  item_selection,
  screen_change,
};

struct PauseTransitionState {
  PauseTransitionKind kind{PauseTransitionKind::none};
  PauseScreen from_screen{PauseScreen::root};
  PauseScreen to_screen{PauseScreen::root};
  std::size_t from_selection{};
  std::size_t to_selection{};
  std::uint8_t frame{};
  std::uint8_t duration{};
  std::uint8_t input_delay{};

  [[nodiscard]] constexpr bool active() const noexcept {
    return kind != PauseTransitionKind::none && frame < duration;
  }
};

enum class PauseRenderKind {
  dim_background,
  panel,
  title,
  text,
  menu_item,
  selection,
  divider,
  slider,
  asset,
  weapon_icon,
  map_marker,
  button_hint,
  dialog,
  page_indicator,
};

enum class PauseColorRole {
  background,
  normal,
  selected,
  muted,
  accent,
  completed,
  failed,
  warning,
  map_highlight,
};

// Commands use the original 384x240 virtual coordinate system. Asset commands
// only carry names from MENU.HOG; image decoding and drawing stay
// platform-side.
struct PauseRenderCommand {
  PauseRenderKind kind{PauseRenderKind::text};
  PauseRect bounds;
  PauseColorRole color{PauseColorRole::normal};
  std::string text;
  std::string asset;
  std::uint32_t id{};
  std::int32_t value{};
  std::int32_t maximum{};
  bool selected{};
  bool enabled{true};
  PausePanelRole panel{PausePanelRole::none};
  PauseTextAlignment alignment{PauseTextAlignment::left};
  std::uint8_t line_height{10};
};

class PauseMenu final {
public:
  static constexpr std::int16_t screen_width = 384;
  static constexpr std::int16_t screen_height = 240;

  explicit PauseMenu(PauseMenuData data = {}, PauseSettings settings = {});

  void reset(PauseMenuData data, PauseSettings settings = {});
  [[nodiscard]] PauseMenuCommand update(const PauseMenuInput &input);
  [[nodiscard]] std::vector<PauseRenderCommand> buildRenderCommands() const;

  // Completes a binding request emitted by update(). Zero leaves the binding
  // unchanged, which lets the platform treat disconnect/cancel uniformly.
  // A conflicting assignment swaps the two actions so every gameplay action
  // stays reachable on the nine-button retail layout.
  [[nodiscard]] PauseMenuCommand
  completeControllerBinding(std::uint32_t button);
  void cancelControllerBinding() noexcept { binding_pending_ = false; }
  void showControllerMissing();
  void setControllerButtonLabels(
      std::array<std::string, 16U> labels) noexcept;
  void resolveWeaponEquip(std::uint32_t id, bool accepted);
  [[nodiscard]] bool controllerBindingPending() const noexcept {
    return binding_pending_;
  }
  void unlockMissionSelect() noexcept { setMissionSelectUnlocked(true); }
  void setMissionSelectUnlocked(bool enabled) noexcept;
  void setRetailCheatEnabled(RetailCheat cheat, bool enabled) noexcept;
  [[nodiscard]] bool missionSelectUnlocked() const noexcept {
    return mission_select_unlocked_;
  }

  [[nodiscard]] PauseScreen screen() const noexcept;
  [[nodiscard]] std::size_t selection() const noexcept;
  [[nodiscard]] std::size_t sectionSelection() const noexcept;
  [[nodiscard]] std::size_t page() const noexcept { return current().page; }
  [[nodiscard]] bool expanded() const noexcept { return current().expanded; }
  [[nodiscard]] const PauseTransitionState &transition() const noexcept {
    return transition_;
  }
  [[nodiscard]] const PauseMenuData &data() const noexcept { return data_; }
  [[nodiscard]] const PauseSettings &settings() const noexcept {
    return settings_;
  }

private:
  struct ScreenState {
    PauseScreen screen{PauseScreen::root};
    std::size_t selection{};
    std::size_t page{};
    bool expanded{};
  };

  enum class ConfirmationAction {
    none,
    restart_checkpoint,
    restart_mission,
    quit_game,
  };

  [[nodiscard]] ScreenState &current() noexcept;
  [[nodiscard]] const ScreenState &current() const noexcept;
  void push(PauseScreen screen);
  void pop();
  void advanceTransition() noexcept;
  void beginTransition(PauseTransitionKind kind, PauseScreen from_screen,
                       PauseScreen to_screen, std::size_t from_selection,
                       std::size_t to_selection) noexcept;
  void openConfirmation(ConfirmationAction action);
  [[nodiscard]] PauseMenuCommand resumeCommand();
  [[nodiscard]] PauseMenuCommand updateRoot(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updateOptions(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updateCheats(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updateSound(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updateController(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updateBindings(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updateBrightness(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updateCentering(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand
  updateMissionSelect(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updateWeapons(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updatePaged(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand updateMap(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand
  updateConfirmation(const PauseMenuInput &input);
  [[nodiscard]] PauseMenuCommand preview(PauseSetting setting,
                                         std::int32_t value) const;

  PauseMenuData data_;
  PauseSettings settings_;
  PauseSettings committed_settings_;
  PauseSettings centering_backup_;
  std::vector<ScreenState> stack_;
  ConfirmationAction confirmation_action_{ConfirmationAction::none};
  std::size_t pending_binding_{};
  bool binding_pending_{};
  std::string notification_;
  std::array<std::string, 16U> controller_button_labels_{};
  PauseTransitionState transition_;
  bool mission_select_unlocked_{};
};

[[nodiscard]] std::string_view pauseScreenName(PauseScreen screen) noexcept;
[[nodiscard]] std::string_view
controllerPresetName(ControllerPreset preset) noexcept;
void applyControllerPreset(PauseSettings &settings, ControllerPreset preset);
[[nodiscard]] std::uint32_t
controllerButtonForAction(const PauseSettings &settings,
                          ControllerAction action) noexcept;

} // namespace sf::game
