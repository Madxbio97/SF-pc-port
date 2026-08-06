#include "sf/game/campaign.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void testConnectedRetailCampaign() {
  sf::game::TitleSaveSlots slots{};
  auto campaign = sf::game::CampaignProgress::startNew(slots, 0U, true);
  require(campaign && campaign->saveSlot() == 0U,
          "New campaign did not reserve the first empty save slot");
  require(!campaign->openingMovieRequired(sf::game::missionDefinition(0U)),
          "New Game replayed the SOL movie already handled by the title");

  const auto mission_count = sf::game::missionCatalog().size();
  for (std::uint32_t mission = 0U; mission < mission_count; ++mission) {
    require(campaign->active() && campaign->missionIndex() == mission,
            "Campaign cursor skipped a retail mission");
    const auto &definition = sf::game::missionDefinition(mission);
    if (mission != 0U) {
      require(campaign->openingMovieRequired(definition) ==
                  !definition.opening_movie_path.empty(),
              "Campaign SOL handoff does not match the mission catalog");
    }
    campaign->markOpeningMovieHandled();
    require(campaign->stageMissionCompletion(slots) &&
                slots[0].pending_eol_mission == mission,
            "Campaign did not durably stage its EOL transaction");
    const auto advance = campaign->completeMission(slots);
    const auto is_final = mission + 1U == mission_count;
    require(advance == (is_final ? sf::game::CampaignAdvance::campaign_complete
                                 : sf::game::CampaignAdvance::next_mission),
            "Campaign completion produced the wrong transition");
  }

  require(!campaign->active() && slots[0].occupied &&
              slots[0].campaign_complete &&
              slots[0].mission_index + 1U == mission_count,
          "Final mission did not persist campaign completion");
  require(!sf::game::CampaignProgress::resume(slots, 0U),
          "A completed save replayed the final mission");
}

void testAllRetailTransitionAssets() {
  constexpr std::array<std::string_view, 20U> opening{
      "SOL/SUBWAY.STR",
      "",
      "",
      "SOL/PARK.STR",
      "SOL/PARK2.STR",
      "SOL/MUSEUM.STR",
      "",
      "SOL/BASEEXT.STR",
      "",
      "SOL/CHOPPER.STR",
      "",
      "SOL/CHURCH.STR",
      "",
      "SOL/CATACOMB.STR",
      "SOL/WHOUSE.STR",
      "SOL/WHOUSE2.STR",
      "",
      "",
      "",
      "",
  };
  constexpr std::array<std::string_view, 20U> ending{
      "EOL/SUBWAY.STR",
      "EOL/SUBWAY2.STR",
      "EOL/SUBWAY3.STR",
      "",
      "EOL/PARK2.STR",
      "EOL/MUSEUM.STR",
      "",
      "",
      "",
      "EOL/CHOPPER.STR",
      "EOL/BASEEXT2.STR",
      "EOL/CHURCH.STR",
      "",
      "EOL/CATACOMB.STR",
      "",
      "EOL/WHOUSE2.STR",
      "EOL/INWHOUSE.STR",
      "EOL/CAVE.STR",
      "EOL/CAVE2.STR",
      "EOL/SILO.STR",
  };
  constexpr std::array<std::string_view, 20U> first_scripted{
      "SOL/INTRO.STR",
      "",
      "",
      "",
      "",
      "CUT/MUSEUM.STR",
      "CUT/MUSEUM2.STR",
      "",
      "",
      "",
      "",
      "",
      "CUT/CHURCH2.STR",
      "CUT/CATACOMB.STR",
      "CUT/WHOUSE.STR",
      "",
      "",
      "",
      "",
      "CUT/SILO.STR",
  };
  constexpr std::array<std::string_view, 20U> second_scripted{
      "", "",
      "", "",
      "", "",
      "", "",
      "", "",
      "", "",
      "", "CUT/CAT2.STR",
      "", "",
      "", "",
      "", "CUT/SILO2.STR",
  };

  const auto catalog = sf::game::missionCatalog();
  require(catalog.size() == opening.size(),
          "Retail transition table does not cover every mission");
  for (std::size_t index = 0U; index < catalog.size(); ++index) {
    require(catalog[index].index == index &&
                catalog[index].opening_movie_path == opening[index] &&
                catalog[index].ending_movie_path == ending[index],
            "Mission SOL/EOL transition mapping mismatch");
    const auto scripted =
        sf::game::missionScriptedMoviePaths(static_cast<std::uint32_t>(index));
    const auto expected_count = second_scripted[index].empty()
                                    ? (first_scripted[index].empty() ? 0U : 1U)
                                    : 2U;
    require(
        scripted.size() == expected_count &&
            (expected_count == 0U || scripted[0] == first_scripted[index]) &&
            (expected_count < 2U || scripted[1] == second_scripted[index]),
        "Mission scripted FMV transition mapping mismatch");
  }
}

