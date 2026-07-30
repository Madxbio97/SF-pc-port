#include "sf/platform/host.hpp"

#include "psycross_audio_output.hpp"
#include "psycross_mission_start.hpp"
#include "psycross_movie_player.hpp"
#include "psycross_scene_viewer.hpp"
#include "psycross_video_mode.hpp"
#include "psycross_window_mode.hpp"

#include "sf/core/error.hpp"
#include "sf/game/campaign.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/retail_cheats.hpp"
#include "sf/game/title.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <SDL.h>
#include <psx/libetc.h>
#include <psx/libgpu.h>
#include <psx/libgte.h>
#include <psx/libpad.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace sf::platform {
namespace {

detail::StandaloneMovieSkipPolicy
endingMovieSkipPolicy(const game::MissionDefinition &definition) noexcept {
  const auto catalog = game::missionCatalog();
  if (!catalog.empty() && definition.index == catalog.back().index) {
    // EOL/SILO.STR contains the credits and the post-credits scene.  It is a
    // single retail stream, so allowing a carried confirm/cancel edge from the
    // save menu to skip it loses the entire campaign ending.
    return detail::StandaloneMovieSkipPolicy::prevent;
  }
  return detail::StandaloneMovieSkipPolicy::allow;
}

void configureGraphics(const GraphicsSettings &settings) noexcept {
  g_cfg_msaaSamples = settings.msaa_samples;
  g_cfg_bilinearFiltering = settings.bilinear_filtering ? 1 : 0;
  g_cfg_anisotropicFiltering = settings.anisotropic_filtering ? 1 : 0;
  g_cfg_aspectMode = settings.aspect_ratio == AspectRatioMode::adaptive
                         ? PSYX_ASPECT_ADAPTIVE
                         : PSYX_ASPECT_ORIGINAL_4_3;
  g_cfg_swapInterval = settings.vsync ? 1 : 0;
  // Presentation is native: no game code samples the displayed framebuffer
  // through PSX VRAM, and the guest simulation has its own deterministic
  // 20 Hz clock. Avoid the legacy readback and busy VBlank compatibility
  // paths, both of which otherwise steal time from display-refresh pacing.
  g_cfg_framebufferFeedback = 0;
  g_cfg_vblankThread = 0;
}

void configurePresentation(const GraphicsSettings &settings) noexcept {
  // SDL's high-resolution timer and GL context both exist only after
  // PsyX_Initialise. Apply the two independent presentation controls here:
  // swap interval removes tearing, while the software cap controls cadence.
  PsyX_EnableSwapInterval(settings.vsync ? 1 : 0);
  PsyX_SetSwapInterval(1);
  PsyX_SetFrameLimit(static_cast<int>(settings.frame_limit));
}

void configureInput() {
  g_cfg_keyboardMapping.kc_dpad_up =
      PsyX_LookupKeyboardMapping("W", g_cfg_keyboardMapping.kc_dpad_up);
  g_cfg_keyboardMapping.kc_dpad_down =
      PsyX_LookupKeyboardMapping("S", g_cfg_keyboardMapping.kc_dpad_down);
  g_cfg_keyboardMapping.kc_dpad_left =
      PsyX_LookupKeyboardMapping("A", g_cfg_keyboardMapping.kc_dpad_left);
  g_cfg_keyboardMapping.kc_dpad_right =
      PsyX_LookupKeyboardMapping("D", g_cfg_keyboardMapping.kc_dpad_right);
  // PC actions are sampled directly by the player-input adapter. Keep them
  // out of PsyCross's merged virtual pad so Left Shift cannot turn a physical
  // R1 target-lock into the PC run action and Space cannot become Select.
  g_cfg_keyboardMapping.kc_r1 =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_r1);
  g_cfg_keyboardMapping.kc_l1 =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_l1);
  g_cfg_keyboardMapping.kc_select =
      PsyX_LookupKeyboardMapping("NONE", g_cfg_keyboardMapping.kc_select);
}

class PsyCrossHost final : public Host {
public:
  PsyCrossHost(std::string title, GraphicsSettings graphics)
      : title_(title.begin(), title.end()), graphics_(graphics) {
    title_.push_back('\0');
  }

  void run() override {
    configureGraphics(graphics_);
    PsyX_Initialise(title_.data(), graphics_.width, graphics_.height, 0);
    configurePresentation(graphics_);
    [[maybe_unused]] detail::PsyCrossWindowMode window_mode{
        graphics_.fullscreen};
    configureInput();
    detail::configurePsyCrossVideoMode(detail::gameplay_video_mode, true);
    for (;;) {
      if (PsyX_BeginScene() != 0) {
        PsyX_EndScene();
      }
    }
  }

private:
  std::vector<char> title_;
  GraphicsSettings graphics_;
};

void uploadTitleNoticeFont() {
  // Keep PsyCross's debug font clear of the title TIMs at x=896..950 and the
  // movie upload rectangle at x=0..319, y=256..495.
  FntLoad(960, 256);
}

void configureTitleNotice() {
  uploadTitleNoticeFont();
  static_cast<void>(FntOpen(54, 70, 252, 154, 2, 256));
}

