#include "sf/assets/emd_scene.hpp"
#include "sf/assets/fog_archive.hpp"
#include "sf/assets/gmd_model.hpp"
#include "sf/assets/hmd_animation.hpp"
#include "sf/assets/hmd_model.hpp"
#include "sf/assets/hog_archive.hpp"
#include "sf/assets/level_layout.hpp"
#include "sf/assets/mission_briefing.hpp"
#include "sf/assets/mission_objects.hpp"
#include "sf/assets/tim_image.hpp"
#include "sf/assets/weapon_descriptions.hpp"
#include "sf/core/error.hpp"
#include "sf/core/polygon_clipper.hpp"
#include "sf/core/sha256.hpp"
#include "sf/disc/cue_sheet.hpp"
#include "sf/disc/iso9660.hpp"
#include "sf/game/actor_animation.hpp"
#include "sf/game/chase_camera.hpp"
#include "sf/game/effects.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/hud.hpp"
#include "sf/game/localization.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/mission_start.hpp"
#include "sf/game/player_controller.hpp"
#include "sf/game/state_stack.hpp"
#include "sf/game/system.hpp"
#include "sf/game/title.hpp"
#include "sf/platform/actor_shadow_stability.hpp"
#include "sf/platform/gameplay_message_reveal_policy.hpp"
#include "sf/platform/retail_scope_text_policy.hpp"
#include "sf/psx/executable.hpp"
#include "sf/psx/function_map.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error{message};
  }
}

void writeLe32(std::span<std::byte> bytes, std::size_t offset,
               std::uint32_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
  bytes[offset + 2] = static_cast<std::byte>(value >> 16U);
  bytes[offset + 3] = static_cast<std::byte>(value >> 24U);
}

void writeLe16(std::span<std::byte> bytes, std::size_t offset,
               std::uint16_t value) {
  bytes[offset] = static_cast<std::byte>(value);
  bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
}

void writeFogEntry(std::span<std::byte> bytes, std::size_t index,
                   std::string_view name, std::uint32_t start_sector,
                   std::uint32_t sector_count) {
  const auto offset = 16U + index * 24U;
  std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), 16U,
              std::byte{0});
  std::ranges::transform(
      name, bytes.begin() + static_cast<std::ptrdiff_t>(offset),
      [](char value) { return static_cast<std::byte>(value); });
  writeLe32(bytes, offset + 16U, start_sector);
  writeLe32(bytes, offset + 20U, sector_count);
}

void testFogArchive() {
  std::vector<std::byte> bytes(3U * sf::assets::FogArchive::sector_size,
                               std::byte{0xcd});
  writeLe32(bytes, 0, 0x80000001U);
  writeLe32(bytes, 4, 3U);
  writeLe32(bytes, 8, 0U);
  writeLe32(bytes, 12, 0U);
  writeFogEntry(bytes, 0, "A.BIN", 1U, 1U);
  writeFogEntry(bytes, 1, "B.HOG", 2U, 1U);
  bytes[sf::assets::FogArchive::sector_size] = std::byte{0x42};
  bytes[2U * sf::assets::FogArchive::sector_size] = std::byte{0x7e};

  const auto archive = sf::assets::FogArchive::parse(std::move(bytes));
  require(archive.flags() == 0x80000001U, "FOG flags mismatch");
  require(archive.declaredSectorCount() == 3U, "FOG sector count mismatch");
  require(archive.entries().size() == 2U, "FOG entry count mismatch");
  require(archive.file("a.bin").front() == std::byte{0x42},
          "FOG case-insensitive lookup failed");
  require(archive.file("B.HOG").front() == std::byte{0x7e},
          "FOG data offset mismatch");

  std::vector<std::byte> unsorted(4U * sf::assets::FogArchive::sector_size,
                                  std::byte{0xcd});
  writeLe32(unsorted, 0, 0x80000001U);
  writeLe32(unsorted, 4, 4U);
  writeLe32(unsorted, 8, 0U);
  writeLe32(unsorted, 12, 0U);
  writeFogEntry(unsorted, 0, "LATE.BIN", 3U, 1U);
  writeFogEntry(unsorted, 1, "OPTIONAL.BIN", 0U, 0U);
  writeFogEntry(unsorted, 2, "EARLY.BIN", 1U, 2U);
  unsorted[sf::assets::FogArchive::sector_size] = std::byte{0x31};
  unsorted[3U * sf::assets::FogArchive::sector_size] = std::byte{0x73};
  const auto unsorted_archive =
      sf::assets::FogArchive::parse(std::move(unsorted));
  require(unsorted_archive.entries().size() == 2U &&
              unsorted_archive.file("EARLY.BIN").front() == std::byte{0x31} &&
              unsorted_archive.file("LATE.BIN").front() == std::byte{0x73},
          "FOG unsorted/optional extent table was not accepted");
}

void testInvalidFogArchive() {
  std::vector<std::byte> bytes(2U * sf::assets::FogArchive::sector_size,
                               std::byte{0xcd});
  writeLe32(bytes, 0, 1U);
  writeLe32(bytes, 4, 2U);
  writeLe32(bytes, 8, 0U);
  writeLe32(bytes, 12, 0U);
  writeFogEntry(bytes, 0, "BAD.BIN", 1U, 2U);
  try {
    static_cast<void>(sf::assets::FogArchive::parse(std::move(bytes)));
    throw std::runtime_error{"Invalid FOG was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_format,
            "Invalid FOG returned the wrong error code");
  }

  std::vector<std::byte> overlap(4U * sf::assets::FogArchive::sector_size,
                                 std::byte{0xcd});
  writeLe32(overlap, 0, 1U);
  writeLe32(overlap, 4, 4U);
  writeLe32(overlap, 8, 0U);
  writeLe32(overlap, 12, 0U);
  writeFogEntry(overlap, 0, "A.BIN", 1U, 2U);
  writeFogEntry(overlap, 1, "B.BIN", 2U, 2U);
  try {
    static_cast<void>(sf::assets::FogArchive::parse(std::move(overlap)));
    throw std::runtime_error{"Overlapping FOG extents were accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_format,
            "Overlapping FOG returned the wrong error code");
  }
}

void testMissionCatalog() {
  const auto missions = sf::game::missionCatalog();
  require(missions.size() == 20U, "Retail mission catalog count mismatch");
  for (std::size_t index = 0; index < missions.size(); ++index) {
    require(missions[index].index == index,
            "Retail mission catalog index mismatch");
  }
  require(missions.front().resource_name == "SUBWAY" &&
              missions.front().title == "Georgia Street" &&
              missions.back().resource_name == "SILO" &&
              missions.back().title == "Missile Silo",
          "Retail mission catalog order mismatch");
  constexpr std::array expected_briefing_overlays{
      std::string_view{"SUBWAY.OVL"},  std::string_view{"SUBWAY2.OVL"},
      std::string_view{"SUBWAY3.OVL"}, std::string_view{"PARK.OVL"},
      std::string_view{"PARK2.OVL"},   std::string_view{"MUSEUM.OVL"},
      std::string_view{"MUSEUM2.OVL"}, std::string_view{"BASEEXT.OVL"},
      std::string_view{"BASEEXT.OVL"}, std::string_view{"CHOPPER.OVL"},
      std::string_view{"BASEEXT.OVL"}, std::string_view{"LEVSPEC.OVL"},
      std::string_view{"LEVSPEC.OVL"}, std::string_view{"CATACOMB.OVL"},
      std::string_view{"WHOUSE.OVL"},  std::string_view{"WHOUSE.OVL"},
      std::string_view{"WHOUSE.OVL"},  std::string_view{"CAVE.OVL"},
      std::string_view{"CAVE.OVL"},    std::string_view{"CAVE.OVL"},
  };
  constexpr std::array<std::uint8_t, 20U> expected_briefing_records{
      0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U,
      2U, 0U, 1U, 0U, 0U, 1U, 2U, 0U, 1U, 2U,
  };
  for (std::size_t index = 0U; index < missions.size(); ++index) {
    const auto briefing_overlay = missions[index].briefing_overlay_name.empty()
                                      ? missions[index].overlay_name
                                      : missions[index].briefing_overlay_name;
    require(briefing_overlay == expected_briefing_overlays[index] &&
                missions[index].briefing_record ==
                    expected_briefing_records[index],
            "Retail mission briefing source mismatch");
  }
  require(sf::game::missionDefinition(13U).overlay_name == "CATACOMB.OVL" &&
              sf::game::missionDefinition(18U).overlay_name == "WHOUSE.OVL",
          "Retail mission overlay mapping mismatch");
  require(sf::game::missionDefinition(2U).briefing_record == 0U &&
              sf::game::missionDefinition(13U).briefing_record == 0U &&
              sf::game::missionDefinition(17U).briefing_record == 0U &&
              sf::game::missionDefinition(18U).briefing_record == 1U &&
              sf::game::missionDefinition(19U).briefing_record == 2U &&
              sf::game::missionDefinition(18U).briefing_overlay_name ==
                  "CAVE.OVL" &&
              sf::game::missionDefinition(19U).briefing_overlay_name ==
                  "CAVE.OVL",
          "Retail mission briefing mapping mismatch");
  require(sf::game::missionDefinition(9U).resource_name == "CHOPPER" &&
              sf::game::missionDefinition(10U).resource_name == "BASEEXT2" &&
              std::ranges::all_of(missions,
                                  [](const auto &mission) {
                                    return mission.selection_index ==
                                           static_cast<std::int32_t>(
                                               mission.index);
                                  }),
          "Retail mission selector mapping mismatch");
  try {
    static_cast<void>(sf::game::missionDefinition(20U));
    throw std::runtime_error{"Invalid retail mission index was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_argument,
            "Invalid retail mission index returned the wrong error code");
  }
}

void testHogArchive() {
  std::vector<std::byte> bytes(72);
  writeLe32(bytes, 0, 0x36a4f0aeU);
  writeLe32(bytes, 4, 2);
  writeLe32(bytes, 8, 0x14);
  writeLe32(bytes, 12, 28);
  writeLe32(bytes, 16, 64);
  writeLe32(bytes, 20, 0);
  writeLe32(bytes, 24, 4);
  constexpr std::string_view names{"A.BIN\0B.BIN\0", 12};
  std::ranges::transform(names, bytes.begin() + 28, [](char value) {
    return static_cast<std::byte>(value);
  });
  bytes[64] = std::byte{1};
  bytes[68] = std::byte{2};

  const auto archive = sf::assets::HogArchive::parse(std::move(bytes));
  require(archive.identifier() == 0x36a4f0aeU, "HOG identifier mismatch");
  require(archive.entries().size() == 2, "HOG entry count mismatch");
  require(archive.file("a.bin").size() == 4,
          "HOG case-insensitive lookup failed");
  require(archive.file("B.BIN")[0] == std::byte{2}, "HOG data offset mismatch");
}

void testTimImage() {
  constexpr std::size_t clut_block_size = 12 + 256 * 2;
  constexpr std::size_t pixel_offset = 8 + clut_block_size;
  std::vector<std::byte> bytes(pixel_offset + 16);
  writeLe32(bytes, 0, 0x10);
  writeLe32(bytes, 4, 0x09);
  writeLe32(bytes, 8, static_cast<std::uint32_t>(clut_block_size));
  writeLe16(bytes, 12, 768);
  writeLe16(bytes, 14, 490);
  writeLe16(bytes, 16, 256);
  writeLe16(bytes, 18, 1);
  writeLe32(bytes, pixel_offset, 16);
  writeLe16(bytes, pixel_offset + 4, 896);
  writeLe16(bytes, pixel_offset + 6, 0);
  writeLe16(bytes, pixel_offset + 8, 1);
  writeLe16(bytes, pixel_offset + 10, 2);
  writeLe16(bytes, pixel_offset + 12, 0x0100);
  writeLe16(bytes, pixel_offset + 14, 0x0302);

  const auto image = sf::assets::TimImage::parse(bytes);
  require(image.mode() == sf::assets::TimPixelMode::indexed8,
          "TIM pixel mode mismatch");
  require(image.clut().has_value() && image.clut()->words.size() == 256,
          "TIM CLUT mismatch");
  require(image.displayWidth() == 2 && image.displayHeight() == 2,
          "TIM display dimensions mismatch");
  require(image.pixels().words[1] == 0x0302, "TIM pixel payload mismatch");
}

void testEmdScene() {
  const auto direct =
      sf::assets::resolveEmdTexturePageSource(0x87U, 1U << 7U, 0U);
  const auto shifted =
      sf::assets::resolveEmdTexturePageSource(0x98U, 1U << 18U, 1U << 24U);
  const auto vlf_fallback =
      sf::assets::resolveEmdTexturePageSource(0x9eU, 0U, 1U << 30U);
  require(direct && *direct == 7U && shifted && *shifted == 18U &&
              vlf_fallback && *vlf_fallback == 30U,
          "EMD logical texture-page resolution mismatch");
  require(!sf::assets::resolveEmdTexturePageSource(
              0x98U, (1U << 18U) | (1U << 24U), 0U) &&
              !sf::assets::resolveEmdTexturePageSource(
                  0x98U, 0U, (1U << 18U) | (1U << 24U)) &&
              !sf::assets::resolveEmdTexturePageSource(0x98U, 0U, 0U),
          "EMD ambiguous/missing texture-page source was accepted");

  constexpr std::size_t section_offset = 0xa0;
  constexpr std::size_t vertex_offset = section_offset + 0x4c;
  std::vector<std::byte> bytes(vertex_offset + 4U * 8U);
  writeLe32(bytes, 0, 0x303U);
  writeLe32(bytes, 4, static_cast<std::uint32_t>(section_offset));
  writeLe32(bytes, 8, 0xffffffffU);
  writeLe16(bytes, section_offset + 4, 2);
  writeLe16(bytes, section_offset + 6, 4);
  writeLe32(bytes, section_offset + 0x14, 2);
  writeLe32(bytes, section_offset + 0x24, 0x4c);
  writeLe32(bytes, section_offset + 0x2c, 0x85404060U);
  writeLe32(bytes, section_offset + 0x30, 0x03870000U);
  writeLe32(bytes, section_offset + 0x34, 0x06000009U);
  // Retail collision-only quads carry only the topology bit. Their unused
  // material selector must never reserve a texture page or reach rendering.
  writeLe32(bytes, section_offset + 0x3c, 0x80000000U);
  writeLe32(bytes, section_offset + 0x40, 0x03cb0000U);
  writeLe32(bytes, section_offset + 0x44, 0x06000009U);
  writeLe32(bytes, 0x88, 0x12345678U);
  for (std::size_t index = 0; index < 4; ++index) {
    writeLe16(bytes, vertex_offset + index * 8U,
              static_cast<std::uint16_t>(index * 10U));
    writeLe16(bytes, vertex_offset + index * 8U + 2U,
              static_cast<std::uint16_t>(index * 20U));
    writeLe16(bytes, vertex_offset + index * 8U + 4U,
              static_cast<std::uint16_t>(index * 30U));
    writeLe16(bytes, vertex_offset + index * 8U + 6U, 0x4210U);
  }

  const auto scene = sf::assets::EmdScene::parse(bytes);
  require(scene.flags() == 0x303U, "EMD flags mismatch");
  require(scene.textureBank() == 0 && scene.texturePageMask() == 0x12345678U,
          "EMD texture metadata mismatch");
  require(scene.sections().size() == 1 && scene.vertexCount() == 4 &&
              scene.polygonCount() == 2,
          "EMD section counts mismatch");
  const auto &polygon = scene.sections().front().polygons.front();
  require(polygon.quad && polygon.vertex_indices ==
                              std::array<std::uint16_t, 4>{1, 0, 2, 3},
          "EMD compact vertex indices mismatch");
  require(polygon.texture_page == 0x87 && polygon.clut == 0x7d70,
          "EMD texture selectors mismatch");
  require(polygon.renderable && scene.sections().front().polygons[1].quad &&
              !scene.sections().front().polygons[1].renderable,
          "EMD collision-only quad was accepted for rendering");
  require(polygon.uv[0].u == 0x60 && polygon.uv[0].v == 0x40 &&
              polygon.uv[3].u == 0x7f && polygon.uv[3].v == 0x7f,
          "EMD quad UV expansion mismatch");
  const auto resolved_mask = scene.resolvedTexturePageMask(1U << 7U);
  require(resolved_mask &&
              *resolved_mask == (scene.texturePageMask() | (1U << 7U)),
          "EMD selective texture-page mask mismatch");

  // The retail header reserves 0x04..0x7f for section offsets. The words at
  // 0x80 and 0x84 point to linked-light metadata and must never become fake
  // geometry sections when all 31 section slots are occupied.
  constexpr std::size_t maximum_retail_sections = 31U;
  constexpr std::size_t empty_section_size = 0x2cU;
  constexpr std::size_t full_section_table_end =
      section_offset + maximum_retail_sections * empty_section_size;
  constexpr std::size_t linked_light_table = full_section_table_end;
  constexpr std::size_t animated_light_table =
      linked_light_table + empty_section_size;
  std::vector<std::byte> full_table_bytes(animated_light_table +
                                          empty_section_size);
  for (std::size_t index = 0; index < maximum_retail_sections; ++index) {
    const auto offset = section_offset + index * empty_section_size;
    writeLe32(full_table_bytes, 4U + index * 4U,
              static_cast<std::uint32_t>(offset));
    writeLe32(full_table_bytes, offset + 0x24U,
              static_cast<std::uint32_t>(empty_section_size));
  }
  writeLe32(full_table_bytes, 0x80U,
            static_cast<std::uint32_t>(linked_light_table));
  writeLe32(full_table_bytes, 0x84U,
            static_cast<std::uint32_t>(animated_light_table));
  writeLe32(full_table_bytes, linked_light_table + 0x24U,
            static_cast<std::uint32_t>(empty_section_size));
  writeLe32(full_table_bytes, animated_light_table + 0x24U,
            static_cast<std::uint32_t>(empty_section_size));

  const auto full_table_scene = sf::assets::EmdScene::parse(full_table_bytes);
  require(full_table_scene.sections().size() == maximum_retail_sections,
          "EMD linked-light metadata was parsed as geometry sections");
}

void testGmdModel() {
  std::vector<std::byte> bytes(76);
  writeLe32(bytes, 0, 0x7b);
  writeLe16(bytes, 4, 2);
  writeLe16(bytes, 6, 0x38);
  writeLe16(bytes, 8, 0x48);
  writeLe16(bytes, 0x0a, static_cast<std::uint16_t>(-34));
  writeLe16(bytes, 0x0c, static_cast<std::uint16_t>(-67));
  writeLe16(bytes, 0x0e, 0);
  writeLe16(bytes, 0x10, 34);
  writeLe16(bytes, 0x12, 0);
  writeLe16(bytes, 0x14, 0);
  writeLe32(bytes, 0x18, 0x9fbd1f00U);
  writeLe32(bytes, 0x1c, 0x1f1f0000U);
  writeLe32(bytes, 0x20, 0x9f010200U);
  writeLe32(bytes, 0x28, 0x00be001fU);
  writeLe32(bytes, 0x2c, 0x00001f1fU);
  writeLe32(bytes, 0x30, 0x00020103U);
  writeLe32(bytes, 0x38, 0x00000022U);
  writeLe32(bytes, 0x3c, 0x000003deU);
  writeLe32(bytes, 0x40, 0x000ef422U);
  writeLe32(bytes, 0x44, 0x000ef7deU);

  const auto model = sf::assets::GmdModel::parse(bytes);
  require(model.vertices().size() == 4 && model.triangles().size() == 2,
          "GMD table counts mismatch");
  require(model.vertices()[2].x == 34 && model.vertices()[2].y == -67,
          "GMD packed vertex mismatch");
  const auto &triangle = model.triangles().front();
  require(triangle.vertex_indices == std::array<std::uint8_t, 3>{0, 2, 1},
          "GMD compact indices mismatch");
  require(triangle.texture_page == 0xbd && triangle.clut == 0x7ff0 &&
              triangle.flags == 0x1f && triangle.semi_transparent,
          "GMD material selectors mismatch");
  require(model.texturePageMask() == ((1U << 29U) | (1U << 30U)) &&
              model.renderableTexturePageMask() == (1U << 29U) &&
              model.planar(),
          "GMD texture mask mismatch");
}

void testCfireSpawnPoint() {
  const sf::assets::MissionTransform transform{
      std::array<std::int16_t, 9>{
          1343,
          0,
          -16003,
          0,
          5779,
          0,
          5689,
          0,
          3482,
      },
      6502,
      2128,
      2589,
  };
  require(sf::game::cfireSpawnPoint(transform) ==
              sf::game::EffectPoint{6502, -2240, 2589},
          "CFIRE class 0x30 origin mismatch");

  const sf::assets::MissionTransform negative_basis{
      std::array<std::int16_t, 9>{
          4096,
          0,
          -1,
          0,
          4096,
          -1,
          0,
          0,
          -1,
      },
      10,
      20,
      30,
  };
  require(sf::game::cfireSpawnPoint(negative_basis) ==
              sf::game::EffectPoint{10, -132, 30},
          "CFIRE origin must not depend on the authored basis");
}

