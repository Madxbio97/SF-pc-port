struct WorldProjectedVertex {
  long packed_xy{};
  int ordering_depth{};
  PGXPVData precise{};
  bool precise_valid{};
};

struct OpticProjectedTriangle {
  std::array<long, 3U> packed_xy{};
  std::array<PGXPVData, 3U> precise{};
  std::array<bool, 3U> precise_valid{};
  int ordering_depth{};
};

using OpticProjectedFrame = std::vector<OpticProjectedTriangle>;
using OpticHistory = sf::platform::RetailOpticHistory<OpticProjectedFrame>;

struct ActorFrameScratch {
  std::vector<Vector3> hmd_world_vertices;
  std::vector<Vector3> hmd_world_normals;
  std::vector<Vector3> shadow_world_vertices;
  std::vector<ActorShadowTriangle> shadow_triangles;
  std::vector<std::optional<ActorShadowReceiverVertex>> shadow_receivers;

  void reset() noexcept {
    hmd_world_vertices.clear();
    hmd_world_normals.clear();
    shadow_world_vertices.clear();
    shadow_triangles.clear();
    shadow_receivers.clear();
  }
};

struct WorldVertexFrameScratch {
  std::vector<double> precise_depths;
  std::vector<int> chunk_depth_cues;
  std::vector<int> depth_cues;
  std::vector<WorldProjectedVertex> projected_vertices;

  void reset() noexcept {
    precise_depths.clear();
    chunk_depth_cues.clear();
    depth_cues.clear();
    projected_vertices.clear();
  }
};

struct PrimitiveBuffer {
  StableFrameVector<POLY_GT3> triangles{"Terrain triangles"};
  StableFrameVector<POLY_GT4> quads{"Terrain quads"};
  StableFrameVector<POLY_GT3> objects{"Object triangles"};
  StableFrameVector<POLY_GT4> object_quads{"Object quads"};
  StableFrameVector<POLY_F3> actor_shadows{"Dynamic actor shadows"};
  StableFrameVector<POLY_F3> player{"Fallback player triangles"};
  StableFrameVector<POLY_F3> optic_subjects{"Optic subject silhouettes"};
  StableFrameVector<POLY_F3> optic_trails{"Optic motion trails"};
  StableFrameVector<LINE_G2> effects{"Effect lines"};
  StableFrameVector<LINE_F2> combat_effect_lines{"Combat effect lines"};
  StableFrameVector<POLY_F3> combat_effect_triangles{"Combat effect triangles"};
  StableFrameVector<POLY_GT4> effect_sprite_quads{"Effect sprite quads"};
  StableFrameVector<POLY_FT4> world_decals{"Retail world decals"};
  StableFrameVector<POLY_FT4> park2_flamethrower_ribbons{
      "PARK2 flamethrower ribbons"};
  StableFrameVector<POLY_FT4> pickup_sprites{"Retail pickup sprites"};
  StableFrameVector<POLY_FT4> projectile_sprites{"Projectile sprites"};
  StableFrameVector<POLY_FT4> guest_sprites{"Retail guest sprites"};
  StableFrameVector<LINE_G2> guest_lines{"Retail guest lines"};
  StableFrameVector<DR_TPAGE> guest_line_modes{"Retail guest line modes"};
  StableFrameVector<TILE_1> guest_raw_tiles{"Retail raw tiles"};
  StableFrameVector<LINE_F2> guest_raw_flat_lines{"Retail raw flat lines"};
  StableFrameVector<POLY_F3> guest_raw_flat_triangles{
      "Retail raw flat triangles"};
  StableFrameVector<POLY_F4> guest_raw_flat_quads{"Retail raw flat quads"};
  StableFrameVector<LINE_G2> guest_raw_gouraud_lines{
      "Retail raw Gouraud lines"};
  StableFrameVector<POLY_G3> guest_raw_gouraud_triangles{
      "Retail raw Gouraud triangles"};
  StableFrameVector<DR_TPAGE> guest_raw_modes{"Retail raw draw modes"};
  ActorFrameScratch actor_scratch;
  WorldVertexFrameScratch world_vertex_scratch;

  [[nodiscard]] bool needsCapacityPlan(std::uint64_t guest_frame,
                                       std::uint16_t room,
                                       bool first_person_aim,
                                       std::uint8_t optic_mode,
                                       bool scanner_active) noexcept {
    if (capacity_plan_valid_ && capacity_guest_frame_ == guest_frame &&
        capacity_room_ == room &&
        capacity_first_person_aim_ == first_person_aim &&
        capacity_optic_mode_ == optic_mode &&
        capacity_scanner_active_ == scanner_active) {
      return false;
    }
    capacity_plan_valid_ = true;
    capacity_guest_frame_ = guest_frame;
    capacity_room_ = room;
    capacity_first_person_aim_ = first_person_aim;
    capacity_optic_mode_ = optic_mode;
    capacity_scanner_active_ = scanner_active;
    return true;
  }

  void invalidateCapacityPlan() noexcept { capacity_plan_valid_ = false; }

  void reset() noexcept {
    triangles.reset();
    quads.reset();
    objects.reset();
    object_quads.reset();
    actor_shadows.reset();
    player.reset();
    optic_subjects.reset();
    optic_trails.reset();
    effects.reset();
    combat_effect_lines.reset();
    combat_effect_triangles.reset();
    effect_sprite_quads.reset();
    world_decals.reset();
    park2_flamethrower_ribbons.reset();
    pickup_sprites.reset();
    projectile_sprites.reset();
    guest_sprites.reset();
    guest_lines.reset();
    guest_line_modes.reset();
    guest_raw_tiles.reset();
    guest_raw_flat_lines.reset();
    guest_raw_flat_triangles.reset();
    guest_raw_flat_quads.reset();
    guest_raw_gouraud_lines.reset();
    guest_raw_gouraud_triangles.reset();
    guest_raw_modes.reset();
    actor_scratch.reset();
    world_vertex_scratch.reset();
  }

