#include "sf/assets/emd_scene.hpp"
#include "sf/assets/tim_image.hpp"
#include "sf/core/error.hpp"
#include "sf/game/dynamic_lighting.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/internal/g4_campaign_transition_probe_access.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

constexpr std::uint32_t default_probe_frames = 240U;

struct EnvironmentKey {
  sf::game::LegacyRgbBridgeState clear;
  sf::game::LegacyRgbBridgeState back;
  sf::game::LegacyRgbBridgeState fog;
  std::int32_t dqa{};
  std::int32_t dqb{};
  std::uint32_t terrain_depth_cue{};
  bool background{};

  [[nodiscard]] friend bool operator<(const EnvironmentKey &left,
                                      const EnvironmentKey &right) noexcept {
    return std::tie(left.clear.red, left.clear.green, left.clear.blue,
                    left.back.red, left.back.green, left.back.blue,
                    left.fog.red, left.fog.green, left.fog.blue, left.dqa,
                    left.dqb, left.terrain_depth_cue, left.background) <
           std::tie(right.clear.red, right.clear.green, right.clear.blue,
                    right.back.red, right.back.green, right.back.blue,
                    right.fog.red, right.fog.green, right.fog.blue, right.dqa,
                    right.dqb, right.terrain_depth_cue, right.background);
  }
};

struct SpriteKey {
  std::uint16_t tpage{};
  std::uint8_t u{};
  std::uint8_t v{};
  std::uint16_t width{};
  std::uint16_t height{};
  std::uint16_t center_x{};
  std::uint16_t center_y{};

  [[nodiscard]] friend auto operator<=>(const SpriteKey &,
                                        const SpriteKey &) = default;
};

struct MissionStats {
  std::uint64_t first_sequence{};
  std::uint64_t last_sequence{};
  std::size_t frames{};
  std::size_t sprite_samples{};
  std::size_t line_samples{};
  std::size_t raw_packet_samples{};
  std::size_t line_particle_samples{};
  std::size_t raw_particle_line_matches{};
  std::size_t raw_particle_line_nonvisible{};
  std::size_t raw_particle_line_misses{};
  std::size_t raw_particle_line_ambiguities{};
  std::size_t ribbon_samples{};
  std::size_t maximum_sprites{};
  std::size_t maximum_lines{};
  std::size_t maximum_raw_packets{};
  std::size_t minimum_raw_packets{std::numeric_limits<std::size_t>::max()};
  std::size_t maximum_ribbons{};
  std::size_t matched_sprite_samples{};
  std::size_t unmatched_sprite_samples{};
  std::size_t ambiguous_sprite_samples{};
  std::size_t scrim_resource_samples{};
  std::size_t scrim_copy_phases{};
  std::size_t scrim_idle_phases{};
  bool previous_scrim_copy_active{};
  std::vector<sf::game::LegacyVramMoveBridgeState> scrim_moves;
  std::set<EnvironmentKey> environments;
  std::set<std::uint32_t> active_depth_cues;
  std::set<std::uint32_t> effective_depth_cues;
  std::set<std::uint16_t> renderer_display_flags;
  std::set<std::uint16_t> renderer_flags;
  std::set<std::uint32_t> screen_filter_materials;
  std::set<std::uint32_t> screen_filter_colors;
  std::set<std::uint32_t> nightvision_clear_colors;
  std::size_t screen_filter_frames{};
  std::size_t nightvision_frames{};
  std::size_t renderer_darkness_frames{};
  std::set<std::array<std::int16_t, 3U>> hmd_back_colors;
  std::size_t hmd_back_color_samples{};
  std::set<std::uint16_t> active_world_models;
  std::set<std::uint16_t> resident_world_models;
  std::set<std::uint16_t> world_vertex_colors;
  std::size_t world_vertex_color_samples{};
  std::size_t active_world_vertex_samples{};
  std::size_t active_world_black_samples{};
  std::size_t active_world_dim_samples{};
  std::size_t maximum_vertex_lights{};
  std::size_t flashlight_frames{};
  std::set<SpriteKey> sprites;
  std::set<std::uint8_t> raw_opcodes;
  std::map<std::uint16_t, std::size_t> raw_packet_formats;
  std::set<std::pair<std::int16_t, std::int16_t>> raw_line_deltas;
  std::set<std::uint64_t> raw_frame_fingerprints;
  std::int16_t minimum_raw_x{std::numeric_limits<std::int16_t>::max()};
  std::int16_t minimum_raw_y{std::numeric_limits<std::int16_t>::max()};
  std::int16_t maximum_raw_x{std::numeric_limits<std::int16_t>::min()};
  std::int16_t maximum_raw_y{std::numeric_limits<std::int16_t>::min()};
  std::size_t raw_line_samples{};
  std::map<std::string, std::size_t> sprite_assets;
};

struct TimPlacement {
  std::string name;
  unsigned int page{};
  unsigned int u{};
  unsigned int v{};
  unsigned int width{};
  unsigned int height{};
  unsigned int clut_x{};
  unsigned int clut_y{};
};

struct WorldMaterialSignature {
  std::size_t polygon_count{};
  std::uint64_t placement_fingerprint{1469598103934665603ULL};
  std::set<std::uint8_t> resolved_pages;
};

[[nodiscard]] std::vector<TimPlacement>
specialEffectPlacements(const sf::assets::HogArchive &archive) {
  std::vector<TimPlacement> result;
  for (const auto &entry : archive.entries()) {
    if (!std::string_view{entry.name}.ends_with(".TIM")) {
      continue;
    }
    const auto image = sf::assets::TimImage::parse(archive.file(entry.name));
    if (!image.clut() || (image.mode() != sf::assets::TimPixelMode::indexed4 &&
                          image.mode() != sf::assets::TimPixelMode::indexed8)) {
      continue;
    }
    const auto page = static_cast<unsigned int>(image.pixels().x / 64U) +
                      static_cast<unsigned int>(image.pixels().y / 256U) * 16U;
    const auto page_x = (page & 15U) * 64U;
    const auto page_y = page > 15U ? 256U : 0U;
    const auto pixels_per_word =
        image.mode() == sf::assets::TimPixelMode::indexed4 ? 4U : 2U;
    result.push_back(TimPlacement{
        entry.name,
        page,
        (static_cast<unsigned int>(image.pixels().x) - page_x) *
            pixels_per_word,
        static_cast<unsigned int>(image.pixels().y) - page_y,
        image.displayWidth(),
        image.displayHeight(),
        image.clut()->x,
        image.clut()->y,
    });
  }
  return result;
}

[[nodiscard]] bool
spriteInside(const sf::game::LegacyGuestSpriteBridgeState &sprite,
             const TimPlacement &placement) noexcept {
  return (sprite.tpage & 0x1fU) == placement.page &&
         sprite.center_x == placement.clut_x &&
         sprite.center_y == placement.clut_y && sprite.u >= placement.u &&
         sprite.v >= placement.v &&
         static_cast<unsigned int>(sprite.u) + sprite.width <=
             placement.u + placement.width &&
         static_cast<unsigned int>(sprite.v) + sprite.height <=
             placement.v + placement.height;
}

