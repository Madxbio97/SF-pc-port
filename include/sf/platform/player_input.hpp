#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace sf::platform {

inline constexpr std::size_t quick_weapon_slot_count = 10U;

// Keyboard values intentionally match SDL scancodes (USB HID usages). Mouse
// values occupy a separate stable range so launcher settings remain portable
// and do not depend on Windows virtual-key codes.
enum class KeyboardMouseInput : std::uint16_t {
  none = 0U,
  a = 4U,
  b,
  c,
  d,
  e,
  f,
  g,
  h,
  i,
  j,
  k,
  l,
  m,
  n,
  o,
  p,
  q,
  r,
  s,
  t,
  u,
  v,
  w,
  x,
  y,
  z,
  digit_1,
  digit_2,
  digit_3,
  digit_4,
  digit_5,
  digit_6,
  digit_7,
  digit_8,
  digit_9,
  digit_0,
  enter,
  escape,
  backspace,
  tab,
  space,
  minus,
  equals,
  left_bracket,
  right_bracket,
  backslash,
  non_us_hash,
  semicolon,
  apostrophe,
  grave,
  comma,
  period,
  slash,
  caps_lock,
  f1,
  f2,
  f3,
  f4,
  f5,
  f6,
  f7,
  f8,
  f9,
  f10,
  f11,
  f12,
  print_screen,
  scroll_lock,
  pause,
  insert,
  home,
  page_up,
  delete_key,
  end,
  page_down,
  right,
  left,
  down,
  up,
  num_lock,
  keypad_divide,
  keypad_multiply,
  keypad_minus,
  keypad_plus,
  keypad_enter,
  keypad_1,
  keypad_2,
  keypad_3,
  keypad_4,
  keypad_5,
  keypad_6,
  keypad_7,
  keypad_8,
  keypad_9,
  keypad_0,
  keypad_period,
  non_us_backslash,
  application,
  keypad_equals = 103U,
  f13,
  f14,
  f15,
  f16,
  f17,
  f18,
  f19,
  f20,
  f21,
  f22,
  f23,
  f24,
  left_control = 224U,
  left_shift,
  left_alt,
  left_gui,
  right_control,
  right_shift,
  right_alt,
  right_gui,
  mouse_left = 0x1000U,
  mouse_right,
  mouse_middle,
  mouse_x1,
  mouse_x2,
  mouse_wheel_up,
  mouse_wheel_down,
};

enum class KeyboardMouseAction : std::uint8_t {
  move_forward,
  move_backward,
  turn_left,
  turn_right,
  strafe_left,
  strafe_right,
  run,
  roll,
  reload,
  aim,
  fire,
  crouch,
  interact,
  target_lock,
  quick_turn,
  quick_weapon,
  previous_weapon,
  next_weapon,
  weapon_menu_previous,
  weapon_menu_next,
  pause,
  quick_weapon_1,
  quick_weapon_2,
  quick_weapon_3,
  quick_weapon_4,
  quick_weapon_5,
  quick_weapon_6,
  quick_weapon_7,
  quick_weapon_8,
  quick_weapon_9,
  quick_weapon_10,
  count,
};

inline constexpr auto keyboard_mouse_action_count =
    static_cast<std::size_t>(KeyboardMouseAction::count);

struct KeyboardMouseBindings {
  std::array<KeyboardMouseInput, keyboard_mouse_action_count> values{};

  [[nodiscard]] KeyboardMouseInput
  operator[](KeyboardMouseAction action) const noexcept {
    return values[static_cast<std::size_t>(action)];
  }
  KeyboardMouseInput &operator[](KeyboardMouseAction action) noexcept {
    return values[static_cast<std::size_t>(action)];
  }

  friend bool operator==(const KeyboardMouseBindings &,
                         const KeyboardMouseBindings &) = default;
};

