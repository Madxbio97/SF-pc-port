void renderObjects(const game::GameplaySession &gameplay,
#if defined(SF_ENABLE_SURFACE_PICKER)
                   const TextureStreamer &textures,
#endif
                   const FireAnimation &fire_animation,
                   const FireTexturePlacement &fire_texture_placement,
                   const game::ActorAnimationBank &actor_animations,
                   std::uint64_t actor_tick, double interpolation_amount,
                   const RenderPresentationSnapshot &presentation,
                   const MATRIX &view, std::vector<OT_TAG> &ordering_table,
                   std::vector<OT_TAG> &shadow_ordering_table,
#if defined(SF_ENABLE_SURFACE_PICKER)
                   std::vector<OT_TAG> &surface_picker_ordering_table,
#endif
                   std::vector<OT_TAG> &fire_ordering_table,
                   ActorShadowPresentationCache &shadow_cache,
                   LocalSceneLightingCache &local_scene_lighting_cache,
                   HmdPoseScratch &pose_scratch, PrimitiveBuffer &primitives,
                   RenderStats &stats
#if defined(SF_ENABLE_SURFACE_PICKER)
                   ,
                   PsyCrossSurfacePicker &surface_picker) {
#else
) {
#endif
  for (const auto object_index : gameplay.activeObjects()) {
    if (object_index >= presentation.objects.size()) {
      continue;
    }
    const auto &object = presentation.objects[object_index];
    const auto &source_object = gameplay.objects()[object_index];
    const auto scanner_subject =
        source_object.model < gameplay.objectModels().size() &&
        source_object.destroyed_model &&
        *source_object.destroyed_model < gameplay.objectModels().size() &&
        game::legacyVirusScannerMarker(
            gameplay.missionIndex(), source_object.class_id,
            gameplay.objectModels()[source_object.model].name,
            gameplay.objectModels()[*source_object.destroyed_model].name);
    if (scanner_subject) {
      // Scanner markers never belong to the ordinary world pass.
      continue;
    }
    const auto *displayed_model = gameplay.displayedObjectModel(object_index);
    if (displayed_model == nullptr) {
      continue;
    }
    const auto &object_model = *displayed_model;
    const auto &geometry = object_model.geometry;
#if defined(SF_ENABLE_SURFACE_PICKER)
    const auto model_index = static_cast<std::uint32_t>(
        displayed_model - gameplay.objectModels().data());
    auto picker_context = SurfacePickerObjectContext{
        .picker = &surface_picker,
        .textures = &textures,
        .ordering_table = &surface_picker_ordering_table,
        .owner = object_index,
        .label = "object-" + std::to_string(object_index) + "-model-" +
                 std::to_string(model_index),
        .room = gameplay.currentRoom(),
        .guest_frame = presentation.guest_frame,
    };
    const auto *active_picker_context =
        surface_picker.active() ? &picker_context : nullptr;
#endif
    const auto texture_bank = gameplay.displayedObjectTextureBank(object_index);
    if (const auto *model = std::get_if<assets::GmdModel>(&geometry)) {
      // The guest display-light pointer is reliable for the two weapon-crate
      // classes. Several flat GMD signs reuse that field for material state;
      // interpreting it as RGB tinted those sprites magenta.
      const auto weapon_crate =
          object.class_id == 0x4fU || object.class_id == 0x50U;
      const auto guest_back_color = weapon_crate
                                        ? retailGmdObjectBackColor(object)
                                        : VertexColor{128U, 128U, 128U};
      // Static GMD props do not carry authored per-vertex colors.  They used
      // to receive local scene light only for weapon crates, leaving switches,
      // control boxes and other active trigger points at neutral brightness.
      // Sample the same active world-color cache for every ordinary GMD prop;
      // emissive billboards/lightbars deliberately retain their own intensity.
      const auto back_color =
          object_model.visual_effect == game::ObjectVisualEffect::none
              ? localSceneBackColor(gameplay, presentation, object_index,
                                    object, guest_back_color,
                                    local_scene_lighting_cache)
              : guest_back_color;
      renderGmdObject(*model, object, object_model.visual_effect, texture_bank,
                      presentation.camera, view, ordering_table, primitives,
                      stats, back_color, false, false
#if defined(SF_ENABLE_SURFACE_PICKER)
                      ,
                      active_picker_context
#endif
      );
    } else if (const auto *hmd_model =
                   std::get_if<assets::HmdModel>(&geometry)) {
      const auto *state = gameplay.npcState(object_index);
      const auto exact_guest_pose = hasLegacyHmdBones(*hmd_model, object);
      const auto dedicated_presentation =
          gameplay.legacyDedicatedActorPresentation(object_index);
      if (!dedicated_presentation &&
          !game::legacyHmdRenderAllowed(
              gameplay.legacyRenderCommandsAuthoritative(), exact_guest_pose)) {
        // Guest-owned dormant HMD props still require an exact render command.
        // Only an active, bound retail NPC may bridge a camera-culled pose.
        continue;
      }
      const auto pose =
          state == nullptr
              ? actor_animations.enemyPose(
                    actor_tick, static_cast<std::uint64_t>(object_index) * 7U)
              : actor_animations.npcPose(
                    gameplay.npcAnimation(object_index),
                    state->locomotion_animation_tick, state->animation_tick,
                    npcAnimationPhase(object, object_index));
      const auto *render_pose =
          dedicated_presentation && !exact_guest_pose ? nullptr : &pose;
      const auto *resolved_pose =
          prepareHmdWorldVertices(*hmd_model, object, render_pose, pose_scratch,
                                  primitives.hmd_world_vertex_scratch);
      const auto world_vertices =
          std::span<const Vector3>{primitives.hmd_world_vertex_scratch};
      auto weapon = gameplay.legacyDedicatedActorWeapon(object_index);
      if (!weapon && state != nullptr && state->health != 0U) {
        weapon = state->weapon;
      }
      const auto *weapon_resolved_pose =
          render_pose == &pose ? resolved_pose : nullptr;
      const auto weapon_presentation =
          weapon ? prepareActorWeaponPresentation(gameplay, *hmd_model, object,
                                                  pose, *weapon, pose_scratch,
                                                  weapon_resolved_pose)
                 : std::nullopt;
      if (state != nullptr || dedicated_presentation) {
        const auto &support_object = object_index < gameplay.objects().size()
                                         ? gameplay.objects()[object_index]
                                         : object;
        const auto support_origin =
            transformPoint(0.0, 0.0, 0.0, support_object.transform);
        prepareActorShadowGeometry(
            *hmd_model, world_vertices,
            weapon_presentation ? &*weapon_presentation : nullptr, primitives);
        renderActorShadow(
            gameplay, *hmd_model, object,
            primitives.actor_shadow_world_vertex_scratch,
            primitives.actor_shadow_triangle_scratch,
            weapon_presentation ? weapon_presentation->geometry : nullptr,
            {support_origin.x, support_origin.y, support_origin.z},
            object_index, presentation.guest_frame, interpolation_amount,
            shadow_cache, view, shadow_ordering_table, primitives, stats);
      }
      auto back_color = retailHmdBackColor(object);
      if (state == nullptr && !dedicated_presentation &&
          !object.legacy_hmd_back_color_valid) {
        // HMD switches occasionally pass through a guest display-controller
        // transition with no valid back-color.  Neutral white is conspicuous
        // in dark rooms, so use the surrounding active map light until the
        // exact retail value becomes available again.
        back_color =
            localSceneBackColor(gameplay, presentation, object_index, object,
                                back_color, local_scene_lighting_cache);
      }
      renderHmdObject(*hmd_model, object, render_pose, texture_bank,
                      pose_scratch, view, ordering_table, primitives, stats,
                      back_color, world_vertices);
      if (weapon_presentation) {
        renderActorWeapon(
            *weapon_presentation, object, game::resident_weapon_texture_bank,
            presentation.camera, view, ordering_table, primitives, stats);
      }
    } else if (const auto *fire =
                   std::get_if<game::ObjectFireEmitter>(&geometry)) {
      // The VM bridge already carries these same CFIRE EXPL particles.
      // Keep the reconstructed host emitter only for non-legacy play.
      if (!gameplay.legacyEffectParticlesAuthoritative()) {
        renderFireObject(*fire, fire_animation.particles(object_index),
                         fire_texture_placement, view, fire_ordering_table,
                         primitives, stats);
      }
    } else {
#if defined(SF_ENABLE_SURFACE_PICKER)
      picker_context.kind = SurfacePickerKind::emd_object;
#endif
      renderEmdObject(std::get<assets::EmdScene>(geometry), object,
                      texture_bank, view, ordering_table, primitives, stats,
                      false
#if defined(SF_ENABLE_SURFACE_PICKER)
                      ,
                      active_picker_context);
#else
      );
#endif
    }
  }
}

void renderVirusScannerMarker(const game::GameplaySession &gameplay,
                              const RenderPresentationSnapshot &presentation,
                              const MATRIX &view,
                              std::vector<OT_TAG> &ordering_table,
                              PrimitiveBuffer &primitives, RenderStats &stats) {
  if (!virusScannerXrayActive(presentation.first_person_aim,
                              gameplay.hud().inventory().current())) {
    return;
  }
  const auto target = gameplay.legacyVirusScannerTargetObject();
  const auto marker =
      target ? gameplay.legacyVirusScannerMarkerObject(*target) : std::nullopt;
  if (!marker || *marker >= gameplay.objects().size() ||
      *marker >= presentation.objects.size()) {
    return;
  }
  const auto &source = gameplay.objects()[*marker];
  if (source.model >= gameplay.objectModels().size()) {
    return;
  }
  const auto &marker_model = gameplay.objectModels()[source.model];
  const auto *geometry = std::get_if<assets::GmdModel>(&marker_model.geometry);
  if (geometry == nullptr) {
    return;
  }

  // GRGLO is the authored four-triangle scanner reveal paired with BOMB.
  // Submit it to the existing projected-overlay OT: it is drawn after the
  // amber grade with world depth disabled, exactly where it can show through
  // the enclosing crate without duplicating the 147-triangle corpse HMD.
  renderGmdObject(*geometry, presentation.objects[*marker],
                  game::ObjectVisualEffect::scanner_xray,
                  gameplay.objectTextureBank(*marker), presentation.camera,
                  view, ordering_table, primitives, stats, {220U, 104U, 36U},
                  true);
}

