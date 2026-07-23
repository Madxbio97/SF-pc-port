#include "psycross_window_mode.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>

#include <algorithm>

extern SDL_Window* g_window;
extern int g_altKeyState;
extern void GR_ResetDevice();

namespace sf::platform::detail {
namespace {

constexpr int minimum_window_width = 320;
constexpr int minimum_window_height = 240;

PsyCrossWindowMode* active_window_mode{};
int mouse_wheel_delta{};

bool isShortcutKey(const SDL_KeyboardEvent& event) noexcept {
    if (event.keysym.scancode == SDL_SCANCODE_F11) {
        return true;
    }
    const auto is_enter = event.keysym.scancode == SDL_SCANCODE_RETURN ||
        event.keysym.scancode == SDL_SCANCODE_KP_ENTER;
    return is_enter && (event.keysym.mod & KMOD_ALT) != 0;
}

} // namespace

PsyCrossWindowMode::PsyCrossWindowMode(bool start_fullscreen) {
    if (g_window == nullptr) {
        PsyX_Log_Warning("Window mode controller disabled: SDL window is unavailable\n");
        return;
    }

    SDL_SetWindowMinimumSize(g_window, minimum_window_width, minimum_window_height);
    captureWindowedBounds();

    SDL_GetEventFilter(&previous_filter_, &previous_filter_userdata_);
    SDL_SetEventFilter(&PsyCrossWindowMode::filterEvent, this);
    previous_debug_key_handler_ = g_dbg_gameDebugKeys;
    active_window_mode = this;
    g_dbg_gameDebugKeys = &PsyCrossWindowMode::handleDebugKey;

    if (start_fullscreen) {
        setFullscreen(true);
    }
}

PsyCrossWindowMode::~PsyCrossWindowMode() {
    if (g_window == nullptr) {
        return;
    }

    SDL_EventFilter current_filter{};
    void* current_userdata{};
    SDL_GetEventFilter(&current_filter, &current_userdata);
    if (current_filter == &PsyCrossWindowMode::filterEvent && current_userdata == this) {
        SDL_SetEventFilter(previous_filter_, previous_filter_userdata_);
    }
    if (active_window_mode == this) {
        g_dbg_gameDebugKeys = previous_debug_key_handler_;
        active_window_mode = nullptr;
    }
}

int PsyCrossWindowMode::filterEvent(void* userdata, SDL_Event* event) {
    auto& self = *static_cast<PsyCrossWindowMode*>(userdata);
    if (self.previous_filter_ != nullptr &&
        self.previous_filter_(self.previous_filter_userdata_, event) == 0) {
        return 0;
    }

    if (event->type == SDL_MOUSEWHEEL) {
        mouse_wheel_delta += event->wheel.y;
        return 1;
    }
    if (event->type == SDL_WINDOWEVENT) {
        self.observeWindowEvent(event->window);
        return 1;
    }
    if (event->type != SDL_KEYDOWN && event->type != SDL_KEYUP) {
        return 1;
    }
    if (!isShortcutKey(event->key)) {
        return 1;
    }

    const auto is_enter = event->key.keysym.scancode == SDL_SCANCODE_RETURN ||
        event->key.keysym.scancode == SDL_SCANCODE_KP_ENTER;
    if (is_enter) {
        g_altKeyState = 1;
    }

    // Consume window shortcuts before PsyCross turns Enter into a pad button or
    // applies its right-Alt-only fullscreen handler. Key repeat is intentionally
    // ignored so holding the chord cannot oscillate between modes.
    if (event->type == SDL_KEYDOWN && event->key.repeat == 0) {
        self.setFullscreen(!self.isFullscreen());
    }
    return 0;
}

int consumePsyCrossMouseWheel() noexcept {
    const auto result = mouse_wheel_delta;
    mouse_wheel_delta = 0;
    return result;
}

void PsyCrossWindowMode::handleDebugKey(int key, char down) {
    if (active_window_mode == nullptr) {
        return;
    }
    if (active_window_mode->previous_debug_key_handler_ != nullptr) {
        active_window_mode->previous_debug_key_handler_(key, down);
    }

    // PsyCross tracks only right Alt. Mirroring SDL's aggregate modifier state
    // keeps pad input suspended for either Alt key while the shortcut is held.
    if (key == SDL_SCANCODE_LALT) {
        g_altKeyState = (SDL_GetModState() & KMOD_ALT) != 0 ? 1 : 0;
    }
}

bool PsyCrossWindowMode::isFullscreen() const noexcept {
    return g_window != nullptr &&
        (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void PsyCrossWindowMode::captureWindowedBounds() noexcept {
    if (g_window == nullptr || isFullscreen()) {
        return;
    }
    const auto flags = SDL_GetWindowFlags(g_window);
    windowed_bounds_.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) != 0) {
        return;
    }
    SDL_GetWindowPosition(g_window, &windowed_bounds_.x, &windowed_bounds_.y);
    SDL_GetWindowSize(g_window, &windowed_bounds_.width, &windowed_bounds_.height);
    windowed_bounds_.valid = windowed_bounds_.width > 0 && windowed_bounds_.height > 0;
}

void PsyCrossWindowMode::observeWindowEvent(const SDL_WindowEvent& event) noexcept {
    if (switching_ || g_window == nullptr ||
        event.windowID != SDL_GetWindowID(g_window) || isFullscreen()) {
        return;
    }
    if (event.event == SDL_WINDOWEVENT_MAXIMIZED) {
        windowed_bounds_.maximized = true;
    } else if (event.event == SDL_WINDOWEVENT_RESTORED) {
        windowed_bounds_.maximized = false;
        captureWindowedBounds();
    } else if (event.event == SDL_WINDOWEVENT_MOVED ||
               event.event == SDL_WINDOWEVENT_RESIZED ||
               event.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        captureWindowedBounds();
    }
}

void PsyCrossWindowMode::restoreWindowedBounds() noexcept {
    if (g_window == nullptr) {
        return;
    }
    SDL_RestoreWindow(g_window);
    if (windowed_bounds_.valid) {
        SDL_SetWindowPosition(g_window, windowed_bounds_.x, windowed_bounds_.y);
        SDL_SetWindowSize(
            g_window,
            std::max(windowed_bounds_.width, minimum_window_width),
            std::max(windowed_bounds_.height, minimum_window_height));
    }
    if (windowed_bounds_.maximized) {
        SDL_MaximizeWindow(g_window);
    }
}

void PsyCrossWindowMode::setFullscreen(bool fullscreen) noexcept {
    if (g_window == nullptr || fullscreen == isFullscreen()) {
        return;
    }
    if (fullscreen) {
        captureWindowedBounds();
    }

    const auto flags = fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0U;
    switching_ = true;
    if (SDL_SetWindowFullscreen(g_window, flags) != 0) {
        switching_ = false;
        PsyX_Log_Warning("Cannot switch window mode: %s\n", SDL_GetError());
        return;
    }
    if (!fullscreen) {
        restoreWindowedBounds();
    }
    switching_ = false;
    syncRendererSize();
    PsyX_Log_Info("Window mode: %s (%dx%d)\n",
                  fullscreen ? "borderless fullscreen" : "windowed",
                  g_windowWidth,
                  g_windowHeight);
}

void PsyCrossWindowMode::syncRendererSize() noexcept {
    if (g_window == nullptr) {
        return;
    }
    int width{};
    int height{};
    SDL_GetWindowSize(g_window, &width, &height);
    g_windowWidth = std::max(width, 1);
    g_windowHeight = std::max(height, 1);
    GR_ResetDevice();
}

} // namespace sf::platform::detail