void testRetailSavePromptAndTransientCampaign() {
  const auto museum2_movies = sf::game::missionScriptedMoviePaths(6U);
  const auto catacomb_movies = sf::game::missionScriptedMoviePaths(13U);
  require(museum2_movies.size() == 1U &&
              museum2_movies.front() == "CUT/MUSEUM2.STR" &&
              catacomb_movies.size() == 2U &&
              catacomb_movies[0] == "CUT/CATACOMB.STR" &&
              catacomb_movies[1] == "CUT/CAT2.STR",
          "Retail scripted CUT movie catalog is incomplete or out of order");

  sf::game::CampaignSaveMenu menu;
  sf::game::TitleSaveSlots empty_slots{};
  require(menu.phase() == sf::game::CampaignSavePhase::prompt &&
              menu.saveSelected(),
          "Mission-complete save prompt did not default to Save");
  require(menu.update({.confirm = true}, empty_slots).decision ==
                  sf::game::CampaignSaveDecision::none &&
              menu.phase() == sf::game::CampaignSavePhase::slots,
          "Accepting the save prompt did not open the memory-card menu");
  static_cast<void>(menu.update({.next = true}, empty_slots));
  require(menu.update({.confirm = true}, empty_slots).decision ==
                  sf::game::CampaignSaveDecision::save &&
              menu.slotSelection() == 1U,
          "Memory-card menu did not return the selected save slot");

  sf::game::CampaignSaveMenu declined;
  static_cast<void>(declined.update({.next = true}, empty_slots));
  require(declined.update({.confirm = true}, empty_slots).decision ==
              sf::game::CampaignSaveDecision::continue_without_saving,
          "Declining the save prompt did not continue the campaign");

  sf::game::TitleSaveSlots occupied_slots{};
  occupied_slots[0] = sf::game::TitleSaveSlot{true, 8U, false};
  sf::game::CampaignSaveMenu overwrite;
  static_cast<void>(overwrite.update({.confirm = true}, occupied_slots));
  require(overwrite.update({.confirm = true}, occupied_slots).decision ==
                  sf::game::CampaignSaveDecision::none &&
              overwrite.phase() == sf::game::CampaignSavePhase::overwrite &&
              overwrite.overwriteSelected(),
          "Occupied mission slot did not request overwrite confirmation");
  static_cast<void>(overwrite.update({.next = true}, occupied_slots));
  require(overwrite.update({.confirm = true}, occupied_slots).decision ==
                  sf::game::CampaignSaveDecision::none &&
              overwrite.phase() == sf::game::CampaignSavePhase::slots,
          "Declining overwrite did not return to the slot picker");
  static_cast<void>(overwrite.update({.confirm = true}, occupied_slots));
  require(overwrite.update({.confirm = true}, occupied_slots).decision ==
                  sf::game::CampaignSaveDecision::save &&
              overwrite.slotSelection() == 0U,
          "Confirmed overwrite did not return the occupied save slot");

  sf::game::TitleSaveSlots slots{};
  slots[3] = sf::game::TitleSaveSlot{true, 7U, false};
  const auto durable_before = slots;
  auto campaign = sf::game::CampaignProgress::startUnsaved(0U, true);
  require(campaign && !campaign->saveSlot(),
          "New Game reserved a memory-card slot before the retail prompt");
  require(campaign->completeMissionWithoutSaving() ==
                  sf::game::CampaignAdvance::next_mission &&
              campaign->missionIndex() == 1U && slots == durable_before,
          "Declining mission save changed durable slots or broke progression");

  require(campaign->stageMissionCompletionInSlot(slots, 3U) &&
              campaign->saveSlot() == 3U && slots[3].pending_eol_mission == 1U,
          "Accepted mission save did not stage the explicitly selected slot");
  require(campaign->completeMission(slots) ==
                  sf::game::CampaignAdvance::next_mission &&
              slots[3] == sf::game::TitleSaveSlot{true, 2U, false},
          "Accepted mission save did not finalize after EOL");
}

