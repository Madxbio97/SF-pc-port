#include "sf/game/pause_menu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace sf::game {
namespace {

// Release-menu topology and labels recovered from BIN/MENU.OVL (USA v1.1).
// The six-entry callback table at 0x8014691c dispatches Map, Objectives,
// Parameters, Briefing, Weapons and Options in this exact order.
constexpr std::size_t root_item_count = 6;
constexpr std::size_t option_item_count = 8;
constexpr std::size_t sound_item_count = 3;
constexpr std::size_t controller_item_count = 7;
constexpr std::size_t binding_item_count = 9;
constexpr std::int32_t volume_step = 5;
constexpr std::int32_t brightness_step = 5;
constexpr std::int16_t centering_limit = 24;
constexpr std::array option_labels{
    "Restart Mission",
    "Restart At Last Checkpoint",
    "Quit Game",
    "Select Mission",
    "Sound",
    "Game Brightness",
    "Screen Centering",
    "Controller",
};

// The release default controller table is shown in MENU.OVL even before the
// player customizes a binding. Keeping it explicit prevents the native menu
// from rendering nine misleading "none" rows on first open.
constexpr std::uint32_t select_button = 0x0001U;
constexpr std::uint32_t l2_button = 0x0100U;
constexpr std::uint32_t r2_button = 0x0200U;
constexpr std::uint32_t l1_button = 0x0400U;
constexpr std::uint32_t r1_button = 0x0800U;
constexpr std::uint32_t triangle_button = 0x1000U;
constexpr std::uint32_t circle_button = 0x2000U;
constexpr std::uint32_t cross_button = 0x4000U;
constexpr std::uint32_t square_button = 0x8000U;

constexpr std::array<ControllerBinding, binding_item_count>
    retail_standard_bindings{{
        {ControllerAction::change_weapon, select_button},
        {ControllerAction::shoot, square_button},
        {ControllerAction::kneel, cross_button},
        {ControllerAction::roll_zoom_out, circle_button},
        {ControllerAction::step_right, r2_button},
        {ControllerAction::step_left, l2_button},
        {ControllerAction::target_lock, r1_button},
        {ControllerAction::use_zoom_in, triangle_button},
        {ControllerAction::aim, l1_button},
    }};

// MENU.OVL 0x801463e0 stores two nine-byte physical-button -> action tables.
// Inverting its alternate table [1,2,7,6,5,4,0,3,8] gives these bindings.
constexpr std::array<ControllerBinding, binding_item_count>
    retail_alternate_bindings{{
        {ControllerAction::change_weapon, r1_button},
        {ControllerAction::shoot, circle_button},
        {ControllerAction::kneel, r2_button},
        {ControllerAction::roll_zoom_out, triangle_button},
        {ControllerAction::step_right, l2_button},
        {ControllerAction::step_left, square_button},
        {ControllerAction::target_lock, cross_button},
        {ControllerAction::use_zoom_in, select_button},
        {ControllerAction::aim, l1_button},
    }};

std::string controllerButtonName(std::uint32_t button) {
  switch (button) {
  case select_button:
    return "SELECT";
  case l2_button:
    return "L2";
  case r2_button:
    return "R2";
  case l1_button:
    return "L1";
  case r1_button:
    return "R1";
  case triangle_button:
    return "TRIANGLE";
  case circle_button:
    return "CIRCLE";
  case cross_button:
    return "CROSS";
  case square_button:
    return "SQUARE";
  case 0x0008U:
    return "START";
  case 0x0010U:
    return "UP";
  case 0x0020U:
    return "RIGHT";
  case 0x0040U:
    return "DOWN";
  case 0x0080U:
    return "LEFT";
  default:
    return button == 0U ? "none" : "BUTTON " + std::to_string(button);
  }
}

template <typename State>
void moveSelection(State &state, std::size_t count,
                   const PauseMenuInput &input) {
  if (count == 0) {
    state.selection = 0;
    return;
  }
  state.selection = std::min(state.selection, count - 1);
  if (input.previous) {
    state.selection = state.selection == 0 ? count - 1 : state.selection - 1;
  } else if (input.next) {
    state.selection = (state.selection + 1) % count;
  }
}

template <typename T>
T adjusted(T value, std::int32_t direction, std::int32_t step, T minimum,
           T maximum) {
  const auto result = static_cast<std::int32_t>(value) + direction * step;
  return static_cast<T>(std::clamp(result, static_cast<std::int32_t>(minimum),
                                   static_cast<std::int32_t>(maximum)));
}

std::size_t visibleEntryCount(const std::vector<MissionMenuEntry> &entries) {
  return static_cast<std::size_t>(std::count_if(
      entries.begin(), entries.end(),
      [](const MissionMenuEntry &entry) { return entry.visible; }));
}

PauseColorRole entryColor(MissionEntryState state) {
  switch (state) {
  case MissionEntryState::active:
    return PauseColorRole::normal;
  case MissionEntryState::completed:
    return PauseColorRole::completed;
  case MissionEntryState::failed:
    return PauseColorRole::failed;
  }
  return PauseColorRole::normal;
}

std::string retailListItem(std::string_view text) {
  return text.starts_with('-') ? std::string{text} : "- " + std::string{text};
}

struct RetailMapLine {
  std::string text;
  bool continuation{};
};

std::vector<RetailMapLine> retailMapLines(std::string_view text) {
  constexpr std::size_t retail_line_columns = 18U;
  std::vector<RetailMapLine> lines;
  auto cursor = std::size_t{};
  auto continuation = false;
  while (cursor < text.size()) {
    while (cursor < text.size() && text[cursor] == ' ') {
      ++cursor;
    }
    if (cursor == text.size()) {
      break;
    }
    auto line = std::string{};
    const auto prefix_columns = continuation ? 2U : 2U;
    const auto available = retail_line_columns - prefix_columns;
    while (cursor < text.size()) {
      const auto word_end = text.find(' ', cursor);
      const auto end =
          word_end == std::string_view::npos ? text.size() : word_end;
      const auto word = text.substr(cursor, end - cursor);
      if (!line.empty() && line.size() + 1U + word.size() > available) {
        break;
      }
      if (!line.empty()) {
        line.push_back(' ');
      }
      line.append(word);
      cursor = end;
      while (cursor < text.size() && text[cursor] == ' ') {
        ++cursor;
      }
      if (line.size() >= available) {
        break;
      }
    }
    lines.push_back(RetailMapLine{
        continuation ? std::move(line) : "- " + line,
        continuation,
    });
    continuation = true;
  }
  return lines;
}

std::string_view objectiveHeading(MissionEntryState state) {
  switch (state) {
  case MissionEntryState::active:
    return "Mission Objectives:";
  case MissionEntryState::completed:
    return "Completed Objectives:";
  case MissionEntryState::failed:
    return "Failed Objectives:";
  }
  return {};
}

std::string_view parameterHeading(MissionEntryState state) {
  switch (state) {
  case MissionEntryState::active:
    return "Mission Parameters:";
  case MissionEntryState::completed:
    return "Mission Parameters:";
  case MissionEntryState::failed:
    return "Failed Parameters:";
  }
  return {};
}

} // namespace