game::TitleSaveSlots
loadTitleSaveSlots(const std::filesystem::path &path) noexcept {
  const auto loaded = game::loadTitleSaveSlotsFile(path);
  if (loaded.status == game::TitleSaveLoadStatus::invalid) {
    PsyX_Log_Error("Ignoring invalid or unreadable title save file\n");
  } else if (loaded.status == game::TitleSaveLoadStatus::recovered) {
    PsyX_Log_Info("Recovered title save from the last complete backup\n");
  }
  return loaded.slots;
}

bool storeTitleSaveSlots(const std::filesystem::path &path,
                         const game::TitleSaveSlots &slots) noexcept {
  const auto stored = game::storeTitleSaveSlotsFile(path, slots);
  if (!stored) {
    PsyX_Log_Error("Campaign progress could not be persisted\n");
  }
  return stored;
}

enum class SaveStoreDecision {
  stored,
  continue_without_saving,
  return_to_title,
};

std::uint16_t readHostButtons(const PADRAW &pad) noexcept {
  return static_cast<std::uint16_t>(pad.buttons[0]) |
         (static_cast<std::uint16_t>(pad.buttons[1]) << 8U);
}

KeyboardMouseActionSnapshot
sampleHostKeyboardMouseActions(const KeyboardMouseBindings &bindings) {
  int keyboard_count{};
  const auto *keyboard = SDL_GetKeyboardState(&keyboard_count);
  const auto keyboard_state =
      keyboard != nullptr && keyboard_count > 0
          ? std::span<const std::uint8_t>{keyboard, static_cast<std::size_t>(
                                                        keyboard_count)}
          : std::span<const std::uint8_t>{};
  const auto mouse_buttons = SDL_GetMouseState(nullptr, nullptr);
  return sampleKeyboardMouseActions(
      bindings, KeyboardMouseDeviceState{
                    .keyboard = keyboard_state,
                    .mouse_left = (mouse_buttons & SDL_BUTTON_LMASK) != 0U,
                    .mouse_right = (mouse_buttons & SDL_BUTTON_RMASK) != 0U,
                    .mouse_middle = (mouse_buttons & SDL_BUTTON_MMASK) != 0U,
                    .mouse_x1 = (mouse_buttons & SDL_BUTTON_X1MASK) != 0U,
                    .mouse_x2 = (mouse_buttons & SDL_BUTTON_X2MASK) != 0U,
                    .mouse_wheel_delta = detail::consumePsyCrossMouseWheel(),
                });
}

SaveStoreDecision
storeTitleSaveSlotsWithRecovery(const std::filesystem::path &path,
                                const game::TitleSaveSlots &slots, PADRAW &pad,
                                std::uint16_t &previous_buttons,
                                const KeyboardMouseBindings &bindings) {
  if (storeTitleSaveSlots(path, slots)) {
    return SaveStoreDecision::stored;
  }

  configureTitleNotice();
  constexpr std::uint16_t retry_buttons = 0x4000U | 0x08U;
  constexpr std::uint16_t continue_buttons = 0x8000U;
  constexpr std::uint16_t title_buttons = 0x2000U | 0x01U;
  const auto input_name = [&](KeyboardMouseAction action) {
    return std::string{keyboardMouseInputName(bindings[action])};
  };
  const auto notice =
      "       SAVE FAILED\n\n  " + input_name(KeyboardMouseAction::interact) +
      "  Retry\n  " + input_name(KeyboardMouseAction::fire) +
      "  Continue without saving\n  " + input_name(KeyboardMouseAction::pause) +
      "  Return to title\n\n"
      "Campaign progress remains active.";
  auto keyboard_initialized = false;
  auto interact_was_down = false;
  auto fire_was_down = false;
  auto pause_was_down = false;
  for (;;) {
    PsyX_UpdateInput();
    const auto buttons = readHostButtons(pad);
    const auto pressed =
        static_cast<std::uint16_t>(~buttons & previous_buttons);
    previous_buttons = buttons;
    const auto actions = sampleHostKeyboardMouseActions(bindings);
    const auto interact_down = actions[KeyboardMouseAction::interact];
    const auto fire_down = actions[KeyboardMouseAction::fire];
    const auto pause_down = actions[KeyboardMouseAction::pause];
    const auto interact_pressed =
        keyboard_initialized && interact_down && !interact_was_down;
    const auto fire_pressed =
        keyboard_initialized && fire_down && !fire_was_down;
    const auto pause_pressed =
        keyboard_initialized && pause_down && !pause_was_down;
    keyboard_initialized = true;
    interact_was_down = interact_down;
    fire_was_down = fire_down;
    pause_was_down = pause_down;

    if ((pressed & title_buttons) != 0U || pause_pressed) {
      return SaveStoreDecision::return_to_title;
    }
    if ((pressed & continue_buttons) != 0U || fire_pressed) {
      PsyX_Log_Info("Continuing campaign without durable save progress\n");
      return SaveStoreDecision::continue_without_saving;
    }
    if (((pressed & retry_buttons) != 0U || interact_pressed) &&
        storeTitleSaveSlots(path, slots)) {
      return SaveStoreDecision::stored;
    }

    if (PsyX_BeginScene() != 0) {
      char format[] = "%s";
      static_cast<void>(FntPrint(format, notice.c_str()));
      static_cast<void>(FntFlush());
      PsyX_EndScene();
    }
  }
}

