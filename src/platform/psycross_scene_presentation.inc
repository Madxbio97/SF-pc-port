std::uint16_t readButtons(const PADRAW &pad) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(pad.buttons[0]) |
      (static_cast<std::uint16_t>(pad.buttons[1]) << 8U));
}

std::uint16_t
keyboardOriginPadMask(const std::uint8_t *keyboard, int keyboard_count,
                      const PsyXKeyboardMapping &mapping) noexcept {
  if (keyboard == nullptr || keyboard_count <= 0) {
    return 0U;
  }
  auto result = std::uint16_t{};
  const auto include = [&](int scancode, std::uint16_t button) {
    // SDL_SCANCODE_UNKNOWN is PsyCross's disabled mapping. Also reject a
    // malformed external mapping instead of indexing past SDL's state array.
    if (scancode > SDL_SCANCODE_UNKNOWN && scancode < keyboard_count &&
        keyboard[scancode] != 0U) {
      result = static_cast<std::uint16_t>(result | button);
    }
  };
  include(mapping.kc_select, 0x0001U);
  include(mapping.kc_l3, 0x0002U);
  include(mapping.kc_r3, 0x0004U);
  include(mapping.kc_start, 0x0008U);
  include(mapping.kc_dpad_up, 0x0010U);
  include(mapping.kc_dpad_right, 0x0020U);
  include(mapping.kc_dpad_down, 0x0040U);
  include(mapping.kc_dpad_left, 0x0080U);
  include(mapping.kc_l2, 0x0100U);
  include(mapping.kc_r2, 0x0200U);
  include(mapping.kc_l1, 0x0400U);
  include(mapping.kc_r1, 0x0800U);
  include(mapping.kc_triangle, 0x1000U);
  include(mapping.kc_circle, 0x2000U);
  include(mapping.kc_cross, 0x4000U);
  include(mapping.kc_square, 0x8000U);
  return result;
}

double normalizedAnalogAxis(int value, int deadzone) noexcept {
  const auto magnitude = std::abs(value);
  if (magnitude <= deadzone) {
    return 0.0;
  }
  constexpr int maximum_magnitude = 128;
  const auto normalized = static_cast<double>(magnitude - deadzone) /
                          static_cast<double>(maximum_magnitude - deadzone);
  return std::copysign(std::min(normalized, 1.0), static_cast<double>(value));
}

sf::platform::PlayerLookSample
nativeManualAimLook(sf::platform::PlayerLookSample mouse_look) noexcept {
  // One badly delayed host frame must not overflow heading arithmetic, but
  // native mouse aim is otherwise independent from the PS1 analog range.
  return sf::platform::PlayerLookSample{
      std::clamp(mouse_look.yaw, -sf::platform::maximum_native_mouse_look_delta,
                 sf::platform::maximum_native_mouse_look_delta),
      std::clamp(mouse_look.pitch,
                 -sf::platform::maximum_native_mouse_look_delta,
                 sf::platform::maximum_native_mouse_look_delta),
  };
}

struct Vector3 {
  double x;
  double y;
  double z;
};

struct WorldCalloutAnchor {
  Vector3 point;
  std::uint16_t object{};
  std::string_view text;
  bool headshot{};
};