void testLegacyEffectSpriteLayouts() {
  using sf::game::LegacyEffectSpriteFamily;
  using sf::game::LegacyEffectSpriteLayout;
  require(
      sf::game::legacyEffectSpriteLayout(LegacyEffectSpriteFamily::fire) ==
              LegacyEffectSpriteLayout{16U, 16U, 64U} &&
          sf::game::legacyEffectSpriteLayout(
              LegacyEffectSpriteFamily::explosion) ==
              LegacyEffectSpriteLayout{12U, 16U, 32U} &&
          sf::game::legacyEffectSpriteLayout(
              LegacyEffectSpriteFamily::breath) ==
              LegacyEffectSpriteLayout{16U, 8U, 16U} &&
          sf::game::legacyEffectSpriteLayout(LegacyEffectSpriteFamily::vapor) ==
              LegacyEffectSpriteLayout{8U, 16U, 32U},
      "Retail SPFX family layout mapping mismatch");
  require(
      sf::game::legacyEffectSpriteFrameValid(0U, 0U) &&
          !sf::game::legacyEffectSpriteFrameValid(0U, 1U) &&
          sf::game::legacyEffectSpriteFrameValid(
              static_cast<std::uint8_t>(LegacyEffectSpriteFamily::fire), 15U) &&
          !sf::game::legacyEffectSpriteFrameValid(
              static_cast<std::uint8_t>(LegacyEffectSpriteFamily::fire), 16U) &&
          !sf::game::legacyEffectSpriteFrameValid(0xffU, 0U),
      "Guest effect sprite frame validation mismatch");
}

void testLegacyDynamicPresentationPolicy() {
  using sf::game::LegacyPresentationResourceKind;
  const auto resource = sf::game::legacyPresentationResourceKind;
  require(resource("HANS.TMD", false, false, true) ==
                  LegacyPresentationResourceKind::hmd &&
              resource("CHOPPER.TMD", false, false, true) ==
                  LegacyPresentationResourceKind::hmd &&
              resource("BOMB.TMD", false, false, true) ==
                  LegacyPresentationResourceKind::hmd,
          "Retail TMD definitions did not resolve their exact HMD conversion");
  require(
      resource("BOMB.TMD", true, false, true) ==
              LegacyPresentationResourceKind::gmd &&
          resource("DOOR.TMD", false, true, true) ==
              LegacyPresentationResourceKind::emd &&
          resource("HANS.HMD", false, false, true) ==
              LegacyPresentationResourceKind::hmd &&
          resource("ROOM.EMD", true, true, false) ==
              LegacyPresentationResourceKind::emd,
      "Presentation resource priority changed authored GMD/EMD/HMD selection");
  require(resource("HANS.TMD", false, false, false) ==
                  LegacyPresentationResourceKind::none &&
              resource("HANS.HAN", false, false, true) ==
                  LegacyPresentationResourceKind::none &&
              resource("HANS.HAN", true, true, true) ==
                  LegacyPresentationResourceKind::none &&
              resource("HANS.HMD", true, false, false) ==
                  LegacyPresentationResourceKind::none &&
              resource("ROOM.EMD", true, false, false) ==
                  LegacyPresentationResourceKind::none,
          "Presentation resource resolution substituted a non-exact geometry");

  require(sf::game::legacyPresentationUsesRetailNpc(
              true, sf::game::legacy_common_npc_handler, 0x80123400U) &&
              !sf::game::legacyPresentationUsesRetailNpc(
                  false, sf::game::legacy_common_npc_handler, 0x80123400U) &&
              !sf::game::legacyPresentationUsesRetailNpc(true, 0x80150e9cU,
                                                         0x80123400U) &&
              !sf::game::legacyPresentationUsesRetailNpc(
                  true, sf::game::legacy_common_npc_handler, 0U) &&
              sf::game::legacyRetailNpcIsAlly(0U) &&
              !sf::game::legacyRetailNpcIsAlly(1U) &&
              sf::game::legacyRetailNpcIsAlly(2U) &&
              !sf::game::legacyRetailNpcIsAlly(3U),
          "Legacy retail NPC handler/faction policy mismatch");
  require(sf::game::legacyHmdRenderAllowed(false, false) &&
              sf::game::legacyHmdRenderAllowed(true, true) &&
              !sf::game::legacyHmdRenderAllowed(true, false),
          "Guest-owned retail NPC escaped exact-pose rendering");
  require(sf::game::legacyGuestUsesSecondaryItemModel(0x4fU, 0x20U) &&
              sf::game::legacyGuestUsesSecondaryItemModel(0x50U, 0xa0U) &&
              !sf::game::legacyGuestUsesSecondaryItemModel(0x50U, 0x80U) &&
              !sf::game::legacyGuestUsesSecondaryItemModel(0x4eU, 0x20U),
          "Retail weapon-crate consumed latch lost its secondary model");
  using sf::game::LegacyDedicatedHmdActor;
  const auto dedicated_actor = sf::game::legacyDedicatedHmdActor;
  constexpr auto overlay_handler = 0x80150000U;
  const auto hans =
      dedicated_actor(true, 4U, 9U, 8U, sf::game::legacy_park2_hans_class,
                      sf::game::legacy_park2_hans_handler,
                      sf::game::legacy_park2_hans_attributes);
  const auto chopper =
      dedicated_actor(true, 9U, 2U, 2U, sf::game::legacy_chopper_class,
                      overlay_handler, sf::game::legacy_chopper_attributes);
  const auto bomb = dedicated_actor(
      true, 4U, 4U, 1U, sf::game::legacy_bomb_class, overlay_handler, 0U);
  require(hans == LegacyDedicatedHmdActor::park2_hans &&
              sf::game::legacy_park2_hans_handler == 0x80147004U &&
              chopper == LegacyDedicatedHmdActor::chopper &&
              bomb == LegacyDedicatedHmdActor::park2_bomb &&
              sf::game::legacyDedicatedHmdWeapon(hans) ==
                  sf::game::WeaponId::flamethrower &&
              sf::game::legacyDedicatedHmdWeapon(chopper) ==
                  sf::game::WeaponId::chopper_gun &&
              !sf::game::legacyDedicatedHmdWeapon(bomb),
          "Dedicated HMD actor/weapon mapping differs from retail identities");
  require(
      dedicated_actor(false, 4U, 9U, 8U, sf::game::legacy_park2_hans_class,
                      sf::game::legacy_park2_hans_handler,
                      sf::game::legacy_park2_hans_attributes) ==
              LegacyDedicatedHmdActor::none &&
          dedicated_actor(true, 3U, 9U, 8U, sf::game::legacy_park2_hans_class,
                          sf::game::legacy_park2_hans_handler,
                          sf::game::legacy_park2_hans_attributes) ==
              LegacyDedicatedHmdActor::none &&
          dedicated_actor(true, 4U, 8U, 8U, sf::game::legacy_park2_hans_class,
                          sf::game::legacy_park2_hans_handler,
                          sf::game::legacy_park2_hans_attributes) ==
              LegacyDedicatedHmdActor::none &&
          dedicated_actor(true, 4U, 9U, 7U, sf::game::legacy_park2_hans_class,
                          sf::game::legacy_park2_hans_handler,
                          sf::game::legacy_park2_hans_attributes) ==
              LegacyDedicatedHmdActor::none &&
          dedicated_actor(true, 4U, 9U, 8U, sf::game::legacy_park2_hans_class,
                          sf::game::legacy_common_npc_handler,
                          sf::game::legacy_park2_hans_attributes) ==
              LegacyDedicatedHmdActor::none &&
          dedicated_actor(true, 4U, 9U, 8U, sf::game::legacy_park2_hans_class,
                          sf::game::legacy_park2_hans_handler,
                          0x41ffU) == LegacyDedicatedHmdActor::none &&
          dedicated_actor(true, 9U, 2U, 2U, sf::game::legacy_chopper_class,
                          overlay_handler,
                          1U) == LegacyDedicatedHmdActor::none &&
          dedicated_actor(true, 0U, 2U, 2U, sf::game::legacy_chopper_class,
                          overlay_handler,
                          sf::game::legacy_chopper_attributes) ==
              LegacyDedicatedHmdActor::none,
      "Dedicated HMD profile accepted a substituted mission identity");
  const auto presentation = sf::game::legacyDedicatedHmdPresentationAllowed;
  require(presentation(hans, true, true, false, true, true) &&
              presentation(chopper, true, true, false, true, true) &&
              presentation(bomb, true, true, false, true, true) &&
              !presentation(LegacyDedicatedHmdActor::none, true, true, false,
                            true, true) &&
              !presentation(hans, false, true, false, true, true) &&
              !presentation(hans, true, false, false, true, true) &&
              !presentation(hans, true, true, true, true, true) &&
              !presentation(hans, true, true, false, false, true) &&
              !presentation(hans, true, true, false, true, false) &&
              presentation(bomb, true, true, false, true, false),
          "Dormant, hidden, dead or unposed dedicated HMD was presented");
  require(sf::game::legacyGuestHmdPoseComplete(15U, 15U) &&
              sf::game::legacyGuestHmdPoseComplete(9U, 9U) &&
              sf::game::legacyGuestHmdPoseComplete(15U, 9U) &&
              !sf::game::legacyGuestHmdPoseComplete(8U, 9U) &&
              !sf::game::legacyGuestHmdPoseComplete(0U, 0U),
          "Retail HMD pose completeness ignored the resolved model parts");
  auto retained_pose_complete = false;
  const auto sample_actor_pose = [&](std::size_t available_bones) {
    const auto current_pose_complete =
        sf::game::legacyGuestHmdPoseComplete(available_bones, 15U);
    const auto available = sf::game::legacyGuestActorPoseAvailable(
        current_pose_complete, retained_pose_complete);
    retained_pose_complete |= current_pose_complete;
    return available;
  };
  require(sample_actor_pose(15U) && sample_actor_pose(3U) &&
              sample_actor_pose(15U) &&
              !sf::game::legacyGuestActorPoseAvailable(false, false),
          "A partial bridge pose retired a previously posed actor lifetime");

  auto first_lifetime = sf::game::LegacyObjectBridgeState{};
  first_lifetime.definition = 37U;
  first_lifetime.class_id = 1;
  first_lifetime.authored_position = {120, 48, -330};
  first_lifetime.path_pointer = 0x801a4934U;
  first_lifetime.instance = 0x801b1000U;
  first_lifetime.attributes = 5U;
  first_lifetime.parameter = 2;
  first_lifetime.linked_slot = 17;
  auto second_lifetime = first_lifetime;
  second_lifetime.instance = 0x801b1800U;
  require(sf::game::legacyGuestIdentity(first_lifetime) ==
                  sf::game::legacyGuestIdentity(first_lifetime) &&
              sf::game::legacyGuestIdentity(first_lifetime) !=
                  sf::game::legacyGuestIdentity(second_lifetime),
          "Recycled guest instances collided in actor lifetime identity");
  require(sf::game::legacy_instance_dormant == 0x02U &&
              sf::game::legacyGuestActorStreamVisible(true, false, 0U, false,
                                                      true, false, true) &&
              sf::game::legacyGuestActorStreamVisible(false, true, 0U, false,
                                                      true, false, true) &&
              sf::game::legacyGuestActorStreamVisible(
                  false, false, sf::game::legacy_hmd_rendered_this_pass, false,
                  false, false, true) &&
              sf::game::legacyGuestActorStreamVisible(false, false, 0U, true,
                                                      false, false, true) &&
              !sf::game::legacyGuestActorStreamVisible(true, false, 0U, false,
                                                       true, false, false) &&
              !sf::game::legacyGuestActorStreamVisible(
                  false, false, sf::game::legacy_hmd_rendered_this_pass, false,
                  false, false, false) &&
              !sf::game::legacyGuestActorStreamVisible(true, false, 0U, false,
                                                       false, false, true) &&
              !sf::game::legacyGuestActorStreamVisible(false, false, 0U, false,
                                                       true, false, true) &&
              !sf::game::legacyGuestActorStreamVisible(
                  true, true, sf::game::legacy_hmd_rendered_this_pass, true,
                  true, true, true),
          "Dormant or unposed story actor escaped retail presentation");
  require(
      sf::game::legacyHmdFallbackUsesContactSpace(false, false, true) &&
          sf::game::legacyHmdFallbackUsesContactSpace(false, true, false) &&
          !sf::game::legacyHmdFallbackUsesContactSpace(false, false, false) &&
          !sf::game::legacyHmdFallbackUsesContactSpace(true, true, true),
      "Guest HMD fallback mixed contact and skeleton root spaces");
  require(
      sf::game::legacyManualAimPresentationActive(true, true, 0, false, false,
                                                  false) &&
          sf::game::legacyManualAimPresentationActive(true, true, 1, false,
                                                      false, false) &&
          !sf::game::legacyManualAimPresentationActive(false, false, 1, false,
                                                       false, false) &&
          !sf::game::legacyManualAimPresentationActive(false, true, 1, false,
                                                       false, false) &&
          !sf::game::legacyManualAimPresentationActive(true, true, 1, true,
                                                       false, false) &&
          !sf::game::legacyManualAimPresentationActive(true, true, 1, false,
                                                       true, false) &&
          !sf::game::legacyManualAimPresentationActive(true, true, 1, false,
                                                       false, true),
      "Retail traversal camera mode leaked into host manual-aim visibility");
  require(sf::game::legacyFirstPersonCircleAllowed(
              sf::game::WeaponId::sniper_rifle) &&
              sf::game::legacyFirstPersonCircleAllowed(
                  sf::game::WeaponId::nightvision_rifle) &&
              !sf::game::legacyFirstPersonCircleAllowed(
                  sf::game::WeaponId::fragmentation_grenade) &&
              !sf::game::legacyFirstPersonCircleAllowed(
                  sf::game::WeaponId::gas_grenade),
          "First-person Circle leaked from optics into grenade aim");
  require(
      sf::game::legacyFirstPersonAimInputAllowed(0U, false, false, false) &&
          !sf::game::legacyFirstPersonAimInputAllowed(1U, false, false,
                                                      false) &&
          !sf::game::legacyFirstPersonAimInputAllowed(0U, true, false, false) &&
          !sf::game::legacyFirstPersonAimInputAllowed(0U, false, true, false) &&
          !sf::game::legacyFirstPersonAimInputAllowed(0U, false, false, true),
      "First-person admission ignored roll, action, radio, or re-arm lock");
  require(!sf::game::legacyFirstPersonLocomotionInputAllowed(true) &&
              sf::game::legacyFirstPersonLocomotionInputAllowed(false),
          "First-person hold did not isolate the collision root");

  const auto first = sf::game::legacyDynamicPoolIndex(355U, 350U, 350U);
  const auto last = sf::game::legacyDynamicPoolIndex(355U, 350U, 354U);
  require(first && *first == 0U && last && *last == 4U &&
              !sf::game::legacyDynamicPoolIndex(355U, 350U, 349U) &&
              !sf::game::legacyDynamicPoolIndex(355U, 350U, 355U) &&
              !sf::game::legacyDynamicPoolIndex(3U, 4U, 4U),
          "Legacy dynamic suffix did not map to stable presentation slots");

  require(!sf::game::legacyDynamicBindingChanged(7U, 7U, 11U, 11U) &&
              sf::game::legacyDynamicBindingChanged(8U, 7U, 11U, 11U) &&
              sf::game::legacyDynamicBindingChanged(7U, 7U, 12U, 11U),
          "Legacy recycled identity did not trigger an exact scene rebind");

  require(
      sf::game::legacyPresentationTemplateMatches(7U, 0x01U, 7U, 0x01U) &&
          !sf::game::legacyPresentationTemplateMatches(6U, 0x01U, 7U, 0x01U) &&
          !sf::game::legacyPresentationTemplateMatches(7U, 0x35U, 7U, 0x01U) &&
          !sf::game::legacyPresentationTemplateMatches(std::nullopt, 0x01U, 7U,
                                                       0x01U),
      "Legacy presentation accepted a same-class or missing definition "
      "substitute");

  require(
      sf::game::legacySceneActiveAfterRoomRebuild(true, -1, false) &&
          sf::game::legacySceneActiveAfterRoomRebuild(false, 352, false) &&
          !sf::game::legacySceneActiveAfterRoomRebuild(false, -1, false) &&
          !sf::game::legacySceneActiveAfterRoomRebuild(true, 352, true),
      "Room rebuild did not preserve guest residency or honor despawn hiding");

  const auto texture_bank = sf::game::resolveTextureBankOwnership;
  require(
      texture_bank(0U, true, 0x02U) == 0U &&
          texture_bank(1U, true, 0x01U) == 1U &&
          texture_bank(0U, false, 0x02U) == 1U &&
          texture_bank(1U, false, 0x01U) == 0U &&
          texture_bank(0U, false, 0x03U) == 0U &&
          texture_bank(1U, false, 0x03U) == 1U &&
          texture_bank(1U, false, 0x00U) == 1U,
      "Texture bank ownership did not prefer current, unique or fail-closed "
      "provenance");
  require(sf::game::resident_weapon_texture_bank == 0U,
          "Resident weapon textures must remain in the authored SPFX bank");
  require(sf::game::resident_hmd_texture_bank == 0U,
          "Resident HMD textures must remain in their authored bank zero");
  require(sf::game::resolveDisplayedObjectTextureBank(1U, true) == 0U &&
              sf::game::resolveDisplayedObjectTextureBank(1U, false) == 1U,
          "Displayed HMD inherited a streamed room texture bank");
  const auto object_texture_bank = sf::game::resolveAuthoredObjectTextureBank;
  require(object_texture_bank(1U, false, false, 0x00U, 0x01U) == 0U &&
              object_texture_bank(0U, false, false, 0x00U, 0x02U) == 1U &&
              object_texture_bank(1U, false, false, 0x01U, 0x03U) == 0U &&
              object_texture_bank(0U, true, true, 0x03U, 0x03U) == 0U,
          "Retained objects lost authored texture ownership across a portal");
}

void testGameplayCheckpointRestorePolicy() {
  for (unsigned int mask = 0U; mask < 32U; ++mask) {
    const auto checkpoint_valid = (mask & 0x01U) != 0U;
    const auto runtime_present = (mask & 0x02U) != 0U;
    const auto runtime_ready = (mask & 0x04U) != 0U;
    const auto host_faulted = (mask & 0x08U) != 0U;
    const auto runtime_faulted = (mask & 0x10U) != 0U;
    const auto expected = checkpoint_valid && runtime_present &&
                          runtime_ready && !host_faulted && !runtime_faulted;
    require(sf::game::gameplayCheckpointRestoreReady(
                checkpoint_valid, runtime_present, runtime_ready, host_faulted,
                runtime_faulted) == expected,
            "Gameplay checkpoint accepted an incoherent guest runtime");
  }
}

void testPoliceLightbarFrames() {
  using sf::game::EffectTextureCopy;
  using sf::game::EffectVramRect;
  using sf::game::PoliceLightbarFrame;
  const auto copy = [](std::int16_t x, std::int16_t y, std::int16_t dx,
                       std::int16_t dy) {
    return EffectTextureCopy{EffectVramRect{x, y, 16, 32}, dx, dy};
  };
  const std::array expected{
      PoliceLightbarFrame{copy(656, 0, 640, 0), copy(672, 32, 640, 96)},
      PoliceLightbarFrame{copy(672, 0, 640, 0), copy(656, 96, 640, 96)},
      PoliceLightbarFrame{copy(688, 0, 640, 0), copy(640, 64, 640, 96)},
      PoliceLightbarFrame{copy(640, 32, 640, 0), copy(656, 64, 640, 96)},
  };
  for (std::size_t phase = 0; phase < expected.size(); ++phase) {
    require(sf::game::policeLightbarFrame(phase * 2U) == expected[phase],
            "Police lightbar VRAM sequence mismatch");
    require(sf::game::policeLightbarFrame(phase * 2U + 1U) == expected[phase],
            "Police lightbar frame cadence mismatch");
  }
  require(sf::game::policeLightbarFrame(8U) == expected[0],
          "Police lightbar sequence must loop after four frames");
}