int titleAnalogDirection(const PADRAW &pad) noexcept;

game::CampaignSaveResult runCampaignSaveMenu(
    const game::MissionPackage &mission, const game::TitleSaveSlots &slots,
    PADRAW &pad, std::uint16_t &previous_buttons,
    detail::PsyCrossUiAudio &ui_audio, const KeyboardMouseBindings &bindings) {
  // Gameplay already owns the correct 384x240 presentation target. Reuse its
  // original font/ACD renderer instead of switching to the debug-font movie
  // target whose VRAM page has been overwritten by the mission renderer.
  detail::PsyCrossCampaignSaveRenderer renderer{mission, bindings};
  PsyX_Log_Info("Campaign save UI entered\n");
  game::CampaignSaveMenu menu;
  auto analog_direction = 0;
  auto first_frame_presented = false;
  constexpr std::uint16_t previous_buttons_mask = 0x80U | 0x10U;
  constexpr std::uint16_t next_buttons_mask = 0x20U | 0x40U;
  constexpr std::uint16_t confirm_buttons_mask = 0x4000U | 0x8000U | 0x08U;
  constexpr std::uint16_t cancel_buttons_mask = 0x2000U | 0x01U;
  auto keyboard_initialized = false;
  auto interact_was_down = false;
  auto pause_was_down = false;
  for (;;) {
    PsyX_UpdateInput();
    ui_audio.update();
    const auto buttons = readHostButtons(pad);
    const auto pressed =
        static_cast<std::uint16_t>(~buttons & previous_buttons);
    previous_buttons = buttons;
    const auto actions = sampleHostKeyboardMouseActions(bindings);
    const auto interact_down = actions[KeyboardMouseAction::interact];
    const auto pause_down = actions[KeyboardMouseAction::pause];
    const auto interact_pressed =
        keyboard_initialized && interact_down && !interact_was_down;
    const auto pause_pressed =
        keyboard_initialized && pause_down && !pause_was_down;
    keyboard_initialized = true;
    interact_was_down = interact_down;
    pause_was_down = pause_down;
    const auto current_analog = titleAnalogDirection(pad);
    const auto analog_previous = current_analog < 0 && analog_direction == 0;
    const auto analog_next = current_analog > 0 && analog_direction == 0;
    analog_direction = current_analog;
    const game::CampaignSaveInput input{
        (pressed & previous_buttons_mask) != 0U || analog_previous,
        (pressed & next_buttons_mask) != 0U || analog_next,
        (pressed & confirm_buttons_mask) != 0U || interact_pressed,
        (pressed & cancel_buttons_mask) != 0U || pause_pressed,
    };
    const auto previous_phase = menu.phase();
    const auto previous_save_selection = menu.saveSelected();
    const auto previous_overwrite_selection = menu.overwriteSelected();
    const auto previous_slot = menu.slotSelection();
    const auto result = menu.update(input, slots);
    if (input.cancel) {
      ui_audio.play(detail::PsyCrossUiCue::cancel);
    } else if (input.confirm) {
      ui_audio.play(detail::PsyCrossUiCue::confirm);
    } else if (menu.phase() != previous_phase ||
               menu.saveSelected() != previous_save_selection ||
               menu.overwriteSelected() != previous_overwrite_selection ||
               menu.slotSelection() != previous_slot) {
      ui_audio.play(detail::PsyCrossUiCue::navigate);
    }
    if (result.decision != game::CampaignSaveDecision::none) {
      return result;
    }

    // Draw into either a fresh scene or the still-open implicit scene left by
    // the terminal guest packet. Ending that scene before covering it used to
    // swap the old, un-faded gameplay backbuffer to the window for one frame.
    // The ACD renderer starts with an opaque full-screen black tile, so it is
    // also the correct recovery path for an inherited scene.
    static_cast<void>(PsyX_BeginScene());
    renderer.draw(menu, slots);
    PsyX_EndScene();
    if (!first_frame_presented) {
      first_frame_presented = true;
      PsyX_Log_Info("Campaign save UI first frame presented\n");
    }
  }
}

int titleAnalogDirection(const PADRAW &pad) noexcept {
  constexpr int analog_deadzone = 24;
  const auto analog_x = static_cast<int>(pad.analog[2]) - 128;
  const auto analog_y = static_cast<int>(pad.analog[3]) - 128;
  if (analog_y < -analog_deadzone || analog_x < -analog_deadzone) {
    return -1;
  }
  if (analog_y > analog_deadzone || analog_x > analog_deadzone) {
    return 1;
  }
  return 0;
}

std::vector<u_long> packWords(std::span<const std::uint16_t> words) {
  std::vector<u_long> packed((words.size() + 1U) / 2U);
  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto shift = static_cast<unsigned int>((index & 1U) * 16U);
    packed[index / 2U] |= static_cast<u_long>(words[index]) << shift;
  }
  return packed;
}