void testLoadAndFailurePaths() {
  sf::game::TitleSaveSlots slots{};
  slots[2] = sf::game::TitleSaveSlot{true, 0U, false};
  auto loaded = sf::game::CampaignProgress::resume(slots, 2U);
  require(loaded &&
              loaded->openingMovieRequired(sf::game::missionDefinition(0U)),
          "Loading mission 1 skipped SOL/SUBWAY.STR");

  auto tampered = slots;
  tampered[2].mission_index = 4U;
  require(loaded->completeMission(tampered) ==
                  sf::game::CampaignAdvance::invalid &&
              loaded->missionIndex() == 0U,
          "Campaign advanced after its durable save cursor was changed");

  sf::game::TitleSaveSlots full{};
  for (std::size_t slot = 0; slot < full.size(); ++slot) {
    full[slot] =
        sf::game::TitleSaveSlot{true, static_cast<std::uint32_t>(slot), false};
  }
  const auto unchanged = full;
  require(!sf::game::CampaignProgress::startNew(full, 0U, true) &&
              full == unchanged,
          "New Game silently overwrote a full save set");
}

void testLoadedProgressSurvivesMissionReplay() {
  sf::game::TitleSaveSlots slots{};
  slots[1] = sf::game::TitleSaveSlot{true, 6U, false};
  const auto durable_before = slots;
  auto campaign = sf::game::CampaignProgress::resume(slots, 1U);
  require(campaign && campaign->missionIndex() == 6U &&
              campaign->maximumUnlockedMission() == 6U,
          "Loaded campaign lost its mission high-water mark");
  require(
      campaign->selectUnlockedMission(2U) && campaign->missionIndex() == 2U &&
          campaign->maximumUnlockedMission() == 6U && slots == durable_before,
      "Replaying an earlier mission lowered durable progress");
  require(campaign->completeMissionWithoutSaving() ==
                  sf::game::CampaignAdvance::next_mission &&
              campaign->missionIndex() == 3U &&
              campaign->maximumUnlockedMission() == 6U &&
              slots == durable_before,
          "Earlier replay did not advance inside the unlocked range");
  require(!campaign->selectUnlockedMission(7U),
          "Campaign replay accepted a mission beyond saved progress");
  const auto reloaded = sf::game::CampaignProgress::resume(slots, 1U);
  require(reloaded && reloaded->missionIndex() == 6U &&
              reloaded->maximumUnlockedMission() == 6U,
          "Reloading after a replay reset saved mission progress");

  require(campaign->selectUnlockedMission(2U) &&
              campaign->stageMissionCompletionInSlot(slots, 1U) &&
              slots[1].pending_eol_mission == 2U,
          "Explicit replay overwrite did not stage the completed mission");
  require(campaign->completeMission(slots) ==
                  sf::game::CampaignAdvance::next_mission &&
              campaign->missionIndex() == 3U &&
              campaign->maximumUnlockedMission() == 6U &&
              slots[1] == sf::game::TitleSaveSlot{true, 3U, false},
          "Explicit replay overwrite did not replace the selected slot");
}

