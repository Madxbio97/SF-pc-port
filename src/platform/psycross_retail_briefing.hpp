#pragma once

#include "sf/platform/player_input.hpp"

#include <cstdint>
#include <memory>

namespace sf::assets {
class MissionBriefing;
}

namespace sf::game {
class MissionPackage;
}

namespace sf::platform::detail {

class PsyCrossRetailBriefing final {
public:
  PsyCrossRetailBriefing(const game::MissionPackage &mission,
                         KeyboardMouseBindings bindings);
  ~PsyCrossRetailBriefing();

  PsyCrossRetailBriefing(const PsyCrossRetailBriefing &) = delete;
  PsyCrossRetailBriefing &operator=(const PsyCrossRetailBriefing &) = delete;

  [[nodiscard]] bool draw(const assets::MissionBriefing &briefing,
                          double retail_time) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sf::platform::detail
