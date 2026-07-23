#pragma once

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
  explicit PsyCrossRetailBriefing(const game::MissionPackage &mission);
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
