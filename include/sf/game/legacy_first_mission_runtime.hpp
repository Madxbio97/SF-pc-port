#pragma once

#include "sf/game/campaign_state.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::game {

enum class LegacyRuntimeFaultReason {
  none,
  execution,
  renderer_bridge,
  mission_bridge,
  presentation_contract,
};

[[nodiscard]] constexpr std::string_view
legacyRuntimeFaultReasonName(LegacyRuntimeFaultReason reason) noexcept {
  switch (reason) {
  case LegacyRuntimeFaultReason::none:
    return "none";
  case LegacyRuntimeFaultReason::execution:
    return "guest-execution";
  case LegacyRuntimeFaultReason::renderer_bridge:
    return "renderer-bridge";
  case LegacyRuntimeFaultReason::mission_bridge:
    return "mission-bridge";
  case LegacyRuntimeFaultReason::presentation_contract:
    return "presentation-contract";
  }
  return "unknown-runtime-fault";
}

class LegacyMissionImage;
struct MissionDefinition;
struct PlayerInput;
class G4CampaignTransitionProbeAccess;

[[nodiscard]] LegacyHostPadState
legacyPadStateFromPlayerInput(const PlayerInput &input) noexcept;

// Host input can sample many presentation frames before one 20 Hz retail
// tick. If R1 auto-lock was latched before L1 manual aim, both buttons used to
// reach FUN_8002fa48 together and leave the two camera controllers fighting.
// Manual aim owns that transition and must discard only the stale R1 bit.
[[nodiscard]] constexpr std::uint16_t
legacyManualAimTransitionButtons(std::uint16_t current_buttons,
                                 std::uint16_t latched_buttons,
                                 bool manual_aim_active) noexcept {
  constexpr std::uint16_t target_lock_button = 0x0800U;
  const auto combined =
      static_cast<std::uint16_t>(current_buttons | latched_buttons);
  return manual_aim_active
             ? static_cast<std::uint16_t>(combined & ~target_lock_button)
             : combined;
}

// FUN_80017844's 0x80115cca byte marks the beginning of the failure fade.
// The retail restart is ready only after its callback reaches application
// state 2; stopping the VM at the byte would freeze that fade in progress.
[[nodiscard]] constexpr bool legacyMissionFailureRestartReady(
    std::uint32_t application_state,
    const LegacyMissionBridgeState &mission) noexcept {
  return mission.failure && application_state == 2U;
}

// A terminal failure deliberately stops at state 2 so the host can restore
// the captured checkpoint. Active transitions and SILO's successful terminal
// preamble must still run the retail state-stack dispatcher.
[[nodiscard]] constexpr bool legacyRetailState2DispatchAllowed(
    std::uint32_t application_state,
    const LegacyMissionBridgeState &mission) noexcept {
  return application_state == 2U && !mission.failure;
}

// States 7 and 9 own asynchronous room/overlay replacement. Their common
// executable profile is shared by every USA v1.1 mission, but object,
// definition and camera pointers may temporarily belong to different sides of
// the stream transaction. Continue executing the guest while retaining the
// last immutable presentation snapshot; a fresh bridge is safe once retail
// returns to a gameplay state.
[[nodiscard]] constexpr bool
legacyRetailStreamingState(std::uint32_t application_state) noexcept {
  return application_state == 7U || application_state == 9U;
}

// A successful retail mission invalidates its gameplay bridge while entering
// the common movie loader. The last coherent immutable mission snapshot owns
// the success latch at this boundary and must be re-published by the host.
[[nodiscard]] constexpr bool legacyRetailTerminalMovieBoundary(
    std::uint32_t application_state,
    const LegacyMissionBridgeState &mission) noexcept {
  return (application_state == 3U || application_state == 4U) &&
         mission.terminal && mission.success;
}

// Manual/native checkpoint requests must observe the same transaction fence
// as automatic retail checkpoint latches. Only the two stable gameplay states
// own a coherent, non-transitioning object graph. State 2 is the failure/title
// dispatcher, 3/4 are FMV hand-offs and 7/9 replace room/overlay tables.
[[nodiscard]] constexpr bool legacyRuntimeCheckpointCaptureAllowed(
    std::uint32_t application_state) noexcept {
  return application_state == 0U || application_state == 5U;
}