  void lockStorage() noexcept {
    triangles.lockStorage();
    quads.lockStorage();
    objects.lockStorage();
    object_quads.lockStorage();
    actor_shadows.lockStorage();
    player.lockStorage();
    optic_subjects.lockStorage();
    optic_trails.lockStorage();
    effects.lockStorage();
    combat_effect_lines.lockStorage();
    combat_effect_triangles.lockStorage();
    effect_sprite_quads.lockStorage();
    world_decals.lockStorage();
    park2_flamethrower_ribbons.lockStorage();
    pickup_sprites.lockStorage();
    projectile_sprites.lockStorage();
    guest_sprites.lockStorage();
    guest_lines.lockStorage();
    guest_line_modes.lockStorage();
    guest_raw_tiles.lockStorage();
    guest_raw_flat_lines.lockStorage();
    guest_raw_flat_triangles.lockStorage();
    guest_raw_flat_quads.lockStorage();
    guest_raw_gouraud_lines.lockStorage();
    guest_raw_gouraud_triangles.lockStorage();
    guest_raw_modes.lockStorage();
  }

  [[nodiscard]] bool storageStable() const noexcept {
    return triangles.storageStable() && quads.storageStable() &&
           objects.storageStable() && object_quads.storageStable() &&
           actor_shadows.storageStable() && player.storageStable() &&
           optic_subjects.storageStable() && optic_trails.storageStable() &&
           effects.storageStable() && combat_effect_lines.storageStable() &&
           combat_effect_triangles.storageStable() &&
           effect_sprite_quads.storageStable() &&
           world_decals.storageStable() &&
           park2_flamethrower_ribbons.storageStable() &&
           pickup_sprites.storageStable() &&
           projectile_sprites.storageStable() &&
           guest_sprites.storageStable() && guest_lines.storageStable() &&
           guest_line_modes.storageStable() &&
           guest_raw_tiles.storageStable() &&
           guest_raw_flat_lines.storageStable() &&
           guest_raw_flat_triangles.storageStable() &&
           guest_raw_flat_quads.storageStable() &&
           guest_raw_gouraud_lines.storageStable() &&
           guest_raw_gouraud_triangles.storageStable() &&
           guest_raw_modes.storageStable();
  }

private:
  std::uint64_t capacity_guest_frame_{};
  std::uint16_t capacity_room_{};
  std::uint8_t capacity_optic_mode_{};
  bool capacity_first_person_aim_{};
  bool capacity_scanner_active_{};
  bool capacity_plan_valid_{};
};

struct RenderStats {
  bool collect_details{true};
  std::size_t submitted{};
  std::size_t rejected{};
  int minimum_depth{std::numeric_limits<int>::max()};
  int maximum_depth{};
  int minimum_x{std::numeric_limits<int>::max()};
  int maximum_x{std::numeric_limits<int>::min()};
  int minimum_y{std::numeric_limits<int>::max()};
  int maximum_y{std::numeric_limits<int>::min()};
};

void includeProjected(RenderStats &stats, long packed) {
  const auto x = static_cast<int>(screenX(packed));
  const auto y = static_cast<int>(screenY(packed));
  stats.minimum_x = std::min(stats.minimum_x, x);
  stats.maximum_x = std::max(stats.maximum_x, x);
  stats.minimum_y = std::min(stats.minimum_y, y);
  stats.maximum_y = std::max(stats.maximum_y, y);
}

SVECTOR makeVertex(const assets::EmdVertex &source) {
  return SVECTOR{source.x, source.y, source.z, 0};
}

SVECTOR makeVertex(double x, double y, double z) {
  auto result = SVECTOR{
      static_cast<short>(std::lround(x)),
      static_cast<short>(std::lround(y)),
      static_cast<short>(std::lround(z)),
      0,
  };
  const std::array exact{x, y, z};
  PGXP_VectorRegister(&result, exact.data());
  return result;
}

int cameraDepth(const MATRIX &view, const SVECTOR &vertex) {
  return static_cast<int>(
             (static_cast<std::int64_t>(view.m[2][0]) * vertex.vx +
              static_cast<std::int64_t>(view.m[2][1]) * vertex.vy +
              static_cast<std::int64_t>(view.m[2][2]) * vertex.vz) >>
             12U) +
         view.t[2];
}

double preciseCameraDepth(const MATRIX &view, double x, double y, double z) {
  std::array<double, 9> exact_rotation{};
  std::array<double, 3> exact_translation{};
  if (PGXP_MatrixLookup(&view, exact_rotation.data()) &&
      PGXP_MatrixLookupTranslation(&view, exact_translation.data())) {
    return exact_rotation[6] * x + exact_rotation[7] * y +
           exact_rotation[8] * z + exact_translation[2];
  }
  return (static_cast<double>(view.m[2][0]) * x +
          static_cast<double>(view.m[2][1]) * y +
          static_cast<double>(view.m[2][2]) * z) /
             4096.0 +
         static_cast<double>(view.t[2]);
}

struct VertexColor {
  std::uint8_t red;
  std::uint8_t green;
  std::uint8_t blue;
};

struct WorldPolygonLightingCacheEntry {
  std::array<VertexColor, 4U> colors{};
  std::uint64_t generation{};
};