std::vector<MissionMenuEntry> makeRetailMissionMenuEntries(
    std::span<const std::string> exact_texts, std::uint32_t entry_count,
    std::uint32_t visible_mask, std::uint32_t completed_mask,
    std::uint32_t failed_mask) {
  constexpr std::uint32_t mask_bit_count = 32U;
  if (entry_count > mask_bit_count || exact_texts.size() != entry_count ||
      std::ranges::any_of(exact_texts,
                          [](const auto &text) { return text.empty(); })) {
    return {};
  }

  std::vector<MissionMenuEntry> entries;
  entries.reserve(entry_count);
  for (auto ordinal = entry_count; ordinal != 0U; --ordinal) {
    const auto index = ordinal - 1U;
    const auto &text = exact_texts[index];
    if (text == ".") {
      continue;
    }
    const auto bit = std::uint32_t{1U} << index;
    auto state = MissionEntryState::active;
    if ((failed_mask & bit) != 0U) {
      state = MissionEntryState::failed;
    } else if ((completed_mask & bit) != 0U) {
      state = MissionEntryState::completed;
    }
    entries.push_back(MissionMenuEntry{
        index + 1U,
        text,
        state,
        ((visible_mask | completed_mask | failed_mask) & bit) != 0U,
    });
  }
  return entries;
}

PauseMenu::PauseMenu(PauseMenuData data, PauseSettings settings) {
  reset(std::move(data), std::move(settings));
}

void PauseMenu::reset(PauseMenuData data, PauseSettings settings) {
  data_ = std::move(data);
  settings_ = std::move(settings);
  if (settings_.controller_preset != ControllerPreset::custom) {
    applyControllerPreset(settings_, settings_.controller_preset);
  }
  committed_settings_ = settings_;
  centering_backup_ = settings_;
  stack_.clear();
  stack_.push_back(ScreenState{});
  confirmation_action_ = ConfirmationAction::none;
  pending_binding_ = 0;
  binding_pending_ = false;
  notification_.clear();
  transition_ = {};
  // Full campaign traversal is a developer feature. The platform may unlock
  // it explicitly after finding the opt-in marker beside the executable.
  mission_select_unlocked_ = false;
}

PauseMenu::ScreenState &PauseMenu::current() noexcept { return stack_.back(); }

const PauseMenu::ScreenState &PauseMenu::current() const noexcept {
  return stack_.back();
}

PauseScreen PauseMenu::screen() const noexcept { return current().screen; }

std::size_t PauseMenu::selection() const noexcept {
  return current().selection;
}

std::size_t PauseMenu::sectionSelection() const noexcept {
  return stack_.empty()
             ? 0
             : std::min(stack_.front().selection, root_item_count - 1);
}

void PauseMenu::advanceTransition() noexcept {
  if (transition_.input_delay > 0) {
    --transition_.input_delay;
  }
  if (!transition_.active()) {
    return;
  }
  ++transition_.frame;
  if (transition_.frame >= transition_.duration) {
    transition_.kind = PauseTransitionKind::none;
  }
}

void PauseMenu::beginTransition(PauseTransitionKind kind,
                                PauseScreen from_screen, PauseScreen to_screen,
                                std::size_t from_selection,
                                std::size_t to_selection) noexcept {
  // MENU.OVL stores one quarter of the frame delta (difference >> 2), so
  // panel/selection interpolation is exactly four frames. Input is paced by
  // the overlay for ten frames after a section change.
  transition_ = PauseTransitionState{
      kind, from_screen, to_screen, from_selection, to_selection, 0, 4, 10,
  };
}

void PauseMenu::push(PauseScreen next_screen) {
  auto next = ScreenState{next_screen, 0, 0};
  if (next_screen == PauseScreen::map) {
    next.page = std::min(data_.mission.map.current_layer,
                         data_.mission.map.layer_assets.empty()
                             ? std::size_t{}
                             : data_.mission.map.layer_assets.size() - 1U);
  } else if (next_screen == PauseScreen::weapons) {
    const auto equipped = std::find_if(
        data_.weapons.begin(), data_.weapons.end(),
        [](const PauseWeaponData &weapon) { return weapon.equipped; });
    if (equipped != data_.weapons.end()) {
      next.selection =
          static_cast<std::size_t>(equipped - data_.weapons.begin());
    }
  }
  stack_.push_back(next);
}

void PauseMenu::pop() {
  if (stack_.size() > 1) {
    stack_.pop_back();
  }
}

void PauseMenu::openConfirmation(ConfirmationAction action) {
  confirmation_action_ = action;
  push(PauseScreen::confirmation);
  current().selection = 1;
}

PauseMenuCommand PauseMenu::preview(PauseSetting setting,
                                    std::int32_t value) const {
  return PauseMenuCommand{
      PauseCommandType::preview_setting,
      static_cast<std::uint32_t>(setting),
      value,
  };
}

PauseMenuCommand PauseMenu::resumeCommand() {
  committed_settings_ = settings_;
  return PauseMenuCommand{PauseCommandType::resume};
}

PauseMenuCommand PauseMenu::update(const PauseMenuInput &input) {
  advanceTransition();
  if (input.pause) {
    binding_pending_ = false;
    return resumeCommand();
  }
  const auto has_navigation_input = input.previous || input.next ||
                                    input.left || input.right ||
                                    input.confirm || input.cancel;
  if (transition_.input_delay > 0 && has_navigation_input) {
    return {};
  }

  const auto old_screen = screen();
  const auto old_section = sectionSelection();
  const auto old_selection = selection();
  PauseMenuCommand result;
  switch (screen()) {
  case PauseScreen::root:
    result = updateRoot(input);
    break;
  case PauseScreen::options:
    result = updateOptions(input);
    break;
  case PauseScreen::sound:
    result = updateSound(input);
    break;
  case PauseScreen::controller:
    result = updateController(input);
    break;
  case PauseScreen::controller_bindings:
    result = updateBindings(input);
    break;
  case PauseScreen::brightness:
    result = updateBrightness(input);
    break;
  case PauseScreen::screen_centering:
    result = updateCentering(input);
    break;
  case PauseScreen::mission_select:
    result = updateMissionSelect(input);
    break;
  case PauseScreen::weapons:
    result = updateWeapons(input);
    break;
  case PauseScreen::briefing:
  case PauseScreen::parameters:
  case PauseScreen::objectives:
    result = updatePaged(input);
    break;
  case PauseScreen::map:
    result = updateMap(input);
    break;
  case PauseScreen::confirmation:
    result = updateConfirmation(input);
    break;
  case PauseScreen::notification:
    if (input.confirm || input.cancel) {
      notification_.clear();
      pop();
    }
    break;
  }

  const auto new_screen = screen();
  const auto new_section = sectionSelection();
  const auto new_selection = selection();
  if (new_screen != old_screen) {
    beginTransition(PauseTransitionKind::screen_change, old_screen, new_screen,
                    old_section, new_section);
  } else if (new_selection != old_selection) {
    beginTransition(old_screen == PauseScreen::root
                        ? PauseTransitionKind::section_selection
                        : PauseTransitionKind::item_selection,
                    old_screen, new_screen, old_selection, new_selection);
  }
  return result;
}

PauseMenuCommand PauseMenu::updateRoot(const PauseMenuInput &input) {
  if (input.cancel) {
    return resumeCommand();
  }
  moveSelection(current(), root_item_count, input);
  if (!input.confirm) {
    return {};
  }

  // Retail FUN_MENU_OVL__8013f714 deliberately leaves selectors 1 and 2
  // (Objectives/Parameters) in the root preview. Only Map and selectors
  // 3..5 enter another screen.
  switch (current().selection) {
  case 0:
    if (data_.mission.map.reconnaissance_available &&
        !data_.mission.map.layer_assets.empty()) {
      push(PauseScreen::map);
    }
    break;
  case 1:
  case 2:
    break;
  case 3:
    push(PauseScreen::briefing);
    break;
  case 4:
    push(PauseScreen::weapons);
    break;
  case 5:
    push(PauseScreen::options);
    break;
  default:
    break;
  }
  return {};
}