// Re-publish the last coherent immutable snapshot while retail completes a
// streaming transaction. Only sequence/guest-frame and edge commands change;
// renderer/UI data remains sourced from one prior coherent frame.
[[nodiscard]] std::shared_ptr<const LegacyPresentationFrame>
republishLegacyCoherentPresentation(
    const std::shared_ptr<const LegacyPresentationFrame> &source,
    std::uint64_t current_sequence, std::uint64_t guest_frame,
    std::span<const LegacyPresentationCommandType> edge_commands = {}) noexcept;

struct LegacyMissionTransitionDecision {
  bool movie_loader_pending{};
  bool request_intro_movie{};
  bool request_ending_movie{};
  bool request_failure_restart{};
  bool finished{};
};

[[nodiscard]] constexpr LegacyMissionTransitionDecision
classifyLegacyMissionTransition(std::uint32_t mission_index,
                                std::uint32_t state_before,
                                std::uint32_t state_after,
                                const LegacyMissionBridgeState &mission,
                                bool movie_loader_pending,
                                std::size_t scripted_movie_count = 0U) noexcept {
  LegacyMissionTransitionDecision result;
  result.movie_loader_pending = movie_loader_pending;
  const auto gameplay_state = state_after == 0U || state_after == 5U;
  if (scripted_movie_count != 0U && state_after == 9U && !mission.terminal) {
    result.movie_loader_pending = true;
  }
  if (scripted_movie_count != 0U && result.movie_loader_pending &&
      state_before == 9U && gameplay_state && !mission.terminal) {
    result.movie_loader_pending = false;
    // SUBWAY.OVL uses the same retail state-9 loader for both its
    // mid-mission INTRO and the final source-30 MOVIE handoff. Objective
    // bit 3 is set only after the upper subway bomb has been tagged, which
    // is the overlay's exact prerequisite for the latter path.
    if (mission_index == 0U &&
        (mission.completed_objectives & 0x08U) != 0U) {
      result.request_ending_movie = true;
      result.finished = true;
    } else {
      result.request_intro_movie = true;
    }
  }
  // Common success enters the retail movie loader through state 4 after
  // its short state-5 fade. Some overlay-specific paths expose state 3
  // directly. Both are terminal EOL boundaries; neither is another guest
  // gameplay frame.
  if (legacyRetailTerminalMovieBoundary(state_after, mission)) {
    result.request_ending_movie = true;
    result.finished = true;
  }
  if (legacyMissionFailureRestartReady(state_after, mission)) {
    result.request_failure_restart = true;
    result.finished = true;
  }
  return result;
}

// Edge requests remain pending until the exact immutable frame carrying their
// command is available. A consumer observing the previous frame must not lose
// a terminal transition before the runtime can publish its new sequence.
[[nodiscard]] inline bool consumeLegacyPresentationTransitionRequest(
    std::uint8_t &pending_requests, std::uint8_t request,
    const std::shared_ptr<const LegacyPresentationFrame> &frame,
    LegacyPresentationCommandType command) noexcept {
  if ((pending_requests & request) == 0U || !frame ||
      !frame->contains(command)) {
    return false;
  }
  pending_requests = static_cast<std::uint8_t>(pending_requests & ~request);
  return true;
}

// Owns one original campaign-mission runtime at the native/guest frame
// boundary. The historical class name is retained for API compatibility, but
// the production constructor accepts every retail MissionDefinition. Gameplay
// remains guest-authoritative. The host supplies PAD and receives one
// immutable renderer/UI command frame per retail tick; native first-person
// camera presentation never rewrites the guest collision/root transform.
class LegacyFirstMissionRuntime final {
public:
  explicit LegacyFirstMissionRuntime(const LegacyMissionImage &image) noexcept;
  LegacyFirstMissionRuntime(const MissionDefinition &mission,
                            const LegacyMissionImage &image) noexcept;