[[nodiscard]] std::optional<std::uint32_t>
parseUnsigned(std::string_view text) {
  std::uint32_t result{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] bool validRawPacket(
    const sf::game::LegacyGuestRawPacketBridgeState &packet) noexcept {
  if (packet.word_count == 0U ||
      packet.word_count > sf::game::legacy_guest_raw_packet_words ||
      packet.opcode != static_cast<std::uint8_t>(packet.words[0] >> 24U) ||
      packet.opcode == 0U || (packet.opcode & 0x80U) != 0U) {
    return false;
  }
  const auto base = static_cast<std::uint8_t>(packet.opcode & 0xfdU);
  return (packet.word_count == 2U && base == 0x68U) ||
         (packet.word_count == 3U && base == 0x40U) ||
         (packet.word_count == 4U && (base == 0x20U || base == 0x50U)) ||
         (packet.word_count == 6U && base == 0x30U);
}

[[nodiscard]] bool validRibbon(
    const sf::game::LegacyPark2FlamethrowerRibbonBridgeState &ribbon) noexcept {
  return ribbon.slot < 72U &&
         ribbon.frame == static_cast<std::uint8_t>(2U + (ribbon.slot & 3U)) &&
         ribbon.ordering_depth != 0U && ribbon.ordering_depth < 4096U &&
         std::ranges::all_of(
             ribbon.corners,
             [](const auto &corner) {
               return corner.x >= -1024 && corner.x <= 1023 &&
                      corner.y >= -1024 && corner.y <= 1023;
             });
}

void hashWord(std::uint64_t &hash, std::uint64_t value) noexcept {
  constexpr std::uint64_t prime = 1099511628211ULL;
  for (std::size_t byte = 0U; byte < sizeof(value); ++byte) {
    hash ^= (value >> (byte * 8U)) & 0xffU;
    hash *= prime;
  }
}

[[nodiscard]] std::uint32_t
readVlfPageMask(const sf::game::MissionPackage &package) {
  const auto bytes = package.archive().file("VLF.RFF");
  if (bytes.size() < sizeof(std::uint32_t)) {
    throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                          "VLF page mask is truncated"};
  }
  return std::to_integer<std::uint32_t>(bytes[0]) |
         (std::to_integer<std::uint32_t>(bytes[1]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[2]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] WorldMaterialSignature worldGroundMaterialSignature(
    const sf::game::MissionPackage &package, std::size_t model_index,
    std::string_view expected_name, std::uint16_t raw_texture_page,
    std::uint16_t clut, std::uint8_t minimum_u, std::uint8_t minimum_v,
    std::uint8_t maximum_u, std::uint8_t maximum_v) {
  if (model_index >= package.worldModels().entries().size() ||
      package.worldModels().entries()[model_index].name != expected_name) {
    throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                          "Retail world-model order does not match profile"};
  }
  const auto &entry = package.worldModels().entries()[model_index];
  const auto scene =
      sf::assets::EmdScene::parse(package.worldModels().file(entry.name));
  const auto vlf_page_mask = readVlfPageMask(package);
  WorldMaterialSignature result;
  std::size_t section_index{};
  for (const auto &section : scene.sections()) {
    std::size_t polygon_index{};
    for (const auto &polygon : section.polygons) {
      const auto used_vertices = polygon.quad ? 4U : 3U;
      auto actual_minimum_u = std::uint8_t{0xffU};
      auto actual_minimum_v = std::uint8_t{0xffU};
      auto actual_maximum_u = std::uint8_t{};
      auto actual_maximum_v = std::uint8_t{};
      const auto first_y = section.vertices[polygon.vertex_indices[0]].y;
      auto horizontal = true;
      for (std::size_t vertex = 0U; vertex < used_vertices; ++vertex) {
        const auto &uv = polygon.uv[vertex];
        actual_minimum_u = std::min(actual_minimum_u, uv.u);
        actual_minimum_v = std::min(actual_minimum_v, uv.v);
        actual_maximum_u = std::max(actual_maximum_u, uv.u);
        actual_maximum_v = std::max(actual_maximum_v, uv.v);
        horizontal =
            horizontal &&
            section.vertices[polygon.vertex_indices[vertex]].y == first_y;
      }
      if (!polygon.renderable || !horizontal ||
          polygon.texture_page != raw_texture_page || polygon.clut != clut ||
          actual_minimum_u != minimum_u || actual_minimum_v != minimum_v ||
          actual_maximum_u != maximum_u || actual_maximum_v != maximum_v) {
        ++polygon_index;
        continue;
      }
      const auto resolved_page = sf::assets::resolveEmdTexturePageSource(
          polygon.texture_page, scene.texturePageMask(), vlf_page_mask);
      if (!resolved_page) {
        throw sf::core::Error{
            sf::core::ErrorCode::invalid_format,
            "Retail ground material has an ambiguous texture-page source"};
      }
      result.resolved_pages.insert(*resolved_page);
      ++result.polygon_count;
      hashWord(result.placement_fingerprint, section_index);
      hashWord(result.placement_fingerprint, polygon_index);
      hashWord(result.placement_fingerprint, polygon.quad);
      hashWord(result.placement_fingerprint, polygon.texture_page);
      hashWord(result.placement_fingerprint, polygon.clut);
      hashWord(result.placement_fingerprint, *resolved_page);
      for (std::size_t vertex = 0U; vertex < used_vertices; ++vertex) {
        const auto vertex_index = polygon.vertex_indices[vertex];
        const auto &position = section.vertices[vertex_index];
        hashWord(result.placement_fingerprint, vertex_index);
        hashWord(result.placement_fingerprint,
                 static_cast<std::uint16_t>(position.x));
        hashWord(result.placement_fingerprint,
                 static_cast<std::uint16_t>(position.y));
        hashWord(result.placement_fingerprint,
                 static_cast<std::uint16_t>(position.z));
        hashWord(result.placement_fingerprint, position.color);
        hashWord(result.placement_fingerprint, polygon.uv[vertex].u);
        hashWord(result.placement_fingerprint, polygon.uv[vertex].v);
      }
      ++polygon_index;
    }
    ++section_index;
  }
  return result;
}

[[nodiscard]] std::uint64_t auxiliaryFingerprint(
    const sf::game::LegacyGameplayBridgeState &state) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  const auto hash_rgb = [&](const auto &color) {
    hashWord(hash, color.red);
    hashWord(hash, color.green);
    hashWord(hash, color.blue);
  };
  hash_rgb(state.environment.clear_color);
  hash_rgb(state.environment.back_color);
  hash_rgb(state.environment.fog_color);
  hashWord(hash, static_cast<std::uint32_t>(state.environment.fog_dqa));
  hashWord(hash, static_cast<std::uint32_t>(state.environment.fog_dqb));
  hashWord(hash, state.environment.terrain_depth_cue);
  hashWord(hash, state.environment.background_enabled);
  hashWord(hash, state.guest_sprites.size());
  for (const auto &sprite : state.guest_sprites) {
    hashWord(hash, sprite.attribute);
    hashWord(hash, static_cast<std::uint16_t>(sprite.x));
    hashWord(hash, static_cast<std::uint16_t>(sprite.y));
    hashWord(hash, sprite.width);
    hashWord(hash, sprite.height);
    hashWord(hash, sprite.tpage);
    hashWord(hash, sprite.u);
    hashWord(hash, sprite.v);
    hashWord(hash, sprite.center_x);
    hashWord(hash, sprite.center_y);
    hash_rgb(sprite.color);
    hashWord(hash, static_cast<std::uint16_t>(sprite.mapping_x));
    hashWord(hash, static_cast<std::uint16_t>(sprite.mapping_y));
    hashWord(hash, static_cast<std::uint16_t>(sprite.scale_x));
    hashWord(hash, static_cast<std::uint16_t>(sprite.scale_y));
    hashWord(hash, static_cast<std::uint32_t>(sprite.rotation));
    hashWord(hash, sprite.ordering_depth);
  }
  hashWord(hash, state.guest_lines.size());
  for (const auto &line : state.guest_lines) {
    hashWord(hash, line.attribute);
    hashWord(hash, static_cast<std::uint16_t>(line.first.x));
    hashWord(hash, static_cast<std::uint16_t>(line.first.y));
    hashWord(hash, static_cast<std::uint16_t>(line.second.x));
    hashWord(hash, static_cast<std::uint16_t>(line.second.y));
    hash_rgb(line.first_color);
    hash_rgb(line.second_color);
  }
  hashWord(hash, state.guest_raw_packets.size());
  for (const auto &packet : state.guest_raw_packets) {
    hashWord(hash, packet.ordering_depth);
    hashWord(hash, packet.word_count);
    hashWord(hash, packet.opcode);
    for (std::size_t word = 0U; word < packet.word_count; ++word) {
      hashWord(hash, packet.words[word]);
    }
  }
  hashWord(hash, state.park2_flamethrower_ribbons.size());
  for (const auto &ribbon : state.park2_flamethrower_ribbons) {
    for (const auto &corner : ribbon.corners) {
      hashWord(hash, static_cast<std::uint16_t>(corner.x));
      hashWord(hash, static_cast<std::uint16_t>(corner.y));
    }
    hash_rgb(ribbon.color);
    hashWord(hash, ribbon.ordering_depth);
    hashWord(hash, ribbon.slot);
    hashWord(hash, ribbon.frame);
  }
  return hash;
}

