#include "launcher.hpp"

#include "sf/game/localization.hpp"
#include "sf/game/mission.hpp"

#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <objidl.h>
#ifndef GDIPVER
#define GDIPVER 0x0110
#endif
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sf::platform {
namespace {

constexpr wchar_t launcher_class_name[] = L"SyphonFilterPCGraphicsLauncher";
constexpr wchar_t controls_class_name[] = L"SyphonFilterPCControlsLauncher";
constexpr wchar_t notice_class_name[] = L"SyphonFilterPCLauncherNotice";
constexpr wchar_t dossier_class_name[] = L"SyphonFilterPCDossierArchive";
constexpr int resolution_control_id = 1001;
constexpr int aspect_control_id = 1002;
constexpr int msaa_control_id = 1003;
constexpr int bilinear_control_id = 1004;
constexpr int fullscreen_control_id = 1005;
constexpr int launch_control_id = 1006;
constexpr int cancel_control_id = 1007;
constexpr int mission_control_id = 1008;
constexpr int controls_control_id = 1009;
constexpr int all_weapons_control_id = 1010;
constexpr int anisotropic_control_id = 1011;
constexpr int game_image_control_id = 1012;
constexpr int browse_image_control_id = 1013;
constexpr int dossier_control_id = 1014;
constexpr int language_control_id = 1015;
constexpr int vsync_control_id = 1016;
constexpr int frame_limit_control_id = 1017;
constexpr int binding_list_control_id = 2001;
constexpr int change_binding_control_id = 2002;
constexpr int clear_binding_control_id = 2003;
constexpr int default_bindings_control_id = 2004;
constexpr int close_bindings_control_id = 2005;
constexpr int previous_dossier_control_id = 3001;
constexpr int next_dossier_control_id = 3002;
constexpr int close_dossier_control_id = 3003;
constexpr int dossier_page_control_id = 3004;
constexpr int minimum_resolution_width = 320;
constexpr int minimum_resolution_height = 240;
constexpr int launcher_client_width = 760;
constexpr int launcher_client_height = 580;
constexpr int controls_client_width = 760;
constexpr int controls_client_height = 520;
constexpr int dossier_client_width = 1240;
constexpr int dossier_client_height = 790;
constexpr COLORREF launcher_background_color = RGB(3, 7, 17);
constexpr COLORREF launcher_panel_color = RGB(7, 13, 29);
constexpr COLORREF launcher_grid_color = RGB(18, 28, 61);
constexpr COLORREF launcher_border_color = RGB(80, 103, 196);
constexpr COLORREF launcher_text_color = RGB(174, 190, 255);
constexpr COLORREF launcher_muted_text_color = RGB(103, 126, 190);
constexpr COLORREF launcher_launch_color = RGB(68, 211, 151);
constexpr COLORREF dossier_accent_color = RGB(183, 239, 67);

struct LauncherState {
  GraphicsSettings settings;
  KeyboardMouseBindings input;
  GameplayTestSettings tests;
  game::GameLanguage language{game::GameLanguage::english};
  std::filesystem::path cue_path;
  std::uint32_t mission_index{};
  bool mission_selection_enabled{};
  std::vector<std::pair<int, int>> resolutions;
  std::vector<std::uint32_t> frame_limits;
  HWND mission_combo{};
  HWND resolution_combo{};
  HWND aspect_combo{};
  HWND msaa_combo{};
  HWND frame_limit_combo{};
  HWND game_image_edit{};
  HWND language_combo{};
  int desktop_width{};
  int desktop_height{};
  bool cheats_visible{};
  HFONT title_font{};
  HFONT heading_font{};
  HFONT ui_font{};
  HBRUSH background_brush{};
  HBRUSH panel_brush{};
  HICON large_icon{};
  HICON small_icon{};
  bool accepted{};
  bool finished{};
};

struct ControlsState {
  KeyboardMouseBindings *input{};
  HWND list{};
  HWND change_button{};
  HWND status{};
  HFONT title_font{};
  HFONT heading_font{};
  HFONT ui_font{};
  HBRUSH background_brush{};
  HBRUSH panel_brush{};
  std::size_t selected{};
  std::optional<KeyboardMouseAction> capture;
  bool finished{};
};

struct DossierState {
  std::array<std::filesystem::path, 4U> files;
  std::unique_ptr<Gdiplus::Bitmap> image;
  std::size_t page{};
  HWND previous_button{};
  HWND next_button{};
  HWND close_button{};
  HWND page_label{};
  HFONT title_font{};
  HFONT heading_font{};
  HFONT ui_font{};
  HBRUSH background_brush{};
  HBRUSH panel_brush{};
  bool finished{};
};

std::wstring widenAscii(std::string_view text) {
  return {text.begin(), text.end()};
}

std::filesystem::path executableDirectory() {
  std::array<wchar_t, 32768U> buffer{};
  const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                         static_cast<DWORD>(buffer.size()));
  if (length == 0U || length >= buffer.size()) {
    return std::filesystem::current_path();
  }
  return std::filesystem::path{buffer.data()}.parent_path();
}

bool cheatMarkerExists() noexcept {
  try {
    std::error_code error;
    return std::filesystem::exists(
               executableDirectory() / L"syphon_filter_cheats", error) &&
           !error;
  } catch (...) {
    return false;
  }
}

std::wstring widenUtf8(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const auto required =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), nullptr, 0);
  if (required <= 0) {
    return widenAscii(text);
  }
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), required);
  return result;
}

std::filesystem::path launcherSettingsPath(bool create_directory) {
  std::filesystem::path directory;
  std::array<wchar_t, 32768U> buffer{};
  const auto length = GetEnvironmentVariableW(
      L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length > 0U && length < buffer.size()) {
    directory = std::filesystem::path{buffer.data()} / L"SyphonFilterPC";
  } else {
    const auto module_length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    directory = module_length > 0U && module_length < buffer.size()
                    ? std::filesystem::path{buffer.data()}.parent_path()
                    : std::filesystem::current_path();
  }
  if (create_directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
  }
  return directory / L"launcher.ini";
}

int readProfileInteger(const std::filesystem::path &path,
                       const wchar_t *section, const wchar_t *key,
                       int fallback) noexcept {
  return static_cast<int>(GetPrivateProfileIntW(
      section, key, static_cast<UINT>(fallback), path.c_str()));
}

void writeProfileInteger(const std::filesystem::path &path,
                         const wchar_t *section, const wchar_t *key,
                         int value) {
  const auto text = std::to_wstring(value);
  static_cast<void>(
      WritePrivateProfileStringW(section, key, text.c_str(), path.c_str()));
}

std::filesystem::path loadGameImagePath() {
  const auto path = launcherSettingsPath(false);
  std::array<wchar_t, 32768U> buffer{};
  const auto length =
      GetPrivateProfileStringW(L"Game", L"Image", L"", buffer.data(),
                               static_cast<DWORD>(buffer.size()), path.c_str());
  if (length == 0U || length >= buffer.size() - 1U) {
    return {};
  }
  return std::filesystem::path{buffer.data()};
}

void saveGameImagePath(const std::filesystem::path &cue_path) {
  const auto path = launcherSettingsPath(true);
  static_cast<void>(WritePrivateProfileStringW(L"Game", L"Image",
                                               cue_path.c_str(), path.c_str()));
}

void loadSettingsFile(GraphicsSettings &graphics, KeyboardMouseBindings &input,
                      game::GameLanguage &language) {
  const auto path = launcherSettingsPath(false);
  const auto width =
      readProfileInteger(path, L"Graphics", L"Width", graphics.width);
  const auto height =
      readProfileInteger(path, L"Graphics", L"Height", graphics.height);
  if (width >= minimum_resolution_width &&
      height >= minimum_resolution_height) {
    graphics.width = width;
    graphics.height = height;
  }
  const auto msaa =
      readProfileInteger(path, L"Graphics", L"MSAA", graphics.msaa_samples);
  if (msaa == 0 || msaa == 2 || msaa == 4 || msaa == 8) {
    graphics.msaa_samples = msaa;
  }
  graphics.bilinear_filtering =
      readProfileInteger(path, L"Graphics", L"Bilinear",
                         graphics.bilinear_filtering ? 1 : 0) != 0;
  graphics.anisotropic_filtering =
      readProfileInteger(path, L"Graphics", L"Anisotropic",
                         graphics.anisotropic_filtering ? 1 : 0) != 0;
  graphics.vsync = readProfileInteger(path, L"Graphics", L"VSync",
                                      graphics.vsync ? 1 : 0) != 0;
  const auto frame_limit = readProfileInteger(
      path, L"Graphics", L"FrameLimit", static_cast<int>(graphics.frame_limit));
  if (frame_limit == 0 || (frame_limit >= 20 && frame_limit <= 1000)) {
    graphics.frame_limit = static_cast<std::uint32_t>(frame_limit);
  }
  graphics.fullscreen = readProfileInteger(path, L"Graphics", L"Fullscreen",
                                           graphics.fullscreen ? 1 : 0) != 0;
  graphics.aspect_ratio =
      readProfileInteger(
          path, L"Graphics", L"Aspect",
          graphics.aspect_ratio == AspectRatioMode::adaptive ? 0 : 1) == 0
          ? AspectRatioMode::adaptive
          : AspectRatioMode::original_4_3;
  language = readProfileInteger(path, L"Game", L"Locale", 0) == 1
                 ? game::GameLanguage::russian_vit
                 : game::GameLanguage::english;

  for (std::size_t index = 0U; index < keyboard_mouse_action_count; ++index) {
    const auto action = static_cast<KeyboardMouseAction>(index);
    const auto key = widenAscii(keyboardMouseActionConfigKey(action));
    const auto fallback = static_cast<int>(input[action]);
    const auto loaded = static_cast<KeyboardMouseInput>(
        readProfileInteger(path, L"KeyboardMouse", key.c_str(), fallback));
    if (isValidKeyboardMouseInput(loaded)) {
      input[action] = loaded;
    }
  }
}