void testSaveMigrationAndCompletedSlotUi() {
  constexpr std::string_view version_one_save{"SFPC_SAVE_V1\n"
                                              "0 1 7\n"
                                              "1 0 0\n"
                                              "2 0 0\n"
                                              "3 0 0\n"
                                              "4 0 0\n"};
  const auto migrated = sf::game::parseTitleSaveSlots(version_one_save);
  require(migrated && (*migrated)[0].occupied &&
              (*migrated)[0].mission_index == 7U &&
              !(*migrated)[0].campaign_complete &&
              !(*migrated)[0].pending_eol_mission &&
              (*migrated)[0].difficulty ==
                  sf::game::CampaignDifficulty::original,
          "V1 campaign save did not migrate to the current slot model");

  constexpr std::string_view version_two_save{"SFPC_SAVE_V2\n"
                                              "0 1 11 0\n"
                                              "1 0 0 0\n"
                                              "2 0 0 0\n"
                                              "3 0 0 0\n"
                                              "4 0 0 0\n"};
  const auto version_two = sf::game::parseTitleSaveSlots(version_two_save);
  require(version_two && (*version_two)[0].occupied &&
              (*version_two)[0].mission_index == 11U &&
              !(*version_two)[0].pending_eol_mission &&
              (*version_two)[0].difficulty ==
                  sf::game::CampaignDifficulty::original,
          "V2 campaign save did not migrate to the current slot model");

  constexpr std::string_view version_three_save{
      "SFPC_SAVE_V3\n"
      "0 1 4 0 0 0\n"
      "1 0 0 0 0 0\n"
      "2 0 0 0 0 0\n"
      "3 0 0 0 0 0\n"
      "4 0 0 0 0 0\n"};
  const auto version_three =
      sf::game::parseTitleSaveSlots(version_three_save);
  require(version_three &&
              (*version_three)[0].difficulty ==
                  sf::game::CampaignDifficulty::original,
          "V3 campaign save did not default to Original difficulty");

  constexpr std::string_view version_four_save{
      "SFPC_SAVE_V4\n"
      "0 1 9 0 0 0 0\n"
      "1 0 0 0 0 0 0\n"
      "2 0 0 0 0 0 0\n"
      "3 0 0 0 0 0 0\n"
      "4 0 0 0 0 0 0\n"};
  const auto version_four =
      sf::game::parseTitleSaveSlots(version_four_save);
  require(version_four &&
              (*version_four)[0].difficulty ==
                  sf::game::CampaignDifficulty::original,
          "V4 campaign save did not default to Original difficulty");

  const auto current_bytes = sf::game::serializeTitleSaveSlots(*version_two);
  require(current_bytes.starts_with("SFPC_SAVE_V5\n") &&
              sf::game::parseTitleSaveSlots(current_bytes) == version_two,
          "Migrated V2 save did not round-trip through V5");

  sf::game::TitleSaveSlots completed{};
  completed[0] = sf::game::TitleSaveSlot{true, 19U, true};
  const auto round_trip = sf::game::parseTitleSaveSlots(
      sf::game::serializeTitleSaveSlots(completed));
  require(round_trip && *round_trip == completed,
          "Completed campaign slot did not round-trip");

  sf::game::TitleMenu menu;
  menu.completeSearch();
  menu.setSaveSlots(completed);
  static_cast<void>(menu.update({.next = true}));
  require(menu.update({.confirm = true}) == sf::game::TitleCommand::none &&
              menu.phase() == sf::game::TitlePhase::load_slots,
          "Completed-save test did not open the slot picker");
  require(menu.update({.confirm = true}) == sf::game::TitleCommand::none &&
              menu.phase() == sf::game::TitlePhase::load_slots,
          "Completed campaign slot replayed the final mission");
}

