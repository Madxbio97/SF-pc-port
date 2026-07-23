#include "sf/game/legacy_first_mission_runtime.hpp"

#include "sf/game/legacy_mission_image.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/player_controller.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace sf::game {
namespace {

// Give the interpreted guest the same kind of CPU headroom as DuckStation's
// overclock control. PsxMachine converts elapsed CPU ticks back to retail
// device time, so SPU/CD timing and the outer 20 Hz gameplay cadence remain
// unchanged.
constexpr psx::CpuClockScale gameplay_cpu_clock_scale{6U, 1U};

void appendExecutionFault(std::string &detail, std::string_view stage,
                          const LegacyGameplayVmResult &result) {
  char buffer[192]{};
  const auto stage_length =
      static_cast<int>(std::min(stage.size(), static_cast<std::size_t>(32U)));
  const auto length = std::snprintf(
      buffer, sizeof(buffer),
      "stage=%.*s stop=%.*s pc=0x%08x instruction=0x%08x budget=%llu",
      stage_length, stage.data(),
      static_cast<int>(psx::toString(result.execution.reason).size()),
      psx::toString(result.execution.reason).data(), result.execution.pc,
      result.execution.instruction,
      static_cast<unsigned long long>(result.execution.instructions));
  if (length > 0) {
    detail.assign(buffer, static_cast<std::size_t>(std::min<int>(
                              length, static_cast<int>(sizeof(buffer) - 1U))));
  }
}

bool isGameplayState(std::uint32_t state) noexcept {
  return state == 0U || state == 5U;
}

bool openingRailFinished(const LegacyGameplayBridgeState &bridge) noexcept {
  constexpr std::uint32_t opening_camera_source = 35U;
  return opening_camera_source < bridge.objects.size() &&
         bridge.objects[opening_camera_source].health <= 0;
}

std::uint8_t padAxis(double value) noexcept {
  const auto clamped = std::clamp(value, -1.0, 1.0);
  const auto sample = static_cast<int>(std::lround(128.0 + clamped * 127.0));
  return static_cast<std::uint8_t>(std::clamp(sample, 0, 255));
}

} // namespace

LegacyHostPadState
legacyPadStateFromPlayerInput(const PlayerInput &input) noexcept {
  constexpr std::uint16_t l2 = 0x0100U;
  constexpr std::uint16_t r2 = 0x0200U;
  constexpr std::uint16_t l1 = 0x0400U;
  constexpr std::uint16_t r1 = 0x0800U;
  constexpr std::uint16_t triangle = 0x1000U;
  constexpr std::uint16_t circle = 0x2000U;
  constexpr std::uint16_t cross = 0x4000U;
  constexpr std::uint16_t square = 0x8000U;
  constexpr double walking_stick_scale = 0.55;

  LegacyHostPadState state;
  const auto movement_scale = input.run ? 1.0 : walking_stick_scale;
  if (input.aim) {
    // L1 owns the original first-person controller: the left stick moves the
    // sight horizontally/vertically while Gabe's collision root remains
    // fixed. Feed only held directional axes here. Relative mouse motion is
    // intentionally absent; it stays on the high-resolution host look/ray
    // path and therefore cannot saturate or quantize the retail PAD.
    state.left_x = padAxis(input.turn);
    state.left_y = padAxis(-input.move);
  } else {
    state.left_x = padAxis(input.turn);
    state.left_y = padAxis(-input.move * movement_scale);
  }

  const auto press = [&state](bool active, std::uint16_t button) {
    if (active) {
      state.buttons = static_cast<std::uint16_t>(state.buttons | button);
    }
  };
  // Mouse wheel/middle-button weapon commands are native UI semantics, not
  // physical PSX buttons. GameplaySession routes them into FUN_800405f4,
  // while real L2/R2 remain lossless in chase and manual aim.
  press(input.strafe < 0.0, l2);
  press(input.strafe > 0.0, r2);
  press(input.aim, l1);
  // R1 owns retail auto-lock and can rotate the sight independently. Keep
  // it available in chase mode, but never let it fight direct L1 aim.
  press(!input.aim && (input.target_lock || input.target_lock_held), r1);
  press(input.interact || input.reload, triangle);
  // Circle is retail scope zoom-out while L1 is held. Keep the physical roll
  // binding live during aim; only the chase-mode quick-turn displacement is
  // suppressed while native first-person aim owns the body heading.
  press(input.roll || (!input.aim && input.quick_turn), circle);
  press(!input.aim && input.kneel, cross);
  press(input.fire_pressed || input.fire_held, square);
  if (input.quick_turn && !input.aim) {
    state.left_y = 0xffU;
  }
  return state;
}

