struct PauseRgb {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
};

PauseRgb pauseColor(game::PauseColorRole role) noexcept {
  switch (role) {
  case game::PauseColorRole::background:
    return {0U, 0U, 0U};
  // Retail MENU.OVL uses these RGB triplets for normal/highlighted text.
  case game::PauseColorRole::normal:
    return {110U, 130U, 200U};
  case game::PauseColorRole::selected:
    return {180U, 180U, 240U};
  case game::PauseColorRole::muted:
    return {60U, 90U, 60U};
  case game::PauseColorRole::accent:
    return {110U, 130U, 200U};
  case game::PauseColorRole::completed:
    return {140U, 240U, 140U};
  case game::PauseColorRole::failed:
    return {240U, 140U, 140U};
  case game::PauseColorRole::warning:
    return {240U, 92U, 92U};
  case game::PauseColorRole::map_highlight:
    return {255U, 142U, 24U};
  }
  return {110U, 130U, 200U};
}

PauseRgb pauseMapHighlightColor(PauseRgb color) noexcept {
  // MENU.OVL's objective light is periodic, but tying its hard on/off state to
  // host frames made it flash faster at 60/120/240 FPS. Drive one gentle
  // triangular glow from wall time instead: the marker never disappears and
  // its matching list entry uses exactly the same phase.
  constexpr std::uint64_t step_ms = 50U;
  constexpr std::uint64_t period_steps = 32U;
  constexpr std::uint64_t half_period = period_steps / 2U;
  const auto step = (SDL_GetTicks64() / step_ms) % period_steps;
  const auto triangle = step <= half_period ? step : period_steps - step;
  const auto intensity = 196U + static_cast<unsigned int>(triangle) * 59U /
                                    static_cast<unsigned int>(half_period);
  const auto modulate = [intensity](std::uint8_t channel) {
    return static_cast<std::uint8_t>(static_cast<unsigned int>(channel) *
                                     intensity / 255U);
  };
  return {modulate(color.red), modulate(color.green), modulate(color.blue)};
}

void drawPauseFontRegion(const assets::TimImage &image, int source_x,
                         int source_y, int source_width, int source_height,
                         int destination_x, int destination_y,
                         int destination_width, int destination_height,
                         PauseRgb color) {
  const auto &pixels = image.pixels();
  const auto resident_x = hudResidentX(pixels.x);
  const auto page_x =
      static_cast<int>(resident_x & static_cast<std::uint16_t>(~63U));
  const auto page_y =
      static_cast<int>(pixels.y & static_cast<std::uint16_t>(~255U));
  const auto pixels_per_word =
      image.mode() == assets::TimPixelMode::indexed4   ? 4
      : image.mode() == assets::TimPixelMode::indexed8 ? 2
                                                       : 1;
  const auto u0 =
      (static_cast<int>(resident_x) - page_x) * pixels_per_word + source_x;
  const auto v0 = static_cast<int>(pixels.y) - page_y + source_y;
  const auto texture_page =
      GetTPage(texturePageMode(image.mode()), 0, page_x, page_y);

  POLY_FT4 polygon{};
  setPolyFT4(&polygon);
  polygon.tpage = texture_page;
  polygon.clut = GetClut(hud_resident_clut_x, hud_resident_clut_y);
  setRGB0(&polygon, color.red, color.green, color.blue);
  setXY4(&polygon, static_cast<float>(destination_x),
         static_cast<float>(destination_y),
         static_cast<float>(destination_x + destination_width),
         static_cast<float>(destination_y), static_cast<float>(destination_x),
         static_cast<float>(destination_y + destination_height),
         static_cast<float>(destination_x + destination_width),
         static_cast<float>(destination_y + destination_height));
  setUV4(&polygon, static_cast<u_char>(u0), static_cast<u_char>(v0),
         static_cast<u_char>(u0 + source_width), static_cast<u_char>(v0),
         static_cast<u_char>(u0), static_cast<u_char>(v0 + source_height),
         static_cast<u_char>(u0 + source_width),
         static_cast<u_char>(v0 + source_height));
  DrawPrim(&polygon);
}

int menuLineDrawExtent(std::string_view text, int scale,
                       float horizontal_scale = 1.0F) noexcept {
  const auto scaled = [scale, horizontal_scale](int value) {
    return std::max(
        1, static_cast<int>(std::lround(value * scale * horizontal_scale)));
  };
  auto x = 0;
  auto right = 0;
  for (const auto source : text) {
    const auto raw = static_cast<unsigned char>(source);
    if (source == ' ') {
      right = std::max(right, x + scaled(4));
      x += scaled(4);
      continue;
    }
    if (raw == 1U || raw == 2U) {
      right = std::max(right, x + scaled(8));
      x += scaled(10);
      continue;
    }
    const auto glyph = originalHudGlyph(static_cast<char>(raw));
    if (!glyph) {
      continue;
    }
    right = std::max(right, x + scaled(static_cast<int>(glyph->width)));
    x += scaled(originalHudGlyphAdvance(*glyph));
  }
  return right;
}

float fittedMenuLineScale(std::string_view text, int scale,
                          int available_width) noexcept {
  // Use the exact same rounded glyph extents as drawMenuLine().  The former
  // advance-sum estimate could be narrower than the actual final quad at a
  // fractional scale, so drawMenuLine() discarded the last one or two glyphs
  // (notably the trailing `ТЬ` in the Russian pause hint).
  constexpr auto edge_guard = 2;
  const auto target = std::max(1, available_width - edge_guard);
  if (menuLineDrawExtent(text, scale) <= target) {
    return 1.0F;
  }
  auto lower = 0.05F;
  auto upper = 1.0F;
  for (auto iteration = 0; iteration < 16; ++iteration) {
    const auto candidate = (lower + upper) * 0.5F;
    if (menuLineDrawExtent(text, scale, candidate) <= target) {
      lower = candidate;
    } else {
      upper = candidate;
    }
  }
  return lower;
}

