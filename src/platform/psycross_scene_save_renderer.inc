struct PsyCrossCampaignSaveRenderer::State {
  State(const game::MissionPackage &mission, KeyboardMouseBindings input)
      : textures{mission}, input{input} {}

  HudTextureAtlas textures;
  KeyboardMouseBindings input;
  std::uint64_t animation_tick{};
};

PsyCrossCampaignSaveRenderer::PsyCrossCampaignSaveRenderer(
    const game::MissionPackage &mission, KeyboardMouseBindings input)
    : state_{std::make_unique<State>(mission, input)} {}

PsyCrossCampaignSaveRenderer::~PsyCrossCampaignSaveRenderer() = default;

void PsyCrossCampaignSaveRenderer::draw(const game::CampaignSaveMenu &menu,
                                        const game::TitleSaveSlots &slots) {
  GR_SetBlendMode(BM_NONE);
  GR_SetPolygonOffset(0.0F, 0.0F);
  GR_SetDepthState(0, 0);
  GR_EnableDepth(0);
  if (!drawOriginalAcdFrame(state_->animation_tick++)) {
    return;
  }

  state_->textures.restoreFont();
  constexpr auto normal = PauseRgb{150U, 160U, 230U};
  constexpr auto selected = PauseRgb{160U, 220U, 255U};
  constexpr auto muted = PauseRgb{85U, 95U, 160U};
  const auto text = [&](std::string_view value, game::PauseRect bounds,
                        PauseRgb color = PauseRgb{150U, 160U, 230U},
                        game::PauseTextAlignment alignment =
                            game::PauseTextAlignment::left) {
    drawMenuText(value, bounds, 1, color, alignment, 11, state_->textures,
                 state_->input);
  };

  text("Mission Complete", {52, 35, 165, 10}, selected);
  if (menu.phase() == game::CampaignSavePhase::prompt) {
    text("Save Mission?", {52, 76, 165, 12}, normal,
         game::PauseTextAlignment::center);
    drawBorderedRect({66, 101, 58, 17}, {0U, 0U, 0U},
                     menu.saveSelected() ? selected : muted);
    drawBorderedRect({144, 101, 58, 17}, {0U, 0U, 0U},
                     menu.saveSelected() ? muted : selected);
    text("Yes", {66, 106, 58, 9}, menu.saveSelected() ? selected : normal,
         game::PauseTextAlignment::center);
    text("No", {144, 106, 58, 9}, menu.saveSelected() ? normal : selected,
         game::PauseTextAlignment::center);
    text("Save completed mission data", {236, 44, 109, 48});
    text("%x select", game::PauseAcdLayout::hint, normal);
  } else if (menu.phase() == game::CampaignSavePhase::slots) {
    text("Memory Card", {52, 54, 165, 10}, selected,
         game::PauseTextAlignment::center);
    for (std::size_t index = 0; index < slots.size(); ++index) {
      std::string label = "Slot " + std::to_string(index + 1U) + "  ";
      if (!slots[index].occupied) {
        label += "Empty";
      } else if (slots[index].campaign_complete) {
        label += "Complete";
      } else if (slots[index].mission_index < game::missionCatalog().size()) {
        label += game::missionCatalog()[slots[index].mission_index].title;
      }
      if (slots[index].occupied) {
        label += "  ";
        label += game::campaignDifficultyDisplayName(slots[index].difficulty);
      }
      const auto active = menu.slotSelection() == index;
      drawBorderedRect(
          {52, static_cast<std::int16_t>(76 + index * 24), 165, 17},
          {0U, 0U, 0U}, active ? selected : muted);
      text(label, {56, static_cast<std::int16_t>(81 + index * 24), 157, 9},
           active ? selected : normal);
    }
    text("Choose a save slot", {236, 44, 109, 48});
    text("%x save   %t back", game::PauseAcdLayout::hint, normal);
  } else {
    text("Overwrite existing files?", {52, 76, 165, 12}, normal,
         game::PauseTextAlignment::center);
    drawBorderedRect({66, 101, 58, 17}, {0U, 0U, 0U},
                     menu.overwriteSelected() ? selected : muted);
    drawBorderedRect({144, 101, 58, 17}, {0U, 0U, 0U},
                     menu.overwriteSelected() ? muted : selected);
    text("Yes", {66, 106, 58, 9}, menu.overwriteSelected() ? selected : normal,
         game::PauseTextAlignment::center);
    text("No", {144, 106, 58, 9}, menu.overwriteSelected() ? normal : selected,
         game::PauseTextAlignment::center);
    text("Overwrite existing files?", {236, 44, 109, 48});
    text("%x select   %t back", game::PauseAcdLayout::hint, normal);
  }
  DrawSync(0);
}

