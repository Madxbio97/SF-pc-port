constexpr std::size_t fire_particle_count = 2U;
// Class 0x30 passes size 0x999. The particle renderer derives scale byte 57,
// projects a 912-unit reference vector, then applies Q12 scale to a 32-pixel
// EXPL frame; the resulting native camera-facing span is 456 world units.
constexpr double fire_particle_size = 456.0;
// FUN_800540dc projects a scale-byte * 0x10 reference vector; the 32-pixel
// EXPL primitive occupies one half of that reference span.
constexpr double legacy_effect_scale_unit = 8.0;
constexpr unsigned int fire_velocity_damping_shift = 4U;
constexpr std::int32_t fire_initial_spread_mask = 37;
constexpr std::int32_t fire_initial_half_spread = 18;
constexpr std::int32_t fire_respawn_spread_mask = 56;
constexpr std::int32_t fire_respawn_offset = 46;
constexpr std::int32_t fire_velocity_mask = 28;
constexpr std::int32_t fire_velocity_offset = 14;
constexpr std::int32_t fire_native_vertical_acceleration = 0x6700;

std::int32_t shiftRightFloor(std::int32_t value, unsigned int bits) {
  if (value >= 0) {
    return value >> bits;
  }
  const auto magnitude = -static_cast<std::int64_t>(value);
  const auto rounded = (magnitude + (std::int64_t{1} << bits) - 1) >> bits;
  return static_cast<std::int32_t>(-rounded);
}

struct FireParticle {
  std::int32_t x{};
  std::int32_t y{};
  std::int32_t z{};
  std::int32_t velocity_x{};
  std::int32_t native_velocity_y_fixed{};
  std::int32_t velocity_z{};
  int total_ticks{};
  int remaining_ticks{};
  game::LegacyEffectSpriteFamily family{
      game::LegacyEffectSpriteFamily::explosion};
  std::uint8_t frame{};
  double size{fire_particle_size};
  std::uint8_t red{128U};
  std::uint8_t green{128U};
  std::uint8_t blue{128U};
};

struct WeaponEffectsScratch {
  std::vector<FireParticle> legacy_particles;
};

struct FireEmitterState {
  bool present{};
  std::int32_t start_x{};
  std::int32_t start_y{};
  std::int32_t start_z{};
  std::uint32_t random_state{};
  std::array<FireParticle, fire_particle_count> particles{};
};

class FireAnimation final {
public:
  explicit FireAnimation(const game::GameplaySession &gameplay)
      : emitters_(gameplay.objects().size()) {
    for (std::size_t index = 0; index < gameplay.objects().size(); ++index) {
      const auto &object = gameplay.objects()[index];
      const auto *fire = std::get_if<game::ObjectFireEmitter>(
          &gameplay.objectModels()[object.model].geometry);
      if (fire == nullptr) {
        continue;
      }
      auto &emitter = emitters_[index];
      emitter.present = true;
      const auto native_start = game::cfireSpawnPoint(object.transform);
      const auto start =
          makeVertex(native_start.x, native_start.y, native_start.z);
      emitter.start_x = start.vx;
      emitter.start_y = start.vy;
      emitter.start_z = start.vz;
      emitter.random_state =
          0x6d2b79f5U ^ (static_cast<std::uint32_t>(index) * 0x9e3779b9U);
      initialize(emitter);
    }
  }

  void update() {
    for (auto &emitter : emitters_) {
      if (!emitter.present) {
        continue;
      }
      for (auto &particle : emitter.particles) {
        --particle.remaining_ticks;
        if (particle.remaining_ticks <= 0) {
          resetParticle(emitter, particle);
        }
        particle.frame = static_cast<std::uint8_t>(std::clamp(
            7 - (7 * particle.remaining_ticks / particle.total_ticks), 0, 7));
        particle.x += particle.velocity_x;
        particle.y -= (particle.native_velocity_y_fixed +
                       fire_native_vertical_acceleration) /
                      4096;
        particle.z += particle.velocity_z;
        damp(particle.velocity_x);
        damp(particle.native_velocity_y_fixed);
        damp(particle.velocity_z);
      }
    }
  }

  [[nodiscard]] const std::array<FireParticle, fire_particle_count> &
  particles(std::size_t object_index) const {
    if (object_index >= emitters_.size() || !emitters_[object_index].present) {
      throw core::Error{core::ErrorCode::invalid_argument,
                        "Object is not a CFIRE emitter"};
    }
    return emitters_[object_index].particles;
  }

private:
  static std::uint32_t random(FireEmitterState &emitter) {
    emitter.random_state = emitter.random_state * 1664525U + 1013904223U;
    return emitter.random_state >> 16U;
  }

  static void damp(std::int32_t &velocity) {
    velocity -= shiftRightFloor(velocity + 1, fire_velocity_damping_shift);
  }

  static void initialize(FireEmitterState &emitter) {
    for (auto &particle : emitter.particles) {
      particle.x =
          emitter.start_x - fire_initial_half_spread +
          static_cast<std::int32_t>(random(emitter) & fire_initial_spread_mask);
      const auto native_y_offset =
          static_cast<std::int32_t>(random(emitter) &
                                    fire_initial_spread_mask) -
          fire_initial_half_spread;
      particle.y = emitter.start_y - std::abs(native_y_offset);
      particle.z =
          emitter.start_z - fire_initial_half_spread +
          static_cast<std::int32_t>(random(emitter) & fire_initial_spread_mask);
      particle.velocity_x = 0;
      particle.native_velocity_y_fixed = 0;
      particle.velocity_z = 0;
      resetLifetime(emitter, particle);
    }
  }

  static void resetLifetime(FireEmitterState &emitter, FireParticle &particle) {
    particle.total_ticks = 7 + static_cast<int>(random(emitter) & 7U);
    particle.remaining_ticks = particle.total_ticks;
    particle.frame = 0;
  }

  static void resetParticle(FireEmitterState &emitter, FireParticle &particle) {
    // Class 0x30 uses the 0x201010 preset. These are bit masks, not modulo
    // ranges; preserving that detail reproduces the original sparse jitter.
    resetLifetime(emitter, particle);
    const auto jitter_x =
        static_cast<std::int32_t>(random(emitter) & fire_respawn_spread_mask);
    const auto jitter_z =
        static_cast<std::int32_t>(random(emitter) & fire_respawn_spread_mask);
    const auto jitter_y =
        static_cast<std::int32_t>(random(emitter) & fire_respawn_spread_mask);
    particle.x = emitter.start_x - fire_respawn_offset + jitter_x;
    particle.z = emitter.start_z - fire_respawn_offset + jitter_z;
    particle.y = emitter.start_y + fire_initial_half_spread - jitter_y;
    particle.velocity_x =
        static_cast<std::int32_t>(random(emitter) & fire_velocity_mask) -
        fire_velocity_offset;
    particle.native_velocity_y_fixed =
        static_cast<std::int32_t>(random(emitter) & fire_velocity_mask) * 1024;
    particle.velocity_z =
        static_cast<std::int32_t>(random(emitter) & fire_velocity_mask) -
        fire_velocity_offset;
  }

  std::vector<FireEmitterState> emitters_;
};

struct FireFrameMaterial {
  double minimum_u{};
  double minimum_v{};
  double maximum_u{};
  double maximum_v{};
  TexturedMaterial material{};
};

FireFrameMaterial fireFrameMaterial(const assets::TimImage &image,
                                    const FireTexturePlacement &placement,
                                    game::LegacyEffectSpriteFamily family,
                                    std::size_t frame_index) {
  if (!image.clut()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "CFIRE frame has no CLUT"};
  }
  if (!placement.valid) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "CFIRE is not resident in VRAM"};
  }
  const auto *resident = placement.frame(family, frame_index);
  if (resident == nullptr) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "CFIRE frame is outside the native atlas"};
  }
  return FireFrameMaterial{
      resident->minimum_u,
      resident->minimum_v,
      resident->maximum_u,
      resident->maximum_v,
      TexturedMaterial{
          resident->texture_page,
          resident->clut,
          true,
          true,
      },
  };
}

