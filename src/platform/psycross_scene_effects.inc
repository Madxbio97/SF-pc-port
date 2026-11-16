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
  int volume_kind_override{-1};
  game::LegacyCfireVolumeTuning volume_tuning{};
  bool volume_may_replace_retail_sprite{true};
  bool retail_expl_frame_volume{};
  bool retail_fire_frame_volume{};
  bool persistent_volume{};
  std::int32_t volume_anchor_x{};
  std::int32_t volume_anchor_y{};
  std::int32_t volume_anchor_z{};
  std::uint32_t volume_seed{};
  std::uint8_t frame{};
  double size{fire_particle_size};
  std::uint8_t red{128U};
  std::uint8_t green{128U};
  std::uint8_t blue{128U};
};

constexpr std::uint64_t world_decal_lifetime_ticks = 5U * 20U;
constexpr double world_decal_surface_bias = 1.0;

struct WorldDecalPresentationCacheEntry {
  std::optional<game::LegacyWorldDecalBridgeState> source;
  std::uint64_t born_tick{};
  bool expired{};
};

struct WeaponEffectsScratch {
  std::vector<FireParticle> legacy_particles;
  std::array<WorldDecalPresentationCacheEntry,
             game::legacy_world_decal_capacity>
      world_decals{};
  std::uint64_t world_decal_last_tick{};
  bool world_decal_tick_initialized{};
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
      const auto volume_tuning = game::legacyCfireVolumeTuning(
          gameplay.missionIndex(),
          object.class_id == game::legacy_cfire_a_class);
      for (auto &particle : emitter.particles) {
        particle.volume_kind_override = GR_VOLUME_FIRE;
        particle.volume_tuning = volume_tuning;
        particle.volume_may_replace_retail_sprite =
            game::legacyCfireVolumeMayReplaceRetailSprite(
                gameplay.missionIndex(), true);
        particle.retail_expl_frame_volume =
            game::legacyCfireUsesRetailExplFrameVolume(
                gameplay.missionIndex(), true, particle.family);
        particle.retail_fire_frame_volume =
            game::legacyCfireUsesRetailFireFrameVolume(
                gameplay.missionIndex(), true, particle.family);
        particle.persistent_volume = true;
        particle.volume_anchor_x = emitter.start_x;
        particle.volume_anchor_y = emitter.start_y;
        particle.volume_anchor_z = emitter.start_z;
        particle.volume_seed =
            sf::platform::persistentFireVolumeSeed(index);
      }
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

constexpr std::size_t volumetric_halo_reserve = 32U;
constexpr std::size_t volumetric_effect_budget =
    game::legacy_effect_particle_capacity + volumetric_halo_reserve;
constexpr double pgxp_camera_unit = 128.0;

std::optional<int>
volumetricKind(game::LegacyEffectSpriteFamily family) noexcept {
  switch (family) {
  case game::LegacyEffectSpriteFamily::fire:
    return GR_VOLUME_FIRE;
  case game::LegacyEffectSpriteFamily::explosion:
    return GR_VOLUME_EXPLOSION;
  case game::LegacyEffectSpriteFamily::breath:
    return GR_VOLUME_SMOKE;
  case game::LegacyEffectSpriteFamily::vapor:
    return GR_VOLUME_FOG;
  }
  return std::nullopt;
}

struct PersistentFireEmitterSource {
  Vector3 anchor{};
  std::uint32_t seed{};
  std::size_t identity{};
};

std::optional<PersistentFireEmitterSource> persistentFireEmitterSource(
    const game::GameplaySession &gameplay, std::int16_t source_slot) noexcept {
  if (source_slot < 0) {
    return std::nullopt;
  }
  const auto guest_slots = gameplay.legacyGuestSlotsBySceneObject();
  const auto &objects = gameplay.objects();
  const auto &models = gameplay.objectModels();
  for (auto index = std::size_t{}; index < objects.size(); ++index) {
    const auto guest_slot =
        index < guest_slots.size() ? guest_slots[index] : std::int32_t{-1};
    if (!sf::platform::persistentFireSourceMatchesSceneObject(
            gameplay.missionIndex(), source_slot, guest_slot,
            objects[index].source_index) ||
        objects[index].model >= models.size() ||
        !game::legacyFireEmitterPresentation(
            objects[index].class_id, models[objects[index].model].name) ||
        !std::holds_alternative<game::ObjectFireEmitter>(
            models[objects[index].model].geometry)) {
      continue;
    }
    const auto spawn = game::cfireSpawnPoint(objects[index].transform);
    return PersistentFireEmitterSource{
        Vector3{static_cast<double>(spawn.x), static_cast<double>(spawn.y),
                static_cast<double>(spawn.z)},
        sf::platform::persistentFireVolumeSeed(index), index};
  }
  return std::nullopt;
}

std::optional<PersistentFireEmitterSource> persistentFireEmitterSource(
    const game::GameplaySession &gameplay,
    std::span<const game::LegacyExplParticle> particles,
    const game::LegacyExplParticle &source) noexcept {
  if (const auto direct =
          persistentFireEmitterSource(gameplay, source.source_slot)) {
    return direct;
  }
  if (!game::legacyUnboundCfireParticleCandidate(
          source.family, source.scale_byte,
          source.attached_explosion_sequence)) {
    return std::nullopt;
  }

  auto controller_group =
      std::vector<sf::platform::PersistentFireParticlePoint>{};
  controller_group.reserve(2U);
  for (const auto &particle : particles) {
    if (particle.controller != source.controller) {
      continue;
    }
    if (particle.source_slot != source.source_slot ||
        !game::legacyUnboundCfireParticleCandidate(
            particle.family, particle.scale_byte,
            particle.attached_explosion_sequence)) {
      return std::nullopt;
    }
    controller_group.push_back({particle.x, particle.y, particle.z});
  }

  const auto &objects = gameplay.objects();
  const auto &models = gameplay.objectModels();
  auto candidates =
      std::vector<sf::platform::PersistentFireEmitterCandidate>{};
  for (auto index = std::size_t{}; index < objects.size(); ++index) {
    if (index > std::numeric_limits<std::uint16_t>::max() ||
        objects[index].model >= models.size()) {
      continue;
    }
    const auto scene_index = static_cast<std::uint16_t>(index);
    const auto &model = models[objects[index].model];
    if (!game::legacyFireEmitterPresentation(objects[index].class_id,
                                              model.name) ||
        !std::holds_alternative<game::ObjectFireEmitter>(model.geometry) ||
        gameplay.objectDestroyed(scene_index) ||
        std::ranges::find(gameplay.activeObjects(), scene_index) ==
            gameplay.activeObjects().end()) {
      continue;
    }
    const auto rooms = gameplay.authoredObjectRooms(scene_index);
    const auto presented = std::ranges::any_of(rooms, [&](const auto room) {
      return std::ranges::find(gameplay.presentationModels(), room) !=
             gameplay.presentationModels().end();
    });
    if (!presented) {
      continue;
    }
    const auto spawn = game::cfireSpawnPoint(objects[index].transform);
    candidates.push_back({index, {spawn.x, spawn.y, spawn.z}});
  }
  const auto match = sf::platform::uniquePersistentFireEmitterMatch(
      controller_group, candidates);
  if (!match || *match >= objects.size()) {
    return std::nullopt;
  }
  const auto spawn = game::cfireSpawnPoint(objects[*match].transform);
  return PersistentFireEmitterSource{
      Vector3{static_cast<double>(spawn.x), static_cast<double>(spawn.y),
              static_cast<double>(spawn.z)},
      sf::platform::persistentFireVolumeSeed(*match), *match};
}

bool legacyParticleGroupOwnsFireEmitter(
    const game::GameplaySession &gameplay,
    std::span<const game::LegacyExplParticle> particles,
    std::uint16_t controller, std::int16_t source_slot,
    game::LegacyEffectSpriteFamily family) noexcept {
  const auto source = std::ranges::find_if(particles, [&](const auto &item) {
    return item.controller == controller && item.source_slot == source_slot &&
           item.family == family;
  });
  return source != particles.end() &&
         persistentFireEmitterSource(gameplay, particles, *source).has_value();
}

bool legacyFireEmitterHasLiveParticle(
    const game::GameplaySession &gameplay,
    std::span<const game::LegacyExplParticle> particles,
    std::size_t emitter_identity) noexcept {
  for (const auto &particle : particles) {
    if (!game::legacyExplParticlePresentsAuthoredFire(particle.family)) {
      continue;
    }
    const auto owner =
        persistentFireEmitterSource(gameplay, particles, particle);
    if (owner && owner->identity == emitter_identity) {
      return true;
    }
  }
  return false;
}

std::optional<PersistentFireEmitterSource>
legacyGuestPersistentFireEmitterSource(
    const game::GameplaySession &gameplay,
    std::span<const game::LegacyExplParticle> particles,
    const game::LegacyGuestSpriteBridgeState &sprite) noexcept {
  if (sprite.effect_particle < 0 ||
      sprite.effect_family > static_cast<std::uint8_t>(
                                 game::LegacyEffectSpriteFamily::vapor)) {
    return std::nullopt;
  }
  const auto family =
      static_cast<game::LegacyEffectSpriteFamily>(sprite.effect_family);
  if (!game::legacyExplParticlePresentsAuthoredFire(family)) {
    return std::nullopt;
  }
  const auto source = std::ranges::find_if(particles, [&](const auto &particle) {
    return particle.pool_index == sprite.effect_particle &&
           static_cast<std::uint8_t>(particle.family) == sprite.effect_family;
  });
  return source != particles.end()
             ? persistentFireEmitterSource(gameplay, particles, *source)
             : std::nullopt;
}

using VolumetricWorldBasis = std::array<Vector3, 3>;

VolumetricWorldBasis orientedVolumetricBasis(Vector3 rise_direction) noexcept {
  const auto length_squared = rise_direction.x * rise_direction.x +
                              rise_direction.y * rise_direction.y +
                              rise_direction.z * rise_direction.z;
  if (!std::isfinite(length_squared) || length_squared <= 0.000001) {
    rise_direction = Vector3{0.0, -1.0, 0.0};
  } else {
    const auto inverse_length = 1.0 / std::sqrt(length_squared);
    rise_direction.x *= inverse_length;
    rise_direction.y *= inverse_length;
    rise_direction.z *= inverse_length;
  }
  // The atlas' negative local Y is the rising/tip direction, so positive
  // local Y points back towards the emitter.
  const auto local_y = Vector3{-rise_direction.x, -rise_direction.y,
                               -rise_direction.z};
  const auto reference = std::abs(local_y.z) < 0.90
                             ? Vector3{0.0, 0.0, 1.0}
                             : Vector3{1.0, 0.0, 0.0};
  const auto local_x = normalize(cross(local_y, reference));
  const auto local_z = normalize(cross(local_x, local_y));
  return {local_x, local_y, local_z};
}

Vector3 volumetricCameraExtents(const GrVolumetricEffect &effect) noexcept {
  return {
      std::abs(effect.axis_x_x) * effect.radius_x +
          std::abs(effect.axis_y_x) * effect.radius_y +
          std::abs(effect.axis_z_x) * effect.radius_z,
      std::abs(effect.axis_x_y) * effect.radius_x +
          std::abs(effect.axis_y_y) * effect.radius_y +
          std::abs(effect.axis_z_y) * effect.radius_z,
      std::abs(effect.axis_x_z) * effect.radius_x +
          std::abs(effect.axis_y_z) * effect.radius_y +
          std::abs(effect.axis_z_z) * effect.radius_z,
  };
}

bool enqueueVolumetricEffect(
    const Vector3 &world_center, const Vector3 &world_radius, int kind,
    double red, double green, double blue, double density, double emission,
    double phase, std::uint32_t seed, const game::CameraState &camera,
    std::vector<GrVolumetricEffect> &effects,
    double presentation_amount, const VolumetricWorldBasis *world_basis,
    unsigned int flags) {
  const auto available = GR_VolumetricEffectsAvailable();
  if (available == 0 || !std::isfinite(presentation_amount)) {
    return false;
  }
  presentation_amount = std::clamp(presentation_amount, 0.0, 1.0);
  if (presentation_amount <= 0.0) {
    return true;
  }
  if (!std::isfinite(world_center.x) || !std::isfinite(world_center.y) ||
      !std::isfinite(world_center.z) || !std::isfinite(world_radius.x) ||
      !std::isfinite(world_radius.y) || !std::isfinite(world_radius.z) ||
      world_radius.x <= 0.0 || world_radius.y <= 0.0 ||
      world_radius.z <= 0.0) {
    return false;
  }

  const auto basis = viewBasis(camera);
  const auto delta = Vector3{world_center.x - camera.x,
                             world_center.y - camera.y,
                             world_center.z - camera.z};
  const auto camera_depth = delta.x * basis[2].x + delta.y * basis[2].y +
                            delta.z * basis[2].z;
  const auto component = [&](const Vector3 &axis) {
    return (delta.x * axis.x + delta.y * axis.y + delta.z * axis.z) /
           pgxp_camera_unit;
  };
  GrVolumetricEffect effect{};
  effect.center_x = static_cast<float>(component(basis[0]));
  effect.center_y = static_cast<float>(component(basis[1]));
  effect.center_z = static_cast<float>(component(basis[2]));
  effect.radius_x = static_cast<float>(world_radius.x / pgxp_camera_unit);
  effect.radius_y = static_cast<float>(world_radius.y / pgxp_camera_unit);
  effect.radius_z = static_cast<float>(world_radius.z / pgxp_camera_unit);
  effect.red = static_cast<float>(std::clamp(red, 0.0, 2.0));
  effect.green = static_cast<float>(std::clamp(green, 0.0, 2.0));
  effect.blue = static_cast<float>(std::clamp(blue, 0.0, 2.0));
  const auto emissive = kind == GR_VOLUME_FIRE ||
                        kind == GR_VOLUME_EXPLOSION ||
                        kind == GR_VOLUME_LIGHT_HALO;
  effect.density = static_cast<float>(
      density * (emissive ? 1.0 : presentation_amount));
  effect.emission = static_cast<float>(emission * presentation_amount);
  if (kind == GR_VOLUME_LIGHT_HALO) {
    const auto fog_visibility = game::legacyHaloFogVisibility(
        static_cast<std::int32_t>(retailObjectDepthCue(camera_depth)));
    effect.emission *= static_cast<float>(fog_visibility);
  }
  effect.phase = static_cast<float>(phase);
  effect.seed = static_cast<float>(seed & 0xffffU);
  effect.axis_x_x = 1.0F;
  effect.axis_y_y = 1.0F;
  effect.axis_z_z = 1.0F;
  if (world_basis != nullptr) {
    const auto camera_axis = [&](const Vector3 &axis, const Vector3 &row) {
      return static_cast<float>(axis.x * row.x + axis.y * row.y +
                                axis.z * row.z);
    };
    effect.axis_x_x = camera_axis((*world_basis)[0], basis[0]);
    effect.axis_x_y = camera_axis((*world_basis)[0], basis[1]);
    effect.axis_x_z = camera_axis((*world_basis)[0], basis[2]);
    effect.axis_y_x = camera_axis((*world_basis)[1], basis[0]);
    effect.axis_y_y = camera_axis((*world_basis)[1], basis[1]);
    effect.axis_y_z = camera_axis((*world_basis)[1], basis[2]);
    effect.axis_z_x = camera_axis((*world_basis)[2], basis[0]);
    effect.axis_z_y = camera_axis((*world_basis)[2], basis[1]);
    effect.axis_z_z = camera_axis((*world_basis)[2], basis[2]);
  }
  effect.kind = kind;
  effect.flags = flags;
  const auto camera_extents = volumetricCameraExtents(effect);
  if (!std::isfinite(effect.center_x) || !std::isfinite(effect.center_y) ||
      !std::isfinite(effect.center_z) ||
      effect.center_z + camera_extents.z <= 0.25F ||
      effect.density <= 0.0F) {
    return false;
  }
  // Do not spend the GPU budget on volumes whose complete projected bounds
  // are outside the viewport. They are still owned by the volume path, so an
  // off-screen retail quad cannot consume a later frame's fallback path.
  const auto projection = static_cast<float>(std::max(camera.projection, 1));
  const auto center_depth = std::max(effect.center_z, 0.251F);
  const auto front_depth =
      std::max(static_cast<double>(effect.center_z) - camera_extents.z, 0.251);
  const auto center_screen_x = static_cast<float>(screen_width) * 0.5F +
                               effect.center_x * projection / center_depth;
  const auto center_screen_y = static_cast<float>(screen_height) * 0.5F +
                               effect.center_y * projection / center_depth;
  const auto half_screen_width =
      (camera_extents.x +
       std::abs(effect.center_x) * camera_extents.z / center_depth) *
      projection / front_depth;
  const auto half_screen_height =
      (camera_extents.y +
       std::abs(effect.center_y) * camera_extents.z / center_depth) *
      projection / front_depth;
  if (center_screen_x + half_screen_width < 0.0F ||
      center_screen_x - half_screen_width > static_cast<float>(screen_width) ||
      center_screen_y + half_screen_height < 0.0F ||
      center_screen_y - half_screen_height > static_cast<float>(screen_height)) {
    return true;
  }
  if (kind == GR_VOLUME_FIRE &&
      (flags & GR_VOLUME_FLAG_PERSISTENT_FIRE) != 0U) {
    const auto same_emitter = [&effect](const GrVolumetricEffect &existing) {
      if (existing.kind != GR_VOLUME_FIRE ||
          (existing.flags & GR_VOLUME_FLAG_PERSISTENT_FIRE) == 0U ||
          existing.seed != effect.seed) {
        return false;
      }
      const auto base = [](const GrVolumetricEffect &candidate) {
        return std::array{
            candidate.center_x + candidate.axis_y_x * candidate.radius_y,
            candidate.center_y + candidate.axis_y_y * candidate.radius_y,
            candidate.center_z + candidate.axis_y_z * candidate.radius_y,
        };
      };
      const auto existing_base = base(existing);
      const auto incoming_base = base(effect);
      const auto close = [](float first, float second) {
        return std::abs(first - second) <= 0.02F;
      };
      return close(existing_base[0], incoming_base[0]) &&
             close(existing_base[1], incoming_base[1]) &&
             close(existing_base[2], incoming_base[2]);
    };
    if (const auto duplicate = std::ranges::find_if(effects, same_emitter);
        duplicate != effects.end()) {
      // A CFIRE controller owns multiple retail particles. They describe one
      // flame envelope; keep one GPU volume while accepting the broadest
      // authored sample instead of letting the first 20 Hz packet make it
      // flat or frame-dependent.
      if (effect.radius_y > duplicate->radius_y) {
        // Both candidates share the authored base. Adopt the taller sample's
        // centre and basis before widening the envelope so its floor contact
        // cannot slide when retail particle sizes differ.
        duplicate->center_x = effect.center_x;
        duplicate->center_y = effect.center_y;
        duplicate->center_z = effect.center_z;
        duplicate->axis_x_x = effect.axis_x_x;
        duplicate->axis_x_y = effect.axis_x_y;
        duplicate->axis_x_z = effect.axis_x_z;
        duplicate->axis_y_x = effect.axis_y_x;
        duplicate->axis_y_y = effect.axis_y_y;
        duplicate->axis_y_z = effect.axis_y_z;
        duplicate->axis_z_x = effect.axis_z_x;
        duplicate->axis_z_y = effect.axis_z_y;
        duplicate->axis_z_z = effect.axis_z_z;
      }
      duplicate->radius_x = std::max(duplicate->radius_x, effect.radius_x);
      duplicate->radius_y = std::max(duplicate->radius_y, effect.radius_y);
      duplicate->radius_z = std::max(duplicate->radius_z, effect.radius_z);
      duplicate->red = std::max(duplicate->red, effect.red);
      duplicate->green = std::max(duplicate->green, effect.green);
      duplicate->blue = std::max(duplicate->blue, effect.blue);
      duplicate->density = std::max(duplicate->density, effect.density);
      duplicate->emission = std::max(duplicate->emission, effect.emission);
      return true;
    }
  }
  if (kind == GR_VOLUME_LIGHT_HALO) {
    const auto same_lamp = [&effect](const GrVolumetricEffect &existing) {
      if (existing.kind != GR_VOLUME_LIGHT_HALO) {
        return false;
      }
      const auto close_axis = [](float first, float second, float radius) {
        return std::abs(first - second) <= std::max(radius * 0.18F, 0.08F);
      };
      return close_axis(existing.center_x, effect.center_x,
                        std::min(existing.radius_x, effect.radius_x)) &&
             close_axis(existing.center_y, effect.center_y,
                        std::min(existing.radius_y, effect.radius_y)) &&
             close_axis(existing.center_z, effect.center_z,
                        std::min(existing.radius_z, effect.radius_z));
    };
    if (const auto duplicate = std::ranges::find_if(effects, same_lamp);
        duplicate != effects.end()) {
      // Static GMD fixtures and their guest-authored retail halo can describe
      // the same lamp. Merge the outer glow while retaining the exact sprite
      // once as the hot core; otherwise two additive volumes look synthetic.
      duplicate->radius_x = std::max(duplicate->radius_x, effect.radius_x);
      duplicate->radius_y = std::max(duplicate->radius_y, effect.radius_y);
      duplicate->radius_z = std::max(duplicate->radius_z, effect.radius_z);
      const auto incoming_authored =
          (effect.flags & GR_VOLUME_FLAG_AUTHORED_HALO_COLOR) != 0U;
      const auto existing_authored =
          (duplicate->flags & GR_VOLUME_FLAG_AUTHORED_HALO_COLOR) != 0U;
      if (incoming_authored && !existing_authored) {
        // The guest sprite owns the actual CLUT hue. The GMD duplicate only
        // contributes a stable envelope; component-wise MAX mixed its
        // distance-cued blue back-colour into a magenta corona.
        duplicate->red = effect.red;
        duplicate->green = effect.green;
        duplicate->blue = effect.blue;
      } else if (!existing_authored) {
        duplicate->red = std::max(duplicate->red, effect.red);
        duplicate->green = std::max(duplicate->green, effect.green);
        duplicate->blue = std::max(duplicate->blue, effect.blue);
      }
      duplicate->flags |= effect.flags;
      duplicate->density = std::max(duplicate->density, effect.density);
      duplicate->emission = std::max(duplicate->emission, effect.emission);
      return true;
    }
  }
  // Capacity is presentation ownership, not permission to revive the PS1
  // sprite. Prefer combat/fire volumes over fog and lamp coronas, then keep
  // the largest on-screen member inside each class. Keep a hard reserve for
  // lamp halos so particle-heavy rooms cannot suppress both halo render paths.
  if (effects.size() >= volumetric_effect_budget) {
    const auto priority = [](const GrVolumetricEffect &candidate) {
      const auto tier = candidate.kind == GR_VOLUME_EXPLOSION
                            ? 5.0
                        : candidate.kind == GR_VOLUME_FIRE
                            ? 4.0
                        : candidate.kind == GR_VOLUME_SMOKE
                            ? 3.0
                        : candidate.kind == GR_VOLUME_FOG ? 2.0
                                                         : 1.0;
      const auto extents = volumetricCameraExtents(candidate);
      const auto front = std::max(static_cast<double>(candidate.center_z) - extents.z, 0.251);
      const auto apparent_area = extents.x * extents.y / (front * front);
      return tier * 1000.0 + static_cast<double>(apparent_area);
    };
    const auto incoming_halo = effect.kind == GR_VOLUME_LIGHT_HALO;
    auto halo_count = std::size_t{};
    for (const auto &queued : effects) {
      if (queued.kind == GR_VOLUME_LIGHT_HALO) {
        ++halo_count;
      }
    }
    auto victim = effects.end();
    for (auto candidate = effects.begin(); candidate != effects.end();
         ++candidate) {
      const auto candidate_halo =
          candidate->kind == GR_VOLUME_LIGHT_HALO;
      const auto eligible = incoming_halo
                                ? (halo_count < volumetric_halo_reserve
                                       ? !candidate_halo
                                       : candidate_halo)
                                : (!candidate_halo ||
                                   halo_count > volumetric_halo_reserve);
      if (!eligible) {
        continue;
      }
      if (victim == effects.end() ||
          priority(*candidate) < priority(*victim)) {
        victim = candidate;
      }
    }
    const auto filling_reserved_halo_slot =
        incoming_halo && halo_count < volumetric_halo_reserve;
    if (victim != effects.end() &&
        (filling_reserved_halo_slot ||
         priority(effect) > priority(*victim))) {
      *victim = effect;
    }
    return true;
  }
  effects.push_back(effect);
  return true;
}

bool enqueueFireVolumetricEffect(
    const FireParticle &particle, double half_width, double half_height,
    std::size_t particle_index, std::size_t frame_count,
    const game::CameraState &camera,
    std::vector<GrVolumetricEffect> &effects, double presentation_amount) {
  const auto family_kind = volumetricKind(particle.family);
  if ((particle.volume_kind_override < 0 && !family_kind) ||
      frame_count == 0U) {
    return false;
  }
  const auto kind = particle.volume_kind_override >= 0
                        ? particle.volume_kind_override
                        : *family_kind;
  auto density = 1.0;
  auto emission = 0.0;
  auto depth_scale = 0.70;
  switch (kind) {
  case GR_VOLUME_FIRE:
    density = 1.25;
    emission = 1.20;
    depth_scale = 0.55;
    break;
  case GR_VOLUME_EXPLOSION:
    density = 1.65;
    emission = 1.55;
    depth_scale = 0.82;
    break;
  case GR_VOLUME_SMOKE:
    density = 0.72;
    depth_scale = 0.48;
    break;
  case GR_VOLUME_FOG:
    density = 0.45;
    depth_scale = 0.72;
    break;
  }
  if (kind == GR_VOLUME_FIRE && particle.persistent_volume &&
      !particle.retail_expl_frame_volume &&
      !particle.retail_fire_frame_volume) {
    const auto layout = sf::platform::persistentFireVolumeLayout(
        static_cast<double>(particle.volume_anchor_x),
        static_cast<double>(particle.volume_anchor_y),
        static_cast<double>(particle.volume_anchor_z), half_width,
        half_height, half_width * depth_scale);
    const auto orientation =
        orientedVolumetricBasis(Vector3{0.0, -1.0, 0.0});
    return enqueueVolumetricEffect(
        Vector3{layout.center_x, layout.center_y, layout.center_z},
        Vector3{layout.radius_x, layout.radius_y, layout.radius_z}, kind,
        static_cast<double>(particle.red) / 128.0,
        static_cast<double>(particle.green) / 128.0,
        static_cast<double>(particle.blue) / 128.0,
        density * particle.volume_tuning.density_scale,
        emission * particle.volume_tuning.emission_scale, 0.0,
        particle.volume_seed, camera, effects, presentation_amount,
        &orientation, GR_VOLUME_FLAG_PERSISTENT_FIRE);
  }
  // Presentation noise follows the particle instead of re-seeding on every
  // 20 Hz position/frame update.
  const auto packed_seed =
      static_cast<std::uint32_t>(particle.velocity_x) * 73856093U ^
      static_cast<std::uint32_t>(particle.native_velocity_y_fixed) *
          19349663U ^
      static_cast<std::uint32_t>(particle.velocity_z) * 83492791U ^
      static_cast<std::uint32_t>(particle.total_ticks) * 2654435761U ^
      static_cast<std::uint32_t>(particle_index) * 2246822519U;
  const auto rising_direction = Vector3{
      static_cast<double>(particle.velocity_x),
      -static_cast<double>(particle.native_velocity_y_fixed +
                           fire_native_vertical_acceleration) /
          4096.0,
      static_cast<double>(particle.velocity_z),
  };
  const auto orientation = orientedVolumetricBasis(
      particle.retail_expl_frame_volume ||
              particle.retail_fire_frame_volume
          ? Vector3{0.0, -1.0, 0.0}
          : rising_direction);
  const auto *oriented =
      kind == GR_VOLUME_FIRE || kind == GR_VOLUME_SMOKE ? &orientation
                                                        : nullptr;
  return enqueueVolumetricEffect(
      Vector3{static_cast<double>(particle.x),
              static_cast<double>(particle.y),
              static_cast<double>(particle.z)},
      Vector3{half_width * particle.volume_tuning.radius_scale,
              half_height * particle.volume_tuning.radius_scale,
              half_width * depth_scale *
                  particle.volume_tuning.radius_scale}, kind,
      static_cast<double>(particle.red) / 128.0,
      static_cast<double>(particle.green) / 128.0,
      static_cast<double>(particle.blue) / 128.0,
      density * particle.volume_tuning.density_scale,
      emission * particle.volume_tuning.emission_scale,
      static_cast<double>(particle.frame) / static_cast<double>(frame_count),
      packed_seed, camera, effects, presentation_amount, oriented,
      particle.retail_expl_frame_volume
          ? GR_VOLUME_FLAG_RETAIL_EXPL_FRAME
          : particle.retail_fire_frame_volume
                ? GR_VOLUME_FLAG_RETAIL_FIRE_FRAME
                : GR_VOLUME_FLAG_NONE);
}

bool enqueueGuestVolumetricEffect(
    const game::LegacyGuestSpriteBridgeState &sprite,
    const Vector3 &world_center, const Vector3 &world_radius,
    const game::CameraState &camera,
    std::vector<GrVolumetricEffect> &effects, int kind_override = -1,
    game::LegacyHaloPresentationDescriptor halo_presentation = {},
    game::LegacyCfireVolumeTuning fire_tuning = {},
    const PersistentFireEmitterSource *persistent_fire = nullptr,
    unsigned int retail_frame_volume_flags = GR_VOLUME_FLAG_NONE) {
  const auto vertical_orientation =
      orientedVolumetricBasis(Vector3{0.0, -1.0, 0.0});
  const auto seed = sprite.source_address != 0U
                        ? sprite.source_address
                        : static_cast<std::uint32_t>(std::max<std::int32_t>(
                              sprite.effect_particle, 0));
  if (sprite.effect_family == 0U) {
    if (kind_override == GR_VOLUME_SMOKE) {
      return enqueueVolumetricEffect(
          world_center, world_radius, GR_VOLUME_SMOKE,
          static_cast<double>(sprite.color.red) / 128.0,
          static_cast<double>(sprite.color.green) / 128.0,
          static_cast<double>(sprite.color.blue) / 128.0, 0.68, 0.0, 0.0, seed,
          camera, effects, 1.0, &vertical_orientation);
    }
    if (!sprite.force_fullbright) {
      return false;
    }
    const auto halo_extent = std::max(world_radius.x, world_radius.y) *
                             halo_presentation.radius_scale;
    const auto halo_radius =
        Vector3{world_radius.x * halo_presentation.radius_scale,
                world_radius.y * halo_presentation.radius_scale,
                std::max(halo_extent * volumetric_halo_depth_scale,
                         volumetric_halo_minimum_depth)};
    return enqueueVolumetricEffect(
        world_center, halo_radius, GR_VOLUME_LIGHT_HALO,
        halo_presentation.red, halo_presentation.green,
        halo_presentation.blue, halo_presentation.density,
        halo_presentation.emission, 0.0, seed, camera, effects, 1.0,
        nullptr, GR_VOLUME_FLAG_AUTHORED_HALO_COLOR);
  }
  const auto family =
      static_cast<game::LegacyEffectSpriteFamily>(sprite.effect_family);
  const auto family_kind = volumetricKind(family);
  const auto layout = game::legacyEffectSpriteLayout(family);
  if ((kind_override < 0 && !family_kind) || layout.frame_count == 0U) {
    return false;
  }
  const auto kind = kind_override >= 0 ? kind_override : *family_kind;
  auto density = 0.9;
  auto emission = 0.0;
  if (kind == GR_VOLUME_FIRE) {
    density = 1.25;
    emission = 1.20;
  } else if (kind == GR_VOLUME_EXPLOSION) {
    density = 1.65;
    emission = 1.55;
  } else if (kind == GR_VOLUME_FOG) {
    density = 0.45;
  } else if (kind == GR_VOLUME_SMOKE) {
    density = 0.72;
  }
  const auto *orientation =
      kind == GR_VOLUME_FIRE || kind == GR_VOLUME_SMOKE
          ? &vertical_orientation
          : nullptr;
  const auto radius_scale =
      kind == GR_VOLUME_FIRE ? fire_tuning.radius_scale : 1.0;
  if (kind == GR_VOLUME_FIRE && persistent_fire != nullptr) {
    const auto persistent_layout = sf::platform::persistentFireVolumeLayout(
        persistent_fire->anchor.x, persistent_fire->anchor.y,
        persistent_fire->anchor.z, world_radius.x * radius_scale,
        world_radius.y * radius_scale, world_radius.z * radius_scale);
    return enqueueVolumetricEffect(
        Vector3{persistent_layout.center_x, persistent_layout.center_y,
                persistent_layout.center_z},
        Vector3{persistent_layout.radius_x, persistent_layout.radius_y,
                persistent_layout.radius_z}, kind,
        static_cast<double>(sprite.color.red) / 128.0,
        static_cast<double>(sprite.color.green) / 128.0,
        static_cast<double>(sprite.color.blue) / 128.0,
        density * fire_tuning.density_scale,
        emission * fire_tuning.emission_scale, 0.0, persistent_fire->seed,
        camera, effects, 1.0, orientation,
        GR_VOLUME_FLAG_PERSISTENT_FIRE);
  }
  return enqueueVolumetricEffect(
      world_center,
      Vector3{world_radius.x * radius_scale, world_radius.y * radius_scale,
              world_radius.z * radius_scale}, kind,
      static_cast<double>(sprite.color.red) / 128.0,
      static_cast<double>(sprite.color.green) / 128.0,
      static_cast<double>(sprite.color.blue) / 128.0,
      density * (kind == GR_VOLUME_FIRE ? fire_tuning.density_scale : 1.0),
      emission *
          (kind == GR_VOLUME_FIRE ? fire_tuning.emission_scale : 1.0),
      static_cast<double>(sprite.effect_frame) /
          static_cast<double>(layout.frame_count),
      seed, camera, effects, 1.0, orientation,
      retail_frame_volume_flags);
}

std::optional<sf::platform::Park2FlameWorldGeometry>
park2FlameWorldGeometry(const game::LegacyPark2FlamethrowerRibbon &ribbon,
                        const game::CameraState &authored_camera) {
  // PARK2 FT4 coordinates were authored for the 320-pixel retail viewport.
  // The packet camera is otherwise scaled to the native 384-pixel canvas.
  constexpr auto retail_width = 320.0;
  const auto retail_projection =
      std::max(static_cast<double>(authored_camera.projection) * retail_width /
                   static_cast<double>(screen_width),
               1.0);
  const auto basis = viewBasis(authored_camera);
  std::array<sf::platform::Park2FlameScreenPoint, 4U> screen_corners{};
  for (std::size_t corner = 0U; corner < screen_corners.size(); ++corner) {
    screen_corners[corner] = sf::platform::Park2FlameScreenPoint{
        static_cast<double>(ribbon.corners[corner].x),
        static_cast<double>(ribbon.corners[corner].y),
    };
  }
  // FUN_80147a8c emits the next/fallback centre into corners 0/1 and the
  // current chain centre into corners 2/3. Packet XY is centre-origin because
  // retail applies the 160,120 DR_ENV offset after GTE projection.
  return sf::platform::recoverPark2FlameWorldGeometry(
      screen_corners,
      std::array{
          game::DynamicLightPoint{static_cast<double>(ribbon.world_first.x),
                                  static_cast<double>(ribbon.world_first.y),
                                  static_cast<double>(ribbon.world_first.z)},
          game::DynamicLightPoint{static_cast<double>(ribbon.world_second.x),
                                  static_cast<double>(ribbon.world_second.y),
                                  static_cast<double>(ribbon.world_second.z)},
      },
      sf::platform::Park2FlameProjectionBasis{
          {authored_camera.x, authored_camera.y, authored_camera.z},
          {basis[0].x, basis[0].y, basis[0].z},
          {basis[1].x, basis[1].y, basis[1].z},
          {basis[2].x, basis[2].y, basis[2].z},
          retail_projection,
          active_near_clip_depth,
      });
}

std::optional<game::DynamicLightPoint>
park2GirdeuxMuzzleAnchor(const game::GameplaySession &gameplay,
                         const game::ActorAnimationBank &actor_animations,
                         const RenderPresentationSnapshot &presentation) {
  const auto count =
      std::min(presentation.objects.size(), gameplay.objects().size());
  for (std::size_t object = 0U; object < count; ++object) {
    const auto object_index = static_cast<std::uint16_t>(object);
    if (gameplay.legacyDedicatedActorWeapon(object_index) !=
        game::WeaponId::flamethrower) {
      continue;
    }
    const auto &presented_object = presentation.objects[object];
    if (presented_object.model >= gameplay.objectModels().size()) {
      continue;
    }
    const auto *model = std::get_if<assets::HmdModel>(
        &gameplay.objectModels()[presented_object.model].geometry);
    if (model == nullptr) {
      continue;
    }
    const auto *state = gameplay.npcState(object_index);
    const auto pose =
        state == nullptr
            ? actor_animations.enemyPose(presentation.guest_frame, object * 7U)
            : actor_animations.npcPose(
                  gameplay.npcAnimation(object_index),
                  state->locomotion_animation_tick, state->animation_tick,
                  npcAnimationPhase(presented_object, object_index));
    const auto muzzle = weaponMuzzleAnchor(gameplay, *model, presented_object,
                                           pose, game::WeaponId::flamethrower);
    if (muzzle) {
      return game::DynamicLightPoint{muzzle->x, muzzle->y, muzzle->z};
    }
  }
  return std::nullopt;
}

std::optional<game::DynamicLightPoint>
park2GirdeuxFlameTranslation(const game::GameplaySession &gameplay,
                             const game::ActorAnimationBank &actor_animations,
                             const RenderPresentationSnapshot &presentation) {
  const auto muzzle =
      park2GirdeuxMuzzleAnchor(gameplay, actor_animations, presentation);
  if (!muzzle) {
    return std::nullopt;
  }
  std::vector<game::DynamicLightPoint> centreline;
  centreline.reserve(gameplay.legacyPark2FlamethrowerRibbons().size() * 2U);
  for (const auto &ribbon : gameplay.legacyPark2FlamethrowerRibbons()) {
    const auto geometry =
        park2FlameWorldGeometry(ribbon, presentation.guest_packet_camera);
    if (!geometry) {
      continue;
    }
    centreline.insert(centreline.end(), geometry->centres.begin(),
                      geometry->centres.end());
  }
  return sf::platform::park2FlameAnchorTranslation(centreline, *muzzle);
}

void renderPark2FlamethrowerRibbons(
    std::span<const game::LegacyPark2FlamethrowerRibbon> ribbons,
    const game::ObjectFireEmitter &fire,
    const FireTexturePlacement &texture_placement,
    const game::CameraState &authored_camera, const MATRIX &view,
    const game::CameraState &presentation_camera,
    std::vector<OT_TAG> &ordering_table, PrimitiveBuffer &primitives,
    game::DynamicLightPoint anchor_translation, RenderStats &stats) {
  for (const auto &ribbon : ribbons) {
    if (ribbon.frame >= fire.frames.size()) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "PARK2 flame ribbon has no matching EXPL frame"};
    }
    auto world = park2FlameWorldGeometry(ribbon, authored_camera);
    if (world) {
      sf::platform::translatePark2FlameWorldGeometry(*world,
                                                     anchor_translation);
    }
    if (!world || std::ranges::any_of(world->corners, [&](const auto &point) {
          return preciseCameraDepth(view, point.x, point.y, point.z) <=
                 active_near_clip_depth;
        })) {
      ++stats.rejected;
      continue;
    }
    auto center = Vector3{};
    for (const auto &point : world->corners) {
      center.x += point.x * 0.25;
      center.y += point.y * 0.25;
      center.z += point.z * 0.25;
    }
    // A retail ribbon is a world-space segment. Keep its analytic volume on
    // that segment instead of rebuilding an ellipsoid on the current camera.
    const auto outward = Vector3{
        world->centres[0].x - world->centres[1].x,
        world->centres[0].y - world->centres[1].y,
        world->centres[0].z - world->centres[1].z,
    };
    const auto segment_length = std::sqrt(
        outward.x * outward.x + outward.y * outward.y +
        outward.z * outward.z);
    const auto orientation = orientedVolumetricBasis(outward);
    auto ribbon_radius = 0.0;
    for (std::size_t pair = 0U; pair < world->centres.size(); ++pair) {
      for (std::size_t corner = pair * 2U; corner < pair * 2U + 2U;
           ++corner) {
        const auto x = world->corners[corner].x - world->centres[pair].x;
        const auto y = world->corners[corner].y - world->centres[pair].y;
        const auto z = world->corners[corner].z - world->centres[pair].z;
        ribbon_radius =
            std::max(ribbon_radius, std::sqrt(x * x + y * y + z * z));
      }
    }
    ribbon_radius = std::max(ribbon_radius, 24.0);
    const auto radius = Vector3{
        ribbon_radius,
        std::max(segment_length * 0.5 + ribbon_radius * 0.15, 24.0),
        std::max(ribbon_radius * 0.70, 24.0),
    };
    const auto seed =
        static_cast<std::uint32_t>(ribbon.world_first.x) * 73856093U ^
        static_cast<std::uint32_t>(ribbon.world_first.y) * 19349663U ^
        static_cast<std::uint32_t>(ribbon.world_second.z) * 83492791U ^
        static_cast<std::uint32_t>(ribbon.frame) * 2654435761U;
    if (enqueueVolumetricEffect(
            center, radius, GR_VOLUME_FIRE,
            static_cast<double>(ribbon.red) / 128.0,
            static_cast<double>(ribbon.green) / 128.0,
            static_cast<double>(ribbon.blue) / 128.0, 1.15, 1.05,
            static_cast<double>(ribbon.frame) /
                static_cast<double>(fire.frames.size()),
            seed, presentation_camera, primitives.volumetric_effects, 1.0,
            &orientation, GR_VOLUME_FLAG_FLAME_JET)) {
      continue;
    }
    const auto frame = fireFrameMaterial(
        fire.frames[ribbon.frame], texture_placement,
        game::LegacyEffectSpriteFamily::explosion, ribbon.frame);
    auto v0 = makeVertex(world->corners[0].x, world->corners[0].y,
                         world->corners[0].z);
    auto v1 = makeVertex(world->corners[1].x, world->corners[1].y,
                         world->corners[1].z);
    auto v2 = makeVertex(world->corners[2].x, world->corners[2].y,
                         world->corners[2].z);
    auto v3 = makeVertex(world->corners[3].x, world->corners[3].y,
                         world->corners[3].z);
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
                          double u, double v,
                          double presentation_amount = 1.0) {
  return TexturedVertex{
      static_cast<double>(particle.x) + right.x * horizontal + up.x * vertical,
      static_cast<double>(particle.y) + right.y * horizontal + up.y * vertical,
      static_cast<double>(particle.z) + right.z * horizontal + up.z * vertical,
      u,
      v,
      static_cast<double>(particle.red) * presentation_amount,
      static_cast<double>(particle.green) * presentation_amount,
      static_cast<double>(particle.blue) * presentation_amount,
  };
}