std::vector<std::byte> makeHmdModel(bool flat_lit) {
  constexpr std::size_t geometry_offset = 0x34U;
  constexpr std::size_t part_size = 0xa4U;
  constexpr std::size_t geometry_end = geometry_offset + part_size;
  std::vector<std::byte> bytes(geometry_end + 0x20U);

  writeLe32(bytes, 0, 0x48000000U | static_cast<std::uint32_t>(flat_lit));
  writeLe32(bytes, 4, 1);
  writeLe32(bytes, 8, 4);
  writeLe32(bytes, 0x14, static_cast<std::uint32_t>(geometry_end));
  constexpr std::string_view model_name{"TEST"};
  std::ranges::transform(model_name, bytes.begin() + 0x1c, [](char value) {
    return static_cast<std::byte>(value);
  });

  writeLe32(bytes, 0x24, 0x7ab0140aU);
  writeLe32(bytes, 0x28, 0x008e281eU);
  writeLe32(bytes, 0x2c, 0x00003c32U);
  const auto stride = flat_lit ? 8U : 12U;
  writeLe32(bytes, 0x30, (2U * stride << 16U) | stride);

  writeLe32(bytes, geometry_offset, static_cast<std::uint32_t>(part_size));
  writeLe32(bytes, geometry_offset + 4U, 1);
  writeLe32(bytes, geometry_offset + 8U, 2);
  writeLe32(bytes, geometry_offset + 0x0cU, 2);
  writeLe16(bytes, geometry_offset + 0x10U, 4096);
  writeLe16(bytes, geometry_offset + 0x18U, 4096);
  writeLe16(bytes, geometry_offset + 0x20U, 4096);
  writeLe16(bytes, geometry_offset + 0x22U, 1);
  writeLe16(bytes, geometry_offset + 0x24U, 2);
  writeLe16(bytes, geometry_offset + 0x26U, 3);
  constexpr std::string_view part_name{"Root"};
  std::ranges::transform(
      part_name, bytes.begin() + geometry_offset + 0x28U,
      [](char value) { return static_cast<std::byte>(value); });
  writeLe32(bytes, geometry_offset + 0x30U, 0x12345678U);
  writeLe16(bytes, geometry_offset + 0x34U, 3);
  writeLe16(bytes, geometry_offset + 0x36U, 3);
  writeLe16(bytes, geometry_offset + 0x38U, 0xffffU);
  writeLe16(bytes, geometry_offset + 0x3aU, 0x8000U);
  writeLe32(bytes, geometry_offset + 0x3cU, 0x87654321U);
  writeLe32(bytes, geometry_offset + 0x40U, 0xa8U);

  constexpr std::array<sf::assets::HmdVertex, 3> vertices{{
      {-10, -20, -30},
      {40, 50, 60},
      {70, 80, 90},
  }};
  constexpr std::array<sf::assets::HmdVertex, 3> normals{{
      {4096, 0, 0},
      {0, 4096, 0},
      {0, 0, 4096},
  }};
  const auto write_vertices = [&](std::size_t offset, const auto &values) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      writeLe16(bytes, offset + index * 8U,
                static_cast<std::uint16_t>(values[index].x));
      writeLe16(bytes, offset + index * 8U + 2U,
                static_cast<std::uint16_t>(values[index].y));
      writeLe16(bytes, offset + index * 8U + 4U,
                static_cast<std::uint16_t>(values[index].z));
    }
  };
  write_vertices(0x78U, vertices);
  write_vertices(0xa8U, normals);

  writeLe32(bytes, geometry_end, static_cast<std::uint32_t>(-10));
  writeLe32(bytes, geometry_end + 4U, static_cast<std::uint32_t>(-20));
  writeLe32(bytes, geometry_end + 8U, static_cast<std::uint32_t>(-30));
  writeLe32(bytes, geometry_end + 0x10U, 70);
  writeLe32(bytes, geometry_end + 0x14U, 80);
  writeLe32(bytes, geometry_end + 0x18U, 90);
  return bytes;
}

void testHmdModel() {
  const auto bytes = makeHmdModel(false);
  const auto model = sf::assets::HmdModel::parse(bytes);
  require(model.flags() == 0x48000000U && !model.flatLit() &&
              model.name() == "TEST",
          "HMD header mismatch");
  require(model.parts().size() == 1 && model.vertices().size() == 3 &&
              model.normals().size() == 3 && model.triangles().size() == 1,
          "HMD table counts mismatch");
  const auto &part = model.parts().front();
  require(part.name == "Root" && part.parent == -1 &&
              part.hierarchy_flags == 0x8000U &&
              part.local_transform.rotation[0] == 4096 &&
              part.local_transform.translation ==
                  std::array<std::int16_t, 3>{1, 2, 3},
          "HMD hierarchy data mismatch");
  require(part.first_vertex == 0 && part.vertex_count == 3 &&
              part.padded_vertex_count == 6 && part.padded_normal_count == 6 &&
              part.declared_vertex_count == 3 &&
              part.bounds.minimum ==
                  std::array<std::int32_t, 3>{-10, -20, -30} &&
              part.bounds.maximum == std::array<std::int32_t, 3>{70, 80, 90},
          "HMD part metadata mismatch");
  require(model.vertices()[1].x == 40 && model.normals()[2].z == 4096 &&
              model.vertexParts() == std::vector<std::uint16_t>({0, 0, 0}),
          "HMD flattened geometry mismatch");
  const auto &triangle = model.triangles().front();
  require(triangle.vertex_indices == std::array<std::uint16_t, 3>{0, 1, 2} &&
              triangle.uv[0].u == 10 && triangle.uv[0].v == 20 &&
              triangle.uv[2].u == 50 && triangle.uv[2].v == 60,
          "HMD triangle data mismatch");
  require(triangle.clut == 0x7ab0U && triangle.texture_page == 0x008eU &&
              model.texturePageMask() == (1U << 14U),
          "HMD material selectors mismatch");

  const auto flat_model = sf::assets::HmdModel::parse(makeHmdModel(true));
  require(flat_model.flatLit() &&
              flat_model.triangles().front().vertex_indices ==
                  std::array<std::uint16_t, 3>{0, 1, 2},
          "Flat-lit HMD vertex stride mismatch");

  auto invalid = bytes;
  writeLe16(invalid, 0x34U + 0x38U, 0);
  try {
    static_cast<void>(sf::assets::HmdModel::parse(invalid));
    throw std::runtime_error{"Invalid HMD hierarchy was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_format,
            "Invalid HMD returned the wrong error code");
  }
}

void testHmdAnimation() {
  constexpr std::array<std::uint8_t, 46> encoded{
      0xfa, 0x01, 0x00, 0x01, 0xbf, 0x85, 0xfb, 0x2e, 0xf6, 0xd7, 0x01, 0xfe,
      0x03, 0xfa, 0x02, 0x00, 0x01, 0x07, 0xc3, 0x04, 0xfb, 0x06, 0xfa, 0x03,
      0x00, 0x01, 0x93, 0x94, 0x6c, 0x07, 0x08, 0x09, 0xfa, 0x04, 0x00, 0x01,
      0xed, 0x43, 0x23, 0x9c, 0x0a, 0x0b, 0x0c, 0xfa, 0xfc, 0x00,
  };
  std::vector<std::byte> bytes(encoded.size());
  std::ranges::transform(encoded, bytes.begin(), [](std::uint8_t value) {
    return static_cast<std::byte>(value);
  });

  const auto clip = sf::assets::HmdAnimationClip::parse(bytes, 1U);
  require(clip.partCount() == 1U && clip.duration() == 4U &&
              !clip.hasRootMotion() && clip.animatedParts() == 1U &&
              clip.frames().size() == 4U,
          "HMD animation header mismatch");
  const auto &first = clip.frames()[0];
  const auto &second = clip.frames()[1];
  const auto &third = clip.frames()[2];
  const auto &fourth = clip.frames()[3];
  require(first.transforms[0].rotation ==
                  std::array<std::int16_t, 3>{-123, -1234, -2345} &&
              first.transforms[0].translation ==
                  std::array<std::int16_t, 3>{1, 2, 3},
          "HMD animation absolute key mismatch");
  require(second.transforms[0].rotation ==
                  std::array<std::int16_t, 3>{-122, -1236, -2342} &&
              second.transforms[0].translation ==
                  std::array<std::int16_t, 3>{1, 2, 3},
          "HMD animation delta key mismatch");
  require(third.transforms[0].rotation ==
                  std::array<std::int16_t, 3>{-172, -1196, -2362} &&
              fourth.transforms[0].rotation ==
                  std::array<std::int16_t, 3>{-472, -996, -2462},
          "HMD animation packed deltas mismatch");
  require(&clip.poseAtTick(0U) == &first && &clip.poseAtTick(1U) == &second &&
              &clip.poseAtTick(3U) == &fourth && &clip.poseAtTick(4U) == &first,
          "HMD animation timeline mismatch");

  constexpr std::array<std::uint8_t, 16> root_track{
      0x01, 0x64, 0x05, 0x00, 0xff, 0x65, 0x06, 0x00,
      0x00, 0x66, 0x07, 0x00, 0x00, 0x67, 0x08, 0x00,
  };
  constexpr std::size_t rooted_animation_offset = 24U;
  std::vector<std::byte> rooted(rooted_animation_offset + bytes.size());
  rooted[0] = std::byte{0xea};
  rooted[1] = static_cast<std::byte>(rooted_animation_offset);
  std::ranges::transform(
      root_track, rooted.begin() + 4,
      [](std::uint8_t value) { return static_cast<std::byte>(value); });
  rooted[rooted_animation_offset - 4U] = std::byte{0xef};
  rooted[rooted_animation_offset - 3U] = std::byte{0xef};
  rooted[rooted_animation_offset - 1U] =
      static_cast<std::byte>(rooted_animation_offset);
  std::ranges::copy(bytes, rooted.begin() + rooted_animation_offset);
  const auto rooted_clip = sf::assets::HmdAnimationClip::parse(rooted, 1U);
  require(rooted_clip.hasRootMotion() && rooted_clip.rootMotion().size() == 4U,
          "HMD root-motion prefix was not decoded");
  require(rooted_clip.rootMotion()[0].x == 1 &&
              rooted_clip.rootMotion()[0].y == 100 &&
              rooted_clip.rootMotion()[0].z == 5 &&
              rooted_clip.rootMotion()[1].x == -1 &&
              rooted_clip.rootMotion()[3].y == 103 &&
              rooted_clip.rootMotion()[3].z == 8,
          "HMD root-motion values were decoded incorrectly");
  auto root_cycle_distance = 0.0;
  for (std::uint64_t tick = 0U; tick < rooted_clip.rootMotion().size();
       ++tick) {
    root_cycle_distance += 2.0 * sf::game::rootMotionForwardDistance(
                                     rooted_clip.rootMotion(), tick, 2U);
  }
  require(std::abs(root_cycle_distance - 26.0) < 0.0001,
          "HMD root motion was not distributed across 60 Hz updates");
  require(std::abs(sf::game::rootMotionPlanarDistance(rooted_clip.rootMotion(),
                                                      0U, 2U) -
                   std::sqrt(26.0) / 2.0) < 0.0001,
          "HMD planar root motion lost its lateral component");

  bytes.resize(bytes.size() - 3U);
  try {
    static_cast<void>(sf::assets::HmdAnimationClip::parse(bytes, 1U));
    throw std::runtime_error{"Unterminated HMD animation was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_format,
            "Invalid HMD animation returned the wrong error code");
  }
}

void testActorAnimationBank() {
  const auto make_clip = [](std::uint16_t mask, std::uint16_t rotation_x) {
    std::vector<std::byte> bytes{
        std::byte{0xfa},
        std::byte{0x01},
        static_cast<std::byte>(mask >> 8U),
        static_cast<std::byte>(mask & 0xffU),
    };
    for (std::size_t part = 0; part < 15U; ++part) {
      if ((mask & (std::uint16_t{1} << part)) == 0U) {
        continue;
      }
      bytes.push_back(static_cast<std::byte>(0xa0U | (rotation_x >> 8U)));
      bytes.push_back(static_cast<std::byte>(rotation_x & 0xffU));
      bytes.insert(bytes.end(), 7U, std::byte{0});
    }
    bytes.push_back(std::byte{0xfa});
    bytes.push_back(std::byte{0xfc});
    return bytes;
  };

  constexpr std::array names{
      std::string_view{"ST0.LWR"},    std::string_view{"ST02.UPR"},
      std::string_view{"WK0.LWR"},    std::string_view{"WK0.UPR"},
      std::string_view{"RN0.LWR"},    std::string_view{"RN0.UPR"},
      std::string_view{"IDLE13.HAN"}, std::string_view{"SWIT0_1.UPR"},
      std::string_view{"CLIMBA.HAN"}, std::string_view{"KIKDR.HAN"},
      std::string_view{"ST1AIM.UPR"}, std::string_view{"KN0.LWR"},
      std::string_view{"STKN0.HAN"},  std::string_view{"STKN2.HAN"},
  };
  constexpr std::uint16_t lower_mask = 0x54d2U;
  constexpr std::uint16_t upper_mask = 0x2b2dU;
  const std::array clips{
      make_clip(lower_mask, 100U), make_clip(upper_mask, 101U),
      make_clip(lower_mask, 200U), make_clip(upper_mask, 201U),
      make_clip(lower_mask, 300U), make_clip(upper_mask, 301U),
      make_clip(0x7fffU, 400U),    make_clip(upper_mask, 501U),
      make_clip(0x7fffU, 502U),    make_clip(0x7fffU, 503U),
      make_clip(upper_mask, 504U), make_clip(lower_mask, 600U),
      make_clip(0x7fffU, 601U),    make_clip(0x7fffU, 602U),
  };

  constexpr std::size_t header_size = 20U + names.size() * 4U;
  auto names_size = std::size_t{};
  auto data_size = std::size_t{};
  for (std::size_t index = 0; index < names.size(); ++index) {
    names_size += names[index].size() + 1U;
    data_size += clips[index].size();
  }
  const auto data_offset = header_size + names_size;
  std::vector<std::byte> hog(data_offset + data_size);
  writeLe32(hog, 0U, 0x36a4f0aeU);
  writeLe32(hog, 4U, static_cast<std::uint32_t>(names.size()));
  writeLe32(hog, 8U, 0x14U);
  writeLe32(hog, 12U, static_cast<std::uint32_t>(header_size));
  writeLe32(hog, 16U, static_cast<std::uint32_t>(data_offset));
  auto name_cursor = header_size;
  auto data_cursor = data_offset;
  for (std::size_t index = 0; index < names.size(); ++index) {
    writeLe32(hog, 20U + index * 4U,
              static_cast<std::uint32_t>(data_cursor - data_offset));
    std::ranges::transform(
        names[index], hog.begin() + name_cursor,
        [](char value) { return static_cast<std::byte>(value); });
    name_cursor += names[index].size() + 1U;
    std::ranges::copy(clips[index], hog.begin() + data_cursor);
    data_cursor += clips[index].size();
  }

  const auto archive = sf::assets::HogArchive::parse(std::move(hog));
  const sf::game::ActorAnimationBank bank{archive, 15U};
  const auto standing = bank.playerPose(sf::game::ActorMotion::idle, 0U);
  const auto walking = bank.playerPose(sf::game::ActorMotion::walk, 1U);
  const auto running = bank.playerPose(sf::game::ActorMotion::run, 2U);
  const auto enemy = bank.enemyPose(3U, 7U);
  const auto sidearm_draw = bank.playerPose(
      sf::game::PlayerAnimationRequest{
          .motion = sf::game::ActorMotion::idle,
          .upper_action = sf::game::PlayerUpperAction::draw,
          .weapon_stance = sf::game::PlayerWeaponStance::sidearm,
      },
      0U);
  const auto walking_sidearm_draw = bank.playerPose(
      sf::game::PlayerAnimationRequest{
          .motion = sf::game::ActorMotion::walk,
          .upper_action = sf::game::PlayerUpperAction::draw,
          .weapon_stance = sf::game::PlayerWeaponStance::sidearm,
      },
      0U);
  const auto climbing = bank.playerPose(sf::game::ActorMotion::climb, 0U);
  const auto kicking_door =
      bank.playerPose(sf::game::ActorMotion::kick_door, 0U);
  const auto sidearm_aim = bank.playerPose(
      sf::game::PlayerAnimationRequest{
          .motion = sf::game::ActorMotion::idle,
          .upper_action = sf::game::PlayerUpperAction::aim,
          .weapon_stance = sf::game::PlayerWeaponStance::sidearm,
      },
      0U);
  const auto sidearm_fire = bank.playerPose(
      sf::game::PlayerAnimationRequest{
          .motion = sf::game::ActorMotion::idle,
          .upper_action = sf::game::PlayerUpperAction::fire,
          .weapon_stance = sf::game::PlayerWeaponStance::sidearm,
      },
      0U);
  const auto kneeling_sidearm = bank.playerPose(
      sf::game::PlayerAnimationRequest{
          .motion = sf::game::ActorMotion::kneel,
          .weapon_stance = sf::game::PlayerWeaponStance::sidearm,
      },
      0U);
  const auto kneeling_long_gun = bank.playerPose(
      sf::game::PlayerAnimationRequest{
          .motion = sf::game::ActorMotion::kneel,
          .weapon_stance = sf::game::PlayerWeaponStance::long_gun,
      },
      0U);
  const auto standing_sidearm_timing = sf::game::playerAnimationTiming({
      .motion = sf::game::ActorMotion::idle,
      .weapon_stance = sf::game::PlayerWeaponStance::sidearm,
  });
  const auto running_long_gun_timing = sf::game::playerAnimationTiming({
      .motion = sf::game::ActorMotion::run,
      .weapon_stance = sf::game::PlayerWeaponStance::long_gun,
  });
  for (std::size_t part = 0; part < 15U; ++part) {
    require(standing.transform(part) != nullptr &&
                walking.transform(part) != nullptr &&
                running.transform(part) != nullptr &&
                enemy.transform(part) != nullptr,
            "Actor animation split left a skeleton part unposed");
  }
  require(standing.transform(0U)->rotation[0] == 101 &&
              walking.transform(0U)->rotation[0] == 201 &&
              running.transform(0U)->rotation[0] == 301 &&
              enemy.transform(0U)->rotation[0] == 400,
          "Actor animation bank selected the wrong channel");
  require(sidearm_draw.transform(0U)->rotation[0] == 501 &&
              walking_sidearm_draw.transform(0U)->rotation[0] == 501,
          "Sidearm draw did not use the native SWIT0_1 upper-body clip");
  require(bank.hasPlayerAnimation({.motion = sf::game::ActorMotion::climb}) &&
              bank.hasPlayerAnimation(
                  {.motion = sf::game::ActorMotion::kick_door}) &&
              climbing.transform(0U)->rotation[0] == 502 &&
              kicking_door.transform(0U)->rotation[0] == 503,
          "Contextual interaction did not select its native full-body clip");
  require(bank.hasPlayerAnimation(sf::game::PlayerAnimationRequest{
              .motion = sf::game::ActorMotion::idle,
              .upper_action = sf::game::PlayerUpperAction::fire,
              .weapon_stance = sf::game::PlayerWeaponStance::sidearm,
          }) &&
              sidearm_aim.transform(5U)->rotation[0] == 504 &&
              sidearm_fire.transform(5U)->rotation[0] == 408,
          "Procedural sidearm recoil did not animate the native aiming pose");
  require(
      kneeling_sidearm.transform(0U)->rotation[0] == 601 &&
          kneeling_sidearm.transform(1U)->rotation[0] == 600 &&
          kneeling_long_gun.transform(0U)->rotation[0] == 602 &&
          kneeling_long_gun.transform(1U)->rotation[0] == 600,
      "Neutral kneel did not preserve the final native transition upper pose");
  require(standing_sidearm_timing.reload_updates == 27U &&
              standing_sidearm_timing.draw_updates == 28U &&
              running_long_gun_timing.reload_updates == 14U &&
              running_long_gun_timing.draw_updates == 14U &&
              standing_sidearm_timing.interact_updates == 46U,
          "Player action timings no longer match the native 20 Hz PCHAN clips");
}