struct RenderPresentationSnapshot {
  std::uint64_t guest_frame{};
  // The mission player is intentionally excluded from GameplaySession::objects
  // and therefore has no entry in legacyGuestSlotsBySceneObject().  Preserve
  // the authoritative guest slot alongside the detached player presentation
  // so one-tick weapon events can still attach to Gabe's posed weapon.
  std::int16_t player_guest_slot{-1};
  game::CameraState camera;
  // Exact retail camera which produced guest_sprites/lines/raw_packets.
  // Native manual aim replaces camera, so packet reprojection must retain
  // this immutable baseline separately.
  game::CameraState guest_packet_camera;
  // Exact FUN_800cd824/FUN_800cd90c RECT and FUN_800410d0 mode-1 phase.
  // They are independent retail animations triggered on the same script edge.
  double presentation_viewport_y{};
  double presentation_viewport_height{240.0};
  std::uint8_t interface_mode{1U};
  double normal_hud_phase{13.0};
  bool retail_ui_available{};
  game::LegacyEnvironmentBridgeState environment;
  game::LegacyScrimBridgeState scrim;
  bool retail_environment_active{};
  bool first_person_aim{};
  // Exact FUN_8002f5d8 optic mode: 0 chase, 2 sniper, 3 SVD night sight,
  // 4 viral scanner.  Keeping this separate from the selected weapon avoids
  // applying a special material during the transition into/out of the scope.
  std::uint8_t optic_mode{};
  bool retail_scope_overlay{};
  bool retail_virus_scanner_overlay{};
  std::optional<game::LegacyGrenadeTrajectoryBridgeState> grenade_trajectory;
  bool flashlight_enabled{};
  std::vector<game::LegacyVertexLightBridgeState> vertex_lights;
  std::span<const game::LegacyWorldSectionColorsBridgeState>
      world_vertex_colors;
  std::vector<game::SceneObject> objects;
  std::optional<game::SceneObject> legacy_player;
  game::PlayerState player;
  std::int32_t player_model_heading{};
  std::vector<game::GameplayProjectile> projectiles;
  std::vector<game::LegacyDroppedItemBridgeState> dropped_items;
  std::vector<game::LegacyGuestSpriteBridgeState> guest_sprites;
  std::vector<game::LegacyGuestLineBridgeState> guest_lines;
  std::vector<game::LegacyGuestRawPacketBridgeState> guest_raw_packets;
  std::vector<game::LegacyLineParticleBridgeState> current_line_particles;
  std::vector<game::LegacyCombatParticleBridgeState> current_combat_particles;
  bool renderer_sprite_fast_path{};
  bool guest_camera_lists_captured{};
};

struct RetailDepthCueState {
  std::int32_t dqa{};
  std::int32_t dqb{};
  std::int32_t projection{};
  std::uint32_t terrain{};
  bool gte_enabled{};
};

RetailDepthCueState active_retail_depth_cue;
game::DynamicLightFrame active_dynamic_lights;
game::DynamicLightFrame active_baked_world_dynamic_lights;
game::DynamicLightFrame active_dynamic_shadow_lights;
game::DynamicLightPoint active_dynamic_light_observer;
std::vector<game::RetailVertexLightState> active_retail_vertex_lights;
std::int32_t active_retail_light_projection{320};
bool active_retail_nightvision{};

