#include "sf/platform/player_input.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{std::string{message}};
  }
}

bool near(double first, double second) {
  return std::abs(first - second) < 0.000001;
}

void testControllerMenuDirectionRejectsDrift() {
  using sf::platform::controllerMenuDirection;

  require(controllerMenuDirection(128U, 128U) == 0,
          "Centered controller moved the menu");
  require(controllerMenuDirection(100U, 156U) == 0,
          "DualSense-sized stick drift escaped the menu deadzone");
  require(controllerMenuDirection(96U, 128U) == 0 &&
              controllerMenuDirection(160U, 128U) == 0,
          "Menu engage boundary was not neutral");
  require(controllerMenuDirection(95U, 128U) == -1 &&
              controllerMenuDirection(161U, 128U) == 1,
          "Deliberate horizontal menu movement was rejected");
  require(controllerMenuDirection(128U, 95U) == -1 &&
              controllerMenuDirection(128U, 161U) == 1,
          "Deliberate vertical menu movement was rejected");

  // Once engaged, the smaller release threshold prevents threshold noise
  // without forcing the player to move through nearly half the stick travel.
  require(controllerMenuDirection(128U, 153U, 1) == 1 &&
              controllerMenuDirection(128U, 152U, 1) == 0,
          "Menu direction hysteresis did not release deterministically");

  // A smaller opposing-axis deflection used to win solely because its sign
  // was checked first, making DualSense navigation appear stuck or reversed.
  require(controllerMenuDirection(68U, 220U) == 1,
          "Negative horizontal drift overrode deliberate downward input");
  require(controllerMenuDirection(230U, 60U) == 1,
          "Negative vertical drift overrode deliberate rightward input");
  require(controllerMenuDirection(40U, 200U) == -1,
          "Dominant negative menu movement was not preserved");
}

sf::platform::PcPlayerInputRaw
expectedPcInputForAction(sf::platform::KeyboardMouseAction action) {
  using sf::platform::KeyboardMouseAction;
  sf::platform::PcPlayerInputRaw expected;
  const auto slot_begin =
      static_cast<std::size_t>(KeyboardMouseAction::quick_weapon_1);
  const auto action_index = static_cast<std::size_t>(action);
  if (action_index >= slot_begin &&
      action_index < slot_begin + sf::platform::quick_weapon_slot_count) {
    expected.quick_weapon_keys[action_index - slot_begin] = true;
    return expected;
  }
  switch (action) {
  case KeyboardMouseAction::move_forward:
    expected.move_forward = true;
    break;
  case KeyboardMouseAction::move_backward:
    expected.move_backward = true;
    break;
  case KeyboardMouseAction::turn_left:
    expected.turn_left = true;
    break;
  case KeyboardMouseAction::turn_right:
    expected.turn_right = true;
    break;
  case KeyboardMouseAction::strafe_left:
    expected.strafe_left = true;
    break;
  case KeyboardMouseAction::strafe_right:
    expected.strafe_right = true;
    break;
  case KeyboardMouseAction::run:
    expected.run = true;
    break;
  case KeyboardMouseAction::roll:
    expected.roll = true;
    break;
  case KeyboardMouseAction::reload:
    expected.reload = true;
    break;
  case KeyboardMouseAction::aim:
    expected.aim = true;
    break;
  case KeyboardMouseAction::fire:
    expected.fire = true;
    break;
  case KeyboardMouseAction::crouch:
    expected.crouch = true;
    break;
  case KeyboardMouseAction::interact:
    expected.interact = true;
    break;
  case KeyboardMouseAction::target_lock:
    expected.target_lock = true;
    break;
  case KeyboardMouseAction::quick_turn:
    expected.quick_turn = true;
    break;
  case KeyboardMouseAction::quick_weapon:
    expected.quick_weapon = true;
    break;
  case KeyboardMouseAction::previous_weapon:
    expected.previous_weapon = true;
    break;
  case KeyboardMouseAction::next_weapon:
    expected.next_weapon = true;
    break;
  case KeyboardMouseAction::weapon_menu_previous:
    expected.weapon_menu_previous = true;
    break;
  case KeyboardMouseAction::weapon_menu_next:
    expected.weapon_menu_next = true;
    break;
  case KeyboardMouseAction::pause:
  case KeyboardMouseAction::quick_weapon_1:
  case KeyboardMouseAction::quick_weapon_2:
  case KeyboardMouseAction::quick_weapon_3:
  case KeyboardMouseAction::quick_weapon_4:
  case KeyboardMouseAction::quick_weapon_5:
  case KeyboardMouseAction::quick_weapon_6:
  case KeyboardMouseAction::quick_weapon_7:
  case KeyboardMouseAction::quick_weapon_8:
  case KeyboardMouseAction::quick_weapon_9:
  case KeyboardMouseAction::quick_weapon_10:
  case KeyboardMouseAction::count:
    break;
  }
  return expected;
}

