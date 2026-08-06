void drawSolidRect(int x, int y, int width, int height, std::uint8_t red,
                   std::uint8_t green, std::uint8_t blue) {
  if (width <= 0 || height <= 0) {
    return;
  }
  TILE tile{};
  setTile(&tile);
  setRGB0(&tile, red, green, blue);
  setXY0(&tile, static_cast<float>(x), static_cast<float>(y));
  setWH(&tile, static_cast<float>(width), static_cast<float>(height));
  DrawPrim(&tile);
}

void drawGameBrightness(std::uint8_t value) {
  if (value == 50U) {
    return;
  }
  const auto brighter = value > 50U;
  const auto distance = brighter ? value - 50U : 50U - value;
  const auto intensity = static_cast<std::uint8_t>(
      std::min(100U, static_cast<unsigned int>(distance) * 2U));
  const auto blend_mode = brighter ? BM_ADD : BM_SUBTRACT;
  const auto abr = brighter ? 1 : 2;
  GR_SetBlendMode(blend_mode);
  GR_EnableDepth(0);
  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, GetTPage(0, abr, 0, 0));
  DrawPrim(&page);
  TILE tile{};
  setTile(&tile);
  setSemiTrans(&tile, 1);
  setRGB0(&tile, intensity, intensity, intensity);
  setXY0(&tile, 0.0F, 0.0F);
  setWH(&tile, static_cast<float>(screen_width),
        static_cast<float>(screen_height));
  DrawPrim(&tile);
  // Finish the full-screen blend before the opaque HUD changes blend mode;
  // otherwise PsyCross re-enables PGXP depth while replaying the next split.
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
}

void drawVirusScannerFilter(bool active) {
  if (!active) {
    return;
  }
  // The retail scanner grades the scene into a dark amber inspection image.
  // The separately rendered GRGLO/GDF pass remains untinted and therefore
  // reads as the bright X-ray content behind otherwise opaque containers.
  DrawSync(0);
  GR_SetBlendMode(BM_AVERAGE);
  GR_EnableDepth(0);
  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, GetTPage(0, 0, 0, 0));
  DrawPrim(&page);
  TILE tile{};
  setTile(&tile);
  setSemiTrans(&tile, 1);
  setRGB0(&tile, 128U, 80U, 30U);
  setXY0(&tile, 0.0F, 0.0F);
  setWH(&tile, static_cast<float>(screen_width),
        static_cast<float>(screen_height));
  DrawPrim(&tile);
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(1);
}

void drawRetailScreenFilter(
    const game::LegacyEnvironmentBridgeState &environment,
    bool retail_environment_active) {
  if (!retail_environment_active || !environment.screen_filter_enabled) {
    return;
  }
  const auto material = environment.screen_filter_material;
  if (material >= 4U) {
    return;
  }
  // FUN_800c9140 emits this TILE at OT depth zero. Descriptor +0x14 is the
  // authored PS1 ABR, independent of the GTE DQA/DQB depth-cue fog.
  constexpr std::array blend_modes{
      BM_AVERAGE,
      BM_ADD,
      BM_SUBTRACT,
      BM_ADD_QUATER_SOURCE,
  };
  GR_SetBlendMode(blend_modes[material]);
  GR_EnableDepth(0);
  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, GetTPage(0, static_cast<int>(material), 0, 0));
  DrawPrim(&page);
  TILE tile{};
  setTile(&tile);
  setSemiTrans(&tile, 1);
  setRGB0(&tile, environment.screen_filter_color.red,
          environment.screen_filter_color.green,
          environment.screen_filter_color.blue);
  setXY0(&tile, 0.0F, 0.0F);
  setWH(&tile, static_cast<float>(screen_width),
        static_cast<float>(screen_height));
  DrawPrim(&tile);
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(1);
}

void drawMapFade(std::uint8_t intensity) {
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
  setWH(&tile, static_cast<float>(screen_width),
        static_cast<float>(screen_height));
  DrawPrim(&tile);
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(1);
}

void drawHudSpriteAtResident(const assets::TimImage &image,
                             std::uint16_t resident_x, std::uint16_t resident_y,
                             int x, int y, std::uint8_t brightness = 128U,
                             bool raw_texture = false,
                             bool semi_transparent = false,
                             int blend_material = 0) {
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
  const auto width = static_cast<int>(image.displayWidth());
  const auto height = static_cast<int>(image.displayHeight());
  const auto texture_page =
      GetTPage(texturePageMode(image.mode()),
               semi_transparent ? blend_material : 0, page_x, page_y);

  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, texture_page);
  DrawPrim(&page);

  // PsyCross uses the active page for its texture format and the polygon's
  // page for UV lookup, so both must be explicit after the world OT.
  POLY_FT4 polygon{};
  setPolyFT4(&polygon);
  polygon.tpage = texture_page;
  setSemiTrans(&polygon, semi_transparent ? 1 : 0);
  polygon.clut = GetClut(hud_resident_clut_x, hud_resident_clut_y);
  setShadeTex(&polygon, raw_texture ? 1 : 0);
  setRGB0(&polygon, brightness, brightness, brightness);
  setXY4(&polygon, static_cast<float>(x), static_cast<float>(y),
         static_cast<float>(x + width), static_cast<float>(y),
         static_cast<float>(x), static_cast<float>(y + height),
         static_cast<float>(x + width), static_cast<float>(y + height));
  // PsyCross interpolates FT4 UVs over the destination extent. Use an
  // exclusive far edge so the final source texel is sampled instead of
  // clipping the rightmost/bottom row of narrow HUD glyphs.
  setUV4(&polygon, static_cast<u_char>(u0), static_cast<u_char>(v0),
         static_cast<u_char>(u0 + width), static_cast<u_char>(v0),
         static_cast<u_char>(u0), static_cast<u_char>(v0 + height),
         static_cast<u_char>(u0 + width), static_cast<u_char>(v0 + height));
  DrawPrim(&polygon);
}

void drawHudSprite(const assets::TimImage &image, int x, int y,
                   std::uint8_t brightness = 128U, bool raw_texture = false,
                   bool semi_transparent = false, int blend_material = 0) {
  drawHudSpriteAtResident(image, hudResidentX(image.pixels().x),
                          image.pixels().y, x, y, brightness, raw_texture,
                          semi_transparent, blend_material);
}

void drawHudSpriteScaled(const assets::TimImage &image, float x, float y,
                         float width, float height,
                         std::uint8_t brightness = 128U,
                         bool raw_texture = false) {
  const auto resident_x = hudResidentX(image.pixels().x);
  const auto resident_y = image.pixels().y;
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
  const auto source_width = static_cast<int>(image.displayWidth());
  const auto source_height = static_cast<int>(image.displayHeight());
  const auto texture_page =
      GetTPage(texturePageMode(image.mode()), 0, page_x, page_y);

  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, texture_page);
  DrawPrim(&page);

  POLY_FT4 polygon{};
  setPolyFT4(&polygon);
  polygon.tpage = texture_page;
  polygon.clut = GetClut(hud_resident_clut_x, hud_resident_clut_y);
  setShadeTex(&polygon, raw_texture ? 1 : 0);
  setRGB0(&polygon, brightness, brightness, brightness);
  setXY4(&polygon, x, y, x + width, y, x, y + height, x + width, y + height);
  setUV4(&polygon, static_cast<u_char>(u0), static_cast<u_char>(v0),
         static_cast<u_char>(u0 + source_width), static_cast<u_char>(v0),
         static_cast<u_char>(u0), static_cast<u_char>(v0 + source_height),
         static_cast<u_char>(u0 + source_width),
         static_cast<u_char>(v0 + source_height));
  DrawPrim(&polygon);
}