std::shared_ptr<const LegacyPresentationFrame>
republishLegacyCoherentPresentation(
    const std::shared_ptr<const LegacyPresentationFrame> &source,
    std::uint64_t current_sequence, std::uint64_t guest_frame,
    std::span<const LegacyPresentationCommandType> edge_commands) noexcept {
  if (!source || !source->renderer || !source->ui) {
    return {};
  }
  const auto next_sequence =
      current_sequence == std::numeric_limits<std::uint64_t>::max()
          ? current_sequence
          : current_sequence + 1U;
  try {
    auto renderer = source->renderer->state;
    renderer.weapon_events.clear();
    // A coherent streaming/terminal re-publish did not execute
    // FUN_80022120 again. Keep the packet rectangles for native residency,
    // but never turn a previously captured positive phase into a second
    // MoveImage event merely because guest_frame advanced.
    renderer.scrim.vram_moves_active = false;
    return buildLegacyPresentationFrame(next_sequence, guest_frame, renderer,
                                        source->ui->mission, edge_commands);
  } catch (...) {
    return {};
  }
}

LegacyFirstMissionRuntime::LegacyFirstMissionRuntime(
    const LegacyMissionImage &image) noexcept
    : LegacyFirstMissionRuntime(missionDefinition(0U), image) {}

LegacyFirstMissionRuntime::LegacyFirstMissionRuntime(
    const MissionDefinition &mission, const LegacyMissionImage &image) noexcept
    : mission_index_(mission.index) {
  try {
    virtual_cd_ = image.createVirtualCd();
    // Scripts, AI and triggers still advance exactly once per retail frame;
    // only the guest CPU instruction room between hardware deadlines grows.
    vm_ = std::make_unique<LegacyGameplayVm>(image.executable(),
                                             gameplay_cpu_clock_scale);
    vm_->bindSyphonFilterUsaV11BootstrapPlatformCalls();
    vm_->bindSyphonFilterUsaV11VirtualCdCalls(virtual_cd_);

    if (mission.selection_index < 0) {
      markFault();
      return;
    }
    const auto bootstrap = vm_->bootstrapMission(
        static_cast<std::uint32_t>(mission.selection_index),
        mission.index == 0U, syphonFilterUsaV11FirstMissionBootstrapProfile(),
        syphonFilterUsaV11RetailPlatformTailProfile(),
        syphonFilterUsaV11FirstMissionOpeningProfile(), 500'000'000U);
    if (!bootstrap.completed()) {
      markFault();
      return;
    }

    // Room 74's retail dynamic descriptor 6 owns the two opening CHEMO
    // actors. In the console loop it is activated before the first rail
    // draw; the host skips that frontend callback, so reproduce the exact
    // descriptor activation at the VM boundary.
    if (mission.index == 0U) {
      constexpr std::uint32_t activate_dynamic_descriptor = 0x8005fd04U;
      constexpr std::array opening_cbdc_descriptor{6U};
      const auto activation =
          vm_->invoke(activate_dynamic_descriptor, opening_cbdc_descriptor);
      if (!activation.completed()) {
        markFault();
        return;
      }
    }
    if (!vm_->writeHostPadState(LegacyHostPadState{})) {
      markFault();
      return;
    }
    // Bootstrap/loading audio is not part of the host gameplay stream.
    // Anchor the deterministic SPU clock at the first visible frame.
    vm_->clearPcm();
    // Loading creates and then destroys its own font objects before gameplay.
    // Drop those pre-scene edges so the first native HUD cannot resurrect a
    // stale loading label; the following retail frame records real mission UI.
    vm_->clearWeaponEvents();
    vm_->clearUiMessages();
    // Event activation is queued by the bootstrap. Retail advances that
    // queue before the first draw, so the visible frame starts on rail 35
    // with the first fade step already applied, never on the stale player
    // camera left in RAM by loading.
    const auto first_frame = vm_->tickRetailOuterFrame();
    if (!first_frame.completed() || !isGameplayState(first_frame.state_after)) {
      recordExecutionFault(first_frame);
      markFault();
      return;
    }
    if (!vm_->advanceAudioFrameClock()) {
      markFault();
      return;
    }

    if (!publishPresentationFrame()) {
      markFault();
      return;
    }
    auto snapshot = vm_->captureSnapshot();
    if (!snapshot.virtual_cd) {
      markFault();
      return;
    }

    initial_snapshot_.emplace(std::move(snapshot));
    ready_ = true;
    faulted_ = false;
    fault_reason_ = LegacyRuntimeFaultReason::none;
    opening_finished_ = mission_index_ != 0U || openingRailFinished(*bridge());
    finished_ = false;
  } catch (...) {
    markFault();
  }
}