class WorldPolygonLightingCache final {
public:
  void beginFrame(std::uint64_t guest_frame, std::uint16_t room,
                  std::uint64_t light_signature) {
    cursor_ = 0U;
    if (valid_ && guest_frame_ == guest_frame && room_ == room &&
        light_signature_ == light_signature) {
      return;
    }
    guest_frame_ = guest_frame;
    room_ = room;
    light_signature_ = light_signature;
    valid_ = true;
    ++generation_;
    if (generation_ == 0U) {
      for (auto &entry : entries_) {
        entry.generation = 0U;
      }
      generation_ = 1U;
    }
  }

  [[nodiscard]] WorldPolygonLightingCacheEntry &nextPolygon() {
    if (cursor_ == entries_.size()) {
      entries_.emplace_back();
    }
    return entries_[cursor_++];
  }

  void advance(std::size_t polygon_count) {
    if (polygon_count > std::numeric_limits<std::size_t>::max() - cursor_) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "World lighting cache index overflows"};
    }
    cursor_ += polygon_count;
    if (entries_.size() < cursor_) {
      entries_.resize(cursor_);
    }
  }

  [[nodiscard]] bool
  current(const WorldPolygonLightingCacheEntry &entry) const noexcept {
    return valid_ && entry.generation == generation_;
  }

  void commit(WorldPolygonLightingCacheEntry &entry) const noexcept {
    entry.generation = generation_;
  }

  void reset() noexcept {
    cursor_ = 0U;
    guest_frame_ = std::numeric_limits<std::uint64_t>::max();
    room_ = std::numeric_limits<std::uint16_t>::max();
    light_signature_ = 0U;
    valid_ = false;
  }

private:
  std::vector<WorldPolygonLightingCacheEntry> entries_;
  std::size_t cursor_{};
  std::uint64_t guest_frame_{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t generation_{1U};
  std::uint16_t room_{std::numeric_limits<std::uint16_t>::max()};
  std::uint64_t light_signature_{};
  bool valid_{};
};

struct LocalSceneLightingSample {
  std::uint16_t model{};
  std::uint16_t section{};
  std::uint32_t polygon{};
  std::array<std::size_t, 3U> corners{};
};

struct LocalSceneLightingCacheEntry {
  assets::MissionTransform transform;
  std::optional<LocalSceneLightingSample> sample;
};

class LocalSceneLightingCache final {
public:
  void beginFrame(std::uint16_t room,
                  std::span<const std::uint16_t> active_models,
                  std::size_t object_count) {
    if (room == room_ && entries_.size() == object_count &&
        std::ranges::equal(active_models_, active_models)) {
      return;
    }
    room_ = room;
    active_models_.assign(active_models.begin(), active_models.end());
    entries_.assign(object_count, std::nullopt);
  }

  [[nodiscard]] const LocalSceneLightingCacheEntry *
  find(std::size_t object_index,
       const assets::MissionTransform &transform) const noexcept {
    if (object_index >= entries_.size() || !entries_[object_index]) {
      return nullptr;
    }
    const auto &entry = *entries_[object_index];
    return sameTransform(entry.transform, transform) ? &entry : nullptr;
  }

  void store(std::size_t object_index,
             const assets::MissionTransform &transform,
             std::optional<LocalSceneLightingSample> sample) noexcept {
    if (object_index < entries_.size()) {
      entries_[object_index] = LocalSceneLightingCacheEntry{transform, sample};
    }
  }

private:
  [[nodiscard]] static bool
  sameTransform(const assets::MissionTransform &left,
                const assets::MissionTransform &right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z &&
           left.rotation == right.rotation;
  }

  std::uint16_t room_{std::numeric_limits<std::uint16_t>::max()};
  std::vector<std::uint16_t> active_models_;
  std::vector<std::optional<LocalSceneLightingCacheEntry>> entries_;
};

struct TexturedVertex {
  double x;
  double y;
  double z;
  double u;
  double v;
  double red;
  double green;
  double blue;
};

struct TexturedMaterial {
  std::uint16_t texture_page;
  std::uint16_t clut;
  bool semi_transparent;
  bool resident{};
  std::uint8_t texture_bank{};
  std::uint8_t texture_source_page{0xffU};
};

game::DynamicLightVertexColor
applyRetailNightvision(game::DynamicLightVertexColor color) noexcept {
  // Night vision is a post-composition TILE grade. Preserve the lit source
  // RGB here so the normal far colour and per-vertex depth cue remain visible
  // beneath that filter instead of being flattened by a second green pass.
  return color;
}

VertexColor decodeColor(std::uint16_t color) {
  return VertexColor{
      static_cast<std::uint8_t>((color & 0x1fU) << 3U),
      static_cast<std::uint8_t>(((color >> 5U) & 0x1fU) << 3U),
      static_cast<std::uint8_t>(((color >> 10U) & 0x1fU) << 3U),
  };
}

TexturedVertex
makeTexturedVertex(const SVECTOR &position, assets::EmdUv uv, VertexColor color,
                   bool apply_lighting = true,
                   game::DynamicLightPoint normal = {},
                   const game::DynamicLightFrame *dynamic_lights = nullptr) {
  auto lit = game::DynamicLightVertexColor{color.red, color.green, color.blue};
  if (apply_lighting) {
    const auto retail_lit = game::applyRetailVertexLighting(
        lit, active_retail_vertex_lights,
        {static_cast<double>(position.vx), static_cast<double>(position.vy),
         static_cast<double>(position.vz)},
        active_retail_light_projection);
    const auto point = game::DynamicLightPoint{
        static_cast<double>(position.vx), static_cast<double>(position.vy),
        static_cast<double>(position.vz)};
    const auto normal_length_squared =
        normal.x * normal.x + normal.y * normal.y + normal.z * normal.z;
    const auto &light_frame =
        dynamic_lights != nullptr ? *dynamic_lights : active_dynamic_lights;
    const auto modulation =
        normal_length_squared > 0.000001
            ? game::sampleDynamicLightingNormalized(light_frame, point, normal)
            : game::sampleDynamicLighting(light_frame, point);
    lit = game::applyDynamicLighting(retail_lit, modulation);
  }
  lit = applyRetailNightvision(lit);
  return TexturedVertex{
      static_cast<double>(position.vx), static_cast<double>(position.vy),
      static_cast<double>(position.vz), static_cast<double>(uv.u),
      static_cast<double>(uv.v),        static_cast<double>(lit.red),
      static_cast<double>(lit.green),   static_cast<double>(lit.blue),
  };
}