const std::vector<assets::TimImage> *
legacyEffectFrames(const game::ObjectFireEmitter &fire,
                   game::LegacyEffectSpriteFamily family) noexcept {
  switch (family) {
  case game::LegacyEffectSpriteFamily::fire:
    return &fire.fire_frames;
  case game::LegacyEffectSpriteFamily::explosion:
    return &fire.frames;
  case game::LegacyEffectSpriteFamily::breath:
    return &fire.breath_frames;
  case game::LegacyEffectSpriteFamily::vapor:
    return &fire.vapor_frames;
  }
  return nullptr;
}

std::optional<std::array<Vector3, 4U>>
park2FlameWorldCorners(const game::LegacyPark2FlamethrowerRibbon &ribbon,
                       const game::CameraState &authored_camera) {
  const auto basis = viewBasis(authored_camera);
  const auto projection =
      static_cast<double>(std::max(authored_camera.projection, 1));
  const auto camera =
      Vector3{authored_camera.x, authored_camera.y, authored_camera.z};
  const auto depth = [&](const game::LegacyNativePoint &point) {
    const auto delta = Vector3{static_cast<double>(point.x) - camera.x,
                               static_cast<double>(point.y) - camera.y,
                               static_cast<double>(point.z) - camera.z};
    return delta.x * basis[2].x + delta.y * basis[2].y + delta.z * basis[2].z;
  };
  const auto first_depth = depth(ribbon.world_first);
  const auto second_depth = depth(ribbon.world_second);
  if (first_depth <= active_near_clip_depth ||
      second_depth <= active_near_clip_depth) {
    return std::nullopt;
  }

  std::array<Vector3, 4U> result{};
  const auto expand_pair = [&](std::size_t first_corner,
                               const game::LegacyNativePoint &centre,
                               double centre_depth) {
    const auto midpoint_x =
        (static_cast<double>(ribbon.corners[first_corner].x) +
         ribbon.corners[first_corner + 1U].x) *
        0.5;
    const auto midpoint_y =
        (static_cast<double>(ribbon.corners[first_corner].y) +
         ribbon.corners[first_corner + 1U].y) *
        0.5;
    for (auto corner = first_corner; corner < first_corner + 2U; ++corner) {
      const auto horizontal =
          (static_cast<double>(ribbon.corners[corner].x) - midpoint_x) *
          centre_depth / projection;
      const auto vertical =
          (static_cast<double>(ribbon.corners[corner].y) - midpoint_y) *
          centre_depth / projection;
      result[corner] = Vector3{
          static_cast<double>(centre.x) + basis[0].x * horizontal +
              basis[1].x * vertical,
          static_cast<double>(centre.y) + basis[0].y * horizontal +
              basis[1].y * vertical,
          static_cast<double>(centre.z) + basis[0].z * horizontal +
              basis[1].z * vertical,
      };
    }
  };
  // FUN_80147a8c emits the next/fallback centre into corners 0/1 and the
  // current chain centre into corners 2/3.
  expand_pair(0U, ribbon.world_first, first_depth);
  expand_pair(2U, ribbon.world_second, second_depth);
  return result;
}

void renderPark2FlamethrowerRibbons(
    std::span<const game::LegacyPark2FlamethrowerRibbon> ribbons,
    const game::ObjectFireEmitter &fire,
    const FireTexturePlacement &texture_placement,
    const game::CameraState &authored_camera, const MATRIX &view,
    std::vector<OT_TAG> &ordering_table, PrimitiveBuffer &primitives,
    RenderStats &stats) {
  for (const auto &ribbon : ribbons) {
    if (ribbon.frame >= fire.frames.size()) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "PARK2 flame ribbon has no matching EXPL frame"};
    }
    const auto frame = fireFrameMaterial(
        fire.frames[ribbon.frame], texture_placement,
        game::LegacyEffectSpriteFamily::explosion, ribbon.frame);
    const auto world = park2FlameWorldCorners(ribbon, authored_camera);
    if (!world || std::ranges::any_of(*world, [&](const auto &point) {
          return preciseCameraDepth(view, point.x, point.y, point.z) <=
                 active_near_clip_depth;
        })) {
      ++stats.rejected;
      continue;
    }
    auto v0 = makeVertex((*world)[0].x, (*world)[0].y, (*world)[0].z);
    auto v1 = makeVertex((*world)[1].x, (*world)[1].y, (*world)[1].z);
    auto v2 = makeVertex((*world)[2].x, (*world)[2].y, (*world)[2].z);
    auto v3 = makeVertex((*world)[3].x, (*world)[3].y, (*world)[3].z);
    long xy0{};
    long xy1{};
    long xy2{};
    long xy3{};
    long depth_cue{};
    long flags{};
    const auto depth = RotTransPers4(&v0, &v1, &v2, &v3, &xy0, &xy1, &xy2, &xy3,
                                     &depth_cue, &flags);
    if (depth <= 0 || flags != 0L) {
      ++stats.rejected;
      continue;
    }
    auto &primitive = primitives.park2_flamethrower_ribbons.emplace_back();
    setPolyFT4(&primitive);
    setSemiTrans(&primitive, 1);
    setRGB0(&primitive, ribbon.red, ribbon.green, ribbon.blue);
    const std::array projected{xy0, xy1, xy2, xy3};
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    setProjected(&primitive.x3, xy3);
    setUV4(&primitive, static_cast<std::uint8_t>(frame.minimum_u),
           static_cast<std::uint8_t>(frame.minimum_v),
           static_cast<std::uint8_t>(frame.maximum_u),
           static_cast<std::uint8_t>(frame.minimum_v),
           static_cast<std::uint8_t>(frame.minimum_u),
           static_cast<std::uint8_t>(frame.maximum_v),
           static_cast<std::uint8_t>(frame.maximum_u),
           static_cast<std::uint8_t>(frame.maximum_v));
    primitive.clut = frame.material.clut;
    primitive.tpage = frame.material.texture_page;
    const auto ordering_depth = depth - (depth >> 2);
    addPrim(
        &ordering_table[std::clamp(ordering_depth, 1, ordering_table_size - 1)],
        &primitive);
    submitStats(stats, depth, projected);
  }
}

TexturedVertex fireVertex(const FireParticle &particle, const Vector3 &right,
                          const Vector3 &up, double horizontal, double vertical,
                          double u, double v) {
  return TexturedVertex{
      static_cast<double>(particle.x) + right.x * horizontal + up.x * vertical,
      static_cast<double>(particle.y) + right.y * horizontal + up.y * vertical,
      static_cast<double>(particle.z) + right.z * horizontal + up.z * vertical,
      u,
      v,
      static_cast<double>(particle.red),
      static_cast<double>(particle.green),
      static_cast<double>(particle.blue),
  };
}

