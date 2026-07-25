#include "psycross_runtime_guards.hpp"

#include <SDL.h>

namespace sf::platform::detail {

RelativeMouseCapture::~RelativeMouseCapture() { set(false); }

void RelativeMouseCapture::set(bool enabled) noexcept {
  if (enabled_ == enabled) {
    return;
  }
  if (SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE) == 0) {
    enabled_ = enabled;
  }
  SDL_GetRelativeMouseState(nullptr, nullptr);
}

} // namespace sf::platform::detail