[[nodiscard]] bool hasAuxiliaryCommands(
    const sf::game::LegacyGameplayBridgeState &state) noexcept {
  return !state.guest_sprites.empty() || !state.guest_lines.empty() ||
         !state.guest_raw_packets.empty() ||
         !state.park2_flamethrower_ribbons.empty();
}

[[nodiscard]] std::optional<std::string>
observeFrame(const sf::game::LegacyPresentationFrame &frame,
             std::uint64_t previous_sequence,
             const std::vector<TimPlacement> &effect_placements,
             MissionStats &stats) {
  if (!sf::game::legacyPresentationFrameConsumable(frame, previous_sequence)) {
    return "presentation frame is stale or incoherent";
  }
  const auto &state = frame.renderer->state;
  if (state.guest_sprites.size() > 512U || state.guest_lines.size() > 512U ||
      state.guest_raw_packets.size() > 1024U ||
      state.park2_flamethrower_ribbons.size() > 72U) {
    return "retail camera auxiliary list exceeds its exact capacity";
  }
  if (!std::ranges::all_of(state.guest_sprites, [](const auto &sprite) {
        return sprite.tpage < 0x20U;
      })) {
    return "retail sprite list contains a non-GPU texture page";
  }
  if (!std::ranges::all_of(state.guest_raw_packets, validRawPacket)) {
    return "retail raw list contains a malformed or unsupported packet";
  }
  if (!std::ranges::all_of(state.park2_flamethrower_ribbons, validRibbon)) {
    return "PARK2 ribbon list contains a malformed projected packet";
  }
  if (state.scrim.resource_present) {
    ++stats.scrim_resource_samples;
    if (!state.scrim.vram_moves.empty()) {
      if (state.scrim.vram_moves.size() != 13U) {
        return "retail SCRIM copy chain is not thirteen packets";
      }
      if (stats.scrim_moves.empty()) {
        stats.scrim_moves = state.scrim.vram_moves;
      } else if (stats.scrim_moves != state.scrim.vram_moves) {
        return "retail SCRIM copy packet order changed between frames";
      }
      for (const auto &move : state.scrim.vram_moves) {
        if ((move.source_x & 63) + move.width > 64 ||
            (move.source_y & 255) + move.height > 256 ||
            (move.destination_x & 63) + move.width > 64 ||
            (move.destination_y & 255) + move.height > 256) {
          return "retail SCRIM copy crosses a texture-page boundary";
        }
      }
    }
    if (state.scrim.vram_moves_active) {
      if (stats.previous_scrim_copy_active) {
        return "retail SCRIM emitted consecutive positive copy phases";
      }
      ++stats.scrim_copy_phases;
    } else if (!state.scrim.vram_moves.empty()) {
      ++stats.scrim_idle_phases;
    }
    stats.previous_scrim_copy_active = state.scrim.vram_moves_active;
  } else {
    stats.previous_scrim_copy_active = false;
  }

  if (stats.frames == 0U) {
    stats.first_sequence = frame.sequence;
  }
  stats.last_sequence = frame.sequence;
  ++stats.frames;
  stats.sprite_samples += state.guest_sprites.size();
  stats.line_samples += state.guest_lines.size();
  stats.raw_packet_samples += state.guest_raw_packets.size();
  stats.line_particle_samples += state.line_particles.size();
  stats.ribbon_samples += state.park2_flamethrower_ribbons.size();
  stats.maximum_sprites =
      std::max(stats.maximum_sprites, state.guest_sprites.size());
  stats.maximum_lines = std::max(stats.maximum_lines, state.guest_lines.size());
  stats.maximum_raw_packets =
      std::max(stats.maximum_raw_packets, state.guest_raw_packets.size());
  stats.minimum_raw_packets =
      std::min(stats.minimum_raw_packets, state.guest_raw_packets.size());
  stats.maximum_ribbons =
      std::max(stats.maximum_ribbons, state.park2_flamethrower_ribbons.size());
  stats.environments.insert(EnvironmentKey{
      state.environment.clear_color,
      state.environment.back_color,
      state.environment.fog_color,
      state.environment.fog_dqa,
      state.environment.fog_dqb,
      state.environment.terrain_depth_cue,
      state.environment.background_enabled,
  });
  stats.active_depth_cues.insert(state.environment.active_terrain_depth_cue);
  stats.effective_depth_cues.insert(
      state.environment.effectiveTerrainDepthCue());
  stats.renderer_display_flags.insert(state.environment.renderer_display_flags);
  stats.renderer_flags.insert(state.environment.renderer_flags);
  stats.screen_filter_materials.insert(
      state.environment.screen_filter_material);
  stats.screen_filter_colors.insert(
      static_cast<std::uint32_t>(state.environment.screen_filter_color.red) |
      (static_cast<std::uint32_t>(state.environment.screen_filter_color.green)
       << 8U) |
      (static_cast<std::uint32_t>(state.environment.screen_filter_color.blue)
       << 16U));
  stats.nightvision_clear_colors.insert(
      static_cast<std::uint32_t>(
          state.environment.nightvision_clear_color.red) |
      (static_cast<std::uint32_t>(
           state.environment.nightvision_clear_color.green)
       << 8U) |
      (static_cast<std::uint32_t>(
           state.environment.nightvision_clear_color.blue)
       << 16U));
  stats.screen_filter_frames += state.environment.screen_filter_enabled;
  stats.nightvision_frames += state.environment.nightvision_enabled;
  stats.renderer_darkness_frames +=
      state.environment.renderer_darkness_enabled ? 1U : 0U;
  for (const auto &object : state.objects) {
    if (!object.hmd_back_color_valid) {
      continue;
    }
    stats.hmd_back_colors.insert(object.hmd_back_color_q12);
    ++stats.hmd_back_color_samples;
  }
  stats.active_world_models.insert(state.active_world_models.begin(),
                                   state.active_world_models.end());
  stats.resident_world_models.insert(state.resident_world_models.begin(),
                                     state.resident_world_models.end());
  stats.maximum_vertex_lights =
      std::max(stats.maximum_vertex_lights, state.vertex_lights.size());
  stats.flashlight_frames += state.flashlight_enabled ? 1U : 0U;
  for (const auto &section : state.world_vertex_colors) {
    stats.world_vertex_color_samples += section.colors.size();
    stats.world_vertex_colors.insert(section.colors.begin(),
                                     section.colors.end());
    if (std::ranges::find(state.active_world_models, section.model) !=
        state.active_world_models.end()) {
      stats.active_world_vertex_samples += section.colors.size();
      stats.active_world_black_samples += static_cast<std::size_t>(
          std::ranges::count(section.colors, std::uint16_t{}));
      stats.active_world_dim_samples += static_cast<std::size_t>(
          std::ranges::count_if(section.colors, [](std::uint16_t packed) {
            const auto red = packed & 0x1fU;
            const auto green = (packed >> 5U) & 0x1fU;
            const auto blue = (packed >> 10U) & 0x1fU;
            return std::max({red, green, blue}) <= 4U;
          }));
    }
  }
  for (const auto &sprite : state.guest_sprites) {
    stats.sprites.insert(SpriteKey{sprite.tpage, sprite.u, sprite.v,
                                   sprite.width, sprite.height, sprite.center_x,
                                   sprite.center_y});
    std::size_t matches{};
    for (const auto &placement : effect_placements) {
      if (spriteInside(sprite, placement)) {
        ++stats.sprite_assets[placement.name];
        ++matches;
      }
    }
    if (matches == 0U) {
      ++stats.unmatched_sprite_samples;
    } else {
      ++stats.matched_sprite_samples;
      if (matches > 1U) {
        ++stats.ambiguous_sprite_samples;
      }
    }
  }
  auto raw_frame_fingerprint = std::uint64_t{1469598103934665603ULL};
  hashWord(raw_frame_fingerprint, state.guest_raw_packets.size());
  for (const auto &packet : state.guest_raw_packets) {
    if (packet.effect_particle >= 0) {
      const auto particle = static_cast<std::uint16_t>(packet.effect_particle);
      const auto matches = static_cast<std::size_t>(std::ranges::count_if(
          state.line_particles,
          [particle](const auto &line) { return line.particle == particle; }));
      if (matches == 1U) {
        ++stats.raw_particle_line_matches;
      } else if (matches == 0U) {
        if (packet.word_count == 4U && packet.opcode == 0x52U &&
            packet.words[1] == packet.words[3]) {
          ++stats.raw_particle_line_nonvisible;
        } else {
          ++stats.raw_particle_line_misses;
        }
      } else {
        ++stats.raw_particle_line_ambiguities;
      }
    }
    stats.raw_opcodes.insert(packet.opcode);
    const auto format = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(packet.word_count) << 8U) | packet.opcode);
    ++stats.raw_packet_formats[format];
    hashWord(raw_frame_fingerprint, packet.ordering_depth);
    hashWord(raw_frame_fingerprint, packet.word_count);
    hashWord(raw_frame_fingerprint, packet.opcode);
    for (std::size_t word = 0U; word < packet.word_count; ++word) {
      hashWord(raw_frame_fingerprint, packet.words[word]);
    }
    if (packet.word_count == 4U && packet.opcode == 0x52U) {
      const auto first_x = static_cast<std::int16_t>(packet.words[1]);
      const auto first_y = static_cast<std::int16_t>(packet.words[1] >> 16U);
      const auto second_x = static_cast<std::int16_t>(packet.words[3]);
      const auto second_y = static_cast<std::int16_t>(packet.words[3] >> 16U);
      stats.minimum_raw_x = std::min({stats.minimum_raw_x, first_x, second_x});
      stats.minimum_raw_y = std::min({stats.minimum_raw_y, first_y, second_y});
      stats.maximum_raw_x = std::max({stats.maximum_raw_x, first_x, second_x});
      stats.maximum_raw_y = std::max({stats.maximum_raw_y, first_y, second_y});
      stats.raw_line_deltas.emplace(
          static_cast<std::int16_t>(second_x - first_x),
          static_cast<std::int16_t>(second_y - first_y));
      ++stats.raw_line_samples;
    }
  }
  stats.raw_frame_fingerprints.insert(raw_frame_fingerprint);
  return std::nullopt;
}