bool mappedActionMatches(const sf::platform::PlayerInput &input,
                         sf::platform::KeyboardMouseAction action) {
  using sf::platform::KeyboardMouseAction;
  const auto slot_begin =
      static_cast<std::size_t>(KeyboardMouseAction::quick_weapon_1);
  const auto action_index = static_cast<std::size_t>(action);
  if (action_index >= slot_begin &&
      action_index < slot_begin + sf::platform::quick_weapon_slot_count) {
    const auto slot = action_index - slot_begin;
    return input.quick_weapon_slots[slot].held &&
           input.quick_weapon_slots[slot].pressed &&
           input.quick_weapon_slot_pressed == slot;
  }
  switch (action) {
  case KeyboardMouseAction::move_forward:
    return near(input.move_forward, 1.0);
  case KeyboardMouseAction::move_backward:
    return near(input.move_forward, -1.0);
  case KeyboardMouseAction::turn_left:
    return near(input.turn, -1.0);
  case KeyboardMouseAction::turn_right:
    return near(input.turn, 1.0);
  case KeyboardMouseAction::strafe_left:
    return near(input.move_strafe, -1.0);
  case KeyboardMouseAction::strafe_right:
    return near(input.move_strafe, 1.0);
  case KeyboardMouseAction::run:
    return input.run.held && input.run.pressed;
  case KeyboardMouseAction::roll:
    return input.roll.held && input.roll.pressed;
  case KeyboardMouseAction::reload:
    return input.reload.held && input.reload.pressed;
  case KeyboardMouseAction::aim:
    return input.aim.held && input.aim.pressed;
  case KeyboardMouseAction::fire:
    return input.fire.held && input.fire.pressed;
  case KeyboardMouseAction::crouch:
    return input.kneel.held && input.kneel.pressed;
  case KeyboardMouseAction::interact:
    return input.interact.held && input.interact.pressed;
  case KeyboardMouseAction::target_lock:
    return input.target_lock.held && input.target_lock.pressed;
  case KeyboardMouseAction::quick_turn:
    return input.quick_turn.held && input.quick_turn.pressed;
  case KeyboardMouseAction::quick_weapon:
    return input.quick_weapon.held && input.quick_weapon.pressed;
  case KeyboardMouseAction::previous_weapon:
    return input.previous_weapon.held && input.previous_weapon.pressed;
  case KeyboardMouseAction::next_weapon:
    return input.next_weapon.held && input.next_weapon.pressed;
  case KeyboardMouseAction::weapon_menu_previous:
    return input.weapon_menu_delta == -1;
  case KeyboardMouseAction::weapon_menu_next:
    return input.weapon_menu_delta == 1;
  case KeyboardMouseAction::pause:
    return true;
  case KeyboardMouseAction::quick_weapon_1:
  case KeyboardMouseAction::quick_weapon_2:
  case KeyboardMouseAction::quick_weapon_3:
  case KeyboardMouseAction::quick_weapon_4:
  case KeyboardMouseAction::quick_weapon_5:
  case KeyboardMouseAction::quick_weapon_6:
  case KeyboardMouseAction::quick_weapon_7:
  case KeyboardMouseAction::quick_weapon_8:
  case KeyboardMouseAction::quick_weapon_9:
  case KeyboardMouseAction::quick_weapon_10:
  case KeyboardMouseAction::count:
    return false;
  }
  return false;
}

void testKeyboardMouseBindingCatalog() {
  using sf::platform::KeyboardMouseAction;
  using sf::platform::KeyboardMouseInput;

  const auto defaults = sf::platform::defaultKeyboardMouseBindings();
  require(sf::platform::keyboard_mouse_action_count == 31U,
          "Keyboard/mouse action catalog is incomplete");
  for (std::size_t index = 0U;
       index < sf::platform::keyboard_mouse_action_count; ++index) {
    const auto action = static_cast<KeyboardMouseAction>(index);
    require(!sf::platform::keyboardMouseActionName(action).empty() &&
                !sf::platform::keyboardMouseActionConfigKey(action).empty() &&
                sf::platform::isValidKeyboardMouseInput(defaults[action]) &&
                !sf::platform::keyboardMouseInputName(defaults[action]).empty(),
            "Binding catalog has an unnamed or invalid default");
  }
  require(defaults[KeyboardMouseAction::move_forward] ==
                  KeyboardMouseInput::w &&
              defaults[KeyboardMouseAction::aim] ==
                  KeyboardMouseInput::mouse_right &&
              defaults[KeyboardMouseAction::fire] ==
                  KeyboardMouseInput::mouse_left &&
              defaults[KeyboardMouseAction::quick_weapon] ==
                  KeyboardMouseInput::mouse_middle &&
              defaults[KeyboardMouseAction::weapon_menu_next] ==
                  KeyboardMouseInput::mouse_wheel_up &&
              defaults[KeyboardMouseAction::weapon_menu_previous] ==
                  KeyboardMouseInput::mouse_wheel_down,
          "Launcher defaults diverged from the established PC scheme");
  require(sf::platform::isKeyboardInput(KeyboardMouseInput::f24) &&
              !sf::platform::isKeyboardInput(KeyboardMouseInput::mouse_x1) &&
              !sf::platform::isValidKeyboardMouseInput(
                  static_cast<KeyboardMouseInput>(102U)),
          "Binding persistence accepted an unknown input code");
}

void testKeyboardMousePromptText() {
  using sf::platform::KeyboardMouseAction;
  using sf::platform::KeyboardMouseInput;

  auto bindings = sf::platform::defaultKeyboardMouseBindings();
  require(
      sf::platform::keyboardMousePromptText("Press %x to continue", bindings) ==
          sf::platform::KeyboardMousePromptText{"Press X to continue",
                                                "Press F to continue"},
      "Cross prompt did not use the launcher interaction binding");
  require(sf::platform::keyboardMousePromptText("Press START to see objectives",
                                                bindings) ==
              sf::platform::KeyboardMousePromptText{
                  "Press START to see objectives",
                  "Press ESCAPE to see objectives",
                  sf::platform::InputPromptAction::pause},
          "START prompt did not use the launcher pause binding");
  bindings[KeyboardMouseAction::interact] = KeyboardMouseInput::mouse_left;
  require(sf::platform::keyboardMousePromptText("Press X to Contact Lian Xing",
                                                bindings) ==
              sf::platform::KeyboardMousePromptText{
                  "Press X to Contact Lian Xing",
                  "Press MOUSE LEFT to Contact Lian Xing",
                  sf::platform::InputPromptAction::interact},
          "Contact prompt did not use a remapped mouse interaction binding");
  require(
      sf::platform::keyboardMousePromptText("Press BUTTON to Contact Lian Xing",
                                            bindings) ==
          sf::platform::KeyboardMousePromptText{
              "Press BUTTON to Contact Lian Xing",
              "Press MOUSE LEFT to Contact Lian Xing",
              sf::platform::InputPromptAction::interact},
      "Reconstructed contact prompt did not resolve its button placeholder");
  bindings[KeyboardMouseAction::interact] = KeyboardMouseInput::left_bracket;
  require(
      sf::platform::keyboardMousePromptText("Press CROSS to continue", bindings)
              ->bound_text == "Press LEFT BRACKET to continue",
      "A bind absent from FONTA was not converted to drawable text");
  bindings[KeyboardMouseAction::pause] = KeyboardMouseInput::mouse_x2;
  require(
      sf::platform::keyboardMouseHintText("%x select   %t back", bindings) ==
          "LEFT BRACKET select   MOUSE X2 back",
      "Native menu hints did not resolve both active PC bindings");
  require(sf::platform::keyboardMouseHintText("ordinary text", bindings) ==
              "ordinary text",
          "Native menu hint resolver changed text without control tokens");
  require(sf::platform::keyboardMouseHintText("Press new button for action",
                                              bindings) ==
              "Press new button for action",
          "Binding-capture instructions were mistaken for control tokens");
  bindings[KeyboardMouseAction::interact] = KeyboardMouseInput::keypad_plus;
  require(sf::platform::keyboardMouseHintText("%x accept", bindings) ==
              "NUMPAD PLUS accept",
          "A keypad binding emitted a glyph absent from the HUD font");
  bindings[KeyboardMouseAction::interact] = KeyboardMouseInput::keypad_equals;
  require(sf::platform::keyboardMouseHintText("%x accept", bindings) ==
              "NUMPAD EQUALS accept",
          "The keypad equals binding emitted an unsupported glyph");
  require(!sf::platform::keyboardMousePromptText("Objective added", bindings),
          "Ordinary retail text was mistaken for a control prompt");
}