void LegacyFirstMissionRuntime::setHostPadState(
    const LegacyHostPadState &state) noexcept {
  latched_pad_buttons_ =
      static_cast<std::uint16_t>(latched_pad_buttons_ | state.buttons);
  host_pad_state_ = state;
}

bool LegacyFirstMissionRuntime::applyHostAimLocomotion(
    const LegacyHostPlayerState &state) noexcept {
  if (!ready_ || finished_ || faulted_ || !vm_) {
    return false;
  }
  try {
    if (vm_->writeHostPlayerState(state)) {
      return true;
    }
  } catch (...) {
  }
  markFault();
  return false;
}

void LegacyFirstMissionRuntime::setHostAimRay(
    std::optional<LegacyHostAimRay> ray) noexcept {
  if (vm_) {
    vm_->setHostAimRay(std::move(ray));
  }
}

bool LegacyFirstMissionRuntime::restoreHostPlayerHeading(
    std::int32_t yaw) noexcept {
  if (!ready_ || finished_ || faulted_ || !vm_) {
    return false;
  }
  try {
    if (vm_->writeHostPlayerHeading(yaw)) {
      return true;
    }
  } catch (...) {
  }
  markFault();
  return false;
}

std::uint64_t LegacyFirstMissionRuntime::hostAimRayPatchCount() const noexcept {
  return vm_ ? vm_->hostAimRayPatchCount() : 0U;
}

bool LegacyFirstMissionRuntime::applyHostWeaponMenuInput(
    bool held, std::int32_t delta) noexcept {
  if (!ready_ || finished_ || !vm_) {
    return false;
  }
  try {
    if (vm_->invokeRetailWeaponMenuInput(held, delta).completed()) {
      return true;
    }
  } catch (...) {
  }
  markFault();
  return false;
}

bool LegacyFirstMissionRuntime::applyHostFirstPersonAim(bool active) noexcept {
  if (!ready_ || finished_ || faulted_ || !vm_) {
    return false;
  }
  try {
    const auto result = vm_->invokeRetailFirstPersonAim(active);
    if (result.completed()) {
      return true;
    }
    appendExecutionFault(fault_detail_, "host-first-person-aim", result);
  } catch (...) {
    fault_detail_ = "stage=host-first-person-aim stop=exception";
  }
  markFault();
  return false;
}

bool LegacyFirstMissionRuntime::activateRetailAllWeaponsCheat() noexcept {
  constexpr std::uint32_t all_weapons_entry = 0x80047c0cU;
  constexpr std::uint64_t execution_budget = 5'000'000U;
  if (!ready_ || finished_ || faulted_ || !vm_) {
    return false;
  }
  try {
    if (vm_->invoke(all_weapons_entry, {}, execution_budget).completed() &&
        publishPresentationFrame()) {
      return true;
    }
  } catch (...) {
  }
  markFault();
  return false;
}

bool LegacyFirstMissionRuntime::applyCampaignCarryState(
    const CampaignCarryState &state) noexcept {
  if (!ready_ || finished_ || faulted_ || !vm_ || !validCampaignCarry(state) ||
      !missionBridge()) {
    return false;
  }
  try {
    auto inventory = missionBridge()->inventory;
    for (std::size_t weapon = 0U; weapon < legacy_inventory_weapon_count;
         ++weapon) {
      const auto bit = std::uint32_t{1U} << weapon;
      if ((campaign_persistent_weapon_mask & bit) == 0U ||
          (state.owned_weapons & bit) == 0U) {
        continue;
      }
      inventory.owned_weapons |= bit;
      inventory.magazines[weapon] = state.magazines[weapon];
      inventory.reserves[weapon] = state.reserves[weapon];
    }
    inventory.current_weapon = state.current_weapon;

    const auto snapshot = vm_->captureSnapshot();
    const auto previous_frame = presentation_frame_;
    const auto previous_sequence = presentation_sequence_;
    const auto rollback = [&] {
      const auto restored = vm_->restoreSnapshot(snapshot);
      presentation_frame_ = previous_frame;
      presentation_sequence_ = previous_sequence;
      return restored;
    };
    try {
      if (!vm_->writeHostInventoryState(inventory) ||
          !vm_->writeHostPlayerVitals(static_cast<std::int16_t>(state.health),
                                      static_cast<std::int16_t>(state.armor)) ||
          !publishPresentationFrame()) {
        if (!rollback()) {
          markFault();
        }
        return false;
      }

      auto committed = vm_->captureSnapshot();
      if (!committed.virtual_cd) {
        if (!rollback()) {
          markFault();
        }
        return false;
      }
      // Mission restart must return to the carried chapter state, not to the
      // package's standalone defaults captured before this transaction.
      initial_snapshot_ = std::move(committed);
      return true;
    } catch (...) {
      if (!rollback()) {
        markFault();
      }
      return false;
    }
  } catch (...) {
    return false;
  }
}