void renderProjectileSprites(const HudTextureAtlas &textures,
                             const game::GameplaySession &gameplay,
                             const RenderPresentationSnapshot &presentation,
                             const MATRIX &view,
                             std::vector<OT_TAG> &ordering_table,
                             PrimitiveBuffer &primitives, RenderStats &stats) {
  if (presentation.projectiles.empty()) {
    return;
  }

  const auto &camera = presentation.camera;
  const auto basis = viewBasis(camera);
  const auto projection = static_cast<double>(std::max(camera.projection, 1));
  constexpr auto center_x = static_cast<double>(screen_width) * 0.5;
  constexpr auto center_y = static_cast<double>(screen_height) * 0.5;
  constexpr double projectile_world_extent = 24.0;
  constexpr double maximum_screen_extent = 10.0;
  constexpr std::uint8_t projectile_brightness = 176U;

  for (const auto &projectile : presentation.projectiles) {
    if (!projectile.active ||
        projectile.phase != game::ProjectilePhase::flying ||
        (projectile.weapon != game::WeaponId::fragmentation_grenade &&
         projectile.weapon != game::WeaponId::gas_grenade)) {
      continue;
    }
    // HUD art is used as a world-space billboard, so it needs the same
    // authored-scene occlusion gate as detached pickup sprites. The PGXP
    // depth test remains active for per-pixel intersections after this
    // centre ray rejects a grenade hidden behind a complete wall.
    if (!gameplay.droppedItemVisibleFrom(
            camera.x, camera.y, camera.z, projectile.x, projectile.y,
            projectile.z, gameplay.currentRoom())) {
      continue;
    }

    const auto delta = Vector3{projectile.x - camera.x, projectile.y - camera.y,
                               projectile.z - camera.z};
    const auto component = [&delta](const Vector3 &axis) {
      return delta.x * axis.x + delta.y * axis.y + delta.z * axis.z;
    };
    const auto depth =
        preciseCameraDepth(view, projectile.x, projectile.y, projectile.z);
    if (depth <= active_near_clip_depth) {
      continue;
    }
    const auto projected_x =
        center_x + component(basis[0]) * projection / depth;
    const auto projected_y =
        center_y + component(basis[1]) * projection / depth;
    if (projected_x < -32.0 || projected_x > screen_width + 32.0 ||
        projected_y < -32.0 || projected_y > screen_height + 32.0) {
      continue;
    }

    const auto layers = game::weaponDefinition(projectile.weapon).icon.layers();
    if (layers.empty()) {
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
    const auto scale = std::min(
        {projection * projectile_world_extent / (depth * source_extent),
         maximum_screen_extent / source_extent, 1.0});
    const auto sort_depth =
        std::clamp(static_cast<int>(std::lround(depth * 0.25)), 1,
                   ordering_table_size - 1);

    for (std::size_t layer = 0U; layer < layers.size(); ++layer) {
      const auto &image = textures.image(layers[layer]);
      const auto resident_x = hudResidentX(image.pixels().x);
      const auto resident_y = image.pixels().y;
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
      const auto left = projected_x + (offsets[layer] - group_center) * scale;
      const auto top = projected_y - heights[layer] * scale * 0.5;
      const auto right = left + widths[layer] * scale;
      const auto bottom = top + heights[layer] * scale;

      const auto source_right = u0 + widths[layer] - 1;
      const auto source_bottom = v0 + heights[layer] - 1;
      auto &polygon = primitives.projectile_sprites.emplace_back();
      setPolyFT4(&polygon);
      polygon.tpage = texture_page;
      polygon.clut = GetClut(hud_resident_clut_x, hud_resident_clut_y);
      setRGB0(&polygon, projectile_brightness, projectile_brightness,
              projectile_brightness);
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

void drawHudSpriteRegionTintScaled(const assets::TimImage &page_image, float x,
                                   float y, std::uint8_t source_u,
                                   std::uint8_t source_v, std::uint8_t width,
                                   std::uint8_t height, float scale,
                                   game::LegacyRgbBridgeState color) {
  const auto &pixels = page_image.pixels();
  const auto resident_x = hudResidentX(pixels.x);
  const auto page_x =
      static_cast<int>(resident_x & static_cast<std::uint16_t>(~63U));
  const auto page_y =
      static_cast<int>(pixels.y & static_cast<std::uint16_t>(~255U));
  const auto texture_page =
      GetTPage(texturePageMode(page_image.mode()), 0, page_x, page_y);

  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, texture_page);
  DrawPrim(&page);

  POLY_FT4 polygon{};
  setPolyFT4(&polygon);
  polygon.tpage = texture_page;
  polygon.clut = GetClut(hud_resident_clut_x, hud_resident_clut_y);
  setRGB0(&polygon, color.red, color.green, color.blue);
  const auto right = x + static_cast<float>(width) * scale;
  const auto bottom = y + static_cast<float>(height) * scale;
  setXY4(&polygon, x, y, right, y, x, bottom, right, bottom);
  // See drawHudSprite(): the far UV edge is exclusive in this renderer.
  setUV4(&polygon, source_u, source_v, static_cast<u_char>(source_u + width),
         source_v, source_u, static_cast<u_char>(source_v + height),
         static_cast<u_char>(source_u + width),
         static_cast<u_char>(source_v + height));
  DrawPrim(&polygon);
}

void drawHudSpriteRegionTint(const assets::TimImage &page_image, int x, int y,
                             std::uint8_t source_u, std::uint8_t source_v,
                             std::uint8_t width, std::uint8_t height,
                             game::LegacyRgbBridgeState color) {
  drawHudSpriteRegionTintScaled(page_image, static_cast<float>(x),
                                static_cast<float>(y), source_u, source_v,
                                width, height, 1.0F, color);
}

void drawHudSpriteRegion(const assets::TimImage &page_image, int x, int y,
                         std::uint8_t source_u, std::uint8_t source_v,
                         std::uint8_t width, std::uint8_t height,
                         std::uint8_t brightness = 128U) {
  drawHudSpriteRegionTint(
      page_image, x, y, source_u, source_v, width, height,
      game::LegacyRgbBridgeState{brightness, brightness, brightness});
}

void drawRetailUiBackdrop(const game::LegacyUiBackdropBridgeState &backdrop,
                          int offset_x, int offset_y) {
  if (backdrop.semi_transparent) {
    DrawSync(0);
    GR_SetBlendMode(BM_AVERAGE);
    DR_TPAGE page{};
    SetDrawTPage(&page, 1, 0, GetTPage(0, 0, 0, 0));
    DrawPrim(&page);
  }
  POLY_F4 polygon{};
  setPolyF4(&polygon);
  setSemiTrans(&polygon, backdrop.semi_transparent ? 1 : 0);
  setRGB0(&polygon, backdrop.color.red, backdrop.color.green,
          backdrop.color.blue);
  const auto x = [offset_x](const auto &point) {
    return static_cast<float>(screen_width / 2 + offset_x + point.x);
  };
  const auto y = [offset_y](const auto &point) {
    return static_cast<float>(screen_height / 2 + offset_y + point.y);
  };
  setXY4(&polygon, x(backdrop.corners[0]), y(backdrop.corners[0]),
         x(backdrop.corners[1]), y(backdrop.corners[1]), x(backdrop.corners[2]),
         y(backdrop.corners[2]), x(backdrop.corners[3]),
         y(backdrop.corners[3]));
  DrawPrim(&polygon);
  if (backdrop.semi_transparent) {
    DrawSync(0);
    GR_SetBlendMode(BM_NONE);
  }
}

void drawRetailUiGlyphs(const HudTextureAtlas &textures,
                        std::span<const game::LegacyUiGlyphBridgeState> glyphs,
                        int offset_x, int offset_y,
                        const PsyCrossFontTexture *font_override = nullptr) {
  const auto &font_page = textures.image("FONTA.TIM");
  const ScopedPsyCrossFontTexture font_binding{
      font_override != nullptr ? font_override : textures.nativeFont()};
  for (const auto &glyph : glyphs) {
    if (glyph.width == 0U || glyph.height == 0U) {
      continue;
    }
    drawHudSpriteRegionTint(font_page, screen_width / 2 + offset_x + glyph.x,
                            screen_height / 2 + offset_y + glyph.y, glyph.u,
                            glyph.v, glyph.width, glyph.height, glyph.color);
  }
}

// Pause rendering lives in the same translation unit and uses the identical
// retail font table. Keep only these thin adapters here; the authoritative
// SCUS metadata and metrics are owned by game::hud.
constexpr int
originalHudGlyphAdvance(const game::OriginalHudGlyph &glyph) noexcept {
  return glyph.advance();
}

std::optional<game::OriginalHudGlyph> originalHudGlyph(char value) noexcept {
  return game::originalHudGlyph(value);
}

void drawOriginalHudText(const HudTextureAtlas &textures, std::string_view text,
                         int x, int y, std::uint8_t brightness = 208U) {
  const auto &font_page = textures.image("FONTA.TIM");
  const ScopedPsyCrossFontTexture font_binding{textures.nativeFont()};
  for (const auto character : text) {
    if (character == ' ') {
      x += 4;
      continue;
    }
    const auto glyph = game::originalHudGlyph(character);
    if (!glyph) {
      continue;
    }
    drawHudSpriteRegion(font_page, x, y, glyph->u, glyph->v, glyph->width,
                        glyph->height(), brightness);
    x += glyph->advance();
  }
}

void drawOriginalHudTextSubpixel(const HudTextureAtlas &textures,
                                 std::string_view text, float x, float y,
                                 std::uint8_t brightness = 208U) {
  const auto &font_page = textures.image("FONTA.TIM");
  const ScopedPsyCrossFontTexture font_binding{textures.nativeFont()};
  const auto color =
      game::LegacyRgbBridgeState{brightness, brightness, brightness};
  for (const auto character : text) {
    if (character == ' ') {
      x += 4.0F;
      continue;
    }
    const auto glyph = game::originalHudGlyph(character);
    if (!glyph) {
      continue;
    }
    drawHudSpriteRegionTintScaled(font_page, x, y, glyph->u, glyph->v,
                                  glyph->width, glyph->height(), 1.0F, color);
    x += static_cast<float>(glyph->advance());
  }
}

std::size_t originalHudDrawableGlyphCount(std::string_view text) noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(text, [](char character) {
        return game::originalHudGlyph(character).has_value();
      }));
}

std::size_t originalHudSourceGlyphCount(std::string_view text,
                                        bool reconstructed) noexcept {
  auto count = originalHudDrawableGlyphCount(text);
  if (!reconstructed) {
    return count;
  }

  // reconstructOriginalHudText represents one non-font controller icon with
  // the diagnostic word BUTTON.  The retail typewriter owns one packet for
  // that icon, not six.  Counting the placeholder as six made translated
  // prompts stop several glyphs early (notably the Lian Xing contact prompt).
  constexpr auto placeholder = std::string_view{"BUTTON"};
  for (auto cursor = text.find(placeholder); cursor != std::string_view::npos;
       cursor = text.find(placeholder, cursor + placeholder.size())) {
    count -= std::min(count, placeholder.size() - 1U);
  }
  return count;
}

std::string_view originalHudTextGlyphPrefix(std::string_view text,
                                            std::size_t glyph_count) noexcept {
  if (glyph_count == 0U) {
    return {};
  }
  auto visible = std::size_t{};
  for (auto index = std::size_t{}; index < text.size(); ++index) {
    if (game::originalHudGlyph(text[index]) && ++visible == glyph_count) {
      return text.substr(0U, index + 1U);
    }
  }
  return text;
}

void drawOriginalHudTextScaled(const HudTextureAtlas &textures,
                               std::string_view text, float x, float y,
                               float scale, game::LegacyRgbBridgeState color) {
  const auto &font_page = textures.image("FONTA.TIM");
  const ScopedPsyCrossFontTexture font_binding{textures.nativeFont()};
  for (const auto character : text) {
    if (character == ' ') {
      x += 4.0F * scale;
      continue;
    }
    const auto glyph = game::originalHudGlyph(character);
    if (!glyph) {
      continue;
    }
    drawHudSpriteRegionTintScaled(font_page, x, y, glyph->u, glyph->v,
                                  glyph->width, glyph->height(), scale, color);
    x += static_cast<float>(glyph->advance()) * scale;
  }
}

int originalHudMaximumLineWidth(std::string_view text) noexcept {
  auto maximum = 0;
  for (auto cursor = std::size_t{}; cursor <= text.size();) {
    const auto newline = text.find('\n', cursor);
    const auto end = newline == std::string_view::npos ? text.size() : newline;
    maximum = std::max(
        maximum, game::originalHudTextWidth(text.substr(cursor, end - cursor)));
    if (newline == std::string_view::npos) {
      break;
    }
    cursor = newline + 1U;
  }
  return maximum;
}

std::optional<char> originalEnglishHudCharacter(
    const game::LegacyUiGlyphBridgeState &glyph) noexcept {
  constexpr auto characters =
      std::string_view{"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRS"
                       "TUVWXYZ!\"'(),-./:?"};
  for (const auto character : characters) {
    const auto candidate = game::originalEnglishHudGlyph(character);
    if (candidate && candidate->u == glyph.u && candidate->v == glyph.v &&
        candidate->width == glyph.width &&
        glyph.height == candidate->height()) {
      return character;
    }
  }
  return std::nullopt;
}

std::string reconstructOriginalHudText(
    std::span<const game::LegacyUiGlyphBridgeState> glyphs) {
  std::string result;
  result.reserve(glyphs.size() + 8U);
  auto first_on_line = true;
  auto line_y = std::int16_t{};
  auto previous_right = 0;
  const auto append_space = [&result] {
    if (!result.empty() && result.back() != ' ' && result.back() != '\n') {
      result.push_back(' ');
    }
  };
  for (const auto &glyph : glyphs) {
    if (glyph.width == 0U || glyph.height == 0U) {
      continue;
    }
    if (first_on_line) {
      line_y = glyph.y;
    } else if (std::abs(static_cast<int>(glyph.y) - static_cast<int>(line_y)) >=
               4) {
      while (!result.empty() && result.back() == ' ') {
        result.pop_back();
      }
      result.push_back('\n');
      first_on_line = true;
      line_y = glyph.y;
    } else if (static_cast<int>(glyph.x) - previous_right >= 4) {
      append_space();
    }

    if (const auto character = originalEnglishHudCharacter(glyph)) {
      result.push_back(*character);
    } else {
      // Controller icons are separate solid glyphs. Retain a word-sized
      // placeholder so templates such as "Press %s to Contact %s" remain
      // matchable even when the original source pointer no longer exists.
      append_space();
      result.append("BUTTON");
      result.push_back(' ');
    }
    previous_right = static_cast<int>(glyph.x) + glyph.width;
    first_on_line = false;
  }
  while (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

bool drawBoundKeyboardMousePrompt(
    const HudTextureAtlas &textures,
    const game::LegacyUiMessageBridgeState &message,
    const KeyboardMouseBindings &bindings, int offset_x, int offset_y) {
  auto reconstructed = std::string{};
  auto reconstructed_source = false;
  auto prompt = keyboardMousePromptText(message.text, bindings);
  if (!prompt) {
    reconstructed = reconstructOriginalHudText(message.glyphs);
    prompt = keyboardMousePromptText(reconstructed, bindings);
    reconstructed_source = prompt.has_value();
  }
  if (!prompt || message.glyphs.empty()) {
    return false;
  }

  const auto retail_glyph_count =
      originalHudSourceGlyphCount(prompt->retail_text, reconstructed_source);
  const auto bound_glyph_count =
      originalHudDrawableGlyphCount(prompt->bound_text);
  if (retail_glyph_count == 0U || bound_glyph_count == 0U) {
    return false;
  }

  const auto visible_bound_glyphs =
      std::min(bound_glyph_count, (message.glyphs.size() * bound_glyph_count +
                                   retail_glyph_count - 1U) /
                                      retail_glyph_count);
  const auto visible_text =
      originalHudTextGlyphPrefix(prompt->bound_text, visible_bound_glyphs);
  const auto leftmost = std::ranges::min_element(
      message.glyphs, {}, &game::LegacyUiGlyphBridgeState::x);
  const auto topmost = std::ranges::min_element(
      message.glyphs, {}, &game::LegacyUiGlyphBridgeState::y);
  const auto origin_x = leftmost->x;
  const auto origin_y = topmost->y;
  const auto retail_width = game::originalHudTextWidth(prompt->retail_text);
  const auto bound_width = game::originalHudTextWidth(prompt->bound_text);
  if (retail_width <= 0 || bound_width <= 0) {
    return false;
  }

  // Retain retail's left edge and fit the replacement into the exact original
  // footprint. This keeps long mouse/key names inside the animated backdrop
  // without resizing it or disturbing its guest-authored transition.
  const auto absolute_x = screen_width / 2 + offset_x + origin_x;
  const auto screen_width_available =
      std::max(1, screen_width - 4 - absolute_x);
  const auto width_available = std::min(retail_width, screen_width_available);
  const auto scale = std::min(1.0F, static_cast<float>(width_available) /
                                        static_cast<float>(bound_width));
  drawOriginalHudTextScaled(
      textures, visible_text, static_cast<float>(absolute_x),
      static_cast<float>(screen_height / 2 + offset_y + origin_y), scale,
      message.glyphs.front().color);
  return true;
}

struct GameplayMessageLayout final {
  std::string wrapped_text;
  std::size_t source_glyph_count{};
  std::size_t presentation_glyph_count{};
  std::size_t visible_glyph_count{};
  float left{};
  float top{};
  float available_width{};
  float scale{1.0F};
  float block_height{8.0F};
};

struct WorldCalloutRevealState final {
  std::uint16_t object{};
  std::string text;
  double visible_glyphs{};
  double opacity{1.0};
  bool present{};
  Vector3 point{};
};

struct ProjectedHudAnchor final {
  double x{};
  double y{};
  double view_depth{};
};

struct GameplayHudScratch final {
  std::vector<std::optional<GameplayMessageLayout>> message_layouts;
  std::vector<std::uint8_t> retail_scope_messages;
  std::vector<WorldCalloutRevealState> world_callout_reveals;
  std::vector<std::size_t> status_indices;
  double reticle_reveal{1.0};
  double reticle_x{};
  double reticle_y{};
  double reticle_scale{1.0};
  double reticle_velocity_x{};
  double reticle_velocity_y{};
  double reticle_scale_velocity{};
  std::optional<std::uint16_t> reticle_target;
  std::uint8_t reticle_mode{};
  bool reticle_follow_initialized{};
  bool reticle_was_visible{};
};

void advanceReticleSpring(double &value, double &velocity, double target,
                          double angular_frequency,
                          double presentation_delta_seconds) noexcept {
  const auto delta = std::clamp(presentation_delta_seconds, 0.0, 1.0 / 15.0);
  if (delta <= 0.0) {
    return;
  }
  const auto displacement = value - target;
  const auto decay = std::exp(-angular_frequency * delta);
  const auto impulse = (velocity + angular_frequency * displacement) * delta;
  value = target + (displacement + impulse) * decay;
  velocity = (velocity - angular_frequency * impulse) * decay;
}

void updateReticleFollow(GameplayHudScratch &scratch,
                         std::optional<std::uint16_t> target,
                         const ProjectedHudAnchor &projected,
                         double target_scale, bool snap_to_anchor,
                         double presentation_delta_seconds) noexcept {
  const auto discontinuity =
      scratch.reticle_follow_initialized &&
      std::hypot(scratch.reticle_x - projected.x,
                 scratch.reticle_y - projected.y) > 160.0;
  if (snap_to_anchor || !scratch.reticle_follow_initialized ||
      scratch.reticle_target != target || discontinuity) {
    scratch.reticle_x = projected.x;
    scratch.reticle_y = projected.y;
    scratch.reticle_scale = target_scale;
    scratch.reticle_velocity_x = 0.0;
    scratch.reticle_velocity_y = 0.0;
    scratch.reticle_scale_velocity = 0.0;
    scratch.reticle_target = target;
    scratch.reticle_follow_initialized = true;
    return;
  }

  const auto previous_x = scratch.reticle_x;
  const auto previous_y = scratch.reticle_y;
  advanceReticleSpring(scratch.reticle_x, scratch.reticle_velocity_x,
                       projected.x, 10.0, presentation_delta_seconds);
  advanceReticleSpring(scratch.reticle_y, scratch.reticle_velocity_y,
                       projected.y, 10.0, presentation_delta_seconds);
  advanceReticleSpring(scratch.reticle_scale, scratch.reticle_scale_velocity,
                       target_scale, 18.0, presentation_delta_seconds);

  constexpr auto maximum_speed = 240.0;
  const auto delta = std::clamp(presentation_delta_seconds, 0.0, 1.0 / 15.0);
  const auto step_x = scratch.reticle_x - previous_x;
  const auto step_y = scratch.reticle_y - previous_y;
  const auto step_length = std::hypot(step_x, step_y);
  const auto maximum_step = maximum_speed * delta;
  if (maximum_step > 0.0 && step_length > maximum_step) {
    const auto amount = maximum_step / step_length;
    scratch.reticle_x = previous_x + step_x * amount;
    scratch.reticle_y = previous_y + step_y * amount;
    scratch.reticle_velocity_x *= amount;
    scratch.reticle_velocity_y *= amount;
  }
}

std::size_t originalHudLineCount(std::string_view text) noexcept {
  return text.empty()
             ? 0U
             : static_cast<std::size_t>(std::ranges::count(text, '\n')) + 1U;
}

std::optional<GameplayMessageLayout>
buildGameplayMessageLayout(const game::LegacyUiMessageBridgeState &message,
                           int offset_x, int offset_y,
                           const KeyboardMouseBindings &bindings,
                           game::CampaignDifficulty difficulty) {
  if (message.glyphs.empty()) {
    return std::nullopt;
  }
  const auto russian_language = game::russianLanguageActive();
  std::string source;
  std::string presented;
  std::string reveal_source;
  auto reconstructed_source = false;
  const auto resolve = [&](std::string_view observed, bool reconstructed) {
    if (observed.empty()) {
      return false;
    }
    const auto completed = game::completeGameplayTextSource(observed);
    const auto use_completed =
        completed &&
        (russian_language ||
         game::completedGameplayTextRequiredInEnglish(*completed));
    source = std::string{use_completed ? *completed : observed};
    presented = game::localizeTextCopy(source);
    // Some retail FONT/TEXT builders expose an identifiable source prefix
    // (for example "MISSION FAI") while their live packet list owns the
    // complete type-on lifetime. When completion selects canonical wording,
    // keep the observed packet source as the reveal denominator. Using
    // the longer completed phrase here left both English and Russian several
    // glyphs short when the guest retired the message.
    reveal_source = std::string{observed};
    reconstructed_source = reconstructed;
    if (russian_language && presented == source) {
      return false;
    }
    return true;
  };
  if (!resolve(message.text, false) &&
      !resolve(reconstructOriginalHudText(message.glyphs), true)) {
    return std::nullopt;
  }
  // Agent reuses the retail hard-mode flag for its AI behaviour, so the guest
  // emits the original Hard notification. Replace only its presentation text;
  // the guest-authored type-on, colour, backdrop and lifetime remain intact.
  const auto difficulty_presentation =
      game::campaignDifficultyGameplayPresentation(source, difficulty);
  if (difficulty_presentation != source) {
    presented = game::localizeTextCopy(difficulty_presentation);
  }
  // The retail contact template uses printf's first %s for the controller
  // icon. Preserve the second, already-authored contact name, but convert the
  // semantic button slot before the common native-token resolver runs.
  const auto prompt = keyboardMousePromptText(source, bindings);
  if (prompt) {
    if (!russian_language) {
      presented = prompt->bound_text;
    } else if (const auto slot = presented.find("%s");
               slot != std::string::npos) {
      presented.replace(slot, 2U, "%x");
    }
  }
  presented = keyboardMouseHintText(presented, bindings);

  const auto source_glyph_count =
      std::max(std::size_t{1U}, originalHudSourceGlyphCount(
                                    reveal_source, reconstructed_source));
  const auto presentation_glyph_count =
      originalHudDrawableGlyphCount(presented);
  if (source_glyph_count == 0U || presentation_glyph_count == 0U) {
    return std::nullopt;
  }
  const auto presented_source_glyphs =
      std::min(source_glyph_count, message.glyphs.size());

  const auto top = std::ranges::min_element(message.glyphs, {},
                                            &game::LegacyUiGlyphBridgeState::y);
  GameplayMessageLayout layout;
  layout.source_glyph_count = source_glyph_count;
  layout.presentation_glyph_count = presentation_glyph_count;
  layout.visible_glyph_count = gameplayMessageVisibleGlyphCount(
      source_glyph_count, presented_source_glyphs, presentation_glyph_count);

  if (russian_language) {
    // Native Russian notifications share one readable 8-pixel font size.
    // Long prose wraps inside a screen-safe maximum, while short status
    // messages keep a backing sized to their actual widest rendered line.
    constexpr int horizontal_margin = 16;
    constexpr int text_width = screen_width - horizontal_margin * 2;
    layout.wrapped_text = game::originalHudWrapText(presented, text_width);
    const auto horizontal = gameplayMessageHorizontalLayout(
        screen_width, horizontal_margin,
        originalHudMaximumLineWidth(layout.wrapped_text));
    layout.available_width = static_cast<float>(horizontal.width);
    layout.scale = 1.0F;
    layout.left = static_cast<float>(horizontal.left);
  } else {
    // English wording already fits the authored packet. Preserve its origin
    // and footprint while replacing only the number of glyphs the guest has
    // revealed. Dynamic PC binding names may be wider, so fit those into the
    // same backdrop instead of changing the original composition.
    const auto left = std::ranges::min_element(
        message.glyphs, {}, &game::LegacyUiGlyphBridgeState::x);
    const auto absolute_left = screen_width / 2 + offset_x + left->x;
    const auto source_width = std::max(1, originalHudMaximumLineWidth(source));
    const auto presentation_width =
        std::max(1, originalHudMaximumLineWidth(presented));
    const auto screen_width_available =
        std::max(1, screen_width - 4 - absolute_left);
    const auto width_available = std::min(source_width, screen_width_available);
    layout.wrapped_text = std::move(presented);
    layout.available_width = static_cast<float>(width_available);
    layout.scale = std::min(1.0F, static_cast<float>(width_available) /
                                      static_cast<float>(presentation_width));
    layout.left = static_cast<float>(absolute_left);
  }

  const auto line_count = originalHudLineCount(layout.wrapped_text);
  if (line_count == 0U) {
    return std::nullopt;
  }
  layout.block_height =
      8.0F * layout.scale +
      static_cast<float>(line_count - 1U) * 9.0F * layout.scale;
  const auto original_top =
      static_cast<float>(screen_height / 2 + offset_y + top->y);
  constexpr float vertical_margin = 4.0F;
  layout.top = std::clamp(original_top, vertical_margin,
                          static_cast<float>(screen_height) - vertical_margin -
                              layout.block_height);
  return layout;
}

void drawGameplayMessage(const HudTextureAtlas &textures,
                         const game::LegacyUiMessageBridgeState &message,
                         const GameplayMessageLayout &layout) {
  const auto &font_page = textures.image("FONTA.TIM");
  const ScopedPsyCrossFontTexture font_binding{textures.nativeFont()};
  auto presentation_glyph = std::size_t{};
  auto line = std::size_t{};
  for (auto cursor = std::size_t{}; cursor <= layout.wrapped_text.size();
       ++line) {
    const auto newline = layout.wrapped_text.find('\n', cursor);
    const auto end = newline == std::string_view::npos
                         ? layout.wrapped_text.size()
                         : newline;
    const auto line_text =
        std::string_view{layout.wrapped_text}.substr(cursor, end - cursor);
    const auto line_width =
        static_cast<float>(game::originalHudTextWidth(line_text)) *
        layout.scale;
    auto x = layout.left + (layout.available_width - line_width) * 0.5F;
    const auto y = layout.top + static_cast<float>(line) * 9.0F * layout.scale;
    for (const auto character : line_text) {
      if (character == ' ') {
        x += 4.0F * layout.scale;
        continue;
      }
      const auto glyph = game::originalHudGlyph(character);
      if (!glyph) {
        continue;
      }
      if (presentation_glyph >= layout.visible_glyph_count) {
        return;
      }
      const auto source_glyph =
          std::min(message.glyphs.size() - 1U,
                   presentation_glyph * layout.source_glyph_count /
                       layout.presentation_glyph_count);
      drawHudSpriteRegionTintScaled(font_page, x, y, glyph->u, glyph->v,
                                    glyph->width, glyph->height(), layout.scale,
                                    message.glyphs[source_glyph].color);
      x += static_cast<float>(glyph->advance()) * layout.scale;
      ++presentation_glyph;
    }
    if (newline == std::string_view::npos) {
      break;
    }
    cursor = newline + 1U;
  }
}

void arrangeStatusMessages(
    std::span<const game::LegacyUiMessageBridgeState> messages,
    std::span<std::optional<GameplayMessageLayout>> layouts,
    std::vector<std::size_t> &status_indices) {
  status_indices.clear();
  status_indices.reserve(layouts.size());
  for (auto index = std::size_t{}; index < layouts.size(); ++index) {
    if (layouts[index] &&
        messages[index].channel == game::LegacyUiMessageChannel::status) {
      status_indices.push_back(index);
    }
  }
  std::ranges::sort(status_indices, {},
                    [&](std::size_t index) { return layouts[index]->top; });
  if (status_indices.empty()) {
    return;
  }

  constexpr float spacing = 2.0F;
  auto cursor = 4.0F;
  for (const auto index : status_indices) {
    auto &layout = *layouts[index];
    layout.top = std::max(layout.top, cursor);
    cursor = layout.top + layout.block_height + spacing;
  }
  const auto excess = std::max(
      0.0F, cursor - spacing - (static_cast<float>(screen_height) - 4.0F));
  if (excess != 0.0F) {
    for (const auto index : status_indices) {
      layouts[index]->top -= excess;
    }
  }
}

bool drawStatusBackdrop(
    std::span<const game::LegacyUiMessageBridgeState> messages,
    std::span<const std::optional<GameplayMessageLayout>> layouts,
    const game::LegacyUiBackdropBridgeState &retail_backdrop, int offset_x,
    int offset_y) {
  auto any = false;
  auto left = static_cast<float>(screen_width);
  auto right = 0.0F;
  auto top = static_cast<float>(screen_height);
  auto bottom = 0.0F;
  constexpr float padding_x = 4.0F;
  constexpr float padding_y = 3.0F;
  for (auto index = std::size_t{}; index < layouts.size(); ++index) {
    if (!layouts[index] ||
        messages[index].channel != game::LegacyUiMessageChannel::status) {
      continue;
    }
    const auto &layout = *layouts[index];
    any = true;
    left = std::min(left, layout.left - padding_x);
    right = std::max(right, layout.left + layout.available_width + padding_x);
    top = std::min(top, layout.top - padding_y);
    bottom = std::max(bottom, layout.top + layout.block_height + padding_y);
  }
  if (!any) {
    return false;
  }

  left = std::clamp(left, 2.0F, static_cast<float>(screen_width - 2));
  right = std::clamp(right, left + 1.0F, static_cast<float>(screen_width - 2));
  top = std::clamp(top, 2.0F, static_cast<float>(screen_height - 2));
  bottom =
      std::clamp(bottom, top + 1.0F, static_cast<float>(screen_height - 2));
  const auto relative_x = [offset_x](float value) {
    return static_cast<std::int16_t>(
        std::lround(value - static_cast<float>(screen_width / 2 + offset_x)));
  };
  const auto relative_y = [offset_y](float value) {
    return static_cast<std::int16_t>(
        std::lround(value - static_cast<float>(screen_height / 2 + offset_y)));
  };
  auto backdrop = retail_backdrop;
  backdrop.corners = {{{relative_x(left), relative_y(top)},
                       {relative_x(right), relative_y(top)},
                       {relative_x(left), relative_y(bottom)},
                       {relative_x(right), relative_y(bottom)}}};
  drawRetailUiBackdrop(backdrop, offset_x, offset_y);
  return true;
}

void drawOriginalHudTextSolid(const HudTextureAtlas &textures,
                              std::string_view text, int x, int y,
                              std::uint8_t brightness = 208U) {
  if (textures.nativeFont() != nullptr) {
    drawOriginalHudText(textures, text, x, y, brightness);
    return;
  }
  const auto modulate = [brightness](std::uint16_t channel) {
    const auto expanded =
        static_cast<unsigned int>((channel << 3U) | (channel >> 2U));
    return static_cast<std::uint8_t>(
        std::min(255U, expanded * brightness / 128U));
  };
  for (const auto character : text) {
    if (character == ' ') {
      x += 4;
      continue;
    }
    const auto glyph = game::originalHudGlyph(character);
    if (!glyph) {
      continue;
    }
    for (auto row = std::uint8_t{}; row < glyph->height(); ++row) {
      for (auto column = std::uint8_t{}; column < glyph->width; ++column) {
        const auto palette_word = textures.fontPaletteWord(
            static_cast<std::uint8_t>(glyph->u + column),
            static_cast<std::uint8_t>(glyph->v + row));
        if (!palette_word || *palette_word == 0U) {
          continue;
        }
        drawSolidRect(x + column, y + row, 1, 1,
                      modulate(*palette_word & 0x1fU),
                      modulate((*palette_word >> 5U) & 0x1fU),
                      modulate((*palette_word >> 10U) & 0x1fU));
      }
    }
    x += glyph->advance();
  }
}

void drawOriginalStatusBar(int y, std::uint8_t value, std::uint8_t reveal,
                           std::uint8_t red, std::uint8_t green,
                           std::uint8_t blue, int offset_x, int offset_y) {
  const auto x = 18 + offset_x;
  y += offset_y;
  const auto width = value == 0U
                         ? std::uint8_t{}
                         : std::min<std::uint8_t>(
                               static_cast<std::uint8_t>(value + 3U), reveal);
  if (reveal != 0U) {
    constexpr auto frame_red = std::uint8_t{107U};
    constexpr auto frame_green = std::uint8_t{126U};
    constexpr auto frame_blue = std::uint8_t{231U};
    drawSolidRect(15 + offset_x, y, 1, 5, frame_red, frame_green, frame_blue);
    drawSolidRect(15 + offset_x, y + 5, static_cast<int>(reveal) + 1, 1,
                  frame_red, frame_green, frame_blue);
  }
  if (width != 0U) {
    drawSolidRect(x, y, static_cast<int>(width), 4, red, green, blue);
  }
}

void drawOriginalStatusScale(int offset_x, int offset_y, double reveal_phase) {
  constexpr auto red = std::uint8_t{107U};
  constexpr auto green = std::uint8_t{126U};
  constexpr auto blue = std::uint8_t{231U};
  const auto reveal_height = static_cast<int>(
      std::lround(sf::platform::retailHudInterpolatedExtent(reveal_phase, 30)));
  drawSolidRect(10 + offset_x, 26 + offset_y, 1, reveal_height + 1, red, green,
                blue);
  for (const auto y : {26, 41, 56}) {
    if (y <= 26 + reveal_height) {
      drawSolidRect(11 + offset_x, y + offset_y, 4, 1, red, green, blue);
    }
  }
}

std::optional<game::AgentMissionHudSample>
agentNpcHealthSample(const game::GameplaySession &gameplay,
                     std::uint16_t source, std::uint32_t class_id) {
  const auto &objects = gameplay.objects();
  for (std::size_t index = 0U;
       index < objects.size() &&
       index <=
           static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
       ++index) {
    const auto &object = objects[index];
    if (object.source_index != source || object.class_id != class_id) {
      continue;
    }
    const auto scene = static_cast<std::uint16_t>(index);
    const auto *npc = gameplay.npcState(scene);
    if (npc != nullptr && npc->source_index == source &&
        npc->maximum_health != 0U) {
      return game::AgentMissionHudSample{npc->health, npc->maximum_health};
    }
  }
  return std::nullopt;
}

std::optional<game::AgentMissionHudSample>
agentNpcRouteProgressSample(const game::GameplaySession &gameplay,
                            std::uint16_t source, std::uint32_t class_id) {
  const auto &objects = gameplay.objects();
  for (std::size_t index = 0U;
       index < objects.size() &&
       index <=
           static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
       ++index) {
    const auto &object = objects[index];
    if (object.source_index != source || object.class_id != class_id) {
      continue;
    }
    const auto scene = static_cast<std::uint16_t>(index);
    const auto *npc = gameplay.npcState(scene);
    if (npc == nullptr || !npc->active || npc->source_index != source) {
      return std::nullopt;
    }
    return game::makeAgentMissionRouteProgressSample(
        std::span<const game::NpcPatrolPoint>{npc->patrol_points},
        game::AgentMissionHudRoutePoint{npc->home_x, npc->home_y, npc->home_z},
        game::AgentMissionHudRoutePoint{npc->x, npc->y, npc->z});
  }
  return std::nullopt;
}

std::optional<game::AgentMissionHudSample>
agentGirdeuxTankDamageSample(const game::GameplaySession &gameplay) {
  constexpr auto girdeux_source = std::uint16_t{9U};
  constexpr auto girdeux_class = std::uint32_t{0x3cU};
  constexpr auto girdeux_health = std::uint32_t{100U};
  const auto &objects = gameplay.objects();
  for (std::size_t index = 0U;
       index < objects.size() &&
       index <=
           static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max());
       ++index) {
    const auto &object = objects[index];
    if (object.source_index != girdeux_source ||
        object.class_id != girdeux_class) {
      continue;
    }
    const auto scene = static_cast<std::uint16_t>(index);
    if (!gameplay.legacyDedicatedActorPresentation(scene)) {
      return std::nullopt;
    }
    const auto health =
        std::min<std::uint32_t>(gameplay.objectHealth(scene), girdeux_health);
    return game::AgentMissionHudSample{girdeux_health - health, girdeux_health};
  }
  return std::nullopt;
}

game::AgentMissionHudState
agentMissionHudState(const game::GameplaySession &gameplay) {
  game::AgentMissionHudState state;
  switch (gameplay.missionIndex()) {
  case 2U:
    state.aramov_escape = agentNpcRouteProgressSample(gameplay, 13U, 0x02U);
    break;
  case 4U:
    if (const auto detonation = gameplay.agentPark2BombDetonationPercent()) {
      state.bomb_detonation = game::AgentMissionHudSample{*detonation, 100U};
    }
    state.girdeux_tank_damage = agentGirdeuxTankDamageSample(gameplay);
    break;
  case 6U:
    state.phagan_health = agentNpcHealthSample(gameplay, 183U, 0x4cU);
    state.aramov_health = agentNpcHealthSample(gameplay, 153U, 0x01U);
    break;
  default:
    break;
  }
  return state;
}

std::array<std::uint8_t, 3U>
agentMissionHudFillColor(game::AgentMissionHudTone tone) noexcept {
  switch (tone) {
  case game::AgentMissionHudTone::friendly:
    return {100U, 255U, 100U};
  case game::AgentMissionHudTone::warning:
    return {255U, 190U, 80U};
  case game::AgentMissionHudTone::critical:
    return {255U, 100U, 100U};
  case game::AgentMissionHudTone::neutral:
    return {150U, 150U, 255U};
  }
  return {150U, 150U, 255U};
}

void drawAgentMissionHudMeter(const HudTextureAtlas &textures,
                              const game::AgentMissionHudMeter &meter,
                              std::size_t row, int offset_x, int offset_y) {
  constexpr auto frame_red = std::uint8_t{107U};
  constexpr auto frame_green = std::uint8_t{126U};
  constexpr auto frame_blue = std::uint8_t{231U};
  constexpr auto right_margin = 14;
  constexpr auto bar_width = 100;
  constexpr auto row_height = 15;
  const auto label_y = 18 + static_cast<int>(row) * row_height;
  const auto bar_y = 26 + static_cast<int>(row) * row_height;

  const auto label =
      game::localizeTextCopy(game::agentMissionHudLabel(meter.kind));
  const auto value = std::to_string(meter.percent);
  constexpr auto percent_width = 4;
  const auto value_width = game::originalHudTextWidth(value) + percent_width;
  const auto right = screen_width - right_margin + offset_x;
  const auto label_x =
      right - value_width - 4 - game::originalHudTextWidth(label);
  const auto value_x = right - value_width;

  drawOriginalHudTextSolid(textures, label, label_x, label_y + offset_y);
  drawOriginalHudTextSolid(textures, value, value_x, label_y + offset_y);

  // FONTA has no registered percent glyph. Keep the counter in the native HUD
  // font and complete the compact 3x5 sign with the same immediate primitive.
  const auto percent_x = right - 3;
  const auto percent_y = label_y + 1 + offset_y;
  drawSolidRect(percent_x, percent_y, 1, 1, frame_red, frame_green, frame_blue);
  drawSolidRect(percent_x + 2, percent_y + 4, 1, 1, frame_red, frame_green,
                frame_blue);
  drawSolidRect(percent_x + 2, percent_y, 1, 1, frame_red, frame_green,
                frame_blue);
  drawSolidRect(percent_x + 1, percent_y + 2, 1, 1, frame_red, frame_green,
                frame_blue);
  drawSolidRect(percent_x, percent_y + 4, 1, 1, frame_red, frame_green,
                frame_blue);

  const auto bar_x = right - bar_width;
  drawSolidRect(bar_x - 1, bar_y + offset_y, 1, 5, frame_red, frame_green,
                frame_blue);
  drawSolidRect(bar_x - 1, bar_y + 5 + offset_y, bar_width + 1, 1, frame_red,
                frame_green, frame_blue);
  if (meter.percent != 0U) {
    const auto color = agentMissionHudFillColor(meter.tone);
    drawSolidRect(bar_x, bar_y + offset_y, static_cast<int>(meter.percent), 4,
                  color[0], color[1], color[2]);
  }
}

void drawOriginalRadar(const game::GameplaySession &gameplay,
                       const game::OriginalRadarGeometry &geometry,
                       std::int32_t heading, int offset_x, int offset_y) {
  constexpr int center_x = 39;
  constexpr int center_y = 200;

  if (geometry.frame == 0U) {
    return;
  }

  // drawGameplayHud commits the preceding text/status layer before entering
  // this pass.  Reset the immediate state explicitly so opening the pause
  // menu can never be what initializes the radar's blend/depth state.
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);

  // Retail radar background: two overlapping semi-transparent blue quads
  // and the darker top fan visible in the reference capture. FUN_800410d0
  // expands these around (-153, 80), which is (39, 200) after the original
  // 192x120 Gs screen offset.
  DR_TPAGE blend_page{};
  SetDrawTPage(&blend_page, 1, 0, GetTPage(0, 0, 0, 0));
  DrawPrim(&blend_page);
  const auto translucent_rect = [offset_x, offset_y](int x, int y, int width,
                                                     int height) {
    if (width <= 0 || height <= 0) {
      return;
    }
    TILE tile{};
    setTile(&tile);
    setSemiTrans(&tile, 1);
    setRGB0(&tile, 40U, 48U, 80U);
    setXY0(&tile, static_cast<float>(x + offset_x),
           static_cast<float>(y + offset_y));
    setWH(&tile, static_cast<float>(width), static_cast<float>(height));
    DrawPrim(&tile);
  };
  translucent_rect(center_x - geometry.outer_half_width,
                   center_y - geometry.outer_half_height,
                   geometry.outer_half_width * 2,
                   geometry.outer_half_height * 2);
  translucent_rect(center_x - geometry.inner_half_width,
                   center_y - geometry.inner_half_height,
                   geometry.inner_half_width * 2,
                   geometry.inner_half_height * 2);

  POLY_G3 background_fan{};
  setPolyG3(&background_fan);
  setSemiTrans(&background_fan, 1);
  setRGB0(&background_fan, 0U, 0U, 0U);
  setRGB1(&background_fan, 40U, 48U, 80U);
  setRGB2(&background_fan, 40U, 48U, 80U);
  setXY3(&background_fan, static_cast<float>(center_x + offset_x),
         static_cast<float>(center_y + offset_y),
         static_cast<float>(center_x - geometry.outer_half_width + offset_x),
         static_cast<float>(center_y - geometry.outer_half_height + offset_y),
         static_cast<float>(center_x + geometry.outer_half_width + offset_x),
         static_cast<float>(center_y - geometry.outer_half_height + offset_y));
  DrawPrim(&background_fan);

  // PsyCross batches immediate primitives and changes depth state while
  // replaying blend splits. Finish the retail semi-transparent background
  // first, then keep every solid radar layer screen-space and depth-free.
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);

  constexpr auto radar_red = std::uint8_t{107U};
  constexpr auto radar_green = std::uint8_t{126U};
  constexpr auto radar_blue = std::uint8_t{231U};
  drawSolidRect(center_x - geometry.outer_half_width + offset_x,
                center_y + offset_y, geometry.outer_half_width * 2, 1,
                radar_red, radar_green, radar_blue);
  if (geometry.reticle_half_width != 0 && geometry.reticle_half_height != 0) {
    const auto left = center_x - geometry.reticle_half_width + offset_x;
    const auto right = center_x + geometry.reticle_half_width + offset_x;
    const auto top = center_y - geometry.reticle_half_height + offset_y;
    const auto bottom = center_y + geometry.reticle_half_height + offset_y;
    drawSolidRect(left, top, geometry.reticle_half_width * 2 + 1, 1, radar_red,
                  radar_green, radar_blue);
    drawSolidRect(left, bottom, geometry.reticle_half_width * 2 + 1, 1,
                  radar_red, radar_green, radar_blue);
    drawSolidRect(left, top, 1, geometry.reticle_half_height * 2 + 1, radar_red,
                  radar_green, radar_blue);
    drawSolidRect(right, top, 1, geometry.reticle_half_height * 2 + 1,
                  radar_red, radar_green, radar_blue);
  }

  // SCUS_942.40 stores one/two blue rectangles for each compass octant.
  // Only the current bearing is visible; drawing four permanent corners was
  // the reason the previous placeholder looked like another aiming reticle.
  constexpr auto compass = std::array{
      std::array{std::array{63, 192, 3, 16}, std::array{0, 0, 0, 0}},
      std::array{std::array{53, 180, 13, 2}, std::array{63, 180, 3, 8}},
      std::array{std::array{29, 180, 20, 2}, std::array{0, 0, 0, 0}},
      std::array{std::array{12, 180, 13, 2}, std::array{15, 180, 3, 8}},
      std::array{std::array{15, 192, 3, 16}, std::array{0, 0, 0, 0}},
      std::array{std::array{12, 220, 13, 2}, std::array{15, 212, 3, 8}},
      std::array{std::array{29, 220, 20, 2}, std::array{0, 0, 0, 0}},
      std::array{std::array{53, 220, 13, 2}, std::array{63, 212, 3, 8}},
  };
  constexpr std::array<std::size_t, 8> compass_counts{1, 2, 1, 2, 1, 2, 1, 2};
  const auto compass_index =
      static_cast<std::size_t>(
          (game::normalizeHeading(static_cast<std::int64_t>(heading) + 1024) +
           256) /
          512) %
      compass.size();
  if (geometry.frame == game::GameplayHud::reveal_duration) {
    for (std::size_t index = 0; index < compass_counts[compass_index];
         ++index) {
      const auto &rectangle = compass[compass_index][index];
      drawSolidRect(rectangle[0] + offset_x, rectangle[1] + offset_y,
                    rectangle[2], rectangle[3], radar_red, radar_green,
                    radar_blue);
    }
  }

  // The original radar is primitive-only. Project the mission's HMD actors
  // into the same player-relative square instead of inventing a texture.
  constexpr double radar_range = 1920.0;
  const auto horizontal_radius = static_cast<double>(geometry.outer_half_width);
  const auto vertical_radius = static_cast<double>(geometry.outer_half_height);
  const auto basis = game::headingBasis(heading);
  const auto target = gameplay.aimTarget();
  const auto &objects = gameplay.objects();
  const auto &models = gameplay.objectModels();
  auto marker_count = std::size_t{};
  for (const auto object_index : gameplay.activeObjects()) {
    if (!gameplay.objectAlive(object_index)) {
      continue;
    }
    const auto &object = objects[object_index];
    if (!std::holds_alternative<assets::HmdModel>(
            models[object.model].geometry)) {
      continue;
    }
    const auto delta_x =
        static_cast<double>(object.transform.x) - gameplay.player().x;
    const auto delta_z =
        static_cast<double>(object.transform.z) - gameplay.player().z;
    const auto distance = std::hypot(delta_x, delta_z);
    if (distance <= 1.0) {
      continue;
    }
    const auto local_x = delta_x * basis.right.x + delta_z * basis.right.z;
    const auto local_z = delta_x * basis.forward.x + delta_z * basis.forward.z;
    const auto marker_x =
        center_x +
        static_cast<int>(std::lround(
            std::clamp(local_x / radar_range, -1.0, 1.0) * horizontal_radius));
    const auto marker_y =
        center_y -
        static_cast<int>(std::lround(
            std::clamp(local_z / radar_range, -1.0, 1.0) * vertical_radius));
    const auto selected = target && *target == object_index;
    // FUN_8003be84: green -> yellow below 0x553, yellow -> red below
    // 0xaa7, then solid red. Distant actors remain clamped to the rim.
    constexpr auto green_to_yellow = 0x553U;
    constexpr auto yellow_to_red = 0xaa7U;
    const auto distance_units = static_cast<unsigned int>(std::min<double>(
        distance,
        static_cast<double>(std::numeric_limits<unsigned int>::max())));
    auto red = std::uint8_t{255U};
    auto green = std::uint8_t{};
    if (distance_units < green_to_yellow) {
      red = static_cast<std::uint8_t>(
          std::min(255U, distance_units * 0x300U / 0x1000U));
      green = 255U;
    } else if (distance_units < yellow_to_red) {
      const auto falloff =
          (distance_units - green_to_yellow) * 0x300U / 0x1000U;
      green = static_cast<std::uint8_t>(255U - std::min(255U, falloff));
    }
    const auto marker_size = selected ? 4 : 2;
    drawSolidRect(marker_x - marker_size / 2 + offset_x,
                  marker_y - marker_size / 2 + offset_y, marker_size,
                  marker_size, red, green, 0U);
    if (++marker_count == 6U) {
      break;
    }
  }

  // Player pointer uses the retail G3 colors and its native 8x4 footprint.
  POLY_G3 pointer{};
  setPolyG3(&pointer);
  setRGB0(&pointer, 196U, 196U, 254U);
  setRGB1(&pointer, 62U, 88U, 117U);
  setRGB2(&pointer, 62U, 88U, 117U);
  setXY3(&pointer, static_cast<float>(center_x + offset_x),
         static_cast<float>(center_y - 2 + offset_y),
         static_cast<float>(center_x - 4 + offset_x),
         static_cast<float>(center_y + 2 + offset_y),
         static_cast<float>(center_x + 4 + offset_x),
         static_cast<float>(center_y + 2 + offset_y));
  DrawPrim(&pointer);
}