void testDeviceAwareInputPromptText() {
  using sf::platform::ControllerInputProtocol;
  using sf::platform::ControllerPromptFamily;
  using sf::platform::InputPromptAction;
  using sf::platform::InputPromptBindingNames;
  using sf::platform::InputPromptDevice;
  using sf::platform::InputPromptText;
  using sf::platform::InputPromptTokenActions;
  using sf::platform::KeyboardMouseAction;
  using sf::platform::KeyboardMouseInput;

  struct FamilyNames {
    ControllerPromptFamily family;
    std::array<std::string_view, 16U> names;
  };
  constexpr std::array family_names{
      FamilyNames{ControllerPromptFamily::generic,
                  {"BUTTON 7", "BUTTON 9", "BUTTON 10", "BUTTON 8", "DPAD UP",
                   "DPAD RIGHT", "DPAD DOWN", "DPAD LEFT", "LEFT TRIGGER",
                   "RIGHT TRIGGER", "BUTTON 5", "BUTTON 6", "BUTTON 4",
                   "BUTTON 2", "BUTTON 1", "BUTTON 3"}},
      FamilyNames{ControllerPromptFamily::xbox,
                  {"VIEW", "LEFT STICK", "RIGHT STICK", "MENU", "DPAD UP",
                   "DPAD RIGHT", "DPAD DOWN", "DPAD LEFT", "LT", "RT", "LB",
                   "RB", "Y", "B", "A", "X"}},
      FamilyNames{ControllerPromptFamily::playstation,
                  {"SHARE", "L3", "R3", "OPTIONS", "DPAD UP", "DPAD RIGHT",
                   "DPAD DOWN", "DPAD LEFT", "L2", "R2", "L1", "R1", "TRIANGLE",
                   "CIRCLE", "CROSS", "SQUARE"}},
      FamilyNames{ControllerPromptFamily::nintendo,
                  {"MINUS", "LEFT STICK", "RIGHT STICK", "PLUS", "DPAD UP",
                   "DPAD RIGHT", "DPAD DOWN", "DPAD LEFT", "ZL", "ZR", "L", "R",
                   "X", "A", "B", "Y"}},
  };
  for (const auto &entry : family_names) {
    for (auto index = std::size_t{}; index < entry.names.size(); ++index) {
      require(sf::platform::controllerButtonPromptName(
                  entry.family, static_cast<std::uint16_t>(1U << index)) ==
                  entry.names[index],
              "Controller label did not follow the real PS1 bit order");
    }
  }
  require(sf::platform::controllerButtonPromptName(
              ControllerPromptFamily::generic, std::uint16_t{0x4000U}) ==
              "BUTTON 1",
          "Generic fallback did not identify physical Cross as BUTTON 1");
  require(sf::platform::controllerButtonPromptName(ControllerPromptFamily::xbox,
                                                   0U) == "UNBOUND" &&
              sf::platform::controllerButtonPromptName(
                  ControllerPromptFamily::xbox,
                  std::uint16_t{(1U << 5U) | (1U << 6U)}) == "UNBOUND",
          "Non-singular PS1 button masks were presented as physical buttons");

  auto keyboard = sf::platform::defaultKeyboardMouseBindings();
  keyboard[KeyboardMouseAction::interact] = KeyboardMouseInput::mouse_left;
  keyboard[KeyboardMouseAction::pause] = KeyboardMouseInput::mouse_x2;
  keyboard[KeyboardMouseAction::fire] = KeyboardMouseInput::keypad_plus;
  const auto keyboard_prompts =
      sf::platform::keyboardMouseInputPromptBindings(keyboard);
  require(keyboard_prompts.device == InputPromptDevice::keyboard_mouse &&
              keyboard_prompts.controller_protocol ==
                  ControllerInputProtocol::unknown &&
              sf::platform::inputPromptLabel(keyboard_prompts,
                                             InputPromptAction::confirm) ==
                  "MOUSE LEFT" &&
              sf::platform::inputPromptLabel(
                  keyboard_prompts, InputPromptAction::cancel) == "MOUSE X2" &&
              sf::platform::inputPromptLabel(
                  keyboard_prompts, InputPromptAction::fire) == "NUMPAD PLUS",
          "Keyboard/mouse prompt bindings lost semantic action labels");

  const auto xbox = sf::platform::controllerInputPromptBindings(
      ControllerInputProtocol::xinput,
      InputPromptBindingNames{
          .confirm = sf::platform::controllerButtonPromptName(
              ControllerPromptFamily::xbox, std::uint16_t{0x4000U}),
          .cancel = sf::platform::controllerButtonPromptName(
              ControllerPromptFamily::xbox, std::uint16_t{0x1000U}),
          .pause = sf::platform::controllerButtonPromptName(
              ControllerPromptFamily::xbox, std::uint16_t{0x0008U}),
          .interact = sf::platform::controllerButtonPromptName(
              ControllerPromptFamily::xbox, std::uint16_t{0x1000U}),
          .fire = sf::platform::controllerButtonPromptName(
              ControllerPromptFamily::xbox, std::uint16_t{0x8000U}),
      });
  require(xbox.device == InputPromptDevice::controller &&
              xbox.controller_protocol == ControllerInputProtocol::xinput,
          "XInput prompt metadata was not retained");
  require(
      sf::platform::inputPromptText("Press %x to continue", xbox) ==
              InputPromptText{"Press X to continue", "Press A to continue"} &&
          sf::platform::inputPromptText("Press START to see objectives",
                                        xbox) ==
              InputPromptText{"Press START to see objectives",
                              "Press MENU to see objectives",
                              InputPromptAction::pause} &&
          sf::platform::inputPromptText("Press X to Contact Lian Xing", xbox) ==
              InputPromptText{"Press X to Contact Lian Xing",
                              "Press Y to Contact Lian Xing",
                              InputPromptAction::interact} &&
          sf::platform::inputPromptText("Press BUTTON to Contact Lian Xing",
                                        xbox) ==
              InputPromptText{"Press BUTTON to Contact Lian Xing",
                              "Press Y to Contact Lian Xing",
                              InputPromptAction::interact},
      "Service prompts did not resolve controller actions semantically");
  const auto continue_prompt =
      sf::platform::inputPromptText("Press %x to continue", xbox);
  const auto contact_prompt =
      sf::platform::inputPromptText("Press X to Contact Lian Xing", xbox);
  require(continue_prompt && contact_prompt &&
              sf::platform::inputHintText(
                  "LOCALIZED %x CONTINUE", xbox,
                  InputPromptTokenActions{.x = continue_prompt->action}) ==
                  "LOCALIZED A CONTINUE" &&
              sf::platform::inputHintText(
                  "LOCALIZED %x CONTACT", xbox,
                  InputPromptTokenActions{.x = contact_prompt->action}) ==
                  "LOCALIZED Y CONTACT",
          "Localized %x lost Confirm versus Interact semantics");

  require(sf::platform::inputHintText("%x select   %t resume", xbox) ==
                  "A select   MENU resume" &&
              sf::platform::inputHintText(
                  "%X accept\t%T cancel", xbox,
                  InputPromptTokenActions{.x = InputPromptAction::confirm,
                                          .t = InputPromptAction::cancel}) ==
                  "A accept Y cancel",
          "Controller hint tokens did not honor caller-supplied roles");
  require(!sf::platform::inputPromptText("Objective added", xbox),
          "Ordinary service text was mistaken for a controller prompt");

  for (const auto protocol : {ControllerInputProtocol::direct_input,
                              ControllerInputProtocol::raw_input}) {
    const auto generic = sf::platform::controllerInputPromptBindings(
        protocol, {.confirm = "button 2",
                   .cancel = "button 3",
                   .pause = "button 10",
                   .interact = "button 4",
                   .fire = ""});
    require(generic.controller_protocol == protocol &&
                sf::platform::inputPromptLabel(
                    generic, InputPromptAction::confirm) == "BUTTON 2" &&
                sf::platform::inputPromptLabel(
                    generic, InputPromptAction::interact) == "BUTTON 4" &&
                sf::platform::inputPromptLabel(
                    generic, InputPromptAction::fire) == "UNBOUND",
            "DInput/Raw Input labels were not normalized safely");
  }
  require(sf::platform::inputPromptLabel(
              xbox, static_cast<InputPromptAction>(0xffU)) == "UNBOUND",
          "Invalid prompt action did not fall back safely");
}
void testBoundKeyboardMouseActionMatrix() {
  using sf::platform::KeyboardMouseAction;
  using sf::platform::KeyboardMouseBindings;
  using sf::platform::KeyboardMouseDeviceState;
  using sf::platform::KeyboardMouseInput;

  std::array<std::uint8_t, 256U> keyboard{};
  keyboard[static_cast<std::size_t>(KeyboardMouseInput::f24)] = 1U;
  for (std::size_t index = 0U;
       index < sf::platform::keyboard_mouse_action_count; ++index) {
    const auto action = static_cast<KeyboardMouseAction>(index);
    KeyboardMouseBindings bindings;
    bindings[action] = KeyboardMouseInput::f24;
    const auto actions = sf::platform::sampleKeyboardMouseActions(
        bindings, KeyboardMouseDeviceState{.keyboard = keyboard});
    for (std::size_t other = 0U;
         other < sf::platform::keyboard_mouse_action_count; ++other) {
      require(actions.held[other] == (other == index) &&
                  actions.wheel_steps[other] == 0,
              "Binding sampler activated the wrong action");
    }
    const auto raw =
        sf::platform::pcPlayerInputFromKeyboardMouseActions(actions);
    require(raw == expectedPcInputForAction(action),
            "Bound action reached the wrong PC input channel");
    sf::platform::RawPlayerInput combined{.pc = raw};
    const auto mapped = sf::platform::PlayerInputMapper{}.update(combined);
    require(mappedActionMatches(mapped, action),
            "Bound action did not reach its mapped gameplay channel");
  }

  auto wheel_bindings = KeyboardMouseBindings{};
  wheel_bindings[KeyboardMouseAction::weapon_menu_previous] =
      KeyboardMouseInput::mouse_wheel_down;
  wheel_bindings[KeyboardMouseAction::weapon_menu_next] =
      KeyboardMouseInput::mouse_wheel_up;
  auto wheel_actions = sf::platform::sampleKeyboardMouseActions(
      wheel_bindings, KeyboardMouseDeviceState{.mouse_wheel_delta = 3});
  auto wheel_raw =
      sf::platform::pcPlayerInputFromKeyboardMouseActions(wheel_actions);
  require(wheel_raw.weapon_cycle_delta == 3 &&
              !wheel_raw.weapon_menu_previous && !wheel_raw.weapon_menu_next,
          "Multi-notch wheel-up input was collapsed or duplicated");
  wheel_actions = sf::platform::sampleKeyboardMouseActions(
      wheel_bindings, KeyboardMouseDeviceState{.mouse_wheel_delta = -2});
  wheel_raw =
      sf::platform::pcPlayerInputFromKeyboardMouseActions(wheel_actions);
  require(wheel_raw.weapon_cycle_delta == -2 &&
              !wheel_raw.weapon_menu_previous && !wheel_raw.weapon_menu_next,
          "Multi-notch wheel-down input was collapsed or duplicated");

  KeyboardMouseBindings side_button_bindings;
  side_button_bindings[KeyboardMouseAction::crouch] =
      KeyboardMouseInput::mouse_x1;
  const auto side_button = sf::platform::sampleKeyboardMouseActions(
      side_button_bindings, KeyboardMouseDeviceState{.mouse_x1 = true});
  require(side_button[KeyboardMouseAction::crouch] &&
              sf::platform::pcPlayerInputFromKeyboardMouseActions(side_button)
                  .crouch,
          "Mouse side-button binding did not reach crouch/stealth");

  KeyboardMouseBindings invalid_bindings;
  invalid_bindings[KeyboardMouseAction::fire] =
      static_cast<KeyboardMouseInput>(0xffffU);
  const auto invalid = sf::platform::sampleKeyboardMouseActions(
      invalid_bindings, KeyboardMouseDeviceState{.keyboard = keyboard});
  require(!invalid[KeyboardMouseAction::fire],
          "Invalid persisted binding escaped safe sampling");
  const std::array<std::uint8_t, 8U> truncated_keyboard{};
  KeyboardMouseBindings truncated_bindings;
  truncated_bindings[KeyboardMouseAction::run] = KeyboardMouseInput::f24;
  const auto truncated = sf::platform::sampleKeyboardMouseActions(
      truncated_bindings,
      KeyboardMouseDeviceState{.keyboard = truncated_keyboard});
  require(!truncated[KeyboardMouseAction::run],
          "Truncated keyboard state caused an out-of-bounds binding read");
}