void testChaseCamera() {
  constexpr auto epsilon = 0.0001;
  constexpr auto player_x = 100.0;
  constexpr auto player_y = 200.0;
  constexpr auto player_z = 300.0;
  const auto dot = [](double first_x, double first_z, double second_x,
                      double second_z) {
    return first_x * second_x + first_z * second_z;
  };
  const auto cross = [](double first_x, double first_z, double second_x,
                        double second_z) {
    return first_x * second_z - first_z * second_x;
  };
  const auto require_aligned_behind = [&](const sf::game::CameraState &state,
                                          std::int32_t heading) {
    const auto forward = sf::game::headingDirection(heading);
    const auto camera_x = state.x - player_x;
    const auto camera_z = state.z - player_z;
    const auto target_x = state.target_x - player_x;
    const auto target_z = state.target_z - player_z;
    require(dot(camera_x, camera_z, forward.x, forward.z) < -epsilon &&
                std::abs(cross(camera_x, camera_z, forward.x, forward.z)) <
                    epsilon,
            "Chase camera is not directly behind the current player heading");
    require(dot(target_x, target_z, forward.x, forward.z) >= -epsilon &&
                std::abs(cross(target_x, target_z, forward.x, forward.z)) <
                    epsilon,
            "Chase camera target left the current player heading axis");

    const auto view_x = state.target_x - state.x;
    const auto view_z = state.target_z - state.z;
    require(dot(view_x, view_z, forward.x, forward.z) > epsilon &&
                std::abs(cross(view_x, view_z, forward.x, forward.z)) < epsilon,
            "Camera view and player movement use different forward directions");
  };

  require(sf::game::normalizeHeading(-1) == 4095 &&
              sf::game::normalizeHeading(4096) == 0 &&
              sf::game::normalizeHeading(4096 + 1024) == 1024,
          "Gameplay heading normalization failed");

  const auto north = sf::game::headingDirection(0);
  const auto east = sf::game::headingDirection(1024);
  require(std::abs(north.x) < 0.0001 && std::abs(north.z - 1.0) < 0.0001 &&
              std::abs(east.x - 1.0) < 0.0001 && std::abs(east.z) < 0.0001,
          "Gameplay heading basis is inconsistent");

  const sf::game::ChaseCamera camera;
  for (const auto heading : std::array<std::int32_t, 8>{
           0, 512, 1024, 1536, 2048, 2560, 3072, 3584}) {
    const auto basis = sf::game::headingBasis(heading);
    require(sf::game::headingFromDirection(basis.forward.x, basis.forward.z) ==
                heading,
            "Heading did not survive a model-forward round trip");
    require(std::abs(dot(basis.right.x, basis.right.z, basis.forward.x,
                         basis.forward.z)) < epsilon,
            "Player heading basis is not orthogonal");
    require_aligned_behind(camera.follow(player_x, player_y, player_z, heading),
                           heading);
  }

  const auto east_basis = sf::game::headingBasis(1024);
  const auto fixed = [](double value) {
    return static_cast<std::int16_t>(std::lround(value * 4096.0));
  };
  const std::array<std::int16_t, 9> east_rotation{
      fixed(east_basis.right.x), 0, fixed(east_basis.forward.x), 0, 4096, 0,
      fixed(east_basis.right.z), 0, fixed(east_basis.forward.z),
  };
  const auto east_from_model =
      sf::game::headingFromDirection(static_cast<double>(east_rotation[2]),
                                     static_cast<double>(east_rotation[8]));
  const auto east_model_forward = sf::game::headingDirection(east_from_model);
  const auto east_camera =
      camera.follow(player_x, player_y, player_z, east_from_model);
  require(east_from_model == 1024 &&
              std::abs(east_model_forward.x - 1.0) < epsilon &&
              std::abs(east_model_forward.z) < epsilon &&
              std::abs(east_camera.target_x - player_x) < epsilon &&
              std::abs(east_camera.target_z - player_z) < epsilon,
          "Model local +Z, forward movement and camera target do not share +X");

  const auto behind_north = camera.follow(player_x, player_y, player_z, 0);
  require(std::abs(behind_north.x - 100.0) < 0.0001 &&
              std::abs(behind_north.y + 100.0) < 0.0001 &&
              std::abs(behind_north.z + 372.0) < 0.0001 &&
              std::abs(behind_north.target_x - 100.0) < 0.0001 &&
              std::abs(behind_north.target_y - 15.0) < 0.0001 &&
              std::abs(behind_north.target_z - 300.0) < 0.0001,
          "Chase camera did not stay behind the player");

  const auto project_player_y = [&](double world_y) {
    constexpr auto native_screen_center_y = 120.0;
    constexpr auto native_projection = 320.0;
    const auto forward_y = behind_north.target_y - behind_north.y;
    const auto forward_z = behind_north.target_z - behind_north.z;
    const auto forward_length = std::hypot(forward_y, forward_z);
    const auto normalized_y = forward_y / forward_length;
    const auto normalized_z = forward_z / forward_length;
    const auto relative_y = world_y - behind_north.y;
    const auto relative_z = player_z - behind_north.z;
    const auto view_y = relative_y * normalized_z - relative_z * normalized_y;
    const auto view_depth =
        relative_y * normalized_y + relative_z * normalized_z;
    require(view_depth > epsilon, "Player is behind the chase camera");
    return native_screen_center_y + native_projection * view_y / view_depth;
  };
  constexpr auto player_height = 390.0;
  const auto projected_head_y = project_player_y(player_y - player_height);
  const auto projected_feet_y = project_player_y(player_y);
  require(projected_head_y >= 12.0 && projected_head_y <= 36.0 &&
              projected_feet_y >= 190.0 && projected_feet_y <= 214.0,
          "Retail chase framing no longer keeps Gabe's full body in view");

  const auto behind_east = camera.follow(player_x, player_y, player_z, 1024);
  require(std::abs(behind_east.x + 572.0) < 0.0001 &&
              std::abs(behind_east.z - 300.0) < 0.0001 &&
              std::abs(behind_east.target_x - 100.0) < 0.0001 &&
              std::abs(behind_east.target_z - 300.0) < 0.0001,
          "Chase camera did not rotate with the player heading");

  const sf::game::FirstPersonCamera aim_camera;
  const auto level_aim = aim_camera.view(player_x, player_y, player_z, 0, 0.0);
  require(std::abs(level_aim.x - player_x) < epsilon &&
              std::abs(level_aim.y - 150.0) < epsilon &&
              std::abs(level_aim.z - 334.0) < epsilon &&
              std::abs(level_aim.target_y - 150.0) < epsilon &&
              std::abs(level_aim.target_z - 1934.0) < epsilon,
          "First-person aiming camera is no longer at the lowered requested "
          "height");

  const sf::game::CameraState previous_peek{100.0, 150.0,  334.0, 100.0,
                                            150.0, 1934.0, 320};
  const sf::game::CameraState current_peek{164.0, 148.0,  350.0, 364.0,
                                           248.0, 1938.0, 400};
  const auto presented_peek = sf::game::interpolateCameraPresentation(
      previous_peek, current_peek, 0.25, true, false);
  require(std::abs(presented_peek.x - 116.0) < epsilon &&
              std::abs(presented_peek.y - 149.5) < epsilon &&
              std::abs(presented_peek.z - 338.0) < epsilon &&
              std::abs((presented_peek.target_x - presented_peek.x) - 200.0) <
                  epsilon &&
              std::abs((presented_peek.target_y - presented_peek.y) - 100.0) <
                  epsilon &&
              std::abs((presented_peek.target_z - presented_peek.z) - 1588.0) <
                  epsilon &&
              presented_peek.projection == 340,
          "First-person presentation did not smooth the collision-limited "
          "peek while retaining the current sight vector");

  const auto aim_cut = sf::game::interpolateCameraPresentation(
      previous_peek, current_peek, 0.25, true, true);
  require(aim_cut.x == current_peek.x && aim_cut.y == current_peek.y &&
              aim_cut.z == current_peek.z &&
              aim_cut.target_x == current_peek.target_x &&
              aim_cut.target_y == current_peek.target_y &&
              aim_cut.target_z == current_peek.target_z &&
              aim_cut.projection == current_peek.projection,
          "Aim entry/exit camera cut was accidentally interpolated");

  const auto lowered_sight = sf::game::cameraRayAtProjectionOffset(
      level_aim, 0.0, sf::game::manual_aim_reticle_vertical_offset);
  const auto lowered_screen_y =
      120.0 + static_cast<double>(level_aim.projection) *
                  lowered_sight.direction_y / lowered_sight.direction_z;
  require(std::abs(lowered_screen_y - 120.0) < epsilon &&
              std::abs(lowered_sight.direction_x) < epsilon,
          "Manual-aim ray no longer passes through the centred reticle");

  const auto turned = camera.follow(player_x, player_y, player_z, 1024);
  require_aligned_behind(turned, 1024);

  constexpr auto forward_step = 80.0;
  const auto moved_x = player_x + east.x * forward_step;
  const auto moved_z = player_z + east.z * forward_step;
  const auto advanced = camera.follow(moved_x, player_y, moved_z, 1024);
  const auto camera_delta_x = advanced.x - turned.x;
  const auto camera_delta_z = advanced.z - turned.z;
  const auto target_delta_x = advanced.target_x - turned.target_x;
  const auto target_delta_z = advanced.target_z - turned.target_z;
  require(
      dot(camera_delta_x, camera_delta_z, east.x, east.z) > epsilon &&
          std::abs(cross(camera_delta_x, camera_delta_z, east.x, east.z)) <
              epsilon &&
          dot(target_delta_x, target_delta_z, east.x, east.z) > epsilon &&
          std::abs(cross(target_delta_x, target_delta_z, east.x, east.z)) <
              epsilon,
      "Chase camera continued moving along the previous heading after a turn");
}

class TestPlayerMovement final : public sf::game::PlayerMovementResolver {
public:
  bool allow{true};
  unsigned int failures_before_accept{};
  unsigned int attempts{};

  bool tryMove(sf::game::PlayerState &player, double desired_x,
               double desired_z) override {
    ++attempts;
    if (!allow || failures_before_accept > 0U) {
      if (failures_before_accept > 0U) {
        --failures_before_accept;
      }
      return false;
    }
    player.x = desired_x;
    player.z = desired_z;
    player.grounded = true;
    return true;
  }
};

void testPlayerController() {
  constexpr std::array walking{
      sf::assets::HmdRootMotionFrame{0, 0, 10, 0},
      sf::assets::HmdRootMotionFrame{0, 0, 10, 0},
  };
  constexpr std::array running{
      sf::assets::HmdRootMotionFrame{0, 0, 20, 0},
      sf::assets::HmdRootMotionFrame{0, 0, 20, 0},
  };
  constexpr std::array strafe{
      sf::assets::HmdRootMotionFrame{12, 0, 0, 0},
  };
  sf::game::PlayerController controller;
  controller.setRootMotionTracks(walking, running);
  controller.setStrafeRootMotionTracks(strafe, strafe);
  const sf::game::PlayerState spawn{100.0, 200.0, 300.0, 0, true};
  controller.reset(spawn);
  TestPlayerMovement movement;
  const auto initial_chase_camera = controller.camera();

  require(controller.state().x == spawn.x && controller.state().z == spawn.z &&
              controller.locomotion() ==
                  sf::game::PlayerLocomotionState::idle &&
              controller.action() == sf::game::PlayerActionState::ready &&
              controller.aim() == sf::game::PlayerAimState::chase &&
              controller.weaponSwitch() ==
                  sf::game::PlayerWeaponSwitchState::none &&
              controller.animationTick() == 0U,
          "Player controller reset state mismatch");

  controller.update(sf::game::PlayerInput{.move = 1.0}, movement);
  require(controller.locomotion() == sf::game::PlayerLocomotionState::walking &&
              controller.actorMotion() == sf::game::ActorMotion::walk &&
              std::abs(controller.state().z - 310.0) < 0.0001 &&
              movement.attempts == 1U,
          "Player walking root motion mismatch");
  require(controller.camera().z < controller.state().z &&
              controller.cameraIntent().mode ==
                  sf::game::PlayerCameraMode::chase,
          "Player chase-camera intent mismatch");
  const auto player_camera_delta =
      controller.camera().z - initial_chase_camera.z;
  require(
      player_camera_delta > 0.0 && player_camera_delta < 10.0,
      "Chase camera remained rigidly attached instead of following smoothly");

  const auto first_person_step = [&](double move, double strafe) {
    controller.reset(spawn);
    controller.update(
        sf::game::PlayerInput{
            .move = move,
            .aim = true,
            .strafe = strafe,
        },
        movement);
    return controller.state();
  };
  const auto forward = first_person_step(1.0, 0.0);
  const auto left = first_person_step(0.0, -1.0);
  const auto backward = first_person_step(-1.0, 0.0);
  const auto right = first_person_step(0.0, 1.0);
  const auto root_is_fixed = [&spawn](const sf::game::PlayerState &state) {
    return std::abs(state.x - spawn.x) < 0.0001 &&
           std::abs(state.y - spawn.y) < 0.0001 &&
           std::abs(state.z - spawn.z) < 0.0001 && state.grounded;
  };
  require(root_is_fixed(forward) && root_is_fixed(left) &&
              root_is_fixed(backward) && root_is_fixed(right),
          "First-person W/A/S/D reached low-level collision movement");

  controller.reset(spawn);
  controller.update(
      sf::game::PlayerInput{
          .move = 1.0,
          .aim = true,
          .look_yaw = 1024.0,
      },
      movement);
  require(root_is_fixed(controller.state()) &&
              controller.state().yaw == spawn.yaw &&
              controller.aimHeading() == 1024,
          "First-person look moved Gabe's collision root or body heading");

  controller.reset(spawn);
  controller.update(
      sf::game::PlayerInput{
          .aim = true,
          .strafe = -1.0,
      },
      movement);
  controller.advanceAnimationClock();
  const auto accepted_left_root = controller.state();
  const auto accepted_left_tick = controller.animationTick();
  const auto accepted_left_heading = controller.aimHeading();
  controller.synchronizeFirstPersonRoot(accepted_left_root);
  require(controller.animationTick() == accepted_left_tick &&
              controller.aimHeading() == accepted_left_heading &&
              controller.aim() == sf::game::PlayerAimState::first_person,
          "Retail root synchronization reset first-person movement state");
  controller.update(
      sf::game::PlayerInput{
          .aim = true,
          .strafe = -1.0,
      },
      movement);
  require(root_is_fixed(controller.state()) &&
              controller.animationTick() == accepted_left_tick,
          "Held first-person movement changed the synchronized collision root");

  const auto stable_aim_root = controller.state();
  const sf::game::PlayerState unresolved_aim_root{
      stable_aim_root.x + 7.0,
      stable_aim_root.y + 4096.0,
      stable_aim_root.z + 9.0,
      1024,
      false,
  };
  controller.synchronizeFirstPersonRoot(unresolved_aim_root);
  require(std::abs(controller.state().x - unresolved_aim_root.x) < 0.0001 &&
              std::abs(controller.state().z - unresolved_aim_root.z) < 0.0001 &&
              std::abs(controller.state().y - stable_aim_root.y) < 0.0001 &&
              controller.state().grounded && controller.state().yaw == 1024,
          "Unresolved first-person pose replaced the grounded world height");
  const sf::game::PlayerState resolved_aim_root{
      unresolved_aim_root.x,
      stable_aim_root.y + 12.0,
      unresolved_aim_root.z,
      1024,
      true,
  };
  controller.synchronizeFirstPersonRoot(resolved_aim_root);
  require(std::abs(controller.state().y - resolved_aim_root.y) < 0.0001 &&
              controller.state().grounded,
          "Resolved first-person floor contact did not update world height");
  const auto accepted_ground_height = controller.state().y;
  auto invalid_grounded_aim_root = resolved_aim_root;
  invalid_grounded_aim_root.y +=
      sf::game::PlayerController::maximum_first_person_root_height_step + 1.0;
  controller.synchronizeFirstPersonRoot(invalid_grounded_aim_root);
  require(std::abs(controller.state().y - accepted_ground_height) < 0.0001 &&
              controller.state().grounded,
          "Out-of-range first-person pose bypassed the grounded height guard");
  controller.setWeaponStance(sf::game::PlayerWeaponStance::sidearm);
  controller.update(
      sf::game::PlayerInput{
          .aim = true,
          .roll = true,
      },
      movement);
  require(controller.action() == sf::game::PlayerActionState::rolling &&
              controller.aim() == sf::game::PlayerAimState::chase &&
              std::abs(controller.state().y - accepted_ground_height) <
                  0.0001 &&
              controller.state().grounded,
          "Sidearm/grenade roll transition corrupted the grounded root");

  controller.reset(spawn);
  movement.allow = false;
  const auto blocked_attempt = movement.attempts;
  controller.update(
      sf::game::PlayerInput{
          .move = 1.0,
          .aim = true,
      },
      movement);
  require(controller.state().x == spawn.x && controller.state().z == spawn.z &&
              controller.locomotion() ==
                  sf::game::PlayerLocomotionState::idle &&
              movement.attempts == blocked_attempt,
          "Blocked first-person W reached the collision resolver");
  movement.allow = true;

  controller.reset(spawn);
  movement.failures_before_accept = 1U;
  const auto slide_attempt = movement.attempts;
  controller.update(
      sf::game::PlayerInput{
          .move = 1.0,
          .aim = true,
          .strafe = 1.0,
      },
      movement);
  require(root_is_fixed(controller.state()) &&
              movement.attempts == slide_attempt,
          "Blocked first-person diagonal reached collision sliding");
  movement.failures_before_accept = 0U;

  controller.reset(spawn);
  const auto attempts_before_aim = movement.attempts;
  controller.update(
      sf::game::PlayerInput{
          .move = 1.0,
          .turn = 1.0,
          .aim = true,
          .strafe = 1.0,
      },
      movement);
  require(controller.locomotion() == sf::game::PlayerLocomotionState::idle &&
              controller.aim() == sf::game::PlayerAimState::first_person &&
              root_is_fixed(controller.state()) &&
              controller.state().yaw == spawn.yaw &&
              movement.attempts == attempts_before_aim,
          "First-person WASD moved the root or body heading");

  controller.reset(spawn);
  controller.update(sf::game::PlayerInput{.turn = 1.0}, movement);
  const auto expected_turn_camera = sf::game::ChaseCamera{}.follow(
      controller.state().x, controller.state().y, controller.state().z,
      controller.state().yaw);
  require(std::abs(controller.camera().x - expected_turn_camera.x) < 0.0001 &&
              std::abs(controller.camera().z - expected_turn_camera.z) <
                  0.0001 &&
              controller.cameraIntent().heading == controller.state().yaw,
          "Chase camera did not follow Gabe's turn immediately");
  controller.reset(spawn);

  controller.advanceAnimationClock();
  require(controller.animationTick() == 1U,
          "Player animation clock did not advance with the native 20 Hz "
          "simulation");

  controller.update(
      sf::game::PlayerInput{
          .move = 1.0,
          .turn = 2.0,
          .run = true,
          .aim = true,
          .next_weapon = true,
      },
      movement);
  require(controller.state().yaw == spawn.yaw &&
              controller.locomotion() ==
                  sf::game::PlayerLocomotionState::idle &&
              controller.aim() == sf::game::PlayerAimState::first_person &&
              controller.weaponSwitch() ==
                  sf::game::PlayerWeaponSwitchState::next &&
              controller.action() ==
                  sf::game::PlayerActionState::weapon_switching &&
              controller.cameraIntent().mode ==
                  sf::game::PlayerCameraMode::first_person_aim,
          "First-person root lock or weapon-switch state was not retained");

  controller.update(
      sf::game::PlayerInput{
          .next_weapon = true,
          .previous_weapon = true,
      },
      movement);
  require(controller.weaponSwitch() ==
                  sf::game::PlayerWeaponSwitchState::next &&
              controller.locomotion() == sf::game::PlayerLocomotionState::idle,
          "Conflicting weapon input cancelled an active draw animation");

  controller.reset(spawn);
  controller.update(sf::game::PlayerInput{.weapon_menu_delta = -1}, movement);
  require(controller.weaponSwitch() ==
                  sf::game::PlayerWeaponSwitchState::previous &&
              controller.action() ==
                  sf::game::PlayerActionState::weapon_switching,
          "Weapon-menu selection did not start Gabe's native draw animation");

  controller.reset(spawn);
  controller.update(
      sf::game::PlayerInput{
          .aim = true,
          .strafe = 1.0,
          .look_yaw = 1024.0,
          .look_pitch = 1000.0,
          .fire_pressed = true,
      },
      movement);
  const auto aimed_strafe_root = controller.state();
  require(controller.locomotion() == sf::game::PlayerLocomotionState::idle &&
              controller.aimHeading() == 1024 &&
              controller.state().yaw == spawn.yaw &&
              root_is_fixed(controller.state()) &&
              controller.action() == sf::game::PlayerActionState::firing &&
              controller.cameraIntent().pitch == 1000.0 &&
              controller.camera().x > controller.state().x,
          "Mouse aim lost yaw/pitch/fire or moved the first-person root");

  controller.update(
      sf::game::PlayerInput{
          .aim = true,
          .look_yaw = -2048.0,
          .look_pitch = -2000.0,
      },
      movement);
  require(controller.aimHeading() == 3072 &&
              controller.state().x == aimed_strafe_root.x &&
              controller.state().y == aimed_strafe_root.y &&
              controller.state().z == aimed_strafe_root.z &&
              controller.state().yaw == spawn.yaw &&
              controller.state().grounded &&
              controller.cameraIntent().heading == controller.aimHeading() &&
              controller.cameraIntent().pitch == -1000.0 &&
              controller.camera().x < controller.state().x,
          "Reverse mouse aim moved the root without locomotion input");

  controller.update({}, movement);
  require(controller.aim() == sf::game::PlayerAimState::chase &&
              controller.state().x == aimed_strafe_root.x &&
              controller.state().y == aimed_strafe_root.y &&
              controller.state().z == aimed_strafe_root.z &&
              controller.state().yaw == spawn.yaw &&
              controller.state().grounded,
          "Releasing first-person aim discarded its locomotion root");

  controller.update(
      sf::game::PlayerInput{
          .roll = true,
          .direct_weapon = std::uint8_t{7},
      },
      movement);
  require(controller.action() == sf::game::PlayerActionState::rolling &&
              controller.weaponSwitch() ==
                  sf::game::PlayerWeaponSwitchState::direct &&
              controller.directWeapon() ==
                  std::optional<std::uint8_t>{std::uint8_t{7}} &&
              controller.cameraIntent().pitch == 0.0,
          "Player roll/direct-weapon state mismatch");
  controller.update({}, movement);
  require(controller.action() == sf::game::PlayerActionState::rolling &&
              controller.actorMotion() == sf::game::ActorMotion::roll,
          "Player roll did not persist for its full-body animation");

  struct RollHeadingCase {
    double move{};
    double strafe{};
    sf::game::PlayerRollDirection direction{};
    std::int32_t heading{};
  };
  constexpr std::array roll_heading_cases{
      RollHeadingCase{1.0, 0.0, sf::game::PlayerRollDirection::forward, 512},
      RollHeadingCase{-1.0, 0.0, sf::game::PlayerRollDirection::forward, 512},
      RollHeadingCase{0.0, -1.0, sf::game::PlayerRollDirection::left, 3584},
      RollHeadingCase{0.0, 1.0, sf::game::PlayerRollDirection::right, 1536},
  };
  for (const auto &roll_case : roll_heading_cases) {
    const sf::game::PlayerState roll_spawn{100.0, 200.0, 300.0, 512, true};
    controller.reset(roll_spawn);
    controller.update(
        sf::game::PlayerInput{
            .move = roll_case.move,
            .strafe = roll_case.strafe,
            .roll = true,
        },
        movement);
    const auto expected_roll_camera = sf::game::ChaseCamera{}.follow(
        controller.state().x, controller.state().y, controller.state().z,
        roll_case.heading);
    require(
        controller.rollDirection() == roll_case.direction &&
            controller.modelHeading() == roll_case.heading &&
            controller.state().yaw == roll_spawn.yaw &&
            controller.cameraIntent().heading == roll_case.heading &&
            std::abs(controller.camera().x - expected_roll_camera.x) < 0.0001 &&
            std::abs(controller.camera().z - expected_roll_camera.z) < 0.0001,
        "Directional roll did not keep Gabe and chase camera aligned");
  }

  controller.reset(spawn);
  controller.update(sf::game::PlayerInput{.kneel = true}, movement);
  require(controller.stance() == sf::game::PlayerStanceState::kneeling &&
              controller.action() ==
                  sf::game::PlayerActionState::kneeling_down &&
              controller.actorMotion() == sf::game::ActorMotion::kneel_down,
          "Player kneel transition did not start");
  for (unsigned int update = 0U;
       update < sf::game::PlayerController::stance_action_updates; ++update) {
    controller.update({}, movement);
  }
  require(controller.action() == sf::game::PlayerActionState::ready &&
              controller.actorMotion() == sf::game::ActorMotion::kneel,
          "Player did not settle into the kneeling pose");
  controller.update(sf::game::PlayerInput{.move = 1.0}, movement);
  require(controller.locomotion() ==
                  sf::game::PlayerLocomotionState::crouch_walking &&
              controller.actorMotion() == sf::game::ActorMotion::crouch_walk,
          "Kneeling movement did not select crouch walk");

  controller.reset(spawn);
  controller.update(sf::game::PlayerInput{.quick_turn = true}, movement);
  require(controller.state().yaw == 2048 &&
              controller.action() ==
                  sf::game::PlayerActionState::quick_turning &&
              controller.actorMotion() == sf::game::ActorMotion::quick_turn &&
              std::abs(controller.camera().z -
                       sf::game::ChaseCamera{}
                           .follow(controller.state().x, controller.state().y,
                                   controller.state().z, controller.state().yaw)
                           .z) < 0.0001,
          "Player quick-turn transition mismatch");

  controller.reset(spawn);
  movement.allow = false;
  movement.attempts = 0U;
  controller.update(sf::game::PlayerInput{.move = 1.0}, movement);
  require(movement.attempts == 1U && controller.state().x == spawn.x &&
              controller.state().z == spawn.z &&
              controller.locomotion() ==
                  sf::game::PlayerLocomotionState::idle &&
              controller.animationTick() == 0U,
          "Blocked cardinal movement retried a zero-distance fallback");

  movement.allow = true;
  controller.reset(spawn);
  controller.update(sf::game::PlayerInput{.aim = true,
                                          .look_yaw = 256.0,
                                          .fire_pressed = true},
                    movement);
  controller.advanceAnimationClock();
  controller.advanceAnimationClock();
  const auto visual_checkpoint = controller;
  controller.reset(spawn);
  controller = visual_checkpoint;
  require(controller.animationTick() == 2U &&
              controller.actionAnimationTick() == 2U &&
              controller.action() == sf::game::PlayerActionState::firing &&
              controller.aim() == sf::game::PlayerAimState::first_person &&
              controller.aimHeading() == 256,
          "Player checkpoint copy lost its visual/action clocks");
}