void renderDroppedItemSprites(const HudTextureAtlas &textures,
                              const game::GameplaySession &gameplay,
                              const RenderPresentationSnapshot &presentation,
                              const MATRIX &view,
                              std::vector<OT_TAG> &ordering_table,
                              PrimitiveBuffer &primitives, RenderStats &stats) {
  const auto &camera = presentation.camera;
  const auto basis = viewBasis(camera);
  const auto projection = static_cast<double>(std::max(camera.projection, 1));
  constexpr auto center_x = static_cast<double>(screen_width) * 0.5;
  constexpr auto center_y = static_cast<double>(screen_height) * 0.5;
  constexpr double pickup_world_extent = 96.0;
  constexpr double maximum_screen_extent = 34.0;
  constexpr double pickup_center_clearance = 72.0;
  constexpr double fallback_visual_lift = 96.0;
  constexpr std::uint64_t pickup_pulse_half_ticks = 12U;
  constexpr std::uint8_t pickup_minimum_brightness = 160U;
  constexpr std::uint8_t pickup_brightness_range = 48U;
  const auto pickup_pulse_phase =
      presentation.guest_frame % (pickup_pulse_half_ticks * 2U);
  const auto pickup_pulse_rise =
      pickup_pulse_phase <= pickup_pulse_half_ticks
          ? pickup_pulse_phase
          : pickup_pulse_half_ticks * 2U - pickup_pulse_phase;
  const auto pickup_brightness = static_cast<std::uint8_t>(
      pickup_minimum_brightness +
      pickup_brightness_range * pickup_pulse_rise / pickup_pulse_half_ticks);

  for (const auto &item : presentation.dropped_items) {
    const auto layers = game::droppedItemIconLayers(item.item);
    if (layers.empty()) {
      continue;
    }
    const auto &position = item.transform.translation;
    const auto raw_world_y = -static_cast<double>(position.y);
    const auto ground_y = gameplay.droppedItemGroundY(
        static_cast<double>(position.x), static_cast<double>(position.z),
        raw_world_y, item.room);
    const auto world = Vector3{static_cast<double>(position.x),
                               ground_y ? *ground_y - pickup_center_clearance
                                        : raw_world_y - fallback_visual_lift,
                               static_cast<double>(position.z)};
    if (!gameplay.droppedItemVisibleFrom(camera.x, camera.y, camera.z, world.x,
                                         world.y, world.z, item.room)) {
      continue;
    }
    const auto delta =
        Vector3{world.x - camera.x, world.y - camera.y, world.z - camera.z};
    const auto component = [&delta](const Vector3 &axis) {
      return delta.x * axis.x + delta.y * axis.y + delta.z * axis.z;
    };
    const auto depth = preciseCameraDepth(view, world.x, world.y, world.z);
    if (depth <= active_near_clip_depth) {
      continue;
    }
    const auto projected_x =
        center_x + component(basis[0]) * projection / depth;
    const auto projected_y =
        center_y + component(basis[1]) * projection / depth;
    if (projected_x < -64.0 || projected_x > screen_width + 64.0 ||
        projected_y < -64.0 || projected_y > screen_height + 64.0) {
      continue;
    }

    std::array<int, game::maximum_weapon_icon_layers> widths{};
    std::array<int, game::maximum_weapon_icon_layers> heights{};
    for (std::size_t layer = 0U; layer < layers.size(); ++layer) {
      const auto &image = textures.image(layers[layer]);
      widths[layer] = static_cast<int>(image.displayWidth());
      heights[layer] = static_cast<int>(image.displayHeight());
    }
    const auto offsets = game::originalWeaponIconOffsets(
        std::span<const int>{widths.data(), layers.size()});
    auto group_left = offsets[0];
    auto group_right = offsets[0] + widths[0];
    for (std::size_t layer = 1U; layer < layers.size(); ++layer) {
      group_left = std::min(group_left, offsets[layer]);
      group_right = std::max(group_right, offsets[layer] + widths[layer]);
    }
    const auto group_center =
        (static_cast<double>(group_left) + group_right) * 0.5;
    const auto group_width = std::max(group_right - group_left, 1);
    const auto maximum_height = *std::max_element(
        heights.begin(),
        heights.begin() + static_cast<std::ptrdiff_t>(layers.size()));
    const auto source_extent =
        static_cast<double>(std::max(group_width, maximum_height));
    const auto category_scale = game::droppedItemPresentationScale(item.item);
    const auto scale =
        std::min({projection * pickup_world_extent * category_scale /
                      (depth * source_extent),
                  maximum_screen_extent * category_scale / source_extent, 1.0});
    const auto sort_depth =
        std::clamp(static_cast<int>(std::lround(depth * 0.25)), 1,
                   ordering_table_size - 1);

    for (std::size_t layer = 0U; layer < layers.size(); ++layer) {
      const auto &image = textures.image(layers[layer]);
      const auto armor_pickup = layers[layer] == armor_pickup_texture;
      const auto resident_x =
          armor_pickup ? pickup_resident_x : hudResidentX(image.pixels().x);
      const auto resident_y =
          armor_pickup ? pickup_resident_y : image.pixels().y;
      const auto page_x =
          static_cast<int>(resident_x & static_cast<std::uint16_t>(~63U));
      const auto page_y =
          static_cast<int>(resident_y & static_cast<std::uint16_t>(~255U));
      const auto pixels_per_word =
          image.mode() == assets::TimPixelMode::indexed4   ? 4
          : image.mode() == assets::TimPixelMode::indexed8 ? 2
                                                           : 1;
      const auto u0 = (static_cast<int>(resident_x) - page_x) * pixels_per_word;
      const auto v0 = static_cast<int>(resident_y) - page_y;
      const auto texture_page =
          GetTPage(texturePageMode(image.mode()), 0, page_x, page_y);
      // PGXP marks this as world geometry, so PsyCross treats its UV bounds as
      // inclusive. Point the far corners at the final authored texel; using
      // the HUD-style exclusive edge sampled the neighbouring atlas tile and
      // produced a bright rectangular seam around multi-part pickups.
      const auto source_right = u0 + widths[layer] - 1;
      const auto source_bottom = v0 + heights[layer] - 1;
      const auto left = projected_x + (offsets[layer] - group_center) * scale;
      const auto top = projected_y - heights[layer] * scale * 0.5;
      const auto right = left + widths[layer] * scale;
      const auto bottom = top + heights[layer] * scale;

      auto &polygon = primitives.pickup_sprites.emplace_back();
      setPolyFT4(&polygon);
      polygon.tpage = texture_page;
      polygon.clut =
          GetClut(hud_resident_clut_x,
                  armor_pickup ? pickup_resident_clut_y : hud_resident_clut_y);
      setRGB0(&polygon, pickup_brightness, pickup_brightness,
              pickup_brightness);
      setXY4(&polygon, static_cast<float>(left), static_cast<float>(top),
             static_cast<float>(right), static_cast<float>(top),
             static_cast<float>(left), static_cast<float>(bottom),
             static_cast<float>(right), static_cast<float>(bottom));
      setUV4(&polygon, static_cast<u_char>(u0), static_cast<u_char>(v0),
             static_cast<u_char>(source_right), static_cast<u_char>(v0),
             static_cast<u_char>(u0), static_cast<u_char>(source_bottom),
             static_cast<u_char>(source_right),
             static_cast<u_char>(source_bottom));

      const auto emit = [&](VERTTYPE &x, VERTTYPE &y, double screen_x,
                            double screen_y) {
        PGXPVData data{};
        data.lookup = PGXP_LOOKUP_VALUE(x, y);
        data.px = static_cast<float>((screen_x - center_x) * depth /
                                     projection / 128.0);
        data.py = static_cast<float>((screen_y - center_y) * depth /
                                     projection / 128.0);
        data.pz = static_cast<float>(depth / 128.0);
        data.sx = static_cast<float>(screen_x);
        data.sy = static_cast<float>(screen_y);
        data.scr_h = static_cast<float>(projection);
        data.ofx = static_cast<float>(center_x);
        data.ofy = static_cast<float>(center_y);
        static_cast<void>(PGXP_EmitCacheData(&data));
      };
      emit(polygon.x0, polygon.y0, left, top);
      emit(polygon.x1, polygon.y1, right, top);
      emit(polygon.x2, polygon.y2, left, bottom);
      emit(polygon.x3, polygon.y3, right, bottom);
      addPrim(&ordering_table[sort_depth], &polygon);
      ++stats.submitted;
    }
  }
}

void submitStats(RenderStats &stats, int depth,
                 std::span<const long> projected) {
  ++stats.submitted;
  stats.minimum_depth = std::min(stats.minimum_depth, depth);
  stats.maximum_depth = std::max(stats.maximum_depth, depth);
  for (const auto point : projected) {
    includeProjected(stats, point);
  }
}

void renderBox(double minimum_x, double minimum_y, double minimum_z,
               double maximum_x, double maximum_y, double maximum_z,
               VertexColor color, const MATRIX &view,
               std::vector<OT_TAG> &ordering_table, PrimitiveBuffer &primitives,
               RenderStats &stats) {
  std::array vertices{
      makeVertex(minimum_x, maximum_y, minimum_z),
      makeVertex(maximum_x, maximum_y, minimum_z),
      makeVertex(minimum_x, maximum_y, maximum_z),
      makeVertex(maximum_x, maximum_y, maximum_z),
      makeVertex(minimum_x, minimum_y, minimum_z),
      makeVertex(maximum_x, minimum_y, minimum_z),
      makeVertex(minimum_x, minimum_y, maximum_z),
      makeVertex(maximum_x, minimum_y, maximum_z),
  };
  constexpr std::array faces{
      std::array{0U, 1U, 2U}, std::array{1U, 3U, 2U}, std::array{4U, 6U, 5U},
      std::array{5U, 6U, 7U}, std::array{0U, 4U, 1U}, std::array{1U, 4U, 5U},
      std::array{2U, 3U, 6U}, std::array{3U, 7U, 6U}, std::array{0U, 2U, 4U},
      std::array{2U, 6U, 4U}, std::array{1U, 5U, 3U}, std::array{3U, 5U, 7U},
  };
  for (std::size_t index = 0; index < faces.size(); ++index) {
    const auto &face = faces[index];
    if (cameraDepth(view, vertices[face[0]]) < active_near_clip_depth ||
        cameraDepth(view, vertices[face[1]]) < active_near_clip_depth ||
        cameraDepth(view, vertices[face[2]]) < active_near_clip_depth) {
      ++stats.rejected;
      continue;
    }
    auto &primitive = primitives.player.emplace_back();
    setPolyF3(&primitive);
    const auto shade = static_cast<unsigned int>(3U + (index & 1U));
    setRGB0(&primitive, static_cast<u_char>(color.red * shade / 4U),
            static_cast<u_char>(color.green * shade / 4U),
            static_cast<u_char>(color.blue * shade / 4U));
    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&vertices[face[0]], &vertices[face[1]],
                      &vertices[face[2]], &xy0, &xy1, &xy2, &depth_cue, &flags);
    if (depth <= 0) {
      primitives.player.pop_back();
      ++stats.rejected;
      continue;
    }
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
            &primitive);
    const std::array projected{xy0, xy1, xy2};
    submitStats(stats, depth, projected);
  }
}

void renderPlayer(const game::PlayerState &player, const MATRIX &view,
                  std::vector<OT_TAG> &ordering_table,
                  PrimitiveBuffer &primitives, RenderStats &stats) {
  const auto box = [&](double left, double top, double front, double right,
                       double bottom, double back, VertexColor color) {
    renderBox(player.x + left, player.y + top, player.z + front,
              player.x + right, player.y + bottom, player.z + back, color, view,
              ordering_table, primitives, stats);
  };
  box(-48.0, -175.0, -34.0, -8.0, -8.0, 34.0, VertexColor{24, 34, 48});
  box(8.0, -175.0, -34.0, 48.0, -8.0, 34.0, VertexColor{24, 34, 48});
  box(-62.0, -340.0, -38.0, 62.0, -170.0, 38.0, VertexColor{28, 68, 108});
  box(-92.0, -325.0, -28.0, -65.0, -180.0, 28.0, VertexColor{24, 58, 94});
  box(65.0, -325.0, -28.0, 92.0, -180.0, 28.0, VertexColor{24, 58, 94});
  box(-38.0, -425.0, -38.0, 38.0, -345.0, 38.0, VertexColor{168, 112, 80});
}

assets::MissionTransform playerTransform(const game::PlayerState &player,
                                         std::int32_t model_heading) {
  const auto basis = game::headingBasis(model_heading);
  const auto fixed = [](double value) {
    return static_cast<std::int16_t>(std::lround(value * 4096.0));
  };
  return assets::MissionTransform{
      std::array<std::int16_t, 9>{
          fixed(basis.right.x),
          0,
          fixed(basis.forward.x),
          0,
          4096,
          0,
          fixed(basis.right.z),
          0,
          fixed(basis.forward.z),
      },
      static_cast<std::int32_t>(std::lround(player.x)),
      static_cast<std::int32_t>(std::lround(-player.y)),
      static_cast<std::int32_t>(std::lround(player.z)),
  };
}

void renderPresentedPlayer(
    const game::GameplaySession &gameplay,
    const game::ActorAnimationBank &actor_animations,
    std::uint64_t player_animation_tick, game::WeaponId presented_weapon,
    double interpolation_amount, const RenderPresentationSnapshot &presentation,
    const MATRIX &view, std::vector<OT_TAG> &ordering_table,
    std::vector<OT_TAG> &shadow_ordering_table,
    ActorShadowPresentationCache &shadow_cache, HmdPoseScratch &pose_scratch,
    PrimitiveBuffer &primitives, RenderStats &stats) {
  if (gameplay.playerAlive() && presentation.first_person_aim) {
    return;
  }
  const auto *guest_player =
      presentation.legacy_player ? &*presentation.legacy_player : nullptr;
  if (const auto *player_model =
          std::get_if<assets::HmdModel>(&gameplay.playerModel().geometry)) {
    auto player_object = game::SceneObject{
        0,
        playerTransform(presentation.player, presentation.player_model_heading),
    };
    if (guest_player != nullptr) {
      player_object = *guest_player;
    }
    const auto pose = [&] {
      if (!gameplay.playerAlive()) {
        return actor_animations.npcPose(
            game::NpcAnimationRequest{
                game::NpcAnimationAction::death,
                game::weaponStance(gameplay.hud().inventory().current()),
                0U,
            },
            gameplay.playerDeathAnimationTick());
      }
      return actor_animations.playerPose(
          gameplay.playerAnimation(), player_animation_tick,
          gameplay.playerPresentationAnimationTick());
    }();
    const auto *resolved_pose = prepareHmdWorldVertices(
        *player_model, player_object, &pose, pose_scratch,
        primitives.hmd_world_vertex_scratch);
    const auto world_vertices =
        std::span<const Vector3>{primitives.hmd_world_vertex_scratch};
    auto support_player_object = game::SceneObject{
        0, playerTransform(gameplay.player(), gameplay.playerModelHeading())};
    if (const auto *authoritative_guest_player =
            gameplay.legacyPlayerPresentation()) {
      support_player_object = *authoritative_guest_player;
    }
    const auto support_origin =
        transformPoint(0.0, 0.0, 0.0, support_player_object.transform);
    constexpr auto player_shadow_key =
        std::numeric_limits<std::uint64_t>::max();
    const auto weapon_presentation =
        gameplay.playerAlive()
            ? prepareActorWeaponPresentation(
                  gameplay, *player_model, player_object, pose,
                  presented_weapon, pose_scratch, resolved_pose)
            : std::nullopt;
    prepareActorShadowGeometry(
        *player_model, world_vertices,
        weapon_presentation ? &*weapon_presentation : nullptr, primitives);
    renderActorShadow(
        gameplay, *player_model, player_object,
        primitives.actor_shadow_world_vertex_scratch,
        primitives.actor_shadow_triangle_scratch,
        weapon_presentation ? weapon_presentation->geometry : nullptr,
        {support_origin.x, support_origin.y, support_origin.z},
        player_shadow_key, presentation.guest_frame, interpolation_amount,
        shadow_cache, view, shadow_ordering_table, primitives, stats);
    const auto visibility =
        gameplay.playerAlive() && !gameplay.cinematic()
            ? playerCameraVisibility(presentation.camera, presentation.player.x,
                                     presentation.player.y,
                                     presentation.player.z)
            : PlayerCameraVisibility::opaque;
    const auto translucent = visibility == PlayerCameraVisibility::translucent;
    if (visibility != PlayerCameraVisibility::hidden) {
      renderHmdObject(*player_model, player_object, &pose,
                      game::resident_hmd_texture_bank, pose_scratch, view,
                      ordering_table, primitives, stats, std::nullopt,
                      world_vertices, translucent);
    }
    if (gameplay.playerAlive() &&
        visibility != PlayerCameraVisibility::hidden && weapon_presentation) {
      renderActorWeapon(*weapon_presentation, player_object,
                        game::resident_weapon_texture_bank, presentation.camera,
                        view, ordering_table, primitives, stats, translucent);
    }
  } else if (!gameplay.legacyRenderCommandsAuthoritative()) {
    renderPlayer(presentation.player, view, ordering_table, primitives, stats);
  }
}