PauseMenuCommand PauseMenu::updateOptions(const PauseMenuInput &input) {
  if (input.cancel) {
    pop();
    return {};
  }
  moveSelection(current(), option_item_count, input);
  if (!input.confirm) {
    return {};
  }

  switch (current().selection) {
  case 0:
    openConfirmation(ConfirmationAction::restart_mission);
    break;
  case 1:
    openConfirmation(ConfirmationAction::restart_checkpoint);
    break;
  case 2:
    openConfirmation(ConfirmationAction::quit_game);
    break;
  case 3:
    push(PauseScreen::mission_select);
    if (!data_.missions.empty()) {
      const auto current_mission = std::ranges::find(
          data_.missions, data_.current_mission, &MissionMenuEntry::id);
      current().selection = current_mission == data_.missions.end()
                                ? 0U
                                : static_cast<std::size_t>(
                                      current_mission - data_.missions.begin());
    }
    break;
  case 4:
    push(PauseScreen::sound);
    break;
  case 5:
    push(PauseScreen::brightness);
    break;
  case 6:
    centering_backup_ = settings_;
    push(PauseScreen::screen_centering);
    break;
  case 7:
    push(PauseScreen::controller);
    break;
  default:
    break;
  }
  return {};
}

PauseMenuCommand PauseMenu::updateMissionSelect(const PauseMenuInput &input) {
  if (input.cancel) {
    pop();
    return {};
  }
  if (data_.missions.empty()) {
    return {};
  }
  moveSelection(current(), data_.missions.size(), input);
  if (!input.confirm) {
    return {};
  }
  const auto &selected = data_.missions[current().selection];
  const auto unlocked_mission =
      std::max(data_.current_mission, data_.maximum_unlocked_mission);
  // Retail progress is a high-water mark: reaching mission N means every
  // earlier campaign entry has already been completed and remains replayable.
  // The opt-in cheat only extends the range forward to unreached missions.
  if (!mission_select_unlocked_ && selected.id > unlocked_mission) {
    notification_ = "Mission locked";
    push(PauseScreen::notification);
    return {};
  }
  return PauseMenuCommand{PauseCommandType::select_mission, selected.id};
}

PauseMenuCommand PauseMenu::updateSound(const PauseMenuInput &input) {
  if (input.cancel) {
    committed_settings_ = settings_;
    pop();
    return PauseMenuCommand{PauseCommandType::commit_settings};
  }
  moveSelection(current(), sound_item_count, input);
  const auto direction = input.left ? -1 : (input.right ? 1 : 0);
  if (direction == 0) {
    return {};
  }

  switch (current().selection) {
  case 0:
    settings_.sound_effects_volume =
        adjusted(settings_.sound_effects_volume, direction, volume_step,
                 std::uint8_t{0}, std::uint8_t{100});
    return preview(PauseSetting::sound_effects_volume,
                   settings_.sound_effects_volume);
  case 1:
    settings_.music_volume =
        adjusted(settings_.music_volume, direction, volume_step,
                 std::uint8_t{0}, std::uint8_t{100});
    return preview(PauseSetting::music_volume, settings_.music_volume);
  case 2:
    settings_.voice_volume =
        adjusted(settings_.voice_volume, direction, volume_step,
                 std::uint8_t{0}, std::uint8_t{100});
    return preview(PauseSetting::voice_volume, settings_.voice_volume);
  default:
    return {};
  }
}

PauseMenuCommand PauseMenu::updateController(const PauseMenuInput &input) {
  if (input.cancel) {
    settings_.controller_preset = committed_settings_.controller_preset;
    settings_.invert_aim = committed_settings_.invert_aim;
    settings_.vibration = committed_settings_.vibration;
    settings_.bindings = committed_settings_.bindings;
    pop();
    return PauseMenuCommand{PauseCommandType::revert_settings};
  }
  moveSelection(current(), controller_item_count, input);

  const auto selection = current().selection;
  if (selection == 0 && (input.left || input.right)) {
    auto preset = static_cast<std::int32_t>(settings_.controller_preset);
    preset += input.left ? -1 : 1;
    if (preset < 0) {
      preset = static_cast<std::int32_t>(ControllerPreset::custom);
    } else if (preset > static_cast<std::int32_t>(ControllerPreset::custom)) {
      preset = 0;
    }
    settings_.controller_preset = static_cast<ControllerPreset>(preset);
    if (settings_.controller_preset != ControllerPreset::custom) {
      applyControllerPreset(settings_, settings_.controller_preset);
    }
    return preview(PauseSetting::controller_preset, preset);
  }
  if (selection == 2 && (input.left || input.right)) {
    settings_.invert_aim = !settings_.invert_aim;
    return preview(PauseSetting::invert_aim, settings_.invert_aim ? 1 : 0);
  }
  if (selection == 3 && (input.left || input.right)) {
    settings_.vibration = !settings_.vibration;
    return preview(PauseSetting::vibration, settings_.vibration ? 1 : 0);
  }
  if (!input.confirm) {
    return {};
  }

  switch (selection) {
  case 0:
    settings_.controller_preset =
        settings_.controller_preset == ControllerPreset::custom
            ? ControllerPreset::standard
            : static_cast<ControllerPreset>(
                  static_cast<std::int32_t>(settings_.controller_preset) + 1);
    applyControllerPreset(settings_, settings_.controller_preset);
    return preview(PauseSetting::controller_preset,
                   static_cast<std::int32_t>(settings_.controller_preset));
  case 1:
    push(PauseScreen::controller_bindings);
    return {};
  case 2:
    settings_.invert_aim = !settings_.invert_aim;
    return preview(PauseSetting::invert_aim, settings_.invert_aim ? 1 : 0);
  case 3:
    settings_.vibration = !settings_.vibration;
    return preview(PauseSetting::vibration, settings_.vibration ? 1 : 0);
  case 4:
    settings_.invert_aim = false;
    settings_.vibration = true;
    applyControllerPreset(settings_, ControllerPreset::standard);
    return preview(PauseSetting::bindings, 0);
  case 5:
    committed_settings_ = settings_;
    pop();
    return PauseMenuCommand{PauseCommandType::commit_settings};
  case 6:
    settings_.controller_preset = committed_settings_.controller_preset;
    settings_.invert_aim = committed_settings_.invert_aim;
    settings_.vibration = committed_settings_.vibration;
    settings_.bindings = committed_settings_.bindings;
    pop();
    return PauseMenuCommand{PauseCommandType::revert_settings};
  default:
    return {};
  }
}

PauseMenuCommand PauseMenu::updateBindings(const PauseMenuInput &input) {
  if (input.cancel) {
    binding_pending_ = false;
    pop();
    return {};
  }
  if (binding_pending_) {
    return {};
  }
  moveSelection(current(), binding_item_count, input);
  if (!input.confirm) {
    return {};
  }
  pending_binding_ = current().selection;
  binding_pending_ = true;
  return PauseMenuCommand{
      PauseCommandType::begin_controller_binding,
      static_cast<std::uint32_t>(pending_binding_),
  };
}

PauseMenuCommand PauseMenu::completeControllerBinding(std::uint32_t button) {
  if (!binding_pending_) {
    return {};
  }
  binding_pending_ = false;
  if (button == 0 || pending_binding_ >= binding_item_count) {
    return {};
  }

  const auto action = static_cast<ControllerAction>(pending_binding_);
  const auto binding =
      std::find_if(settings_.bindings.begin(), settings_.bindings.end(),
                   [action](const ControllerBinding &value) {
                     return value.action == action;
                   });
  if (binding == settings_.bindings.end()) {
    settings_.bindings.push_back(ControllerBinding{action, button});
  } else {
    binding->button = button;
  }
  settings_.controller_preset = ControllerPreset::custom;
  return PauseMenuCommand{
      PauseCommandType::preview_setting,
      static_cast<std::uint32_t>(PauseSetting::bindings),
      static_cast<std::int32_t>(button),
  };
}