void captureRenderPresentation(const game::GameplaySession &gameplay,
                               RenderPresentationSnapshot &result) {
  result.guest_frame = 0U;
  result.player_guest_slot = -1;
  result.camera = gameplay.camera();
  result.guest_packet_camera = result.camera;
  // Keep the last complete retail environment across a transient renderer
  // snapshot gap. A fresh run starts with the value-initialized state below.
  result.retail_ui_available = false;
  result.scrim = {};
  result.first_person_aim =
      gameplay.playerAim() == game::PlayerAimState::first_person;
  result.optic_mode =
      !result.first_person_aim ? 0U
      : gameplay.hud().inventory().current() ==
              game::WeaponId::nightvision_rifle
          ? 3U
      : gameplay.hud().inventory().current() == game::WeaponId::virus_scanner
          ? 4U
          : 2U;
  result.retail_scope_overlay = false;
  result.retail_virus_scanner_overlay = false;
  result.grenade_trajectory.reset();
  result.flashlight_enabled = false;
  result.vertex_lights.clear();
  result.world_vertex_colors = gameplay.legacyWorldVertexColors();
  result.objects.assign(gameplay.objects().begin(), gameplay.objects().end());
  result.legacy_player.reset();
  result.player = gameplay.player();
  result.player_model_heading = gameplay.playerModelHeading();
  result.projectiles.assign(gameplay.projectiles().begin(),
                            gameplay.projectiles().end());
  result.dropped_items.clear();
  result.guest_sprites.clear();
  result.guest_lines.clear();
  result.guest_raw_packets.clear();
  result.current_line_particles.clear();
  result.current_combat_particles.clear();
  result.renderer_sprite_fast_path = false;
  result.guest_camera_lists_captured = false;
  if (const auto *player = gameplay.legacyPlayerPresentation()) {
    result.legacy_player = *player;
  }
  if (const auto frame = gameplay.legacyPresentationFrame();
      frame && frame->renderer) {
    result.guest_frame = frame->guest_frame;
    const auto &guest_camera = frame->renderer->state.camera;
    result.guest_packet_camera = game::CameraState{
        static_cast<double>(guest_camera.eye.x),
        static_cast<double>(guest_camera.eye.y),
        static_cast<double>(guest_camera.eye.z),
        static_cast<double>(guest_camera.target.x),
        static_cast<double>(guest_camera.target.y),
        static_cast<double>(guest_camera.target.z),
        guest_camera.projectionForDisplayWidth(screen_width),
    };
    result.presentation_viewport_y = guest_camera.presentation_viewport_y;
    result.presentation_viewport_height =
        guest_camera.presentation_viewport_height;
    result.environment = frame->renderer->state.environment;
    result.scrim = frame->renderer->state.scrim;
    result.flashlight_enabled = frame->renderer->state.flashlight_enabled;
    result.vertex_lights = frame->renderer->state.vertex_lights;
    result.dropped_items = frame->renderer->state.dropped_items;
    result.guest_sprites = frame->renderer->state.guest_sprites;
    result.guest_lines = frame->renderer->state.guest_lines;
    result.guest_raw_packets = frame->renderer->state.guest_raw_packets;
    result.grenade_trajectory = frame->renderer->state.grenade_trajectory;
    result.current_line_particles = frame->renderer->state.line_particles;
    result.current_combat_particles = frame->renderer->state.combat_particles;
    result.renderer_sprite_fast_path =
        frame->renderer->state.renderer_sprite_fast_path;
    result.guest_camera_lists_captured =
        frame->renderer->state.guest_camera_lists_captured;
    result.retail_environment_active = true;
    if (frame->ui) {
      const auto &mission = frame->ui->mission;
      result.retail_ui_available = true;
      result.interface_mode = mission.interface_mode;
      result.normal_hud_phase = mission.normal_hud_phase;
      result.player_guest_slot = mission.player_slot;
      if (result.first_person_aim && mission.first_person_aim_mode != 0U) {
        result.optic_mode = mission.first_person_aim_mode;
      } else if (!result.first_person_aim) {
        result.optic_mode = 0U;
      }
      result.retail_scope_overlay = retailRifleScopeOverlayActive(
          result.first_person_aim, mission.interface_mode,
          mission.first_person_aim_mode);
      result.retail_virus_scanner_overlay = retailVirusScannerOverlayActive(
          result.first_person_aim, mission.interface_mode,
          mission.first_person_aim_mode);
    }
  }
}

void applyRetailEnvironment(
    const game::LegacyEnvironmentBridgeState &environment,
    std::int32_t projection, bool retail_environment_active) {
  DRAWENV draw_environment{};
  if (GetDrawEnv(&draw_environment) == nullptr) {
    SetDefDrawEnv(&draw_environment, 0, 0, screen_width, screen_height);
    draw_environment.dtd = 0;
  }
  // The authored clear and far colours are the dark horizon used by every
  // retail mission to hide its finite PS1 draw boundary. Washington Park's
  // green cast is a separate screen filter and must not replace this cue.
  draw_environment.clip = RECT16{0, 0, screen_width, screen_height};
  draw_environment.ofs[0] = 0;
  draw_environment.ofs[1] = 0;
  draw_environment.dfe = 1;
  draw_environment.isbg = 1;
  if (retail_environment_active) {
    const auto clear = environment.nightvision_clear_override_enabled
                           ? environment.nightvision_clear_color
                           : environment.clear_color;
    setRGB0(&draw_environment, clear.red, clear.green, clear.blue);
  } else {
    setRGB0(&draw_environment, 0, 0, 0);
  }
  PutDrawEnv(&draw_environment);

  SetBackColor(environment.back_color.red, environment.back_color.green,
               environment.back_color.blue);
  if (retail_environment_active) {
    SetFarColor(environment.fog_color.red, environment.fog_color.green,
                environment.fog_color.blue);
  } else {
    SetFarColor(0, 0, 0);
  }
  active_retail_depth_cue = {
      environment.fog_dqa,
      environment.fog_dqb,
      projection,
      retail_environment_active ? environment.effectiveTerrainDepthCue() : 0U,
      retail_environment_active && environment.fogEnabled(),
  };
  active_retail_nightvision =
      retail_environment_active && environment.nightvision_enabled;

  if (active_retail_depth_cue.gte_enabled) {
    SetDQA(environment.fog_dqa);
    SetDQB(environment.fog_dqb);
  } else {
    // DQB can survive a retail transition independently. A zero DQA is the
    // fail-closed no-fog state and must not become a full-screen color wash.
    SetDQA(0);
    SetDQB(0);
  }
}