void testPcActionsAndEdges() {
  sf::platform::PlayerInputMapper mapper{sf::platform::PlayerInputConfiguration{
      .mouse_yaw_sensitivity = 0.5,
      .mouse_pitch_sensitivity = 0.25,
  }};
  sf::platform::RawPlayerInput raw;
  raw.pc.move_forward = true;
  raw.pc.turn_right = true;
  raw.pc.run = true;
  raw.pc.roll = true;
  raw.pc.reload = true;
  raw.pc.fire = true;
  raw.pc.aim = true;
  raw.pc.quick_weapon_keys[0] = true;
  raw.pc.mouse_delta_x = 4.0;
  raw.pc.mouse_delta_y = 8.0;

  const auto first = mapper.update(raw);
  require(near(first.move_forward, 1.0) && near(first.move_strafe, 0.0) &&
              near(first.turn, 1.0),
          "Directional input did not remain on the retail manual-aim axes");
  require(first.run.held && first.run.pressed && first.roll.pressed &&
              first.reload.pressed && first.aim.pressed && first.fire.pressed,
          "PC defaults did not emit initial action edges");
  require(first.quick_weapon_slot_pressed == 0U &&
              first.quick_weapon_slots[0].pressed,
          "Keyboard 1 did not select quick slot zero");
  require(near(first.look_yaw, 2.0) && near(first.look_pitch, -2.0),
          "Mouse look scale or Y convention is wrong");
  require(near(first.mouse_look_yaw, 2.0) &&
              near(first.mouse_look_pitch, -2.0) &&
              near(first.controller_look_yaw, 0.0) &&
              near(first.controller_look_pitch, 0.0),
          "Mouse look was not kept separate from the controller sample");

  raw.pc.mouse_delta_x = 0.0;
  raw.pc.mouse_delta_y = 0.0;
  const auto held = mapper.update(raw);
  require(held.fire.held && !held.fire.pressed && held.aim.held &&
              !held.aim.pressed && !held.quick_weapon_slot_pressed,
          "Held controls generated duplicate presses");

  raw = {};
  const auto released = mapper.update(raw);
  require(released.fire.released && released.aim.released &&
              released.run.released && released.quick_weapon_slots[0].released,
          "Released controls did not emit edges");
}