void printRgb(const sf::game::LegacyRgbBridgeState &color) {
  std::cout << static_cast<unsigned int>(color.red) << ':'
            << static_cast<unsigned int>(color.green) << ':'
            << static_cast<unsigned int>(color.blue);
}

void printStats(const sf::game::MissionDefinition &mission,
                const MissionStats &stats) {
  std::cout << "mission=" << mission.index
            << " resource=" << mission.resource_name
            << " frames=" << stats.frames
            << " sequence=" << stats.first_sequence << ".."
            << stats.last_sequence << " aux-max=" << stats.maximum_sprites
            << '/' << stats.maximum_lines << '/' << stats.maximum_raw_packets
            << " aux-samples=" << stats.sprite_samples << '/'
            << stats.line_samples << '/' << stats.raw_packet_samples
            << " world-lines=" << stats.line_particle_samples << '/'
            << stats.raw_particle_line_matches << '/'
            << stats.raw_particle_line_nonvisible << '/'
            << stats.raw_particle_line_misses << '/'
            << stats.raw_particle_line_ambiguities
            << " sprite-coverage=" << stats.matched_sprite_samples << '/'
            << stats.unmatched_sprite_samples << '/'
            << stats.ambiguous_sprite_samples
            << " ribbons=" << stats.maximum_ribbons << '/'
            << stats.ribbon_samples << " scrim=" << stats.scrim_resource_samples
            << '/' << stats.scrim_copy_phases << '/' << stats.scrim_idle_phases
            << '/' << stats.scrim_moves.size()
            << " env=" << stats.environments.size()
            << " depth-live/effective/dark=" << stats.active_depth_cues.size()
            << '/' << stats.effective_depth_cues.size() << '/'
            << stats.renderer_darkness_frames
            << " retail-lights=" << stats.maximum_vertex_lights << '/'
            << stats.flashlight_frames
            << " world-colors=" << stats.world_vertex_colors.size() << '/'
            << stats.world_vertex_color_samples
            << " active-dark=" << stats.active_world_black_samples << '/'
            << stats.active_world_dim_samples << '/'
            << stats.active_world_vertex_samples;
  for (const auto &environment : stats.environments) {
    std::cout << " [clear=";
    printRgb(environment.clear);
    std::cout << " back=";
    printRgb(environment.back);
    std::cout << " fog=";
    printRgb(environment.fog);
    std::cout << " dqa=" << environment.dqa << " dqb=" << environment.dqb
              << " depth=0x" << std::hex << environment.terrain_depth_cue
              << std::dec << " bg=" << environment.background << ']';
  }
  std::cout << " depth-live=";
  for (const auto cue : stats.active_depth_cues) {
    std::cout << "0x" << std::hex << cue << ',';
  }
  std::cout << " effective=";
  for (const auto cue : stats.effective_depth_cues) {
    std::cout << "0x" << std::hex << cue << ',';
  }
  std::cout << " display=";
  for (const auto flags : stats.renderer_display_flags) {
    std::cout << "0x" << std::hex << flags << ',';
  }
  std::cout << " camera-flags=";
  for (const auto flags : stats.renderer_flags) {
    std::cout << "0x" << std::hex << flags << ',';
  }
  std::cout << " filter=" << std::dec << stats.screen_filter_frames << ':';
  for (const auto material : stats.screen_filter_materials) {
    std::cout << "m" << material << ',';
  }
  for (const auto color : stats.screen_filter_colors) {
    std::cout << "0x" << std::hex << color << ',';
  }
  std::cout << " nv=" << std::dec << stats.nightvision_frames << ':';
  for (const auto color : stats.nightvision_clear_colors) {
    std::cout << "0x" << std::hex << color << ',';
  }
  std::cout << std::dec;
  if (!stats.hmd_back_colors.empty()) {
    std::cout << " hmd-back=" << stats.hmd_back_color_samples << '/'
              << stats.hmd_back_colors.size();
  }
  if (!stats.world_vertex_colors.empty()) {
    auto minimum = std::array<std::uint8_t, 3U>{255U, 255U, 255U};
    auto maximum = std::array<std::uint8_t, 3U>{};
    for (const auto packed : stats.world_vertex_colors) {
      const std::array channels{
          static_cast<std::uint8_t>((packed & 0x1fU) << 3U),
          static_cast<std::uint8_t>(((packed >> 5U) & 0x1fU) << 3U),
          static_cast<std::uint8_t>(((packed >> 10U) & 0x1fU) << 3U),
      };
      for (std::size_t channel = 0U; channel < channels.size(); ++channel) {
        minimum[channel] = std::min(minimum[channel], channels[channel]);
        maximum[channel] = std::max(maximum[channel], channels[channel]);
      }
    }
    std::cout << " rgb-range=" << static_cast<unsigned int>(minimum[0]) << ':'
              << static_cast<unsigned int>(minimum[1]) << ':'
              << static_cast<unsigned int>(minimum[2]) << ".."
              << static_cast<unsigned int>(maximum[0]) << ':'
              << static_cast<unsigned int>(maximum[1]) << ':'
              << static_cast<unsigned int>(maximum[2]);
  }
  if (!stats.raw_opcodes.empty()) {
    std::cout << " raw-opcodes=";
    auto separator = std::string_view{};
    for (const auto opcode : stats.raw_opcodes) {
      std::cout << separator << "0x" << std::hex
                << static_cast<unsigned int>(opcode) << std::dec;
      separator = ",";
    }
  }
  if (!stats.raw_packet_formats.empty()) {
    std::cout << " raw-formats=";
    auto separator = std::string_view{};
    for (const auto &[format, count] : stats.raw_packet_formats) {
      std::cout << separator << (format >> 8U) << "x0x" << std::hex
                << (format & 0xffU) << std::dec << ':' << count;
      separator = ",";
    }
  }
  if (stats.raw_line_samples != 0U) {
    std::cout << " raw-line-screen=" << stats.minimum_raw_x << ':'
              << stats.minimum_raw_y << ".." << stats.maximum_raw_x << ':'
              << stats.maximum_raw_y
              << " delta-patterns=" << stats.raw_line_deltas.size()
              << " dynamic-frames=" << stats.raw_frame_fingerprints.size();
  }
  if (!stats.sprites.empty()) {
    std::cout << " sprite-materials=";
    auto separator = std::string_view{};
    for (const auto &sprite : stats.sprites) {
      std::cout << separator << sprite.tpage << ':'
                << static_cast<unsigned int>(sprite.u) << ':'
                << static_cast<unsigned int>(sprite.v) << ':' << sprite.width
                << 'x' << sprite.height << ':' << sprite.center_x << ':'
                << sprite.center_y;
      separator = ",";
    }
  }
  if (!stats.sprite_assets.empty()) {
    std::cout << " sprite-assets=";
    auto separator = std::string_view{};
    for (const auto &[name, count] : stats.sprite_assets) {
      std::cout << separator << name << ':' << count;
      separator = ",";
    }
  }
  if (!stats.active_world_models.empty()) {
    std::cout << " active-world=";
    auto separator = std::string_view{};
    for (const auto model : stats.active_world_models) {
      std::cout << separator << model;
      separator = ",";
    }
  }
  if (!stats.resident_world_models.empty()) {
    std::cout << " resident-world=";
    auto separator = std::string_view{};
    for (const auto model : stats.resident_world_models) {
      std::cout << separator << model;
      separator = ",";
    }
  }
  std::cout << '\n';
}