void testCampaignDifficultyPersistence() {
  using sf::game::CampaignDifficulty;

  require(sf::game::campaignDifficultyDisplayName(
              CampaignDifficulty::original) == "Normal" &&
              sf::game::campaignDifficultyDisplayName(
                  CampaignDifficulty::hard_mode) == "Hard Mode" &&
              sf::game::campaignDifficultyDisplayName(
                  CampaignDifficulty::agent) == "Agent",
          "Campaign difficulty display names do not match the title menu");
  require(
      sf::game::campaignDifficultyGameplayNotice(CampaignDifficulty::original)
              .empty() &&
          sf::game::campaignDifficultyGameplayNotice(
              CampaignDifficulty::hard_mode) == "Playing on HARD difficulty" &&
          sf::game::campaignDifficultyGameplayNotice(
              CampaignDifficulty::agent) == "Playing Agent mode",
      "Campaign difficulty gameplay notices are incorrect");
  require(
      sf::game::campaignDifficultyGameplayPresentation(
          "Playing on HARD difficulty", CampaignDifficulty::agent) ==
              "Playing Agent mode" &&
          sf::game::campaignDifficultyGameplayPresentation(
              "Playing on HARD difficulty", CampaignDifficulty::hard_mode) ==
              "Playing on HARD difficulty" &&
          sf::game::campaignDifficultyGameplayPresentation(
              "Playing on HARD", CampaignDifficulty::agent) ==
              "Playing Agent mode" &&
          sf::game::campaignDifficultyGameplayPresentation(
              "Checkpoint", CampaignDifficulty::agent) == "Checkpoint",
      "Agent gameplay presentation did not isolate the retail Hard notice");

  sf::game::TitleSaveSlots slots{};
  auto campaign = sf::game::CampaignProgress::startNew(
      slots, 0U, true, CampaignDifficulty::agent);
  require(campaign && campaign->difficulty() == CampaignDifficulty::agent &&
              slots[0].difficulty == CampaignDifficulty::agent,
          "New Agent campaign did not persist its difficulty");
  require(campaign->stageMissionCompletion(slots) &&
              slots[0].difficulty == CampaignDifficulty::agent,
          "Staging mission completion lost Agent difficulty");

  const auto durable =
      sf::game::parseTitleSaveSlots(sf::game::serializeTitleSaveSlots(slots));
  require(durable && (*durable)[0].difficulty == CampaignDifficulty::agent,
          "Agent difficulty did not round-trip through V5");
  auto resumed = sf::game::CampaignProgress::resume(*durable, 0U);
  auto finalized = *durable;
  require(resumed && resumed->difficulty() == CampaignDifficulty::agent &&
              resumed->completeMission(finalized) ==
                  sf::game::CampaignAdvance::next_mission &&
              finalized[0].difficulty == CampaignDifficulty::agent,
          "Resuming or advancing a campaign lost Agent difficulty");

  auto unsaved = sf::game::CampaignProgress::startUnsaved(
      0U, true, CampaignDifficulty::hard_mode);
  sf::game::TitleSaveSlots target{};
  require(unsaved &&
              unsaved->stageMissionCompletionInSlot(target, 3U) &&
              target[3].difficulty == CampaignDifficulty::hard_mode,
          "Saving an unsaved Hard Mode campaign lost its difficulty");

  require(!sf::game::CampaignProgress::startUnsaved(
              0U, true, static_cast<CampaignDifficulty>(255U)),
          "Campaign accepted an invalid difficulty");
  constexpr std::string_view invalid_difficulty{
      "SFPC_SAVE_V5\n"
      "0 1 0 0 0 0 0 3\n"
      "1 0 0 0 0 0 0 0\n"
      "2 0 0 0 0 0 0 0\n"
      "3 0 0 0 0 0 0 0\n"
      "4 0 0 0 0 0 0 0\n"};
  require(!sf::game::parseTitleSaveSlots(invalid_difficulty),
          "V5 parser accepted an invalid difficulty");
}

void testInterruptedEolRecovery() {
  sf::game::TitleSaveSlots normal_slots{};
  auto normal = sf::game::CampaignProgress::startNew(normal_slots, 0U, true);
  require(normal && normal->stageMissionCompletion(normal_slots),
          "Normal mission EOL was not staged");
  const auto normal_durable = sf::game::parseTitleSaveSlots(
      sf::game::serializeTitleSaveSlots(normal_slots));
  require(normal_durable && (*normal_durable)[0].pending_eol_mission == 0U,
          "Interrupted normal EOL was not durable");
  auto resumed_normal = sf::game::CampaignProgress::resume(*normal_durable, 0U);
  require(resumed_normal && resumed_normal->pendingEndingMovieMission() == 0U &&
              !resumed_normal->openingMovieRequired(
                  sf::game::missionDefinition(0U)),
          "Loading an interrupted EOL replayed SOL/gameplay");
  auto finalized_normal = *normal_durable;
  require(resumed_normal->completeMission(finalized_normal) ==
                  sf::game::CampaignAdvance::next_mission &&
              finalized_normal[0].mission_index == 1U &&
              !finalized_normal[0].pending_eol_mission,
          "Normal interrupted EOL did not atomically advance");

  constexpr std::uint32_t final_mission = 19U;
  sf::game::TitleSaveSlots final_slots{};
  auto final =
      sf::game::CampaignProgress::startNew(final_slots, final_mission, false);
  require(final && final->stageMissionCompletion(final_slots),
          "Final mission EOL was not staged");
  const auto final_durable = sf::game::parseTitleSaveSlots(
      sf::game::serializeTitleSaveSlots(final_slots));
  require(final_durable &&
              (*final_durable)[0].pending_eol_mission == final_mission,
          "Interrupted final EOL was not durable");
  auto resumed_final = sf::game::CampaignProgress::resume(*final_durable, 0U);
  auto finalized_final = *final_durable;
  require(resumed_final &&
              resumed_final->pendingEndingMovieMission() == final_mission &&
              resumed_final->completeMission(finalized_final) ==
                  sf::game::CampaignAdvance::campaign_complete &&
              finalized_final[0].campaign_complete &&
              !finalized_final[0].pending_eol_mission,
          "Final interrupted EOL did not atomically complete campaign");
}