void testFirstPersonMousePrecisionTuning() {
  sf::platform::PlayerInputMapper mapper{sf::platform::PlayerInputConfiguration{
      .mouse_yaw_sensitivity = sf::platform::first_person_mouse_yaw_sensitivity,
      .mouse_pitch_sensitivity =
          sf::platform::first_person_mouse_pitch_sensitivity,
  }};
  sf::platform::RawPlayerInput raw;
  raw.pc.aim = true;
  raw.pc.strafe_left = true;
  raw.pc.mouse_delta_x = 1.0;
  raw.pc.mouse_delta_y = -1.0;

  const auto input = sf::platform::firstPersonAimInput(mapper.update(raw));
  require(near(input.mouse_look.yaw, 3.0) &&
              near(input.mouse_look.pitch, 2.75) &&
              near(input.directional_look_per_guest_tick.yaw, 0.0) &&
              near(input.directional_look_per_guest_tick.pitch, 0.0) &&
              near(input.move, 0.0) && near(input.strafe, 0.0) &&
              near(input.peek, -1.0),
          "First-person mouse gain or retail peek routing is incorrect");
}

void testTurnControllerLookAndInvert() {
  sf::platform::PlayerInputMapper mapper{sf::platform::PlayerInputConfiguration{
      .movement_deadzone = 0.2,
      .look_deadzone = 0.2,
      .controller_yaw_sensitivity = 2.0,
      .controller_pitch_sensitivity = 2.0,
      .invert_pitch = true,
  }};
  sf::platform::RawPlayerInput raw;
  raw.pc.turn_left = true;
  raw.controller.left_y = 0.6;
  raw.controller.right_x = 0.6;
  raw.controller.right_y = 0.6;

  const auto output = mapper.update(raw);
  require(near(output.move_forward, 0.5) && near(output.turn, -1.0) &&
              near(output.move_strafe, 0.0),
          "Tank turn or controller movement deadzone is wrong");
  require(near(output.look_yaw, 1.0) && near(output.look_pitch, -1.0),
          "Right-stick look or invert pitch is wrong");
  require(near(output.mouse_look_yaw, 0.0) &&
              near(output.mouse_look_pitch, 0.0) &&
              near(output.controller_look_yaw, 1.0) &&
              near(output.controller_look_pitch, -1.0),
          "Right-stick look was not kept as an absolute sample");

  sf::platform::PlayerInputMapper proportional{
      sf::platform::PlayerInputConfiguration{
          .look_deadzone = 0.0,
          .controller_yaw_sensitivity = 1.0,
          .controller_pitch_sensitivity = 1.0,
      }};
  sf::platform::RawPlayerInput half_stick;
  half_stick.controller.aim = true;
  half_stick.controller.right_x = 0.5;
  half_stick.controller.right_y = -0.5;
  const auto first_person = sf::platform::firstPersonAimInput(
      proportional.update(half_stick));
  require(near(first_person.directional_look_per_guest_tick.yaw,
               sf::platform::controller_first_person_yaw_units_per_tick *
                   0.5) &&
              near(first_person.directional_look_per_guest_tick.pitch,
                   -sf::platform::controller_first_person_pitch_units_per_tick *
                       0.5),
          "Half-stick first-person aim was clamped to a digital full-rate "
          "sample");
}