VertexColor dynamicallyLit(VertexColor base, double x, double y,
                           double z) noexcept {
  const auto retail_lit = game::applyRetailVertexLighting(
      {base.red, base.green, base.blue}, active_retail_vertex_lights, {x, y, z},
      active_retail_light_projection);
  const auto lit = applyRetailNightvision(game::applyDynamicLighting(
      retail_lit,
      game::sampleDynamicLighting(active_dynamic_lights, {x, y, z})));
  return VertexColor{lit.red, lit.green, lit.blue};
}

VertexColor
dynamicallyLit(VertexColor base, double x, double y, double z,
               game::DynamicLightPoint normal,
               const game::DynamicLightFrame &light_frame) noexcept {
  const auto retail_lit = game::applyRetailVertexLighting(
      {base.red, base.green, base.blue}, active_retail_vertex_lights, {x, y, z},
      active_retail_light_projection);
  const auto lit = applyRetailNightvision(game::applyDynamicLighting(
      retail_lit,
      game::sampleDynamicLightingNormalized(light_frame, {x, y, z}, normal)));
  return VertexColor{lit.red, lit.green, lit.blue};
}

template <std::size_t Count>
game::DynamicLightPoint
visibleSurfaceNormal(const std::array<SVECTOR, Count> &vertices) noexcept {
  static_assert(Count >= 3U);
  const auto first = game::DynamicLightPoint{
      static_cast<double>(vertices[0].vx), static_cast<double>(vertices[0].vy),
      static_cast<double>(vertices[0].vz)};
  const auto edge_a =
      game::DynamicLightPoint{static_cast<double>(vertices[1].vx) - first.x,
                              static_cast<double>(vertices[1].vy) - first.y,
                              static_cast<double>(vertices[1].vz) - first.z};
  const auto edge_b =
      game::DynamicLightPoint{static_cast<double>(vertices[2].vx) - first.x,
                              static_cast<double>(vertices[2].vy) - first.y,
                              static_cast<double>(vertices[2].vz) - first.z};
  auto normal =
      game::DynamicLightPoint{edge_a.y * edge_b.z - edge_a.z * edge_b.y,
                              edge_a.z * edge_b.x - edge_a.x * edge_b.z,
                              edge_a.x * edge_b.y - edge_a.y * edge_b.x};
  const auto length = std::sqrt(normal.x * normal.x + normal.y * normal.y +
                                normal.z * normal.z);
  if (!std::isfinite(length) || length <= 0.000001) {
    return {};
  }
  normal = {normal.x / length, normal.y / length, normal.z / length};
  auto centre = game::DynamicLightPoint{};
  for (const auto &vertex : vertices) {
    centre.x += static_cast<double>(vertex.vx);
    centre.y += static_cast<double>(vertex.vy);
    centre.z += static_cast<double>(vertex.vz);
  }
  const auto inverse_count = 1.0 / static_cast<double>(Count);
  centre = {centre.x * inverse_count, centre.y * inverse_count,
            centre.z * inverse_count};
  const auto to_observer =
      game::DynamicLightPoint{active_dynamic_light_observer.x - centre.x,
                              active_dynamic_light_observer.y - centre.y,
                              active_dynamic_light_observer.z - centre.z};
  if (normal.x * to_observer.x + normal.y * to_observer.y +
          normal.z * to_observer.z <
      0.0) {
    normal = {-normal.x, -normal.y, -normal.z};
  }
  return normal;
}

void dynamicallyLight(
    POLY_GT3 &primitive, const std::array<SVECTOR, 3U> &vertices,
    const std::array<game::DynamicLightPoint, 3U> &normals,
    const game::DynamicLightFrame &light_frame =
        active_dynamic_lights) noexcept {
  const auto first =
      dynamicallyLit({primitive.r0, primitive.g0, primitive.b0}, vertices[0].vx,
                     vertices[0].vy, vertices[0].vz, normals[0], light_frame);
  const auto second =
      dynamicallyLit({primitive.r1, primitive.g1, primitive.b1}, vertices[1].vx,
                     vertices[1].vy, vertices[1].vz, normals[1], light_frame);
  const auto third =
      dynamicallyLit({primitive.r2, primitive.g2, primitive.b2}, vertices[2].vx,
                     vertices[2].vy, vertices[2].vz, normals[2], light_frame);
  setRGB0(&primitive, first.red, first.green, first.blue);
  setRGB1(&primitive, second.red, second.green, second.blue);
  setRGB2(&primitive, third.red, third.green, third.blue);
}

void dynamicallyLight(POLY_GT3 &primitive,
                      const std::array<SVECTOR, 3U> &vertices,
                      const game::DynamicLightFrame &light_frame =
                          active_dynamic_lights) noexcept {
  const auto normal = visibleSurfaceNormal(vertices);
  dynamicallyLight(primitive, vertices,
                   std::array<game::DynamicLightPoint, 3U>{normal, normal,
                                                           normal},
                   light_frame);
}

