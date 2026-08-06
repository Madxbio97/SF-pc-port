#include "psycross_mission_start.hpp"
#include "psycross_audio_output.hpp"
#include "psycross_retail_briefing.hpp"
#include "psycross_window_mode.hpp"

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

ControllerPromptFamily briefingControllerFamily(int family) noexcept {
  switch (family) {
  case PSYX_CONTROLLER_FAMILY_XBOX:
    return ControllerPromptFamily::xbox;
  case PSYX_CONTROLLER_FAMILY_PLAYSTATION:
    return ControllerPromptFamily::playstation;
  case PSYX_CONTROLLER_FAMILY_NINTENDO:
    return ControllerPromptFamily::nintendo;
  default:
    return ControllerPromptFamily::generic;
  }
}

InputPromptBindings
briefingControllerPrompts(const PsyXControllerSnapshot &snapshot) {
  constexpr std::uint16_t start_button = 0x0008U;
  constexpr std::uint16_t triangle_button = 0x1000U;
  constexpr std::uint16_t cross_button = 0x4000U;
  constexpr std::uint16_t square_button = 0x8000U;
  const auto family = briefingControllerFamily(snapshot.family);
  const auto name = [family](std::uint16_t button) {
    return controllerButtonPromptName(family, button);
  };
  return controllerInputPromptBindings(ControllerInputProtocol::unknown,
                                       InputPromptBindingNames{
                                           .confirm = name(cross_button),
                                           .cancel = name(triangle_button),
                                           .pause = name(start_button),
                                           .interact = name(triangle_button),
                                           .fire = name(square_button),
                                       });
}

void drawMissionStartFade(std::uint8_t intensity) {
  if (intensity == 0U) {
    return;
  }
  GR_SetBlendMode(BM_SUBTRACT);
  GR_EnableDepth(0);
  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, GetTPage(0, 2, 0, 0));
  DrawPrim(&page);
  TILE tile{};
  setTile(&tile);
  setSemiTrans(&tile, 1);
  setRGB0(&tile, intensity, intensity, intensity);
  setXY0(&tile, 0.0F, 0.0F);
  setWH(&tile, 384.0F, 240.0F);
  DrawPrim(&tile);
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(1);
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

