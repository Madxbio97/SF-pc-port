#include "sf/game/controller_bindings.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
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

void testCanonicalCatalogAndPresets() {
  using namespace sf::game;
  const auto &catalog = controllerActionCatalog();
  require(catalog.size() == controller_action_count,
          "controller action catalog has the wrong size");
  const auto standard =
      controllerBindingsForPreset(ControllerBindingPreset::standard);
  const auto alternate =
      controllerBindingsForPreset(ControllerBindingPreset::alternate);
  for (std::size_t index = 0U; index < catalog.size(); ++index) {
    const auto &metadata = catalog[index];
    require(static_cast<std::size_t>(metadata.action) == index,
            "controller action catalog order is not canonical");
    require(!metadata.name.empty() && !metadata.config_key.empty(),
            "controller action metadata is incomplete");
    require(standard[metadata.action] == metadata.standard_button &&
                alternate[metadata.action] == metadata.alternate_button,
            "preset does not use canonical action metadata");
  }
  require(areControllerBindingsValid(standard) &&
              areControllerBindingsValid(alternate),
          "retail controller preset is invalid");
  require(standard == ControllerButtonBindings{},
          "default controller bindings differ from standard preset");
  require(standard.stick_layout ==
                  ControllerStickLayout::character_left_camera_right &&
              alternate.stick_layout ==
                  ControllerStickLayout::character_left_camera_right,
          "controller presets do not use the default stick layout");
}

void testValidationAndAtomicSwap() {
  using namespace sf::game;
  auto bindings = ControllerButtonBindings{};
  const auto original = bindings;
  require(rebindControllerButton(bindings, ControllerAction::change_weapon,
                                 controller_square_button) ==
              ControllerRebindResult::swapped,
          "occupied controller button was not swapped");
  require(bindings[ControllerAction::change_weapon] ==
                  controller_square_button &&
              bindings[ControllerAction::shoot] == controller_select_button,
          "controller swap did not preserve both actions");
  require(areControllerBindingsValid(bindings),
          "controller swap produced an invalid layout");

  const auto after_swap = bindings;
  require(rebindControllerButton(bindings, ControllerAction::shoot, 0x0010U) ==
                  ControllerRebindResult::invalid &&
              bindings == after_swap,
          "invalid controller rebind mutated the layout");
  require(rebindControllerButton(
              bindings, static_cast<ControllerAction>(controller_action_count),
              controller_cross_button) == ControllerRebindResult::invalid &&
              bindings == after_swap,
          "invalid controller action mutated the layout");

  auto duplicate = original;
  duplicate[ControllerAction::shoot] = controller_select_button;
  require(!areControllerBindingsValid(duplicate),
          "duplicate controller buttons passed validation");
  duplicate = original;
  duplicate[ControllerAction::shoot] = 0x0010U;
  require(!areControllerBindingsValid(duplicate),
          "non-bindable controller button passed validation");

  auto invalid_layout = original;
  invalid_layout.stick_layout = static_cast<ControllerStickLayout>(0xffU);
  require(!areControllerBindingsValid(invalid_layout),
          "invalid controller stick layout passed validation");
}

void testStickLayoutSemantics() {
  using namespace sf::game;
  constexpr auto standard = ControllerStickLayout::character_left_camera_right;
  constexpr auto swapped = ControllerStickLayout::character_right_camera_left;
  constexpr auto original = ControllerStickLayout::original_one_stick;

  require(
      isValidControllerStickLayout(standard) &&
          isValidControllerStickLayout(swapped) &&
          isValidControllerStickLayout(original) &&
          !isValidControllerStickLayout(static_cast<ControllerStickLayout>(3U)),
      "controller stick layout validation is incorrect");
  require(cycledControllerStickLayout(standard) == swapped &&
              cycledControllerStickLayout(swapped) == original &&
              cycledControllerStickLayout(original) == standard &&
              cycledControllerStickLayout(standard, -1) == original &&
              cycledControllerStickLayout(original, -1) == swapped &&
              cycledControllerStickLayout(swapped, -1) == standard,
          "controller stick layout cycle is incorrect");
  require(controllerStickLayoutName(standard) ==
                  "Character Left / Camera Right" &&
              controllerStickLayoutName(swapped) ==
                  "Character Right / Camera Left" &&
              controllerStickLayoutName(original) == "Original (One Stick)",
          "controller stick layout name is incorrect");
}

void testStickLayoutRouting() {
  using namespace sf::game;
  const auto standard_axes = controllerStickAxes(
      ControllerStickLayout::character_left_camera_right, 1U, 2U, 3U, 4U);
  require(standard_axes.character_horizontal == 1U &&
              standard_axes.character_vertical == 2U &&
              standard_axes.camera_horizontal == 3U &&
              standard_axes.camera_vertical == 4U,
          "standard controller stick axes are routed incorrectly");

  const auto swapped_axes = controllerStickAxes(
      ControllerStickLayout::character_right_camera_left, 1U, 2U, 3U, 4U);
  require(swapped_axes.character_horizontal == 3U &&
              swapped_axes.character_vertical == 4U &&
              swapped_axes.camera_horizontal == 1U &&
              swapped_axes.camera_vertical == 2U,
          "swapped controller stick axes are routed incorrectly");

  const auto original_axes = controllerStickAxes(
      ControllerStickLayout::original_one_stick, 1U, 2U, 3U, 4U);
  require(original_axes.character_horizontal == 1U &&
              original_axes.character_vertical == 2U &&
              original_axes.camera_horizontal == 1U &&
              original_axes.camera_vertical == 2U,
          "original one-stick controller axes are routed incorrectly");
}

void testRebindPreservesStickLayout() {
  using namespace sf::game;
  auto bindings = ControllerButtonBindings{};
  bindings.stick_layout = ControllerStickLayout::original_one_stick;
  require(rebindControllerButton(bindings, ControllerAction::change_weapon,
                                 controller_square_button) ==
                  ControllerRebindResult::swapped &&
              bindings.stick_layout ==
                  ControllerStickLayout::original_one_stick,
          "controller rebind changed the stick layout");
}

void testEntryRoundTripIsAtomic() {
  using namespace sf::game;
  const auto alternate =
      controllerBindingsForPreset(ControllerBindingPreset::alternate);
  auto entries = controllerBindingEntries(alternate);
  const auto round_trip = controllerBindingsFromEntries(entries);
  require(round_trip && *round_trip == alternate,
          "controller binding entry round-trip failed");
  require(
      round_trip->stick_layout ==
          ControllerStickLayout::character_left_camera_right,
      "controller entry conversion did not restore the default stick layout");

  std::swap(entries[0], entries[8]);
  const auto reordered = controllerBindingsFromEntries(entries);
  require(reordered && *reordered == alternate,
          "controller entry conversion depends on input order");

  entries[0].action = entries[1].action;
  require(!controllerBindingsFromEntries(entries),
          "duplicate controller action passed conversion");
  require(!controllerBindingsFromEntries(
              std::span<const ControllerBinding>{entries}.first(8U)),
          "incomplete controller layout passed conversion");
}

} // namespace

int main() {
  try {
    testCanonicalCatalogAndPresets();
    testValidationAndAtomicSwap();
    testStickLayoutSemantics();
    testStickLayoutRouting();
    testRebindPreservesStickLayout();
    testEntryRoundTripIsAtomic();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