void dynamicallyLight(POLY_GT4 &primitive,
                      const std::array<SVECTOR, 4U> &vertices,
                      const game::DynamicLightFrame &light_frame =
                          active_dynamic_lights) noexcept {
  const auto normal = visibleSurfaceNormal(vertices);
  const auto first =
      dynamicallyLit({primitive.r0, primitive.g0, primitive.b0}, vertices[0].vx,
                     vertices[0].vy, vertices[0].vz, normal, light_frame);
  const auto second =
      dynamicallyLit({primitive.r1, primitive.g1, primitive.b1}, vertices[1].vx,
                     vertices[1].vy, vertices[1].vz, normal, light_frame);
  const auto third =
      dynamicallyLit({primitive.r2, primitive.g2, primitive.b2}, vertices[2].vx,
                     vertices[2].vy, vertices[2].vz, normal, light_frame);
  const auto fourth =
      dynamicallyLit({primitive.r3, primitive.g3, primitive.b3}, vertices[3].vx,
                     vertices[3].vy, vertices[3].vz, normal, light_frame);
  setRGB0(&primitive, first.red, first.green, first.blue);
  setRGB1(&primitive, second.red, second.green, second.blue);
  setRGB2(&primitive, third.red, third.green, third.blue);
  setRGB3(&primitive, fourth.red, fourth.green, fourth.blue);
}

double cameraDepth(const MATRIX &view, const TexturedVertex &vertex) {
  return preciseCameraDepth(view, vertex.x, vertex.y, vertex.z);
}

TexturedVertex interpolate(const TexturedVertex &first,
                           const TexturedVertex &second, double amount) {
  const auto component = [amount](double a, double b) {
    return std::lerp(a, b, amount);
  };
  const auto shared_edge_position = [&component](double a, double b) {
    // Adjacent PS1 tiles are clipped independently. Snap only generated
    // world-space intersections to a sub-unit canonical grid so the same
    // shared edge cannot differ by a floating-point ulp and open a crack.
    constexpr double intersection_grid = 4096.0;
    return std::nearbyint(component(a, b) * intersection_grid) /
           intersection_grid;
  };
  return TexturedVertex{
      shared_edge_position(first.x, second.x),
      shared_edge_position(first.y, second.y),
      shared_edge_position(first.z, second.z),
      component(first.u, second.u),
      component(first.v, second.v),
      component(first.red, second.red),
      component(first.green, second.green),
      component(first.blue, second.blue),
  };
}

enum class NearPlaneStatus {
  inside,
  intersecting,
  outside,
};

NearPlaneStatus classifyNearPlane(const MATRIX &view,
                                  std::span<const SVECTOR> vertices) {
  const auto behind = static_cast<std::size_t>(
      std::ranges::count_if(vertices, [&view](const SVECTOR &vertex) {
        return preciseCameraDepth(view, vertex.vx, vertex.vy, vertex.vz) <
               active_near_clip_depth;
      }));
  if (behind == 0U) {
    return NearPlaneStatus::inside;
  }
  return behind == vertices.size() ? NearPlaneStatus::outside
                                   : NearPlaneStatus::intersecting;
}

std::uint8_t quantizeColorOrUv(double value) {
  return static_cast<std::uint8_t>(
      std::clamp<long>(std::lround(value), 0L, 255L));
}

std::uint16_t relocateTexturePage(std::uint16_t texture_page,
                                  unsigned int texture_bank,
                                  unsigned int source_page = 0xffU) {
  const auto logical_page =
      source_page < 32U ? source_page : texture_page & 0x1fU;
  const auto bank = std::min<std::size_t>(texture_bank, 1U);
  return encodeResidentTexturePage(
      texture_page, streamed_texture_page_remap[bank][logical_page]);
}

std::uint16_t relocateClut(std::uint16_t clut, std::uint16_t texture_page,
                           unsigned int texture_bank,
                           unsigned int source_page = 0xffU) {
  const auto y = static_cast<unsigned int>(clut >> 6U);
  if (y < mission_clut_source_y || y >= mission_clut_source_y + 32U) {
    return clut;
  }
  const auto source_x_block = static_cast<unsigned int>(clut & 0x3fU);
  constexpr auto first_source_x_block = mission_clut_source_x / 16U;
  constexpr auto resident_x_block = mission_clut_resident_x / 16U;
  if (source_x_block < first_source_x_block ||
      source_x_block >= first_source_x_block + 16U) {
    return clut;
  }
  // Texture-page sharing and palette sharing are independent.  VLF seeds
  // bank zero, but a bank-one model can legitimately reuse the same TPAGE
  // address with a different ZCLUT row.  The residency builder aliases rows
  // only after comparing their actual bytes.
  static_cast<void>(texture_page);
  static_cast<void>(source_page);
  const auto bank = std::min<std::size_t>(texture_bank, 1U);
  const auto physical_row =
      streamed_clut_row_remap[bank][y - mission_clut_source_y];
  const auto physical_x_block =
      resident_x_block + source_x_block - first_source_x_block;
  return static_cast<std::uint16_t>(
      physical_x_block | ((physical_row + mission_clut_resident_y) << 6U));
}