  LegacyFirstMissionRuntime(const LegacyFirstMissionRuntime &) = delete;
  LegacyFirstMissionRuntime &
  operator=(const LegacyFirstMissionRuntime &) = delete;
  LegacyFirstMissionRuntime(LegacyFirstMissionRuntime &&) = delete;
  LegacyFirstMissionRuntime &operator=(LegacyFirstMissionRuntime &&) = delete;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] bool finished() const noexcept { return finished_; }
  [[nodiscard]] bool openingFinished() const noexcept {
    return opening_finished_;
  }
  [[nodiscard]] std::uint64_t guestFrame() const noexcept {
    return guest_frame_;
  }
  [[nodiscard]] bool faulted() const noexcept { return faulted_; }
  [[nodiscard]] LegacyRuntimeFaultReason faultReason() const noexcept {
    return fault_reason_;
  }
  [[nodiscard]] std::string_view faultDetail() const noexcept {
    return fault_detail_.empty() ? std::string_view{"none"}
                                 : std::string_view{fault_detail_};
  }
  [[nodiscard]] LegacyGameplayBridgeReadFault
  rendererBridgeFault() const noexcept {
    return vm_ ? vm_->lastBridgeReadFault()
               : LegacyGameplayBridgeReadFault::none;
  }
  [[nodiscard]] LegacyGameplayBridgeReadStage
  rendererBridgeStage() const noexcept {
    return vm_ ? vm_->lastBridgeReadStage()
               : LegacyGameplayBridgeReadStage::none;
  }
  [[nodiscard]] const std::shared_ptr<const LegacyPresentationFrame> &
  presentationFrame() const noexcept {
    return presentation_frame_;
  }
  [[nodiscard]] const LegacyGameplayBridgeState *bridge() const noexcept {
    return presentation_frame_ && presentation_frame_->renderer
               ? &presentation_frame_->renderer->state
               : nullptr;
  }
  [[nodiscard]] const LegacyMissionBridgeState *missionBridge() const noexcept {
    return presentation_frame_ && presentation_frame_->ui
               ? &presentation_frame_->ui->mission
               : nullptr;
  }

  void setHostPadState(const LegacyHostPadState &state) noexcept;
  [[nodiscard]] bool
  applyHostAimLocomotion(const LegacyHostPlayerLocomotion &state) noexcept;
  void setHostAimRay(std::optional<LegacyHostAimRay> ray) noexcept;
  [[nodiscard]] bool restoreHostPlayerHeading(std::int32_t yaw) noexcept;
  [[nodiscard]] std::uint64_t hostAimRayPatchCount() const noexcept;
  [[nodiscard]] bool applyHostWeaponMenuInput(bool held,
                                              std::int32_t delta) noexcept;
  [[nodiscard]] bool applyHostFirstPersonAim(bool active) noexcept;
  [[nodiscard]] bool activateRetailAllWeaponsCheat() noexcept;
  [[nodiscard]] bool setRetailAllWeaponsCheat(bool enabled) noexcept;
  [[nodiscard]] bool setRetailHardMode(bool enabled) noexcept;
  [[nodiscard]] bool setRetailOneShotKills(bool enabled) noexcept;
  [[nodiscard]] bool setRetailWeakEnemies(bool enabled) noexcept;
  [[nodiscard]] bool activateRetailMovieTheaterCheat() noexcept;
  [[nodiscard]] bool
  applyCampaignCarryState(const CampaignCarryState &state) noexcept;
  [[nodiscard]] bool consumeCheckpointCommit() noexcept;
  [[nodiscard]] std::optional<std::size_t>
  consumeIntroMovieRequest() noexcept;
  [[nodiscard]] bool consumeEndingMovieRequest() noexcept;
  [[nodiscard]] bool consumeFailureRestartRequest() noexcept;
  [[nodiscard]] bool captureCheckpoint() noexcept;
  [[nodiscard]] bool restoreCheckpoint() noexcept;
  void reset() noexcept;
  void advanceHostUpdate() noexcept;
  [[nodiscard]] bool
  setRetailAudioVolumes(const LegacyRetailAudioVolumes &volumes) noexcept;
  [[nodiscard]] std::optional<LegacyRetailAudioVolumes>
  retailAudioVolumes() const noexcept;
  // Advances only the retail audio/SPU clock. Mission-start UI uses this
  // after bootstrap so level music can play without advancing gameplay.
  [[nodiscard]] bool advanceAudioFrameClock() noexcept;
  [[nodiscard]] bool advanceAudioSliceClock() noexcept;
  [[nodiscard]] std::size_t
  takePcm(std::span<psx::SpuPcmFrame> destination) noexcept;
  void clearPcm() noexcept;
  [[nodiscard]] std::optional<LegacyAudioDiagnostics>
  audioDiagnostics() const noexcept;

