#include "psycross_scene_viewer.hpp"
#include "muzzle_flash_texture.hpp"
#include "psycross_audio_output.hpp"
#include "psycross_font_texture.hpp"
#include "psycross_movie_player.hpp"
#include "psycross_runtime_guards.hpp"
#include "psycross_vram.hpp"
#include "psycross_window_mode.hpp"

#include "sf/assets/tim_image.hpp"
#include "sf/core/error.hpp"
#include "sf/core/polygon_clipper.hpp"
#include "sf/game/dynamic_lighting.hpp"
#include "sf/game/effects.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/localization.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/pause_menu.hpp"
#include "sf/game/pause_menu_data.hpp"
#include "sf/game/retail_cheats.hpp"
#include "sf/platform/player_input.hpp"
#include "sf/platform/stable_frame_vector.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>
#include <SDL.h>
#include <psx/libetc.h>
#include <psx/libgpu.h>
#include <psx/libgte.h>
#include <psx/libpad.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

extern "C" void DpqColor(CVECTOR *input, int depth_cue, CVECTOR *output);

namespace sf::platform::detail {
namespace {

constexpr int screen_width = 384;
constexpr int screen_height = 240;
constexpr double guest_draw_offset_x = static_cast<double>(screen_width) * 0.5;
constexpr double guest_draw_offset_y = static_cast<double>(screen_height) * 0.5;
constexpr int ordering_table_size = 4096;
// USA v1.1 DAT_8012f9b8: exact source record inserted into DAT_80116464 by
// the retail flashlight toggle path.
// The EMD/HMD paths clip complete polygons, so a close gameplay plane no
// longer drops whole wall or actor rectangles near the camera.
constexpr int near_plane = 32;
constexpr double near_clip_depth = static_cast<double>(near_plane) + 2.0;
// Manual aim now keeps its eye outside the same conservative gameplay plane;
// using one depth here also avoids tiny GTE depths and projected overflow.
constexpr double first_person_near_clip_depth = near_clip_depth;
double active_near_clip_depth = near_clip_depth;
constexpr std::uint32_t retail_flashlight_source = 0x8012f9b8U;
constexpr std::uint16_t mission_clut_source_x = 768U;
constexpr std::uint16_t mission_clut_source_y = 480U;
// Mission residency can use every physical page 6..31. Keep the PC HUD
// copy in framebuffer pages 0..3 instead of restoring its retail page-12
// atlas over streamed wall textures.
constexpr std::uint16_t hud_resident_clut_x = 0U;
constexpr std::uint16_t hud_resident_clut_y = 254U;
// Combat sprites occupy the unused bottom 64 rows of native CFIRE page 5.
// CFIRE uses rows 0..191, so this keeps every authored combat texel persistent
// while leaving page 18 available to missions that need all 26 streamed pages.
constexpr unsigned int effect_resident_page = 5U;
constexpr std::uint16_t effect_resident_page_x = 320U;
constexpr std::uint16_t effect_resident_page_y = 0U;
constexpr std::uint16_t effect_resident_vram_y = 192U;
constexpr std::uint16_t effect_resident_clut_x = 0U;
constexpr std::uint16_t effect_resident_secondary_clut_y = 253U;
constexpr std::uint16_t muzzle_flash_resident_u = 0U;
constexpr std::uint16_t muzzle_flash_resident_clut_y = 252U;
constexpr std::uint16_t pickup_resident_clut_y = 248U;
// Pack the 32x32 vest below YELOSHOT in the already reserved lower half of
// CFIRE page 5. The previous framebuffer placement overlapped the relocated
// mission CLUT rows (including Gabe's skin palette).
constexpr std::uint16_t pickup_resident_x = 336U;
constexpr std::uint16_t pickup_resident_y = 224U;
constexpr std::string_view armor_pickup_texture = "VEST2.TIM";
constexpr std::uint16_t environment_resident_clut_y = 251U;
// Dynamic CFIRE/SPFX frames used to be uploaded into their retail pages
// 28/29/31 every frame, temporarily replacing live world/actor textures and
// ZCLUT row 511. Native presentation owns framebuffer pages 4-5, so keep one
// immutable fire atlas there. HUD/CFIRE/combat palettes occupy the otherwise
// unused rows 252-255 of framebuffer pages 0-3; resident HUD pixels end at
// row 191, so no palette write intersects streamed texture storage.
constexpr unsigned int fire_resident_fire_page = 4U;
constexpr unsigned int fire_resident_other_page = 5U;
constexpr std::uint16_t fire_resident_clut_x = 0U;
constexpr std::uint16_t fire_resident_clut_y = 255U;
constexpr std::array<std::string_view, 8> scope_bearings{
    "SCP0.TIM",   "SCP45.TIM",  "SCP90.TIM",  "SCP135.TIM",
    "SCP180.TIM", "SCP225.TIM", "SCP270.TIM", "SCP315.TIM",
};
constexpr std::array<std::string_view, 5> nightvision_scope_layers{
    "INFRA.TIM", "INFRA_R.TIM", "INFRAA.TIM", "INFRAB.TIM", "INFRAC.TIM",
};
// The authored INFRA sprites live on streamed page 24. Relocate them as one
// non-overlapping strip into the unused right half of the resident HUD atlas.
constexpr std::array<std::uint16_t, 5> nightvision_resident_x{
    128U, 144U, 160U, 174U, 189U,
};
constexpr std::uint16_t nightvision_resident_y = 0U;

std::array<std::array<unsigned int, 32>, 2> streamed_texture_page_remap = [] {
  std::array<std::array<unsigned int, 32>, 2> result{};
  for (auto &bank : result) {
    for (unsigned int page = 0; page < bank.size(); ++page) {
      bank[page] = physicalTexturePage(page);
    }
  }
  return result;
}();

std::array<std::array<unsigned int, 32>, 2> streamed_clut_row_remap = [] {
  std::array<std::array<unsigned int, 32>, 2> result{};
  for (auto &bank : result) {
    for (unsigned int row = 0; row < bank.size(); ++row) {
      bank[row] = row;
    }
  }
  return result;
}();

std::uint32_t streamed_vlf_page_mask{};

class RetailCrateTextureOverlay final {
public:
  explicit RetailCrateTextureOverlay(const game::MissionPackage &mission)
      : trim_(assets::TimImage::parse(
            mission.interfaceAssets().file("WEPTRIM.TIM"))),
        inner_(assets::TimImage::parse(
            mission.interfaceAssets().file("WEPINNER.TIM"))),
        top_(assets::TimImage::parse(
            mission.interfaceAssets().file("WEPTOP.TIM"))) {
    const auto valid = [](const assets::TimImage &image) {
      return image.mode() == assets::TimPixelMode::indexed8 && image.clut() &&
             image.clut()->x == mission_clut_source_x &&
             image.clut()->y == mission_clut_source_y + 3U &&
             image.clut()->width_words == 256U && image.clut()->height == 1U;
    };
    if (!valid(trim_) || !valid(inner_) || !valid(top_) ||
        trim_.clut()->words != inner_.clut()->words ||
        trim_.clut()->words != top_.clut()->words ||
        timTexturePage(trim_) != 12U || timTexturePage(inner_) != 13U ||
        timTexturePage(top_) != 13U) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Retail weapon-crate texture layout is invalid"};
    }
  }

  [[nodiscard]] static constexpr std::size_t sourceClutRow() noexcept {
    return 3U;
  }

  void copyClut(std::span<std::byte> destination) const {
    constexpr std::size_t row_bytes = 256U * 2U;
    if (destination.size() != row_bytes) {
      throw core::Error{core::ErrorCode::invalid_argument,
                        "Weapon-crate CLUT destination is invalid"};
    }
    const auto &words = trim_.clut()->words;
    for (std::size_t index = 0U; index < words.size(); ++index) {
      destination[index * 2U] = static_cast<std::byte>(words[index] & 0xffU);
      destination[index * 2U + 1U] = static_cast<std::byte>(words[index] >> 8U);
    }
  }

  void upload(unsigned int trim_page, unsigned int top_page) const {
    uploadAt(trim_, trim_page);
    uploadAt(inner_, top_page);
    uploadAt(top_, top_page);
  }

private:
  static void uploadAt(const assets::TimImage &image,
                       unsigned int physical_page) {
    if (physical_page >= 32U) {
      throw core::Error{core::ErrorCode::invalid_argument,
                        "Weapon-crate texture page is invalid"};
    }
    const auto source_page = timTexturePage(image);
    const auto source_page_x = (source_page & 15U) * 64U;
    const auto source_page_y = source_page > 15U ? 256U : 0U;
    const auto destination_page_x = (physical_page & 15U) * 64U;
    const auto destination_page_y = physical_page > 15U ? 256U : 0U;
    uploadTimBlockAt(
        image.pixels(),
        static_cast<std::uint16_t>(destination_page_x + image.pixels().x -
                                   source_page_x),
        static_cast<std::uint16_t>(destination_page_y + image.pixels().y -
                                   source_page_y));
  }

  assets::TimImage trim_;
  assets::TimImage inner_;
  assets::TimImage top_;
};

struct EffectSpritePlacement {
  std::uint16_t texture_page{};
  std::uint16_t clut{};
  double minimum_u{};
  double minimum_v{};
  double maximum_u{};
  double maximum_v{};
};

struct FireTexturePlacement {
  bool valid{};
  std::array<std::array<EffectSpritePlacement, 16U>, 4U> frames{};

  [[nodiscard]] const EffectSpritePlacement *
  frame(game::LegacyEffectSpriteFamily family,
        std::size_t frame_index) const noexcept {
    const auto family_index = static_cast<std::size_t>(family);
    if (!valid || family_index == 0U || family_index > frames.size() ||
        frame_index >= game::legacyEffectSpriteLayout(family).frame_count) {
      return nullptr;
    }
    return &frames[family_index - 1U][frame_index];
  }
};

class CombatEffectTextureAtlas final {
public:
  explicit CombatEffectTextureAtlas(const game::MissionPackage &mission)
      : blood_(
            assets::TimImage::parse(mission.specialEffects().file("BLOT.TIM"))),
        blood_mark_(assets::TimImage::parse(
            mission.specialEffects().file("BBHOLE.TIM"))) {
    validate(blood_, "BLOT.TIM");
    validate(blood_mark_, "BBHOLE.TIM");
    if (blood_.clut()->words != blood_mark_.clut()->words) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "SPFX combat sprites do not share retail palettes"};
    }
    restore();
  }

  void restore() const {
    uploadMuzzleFlash();
    upload(blood_, 80U, effect_resident_secondary_clut_y);
    upload(blood_mark_, 96U, effect_resident_secondary_clut_y);
    DrawSync(0);
  }

  [[nodiscard]] EffectSpritePlacement muzzleFlash() const noexcept {
    return EffectSpritePlacement{
        static_cast<std::uint16_t>(
            GetTPage(0, 1, effect_resident_page_x, effect_resident_page_y)),
        static_cast<std::uint16_t>(
            GetClut(effect_resident_clut_x, muzzle_flash_resident_clut_y)),
        static_cast<double>(muzzle_flash_resident_u),
        static_cast<double>(effect_resident_vram_y - effect_resident_page_y),
        static_cast<double>(muzzle_flash_resident_u +
                            muzzle_flash_texture::width - 1U),
        static_cast<double>(effect_resident_vram_y - effect_resident_page_y +
                            muzzle_flash_texture::height - 1U),
    };
  }
  [[nodiscard]] EffectSpritePlacement blood() const noexcept {
    return placement(80U, effect_resident_secondary_clut_y, blood_);
  }
  [[nodiscard]] EffectSpritePlacement bloodMark() const noexcept {
    return placement(96U, effect_resident_secondary_clut_y, blood_mark_);
  }

private:
  static void uploadMuzzleFlash() {
    static_assert(muzzle_flash_texture::pixels.size() ==
                  muzzle_flash_texture::width_words *
                      muzzle_flash_texture::height);
    RECT16 pixels{
        static_cast<short>(effect_resident_page_x +
                           muzzle_flash_resident_u / 4U),
        static_cast<short>(effect_resident_vram_y),
        static_cast<short>(muzzle_flash_texture::width_words),
        static_cast<short>(muzzle_flash_texture::height),
    };
    LoadImage(&pixels, reinterpret_cast<u_long *>(const_cast<std::uint16_t *>(
                           muzzle_flash_texture::pixels.data())));
    RECT16 clut{
        static_cast<short>(effect_resident_clut_x),
        static_cast<short>(muzzle_flash_resident_clut_y),
        static_cast<short>(muzzle_flash_texture::clut.size()),
        1,
    };
    LoadImage(&clut, reinterpret_cast<u_long *>(const_cast<std::uint16_t *>(
                         muzzle_flash_texture::clut.data())));
  }

  static void validate(const assets::TimImage &image, std::string_view name) {
    if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut() ||
        image.clut()->width_words != 256U || image.clut()->height != 1U ||
        image.displayWidth() > 32U || image.displayHeight() > 64U) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Unexpected combat effect TIM layout: " +
                            std::string{name}};
    }
  }

  static void upload(const assets::TimImage &image, std::uint16_t u,
                     std::uint16_t clut_y) {
    uploadTimBlockAt(
        image.pixels(),
        static_cast<std::uint16_t>(effect_resident_page_x + u / 2U),
        effect_resident_vram_y);
    uploadTimBlockAt(*image.clut(), effect_resident_clut_x, clut_y);
  }

  static EffectSpritePlacement
  placement(std::uint16_t u, std::uint16_t clut_y,
            const assets::TimImage &image) noexcept {
    const auto width = image.displayWidth();
    const auto height = image.displayHeight();
    return EffectSpritePlacement{
        static_cast<std::uint16_t>(
            GetTPage(1, 1, effect_resident_page_x, effect_resident_page_y)),
        static_cast<std::uint16_t>(GetClut(effect_resident_clut_x, clut_y)),
        static_cast<double>(u),
        static_cast<double>(effect_resident_vram_y - effect_resident_page_y),
        static_cast<double>(u + width - 1U),
        static_cast<double>(effect_resident_vram_y - effect_resident_page_y +
                            height - 1U),
    };
  }

  assets::TimImage blood_;
  assets::TimImage blood_mark_;
};

FireTexturePlacement
uploadFireTextureAtlas(const game::ObjectFireEmitter &fire) {
  if (fire.frames.empty() || fire.fire_frames.empty() ||
      fire.breath_frames.empty() || fire.vapor_frames.empty() ||
      !fire.frames.front().clut()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "CFIRE texture set is empty"};
  }
  auto placement = FireTexturePlacement{true};
  const auto upload_family = [&](const std::vector<assets::TimImage> &frames,
                                 game::LegacyEffectSpriteFamily family,
                                 unsigned int physical_page,
                                 std::uint16_t base_y, std::size_t columns) {
    const auto layout = game::legacyEffectSpriteLayout(family);
    if (frames.size() != layout.frame_count || columns == 0U) {
      throw core::Error{core::ErrorCode::unsupported,
                        "SPFX animation has an unexpected frame count"};
    }
    const auto page_x = static_cast<std::uint16_t>((physical_page & 15U) * 64U);
    const auto page_y =
        static_cast<std::uint16_t>(physical_page > 15U ? 256U : 0U);
    for (std::size_t index = 0; index < frames.size(); ++index) {
      const auto &frame = frames[index];
      const auto &pixels = frame.pixels();
      if (frame.mode() != assets::TimPixelMode::indexed8 ||
          pixels.width_words != layout.width_words ||
          pixels.height != layout.height || !frame.clut() ||
          frame.clut()->words != fire.frames.front().clut()->words) {
        throw core::Error{core::ErrorCode::unsupported,
                          "SPFX frame does not match its retail family layout"};
      }
      const auto column = index % columns;
      const auto row = index / columns;
      const auto local_x = column * layout.width_words;
      const auto local_y =
          static_cast<std::size_t>(base_y) + row * layout.height;
      if (local_x + layout.width_words > 64U ||
          local_y + layout.height > 256U) {
        throw core::Error{core::ErrorCode::unsupported,
                          "Native SPFX atlas is exhausted"};
      }
      const auto destination_x = static_cast<std::uint16_t>(page_x + local_x);
      const auto destination_y = static_cast<std::uint16_t>(page_y + local_y);
      uploadTimBlockAt(pixels, destination_x, destination_y);
      const auto minimum_u = static_cast<double>(local_x * 2U);
      const auto minimum_v = static_cast<double>(local_y);
      placement.frames[static_cast<std::size_t>(family) - 1U][index] =
          EffectSpritePlacement{
              static_cast<std::uint16_t>(
                  GetTPage(texturePageMode(frame.mode()), 1, page_x, page_y)),
              static_cast<std::uint16_t>(
                  GetClut(fire_resident_clut_x, fire_resident_clut_y)),
              minimum_u,
              minimum_v,
              minimum_u + static_cast<double>(frame.displayWidth() - 1U),
              minimum_v + static_cast<double>(frame.displayHeight() - 1U),
          };
    }
  };
  upload_family(fire.fire_frames, game::LegacyEffectSpriteFamily::fire,
                fire_resident_fire_page, 0U, 4U);
  upload_family(fire.frames, game::LegacyEffectSpriteFamily::explosion,
                fire_resident_other_page, 0U, 4U);
  upload_family(fire.vapor_frames, game::LegacyEffectSpriteFamily::vapor,
                fire_resident_other_page, 96U, 4U);
  // BRETH frames are the only 16-pixel-wide indexed8 family: their TIM
  // payload is eight 16-bit words per row, not the 16 used by 32px families.
  upload_family(fire.breath_frames, game::LegacyEffectSpriteFamily::breath,
                fire_resident_other_page, 160U, 8U);
  uploadTimBlockAt(*fire.frames.front().clut(), fire_resident_clut_x,
                   fire_resident_clut_y);
  return placement;
}

struct GuestSpriteTexturePlacement {
  std::uint16_t texture_page{};
  std::uint16_t clut{};
  std::uint8_t u{};
  std::uint8_t v{};
};

struct GlassShardVertexState {
  double x{};
  double y{};
  double z{};
  double u{};
  double v{};
};

struct GlassShardVectorState {
  double x{};
  double y{};
  double z{};
};

struct GlassShardPresentationState {
  std::array<GlassShardVertexState, 3U> vertices{};
  GlassShardVectorState centre{};
  GlassShardVectorState velocity{};
  GlassShardVectorState spin_axis{0.0, 1.0, 0.0};
  double angular_speed{};
  std::uint16_t texture_page{};
  std::uint16_t clut{};
  std::uint8_t texture_bank{};
  bool semi_transparent{};
  std::uint16_t age{};
  std::uint16_t lifetime{28U};
};

class EnvironmentTextureAtlas final {
public:
  explicit EnvironmentTextureAtlas(const game::MissionPackage &mission) {
    const auto append = [&](std::string_view name, std::uint16_t u,
                            std::uint16_t v) {
      auto image = assets::TimImage::parse(mission.specialEffects().file(name));
      if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut() ||
          image.clut()->width_words != 256U || image.clut()->height != 1U ||
          static_cast<unsigned int>(u) + image.displayWidth() > 256U ||
          static_cast<unsigned int>(v) + image.displayHeight() > 256U) {
        throw core::Error{core::ErrorCode::invalid_format,
                          "Unexpected retail environment TIM layout: " +
                              std::string{name}};
      }
      if (!entries_.empty() &&
          image.clut()->words != entries_.front().image.clut()->words) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "Retail environment TIM files do not share the SPFX palette"};
      }
      entries_.push_back(Entry{std::move(image), u, v});
    };
    for (std::size_t index = 0; index < 8U; ++index) {
      auto name = std::string{"STEAM00"} + std::to_string(index) + ".TIM";
      append(name, static_cast<std::uint16_t>((index % 4U) * 32U),
             static_cast<std::uint16_t>((index / 4U) * 64U));
    }
    append("SNOPRTR.TIM", 0U, 128U);
    append("SNOPRTL.TIM", 32U, 128U);
    for (std::size_t index = 0; index < 16U; ++index) {
      auto name = std::string{"BRETH"};
      if (index < 10U) {
        name.push_back('0');
      }
      name += std::to_string(index) + ".TIM";
      append(name, static_cast<std::uint16_t>((index % 8U) * 16U),
             static_cast<std::uint16_t>(160U + (index / 8U) * 16U));
    }
  }

  [[nodiscard]] bool
  contains(const game::LegacyGuestSpriteBridgeState &sprite) const noexcept {
    return find(sprite) != nullptr;
  }

  [[nodiscard]] std::optional<GuestSpriteTexturePlacement>
  relocate(const game::LegacyGuestSpriteBridgeState &sprite,
           unsigned int physical_page) const noexcept {
    const auto *entry = find(sprite);
    if (entry == nullptr || physical_page >= 32U) {
      return std::nullopt;
    }
    const auto source = sourcePlacement(entry->image);
    const auto local_u = static_cast<unsigned int>(sprite.u) - source.u;
    const auto local_v = static_cast<unsigned int>(sprite.v) - source.v;
    const auto page_x = static_cast<std::uint16_t>((physical_page & 15U) * 64U);
    const auto page_y =
        static_cast<std::uint16_t>(physical_page > 15U ? 256U : 0U);
    return GuestSpriteTexturePlacement{
        static_cast<std::uint16_t>(GetTPage(1, 0, page_x, page_y)),
        static_cast<std::uint16_t>(
            GetClut(effect_resident_clut_x, environment_resident_clut_y)),
        static_cast<std::uint8_t>(entry->destination_u + local_u),
        static_cast<std::uint8_t>(entry->destination_v + local_v),
    };
  }

  void upload(unsigned int physical_page) const {
    if (physical_page >= 32U) {
      throw core::Error{core::ErrorCode::invalid_argument,
                        "Environment atlas page is invalid"};
    }
    const auto page_x = static_cast<std::uint16_t>((physical_page & 15U) * 64U);
    const auto page_y =
        static_cast<std::uint16_t>(physical_page > 15U ? 256U : 0U);
    for (const auto &entry : entries_) {
      uploadTimBlockAt(
          entry.image.pixels(),
          static_cast<std::uint16_t>(page_x + entry.destination_u / 2U),
          static_cast<std::uint16_t>(page_y + entry.destination_v));
    }
    uploadTimBlockAt(*entries_.front().image.clut(), effect_resident_clut_x,
                     environment_resident_clut_y);
  }

private:
  struct SourcePlacement {
    unsigned int page{};
    unsigned int u{};
    unsigned int v{};
    unsigned int clut_x{};
    unsigned int clut_y{};
  };

  struct Entry {
    assets::TimImage image;
    std::uint16_t destination_u{};
    std::uint16_t destination_v{};
  };

  [[nodiscard]] static SourcePlacement
  sourcePlacement(const assets::TimImage &image) noexcept {
    const auto page = timTexturePage(image);
    const auto page_x = (page & 15U) * 64U;
    const auto page_y = page > 15U ? 256U : 0U;
    return SourcePlacement{
        page,
        (static_cast<unsigned int>(image.pixels().x) - page_x) * 2U,
        static_cast<unsigned int>(image.pixels().y) - page_y,
        image.clut()->x,
        image.clut()->y,
    };
  }

  [[nodiscard]] const Entry *
  find(const game::LegacyGuestSpriteBridgeState &sprite) const noexcept {
    const auto match = std::ranges::find_if(entries_, [&](const Entry &entry) {
      const auto source = sourcePlacement(entry.image);
      return (sprite.tpage & 0x1fU) == source.page &&
             sprite.center_x == source.clut_x &&
             sprite.center_y == source.clut_y && sprite.u >= source.u &&
             sprite.v >= source.v &&
             static_cast<unsigned int>(sprite.u) + sprite.width <=
                 source.u + entry.image.displayWidth() &&
             static_cast<unsigned int>(sprite.v) + sprite.height <=
                 source.v + entry.image.displayHeight();
    });
    return match == entries_.end() ? nullptr : &*match;
  }

  std::vector<Entry> entries_;
};

struct FireGuestSpriteMatch {
  game::LegacyEffectSpriteFamily family{};
  std::size_t frame{};
  unsigned int source_u{};
  unsigned int source_v{};
};

[[nodiscard]] bool timContainsGuestSprite(
    const assets::TimImage &image,
    const game::LegacyGuestSpriteBridgeState &sprite) noexcept {
  if (!image.clut()) {
    return false;
  }
  const auto page = timTexturePage(image);
  const auto page_x = (page & 15U) * 64U;
  const auto page_y = page > 15U ? 256U : 0U;
  const auto pixels_per_word =
      image.mode() == assets::TimPixelMode::indexed4   ? 4U
      : image.mode() == assets::TimPixelMode::indexed8 ? 2U
                                                       : 1U;
  const auto source_u =
      (static_cast<unsigned int>(image.pixels().x) - page_x) * pixels_per_word;
  const auto source_v = static_cast<unsigned int>(image.pixels().y) - page_y;
  return (sprite.tpage & 0x1fU) == page && sprite.center_x == image.clut()->x &&
         sprite.center_y == image.clut()->y && sprite.u >= source_u &&
         sprite.v >= source_v &&
         static_cast<unsigned int>(sprite.u) + sprite.width <=
             source_u + image.displayWidth() &&
         static_cast<unsigned int>(sprite.v) + sprite.height <=
             source_v + image.displayHeight();
}

[[nodiscard]] std::optional<FireGuestSpriteMatch> matchFireGuestSprite(
    const game::ObjectFireEmitter &fire,
    const game::LegacyGuestSpriteBridgeState &sprite) noexcept {
  const auto match_family = [&](const std::vector<assets::TimImage> &frames,
                                game::LegacyEffectSpriteFamily family)
      -> std::optional<FireGuestSpriteMatch> {
    for (std::size_t index = 0; index < frames.size(); ++index) {
      if (!timContainsGuestSprite(frames[index], sprite)) {
        continue;
      }
      const auto page = timTexturePage(frames[index]);
      return FireGuestSpriteMatch{
          family,
          index,
          (static_cast<unsigned int>(frames[index].pixels().x) -
           (page & 15U) * 64U) *
              2U,
          static_cast<unsigned int>(frames[index].pixels().y) -
              (page > 15U ? 256U : 0U),
      };
    }
    return std::nullopt;
  };
  if (sprite.effect_family != 0U) {
    const auto family =
        static_cast<game::LegacyEffectSpriteFamily>(sprite.effect_family);
    const auto *frames = [&]() -> const std::vector<assets::TimImage> * {
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
    }();
    if (frames == nullptr || sprite.effect_frame >= frames->size() ||
        !timContainsGuestSprite((*frames)[sprite.effect_frame], sprite)) {
      return std::nullopt;
    }
    const auto page = timTexturePage((*frames)[sprite.effect_frame]);
    return FireGuestSpriteMatch{
        family,
        sprite.effect_frame,
        (static_cast<unsigned int>((*frames)[sprite.effect_frame].pixels().x) -
         (page & 15U) * 64U) *
            2U,
        static_cast<unsigned int>((*frames)[sprite.effect_frame].pixels().y) -
            (page > 15U ? 256U : 0U),
    };
  }
  if (const auto match = match_family(fire.fire_frames,
                                      game::LegacyEffectSpriteFamily::fire)) {
    return match;
  }
  if (const auto match = match_family(
          fire.frames, game::LegacyEffectSpriteFamily::explosion)) {
    return match;
  }
  if (const auto match = match_family(fire.breath_frames,
                                      game::LegacyEffectSpriteFamily::breath)) {
    return match;
  }
  return match_family(fire.vapor_frames, game::LegacyEffectSpriteFamily::vapor);
}