[[nodiscard]] KeyboardMouseBindings defaultKeyboardMouseBindings() noexcept;
[[nodiscard]] bool isValidKeyboardMouseInput(KeyboardMouseInput input) noexcept;
[[nodiscard]] bool isKeyboardInput(KeyboardMouseInput input) noexcept;
[[nodiscard]] std::string_view
keyboardMouseInputName(KeyboardMouseInput input) noexcept;
[[nodiscard]] std::string_view
keyboardMouseActionName(KeyboardMouseAction action) noexcept;
[[nodiscard]] std::string_view
keyboardMouseActionConfigKey(KeyboardMouseAction action) noexcept;

// Retail strings keep their original control tokens so the guest remains the
// authority for message timing and animation. Presentation may replace only
// a recognized prompt token with the launcher's active keyboard/mouse bind.
struct KeyboardMousePromptText {
  std::string retail_text;
  std::string bound_text;

  friend bool operator==(const KeyboardMousePromptText &,
                         const KeyboardMousePromptText &) = default;
};

[[nodiscard]] std::optional<KeyboardMousePromptText>
keyboardMousePromptText(std::string_view source,
                        const KeyboardMouseBindings &bindings);

// Native UI text retains the retail MENU/INIT control tokens in portable
// data.  Resolve them only at presentation time so launcher rebinding is
// reflected by every pause, briefing and save-menu hint without mutating the
// localized strings.  %x is the PC action/confirm binding; %t is the active
// pause binding used for back/cancel/resume.
[[nodiscard]] std::string
keyboardMouseHintText(std::string_view source,
                      const KeyboardMouseBindings &bindings);

// Platform-neutral device snapshot used by the SDL adapter and unit tests.
// KeyboardMouseInput keyboard values are stable SDL/USB scancodes, so the
// platform layer can safely bounds-check them without depending on SDL.
struct KeyboardMouseDeviceState {
  std::span<const std::uint8_t> keyboard;
  bool mouse_left{};
  bool mouse_right{};
  bool mouse_middle{};
  bool mouse_x1{};
  bool mouse_x2{};
  std::int32_t mouse_wheel_delta{};
};

struct KeyboardMouseActionSnapshot {
  std::array<bool, keyboard_mouse_action_count> held{};
  // Non-zero only when this action was activated by a wheel binding. This
  // preserves multi-notch wheel input without repeating ordinary keys.
  std::array<std::int32_t, keyboard_mouse_action_count> wheel_steps{};

  [[nodiscard]] bool operator[](KeyboardMouseAction action) const noexcept {
    const auto index = static_cast<std::size_t>(action);
    return index < held.size() && held[index];
  }
};

[[nodiscard]] KeyboardMouseActionSnapshot
sampleKeyboardMouseActions(const KeyboardMouseBindings &bindings,
                           const KeyboardMouseDeviceState &device) noexcept;

// PC adapter input. quick_weapon_keys are ordered as keyboard 1..9,0. The
// middle button is the short retail Select action; wheel delta drives the
// directional weapon carousel separately instead of manufacturing held input.
struct PcPlayerInputRaw {
  bool move_forward{};
  bool move_backward{};
  bool turn_left{};
  bool turn_right{};
  bool strafe_left{};
  bool strafe_right{};
  bool run{};
  bool roll{};
  bool reload{};
  bool aim{};
  bool fire{};
  bool crouch{};
  bool interact{};
  bool target_lock{};
  bool quick_turn{};
  bool quick_weapon{};
  bool previous_weapon{};
  bool next_weapon{};
  bool weapon_menu_previous{};
  bool weapon_menu_next{};
  std::array<bool, quick_weapon_slot_count> quick_weapon_keys{};
  std::int32_t weapon_cycle_delta{};
  double mouse_delta_x{};
  double mouse_delta_y{};

  friend bool operator==(const PcPlayerInputRaw &,
                         const PcPlayerInputRaw &) = default;
};

[[nodiscard]] PcPlayerInputRaw pcPlayerInputFromKeyboardMouseActions(
    const KeyboardMouseActionSnapshot &actions) noexcept;

// The platform adapter normalizes axes to [-1,1]. Positive left_x/right_x is
// right; positive left_y is forward and positive right_y is look-up. PsyCross
// therefore adapts PADRAW Y axes by negating their normalized values.
struct ControllerPlayerInputRaw {
  double left_x{};
  double left_y{};
  double right_x{};
  double right_y{};
  bool run{};
  bool roll{};
  bool reload{};
  bool aim{};
  bool fire{};
  bool kneel{};
  bool interact{};
  bool target_lock{};
  bool strafe_left{};
  bool strafe_right{};
  bool change_weapon{};
  bool quick_turn{};
  bool previous_weapon{};
  bool next_weapon{};
};