RECT16 blockRect(const assets::TimBlock &block) {
  const auto checked = [](std::uint16_t value) {
    if (value > static_cast<std::uint16_t>(std::numeric_limits<short>::max())) {
      throw core::Error{core::ErrorCode::unsupported,
                        "TIM VRAM coordinate exceeds PsyCross range"};
    }
    return static_cast<short>(value);
  };
  return RECT16{
      checked(block.x),
      checked(block.y),
      checked(block.width_words),
      checked(block.height),
  };
}

void uploadBlock(const assets::TimBlock &block) {
  auto rect = blockRect(block);
  auto packed = packWords(block.words);
  LoadImage(&rect, packed.data());
}

int texturePageMode(assets::TimPixelMode mode) {
  switch (mode) {
  case assets::TimPixelMode::indexed4:
    return 0;
  case assets::TimPixelMode::indexed8:
    return 1;
  case assets::TimPixelMode::direct16:
    return 2;
  case assets::TimPixelMode::direct24:
    return 3;
  }
  return 2;
}

void uploadTitleAssets(const game::TitleAssets &assets) {
  for (const auto &sprite : assets.sprites()) {
    if (sprite.image.clut()) {
      uploadBlock(*sprite.image.clut());
    }
    uploadBlock(sprite.image.pixels());
  }
  DrawSync(0);
}

void drawTitleSprite(const game::TitleSprite &source, std::uint8_t brightness) {
  if (source.image.mode() != assets::TimPixelMode::indexed8 ||
      !source.image.clut()) {
    throw core::Error{core::ErrorCode::unsupported,
                      "Title menu sprites must use an 8-bit indexed TIM"};
  }

  const auto &pixels = source.image.pixels();
  const auto &clut = *source.image.clut();
  constexpr unsigned int texture_page_word_width = 64U;
  constexpr unsigned int texture_page_height = 256U;
  constexpr unsigned int indexed8_pixels_per_word = 2U;
  const auto u = static_cast<unsigned int>(pixels.x % texture_page_word_width) *
                 indexed8_pixels_per_word;
  const auto v = static_cast<unsigned int>(pixels.y % texture_page_height);
  const auto width = static_cast<unsigned int>(source.image.displayWidth());
  const auto height = static_cast<unsigned int>(source.image.displayHeight());
  if (u + width > 256U || v + height > 256U) {
    throw core::Error{core::ErrorCode::unsupported,
                      "Title menu TIM crosses its texture-page boundary"};
  }

  DR_TPAGE page{};
  // TITLE.OVL assigns ABR 1 (background + foreground) to all four sprites.
  // Title sprites are part of the visible 320x240 movie frame. Keep DFE
  // enabled so PsyCross composes them into the active display target, not its
  // VRAM-copy offscreen path.
  SetDrawTPage(&page, 1, 1, GetTPage(1, 1, pixels.x, pixels.y));
  DrawPrim(&page);

  // TITLE.OVL authored these positions for the regular 384x240 display, but
  // MOVIE.OVL presents TITLE.STR in a 320x240 target. A raw SPRT kept the
  // 384-wide coordinates and consequently pushed VIDEO.TIM through the right
  // edge of the 4:3 movie. Reproject the horizontal layout into the active
  // movie canvas; FT4 keeps the complete source image while independently
  // scaling its destination width.
  constexpr auto title_layout_width = 384U;
  constexpr auto title_movie_width = 320U;
  const auto scale_title_x = [](unsigned int value) {
    return (value * title_movie_width + title_layout_width / 2U) /
           title_layout_width;
  };
  const auto destination_x = scale_title_x(
      static_cast<unsigned int>(std::max<std::int16_t>(source.x, 0)));
  const auto destination_width = scale_title_x(width);
  const auto destination_y =
      static_cast<unsigned int>(std::max<std::int16_t>(source.y, 0));

  POLY_FT4 sprite{};
  setPolyFT4(&sprite);
  setSemiTrans(&sprite, 1);
  setRGB0(&sprite, brightness, brightness, brightness);
  sprite.tpage = GetTPage(1, 1, pixels.x, pixels.y);
  sprite.clut = GetClut(clut.x, clut.y);
  setXY4(&sprite, static_cast<float>(destination_x),
         static_cast<float>(destination_y),
         static_cast<float>(destination_x + destination_width),
         static_cast<float>(destination_y), static_cast<float>(destination_x),
         static_cast<float>(destination_y + height),
         static_cast<float>(destination_x + destination_width),
         static_cast<float>(destination_y + height));
  setUV4(&sprite, static_cast<u_char>(u), static_cast<u_char>(v),
         static_cast<u_char>(u + width), static_cast<u_char>(v),
         static_cast<u_char>(u), static_cast<u_char>(v + height),
         static_cast<u_char>(u + width), static_cast<u_char>(v + height));
  DrawPrim(&sprite);
}