[[nodiscard]] constexpr std::int16_t packedScreenX(std::uint32_t word) {
  return static_cast<std::int16_t>(word & 0xffffU);
}

[[nodiscard]] constexpr std::int16_t packedScreenY(std::uint32_t word) {
  return static_cast<std::int16_t>(word >> 16U);
}

[[nodiscard]] constexpr double
reprojectGuestCoordinate(double guest_coordinate, double draw_offset,
                         double presented_minus_guest) noexcept {
  return guest_coordinate + draw_offset + presented_minus_guest;
}

// Raw list coordinates are captured before retail's GPU draw offset. The
// active interface/world contexts use centre-origin XY (the SVD scope spans
// exactly -155..155), then DR_ENV adds the 160,120 PS1 centre. Rebuild that
// state with the native 192,120 centre before applying an optional camera
// reprojection delta. Omitting it pinned scopes and glass fragments to the
// upper left while the sight ray correctly passed through the native centre.
static_assert(reprojectGuestCoordinate(0.0, guest_draw_offset_x, 0.0) == 192.0);
static_assert(reprojectGuestCoordinate(-155.0, guest_draw_offset_x, 0.0) ==
              37.0);
static_assert(reprojectGuestCoordinate(0.0, guest_draw_offset_y, 0.0) == 120.0);

void setPacketColor(std::uint32_t word, std::uint8_t &red, std::uint8_t &green,
                    std::uint8_t &blue) {
  red = static_cast<std::uint8_t>(word);
  green = static_cast<std::uint8_t>(word >> 8U);
  blue = static_cast<std::uint8_t>(word >> 16U);
}