void saveSettingsFile(const GraphicsSettings &graphics,
                      const KeyboardMouseBindings &input,
                      game::GameLanguage language,
                      const std::filesystem::path &cue_path) {
  const auto path = launcherSettingsPath(true);
  writeProfileInteger(path, L"Graphics", L"Width", graphics.width);
  writeProfileInteger(path, L"Graphics", L"Height", graphics.height);
  writeProfileInteger(path, L"Graphics", L"MSAA", graphics.msaa_samples);
  writeProfileInteger(path, L"Graphics", L"Bilinear",
                      graphics.bilinear_filtering ? 1 : 0);
  writeProfileInteger(path, L"Graphics", L"Anisotropic",
                      graphics.anisotropic_filtering ? 1 : 0);
  writeProfileInteger(path, L"Graphics", L"VSync", graphics.vsync ? 1 : 0);
  writeProfileInteger(path, L"Graphics", L"FrameLimit",
                      static_cast<int>(graphics.frame_limit));
  writeProfileInteger(path, L"Graphics", L"Fullscreen",
                      graphics.fullscreen ? 1 : 0);
  writeProfileInteger(path, L"Graphics", L"Aspect",
                      graphics.aspect_ratio == AspectRatioMode::adaptive ? 0
                                                                         : 1);
  writeProfileInteger(path, L"Game", L"Locale",
                      language == game::GameLanguage::russian_vit ? 1 : 0);
  for (std::size_t index = 0U; index < keyboard_mouse_action_count; ++index) {
    const auto action = static_cast<KeyboardMouseAction>(index);
    const auto key = widenAscii(keyboardMouseActionConfigKey(action));
    writeProfileInteger(path, L"KeyboardMouse", key.c_str(),
                        static_cast<int>(input[action]));
  }
  saveGameImagePath(cue_path);
}

void populateMissions(LauncherState &state) {
  const auto missions = game::missionCatalog();
  int selected{};
  for (std::size_t index = 0; index < missions.size(); ++index) {
    const auto &mission = missions[index];
    auto label = std::to_wstring(index + 1U) + L". " +
                 widenAscii(mission.title) + L" [" +
                 widenAscii(mission.resource_name) + L"]";
    SendMessageW(state.mission_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
    if (mission.index == state.mission_index) {
      selected = static_cast<int>(index);
    }
  }
  SendMessageW(state.mission_combo, CB_SETCURSEL, static_cast<WPARAM>(selected),
               0);
  SendMessageW(state.mission_combo, CB_SETDROPPEDWIDTH, 320, 0);
  EnableWindow(state.mission_combo,
               state.mission_selection_enabled ? TRUE : FALSE);
}

HWND createControl(HWND parent, const wchar_t *class_name, const wchar_t *text,
                   DWORD style, int x, int y, int width, int height, int id,
                   HFONT font = nullptr) {
  const auto control = CreateWindowExW(
      0, class_name, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height,
      parent, reinterpret_cast<HMENU>(static_cast<std::intptr_t>(id)),
      GetModuleHandleW(nullptr), nullptr);
  if (control != nullptr) {
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(
                     font != nullptr ? font : GetStockObject(DEFAULT_GUI_FONT)),
                 TRUE);
  }
  return control;
}

std::optional<KeyboardMouseInput>
keyboardInputFromVirtualKey(WPARAM virtual_key, LPARAM key_data) noexcept {
  if (virtual_key >= 'A' && virtual_key <= 'Z') {
    return static_cast<KeyboardMouseInput>(
        static_cast<std::uint16_t>(KeyboardMouseInput::a) +
        static_cast<std::uint16_t>(virtual_key - 'A'));
  }
  if (virtual_key >= '1' && virtual_key <= '9') {
    return static_cast<KeyboardMouseInput>(
        static_cast<std::uint16_t>(KeyboardMouseInput::digit_1) +
        static_cast<std::uint16_t>(virtual_key - '1'));
  }
  if (virtual_key == '0') {
    return KeyboardMouseInput::digit_0;
  }
  if (virtual_key >= VK_F1 && virtual_key <= VK_F12) {
    return static_cast<KeyboardMouseInput>(
        static_cast<std::uint16_t>(KeyboardMouseInput::f1) +
        static_cast<std::uint16_t>(virtual_key - VK_F1));
  }
  if (virtual_key >= VK_F13 && virtual_key <= VK_F24) {
    return static_cast<KeyboardMouseInput>(
        static_cast<std::uint16_t>(KeyboardMouseInput::f13) +
        static_cast<std::uint16_t>(virtual_key - VK_F13));
  }
  if (virtual_key >= VK_NUMPAD1 && virtual_key <= VK_NUMPAD9) {
    return static_cast<KeyboardMouseInput>(
        static_cast<std::uint16_t>(KeyboardMouseInput::keypad_1) +
        static_cast<std::uint16_t>(virtual_key - VK_NUMPAD1));
  }
  if (virtual_key == VK_NUMPAD0) {
    return KeyboardMouseInput::keypad_0;
  }

  const auto extended =
      (static_cast<std::uint64_t>(key_data) & (1ULL << 24U)) != 0U;
  if (virtual_key == VK_SHIFT) {
    const auto scan_code = static_cast<UINT>((key_data >> 16U) & 0xffU);
    virtual_key = MapVirtualKeyW(scan_code, MAPVK_VSC_TO_VK_EX);
  } else if (virtual_key == VK_CONTROL) {
    virtual_key = extended ? VK_RCONTROL : VK_LCONTROL;
  } else if (virtual_key == VK_MENU) {
    virtual_key = extended ? VK_RMENU : VK_LMENU;
  }

  switch (virtual_key) {
  case VK_RETURN:
    return extended ? KeyboardMouseInput::keypad_enter
                    : KeyboardMouseInput::enter;
  case VK_ESCAPE:
    return KeyboardMouseInput::escape;
  case VK_BACK:
    return KeyboardMouseInput::backspace;
  case VK_TAB:
    return KeyboardMouseInput::tab;
  case VK_SPACE:
    return KeyboardMouseInput::space;
  case VK_OEM_MINUS:
    return KeyboardMouseInput::minus;
  case VK_OEM_PLUS:
    return KeyboardMouseInput::equals;
  case VK_OEM_4:
    return KeyboardMouseInput::left_bracket;
  case VK_OEM_6:
    return KeyboardMouseInput::right_bracket;
  case VK_OEM_5:
    return KeyboardMouseInput::backslash;
  case VK_OEM_1:
    return KeyboardMouseInput::semicolon;
  case VK_OEM_7:
    return KeyboardMouseInput::apostrophe;
  case VK_OEM_3:
    return KeyboardMouseInput::grave;
  case VK_OEM_COMMA:
    return KeyboardMouseInput::comma;
  case VK_OEM_PERIOD:
    return KeyboardMouseInput::period;
  case VK_OEM_2:
    return KeyboardMouseInput::slash;
  case VK_OEM_102:
    return KeyboardMouseInput::non_us_backslash;
  case VK_CAPITAL:
    return KeyboardMouseInput::caps_lock;
  case VK_SNAPSHOT:
    return KeyboardMouseInput::print_screen;
  case VK_SCROLL:
    return KeyboardMouseInput::scroll_lock;
  case VK_PAUSE:
    return KeyboardMouseInput::pause;
  case VK_INSERT:
    return KeyboardMouseInput::insert;
  case VK_HOME:
    return KeyboardMouseInput::home;
  case VK_PRIOR:
    return KeyboardMouseInput::page_up;
  case VK_DELETE:
    return KeyboardMouseInput::delete_key;
  case VK_END:
    return KeyboardMouseInput::end;
  case VK_NEXT:
    return KeyboardMouseInput::page_down;
  case VK_RIGHT:
    return KeyboardMouseInput::right;
  case VK_LEFT:
    return KeyboardMouseInput::left;
  case VK_DOWN:
    return KeyboardMouseInput::down;
  case VK_UP:
    return KeyboardMouseInput::up;
  case VK_NUMLOCK:
    return KeyboardMouseInput::num_lock;
  case VK_DIVIDE:
    return KeyboardMouseInput::keypad_divide;
  case VK_MULTIPLY:
    return KeyboardMouseInput::keypad_multiply;
  case VK_SUBTRACT:
    return KeyboardMouseInput::keypad_minus;
  case VK_ADD:
    return KeyboardMouseInput::keypad_plus;
  case VK_DECIMAL:
    return KeyboardMouseInput::keypad_period;
  case VK_APPS:
    return KeyboardMouseInput::application;
  case VK_LCONTROL:
    return KeyboardMouseInput::left_control;
  case VK_LSHIFT:
    return KeyboardMouseInput::left_shift;
  case VK_LMENU:
    return KeyboardMouseInput::left_alt;
  case VK_LWIN:
    return KeyboardMouseInput::left_gui;
  case VK_RCONTROL:
    return KeyboardMouseInput::right_control;
  case VK_RSHIFT:
    return KeyboardMouseInput::right_shift;
  case VK_RMENU:
    return KeyboardMouseInput::right_alt;
  case VK_RWIN:
    return KeyboardMouseInput::right_gui;
  default:
    return std::nullopt;
  }
}

std::optional<KeyboardMouseInput> capturedInput(const MSG &message) noexcept {
  if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) &&
      (static_cast<std::uint64_t>(message.lParam) & (1ULL << 30U)) == 0U) {
    return keyboardInputFromVirtualKey(message.wParam, message.lParam);
  }
  switch (message.message) {
  case WM_LBUTTONDOWN:
    return KeyboardMouseInput::mouse_left;
  case WM_RBUTTONDOWN:
    return KeyboardMouseInput::mouse_right;
  case WM_MBUTTONDOWN:
    return KeyboardMouseInput::mouse_middle;
  case WM_XBUTTONDOWN:
    return GET_XBUTTON_WPARAM(message.wParam) == XBUTTON1
               ? KeyboardMouseInput::mouse_x1
               : KeyboardMouseInput::mouse_x2;
  case WM_MOUSEWHEEL:
    return GET_WHEEL_DELTA_WPARAM(message.wParam) >= 0
               ? KeyboardMouseInput::mouse_wheel_up
               : KeyboardMouseInput::mouse_wheel_down;
  default:
    return std::nullopt;
  }
}

void refreshControlsList(ControlsState &state) {
  SendMessageW(state.list, LB_RESETCONTENT, 0, 0);
  for (std::size_t index = 0U; index < keyboard_mouse_action_count; ++index) {
    const auto action = static_cast<KeyboardMouseAction>(index);
    const auto row =
        widenAscii(keyboardMouseActionName(action)) + L"    [" +
        widenAscii(keyboardMouseInputName((*state.input)[action])) + L"]";
    SendMessageW(state.list, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(row.c_str()));
  }
  state.selected = std::min(state.selected, keyboard_mouse_action_count - 1U);
  SendMessageW(state.list, LB_SETCURSEL, static_cast<WPARAM>(state.selected),
               0);
  const auto action = static_cast<KeyboardMouseAction>(state.selected);
  const auto label =
      L"Change: " + widenAscii(keyboardMouseInputName((*state.input)[action]));
  SetWindowTextW(state.change_button, label.c_str());
}