class PsyCrossTitleHost final : public Host {
public:
  PsyCrossTitleHost(std::string title, game::TitleAssets assets,
                    game::TitleMovies movies,
                    game::MissionPackage initial_mission,
                    std::filesystem::path cue_path,
                    std::string supported_game_serial,
                    GraphicsSettings graphics, KeyboardMouseBindings input,
                    game::RetailCheatState cheats)
      : title_(title.begin(), title.end()), assets_(std::move(assets)),
        movies_(std::move(movies)),
        initial_mission_(std::move(initial_mission)),
        cue_path_(std::move(cue_path)),
        supported_game_serial_(std::move(supported_game_serial)),
        graphics_(graphics), input_(input), cheats_(cheats) {
    title_.push_back('\0');
  }

  void run() override {
    configureGraphics(graphics_);
    PsyX_Initialise(title_.data(), graphics_.width, graphics_.height, 0);
    configurePresentation(graphics_);
    [[maybe_unused]] detail::PsyCrossWindowMode window_mode{
        graphics_.fullscreen};
    configureInput();
    detail::configurePsyCrossVideoMode(detail::gameplay_video_mode, true);

    uploadTitleAssets(assets_);
    configureTitleNotice();
    const auto save_location =
        game::defaultTitleSaveLocation(cue_path_, supported_game_serial_);
    const auto migration = game::migrateLegacyTitleSaveSlotsFile(save_location);
    if (migration == game::TitleSaveMigrationStatus::migrated) {
      PsyX_Log_Info("Migrated campaign save to the user-data directory\n");
    } else if (migration == game::TitleSaveMigrationStatus::failed) {
      PsyX_Log_Error("Legacy campaign save migration failed\n");
    }
    const auto &save_path = save_location.primary;
    menu_.setSaveSlots(loadTitleSaveSlots(save_path));
    PADRAW pad{};
    PadInitDirect(reinterpret_cast<unsigned char *>(&pad), nullptr);
    PadStartCom();

    std::uint16_t previous_buttons = 0xffffU;
    bool title_cheat_latched{};
    bool title_keyboard_initialized{};
    bool title_interact_was_down{};
    bool title_pause_was_down{};
    detail::PsyCrossUiAudio ui_audio{cue_path_};
    detail::PsyCrossMoviePlayer movie_player;
    detail::PsyCrossCampaignSaveRenderer title_load_renderer{initial_mission_,
                                                             input_};
    const detail::MovieOverlayCallbacks overlay{
        [this, &pad, &ui_audio, &title_cheat_latched,
         &title_keyboard_initialized, &title_interact_was_down,
         &title_pause_was_down](std::uint16_t pressed,
                                std::uint32_t movie_frame) {
          ui_audio.update();
          const auto actions = sampleHostKeyboardMouseActions(input_);
          const auto interact_down = actions[KeyboardMouseAction::interact];
          const auto pause_down = actions[KeyboardMouseAction::pause];
          const auto interact_pressed = title_keyboard_initialized &&
                                        interact_down &&
                                        !title_interact_was_down;
          const auto pause_pressed =
              title_keyboard_initialized && pause_down && !title_pause_was_down;
          title_keyboard_initialized = true;
          title_interact_was_down = interact_down;
          title_pause_was_down = pause_down;
          const auto held = static_cast<std::uint16_t>(~readHostButtons(pad));
          const auto title_cheat = game::detectRetailTitleCheat(
              held, menu_.phase() == game::TitlePhase::menu &&
                        menu_.selection() == 0U);
          if (!title_cheat) {
            title_cheat_latched = false;
          } else if (!title_cheat_latched &&
                     *title_cheat == game::RetailCheat::hard_mode) {
            cheats_.hard_mode = true;
            title_cheat_latched = true;
            PsyX_Log_Info("Retail cheat %s: activated\n",
                          game::retailCheatName(*title_cheat));
            ui_audio.play(detail::PsyCrossUiCue::confirm);
          }
          const auto analog_direction = titleAnalogDirection(pad);
          const auto analog_previous =
              analog_direction < 0 && title_analog_direction_ == 0;
          const auto analog_next =
              analog_direction > 0 && title_analog_direction_ == 0;
          title_analog_direction_ = analog_direction;
          const game::TitleInput input{
              .previous = (pressed & (0x80U | 0x10U)) != 0 || analog_previous,
              .next = (pressed & (0x20U | 0x40U)) != 0 || analog_next,
              .confirm = (pressed & (0x4000U | 0x8000U | 0x08U)) != 0 ||
                         interact_pressed,
              .cancel = (pressed & (0x2000U | 0x01U)) != 0 || pause_pressed,
              .confirm_down = ((~readHostButtons(pad)) &
                               (0x4000U | 0x8000U | 0x08U)) != 0U ||
                              interact_down,
          };
          const auto previous_selection = menu_.selection();
          const auto previous_phase = menu_.phase();
          const auto previous_slot = menu_.loadSlotSelection();
          const auto command = menu_.update(input, movie_frame);
          if (previous_phase == game::TitlePhase::load_slots &&
              menu_.phase() != game::TitlePhase::load_slots) {
            // The retail font atlas shares VRAM pages with TITLE.HOG. Restore
            // the title sprites before returning from the slot picker.
            uploadTitleAssets(assets_);
          }
          if (input.cancel) {
            ui_audio.play(detail::PsyCrossUiCue::cancel);
          } else if (input.confirm && (command != game::TitleCommand::none ||
                                       menu_.phase() != previous_phase)) {
            ui_audio.play(detail::PsyCrossUiCue::confirm);
          } else if (menu_.selection() != previous_selection ||
                     menu_.loadSlotSelection() != previous_slot) {
            ui_audio.play(detail::PsyCrossUiCue::navigate);
          }
          if (command == game::TitleCommand::exit ||
              command == game::TitleCommand::new_game ||
              command == game::TitleCommand::load_game ||
              command == game::TitleCommand::training_video) {
            selected_command_ = command;
            PsyX_Log_Info("Title command accepted: %s\n",
                          game::titleCommandName(command).data());
            return false;
          }
          return true;
        },
        [this, &title_load_renderer] {
          if (menu_.phase() == game::TitlePhase::load_slots) {
            title_load_renderer.drawLoadSlots(menu_.saveSlots(),
                                              menu_.loadSlotSelection());
          } else {
            for (std::size_t index = 0; index < game::TitleMenu::visual_count;
                 ++index) {
              const auto visual = static_cast<game::TitleVisual>(index);
              const auto brightness = menu_.brightness(visual);
              if (brightness != 0) {
                drawTitleSprite(assets_.sprite(visual), brightness);
              }
            }
          }
        },
        [this]() -> const game::DiscMovie * {
          if (selected_command_ == game::TitleCommand::new_game &&
              !initial_mission_.openingMovie().path.empty()) {
            return &initial_mission_.openingMovie();
          }
          if (selected_command_ == game::TitleCommand::training_video) {
            return &movies_.trainingMovie();
          }
          return nullptr;
        },
    };
    auto play_startup_movies = true;
    for (;;) {
      selected_command_ = game::TitleCommand::none;
      title_analog_direction_ =
          play_startup_movies ? 0 : titleAnalogDirection(pad);
      previous_buttons = movie_player.play(movies_, pad, previous_buttons,
                                           overlay, play_startup_movies);
      if (selected_command_ == game::TitleCommand::training_video) {
        menu_.completeSearch();
        play_startup_movies = false;
        continue;
      }
      play_startup_movies = false;

      if (selected_command_ != game::TitleCommand::new_game &&
          selected_command_ != game::TitleCommand::load_game) {
        break;
      }
      auto save_slots = menu_.saveSlots();
      const auto title_played_opening_movie =
          selected_command_ == game::TitleCommand::new_game &&
          !initial_mission_.openingMovie().path.empty();
      std::optional<game::CampaignProgress> campaign;
      auto campaign_carry = std::optional<game::CampaignCarryState>{};
      if (selected_command_ == game::TitleCommand::load_game) {
        campaign = game::CampaignProgress::resume(save_slots,
                                                  menu_.loadSlotSelection());
        if (!campaign) {
          PsyX_Log_Error("Load Game rejected invalid selected slot\n");
          uploadTitleAssets(assets_);
          configureTitleNotice();
          menu_.setSaveSlots(loadTitleSaveSlots(save_path));
          continue;
        }
        PsyX_Log_Info("Load Game: slot=%zu mission=%u\n",
                      *campaign->saveSlot() + 1U, campaign->missionIndex());
        campaign_carry = save_slots[*campaign->saveSlot()].carry;
      } else {
        campaign = game::CampaignProgress::startUnsaved(
            initial_mission_.definition().index, title_played_opening_movie);
        if (!campaign) {
          PsyX_Log_Error("New Game rejected an invalid campaign cursor\n");
          uploadTitleAssets(assets_);
          configureTitleNotice();
          menu_.setSaveSlots(loadTitleSaveSlots(save_path));
          continue;
        }
        PsyX_Log_Info("New Game FMV complete; opening unsaved mission %u\n",
                      campaign->missionIndex() + 1U);
      }
      detail::PsyCrossMissionStart mission_start;
      detail::PsyCrossSceneViewer scene_viewer{input_, cheats_};
      std::optional<game::MissionPackage> loaded_mission;
      auto exit_application = false;
      while (campaign->active()) {
        const auto mission_index = campaign->missionIndex();
        game::MissionPackage *mission{};
        if (mission_index == initial_mission_.definition().index) {
          mission = &initial_mission_;
        } else {
          auto disc = game::GameDisc::open(cue_path_);
          loaded_mission.emplace(
              game::MissionPackage::load(disc, mission_index));
          mission = &*loaded_mission;
        }
        if (campaign->pendingEndingMovieMission()) {
          PsyX_Log_Info("Recovering interrupted EOL for mission %u\n",
                        mission_index + 1U);
          if (!mission->endingMovie().path.empty()) {
            previous_buttons = movie_player.playStandalone(
                mission->endingMovie(), pad, previous_buttons,
                endingMovieSkipPolicy(mission->definition()));
          }
          auto candidate_campaign = *campaign;
          auto candidate_slots = save_slots;
          const auto advance =
              candidate_campaign.completeMission(candidate_slots);
          if (advance == game::CampaignAdvance::invalid) {
            PsyX_Log_Error("Pending EOL transition is inconsistent\n");
            break;
          }
          const auto store_decision = storeTitleSaveSlotsWithRecovery(
              save_path, candidate_slots, pad, previous_buttons, input_);
          if (store_decision == SaveStoreDecision::return_to_title) {
            break;
          }
          *campaign = candidate_campaign;
          save_slots = candidate_slots;
          campaign_carry = campaign->saveSlot()
                               ? save_slots[*campaign->saveSlot()].carry
                               : std::nullopt;
          if (advance == game::CampaignAdvance::campaign_complete) {
            PsyX_Log_Info("Campaign complete\n");
            break;
          }
          continue;
        }
        if (campaign->openingMovieRequired(mission->definition())) {
          previous_buttons = movie_player.playStandalone(
              mission->openingMovie(), pad, previous_buttons);
        }
        campaign->markOpeningMovieHandled();
        // Every campaign entry owns a mission-start loading boundary. DLFs
        // with authored directive text use it verbatim; continuation maps use
        // their catalog title instead of silently skipping the briefing UI.
        previous_buttons = mission_start.run(*mission, pad, previous_buttons,
                                             input_, campaign_carry);

        const auto &definition = mission->definition();
        std::cout << "Starting mission " << (definition.index + 1U) << ": "
                  << definition.title << " [" << definition.resource_name
                  << "]\nMounted " << mission->archive().entries().size()
                  << " FOG files, " << mission->textureFileCount()
                  << " textures and " << mission->worldModelCount()
                  << " world models\n";
        const auto scene_result =
            scene_viewer.run(*mission, pad, previous_buttons, cue_path_,
                             campaign->maximumUnlockedMission(),
                             mission_start.takePreloadedGameplay(),
                             mission_start.takePreloadedAudio());
        previous_buttons = scene_result.previous_buttons;
        if (scene_result.reason == detail::SceneExitReason::mission_selected &&
            scene_result.selected_mission) {
          const auto selected = *scene_result.selected_mission;
          if (!campaign->selectUnlockedMission(selected)) {
            // Reaching this branch requires the explicit all-missions cheat;
            // normal mission selection can never move beyond the high-water
            // mark of the loaded save.
            auto replacement =
                game::CampaignProgress::startUnsaved(selected, false);
            if (!replacement) {
              PsyX_Log_Error("Pause mission selection rejected mission %u\n",
                             selected + 1U);
              break;
            }
            campaign = std::move(replacement);
          }
          campaign_carry.reset();
          loaded_mission.reset();
          PsyX_Log_Info("Pause mission selection: mission=%u\n", selected + 1U);
          continue;
        }
        if (scene_result.reason == detail::SceneExitReason::mission_complete) {
          const auto next_mission = mission_index + 1U;
          const auto carry_for_next =
              game::campaignMissionsShareCarry(mission_index, next_mission)
                  ? scene_result.carry
                  : std::nullopt;
          if (game::campaignMissionsShareCarry(mission_index, next_mission) &&
              !carry_for_next) {
            // A terminal overlay can retire its live inventory table before
            // the native host observes the EOL request. The scene viewer
            // normally supplies its last coherent snapshot; if even that is
            // unavailable, continuing with mission defaults is safer than
            // tearing down the entire campaign and returning to title.
            PsyX_Log_Warning(
                "Campaign transition has no coherent player carry; "
                "continuing with mission defaults\n");
          }
          const auto save_result = runCampaignSaveMenu(
              *mission, save_slots, pad, previous_buttons, ui_audio, input_);
          auto completion_is_saved = false;
          if (save_result.decision == game::CampaignSaveDecision::save &&
              save_result.slot) {
            auto staged_campaign = *campaign;
            auto staged_slots = save_slots;
            if (!staged_campaign.stageMissionCompletionInSlot(
                    staged_slots, *save_result.slot, carry_for_next)) {
              PsyX_Log_Error("Campaign save transaction rejected state\n");
              break;
            }
            // The selected slot remains on this mission with a pending EOL.
            // A shutdown during the following movie therefore resumes the
            // exact retail handoff instead of skipping it.
            const auto store_decision = storeTitleSaveSlotsWithRecovery(
                save_path, staged_slots, pad, previous_buttons, input_);
            if (store_decision == SaveStoreDecision::return_to_title) {
              break;
            }
            if (store_decision == SaveStoreDecision::stored) {
              *campaign = staged_campaign;
              save_slots = staged_slots;
              completion_is_saved = true;
            }
          }
          if (!mission->endingMovie().path.empty()) {
            previous_buttons = movie_player.playStandalone(
                mission->endingMovie(), pad, previous_buttons,
                endingMovieSkipPolicy(mission->definition()));
          }

          auto candidate_campaign = *campaign;
          auto candidate_slots = save_slots;
          const auto advance =
              completion_is_saved
                  ? candidate_campaign.completeMission(candidate_slots)
                  : candidate_campaign.completeMissionWithoutSaving();
          if (advance == game::CampaignAdvance::invalid) {
            PsyX_Log_Error("Campaign transition rejected inconsistent state\n");
            break;
          }
          if (completion_is_saved) {
            const auto store_decision = storeTitleSaveSlotsWithRecovery(
                save_path, candidate_slots, pad, previous_buttons, input_);
            if (store_decision == SaveStoreDecision::return_to_title) {
              break;
            }
          }
          *campaign = candidate_campaign;
          save_slots = candidate_slots;
          campaign_carry = advance == game::CampaignAdvance::next_mission
                               ? carry_for_next
                               : std::nullopt;
          if (advance == game::CampaignAdvance::campaign_complete) {
            PsyX_Log_Info("Campaign complete\n");
            break;
          }
          continue;
        }
        exit_application =
            scene_result.reason == detail::SceneExitReason::exit_application;
        break;
      }
      if (exit_application) {
        break;
      }
      uploadTitleAssets(assets_);
      configureTitleNotice();
      menu_.completeSearch();
      menu_.setSaveSlots(loadTitleSaveSlots(save_path));
    }
    PadStopCom();
  }

private:
  std::vector<char> title_;
  game::TitleAssets assets_;
  game::TitleMovies movies_;
  game::MissionPackage initial_mission_;
  std::filesystem::path cue_path_;
  std::string supported_game_serial_;
  game::TitleMenu menu_;
  game::TitleCommand selected_command_{game::TitleCommand::none};
  int title_analog_direction_{};
  GraphicsSettings graphics_;
  KeyboardMouseBindings input_;
  game::RetailCheatState cheats_;
};