bool frontFacing(long first, long second, long third,
                 unsigned int transformed_vertex_count) {
  auto first_x = static_cast<double>(screenX(first));
  auto first_y = static_cast<double>(screenY(first));
  auto second_x = static_cast<double>(screenX(second));
  auto second_y = static_cast<double>(screenY(second));
  auto third_x = static_cast<double>(screenX(third));
  auto third_y = static_cast<double>(screenY(third));

  // Cull in the same full-precision projection that is sent to the GPU.
  // Packed PS1 XY is quantized; using it here made thin and edge-on triangles
  // randomly change winding while their PGXP positions remained stable.
  const auto cache_end = PGXP_GetIndex(0);
  if (transformed_vertex_count >= 3U && cache_end != 0xffffU &&
      cache_end >= transformed_vertex_count) {
    const auto first_index = static_cast<ushort>(
        cache_end - static_cast<ushort>(transformed_vertex_count));
    PGXPVData precise_first{};
    PGXPVData precise_second{};
    PGXPVData precise_third{};
    if (PGXP_GetCacheDataExact(&precise_first, first_index) != 0 &&
        PGXP_GetCacheDataExact(&precise_second,
                               static_cast<ushort>(first_index + 1U)) != 0 &&
        PGXP_GetCacheDataExact(&precise_third,
                               static_cast<ushort>(first_index + 2U)) != 0) {
      first_x = precise_first.sx;
      first_y = precise_first.sy;
      second_x = precise_second.sx;
      second_y = precise_second.sy;
      third_x = precise_third.sx;
      third_y = precise_third.sy;
    }
  }
  const auto area = (second_x - first_x) * (third_y - first_y) -
                    (second_y - first_y) * (third_x - first_x);
  return area > 1.0e-8;
}

[[nodiscard]] double projectedX(const WorldProjectedVertex &vertex) noexcept {
  return vertex.precise_valid ? vertex.precise.sx
                              : static_cast<double>(screenX(vertex.packed_xy));
}

[[nodiscard]] double projectedY(const WorldProjectedVertex &vertex) noexcept {
  return vertex.precise_valid ? vertex.precise.sy
                              : static_cast<double>(screenY(vertex.packed_xy));
}

[[nodiscard]] bool
frontFacingProjected(const WorldProjectedVertex &first,
                     const WorldProjectedVertex &second,
                     const WorldProjectedVertex &third) noexcept {
  const auto first_x = projectedX(first);
  const auto first_y = projectedY(first);
  const auto second_x = projectedX(second);
  const auto second_y = projectedY(second);
  const auto third_x = projectedX(third);
  const auto third_y = projectedY(third);
  const auto area = (second_x - first_x) * (third_y - first_y) -
                    (second_y - first_y) * (third_x - first_x);
  return area > 1.0e-8;
}

template <std::size_t Count>
[[nodiscard]] bool triviallyOutsideViewport(
    const std::array<const WorldProjectedVertex *, Count> &vertices) noexcept {
  constexpr auto margin = 96.0;
  auto all_left = true;
  auto all_right = true;
  auto all_above = true;
  auto all_below = true;
  for (const auto *vertex : vertices) {
    const auto x = projectedX(*vertex);
    const auto y = projectedY(*vertex);
    all_left = all_left && x < -margin;
    all_right = all_right && x > static_cast<double>(screen_width) + margin;
    all_above = all_above && y < -margin;
    all_below = all_below && y > static_cast<double>(screen_height) + margin;
  }
  return all_left || all_right || all_above || all_below;
}

template <std::size_t Count>
[[nodiscard]] bool
triviallyOutsideViewport(const std::array<long, Count> &vertices) noexcept {
  constexpr auto margin = 96;
  auto all_left = true;
  auto all_right = true;
  auto all_above = true;
  auto all_below = true;
  for (const auto vertex : vertices) {
    const auto x = static_cast<int>(screenX(vertex));
    const auto y = static_cast<int>(screenY(vertex));
    all_left = all_left && x < -margin;
    all_right = all_right && x > screen_width + margin;
    all_above = all_above && y < -margin;
    all_below = all_below && y > screen_height + margin;
  }
  return all_left || all_right || all_above || all_below;
}

std::uint8_t emdTextureSourcePage(const assets::EmdScene &scene,
                                  const assets::EmdPolygon &polygon) {
  const auto page = assets::resolveEmdTexturePageSource(
      polygon.texture_page, scene.texturePageMask(), streamed_vlf_page_mask);
  if (!page) {
    throw core::Error{
        core::ErrorCode::unsupported,
        "EMD texture selector has ambiguous source-page ownership"};
  }
  return *page;
}

void configurePrimitive(POLY_GT3 &primitive, const assets::EmdSection &section,
                        const assets::EmdPolygon &polygon,
                        unsigned int texture_bank,
                        unsigned int texture_source_page,
                        std::span<const std::uint16_t> live_colors = {}) {
  setPolyGT3(&primitive);
  const auto color = [&](std::size_t corner) {
    const auto index = polygon.vertex_indices[corner];
    return decodeColor(live_colors.empty() ? section.vertices[index].color
                                           : live_colors[index]);
  };
  const auto color0 = color(0U);
  const auto color1 = color(1U);
  const auto color2 = color(2U);
  setRGB0(&primitive, color0.red, color0.green, color0.blue);
  setRGB1(&primitive, color1.red, color1.green, color1.blue);
  setRGB2(&primitive, color2.red, color2.green, color2.blue);
  setUV3(&primitive, polygon.uv[0].u, polygon.uv[0].v, polygon.uv[1].u,
         polygon.uv[1].v, polygon.uv[2].u, polygon.uv[2].v);
  primitive.clut = relocateClut(polygon.clut, polygon.texture_page,
                                texture_bank, texture_source_page);
  primitive.tpage = relocateTexturePage(polygon.texture_page, texture_bank,
                                        texture_source_page);
}