bool LegacyFirstMissionRuntime::consumeCheckpointCommit() noexcept {
  const bool committed = checkpoint_commit_pending_ && presentation_frame_ &&
                         presentation_frame_->contains(
                             LegacyPresentationCommandType::checkpoint_commit);
  checkpoint_commit_pending_ = false;
  return committed;
}

std::optional<std::size_t>
LegacyFirstMissionRuntime::consumeIntroMovieRequest() noexcept {
  if (!consumeTransitionRequest(intro_movie_request) ||
      !pending_intro_movie_index_) {
    return std::nullopt;
  }
  const auto index = pending_intro_movie_index_;
  pending_intro_movie_index_.reset();
  return index;
}

bool LegacyFirstMissionRuntime::consumeEndingMovieRequest() noexcept {
  return consumeTransitionRequest(ending_movie_request);
}

bool LegacyFirstMissionRuntime::consumeFailureRestartRequest() noexcept {
  return consumeTransitionRequest(failure_restart_request);
}

bool LegacyFirstMissionRuntime::captureCheckpoint() noexcept {
  if (!ready_ || faulted_ || finished_ || !vm_ || !presentation_frame_ ||
      !presentation_frame_->renderer || !presentation_frame_->ui ||
      presentation_frame_->guest_frame != guest_frame_ ||
      consecutive_renderer_snapshot_replays_ != 0U ||
      presentation_frame_->ui->mission.terminal || transition_requests_ != 0U ||
      movie_loader_pending_) {
    return false;
  }
  try {
    std::uint32_t application_state{};
    constexpr auto outer_profile = syphonFilterUsaV11RetailOuterFrameProfile();
    if (!vm_->runtime().read32(outer_profile.current_state,
                               application_state)) {
      markFault();
      return false;
    }
    if (!legacyRuntimeCheckpointCaptureAllowed(application_state)) {
      return false;
    }
    auto snapshot = vm_->captureSnapshot();
    if (!snapshot.virtual_cd) {
      markFault();
      return false;
    }
    checkpoint_.emplace(RuntimeCheckpoint{
        std::move(snapshot),
        host_pad_state_,
        latched_pad_buttons_,
        guest_frame_,
        native_update_phase_,
        last_checkpoint_frame_,
        checkpoint_commit_pending_,
        transition_requests_,
        issued_transitions_,
        next_intro_movie_index_,
        pending_intro_movie_index_,
        movie_loader_pending_,
        opening_finished_,
        finished_,
    });
    return true;
  } catch (...) {
    markFault();
    return false;
  }
}

bool LegacyFirstMissionRuntime::restoreCheckpoint() noexcept {
  if (!vm_ || !checkpoint_) {
    markFault();
    return false;
  }
  try {
    if (!vm_->restoreSnapshot(checkpoint_->vm)) {
      markFault();
      return false;
    }
    vm_->setHostAimRay(std::nullopt);
    host_pad_state_ = checkpoint_->host_pad;
    latched_pad_buttons_ = checkpoint_->latched_pad_buttons;
    guest_frame_ = checkpoint_->guest_frame;
    native_update_phase_ = checkpoint_->native_update_phase;
    consecutive_renderer_snapshot_replays_ = 0U;
    last_checkpoint_frame_ = checkpoint_->last_checkpoint_frame;
    checkpoint_commit_pending_ = checkpoint_->checkpoint_commit_pending;
    transition_requests_ = checkpoint_->transition_requests;
    issued_transitions_ = checkpoint_->issued_transitions;
    next_intro_movie_index_ = checkpoint_->next_intro_movie_index;
    pending_intro_movie_index_ = checkpoint_->pending_intro_movie_index;
    movie_loader_pending_ = checkpoint_->movie_loader_pending;
    opening_finished_ = checkpoint_->opening_finished;
    finished_ = checkpoint_->finished;
    if (!applyRetailAudioVolumes()) {
      markFault();
      return false;
    }
    // The VM snapshot is authoritative after a failure restart. Re-reading
    // its object graph is essential: replaying the cached checkpoint bridge
    // can carry stale destroyed/dormant presentation bits into the restored
    // mission, leaving windows, locks and glass panels permanently hidden.
    // It also avoids replaying one-shot particles and weapon events captured
    // on the checkpoint edge.
    if (!publishPresentationFrame()) {
      markFault();
      return false;
    }
    ready_ = true;
    faulted_ = false;
    fault_reason_ = LegacyRuntimeFaultReason::none;
    return true;
  } catch (...) {
    markFault();
    return false;
  }
}