std::uint16_t
PsyCrossMissionStart::run(const game::MissionPackage &mission, PADRAW &pad,
                          std::uint16_t previous_buttons,
                          const KeyboardMouseBindings &bindings,
                          std::optional<game::CampaignCarryState> carry,
                          bool initial_agent_difficulty) {
  // The retail briefing is also the level-loading boundary. Remove the STR
  // framebuffer and its texture-page residue before presenting it; the
  // scene viewer uploads a fresh mission working set after confirmation.
  RECT16 whole_vram{0, 0, 1024, 512};
  ClearImage(&whole_vram, 0, 0, 0);
  DrawSync(0);
  PsyCrossRetailBriefing retail_briefing{mission};
  preloaded_gameplay_.reset();
  preloaded_audio_ = std::make_unique<PsyCrossAudioOutput>();
  auto preload = std::async(std::launch::async, [&mission, carry,
                                                 initial_agent_difficulty] {
    auto gameplay = std::make_unique<game::GameplaySession>(
        mission, initial_agent_difficulty);
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
  auto audio_callback_tick = std::optional<std::uint64_t>{};
  auto audio_clock_started = false;
  auto fade_out_start = std::optional<std::uint64_t>{};
  constexpr std::uint32_t retail_audio_callback_hz = 120U;
  constexpr std::uint64_t maximum_audio_updates_per_iteration = 30U;
  std::array<psx::SpuPcmFrame, 4096U> briefing_pcm{};
  std::uint64_t audio_slices_completed{};
  std::uint64_t audio_pcm_frames_pumped{};
  std::uint64_t audio_pcm_blocks_pumped{};
  std::uint64_t audio_diagnostic_sequence{};
  auto active_prompt_bindings = keyboardMouseInputPromptBindings(bindings);
  auto previous_controller_buttons = std::uint16_t{0xffffU};
  auto previous_controller_instance = -1;
  KeyboardMouseActionSnapshot previous_bound_actions;
  const auto periodic_audio_diagnostics = psyCrossAudioDiagnosticsEnabled();
  auto next_audio_diagnostic_counter =
      SDL_GetPerformanceCounter() + performance_frequency;
  const auto pump_audio = [&] {
    while (const auto count = preloaded_gameplay_->takePcm(briefing_pcm)) {
      audio_pcm_frames_pumped += count;
      ++audio_pcm_blocks_pumped;
      preloaded_audio_->queue(
          std::span<const psx::SpuPcmFrame>{briefing_pcm}.first(count));
    }
    preloaded_audio_->flush();
    preloaded_audio_->update();
  };
  PsyX_Log_Info(
      "Mission briefing: retail transition and gameplay preload started\n");
  for (;;) {
    PsyX_UpdateInput();
    previous_buttons = readButtons(pad);
    const auto held = static_cast<std::uint16_t>(~previous_buttons);
    int keyboard_count{};
    const auto *keyboard = SDL_GetKeyboardState(&keyboard_count);
    const auto mouse_buttons = SDL_GetMouseState(nullptr, nullptr);
    const auto keyboard_state =
        keyboard != nullptr && keyboard_count > 0
            ? std::span<const std::uint8_t>{keyboard, static_cast<std::size_t>(
                                                          keyboard_count)}
            : std::span<const std::uint8_t>{};
    const auto bound_actions = sampleKeyboardMouseActions(
        bindings, KeyboardMouseDeviceState{
                      .keyboard = keyboard_state,
                      .mouse_left = (mouse_buttons & SDL_BUTTON_LMASK) != 0U,
                      .mouse_right = (mouse_buttons & SDL_BUTTON_RMASK) != 0U,
                      .mouse_middle = (mouse_buttons & SDL_BUTTON_MMASK) != 0U,
                      .mouse_x1 = (mouse_buttons & SDL_BUTTON_X1MASK) != 0U,
                      .mouse_x2 = (mouse_buttons & SDL_BUTTON_X2MASK) != 0U,
                      .mouse_wheel_delta = consumePsyCrossMouseWheel(),
                  });
    PsyXControllerSnapshot controller_snapshot{};
    static_cast<void>(PsyX_Pad_GetControllerSnapshot(0, &controller_snapshot));
    const auto controller_buttons = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(controller_snapshot.buttons[0]) |
        (static_cast<std::uint16_t>(controller_snapshot.buttons[1]) << 8U));
    const auto controller_identity_changed =
        controller_snapshot.connected != 0U &&
        controller_snapshot.instanceId != previous_controller_instance;
    const auto controller_pressed =
        controller_identity_changed
            ? std::uint16_t{}
            : static_cast<std::uint16_t>(previous_controller_buttons &
                                         ~controller_buttons);
    auto pc_action_edge = false;
    for (std::size_t index = 0U; index < bound_actions.held.size(); ++index) {
      pc_action_edge = pc_action_edge || (bound_actions.held[index] &&
                                          !previous_bound_actions.held[index]);
    }
    if (controller_snapshot.connected == 0U || pc_action_edge) {
      active_prompt_bindings = keyboardMouseInputPromptBindings(bindings);
    } else if (controller_pressed != 0U || controller_identity_changed ||
               active_prompt_bindings.device == InputPromptDevice::controller) {
      active_prompt_bindings = briefingControllerPrompts(controller_snapshot);
    }
    previous_bound_actions = bound_actions;
    previous_controller_buttons = controller_buttons;
    previous_controller_instance = controller_snapshot.connected != 0U
                                       ? controller_snapshot.instanceId
                                       : -1;

    if (!preloaded_gameplay_ && preload.wait_for(std::chrono::seconds{0}) ==
                                    std::future_status::ready) {
      preloaded_gameplay_ = preload.get();
      animation_start = SDL_GetPerformanceCounter();
      audio_clock_started = true;
      PsyX_Log_Info("Mission briefing: gameplay preload complete\n");
    }

    const auto current_counter = SDL_GetPerformanceCounter();
    const auto elapsed =
        animation_start ? current_counter - *animation_start : 0U;
    const auto retail_time =
        performance_frequency == 0U
            ? 0.0
            : static_cast<double>(elapsed) * 20.0 /
                  static_cast<double>(performance_frequency);
    if (audio_clock_started) {
      const auto audio_time =
          performance_frequency == 0U
              ? 0.0
              : static_cast<double>(elapsed) *
                    static_cast<double>(retail_audio_callback_hz) /
                    static_cast<double>(performance_frequency);
      const auto callback_tick = static_cast<std::uint64_t>(audio_time);
      if (!audio_callback_tick) {
        audio_callback_tick = callback_tick;
      }
      const auto pending_audio_updates =
          static_cast<std::size_t>(callback_tick - *audio_callback_tick);
      auto audio_updates = std::uint64_t{};
      while (*audio_callback_tick < callback_tick &&
             audio_updates < maximum_audio_updates_per_iteration) {
        if (!preloaded_gameplay_->advanceAudioSliceClock()) {
          throw core::Error{core::ErrorCode::invalid_format,
                            "Mission briefing audio clock failed"};
        }
        ++*audio_callback_tick;
        ++audio_slices_completed;
        ++audio_updates;
        pump_audio();
      }
      if (periodic_audio_diagnostics && performance_frequency != 0U &&
          current_counter >= next_audio_diagnostic_counter) {
        ++audio_diagnostic_sequence;
        preloaded_audio_->logDiagnostics("briefing-periodic");
        PsyX_Log_Info(
            "[AudioDiag][briefing-clock] sequence=%llu callback_tick=%llu "
            "pending=%zu slices=%llu pcm_frames=%llu pcm_blocks=%llu "
            "remaining=%llu\n",
            static_cast<unsigned long long>(audio_diagnostic_sequence),
            static_cast<unsigned long long>(*audio_callback_tick),
            pending_audio_updates,
            static_cast<unsigned long long>(audio_slices_completed),
            static_cast<unsigned long long>(audio_pcm_frames_pumped),
            static_cast<unsigned long long>(audio_pcm_blocks_pumped),
            static_cast<unsigned long long>(callback_tick -
                                            *audio_callback_tick));
        if (const auto guest = preloaded_gameplay_->audioDiagnostics()) {
          PsyX_Log_Info(
              "[AudioDiag][briefing-guest] sequence=%llu machine_tick=%llu "
              "audio_tick=%llu spu_sample=%llu mixed=%llu pcm_queued=%zu "
              "pcm_dropped=%llu cd_queued=%zu voices=%zu cd_read=%u "
              "cd_lba=%u xa_set=%u xa_file=%u xa_channel=%u\n",
              static_cast<unsigned long long>(audio_diagnostic_sequence),
              static_cast<unsigned long long>(guest->machine_tick),
              static_cast<unsigned long long>(guest->audio_frame_tick),
              static_cast<unsigned long long>(guest->spu_sample_clock),
              static_cast<unsigned long long>(guest->spu_mixed_frames),
              guest->spu_pcm_frames,
              static_cast<unsigned long long>(guest->spu_dropped_pcm_frames),
              guest->spu_cd_frames, guest->active_spu_voices,
              static_cast<unsigned int>(guest->cd_reading), guest->cd_lba,
              static_cast<unsigned int>(guest->xa_stream_set),
              static_cast<unsigned int>(guest->xa_file),
              static_cast<unsigned int>(guest->xa_channel));
        }
        next_audio_diagnostic_counter = current_counter + performance_frequency;
      }
    }
    const auto fade_out_elapsed =
        !fade_out_start ? 0.0
        : performance_frequency == 0U
            ? game::MissionStartGate::fade_out_seconds
            : static_cast<double>(current_counter - *fade_out_start) /
                  static_cast<double>(performance_frequency);
    const auto fade_out_intensity =
        game::MissionStartGate::fadeOutIntensity(fade_out_elapsed);
    auto text_animation_complete = false;
    if (PsyX_BeginScene() != 0) {
      text_animation_complete = retail_briefing.draw(
          mission.briefing(), retail_time, active_prompt_bindings);
      drawMissionStartFade(fade_out_intensity);
      PsyX_EndScene();
    }
    if (!fade_out_start &&
        gate.update((held & confirm_buttons) != 0U ||
                        bound_actions[KeyboardMouseAction::interact],
                    text_animation_complete)) {
      fade_out_start = current_counter;
    }
    if (fade_out_start && fade_out_intensity == 0xffU && preloaded_gameplay_) {
      PsyX_Log_Info("Mission briefing confirmed; entering gameplay\n");
      return previous_buttons;
    }
  }
}

} // namespace sf::platform::detail