void testConnectedMissionCarryAndSaveRoundTrip() {
  for (std::uint32_t completed = 0U; completed < 19U; ++completed) {
    const auto boundary = completed == 4U || completed == 6U ||
                          completed == 10U || completed == 13U;
    require(sf::game::campaignMissionsShareCarry(completed, completed + 1U) ==
                !boundary,
            "Campaign carry groups do not match the authored chapters");
  }
  require(!sf::game::campaignMissionsShareCarry(0U, 2U) &&
              !sf::game::campaignMissionsShareCarry(19U, 20U),
          "Campaign carry accepted a non-adjacent or terminal transition");

  sf::game::CampaignCarryState carry;
  const auto unarmed = static_cast<unsigned>(sf::game::WeaponId::unarmed);
  const auto silenced = static_cast<unsigned>(sf::game::WeaponId::silenced_9mm);
  const auto shotgun =
      static_cast<unsigned>(sf::game::WeaponId::combat_shotgun);
  carry.current_weapon = static_cast<std::uint8_t>(shotgun);
  carry.owned_weapons = (std::uint32_t{1U} << unarmed) |
                        (std::uint32_t{1U} << silenced) |
                        (std::uint32_t{1U} << shotgun);
  carry.magazines[silenced] = 7U;
  carry.reserves[silenced] = 31U;
  carry.magazines[shotgun] = 4U;
  carry.reserves[shotgun] = 18U;
  carry.health = 87U;
  carry.armor = 321U;
  require(sf::game::validCampaignCarry(carry),
          "Valid campaign carry was rejected");

  sf::game::TitleSaveSlots slots{};
  auto campaign = sf::game::CampaignProgress::startNew(slots, 0U, true);
  require(campaign &&
              campaign->stageMissionCompletionInSlot(slots, 0U, carry) &&
              slots[0].carry == carry,
          "Mission 1 completion did not stage its player state");
  const auto durable =
      sf::game::parseTitleSaveSlots(sf::game::serializeTitleSaveSlots(slots));
  require(durable && (*durable)[0].carry == carry,
          "Campaign player state did not round-trip through V5");
  auto resumed = sf::game::CampaignProgress::resume(*durable, 0U);
  auto finalized = *durable;
  require(resumed &&
              resumed->completeMission(finalized) ==
                  sf::game::CampaignAdvance::next_mission &&
              finalized[0].mission_index == 1U && finalized[0].carry == carry,
          "Interrupted EOL lost its carried player state");

  sf::game::TitleSaveSlots boundary_slots{};
  auto boundary =
      sf::game::CampaignProgress::startNewInSlot(boundary_slots, 0U, 4U, false);
  require(
      boundary &&
          boundary->stageMissionCompletionInSlot(boundary_slots, 0U, carry) &&
          !boundary_slots[0].carry &&
          boundary->completeMission(boundary_slots) ==
              sf::game::CampaignAdvance::next_mission &&
          boundary_slots[0].mission_index == 5U && !boundary_slots[0].carry,
      "Mission 5->6 boundary leaked the previous chapter loadout");

  auto invalid = carry;
  invalid.owned_weapons |=
      std::uint32_t{1U} << static_cast<unsigned>(sf::game::WeaponId::key_card);
  sf::game::TitleSaveSlots invalid_slots{};
  invalid_slots[0] =
      sf::game::TitleSaveSlot{true, 1U, false, std::nullopt, invalid};
  const auto invalid_path =
      std::filesystem::temp_directory_path() / "sf_invalid_carry.sav";
  require(!sf::game::storeTitleSaveSlotsFile(invalid_path, invalid_slots),
          "Objective inventory leaked into a durable campaign loadout");
}

