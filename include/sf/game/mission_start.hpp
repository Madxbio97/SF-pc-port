#pragma once

#include <cstdint>

namespace sf::game {

enum class MissionStartPhase {
    waiting_for_release,
    waiting_for_confirm,
    accepted,
};

class MissionStartGate final {
public:
    static constexpr std::uint32_t release_frames = 2U;
    static constexpr std::uint32_t prompt_blink_frames = 32U;

    [[nodiscard]] bool update(
        bool confirm_down,
        bool text_animation_complete = true) noexcept;
    [[nodiscard]] MissionStartPhase phase() const noexcept { return phase_; }
    [[nodiscard]] static bool brightPrompt(std::uint32_t frame) noexcept {
        return ((frame / prompt_blink_frames) & 1U) != 0U;
    }
private:
    MissionStartPhase phase_{MissionStartPhase::waiting_for_release};
    std::uint32_t released_frames_{};
};

} // namespace sf::game