void LegacyFirstMissionRuntime::reset() noexcept {
  if (!vm_ || !initial_snapshot_) {
    markFault();
    return;
  }

  try {
    if (!vm_->restoreSnapshot(*initial_snapshot_)) {
      markFault();
      return;
    }
    vm_->setHostAimRay(std::nullopt);
    checkpoint_.reset();
    host_pad_state_ = {};
    latched_pad_buttons_ = 0U;
    guest_frame_ = 0U;
    native_update_phase_ = 0U;
    consecutive_renderer_snapshot_replays_ = 0U;
    last_checkpoint_frame_.reset();
    checkpoint_commit_pending_ = false;
    transition_requests_ = 0U;
    issued_transitions_ = 0U;
    next_intro_movie_index_ = 0U;
    pending_intro_movie_index_.reset();
    movie_loader_pending_ = false;
    if (!applyRetailAudioVolumes()) {
      markFault();
      return;
    }
    // Rebuild the renderer/UI bridge from the restored VM. Re-publishing the
    // cached startup frame leaves object-lifetime fields detached from the
    // freshly restored object graph; after a later mission restart that can
    // replay destroyed/dormant presentation for intact windows, locks and
    // glass panels. A fresh bridge read makes the reset one atomic guest-owned
    // transaction and also clears stale one-shot camera-list packets.
    if (!publishPresentationFrame()) {
      markFault();
      return;
    }
    ready_ = true;
    faulted_ = false;
    fault_reason_ = LegacyRuntimeFaultReason::none;
    opening_finished_ = mission_index_ != 0U || openingRailFinished(*bridge());
    finished_ = false;
  } catch (...) {
    markFault();
  }
}

void LegacyFirstMissionRuntime::advanceHostUpdate() noexcept {
  if (!ready_ || finished_ || !vm_) {
    return;
  }

  ++native_update_phase_;
  if (native_update_phase_ < native_updates_per_guest_frame) {
    return;
  }
  native_update_phase_ = 0U;

  try {
    std::uint32_t application_state{};
    constexpr auto outer_profile = syphonFilterUsaV11RetailOuterFrameProfile();
    if (!vm_->runtime().read32(outer_profile.current_state,
                               application_state)) {
      markFault();
      return;
    }
    if (application_state == 2U) {
      const auto transition_mission = vm_->readMissionBridgeState();
      if (!transition_mission) {
        markFault();
        return;
      }
      if (!legacyRetailState2DispatchAllowed(application_state,
                                             *transition_mission)) {
        classifyTransitionRequest(application_state, application_state,
                                  *transition_mission);
        if (!finished_ || !publishPresentationFrame()) {
          markFault();
        }
        return;
      }
      const auto transition = vm_->dispatchRetailState2Transition();
      if (!transition.completed() || !applyRetailAudioVolumes()) {
        markFault();
        return;
      }
      if (legacyRetailTerminalMovieBoundary(transition.final_state,
                                            *transition_mission)) {
        const auto coherent_frame = presentation_frame_;
        if (!coherent_frame) {
          markFault();
          return;
        }
        classifyTransitionRequest(application_state, transition.final_state,
                                  *transition_mission);
        if (!finished_ || !republishPresentationFrame(coherent_frame)) {
          markFault();
        }
        return;
      }
    }

    auto guest_pad = host_pad_state_;
    guest_pad.buttons =
        static_cast<std::uint16_t>(guest_pad.buttons | latched_pad_buttons_);
    if (!vm_->writeHostPadState(guest_pad)) {
      markFault();
      return;
    }

    // H4 has no native-driven gameplay frame. The retail outer loop owns
    // input processing, gameplay, player, animation and mission state.
    const auto frame = vm_->tickRetailOuterFrame();
    if (!frame.completed()) {
      recordExecutionFault(frame);
      markFault();
      return;
    }
    if (!vm_->advanceAudioFrameClock()) {
      markFault();
      return;
    }
    latched_pad_buttons_ = 0U;

    if (legacyRetailStreamingState(frame.state_after)) {
      // State 7/9 can replace the room object/definition tables in
      // several guest calls. Sampling midway through that transaction
      // couples native presentation to stale pointers from the previous
      // overlay. Keep the last coherent frame visible while the complete
      // retail stream loop advances, then rebuild on its gameplay edge.
      const auto coherent_frame = presentation_frame_;
      const auto *coherent_mission = missionBridge();
      if (!coherent_frame || coherent_mission == nullptr) {
        markFault();
        return;
      }
      classifyTransitionRequest(frame.state_before, frame.state_after,
                                *coherent_mission);
      ++guest_frame_;
      if (!republishPresentationFrame(coherent_frame)) {
        markFault();
      }
      return;
    }

    // Common mission success enters state 3/4 after the guest has already
    // retired its gameplay pointer tables. The preceding immutable frame
    // contains the exact terminal latch, so carry that coherent snapshot
    // across the movie-loader edge instead of sampling invalid pointers.
    if (frame.state_after == 3U || frame.state_after == 4U) {
      const auto coherent_frame = presentation_frame_;
      const auto *coherent_mission = missionBridge();
      if (!coherent_frame || coherent_mission == nullptr ||
          !legacyRetailTerminalMovieBoundary(frame.state_after,
                                             *coherent_mission)) {
        markFault();
        return;
      }
      classifyTransitionRequest(frame.state_before, frame.state_after,
                                *coherent_mission);
      ++guest_frame_;
      if (!finished_ || !republishPresentationFrame(coherent_frame)) {
        markFault();
      }
      return;
    }

    // A checkpoint snapshot must never bisect state 7/9's pointer-table
    // replacement. Any latch raised by retail during streaming remains in
    // RAM and is observed here on the first coherent gameplay frame.
    if (!detectCheckpointCommit()) {
      markFault();
      return;
    }

    const auto mission = vm_->readMissionBridgeState();
    if (!mission) {
      markFault();
      return;
    }
    classifyTransitionRequest(frame.state_before, frame.state_after, *mission);
    ++guest_frame_;
    if (!publishPresentationFrame()) {
      markFault();
      return;
    }
    opening_finished_ = opening_finished_ || openingRailFinished(*bridge());
  } catch (...) {
    markFault();
  }
}