class PsyCrossSceneHost final : public Host {
public:
  PsyCrossSceneHost(std::string title, game::MissionPackage mission,
                    std::filesystem::path cue_path, GraphicsSettings graphics,
                    KeyboardMouseBindings input, game::RetailCheatState cheats)
      : title_(title.begin(), title.end()), mission_(std::move(mission)),
        cue_path_(std::move(cue_path)), graphics_(graphics), input_(input),
        cheats_(cheats) {
    title_.push_back('\0');
  }

  void run() override {
    configureGraphics(graphics_);
    PsyX_Initialise(title_.data(), graphics_.width, graphics_.height, 0);
    configurePresentation(graphics_);
    [[maybe_unused]] detail::PsyCrossWindowMode window_mode{
        graphics_.fullscreen};
    configureInput();
    detail::configurePsyCrossVideoMode(detail::gameplay_video_mode, true);

    PADRAW pad{};
    PadInitDirect(reinterpret_cast<unsigned char *>(&pad), nullptr);
    PadStartCom();
    detail::PsyCrossMoviePlayer movie_player;
    auto previous_buttons = std::uint16_t{0xffffU};
    if (!mission_.openingMovie().path.empty()) {
      previous_buttons = movie_player.playStandalone(mission_.openingMovie(),
                                                     pad, previous_buttons);
    }
    detail::PsyCrossMissionStart mission_start;
    previous_buttons =
        mission_start.run(mission_, pad, previous_buttons, input_);
    detail::PsyCrossSceneViewer scene_viewer{input_, cheats_};
    const auto result = scene_viewer.run(mission_, pad, previous_buttons,
                                         cue_path_, mission_.definition().index,
                                         mission_start.takePreloadedGameplay(),
                                         mission_start.takePreloadedAudio());
    if (result.reason == detail::SceneExitReason::mission_complete &&
        !mission_.endingMovie().path.empty()) {
      static_cast<void>(movie_player.playStandalone(
          mission_.endingMovie(), pad, result.previous_buttons,
          endingMovieSkipPolicy(mission_.definition())));
    }
    PadStopCom();
  }

private:
  std::vector<char> title_;
  game::MissionPackage mission_;
  std::filesystem::path cue_path_;
  GraphicsSettings graphics_;
  KeyboardMouseBindings input_;
  game::RetailCheatState cheats_;
};

} // namespace

std::unique_ptr<Host> createPsyCrossHost(std::string title,
                                         GraphicsSettings graphics) {
  return std::make_unique<PsyCrossHost>(std::move(title), graphics);
}

std::unique_ptr<Host> createPsyCrossTitleHost(
    std::string title, game::TitleAssets assets, game::TitleMovies movies,
    game::MissionPackage initial_mission, std::filesystem::path cue_path,
    std::string supported_game_serial, GraphicsSettings graphics,
    KeyboardMouseBindings input, game::RetailCheatState cheats) {
  return std::make_unique<PsyCrossTitleHost>(
      std::move(title), std::move(assets), std::move(movies),
      std::move(initial_mission), std::move(cue_path),
      std::move(supported_game_serial), graphics, input, cheats);
}

std::unique_ptr<Host> createPsyCrossSceneHost(std::string title,
                                              game::MissionPackage mission,
                                              std::filesystem::path cue_path,
                                              GraphicsSettings graphics,
                                              KeyboardMouseBindings input,
                                              game::RetailCheatState cheats) {
  return std::make_unique<PsyCrossSceneHost>(
      std::move(title), std::move(mission), std::move(cue_path), graphics,
      input, cheats);
}

} // namespace sf::platform