void drawOriginalAimReticle(int center_x, int center_y, bool head_target,
                            double reveal, double scale = 1.0) {
  constexpr auto red = std::uint8_t{64U};
  constexpr auto green = std::uint8_t{255U};
  constexpr auto blue = std::uint8_t{64U};
  const auto geometry =
      game::scaledOriginalAimReticleGeometry(head_target, scale);
  const auto eased_reveal = std::clamp(reveal, 0.0, 1.0);
  const auto ray_slide = static_cast<int>(
      std::lround((1.0 - eased_reveal * eased_reveal) * 12.0 * scale));
  const auto left = center_x - geometry.half_width;
  const auto right = center_x + geometry.half_width;
  const auto top = center_y - geometry.half_height;
  const auto bottom = center_y + geometry.half_height;

  // Retail FUN_80041830 uses one complete target box plus four rays centred
  // on its sides.  Do not add a dark drop shadow: the PS1 primitive colour
  // is the exact 0x40ff40 green initialized by the executable.
  drawSolidRect(left, top, right - left + 1, 1, red, green, blue);
  drawSolidRect(left, bottom, right - left + 1, 1, red, green, blue);
  drawSolidRect(left, top, 1, bottom - top + 1, red, green, blue);
  drawSolidRect(right, top, 1, bottom - top + 1, red, green, blue);
  drawSolidRect(left - geometry.horizontal_ray - ray_slide, center_y,
                geometry.horizontal_ray, 1, red, green, blue);
  drawSolidRect(right + 1 + ray_slide, center_y, geometry.horizontal_ray, 1,
                red, green, blue);
  drawSolidRect(center_x, top - geometry.vertical_ray - ray_slide, 1,
                geometry.vertical_ray, red, green, blue);
  drawSolidRect(center_x, bottom + 1 + ray_slide, 1, geometry.vertical_ray, red,
                green, blue);
  drawSolidRect(center_x - 1, center_y, 3, 1, red, green, blue);
}