void renderFireObject(const game::ObjectFireEmitter &fire,
                      std::span<const FireParticle> particles,
                      const FireTexturePlacement &texture_placement,
                      const MATRIX &view, std::vector<OT_TAG> &ordering_table,
                      PrimitiveBuffer &primitives, RenderStats &stats) {
  const auto right = normalize(Vector3{
      static_cast<double>(view.m[0][0]),
      static_cast<double>(view.m[0][1]),
      static_cast<double>(view.m[0][2]),
  });
  const auto up = normalize(Vector3{
      -static_cast<double>(view.m[1][0]),
      -static_cast<double>(view.m[1][1]),
      -static_cast<double>(view.m[1][2]),
  });
  for (const auto &particle : particles) {
    const auto *images = legacyEffectFrames(fire, particle.family);
    if (images == nullptr || particle.frame >= images->size()) {
      continue;
    }
    const auto &image = (*images)[particle.frame];
    const auto half_width = particle.size * 0.5;
    const auto half_height = half_width *
                             static_cast<double>(image.displayHeight()) /
                             static_cast<double>(image.displayWidth());
    const auto frame = fireFrameMaterial(image, texture_placement,
                                         particle.family, particle.frame);
    const auto top_left =
        fireVertex(particle, right, up, -half_width, half_height,
                   frame.minimum_u, frame.minimum_v);
    const auto top_right =
        fireVertex(particle, right, up, half_width, half_height,
                   frame.maximum_u, frame.minimum_v);
    const auto bottom_left =
        fireVertex(particle, right, up, -half_width, -half_height,
                   frame.minimum_u, frame.maximum_v);
    const auto bottom_right =
        fireVertex(particle, right, up, half_width, -half_height,
                   frame.maximum_u, frame.maximum_v);
    const std::array quad{top_left, top_right, bottom_left, bottom_right};
    const std::array perimeter{top_left, top_right, bottom_right, bottom_left};
    const auto fully_inside = std::all_of(
        perimeter.begin(), perimeter.end(),
        [&view](const TexturedVertex &vertex) {
          return cameraDepth(view, vertex) >= active_near_clip_depth;
        });
    const auto clipped = core::clipConvexPolygon<TexturedVertex, 5>(
        perimeter,
        [&view](const TexturedVertex &vertex) {
          return cameraDepth(view, vertex) - active_near_clip_depth;
        },
        interpolate);
    if (clipped.count < 3U) {
      ++stats.rejected;
      continue;
    }
    if (!fully_inside) {
      for (std::size_t index = 1; index + 1U < clipped.count; ++index) {
        const std::array triangle{
            clipped.vertices[0],
            clipped.vertices[index],
            clipped.vertices[index + 1U],
        };
        submitClippedTriangle(triangle, frame.material, true, false, view,
                              ordering_table, primitives.objects, stats,
                              fireOrderingDepth);
      }
      continue;
    }

    auto v0 = makeVertex(quad[0].x, quad[0].y, quad[0].z);
    auto v1 = makeVertex(quad[1].x, quad[1].y, quad[1].z);
    auto v2 = makeVertex(quad[2].x, quad[2].y, quad[2].z);
    auto v3 = makeVertex(quad[3].x, quad[3].y, quad[3].z);
    long xy0{};
    long xy1{};
    long xy2{};
    long xy3{};
    long depth_cue{};
    long flags{};
    const auto depth = RotTransPers4(&v0, &v1, &v2, &v3, &xy0, &xy1, &xy2, &xy3,
                                     &depth_cue, &flags);
    if (depth <= 0) {
      ++stats.rejected;
      continue;
    }
    auto &primitive = primitives.object_quads.emplace_back();
    configurePrimitive(primitive, quad, frame.material);
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    setProjected(&primitive.x3, xy3);
    addPrim(&ordering_table[std::clamp(fireOrderingDepth(depth), 1,
                                       ordering_table_size - 1)],
            &primitive);
    const std::array projected{xy0, xy1, xy2, xy3};
    submitStats(stats, depth, projected);
  }
}

void renderEffectLine(Vector3 first, Vector3 second, VertexColor first_color,
                      VertexColor second_color,
                      std::vector<OT_TAG> &ordering_table,
                      PrimitiveBuffer &primitives, RenderStats &stats,
                      bool semi_transparent = true) {
  auto v0 = makeVertex(first.x, first.y, first.z);
  auto v1 = makeVertex(second.x, second.y, second.z);
  auto v2 = v1;
  long xy0{};
  long xy1{};
  long xy2{};
  long depth_cue{};
  long flags{};
  const auto depth =
      RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
  if (depth <= 0) {
    return;
  }
  auto &line = primitives.effects.emplace_back();
  setLineG2(&line);
  if (semi_transparent) {
    setSemiTrans(&line, 1);
  }
  setRGB0(&line, first_color.red, first_color.green, first_color.blue);
  setRGB1(&line, second_color.red, second_color.green, second_color.blue);
  setProjected(&line.x0, xy0);
  setProjected(&line.x1, xy1);
  addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
          &line);
  const std::array projected{xy0, xy1};
  submitStats(stats, depth, projected);
}

void renderRetailCombatParticles(
    const game::GameplaySession &gameplay,
    std::span<const game::LegacyCombatParticleBridgeState> particles,
    const RenderPresentationSnapshot &presentation,
    std::vector<OT_TAG> &ordering_table, PrimitiveBuffer &primitives,
    RenderStats &stats) {
  constexpr double line_radius = 4.0;
  const auto camera_basis = viewBasis(presentation.camera);
  const auto projection =
      static_cast<double>(std::max(presentation.camera.projection, 1));
  const auto radians = [](std::int32_t angle) {
    return static_cast<double>(angle) * (2.0 * std::numbers::pi / 4096.0);
  };
  for (const auto &particle : particles) {
    auto centre = Vector3{static_cast<double>(particle.position.x),
                          -static_cast<double>(particle.position.y),
                          static_cast<double>(particle.position.z)};
    if (particle.kind ==
            game::LegacyCombatParticleKind::blood_impact_triangle &&
        particle.attached_slot >= 0) {
      const auto current_root =
          actorRootForGuestSlot(gameplay, nullptr, particle.attached_slot);
      const auto presented_root = actorRootForGuestSlot(gameplay, &presentation,
                                                        particle.attached_slot);
      if (current_root && presented_root) {
        centre.x += presented_root->x - current_root->x;
        centre.y += presented_root->y - current_root->y;
        centre.z += presented_root->z - current_root->z;
      }
    }

    const auto camera_delta = Vector3{centre.x - presentation.camera.x,
                                      centre.y - presentation.camera.y,
                                      centre.z - presentation.camera.z};
    const auto view_depth = camera_delta.x * camera_basis[2].x +
                            camera_delta.y * camera_basis[2].y +
                            camera_delta.z * camera_basis[2].z;
    if (view_depth <= active_near_clip_depth) {
      continue;
    }
    const auto screen_to_world = view_depth / projection;
    long projected_centre{};
    long projected_second{};
    long projected_third{};
    long depth_cue{};
    long flags{};
    const auto color = particle.color;
    if (particle.kind == game::LegacyCombatParticleKind::ejected_shot_line) {
      const auto angle = radians(particle.angle);
      const auto endpoint = Vector3{
          centre.x + (camera_basis[0].x * std::cos(angle) +
                      camera_basis[1].x * std::sin(angle)) *
                         line_radius * screen_to_world,
          centre.y + (camera_basis[0].y * std::cos(angle) +
                      camera_basis[1].y * std::sin(angle)) *
                         line_radius * screen_to_world,
          centre.z + (camera_basis[0].z * std::cos(angle) +
                      camera_basis[1].z * std::sin(angle)) *
                         line_radius * screen_to_world,
      };
      auto vertex = makeVertex(centre.x, centre.y, centre.z);
      auto second_vertex = makeVertex(endpoint.x, endpoint.y, endpoint.z);
      auto third_vertex = second_vertex;
      const auto depth = RotTransPers3(&vertex, &second_vertex, &third_vertex,
                                       &projected_centre, &projected_second,
                                       &projected_third, &depth_cue, &flags);
      if (depth <= 0) {
        continue;
      }
      auto &line = primitives.combat_effect_lines.emplace_back();
      setLineF2(&line);
      setRGB0(&line, color.red, color.green, color.blue);
      setProjected(&line.x0, projected_centre);
      setProjected(&line.x1, projected_second);
      addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
              &line);
      const std::array projected{projected_centre, projected_second};
      submitStats(stats, depth, projected);
    } else if (particle.kind ==
               game::LegacyCombatParticleKind::blood_impact_triangle) {
      const auto radius =
          static_cast<double>(particle.scale_byte) * screen_to_world;
      const std::array angles{
          radians(particle.angle),
          radians(static_cast<std::int32_t>(particle.angle) +
                  particle.second_angle),
          radians(static_cast<std::int32_t>(particle.angle) +
                  particle.third_angle),
      };
      const auto point = [&](double angle) {
        return Vector3{
            centre.x + (camera_basis[0].x * std::cos(angle) +
                        camera_basis[1].x * std::sin(angle)) *
                           radius,
            centre.y + (camera_basis[0].y * std::cos(angle) +
                        camera_basis[1].y * std::sin(angle)) *
                           radius,
            centre.z + (camera_basis[0].z * std::cos(angle) +
                        camera_basis[1].z * std::sin(angle)) *
                           radius,
        };
      };
      const auto first = point(angles[0]);
      const auto second = point(angles[1]);
      const auto third = point(angles[2]);
      auto vertex = makeVertex(first.x, first.y, first.z);
      auto second_vertex = makeVertex(second.x, second.y, second.z);
      auto third_vertex = makeVertex(third.x, third.y, third.z);
      const auto depth = RotTransPers3(&vertex, &second_vertex, &third_vertex,
                                       &projected_centre, &projected_second,
                                       &projected_third, &depth_cue, &flags);
      if (depth <= 0) {
        continue;
      }
      auto &triangle = primitives.combat_effect_triangles.emplace_back();
      setPolyF3(&triangle);
      if (particle.semi_transparent) {
        setSemiTrans(&triangle, 1);
      }
      setRGB0(&triangle, color.red, color.green, color.blue);
      setProjected(&triangle.x0, projected_centre);
      setProjected(&triangle.x1, projected_second);
      setProjected(&triangle.x2, projected_third);
      addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
              &triangle);
      const std::array projected{projected_centre, projected_second,
                                 projected_third};
      submitStats(stats, depth, projected);
    }
  }
}

