#include "sf/game/controller_bindings.hpp"

#include <algorithm>
#include <bit>

namespace sf::game {
namespace {

constexpr std::array<ControllerActionMetadata, controller_action_count>
    controller_action_catalog{{
        {ControllerAction::change_weapon, "Change Weapon", "ChangeWeapon",
         controller_select_button, controller_r1_button},
        {ControllerAction::shoot, "Shoot", "Shoot", controller_square_button,
         controller_circle_button},
        {ControllerAction::kneel, "Kneel", "Kneel", controller_cross_button,
         controller_r2_button},
        {ControllerAction::roll_zoom_out, "Roll/Zoom Out", "RollZoomOut",
         controller_circle_button, controller_triangle_button},
        {ControllerAction::step_right, "Step Right", "StepRight",
         controller_r2_button, controller_l2_button},
        {ControllerAction::step_left, "Step Left", "StepLeft",
         controller_l2_button, controller_square_button},
        {ControllerAction::target_lock, "Target Lock", "TargetLock",
         controller_r1_button, controller_cross_button},
        {ControllerAction::use_zoom_in, "Use/Zoom In", "UseZoomIn",
         controller_triangle_button, controller_select_button},
        {ControllerAction::aim, "Aim", "Aim", controller_l1_button,
         controller_l1_button},
    }};

constexpr ControllerActionMetadata invalid_controller_action{
    ControllerAction::change_weapon, "Unknown", "Unknown", 0U, 0U};

} // namespace

const std::array<ControllerActionMetadata, controller_action_count> &
controllerActionCatalog() noexcept {
  return controller_action_catalog;
}

bool isValidControllerAction(ControllerAction action) noexcept {
  return static_cast<std::size_t>(action) < controller_action_count;
}

const ControllerActionMetadata &
controllerActionMetadata(ControllerAction action) noexcept {
  if (!isValidControllerAction(action)) {
    return invalid_controller_action;
  }
  return controller_action_catalog[static_cast<std::size_t>(action)];
}

std::string_view controllerActionName(ControllerAction action) noexcept {
  return controllerActionMetadata(action).name;
}

std::string_view controllerActionConfigKey(ControllerAction action) noexcept {
  return controllerActionMetadata(action).config_key;
}

bool isBindableControllerButton(std::uint32_t button) noexcept {
  return std::has_single_bit(button) &&
         (button & bindable_controller_button_mask) == button;
}

bool areControllerBindingsValid(
    const ControllerButtonBindings &bindings) noexcept {
  std::uint32_t assigned_buttons{};
  for (const auto button : bindings.buttons) {
    if (!isBindableControllerButton(button) ||
        (assigned_buttons & button) != 0U) {
      return false;
    }
    assigned_buttons |= button;
  }
  return true;
}

ControllerButtonBindings
controllerBindingsForPreset(ControllerBindingPreset preset) noexcept {
  auto bindings = ControllerButtonBindings{};
  for (const auto &metadata : controller_action_catalog) {
    bindings[metadata.action] = preset == ControllerBindingPreset::alternate
                                    ? metadata.alternate_button
                                    : metadata.standard_button;
  }
  return bindings;
}

std::uint32_t
controllerButtonForAction(const ControllerButtonBindings &bindings,
                          ControllerAction action) noexcept {
  return isValidControllerAction(action) ? bindings[action] : 0U;
}

ControllerRebindResult
rebindControllerButton(ControllerButtonBindings &bindings,
                       ControllerAction action, std::uint32_t button) noexcept {
  if (!isValidControllerAction(action) ||
      !isBindableControllerButton(button)) {
    return ControllerRebindResult::invalid;
  }

  auto &current = bindings[action];
  if (current == button) {
    return ControllerRebindResult::unchanged;
  }
  const auto previous = current;
  const auto conflict =
      std::find(bindings.buttons.begin(), bindings.buttons.end(), button);
  current = button;
  if (conflict == bindings.buttons.end()) {
    return ControllerRebindResult::assigned;
  }
  *conflict = previous;
  return ControllerRebindResult::swapped;
}

std::array<ControllerBinding, controller_action_count>
controllerBindingEntries(const ControllerButtonBindings &bindings) noexcept {
  auto entries = std::array<ControllerBinding, controller_action_count>{};
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    entries[index] = ControllerBinding{static_cast<ControllerAction>(index),
                                       bindings.buttons[index]};
  }
  return entries;
}

std::optional<ControllerButtonBindings>
controllerBindingsFromEntries(
    std::span<const ControllerBinding> entries) noexcept {
  if (entries.size() != controller_action_count) {
    return std::nullopt;
  }
  auto bindings = ControllerButtonBindings{};
  auto seen_actions = std::array<bool, controller_action_count>{};
  for (const auto &entry : entries) {
    if (!isValidControllerAction(entry.action)) {
      return std::nullopt;
    }
    const auto index = static_cast<std::size_t>(entry.action);
    if (seen_actions[index]) {
      return std::nullopt;
    }
    seen_actions[index] = true;
    bindings[entry.action] = entry.button;
  }
  return areControllerBindingsValid(bindings)
             ? std::optional<ControllerButtonBindings>{bindings}
             : std::nullopt;
}

} // namespace sf::game