void drawOriginalWorldCallout(const HudTextureAtlas &textures,
                              std::string_view text, int center_x, int center_y,
                              std::uint8_t brightness = 224U) {
  const auto geometry = game::originalHeadshotCalloutGeometry();
  LINE_F3 callout{};
  setLineF3(&callout);
  setRGB0(&callout, brightness, brightness, brightness);
  setXY3(&callout, static_cast<float>(center_x + geometry.start_x),
         static_cast<float>(center_y + geometry.start_y),
         static_cast<float>(center_x + geometry.elbow_x),
         static_cast<float>(center_y + geometry.elbow_y),
         static_cast<float>(center_x + geometry.end_x),
         static_cast<float>(center_y + geometry.end_y));
  DrawPrim(&callout);

  const auto localized = game::localizeTextCopy(text);
  drawOriginalHudTextSolid(textures, localized, center_x + geometry.text_x,
                           center_y + geometry.text_y, brightness);
}

void drawOriginalHeadshotCallout(const HudTextureAtlas &textures,
                                 std::string_view text, int center_x,
                                 int center_y) {
  drawOriginalWorldCallout(textures, text.empty() ? "HEAD SHOT" : text,
                           center_x, center_y);
}

void drawOriginalSniperScope(const HudTextureAtlas &textures,
                             std::int32_t heading, bool head_target,
                             int offset_x, int offset_y) {
  const auto center_x = screen_width / 2 + offset_x;
  const auto center_y = screen_height / 2 + offset_y;
  const auto geometry = game::originalAimReticleGeometry(head_target);

  // Retail GsSPRITE 0x80119d94 uses attribute 0x51000000, tpage 0x000c and
  // RGB 64. Bit 30 enables semitransparency, bit 28 selects ABR 1 and bit 24
  // selects indexed-8. Every visible SCOPED.TIM palette entry carries STP,
  // producing the authored soft material around the centre sight. Drawing it
  // opaque loses that effect completely.
  const auto &scope = textures.image("SCOPED.TIM");
  drawHudSprite(scope, center_x - static_cast<int>(scope.displayWidth()) / 2,
                center_y - static_cast<int>(scope.displayHeight()) / 2, 64U,
                false, true, 1);

  // The target box/crosshair itself remains the exact FUN_80041830 packet
  // stream; this native layer supplies only the authored TIM material.
  // FUN_80040ba8 creates a five-label vertical wrap beside the target box
  // and keeps all eight authored bearings on the horizontal compass tape.
  constexpr std::array<std::string_view, 5> vertical_bearings{
      "SCP180.TIM", "SCP135.TIM", "SCP0.TIM", "SCP135.TIM", "SCP180.TIM",
  };
  const auto vertical_x = center_x - geometry.half_width - 27;
  for (std::size_t index = 0; index < vertical_bearings.size(); ++index) {
    const auto &bearing = textures.image(vertical_bearings[index]);
    const auto y = center_y + (static_cast<int>(index) - 2) * 18;
    drawHudSprite(bearing,
                  vertical_x - static_cast<int>(bearing.displayWidth()) / 2,
                  y - static_cast<int>(bearing.displayHeight()) / 2, 112U);
  }

  const auto normalized_heading = game::normalizeHeading(heading);
  const auto horizontal_y = center_y + geometry.half_height + 25;
  for (std::size_t index = 0; index < scope_bearings.size(); ++index) {
    auto delta = game::normalizeHeading(
        static_cast<std::int64_t>(index * 512U) - normalized_heading + 2048);
    delta -= 2048;
    const auto x = center_x + static_cast<int>(std::lround(
                                  static_cast<double>(delta) * 30.0 / 512.0));
    const auto &bearing = textures.image(scope_bearings[index]);
    drawHudSprite(
        bearing, x - static_cast<int>(bearing.displayWidth()) / 2, horizontal_y,
        std::abs(delta) < 256 ? std::uint8_t{176U} : std::uint8_t{112U});
  }
}

