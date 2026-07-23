#include "sf/game/system.hpp"

namespace sf::game {

int runSystem(SystemServices& services) {
    services.resetCallbacks();
    services.setVideoMode(VideoMode::ntsc);
    services.runStateMachine();
    return 0;
}

} // namespace sf::game
