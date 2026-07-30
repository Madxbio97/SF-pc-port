#include "sf/game/pause_menu_data.hpp"

#include "sf/assets/tim_image.hpp"
#include "sf/assets/weapon_descriptions.hpp"
#include "sf/game/combat.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/hud.hpp"
#include "sf/game/localization.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/pause_menu.hpp"
#include "sf/game/retail_pause_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sf::game {
namespace {
constexpr int briefing_text_width = 157;
constexpr std::size_t briefing_lines_per_page = 9U;

bool briefingWordFits(std::string_view line, std::string_view word) noexcept {
  auto candidate = std::string{line};
  if (!candidate.empty()) {
    candidate.push_back(' ');
  }
  candidate.append(word);
  return originalHudTextWidth(candidate) <= briefing_text_width;
}

void appendBriefingWord(std::vector<std::string> &lines, std::string &line,
                        std::string_view word) {
  if (word.empty()) {
    return;
  }
  if (briefingWordFits(line, word)) {
    if (!line.empty()) {
      line.push_back(' ');
    }
    line.append(word);
    return;
  }
  if (!line.empty()) {
    lines.push_back(std::move(line));
    line.clear();
  }

  // Authored prose normally breaks at spaces.  Keep an overlong identifier
  // safe as well, without dropping bytes from single-byte SCUS/ViT text.
  auto cursor = std::size_t{};
  while (cursor < word.size()) {
    auto end = cursor;
    std::string fragment;
    while (end < word.size()) {
      auto candidate = fragment;
      candidate.push_back(word[end]);
      if (!fragment.empty() &&
          originalHudTextWidth(candidate) > briefing_text_width) {
        break;
      }
      fragment = std::move(candidate);
      ++end;
    }
    if (end == cursor) {
      ++end;
      fragment.assign(word.substr(cursor, 1U));
    }
    if (end < word.size()) {
      lines.push_back(std::move(fragment));
    } else {
      line = std::move(fragment);
    }
    cursor = end;
  }
}

void appendBriefingSection(std::vector<std::string> &lines,
                           std::string_view source) {
  std::string line;
  auto word_start = std::size_t{};
  for (auto cursor = std::size_t{}; cursor <= source.size(); ++cursor) {
    const auto at_end = cursor == source.size();
    const auto character = at_end ? '\0' : source[cursor];
    const auto separator = at_end || character == ' ' || character == '\t' ||
                           character == '\r' || character == '\n';
    if (!separator) {
      continue;
    }
    if (cursor > word_start) {
      appendBriefingWord(lines, line,
                         source.substr(word_start, cursor - word_start));
    }
    if (character == '\n' && !line.empty()) {
      lines.push_back(std::move(line));
      line.clear();
    }
    word_start = cursor + 1U;
  }
  if (!line.empty()) {
    lines.push_back(std::move(line));
  }
}

std::string_view pauseWeaponIcon(WeaponId id) noexcept {
  switch (id) {
  case WeaponId::silenced_9mm:
  case WeaponId::pistol_9mm:
    return "GLOKSIL.TIM";
  case WeaponId::pistol_45:
    return "COLT45.TIM";
  case WeaponId::g_18:
    return "GLOCK18.TIM";
  case WeaponId::combat_shotgun:
    return "BERELLI.TIM";
  case WeaponId::shotgun:
    return "ITHICA37.TIM";
  case WeaponId::pk_102:
    return "AK102.TIM";
  case WeaponId::m_16:
    return "M16.TIM";
  case WeaponId::biz_2:
    return "BIZON2.TIM";
  case WeaponId::hk_5:
    return "MP5.TIM";
  case WeaponId::k3g4:
    return "G3.TIM";
  case WeaponId::nightvision_rifle:
    return "DRAGSVD.TIM";
  case WeaponId::sniper_rifle:
    return "SUPERG.TIM";
  case WeaponId::taser:
    return "TASER.TIM";
  case WeaponId::m_79:
    return "GRENLAUN.TIM";
  case WeaponId::virus_scanner:
    return "VIRLSCAN.TIM";
  case WeaponId::fragmentation_grenade:
    return "GRENADE.TIM";
  case WeaponId::gas_grenade:
    return "GASGREN.TIM";
  case WeaponId::flashlight:
    return "FLASHLT.TIM";
  case WeaponId::key_card:
    return "KCARD.TIM";
  case WeaponId::c4_explosives:
    return "C4.TIM";
  case WeaponId::viral_antigen:
    return "ANTIGEN.TIM";
  default:
    return {};
  }
}

std::string_view pauseWeaponDescription(WeaponId id) noexcept {
  switch (id) {
  case WeaponId::silenced_9mm:
  case WeaponId::pistol_9mm:
    return "The 9mm handgun is the standard issue side-arm for NATO and all "
           "five branches of the US armed forces since passing the 1979 MRBF "
           "(Mean Rounds Before operational Failure) performance test, "
           "expending 35,000 rounds, six times the pistol's service life.";
  case WeaponId::pistol_45:
    return "This tough, durable pistol has been in production for almost a "
           "century. It has tremendous stopping power, and in spite of it's "
           "strong recoil and heavy slide and bolt, in the hands of a seasoned "
           "professional, it is a deadly weapon.";
  case WeaponId::g_18:
    return "With a rate of fire topping 60 rounds per second, the G-18 is "
           "perhaps the most deadly pistol-machinegun in the world. It's only "
           "weakness is it's tendency to expend ammunition faster than most "
           "shooters are prepared for, leaving them defenseless during a "
           "reload.";
  case WeaponId::combat_shotgun:
    return "The overly heavy recoil of this 12 gauge shotgun is more than "
           "compensated for by it's unparalleled stopping power and its "
           "recoil-inertia operation which is significantly faster than the "
           "gas operated system found in most autoloading shotguns.";
  case WeaponId::shotgun:
    return "The 12-gauge modified choke shotgun is standard issue for the DEA, "
           "FBI and USSS. In firing tests using tactical 00 shot with nine "
           "lead "
           "on an ISCP regulation target at 25 yards, the payload was "
           "delivered "
           "into the \"A\" kill zone with limited collateral damage.";
  case WeaponId::pk_102:
    return "A variant of the popular Vokinhsilak system (one of the most "
           "widely "
           "used and modified designs in the world) the PK102 is a compact, "
           "lightweight, full assault rifle that is easy to conceal, making it "
           "a popular choice for terrorists.";
  case WeaponId::m_16:
    return "This weapon is lightweight, accurate, and has very low recoil. The "
           "preeminent assault rifle in the world, it was developed by the US "
           "Army in 1965, and has since become a mainstay for armed forces, "
           "police, and personal defense enthusiasts.";
  case WeaponId::biz_2:
    return "This pistol-machine gun is designed to deliver sustained firepower "
           "in tight quarters. The unconventional design of its large capacity "
           "magazine keeps the weapon compact but still provides a "
           "near-bottomless source of ammunition.";
  case WeaponId::hk_5:
    return "The HK5's modular design and small size makes it very popular with "
           "both military special forces and terrorists. With over 23 "
           "officially recognized variants, it is fast becoming the most "
           "widely "
           "used pistol-machine gun in the world.";
  case WeaponId::nightvision_rifle:
    return "A Russian rifle capable of high accuracy, it is often used by "
           "Russian Army snipers. It excels in engaging fleeting, moving, open "
           "and masked single targets. This model comes standard equipped with "
           "an SVDN2 night sight and silencer.";
  case WeaponId::sniper_rifle:
    return "This high-caliber, silenced rifle comes equipped with a classified "
           "digital scope with basic optical character recognition, making it "
           "a highly accurate weapon capable of identifying and classifying "
           "human targets and impact points prior to firing.";
  case WeaponId::taser:
    return "Using CO2 cartridges, this weapon fires a probe that lodges one "
           "inch "
           "deep in the victim's body. Then a charge of 500,000 volts is "
           "passed "
           "along a wire connecting the weapon to the probe. This charge can "
           "be "
           "sustained indefinitely.";
  case WeaponId::m_79:
    return "This single-barreled, break-action grenade launcher was developed "
           "during the Vietnam war. Commonly referred to as the \"Blooper\", "
           "it "
           "fires 40mm HE grenades that contain enough explosives to produce "
           "over 300 fragments with a lethal radius of up to five meters.";
  case WeaponId::k3g4:
    return "A popular assault rifle, the K3G4 is commonly armed with "
           "Teflon-coated bullets, making it a deadly weapon capable of "
           "cutting "
           "through most standard-issue flak jackets like a hot knife through "
           "butter.";
  case WeaponId::virus_scanner:
    return "Developed in secret by the viral research branch of PHARCOM Inc., "
           "this device is capable of detecting trace particles of the Syphon "
           "Filter virus from up to 50 meters away. It can also scan through "
           "some solid objects and provide visual feedback of their contents.";
  case WeaponId::fragmentation_grenade:
    return "Upon detonation, this incendiary weapon spreads ammonium "
           "perchlorate "
           "three meters outwards from the blast point. It is instantly "
           "ignited "
           "by the explosion and quickly burns out, fatally burning anyone "
           "nearby, but leaving little collateral damage in the terrain.";
  case WeaponId::gas_grenade:
    return "Primarily used as a stealth weapon against multiple targets, this "
           "grenade releases trace amounts of Soman nerve agent into the air. "
           "The gas quickly dissipates, but not before rendering victims "
           "unconscious. If no antidote is administered, death follows within "
           "15 minutes.";
  case WeaponId::flashlight:
    return "Standard equipment for all agency operatives, this flashlight is "
           "shockproof and charged by a 300 hour battery.";
  case WeaponId::key_card:
    return "A standard magnetic-strip card key.";
  case WeaponId::c4_explosives:
    return "These incendiary blocks are made of a putty-like material which "
           "can "
           "be molded to the user's liking. The C4 explosive putty is then "
           "wired "
           "to a fuse and a friction igniter, allowing the user to detonate "
           "the "
           "explosive from a distant or protected position.";
  case WeaponId::viral_antigen:
    return "This device is used to subcutanelously inject a fine stream of "
           "fluid under high pressure without puncturing the skin, and is "
           "loaded with an experimental serum capable of counteracting the "
           "effects of the Syphon Filter virus.";
  default:
    return {};
  }
}

struct PauseWeaponSpecifications {
  std::uint8_t fire_rate{};
  std::uint8_t damage{};
  std::string_view clip_size;
  std::string_view maximum_rounds;
  bool authored{};
};

PauseWeaponSpecifications pauseWeaponSpecifications(WeaponId id) noexcept {
  using Specifications = PauseWeaponSpecifications;
  switch (id) {
  case WeaponId::silenced_9mm:
  case WeaponId::pistol_9mm:
    return Specifications{3U, 2U, "15", "90", true};
  case WeaponId::pistol_45:
    return Specifications{2U, 3U, "10", "60", true};
  case WeaponId::g_18:
    return Specifications{5U, 2U, "33", "198", true};
  case WeaponId::combat_shotgun:
    return Specifications{2U, 4U, "N/A", "30", true};
  case WeaponId::shotgun:
    return Specifications{2U, 4U, "N/A", "25", true};
  case WeaponId::pk_102:
  case WeaponId::m_16:
    return Specifications{4U, 2U, "30", "180", true};
  case WeaponId::biz_2:
    return Specifications{4U, 3U, "66", "396", true};
  case WeaponId::hk_5:
    return Specifications{4U, 3U, "32", "192", true};
  case WeaponId::nightvision_rifle:
    return Specifications{1U, 3U, "10", "30", true};
  case WeaponId::sniper_rifle:
    return Specifications{2U, 2U, "10", "30", true};
  case WeaponId::taser:
    return Specifications{1U, 5U, "N/A", "Infinite", true};
  case WeaponId::m_79:
    return Specifications{1U, 5U, "N/A", "15", true};
  case WeaponId::k3g4:
    return Specifications{4U, 2U, "20", "120", true};
  case WeaponId::virus_scanner:
  case WeaponId::flashlight:
  case WeaponId::key_card:
  case WeaponId::viral_antigen:
    return Specifications{0U, 0U, "N/A", "N/A", true};
  case WeaponId::fragmentation_grenade:
  case WeaponId::gas_grenade:
    return Specifications{1U, 5U, "N/A", "10", true};
  case WeaponId::c4_explosives:
    return Specifications{0U, 5U, "N/A", "N/A", true};
  default:
    return {};
  }
}

std::string normalizedDescriptionText(std::string_view source) {
  std::string result;
  result.reserve(source.size());
  auto pending_space = false;
  for (const auto character : source) {
    const auto whitespace = character == ' ' || character == '\t' ||
                            character == '\r' || character == '\n';
    if (whitespace) {
      pending_space = !result.empty();
      continue;
    }
    if (pending_space) {
      result.push_back(' ');
      pending_space = false;
    }
    result.push_back(character);
  }
  return result;
}

std::pair<std::uint8_t, std::uint8_t> pauseWeaponRatings(WeaponId id) noexcept {
  const auto specifications = pauseWeaponSpecifications(id);
  if (specifications.authored) {
    return {specifications.fire_rate, specifications.damage};
  }
  return {std::uint8_t{0}, std::uint8_t{0}};
}

std::uint8_t pauseWeaponAccuracy(WeaponId id) noexcept {
  const auto &definition = weaponCombatDefinition(id);
  if (!definition.fires()) {
    return 0U;
  }
  if (definition.spread_angle == 0U) {
    return 5U;
  }
  if (definition.spread_angle <= 32U) {
    return 4U;
  }
  if (definition.spread_angle <= 64U) {
    return 3U;
  }
  if (definition.spread_angle <= 96U) {
    return 2U;
  }
  return 1U;
}

std::optional<std::uint32_t>
pauseMapAssetIndex(std::string_view name) noexcept {
  constexpr std::string_view prefix{"MAP"};
  constexpr std::string_view suffix{".TIM"};
  if (!name.starts_with(prefix) || !name.ends_with(suffix) ||
      name.size() <= prefix.size() + suffix.size()) {
    return std::nullopt;
  }
  auto index = std::uint32_t{};
  const auto digits =
      name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  for (const auto character : digits) {
    if (character < '0' || character > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::uint32_t>(character - '0');
    if (index > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U) {
      return std::nullopt;
    }
    index = index * 10U + digit;
  }
  return index;
}

} // namespace

std::string_view pauseWeaponArtAsset(WeaponId id) noexcept {
  return pauseWeaponIcon(id);
}

std::vector<std::string>
paginatePauseBriefing(std::string_view directive,
                      std::string_view additional_directive) {
  std::vector<std::string> lines;
  appendBriefingSection(lines, directive);
  appendBriefingSection(lines, additional_directive);
  if (lines.empty()) {
    return {};
  }

  std::vector<std::string> pages;
  pages.reserve((lines.size() + briefing_lines_per_page - 1U) /
                briefing_lines_per_page);
  for (auto first = std::size_t{}; first < lines.size();
       first += briefing_lines_per_page) {
    const auto last = std::min(first + briefing_lines_per_page, lines.size());
    std::string page;
    for (auto line = first; line < last; ++line) {
      if (!page.empty()) {
        page.push_back('\n');
      }
      page += lines[line];
    }
    pages.push_back(std::move(page));
  }
  return pages;
}

PauseMenuData makePauseMenuData(const MissionPackage &mission,
                                const GameplaySession &gameplay,
                                std::uint32_t maximum_unlocked_mission) {
  PauseMenuData data;
  data.current_mission = mission.definition().index;
  data.maximum_unlocked_mission = maximum_unlocked_mission;
  for (const auto &definition : missionCatalog()) {
    auto label = std::to_string(definition.index + 1U) + ". ";
    label.append(localizeText(definition.title));
    data.missions.push_back(MissionMenuEntry{
        definition.index,
        std::move(label),
        MissionEntryState::active,
        true,
    });
  }
  const auto &briefing = mission.briefing();
  data.mission.mission_name = std::string{briefing.missionTitle()};
  data.mission.date_time = std::string{briefing.dateTime()};
  data.mission.location = std::string{briefing.location()};
  data.mission.briefing_pages = paginatePauseBriefing(
      briefing.directive(), briefing.additionalDirective());
  const auto localized_menu = localizedMissionMenuTexts(
      mission.definition().index, gameplay.missionObjectiveTexts(),
      gameplay.missionParameterTexts());
  const auto objective_texts =
      localized_menu ? std::span<const std::string>{localized_menu->objectives}
                     : gameplay.missionObjectiveTexts();
  const auto parameter_texts =
      localized_menu ? std::span<const std::string>{localized_menu->parameters}
                     : gameplay.missionParameterTexts();
  data.mission.objectives = makeRetailMissionMenuEntries(
      objective_texts, gameplay.missionObjectiveCount(),
      gameplay.revealedObjectiveMask(), gameplay.completedObjectiveMask(),
      gameplay.failedObjectiveMask());
  data.mission.parameters = makeRetailMissionMenuEntries(
      parameter_texts, gameplay.missionParameterCount(),
      gameplay.missionParameterMask(), 0U, gameplay.failedParameterMask());
  std::vector<std::pair<std::uint32_t, std::string>> map_assets;
  for (const auto &entry : mission.menuAssets().entries()) {
    if (const auto index = pauseMapAssetIndex(entry.name)) {
      map_assets.emplace_back(*index, entry.name);
    }
  }
  std::ranges::sort(map_assets, [](const auto &left, const auto &right) {
    return left.first != right.first ? left.first < right.first
                                     : left.second < right.second;
  });
  std::vector<std::pair<std::uint16_t, std::uint16_t>> map_dimensions;
  data.mission.map.layer_assets.reserve(map_assets.size());
  map_dimensions.reserve(map_assets.size());
  for (auto &[index, name] : map_assets) {
    static_cast<void>(index);
    const auto image = assets::TimImage::parse(mission.menuAssets().file(name));
    map_dimensions.emplace_back(image.displayWidth(), image.displayHeight());
    data.mission.map.layer_assets.push_back(std::move(name));
  }
  const auto mission_index =
      static_cast<std::size_t>(mission.definition().index);
  data.mission.map.reconnaissance_available =
      !data.mission.map.layer_assets.empty() &&
      retailPauseMapAvailable(mission_index);
  data.mission.map.current_location = data.mission.mission_name;
  const auto &guest_motion = gameplay.legacyPlayerGuestMotionPosition();
  const auto motion_x =
      guest_motion
          ? guest_motion->x
          : static_cast<std::int32_t>(std::lround(gameplay.player().x));
  const auto motion_y =
      guest_motion
          ? guest_motion->y
          : -static_cast<std::int32_t>(std::lround(gameplay.player().y));
  const auto motion_z =
      guest_motion
          ? guest_motion->z
          : static_cast<std::int32_t>(std::lround(gameplay.player().z));
  const auto player_map_point =
      retailPauseMapPlayer(mission_index, motion_x, motion_y, motion_z);
  data.mission.map.current_layer =
      player_map_point && player_map_point->page < map_dimensions.size()
          ? player_map_point->page
          : 0U;

  const auto normalized_map_point =
      [&map_dimensions](
          std::uint8_t page, std::int32_t x,
          std::int32_t y) -> std::optional<std::pair<float, float>> {
    if (page >= map_dimensions.size()) {
      return std::nullopt;
    }
    const auto [width, height] = map_dimensions[page];
    if (width == 0U || height == 0U) {
      return std::nullopt;
    }
    return std::pair{
        0.5F + static_cast<float>(x) / static_cast<float>(width),
        0.5F + static_cast<float>(y) / static_cast<float>(height),
    };
  };
  if (data.mission.map.reconnaissance_available) {
    if (player_map_point) {
      auto map_heading = static_cast<float>(gameplay.player().yaw) / 4096.0F;
      if (const auto &rotation = gameplay.legacyPlayerGuestRotation()) {
        // MENU.OVL projects both the motion root and one matrix-forward point,
        // then derives the triangle direction in map space. World yaw is not
        // equivalent on missions whose map swaps or negates axes.
        const auto forward_point = retailPauseMapPlayerOnPage(
            mission_index, player_map_point->page, motion_x + (*rotation)[2],
            motion_y, motion_z + (*rotation)[8]);
        if (forward_point) {
          const auto delta_x = forward_point->x - player_map_point->x;
          const auto delta_y = forward_point->y - player_map_point->y;
          if (delta_x != 0 || delta_y != 0) {
            auto angle = std::atan2(static_cast<double>(delta_x),
                                    -static_cast<double>(delta_y));
            if (angle < 0.0) {
              angle += 2.0 * std::numbers::pi;
            }
            map_heading = static_cast<float>(angle / (2.0 * std::numbers::pi));
          }
        }
      }
      if (const auto position =
              normalized_map_point(player_map_point->page, player_map_point->x,
                                   player_map_point->y)) {
        data.mission.map.markers.push_back(PauseMapMarker{
            MapMarkerKind::player,
            position->first,
            position->second,
            map_heading,
            true,
            0U,
            {},
            player_map_point->page,
        });
      }
    }

    const auto records = retailPauseMapRecords(mission_index);
    for (const auto &objective : data.mission.objectives) {
      if (!objective.visible || objective.state != MissionEntryState::active) {
        continue;
      }
      const auto record = std::ranges::find_if(records, [&](const auto &entry) {
        return static_cast<std::uint32_t>(entry.objective) + 1U ==
                   objective.id &&
               entry.page != 0xffU;
      });
      if (record == records.end()) {
        continue;
      }
      if (const auto position =
              normalized_map_point(record->page, record->x, record->y)) {
        data.mission.map.markers.push_back(PauseMapMarker{
            MapMarkerKind::objective,
            position->first,
            position->second,
            0.0F,
            true,
            objective.id,
            {},
            record->page,
        });
      }
    }
  }

  const auto &inventory = gameplay.hud().inventory();
  // Always use the original table for stable IDs, ratings and ammunition
  // metadata. Russian prose is authored in the central UTF-8 catalogue; the
  // historical translated WEAPDESC mixed encodings and assigned descriptions
  // to the wrong inventory slots.
  const auto descriptions = assets::WeaponDescriptionTable::parse(
      mission.menuAssets().file("WEAPDESC.TXT"));
  for (std::size_t index = 1; index < weapon_slot_count; ++index) {
    const auto id = static_cast<WeaponId>(index);
    const auto *state = inventory.tryState(id);
    const auto *definition = tryWeaponDefinition(id);
    if (state == nullptr || definition == nullptr || !state->owned) {
      continue;
    }
    auto *original = descriptions.find(definition->name);
    if (original == nullptr && id == WeaponId::glock_17) {
      original = descriptions.find("9MM");
    }
    // Retail records follow the native inventory slot order.  Keep an index
    // fallback for localized/repacked images whose encoded record name no
    // longer compares equal to the English WeaponDefinition name.
    if (original == nullptr && index < descriptions.entries().size()) {
      original = &descriptions.entries()[index];
    }
    const auto specifications = pauseWeaponSpecifications(id);
    const auto fallback_ratings = pauseWeaponRatings(id);
    const auto fire_rate = specifications.authored
                               ? specifications.fire_rate
                               : (original == nullptr ? fallback_ratings.first
                                                      : original->fire_rate);
    const auto damage = specifications.authored
                            ? specifications.damage
                            : (original == nullptr ? fallback_ratings.second
                                                   : original->damage);
    const auto original_description =
        original == nullptr ? std::string{}
                            : normalizedDescriptionText(original->description);
    // Pause-menu text is localized at the renderer boundary.  Keep the
    // canonical English description here so it goes through the same single
    // localization pass as every other render command.  Pre-localizing this
    // field made the ViT-encoded result pass through localizeTextCopy() a
    // second time and could corrupt or bypass the authored description.
    const auto authored_description = pauseWeaponDescription(id);
    const auto description =
        !authored_description.empty()   ? std::string{authored_description}
        : !original_description.empty() ? original_description
                                        : std::string{"Agency field weapon."};
    const auto maximum_ammo =
        definition->uses_ammo
            ? static_cast<std::int32_t>(definition->magazine_capacity) *
                  (static_cast<std::int32_t>(definition->reserve_magazines) + 1)
            : 0;
    data.weapons.push_back(PauseWeaponData{
        static_cast<std::uint32_t>(id),
        localizeTextCopy(definition->name),
        std::string{pauseWeaponArtAsset(id)},
        description,
        static_cast<std::int32_t>(state->magazine) + state->reserve,
        maximum_ammo,
        fire_rate,
        damage,
        pauseWeaponAccuracy(id),
        true,
        inventory.current() == id,
        gameplay.canEquipWeapon(id),
        specifications.authored
            ? std::string{specifications.clip_size}
            : (original == nullptr
                   ? std::to_string(definition->magazine_capacity)
                   : original->clip_size),
        specifications.authored
            ? std::string{specifications.maximum_rounds}
            : (original == nullptr ? std::to_string(maximum_ammo)
                                   : original->maximum_rounds),
    });
  }
  return data;
}

} // namespace sf::game