void drawOriginalNightvisionScope(const HudTextureAtlas &textures, int offset_x,
                                  int offset_y) {
  const auto center_x = screen_width / 2 + offset_x;
  const auto center_y = screen_height / 2 + offset_y;
  const auto draw_layer = [&](std::size_t index, int x, int y) {
    drawHudSpriteAtResident(textures.image(nightvision_scope_layers[index]),
                            nightvision_resident_x[index],
                            nightvision_resident_y, x, y);
  };

  // Resource group 0x2e is two concentric authored rings: the dense INFRA
  // pair and the three-piece A/B/C outer sight.
  const auto &dense_left = textures.image(nightvision_scope_layers[0]);
  const auto dense_y =
      center_y - static_cast<int>(dense_left.displayHeight()) / 2;
  draw_layer(0, center_x - static_cast<int>(dense_left.displayWidth()),
             dense_y);
  draw_layer(1, center_x, dense_y);

  auto outer_width = 0;
  for (std::size_t index = 2; index < nightvision_scope_layers.size();
       ++index) {
    outer_width += static_cast<int>(
        textures.image(nightvision_scope_layers[index]).displayWidth());
  }
  auto outer_x = center_x - outer_width / 2;
  for (std::size_t index = 2; index < nightvision_scope_layers.size();
       ++index) {
    const auto &layer = textures.image(nightvision_scope_layers[index]);
    draw_layer(index, outer_x,
               center_y - static_cast<int>(layer.displayHeight()) / 2);
    outer_x += static_cast<int>(layer.displayWidth());
  }

  // Mode 3's broken-corner frame is also emitted by the authoritative retail
  // raw packet stream; drawing a host copy here produced the second sight.
}

