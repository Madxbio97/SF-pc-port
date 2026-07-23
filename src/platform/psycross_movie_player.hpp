#pragma once

#include <cstdint>
#include <functional>

struct PADRAW;

namespace sf::game {
struct DiscMovie;
class TitleMovies;
}

namespace sf::platform::detail {

struct MovieOverlayCallbacks {
    std::function<bool(std::uint16_t, std::uint32_t)> update;
    std::function<void()> draw;
    std::function<const game::DiscMovie*()> transition_movie;
};

class PsyCrossMoviePlayer final {
public:
    [[nodiscard]] std::uint16_t playStandalone(
        const game::DiscMovie& movie,
        PADRAW& pad,
        std::uint16_t previous_buttons);

    [[nodiscard]] std::uint16_t play(
        game::TitleMovies& movies,
        PADRAW& pad,
        std::uint16_t previous_buttons,
        const MovieOverlayCallbacks& overlay,
        bool play_startup_movies = true);
};

} // namespace sf::platform::detail