struct RawPlayerInput {
  PcPlayerInputRaw pc;
  ControllerPlayerInputRaw controller;
};

struct PlayerInputConfiguration {
  double movement_deadzone{0.1875};
  double look_deadzone{0.1875};
  double mouse_yaw_sensitivity{1.0};
  double mouse_pitch_sensitivity{1.0};
  double controller_yaw_sensitivity{1.0};
  double controller_pitch_sensitivity{1.0};
  bool invert_pitch{};
};

// Product tuning for raw relative mouse counts in native first-person aim.
// Keep the response linear: acceleration applied to per-frame SDL batches
// would make identical physical travel depend on presentation refresh.
inline constexpr double first_person_mouse_yaw_sensitivity = 3.0;
inline constexpr double first_person_mouse_pitch_sensitivity = 2.75;

struct PlayerActionState {
  bool held{};
  bool pressed{};
  bool released{};

  friend bool operator==(const PlayerActionState &,
                         const PlayerActionState &) = default;
};

// Axis convention: positive forward/strafe/turn/yaw is forward/right/right/
// right; positive pitch is up. In chase mode A/D turns and Q/E strafes.
// During first-person aim W/S and A/D become the retail vertical/horizontal
// sight axes. Q/E remain physical L2/R2 corner movement and relative mouse
// deltas extend the same sight without being quantized to the PS1 stick.
struct PlayerInput {
  double move_forward{};
  double move_strafe{};
  double turn{};
  // Mouse values are relative deltas and must be accumulated until the next
  // guest tick. Controller values are absolute stick samples and must only
  // replace the previous sample; keeping them separate makes aiming
  // independent of the native presentation refresh rate.
  double mouse_look_yaw{};
  double mouse_look_pitch{};
  double controller_look_yaw{};
  double controller_look_pitch{};
  // Combined instantaneous value retained for non-latched consumers.
  double look_yaw{};
  double look_pitch{};
  PlayerActionState run;
  PlayerActionState roll;
  PlayerActionState reload;
  PlayerActionState aim;
  PlayerActionState fire;
  PlayerActionState kneel;
  PlayerActionState interact;
  PlayerActionState target_lock;
  PlayerActionState quick_turn;
  PlayerActionState quick_weapon;
  PlayerActionState previous_weapon;
  PlayerActionState next_weapon;
  std::int32_t weapon_menu_delta{};
  std::array<PlayerActionState, quick_weapon_slot_count> quick_weapon_slots{};
  std::optional<std::uint8_t> quick_weapon_slot_pressed;
};

struct PlayerLookSample {
  double yaw{};
  double pitch{};

  friend bool operator==(const PlayerLookSample &,
                         const PlayerLookSample &) = default;
};

struct FirstPersonAimInput {
  PlayerLookSample mouse_look{};
  PlayerLookSample directional_look_per_guest_tick{};
  double strafe{};
};

// ROM measurement of the USA v1.1 L1 camera after its retail smoothing:
// holding a full horizontal direction advances about ten heading units per
// 20 Hz tick, while the vertical channel advances about thirteen. Keeping the
// rates in heading units lets the display integrator reproduce the unfinished
// guest tick at 60 Hz without changing the authoritative retail PAD state.
inline constexpr double retail_first_person_yaw_units_per_tick = 10.0;
inline constexpr double retail_first_person_pitch_units_per_tick = 13.0;

// First-person routing preserves the original four directional axes and the
// dedicated L2/R2 corner channel. Relative mouse input remains separate so it
// can be accumulated losslessly between retail guest ticks.
[[nodiscard]] FirstPersonAimInput
firstPersonAimInput(const PlayerInput &input) noexcept;

inline constexpr double maximum_presentation_yaw_lead = 128.0;
inline constexpr double maximum_presentation_pitch_lead = 96.0;
inline constexpr double maximum_native_mouse_look_delta = 4096.0;

