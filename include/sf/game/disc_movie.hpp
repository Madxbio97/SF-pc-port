#pragma once

#include "sf/disc/iso9660.hpp"

#include <string>

namespace sf::game {

struct DiscMovie {
    std::string path;
    disc::RawSectorFile sectors;
};

} // namespace sf::game
