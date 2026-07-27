#include "sf/core/error.hpp"
#include "sf/game/campaign.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/internal/g4_campaign_transition_probe_access.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace {

constexpr std::uint32_t maximum_transition_updates = 320U;
constexpr std::uint32_t maximum_checkpoint_wait_updates = 64U;
constexpr std::uint32_t maximum_control_wait_updates = 2'000U;
constexpr std::uint32_t stable_control_updates = 8U;

int fail(std::uint32_t mission, std::string_view phase,
         const sf::game::GameplaySession &gameplay) {
  std::cerr << "G4 campaign transition gate failed: mission=" << mission
            << " phase=" << phase;
  if (const auto state =
          sf::game::G4CampaignTransitionProbeAccess::applicationState(
              gameplay)) {
    std::cerr << " state=" << *state;
  }
  std::cerr << " sequence=" << gameplay.legacyPresentationSequence()
            << " fault=" << gameplay.runtimeFaulted() << " finished="
            << sf::game::G4CampaignTransitionProbeAccess::runtimeFinished(
                   gameplay);
  if (const auto mission_state =
          sf::game::G4CampaignTransitionProbeAccess::liveMissionState(
              gameplay)) {
    std::cerr << " terminal=" << mission_state->terminal
              << " success=" << mission_state->success
              << " failure=" << mission_state->failure
              << " transition=" << mission_state->failure_transition;
  }
  std::cerr << '\n';
  return 2;
}

bool coherentPresentation(const sf::game::GameplaySession &gameplay,
                          std::uint64_t after_sequence) {
  const auto frame = gameplay.legacyPresentationFrame();
  return !gameplay.runtimeFaulted() &&
         gameplay.legacyRenderCommandsAuthoritative() && frame &&
         sf::game::legacyPresentationFrameConsumable(*frame, after_sequence) &&
         gameplay.legacyPresentationSequence() == frame->sequence;
}

bool step(sf::game::GameplaySession &gameplay) {
  gameplay.update({});
  return gameplay.advanceAudioFrameClock() ||
         sf::game::G4CampaignTransitionProbeAccess::runtimeFinished(gameplay);
}

bool captureStableCheckpoint(sf::game::GameplaySession &gameplay) {
  for (std::uint32_t update = 0U; update < maximum_checkpoint_wait_updates;
       ++update) {
    if (sf::game::G4CampaignTransitionProbeAccess::captureCheckpoint(
            gameplay)) {
      return true;
    }
    if (!step(gameplay) || gameplay.runtimeFaulted()) {
      return false;
    }
  }
  return false;
}

bool productionControlReady(const sf::game::GameplaySession &gameplay) {
  const auto frame = gameplay.legacyPresentationFrame();
  if (!frame || !frame->renderer || !gameplay.legacyOpeningFinished()) {
    return false;
  }
  const auto &state = frame->renderer->state;
  return state.player.resident && !state.player.control_locked &&
         !state.camera.scripted && !state.camera.locked;
}

bool carriesState(const sf::game::CampaignCarryState &actual,
                  const sf::game::CampaignCarryState &expected) {
  if (actual.current_weapon != expected.current_weapon ||
      actual.health != expected.health || actual.armor != expected.armor ||
      (actual.owned_weapons & expected.owned_weapons) !=
          expected.owned_weapons) {
    return false;
  }
  for (std::size_t weapon = 0U; weapon < sf::game::weapon_slot_count;
       ++weapon) {
    const auto bit = std::uint32_t{1U} << weapon;
    if ((expected.owned_weapons & bit) != 0U &&
        (actual.magazines[weapon] != expected.magazines[weapon] ||
         actual.reserves[weapon] != expected.reserves[weapon])) {
      return false;
    }
  }
  return true;
}

bool waitForActiveGameplay(sf::game::GameplaySession &gameplay,
                           std::uint64_t &updates) {
  auto stable = std::uint32_t{};
  for (std::uint32_t update = 0U;
       update < maximum_control_wait_updates && stable < stable_control_updates;
       ++update) {
    const auto previous_sequence = gameplay.legacyPresentationSequence();
    if (!step(gameplay)) {
      return false;
    }
    ++updates;
    // The headless gate acknowledges the same guest edge as the platform;
    // decoding the STR does not mutate gameplay state.
    static_cast<void>(gameplay.consumeScriptedIntroMovieRequest());
    if (!coherentPresentation(gameplay, previous_sequence)) {
      return false;
    }
    stable = productionControlReady(gameplay) ? stable + 1U : 0U;
  }
  return stable == stable_control_updates;
}