void configurePrimitive(POLY_GT4 &primitive, const assets::EmdSection &section,
                        const assets::EmdPolygon &polygon,
                        unsigned int texture_bank,
                        unsigned int texture_source_page,
                        std::span<const std::uint16_t> live_colors = {}) {
  setPolyGT4(&primitive);
  const auto color = [&](std::size_t corner) {
    const auto index = polygon.vertex_indices[corner];
    return decodeColor(live_colors.empty() ? section.vertices[index].color
                                           : live_colors[index]);
  };
  const auto color0 = color(0U);
  const auto color1 = color(1U);
  const auto color2 = color(2U);
  const auto color3 = color(3U);
  setRGB0(&primitive, color0.red, color0.green, color0.blue);
  setRGB1(&primitive, color1.red, color1.green, color1.blue);
  setRGB2(&primitive, color2.red, color2.green, color2.blue);
  setRGB3(&primitive, color3.red, color3.green, color3.blue);
  setUV4(&primitive, polygon.uv[0].u, polygon.uv[0].v, polygon.uv[1].u,
         polygon.uv[1].v, polygon.uv[2].u, polygon.uv[2].v, polygon.uv[3].u,
         polygon.uv[3].v);
  primitive.clut = relocateClut(polygon.clut, polygon.texture_page,
                                texture_bank, texture_source_page);
  primitive.tpage = relocateTexturePage(polygon.texture_page, texture_bank,
                                        texture_source_page);
}

void configurePrimitive(POLY_GT3 &primitive,
                        const assets::GmdTriangle &triangle,
                        unsigned int texture_bank,
                        VertexColor color = {128U, 128U, 128U}) {
  setPolyGT3(&primitive);
  setRGB0(&primitive, color.red, color.green, color.blue);
  setRGB1(&primitive, color.red, color.green, color.blue);
  setRGB2(&primitive, color.red, color.green, color.blue);
  setUV3(&primitive, triangle.uv[0].u, triangle.uv[0].v, triangle.uv[1].u,
         triangle.uv[1].v, triangle.uv[2].u, triangle.uv[2].v);
  primitive.clut =
      relocateClut(triangle.clut, triangle.texture_page, texture_bank);
  primitive.tpage = relocateTexturePage(triangle.texture_page, texture_bank);
  if (triangle.semi_transparent) {
    setSemiTrans(&primitive, 1);
  }
}

void configurePrimitive(POLY_GT3 &primitive,
                        const assets::HmdTriangle &triangle,
                        unsigned int texture_bank,
                        VertexColor color = {128U, 128U, 128U}) {
  setPolyGT3(&primitive);
  setRGB0(&primitive, color.red, color.green, color.blue);
  setRGB1(&primitive, color.red, color.green, color.blue);
  setRGB2(&primitive, color.red, color.green, color.blue);
  setUV3(&primitive, triangle.uv[0].u, triangle.uv[0].v, triangle.uv[1].u,
         triangle.uv[1].v, triangle.uv[2].u, triangle.uv[2].v);
  primitive.clut =
      relocateClut(triangle.clut, triangle.texture_page, texture_bank);
  primitive.tpage = relocateTexturePage(triangle.texture_page, texture_bank);
}

VertexColor retailHmdBackColor(const game::SceneObject &object) noexcept {
  if (!object.legacy_hmd_back_color_valid) {
    return {128U, 128U, 128U};
  }
  const auto channel = [](std::int16_t q12) {
    return static_cast<std::uint8_t>(
        std::clamp((static_cast<int>(q12) + 8) >> 4, 0, 255));
  };
  return {
      channel(object.legacy_hmd_back_color_q12[0]),
      channel(object.legacy_hmd_back_color_q12[1]),
      channel(object.legacy_hmd_back_color_q12[2]),
  };
}

VertexColor retailGmdObjectBackColor(const game::SceneObject &object) noexcept {
  if (!object.legacy_hmd_back_color_valid) {
    return {128U, 128U, 128U};
  }
  // GMD consumes the display controller's intensity, not HMD's independent
  // RGB back-color channels. Treating those three words as GMD RGB produced
  // saturated purple crates. Preserve their luminance so the crate still
  // follows authored darkness and live vertex lights without acquiring hue.
  const auto intensity =
      game::retailGmdBackColorModulation(object.legacy_hmd_back_color_q12);
  return {intensity.red, intensity.green, intensity.blue};
}

Vector3 transformPoint(double x, double y, double z,
                       const assets::MissionTransform &transform) {
  const auto component = [&](std::size_t row) {
    return (static_cast<double>(transform.rotation[row * 3U]) * x +
            static_cast<double>(transform.rotation[row * 3U + 1U]) * y +
            static_cast<double>(transform.rotation[row * 3U + 2U]) * z) /
           4096.0;
  };
  return Vector3{
      static_cast<double>(transform.x) + component(0),
      -static_cast<double>(transform.y) + component(1),
      static_cast<double>(transform.z) + component(2),
  };
}

class WorldSectionFrustum final {
public:
  WorldSectionFrustum(const MATRIX &view, int projection)
      : projection_(static_cast<double>(std::max(projection, 1))) {
    if (!PGXP_MatrixLookup(&view, rotation_.data()) ||
        !PGXP_MatrixLookupTranslation(&view, translation_.data())) {
      for (auto row = std::size_t{}; row < 3U; ++row) {
        for (auto column = std::size_t{}; column < 3U; ++column) {
          rotation_[row * 3U + column] =
              static_cast<double>(view.m[row][column]) / 4096.0;
        }
        translation_[row] = static_cast<double>(view.t[row]);
      }
    }
  }

  [[nodiscard]] bool outside(const assets::EmdBounds &bounds) const noexcept {
    return outside(
        bounds, [](double x, double y, double z) { return Vector3{x, y, z}; });
  }

