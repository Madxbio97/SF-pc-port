#include "psycross_mission_start.hpp"
#include "psycross_audio_output.hpp"
#include "psycross_retail_briefing.hpp"

#include "sf/core/error.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/mission_start.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <SDL.h>
#include <psx/libetc.h>
#include <psx/libgpu.h>
#include <psx/libpad.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace sf::platform::detail {
namespace {

constexpr std::uint16_t confirm_buttons = 0x4000U | 0x08U;

std::uint16_t readButtons(const PADRAW &pad) noexcept {
  return static_cast<std::uint16_t>(pad.buttons[0]) |
         (static_cast<std::uint16_t>(pad.buttons[1]) << 8U);
}

} // namespace

PsyCrossMissionStart::PsyCrossMissionStart() = default;
PsyCrossMissionStart::~PsyCrossMissionStart() = default;

std::unique_ptr<game::GameplaySession>
PsyCrossMissionStart::takePreloadedGameplay() noexcept {
  return std::move(preloaded_gameplay_);
}

std::unique_ptr<PsyCrossAudioOutput>
PsyCrossMissionStart::takePreloadedAudio() noexcept {
  return std::move(preloaded_audio_);
}

std::uint16_t PsyCrossMissionStart::run(const game::MissionPackage &mission,
                                        PADRAW &pad,
                                        std::uint16_t previous_buttons,
                                        std::optional<game::CampaignCarryState>
                                            carry) {
  // The retail briefing is also the level-loading boundary. Remove the STR
  // framebuffer and its texture-page residue before presenting it; the
  // scene viewer uploads a fresh mission working set after confirmation.
  RECT16 whole_vram{0, 0, 1024, 512};
  ClearImage(&whole_vram, 0, 0, 0);
  DrawSync(0);
  PsyCrossRetailBriefing retail_briefing{mission};
  preloaded_gameplay_.reset();
  preloaded_audio_ = std::make_unique<PsyCrossAudioOutput>();
  auto preload = std::async(std::launch::async, [&mission, carry] {
    auto gameplay = std::make_unique<game::GameplaySession>(mission);
    if (carry && !gameplay->applyCampaignCarryState(*carry)) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Campaign carry could not be applied to retail RAM"};
    }
    return gameplay;
  });
  game::MissionStartGate gate;
  static_cast<void>(gate.update(
      (static_cast<std::uint16_t>(~previous_buttons) & confirm_buttons) != 0U,
      false));
  const auto performance_frequency = SDL_GetPerformanceFrequency();
  auto animation_start = std::optional<std::uint64_t>{};
  auto audio_retail_tick = std::optional<std::uint32_t>{};
  auto audio_clock_started = false;
  std::array<psx::SpuPcmFrame, 4096U> briefing_pcm{};
  const auto pump_audio = [&] {
    while (const auto count = preloaded_gameplay_->takePcm(briefing_pcm)) {
      preloaded_audio_->queue(
          std::span<const psx::SpuPcmFrame>{briefing_pcm}.first(count));
    }
    preloaded_audio_->update();
  };
  PsyX_Log_Info(
      "Mission briefing: retail transition and gameplay preload started\n");
  for (;;) {
    PsyX_UpdateInput();
    previous_buttons = readButtons(pad);
    const auto held = static_cast<std::uint16_t>(~previous_buttons);

    if (!preloaded_gameplay_ && preload.wait_for(std::chrono::seconds{0}) ==
                                    std::future_status::ready) {
      preloaded_gameplay_ = preload.get();
      animation_start = SDL_GetPerformanceCounter();
      audio_clock_started = true;
      PsyX_Log_Info("Mission briefing: gameplay preload complete\n");
    }

    const auto elapsed =
        animation_start ? SDL_GetPerformanceCounter() - *animation_start : 0U;
    const auto retail_time =
        performance_frequency == 0U
            ? 0.0
            : static_cast<double>(elapsed) * 20.0 /
                  static_cast<double>(performance_frequency);
    const auto retail_tick = static_cast<std::uint32_t>(retail_time);
    if (audio_clock_started) {
      if (!audio_retail_tick) {
        audio_retail_tick = retail_tick;
      }
      while (*audio_retail_tick < retail_tick) {
        if (!preloaded_gameplay_->advanceAudioFrameClock()) {
          throw core::Error{core::ErrorCode::invalid_format,
                            "Mission briefing audio clock failed"};
        }
        ++*audio_retail_tick;
      }
      pump_audio();
    }
    auto text_animation_complete = false;
    if (PsyX_BeginScene() != 0) {
      text_animation_complete =
          retail_briefing.draw(mission.briefing(), retail_time);
      PsyX_EndScene();
    }
    if (gate.update((held & confirm_buttons) != 0U, text_animation_complete) &&
        preloaded_gameplay_) {
      PsyX_Log_Info("Mission briefing confirmed; entering gameplay\n");
      return previous_buttons;
    }
  }
}

} // namespace sf::platform::detail