void renderGuestCameraLists(
    const game::GameplaySession &gameplay, const TextureStreamer &textures,
    const RenderPresentationSnapshot &presentation,
    std::span<const game::LegacyGuestSpritePresentationState> retained_sprites,
    std::span<const game::LegacyGuestRawPacketPresentationState>
        retained_raw_packets,
    const MATRIX &view, std::vector<OT_TAG> &ordering_table,
    std::vector<OT_TAG> &overlay_ordering_table, PrimitiveBuffer &primitives,
    RenderStats &stats) {
  // Texture residency follows the native camera, while projected guest
  // packets retain the exact retail camera which created their XY values.
  // Mixing those two cameras near a bank portal either offsets effects or
  // selects a texture alias which TextureStreamer did not make resident.
  const auto residency_camera = gameplay.camera();
  const auto &packet_camera = presentation.guest_packet_camera;
  const auto texture_bank =
      gameplay.textureBankAt(residency_camera.x, residency_camera.z);
  const auto guest_effects_authoritative =
      game::legacyGuestEffectsAuthoritative(
          presentation.guest_camera_lists_captured,
          gameplay.legacyEffectParticlesAuthoritative());
  constexpr auto center_x = static_cast<double>(screen_width) * 0.5;
  constexpr auto center_y = static_cast<double>(screen_height) * 0.5;
  const auto project_world = [](const game::CameraState &camera,
                                const game::LegacyNativePoint &point)
      -> std::optional<std::array<double, 3U>> {
    const auto basis = viewBasis(camera);
    const auto delta = Vector3{static_cast<double>(point.x) - camera.x,
                               -static_cast<double>(point.y) - camera.y,
                               static_cast<double>(point.z) - camera.z};
    const auto component = [&delta](const Vector3 &axis) {
      return delta.x * axis.x + delta.y * axis.y + delta.z * axis.z;
    };
    const auto depth = component(basis[2]);
    if (depth <= active_near_clip_depth) {
      return std::nullopt;
    }
    const auto projection = static_cast<double>(std::max(camera.projection, 1));
    return std::array{
        center_x + component(basis[0]) * projection / depth,
        center_y + component(basis[1]) * projection / depth,
        depth,
    };
  };

  const auto bridge_camera = [](const game::LegacyCameraBridgeState &camera) {
    return game::CameraState{
        static_cast<double>(camera.eye.x),
        static_cast<double>(camera.eye.y),
        static_cast<double>(camera.eye.z),
        static_cast<double>(camera.target.x),
        static_cast<double>(camera.target.y),
        static_cast<double>(camera.target.z),
        camera.projectionForDisplayWidth(screen_width),
    };
  };
  const auto retained_sprite = [&](const auto &sprite) {
    return std::ranges::find_if(retained_sprites, [&](const auto &entry) {
             return sprite.source_address != 0U
                        ? entry.sprite.source_address == sprite.source_address
                        : entry.sprite.source_address == 0U &&
                              entry.sprite.effect_particle ==
                                  sprite.effect_particle;
           }) != retained_sprites.end();
  };

  const auto current_sprite_count = presentation.guest_sprites.size();
  for (auto index = std::size_t{};
       index < current_sprite_count + retained_sprites.size(); ++index) {
    const auto *retained_entry =
        index < current_sprite_count
            ? nullptr
            : &retained_sprites[index - current_sprite_count];
    const auto &sprite = retained_entry != nullptr
                             ? retained_entry->sprite
                             : presentation.guest_sprites[index];
    if ((sprite.attribute & 0x80000000U) != 0U || sprite.width == 0U ||
        sprite.height == 0U) {
      continue;
    }
    if (sprite.effect_family != 0U && !guest_effects_authoritative) {
      // A retained SPFX packet may outlive an incomplete guest frame. Let
      // native reconstruction own that frame instead of drawing stale and
      // fallback particles together.
      continue;
    }
    const auto world_effect = game::legacyGuestSpriteUsesWorldDepth(sprite);
    if (retained_entry == nullptr && world_effect && retained_sprite(sprite)) {
      continue;
    }
    if (!game::legacyGuestCameraItemVisibleWithNativeFirstPerson(
            presentation.first_person_aim, world_effect)) {
      continue;
    }
    auto &primitive = primitives.guest_sprites.emplace_back();
    setPolyFT4(&primitive);
    const auto sprite_color = sprite.force_fullbright
                                  ? game::LegacyRgbBridgeState{128U, 128U, 128U}
                                  : sprite.color;
    setRGB0(&primitive, sprite_color.red, sprite_color.green,
            sprite_color.blue);
    if ((sprite.attribute & 0x40000000U) != 0U) {
      setSemiTrans(&primitive, 1);
    }
    setShadeTex(
        &primitive,
        sprite.force_fullbright || (sprite.attribute & 0x40U) != 0U ? 1 : 0);

    const auto sort_transform = game::legacyGuestSpriteSortTransform(
        sprite, retained_entry != nullptr
                    ? retained_entry->renderer_fast_path
                    : presentation.renderer_sprite_fast_path);
    // PsyQ GsSPRITE.rotate stores degrees in Q12. GsSortSprite converts it
    // to the 4096-unit GTE circle by dividing the raw value by 360.
    const auto angle = sort_transform.angle_units *
                       (2.0 * std::numbers::pi /
                        static_cast<double>(game::heading_angle_units));
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    auto sprite_center_x = center_x + sprite.x;
    auto sprite_center_y = center_y + sprite.y;
    std::optional<std::array<double, 3U>> projected_effect;
    if (world_effect) {
      const auto authored_camera = retained_entry != nullptr
                                       ? bridge_camera(retained_entry->camera)
                                       : packet_camera;
      const auto authored_effect =
          project_world(authored_camera, sprite.effect_position);
      projected_effect =
          project_world(presentation.camera, sprite.effect_position);
      if (!authored_effect || !projected_effect) {
        ++stats.rejected;
        continue;
      }
      sprite_center_x += (*projected_effect)[0] - (*authored_effect)[0];
      sprite_center_y += (*projected_effect)[1] - (*authored_effect)[1];
    }
    const auto transform = [&](double local_x, double local_y) {
      local_x *= sort_transform.scale_x;
      local_y *= sort_transform.scale_y;
      return std::array{
          sprite_center_x + local_x * cosine - local_y * sine,
          sprite_center_y + local_x * sine + local_y * cosine,
      };
    };
    const auto first =
        transform(sort_transform.local_left, sort_transform.local_top);
    const auto second =
        transform(sort_transform.local_right, sort_transform.local_top);
    const auto third =
        transform(sort_transform.local_left, sort_transform.local_bottom);
    const auto fourth =
        transform(sort_transform.local_right, sort_transform.local_bottom);
    setXY4(&primitive, static_cast<float>(first[0]),
           static_cast<float>(first[1]), static_cast<float>(second[0]),
           static_cast<float>(second[1]), static_cast<float>(third[0]),
           static_cast<float>(third[1]), static_cast<float>(fourth[0]),
           static_cast<float>(fourth[1]));

    const auto horizontal_flip = (sprite.attribute & 0x800000U) != 0U;
    const auto vertical_flip = (sprite.attribute & 0x400000U) != 0U;
    const auto last_u = static_cast<std::uint8_t>(
        static_cast<unsigned int>(sprite.u) + sprite.width - 1U);
    const auto last_v = static_cast<std::uint8_t>(
        static_cast<unsigned int>(sprite.v) + sprite.height - 1U);
    const auto first_u = horizontal_flip ? last_u : sprite.u;
    const auto second_u = horizontal_flip ? sprite.u : last_u;
    const auto first_v = vertical_flip ? last_v : sprite.v;
    const auto second_v = vertical_flip ? sprite.v : last_v;
    auto material_u0 = first_u;
    auto material_u1 = second_u;
    auto material_v0 = first_v;
    auto material_v1 = second_v;

    const auto source_page = static_cast<unsigned int>(sprite.tpage & 0x1fU);
    const auto retail_tpage = static_cast<std::uint16_t>(
        source_page | ((sprite.attribute >> 17U) & 0x180U) |
        ((sprite.attribute >> 23U) & 0x60U));
    const auto retail_clut = static_cast<std::uint16_t>(
        (static_cast<unsigned int>(sprite.center_y) << 6U) |
        ((static_cast<unsigned int>(sprite.center_x) >> 4U) & 0x3fU));
    const auto special_material = textures.relocateGuestSprite(sprite);
    if (world_effect && sprite.effect_family != 0U && !special_material) {
      primitives.guest_sprites.pop_back();
      ++stats.rejected;
      continue;
    }
    if (const auto &environment = special_material) {
      const auto u_delta = static_cast<int>(environment->u) - sprite.u;
      const auto v_delta = static_cast<int>(environment->v) - sprite.v;
      material_u0 =
          static_cast<std::uint8_t>(static_cast<int>(material_u0) + u_delta);
      material_u1 =
          static_cast<std::uint8_t>(static_cast<int>(material_u1) + u_delta);
      material_v0 =
          static_cast<std::uint8_t>(static_cast<int>(material_v0) + v_delta);
      material_v1 =
          static_cast<std::uint8_t>(static_cast<int>(material_v1) + v_delta);
      primitive.tpage = static_cast<std::uint16_t>(
          (environment->texture_page & 0xfc1fU) | (retail_tpage & 0x03e0U));
      primitive.clut = environment->clut;
    } else {
      primitive.tpage =
          relocateTexturePage(retail_tpage, texture_bank, source_page);
      primitive.clut =
          relocateClut(retail_clut, sprite.tpage, texture_bank, source_page);
    }
    setUV4(&primitive, material_u0, material_v0, material_u1, material_v0,
           material_u0, material_v1, material_u1, material_v1);
    const auto depth =
        projected_effect
            ? std::clamp(
                  static_cast<int>(std::lround((*projected_effect)[2] * 0.25)),
                  1, ordering_table_size - 1)
            : static_cast<int>(std::clamp<std::uint32_t>(
                  sprite.ordering_depth, 1U, ordering_table_size - 1U));
    auto *target_table = &overlay_ordering_table;
    if (world_effect) {
      const auto view_depth =
          preciseCameraDepth(view, sprite.effect_position.x,
                             -static_cast<double>(sprite.effect_position.y),
                             sprite.effect_position.z);
      if (view_depth <= 0.0) {
        primitives.guest_sprites.pop_back();
        ++stats.rejected;
        continue;
      }
      const auto projection =
          static_cast<float>(std::max(presentation.camera.projection, 1));
      const auto pgxp_depth = static_cast<float>(view_depth / 128.0);
      const auto emit = [&](VERTTYPE &x, VERTTYPE &y, const auto &point) {
        PGXPVData data{};
        data.lookup = PGXP_LOOKUP_VALUE(x, y);
        data.px = static_cast<float>((point[0] - center_x) * view_depth /
                                     projection / 128.0);
        data.py = static_cast<float>((point[1] - center_y) * view_depth /
                                     projection / 128.0);
        data.pz = pgxp_depth;
        data.sx = static_cast<float>(point[0]);
        data.sy = static_cast<float>(point[1]);
        data.scr_h = projection;
        data.ofx = static_cast<float>(center_x);
        data.ofy = static_cast<float>(center_y);
        static_cast<void>(PGXP_EmitCacheData(&data));
      };
      emit(primitive.x0, primitive.y0, first);
      emit(primitive.x1, primitive.y1, second);
      emit(primitive.x2, primitive.y2, third);
      emit(primitive.x3, primitive.y3, fourth);
      target_table = &ordering_table;
    }
    addPrim(&(*target_table)[depth], &primitive);
    ++stats.submitted;
  }

  for (const auto &line : presentation.guest_lines) {
    if ((line.attribute & 0x80000000U) != 0U) {
      continue;
    }
    // This camera list has no effect-pool/world provenance. Exact LINE_G2
    // particles are reconstructed separately from current_line_particles.
    if (!game::legacyGuestCameraItemVisibleWithNativeFirstPerson(
            presentation.first_person_aim, false)) {
      continue;
    }
    auto &primitive = primitives.guest_lines.emplace_back();
    setLineG2(&primitive);
    setRGB0(&primitive, line.first_color.red, line.first_color.green,
            line.first_color.blue);
    setRGB1(&primitive, line.second_color.red, line.second_color.green,
            line.second_color.blue);
    setXY2(&primitive, static_cast<float>(line.first.x + screen_width / 2),
           static_cast<float>(line.first.y + screen_height / 2),
           static_cast<float>(line.second.x + screen_width / 2),
           static_cast<float>(line.second.y + screen_height / 2));
    if ((line.attribute & 0x40000000U) != 0U) {
      setSemiTrans(&primitive, 1);
    }
    auto &mode = primitives.guest_line_modes.emplace_back();
    setDrawTPage(&mode, 1, 1,
                 static_cast<int>((line.attribute >> 23U) & 0x60U));
    // GsSortGLine emits draw-mode then LINE_G2 as one packet at priority 1.
    // addPrim prepends, therefore link the line first and mode second.
    addPrim(&overlay_ordering_table[1], &primitive);
    addPrim(&overlay_ordering_table[1], &mode);
    ++stats.submitted;
  }

  const auto add_raw = [&](auto &primitive, std::uint32_t depth,
                           std::uint8_t opcode,
                           std::vector<OT_TAG> &target_table) {
    // FUN_800c84f4 accepts priority zero. Preserve it; only the upper bound is
    // saturated to the active OT length.
    auto &ot = target_table[static_cast<std::size_t>(
        game::legacyGuestRawPacketOtIndex(depth, ordering_table_size))];
    addPrim(&ot, &primitive);
    if (game::legacyGuestRawPacketNeedsDrawMode(opcode)) {
      auto &mode = primitives.guest_raw_modes.emplace_back();
      setDrawTPage(&mode, 1, 1, GetTPage(2, 0, 0, 0));
      // FUN_800c84f4 links the raw packet first, then prepends DR_TPAGE at the
      // same OT priority so the draw mode executes immediately before it.
      addPrim(&ot, &mode);
    }
    ++stats.submitted;
  };
  const auto render_raw_packet = [&](const game::LegacyGuestRawPacketBridgeState
                                         &packet,
                                     const game::CameraState &authored_camera,
                                     bool suppress_reconstructed_particle) {
    if (suppress_reconstructed_particle &&
        (game::legacyGuestRawPacketHasWorldLine(
             packet, presentation.current_line_particles) ||
         game::legacyGuestRawPacketHasWorldCombatParticle(
             packet, presentation.current_combat_particles))) {
      return;
    }
    const auto base_opcode = packet.opcode & 0xfdU;
    if (packet.opcode == 0U || (packet.opcode & 0x80U) != 0U) {
      return;
    }
    const auto world_effect = game::legacyGuestRawPacketUsesWorldDepth(packet);
    const auto scope_packet =
        game::legacyGuestRawPacketIsRetailScopeOverlay(packet);
    // The fixed optic arrays remain linked in the interface renderer after
    // aim teardown. They are not generic camera packets: submit them only
    // while the retail interface state explicitly owns the active scope.
    if (scope_packet && !presentation.retail_scope_overlay) {
      return;
    }
    if (!game::legacyGuestCameraItemVisibleWithNativeFirstPerson(
            presentation.first_person_aim, world_effect || scope_packet)) {
      return;
    }
    // Effect-pool packets never fall back to the screen overlay. A malformed
    // provenance record was already rejected by the immutable bridge frame;
    // retaining this guard prevents future partial readers from pinning blood,
    // flashes or tracers to the guest reticle plane.
    if (packet.effect_particle >= 0 && !world_effect) {
      ++stats.rejected;
      return;
    }
    auto *target_table = &overlay_ordering_table;
    auto raw_view_depth = 0.0;
    auto raw_offset_x = 0.0;
    auto raw_offset_y = 0.0;
    if (world_effect) {
      const auto authored =
          project_world(authored_camera, packet.effect_position);
      const auto presented =
          project_world(presentation.camera, packet.effect_position);
      if (!authored || !presented) {
        ++stats.rejected;
        return;
      }
      raw_offset_x = (*presented)[0] - (*authored)[0];
      raw_offset_y = (*presented)[1] - (*authored)[1];
      raw_view_depth =
          preciseCameraDepth(view, packet.effect_position.x,
                             -static_cast<double>(packet.effect_position.y),
                             packet.effect_position.z);
      target_table = &ordering_table;
    }
    const auto raw_x = [raw_offset_x](std::uint32_t word) {
      return static_cast<float>(
          reprojectGuestCoordinate(static_cast<double>(packedScreenX(word)),
                                   guest_draw_offset_x, raw_offset_x));
    };
    const auto raw_y = [raw_offset_y](std::uint32_t word) {
      return static_cast<float>(
          reprojectGuestCoordinate(static_cast<double>(packedScreenY(word)),
                                   guest_draw_offset_y, raw_offset_y));
    };
    const auto raw_depth =
        world_effect ? static_cast<std::uint32_t>(std::clamp(
                           static_cast<int>(std::lround(raw_view_depth * 0.25)),
                           1, ordering_table_size - 1))
                     : packet.ordering_depth;
    const auto emit_raw_pgxp = [&](VERTTYPE &x, VERTTYPE &y) {
      if (!world_effect) {
        return;
      }
      const auto projection =
          static_cast<float>(std::max(presentation.camera.projection, 1));
      const auto screen_x = static_cast<float>(x);
      const auto screen_y = static_cast<float>(y);
      PGXPVData data{};
      data.lookup = PGXP_LOOKUP_VALUE(x, y);
      data.px = static_cast<float>((screen_x - center_x) * raw_view_depth /
                                   projection / 128.0);
      data.py = static_cast<float>((screen_y - center_y) * raw_view_depth /
                                   projection / 128.0);
      data.pz = static_cast<float>(raw_view_depth / 128.0);
      data.sx = screen_x;
      data.sy = screen_y;
      data.scr_h = projection;
      data.ofx = static_cast<float>(center_x);
      data.ofy = static_cast<float>(center_y);
      static_cast<void>(PGXP_EmitCacheData(&data));
    };
    if (packet.word_count == 2U && base_opcode == 0x68U) {
      auto &primitive = primitives.guest_raw_tiles.emplace_back();
      setTile1(&primitive);
      setPacketColor(packet.words[0], primitive.r0, primitive.g0, primitive.b0);
      primitive.code = packet.opcode;
      setXY0(&primitive, raw_x(packet.words[1]), raw_y(packet.words[1]));
      emit_raw_pgxp(primitive.x0, primitive.y0);
      add_raw(primitive, raw_depth, packet.opcode, *target_table);
    } else if (packet.word_count == 3U && base_opcode == 0x40U) {
      auto &primitive = primitives.guest_raw_flat_lines.emplace_back();
      setLineF2(&primitive);
      setPacketColor(packet.words[0], primitive.r0, primitive.g0, primitive.b0);
      primitive.code = packet.opcode;
      setXY2(&primitive, raw_x(packet.words[1]), raw_y(packet.words[1]),
             raw_x(packet.words[2]), raw_y(packet.words[2]));
      emit_raw_pgxp(primitive.x0, primitive.y0);
      emit_raw_pgxp(primitive.x1, primitive.y1);
      add_raw(primitive, raw_depth, packet.opcode, *target_table);
    } else if (packet.word_count == 4U && base_opcode == 0x20U) {
      auto &primitive = primitives.guest_raw_flat_triangles.emplace_back();
      setPolyF3(&primitive);
      setPacketColor(packet.words[0], primitive.r0, primitive.g0, primitive.b0);
      primitive.code = packet.opcode;
      setXY3(&primitive, raw_x(packet.words[1]), raw_y(packet.words[1]),
             raw_x(packet.words[2]), raw_y(packet.words[2]),
             raw_x(packet.words[3]), raw_y(packet.words[3]));
      emit_raw_pgxp(primitive.x0, primitive.y0);
      emit_raw_pgxp(primitive.x1, primitive.y1);
      emit_raw_pgxp(primitive.x2, primitive.y2);
      add_raw(primitive, raw_depth, packet.opcode, *target_table);
    } else if (packet.word_count == 4U && base_opcode == 0x50U) {
      auto &primitive = primitives.guest_raw_gouraud_lines.emplace_back();
      setLineG2(&primitive);
      setPacketColor(packet.words[0], primitive.r0, primitive.g0, primitive.b0);
      primitive.code = packet.opcode;
      setXY0(&primitive, raw_x(packet.words[1]), raw_y(packet.words[1]));
      setPacketColor(packet.words[2], primitive.r1, primitive.g1, primitive.b1);
      primitive.x1 = raw_x(packet.words[3]);
      primitive.y1 = raw_y(packet.words[3]);
      emit_raw_pgxp(primitive.x0, primitive.y0);
      emit_raw_pgxp(primitive.x1, primitive.y1);
      add_raw(primitive, raw_depth, packet.opcode, *target_table);
    } else if (packet.word_count == 5U && base_opcode == 0x28U) {
      auto &primitive = primitives.guest_raw_flat_quads.emplace_back();
      setPolyF4(&primitive);
      setPacketColor(packet.words[0], primitive.r0, primitive.g0, primitive.b0);
      primitive.code = packet.opcode;
      setXY4(&primitive, raw_x(packet.words[1]), raw_y(packet.words[1]),
             raw_x(packet.words[2]), raw_y(packet.words[2]),
             raw_x(packet.words[3]), raw_y(packet.words[3]),
             raw_x(packet.words[4]), raw_y(packet.words[4]));
      emit_raw_pgxp(primitive.x0, primitive.y0);
      emit_raw_pgxp(primitive.x1, primitive.y1);
      emit_raw_pgxp(primitive.x2, primitive.y2);
      emit_raw_pgxp(primitive.x3, primitive.y3);
      add_raw(primitive, raw_depth, packet.opcode, *target_table);
    } else if (packet.word_count == 6U && base_opcode == 0x30U) {
      auto &primitive = primitives.guest_raw_gouraud_triangles.emplace_back();
      setPolyG3(&primitive);
      setPacketColor(packet.words[0], primitive.r0, primitive.g0, primitive.b0);
      primitive.code = packet.opcode;
      setXY0(&primitive, raw_x(packet.words[1]), raw_y(packet.words[1]));
      setPacketColor(packet.words[2], primitive.r1, primitive.g1, primitive.b1);
      primitive.x1 = raw_x(packet.words[3]);
      primitive.y1 = raw_y(packet.words[3]);
      setPacketColor(packet.words[4], primitive.r2, primitive.g2, primitive.b2);
      primitive.x2 = raw_x(packet.words[5]);
      primitive.y2 = raw_y(packet.words[5]);
      emit_raw_pgxp(primitive.x0, primitive.y0);
      emit_raw_pgxp(primitive.x1, primitive.y1);
      emit_raw_pgxp(primitive.x2, primitive.y2);
      add_raw(primitive, raw_depth, packet.opcode, *target_table);
    }
  };
  const auto retained = [&](const auto &packet) {
    return std::ranges::any_of(retained_raw_packets, [&](const auto &entry) {
      return packet.source_address != 0U
                 ? entry.packet.source_address == packet.source_address
                 : entry.packet.source_address == 0U &&
                       entry.packet.effect_particle == packet.effect_particle;
    });
  };
  for (const auto &packet : presentation.guest_raw_packets) {
    if (game::legacyGuestRawPacketUsesWorldDepth(packet) && retained(packet)) {
      continue;
    }
    render_raw_packet(packet, packet_camera, true);
  }
  for (const auto &entry : retained_raw_packets) {
    const auto &camera = entry.camera;
    const auto authored_camera = game::CameraState{
        static_cast<double>(camera.eye.x),
        static_cast<double>(camera.eye.y),
        static_cast<double>(camera.eye.z),
        static_cast<double>(camera.target.x),
        static_cast<double>(camera.target.y),
        static_cast<double>(camera.target.z),
        camera.projectionForDisplayWidth(screen_width),
    };
    // Source-tick semantic duplicates were discarded by the queue. A newer
    // guest tick may already have reused the same particle index, so it must
    // not suppress this retained retail packet.
    render_raw_packet(entry.packet, authored_camera, false);
  }
}

std::optional<Vector3>
dynamicEffectCentre(const game::GameplaySession &gameplay,
                    const game::ActorAnimationBank &actor_animations,
                    const RenderPresentationSnapshot &presentation,
                    const game::GameplayEffect &effect) {
  if (effect.attachment == game::GameplayEffectAttachment::world) {
    return Vector3{effect.x, effect.y, effect.z};
  }
  if (effect.attachment == game::GameplayEffectAttachment::player_muzzle) {
    return playerWeaponMuzzleAnchor(gameplay, actor_animations,
                                    presentedPlayerObject(presentation),
                                    gameplay.hud().inventory().current());
  }
  if (effect.attachment != game::GameplayEffectAttachment::npc_muzzle ||
      effect.owner_object >= presentation.objects.size()) {
    return std::nullopt;
  }
  const auto *state = gameplay.npcState(effect.owner_object);
  if (state == nullptr) {
    return std::nullopt;
  }
  return npcWeaponMuzzleAnchor(gameplay, actor_animations,
                               presentation.objects[effect.owner_object],
                               effect.owner_object, state->weapon);
}

game::RetailVertexLightState
retailVertexLightState(const game::LegacyVertexLightBridgeState &source) {
  auto light = game::RetailVertexLightState{};
  light.matrix.rotation = source.matrix.rotation;
  light.matrix.translation = {
      source.matrix.translation.x,
      source.matrix.translation.y,
      source.matrix.translation.z,
  };
  light.flags = source.flags;
  light.extent = source.shape;
  light.screen_shift = source.screen_shift;
  light.depth_shift = source.depth_shift;
  light.threshold = source.threshold;
  light.channel_mask = source.channel_mask;
  return light;
}