[[nodiscard]] bool hasSpritePage(const MissionStats &stats,
                                 std::uint16_t tpage) {
  return std::ranges::any_of(stats.sprites, [tpage](const auto &sprite) {
    return sprite.tpage == tpage;
  });
}

[[nodiscard]] bool hasExactNeutralEnvironment(const MissionStats &stats,
                                              std::uint32_t terrain_depth_cue) {
  if (stats.environments.size() != 1U) {
    return false;
  }
  const auto &environment = *stats.environments.begin();
  return environment.clear.red == 0U && environment.clear.green == 0U &&
         environment.clear.blue == 0U && environment.back.red == 128U &&
         environment.back.green == 128U && environment.back.blue == 128U &&
         environment.fog.red == 0U && environment.fog.green == 0U &&
         environment.fog.blue == 0U && environment.dqa == 0 &&
         environment.dqb == 0 &&
         environment.terrain_depth_cue == terrain_depth_cue &&
         environment.background;
}

[[nodiscard]] bool onlySpriteAssetFamily(const MissionStats &stats,
                                         std::string_view prefix) {
  return !stats.sprite_assets.empty() &&
         std::ranges::all_of(stats.sprite_assets, [prefix](const auto &entry) {
           return std::string_view{entry.first}.starts_with(prefix);
         });
}

[[nodiscard]] std::string_view spriteAssetCategory(std::string_view name) {
  constexpr std::array categories{
      std::string_view{"BRETH"}, std::string_view{"EXPL"},
      std::string_view{"FIRE"},  std::string_view{"VAPOR"},
      std::string_view{"STEAM"}, std::string_view{"SNOPRT"}};
  const auto category =
      std::ranges::find_if(categories, [name](const auto prefix) {
        return name.starts_with(prefix);
      });
  return category == categories.end() ? std::string_view{} : *category;
}