void PsyCrossCampaignSaveRenderer::drawLoadSlots(
    const game::TitleSaveSlots &slots, std::size_t selection) {
  GR_SetBlendMode(BM_NONE);
  GR_SetPolygonOffset(0.0F, 0.0F);
  GR_SetDepthState(0, 0);
  GR_EnableDepth(0);
  state_->textures.restoreFont();

  constexpr auto normal = PauseRgb{150U, 160U, 230U};
  constexpr auto selected = PauseRgb{160U, 220U, 255U};
  constexpr auto muted = PauseRgb{70U, 78U, 130U};
  const auto text = [&](std::string_view value, game::PauseRect bounds,
                        PauseRgb color = PauseRgb{150U, 160U, 230U},
                        game::PauseTextAlignment alignment =
                            game::PauseTextAlignment::left) {
    drawMenuText(value, bounds, 1, color, alignment, 11, state_->textures,
                 state_->input);
  };

  drawBorderedRect({18, 18, 284, 204}, {2U, 4U, 18U}, muted);
  text("Load Game", {28, 28, 264, 12}, selected,
       game::PauseTextAlignment::center);
  for (std::size_t index = 0U; index < slots.size(); ++index) {
    std::string label = "Slot " + std::to_string(index + 1U) + "  ";
    const auto enabled =
        slots[index].occupied && !slots[index].campaign_complete &&
        slots[index].mission_index < game::missionCatalog().size();
    if (!slots[index].occupied) {
      label += "Empty";
    } else if (slots[index].campaign_complete) {
      label += "Campaign Complete";
    } else if (enabled) {
      label += game::missionCatalog()[slots[index].mission_index].title;
    } else {
      label += "Invalid";
    }
    if (slots[index].occupied) {
      label += "  ";
      label += game::campaignDifficultyDisplayName(slots[index].difficulty);
    }
    const auto active = selection == index;
    const auto y = static_cast<std::int16_t>(52 + index * 27);
    drawBorderedRect({30, y, 260, 20}, {0U, 0U, 0U}, active ? selected : muted);
    text(label, {36, static_cast<std::int16_t>(y + 5), 248, 10},
         active ? selected : (enabled ? normal : muted));
  }
  const auto cancel_active = selection == slots.size();
  drawBorderedRect({30, 187, 260, 20}, {0U, 0U, 0U},
                   cancel_active ? selected : muted);
  text("Cancel", {36, 192, 248, 10}, cancel_active ? selected : normal,
       game::PauseTextAlignment::center);
  text("%x load   %t back", {30, 211, 260, 10}, normal,
       game::PauseTextAlignment::center);
  DrawSync(0);
}

void PsyCrossCampaignSaveRenderer::drawDifficultySelection(
    const game::TitleMenu &menu) {
  GR_SetBlendMode(BM_NONE);
  GR_SetPolygonOffset(0.0F, 0.0F);
  GR_SetDepthState(0, 0);
  GR_EnableDepth(0);
  state_->textures.restoreFont();

  // TITLE.HOG uses a dim/bright green ramp for its three authored buttons.
  // Draw the new labels through the retail font atlas with the same visual
  // hierarchy, leaving TITLE.STR visible instead of opening an ACD panel.
  constexpr auto normal = PauseRgb{35U, 70U, 41U};
  constexpr auto selected = PauseRgb{100U, 200U, 116U};
  constexpr auto muted = PauseRgb{10U, 28U, 15U};
  const auto text = [&](std::string_view value, game::PauseRect bounds,
                        PauseRgb color = PauseRgb{48U, 118U, 64U},
                        game::PauseTextAlignment alignment =
                            game::PauseTextAlignment::left) {
    drawMenuText(value, bounds, 1, color, alignment, 11, state_->textures,
                 state_->input);
  };

  drawBorderedRect({42, 60, 236, 23}, {0U, 0U, 0U}, selected);
  text("Select Difficulty", {48, 67, 224, 10}, selected,
       game::PauseTextAlignment::center);

  constexpr std::array labels{
      std::string_view{"Normal"},
      std::string_view{"Hard Mode"},
      std::string_view{"Agent"},
  };
  for (std::size_t index = 0U; index < labels.size(); ++index) {
    const auto brightness = menu.difficultyBrightness(index);
    const auto color = PauseRgb{
        static_cast<std::uint8_t>(brightness / 2U),
        brightness,
        static_cast<std::uint8_t>((brightness * 29U) / 50U),
    };
    const auto y = static_cast<std::int16_t>(100 + index * 34);
    drawBorderedRect({42, y, 236, 23}, {0U, 0U, 0U},
                     brightness == 0U ? muted : color);
    text(labels[index], {48, static_cast<std::int16_t>(y + 7), 224, 10},
         brightness == 0U ? normal : color,
         game::PauseTextAlignment::center);
  }

  text("%x select   %t back", {30, 211, 260, 10}, normal,
       game::PauseTextAlignment::center);
  DrawSync(0);
}

void PsyCrossCampaignSaveRenderer::drawAgentModeWarning() {
  GR_SetBlendMode(BM_NONE);
  GR_SetPolygonOffset(0.0F, 0.0F);
  GR_SetDepthState(0, 0);
  GR_EnableDepth(0);
  state_->textures.restoreFont();

  constexpr auto normal = PauseRgb{48U, 118U, 64U};
  constexpr auto selected = PauseRgb{100U, 200U, 116U};
  constexpr auto muted = PauseRgb{10U, 28U, 15U};
  const auto text = [&](std::string_view value, game::PauseRect bounds,
                        PauseRgb color = PauseRgb{48U, 118U, 64U},
                        game::PauseTextAlignment alignment =
                            game::PauseTextAlignment::left) {
    drawMenuText(value, bounds, 1, color, alignment, 11, state_->textures,
                 state_->input);
  };

  drawBorderedRect({24, 52, 272, 142}, {0U, 0U, 0U}, muted);
  drawBorderedRect({42, 60, 236, 23}, {0U, 0U, 0U}, selected);
  text("DIFFICULTY", {48, 67, 224, 10}, selected,
       game::PauseTextAlignment::center);
  text("AGENT MODE", {36, 91, 248, 12}, selected,
       game::PauseTextAlignment::center);
  text("A special PC-version mode not present in the original game. Enemies "
       "are more accurate and aggressive, and some missions have stricter "
       "conditions.",
       {40, 112, 240, 70}, normal, game::PauseTextAlignment::center);
  text("%x - continue; %t - back", {30, 211, 260, 10}, normal,
       game::PauseTextAlignment::center);
  DrawSync(0);
}