private:
  friend class G4CampaignTransitionProbeAccess;

  // GameplaySession is clocked directly at the USA v1.1 retail cadence:
  // renderer_vblank_interval (0x80116484) is three, hence exactly 20 Hz.
  static constexpr std::uint32_t native_updates_per_guest_frame = 1U;
  static constexpr std::uint8_t intro_movie_request = 1U << 0U;
  static constexpr std::uint8_t ending_movie_request = 1U << 1U;
  static constexpr std::uint8_t failure_restart_request = 1U << 2U;

  struct RuntimeCheckpoint {
    LegacyGameplayVmSnapshot vm;
    LegacyHostPadState host_pad;
    std::uint16_t latched_pad_buttons{};
    std::uint64_t guest_frame{};
    std::uint32_t native_update_phase{};
    std::optional<std::uint32_t> last_checkpoint_frame;
    bool checkpoint_commit_pending{};
    std::uint8_t transition_requests{};
    std::uint8_t issued_transitions{};
    std::size_t next_intro_movie_index{};
    std::optional<std::size_t> pending_intro_movie_index;
    bool movie_loader_pending{};
    bool opening_finished{};
    bool finished{};
  };

  [[nodiscard]] bool detectCheckpointCommit() noexcept;
  [[nodiscard]] bool consumeTransitionRequest(std::uint8_t request) noexcept;
  void
  classifyTransitionRequest(std::uint32_t state_before,
                            std::uint32_t state_after,
                            const LegacyMissionBridgeState &mission) noexcept;
  [[nodiscard]] bool publishPresentationFrame() noexcept;
  [[nodiscard]] bool
  publishPresentationFrame(const LegacyGameplayBridgeState &renderer,
                           const LegacyMissionBridgeState &ui) noexcept;
  [[nodiscard]] bool republishPresentationFrame(
      const std::shared_ptr<const LegacyPresentationFrame> &source) noexcept;
  [[nodiscard]] bool applyRetailAudioVolumes() noexcept;
  [[nodiscard]] bool
  maintainRetailCheats(const LegacyMissionBridgeState &mission) noexcept;
  [[nodiscard]] std::vector<LegacyPresentationCommandType>
  pendingPresentationCommands() const;
  void markFault(LegacyRuntimeFaultReason reason =
                     LegacyRuntimeFaultReason::execution) noexcept;
  void recordExecutionFault(const LegacyRetailOuterFrameResult &frame) noexcept;

  std::shared_ptr<LegacyVirtualCd> virtual_cd_;
  std::unique_ptr<LegacyGameplayVm> vm_;
  std::optional<LegacyGameplayVmSnapshot> initial_snapshot_;
  std::optional<RuntimeCheckpoint> checkpoint_;
  std::shared_ptr<const LegacyPresentationFrame> presentation_frame_;
  LegacyHostPadState host_pad_state_;
  std::uint16_t latched_pad_buttons_{};
  std::uint64_t guest_frame_{};
  std::uint64_t presentation_sequence_{};
  std::uint32_t native_update_phase_{};
  std::uint8_t consecutive_renderer_snapshot_replays_{};
  std::optional<std::uint32_t> last_checkpoint_frame_;
  std::optional<LegacyRetailAudioVolumes> retail_audio_volumes_;
  std::optional<LegacyInventoryBridgeState> retail_infinite_ammo_;
  bool retail_weak_enemies_{};
  bool checkpoint_commit_pending_{};
  std::uint8_t transition_requests_{};
  std::uint8_t issued_transitions_{};
  std::size_t next_intro_movie_index_{};
  std::optional<std::size_t> pending_intro_movie_index_;
  bool movie_loader_pending_{};
  std::uint32_t mission_index_{};
  bool ready_{};
  bool faulted_{};
  LegacyRuntimeFaultReason fault_reason_{LegacyRuntimeFaultReason::none};
  std::string fault_detail_;
  bool opening_finished_{};
  bool finished_{true};
};

} // namespace sf::game