void renderEffectSpriteInPlane(Vector3 centre, double half_width,
                               double half_height, Vector3 horizontal_axis,
                               Vector3 vertical_axis,
                               const EffectSpritePlacement &sprite,
                               std::vector<OT_TAG> &ordering_table,
                               PrimitiveBuffer &primitives,
                               RenderStats &stats) {
  const auto vertex = [&](double horizontal, double vertical, double u,
                          double v) {
    return TexturedVertex{
        centre.x + horizontal_axis.x * horizontal + vertical_axis.x * vertical,
        centre.y + horizontal_axis.y * horizontal + vertical_axis.y * vertical,
        centre.z + horizontal_axis.z * horizontal + vertical_axis.z * vertical,
        u,
        v,
        128.0,
        128.0,
        128.0,
    };
  };
  const std::array quad{
      vertex(-half_width, -half_height, sprite.minimum_u, sprite.minimum_v),
      vertex(half_width, -half_height, sprite.maximum_u, sprite.minimum_v),
      vertex(-half_width, half_height, sprite.minimum_u, sprite.maximum_v),
      vertex(half_width, half_height, sprite.maximum_u, sprite.maximum_v),
  };
  auto v0 = makeVertex(quad[0].x, quad[0].y, quad[0].z);
  auto v1 = makeVertex(quad[1].x, quad[1].y, quad[1].z);
  auto v2 = makeVertex(quad[2].x, quad[2].y, quad[2].z);
  auto v3 = makeVertex(quad[3].x, quad[3].y, quad[3].z);
  long xy0{};
  long xy1{};
  long xy2{};
  long xy3{};
  long depth_cue{};
  long flags{};
  const auto depth = RotTransPers4(&v0, &v1, &v2, &v3, &xy0, &xy1, &xy2, &xy3,
                                   &depth_cue, &flags);
  if (depth <= 0) {
    return;
  }
  auto &primitive = primitives.effect_sprite_quads.emplace_back();
  configurePrimitive(
      primitive, quad,
      TexturedMaterial{sprite.texture_page, sprite.clut, true, true});
  setProjected(&primitive.x0, xy0);
  setProjected(&primitive.x1, xy1);
  setProjected(&primitive.x2, xy2);
  setProjected(&primitive.x3, xy3);
  addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
          &primitive);
  const std::array projected{xy0, xy1, xy2, xy3};
  submitStats(stats, depth, projected);
}

void renderEffectSprite(Vector3 centre, double half_width, double half_height,
                        const EffectSpritePlacement &sprite,
                        const game::CameraState &camera,
                        std::vector<OT_TAG> &ordering_table,
                        PrimitiveBuffer &primitives, RenderStats &stats) {
  const auto basis = viewBasis(camera);
  renderEffectSpriteInPlane(centre, half_width, half_height, basis[0], basis[1],
                            sprite, ordering_table, primitives, stats);
}

void renderMuzzleFlash(Vector3 muzzle, double scale, std::uint64_t phase_seed,
                       const EffectSpritePlacement &sprite,
                       const game::CameraState &camera,
                       std::vector<OT_TAG> &ordering_table,
                       PrimitiveBuffer &primitives, RenderStats &stats) {
  const auto basis = viewBasis(camera);
  // The texture is a radial star, so it stays centred on the barrel and only
  // receives a small per-shot rotation. Orienting it along the shot ray moved
  // the whole quad forward and made the burst read as a crooked flame tail.
  const auto angle = static_cast<double>(phase_seed & 0xffU) *
                     (2.0 * std::numbers::pi / 256.0);
  const auto cosine = std::cos(angle);
  const auto sine = std::sin(angle);
  const auto horizontal_axis = Vector3{
      basis[0].x * cosine + basis[1].x * sine,
      basis[0].y * cosine + basis[1].y * sine,
      basis[0].z * cosine + basis[1].z * sine,
  };
  const auto vertical_axis = Vector3{
      -basis[0].x * sine + basis[1].x * cosine,
      -basis[0].y * sine + basis[1].y * cosine,
      -basis[0].z * sine + basis[1].z * cosine,
  };
  const auto safe_scale = std::clamp(scale, 0.5, 1.25);
  const auto half_extent = 24.0 * safe_scale;
  renderEffectSpriteInPlane(muzzle, half_extent, half_extent, horizontal_axis,
                            vertical_axis, sprite, ordering_table, primitives,
                            stats);
}

[[nodiscard]] bool weaponUsesRetailMuzzleFlash(game::WeaponId weapon) noexcept {
  if (!game::isValidWeaponId(weapon)) {
    return false;
  }
  const auto damage_kind = game::weaponCombatDefinition(weapon).damage_kind;
  return damage_kind == game::WeaponDamageKind::ballistic ||
         damage_kind == game::WeaponDamageKind::pellet;
}