[[nodiscard]] std::optional<std::string>
validateMissionSignature(std::uint32_t mission_index,
                         const MissionStats &stats) {
  if (stats.environments.empty() ||
      !std::ranges::all_of(stats.environments, [](const auto &environment) {
        return environment.background && environment.terrain_depth_cue != 0U;
      })) {
    return "retail backdrop/depth-cue state was absent";
  }
  const auto has_retail_scrim =
      mission_index == 0U || mission_index == 11U || mission_index == 12U;
  if (has_retail_scrim) {
    if (stats.scrim_resource_samples == 0U) {
      return "retail SCRIM resource was absent";
    }
    if (!stats.scrim_moves.empty() &&
        (stats.scrim_copy_phases == 0U || stats.scrim_idle_phases == 0U ||
         stats.scrim_moves.size() != 13U)) {
      return "active retail SCRIM copy cycle was incomplete";
    }
  } else if (stats.scrim_resource_samples != 0U || !stats.scrim_moves.empty()) {
    return "non-SCRIM mission published a detached backdrop";
  }
  switch (mission_index) {
  case 3U:
    if (!hasExactNeutralEnvironment(stats, 0x00020320U) ||
        stats.minimum_raw_packets != 80U || stats.maximum_raw_packets != 80U ||
        stats.raw_opcodes.size() != 1U || !stats.raw_opcodes.contains(0x52U) ||
        stats.raw_packet_formats.size() != 1U ||
        stats.raw_packet_formats.find(0x0452U) ==
            stats.raw_packet_formats.end() ||
        stats.raw_packet_formats.at(0x0452U) != stats.frames * 80U ||
        stats.raw_line_samples != stats.frames * 80U ||
        stats.line_particle_samples != stats.raw_particle_line_matches ||
        stats.raw_particle_line_matches + stats.raw_particle_line_nonvisible !=
            stats.frames * 80U ||
        stats.raw_particle_line_misses != 0U ||
        stats.raw_particle_line_ambiguities != 0U ||
        stats.raw_frame_fingerprints.size() != stats.frames ||
        stats.raw_line_deltas.empty() || stats.minimum_raw_x < -1024 ||
        stats.minimum_raw_y < -1024 || stats.maximum_raw_x > 1024 ||
        stats.maximum_raw_y > 1024 ||
        (stats.frames == 241U &&
         (stats.minimum_raw_x != -910 || stats.minimum_raw_y != -580 ||
          stats.maximum_raw_x != 1024 || stats.maximum_raw_y != 1024))) {
      return "PARK rain/backdrop packet signature was absent";
    }
    break;
  case 4U:
    if (!hasExactNeutralEnvironment(stats, 0x000207d0U)) {
      return "PARK2 backdrop signature was absent";
    }
    // The neutral-input probe does not activate Girdeux's scripted fight, so
    // the authored flame/ribbon pool is normally idle. If another probe path
    // does activate it, validate the semantic asset family without coupling
    // the oracle to exact frame counts or trigger timing.
    if (stats.maximum_sprites != 0U || stats.sprite_samples != 0U ||
        stats.maximum_ribbons != 0U || stats.ribbon_samples != 0U) {
      if (stats.maximum_ribbons == 0U || stats.ribbon_samples == 0U ||
          !hasSpritePage(stats, 28U) ||
          stats.matched_sprite_samples != stats.sprite_samples ||
          stats.unmatched_sprite_samples != 0U ||
          stats.ambiguous_sprite_samples != 0U ||
          !onlySpriteAssetFamily(stats, "EXPL")) {
        return "PARK2 Girdeux flame signature was inconsistent";
      }
    }
    break;
  case 7U:
    if (!hasExactNeutralEnvironment(stats, 0x000204b0U) ||
        stats.minimum_raw_packets != 80U || stats.maximum_raw_packets != 80U ||
        stats.raw_opcodes != std::set<std::uint8_t>{0x22U} ||
        stats.raw_packet_formats.size() != 1U ||
        stats.raw_packet_formats.find(0x0422U) ==
            stats.raw_packet_formats.end() ||
        stats.raw_packet_formats.at(0x0422U) != stats.frames * 80U ||
        !hasSpritePage(stats, 31U) ||
        stats.matched_sprite_samples != stats.sprite_samples ||
        stats.unmatched_sprite_samples != 0U ||
        stats.ambiguous_sprite_samples != 0U ||
        !onlySpriteAssetFamily(stats, "BRETH")) {
      return "BASEEXT frost/backdrop packet signature was absent";
    }
    break;
  case 8U:
    // The bunker begins indoors, so retail correctly has no exterior weather
    // packets here. Keep its authored backdrop/depth-cue state distinct from
    // the two exterior Kazakhstan maps.
    if (!hasExactNeutralEnvironment(stats, 0x00031f40U) ||
        stats.maximum_sprites != 0U || stats.maximum_lines != 0U ||
        stats.maximum_raw_packets != 0U || stats.maximum_ribbons != 0U) {
      return "BUNKER indoor backdrop/depth-cue signature was absent";
    }
    break;
  case 10U:
    if (!hasExactNeutralEnvironment(stats, 0x0002041aU) ||
        stats.minimum_raw_packets != 80U || stats.maximum_raw_packets != 80U ||
        stats.raw_opcodes != std::set<std::uint8_t>{0x22U} ||
        stats.raw_packet_formats.size() != 1U ||
        stats.raw_packet_formats.find(0x0422U) ==
            stats.raw_packet_formats.end() ||
        stats.raw_packet_formats.at(0x0422U) != stats.frames * 80U ||
        !hasSpritePage(stats, 31U) ||
        stats.matched_sprite_samples != stats.sprite_samples ||
        stats.unmatched_sprite_samples != 0U ||
        stats.ambiguous_sprite_samples != 0U ||
        !onlySpriteAssetFamily(stats, "BRETH")) {
      return "BASEEXT2 frost/backdrop packet signature was absent";
    }
    break;
  case 18U:
    // CAVE2's blackout is authored in its live BGR555 world vertices. The
    // flashlight is a separate retail vertex-light list entry activated by
    // the player; do not replace the base darkness with a mission heuristic.
    // Combat/VAPOR sprites legitimately begin after the opening samples; they
    // are independent of the level atmosphere and must not invalidate the
    // blackout signature.
    if (!hasExactNeutralEnvironment(stats, 0x00021f40U) ||
        stats.maximum_lines != 0U || stats.maximum_raw_packets != 0U ||
        stats.maximum_ribbons != 0U || stats.unmatched_sprite_samples != 0U ||
        stats.ambiguous_sprite_samples != 0U ||
        stats.world_vertex_color_samples == 0U ||
        !stats.world_vertex_colors.contains(0U)) {
      return "CAVE2 authored blackout vertex state was absent";
    }
    break;
  default:
    break;
  }
  return std::nullopt;
}