void testRefreshIndependentLookLatch() {
  const auto sample_at_refresh = [](int presentation_samples) {
    sf::platform::PlayerLookLatch latch;
    for (int sample = 0; sample < presentation_samples; ++sample) {
      sf::platform::PlayerInput input;
      // The same physical mouse travel is distributed over however many
      // presentation frames the display produced during one guest tick.
      input.mouse_look_yaw = 12.0 / presentation_samples;
      input.mouse_look_pitch = -6.0 / presentation_samples;
      input.controller_look_yaw = 32.0;
      input.controller_look_pitch = -24.0;
      latch.latch(input);
    }
    return latch.consumeForGuestTick();
  };

  const auto at_60_hz = sample_at_refresh(3);
  const auto at_144_hz = sample_at_refresh(7);
  require(near(at_60_hz.yaw, 44.0) && near(at_60_hz.pitch, -30.0) &&
              near(at_144_hz.yaw, at_60_hz.yaw) &&
              near(at_144_hz.pitch, at_60_hz.pitch),
          "Guest look sample changed with presentation refresh rate");

  sf::platform::PlayerLookLatch catch_up;
  sf::platform::PlayerInput input;
  input.mouse_look_yaw = 8.0;
  input.controller_look_yaw = 20.0;
  catch_up.latch(input);
  require(near(catch_up.consumeForGuestTick().yaw, 28.0) &&
              near(catch_up.controllerForCatchUpTick().yaw, 20.0) &&
              near(catch_up.consumeForGuestTick().yaw, 20.0),
          "Catch-up tick replayed mouse motion or lost the held stick");
  catch_up.reset();
  require(near(catch_up.consumeForGuestTick().yaw, 0.0),
          "Look latch reset retained stale input");

  sf::platform::PlayerLookLatch disconnected_controller;
  input = {};
  input.controller_look_yaw = 48.0;
  input.controller_look_pitch = -36.0;
  disconnected_controller.latch(input);
  input = {};
  input.mouse_look_yaw = 5.0;
  input.mouse_look_pitch = -3.0;
  // A removed controller is reported as a neutral latest absolute sample.
  // Its pre-removal stick value must not survive until the guest tick.
  disconnected_controller.latch(input);
  const auto after_disconnect = disconnected_controller.consumeForGuestTick();
  require(
      near(after_disconnect.yaw, 5.0) && near(after_disconnect.pitch, -3.0) &&
          near(disconnected_controller.controllerForCatchUpTick().yaw, 0.0) &&
          near(disconnected_controller.controllerForCatchUpTick().pitch, 0.0),
      "Controller disconnect retained stale look on a high-refresh latch");

  const auto pitch_clamped =
      sf::platform::clampPresentationLook({500.0, -500.0});
  require(near(pitch_clamped.yaw, 128.0) && near(pitch_clamped.pitch, -96.0),
          "Display-rate look prediction escaped its bounded visual lead");
  const auto partial =
      sf::platform::reconcilePresentationLook({80.0, 60.0}, {32.0, 20.0});
  const auto partial_start =
      sf::platform::blendPresentationLook(partial, {}, 0.0);
  const auto partial_end =
      sf::platform::blendPresentationLook(partial, {}, 1.0);
  require(near(partial_start.yaw, 48.0) && near(partial_start.pitch, 40.0) &&
              near(partial_end.yaw, 48.0) && near(partial_end.pitch, 40.0),
          "Unconsumed mouse aim sprang back before the guest consumed it");
  const auto blocked =
      sf::platform::reconcilePresentationLook({72.0, -48.0}, {});
  const auto blocked_half =
      sf::platform::blendPresentationLook(blocked, {}, 0.5);
  require(near(blocked_half.yaw, 72.0) && near(blocked_half.pitch, -48.0),
          "Blocked guest camera response did not retain its pending aim");

  const auto bounded_reconciliation =
      sf::platform::reconcilePresentationLook({128.0, 96.0}, {-500.0, 500.0});
  const auto bounded_sum = sf::platform::blendPresentationLook(
      bounded_reconciliation, {100.0, -100.0}, 0.25);
  require(std::abs(bounded_reconciliation.yaw) <= 128.0 &&
              std::abs(bounded_reconciliation.pitch) <= 96.0 &&
              std::abs(bounded_sum.yaw) <= 128.0 &&
              std::abs(bounded_sum.pitch) <= 96.0,
          "Reconciliation plus pending input escaped the final lead clamp");
  const auto integrate_at_refresh = [](int samples) {
    sf::platform::PlayerLookDisplayIntegrator integrator;
    for (int sample = 0; sample < samples; ++sample) {
      integrator.integrate({12.0 / samples, -6.0 / samples}, {48.0, -24.0},
                           0.05 / samples, 0.05);
    }
    return integrator.sample();
  };
  const auto integrated_60 = integrate_at_refresh(3);
  const auto integrated_144 = integrate_at_refresh(7);
  require(near(integrated_60.yaw, 60.0) && near(integrated_60.pitch, -30.0) &&
              near(integrated_144.yaw, integrated_60.yaw) &&
              near(integrated_144.pitch, integrated_60.pitch),
          "Absolute display look integration changed with refresh rate");

  const auto integrate_native_sweep = [](int samples) {
    sf::platform::PlayerLookDisplayIntegrator integrator;
    for (int sample = 0; sample < samples; ++sample) {
      integrator.integrate({900.0 / samples, -600.0 / samples}, {}, 0.0, 0.05);
    }
    return integrator.nativeSample();
  };
  const auto native_60 = integrate_native_sweep(3);
  const auto native_144 = integrate_native_sweep(7);
  require(near(native_60.yaw, 900.0) && near(native_60.pitch, -600.0) &&
              near(native_144.yaw, native_60.yaw) &&
              near(native_144.pitch, native_60.pitch),
          "Fast native mouse sweep was clipped or changed with refresh rate");
  sf::platform::PlayerLookDisplayIntegrator delayed_native_frame;
  delayed_native_frame.integrate({9000.0, -9000.0}, {}, 0.0, 0.05);
  require(near(delayed_native_frame.nativeSample().yaw,
               sf::platform::maximum_native_mouse_look_delta) &&
              near(delayed_native_frame.nativeSample().pitch,
                   -sf::platform::maximum_native_mouse_look_delta),
          "Native mouse safety bound mismatch");

  sf::platform::PlayerLookDisplayIntegrator mid_tick;
  mid_tick.integrate({}, {48.0, -24.0}, 0.025, 0.05);
  mid_tick.integrate({}, {}, 0.025, 0.05);
  require(near(mid_tick.sample().yaw, 24.0) &&
              near(mid_tick.sample().pitch, -12.0),
          "Mid-tick stick release rewrote already integrated display motion");
  mid_tick.reset();
  require(near(mid_tick.sample().yaw, 0.0) &&
              near(mid_tick.sample().pitch, 0.0),
          "Display look integrator reset retained a stale axis sample");

  sf::platform::PlayerAimFireLatch aim_fire;
  aim_fire.latch(true, true);
  aim_fire.latch(false, false);
  require(aim_fire.pending() && aim_fire.consume(false, true) &&
              !aim_fire.pending() && !aim_fire.consume(false, true),
          "RMB release reinterpreted a latched aimed fire edge or replayed it");
  aim_fire.latch(false, true);
  require(!aim_fire.pending() && !aim_fire.consume(false, true),
          "An unaimed fire edge manufactured a latched aim state");
  aim_fire.latch(true, true);
  aim_fire.reset();
  require(!aim_fire.pending(),
          "Aim-at-fire reset retained a stale shot across a clear path");
}