void testPlayerRootMotionCadence() {
  constexpr auto walking = [] {
    std::array<sf::assets::HmdRootMotionFrame, 25> result{};
    for (auto &frame : result) {
      frame.z = 6;
    }
    for (std::size_t index = 0; index < 9U; ++index) {
      result[index].z = 7;
    }
    return result;
  }();
  constexpr auto running = [] {
    std::array<sf::assets::HmdRootMotionFrame, 14> result{};
    for (auto &frame : result) {
      frame.z = 22;
    }
    result.back().z = 21;
    return result;
  }();

  sf::game::PlayerController controller;
  controller.setRootMotionTracks(walking, running);
  TestPlayerMovement movement;
  constexpr sf::game::PlayerState spawn{0.0, 0.0, 0.0, 0, true};
  controller.reset(spawn);

  double expected_z = 0.0;
  for (std::size_t frame = 0; frame < walking.size(); ++frame) {
    for (unsigned int update = 0U;
         update < sf::game::PlayerController::updates_per_animation_frame;
         ++update) {
      require(
          controller.animationTick() == frame,
          "Walk root motion did not match the current 20 Hz simulation frame");
      controller.update(sf::game::PlayerInput{.move = 1.0}, movement);
      expected_z +=
          static_cast<double>(walking[frame].z) /
          static_cast<double>(
              sf::game::PlayerController::updates_per_animation_frame);
      require(std::abs(controller.state().z - expected_z) < 0.0001,
              "Walk root motion did not use the current native frame exactly");
      controller.advanceAnimationClock();
    }
  }
  require(std::abs(controller.state().z - 159.0) < 0.0001 &&
              controller.animationTick() == walking.size(),
          "WK0 root motion did not travel 159 units over 25 native frames");

  controller.reset(spawn);
  expected_z = 0.0;
  for (std::size_t frame = 0; frame < running.size(); ++frame) {
    for (unsigned int update = 0U;
         update < sf::game::PlayerController::updates_per_animation_frame;
         ++update) {
      require(
          controller.animationTick() == frame,
          "Run root motion did not match the current 20 Hz simulation frame");
      controller.update(sf::game::PlayerInput{.move = 1.0, .run = true},
                        movement);
      expected_z +=
          static_cast<double>(running[frame].z) /
          static_cast<double>(
              sf::game::PlayerController::updates_per_animation_frame);
      require(controller.locomotion() ==
                      sf::game::PlayerLocomotionState::running &&
                  std::abs(controller.state().z - expected_z) < 0.0001,
              "Run root motion did not use the current native frame exactly");
      controller.advanceAnimationClock();
    }
  }
  require(std::abs(controller.state().z - 307.0) < 0.0001 &&
              controller.animationTick() == running.size(),
          "RN0 root motion did not travel 307 units over 14 native frames");
}

void testPlayerPersistentActions() {
  constexpr sf::game::PlayerState spawn{10.0, 20.0, 30.0, 0, true};
  TestPlayerMovement movement;

  const auto require_persistent =
      [&movement, &spawn](sf::game::PlayerInput trigger,
                          sf::game::PlayerActionState expected,
                          unsigned int duration, const char *early_message,
                          const char *late_message) {
        sf::game::PlayerController controller;
        controller.reset(spawn);
        controller.update(trigger, movement);
        require(controller.action() == expected, early_message);
        for (unsigned int update = 1U; update < duration; ++update) {
          controller.update({}, movement);
          require(controller.action() == expected, early_message);
        }
        controller.update({}, movement);
        require(controller.action() == sf::game::PlayerActionState::ready,
                late_message);
      };

  require_persistent(
      sf::game::PlayerInput{.roll = true}, sf::game::PlayerActionState::rolling,
      sf::game::PlayerController::minimum_roll_updates,
      "Roll ended before its native full-body action duration",
      "Roll remained locked after its native full-body action duration");
  require_persistent(sf::game::PlayerInput{.reload = true},
                     sf::game::PlayerActionState::reloading,
                     sf::game::PlayerController::reload_action_updates,
                     "Reload animation did not persist after its input pulse",
                     "Reload animation remained active beyond its duration");
  require_persistent(
      sf::game::PlayerInput{.next_weapon = true},
      sf::game::PlayerActionState::weapon_switching,
      sf::game::PlayerController::weapon_switch_action_updates,
      "Weapon-switch animation did not persist after its input pulse",
      "Weapon-switch animation remained active beyond its duration");
  require_persistent(sf::game::PlayerInput{.kneel = true},
                     sf::game::PlayerActionState::kneeling_down,
                     sf::game::PlayerController::stance_action_updates,
                     "Kneel transition ended before its full-body duration",
                     "Kneel transition remained locked beyond its duration");
  require_persistent(sf::game::PlayerInput{.quick_turn = true},
                     sf::game::PlayerActionState::quick_turning,
                     sf::game::PlayerController::quick_turn_action_updates,
                     "Quick turn ended before its full-body duration",
                     "Quick turn remained locked beyond its duration");

  sf::game::PlayerController controller;
  controller.reset(spawn);
  controller.update(sf::game::PlayerInput{.reload = true}, movement);
  require(controller.actionAnimationTick() == 0U,
          "Reload action clock did not start at native frame zero");
  controller.advanceAnimationClock();
  require(controller.actionAnimationTick() == 1U,
          "Action clock did not advance with the native 20 Hz simulation");

  controller.reset(spawn);
  controller.update(
      sf::game::PlayerInput{
          .next_weapon = true,
          .fire_pressed = true,
          .roll = true,
          .reload = true,
      },
      movement);
  require(controller.action() == sf::game::PlayerActionState::rolling &&
              controller.weaponSwitch() ==
                  sf::game::PlayerWeaponSwitchState::next,
          "Full-body roll did not win the simultaneous action conflict");
  for (unsigned int update = 1U;
       update < sf::game::PlayerController::minimum_roll_updates; ++update) {
    controller.update({}, movement);
    require(controller.action() == sf::game::PlayerActionState::rolling,
            "Queued weapon switch interrupted a full-body roll");
  }
  controller.update({}, movement);
  require(
      controller.action() == sf::game::PlayerActionState::weapon_switching &&
          controller.weaponSwitch() == sf::game::PlayerWeaponSwitchState::next,
      "Weapon switch queued during roll did not start after the roll");

  controller.reset(spawn);
  controller.update(
      sf::game::PlayerInput{
          .next_weapon = true,
          .reload = true,
      },
      movement);
  require(controller.action() == sf::game::PlayerActionState::reloading &&
              controller.weaponSwitch() ==
                  sf::game::PlayerWeaponSwitchState::next,
          "Reload did not precede a simultaneous queued weapon switch");
  for (unsigned int update = 1U;
       update < sf::game::PlayerController::reload_action_updates; ++update) {
    controller.update({}, movement);
    require(controller.action() == sf::game::PlayerActionState::reloading,
            "Queued weapon switch interrupted reload");
  }
  controller.update({}, movement);
  require(
      controller.action() == sf::game::PlayerActionState::weapon_switching &&
          controller.weaponSwitch() == sf::game::PlayerWeaponSwitchState::next,
      "Weapon switch queued during reload did not start after reload");
}

void testPolygonClipper() {
  struct Vertex {
    double depth;
    double uv;
    double color;
  };
  const auto interpolate = [](const Vertex &first, const Vertex &second,
                              double amount) {
    return Vertex{
        std::lerp(first.depth, second.depth, amount),
        std::lerp(first.uv, second.uv, amount),
        std::lerp(first.color, second.color, amount),
    };
  };
  const auto clip = [&interpolate](std::span<const Vertex> input) {
    return sf::core::clipConvexPolygon<Vertex, 4>(
        input, [](const Vertex &vertex) { return vertex.depth; }, interpolate);
  };

  const std::array inside{
      Vertex{1.0, 0.0, 0.0},
      Vertex{2.0, 10.0, 20.0},
      Vertex{3.0, 20.0, 40.0},
  };
  const std::array outside{
      Vertex{-3.0, 0.0, 0.0},
      Vertex{-2.0, 10.0, 20.0},
      Vertex{-1.0, 20.0, 40.0},
  };
  require(clip(inside).count == 3U, "Inside polygon was altered by clipping");
  require(clip(outside).count == 0U, "Outside polygon survived clipping");

  const std::array one_inside{
      Vertex{-2.0, 0.0, 0.0},
      Vertex{-1.0, 10.0, 20.0},
      Vertex{2.0, 20.0, 40.0},
  };
  const auto clipped_one = clip(one_inside);
  require(clipped_one.count == 3U,
          "One-inside triangle did not remain a triangle");
  require(std::abs(clipped_one.vertices[0].depth) < 0.0001 &&
              std::abs(clipped_one.vertices[1].depth) < 0.0001,
          "Clipped vertices are not on the plane");
  require(std::abs(clipped_one.vertices[0].uv - 10.0) < 0.0001 &&
              std::abs(clipped_one.vertices[0].color - 20.0) < 0.0001,
          "Clipped attributes were not interpolated");

  const std::array two_inside{
      Vertex{-2.0, 0.0, 0.0},
      Vertex{2.0, 10.0, 20.0},
      Vertex{3.0, 20.0, 40.0},
  };
  require(clip(two_inside).count == 4U,
          "Two-inside triangle did not become a quad");

  const std::array on_plane{
      Vertex{0.0, 0.0, 0.0},
      Vertex{0.0, 10.0, 20.0},
      Vertex{1.0, 20.0, 40.0},
  };
  require(clip(on_plane).count == 3U, "On-plane edge was lost during clipping");

  const std::array one_on_plane{
      Vertex{0.0, 0.0, 0.0},
      Vertex{1.0, 10.0, 20.0},
      Vertex{2.0, 20.0, 40.0},
  };
  require(clip(one_on_plane).count == 3U,
          "On-plane vertex was duplicated during clipping");

  const std::array all_on_plane{
      Vertex{0.0, 0.0, 0.0},
      Vertex{0.0, 10.0, 20.0},
      Vertex{0.0, 20.0, 40.0},
  };
  require(clip(all_on_plane).count == 3U,
          "Polygon on the clipping plane was discarded");

  const std::array crossing_on_plane{
      Vertex{-1.0, 0.0, 0.0},
      Vertex{0.0, 10.0, 20.0},
      Vertex{1.0, 20.0, 40.0},
  };
  const auto clipped_crossing = clip(crossing_on_plane);
  require(clipped_crossing.count == 3U,
          "Crossing triangle duplicated its on-plane vertex");
  require(std::abs(clipped_crossing.vertices[0].depth) < 0.0001 &&
              std::abs(clipped_crossing.vertices[0].uv - 10.0) < 0.0001,
          "Crossing intersection attributes are invalid");

  const std::array quad_one_outside{
      Vertex{-1.0, 0.0, 0.0},
      Vertex{1.0, 10.0, 20.0},
      Vertex{1.0, 20.0, 40.0},
      Vertex{1.0, 30.0, 60.0},
  };
  const auto clipped_quad = sf::core::clipConvexPolygon<Vertex, 5>(
      quad_one_outside, [](const Vertex &vertex) { return vertex.depth; },
      interpolate);
  require(
      clipped_quad.count == 5U,
      "One-outside billboard quad did not retain a complete clipped perimeter");
  require(std::ranges::all_of(
              std::span{clipped_quad.vertices}.first(clipped_quad.count),
              [](const Vertex &vertex) { return vertex.depth >= -0.0001; }),
          "Clipped billboard quad retained an outside vertex");

  // EMD wall quads use PSX strip order 0,1,2 / 1,3,2. A camera moving
  // alongside a wall can put its left edge behind the near plane; both
  // clipped halves must still tile the complete visible rectangle.
  const std::array wall{
      Vertex{-1.0, 0.0, 0.0},
      Vertex{1.0, 2.0, 0.0},
      Vertex{-1.0, 0.0, 2.0},
      Vertex{1.0, 2.0, 2.0},
  };
  const std::array wall_first{wall[0], wall[1], wall[2]};
  const std::array wall_second{wall[1], wall[3], wall[2]};
  const auto clipped_wall_first = clip(wall_first);
  const auto clipped_wall_second = clip(wall_second);
  const auto area = [](const auto &polygon) {
    auto twice_area = 0.0;
    for (std::size_t index = 0; index < polygon.count; ++index) {
      const auto next = (index + 1U) % polygon.count;
      twice_area += polygon.vertices[index].uv * polygon.vertices[next].color -
                    polygon.vertices[next].uv * polygon.vertices[index].color;
    }
    return std::abs(twice_area) * 0.5;
  };
  require(clipped_wall_first.count >= 3U && clipped_wall_second.count >= 3U,
          "Near-plane clipping removed one half of a wall rectangle");
  require(std::abs(area(clipped_wall_first) + area(clipped_wall_second) - 2.0) <
              0.0001,
          "Near-plane clipping left a rectangular hole in a wall");
}

void testLevelLayout() {
  std::vector<std::byte> bytes(0x90 + 3U * 15U, std::byte{0x5b});
  writeLe32(bytes, 0x88, 3);
  writeLe32(bytes, 0x8c, 1);
  bytes[0x78] = std::byte{0};
  bytes[0x79] = std::byte{2};
  bytes[0x7a] = std::byte{0xff};
  bytes[0x90] = std::byte{1};
  bytes[0x91] = std::byte{0xfe};
  bytes[0x92] = std::byte{2};
  bytes[0x93] = std::byte{0xfe};
  bytes[0x94] = std::byte{0};
  bytes[0x95] = std::byte{0xff};
  bytes[0x9f] = std::byte{0};
  bytes[0xa0] = std::byte{2};
  bytes[0xa1] = std::byte{0xff};
  bytes[0xae] = std::byte{1};
  bytes[0xaf] = std::byte{0xff};

  const auto layout = sf::assets::LevelLayout::parse(bytes, 3);
  require(layout.modelCount() == 3 && layout.initialRoom() == 1,
          "Level-layout header mismatch");
  require(layout.residentModels().size() == 2 &&
              layout.residentModels()[1] == 2,
          "Level resident models mismatch");
  require(layout.visibility(0).active_models == std::vector<std::uint16_t>{1} &&
              layout.visibility(0).prefetched_models ==
                  std::vector<std::uint16_t>({2, 0}),
          "Level visibility split mismatch");
  require(layout.visibility(1).active_models ==
              std::vector<std::uint16_t>({0, 2}),
          "Level visibility list mismatch");
}