int runProbe(const std::filesystem::path &cue_path,
             std::optional<std::uint32_t> only_mission,
             std::uint32_t frame_count, bool enable_flashlight) {
  auto disc = sf::game::GameDisc::open(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{
        sf::core::ErrorCode::unsupported,
        "Retail environment probe requires Syphon Filter USA v1.1"};
  }

  std::size_t missions{};
  std::size_t missions_with_auxiliary_packets{};
  std::size_t environment_states{};
  std::size_t total_sprite_samples{};
  std::size_t total_matched_sprite_samples{};
  std::size_t total_unmatched_sprite_samples{};
  std::size_t total_ambiguous_sprite_samples{};
  std::map<std::string, std::size_t> total_sprite_assets;
  for (const auto &mission : sf::game::missionCatalog()) {
    if (only_mission && mission.index != *only_mission) {
      continue;
    }
    auto package = sf::game::MissionPackage::load(disc, mission.index);
    if (enable_flashlight) {
      const sf::game::GameplaySession gameplay{package};
      const auto *flashlight =
          gameplay.weaponModel(sf::game::WeaponId::flashlight);
      if (flashlight == nullptr) {
        std::cout << "flashlight-model=missing\n";
      } else if (const auto *model =
                     std::get_if<sf::assets::GmdModel>(&flashlight->geometry)) {
        const auto visible = static_cast<std::size_t>(
            std::ranges::count_if(model->triangles(), [](const auto &triangle) {
              return triangle.flags != 0U;
            }));
        std::cout << "flashlight-model=" << flashlight->name
                  << " triangles=" << model->triangles().size() << '/'
                  << visible << " bounds=" << model->bounds().minimum_x << ','
                  << model->bounds().minimum_y << ','
                  << model->bounds().minimum_z << ".."
                  << model->bounds().maximum_x << ','
                  << model->bounds().maximum_y << ','
                  << model->bounds().maximum_z << '\n';
      } else {
        std::cout << "flashlight-model=non-gmd\n";
      }
    }
    auto ground_first = std::optional<WorldMaterialSignature>{};
    auto ground_second = std::optional<WorldMaterialSignature>{};
    if (mission.index == 3U) {
      ground_first = worldGroundMaterialSignature(package, 2U, "A5.EMD", 0x8bU,
                                                  32048U, 64U, 0U, 95U, 63U);
      std::cout << "PARK-wet-ground count=" << ground_first->polygon_count
                << " hash=0x" << std::hex << ground_first->placement_fingerprint
                << std::dec << " pages=";
      for (const auto page : ground_first->resolved_pages) {
        std::cout << static_cast<unsigned int>(page) << ',';
      }
      std::cout << '\n';
    } else if (mission.index == 4U) {
      ground_first = worldGroundMaterialSignature(package, 1U, "A2.EMD", 0x96U,
                                                  32624U, 0U, 192U, 31U, 255U);
      ground_second = worldGroundMaterialSignature(package, 1U, "A2.EMD", 0x97U,
                                                   32624U, 0U, 64U, 31U, 95U);
      std::cout << "PARK2-ground first=" << ground_first->polygon_count << ":0x"
                << std::hex << ground_first->placement_fingerprint
                << " second=" << std::dec << ground_second->polygon_count
                << ":0x" << std::hex << ground_second->placement_fingerprint
                << std::dec << " pages=";
      for (const auto page : ground_first->resolved_pages) {
        std::cout << static_cast<unsigned int>(page) << ',';
      }
      for (const auto page : ground_second->resolved_pages) {
        std::cout << static_cast<unsigned int>(page) << ',';
      }
      std::cout << '\n';
    }
    const auto effect_placements =
        specialEffectPlacements(package.specialEffects());
    sf::game::LegacyFirstMissionRuntime runtime{package.definition(),
                                                package.legacyImage()};
    if (!runtime.ready() || runtime.faulted()) {
      std::cerr << "mission=" << mission.index << " bootstrap fault="
                << sf::game::legacyRuntimeFaultReasonName(runtime.faultReason())
                << " bridge="
                << sf::game::legacyGameplayBridgeReadFaultName(
                       runtime.rendererBridgeFault())
                << '\n';
      return 2;
    }
    if (enable_flashlight &&
        !sf::game::G4CampaignTransitionProbeAccess::invokeRetailFlashlight(
            runtime, true)) {
      std::cerr << "mission=" << mission.index
                << " could not activate the retail flashlight\n";
      return 2;
    }
    if (enable_flashlight) {
      const auto *lighting = runtime.bridge();
      if (lighting == nullptr) {
        return 2;
      }
      for (const auto &light : lighting->vertex_lights) {
        std::cout << "light source=0x" << std::hex << light.source
                  << " flags=0x" << light.flags << std::dec
                  << " shape=" << light.shape
                  << " screen/depth=" << light.screen_shift << '/'
                  << light.depth_shift << " threshold=" << light.threshold
                  << " mask=0x" << std::hex << light.channel_mask << std::dec
                  << " matrix=";
        for (const auto component : light.matrix.rotation) {
          std::cout << component << ',';
        }
        std::cout << " t=" << light.matrix.translation.x << ','
                  << light.matrix.translation.y << ','
                  << light.matrix.translation.z << '\n';
      }
      std::size_t tested_vertices{};
      std::size_t illuminated_vertices{};
      auto first_illuminated = std::optional<std::array<std::int32_t, 5U>>{};
      for (const auto &light : lighting->vertex_lights) {
        auto retail = sf::game::RetailVertexLightState{};
        retail.matrix.rotation = light.matrix.rotation;
        retail.matrix.translation = {light.matrix.translation.x,
                                     light.matrix.translation.y,
                                     light.matrix.translation.z};
        retail.flags = light.flags;
        retail.extent = light.shape;
        retail.screen_shift = light.screen_shift;
        retail.depth_shift = light.depth_shift;
        retail.threshold = light.threshold;
        retail.channel_mask = light.channel_mask;
        const std::array sources{retail};
        for (const auto &entry : package.worldModels().entries()) {
          const auto scene = sf::assets::EmdScene::parse(
              package.worldModels().file(entry.name));
          for (const auto &section : scene.sections()) {
            for (const auto &vertex : section.vertices) {
              const auto base =
                  static_cast<std::uint32_t>((vertex.color & 0x1fU) << 3U) |
                  (static_cast<std::uint32_t>((vertex.color >> 5U) & 0x1fU)
                   << 11U) |
                  (static_cast<std::uint32_t>((vertex.color >> 10U) & 0x1fU)
                   << 19U);
              const auto lit = sf::game::applyRetailVertexLightingPacked(
                  base, sources,
                  {static_cast<double>(vertex.x), static_cast<double>(vertex.y),
                   static_cast<double>(vertex.z)},
                  320);
              ++tested_vertices;
              illuminated_vertices += lit != base ? 1U : 0U;
              if (lit != base && !first_illuminated) {
                first_illuminated = std::array<std::int32_t, 5U>{
                    vertex.x, vertex.y, vertex.z,
                    static_cast<std::int32_t>(base),
                    static_cast<std::int32_t>(lit)};
              }
            }
          }
        }
      }
      std::cout << "flashlight-vertex-coverage=" << illuminated_vertices << '/'
                << tested_vertices;
      if (first_illuminated) {
        std::cout << " first=" << (*first_illuminated)[0] << ','
                  << (*first_illuminated)[1] << ',' << (*first_illuminated)[2]
                  << ":0x" << std::hex << (*first_illuminated)[3] << "->0x"
                  << (*first_illuminated)[4] << std::dec;
      }
      std::cout << '\n';
    }

    MissionStats stats;
    auto previous_sequence = std::uint64_t{};
    auto immutable_anchor =
        std::shared_ptr<const sf::game::LegacyPresentationFrame>{};
    auto immutable_anchor_fingerprint = std::uint64_t{};
    auto immutable_anchor_has_auxiliary_commands = false;
    const auto observe = [&]() -> bool {
      const auto &frame = runtime.presentationFrame();
      if (!frame) {
        std::cerr << "mission=" << mission.index
                  << " missing immutable presentation frame\n";
        return false;
      }
      if (const auto error = observeFrame(*frame, previous_sequence,
                                          effect_placements, stats)) {
        std::cerr << "mission=" << mission.index
                  << " sequence=" << frame->sequence << " " << *error << '\n';
        return false;
      }
      const auto has_auxiliary_commands =
          hasAuxiliaryCommands(frame->renderer->state);
      if (!immutable_anchor || (!immutable_anchor_has_auxiliary_commands &&
                                has_auxiliary_commands)) {
        immutable_anchor = frame;
        immutable_anchor_fingerprint =
            auxiliaryFingerprint(frame->renderer->state);
        immutable_anchor_has_auxiliary_commands = has_auxiliary_commands;
      }
      previous_sequence = frame->sequence;
      return true;
    };
    if (!observe()) {
      return 3;
    }
    runtime.setHostPadState({});
    for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
      runtime.advanceHostUpdate();
      if (runtime.faulted()) {
        std::cerr << "mission=" << mission.index << " frame=" << frame
                  << " runtime fault="
                  << sf::game::legacyRuntimeFaultReasonName(
                         runtime.faultReason())
                  << " bridge="
                  << sf::game::legacyGameplayBridgeReadFaultName(
                         runtime.rendererBridgeFault())
                  << '\n';
        return 4;
      }
      if (!observe()) {
        return 5;
      }
    }
    if (!immutable_anchor || !immutable_anchor->renderer ||
        auxiliaryFingerprint(immutable_anchor->renderer->state) !=
            immutable_anchor_fingerprint) {
      std::cerr << "mission=" << mission.index
                << " retained presentation frame was mutated\n";
      return 6;
    }

    printStats(mission, stats);
    if (mission.index == 3U &&
        (!ground_first || ground_first->polygon_count != 4U ||
         ground_first->placement_fingerprint != 0xf50f0012eb689867ULL ||
         ground_first->resolved_pages != std::set<std::uint8_t>{11U} ||
         !stats.active_world_models.contains(2U))) {
      std::cerr << "mission=3 PARK authored wet-ground placement signature "
                   "was absent\n";
      return 7;
    }
    if (mission.index == 4U &&
        (!ground_first || !ground_second ||
         ground_first->polygon_count != 35U ||
         ground_first->placement_fingerprint != 0x4996833caad408ceULL ||
         ground_first->resolved_pages != std::set<std::uint8_t>{22U} ||
         ground_second->polygon_count != 49U ||
         ground_second->placement_fingerprint != 0xbe9a78230273c8cfULL ||
         ground_second->resolved_pages != std::set<std::uint8_t>{23U} ||
         !stats.active_world_models.contains(1U))) {
      std::cerr << "mission=4 PARK2 authored ground placement signature was "
                   "absent\n";
      return 7;
    }
    if (const auto error = validateMissionSignature(mission.index, stats)) {
      std::cerr << "mission=" << mission.index << " " << *error << '\n';
      return 7;
    }
    ++missions;
    environment_states += stats.environments.size();
    total_sprite_samples += stats.sprite_samples;
    total_matched_sprite_samples += stats.matched_sprite_samples;
    total_unmatched_sprite_samples += stats.unmatched_sprite_samples;
    total_ambiguous_sprite_samples += stats.ambiguous_sprite_samples;
    for (const auto &[name, count] : stats.sprite_assets) {
      if (spriteAssetCategory(name).empty()) {
        std::cerr << "mission=" << mission.index
                  << " camera sprite is outside the supported retail "
                     "SPFX families: "
                  << name << '\n';
        return 8;
      }
      total_sprite_assets[name] += count;
    }
    if (stats.sprite_samples != 0U || stats.line_samples != 0U ||
        stats.raw_packet_samples != 0U || stats.ribbon_samples != 0U) {
      ++missions_with_auxiliary_packets;
    }
  }

  const auto expected_missions =
      only_mission ? 1U : sf::game::missionCatalog().size();
  if (missions != expected_missions || environment_states < missions) {
    std::cerr << "Retail environment gate did not observe every mission\n";
    return 9;
  }
  if (total_matched_sprite_samples != total_sprite_samples ||
      total_unmatched_sprite_samples != 0U ||
      total_ambiguous_sprite_samples != 0U) {
    std::cerr << "Camera sprite material classification is incomplete or "
                 "ambiguous\n";
    return 10;
  }
  std::map<std::string_view, std::size_t> category_totals;
  for (const auto &[name, count] : total_sprite_assets) {
    category_totals[spriteAssetCategory(name)] += count;
  }
  std::cout << "camera-sprite-categories=";
  auto category_separator = std::string_view{};
  for (const auto &[category, count] : category_totals) {
    std::cout << category_separator << category << ':' << count;
    category_separator = ",";
  }
  std::cout << '\n';
  std::cout << "camera-sprite-summary=" << total_sprite_samples << '/'
            << total_matched_sprite_samples << '/'
            << total_unmatched_sprite_samples << '/'
            << total_ambiguous_sprite_samples << " assets=";
  auto separator = std::string_view{};
  for (const auto &[name, count] : total_sprite_assets) {
    std::cout << separator << name << ':' << count;
    separator = ",";
  }
  std::cout << '\n';
  std::cout << "Retail environment gate passed: missions=" << missions
            << " frames-per-mission=" << (frame_count + 1U)
            << " environment-states=" << environment_states
            << " missions-with-camera-aux=" << missions_with_auxiliary_packets
            << '\n';
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argc > 5) {
    std::cerr << "Usage: sf_retail_environment_probe <game.cue> "
                 "[mission-index] [frames] [flashlight]\n";
    return 1;
  }
  try {
    auto only_mission = std::optional<std::uint32_t>{};
    if (argc >= 3) {
      only_mission = parseUnsigned(argv[2]);
      if (!only_mission || *only_mission >= sf::game::missionCatalog().size()) {
        std::cerr << "Invalid mission index\n";
        return 1;
      }
    }
    auto frames = default_probe_frames;
    if (argc >= 4) {
      const auto parsed = parseUnsigned(argv[3]);
      if (!parsed || *parsed == 0U || *parsed > 2400U) {
        std::cerr << "Invalid frame count\n";
        return 1;
      }
      frames = *parsed;
    }
    const auto enable_flashlight =
        argc == 5 && std::string_view{argv[4]} == "flashlight";
    return runProbe(std::filesystem::path{argv[1]}, only_mission, frames,
                    enable_flashlight);
  } catch (const std::exception &error) {
    std::cerr << "Retail environment gate failed: " << error.what() << '\n';
    return 10;
  }
}
