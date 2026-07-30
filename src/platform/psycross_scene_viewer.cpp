#include "psycross_scene_viewer.hpp"
#include "muzzle_flash_texture.hpp"
#include "psycross_audio_output.hpp"
#include "psycross_font_texture.hpp"
#include "psycross_movie_player.hpp"
#include "psycross_runtime_guards.hpp"
#if defined(SF_ENABLE_SURFACE_PICKER)
#include "psycross_surface_picker.hpp"
#endif
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
#include "sf/platform/actor_shadow_stability.hpp"
#include "sf/platform/gameplay_message_reveal_policy.hpp"
#include "sf/platform/player_camera_fade.hpp"
#include "sf/platform/player_input.hpp"
#include "sf/platform/presentation_frame_meter.hpp"
#include "sf/platform/retail_scope_text_policy.hpp"
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
// Mission residency starts with physical pages 6..31 and spills into the
// host-only extension. Keep the PC HUD copy in framebuffer pages 0..3 instead
// of restoring its retail page-12 atlas over streamed wall textures.
constexpr std::uint16_t hud_resident_clut_x = 0U;
constexpr std::uint16_t hud_resident_clut_y = 254U;
// Combat sprites occupy the unused bottom 64 rows of native CFIRE page 5.
// CFIRE uses rows 0..191, so this keeps every authored combat texel persistent
// while leaving every complete mission page available to the streamer.
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

[[nodiscard]] bool textureDiagnosticsEnabled() noexcept {
  static const auto enabled = [] {
    const auto *value = SDL_getenv("SF_TEXTURE_DIAGNOSTICS");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
  }();
  return enabled;
}
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

#include "psycross_scene_textures.inc"
#include "psycross_scene_presentation.inc"
#include "psycross_scene_render_core.inc"
#include "psycross_scene_models.inc"
#include "psycross_scene_effects.inc"
#include "psycross_scene_world.inc"
#include "psycross_scene_hud.inc"
#include "psycross_scene_pause.inc"
} // namespace

#include "psycross_scene_runtime.inc"
#include "psycross_scene_save_renderer.inc"
} // namespace sf::platform::detail