int runProbe(const std::filesystem::path &cue_path) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{
        sf::core::ErrorCode::unsupported,
        "G4 campaign transition probe requires Syphon Filter USA v1.1"};
  }

  sf::game::TitleSaveSlots saves{};
  auto campaign = sf::game::CampaignProgress::startNew(saves, 0U, true);
  if (!campaign) {
    std::cerr << "G4 campaign transition gate failed: campaign start\n";
    return 2;
  }

  std::uint64_t transition_updates{};
  std::uint32_t failure_restarts{};
  std::uint32_t ending_requests{};
  std::uint32_t staged_completions{};
  std::uint32_t finalized_completions{};
  auto campaign_carry = std::optional<sf::game::CampaignCarryState>{};
  const auto mission_count =
      static_cast<std::uint32_t>(sf::game::missionCatalog().size());
  for (const auto &mission : sf::game::missionCatalog()) {
    if (!campaign->active() || campaign->missionIndex() != mission.index ||
        campaign->openingMovieRequired(mission) !=
            (mission.index != 0U && !mission.opening_movie_path.empty())) {
      std::cerr << "G4 campaign transition gate failed: mission="
                << mission.index << " phase=campaign-entry\n";
      return 2;
    }
    campaign->markOpeningMovieHandled();

    const auto package = sf::game::MissionPackage::load(disc, mission.index);
    auto gameplay = std::make_unique<sf::game::GameplaySession>(package);
    if (campaign_carry &&
        (!gameplay->applyCampaignCarryState(*campaign_carry) ||
         !gameplay->campaignCarryState() ||
         !carriesState(*gameplay->campaignCarryState(), *campaign_carry))) {
      return fail(mission.index, "campaign-carry-apply", *gameplay);
    }
    const auto boot_sequence = gameplay->legacyPresentationSequence();
    if (boot_sequence == 0U || gameplay->runtimeFaulted()) {
      return fail(mission.index, "production-boot", *gameplay);
    }
    if (!step(*gameplay)) {
      return fail(mission.index, "coherent-frame-audio-clock", *gameplay);
    }
    if (!coherentPresentation(*gameplay, boot_sequence)) {
      return fail(mission.index, "coherent-frame", *gameplay);
    }
    if (!waitForActiveGameplay(*gameplay, transition_updates)) {
      return fail(mission.index, "active-gameplay", *gameplay);
    }
    if (!captureStableCheckpoint(*gameplay)) {
      return fail(mission.index, "checkpoint-capture", *gameplay);
    }

    const auto save_before_failure = saves;
    if (!sf::game::G4CampaignTransitionProbeAccess::invokeRetailFailure(
            *gameplay)) {
      return fail(mission.index, "retail-failure-callback", *gameplay);
    }

    auto restarted = false;
    for (std::uint32_t update = 0U; update < maximum_transition_updates;
         ++update) {
      if (!step(*gameplay)) {
        return fail(mission.index, "failure-transition-audio-clock", *gameplay);
      }
      ++transition_updates;
      if (gameplay->runtimeFaulted()) {
        return fail(mission.index, "failure-transition", *gameplay);
      }
      if (!gameplay->failureRestartRequested()) {
        continue;
      }
      if (gameplay->consumeEndingMovieRequest()) {
        return fail(mission.index, "failure-issued-ending", *gameplay);
      }
      const auto terminal_sequence = gameplay->legacyPresentationSequence();
      if (!gameplay->restartCheckpoint()) {
        return fail(mission.index, "failure-checkpoint-restore", *gameplay);
      }
      ++failure_restarts;
      restarted = true;
      if (gameplay->failureRestartRequested() || gameplay->missionFailed() ||
          !coherentPresentation(*gameplay, terminal_sequence)) {
        return fail(mission.index, "failure-restart-edge", *gameplay);
      }
      break;
    }
    if (!restarted || saves != save_before_failure) {
      return fail(mission.index, "failure-exactly-once", *gameplay);
    }
    const auto restored_sequence = gameplay->legacyPresentationSequence();
    if (!step(*gameplay)) {
      return fail(mission.index, "post-restart-audio-clock", *gameplay);
    }
    if (gameplay->failureRestartRequested() ||
        gameplay->consumeEndingMovieRequest() ||
        !coherentPresentation(*gameplay, restored_sequence)) {
      return fail(mission.index, "post-restart-healthy-frame", *gameplay);
    }

    gameplay->reset();
    if (gameplay->runtimeFaulted() || gameplay->failureRestartRequested() ||
        gameplay->consumeEndingMovieRequest() ||
        !gameplay->legacyPresentationFrame()) {
      return fail(mission.index, "mission-reset", *gameplay);
    }
    if (!waitForActiveGameplay(*gameplay, transition_updates)) {
      return fail(mission.index, "reset-active-gameplay", *gameplay);
    }
    if (campaign_carry &&
        (!gameplay->campaignCarryState() ||
         !carriesState(*gameplay->campaignCarryState(), *campaign_carry))) {
      return fail(mission.index, "campaign-carry-reset", *gameplay);
    }
    if (!sf::game::G4CampaignTransitionProbeAccess::invokeRetailSuccess(
            *gameplay)) {
      return fail(mission.index, "retail-success-callback", *gameplay);
    }

    auto ending_requested = false;
    for (std::uint32_t update = 0U; update < maximum_transition_updates;
         ++update) {
      if (!step(*gameplay)) {
        return fail(mission.index, "success-transition-audio-clock", *gameplay);
      }
      ++transition_updates;
      if (gameplay->runtimeFaulted() || gameplay->failureRestartRequested()) {
        return fail(mission.index, "success-transition", *gameplay);
      }
      if (!gameplay->consumeEndingMovieRequest()) {
        continue;
      }
      ++ending_requests;
      ending_requested = true;
      if (gameplay->consumeEndingMovieRequest()) {
        return fail(mission.index, "ending-request-repeated", *gameplay);
      }
      break;
    }
    if (!ending_requested) {
      return fail(mission.index, "ending-request-timeout", *gameplay);
    }

    const auto save_slot = campaign->saveSlot();
    const auto next_mission = mission.index + 1U;
    const auto carry_for_next =
        sf::game::campaignMissionsShareCarry(mission.index, next_mission)
            ? gameplay->campaignCarryState()
            : std::nullopt;
    if (!save_slot ||
        (sf::game::campaignMissionsShareCarry(mission.index, next_mission) &&
         !carry_for_next) ||
        !campaign->stageMissionCompletionInSlot(saves, *save_slot,
                                                carry_for_next)) {
      return fail(mission.index, "campaign-eol-stage", *gameplay);
    }
    ++staged_completions;
    const auto staged_saves = saves;
    if (campaign->pendingEndingMovieMission() != mission.index ||
        saves[*save_slot].pending_eol_mission != mission.index ||
        campaign->openingMovieRequired(mission) ||
        campaign->stageMissionCompletion(saves) || saves != staged_saves) {
      return fail(mission.index, "campaign-eol-stage-exactly-once", *gameplay);
    }

    auto resumed = sf::game::CampaignProgress::resume(saves, *save_slot);
    if (!resumed || resumed->missionIndex() != mission.index ||
        resumed->pendingEndingMovieMission() != mission.index ||
        resumed->openingMovieRequired(mission)) {
      return fail(mission.index, "campaign-eol-resume", *gameplay);
    }
    campaign = std::move(resumed);

    const auto final_mission = mission.index + 1U == mission_count;
    const auto advance = campaign->completeMission(saves);
    ++finalized_completions;
    const auto expected = final_mission
                              ? sf::game::CampaignAdvance::campaign_complete
                              : sf::game::CampaignAdvance::next_mission;
    if (advance != expected || campaign->pendingEndingMovieMission() ||
        saves[*save_slot].pending_eol_mission ||
        (!final_mission && campaign->missionIndex() != mission.index + 1U)) {
      return fail(mission.index, "campaign-advance", *gameplay);
    }
    campaign_carry = final_mission ? std::nullopt : saves[*save_slot].carry;

    std::cout << "mission=" << mission.index
              << " resource=" << mission.resource_name
              << " failure-restart=1 ending-request=1 eol-stage/finalize=1/1 "
                 "advance="
              << (final_mission ? "complete" : "next") << '\n';
  }

  if (campaign->active() || !campaign->saveSlot() ||
      !saves[*campaign->saveSlot()].campaign_complete ||
      failure_restarts != mission_count || ending_requests != mission_count ||
      staged_completions != mission_count ||
      finalized_completions != mission_count) {
    std::cerr << "G4 campaign transition gate failed: final coverage="
              << failure_restarts << '/' << ending_requests << '/'
              << staged_completions << '/' << finalized_completions << '/'
              << mission_count << '\n';
    return 2;
  }

  std::cout << "G4 campaign transition gate passed: missions=" << mission_count
            << " retail-failures=" << failure_restarts
            << " retail-successes=" << ending_requests
            << " eol-stage/finalize=" << staged_completions << '/'
            << finalized_completions << " updates=" << transition_updates
            << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: sf_g4_campaign_transition_probe <game.cue>\n";
    return 1;
  }
  try {
    return runProbe(std::filesystem::path{argv[1]});
  } catch (const std::exception &error) {
    std::cerr << "G4 campaign transition gate failed: " << error.what() << '\n';
    return 10;
  }
}
