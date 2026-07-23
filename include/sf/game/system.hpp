#pragma once

namespace sf::game {

enum class VideoMode {
    ntsc = 0,
    pal = 1,
};

class SystemServices {
public:
    virtual ~SystemServices() = default;
    virtual void resetCallbacks() = 0;
    virtual void setVideoMode(VideoMode mode) = 0;
    virtual void runStateMachine() = 0;

protected:
    SystemServices() = default;
};

// Native equivalent of the game main at 0x8001457c. The host C++ runtime
// already performs the constructor work handled by PS-X __main.
[[nodiscard]] int runSystem(SystemServices& services);

} // namespace sf::game
