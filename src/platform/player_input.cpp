#include "sf/platform/player_input.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string>

namespace sf::platform {
namespace {

struct KeyboardMouseActionDescriptor {
  std::string_view name;
  std::string_view config_key;
  KeyboardMouseInput default_input;
};

constexpr std::array<KeyboardMouseActionDescriptor, keyboard_mouse_action_count>
    keyboard_mouse_actions{{
        {"Move Forward", "MoveForward", KeyboardMouseInput::w},
        {"Move Backward", "MoveBackward", KeyboardMouseInput::s},
        {"Turn Left", "TurnLeft", KeyboardMouseInput::a},
        {"Turn Right", "TurnRight", KeyboardMouseInput::d},
        {"Strafe Left", "StrafeLeft", KeyboardMouseInput::q},
        {"Strafe Right", "StrafeRight", KeyboardMouseInput::e},
        {"Run", "Run", KeyboardMouseInput::left_shift},
        {"Roll", "Roll", KeyboardMouseInput::space},
        {"Reload", "Reload", KeyboardMouseInput::r},
        {"Aim", "Aim", KeyboardMouseInput::mouse_right},
        {"Fire", "Fire", KeyboardMouseInput::mouse_left},
        {"Crouch / Stealth", "Crouch", KeyboardMouseInput::c},
        {"Action / Interact", "Interact", KeyboardMouseInput::f},
        {"Target Lock", "TargetLock", KeyboardMouseInput::tab},
        {"Quick Turn", "QuickTurn", KeyboardMouseInput::backspace},
        {"Quick Weapon Switch", "QuickWeapon",
         KeyboardMouseInput::mouse_middle},
        {"Previous Weapon", "PreviousWeapon", KeyboardMouseInput::left_bracket},
        {"Next Weapon", "NextWeapon", KeyboardMouseInput::right_bracket},
        {"Weapon Menu Previous", "WeaponMenuPrevious",
         KeyboardMouseInput::mouse_wheel_down},
        {"Weapon Menu Next", "WeaponMenuNext",
         KeyboardMouseInput::mouse_wheel_up},
        {"Pause Menu", "Pause", KeyboardMouseInput::escape},
        {"Quick Weapon 1", "QuickWeapon1", KeyboardMouseInput::digit_1},
        {"Quick Weapon 2", "QuickWeapon2", KeyboardMouseInput::digit_2},
        {"Quick Weapon 3", "QuickWeapon3", KeyboardMouseInput::digit_3},
        {"Quick Weapon 4", "QuickWeapon4", KeyboardMouseInput::digit_4},
        {"Quick Weapon 5", "QuickWeapon5", KeyboardMouseInput::digit_5},
        {"Quick Weapon 6", "QuickWeapon6", KeyboardMouseInput::digit_6},
        {"Quick Weapon 7", "QuickWeapon7", KeyboardMouseInput::digit_7},
        {"Quick Weapon 8", "QuickWeapon8", KeyboardMouseInput::digit_8},
        {"Quick Weapon 9", "QuickWeapon9", KeyboardMouseInput::digit_9},
        {"Quick Weapon 10", "QuickWeapon10", KeyboardMouseInput::digit_0},
    }};

constexpr std::array<std::string_view, 26U> letter_names{
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
};
constexpr std::array<std::string_view, 10U> digit_names{
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
};
constexpr std::array<std::string_view, 24U> function_names{
    "F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",
    "F9",  "F10", "F11", "F12", "F13", "F14", "F15", "F16",
    "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24",
};
constexpr std::array<std::string_view, 10U> keypad_digit_names{
    "Numpad 1", "Numpad 2", "Numpad 3", "Numpad 4", "Numpad 5",
    "Numpad 6", "Numpad 7", "Numpad 8", "Numpad 9", "Numpad 0",
};

double finiteOrZero(double value) noexcept {
  return std::isfinite(value) ? value : 0.0;
}

double nonNegativeFinite(double value) noexcept {
  return std::max(0.0, finiteOrZero(value));
}

double sanitizedDeadzone(double value) noexcept {
  return std::clamp(finiteOrZero(value), 0.0, 0.95);
}

PlayerInputConfiguration sanitized(PlayerInputConfiguration value) noexcept {
  value.movement_deadzone = sanitizedDeadzone(value.movement_deadzone);
  value.look_deadzone = sanitizedDeadzone(value.look_deadzone);
  value.mouse_yaw_sensitivity = nonNegativeFinite(value.mouse_yaw_sensitivity);
  value.mouse_pitch_sensitivity =
      nonNegativeFinite(value.mouse_pitch_sensitivity);
  value.controller_yaw_sensitivity =
      nonNegativeFinite(value.controller_yaw_sensitivity);
  value.controller_pitch_sensitivity =
      nonNegativeFinite(value.controller_pitch_sensitivity);
  return value;
}

double deadzoneAxis(double value, double deadzone) noexcept {
  value = std::clamp(finiteOrZero(value), -1.0, 1.0);
  const auto magnitude = std::abs(value);
  if (magnitude <= deadzone) {
    return 0.0;
  }
  const auto normalized = (magnitude - deadzone) / (1.0 - deadzone);
  return std::copysign(normalized, value);
}

double digitalAxis(bool positive, bool negative) noexcept {
  if (positive == negative) {
    return 0.0;
  }
  return positive ? 1.0 : -1.0;
}

double digitalOrAnalog(double digital, double analog) noexcept {
  return digital != 0.0 ? digital : analog;
}

PlayerActionState actionState(bool held, bool previously_held,
                              bool pulse = false) noexcept {
  return PlayerActionState{
      held,
      pulse || (held && !previously_held),
      !held && previously_held,
  };
}

bool keyboardMouseInputDown(KeyboardMouseInput input,
                            const KeyboardMouseDeviceState &device) noexcept {
  if (isKeyboardInput(input)) {
    const auto scancode = static_cast<std::size_t>(input);
    return scancode < device.keyboard.size() && device.keyboard[scancode] != 0U;
  }
  switch (input) {
  case KeyboardMouseInput::mouse_left:
    return device.mouse_left;
  case KeyboardMouseInput::mouse_right:
    return device.mouse_right;
  case KeyboardMouseInput::mouse_middle:
    return device.mouse_middle;
  case KeyboardMouseInput::mouse_x1:
    return device.mouse_x1;
  case KeyboardMouseInput::mouse_x2:
    return device.mouse_x2;
  case KeyboardMouseInput::mouse_wheel_up:
    return device.mouse_wheel_delta > 0;
  case KeyboardMouseInput::mouse_wheel_down:
    return device.mouse_wheel_delta < 0;
  default:
    return false;
  }
}

std::int32_t mouseWheelActivationCount(KeyboardMouseInput input,
                                       std::int32_t wheel_delta) noexcept {
  if (input == KeyboardMouseInput::mouse_wheel_up && wheel_delta > 0) {
    return wheel_delta;
  }
  if (input == KeyboardMouseInput::mouse_wheel_down && wheel_delta < 0) {
    return wheel_delta == std::numeric_limits<std::int32_t>::min()
               ? std::numeric_limits<std::int32_t>::max()
               : -wheel_delta;
  }
  return 0;
}

char asciiLower(char value) noexcept {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool equalsIgnoringAsciiCase(std::string_view first,
                             std::string_view second) noexcept {
  return first.size() == second.size() &&
         std::ranges::equal(first, second, [](char left, char right) {
           return asciiLower(left) == asciiLower(right);
         });
}

bool startsWithIgnoringAsciiCase(std::string_view text,
                                 std::string_view prefix) noexcept {
  return text.size() >= prefix.size() &&
         equalsIgnoringAsciiCase(text.substr(0U, prefix.size()), prefix);
}

std::size_t findIgnoringAsciiCase(std::string_view text,
                                  std::string_view needle,
                                  std::size_t offset = 0U) noexcept {
  if (needle.empty()) {
    return std::min(offset, text.size());
  }
  for (auto index = offset; index + needle.size() <= text.size(); ++index) {
    if (equalsIgnoringAsciiCase(text.substr(index, needle.size()), needle)) {
      return index;
    }
  }
  return std::string_view::npos;
}

std::string hudPromptName(std::string_view source) {
  std::string name{source};
  if (name.empty()) {
    name = "Unbound";
  }
  std::ranges::replace(name, '\t', ' ');
  std::ranges::transform(name, name.begin(), [](const char value) {
    const auto raw = static_cast<unsigned char>(value);
    return raw >= static_cast<unsigned char>('a') &&
                   raw <= static_cast<unsigned char>('z')
               ? static_cast<char>(raw - static_cast<unsigned char>('a') +
                                   static_cast<unsigned char>('A'))
               : value;
  });
  return name;
}

std::string normalizedRetailPrompt(std::string_view source) {
  std::string result{source};
  for (auto offset = std::size_t{};;) {
    const auto token = findIgnoringAsciiCase(result, "%x", offset);
    if (token == std::string::npos) {
      break;
    }
    result.replace(token, 2U, "X");
    offset = token + 1U;
  }
  return result;
}

std::string hudInputName(KeyboardMouseInput input) {
  // FONTA does not contain these punctuation glyphs. Spell the handful of
  // affected inputs out so every launcher-supported binding remains visible.
  std::string name;
  switch (input) {
  case KeyboardMouseInput::left_bracket:
    name = "Left Bracket";
    break;
  case KeyboardMouseInput::right_bracket:
    name = "Right Bracket";
    break;
  case KeyboardMouseInput::equals:
    name = "Equals";
    break;
  case KeyboardMouseInput::semicolon:
    name = "Semicolon";
    break;
  case KeyboardMouseInput::grave:
    name = "Grave";
    break;
  case KeyboardMouseInput::non_us_hash:
    name = "Non-US Hash";
    break;
  case KeyboardMouseInput::keypad_multiply:
    name = "Numpad Multiply";
    break;
  case KeyboardMouseInput::keypad_plus:
    name = "Numpad Plus";
    break;
  case KeyboardMouseInput::keypad_equals:
    name = "Numpad Equals";
    break;
  default:
    name = keyboardMouseInputName(input);
    break;
  }

  // The ViT one-byte font map deliberately reuses `a..z` for Cyrillic.
  // Keep dynamic PC binding labels in the atlas' untouched `A..Z` cells;
  // otherwise a mixed-case name such as "Escape" renders its lower-case
  // `s` through the Cyrillic slot and appears as "E\u041bCAPE". This mapping is
  // presentation-only: launcher labels and persisted binding names retain
  // their normal title case.
  return hudPromptName(name);
}

} // namespace

int controllerMenuDirection(std::uint8_t horizontal, std::uint8_t vertical,
                            int previous_direction) noexcept {
  constexpr int center = 128;
  constexpr int engage_deadzone = 56;
  constexpr int release_deadzone = 40;
  const auto x = static_cast<int>(horizontal) - center;
  const auto y = static_cast<int>(vertical) - center;
  if (previous_direction != 0) {
    if (std::abs(x) <= release_deadzone && std::abs(y) <= release_deadzone) {
      return 0;
    }
    return previous_direction < 0 ? -1 : 1;
  }
  const auto dominant = std::abs(y) >= std::abs(x) ? y : x;
  if (std::abs(dominant) <= engage_deadzone) {
    return 0;
  }
  return dominant < 0 ? -1 : 1;
}

std::uint8_t dominantControllerMenuAxis(
    std::uint8_t right_horizontal, std::uint8_t right_vertical,
    std::uint8_t left_horizontal, std::uint8_t left_vertical) noexcept {
  constexpr int center = 128;
  // Prefer Y on ties so ordinary list navigation remains intuitive, but
  // accept X as well: the retail title code maps every D-pad direction to
  // previous/next and controller axis layouts are not part of save data.
  const std::array candidates{left_vertical, right_vertical, left_horizontal,
                              right_horizontal};
  auto selected = std::uint8_t{center};
  auto selected_magnitude = 0;
  for (const auto candidate : candidates) {
    const auto magnitude = std::abs(static_cast<int>(candidate) - center);
    if (magnitude > selected_magnitude) {
      selected = candidate;
      selected_magnitude = magnitude;
    }
  }
  return selected;
}

ControllerMenuStep
ControllerMenuNavigator::update(ControllerMenuSample sample) noexcept {
  if (!sample.connected) {
    reset();
    return {};
  }

  if (!initialized_ || instance_id_ != sample.instance_id) {
    initialized_ = true;
    instance_id_ = sample.instance_id;
    direction_ = controllerMenuDirection(sample.horizontal, sample.vertical, 0);
    return {};
  }

  const auto previous_direction = direction_;
  direction_ = controllerMenuDirection(sample.horizontal, sample.vertical,
                                       previous_direction);
  if (previous_direction != 0 || direction_ == 0) {
    return {};
  }
  return ControllerMenuStep{.previous = direction_ < 0, .next = direction_ > 0};
}

void ControllerMenuNavigator::reset() noexcept {
  instance_id_ = -1;
  direction_ = 0;
  initialized_ = false;
}

KeyboardMouseBindings defaultKeyboardMouseBindings() noexcept {
  KeyboardMouseBindings result;
  for (std::size_t index = 0U; index < keyboard_mouse_actions.size(); ++index) {
    result.values[index] = keyboard_mouse_actions[index].default_input;
  }
  return result;
}

bool isKeyboardInput(KeyboardMouseInput input) noexcept {
  const auto value = static_cast<std::uint16_t>(input);
  return (value >= static_cast<std::uint16_t>(KeyboardMouseInput::a) &&
          value <=
              static_cast<std::uint16_t>(KeyboardMouseInput::application)) ||
         (value >=
              static_cast<std::uint16_t>(KeyboardMouseInput::keypad_equals) &&
          value <= static_cast<std::uint16_t>(KeyboardMouseInput::f24)) ||
         (value >=
              static_cast<std::uint16_t>(KeyboardMouseInput::left_control) &&
          value <= static_cast<std::uint16_t>(KeyboardMouseInput::right_gui));
}

bool isValidKeyboardMouseInput(KeyboardMouseInput input) noexcept {
  const auto value = static_cast<std::uint16_t>(input);
  return input == KeyboardMouseInput::none || isKeyboardInput(input) ||
         (value >= static_cast<std::uint16_t>(KeyboardMouseInput::mouse_left) &&
          value <=
              static_cast<std::uint16_t>(KeyboardMouseInput::mouse_wheel_down));
}

std::string_view keyboardMouseInputName(KeyboardMouseInput input) noexcept {
  const auto value = static_cast<std::uint16_t>(input);
  const auto letter_begin = static_cast<std::uint16_t>(KeyboardMouseInput::a);
  const auto digit_begin =
      static_cast<std::uint16_t>(KeyboardMouseInput::digit_1);
  const auto f1_value = static_cast<std::uint16_t>(KeyboardMouseInput::f1);
  const auto f13_value = static_cast<std::uint16_t>(KeyboardMouseInput::f13);
  const auto keypad_begin =
      static_cast<std::uint16_t>(KeyboardMouseInput::keypad_1);
  if (value >= letter_begin && value < letter_begin + letter_names.size()) {
    return letter_names[value - letter_begin];
  }
  if (value >= digit_begin && value < digit_begin + digit_names.size()) {
    return digit_names[value - digit_begin];
  }
  if (value >= f1_value && value < f1_value + 12U) {
    return function_names[value - f1_value];
  }
  if (value >= f13_value && value < f13_value + 12U) {
    return function_names[12U + value - f13_value];
  }
  if (value >= keypad_begin &&
      value < keypad_begin + keypad_digit_names.size()) {
    return keypad_digit_names[value - keypad_begin];
  }
  switch (input) {
  case KeyboardMouseInput::none:
    return "Unbound";
  case KeyboardMouseInput::enter:
    return "Enter";
  case KeyboardMouseInput::escape:
    return "Escape";
  case KeyboardMouseInput::backspace:
    return "Backspace";
  case KeyboardMouseInput::tab:
    return "Tab";
  case KeyboardMouseInput::space:
    return "Space";
  case KeyboardMouseInput::minus:
    return "-";
  case KeyboardMouseInput::equals:
    return "=";
  case KeyboardMouseInput::left_bracket:
    return "[";
  case KeyboardMouseInput::right_bracket:
    return "]";
  case KeyboardMouseInput::backslash:
    return "Backslash";
  case KeyboardMouseInput::non_us_hash:
    return "Non-US #";
  case KeyboardMouseInput::semicolon:
    return ";";
  case KeyboardMouseInput::apostrophe:
    return "'";
  case KeyboardMouseInput::grave:
    return "`";
  case KeyboardMouseInput::comma:
    return ",";
  case KeyboardMouseInput::period:
    return ".";
  case KeyboardMouseInput::slash:
    return "/";
  case KeyboardMouseInput::caps_lock:
    return "Caps Lock";
  case KeyboardMouseInput::print_screen:
    return "Print Screen";
  case KeyboardMouseInput::scroll_lock:
    return "Scroll Lock";
  case KeyboardMouseInput::pause:
    return "Pause";
  case KeyboardMouseInput::insert:
    return "Insert";
  case KeyboardMouseInput::home:
    return "Home";
  case KeyboardMouseInput::page_up:
    return "Page Up";
  case KeyboardMouseInput::delete_key:
    return "Delete";
  case KeyboardMouseInput::end:
    return "End";
  case KeyboardMouseInput::page_down:
    return "Page Down";
  case KeyboardMouseInput::right:
    return "Right Arrow";
  case KeyboardMouseInput::left:
    return "Left Arrow";
  case KeyboardMouseInput::down:
    return "Down Arrow";
  case KeyboardMouseInput::up:
    return "Up Arrow";
  case KeyboardMouseInput::num_lock:
    return "Num Lock";
  case KeyboardMouseInput::keypad_divide:
    return "Numpad /";
  case KeyboardMouseInput::keypad_multiply:
    return "Numpad *";
  case KeyboardMouseInput::keypad_minus:
    return "Numpad -";
  case KeyboardMouseInput::keypad_plus:
    return "Numpad +";
  case KeyboardMouseInput::keypad_enter:
    return "Numpad Enter";
  case KeyboardMouseInput::keypad_period:
    return "Numpad .";
  case KeyboardMouseInput::non_us_backslash:
    return "Non-US Backslash";
  case KeyboardMouseInput::application:
    return "Menu";
  case KeyboardMouseInput::keypad_equals:
    return "Numpad =";
  case KeyboardMouseInput::left_control:
    return "Left Ctrl";
  case KeyboardMouseInput::left_shift:
    return "Left Shift";
  case KeyboardMouseInput::left_alt:
    return "Left Alt";
  case KeyboardMouseInput::left_gui:
    return "Left Win";
  case KeyboardMouseInput::right_control:
    return "Right Ctrl";
  case KeyboardMouseInput::right_shift:
    return "Right Shift";
  case KeyboardMouseInput::right_alt:
    return "Right Alt";
  case KeyboardMouseInput::right_gui:
    return "Right Win";
  case KeyboardMouseInput::mouse_left:
    return "Mouse Left";
  case KeyboardMouseInput::mouse_right:
    return "Mouse Right";
  case KeyboardMouseInput::mouse_middle:
    return "Mouse Middle";
  case KeyboardMouseInput::mouse_x1:
    return "Mouse X1";
  case KeyboardMouseInput::mouse_x2:
    return "Mouse X2";
  case KeyboardMouseInput::mouse_wheel_up:
    return "Mouse Wheel Up";
  case KeyboardMouseInput::mouse_wheel_down:
    return "Mouse Wheel Down";
  default:
    return {};
  }
}

std::string_view keyboardMouseActionName(KeyboardMouseAction action) noexcept {
  const auto index = static_cast<std::size_t>(action);
  return index < keyboard_mouse_actions.size()
             ? keyboard_mouse_actions[index].name
             : std::string_view{};
}

std::string_view
keyboardMouseActionConfigKey(KeyboardMouseAction action) noexcept {
  const auto index = static_cast<std::size_t>(action);
  return index < keyboard_mouse_actions.size()
             ? keyboard_mouse_actions[index].config_key
             : std::string_view{};
}

std::string_view
controllerButtonPromptName(ControllerPromptFamily family,
                           std::uint16_t ps1_active_high_bit) noexcept {
  static constexpr std::array<std::string_view, 16U> generic_names{
      "BUTTON 7",     "BUTTON 9",      "BUTTON 10", "BUTTON 8",
      "DPAD UP",      "DPAD RIGHT",    "DPAD DOWN", "DPAD LEFT",
      "LEFT TRIGGER", "RIGHT TRIGGER", "BUTTON 5",  "BUTTON 6",
      "BUTTON 4",     "BUTTON 2",      "BUTTON 1",  "BUTTON 3",
  };
  static constexpr std::array<std::string_view, 16U> xbox_names{
      "VIEW",      "LEFT STICK", "RIGHT STICK", "MENU", "DPAD UP", "DPAD RIGHT",
      "DPAD DOWN", "DPAD LEFT",  "LT",          "RT",   "LB",      "RB",
      "Y",         "B",          "A",           "X",
  };
  static constexpr std::array<std::string_view, 16U> playstation_names{
      "SHARE",     "L3",        "R3",    "OPTIONS", "DPAD UP", "DPAD RIGHT",
      "DPAD DOWN", "DPAD LEFT", "L2",    "R2",      "L1",      "R1",
      "TRIANGLE",  "CIRCLE",    "CROSS", "SQUARE",
  };
  static constexpr std::array<std::string_view, 16U> nintendo_names{
      "MINUS",     "LEFT STICK", "RIGHT STICK", "PLUS", "DPAD UP", "DPAD RIGHT",
      "DPAD DOWN", "DPAD LEFT",  "ZL",          "ZR",   "L",       "R",
      "X",         "A",          "B",           "Y",
  };

  if (ps1_active_high_bit == 0U ||
      (ps1_active_high_bit & (ps1_active_high_bit - 1U)) != 0U) {
    return "UNBOUND";
  }
  auto index = std::size_t{};
  for (auto remaining = ps1_active_high_bit; remaining > 1U;
       remaining = static_cast<std::uint16_t>(remaining >> 1U)) {
    ++index;
  }
  switch (family) {
  case ControllerPromptFamily::xbox:
    return xbox_names[index];
  case ControllerPromptFamily::playstation:
    return playstation_names[index];
  case ControllerPromptFamily::nintendo:
    return nintendo_names[index];
  case ControllerPromptFamily::generic:
  default:
    return generic_names[index];
  }
}
InputPromptBindings
keyboardMouseInputPromptBindings(const KeyboardMouseBindings &bindings) {
  InputPromptBindings result;
  const auto set = [&](InputPromptAction destination,
                       KeyboardMouseAction source) {
    result.values[static_cast<std::size_t>(destination)] =
        hudInputName(bindings[source]);
  };
  set(InputPromptAction::confirm, KeyboardMouseAction::interact);
  set(InputPromptAction::cancel, KeyboardMouseAction::pause);
  set(InputPromptAction::pause, KeyboardMouseAction::pause);
  set(InputPromptAction::interact, KeyboardMouseAction::interact);
  set(InputPromptAction::fire, KeyboardMouseAction::fire);
  return result;
}

InputPromptBindings
controllerInputPromptBindings(ControllerInputProtocol protocol,
                              InputPromptBindingNames names) {
  InputPromptBindings result;
  result.device = InputPromptDevice::controller;
  result.controller_protocol = protocol;
  const std::array<std::string_view, input_prompt_action_count> labels{{
      names.confirm,
      names.cancel,
      names.pause,
      names.interact,
      names.fire,
  }};
  for (std::size_t index = 0U; index < labels.size(); ++index) {
    result.values[index] = hudPromptName(labels[index]);
  }
  return result;
}

InputPromptBindings retailMenuControllerInputPromptBindings(
    ControllerPromptFamily family) noexcept {
  constexpr std::uint16_t start_button = 0x0008U;
  constexpr std::uint16_t circle_button = 0x2000U;
  constexpr std::uint16_t cross_button = 0x4000U;
  constexpr std::uint16_t square_button = 0x8000U;
  const auto name = [family](std::uint16_t button) {
    return controllerButtonPromptName(family, button);
  };
  return controllerInputPromptBindings(
      ControllerInputProtocol::unknown,
      InputPromptBindingNames{.confirm = name(cross_button),
                              .cancel = name(circle_button),
                              .pause = name(start_button),
                              .interact = name(cross_button),
                              .fire = name(square_button)});
}

std::string_view inputPromptLabel(const InputPromptBindings &bindings,
                                  InputPromptAction action) noexcept {
  const auto label = bindings[action];
  return label.empty() ? std::string_view{"UNBOUND"} : label;
}

std::optional<InputPromptText>
inputPromptText(std::string_view source, const InputPromptBindings &bindings) {
  constexpr std::string_view press_prefix = "Press ";
  constexpr std::string_view contact_suffix = " to Contact ";
  auto retail = normalizedRetailPrompt(source);
  if (!startsWithIgnoringAsciiCase(retail, press_prefix)) {
    return std::nullopt;
  }

  auto action = InputPromptAction::confirm;
  const auto token_begin = press_prefix.size();
  auto token_end = std::string::npos;
  if (const auto contact =
          findIgnoringAsciiCase(retail, contact_suffix, token_begin);
      contact != std::string::npos && contact > token_begin) {
    action = InputPromptAction::interact;
    token_end = contact;
  } else if (startsWithIgnoringAsciiCase(
                 std::string_view{retail}.substr(token_begin), "START") &&
             (retail.size() == token_begin + 5U ||
              std::isspace(
                  static_cast<unsigned char>(retail[token_begin + 5U])))) {
    action = InputPromptAction::pause;
    token_end = token_begin + 5U;
  } else if (startsWithIgnoringAsciiCase(
                 std::string_view{retail}.substr(token_begin), "CROSS") &&
             (retail.size() == token_begin + 5U ||
              std::isspace(
                  static_cast<unsigned char>(retail[token_begin + 5U])))) {
    token_end = token_begin + 5U;
  } else if (startsWithIgnoringAsciiCase(
                 std::string_view{retail}.substr(token_begin), "X") &&
             (retail.size() == token_begin + 1U ||
              std::isspace(
                  static_cast<unsigned char>(retail[token_begin + 1U])))) {
    token_end = token_begin + 1U;
  }
  if (token_end == std::string::npos) {
    return std::nullopt;
  }

  auto bound = retail;
  bound.replace(token_begin, token_end - token_begin,
                inputPromptLabel(bindings, action));
  return InputPromptText{std::move(retail), std::move(bound), action};
}

std::string inputHintText(std::string_view source,
                          const InputPromptBindings &bindings,
                          InputPromptTokenActions tokens) {
  std::string result;
  result.reserve(source.size());
  for (auto index = std::size_t{}; index < source.size(); ++index) {
    if (source[index] == '%' && index + 1U < source.size()) {
      const auto token = asciiLower(source[index + 1U]);
      const auto action = token == 'x'   ? std::optional{tokens.x}
                          : token == 't' ? std::optional{tokens.t}
                                         : std::nullopt;
      if (action) {
        result.append(inputPromptLabel(bindings, *action));
        ++index;
        continue;
      }
    }
    result.push_back(source[index] == '\t' ? ' ' : source[index]);
  }
  return result;
}

std::optional<KeyboardMousePromptText>
keyboardMousePromptText(std::string_view source,
                        const KeyboardMouseBindings &bindings) {
  return inputPromptText(source, keyboardMouseInputPromptBindings(bindings));
}

std::string keyboardMouseHintText(std::string_view source,
                                  const KeyboardMouseBindings &bindings) {
  return inputHintText(source, keyboardMouseInputPromptBindings(bindings),
                       InputPromptTokenActions{.x = InputPromptAction::interact,
                                               .t = InputPromptAction::pause});
}

KeyboardMouseActionSnapshot
sampleKeyboardMouseActions(const KeyboardMouseBindings &bindings,
                           const KeyboardMouseDeviceState &device) noexcept {
  KeyboardMouseActionSnapshot result;
  for (std::size_t index = 0U; index < keyboard_mouse_action_count; ++index) {
    const auto action = static_cast<KeyboardMouseAction>(index);
    const auto input = bindings[action];
    result.held[index] = keyboardMouseInputDown(input, device);
    result.wheel_steps[index] =
        mouseWheelActivationCount(input, device.mouse_wheel_delta);
  }
  return result;
}

PcPlayerInputRaw pcPlayerInputFromKeyboardMouseActions(
    const KeyboardMouseActionSnapshot &actions) noexcept {
  const auto down = [&actions](KeyboardMouseAction action) {
    return actions[action];
  };
  PcPlayerInputRaw result{
      .move_forward = down(KeyboardMouseAction::move_forward),
      .move_backward = down(KeyboardMouseAction::move_backward),
      .turn_left = down(KeyboardMouseAction::turn_left),
      .turn_right = down(KeyboardMouseAction::turn_right),
      .strafe_left = down(KeyboardMouseAction::strafe_left),
      .strafe_right = down(KeyboardMouseAction::strafe_right),
      .run = down(KeyboardMouseAction::run),
      .roll = down(KeyboardMouseAction::roll),
      .reload = down(KeyboardMouseAction::reload),
      .aim = down(KeyboardMouseAction::aim),
      .fire = down(KeyboardMouseAction::fire),
      .crouch = down(KeyboardMouseAction::crouch),
      .interact = down(KeyboardMouseAction::interact),
      .target_lock = down(KeyboardMouseAction::target_lock),
      .quick_turn = down(KeyboardMouseAction::quick_turn),
      .quick_weapon = down(KeyboardMouseAction::quick_weapon),
      .previous_weapon = down(KeyboardMouseAction::previous_weapon),
      .next_weapon = down(KeyboardMouseAction::next_weapon),
  };
  const auto menu_previous =
      static_cast<std::size_t>(KeyboardMouseAction::weapon_menu_previous);
  const auto menu_next =
      static_cast<std::size_t>(KeyboardMouseAction::weapon_menu_next);
  result.weapon_menu_previous = actions.wheel_steps[menu_previous] == 0 &&
                                down(KeyboardMouseAction::weapon_menu_previous);
  result.weapon_menu_next = actions.wheel_steps[menu_next] == 0 &&
                            down(KeyboardMouseAction::weapon_menu_next);
  for (std::size_t index = 0U; index < quick_weapon_slot_count; ++index) {
    const auto action = static_cast<KeyboardMouseAction>(
        static_cast<std::size_t>(KeyboardMouseAction::quick_weapon_1) + index);
    result.quick_weapon_keys[index] = down(action);
  }
  const auto wheel_delta = std::clamp<std::int64_t>(
      static_cast<std::int64_t>(actions.wheel_steps[menu_next]) -
          actions.wheel_steps[menu_previous],
      std::numeric_limits<std::int32_t>::min(),
      std::numeric_limits<std::int32_t>::max());
  result.weapon_cycle_delta = static_cast<std::int32_t>(wheel_delta);
  return result;
}

PlayerInputMapper::PlayerInputMapper(
    PlayerInputConfiguration configuration) noexcept {
  setConfiguration(configuration);
}

PlayerInputMapper::DigitalSnapshot
PlayerInputMapper::snapshot(const RawPlayerInput &raw) noexcept {
  DigitalSnapshot result{
      .run = raw.pc.run || raw.controller.run,
      .roll = raw.pc.roll || raw.controller.roll,
      .reload = raw.pc.reload || raw.controller.reload,
      .aim = raw.pc.aim || raw.controller.aim,
      .fire = raw.pc.fire || raw.controller.fire,
      .kneel = raw.pc.crouch || raw.controller.kneel,
      .interact = raw.pc.interact || raw.controller.interact,
      .target_lock = raw.pc.target_lock || raw.controller.target_lock,
      .quick_turn = raw.pc.quick_turn || raw.controller.quick_turn,
      .quick_weapon = raw.pc.quick_weapon || raw.controller.change_weapon,
      .previous_weapon =
          raw.pc.previous_weapon || raw.controller.previous_weapon,
      .next_weapon = raw.pc.next_weapon || raw.controller.next_weapon,
      .weapon_menu_previous = raw.pc.weapon_menu_previous,
      .weapon_menu_next = raw.pc.weapon_menu_next,
      .quick_weapon_slots = raw.pc.quick_weapon_keys,
  };
  return result;
}

PlayerInput PlayerInputMapper::update(const RawPlayerInput &raw) noexcept {
  const auto current = snapshot(raw);
  PlayerInput output;

  output.run = actionState(current.run, previous_.run);
  output.roll = actionState(current.roll, previous_.roll);
  output.reload = actionState(current.reload, previous_.reload);
  output.aim = actionState(current.aim, previous_.aim);
  output.fire = actionState(current.fire, previous_.fire);
  output.kneel = actionState(current.kneel, previous_.kneel);
  output.interact = actionState(current.interact, previous_.interact);
  output.target_lock = actionState(current.target_lock, previous_.target_lock);
  output.quick_turn = actionState(current.quick_turn, previous_.quick_turn);
  output.quick_weapon =
      actionState(current.quick_weapon, previous_.quick_weapon);
  output.previous_weapon =
      actionState(current.previous_weapon, previous_.previous_weapon);
  output.next_weapon = actionState(current.next_weapon, previous_.next_weapon);
  output.weapon_menu_delta = raw.pc.weapon_cycle_delta;
  const auto previous_menu =
      actionState(current.weapon_menu_previous, previous_.weapon_menu_previous);
  const auto next_menu =
      actionState(current.weapon_menu_next, previous_.weapon_menu_next);
  if (previous_menu.pressed != next_menu.pressed) {
    output.weapon_menu_delta += next_menu.pressed ? 1 : -1;
  }

  for (std::size_t index = 0; index < quick_weapon_slot_count; ++index) {
    output.quick_weapon_slots[index] = actionState(
        current.quick_weapon_slots[index], previous_.quick_weapon_slots[index]);
    if (!output.quick_weapon_slot_pressed &&
        output.quick_weapon_slots[index].pressed) {
      output.quick_weapon_slot_pressed = static_cast<std::uint8_t>(index);
    }
  }

  const auto digital_forward =
      digitalAxis(raw.pc.move_forward, raw.pc.move_backward);
  const auto digital_lateral = digitalAxis(raw.pc.turn_right, raw.pc.turn_left);
  const auto digital_strafe =
      digitalAxis(raw.pc.strafe_right, raw.pc.strafe_left);
  output.move_forward = digitalOrAnalog(
      digital_forward,
      deadzoneAxis(raw.controller.left_y, configuration_.movement_deadzone));
  const auto lateral = digitalOrAnalog(
      digital_lateral,
      deadzoneAxis(raw.controller.left_x, configuration_.movement_deadzone));
  const auto controller_strafe =
      digitalAxis(raw.controller.strafe_right, raw.controller.strafe_left);
  // Keep locomotion and the dedicated corner-peek channel separate here.
  // First-person routing turns forward/lateral into W/S movement and A/D
  // strafe; Q/E remains the independent retail L2/R2 channel.
  output.turn = lateral;
  output.move_strafe = digitalOrAnalog(digital_strafe, controller_strafe);

  const auto mouse_yaw =
      finiteOrZero(raw.pc.mouse_delta_x) * configuration_.mouse_yaw_sensitivity;
  const auto mouse_pitch = -finiteOrZero(raw.pc.mouse_delta_y) *
                           configuration_.mouse_pitch_sensitivity;
  const auto controller_yaw =
      deadzoneAxis(raw.controller.right_x, configuration_.look_deadzone) *
      configuration_.controller_yaw_sensitivity;
  const auto controller_pitch =
      deadzoneAxis(raw.controller.right_y, configuration_.look_deadzone) *
      configuration_.controller_pitch_sensitivity;
  output.mouse_look_yaw = mouse_yaw;
  output.mouse_look_pitch = mouse_pitch;
  output.controller_look_yaw = controller_yaw;
  output.controller_look_pitch = controller_pitch;
  if (configuration_.invert_pitch) {
    output.mouse_look_pitch = -output.mouse_look_pitch;
    output.controller_look_pitch = -output.controller_look_pitch;
  }
  output.look_yaw = output.mouse_look_yaw + output.controller_look_yaw;
  output.look_pitch = output.mouse_look_pitch + output.controller_look_pitch;

  previous_ = current;
  return output;
}

FirstPersonAimInput firstPersonAimInput(const PlayerInput &input) noexcept {
  return FirstPersonAimInput{
      PlayerLookSample{
          finiteOrZero(input.mouse_look_yaw),
          finiteOrZero(input.mouse_look_pitch),
      },
      PlayerLookSample{
          std::clamp(finiteOrZero(input.controller_look_yaw), -1.0, 1.0) *
              controller_first_person_yaw_units_per_tick,
          std::clamp(finiteOrZero(input.controller_look_pitch), -1.0, 1.0) *
              controller_first_person_pitch_units_per_tick,
      },
      std::clamp(finiteOrZero(input.move_forward), -1.0, 1.0),
      std::clamp(finiteOrZero(input.turn), -1.0, 1.0),
      std::clamp(finiteOrZero(input.move_strafe), -1.0, 1.0),
  };
}

void PlayerInputMapper::synchronize(const RawPlayerInput &raw) noexcept {
  previous_ = snapshot(raw);
}

void PlayerInputMapper::reset() noexcept { previous_ = {}; }

void PlayerInputMapper::setConfiguration(
    PlayerInputConfiguration configuration) noexcept {
  configuration_ = sanitized(configuration);
}

void PlayerLookLatch::latch(const PlayerInput &input) noexcept {
  mouse_yaw_ += finiteOrZero(input.mouse_look_yaw);
  mouse_pitch_ += finiteOrZero(input.mouse_look_pitch);
  controller_yaw_ = finiteOrZero(input.controller_look_yaw);
  controller_pitch_ = finiteOrZero(input.controller_look_pitch);
}

PlayerLookSample PlayerLookLatch::consumeForGuestTick() noexcept {
  const PlayerLookSample result{
      finiteOrZero(mouse_yaw_ + controller_yaw_),
      finiteOrZero(mouse_pitch_ + controller_pitch_),
  };
  mouse_yaw_ = 0.0;
  mouse_pitch_ = 0.0;
  return result;
}

PlayerLookSample PlayerLookLatch::controllerForCatchUpTick() const noexcept {
  return PlayerLookSample{controller_yaw_, controller_pitch_};
}

void PlayerLookLatch::reset() noexcept {
  mouse_yaw_ = 0.0;
  mouse_pitch_ = 0.0;
  controller_yaw_ = 0.0;
  controller_pitch_ = 0.0;
}

PlayerLookSample clampPresentationLook(PlayerLookSample sample) noexcept {
  return PlayerLookSample{
      std::clamp(finiteOrZero(sample.yaw), -maximum_presentation_yaw_lead,
                 maximum_presentation_yaw_lead),
      std::clamp(finiteOrZero(sample.pitch), -maximum_presentation_pitch_lead,
                 maximum_presentation_pitch_lead),
  };
}

PlayerLookSample reconcilePresentationLook(PlayerLookSample predicted,
                                           PlayerLookSample observed) noexcept {
  predicted = clampPresentationLook(predicted);
  return clampPresentationLook(PlayerLookSample{
      predicted.yaw - finiteOrZero(observed.yaw),
      predicted.pitch - finiteOrZero(observed.pitch),
  });
}

PlayerLookSample blendPresentationLook(PlayerLookSample reconciliation,
                                       PlayerLookSample pending,
                                       double guest_tick_fraction) noexcept {
  static_cast<void>(guest_tick_fraction);
  reconciliation = clampPresentationLook(reconciliation);
  pending = clampPresentationLook(pending);
  return clampPresentationLook(PlayerLookSample{
      finiteOrZero(reconciliation.yaw) + pending.yaw,
      finiteOrZero(reconciliation.pitch) + pending.pitch,
  });
}

void PlayerLookDisplayIntegrator::integrate(
    PlayerLookSample relative, PlayerLookSample absolute_per_guest_tick,
    double elapsed_seconds, double guest_tick_seconds) noexcept {
  elapsed_seconds = nonNegativeFinite(elapsed_seconds);
  guest_tick_seconds = nonNegativeFinite(guest_tick_seconds);
  const auto tick_fraction =
      guest_tick_seconds > 0.0 ? elapsed_seconds / guest_tick_seconds : 0.0;
  integrated_.yaw =
      finiteOrZero(integrated_.yaw + finiteOrZero(relative.yaw) +
                   finiteOrZero(absolute_per_guest_tick.yaw) * tick_fraction);
  integrated_.pitch =
      finiteOrZero(integrated_.pitch + finiteOrZero(relative.pitch) +
                   finiteOrZero(absolute_per_guest_tick.pitch) * tick_fraction);
}

PlayerLookSample PlayerLookDisplayIntegrator::sample() const noexcept {
  return clampPresentationLook(integrated_);
}

PlayerLookSample PlayerLookDisplayIntegrator::nativeSample() const noexcept {
  return PlayerLookSample{
      std::clamp(finiteOrZero(integrated_.yaw),
                 -maximum_native_mouse_look_delta,
                 maximum_native_mouse_look_delta),
      std::clamp(finiteOrZero(integrated_.pitch),
                 -maximum_native_mouse_look_delta,
                 maximum_native_mouse_look_delta),
  };
}

void PlayerLookDisplayIntegrator::reset() noexcept { integrated_ = {}; }

void PlayerAimFireLatch::latch(bool aiming, bool fire_pressed) noexcept {
  pending_ = pending_ || (aiming && fire_pressed);
}

bool PlayerAimFireLatch::consume(bool aiming, bool fire_pressed) noexcept {
  const auto result = aiming || (pending_ && fire_pressed);
  pending_ = false;
  return result;
}

} // namespace sf::platform
