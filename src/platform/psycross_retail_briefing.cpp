#include "psycross_retail_briefing.hpp"
#include "psycross_font_texture.hpp"

#include "sf/assets/mission_briefing.hpp"
#include "sf/assets/tim_image.hpp"
#include "sf/core/error.hpp"
#include "sf/game/hud.hpp"
#include "sf/game/localization.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/mission_start.hpp"

#include <PsyX/PsyX_render.h>
#include <psx/libgpu.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sf::platform::detail {
namespace {

constexpr int screen_width = 384;
constexpr int screen_height = 240;
constexpr int screen_center_x = screen_width / 2;
constexpr int screen_center_y = screen_height / 2;
constexpr int retail_line_height = 8;
constexpr std::uint32_t surround_transition_ticks = 12U;
constexpr std::uint32_t surround_frame_elements = 17U;
constexpr std::uint32_t surround_grid_lines = 43U;

// FUN_80084698 installs the authored 384x240 briefing surround before
// INIT.OVL creates its text region.  The original surround is geometry, not a
// TIM in INTRFACE.HOG, so keep its coordinates in the same UI space as the
// retail text objects.
struct Point {
  int x{};
  int y{};
};

struct Rgb {
  std::uint8_t red{};
  std::uint8_t green{};
  std::uint8_t blue{};
};

struct LineSegment {
  Point first;
  Point second;
};

constexpr Rgb panel_fill{12U, 20U, 68U};
constexpr Rgb panel_outline{64U, 75U, 148U};
constexpr Rgb panel_inner_outline{42U, 52U, 121U};
constexpr Rgb panel_grid{17U, 11U, 48U};
constexpr Rgb panel_progress{87U, 175U, 230U};

constexpr std::array outer_panel{
    Point{31, 7},    Point{353, 7},   Point{365, 20},  Point{365, 190},
    Point{353, 203}, Point{280, 203}, Point{270, 214}, Point{114, 214},
    Point{104, 203}, Point{31, 203},  Point{19, 190},  Point{19, 20},
};

constexpr std::array inner_panel{
    Point{40, 21},   Point{344, 21},  Point{355, 32},  Point{355, 181},
    Point{344, 193}, Point{276, 193}, Point{268, 201}, Point{116, 201},
    Point{108, 193}, Point{40, 193},  Point{29, 181},  Point{29, 32},
};

constexpr Rgb briefing_color{
    assets::RetailBriefingLayout::red,
    assets::RetailBriefingLayout::green,
    assets::RetailBriefingLayout::blue,
};

struct BriefingTexture {
  std::string name;
  assets::TimImage image;
};

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

void uploadTimBlock(const assets::TimBlock &block) {
  const auto checked = [](std::uint16_t value) {
    if (value > static_cast<std::uint16_t>(std::numeric_limits<short>::max())) {
      throw core::Error{core::ErrorCode::unsupported,
                        "Briefing TIM coordinate exceeds PsyCross range"};
    }
    return static_cast<short>(value);
  };
  RECT16 rect{checked(block.x), checked(block.y), checked(block.width_words),
              checked(block.height)};
  // Dense 32-bit packing: LoadImage reinterprets the buffer as u16 units,
  // and u_long leaves zero gaps between them on LP64 hosts.
  std::vector<std::uint32_t> packed((block.words.size() + 1U) / 2U);
  for (std::size_t index = 0U; index < block.words.size(); ++index) {
    packed[index / 2U] |= static_cast<std::uint32_t>(block.words[index])
                          << ((index & 1U) * 16U);
  }
  LoadImage(&rect, reinterpret_cast<u_long *>(packed.data()));
}

void drawSolidRect(int x, int y, int width, int height, Rgb color) {
  TILE tile{};
  setTile(&tile);
  setRGB0(&tile, color.red, color.green, color.blue);
  setXY0(&tile, static_cast<float>(x), static_cast<float>(y));
  setWH(&tile, static_cast<float>(width), static_cast<float>(height));
  DrawPrim(&tile);
}

void drawSolidQuad(Point top_left, Point top_right, Point bottom_left,
                   Point bottom_right, Rgb color) {
  POLY_F4 polygon{};
  setPolyF4(&polygon);
  setRGB0(&polygon, color.red, color.green, color.blue);
  setXY4(&polygon, static_cast<float>(top_left.x),
         static_cast<float>(top_left.y), static_cast<float>(top_right.x),
         static_cast<float>(top_right.y), static_cast<float>(bottom_left.x),
         static_cast<float>(bottom_left.y), static_cast<float>(bottom_right.x),
         static_cast<float>(bottom_right.y));
  DrawPrim(&polygon);
}

void drawLine(Point first, Point second, Rgb color) {
  LINE_F2 line{};
  setLineF2(&line);
  setRGB0(&line, color.red, color.green, color.blue);
  setXY2(&line, static_cast<float>(first.x), static_cast<float>(first.y),
         static_cast<float>(second.x), static_cast<float>(second.y));
  DrawPrim(&line);
}

template <std::size_t Size>
void drawOutlinePrefix(const std::array<Point, Size> &points,
                       std::size_t visible_segments, Rgb color) {
  visible_segments = std::min(visible_segments, points.size());
  for (std::size_t index = 0U; index < visible_segments; ++index) {
    drawLine(points[index], points[(index + 1U) % points.size()], color);
  }
}

void drawBriefingSurround(std::uint32_t retail_tick,
                          double animation_progress) {
  const auto transition_tick = std::min(retail_tick, surround_transition_ticks);
  const auto visible_frame = static_cast<std::size_t>(
      transition_tick * surround_frame_elements / surround_transition_ticks);
  const auto visible_grid = static_cast<std::size_t>(
      transition_tick * surround_grid_lines / surround_transition_ticks);

  // Lay down the concave authored silhouette as four convex spans.
  if (visible_frame != 0U) {
    drawSolidQuad({31, 7}, {353, 7}, {19, 20}, {365, 20}, panel_fill);
    drawSolidRect(19, 20, 346, 170, panel_fill);
    drawSolidQuad({19, 190}, {365, 190}, {31, 203}, {353, 203}, panel_fill);
    drawSolidQuad({104, 203}, {280, 203}, {114, 214}, {270, 214}, panel_fill);

    // Cut the display aperture back out. Its lower edge has the retail centre
    // step which the old black-only bridge lost.
    drawSolidQuad({40, 21}, {344, 21}, {29, 32}, {355, 32}, {});
    drawSolidRect(29, 32, 326, 149, {});
    drawSolidQuad({29, 181}, {355, 181}, {40, 193}, {344, 193}, {});
    drawSolidQuad({108, 193}, {276, 193}, {116, 201}, {268, 201}, {});
  }

  // INIT.OVL authors 27 vertical and 16 horizontal primitives.  Mode 6
  // publishes floor(tick * 43 / 12) of them and highlights the leading edge.
  std::array<LineSegment, surround_grid_lines> grid{};
  auto grid_index = std::size_t{};
  for (auto index = 0; index < 27; ++index) {
    const auto x = 36 + index * 12;
    const auto bottom = index >= 9 && index < 18 ? 203 : 190;
    grid[grid_index++] = {{x, 27}, {x, bottom}};
  }
  for (auto index = 0; index < 16; ++index) {
    const auto y = 32 + index * 10;
    grid[grid_index++] = {{25, y}, {359, y}};
  }
  for (std::size_t index = 0U; index < visible_grid; ++index) {
    drawLine(grid[index].first, grid[index].second, panel_grid);
  }
  if (visible_grid < grid.size()) {
    drawLine(grid[visible_grid].first, grid[visible_grid].second,
             panel_progress);
  }

  constexpr auto outline_segments = outer_panel.size() + inner_panel.size();
  const auto visible_outline =
      visible_frame * outline_segments / surround_frame_elements;
  const auto visible_outer = std::min(visible_outline, outer_panel.size());
  const auto visible_inner = visible_outline > outer_panel.size()
                                 ? visible_outline - outer_panel.size()
                                 : 0U;
  drawOutlinePrefix(outer_panel, visible_outer, panel_outline);
  drawOutlinePrefix(inner_panel, visible_inner, panel_inner_outline);
  if (visible_outer < outer_panel.size()) {
    drawLine(outer_panel[visible_outer],
             outer_panel[(visible_outer + 1U) % outer_panel.size()],
             panel_progress);
  } else if (visible_inner < inner_panel.size()) {
    drawLine(inner_panel[visible_inner],
             inner_panel[(visible_inner + 1U) % inner_panel.size()],
             panel_progress);
  }

  // The three permanent INIT.OVL primitives form the briefing gauge. Its
  // fill follows the text-object reveal rather than unrelated host I/O.
  drawSolidRect(139, 202, 106, 10, {});
  drawLine({139, 202}, {245, 202}, panel_outline);
  drawLine({245, 202}, {245, 212}, panel_outline);
  drawLine({245, 212}, {139, 212}, panel_outline);
  drawLine({139, 212}, {139, 202}, panel_outline);
  const auto progress_width = static_cast<int>(
      std::lround(std::clamp(animation_progress, 0.0, 1.0) * 94.0));
  if (progress_width > 0) {
    drawSolidRect(145, 205, progress_width, 3, panel_progress);
  }
}

void drawTextureRegion(const assets::TimImage &page_image, int x, int y,
                       std::uint8_t source_u, std::uint8_t source_v,
                       std::uint8_t width, std::uint8_t height, Rgb color) {
  const auto &pixels = page_image.pixels();
  const auto page_x =
      static_cast<int>(pixels.x & static_cast<std::uint16_t>(~63U));
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
  polygon.clut = GetClut(page_image.clut()->x, page_image.clut()->y);
  setRGB0(&polygon, color.red, color.green, color.blue);
  setXY4(&polygon, static_cast<float>(x), static_cast<float>(y),
         static_cast<float>(x + width), static_cast<float>(y),
         static_cast<float>(x), static_cast<float>(y + height),
         static_cast<float>(x + width), static_cast<float>(y + height));
  setUV4(&polygon, source_u, source_v, static_cast<u_char>(source_u + width),
         source_v, source_u, static_cast<u_char>(source_v + height),
         static_cast<u_char>(source_u + width),
         static_cast<u_char>(source_v + height));
  DrawPrim(&polygon);
}

int characterAdvance(char value) noexcept {
  if (value == ' ') {
    return 4;
  }
  const auto glyph = game::originalHudGlyph(value);
  return glyph ? glyph->advance() : 0;
}

int promptTextWidth(std::string_view source) noexcept {
  auto width = 0;
  for (std::size_t index = 0U; index < source.size(); ++index) {
    width += characterAdvance(source[index]);
  }
  return width == 0 ? 0 : width - 2;
}

struct PositionedGlyph {
  game::OriginalHudGlyph glyph;
  int x{};
  int y{};
};

struct TextLayout {
  std::vector<PositionedGlyph> glyphs;
  int lines{1};
};

TextLayout layoutTextObject(std::string_view text, int left, int top, int right,
                            int bottom) {
  // The generated Russian atlas is sampled bilinearly from a 2x texture.
  // Reserve a small logical guard at the right edge so the final texel's
  // filter footprint never touches the briefing surround.  English keeps the
  // exact retail layout.
  if (game::russianLanguageActive()) {
    right -= 2;
  }
  TextLayout layout;
  auto x = left;
  auto y = top;
  for (std::size_t index = 0U; index < text.size();) {
    if (text[index] == '\r') {
      ++index;
      continue;
    }
    if (text[index] == '\n') {
      x = left;
      y += retail_line_height;
      ++layout.lines;
      ++index;
      continue;
    }
    if (text[index] == ' ') {
      x += 4;
      ++index;
      continue;
    }

    auto word_end = index;
    while (word_end < text.size() && text[word_end] != ' ' &&
           text[word_end] != '\n' && text[word_end] != '\r') {
      ++word_end;
    }
    const auto word = text.substr(index, word_end - index);
    if (x != left && x + game::originalHudTextWidth(word) > right) {
      x = left;
      y += retail_line_height;
      ++layout.lines;
    }

    for (const auto value : word) {
      const auto glyph = game::originalHudGlyph(value);
      if (!glyph) {
        continue;
      }
      if (x != left && x + glyph->width > right) {
        x = left;
        y += retail_line_height;
        ++layout.lines;
      }
      if (y + glyph->height() <= bottom) {
        layout.glyphs.push_back(PositionedGlyph{*glyph, x, y});
      }
      x += glyph->advance();
    }
    index = word_end;
  }
  return layout;
}

double briefingTextProgress(const assets::MissionBriefing &briefing, int left,
                            int top, int right, int bottom,
                            double retail_time) {
  auto y = top;
  auto animation_steps = std::size_t{};
  const auto include = [&](std::string_view text) {
    const auto layout = layoutTextObject(text, left, y, right, bottom);
    y += layout.lines * retail_line_height;
    if (layout.glyphs.empty()) {
      return;
    }
    const auto glyphs_per_tick =
        static_cast<std::size_t>(std::max(layout.lines, 1));
    animation_steps = std::max(animation_steps,
                               (layout.glyphs.size() + glyphs_per_tick - 1U) /
                                   glyphs_per_tick);
  };
  include(briefing.retailTitle());
  for (const auto directive : briefing.retailDirectives()) {
    if (!directive.empty()) {
      include(directive);
    }
  }
  if (animation_steps == 0U) {
    return 1.0;
  }
  return std::clamp((std::max(retail_time, 0.0) + 1.0) /
                        static_cast<double>(animation_steps),
                    0.0, 1.0);
}

std::uint8_t settleChannel(std::uint8_t target,
                           std::uint32_t settle_ticks) noexcept {
  auto current = 255;
  for (auto tick = 0U; tick < std::min(settle_ticks, 64U); ++tick) {
    const auto change = (static_cast<int>(target) - current) / 4;
    if (change == 0) {
      break;
    }
    current += change;
  }
  return static_cast<std::uint8_t>(current);
}

int drawTextObject(const assets::TimImage &font, std::string_view text,
                   int left, int top, int right, int bottom,
                   std::uint32_t retail_tick,
                   const PsyCrossFontTexture *native_font,
                   bool *animation_complete = nullptr) {
  const ScopedPsyCrossFontTexture font_binding{native_font};
  const auto layout = layoutTextObject(text, left, top, right, bottom);
  const auto glyphs_per_tick =
      static_cast<std::size_t>(std::max(layout.lines, 1));
  const auto leading = static_cast<std::size_t>(retail_tick) * glyphs_per_tick;
  const auto visible =
      std::min(layout.glyphs.size(), leading + glyphs_per_tick);
  if (animation_complete != nullptr) {
    *animation_complete =
        *animation_complete && visible == layout.glyphs.size();
  }
  for (std::size_t index = 0U; index < visible; ++index) {
    const auto first_tick = static_cast<std::uint32_t>(index / glyphs_per_tick);
    const auto settle_ticks = retail_tick - first_tick;
    const auto color =
        settle_ticks == 0U
            ? Rgb{255U, 255U, 255U}
            : Rgb{settleChannel(briefing_color.red, settle_ticks),
                  settleChannel(briefing_color.green, settle_ticks),
                  settleChannel(briefing_color.blue, settle_ticks)};
    const auto &entry = layout.glyphs[index];
    drawTextureRegion(font, entry.x, entry.y, entry.glyph.u, entry.glyph.v,
                      entry.glyph.width, entry.glyph.height(), color);
  }
  return layout.lines;
}

void drawPrompt(const std::vector<BriefingTexture> &textures,
                std::uint32_t prompt_tick,
                const PsyCrossFontTexture *native_font,
                const InputPromptBindings &bindings) {
  const auto find = [&](std::string_view name) -> const assets::TimImage & {
    const auto match =
        std::find_if(textures.begin(), textures.end(),
                     [&](const auto &entry) { return entry.name == name; });
    if (match == textures.end()) {
      throw core::Error{core::ErrorCode::not_found,
                        "Retail briefing texture is missing: " +
                            std::string{name}};
    }
    return match->image;
  };

  const auto brightness = game::MissionStartGate::promptBrightness(prompt_tick);
  const auto color = Rgb{brightness, brightness, brightness};
  const auto right = screen_center_x + assets::RetailBriefingLayout::prompt_x;
  const auto source = inputHintText(
      game::localizeTextCopy(assets::RetailBriefingLayout::prompt), bindings);
  const auto prompt_width = promptTextWidth(source);
  auto x = right - prompt_width;
  const auto y = screen_center_y + assets::RetailBriefingLayout::prompt_y;
  const auto &font = find("FONTA.TIM");
  const ScopedPsyCrossFontTexture font_binding{native_font};
  for (std::size_t index = 0U; index < source.size(); ++index) {
    if (source[index] == ' ') {
      x += 4;
      continue;
    }
    const auto glyph = game::originalHudGlyph(source[index]);
    if (!glyph) {
      continue;
    }
    drawTextureRegion(font, x, y, glyph->u, glyph->v, glyph->width,
                      glyph->height(), color);
    x += glyph->advance();
  }
}

} // namespace

