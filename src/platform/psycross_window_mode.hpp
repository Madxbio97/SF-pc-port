#pragma once

#include <SDL.h>

namespace sf::platform::detail {

[[nodiscard]] int consumePsyCrossMouseWheel() noexcept;

class PsyCrossWindowMode final {
public:
    explicit PsyCrossWindowMode(bool start_fullscreen);
    ~PsyCrossWindowMode();

    PsyCrossWindowMode(const PsyCrossWindowMode&) = delete;
    PsyCrossWindowMode& operator=(const PsyCrossWindowMode&) = delete;

private:
    struct WindowedBounds {
        int x{};
        int y{};
        int width{};
        int height{};
        bool valid{};
        bool maximized{};
    };

    static int filterEvent(void* userdata, SDL_Event* event);
    static void handleDebugKey(int key, char down);

    [[nodiscard]] bool isFullscreen() const noexcept;
    void captureWindowedBounds() noexcept;
    void observeWindowEvent(const SDL_WindowEvent& event) noexcept;
    void restoreWindowedBounds() noexcept;
    void setFullscreen(bool fullscreen) noexcept;
    void syncRendererSize() noexcept;

    WindowedBounds windowed_bounds_;
    SDL_EventFilter previous_filter_{};
    void* previous_filter_userdata_{};
    void (*previous_debug_key_handler_)(int, char){};
    bool switching_{};
};

} // namespace sf::platform::detail