void testPadNeutralAndDisconnect() {
  sf::platform::PlayerInputMapper mapper;
  sf::platform::RawPlayerInput raw;
  // PsyCross reports 127 for every analog byte when no physical controller
  // is present. After its 128-centered normalization this is -1/128, not
  // exact zero, and must remain inside both mapper deadzones.
  constexpr auto disconnected_axis = -1.0 / 128.0;
  raw.controller.left_x = disconnected_axis;
  raw.controller.left_y = disconnected_axis;
  raw.controller.right_x = disconnected_axis;
  raw.controller.right_y = disconnected_axis;

  const auto neutral = mapper.update(raw);
  require(near(neutral.move_forward, 0.0) && near(neutral.move_strafe, 0.0) &&
              near(neutral.turn, 0.0) && near(neutral.look_yaw, 0.0) &&
              near(neutral.look_pitch, 0.0) && !neutral.run.held &&
              !neutral.aim.held && !neutral.fire.held,
          "Disconnected PAD analog neutral manufactured movement or look");

  raw.controller.left_y = 1.0;
  raw.controller.right_x = 1.0;
  raw.controller.aim = true;
  raw.controller.fire = true;
  const auto connected = mapper.update(raw);
  require(near(connected.move_forward, 1.0) &&
              near(connected.controller_look_yaw, 1.0) &&
              connected.aim.pressed && connected.fire.pressed,
          "Connected controller sample did not reach the mapper");

  raw.controller = {};
  const auto disconnected = mapper.update(raw);
  require(near(disconnected.move_forward, 0.0) &&
              near(disconnected.turn, 0.0) &&
              near(disconnected.controller_look_yaw, 0.0) &&
              disconnected.aim.released && disconnected.fire.released,
          "Controller removal retained movement, look, or held actions");
}

void testSynchronizationMenuAndKeyboardZero() {
  sf::platform::PlayerInputMapper mapper;
  sf::platform::RawPlayerInput raw;
  raw.pc.aim = true;
  mapper.synchronize(raw);
  const auto synchronized = mapper.update(raw);
  require(synchronized.aim.held && !synchronized.aim.pressed,
          "Synchronize emitted a phantom press");

  raw.pc.weapon_cycle_delta = 1;
  const auto cycle = mapper.update(raw);
  require(cycle.weapon_menu_delta == 1 && !cycle.next_weapon.pressed &&
              !cycle.previous_weapon.pressed && !cycle.quick_weapon.pressed,
          "Mouse wheel did not stay on the separate weapon-menu path");

  raw.pc.weapon_cycle_delta = 0;
  raw.pc.quick_weapon = true;
  const auto short_switch = mapper.update(raw);
  require(short_switch.quick_weapon.pressed && short_switch.quick_weapon.held &&
              !short_switch.next_weapon.pressed &&
              short_switch.weapon_menu_delta == 0,
          "Middle mouse button did not emit a short retail Select action");
  const auto short_switch_held = mapper.update(raw);
  require(short_switch_held.quick_weapon.held &&
              !short_switch_held.quick_weapon.pressed,
          "Held middle mouse button generated duplicate switch presses");

  raw.pc.quick_weapon = false;
  raw.pc.quick_weapon_keys[9] = true;
  const auto zero = mapper.update(raw);
  require(zero.quick_weapon.released && zero.quick_weapon_slot_pressed == 9U,
          "Keyboard 0 did not map to the tenth quick slot");

  raw.pc.quick_weapon_keys[9] = false;
  raw.pc.quick_weapon = true;
  raw.pc.weapon_cycle_delta = 3;
  const auto simultaneous = mapper.update(raw);
  require(simultaneous.quick_weapon.pressed &&
              simultaneous.weapon_menu_delta == 3 &&
              !simultaneous.next_weapon.pressed &&
              !simultaneous.previous_weapon.pressed,
          "Middle quick-switch swallowed the wheel weapon-ribbon impulse");

  raw.pc.weapon_cycle_delta = 0;
  const auto no_replay = mapper.update(raw);
  require(no_replay.quick_weapon.held && !no_replay.quick_weapon.pressed &&
              no_replay.weapon_menu_delta == 0,
          "Held middle switch or consumed wheel impulse replayed an edge");

  raw = {};
  raw.pc.weapon_menu_next = true;
  const auto remapped_menu_next = mapper.update(raw);
  const auto remapped_menu_held = mapper.update(raw);
  raw.pc.weapon_menu_next = false;
  raw.pc.weapon_menu_previous = true;
  const auto remapped_menu_previous = mapper.update(raw);
  require(remapped_menu_next.weapon_menu_delta == 1 &&
              remapped_menu_held.weapon_menu_delta == 0 &&
              remapped_menu_previous.weapon_menu_delta == -1,
          "Remapped digital weapon-menu actions repeated or used the wrong "
          "direction");
}

void testOriginalControllerActions() {
  sf::platform::PlayerInputMapper mapper;
  sf::platform::RawPlayerInput raw;
  raw.controller.aim = true;
  raw.controller.fire = true;
  raw.controller.roll = true;
  raw.controller.kneel = true;
  raw.controller.interact = true;
  raw.controller.reload = true;
  raw.controller.target_lock = true;
  raw.controller.strafe_left = true;
  raw.controller.change_weapon = true;
  raw.controller.run = true;
  raw.controller.quick_turn = true;

  const auto output = mapper.update(raw);
  require(output.aim.pressed && output.fire.pressed && output.roll.pressed &&
              output.kneel.pressed && output.interact.pressed &&
              output.reload.pressed && output.target_lock.pressed &&
              output.run.pressed && output.quick_turn.pressed &&
              output.quick_weapon.pressed && !output.next_weapon.pressed,
          "Original controller actions did not preserve their press edges");
  require(near(output.move_strafe, -1.0),
          "Original L2 strafe-left action was not mapped");
}