void renderWeaponEffects(
    const game::GameplaySession &gameplay, std::uint64_t tick,
    std::span<const game::LegacyWeaponEventBridgeState> weapon_edges,
    std::span<const game::LegacyMuzzleFlashPresentationState>
        retail_muzzle_flashes,
    std::span<const game::GameplayEffect> native_muzzle_flashes,
    std::span<const game::LegacyLineParticleBridgeState> retail_lines,
    std::span<const game::LegacyCombatParticleBridgeState> combat_particles,
    const FireTexturePlacement &fire_texture_placement,
    const CombatEffectTextureAtlas &effect_textures,
    const game::ActorAnimationBank &actor_animations,
    const RenderPresentationSnapshot &presentation, const MATRIX &view,
    std::vector<OT_TAG> &ordering_table,
    std::vector<OT_TAG> &fire_ordering_table, PrimitiveBuffer &primitives,
    RenderStats &stats, WeaponEffectsScratch &scratch) {
  const auto &presentation_camera = presentation.camera;
  const auto &shot = gameplay.lastShot();
  const auto &player = gameplay.player();
  const auto guest_effects_authoritative =
      game::legacyGuestEffectsAuthoritative(
          presentation.guest_camera_lists_captured,
          gameplay.legacyEffectParticlesAuthoritative());
  if (!presentation.guest_camera_lists_captured) {
    // Render modes 0/2 are already present as exact raw camera-list packets.
    // This reconstruction remains only for non-retail/fallback presentation.
    renderRetailCombatParticles(gameplay, combat_particles, presentation,
                                ordering_table, primitives, stats);
  }
  if (const auto taser_target = gameplay.taserTarget()) {
    const auto *target = gameplay.npcState(*taser_target);
    if (target == nullptr) {
      // A killed/despawned target breaks both the conductor and arc.
    } else {
      auto tether_origin = game::GameplayEffect{};
      tether_origin.x = player.x;
      tether_origin.y = player.y - 245.0;
      tether_origin.z = player.z;
      tether_origin.attachment = game::GameplayEffectAttachment::player_muzzle;
      const auto start = attachedEffectCentre(gameplay, actor_animations,
                                              presentation, tether_origin);
      auto tether_target = game::GameplayEffect{};
      tether_target.attachment = game::GameplayEffectAttachment::npc_body;
      tether_target.owner_object = *taser_target;
      tether_target.attachment_offset_y = -190.0;
      const auto end = attachedEffectCentre(gameplay, actor_animations,
                                            presentation, tether_target);
      auto previous_wire = start;
      auto previous_arc = start;
      constexpr std::size_t segment_count = 12U;
      const auto basis = viewBasis(presentation_camera);
      for (std::size_t segment = 1U; segment <= segment_count; ++segment) {
        const auto amount =
            static_cast<double>(segment) / static_cast<double>(segment_count);
        const auto wire_point = Vector3{
            std::lerp(start.x, end.x, amount),
            std::lerp(start.y, end.y, amount),
            std::lerp(start.z, end.z, amount),
        };
        const auto jitter =
            segment == segment_count
                ? 0.0
                : std::sin(static_cast<double>(tick * 5U + segment * 11U)) *
                      18.0;
        const auto arc_point = Vector3{
            wire_point.x + basis[0].x * jitter,
            wire_point.y + basis[0].y * jitter,
            wire_point.z + basis[0].z * jitter,
        };
        renderEffectLine(
            previous_wire, wire_point, VertexColor{104U, 104U, 96U},
            VertexColor{184U, 176U, 144U}, ordering_table, primitives, stats);
        renderEffectLine(previous_arc, arc_point, VertexColor{180U, 220U, 255U},
                         VertexColor{80U, 140U, 255U}, ordering_table,
                         primitives, stats);
        previous_wire = wire_point;
        previous_arc = arc_point;
      }
    }
  }

  // Reproject semantic LINE_G2 particles through the native/interpolated
  // camera. Their matching 20 Hz raw packets are suppressed below, otherwise
  // rain splashes and weapon trails slide over walls as screen-space overlays.
  const auto camera_basis = viewBasis(presentation_camera);
  for (const auto &line : retail_lines) {
    auto first = Vector3{static_cast<double>(line.first.x),
                         -static_cast<double>(line.first.y),
                         static_cast<double>(line.first.z)};
    auto second = Vector3{static_cast<double>(line.second.x),
                          -static_cast<double>(line.second.y),
                          static_cast<double>(line.second.z)};
    if (line.kind == game::LegacyLineParticleKind::rain_splash) {
      const auto view_depth =
          cameraDepth(view, makeVertex(first.x, first.y, first.z));
      if (view_depth <= 0.0) {
        continue;
      }
      const auto projection =
          static_cast<double>(std::max(presentation_camera.projection, 1));
      const auto half_width =
          static_cast<double>(line.screen_half_width) * view_depth / projection;
      second = Vector3{first.x + camera_basis[0].x * half_width, first.y,
                       first.z + camera_basis[0].z * half_width};
      first = Vector3{first.x - camera_basis[0].x * half_width, first.y,
                      first.z - camera_basis[0].z * half_width};
    }
    if (line.kind == game::LegacyLineParticleKind::ballistic_tracer) {
      const auto matches_retail_path = [&](const auto &item) {
        if (item.type != game::LegacyWeaponEventType::shot ||
            !game::isValidWeaponId(static_cast<game::WeaponId>(item.weapon)) ||
            (line.source_slot >= 0 && item.actor_slot != line.source_slot)) {
          return false;
        }
        if (line.source_slot >= 0) {
          return true;
        }
        // Controller +0x20 may be the allocator's normal -1. Pair that line
        // with a retained player edge only when its retail head lies on the
        // same ray, so an unrelated NPC tracer never acquires Gabe's muzzle.
        const auto origin = Vector3{static_cast<double>(item.origin.x),
                                    -static_cast<double>(item.origin.y),
                                    static_cast<double>(item.origin.z)};
        const auto endpoint = Vector3{static_cast<double>(item.endpoint.x),
                                      -static_cast<double>(item.endpoint.y),
                                      static_cast<double>(item.endpoint.z)};
        const auto ray = Vector3{endpoint.x - origin.x, endpoint.y - origin.y,
                                 endpoint.z - origin.z};
        const auto ray_length_squared =
            ray.x * ray.x + ray.y * ray.y + ray.z * ray.z;
        if (ray_length_squared <= 1.0) {
          return false;
        }
        const auto to_head = Vector3{second.x - origin.x, second.y - origin.y,
                                     second.z - origin.z};
        const auto amount =
            (to_head.x * ray.x + to_head.y * ray.y + to_head.z * ray.z) /
            ray_length_squared;
        if (amount < -0.05 || amount > 1.05) {
          return false;
        }
        const auto nearest =
            Vector3{origin.x + ray.x * amount, origin.y + ray.y * amount,
                    origin.z + ray.z * amount};
        const auto dx = second.x - nearest.x;
        const auto dy = second.y - nearest.y;
        const auto dz = second.z - nearest.z;
        constexpr double maximum_ray_error = 384.0;
        return dx * dx + dy * dy + dz * dz <=
               maximum_ray_error * maximum_ray_error;
      };
      const auto edge = std::find_if(weapon_edges.rbegin(), weapon_edges.rend(),
                                     matches_retail_path);
      const auto source_slot = line.source_slot >= 0         ? line.source_slot
                               : edge != weapon_edges.rend() ? edge->actor_slot
                                                             : std::int16_t{-1};
      const auto weapon = edge != weapon_edges.rend()
                              ? static_cast<game::WeaponId>(edge->weapon)
                              : gameplay.hud().inventory().current();
      const auto presented_source = weaponMuzzleAnchorForGuestSlot(
          gameplay, actor_animations, &presentation, source_slot, weapon);
      if (presented_source) {
        // FUN_800558c0 already publishes the exact world-space forward
        // endpoint.  Anchor only the tail to the posed barrel; translating
        // both endpoints makes the complete trajectory miss its retail
        // target whenever actor presentation is interpolated.
        first = *presented_source;
      }
    }
    renderEffectLine(first, second,
                     VertexColor{line.first_color.red, line.first_color.green,
                                 line.first_color.blue},
                     VertexColor{line.second_color.red, line.second_color.green,
                                 line.second_color.blue},
                     ordering_table, primitives, stats, line.semi_transparent);
  }

  for (const auto &edge : weapon_edges) {
    // Weapon edges are the authoritative accepted-shot signal. Retail's
    // auxiliary sprite/effect pools expose impacts and moving lines, while
    // the muzzle flash is a native textured presentation.
    if (edge.type != game::LegacyWeaponEventType::shot) {
      continue;
    }
    const auto weapon = static_cast<game::WeaponId>(edge.weapon);
    // Only conventional firearms produce the retail muzzle star.  Projectile,
    // thrown, utility, taser and flame events own different effects.
    if (!weaponUsesRetailMuzzleFlash(weapon)) {
      continue;
    }
    const auto &profile = game::weaponCombatDefinition(weapon);
    const auto muzzle = weaponMuzzleAnchorForGuestSlot(
        gameplay, actor_animations, &presentation, edge.actor_slot, weapon);
    if (!muzzle) {
      continue;
    }
    // The optic hides Gabe's own barrel only. NPC muzzle flashes are regular
    // world effects and remain visible through first-person aim.
    if (presentation.first_person_aim &&
        edge.actor_slot == presentation.player_guest_slot) {
      continue;
    }
    const auto flash_scale =
        profile.damage_kind == game::WeaponDamageKind::pellet ? 1.15 : 1.0;
    const auto phase_seed =
        tick * 17U + static_cast<std::uint64_t>(edge.actor_slot + 1) * 43U +
        static_cast<std::uint64_t>(edge.weapon) * 29U;
    renderMuzzleFlash(*muzzle, flash_scale, phase_seed,
                      effect_textures.muzzleFlash(), presentation_camera,
                      ordering_table, primitives, stats);
  }

  for (const auto &flash : retail_muzzle_flashes) {
    // The accepted-shot hook reports Gabe only. A freshly allocated ballistic
    // line supplies the corresponding per-shot edge for enemy firearms.
    if (flash.source_slot == presentation.player_guest_slot) {
      continue;
    }
    const auto muzzle = weaponMuzzleAnchorForGuestSlot(
        gameplay, actor_animations, &presentation, flash.source_slot,
        gameplay.hud().inventory().current());
    if (!muzzle) {
      continue;
    }
    const auto phase_seed =
        flash.sequence * 17U +
        static_cast<std::uint64_t>(flash.source_slot + 1) * 43U +
        static_cast<std::uint64_t>(flash.controller) * 29U + flash.particle;
    renderMuzzleFlash(*muzzle, 1.0, phase_seed, effect_textures.muzzleFlash(),
                      presentation_camera, ordering_table, primitives, stats);
  }

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
        attachedEffectCentre(gameplay, actor_animations, presentation, effect);
    renderMuzzleFlash(centre, effect.scale, effect.seed,
                      effect_textures.muzzleFlash(), presentation_camera,
                      ordering_table, primitives, stats);
  }

  for (const auto &effect : gameplay.effects()) {
    // Muzzle flashes use the catch-up queue above. The live collection owns
    // the remaining multi-tick effects only.
    if (effect.type == game::GameplayEffectType::muzzle_flash) {
      continue;
    }
    if (!game::nativeGameplayEffectPresentationAllowed(
            effect, guest_effects_authoritative,
            presentation.first_person_aim)) {
      continue;
    }
    const auto age =
        static_cast<double>(effect.total_updates - effect.remaining_updates);
    const auto centre =
        attachedEffectCentre(gameplay, actor_animations, presentation, effect);
    switch (effect.type) {
    case game::GameplayEffectType::muzzle_flash:
      break;
    case game::GameplayEffectType::blood_spray: {
      auto random = effect.seed;
      constexpr std::size_t droplet_count = 9U;
      for (std::size_t index = 0U; index < droplet_count; ++index) {
        random = random * 1664525U + 1013904223U;
        const auto horizontal =
            (static_cast<double>((random >> 16U) & 0xffU) / 255.0 - 0.5) * 9.0;
        random = random * 1664525U + 1013904223U;
        const auto vertical =
            (static_cast<double>((random >> 16U) & 0xffU) / 255.0 - 0.65) * 7.0;
        const auto speed =
            (5.0 + static_cast<double>(index) * 0.7) * effect.scale;
        const auto end = Vector3{
            centre.x + effect.direction_x * age * speed +
                camera_basis[0].x * horizontal * age,
            centre.y + effect.direction_y * age * speed + vertical * age +
                age * age * 0.75,
            centre.z + effect.direction_z * age * speed +
                camera_basis[0].z * horizontal * age,
        };
        const auto radius =
            (4.0 + static_cast<double>(index % 3U) * 1.5) * effect.scale;
        renderEffectSprite(end, radius, radius, effect_textures.blood(),
                           presentation_camera, ordering_table, primitives,
                           stats);
      }
      break;
    }
    case game::GameplayEffectType::blood_decal:
      renderEffectSprite(centre, 18.0 * effect.scale, 18.0 * effect.scale,
                         effect_textures.bloodMark(), presentation_camera,
                         ordering_table, primitives, stats);
      break;
    case game::GameplayEffectType::explosion:
    case game::GameplayEffectType::burning_fire:
      break;
    }
  }

  if (shot.fired && shot.weapon == game::WeaponId::flamethrower &&
      !gameplay.legacyEffectParticlesAuthoritative()) {
    const auto fire_model = std::ranges::find_if(
        gameplay.objectModels(), [](const game::ObjectModel &model) {
          return std::holds_alternative<game::ObjectFireEmitter>(
              model.geometry);
        });
    if (fire_model == gameplay.objectModels().end()) {
      return;
    }
    const auto &fire = std::get<game::ObjectFireEmitter>(fire_model->geometry);
    const auto forward = game::headingDirection(presentation.player.yaw);
    auto flame_origin_effect = game::GameplayEffect{};
    flame_origin_effect.x = player.x + forward.x * 95.0;
    flame_origin_effect.y = player.y - 235.0;
    flame_origin_effect.z = player.z + forward.z * 95.0;
    flame_origin_effect.attachment =
        game::GameplayEffectAttachment::player_muzzle;
    const auto flame_origin = attachedEffectCentre(
        gameplay, actor_animations, presentation, flame_origin_effect);
    std::array<FireParticle, 7U> particles{};
    for (std::size_t index = 0; index < particles.size(); ++index) {
      const auto distance = 40.0 + static_cast<double>(index) * 260.0;
      const auto lateral = std::sin(static_cast<double>(tick + index * 3U)) *
                           (30.0 + static_cast<double>(index) * 12.0);
      particles[index].x = static_cast<std::int32_t>(std::lround(
          flame_origin.x + forward.x * distance + forward.z * lateral));
      particles[index].y = static_cast<std::int32_t>(
          std::lround(flame_origin.y - static_cast<double>(index) * 10.0));
      particles[index].z = static_cast<std::int32_t>(std::lround(
          flame_origin.z + forward.z * distance - forward.x * lateral));
      particles[index].frame =
          static_cast<std::uint8_t>((tick + index) % fire.frames.size());
    }
    renderFireObject(fire, particles, fire_texture_placement, view,
                     fire_ordering_table, primitives, stats);
  }

  const auto fire_model = std::ranges::find_if(
      gameplay.objectModels(), [](const game::ObjectModel &model) {
        return std::holds_alternative<game::ObjectFireEmitter>(model.geometry);
      });
  if (fire_model == gameplay.objectModels().end()) {
    return;
  }
  const auto &fire = std::get<game::ObjectFireEmitter>(fire_model->geometry);
  // The camera list contains exact GsSPRITE quads only for particles retail
  // linked into the current camera list. Keep native reconstruction for every
  // live EXPL particle not represented by that exact pool ID. This is what
  // keeps authored fires visible at distance without duplicating near sprites.
  auto &legacy_particles = scratch.legacy_particles;
  legacy_particles.clear();
  legacy_particles.reserve(gameplay.legacyExplParticles().size());
  for (const auto &source : gameplay.legacyExplParticles()) {
    if (game::legacyExplParticleHasGuestSprite(
            source.pool_index, static_cast<std::uint8_t>(source.family),
            presentation.guest_sprites,
            presentation.guest_camera_lists_captured,
            gameplay.legacyEffectParticlesAuthoritative())) {
      continue;
    }
    const auto *frames = legacyEffectFrames(fire, source.family);
    if (frames == nullptr || source.scale_byte == 0U ||
        source.frame >= frames->size()) {
      continue;
    }
    FireParticle particle;
    particle.x = source.x;
    particle.y = source.y;
    particle.z = source.z;
    particle.family = source.family;
    particle.frame = source.frame;
    particle.size =
        static_cast<double>(source.scale_byte) * legacy_effect_scale_unit;
    particle.red = source.red;
    particle.green = source.green;
    particle.blue = source.blue;
    legacy_particles.push_back(particle);
  }
  if (!legacy_particles.empty()) {
    renderFireObject(fire, legacy_particles, fire_texture_placement, view,
                     fire_ordering_table, primitives, stats);
  }
  // PARK2's FT4 ribbon pool is not part of the +0x90 sprite list and its
  // textured opcode is deliberately absent from the +0x98 whitelist.
  renderPark2FlamethrowerRibbons(gameplay.legacyPark2FlamethrowerRibbons(),
                                 fire, fire_texture_placement,
                                 presentation.guest_packet_camera, view,
                                 fire_ordering_table, primitives, stats);
  if (!guest_effects_authoritative) {
    for (const auto &projectile : gameplay.projectiles()) {
      if (!projectile.active ||
          projectile.phase != game::ProjectilePhase::explosion) {
        continue;
      }
      std::array<FireParticle, 6U> particles{};
      for (std::size_t index = 0; index < particles.size(); ++index) {
        const auto angle =
            static_cast<double>(index) *
            (2.0 * std::numbers::pi / static_cast<double>(particles.size()));
        const auto expansion =
            80.0 + static_cast<double>(projectile.age_updates) * 24.0;
        particles[index].x = static_cast<std::int32_t>(
            std::lround(projectile.x + std::cos(angle) * expansion));
        particles[index].y = static_cast<std::int32_t>(std::lround(
            projectile.y - std::sin(angle * 2.0) * expansion * 0.45));
        particles[index].z = static_cast<std::int32_t>(
            std::lround(projectile.z + std::sin(angle) * expansion));
        particles[index].frame =
            static_cast<std::uint8_t>(std::min<std::size_t>(
                projectile.age_updates / 2U, fire.frames.size() - 1U));
      }
      renderFireObject(fire, particles, fire_texture_placement, view,
                       fire_ordering_table, primitives, stats);
    }
    for (const auto &effect : gameplay.effects()) {
      if (effect.type != game::GameplayEffectType::explosion &&
          effect.type != game::GameplayEffectType::burning_fire) {
        continue;
      }
      if (effect.type == game::GameplayEffectType::burning_fire) {
        std::array<FireParticle, 12U> particles{};
        for (std::size_t index = 0U; index < particles.size(); ++index) {
          const auto row = static_cast<double>(index / 4U);
          const auto column = static_cast<double>(index % 4U) - 1.5;
          const auto flicker = std::sin(static_cast<double>(tick) * 0.16 +
                                        static_cast<double>(index) * 1.7);
          particles[index].x = static_cast<std::int32_t>(std::lround(
              effect.x + column * 92.0 * effect.scale + flicker * 24.0));
          particles[index].y = static_cast<std::int32_t>(
              std::lround(effect.y - row * 92.0 - std::abs(flicker) * 55.0));
          particles[index].z = static_cast<std::int32_t>(
              std::lround(effect.z + (row - 1.0) * 105.0 * effect.scale));
          particles[index].frame = static_cast<std::uint8_t>(
              (tick / 4U + index * 2U) % fire.frames.size());
        }
        renderFireObject(fire, particles, fire_texture_placement, view,
                         fire_ordering_table, primitives, stats);
        continue;
      }
      const auto age = static_cast<std::size_t>(effect.total_updates -
                                                effect.remaining_updates);
      std::array<FireParticle, 9U> particles{};
      for (std::size_t index = 0U; index < particles.size(); ++index) {
        const auto angle =
            static_cast<double>(index) *
            (2.0 * std::numbers::pi / static_cast<double>(particles.size()));
        const auto expansion =
            (45.0 + static_cast<double>(age) * 22.0) * effect.scale;
        particles[index].x = static_cast<std::int32_t>(
            std::lround(effect.x + std::cos(angle) * expansion));
        particles[index].y = static_cast<std::int32_t>(std::lround(
            effect.y - 80.0 - std::sin(angle * 2.0) * expansion * 0.4));
        particles[index].z = static_cast<std::int32_t>(
            std::lround(effect.z + std::sin(angle) * expansion));
        particles[index].frame = static_cast<std::uint8_t>(
            std::min<std::size_t>(age / 2U, fire.frames.size() - 1U));
      }
      renderFireObject(fire, particles, fire_texture_placement, view,
                       fire_ordering_table, primitives, stats);
    }
  }
}