bool LegacyFirstMissionRuntime::setRetailAudioVolumes(
    const LegacyRetailAudioVolumes &volumes) noexcept {
  if (!ready_ || !vm_ || !volumes.valid()) {
    return false;
  }
  if (!vm_->setRetailAudioVolumes(volumes)) {
    markFault();
    return false;
  }
  retail_audio_volumes_ = volumes;
  return true;
}

std::optional<LegacyRetailAudioVolumes>
LegacyFirstMissionRuntime::retailAudioVolumes() const noexcept {
  return vm_ ? vm_->readRetailAudioVolumes() : std::nullopt;
}

bool LegacyFirstMissionRuntime::advanceAudioFrameClock() noexcept {
  if (!ready_ || finished_ || faulted_ || !vm_) {
    return false;
  }
  if (!vm_->advanceAudioFrameClock()) {
    markFault();
    return false;
  }
  return true;
}

bool LegacyFirstMissionRuntime::applyRetailAudioVolumes() noexcept {
  return !retail_audio_volumes_ ||
         (vm_ && vm_->setRetailAudioVolumes(*retail_audio_volumes_));
}

std::size_t LegacyFirstMissionRuntime::takePcm(
    std::span<psx::SpuPcmFrame> destination) noexcept {
  return vm_ ? vm_->takePcm(destination) : 0U;
}

void LegacyFirstMissionRuntime::clearPcm() noexcept {
  if (vm_) {
    vm_->clearPcm();
  }
}

bool LegacyFirstMissionRuntime::detectCheckpointCommit() noexcept {
  constexpr std::uint32_t checkpoint_latch_address = 0x801163b1U;
  constexpr std::uint32_t checkpoint_frame_address = 0x80121950U;
  std::uint8_t checkpoint_latch{};
  std::uint32_t checkpoint_frame{};
  if (!vm_ ||
      !vm_->runtime().read8(checkpoint_latch_address, checkpoint_latch) ||
      !vm_->runtime().read32(checkpoint_frame_address, checkpoint_frame)) {
    return false;
  }
  if (checkpoint_latch == 0U ||
      (last_checkpoint_frame_ && *last_checkpoint_frame_ == checkpoint_frame)) {
    return true;
  }

  last_checkpoint_frame_ = checkpoint_frame;
  checkpoint_commit_pending_ = true;
  return true;
}

bool LegacyFirstMissionRuntime::consumeTransitionRequest(
    std::uint8_t request) noexcept {
  const auto command =
      request == intro_movie_request
          ? LegacyPresentationCommandType::play_intro_fmv
      : request == ending_movie_request
          ? LegacyPresentationCommandType::play_ending_fmv
          : LegacyPresentationCommandType::restart_after_failure;
  return consumeLegacyPresentationTransitionRequest(
      transition_requests_, request, presentation_frame_, command);
}