PauseMenuCommand PauseMenu::updateBrightness(const PauseMenuInput &input) {
  if (input.cancel) {
    settings_.brightness = committed_settings_.brightness;
    pop();
    return PauseMenuCommand{PauseCommandType::revert_settings};
  }
  const auto direction = (input.previous || input.left)
                             ? -1
                             : ((input.next || input.right) ? 1 : 0);
  if (direction != 0) {
    settings_.brightness =
        adjusted(settings_.brightness, direction, brightness_step,
                 std::uint8_t{0}, std::uint8_t{100});
    return preview(PauseSetting::brightness, settings_.brightness);
  }
  if (input.confirm) {
    committed_settings_ = settings_;
    pop();
    return PauseMenuCommand{PauseCommandType::commit_settings};
  }
  return {};
}

PauseMenuCommand PauseMenu::updateCentering(const PauseMenuInput &input) {
  if (input.cancel) {
    settings_.screen_center_x = centering_backup_.screen_center_x;
    settings_.screen_center_y = centering_backup_.screen_center_y;
    pop();
    return PauseMenuCommand{PauseCommandType::revert_settings};
  }
  if (input.confirm) {
    committed_settings_ = settings_;
    pop();
    return PauseMenuCommand{PauseCommandType::commit_settings};
  }

  if (input.left || input.right) {
    settings_.screen_center_x =
        adjusted(settings_.screen_center_x, input.left ? -1 : 1, 1,
                 static_cast<std::int16_t>(-centering_limit), centering_limit);
    return preview(PauseSetting::screen_center_x, settings_.screen_center_x);
  }
  if (input.previous || input.next) {
    settings_.screen_center_y =
        adjusted(settings_.screen_center_y, input.previous ? -1 : 1, 1,
                 static_cast<std::int16_t>(-centering_limit), centering_limit);
    return preview(PauseSetting::screen_center_y, settings_.screen_center_y);
  }
  return {};
}

PauseMenuCommand PauseMenu::updateWeapons(const PauseMenuInput &input) {
  if (input.cancel) {
    if (current().expanded) {
      current().expanded = false;
      current().page = 0;
    } else {
      pop();
    }
    return {};
  }
  if (current().expanded) {
    if (input.left && current().page > 0) {
      --current().page;
    } else if (input.right && current().page < 1) {
      ++current().page;
    }
  }
  moveSelection(current(), data_.weapons.size(), input);
  if (!input.confirm || data_.weapons.empty()) {
    return {};
  }
  if (!current().expanded) {
    current().expanded = true;
    current().page = 0;
    return {};
  }

  auto &weapon = data_.weapons[current().selection];
  if (!weapon.available) {
    return {};
  }
  if (!weapon.equip_allowed) {
    notification_ = "Cannot change weapons while performing current action";
    push(PauseScreen::notification);
    return {};
  }
  return PauseMenuCommand{PauseCommandType::equip_weapon, weapon.id};
}

PauseMenuCommand PauseMenu::updatePaged(const PauseMenuInput &input) {
  if (input.cancel) {
    pop();
    return {};
  }

  auto &state = current();
  if (state.screen == PauseScreen::briefing) {
    const auto count =
        std::max<std::size_t>(data_.mission.briefing_pages.size(), 1);
    if ((input.next || input.right || input.confirm) &&
        state.page + 1 < count) {
      ++state.page;
    } else if ((input.previous || input.left) && state.page > 0) {
      --state.page;
    }
    return {};
  }

  const auto &entries = state.screen == PauseScreen::objectives
                            ? data_.mission.objectives
                            : data_.mission.parameters;
  const auto count = visibleEntryCount(entries);
  if ((input.next || input.right) && state.page + 1 < count) {
    ++state.page;
  } else if ((input.previous || input.left) && state.page > 0) {
    --state.page;
  }
  return {};
}

PauseMenuCommand PauseMenu::updateMap(const PauseMenuInput &input) {
  if (input.cancel) {
    if (current().expanded) {
      current().expanded = false;
    } else {
      pop();
    }
    return {};
  }
  if (input.confirm) {
    current().expanded = true;
  }
  const auto count = data_.mission.map.layer_assets.size();
  if ((input.right || input.next) && current().page + 1 < count) {
    ++current().page;
  } else if ((input.left || input.previous) && current().page > 0) {
    --current().page;
  }
  return {};
}

PauseMenuCommand PauseMenu::updateConfirmation(const PauseMenuInput &input) {
  moveSelection(current(), 2, input);
  if (input.cancel) {
    confirmation_action_ = ConfirmationAction::none;
    pop();
    return {};
  }
  if (!input.confirm) {
    return {};
  }
  if (current().selection == 1) {
    confirmation_action_ = ConfirmationAction::none;
    pop();
    return {};
  }

  const auto action = confirmation_action_;
  confirmation_action_ = ConfirmationAction::none;
  switch (action) {
  case ConfirmationAction::restart_checkpoint:
    return PauseMenuCommand{PauseCommandType::restart_checkpoint};
  case ConfirmationAction::restart_mission:
    return PauseMenuCommand{PauseCommandType::restart_mission};
  case ConfirmationAction::quit_game:
    return PauseMenuCommand{PauseCommandType::quit_game};
  case ConfirmationAction::none:
    return {};
  }
  return {};
}

void PauseMenu::showControllerMissing() {
  binding_pending_ = false;
  notification_ = "Controller missing. Please reinsert controller into "
                  "controller port 1 and press the START "
                  "button to continue";
  if (screen() != PauseScreen::notification) {
    push(PauseScreen::notification);
  }
}

void PauseMenu::resolveWeaponEquip(std::uint32_t id, bool accepted) {
  if (accepted) {
    for (auto &item : data_.weapons) {
      item.equipped = item.id == id;
    }
    return;
  }
  notification_ = "Cannot change weapons while performing current action";
  if (screen() != PauseScreen::notification) {
    push(PauseScreen::notification);
  }
}