void drawMenuLine(std::string_view text, int x, int y, int scale, int maximum_x,
                  PauseRgb color, const HudTextureAtlas &interface_textures,
                  float horizontal_scale = 1.0F, float vertical_scale = 1.0F) {
  // FONTA/B/C occupy one continuous retail texture page. The SCUS glyph
  // metadata above therefore addresses all three sheets through FONTA's
  // page instead of treating them as fixed 8x8 ASCII cells.
  const auto &font_page = interface_textures.image("FONTA.TIM");
  const auto &symbols = interface_textures.image("SYMBOL.TIM");
  const auto *native_font = interface_textures.nativeFont();
  const ScopedPsyCrossFontTexture font_binding{native_font};
  const auto scaled = [scale, horizontal_scale](int value) {
    return std::max(
        1, static_cast<int>(std::lround(value * scale * horizontal_scale)));
  };
  const auto glyph_height =
      std::max(1, static_cast<int>(std::lround(8 * scale * vertical_scale)));
  const auto symbol_height =
      std::max(1, static_cast<int>(std::lround(9 * scale * vertical_scale)));
  for (const auto source : text) {
    const auto raw = static_cast<unsigned char>(source);
    if (source == ' ') {
      if (x + scaled(4) > maximum_x) {
        return;
      }
      x += scaled(4);
      continue;
    }
    const auto value = raw;
    if (value == 1U || value == 2U) {
      // Original CROSS / TRIANGLE cells in SYMBOL.TIM.
      constexpr auto symbol_x = 14;
      const auto symbol_y = value == 1U ? 51 : 60;
      if (x + scaled(8) > maximum_x) {
        return;
      }
      if (native_font != nullptr) {
        PsyCrossFontTexture::restoreVram();
      }
      drawPauseFontRegion(symbols, symbol_x, symbol_y, 11, 9, x,
                          y - std::max(1, glyph_height / 8), scaled(8),
                          symbol_height, color);
      if (native_font != nullptr) {
        native_font->bind();
      }
      x += scaled(10);
      continue;
    }
    const auto glyph = originalHudGlyph(static_cast<char>(value));
    if (!glyph) {
      continue;
    }
    if (x + scaled(static_cast<int>(glyph->width)) > maximum_x) {
      return;
    }
    drawPauseFontRegion(font_page, glyph->u, glyph->v, glyph->width, 8, x, y,
                        scaled(static_cast<int>(glyph->width)), glyph_height,
                        color);
    x += scaled(originalHudGlyphAdvance(*glyph));
  }
}

int menuLineWidth(std::string_view text, int scale,
                  float horizontal_scale = 1.0F) noexcept {
  return menuLineDrawExtent(text, scale, horizontal_scale);
}

void drawMenuText(std::string_view source, const game::PauseRect &bounds,
                  int scale, PauseRgb color, game::PauseTextAlignment alignment,
                  int line_height, const HudTextureAtlas &interface_textures,
                  const InputPromptBindings &bindings,
                  InputPromptTokenActions tokens = {}) {
  const auto localized = game::localizeTextCopy(source);
  const auto text = inputHintText(localized, bindings, tokens);
  const auto retail_line_height = std::max(line_height, 1);
  // Eight pixels is the physical glyph height. Inter-line leading may shrink
  // from the retail default when a translated paragraph needs the room, but
  // glyphs themselves are never vertically filtered or overlapped.
  const auto maximum_lines =
      std::max(1, static_cast<int>(bounds.height) / (8 * scale));
  if (maximum_lines == 1) {
    // A one-line hint has no vertical room for wrapping, but it must not lose
    // authored text if localization supplied an explicit break. Flatten the
    // break and fit the complete semantic string instead of truncating it.
    auto line_text = text;
    std::ranges::replace(line_text, '\n', ' ');
    const auto natural_width = menuLineWidth(line_text, scale);
    // A one-line retail label is never clipped. Localized labels may be
    // proportionally condensed, but every authored glyph remains visible.
    const auto horizontal_scale =
        natural_width == 0
            ? 1.0F
            : fittedMenuLineScale(line_text, scale, bounds.width);
    const auto line_width = menuLineWidth(line_text, scale, horizontal_scale);
    auto x = static_cast<int>(bounds.x);
    if (alignment == game::PauseTextAlignment::center) {
      x += (static_cast<int>(bounds.width) - line_width) / 2;
    } else if (alignment == game::PauseTextAlignment::right) {
      x += static_cast<int>(bounds.width) - line_width;
    }
    x = std::max(x, static_cast<int>(bounds.x));
    drawMenuLine(line_text, x, bounds.y, scale,
                 static_cast<int>(bounds.x + bounds.width), color,
                 interface_textures, horizontal_scale);
    return;
  }
  const auto wrap = [&](float horizontal_scale) {
    std::vector<std::string_view> lines;
    auto cursor = std::size_t{};
    while (cursor < text.size()) {
      while (cursor < text.size() && text[cursor] == ' ') {
        ++cursor;
      }
      if (cursor >= text.size()) {
        break;
      }
      auto end = cursor;
      auto last_space = std::string::npos;
      while (end < text.size() && text[end] != '\n') {
        if (text[end] == ' ') {
          last_space = end;
        }
        const auto candidate =
            std::string_view{text}.substr(cursor, end - cursor + 1U);
        if (menuLineWidth(candidate, scale, horizontal_scale) > bounds.width) {
          if (last_space != std::string::npos && last_space > cursor) {
            end = last_space;
          }
          break;
        }
        ++end;
      }
      if (end == cursor) {
        ++end;
      }
      auto visible_end = end;
      while (visible_end > cursor && text[visible_end - 1U] == ' ') {
        --visible_end;
      }
      lines.push_back(
          std::string_view{text}.substr(cursor, visible_end - cursor));
      cursor = end;
      while (cursor < text.size() &&
             (text[cursor] == ' ' || text[cursor] == '\n')) {
        ++cursor;
      }
    }
    return lines;
  };

  auto horizontal_scale = 1.0F;
  auto lines = wrap(horizontal_scale);
  for (const auto candidate :
       {0.9F, 0.8F, 0.7F, 0.6F, 0.5F, 0.45F, 0.4F, 0.35F, 0.3F}) {
    if (static_cast<int>(lines.size()) <= maximum_lines) {
      break;
    }
    horizontal_scale = candidate;
    lines = wrap(horizontal_scale);
  }
  // Never discard overflow lines.  The high-resolution PC atlas lets us
  // reduce vertical sampling cleanly when a translation is denser than the
  // original English copy. Authored pagination remains preferable for long
  // briefing pages, but this is a lossless last line of defence for every
  // locale and every menu panel.
  const auto natural_height =
      static_cast<int>(lines.size()) * retail_line_height * scale;
  const auto vertical_scale =
      natural_height <= bounds.height || natural_height == 0
          ? 1.0F
          : static_cast<float>(bounds.height) /
                static_cast<float>(natural_height);
  const auto fitted_line_height =
      std::max(1, static_cast<int>(
                      std::floor(retail_line_height * scale * vertical_scale)));
  for (std::size_t line = 0; line < lines.size(); ++line) {
    const auto line_text = lines[line];
    const auto line_width = menuLineWidth(line_text, scale, horizontal_scale);
    auto x = static_cast<int>(bounds.x);
    if (alignment == game::PauseTextAlignment::center) {
      x += (static_cast<int>(bounds.width) - line_width) / 2;
    } else if (alignment == game::PauseTextAlignment::right) {
      x += static_cast<int>(bounds.width) - line_width;
    }
    x = std::max(x, static_cast<int>(bounds.x));
    drawMenuLine(line_text, x,
                 static_cast<int>(bounds.y) +
                     static_cast<int>(line) * fitted_line_height,
                 scale, static_cast<int>(bounds.x + bounds.width), color,
                 interface_textures, horizontal_scale, vertical_scale);
  }
}