void renderFireObject(const game::ObjectFireEmitter &fire,
                      std::span<const FireParticle> particles,
                      const FireTexturePlacement &texture_placement,
                      const game::CameraState &camera,
                      const MATRIX &view, std::vector<OT_TAG> &ordering_table,
                      PrimitiveBuffer &primitives, RenderStats &stats,
                      double presentation_amount = 1.0,
                      bool allow_volumetric_effects = true,
                      std::vector<GrVolumetricEffect> *volume_target = nullptr) {
  presentation_amount = std::clamp(presentation_amount, 0.0, 1.0);
  auto &volumes = volume_target != nullptr ? *volume_target : primitives.volumetric_effects;
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
  for (std::size_t particle_index = 0U; particle_index < particles.size();
       ++particle_index) {
    const auto &particle = particles[particle_index];
    const auto *images = legacyEffectFrames(fire, particle.family);
    if (images == nullptr || particle.frame >= images->size()) {
      continue;
    }
    const auto &image = (*images)[particle.frame];
    const auto half_width = particle.size * 0.5;
    const auto half_height = half_width *
                             static_cast<double>(image.displayHeight()) /
                             static_cast<double>(image.displayWidth());
    const auto volume_requested =
        particle.volume_may_replace_retail_sprite ||
        particle.retail_expl_frame_volume ||
        particle.retail_fire_frame_volume;
    const auto volume_presented =
        allow_volumetric_effects && volume_requested &&
        enqueueFireVolumetricEffect(
            particle, half_width, half_height, particle_index, images->size(),
            camera, volumes, presentation_amount);
    const auto retail_frame_volume = particle.retail_expl_frame_volume ||
                                     particle.retail_fire_frame_volume;
    if (game::legacyEffectVolumeConsumesRetailSprite(
            particle.volume_may_replace_retail_sprite, volume_presented,
            retail_frame_volume)) {
      continue;
    }
    const auto frame = fireFrameMaterial(image, texture_placement,
                                         particle.family, particle.frame);
    const auto top_left =
        fireVertex(particle, right, up, -half_width, half_height,
                   frame.minimum_u, frame.minimum_v, presentation_amount);
    const auto top_right =
        fireVertex(particle, right, up, half_width, half_height,
                   frame.maximum_u, frame.minimum_v, presentation_amount);
    const auto bottom_left =
        fireVertex(particle, right, up, -half_width, -half_height,
                   frame.minimum_u, frame.maximum_v, presentation_amount);
    const auto bottom_right =
        fireVertex(particle, right, up, half_width, -half_height,
                   frame.maximum_u, frame.maximum_v, presentation_amount);
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
      if (particle.attached_slot >= 0) {
        // Actor mode9 supplies the exact polygon-selection point above; it is
        // not an additional camera-facing blood triangle.
        continue;
      }
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

void renderRetailWorldDecals(
    std::span<const game::LegacyWorldDecalBridgeState> observed_decals,
    std::uint64_t tick, WeaponEffectsScratch &scratch,
    const CombatEffectTextureAtlas &effect_textures, bool nightvision,
    const MATRIX &view, std::vector<OT_TAG> &ordering_table, PrimitiveBuffer &primitives,
    RenderStats &stats) {
  constexpr std::array<std::size_t, 4U> retail_vertex_order{0U, 1U, 3U, 2U};
  if (scratch.world_decal_tick_initialized &&
      tick < scratch.world_decal_last_tick) {
    scratch.world_decals = {};
  }
  scratch.world_decal_tick_initialized = true;
  scratch.world_decal_last_tick = tick;

  std::array<bool, game::legacy_world_decal_capacity> observed_slots{};
  for (const auto &decal : observed_decals) {
    if (decal.slot >= scratch.world_decals.size()) {
      continue;
    }
    observed_slots[decal.slot] = true;
    auto &entry = scratch.world_decals[decal.slot];
    if (!entry.source || *entry.source != decal) {
      entry.source = decal;
      entry.born_tick = tick;
      entry.expired = false;
    } else if (tick >= entry.born_tick &&
               tick - entry.born_tick >= world_decal_lifetime_ticks) {
      entry.expired = true;
    }
  }
  for (std::size_t slot = 0U; slot < scratch.world_decals.size(); ++slot) {
    if (!observed_slots[slot]) {
      scratch.world_decals[slot] = {};
    }
  }

  for (const auto &entry : scratch.world_decals) {
    if (!entry.source || entry.expired) {
      continue;
    }
    const auto &decal = *entry.source;
    const auto sprite = effect_textures.bulletMark(decal.material_words);
    const std::array<std::pair<double, double>, 4U> uv{{
        {sprite.minimum_u, sprite.minimum_v},
        {sprite.maximum_u, sprite.minimum_v},
        {sprite.minimum_u, sprite.maximum_v},
        {sprite.maximum_u, sprite.maximum_v},
    }};
    const auto &normal_origin = decal.vertices[0];
    const auto &normal_first = decal.vertices[1];
    const auto &normal_second = decal.vertices[3];
    const auto first_edge = Vector3{
        static_cast<double>(normal_first.x - normal_origin.x),
        static_cast<double>(normal_first.y - normal_origin.y),
        static_cast<double>(normal_first.z - normal_origin.z),
    };
    const auto second_edge = Vector3{
        static_cast<double>(normal_second.x - normal_origin.x),
        static_cast<double>(normal_second.y - normal_origin.y),
        static_cast<double>(normal_second.z - normal_origin.z),
    };
    auto normal = Vector3{
        first_edge.y * second_edge.z - first_edge.z * second_edge.y,
        first_edge.z * second_edge.x - first_edge.x * second_edge.z,
        first_edge.x * second_edge.y - first_edge.y * second_edge.x,
    };
    const auto normal_length = std::sqrt(
        normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (!std::isfinite(normal_length) || normal_length <= 0.000001) {
      continue;
    }
    normal = {normal.x / normal_length, normal.y / normal_length,
              normal.z / normal_length};
    const auto centre = Vector3{
        (static_cast<double>(decal.vertices[0].x) +
         static_cast<double>(decal.vertices[1].x) +
         static_cast<double>(decal.vertices[2].x) +
         static_cast<double>(decal.vertices[3].x)) *
            0.25,
        (static_cast<double>(decal.vertices[0].y) +
         static_cast<double>(decal.vertices[1].y) +
         static_cast<double>(decal.vertices[2].y) +
         static_cast<double>(decal.vertices[3].y)) *
            0.25,
        (static_cast<double>(decal.vertices[0].z) +
         static_cast<double>(decal.vertices[1].z) +
         static_cast<double>(decal.vertices[2].z) +
         static_cast<double>(decal.vertices[3].z)) *
            0.25,
    };
    const auto positive_depth = preciseCameraDepth(
        view, centre.x + normal.x, centre.y + normal.y,
        centre.z + normal.z);
    const auto negative_depth = preciseCameraDepth(
        view, centre.x - normal.x, centre.y - normal.y,
        centre.z - normal.z);
    if (positive_depth > negative_depth) {
      normal = {-normal.x, -normal.y, -normal.z};
    }
    const auto biased_centre_depth =
        std::min(positive_depth, negative_depth);
    if (!std::isfinite(biased_centre_depth) ||
        biased_centre_depth <= active_near_clip_depth) {
      continue;
    }
    std::array<SVECTOR, 4U> vertices{};
    for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
      const auto &source = decal.vertices[retail_vertex_order[corner]];
      vertices[corner] = makeVertex(
          static_cast<double>(source.x) + normal.x * world_decal_surface_bias,
          static_cast<double>(source.y) + normal.y * world_decal_surface_bias,
          static_cast<double>(source.z) + normal.z * world_decal_surface_bias);
    }
    long xy0{};
    long xy1{};
    long xy2{};
    long xy3{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&vertices[0], &vertices[1], &vertices[2], &xy0, &xy1,
                      &xy2, &depth_cue, &flags);
    if (depth <= 0) {
      continue;
    }
    const auto ordering_depth = depth - 0x32L;
    if (ordering_depth >= 0x1001L) {
      continue;
    }
    int packed_xy3{};
    static_cast<void>(RotTransPers(&vertices[3], &packed_xy3, &depth_cue, &flags));
    xy3 = static_cast<long>(packed_xy3);
    auto &primitive = primitives.world_decals.emplace_back();
    setPolyFT4(&primitive);
    setSemiTrans(&primitive, 1);
    setRGB0(&primitive, nightvision ? 0U : 40U, 40U,
            nightvision ? 0U : 40U);
    setUV4(&primitive, static_cast<std::uint8_t>(uv[0].first),
           static_cast<std::uint8_t>(uv[0].second),
           static_cast<std::uint8_t>(uv[1].first),
           static_cast<std::uint8_t>(uv[1].second),
           static_cast<std::uint8_t>(uv[2].first),
           static_cast<std::uint8_t>(uv[2].second),
           static_cast<std::uint8_t>(uv[3].first),
           static_cast<std::uint8_t>(uv[3].second));
    primitive.clut = sprite.clut;
    primitive.tpage = sprite.texture_page;
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    setProjected(&primitive.x3, xy3);
    const auto ordering_index = std::clamp(
        ordering_depth, 1L, static_cast<long>(ordering_table_size - 1));
    addPrim(&ordering_table[static_cast<std::size_t>(ordering_index)], &primitive);
    const std::array projected{xy0, xy1, xy2, xy3};
    submitStats(stats, depth, projected);
  }
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
    std::span<const game::LegacyWorldDecalBridgeState> world_decals,
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

  renderRetailWorldDecals(world_decals, tick, scratch, effect_textures,
                          presentation.environment.nightvision_enabled,
                          view, ordering_table, primitives, stats);
  if (!presentation.guest_camera_lists_captured) {
    // Render modes 0/2 are already present as exact raw camera-list packets.
    // This reconstruction remains only for non-retail/fallback presentation.
    renderRetailCombatParticles(gameplay, combat_particles, presentation,
                                ordering_table, primitives, stats);
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
      particles[index].volume_kind_override = GR_VOLUME_FIRE;
    }
    renderFireObject(fire, particles, fire_texture_placement,
                     presentation_camera, view, fire_ordering_table, primitives,
                     stats);
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
  legacy_particles.reserve(presentation.expl_particles.size());
  for (const auto &source : presentation.expl_particles) {
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
    const auto persistent_fire =
        game::legacyExplParticlePresentsAuthoredFire(source.family)
            ? persistentFireEmitterSource(gameplay,
                                          presentation.expl_particles, source)
            : std::optional<PersistentFireEmitterSource>{};
    const auto cfire_owned = persistent_fire.has_value();
    if (persistent_fire) {
      particle.volume_kind_override = GR_VOLUME_FIRE;
      // The camera-distance native fallback is still the same authored
      // CFIRE emitter. Marking it as a transient particle made its volume
      // jump at 20 Hz and exposed the analytic cone whenever the exact guest
      // sprite was not linked into the camera list.
      particle.persistent_volume = true;
      particle.volume_anchor_x = static_cast<std::int32_t>(
          std::lround(persistent_fire->anchor.x));
      particle.volume_anchor_y = static_cast<std::int32_t>(
          std::lround(persistent_fire->anchor.y));
      particle.volume_anchor_z = static_cast<std::int32_t>(
          std::lround(persistent_fire->anchor.z));
      particle.volume_seed = persistent_fire->seed;
    }
    particle.volume_may_replace_retail_sprite =
        game::legacyCfireVolumeMayReplaceRetailSprite(
            gameplay.missionIndex(), cfire_owned);
    particle.retail_expl_frame_volume =
        game::legacyCfireUsesRetailExplFrameVolume(
            gameplay.missionIndex(), cfire_owned, particle.family);
    particle.retail_fire_frame_volume =
        game::legacyCfireUsesRetailFireFrameVolume(
            gameplay.missionIndex(), cfire_owned, particle.family);
    particle.volume_tuning =
        game::legacyCfireVolumeTuning(gameplay.missionIndex(), cfire_owned);
    particle.frame = source.frame;
    particle.size =
        static_cast<double>(source.scale_byte) * legacy_effect_scale_unit;
    particle.red = source.red;
    particle.green = source.green;
    particle.blue = source.blue;
    legacy_particles.push_back(particle);
  }
  if (!legacy_particles.empty()) {
    renderFireObject(fire, legacy_particles, fire_texture_placement,
                     presentation_camera, view, fire_ordering_table, primitives,
                     stats);
  }
  // PARK2's FT4 ribbon pool is not part of the +0x90 sprite list and its
  // textured opcode is deliberately absent from the +0x98 whitelist.
  const auto park2_flame_translation =
      park2GirdeuxFlameTranslation(gameplay, actor_animations, presentation)
          .value_or(game::DynamicLightPoint{});
  renderPark2FlamethrowerRibbons(
      gameplay.legacyPark2FlamethrowerRibbons(), fire, fire_texture_placement,
      presentation.guest_packet_camera, view, presentation_camera,
      fire_ordering_table, primitives, park2_flame_translation, stats);
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
      renderFireObject(fire, particles, fire_texture_placement, presentation_camera, view,
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
          particles[index].volume_kind_override = GR_VOLUME_FIRE;
        }
        renderFireObject(fire, particles, fire_texture_placement, presentation_camera, view,
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
      renderFireObject(fire, particles, fire_texture_placement, presentation_camera, view,
                       fire_ordering_table, primitives, stats);
    }
  }
}

void renderEmdObject(const assets::EmdScene &scene,
                     const game::SceneObject &object, unsigned int texture_bank,
                     const MATRIX &view, std::vector<OT_TAG> &ordering_table,
                     PrimitiveBuffer &primitives, RenderStats &stats,
                     bool retail_scrim = false,
                     bool force_semi_transparent = false,
                     long depth_cue_floor = 0L) {
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
                                false,
                                !retail_scrim && !force_semi_transparent,
                                depth_cue_floor);
          submitClippedTriangle(second_triangle, material, false,
                                reverse_winding, view, ordering_table,
                                primitives.objects, stats, worldOrderingDepth,
                                false,
                                !retail_scrim && !force_semi_transparent,
                                depth_cue_floor);
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
        if (!retail_scrim && !force_semi_transparent) {
          depthCuePrimitive(primitive,
                            {std::max(depth_cue_floor,
                                      retailObjectDepthCue(cameraDepth(view, v0))),
                             std::max(depth_cue_floor,
                                      retailObjectDepthCue(cameraDepth(view, v1))),
                             std::max(depth_cue_floor,
                                      retailObjectDepthCue(cameraDepth(view, v2))),
                             std::max(depth_cue_floor,
                                      retailObjectDepthCue(cameraDepth(view, v3)))});
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
              stats, worldOrderingDepth, false,
              !retail_scrim && !force_semi_transparent, depth_cue_floor);
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
        if (!retail_scrim && !force_semi_transparent) {
          depthCuePrimitive(primitive,
                            {std::max(depth_cue_floor,
                                      retailObjectDepthCue(cameraDepth(view, v0))),
                             std::max(depth_cue_floor,
                                      retailObjectDepthCue(cameraDepth(view, v1))),
                             std::max(depth_cue_floor,
                                      retailObjectDepthCue(cameraDepth(view, v2)))});
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