void renderEmdObject(
    const assets::EmdScene &scene, const game::SceneObject &object,
    unsigned int texture_bank, const MATRIX &view,
    std::vector<OT_TAG> &ordering_table, PrimitiveBuffer &primitives,
    RenderStats &stats, bool retail_scrim = false,
    bool force_semi_transparent = false) {
  const auto reverse_winding = reversesWinding(object.transform);
  for (auto section_index = std::size_t{};
       section_index < scene.sections().size(); ++section_index) {
    const auto &section = scene.sections()[section_index];
    for (auto polygon_index = std::size_t{};
         polygon_index < section.polygons.size(); ++polygon_index) {
      const auto &polygon = section.polygons[polygon_index];
      if (!polygon.renderable || polygon.degenerate) {
        continue;
      }
      const auto texture_source_page = emdTextureSourcePage(scene, polygon);
      auto v0 = transformVertex(section.vertices[polygon.vertex_indices[0]],
                                object.transform);
      auto v1 = transformVertex(section.vertices[polygon.vertex_indices[1]],
                                object.transform);
      auto v2 = transformVertex(section.vertices[polygon.vertex_indices[2]],
                                object.transform);

      long xy0{};
      long xy1{};
      long xy2{};
      long xy3{};
      long depth_cue{};
      long flags{};
      int depth{};
      if (polygon.quad) {
        auto v3 = transformVertex(section.vertices[polygon.vertex_indices[3]],
                                  object.transform);
        const std::array positions{v0, v1, v2, v3};
        const auto near_status = classifyNearPlane(view, positions);
        if (near_status == NearPlaneStatus::outside) {
          ++stats.rejected;
          continue;
        }
        if (near_status == NearPlaneStatus::intersecting) {
          const auto t0 =
              emdTexturedVertex(v0, section, polygon, 0, {}, !retail_scrim);
          const auto t1 =
              emdTexturedVertex(v1, section, polygon, 1, {}, !retail_scrim);
          const auto t2 =
              emdTexturedVertex(v2, section, polygon, 2, {}, !retail_scrim);
          const auto t3 =
              emdTexturedVertex(v3, section, polygon, 3, {}, !retail_scrim);
          const std::array first_triangle{t0, t1, t2};
          const std::array second_triangle{t1, t3, t2};
          const TexturedMaterial material{
              polygon.texture_page,
              polygon.clut,
              force_semi_transparent,
              false,
              static_cast<std::uint8_t>(texture_bank),
              texture_source_page,
          };
          submitClippedTriangle(first_triangle, material, false,
                                reverse_winding, view, ordering_table,
                                primitives.objects, stats, worldOrderingDepth,
                                false, !retail_scrim);
          submitClippedTriangle(second_triangle, material, false,
                                reverse_winding, view, ordering_table,
                                primitives.objects, stats, worldOrderingDepth,
                                false, !retail_scrim);
          continue;
        }
        depth = RotTransPers4(&v0, &v1, &v2, &v3, &xy0, &xy1, &xy2, &xy3,
                              &depth_cue, &flags);
        if (depth <= 0 || frontFacing(xy0, xy1, xy2, 4U) == reverse_winding) {
          ++stats.rejected;
          continue;
        }
        auto &primitive = primitives.object_quads.emplace_back();
        configurePrimitive(primitive, section, polygon, texture_bank,
                           texture_source_page);
        if (force_semi_transparent) {
          setSemiTrans(&primitive, 1);
        }
        if (!retail_scrim) {
          dynamicallyLight(primitive, positions);
        }
        setProjected(&primitive.x0, xy0);
        setProjected(&primitive.x1, xy1);
        setProjected(&primitive.x2, xy2);
        setProjected(&primitive.x3, xy3);
        if (!retail_scrim) {
          depthCuePrimitive(primitive,
                            {retailObjectDepthCue(cameraDepth(view, v0)),
                             retailObjectDepthCue(cameraDepth(view, v1)),
                             retailObjectDepthCue(cameraDepth(view, v2)),
                             retailObjectDepthCue(cameraDepth(view, v3))});
        }
        addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
                &primitive);
        const std::array projected{xy0, xy1, xy2, xy3};
        submitStats(stats, depth, projected);
      } else {
        const std::array positions{v0, v1, v2};
        const auto near_status = classifyNearPlane(view, positions);
        if (near_status == NearPlaneStatus::outside) {
          ++stats.rejected;
          continue;
        }
        if (near_status == NearPlaneStatus::intersecting) {
          const std::array textured{
              emdTexturedVertex(v0, section, polygon, 0, {}, !retail_scrim),
              emdTexturedVertex(v1, section, polygon, 1, {}, !retail_scrim),
              emdTexturedVertex(v2, section, polygon, 2, {}, !retail_scrim),
          };
          submitClippedTriangle(
              textured,
              TexturedMaterial{polygon.texture_page, polygon.clut,
                               force_semi_transparent, false,
                               static_cast<std::uint8_t>(texture_bank),
                               texture_source_page},
              false, reverse_winding, view, ordering_table, primitives.objects,
              stats, worldOrderingDepth, false, !retail_scrim);
          continue;
        }
        depth =
            RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
        if (depth <= 0 || frontFacing(xy0, xy1, xy2, 3U) == reverse_winding) {
          ++stats.rejected;
          continue;
        }
        auto &primitive = primitives.objects.emplace_back();
        configurePrimitive(primitive, section, polygon, texture_bank,
                           texture_source_page);
        if (force_semi_transparent) {
          setSemiTrans(&primitive, 1);
        }
        if (!retail_scrim) {
          dynamicallyLight(primitive, positions);
        }
        setProjected(&primitive.x0, xy0);
        setProjected(&primitive.x1, xy1);
        setProjected(&primitive.x2, xy2);
        if (!retail_scrim) {
          depthCuePrimitive(primitive,
                            {retailObjectDepthCue(cameraDepth(view, v0)),
                             retailObjectDepthCue(cameraDepth(view, v1)),
                             retailObjectDepthCue(cameraDepth(view, v2))});
        }
        addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
                &primitive);
        const std::array projected{xy0, xy1, xy2};
        submitStats(stats, depth, projected);
      }
    }
  }
}