void drawBorderedRect(const game::PauseRect &bounds, PauseRgb fill,
                      PauseRgb border) {
  drawSolidRect(bounds.x, bounds.y, bounds.width, bounds.height, fill.red,
                fill.green, fill.blue);
  drawSolidRect(bounds.x, bounds.y, bounds.width, 1, border.red, border.green,
                border.blue);
  drawSolidRect(bounds.x, bounds.y + bounds.height - 1, bounds.width, 1,
                border.red, border.green, border.blue);
  drawSolidRect(bounds.x, bounds.y, 1, bounds.height, border.red, border.green,
                border.blue);
  drawSolidRect(bounds.x + bounds.width - 1, bounds.y, 1, bounds.height,
                border.red, border.green, border.blue);
}

void drawAcdLine(int x0, int y0, int x1, int y1, PauseRgb color,
                 bool semi_transparent = false) {
  LINE_F2 line{};
  setLineF2(&line);
  setSemiTrans(&line, semi_transparent ? 1 : 0);
  setRGB0(&line, color.red, color.green, color.blue);
  setXY2(&line, static_cast<float>(x0), static_cast<float>(y0),
         static_cast<float>(x1), static_cast<float>(y1));
  DrawPrim(&line);
}

void drawAcdTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                     PauseRgb color) {
  POLY_F3 triangle{};
  setPolyF3(&triangle);
  setRGB0(&triangle, color.red, color.green, color.blue);
  setXY3(&triangle, static_cast<float>(x0), static_cast<float>(y0),
         static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(x2),
         static_cast<float>(y2));
  DrawPrim(&triangle);
}

void drawAcdOutline(const game::PauseRect &bounds, PauseRgb color) {
  drawAcdLine(bounds.x, bounds.y, bounds.x + bounds.width, bounds.y, color);
  drawAcdLine(bounds.x, bounds.y + bounds.height, bounds.x + bounds.width,
              bounds.y + bounds.height, color);
  drawAcdLine(bounds.x, bounds.y, bounds.x, bounds.y + bounds.height, color);
  drawAcdLine(bounds.x + bounds.width, bounds.y, bounds.x + bounds.width,
              bounds.y + bounds.height, color);
}

std::optional<game::PauseRect>
animatedSectionSelection(const game::PauseMenu &menu) noexcept {
  const auto &transition = menu.transition();
  if (!transition.active() ||
      transition.kind != game::PauseTransitionKind::section_selection ||
      transition.duration == 0U) {
    return std::nullopt;
  }
  const auto from =
      game::PauseAcdLayout::sectionSelection(transition.from_selection);
  const auto to =
      game::PauseAcdLayout::sectionSelection(transition.to_selection);
  const auto frame = std::min<int>(transition.frame + 1, transition.duration);
  const auto interpolate =
      [frame, duration = static_cast<int>(transition.duration)](int first,
                                                                int second) {
        return first + (second - first) * frame / duration;
      };
  return game::PauseRect{
      static_cast<std::int16_t>(interpolate(from.x, to.x)),
      static_cast<std::int16_t>(interpolate(from.y, to.y)),
      static_cast<std::int16_t>(interpolate(from.width, to.width)),
      static_cast<std::int16_t>(interpolate(from.height, to.height)),
  };
}