struct FlashlightPresentationRay {
  Vector3 origin;
  Vector3 direction;
};

struct FlashlightWorldSurfaceHit {
  game::DynamicLightSurfaceHit surface;
  std::array<Vector3, 4U> receiver;
  std::size_t receiver_count{};
};

std::optional<FlashlightWorldSurfaceHit>
flashlightWorldSurfaceHit(const game::GameplaySession &gameplay,
                          const RenderPresentationSnapshot &presentation,
                          const FlashlightPresentationRay &ray) {
  constexpr double maximum_flashlight_distance = 2800.0;
  auto nearest = std::optional<FlashlightWorldSurfaceHit>{};
  const auto intersects = [&](const game::DynamicLightBounds &bounds) {
    return game::dynamicLightSegmentIntersectsBounds(
        {ray.origin.x, ray.origin.y, ray.origin.z},
        {ray.direction.x, ray.direction.y, ray.direction.z},
        maximum_flashlight_distance, bounds);
  };
  const auto raw_bounds = [](const assets::EmdBounds &bounds) {
    return game::DynamicLightBounds{
        {static_cast<double>(bounds.minimum_x),
         static_cast<double>(bounds.minimum_y),
         static_cast<double>(bounds.minimum_z)},
        {static_cast<double>(bounds.maximum_x),
         static_cast<double>(bounds.maximum_y),
         static_cast<double>(bounds.maximum_z)},
    };
  };
  const auto transformed_bounds =
      [](const assets::EmdBounds &bounds,
         const assets::MissionTransform &transform) {
        auto result = game::DynamicLightBounds{
            {std::numeric_limits<double>::max(),
             std::numeric_limits<double>::max(),
             std::numeric_limits<double>::max()},
            {std::numeric_limits<double>::lowest(),
             std::numeric_limits<double>::lowest(),
             std::numeric_limits<double>::lowest()},
        };
        for (const auto x : {bounds.minimum_x, bounds.maximum_x}) {
          for (const auto y : {bounds.minimum_y, bounds.maximum_y}) {
            for (const auto z : {bounds.minimum_z, bounds.maximum_z}) {
              const auto point = transformPoint(x, y, z, transform);
              result.minimum.x = std::min(result.minimum.x, point.x);
              result.minimum.y = std::min(result.minimum.y, point.y);
              result.minimum.z = std::min(result.minimum.z, point.z);
              result.maximum.x = std::max(result.maximum.x, point.x);
              result.maximum.y = std::max(result.maximum.y, point.y);
              result.maximum.z = std::max(result.maximum.z, point.z);
            }
          }
        }
        return result;
      };
  const auto consider = [&](const std::array<Vector3, 4U> &receiver,
                            std::size_t receiver_count, std::size_t first,
                            std::size_t second, std::size_t third) {
    const auto hit = game::dynamicLightSurfaceHit(
        {ray.origin.x, ray.origin.y, ray.origin.z},
        {ray.direction.x, ray.direction.y, ray.direction.z},
        {{receiver[first].x, receiver[first].y, receiver[first].z},
         {receiver[second].x, receiver[second].y, receiver[second].z},
         {receiver[third].x, receiver[third].y, receiver[third].z}},
        maximum_flashlight_distance);
    if (hit && (!nearest || hit->distance < nearest->surface.distance)) {
      nearest = FlashlightWorldSurfaceHit{*hit, receiver, receiver_count};
    }
  };
  for (const auto model_index : gameplay.presentationModels()) {
    if (model_index >= gameplay.models().size()) {
      continue;
    }
    const auto &model = gameplay.models()[model_index];
    if (!intersects(raw_bounds(model.bounds))) {
      continue;
    }
    const auto &scene = model.scene;
    for (const auto &section : scene.sections()) {
      if (!intersects(raw_bounds(section.bounds))) {
        continue;
      }
      for (const auto &polygon : section.polygons) {
        if (!polygon.renderable) {
          continue;
        }
        const auto required_vertices = polygon.quad ? 4U : 3U;
        auto indices_valid = true;
        for (auto corner = std::size_t{}; corner < required_vertices;
             ++corner) {
          indices_valid = indices_valid && polygon.vertex_indices[corner] <
                                               section.vertices.size();
        }
        if (!indices_valid) {
          continue;
        }
        auto receiver = std::array<Vector3, 4U>{};
        const auto source_corner =
            [&](std::size_t boundary_corner) -> std::size_t {
          if (!polygon.quad || boundary_corner < 2U) {
            return boundary_corner;
          }
          return boundary_corner == 2U ? 3U : 2U;
        };
        for (auto corner = std::size_t{}; corner < required_vertices;
             ++corner) {
          const auto &vertex =
              section.vertices[polygon.vertex_indices[source_corner(corner)]];
          receiver[corner] = {static_cast<double>(vertex.x),
                              static_cast<double>(vertex.y),
                              static_cast<double>(vertex.z)};
        }
        consider(receiver, required_vertices, 0U, 1U, polygon.quad ? 3U : 2U);
        if (polygon.quad) {
          consider(receiver, required_vertices, 1U, 2U, 3U);
        }
      }
    }
  }
  for (const auto object_index : gameplay.activeObjects()) {
    if (object_index >= presentation.objects.size()) {
      continue;
    }
    const auto &object = presentation.objects[object_index];
    const auto *displayed = gameplay.displayedObjectModel(object_index);
    if (displayed == nullptr ||
        displayed->visual_effect != game::ObjectVisualEffect::none) {
      continue;
    }
    if (const auto *model =
            std::get_if<assets::GmdModel>(&displayed->geometry)) {
      if (!intersects(transformed_bounds(model->bounds(), object.transform))) {
        continue;
      }
      for (const auto &triangle : model->triangles()) {
        if (triangle.flags == 0U || triangle.semi_transparent ||
            std::ranges::any_of(triangle.vertex_indices,
                                [&](std::uint8_t index) {
                                  return index >= model->vertices().size();
                                })) {
          continue;
        }
        auto receiver = std::array<Vector3, 4U>{};
        for (auto corner = std::size_t{}; corner < 3U; ++corner) {
          const auto &vertex =
              model->vertices()[triangle.vertex_indices[corner]];
          receiver[corner] =
              transformPoint(vertex.x, vertex.y, vertex.z, object.transform);
        }
        consider(receiver, 3U, 0U, 1U, 2U);
      }
    } else if (const auto *scene =
                   std::get_if<assets::EmdScene>(&displayed->geometry)) {
      if (displayed->bounds && !intersects(transformed_bounds(
                                   *displayed->bounds, object.transform))) {
        continue;
      }
      for (const auto &section : scene->sections()) {
        if (!intersects(transformed_bounds(section.bounds, object.transform))) {
          continue;
        }
        for (const auto &polygon : section.polygons) {
          if (!polygon.renderable) {
            continue;
          }
          const auto required_vertices = polygon.quad ? 4U : 3U;
          auto receiver = std::array<Vector3, 4U>{};
          auto valid = true;
          for (auto corner = std::size_t{}; corner < required_vertices;
               ++corner) {
            const auto source = !polygon.quad || corner < 2U
                                    ? corner
                                    : (corner == 2U ? 3U : 2U);
            const auto index = polygon.vertex_indices[source];
            if (index >= section.vertices.size()) {
              valid = false;
              break;
            }
            const auto &vertex = section.vertices[index];
            receiver[corner] =
                transformPoint(vertex.x, vertex.y, vertex.z, object.transform);
          }
          if (!valid) {
            continue;
          }
          consider(receiver, required_vertices, 0U, 1U, polygon.quad ? 3U : 2U);
          if (polygon.quad) {
            consider(receiver, required_vertices, 1U, 2U, 3U);
          }
        }
      }
    }
  }
  return nearest;
}

void renderFlashlightCone(const game::GameplaySession &gameplay,
                          const RenderPresentationSnapshot &presentation,
                          const FlashlightPresentationRay &ray,
                          const MATRIX &view,
                          std::vector<OT_TAG> &fire_ordering_table,
                          PrimitiveBuffer &primitives, RenderStats &stats) {
  constexpr double maximum_flashlight_distance = 2800.0;
  const auto hit = flashlightWorldSurfaceHit(gameplay, presentation, ray);
  const auto length =
      hit ? std::min(hit->surface.distance - 6.0, maximum_flashlight_distance)
          : maximum_flashlight_distance;
  if (!std::isfinite(length) || length <= 48.0) {
    return;
  }

  const auto direction = normalize(ray.direction);
  const auto reference = std::abs(direction.y) < 0.9 ? Vector3{0.0, 1.0, 0.0}
                                                     : Vector3{1.0, 0.0, 0.0};
  const auto tangent = normalize(cross(reference, direction));
  const auto bitangent = cross(direction, tangent);
  constexpr auto segment_count = std::size_t{12U};
  constexpr auto ring_count = std::size_t{4U};
  constexpr std::array<double, ring_count> axial_positions{0.018, 0.24, 0.58,
                                                           1.0};
  constexpr std::array<double, ring_count> axial_fade{0.75, 1.0, 0.55, 0.0};
  constexpr std::array<double, 3U> shell_scales{1.0, 0.68, 0.36};
  constexpr std::array<double, 3U> shell_brightness{4.0, 6.0, 9.0};
  constexpr double cone_tangent = 0.4040262258; // tan(22 degrees)

  const auto ring_point = [&](double distance, double radius,
                              std::size_t segment) {
    const auto angle = static_cast<double>(segment) * 2.0 * std::numbers::pi /
                       static_cast<double>(segment_count);
    const auto across = std::cos(angle) * radius;
    const auto down = std::sin(angle) * radius;
    return Vector3{
        ray.origin.x + direction.x * distance + tangent.x * across +
            bitangent.x * down,
        ray.origin.y + direction.y * distance + tangent.y * across +
            bitangent.y * down,
        ray.origin.z + direction.z * distance + tangent.z * across +
            bitangent.z * down,
    };
  };
  const auto projected_valid = [](long packed) {
    constexpr auto maximum_safe_coordinate = 8192.0F;
    const auto x = screenX(packed);
    const auto y = screenY(packed);
    return std::isfinite(x) && std::isfinite(y) &&
           std::abs(x) <= maximum_safe_coordinate &&
           std::abs(y) <= maximum_safe_coordinate;
  };
  const auto color = [](double brightness, double fade) {
    const auto channel = [&](double scale) {
      return static_cast<std::uint8_t>(
          std::clamp<long>(std::lround(brightness * fade * scale), 0L, 255L));
    };
    return VertexColor{channel(0.88), channel(0.94), channel(1.0)};
  };

  for (auto shell = std::size_t{}; shell < shell_scales.size(); ++shell) {
    for (auto ring = std::size_t{}; ring + 1U < ring_count; ++ring) {
      const auto first_distance =
          std::max(32.0, length * axial_positions[ring]);
      const auto second_distance = length * axial_positions[ring + 1U];
      if (second_distance <= first_distance) {
        continue;
      }
      const auto first_radius =
          first_distance * cone_tangent * shell_scales[shell];
      const auto second_radius =
          second_distance * cone_tangent * shell_scales[shell];
      const auto first_color = color(shell_brightness[shell], axial_fade[ring]);
      const auto second_color =
          color(shell_brightness[shell], axial_fade[ring + 1U]);
      for (auto segment = std::size_t{}; segment < segment_count; ++segment) {
        const auto next = (segment + 1U) % segment_count;
        const auto first = ring_point(first_distance, first_radius, segment);
        const auto first_next = ring_point(first_distance, first_radius, next);
        const auto second = ring_point(second_distance, second_radius, segment);
        const auto second_next =
            ring_point(second_distance, second_radius, next);
        auto v0 = makeVertex(first.x, first.y, first.z);
        auto v1 = makeVertex(first_next.x, first_next.y, first_next.z);
        auto v2 = makeVertex(second.x, second.y, second.z);
        auto v3 = makeVertex(second_next.x, second_next.y, second_next.z);
        const std::array positions{v0, v1, v2, v3};
        if (classifyNearPlane(view, positions) != NearPlaneStatus::inside) {
          ++stats.rejected;
          continue;
        }
        long xy0{};
        long xy1{};
        long xy2{};
        long xy3{};
        long depth_cue{};
        long flags{};
        const auto depth = RotTransPers4(&v0, &v1, &v2, &v3, &xy0, &xy1, &xy2,
                                         &xy3, &depth_cue, &flags);
        if (depth <= 0 || flags != 0L || !projected_valid(xy0) ||
            !projected_valid(xy1) || !projected_valid(xy2) ||
            !projected_valid(xy3)) {
          ++stats.rejected;
          continue;
        }
        auto &primitive = primitives.flashlight_cone_quads.emplace_back();
        setPolyG4(&primitive);
        setSemiTrans(&primitive, 1);
        setRGB0(&primitive, first_color.red, first_color.green,
                first_color.blue);
        setRGB1(&primitive, first_color.red, first_color.green,
                first_color.blue);
        setRGB2(&primitive, second_color.red, second_color.green,
                second_color.blue);
        setRGB3(&primitive, second_color.red, second_color.green,
                second_color.blue);
        setProjected(&primitive.x0, xy0);
        setProjected(&primitive.x1, xy1);
        setProjected(&primitive.x2, xy2);
        setProjected(&primitive.x3, xy3);
        addPrim(&fire_ordering_table[std::clamp(fireOrderingDepth(depth), 1,
                                                ordering_table_size - 1)],
                &primitive);
        const std::array projected{xy0, xy1, xy2, xy3};
        submitStats(stats, depth, projected);
      }
    }
  }
}