void testMissionObjects() {
  constexpr std::size_t name_offset = 0x30;
  constexpr std::size_t empty_name_offset = 0x39;
  constexpr std::size_t definitions_offset = 0x40;
  constexpr std::size_t rooms_offset = 0x58;
  constexpr std::size_t room_indices_offset = 0x60;
  constexpr std::size_t table_offset = 0x68;
  constexpr std::size_t object_size = 0x4c;
  constexpr std::size_t path_offset = table_offset + 2U * object_size;
  std::vector<std::byte> bytes(path_offset + 2U * 12U);
  constexpr std::string_view name{"GABE.HMD\0", 9};
  std::ranges::transform(name, bytes.begin() + name_offset, [](char value) {
    return static_cast<std::byte>(value);
  });
  bytes[empty_name_offset] = std::byte{0};
  writeLe32(bytes, 0x04, 1);
  writeLe32(bytes, 0x08, 2);
  writeLe32(bytes, 0x0c, 1);
  writeLe32(bytes, 0x10, definitions_offset);
  writeLe32(bytes, 0x14, table_offset);
  writeLe32(bytes, 0x18, rooms_offset);
  writeLe32(bytes, 0x1c, 1);
  writeLe32(bytes, definitions_offset, 0x73U);
  writeLe32(bytes, definitions_offset + 4, name_offset);
  writeLe32(bytes, definitions_offset + 0x0c, empty_name_offset);
  writeLe32(bytes, rooms_offset, 2);
  writeLe32(bytes, rooms_offset + 4, room_indices_offset);
  writeLe32(bytes, room_indices_offset, 0);
  writeLe32(bytes, room_indices_offset + 4, 1);
  writeLe32(bytes, table_offset + 0x2c, path_offset);
  writeLe32(bytes, table_offset + 0x30, 1U);
  writeLe16(bytes, path_offset, 100);
  writeLe16(bytes, path_offset + 2U, 200);
  writeLe16(bytes, path_offset + 4U, 300);
  bytes[path_offset + 8U] = std::byte{1};
  bytes[path_offset + 11U] = std::byte{0xca};
  writeLe16(bytes, path_offset + 12U, 400);
  writeLe16(bytes, path_offset + 14U, 500);
  writeLe16(bytes, path_offset + 16U, 600);
  bytes[path_offset + 20U] = std::byte{0xff};
  bytes[path_offset + 23U] = std::byte{0xca};
  const auto player_offset = table_offset + object_size;
  writeLe32(bytes, player_offset, 0);
  const std::array<std::int16_t, 9> rotation{-111, 0,    -4096, 0,   4096,
                                             0,    4096, 0,     -111};
  for (std::size_t index = 0; index < rotation.size(); ++index) {
    writeLe16(bytes, player_offset + 4U + index * 2U,
              static_cast<std::uint16_t>(rotation[index]));
  }
  writeLe32(bytes, player_offset + 0x18, 4780);
  writeLe32(bytes, player_offset + 0x1c, 2133);
  writeLe32(bytes, player_offset + 0x20, 2825);

  const auto objects = sf::assets::MissionObjects::parse(bytes);
  require(objects.objects().size() == 2 && objects.playerIndex() == 1,
          "Mission-object header mismatch");
  const auto room_objects = objects.objectsInRoom(0);
  const auto object_rooms = objects.roomsContainingObject(0);
  require(objects.definitions().size() == 1 &&
              objects.definition(0).primary_model == "GABE.HMD" &&
              room_objects.size() == 2 && room_objects[0] == 0 &&
              room_objects[1] == 1 && object_rooms.size() == 1 &&
              object_rooms[0] == 0,
          "Mission-object definitions mismatch");
  require(objects.player().type == 0 && objects.player().transform.x == 4780 &&
              objects.player().transform.y == 2133 &&
              objects.player().transform.z == 2825,
          "Mission player transform mismatch");
  require(objects.player().transform.rotation == rotation,
          "Mission player rotation mismatch");
  require(objects.objects()[0].patrol_path.size() == 2U &&
              objects.objects()[0].patrol_path[1].x == 400 &&
              objects.objects()[0].patrol_path[1].z == 600 &&
              objects.objects()[0].linked_object == 1,
          "Mission transition path was not decoded from its native node links");
}

void testInvalidAssets() {
  std::vector<std::byte> hog(24);
  writeLe32(hog, 4, 1);
  writeLe32(hog, 12, 24);
  writeLe32(hog, 16, 24);
  try {
    static_cast<void>(sf::assets::HogArchive::parse(std::move(hog)));
    throw std::runtime_error{"Invalid HOG was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_format,
            "Invalid HOG returned the wrong error code");
  }

  std::vector<std::byte> tim(20);
  writeLe32(tim, 0, 0x10);
  writeLe32(tim, 4, 0x09);
  try {
    static_cast<void>(sf::assets::TimImage::parse(tim));
    throw std::runtime_error{"Invalid TIM was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_format,
            "Invalid TIM returned the wrong error code");
  }
}

