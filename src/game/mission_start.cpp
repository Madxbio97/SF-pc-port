#include "sf/game/mission_start.hpp"

namespace sf::game {

bool MissionStartGate::update(
    bool confirm_down,
    bool text_animation_complete) noexcept {
    if (phase_ == MissionStartPhase::accepted) {
        return true;
    }
    if (!text_animation_complete) {
        phase_ = MissionStartPhase::waiting_for_release;
        released_frames_ = 0U;
        return false;
    }
    if (phase_ == MissionStartPhase::waiting_for_release) {
        if (confirm_down) {
            released_frames_ = 0U;
            return false;
        }
        ++released_frames_;
        if (released_frames_ >= release_frames) {
            phase_ = MissionStartPhase::waiting_for_confirm;
        }
        return false;
    }
    if (!confirm_down) {
        return false;
    }
    phase_ = MissionStartPhase::accepted;
    return true;
}

} // namespace sf::game