void drawOriginalScope(const HudTextureAtlas &textures, game::WeaponId weapon,
                       std::int32_t heading, bool head_target, int offset_x,
                       int offset_y) {
  if (weapon == game::WeaponId::nightvision_rifle) {
    drawOriginalNightvisionScope(textures, offset_x, offset_y);
  } else {
    drawOriginalSniperScope(textures, heading, head_target, offset_x, offset_y);
  }
}

void drawOriginalWeaponMenu(const HudTextureAtlas &textures,
                            const game::GameplayHud &hud, int offset_x,
                            int offset_y) {
  if (hud.weaponMenuFrames() == 0U) {
    return;
  }

  const auto geometry = game::originalWeaponMenuGeometry();
  const auto center_x = screen_width / 2 + offset_x;
  const auto center_y = screen_height / 2 + offset_y;

  // FUN_800405f4 layers the narrow full strip and the taller selected slot
  // using the PS1 average blend. Their overlap produces the darker central
  // backing seen behind the selected weapon.
  DrawSync(0);
  GR_EnableDepth(0);
  DR_TPAGE blend_page{};
  SetDrawTPage(&blend_page, 1, 0, GetTPage(0, 0, 0, 0));
  DrawPrim(&blend_page);
  const auto draw_backing = [&](int left, int top, int right, int bottom) {
    POLY_F4 polygon{};
    setPolyF4(&polygon);
    setSemiTrans(&polygon, 1);
    setRGB0(&polygon, geometry.background_color.red,
            geometry.background_color.green, geometry.background_color.blue);
    setXY4(&polygon, static_cast<float>(center_x + left),
           static_cast<float>(center_y + top),
           static_cast<float>(center_x + right),
           static_cast<float>(center_y + top),
           static_cast<float>(center_x + left),
           static_cast<float>(center_y + bottom),
           static_cast<float>(center_x + right),
           static_cast<float>(center_y + bottom));
    DrawPrim(&polygon);
  };
  draw_backing(geometry.strip_left, geometry.strip_top, geometry.strip_right,
               geometry.strip_bottom);
  draw_backing(geometry.selection_left, geometry.selection_top,
               geometry.selection_right, geometry.selection_bottom);
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);

  const auto draw_frame_line = [&](int y) {
    LINE_F2 line{};
    setLineF2(&line);
    setRGB0(&line, geometry.frame_color.red, geometry.frame_color.green,
            geometry.frame_color.blue);
    setXY2(&line, static_cast<float>(center_x + geometry.frame_left),
           static_cast<float>(center_y + y),
           static_cast<float>(center_x + geometry.frame_right),
           static_cast<float>(center_y + y));
    DrawPrim(&line);
  };
  draw_frame_line(geometry.frame_top);
  draw_frame_line(geometry.frame_bottom);

  // FUN_80040328 builds seven groups at x=-294..294 with 0x62 spacing
  // and y=-0x50 under the HUD's native 192x120 centre-origin transform.
  constexpr int spacing = 0x62;
  constexpr int centre_slot =
      static_cast<int>(game::weapon_menu_slot_count / 2U);
  constexpr int origin_y = screen_height / 2 - 0x50;
  const auto weapons = hud.weaponMenuWindow();
  for (std::size_t slot = 0; slot < weapons.size(); ++slot) {
    const auto layers = game::weaponDefinition(weapons[slot]).icon.layers();
    if (layers.empty()) {
      continue;
    }
    std::array<int, game::maximum_weapon_icon_layers> widths{};
    std::array<int, game::maximum_weapon_icon_layers> heights{};
    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
      const auto &image = textures.image(layers[layer]);
      widths[layer] = static_cast<int>(image.displayWidth());
      heights[layer] = static_cast<int>(image.displayHeight());
    }
    const auto offsets = game::originalWeaponIconOffsets(
        std::span<const int>{widths.data(), layers.size()});
    const auto centre_x = screen_width / 2 + offset_x +
                          (static_cast<int>(slot) - centre_slot) * spacing;
    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
      drawHudSprite(textures.image(layers[layer]), centre_x + offsets[layer],
                    origin_y + offset_y - heights[layer] / 2, 128U, true);
    }
  }
}

void drawRetailGrenadeTrajectory(const RenderPresentationSnapshot &presentation,
                                 int offset_x, int offset_y) {
  if (!presentation.grenade_trajectory) {
    return;
  }
  const auto &retail = *presentation.grenade_trajectory;
  // FUN_80027584 converts the hand and target into its up-positive ballistic
  // space. Keep that space for the retail solver below; only convert each
  // simulated point back to the renderer's down-positive Y at projection.
  const auto origin = Vector3{static_cast<double>(retail.origin.x),
                              static_cast<double>(retail.origin.y),
                              static_cast<double>(retail.origin.z)};
  const auto target = Vector3{static_cast<double>(retail.target.x),
                              static_cast<double>(retail.target.y),
                              static_cast<double>(retail.target.z)};
  const auto horizontal_x = target.x - origin.x;
  const auto horizontal_z = target.z - origin.z;
  const auto horizontal_distance = std::hypot(horizontal_x, horizontal_z);
  if (horizontal_distance < 1.0) {
    return;
  }
  const auto vertical_distance = target.y - origin.y;
  const auto direct_angle = std::atan2(vertical_distance, horizontal_distance);
  const auto charge =
      std::clamp(static_cast<double>(retail.strength_q12) / 4096.0, 0.0, 1.0);
  // FUN_80027094 blends the direct pitch toward +0x400 and solves the launch
  // speed against -0x2916 Q12 gravity. Use the same retail inputs and constant
  // so the final stitch marks the point reached by FUN_80026808.
  const auto launch_angle =
      std::lerp(direct_angle, std::numbers::pi * 0.5, charge);
  const auto cosine = std::cos(launch_angle);
  const auto tangent = std::tan(launch_angle);
  constexpr double gravity = 10518.0 / 4096.0;
  const auto denominator = 2.0 * cosine * cosine *
                           (horizontal_distance * tangent - vertical_distance);
  if (!std::isfinite(denominator) || denominator <= 0.001) {
    return;
  }
  const auto speed_squared =
      gravity * horizontal_distance * horizontal_distance / denominator;
  if (!std::isfinite(speed_squared) || speed_squared <= 0.0) {
    return;
  }
  const auto speed = std::sqrt(speed_squared);
  const auto horizontal_speed = speed * cosine;
  if (!std::isfinite(horizontal_speed) || horizontal_speed <= 0.001) {
    return;
  }
  constexpr double retail_lifetime = 60.0;
  const auto flight_time =
      std::min(horizontal_distance / horizontal_speed, retail_lifetime);
  const auto direction_x = horizontal_x / horizontal_distance;
  const auto direction_z = horizontal_z / horizontal_distance;
  const auto vertical_speed = speed * std::sin(launch_angle);
  const auto trajectory_point = [&](double tick) {
    const auto horizontal = horizontal_speed * tick;
    const auto ballistic_y =
        origin.y + vertical_speed * tick - 0.5 * gravity * tick * tick;
    return Vector3{origin.x + direction_x * horizontal, -ballistic_y,
                   origin.z + direction_z * horizontal};
  };
  const auto basis = viewBasis(presentation.camera);
  const auto projection =
      static_cast<double>(std::max(presentation.camera.projection, 1));
  const auto project =
      [&](const Vector3 &point) -> std::optional<std::pair<float, float>> {
    const auto delta = Vector3{point.x - presentation.camera.x,
                               point.y - presentation.camera.y,
                               point.z - presentation.camera.z};
    const auto component = [&delta](const Vector3 &axis) {
      return delta.x * axis.x + delta.y * axis.y + delta.z * axis.z;
    };
    const auto depth = component(basis[2]);
    if (depth <= active_near_clip_depth) {
      return std::nullopt;
    }
    const auto x = static_cast<float>(screen_width * 0.5 + offset_x +
                                      component(basis[0]) * projection / depth);
    const auto y = static_cast<float>(screen_height * 0.5 + offset_y +
                                      component(basis[1]) * projection / depth);
    constexpr auto margin = 48.0F;
    if (x < -margin || x > screen_width + margin || y < -margin ||
        y > screen_height + margin) {
      return std::nullopt;
    }
    return std::pair{x, y};
  };

  const auto draw_projected_segment = [](const std::pair<float, float> &first,
                                         const std::pair<float, float> &second,
                                         float half_width,
                                         std::uint8_t brightness) {
    const auto x0 = first.first;
    const auto y0 = first.second;
    const auto x1 = second.first;
    const auto y1 = second.second;
    const auto dx = x1 - x0;
    const auto dy = y1 - y0;
    const auto length = std::hypot(dx, dy);
    const auto perpendicular_x =
        length > 0.0F ? -dy * half_width / length : half_width;
    const auto perpendicular_y =
        length > 0.0F ? dx * half_width / length : 0.0F;
    const auto extent_x = length > 0.0F ? 0.0F : half_width;
    const auto extent_y = length > 0.0F ? 0.0F : half_width;
    POLY_F4 segment{};
    setPolyF4(&segment);
    setRGB0(&segment, 32U, brightness, 48U);
    setXY4(&segment, x0 - perpendicular_x - extent_x,
           y0 - perpendicular_y - extent_y, x0 + perpendicular_x + extent_x,
           y0 + perpendicular_y + extent_y, x1 - perpendicular_x - extent_x,
           y1 - perpendicular_y - extent_y, x1 + perpendicular_x + extent_x,
           y1 + perpendicular_y + extent_y);
    DrawPrim(&segment);
  };

  constexpr std::size_t stitch_count = 24U;
  constexpr double stitch_fraction = 0.38;
  auto submitted = false;
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);
  GR_SetDepthState(0, 0);
  for (std::size_t index = 0U; index < stitch_count; ++index) {
    const auto cell_begin =
        flight_time * static_cast<double>(index) / stitch_count;
    const auto cell_end =
        flight_time * static_cast<double>(index + 1U) / stitch_count;
    const auto first = project(trajectory_point(cell_begin));
    const auto second = project(
        trajectory_point(std::lerp(cell_begin, cell_end, stitch_fraction)));
    if (!first || !second) {
      continue;
    }
    constexpr auto half_width = 1.0F;
    // PS1 LINE_F2 rasterization guarantees a pixel for zero-length segments
    // and expands line coverage with the low-resolution framebuffer. Modern
    // APIs may discard the point or leave a sub-pixel hairline after scaling.
    // Preserve the retail endpoints and color in a minimum-width screen quad.
    const auto brightness = static_cast<u_char>(176U + (index % 3U) * 24U);
    draw_projected_segment(*first, *second, half_width, brightness);
    submitted = true;
  }

  // The grenade endpoint is a world-space landing marker, not the ordinary
  // screen-facing weapon reticle. Four broken corners are laid in the local
  // XZ floor plane and projected independently, so the marker foreshortens
  // with the ground and indicates the actual end of the ballistic guide.
  const auto landing = trajectory_point(flight_time);
  const auto forward = Vector3{direction_x, 0.0, direction_z};
  const auto right = Vector3{-direction_z, 0.0, direction_x};
  const auto offset_landing = [&](double forward_offset, double right_offset) {
    return Vector3{
        landing.x + forward.x * forward_offset + right.x * right_offset,
        landing.y,
        landing.z + forward.z * forward_offset + right.z * right_offset};
  };
  constexpr double marker_half_size = 62.0;
  constexpr double marker_arm = 25.0;
  for (const auto forward_sign : {-1.0, 1.0}) {
    for (const auto right_sign : {-1.0, 1.0}) {
      const auto corner = offset_landing(forward_sign * marker_half_size,
                                         right_sign * marker_half_size);
      const auto forward_arm =
          offset_landing(forward_sign * (marker_half_size - marker_arm),
                         right_sign * marker_half_size);
      const auto right_arm =
          offset_landing(forward_sign * marker_half_size,
                         right_sign * (marker_half_size - marker_arm));
      const auto projected_corner = project(corner);
      const auto projected_forward_arm = project(forward_arm);
      const auto projected_right_arm = project(right_arm);
      if (projected_corner && projected_forward_arm) {
        draw_projected_segment(*projected_corner, *projected_forward_arm, 1.25F,
                               255U);
        submitted = true;
      }
      if (projected_corner && projected_right_arm) {
        draw_projected_segment(*projected_corner, *projected_right_arm, 1.25F,
                               255U);
        submitted = true;
      }
    }
  }
  if (submitted) {
    DrawSync(0);
  }
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);
}

