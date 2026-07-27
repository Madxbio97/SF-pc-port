#pragma once

#include "sf/game/gameplay.hpp"
#include "sf/platform/player_input.hpp"

#include <cstdint>
#include <memory>
#include <optional>

struct PADRAW;

namespace sf::game {
class MissionPackage;
}

namespace sf::platform::detail {

class PsyCrossAudioOutput;

class PsyCrossMissionStart final {
public:
    PsyCrossMissionStart();
    ~PsyCrossMissionStart();

    [[nodiscard]] std::uint16_t run(
        const game::MissionPackage& mission,
        PADRAW& pad,
        std::uint16_t previous_buttons,
        const KeyboardMouseBindings& bindings,
        std::optional<game::CampaignCarryState> carry = std::nullopt);

    [[nodiscard]] std::unique_ptr<game::GameplaySession>
    takePreloadedGameplay() noexcept;
    [[nodiscard]] std::unique_ptr<PsyCrossAudioOutput>
    takePreloadedAudio() noexcept;

private:
    std::unique_ptr<game::GameplaySession> preloaded_gameplay_;
    std::unique_ptr<PsyCrossAudioOutput> preloaded_audio_;
};

} // namespace sf::platform::detail