void beginBindingCapture(ControlsState &state) {
  state.capture = static_cast<KeyboardMouseAction>(state.selected);
  SetWindowTextW(state.change_button, L"Waiting for input...");
  SetWindowTextW(
      state.status,
      L"Press any keyboard key, mouse button, or mouse-wheel direction.");
}

void drawTerminalButton(const DRAWITEMSTRUCT &item, HFONT font,
                        COLORREF accent = launcher_border_color) {
  const auto pressed = (item.itemState & ODS_SELECTED) != 0U;
  const auto disabled = (item.itemState & ODS_DISABLED) != 0U;
  const auto fill = pressed ? RGB(24, 39, 77) : launcher_panel_color;
  const auto fill_brush = CreateSolidBrush(fill);
  FillRect(item.hDC, &item.rcItem, fill_brush);
  DeleteObject(fill_brush);

  const auto border_pen =
      CreatePen(PS_SOLID, 2, disabled ? launcher_grid_color : accent);
  const auto old_pen = SelectObject(item.hDC, border_pen);
  SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
  Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right,
            item.rcItem.bottom);
  SelectObject(item.hDC, old_pen);
  DeleteObject(border_pen);

  std::array<wchar_t, 128U> label{};
  GetWindowTextW(item.hwndItem, label.data(), static_cast<int>(label.size()));
  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC,
               disabled ? launcher_muted_text_color : launcher_text_color);
  SelectObject(item.hDC, font);
  auto text_bounds = item.rcItem;
  if (pressed) {
    OffsetRect(&text_bounds, 1, 1);
  }
  DrawTextW(item.hDC, label.data(), -1, &text_bounds,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
  if ((item.itemState & ODS_FOCUS) != 0U) {
    InflateRect(&text_bounds, -4, -4);
    DrawFocusRect(item.hDC, &text_bounds);
  }
}

HICON createLauncherIcon(int size) {
  if (const auto resource =
          LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                     size, size, LR_DEFAULTCOLOR)) {
    return static_cast<HICON>(resource);
  }
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00ff0000U;
  header.bV5GreenMask = 0x0000ff00U;
  header.bV5BlueMask = 0x000000ffU;
  header.bV5AlphaMask = 0xff000000U;
  void *bits{};
  const auto color =
      CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO *>(&header),
                       DIB_RGB_COLORS, &bits, nullptr, 0);
  if (color == nullptr || bits == nullptr) {
    return nullptr;
  }
  auto *pixels = static_cast<std::uint32_t *>(bits);
  for (auto y = 0; y < size; ++y) {
    for (auto x = 0; x < size; ++x) {
      const auto border = x < 2 || y < 2 || x >= size - 2 || y >= size - 2;
      const auto diagonal =
          std::abs(x - y) <= std::max(1, size / 16) ||
          std::abs((size - 1 - x) - y) <= std::max(1, size / 16);
      const auto center = std::abs(x - size / 2) <= std::max(1, size / 12) ||
                          std::abs(y - size / 2) <= std::max(1, size / 12);
      const auto rgb = center && diagonal ? 0x00f29a2eU
                       : border           ? 0x005067c4U
                                          : 0x00070d1dU;
      pixels[static_cast<std::size_t>(y * size + x)] = 0xff000000U | rgb;
    }
  }
  const auto mask = CreateBitmap(size, size, 1, 1, nullptr);
  ICONINFO info{};
  info.fIcon = TRUE;
  info.hbmColor = color;
  info.hbmMask = mask;
  const auto icon = CreateIconIndirect(&info);
  DeleteObject(mask);
  DeleteObject(color);
  return icon;
}

struct NoticeState {
  std::wstring title;
  std::wstring message;
  HFONT title_font{};
  HFONT ui_font{};
  HBRUSH background_brush{};
  HBRUSH panel_brush{};
  bool finished{};
};

LRESULT CALLBACK noticeWindowProc(HWND window, UINT message, WPARAM w_param,
                                  LPARAM l_param) {
  auto *state =
      reinterpret_cast<NoticeState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(l_param);
    state = static_cast<NoticeState *>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) {
    return DefWindowProcW(window, message, w_param, l_param);
  }
  switch (message) {
  case WM_CREATE:
    createControl(window, L"BUTTON", L"ACKNOWLEDGE", WS_TABSTOP | BS_OWNERDRAW,
                  314, 176, 138, 34, IDOK, state->title_font);
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT paint{};
    const auto dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, state->background_brush);
    const auto grid = CreatePen(PS_SOLID, 1, launcher_grid_color);
    const auto old_pen = SelectObject(dc, grid);
    for (auto x = 0; x < client.right; x += 24) {
      MoveToEx(dc, x, 58, nullptr);
      LineTo(dc, x, client.bottom);
    }
    for (auto y = 58; y < client.bottom; y += 24) {
      MoveToEx(dc, 0, y, nullptr);
      LineTo(dc, client.right, y);
    }
    RECT panel{20, 64, client.right - 20, 158};
    FillRect(dc, &panel, state->panel_brush);
    const auto border = CreatePen(PS_SOLID, 2, launcher_border_color);
    SelectObject(dc, border);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, panel.left, panel.top, panel.right, panel.bottom);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, launcher_text_color);
    SelectObject(dc, state->title_font);
    RECT title{22, 14, client.right - 22, 50};
    DrawTextW(dc, state->title.c_str(), -1, &title,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, state->ui_font);
    RECT body{34, 78, client.right - 34, 146};
    DrawTextW(dc, state->message.c_str(), -1, &body,
              DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old_pen);
    DeleteObject(border);
    DeleteObject(grid);
    EndPaint(window, &paint);
    return 0;
  }
  case WM_ERASEBKGND:
    return 1;
  case WM_DRAWITEM: {
    const auto *item = reinterpret_cast<const DRAWITEMSTRUCT *>(l_param);
    if (item != nullptr && item->CtlID == IDOK) {
      drawTerminalButton(*item, state->title_font, launcher_launch_color);
      return TRUE;
    }
    break;
  }
  case WM_COMMAND:
    if (LOWORD(w_param) == IDOK) {
      DestroyWindow(window);
      return 0;
    }
    break;
  case WM_CLOSE:
    DestroyWindow(window);
    return 0;
  case WM_DESTROY:
    state->finished = true;
    return 0;
  default:
    break;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