struct LegacyLightAccumulator {
  std::uint16_t controller{};
  std::int16_t source_slot{-1};
  game::LegacyEffectSpriteFamily family{
      game::LegacyEffectSpriteFamily::explosion};
  double x{};
  double y{};
  double z{};
  double maximum_scale{};
  std::uint8_t earliest_frame{0xffU};
  std::uint16_t count{};
};

struct DynamicLightFrameScratch {
  std::vector<game::PersistentDynamicLightState> persistent;
  std::vector<game::TransientDynamicLightState> transient;
  std::vector<LegacyLightAccumulator> legacy_lights;
  std::uint64_t guest_frame{std::numeric_limits<std::uint64_t>::max()};
  bool valid{};

  void reset() noexcept {
    persistent.clear();
    transient.clear();
    legacy_lights.clear();
    guest_frame = std::numeric_limits<std::uint64_t>::max();
    valid = false;
  }
};

void updateDynamicLightFrame(
    const game::GameplaySession &gameplay,
    const game::ActorAnimationBank &actor_animations,
    const RenderPresentationSnapshot &presentation,
    std::span<const game::LegacyWeaponEventBridgeState> weapon_edges,
    std::span<const game::LegacyMuzzleFlashPresentationState>
        retail_muzzle_flashes,
    std::span<const game::GameplayEffect> native_muzzle_flashes,
    DynamicLightFrameScratch &scratch) {
  // Retail lighting changes only on a 20 Hz guest update. Rebuilding and
  // sorting it on every 120/240 Hz presentation repeated object, particle and
  // muzzle scans up to twelve times for an identical scene state. Keeping the
  // complete light frame also makes a one-tick muzzle flash last equally long
  // at every configured presentation rate.
  if (scratch.valid && scratch.guest_frame == presentation.guest_frame) {
    return;
  }
  scratch.valid = true;
  scratch.guest_frame = presentation.guest_frame;
  active_retail_vertex_lights.clear();
  active_retail_vertex_lights.reserve(presentation.vertex_lights.size());
  for (const auto &source : presentation.vertex_lights) {
    active_retail_vertex_lights.push_back(retailVertexLightState(source));
  }
  active_retail_light_projection = std::max(presentation.camera.projection, 1);

  auto &persistent = scratch.persistent;
  persistent.clear();
  persistent.reserve(gameplay.activeObjects().size());
  for (const auto object_index : gameplay.activeObjects()) {
    if (object_index >= presentation.objects.size()) {
      continue;
    }
    const auto &object = presentation.objects[object_index];
    if (object.model >= gameplay.objectModels().size()) {
      continue;
    }
    const auto &model = gameplay.objectModels()[object.model];
    auto kind = std::optional<game::DynamicLightKind>{};
    if (model.visual_effect == game::ObjectVisualEffect::police_lightbar) {
      kind = game::DynamicLightKind::police_lightbar;
    } else if (std::holds_alternative<game::ObjectFireEmitter>(
                   model.geometry)) {
      kind = game::DynamicLightKind::steady_fire;
    } else if (object.damage_response ==
                   game::ObjectDamageResponse::extinguish ||
               model.visual_effect ==
                   game::ObjectVisualEffect::billboard_glow) {
      // All authored lamp families share the retail extinguish response. This
      // is a stronger identity than a guessed filename and automatically
      // keeps mission-specific lamps and their destruction latch in sync.
      kind = game::DynamicLightKind::street_lamp;
    }
    if (!kind) {
      continue;
    }
    auto centre = transformPoint(0.0, 0.0, 0.0, object.transform);
    if (model.bounds) {
      centre = transformPoint((static_cast<double>(model.bounds->minimum_x) +
                               model.bounds->maximum_x) *
                                  0.5,
                              (static_cast<double>(model.bounds->minimum_y) +
                               model.bounds->maximum_y) *
                                  0.5,
                              (static_cast<double>(model.bounds->minimum_z) +
                               model.bounds->maximum_z) *
                                  0.5,
                              object.transform);
    }
    persistent.push_back(game::PersistentDynamicLightState{
        *kind,
        {centre.x, centre.y, centre.z},
        object_index,
        true,
        true,
        gameplay.displayedObjectModel(object_index) != nullptr,
        gameplay.objectDestroyed(object_index),
    });
  }

  auto &transient = scratch.transient;
  transient.clear();
  transient.reserve(weapon_edges.size() + retail_muzzle_flashes.size() +
                    native_muzzle_flashes.size() + gameplay.effects().size() +
                    presentation.projectiles.size() +
                    gameplay.legacyExplParticles().size());

  // Retail SPFX particles are the authoritative explosion/fire presentation
  // once the guest renderer is active. Group the child sprites by controller
  // and recover one bounded light at their common centre; otherwise native
  // explosion effects are correctly suppressed but their illumination also
  // disappears.
  auto &legacy_lights = scratch.legacy_lights;
  legacy_lights.clear();
  legacy_lights.reserve(gameplay.legacyExplParticles().size());
  for (const auto &particle : gameplay.legacyExplParticles()) {
    if (particle.family != game::LegacyEffectSpriteFamily::explosion &&
        particle.family != game::LegacyEffectSpriteFamily::fire) {
      continue;
    }
    auto accumulator = std::ranges::find_if(
        legacy_lights, [&](const LegacyLightAccumulator &candidate) {
          return candidate.controller == particle.controller &&
                 candidate.source_slot == particle.source_slot &&
                 candidate.family == particle.family;
        });
    if (accumulator == legacy_lights.end()) {
      legacy_lights.push_back(LegacyLightAccumulator{
          particle.controller,
          particle.source_slot,
          particle.family,
      });
      accumulator = std::prev(legacy_lights.end());
    }
    accumulator->x += static_cast<double>(particle.x);
    accumulator->y += static_cast<double>(particle.y);
    accumulator->z += static_cast<double>(particle.z);
    accumulator->maximum_scale =
        std::max(accumulator->maximum_scale,
                 static_cast<double>(particle.scale_byte) / 57.0);
    accumulator->earliest_frame =
        std::min(accumulator->earliest_frame, particle.frame);
    ++accumulator->count;
  }
  for (const auto &source : legacy_lights) {
    if (source.count == 0U) {
      continue;
    }
    const auto layout = game::legacyEffectSpriteLayout(source.family);
    if (layout.frame_count == 0U ||
        source.earliest_frame >= layout.frame_count) {
      continue;
    }
    const auto fire = source.family == game::LegacyEffectSpriteFamily::fire;
    const auto total = static_cast<std::uint16_t>(layout.frame_count);
    const auto remaining =
        fire ? std::uint16_t{1U}
             : static_cast<std::uint16_t>(layout.frame_count -
                                          source.earliest_frame);
    const auto inverse_count = 1.0 / static_cast<double>(source.count);
    transient.push_back(game::TransientDynamicLightState{
        fire ? game::GameplayEffectType::burning_fire
             : game::GameplayEffectType::explosion,
        {source.x * inverse_count, source.y * inverse_count,
         source.z * inverse_count},
        static_cast<std::uint32_t>(
            0x20000000U ^
            (static_cast<std::uint32_t>(source.controller) << 8U) ^
            static_cast<std::uint16_t>(source.source_slot) ^
            static_cast<std::uint32_t>(source.family)),
        std::clamp(source.maximum_scale, 0.5, 3.0),
        remaining,
        fire ? std::uint16_t{1U} : total,
        true,
    });
  }
  for (const auto &edge : weapon_edges) {
    const auto weapon = static_cast<game::WeaponId>(edge.weapon);
    if (edge.type != game::LegacyWeaponEventType::shot ||
        !weaponUsesRetailMuzzleFlash(weapon)) {
      continue;
    }
    if (presentation.first_person_aim &&
        edge.actor_slot == presentation.player_guest_slot) {
      continue;
    }
    const auto muzzle = weaponMuzzleAnchorForGuestSlot(
        gameplay, actor_animations, &presentation, edge.actor_slot, weapon);
    if (!muzzle) {
      continue;
    }
    transient.push_back(game::TransientDynamicLightState{
        game::GameplayEffectType::muzzle_flash,
        {muzzle->x, muzzle->y, muzzle->z},
        static_cast<std::uint32_t>(0x80000000U |
                                   static_cast<std::uint16_t>(edge.actor_slot)),
        1.0,
        1U,
        1U,
        true,
    });
  }
  for (const auto &flash : retail_muzzle_flashes) {
    if (flash.source_slot == presentation.player_guest_slot) {
      continue;
    }
    const auto muzzle = weaponMuzzleAnchorForGuestSlot(
        gameplay, actor_animations, &presentation, flash.source_slot,
        gameplay.hud().inventory().current());
    if (!muzzle) {
      continue;
    }
    transient.push_back(game::TransientDynamicLightState{
        game::GameplayEffectType::muzzle_flash,
        {muzzle->x, muzzle->y, muzzle->z},
        static_cast<std::uint32_t>(flash.sequence * 17U +
                                   flash.controller * 29U + flash.particle),
        1.0,
        1U,
        1U,
        true,
    });
  }
  const auto guest_effects_authoritative =
      presentation.guest_camera_lists_captured &&
      gameplay.legacyEffectParticlesAuthoritative();
  const auto guest_slots = gameplay.legacyGuestSlotsBySceneObject();
  for (const auto &effect : native_muzzle_flashes) {
    if (!game::nativeGameplayEffectPresentationAllowed(
            effect, guest_effects_authoritative,
            presentation.first_person_aim)) {
      continue;
    }
    const auto duplicated_by_retail_line =
        effect.attachment == game::GameplayEffectAttachment::npc_muzzle &&
        effect.owner_object < guest_slots.size() &&
        std::ranges::find(
            retail_muzzle_flashes, guest_slots[effect.owner_object],
            &game::LegacyMuzzleFlashPresentationState::source_slot) !=
            retail_muzzle_flashes.end();
    if (duplicated_by_retail_line) {
      continue;
    }
    const auto centre =
        dynamicEffectCentre(gameplay, actor_animations, presentation, effect);
    if (!centre) {
      continue;
    }
    transient.push_back(game::TransientDynamicLightState{
        effect.type,
        {centre->x, centre->y, centre->z},
        effect.seed,
        effect.scale,
        effect.remaining_updates,
        effect.total_updates,
        true,
    });
  }
  for (const auto &effect : gameplay.effects()) {
    if (effect.type != game::GameplayEffectType::explosion &&
        effect.type != game::GameplayEffectType::burning_fire) {
      continue;
    }
    if (!game::nativeGameplayEffectPresentationAllowed(
            effect, guest_effects_authoritative,
            presentation.first_person_aim)) {
      continue;
    }
    const auto centre =
        dynamicEffectCentre(gameplay, actor_animations, presentation, effect);
    if (!centre) {
      continue;
    }
    transient.push_back(game::TransientDynamicLightState{
        effect.type,
        {centre->x, centre->y, centre->z},
        effect.seed,
        effect.scale,
        effect.remaining_updates,
        effect.total_updates,
        true,
    });
  }
  for (std::size_t index = 0U; index < presentation.projectiles.size();
       ++index) {
    const auto &projectile = presentation.projectiles[index];
    if (!projectile.active ||
        projectile.phase != game::ProjectilePhase::explosion ||
        projectile.remaining_updates == 0U) {
      continue;
    }
    const auto total = projectile.remaining_updates + projectile.age_updates;
    if (total > std::numeric_limits<std::uint16_t>::max()) {
      continue;
    }
    transient.push_back(game::TransientDynamicLightState{
        game::GameplayEffectType::explosion,
        {projectile.x, projectile.y, projectile.z},
        static_cast<std::uint32_t>(0x40000000U | index),
        std::max(projectile.radius / 480.0, 0.5),
        static_cast<std::uint16_t>(projectile.remaining_updates),
        static_cast<std::uint16_t>(total),
        true,
    });
  }

  std::array<game::DirectionalDynamicLightState, 1U> directional{};
  auto directional_count = std::size_t{};
  if (presentation.flashlight_enabled) {
    const auto flashlight =
        std::ranges::find(presentation.vertex_lights, retail_flashlight_source,
                          &game::LegacyVertexLightBridgeState::source);
    if (flashlight != presentation.vertex_lights.end()) {
      const auto retail = retailVertexLightState(*flashlight);
      if (const auto ray = game::retailVertexLightRay(retail)) {
        directional[directional_count++] = game::DirectionalDynamicLightState{
            game::DynamicLightKind::flashlight, ray->origin, ray->direction,
            retail_flashlight_source,           true,        true,
        };
      }
    }
  }
  active_dynamic_lights = game::buildDynamicLightFrame(
      persistent, transient,
      {presentation.camera.x, presentation.camera.y, presentation.camera.z},
      std::span{directional}.first(directional_count),
      presentation.guest_frame);
  active_baked_world_dynamic_lights =
      game::dynamicLightFrameForBakedWorld(active_dynamic_lights);
  active_dynamic_shadow_lights = game::buildDynamicLightFrame(
      persistent, std::span<const game::TransientDynamicLightState>{},
      {presentation.camera.x, presentation.camera.y, presentation.camera.z},
      std::span<const game::DirectionalDynamicLightState>{},
      presentation.guest_frame);
  active_dynamic_light_observer = {presentation.camera.x, presentation.camera.y,
                                   presentation.camera.z};
}

