#include "sf/game/controller_bindings.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <string>

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
}

void testEntryRoundTripIsAtomic() {
  using namespace sf::game;
  const auto alternate =
      controllerBindingsForPreset(ControllerBindingPreset::alternate);
  auto entries = controllerBindingEntries(alternate);
  const auto round_trip = controllerBindingsFromEntries(entries);
  require(round_trip && *round_trip == alternate,
          "controller binding entry round-trip failed");

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
    testEntryRoundTripIsAtomic();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