std::span<const std::uint16_t>
liveWorldVertexColors(const RenderPresentationSnapshot &presentation,
                      std::uint16_t model, std::uint16_t section,
                      std::size_t expected_vertices);

VertexColor localSceneBackColor(const game::GameplaySession &gameplay,
                                const RenderPresentationSnapshot &presentation,
                                std::size_t object_index,
                                const game::SceneObject &object,
                                VertexColor fallback,
                                LocalSceneLightingCache &cache) {
  const auto origin = transformPoint(0.0, 0.0, 0.0, object.transform);
  struct SampledLighting {
    double vertical_distance{};
    VertexColor color{};
  };
  const auto sample_triangle = [&](const assets::EmdSection &section,
                                   const assets::EmdPolygon &polygon,
                                   std::span<const std::uint16_t> live_colors,
                                   std::array<std::size_t, 3U> corners)
      -> std::optional<SampledLighting> {
    const auto point = [&](std::size_t corner) {
      const auto &source =
          section.vertices[polygon.vertex_indices[corners[corner]]];
      return game::DynamicLightPoint{static_cast<double>(source.x),
                                     static_cast<double>(source.y),
                                     static_cast<double>(source.z)};
    };
    const auto color = [&](std::size_t corner) {
      const auto vertex = polygon.vertex_indices[corners[corner]];
      const auto decoded =
          decodeColor(live_colors.empty() ? section.vertices[vertex].color
                                          : live_colors[vertex]);
      return game::DynamicLightVertexColor{decoded.red, decoded.green,
                                           decoded.blue};
    };
    const auto sample = game::sampleSceneTriangleLighting(
        {point(0U), point(1U), point(2U)}, {color(0U), color(1U), color(2U)},
        {origin.x, origin.y, origin.z});
    if (!sample) {
      return std::nullopt;
    }
    return SampledLighting{
        .vertical_distance = std::abs(sample->surface_y - origin.y),
        .color = {sample->color.red, sample->color.green, sample->color.blue},
    };
  };
  const auto sample_reference = [&](const LocalSceneLightingSample &reference)
      -> std::optional<VertexColor> {
    if (reference.model >= gameplay.models().size()) {
      return std::nullopt;
    }
    const auto &scene = gameplay.models()[reference.model].scene;
    if (reference.section >= scene.sections().size()) {
      return std::nullopt;
    }
    const auto &section = scene.sections()[reference.section];
    if (reference.polygon >= section.polygons.size()) {
      return std::nullopt;
    }
    const auto &polygon = section.polygons[reference.polygon];
    const auto live_colors =
        liveWorldVertexColors(presentation, reference.model, reference.section,
                              section.vertices.size());
    const auto sampled =
        sample_triangle(section, polygon, live_colors, reference.corners);
    return sampled ? std::optional<VertexColor>{sampled->color} : std::nullopt;
  };
  if (const auto *cached = cache.find(object_index, object.transform)) {
    if (!cached->sample) {
      return fallback;
    }
    if (const auto sampled = sample_reference(*cached->sample)) {
      return *sampled;
    }
  }

  auto nearest_vertical_distance = std::numeric_limits<double>::max();
  auto result = fallback;
  std::optional<LocalSceneLightingSample> nearest_reference;
  const auto consider = [&](std::uint16_t model_index,
                            std::uint16_t section_index,
                            std::uint32_t polygon_index,
                            const assets::EmdSection &section,
                            const assets::EmdPolygon &polygon,
                            std::span<const std::uint16_t> live_colors,
                            std::array<std::size_t, 3U> corners) {
    const auto sample = sample_triangle(section, polygon, live_colors, corners);
    if (!sample) {
      return;
    }
    // Crate roots sit above their supporting plane. This bound spans the
    // complete retail crate while rejecting another storey of the map.
    constexpr auto maximum_vertical_distance = 768.0;
    if (sample->vertical_distance > maximum_vertical_distance ||
        sample->vertical_distance >= nearest_vertical_distance) {
      return;
    }
    nearest_vertical_distance = sample->vertical_distance;
    result = sample->color;
    nearest_reference = LocalSceneLightingSample{
        .model = model_index,
        .section = section_index,
        .polygon = polygon_index,
        .corners = corners,
    };
  };
  for (const auto model_index : gameplay.presentationModels()) {
    if (model_index >= gameplay.models().size()) {
      continue;
    }
    const auto &scene = gameplay.models()[model_index].scene;
    for (std::size_t section_index = 0U;
         section_index < scene.sections().size(); ++section_index) {
      const auto &section = scene.sections()[section_index];
      const auto live_colors = liveWorldVertexColors(
          presentation, model_index, static_cast<std::uint16_t>(section_index),
          section.vertices.size());
      for (std::size_t polygon_index = 0U;
           polygon_index < section.polygons.size(); ++polygon_index) {
        const auto &polygon = section.polygons[polygon_index];
        if (!polygon.renderable || polygon.degenerate) {
          continue;
        }
        consider(model_index, static_cast<std::uint16_t>(section_index),
                 static_cast<std::uint32_t>(polygon_index), section, polygon,
                 live_colors, {0U, 1U, 2U});
        if (polygon.quad) {
          consider(model_index, static_cast<std::uint16_t>(section_index),
                   static_cast<std::uint32_t>(polygon_index), section, polygon,
                   live_colors, {1U, 3U, 2U});
        }
      }
    }
  }
  cache.store(object_index, object.transform, nearest_reference);
  return result;
}