std::uint32_t uploadVlf(const game::MissionPackage &mission) {
  const auto bytes = mission.archive().file("VLF.RFF");
  const auto page_mask = validateVlf(bytes);
  streamed_vlf_page_mask = page_mask;

  for (unsigned int page = 0; page < 32U; ++page) {
    if ((page_mask & (1U << page)) == 0) {
      continue;
    }
    uploadTexturePage(page, vlfPage(bytes, page_mask, page));
  }
  uploadClut(vlfClut(bytes, page_mask));
  DrawSync(0);
  return page_mask;
}

std::string texturePageName(unsigned int page) {
  std::string result = "TP";
  if (page < 10U) {
    result.push_back('0');
  }
  result += std::to_string(page);
  result += ".BIN";
  return result;
}

bool hasLegacyHmdBones(const assets::HmdModel &model,
                       const game::SceneObject &object) noexcept;

class TextureStreamer final {
public:
  explicit TextureStreamer(const game::MissionPackage &mission)
      : mission_(mission), environment_atlas_(mission),
        crate_texture_overlay_(mission) {
    page_mask_ = uploadVlf(mission_);
    const auto vlf = mission_.archive().file("VLF.RFF");
    const auto initial_clut = vlfClut(vlf, page_mask_);
    loaded_clut_.assign(initial_clut.begin(), initial_clut.end());
    for (unsigned int page = 0; page < 32U; ++page) {
      if ((page_mask_ & (1U << page)) != 0) {
        const auto physical_page = physicalTexturePage(page);
        loaded_pages_[physical_page] =
            PageState{static_cast<int>(page), shared_bank};
        ++page_generations_[physical_page];
      }
    }
  }

  void ensure(const game::GameplaySession &gameplay,
              std::span<const game::LegacyGuestSpritePresentationState>
                  retained_sprites = {},
              std::span<const GlassShardPresentationState> glass_shards = {}) {
    const auto &models = gameplay.models();
    const auto vlf = mission_.archive().file("VLF.RFF");
    const auto object_bank =
        static_cast<int>(models[gameplay.currentRoom()].scene.textureBank());
    const auto camera = gameplay.camera();
    const auto guest_sprite_bank =
        static_cast<int>(gameplay.textureBankAt(camera.x, camera.z));
    if (requirementsChanged(gameplay, object_bank, guest_sprite_bank,
                            retained_sprites, glass_shards)) {
      required_pages_.fill(unused_bank);
      required_banks_.fill(-1);
      for (auto &sources : required_clut_sources_) {
        sources.fill(false);
      }
      required_active_fire_ = nullptr;
      required_environment_page_.reset();
      required_crate_banks_.fill(false);
      auto requires_environment_atlas = false;
      for (auto &bank_remap : required_page_remap_) {
        for (unsigned int page = 0; page < bank_remap.size(); ++page) {
          bank_remap[page] = physicalTexturePage(page);
        }
      }
      for (auto &bank_remap : required_clut_row_remap_) {
        for (unsigned int row = 0; row < bank_remap.size(); ++row) {
          bank_remap[row] = row;
        }
      }
      auto requirement_context = std::string{"unclassified"};
      const auto require_page = [&](unsigned int source_page, int bank) {
        if (bank < 0 || bank >= 2) {
          return;
        }
        source_page &= 0x1fU;
        const auto source_bank =
            (page_mask_ & (1U << source_page)) != 0 ? shared_bank : bank;
        auto slot = physicalTexturePage(source_page);
        const auto canonical_slot_reserved = slot == effect_resident_page;
        if (canonical_slot_reserved ||
            (required_pages_[slot] >= 0 &&
             (required_pages_[slot] != static_cast<int>(source_page) ||
              required_banks_[slot] != source_bank))) {
          auto existing = required_pages_.size();
          for (unsigned int candidate = 0; candidate < required_pages_.size();
               ++candidate) {
            if (required_pages_[candidate] == static_cast<int>(source_page) &&
                required_banks_[candidate] == source_bank) {
              existing = candidate;
              break;
            }
          }
          if (existing != required_pages_.size()) {
            slot = static_cast<unsigned int>(existing);
          } else {
            // A bank conflict may use any currently free mission-safe page,
            // not only the five framebuffer-tail slots. MUSEUM legitimately
            // keeps enough dual-bank scenery and actors resident to need more
            // than five aliases. Pages 0..5 belong to native presentation;
            // combat effects share unused rows of page 5. Native ZCLUT is
            // relocated out of pages 28..31, making every page 6..31 a safe
            // full-page mission target.
            constexpr std::array alias_pages{
                16U, 17U, 18U, 19U, 20U, 21U, 6U,  7U,  8U,  9U,  10U, 11U, 12U,
                13U, 14U, 15U, 22U, 23U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U,
            };
            const auto available =
                std::ranges::find_if(alias_pages, [&](unsigned int candidate) {
                  return required_pages_[candidate] < 0;
                });
            if (available == alias_pages.end()) {
              auto detail = std::string{"Active scene exhausts the VRAM "
                                        "texture alias pool: source="} +
                            std::to_string(source_page) + "/" +
                            std::to_string(source_bank) +
                            " owner=" + requirement_context + " resident=";
              for (std::size_t resident = 0; resident < required_pages_.size();
                   ++resident) {
                if (required_pages_[resident] < 0) {
                  continue;
                }
                detail += std::to_string(resident) + ":" +
                          std::to_string(required_pages_[resident]) + "/" +
                          std::to_string(required_banks_[resident]) + ",";
              }
              throw core::Error{core::ErrorCode::unsupported,
                                std::move(detail)};
            }
            slot = *available;
          }
        }
        required_page_remap_[static_cast<std::size_t>(bank)][source_page] =
            slot;
        if (required_pages_[slot] < 0) {
          required_pages_[slot] = static_cast<int>(source_page);
          required_banks_[slot] = source_bank;
        } else if (required_pages_[slot] != static_cast<int>(source_page) ||
                   required_banks_[slot] != source_bank) {
          throw core::Error{
              core::ErrorCode::unsupported,
              "Texture alias allocation produced an ownership conflict"};
        }
      };
      const auto require_mask = [&](std::uint32_t mask, int bank) {
        for (unsigned int page = 0; page < 32U; ++page) {
          if ((mask & (1U << page)) != 0U) {
            require_page(page, bank);
          }
        }
      };
      const auto clut_for_bank = [&](int bank) -> std::span<const std::byte> {
        if (bank == 0) {
          // VLF carries the complete bank-zero palette and is byte-identical
          // to VRAM.HOG/ZCLUT.BIN for this mission.
          return vlfClut(vlf, page_mask_);
        }
        return mission_.textureBank(static_cast<std::size_t>(bank))
            .file("ZCLUT.BIN");
      };
      const auto require_clut = [&](std::uint16_t clut,
                                    unsigned int source_page, int bank) {
        if (bank < 0 || bank >= 2) {
          return;
        }
        const auto y = static_cast<unsigned int>(clut >> 6U);
        if (y < mission_clut_source_y || y >= mission_clut_source_y + 32U) {
          return;
        }
        const auto palette_bank =
            (page_mask_ & (1U << (source_page & 0x1fU))) != 0U ? 0 : bank;
        const auto row = y - mission_clut_source_y;
        required_clut_sources_[static_cast<std::size_t>(palette_bank)][row] =
            true;
      };
      const auto require_emd_scene = [&](const assets::EmdScene &scene,
                                         int bank) {
        const auto resolved_mask = scene.resolvedTexturePageMask(page_mask_);
        if (!resolved_mask) {
          throw core::Error{
              core::ErrorCode::unsupported,
              "EMD texture selector has ambiguous source-page ownership"};
        }
        // resolvedTexturePageMask validates authored selector ownership. The
        // exact resident set comes from renderable polygons below; requiring
        // the complete authored mask also retained pages used only by culled
        // or malformed records and could exhaust native alias space.
        for (const auto &section : scene.sections()) {
          for (const auto &polygon : section.polygons) {
            if (!polygon.renderable) {
              continue;
            }
            const auto source_page = assets::resolveEmdTexturePageSource(
                polygon.texture_page, scene.texturePageMask(), page_mask_);
            if (!source_page) {
              throw core::Error{
                  core::ErrorCode::unsupported,
                  "EMD texture selector cannot resolve its source page"};
            }
            require_page(*source_page, bank);
            require_clut(polygon.clut, *source_page, bank);
          }
        }
      };
      for (const auto model_index : gameplay.activeModels()) {
        requirement_context = "world-model-" + std::to_string(model_index);
        const auto &scene = models[model_index].scene;
        const auto bank = scene.textureBank();
        if (bank >= 2U) {
          throw core::Error{core::ErrorCode::unsupported,
                            "Unsupported EMD texture bank"};
        }
        require_emd_scene(scene, bank);
      }
      const auto require_object_geometry =
          [&](const game::ObjectGeometry &geometry, int selected_bank) {
            if (const auto *fire =
                    std::get_if<game::ObjectFireEmitter>(&geometry)) {
              required_active_fire_ = fire;
              const auto validate_source_pages = [&](const auto &frames) {
                for (const auto &frame : frames) {
                  const auto page = timTexturePage(frame);
                  if (page >= loaded_pages_.size() ||
                      physicalTexturePage(page) != page) {
                    throw core::Error{
                        core::ErrorCode::unsupported,
                        "SPFX TIM uses a reserved display texture page"};
                  }
                }
              };
              validate_source_pages(fire->frames);
              validate_source_pages(fire->fire_frames);
              validate_source_pages(fire->breath_frames);
              validate_source_pages(fire->vapor_frames);
              return;
            }
            const auto *gmd = std::get_if<assets::GmdModel>(&geometry);
            const auto *emd = std::get_if<assets::EmdScene>(&geometry);
            const auto *hmd = std::get_if<assets::HmdModel>(&geometry);
            const auto bank = emd != nullptr
                                  ? static_cast<int>(emd->textureBank())
                                  : selected_bank;
            if (bank >= 2U) {
              throw core::Error{core::ErrorCode::unsupported,
                                "Unsupported object texture bank"};
            }
            if (gmd != nullptr) {
              require_mask(gmd->renderableTexturePageMask(), bank);
              for (const auto &triangle : gmd->triangles()) {
                if (triangle.flags == 0U) {
                  continue;
                }
                const auto page = triangle.texture_page & 0x1fU;
                require_clut(triangle.clut, page, bank);
              }
            } else if (hmd != nullptr) {
              require_mask(hmd->texturePageMask(), bank);
              for (const auto &triangle : hmd->triangles()) {
                const auto page = triangle.texture_page & 0x1fU;
                require_clut(triangle.clut, page, bank);
              }
            } else if (emd != nullptr) {
              require_emd_scene(*emd, bank);
            }
          };
      for (const auto object_index : gameplay.activeObjects()) {
        requirement_context =
            "object-" + std::to_string(object_index) + "-geometry";
        const auto bank = gameplay.objectTextureBank(object_index);
        const auto *object_model = gameplay.displayedObjectModel(object_index);
        if (object_model != nullptr) {
          if (object_index < gameplay.objects().size()) {
            const auto class_id = gameplay.objects()[object_index].class_id;
            const auto weapon_crate = object_model->name == "WEPCRATE.GMD" ||
                                      object_model->name == "WEPCRATX.GMD";
            if ((class_id == 0x4fU || class_id == 0x50U) && weapon_crate &&
                bank < 2U) {
              required_crate_banks_[bank] = true;
            }
          }
          const auto *hmd_model =
              std::get_if<assets::HmdModel>(&object_model->geometry);
          if (hmd_model != nullptr &&
              !gameplay.legacyDedicatedActorPresentation(object_index) &&
              !game::legacyHmdRenderAllowed(
                  gameplay.legacyRenderCommandsAuthoritative(),
                  object_index < gameplay.objects().size() &&
                      hasLegacyHmdBones(*hmd_model,
                                        gameplay.objects()[object_index]))) {
            continue;
          }
          require_object_geometry(object_model->geometry, bank);
          const auto *state = gameplay.npcState(object_index);
          auto actor_weapon =
              hmd_model != nullptr
                  ? gameplay.legacyDedicatedActorWeapon(object_index)
                  : std::optional<game::WeaponId>{};
          if (!actor_weapon && hmd_model != nullptr && state != nullptr &&
              state->health != 0U) {
            actor_weapon = state->weapon;
          }
          if (actor_weapon) {
            if (const auto *weapon = gameplay.weaponModel(*actor_weapon)) {
              requirement_context =
                  "object-" + std::to_string(object_index) + "-weapon-" +
                  std::to_string(static_cast<unsigned int>(*actor_weapon));
              require_object_geometry(weapon->geometry, bank);
            }
          }
        }
      }
      const auto player_bank =
          gameplay.textureBankAt(gameplay.player().x, gameplay.player().z);
      requirement_context = "player-model";
      require_object_geometry(gameplay.playerModel().geometry, player_bank);
      const auto require_player_weapon = [&](game::WeaponId weapon_id) {
        const auto *weapon = gameplay.weaponModel(weapon_id);
        if (weapon == nullptr) {
          return;
        }
        requirement_context =
            "player-weapon-" +
            std::to_string(static_cast<unsigned int>(weapon_id));
        require_object_geometry(weapon->geometry, player_bank);
      };
      require_player_weapon(gameplay.hud().inventory().current());
      if (const auto frame = gameplay.legacyPresentationFrame();
          frame && frame->renderer &&
          frame->renderer->state.flashlight_enabled &&
          gameplay.hud().inventory().current() != game::WeaponId::flashlight) {
        // The retail vertex-light record is the authoritative held-tool
        // state. Keep FLASHLT resident even if the interpolated native HUD
        // selection trails the guest by one presentation frame.
        require_player_weapon(game::WeaponId::flashlight);
      }
      const auto needs_dynamic_fire = needsDynamicFire(gameplay);
      if (needs_dynamic_fire) {
        const auto fire = std::ranges::find_if(
            gameplay.objectModels(), [](const game::ObjectModel &model) {
              return std::holds_alternative<game::ObjectFireEmitter>(
                  model.geometry);
            });
        if (fire != gameplay.objectModels().end()) {
          requirement_context = "dynamic-fire";
          require_object_geometry(fire->geometry, object_bank);
        }
      }
      if (const auto frame = gameplay.legacyPresentationFrame();
          frame && frame->renderer) {
        const auto &scrim_state = frame->renderer->state.scrim;
        if (scrim_state.resource_present) {
          const auto *scrim = gameplay.detachedScrimModel();
          if (scrim == nullptr) {
            throw core::Error{core::ErrorCode::not_found,
                              "Resident retail SCRIM.EMD is missing"};
          }
          requirement_context = "retail-scrim";
          require_emd_scene(*scrim, static_cast<int>(scrim->textureBank()));
          const auto require_move_page = [&](int x, int y) {
            const auto page = static_cast<unsigned int>(x / 64) +
                              static_cast<unsigned int>(y / 256) * 16U;
            require_page(page, static_cast<int>(scrim->textureBank()));
          };
          for (const auto &move : scrim_state.vram_moves) {
            requirement_context = "retail-scrim-copy-source";
            require_move_page(move.source_x, move.source_y);
            requirement_context = "retail-scrim-copy-destination";
            require_move_page(move.destination_x, move.destination_y);
          }
        }
        const auto fire_model = std::ranges::find_if(
            gameplay.objectModels(), [](const game::ObjectModel &model) {
              return std::holds_alternative<game::ObjectFireEmitter>(
                  model.geometry);
            });
        const auto *fire_source =
            fire_model == gameplay.objectModels().end()
                ? nullptr
                : &std::get<game::ObjectFireEmitter>(fire_model->geometry);
        const auto require_guest_sprite = [&](const auto &sprite) {
          if ((sprite.attribute & 0x80000000U) != 0U || sprite.width == 0U ||
              sprite.height == 0U) {
            return;
          }
          if (fire_source != nullptr &&
              matchFireGuestSprite(*fire_source, sprite)) {
            required_active_fire_ = fire_source;
            return;
          }
          if (sprite.effect_family != 0U) {
            throw core::Error{
                core::ErrorCode::invalid_format,
                "Retail particle SPRITE provenance does not match SPFX"};
          }
          if (environment_atlas_.contains(sprite)) {
            requires_environment_atlas = true;
            return;
          }
          const auto source_page =
              static_cast<unsigned int>(sprite.tpage & 0x1fU);
          const auto clut = static_cast<std::uint16_t>(
              (static_cast<unsigned int>(sprite.center_y) << 6U) |
              ((static_cast<unsigned int>(sprite.center_x) >> 4U) & 0x3fU));
          requirement_context = "guest-camera-sprite";
          // FUN_800c84f4 builds this list for the retail camera, which can
          // already have crossed a texture-bank portal while currentRoom()
          // still names the previous room. Loading these resources through
          // the room bank and sampling them through the camera bank produces
          // exactly the unrelated palettes and oversized false panels seen
          // during transitions.
          require_page(source_page, guest_sprite_bank);
          require_clut(clut, source_page, guest_sprite_bank);
        };
        for (const auto &sprite : frame->renderer->state.guest_sprites) {
          require_guest_sprite(sprite);
        }
        // The guest runs at 20 Hz while presentation can run at 60 Hz. Glass
        // fragments may be born and expire between two displayed guest
        // snapshots, so their edge queue deliberately retains them for one
        // host frame. Keep the matching VLF page/CLUT resident for that same
        // frame; otherwise the packet survives but samples an evicted alias.
        for (const auto &entry : retained_sprites) {
          require_guest_sprite(entry.sprite);
        }
        for (const auto &shard : glass_shards) {
          const auto source_page =
              static_cast<unsigned int>(shard.texture_page & 0x1fU);
          requirement_context = "glass-shard";
          require_page(source_page, shard.texture_bank);
          require_clut(shard.clut, shard.texture_page, shard.texture_bank);
        }
      }
      if (requires_environment_atlas) {
        constexpr std::array alias_pages{
            16U, 17U, 18U, 19U, 20U, 21U, 6U,  7U,  8U,  9U,  10U, 11U, 12U,
            13U, 14U, 15U, 22U, 23U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U,
        };
        const auto available =
            std::ranges::find_if(alias_pages, [&](unsigned int candidate) {
              return required_pages_[candidate] < 0;
            });
        if (available == alias_pages.end()) {
          throw core::Error{
              core::ErrorCode::unsupported,
              "Active scene exhausts VRAM while reserving retail weather"};
        }
        required_environment_page_ = *available;
        required_pages_[*available] = environment_page_marker;
        required_banks_[*available] = environment_bank;
      }
      const auto base_clut = vlfClut(vlf, page_mask_);
      required_clut_.assign(base_clut.begin(), base_clut.end());
      constexpr std::size_t row_bytes = 256U * 2U;
      std::array<bool, 32> reserved_rows{};
      std::array<bool, 32> occupied_rows{};
      for (std::size_t row = 0; row < reserved_rows.size(); ++row) {
        reserved_rows[row] =
            required_clut_sources_[0][row] || required_clut_sources_[1][row];
      }
      const auto assign_clut = [&](std::size_t logical_row, int bank,
                                   std::size_t physical_row) {
        required_clut_row_remap_[static_cast<std::size_t>(bank)][logical_row] =
            static_cast<unsigned int>(physical_row);
        const auto source =
            clut_for_bank(bank).subspan(logical_row * row_bytes, row_bytes);
        std::copy(source.begin(), source.end(),
                  required_clut_.begin() + physical_row * row_bytes);
        occupied_rows[physical_row] = true;
      };
      for (std::size_t row = 0; row < reserved_rows.size(); ++row) {
        if (!reserved_rows[row]) {
          continue;
        }
        const auto preferred_bank =
            object_bank >= 0 && object_bank < 2 &&
                    required_clut_sources_[static_cast<std::size_t>(
                        object_bank)][row]
                ? object_bank
            : required_clut_sources_[0][row] ? 0
                                             : 1;
        assign_clut(row, preferred_bank, row);

        const auto other_bank = 1 - preferred_bank;
        if (!required_clut_sources_[static_cast<std::size_t>(other_bank)]
                                   [row]) {
          continue;
        }
        const auto preferred =
            clut_for_bank(preferred_bank).subspan(row * row_bytes, row_bytes);
        const auto other =
            clut_for_bank(other_bank).subspan(row * row_bytes, row_bytes);
        if (std::equal(preferred.begin(), preferred.end(), other.begin(),
                       other.end())) {
          required_clut_row_remap_[static_cast<std::size_t>(other_bank)][row] =
              static_cast<unsigned int>(row);
          continue;
        }

        auto escape = reserved_rows.size();
        while (escape > 0U) {
          --escape;
          if (!reserved_rows[escape] && !occupied_rows[escape]) {
            break;
          }
        }
        if (reserved_rows[escape] || occupied_rows[escape]) {
          throw core::Error{core::ErrorCode::unsupported,
                            "Active scene exhausts the VRAM CLUT alias pool"};
        }
        assign_clut(row, other_bank, escape);
      }
      for (std::size_t bank = 0U; bank < required_crate_banks_.size(); ++bank) {
        if (!required_crate_banks_[bank]) {
          continue;
        }
        const auto physical_row = required_clut_row_remap_
            [bank][RetailCrateTextureOverlay::sourceClutRow()];
        crate_texture_overlay_.copyClut(
            std::span<std::byte>{required_clut_}.subspan(
                physical_row * row_bytes, row_bytes));
      }
      // A slot which leaves the active requirement set no longer has a
      // trustworthy resident owner. Native effect/framebuffer scratch may use
      // it before the same logical page is requested again. Forget that stale
      // claim now so a later re-entry always uploads the authored page.
      for (std::size_t slot = 0; slot < required_pages_.size(); ++slot) {
        if (required_pages_[slot] < 0) {
          loaded_pages_[slot] = PageState{};
        }
      }
      requirements_valid_ = true;
    }

    // Rendering reads this legacy lookup directly. Re-publish the cached
    // requirement result every frame so transient VRAM ownership invalidation
    // can never leave geometry sampling a stale logical-to-physical mapping.
    streamed_texture_page_remap = required_page_remap_;
    streamed_clut_row_remap = required_clut_row_remap_;

    auto changed = false;
    for (unsigned int slot = 0; slot < 32U; ++slot) {
      const auto page = required_pages_[slot];
      const auto bank = required_banks_[slot];
      if (page < 0 || (loaded_pages_[slot].logical_page == page &&
                       loaded_pages_[slot].bank == bank)) {
        continue;
      }
      if (bank == environment_bank) {
        environment_atlas_.upload(slot);
      } else if (bank == shared_bank) {
        uploadTexturePageAt(
            slot, vlfPage(vlf, page_mask_, static_cast<unsigned int>(page)));
      } else {
        uploadTexturePageAt(
            slot, mission_.textureBank(static_cast<std::size_t>(bank))
                      .file(texturePageName(static_cast<unsigned int>(page))));
      }
      loaded_pages_[slot] = PageState{page, bank};
      ++page_generations_[slot];
      changed = true;
    }

    if (loaded_clut_ != required_clut_) {
      uploadClut(required_clut_);
      loaded_clut_ = required_clut_;
      changed = true;
    }
    for (std::size_t bank = 0U; bank < required_crate_banks_.size(); ++bank) {
      if (!required_crate_banks_[bank]) {
        crate_overlay_tokens_[bank] = {};
        continue;
      }
      const auto trim_page = residentPhysicalPage(12U, static_cast<int>(bank));
      const auto top_page = residentPhysicalPage(13U, static_cast<int>(bank));
      if (!trim_page || !top_page) {
        throw core::Error{core::ErrorCode::unsupported,
                          "Resident weapon-crate texture pages are missing"};
      }
      const std::array tokens{
          page_generations_[*trim_page],
          page_generations_[*top_page],
      };
      if (crate_overlay_tokens_[bank] != tokens) {
        crate_texture_overlay_.upload(*trim_page, *top_page);
        crate_overlay_tokens_[bank] = tokens;
        changed = true;
      }
    }
    if (active_fire_ != required_active_fire_) {
      active_fire_ = required_active_fire_;
      fire_placement_ = active_fire_ != nullptr
                            ? uploadFireTextureAtlas(*active_fire_)
                            : FireTexturePlacement{};
      changed = changed || active_fire_ != nullptr;
    }
    if (changed) {
      DrawSync(0);
    }
  }

  void invalidate() noexcept {
    // Only resident VRAM ownership is lost here. Source requirements remain
    // valid until requirementsChanged observes an exact gameplay input change.
    loaded_pages_.fill(PageState{});
    loaded_clut_.clear();
    fire_placement_ = {};
    active_fire_ = nullptr;
    crate_overlay_tokens_ = {};
  }

  void invalidatePhysicalPage(unsigned int physical_page) noexcept {
    if (physical_page < loaded_pages_.size()) {
      loaded_pages_[physical_page] = PageState{};
    }
  }

  [[nodiscard]] std::optional<unsigned int>
  residentPhysicalPage(unsigned int logical_page,
                       int texture_bank) const noexcept {
    if (!requirements_valid_ || logical_page >= 32U || texture_bank < 0 ||
        texture_bank >= 2) {
      return std::nullopt;
    }
    const auto bank = static_cast<std::size_t>(texture_bank);
    const auto physical_page = required_page_remap_[bank][logical_page];
    if (physical_page >= required_pages_.size()) {
      return std::nullopt;
    }
    const auto expected_bank =
        (page_mask_ & (std::uint32_t{1U} << logical_page)) != 0U ? shared_bank
                                                                 : texture_bank;
    if (required_pages_[physical_page] != static_cast<int>(logical_page) ||
        required_banks_[physical_page] != expected_bank ||
        loaded_pages_[physical_page].logical_page !=
            static_cast<int>(logical_page) ||
        loaded_pages_[physical_page].bank != expected_bank) {
      return std::nullopt;
    }
    return physical_page;
  }

  [[nodiscard]] std::optional<std::uint64_t>
  residentPageToken(unsigned int logical_page,
                    int texture_bank) const noexcept {
    const auto physical_page = residentPhysicalPage(logical_page, texture_bank);
    if (!physical_page) {
      return std::nullopt;
    }
    return (page_generations_[*physical_page] << 5U) | *physical_page;
  }