std::optional<game::PauseRect> animatedItemSelection(
    const game::PauseMenu &menu,
    const std::vector<game::PauseRenderCommand> &commands) noexcept {
  const auto &transition = menu.transition();
  if (!transition.active() ||
      transition.kind != game::PauseTransitionKind::item_selection ||
      transition.duration == 0U) {
    return std::nullopt;
  }
  const auto find_item = [&commands](std::size_t id) {
    return std::ranges::find_if(commands, [id](const auto &command) {
      return command.kind == game::PauseRenderKind::menu_item &&
             command.panel != game::PausePanelRole::right_sections &&
             command.id == id;
    });
  };
  const auto from = find_item(transition.from_selection);
  const auto to = find_item(transition.to_selection);
  if (from == commands.end() || to == commands.end()) {
    return std::nullopt;
  }
  const auto frame = std::min<int>(transition.frame + 1, transition.duration);
  const auto interpolate =
      [frame, duration = static_cast<int>(transition.duration)](int first,
                                                                int second) {
        return first + (second - first) * frame / duration;
      };
  return game::PauseRect{
      static_cast<std::int16_t>(interpolate(from->bounds.x, to->bounds.x)),
      static_cast<std::int16_t>(interpolate(from->bounds.y, to->bounds.y)),
      static_cast<std::int16_t>(
          interpolate(from->bounds.width, to->bounds.width)),
      static_cast<std::int16_t>(
          interpolate(from->bounds.height, to->bounds.height)),
  };
}

game::PauseRect
animatedScreenCommandBounds(const game::PauseMenu &menu,
                            const game::PauseRenderCommand &command) noexcept {
  const auto &transition = menu.transition();
  if (!transition.active() ||
      transition.kind != game::PauseTransitionKind::screen_change ||
      transition.duration == 0U ||
      command.kind == game::PauseRenderKind::panel ||
      command.kind == game::PauseRenderKind::dim_background ||
      command.panel == game::PausePanelRole::right_sections) {
    return command.bounds;
  }

  const auto frame = std::min<int>(transition.frame + 1, transition.duration);
  const auto remaining = static_cast<int>(transition.duration) - frame;
  auto bounds = command.bounds;
  switch (command.panel) {
  case game::PausePanelRole::left_content:
    bounds.x = static_cast<std::int16_t>(bounds.x - (24 * remaining) /
                                                        transition.duration);
    break;
  case game::PausePanelRole::right_information:
    bounds.x = static_cast<std::int16_t>(bounds.x + (16 * remaining) /
                                                        transition.duration);
    break;
  case game::PausePanelRole::hint:
    bounds.y = static_cast<std::int16_t>(bounds.y +
                                         (8 * remaining) / transition.duration);
    break;
  case game::PausePanelRole::none:
  case game::PausePanelRole::right_sections:
    break;
  }
  return bounds;
}

struct AcdCrossSection {
  int ax{};
  int ay{};
  int bx{};
  int by{};
};

struct AcdLineSegment {
  int x0{};
  int y0{};
  int x1{};
  int y1{};
};

constexpr std::array<AcdCrossSection, 31> acd_frame_sections{{
    {231, 30, 227, 34},   {220, 19, 217, 24},   {29, 19, 42, 24},
    {17, 31, 35, 31},     {17, 84, 35, 96},     {29, 96, 44, 105},
    {29, 165, 44, 175},   {17, 177, 44, 183},   {17, 204, 44, 203},
    {27, 214, 50, 209},   {134, 214, 137, 209}, {143, 223, 154, 209},
    {217, 223, 219, 209}, {233, 207, 227, 201}, {233, 142, 227, 140},
    {233, 108, 227, 110}, {233, 40, 227, 34},   {239, 34, 231, 30},
    {307, 34, 301, 30},   {322, 34, 309, 22},   {344, 34, 343, 22},
    {349, 39, 355, 34},   {349, 75, 355, 71},   {349, 83, 367, 83},
    {349, 108, 367, 116}, {341, 116, 355, 128}, {313, 116, 311, 128},
    {303, 116, 303, 120}, {258, 116, 255, 120}, {241, 116, 241, 134},
    {233, 108, 233, 142},
}};

constexpr std::array<AcdLineSegment, 26> acd_horizontal_grid{{
    {36, 31, 223, 31},    {36, 41, 226, 41},    {36, 51, 226, 51},
    {36, 61, 226, 61},    {36, 71, 226, 71},    {36, 81, 226, 81},
    {36, 91, 226, 91},    {41, 101, 226, 101},  {45, 111, 226, 111},
    {45, 121, 226, 121},  {45, 131, 226, 131},  {45, 141, 226, 141},
    {45, 151, 226, 151},  {45, 161, 226, 161},  {45, 171, 226, 171},
    {45, 181, 226, 181},  {45, 191, 226, 191},  {45, 201, 226, 201},
    {235, 110, 346, 110}, {234, 100, 348, 100}, {234, 90, 348, 90},
    {234, 80, 348, 80},   {234, 70, 348, 70},   {234, 60, 348, 60},
    {234, 50, 348, 50},   {235, 40, 348, 40},
}};

constexpr std::array<AcdLineSegment, 26> acd_vertical_grid{{
    {41, 100, 41, 26},   {53, 208, 53, 25},   {65, 208, 65, 25},
    {77, 208, 77, 25},   {89, 208, 89, 25},   {101, 208, 101, 25},
    {113, 208, 113, 25}, {125, 208, 125, 25}, {137, 208, 137, 25},
    {149, 208, 149, 25}, {161, 208, 161, 25}, {173, 208, 173, 25},
    {185, 208, 185, 25}, {197, 208, 197, 25}, {209, 208, 209, 25},
    {221, 206, 221, 29}, {233, 110, 233, 38}, {245, 115, 245, 35},
    {260, 115, 260, 35}, {272, 115, 272, 35}, {284, 115, 284, 35},
    {296, 115, 296, 35}, {308, 115, 308, 35}, {320, 115, 320, 35},
    {332, 115, 332, 35}, {344, 111, 344, 36},
}};

constexpr std::array<AcdLineSegment, 5> acd_finishing_lines{{
    {307, 124, 251, 124},
    {227, 26, 305, 26},
    {138, 218, 23, 218},
    {23, 210, 23, 218},
    {23, 171, 23, 90},
}};