void drawGameplayHud(const HudTextureAtlas &textures,
                     const game::GameplaySession &gameplay,
                     const game::CameraState &camera,
                     const std::optional<Vector3> &target_anchor,
                     std::span<const WorldCalloutAnchor> world_callouts,
                     const KeyboardMouseBindings &bindings,
                     bool first_person_aim, std::int32_t aim_heading,
                     double presented_weapon_switch_frames,
                     double presentation_delta_seconds, int offset_x,
                     int offset_y, double visibility, double normal_hud_phase,
                     game::CampaignDifficulty difficulty,
                     GameplayHudScratch &scratch) {
  const auto &hud = gameplay.hud();
  const auto &definition = hud.inventory().currentDefinition();
  const auto hud_visibility = std::clamp(visibility, 0.0, 1.0);
  constexpr auto maximum_phase = 12.0;
  const auto retail_hud_phase =
      std::clamp(normal_hud_phase, 0.0, maximum_phase);
  const auto top_offset_y = offset_y;
  const auto radar_offset_x = offset_x;
  const auto radar_offset_y = offset_y;
  const auto weapon_offset_x = offset_x;
  const auto weapon_offset_y = offset_y;
  const auto target_locked = gameplay.targetLocked();
  const auto project_anchor =
      [&camera, offset_x,
       offset_y](const Vector3 &anchor) -> std::optional<ProjectedHudAnchor> {
    const auto rows = viewBasis(camera);
    const auto delta = Vector3{
        anchor.x - camera.x,
        anchor.y - camera.y,
        anchor.z - camera.z,
    };
    const auto view_x =
        rows[0].x * delta.x + rows[0].y * delta.y + rows[0].z * delta.z;
    const auto view_y =
        rows[1].x * delta.x + rows[1].y * delta.y + rows[1].z * delta.z;
    const auto view_z =
        rows[2].x * delta.x + rows[2].y * delta.y + rows[2].z * delta.z;
    if (view_z <= active_near_clip_depth) {
      return std::nullopt;
    }
    const auto projection = static_cast<double>(camera.projection);
    const auto projected_x =
        screen_width / 2.0 + offset_x + projection * view_x / view_z;
    const auto projected_y =
        screen_height / 2.0 + offset_y + projection * view_y / view_z;
    if (!std::isfinite(projected_x) || !std::isfinite(projected_y) ||
        projected_x < static_cast<double>(std::numeric_limits<int>::min()) ||
        projected_x > static_cast<double>(std::numeric_limits<int>::max()) ||
        projected_y < static_cast<double>(std::numeric_limits<int>::min()) ||
        projected_y > static_cast<double>(std::numeric_limits<int>::max())) {
      return std::nullopt;
    }
    return ProjectedHudAnchor{projected_x, projected_y, view_z};
  };

  // HUD is a depth-free native 384x240 pass. Finish any queued world/full-
  // screen primitive first so text, radar blend and weapon layers cannot be
  // replayed with stale PGXP or blend state.
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);

  if (hud_visibility > 0.0) {
    // Retail uses its FONTA/B/C text layer here. ARMOR.TIM is a green legacy
    // image and is not the white in-game label seen above the lavender bar.
    const auto primary_status = game::localizeTextCopy(
        game::originalPrimaryStatusLabel(hud.primaryStatus()));
    drawOriginalHudTextSolid(textures, primary_status, 20 + offset_x,
                             18 + top_offset_y);
    const auto health_color = hud.healthBarColor();
    drawOriginalStatusBar(26, hud.displayedPrimaryTrail(), hud.primaryReveal(),
                          health_color.red, health_color.green,
                          health_color.blue, offset_x, top_offset_y);
    drawOriginalStatusBar(26, hud.displayedPrimaryBar(), hud.primaryReveal(),
                          150U, 150U, 255U, offset_x, top_offset_y);

    if (hud.dangerReveal() != 0U) {
      const auto danger = game::localizeTextCopy("DANGER");
      drawOriginalHudTextSolid(textures, danger, 20 + offset_x,
                               33 + top_offset_y);
      auto red = std::uint8_t{255U};
      auto green = std::uint8_t{100U};
      auto blue = std::uint8_t{100U};
      if (hud.dangerCritical()) {
        const auto phase = static_cast<double>(hud.tick() & 15U) *
                           (2.0 * std::numbers::pi / 16.0);
        red = static_cast<std::uint8_t>(
            std::lround((std::cos(phase) + 1.0) * 127.5));
        green = 0U;
        blue = 0U;
      }
      drawOriginalStatusBar(41, hud.displayedDangerBar(), hud.dangerReveal(),
                            red, green, blue, offset_x, top_offset_y);
    }

    if (hud.targetReveal() != 0U) {
      const auto target = game::localizeTextCopy("TARGET");
      drawOriginalHudTextSolid(textures, target, 20 + offset_x,
                               48 + top_offset_y);
      drawOriginalStatusBar(56, hud.displayedTargetBar(), hud.targetReveal(),
                            100U, 255U, 100U, offset_x, top_offset_y);
    }

    if (difficulty == game::CampaignDifficulty::agent) {
      const auto meters = game::makeAgentMissionHudMeters(
          gameplay.missionIndex(), agentMissionHudState(gameplay));
      std::size_t row = 0U;
      for (const auto &meter : meters.entries()) {
        drawAgentMissionHudMeter(textures, meter, row++, offset_x,
                                 top_offset_y);
      }
    }

    drawOriginalStatusScale(offset_x, top_offset_y, retail_hud_phase);

    // Commit the status layer before the radar switches to average blending.
    // This also makes the ARMOR/HEALTH label independent of radar visibility.
    DrawSync(0);
    drawOriginalRadar(gameplay,
                      game::originalRadarPresentationGeometry(retail_hud_phase),
                      aim_heading, radar_offset_x, radar_offset_y);
    drawOriginalWeaponMenu(textures, hud, weapon_offset_x, weapon_offset_y);

    // FUN_800410d0 moves all 23 weapon sprites from x=404 to x=133 and the
    // ammunition TEXT from x=389 to x=118 using this exact integer division.
    const auto reveal_slide = 271.0 - sf::platform::retailHudInterpolatedExtent(
                                          retail_hud_phase, 271);
    // The authoritative countdown advances at the retail 20 Hz. Its native
    // slide is presentation-only, so retain fractional frames and sub-pixel
    // coordinates between guest updates instead of snapping by five pixels.
    const auto switch_slide =
        std::clamp(
            presented_weapon_switch_frames, 0.0,
            static_cast<double>(game::GameplayHud::weapon_switch_duration)) *
        98.0 / static_cast<double>(game::GameplayHud::weapon_switch_duration);
    const auto horizontal_slide =
        static_cast<float>(static_cast<double>(reveal_slide) + switch_slide);
    const auto icon_layers = definition.icon.layers();
    if (!icon_layers.empty()) {
      std::array<int, game::maximum_weapon_icon_layers> widths{};
      std::array<int, game::maximum_weapon_icon_layers> heights{};
      for (std::size_t index = 0; index < icon_layers.size(); ++index) {
        const auto &image = textures.image(icon_layers[index]);
        widths[index] = static_cast<int>(image.displayWidth());
        heights[index] = static_cast<int>(image.displayHeight());
      }

      // Native SPRT x/y values are layer centres, not top-left corners.
      // Converting the centre anchor here lifts the silhouette above the
      // ammunition line and restores the intended negative space around it.
      const auto icon_offsets = game::originalWeaponIconOffsets(
          std::span<const int>{widths.data(), icon_layers.size()});
      constexpr float icon_origin_x =
          static_cast<float>(screen_width / 2 + 133);
      constexpr float icon_origin_y =
          static_cast<float>(screen_height / 2 + 91);
      for (std::size_t index = 0; index < icon_layers.size(); ++index) {
        const auto &image = textures.image(icon_layers[index]);
        drawHudSpriteScaled(
            image,
            icon_origin_x + static_cast<float>(weapon_offset_x) +
                horizontal_slide + static_cast<float>(icon_offsets[index]),
            icon_origin_y + static_cast<float>(weapon_offset_y) -
                static_cast<float>(heights[index]) * 0.5F,
            static_cast<float>(widths[index]),
            static_cast<float>(heights[index]), 128U, true);
      }
    }
    if (definition.shows_ammo) {
      const auto &weapon = hud.inventory().currentState();
      const auto ammo = game::originalAmmoText(definition, weapon);
      // FUN_800410d0 supplies a left text origin, not a centre point.
      constexpr float ammo_x = static_cast<float>(screen_width / 2 + 118);
      constexpr float ammo_y = static_cast<float>(screen_height / 2 + 94);
      drawOriginalHudTextSubpixel(textures, ammo,
                                  ammo_x + static_cast<float>(weapon_offset_x) +
                                      horizontal_slide,
                                  ammo_y + static_cast<float>(weapon_offset_y));
    }
  }
  const auto weapon = hud.inventory().current();
  const auto grenade_weapon = weapon == game::WeaponId::fragmentation_grenade ||
                              weapon == game::WeaponId::gas_grenade;
  const auto scoped =
      first_person_aim && (weapon == game::WeaponId::nightvision_rifle ||
                           weapon == game::WeaponId::sniper_rifle);
  const auto scanner_optic =
      first_person_aim && weapon == game::WeaponId::virus_scanner;
  const auto authored_optic = scoped || scanner_optic;
  const auto retail_optic_text =
      first_person_aim && usesRetailEnglishOpticText(weapon);
  const auto reticle_owner = game::aimReticleOwner(
      first_person_aim, target_locked, grenade_weapon, authored_optic);
  const auto reticle_requested = reticle_owner == game::AimReticleOwner::host;
  const auto instant_target_lock = target_locked && !first_person_aim;
  if (reticle_requested != scratch.reticle_was_visible) {
    scratch.reticle_was_visible = reticle_requested;
    scratch.reticle_reveal =
        reticle_requested && !instant_target_lock ? 0.0 : 1.0;
  }
  if (reticle_requested) {
    constexpr double reticle_reveal_seconds = 0.14;
    scratch.reticle_reveal =
        std::min(1.0, scratch.reticle_reveal +
                          std::max(0.0, presentation_delta_seconds) /
                              reticle_reveal_seconds);
  }
  const auto reset_reticle_follow = [&scratch] {
    scratch.reticle_follow_initialized = false;
    scratch.reticle_target.reset();
    scratch.reticle_velocity_x = 0.0;
    scratch.reticle_velocity_y = 0.0;
    scratch.reticle_scale_velocity = 0.0;
  };
  const auto reticle_mode = static_cast<std::uint8_t>(first_person_aim ? 1U
                                                      : target_locked  ? 2U
                                                                       : 0U);
  if (!reticle_requested || scratch.reticle_mode != reticle_mode) {
    reset_reticle_follow();
    scratch.reticle_mode = reticle_mode;
  }
  if (reticle_requested || authored_optic) {
    if (scoped) {
      drawOriginalScope(textures, weapon, aim_heading,
                        gameplay.headshotTargeted(), offset_x, offset_y);
    }

    // Textured weapon/scope primitives change the active texture page.
    // Commit them first, then render the manual-aim marker in an isolated
    // opaque, depth-free pass so it cannot disappear behind a wall batch.
    DrawSync(0);
    GR_SetBlendMode(BM_NONE);
    GR_EnableDepth(0);
    if (reticle_requested) {
      auto center_x = screen_width / 2 + offset_x;
      auto center_y = screen_height / 2 + offset_y;
      const auto default_reticle_scale =
          game::originalAimReticleScale(camera.projection, 0.0);
      auto reticle_scale = default_reticle_scale;
      auto reticle_visible = false;
      auto resolved = std::optional<ProjectedHudAnchor>{};
      auto target_scale = default_reticle_scale;

      if (first_person_aim) {
        auto depth_source = std::optional<ProjectedHudAnchor>{};
        if (const auto &retail_point = gameplay.retailAimPoint()) {
          depth_source = project_anchor(Vector3{
              static_cast<double>(retail_point->x),
              -static_cast<double>(retail_point->y),
              static_cast<double>(retail_point->z),
          });
        }
        if (!depth_source && target_anchor) {
          depth_source = project_anchor(*target_anchor);
        }
        center_y += static_cast<int>(
            std::lround(gameplay.manualAimReticleVerticalOffset()));
        resolved = ProjectedHudAnchor{
            static_cast<double>(center_x), static_cast<double>(center_y),
            depth_source ? depth_source->view_depth : 0.0};
        if (depth_source) {
          target_scale = game::originalAimReticleScale(
              camera.projection, depth_source->view_depth);
        }
      } else if (target_locked && target_anchor) {
        resolved = project_anchor(*target_anchor);
        if (resolved) {
          target_scale = game::originalAimReticleScale(camera.projection,
                                                       resolved->view_depth);
        }
      }

      if (resolved) {
        updateReticleFollow(scratch, gameplay.aimTarget(), *resolved,
                            target_scale, instant_target_lock,
                            presentation_delta_seconds);
        center_x = static_cast<int>(std::lround(scratch.reticle_x));
        center_y = static_cast<int>(std::lround(scratch.reticle_y));
        reticle_scale = scratch.reticle_scale;
        reticle_visible = true;
      }
      if (!reticle_visible) {
        reset_reticle_follow();
      } else {
        drawOriginalAimReticle(center_x, center_y, gameplay.headshotTargeted(),
                               scratch.reticle_reveal * hud_visibility,
                               reticle_scale);
        if (gameplay.headshotTargeted()) {
          const auto callout = std::ranges::find_if(
              world_callouts, [](const WorldCalloutAnchor &candidate) {
                return candidate.headshot;
              });
          drawOriginalHeadshotCallout(textures,
                                      callout == world_callouts.end()
                                          ? std::string_view{}
                                          : callout->text,
                                      center_x, center_y);
        }
      }
    }
    if (scoped && gameplay.headshotTargeted()) {
      auto center_x = screen_width / 2 + offset_x;
      auto center_y = screen_height / 2 + offset_y;
      if (target_anchor) {
        if (const auto projected = project_anchor(*target_anchor)) {
          center_x = static_cast<int>(std::lround(projected->x));
          center_y = static_cast<int>(std::lround(projected->y));
        }
      }
      const auto callout = std::ranges::find_if(
          world_callouts, [](const WorldCalloutAnchor &candidate) {
            return candidate.headshot;
          });
      drawOriginalHeadshotCallout(
          textures,
          callout == world_callouts.end() ? std::string_view{} : callout->text,
          center_x, center_y);
    }
  }

  for (auto &state : scratch.world_callout_reveals) {
    state.present = false;
  }
  for (const auto &callout : world_callouts) {
    if (callout.headshot) {
      continue;
    }
    const auto localized = game::localizeTextCopy(callout.text);
    auto state = std::ranges::find_if(
        scratch.world_callout_reveals,
        [&callout, &localized](const WorldCalloutRevealState &candidate) {
          return candidate.object == callout.object &&
                 candidate.text == localized;
        });
    if (state == scratch.world_callout_reveals.end()) {
      scratch.world_callout_reveals.push_back(WorldCalloutRevealState{
          .object = callout.object,
          .text = localized,
      });
      state = std::prev(scratch.world_callout_reveals.end());
    }
    state->present = true;
    state->point = callout.point;
    constexpr double world_callout_glyphs_per_second = 20.0;
    state->visible_glyphs = std::min(
        static_cast<double>(originalHudDrawableGlyphCount(state->text)),
        state->visible_glyphs + std::max(0.0, presentation_delta_seconds) *
                                    world_callout_glyphs_per_second);
  }

  for (auto &state : scratch.world_callout_reveals) {
    state.opacity = game::advanceWorldCalloutOpacity(
        state.opacity, state.present, presentation_delta_seconds);
    if (state.opacity <= 0.0) {
      continue;
    }
    const auto visible_text = originalHudTextGlyphPrefix(
        state.text, static_cast<std::size_t>(state.visible_glyphs));
    if (const auto projected = project_anchor(state.point)) {
      drawOriginalWorldCallout(
          textures, visible_text, static_cast<int>(std::lround(projected->x)),
          static_cast<int>(std::lround(projected->y)),
          game::worldCalloutBrightness(state.opacity * hud_visibility));
    }
  }
  std::erase_if(scratch.world_callout_reveals,
                [](const WorldCalloutRevealState &state) {
                  return !state.present && state.opacity <= 0.0;
                });

  // All locales retain the guest's lifetime, type-on colors and slide timing.
  // Native layout changes only wording/reflow and maps the live retail glyph
  // count onto the presented string, so English and translated text reveal
  // identically.
  const auto messages = gameplay.legacyUiMessages();
  const auto status_backdrop =
      std::ranges::find_if(messages, [](const auto &message) {
        return message.channel == game::LegacyUiMessageChannel::status &&
               message.backdrop.has_value();
      });
  auto &message_layouts = scratch.message_layouts;
  message_layouts.clear();
  message_layouts.reserve(messages.size());
  auto &retail_scope_messages = scratch.retail_scope_messages;
  retail_scope_messages.clear();
  retail_scope_messages.reserve(messages.size());
  for (const auto &message : messages) {
    const auto scope_message =
        !message.force_gameplay_layout &&
        isRetailScopeMessage(retail_optic_text, message.channel,
                             message.backdrop.has_value(),
                             message.glyphs.size());
    const auto retail_scope_message =
        !message.force_gameplay_layout &&
        useRetailEnglishScopeFont(
            retail_optic_text, game::russianLanguageActive(), message.channel,
            message.backdrop.has_value(), message.glyphs.size());
    retail_scope_messages.push_back(retail_scope_message);
    // The guest packet already contains its authored type-on, slide and fade
    // position. Applying the normal-HUD transition here would animate it a
    // second time when a skipped radio call restores the HUD.
    message_layouts.push_back(
        scope_message ? std::nullopt
                      : buildGameplayMessageLayout(message, offset_x, offset_y,
                                                   bindings, difficulty));
  }
  const auto russian_language = game::russianLanguageActive();
  if (russian_language) {
    arrangeStatusMessages(messages, message_layouts, scratch.status_indices);
  }

  auto adaptive_status_backdrop = false;
  if (status_backdrop != messages.end()) {
    adaptive_status_backdrop =
        drawStatusBackdrop(messages, message_layouts,
                           *status_backdrop->backdrop, offset_x, offset_y);
  }
  for (const auto &message : messages) {
    if (!message.backdrop ||
        (adaptive_status_backdrop &&
         message.channel == game::LegacyUiMessageChannel::status)) {
      continue;
    }
    drawRetailUiBackdrop(*message.backdrop, offset_x, offset_y);
  }
  if (const auto &timer = gameplay.legacyUiTimer()) {
    drawRetailUiGlyphs(textures, timer->glyphs, offset_x, offset_y);
  }
  for (auto index = std::size_t{}; index < messages.size(); ++index) {
    const auto &message = messages[index];
    if (retail_scope_messages[index]) {
      // Scope labels are completed retail FONT/TEXT packets. Draw their exact
      // original UVs through the untouched SCUS font atlas instead of the ViT
      // Cyrillic replacement, preserving wording, reveal animation and layout.
      drawRetailUiGlyphs(textures, message.glyphs, offset_x, offset_y,
                         textures.retailEnglishFont());
    } else if (message_layouts[index]) {
      drawGameplayMessage(textures, message, *message_layouts[index]);
    } else if (!drawBoundKeyboardMousePrompt(textures, message, bindings,
                                             offset_x, offset_y)) {
      drawRetailUiGlyphs(textures, message.glyphs, offset_x, offset_y);
    }
  }

  // DrawPrim only queues immediate primitives in PsyCross. Flush the HUD
  // before the scene is presented; ordering-table draws were already flushed.
  DrawSync(0);
  GR_EnableDepth(1);
}

