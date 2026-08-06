#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sf::game {

enum class ControllerAction : std::uint32_t {
  change_weapon,
  shoot,
  kneel,
  roll_zoom_out,
  step_right,
  step_left,
  target_lock,
  use_zoom_in,
  aim,
};

inline constexpr std::size_t controller_action_count = 9U;

inline constexpr std::uint32_t controller_select_button = 0x0001U;
inline constexpr std::uint32_t controller_l2_button = 0x0100U;
inline constexpr std::uint32_t controller_r2_button = 0x0200U;
inline constexpr std::uint32_t controller_l1_button = 0x0400U;
inline constexpr std::uint32_t controller_r1_button = 0x0800U;
inline constexpr std::uint32_t controller_triangle_button = 0x1000U;
inline constexpr std::uint32_t controller_circle_button = 0x2000U;
inline constexpr std::uint32_t controller_cross_button = 0x4000U;
inline constexpr std::uint32_t controller_square_button = 0x8000U;
inline constexpr std::uint32_t bindable_controller_button_mask =
    controller_select_button | controller_l2_button | controller_r2_button |
    controller_l1_button | controller_r1_button |
    controller_triangle_button | controller_circle_button |
    controller_cross_button | controller_square_button;

struct ControllerActionMetadata {
  ControllerAction action{ControllerAction::change_weapon};
  std::string_view name;
  std::string_view config_key;
  std::uint32_t standard_button{};
  std::uint32_t alternate_button{};
};

struct ControllerButtonBindings {
  std::array<std::uint32_t, controller_action_count> buttons{{
      controller_select_button,
      controller_square_button,
      controller_cross_button,
      controller_circle_button,
      controller_r2_button,
      controller_l2_button,
      controller_r1_button,
      controller_triangle_button,
      controller_l1_button,
  }};

  [[nodiscard]] constexpr std::uint32_t
  operator[](ControllerAction action) const noexcept {
    return buttons[static_cast<std::size_t>(action)];
  }

  constexpr std::uint32_t &operator[](ControllerAction action) noexcept {
    return buttons[static_cast<std::size_t>(action)];
  }

  friend bool operator==(const ControllerButtonBindings &,
                         const ControllerButtonBindings &) = default;
};

struct ControllerBinding {
  ControllerAction action{ControllerAction::change_weapon};
  std::uint32_t button{};
};

enum class ControllerBindingPreset {
  standard,
  alternate,
};

enum class ControllerRebindResult {
  invalid,
  unchanged,
  assigned,
  swapped,
};

[[nodiscard]] const std::array<ControllerActionMetadata,
                               controller_action_count> &
controllerActionCatalog() noexcept;
[[nodiscard]] const ControllerActionMetadata &
controllerActionMetadata(ControllerAction action) noexcept;
[[nodiscard]] std::string_view
controllerActionName(ControllerAction action) noexcept;
[[nodiscard]] std::string_view
controllerActionConfigKey(ControllerAction action) noexcept;
[[nodiscard]] bool isValidControllerAction(ControllerAction action) noexcept;
[[nodiscard]] bool isBindableControllerButton(std::uint32_t button) noexcept;
[[nodiscard]] bool
areControllerBindingsValid(const ControllerButtonBindings &bindings) noexcept;
[[nodiscard]] ControllerButtonBindings
controllerBindingsForPreset(ControllerBindingPreset preset) noexcept;
[[nodiscard]] std::uint32_t
controllerButtonForAction(const ControllerButtonBindings &bindings,
                          ControllerAction action) noexcept;
[[nodiscard]] ControllerRebindResult
rebindControllerButton(ControllerButtonBindings &bindings,
                       ControllerAction action, std::uint32_t button) noexcept;
[[nodiscard]] std::array<ControllerBinding, controller_action_count>
controllerBindingEntries(const ControllerButtonBindings &bindings) noexcept;
[[nodiscard]] std::optional<ControllerButtonBindings>
controllerBindingsFromEntries(std::span<const ControllerBinding> entries) noexcept;

} // namespace sf::game