constexpr std::uint64_t acd_reveal_duration = 12U;

void drawAcdStrip(const AcdCrossSection &first, const AcdCrossSection &second,
                  PauseRgb color) {
  POLY_F4 polygon{};
  setPolyF4(&polygon);
  setSemiTrans(&polygon, 1);
  setRGB0(&polygon, color.red, color.green, color.blue);
  setXY4(&polygon, static_cast<float>(first.ax), static_cast<float>(first.ay),
         static_cast<float>(first.bx), static_cast<float>(first.by),
         static_cast<float>(second.ax), static_cast<float>(second.ay),
         static_cast<float>(second.bx), static_cast<float>(second.by));
  DrawPrim(&polygon);
}

void drawAcdSegment(const AcdLineSegment &segment, PauseRgb color,
                    bool semi_transparent) {
  drawAcdLine(segment.x0, segment.y0, segment.x1, segment.y1, color,
              semi_transparent);
}

bool drawOriginalAcdFrame(std::uint64_t animation_tick) {
  drawSolidRect(0, 0, screen_width, screen_height, 0U, 0U, 0U);
  DrawSync(0);

  const auto state = std::min(animation_tick, acd_reveal_duration);
  const auto strip_count =
      static_cast<std::size_t>((30U * state) / acd_reveal_duration);
  const auto grid_count =
      static_cast<std::size_t>((26U * state) / acd_reveal_duration);
  constexpr auto strip_color = PauseRgb{60U, 70U, 160U};
  constexpr auto strip_lead = PauseRgb{120U, 180U, 255U};
  constexpr auto outline_color = PauseRgb{90U, 100U, 180U};
  constexpr auto outline_lead = PauseRgb{120U, 180U, 240U};
  constexpr auto grid_color = PauseRgb{70U, 60U, 140U};

  GR_SetBlendMode(BM_AVERAGE);
  DR_TPAGE blend_page{};
  SetDrawTPage(&blend_page, 1, 0, GetTPage(0, 0, 0, 0));
  DrawPrim(&blend_page);
  for (std::size_t index = 0; index < strip_count; ++index) {
    const auto leading =
        state < acd_reveal_duration && index + 1U == strip_count;
    drawAcdStrip(acd_frame_sections[index], acd_frame_sections[index + 1U],
                 leading ? strip_lead : strip_color);
  }
  for (std::size_t index = 0; index < grid_count; ++index) {
    const auto leading =
        state < acd_reveal_duration && index + 1U == grid_count;
    const auto color = leading ? outline_lead : grid_color;
    drawAcdSegment(acd_horizontal_grid[index], color, true);
    drawAcdSegment(acd_vertical_grid[index], color, true);
  }
  DrawSync(0);

  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);
  for (std::size_t index = 0; index < strip_count; ++index) {
    const auto leading =
        state < acd_reveal_duration && index + 1U == strip_count;
    const auto color = leading ? outline_lead : outline_color;
    const auto &first = acd_frame_sections[index];
    const auto &second = acd_frame_sections[index + 1U];
    if (index != 14U) {
      drawAcdLine(first.ax, first.ay, second.ax, second.ay, color);
    }
    if (index != 16U) {
      drawAcdLine(first.bx, first.by, second.bx, second.by, color);
    }
  }
  if (state == acd_reveal_duration) {
    for (const auto &line : acd_finishing_lines) {
      drawAcdSegment(line, outline_color, false);
    }
  }
  DrawSync(0);
  return state == acd_reveal_duration;
}