void testPcAimAndOpposingControlConflicts() {
  sf::platform::PlayerInputMapper mapper{sf::platform::PlayerInputConfiguration{
      .mouse_yaw_sensitivity = 0.5,
      .mouse_pitch_sensitivity = 0.25,
  }};
  sf::platform::RawPlayerInput raw;
  raw.pc.turn_left = true;
  raw.pc.run = true;
  raw.pc.mouse_delta_x = 8.0;
  raw.pc.mouse_delta_y = -4.0;

  const auto chase = mapper.update(raw);
  require(near(chase.turn, -1.0) && near(chase.move_strafe, 0.0),
          "A did not turn Gabe while RMB aim was released");
  require(chase.run.held && !chase.aim.held,
          "Left Shift was incorrectly consumed as aim instead of run");

  raw.pc.aim = true;
  raw.pc.fire = true;
  raw.pc.move_forward = true;
  const auto aiming = mapper.update(raw);
  require(aiming.aim.pressed && aiming.fire.pressed && aiming.run.held,
          "RMB aim, LMB fire, and Left Shift run did not remain independent");
  require(near(aiming.move_forward, 1.0) && near(aiming.turn, -1.0) &&
              near(aiming.move_strafe, 0.0),
          "Raw WASD channels were lost before mode-specific routing");
  require(near(aiming.look_yaw, 4.0) && near(aiming.look_pitch, 1.0),
          "RMB mouse aim lost its configured yaw or pitch scaling");
  const auto first_person = sf::platform::firstPersonAimInput(aiming);
  require(near(first_person.mouse_look.yaw, 4.0) &&
              near(first_person.mouse_look.pitch, 1.0) &&
              near(first_person.directional_look_per_guest_tick.yaw, 0.0) &&
              near(first_person.directional_look_per_guest_tick.pitch, 0.0) &&
              near(first_person.move, 1.0) && near(first_person.strafe, -1.0) &&
              near(first_person.peek, 0.0),
          "First-person routing lost WASD locomotion or mouse sight input");

  raw.pc.strafe_right = true;
  const auto corner_peek =
      sf::platform::firstPersonAimInput(mapper.update(raw));
  require(near(corner_peek.move, 1.0) && near(corner_peek.strafe, -1.0) &&
              near(corner_peek.peek, 1.0) &&
              near(corner_peek.mouse_look.yaw, 4.0) &&
              near(corner_peek.mouse_look.pitch, 1.0) &&
              corner_peek.directional_look_per_guest_tick ==
                  first_person.directional_look_per_guest_tick,
          "Dedicated first-person peek was not retained independently of "
          "locomotion and mouse sight");
  raw.pc.strafe_right = false;

  raw.controller.right_x = 1.0;
  raw.controller.right_y = -1.0;
  raw.pc.mouse_delta_x = 0.0;
  raw.pc.mouse_delta_y = 0.0;
  const auto neutral_mouse = mapper.update(raw);
  const auto neutral_first_person =
      sf::platform::firstPersonAimInput(neutral_mouse);
  require(near(neutral_first_person.mouse_look.yaw, 0.0) &&
              near(neutral_first_person.mouse_look.pitch, 0.0) &&
              near(neutral_first_person.directional_look_per_guest_tick.yaw,
                   sf::platform::controller_first_person_yaw_units_per_tick) &&
              near(neutral_first_person.directional_look_per_guest_tick.pitch,
                   -sf::platform::controller_first_person_pitch_units_per_tick) &&
              near(neutral_first_person.move, 1.0) &&
              near(neutral_first_person.strafe, -1.0) &&
              !near(neutral_mouse.controller_look_yaw, 0.0) &&
              !near(neutral_mouse.controller_look_pitch, 0.0),
          "First-person right-stick sight or WASD movement channel was lost");

  mapper.reset();
  raw = {};
  raw.pc.move_forward = true;
  raw.pc.move_backward = true;
  raw.pc.turn_left = true;
  raw.pc.turn_right = true;
  raw.pc.strafe_left = true;
  raw.pc.strafe_right = true;
  raw.pc.previous_weapon = true;
  raw.pc.next_weapon = true;
  raw.controller.strafe_left = true;
  raw.controller.strafe_right = true;
  const auto opposed = mapper.update(raw);
  require(near(opposed.move_forward, 0.0) && near(opposed.turn, 0.0) &&
              near(opposed.move_strafe, 0.0),
          "Opposing movement controls did not cancel deterministically");
  require(
      opposed.previous_weapon.pressed && opposed.next_weapon.pressed,
      "Simultaneous weapon-cycle edges were lost before conflict resolution");
}

void testMergedDeviceActionEdges() {
  sf::platform::PlayerInputMapper mapper;
  sf::platform::RawPlayerInput raw;
  raw.pc.aim = true;
  raw.controller.aim = true;
  raw.pc.fire = true;
  raw.controller.fire = true;

  const auto pressed = mapper.update(raw);
  require(pressed.aim.pressed && pressed.fire.pressed,
          "Merged PC/controller actions did not emit their initial edges");

  raw.pc.aim = false;
  raw.pc.fire = false;
  const auto controller_held = mapper.update(raw);
  require(controller_held.aim.held && !controller_held.aim.pressed &&
              !controller_held.aim.released && controller_held.fire.held &&
              !controller_held.fire.released,
          "Releasing one device manufactured a false merged-action release");

  raw.controller.aim = false;
  raw.controller.fire = false;
  const auto released = mapper.update(raw);
  require(released.aim.released && released.fire.released,
          "Merged action did not release after both devices released it");
}

} // namespace

int main() {
  try {
    testControllerMenuDirectionRejectsDrift();
    testKeyboardMouseBindingCatalog();
    testKeyboardMousePromptText();
    testDeviceAwareInputPromptText();
    testBoundKeyboardMouseActionMatrix();
    testPcActionsAndEdges();
    testFirstPersonMousePrecisionTuning();
    testTurnControllerLookAndInvert();
    testRefreshIndependentLookLatch();
    testPadNeutralAndDisconnect();
    testSynchronizationMenuAndKeyboardZero();
    testOriginalControllerActions();
    testPcAimAndOpposingControlConflicts();
    testMergedDeviceActionEdges();
    std::cout << "player_input_tests: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "player_input_tests: " << error.what() << '\n';
    return 1;
  }
}