std::vector<PauseRenderCommand> PauseMenu::buildRenderCommands() const {
  std::vector<PauseRenderCommand> commands;
  commands.reserve(64);

  const auto add =
      [&commands](PauseRenderKind kind, PauseRect bounds,
                  std::string_view text = {},
                  PauseColorRole color =
                      PauseColorRole::normal) -> PauseRenderCommand & {
    PauseRenderCommand command;
    command.kind = kind;
    command.bounds = bounds;
    command.color = color;
    command.text = text;
    commands.push_back(std::move(command));
    return commands.back();
  };
  const auto addMenu = [&add](std::string_view text, std::size_t index,
                              std::size_t selected, std::int16_t y,
                              bool enabled = true) {
    auto &command =
        add(PauseRenderKind::menu_item, PauseRect{52, y, 165, 12}, text,
            index == selected
                ? PauseColorRole::selected
                : (enabled ? PauseColorRole::normal : PauseColorRole::muted));
    command.id = static_cast<std::uint32_t>(index);
    command.selected = index == selected;
    command.enabled = enabled;
  };
  const auto addMapMarkers = [&add](const PauseMapData &map,
                                    PauseRect map_bounds, std::size_t page,
                                    std::int16_t marker_size) {
    for (const auto &source : map.markers) {
      if (!source.visible ||
          (source.layer != all_pause_map_layers && source.layer != page)) {
        continue;
      }
      const auto marker_x =
          std::isfinite(source.x) ? std::clamp(source.x, 0.0F, 1.0F) : 0.5F;
      const auto marker_y =
          std::isfinite(source.y) ? std::clamp(source.y, 0.0F, 1.0F) : 0.5F;
      const auto marker_heading =
          std::isfinite(source.heading)
              ? std::fmod(static_cast<double>(source.heading), 1.0)
              : 0.0;
      auto &marker = add(
          PauseRenderKind::map_marker,
          PauseRect{
              static_cast<std::int16_t>(
                  map_bounds.x + marker_x * map_bounds.width - marker_size / 2),
              static_cast<std::int16_t>(map_bounds.y +
                                        marker_y * map_bounds.height -
                                        marker_size / 2),
              marker_size,
              marker_size,
          });
      marker.id = static_cast<std::uint32_t>(source.kind);
      marker.value = static_cast<std::int32_t>(marker_heading * 4096.0);
      marker.maximum = static_cast<std::int32_t>(source.objective_id);
      switch (source.kind) {
      case MapMarkerKind::player:
      case MapMarkerKind::objective:
        marker.color = PauseColorRole::map_highlight;
        break;
      case MapMarkerKind::hostile:
        marker.color = PauseColorRole::warning;
        break;
      case MapMarkerKind::friendly:
        marker.color = PauseColorRole::accent;
        break;
      }
    }
  };
  const auto addMapInformation = [&add](const PauseMenuData &data,
                                        PauseRect bounds, std::size_t page,
                                        bool expanded) {
    const auto &map = data.mission.map;
    const auto marker_on_page = [&map, page](MapMarkerKind kind,
                                             std::uint32_t objective_id) {
      return std::ranges::any_of(map.markers, [&](const auto &marker) {
        return marker.visible && marker.kind == kind &&
               (marker.layer == all_pause_map_layers || marker.layer == page) &&
               (kind != MapMarkerKind::objective ||
                marker.objective_id == objective_id);
      });
    };
    const auto add_line =
        [&add, bounds](std::int16_t y, std::string text, bool continuation,
                       PauseColorRole color, std::uint32_t id, bool highlighted,
                       std::uint8_t line_height) {
          const auto indent = static_cast<std::int16_t>(continuation ? 7 : 0);
          auto &line =
              add(PauseRenderKind::text,
                  PauseRect{static_cast<std::int16_t>(bounds.x + indent), y,
                            static_cast<std::int16_t>(bounds.width - indent),
                            static_cast<std::int16_t>(line_height)},
                  text, color);
          line.id = id;
          line.selected = highlighted;
          line.line_height = line_height;
        };

    const auto add_entry = [&](std::string_view text, std::uint32_t id,
                               bool highlighted, std::int16_t &y) {
      const auto color =
          highlighted ? PauseColorRole::map_highlight : PauseColorRole::normal;
      const auto line_step = static_cast<std::int16_t>(expanded ? 9 : 7);
      auto lines = retailMapLines(text);
      const auto required_height = static_cast<std::int16_t>(
          lines.size() * static_cast<std::size_t>(line_step));
      if (y + required_height > bounds.y + bounds.height) {
        return false;
      }
      for (auto &line : lines) {
        add_line(y, std::move(line.text), line.continuation, color, id,
                 highlighted, expanded ? 9U : 7U);
        y = static_cast<std::int16_t>(y + line_step);
      }
      return true;
    };

    const auto player_highlighted = marker_on_page(MapMarkerKind::player, 0U);
    auto y = bounds.y;
    static_cast<void>(add_entry("Current Location", 0U, player_highlighted, y));

    for (const auto &objective : data.mission.objectives) {
      if (!objective.visible || objective.state != MissionEntryState::active ||
          y + 7 > bounds.y + bounds.height) {
        continue;
      }
      const auto highlighted =
          marker_on_page(MapMarkerKind::objective, objective.id);
      if (!add_entry(objective.text, objective.id, highlighted, y)) {
        break;
      }
    }
  };

  const auto &state = current();
  const auto expanded_detail =
      state.expanded && (state.screen == PauseScreen::map ||
                         state.screen == PauseScreen::weapons);

  add(PauseRenderKind::dim_background, PauseAcdLayout::canvas);
  if (!expanded_detail) {
    auto &left_panel = add(PauseRenderKind::panel, PauseAcdLayout::left_grid,
                           {}, PauseColorRole::background);
    left_panel.panel = PausePanelRole::left_content;
    auto &information_panel =
        add(PauseRenderKind::panel, PauseAcdLayout::information_grid, {},
            PauseColorRole::background);
    information_panel.panel = PausePanelRole::right_information;
    auto &section_panel =
        add(PauseRenderKind::panel, PauseAcdLayout::section_menu, {},
            PauseColorRole::background);
    section_panel.panel = PausePanelRole::right_sections;
    auto &hint_panel = add(PauseRenderKind::panel, PauseAcdLayout::hint, {},
                           PauseColorRole::background);
    hint_panel.panel = PausePanelRole::hint;
  }
  switch (state.screen) {
  case PauseScreen::root: {
    // The original ACD keeps the six section selectors on the right and
    // previews the highlighted section in the left display.
    constexpr std::array labels{"Map",      "Objectives", "Parameters",
                                "Briefing", "Weapons",    "Options"};
    for (std::size_t index = 0; index < labels.size(); ++index) {
      auto &item = add(PauseRenderKind::menu_item,
                       PauseAcdLayout::sectionSelection(index), labels[index],
                       index == state.selection ? PauseColorRole::selected
                                                : PauseColorRole::normal);
      item.id = static_cast<std::uint32_t>(index);
      item.selected = index == state.selection;
    }

    if (state.selection == 0) {
      const auto &map = data_.mission.map;
      if (!map.reconnaissance_available || map.layer_assets.empty()) {
        add(PauseRenderKind::text, PauseRect{56, 96, 157, 18},
            "No Reconnaissance", PauseColorRole::warning);
      } else {
        constexpr auto map_bounds = PauseAcdLayout::map_image;
        const auto page =
            std::min(map.current_layer, map.layer_assets.size() - 1U);
        auto &asset = add(PauseRenderKind::asset, map_bounds);
        asset.asset = map.layer_assets[page];
        addMapMarkers(map, map_bounds, page, 6);
        addMapInformation(data_, PauseAcdLayout::information_content, page,
                          false);
      }
    } else {
      const auto heading = labels[state.selection];
      add(PauseRenderKind::title, PauseRect{56, 36, 157, 10}, heading,
          PauseColorRole::accent);
      std::string preview;
      std::string information;
      if (state.selection == 1 || state.selection == 2) {
        const auto &entries = state.selection == 1 ? data_.mission.objectives
                                                   : data_.mission.parameters;
        auto active = std::size_t{};
        auto completed = std::size_t{};
        for (const auto &entry : entries) {
          if (entry.visible) {
            if (!preview.empty()) {
              preview += "\n";
            }
            preview += retailListItem(entry.text);
            active += entry.state == MissionEntryState::active ? 1U : 0U;
            completed += entry.state == MissionEntryState::completed ? 1U : 0U;
          }
        }
        if (preview.empty()) {
          preview = "none";
        }
        information = state.selection == 1 ? "Mission Objectives\n"
                                           : "Mission Parameters\n";
        information += "Active: " + std::to_string(active);
        if (state.selection == 1) {
          information += "\nCompleted: " + std::to_string(completed);
        }
      } else if (state.selection == 3) {
        preview = data_.mission.briefing_pages.empty()
                      ? "No briefing data available"
                      : data_.mission.briefing_pages.front();
        information = data_.mission.mission_name;
        if (!data_.mission.location.empty()) {
          information += "\n" + data_.mission.location;
        }
      } else if (state.selection == 4) {
        const auto equipped = std::find_if(
            data_.weapons.begin(), data_.weapons.end(),
            [](const PauseWeaponData &weapon) { return weapon.equipped; });
        preview = equipped == data_.weapons.end()
                      ? "No weapon equipped"
                      : "Equipped " + equipped->name + ".";
        if (equipped != data_.weapons.end()) {
          if (equipped->maximum_ammo > 0) {
            preview += "\nAmmo: " + std::to_string(equipped->ammo) + "/" +
                       std::to_string(equipped->maximum_ammo);
          }
        }
        if (equipped != data_.weapons.end() && !equipped->icon_asset.empty()) {
          auto &asset =
              add(PauseRenderKind::asset, PauseRect{238, 42, 105, 68});
          asset.asset = equipped->icon_asset;
          asset.id = equipped->id;
        }
      } else {
        information = "Configuration\nBrightness: " +
                      std::to_string(settings_.brightness);
        for (std::size_t index = 0; index < option_labels.size(); ++index) {
          addMenu(option_labels[index], index, option_labels.size(),
                  static_cast<std::int16_t>(51 + index * 18));
        }
      }
      if (!preview.empty()) {
        add(PauseRenderKind::text, PauseRect{56, 51, 157, 131}, preview);
      }
      if (!information.empty()) {
        add(PauseRenderKind::text, PauseRect{240, 44, 101, 60}, information);
      }
    }
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        "%x select   %t resume");
    break;
  }
  case PauseScreen::options: {
    add(PauseRenderKind::title, PauseRect{56, 36, 157, 10}, "Options",
        PauseColorRole::accent);
    // MENU.OVL keeps Select Mission in this list. Retail restricts it to
    // reached missions until its held-button cheat opens the full table.
    for (std::size_t index = 0; index < option_labels.size(); ++index) {
      addMenu(option_labels[index], index, state.selection,
              static_cast<std::int16_t>(51 + index * 18));
    }
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        "%x select   %t back");
    break;
  }
  case PauseScreen::mission_select: {
    add(PauseRenderKind::title, PauseRect{56, 36, 157, 10},
        mission_select_unlocked_ ? "Select Mission - All" : "Select Mission",
        PauseColorRole::accent);
    constexpr std::size_t rows = 10U;
    const auto first = state.selection < rows
                           ? 0U
                           : std::min(state.selection - rows + 1U,
                                      data_.missions.size() > rows
                                          ? data_.missions.size() - rows
                                          : 0U);
    const auto last = std::min(first + rows, data_.missions.size());
    for (auto index = first; index < last; ++index) {
      const auto &mission = data_.missions[index];
      const auto unlocked_mission =
          std::max(data_.current_mission, data_.maximum_unlocked_mission);
      const auto enabled =
          mission_select_unlocked_ || mission.id <= unlocked_mission;
      addMenu(mission.text, index, state.selection,
              static_cast<std::int16_t>(50 + (index - first) * 14), enabled);
    }
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        "%x select   %t back");
    break;
  }
  case PauseScreen::sound: {
    add(PauseRenderKind::title, PauseRect{56, 36, 157, 10}, "Sound",
        PauseColorRole::accent);
    constexpr std::array labels{"Sound FX", "Music", "Voice-over"};
    const std::array values{
        settings_.sound_effects_volume,
        settings_.music_volume,
        settings_.voice_volume,
    };
    for (std::size_t index = 0; index < labels.size(); ++index) {
      const auto y = static_cast<std::int16_t>(54 + index * 40);
      auto &item = add(PauseRenderKind::menu_item, PauseRect{56, y, 157, 11},
                       labels[index],
                       index == state.selection ? PauseColorRole::selected
                                                : PauseColorRole::normal);
      item.id = static_cast<std::uint32_t>(index);
      item.selected = index == state.selection;
      auto &slider =
          add(PauseRenderKind::slider,
              PauseRect{72, static_cast<std::int16_t>(y + 14), 129, 8});
      slider.value = values[index];
      slider.maximum = 100;
      slider.selected = index == state.selection;
    }
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint, "%t back");
    break;
  }
  case PauseScreen::controller: {
    add(PauseRenderKind::title, PauseRect{56, 36, 157, 10}, "Controller",
        PauseColorRole::accent);
    const std::array labels{
        std::string{"Preset config: "} +
            std::string{controllerPresetName(settings_.controller_preset)},
        std::string{"Controller Configuration:"},
        std::string{"Invert Aim: "} + (settings_.invert_aim ? "yes" : "no"),
        std::string{"Vibration: "} + (settings_.vibration ? "yes" : "no"),
        std::string{"Reset"},
        std::string{"Accept"},
        std::string{"Cancel"},
    };
    for (std::size_t index = 0; index < labels.size(); ++index) {
      addMenu(labels[index], index, state.selection,
              static_cast<std::int16_t>(49 + index * 18));
    }
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        "%x select   %t cancel");
    break;
  }
  case PauseScreen::controller_bindings: {
    add(PauseRenderKind::title, PauseRect{56, 35, 157, 10},
        "Controller Configuration:", PauseColorRole::accent);
    for (std::size_t index = 0; index < binding_item_count; ++index) {
      const auto action = static_cast<ControllerAction>(index);
      std::string label{controllerActionName(action)};
      const auto binding =
          std::find_if(settings_.bindings.begin(), settings_.bindings.end(),
                       [action](const ControllerBinding &value) {
                         return value.action == action;
                       });
      label += ": ";
      label += binding == settings_.bindings.end()
                   ? "none"
                   : controllerButtonName(binding->button);
      addMenu(label, index, state.selection,
              static_cast<std::int16_t>(48 + index * 14));
    }
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        binding_pending_ ? "Press new button for action"
                         : "%x select   %t back");
    break;
  }
  case PauseScreen::brightness: {
    add(PauseRenderKind::title, PauseRect{56, 36, 157, 10}, "Game Brightness",
        PauseColorRole::accent);
    add(PauseRenderKind::text, PauseRect{56, 59, 157, 38},
        "Adjust game lighting levels by moving bar up or down");
    auto &slider = add(PauseRenderKind::slider, PauseRect{60, 116, 149, 10});
    slider.value = settings_.brightness;
    slider.maximum = 100;
    slider.selected = true;
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        "%x accept   %t cancel");
    break;
  }
  case PauseScreen::screen_centering: {
    add(PauseRenderKind::title, PauseRect{56, 36, 157, 10}, "Screen Centering",
        PauseColorRole::accent);
    add(PauseRenderKind::text, PauseRect{56, 59, 157, 28},
        "Use directional buttons to center the image.");
    auto &marker = add(PauseRenderKind::selection, PauseRect{129, 105, 12, 12});
    marker.bounds.x = static_cast<std::int16_t>(marker.bounds.x +
                                                settings_.screen_center_x * 2);
    marker.bounds.y = static_cast<std::int16_t>(marker.bounds.y +
                                                settings_.screen_center_y * 2);
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        "%x Save  %t Cancel");
    break;
  }
  case PauseScreen::briefing: {
    add(PauseRenderKind::title, PauseRect{56, 36, 157, 10}, "Briefing",
        PauseColorRole::accent);
    std::string metadata;
    if (!data_.mission.date_time.empty()) {
      metadata += data_.mission.date_time;
    }
    if (!data_.mission.location.empty()) {
      if (!metadata.empty()) {
        metadata += '\n';
      }
      metadata += data_.mission.location;
    }
    if (!data_.mission.area.empty()) {
      if (!metadata.empty()) {
        metadata += '\n';
      }
      metadata += data_.mission.area;
    }
    add(PauseRenderKind::text, PauseRect{56, 50, 157, 20}, metadata,
        PauseColorRole::muted);
    const auto page_count =
        std::max<std::size_t>(data_.mission.briefing_pages.size(), 1);
    const auto page = std::min(state.page, page_count - 1);
    const auto text =
        data_.mission.briefing_pages.empty()
            ? std::string_view{"No briefing data available"}
            : std::string_view{data_.mission.briefing_pages[page]};
    add(PauseRenderKind::text, PauseRect{56, 74, 157, 96}, text);
    auto &indicator =
        add(PauseRenderKind::page_indicator, PauseRect{176, 174, 37, 10});
    indicator.value = static_cast<std::int32_t>(page + 1);
    indicator.maximum = static_cast<std::int32_t>(page_count);
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        page + 1 < page_count ? "%x next   %t back" : "%t back");
    break;
  }
  case PauseScreen::weapons: {
    const auto count = data_.weapons.size();
    const auto selected = count == 0 ? 0 : std::min(state.selection, count - 1);
    if (state.expanded) {
      add(PauseRenderKind::panel, PauseAcdLayout::expanded_content, {},
          PauseColorRole::background);
      add(PauseRenderKind::panel, PauseAcdLayout::expanded_weapon_image_panel,
          {}, PauseColorRole::background);
      add(PauseRenderKind::panel,
          PauseAcdLayout::expanded_weapon_information_panel, {},
          PauseColorRole::background);
      const auto ratings_page = state.page == 0;
      add(PauseRenderKind::title, PauseRect{20, 14, 344, 12},
          ratings_page ? "Weapons - Ratings" : "Weapons - Description",
          PauseColorRole::accent);
      if (count != 0) {
        const auto &weapon = data_.weapons[selected];
        if (!weapon.icon_asset.empty()) {
          auto &asset =
              add(PauseRenderKind::asset, PauseRect{24, 42, 156, 154});
          asset.asset = weapon.icon_asset;
          asset.id = weapon.id;
        }
        std::string information = weapon.equipped ? "Equipped " : "Selected ";
        information += weapon.name + ".";
        if (weapon.maximum_ammo > 0) {
          information += "\nAmmo: " + std::to_string(weapon.ammo) + "/" +
                         std::to_string(weapon.maximum_ammo);
        }
        add(PauseRenderKind::text, PauseRect{200, 42, 158, 42}, information);
        if (ratings_page) {
          const auto add_rating = [&](std::int16_t y, std::string label,
                                      std::uint8_t value) {
            auto &rating =
                add(PauseRenderKind::slider, PauseRect{202, y, 154, 10});
            rating.text = std::move(label);
            rating.value = value;
            rating.maximum = 5;
          };
          add_rating(105, "Fire Rate", weapon.fire_rate);
          add_rating(137, "Power", weapon.damage);
          add_rating(169, "Accuracy", weapon.accuracy);
        } else {
          add(PauseRenderKind::text, PauseRect{200, 91, 158, 100},
              weapon.description.empty() ? "No description available"
                                         : weapon.description);
        }
        auto &indicator =
            add(PauseRenderKind::page_indicator, PauseRect{132, 184, 48, 10});
        indicator.value = static_cast<std::int32_t>(selected + 1);
        indicator.maximum = static_cast<std::int32_t>(count);
        auto &detail_indicator =
            add(PauseRenderKind::page_indicator, PauseRect{320, 184, 34, 10});
        detail_indicator.value = ratings_page ? 1 : 2;
        detail_indicator.maximum = 2;
      } else {
        add(PauseRenderKind::text, PauseRect{20, 72, 344, 20},
            "No weapons available");
      }
      add(PauseRenderKind::button_hint, PauseAcdLayout::expanded_hint,
          "%x equip   A/D page   W/S weapon   %t close");
      break;
    }

    add(PauseRenderKind::title, PauseRect{56, 36, 157, 10}, "Weapons",
        PauseColorRole::accent);
    if (count != 0) {
      const auto &weapon = data_.weapons[selected];
      if (!weapon.icon_asset.empty()) {
        auto &asset = add(PauseRenderKind::asset, PauseRect{238, 42, 105, 68});
        asset.asset = weapon.icon_asset;
        asset.id = weapon.id;
      }
      std::string information = weapon.equipped ? "Equipped " : "Selected ";
      information += weapon.name + ".";
      if (weapon.maximum_ammo > 0) {
        information += "\nAmmo: " + std::to_string(weapon.ammo) + "/" +
                       std::to_string(weapon.maximum_ammo);
      }
      information += "\nRate: " + std::to_string(weapon.fire_rate) + "/5";
      information += "\nDamage: " + std::to_string(weapon.damage) + "/5";
      add(PauseRenderKind::text, PauseRect{56, 52, 157, 74}, information);
      auto &indicator =
          add(PauseRenderKind::page_indicator, PauseRect{168, 176, 41, 10});
      indicator.value = static_cast<std::int32_t>(selected + 1);
      indicator.maximum = static_cast<std::int32_t>(count);
    } else {
      add(PauseRenderKind::text, PauseRect{56, 72, 157, 20},
          "No weapons available");
    }
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        "%x info   W/S   %t back");
    break;
  }
  case PauseScreen::objectives:
  case PauseScreen::parameters: {
    const auto is_objectives = state.screen == PauseScreen::objectives;
    add(PauseRenderKind::title, PauseRect{56, 36, 157, 10},
        is_objectives ? "Objectives" : "Parameters", PauseColorRole::accent);
    const auto &entries =
        is_objectives ? data_.mission.objectives : data_.mission.parameters;
    std::vector<const MissionMenuEntry *> visible;
    visible.reserve(entries.size());
    for (const auto &entry : entries) {
      if (entry.visible) {
        visible.push_back(&entry);
      }
    }
    const auto begin = std::min(state.page, visible.size());
    auto y = std::int16_t{51};
    constexpr auto content_bottom = static_cast<std::int16_t>(
        PauseAcdLayout::left_content.y + PauseAcdLayout::left_content.height);
    auto previous_state = MissionEntryState::failed;
    bool first = true;
    for (auto index = begin; index < visible.size(); ++index) {
      const auto &entry = *visible[index];
      const auto needs_heading = first || entry.state != previous_state;
      const auto required_height =
          static_cast<std::int16_t>((needs_heading ? 11 : 0) + 12);
      if (y + required_height > content_bottom) {
        break;
      }
      if (needs_heading) {
        add(PauseRenderKind::text, PauseRect{56, y, 157, 10},
            is_objectives ? objectiveHeading(entry.state)
                          : parameterHeading(entry.state),
            PauseColorRole::accent);
        y = static_cast<std::int16_t>(y + 11);
        previous_state = entry.state;
        first = false;
      }
      auto &line = add(PauseRenderKind::text, PauseRect{60, y, 153, 12},
                       retailListItem(entry.text), entryColor(entry.state));
      line.id = entry.id;
      y = static_cast<std::int16_t>(y + 14);
    }
    if (visible.empty()) {
      add(PauseRenderKind::text, PauseRect{60, 65, 153, 12}, "none",
          PauseColorRole::muted);
    }
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint, "%t back");
    break;
  }
  case PauseScreen::map: {
    const auto &map = data_.mission.map;
    if (!map.reconnaissance_available || map.layer_assets.empty()) {
      add(PauseRenderKind::text, PauseRect{56, 96, 157, 18},
          "No Reconnaissance", PauseColorRole::warning);
    } else {
      if (state.expanded) {
        add(PauseRenderKind::panel, PauseAcdLayout::expanded_map_panel, {},
            PauseColorRole::background);
        add(PauseRenderKind::panel, PauseAcdLayout::expanded_information_panel,
            {}, PauseColorRole::background);
      }
      const auto map_bounds = state.expanded
                                  ? PauseAcdLayout::expanded_map_image
                                  : PauseAcdLayout::map_image;
      auto &asset = add(PauseRenderKind::asset, map_bounds);
      asset.asset =
          map.layer_assets[std::min(state.page, map.layer_assets.size() - 1)];
      addMapMarkers(map, map_bounds, state.page, state.expanded ? 8 : 6);
      addMapInformation(data_,
                        state.expanded
                            ? PauseAcdLayout::expanded_information_content
                            : PauseAcdLayout::information_content,
                        state.page, state.expanded);
      if (state.expanded) {
        auto &indicator =
            add(PauseRenderKind::page_indicator, PauseRect{326, 196, 38, 10});
        indicator.value = static_cast<std::int32_t>(state.page + 1U);
        indicator.maximum = static_cast<std::int32_t>(map.layer_assets.size());
      }
    }
    add(PauseRenderKind::button_hint,
        state.expanded ? PauseAcdLayout::expanded_hint : PauseAcdLayout::hint,
        state.expanded
            ? (map.layer_assets.size() > 1 ? "A/D map   %t close" : "%t close")
            : (map.layer_assets.size() > 1 ? "A/D map   %x full   %t back"
                                           : "%x full   %t back"));
    break;
  }
  case PauseScreen::confirmation: {
    std::string_view question;
    switch (confirmation_action_) {
    case ConfirmationAction::restart_checkpoint:
      question = "Do you really want to restart at the last checkpoint?";
      break;
    case ConfirmationAction::restart_mission:
      question = "Do you really want to restart this mission?";
      break;
    case ConfirmationAction::quit_game:
      question = "Do you really want to abort this game?";
      break;
    case ConfirmationAction::none:
      break;
    }
    add(PauseRenderKind::dialog, PauseRect{56, 62, 157, 72}, question,
        PauseColorRole::warning);
    addMenu("Yes", 0, state.selection, 143);
    addMenu("No", 1, state.selection, 159);
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        "%x accept   %t cancel");
    break;
  }
  case PauseScreen::notification:
    add(PauseRenderKind::dialog, PauseRect{56, 66, 157, 88}, notification_,
        PauseColorRole::warning);
    add(PauseRenderKind::button_hint, PauseAcdLayout::hint,
        "Press %x to continue");
    break;
  }

  constexpr std::array section_labels{
      "Map", "Objectives", "Parameters", "Briefing", "Weapons", "Options",
  };
  if (state.screen != PauseScreen::root && !expanded_detail) {
    const auto has_information =
        std::ranges::any_of(commands, [](const auto &command) {
          return (command.kind == PauseRenderKind::title ||
                  command.kind == PauseRenderKind::text ||
                  command.kind == PauseRenderKind::asset) &&
                 command.bounds.x >= PauseAcdLayout::information_content.x &&
                 command.bounds.y >= PauseAcdLayout::information_content.y &&
                 command.bounds.y <
                     PauseAcdLayout::information_content.y +
                         PauseAcdLayout::information_content.height;
        });
    if (!has_information) {
      std::string information{pauseScreenName(state.screen)};
      if (!data_.mission.mission_name.empty()) {
        information += "\n" + data_.mission.mission_name;
      }
      if (!data_.mission.location.empty()) {
        information += "\n" + data_.mission.location;
      }
      add(PauseRenderKind::text, PauseRect{240, 44, 101, 60}, information);
    }

    const auto selected_section = sectionSelection();
    for (std::size_t index = 0; index < section_labels.size(); ++index) {
      auto &item =
          add(PauseRenderKind::menu_item,
              PauseAcdLayout::sectionSelection(index), section_labels[index],
              index == selected_section ? PauseColorRole::selected
                                        : PauseColorRole::normal);
      item.id = static_cast<std::uint32_t>(index);
      item.selected = index == selected_section;
    }
  }

  // Carry semantic placement to the platform renderer. This prevents detail
  // screens from silently falling back to a full-screen panel and makes font
  // alignment deterministic across render backends.
  for (auto &command : commands) {
    switch (command.kind) {
    case PauseRenderKind::dim_background:
      break;
    case PauseRenderKind::panel:
      break;
    case PauseRenderKind::button_hint:
      if (!expanded_detail) {
        command.bounds = PauseAcdLayout::hint;
        command.panel = PausePanelRole::hint;
      }
      command.alignment = PauseTextAlignment::center;
      break;
    case PauseRenderKind::menu_item:
      command.panel = command.bounds.x >= PauseAcdLayout::section_menu.x
                          ? PausePanelRole::right_sections
                          : PausePanelRole::left_content;
      command.alignment = PauseTextAlignment::center;
      break;
    default:
      if (!expanded_detail) {
        command.panel =
            command.bounds.x >= PauseAcdLayout::information_content.x
                ? PausePanelRole::right_information
                : PausePanelRole::left_content;
      }
      if (command.kind == PauseRenderKind::page_indicator) {
        command.alignment = PauseTextAlignment::center;
      }
      break;
    }
  }

  return commands;
}

