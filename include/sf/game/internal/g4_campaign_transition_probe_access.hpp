#pragma once

#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace sf::game {

// Internal ROM-gate seam. Shipping gameplay has no host-authored outcome
// API: this friend invokes the retail callbacks only so the all-mission probe
// can drive the same production transition and checkpoint consumers used by
// PsyCross.
class G4CampaignTransitionProbeAccess final {
public:
  [[nodiscard]] static bool
  invokeRetailFlashlight(LegacyFirstMissionRuntime &runtime, bool enabled) {
    if (!runtime.ready_ || runtime.faulted_ || runtime.finished_ ||
        runtime.vm_ == nullptr) {
      return false;
    }
    constexpr std::uint32_t flashlight_toggle_entry = 0x80024190U;
    constexpr std::uint64_t callback_budget = 5'000'000U;
    const std::array arguments{enabled ? 1U : 0U};
    return runtime.vm_->invoke(flashlight_toggle_entry, arguments,
                               callback_budget)
               .completed() &&
           runtime.publishPresentationFrame();
  }

  [[nodiscard]] static bool captureCheckpoint(GameplaySession &gameplay) {
    gameplay.checkpoint_pending_ = true;
    gameplay.captureCheckpoint();
    return gameplay.checkpoint_valid_;
  }

  [[nodiscard]] static bool invokeRetailFailure(GameplaySession &gameplay) {
    auto *vm = gameplayVm(gameplay);
    if (vm == nullptr) {
      return false;
    }
    const auto before = vm->readMissionBridgeState();
    if (!before || before->terminal || before->objective_count == 0U) {
      return false;
    }

    const auto fail_objective = before->parameter_count == 0U;
    constexpr auto no_sound = 0xffffffffU;
    const std::array failure_arguments{std::uint32_t{0U}, no_sound,
                                       fail_objective ? 1U : 0U};
    constexpr std::uint32_t mission_failure_entry = 0x80017970U;
    constexpr std::uint32_t mission_failure_transition_entry = 0x80017890U;
    constexpr std::uint64_t callback_budget = 5'000'000U;
    const auto failure =
        vm->invoke(mission_failure_entry, failure_arguments, callback_budget);
    const auto transition =
        failure.completed()
            ? vm->invoke(mission_failure_transition_entry, {}, callback_budget)
            : LegacyGameplayVmResult{};
    const auto after = vm->readMissionBridgeState();
    return failure.completed() && transition.completed() && after &&
           after->terminal && after->failure && !after->success &&
           after->failure_transition;
  }

  [[nodiscard]] static bool invokeRetailSuccess(GameplaySession &gameplay) {
    auto *vm = gameplayVm(gameplay);
    if (vm == nullptr) {
      return false;
    }
    constexpr std::uint32_t mission_success_entry = 0x80017698U;
    constexpr std::uint64_t callback_budget = 5'000'000U;
    constexpr std::array success_arguments{1U};
    const auto success =
        vm->invoke(mission_success_entry, success_arguments, callback_budget);
    const auto after = vm->readMissionBridgeState();
    return success.completed() && success.return_value == 1U && after &&
           after->terminal && after->success && !after->failure;
  }

  [[nodiscard]] static std::optional<std::uint32_t>
  applicationState(const GameplaySession &gameplay) noexcept {
    const auto *vm = gameplayVm(gameplay);
    if (vm == nullptr) {
      return std::nullopt;
    }
    std::uint32_t state{};
    constexpr auto profile = syphonFilterUsaV11RetailOuterFrameProfile();
    return vm->runtime().read32(profile.current_state, state)
               ? std::optional{state}
               : std::nullopt;
  }

  [[nodiscard]] static std::optional<LegacyMissionBridgeState>
  liveMissionState(const GameplaySession &gameplay) noexcept {
    const auto *vm = gameplayVm(gameplay);
    return vm != nullptr ? vm->readMissionBridgeState() : std::nullopt;
  }

  [[nodiscard]] static bool
  runtimeFinished(const GameplaySession &gameplay) noexcept {
    const auto *runtime = gameplay.legacy_first_mission_.get();
    return runtime != nullptr && runtime->finished_;
  }

private:
  [[nodiscard]] static LegacyGameplayVm *
  gameplayVm(GameplaySession &gameplay) noexcept {
    auto *runtime = gameplay.legacy_first_mission_.get();
    return runtime != nullptr && runtime->ready_ && !runtime->faulted_ &&
                   !runtime->finished_
               ? runtime->vm_.get()
               : nullptr;
  }

  [[nodiscard]] static const LegacyGameplayVm *
  gameplayVm(const GameplaySession &gameplay) noexcept {
    const auto *runtime = gameplay.legacy_first_mission_.get();
    return runtime != nullptr && runtime->ready_ && !runtime->faulted_
               ? runtime->vm_.get()
               : nullptr;
  }
};

} // namespace sf::game