void drawPresentationFrameMeter(const HudTextureAtlas &textures,
                                std::string_view text) {
  if (text.empty()) {
    return;
  }
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);
  GR_SetDepthState(0, 0);
  constexpr auto top = 4;
  constexpr auto right_margin = 4;
  const auto width = game::originalHudTextWidth(text);
  const auto left = screen_width - right_margin - width;
  drawSolidRect(left - 2, top - 2, width + 4, 11, 0U, 0U, 0U);
  drawOriginalHudTextSolid(textures, text, left, top, 255U);
  // The regular HUD has already flushed its immediate PsyCross primitives.
  // Submit this late diagnostic layer before EndScene instead of silently
  // leaving its rectangle and glyphs in the next frame's command queue.
  DrawSync(0);
  GR_EnableDepth(1);
  GR_SetDepthState(1, 1);
}

void drawMissionCompleteOverlay(const HudTextureAtlas &textures) {
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);
  drawSolidRect(0, 0, screen_width, screen_height, 0U, 0U, 0U);
  const auto complete = game::localizeText("MISSION COMPLETE");
  drawOriginalHudTextSolid(
      textures, complete,
      (screen_width - game::originalHudTextWidth(complete)) / 2,
      screen_height / 2 - 5, 255U);
  DrawSync(0);
  GR_EnableDepth(1);
}