  [[nodiscard]] bool reloadResidentPage(unsigned int logical_page,
                                        int texture_bank) {
    const auto physical_page = residentPhysicalPage(logical_page, texture_bank);
    if (!physical_page) {
      return false;
    }
    if ((page_mask_ & (std::uint32_t{1U} << logical_page)) != 0U) {
      const auto vlf = mission_.archive().file("VLF.RFF");
      uploadTexturePageAt(*physical_page,
                          vlfPage(vlf, page_mask_, logical_page));
    } else {
      uploadTexturePageAt(
          *physical_page,
          mission_.textureBank(static_cast<std::size_t>(texture_bank))
              .file(texturePageName(logical_page)));
    }
    ++page_generations_[*physical_page];
    return true;
  }

  [[nodiscard]] std::optional<game::EffectTextureCopy>
  relocateEffectTextureCopy(const game::EffectTextureCopy &copy,
                            int texture_bank) const noexcept {
    if (!requirements_valid_ || texture_bank < 0 || texture_bank >= 2) {
      return std::nullopt;
    }
    const auto relocate =
        [&](int x, int y, int width,
            int height) -> std::optional<game::EffectVramRect> {
      if (x < 0 || y < 0 || width <= 0 || height <= 0 || x >= 1024 ||
          y >= 512) {
        return std::nullopt;
      }
      const auto logical_page = static_cast<unsigned int>(x / 64) +
                                (static_cast<unsigned int>(y / 256) * 16U);
      const auto local_x = x & 63;
      const auto local_y = y & 255;
      if (local_x + width > 64 || local_y + height > 256) {
        return std::nullopt;
      }
      const auto physical_page =
          residentPhysicalPage(logical_page, texture_bank);
      if (!physical_page) {
        return std::nullopt;
      }
      return game::EffectVramRect{
          static_cast<std::int16_t>((*physical_page & 15U) * 64U +
                                    static_cast<unsigned int>(local_x)),
          static_cast<std::int16_t>((*physical_page > 15U ? 256U : 0U) +
                                    static_cast<unsigned int>(local_y)),
          static_cast<std::int16_t>(width),
          static_cast<std::int16_t>(height),
      };
    };

    const auto source = relocate(copy.source.x, copy.source.y,
                                 copy.source.width, copy.source.height);
    const auto destination = relocate(copy.destination_x, copy.destination_y,
                                      copy.source.width, copy.source.height);
    if (!source || !destination) {
      return std::nullopt;
    }
    return game::EffectTextureCopy{
        *source,
        destination->x,
        destination->y,
    };
  }

  [[nodiscard]] const FireTexturePlacement &firePlacement() const noexcept {
    return fire_placement_;
  }

  [[nodiscard]] std::optional<GuestSpriteTexturePlacement> relocateGuestSprite(
      const game::LegacyGuestSpriteBridgeState &sprite) const noexcept {
    if (active_fire_ != nullptr) {
      if (const auto match = matchFireGuestSprite(*active_fire_, sprite)) {
        if (const auto *placement =
                fire_placement_.frame(match->family, match->frame)) {
          return GuestSpriteTexturePlacement{
              placement->texture_page,
              placement->clut,
              static_cast<std::uint8_t>(
                  placement->minimum_u +
                  (static_cast<unsigned int>(sprite.u) - match->source_u)),
              static_cast<std::uint8_t>(
                  placement->minimum_v +
                  (static_cast<unsigned int>(sprite.v) - match->source_v)),
          };
        }
      }
    }
    if (required_environment_page_) {
      if (const auto environment = environment_atlas_.relocate(
              sprite, *required_environment_page_)) {
        return environment;
      }
    }
    return std::nullopt;
  }

private:
  struct PageState {
    int logical_page{unused_bank};
    int bank{unused_bank};
  };

  static constexpr int shared_bank = -2;
  static constexpr int unused_bank = -3;
  static constexpr int environment_bank = -4;
  static constexpr int environment_page_marker = 32;

  enum class RequirementInputKind : std::uint8_t {
    room,
    object_bank,
    guest_sprite_bank,
    active_model,
    active_object,
    object_texture_bank,
    displayed_model,
    object_render_eligible,
    npc_weapon,
    dedicated_actor_weapon,
    player_model,
    player_texture_bank,
    player_weapon,
    dynamic_fire,
    guest_sprite_material,
    guest_sprite_rectangle,
    guest_sprite_effect,
    glass_shard_material,
    retail_scrim,
    retail_scrim_move,
  };

  struct RequirementInput {
    RequirementInputKind kind{};
    std::uintptr_t value{};

    friend bool operator==(const RequirementInput &,
                           const RequirementInput &) = default;
  };

  [[nodiscard]] static bool
  needsDynamicFire(const game::GameplaySession &gameplay) noexcept {
    return !gameplay.legacyExplParticles().empty() ||
           !gameplay.legacyPark2FlamethrowerRibbons().empty() ||
           gameplay.hud().inventory().current() ==
               game::WeaponId::flamethrower ||
           std::ranges::any_of(gameplay.projectiles(),
                               [](const game::GameplayProjectile &projectile) {
                                 return projectile.active &&
                                        projectile.phase ==
                                            game::ProjectilePhase::explosion;
                               }) ||
           std::ranges::any_of(
               gameplay.effects(), [](const game::GameplayEffect &effect) {
                 return effect.type == game::GameplayEffectType::explosion ||
                        effect.type == game::GameplayEffectType::burning_fire;
               });
  }

  [[nodiscard]] bool requirementsChanged(
      const game::GameplaySession &gameplay, int object_bank,
      int guest_sprite_bank,
      std::span<const game::LegacyGuestSpritePresentationState>
          retained_sprites,
      std::span<const GlassShardPresentationState> glass_shards) {
    scratch_requirement_inputs_.clear();
    const auto append = [this](RequirementInputKind kind,
                               std::uintptr_t value) {
      scratch_requirement_inputs_.push_back(RequirementInput{kind, value});
    };
    const auto address = [](const void *value) noexcept {
      return reinterpret_cast<std::uintptr_t>(value);
    };

    append(RequirementInputKind::room, gameplay.currentRoom());
    append(RequirementInputKind::object_bank,
           static_cast<std::uintptr_t>(object_bank));
    append(RequirementInputKind::guest_sprite_bank,
           static_cast<std::uintptr_t>(guest_sprite_bank));
    for (const auto &shard : glass_shards) {
      const auto material =
          static_cast<std::uintptr_t>(shard.texture_page) |
          (static_cast<std::uintptr_t>(shard.clut) << 16U) |
          (static_cast<std::uintptr_t>(shard.texture_bank) << 32U);
      append(RequirementInputKind::glass_shard_material, material);
    }
    for (const auto model : gameplay.activeModels()) {
      append(RequirementInputKind::active_model, model);
    }
    for (const auto object : gameplay.activeObjects()) {
      append(RequirementInputKind::active_object, object);
      append(RequirementInputKind::object_texture_bank,
             gameplay.objectTextureBank(object));
      append(RequirementInputKind::displayed_model,
             address(gameplay.displayedObjectModel(object)));
      const auto *displayed = gameplay.displayedObjectModel(object);
      const auto *hmd =
          displayed != nullptr
              ? std::get_if<assets::HmdModel>(&displayed->geometry)
              : nullptr;
      append(RequirementInputKind::object_render_eligible,
             (hmd == nullptr ||
              gameplay.legacyDedicatedActorPresentation(object) ||
              game::legacyHmdRenderAllowed(
                  gameplay.legacyRenderCommandsAuthoritative(),
                  object < gameplay.objects().size() &&
                      hasLegacyHmdBones(*hmd, gameplay.objects()[object])))
                 ? 1U
                 : 0U);
      if (const auto *npc = gameplay.npcState(object)) {
        append(RequirementInputKind::npc_weapon,
               static_cast<std::uintptr_t>(npc->weapon));
      }
      if (const auto weapon = gameplay.legacyDedicatedActorWeapon(object)) {
        append(RequirementInputKind::dedicated_actor_weapon,
               static_cast<std::uintptr_t>(*weapon));
      }
    }
    append(RequirementInputKind::player_model,
           address(&gameplay.playerModel()));
    append(RequirementInputKind::player_texture_bank,
           gameplay.textureBankAt(gameplay.player().x, gameplay.player().z));
    append(RequirementInputKind::player_weapon,
           static_cast<std::uintptr_t>(gameplay.hud().inventory().current()));
    append(RequirementInputKind::dynamic_fire,
           needsDynamicFire(gameplay) ? 1U : 0U);
    if (const auto frame = gameplay.legacyPresentationFrame();
        frame && frame->renderer) {
      const auto &scrim = frame->renderer->state.scrim;
      append(RequirementInputKind::retail_scrim,
             (scrim.resource_present ? 1U : 0U) | (scrim.visible ? 2U : 0U));
      for (const auto &move : scrim.vram_moves) {
        const auto source_page =
            static_cast<std::uintptr_t>(move.source_x / 64) +
            static_cast<std::uintptr_t>(move.source_y / 256) * 16U;
        const auto destination_page =
            static_cast<std::uintptr_t>(move.destination_x / 64) +
            static_cast<std::uintptr_t>(move.destination_y / 256) * 16U;
        append(RequirementInputKind::retail_scrim_move,
               source_page | (destination_page << 5U));
      }
      const auto append_guest_sprite = [&](const auto &sprite) {
        append(RequirementInputKind::guest_sprite_material,
               static_cast<std::uintptr_t>(sprite.tpage) |
                   (static_cast<std::uintptr_t>(sprite.center_x) << 16U) |
                   (static_cast<std::uintptr_t>(sprite.center_y) << 32U));
        append(RequirementInputKind::guest_sprite_rectangle,
               static_cast<std::uintptr_t>(sprite.u) |
                   (static_cast<std::uintptr_t>(sprite.v) << 8U) |
                   (static_cast<std::uintptr_t>(sprite.width) << 16U) |
                   (static_cast<std::uintptr_t>(sprite.height) << 32U));
        append(RequirementInputKind::guest_sprite_effect,
               static_cast<std::uintptr_t>(sprite.effect_family) |
                   (static_cast<std::uintptr_t>(sprite.effect_frame) << 8U));
      };
      for (const auto &sprite : frame->renderer->state.guest_sprites) {
        append_guest_sprite(sprite);
      }
      for (const auto &entry : retained_sprites) {
        append_guest_sprite(entry.sprite);
      }
    }

    // Exact source identity, not a hash, is the correctness boundary. This
    // mirrors every resource-selecting branch in ensure: room visibility,
    // displayed object/model identity, every actor/player/projectile weapon,
    // the dynamic-fire need and every field which selects a guest-sprite TIM.
    // Camera/animation and reset-to-an-equivalent state safely reuse the key.
    if (requirements_valid_ &&
        scratch_requirement_inputs_ == requirement_inputs_) {
      return false;
    }
    requirement_inputs_.swap(scratch_requirement_inputs_);
    return true;
  }

  const game::MissionPackage &mission_;
  EnvironmentTextureAtlas environment_atlas_;
  RetailCrateTextureOverlay crate_texture_overlay_;
  std::uint32_t page_mask_{};
  std::array<int, 32> required_pages_{};
  std::array<int, 32> required_banks_{};
  std::array<std::array<bool, 32>, 2> required_clut_sources_{};
  std::array<std::array<unsigned int, 32>, 2> required_page_remap_{};
  std::array<std::array<unsigned int, 32>, 2> required_clut_row_remap_{};
  std::vector<std::byte> required_clut_;
  std::vector<RequirementInput> requirement_inputs_;
  std::vector<RequirementInput> scratch_requirement_inputs_;
  const game::ObjectFireEmitter *required_active_fire_{};
  std::optional<unsigned int> required_environment_page_;
  std::array<bool, 2U> required_crate_banks_{};
  bool requirements_valid_{};
  std::array<PageState, 32> loaded_pages_{};
  std::array<std::uint64_t, 32> page_generations_{};
  std::vector<std::byte> loaded_clut_;
  FireTexturePlacement fire_placement_{};
  const game::ObjectFireEmitter *active_fire_{};
  std::array<std::array<std::uint64_t, 2U>, 2U> crate_overlay_tokens_{};
};

struct HudTextureAsset {
  std::string name;
  assets::TimImage image;
};

[[nodiscard]] bool isNativeHdFontSheet(std::string_view name,
                                       const assets::TimImage &image) noexcept {
  if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut() ||
      image.pixels().y != 0U) {
    return false;
  }
  if (name == "FONTA.TIM") {
    return image.pixels().x == 832U && image.displayWidth() == 64U &&
           image.displayHeight() == 128U;
  }
  if (name == "FONTB.TIM") {
    return image.pixels().x == 864U && image.displayWidth() == 64U &&
           image.displayHeight() == 128U;
  }
  if (name == "FONTC.TIM") {
    return image.pixels().x == 896U && image.displayWidth() == 84U &&
           image.displayHeight() == 246U;
  }
  return false;
}

class HudTextureAtlas final {
public:
  explicit HudTextureAtlas(const game::MissionPackage &mission) {
    const auto required = [](std::string_view name) {
      // The three font sheets and SYMBOL are shared by the original
      // gameplay and pause interfaces. Keep them in the same restored
      // VRAM atlas as the weapon silhouettes.
      if (name == "FONTA.TIM" || name == "FONTB.TIM" || name == "FONTC.TIM" ||
          name == "SYMBOL.TIM" || name == "SCOPED.TIM" || name == "SCP0.TIM" ||
          name == "SCP45.TIM" || name == "SCP90.TIM" || name == "SCP135.TIM" ||
          name == "SCP180.TIM" || name == "SCP225.TIM" ||
          name == "SCP270.TIM" || name == "SCP315.TIM" ||
          name == "DANGER.TIM" || name == "TARGET.TIM" || name == "ARMOR.TIM") {
        return true;
      }
      if (std::ranges::find(nightvision_scope_layers, name) !=
          nightvision_scope_layers.end()) {
        return true;
      }
      for (std::size_t index = 0; index < game::weapon_slot_count; ++index) {
        for (const auto layer :
             game::droppedItemIconLayers(static_cast<std::uint16_t>(index))) {
          if (name == layer) {
            return true;
          }
        }
      }
      return false;
    };
    for (const auto &entry : mission.interfaceAssets().entries()) {
      if (!required(entry.name)) {
        continue;
      }
      auto localized = (entry.name == "FONTA.TIM" ||
                        entry.name == "FONTB.TIM" || entry.name == "FONTC.TIM")
                           ? game::readLocalizedAsset("fonts/" + entry.name)
                           : std::nullopt;
      auto image = localized ? assets::TimImage::parse(*localized)
                             : assets::TimImage::parse(
                                   mission.interfaceAssets().file(entry.name));
      const auto native_hd_font_layout =
          localized.has_value() && isNativeHdFontSheet(entry.name, image);
      const auto nightvision_layer =
          std::ranges::find(nightvision_scope_layers, entry.name) !=
          nightvision_scope_layers.end();
      const auto regular_hud_layout =
          image.pixels().x >= 768U && image.pixels().y < 256U &&
          static_cast<unsigned int>(image.pixels().x) +
                  image.pixels().width_words <=
              1024U &&
          static_cast<unsigned int>(image.pixels().y) + image.pixels().height <=
              mission_clut_resident_y;
      const auto nightvision_layout =
          nightvision_layer && image.pixels().x >= 512U &&
          image.pixels().y >= 384U &&
          static_cast<unsigned int>(image.pixels().x) +
                  image.pixels().width_words <=
              576U &&
          static_cast<unsigned int>(image.pixels().y) + image.pixels().height <=
              502U;
      if (image.mode() != assets::TimPixelMode::indexed8 || !image.clut() ||
          (!regular_hud_layout && !nightvision_layout &&
           !native_hd_font_layout) ||
          image.clut()->x != 768U || image.clut()->y != 483U ||
          image.clut()->width_words != 256U || image.clut()->height != 1U) {
        throw core::Error{core::ErrorCode::invalid_format,
                          "INTERFACE HUD atlas uses an unexpected VRAM layout"};
      }
      if (!assets_.empty() &&
          image.clut()->words != assets_.front().image.clut()->words) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "INTERFACE HUD assets do not share the original palette"};
      }
      assets_.push_back(HudTextureAsset{entry.name, std::move(image)});
    }
    auto armor_pickup = assets::TimImage::parse(
        mission.specialEffects().file(armor_pickup_texture));
    const auto secondary_spfx_palette =
        assets::TimImage::parse(mission.specialEffects().file("BLOT.TIM"));
    if (armor_pickup.mode() != assets::TimPixelMode::indexed8 ||
        !armor_pickup.clut() || armor_pickup.displayWidth() != 32U ||
        armor_pickup.pixels().height != 32U ||
        armor_pickup.clut()->width_words != 256U ||
        armor_pickup.clut()->height != 1U ||
        armor_pickup.clut()->words.empty() || !secondary_spfx_palette.clut() ||
        armor_pickup.clut()->words != secondary_spfx_palette.clut()->words) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "SPFX armour pickup uses an unexpected TIM layout"};
    }
    armor_pickup_palette_ = *armor_pickup.clut();
    armor_pickup_palette_.words.front() = 0U;
    armor_pickup_pixels_ = armor_pickup.pixels();
    const auto keep_vest_pixel = [](std::uint16_t x, std::uint16_t y) {
      if (y < 4U) {
        return (x >= 7U && x <= 12U) || (x >= 19U && x <= 24U);
      }
      if (y < 8U) {
        return (x >= 4U && x <= 12U) || (x >= 19U && x <= 27U);
      }
      if (y < 12U) {
        return x >= 2U && x <= 29U;
      }
      if (y < 26U) {
        return x >= 5U && x <= 26U;
      }
      if (y < 30U) {
        return x >= 7U && x <= 24U;
      }
      return x >= 9U && x <= 22U;
    };
    for (std::uint16_t y = 0U; y < armor_pickup_pixels_.height; ++y) {
      for (std::uint16_t x = 0U; x < armor_pickup.displayWidth(); ++x) {
        if (keep_vest_pixel(x, y)) {
          continue;
        }
        auto &word =
            armor_pickup_pixels_.words[static_cast<std::size_t>(y) *
                                           armor_pickup_pixels_.width_words +
                                       x / 2U];
        word = x % 2U == 0U ? static_cast<std::uint16_t>(word & 0xff00U)
                            : static_cast<std::uint16_t>(word & 0x00ffU);
      }
    }
    assets_.push_back(HudTextureAsset{std::string{armor_pickup_texture},
                                      std::move(armor_pickup)});
    if (assets_.empty()) {
      throw core::Error{core::ErrorCode::not_found,
                        "INTERFACE HUD atlas is empty"};
    }
    static_cast<void>(image("FONTA.TIM"));
    static_cast<void>(image("FONTB.TIM"));
    static_cast<void>(image("FONTC.TIM"));
    static_cast<void>(image("SYMBOL.TIM"));
    static_cast<void>(image("SCOPED.TIM"));
    for (const auto bearing : scope_bearings) {
      static_cast<void>(image(bearing));
    }
    for (const auto layer : nightvision_scope_layers) {
      static_cast<void>(image(layer));
    }
    if (game::russianLanguageActive()) {
      native_font_ = std::make_unique<PsyCrossFontTexture>(
          image("FONTA.TIM"), image("FONTB.TIM"), image("FONTC.TIM"));
    }
  }

  [[nodiscard]] const PsyCrossFontTexture *nativeFont() const noexcept {
    return native_font_.get();
  }

  void invalidate() const noexcept {
    gameplay_state_.reset();
    gameplay_font_resident_ = false;
    gameplay_palette_resident_ = false;
  }

  void restoreGameplay(
      const game::GameplayHud &hud, bool first_person_aim,
      std::span<const game::LegacyDroppedItemBridgeState> dropped_items,
      std::span<const game::GameplayProjectile> projectiles) const {
    const auto weapon = hud.inventory().current();
    const auto scoped_weapon = weapon == game::WeaponId::nightvision_rifle ||
                               weapon == game::WeaponId::sniper_rifle;
    const auto menu_visible = hud.weaponMenuFrames() != 0U;
    auto dropped_item_mask = std::uint32_t{};
    for (const auto &item : dropped_items) {
      if (item.item < game::weapon_slot_count) {
        dropped_item_mask |= std::uint32_t{1U} << item.item;
      } else if (item.item == 0x80U) {
        dropped_item_mask |= std::uint32_t{1U} << game::weapon_slot_count;
      }
    }
    auto projectile_mask = std::uint32_t{};
    for (const auto &projectile : projectiles) {
      if (projectile.active &&
          projectile.phase == game::ProjectilePhase::flying &&
          (projectile.weapon == game::WeaponId::fragmentation_grenade ||
           projectile.weapon == game::WeaponId::gas_grenade)) {
        projectile_mask |= std::uint32_t{1U}
                           << static_cast<std::size_t>(projectile.weapon);
      }
    }
    const auto state = GameplayResidentState{
        weapon,
        menu_visible,
        menu_visible
            ? hud.weaponMenuWindow()
            : std::array<game::WeaponId, game::weapon_menu_slot_count>{},
        first_person_aim && scoped_weapon,
        dropped_item_mask,
        projectile_mask,
    };
    if (gameplay_state_ && *gameplay_state_ == state &&
        gameplay_font_resident_ && gameplay_palette_resident_) {
      return;
    }

    auto uploaded = false;
    const auto upload = [&](std::string_view name) {
      if (name == armor_pickup_texture) {
        uploadTimBlockAt(armor_pickup_pixels_, pickup_resident_x,
                         pickup_resident_y);
      } else {
        uploadHudPixels(image(name).pixels());
      }
      uploaded = true;
    };
    const auto upload_nightvision = [&](std::size_t index) {
      uploadTimBlockAt(image(nightvision_scope_layers[index]).pixels(),
                       nightvision_resident_x[index], nightvision_resident_y);
      uploaded = true;
    };
    // Retail glyph UVs address one page assembled from all three font TIMs.
    // The world pass owns the same transient framebuffer-backed VRAM area, so
    // restoring only FONTA (and only for ammo weapons) leaves glyphs from the
    // other two sheets sampling wall/effect pixels after any gameplay frame.
    // Upload the complete authored font page once here and keep the single
    // palette upload/DrawSync below as the batch boundary.
    if (!gameplay_font_resident_) {
      if (native_font_ == nullptr) {
        upload("FONTA.TIM");
        upload("FONTB.TIM");
        upload("FONTC.TIM");
      }
      gameplay_font_resident_ = true;
    }
    const auto &definition = hud.inventory().currentDefinition();
    for (const auto layer : definition.icon.layers()) {
      upload(layer);
    }
    if (menu_visible) {
      for (const auto menu_weapon : state.menu_window) {
        for (const auto layer :
             game::weaponDefinition(menu_weapon).icon.layers()) {
          upload(layer);
        }
      }
    }
    for (std::size_t item = 0U; item < game::weapon_slot_count; ++item) {
      if ((dropped_item_mask & (std::uint32_t{1U} << item)) == 0U) {
        continue;
      }
      for (const auto layer :
           game::droppedItemIconLayers(static_cast<std::uint16_t>(item))) {
        upload(layer);
      }
    }
    if ((dropped_item_mask & (std::uint32_t{1U} << game::weapon_slot_count)) !=
        0U) {
      upload(armor_pickup_texture);
      uploadTimBlockAt(armor_pickup_palette_, hud_resident_clut_x,
                       pickup_resident_clut_y);
    }
    for (std::size_t weapon_index = 0U; weapon_index < game::weapon_slot_count;
         ++weapon_index) {
      if ((projectile_mask & (std::uint32_t{1U} << weapon_index)) == 0U) {
        continue;
      }
      for (const auto layer :
           game::weaponDefinition(static_cast<game::WeaponId>(weapon_index))
               .icon.layers()) {
        upload(layer);
      }
    }
    if (state.scoped) {
      if (weapon == game::WeaponId::nightvision_rifle) {
        for (std::size_t index = 0; index < nightvision_scope_layers.size();
             ++index) {
          upload_nightvision(index);
        }
      } else {
        upload("SCOPED.TIM");
        for (const auto bearing : scope_bearings) {
          upload(bearing);
        }
      }
    }
    if (!gameplay_palette_resident_) {
      uploadTimBlockAt(*assets_.front().image.clut(), hud_resident_clut_x,
                       hud_resident_clut_y);
      gameplay_palette_resident_ = true;
      uploaded = true;
    }
    if (uploaded) {
      DrawSync(0);
    }
    gameplay_state_ = state;
  }

  void restoreFont() const {
    constexpr std::array names{
        std::string_view{"FONTA.TIM"},
        std::string_view{"FONTB.TIM"},
        std::string_view{"FONTC.TIM"},
        std::string_view{"SYMBOL.TIM"},
    };
    for (const auto name : names) {
      if (native_font_ != nullptr && name.starts_with("FONT")) {
        continue;
      }
      uploadHudPixels(image(name).pixels());
    }
    uploadTimBlockAt(*image("FONTA.TIM").clut(), hud_resident_clut_x,
                     hud_resident_clut_y);
    DrawSync(0);
  }

  void restoreWeaponIcon(game::WeaponId weapon) const {
    for (const auto layer :
         game::droppedItemIconLayers(static_cast<std::uint16_t>(weapon))) {
      uploadHudPixels(image(layer).pixels());
    }
    uploadTimBlockAt(*image("FONTA.TIM").clut(), hud_resident_clut_x,
                     hud_resident_clut_y);
    DrawSync(0);
  }

  [[nodiscard]] const assets::TimImage *
  tryImage(std::string_view name) const noexcept {
    const auto match =
        std::ranges::find_if(assets_, [&](const HudTextureAsset &asset) {
          return asset.name == name;
        });
    return match == assets_.end() ? nullptr : &match->image;
  }

  [[nodiscard]] const assets::TimImage &image(std::string_view name) const {
    const auto *result = tryImage(name);
    if (result == nullptr) {
      throw core::Error{core::ErrorCode::not_found,
                        "HUD texture is missing from the retail atlases: " +
                            std::string{name}};
    }
    return *result;
  }

  [[nodiscard]] std::optional<std::uint16_t>
  fontPaletteWord(std::uint8_t page_u, std::uint8_t page_v) const noexcept {
    const auto &font_page = image("FONTA.TIM");
    const auto page_x = static_cast<int>(font_page.pixels().x &
                                         static_cast<std::uint16_t>(~63U));
    const auto page_y = static_cast<int>(font_page.pixels().y &
                                         static_cast<std::uint16_t>(~255U));
    constexpr std::array names{
        std::string_view{"FONTA.TIM"},
        std::string_view{"FONTB.TIM"},
        std::string_view{"FONTC.TIM"},
    };
    for (const auto name : names) {
      const auto &font = image(name);
      const auto &pixels = font.pixels();
      const auto first_u = (static_cast<int>(pixels.x) - page_x) * 2;
      const auto first_v = static_cast<int>(pixels.y) - page_y;
      const auto local_u = static_cast<int>(page_u) - first_u;
      const auto local_v = static_cast<int>(page_v) - first_v;
      if (local_u < 0 || local_v < 0 ||
          local_u >= static_cast<int>(font.displayWidth()) ||
          local_v >= static_cast<int>(font.displayHeight())) {
        continue;
      }
      const auto word =
          pixels.words[static_cast<std::size_t>(local_v) * pixels.width_words +
                       static_cast<std::size_t>(local_u / 2)];
      const auto palette_index = static_cast<std::uint8_t>(
          local_u % 2 == 0 ? word & 0xffU : word >> 8U);
      return font.clut()->words[palette_index];
    }
    return std::nullopt;
  }