std::optional<game::PauseRect>
drawPauseWeaponIcon(const HudTextureAtlas &textures, std::uint32_t item,
                    const game::PauseRect &bounds) {
  if (item >= game::weapon_slot_count) {
    return std::nullopt;
  }
  const auto weapon = static_cast<game::WeaponId>(item);
  // CHNGUN is vehicle-only retail equipment with no inventory icon. Its
  // dedicated TIM exists solely to keep the generic floor-drop bridge visible.
  if (weapon == game::WeaponId::chopper_gun) {
    return std::nullopt;
  }
  const auto layers =
      game::droppedItemIconLayers(static_cast<std::uint16_t>(weapon));
  if (layers.empty()) {
    return std::nullopt;
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
  auto group_height = heights[0];
  for (std::size_t layer = 1U; layer < layers.size(); ++layer) {
    group_left = std::min(group_left, offsets[layer]);
    group_right = std::max(group_right, offsets[layer] + widths[layer]);
    group_height = std::max(group_height, heights[layer]);
  }
  const auto group_width = std::max(group_right - group_left, 1);
  group_height = std::max(group_height, 1);
  const auto scale = std::min(
      static_cast<float>(bounds.width) / static_cast<float>(group_width),
      static_cast<float>(bounds.height) / static_cast<float>(group_height));
  const auto draw_width = static_cast<float>(group_width) * scale;
  const auto draw_height = static_cast<float>(group_height) * scale;
  const auto left = static_cast<float>(bounds.x) +
                    (static_cast<float>(bounds.width) - draw_width) * 0.5F;
  const auto top = static_cast<float>(bounds.y) +
                   (static_cast<float>(bounds.height) - draw_height) * 0.5F;
  const auto group_center =
      (static_cast<float>(group_left) + static_cast<float>(group_right)) * 0.5F;
  const auto destination_center = left + draw_width * 0.5F;

  textures.restoreWeaponIcon(weapon);
  for (std::size_t layer = 0U; layer < layers.size(); ++layer) {
    const auto &image = textures.image(layers[layer]);
    const auto layer_width = static_cast<float>(widths[layer]) * scale;
    const auto layer_height = static_cast<float>(heights[layer]) * scale;
    drawHudSpriteScaled(
        image,
        destination_center +
            (static_cast<float>(offsets[layer]) - group_center) * scale,
        top + (draw_height - layer_height) * 0.5F, layer_width, layer_height,
        128U, true);
  }
  DrawSync(0);

  return game::PauseRect{
      static_cast<std::int16_t>(std::lround(left)),
      static_cast<std::int16_t>(std::lround(top)),
      static_cast<std::int16_t>(std::lround(draw_width)),
      static_cast<std::int16_t>(std::lround(draw_height)),
  };
}

std::optional<game::PauseRect>
drawPauseTexture(const PauseTextureAtlas &textures, std::string_view name,
                 const game::PauseRect &bounds) {
  const auto *image = textures.image(name);
  if (image == nullptr || !image->clut()) {
    return std::nullopt;
  }
  uploadTimBlock(image->pixels());
  uploadTimBlock(*image->clut());
  DrawSync(0);

  const auto &pixels = image->pixels();
  const auto page_x =
      static_cast<int>(pixels.x & static_cast<std::uint16_t>(~63U));
  const auto page_y =
      static_cast<int>(pixels.y & static_cast<std::uint16_t>(~255U));
  const auto pixels_per_word =
      image->mode() == assets::TimPixelMode::indexed4   ? 4
      : image->mode() == assets::TimPixelMode::indexed8 ? 2
                                                        : 1;
  const auto u0 = (static_cast<int>(pixels.x) - page_x) * pixels_per_word;
  const auto v0 = static_cast<int>(pixels.y) - page_y;
  const auto u1 = std::min(255, u0 + static_cast<int>(image->displayWidth()));
  const auto v1 = std::min(255, v0 + static_cast<int>(image->displayHeight()));
  const auto scale =
      std::min(static_cast<double>(bounds.width) / image->displayWidth(),
               static_cast<double>(bounds.height) / image->displayHeight());
  const auto draw_width =
      std::max(1, static_cast<int>(std::lround(
                      static_cast<double>(image->displayWidth()) * scale)));
  const auto draw_height =
      std::max(1, static_cast<int>(std::lround(
                      static_cast<double>(image->displayHeight()) * scale)));
  const auto draw_bounds = game::PauseRect{
      static_cast<std::int16_t>(bounds.x + (bounds.width - draw_width) / 2),
      static_cast<std::int16_t>(bounds.y + (bounds.height - draw_height) / 2),
      static_cast<std::int16_t>(draw_width),
      static_cast<std::int16_t>(draw_height),
  };

  const auto texture_page =
      GetTPage(texturePageMode(image->mode()), 0, page_x, page_y);
  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, texture_page);
  DrawPrim(&page);

  POLY_FT4 polygon{};
  setPolyFT4(&polygon);
  // POLY_FT4 carries its own tpage in PsyCross. Without it MENU.HOG TIMs
  // sample page zero, which produced the flat map/weapon placeholders.
  polygon.tpage = texture_page;
  setRGB0(&polygon, 128U, 128U, 128U);
  setXY4(&polygon, static_cast<float>(draw_bounds.x),
         static_cast<float>(draw_bounds.y),
         static_cast<float>(draw_bounds.x + draw_bounds.width),
         static_cast<float>(draw_bounds.y), static_cast<float>(draw_bounds.x),
         static_cast<float>(draw_bounds.y + draw_bounds.height),
         static_cast<float>(draw_bounds.x + draw_bounds.width),
         static_cast<float>(draw_bounds.y + draw_bounds.height));
  const auto texture_u0 = static_cast<u_char>(u0);
  const auto texture_v0 = static_cast<u_char>(v0);
  const auto texture_u1 = static_cast<u_char>(u1);
  const auto texture_v1 = static_cast<u_char>(v1);
  setUV4(&polygon, texture_u0, texture_v0, texture_u1, texture_v0, texture_u0,
         texture_v1, texture_u1, texture_v1);
  polygon.clut = GetClut(image->clut()->x, image->clut()->y);
  DrawPrim(&polygon);
  // MENU.HOG images share VRAM pages with INTRFACE.HOG. Complete this
  // textured draw before another asset or the font atlas is uploaded.
  DrawSync(0);
  return draw_bounds;
}

void drawPauseDim(const game::PauseRect &bounds) {
  GR_SetBlendMode(BM_AVERAGE);
  GR_EnableDepth(0);
  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, GetTPage(0, 0, 0, 0));
  DrawPrim(&page);
  TILE tile{};
  setTile(&tile);
  setSemiTrans(&tile, 1);
  setRGB0(&tile, 0U, 0U, 0U);
  setXY0(&tile, static_cast<float>(bounds.x), static_cast<float>(bounds.y));
  setWH(&tile, static_cast<float>(bounds.width),
        static_cast<float>(bounds.height));
  DrawPrim(&tile);
  GR_SetBlendMode(BM_NONE);
}

