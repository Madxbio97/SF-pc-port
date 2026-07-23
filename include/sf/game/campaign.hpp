#pragma once

#include "sf/game/mission.hpp"
#include "sf/game/title.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace sf::game {

enum class CampaignAdvance {
  next_mission,
  campaign_complete,
  invalid,
};

enum class CampaignSavePhase {
  prompt,
  slots,
  complete,
};

enum class CampaignSaveDecision {
  none,
  continue_without_saving,
  save,
};

struct CampaignSaveInput {
  bool previous{};
  bool next{};
  bool confirm{};
  bool cancel{};
};

struct CampaignSaveResult {
  CampaignSaveDecision decision{CampaignSaveDecision::none};
  std::optional<std::size_t> slot;
};

// Retail first asks whether the completed mission should be saved. Choosing
// Yes enters a separate memory-card slot picker; cancelling that picker
// returns to the question without changing durable data.
class CampaignSaveMenu final {
public:
  [[nodiscard]] CampaignSaveResult
  update(const CampaignSaveInput &input) noexcept;
  [[nodiscard]] CampaignSavePhase phase() const noexcept { return phase_; }
  [[nodiscard]] bool saveSelected() const noexcept { return save_selected_; }
  [[nodiscard]] std::size_t slotSelection() const noexcept {
    return slot_selection_;
  }

private:
  CampaignSavePhase phase_{CampaignSavePhase::prompt};
  bool save_selected_{true};
  std::size_t slot_selection_{};
};

// Owns the small native part of campaign progression. Retail gameplay decides
// when a mission succeeds; this cursor only connects that terminal event to
// the next package, its SOL movie and the durable title save slot.
class CampaignProgress final {
public:
  [[nodiscard]] static std::optional<CampaignProgress>
  startUnsaved(std::uint32_t mission_index,
               bool opening_movie_already_played) noexcept;
  [[nodiscard]] static std::optional<CampaignProgress>
  startNew(TitleSaveSlots &slots, std::uint32_t mission_index,
           bool opening_movie_already_played) noexcept;
  [[nodiscard]] static std::optional<CampaignProgress>
  startNewInSlot(TitleSaveSlots &slots, std::size_t save_slot,
                 std::uint32_t mission_index,
                 bool opening_movie_already_played) noexcept;
  [[nodiscard]] static std::optional<CampaignProgress>
  resume(const TitleSaveSlots &slots, std::size_t save_slot) noexcept;

  [[nodiscard]] std::optional<std::size_t> saveSlot() const noexcept {
    return save_slot_;
  }
  [[nodiscard]] std::uint32_t missionIndex() const noexcept {
    return mission_index_;
  }
  [[nodiscard]] std::uint32_t maximumUnlockedMission() const noexcept {
    return maximum_unlocked_mission_;
  }
  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] bool
  openingMovieRequired(const MissionDefinition &mission) const noexcept;
  void markOpeningMovieHandled() noexcept { opening_movie_handled_ = true; }
  [[nodiscard]] std::optional<std::uint32_t>
  pendingEndingMovieMission() const noexcept {
    return pending_eol_mission_;
  }

  // Stage the durable half of a successful retail mission. Production must
  // persist the resulting slots before handing control to the EOL player.
  [[nodiscard]] bool stageMissionCompletion(TitleSaveSlots &slots) noexcept;
  // Save a completed mission into the explicitly selected memory-card slot.
  // This is the only operation that may rebind a campaign to another slot.
  [[nodiscard]] bool stageMissionCompletionInSlot(
      TitleSaveSlots &slots, std::size_t save_slot,
      std::optional<CampaignCarryState> carry = std::nullopt) noexcept;
  // Finalize a staged EOL transaction. For compatibility with deterministic
  // probes this also accepts an unstaged in-memory completion, but the title
  // host always stages and persists first.
  [[nodiscard]] CampaignAdvance completeMission(TitleSaveSlots &slots) noexcept;
  // Declining the retail save prompt advances only the live campaign. Any
  // previously loaded save remains untouched.
  [[nodiscard]] CampaignAdvance completeMissionWithoutSaving() noexcept;
  // Replays an already reached mission without lowering the durable
  // campaign frontier kept by the loaded save slot.
  [[nodiscard]] bool
  selectUnlockedMission(std::uint32_t mission_index) noexcept;

private:
  CampaignProgress(
      std::optional<std::size_t> save_slot, std::uint32_t mission_index,
      std::uint32_t maximum_unlocked_mission, bool opening_movie_handled,
      std::optional<std::uint32_t> pending_eol_mission = std::nullopt) noexcept;

  [[nodiscard]] CampaignAdvance advance() noexcept;

  std::optional<std::size_t> save_slot_;
  std::uint32_t mission_index_{};
  std::uint32_t maximum_unlocked_mission_{};
  bool opening_movie_handled_{};
  std::optional<std::uint32_t> pending_eol_mission_;
  bool active_{true};
};

} // namespace sf::game