std::span<const std::uint16_t>
liveWorldVertexColors(const RenderPresentationSnapshot &presentation,
                      std::uint16_t model, std::uint16_t section,
                      std::size_t expected_vertices) {
  const auto key = (static_cast<std::uint32_t>(model) << 16U) | section;
  auto first = std::size_t{};
  auto count = presentation.world_vertex_colors.size();
  while (count != 0U) {
    const auto step = count / 2U;
    const auto &candidate = presentation.world_vertex_colors[first + step];
    const auto candidate_key =
        (static_cast<std::uint32_t>(candidate.model) << 16U) |
        candidate.section;
    if (candidate_key < key) {
      first += step + 1U;
      count -= step + 1U;
    } else {
      count = step;
    }
  }
  const auto colors = presentation.world_vertex_colors.begin() +
                      static_cast<std::ptrdiff_t>(first);
  if (colors == presentation.world_vertex_colors.end()) {
    return {};
  }
  if (colors->model != model || colors->section != section) {
    return {};
  }
  if (colors->colors.size() != expected_vertices) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Retail world vertex-color layout does not match EMD"};
  }
  return colors->colors;
}

void renderProjectileSprites(const HudTextureAtlas &textures,
                             const game::GameplaySession &gameplay,
                             const RenderPresentationSnapshot &presentation,
                             const MATRIX &view,
                             std::vector<OT_TAG> &ordering_table,
                             PrimitiveBuffer &primitives, RenderStats &stats);

RenderStats renderWorld(
    const game::GameplaySession &gameplay, const TextureStreamer &textures,
    const HudTextureAtlas &hud_textures, const FireAnimation &fire_animation,
    const FireTexturePlacement &fire_texture_placement,
    const CombatEffectTextureAtlas &effect_textures,
    const game::ActorAnimationBank &actor_animations, std::uint64_t actor_tick,
    std::uint64_t player_animation_tick,
    std::span<const game::LegacyWeaponEventBridgeState> weapon_edges,
    std::span<const game::LegacyMuzzleFlashPresentationState>
        retail_muzzle_flashes,
    std::span<const game::GameplayEffect> native_muzzle_flashes,
    std::span<const game::LegacyLineParticleBridgeState> retail_lines,
    std::span<const game::LegacyCombatParticleBridgeState> combat_particles,
    std::span<const game::LegacyGuestSpritePresentationState> retained_sprites,
    std::span<const game::LegacyGuestRawPacketPresentationState>
        retained_raw_packets,
    std::span<const GlassShardPresentationState> glass_shards,
    double interpolation_amount, const RenderPresentationSnapshot &presentation,
    const MATRIX &view, std::vector<OT_TAG> &ordering_table,
    std::vector<OT_TAG> &shadow_ordering_table,
    std::vector<OT_TAG> &scrim_ordering_table,
    std::vector<OT_TAG> &guest_overlay_ordering_table,
    std::vector<OT_TAG> &fire_ordering_table,
#if defined(SF_ENABLE_SURFACE_PICKER)
    std::vector<OT_TAG> &surface_picker_ordering_table,
#endif
    ActorShadowPresentationCache &shadow_cache,
    DynamicLightFrameScratch &dynamic_light_scratch,
    LocalSceneLightingCache &local_scene_lighting_cache,
    const WorldChunkAppearance &world_chunk_appearance,
    WeaponEffectsScratch &weapon_effects_scratch, HmdPoseScratch &pose_scratch,
    PrimitiveBuffer &primitives
#if defined(SF_ENABLE_SURFACE_PICKER)
    ,
    PsyCrossSurfacePicker &surface_picker) {
#else
) {
#endif
  ClearOTagR(reinterpret_cast<u_long *>(ordering_table.data()),
             ordering_table_size);
  ClearOTagR(reinterpret_cast<u_long *>(shadow_ordering_table.data()),
             ordering_table_size);
  ClearOTagR(reinterpret_cast<u_long *>(scrim_ordering_table.data()),
             ordering_table_size);
  ClearOTagR(reinterpret_cast<u_long *>(guest_overlay_ordering_table.data()),
             ordering_table_size);
  ClearOTagR(reinterpret_cast<u_long *>(fire_ordering_table.data()),
             ordering_table_size);
#if defined(SF_ENABLE_SURFACE_PICKER)
  ClearOTagR(reinterpret_cast<u_long *>(surface_picker_ordering_table.data()),
             ordering_table_size);
#endif
  primitives.reset();
  shadow_cache.beginFrame(presentation.guest_frame);
  updateDynamicLightFrame(gameplay, actor_animations, presentation,
                          weapon_edges, retail_muzzle_flashes,
                          native_muzzle_flashes, dynamic_light_scratch);
  local_scene_lighting_cache.beginFrame(gameplay.currentRoom(),
                                        gameplay.presentationModels(),
                                        presentation.objects.size());
  const auto presented_player_weapon =
      presentation.flashlight_enabled ? game::WeaponId::flashlight
                                      : gameplay.hud().inventory().current();

  // The retail state can only change at 20 Hz. Recounting every model,
  // object, effect and pickup on each native presentation frame made the
  // 120/240 FPS modes repeat the same capacity pass up to twelve times.
  // Existing capacities remain valid after reset(), so plan them once per
  // guest frame (and explicitly invalidate at mission/room transitions).
  if (primitives.needsCapacityPlan(presentation.guest_frame,
                                   gameplay.currentRoom())) {
    const auto add_to_budget = [](std::size_t &budget, std::size_t amount,
                                  const char *description) {
      if (amount > std::numeric_limits<std::size_t>::max() - budget) {
        throw core::Error{core::ErrorCode::invalid_format,
                          std::string{description} +
                              " primitive count overflows"};
      }
      budget += amount;
    };
    std::size_t polygon_count = 0;
    for (const auto model_index : gameplay.presentationModels()) {
      add_to_budget(polygon_count,
                    gameplay.models()[model_index].scene.polygonCount(),
                    "Terrain");
    }
    constexpr std::size_t maximum_clipped_triangles = 4U;
    if (polygon_count >
        std::numeric_limits<std::size_t>::max() / maximum_clipped_triangles) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Terrain primitive count overflows"};
    }
    primitives.triangles.reserve(polygon_count * maximum_clipped_triangles);
    primitives.quads.reserve(polygon_count);
    std::size_t object_polygon_count{};
    std::size_t object_quad_count{};
    std::size_t actor_shadow_triangle_count{};
    std::size_t fire_particle_budget{};
    const auto guest_effects_authoritative =
        game::legacyGuestEffectsAuthoritative(
            presentation.guest_camera_lists_captured,
            gameplay.legacyEffectParticlesAuthoritative());
    const auto add_shadow_budget = [&](std::size_t triangle_count,
                                       const char *description) {
      constexpr auto receiver_subdivision_triangles = std::size_t{4U};
      if (triangle_count > std::numeric_limits<std::size_t>::max() /
                               receiver_subdivision_triangles) {
        throw core::Error{core::ErrorCode::invalid_format,
                          std::string{description} +
                              " primitive count overflows"};
      }
      add_to_budget(actor_shadow_triangle_count,
                    triangle_count * receiver_subdivision_triangles,
                    description);
    };
    if (presentation.scrim.visible) {
      const auto *scrim = gameplay.detachedScrimModel();
      if (scrim == nullptr || !presentation.scrim.transform_valid) {
        throw core::Error{core::ErrorCode::not_found,
                          "Visible retail SCRIM is incomplete"};
      }
      add_to_budget(object_polygon_count, scrim->polygonCount(),
                    "Retail SCRIM");
    }
    const auto add_weapon_model = [&](game::WeaponId weapon,
                                      bool casts_shadow) {
      const auto *weapon_model = gameplay.weaponModel(weapon);
      if (weapon_model == nullptr) {
        return;
      }
      const auto *geometry =
          std::get_if<assets::GmdModel>(&weapon_model->geometry);
      if (geometry != nullptr) {
        add_to_budget(object_polygon_count, geometry->triangles().size(),
                      "Weapon model");
        if (casts_shadow) {
          add_shadow_budget(geometry->triangles().size(), "Weapon shadow");
        }
      }
    };
    for (const auto object_index : gameplay.activeObjects()) {
      const auto *displayed_model = gameplay.displayedObjectModel(object_index);
      if (displayed_model == nullptr) {
        continue;
      }
      const auto &object_model = *displayed_model;
      const auto &geometry = object_model.geometry;
      if (const auto *model = std::get_if<assets::GmdModel>(&geometry)) {
        add_to_budget(object_polygon_count, model->triangles().size(),
                      "Object model");
      } else if (const auto *hmd_model =
                     std::get_if<assets::HmdModel>(&geometry)) {
        add_to_budget(object_polygon_count, hmd_model->triangles().size(),
                      "Actor model");
        const auto *state = gameplay.npcState(object_index);
        const auto casts_shadow =
            state != nullptr ||
            gameplay.legacyDedicatedActorPresentation(object_index);
        if (casts_shadow) {
          add_shadow_budget(hmd_model->triangles().size(), "Actor shadow");
        }
        if (const auto dedicated =
                gameplay.legacyDedicatedActorWeapon(object_index)) {
          add_weapon_model(*dedicated, casts_shadow);
        } else if (state != nullptr && state->health != 0U) {
          add_weapon_model(state->weapon, casts_shadow);
        }
      } else if (std::holds_alternative<game::ObjectFireEmitter>(geometry)) {
        if (!gameplay.legacyEffectParticlesAuthoritative()) {
          add_to_budget(fire_particle_budget, fire_particle_count,
                        "Object fire");
        }
      } else {
        add_to_budget(object_polygon_count,
                      std::get<assets::EmdScene>(geometry).polygonCount(),
                      "Object scene");
      }
    }
    // The scanner reveal is script-hidden from activeObjects. Reserve only its
    // four authored GRGLO triangles; never duplicate the complete corpse HMD.
    const auto scanner_active = virusScannerXrayActive(
        presentation.first_person_aim, gameplay.hud().inventory().current());
    if (const auto target = scanner_active
                                ? gameplay.legacyVirusScannerTargetObject()
                                : std::nullopt) {
      const auto marker = gameplay.legacyVirusScannerMarkerObject(*target);
      const auto model_index = marker && *marker < gameplay.objects().size()
                                   ? gameplay.objects()[*marker].model
                                   : std::numeric_limits<std::uint16_t>::max();
      if (model_index < gameplay.objectModels().size()) {
        const auto &geometry = gameplay.objectModels()[model_index].geometry;
        if (const auto *gmd = std::get_if<assets::GmdModel>(&geometry)) {
          add_to_budget(object_polygon_count, gmd->triangles().size(),
                        "Virus scanner reveal");
        }
      }
    }
    add_to_budget(object_polygon_count, glass_shards.size(), "Glass shards");
    if (const auto *player_model =
            std::get_if<assets::HmdModel>(&gameplay.playerModel().geometry)) {
      add_to_budget(object_polygon_count, player_model->triangles().size(),
                    "Player model");
      if (gameplay.playerAlive()) {
        add_weapon_model(presented_player_weapon,
                         !presentation.first_person_aim);
      }
      if (!presentation.first_person_aim) {
        add_shadow_budget(player_model->triangles().size(), "Player shadow");
      }
    }
    const auto legacy_fallback_particle_count = std::ranges::count_if(
        gameplay.legacyExplParticles(), [&](const auto &particle) {
          return !game::legacyExplParticleHasGuestSprite(
              particle.pool_index, static_cast<std::uint8_t>(particle.family),
              presentation.guest_sprites,
              presentation.guest_camera_lists_captured,
              gameplay.legacyEffectParticlesAuthoritative());
        });
    add_to_budget(fire_particle_budget,
                  static_cast<std::size_t>(legacy_fallback_particle_count),
                  "Legacy effect");
    const auto &shot = gameplay.lastShot();
    if (shot.fired && shot.weapon == game::WeaponId::flamethrower &&
        !gameplay.legacyEffectParticlesAuthoritative()) {
      add_to_budget(fire_particle_budget, 7U, "Flamethrower");
    }
    if (!guest_effects_authoritative) {
      for (const auto &projectile : gameplay.projectiles()) {
        if (projectile.active &&
            projectile.phase == game::ProjectilePhase::explosion) {
          add_to_budget(fire_particle_budget, 6U, "Projectile explosion");
        }
      }
      for (const auto &effect : gameplay.effects()) {
        if (effect.type == game::GameplayEffectType::burning_fire) {
          add_to_budget(fire_particle_budget, 12U, "Burning effect");
        } else if (effect.type == game::GameplayEffectType::explosion) {
          add_to_budget(fire_particle_budget, 9U, "Explosion effect");
        }
      }
    }
    // A near-plane clipped camera-facing particle becomes at most three
    // triangles. Counting it as a regular four-triangle clipped polygon keeps
    // the shared object vector stable and therefore every OT pointer valid.
    add_to_budget(object_polygon_count, fire_particle_budget, "Fire effect");
    object_quad_count = object_polygon_count;
    if (object_polygon_count >
        std::numeric_limits<std::size_t>::max() / maximum_clipped_triangles) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Object primitive count overflows"};
    }
    primitives.objects.reserve(object_polygon_count *
                               maximum_clipped_triangles);
    primitives.object_quads.reserve(object_quad_count);
#if defined(SF_ENABLE_SURFACE_PICKER)
    primitives.surface_picker_triangles.reserve(1U);
    primitives.surface_picker_quads.reserve(1U);