private:
  struct GameplayResidentState {
    game::WeaponId weapon{};
    bool menu_visible{};
    std::array<game::WeaponId, game::weapon_menu_slot_count> menu_window{};
    bool scoped{};
    std::uint32_t dropped_item_mask{};
    std::uint32_t projectile_mask{};

    [[nodiscard]] friend bool
    operator==(const GameplayResidentState &,
               const GameplayResidentState &) noexcept = default;
  };

  std::vector<HudTextureAsset> assets_;
  std::unique_ptr<PsyCrossFontTexture> native_font_;
  assets::TimBlock armor_pickup_pixels_;
  assets::TimBlock armor_pickup_palette_;
  mutable std::optional<GameplayResidentState> gameplay_state_;
  mutable bool gameplay_font_resident_{};
  mutable bool gameplay_palette_resident_{};
};

struct PauseTextureAsset {
  std::string name;
  assets::TimImage image;
};

class PauseTextureAtlas final {
public:
  explicit PauseTextureAtlas(const game::MissionPackage &mission) {
    for (const auto &entry : mission.menuAssets().entries()) {
      if (!std::string_view{entry.name}.ends_with(".TIM")) {
        continue;
      }
      const auto map_asset = std::string_view{entry.name}.starts_with("MAP");
      auto localized =
          map_asset ? game::readLocalizedAsset(
                          "maps/" + std::to_string(mission.definition().index) +
                          "/" + entry.name)
                    : std::nullopt;
      assets_.push_back(PauseTextureAsset{
          entry.name,
          localized
              ? assets::TimImage::parse(*localized)
              : assets::TimImage::parse(mission.menuAssets().file(entry.name)),
      });
    }
    if (image("GLOKSIL.TIM") == nullptr) {
      throw core::Error{
          core::ErrorCode::not_found,
          "MENU.HOG does not contain the universal pause weapon texture"};
    }
  }

  [[nodiscard]] const assets::TimImage *
  image(std::string_view name) const noexcept {
    const auto match =
        std::ranges::find_if(assets_, [&](const PauseTextureAsset &asset) {
          return asset.name == name;
        });
    return match == assets_.end() ? nullptr : &match->image;
  }

private:
  std::vector<PauseTextureAsset> assets_;
};

class PoliceLightbarAnimation final {
public:
  void invalidate() noexcept {
    resident_copies_.clear();
    touched_pages_.clear();
  }

  [[nodiscard]] bool
  synchronize(const game::GameplaySession &gameplay, std::uint64_t tick,
              TextureStreamer &textures,
              std::span<const game::LegacyGuestSpritePresentationState>
                  retained_sprites = {}) {
    const auto &frame = game::policeLightbarFrame(tick);
    std::vector<game::EffectTextureCopy> required_copies;
    for (const auto object_index : gameplay.activeObjects()) {
      const auto *model = gameplay.displayedObjectModel(object_index);
      if (model == nullptr ||
          model->visual_effect != game::ObjectVisualEffect::police_lightbar) {
        continue;
      }
      const auto bank = gameplay.objectTextureBank(object_index);
      const auto blue = textures.relocateEffectTextureCopy(frame.blue, bank);
      const auto red = textures.relocateEffectTextureCopy(frame.red, bank);
      // Never fall back to retail physical TP10: that slot may currently be
      // owned by a different bank after native alias allocation.
      if (!blue || !red) {
        required_copies.clear();
        break;
      }
      for (const auto &copy : std::array{*blue, *red}) {
        if (std::ranges::find(required_copies, copy) == required_copies.end()) {
          required_copies.push_back(copy);
        }
      }
    }
    if (required_copies == resident_copies_) {
      return false;
    }

    for (const auto page : touched_pages_) {
      textures.invalidatePhysicalPage(page);
    }
    if (!touched_pages_.empty()) {
      textures.ensure(gameplay, retained_sprites);
    }
    touched_pages_.clear();
    resident_copies_.clear();
    if (required_copies.empty()) {
      return false;
    }

    const auto apply_copy = [](const game::EffectTextureCopy &texture_copy) {
      auto source = RECT16{
          texture_copy.source.x,
          texture_copy.source.y,
          texture_copy.source.width,
          texture_copy.source.height,
      };
      static_cast<void>(MoveImage(&source, texture_copy.destination_x,
                                  texture_copy.destination_y));
    };
    for (const auto &copy : required_copies) {
      apply_copy(copy);
      const auto page =
          static_cast<unsigned int>(copy.destination_x / 64) +
          (static_cast<unsigned int>(copy.destination_y / 256) * 16U);
      if (std::ranges::find(touched_pages_, page) == touched_pages_.end()) {
        touched_pages_.push_back(page);
      }
    }
    DrawSync(0);
    resident_copies_ = std::move(required_copies);
    return true;
  }

private:
  std::vector<game::EffectTextureCopy> resident_copies_;
  std::vector<unsigned int> touched_pages_;
};

class RetailScrimAnimation final {
public:
  void reset() noexcept {
    executed_copy_count_ = 0U;
    residency_.clear();
  }

  void restore(const game::GameplaySession &gameplay,
               const game::LegacyScrimBridgeState &state,
               TextureStreamer &textures) {
    residency_.clear();
    if (!state.resource_present || state.vram_moves.empty()) {
      return;
    }
    const auto bank = textureBank(gameplay);
    residency_ = residency(state.vram_moves, bank, textures);
    const auto relocated = relocate(state.vram_moves, bank, textures);
    for (auto phase = std::uint64_t{}; phase < executed_copy_count_; ++phase) {
      submit(relocated);
    }
    if (executed_copy_count_ != 0U) {
      DrawSync(0);
    }
  }

  [[nodiscard]] bool
  prepareFrame(const game::GameplaySession &gameplay,
               const game::LegacyScrimBridgeState &state,
               std::span<const game::LegacyScrimCopyPresentationPhase> phases,
               std::uint64_t displayed_guest_frame, TextureStreamer &textures) {
    return synchronize(gameplay, state, phases, displayed_guest_frame,
                       PhaseSelection::preceding, textures);
  }

  [[nodiscard]] bool
  commitFrame(const game::GameplaySession &gameplay,
              const game::LegacyScrimBridgeState &state,
              std::span<const game::LegacyScrimCopyPresentationPhase> phases,
              std::uint64_t displayed_guest_frame, TextureStreamer &textures) {
    return synchronize(gameplay, state, phases, displayed_guest_frame,
                       PhaseSelection::displayed, textures);
  }

  [[nodiscard]] bool commitSkippedFrame(
      const game::GameplaySession &gameplay,
      const game::LegacyScrimBridgeState &state,
      std::span<const game::LegacyScrimCopyPresentationPhase> phases,
      std::uint64_t displayed_guest_frame, TextureStreamer &textures) {
    return synchronize(gameplay, state, phases, displayed_guest_frame,
                       PhaseSelection::completed, textures);
  }

private:
  enum class PhaseSelection : std::uint8_t {
    preceding,
    displayed,
    completed,
  };

  [[nodiscard]] bool
  synchronize(const game::GameplaySession &gameplay,
              const game::LegacyScrimBridgeState &state,
              std::span<const game::LegacyScrimCopyPresentationPhase> phases,
              std::uint64_t displayed_guest_frame, PhaseSelection selection,
              TextureStreamer &textures) {
    if (!state.resource_present) {
      reset();
      return false;
    }
    if (std::ranges::any_of(phases, [displayed_guest_frame](const auto &phase) {
          return game::legacyScrimCopyPhasePosition(phase.guest_frame,
                                                    displayed_guest_frame) ==
                 game::LegacyScrimCopyPhasePosition::future_frame;
        })) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "SCRIM copy phase is newer than its displayed frame"};
    }
    const auto moves =
        !state.vram_moves.empty()
            ? std::span<const game::LegacyVramMoveBridgeState>{state.vram_moves}
        : phases.empty() ? std::span<const game::LegacyVramMoveBridgeState>{}
                         : std::span<const game::LegacyVramMoveBridgeState>{
                               phases.back().moves};
    if (moves.empty()) {
      return false;
    }
    const auto bank = textureBank(gameplay);
    auto current_residency = residency(moves, bank, textures);
    auto changed = false;
    if (!residency_.empty() && current_residency != residency_) {
      // A room/bank/effect transition replaced at least one physical page.
      // Restore every page in the two-page SCRIM ring to authored content,
      // then replay the exact accumulated phase. Reloading only the changed
      // half would splice a fresh strip into the already-scrolled half.
      for (const auto &page : current_residency) {
        if (!textures.reloadResidentPage(page.logical_page, bank)) {
          throw core::Error{core::ErrorCode::invalid_format,
                            "Cannot restore a resident SCRIM texture page"};
        }
      }
      DrawSync(0);
      current_residency = residency(moves, bank, textures);
      const auto relocated = relocate(moves, bank, textures);
      for (auto phase = std::uint64_t{}; phase < executed_copy_count_;
           ++phase) {
        submit(relocated);
      }
      if (executed_copy_count_ != 0U) {
        DrawSync(0);
      }
      changed = true;
    }
    residency_ = std::move(current_residency);

    const auto selected = [displayed_guest_frame,
                           selection](const auto &phase) {
      const auto position = game::legacyScrimCopyPhasePosition(
          phase.guest_frame, displayed_guest_frame);
      switch (selection) {
      case PhaseSelection::preceding:
        return position ==
               game::LegacyScrimCopyPhasePosition::preceding_display;
      case PhaseSelection::displayed:
        return position == game::LegacyScrimCopyPhasePosition::displayed_frame;
      case PhaseSelection::completed:
        return position != game::LegacyScrimCopyPhasePosition::future_frame;
      }
      return false;
    };
    const auto has_selected = std::ranges::any_of(phases, selected);
    // Retail links DR_MOVE at OT depth 1 after world/SCRIM geometry. A phase
    // belonging to the displayed tick therefore sees the old texture and
    // commits only after that frame's GPU work. Catch-up phases from older
    // guest ticks are applied before drawing the current frame.
    if (has_selected && selection != PhaseSelection::preceding) {
      DrawSync(0);
    }
    for (const auto &phase : phases) {
      if (!selected(phase)) {
        continue;
      }
      submit(relocate(phase.moves, bank, textures));
      executed_copy_count_ =
          (executed_copy_count_ + 1U) % game::legacy_scrim_copy_cycle;
      changed = true;
    }
    if (has_selected) {
      DrawSync(0);
    }
    return changed;
  }
  struct PageResidency {
    unsigned int logical_page{};
    std::uint64_t token{};

    [[nodiscard]] friend bool operator==(const PageResidency &,
                                         const PageResidency &) = default;
  };

  [[nodiscard]] static int textureBank(const game::GameplaySession &gameplay) {
    const auto *model = gameplay.detachedScrimModel();
    if (model == nullptr) {
      throw core::Error{core::ErrorCode::not_found,
                        "Resident retail SCRIM has no model resource"};
    }
    return static_cast<int>(model->textureBank());
  }

  [[nodiscard]] static std::vector<PageResidency>
  residency(std::span<const game::LegacyVramMoveBridgeState> moves, int bank,
            const TextureStreamer &textures) {
    std::vector<PageResidency> result;
    const auto append = [&](int x, int y, int width, int height) {
      const auto local_x = x & 63;
      const auto local_y = y & 255;
      if (local_x + width > 64 || local_y + height > 256) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "Retail SCRIM copy crosses a logical texture-page boundary"};
      }
      const auto page = static_cast<unsigned int>(x / 64) +
                        static_cast<unsigned int>(y / 256) * 16U;
      if (std::ranges::any_of(result, [page](const auto &entry) {
            return entry.logical_page == page;
          })) {
        return;
      }
      const auto token = textures.residentPageToken(page, bank);
      if (!token) {
        throw core::Error{core::ErrorCode::invalid_format,
                          "Retail SCRIM texture page is not resident"};
      }
      result.push_back(PageResidency{page, *token});
    };
    for (const auto &move : moves) {
      append(move.source_x, move.source_y, move.width, move.height);
      append(move.destination_x, move.destination_y, move.width, move.height);
    }
    return result;
  }

  [[nodiscard]] static std::vector<game::EffectTextureCopy>
  relocate(std::span<const game::LegacyVramMoveBridgeState> moves, int bank,
           const TextureStreamer &textures) {
    std::vector<game::EffectTextureCopy> relocated;
    relocated.reserve(moves.size());
    for (const auto &move : moves) {
      const auto copy = textures.relocateEffectTextureCopy(
          game::EffectTextureCopy{
              game::EffectVramRect{move.source_x, move.source_y, move.width,
                                   move.height},
              move.destination_x,
              move.destination_y,
          },
          bank);
      if (!copy) {
        throw core::Error{
            core::ErrorCode::invalid_format,
            "Retail SCRIM copy is outside resident texture pages"};
      }
      relocated.push_back(*copy);
    }
    return relocated;
  }

  static void submit(std::span<const game::EffectTextureCopy> relocated) {
    for (const auto &copy : relocated) {
      auto source = RECT16{copy.source.x, copy.source.y, copy.source.width,
                           copy.source.height};
      static_cast<void>(
          MoveImage(&source, copy.destination_x, copy.destination_y));
    }
  }

  std::uint64_t executed_copy_count_{};
  std::vector<PageResidency> residency_;
};

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
  game::LegacyEnvironmentBridgeState environment;
  game::LegacyScrimBridgeState scrim;
  bool retail_environment_active{};
  bool first_person_aim{};
  bool retail_scope_overlay{};
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
std::vector<game::RetailVertexLightState> active_retail_vertex_lights;
std::int32_t active_retail_light_projection{320};
bool active_retail_nightvision{};
bool active_retail_blackout{};

void captureRenderPresentation(const game::GameplaySession &gameplay,
                               RenderPresentationSnapshot &result) {
  result.guest_frame = 0U;
  result.player_guest_slot = -1;
  result.camera = gameplay.camera();
  result.guest_packet_camera = result.camera;
  result.environment = {};
  result.scrim = {};
  result.retail_environment_active = false;
  result.first_person_aim =
      gameplay.playerAim() == game::PlayerAimState::first_person;
  result.retail_scope_overlay = false;
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
      result.player_guest_slot = mission.player_slot;
      result.retail_scope_overlay =
          result.first_person_aim && ((mission.interface_mode == 2U &&
                                       mission.first_person_aim_mode == 2U) ||
                                      (mission.interface_mode == 3U &&
                                       mission.first_person_aim_mode == 3U));
    }
  }
}

void applyRetailEnvironment(
    const game::LegacyEnvironmentBridgeState &environment,
    std::int32_t projection, bool retail_environment_active,
    bool retail_blackout_mission) {
  DRAWENV draw_environment{};
  if (GetDrawEnv(&draw_environment) == nullptr) {
    SetDefDrawEnv(&draw_environment, 0, 0, screen_width, screen_height);
    draw_environment.dtd = 0;
  }
  // Native rendering always starts a fresh back buffer. Use the retail clear
  // color as its authored horizon even when the PS1 background-enable bit is
  // clear, rather than preserving stale pixels from the previous PC frame.
  // Guest auxiliary DR_TPAGE packets are presentation data, not permission to
  // redirect the next native frame into emulated VRAM. Re-establish the exact
  // on-screen draw target every frame so a stale DFE/clip cannot overwrite
  // resident world pages or the relocated mission CLUT.
  draw_environment.clip = RECT16{0, 0, screen_width, screen_height};
  draw_environment.ofs[0] = 0;
  draw_environment.ofs[1] = 0;
  draw_environment.dfe = 1;
  draw_environment.isbg = 1;
  setRGB0(&draw_environment, environment.clear_color.red,
          environment.clear_color.green, environment.clear_color.blue);
  PutDrawEnv(&draw_environment);

  SetBackColor(environment.back_color.red, environment.back_color.green,
               environment.back_color.blue);
  SetFarColor(environment.fog_color.red, environment.fog_color.green,
              environment.fog_color.blue);
  active_retail_depth_cue = {
      environment.fog_dqa,
      environment.fog_dqb,
      projection,
      retail_environment_active ? environment.effectiveTerrainDepthCue()
                                : 0x1000U,
      retail_environment_active && environment.fogEnabled(),
  };
  active_retail_nightvision =
      retail_environment_active && environment.nightvision_enabled;
  // The blackout is authored by the CAVE2 mission package, not by a stable
  // camera flag. Retail rewrites the display/background flags while changing
  // rooms and while entering aim mode, so using a momentary renderer snapshot
  // as the level discriminator makes the ambient darkness disappear. Keep the
  // immutable mission identity authoritative and use the live state only for
  // the night-vision override.
  active_retail_blackout =
      retail_blackout_mission && !active_retail_nightvision;
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
                   camera_z * static_cast<double>(quotient_one)),
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
  if (camera_z <= 0.0) {
    return 0L;
  }
  auto packed = active_retail_depth_cue.terrain;
  if (packed == 0U) {
    packed = 0x07ffU;
  }
  const auto threshold = static_cast<std::uint16_t>(packed);
  const auto shift = static_cast<unsigned int>(packed >> 16U);
  if (threshold >= 0x1000U || shift > 15U) {
    return 0L;
  }
  // The retail terrain formula consumes the value returned by
  // RotTransPers: SZ3 >> 2, not the full camera-space GTE Z. Feeding it the
  // full Z pulled the atmospheric boundary four times closer to the camera.
  const auto sz3 =
      std::clamp<std::int64_t>(std::llround(camera_z), 0LL, 0xffffLL);
  const auto ordering_depth = sz3 >> 2U;
  const auto distance = ((ordering_depth * 3LL) >> 2U) - threshold;
  if (distance <= 0) {
    return 0L;
  }
  const auto factor = distance << shift;
  // Keep the far edge fully depth-cued. Returning zero after the authored
  // limit made the scene boundary brighten again instead of fading out.
  return static_cast<long>(std::min<std::int64_t>(factor, 0x1500LL));
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
  result.guest_frame = current.guest_frame;
  result.player_guest_slot = current.player_guest_slot;
  result.camera = current.camera;
  // Camera-list packets belong to the current guest tick and must never be
  // interpolated with the previous packet camera.
  result.guest_packet_camera = current.guest_packet_camera;
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
  result.retail_scope_overlay = current.retail_scope_overlay;
  result.grenade_trajectory = current.grenade_trajectory;
  result.flashlight_enabled = current.flashlight_enabled;
  result.vertex_lights = current.vertex_lights;
  result.world_vertex_colors = current.world_vertex_colors;
  result.objects.assign(current.objects.begin(), current.objects.end());
  result.legacy_player = current.legacy_player;
  result.player = current.player;
  result.player_model_heading = current.player_model_heading;
  result.projectiles.assign(current.projectiles.begin(),
                            current.projectiles.end());
  result.dropped_items = current.dropped_items;
  // These lists are already projected by the retail frame builder. Mixing
  // records from two guest ticks would make weather/backdrop particles tear.
  result.guest_sprites = current.guest_sprites;
  result.guest_lines = current.guest_lines;
  result.guest_raw_packets = current.guest_raw_packets;
  result.current_line_particles = current.current_line_particles;
  result.current_combat_particles = current.current_combat_particles;
  result.renderer_sprite_fast_path = current.renderer_sprite_fast_path;
  result.guest_camera_lists_captured = current.guest_camera_lists_captured;
  const auto aim_transition =
      previous.first_person_aim != current.first_person_aim;
  // Chase/manual-aim transitions are camera cuts in retail. Interpolating
  // across the cut moves the eye through Gabe while visibility has already
  // switched, producing a frame of his head/weapon inside the view.
  if (!aim_transition && !current.first_person_aim) {
    result.camera.x = component(previous.camera.x, current.camera.x);
    result.camera.y = component(previous.camera.y, current.camera.y);
    result.camera.z = component(previous.camera.z, current.camera.z);
    result.camera.target_x =
        component(previous.camera.target_x, current.camera.target_x);
    result.camera.target_y =
        component(previous.camera.target_y, current.camera.target_y);
    result.camera.target_z =
        component(previous.camera.target_z, current.camera.target_z);
    result.camera.projection = static_cast<std::int32_t>(std::llround(
        component(previous.camera.projection, current.camera.projection)));
  }

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

struct PrimitiveBuffer {
  StableFrameVector<POLY_GT3> triangles{"Terrain triangles"};
  StableFrameVector<POLY_GT4> quads{"Terrain quads"};
  StableFrameVector<POLY_GT3> objects{"Object triangles"};
  StableFrameVector<POLY_GT4> object_quads{"Object quads"};
  StableFrameVector<POLY_F3> player{"Fallback player triangles"};
  StableFrameVector<LINE_G2> effects{"Effect lines"};
  StableFrameVector<LINE_F2> combat_effect_lines{"Combat effect lines"};
  StableFrameVector<POLY_F3> combat_effect_triangles{"Combat effect triangles"};
  StableFrameVector<POLY_G4> flashlight_cone_quads{"Flashlight cone quads"};
  StableFrameVector<POLY_GT4> effect_sprite_quads{"Effect sprite quads"};
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

  void reset() noexcept {
    triangles.reset();
    quads.reset();
    objects.reset();
    object_quads.reset();
    player.reset();
    effects.reset();
    combat_effect_lines.reset();
    combat_effect_triangles.reset();
    flashlight_cone_quads.reset();
    effect_sprite_quads.reset();
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
  }

