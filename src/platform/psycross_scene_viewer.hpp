#pragma once

#include "sf/game/campaign.hpp"
#include "sf/game/pause_menu.hpp"
#include "sf/platform/host.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

struct PADRAW;

namespace sf::game {
class GameplaySession;
class MissionPackage;
} // namespace sf::game

namespace sf::platform::detail {

class PsyCrossAudioOutput;

enum class SceneExitReason {
  exit_application,
  return_to_title,
  mission_complete,
  mission_selected,
};

struct SceneViewerResult {
  std::uint16_t previous_buttons{0xffffU};
  SceneExitReason reason{SceneExitReason::exit_application};
  std::optional<std::uint32_t> selected_mission;
  std::optional<game::CampaignCarryState> carry;
};

// Uses the same retail INTRFACE font page and ACD primitive path as the
// in-game pause screen. Keeping mission-complete UI on this renderer avoids
// PsyCross's debug-font VRAM page, which gameplay legitimately overwrites.
class PsyCrossCampaignSaveRenderer final {
public:
  explicit PsyCrossCampaignSaveRenderer(const game::MissionPackage &mission);
  ~PsyCrossCampaignSaveRenderer();

  PsyCrossCampaignSaveRenderer(const PsyCrossCampaignSaveRenderer &) = delete;
  PsyCrossCampaignSaveRenderer &
  operator=(const PsyCrossCampaignSaveRenderer &) = delete;

  void draw(const game::CampaignSaveMenu &menu,
            const game::TitleSaveSlots &slots);
  void drawLoadSlots(const game::TitleSaveSlots &slots, std::size_t selection);

private:
  struct State;
  std::unique_ptr<State> state_;
};

class PsyCrossSceneViewer final {
public:
  explicit PsyCrossSceneViewer(
      KeyboardMouseBindings input = defaultKeyboardMouseBindings(),
      GameplayTestSettings tests = {}) noexcept
      : input_(input), tests_(tests) {}

  [[nodiscard]] SceneViewerResult
  run(const game::MissionPackage &mission, PADRAW &pad,
      std::uint16_t previous_buttons, const std::filesystem::path &cue_path,
      std::uint32_t maximum_unlocked_mission,
      std::unique_ptr<game::GameplaySession> preloaded_gameplay = {},
      std::unique_ptr<PsyCrossAudioOutput> preloaded_audio = {});

private:
  KeyboardMouseBindings input_;
  GameplayTestSettings tests_;
  game::PauseSettings pause_settings_;
  bool pause_settings_initialized_{};
};

} // namespace sf::platform::detail