struct PsyCrossRetailBriefing::Impl final {
  explicit Impl(const game::MissionPackage &mission) {
    constexpr std::array required{
        std::string_view{"FONTA.TIM"},
        std::string_view{"FONTB.TIM"},
        std::string_view{"FONTC.TIM"},
        std::string_view{"SYMBOL.TIM"},
    };
    for (const auto name : required) {
      const auto localized =
          name != "SYMBOL.TIM" ? game::readLocalizedAsset(
                                     std::string{"fonts/"} + std::string{name})
                               : std::nullopt;
      auto image = assets::TimImage::parse(
          localized ? std::span<const std::byte>{*localized}
                    : mission.interfaceAssets().file(name));
      if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut()) {
        throw core::Error{core::ErrorCode::invalid_format,
                          "Retail briefing font is not indexed 8-bit TIM"};
      }
      if (!textures.empty() &&
          image.clut()->words != textures.front().image.clut()->words) {
        throw core::Error{core::ErrorCode::invalid_format,
                          "Retail briefing TIM palettes do not match"};
      }
      textures.push_back(BriefingTexture{std::string{name}, std::move(image)});
    }

    const auto &font = image("FONTA.TIM");
    if (game::russianLanguageActive()) {
      native_font = std::make_unique<PsyCrossFontTexture>(
          font, image("FONTB.TIM"), image("FONTC.TIM"));
    }
    const auto page_x = font.pixels().x & ~std::uint16_t{63U};
    const auto page_y = font.pixels().y & ~std::uint16_t{255U};
    if (native_font == nullptr) {
      for (const auto &texture : textures) {
        if ((texture.image.pixels().x & ~std::uint16_t{63U}) != page_x ||
            (texture.image.pixels().y & ~std::uint16_t{255U}) != page_y) {
          throw core::Error{core::ErrorCode::invalid_format,
                            "Retail briefing TIMs do not share one page"};
        }
      }
    }
    for (const auto &texture : textures) {
      if (native_font != nullptr && texture.name.starts_with("FONT")) {
        continue;
      }
      uploadTimBlock(texture.image.pixels());
    }
    uploadTimBlock(*textures.front().image.clut());
    DrawSync(0);
  }

  [[nodiscard]] const assets::TimImage &image(std::string_view name) const {
    const auto match =
        std::find_if(textures.begin(), textures.end(),
                     [&](const auto &entry) { return entry.name == name; });
    if (match == textures.end()) {
      throw core::Error{core::ErrorCode::not_found,
                        "Retail briefing texture is missing: " +
                            std::string{name}};
    }
    return match->image;
  }

  std::vector<BriefingTexture> textures;
  std::unique_ptr<PsyCrossFontTexture> native_font;
  mutable std::optional<std::uint32_t> prompt_start_tick;
};