bool drawPauseMenu(const game::PauseMenu &menu,
                   const PauseTextureAtlas &textures,
                   const HudTextureAtlas &interface_textures,
                   std::uint64_t animation_tick,
                   const InputPromptBindings &bindings) {
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);
  const auto full_screen =
      menu.screen() == game::PauseScreen::map && menu.expanded();
  if (full_screen) {
    drawSolidRect(0, 0, screen_width, screen_height, 0U, 0U, 0U);
    DrawSync(0);
  } else if (!drawOriginalAcdFrame(animation_tick)) {
    GR_EnableDepth(1);
    return false;
  }
  const auto prompt_tokens = InputPromptTokenActions{
      .x = InputPromptAction::confirm,
      .t = menu.screen() == game::PauseScreen::root ||
                   menu.controllerBindingPending()
               ? InputPromptAction::pause
                                                    : InputPromptAction::cancel,
  };
  const auto draw_text =
      [&interface_textures, &bindings, prompt_tokens](
          std::string_view source, const game::PauseRect &bounds, int scale,
          PauseRgb color,
          game::PauseTextAlignment alignment = game::PauseTextAlignment::left,
          int line_height = 10) {
        drawMenuText(source, bounds, scale, color, alignment, line_height,
                     interface_textures, bindings, prompt_tokens);
      };
  const auto commands = menu.buildRenderCommands();
  std::optional<game::PauseRect> map_command_bounds;
  std::optional<game::PauseRect> map_draw_bounds;
  auto texture_uploaded = false;

  if (full_screen) {
    constexpr auto panel_fill = PauseRgb{2U, 4U, 16U};
    const auto panel_border = pauseColor(game::PauseColorRole::accent);
    for (const auto &command : commands) {
      if (command.kind == game::PauseRenderKind::panel) {
        drawBorderedRect(command.bounds, panel_fill, panel_border);
      }
    }
    DrawSync(0);
  }

  // Retail MENU.HOG assets and the interface font occupy overlapping PSX
  // VRAM pages. Draw and flush every menu asset first, then restore the
  // original font page once for the complete text/overlay pass. Mixing both
  // in one deferred pass made glyphs disappear or sample map pixels.
  DrawSync(0);
  for (const auto &command : commands) {
    if (command.kind != game::PauseRenderKind::asset &&
        command.kind != game::PauseRenderKind::weapon_icon) {
      continue;
    }
    const auto animated_bounds = animatedScreenCommandBounds(menu, command);
    auto drawn = std::optional<game::PauseRect>{};
    if (command.kind == game::PauseRenderKind::weapon_icon) {
      // MENU.HOG contains the original pre-rendered weapon artwork shown by
      // the retail pause menu. HUD/pickup sprites are only a safe fallback
      // for a damaged or incomplete archive.
      drawn = command.asset.empty()
                  ? std::nullopt
                  : drawPauseTexture(textures, command.asset, animated_bounds);
      if (!drawn) {
        drawn = drawPauseWeaponIcon(interface_textures, command.id,
                                    animated_bounds);
      }
    } else {
      drawn = drawPauseTexture(textures, command.asset, animated_bounds);
    }
    if (drawn) {
      texture_uploaded = true;
      if (command.kind == game::PauseRenderKind::asset &&
          std::string_view{command.asset}.starts_with("MAP")) {
        map_command_bounds = animated_bounds;
        map_draw_bounds = *drawn;
      }
    } else {
      drawBorderedRect(animated_bounds, PauseRgb{0U, 0U, 0U},
                       pauseColor(game::PauseColorRole::muted));
    }
  }
  interface_textures.restoreFont();
  const auto animated_section = animatedSectionSelection(menu);
  const auto animated_item = animatedItemSelection(menu, commands);
  const auto map_highlight =
      pauseMapHighlightColor(pauseColor(game::PauseColorRole::map_highlight));

  for (const auto &command : commands) {
    const auto command_bounds = animatedScreenCommandBounds(menu, command);
    const auto color =
        command.selected && command.color == game::PauseColorRole::map_highlight
            ? map_highlight
            : pauseColor(command.color);
    switch (command.kind) {
    case game::PauseRenderKind::dim_background:
      // The ACD owns an opaque black framebuffer in the original game.
      break;
    case game::PauseRenderKind::panel:
      break;
    case game::PauseRenderKind::title:
      draw_text(command.text, command_bounds, 1, color, command.alignment,
                command.line_height);
      break;
    case game::PauseRenderKind::text:
      // The selected objective/location and its map light share the same
      // low-contrast, fixed-rate pulse calculated above.
      draw_text(command.text, command_bounds, 1, color, command.alignment,
                command.line_height);
      break;
    case game::PauseRenderKind::menu_item:
      if (command.panel == game::PausePanelRole::right_sections) {
        const auto border = command.selected && !animated_section
                                ? color
                                : pauseColor(game::PauseColorRole::normal);
        drawAcdOutline(command_bounds, border);
      } else {
        const auto retail_weapon_list =
            menu.screen() == game::PauseScreen::weapons && !menu.expanded() &&
            command.panel == game::PausePanelRole::left_content;
        // The weapon index is plain text in MENU.OVL.  Only the current row
        // receives the compact selection plate shown in the retail capture.
        if (!retail_weapon_list || command.selected) {
          drawBorderedRect(command_bounds, PauseRgb{0U, 0U, 0U},
                           command.selected && !animated_item
                               ? color
                               : PauseRgb{38U, 46U, 128U});
        }
      }
      draw_text(command.text,
                game::PauseRect{
                    static_cast<std::int16_t>(
                        command.panel == game::PausePanelRole::right_sections
                            ? command_bounds.x
                            : command_bounds.x + 2),
                    static_cast<std::int16_t>(command_bounds.y + 2),
                    static_cast<std::int16_t>(
                        command.panel == game::PausePanelRole::right_sections
                            ? command_bounds.width
                            : command_bounds.width - 4),
                    static_cast<std::int16_t>(command_bounds.height - 2),
                },
                1, color, command.alignment, command.line_height);
      break;
    case game::PauseRenderKind::selection:
      drawBorderedRect(command_bounds, PauseRgb{0U, 0U, 0U},
                       pauseColor(game::PauseColorRole::selected));
      drawSolidRect(command_bounds.x + command_bounds.width / 2,
                    command_bounds.y + 2, 1, command_bounds.height - 4,
                    color.red, color.green, color.blue);
      drawSolidRect(
          command_bounds.x + 2, command_bounds.y + command_bounds.height / 2,
          command_bounds.width - 4, 1, color.red, color.green, color.blue);
      break;
    case game::PauseRenderKind::divider:
      drawSolidRect(command_bounds.x, command_bounds.y, command_bounds.width,
                    std::max<std::int16_t>(command_bounds.height, 1), color.red,
                    color.green, color.blue);
      break;
    case game::PauseRenderKind::slider: {
      if (!command.text.empty()) {
        draw_text(command.text,
                  game::PauseRect{
                      command_bounds.x,
                      static_cast<std::int16_t>(command_bounds.y - 9),
                      command_bounds.width,
                      8,
                  },
                  1, pauseColor(game::PauseColorRole::muted));
      }
      drawBorderedRect(command_bounds, PauseRgb{4U, 6U, 22U},
                       command.selected
                           ? pauseColor(game::PauseColorRole::selected)
                           : pauseColor(game::PauseColorRole::muted));
      const auto maximum = std::max(command.maximum, 1);
      const auto value = std::clamp(command.value, 0, maximum);
      const auto width =
          (std::max(0, static_cast<int>(command_bounds.width) - 4) * value) /
          maximum;
      drawSolidRect(command_bounds.x + 2, command_bounds.y + 2, width,
                    std::max(1, static_cast<int>(command_bounds.height) - 4),
                    color.red, color.green, color.blue);
      break;
    }
    case game::PauseRenderKind::asset:
    case game::PauseRenderKind::weapon_icon:
      // Rendered in the synchronized asset pass above.
      break;
    case game::PauseRenderKind::map_marker: {
      auto marker = command_bounds;
      if (map_command_bounds && map_draw_bounds) {
        // Marker coordinates describe authored centres.  Keeping the centre
        // normalized makes the MENU.OVL offsets exact even when an expanded
        // view aspect-fits the native MAPn.TIM into a larger rectangle.
        const auto normalized_x =
            static_cast<double>(marker.x + marker.width / 2 -
                                map_command_bounds->x) /
            std::max(1, static_cast<int>(map_command_bounds->width));
        const auto normalized_y =
            static_cast<double>(marker.y + marker.height / 2 -
                                map_command_bounds->y) /
            std::max(1, static_cast<int>(map_command_bounds->height));
        marker.x = static_cast<std::int16_t>(
            map_draw_bounds->x +
            std::lround(normalized_x * map_draw_bounds->width) -
            marker.width / 2);
        marker.y = static_cast<std::int16_t>(
            map_draw_bounds->y +
            std::lround(normalized_y * map_draw_bounds->height) -
            marker.height / 2);
      }
      const auto marker_kind = static_cast<game::MapMarkerKind>(command.id);
      if (marker_kind == game::MapMarkerKind::player) {
        const auto angle = static_cast<double>(command.value) *
                           (2.0 * std::numbers::pi / 4096.0);
        const auto center_x = marker.x + marker.width / 2;
        const auto center_y = marker.y + marker.height / 2;
        const auto radius = std::max(4, static_cast<int>(marker.width) / 2);
        const auto point_x =
            center_x + static_cast<int>(std::lround(std::sin(angle) * radius));
        const auto point_y =
            center_y - static_cast<int>(std::lround(std::cos(angle) * radius));
        const auto left_x =
            center_x +
            static_cast<int>(std::lround(std::sin(angle + 2.45) *
                                         static_cast<double>(radius)));
        const auto left_y =
            center_y -
            static_cast<int>(std::lround(std::cos(angle + 2.45) *
                                         static_cast<double>(radius)));
        const auto right_x =
            center_x +
            static_cast<int>(std::lround(std::sin(angle - 2.45) *
                                         static_cast<double>(radius)));
        const auto right_y =
            center_y -
            static_cast<int>(std::lround(std::cos(angle - 2.45) *
                                         static_cast<double>(radius)));
        drawAcdTriangle(point_x, point_y, left_x, left_y, right_x, right_y,
                        color);
      } else if (marker_kind == game::MapMarkerKind::objective) {
        const auto center_x = marker.x + marker.width / 2;
        const auto center_y = marker.y + marker.height / 2;
        constexpr auto glow_radius = std::int16_t{3};
        drawAcdOutline(
            game::PauseRect{
                static_cast<std::int16_t>(center_x - glow_radius),
                static_cast<std::int16_t>(center_y - glow_radius),
                static_cast<std::int16_t>(glow_radius * 2 + 1),
                static_cast<std::int16_t>(glow_radius * 2 + 1),
            },
            color);
        drawSolidRect(center_x - 1, center_y - 1, 3, 3, color.red, color.green,
                      color.blue);
        drawSolidRect(center_x, center_y - 5, 1, 3, 255U, 210U, 96U);
      } else {
        drawBorderedRect(marker, color, PauseRgb{255U, 255U, 255U});
      }
      break;
    }
    case game::PauseRenderKind::button_hint:
      draw_text(command.text, command_bounds, 1, color, command.alignment,
                command.line_height);
      break;
    case game::PauseRenderKind::dialog:
      drawBorderedRect(command_bounds, PauseRgb{0U, 0U, 0U},
                       pauseColor(game::PauseColorRole::warning));
      draw_text(command.text,
                game::PauseRect{
                    static_cast<std::int16_t>(command_bounds.x + 10),
                    static_cast<std::int16_t>(command_bounds.y + 10),
                    static_cast<std::int16_t>(command_bounds.width - 20),
                    static_cast<std::int16_t>(command_bounds.height - 20),
                },
                1, color, game::PauseTextAlignment::center,
                command.line_height);
      break;
    case game::PauseRenderKind::page_indicator: {
      const auto indicator =
          std::to_string(command.value) + "/" + std::to_string(command.maximum);
      draw_text(indicator, command_bounds, 1, color, command.alignment,
                command.line_height);
      break;
    }
    }
  }
  if (animated_section) {
    drawAcdOutline(*animated_section,
                   pauseColor(game::PauseColorRole::selected));
  }
  if (animated_item) {
    drawAcdOutline(*animated_item, pauseColor(game::PauseColorRole::selected));
  }
  // PsyCross records immediate primitives. Complete every final label/button
  // while the pause pass is still depth-free; otherwise the last queued rows
  // inherit gameplay depth state and intermittently disappear.
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(1);
  return texture_uploaded;
}