void LegacyFirstMissionRuntime::classifyTransitionRequest(
    std::uint32_t state_before, std::uint32_t state_after,
    const LegacyMissionBridgeState &mission) noexcept {
  const auto issue = [this](std::uint8_t request) {
    // A mission can contain more than one retail state-9 CUT handoff. The
    // pending request bit de-duplicates a live edge; only terminal requests
    // remain permanently latched in issued_transitions_.
    if (request == intro_movie_request) {
      if ((transition_requests_ & intro_movie_request) == 0U) {
        pending_intro_movie_index_ = next_intro_movie_index_++;
      }
      transition_requests_ =
          static_cast<std::uint8_t>(transition_requests_ | intro_movie_request);
      return;
    }
    if ((issued_transitions_ & request) != 0U) {
      return;
    }
    issued_transitions_ =
        static_cast<std::uint8_t>(issued_transitions_ | request);
    transition_requests_ =
        static_cast<std::uint8_t>(transition_requests_ | request);
  };

  const auto decision = classifyLegacyMissionTransition(
      mission_index_, state_before, state_after, mission, movie_loader_pending_,
      missionScriptedMoviePaths(mission_index_).size());
  movie_loader_pending_ = decision.movie_loader_pending;
  if (decision.request_intro_movie) {
    issue(intro_movie_request);
  }
  if (decision.request_ending_movie) {
    issue(ending_movie_request);
  }
  if (decision.request_failure_restart) {
    issue(failure_restart_request);
  }
  if (decision.finished) {
    finished_ = true;
  }
}

std::vector<LegacyPresentationCommandType>
LegacyFirstMissionRuntime::pendingPresentationCommands() const {
  std::vector<LegacyPresentationCommandType> commands;
  commands.reserve(4U);
  if (checkpoint_commit_pending_) {
    commands.push_back(LegacyPresentationCommandType::checkpoint_commit);
  }
  if ((transition_requests_ & intro_movie_request) != 0U) {
    commands.push_back(LegacyPresentationCommandType::play_intro_fmv);
  }
  if ((transition_requests_ & ending_movie_request) != 0U) {
    commands.push_back(LegacyPresentationCommandType::play_ending_fmv);
  }
  if ((transition_requests_ & failure_restart_request) != 0U) {
    commands.push_back(LegacyPresentationCommandType::restart_after_failure);
  }
  return commands;
}

bool LegacyFirstMissionRuntime::publishPresentationFrame() noexcept {
  try {
    if (!vm_) {
      fault_reason_ = LegacyRuntimeFaultReason::execution;
      return false;
    }
    auto renderer_profile = syphonFilterUsaV11GameplayBridgeProfile();
    renderer_profile.scrim.enabled =
        mission_index_ == 0U || mission_index_ == 11U || mission_index_ == 12U;
    renderer_profile.park2_flamethrower.enabled = mission_index_ == 4U;
    auto renderer = vm_->readBridgeState(renderer_profile);
    auto ui = vm_->readMissionBridgeState();
    if (!ui) {
      fault_reason_ = LegacyRuntimeFaultReason::mission_bridge;
      return false;
    }
    if (!renderer) {
      // Retail pointer tables have a few short hand-off frames outside the
      // explicit state-7/9 streaming loop. Keep advancing authoritative guest
      // logic and republish the last immutable renderer with the current UI;
      // this is the same guest-owned coherence rule used by streaming, not a
      // native gameplay fallback. Persistent corruption still fails closed.
      constexpr auto maximum_consecutive_replays = std::uint8_t{8U};
      const auto coherent_renderer =
          presentation_frame_ && presentation_frame_->renderer
              ? &presentation_frame_->renderer->state
              : nullptr;
      if (vm_->lastBridgeReadFault() !=
              LegacyGameplayBridgeReadFault::invalid_snapshot ||
          coherent_renderer == nullptr ||
          consecutive_renderer_snapshot_replays_ >=
              maximum_consecutive_replays) {
        fault_reason_ = LegacyRuntimeFaultReason::renderer_bridge;
        return false;
      }
      ++consecutive_renderer_snapshot_replays_;
      if (!publishPresentationFrame(*coherent_renderer, *ui)) {
        return false;
      }
      vm_->clearWeaponEvents();
      vm_->clearUiMessages();
      return true;
    }
    consecutive_renderer_snapshot_replays_ = 0U;
    if (!publishPresentationFrame(*renderer, *ui)) {
      fault_reason_ = LegacyRuntimeFaultReason::presentation_contract;
      return false;
    }
    vm_->clearWeaponEvents();
    vm_->clearUiMessages();
    return true;
  } catch (...) {
    fault_reason_ = LegacyRuntimeFaultReason::presentation_contract;
    return false;
  }
}