  void lockStorage() noexcept {
    triangles.lockStorage();
    quads.lockStorage();
    objects.lockStorage();
    object_quads.lockStorage();
    player.lockStorage();
    effects.lockStorage();
    combat_effect_lines.lockStorage();
    combat_effect_triangles.lockStorage();
    flashlight_cone_quads.lockStorage();
    effect_sprite_quads.lockStorage();
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
           player.storageStable() && effects.storageStable() &&
           combat_effect_lines.storageStable() &&
           combat_effect_triangles.storageStable() &&
           flashlight_cone_quads.storageStable() &&
           effect_sprite_quads.storageStable() &&
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
};

struct RenderStats {
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
  if (!active_retail_nightvision) {
    return color;
  }
  // FUN_800d0058 loads BK=(0,3000,0) for the night-vision camera target and
  // FUN_800cf0e4 feeds NCDT an exact 0x00ff00 source when red BK is zero.
  // Preserve that exact Q12 floor while retaining brighter geometry/lights
  // on the same green-only scene basis.
  constexpr auto green_floor = std::uint8_t{188U};
  const auto intensity =
      std::max({color.red, color.green, color.blue, green_floor});
  return {0U, intensity, 0U};
}

game::DynamicLightVertexColor
applyRetailBlackoutBase(game::DynamicLightVertexColor color) noexcept {
  if (!active_retail_blackout) {
    return color;
  }
  // CAVE2 first establishes an almost-black ambient base, then lets its live
  // vertex lights reveal geometry. Apply the veil before both the exact guest
  // flashlight record and native transient flashes so local light survives.
  const auto veil = [](std::uint8_t channel) {
    return static_cast<std::uint8_t>((static_cast<unsigned int>(channel) + 4U) /
                                     8U);
  };
  return {veil(color.red), veil(color.green), veil(color.blue)};
}

VertexColor decodeColor(std::uint16_t color) {
  return VertexColor{
      static_cast<std::uint8_t>((color & 0x1fU) << 3U),
      static_cast<std::uint8_t>(((color >> 5U) & 0x1fU) << 3U),
      static_cast<std::uint8_t>(((color >> 10U) & 0x1fU) << 3U),
  };
}

TexturedVertex makeTexturedVertex(const SVECTOR &position, assets::EmdUv uv,
                                  VertexColor color,
                                  bool apply_lighting = true) {
  auto lit = game::DynamicLightVertexColor{color.red, color.green, color.blue};
  if (apply_lighting) {
    lit = applyRetailBlackoutBase(lit);
    const auto retail_lit = game::applyRetailVertexLighting(
        lit, active_retail_vertex_lights,
        {static_cast<double>(position.vx), static_cast<double>(position.vy),
         static_cast<double>(position.vz)},
        active_retail_light_projection);
    lit = game::applyDynamicLighting(
        retail_lit,
        game::sampleDynamicLighting(active_dynamic_lights,
                                    {static_cast<double>(position.vx),
                                     static_cast<double>(position.vy),
                                     static_cast<double>(position.vz)}));
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
  const auto dark_base =
      applyRetailBlackoutBase({base.red, base.green, base.blue});
  const auto retail_lit = game::applyRetailVertexLighting(
      dark_base, active_retail_vertex_lights, {x, y, z},
      active_retail_light_projection);
  const auto lit = applyRetailNightvision(game::applyDynamicLighting(
      retail_lit,
      game::sampleDynamicLighting(active_dynamic_lights, {x, y, z})));
  return VertexColor{lit.red, lit.green, lit.blue};
}

void dynamicallyLight(POLY_GT3 &primitive,
                      const std::array<SVECTOR, 3U> &vertices) noexcept {
  const auto first =
      dynamicallyLit({primitive.r0, primitive.g0, primitive.b0}, vertices[0].vx,
                     vertices[0].vy, vertices[0].vz);
  const auto second =
      dynamicallyLit({primitive.r1, primitive.g1, primitive.b1}, vertices[1].vx,
                     vertices[1].vy, vertices[1].vz);
  const auto third =
      dynamicallyLit({primitive.r2, primitive.g2, primitive.b2}, vertices[2].vx,
                     vertices[2].vy, vertices[2].vz);
  setRGB0(&primitive, first.red, first.green, first.blue);
  setRGB1(&primitive, second.red, second.green, second.blue);
  setRGB2(&primitive, third.red, third.green, third.blue);
}

void dynamicallyLight(POLY_GT4 &primitive,
                      const std::array<SVECTOR, 4U> &vertices) noexcept {
  const auto first =
      dynamicallyLit({primitive.r0, primitive.g0, primitive.b0}, vertices[0].vx,
                     vertices[0].vy, vertices[0].vz);
  const auto second =
      dynamicallyLit({primitive.r1, primitive.g1, primitive.b1}, vertices[1].vx,
                     vertices[1].vy, vertices[1].vz);
  const auto third =
      dynamicallyLit({primitive.r2, primitive.g2, primitive.b2}, vertices[2].vx,
                     vertices[2].vy, vertices[2].vz);
  const auto fourth =
      dynamicallyLit({primitive.r3, primitive.g3, primitive.b3}, vertices[3].vx,
                     vertices[3].vy, vertices[3].vz);
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
  return static_cast<std::uint16_t>(
      (texture_page & static_cast<std::uint16_t>(~0x1fU)) |
      streamed_texture_page_remap[bank][logical_page]);
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
  const auto logical_page =
      source_page < 32U ? source_page : texture_page & 0x1fU;
  const auto bank = (streamed_vlf_page_mask & (1U << logical_page)) != 0U
                        ? std::size_t{0U}
                        : std::min<std::size_t>(texture_bank, 1U);
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

class GlassShatterPresentation final {
public:
  void reset(const game::GameplaySession &gameplay) {
    shards_.clear();
    destroyed_.resize(gameplay.objects().size());
    identities_.resize(gameplay.objects().size());
    for (std::size_t object = 0U; object < gameplay.objects().size();
         ++object) {
      destroyed_[object] =
          gameplay.objectDestroyed(static_cast<std::uint16_t>(object));
      identities_[object] = identity(gameplay.objects()[object]);
    }
  }

  void observe(const game::GameplaySession &gameplay,
               const RenderPresentationSnapshot &presentation) {
    if (destroyed_.size() != gameplay.objects().size()) {
      const auto previous_size = destroyed_.size();
      destroyed_.resize(gameplay.objects().size());
      identities_.resize(gameplay.objects().size());
      for (auto object = previous_size; object < gameplay.objects().size();
           ++object) {
        destroyed_[object] =
            gameplay.objectDestroyed(static_cast<std::uint16_t>(object));
        identities_[object] = identity(gameplay.objects()[object]);
      }
    }
    const auto count =
        std::min(gameplay.objects().size(), presentation.objects.size());
    for (std::size_t object = 0U; object < count; ++object) {
      const auto &scene_object = gameplay.objects()[object];
      const auto current_identity = identity(scene_object);
      const auto destroyed =
          gameplay.objectDestroyed(static_cast<std::uint16_t>(object));
      if (identities_[object] != current_identity) {
        identities_[object] = current_identity;
        destroyed_[object] = destroyed;
        continue;
      }
      if (destroyed && !destroyed_[object] &&
          scene_object.damage_response == game::ObjectDamageResponse::shatter) {
        spawn(gameplay, presentation, static_cast<std::uint16_t>(object));
      }
      destroyed_[object] = destroyed;
    }
  }

  void advance() {
    for (auto &shard : shards_) {
      ++shard.age;
    }
    std::erase_if(
        shards_, [](const auto &shard) { return shard.age >= shard.lifetime; });
  }

  [[nodiscard]] std::span<const GlassShardPresentationState>
  shards() const noexcept {
    return shards_;
  }

private:
  struct SourceVertex {
    Vector3 position;
    double u{};
    double v{};
  };

  [[nodiscard]] static std::uint64_t
  identity(const game::SceneObject &object) noexcept {
    return static_cast<std::uint64_t>(object.model) |
           (static_cast<std::uint64_t>(object.source_index) << 16U) |
           (static_cast<std::uint64_t>(object.class_id) << 32U);
  }

  [[nodiscard]] static SourceVertex midpoint(const SourceVertex &first,
                                             const SourceVertex &second) {
    return SourceVertex{
        Vector3{(first.position.x + second.position.x) * 0.5,
                (first.position.y + second.position.y) * 0.5,
                (first.position.z + second.position.z) * 0.5},
        (first.u + second.u) * 0.5,
        (first.v + second.v) * 0.5,
    };
  }

  void appendShard(const std::array<SourceVertex, 3U> &vertices,
                   const assets::GmdTriangle &material,
                   std::uint8_t texture_bank, const game::CameraState &camera,
                   std::uint32_t seed) {
    constexpr auto maximum_active_shards = std::size_t{256U};
    if (shards_.size() == maximum_active_shards) {
      shards_.erase(shards_.begin());
    }
    const auto centre = Vector3{
        (vertices[0].position.x + vertices[1].position.x +
         vertices[2].position.x) /
            3.0,
        (vertices[0].position.y + vertices[1].position.y +
         vertices[2].position.y) /
            3.0,
        (vertices[0].position.z + vertices[1].position.z +
         vertices[2].position.z) /
            3.0,
    };
    const auto edge1 = Vector3{vertices[1].position.x - vertices[0].position.x,
                               vertices[1].position.y - vertices[0].position.y,
                               vertices[1].position.z - vertices[0].position.z};
    const auto edge2 = Vector3{vertices[2].position.x - vertices[0].position.x,
                               vertices[2].position.y - vertices[0].position.y,
                               vertices[2].position.z - vertices[0].position.z};
    auto normal = cross(edge1, edge2);
    const auto normal_length = std::sqrt(
        normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (normal_length > 0.0001) {
      normal.x /= normal_length;
      normal.y /= normal_length;
      normal.z /= normal_length;
    } else {
      normal = {0.0, 0.0, 1.0};
    }
    const auto to_camera =
        Vector3{camera.x - centre.x, camera.y - centre.y, camera.z - centre.z};
    if (normal.x * to_camera.x + normal.y * to_camera.y +
            normal.z * to_camera.z <
        0.0) {
      normal.x = -normal.x;
      normal.y = -normal.y;
      normal.z = -normal.z;
    }
    auto random = [&seed]() {
      seed = seed * 1664525U + 1013904223U;
      return static_cast<double>((seed >> 8U) & 0xffffU) / 65535.0;
    };
    const auto outward_speed = 42.0 + random() * 58.0;
    const auto spin_axis_raw = Vector3{
        random() * 2.0 - 1.0, random() * 2.0 - 1.0, random() * 2.0 - 1.0};
    const auto spin_length = std::sqrt(spin_axis_raw.x * spin_axis_raw.x +
                                       spin_axis_raw.y * spin_axis_raw.y +
                                       spin_axis_raw.z * spin_axis_raw.z);
    const auto spin_axis = spin_length > 0.0001
                               ? Vector3{spin_axis_raw.x / spin_length,
                                         spin_axis_raw.y / spin_length,
                                         spin_axis_raw.z / spin_length}
                               : Vector3{0.0, 1.0, 0.0};
    GlassShardPresentationState shard;
    for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
      shard.vertices[corner] = GlassShardVertexState{
          vertices[corner].position.x, vertices[corner].position.y,
          vertices[corner].position.z, vertices[corner].u, vertices[corner].v};
    }
    shard.centre = {centre.x, centre.y, centre.z};
    shard.velocity = {
        normal.x * outward_speed + (random() * 2.0 - 1.0) * 30.0,
        normal.y * outward_speed - (28.0 + random() * 34.0),
        normal.z * outward_speed + (random() * 2.0 - 1.0) * 30.0,
    };
    shard.spin_axis = {spin_axis.x, spin_axis.y, spin_axis.z};
    shard.angular_speed = (random() * 2.0 - 1.0) * 0.34;
    shard.texture_page = material.texture_page;
    shard.clut = material.clut;
    shard.texture_bank = texture_bank;
    // Broken panes use the same authored texels but become translucent while
    // falling. This matches the retail fragment pass even for an opaque
    // intact-window material.
    shard.semi_transparent = true;
    shard.lifetime = static_cast<std::uint16_t>(24U + random() * 10.0);
    shards_.push_back(shard);
  }

  void spawn(const game::GameplaySession &gameplay,
             const RenderPresentationSnapshot &presentation,
             std::uint16_t object_index) {
    if (object_index >= presentation.objects.size()) {
      return;
    }
    const auto &object = presentation.objects[object_index];
    if (object.model >= gameplay.objectModels().size()) {
      return;
    }
    const auto *model = std::get_if<assets::GmdModel>(
        &gameplay.objectModels()[object.model].geometry);
    if (model == nullptr) {
      return;
    }
    const auto renderable_count = static_cast<std::size_t>(
        std::ranges::count_if(model->triangles(), [](const auto &triangle) {
          return triangle.flags != 0U;
        }));
    const auto subdivide = renderable_count <= 20U;
    auto emitted = std::size_t{};
    constexpr auto maximum_shards_per_break = std::size_t{80U};
    for (std::size_t triangle_index = 0U;
         triangle_index < model->triangles().size() &&
         emitted < maximum_shards_per_break;
         ++triangle_index) {
      const auto &triangle = model->triangles()[triangle_index];
      if (triangle.flags == 0U) {
        continue;
      }
      std::array<SourceVertex, 3U> source{};
      for (std::size_t corner = 0U; corner < source.size(); ++corner) {
        const auto &vertex = model->vertices()[triangle.vertex_indices[corner]];
        source[corner] = SourceVertex{
            transformPoint(vertex.x, vertex.y, vertex.z, object.transform),
            static_cast<double>(triangle.uv[corner].u),
            static_cast<double>(triangle.uv[corner].v),
        };
      }
      const auto base_seed =
          0x9e3779b9U ^ (static_cast<std::uint32_t>(object_index) << 16U) ^
          static_cast<std::uint32_t>(triangle_index * 0x45d9f3bU);
      if (!subdivide) {
        appendShard(source, triangle, gameplay.objectTextureBank(object_index),
                    presentation.camera, base_seed);
        ++emitted;
        continue;
      }
      const auto ab = midpoint(source[0], source[1]);
      const auto bc = midpoint(source[1], source[2]);
      const auto ca = midpoint(source[2], source[0]);
      const std::array fragments{
          std::array{source[0], ab, ca},
          std::array{ab, source[1], bc},
          std::array{ca, bc, source[2]},
          std::array{ab, bc, ca},
      };
      for (std::size_t fragment = 0U;
           fragment < fragments.size() && emitted < maximum_shards_per_break;
           ++fragment) {
        appendShard(
            fragments[fragment], triangle,
            gameplay.objectTextureBank(object_index), presentation.camera,
            base_seed ^ static_cast<std::uint32_t>(fragment * 0x85ebca6bU));
        ++emitted;
      }
    }
  }

  std::vector<bool> destroyed_;
  std::vector<std::uint64_t> identities_;
  std::vector<GlassShardPresentationState> shards_;
};

SVECTOR
transformCylindricalBillboardVertex(const assets::GmdModel &model,
                                    const assets::GmdVertex &vertex,
                                    const assets::MissionTransform &transform,
                                    const game::CameraState &camera) {
  const auto &bounds = model.bounds();
  const auto span_x = static_cast<int>(bounds.maximum_x) - bounds.minimum_x;
  const auto span_z = static_cast<int>(bounds.maximum_z) - bounds.minimum_z;
  const auto horizontal_uses_x = span_x >= span_z;
  const auto center_x =
      (static_cast<double>(bounds.minimum_x) + bounds.maximum_x) * 0.5;
  const auto center_y =
      (static_cast<double>(bounds.minimum_y) + bounds.maximum_y) * 0.5;
  const auto center_z =
      (static_cast<double>(bounds.minimum_z) + bounds.maximum_z) * 0.5;
  const auto center = transformPoint(center_x, center_y, center_z, transform);
  const auto authored = transformPoint(vertex.x, vertex.y, vertex.z, transform);

  const auto axis = horizontal_uses_x ? std::size_t{0U} : std::size_t{2U};
  const auto axis_scale =
      std::sqrt(static_cast<double>(transform.rotation[axis]) *
                    transform.rotation[axis] +
                static_cast<double>(transform.rotation[3U + axis]) *
                    transform.rotation[3U + axis] +
                static_cast<double>(transform.rotation[6U + axis]) *
                    transform.rotation[6U + axis]) /
      4096.0;
  const auto horizontal =
      (horizontal_uses_x ? static_cast<double>(vertex.x) - center_x
                         : static_cast<double>(vertex.z) - center_z) *
      axis_scale;
  const auto to_camera_x = camera.x - center.x;
  const auto to_camera_z = camera.z - center.z;
  const auto camera_distance = std::hypot(to_camera_x, to_camera_z);
  const auto camera_right = camera_distance > 0.0001
                                ? Vector3{-to_camera_z / camera_distance, 0.0,
                                          to_camera_x / camera_distance}
                                : viewBasis(camera)[0];
  return makeVertex(center.x + camera_right.x * horizontal, authored.y,
                    center.z + camera_right.z * horizontal);
}

bool reversesWinding(const assets::MissionTransform &transform) {
  const auto &matrix = transform.rotation;
  const auto determinant =
      static_cast<std::int64_t>(matrix[0]) *
          (static_cast<std::int64_t>(matrix[4]) * matrix[8] -
           static_cast<std::int64_t>(matrix[5]) * matrix[7]) -
      static_cast<std::int64_t>(matrix[1]) *
          (static_cast<std::int64_t>(matrix[3]) * matrix[8] -
           static_cast<std::int64_t>(matrix[5]) * matrix[6]) +
      static_cast<std::int64_t>(matrix[2]) *
          (static_cast<std::int64_t>(matrix[3]) * matrix[7] -
           static_cast<std::int64_t>(matrix[4]) * matrix[6]);
  return determinant < 0;
}

struct HmdPoseTransform {
  std::array<double, 9> rotation{};
  Vector3 translation{};
};

Vector3 rotateHmdPose(const HmdPoseTransform &pose, Vector3 vertex) {
  return Vector3{
      pose.rotation[0] * vertex.x + pose.rotation[1] * vertex.y +
          pose.rotation[2] * vertex.z,
      pose.rotation[3] * vertex.x + pose.rotation[4] * vertex.y +
          pose.rotation[5] * vertex.z,
      pose.rotation[6] * vertex.x + pose.rotation[7] * vertex.y +
          pose.rotation[8] * vertex.z,
  };
}

HmdPoseTransform localHmdBindPose(const assets::HmdPart &part) {
  std::array<double, 9> encoded_rotation{};
  for (std::size_t component = 0; component < encoded_rotation.size();
       ++component) {
    encoded_rotation[component] =
        static_cast<double>(part.local_transform.rotation[component]) / 4096.0;
  }
  const auto encoded_translation = Vector3{
      static_cast<double>(part.local_transform.translation[0]),
      static_cast<double>(part.local_transform.translation[1]),
      static_cast<double>(part.local_transform.translation[2]),
  };
  HmdPoseTransform result;
  // HMD stores the parent-to-part coordinate transform. The bind pose needs
  // its affine inverse: R^-1 = R^T and t^-1 = -R^T t.
  for (std::size_t row = 0; row < 3U; ++row) {
    for (std::size_t column = 0; column < 3U; ++column) {
      result.rotation[row * 3U + column] = encoded_rotation[column * 3U + row];
    }
  }
  const auto inverse_translation = rotateHmdPose(result, encoded_translation);
  result.translation = Vector3{
      -inverse_translation.x,
      -inverse_translation.y,
      -inverse_translation.z,
  };
  return result;
}

HmdPoseTransform
localHmdAnimationPose(const assets::HmdAnimationTransform &transform) {
  const auto radians = [](std::int16_t angle) {
    return static_cast<double>(angle) * 2.0 * std::numbers::pi / 4096.0;
  };
  const auto x = radians(transform.rotation[0]);
  const auto y = radians(transform.rotation[1]);
  const auto z = radians(transform.rotation[2]);
  const auto sx = std::sin(x);
  const auto cx = std::cos(x);
  const auto sy = std::sin(y);
  const auto cy = std::cos(y);
  const auto sz = std::sin(z);
  const auto cz = std::cos(z);
  // Exact floating-point equivalent of the original FUN_800d1608 matrix.
  const std::array<double, 9> encoded_rotation{
      cy * cz + sy * sx * sz,
      sy * sx * cz - cy * sz,
      sy * cx,
      sz * cx,
      cz * cx,
      -sx,
      cy * sx * sz - sy * cz,
      sy * sz + cy * sx * cz,
      cy * cx,
  };
  // PCHAN already stores a child-to-parent local pose. Its Y translation is
  // converted while decoding, unlike the parent-to-child HMD bind matrix.
  return HmdPoseTransform{
      encoded_rotation,
      Vector3{
          static_cast<double>(transform.translation[0]),
          static_cast<double>(transform.translation[1]),
          static_cast<double>(transform.translation[2]),
      },
  };
}

Vector3 applyHmdPose(const HmdPoseTransform &pose, Vector3 vertex) {
  const auto rotated = rotateHmdPose(pose, vertex);
  return Vector3{
      rotated.x + pose.translation.x,
      rotated.y + pose.translation.y,
      rotated.z + pose.translation.z,
  };
}

HmdPoseTransform composeHmdPose(const HmdPoseTransform &parent,
                                const HmdPoseTransform &local) {
  HmdPoseTransform result;
  for (std::size_t row = 0; row < 3U; ++row) {
    for (std::size_t column = 0; column < 3U; ++column) {
      auto value = 0.0;
      for (std::size_t inner = 0; inner < 3U; ++inner) {
        value += parent.rotation[row * 3U + inner] *
                 local.rotation[inner * 3U + column];
      }
      result.rotation[row * 3U + column] = value;
    }
  }
  const auto translated = applyHmdPose(parent, local.translation);
  result.translation = translated;
  return result;
}

struct ResolvedHmdPose {
  std::vector<HmdPoseTransform> parts;
  double ground_y{};
};

ResolvedHmdPose resolveHmdPose(const assets::HmdModel &model,
                               const game::ActorPose *animation_pose) {
  ResolvedHmdPose result;
  result.parts.resize(model.parts().size());
  std::vector<std::uint8_t> resolved(model.parts().size());
  auto resolved_count = std::size_t{};
  while (resolved_count < model.parts().size()) {
    auto made_progress = false;
    for (std::size_t index = 0; index < model.parts().size(); ++index) {
      if (resolved[index] != 0U) {
        continue;
      }
      const auto &part = model.parts()[index];
      if (part.parent >= 0 &&
          resolved[static_cast<std::size_t>(part.parent)] == 0U) {
        continue;
      }
      const auto *animated = animation_pose == nullptr
                                 ? nullptr
                                 : animation_pose->transform(index);
      const auto local = animated == nullptr ? localHmdBindPose(part)
                                             : localHmdAnimationPose(*animated);
      result.parts[index] =
          part.parent < 0
              ? local
              : composeHmdPose(
                    result.parts[static_cast<std::size_t>(part.parent)], local);
      resolved[index] = 1U;
      ++resolved_count;
      made_progress = true;
    }
    if (!made_progress) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Unresolvable HMD hierarchy"};
    }
  }

  std::vector<std::uint8_t> referenced(model.vertices().size());
  for (const auto &triangle : model.triangles()) {
    for (const auto index : triangle.vertex_indices) {
      referenced[index] = 1U;
    }
  }
  // HMD model-space Y grows downward, so the largest referenced Y is the
  // contact plane. Preserve the axis direction and only move it to zero.
  result.ground_y = std::numeric_limits<double>::lowest();
  for (std::size_t index = 0; index < model.vertices().size(); ++index) {
    const auto &vertex = model.vertices()[index];
    const auto posed = applyHmdPose(result.parts[model.vertexParts()[index]],
                                    Vector3{
                                        static_cast<double>(vertex.x),
                                        static_cast<double>(vertex.y),
                                        static_cast<double>(vertex.z),
                                    });
    if (referenced[index] != 0U) {
      result.ground_y = std::max(result.ground_y, posed.y);
    }
  }
  if (result.ground_y == std::numeric_limits<double>::lowest()) {
    result.ground_y = 0.0;
  }
  return result;
}

std::vector<Vector3> poseHmdVertices(const assets::HmdModel &model,
                                     const game::ActorPose *animation_pose,
                                     bool preserve_ground_offset) {
  const auto pose = resolveHmdPose(model, animation_pose);
  std::vector<Vector3> result;
  result.reserve(model.vertices().size());
  for (std::size_t index = 0; index < model.vertices().size(); ++index) {
    const auto &vertex = model.vertices()[index];
    auto posed = applyHmdPose(pose.parts[model.vertexParts()[index]],
                              Vector3{
                                  static_cast<double>(vertex.x),
                                  static_cast<double>(vertex.y),
                                  static_cast<double>(vertex.z),
                              });
    if (!preserve_ground_offset) {
      posed.y -= pose.ground_y;
    }
    result.push_back(posed);
  }
  return result;
}

bool hasLegacyHmdBones(const assets::HmdModel &model,
                       const game::SceneObject &object) noexcept {
  return !model.parts().empty() &&
         model.parts().size() <= object.legacy_hmd_bones.size() &&
         object.legacy_hmd_bone_count >= model.parts().size();
}

std::uint64_t npcAnimationPhase(const game::SceneObject &object,
                                std::uint16_t object_index) noexcept {
  return object.legacy_hmd_root_space || object.legacy_hmd_bone_count != 0U
             ? 0U
             : static_cast<std::uint64_t>(object_index) * 7U;
}

std::optional<assets::MissionTransform> posedActorPartTransform(
    const assets::HmdModel &model, const game::SceneObject &object,
    const game::ActorPose &animation_pose, std::string_view part_prefix) {
  const auto part = std::ranges::find_if(
      model.parts(), [part_prefix](const assets::HmdPart &candidate) {
        return candidate.name.starts_with(part_prefix);
      });
  if (part == model.parts().end()) {
    return std::nullopt;
  }
  const auto part_index =
      static_cast<std::size_t>(std::distance(model.parts().begin(), part));
  if (hasLegacyHmdBones(model, object)) {
    return object.legacy_hmd_bones[part_index];
  }
  const auto pose = resolveHmdPose(model, &animation_pose);
  const auto &local = pose.parts[part_index];
  std::array<double, 9> actor_rotation{};
  for (std::size_t index = 0; index < actor_rotation.size(); ++index) {
    actor_rotation[index] =
        static_cast<double>(object.transform.rotation[index]) / 4096.0;
  }
  std::array<std::int16_t, 9> combined_rotation{};
  for (std::size_t row = 0; row < 3U; ++row) {
    for (std::size_t column = 0; column < 3U; ++column) {
      auto value = 0.0;
      for (std::size_t inner = 0; inner < 3U; ++inner) {
        value += actor_rotation[row * 3U + inner] *
                 local.rotation[inner * 3U + column];
      }
      combined_rotation[row * 3U + column] = static_cast<std::int16_t>(
          std::clamp(std::lround(value * 4096.0), -32768L, 32767L));
    }
  }
  const auto world =
      transformPoint(local.translation.x,
                     local.translation.y -
                         (object.legacy_hmd_root_space ? 0.0 : pose.ground_y),
                     local.translation.z, object.transform);
  return assets::MissionTransform{
      combined_rotation,
      static_cast<std::int32_t>(std::lround(world.x)),
      static_cast<std::int32_t>(std::lround(-world.y)),
      static_cast<std::int32_t>(std::lround(world.z)),
  };
}

SVECTOR transformHmdVertex(const Vector3 &vertex,
                           const assets::MissionTransform &transform) {
  const auto point = transformPoint(vertex.x, vertex.y, vertex.z, transform);
  return makeVertex(point.x, point.y, point.z);
}

std::optional<Vector3> posedActorPartAnchor(const assets::HmdModel &model,
                                            const game::SceneObject &object,
                                            const game::ActorPose &pose,
                                            std::string_view part_prefix) {
  const auto part = std::ranges::find_if(
      model.parts(), [part_prefix](const assets::HmdPart &candidate) {
        return candidate.name.starts_with(part_prefix);
      });
  if (part == model.parts().end()) {
    return std::nullopt;
  }
  const auto part_index =
      static_cast<std::uint16_t>(std::distance(model.parts().begin(), part));
  const auto direct_legacy_pose = hasLegacyHmdBones(model, object);
  const auto vertices =
      direct_legacy_pose
          ? std::vector<Vector3>{}
          : poseHmdVertices(model, &pose, object.legacy_hmd_root_space);
  auto minimum = Vector3{
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(),
  };
  auto maximum = Vector3{
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest(),
  };
  auto found = false;
  for (std::size_t index = 0; index < model.vertices().size(); ++index) {
    if (model.vertexParts()[index] != part_index) {
      continue;
    }
    const auto &source = model.vertices()[index];
    const auto vertex =
        direct_legacy_pose ? transformPoint(static_cast<double>(source.x),
                                            static_cast<double>(source.y),
                                            static_cast<double>(source.z),
                                            object.legacy_hmd_bones[part_index])
                           : vertices[index];
    minimum.x = std::min(minimum.x, vertex.x);
    minimum.y = std::min(minimum.y, vertex.y);
    minimum.z = std::min(minimum.z, vertex.z);
    maximum.x = std::max(maximum.x, vertex.x);
    maximum.y = std::max(maximum.y, vertex.y);
    maximum.z = std::max(maximum.z, vertex.z);
    found = true;
  }
  if (!found) {
    return std::nullopt;
  }
  const auto centre = Vector3{
      (minimum.x + maximum.x) * 0.5,
      (minimum.y + maximum.y) * 0.5,
      (minimum.z + maximum.z) * 0.5,
  };
  return direct_legacy_pose
             ? centre
             : transformPoint(centre.x, centre.y, centre.z, object.transform);
}

std::optional<Vector3> sceneObjectCalloutAnchor(
    const game::GameplaySession &gameplay,
    const game::ActorAnimationBank &actor_animations, std::uint64_t actor_tick,
    std::span<const game::SceneObject> presentation_objects,
    std::uint16_t scene_object, bool headshot) {
  if (scene_object >= presentation_objects.size()) {
    return std::nullopt;
  }
  const auto &object = presentation_objects[scene_object];
  if (object.model >= gameplay.objectModels().size()) {
    return std::nullopt;
  }
  const auto &object_model = gameplay.objectModels()[object.model];
  const auto &geometry = object_model.geometry;
  const auto *model = std::get_if<assets::HmdModel>(&geometry);
  if (model != nullptr) {
    const auto *state = gameplay.npcState(scene_object);
    const auto pose =
        state == nullptr
            ? actor_animations.enemyPose(
                  actor_tick, static_cast<std::uint64_t>(scene_object) * 7U)
            : actor_animations.npcPose(gameplay.npcAnimation(scene_object),
                                       state->locomotion_animation_tick,
                                       state->animation_tick,
                                       npcAnimationPhase(object, scene_object));
    if (const auto anchor = posedActorPartAnchor(
            *model, object, pose,
            headshot ? std::string_view{"Head"} : std::string_view{"Chest"})) {
      return anchor;
    }
  }
  if (object_model.bounds) {
    const auto &bounds = *object_model.bounds;
    return transformPoint(
        (static_cast<double>(bounds.minimum_x) + bounds.maximum_x) * 0.5,
        (static_cast<double>(bounds.minimum_y) + bounds.maximum_y) * 0.5,
        (static_cast<double>(bounds.minimum_z) + bounds.maximum_z) * 0.5,
        object.transform);
  }
  return transformPoint(0.0, 0.0, 0.0, object.transform);
}

std::optional<Vector3>
aimTargetAnchor(const game::GameplaySession &gameplay,
                const game::ActorAnimationBank &actor_animations,
                std::uint64_t actor_tick,
                std::span<const game::SceneObject> presentation_objects) {
  const auto target = gameplay.aimTarget();
  return target ? sceneObjectCalloutAnchor(gameplay, actor_animations,
                                           actor_tick, presentation_objects,
                                           *target, gameplay.headshotTargeted())
                : std::nullopt;
}

std::vector<WorldCalloutAnchor>
worldCalloutAnchors(const game::GameplaySession &gameplay,
                    const game::ActorAnimationBank &actor_animations,
                    std::uint64_t actor_tick,
                    std::span<const game::SceneObject> presentation_objects) {
  std::vector<WorldCalloutAnchor> result;
  result.reserve(gameplay.legacyWorldCallouts().size());
  for (const auto &callout : gameplay.legacyWorldCallouts()) {
    const auto anchor = sceneObjectCalloutAnchor(
        gameplay, actor_animations, actor_tick, presentation_objects,
        callout.object, callout.headshot);
    if (!anchor || callout.text.empty()) {
      continue;
    }
    result.push_back(
        WorldCalloutAnchor{*anchor, callout.text, callout.headshot});
  }
  return result;
}

std::optional<Vector3> weaponMuzzleAnchor(const game::GameplaySession &gameplay,
                                          const assets::HmdModel &actor_model,
                                          const game::SceneObject &actor_object,
                                          const game::ActorPose &actor_pose,
                                          game::WeaponId weapon) {
  const auto hand_transform = posedActorPartTransform(actor_model, actor_object,
                                                      actor_pose, "RightHan");
  if (!hand_transform) {
    return std::nullopt;
  }
  const auto *weapon_model = gameplay.weaponModel(weapon);
  if (weapon_model == nullptr || !weapon_model->bounds) {
    return transformPoint(0.0, 64.0, 0.0, *hand_transform);
  }
  const auto &bounds = *weapon_model->bounds;
  // Retail weapon GMDs are authored with the barrel running along local +Y
  // (for example GLOCK17 reaches Y=23 while M16 reaches Y=84).  Using +Z
  // placed every attached tracer/light beside the hand instead of at the
  // rendered barrel tip.
  return transformPoint(
      (static_cast<double>(bounds.minimum_x) + bounds.maximum_x) * 0.5,
      static_cast<double>(bounds.maximum_y),
      (static_cast<double>(bounds.minimum_z) + bounds.maximum_z) * 0.5,
      *hand_transform);
}

assets::MissionTransform playerTransform(const game::PlayerState &player,
                                         std::int32_t model_heading);

game::SceneObject
presentedPlayerObject(const RenderPresentationSnapshot &presentation) {
  if (presentation.legacy_player) {
    return *presentation.legacy_player;
  }
  return game::SceneObject{
      .transform = playerTransform(presentation.player,
                                   presentation.player_model_heading),
  };
}

game::SceneObject currentPlayerObject(const game::GameplaySession &gameplay) {
  if (const auto *legacy_player = gameplay.legacyPlayerPresentation()) {
    return *legacy_player;
  }
  return game::SceneObject{
      .transform =
          playerTransform(gameplay.player(), gameplay.playerModelHeading()),
  };
}

std::optional<Vector3>
playerWeaponMuzzleAnchor(const game::GameplaySession &gameplay,
                         const game::ActorAnimationBank &actor_animations,
                         const game::SceneObject &player_object,
                         game::WeaponId weapon) {
  const auto *model =
      std::get_if<assets::HmdModel>(&gameplay.playerModel().geometry);
  if (model == nullptr) {
    return std::nullopt;
  }
  const auto action_tick =
      gameplay.playerAction() == game::PlayerActionState::ready
          ? gameplay.playerAnimationTick()
          : gameplay.playerActionAnimationTick();
  const auto pose = actor_animations.playerPose(
      gameplay.playerAnimation(), gameplay.playerAnimationTick(), action_tick);
  return weaponMuzzleAnchor(gameplay, *model, player_object, pose, weapon);
}

std::optional<Vector3>
npcWeaponMuzzleAnchor(const game::GameplaySession &gameplay,
                      const game::ActorAnimationBank &actor_animations,
                      const game::SceneObject &object,
                      std::uint16_t scene_object, game::WeaponId weapon) {
  if (object.model >= gameplay.objectModels().size()) {
    return std::nullopt;
  }
  const auto *state = gameplay.npcState(scene_object);
  const auto *model = std::get_if<assets::HmdModel>(
      &gameplay.objectModels()[object.model].geometry);
  if (state == nullptr || model == nullptr) {
    return std::nullopt;
  }
  const auto pose = actor_animations.npcPose(
      gameplay.npcAnimation(scene_object), state->locomotion_animation_tick,
      state->animation_tick, npcAnimationPhase(object, scene_object));
  return weaponMuzzleAnchor(gameplay, *model, object, pose, weapon);
}

std::optional<Vector3>
weaponMuzzleAnchorForGuestSlot(const game::GameplaySession &gameplay,
                               const game::ActorAnimationBank &actor_animations,
                               const RenderPresentationSnapshot *presentation,
                               std::int16_t guest_slot,
                               game::WeaponId player_weapon) {
  if (presentation != nullptr && guest_slot >= 0 &&
      guest_slot == presentation->player_guest_slot) {
    return playerWeaponMuzzleAnchor(gameplay, actor_animations,
                                    presentedPlayerObject(*presentation),
                                    player_weapon);
  }
  const auto guest_slots = gameplay.legacyGuestSlotsBySceneObject();
  const auto mapped = std::ranges::find(guest_slots, guest_slot);
  if (mapped != guest_slots.end()) {
    const auto scene =
        static_cast<std::size_t>(std::distance(guest_slots.begin(), mapped));
    const auto objects =
        presentation != nullptr
            ? std::span<const game::SceneObject>{presentation->objects}
            : gameplay.objects();
    if (scene >= objects.size()) {
      return std::nullopt;
    }
    if (objects[scene].class_id == 0U) {
      const auto player_object = presentation != nullptr
                                     ? presentedPlayerObject(*presentation)
                                     : currentPlayerObject(gameplay);
      return playerWeaponMuzzleAnchor(gameplay, actor_animations, player_object,
                                      player_weapon);
    }
    const auto *state = gameplay.npcState(static_cast<std::uint16_t>(scene));
    if (state == nullptr) {
      return std::nullopt;
    }
    return npcWeaponMuzzleAnchor(gameplay, actor_animations, objects[scene],
                                 static_cast<std::uint16_t>(scene),
                                 state->weapon);
  }
  // Controller +0x20 is optional provenance and is initialized to -1 by the
  // retail allocator.  A stale/non-resident guest slot is not implicitly the
  // player: doing so drags exact NPC tracers by Gabe's interpolation delta.
  return std::nullopt;
}

std::optional<Vector3>
actorRootForGuestSlot(const game::GameplaySession &gameplay,
                      const RenderPresentationSnapshot *presentation,
                      std::int16_t guest_slot) {
  const auto guest_slots = gameplay.legacyGuestSlotsBySceneObject();
  const auto mapped = std::ranges::find(guest_slots, guest_slot);
  if (mapped == guest_slots.end()) {
    return std::nullopt;
  }
  const auto scene =
      static_cast<std::size_t>(std::distance(guest_slots.begin(), mapped));
  const auto objects =
      presentation != nullptr
          ? std::span<const game::SceneObject>{presentation->objects}
          : gameplay.objects();
  if (scene >= objects.size()) {
    return std::nullopt;
  }
  const auto &mapped_object = objects[scene];
  const auto object =
      mapped_object.class_id == 0U
          ? (presentation != nullptr ? presentedPlayerObject(*presentation)
                                     : currentPlayerObject(gameplay))
          : mapped_object;
  return transformPoint(0.0, 0.0, 0.0, object.transform);
}

Vector3 attachedEffectCentre(const game::GameplaySession &gameplay,
                             const game::ActorAnimationBank &actor_animations,
                             const RenderPresentationSnapshot &presentation,
                             const game::GameplayEffect &effect) {
  const auto fallback = Vector3{effect.x, effect.y, effect.z};
  if (effect.attachment == game::GameplayEffectAttachment::world) {
    return fallback;
  }
  if (effect.attachment == game::GameplayEffectAttachment::player_muzzle) {
    return playerWeaponMuzzleAnchor(gameplay, actor_animations,
                                    presentedPlayerObject(presentation),
                                    gameplay.hud().inventory().current())
        .value_or(fallback);
  }
  if (effect.attachment == game::GameplayEffectAttachment::player_body) {
    const auto &player = presentation.player;
    return Vector3{
        player.x + effect.attachment_offset_x,
        player.y + effect.attachment_offset_y,
        player.z + effect.attachment_offset_z,
    };
  }
  if (effect.owner_object >= presentation.objects.size()) {
    return fallback;
  }
  const auto *state = gameplay.npcState(effect.owner_object);
  const auto &object = presentation.objects[effect.owner_object];
  const auto *model = std::get_if<assets::HmdModel>(
      &gameplay.objectModels()[object.model].geometry);
  if (state == nullptr || model == nullptr) {
    return fallback;
  }
  if (effect.attachment == game::GameplayEffectAttachment::npc_body) {
    const auto root = transformPoint(0.0, 0.0, 0.0, object.transform);
    return Vector3{
        root.x + effect.attachment_offset_x,
        root.y + effect.attachment_offset_y,
        root.z + effect.attachment_offset_z,
    };
  }
  return npcWeaponMuzzleAnchor(gameplay, actor_animations, object,
                               effect.owner_object, state->weapon)
      .value_or(fallback);
}

void submitStats(RenderStats &stats, int depth,
                 std::span<const long> projected);

void configurePrimitive(POLY_GT3 &primitive,
                        const std::array<TexturedVertex, 3> &vertices,
                        TexturedMaterial material) {
  setPolyGT3(&primitive);
  setRGB0(&primitive, quantizeColorOrUv(vertices[0].red),
          quantizeColorOrUv(vertices[0].green),
          quantizeColorOrUv(vertices[0].blue));
  setRGB1(&primitive, quantizeColorOrUv(vertices[1].red),
          quantizeColorOrUv(vertices[1].green),
          quantizeColorOrUv(vertices[1].blue));
  setRGB2(&primitive, quantizeColorOrUv(vertices[2].red),
          quantizeColorOrUv(vertices[2].green),
          quantizeColorOrUv(vertices[2].blue));
  setUV3(&primitive, quantizeColorOrUv(vertices[0].u),
         quantizeColorOrUv(vertices[0].v), quantizeColorOrUv(vertices[1].u),
         quantizeColorOrUv(vertices[1].v), quantizeColorOrUv(vertices[2].u),
         quantizeColorOrUv(vertices[2].v));
  primitive.clut =
      material.resident
          ? material.clut
          : relocateClut(material.clut, material.texture_page,
                         material.texture_bank, material.texture_source_page);
  primitive.tpage =
      material.resident
          ? material.texture_page
          : relocateTexturePage(material.texture_page, material.texture_bank,
                                material.texture_source_page);
  if (material.semi_transparent) {
    setSemiTrans(&primitive, 1);
  }
}

void configurePrimitive(POLY_GT4 &primitive,
                        const std::array<TexturedVertex, 4> &vertices,
                        TexturedMaterial material) {
  setPolyGT4(&primitive);
  setRGB0(&primitive, quantizeColorOrUv(vertices[0].red),
          quantizeColorOrUv(vertices[0].green),
          quantizeColorOrUv(vertices[0].blue));
  setRGB1(&primitive, quantizeColorOrUv(vertices[1].red),
          quantizeColorOrUv(vertices[1].green),
          quantizeColorOrUv(vertices[1].blue));
  setRGB2(&primitive, quantizeColorOrUv(vertices[2].red),
          quantizeColorOrUv(vertices[2].green),
          quantizeColorOrUv(vertices[2].blue));
  setRGB3(&primitive, quantizeColorOrUv(vertices[3].red),
          quantizeColorOrUv(vertices[3].green),
          quantizeColorOrUv(vertices[3].blue));
  setUV4(&primitive, quantizeColorOrUv(vertices[0].u),
         quantizeColorOrUv(vertices[0].v), quantizeColorOrUv(vertices[1].u),
         quantizeColorOrUv(vertices[1].v), quantizeColorOrUv(vertices[2].u),
         quantizeColorOrUv(vertices[2].v), quantizeColorOrUv(vertices[3].u),
         quantizeColorOrUv(vertices[3].v));
  primitive.clut =
      material.resident
          ? material.clut
          : relocateClut(material.clut, material.texture_page,
                         material.texture_bank, material.texture_source_page);
  primitive.tpage =
      material.resident
          ? material.texture_page
          : relocateTexturePage(material.texture_page, material.texture_bank,
                                material.texture_source_page);
  if (material.semi_transparent) {
    setSemiTrans(&primitive, 1);
  }
}

TexturedVertex
emdTexturedVertex(const SVECTOR &position, const assets::EmdSection &section,
                  const assets::EmdPolygon &polygon, std::size_t corner,
                  std::span<const std::uint16_t> live_colors = {},
                  bool apply_lighting = true) {
  const auto vertex = polygon.vertex_indices[corner];
  return makeTexturedVertex(position, polygon.uv[corner],
                            decodeColor(live_colors.empty()
                                            ? section.vertices[vertex].color
                                            : live_colors[vertex]),
                            apply_lighting);
}

using OrderingDepthTransform = int (*)(int depth);

int worldOrderingDepth(int depth) { return depth; }

int fireOrderingDepth(int depth) {
  // The original GsSortSprite path uses 3/4 of RotTransPers depth and pulls
  // CFIRE another 32 OT slots toward the camera. This keeps the centered
  // billboard above coplanar street geometry without moving it in 3D.
  return depth - (depth >> 2) - 32;
}

void submitClippedTriangle(
    std::span<const TexturedVertex> vertices, TexturedMaterial material,
    bool two_sided, bool reverse_winding, const MATRIX &view,
    std::vector<OT_TAG> &ordering_table,
    StableFrameVector<POLY_GT3> &primitive_buffer, RenderStats &stats,
    OrderingDepthTransform ordering_depth = worldOrderingDepth,
    bool terrain_depth_cue = false, bool depth_cue_enabled = true) {
  if (vertices.size() != 3U) {
    throw core::Error{core::ErrorCode::invalid_argument,
                      "Near-plane submission expects one triangle"};
  }
  std::array<float, 4> source_uv_bounds{
      static_cast<float>(vertices[0].u),
      static_cast<float>(vertices[0].v),
      static_cast<float>(vertices[0].u),
      static_cast<float>(vertices[0].v),
  };
  for (const auto &vertex : vertices.subspan(1U)) {
    source_uv_bounds[0] =
        std::min(source_uv_bounds[0], static_cast<float>(vertex.u));
    source_uv_bounds[1] =
        std::min(source_uv_bounds[1], static_cast<float>(vertex.v));
    source_uv_bounds[2] =
        std::max(source_uv_bounds[2], static_cast<float>(vertex.u));
    source_uv_bounds[3] =
        std::max(source_uv_bounds[3], static_cast<float>(vertex.v));
  }
  const auto clipped = core::clipConvexPolygon<TexturedVertex, 4>(
      vertices,
      [&view](const TexturedVertex &vertex) {
        return cameraDepth(view, vertex) - active_near_clip_depth;
      },
      interpolate);
  if (clipped.count < 3U) {
    ++stats.rejected;
    return;
  }
  for (std::size_t index = 1; index + 1U < clipped.count; ++index) {
    const std::array triangle{
        clipped.vertices[0],
        clipped.vertices[index],
        clipped.vertices[index + 1U],
    };
    auto v0 = makeVertex(triangle[0].x, triangle[0].y, triangle[0].z);
    auto v1 = makeVertex(triangle[1].x, triangle[1].y, triangle[1].z);
    auto v2 = makeVertex(triangle[2].x, triangle[2].y, triangle[2].z);
    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
    const std::array precise_uv{
        static_cast<float>(triangle[0].u), static_cast<float>(triangle[0].v),
        static_cast<float>(triangle[1].u), static_cast<float>(triangle[1].v),
        static_cast<float>(triangle[2].u), static_cast<float>(triangle[2].v),
    };
    PGXP_SetLastTextureCoords(precise_uv.data(), 3, source_uv_bounds.data());
    const auto visible_face = frontFacing(xy0, xy1, xy2, 3U) != reverse_winding;
    if (depth <= 0 || (!two_sided && !visible_face)) {
      ++stats.rejected;
      continue;
    }

    auto &primitive = primitive_buffer.emplace_back();
    configurePrimitive(primitive, triangle, material);
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    if (depth_cue_enabled && !material.semi_transparent) {
      const auto cue = [terrain_depth_cue](double camera_z) {
        return terrain_depth_cue ? retailTerrainDepthCue(camera_z)
                                 : retailObjectDepthCue(camera_z);
      };
      depthCuePrimitive(primitive, {cue(cameraDepth(view, triangle[0])),
                                    cue(cameraDepth(view, triangle[1])),
                                    cue(cameraDepth(view, triangle[2]))});
    }
    addPrim(&ordering_table[std::clamp(ordering_depth(depth), 1,
                                       ordering_table_size - 1)],
            &primitive);
    const std::array projected{xy0, xy1, xy2};
    submitStats(stats, depth, projected);
  }
}

void renderGlassShards(std::span<const GlassShardPresentationState> shards,
                       double interpolation_amount, const MATRIX &view,
                       std::vector<OT_TAG> &ordering_table,
                       PrimitiveBuffer &primitives, RenderStats &stats) {
  interpolation_amount = std::clamp(interpolation_amount, 0.0, 1.0);
  constexpr auto gravity_per_update = 11.0;
  for (const auto &shard : shards) {
    const auto time = static_cast<double>(shard.age) + interpolation_amount;
    const auto angle = shard.angular_speed * time;
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    const auto axis =
        Vector3{shard.spin_axis.x, shard.spin_axis.y, shard.spin_axis.z};
    const auto centre = Vector3{shard.centre.x, shard.centre.y, shard.centre.z};
    const auto translation = Vector3{
        shard.velocity.x * time,
        shard.velocity.y * time + gravity_per_update * time * time * 0.5,
        shard.velocity.z * time,
    };
    std::array<TexturedVertex, 3U> vertices{};
    for (std::size_t corner = 0U; corner < vertices.size(); ++corner) {
      const auto &source = shard.vertices[corner];
      const auto local = Vector3{source.x - centre.x, source.y - centre.y,
                                 source.z - centre.z};
      const auto axis_cross = cross(axis, local);
      const auto axis_dot =
          axis.x * local.x + axis.y * local.y + axis.z * local.z;
      const auto one_minus_cosine = 1.0 - cosine;
      const auto rotated = Vector3{
          local.x * cosine + axis_cross.x * sine +
              axis.x * axis_dot * one_minus_cosine,
          local.y * cosine + axis_cross.y * sine +
              axis.y * axis_dot * one_minus_cosine,
          local.z * cosine + axis_cross.z * sine +
              axis.z * axis_dot * one_minus_cosine,
      };
      const auto position = Vector3{centre.x + rotated.x + translation.x,
                                    centre.y + rotated.y + translation.y,
                                    centre.z + rotated.z + translation.z};
      auto lit = makeTexturedVertex(
          makeVertex(position.x, position.y, position.z),
          assets::EmdUv{static_cast<std::uint8_t>(
                            std::clamp(std::lround(source.u), 0L, 255L)),
                        static_cast<std::uint8_t>(
                            std::clamp(std::lround(source.v), 0L, 255L))},
          VertexColor{128U, 128U, 128U});
      lit.x = position.x;
      lit.y = position.y;
      lit.z = position.z;
      lit.u = source.u;
      lit.v = source.v;
      vertices[corner] = lit;
    }
    submitClippedTriangle(
        vertices,
        TexturedMaterial{shard.texture_page, shard.clut, shard.semi_transparent,
                         false, shard.texture_bank},
        true, false, view, ordering_table, primitives.objects, stats);
  }
}

void renderGmdObject(const assets::GmdModel &model,
                     const game::SceneObject &object,
                     game::ObjectVisualEffect visual_effect,
                     unsigned int texture_bank, const game::CameraState &camera,
                     const MATRIX &view, std::vector<OT_TAG> &ordering_table,
                     PrimitiveBuffer &primitives, RenderStats &stats,
                     VertexColor back_color = {128U, 128U, 128U},
                     bool force_two_sided = false) {
  const auto reverse_winding = reversesWinding(object.transform);
  for (const auto &triangle : model.triangles()) {
    if (triangle.flags == 0) {
      continue;
    }
    const auto transform_vertex = [&](std::uint8_t index) {
      const auto &vertex = model.vertices()[index];
      return visual_effect == game::ObjectVisualEffect::billboard_glow
                 ? transformCylindricalBillboardVertex(model, vertex,
                                                       object.transform, camera)
                 : transformVertex(vertex, object.transform);
    };
    auto v0 = transform_vertex(triangle.vertex_indices[0]);
    auto v1 = transform_vertex(triangle.vertex_indices[1]);
    auto v2 = transform_vertex(triangle.vertex_indices[2]);
    const std::array positions{v0, v1, v2};
    const auto near_status = classifyNearPlane(view, positions);
    const auto two_sided_effect =
        force_two_sided || (model.planar() && triangle.semi_transparent);
    if (near_status == NearPlaneStatus::outside) {
      ++stats.rejected;
      continue;
    }
    if (near_status == NearPlaneStatus::intersecting) {
      const std::array textured{
          makeTexturedVertex(v0, triangle.uv[0], back_color),
          makeTexturedVertex(v1, triangle.uv[1], back_color),
          makeTexturedVertex(v2, triangle.uv[2], back_color),
      };
      submitClippedTriangle(textured,
                            TexturedMaterial{
                                triangle.texture_page,
                                triangle.clut,
                                triangle.semi_transparent,
                                false,
                                static_cast<std::uint8_t>(texture_bank),
                            },
                            two_sided_effect, reverse_winding, view,
                            ordering_table, primitives.objects, stats);
      continue;
    }

    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
    const auto visible_face = frontFacing(xy0, xy1, xy2, 3U) != reverse_winding;
    if (depth <= 0 || (!two_sided_effect && !visible_face)) {
      ++stats.rejected;
      continue;
    }

    auto &primitive = primitives.objects.emplace_back();
    configurePrimitive(primitive, triangle, texture_bank, back_color);
    dynamicallyLight(primitive, positions);
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    if (!triangle.semi_transparent) {
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

void renderActorWeapon(const game::GameplaySession &gameplay,
                       const assets::HmdModel &actor_model,
                       const game::SceneObject &actor_object,
                       const game::ActorPose &actor_pose, game::WeaponId weapon,
                       unsigned int texture_bank,
                       const game::CameraState &camera, const MATRIX &view,
                       std::vector<OT_TAG> &ordering_table,
                       PrimitiveBuffer &primitives, RenderStats &stats) {
  const auto *weapon_model = gameplay.weaponModel(weapon);
  if (weapon_model == nullptr) {
    return;
  }
  const auto *geometry = std::get_if<assets::GmdModel>(&weapon_model->geometry);
  if (geometry == nullptr) {
    return;
  }
  const auto transform = posedActorPartTransform(actor_model, actor_object,
                                                 actor_pose, "RightHan");
  if (!transform) {
    return;
  }
  auto weapon_object = game::SceneObject{.transform = *transform};
  weapon_object.legacy_hmd_back_color_q12 =
      actor_object.legacy_hmd_back_color_q12;
  weapon_object.legacy_hmd_back_color_valid =
      actor_object.legacy_hmd_back_color_valid;
  renderGmdObject(*geometry, weapon_object, game::ObjectVisualEffect::none,
                  texture_bank, camera, view, ordering_table, primitives, stats,
                  retailHmdBackColor(actor_object));
}

void renderHmdObject(const assets::HmdModel &model,
                     const game::SceneObject &object,
                     const game::ActorPose *animation_pose,
                     unsigned int texture_bank, const MATRIX &view,
                     std::vector<OT_TAG> &ordering_table,
                     PrimitiveBuffer &primitives, RenderStats &stats) {
  const auto direct_legacy_pose = hasLegacyHmdBones(model, object);
  const auto posed_vertices =
      direct_legacy_pose ? std::vector<Vector3>{}
                         : poseHmdVertices(model, animation_pose,
                                           object.legacy_hmd_root_space);
  const auto back_color = retailHmdBackColor(object);
  const auto vertex = [&](std::size_t index) {
    if (!direct_legacy_pose) {
      return transformHmdVertex(posed_vertices[index], object.transform);
    }
    const auto &source = model.vertices()[index];
    const auto world = transformPoint(
        static_cast<double>(source.x), static_cast<double>(source.y),
        static_cast<double>(source.z),
        object.legacy_hmd_bones[model.vertexParts()[index]]);
    return makeVertex(world.x, world.y, world.z);
  };
  for (const auto &triangle : model.triangles()) {
    auto v0 = vertex(triangle.vertex_indices[0]);
    auto v1 = vertex(triangle.vertex_indices[1]);
    auto v2 = vertex(triangle.vertex_indices[2]);
    const std::array positions{v0, v1, v2};
    const auto near_status = classifyNearPlane(view, positions);
    if (near_status == NearPlaneStatus::outside) {
      ++stats.rejected;
      continue;
    }
    if (near_status == NearPlaneStatus::intersecting) {
      const std::array textured{
          makeTexturedVertex(v0, triangle.uv[0], back_color),
          makeTexturedVertex(v1, triangle.uv[1], back_color),
          makeTexturedVertex(v2, triangle.uv[2], back_color),
      };
      submitClippedTriangle(
          textured,
          TexturedMaterial{triangle.texture_page, triangle.clut, false, false,
                           static_cast<std::uint8_t>(texture_bank)},
          true, false, view, ordering_table, primitives.objects, stats);
      continue;
    }

    long xy0{};
    long xy1{};
    long xy2{};
    long depth_cue{};
    long flags{};
    const auto depth =
        RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue, &flags);
    if (depth <= 0) {
      ++stats.rejected;
      continue;
    }
    auto &primitive = primitives.objects.emplace_back();
    configurePrimitive(primitive, triangle, texture_bank, back_color);
    dynamicallyLight(primitive, positions);
    setProjected(&primitive.x0, xy0);
    setProjected(&primitive.x1, xy1);
    setProjected(&primitive.x2, xy2);
    depthCuePrimitive(primitive, {retailObjectDepthCue(cameraDepth(view, v0)),
                                  retailObjectDepthCue(cameraDepth(view, v1)),
                                  retailObjectDepthCue(cameraDepth(view, v2))});
    addPrim(&ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
            &primitive);
    const std::array projected{xy0, xy1, xy2};
    submitStats(stats, depth, projected);
  }
}

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
    RenderStats &stats) {
  const auto &presentation_camera = presentation.camera;
  const auto &shot = gameplay.lastShot();
  const auto &player = gameplay.player();
  const auto guest_effects_authoritative =
      presentation.guest_camera_lists_captured &&
      gameplay.legacyEffectParticlesAuthoritative();
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
  // The embedded GsSPRITE packet is already projected for the 20 Hz guest
  // camera. Its controller record is the stronger source: exact family,
  // frame, world position, scale and colour survive interpolation, aspect
  // changes and native first-person aim without a screen-space correction.
  // renderGuestCameraLists suppresses only these proven SPFX sprites below;
  // unrelated family-zero sprites (glass, weather and overlay effects) keep
  // their exact retail packets.
  std::vector<FireParticle> legacy_particles;
  legacy_particles.reserve(gameplay.legacyExplParticles().size());
  for (const auto &source : gameplay.legacyExplParticles()) {
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
      particles[index].y = static_cast<std::int32_t>(
          std::lround(projectile.y - std::sin(angle * 2.0) * expansion * 0.45));
      particles[index].z = static_cast<std::int32_t>(
          std::lround(projectile.z + std::sin(angle) * expansion));
      particles[index].frame = static_cast<std::uint8_t>(std::min<std::size_t>(
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

void renderEmdObject(const assets::EmdScene &scene,
                     const game::SceneObject &object, const MATRIX &view,
                     std::vector<OT_TAG> &ordering_table,
                     PrimitiveBuffer &primitives, RenderStats &stats,
                     bool retail_scrim = false) {
  const auto reverse_winding = reversesWinding(object.transform);
  const auto texture_bank = scene.textureBank();
  for (const auto &section : scene.sections()) {
    for (const auto &polygon : section.polygons) {
      if (!polygon.renderable) {
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
              false,
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
              TexturedMaterial{polygon.texture_page, polygon.clut, false, false,
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
                                const game::SceneObject &object,
                                VertexColor fallback) {
  const auto origin = transformPoint(0.0, 0.0, 0.0, object.transform);
  auto nearest_vertical_distance = std::numeric_limits<double>::max();
  auto result = fallback;
  const auto consider = [&](const assets::EmdSection &section,
                            const assets::EmdPolygon &polygon,
                            std::span<const std::uint16_t> live_colors,
                            std::array<std::size_t, 3U> corners) {
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
      return;
    }
    const auto vertical_distance = std::abs(sample->surface_y - origin.y);
    // Crate roots sit above their supporting plane. This bound spans the
    // complete retail crate while rejecting another storey of the map.
    constexpr auto maximum_vertical_distance = 768.0;
    if (vertical_distance > maximum_vertical_distance ||
        vertical_distance >= nearest_vertical_distance) {
      return;
    }
    nearest_vertical_distance = vertical_distance;
    result = {sample->color.red, sample->color.green, sample->color.blue};
  };
  for (const auto model_index : gameplay.activeModels()) {
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
      for (const auto &polygon : section.polygons) {
        if (!polygon.renderable) {
          continue;
        }
        consider(section, polygon, live_colors, {0U, 1U, 2U});
        if (polygon.quad) {
          consider(section, polygon, live_colors, {1U, 3U, 2U});
        }
      }
    }
  }
  return result;
}

void renderObjects(const game::GameplaySession &gameplay,
                   const FireAnimation &fire_animation,
                   const FireTexturePlacement &fire_texture_placement,
                   const game::ActorAnimationBank &actor_animations,
                   std::uint64_t actor_tick,
                   const RenderPresentationSnapshot &presentation,
                   const MATRIX &view, std::vector<OT_TAG> &ordering_table,
                   std::vector<OT_TAG> &fire_ordering_table,
                   PrimitiveBuffer &primitives, RenderStats &stats) {
  for (const auto object_index : gameplay.activeObjects()) {
    if (object_index >= presentation.objects.size()) {
      continue;
    }
    const auto &object = presentation.objects[object_index];
    const auto *displayed_model = gameplay.displayedObjectModel(object_index);
    if (displayed_model == nullptr) {
      continue;
    }
    const auto &object_model = *displayed_model;
    const auto &geometry = object_model.geometry;
    const auto texture_bank = gameplay.objectTextureBank(object_index);
    if (const auto *model = std::get_if<assets::GmdModel>(&geometry)) {
      // The guest display-light pointer is reliable for the two weapon-crate
      // classes. Several flat GMD signs reuse that field for material state;
      // interpreting it as RGB tinted those sprites magenta.
      const auto weapon_crate =
          object.class_id == 0x4fU || object.class_id == 0x50U;
      const auto guest_back_color = weapon_crate
                                        ? retailGmdObjectBackColor(object)
                                        : VertexColor{128U, 128U, 128U};
      const auto back_color =
          weapon_crate ? localSceneBackColor(gameplay, presentation, object,
                                             guest_back_color)
                       : guest_back_color;
      renderGmdObject(*model, object, object_model.visual_effect, texture_bank,
                      presentation.camera, view, ordering_table, primitives,
                      stats, back_color);
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
      renderHmdObject(*hmd_model, object, render_pose, texture_bank, view,
                      ordering_table, primitives, stats);
      auto weapon = gameplay.legacyDedicatedActorWeapon(object_index);
      if (!weapon && state != nullptr && state->health != 0U) {
        weapon = state->weapon;
      }
      if (weapon) {
        renderActorWeapon(gameplay, *hmd_model, object, pose, *weapon,
                          texture_bank, presentation.camera, view,
                          ordering_table, primitives, stats);
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
      renderEmdObject(std::get<assets::EmdScene>(geometry), object, view,
                      ordering_table, primitives, stats);
    }
  }
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
    const auto compact_pistol = [&item] {
      if (item.item >= game::weapon_slot_count) {
        return false;
      }
      switch (static_cast<game::WeaponId>(item.item)) {
      case game::WeaponId::silenced_9mm:
      case game::WeaponId::pistol_9mm:
      case game::WeaponId::unused_357:
      case game::WeaponId::pistol_45:
      case game::WeaponId::g_18:
        return true;
      default:
        return false;
      }
    }();
    const auto category_scale = compact_pistol ? 0.48 : 1.0;
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

void renderPresentedPlayer(const game::GameplaySession &gameplay,
                           const game::ActorAnimationBank &actor_animations,
                           std::uint64_t player_animation_tick,
                           game::WeaponId presented_weapon,
                           const RenderPresentationSnapshot &presentation,
                           const MATRIX &view,
                           std::vector<OT_TAG> &ordering_table,
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
    const auto player_texture_bank =
        gameplay.textureBankAt(gameplay.player().x, gameplay.player().z);
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
    renderHmdObject(*player_model, player_object, &pose, player_texture_bank,
                    view, ordering_table, primitives, stats);
    if (gameplay.playerAlive()) {
      renderActorWeapon(gameplay, *player_model, player_object, pose,
                        presented_weapon, player_texture_bank,
                        presentation.camera, view, ordering_table, primitives,
                        stats);
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
    if (sprite.effect_family != 0U) {
      // Exact SPFX controller state is rendered in world space by
      // renderWeaponEffects. Do not submit its embedded projected sprite a
      // second time.
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
    setRGB0(&primitive, sprite.color.red, sprite.color.green,
            sprite.color.blue);
    if ((sprite.attribute & 0x40000000U) != 0U) {
      setSemiTrans(&primitive, 1);
    }
    setShadeTex(&primitive, (sprite.attribute & 0x40U) != 0U ? 1 : 0);

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
          (environment->texture_page & 0x1fU) | (retail_tpage & ~0x1fU));
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
  for (const auto model_index : gameplay.activeModels()) {
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

void updateDynamicLightFrame(
    const game::GameplaySession &gameplay,
    const game::ActorAnimationBank &actor_animations,
    const RenderPresentationSnapshot &presentation,
    std::span<const game::LegacyWeaponEventBridgeState> weapon_edges,
    std::span<const game::LegacyMuzzleFlashPresentationState>
        retail_muzzle_flashes,
    std::span<const game::GameplayEffect> native_muzzle_flashes) {
  active_retail_vertex_lights.clear();
  active_retail_vertex_lights.reserve(presentation.vertex_lights.size());
  for (const auto &source : presentation.vertex_lights) {
    active_retail_vertex_lights.push_back(retailVertexLightState(source));
  }
  active_retail_light_projection = std::max(presentation.camera.projection, 1);

  std::vector<game::PersistentDynamicLightState> persistent;
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

  std::vector<game::TransientDynamicLightState> transient;
  transient.reserve(weapon_edges.size() + retail_muzzle_flashes.size() +
                    native_muzzle_flashes.size() + gameplay.effects().size() +
                    presentation.projectiles.size());
  for (const auto &edge : weapon_edges) {
    const auto weapon = static_cast<game::WeaponId>(edge.weapon);
    if (presentation.first_person_aim ||
        edge.type != game::LegacyWeaponEventType::shot ||
        !weaponUsesRetailMuzzleFlash(weapon)) {
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
    if (presentation.first_person_aim ||
        flash.source_slot == presentation.player_guest_slot) {
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
      std::span{directional}.first(directional_count));
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
    std::vector<OT_TAG> &scrim_ordering_table,
    std::vector<OT_TAG> &guest_overlay_ordering_table,
    std::vector<OT_TAG> &fire_ordering_table, PrimitiveBuffer &primitives) {
  ClearOTagR(reinterpret_cast<u_long *>(ordering_table.data()),
             ordering_table_size);
  ClearOTagR(reinterpret_cast<u_long *>(scrim_ordering_table.data()),
             ordering_table_size);
  ClearOTagR(reinterpret_cast<u_long *>(guest_overlay_ordering_table.data()),
             ordering_table_size);
  ClearOTagR(reinterpret_cast<u_long *>(fire_ordering_table.data()),
             ordering_table_size);
  primitives.reset();
  updateDynamicLightFrame(gameplay, actor_animations, presentation,
                          weapon_edges, retail_muzzle_flashes,
                          native_muzzle_flashes);
  const auto presented_player_weapon =
      presentation.flashlight_enabled ? game::WeaponId::flashlight
                                      : gameplay.hud().inventory().current();

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
  for (const auto model_index : gameplay.activeModels()) {
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
  std::size_t fire_particle_budget{};
  if (presentation.scrim.visible) {
    const auto *scrim = gameplay.detachedScrimModel();
    if (scrim == nullptr || !presentation.scrim.transform_valid) {
      throw core::Error{core::ErrorCode::not_found,
                        "Visible retail SCRIM is incomplete"};
    }
    add_to_budget(object_polygon_count, scrim->polygonCount(), "Retail SCRIM");
  }
  const auto add_weapon_model = [&](game::WeaponId weapon) {
    const auto *weapon_model = gameplay.weaponModel(weapon);
    if (weapon_model == nullptr) {
      return;
    }
    const auto *geometry =
        std::get_if<assets::GmdModel>(&weapon_model->geometry);
    if (geometry != nullptr) {
      add_to_budget(object_polygon_count, geometry->triangles().size(),
                    "Weapon model");
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
      if (const auto dedicated =
              gameplay.legacyDedicatedActorWeapon(object_index)) {
        add_weapon_model(*dedicated);
      } else if (state != nullptr && state->health != 0U) {
        add_weapon_model(state->weapon);
      }
    } else if (std::holds_alternative<game::ObjectFireEmitter>(geometry)) {
      if (!gameplay.legacyEffectParticlesAuthoritative()) {
        add_to_budget(fire_particle_budget, fire_particle_count, "Object fire");
      }
    } else {
      add_to_budget(object_polygon_count,
                    std::get<assets::EmdScene>(geometry).polygonCount(),
                    "Object scene");
    }
  }
  add_to_budget(object_polygon_count, glass_shards.size(), "Glass shards");
  if (const auto *player_model =
          std::get_if<assets::HmdModel>(&gameplay.playerModel().geometry)) {
    add_to_budget(object_polygon_count, player_model->triangles().size(),
                  "Player model");
    if (gameplay.playerAlive()) {
      add_weapon_model(presented_player_weapon);
    }
  }
  if (!presentation.guest_camera_lists_captured) {
    add_to_budget(fire_particle_budget, gameplay.legacyExplParticles().size(),
                  "Legacy effect");
  }
  const auto &shot = gameplay.lastShot();
  if (shot.fired && shot.weapon == game::WeaponId::flamethrower &&
      !gameplay.legacyEffectParticlesAuthoritative()) {
    add_to_budget(fire_particle_budget, 7U, "Flamethrower");
  }
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
  primitives.objects.reserve(object_polygon_count * maximum_clipped_triangles);
  primitives.object_quads.reserve(object_quad_count);
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
  primitives.lockStorage();

  SetRotMatrix(const_cast<MATRIX *>(&view));
  SetTransMatrix(const_cast<MATRIX *>(&view));
  RenderStats stats;
  for (const auto model_index : gameplay.activeModels()) {
    const auto &scene = gameplay.models()[model_index].scene;
    const auto texture_bank = scene.textureBank();
    for (auto section_index = std::size_t{};
         section_index < scene.sections().size(); ++section_index) {
      const auto &section = scene.sections()[section_index];
      const auto live_colors = liveWorldVertexColors(
          presentation, model_index, static_cast<std::uint16_t>(section_index),
          section.vertices.size());
      for (const auto &polygon : section.polygons) {
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
            const auto t0 =
                emdTexturedVertex(v0, section, polygon, 0, live_colors);
            const auto t1 =
                emdTexturedVertex(v1, section, polygon, 1, live_colors);
            const auto t2 =
                emdTexturedVertex(v2, section, polygon, 2, live_colors);
            const auto t3 =
                emdTexturedVertex(v3, section, polygon, 3, live_colors);
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
                                  worldOrderingDepth, true);
            submitClippedTriangle(second_triangle, material, false, false, view,
                                  ordering_table, primitives.triangles, stats,
                                  worldOrderingDepth, true);
            continue;
          }
          auto &primitive = primitives.quads.emplace_back();
          configurePrimitive(primitive, section, polygon, texture_bank,
                             texture_source_page, live_colors);
          dynamicallyLight(primitive, positions);
          depth = RotTransPers4(&v0, &v1, &v2, &v3, &xy0, &xy1, &xy2, &xy3,
                                &depth_cue, &flags);
          if (depth <= 0 || !frontFacing(xy0, xy1, xy2, 4U)) {
            primitives.quads.pop_back();
            ++stats.rejected;
            continue;
          }
          setProjected(&primitive.x0, xy0);
          setProjected(&primitive.x1, xy1);
          setProjected(&primitive.x2, xy2);
          setProjected(&primitive.x3, xy3);
          depthCuePrimitive(primitive,
                            {retailTerrainDepthCue(cameraDepth(view, v0)),
                             retailTerrainDepthCue(cameraDepth(view, v1)),
                             retailTerrainDepthCue(cameraDepth(view, v2)),
                             retailTerrainDepthCue(cameraDepth(view, v3))});
          addPrim(
              &ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
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
                emdTexturedVertex(v0, section, polygon, 0, live_colors),
                emdTexturedVertex(v1, section, polygon, 1, live_colors),
                emdTexturedVertex(v2, section, polygon, 2, live_colors),
            };
            submitClippedTriangle(
                textured,
                TexturedMaterial{polygon.texture_page, polygon.clut, false,
                                 false, static_cast<std::uint8_t>(texture_bank),
                                 texture_source_page},
                false, false, view, ordering_table, primitives.triangles, stats,
                worldOrderingDepth, true);
            continue;
          }
          auto &primitive = primitives.triangles.emplace_back();
          configurePrimitive(primitive, section, polygon, texture_bank,
                             texture_source_page, live_colors);
          dynamicallyLight(primitive, positions);
          depth = RotTransPers3(&v0, &v1, &v2, &xy0, &xy1, &xy2, &depth_cue,
                                &flags);
          if (depth <= 0 || !frontFacing(xy0, xy1, xy2, 3U)) {
            primitives.triangles.pop_back();
            ++stats.rejected;
            continue;
          }
          setProjected(&primitive.x0, xy0);
          setProjected(&primitive.x1, xy1);
          setProjected(&primitive.x2, xy2);
          depthCuePrimitive(primitive,
                            {retailTerrainDepthCue(cameraDepth(view, v0)),
                             retailTerrainDepthCue(cameraDepth(view, v1)),
                             retailTerrainDepthCue(cameraDepth(view, v2))});
          addPrim(
              &ordering_table[std::clamp(depth, 1, ordering_table_size - 1)],
              &primitive);
          const std::array projected{xy0, xy1, xy2};
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
    // Retail flag 0x00200000 disables distance fade/culling and 0x00100000
    // excludes SCRIM from local vertex lights. Native presentation also gives
    // this semantic background its own non-depth-writing pass: the sky keeps
    // its retail painter order but can never compete with level geometry in Z.
    renderEmdObject(*scrim, object, view, scrim_ordering_table, primitives,
                    stats, true);
  }
  renderObjects(gameplay, fire_animation, fire_texture_placement,
                actor_animations, actor_tick, presentation, view,
                ordering_table, fire_ordering_table, primitives, stats);
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
                      primitives, stats);
  renderGuestCameraLists(gameplay, textures, presentation, retained_sprites,
                         retained_raw_packets, view, ordering_table,
                         guest_overlay_ordering_table, primitives, stats);
  renderPresentedPlayer(gameplay, actor_animations, player_animation_tick,
                        presented_player_weapon, presentation, view,
                        ordering_table, primitives, stats);
  if (!primitives.storageStable()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Primitive storage moved while building the OT"};
  }
  return stats;
}

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
                             int x, int y, std::uint8_t brightness = 128U) {
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
      GetTPage(texturePageMode(image.mode()), 0, page_x, page_y);

  DR_TPAGE page{};
  SetDrawTPage(&page, 1, 0, texture_page);
  DrawPrim(&page);

  // PsyCross uses the active page for its texture format and the polygon's
  // page for UV lookup, so both must be explicit after the world OT.
  POLY_FT4 polygon{};
  setPolyFT4(&polygon);
  polygon.tpage = texture_page;
  polygon.clut = GetClut(hud_resident_clut_x, hud_resident_clut_y);
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
                   std::uint8_t brightness = 128U) {
  drawHudSpriteAtResident(image, hudResidentX(image.pixels().x),
                          image.pixels().y, x, y, brightness);
}

void drawHudSpriteScaled(const assets::TimImage &image, float x, float y,
                         float width, float height,
                         std::uint8_t brightness = 128U) {
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
                        int offset_x, int offset_y) {
  const auto &font_page = textures.image("FONTA.TIM");
  const ScopedPsyCrossFontTexture font_binding{textures.nativeFont()};
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

std::size_t originalHudDrawableGlyphCount(std::string_view text) noexcept {
  return static_cast<std::size_t>(
      std::ranges::count_if(text, [](char character) {
        return game::originalHudGlyph(character).has_value();
      }));
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

void drawOriginalHudTextBlockScaled(const HudTextureAtlas &textures,
                                    std::string_view text, float left,
                                    float top, float available_width,
                                    float scale,
                                    game::LegacyRgbBridgeState color) {
  auto line = 0U;
  for (auto cursor = std::size_t{}; cursor <= text.size(); ++line) {
    const auto newline = text.find('\n', cursor);
    const auto end = newline == std::string_view::npos ? text.size() : newline;
    const auto line_text = text.substr(cursor, end - cursor);
    const auto line_width =
        static_cast<float>(game::originalHudTextWidth(line_text)) * scale;
    drawOriginalHudTextScaled(
        textures, line_text, left + (available_width - line_width) * 0.5F,
        top + static_cast<float>(line) * 9.0F * scale, scale, color);
    if (newline == std::string_view::npos) {
      break;
    }
    cursor = newline + 1U;
  }
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

std::string uppercaseAscii(std::string_view text) {
  auto result = std::string{text};
  for (auto &character : result) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
  }
  return result;
}

std::optional<std::string> rifleScopeEnglishSource(
    const game::LegacyUiMessageBridgeState &message) {
  if (!game::russianLanguageActive()) {
    return std::nullopt;
  }
  const auto resolve = [](std::string_view observed)
      -> std::optional<std::string> {
    if (observed.empty()) {
      return std::nullopt;
    }
    if (const auto completed = game::completeGameplayTextSource(observed)) {
      return std::string{*completed};
    }
    return std::nullopt;
  };
  if (auto source = resolve(message.text)) {
    return source;
  }
  return resolve(reconstructOriginalHudText(message.glyphs));
}

bool drawBoundKeyboardMousePrompt(
    const HudTextureAtlas &textures,
    const game::LegacyUiMessageBridgeState &message,
    const KeyboardMouseBindings &bindings, int offset_x, int offset_y) {
  const auto prompt = keyboardMousePromptText(message.text, bindings);
  if (!prompt || message.glyphs.empty()) {
    return false;
  }

  const auto retail_glyph_count =
      originalHudDrawableGlyphCount(prompt->retail_text);
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

bool drawLocalizedGameplayMessage(
    const HudTextureAtlas &textures,
    const game::LegacyUiMessageBridgeState &message, int offset_x, int offset_y,
    const game::LegacyUiBackdropBridgeState *layout_backdrop,
    std::string_view forced_english = {}) {
  if (message.glyphs.empty() ||
      (!game::russianLanguageActive() && forced_english.empty())) {
    return false;
  }
  std::string source;
  std::string localized;
  const auto resolve = [&](std::string_view observed) {
    if (observed.empty()) {
      return false;
    }
    source = std::string{observed};
    localized = game::localizeTextCopy(source);
    if (localized != source) {
      return true;
    }
    const auto completed = game::completeGameplayTextSource(observed);
    if (!completed) {
      return false;
    }
    source = std::string{*completed};
    localized = game::localizeTextCopy(source);
    return localized != source;
  };
  if (!forced_english.empty()) {
    source = std::string{forced_english};
    // The ViT atlas intentionally replaces lowercase ASCII cells with
    // Cyrillic capitals. Rifle scopes retain retail English, so use the
    // untouched uppercase ASCII cells instead of sampling those remapped
    // slots and producing mixed-language garbage.
    localized = uppercaseAscii(forced_english);
  } else {
    if (!resolve(message.text) &&
        !resolve(reconstructOriginalHudText(message.glyphs))) {
      return false;
    }
  }

  const auto source_glyph_count =
      std::max(std::size_t{1U}, originalHudDrawableGlyphCount(source));
  const auto localized_glyph_count = originalHudDrawableGlyphCount(localized);
  if (source_glyph_count == 0U || localized_glyph_count == 0U) {
    return false;
  }
  const auto visible_localized_glyphs = std::min(
      localized_glyph_count, (message.glyphs.size() * localized_glyph_count +
                              source_glyph_count - 1U) /
                                 source_glyph_count);
  const auto visible_text =
      originalHudTextGlyphPrefix(localized, visible_localized_glyphs);

  const auto left = std::ranges::min_element(
      message.glyphs, {}, &game::LegacyUiGlyphBridgeState::x);
  const auto top = std::ranges::min_element(message.glyphs, {},
                                            &game::LegacyUiGlyphBridgeState::y);
  const auto original_left = static_cast<int>(left->x);
  // Retail positions the first visible glyph against the final centered
  // line, even while the type-on reveal is incomplete. Use the full source
  // metric so the Russian line does not change scale as glyphs appear.
  const auto original_width = originalHudMaximumLineWidth(source);
  const auto localized_width = originalHudMaximumLineWidth(localized);
  if (original_width <= 0 || localized_width <= 0) {
    return false;
  }
  auto text_left = original_left;
  auto available_width = original_width;
  if (layout_backdrop != nullptr) {
    const auto [left_corner, right_corner] =
        std::ranges::minmax_element(layout_backdrop->corners, {},
                                    &game::LegacyProjectedPointBridgeState::x);
    constexpr int backdrop_padding = 4;
    text_left = static_cast<int>(left_corner->x) + backdrop_padding;
    available_width = std::max(1, static_cast<int>(right_corner->x) -
                                      static_cast<int>(left_corner->x) -
                                      backdrop_padding * 2);
  }
  const auto scale =
      std::min(1.0F, static_cast<float>(available_width) / localized_width);
  const auto x = static_cast<float>(screen_width / 2 + offset_x + text_left);
  const auto y = static_cast<float>(screen_height / 2 + offset_y + top->y);
  drawOriginalHudTextBlockScaled(textures, visible_text, x, y,
                                 static_cast<float>(available_width), scale,
                                 message.glyphs.front().color);
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

void drawOriginalStatusScale(int offset_x, int offset_y) {
  constexpr auto red = std::uint8_t{107U};
  constexpr auto green = std::uint8_t{126U};
  constexpr auto blue = std::uint8_t{231U};
  drawSolidRect(10 + offset_x, 26 + offset_y, 1, 31, red, green, blue);
  for (const auto y : {26, 41, 56}) {
    drawSolidRect(11 + offset_x, y + offset_y, 4, 1, red, green, blue);
  }
}

void drawOriginalRadar(const game::GameplaySession &gameplay,
                       const game::OriginalRadarGeometry &geometry,
                       int offset_x, int offset_y) {
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
          (game::normalizeHeading(
               static_cast<std::int64_t>(gameplay.player().yaw) + 1024) +
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
  const auto basis = game::headingBasis(gameplay.player().yaw);
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

void drawOriginalAimReticle(int center_x, int center_y, bool head_target) {
  constexpr auto red = std::uint8_t{64U};
  constexpr auto green = std::uint8_t{255U};
  constexpr auto blue = std::uint8_t{64U};
  const auto geometry = game::originalAimReticleGeometry(head_target);
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
  drawSolidRect(left - geometry.horizontal_ray, center_y,
                geometry.horizontal_ray, 1, red, green, blue);
  drawSolidRect(right + 1, center_y, geometry.horizontal_ray, 1, red, green,
                blue);
  drawSolidRect(center_x, top - geometry.vertical_ray, 1, geometry.vertical_ray,
                red, green, blue);
  drawSolidRect(center_x, bottom + 1, 1, geometry.vertical_ray, red, green,
                blue);
  drawSolidRect(center_x - 1, center_y, 3, 1, red, green, blue);
}

void drawOriginalWorldCallout(const HudTextureAtlas &textures,
                              std::string_view text, int center_x,
                              int center_y) {
  const auto geometry = game::originalHeadshotCalloutGeometry();
  LINE_F3 callout{};
  setLineF3(&callout);
  setRGB0(&callout, 224U, 224U, 224U);
  setXY3(&callout, static_cast<float>(center_x + geometry.start_x),
         static_cast<float>(center_y + geometry.start_y),
         static_cast<float>(center_x + geometry.elbow_x),
         static_cast<float>(center_y + geometry.elbow_y),
         static_cast<float>(center_x + geometry.end_x),
         static_cast<float>(center_y + geometry.end_y));
  DrawPrim(&callout);

  const auto localized = game::localizeTextCopy(text);
  drawOriginalHudTextSolid(textures, localized, center_x + geometry.text_x,
                           center_y + geometry.text_y, 224U);
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

  // The target box/crosshair is already the exact FUN_80041830 raw packet
  // stream. This native layer supplies only authored TIM assets which cannot
  // yet be relocated losslessly from guest VRAM.
  const auto &scope = textures.image("SCOPED.TIM");
  drawHudSprite(scope, center_x - static_cast<int>(scope.displayWidth()) / 2,
                center_y - static_cast<int>(scope.displayHeight()) / 2);

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
                    origin_y + offset_y - heights[layer] / 2);
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
                     int offset_x, int offset_y) {
  const auto &hud = gameplay.hud();
  const auto target_locked = gameplay.targetLocked();
  const auto project_anchor =
      [&camera, offset_x,
       offset_y](const Vector3 &anchor) -> std::optional<std::pair<int, int>> {
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
    return std::pair{static_cast<int>(std::lround(projected_x)),
                     static_cast<int>(std::lround(projected_y))};
  };

  // HUD is a depth-free native 384x240 pass. Finish any queued world/full-
  // screen primitive first so text, radar blend and weapon layers cannot be
  // replayed with stale PGXP or blend state.
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);

  // Retail uses its FONTA/B/C text layer here. ARMOR.TIM is a green legacy
  // image and is not the white in-game label seen above the lavender bar.
  const auto primary_status = game::localizeTextCopy(
      game::originalPrimaryStatusLabel(hud.primaryStatus()));
  drawOriginalHudTextSolid(textures, primary_status, 20 + offset_x,
                           18 + offset_y);
  const auto health_color = hud.healthBarColor();
  drawOriginalStatusBar(26, hud.displayedPrimaryTrail(), hud.primaryReveal(),
                        health_color.red, health_color.green, health_color.blue,
                        offset_x, offset_y);
  drawOriginalStatusBar(26, hud.displayedPrimaryBar(), hud.primaryReveal(),
                        150U, 150U, 255U, offset_x, offset_y);

  if (hud.dangerReveal() != 0U) {
    const auto danger = game::localizeTextCopy("DANGER");
    drawOriginalHudTextSolid(textures, danger, 20 + offset_x, 33 + offset_y);
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
    drawOriginalStatusBar(41, hud.displayedDangerBar(), hud.dangerReveal(), red,
                          green, blue, offset_x, offset_y);
  }

  if (hud.targetReveal() != 0U) {
    const auto target = game::localizeTextCopy("TARGET");
    drawOriginalHudTextSolid(textures, target, 20 + offset_x, 48 + offset_y);
    drawOriginalStatusBar(56, hud.displayedTargetBar(), hud.targetReveal(),
                          100U, 255U, 100U, offset_x, offset_y);
  }

  drawOriginalStatusScale(offset_x, offset_y);

  // Commit the status layer before the radar switches to average blending.
  // This also makes the ARMOR/HEALTH label independent of radar visibility.
  DrawSync(0);
  drawOriginalRadar(gameplay, game::originalRadarGeometry(hud.revealFrame()),
                    offset_x, offset_y);
  drawOriginalWeaponMenu(textures, hud, offset_x, offset_y);

  const auto &definition = hud.inventory().currentDefinition();
  const auto reveal_remaining =
      static_cast<int>(game::GameplayHud::reveal_duration) -
      static_cast<int>(hud.revealFrame());
  const auto reveal_slide =
      reveal_remaining * 271 /
      static_cast<int>(game::GameplayHud::reveal_duration);
  const auto switch_slide =
      static_cast<int>(hud.weaponSwitchFrames()) * 98 /
      static_cast<int>(game::GameplayHud::weapon_switch_duration);
  const auto horizontal_slide = reveal_slide + switch_slide;
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
    constexpr int icon_origin_x = screen_width / 2 + 133;
    constexpr int icon_origin_y = screen_height / 2 + 91;
    for (std::size_t index = 0; index < icon_layers.size(); ++index) {
      drawHudSprite(textures.image(icon_layers[index]),
                    icon_origin_x + offset_x + horizontal_slide +
                        icon_offsets[index],
                    icon_origin_y + offset_y - heights[index] / 2);
    }
  }
  if (definition.shows_ammo) {
    const auto &weapon = hud.inventory().currentState();
    const auto ammo = game::originalAmmoText(definition, weapon);
    // FUN_800410d0 supplies a left text origin, not a centre point.
    constexpr int ammo_x = screen_width / 2 + 118;
    constexpr int ammo_y = screen_height / 2 + 94;
    drawOriginalHudText(textures, ammo, ammo_x + offset_x + horizontal_slide,
                        ammo_y + offset_y);
  }
  const auto weapon = hud.inventory().current();
  const auto thrown = weapon == game::WeaponId::fragmentation_grenade ||
                      weapon == game::WeaponId::gas_grenade;
  const auto scoped =
      first_person_aim && (weapon == game::WeaponId::nightvision_rifle ||
                           weapon == game::WeaponId::sniper_rifle);
  if ((first_person_aim || target_locked) && !thrown) {
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
    if (!scoped) {
      auto center_x = screen_width / 2 + offset_x;
      auto center_y = screen_height / 2 + offset_y;
      auto reticle_visible = first_person_aim;
      if (first_person_aim) {
        center_y += static_cast<int>(
            std::lround(gameplay.manualAimReticleVerticalOffset()));
      }
      if (target_locked) {
        reticle_visible = false;
        if (target_anchor) {
          // Follow the animated HMD Head/Chest part. A fixed world height
          // drifts when the mission transform scales actors.
          if (const auto projected = project_anchor(*target_anchor)) {
            center_x = projected->first;
            center_y = projected->second;
            reticle_visible = true;
          }
        }
      }
      if (reticle_visible) {
        drawOriginalAimReticle(center_x, center_y, gameplay.headshotTargeted());
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
          center_x = projected->first;
          center_y = projected->second;
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

  for (const auto &callout : world_callouts) {
    if (callout.headshot) {
      continue;
    }
    if (const auto projected = project_anchor(callout.point)) {
      drawOriginalWorldCallout(textures, callout.text, projected->first,
                               projected->second);
    }
  }

  // These packets are the completed output of retail's FONT/TEXT state
  // machine. Coordinates are relative to the PS1 draw offset (192,120), and
  // already include wrapping, alignment, status stacking and HUD slide-in.
  // The status POLY_F4 is submitted behind all glyph packets.
  for (const auto &message : gameplay.legacyUiMessages()) {
    if (message.backdrop) {
      drawRetailUiBackdrop(*message.backdrop, offset_x, offset_y);
    }
  }
  const auto status_backdrop = std::ranges::find_if(
      gameplay.legacyUiMessages(), [](const auto &message) {
        return message.channel == game::LegacyUiMessageChannel::status &&
               message.backdrop.has_value();
      });
  if (const auto &timer = gameplay.legacyUiTimer()) {
    drawRetailUiGlyphs(textures, timer->glyphs, offset_x, offset_y);
  }
  for (const auto &message : gameplay.legacyUiMessages()) {
    const auto *layout_backdrop =
        message.backdrop ? &*message.backdrop
        : message.channel == game::LegacyUiMessageChannel::status &&
                status_backdrop != gameplay.legacyUiMessages().end()
            ? &*status_backdrop->backdrop
            : nullptr;
    const auto scope_english =
        scoped ? rifleScopeEnglishSource(message) : std::nullopt;
    if (!drawLocalizedGameplayMessage(
            textures, message, offset_x, offset_y, layout_backdrop,
            scope_english ? std::string_view{*scope_english}
                          : std::string_view{}) &&
        !drawBoundKeyboardMousePrompt(textures, message, bindings, offset_x,
                                      offset_y)) {
      drawRetailUiGlyphs(textures, message.glyphs, offset_x, offset_y);
    }
  }

  // DrawPrim only queues immediate primitives in PsyCross. Flush the HUD
  // before the scene is presented; ordering-table draws were already flushed.
  DrawSync(0);
  GR_EnableDepth(1);
}

void drawMissionFailedOverlay(const HudTextureAtlas &textures) {
  DrawSync(0);
  GR_SetBlendMode(BM_NONE);
  GR_EnableDepth(0);
  drawSolidRect(0, 0, screen_width, screen_height, 0U, 0U, 0U);
  const auto failed = game::localizeText("MISSION FAILED");
  const auto retry = game::localizeText("FIRE OR ACTION TO RETRY");
  drawOriginalHudTextSolid(
      textures, failed, (screen_width - game::originalHudTextWidth(failed)) / 2,
      screen_height / 2 - 12, 255U);
  drawOriginalHudTextSolid(
      textures, retry, (screen_width - game::originalHudTextWidth(retry)) / 2,
      screen_height / 2 + 10, 180U);
  DrawSync(0);
  GR_EnableDepth(1);
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

int menuCharacterAdvance(char source) noexcept {
  const auto value = static_cast<unsigned char>(source);
  if (value == 1U || value == 2U) {
    return 10;
  }
  if (source == ' ') {
    return 4;
  }
  const auto glyph = originalHudGlyph(static_cast<char>(value));
  return glyph ? originalHudGlyphAdvance(*glyph) : 0;
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
  auto width = 0;
  for (const auto source : text) {
    width +=
        std::max(1, static_cast<int>(std::lround(menuCharacterAdvance(source) *
                                                 scale * horizontal_scale)));
  }
  return width == 0 ? 0
                    : width - std::max(1, static_cast<int>(std::lround(
                                              2 * scale * horizontal_scale)));
}

std::string expandPauseHints(std::string_view source) {
  std::string result;
  result.reserve(source.size());
  for (std::size_t index = 0; index < source.size(); ++index) {
    if (source[index] == '%' && index + 1 < source.size()) {
      if (source[index + 1] == 'x') {
        result.push_back('\x01');
        ++index;
        continue;
      }
      if (source[index + 1] == 't') {
        result.push_back('\x02');
        ++index;
        continue;
      }
    }
    result.push_back(source[index] == '\t' ? ' ' : source[index]);
  }
  return result;
}

void drawMenuText(std::string_view source, const game::PauseRect &bounds,
                  int scale, PauseRgb color, game::PauseTextAlignment alignment,
                  int line_height, const HudTextureAtlas &interface_textures) {
  const auto localized = game::localizeTextCopy(source);
  const auto text = expandPauseHints(localized);
  const auto retail_line_height = std::max(line_height, 1);
  // Eight pixels is the physical glyph height. Inter-line leading may shrink
  // from the retail default when a translated paragraph needs the room, but
  // glyphs themselves are never vertically filtered or overlapped.
  const auto maximum_lines =
      std::max(1, static_cast<int>(bounds.height) / (8 * scale));
  if (maximum_lines == 1) {
    const auto newline = text.find('\n');
    const auto line_text = std::string_view{text}.substr(0, newline);
    const auto natural_width = menuLineWidth(line_text, scale);
    // A one-line retail label is never clipped. Localized labels may be
    // proportionally condensed, but every authored glyph remains visible.
    const auto horizontal_scale =
        natural_width <= bounds.width || natural_width == 0
            ? 1.0F
            : static_cast<float>(bounds.width) /
                  static_cast<float>(natural_width);
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
        top + (draw_height - layer_height) * 0.5F, layer_width, layer_height);
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
                   std::uint64_t animation_tick) {
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
  const auto draw_text =
      [&interface_textures](
          std::string_view source, const game::PauseRect &bounds, int scale,
          PauseRgb color,
          game::PauseTextAlignment alignment = game::PauseTextAlignment::left,
          int line_height = 10) {
        drawMenuText(source, bounds, scale, color, alignment, line_height,
                     interface_textures);
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
    const auto drawn =
        command.kind == game::PauseRenderKind::weapon_icon
            ? drawPauseWeaponIcon(interface_textures, command.id,
                                  animated_bounds)
            : drawPauseTexture(textures, command.asset, animated_bounds);
    const auto fallback =
        !drawn && command.kind == game::PauseRenderKind::weapon_icon &&
                !command.asset.empty()
            ? drawPauseTexture(textures, command.asset, animated_bounds)
            : std::optional<game::PauseRect>{};
    if (drawn || fallback) {
      texture_uploaded = true;
      if (command.kind == game::PauseRenderKind::asset &&
          std::string_view{command.asset}.starts_with("MAP")) {
        map_command_bounds = animated_bounds;
        map_draw_bounds = drawn ? *drawn : *fallback;
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

} // namespace

struct PsyCrossCampaignSaveRenderer::State {
  explicit State(const game::MissionPackage &mission) : textures{mission} {}

  HudTextureAtlas textures;
  std::uint64_t animation_tick{};
};

PsyCrossCampaignSaveRenderer::PsyCrossCampaignSaveRenderer(
    const game::MissionPackage &mission)
    : state_{std::make_unique<State>(mission)} {}

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
    drawMenuText(value, bounds, 1, color, alignment, 11, state_->textures);
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
  } else {
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
      const auto active = menu.slotSelection() == index;
      drawBorderedRect(
          {52, static_cast<std::int16_t>(76 + index * 24), 165, 17},
          {0U, 0U, 0U}, active ? selected : muted);
      text(label, {56, static_cast<std::int16_t>(81 + index * 24), 157, 9},
           active ? selected : normal);
    }
    text("Choose a save slot", {236, 44, 109, 48});
    text("%x save   %t back", game::PauseAcdLayout::hint, normal);
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
    drawMenuText(value, bounds, 1, color, alignment, 11, state_->textures);
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
    if (cheats_.all_weapons &&
        !gameplay.activateRetailAllWeaponsCheat()) {
      PsyX_Log_Error("Retail all-weapons cheat activation failed\n");
    }
    if (cheats_.hard_mode && !gameplay.setRetailHardMode(true)) {
      PsyX_Log_Error("Retail hard-mode cheat activation failed\n");
    }
    if (cheats_.one_shot_kills &&
        !gameplay.setRetailOneShotKills(true)) {
      PsyX_Log_Error("Retail one-shot cheat activation failed\n");
    }
    if (cheats_.weak_enemies &&
        !gameplay.setRetailWeakEnemies(true)) {
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
                gameplay.currentRoom(), gameplay.activeModels().size(),
                gameplay.activeObjects().size(), gameplay.player().x,
                gameplay.player().y, gameplay.player().z,
                gameplay.player().yaw);

  std::vector<OT_TAG> ordering_table(ordering_table_size);
  std::vector<OT_TAG> scrim_ordering_table(ordering_table_size);
  std::vector<OT_TAG> guest_overlay_ordering_table(ordering_table_size);
  std::vector<OT_TAG> fire_ordering_table(ordering_table_size);
  PrimitiveBuffer primitives;
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
  constexpr unsigned int maximum_updates_per_presentation = 2U;
  constexpr unsigned int maximum_audio_backlog_updates = 30U;
  constexpr unsigned int maximum_audio_updates_per_presentation = 30U;
  constexpr unsigned int coherent_audio_updates_per_presentation = 6U;
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
  int pause_analog_x = 0;
  int pause_analog_y = 0;
  double dpad_down_held_seconds = 0.0;
  double simulation_accumulator_seconds = 0.0;
  double audio_accumulator_seconds = 0.0;
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
  glass_shatter.reset(gameplay);
  RenderPresentationSnapshot current_render_presentation;
  captureRenderPresentation(gameplay, current_render_presentation);
  weapon_presentation_edges.observe(gameplay.legacyPresentationFrame());
  muzzle_flash_edges.observe(gameplay.effects());
  scrim_copy_edges.observe(gameplay.legacyPresentationFrame());
  auto previous_render_presentation = current_render_presentation;
  auto interpolated_render_presentation = current_render_presentation;
  const auto reset_render_presentation = [&] {
    captureRenderPresentation(gameplay, current_render_presentation);
    previous_render_presentation = current_render_presentation;
    interpolated_render_presentation = current_render_presentation;
    weapon_presentation_edges.reset();
    muzzle_flash_edges.reset();
    glass_shatter.reset(gameplay);
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
  const auto draw_gameplay_frame = [&](bool advance_effects) {
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
    applyRetailEnvironment(presentation.environment, camera.projection,
                           presentation.retail_environment_active,
                           retail_blackout_mission);
    SetGeomScreen(camera.projection);
    const auto target_anchor = aimTargetAnchor(
        gameplay, actor_animations, actor_tick, presentation.objects);
    const auto world_callouts = worldCalloutAnchors(
        gameplay, actor_animations, actor_tick, presentation.objects);
    auto view = makeViewMatrix(camera);
    registerPreciseViewMatrix(view, camera);
    // Retail pickups and flying grenade billboards use interface art but are
    // inserted into the world OT. Their TIMs must already be resident when
    // that OT executes; a late HUD upload would lose scene depth ordering.
    hud_textures.restoreGameplay(gameplay.hud(), presentation.first_person_aim,
                                 presentation.dropped_items,
                                 presentation.projectiles);
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
        scrim_ordering_table, guest_overlay_ordering_table, fire_ordering_table,
        primitives);
    if (advance_effects) {
      fire_animation.update();
      glass_shatter.advance();
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
    if (!gameplay.cinematic() && !gameplay.missionComplete()) {
      drawGameplayHud(hud_textures, gameplay, camera, target_anchor,
                      world_callouts, input_, presentation.first_person_aim,
                      aim_heading, pause_settings.screen_center_x,
                      pause_settings.screen_center_y);
      drawRetailGrenadeTrajectory(presentation, pause_settings.screen_center_x,
                                  pause_settings.screen_center_y);
    }
    if (gameplay.missionFailed()) {
      drawMissionFailedOverlay(hud_textures);
    } else if (gameplay.missionComplete()) {
      drawMissionCompleteOverlay(hud_textures);
    }
    drawMapFade(gameplay.mapFade());
    return stats;
  };
  static_cast<void>(
      police_lightbar_animation.synchronize(gameplay, actor_tick, textures));
  std::array<psx::SpuPcmFrame, 4096U> gameplay_pcm{};
  AudioOutputCatchUpPolicy audio_catch_up{
      coherent_audio_updates_per_presentation};
  std::uint64_t audio_slices_completed{};
  std::uint64_t audio_pcm_frames_pumped{};
  std::uint64_t audio_pcm_blocks_pumped{};
  std::uint64_t audio_guest_clear_events{};
  std::uint64_t audio_catch_up_events{};
  std::uint64_t audio_guest_pcm_frames_read{};
  std::uint64_t audio_guest_pcm_frames_trimmed{};
  AudioOutputCadencePolicy audio_output_cadence{psx::Spu::sample_rate,
                                                retail_audio_callback_hz};
  std::size_t audio_output_frames_due{};
  std::vector<psx::SpuPcmFrame> gameplay_pcm_batch;
  gameplay_pcm_batch.reserve(psx::Spu::pcm_queue_capacity);
  const auto pump_gameplay_audio = [&] {
    if (audio_output_frames_due != 0U) {
      gameplay_pcm_batch.clear();
      while (const auto count = gameplay.takePcm(gameplay_pcm)) {
        audio_guest_pcm_frames_read += count;
        gameplay_pcm_batch.insert(
            gameplay_pcm_batch.end(), gameplay_pcm.begin(),
            gameplay_pcm.begin() + static_cast<std::ptrdiff_t>(count));
      }

      const auto output_count =
          std::min(audio_output_frames_due, gameplay_pcm_batch.size());
      const auto trimmed = gameplay_pcm_batch.size() - output_count;
      audio_guest_pcm_frames_trimmed += trimmed;
      if (output_count != 0U) {
        gameplay_audio->queue(
            std::span<const psx::SpuPcmFrame>{gameplay_pcm_batch}.last(
                output_count));
        audio_pcm_frames_pumped += output_count;
        ++audio_pcm_blocks_pumped;
        audio_output_frames_due -= output_count;
      }
    }
    // Submit the fractional 128-frame tail as well. Holding it until the next
    // retail callback leaves every host batch up to 2.9 ms too short and can
    // repeatedly starve OpenAL at an exact VSYNC boundary.
    gameplay_audio->flush();
  };
  const auto discard_gameplay_audio = [&](std::string_view reason) {
    gameplay_audio->reset(reason);
    gameplay.clearPcm();
    ++audio_guest_clear_events;
    audio_catch_up.reset();
    audio_output_cadence.reset();
    audio_output_frames_due = 0U;
    gameplay_pcm_batch.clear();
    audio_accumulator_seconds = 0.0;
  };

  const auto pending_audio_updates = [&](double accumulator_seconds) {
    return std::min<std::size_t>(
        maximum_audio_backlog_updates,
        static_cast<std::size_t>((accumulator_seconds + 1.0e-9) /
                                 retail_audio_step_seconds));
  };
  const auto begin_audio_catch_up = [&](double accumulator_seconds) {
    if (!audio_catch_up.beginFrame(
            pending_audio_updates(accumulator_seconds))) {
      return;
    }
    ++audio_catch_up_events;
    PsyX_Log_Warning(
        "[AudioDiag][catch-up] sequence=%llu pending=%zu accumulator_ms=%.3f\n",
        static_cast<unsigned long long>(audio_catch_up_events),
        pending_audio_updates(accumulator_seconds),
        accumulator_seconds * 1000.0);
    // Preserve the live host queue: resetting it here turns a transient long
    // frame into an audible dropout. Guest PCM older than the bounded recovery
    // window is still discarded below, so no stale tail can accumulate.
    gameplay.clearPcm();
    audio_output_frames_due = 0U;
    gameplay_pcm_batch.clear();
    ++audio_guest_clear_events;
  };
  const auto finish_audio_update = [&](double accumulator_seconds) {
    const auto remaining_updates = pending_audio_updates(accumulator_seconds);
    if (!audio_catch_up.retainCompletedUpdate(remaining_updates)) {
      gameplay.clearPcm();
      ++audio_guest_clear_events;
      return false;
    }
    return true;
  };
  const auto service_realtime_audio = [&]() {
    begin_audio_catch_up(audio_accumulator_seconds);
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
      const auto frames_due = audio_output_cadence.advanceCallback();
      if (finish_audio_update(audio_accumulator_seconds)) {
        audio_output_frames_due =
            std::min(audio_output_frames_due + frames_due,
                     static_cast<std::size_t>(psx::Spu::pcm_queue_capacity));
      }
    }
    // The guest can mix more samples than wall time permits while executing a
    // callback. Submit only the exact 120 Hz wall-clock budget and retain its
    // newest samples, preventing both an ever-growing delay and stale audio
    // after a heavy frame or mission restart.
    pump_gameplay_audio();
    return true;
  };

  const auto end_presented_frame = [] { PsyX_EndScene(); };
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
        "pcm_blocks=%llu guest_pcm_read=%llu pcm_trimmed=%llu pcm_due=%zu "
        "guest_clears=%llu catch_ups=%llu\n",
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
        static_cast<unsigned long long>(audio_guest_pcm_frames_trimmed),
        audio_output_frames_due,
        static_cast<unsigned long long>(audio_guest_clear_events),
        static_cast<unsigned long long>(audio_catch_up_events));
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
    const auto elapsed_seconds =
        std::clamp(performance_frequency == 0U
                       ? 0.0
                       : static_cast<double>(counter_delta) /
                             static_cast<double>(performance_frequency),
                   0.0, maximum_frame_time_seconds);
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
    const auto manual_aim_down = bound_actions[KeyboardMouseAction::aim];
    mouse_capture.set(!paused && manual_aim_down);
    int mouse_delta_x{};
    int mouse_delta_y{};
    SDL_GetRelativeMouseState(&mouse_delta_x, &mouse_delta_y);
    pause_was_down = pause_down;
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
    const auto pause_toggled = (pressed & pause_button) != 0 || pause_pressed;
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
                                      pause_animation_tick++));
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
          .confirm = acd_navigation_ready && (pressed & confirm_button) != 0,
          .cancel = acd_navigation_ready && (pressed & cancel_button) != 0,
          .pause = pause_toggled,
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
        const auto enabled = *detected_cheat == game::RetailCheat::one_shot_kills
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
                                      pause_animation_tick++));
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
    // Retail posts the failure text immediately, waits 0xc8 gameplay
    // ticks, then completes its fade before state 2 requests the actual
    // checkpoint restore. Do not let a face-button press bypass that VM
    // timeline while the failure overlay is visible.
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
      mapped_input = {};
      log_next_frame = true;
      restore_gameplay_textures(true);
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
        // In chase these are locomotion axes. Under L1 the guest interprets
        // the same exact PS1 channels as vertical/horizontal sight motion and
        // keeps Gabe's root fixed.
        .move = movement_armed ? mapped_input.move_forward : 0.0,
        .turn = movement_armed ? mapped_input.turn : 0.0,
        .run = movement_armed && mapped_input.run.held,
        .aim = manual_aim,
        .next_weapon = mapped_input.next_weapon.pressed,
        .previous_weapon = mapped_input.previous_weapon.pressed,
        .quick_weapon = mapped_input.quick_weapon.pressed,
        // Q/E or physical L2/R2 retain the original manual-aim corner strafe.
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
      gameplay.update(simulation_input);
      if (auto carry = gameplay.campaignCarryState()) {
        latest_campaign_carry = std::move(carry);
      }
      weapon_presentation_edges.observe(gameplay.legacyPresentationFrame());
      muzzle_flash_edges.observe(gameplay.effects());
      scrim_copy_edges.observe(gameplay.legacyPresentationFrame());
      captureRenderPresentation(gameplay, current_render_presentation);
      ++simulation_updates_this_frame;
      simulation_accumulator_seconds = std::max(
          0.0, simulation_accumulator_seconds - retail_simulation_step_seconds);

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
      textures.ensure(gameplay);
      log_next_frame = true;
    }
    if (gameplay.hud().inventory().current() != previous_weapon) {
      previous_weapon = gameplay.hud().inventory().current();
      textures.ensure(gameplay);
    }
    if (gameplay.projectiles().size() != previous_projectile_count) {
      previous_projectile_count = gameplay.projectiles().size();
      textures.ensure(gameplay);
    }
    const auto dynamic_fire = std::ranges::any_of(
        gameplay.projectiles(), [](const game::GameplayProjectile &projectile) {
          return projectile.active &&
                 projectile.phase == game::ProjectilePhase::explosion;
        });
    if (dynamic_fire != previous_dynamic_fire) {
      previous_dynamic_fire = dynamic_fire;
      textures.ensure(gameplay);
    }
    // If the renderer missed more than three VBlanks, move native visual
    // clocks to the final completed guest state before submitting it. The
    // last step remains after the draw, preserving the normal pose boundary.
    for (auto update = 1U; update < simulation_updates_this_frame; ++update) {
      fire_animation.update();
      ++actor_tick;
      gameplay.advanceAnimationClock();
    }
    const auto stats = draw_gameplay_frame(simulation_updates_this_frame != 0U);
    submitted_presentation_sequence = presentation_sequence;
    if (simulation_updates_this_frame != 0U) {
      // Root motion and the rendered HMD pose must sample the same retail
      // tick. Advance only after that pose has been submitted.
      gameplay.advanceAnimationClock();
    }
    if (log_next_frame) {
      PsyX_Log_Info("Gameplay frame: room=%u models=%zu objects=%zu "
                    "submitted=%zu rejected=%zu depth=%d..%d "
                    "screen=(%d..%d,%d..%d)\n",
                    gameplay.currentRoom(), gameplay.activeModels().size(),
                    gameplay.activeObjects().size(), stats.submitted,
                    stats.rejected, stats.minimum_depth, stats.maximum_depth,
                    stats.minimum_x, stats.maximum_x, stats.minimum_y,
                    stats.maximum_y);
      log_next_frame = false;
    }
    end_presented_frame();
  }
  return SceneViewerResult{previous_buttons, SceneExitReason::exit_application};
}

} // namespace sf::platform::detail