  [[nodiscard]] bool
  outside(const assets::EmdBounds &bounds,
          const assets::MissionTransform &transform) const noexcept {
    return outside(bounds, [&](double x, double y, double z) {
      return transformPoint(x, y, z, transform);
    });
  }

  [[nodiscard]] bool
  outside(std::span<const Vector3> world_points) const noexcept {
    if (world_points.empty()) {
      return false;
    }
    constexpr double near_margin = 32.0;
    constexpr double horizontal_margin = 96.0;
    constexpr double vertical_margin = 72.0;
    constexpr double horizontal_extent =
        static_cast<double>(screen_width) * 0.5 + horizontal_margin;
    constexpr double vertical_extent =
        static_cast<double>(screen_height) * 0.5 + vertical_margin;
    const auto conservative_near = active_near_clip_depth - near_margin;
    auto all_behind_near = true;
    auto any_behind_near = false;
    auto all_left = true;
    auto all_right = true;
    auto all_above = true;
    auto all_below = true;
    for (const auto &world : world_points) {
      const auto camera_x = rotation_[0] * world.x + rotation_[1] * world.y +
                            rotation_[2] * world.z + translation_[0];
      const auto camera_y = rotation_[3] * world.x + rotation_[4] * world.y +
                            rotation_[5] * world.z + translation_[1];
      const auto camera_z = rotation_[6] * world.x + rotation_[7] * world.y +
                            rotation_[8] * world.z + translation_[2];
      const auto behind_near = camera_z < conservative_near;
      all_behind_near = all_behind_near && behind_near;
      any_behind_near = any_behind_near || behind_near;
      all_left =
          all_left && camera_x * projection_ < -horizontal_extent * camera_z;
      all_right =
          all_right && camera_x * projection_ > horizontal_extent * camera_z;
      all_above =
          all_above && camera_y * projection_ < -vertical_extent * camera_z;
      all_below =
          all_below && camera_y * projection_ > vertical_extent * camera_z;
    }
    if (all_behind_near) {
      return true;
    }
    // Keep geometry crossing the near plane: its projection can cover the
    // complete output even when all remaining points share a side plane.
    if (any_behind_near) {
      return false;
    }
    return all_left || all_right || all_above || all_below;
  }

private:
  struct CameraPoint {
    double x{};
    double y{};
    double z{};
  };

  template <typename Transform>
  [[nodiscard]] bool outside(const assets::EmdBounds &bounds,
                             Transform &&transform) const noexcept {
    std::array<CameraPoint, 8U> points{};
    auto point_index = std::size_t{};
    for (const auto x : {bounds.minimum_x, bounds.maximum_x}) {
      for (const auto y : {bounds.minimum_y, bounds.maximum_y}) {
        for (const auto z : {bounds.minimum_z, bounds.maximum_z}) {
          const auto world =
              transform(static_cast<double>(x), static_cast<double>(y),
                        static_cast<double>(z));
          auto &camera = points[point_index++];
          camera.x = rotation_[0] * world.x + rotation_[1] * world.y +
                     rotation_[2] * world.z + translation_[0];
          camera.y = rotation_[3] * world.x + rotation_[4] * world.y +
                     rotation_[5] * world.z + translation_[1];
          camera.z = rotation_[6] * world.x + rotation_[7] * world.y +
                     rotation_[8] * world.z + translation_[2];
        }
      }
    }

    constexpr double near_margin = 32.0;
    const auto conservative_near = active_near_clip_depth - near_margin;
    if (std::ranges::all_of(points, [conservative_near](const auto &point) {
          return point.z < conservative_near;
        })) {
      return true;
    }
    // A box crossing the near plane can fan out over the complete screen.
    // Leave it to the polygon clipper rather than risk a false side-plane
    // rejection. Fully forward boxes use deliberately oversized margins.
    if (std::ranges::any_of(points, [conservative_near](const auto &point) {
          return point.z < conservative_near;
        })) {
      return false;
    }
    constexpr double horizontal_margin = 96.0;
    constexpr double vertical_margin = 72.0;
    constexpr double horizontal_extent =
        static_cast<double>(screen_width) * 0.5 + horizontal_margin;
    constexpr double vertical_extent =
        static_cast<double>(screen_height) * 0.5 + vertical_margin;
    const auto outside_plane = [&](const auto &predicate) {
      return std::ranges::all_of(points, predicate);
    };
    return outside_plane([&](const auto &point) {
             return point.x * projection_ < -horizontal_extent * point.z;
           }) ||
           outside_plane([&](const auto &point) {
             return point.x * projection_ > horizontal_extent * point.z;
           }) ||
           outside_plane([&](const auto &point) {
             return point.y * projection_ < -vertical_extent * point.z;
           }) ||
           outside_plane([&](const auto &point) {
             return point.y * projection_ > vertical_extent * point.z;
           });
  }

  std::array<double, 9U> rotation_{};
  std::array<double, 3U> translation_{};
  double projection_{};
};

SVECTOR transformVertex(std::int16_t x, std::int16_t y, std::int16_t z,
                        const assets::MissionTransform &transform) {
  const auto point = transformPoint(x, y, z, transform);
  return makeVertex(point.x, point.y, point.z);
}

SVECTOR transformVertex(const assets::GmdVertex &vertex,
                        const assets::MissionTransform &transform) {
  return transformVertex(vertex.x, vertex.y, vertex.z, transform);
}

SVECTOR transformVertex(const assets::EmdVertex &vertex,
                        const assets::MissionTransform &transform) {
  return transformVertex(vertex.x, vertex.y, vertex.z, transform);
}