long retailGteDepthCue(double camera_z) {
  if (!active_retail_depth_cue.gte_enabled || camera_z <= 0.0 ||
      active_retail_depth_cue.projection <= 0) {
    return 0L;
  }
  // RTPS forms its interpolation value from the perspective quotient H/SZ,
  // not linearly from camera Z. Keep a smooth native quotient while retaining
  // the exact retail DQA/DQB fixed-point equation and Q12 clamp.
  constexpr std::int64_t quotient_one = 1LL << 16U;
  constexpr std::int64_t maximum_quotient = 0x1ffffLL;
  const auto quotient = std::clamp<std::int64_t>(
      std::llround(static_cast<double>(active_retail_depth_cue.projection) /
                   nativeDepthCueCameraZ(camera_z) *
                   static_cast<double>(quotient_one)),
      0LL, maximum_quotient);
  const auto accumulator =
      static_cast<std::int64_t>(active_retail_depth_cue.dqb) +
      static_cast<std::int64_t>(active_retail_depth_cue.dqa) * quotient;
  if (accumulator <= 0) {
    return 0L;
  }
  return static_cast<long>(
      std::clamp<std::int64_t>(accumulator >> 12U, 0LL, 4096LL));
}

long retailTerrainDepthCue(double camera_z) {
  return retailTerrainDepthCueFactorQ12(active_retail_depth_cue.terrain,
                                        nativeDepthCueCameraZ(camera_z));
}
long retailObjectDepthCue(double camera_z) {
  return active_retail_depth_cue.gte_enabled ? retailGteDepthCue(camera_z)
                                             : retailTerrainDepthCue(camera_z);
}

void depthCueColor(std::uint8_t &red, std::uint8_t &green, std::uint8_t &blue,
                   long depth_cue) {
  CVECTOR input{red, green, blue, 0U};
  CVECTOR output{};
  DpqColor(&input, static_cast<int>(std::clamp<long>(depth_cue, 0L, 0x1500L)),
           &output);
  red = output.r;
  green = output.g;
  blue = output.b;
}

void depthCuePrimitive(POLY_GT3 &primitive,
                       const std::array<long, 3U> &depth_cues) {
  depthCueColor(primitive.r0, primitive.g0, primitive.b0, depth_cues[0]);
  depthCueColor(primitive.r1, primitive.g1, primitive.b1, depth_cues[1]);
  depthCueColor(primitive.r2, primitive.g2, primitive.b2, depth_cues[2]);
}

void depthCuePrimitive(POLY_GT4 &primitive,
                       const std::array<long, 4U> &depth_cues) {
  depthCueColor(primitive.r0, primitive.g0, primitive.b0, depth_cues[0]);
  depthCueColor(primitive.r1, primitive.g1, primitive.b1, depth_cues[1]);
  depthCueColor(primitive.r2, primitive.g2, primitive.b2, depth_cues[2]);
  depthCueColor(primitive.r3, primitive.g3, primitive.b3, depth_cues[3]);
}