PsyCrossRetailBriefing::PsyCrossRetailBriefing(
    const game::MissionPackage &mission)
    : impl_(std::make_unique<Impl>(mission)) {}

PsyCrossRetailBriefing::~PsyCrossRetailBriefing() = default;

bool PsyCrossRetailBriefing::draw(const assets::MissionBriefing &briefing,
                                  double retail_time,
                                  const InputPromptBindings &bindings) const {
  const auto retail_tick =
      static_cast<std::uint32_t>(std::max(std::floor(retail_time), 0.0));
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);
  drawSolidRect(0, 0, screen_width, screen_height, {});

  const auto left = screen_center_x + assets::RetailBriefingLayout::region_x;
  const auto top = screen_center_y + assets::RetailBriefingLayout::region_y;
  const auto right = left + assets::RetailBriefingLayout::region_width;
  const auto bottom = top + assets::RetailBriefingLayout::region_height;
  const auto &font = impl_->image("FONTA.TIM");
  drawBriefingSurround(
      retail_tick,
      briefingTextProgress(briefing, left, top, right, bottom, retail_time));

  auto text_animation_complete = true;
  auto y = top;
  y += drawTextObject(font, briefing.retailTitle(), left, y, right, bottom,
                      retail_tick, impl_->native_font.get(),
                      &text_animation_complete) *
       retail_line_height;
  for (const auto directive : briefing.retailDirectives()) {
    if (directive.empty()) {
      continue;
    }
    y += drawTextObject(font, directive, left, y, right, bottom, retail_tick,
                        impl_->native_font.get(), &text_animation_complete) *
         retail_line_height;
  }
  if (text_animation_complete) {
    if (!impl_->prompt_start_tick) {
      impl_->prompt_start_tick = retail_tick;
    }
    drawPrompt(impl_->textures, retail_tick - *impl_->prompt_start_tick,
               impl_->native_font.get(), bindings);
  } else {
    impl_->prompt_start_tick.reset();
  }

  DrawSync(0);
  GR_EnableDepth(1);
  return text_animation_complete;
}

} // namespace sf::platform::detail