void testExplicitFullSaveOverwrite() {
  sf::game::TitleSaveSlots full{};
  for (std::size_t slot = 0U; slot < full.size(); ++slot) {
    full[slot] = sf::game::TitleSaveSlot{
        true, static_cast<std::uint32_t>(slot + 2U), false};
  }
  const auto before = full;

  sf::game::TitleMenu menu;
  menu.completeSearch();
  menu.setSaveSlots(full);
  require(menu.update({.confirm = true}) == sf::game::TitleCommand::none &&
              menu.phase() == sf::game::TitlePhase::select_difficulty &&
              full == before,
          "New Game changed a slot before difficulty selection");
  static_cast<void>(menu.update({}));
  require(menu.update({.confirm = true}) ==
                  sf::game::TitleCommand::new_game &&
              menu.phase() == sf::game::TitlePhase::menu && full == before,
          "New Game requested or changed a slot before mission completion");
}

void testAgentWarningGate() {
  using sf::game::CampaignDifficulty;
  using sf::game::TitleCommand;
  using sf::game::TitlePhase;

  sf::game::TitleSaveSlots slots{};
  const auto before = slots;
  sf::game::TitleMenu menu;
  menu.completeSearch();
  menu.setSaveSlots(slots);
  require(menu.update({.confirm = true}) == TitleCommand::none &&
              menu.phase() == TitlePhase::select_difficulty,
          "New Game did not open the difficulty selector");
  static_cast<void>(menu.update({}));
  static_cast<void>(menu.update({.next = true}));
  static_cast<void>(menu.update({.next = true}));
  require(menu.selectedDifficulty() == CampaignDifficulty::agent &&
              menu.update({.confirm = true, .confirm_down = true}) ==
                  TitleCommand::none &&
              menu.phase() == TitlePhase::agent_warning &&
              menu.saveSlots() == before,
          "Agent selection bypassed its warning or changed save data");
  require(menu.update({.confirm = true, .confirm_down = true}) ==
                  TitleCommand::none &&
              menu.phase() == TitlePhase::agent_warning,
          "Held confirm skipped the Agent warning");
  require(menu.update({.cancel = true}) == TitleCommand::none &&
              menu.phase() == TitlePhase::select_difficulty &&
              menu.selectedDifficulty() == CampaignDifficulty::agent,
          "Agent warning did not return to the preserved selection");

  require(menu.update({.confirm = true, .confirm_down = true}) ==
                  TitleCommand::none &&
              menu.phase() == TitlePhase::agent_warning,
          "Agent warning did not reopen after returning to difficulty");
  require(menu.update({.confirm_down = true}) == TitleCommand::none &&
              menu.phase() == TitlePhase::agent_warning,
          "Held confirm leaked through the reopened Agent warning");
  static_cast<void>(menu.update({}));
  require(menu.update({.confirm = true}) == TitleCommand::new_game &&
              menu.phase() == TitlePhase::menu && menu.saveSlots() == before,
          "Fresh confirmation did not accept the Agent warning");

  sf::game::TitleMenu hard;
  hard.completeSearch();
  static_cast<void>(hard.update({.confirm = true}));
  static_cast<void>(hard.update({}));
  static_cast<void>(hard.update({.next = true}));
  require(hard.selectedDifficulty() == CampaignDifficulty::hard_mode &&
              hard.update({.confirm = true}) == TitleCommand::new_game &&
              hard.phase() == TitlePhase::menu,
          "Hard Mode incorrectly entered the Agent warning");
}