assets::MissionTransform
interpolateTransform(const assets::MissionTransform &previous,
                     const assets::MissionTransform &current, double amount) {
  auto result = current;
  for (std::size_t index = 0; index < result.rotation.size(); ++index) {
    result.rotation[index] = static_cast<std::int16_t>(std::clamp<long>(
        std::lround(std::lerp(static_cast<double>(previous.rotation[index]),
                              static_cast<double>(current.rotation[index]),
                              amount)),
        std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
  }
  const auto translation = [amount](std::int32_t from, std::int32_t to) {
    return static_cast<std::int32_t>(std::clamp<long long>(
        std::llround(std::lerp(static_cast<double>(from),
                               static_cast<double>(to), amount)),
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max()));
  };
  result.x = translation(previous.x, current.x);
  result.y = translation(previous.y, current.y);
  result.z = translation(previous.z, current.z);
  return result;
}

bool samePresentationObject(const game::SceneObject &previous,
                            const game::SceneObject &current) {
  if (previous.model != current.model ||
      previous.class_id != current.class_id ||
      previous.source_index != current.source_index ||
      previous.destroyed_model != current.destroyed_model ||
      previous.legacy_hmd_bone_count != current.legacy_hmd_bone_count ||
      previous.legacy_hmd_root_space != current.legacy_hmd_root_space) {
    return false;
  }
  const auto dx =
      static_cast<double>(current.transform.x) - previous.transform.x;
  const auto dy =
      static_cast<double>(current.transform.y) - previous.transform.y;
  const auto dz =
      static_cast<double>(current.transform.z) - previous.transform.z;
  constexpr double maximum_interpolated_displacement = 2048.0;
  return dx * dx + dy * dy + dz * dz <=
         maximum_interpolated_displacement * maximum_interpolated_displacement;
}

game::SceneObject interpolateObject(const game::SceneObject &previous,
                                    const game::SceneObject &current,
                                    double amount) {
  if (!samePresentationObject(previous, current)) {
    return current;
  }
  auto result = current;
  result.transform =
      interpolateTransform(previous.transform, current.transform, amount);
  for (std::size_t index = 0; index < current.legacy_hmd_bone_count; ++index) {
    result.legacy_hmd_bones[index] =
        interpolateTransform(previous.legacy_hmd_bones[index],
                             current.legacy_hmd_bones[index], amount);
  }
  return result;
}

std::int32_t interpolateHeading(std::int32_t previous, std::int32_t current,
                                double amount) {
  auto delta =
      game::normalizeHeading(static_cast<std::int64_t>(current) - previous);
  if (delta > game::heading_angle_units / 2) {
    delta -= game::heading_angle_units;
  }
  return game::normalizeHeading(static_cast<std::int64_t>(std::llround(
      static_cast<double>(previous) + static_cast<double>(delta) * amount)));
}

struct CameraLookAngles {
  double yaw{};
  double pitch{};
  double distance{};
};

CameraLookAngles cameraLookAngles(const game::CameraState &camera) noexcept {
  const auto x = camera.target_x - camera.x;
  const auto y = camera.target_y - camera.y;
  const auto z = camera.target_z - camera.z;
  const auto horizontal = std::hypot(x, z);
  const auto distance = std::hypot(horizontal, y);
  constexpr auto units_per_radian =
      static_cast<double>(game::heading_angle_units) / (2.0 * std::numbers::pi);
  return CameraLookAngles{
      static_cast<double>(game::headingFromDirection(x, z)),
      std::atan2(y, horizontal) * units_per_radian,
      distance,
  };
}

void applyPresentationLook(game::CameraState &camera,
                           const game::CameraState &authoritative,
                           sf::platform::PlayerLookSample look) noexcept {
  const auto base = cameraLookAngles(authoritative);
  if (base.distance <= 0.0001) {
    return;
  }
  const auto yaw = base.yaw + look.yaw;
  const auto pitch =
      std::clamp(base.pitch - look.pitch, -game::maximum_first_person_aim_pitch,
                 game::maximum_first_person_aim_pitch);
  const auto yaw_radians =
      yaw * (2.0 * std::numbers::pi / game::heading_angle_units);
  const auto pitch_radians =
      pitch * (2.0 * std::numbers::pi / game::heading_angle_units);
  const auto horizontal = std::cos(pitch_radians) * base.distance;
  camera.target_x = camera.x + std::sin(yaw_radians) * horizontal;
  camera.target_y = camera.y + std::sin(pitch_radians) * base.distance;
  camera.target_z = camera.z + std::cos(yaw_radians) * horizontal;
}

void interpolateRenderPresentation(const RenderPresentationSnapshot &previous,
                                   const RenderPresentationSnapshot &current,
                                   double amount,
                                   RenderPresentationSnapshot &result) {
  amount = std::clamp(amount, 0.0, 1.0);
  const auto component = [amount](double from, double to) {
    return std::lerp(from, to, amount);
  };
  const auto guest_buffers_changed = result.guest_frame != current.guest_frame;
  result.guest_frame = current.guest_frame;
  result.player_guest_slot = current.player_guest_slot;
  result.camera = current.camera;
  // Camera-list packets belong to the current guest tick and must never be
  // interpolated with the previous packet camera.
  result.guest_packet_camera = current.guest_packet_camera;
  // Keep FUN_80040b50/FUN_800cd90c authoritative at 20 Hz, but present each
  // adjacent geometry step at the configured display refresh. Sentinel edges
  // have identical endpoint geometry, so this adds no extra transition phase.
  const auto retail_ui = sf::platform::interpolateRetailUiPresentation(
      sf::platform::RetailUiPresentationSample{
          .viewport_y = previous.presentation_viewport_y,
          .viewport_height = previous.presentation_viewport_height,
          .normal_hud_phase = previous.normal_hud_phase,
          .interface_mode = previous.interface_mode,
          .available = previous.retail_ui_available,
      },
      sf::platform::RetailUiPresentationSample{
          .viewport_y = current.presentation_viewport_y,
          .viewport_height = current.presentation_viewport_height,
          .normal_hud_phase = current.normal_hud_phase,
          .interface_mode = current.interface_mode,
          .available = current.retail_ui_available,
      },
      amount);
  result.presentation_viewport_y = retail_ui.viewport_y;
  result.presentation_viewport_height = retail_ui.viewport_height;
  result.interface_mode = retail_ui.interface_mode;
  result.normal_hud_phase = retail_ui.normal_hud_phase;
  result.retail_ui_available = retail_ui.available;
  result.environment = current.environment;
  result.scrim = current.scrim;
  if (previous.scrim.visible && previous.scrim.transform_valid &&
      current.scrim.visible && current.scrim.transform_valid) {
    for (std::size_t index = 0U; index < result.scrim.transform.rotation.size();
         ++index) {
      result.scrim.transform.rotation[index] =
          static_cast<std::int16_t>(std::clamp<long>(
              std::lround(component(previous.scrim.transform.rotation[index],
                                    current.scrim.transform.rotation[index])),
              std::numeric_limits<std::int16_t>::min(),
              std::numeric_limits<std::int16_t>::max()));
    }
    const auto translation = [&component](std::int32_t from, std::int32_t to) {
      return static_cast<std::int32_t>(
          std::clamp<long long>(std::llround(component(from, to)),
                                std::numeric_limits<std::int32_t>::min(),
                                std::numeric_limits<std::int32_t>::max()));
    };
    result.scrim.transform.translation.x =
        translation(previous.scrim.transform.translation.x,
                    current.scrim.transform.translation.x);
    result.scrim.transform.translation.y =
        translation(previous.scrim.transform.translation.y,
                    current.scrim.transform.translation.y);
    result.scrim.transform.translation.z =
        translation(previous.scrim.transform.translation.z,
                    current.scrim.transform.translation.z);
  }
  result.retail_environment_active = current.retail_environment_active;
  result.first_person_aim = current.first_person_aim;
  result.optic_mode = current.optic_mode;
  result.retail_scope_overlay = current.retail_scope_overlay;
  result.retail_virus_scanner_overlay = current.retail_virus_scanner_overlay;
  result.grenade_trajectory = current.grenade_trajectory;
  result.flashlight_enabled = current.flashlight_enabled;
  result.objects.assign(current.objects.begin(), current.objects.end());
  result.legacy_player = current.legacy_player;
  result.player = current.player;
  result.player_model_heading = current.player_model_heading;
  result.projectiles.assign(current.projectiles.begin(),
                            current.projectiles.end());
  sf::platform::interpolateRetailVertexLights(previous.vertex_lights,
                                              current.vertex_lights, amount,
                                              result.vertex_lights);
  if (guest_buffers_changed) {
    result.world_vertex_colors = current.world_vertex_colors;
    result.dropped_items = current.dropped_items;
    // These lists are already projected by the retail frame builder and are
    // immutable until the next 20 Hz guest tick. Retaining them avoids
    // recopying large vectors at 60-240 presentation FPS.
    result.guest_sprites = current.guest_sprites;
    result.guest_lines = current.guest_lines;
    result.guest_raw_packets = current.guest_raw_packets;
    result.current_line_particles = current.current_line_particles;
    result.current_combat_particles = current.current_combat_particles;
  }
  result.renderer_sprite_fast_path = current.renderer_sprite_fast_path;
  result.guest_camera_lists_captured = current.guest_camera_lists_captured;
  const auto aim_transition =
      previous.first_person_aim != current.first_person_aim;
  // Chase/manual-aim transitions are camera cuts in retail. Interpolating
  // across the cut moves the eye through Gabe while visibility has already
  // switched, producing a frame of his head/weapon inside the view.
  result.camera = game::interpolateCameraPresentation(
      previous.camera, current.camera, amount, current.first_person_aim,
      aim_transition);

  const auto shared_objects =
      std::min(previous.objects.size(), current.objects.size());
  for (std::size_t index = 0; index < shared_objects; ++index) {
    result.objects[index] = interpolateObject(previous.objects[index],
                                              current.objects[index], amount);
  }
  if (previous.legacy_player && current.legacy_player) {
    result.legacy_player = interpolateObject(*previous.legacy_player,
                                             *current.legacy_player, amount);
  }
  result.player.x = component(previous.player.x, current.player.x);
  result.player.y = component(previous.player.y, current.player.y);
  result.player.z = component(previous.player.z, current.player.z);
  // Scope bearing belongs to the same immutable presentation state as the
  // first-person camera. Do not leave it one interpolated frame behind a cut.
  result.player.yaw =
      aim_transition
          ? current.player.yaw
          : interpolateHeading(previous.player.yaw, current.player.yaw, amount);
  result.player_model_heading = interpolateHeading(
      previous.player_model_heading, current.player_model_heading, amount);

  const auto shared_projectiles =
      std::min(previous.projectiles.size(), current.projectiles.size());
  for (std::size_t index = 0; index < shared_projectiles; ++index) {
    const auto &from = previous.projectiles[index];
    auto &to = result.projectiles[index];
    if (from.active && to.active && from.weapon == to.weapon &&
        from.phase == to.phase) {
      to.x = component(from.x, to.x);
      to.y = component(from.y, to.y);
      to.z = component(from.z, to.z);
    }
  }
}

Vector3 normalize(Vector3 value) {
  const auto length =
      std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
  if (length <= 0.0001) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Degenerate camera vector"};
  }
  return Vector3{value.x / length, value.y / length, value.z / length};
}

Vector3 cross(const Vector3 &first, const Vector3 &second) {
  return Vector3{
      first.y * second.z - first.z * second.y,
      first.z * second.x - first.x * second.z,
      first.x * second.y - first.y * second.x,
  };
}

std::array<Vector3, 3> viewBasis(const game::CameraState &camera) {
  const auto forward = normalize(Vector3{
      camera.target_x - camera.x,
      camera.target_y - camera.y,
      camera.target_z - camera.z,
  });
  const auto right = normalize(cross(forward, Vector3{0.0, -1.0, 0.0}));
  const auto down = cross(forward, right);
  return std::array{right, down, forward};
}

MATRIX makeViewMatrix(const game::CameraState &camera) {
  const auto rows = viewBasis(camera);

  MATRIX result{};
  const auto camera_x = static_cast<std::int32_t>(std::lround(camera.x));
  const auto camera_y = static_cast<std::int32_t>(std::lround(camera.y));
  const auto camera_z = static_cast<std::int32_t>(std::lround(camera.z));
  for (std::size_t row = 0; row < rows.size(); ++row) {
    result.m[row][0] = static_cast<short>(std::lround(rows[row].x * 4096.0));
    result.m[row][1] = static_cast<short>(std::lround(rows[row].y * 4096.0));
    result.m[row][2] = static_cast<short>(std::lround(rows[row].z * 4096.0));
    result.t[row] = -static_cast<long>(
        (static_cast<std::int64_t>(result.m[row][0]) * camera_x +
         static_cast<std::int64_t>(result.m[row][1]) * camera_y +
         static_cast<std::int64_t>(result.m[row][2]) * camera_z) >>
        12U);
  }
  return result;
}

void registerPreciseViewMatrix(MATRIX &matrix,
                               const game::CameraState &camera) {
  const auto rows = viewBasis(camera);
  std::array<double, 9> exact_rotation{};
  std::array<double, 3> exact_translation{};
  for (std::size_t row = 0; row < rows.size(); ++row) {
    exact_rotation[row * 3U] = rows[row].x;
    exact_rotation[row * 3U + 1U] = rows[row].y;
    exact_rotation[row * 3U + 2U] = rows[row].z;
    exact_translation[row] = -(rows[row].x * camera.x + rows[row].y * camera.y +
                               rows[row].z * camera.z);
  }
  PGXP_MatrixRegister(&matrix, exact_rotation.data());
  PGXP_MatrixRegisterTranslation(&matrix, exact_translation.data());
}

float screenX(long packed) {
  return from_half_float(
      static_cast<short>(static_cast<std::uint32_t>(packed)));
}

float screenY(long packed) {
  return from_half_float(
      static_cast<short>(static_cast<std::uint32_t>(packed) >> 16U));
}

void setProjected(VERTTYPE *destination, long packed) {
  static_assert(sizeof(VERTTYPE) * 2U == sizeof(std::uint32_t));
  const auto bits = static_cast<std::uint32_t>(packed);
  std::memcpy(destination, &bits, sizeof(bits));
}

struct ActorShadowReceiverVertex {
  game::DynamicLightPoint point{};
  game::DynamicLightPoint normal{};
  bool wall{};
};

struct ActorShadowTriangle {
  std::array<std::size_t, 3U> vertex_indices{};
};

struct ActorShadowPresentationState {
  std::uint64_t actor{};
  std::uint64_t last_seen_guest_frame{};
  std::uint64_t receiver_guest_frame{std::numeric_limits<std::uint64_t>::max()};
  const void *body_geometry{};
  const void *attachment_geometry{};
  ActorShadowSupportState support;
  game::DynamicShadowProjectionState projection;
  std::vector<ActorShadowCachedReceiver> receivers;
  std::vector<ActorShadowCachedReceiver> edge_receivers;
};

class ActorShadowPresentationCache {
public:
  ActorShadowPresentationCache() { states_.reserve(maximum_cached_shadows); }

  void beginFrame(std::uint64_t guest_frame) {
    constexpr auto retention_guest_frames = std::uint64_t{40U};
    if (guest_frame < last_guest_frame_) {
      reset();
    } else if (guest_frame != last_guest_frame_) {
      std::erase_if(states_, [&](const auto &state) {
        return guest_frame > state.last_seen_guest_frame &&
               guest_frame - state.last_seen_guest_frame >
                   retention_guest_frames;
      });
    }
    last_guest_frame_ = guest_frame;
  }

  [[nodiscard]] ActorShadowPresentationState &
  state(std::uint64_t actor, std::uint64_t guest_frame,
        const void *body_geometry, const void *attachment_geometry,
        std::size_t vertex_count, std::size_t edge_count) {
    auto found =
        std::ranges::find(states_, actor, &ActorShadowPresentationState::actor);
    if (found == states_.end()) {
      // Mission object slots are stable, but a long session can stream many
      // actor identities through one room. Bound retained pose-sized arrays
      // and evict the least recently used shadow only; actor rendering itself
      // is never coupled to this enhancement cache.
      if (states_.size() >= maximum_cached_shadows) {
        const auto oldest = std::ranges::min_element(
            states_, {}, &ActorShadowPresentationState::last_seen_guest_frame);
        states_.erase(oldest);
      }
      states_.push_back(ActorShadowPresentationState{
          .actor = actor, .last_seen_guest_frame = guest_frame});
      found = std::prev(states_.end());
    }
    found->last_seen_guest_frame = guest_frame;
    if (found->body_geometry != body_geometry ||
        found->attachment_geometry != attachment_geometry ||
        found->receivers.size() != vertex_count ||
        found->edge_receivers.size() != edge_count) {
      found->body_geometry = body_geometry;
      found->attachment_geometry = attachment_geometry;
      found->receiver_guest_frame = std::numeric_limits<std::uint64_t>::max();
      found->support = {};
      found->projection = {};
      found->receivers.assign(vertex_count, ActorShadowCachedReceiver{});
      found->edge_receivers.assign(edge_count, ActorShadowCachedReceiver{});
    }
    return *found;
  }

  void transitionRoom() noexcept {
    for (auto &state : states_) {
      state.receiver_guest_frame = std::numeric_limits<std::uint64_t>::max();
      std::ranges::fill(state.receivers, ActorShadowCachedReceiver{});
      std::ranges::fill(state.edge_receivers, ActorShadowCachedReceiver{});
    }
  }

  void reset() noexcept {
    states_.clear();
    last_guest_frame_ = 0U;
  }

private:
  static constexpr auto maximum_cached_shadows = std::size_t{256U};
  std::vector<ActorShadowPresentationState> states_;
  std::uint64_t last_guest_frame_{};
};