void showStyledNotice(HWND owner, std::wstring title, std::wstring message) {
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = noticeWindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.lpszClassName = notice_class_name;
  if (RegisterClassW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    MessageBoxW(owner, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
    return;
  }

  NoticeState state{};
  state.title = std::move(title);
  state.message = std::move(message);
  state.title_font =
      CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift SemiCondensed");
  state.ui_font =
      CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift");
  state.background_brush = CreateSolidBrush(launcher_background_color);
  state.panel_brush = CreateSolidBrush(launcher_panel_color);
  constexpr DWORD style = WS_CAPTION | WS_SYSMENU;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  RECT bounds{0, 0, 480, 230};
  AdjustWindowRectEx(&bounds, style, FALSE, extended_style);
  const auto window = CreateWindowExW(
      extended_style, notice_class_name, state.title.c_str(), style,
      CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
      bounds.bottom - bounds.top, owner, nullptr, instance, &state);
  if (window != nullptr) {
    RECT reference{};
    if (owner != nullptr) {
      GetWindowRect(owner, &reference);
      EnableWindow(owner, FALSE);
    } else {
      SystemParametersInfoW(SPI_GETWORKAREA, 0, &reference, 0);
    }
    RECT window_bounds{};
    GetWindowRect(window, &window_bounds);
    const auto width = window_bounds.right - window_bounds.left;
    const auto height = window_bounds.bottom - window_bounds.top;
    SetWindowPos(
        window, HWND_TOP,
        reference.left + (reference.right - reference.left - width) / 2,
        reference.top + (reference.bottom - reference.top - height) / 2, 0, 0,
        SWP_NOSIZE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG event{};
    while (!state.finished && GetMessageW(&event, nullptr, 0, 0) > 0) {
      if (IsDialogMessageW(window, &event) == FALSE) {
        TranslateMessage(&event);
        DispatchMessageW(&event);
      }
    }
    if (owner != nullptr) {
      EnableWindow(owner, TRUE);
      SetForegroundWindow(owner);
    }
  }
  DeleteObject(state.panel_brush);
  DeleteObject(state.background_brush);
  DeleteObject(state.ui_font);
  DeleteObject(state.title_font);
}

bool isControlsOwnerDrawButton(UINT id) noexcept {
  return id == change_binding_control_id || id == clear_binding_control_id ||
         id == default_bindings_control_id || id == close_bindings_control_id;
}

void drawControlsFrame(HWND window, ControlsState &state) {
  PAINTSTRUCT paint{};
  const auto dc = BeginPaint(window, &paint);
  RECT client{};
  GetClientRect(window, &client);
  FillRect(dc, &client, state.background_brush);

  const auto grid_pen = CreatePen(PS_SOLID, 1, launcher_grid_color);
  const auto old_pen = SelectObject(dc, grid_pen);
  for (auto x = 0; x < client.right; x += 28) {
    MoveToEx(dc, x, 94, nullptr);
    LineTo(dc, x, client.bottom);
  }
  for (auto y = 94; y < client.bottom; y += 28) {
    MoveToEx(dc, 0, y, nullptr);
    LineTo(dc, client.right, y);
  }

  RECT bindings_panel{24, 110, 432, 492};
  RECT action_panel{452, 110, 736, 492};
  FillRect(dc, &bindings_panel, state.panel_brush);
  FillRect(dc, &action_panel, state.panel_brush);
  const auto border_pen = CreatePen(PS_SOLID, 2, launcher_border_color);
  SelectObject(dc, border_pen);
  SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, bindings_panel.left, bindings_panel.top, bindings_panel.right,
            bindings_panel.bottom);
  Rectangle(dc, action_panel.left, action_panel.top, action_panel.right,
            action_panel.bottom);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, launcher_text_color);
  SelectObject(dc, state.title_font);
  RECT title{28, 18, client.right - 28, 60};
  DrawTextW(dc, L"INPUT CONFIGURATION", -1, &title,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  SetTextColor(dc, launcher_muted_text_color);
  SelectObject(dc, state.ui_font);
  RECT subtitle{31, 60, client.right - 28, 86};
  DrawTextW(dc, L"AGENCY FIELD TERMINAL  /  KEYBOARD + MOUSE", -1, &subtitle,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  SelectObject(dc, old_pen);
  DeleteObject(border_pen);
  DeleteObject(grid_pen);
  EndPaint(window, &paint);
}

LRESULT CALLBACK controlsWindowProc(HWND window, UINT message, WPARAM w_param,
                                    LPARAM l_param) {
  auto *state = reinterpret_cast<ControlsState *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(l_param);
    state = static_cast<ControlsState *>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) {
    return DefWindowProcW(window, message, w_param, l_param);
  }

  switch (message) {
  case WM_CREATE:
    createControl(window, L"STATIC", L"CONTROL ASSIGNMENTS", 0, 44, 122, 360,
                  26, 0, state->heading_font);
    createControl(window, L"STATIC",
                  L"Double-click an action to capture a new input.", 0, 44, 150,
                  360, 22, 0, state->ui_font);
    state->list = createControl(
        window, L"LISTBOX", L"",
        WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
        44, 180, 368, 286, binding_list_control_id, state->ui_font);
    createControl(window, L"STATIC", L"BINDING CONTROL", 0, 472, 122, 244, 26,
                  0, state->heading_font);
    state->change_button = createControl(
        window, L"BUTTON", L"Change", WS_TABSTOP | BS_OWNERDRAW, 472, 166, 244,
        42, change_binding_control_id, state->heading_font);
    createControl(window, L"BUTTON", L"Clear binding",
                  WS_TABSTOP | BS_OWNERDRAW, 472, 220, 244, 38,
                  clear_binding_control_id, state->heading_font);
    createControl(window, L"BUTTON", L"Restore defaults",
                  WS_TABSTOP | BS_OWNERDRAW, 472, 270, 244, 38,
                  default_bindings_control_id, state->heading_font);
    state->status = createControl(window, L"STATIC",
                                  L"Select an action, then press Change.", 0,
                                  472, 332, 244, 62, 0, state->ui_font);
    createControl(window, L"STATIC",
                  L"Stealth: crouch + movement\nSide roll: roll + strafe", 0,
                  472, 398, 244, 44, 0, state->ui_font);
    createControl(window, L"BUTTON", L"APPLY", WS_TABSTOP | BS_OWNERDRAW, 472,
                  448, 244, 34, close_bindings_control_id, state->heading_font);
    refreshControlsList(*state);
    return 0;
  case WM_PAINT:
    drawControlsFrame(window, *state);
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_CTLCOLORSTATIC: {
    const auto dc = reinterpret_cast<HDC>(w_param);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, launcher_text_color);
    return reinterpret_cast<LRESULT>(state->panel_brush);
  }
  case WM_CTLCOLORLISTBOX: {
    const auto dc = reinterpret_cast<HDC>(w_param);
    SetBkColor(dc, launcher_panel_color);
    SetTextColor(dc, launcher_text_color);
    return reinterpret_cast<LRESULT>(state->panel_brush);
  }
  case WM_DRAWITEM: {
    const auto *item = reinterpret_cast<const DRAWITEMSTRUCT *>(l_param);
    if (item != nullptr && item->CtlType == ODT_BUTTON &&
        isControlsOwnerDrawButton(item->CtlID)) {
      drawTerminalButton(*item, state->heading_font,
                         item->CtlID == close_bindings_control_id
                             ? launcher_launch_color
                             : launcher_border_color);
      return TRUE;
    }
    break;
  }
  case WM_COMMAND:
    if (LOWORD(w_param) == binding_list_control_id) {
      if (HIWORD(w_param) == LBN_SELCHANGE || HIWORD(w_param) == LBN_DBLCLK) {
        const auto selected = SendMessageW(state->list, LB_GETCURSEL, 0, 0);
        if (selected >= 0 &&
            static_cast<std::size_t>(selected) < keyboard_mouse_action_count) {
          state->selected = static_cast<std::size_t>(selected);
          state->capture.reset();
          refreshControlsList(*state);
          SetWindowTextW(state->status,
                         L"Select an action, then press Change.");
          if (HIWORD(w_param) == LBN_DBLCLK) {
            beginBindingCapture(*state);
          }
        }
        return 0;
      }
    }
    if (HIWORD(w_param) != BN_CLICKED) {
      return 0;
    }
    if (LOWORD(w_param) == change_binding_control_id) {
      beginBindingCapture(*state);
      return 0;
    }
    if (LOWORD(w_param) == clear_binding_control_id) {
      (*state->input)[static_cast<KeyboardMouseAction>(state->selected)] =
          KeyboardMouseInput::none;
      state->capture.reset();
      refreshControlsList(*state);
      SetWindowTextW(state->status, L"Binding cleared.");
      return 0;
    }
    if (LOWORD(w_param) == default_bindings_control_id) {
      *state->input = defaultKeyboardMouseBindings();
      state->capture.reset();
      refreshControlsList(*state);
      SetWindowTextW(state->status, L"Default controls restored.");
      return 0;
    }
    if (LOWORD(w_param) == close_bindings_control_id) {
      DestroyWindow(window);
      return 0;
    }
    break;
  case WM_CLOSE:
    DestroyWindow(window);
    return 0;
  case WM_DESTROY:
    state->finished = true;
    return 0;
  default:
    break;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

void showControlsWindow(HWND owner, KeyboardMouseBindings &input) {
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = controlsWindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.hbrBackground = nullptr;
  window_class.lpszClassName = controls_class_name;
  if (RegisterClassW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;
  }

  constexpr DWORD style = WS_CAPTION | WS_SYSMENU;
  constexpr DWORD extended_style = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME;
  RECT bounds{0, 0, controls_client_width, controls_client_height};
  AdjustWindowRectEx(&bounds, style, FALSE, extended_style);
  ControlsState state{.input = &input};
  state.title_font =
      CreateFontW(-32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift SemiCondensed");
  state.heading_font =
      CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift SemiCondensed");
  state.ui_font =
      CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift");
  state.background_brush = CreateSolidBrush(launcher_background_color);
  state.panel_brush = CreateSolidBrush(launcher_panel_color);
  const auto window = CreateWindowExW(
      extended_style, controls_class_name,
      L"Syphon Filter PC - Keyboard + Mouse Controls", style, CW_USEDEFAULT,
      CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
      owner, nullptr, instance, &state);
  if (window == nullptr) {
    DeleteObject(state.panel_brush);
    DeleteObject(state.background_brush);
    DeleteObject(state.ui_font);
    DeleteObject(state.heading_font);
    DeleteObject(state.title_font);
    return;
  }

  RECT owner_bounds{};
  RECT window_bounds{};
  if (GetWindowRect(owner, &owner_bounds) != FALSE &&
      GetWindowRect(window, &window_bounds) != FALSE) {
    const auto width = window_bounds.right - window_bounds.left;
    const auto height = window_bounds.bottom - window_bounds.top;
    SetWindowPos(window, nullptr,
                 owner_bounds.left +
                     (owner_bounds.right - owner_bounds.left - width) / 2,
                 owner_bounds.top +
                     (owner_bounds.bottom - owner_bounds.top - height) / 2,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  EnableWindow(owner, FALSE);
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);

  MSG message{};
  while (!state.finished && GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (state.capture) {
      if (const auto captured = capturedInput(message)) {
        (*state.input)[*state.capture] = *captured;
        state.capture.reset();
        refreshControlsList(state);
        SetWindowTextW(state.status, L"Binding updated.");
        continue;
      }
    }
    if (IsDialogMessageW(window, &message) == FALSE) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  DeleteObject(state.panel_brush);
  DeleteObject(state.background_brush);
  DeleteObject(state.ui_font);
  DeleteObject(state.heading_font);
  DeleteObject(state.title_font);
  EnableWindow(owner, TRUE);
  SetForegroundWindow(owner);
}

std::array<std::filesystem::path, 4U> dossierFiles() {
  auto directory = executableDirectory() / L"assets" / L"dossiers" / L"screens";
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error) || error) {
    directory = std::filesystem::current_path(error) / L"assets" / L"dossiers" /
                L"screens";
  }
  return {
      directory / L"dossier_01.png",
      directory / L"dossier_02.png",
      directory / L"dossier_03.png",
      directory / L"dossier_04.png",
  };
}

RECT dossierImagePanel(HWND window) {
  RECT client{};
  GetClientRect(window, &client);
  return RECT{24, 82, std::max(25L, client.right - 24),
              std::max(83L, client.bottom - 70)};
}

void layoutDossierControls(HWND window, DossierState &state) {
  RECT client{};
  GetClientRect(window, &client);
  const auto button_y = std::max(0L, client.bottom - 52);
  MoveWindow(state.previous_button, 24, button_y, 126, 34, TRUE);
  MoveWindow(state.next_button, 162, button_y, 96, 34, TRUE);
  MoveWindow(state.page_label, std::max(270L, client.right / 2 - 90),
             button_y + 2, 180, 30, TRUE);
  MoveWindow(state.close_button, std::max(282L, client.right - 150), button_y,
             126, 34, TRUE);
}

void updateDossierNavigation(DossierState &state) {
  const auto label = L"FILE " + std::to_wstring(state.page + 1U) + L" / " +
                     std::to_wstring(state.files.size());
  SetWindowTextW(state.page_label, label.c_str());
  EnableWindow(state.previous_button, state.page > 0U ? TRUE : FALSE);
  EnableWindow(state.next_button,
               state.page + 1U < state.files.size() ? TRUE : FALSE);
}

bool loadDossierPage(DossierState &state, std::size_t page) {
  if (page >= state.files.size()) {
    return false;
  }
  auto image = std::make_unique<Gdiplus::Bitmap>(state.files[page].c_str());
  if (image->GetLastStatus() != Gdiplus::Ok || image->GetWidth() == 0U ||
      image->GetHeight() == 0U) {
    return false;
  }
  // The source pages contain fine terminal text and are normally downscaled
  // in the viewer. A restrained GDI+ 1.1 sharpen pass keeps that text crisp
  // without changing the artwork or introducing visible halos.
  Gdiplus::Sharpen sharpen;
  const Gdiplus::SharpenParams parameters{1.0F, 14.0F};
  if (sharpen.SetParameters(&parameters) == Gdiplus::Ok) {
    RECT region{0, 0, static_cast<LONG>(image->GetWidth()),
                static_cast<LONG>(image->GetHeight())};
    static_cast<void>(image->ApplyEffect(&sharpen, &region));
  }
  state.image = std::move(image);
  state.page = page;
  if (state.page_label != nullptr) {
    updateDossierNavigation(state);
  }
  return true;
}

void drawDossierFrame(HWND window, DossierState &state) {
  PAINTSTRUCT paint{};
  const auto dc = BeginPaint(window, &paint);
  RECT client{};
  GetClientRect(window, &client);
  FillRect(dc, &client, state.background_brush);

  const auto grid_pen = CreatePen(PS_SOLID, 1, launcher_grid_color);
  const auto old_pen = SelectObject(dc, grid_pen);
  for (auto x = 0; x < client.right; x += 32) {
    MoveToEx(dc, x, 70, nullptr);
    LineTo(dc, x, client.bottom);
  }
  for (auto y = 70; y < client.bottom; y += 32) {
    MoveToEx(dc, 0, y, nullptr);
    LineTo(dc, client.right, y);
  }

  const auto panel = dossierImagePanel(window);
  FillRect(dc, &panel, state.panel_brush);
  const auto border_pen = CreatePen(PS_SOLID, 2, dossier_accent_color);
  SelectObject(dc, border_pen);
  SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, panel.left, panel.top, panel.right, panel.bottom);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, dossier_accent_color);
  SelectObject(dc, state.title_font);
  RECT title{28, 12, client.right - 28, 50};
  DrawTextW(dc, L"DOSSIERS", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  SetTextColor(dc, launcher_text_color);
  SelectObject(dc, state.ui_font);
  RECT subtitle{216, 17, client.right - 28, 49};
  DrawTextW(dc, L"AGENCY  /  CLASSIFIED ARCHIVE", -1, &subtitle,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  const auto accent_pen = CreatePen(PS_SOLID, 1, dossier_accent_color);
  SelectObject(dc, accent_pen);
  MoveToEx(dc, 28, 59, nullptr);
  LineTo(dc, client.right - 28, 59);

  if (state.image != nullptr) {
    constexpr int padding = 10;
    const auto available_width = static_cast<int>(
        std::max<LONG>(1L, panel.right - panel.left - padding * 2));
    const auto available_height = static_cast<int>(
        std::max<LONG>(1L, panel.bottom - panel.top - padding * 2));
    const auto image_width = static_cast<double>(state.image->GetWidth());
    const auto image_height = static_cast<double>(state.image->GetHeight());
    const auto scale =
        std::min(static_cast<double>(available_width) / image_width,
                 static_cast<double>(available_height) / image_height);
    const auto width =
        std::max(1, static_cast<int>(std::lround(image_width * scale)));
    const auto height =
        std::max(1, static_cast<int>(std::lround(image_height * scale)));
    const auto x = panel.left + (panel.right - panel.left - width) / 2;
    const auto y = panel.top + (panel.bottom - panel.top - height) / 2;
    Gdiplus::Graphics graphics{dc};
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.DrawImage(state.image.get(), x, y, width, height);
  }

  SetTextColor(dc, launcher_muted_text_color);
  SelectObject(dc, state.ui_font);
  RECT hint{276, client.bottom - 52, client.right - 168, client.bottom - 18};
  DrawTextW(dc, L"A / D  OR  LEFT / RIGHT  //  CHANGE FILE", -1, &hint,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

  SelectObject(dc, old_pen);
  DeleteObject(accent_pen);
  DeleteObject(border_pen);
  DeleteObject(grid_pen);
  EndPaint(window, &paint);
}

LRESULT CALLBACK dossierWindowProc(HWND window, UINT message, WPARAM w_param,
                                   LPARAM l_param) {
  auto *state = reinterpret_cast<DossierState *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(l_param);
    state = static_cast<DossierState *>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) {
    return DefWindowProcW(window, message, w_param, l_param);
  }

  switch (message) {
  case WM_CREATE:
    state->previous_button = createControl(
        window, L"BUTTON", L"PREVIOUS", WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
        previous_dossier_control_id, state->heading_font);
    state->next_button =
        createControl(window, L"BUTTON", L"NEXT", WS_TABSTOP | BS_OWNERDRAW, 0,
                      0, 0, 0, next_dossier_control_id, state->heading_font);
    state->close_button =
        createControl(window, L"BUTTON", L"CLOSE", WS_TABSTOP | BS_OWNERDRAW, 0,
                      0, 0, 0, close_dossier_control_id, state->heading_font);
    state->page_label =
        createControl(window, L"STATIC", L"", SS_CENTER, 0, 0, 0, 0,
                      dossier_page_control_id, state->heading_font);
    layoutDossierControls(window, *state);
    updateDossierNavigation(*state);
    return 0;
  case WM_PAINT:
    drawDossierFrame(window, *state);
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_SIZE:
    layoutDossierControls(window, *state);
    InvalidateRect(window, nullptr, FALSE);
    return 0;
  case WM_GETMINMAXINFO: {
    auto *limits = reinterpret_cast<MINMAXINFO *>(l_param);
    limits->ptMinTrackSize.x = 760;
    limits->ptMinTrackSize.y = 520;
    return 0;
  }
  case WM_CTLCOLORSTATIC: {
    const auto dc = reinterpret_cast<HDC>(w_param);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, dossier_accent_color);
    return reinterpret_cast<LRESULT>(state->background_brush);
  }
  case WM_DRAWITEM: {
    const auto *item = reinterpret_cast<const DRAWITEMSTRUCT *>(l_param);
    if (item != nullptr && item->CtlType == ODT_BUTTON) {
      drawTerminalButton(*item, state->heading_font, dossier_accent_color);
      return TRUE;
    }
    break;
  }
  case WM_COMMAND:
    if (HIWORD(w_param) != BN_CLICKED) {
      return 0;
    }
    if (LOWORD(w_param) == previous_dossier_control_id && state->page > 0U) {
      if (loadDossierPage(*state, state->page - 1U)) {
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    if (LOWORD(w_param) == next_dossier_control_id &&
        state->page + 1U < state->files.size()) {
      if (loadDossierPage(*state, state->page + 1U)) {
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    if (LOWORD(w_param) == close_dossier_control_id) {
      DestroyWindow(window);
      return 0;
    }
    break;
  case WM_KEYDOWN:
    if ((w_param == VK_LEFT || w_param == 'A') && state->page > 0U) {
      if (loadDossierPage(*state, state->page - 1U)) {
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    if ((w_param == VK_RIGHT || w_param == 'D') &&
        state->page + 1U < state->files.size()) {
      if (loadDossierPage(*state, state->page + 1U)) {
        InvalidateRect(window, nullptr, FALSE);
      }
      return 0;
    }
    if (w_param == VK_ESCAPE) {
      DestroyWindow(window);
      return 0;
    }
    break;
  case WM_CLOSE:
    DestroyWindow(window);
    return 0;
  case WM_DESTROY:
    state->finished = true;
    return 0;
  default:
    break;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

void showDossierWindow(HWND owner) {
  Gdiplus::GdiplusStartupInputEx startup_input{};
  ULONG_PTR gdiplus_token{};
  if (Gdiplus::GdiplusStartup(&gdiplus_token, &startup_input, nullptr) !=
      Gdiplus::Ok) {
    showStyledNotice(
        owner, L"DOSSIER ARCHIVE UNAVAILABLE",
        L"Windows could not initialize the dossier image decoder.");
    return;
  }

  DossierState state{};
  state.files = dossierFiles();
  const auto missing = std::ranges::find_if(state.files, [](const auto &path) {
    std::error_code error;
    return !std::filesystem::is_regular_file(path, error) || error;
  });
  if (missing != state.files.end() || !loadDossierPage(state, 0U)) {
    Gdiplus::GdiplusShutdown(gdiplus_token);
    showStyledNotice(owner, L"DOSSIER ARCHIVE UNAVAILABLE",
                     L"The dossier image files are missing or damaged.");
    return;
  }

  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = dossierWindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.hbrBackground = nullptr;
  window_class.lpszClassName = dossier_class_name;
  if (RegisterClassW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    state.image.reset();
    Gdiplus::GdiplusShutdown(gdiplus_token);
    showStyledNotice(owner, L"DOSSIER ARCHIVE UNAVAILABLE",
                     L"Windows could not create the dossier viewer.");
    return;
  }

  state.title_font =
      CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift SemiCondensed");
  state.heading_font =
      CreateFontW(-17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift SemiCondensed");
  state.ui_font =
      CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift");
  state.background_brush = CreateSolidBrush(launcher_background_color);
  state.panel_brush = CreateSolidBrush(launcher_panel_color);

  RECT work_area{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
  const auto work_width = static_cast<int>(work_area.right - work_area.left);
  const auto work_height = static_cast<int>(work_area.bottom - work_area.top);
  const auto client_width =
      std::min(dossier_client_width, std::max(760, work_width - 48));
  const auto client_height =
      std::min(dossier_client_height, std::max(520, work_height - 48));
  constexpr DWORD style =
      WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX;
  constexpr DWORD extended_style = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
  RECT bounds{0, 0, client_width, client_height};
  AdjustWindowRectEx(&bounds, style, FALSE, extended_style);
  const auto window = CreateWindowExW(
      extended_style, dossier_class_name, L"Syphon Filter PC - Dossiers", style,
      CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
      bounds.bottom - bounds.top, owner, nullptr, instance, &state);
  if (window != nullptr) {
    RECT window_bounds{};
    GetWindowRect(window, &window_bounds);
    const auto width = window_bounds.right - window_bounds.left;
    const auto height = window_bounds.bottom - window_bounds.top;
    SetWindowPos(
        window, HWND_TOP,
        work_area.left + (work_area.right - work_area.left - width) / 2,
        work_area.top + (work_area.bottom - work_area.top - height) / 2, 0, 0,
        SWP_NOSIZE);
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (!state.finished && GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.message == WM_KEYDOWN &&
          (message.wParam == VK_LEFT || message.wParam == VK_RIGHT ||
           message.wParam == VK_ESCAPE || message.wParam == 'A' ||
           message.wParam == 'D')) {
        SendMessageW(window, WM_KEYDOWN, message.wParam, message.lParam);
        continue;
      }
      if (IsDialogMessageW(window, &message) == FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }

  state.image.reset();
  DeleteObject(state.panel_brush);
  DeleteObject(state.background_brush);
  DeleteObject(state.ui_font);
  DeleteObject(state.heading_font);
  DeleteObject(state.title_font);
  Gdiplus::GdiplusShutdown(gdiplus_token);
}

std::optional<int> parseResolutionDimension(std::wstring_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  int value{};
  for (const auto character : text) {
    if (character < L'0' || character > L'9') {
      return std::nullopt;
    }
    const auto digit = static_cast<int>(character - L'0');
    if (value > (std::numeric_limits<int>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  return value;
}

std::optional<std::pair<int, int>>
parseResolutionText(std::wstring_view text) noexcept {
  std::wstring compact;
  compact.reserve(text.size());
  for (const auto character : text) {
    if (character != L' ' && character != L'\t') {
      compact.push_back(character);
    }
  }

  const auto separator = compact.find_first_of(L"xX\u00d7");
  if (separator == std::wstring::npos ||
      compact.find_first_of(L"xX\u00d7", separator + 1U) !=
          std::wstring::npos) {
    return std::nullopt;
  }
  const auto width = parseResolutionDimension(
      std::wstring_view{compact}.substr(0U, separator));
  const auto height = parseResolutionDimension(
      std::wstring_view{compact}.substr(separator + 1U));
  if (!width || !height || *width < minimum_resolution_width ||
      *height < minimum_resolution_height) {
    return std::nullopt;
  }
  return std::pair{*width, *height};
}

std::optional<std::pair<int, int>>
selectedResolution(const LauncherState &state) noexcept {
  const auto text_length = GetWindowTextLengthW(state.resolution_combo);
  if (text_length <= 0) {
    return std::nullopt;
  }
  std::wstring text(static_cast<std::size_t>(text_length) + 1U, L'\0');
  const auto copied = GetWindowTextW(state.resolution_combo, text.data(),
                                     static_cast<int>(text.size()));
  if (copied <= 0) {
    return std::nullopt;
  }
  text.resize(static_cast<std::size_t>(copied));
  return parseResolutionText(text);
}

void populateResolutions(LauncherState &state) {
  std::set<std::pair<int, int>> unique_resolutions;
  DEVMODEW current_mode{};
  current_mode.dmSize = sizeof(current_mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current_mode) !=
      FALSE) {
    state.desktop_width = static_cast<int>(current_mode.dmPelsWidth);
    state.desktop_height = static_cast<int>(current_mode.dmPelsHeight);
  }
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  for (DWORD index = 0; EnumDisplaySettingsW(nullptr, index, &mode) != FALSE;
       ++index) {
    if (mode.dmPelsWidth >= minimum_resolution_width &&
        mode.dmPelsHeight >= minimum_resolution_height) {
      unique_resolutions.emplace(static_cast<int>(mode.dmPelsWidth),
                                 static_cast<int>(mode.dmPelsHeight));
    }
  }
  unique_resolutions.emplace(640, 480);
  unique_resolutions.emplace(state.settings.width, state.settings.height);
  state.resolutions.assign(unique_resolutions.begin(),
                           unique_resolutions.end());
  std::sort(
      state.resolutions.begin(), state.resolutions.end(),
      [](const auto &left, const auto &right) {
        const auto left_area = static_cast<long long>(left.first) * left.second;
        const auto right_area =
            static_cast<long long>(right.first) * right.second;
        return left_area == right_area ? left < right : left_area < right_area;
      });

  int selected = 0;
  for (std::size_t index = 0; index < state.resolutions.size(); ++index) {
    const auto [width, height] = state.resolutions[index];
    const auto label =
        std::to_wstring(width) + L" x " + std::to_wstring(height);
    SendMessageW(state.resolution_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
    if (width == state.settings.width && height == state.settings.height) {
      selected = static_cast<int>(index);
    }
  }
  SendMessageW(state.resolution_combo, CB_SETCURSEL,
               static_cast<WPARAM>(selected), 0);
}

bool selectedResolutionIsDesktop(const LauncherState &state) {
  const auto selected = selectedResolution(state);
  if (!selected) {
    return false;
  }
  const auto [width, height] = *selected;
  return width == state.desktop_width && height == state.desktop_height;
}

void populateMsaa(LauncherState &state) {
  constexpr std::array<const wchar_t *, 4> labels{
      L"Off",
      L"2x",
      L"4x",
      L"8x",
  };
  constexpr std::array<int, 4> samples{0, 2, 4, 8};
  int selected = 0;
  for (std::size_t index = 0; index < labels.size(); ++index) {
    SendMessageW(state.msaa_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(labels[index]));
    if (samples[index] == state.settings.msaa_samples) {
      selected = static_cast<int>(index);
    }
  }
  SendMessageW(state.msaa_combo, CB_SETCURSEL, static_cast<WPARAM>(selected),
               0);
}

void populateFrameLimits(LauncherState &state) {
  constexpr std::array<std::uint32_t, 9> standard_limits{
      0U, 30U, 60U, 72U, 90U, 120U, 144U, 165U, 240U,
  };
  state.frame_limits.assign(standard_limits.begin(), standard_limits.end());
  if (std::ranges::find(state.frame_limits, state.settings.frame_limit) ==
      state.frame_limits.end()) {
    state.frame_limits.push_back(state.settings.frame_limit);
    std::ranges::sort(state.frame_limits);
  }
  int selected{};
  for (std::size_t index = 0; index < state.frame_limits.size(); ++index) {
    const auto label =
        state.frame_limits[index] == 0U
            ? std::wstring{L"Unlimited"}
            : std::to_wstring(state.frame_limits[index]) + L" FPS";
    SendMessageW(state.frame_limit_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
    if (state.frame_limits[index] == state.settings.frame_limit) {
      selected = static_cast<int>(index);
    }
  }
  SendMessageW(state.frame_limit_combo, CB_SETCURSEL,
               static_cast<WPARAM>(selected), 0);
}

void populateAspectRatios(LauncherState &state) {
  constexpr std::array<const wchar_t *, 2> labels{
      L"Adaptive (display aspect)",
      L"Original PS1 (4:3)",
  };
  for (const auto *label : labels) {
    SendMessageW(state.aspect_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label));
  }
  const auto selected =
      state.settings.aspect_ratio == AspectRatioMode::adaptive ? 0 : 1;
  SendMessageW(state.aspect_combo, CB_SETCURSEL, selected, 0);
  SendMessageW(state.aspect_combo, CB_SETDROPPEDWIDTH, 240, 0);
}

void populateLanguages(LauncherState &state) {
  constexpr std::array<const wchar_t *, 2> labels{
      L"English",
      // Keep the source ASCII-only: MSVC installations using the system ANSI
      // code page otherwise reinterpret the UTF-8 literal as mojibake.
      L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439 (ViT Co.)",
  };
  for (const auto *label : labels) {
    SendMessageW(state.language_combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label));
  }
  SendMessageW(state.language_combo, CB_SETCURSEL,
               state.language == game::GameLanguage::russian_vit ? 1 : 0, 0);
  SendMessageW(state.language_combo, CB_SETDROPPEDWIDTH, 220, 0);
}

std::wstring windowText(HWND control) {
  const auto length = GetWindowTextLengthW(control);
  if (length <= 0) {
    return {};
  }
  std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
  const auto copied = GetWindowTextW(control, text.data(), length + 1);
  text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
  return text;
}

void browseGameImage(HWND window, LauncherState &state) {
  std::array<wchar_t, 32768U> selected{};
  const auto current = windowText(state.game_image_edit);
  if (!current.empty() && current.size() < selected.size()) {
    std::copy(current.begin(), current.end(), selected.begin());
  }
  constexpr wchar_t filter[] =
      L"Syphon Filter disc image (*.cue)\0*.cue\0All files (*.*)\0*.*\0\0";
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter = filter;
  dialog.lpstrFile = selected.data();
  dialog.nMaxFile = static_cast<DWORD>(selected.size());
  dialog.lpstrDefExt = L"cue";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                 OFN_NOCHANGEDIR;
  if (GetOpenFileNameW(&dialog) != FALSE) {
    SetWindowTextW(state.game_image_edit, selected.data());
    SetFocus(state.game_image_edit);
    SendMessageW(state.game_image_edit, EM_SETSEL, 0, -1);
  }
}

bool isCueImage(const std::filesystem::path &path) {
  auto extension = path.extension().wstring();
  std::ranges::transform(extension, extension.begin(), [](wchar_t character) {
    return static_cast<wchar_t>(std::towlower(character));
  });
  return extension == L".cue";
}

void acceptSettings(HWND window, LauncherState &state) {
  const auto image_text = windowText(state.game_image_edit);
  const auto image_path = std::filesystem::path{image_text};
  std::error_code image_error;
  if (image_path.empty() || !isCueImage(image_path) ||
      !std::filesystem::is_regular_file(image_path, image_error) ||
      image_error) {
    showStyledNotice(
        window, L"DISC IMAGE REQUIRED",
        L"Select the CUE file from your original Syphon Filter USA v1.1 "
        L"disc image. Keep every referenced BIN file beside it.");
    SetFocus(state.game_image_edit);
    SendMessageW(state.game_image_edit, EM_SETSEL, 0, -1);
    return;
  }
  state.cue_path = std::filesystem::absolute(image_path, image_error);
  if (image_error) {
    state.cue_path = image_path;
  }

  if (state.cheats_visible) {
    const auto selected = SendMessageW(state.mission_combo, CB_GETCURSEL, 0, 0);
    const auto missions = game::missionCatalog();
    if (selected < 0 || static_cast<std::size_t>(selected) >= missions.size()) {
      showStyledNotice(window, L"INVALID INSERTION POINT",
                       L"Select one of the 20 retail missions.");
      SetFocus(state.mission_combo);
      return;
    }
    state.mission_index = missions[static_cast<std::size_t>(selected)].index;
  }

  const auto resolution = selectedResolution(state);
  if (!resolution) {
    showStyledNotice(
        window, L"INVALID RENDERING RESOLUTION",
        L"Enter a resolution as WIDTH x HEIGHT (minimum 320 x 240).");
    SetFocus(state.resolution_combo);
    SendMessageW(state.resolution_combo, CB_SETEDITSEL, 0, MAKELPARAM(0, -1));
    return;
  }
  state.settings.width = resolution->first;
  state.settings.height = resolution->second;

  constexpr std::array<int, 4> samples{0, 2, 4, 8};
  const auto msaa_index =
      static_cast<int>(SendMessageW(state.msaa_combo, CB_GETCURSEL, 0, 0));
  if (msaa_index >= 0 &&
      static_cast<std::size_t>(msaa_index) < samples.size()) {
    state.settings.msaa_samples = samples[static_cast<std::size_t>(msaa_index)];
  }
  const auto frame_limit_index = static_cast<int>(
      SendMessageW(state.frame_limit_combo, CB_GETCURSEL, 0, 0));
  if (frame_limit_index >= 0 &&
      static_cast<std::size_t>(frame_limit_index) < state.frame_limits.size()) {
    state.settings.frame_limit =
        state.frame_limits[static_cast<std::size_t>(frame_limit_index)];
  }
  state.settings.aspect_ratio =
      SendMessageW(state.aspect_combo, CB_GETCURSEL, 0, 0) == 0
          ? AspectRatioMode::adaptive
          : AspectRatioMode::original_4_3;
  state.settings.bilinear_filtering =
      IsDlgButtonChecked(window, bilinear_control_id) == BST_CHECKED;
  state.settings.anisotropic_filtering =
      IsDlgButtonChecked(window, anisotropic_control_id) == BST_CHECKED;
  state.settings.vsync =
      IsDlgButtonChecked(window, vsync_control_id) == BST_CHECKED;
  state.settings.fullscreen =
      IsDlgButtonChecked(window, fullscreen_control_id) == BST_CHECKED;
  state.language = SendMessageW(state.language_combo, CB_GETCURSEL, 0, 0) == 1
                       ? game::GameLanguage::russian_vit
                       : game::GameLanguage::english;
  if (!game::localizationPackAvailable(state.language)) {
    showStyledNotice(window, L"LANGUAGE PACK MISSING",
                     L"The Russian text pack is missing or incomplete. "
                     L"Reinstall the full release package.");
    SetFocus(state.language_combo);
    return;
  }
  state.tests.retail_all_weapons =
      state.cheats_visible &&
      IsDlgButtonChecked(window, all_weapons_control_id) == BST_CHECKED;
  state.accepted = true;
  DestroyWindow(window);
}

void drawLauncherFrame(HWND window, LauncherState &state) {
  PAINTSTRUCT paint{};
  const auto dc = BeginPaint(window, &paint);
  RECT client{};
  GetClientRect(window, &client);
  FillRect(dc, &client, state.background_brush);

  const auto grid_pen = CreatePen(PS_SOLID, 1, launcher_grid_color);
  const auto old_pen = SelectObject(dc, grid_pen);
  for (auto x = 0; x < client.right; x += 28) {
    MoveToEx(dc, x, 94, nullptr);
    LineTo(dc, x, client.bottom);
  }
  for (auto y = 94; y < client.bottom; y += 28) {
    MoveToEx(dc, 0, y, nullptr);
    LineTo(dc, client.right, y);
  }

  RECT image_panel{24, 102, 736, 160};
  RECT left_panel{24, 176, 370, 474};
  RECT right_panel{390, 176, 736, 474};
  RECT archive_panel{176, 514, 456, 556};
  FillRect(dc, &image_panel, state.panel_brush);
  FillRect(dc, &left_panel, state.panel_brush);
  FillRect(dc, &right_panel, state.panel_brush);
  FillRect(dc, &archive_panel, state.panel_brush);
  const auto border_pen = CreatePen(PS_SOLID, 2, launcher_border_color);
  SelectObject(dc, border_pen);
  SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, left_panel.left, left_panel.top, left_panel.right,
            left_panel.bottom);
  Rectangle(dc, right_panel.left, right_panel.top, right_panel.right,
            right_panel.bottom);
  Rectangle(dc, image_panel.left, image_panel.top, image_panel.right,
            image_panel.bottom);
  Rectangle(dc, archive_panel.left, archive_panel.top, archive_panel.right,
            archive_panel.bottom);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, launcher_text_color);
  SelectObject(dc, state.title_font);
  RECT title{28, 18, client.right - 28, 60};
  DrawTextW(dc, L"SYPHON FILTER // PC", -1, &title,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER);
  SetTextColor(dc, launcher_muted_text_color);
  SelectObject(dc, state.ui_font);
  RECT subtitle{31, 60, client.right - 28, 86};
  DrawTextW(dc, L"AGENCY FIELD TERMINAL  /  LAUNCH CONFIGURATION", -1,
            &subtitle, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

  SelectObject(dc, state.heading_font);
  RECT archive_text = archive_panel;
  DrawTextW(dc, L"CLASSIFIED  //  AGENCY ARCHIVE", -1, &archive_text,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

  SelectObject(dc, old_pen);
  DeleteObject(border_pen);
  DeleteObject(grid_pen);
  EndPaint(window, &paint);
}

bool isLauncherOwnerDrawButton(UINT id) noexcept {
  return id == controls_control_id || id == launch_control_id ||
         id == cancel_control_id || id == browse_image_control_id ||
         id == dossier_control_id;
}

void drawLauncherButton(const DRAWITEMSTRUCT &item,
                        const LauncherState &state) {
  const auto pressed = (item.itemState & ODS_SELECTED) != 0U;
  const auto disabled = (item.itemState & ODS_DISABLED) != 0U;
  const auto accent = item.CtlID == launch_control_id ? launcher_launch_color
                      : item.CtlID == dossier_control_id
                          ? dossier_accent_color
                          : launcher_border_color;
  const auto fill = pressed ? RGB(24, 39, 77) : launcher_panel_color;
  const auto fill_brush = CreateSolidBrush(fill);
  FillRect(item.hDC, &item.rcItem, fill_brush);
  DeleteObject(fill_brush);

  const auto border_pen =
      CreatePen(PS_SOLID, 2, disabled ? launcher_grid_color : accent);
  const auto old_pen = SelectObject(item.hDC, border_pen);
  SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
  Rectangle(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right,
            item.rcItem.bottom);
  SelectObject(item.hDC, old_pen);
  DeleteObject(border_pen);

  std::array<wchar_t, 128U> label{};
  GetWindowTextW(item.hwndItem, label.data(), static_cast<int>(label.size()));
  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC,
               disabled ? launcher_muted_text_color : launcher_text_color);
  SelectObject(item.hDC, state.heading_font);
  auto text_bounds = item.rcItem;
  if (pressed) {
    OffsetRect(&text_bounds, 1, 1);
  }
  DrawTextW(item.hDC, label.data(), -1, &text_bounds,
            DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  if ((item.itemState & ODS_FOCUS) != 0U) {
    InflateRect(&text_bounds, -4, -4);
    DrawFocusRect(item.hDC, &text_bounds);
  }
}

LRESULT CALLBACK launcherWindowProc(HWND window, UINT message, WPARAM w_param,
                                    LPARAM l_param) {
  auto *state = reinterpret_cast<LauncherState *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto *create = reinterpret_cast<const CREATESTRUCTW *>(l_param);
    state = static_cast<LauncherState *>(create->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  }
  if (state == nullptr) {
    return DefWindowProcW(window, message, w_param, l_param);
  }

  switch (message) {
  case WM_CREATE:
    state->large_icon = createLauncherIcon(32);
    state->small_icon = createLauncherIcon(16);
    SendMessageW(window, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(state->large_icon));
    SendMessageW(window, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(state->small_icon));
    createControl(window, L"STATIC", L"GAME IMAGE", 0, 40, 118, 96, 24, 0,
                  state->heading_font);
    state->game_image_edit =
        createControl(window, L"EDIT", state->cue_path.c_str(),
                      WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 140, 116, 478,
                      26, game_image_control_id, state->ui_font);
    createControl(window, L"BUTTON", L"BROWSE", WS_TABSTOP | BS_OWNERDRAW, 628,
                  112, 90, 34, browse_image_control_id, state->heading_font);

    createControl(window, L"STATIC", L"DISPLAY SYSTEM", 0, 44, 188, 270, 26, 0,
                  state->heading_font);
    createControl(window, L"STATIC", L"Resolution", 0, 48, 228, 92, 20, 0,
                  state->ui_font);
    state->resolution_combo = createControl(
        window, L"COMBOBOX", L"",
        WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL, 144, 222, 214,
        240, resolution_control_id, state->ui_font);
    createControl(window, L"STATIC", L"Aspect ratio", 0, 48, 264, 92, 20, 0,
                  state->ui_font);
    state->aspect_combo =
        createControl(window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                      144, 258, 214, 120, aspect_control_id, state->ui_font);
    createControl(window, L"STATIC", L"Antialiasing", 0, 48, 300, 92, 20, 0,
                  state->ui_font);
    state->msaa_combo =
        createControl(window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                      144, 294, 214, 160, msaa_control_id, state->ui_font);
    createControl(window, L"STATIC", L"Frame limit", 0, 48, 336, 92, 20, 0,
                  state->ui_font);
    state->frame_limit_combo = createControl(
        window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, 144, 330, 214,
        220, frame_limit_control_id, state->ui_font);
    createControl(window, L"BUTTON", L"Bilinear filtering",
                  WS_TABSTOP | BS_AUTOCHECKBOX, 48, 366, 294, 24,
                  bilinear_control_id, state->ui_font);
    createControl(window, L"BUTTON", L"Anisotropic filtering",
                  WS_TABSTOP | BS_AUTOCHECKBOX, 48, 392, 294, 24,
                  anisotropic_control_id, state->ui_font);
    createControl(window, L"BUTTON", L"Vertical synchronization",
                  WS_TABSTOP | BS_AUTOCHECKBOX, 48, 418, 294, 24,
                  vsync_control_id, state->ui_font);
    createControl(window, L"BUTTON", L"Borderless fullscreen",
                  WS_TABSTOP | BS_AUTOCHECKBOX, 48, 444, 294, 24,
                  fullscreen_control_id, state->ui_font);

    createControl(window, L"STATIC",
                  state->cheats_visible ? L"RESTRICTED ACCESS"
                                        : L"MISSION CONTROL",
                  0, 410, 188, 286, 26, 0, state->heading_font);
    if (state->cheats_visible) {
      createControl(window, L"STATIC", L"Insertion point", 0, 414, 228, 96, 20,
                    0, state->ui_font);
      state->mission_combo = createControl(
          window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
          516, 222, 204, 420, mission_control_id, state->ui_font);
      createControl(window, L"BUTTON", L"Full weapons authorization",
                    WS_TABSTOP | BS_AUTOCHECKBOX, 414, 278, 294, 24,
                    all_weapons_control_id, state->ui_font);
      createControl(window, L"STATIC",
                    L"Developer override detected. Campaign routing and "
                    L"inventory bypass are available for this session.",
                    SS_LEFT, 414, 328, 294, 58, 0, state->ui_font);
    } else {
      createControl(window, L"STATIC",
                    L"Retail campaign routing active. Mission progress "
                    L"and equipment are managed by the game.",
                    SS_LEFT, 414, 228, 294, 62, 0, state->ui_font);
      createControl(window, L"STATIC",
                    L"GRAPHICS / INPUT / AUDIO\nSYSTEM STATUS: READY", SS_LEFT,
                    414, 342, 294, 42, 0, state->ui_font);
    }
    createControl(window, L"STATIC", L"Text language", 0, 414, 306, 98, 20, 0,
                  state->ui_font);
    state->language_combo =
        createControl(window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                      516, 300, 204, 96, language_control_id, state->ui_font);
    createControl(window, L"BUTTON", L"INPUT CONFIGURATION",
                  WS_TABSTOP | BS_OWNERDRAW, 414, 408, 294, 38,
                  controls_control_id, state->heading_font);
    createControl(window, L"BUTTON", L"DOSSIERS", WS_TABSTOP | BS_OWNERDRAW, 26,
                  514, 140, 42, dossier_control_id, state->heading_font);
    createControl(window, L"BUTTON", L"DEPLOY", WS_TABSTOP | BS_OWNERDRAW, 476,
                  514, 126, 42, launch_control_id, state->heading_font);
    createControl(window, L"BUTTON", L"ABORT", WS_TABSTOP | BS_OWNERDRAW, 618,
                  514, 118, 42, cancel_control_id, state->heading_font);
    CheckDlgButton(window, bilinear_control_id,
                   state->settings.bilinear_filtering ? BST_CHECKED
                                                      : BST_UNCHECKED);
    CheckDlgButton(window, anisotropic_control_id,
                   state->settings.anisotropic_filtering ? BST_CHECKED
                                                         : BST_UNCHECKED);
    CheckDlgButton(window, vsync_control_id,
                   state->settings.vsync ? BST_CHECKED : BST_UNCHECKED);
    if (state->cheats_visible) {
      CheckDlgButton(window, all_weapons_control_id,
                     state->tests.retail_all_weapons ? BST_CHECKED
                                                     : BST_UNCHECKED);
      populateMissions(*state);
    }
    populateResolutions(*state);
    populateAspectRatios(*state);
    populateMsaa(*state);
    populateFrameLimits(*state);
    populateLanguages(*state);
    CheckDlgButton(window, fullscreen_control_id,
                   state->settings.fullscreen ||
                           selectedResolutionIsDesktop(*state)
                       ? BST_CHECKED
                       : BST_UNCHECKED);
    return 0;
  case WM_PAINT:
    drawLauncherFrame(window, *state);
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN: {
    const auto dc = reinterpret_cast<HDC>(w_param);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, launcher_text_color);
    return reinterpret_cast<LRESULT>(state->panel_brush);
  }
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX: {
    const auto dc = reinterpret_cast<HDC>(w_param);
    SetBkColor(dc, launcher_panel_color);
    SetTextColor(dc, launcher_text_color);
    return reinterpret_cast<LRESULT>(state->panel_brush);
  }
  case WM_DRAWITEM: {
    const auto *item = reinterpret_cast<const DRAWITEMSTRUCT *>(l_param);
    if (item != nullptr && item->CtlType == ODT_BUTTON &&
        isLauncherOwnerDrawButton(item->CtlID)) {
      drawLauncherButton(*item, *state);
      return TRUE;
    }
    break;
  }
  case WM_COMMAND:
    if (LOWORD(w_param) == resolution_control_id &&
        (HIWORD(w_param) == CBN_SELCHANGE ||
         HIWORD(w_param) == CBN_EDITCHANGE)) {
      CheckDlgButton(window, fullscreen_control_id,
                     selectedResolutionIsDesktop(*state) ? BST_CHECKED
                                                         : BST_UNCHECKED);
      return 0;
    }
    if (HIWORD(w_param) != BN_CLICKED) {
      return 0;
    }
    if (LOWORD(w_param) == launch_control_id) {
      acceptSettings(window, *state);
      return 0;
    }
    if (LOWORD(w_param) == controls_control_id) {
      showControlsWindow(window, state->input);
      return 0;
    }
    if (LOWORD(w_param) == dossier_control_id) {
      showDossierWindow(window);
      return 0;
    }
    if (LOWORD(w_param) == browse_image_control_id) {
      browseGameImage(window, *state);
      return 0;
    }
    if (LOWORD(w_param) == cancel_control_id) {
      DestroyWindow(window);
      return 0;
    }
    break;
  case WM_CLOSE:
    DestroyWindow(window);
    return 0;
  case WM_DESTROY:
    if (state->small_icon != nullptr) {
      DestroyIcon(state->small_icon);
      state->small_icon = nullptr;
    }
    if (state->large_icon != nullptr) {
      DestroyIcon(state->large_icon);
      state->large_icon = nullptr;
    }
    state->finished = true;
    PostQuitMessage(0);
    return 0;
  default:
    break;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace

void loadLauncherSettings(GraphicsSettings &graphics,
                          KeyboardMouseBindings &input,
                          game::GameLanguage &language) noexcept {
  try {
    loadSettingsFile(graphics, input, language);
  } catch (...) {
    // A malformed or inaccessible optional launcher file must not block
    // startup; the caller's defaults remain authoritative.
  }
}

bool showGraphicsLauncher(GraphicsSettings &settings,
                          KeyboardMouseBindings &input,
                          GameplayTestSettings &tests,
                          game::GameLanguage &language,
                          std::filesystem::path &cue_path,
                          std::uint32_t &mission_index,
                          bool mission_selection_enabled) {
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = launcherWindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.hbrBackground = nullptr;
  window_class.lpszClassName = launcher_class_name;
  if (RegisterClassW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  constexpr DWORD style = WS_CAPTION | WS_SYSMENU;
  constexpr DWORD extended_style = WS_EX_CONTROLPARENT | WS_EX_APPWINDOW;
  RECT bounds{0, 0, launcher_client_width, launcher_client_height};
  AdjustWindowRectEx(&bounds, style, FALSE, extended_style);

  LauncherState state{};
  state.settings = settings;
  state.input = input;
  state.tests = tests;
  state.language = language;
  state.cue_path = cue_path.empty() ? loadGameImagePath() : cue_path;
  state.mission_index = mission_index;
  state.mission_selection_enabled = mission_selection_enabled;
  state.cheats_visible = mission_selection_enabled && cheatMarkerExists();
  state.title_font =
      CreateFontW(-32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift SemiCondensed");
  state.heading_font =
      CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift SemiCondensed");
  state.ui_font =
      CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  FF_DONTCARE, L"Bahnschrift");
  state.background_brush = CreateSolidBrush(launcher_background_color);
  state.panel_brush = CreateSolidBrush(launcher_panel_color);
  const auto window = CreateWindowExW(
      extended_style, launcher_class_name, L"Syphon Filter PC - Launcher",
      style, CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
      bounds.bottom - bounds.top, nullptr, nullptr, instance, &state);
  if (window == nullptr) {
    DeleteObject(state.panel_brush);
    DeleteObject(state.background_brush);
    DeleteObject(state.ui_font);
    DeleteObject(state.heading_font);
    DeleteObject(state.title_font);
    return false;
  }

  RECT work_area{};
  if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0) != FALSE) {
    RECT window_bounds{};
    GetWindowRect(window, &window_bounds);
    const auto width = window_bounds.right - window_bounds.left;
    const auto height = window_bounds.bottom - window_bounds.top;
    SetWindowPos(
        window, nullptr,
        work_area.left + (work_area.right - work_area.left - width) / 2,
        work_area.top + (work_area.bottom - work_area.top - height) / 2, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  ShowWindow(window, SW_SHOW);
  UpdateWindow(window);

  MSG message{};
  while (!state.finished && GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(window, &message) == FALSE) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  DeleteObject(state.panel_brush);
  DeleteObject(state.background_brush);
  DeleteObject(state.ui_font);
  DeleteObject(state.heading_font);
  DeleteObject(state.title_font);
  if (state.accepted) {
    settings = state.settings;
    input = state.input;
    tests = state.tests;
    language = state.language;
    cue_path = state.cue_path;
    mission_index = state.mission_index;
    try {
      saveSettingsFile(settings, input, language, cue_path);
    } catch (...) {
      // The selected settings still apply for this run even when the
      // per-user launcher file cannot be persisted.
    }
  }
  return state.accepted;
}

bool launcherCheatsEnabled() noexcept { return cheatMarkerExists(); }

void showLauncherError(std::string_view title,
                       std::string_view message) noexcept {
  try {
    showStyledNotice(nullptr, widenUtf8(title), widenUtf8(message));
  } catch (...) {
    const auto wide_title = widenUtf8(title);
    const auto wide_message = widenUtf8(message);
    MessageBoxW(nullptr, wide_message.c_str(), wide_title.c_str(),
                MB_OK | MB_ICONERROR);
  }
}

} // namespace sf::platform

#else

namespace sf::platform {

void loadLauncherSettings(GraphicsSettings &, KeyboardMouseBindings &,
                          game::GameLanguage &) noexcept {}

bool showGraphicsLauncher(GraphicsSettings &, KeyboardMouseBindings &,
                          GameplayTestSettings &, game::GameLanguage &,
                          std::filesystem::path &, std::uint32_t &, bool) {
  return true;
}

bool launcherCheatsEnabled() noexcept { return false; }

void showLauncherError(std::string_view, std::string_view) noexcept {}

} // namespace sf::platform

#endif