void testUserDataSaveMigrationAndRecovery() {
  const auto directory =
      std::filesystem::temp_directory_path() / "sf_campaign_save_tests";
  std::filesystem::remove_all(directory);
  const auto cue_path = directory / "readonly-rom" / "game.cue";
  const auto user_data = directory / "user-data";
  const auto location =
      sf::game::titleSaveLocation(cue_path, "SCUS-94240", user_data);
  require(location.primary ==
                  user_data / "SyphonFilterPC" / "Saves" / "SCUS-94240.sav" &&
              location.legacy == cue_path.parent_path() / "SyphonFilterPC.sav",
          "Campaign save location is not stable or keyed by game serial");

  std::filesystem::create_directories(location.legacy.parent_path());
  constexpr std::string_view legacy_version_one{"SFPC_SAVE_V1\n"
                                                "0 1 7\n"
                                                "1 0 0\n"
                                                "2 0 0\n"
                                                "3 0 0\n"
                                                "4 0 0\n"};
  {
    std::ofstream output{location.legacy, std::ios::binary};
    output << legacy_version_one;
  }
  require(sf::game::migrateLegacyTitleSaveSlotsFile(location) ==
                  sf::game::TitleSaveMigrationStatus::migrated &&
              std::filesystem::exists(location.legacy),
          "Read-only-compatible migration removed or skipped the legacy save");

  const auto migrated = sf::game::loadTitleSaveSlotsFile(location.primary);
  auto backup = location.primary;
  backup += ".bak";
  require(migrated.status == sf::game::TitleSaveLoadStatus::loaded &&
              migrated.slots[0] == sf::game::TitleSaveSlot{true, 7U, false} &&
              std::filesystem::exists(backup),
          "Migrated V1 save or its durable backup is incomplete");

  {
    std::ofstream corrupt{location.primary, std::ios::binary | std::ios::trunc};
    corrupt << "damaged";
  }
  const auto recovered = sf::game::loadTitleSaveSlotsFile(location.primary);
  require(recovered.status == sf::game::TitleSaveLoadStatus::recovered &&
              recovered.slots == migrated.slots,
          "Corrupt user-data primary did not recover its complete backup");

  require(sf::game::migrateLegacyTitleSaveSlotsFile(location) ==
              sf::game::TitleSaveMigrationStatus::not_needed,
          "Migration overwrote an existing user-data primary or backup");

  {
    std::ofstream corrupt{backup, std::ios::binary | std::ios::trunc};
    corrupt << "also damaged";
  }
  require(sf::game::loadTitleSaveSlotsFile(location.primary).status ==
                  sf::game::TitleSaveLoadStatus::invalid &&
              sf::game::migrateLegacyTitleSaveSlotsFile(location) ==
                  sf::game::TitleSaveMigrationStatus::migrated,
          "Corrupt primary/backup pair masked a valid legacy save");
  const auto remigrated = sf::game::loadTitleSaveSlotsFile(location.primary);
  const auto repaired_backup = sf::game::loadTitleSaveSlotsFile(backup);
  require(remigrated.status == sf::game::TitleSaveLoadStatus::loaded &&
              remigrated.slots[0] == sf::game::TitleSaveSlot{true, 7U, false} &&
              repaired_backup.status == sf::game::TitleSaveLoadStatus::loaded &&
              repaired_backup.slots == remigrated.slots,
          "Legacy recovery did not replace the corrupt backup with V5");
  std::filesystem::remove_all(directory);
}

void testBackupRepairFailureKeepsPrimary() {
  const auto directory =
      std::filesystem::temp_directory_path() / "sf_campaign_backup_failure";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto primary = directory / "campaign.sav";
  auto impossible_backup = primary;
  impossible_backup += ".bak";
  std::filesystem::create_directories(impossible_backup / "occupied");

  sf::game::TitleSaveSlots slots{};
  slots[0] = sf::game::TitleSaveSlot{true, 3U, false};
  require(!sf::game::storeTitleSaveSlotsFile(primary, slots),
          "Impossible backup unexpectedly reported a durable commit");
  const auto committed = sf::game::loadTitleSaveSlotsFile(primary);
  require(committed.status == sf::game::TitleSaveLoadStatus::loaded &&
              committed.slots == slots,
          "Backup repair failure deleted the only valid primary save");
  std::filesystem::remove_all(directory);
}

} // namespace

int main() {
  testConnectedRetailCampaign();
  testAllRetailTransitionAssets();
  testRetailSavePromptAndTransientCampaign();
  testLoadAndFailurePaths();
  testLoadedProgressSurvivesMissionReplay();
  testSaveMigrationAndCompletedSlotUi();
  testCampaignDifficultyPersistence();
  testInterruptedEolRecovery();
  testConnectedMissionCarryAndSaveRoundTrip();
  testExplicitFullSaveOverwrite();
  testAgentWarningGate();
  testUserDataSaveMigrationAndRecovery();
  testBackupRepairFailureKeepsPrimary();
  return 0;
}