bool LegacyFirstMissionRuntime::publishPresentationFrame(
    const LegacyGameplayBridgeState &renderer,
    const LegacyMissionBridgeState &ui) noexcept {
  try {
    const auto next_sequence =
        presentation_sequence_ == std::numeric_limits<std::uint64_t>::max()
            ? presentation_sequence_
            : presentation_sequence_ + 1U;
    auto frame =
        buildLegacyPresentationFrame(next_sequence, guest_frame_, renderer, ui,
                                     pendingPresentationCommands());
    if (!frame) {
      fault_reason_ = LegacyRuntimeFaultReason::presentation_contract;
      return false;
    }
    presentation_sequence_ = next_sequence;
    presentation_frame_ = std::move(frame);
    return true;
  } catch (...) {
    fault_reason_ = LegacyRuntimeFaultReason::presentation_contract;
    return false;
  }
}

bool LegacyFirstMissionRuntime::republishPresentationFrame(
    const std::shared_ptr<const LegacyPresentationFrame> &source) noexcept {
  const auto commands = pendingPresentationCommands();
  const auto frame = republishLegacyCoherentPresentation(
      source, presentation_sequence_, guest_frame_, commands);
  if (!frame) {
    fault_reason_ = LegacyRuntimeFaultReason::presentation_contract;
    return false;
  }
  presentation_sequence_ = frame->sequence;
  presentation_frame_ = frame;
  if (vm_) {
    vm_->clearWeaponEvents();
    vm_->clearUiMessages();
  }
  return true;
}

void LegacyFirstMissionRuntime::markFault(
    LegacyRuntimeFaultReason reason) noexcept {
  // A lower presentation layer can identify the exact failed contract before
  // its caller enters the common fault path. Preserve that specific reason;
  // the default execution reason is only a fallback for older call sites.
  if (fault_reason_ == LegacyRuntimeFaultReason::none ||
      reason != LegacyRuntimeFaultReason::execution) {
    fault_reason_ = reason;
  }
  if (vm_) {
    vm_->clearWeaponEvents();
    vm_->clearUiMessages();
  }
  checkpoint_commit_pending_ = false;
  transition_requests_ = 0U;
  movie_loader_pending_ = false;
  const auto next_sequence =
      presentation_sequence_ == std::numeric_limits<std::uint64_t>::max()
          ? presentation_sequence_
          : presentation_sequence_ + 1U;
  try {
    if (auto frame =
            buildLegacyPresentationFaultFrame(next_sequence, guest_frame_)) {
      presentation_sequence_ = next_sequence;
      presentation_frame_ = std::move(frame);
    } else {
      presentation_frame_.reset();
    }
  } catch (...) {
    presentation_frame_.reset();
  }
  ready_ = false;
  faulted_ = true;
  opening_finished_ = true;
  finished_ = true;
}

void LegacyFirstMissionRuntime::recordExecutionFault(
    const LegacyRetailOuterFrameResult &frame) noexcept {
  try {
    fault_detail_.clear();
    for (std::size_t index = 0; index < frame.guest_calls.size(); ++index) {
      if (!frame.guest_calls[index].completed()) {
        const auto stage = index == 0U   ? std::string_view{"input"}
                           : index == 1U ? std::string_view{"gameplay"}
                           : index == 2U ? std::string_view{"player"}
                                         : std::string_view{"guest-call"};
        appendExecutionFault(fault_detail_, stage, frame.guest_calls[index]);
        return;
      }
    }
    if (frame.renderer_tail && !frame.renderer_tail->completed()) {
      appendExecutionFault(fault_detail_, "renderer", *frame.renderer_tail);
      return;
    }
    if (frame.bridge_fault) {
      fault_detail_ = "stage=outer-frame bridge-fault";
      if (!frame.bridge_fault_stage.empty()) {
        fault_detail_.append(" detail=");
        fault_detail_.append(frame.bridge_fault_stage);
      }
      return;
    }
    if (frame.unsupported_state) {
      char buffer[128]{};
      const auto length = std::snprintf(
          buffer, sizeof(buffer),
          "stage=outer-frame unsupported-state before=%u after=%u",
          frame.state_before, frame.state_after);
      fault_detail_.assign(
          buffer,
          static_cast<std::size_t>(std::max(
              0, std::min(length, static_cast<int>(sizeof(buffer) - 1U)))));
      return;
    }
    if (!frame.renderer_tail &&
        !frame.platform_tail.delayed_callbacks.completed()) {
      appendExecutionFault(fault_detail_, "delayed-callbacks",
                           frame.platform_tail.delayed_callbacks);
      return;
    }
    if (!frame.renderer_tail && frame.platform_tail.fade_callback &&
        !frame.platform_tail.fade_callback->completed()) {
      appendExecutionFault(fault_detail_, "fade-callback",
                           *frame.platform_tail.fade_callback);
      return;
    }
  } catch (...) {
    fault_detail_.clear();
  }
}

} // namespace sf::game