#endif
    primitives.actor_shadows.reserve(actor_shadow_triangle_count);
    primitives.player.reserve(72U);
    // OT entries hold raw primitive addresses. Reserve the full worst-case
    // combat budget up front so a late blood spray/taser arc cannot reallocate
    // either vector and invalidate already-linked wall/effect primitives.
    primitives.effects.reserve(4096U);
    primitives.combat_effect_lines.reserve(256U);
    primitives.combat_effect_triangles.reserve(256U);
    constexpr auto flashlight_cone_quad_budget = std::size_t{108U};
    primitives.flashlight_cone_quads.reserve(flashlight_cone_quad_budget);
    primitives.effect_sprite_quads.reserve(2048U);
    primitives.park2_flamethrower_ribbons.reserve(
        gameplay.legacyPark2FlamethrowerRibbons().size());
    std::size_t pickup_sprite_budget{};
    for (const auto &item : presentation.dropped_items) {
      add_to_budget(pickup_sprite_budget,
                    game::droppedItemIconLayers(item.item).size(),
                    "Pickup sprite");
    }
    primitives.pickup_sprites.reserve(pickup_sprite_budget);
    std::size_t projectile_sprite_budget{};
    for (const auto &projectile : presentation.projectiles) {
      if (!projectile.active ||
          projectile.phase != game::ProjectilePhase::flying ||
          (projectile.weapon != game::WeaponId::fragmentation_grenade &&
           projectile.weapon != game::WeaponId::gas_grenade)) {
        continue;
      }
      add_to_budget(
          projectile_sprite_budget,
          game::weaponDefinition(projectile.weapon).icon.layers().size(),
          "Projectile sprite");
    }
    primitives.projectile_sprites.reserve(projectile_sprite_budget);
    primitives.guest_sprites.reserve(presentation.guest_sprites.size() +
                                     retained_sprites.size());
    primitives.guest_lines.reserve(presentation.guest_lines.size());
    primitives.guest_line_modes.reserve(presentation.guest_lines.size());
    const auto guest_raw_packet_budget =
        presentation.guest_raw_packets.size() + retained_raw_packets.size();
    primitives.guest_raw_tiles.reserve(guest_raw_packet_budget);
    primitives.guest_raw_flat_lines.reserve(guest_raw_packet_budget);
    primitives.guest_raw_flat_triangles.reserve(guest_raw_packet_budget);
    primitives.guest_raw_flat_quads.reserve(guest_raw_packet_budget);
    primitives.guest_raw_gouraud_lines.reserve(guest_raw_packet_budget);
    primitives.guest_raw_gouraud_triangles.reserve(guest_raw_packet_budget);
    primitives.guest_raw_modes.reserve(guest_raw_packet_budget);
  }
  primitives.lockStorage();

  SetRotMatrix(const_cast<MATRIX *>(&view));
  SetTransMatrix(const_cast<MATRIX *>(&view));
  RenderStats stats;
  for (const auto model_index : gameplay.presentationModels()) {
    const auto &scene = gameplay.models()[model_index].scene;
    const auto chunk_depth_cue_floor =
        world_chunk_appearance.depthCueFloorQ12(model_index);
    const auto terrain_depth_cue = [&](const SVECTOR &vertex) {
      return std::max(chunk_depth_cue_floor,
                      retailTerrainDepthCue(cameraDepth(view, vertex)));
    };
    const auto texture_bank = scene.textureBank();
    for (auto section_index = std::size_t{};
         section_index < scene.sections().size(); ++section_index) {
      const auto &section = scene.sections()[section_index];
      const auto live_colors = liveWorldVertexColors(
          presentation, model_index, static_cast<std::uint16_t>(section_index),
          section.vertices.size());
      for (auto polygon_index = std::size_t{};
           polygon_index < section.polygons.size(); ++polygon_index) {
        const auto &polygon = section.polygons[polygon_index];
        if (!polygon.renderable) {
          continue;
        }
        const auto texture_source_page = emdTextureSourcePage(scene, polygon);
        auto v0 = makeVertex(section.vertices[polygon.vertex_indices[0]]);
        auto v1 = makeVertex(section.vertices[polygon.vertex_indices[1]]);
        auto v2 = makeVertex(section.vertices[polygon.vertex_indices[2]]);
        long xy0{};
        long xy1{};
        long xy2{};
        long xy3{};
        long depth_cue{};
        long flags{};
        int depth{};

        if (polygon.quad) {
          auto v3 = makeVertex(section.vertices[polygon.vertex_indices[3]]);
          const std::array positions{v0, v1, v2, v3};
          const auto near_status = classifyNearPlane(view, positions);
          if (near_status == NearPlaneStatus::outside) {
            ++stats.rejected;
            continue;
          }
          if (near_status == NearPlaneStatus::intersecting) {
            const auto normal = visibleSurfaceNormal(positions);
            const auto t0 =
                emdTexturedVertex(v0, section, polygon, 0, live_colors, true,
                                  normal, &active_baked_world_dynamic_lights);
            const auto t1 =
                emdTexturedVertex(v1, section, polygon, 1, live_colors, true,
                                  normal, &active_baked_world_dynamic_lights);
            const auto t2 =
                emdTexturedVertex(v2, section, polygon, 2, live_colors, true,
                                  normal, &active_baked_world_dynamic_lights);
            const auto t3 =
                emdTexturedVertex(v3, section, polygon, 3, live_colors, true,
                                  normal, &active_baked_world_dynamic_lights);
            const std::array first_triangle{t0, t1, t2};
            const std::array second_triangle{t1, t3, t2};
            const TexturedMaterial material{
                polygon.texture_page,
                polygon.clut,
                false,
                false,
                static_cast<std::uint8_t>(texture_bank),
                texture_source_page,
            };
            submitClippedTriangle(first_triangle, material, false, false, view,
                                  ordering_table, primitives.triangles, stats,
                                  worldOrderingDepth, true, true,
                                  chunk_depth_cue_floor);
            submitClippedTriangle(second_triangle, material, false, false, view,
                                  ordering_table, primitives.triangles, stats,
                                  worldOrderingDepth, true, true,
                                  chunk_depth_cue_floor);
            continue;
          }
          auto &primitive = primitives.quads.emplace_back();
          configurePrimitive(primitive, section, polygon, texture_bank,
                             texture_source_page, live_colors);
          depth = RotTransPers4(&v0, &v1, &v2, &v3, &xy0, &xy1, &xy2, &xy3,
                                &depth_cue, &flags);
          if (depth <= 0 || !frontFacing(xy0, xy1, xy2, 4U)) {
            primitives.quads.pop_back();
            ++stats.rejected;
            continue;
          }
          // Dynamic light sampling scans the active light frame per vertex.
          // Do it only for geometry which survived depth/back-face culling.
          dynamicallyLight(primitive, positions,
                           active_baked_world_dynamic_lights);
          setProjected(&primitive.x0, xy0);
          setProjected(&primitive.x1, xy1);
          setProjected(&primitive.x2, xy2);
          setProjected(&primitive.x3, xy3);
          depthCuePrimitive(primitive,
                            {terrain_depth_cue(v0), terrain_depth_cue(v1),
                             terrain_depth_cue(v2), terrain_depth_cue(v3)});
          addPrim(
              &ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
              &primitive);
          const std::array projected{xy0, xy1, xy2, xy3};
#if defined(SF_ENABLE_SURFACE_PICKER)
          if (surface_picker.active() &&
              surface_picker.observe(makeSurfacePickerSurface(
                  SurfacePickerId{SurfacePickerKind::world, model_index,
                                  static_cast<std::uint32_t>(section_index),
                                  static_cast<std::uint32_t>(polygon_index)},
                  "world-model-" + std::to_string(model_index),
                  gameplay.currentRoom(), presentation.guest_frame, positions,
                  projected, polygon.uv, depth, polygon.texture_page,
                  primitive.tpage, polygon.clut, primitive.clut,
                  texture_source_page, texture_bank, textures))) {
            submitSurfacePickerHighlight(
                projected, depth, surface_picker_ordering_table, primitives);
          }
#endif
          submitStats(stats, depth, projected);
        } else {
          const std::array positions{v0, v1, v2};
          const auto near_status = classifyNearPlane(view, positions);
          if (near_status == NearPlaneStatus::outside) {
            ++stats.rejected;
            continue;
          }
          if (near_status == NearPlaneStatus::intersecting) {
            const auto normal = visibleSurfaceNormal(positions);
            const std::array textured{
                emdTexturedVertex(v0, section, polygon, 0, live_colors, true,
                                  normal, &active_baked_world_dynamic_lights),
                emdTexturedVertex(v1, section, polygon, 1, live_colors, true,
                                  normal, &active_baked_world_dynamic_lights),
                emdTexturedVertex(v2, section, polygon, 2, live_colors, true,
                                  normal, &active_baked_world_dynamic_lights),
            };
            submitClippedTriangle(
                textured,
                TexturedMaterial{polygon.texture_page, polygon.clut, false,
                                 false, static_cast<std::uint8_t>(texture_bank),
                                 texture_source_page},
                false, false, view, ordering_table, primitives.triangles, stats,
                worldOrderingDepth, true, true, chunk_depth_cue_floor);
            continue;
          }
          auto &primitive = primitives.triangles.emplace_back();
          configurePrimitive(primitive, section, polygon, texture_bank,
                             texture_source_page, live_colors);
          depth = RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue,
                                &flags);
          if (depth <= 0 || !frontFacing(xy0, xy1, xy2, 3U)) {
            primitives.triangles.pop_back();
            ++stats.rejected;
            continue;
          }
          // Avoid normalisation and up to 32 light tests for rejected faces.
          // Visible output is identical because rejected packets are removed.
          dynamicallyLight(primitive, positions,
                           active_baked_world_dynamic_lights);
          setProjected(&primitive.x0, xy0);
          setProjected(&primitive.x1, xy1);
          setProjected(&primitive.x2, xy2);
          depthCuePrimitive(primitive,
                            {terrain_depth_cue(v0), terrain_depth_cue(v1),
                             terrain_depth_cue(v2)});
          addPrim(
              &ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
              &primitive);
          const std::array projected{xy0, xy1, xy2};
#if defined(SF_ENABLE_SURFACE_PICKER)
          const std::array uv{polygon.uv[0], polygon.uv[1], polygon.uv[2]};
          if (surface_picker.active() &&
              surface_picker.observe(makeSurfacePickerSurface(
                  SurfacePickerId{SurfacePickerKind::world, model_index,
                                  static_cast<std::uint32_t>(section_index),
                                  static_cast<std::uint32_t>(polygon_index)},
                  "world-model-" + std::to_string(model_index),
                  gameplay.currentRoom(), presentation.guest_frame, positions,
                  projected, uv, depth, polygon.texture_page, primitive.tpage,
                  polygon.clut, primitive.clut, texture_source_page,
                  texture_bank, textures))) {
            submitSurfacePickerHighlight(
                projected, depth, surface_picker_ordering_table, primitives);
          }
#endif
          submitStats(stats, depth, projected);
        }
      }
    }
  }
  if (presentation.scrim.visible) {
    const auto *scrim = gameplay.detachedScrimModel();
    if (scrim == nullptr || !presentation.scrim.transform_valid) {
      throw core::Error{core::ErrorCode::not_found,
                        "Visible retail SCRIM is incomplete"};
    }
    const auto &source = presentation.scrim.transform;
    auto object = game::SceneObject{};
    object.transform.rotation = source.rotation;
    object.transform.x = source.translation.x;
    object.transform.y = -source.translation.y;
    object.transform.z = source.translation.z;
#if defined(SF_ENABLE_SURFACE_PICKER)
    const auto picker_context = SurfacePickerObjectContext{
        .picker = &surface_picker,
        .textures = &textures,
        .ordering_table = &surface_picker_ordering_table,
        .kind = SurfacePickerKind::scrim,
        .owner = 0U,
        .label = "retail-scrim",
        .room = gameplay.currentRoom(),
        .guest_frame = presentation.guest_frame,
    };
#endif
    // Retail flag 0x00200000 disables distance fade/culling and 0x00100000
    // excludes SCRIM from local vertex lights. Native presentation also gives
    // this semantic background its own non-depth-writing pass: the sky keeps
    // its retail painter order but can never compete with level geometry in Z.
    renderEmdObject(*scrim, object, scrim->textureBank(), view,
                    scrim_ordering_table, primitives, stats, true
#if defined(SF_ENABLE_SURFACE_PICKER)
                    ,
                    surface_picker.active() ? &picker_context : nullptr
#endif
    );
  }
  renderObjects(gameplay,
#if defined(SF_ENABLE_SURFACE_PICKER)
                textures,
#endif
                fire_animation, fire_texture_placement, actor_animations,
                actor_tick, interpolation_amount, presentation, view,
                ordering_table, shadow_ordering_table,
#if defined(SF_ENABLE_SURFACE_PICKER)
                surface_picker_ordering_table,
#endif
                fire_ordering_table, shadow_cache, local_scene_lighting_cache,
                pose_scratch, primitives, stats
#if defined(SF_ENABLE_SURFACE_PICKER)
                ,
                surface_picker
#endif
  );
  renderDroppedItemSprites(hud_textures, gameplay, presentation, view,
                           ordering_table, primitives, stats);
  renderProjectileSprites(hud_textures, gameplay, presentation, view,
                          ordering_table, primitives, stats);
  renderGlassShards(glass_shards, interpolation_amount, view, ordering_table,
                    primitives, stats);
  renderWeaponEffects(gameplay, actor_tick, weapon_edges, retail_muzzle_flashes,
                      native_muzzle_flashes, retail_lines, combat_particles,
                      fire_texture_placement, effect_textures, actor_animations,
                      presentation, view, ordering_table, fire_ordering_table,
                      primitives, stats, weapon_effects_scratch);
  renderGuestCameraLists(gameplay, textures, presentation, retained_sprites,
                         retained_raw_packets, view, ordering_table,
                         guest_overlay_ordering_table, primitives, stats);
  renderVirusScannerMarker(gameplay, presentation, view,
                           guest_overlay_ordering_table, primitives, stats);
  renderPresentedPlayer(gameplay, actor_animations, player_animation_tick,
                        presented_player_weapon, interpolation_amount,
                        presentation, view, ordering_table,
                        shadow_ordering_table, shadow_cache, pose_scratch,
                        primitives, stats);
  if (!primitives.storageStable()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Primitive storage moved while building the OT"};
  }
  return stats;
}
