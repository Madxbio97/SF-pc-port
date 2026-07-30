#include "sf/game/campaign.hpp"

#include <algorithm>
#include <utility>

namespace sf::game {

CampaignProgress::CampaignProgress(
    std::optional<std::size_t> save_slot, std::uint32_t mission_index,
    std::uint32_t maximum_unlocked_mission, bool opening_movie_handled,
    std::optional<std::uint32_t> pending_eol_mission) noexcept
    : save_slot_(save_slot), mission_index_(mission_index),
      maximum_unlocked_mission_(maximum_unlocked_mission),
      opening_movie_handled_(opening_movie_handled),
      pending_eol_mission_(pending_eol_mission) {}

CampaignSaveResult
CampaignSaveMenu::update(const CampaignSaveInput &input,
                         const TitleSaveSlots &slots) noexcept {
  if (phase_ == CampaignSavePhase::complete) {
    return {};
  }
  if (phase_ == CampaignSavePhase::prompt) {
    if (input.previous || input.next) {
      save_selected_ = !save_selected_;
    }
    if (input.cancel || (input.confirm && !save_selected_)) {
      phase_ = CampaignSavePhase::complete;
      return {CampaignSaveDecision::continue_without_saving, std::nullopt};
    }
    if (input.confirm) {
      phase_ = CampaignSavePhase::slots;
    }
    return {};
  }

  if (phase_ == CampaignSavePhase::slots) {
    if (input.cancel) {
      phase_ = CampaignSavePhase::prompt;
      return {};
    }
    if (input.next) {
      slot_selection_ = (slot_selection_ + 1U) % title_save_slot_count;
    }
    if (input.previous) {
      slot_selection_ = slot_selection_ == 0U ? title_save_slot_count - 1U
                                              : slot_selection_ - 1U;
    }
    if (input.confirm) {
      if (slots[slot_selection_].occupied) {
        overwrite_selected_ = true;
        phase_ = CampaignSavePhase::overwrite;
        return {};
      }
      phase_ = CampaignSavePhase::complete;
      return {CampaignSaveDecision::save, slot_selection_};
    }
    return {};
  }

  if (input.previous || input.next) {
    overwrite_selected_ = !overwrite_selected_;
  }
  if (input.cancel || (input.confirm && !overwrite_selected_)) {
    phase_ = CampaignSavePhase::slots;
    return {};
  }
  if (input.confirm) {
    phase_ = CampaignSavePhase::complete;
    return {CampaignSaveDecision::save, slot_selection_};
  }
  return {};
}

std::optional<CampaignProgress>
CampaignProgress::startUnsaved(std::uint32_t mission_index,
                               bool opening_movie_already_played) noexcept {
  if (mission_index >= missionCatalog().size()) {
    return std::nullopt;
  }
  return CampaignProgress{std::nullopt, mission_index, mission_index,
                          opening_movie_already_played};
}

std::optional<CampaignProgress>
CampaignProgress::startNew(TitleSaveSlots &slots, std::uint32_t mission_index,
                           bool opening_movie_already_played) noexcept {
  if (mission_index >= missionCatalog().size()) {
    return std::nullopt;
  }
  const auto empty = std::ranges::find_if(
      slots, [](const TitleSaveSlot &slot) { return !slot.occupied; });
  if (empty == slots.end()) {
    return std::nullopt;
  }
  const auto save_slot = static_cast<std::size_t>(empty - slots.begin());
  return startNewInSlot(slots, save_slot, mission_index,
                        opening_movie_already_played);
}

std::optional<CampaignProgress>
CampaignProgress::startNewInSlot(TitleSaveSlots &slots, std::size_t save_slot,
                                 std::uint32_t mission_index,
                                 bool opening_movie_already_played) noexcept {
  if (save_slot >= slots.size() || mission_index >= missionCatalog().size()) {
    return std::nullopt;
  }
  slots[save_slot] = TitleSaveSlot{true, mission_index, false};
  return CampaignProgress{save_slot, mission_index, mission_index,
                          opening_movie_already_played};
}

std::optional<CampaignProgress>
CampaignProgress::resume(const TitleSaveSlots &slots,
                         std::size_t save_slot) noexcept {
  if (save_slot >= slots.size()) {
    return std::nullopt;
  }
  const auto &slot = slots[save_slot];
  if (!slot.occupied || slot.campaign_complete ||
      slot.mission_index >= missionCatalog().size() ||
      (slot.pending_eol_mission &&
       *slot.pending_eol_mission != slot.mission_index)) {
    return std::nullopt;
  }
  // Loading a mission starts before its native SOL handoff. This includes
  // Georgia Street: only New Game has already played SOL/SUBWAY.STR in the
  // title overlay.
  return CampaignProgress{save_slot, slot.mission_index, slot.mission_index,
                          slot.pending_eol_mission.has_value(),
                          slot.pending_eol_mission};
}

bool CampaignProgress::openingMovieRequired(
    const MissionDefinition &mission) const noexcept {
  return active_ && mission.index == mission_index_ && !pending_eol_mission_ &&
         !opening_movie_handled_ && !mission.opening_movie_path.empty();
}

bool CampaignProgress::stageMissionCompletion(TitleSaveSlots &slots) noexcept {
  if (!save_slot_) {
    return false;
  }
  return stageMissionCompletionInSlot(slots, *save_slot_);
}

bool CampaignProgress::stageMissionCompletionInSlot(
    TitleSaveSlots &slots, std::size_t save_slot,
    std::optional<CampaignCarryState> carry) noexcept {
  if (!active_ || pending_eol_mission_ || save_slot >= slots.size() ||
      mission_index_ >= missionCatalog().size()) {
    return false;
  }
  if (mission_index_ + 1U >= missionCatalog().size() ||
      !campaignMissionsShareCarry(mission_index_, mission_index_ + 1U)) {
    carry.reset();
  }
  save_slot_ = save_slot;
  slots[save_slot] = TitleSaveSlot{true, mission_index_, false, mission_index_,
                                   std::move(carry)};
  pending_eol_mission_ = mission_index_;
  opening_movie_handled_ = true;
  return true;
}

CampaignAdvance
CampaignProgress::completeMission(TitleSaveSlots &slots) noexcept {
  if (!active_ || !save_slot_ || *save_slot_ >= slots.size() ||
      mission_index_ >= missionCatalog().size()) {
    return CampaignAdvance::invalid;
  }
  const auto &saved = slots[*save_slot_];
  if (!saved.occupied || saved.campaign_complete ||
      saved.mission_index != mission_index_ ||
      saved.pending_eol_mission != pending_eol_mission_) {
    return CampaignAdvance::invalid;
  }

  const auto completed_mission = mission_index_;
  const auto saved_carry = saved.carry;
  const auto result = advance();
  if (result == CampaignAdvance::campaign_complete) {
    slots[*save_slot_] = TitleSaveSlot{true, completed_mission, true,
                                       std::nullopt, std::nullopt};
  } else if (result == CampaignAdvance::next_mission) {
    slots[*save_slot_] = TitleSaveSlot{
        true, mission_index_, false, std::nullopt,
        campaignMissionsShareCarry(completed_mission, mission_index_)
            ? saved_carry
            : std::nullopt};
  }
  return result;
}

CampaignAdvance CampaignProgress::completeMissionWithoutSaving() noexcept {
  if (!active_ || pending_eol_mission_ ||
      mission_index_ >= missionCatalog().size()) {
    return CampaignAdvance::invalid;
  }
  return advance();
}

bool CampaignProgress::selectUnlockedMission(
    std::uint32_t mission_index) noexcept {
  if (!active_ || pending_eol_mission_ ||
      mission_index > maximum_unlocked_mission_ ||
      mission_index >= missionCatalog().size()) {
    return false;
  }
  mission_index_ = mission_index;
  opening_movie_handled_ = false;
  return true;
}

CampaignAdvance CampaignProgress::advance() noexcept {
  if (!active_ || mission_index_ >= missionCatalog().size()) {
    return CampaignAdvance::invalid;
  }

  const auto next = mission_index_ + 1U;
  if (next >= missionCatalog().size()) {
    pending_eol_mission_.reset();
    active_ = false;
    return CampaignAdvance::campaign_complete;
  }

  mission_index_ = next;
  maximum_unlocked_mission_ =
      std::max(maximum_unlocked_mission_, mission_index_);
  opening_movie_handled_ = false;
  pending_eol_mission_.reset();
  return CampaignAdvance::next_mission;
}

} // namespace sf::game