void testSha256() {
  require(
      sf::core::toHex(sf::core::sha256({})) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "Empty SHA-256 test vector failed");
  constexpr std::array input{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  require(
      sf::core::toHex(sf::core::sha256(input)) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "SHA-256 test vector failed");
}

void testInvalidExecutable() {
  std::vector<std::byte> file(2048);
  try {
    static_cast<void>(sf::psx::Executable::parse(file));
    throw std::runtime_error{"Invalid PS-X EXE was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_format,
            "Invalid PS-X EXE returned the wrong error code");
  }
}

void testExecutable() {
  std::vector<std::byte> file(4096);
  constexpr std::string_view signature = "PS-X EXE";
  std::ranges::transform(signature, file.begin(), [](char value) {
    return static_cast<std::byte>(value);
  });
  writeLe32(file, 0x10, 0x80010020U);
  writeLe32(file, 0x18, 0x80010000U);
  writeLe32(file, 0x1c, 2048U);
  writeLe32(file, 0x30, 0x801ffff0U);

  const auto executable = sf::psx::Executable::parse(file);
  require(executable.header().initial_pc == 0x80010020U,
          "PS-X EXE entry point mismatch");
  require(executable.text().size() == 2048, "PS-X EXE text size mismatch");
}

void testCueSheet() {
  const auto directory =
      std::filesystem::temp_directory_path() / "sf_port_tests";
  std::filesystem::create_directories(directory);
  const auto binary_path = directory / "test image.bin";
  const auto cue_path = directory / "test image.cue";
  {
    std::ofstream binary{binary_path, std::ios::binary | std::ios::trunc};
    binary.put('\0');
  }
  {
    std::ofstream cue{cue_path, std::ios::trunc};
    cue << "FILE \"test image.bin\" BINARY\n"
           "  TRACK 01 MODE2/2352\n"
           "    INDEX 01 00:02:00\n";
  }

  const auto cue = sf::disc::CueSheet::load(cue_path);
  require(cue.dataTrack().sectorSize() == 2352, "CUE sector size mismatch");
  require(cue.dataTrack().userDataOffset() == 24, "CUE data offset mismatch");
  require(cue.dataTrack().index_lba == 150, "CUE index mismatch");
  std::filesystem::remove_all(directory);
}

void testRawSectorFile() {
  constexpr std::size_t sector_size = 2352;
  constexpr std::size_t user_offset = 24;
  constexpr std::size_t sector_count = 21;
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() /
                         ("sf_raw_sector_tests_" + std::to_string(nonce));
  std::filesystem::create_directories(directory);
  const auto binary_path = directory / "raw.bin";
  const auto cue_path = directory / "raw.cue";
  std::vector<std::byte> image(sector_count * sector_size);

  auto sectorUserData = [&](std::size_t lba) {
    return std::span{image}.subspan(lba * sector_size + user_offset, 2048);
  };
  auto descriptor = sectorUserData(16);
  descriptor[0] = std::byte{1};
  constexpr std::string_view magic{"CD001"};
  std::ranges::transform(magic, descriptor.begin() + 1, [](char value) {
    return static_cast<std::byte>(value);
  });
  descriptor[6] = std::byte{1};
  std::fill(descriptor.begin() + 40, descriptor.begin() + 72, std::byte{' '});
  descriptor[156] = std::byte{34};
  writeLe32(descriptor, 158, 18);
  writeLe32(descriptor, 166, 2048);
  descriptor[181] = std::byte{2};
  descriptor[188] = std::byte{1};

  auto root = sectorUserData(18);
  constexpr std::string_view filename{"MOVIE.STR;1"};
  root[0] = std::byte{44};
  writeLe32(root, 2, 19);
  writeLe32(root, 10, 3000);
  root[25] = std::byte{0};
  root[32] = static_cast<std::byte>(filename.size());
  std::ranges::transform(filename, root.begin() + 33, [](char value) {
    return static_cast<std::byte>(value);
  });
  std::fill_n(image.begin() + 19 * sector_size, sector_size, std::byte{0xa5});
  std::fill_n(image.begin() + 20 * sector_size, sector_size, std::byte{0x5a});

  {
    std::ofstream binary{binary_path, std::ios::binary | std::ios::trunc};
    binary.write(reinterpret_cast<const char *>(image.data()),
                 static_cast<std::streamsize>(image.size()));
  }
  {
    std::ofstream cue{cue_path, std::ios::trunc};
    cue << "FILE \"raw.bin\" BINARY\n"
           "  TRACK 01 MODE2/2352\n"
           "    INDEX 01 00:00:00\n";
  }

  {
    auto disc = sf::disc::Iso9660Image::open(cue_path);
    const auto raw = disc.readRawSectorFile("movie.str");
    require(raw.sector_size == sector_size, "Raw sector size mismatch");
    require(raw.sector_count == 2, "Raw sector count mismatch");
    require(raw.bytes.size() == 2 * sector_size,
            "Raw extent byte count mismatch");
    require(std::ranges::all_of(
                raw.bytes.begin(), raw.bytes.begin() + sector_size,
                [](std::byte value) { return value == std::byte{0xa5}; }),
            "First raw sector payload mismatch");
    require(std::ranges::all_of(
                raw.bytes.begin() + sector_size, raw.bytes.end(),
                [](std::byte value) { return value == std::byte{0x5a}; }),
            "Second raw sector payload mismatch");
  }
  std::filesystem::remove_all(directory);
}

void testFunctionMap() {
  constexpr std::uint32_t base = 0x80010000U;
  constexpr std::uint32_t target = base + 0x20U;
  std::array<std::byte, 64> text{};
  const auto jal = 0x0C000000U | ((target >> 2U) & 0x03FFFFFFU);
  writeLe32(text, 0, jal);
  writeLe32(text, 4, jal);

  const auto functions = sf::psx::discoverFunctionCandidates(text, base, base);
  require(functions.size() == 2, "Function seed count mismatch");
  require(functions[0].address == base && functions[0].static_call_count == 0,
          "Entry-point seed mismatch");
  require(functions[1].address == target && functions[1].static_call_count == 2,
          "JAL target seed mismatch");
}

class RecordingStateSink final : public sf::game::StateTransitionSink {
public:
  void changeState(sf::game::SystemState state, bool entering) override {
    transitions.emplace_back(state, entering);
  }

  std::vector<std::pair<sf::game::SystemState, bool>> transitions;
};

void testStateStack() {
  RecordingStateSink sink;
  sf::game::StateStack states{2, sink};
  require(states.current() == 2 && states.depth() == 1,
          "Initial system state mismatch");

  states.push(4);
  states.push(7);
  require(states.current() == 7 && states.depth() == 3,
          "System state push mismatch");
  states.pop();
  require(states.current() == 4 && states.depth() == 2,
          "System state pop mismatch");
  require(sink.transitions.size() == 3,
          "System state transition count mismatch");
  require(sink.transitions[0] ==
              std::pair<sf::game::SystemState, bool>{4, true},
          "Push transition mismatch");
  require(sink.transitions[2] ==
              std::pair<sf::game::SystemState, bool>{4, false},
          "Pop transition mismatch");
}

void testStateStackBounds() {
  RecordingStateSink sink;
  sf::game::StateStack states{0, sink};
  try {
    states.pop();
    throw std::runtime_error{"State-stack underflow was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_argument,
            "State-stack underflow returned the wrong error code");
  }

  for (std::size_t index = 1; index < sf::game::StateStack::capacity; ++index) {
    states.push(static_cast<sf::game::SystemState>(index));
  }
  try {
    states.push(99);
    throw std::runtime_error{"State-stack overflow was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_argument,
            "State-stack overflow returned the wrong error code");
  }
}

class RecordingSystemServices final : public sf::game::SystemServices {
public:
  void resetCallbacks() override { calls.push_back(1); }
  void setVideoMode(sf::game::VideoMode mode) override {
    require(mode == sf::game::VideoMode::ntsc,
            "System selected the wrong video mode");
    calls.push_back(2);
  }
  void runStateMachine() override { calls.push_back(3); }

  std::vector<int> calls;
};

void testSystemBootOrder() {
  RecordingSystemServices services;
  require(sf::game::runSystem(services) == 0, "System main returned an error");
  require(services.calls == std::vector<int>({1, 2, 3}),
          "System boot order mismatch");
}

void testPlayerInventory() {
  constexpr std::array<std::string_view, sf::game::weapon_slot_count>
      expected_names{
          "No Weapon",
          "Silenced 9mm",
          "9mm",
          ".357",
          ".45",
          "G-18",
          "Combat Shotgun",
          "Shotgun",
          "PK-102",
          "M-16",
          "BIZ-2",
          "HK-5",
          "Nightvision Rifle",
          "Sniper Rifle",
          "Taser",
          "Flamethrower",
          "M-79",
          "K3G4",
          "Virus Scanner",
          "Grenade",
          "Gas Grenade",
          "Flashlight",
          "Chopper Gun",
          "Keycard",
          "C4 Explosives",
          "Viral Antigen",
      };
  for (std::size_t index = 0U; index < expected_names.size(); ++index) {
    const auto id = static_cast<sf::game::WeaponId>(index);
    const auto *definition = sf::game::tryWeaponDefinition(id);
    require(definition != nullptr && definition->id == id &&
                definition->name == expected_names[index],
            "Original weapon ID/name table mismatch");
  }
  require(static_cast<std::uint8_t>(sf::game::WeaponId::virus_scanner) == 18U &&
              static_cast<std::uint8_t>(sf::game::WeaponId::flashlight) == 21U,
          "Scanner/flashlight weapon slots were swapped");

  const auto combat_shotgun_layers =
      sf::game::weaponDefinition(sf::game::WeaponId::combat_shotgun)
          .icon.layers();
  require(
      combat_shotgun_layers.size() == 3U &&
          combat_shotgun_layers[0] == "SHOT1A.TIM" &&
          combat_shotgun_layers[1] == "SHOT1B.TIM" &&
          combat_shotgun_layers[2] == "SHOT1C.TIM",
      "Weapon A/B/C images were not preserved as ordered horizontal layers");
  const auto scanner_layers =
      sf::game::weaponDefinition(sf::game::WeaponId::virus_scanner)
          .icon.layers();
  const auto flashlight_layers =
      sf::game::weaponDefinition(sf::game::WeaponId::flashlight).icon.layers();
  require(scanner_layers.size() == 1U && scanner_layers[0] == "SNIFFER.TIM" &&
              flashlight_layers.size() == 2U &&
              flashlight_layers[0] == "FLASHLTA.TIM" &&
              flashlight_layers[1] == "FLASHLTB.TIM",
          "Scanner/flashlight HUD layer mapping mismatch");
  const auto pistol_pickup = sf::game::droppedItemIconLayers(
      static_cast<std::uint16_t>(sf::game::WeaponId::pistol_9mm));
  require(pistol_pickup.size() == 2U && pistol_pickup[0] == "PISTOL2A.TIM" &&
              pistol_pickup[1] == "PISTOL2B.TIM",
          "Glock 17 floor pickup must use its authored interface sprite");
  const auto flamethrower_pickup = sf::game::droppedItemIconLayers(
      static_cast<std::uint16_t>(sf::game::WeaponId::flamethrower));
  require(flamethrower_pickup.size() == 2U &&
              flamethrower_pickup[0] == "FLAKA.TIM" &&
              flamethrower_pickup[1] == "FLAKB.TIM",
          "Flamethrower floor pickup must use its authored interface sprite");
  const auto armor_pickup = sf::game::droppedItemIconLayers(0x80U);
  require(armor_pickup.size() == 1U && armor_pickup[0] == "VEST2.TIM",
          "Armour floor pickup must use the retail SPFX vest sprite");
  constexpr std::array visible_floor_pickups{
      sf::game::WeaponId::silenced_9mm,
      sf::game::WeaponId::pistol_9mm,
      sf::game::WeaponId::pistol_45,
      sf::game::WeaponId::g_18,
      sf::game::WeaponId::combat_shotgun,
      sf::game::WeaponId::shotgun,
      sf::game::WeaponId::pk_102,
      sf::game::WeaponId::m_16,
      sf::game::WeaponId::biz_2,
      sf::game::WeaponId::hk_5,
      sf::game::WeaponId::nightvision_rifle,
      sf::game::WeaponId::sniper_rifle,
      sf::game::WeaponId::k3g4,
      sf::game::WeaponId::key_card,
  };
  require(std::ranges::all_of(visible_floor_pickups,
                              [](const auto item) {
                                return !sf::game::droppedItemIconLayers(
                                            static_cast<std::uint16_t>(item))
                                            .empty();
                              }),
          "A retail weapon, key-card, or rifle floor pickup has no sprite");

  sf::game::PlayerInventory inventory;
  require(inventory.current() == sf::game::WeaponId::silenced_9mm,
          "First mission selected the wrong weapon");
  require(inventory.state(sf::game::WeaponId::silenced_9mm).magazine == 15U &&
              inventory.state(sf::game::WeaponId::silenced_9mm).reserve == 45U,
          "First mission 9mm ammunition mismatch");
  require(inventory.state(sf::game::WeaponId::taser).owned &&
              inventory.state(sf::game::WeaponId::flashlight).owned &&
              !inventory.state(sf::game::WeaponId::virus_scanner).owned,
          "First mission equipment mismatch");
  require(inventory.weaponMenuWindow() ==
              std::array{
                  sf::game::WeaponId::silenced_9mm,
                  sf::game::WeaponId::taser,
                  sf::game::WeaponId::flashlight,
                  sf::game::WeaponId::silenced_9mm,
                  sf::game::WeaponId::taser,
                  sf::game::WeaponId::flashlight,
                  sf::game::WeaponId::silenced_9mm,
              },
          "Original seven-slot weapon-menu window mismatch");

  constexpr auto invalid_weapon = static_cast<sf::game::WeaponId>(0xffU);
  require(!sf::game::isValidWeaponId(invalid_weapon) &&
              sf::game::tryWeaponDefinition(invalid_weapon) == nullptr &&
              inventory.tryState(invalid_weapon) == nullptr,
          "Invalid weapon ID was accepted by a checked lookup");
  const auto current_before_invalid_mutation = inventory.current();
  inventory.grant(invalid_weapon, 99U, 999U);
  inventory.remove(invalid_weapon);
  require(!inventory.select(invalid_weapon) &&
              inventory.current() == current_before_invalid_mutation,
          "Invalid weapon ID changed inventory state");
  auto definition_threw = false;
  try {
    static_cast<void>(sf::game::weaponDefinition(invalid_weapon));
  } catch (const std::out_of_range &) {
    definition_threw = true;
  }
  require(definition_threw,
          "Strict weapon lookup did not reject an invalid ID");
  auto state_threw = false;
  try {
    static_cast<void>(inventory.state(invalid_weapon));
  } catch (const std::out_of_range &) {
    state_threw = true;
  }
  require(state_threw, "Strict inventory lookup did not reject an invalid ID");

  require(inventory.selectNext() &&
              inventory.current() == sf::game::WeaponId::flashlight,
          "Original next-weapon chain mismatch");
  require(inventory.selectNext() &&
              inventory.current() == sf::game::WeaponId::taser,
          "Original next-weapon wrap mismatch");
  require(inventory.selectNext() &&
              inventory.current() == sf::game::WeaponId::silenced_9mm,
          "Original next-weapon cycle did not return to the 9mm");
  require(inventory.selectPrevious() &&
              inventory.current() == sf::game::WeaponId::taser,
          "Original previous-weapon chain mismatch");
  require(inventory.select(sf::game::WeaponId::silenced_9mm),
          "Owned 9mm could not be selected");

  for (unsigned int round = 0; round < 15U; ++round) {
    require(inventory.consumeRound(), "Loaded 9mm round was rejected");
  }
  require(!inventory.consumeRound(), "Empty 9mm magazine fired a round");
  require(inventory.reload() == 15U, "9mm reload transferred the wrong amount");
  require(inventory.currentState().magazine == 15U &&
              inventory.currentState().reserve == 30U,
          "9mm reload state mismatch");

  inventory.grant(sf::game::WeaponId::silenced_9mm, 99U, 999U);
  require(inventory.currentState().magazine == 15U &&
              inventory.currentState().reserve == 75U,
          "Original 9mm ammunition limits were not enforced");
  const auto silenced_layers =
      sf::game::weaponDefinition(sf::game::WeaponId::silenced_9mm)
          .icon.layers();
  require(silenced_layers.size() == 2U &&
              silenced_layers[0] == "PISTOL1A.TIM" &&
              silenced_layers[1] == "PISTOL1B.TIM",
          "9mm HUD icon mapping mismatch");
}

void testGameplayHud() {
  using sf::game::LegacyUiMessageChannel;
  constexpr auto epsilon = 0.0001;
  const auto interpolate_countdown = sf::game::interpolateHudCountdown;
  require(std::abs(interpolate_countdown(18U, 17U, 0.0) - 18.0) < epsilon &&
              std::abs(interpolate_countdown(18U, 17U, 0.5) - 17.5) < epsilon &&
              std::abs(interpolate_countdown(18U, 17U, 1.0) - 17.0) < epsilon,
          "Weapon HUD countdown no longer interpolates between 20 Hz ticks");
  require(interpolate_countdown(0U, 18U, 0.0) == 18.0 &&
              interpolate_countdown(17U, 18U, 0.5) == 18.0,
          "A newly armed weapon HUD animation was delayed by interpolation");
  require(interpolate_countdown(4U, 3U, -1.0) == 4.0 &&
              interpolate_countdown(4U, 3U, 2.0) == 3.0,
          "Weapon HUD countdown interpolation did not clamp render alpha");

  const auto retail_scope_message = sf::platform::isRetailScopeMessage;
  require(
      retail_scope_message(true, LegacyUiMessageChannel::centered, false, 1U) &&
          retail_scope_message(true, LegacyUiMessageChannel::centered, false,
                               3U),
      "Partial scope packets no longer stay on the retail rendering path");
  require(!retail_scope_message(false, LegacyUiMessageChannel::centered, false,
                                3U) &&
              !retail_scope_message(true, LegacyUiMessageChannel::status, false,
                                    3U) &&
              !retail_scope_message(true, LegacyUiMessageChannel::centered,
                                    true, 3U) &&
              !retail_scope_message(true, LegacyUiMessageChannel::centered,
                                    false, 0U),
          "Non-scope gameplay text was classified as a retail scope packet");
  const auto retail_scope_font = sf::platform::useRetailEnglishScopeFont;
  require(retail_scope_font(true, true, LegacyUiMessageChannel::centered, false,
                            1U) &&
              retail_scope_font(true, true, LegacyUiMessageChannel::centered,
                                false, 3U),
          "Empty or partial scope source no longer selects the retail font");
  require(!retail_scope_font(false, true, LegacyUiMessageChannel::centered,
                             false, 3U) &&
              !retail_scope_font(true, false, LegacyUiMessageChannel::centered,
                                 false, 3U) &&
              !retail_scope_font(true, true, LegacyUiMessageChannel::status,
                                 false, 3U) &&
              !retail_scope_font(true, true, LegacyUiMessageChannel::centered,
                                 true, 3U) &&
              !retail_scope_font(true, true, LegacyUiMessageChannel::centered,
                                 false, 0U),
          "Non-scope gameplay text escaped into the retail font path");

  const auto revealed = sf::platform::gameplayMessageVisibleGlyphCount;
  require(revealed(10U, 0U, 10U) == 0U && revealed(10U, 1U, 10U) == 1U &&
              revealed(10U, 4U, 20U) == 8U && revealed(10U, 10U, 20U) == 20U &&
              revealed(10U, 40U, 20U) == 20U,
          "Gameplay message typewriter mapping no longer follows guest "
          "reveal progress");
  require(revealed(0U, 1U, 20U) == 0U && revealed(10U, 1U, 0U) == 0U,
          "Empty gameplay message reveal inputs were not rejected");

  require(sf::game::originalHudGlyph('!') ==
                  sf::game::OriginalHudGlyph{8U, 24U, 1U} &&
              sf::game::originalHudGlyph('\'') ==
                  sf::game::OriginalHudGlyph{24U, 24U, 1U} &&
              sf::game::originalHudGlyph('\"') ==
                  sf::game::OriginalHudGlyph{24U, 24U, 3U} &&
              sf::game::originalHudGlyph('?') ==
                  sf::game::OriginalHudGlyph{0U, 24U, 4U} &&
              sf::game::originalHudTextWidth("armor") == 32 &&
              sf::game::originalPrimaryStatusLabel(
                  sf::game::PrimaryStatus::armor) == "ARMOR" &&
              sf::game::originalPrimaryStatusLabel(
                  sf::game::PrimaryStatus::health) == "HEALTH",
          "Original gameplay-font UV/advance table mismatch");

  const auto wrapped = sf::game::originalHudWrapText(
      "ALPHA BETA GAMMA", sf::game::originalHudTextWidth("ALPHA BETA"));
  require(wrapped == "ALPHA BETA\nGAMMA",
          "Gameplay notification word wrapping mismatch");
  require(sf::game::originalHudWrapText("ALPHA\nBETA", 1000) == "ALPHA\nBETA",
          "Gameplay notification wrapping lost an explicit newline");
  const auto split_word = sf::game::originalHudWrapText(
      "ABCDEFGHIJ", sf::game::originalHudTextWidth("ABCD"));
  require(split_word.find('\n') != std::string::npos,
          "Overlong gameplay notification token was not split");
  for (auto cursor = std::size_t{}; cursor <= split_word.size();) {
    const auto end = split_word.find('\n', cursor);
    const auto line_end = end == std::string::npos ? split_word.size() : end;
    require(sf::game::originalHudTextWidth(std::string_view{split_word}.substr(
                cursor, line_end - cursor)) <=
                sf::game::originalHudTextWidth("ABCD"),
            "Wrapped gameplay notification exceeds its text column");
    if (end == std::string::npos) {
      break;
    }
    cursor = end + 1U;
  }

  sf::game::setGameLanguage(sf::game::GameLanguage::russian_vit);
  require(sf::game::originalHudGlyph(static_cast<char>(0xdfU)) ==
                  sf::game::OriginalHudGlyph{64U, 24U, 6U} &&
              sf::game::originalHudGlyph(static_cast<char>(0xeaU)) ==
                  sf::game::OriginalHudGlyph{72U, 40U, 5U} &&
              sf::game::originalHudGlyph(static_cast<char>(0xe5U)) ==
                  sf::game::OriginalHudGlyph{72U, 32U, 5U} &&
              sf::game::originalHudGlyph(static_cast<char>(0xf5U)) ==
                  sf::game::OriginalHudGlyph{80U, 56U, 5U},
          "Russian distinct Cyrillic glyph mapping mismatch");
  sf::game::setGameLanguage(sf::game::GameLanguage::english);

  constexpr std::array pistol_icon_widths{32, 32};
  constexpr std::array rifle_icon_widths{24, 24, 24};
  require(sf::game::originalWeaponIconOffsets(pistol_icon_widths) ==
                  std::array<int, sf::game::maximum_weapon_icon_layers>{-32, 0,
                                                                        0} &&
              sf::game::originalWeaponIconOffsets(rifle_icon_widths) ==
                  std::array<int, sf::game::maximum_weapon_icon_layers>{
                      -36, -12, 12},
          "Original centred weapon-icon layout mismatch");

  require(sf::game::originalWeaponMenuGeometry() ==
              sf::game::OriginalWeaponMenuGeometry{
                  -200, -90, 200, -69, -49, -93, 49, -66, -200, 200, -92, -68,
                  sf::game::HudRgb{40U, 48U, 80U},
                  sf::game::HudRgb{128U, 128U, 128U}},
          "Original long-switch weapon-menu backing mismatch");

  require(sf::game::originalAimReticleGeometry(false) ==
                  sf::game::OriginalAimReticleGeometry{17, 8, 17, 9} &&
              sf::game::originalAimReticleGeometry(true) ==
                  sf::game::OriginalAimReticleGeometry{10, 7, 10, 7} &&
              sf::game::originalHeadshotCalloutGeometry() ==
                  sf::game::OriginalHeadshotCalloutGeometry{0, -14, 9, -20, 16,
                                                            -20, 8, -28},
          "Original target reticle/callout geometry mismatch");

  const auto &pistol =
      sf::game::weaponDefinition(sf::game::WeaponId::silenced_9mm);
  require(sf::game::originalAmmoText(
              pistol, sf::game::WeaponState{true, 15U, 45U}) == "15/45" &&
              sf::game::originalAmmoText(
                  pistol, sf::game::WeaponState{true, 120U, 123U}) ==
                  "99/123" &&
              sf::game::originalAmmoText(
                  sf::game::weaponDefinition(sf::game::WeaponId::taser),
                  sf::game::WeaponState{true, 1U, 0U})
                  .empty(),
          "Original ammunition-counter formatting mismatch");

  require(sf::game::originalRadarGeometry(0U) ==
                  sf::game::OriginalRadarGeometry{} &&
              sf::game::originalRadarGeometry(6U) ==
                  sf::game::OriginalRadarGeometry{6U, 12, 10, 4, 3, 0, 0} &&
              sf::game::originalRadarGeometry(12U) ==
                  sf::game::OriginalRadarGeometry{12U, 24, 20, 18, 15, 9, 8} &&
              sf::game::originalRadarGeometry(255U) ==
                  sf::game::OriginalRadarGeometry{12U, 24, 20, 18, 15, 9, 8},
          "Original radar reveal geometry mismatch");

  sf::game::GameplayHud hud;
  require(hud.primaryStatus() == sf::game::PrimaryStatus::armor &&
              hud.primaryBar() == sf::game::GameplayHud::bar_maximum &&
              hud.displayedPrimaryBar() == sf::game::GameplayHud::bar_maximum &&
              hud.primaryReveal() == 0U && hud.revealFrame() == 0U,
          "Initial armor HUD state mismatch");

  hud.setVitals(sf::game::PlayerVitals{
      .health = 90U,
      .maximum_health = 150U,
      .armor = 300U,
      .maximum_armor = 600U,
  });
  require(hud.primaryStatus() == sf::game::PrimaryStatus::armor &&
              hud.primaryBar() == 25U && hud.armorBar() == 25U &&
              hud.healthBar() == 30U &&
              hud.healthBarColor() == sf::game::HudRgb{255U, 100U, 100U},
          "Armor HUD scaling mismatch");
  hud.setDanger(100U);
  hud.setTargetHealth(static_cast<std::uint8_t>(75U));
  require(hud.dangerBar() == 50U && hud.targetBar() == 37U,
          "DANGER/TARGET HUD scaling mismatch");

  hud.update(sf::game::HudInput{.aiming = true, .next_weapon = true});
  require(hud.aiming() &&
              hud.inventory().current() == sf::game::WeaponId::flashlight &&
              hud.weaponSwitchFrames() == 18U,
          "Aiming/weapon-switch HUD state mismatch");
  require(
      hud.displayedPrimaryBar() == 45U && hud.displayedPrimaryTrail() == 45U &&
          hud.displayedDangerBar() == 50U && hud.displayedTargetBar() == 5U &&
          hud.primaryReveal() == 8U && hud.dangerReveal() == 8U &&
          hud.targetReveal() == 8U && hud.revealFrame() == 1U,
      "Original HUD bar/reveal animation step mismatch");

  sf::game::GameplayHud short_switch;
  short_switch.update(sf::game::HudInput{.next_weapon = true});
  require(short_switch.inventory().current() ==
                  sf::game::WeaponId::flashlight &&
              short_switch.weaponSwitchFrames() ==
                  sf::game::GameplayHud::weapon_switch_duration &&
              short_switch.weaponMenuFrames() == 0U,
          "Short weapon switch incorrectly opened the weapon menu");

  sf::game::GameplayHud guest_presentation;
  const auto guest_weapon = guest_presentation.inventory().current();
  guest_presentation.showWeaponMenu();
  require(guest_presentation.inventory().current() == guest_weapon &&
              guest_presentation.weaponMenuFrames() ==
                  sf::game::GameplayHud::weapon_menu_duration &&
              guest_presentation.weaponSwitchFrames() == 0U,
          "Guest weapon-menu presentation mutated the authoritative inventory");
  guest_presentation.notifyWeaponChanged();
  require(
      guest_presentation.inventory().current() == guest_weapon &&
          guest_presentation.weaponSwitchFrames() ==
              sf::game::GameplayHud::weapon_switch_duration &&
          guest_presentation.weaponMenuFrames() ==
              sf::game::GameplayHud::weapon_menu_duration,
      "Guest weapon-change presentation did not preserve menu/inventory state");

  sf::game::GameplayHud weapon_menu;
  weapon_menu.update(sf::game::HudInput{.weapon_menu_delta = -1});
  require(weapon_menu.inventory().current() == sf::game::WeaponId::taser &&
              weapon_menu.weaponSwitchFrames() ==
                  sf::game::GameplayHud::weapon_switch_duration &&
              weapon_menu.weaponMenuFrames() ==
                  sf::game::GameplayHud::weapon_menu_duration &&
              weapon_menu.weaponMenuWindow()[3] == sf::game::WeaponId::taser,
          "Mouse wheel did not open and move the original weapon menu");
  weapon_menu.update(sf::game::HudInput{.weapon_menu_delta = 2});
  require(weapon_menu.inventory().current() == sf::game::WeaponId::flashlight &&
              weapon_menu.weaponMenuFrames() ==
                  sf::game::GameplayHud::weapon_menu_duration &&
              weapon_menu.weaponMenuWindow()[3] ==
                  sf::game::WeaponId::flashlight,
          "Multi-notch wheel input did not advance each available weapon");
  weapon_menu.update({});
  require(weapon_menu.weaponMenuFrames() ==
              sf::game::GameplayHud::weapon_menu_duration - 1U,
          "Weapon menu did not close on its deterministic timer");

  hud.update(sf::game::HudInput{.next_weapon = true, .previous_weapon = true});
  require(hud.inventory().current() == sf::game::WeaponId::flashlight &&
              hud.weaponSwitchFrames() == 17U,
          "Conflicting weapon input changed HUD state");
  require(hud.selectWeapon(sf::game::WeaponId::taser) &&
              hud.inventory().current() == sf::game::WeaponId::taser &&
              hud.weaponSwitchFrames() ==
                  sf::game::GameplayHud::weapon_switch_duration,
          "Direct HUD weapon selection did not start its animation");
  hud.update({});
  require(hud.weaponSwitchFrames() ==
                  sf::game::GameplayHud::weapon_switch_duration - 1U &&
              !hud.selectWeapon(sf::game::WeaponId::taser) &&
              hud.weaponSwitchFrames() ==
                  sf::game::GameplayHud::weapon_switch_duration - 1U,
          "Selecting the current weapon restarted its animation");
  require(!hud.selectWeapon(static_cast<sf::game::WeaponId>(0xffU)) &&
              hud.weaponSwitchFrames() ==
                  sf::game::GameplayHud::weapon_switch_duration - 1U,
          "Invalid direct weapon selection changed HUD animation state");
  hud.setTargetHealth(std::nullopt);
  require(!hud.targetBar(), "TARGET HUD did not clear its target");
  const auto target_reveal = hud.targetReveal();
  hud.update({});
  require(hud.targetReveal() < target_reveal,
          "TARGET HUD did not begin its original close animation");

  sf::game::GameplayHud critical;
  critical.setVitals(sf::game::PlayerVitals{
      .health = 30U,
      .maximum_health = 150U,
      .armor = 0U,
      .maximum_armor = 600U,
  });
  require(critical.primaryStatus() == sf::game::PrimaryStatus::health &&
              critical.primaryBar() == 10U && critical.armorBar() == 0U &&
              critical.healthBar() == 10U &&
              critical.healthBarColor() == sf::game::HudRgb{255U, 0U, 0U},
          "Critical HEALTH HUD state mismatch");
  for (auto update = 0; update < 8; ++update) {
    critical.update({});
  }
  require(critical.healthBarColor() == sf::game::HudRgb{0U, 0U, 0U},
          "Critical HEALTH pulse did not reach its dark phase");
  for (auto update = 0; update < 8; ++update) {
    critical.update({});
  }
  require(critical.displayedPrimaryBar() == 0U &&
              critical.displayedPrimaryTrail() == 10U &&
              critical.healthBarColor() == sf::game::HudRgb{255U, 0U, 0U},
          "Two-layer ARMOR/HEALTH animation or pulse period mismatch");
}

void testActorAimZones() {
  const auto head = sf::game::actorAimHit(
      sf::game::ActorAimRay{0.0, -275.0, -1000.0, 0.0, 0.0, 1.0}, 0.0, 0.0,
      0.0);
  const auto lower_head = sf::game::actorAimHit(
      sf::game::ActorAimRay{0.0, -200.0, -1000.0, 0.0, 0.0, 1.0}, 0.0, 0.0,
      0.0);
  const auto body = sf::game::actorAimHit(
      sf::game::ActorAimRay{0.0, -185.0, -1000.0, 0.0, 0.0, 1.0}, 0.0, 0.0,
      0.0);
  const auto miss = sf::game::actorAimHit(
      sf::game::ActorAimRay{0.0, 100.0, -1000.0, 0.0, 0.0, 1.0}, 0.0, 0.0, 0.0);
  require(head && head->zone == sf::game::ActorAimZone::head && lower_head &&
              lower_head->zone == sf::game::ActorAimZone::head && body &&
              body->zone == sf::game::ActorAimZone::body && !miss,
          "Contextual actor head/body aim zones mismatch");
}

void testMissionBriefing() {
  std::vector<std::byte> dlf(768U);
  constexpr std::size_t data_offset = 64U;
  writeLe32(dlf, 0x14U, static_cast<std::uint32_t>(data_offset));
  auto cursor = data_offset + 0x18U;
  const auto appendText = [&dlf](std::size_t &offset, std::string_view text) {
    std::ranges::transform(
        text, dlf.begin() + static_cast<std::ptrdiff_t>(offset),
        [](char value) { return static_cast<std::byte>(value); });
    offset += text.size();
    dlf[offset++] = std::byte{};
  };
  appendText(cursor, "Your targets are in the subway.");
  cursor += 3U;
  appendText(cursor,
             "AGENCY DIRECTIVE:\n\nEnter after CBDC operations begin.\n");
  cursor += 3U;
  appendText(cursor, "08/23 22:45\n");
  cursor += 3U;
  appendText(cursor, "Georgia Street");
  cursor += 3U;
  appendText(cursor, "Washington DC");

  const auto briefing = sf::assets::MissionBriefing::parse(dlf);
  require(briefing.location() == "Washington DC" &&
              briefing.missionTitle() == "Georgia Street" &&
              briefing.dateTime() == "08/23 22:45",
          "DLF mission identity was not recovered");
  require(briefing.directive() ==
                  "AGENCY DIRECTIVE:\n\nEnter after CBDC operations begin.\n" &&
              briefing.additionalDirective() ==
                  "Your targets are in the subway.",
          "DLF mission directives were not recovered");
  const auto retail_directives = briefing.retailDirectives();
  require(briefing.retailTitle() == "Washington DC: Georgia Street" &&
              retail_directives[0] == briefing.directive() &&
              retail_directives[1] == briefing.additionalDirective(),
          "Retail briefing title/directive order mismatch");
  require(
      sf::assets::MissionBriefing::fallback("Washington Park").retailTitle() ==
          "Washington Park",
      "Fallback briefing added an empty location prefix");

  std::vector<std::byte> shared_dlf(1024U);
  writeLe32(shared_dlf, 0x14U, static_cast<std::uint32_t>(data_offset));
  auto shared_cursor = data_offset + 0x18U;
  const auto appendShared = [&shared_dlf](std::size_t &offset,
                                          std::string_view text) {
    std::ranges::transform(
        text, shared_dlf.begin() + static_cast<std::ptrdiff_t>(offset),
        [](char value) { return static_cast<std::byte>(value); });
    offset += text.size();
    shared_dlf[offset++] = std::byte{};
  };
  appendShared(shared_cursor, "First mission context");
  appendShared(shared_cursor, "INCOMING FROM LIAN:\nFirst directive\n");
  appendShared(shared_cursor, "09/08 03:00\n");
  appendShared(shared_cursor, "Warehouse 76");
  appendShared(shared_cursor, "Almaty, Kazakhstan");
  appendShared(shared_cursor, "Continuation context");
  appendShared(shared_cursor, "INCOMING FROM LIAN:\nSecond directive\n");
  appendShared(shared_cursor, "09/08 04:00\n");
  appendShared(shared_cursor, "Warehouse continuation");
  const auto continuation = sf::assets::MissionBriefing::parseRecord(
      shared_dlf, 1U, "Tunnel Blackout");
  require(continuation.location() == "Almaty, Kazakhstan" &&
              continuation.missionTitle() == "Tunnel Blackout" &&
              continuation.dateTime() == "09/08 04:00" &&
              continuation.directive() ==
                  "INCOMING FROM LIAN:\nSecond directive\n" &&
              continuation.additionalDirective() == "Continuation context",
          "Shared DLF continuation briefing record mismatch");

  std::vector<std::byte> overlay(1024U);
  auto overlay_cursor = std::size_t{32U};
  const auto appendOverlay = [&overlay](std::size_t &offset,
                                        std::string_view text) {
    std::ranges::transform(
        text, overlay.begin() + static_cast<std::ptrdiff_t>(offset),
        [](char value) { return static_cast<std::byte>(value); });
    offset += text.size();
    overlay[offset++] = std::byte{};
  };
  appendOverlay(overlay_cursor, "Warehouse context");
  appendOverlay(overlay_cursor, "INCOMING FROM LIAN:\nFirst overlay directive");
  appendOverlay(overlay_cursor, "09/08 04:00");
  appendOverlay(overlay_cursor, "Warehouse 76");
  appendOverlay(overlay_cursor, "Almaty, Kazakhstan");
  appendOverlay(overlay_cursor, "Tunnel context");
  appendOverlay(overlay_cursor,
                "INCOMING FROM LIAN:\nSecond overlay directive");
  appendOverlay(overlay_cursor, "09/08 04:45");
  appendOverlay(overlay_cursor, "Tunnel blackout");
  const auto overlay_continuation =
      sf::assets::MissionBriefing::parseOverlayRecord(overlay, 1U,
                                                      "Tunnel blackout");
  require(overlay_continuation.location() == "Almaty, Kazakhstan" &&
              overlay_continuation.missionTitle() == "Tunnel blackout" &&
              overlay_continuation.dateTime() == "09/08 04:45" &&
              overlay_continuation.directive() ==
                  "INCOMING FROM LIAN:\nSecond overlay directive" &&
              overlay_continuation.additionalDirective() == "Tunnel context",
          "Mission overlay continuation briefing record mismatch");
  auto camera_environment = sf::game::LegacyEnvironmentBridgeState{};
  camera_environment.terrain_depth_cue = 0x00020320U;
  camera_environment.renderer_darkness_enabled = true;
  require(camera_environment.effectiveTerrainDepthCue() == 0x00020320U,
          "Per-object dark-frame cue leaked into camera atmosphere");
  camera_environment = {};
  camera_environment.background_enabled = true;
  camera_environment.terrain_depth_cue = 0x00021f40U;
  require(camera_environment.blackoutEnabled(),
          "CAVE2 retail blackout signature was not recognized");
  camera_environment.fog_color.blue = 1U;
  require(!camera_environment.blackoutEnabled(),
          "A non-CAVE2 atmosphere enabled the blackout base");
  static_assert(sf::assets::RetailBriefingLayout::region_x == -155 &&
                sf::assets::RetailBriefingLayout::region_y == -90 &&
                sf::assets::RetailBriefingLayout::region_width == 310 &&
                sf::assets::RetailBriefingLayout::region_height == 170 &&
                sf::assets::RetailBriefingLayout::red == 110U &&
                sf::assets::RetailBriefingLayout::green == 130U &&
                sf::assets::RetailBriefingLayout::blue == 200U &&
                sf::assets::RetailBriefingLayout::prompt_x == 170 &&
                sf::assets::RetailBriefingLayout::prompt_y == 98 &&
                sf::assets::RetailBriefingLayout::prompt ==
                    "Press %x to continue" &&
                sf::assets::RetailBriefingLayout::cross_u == 94U &&
                sf::assets::RetailBriefingLayout::cross_v == 175U &&
                sf::assets::RetailBriefingLayout::cross_width == 10U &&
                sf::assets::RetailBriefingLayout::cross_height == 8U &&
                sf::assets::RetailBriefingLayout::cross_advance == 12 &&
                sf::assets::RetailBriefingLayout::prompt_width == 117);

  std::vector<std::byte> truncated(32U);
  writeLe32(truncated, 0x14U, 31U);
  try {
    static_cast<void>(sf::assets::MissionBriefing::parse(truncated));
    throw std::runtime_error{"Truncated DLF briefing was accepted"};
  } catch (const sf::core::Error &error) {
    require(error.code() == sf::core::ErrorCode::invalid_format,
            "Truncated DLF briefing returned the wrong error code");
  }
}

void testWeaponDescriptions() {
  const std::string source =
      "SILENCED 9MM\r\n\r\nFire rate\tIII\r\nDamage\t\tII\r\n"
      "Clip size\t\t15\r\nMax rounds\t90\r\n\r\nOriginal description.\r\n*\r\n"
      "FLASHLIGHT\r\n\r\nFire rate\tN/A\r\nDamage\t\tN/A\r\n"
      "Clip size\t\tN/A\r\nMax rounds\tN/A\r\n\r\nField light.\r\n*\r\n";
  std::vector<std::byte> bytes;
  bytes.reserve(source.size() + 1U);
  for (const auto character : source) {
    bytes.push_back(
        static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  bytes.push_back(std::byte{});

  const auto descriptions = sf::assets::WeaponDescriptionTable::parse(bytes);
  require(descriptions.entries().size() == 2U,
          "WEAPDESC record count mismatch");
  const auto *pistol = descriptions.find("silenced 9mm");
  require(pistol != nullptr && pistol->fire_rate == 3U &&
              pistol->damage == 2U && pistol->clip_size == "15" &&
              pistol->maximum_rounds == "90" &&
              pistol->description == "Original description.",
          "WEAPDESC pistol fields mismatch");
  const auto *flashlight = descriptions.find("FLASHLIGHT");
  require(flashlight != nullptr && flashlight->fire_rate == 0U &&
              flashlight->description == "Field light.",
          "WEAPDESC N/A fields mismatch");
}

void testMissionStartGate() {
  sf::game::MissionStartGate gate;
  require(!gate.update(false, false) && !gate.update(false, false) &&
              !gate.update(true, false) &&
              gate.phase() == sf::game::MissionStartPhase::waiting_for_release,
          "Briefing accepted input before its text animation completed");
  require(!gate.update(true) && !gate.update(true) &&
              gate.phase() == sf::game::MissionStartPhase::waiting_for_release,
          "Held movie input skipped the mission briefing");
  require(!gate.update(false) && !gate.update(false) &&
              gate.phase() == sf::game::MissionStartPhase::waiting_for_confirm,
          "Mission briefing did not arm after input release");
  require(!gate.update(false) && gate.update(true) &&
              gate.phase() == sf::game::MissionStartPhase::accepted,
          "Fresh mission-start confirmation was not accepted");
  require(!sf::game::MissionStartGate::brightPrompt(0U) &&
              !sf::game::MissionStartGate::brightPrompt(31U) &&
              sf::game::MissionStartGate::brightPrompt(32U) &&
              sf::game::MissionStartGate::brightPrompt(63U) &&
              !sf::game::MissionStartGate::brightPrompt(64U),
          "Briefing prompt did not preserve its calm 1.6-second phase");
}

void testTitleMenu() {
  sf::game::TitleMenu menu;
  require(menu.phase() == sf::game::TitlePhase::searching,
          "Title did not start in search phase");
  require(menu.itemEnabled(0) && !menu.itemEnabled(1) && menu.itemEnabled(2),
          "Title memory-card search enabled the wrong items");
  require(menu.brightness(sf::game::TitleVisual::new_game) == 0 &&
              menu.brightness(sf::game::TitleVisual::load_game) == 0 &&
              menu.brightness(sf::game::TitleVisual::training_video) == 0 &&
              menu.brightness(sf::game::TitleVisual::searching) == 0,
          "Title sprites did not start dark");

  static_cast<void>(menu.update({}));
  require(menu.brightness(sf::game::TitleVisual::new_game) == 10 &&
              menu.brightness(sf::game::TitleVisual::load_game) == 0 &&
              menu.brightness(sf::game::TitleVisual::training_video) == 10 &&
              menu.brightness(sf::game::TitleVisual::searching) == 10,
          "Native title fade-in targets were not applied");

  static_cast<void>(menu.update(sf::game::TitleInput{.next = true}));
  require(menu.selection() == 2,
          "Title selection did not skip Load Game while searching");
  require(menu.update(sf::game::TitleInput{.confirm = true}) ==
              sf::game::TitleCommand::training_video,
          "Training Video was unavailable while searching");
  static_cast<void>(menu.update(sf::game::TitleInput{.previous = true}));
  require(menu.selection() == 0,
          "Reverse title navigation did not skip unavailable Load Game");
  require(menu.update(sf::game::TitleInput{.confirm = true}) ==
              sf::game::TitleCommand::new_game,
          "New Game was unavailable while searching");
  require(menu.update(sf::game::TitleInput{.cancel = true}) ==
              sf::game::TitleCommand::exit,
          "Title cancel command mismatch");

  sf::game::TitleMenu training_return_menu;
  static_cast<void>(
      training_return_menu.update(sf::game::TitleInput{.next = true}));
  require(training_return_menu.update(sf::game::TitleInput{.confirm = true}) ==
                  sf::game::TitleCommand::training_video &&
              training_return_menu.phase() == sf::game::TitlePhase::searching,
          "Early Training Video selection did not preserve the active search");
  training_return_menu.completeSearch();
  require(training_return_menu.phase() == sf::game::TitlePhase::menu &&
              training_return_menu.remainingSearchFrames() == 0 &&
              training_return_menu.itemEnabled(1),
          "Returning from Training Video did not complete the title search");

  sf::game::TitleMenu timed_menu;
  for (std::uint32_t frame = 1; frame < sf::game::TitleMenu::search_frames;
       ++frame) {
    static_cast<void>(timed_menu.update({}));
  }
  require(timed_menu.phase() == sf::game::TitlePhase::searching,
          "Title search ended one frame early");
  static_cast<void>(timed_menu.update({}));
  require(timed_menu.phase() == sf::game::TitlePhase::menu,
          "Title search did not end on schedule");
  require(timed_menu.itemEnabled(1) &&
              timed_menu.brightness(sf::game::TitleVisual::load_game) == 10 &&
              timed_menu.brightness(sf::game::TitleVisual::searching) == 60,
          "Title search/load cross-fade did not start on schedule");

  static_cast<void>(timed_menu.update(sf::game::TitleInput{.next = true}));
  require(timed_menu.selection() == 1,
          "Load Game could not be selected after searching");
  require(timed_menu.update(sf::game::TitleInput{.confirm = true}) ==
              sf::game::TitleCommand::none,
          "Load Game did not open the retail slot picker");
  require(timed_menu.phase() == sf::game::TitlePhase::load_slots &&
              !timed_menu.itemEnabled(0) && !timed_menu.itemEnabled(1) &&
              !timed_menu.itemEnabled(2),
          "Load slot picker did not suspend the background menu");
  static_cast<void>(timed_menu.update(sf::game::TitleInput{.next = true}));
  require(timed_menu.selection() == 1 && timed_menu.loadSlotSelection() == 1 &&
              timed_menu.phase() == sf::game::TitlePhase::load_slots,
          "Load slot picker moved the background menu");
  require(timed_menu.update(sf::game::TitleInput{.confirm = true}) ==
              sf::game::TitleCommand::none,
          "Empty save slot was accepted");
  require(timed_menu.update(sf::game::TitleInput{.cancel = true}) ==
                  sf::game::TitleCommand::none &&
              timed_menu.phase() == sf::game::TitlePhase::menu,
          "Load slot picker did not return to the title menu");

  sf::game::TitleSaveSlots slots{};
  slots[1] = sf::game::TitleSaveSlot{true, 7U};
  timed_menu.setSaveSlots(slots);
  require(timed_menu.update(sf::game::TitleInput{.confirm = true}) ==
                  sf::game::TitleCommand::none &&
              timed_menu.phase() == sf::game::TitlePhase::load_slots,
          "Load slot picker did not reopen");
  static_cast<void>(timed_menu.update(sf::game::TitleInput{.next = true}));
  require(timed_menu.loadSlotSelection() == 1U &&
              timed_menu.update(sf::game::TitleInput{.confirm = true}) ==
                  sf::game::TitleCommand::load_game &&
              timed_menu.phase() == sf::game::TitlePhase::menu,
          "Occupied save slot did not produce Load Game");

  sf::game::TitleMenu held_confirm_menu;
  held_confirm_menu.completeSearch();
  sf::game::TitleSaveSlots first_slot{};
  first_slot[0] = sf::game::TitleSaveSlot{true, 3U};
  held_confirm_menu.setSaveSlots(first_slot);
  static_cast<void>(
      held_confirm_menu.update(sf::game::TitleInput{.next = true}));
  require(held_confirm_menu.update(
              sf::game::TitleInput{.confirm = true, .confirm_down = true}) ==
                  sf::game::TitleCommand::none &&
              held_confirm_menu.phase() == sf::game::TitlePhase::load_slots,
          "Load Game press did not enter the slot picker");
  require(held_confirm_menu.update(
              sf::game::TitleInput{.confirm = true, .confirm_down = true}) ==
                  sf::game::TitleCommand::none &&
              held_confirm_menu.phase() == sf::game::TitlePhase::load_slots,
          "Opening Load Game press leaked into the first save slot");
  static_cast<void>(held_confirm_menu.update({}));
  require(held_confirm_menu.update(
              sf::game::TitleInput{.confirm = true, .confirm_down = true}) ==
              sf::game::TitleCommand::load_game,
          "Fresh slot confirmation was not accepted after release");

  const auto encoded_slots = sf::game::serializeTitleSaveSlots(slots);
  const auto decoded_slots = sf::game::parseTitleSaveSlots(encoded_slots);
  require(decoded_slots && *decoded_slots == slots &&
              !sf::game::parseTitleSaveSlots("SFPC_SAVE_V1\n0 1 7\n"),
          "Native title save serialization was not deterministic/fail-closed");

  const auto save_directory =
      std::filesystem::temp_directory_path() / "sf_title_save_tests";
  std::filesystem::remove_all(save_directory);
  std::filesystem::create_directories(save_directory);
  const auto save_path = save_directory / "SyphonFilterPC.sav";
  require(sf::game::storeTitleSaveSlotsFile(save_path, slots),
          "Native title save file could not be committed");
  const auto loaded_slots = sf::game::loadTitleSaveSlotsFile(save_path);
  require(loaded_slots.status == sf::game::TitleSaveLoadStatus::loaded &&
              loaded_slots.slots == slots,
          "Native title save file did not round-trip");

  auto backup_path = save_path;
  backup_path += ".bak";
  require(std::filesystem::exists(backup_path),
          "First native title save did not establish a durable backup");
  {
    std::ofstream corrupt{save_path, std::ios::binary | std::ios::trunc};
    auto invalid_slots = slots;
    invalid_slots[1].mission_index = std::numeric_limits<std::uint32_t>::max();
    corrupt << sf::game::serializeTitleSaveSlots(invalid_slots);
  }
  const auto recovered_slots = sf::game::loadTitleSaveSlotsFile(save_path);
  require(recovered_slots.status == sf::game::TitleSaveLoadStatus::recovered &&
              recovered_slots.slots == slots,
          "Native title save did not recover the last complete backup");

  auto replacement_slots = slots;
  replacement_slots[1].mission_index = 8U;
  require(sf::game::storeTitleSaveSlotsFile(save_path, replacement_slots),
          "Native title save could not replace an interrupted commit");
  const auto replaced_slots = sf::game::loadTitleSaveSlotsFile(save_path);
  require(replaced_slots.status == sf::game::TitleSaveLoadStatus::loaded &&
              replaced_slots.slots == replacement_slots,
          "Native title save replacement was not durable");
  std::filesystem::remove_all(save_directory);
  for (int frame = 0; frame < 17; ++frame) {
    static_cast<void>(timed_menu.update({}));
  }
  require(timed_menu.brightness(sf::game::TitleVisual::new_game) == 70 &&
              timed_menu.brightness(sf::game::TitleVisual::load_game) == 200 &&
              timed_menu.brightness(sf::game::TitleVisual::training_video) ==
                  70 &&
              timed_menu.brightness(sf::game::TitleVisual::searching) == 0,
          "Title selection brightness did not converge to the native values");

  static_cast<void>(timed_menu.update(sf::game::TitleInput{.previous = true}));
  static_cast<void>(timed_menu.update(sf::game::TitleInput{.previous = true}));
  require(timed_menu.selection() == 0,
          "Title selection moved before the first item");

  for (int frame = 0; frame < 20; ++frame) {
    static_cast<void>(
        timed_menu.update({}, sf::game::TitleMenu::movie_fade_frame + 1U));
  }
  require(timed_menu.brightness(sf::game::TitleVisual::new_game) == 0 &&
              timed_menu.brightness(sf::game::TitleVisual::load_game) == 0 &&
              timed_menu.brightness(sf::game::TitleVisual::training_video) ==
                  0 &&
              timed_menu.brightness(sf::game::TitleVisual::searching) == 0,
          "Title sprites did not fade before the movie loop boundary");
  static_cast<void>(timed_menu.update({}, 0));
  require(timed_menu.brightness(sf::game::TitleVisual::new_game) == 10 &&
              timed_menu.brightness(sf::game::TitleVisual::load_game) == 10 &&
              timed_menu.brightness(sf::game::TitleVisual::training_video) ==
                  10,
          "Title sprites did not restart their fade on the next movie pass");
}

void testActorShadowReceiverStability() {
  using sf::game::DynamicLightPoint;
  using sf::platform::ActorShadowCachedReceiver;
  using sf::platform::ActorShadowReceiverPlane;

  require(!sf::platform::actorShadowReceiverIsWall({0.0, -1.0, 0.0},
                                                   {0.0, 1.0, 0.0}),
          "Opposite-winding floor triangle was classified as a wall");
  require(
      sf::platform::actorShadowReceiverIsWall({1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}),
      "Perpendicular receiver was not classified as a wall");

  const auto wall =
      ActorShadowReceiverPlane{DynamicLightPoint{100.0, 0.0, 0.0},
                               DynamicLightPoint{-1.0, 0.0, 0.0}, true};
  auto history = ActorShadowCachedReceiver{};
  sf::platform::updateActorShadowReceiver(history, std::nullopt);
  sf::platform::updateActorShadowReceiver(history, wall);
  require(!history.stable,
          "Single wall sample replaced the stable floor receiver");
  sf::platform::updateActorShadowReceiver(history, wall);
  require(history.stable && history.stable->wall,
          "Confirmed wall receiver did not become stable");
  auto opposite_winding_wall = wall;
  opposite_winding_wall.normal = {1.0, 0.0, 0.0};
  sf::platform::updateActorShadowReceiver(history, opposite_winding_wall);
  require(history.stable && history.stable->normal.x < -0.99,
          "Opposite-winding wall flipped the cached receiver bias");
  sf::platform::updateActorShadowReceiver(history, std::nullopt);
  require(history.stable.has_value(),
          "Single missing receiver discarded the stable wall");
  sf::platform::updateActorShadowReceiver(history, std::nullopt);
  require(!history.stable,
          "Confirmed floor receiver did not replace the cached wall");

  const auto projected = sf::platform::projectActorShadowOntoCachedPlane(
      {0.0, 0.0, 0.0}, {200.0, 0.0, 0.0}, wall);
  require(projected && std::abs(projected->x - 96.0) < 0.000001,
          "Cached wall plane projection lost its surface bias");
  require(!sf::platform::projectActorShadowOntoCachedPlane(
              {0.0, 0.0, 0.0}, {0.0, 200.0, 0.0}, wall),
          "Parallel shadow segment intersected the cached plane");
  auto distant_wall = wall;
  distant_wall.point.x = 300.0;
  require(!sf::platform::projectActorShadowOntoCachedPlane(
              {0.0, 0.0, 0.0}, {200.0, 0.0, 0.0}, distant_wall),
          "Cached plane projection escaped the shadow segment");

  auto support = sf::platform::ActorShadowSupportState{};
  const auto floor_a =
      ActorShadowReceiverPlane{{0.0, 20.0, 0.0}, {0.0, 1.0, 0.0}, false};
  auto floor_b =
      ActorShadowReceiverPlane{{100.0, 120.0, 0.0}, {0.0, -1.0, 0.0}, false};
  support = sf::platform::advanceActorShadowSupport(support, floor_a, 10U);
  support = sf::platform::advanceActorShadowSupport(support, floor_b, 10U);
  require(support.current.point == floor_a.point,
          "Support plane advanced twice during one guest tick");
  support = sf::platform::advanceActorShadowSupport(support, floor_b, 11U);
  require(support.current.point == floor_a.point,
          "One ledge sample replaced the stable support plane");
  support = sf::platform::advanceActorShadowSupport(support, floor_a, 12U);
  support = sf::platform::advanceActorShadowSupport(support, floor_b, 13U);
  require(support.current.point == floor_a.point,
          "Alternating ledge candidates made the support plane dance");
  support = sf::platform::advanceActorShadowSupport(support, floor_b, 14U);
  const auto halfway = sf::platform::sampleActorShadowSupport(support, 0.5);
  require(halfway && std::abs(halfway->point.x - 50.0) < 0.000001 &&
              std::abs(halfway->point.y - 70.0) < 0.000001 &&
              halfway->normal.y > 0.99,
          "Support plane did not interpolate smoothly across a ledge");
  support = sf::platform::advanceActorShadowSupport(support, std::nullopt, 15U);
  const auto retained = sf::platform::sampleActorShadowSupport(support, 1.0);
  require(retained && retained->point == floor_b.point,
          "One missing support query discarded the actor shadow");
  support = sf::platform::advanceActorShadowSupport(support, std::nullopt, 16U);
  require(!sf::platform::sampleActorShadowSupport(support, 1.0),
          "Confirmed missing support left a stale actor shadow suspended");
}

} // namespace

int main() {
  try {
    testSha256();
    testFogArchive();
    testInvalidFogArchive();
    testMissionCatalog();
    testHogArchive();
    testTimImage();
    testEmdScene();
    testGmdModel();
    testCfireSpawnPoint();
    testLegacyEffectSpriteLayouts();
    testLegacyDynamicPresentationPolicy();
    testGameplayCheckpointRestorePolicy();
    testPoliceLightbarFrames();
    testHmdModel();
    testHmdAnimation();
    testActorAnimationBank();
    testChaseCamera();
    testPlayerController();
    testPlayerRootMotionCadence();
    testPlayerPersistentActions();
    testPolygonClipper();
    testLevelLayout();
    testMissionObjects();
    testInvalidAssets();
    testExecutable();
    testInvalidExecutable();
    testCueSheet();
    testRawSectorFile();
    testFunctionMap();
    testStateStack();
    testStateStackBounds();
    testSystemBootOrder();
    testPlayerInventory();
    testGameplayHud();
    testActorAimZones();
    testMissionBriefing();
    testWeaponDescriptions();
    testMissionStartGate();
    testTitleMenu();
    testActorShadowReceiverStability();
    std::cout << "All tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Test failure: " << error.what() << '\n';
    return 1;
  }
}
