SceneViewerResult PsyCrossSceneViewer::run(
    const game::MissionPackage &mission, PADRAW &pad,
    std::uint16_t previous_buttons, const std::filesystem::path &cue_path,
    std::uint32_t maximum_unlocked_mission,
    std::unique_ptr<game::GameplaySession> preloaded_gameplay,
    std::unique_ptr<PsyCrossAudioOutput> preloaded_audio) {
  g_cfg_pgxpTextureCorrection = 1;
  g_cfg_pgxpZBuffer = 1;
  InitGeom();
  SetGeomOffset(screen_width / 2, screen_height / 2);
  SetGeomScreen(320);

  if (!preloaded_gameplay) {
    preloaded_gameplay = std::make_unique<game::GameplaySession>(mission);
  }
  auto &gameplay = *preloaded_gameplay;
  // The retail terminal transition can retire the live mission/inventory
  // tables on the same 20 Hz tick that requests EOL. Retain the newest valid
  // carry snapshot while those tables are still coherent so campaign flow
  // never depends on reading already-unloaded guest pointers.
  auto latest_campaign_carry = gameplay.campaignCarryState();
  const auto apply_retail_cheats = [&] {
    if (cheats_.all_weapons && !gameplay.activateRetailAllWeaponsCheat()) {
      PsyX_Log_Error("Retail all-weapons cheat activation failed\n");
    }
    if (cheats_.hard_mode && !gameplay.setRetailHardMode(true)) {
      PsyX_Log_Error("Retail hard-mode cheat activation failed\n");
    }
    if (cheats_.one_shot_kills && !gameplay.setRetailOneShotKills(true)) {
      PsyX_Log_Error("Retail one-shot cheat activation failed\n");
    }
    if (cheats_.weak_enemies && !gameplay.setRetailWeakEnemies(true)) {
      PsyX_Log_Error("Retail weak-enemies cheat activation failed\n");
    }
  };
  apply_retail_cheats();
  auto gameplay_audio = preloaded_audio
                            ? std::move(preloaded_audio)
                            : std::make_unique<PsyCrossAudioOutput>();
  PsyCrossUiAudio ui_audio{cue_path};
  PsyCrossMoviePlayer movie_player;
  FireAnimation fire_animation{gameplay};
  const auto &player_hmd =
      std::get<assets::HmdModel>(gameplay.playerModel().geometry);
  game::ActorAnimationBank actor_animations{mission.characterAnimations(),
                                            player_hmd.parts().size()};
  TextureStreamer textures{mission};
  HudTextureAtlas hud_textures{mission};
  PauseTextureAtlas pause_textures{mission};
  CombatEffectTextureAtlas effect_textures{mission};
  PoliceLightbarAnimation police_lightbar_animation;
  RetailScrimAnimation retail_scrim_animation;
#if defined(SF_ENABLE_SURFACE_PICKER)
  PsyCrossSurfacePicker surface_picker;
#endif
  // TextureStreamer already uploaded VLF and the effect atlas occupies a
  // reserved page. Reconcile only bank-specific scene data here; invalidating
  // and restoring both atlases doubled the synchronous startup transfer.
  textures.ensure(gameplay);
  const auto *world_filter = g_cfg_anisotropicFiltering ? "anisotropic"
                             : g_cfg_bilinearFiltering  ? "bilinear"
                                                        : "nearest";
  PsyX_Log_Info("Renderer: stable PGXP geometry, perspective-correct textures, "
                "%s world filtering, Z-buffer, no PS1 dither\n",
                world_filter);
  PsyX_Log_Info("Gameplay start: room=%u models=%zu objects=%zu "
                "player=(%.0f,%.0f,%.0f) yaw=%d\n",
                gameplay.currentRoom(), gameplay.presentationModels().size(),
                gameplay.activeObjects().size(), gameplay.player().x,
                gameplay.player().y, gameplay.player().z,
                gameplay.player().yaw);

  std::vector<OT_TAG> ordering_table(ordering_table_size);
  std::vector<OT_TAG> shadow_ordering_table(ordering_table_size);
  std::vector<OT_TAG> scrim_ordering_table(ordering_table_size);
  std::vector<OT_TAG> guest_overlay_ordering_table(ordering_table_size);
  std::vector<OT_TAG> fire_ordering_table(ordering_table_size);
#if defined(SF_ENABLE_SURFACE_PICKER)
  std::vector<OT_TAG> surface_picker_ordering_table(ordering_table_size);
#endif
  PrimitiveBuffer primitives;
  ActorShadowPresentationCache actor_shadow_cache;
  DynamicLightFrameScratch dynamic_light_scratch;
  LocalSceneLightingCache local_scene_lighting_cache;
  WorldChunkAppearance world_chunk_appearance;
  GameplayPresentationTransition gameplay_presentation_transition;
  WeaponEffectsScratch weapon_effects_scratch;
  HmdPoseScratch hmd_pose_scratch;
  std::vector<WorldCalloutAnchor> world_callout_scratch;
  GameplayHudScratch gameplay_hud_scratch;
  constexpr std::uint16_t pause_button = 0x08U;
  constexpr std::uint16_t cancel_button = 0x1000U | 0x2000U;
  constexpr std::uint16_t confirm_button = 0x4000U | 0x8000U;
  constexpr std::uint16_t left_button = 0x80U;
  constexpr std::uint16_t right_button = 0x20U;
  constexpr std::uint16_t up_button = 0x10U;
  constexpr std::uint16_t down_button = 0x40U;
  constexpr std::uint16_t movement_buttons =
      left_button | right_button | up_button | down_button;
  constexpr int analog_deadzone = 24;
  constexpr double retail_simulation_step_seconds = 1.0 / 20.0;
  constexpr std::uint32_t retail_audio_callback_hz = 120U;
  constexpr double retail_audio_step_seconds =
      1.0 / static_cast<double>(retail_audio_callback_hz);
  constexpr double maximum_frame_time_seconds = 0.25;
  constexpr unsigned int maximum_backlog_updates = 5U;
  constexpr unsigned int maximum_updates_per_presentation = 1U;
  constexpr unsigned int maximum_audio_backlog_updates = 30U;
  constexpr unsigned int maximum_audio_updates_per_presentation = 30U;
  constexpr double quick_turn_tap_seconds = 0.1;
  constexpr double startup_input_settle_seconds = 1.0;
  constexpr double restart_input_settle_seconds = 0.25;
  constexpr double neutral_input_arm_seconds = 0.25;
  constexpr double controller_run_threshold = 0.72;
  double input_settle_seconds = startup_input_settle_seconds;
  double neutral_movement_seconds = 0.0;
  bool movement_armed = false;
  bool log_next_frame = true;
  bool paused = false;
  bool binding_capture = false;
  bool pause_was_down = false;
  bool interact_was_down = false;
  int pause_analog_x = 0;
  int pause_analog_y = 0;
  double dpad_down_held_seconds = 0.0;
  double simulation_accumulator_seconds = 0.0;
  double audio_accumulator_seconds = 0.0;
  double presentation_delta_seconds = 0.0;
  game::GameplayInput latched_gameplay_input{};
  sf::platform::PlayerLookLatch latched_player_look;
  sf::platform::PlayerLookDisplayIntegrator display_player_look;
  sf::platform::PlayerAimFireLatch latched_aim_for_fire;
  sf::platform::PlayerLookSample presentation_look_correction{};
  const auto clear_latched_gameplay_input = [&] {
    latched_gameplay_input = {};
    latched_player_look.reset();
    display_player_look.reset();
    latched_aim_for_fire.reset();
    presentation_look_correction = {};
  };
  const auto latch_gameplay_input = [&](const game::GameplayInput &input) {
    // Continuous controls use the newest sampled state. Edges and sampled
    // impulses survive until the next 20 Hz retail step; look and wheel
    // deltas are accumulated across every native presentation frame.
    latched_gameplay_input.move = input.move;
    latched_gameplay_input.turn = input.turn;
    latched_gameplay_input.run = input.run;
    latched_gameplay_input.aim = input.aim;
    latched_gameplay_input.strafe = input.strafe;
    latched_gameplay_input.aim_peek = input.aim_peek;
    latched_gameplay_input.fire_held = input.fire_held;
    latched_gameplay_input.target_lock_held = input.target_lock_held;
    latched_gameplay_input.next_weapon =
        latched_gameplay_input.next_weapon || input.next_weapon;
    latched_gameplay_input.previous_weapon =
        latched_gameplay_input.previous_weapon || input.previous_weapon;
    latched_gameplay_input.fire_pressed =
        latched_gameplay_input.fire_pressed || input.fire_pressed;
    latched_aim_for_fire.latch(input.aim, input.fire_pressed);
    latched_gameplay_input.roll = latched_gameplay_input.roll || input.roll;
    latched_gameplay_input.reload =
        latched_gameplay_input.reload || input.reload;
    latched_gameplay_input.kneel = latched_gameplay_input.kneel || input.kneel;
    latched_gameplay_input.interact =
        latched_gameplay_input.interact || input.interact;
    latched_gameplay_input.target_lock =
        latched_gameplay_input.target_lock || input.target_lock;
    latched_gameplay_input.target_lock_released =
        latched_gameplay_input.target_lock_released ||
        input.target_lock_released;
    latched_gameplay_input.quick_turn =
        latched_gameplay_input.quick_turn || input.quick_turn;
    latched_gameplay_input.quick_weapon =
        latched_gameplay_input.quick_weapon || input.quick_weapon;
    const auto wheel_delta = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(latched_gameplay_input.weapon_menu_delta) +
            input.weapon_menu_delta,
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max());
    latched_gameplay_input.weapon_menu_delta =
        static_cast<std::int32_t>(wheel_delta);
    if (input.direct_weapon) {
      latched_gameplay_input.direct_weapon = input.direct_weapon;
    }
  };
  std::uint64_t actor_tick = 0U;
  std::uint64_t submitted_presentation_sequence = 0U;
  game::LegacyWeaponEffectPresentationQueue weapon_presentation_edges;
  game::GameplayMuzzleFlashPresentationQueue muzzle_flash_edges;
  game::LegacyScrimCopyPresentationQueue scrim_copy_edges;
  GlassShatterPresentation glass_shatter;
  game::LegacyDroppedItemPresentationCache dropped_item_presentation;
  glass_shatter.reset(gameplay);
  RenderPresentationSnapshot current_render_presentation;
  captureRenderPresentation(gameplay, current_render_presentation);
  dropped_item_presentation.reconcile(
      current_render_presentation.guest_frame,
      current_render_presentation.dropped_items);
  auto current_weapon_switch_countdown = gameplay.hud().weaponSwitchFrames();
  auto previous_weapon_switch_countdown = current_weapon_switch_countdown;
  weapon_presentation_edges.observe(gameplay.legacyPresentationFrame());
  muzzle_flash_edges.observe(gameplay.effects());
  scrim_copy_edges.observe(gameplay.legacyPresentationFrame());
  auto previous_render_presentation = current_render_presentation;
  auto interpolated_render_presentation = current_render_presentation;
  const auto reset_render_presentation = [&] {
    dropped_item_presentation.reset();
    captureRenderPresentation(gameplay, current_render_presentation);
    dropped_item_presentation.reconcile(
        current_render_presentation.guest_frame,
        current_render_presentation.dropped_items);
    previous_render_presentation = current_render_presentation;
    interpolated_render_presentation = current_render_presentation;
    gameplay_hud_scratch = {};
    current_weapon_switch_countdown = gameplay.hud().weaponSwitchFrames();
    previous_weapon_switch_countdown = current_weapon_switch_countdown;
    weapon_presentation_edges.reset();
    muzzle_flash_edges.reset();
    glass_shatter.reset(gameplay);
    actor_shadow_cache.reset();
    dynamic_light_scratch.reset();
    world_chunk_appearance.reset();
    gameplay_presentation_transition.reset();
    primitives.invalidateCapacityPlan();
    weapon_presentation_edges.observe(gameplay.legacyPresentationFrame());
    muzzle_flash_edges.observe(gameplay.effects());
    simulation_accumulator_seconds = 0.0;
  };
  auto previous_room = gameplay.currentRoom();
  auto previous_weapon = gameplay.hud().inventory().current();
  auto previous_projectile_count = gameplay.projectiles().size();
  auto previous_dynamic_fire = false;
  sf::platform::PlayerInputMapper player_input{
      sf::platform::PlayerInputConfiguration{
          // Keep the exact 20 Hz retail simulation while native mouse aim
          // remains smooth at the display refresh. Raw counts stay linear and
          // the lower gain preserves fine target selection without adding
          // refresh-dependent acceleration.
          .look_deadzone = 0.10,
          .mouse_yaw_sensitivity =
              sf::platform::first_person_mouse_yaw_sensitivity,
          .mouse_pitch_sensitivity =
              sf::platform::first_person_mouse_pitch_sensitivity,
          // One full stick sample is consumed once per 20 Hz retail tick.
          // Earlier saturation makes large turns faster; the smaller
          // deadzone retains smooth fine adjustment near the stick centre.
          .controller_yaw_sensitivity = 176.0,
          .controller_pitch_sensitivity = 160.0,
      }};
  RelativeMouseCapture mouse_capture;
  auto &pause_settings = pause_settings_;
  if (!pause_settings_initialized_) {
    if (const auto volumes = gameplay.audioVolumes()) {
      pause_settings.sound_effects_volume = volumes->sound_effects;
      pause_settings.music_volume = volumes->music;
      pause_settings.voice_volume = volumes->voice_over;
    }
    pause_settings_initialized_ = true;
  }
  ui_audio.setVolumePercent(pause_settings.sound_effects_volume);
  ui_audio.setVoiceVolumePercent(pause_settings.voice_volume);
  game::PauseMenu pause_menu;
  std::optional<game::RetailCheat> latched_pause_cheat;
  std::uint64_t pause_animation_tick = 0U;
  const auto set_retail_cheat = [&](game::RetailCheat cheat, bool enabled) {
    auto accepted = false;
    switch (cheat) {
    case game::RetailCheat::all_weapons:
      accepted = gameplay.setRetailAllWeaponsCheat(enabled);
      break;
    case game::RetailCheat::hard_mode:
      accepted = gameplay.setRetailHardMode(enabled);
      break;
    case game::RetailCheat::one_shot_kills:
      accepted = gameplay.setRetailOneShotKills(enabled);
      break;
    case game::RetailCheat::stage_select:
      accepted = true;
      break;
    case game::RetailCheat::weak_enemies:
      accepted = gameplay.setRetailWeakEnemies(enabled);
      break;
    case game::RetailCheat::movie_theater:
      // The retail action is valid only at the Georgia Street theater
      // trigger. Disabling merely clears the persistent host-side latch.
      accepted = !enabled || gameplay.activateRetailMovieTheaterCheat();
      break;
    }
    if (accepted) {
      cheats_.set(cheat, enabled);
    }
    return accepted;
  };
  const auto apply_pause_settings = [&](const game::PauseSettings &settings) {
    SetGeomOffset(screen_width / 2 + settings.screen_center_x,
                  screen_height / 2 + settings.screen_center_y);
    auto configuration = player_input.configuration();
    configuration.invert_pitch = settings.invert_aim;
    player_input.setConfiguration(configuration);
    if (!gameplay.setAudioVolumes({
            .sound_effects = settings.sound_effects_volume,
            .music = settings.music_volume,
            .voice_over = settings.voice_volume,
        })) {
      PsyX_Log_Error("Cannot apply retail gameplay audio volumes\n");
      return false;
    }
    // MENU.OVL mutes the mission SPU/XA mix, but its BEEPSX bank remains
    // audible.  The UI source is independent from gameplay_audio for exactly
    // this reason.
    ui_audio.setVolumePercent(settings.sound_effects_volume);
    ui_audio.setVoiceVolumePercent(settings.voice_volume);
    return true;
  };
  if (!apply_pause_settings(pause_settings)) {
    return SceneViewerResult{previous_buttons,
                             SceneExitReason::return_to_title};
  }
  const auto restore_gameplay_textures = [&](bool reset_scrim = false) {
    if (reset_scrim) {
      retail_scrim_animation.reset();
      scrim_copy_edges.reset();
      scrim_copy_edges.observe(gameplay.legacyPresentationFrame());
    }
    textures.invalidate();
    textures.ensure(gameplay);
    effect_textures.restore();
    hud_textures.invalidate();
    police_lightbar_animation.invalidate();
    if (!reset_scrim) {
      retail_scrim_animation.restore(
          gameplay, current_render_presentation.scrim, textures);
    }
  };
  const auto retail_blackout_mission =
      mission.definition().resource_name == "CAVE2";
  const auto draw_gameplay_frame = [&](unsigned int effect_updates) {
    glass_shatter.observe(gameplay, current_render_presentation);
    // Reconcile streamed world ownership before every frame. Dynamic effects
    // live in dedicated native pages and never patch these mission slots.
    textures.ensure(gameplay, weapon_presentation_edges.sprites(),
                    glass_shatter.shards());
    static_cast<void>(police_lightbar_animation.synchronize(
        gameplay, actor_tick, textures, weapon_presentation_edges.sprites()));
    const auto interpolation_amount = std::clamp(
        simulation_accumulator_seconds / retail_simulation_step_seconds, 0.0,
        1.0);
    const auto presented_weapon_switch_frames = game::interpolateHudCountdown(
        previous_weapon_switch_countdown, current_weapon_switch_countdown,
        interpolation_amount);
    interpolateRenderPresentation(
        previous_render_presentation, current_render_presentation,
        interpolation_amount, interpolated_render_presentation);
    auto &presentation = interpolated_render_presentation;
    static_cast<void>(retail_scrim_animation.prepareFrame(
        gameplay, presentation.scrim, scrim_copy_edges.phases(),
        presentation.guest_frame, textures));
    if (presentation.first_person_aim) {
      applyPresentationLook(presentation.camera,
                            current_render_presentation.camera,
                            presentation_look_correction);
    }
    active_near_clip_depth = presentation.first_person_aim
                                 ? first_person_near_clip_depth
                                 : near_clip_depth;
    const auto &camera = presentation.camera;
#if defined(SF_ENABLE_SURFACE_PICKER)
    surface_picker.beginFrame(SurfacePickerFrameContext{
        .mission = std::string{mission.definition().resource_name},
        .guest_frame = presentation.guest_frame,
        .room = gameplay.currentRoom(),
        .camera = SurfacePickerPoint3{camera.x, camera.y, camera.z},
    });
#endif
    applyRetailEnvironment(presentation.environment, camera.projection,
                           presentation.retail_environment_active,
                           retail_blackout_mission);
    SetGeomScreen(camera.projection);
    worldCalloutAnchors(gameplay, actor_animations, actor_tick,
                        presentation.objects, hmd_pose_scratch,
                        world_callout_scratch);
    const auto target_anchor = [&]() -> std::optional<Vector3> {
      if (const auto target = gameplay.aimTarget()) {
        const auto existing = std::ranges::find_if(
            world_callout_scratch, [&](const WorldCalloutAnchor &callout) {
              return callout.object == *target &&
                     callout.headshot == gameplay.headshotTargeted();
            });
        if (existing != world_callout_scratch.end()) {
          return existing->point;
        }
      }
      return aimTargetAnchor(gameplay, actor_animations, actor_tick,
                             presentation.objects, hmd_pose_scratch);
    }();
    auto view = makeViewMatrix(camera);
    registerPreciseViewMatrix(view, camera);
    // Retail pickups and flying grenade billboards use interface art but are
    // inserted into the world OT. Their TIMs must already be resident when
    // that OT executes; a late HUD upload would lose scene depth ordering.
    hud_textures.restoreGameplay(gameplay.hud(), presentation.first_person_aim,
                                 presentation.dropped_items,
                                 presentation.projectiles);
    world_chunk_appearance.advance(gameplay.presentationModels(),
                                   presentation_delta_seconds);
    const auto stats = renderWorld(
        gameplay, textures, hud_textures, fire_animation,
        textures.firePlacement(), effect_textures, actor_animations, actor_tick,
        gameplay.playerAnimationTick(), weapon_presentation_edges.events(),
        weapon_presentation_edges.muzzleFlashes(), muzzle_flash_edges.flashes(),
        weapon_presentation_edges.lines(),
        weapon_presentation_edges.particles(),
        weapon_presentation_edges.sprites(),
        weapon_presentation_edges.rawPackets(), glass_shatter.shards(),
        interpolation_amount, presentation, view, ordering_table,
        shadow_ordering_table, scrim_ordering_table,
        guest_overlay_ordering_table, fire_ordering_table,
#if defined(SF_ENABLE_SURFACE_PICKER)
        surface_picker_ordering_table,
#endif
        actor_shadow_cache, dynamic_light_scratch, local_scene_lighting_cache,
        world_chunk_appearance, weapon_effects_scratch, hmd_pose_scratch,
        primitives
#if defined(SF_ENABLE_SURFACE_PICKER)
        ,
        surface_picker
#endif
    );
#if defined(SF_ENABLE_SURFACE_PICKER)
    surface_picker.finishFrame();
#endif
    if (effect_updates != 0U) {
      fire_animation.update();
      glass_shatter.advance(effect_updates);
      ++actor_tick;
    }
    // SCRIM is an unconditional background. Draw it first without reading or
    // writing depth; the opaque world then starts against the pristine frame
    // depth and overwrites the sky wherever real scene geometry exists.
    GR_SetBlendMode(BM_NONE);
    GR_EnableDepth(0);
    GR_SetDepthState(0, 0);
    DrawOTag(reinterpret_cast<u_long *>(&scrim_ordering_table.back()));
    GR_EnableDepth(1);
    GR_SetDepthState(1, 1);
    DrawOTag(reinterpret_cast<u_long *>(&ordering_table.back()));
#if defined(SF_ENABLE_SURFACE_PICKER)
    // Picker tint is a diagnostic overlay over the exact selected model
    // surface. It writes no depth and a small offset prevents z-fighting while
    // preserving ordinary world occlusion.
    if (surface_picker.active()) {
      GR_SetDepthState(1, 0);
      GR_SetPolygonOffset(0.0F, -2.0F);
      DrawOTag(
          reinterpret_cast<u_long *>(&surface_picker_ordering_table.back()));
      GR_SetPolygonOffset(0.0F, 0.0F);
      GR_SetDepthState(1, 1);
    }
#endif
    // Posed silhouettes are projected onto exact gameplay receiver surfaces.
    // First union their model triangles into a dedicated stencil bit, then
    // shade that union once. This preserves limbs and equipment detail without
    // exposing overlapping internal geometry through darker patches.
    GR_SetBlendMode(BM_SUBTRACT);
    GR_SetDepthState(1, 0);
    GR_SetPolygonOffset(0.0F, -1.0F);
    // Semi-transparent untextured PSX primitives inherit ABR from the active
    // texture page. Pin ABR=2 here; otherwise the final world texture chosen
    // by camera/OT order can turn subtractive shadows additive and white.
    DR_TPAGE shadow_blend_page{};
    SetDrawTPage(&shadow_blend_page, 1, 0, GetTPage(0, 2, 0, 0));
    DrawPrim(&shadow_blend_page);
    GR_BeginShadowMask();
    DrawOTag(reinterpret_cast<u_long *>(&shadow_ordering_table.back()));
    GR_BeginShadowShade();
    DrawPrim(&shadow_blend_page);
    DrawOTag(reinterpret_cast<u_long *>(&shadow_ordering_table.back()));
    GR_EndShadowMask();
    GR_SetPolygonOffset(0.0F, 0.0F);
    GR_SetDepthState(1, 1);
    GR_SetBlendMode(BM_NONE);
    // Opaque geometry owns PGXP-Z. Fire blends additively against that
    // depth without writing it, so walls occlude particles and lightbars.
    GR_SetBlendMode(BM_ADD);
    GR_EnableDepth(1);
    GR_SetDepthState(1, 0);
    GR_SetPolygonOffset(0.0F, -1.0F);
    DrawOTag(reinterpret_cast<u_long *>(&fire_ordering_table.back()));
    GR_SetPolygonOffset(0.0F, 0.0F);
    GR_SetDepthState(1, 1);
    GR_SetBlendMode(BM_NONE);
    drawVirusScannerFilter(virusScannerXrayActive(
        presentation.first_person_aim, gameplay.hud().inventory().current()));
    // Remaining non-world raw packets and non-particle retail sprites are
    // already projected presentation. Rain and other identified particles
    // were reprojected into the world OT above; only true screen-space data
    // reaches this painter-order pass.
    GR_EnableDepth(0);
    GR_SetDepthState(0, 0);
    DrawOTag(reinterpret_cast<u_long *>(&guest_overlay_ordering_table.back()));
    GR_SetBlendMode(BM_NONE);
    GR_EnableDepth(1);
    GR_SetDepthState(1, 1);
    static_cast<void>(retail_scrim_animation.commitFrame(
        gameplay, presentation.scrim, scrim_copy_edges.phases(),
        presentation.guest_frame, textures));
    weapon_presentation_edges.consumeFrame();
    muzzle_flash_edges.consumeFrame();
    scrim_copy_edges.consumeFrame();
    const auto aim_heading = game::headingFromDirection(
        camera.target_x - camera.x, camera.target_z - camera.z);
    // Reserved framebuffer pages are transient. Restore the font, current
    // weapon and complete weapon-specific optic immediately before sampling.
    hud_textures.restoreGameplay(gameplay.hud(), presentation.first_person_aim,
                                 presentation.dropped_items,
                                 presentation.projectiles);
    drawRetailScreenFilter(presentation.environment,
                           presentation.retail_environment_active);
    drawGameBrightness(pause_settings.brightness);
    const auto letterbox_requested = gameplay.letterboxActive();
    gameplay_presentation_transition.advance(letterbox_requested,
                                             presentation_delta_seconds);
    const auto letterbox_amount =
        gameplay_presentation_transition.letterboxAmount();
    const auto hud_visibility =
        gameplay_presentation_transition.hudVisibility();
    const auto bar_height =
        static_cast<int>(std::lround(letterbox_amount * 18.0));
    if (bar_height > 0) {
      GR_SetBlendMode(BM_NONE);
      GR_EnableDepth(0);
      GR_SetDepthState(0, 0);
      int output_width = 1;
      int output_height = 1;
      PsyX_GetScreenSize(&output_width, &output_height);
      const auto presentation_scale = PsyX_CalculatePresentationScale(
          output_width, output_height, g_cfg_aspectMode);
      const auto horizontal_scale =
          std::isfinite(presentation_scale.x) && presentation_scale.x > 0.001F
              ? static_cast<double>(presentation_scale.x)
              : 1.0;
      const auto visible_width =
          static_cast<double>(screen_width) / horizontal_scale;
      const auto bar_left = static_cast<int>(std::floor(
          (static_cast<double>(screen_width) - visible_width) * 0.5));
      const auto bar_right = static_cast<int>(
          std::ceil((static_cast<double>(screen_width) + visible_width) * 0.5));
      const auto bar_width = std::max(bar_right - bar_left, screen_width);
      drawSolidRect(bar_left, 0, bar_width, bar_height, 0U, 0U, 0U);
      drawSolidRect(bar_left, screen_height - bar_height, bar_width, bar_height,
                    0U, 0U, 0U);
      DrawSync(0);
      GR_EnableDepth(1);
      GR_SetDepthState(1, 1);
    }
    constexpr double hidden_hud_threshold = 0.001;
    const auto hud_fully_hidden = hud_visibility <= hidden_hud_threshold;
    if (game::legacyGameplayHudPresentationActive(gameplay.missionComplete(),
                                                  hud_fully_hidden)) {
      drawGameplayHud(
          hud_textures, gameplay, camera, target_anchor, world_callout_scratch,
          input_, presentation.first_person_aim, aim_heading,
          presented_weapon_switch_frames, presentation_delta_seconds,
          pause_settings.screen_center_x, pause_settings.screen_center_y,
          hud_visibility, gameplay_hud_scratch);
      if (hud_visibility > 0.5) {
        drawRetailGrenadeTrajectory(presentation,
                                    pause_settings.screen_center_x,
                                    pause_settings.screen_center_y);
      }
    }
    // Failure presentation belongs to retail FONT/TEXT and fade state.  The
    // guest publishes the same type-on glyph packets for player death and
    // scripted contract violations, then starts its authored 0xc8-tick fade.
    // A native full-screen black overlay used to cover those packets as soon
    // as the failure latch rose, turning objective/parameter failures into an
    // abrupt black frame.  Keep only the native success handoff overlay; the
    // common failure path has already been drawn by drawGameplayHud above.
    if (gameplay.missionComplete()) {
      drawMissionCompleteOverlay(hud_textures);
    }
    drawMapFade(gameplay.mapFade());
    return stats;
  };
  static_cast<void>(
      police_lightbar_animation.synchronize(gameplay, actor_tick, textures));
  std::array<psx::SpuPcmFrame, 4096U> gameplay_pcm{};
  std::uint64_t audio_slices_completed{};
  std::uint64_t audio_pcm_frames_pumped{};
  std::uint64_t audio_pcm_blocks_pumped{};
  std::uint64_t audio_guest_clear_events{};
  std::uint64_t audio_guest_pcm_frames_read{};
  const auto pump_gameplay_audio = [&] {
    // SPU output is already clocked by the exact 120 Hz hardware scheduler.
    // Preserve that stream byte-for-byte and in generation order. The former
    // wall-budget path retained only the newest tail of a burst, so streaming
    // callbacks silently removed gunshots/music and made the remaining audio
    // jump forward relative to the picture.
    while (const auto count = gameplay.takePcm(gameplay_pcm)) {
      audio_guest_pcm_frames_read += count;
      gameplay_audio->queue(
          std::span<const psx::SpuPcmFrame>{gameplay_pcm}.first(count));
      audio_pcm_frames_pumped += count;
      ++audio_pcm_blocks_pumped;
    }
    gameplay_audio->flush();
  };
  const auto discard_gameplay_audio = [&](std::string_view reason) {
    gameplay_audio->reset(reason);
    gameplay.clearPcm();
    ++audio_guest_clear_events;
    audio_accumulator_seconds = 0.0;
  };

  const auto pending_audio_updates = [&](double accumulator_seconds) {
    return std::min<std::size_t>(
        maximum_audio_backlog_updates,
        static_cast<std::size_t>((accumulator_seconds + 1.0e-9) /
                                 retail_audio_step_seconds));
  };
  const auto service_realtime_audio = [&]() {
    auto audio_updates = 0U;
    while (audio_updates < maximum_audio_updates_per_presentation &&
           audio_accumulator_seconds + 1.0e-9 >= retail_audio_step_seconds) {
      if (!gameplay.advanceAudioSliceClock()) {
        return false;
      }
      ++audio_updates;
      ++audio_slices_completed;
      audio_accumulator_seconds =
          std::max(0.0, audio_accumulator_seconds - retail_audio_step_seconds);
      // Publish each completed hardware slice immediately. This keeps input
      // ordering intact and lets the device-side jitter ring absorb a slow
      // render frame without a destructive catch-up/reset cycle.
      pump_gameplay_audio();
    }
    return true;
  };

  PresentationFrameMeter presentation_frame_meter;
  const auto end_presented_frame = [&] {
    drawPresentationFrameMeter(hud_textures, presentation_frame_meter.text());
    PsyX_EndScene();
  };
  const auto performance_frequency = SDL_GetPerformanceFrequency();
  auto previous_frame_counter = SDL_GetPerformanceCounter();
  const auto periodic_audio_diagnostics = psyCrossAudioDiagnosticsEnabled();
  auto next_audio_diagnostic_counter =
      previous_frame_counter + performance_frequency;
  std::uint64_t audio_diagnostic_sequence{};
  const auto log_audio_diagnostics = [&](std::string_view context) {
    ++audio_diagnostic_sequence;
    gameplay_audio->logDiagnostics(context);
    PsyX_Log_Info(
        "[AudioDiag][clock] sequence=%llu context=%.*s guest_frame=%llu "
        "accumulator_ms=%.6f pending=%zu slices=%llu pcm_frames=%llu "
        "pcm_blocks=%llu guest_pcm_read=%llu guest_clears=%llu\n",
        static_cast<unsigned long long>(audio_diagnostic_sequence),
        static_cast<int>(context.size()), context.data(),
        static_cast<unsigned long long>(
            current_render_presentation.guest_frame),
        audio_accumulator_seconds * 1000.0,
        pending_audio_updates(audio_accumulator_seconds),
        static_cast<unsigned long long>(audio_slices_completed),
        static_cast<unsigned long long>(audio_pcm_frames_pumped),
        static_cast<unsigned long long>(audio_pcm_blocks_pumped),
        static_cast<unsigned long long>(audio_guest_pcm_frames_read),
        static_cast<unsigned long long>(audio_guest_clear_events));
    if (const auto guest = gameplay.audioDiagnostics()) {
      PsyX_Log_Info(
          "[AudioDiag][guest] sequence=%llu machine_tick=%llu "
          "audio_tick=%llu audio_tick_init=%u spu_sample=%llu mixed=%llu "
          "pcm_queued=%zu pcm_dropped=%llu cd_queued=%zu voices=%zu "
          "spucnt=0x%04x spustat=0x%04x cd_read=%u cd_lba=%u "
          "cd_mode=0x%02x cd_mute=%u adpcm_mute=%u xa_set=%u "
          "xa_file=%u xa_channel=%u\n",
          static_cast<unsigned long long>(audio_diagnostic_sequence),
          static_cast<unsigned long long>(guest->machine_tick),
          static_cast<unsigned long long>(guest->audio_frame_tick),
          guest->audio_frame_tick_initialized ? 1U : 0U,
          static_cast<unsigned long long>(guest->spu_sample_clock),
          static_cast<unsigned long long>(guest->spu_mixed_frames),
          guest->spu_pcm_frames,
          static_cast<unsigned long long>(guest->spu_dropped_pcm_frames),
          guest->spu_cd_frames, guest->active_spu_voices,
          static_cast<unsigned int>(guest->spu_control),
          static_cast<unsigned int>(guest->spu_status),
          static_cast<unsigned int>(guest->cd_reading), guest->cd_lba,
          static_cast<unsigned int>(guest->cd_mode),
          static_cast<unsigned int>(guest->cd_muted),
          static_cast<unsigned int>(guest->cd_adpcm_muted),
          static_cast<unsigned int>(guest->xa_stream_set),
          static_cast<unsigned int>(guest->xa_file),
          static_cast<unsigned int>(guest->xa_channel));
    } else {
      PsyX_Log_Warning(
          "[AudioDiag][guest] sequence=%llu diagnostics=unavailable\n",
          static_cast<unsigned long long>(audio_diagnostic_sequence));
    }
  };
  PsyX_Log_Info("Presentation uses configured VSYNC/frame limit; gameplay "
                "remains deterministic at 20 Hz; SPU streams at 120 Hz\n");

  for (;;) {
    // Wall time drives the fixed 20 Hz guest frame and the independent 120 Hz
    // hardware/audio clock. Display refresh only selects interpolation samples
    // and never changes simulation or audio rates.
    const auto current_frame_counter = SDL_GetPerformanceCounter();
    const auto counter_delta = current_frame_counter - previous_frame_counter;
    previous_frame_counter = current_frame_counter;
    const auto raw_elapsed_seconds =
        performance_frequency == 0U
            ? 0.0
            : static_cast<double>(counter_delta) /
                  static_cast<double>(performance_frequency);
    presentation_frame_meter.advance(raw_elapsed_seconds);
    const auto elapsed_seconds =
        std::clamp(raw_elapsed_seconds, 0.0, maximum_frame_time_seconds);
    presentation_delta_seconds = elapsed_seconds;
    audio_accumulator_seconds =
        std::min(audio_accumulator_seconds + elapsed_seconds,
                 retail_audio_step_seconds * maximum_audio_backlog_updates);
    if (periodic_audio_diagnostics && performance_frequency != 0U &&
        current_frame_counter >= next_audio_diagnostic_counter) {
      log_audio_diagnostics("periodic");
      next_audio_diagnostic_counter =
          current_frame_counter + performance_frequency;
    }
    gameplay_audio->update();
    ui_audio.update();
    PsyX_UpdateInput();
    const auto buttons = readButtons(pad);
    const auto held = static_cast<std::uint16_t>(~buttons);
    const auto settling_input = input_settle_seconds > 0.0;
    const auto pressed =
        settling_input ? std::uint16_t{}
                       : static_cast<std::uint16_t>(held & previous_buttons);
    int keyboard_count{};
    const auto *keyboard = SDL_GetKeyboardState(&keyboard_count);
    const auto keyboard_pad_mask =
        keyboardOriginPadMask(keyboard, keyboard_count, g_cfg_keyboardMapping);
    const auto controller_held = static_cast<std::uint16_t>(
        held & static_cast<std::uint16_t>(~keyboard_pad_mask));
    const auto controller_pressed = static_cast<std::uint16_t>(
        pressed & static_cast<std::uint16_t>(~keyboard_pad_mask));
    const auto mouse_buttons = SDL_GetMouseState(nullptr, nullptr);
    const auto mouse_wheel_delta = consumePsyCrossMouseWheel();
    const auto keyboard_state =
        keyboard != nullptr && keyboard_count > 0
            ? std::span<const std::uint8_t>{keyboard, static_cast<std::size_t>(
                                                          keyboard_count)}
            : std::span<const std::uint8_t>{};
#if defined(SF_ENABLE_SURFACE_PICKER)
    surface_picker.handleInput(keyboard_state);
#endif
    const auto bound_actions = sampleKeyboardMouseActions(
        input_, KeyboardMouseDeviceState{
                    .keyboard = keyboard_state,
                    .mouse_left = (mouse_buttons & SDL_BUTTON_LMASK) != 0U,
                    .mouse_right = (mouse_buttons & SDL_BUTTON_RMASK) != 0U,
                    .mouse_middle = (mouse_buttons & SDL_BUTTON_MMASK) != 0U,
                    .mouse_x1 = (mouse_buttons & SDL_BUTTON_X1MASK) != 0U,
                    .mouse_x2 = (mouse_buttons & SDL_BUTTON_X2MASK) != 0U,
                    .mouse_wheel_delta = mouse_wheel_delta,
                });
    const auto pause_down = bound_actions[KeyboardMouseAction::pause];
    const auto pause_pressed =
        input_settle_seconds <= 0.0 && pause_down && !pause_was_down;
    const auto interact_down = bound_actions[KeyboardMouseAction::interact];
    const auto interact_pressed =
        input_settle_seconds <= 0.0 && interact_down && !interact_was_down;
    const auto manual_aim_down = bound_actions[KeyboardMouseAction::aim];
    mouse_capture.set(!paused && manual_aim_down);
    int mouse_delta_x{};
    int mouse_delta_y{};
    SDL_GetRelativeMouseState(&mouse_delta_x, &mouse_delta_y);
    pause_was_down = pause_down;
    interact_was_down = interact_down;
    const auto controller_movement =
        static_cast<std::uint16_t>(controller_held & movement_buttons);
    const auto analog_x = static_cast<int>(pad.analog[2]) - 128;
    const auto analog_y = static_cast<int>(pad.analog[3]) - 128;
    const auto analog_forward = analog_y < -analog_deadzone;
    const auto analog_backward = analog_y > analog_deadzone;
    const auto analog_left = analog_x < -analog_deadzone;
    const auto analog_right = analog_x > analog_deadzone;
    const auto analog_move = -normalizedAnalogAxis(analog_y, analog_deadzone);
    const auto analog_turn = normalizedAnalogAxis(analog_x, analog_deadzone);
    sf::platform::RawPlayerInput raw_player_input;
    raw_player_input.pc = pcPlayerInputFromKeyboardMouseActions(bound_actions);
    raw_player_input.pc.mouse_delta_x =
        mouse_capture.enabled() ? static_cast<double>(mouse_delta_x) : 0.0;
    raw_player_input.pc.mouse_delta_y =
        mouse_capture.enabled() ? static_cast<double>(mouse_delta_y) : 0.0;

    // PADRAW merges PsyCross keyboard mappings with the physical controller.
    // Menus intentionally use that raw state, while gameplay keeps the PC and
    // controller paths disjoint so one key cannot trigger both mappings.
    const auto digital_forward = (controller_held & up_button) != 0U;
    const auto physical_dpad_down = (controller_held & down_button) != 0U;
    auto quick_turn_pulse = false;
    if (physical_dpad_down) {
      dpad_down_held_seconds =
          std::min(dpad_down_held_seconds + elapsed_seconds,
                   quick_turn_tap_seconds + maximum_frame_time_seconds);
    } else {
      quick_turn_pulse = dpad_down_held_seconds > 0.0 &&
                         dpad_down_held_seconds <= quick_turn_tap_seconds;
      dpad_down_held_seconds = 0.0;
    }
    const auto digital_backward =
        physical_dpad_down && dpad_down_held_seconds > quick_turn_tap_seconds;
    const auto digital_left = (controller_held & left_button) != 0U;
    const auto digital_right = (controller_held & right_button) != 0U;
    raw_player_input.controller.left_y = digital_forward != digital_backward
                                             ? digital_forward ? 1.0 : -1.0
                                             : analog_move;
    raw_player_input.controller.left_x =
        digital_left != digital_right ? digital_left ? -1.0 : 1.0 : analog_turn;
    raw_player_input.controller.right_x =
        normalizedAnalogAxis(static_cast<int>(pad.analog[0]) - 128, 0);
    raw_player_input.controller.right_y =
        -normalizedAnalogAxis(static_cast<int>(pad.analog[1]) - 128, 0);
    const auto action_button =
        [&pause_settings](game::ControllerAction action) {
          return static_cast<std::uint16_t>(
              game::controllerButtonForAction(pause_settings, action));
        };
    const auto aim_button = action_button(game::ControllerAction::aim);
    const auto fire_button = action_button(game::ControllerAction::shoot);
    const auto roll_button =
        action_button(game::ControllerAction::roll_zoom_out);
    const auto kneel_button = action_button(game::ControllerAction::kneel);
    const auto interact_button =
        action_button(game::ControllerAction::use_zoom_in);
    const auto target_lock_button =
        action_button(game::ControllerAction::target_lock);
    const auto change_weapon_button =
        action_button(game::ControllerAction::change_weapon);
    const auto strafe_left_button =
        action_button(game::ControllerAction::step_left);
    const auto strafe_right_button =
        action_button(game::ControllerAction::step_right);
    raw_player_input.controller.aim = (controller_held & aim_button) != 0U;
    raw_player_input.controller.fire = (controller_held & fire_button) != 0U;
    raw_player_input.controller.roll = (controller_held & roll_button) != 0U;
    raw_player_input.controller.kneel = (controller_held & kneel_button) != 0U;
    raw_player_input.controller.interact =
        (controller_held & interact_button) != 0U;
    raw_player_input.controller.reload =
        (controller_held & interact_button) != 0U;
    raw_player_input.controller.target_lock =
        (controller_held & target_lock_button) != 0U;
    raw_player_input.controller.run =
        raw_player_input.controller.left_y >= controller_run_threshold;
    raw_player_input.controller.quick_turn = quick_turn_pulse;
    const auto selecting_weapon =
        (controller_held & change_weapon_button) != 0U;
    raw_player_input.controller.strafe_left =
        !selecting_weapon && (controller_held & strafe_left_button) != 0U;
    raw_player_input.controller.strafe_right =
        !selecting_weapon && (controller_held & strafe_right_button) != 0U;
    raw_player_input.controller.change_weapon =
        (controller_pressed & change_weapon_button) != 0U;
    raw_player_input.controller.previous_weapon =
        selecting_weapon && (controller_held & strafe_left_button) != 0U;
    raw_player_input.controller.next_weapon =
        selecting_weapon && (controller_held & strafe_right_button) != 0U;
    const auto movement_active =
        controller_movement != 0 || analog_forward || analog_backward ||
        analog_left || analog_right || raw_player_input.pc.move_forward ||
        raw_player_input.pc.move_backward || raw_player_input.pc.turn_left ||
        raw_player_input.pc.turn_right || raw_player_input.pc.strafe_left ||
        raw_player_input.pc.strafe_right;
    previous_buttons = buttons;
    input_settle_seconds =
        std::max(0.0, input_settle_seconds - elapsed_seconds);
    const auto controller_pause_pressed = (pressed & pause_button) != 0;
    const auto pause_toggled = controller_pause_pressed || pause_pressed;
    const auto pause_direction = [](int value) {
      constexpr int pause_deadzone = 56;
      return value < -pause_deadzone ? -1 : (value > pause_deadzone ? 1 : 0);
    };
    const auto current_pause_x = pause_direction(analog_x);
    const auto current_pause_y = pause_direction(analog_y);
    if (!paused && pause_toggled) {
      if (!service_realtime_audio()) {
        PsyX_Log_Error("Realtime retail audio clock failed\n");
        return SceneViewerResult{previous_buttons,
                                 SceneExitReason::return_to_title};
      }
      mouse_capture.set(false);
      player_input.synchronize(raw_player_input);
      clear_latched_gameplay_input();
      auto pause_data =
          game::makePauseMenuData(mission, gameplay, maximum_unlocked_mission);
      pause_data.cheats = cheats_;
      pause_menu.reset(std::move(pause_data), pause_settings);
      if (cheats_.stage_select) {
        pause_menu.unlockMissionSelect();
      }
      paused = true;
      // Preserve the guest SPU/XA clock and queued PCM, but fade every host
      // output to silence while the ACD is open. Restoring the gain on exit
      // resumes music and ambient sound at the exact sample position.
      gameplay_audio->setGainPercent(0U);
      ui_audio.setVolumePercent(pause_settings.sound_effects_volume);
      binding_capture = false;
      pause_analog_x = current_pause_x;
      pause_analog_y = current_pause_y;
      pause_animation_tick = 0U;
      static_cast<void>(drawPauseMenu(pause_menu, pause_textures, hud_textures,
                                      pause_animation_tick++, input_));
      end_presented_frame();
      continue;
    }

    if (paused) {
      // Mission simulation stops in the ACD, while the 120 Hz SPU/CD stream
      // continues uninterrupted at its original sample rate.
      if (!service_realtime_audio()) {
        PsyX_Log_Error("Paused retail audio clock failed\n");
        return SceneViewerResult{previous_buttons,
                                 SceneExitReason::return_to_title};
      }
      player_input.synchronize(raw_player_input);
      auto analog_left_edge = false;
      auto analog_right_edge = false;
      auto analog_up_edge = false;
      auto analog_down_edge = false;
      if (current_pause_x == 0) {
        pause_analog_x = 0;
      } else if (pause_analog_x == 0) {
        analog_left_edge = current_pause_x < 0;
        analog_right_edge = current_pause_x > 0;
        pause_analog_x = current_pause_x;
      }
      if (current_pause_y == 0) {
        pause_analog_y = 0;
      } else if (pause_analog_y == 0) {
        analog_up_edge = current_pause_y < 0;
        analog_down_edge = current_pause_y > 0;
        pause_analog_y = current_pause_y;
      }

      const auto acd_navigation_ready =
          pause_animation_tick > acd_reveal_duration;
      auto pause_input = game::PauseMenuInput{
          .previous = acd_navigation_ready &&
                      ((pressed & up_button) != 0 || analog_up_edge),
          .next = acd_navigation_ready &&
                  ((pressed & down_button) != 0 || analog_down_edge),
          .left = acd_navigation_ready &&
                  ((pressed & left_button) != 0 || analog_left_edge),
          .right = acd_navigation_ready &&
                   ((pressed & right_button) != 0 || analog_right_edge),
          .confirm = acd_navigation_ready &&
                     ((pressed & confirm_button) != 0 || interact_pressed),
          .cancel = acd_navigation_ready &&
                    ((pressed & cancel_button) != 0 ||
                     (pause_pressed &&
                      pause_menu.screen() != game::PauseScreen::root)),
          .pause =
              controller_pause_pressed ||
              (pause_pressed && pause_menu.screen() == game::PauseScreen::root),
      };

      const auto cheat_context = [&] {
        if (pause_menu.screen() == game::PauseScreen::root) {
          if (pause_menu.selection() == 0U) {
            return game::RetailPauseCheatContext::map;
          }
          if (pause_menu.selection() == 1U) {
            return game::RetailPauseCheatContext::objectives;
          }
          if (pause_menu.selection() == 4U) {
            return game::RetailPauseCheatContext::weapons_section;
          }
        }
        if (pause_menu.screen() == game::PauseScreen::options &&
            pause_menu.selection() == 3U) {
          return game::RetailPauseCheatContext::select_mission;
        }
        if (pause_menu.screen() == game::PauseScreen::weapons &&
            pause_menu.selection() < pause_menu.data().weapons.size() &&
            pause_menu.data().weapons[pause_menu.selection()].id ==
                static_cast<std::uint32_t>(game::WeaponId::silenced_9mm)) {
          return game::RetailPauseCheatContext::silenced_9mm;
        }
        return game::RetailPauseCheatContext::none;
      }();
      const auto detected_cheat =
          game::detectRetailPauseCheat(held, cheat_context);
      auto cheat_consumed_input = false;
      if (!detected_cheat) {
        latched_pause_cheat.reset();
      } else if (detected_cheat != latched_pause_cheat) {
        const auto enabled =
            *detected_cheat == game::RetailCheat::one_shot_kills
                ? !cheats_.one_shot_kills
                : true;
        const auto activated = set_retail_cheat(*detected_cheat, enabled);
        if (activated) {
          pause_menu.setRetailCheatEnabled(*detected_cheat, enabled);
        }
        PsyX_Log_Info("Retail cheat %s: %s\n",
                      game::retailCheatName(*detected_cheat),
                      activated ? "activated" : "not available here");
        if (activated) {
          ui_audio.play(PsyCrossUiCue::confirm);
        }
        latched_pause_cheat = detected_cheat;
        cheat_consumed_input = true;
      }
      if (cheat_consumed_input) {
        pause_input = {};
      }

      const auto previous_pause_screen = pause_menu.screen();
      const auto previous_pause_selection = pause_menu.selection();
      const auto previous_pause_page = pause_menu.page();
      const auto previous_pause_expanded = pause_menu.expanded();
      game::PauseMenuCommand command;
      if (binding_capture && !pause_input.pause && !pause_input.cancel) {
        const auto binding_button = static_cast<std::uint16_t>(
            pressed & static_cast<std::uint16_t>(~pause_button));
        if (binding_button != 0U) {
          command = pause_menu.completeControllerBinding(binding_button);
          binding_capture = false;
        }
      }
      if (!command) {
        command = pause_menu.update(pause_input);
      }
      if (pause_input.cancel || pause_input.pause) {
        binding_capture = false;
      }
      const auto pause_changed =
          pause_menu.screen() != previous_pause_screen ||
          pause_menu.selection() != previous_pause_selection ||
          pause_menu.page() != previous_pause_page ||
          pause_menu.expanded() != previous_pause_expanded ||
          static_cast<bool>(command);
      if ((pause_input.cancel || pause_input.pause) && pause_changed) {
        ui_audio.play(PsyCrossUiCue::cancel);
      } else if (pause_input.confirm && pause_changed) {
        ui_audio.play(PsyCrossUiCue::confirm);
      } else if ((pause_input.previous || pause_input.next ||
                  pause_input.left || pause_input.right) &&
                 pause_changed) {
        ui_audio.play(PsyCrossUiCue::navigate);
      }

      switch (command.type) {
      case game::PauseCommandType::none:
        break;
      case game::PauseCommandType::resume:
        pause_settings = pause_menu.settings();
        ui_audio.reset();
        paused = false;
        gameplay_audio->setGainPercent(100U);
        if (!apply_pause_settings(pause_settings)) {
          return SceneViewerResult{previous_buttons,
                                   SceneExitReason::return_to_title};
        }
        player_input.synchronize(raw_player_input);
        clear_latched_gameplay_input();
        movement_armed = false;
        neutral_movement_seconds = 0.0;
        restore_gameplay_textures();
        continue;
      case game::PauseCommandType::equip_weapon:
        pause_menu.resolveWeaponEquip(
            command.subject,
            command.subject < game::weapon_slot_count &&
                gameplay.equipWeapon(
                    static_cast<game::WeaponId>(command.subject)));
        break;
      case game::PauseCommandType::preview_setting:
        pause_settings = pause_menu.settings();
        if (!apply_pause_settings(pause_menu.settings())) {
          return SceneViewerResult{previous_buttons,
                                   SceneExitReason::return_to_title};
        }
        if (command.subject ==
            static_cast<std::uint32_t>(game::PauseSetting::vibration)) {
          unsigned char motors[2]{
              static_cast<unsigned char>(pause_settings.vibration ? 0x40U : 0U),
              static_cast<unsigned char>(pause_settings.vibration ? 0x40U : 0U),
          };
          PadSetAct(0, motors, 2);
        } else if (command.subject == static_cast<std::uint32_t>(
                                          game::PauseSetting::voice_volume)) {
          ui_audio.playVoicePreview();
        }
        break;
      case game::PauseCommandType::commit_settings:
        pause_settings = pause_menu.settings();
        if (!apply_pause_settings(pause_settings)) {
          return SceneViewerResult{previous_buttons,
                                   SceneExitReason::return_to_title};
        }
        break;
      case game::PauseCommandType::revert_settings:
        pause_settings = pause_menu.settings();
        if (!apply_pause_settings(pause_menu.settings())) {
          return SceneViewerResult{previous_buttons,
                                   SceneExitReason::return_to_title};
        }
        break;
      case game::PauseCommandType::begin_controller_binding:
        binding_capture = true;
        break;
      case game::PauseCommandType::set_retail_cheat: {
        if (command.subject >= game::retail_cheat_count) {
          break;
        }
        const auto cheat =
            game::retailCheatAt(static_cast<std::size_t>(command.subject));
        const auto enabled = command.value != 0;
        const auto accepted = set_retail_cheat(cheat, enabled);
        if (!accepted) {
          pause_menu.setRetailCheatEnabled(cheat, !enabled);
        } else {
          pause_menu.setMissionSelectUnlocked(cheats_.stage_select);
        }
        ui_audio.play(accepted ? PsyCrossUiCue::confirm
                               : PsyCrossUiCue::cancel);
        PsyX_Log_Info("Retail cheat %s: %s\n", game::retailCheatName(cheat),
                      accepted ? (enabled ? "enabled" : "disabled")
                               : "not available here");
        break;
      }
      case game::PauseCommandType::restart_checkpoint:
        pause_settings = pause_menu.settings();
        if (!gameplay.restartCheckpoint()) {
          PsyX_Log_Error(
              "Checkpoint restore rejected incoherent guest state\n");
          return SceneViewerResult{previous_buttons,
                                   SceneExitReason::return_to_title};
        }
        apply_retail_cheats();
        discard_gameplay_audio("pause-restart-checkpoint");
        ui_audio.reset();
        paused = false;
        gameplay_audio->setGainPercent(100U);
        if (!apply_pause_settings(pause_settings)) {
          return SceneViewerResult{previous_buttons,
                                   SceneExitReason::return_to_title};
        }
        fire_animation = FireAnimation{gameplay};
        actor_tick = 0U;
        previous_room = gameplay.currentRoom();
        previous_weapon = gameplay.hud().inventory().current();
        previous_projectile_count = gameplay.projectiles().size();
        previous_dynamic_fire = false;
        police_lightbar_animation.invalidate();
        movement_armed = false;
        neutral_movement_seconds = 0.0;
        input_settle_seconds = restart_input_settle_seconds;
        player_input.synchronize(raw_player_input);
        reset_render_presentation();
        clear_latched_gameplay_input();
        log_next_frame = true;
        restore_gameplay_textures(true);
        continue;
      case game::PauseCommandType::restart_mission:
        pause_settings = pause_menu.settings();
        gameplay.reset();
        apply_retail_cheats();
        discard_gameplay_audio("pause-restart-mission");
        ui_audio.reset();
        paused = false;
        gameplay_audio->setGainPercent(100U);
        if (!apply_pause_settings(pause_settings)) {
          return SceneViewerResult{previous_buttons,
                                   SceneExitReason::return_to_title};
        }
        fire_animation = FireAnimation{gameplay};
        actor_tick = 0U;
        previous_room = gameplay.currentRoom();
        previous_weapon = gameplay.hud().inventory().current();
        previous_projectile_count = gameplay.projectiles().size();
        previous_dynamic_fire = false;
        police_lightbar_animation.invalidate();
        movement_armed = false;
        neutral_movement_seconds = 0.0;
        input_settle_seconds = restart_input_settle_seconds;
        player_input.synchronize(raw_player_input);
        reset_render_presentation();
        clear_latched_gameplay_input();
        log_next_frame = true;
        restore_gameplay_textures(true);
        continue;
      case game::PauseCommandType::select_mission:
        if (command.subject >= game::missionCatalog().size()) {
          break;
        }
        return SceneViewerResult{
            previous_buttons,
            SceneExitReason::mission_selected,
            command.subject,
        };
      case game::PauseCommandType::quit_game:
        return SceneViewerResult{previous_buttons,
                                 SceneExitReason::return_to_title};
      }

      static_cast<void>(drawPauseMenu(pause_menu, pause_textures, hud_textures,
                                      pause_animation_tick++, input_));
      end_presented_frame();
      continue;
    }

    if (!movement_armed && input_settle_seconds <= 0.0) {
      if (!movement_active) {
        neutral_movement_seconds += elapsed_seconds;
        movement_armed = neutral_movement_seconds >= neutral_input_arm_seconds;
      } else {
        neutral_movement_seconds = 0.0;
      }
    }
    sf::platform::PlayerInput mapped_input{};
    if (settling_input) {
      player_input.synchronize(raw_player_input);
    } else {
      mapped_input = player_input.update(raw_player_input);
    }
    auto simulation_updates_this_frame = 0U;
    std::optional<std::uint8_t> direct_weapon;
    if (mapped_input.quick_weapon_slot_pressed) {
      if (const auto weapon =
              gameplay.quickWeapon(*mapped_input.quick_weapon_slot_pressed)) {
        direct_weapon = static_cast<std::uint8_t>(*weapon);
      }
    }
    const auto manual_aim = mapped_input.aim.held;
    const auto first_person_input =
        sf::platform::firstPersonAimInput(mapped_input);
    auto sampled_input = game::GameplayInput{
        // Chase keeps the original tank controls. First-person uses a modern
        // split: W/S moves, A/D strafes, mouse/right-stick controls sight.
        .move = movement_armed ? (manual_aim ? first_person_input.move
                                             : mapped_input.move_forward)
                               : 0.0,
        .turn = movement_armed && !manual_aim ? mapped_input.turn : 0.0,
        .run = movement_armed && mapped_input.run.held,
        .aim = manual_aim,
        .next_weapon = mapped_input.next_weapon.pressed,
        .previous_weapon = mapped_input.previous_weapon.pressed,
        .quick_weapon = mapped_input.quick_weapon.pressed,
        .strafe = movement_armed ? (manual_aim ? first_person_input.strafe
                                               : mapped_input.move_strafe)
                                 : 0.0,
        // Directional look is a held rate and is safe on every catch-up tick.
        // Relative mouse motion is accumulated separately below and consumed
        // exactly once.
        .look_yaw = manual_aim
                        ? first_person_input.directional_look_per_guest_tick.yaw
                        : 0.0,
        .look_pitch =
            manual_aim
                ? -first_person_input.directional_look_per_guest_tick.pitch
                : 0.0,
        .fire_pressed = mapped_input.fire.pressed,
        .fire_held = mapped_input.fire.held,
        // These are physical retail PAD buttons, not native one-shot
        // commands. Preserve their held state across every 20 Hz guest tick.
        .roll = mapped_input.roll.held,
        .reload = mapped_input.reload.held,
        .kneel = mapped_input.kneel.held,
        .interact = mapped_input.interact.held,
        .target_lock = mapped_input.target_lock.pressed,
        .target_lock_held = mapped_input.target_lock.held,
        .target_lock_released = mapped_input.target_lock.released,
        .quick_turn = mapped_input.quick_turn.pressed,
        .weapon_menu_delta = mapped_input.weapon_menu_delta,
        .direct_weapon = direct_weapon,
        // Q/E or physical L2/R2 retain the original camera-only corner peek
        // without being confused with first-person A/D locomotion.
        .aim_peek =
            movement_armed && manual_aim ? first_person_input.peek : 0.0,
    };
    if (manual_aim) {
      sf::platform::PlayerInput relative_look;
      relative_look.mouse_look_yaw = first_person_input.mouse_look.yaw;
      relative_look.mouse_look_pitch = first_person_input.mouse_look.pitch;
      latched_player_look.latch(relative_look);
    } else if (!latched_aim_for_fire.pending()) {
      // If an aimed fire edge is pending, retain the look sample paired with
      // that edge instead of replacing it with post-RMB-release controller
      // state before the guest consumes the shot.
      latched_player_look.reset();
    }
    latch_gameplay_input(sampled_input);
    simulation_accumulator_seconds =
        std::min(simulation_accumulator_seconds + elapsed_seconds,
                 retail_simulation_step_seconds * maximum_backlog_updates);
    while (simulation_updates_this_frame < maximum_updates_per_presentation &&
           simulation_accumulator_seconds + 1.0e-9 >=
               retail_simulation_step_seconds) {
      auto simulation_input = simulation_updates_this_frame == 0U
                                  ? latched_gameplay_input
                                  : sampled_input;
      simulation_input.aim = latched_aim_for_fire.consume(
          simulation_input.aim, simulation_input.fire_pressed);
      sf::platform::PlayerLookSample consumed_look{};
      if (simulation_input.aim) {
        const auto relative = simulation_updates_this_frame == 0U
                                  ? latched_player_look.consumeForGuestTick()
                                  : sf::platform::PlayerLookSample{};
        consumed_look = nativeManualAimLook(relative);
        simulation_input.look_yaw += consumed_look.yaw;
        simulation_input.look_pitch -= consumed_look.pitch;
      }
      latched_gameplay_input = {};
      latched_player_look.reset();
      display_player_look.reset();
      std::swap(previous_render_presentation, current_render_presentation);
      previous_weapon_switch_countdown = current_weapon_switch_countdown;
      gameplay.update(simulation_input);
      current_weapon_switch_countdown = gameplay.hud().weaponSwitchFrames();
      if (auto carry = gameplay.campaignCarryState()) {
        latest_campaign_carry = std::move(carry);
      }
      weapon_presentation_edges.observe(gameplay.legacyPresentationFrame());
      muzzle_flash_edges.observe(gameplay.effects());
      scrim_copy_edges.observe(gameplay.legacyPresentationFrame());
      captureRenderPresentation(gameplay, current_render_presentation);
      dropped_item_presentation.reconcile(
          current_render_presentation.guest_frame,
          current_render_presentation.dropped_items);
      ++simulation_updates_this_frame;
      simulation_accumulator_seconds = std::max(
          0.0, simulation_accumulator_seconds - retail_simulation_step_seconds);

      // State 2 retires the guest runtime on the exact tick which publishes
      // the failure-restart request. Do not execute a second catch-up tick on
      // that finished timeline; the coherent checkpoint is restored below.
      if (gameplay.failureRestartRequested()) {
        break;
      }

      // Catch-up ticks retain held direction/buttons but never replay a host
      // edge or relative mouse/wheel impulse.
      sampled_input.next_weapon = false;
      sampled_input.previous_weapon = false;
      sampled_input.quick_weapon = false;
      sampled_input.fire_pressed = false;
      sampled_input.target_lock = false;
      sampled_input.target_lock_released = false;
      sampled_input.quick_turn = false;
      sampled_input.weapon_menu_delta = 0;
      sampled_input.direct_weapon.reset();
    }

    // Retail posts the failure text immediately, waits 0xc8 gameplay ticks,
    // completes its fade and then reaches state 2. Handle that terminal edge
    // before advancing the independent 120 Hz audio clock: state 2 has
    // intentionally retired the old runtime, so another audio slice would be
    // misclassified as a fatal clock error and return to the title screen.
    if (gameplay.failureRestartRequested()) {
      if (!gameplay.restartCheckpoint()) {
        PsyX_Log_Error(
            "Failure restart rejected incoherent guest checkpoint\n");
        return SceneViewerResult{previous_buttons,
                                 SceneExitReason::return_to_title};
      }
      apply_retail_cheats();
      discard_gameplay_audio("failure-restart-checkpoint");
      fire_animation = FireAnimation{gameplay};
      actor_tick = 0U;
      previous_room = gameplay.currentRoom();
      previous_weapon = gameplay.hud().inventory().current();
      previous_projectile_count = gameplay.projectiles().size();
      previous_dynamic_fire = false;
      police_lightbar_animation.invalidate();
      movement_armed = false;
      neutral_movement_seconds = 0.0;
      input_settle_seconds = restart_input_settle_seconds;
      player_input.synchronize(raw_player_input);
      reset_render_presentation();
      clear_latched_gameplay_input();
      log_next_frame = true;
      restore_gameplay_textures(true);
      PsyX_Log_Info("Failure checkpoint restored; gameplay resumed\n");
      continue;
    }
    if (manual_aim) {
      // Lead only the unfinished 20 Hz retail tick. Relative mouse counts are
      // lossless impulses; WASD is a held rate sampled from the same four-way
      // sight axes which were staged into the guest PAD above.
      display_player_look.integrate(
          simulation_updates_this_frame == 0U
              ? first_person_input.mouse_look
              : sf::platform::PlayerLookSample{},
          first_person_input.directional_look_per_guest_tick,
          simulation_updates_this_frame == 0U ? elapsed_seconds
                                              : simulation_accumulator_seconds,
          retail_simulation_step_seconds);

      presentation_look_correction = display_player_look.nativeSample();
    } else if (!current_render_presentation.first_person_aim) {
      // Keep the last correction after RMB release until the authoritative
      // guest camera performs its aim-exit cut. Clearing it on the host edge
      // creates one rollback frame before that cut.
      display_player_look.reset();
      presentation_look_correction = {};
    }
    if (gameplay.runtimeFaulted()) {
      const auto fault_reason = gameplay.runtimeFaultReason();
      const auto fault_detail = gameplay.runtimeFaultDetail();
      const auto fault_frame = gameplay.legacyPresentationFrame();
      PsyX_Log_Error("Guest renderer/UI bridge fault: reason=%.*s detail=%.*s "
                     "guest-frame=%llu "
                     "sequence=%llu; native gameplay fallback is disabled\n",
                     static_cast<int>(fault_reason.size()), fault_reason.data(),
                     static_cast<int>(fault_detail.size()), fault_detail.data(),
                     static_cast<unsigned long long>(
                         fault_frame ? fault_frame->guest_frame : 0U),
                     static_cast<unsigned long long>(
                         gameplay.legacyPresentationSequence()));
      return SceneViewerResult{previous_buttons,
                               SceneExitReason::return_to_title};
    }
    const auto presentation_sequence = gameplay.legacyPresentationSequence();
    if (gameplay.legacyRenderCommandsAuthoritative() &&
        (presentation_sequence == 0U ||
         presentation_sequence < submitted_presentation_sequence)) {
      PsyX_Log_Error(
          "Guest renderer/UI command sequence is missing or regressed\n");
      return SceneViewerResult{previous_buttons,
                               SceneExitReason::return_to_title};
    }
    // State 3/4 is a terminal retail handoff: the completed gameplay update
    // deliberately retires the live runtime before publishing the EOL
    // request. Consume that request before asking for another 120 Hz SPU
    // callback. Treating the expected terminal state as an audio-clock
    // failure used to abort the campaign immediately before the save menu.
    if (gameplay.consumeEndingMovieRequest()) {
      discard_gameplay_audio("ending-movie-handoff");
      mouse_capture.set(false);

      // Both native swap buffers may still contain pre-fade gameplay. Seal
      // the terminal boundary with the already-authored completion blackout
      // before handing control to the save UI or EOL decoder. Drawing twice
      // covers both buffers and prevents a stale un-faded frame from flashing
      // while the next frontend changes its texture/VRAM working set.
      for (auto buffer = 0U; buffer < 2U; ++buffer) {
        static_cast<void>(PsyX_BeginScene());
        hud_textures.restoreFont();
        drawMissionCompleteOverlay(hud_textures);
        PsyX_EndScene();
      }
      return SceneViewerResult{previous_buttons,
                               SceneExitReason::mission_complete, std::nullopt,
                               latest_campaign_carry};
    }
    // Retire the 20 Hz game frame before the coincident 120 Hz SPU slice.
    // This matches the PS1 boundary: a key-on authored by this frame reaches
    // the mixer immediately instead of waiting one complete 8.33 ms callback.
    // At 120/144/240 Hz that removes the refresh-dependent visible lead which
    // made animation advance several presentation frames ahead of its sound.
    if (!service_realtime_audio()) {
      PsyX_Log_Error("Realtime retail audio clock failed\n");
      return SceneViewerResult{previous_buttons,
                               SceneExitReason::return_to_title};
    }
    if (const auto scripted_movie_index =
            gameplay.consumeScriptedIntroMovieRequest()) {
      discard_gameplay_audio("scripted-movie-handoff");
      // This completed retail tick is not drawn before the movie. Commit all
      // queued SCRIM copy phases now so the post-movie authored-page restore
      // can replay the exact accumulated texture phase.
      textures.ensure(gameplay);
      static_cast<void>(retail_scrim_animation.commitSkippedFrame(
          gameplay, current_render_presentation.scrim,
          scrim_copy_edges.phases(), current_render_presentation.guest_frame,
          textures));
      scrim_copy_edges.consumeFrame();
      // State 9 has finished loading MOVIE.OVL and its completion callback
      // has returned to gameplay after the skipped guest movie call. Play
      // the decoded STR at that exact handoff, retaining the live session.
      if (simulation_updates_this_frame != 0U) {
        // This completed retail tick will not submit a world frame before
        // the standalone movie takes over. Commit every caught-up retail
        // tick so actors and fire resume on the following guest tick.
        for (auto update = 0U; update < simulation_updates_this_frame;
             ++update) {
          fire_animation.update();
          ++actor_tick;
          gameplay.advanceAnimationClock();
        }
      }
      mouse_capture.set(false);
      GR_EnableDepth(0);
      GR_SetDepthState(0, 0);
      const auto scripted_movies = mission.scriptedMovies();
      if (*scripted_movie_index >= scripted_movies.size()) {
        PsyX_Log_Error(
            "Retail state-9 movie handoff has no matching mission STR\n");
        return SceneViewerResult{previous_buttons,
                                 SceneExitReason::return_to_title};
      }
      previous_buttons = movie_player.playStandalone(
          scripted_movies[*scripted_movie_index], pad, previous_buttons);
      previous_frame_counter = SDL_GetPerformanceCounter();
      presentation_frame_meter.reset();
      GR_EnableDepth(1);
      GR_SetDepthState(1, 1);
      GR_SetBlendMode(BM_NONE);
      movement_armed = false;
      neutral_movement_seconds = 0.0;
      input_settle_seconds = restart_input_settle_seconds;
      dpad_down_held_seconds = 0.0;
      reset_render_presentation();
      clear_latched_gameplay_input();
      log_next_frame = true;
      restore_gameplay_textures();
      continue;
    }
    if (gameplay.currentRoom() != previous_room) {
      previous_room = gameplay.currentRoom();
      actor_shadow_cache.transitionRoom();
      primitives.invalidateCapacityPlan();
      log_next_frame = true;
    }
    if (gameplay.hud().inventory().current() != previous_weapon) {
      previous_weapon = gameplay.hud().inventory().current();
    }
    if (gameplay.projectiles().size() != previous_projectile_count) {
      previous_projectile_count = gameplay.projectiles().size();
    }
    const auto dynamic_fire = std::ranges::any_of(
        gameplay.projectiles(), [](const game::GameplayProjectile &projectile) {
          return projectile.active &&
                 projectile.phase == game::ProjectilePhase::explosion;
        });
    if (dynamic_fire != previous_dynamic_fire) {
      previous_dynamic_fire = dynamic_fire;
    }
    // If the renderer missed more than three VBlanks, move native visual
    // clocks to the final completed guest state before submitting it. The
    // last step remains after the draw, preserving the normal pose boundary.
    for (auto update = 1U; update < simulation_updates_this_frame; ++update) {
      fire_animation.update();
      ++actor_tick;
      gameplay.advanceAnimationClock();
    }
    const auto stats = draw_gameplay_frame(simulation_updates_this_frame);
    submitted_presentation_sequence = presentation_sequence;
    if (simulation_updates_this_frame != 0U) {
      // Root motion and the rendered HMD pose must sample the same retail
      // tick. Advance only after that pose has been submitted.
      gameplay.advanceAnimationClock();
    }
    if (log_next_frame) {
      PsyX_Log_Info(
          "Gameplay frame: room=%u models=%zu objects=%zu "
          "submitted=%zu rejected=%zu depth=%d..%d "
          "screen=(%d..%d,%d..%d)\n",
          gameplay.currentRoom(), gameplay.presentationModels().size(),
          gameplay.activeObjects().size(), stats.submitted, stats.rejected,
          stats.minimum_depth, stats.maximum_depth, stats.minimum_x,
          stats.maximum_x, stats.minimum_y, stats.maximum_y);
      log_next_frame = false;
    }
    end_presented_frame();
  }
}