std::string_view pauseScreenName(PauseScreen screen) noexcept {
  switch (screen) {
  case PauseScreen::root:
    return "Pause";
  case PauseScreen::briefing:
    return "Briefing";
  case PauseScreen::options:
    return "Options";
  case PauseScreen::sound:
    return "Sound";
  case PauseScreen::controller:
    return "Controller";
  case PauseScreen::controller_bindings:
    return "Controller Configuration";
  case PauseScreen::brightness:
    return "Game Brightness";
  case PauseScreen::screen_centering:
    return "Screen Centering";
  case PauseScreen::mission_select:
    return "Select Mission";
  case PauseScreen::weapons:
    return "Weapons";
  case PauseScreen::parameters:
    return "Parameters";
  case PauseScreen::objectives:
    return "Objectives";
  case PauseScreen::map:
    return "Map";
  case PauseScreen::confirmation:
    return "Confirmation";
  case PauseScreen::notification:
    return "Notification";
  }
  return "Unknown";
}

std::string_view controllerActionName(ControllerAction action) noexcept {
  switch (action) {
  case ControllerAction::change_weapon:
    return "Change Weapon";
  case ControllerAction::shoot:
    return "Shoot";
  case ControllerAction::kneel:
    return "Kneel";
  case ControllerAction::roll_zoom_out:
    return "Roll/Zoom Out";
  case ControllerAction::step_right:
    return "Step Right";
  case ControllerAction::step_left:
    return "Step Left";
  case ControllerAction::target_lock:
    return "Target Lock";
  case ControllerAction::use_zoom_in:
    return "Use/Zoom In";
  case ControllerAction::aim:
    return "Aim";
  }
  return "Unknown";
}

std::string_view controllerPresetName(ControllerPreset preset) noexcept {
  switch (preset) {
  case ControllerPreset::standard:
    return "Standard";
  case ControllerPreset::alternate:
    return "Alternate";
  case ControllerPreset::custom:
    return "Custom";
  }
  return "Unknown";
}

void applyControllerPreset(PauseSettings &settings, ControllerPreset preset) {
  const auto *bindings = &retail_standard_bindings;
  switch (preset) {
  case ControllerPreset::standard:
    break;
  case ControllerPreset::alternate:
    bindings = &retail_alternate_bindings;
    break;
  case ControllerPreset::custom:
    settings.controller_preset = preset;
    return;
  }
  settings.controller_preset = preset;
  settings.bindings.assign(bindings->begin(), bindings->end());
}

std::uint32_t controllerButtonForAction(const PauseSettings &settings,
                                        ControllerAction action) noexcept {
  const auto binding =
      std::find_if(settings.bindings.begin(), settings.bindings.end(),
                   [action](const ControllerBinding &value) {
                     return value.action == action;
                   });
  return binding == settings.bindings.end() ? 0U : binding->button;
}

} // namespace sf::game