// The renderer predicts the unfinished guest tick, then reconciles that lead
// against the camera delta actually produced by the guest. The unconsumed
// remainder is held and resubmitted instead of springing back during the next
// 20 Hz interval.
[[nodiscard]] PlayerLookSample
clampPresentationLook(PlayerLookSample sample) noexcept;
[[nodiscard]] PlayerLookSample
reconcilePresentationLook(PlayerLookSample predicted,
                          PlayerLookSample observed) noexcept;
[[nodiscard]] PlayerLookSample
blendPresentationLook(PlayerLookSample reconciliation, PlayerLookSample pending,
                      double guest_tick_fraction) noexcept;
// Presentation-only integration of relative mouse deltas and absolute axes.
// The latter are rates expressed as "units per guest tick" and are integrated
// by real frame time, rather than resampling the latest stick by render alpha.
class PlayerLookDisplayIntegrator final {
public:
  void integrate(PlayerLookSample relative,
                 PlayerLookSample absolute_per_guest_tick,
                 double elapsed_seconds, double guest_tick_seconds) noexcept;
  [[nodiscard]] PlayerLookSample sample() const noexcept;
  // Native first-person presentation is not restricted to the old PS1
  // prediction window. The broad safety bound only protects arithmetic
  // after a badly delayed host frame.
  [[nodiscard]] PlayerLookSample nativeSample() const noexcept;
  void reset() noexcept;

private:
  PlayerLookSample integrated_{};
};

// Preserves the aim state paired with a native fire edge until exactly one
// guest tick consumes that edge. Releasing RMB between display frames must not
// reinterpret an already latched aimed shot as a chase-camera shot.
class PlayerAimFireLatch final {
public:
  void latch(bool aiming, bool fire_pressed) noexcept;
  [[nodiscard]] bool consume(bool aiming, bool fire_pressed) noexcept;
  [[nodiscard]] bool pending() const noexcept { return pending_; }
  void reset() noexcept { pending_ = false; }

private:
  bool pending_{};
};

// Converts arbitrary-rate native samples into one refresh-independent guest
// look sample. Relative mouse motion is integrated; the latest absolute stick
// position is sampled once. A catch-up guest tick reuses only the held stick.
class PlayerLookLatch final {
public:
  void latch(const PlayerInput &input) noexcept;
  [[nodiscard]] PlayerLookSample consumeForGuestTick() noexcept;
  [[nodiscard]] PlayerLookSample controllerForCatchUpTick() const noexcept;
  void reset() noexcept;

private:
  double mouse_yaw_{};
  double mouse_pitch_{};
  double controller_yaw_{};
  double controller_pitch_{};
};

// Stateful only for action edges. synchronize() is intended for gameplay
// entry/resume or an explicit device resynchronization: it latches currently
// held controls without emitting presses.
class PlayerInputMapper final {
public:
  explicit PlayerInputMapper(
      PlayerInputConfiguration configuration = {}) noexcept;

  [[nodiscard]] PlayerInput update(const RawPlayerInput &raw) noexcept;
  void synchronize(const RawPlayerInput &raw) noexcept;
  void reset() noexcept;

  void setConfiguration(PlayerInputConfiguration configuration) noexcept;
  [[nodiscard]] const PlayerInputConfiguration &configuration() const noexcept {
    return configuration_;
  }

private:
  struct DigitalSnapshot {
    bool run{};
    bool roll{};
    bool reload{};
    bool aim{};
    bool fire{};
    bool kneel{};
    bool interact{};
    bool target_lock{};
    bool quick_turn{};
    bool quick_weapon{};
    bool previous_weapon{};
    bool next_weapon{};
    bool weapon_menu_previous{};
    bool weapon_menu_next{};
    std::array<bool, quick_weapon_slot_count> quick_weapon_slots{};
  };

  [[nodiscard]] static DigitalSnapshot
  snapshot(const RawPlayerInput &raw) noexcept;

  PlayerInputConfiguration configuration_;
  DigitalSnapshot previous_{};
};

} // namespace sf::platform
