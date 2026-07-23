#include "sf/assets/emd_scene.hpp"
#include "sf/core/error.hpp"
#include "sf/core/sha256.hpp"
#include "sf/game/actor_animation.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/legacy_first_mission_runtime.hpp"
#include "sf/game/legacy_gameplay_vm.hpp"
#include "sf/game/mission.hpp"
#include "sf/game/title.hpp"
#include "sf/psx/function_map.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

void printUsage() {
  std::cerr
      << "Usage:\n"
      << "  sf_tool inspect <game.cue>\n"
      << "  sf_tool inspect-title <game.cue>\n"
      << "  sf_tool inspect-mission <game.cue> [mission-index]\n"
      << "  sf_tool catalog <game.cue>\n"
      << "  sf_tool list-files <game.cue> [iso-path]\n"
      << "  sf_tool extract-exe <game.cue> <output-file>\n"
      << "  sf_tool extract-file <game.cue> <iso-path> <output-file>\n"
      << "  sf_tool extract-mission-file <game.cue> <fog-name> <output-file>\n"
      << "  sf_tool map-functions <game.cue> <output.csv>\n"
      << "  sf_tool probe-legacy-vm <game.cue>\n"
      << "  sf_tool probe-legacy-cd <game.cue>\n"
      << "  sf_tool probe-legacy-loop <game.cue>\n"
      << "  sf_tool probe-legacy-bootstrap <game.cue>\n"
      << "  sf_tool probe-legacy-level <game.cue> [frames]\n"
      << "  sf_tool probe-legacy-mission <game.cue> <raw-ram.bin>\n"
      << "  sf_tool probe-legacy-frame <game.cue> <raw-ram.bin> [frames]\n";
}

sf::game::GameDisc openDisc(const char *path) {
  return sf::game::GameDisc::open(std::filesystem::path{path});
}

std::vector<std::byte> readHostFile(const char *path) {
  std::ifstream input{std::filesystem::path{path},
                      std::ios::binary | std::ios::ate};
  if (!input) {
    throw sf::core::Error{sf::core::ErrorCode::io, "Cannot open host file"};
  }
  const auto end = input.tellg();
  if (end < 0 ||
      static_cast<std::uint64_t>(end) >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw sf::core::Error{sf::core::ErrorCode::io, "Host file is too large"};
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input) {
    throw sf::core::Error{sf::core::ErrorCode::io, "Failed to read host file"};
  }
  return bytes;
}

std::uint32_t parseFrameCount(const char *text) {
  const std::string_view value{text};
  std::uint32_t count{};
  const auto *const value_end = value.data() + value.size();
  const auto [end, error] = std::from_chars(value.data(), value_end, count);
  if (error != std::errc{} || end != value_end || count == 0U ||
      count > 10'000U) {
    throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                          "Legacy frame count must be in the range 1..10000"};
  }
  return count;
}

int inspect(const char *path) {
  auto disc = openDisc(path);
  const auto &header = disc.executable().header();

  std::cout << "Volume ID:       " << disc.image().volumeId() << '\n'
            << "Boot executable: " << disc.bootPath() << '\n'
            << "Executable SHA:  " << sf::core::toHex(disc.executableHash())
            << '\n'
            << "Entry point:     0x" << std::hex << std::uppercase
            << header.initial_pc << '\n'
            << "Text address:    0x" << header.text_address << '\n'
            << "Text size:       0x" << header.text_size << std::dec << " ("
            << header.text_size << ")\n";

  if (disc.game()) {
    const auto &game = *disc.game();
    std::cout << "Recognized:      " << game.title << " " << game.region << " v"
              << game.version << " [" << game.serial << "]\n";
    return 0;
  }

  std::cout << "Recognized:      no\n";
  return 2;
}

int extractExecutable(const char *cue_path, const char *output_path) {
  auto disc = openDisc(cue_path);
  std::ofstream output{std::filesystem::path{output_path},
                       std::ios::binary | std::ios::trunc};
  if (!output) {
    throw sf::core::Error{sf::core::ErrorCode::io, "Cannot open output file"};
  }
  const auto &bytes = disc.executableFile();
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw sf::core::Error{sf::core::ErrorCode::io,
                          "Failed to write executable"};
  }
  std::cout << "Extracted " << bytes.size() << " bytes to " << output_path
            << '\n';
  return 0;
}

int extractFile(const char *cue_path, const char *iso_path,
                const char *output_path) {
  auto disc = openDisc(cue_path);
  const auto bytes = disc.image().readFile(iso_path);
  std::ofstream output{std::filesystem::path{output_path},
                       std::ios::binary | std::ios::trunc};
  if (!output) {
    throw sf::core::Error{sf::core::ErrorCode::io, "Cannot open output file"};
  }
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw sf::core::Error{sf::core::ErrorCode::io,
                          "Failed to write extracted file"};
  }
  std::cout << "Extracted " << bytes.size() << " bytes from " << iso_path
            << " to " << output_path << '\n';
  return 0;
}

int extractMissionFile(const char *cue_path, const char *name,
                       const char *output_path) {
  auto disc = openDisc(cue_path);
  const auto mission = sf::game::MissionPackage::loadFirst(disc);
  const auto bytes = mission.archive().file(name);
  std::ofstream output{std::filesystem::path{output_path},
                       std::ios::binary | std::ios::trunc};
  if (!output) {
    throw sf::core::Error{sf::core::ErrorCode::io, "Cannot open output file"};
  }
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw sf::core::Error{sf::core::ErrorCode::io,
                          "Failed to write extracted mission file"};
  }
  std::cout << "Extracted " << bytes.size() << " bytes from " << name << " to "
            << output_path << '\n';
  return 0;
}

int catalog(const char *cue_path) {
  auto disc = openDisc(cue_path);
  const auto overlays = disc.overlays();
  std::cout << "path,size,sha256\n";
  for (const auto &overlay : overlays) {
    std::cout << overlay.path << ',' << overlay.size << ','
              << sf::core::toHex(overlay.sha256) << '\n';
  }
  return 0;
}

void listFiles(sf::disc::Iso9660Image &image, const std::string &path) {
  for (const auto &entry : image.list(path)) {
    const auto child = path.empty() ? entry.name : path + '/' + entry.name;
    if (entry.is_directory) {
      listFiles(image, child);
    } else {
      std::cout << child << ',' << entry.size << '\n';
    }
  }
}

int listDiscFiles(const char *cue_path, const char *root) {
  auto disc = openDisc(cue_path);
  std::cout << "path,size\n";
  listFiles(disc.image(), root);
  return 0;
}

int inspectTitle(const char *cue_path) {
  auto disc = openDisc(cue_path);
  const auto title = sf::game::TitleAssets::load(disc);
  std::cout << "name,mode,width,height,vram_x,vram_y,screen_x,screen_y\n";
  for (const auto &sprite : title.sprites()) {
    const auto &pixels = sprite.image.pixels();
    std::cout << sprite.name << ','
              << static_cast<unsigned int>(sprite.image.mode()) << ','
              << sprite.image.displayWidth() << ','
              << sprite.image.displayHeight() << ',' << pixels.x << ','
              << pixels.y << ',' << sprite.x << ',' << sprite.y << '\n';
  }
  return 0;
}

int inspectMission(const char *cue_path, std::uint32_t mission_index) {
  auto disc = openDisc(cue_path);
  const auto mission = sf::game::MissionPackage::load(disc, mission_index);
  const auto &definition = mission.definition();
  std::size_t section_count = 0;
  std::size_t vertex_count = 0;
  std::size_t polygon_count = 0;
  std::vector<sf::assets::EmdScene> world_scenes;
  world_scenes.reserve(mission.worldModels().entries().size());
  for (const auto &entry : mission.worldModels().entries()) {
    auto scene =
        sf::assets::EmdScene::parse(mission.worldModels().file(entry.name));
    section_count += scene.sections().size();
    vertex_count += scene.vertexCount();
    polygon_count += scene.polygonCount();
    world_scenes.push_back(std::move(scene));
  }
  const sf::game::GameplaySession gameplay{mission};
  const auto *player_hmd =
      std::get_if<sf::assets::HmdModel>(&gameplay.playerModel().geometry);
  if (player_hmd == nullptr) {
    throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                          "Player model is not an HMD"};
  }
  const sf::game::ActorAnimationBank actor_animations{
      mission.characterAnimations(), player_hmd->parts().size()};
  static_cast<void>(actor_animations);
  std::size_t animation_clip_count{};
  std::size_t animation_frame_count{};
  std::size_t walking_root_frames{};
  std::size_t running_root_frames{};
  std::int64_t walking_root_distance{};
  std::int64_t running_root_distance{};
  for (const auto &entry : mission.characterAnimations().entries()) {
    if (!entry.name.ends_with(".HAN") && !entry.name.ends_with(".LWR") &&
        !entry.name.ends_with(".UPR")) {
      continue;
    }
    const auto clip = sf::assets::HmdAnimationClip::parse(
        mission.characterAnimations().file(entry.name),
        player_hmd->parts().size());
    ++animation_clip_count;
    animation_frame_count += clip.frames().size();
    if (entry.name == "WK0.LWR" || entry.name == "RN0.LWR") {
      auto distance = std::int64_t{};
      for (const auto &frame : clip.rootMotion()) {
        distance += frame.z;
      }
      if (entry.name == "WK0.LWR") {
        walking_root_frames = clip.rootMotion().size();
        walking_root_distance = distance;
      } else {
        running_root_frames = clip.rootMotion().size();
        running_root_distance = distance;
      }
    }
  }
  const auto hmd_model_count = static_cast<std::size_t>(std::ranges::count_if(
      gameplay.objectModels(), [](const sf::game::ObjectModel &model) {
        return std::holds_alternative<sf::assets::HmdModel>(model.geometry);
      }));

  const auto vlf = mission.archive().file("VLF.RFF");
  const auto byte = [&vlf](std::size_t index) {
    return std::to_integer<std::uint32_t>(vlf[index]);
  };
  const auto vlf_mask =
      byte(0) | (byte(1) << 8U) | (byte(2) << 16U) | (byte(3) << 24U);
  auto vram_conflict_rooms = std::size_t{};
  std::vector<std::string> vram_conflict_names;
  std::vector<std::string> vram_conflict_details;
  std::vector<std::string> vram_alias_remap_names;
  std::vector<std::string> no_effect_page_names;
  auto minimum_free_effect_pages = std::numeric_limits<std::size_t>::max();
  constexpr std::array effect_pages{
      31U, 30U, 29U, 28U, 27U, 26U, 25U, 24U, 23U, 22U,
      15U, 14U, 13U, 12U, 11U, 9U,  8U,  7U,  6U,
  };
  for (std::size_t room = 0; room < world_scenes.size(); ++room) {
    struct SlotOwner {
      int page{-1};
      int bank{-4};
    };
    std::array<SlotOwner, 32> slots{};
    std::array<unsigned int, 32> remap{};
    for (unsigned int page = 0; page < remap.size(); ++page) {
      remap[page] = (page & 15U) < 6U ? page + 6U : page;
    }
    auto conflict = false;
    auto alias_remapped = false;
    std::vector<std::string> room_conflicts;
    const auto page_bytes = [&](unsigned int page,
                                int bank) -> std::span<const std::byte> {
      constexpr std::size_t texture_page_size = 64U * 256U * 2U;
      if (bank == -2) {
        const auto preceding = page == 0U ? 0U : vlf_mask & ((1U << page) - 1U);
        return vlf.subspan(static_cast<std::size_t>(std::popcount(preceding)) *
                               texture_page_size,
                           texture_page_size);
      }
      auto name = std::string{"TP"};
      if (page < 10U) {
        name.push_back('0');
      }
      name += std::to_string(page) + ".BIN";
      return mission.textureBank(static_cast<std::size_t>(bank)).file(name);
    };
    const auto require_page = [&](unsigned int page, int bank) {
      page &= 0x1fU;
      auto physical = remap[page];
      const auto source_bank = (vlf_mask & (1U << page)) != 0U ? -2 : bank;
      auto *owner = &slots[physical];
      if (owner->page >= 0 && (owner->page != static_cast<int>(page) ||
                               owner->bank != source_bank)) {
        constexpr unsigned int escape_page = 21U;
        if (slots[escape_page].page < 0) {
          remap[page] = escape_page;
          physical = escape_page;
          owner = &slots[physical];
          alias_remapped = true;
        } else {
          conflict = true;
          room_conflicts.push_back(
              "slot" + std::to_string(physical) + "=" +
              std::to_string(owner->page) + "/" + std::to_string(owner->bank) +
              " vs " + std::to_string(page) + "/" +
              std::to_string(source_bank) +
              (std::ranges::equal(
                   page_bytes(static_cast<unsigned int>(owner->page),
                              owner->bank),
                   page_bytes(page, source_bank))
                   ? " equal"
                   : " different"));
          return;
        }
      }
      *owner = SlotOwner{static_cast<int>(page), source_bank};
    };
    const auto require_mask = [&](std::uint32_t mask, int bank) {
      for (unsigned int page = 0; page < 32U; ++page) {
        if ((mask & (1U << page)) != 0U) {
          require_page(page, bank);
        }
      }
    };
    std::vector<std::uint16_t> active_rooms{static_cast<std::uint16_t>(room)};
    for (const auto model : mission.layout().visibility(room).active_models) {
      if (std::ranges::find(active_rooms, model) == active_rooms.end()) {
        active_rooms.push_back(model);
      }
    }
    for (const auto model : active_rooms) {
      const auto &scene = world_scenes[model];
      require_mask(scene.texturePageMask(), scene.textureBank());
    }
    const auto object_bank = static_cast<int>(world_scenes[room].textureBank());
    const auto require_geometry =
        [&](const sf::game::ObjectGeometry &geometry) {
          if (const auto *gmd = std::get_if<sf::assets::GmdModel>(&geometry)) {
            require_mask(gmd->texturePageMask(), object_bank);
          } else if (const auto *hmd =
                         std::get_if<sf::assets::HmdModel>(&geometry)) {
            require_mask(hmd->texturePageMask(), object_bank);
          } else if (const auto *emd =
                         std::get_if<sf::assets::EmdScene>(&geometry)) {
            require_mask(emd->texturePageMask(), emd->textureBank());
          }
        };
    for (const auto active_room : active_rooms) {
      for (const auto source : mission.objects().objectsInRoom(active_room)) {
        const auto object = std::ranges::find_if(
            gameplay.objects(),
            [source](const sf::game::SceneObject &candidate) {
              return candidate.source_index == source;
            });
        if (object != gameplay.objects().end()) {
          require_geometry(gameplay.objectModels()[object->model].geometry);
        }
      }
    }
    require_geometry(gameplay.playerModel().geometry);
    if (const auto *weapon =
            gameplay.weaponModel(gameplay.hud().inventory().current())) {
      require_geometry(weapon->geometry);
    }
    const auto free_pages = static_cast<std::size_t>(
        std::ranges::count_if(effect_pages, [&slots](unsigned int page) {
          return slots[page].page < 0;
        }));
    minimum_free_effect_pages = std::min(minimum_free_effect_pages, free_pages);
    vram_conflict_rooms += conflict ? 1U : 0U;
    if (conflict) {
      vram_conflict_names.push_back(std::to_string(room) + ":" +
                                    mission.worldModels().entries()[room].name);
      vram_conflict_details.push_back(std::to_string(room) + ":" +
                                      room_conflicts.front());
    }
    if (alias_remapped) {
      vram_alias_remap_names.push_back(
          std::to_string(room) + ":" +
          mission.worldModels().entries()[room].name);
    }
    if (free_pages == 0U) {
      no_effect_page_names.push_back(
          std::to_string(room) + ":" +
          mission.worldModels().entries()[room].name);
    }
  }

  std::cout << "Mission:      " << definition.index << " - " << definition.title
            << '\n'
            << "Resource:     " << definition.resource_name << '\n'
            << "Overlay:      " << definition.overlay_name << '\n'
            << "Opening:      " << definition.opening_movie_path << '\n'
            << "FOG files:    " << mission.archive().entries().size() << '\n'
            << "Textures:     " << mission.textureFileCount() << '\n'
            << "World models: " << mission.worldModelCount() << '\n'
            << "EMD sections: " << section_count << '\n'
            << "EMD vertices: " << vertex_count << '\n'
            << "EMD polygons: " << polygon_count << '\n'
            << "Native objects: " << gameplay.objects().size() << '\n'
            << "Active objects: " << gameplay.activeObjects().size() << '\n'
            << "HMD models:     " << hmd_model_count << '\n'
            << "VRAM conflicts: " << vram_conflict_rooms << '\n'
            << "VRAM alias remaps: " << vram_alias_remap_names.size() << '\n'
            << "Min free effect pages: " << minimum_free_effect_pages << "\n\n"
            << "Actor clips:    " << animation_clip_count << " ("
            << animation_frame_count << " frames validated)\n"
            << "Root motion:    WK0 " << walking_root_frames << "/"
            << walking_root_distance << ", RN0 " << running_root_frames << "/"
            << running_root_distance << " (frames/world units)\n\n"
            << "name,start_sector,sector_count,size\n";
  std::cout << "Object definitions:\n";
  for (std::size_t index = 0; index < mission.objects().definitions().size();
       ++index) {
    const auto &object_definition = mission.objects().definitions()[index];
    const auto object_count = static_cast<std::size_t>(
        std::ranges::count_if(mission.objects().objects(),
                              [index](const sf::assets::MissionObject &object) {
                                return object.type == index;
                              }));
    std::cout << index << ",0x" << std::hex << object_definition.class_id
              << std::dec << ',' << object_count << ','
              << object_definition.primary_model << ','
              << object_definition.secondary_model << '\n';
  }
  std::cout << "Mission objects:\n";
  for (std::size_t index = 0; index < mission.objects().objects().size();
       ++index) {
    const auto &object = mission.objects().objects()[index];
    const auto &object_definition = mission.objects().definition(object.type);
    auto room = std::numeric_limits<std::size_t>::max();
    for (std::size_t candidate = 0; candidate < world_scenes.size();
         ++candidate) {
      if (std::ranges::find(mission.objects().objectsInRoom(candidate),
                            index) !=
          mission.objects().objectsInRoom(candidate).end()) {
        room = candidate;
        break;
      }
    }
    std::cout << index << ','
              << (room == std::numeric_limits<std::size_t>::max()
                      ? -1
                      : static_cast<int>(room))
              << ",0x" << std::hex << object_definition.class_id << std::dec
              << ',' << object_definition.primary_model << ','
              << object_definition.secondary_model << ','
              << object.maximum_health << ',' << object.health << ",0x"
              << std::hex << object.attributes << ",0x" << object.ai_parameter
              << ",0x" << object.path_data_offset << std::dec << ','
              << object.linked_object << ',' << object.transform.x << ','
              << object.transform.y << ',' << object.transform.z << '\n';
  }
  std::cout << "Special effects:\n";
  for (const auto &entry : mission.specialEffects().entries()) {
    std::cout << entry.name << ',' << entry.size << '\n';
  }
  if (!vram_conflict_names.empty() || !no_effect_page_names.empty()) {
    std::cout << "VRAM conflict rooms:";
    for (const auto &name : vram_conflict_names) {
      std::cout << ' ' << name;
    }
    std::cout << "\nVRAM conflict details:";
    for (const auto &detail : vram_conflict_details) {
      std::cout << ' ' << detail;
    }
    std::cout << "\nNo CFIRE scratch page rooms:";
    for (const auto &name : no_effect_page_names) {
      std::cout << ' ' << name;
    }
    std::cout << "\n\n";
  }
  if (!vram_alias_remap_names.empty()) {
    std::cout << "VRAM alias-remap rooms:";
    for (const auto &name : vram_alias_remap_names) {
      std::cout << ' ' << name;
    }
    std::cout << "\n\n";
  }
  for (const auto &entry : mission.archive().entries()) {
    std::cout << entry.name << ',' << entry.start_sector << ','
              << entry.sector_count << ',' << entry.size << '\n';
  }
  return 0;
}

int mapFunctions(const char *cue_path, const char *output_path) {
  const auto disc = openDisc(cue_path);
  const auto &executable = disc.executable();
  const auto &header = executable.header();
  const auto candidates = sf::psx::discoverFunctionCandidates(
      executable.text(), header.text_address, header.initial_pc);

  std::ofstream output{std::filesystem::path{output_path}, std::ios::trunc};
  if (!output) {
    throw sf::core::Error{sf::core::ErrorCode::io,
                          "Cannot open function-map output"};
  }
  output << "address,static_call_sites\n" << std::hex << std::uppercase;
  for (const auto &candidate : candidates) {
    output << "0x" << candidate.address << ',' << std::dec
           << candidate.static_call_count << '\n'
           << std::hex;
  }
  if (!output) {
    throw sf::core::Error{sf::core::ErrorCode::io,
                          "Failed to write function map"};
  }
  std::cout << "Wrote " << candidates.size() << " function seeds to "
            << output_path << '\n';
  return 0;
}

int probeLegacyVm(const char *cue_path) {
  auto disc = openDisc(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "Legacy VM probe requires Syphon Filter USA v1.1"};
  }

  // Original SCUS fixed-point multiply routine, used as a side-effect-free
  // proof target.
  constexpr std::uint32_t fixed_multiply_address = 0x800c6d4cU;
  constexpr std::array cases{
      std::array<std::int32_t, 2>{4096, 8192},
      std::array<std::int32_t, 2>{-4096, 8192},
      std::array<std::int32_t, 2>{12345, -2345},
      std::array<std::int32_t, 2>{-32767, -8191},
  };

  sf::game::LegacyGameplayVm vm{disc.executable()};
  std::uint64_t total_instructions{};
  for (const auto &values : cases) {
    const std::array arguments{
        std::bit_cast<std::uint32_t>(values[0]),
        std::bit_cast<std::uint32_t>(values[1]),
    };
    const auto result = vm.invoke(fixed_multiply_address, arguments, 64U);
    if (!result.completed()) {
      std::cerr << "LegacyGameplayVM stopped at 0x" << std::hex
                << std::uppercase << result.execution.pc << ": "
                << sf::psx::toString(result.execution.reason) << '\n';
      return 3;
    }
    const auto expected = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(values[0]) * values[1]) / 4096);
    if (std::bit_cast<std::int32_t>(result.return_value) != expected) {
      std::cerr << "LegacyGameplayVM result mismatch for " << values[0] << " * "
                << values[1] << '\n';
      return 4;
    }
    total_instructions += result.execution.instructions;
  }

  constexpr std::uint32_t mission_overlay_address = 0x80146630U;
  constexpr std::uint32_t mission_overlay_bootstrap = 0x80146c18U;
  const auto mission = sf::game::MissionPackage::loadFirst(disc);
  if (!vm.loadOverlay(
          mission_overlay_address,
          mission.archive().file(mission.definition().overlay_name))) {
    throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                          "Mission overlay does not fit LegacyGameplayVM RAM"};
  }
  const auto overlay_result =
      vm.invoke(mission_overlay_bootstrap, {}, 100'000U);
  if (!overlay_result.completed()) {
    std::cerr << "LegacyGameplayVM mission bootstrap stopped at 0x" << std::hex
              << std::uppercase << overlay_result.execution.pc << ": "
              << sf::psx::toString(overlay_result.execution.reason) << '\n';
    return 5;
  }
  total_instructions += overlay_result.execution.instructions;

  std::cout << "LegacyGameplayVM SCUS probe passed: " << cases.size()
            << " math cases + SUBWAY.OVL bootstrap, " << total_instructions
            << " instructions\n";
  return 0;
}

int probeLegacyCd(const char *cue_path) {
  auto disc = openDisc(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "Legacy CD probe requires Syphon Filter USA v1.1"};
  }

  const auto mission = sf::game::MissionPackage::loadFirst(disc);
  const auto &legacy_image = mission.legacyImage();
  sf::game::LegacyGameplayVm vm{legacy_image.executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(legacy_image.createVirtualCd());
  const auto bootstrap = vm.bootstrapFirstMission();
  if (!bootstrap.completed()) {
    std::cerr << "Legacy CD bootstrap failed at phase "
              << static_cast<unsigned int>(bootstrap.phase) << '\n';
    return 3;
  }

  constexpr std::uint32_t overlay_address = 0x80146630U;
  constexpr std::uint32_t bootstrap_offset = 0x5e8U;
  constexpr std::size_t code_probe_size = 64U;
  const auto overlay =
      mission.archive().file(mission.definition().overlay_name);
  if (overlay.size() < bootstrap_offset + code_probe_size) {
    return 4;
  }
  std::array<std::byte, code_probe_size> guest_code{};
  if (!vm.runtime().copyBytes(overlay_address + bootstrap_offset, guest_code) ||
      !std::ranges::equal(guest_code,
                          overlay.subspan(bootstrap_offset, code_probe_size))) {
    std::cerr << "SUBWAY.OVL code differs after CD/DMA3 loading\n";
    return 5;
  }

  const auto &machine = vm.machine();
  const auto cdrom = machine.cdrom().captureState();
  const auto dma3_control = machine.dma().chcr(sf::psx::DmaChannel::cdrom);
  constexpr std::uint16_t cdrom_irq = 1U << 2U;
  if (machine.currentTick() == 0U || cdrom.mode != 0xa0U ||
      cdrom.current_lba == 0U || cdrom.interrupt_flags != 0U ||
      cdrom.command_event.pending != 0U || cdrom.sector_event.pending != 0U ||
      (machine.interrupts().status() & cdrom_irq) != 0U ||
      machine.dma().madr(sf::psx::DmaChannel::cdrom) == 0U ||
      machine.dma().bcr(sf::psx::DmaChannel::cdrom) != 0x00010200U ||
      (dma3_control & (1U << 24U)) != 0U ||
      !machine.validateState(machine.captureState())) {
    std::cerr << "Legacy CD/DMA3 did not reach a clean hardware boundary\n";
    return 6;
  }

  std::cout << "Legacy CD probe passed: SUBWAY.OVL via CD-ROM/DMA3, lba="
            << cdrom.current_lba << ", ticks=" << machine.currentTick() << '\n';
  return 0;
}

int probeLegacyLoop(const char *cue_path) {
  auto disc = openDisc(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "Legacy loop probe requires Syphon Filter USA v1.1"};
  }

  const auto mission = sf::game::MissionPackage::loadFirst(disc);
  const auto &legacy_image = mission.legacyImage();
  sf::game::LegacyGameplayVm vm{legacy_image.executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(legacy_image.createVirtualCd());

  constexpr std::uint32_t native_gpu_boundary = 0x800e6e74U;
  constexpr std::uint64_t boot_budget = 20'000'000U;
  const auto first =
      vm.runCurrentPcUntilHostBoundary(native_gpu_boundary, boot_budget);
  if (!first.stoppedAtHostBoundary()) {
    std::cerr << "Continuous guest boot stopped: "
              << sf::psx::toString(first.execution.reason) << ", pc=0x"
              << std::hex << first.execution.pc << std::dec
              << ", instructions=" << first.execution.instructions
              << ", vector-call=0x" << std::hex << vm.runtime().state().gpr[9]
              << ", a0=0x" << vm.runtime().state().gpr[4] << ", a1=0x"
              << vm.runtime().state().gpr[5] << std::dec << '\n';
    return 3;
  }
  struct FrameCounters {
    std::uint32_t vblank{};
    std::uint32_t system_clock{};
    std::uint32_t gameplay_frame{};
  };
  const auto read_counters = [&vm](FrameCounters &counters) {
    return vm.runtime().read32(0x8010f378U, counters.vblank) &&
           vm.runtime().read32(0x801169a4U, counters.system_clock) &&
           vm.runtime().read32(0x80116a88U, counters.gameplay_frame);
  };

  const auto first_frame = vm.runtime().state();
  FrameCounters initial_counters{};
  if (!read_counters(initial_counters)) {
    return 4;
  }
  auto boundary_frame = first_frame;
  auto previous_tick = vm.machine().currentTick();
  auto total_instructions = first.execution.instructions;
  std::uint32_t boundary_count = 1U;
  constexpr std::uint32_t maximum_boundaries = 8U;
  for (; boundary_count < maximum_boundaries; ++boundary_count) {
    const auto submit = vm.resumeCurrentPc(1U);
    const auto after_submit = vm.runtime().state();
    if (submit.host_calls != 1U ||
        submit.execution.reason !=
            sf::psx::R3000StopReason::instruction_budget ||
        after_submit.pc != boundary_frame.gpr[31] ||
        after_submit.gpr[29] != boundary_frame.gpr[29]) {
      return 4;
    }

    vm.machine().pulseVBlank();
    if ((vm.machine().interrupts().status() & 1U) == 0U) {
      return 4;
    }
    const auto next =
        vm.runCurrentPcUntilHostBoundary(native_gpu_boundary, boot_budget);
    if (!next.stoppedAtHostBoundary()) {
      std::cerr << "Continuous guest loop stopped: "
                << sf::psx::toString(next.execution.reason) << ", pc=0x"
                << std::hex << next.execution.pc << std::dec
                << ", instructions=" << next.execution.instructions << '\n';
      return 5;
    }

    const auto next_tick = vm.machine().currentTick();
    const auto next_frame = vm.runtime().state();
    FrameCounters counters{};
    if (!read_counters(counters) || next_tick <= previous_tick ||
        next.execution.instructions == 0U ||
        next_frame.gpr[31] != first_frame.gpr[31]) {
      return 6;
    }
    total_instructions += next.execution.instructions;
    if (counters.vblank != initial_counters.vblank ||
        counters.system_clock != initial_counters.system_clock ||
        counters.gameplay_frame != initial_counters.gameplay_frame) {
      std::cout << "Legacy continuous loop passed: " << boundary_count + 1U
                << " native GPU boundaries, " << total_instructions
                << " guest instructions, vblank=" << initial_counters.vblank
                << "/" << counters.vblank
                << ", clock=" << initial_counters.system_clock << "/"
                << counters.system_clock
                << ", frame=" << initial_counters.gameplay_frame << "/"
                << counters.gameplay_frame << '\n';
      return 0;
    }
    boundary_frame = next_frame;
    previous_tick = next_tick;
  }

  std::cerr << "Continuous loop reached " << boundary_count
            << " GPU boundaries without advancing guest frame counters\n";
  return 6;
}

int probeLegacyBootstrap(const char *cue_path) {
  auto disc = openDisc(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{
        sf::core::ErrorCode::unsupported,
        "Legacy bootstrap probe requires Syphon Filter USA v1.1"};
  }
  const auto mission = sf::game::MissionPackage::loadFirst(disc);
  const auto &legacy_image = mission.legacyImage();
  auto virtual_cd = legacy_image.createVirtualCd();
  sf::game::LegacyGameplayVm vm{legacy_image.executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(std::move(virtual_cd));

  const auto report_phase =
      [&vm](std::string_view phase,
            const sf::game::LegacyGameplayVmResult &result) {
        std::cout << "legacy-bootstrap-" << phase << ": "
                  << sf::psx::toString(result.execution.reason)
                  << ", instructions=" << result.execution.instructions
                  << ", host-calls=" << result.host_calls << ", pc=0x"
                  << std::hex << std::uppercase << result.execution.pc
                  << ", instruction=0x" << result.execution.instruction
                  << ", ra=0x" << vm.runtime().state().gpr[31] << ", sp=0x"
                  << vm.runtime().state().gpr[29] << ", a0=0x"
                  << vm.runtime().state().gpr[4] << ", a1=0x"
                  << vm.runtime().state().gpr[5] << ", a2=0x"
                  << vm.runtime().state().gpr[6] << ", a3=0x"
                  << vm.runtime().state().gpr[7] << ", t1=0x"
                  << vm.runtime().state().gpr[9] << ", t2=0x"
                  << vm.runtime().state().gpr[10] << std::dec << '\n';
      };

  const auto bootstrap = vm.bootstrapFirstMission();
  report_phase("first-mission", bootstrap.execution);
  if (!bootstrap.completed()) {
    std::cout << "legacy-bootstrap-failed-phase="
              << static_cast<unsigned int>(bootstrap.phase)
              << ", bridge-fault=" << bootstrap.bridge_fault << '\n';
    return 11;
  }

  const auto activate_opening_cbdc = vm.invoke(0x8005fd04U, std::array{6U});
  if (!activate_opening_cbdc.completed()) {
    report_phase("activate-opening-cbdc", activate_opening_cbdc);
    return 12;
  }
  constexpr std::array mission_state_addresses{
      0x80115c78U, 0x80115c7cU, 0x80115c74U, 0x80102aa8U, 0x80116a88U,
      0x801169a4U, 0x801163b4U, 0x80116a20U, 0x80115cd4U, 0x80116958U,
      0x80115cccU, 0x80115d84U, 0x8015469cU, 0x80116af0U, 0x80116b9cU,
      0x80116ab0U, 0x801169d4U, 0x80130c8cU, 0x80116c68U, 0x8011775cU,
  };
  std::cout << "legacy-bootstrap-mission-state:";
  for (const auto address : mission_state_addresses) {
    std::uint32_t value{};
    if (!vm.runtime().read32(address, value)) {
      return 17;
    }
    std::cout << " [0x" << std::hex << std::uppercase << address << "]=0x"
              << value;
  }
  std::cout << std::dec << '\n';
  std::uint32_t pending_events{};
  if (!vm.runtime().read32(0x80116c68U, pending_events)) {
    return 17;
  }
  std::cout << "legacy-bootstrap-pending-events:";
  for (std::uint32_t index = 0U; index < pending_events; ++index) {
    const auto entry = 0x80116c6cU + index * 0x1cU;
    std::uint16_t event{};
    std::uint16_t priority{};
    std::uint32_t source{};
    std::uint32_t target{};
    if (!vm.runtime().read16(entry, event) ||
        !vm.runtime().read16(entry + 2U, priority) ||
        !vm.runtime().read32(entry + 4U, source) ||
        !vm.runtime().read32(entry + 8U, target)) {
      return 17;
    }
    std::cout << " [" << index << ":e" << event << ",p" << priority << ",s"
              << static_cast<std::int32_t>(source) << ",t"
              << static_cast<std::int32_t>(target) << ']';
  }
  std::cout << '\n';

  const auto report_bridge = [&vm](std::uint32_t frame) {
    const auto bridge = vm.readBridgeState();
    if (!bridge) {
      return false;
    }
    std::uint32_t state{};
    std::uint32_t state_depth{};
    static_cast<void>(vm.runtime().read32(0x80115c78U, state));
    static_cast<void>(vm.runtime().read32(0x80115c74U, state_depth));
    std::cout << "legacy-bridge-frame-" << frame << ": state=" << state << '/'
              << state_depth << ": eye=(" << bridge->camera.eye.x << ','
              << bridge->camera.eye.y << ',' << bridge->camera.eye.z
              << "), target=(" << bridge->camera.target.x << ','
              << bridge->camera.target.y << ',' << bridge->camera.target.z
              << "), projection=" << bridge->camera.projection
              << ", native-projection="
              << bridge->camera.projectionForDisplayWidth(384)
              << ", fov=" << bridge->camera.fov_raw
              << ", fade=" << bridge->fade.current << '/'
              << static_cast<unsigned int>(bridge->fade.floor)
              << " step=" << bridge->fade.step << " cb=0x" << std::hex
              << bridge->fade.callback << std::dec << ", opacity=" << std::fixed
              << std::setprecision(3) << bridge->fade.blackOpacity()
              << std::defaultfloat << ", objects=" << bridge->objects.size();
    constexpr std::array opening_slots{
        35U,  36U,  57U,  61U,  64U,  83U,  172U,
        173U, 184U, 350U, 351U, 352U, 353U, 354U,
    };
    for (const auto slot : opening_slots) {
      if (slot >= bridge->objects.size()) {
        continue;
      }
      const auto &object = bridge->objects[slot];
      std::uint32_t records{};
      std::uint32_t source{};
      std::uint32_t source_path{};
      std::uint32_t instance{};
      std::uint32_t actor_state{};
      static_cast<void>(vm.runtime().read32(0x80115cccU, records));
      static_cast<void>(
          vm.runtime().read32(records + slot * 0x4cU + 0x2cU, source));
      if (source != 0U) {
        static_cast<void>(vm.runtime().read32(source + 0x2cU, source_path));
      }
      static_cast<void>(
          vm.runtime().read32(records + slot * 0x4cU + 0x34U, instance));
      if (instance != 0U) {
        static_cast<void>(vm.runtime().read32(instance + 0x1cU, actor_state));
      }
      std::cout << " [" << slot << ":c" << object.class_id << ",r"
                << object.resident << ",hp" << object.health << ",a0x"
                << std::hex << object.attributes << std::dec << ",arg"
                << object.parameter << ",link" << object.linked_slot << ",src0x"
                << std::hex << source << ",path0x" << source_path << ",inst0x"
                << instance << ",root0x" << object.root_node << ",mot0x"
                << object.motion_controller << ",pres0x"
                << object.presentation_controller << ",ai0x" << actor_state
                << ",aif0x" << object.ai_flags << ",fire" << std::dec
                << static_cast<unsigned int>(object.ai_fire_latch) << std::hex
                << ",route0x" << object.path_pointer << ",node" << std::dec
                << static_cast<unsigned int>(object.ai_route_node) << ",rf0x"
                << std::hex << object.ai_route_flags << ",pose0x"
                << object.pose_flags << std::dec << ",pe"
                << static_cast<unsigned int>(object.presentation_enabled)
                << ",pm" << static_cast<unsigned int>(object.presentation_mode)
                << ",sim" << object.simulated << ",t" << object.target_slot
                << ",am" << static_cast<unsigned int>(object.ai_mode) << ",ac"
                << static_cast<unsigned int>(object.ai_combat_mode) << ",p("
                << object.position.x << ',' << object.position.y << ','
                << object.position.z << ")]";
    }
    if (frame == 0U || frame == 120U || frame == 193U || frame == 208U) {
      for (const auto &object : bridge->objects) {
        if (object.class_id != 0x35 || !object.resident) {
          continue;
        }
        std::cout << " [CHEMO" << object.slot << ":hp" << object.health
                  << ",a0x" << std::hex << object.attributes << std::dec
                  << ",arg" << object.parameter << ",link" << object.linked_slot
                  << ",p(" << object.position.x << ',' << object.position.y
                  << ',' << object.position.z << ")]";
      }
    }
    std::cout << '\n';
    return true;
  };
  constexpr std::array report_frames{
      0U,   1U,   10U,  30U,  60U,  120U, 193U, 199U, 200U,
      201U, 202U, 203U, 204U, 205U, 206U, 207U, 208U, 209U,
      210U, 211U, 212U, 220U, 250U, 300U, 400U, 499U,
  };
  constexpr std::uint32_t retail_opening_updates = 209U;
  constexpr std::uint32_t retail_followup_updates = 180U;
  // GameplaySession owns the already-rendered direct frame 0 snapshot, then
  // advances the guest once per retail 20 Hz simulation update.
  constexpr std::uint32_t matching_direct_frame = retail_opening_updates;
  constexpr std::uint32_t matching_followup_direct_frame =
      matching_direct_frame + retail_followup_updates;
  std::optional<sf::game::LegacyGameplayBridgeState> matching_guest_bridge;
  std::optional<sf::game::LegacyGameplayBridgeState>
      matching_followup_guest_bridge;
  std::optional<sf::game::LegacyObjectBridgeState> matching_guest_vehicle;
  bool native_driven{};
  auto previous_current_state = std::numeric_limits<std::uint32_t>::max();
  auto previous_next_state = std::numeric_limits<std::uint32_t>::max();
  for (std::uint32_t frame = 0U; frame < 500U; ++frame) {
    if (!vm.writeHostPadState(sf::game::LegacyHostPadState{})) {
      return 24;
    }
    std::uint32_t current_state{};
    if (!vm.runtime().read32(0x80115c78U, current_state)) {
      return 24;
    }
    const auto gameplay_state = current_state == 0U || current_state == 5U;
    const auto retail_frame = native_driven && gameplay_state
                                  ? vm.tickNativeDrivenGameplayFrame()
                                  : vm.tickRetailOuterFrame();
    if (!retail_frame.completed()) {
      std::uint32_t next_state{};
      static_cast<void>(vm.runtime().read32(0x80115c7cU, next_state));
      std::cout << "legacy-bootstrap-outer-state: before="
                << retail_frame.state_before
                << ", after=" << retail_frame.state_after
                << ", next=" << next_state
                << ", bridge-fault=" << retail_frame.bridge_fault
                << ", unsupported=" << retail_frame.unsupported_state << '\n';
      if (!retail_frame.guest_calls.empty()) {
        report_phase("outer-frame", retail_frame.guest_calls.back());
      }
      if (retail_frame.renderer_tail) {
        report_phase("retail-render-tail", *retail_frame.renderer_tail);
      }
      report_phase("retail-tail-callbacks",
                   retail_frame.platform_tail.delayed_callbacks);
      if (retail_frame.platform_tail.fade_callback) {
        report_phase("retail-tail-fade",
                     *retail_frame.platform_tail.fade_callback);
      }
      return 24;
    }
    std::uint32_t current_after{};
    std::uint32_t next_after{};
    std::uint32_t stack_depth{};
    std::uint32_t loader_callback{};
    std::uint32_t movie_callback{};
    std::uint32_t loader_ticks{};
    std::uint8_t overlay_ready{};
    std::uint8_t fade_ready{};
    if (!vm.runtime().read32(0x80115c78U, current_after) ||
        !vm.runtime().read32(0x80115c7cU, next_after) ||
        !vm.runtime().read32(0x80115c74U, stack_depth) ||
        !vm.runtime().read32(0x80116b04U, loader_callback) ||
        !vm.runtime().read32(0x80115c80U, movie_callback) ||
        !vm.runtime().read32(0x80116978U, loader_ticks) ||
        !vm.runtime().read8(0x801169f0U, overlay_ready) ||
        !vm.runtime().read8(0x80116940U, fade_ready)) {
      return 25;
    }
    if (current_after != previous_current_state ||
        next_after != previous_next_state) {
      std::array<std::uint32_t, 4U> state_stack{};
      std::array<std::uint32_t, 4U> state7_words{};
      std::array<char, 17U> loader_name{};
      for (std::size_t index = 0U; index < state_stack.size(); ++index) {
        if (!vm.runtime().read32(0x80102aa4U +
                                     static_cast<std::uint32_t>(index * 4U),
                                 state_stack[index]) ||
            !vm.runtime().read32(0x80145accU +
                                     static_cast<std::uint32_t>(index * 4U),
                                 state7_words[index])) {
          return 25;
        }
      }
      for (std::size_t index = 0U; index + 1U < loader_name.size(); ++index) {
        std::uint8_t character{};
        if (!vm.runtime().read8(0x80115ca8U + static_cast<std::uint32_t>(index),
                                character)) {
          return 25;
        }
        loader_name[index] = static_cast<char>(character);
        if (character == 0U) {
          break;
        }
      }
      std::cout << "legacy-state-frame-" << frame
                << ": call=" << retail_frame.state_before << '/'
                << retail_frame.state_after << ", live=" << current_after << '/'
                << next_after << ", depth=" << stack_depth
                << ", loader=" << loader_ticks << '/'
                << static_cast<unsigned int>(overlay_ready) << '/'
                << static_cast<unsigned int>(fade_ready) << ", callbacks=0x"
                << std::hex << loader_callback << "/0x" << movie_callback
                << ", stack=" << state_stack[0] << '/' << state_stack[1] << '/'
                << state_stack[2] << '/' << state_stack[3]
                << ", state7=" << state7_words[0] << '/' << state7_words[1]
                << '/' << state7_words[2] << '/' << state7_words[3] << std::dec
                << ", name=" << loader_name.data() << '\n';
      previous_current_state = current_after;
      previous_next_state = next_after;
    }
    if (std::ranges::find(report_frames, frame) != report_frames.end() &&
        !report_bridge(frame)) {
      return 26;
    }
    const auto bridge = vm.readBridgeState();
    if (!bridge || bridge->objects.size() <= 35U) {
      return 26;
    }
    if (frame == matching_direct_frame) {
      if (bridge->objects.size() <= 352U) {
        return 26;
      }
      matching_guest_bridge = *bridge;
      matching_guest_vehicle = bridge->objects[57U];
    }
    if (frame == matching_followup_direct_frame) {
      if (bridge->objects.size() <= 354U) {
        return 26;
      }
      matching_followup_guest_bridge = *bridge;
    }
    native_driven = native_driven || bridge->objects[35U].health <= 0;
  }
  if (!matching_guest_bridge || !matching_followup_guest_bridge ||
      !matching_guest_vehicle) {
    return 26;
  }

  sf::game::GameplaySession gameplay{mission};
  const auto initial_camera = gameplay.camera();
  if (initial_camera.x != 2372.0 || initial_camera.y != -3206.0 ||
      initial_camera.z != 5977.0 || gameplay.mapFade() != 0U) {
    std::cout << "native-opening-initial-bridge-mismatch: eye=("
              << initial_camera.x << ',' << initial_camera.y << ','
              << initial_camera.z
              << "), fade=" << static_cast<unsigned int>(gameplay.mapFade())
              << ", cinematic=" << gameplay.cinematic()
              << ", complete=" << gameplay.missionComplete() << '\n';
    return 26;
  }
  bool saw_both_cbdc_during_rail{};
  bool saw_both_hostiles_during_rail{};
  bool saw_crouched_cbdc_motion{};
  bool saw_guest_expl_particles{};
  bool retained_authored_glit_rotation{true};
  const auto &authored_glit_rotation =
      mission.objects().objects()[74U].transform.rotation;
  for (std::uint32_t update = 0U; update < retail_opening_updates; ++update) {
    gameplay.update(sf::game::GameplayInput{});
    gameplay.advanceAnimationClock();
    saw_guest_expl_particles =
        saw_guest_expl_particles ||
        std::ranges::any_of(gameplay.legacyExplParticles(),
                            [](const sf::game::LegacyExplParticle &particle) {
                              return particle.scale_byte != 0U &&
                                     particle.frame <= 7U &&
                                     (particle.red != 0U ||
                                      particle.green != 0U ||
                                      particle.blue != 0U);
                            });
    const auto glit = std::ranges::find_if(
        gameplay.objects(), [](const sf::game::SceneObject &object) {
          return object.source_index == 74U;
        });
    retained_authored_glit_rotation =
        retained_authored_glit_rotation && glit != gameplay.objects().end() &&
        glit->transform.rotation == authored_glit_rotation;
    if (!gameplay.cinematic()) {
      continue;
    }
    std::size_t cbdc_count{};
    std::size_t hostile_count{};
    for (std::uint16_t object = 0U; object < gameplay.objects().size();
         ++object) {
      const auto *state = gameplay.npcState(object);
      if (state == nullptr || state->scripted_opening_lane >= 2U) {
        continue;
      }
      if (state->scripted_intro_agent) {
        ++cbdc_count;
        saw_crouched_cbdc_motion =
            saw_crouched_cbdc_motion || state->scripted_low_locomotion;
      } else if (state->disposition == sf::game::NpcDisposition::hostile) {
        ++hostile_count;
      }
    }
    saw_both_cbdc_during_rail = saw_both_cbdc_during_rail || cbdc_count == 2U;
    saw_both_hostiles_during_rail =
        saw_both_hostiles_during_rail || hostile_count == 2U;
  }

  std::array<const sf::game::NpcState *, 2U> opening_hostiles{};
  std::array<const sf::game::NpcState *, 2U> opening_cbdc{};
  for (std::uint16_t object = 0U; object < gameplay.objects().size();
       ++object) {
    const auto *state = gameplay.npcState(object);
    if (state == nullptr ||
        state->scripted_opening_lane >= opening_hostiles.size()) {
      continue;
    }
    if (state->disposition == sf::game::NpcDisposition::hostile) {
      opening_hostiles[state->scripted_opening_lane] = state;
    } else if (state->scripted_intro_agent) {
      opening_cbdc[state->scripted_opening_lane] = state;
    }
  }
  const auto settled_vehicle = std::ranges::find_if(
      gameplay.objects(), [](const sf::game::SceneObject &object) {
        return object.source_index == 57U;
      });
  const auto vehicle_basis_matches_guest =
      matching_guest_vehicle->position.y !=
          std::numeric_limits<std::int32_t>::min() &&
      settled_vehicle != gameplay.objects().end() &&
      settled_vehicle->transform.x == matching_guest_vehicle->position.x &&
      settled_vehicle->transform.y == -matching_guest_vehicle->position.y &&
      settled_vehicle->transform.z == matching_guest_vehicle->position.z &&
      settled_vehicle->transform.rotation ==
          matching_guest_vehicle->guest_rotation;
  const auto actor_matches_guest =
      [&gameplay](const sf::game::NpcState *actor,
                  const sf::game::LegacyObjectBridgeState &guest) {
        const auto presented =
            guest.resident && (guest.presentation_controller == 0U ||
                               guest.presentation_enabled != 0U);
        if (!presented) {
          return actor == nullptr;
        }
        const auto exact_guest_pose =
            actor != nullptr && actor->object < gameplay.objects().size() &&
            guest.bone_matrix_count == sf::game::legacy_actor_bone_count &&
            gameplay.objects()[actor->object].legacy_hmd_bone_count ==
                sf::game::legacy_actor_bone_count;
        const auto legacy_pose_available =
            exact_guest_pose ||
            (actor != nullptr && actor->object < gameplay.objects().size() &&
             gameplay.objects()[actor->object].legacy_hmd_root_space);
        return actor != nullptr && actor->object < gameplay.objects().size() &&
               legacy_pose_available &&
               actor->health ==
                   static_cast<std::uint16_t>(std::max<int>(guest.health, 0)) &&
               actor->x == guest.position.x && actor->y == guest.position.y &&
               actor->z == guest.position.z;
      };
  // Static source 184 is lane 0. Retail allocates lane 1 in dynamic slot
  // 350 and the two CBDC actors in slots 352/351 respectively.
  const auto opening_actors_match_guest =
      actor_matches_guest(opening_hostiles[0],
                          matching_guest_bridge->objects[184U]) &&
      actor_matches_guest(opening_hostiles[1],
                          matching_guest_bridge->objects[350U]) &&
      actor_matches_guest(opening_cbdc[0],
                          matching_guest_bridge->objects[352U]) &&
      actor_matches_guest(opening_cbdc[1],
                          matching_guest_bridge->objects[351U]);
  if (gameplay.cinematic() || !saw_both_cbdc_during_rail ||
      !saw_both_hostiles_during_rail || !saw_crouched_cbdc_motion ||
      !saw_guest_expl_particles || !retained_authored_glit_rotation ||
      !vehicle_basis_matches_guest || !opening_actors_match_guest) {
    std::cout << "native-opening-handoff-mismatch: cinematic="
              << gameplay.cinematic()
              << ",rail-cbdc=" << saw_both_cbdc_during_rail
              << ",rail-hostiles=" << saw_both_hostiles_during_rail
              << ",crouch-motion=" << saw_crouched_cbdc_motion
              << ",guest-expl=" << saw_guest_expl_particles
              << ",glit-authored=" << retained_authored_glit_rotation
              << ",vehicle-basis=" << vehicle_basis_matches_guest
              << ",actors-match=" << opening_actors_match_guest;
    const auto report_actor = [](std::string_view name, const auto *actor) {
      std::cout << ", " << name << '=';
      if (actor == nullptr) {
        std::cout << "missing";
        return;
      }
      std::cout << std::setprecision(17) << '(' << actor->x << ',' << actor->y
                << ',' << actor->z << ')' << std::setprecision(6);
    };
    report_actor("hostile0", opening_hostiles[0]);
    report_actor("hostile1", opening_hostiles[1]);
    report_actor("cbdc0", opening_cbdc[0]);
    report_actor("cbdc1", opening_cbdc[1]);
    std::cout << '\n';
    return 27;
  }
  const auto static_184_active = std::ranges::any_of(
      gameplay.activeObjects(), [&gameplay](std::uint16_t object) {
        const auto *state = gameplay.npcState(object);
        return state != nullptr && state->source_index == 184U &&
               state->scripted_opening_lane == 0U;
      });
  std::cout << "native-opening-bridge: updates=" << retail_opening_updates
            << ", fade=" << static_cast<unsigned int>(gameplay.mapFade())
            << ", guest-expl=" << saw_guest_expl_particles
            << ", glit-authored=" << retained_authored_glit_rotation
            << ", vehicle-basis=" << vehicle_basis_matches_guest
            << ", actors-match=" << opening_actors_match_guest
            << ", static184=" << static_184_active;
  const auto report_actor = [](std::string_view name, const auto *actor) {
    std::cout << ", " << name << '=';
    if (actor == nullptr) {
      std::cout << "dead";
      return;
    }
    std::cout << '(' << actor->x << ',' << actor->y << ',' << actor->z << ')';
  };
  report_actor("hostile0", opening_hostiles[0]);
  report_actor("hostile1", opening_hostiles[1]);
  report_actor("cbdc0", opening_cbdc[0]);
  report_actor("cbdc1", opening_cbdc[1]);
  std::cout << '\n';
  const std::array followup_actor_objects{
      opening_hostiles[1]->object,
      opening_cbdc[0]->object,
      opening_cbdc[1]->object,
  };
  const std::array initial_shot_serials{
      opening_hostiles[1]->shot_serial,
      opening_cbdc[0]->shot_serial,
      opening_cbdc[1]->shot_serial,
  };
  const std::array initial_animation_ticks{
      opening_hostiles[1]->animation_tick,
      opening_cbdc[0]->animation_tick,
      opening_cbdc[1]->animation_tick,
  };
  std::array<bool, 3U> saw_followup_motion{};
  std::array<bool, 3U> saw_followup_animation{};
  std::array<bool, 3U> saw_followup_target{};
  std::array<bool, 3U> saw_followup_fire{};
  for (std::uint32_t update = 0U; update < retail_followup_updates; ++update) {
    gameplay.update(sf::game::GameplayInput{});
    gameplay.advanceAnimationClock();
    for (std::size_t actor = 0U; actor < followup_actor_objects.size();
         ++actor) {
      const auto *state = gameplay.npcState(followup_actor_objects[actor]);
      if (state == nullptr) {
        continue;
      }
      saw_followup_motion[actor] =
          saw_followup_motion[actor] || state->movement_distance > 1.0;
      saw_followup_animation[actor] =
          saw_followup_animation[actor] ||
          state->animation_tick != initial_animation_ticks[actor];
      saw_followup_target[actor] =
          saw_followup_target[actor] ||
          state->behavior == sf::game::NpcBehavior::attack;
      saw_followup_fire[actor] =
          saw_followup_fire[actor] ||
          state->shot_serial > initial_shot_serials[actor];
    }
  }
  const auto *followup_opening_hostile =
      gameplay.npcState(followup_actor_objects[0]);
  const auto *followup_cbdc0 = gameplay.npcState(followup_actor_objects[1]);
  const auto *followup_cbdc1 = gameplay.npcState(followup_actor_objects[2]);
  const auto &followup_guest = *matching_followup_guest_bridge;
  const auto presentation_matches_guest =
      [&actor_matches_guest](const sf::game::NpcState *actor,
                             const sf::game::LegacyObjectBridgeState &guest) {
        const auto expected_phase =
            guest.ai_fire_latch != 0U ? sf::game::NpcCombatPhase::burst
            : guest.has_target        ? sf::game::NpcCombatPhase::aim
                                      : sf::game::NpcCombatPhase::acquire;
        return actor_matches_guest(actor, guest) && actor != nullptr &&
               actor->behavior == sf::game::NpcBehavior::attack &&
               guest.has_target && actor->combat_phase == expected_phase &&
               actor->fire_animation_updates == guest.ai_fire_latch;
      };
  const sf::game::NpcState *followup_hostile{};
  for (std::uint16_t object = 0U; object < gameplay.objects().size();
       ++object) {
    const auto *actor = gameplay.npcState(object);
    if (actor == followup_opening_hostile ||
        !presentation_matches_guest(actor, followup_guest.objects[354U])) {
      continue;
    }
    followup_hostile = actor;
    break;
  }
  // At direct frame 389 the dead slot-350 opening lifetime and live slot 354
  // coexist. They must retain separate scene objects; slot 354 may not
  // overwrite the dedicated opening presentation.
  const auto opening_identity_preserved =
      actor_matches_guest(followup_opening_hostile,
                          followup_guest.objects[350U]) &&
      !actor_matches_guest(followup_opening_hostile,
                           followup_guest.objects[354U]);
  const auto followup_actors_match_guest =
      !followup_guest.objects[184U].alive() && opening_identity_preserved &&
      presentation_matches_guest(followup_hostile,
                                 followup_guest.objects[354U]) &&
      presentation_matches_guest(followup_cbdc0,
                                 followup_guest.objects[352U]) &&
      presentation_matches_guest(followup_cbdc1, followup_guest.objects[351U]);
  const auto all_saw = [](const std::array<bool, 3U> &values) {
    return std::ranges::all_of(values, std::identity{});
  };
  const auto followup_static_184_active = std::ranges::any_of(
      gameplay.activeObjects(), [&gameplay](std::uint16_t object) {
        const auto *state = gameplay.npcState(object);
        return state != nullptr && state->source_index == 184U &&
               state->scripted_opening_lane == 0U;
      });
  const auto followup_ok =
      followup_actors_match_guest && followup_static_184_active &&
      all_saw(saw_followup_motion) && all_saw(saw_followup_animation) &&
      all_saw(saw_followup_target) && all_saw(saw_followup_fire);
  std::cout << "native-opening-followup: updates=" << retail_followup_updates;
  report_actor("hostile0", static_cast<const sf::game::NpcState *>(nullptr));
  report_actor("hostile1", followup_hostile);
  report_actor("cbdc0", followup_cbdc0);
  report_actor("cbdc1", followup_cbdc1);
  std::cout << ", actors-match=" << followup_actors_match_guest
            << ", motion=" << all_saw(saw_followup_motion)
            << ", animation=" << all_saw(saw_followup_animation)
            << ", target=" << all_saw(saw_followup_target)
            << ", fire=" << all_saw(saw_followup_fire)
            << ", opening-identity=" << opening_identity_preserved
            << ", static184=" << followup_static_184_active << '\n';
  if (!followup_ok) {
    return 28;
  }
  return 0;
}

struct LegacyLevelActorIdentity {
  std::int16_t class_id{};
  std::uint32_t definition{};
  std::int32_t parameter{};
  sf::game::LegacyNativePoint authored_position;
  std::uint32_t path_pointer{};
};

struct LegacyLevelActorLifetime {
  LegacyLevelActorIdentity identity;
  std::uint32_t slot{};
  std::uint32_t generation{};
  std::uint32_t first_frame{};
  std::uint32_t last_frame{};
  std::int16_t start_health{};
  std::int16_t minimum_health{};
  std::int16_t end_health{};
  sf::game::LegacyNativePoint start_position;
  sf::game::LegacyNativePoint end_position;
  std::int16_t last_target{-1};
  std::uint64_t last_pose_fingerprint{};
  std::uint32_t target_changes{};
  std::uint32_t animation_changes{};
  std::uint32_t fire_frames{};
  std::uint32_t simulated_frames{};
  std::uint32_t exact_pose_frames{};
  std::uint32_t ground_frames{};
  std::uint32_t packed_ground_sentinel_frames{};
  std::uint32_t current_stagnant_combat_frames{};
  std::uint32_t longest_stagnant_combat_frames{};
  std::int64_t maximum_ground_delta{};
  bool saw_positive_health{};
  bool died{};
  bool retired{};
  bool saw_target{};
  bool moved{};
};

struct LegacyLevelSlotTrace {
  std::optional<std::size_t> lifetime;
  std::uint32_t generations{};
};

enum class LegacyLevelDriverStage : std::uint8_t {
  waiting_opening,
  failure_branch,
  clear_opening,
  trigger_256,
  passage_64,
  passage_65,
  trigger_257,
  intro_157,
  lock_140,
  bank_175,
  kravitch_174,
  radio_260,
  bomb_29,
  trigger_190,
  trigger_194,
  power_317,
  trigger_192,
  trigger_193,
  elevator_315,
  elevator_316,
  bomb_28,
  trigger_191,
  trigger_258,
  station_318,
  station_319,
  trigger_259,
  finale_30,
  complete,
};

std::string_view
legacyLevelDriverStageName(LegacyLevelDriverStage stage) noexcept {
  using enum LegacyLevelDriverStage;
  switch (stage) {
  case waiting_opening:
    return "waiting-opening";
  case failure_branch:
    return "failure-branch";
  case clear_opening:
    return "clear-opening";
  case trigger_256:
    return "trigger-256";
  case passage_64:
    return "passage-64";
  case passage_65:
    return "passage-65";
  case trigger_257:
    return "trigger-257";
  case intro_157:
    return "intro-157";
  case lock_140:
    return "lock-140";
  case bank_175:
    return "bank-175";
  case kravitch_174:
    return "kravitch-174";
  case radio_260:
    return "radio-260";
  case bomb_29:
    return "bomb-29";
  case trigger_190:
    return "trigger-190";
  case trigger_194:
    return "trigger-194";
  case power_317:
    return "power-317";
  case trigger_192:
    return "trigger-192";
  case trigger_193:
    return "trigger-193";
  case elevator_315:
    return "elevator-315";
  case elevator_316:
    return "elevator-316";
  case bomb_28:
    return "bomb-28";
  case trigger_191:
    return "trigger-191";
  case trigger_258:
    return "trigger-258";
  case station_318:
    return "station-318";
  case station_319:
    return "station-319";
  case trigger_259:
    return "trigger-259";
  case finale_30:
    return "finale-30";
  case complete:
    return "complete";
  }
  return "unknown";
}

bool sameLegacyPoint(const sf::game::LegacyNativePoint &lhs,
                     const sf::game::LegacyNativePoint &rhs) noexcept {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool sameLegacyActorIdentity(const LegacyLevelActorIdentity &lhs,
                             const LegacyLevelActorIdentity &rhs) noexcept {
  return lhs.class_id == rhs.class_id && lhs.definition == rhs.definition &&
         lhs.parameter == rhs.parameter &&
         sameLegacyPoint(lhs.authored_position, rhs.authored_position) &&
         lhs.path_pointer == rhs.path_pointer;
}

bool sameLegacyMissionState(
    const sf::game::LegacyMissionBridgeState &lhs,
    const sf::game::LegacyMissionBridgeState &rhs) noexcept {
  return lhs.player_slot == rhs.player_slot &&
         lhs.player_health == rhs.player_health &&
         lhs.player_armor == rhs.player_armor &&
         lhs.objective_count == rhs.objective_count &&
         lhs.parameter_count == rhs.parameter_count &&
         lhs.objective_texts == rhs.objective_texts &&
         lhs.parameter_texts == rhs.parameter_texts &&
         lhs.completed_objectives == rhs.completed_objectives &&
         lhs.failed_objectives == rhs.failed_objectives &&
         lhs.revealed_objectives == rhs.revealed_objectives &&
         lhs.notified_objectives == rhs.notified_objectives &&
         lhs.failed_parameters == rhs.failed_parameters &&
         lhs.parameter_mask == rhs.parameter_mask &&
         lhs.success == rhs.success && lhs.terminal == rhs.terminal &&
         lhs.failure == rhs.failure &&
         lhs.failure_transition == rhs.failure_transition;
}

std::uint64_t legacyPoseFingerprint(
    const sf::game::LegacyObjectBridgeState &object) noexcept {
  auto fingerprint = std::uint64_t{1469598103934665603ULL};
  const auto mix = [&fingerprint](std::uint64_t value) {
    fingerprint ^= value;
    fingerprint *= 1099511628211ULL;
  };
  mix(object.pose_flags);
  mix(object.presentation_enabled);
  mix(object.presentation_mode);
  mix(object.bone_matrix_count);
  for (std::size_t bone = 0U; bone < object.bone_matrix_count; ++bone) {
    const auto &matrix = object.bone_matrices[bone];
    for (const auto value : matrix.rotation) {
      mix(static_cast<std::uint16_t>(value));
    }
    mix(static_cast<std::uint32_t>(matrix.translation.x));
    mix(static_cast<std::uint32_t>(matrix.translation.y));
    mix(static_cast<std::uint32_t>(matrix.translation.z));
  }
  return fingerprint;
}

bool legacyActorAllocated(const sf::game::LegacyObjectBridgeState &object,
                          std::uint16_t dynamic_first_slot) noexcept {
  const auto actor_class =
      object.class_id == 0 || object.class_id == 1 || object.class_id == 0x35;
  if (!actor_class || !object.resident) {
    return false;
  }
  if (object.slot < dynamic_first_slot) {
    return true;
  }
  return object.maximum_health != 0 || object.health != 0 ||
         object.attributes != 0U || object.parameter != 0 ||
         object.path_pointer != 0U || object.authored_position.x != 0 ||
         object.authored_position.y != 0 || object.authored_position.z != 0;
}

int probeLegacyLevel(const char *cue_path, std::uint32_t frame_count) {
  auto disc = openDisc(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "Legacy level probe requires Syphon Filter USA v1.1"};
  }

  const auto mission = sf::game::MissionPackage::loadFirst(disc);
  const auto &legacy_image = mission.legacyImage();
  auto virtual_cd = legacy_image.createVirtualCd();
  sf::game::LegacyGameplayVm vm{legacy_image.executable()};
  vm.bindSyphonFilterUsaV11BootstrapPlatformCalls();
  vm.bindSyphonFilterUsaV11VirtualCdCalls(std::move(virtual_cd));
  vm.bindHostCall(0x800ddc34U, [](sf::game::LegacyHostCallContext &context) {
    std::cout << "legacy-assert: ra=0x" << std::hex << std::uppercase
              << context.returnAddress();
    for (std::size_t index = 0U; index < 4U; ++index) {
      const auto argument = context.argument(index);
      std::cout << ", a" << index << "=0x" << argument;
      std::string text;
      if (context.readCString(argument, text, 128U)) {
        std::cout << "(\"" << text << "\")";
      }
    }
    std::cout << std::dec << '\n';
    context.rejectHostCall();
  });

  const auto bootstrap = vm.bootstrapFirstMission();
  if (!bootstrap.completed()) {
    std::cout << "legacy-level-bootstrap-fault: phase="
              << static_cast<unsigned int>(bootstrap.phase) << ", reason="
              << sf::psx::toString(bootstrap.execution.execution.reason)
              << ", pc=0x" << std::hex << std::uppercase
              << bootstrap.execution.execution.pc << std::dec
              << ", bridge=" << bootstrap.bridge_fault << '\n';
    return 30;
  }
  const auto activate_opening_cbdc = vm.invoke(0x8005fd04U, std::array{6U});
  if (!activate_opening_cbdc.completed()) {
    std::cout << "legacy-level-opening-activation-fault: reason="
              << sf::psx::toString(activate_opening_cbdc.execution.reason)
              << ", pc=0x" << std::hex << std::uppercase
              << activate_opening_cbdc.execution.pc << std::dec << '\n';
    return 30;
  }

  std::vector<LegacyLevelActorLifetime> lifetimes;
  std::vector<LegacyLevelSlotTrace> slots;
  std::optional<sf::game::LegacyMissionBridgeState> first_mission;
  std::optional<sf::game::LegacyMissionBridgeState> previous_mission;
  std::optional<sf::game::LegacyMissionBridgeState> last_mission;
  std::optional<sf::game::LegacyNativePoint> post_opening_player_origin;
  std::optional<std::uint32_t> opening_complete_frame;
  std::optional<std::uint32_t> last_checkpoint_frame;
  std::uint16_t dynamic_first_slot{};
  std::uint32_t completed_frames{};
  std::uint32_t outer_updates{};
  std::uint32_t native_updates{};
  std::uint32_t mission_transitions{};
  std::uint32_t checkpoints{};
  std::uint32_t maximum_pending_events{};
  std::uint32_t maximum_ready_events{};
  std::uint32_t invalid_targets{};
  bool native_driven{};
  bool post_opening_player_moved{};
  std::optional<sf::game::LegacyGameplayBridgeState> driver_bridge;
  std::optional<sf::game::LegacyMissionBridgeState> driver_mission;
  std::optional<sf::game::LegacyNativePoint> previous_applied_host_position;
  std::optional<std::string> driver_first_blocker;
  std::optional<std::uint32_t> previous_application_state;
  std::optional<std::uint32_t> previous_camera_controller;
  std::optional<std::uint32_t> previous_camera_mode;
  std::optional<std::uint32_t> previous_camera_lock;
  std::optional<sf::game::LegacyNativePoint> previous_camera_eye;
  std::vector<std::uint64_t> damaged_actor_generations;
  LegacyLevelDriverStage driver_stage{LegacyLevelDriverStage::waiting_opening};
  std::uint32_t driver_stage_frames{};
  std::uint32_t driver_stage_guest_updates{};
  std::uint32_t driver_stage_entry_trace_frame{};
  std::uint32_t bank_reinforcement_kills{};
  std::uint32_t bank_roots_materialized{};
  std::uint32_t bank_reinforcement_goal{};
  std::uint32_t bank_quiescent_updates{};
  std::uint32_t bank_last_root_idle_updates{};
  std::uint32_t power_post_transition_updates{};
  std::array<std::uint32_t, 5U> bank_descriptor_paths{};
  std::optional<std::uint16_t> bank_materialized_slot;
  std::optional<std::uint64_t> bank_materialized_generation;
  std::optional<sf::game::LegacyNativePoint> bank_route_hold_position;
  std::uint32_t bank_route_next_sync_update{1U};
  bool bank_activation_volume_entered{};
  bool bank_source_fallback_attempted{};
  bool bank_descriptor_fallback_attempted{};
  bool bomb_29_callback_attempted{};
  bool bomb_29_callback_completed{};
  bool bomb_28_callback_attempted{};
  bool bomb_28_callback_completed{};
  bool non_gameplay_transition_traced{};
  bool power_scripted_transition_completed{};
  std::uint32_t camera_controller_changes{};
  std::uint32_t camera_discontinuities{};
  std::uint32_t scripted_camera_rail_frames{};
  std::uint32_t player_position_overrides{};
  std::uint32_t packed_ground_sentinel_samples{};
  std::uint32_t raw_ground_dumps{};
  bool raw_ground_sentinel_dumped{};
  std::array<bool, 9U> trigger_visited{};
  std::array<bool, 9U> trigger_observed{};
  std::array<bool, 10U> trigger_fallback_attempted{};
  std::array<bool, 10U> trigger_fallback_completed{};
  bool gate_event14_fallback_attempted{};
  bool gate_event14_fallback_completed{};
  bool gate_handler14_fallback_completed{};
  bool bank_descriptor_completed{};
  bool failure_branch_checked{};
  bool failure_branch_passed{};
  bool intro_state9_seen{};
  bool intro_state9_returned{};
  bool scripted_camera_rail_seen{};
  bool elevator_315_event14_attempted{};
  bool elevator_315_event14_completed{};
  bool elevator_315_motion_started{};
  bool elevator_315_motion_completed{};
  std::uint32_t elevator_passenger_boarding_updates{};
  std::optional<std::int32_t> elevator_passenger_board_y;
  bool elevator_passenger_positioned{};
  bool elevator_return_started{};
  bool elevator_return_completed{};
  bool elevator_passenger_carried{};
  bool elevator_316_event14_attempted{};
  bool elevator_316_event14_completed{};
  bool elevator_316_motion_started{};
  bool elevator_316_motion_completed{};
  bool station_318_event14_attempted{};
  bool station_318_event14_completed{};
  bool station_318_handler_fallback_attempted{};
  bool station_318_scripted_transition_completed{};
  bool station_318_motion_started{};
  bool station_318_motion_completed{};
  bool station_319_event14_attempted{};
  bool station_319_event14_completed{};
  bool station_319_handler_fallback_attempted{};
  bool station_319_motion_started{};
  bool station_319_motion_completed{};
  bool finale_callback_attempted{};
  bool finale_callback_completed{};
  bool finale_state9_seen{};
  bool finale_state9_returned{};
  bool scripted_route_complete{};
  bool driver_failed{};

  const auto source_host_position = [&mission](std::uint16_t source) {
    const auto &transform = mission.objects().objects()[source].transform;
    return sf::game::LegacyNativePoint{
        transform.x,
        -transform.y,
        transform.z,
    };
  };
  const auto source_room = [&mission](std::uint16_t source) {
    // Static event volumes are authored in EMD room records rather than
    // the mission object's room list.
    switch (source) {
    case 157U:
      return std::uint16_t{80U};
    case 190U:
      return std::uint16_t{21U};
    case 191U:
      return std::uint16_t{42U};
    case 192U:
      return std::uint16_t{40U};
    case 193U:
      return std::uint16_t{0U};
    case 194U:
      return std::uint16_t{24U};
    case 257U:
      return std::uint16_t{68U};
    case 258U:
      return std::uint16_t{9U};
    case 259U:
      return std::uint16_t{44U};
    default:
      break;
    }
    for (std::size_t room = 0U; room < mission.objects().roomCount(); ++room) {
      if (std::ranges::find(mission.objects().objectsInRoom(room), source) !=
          mission.objects().objectsInRoom(room).end()) {
        return static_cast<std::uint16_t>(room);
      }
    }
    const auto &source_transform =
        mission.objects().objects()[source].transform;
    auto nearest_room = std::uint16_t{};
    auto nearest_distance = std::numeric_limits<std::int64_t>::max();
    for (std::size_t room = 0U; room < mission.objects().roomCount(); ++room) {
      for (const auto candidate : mission.objects().objectsInRoom(room)) {
        const auto &transform =
            mission.objects().objects()[candidate].transform;
        const auto dx =
            static_cast<std::int64_t>(transform.x) - source_transform.x;
        const auto dy =
            static_cast<std::int64_t>(transform.y) - source_transform.y;
        const auto dz =
            static_cast<std::int64_t>(transform.z) - source_transform.z;
        const auto distance = dx * dx + dy * dy + dz * dz;
        if (distance < nearest_distance) {
          nearest_distance = distance;
          nearest_room = static_cast<std::uint16_t>(room);
        }
      }
    }
    return nearest_room;
  };
  const auto object_matches_source =
      [&](const sf::game::LegacyObjectBridgeState &object,
          std::uint16_t source) {
        if (object.slot == source) {
          return true;
        }
        return sameLegacyPoint(object.authored_position,
                               source_host_position(source));
      };
  const auto record_driver_blocker = [&](std::string blocker) {
    driver_failed = true;
    if (!driver_first_blocker) {
      std::cout << "driver-blocker: stage="
                << legacyLevelDriverStageName(driver_stage)
                << ", reason=" << blocker << '\n';
      driver_first_blocker = std::move(blocker);
    }
  };
  const auto enter_driver_stage = [&](LegacyLevelDriverStage stage,
                                      std::uint32_t frame) {
    driver_stage = stage;
    driver_stage_frames = 0U;
    driver_stage_guest_updates = 0U;
    driver_stage_entry_trace_frame = frame;
    previous_applied_host_position.reset();
    std::cout << "driver-stage: frame=" << frame
              << ", stage=" << legacyLevelDriverStageName(stage) << '\n';
  };
  const auto dispatch_trigger_fallback = [&](std::size_t index,
                                             std::uint16_t source) {
    if (trigger_fallback_attempted[index]) {
      return trigger_fallback_completed[index];
    }
    trigger_fallback_attempted[index] = true;
    const auto event =
        vm.queueHostInteraction(static_cast<std::int16_t>(source));
    trigger_fallback_completed[index] = event.completed();
    std::cout << "trigger-event12-fallback: stage="
              << legacyLevelDriverStageName(driver_stage)
              << ", source=" << source << ", completed=" << event.completed()
              << ", reason=" << sf::psx::toString(event.execution.reason)
              << ", pc=0x" << std::hex << std::uppercase << event.execution.pc
              << std::dec << '\n';
    if (!event.completed()) {
      record_driver_blocker("source-" + std::to_string(source) +
                            "-event12-dispatch-fault");
    }
    return event.completed();
  };
  const auto dispatch_direct_object_event_from = [&](std::uint16_t destination,
                                                     std::uint16_t event_id,
                                                     std::uint16_t source) {
    constexpr std::uint32_t event_address = 0x80117e60U;
    constexpr std::uint32_t object_handler_table = 0x801028a4U;
    const auto bridge = vm.readBridgeState();
    if (!bridge || destination >= bridge->objects.size() ||
        bridge->objects[destination].class_id < 0) {
      return false;
    }
    auto event_written = true;
    for (std::uint32_t offset = 0U; offset < 0x1cU; offset += 4U) {
      event_written =
          event_written && vm.runtime().write32(event_address + offset, 0U);
    }
    event_written = event_written &&
                    vm.runtime().write16(event_address, event_id) &&
                    vm.runtime().write32(event_address + 4U, source) &&
                    vm.runtime().write32(event_address + 8U, destination);
    std::uint32_t handler_entry{};
    event_written =
        event_written &&
        vm.runtime().read32(object_handler_table +
                                static_cast<std::uint32_t>(
                                    bridge->objects[destination].class_id) *
                                    4U,
                            handler_entry);
    std::optional<sf::game::LegacyGameplayVmResult> handler;
    if (event_written && handler_entry != 0U) {
      handler = vm.invoke(handler_entry, std::array{event_address});
    }
    const auto completed = handler && handler->completed();
    std::cout << "probe-direct-object-event: stage="
              << legacyLevelDriverStageName(driver_stage)
              << ", source=" << source << ", destination=" << destination
              << ", event=0x" << std::hex << std::uppercase << event_id
              << ", handler=0x" << handler_entry << std::dec
              << ", written=" << event_written << ", completed=" << completed;
    if (handler) {
      std::cout << ", reason=" << sf::psx::toString(handler->execution.reason)
                << ", pc=0x" << std::hex << std::uppercase
                << handler->execution.pc << std::dec;
    }
    std::cout << '\n';
    return completed;
  };
  const auto dispatch_direct_object_event =
      [&dispatch_direct_object_event_from](std::uint16_t destination,
                                           std::uint16_t event_id) {
        return dispatch_direct_object_event_from(destination, event_id,
                                                 destination);
      };
  const auto dispatch_load_fallback = [&](std::uint16_t source) {
    constexpr std::uint32_t event_entry = 0x80015364U;
    const std::array arguments{
        0x0aU,
        2U,
        static_cast<std::uint32_t>(source),
        static_cast<std::uint32_t>(source),
        0U,
        0U,
        0U,
        0U,
    };
    const auto event = vm.invoke(event_entry, arguments, 5'000'000U);
    std::cout << "probe-event0a-continuation: stage="
              << legacyLevelDriverStageName(driver_stage)
              << ", source=" << source << ", completed=" << event.completed()
              << ", reason=" << sf::psx::toString(event.execution.reason)
              << ", pc=0x" << std::hex << std::uppercase << event.execution.pc
              << std::dec << '\n';
    if (!event.completed()) {
      record_driver_blocker("source-" + std::to_string(source) +
                            "-event0a-continuation-fault");
    }
    return event.completed();
  };
  const auto materialize_descriptor_actor = [&](std::uint8_t descriptor,
                                                std::uint16_t slot,
                                                std::uint8_t root,
                                                std::uint8_t type) {
    std::uint32_t descriptor_table{};
    const auto descriptor_read =
        vm.runtime().read32(0x80116adcU, descriptor_table);
    const auto descriptor_address =
        descriptor_table + static_cast<std::uint32_t>(descriptor) * 16U;
    const auto packed = (static_cast<std::uint32_t>(type) << 24U) |
                        (static_cast<std::uint32_t>(root) << 16U) |
                        (static_cast<std::uint32_t>(descriptor) << 8U) | 0xe0U;
    std::optional<sf::game::LegacyGameplayVmResult> materialization;
    if (descriptor_read) {
      materialization = vm.invoke(0x800663a8U,
                                  std::array{
                                      static_cast<std::uint32_t>(slot),
                                      0U,
                                      packed,
                                      0U,
                                  },
                                  5'000'000U);
    }
    const auto materialized = materialization && materialization->completed();
    // The retail visibility path follows FUN800663a8 with class-1 event 6.
    // FUN80061874 then calls FUN800659b8, which enables NPC simulation.
    const auto simulated =
        materialized && dispatch_direct_object_event_from(slot, 0x06U, slot);
    std::cout << "probe-descriptor-materialization: descriptor="
              << static_cast<unsigned int>(descriptor) << ", slot=" << slot
              << ", root=" << static_cast<unsigned int>(root)
              << ", type=" << static_cast<unsigned int>(type) << ", address=0x"
              << std::hex << std::uppercase << descriptor_address
              << ", packed=0x" << packed << std::dec
              << ", table-read=" << descriptor_read
              << ", materialized=" << materialized << ", event6=" << simulated;
    if (materialization) {
      std::cout << ", return=" << materialization->return_value << ", reason="
                << sf::psx::toString(materialization->execution.reason)
                << ", pc=0x" << std::hex << std::uppercase
                << materialization->execution.pc << std::dec;
    }
    std::cout << '\n';
    return simulated;
  };
  const auto trace_driver_object = [&](std::string_view label,
                                       std::uint16_t source) {
    const auto bridge = vm.readBridgeState();
    if (!bridge || source >= bridge->objects.size()) {
      std::cout << "driver-object-state: label=" << label
                << ", source=" << source << ", bridge=0\n";
      return;
    }
    const auto &object = bridge->objects[source];
    std::uint8_t instance_flags{};
    std::uint8_t instance_secondary_flags{};
    std::uint32_t pending_events{};
    std::uint32_t ready_events{};
    static_cast<void>(vm.runtime().read8(object.instance, instance_flags));
    static_cast<void>(
        vm.runtime().read8(object.instance + 1U, instance_secondary_flags));
    static_cast<void>(vm.runtime().read32(0x80116c68U, pending_events));
    static_cast<void>(vm.runtime().read32(0x8011775cU, ready_events));
    std::cout << "driver-object-state: label=" << label << ", source=" << source
              << ", class=0x" << std::hex << std::uppercase
              << static_cast<std::uint16_t>(object.class_id) << std::dec
              << ", hp=" << object.health << '/' << object.maximum_health
              << ", linked=" << object.linked_slot
              << ", resident=" << object.resident
              << ", simulated=" << object.simulated << ", instance=0x"
              << std::hex << std::uppercase << object.instance << ':'
              << static_cast<unsigned int>(instance_flags) << '/'
              << static_cast<unsigned int>(instance_secondary_flags)
              << ", state="
              << static_cast<unsigned int>(object.instance_state[0]) << '/'
              << static_cast<unsigned int>(object.instance_state[1]) << '/'
              << static_cast<unsigned int>(object.instance_state[2]) << '/'
              << static_cast<unsigned int>(object.instance_state[3])
              << ", presentation=0x" << object.presentation_controller << ':'
              << static_cast<unsigned int>(object.presentation_enabled) << '/'
              << static_cast<unsigned int>(object.presentation_mode) << std::dec
              << ", position=(" << object.position.x << ',' << object.position.y
              << ',' << object.position.z << ')'
              << ", ground=" << object.ground_contact_valid << ':'
              << object.ground_contact_y << ", events=" << pending_events << '/'
              << ready_events << '\n';
  };
  const auto trace_driver_events = [&](std::string_view label) {
    const auto trace_queue = [&](std::string_view queue,
                                 std::uint32_t count_address) {
      std::uint32_t count{};
      if (!vm.runtime().read32(count_address, count)) {
        return;
      }
      count = std::min(count, 16U);
      for (std::uint32_t index = 0U; index < count; ++index) {
        const auto event = count_address + 4U + index * 0x1cU;
        std::uint16_t id{};
        std::uint32_t source{};
        std::uint32_t destination{};
        if (!vm.runtime().read16(event, id) ||
            !vm.runtime().read32(event + 4U, source) ||
            !vm.runtime().read32(event + 8U, destination)) {
          break;
        }
        std::cout << "driver-event-state: label=" << label
                  << ", queue=" << queue << ", index=" << index << ", id=0x"
                  << std::hex << std::uppercase << id << ", source=0x" << source
                  << ", destination=0x" << destination << std::dec << '\n';
      }
    };
    trace_queue("pending", 0x80116c68U);
    trace_queue("ready", 0x8011775cU);
  };
  const auto queue_driver_damage =
      [&](const sf::game::LegacyObjectBridgeState &object,
          std::string_view reason) {
        const auto generation =
            object.slot < slots.size() ? slots[object.slot].generations : 0U;
        const auto key =
            (static_cast<std::uint64_t>(object.slot) << 32U) | generation;
        if (std::ranges::find(damaged_actor_generations, key) !=
            damaged_actor_generations.end()) {
          return 0;
        }
        if (!driver_mission || driver_mission->player_slot < 0) {
          return -1;
        }
        if (object.slot == 140U) {
          trace_driver_object("lock-before-damage", 140U);
          trace_driver_object("gate-before-damage", 67U);
        }
        const auto impact_only = object.class_id == 0x3a;
        if (impact_only) {
          const auto impact =
              vm.queueHostImpact(driver_mission->player_slot,
                                 static_cast<std::int16_t>(object.slot));
          if (!impact.completed()) {
            std::cout << "driver-impact-fault: slot=" << object.slot
                      << ", reason=" << reason << ", pc=0x" << std::hex
                      << std::uppercase << impact.execution.pc << std::dec
                      << '\n';
            return -1;
          }
        } else {
          const auto immediate_static_actor =
              object.slot < dynamic_first_slot &&
              (object.class_id == 0 || object.class_id == 1 ||
               object.class_id == 0x35);
          if (immediate_static_actor) {
            std::uint16_t hit_part{1U};
            if (object.health_controller != 0U) {
              static_cast<void>(
                  vm.runtime().read16(object.health_controller + 2U, hit_part));
            }
            // Headless actors have no HMD display root from which
            // FUN80069224 can derive an impact vector. Continue at the
            // exact retail health core with its decoded arguments.
            const auto damage = vm.invoke(
                0x80068770U,
                std::array{
                    static_cast<std::uint32_t>(object.slot),
                    0xffffffffU,
                    static_cast<std::uint32_t>(driver_mission->player_slot),
                    0x7fffU,
                    static_cast<std::uint32_t>(hit_part),
                    0U,
                    0x8011e670U,
                    0x8011e670U,
                },
                5'000'000U);
            if (!damage.completed()) {
              std::cout << "driver-damage-core-fault: slot=" << object.slot
                        << ", reason=" << reason << '\n';
              return -1;
            }
          } else {
            const auto damage =
                vm.queueHostDamage(sf::game::LegacyHostDamageEvent{
                    driver_mission->player_slot,
                    driver_mission->player_slot,
                    static_cast<std::int16_t>(object.slot),
                    std::numeric_limits<std::int16_t>::max(),
                    0x0f,
                });
            if (!damage.completed()) {
              std::cout << "driver-damage-fault: slot=" << object.slot
                        << ", reason=" << reason << ", pc=0x" << std::hex
                        << std::uppercase << damage.execution.pc << std::dec
                        << '\n';
              return -1;
            }
          }
        }
        if (object.slot == 174U || object.slot == 260U) {
          // The headless route bypasses the normal death dispatcher. Replay
          // the exact SUBWAY.BIN callback registered by FUN800686f4.
          const auto overlay_death = vm.invoke(
              0x80148168U,
              std::array{
                  static_cast<std::uint32_t>(object.slot),
                  static_cast<std::uint32_t>(driver_mission->player_slot),
              },
              5'000'000U);
          if (!overlay_death.completed()) {
            std::cout << "driver-overlay-death-fault: slot=" << object.slot
                      << ", reason=" << reason << '\n';
            return -1;
          }
        }
        damaged_actor_generations.push_back(key);
        std::cout << "driver-damage: stage="
                  << legacyLevelDriverStageName(driver_stage)
                  << ", slot=" << object.slot << ", generation=" << generation
                  << ", reason=" << reason << '\n';
        if (object.slot == 140U) {
          trace_driver_object("lock-after-damage", 140U);
          trace_driver_object("gate-after-damage", 67U);
        }
        return 1;
      };
  const auto verify_failure_branch = [&](std::uint16_t protected_slot) {
    constexpr std::uint32_t retail_failure_delay = 0xc8U;
    const auto snapshot = vm.captureSnapshot();
    std::optional<std::uint32_t> failure_update;
    std::optional<std::uint32_t> terminal_update;
    std::optional<std::uint32_t> transition_update;
    auto completed_seen = false;
    auto failure_bridge_seen = false;
    auto early_transition = false;
    const auto player_slot =
        driver_mission ? driver_mission->player_slot : std::int16_t{-1};
    const auto trace_failure_latches = [&](std::string_view phase,
                                           std::uint32_t update) {
      std::uint8_t terminal{};
      std::uint8_t transition_started{};
      std::uint8_t transition{};
      std::uint8_t failure{};
      std::uint8_t completed{};
      const auto raw = vm.runtime().read8(0x80115cc8U, terminal) &&
                       vm.runtime().read8(0x80115cc9U, transition_started) &&
                       vm.runtime().read8(0x80115ccaU, transition) &&
                       vm.runtime().read8(0x80116b24U, failure) &&
                       vm.runtime().read8(0x80116b25U, completed);
      const auto mission_state = vm.readMissionBridgeState();
      const auto bridge_state = vm.readBridgeState();
      std::cout << "failure-latch-trace: phase=" << phase
                << ", update=" << update << ", raw=" << raw << ':'
                << static_cast<unsigned int>(terminal) << '/'
                << static_cast<unsigned int>(transition_started) << '/'
                << static_cast<unsigned int>(transition) << '/'
                << static_cast<unsigned int>(failure) << '/'
                << static_cast<unsigned int>(completed)
                << ", bridge=" << mission_state.has_value();
      if (mission_state) {
        std::cout << ':' << mission_state->terminal << '/'
                  << mission_state->success << '/' << mission_state->failure
                  << '/' << mission_state->failure_transition;
      }
      if (bridge_state && bridge_state->objects.size() > protected_slot) {
        std::cout << ", health="
                  << bridge_state->objects[protected_slot].health;
      }
      std::cout << '\n';
    };
    trace_failure_latches("before-damage", 0U);
    const auto damage = vm.queueHostDamage(sf::game::LegacyHostDamageEvent{
        player_slot,
        player_slot,
        std::bit_cast<std::int16_t>(protected_slot),
        std::numeric_limits<std::int16_t>::max(),
        0x0f,
    });
    trace_failure_latches("after-damage", 0U);
    if (damage.completed()) {
      for (std::uint32_t update = 0U; update < retail_failure_delay + 160U;
           ++update) {
        std::uint8_t terminal{};
        std::uint8_t transition{};
        std::uint8_t failure{};
        std::uint8_t completed{};
        if (!vm.runtime().read8(0x80115cc8U, terminal) ||
            !vm.runtime().read8(0x80115ccaU, transition) ||
            !vm.runtime().read8(0x80116b24U, failure) ||
            !vm.runtime().read8(0x80116b25U, completed)) {
          break;
        }
        completed_seen = completed_seen || completed != 0U;
        if (failure != 0U && !failure_update) {
          failure_update = update;
        }
        if (terminal != 0U && !terminal_update) {
          terminal_update = update;
        }
        const auto mission_state = vm.readMissionBridgeState();
        failure_bridge_seen =
            failure_bridge_seen || (mission_state && mission_state->failure);
        if (transition != 0U && failure_update) {
          transition_update = update;
          early_transition = update - *failure_update < retail_failure_delay;
          break;
        }
        std::uint32_t application_state{};
        if (!vm.runtime().read32(0x80115c78U, application_state) ||
            !vm.writeHostPadState(sf::game::LegacyHostPadState{})) {
          break;
        }
        const auto gameplay =
            application_state == 0U || application_state == 5U;
        const auto result = gameplay ? vm.tickNativeDrivenGameplayFrame()
                                     : vm.tickRetailOuterFrame();
        if (!result.completed()) {
          break;
        }
        const auto traced_update = update + 1U;
        if (traced_update <= 3U ||
            (failure_update &&
             traced_update + 2U >= *failure_update + retail_failure_delay)) {
          trace_failure_latches("after-tick", traced_update);
        }
      }
    }
    constexpr std::uint32_t exact_failure_update = 2U;
    constexpr std::uint32_t exact_transition_update =
        exact_failure_update + retail_failure_delay;
    const auto passed = damage.completed() && failure_update.has_value() &&
                        terminal_update.has_value() &&
                        transition_update.has_value() && failure_bridge_seen &&
                        !completed_seen && !early_transition &&
                        *failure_update == exact_failure_update &&
                        *terminal_update == exact_failure_update &&
                        *transition_update == exact_transition_update;
    const auto restored = vm.restoreSnapshot(snapshot);
    std::cout << "failure-branch: protected-slot=" << protected_slot
              << ", failure=" << passed << ", outcome-update=";
    if (failure_update) {
      std::cout << *failure_update;
    } else {
      std::cout << "none";
    }
    std::cout << ", terminal-update=";
    if (terminal_update) {
      std::cout << *terminal_update;
    } else {
      std::cout << "none";
    }
    std::cout << ", transition-update=";
    if (transition_update) {
      std::cout << *transition_update;
    } else {
      std::cout << "none";
    }
    std::cout << ", completed-seen=" << completed_seen
              << ", bridge-failure=" << failure_bridge_seen
              << ", early-transition=" << early_transition
              << ", restored=" << restored << '\n';
    return passed && restored;
  };
  const auto force_driver_room = [&](std::uint16_t source) {
    const auto room = source_room(source);
    // The probe teleports instead of crossing authored portals, but uses
    // the same exact FUN_800820d4 room-change boundary as production.
    constexpr std::uint32_t current_room_address = 0x80116946U;
    std::uint16_t previous_room{};
    const auto previous_room_read =
        vm.runtime().read16(current_room_address, previous_room);
    const auto room_changed = previous_room_read && previous_room != room;
    auto room_synchronized = previous_room_read;
    std::vector<std::uint16_t> room_route;
    std::optional<std::uint16_t> failed_route_room;
    auto bank_portal_route_selected = false;
    if (room_synchronized && room_changed) {
      // The neighbour table also contains visibility-only links. This
      // retail route is the portal/AABB-connected path extracted from
      // SUBWAY.DAT for the radio/Kravitch area back to bank room 18.
      constexpr std::array<std::uint16_t, 19U> bank_portal_route{
          71U, 70U, 68U, 69U, 67U, 82U, 81U, 73U, 75U, 76U,
          84U, 85U, 86U, 89U, 90U, 23U, 21U, 22U, 18U,
      };
      const auto bank_route_position =
          std::ranges::find(bank_portal_route, previous_room);
      if (room == 18U && bank_route_position != bank_portal_route.end() &&
          std::next(bank_route_position) != bank_portal_route.end()) {
        bank_portal_route_selected = true;
        room_route.push_back(*std::next(bank_route_position));
      } else {
        std::uint32_t room_data{};
        std::uint32_t room_count{};
        room_synchronized =
            vm.runtime().read32(0x80116a60U, room_data) && room_data != 0U &&
            vm.runtime().read32(room_data + 0x88U, room_count) &&
            room_count != 0U && room_count <= 256U &&
            previous_room < room_count && room < room_count;
        std::vector<std::int16_t> predecessor;
        std::vector<std::uint16_t> pending;
        if (room_synchronized) {
          predecessor.assign(room_count, std::int16_t{-2});
          predecessor[previous_room] = -1;
          pending.push_back(previous_room);
        }
        for (std::size_t cursor = 0U;
             room_synchronized && cursor < pending.size() &&
             predecessor[room] == -2;
             ++cursor) {
          const auto current = pending[cursor];
          const auto neighbours =
              room_data + 0x90U + static_cast<std::uint32_t>(current) * 0x0fU;
          for (std::uint32_t index = 0U; index < 0x0fU; ++index) {
            std::uint8_t neighbour{};
            if (!vm.runtime().read8(neighbours + index, neighbour)) {
              room_synchronized = false;
              break;
            }
            if (neighbour >= room_count) {
              break;
            }
            if (predecessor[neighbour] == -2) {
              predecessor[neighbour] = static_cast<std::int16_t>(current);
              pending.push_back(neighbour);
            }
          }
        }
        room_synchronized = room_synchronized && predecessor[room] != -2;
        if (room_synchronized) {
          for (auto current = room; current != previous_room;
               current = static_cast<std::uint16_t>(predecessor[current])) {
            room_route.push_back(current);
          }
          std::ranges::reverse(room_route);
        }
      }
      if (room_synchronized) {
        constexpr std::array bank_route_portal_seeds{
            std::pair{70U, sf::game::LegacyNativePoint{-302, -2'140, 5'606}},
            std::pair{68U, sf::game::LegacyNativePoint{1'133, -2'140, 6'810}},
            std::pair{69U, sf::game::LegacyNativePoint{1'400, -2'140, 5'154}},
            std::pair{67U, sf::game::LegacyNativePoint{1'766, -2'140, 3'410}},
            std::pair{82U, sf::game::LegacyNativePoint{2'425, -2'140, 5'137}},
            std::pair{81U, sf::game::LegacyNativePoint{2'977, -2'140, 4'495}},
            std::pair{73U, sf::game::LegacyNativePoint{4'221, -2'140, 3'177}},
            std::pair{75U, sf::game::LegacyNativePoint{5'439, -2'140, 3'639}},
            std::pair{76U, sf::game::LegacyNativePoint{5'766, -2'140, 4'532}},
            std::pair{84U, sf::game::LegacyNativePoint{6'431, -2'140, 4'979}},
            std::pair{85U, sf::game::LegacyNativePoint{7'340, -2'140, 6'669}},
            std::pair{86U, sf::game::LegacyNativePoint{8'669, -2'140, 5'656}},
            std::pair{89U, sf::game::LegacyNativePoint{10'358, -2'140, 7'237}},
            std::pair{90U, sf::game::LegacyNativePoint{10'472, -2'140, 9'040}},
            std::pair{23U, sf::game::LegacyNativePoint{10'840, -2'140, 9'813}},
            std::pair{21U, sf::game::LegacyNativePoint{11'682, -2'140, 10'890}},
            std::pair{22U, sf::game::LegacyNativePoint{11'812, -2'140, 11'447}},
            std::pair{18U, sf::game::LegacyNativePoint{12'546, -2'140, 12'554}},
        };
        constexpr std::array bank_route_destination_points{
            std::pair{70U, sf::game::LegacyNativePoint{418, -2'141, 6'148}},
            std::pair{68U, sf::game::LegacyNativePoint{1'449, -2'140, 6'062}},
            std::pair{69U, sf::game::LegacyNativePoint{1'473, -2'141, 3'877}},
            std::pair{67U, sf::game::LegacyNativePoint{2'315, -2'140, 4'186}},
            std::pair{82U, sf::game::LegacyNativePoint{3'318, -2'133, 5'272}},
            std::pair{81U, sf::game::LegacyNativePoint{3'633, -2'133, 3'836}},
            std::pair{73U, sf::game::LegacyNativePoint{5'112, -2'133, 2'632}},
            std::pair{75U, sf::game::LegacyNativePoint{5'434, -2'133, 4'198}},
            std::pair{76U, sf::game::LegacyNativePoint{6'212, -2'133, 4'757}},
            std::pair{84U, sf::game::LegacyNativePoint{6'557, -2'133, 6'101}},
            std::pair{85U, sf::game::LegacyNativePoint{8'008, -2'133, 6'108}},
            std::pair{86U, sf::game::LegacyNativePoint{9'678, -2'133, 6'342}},
            std::pair{89U, sf::game::LegacyNativePoint{9'682, -2'133, 8'150}},
            std::pair{90U, sf::game::LegacyNativePoint{9'676, -2'133, 10'206}},
            std::pair{23U, sf::game::LegacyNativePoint{11'196, -2'140, 10'367}},
            std::pair{21U, sf::game::LegacyNativePoint{12'065, -2'140, 11'004}},
            std::pair{22U, sf::game::LegacyNativePoint{12'113, -2'140, 12'114}},
            std::pair{18U, sf::game::LegacyNativePoint{13'666, -2'140, 11'801}},
        };
        for (const auto route_room : room_route) {
          if (bank_portal_route_selected) {
            const auto portal_seed = std::ranges::find_if(
                bank_route_portal_seeds, [route_room](const auto &entry) {
                  return entry.first == route_room;
                });
            if (portal_seed == bank_route_portal_seeds.end()) {
              room_synchronized = false;
              failed_route_room = route_room;
              break;
            }
            if (!vm.writeHostPlayerState(sf::game::LegacyHostPlayerState{
                    portal_seed->second,
                    0,
                    150,
                    600,
                })) {
              room_synchronized = false;
              failed_route_room = route_room;
              break;
            }
          }
          std::uint16_t synchronized_room{};
          if (!vm.synchronizeHostRoom(static_cast<std::int16_t>(route_room)) ||
              !vm.runtime().read16(current_room_address, synchronized_room) ||
              synchronized_room != route_room) {
            room_synchronized = false;
            failed_route_room = route_room;
            break;
          }
          if (bank_portal_route_selected) {
            const auto destination = std::ranges::find_if(
                bank_route_destination_points, [route_room](const auto &entry) {
                  return entry.first == route_room;
                });
            if (destination == bank_route_destination_points.end() ||
                !vm.writeHostPlayerState(sf::game::LegacyHostPlayerState{
                    destination->second,
                    0,
                    150,
                    600,
                })) {
              room_synchronized = false;
              failed_route_room = route_room;
              break;
            }
            bank_route_hold_position = destination->second;
            bank_route_next_sync_update = driver_stage_guest_updates + 2U;
          }
        }
      }
    }
    if (room_changed) {
      previous_applied_host_position.reset();
    }
    const auto bridge_after_bootstrap = vm.readBridgeState();
    const auto source_resident =
        bridge_after_bootstrap &&
        source < bridge_after_bootstrap->objects.size() &&
        bridge_after_bootstrap->objects[source].resident;
    const auto source_simulated =
        bridge_after_bootstrap &&
        source < bridge_after_bootstrap->objects.size() &&
        bridge_after_bootstrap->objects[source].simulated;
    std::uint16_t current_room{};
    const auto bridge_ok =
        vm.runtime().read16(current_room_address, current_room);
    std::cout << "driver-room: stage="
              << legacyLevelDriverStageName(driver_stage)
              << ", source=" << source << ", requested=" << room
              << ", previous=" << std::bit_cast<std::int16_t>(previous_room)
              << ", current=" << std::bit_cast<std::int16_t>(current_room)
              << ", production-room-sync=" << room_synchronized
              << ", changed=" << room_changed << ", route=";
    if (room_route.empty()) {
      std::cout << "same";
    } else {
      for (const auto route_room : room_route) {
        std::cout << route_room << '/';
      }
    }
    if (failed_route_room) {
      std::cout << ", failed-route-room=" << *failed_route_room;
    }
    std::cout << ", resident=" << source_resident
              << ", simulated=" << source_simulated << '\n';
    if (!room_synchronized) {
      const auto trace_word = [&](std::string_view name,
                                  std::uint32_t address) {
        std::uint32_t value{};
        const auto read = vm.runtime().read32(address, value);
        std::cout << "room-stream-state: " << name << "=";
        if (read) {
          std::cout << "0x" << std::hex << value << std::dec;
        } else {
          std::cout << "unreadable";
        }
        std::cout << '\n';
      };
      trace_word("queue-a", 0x80116a08U);
      trace_word("queue-b", 0x80116b3cU);
      trace_word("stream-transfer", 0x8011609cU);
      trace_word("stream-destination", 0x80116098U);
      trace_word("stream-count", 0x801160a0U);
      trace_word("stream-callback", 0x801160acU);
      trace_word("stream-state", 0x80116944U);
      trace_word("stream-request", 0x80115e80U);
      std::uint32_t queue_link{};
      if (vm.runtime().read32(0x80116a08U, queue_link) && queue_link != 0U) {
        for (std::uint32_t offset = 0U; offset < 0x24U; offset += 4U) {
          trace_word("queue-link+" + std::to_string(offset),
                     queue_link + offset);
        }
        std::uint32_t request{};
        if (vm.runtime().read32(queue_link, request) && request != 0U) {
          for (std::uint32_t offset = 0U; offset < 0x24U; offset += 4U) {
            trace_word("request+" + std::to_string(offset), request + offset);
          }
        }
      }
    }
    return previous_room_read && room_synchronized && bridge_ok;
  };

  const auto begin_lifetime =
      [&](const sf::game::LegacyObjectBridgeState &object,
          std::uint32_t frame) {
        auto &trace = slots[object.slot];
        LegacyLevelActorLifetime lifetime;
        lifetime.identity = LegacyLevelActorIdentity{
            object.class_id,          object.definition,   object.parameter,
            object.authored_position, object.path_pointer,
        };
        lifetime.slot = object.slot;
        lifetime.generation = trace.generations++;
        lifetime.first_frame = frame;
        lifetime.last_frame = frame;
        lifetime.start_health = object.health;
        lifetime.minimum_health = object.health;
        lifetime.end_health = object.health;
        lifetime.start_position = object.position;
        lifetime.end_position = object.position;
        lifetime.last_target = object.has_target ? object.target_slot : -1;
        lifetime.last_pose_fingerprint = legacyPoseFingerprint(object);
        lifetime.saw_positive_health = object.health > 0;
        lifetime.saw_target = object.has_target;
        lifetimes.push_back(lifetime);
        trace.lifetime = lifetimes.size() - 1U;
      };

  for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
    std::uint32_t current_state{};
    if (!vm.runtime().read32(0x80115c78U, current_state)) {
      return 31;
    }
    const auto gameplay_state = current_state == 0U || current_state == 5U;
    if (!previous_application_state ||
        *previous_application_state != current_state) {
      std::cout << "application-state: frame=" << frame
                << ", state=" << current_state
                << ", driver=" << legacyLevelDriverStageName(driver_stage)
                << '\n';
      previous_application_state = current_state;
    }
    sf::game::LegacyHostPadState driver_pad;
    std::optional<sf::game::LegacyNativePoint> applied_host_position;
    std::optional<std::uint16_t> room_bootstrap_source;
    if (native_driven && driver_stage != LegacyLevelDriverStage::complete) {
      ++driver_stage_frames;
    }
    if (native_driven && gameplay_state && driver_bridge && driver_mission) {
      ++driver_stage_guest_updates;
      const auto hold_source = [&](std::uint16_t source) {
        if (driver_stage_guest_updates == 1U) {
          room_bootstrap_source = source;
        }
        applied_host_position = source_host_position(source);
      };
      const auto cross_trigger = [&](std::uint16_t source,
                                     sf::game::LegacyNativePoint outside,
                                     sf::game::LegacyNativePoint inside) {
        if (driver_stage_guest_updates == 1U) {
          room_bootstrap_source = source;
        }
        applied_host_position =
            driver_stage_guest_updates <= 2U ? outside : inside;
      };
      const auto interact_after_settle = [&](std::uint16_t source) {
        if (driver_stage_guest_updates <= 2U) {
          hold_source(source);
        }
        if (driver_stage_guest_updates == 8U) {
          const auto interaction =
              vm.queueHostInteraction(std::bit_cast<std::int16_t>(source));
          std::cout << "driver-interaction: frame=" << frame
                    << ", stage=" << legacyLevelDriverStageName(driver_stage)
                    << ", source=" << source
                    << ", completed=" << interaction.completed() << ", reason="
                    << sf::psx::toString(interaction.execution.reason)
                    << ", pc=0x" << std::hex << std::uppercase
                    << interaction.execution.pc << std::dec << '\n';
          if (!interaction.completed()) {
            record_driver_blocker("source-" + std::to_string(source) +
                                  "-interaction-fault");
          }
        }
      };
      using enum LegacyLevelDriverStage;
      switch (driver_stage) {
      case waiting_opening:
      case failure_branch:
      case complete:
        break;
      case clear_opening: {
        hold_source(
            static_cast<std::uint16_t>(mission.objects().playerIndex()));
        if (!failure_branch_checked) {
          const auto protected_cbdc = std::ranges::find_if(
              driver_bridge->objects,
              [&](const sf::game::LegacyObjectBridgeState &object) {
                return object.slot == 351U && object.class_id == 0x35 &&
                       object.definition == 0x0dU && object.simulated &&
                       object.health > 0;
              });
          if (protected_cbdc != driver_bridge->objects.end()) {
            failure_branch_checked = true;
            failure_branch_passed = verify_failure_branch(
                static_cast<std::uint16_t>(protected_cbdc->slot));
            if (!failure_branch_passed) {
              record_driver_blocker("protected-cbdc-failure-branch-missing");
            }
          }
        }
        const auto hostile = std::ranges::find_if(
            driver_bridge->objects,
            [&](const sf::game::LegacyObjectBridgeState &object) {
              return object.class_id == 1 && object.simulated &&
                     object.health > 0 &&
                     !object_matches_source(object, 174U) &&
                     !object_matches_source(object, 175U);
            });
        if (hostile != driver_bridge->objects.end() &&
            queue_driver_damage(*hostile, "opening-hostile") < 0) {
          record_driver_blocker("opening-hostile-damage-fault");
        }
        break;
      }
      case trigger_256:
        hold_source(256U);
        if (driver_stage_guest_updates == 8U) {
          static_cast<void>(dispatch_trigger_fallback(0U, 256U));
        }
        break;
      case passage_64:
        if (driver_stage_guest_updates == 1U) {
          room_bootstrap_source = std::uint16_t{257U};
        }
        applied_host_position = source_host_position(64U);
        applied_host_position->y = source_host_position(257U).y;
        break;
      case passage_65:
        applied_host_position = source_host_position(65U);
        applied_host_position->y = source_host_position(257U).y;
        break;
      case trigger_257:
        cross_trigger(257U, {1'040, -2'150, 4'796}, {1'328, -2'150, 4'796});
        if (driver_stage_guest_updates == 8U &&
            driver_bridge->objects.size() > 257U &&
            (driver_bridge->objects[257U].attributes & 0x20U) == 0U) {
          static_cast<void>(dispatch_trigger_fallback(1U, 257U));
        }
        break;
      case intro_157:
        cross_trigger(157U, {-5'784, -2'204, 3'636}, {-5'636, -2'204, 3'636});
        if (driver_stage_guest_updates == 8U && !intro_state9_seen) {
          static_cast<void>(dispatch_trigger_fallback(2U, 157U));
        }
        break;
      case lock_140:
        hold_source(140U);
        if (driver_stage_guest_updates >= 3U &&
            driver_bridge->objects.size() > 140U &&
            driver_bridge->objects[140U].health > 0 &&
            queue_driver_damage(driver_bridge->objects[140U], "gate-lock") <
                0) {
          record_driver_blocker("gate-lock-damage-fault");
        }
        if (driver_stage_guest_updates == 8U &&
            driver_bridge->objects.size() > 140U &&
            (driver_bridge->objects[140U].instance_state[3] & 0x02U) != 0U &&
            (driver_bridge->objects[67U].instance_state[0] & 0x08U) == 0U) {
          gate_event14_fallback_attempted = true;
          const std::array arguments{
              0x14U, 3U, 140U, 67U, 0U, 0U, 0U, 0U,
          };
          const auto event = vm.invoke(0x80015364U, arguments);
          gate_event14_fallback_completed = event.completed();
          std::cout << "gate-event14-fallback: completed=" << event.completed()
                    << ", reason=" << sf::psx::toString(event.execution.reason)
                    << ", pc=0x" << std::hex << std::uppercase
                    << event.execution.pc << std::dec << '\n';
        }
        if (driver_stage_guest_updates == 12U &&
            gate_event14_fallback_attempted &&
            gate_event14_fallback_completed &&
            driver_bridge->objects.size() > 140U &&
            (driver_bridge->objects[140U].instance_state[3] & 0x02U) != 0U &&
            (driver_bridge->objects[67U].instance_state[0] & 0x08U) == 0U) {
          // Probe-only continuation: portal visibility is not reproduced
          // by the teleported route, so dispatch the exact class handler.
          gate_handler14_fallback_completed =
              dispatch_direct_object_event_from(67U, 0x14U, 140U);
          trace_driver_object("gate-after-direct-handler14", 67U);
        }
        break;
      case bank_175: {
        constexpr sf::game::LegacyNativePoint bank_safe_position{
            14'000,
            -2'140,
            10'700,
        };
        // Source 173 is not activated by entering room 18 directly.
        // Retail room 22 keeps room 18 resident; cross source 173's
        // exact AAA4B1 event OBB before taking the final portal.
        constexpr sf::game::LegacyNativePoint bank_activation_outside{
            12'120,
            -2'140,
            12'081,
        };
        constexpr sf::game::LegacyNativePoint bank_activation_inside{
            11'802,
            -2'140,
            12'081,
        };
        std::uint16_t bank_current_room{};
        const auto bank_room_read =
            vm.runtime().read16(0x80116946U, bank_current_room);
        const auto bank_route_in_progress =
            bank_room_read && bank_current_room != 18U;
        const auto bank_activation_pending =
            bank_route_in_progress && bank_current_room == 22U &&
            driver_stage_guest_updates < bank_route_next_sync_update + 3U;
        const auto bank_route_settling =
            bank_route_hold_position &&
            driver_stage_guest_updates < bank_route_next_sync_update;
        if (bank_route_in_progress && !bank_activation_pending &&
            driver_stage_guest_updates >= bank_route_next_sync_update) {
          room_bootstrap_source = std::uint16_t{173U};
        }
        // Room 18 bounds are x=12546..14787, z=10601..13001. Keep the
        // probe inside visibility range but off source 173's exact EA0
        // route (which starts at 12606,10770 and ends at 14559,12464).
        if (bank_activation_pending) {
          const auto crossing_volume =
              driver_stage_guest_updates >= bank_route_next_sync_update;
          applied_host_position = crossing_volume ? bank_activation_inside
                                                  : bank_activation_outside;
          if (crossing_volume && !bank_activation_volume_entered) {
            bank_activation_volume_entered = true;
            std::cout << "bank-source-173-volume-enter: update="
                      << driver_stage_guest_updates << '\n';
          }
        } else {
          applied_host_position =
              (bank_route_in_progress || bank_route_settling) &&
                      bank_route_hold_position
                  ? *bank_route_hold_position
                  : bank_safe_position;
        }
        if (!bank_route_in_progress && !bank_route_settling) {
          bank_route_hold_position.reset();
        }
        if (bank_activation_volume_entered && bank_current_room == 18U &&
            !bank_source_fallback_attempted) {
          bank_source_fallback_attempted = true;
          const auto source_173_loaded = dispatch_load_fallback(173U);
          const auto source_175_loaded = dispatch_load_fallback(175U);
          const auto sources_activated =
              source_173_loaded && source_175_loaded &&
              dispatch_direct_object_event(173U, 0x0aU) &&
              dispatch_direct_object_event(175U, 0x0aU) &&
              dispatch_direct_object_event(173U, 0x06U) &&
              dispatch_direct_object_event(175U, 0x06U);
          std::cout << "bank-sources-teleport-continuation: loaded="
                    << source_173_loaded << '/' << source_175_loaded
                    << ", activated=" << sources_activated << '\n';
          if (!sources_activated) {
            record_driver_blocker("bank-source-visibility-continuation-fault");
          }
        }
        std::uint32_t descriptor_table{};
        std::uint16_t descriptor_flags{};
        std::uint32_t descriptor_roots{};
        std::uint32_t root_table{};
        auto descriptor_state_read =
            vm.runtime().read32(0x80116adcU, descriptor_table) &&
            vm.runtime().read16(descriptor_table + 10U * 16U,
                                descriptor_flags) &&
            vm.runtime().read32(descriptor_table + 10U * 16U + 4U,
                                descriptor_roots) &&
            vm.runtime().read32(descriptor_table + 10U * 16U + 8U,
                                root_table) &&
            (descriptor_roots & 0xffU) == bank_descriptor_paths.size();
        for (std::size_t root = 0U;
             descriptor_state_read && root < bank_descriptor_paths.size();
             ++root) {
          descriptor_state_read = vm.runtime().read32(
              root_table + static_cast<std::uint32_t>(root * 4U),
              bank_descriptor_paths[root]);
        }
        if (descriptor_state_read) {
          auto active = (descriptor_flags & 0x4000U) != 0U;
          if (!active && bank_activation_volume_entered &&
              bank_current_room == 18U &&
              !bank_descriptor_fallback_attempted) {
            bank_descriptor_fallback_attempted = true;
            const auto activation =
                vm.invoke(0x8005fd04U, std::array{10U}, 5'000'000U);
            const auto flags_read = vm.runtime().read16(
                descriptor_table + 10U * 16U, descriptor_flags);
            active = flags_read && (descriptor_flags & 0x4000U) != 0U;
            std::cout << "bank-descriptor-10-teleport-continuation: completed="
                      << activation.completed() << ", flags=0x" << std::hex
                      << std::uppercase << descriptor_flags << std::dec
                      << ", active=" << active << ", reason="
                      << sf::psx::toString(activation.execution.reason)
                      << ", pc=0x" << std::hex << std::uppercase
                      << activation.execution.pc << std::dec << '\n';
            if (!activation.completed() || !active) {
              record_driver_blocker("bank-descriptor-10-activation-fault");
            }
          }
          const auto remaining = descriptor_flags & 0x3fffU;
          if (bank_reinforcement_goal == 0U && remaining != 0U) {
            // The five roots are candidate paths. The low flag
            // field is the finite actor count; activation consumes
            // the first count in the same guest update.
            bank_reinforcement_goal = remaining + (active ? 1U : 0U);
          }
          if (active && !bank_descriptor_completed) {
            bank_descriptor_completed = true;
            std::cout << "bank-descriptor-10-natural: flags=0x" << std::hex
                      << std::uppercase << descriptor_flags << ", paths=";
            for (const auto path : bank_descriptor_paths) {
              std::cout << " 0x" << path;
            }
            std::cout << std::dec << '\n';
          }
        }

        if (driver_stage_guest_updates == 4U ||
            driver_stage_guest_updates % 120U == 0U) {
          std::cout << "bank-natural-state: update="
                    << driver_stage_guest_updates << ", flags=0x" << std::hex
                    << std::uppercase << descriptor_flags << std::dec
                    << ", descriptor-read=" << descriptor_state_read
                    << ", seen=" << bank_roots_materialized
                    << ", kills=" << bank_reinforcement_kills << '/'
                    << bank_reinforcement_goal << '\n';
          trace_driver_object("bank-protected-state", 173U);
          trace_driver_object("bank-static-state", 175U);
        }

        if (driver_bridge->objects.size() > 175U) {
          const auto &static_hostile = driver_bridge->objects[175U];
          if (static_hostile.simulated && static_hostile.health > 0 &&
              queue_driver_damage(static_hostile, "bank-static-hostile") < 0) {
            record_driver_blocker("bank-static-hostile-damage-fault");
          }
        }
        if (bank_materialized_slot) {
          const auto slot = *bank_materialized_slot;
          if (slot < driver_bridge->objects.size()) {
            const auto &object = driver_bridge->objects[slot];
            const auto generation =
                slot < slots.size() ? slots[slot].generations : 0U;
            const auto key =
                (static_cast<std::uint64_t>(slot) << 32U) | generation;
            const auto bank_path =
                std::ranges::find(bank_descriptor_paths, object.path_pointer) !=
                bank_descriptor_paths.end();
            if (bank_materialized_generation &&
                *bank_materialized_generation != key) {
              const auto damaged =
                  std::ranges::find(damaged_actor_generations,
                                    *bank_materialized_generation) !=
                  damaged_actor_generations.end();
              if (damaged) {
                ++bank_reinforcement_kills;
                std::cout << "bank-materialized-recycled: slot=" << slot
                          << ", generation=" << generation
                          << ", kills=" << bank_reinforcement_kills << '\n';
              }
              bank_materialized_generation.reset();
              if (!bank_path || object.health <= 0) {
                bank_materialized_slot.reset();
              }
            }
            if (!bank_materialized_generation && bank_path &&
                object.health > 0) {
              bank_materialized_generation = key;
              ++bank_roots_materialized;
              std::cout << "bank-materialized-actor: slot=" << slot
                        << ", generation=" << generation << ", path=0x"
                        << std::hex << std::uppercase << object.path_pointer
                        << std::dec << '\n';
            }
            if (bank_materialized_generation &&
                *bank_materialized_generation == key) {
              if (object.health <= 0) {
                ++bank_reinforcement_kills;
                std::cout << "bank-materialized-death: slot=" << slot
                          << ", generation=" << generation
                          << ", kills=" << bank_reinforcement_kills << '\n';
                bank_materialized_generation.reset();
                bank_materialized_slot.reset();
              } else if (queue_driver_damage(object,
                                             "bank-finite-descriptor-10") < 0) {
                record_driver_blocker("bank-finite-hostile-damage-fault");
              }
            }
          }
        }
        if (bank_descriptor_completed && !bank_materialized_slot) {
          const auto materialized = std::ranges::find_if(
              driver_bridge->objects,
              [&](const sf::game::LegacyObjectBridgeState &object) {
                return object.slot >= dynamic_first_slot &&
                       object.class_id == 1 && object.health > 0 &&
                       object.simulated &&
                       std::ranges::find(bank_descriptor_paths,
                                         object.path_pointer) !=
                           bank_descriptor_paths.end();
              });
          if (materialized != driver_bridge->objects.end()) {
            const auto generation = materialized->slot < slots.size()
                                        ? slots[materialized->slot].generations
                                        : 0U;
            bank_materialized_slot =
                static_cast<std::uint16_t>(materialized->slot);
            bank_materialized_generation =
                (static_cast<std::uint64_t>(materialized->slot) << 32U) |
                generation;
            ++bank_roots_materialized;
            std::cout << "bank-materialized-actor: slot=" << materialized->slot
                      << ", generation=" << generation
                      << ", root=" << bank_roots_materialized << ", path=0x"
                      << std::hex << std::uppercase
                      << materialized->path_pointer << std::dec << '\n';
          }
        }
        const auto live_bank_root = std::ranges::any_of(
            driver_bridge->objects,
            [&](const sf::game::LegacyObjectBridgeState &object) {
              return object.slot >= dynamic_first_slot &&
                     object.class_id == 1 && object.health > 0 &&
                     std::ranges::find(bank_descriptor_paths,
                                       object.path_pointer) !=
                         bank_descriptor_paths.end();
            });
        const auto descriptor_remaining = descriptor_flags & 0x3fffU;
        const auto last_root_stranded =
            bank_descriptor_completed && descriptor_state_read &&
            descriptor_remaining == 1U && bank_reinforcement_goal != 0U &&
            bank_reinforcement_kills + 1U == bank_reinforcement_goal &&
            !live_bank_root && !bank_materialized_slot;
        if (last_root_stranded) {
          ++bank_last_root_idle_updates;
        } else {
          bank_last_root_idle_updates = 0U;
        }
        if (bank_last_root_idle_updates == 20U) {
          std::optional<std::uint8_t> unseen_root;
          const auto candidate_count = std::min<std::size_t>(
              bank_reinforcement_goal, bank_descriptor_paths.size());
          for (std::size_t root = 0U; root < candidate_count; ++root) {
            const auto seen = std::ranges::any_of(
                lifetimes, [&](const LegacyLevelActorLifetime &lifetime) {
                  return lifetime.slot >= dynamic_first_slot &&
                         lifetime.first_frame >=
                             driver_stage_entry_trace_frame &&
                         lifetime.identity.path_pointer ==
                             bank_descriptor_paths[root];
                });
            if (!seen) {
              if (unseen_root) {
                unseen_root.reset();
                break;
              }
              unseen_root = static_cast<std::uint8_t>(root);
            }
          }
          if (!unseen_root ||
              !materialize_descriptor_actor(10U, dynamic_first_slot,
                                            *unseen_root, 0U)) {
            record_driver_blocker(
                "bank-last-root-materialization-continuation-fault");
          } else {
            std::cout << "bank-last-root-teleport-continuation: root="
                      << static_cast<unsigned int>(*unseen_root) << '\n';
          }
          bank_last_root_idle_updates = 0U;
        }
        if (bank_reinforcement_goal != 0U &&
            bank_reinforcement_kills == bank_reinforcement_goal &&
            !live_bank_root) {
          ++bank_quiescent_updates;
        } else {
          bank_quiescent_updates = 0U;
        }
        break;
      }
      case kravitch_174:
        hold_source(174U);
        if (driver_stage_guest_updates >= 3U &&
            driver_bridge->objects.size() > 174U &&
            driver_bridge->objects[174U].health > 0 &&
            queue_driver_damage(driver_bridge->objects[174U], "kravitch") < 0) {
          record_driver_blocker("kravitch-damage-fault");
        }
        break;
      case radio_260:
        hold_source(260U);
        if (driver_stage_guest_updates >= 3U &&
            driver_bridge->objects.size() > 260U &&
            driver_bridge->objects[260U].health > 0 &&
            queue_driver_damage(driver_bridge->objects[260U],
                                "communications-array") < 0) {
          record_driver_blocker("communications-damage-fault");
        }
        break;
      case bomb_29:
        hold_source(29U);
        if (driver_stage_guest_updates == 4U) {
          driver_pad.buttons = 0x1000U;
          std::cout << "driver-triangle: frame=" << frame
                    << ", stage=" << legacyLevelDriverStageName(driver_stage)
                    << '\n';
        }
        if (driver_stage_guest_updates == 8U &&
            (driver_mission->completed_objectives & 0x02U) == 0U &&
            !bomb_29_callback_attempted) {
          bomb_29_callback_attempted = true;
          const auto callback =
              vm.invoke(0x801485b8U, std::array{29U}, 5'000'000U);
          bomb_29_callback_completed = callback.completed();
          std::cout << "probe-bomb-callback: source=29"
                    << ", production-interaction=0"
                    << ", completed=" << callback.completed()
                    << ", reason="
                    << sf::psx::toString(callback.execution.reason)
                    << ", pc=0x" << std::hex << std::uppercase
                    << callback.execution.pc << std::dec << '\n';
          if (!callback.completed()) {
            record_driver_blocker("source-29-bomb-callback-fault");
          }
        }
        break;
      case trigger_190:
        cross_trigger(190U, {12'120, -2'144, 11'349}, {12'323, -2'144, 11'345});
        if (driver_stage_guest_updates == 8U &&
            driver_bridge->objects.size() > 190U &&
            (driver_bridge->objects[190U].attributes & 0x20U) == 0U) {
          static_cast<void>(dispatch_trigger_fallback(3U, 190U));
        }
        break;
      case trigger_194:
        cross_trigger(194U, {753, -2'224, 3'661}, {754, -2'224, 3'157});
        if (driver_stage_guest_updates == 8U &&
            driver_bridge->objects.size() > 194U &&
            (driver_bridge->objects[194U].attributes & 0x20U) == 0U) {
          static_cast<void>(dispatch_trigger_fallback(4U, 194U));
        }
        break;
      case power_317:
        if (!power_scripted_transition_completed) {
          break;
        }
        ++power_post_transition_updates;
        if (power_post_transition_updates <= 2U) {
          hold_source(317U);
        }
        if (power_post_transition_updates == 2U ||
            power_post_transition_updates == 3U) {
          trace_driver_object("power-317-before-interaction", 317U);
          std::uint32_t records{};
          std::uint32_t word_28{};
          std::uint32_t word_30{};
          std::uint32_t word_34{};
          const auto raw = vm.runtime().read32(0x80115cccU, records) &&
                           vm.runtime().read32(
                               records + 317U * 0x4cU + 0x28U, word_28) &&
                           vm.runtime().read32(
                               records + 317U * 0x4cU + 0x30U, word_30) &&
                           vm.runtime().read32(
                               records + 317U * 0x4cU + 0x34U, word_34);
          const auto record =
              records + static_cast<std::uint32_t>(317U * 0x4cU);
          std::cout << "power-317-raw: update="
                    << power_post_transition_updates << ", read=" << raw
                    << ", base=0x" << std::hex << std::uppercase << records
                    << ", record=0x" << record << ", +28=0x" << word_28
                    << ", +30=0x" << word_30 << ", +34=0x" << word_34
                    << std::dec << '\n';
        }
        if (power_post_transition_updates == 4U) {
          driver_pad.buttons = 0x1000U;
          std::cout << "driver-triangle: frame=" << frame
                    << ", stage=power-317\n";
        }
        if (power_post_transition_updates == 8U &&
            (driver_mission->completed_objectives & 0x04U) == 0U) {
          const std::array arguments{
              0x12U, 5U, 317U, 317U, 0U, 0U, 0U, 0U,
          };
          const auto interaction =
              vm.invoke(0x80015364U, arguments, 5'000'000U);
          std::cout << "power-317-event12-continuation: priority=5"
                    << ", completed=" << interaction.completed()
                    << ", reason="
                    << sf::psx::toString(interaction.execution.reason)
                    << ", pc=0x" << std::hex << std::uppercase
                    << interaction.execution.pc << std::dec << '\n';
          if (!interaction.completed()) {
            record_driver_blocker("power-317-event12-continuation-fault");
          }
        }
        if (power_post_transition_updates == 12U &&
            (driver_mission->completed_objectives & 0x04U) == 0U) {
          std::uint32_t records{};
          std::uint32_t linked_source{};
          const auto link_guard =
              vm.runtime().read32(0x80115cccU, records) &&
              vm.runtime().read32(records + 317U * 0x4cU + 0x30U,
                                  linked_source) &&
              linked_source == 68U;
          std::optional<sf::game::LegacyGameplayVmResult> callback;
          if (link_guard) {
            callback.emplace(
                vm.invoke(0x801488c8U, std::array{317U}, 5'000'000U));
          }
          const auto mission_after_callback = vm.readMissionBridgeState();
          std::cout << "probe-power-callback: source=317"
                    << ", link-guard=" << link_guard
                    << ", linked=" << linked_source
                    << ", completed="
                    << (callback && callback->completed())
                    << ", objective2="
                    << (mission_after_callback &&
                        (mission_after_callback->completed_objectives &
                         0x04U) != 0U);
          if (callback) {
            std::cout << ", reason="
                      << sf::psx::toString(callback->execution.reason)
                      << ", pc=0x" << std::hex << std::uppercase
                      << callback->execution.pc << std::dec;
          }
          std::cout << '\n';
          if (!link_guard) {
            record_driver_blocker("power-317-overlay-link-guard-fault");
          } else if (!callback || !callback->completed()) {
            record_driver_blocker("power-317-overlay-callback-fault");
          }
        }
        if (power_post_transition_updates == 16U &&
            (driver_mission->completed_objectives & 0x04U) == 0U &&
            (driver_mission->revealed_objectives & 0x04U) != 0U &&
            (driver_mission->notified_objectives & 0x04U) != 0U) {
          driver_pad.buttons = 0x1000U;
          std::cout << "driver-triangle: frame=" << frame
                    << ", stage=power-317, phase=revealed\n";
        }
        if (power_post_transition_updates == 20U &&
            (driver_mission->completed_objectives & 0x04U) == 0U &&
            (driver_mission->revealed_objectives & 0x04U) != 0U &&
            (driver_mission->notified_objectives & 0x04U) != 0U) {
          const std::array arguments{
              0x12U, 5U, 317U, 317U, 0U, 0U, 0U, 0U,
          };
          const auto interaction =
              vm.invoke(0x80015364U, arguments, 5'000'000U);
          std::cout << "power-317-event12-continuation: priority=5"
                    << ", phase=revealed"
                    << ", completed=" << interaction.completed()
                    << ", reason="
                    << sf::psx::toString(interaction.execution.reason)
                    << ", pc=0x" << std::hex << std::uppercase
                    << interaction.execution.pc << std::dec << '\n';
          if (!interaction.completed()) {
            record_driver_blocker(
                "power-317-revealed-event12-continuation-fault");
          }
        }
        if (power_post_transition_updates == 24U &&
            (driver_mission->completed_objectives & 0x04U) == 0U &&
            (driver_mission->revealed_objectives & 0x04U) != 0U &&
            (driver_mission->notified_objectives & 0x04U) != 0U) {
          std::uint32_t records{};
          std::uint32_t linked_source{};
          const auto bridge = vm.readBridgeState();
          const auto link_guard =
              vm.runtime().read32(0x80115cccU, records) &&
              vm.runtime().read32(records + 317U * 0x4cU + 0x30U,
                                  linked_source) &&
              linked_source == 68U && bridge &&
              bridge->objects.size() > 68U &&
              bridge->objects[68U].class_id == 0x54;
          const auto player_source =
              driver_mission->player_slot >= 0
                  ? static_cast<std::uint16_t>(driver_mission->player_slot)
                  : std::numeric_limits<std::uint16_t>::max();
          const auto event_completed =
              link_guard && player_source !=
                                std::numeric_limits<std::uint16_t>::max() &&
              dispatch_direct_object_event_from(317U, 0x14U, player_source);
          const auto bridge_after_event = vm.readBridgeState();
          const auto mission_after_event = vm.readMissionBridgeState();
          std::uint32_t overlay_flags{};
          static_cast<void>(
              vm.runtime().read32(0x80149910U, overlay_flags));
          const auto instance_latched =
              bridge_after_event && bridge_after_event->objects.size() > 317U &&
              (bridge_after_event->objects[317U].instance_flags & 0x20U) != 0U;
          std::cout << "probe-power-switch-event14: source=317"
                    << ", link-guard=" << link_guard
                    << ", linked=" << linked_source
                    << ", linked-class=0x" << std::hex << std::uppercase
                    << (bridge && bridge->objects.size() > 68U
                            ? static_cast<std::uint16_t>(
                                  bridge->objects[68U].class_id)
                            : 0xffffU)
                    << ", overlay-flags=0x" << overlay_flags << std::dec
                    << ", completed=" << event_completed
                    << ", instance-latched=" << instance_latched
                    << ", objective2="
                    << (mission_after_event &&
                        (mission_after_event->completed_objectives &
                         0x04U) != 0U);
          std::cout << '\n';
          if (!link_guard) {
            record_driver_blocker("power-317-switch-link-guard-fault");
          } else if (!event_completed || !instance_latched) {
            record_driver_blocker("power-317-switch-event14-fault");
          }
        }
        break;
      case trigger_192:
        cross_trigger(192U, {-666, -1'198, 4'925}, {-204, -1'198, 4'925});
        if (driver_stage_guest_updates == 8U &&
            driver_bridge->objects.size() > 192U &&
            (driver_bridge->objects[192U].attributes & 0x20U) == 0U) {
          static_cast<void>(dispatch_trigger_fallback(5U, 192U));
        }
        break;
      case trigger_193:
        cross_trigger(193U, {143, -131, 4'808}, {336, -131, 4'807});
        if (driver_stage_guest_updates == 8U &&
            driver_bridge->objects.size() > 193U &&
            (driver_bridge->objects[193U].attributes & 0x20U) == 0U) {
          static_cast<void>(dispatch_trigger_fallback(6U, 193U));
        }
        break;
      case elevator_315:
        if (!elevator_315_motion_completed) {
          interact_after_settle(315U);
        } else if (!elevator_return_started) {
          ++elevator_passenger_boarding_updates;
          if (elevator_passenger_boarding_updates <= 4U &&
              driver_bridge->objects.size() > 62U) {
            constexpr std::int32_t passenger_root_offset_y = -127;
            const auto &platform = driver_bridge->objects[62U].position;
            applied_host_position = sf::game::LegacyNativePoint{
                platform.x,
                platform.y + passenger_root_offset_y,
                platform.z,
            };
            elevator_passenger_positioned = true;
            if (elevator_passenger_boarding_updates == 1U) {
              std::cout << "probe-elevator-passenger-board: linked=62"
                        << ", platform=(" << platform.x << ',' << platform.y
                        << ',' << platform.z << "), player-root=("
                        << applied_host_position->x << ','
                        << applied_host_position->y << ','
                        << applied_host_position->z << ")\n";
            }
          }
        }
        if (driver_stage_guest_updates == 1U ||
            driver_stage_guest_updates == 12U ||
            driver_stage_guest_updates == 20U ||
            driver_stage_guest_updates == 40U ||
            driver_stage_guest_updates == 80U ||
            driver_stage_guest_updates == 100U ||
            driver_stage_guest_updates == 120U ||
            driver_stage_guest_updates == 140U) {
          trace_driver_object("elevator-315-switch", 315U);
          trace_driver_object("elevator-315-linked", 62U);
        }
        if (driver_stage_guest_updates == 12U &&
            !elevator_315_event14_attempted) {
          elevator_315_event14_attempted = true;
          const auto player_source =
              driver_mission->player_slot >= 0
                  ? static_cast<std::uint16_t>(driver_mission->player_slot)
                  : std::numeric_limits<std::uint16_t>::max();
          const auto guard =
              driver_bridge->objects.size() > 315U &&
              driver_bridge->objects[315U].class_id == 0x24 &&
              driver_bridge->objects[315U].parameter == 0 &&
              driver_bridge->objects[315U].linked_slot == 62 &&
              driver_bridge->objects.size() > 62U &&
              driver_bridge->objects[62U].class_id == 0x0b &&
              (driver_mission->completed_objectives & 0x04U) != 0U &&
              player_source != std::numeric_limits<std::uint16_t>::max();
          elevator_315_event14_completed =
              guard && dispatch_direct_object_event_from(
                           315U, 0x14U, player_source);
          std::cout << "probe-elevator-switch-event14: source=315"
                    << ", guard=" << guard << ", player=" << player_source
                    << ", parameter="
                    << (driver_bridge->objects.size() > 315U
                            ? driver_bridge->objects[315U].parameter
                            : -1)
                    << ", link="
                    << (driver_bridge->objects.size() > 315U
                            ? driver_bridge->objects[315U].linked_slot
                            : -1)
                    << ", linked-class=0x" << std::hex << std::uppercase
                    << (driver_bridge->objects.size() > 62U
                            ? static_cast<std::uint16_t>(
                                  driver_bridge->objects[62U].class_id)
                            : 0U)
                    << std::dec << ", completed="
                    << elevator_315_event14_completed << '\n';
          if (!guard) {
            record_driver_blocker("elevator-315-switch-event14-guard-fault");
          } else if (!elevator_315_event14_completed) {
            record_driver_blocker("elevator-315-switch-event14-fault");
          }
        }
        break;
      case elevator_316:
        interact_after_settle(316U);
        if (driver_stage_guest_updates == 1U ||
            driver_stage_guest_updates == 12U ||
            driver_stage_guest_updates == 40U ||
            driver_stage_guest_updates == 80U ||
            driver_stage_guest_updates == 120U) {
          trace_driver_object("elevator-316-switch", 316U);
          trace_driver_object("elevator-316-linked", 62U);
        }
        if (driver_stage_guest_updates == 12U &&
            !elevator_316_event14_attempted) {
          elevator_316_event14_attempted = true;
          const auto player_source =
              driver_mission->player_slot >= 0
                  ? static_cast<std::uint16_t>(driver_mission->player_slot)
                  : std::numeric_limits<std::uint16_t>::max();
          const auto guard =
              driver_bridge->objects.size() > 316U &&
              driver_bridge->objects[316U].class_id == 0x24 &&
              driver_bridge->objects[316U].parameter == 0 &&
              driver_bridge->objects[316U].linked_slot == 62 &&
              driver_bridge->objects.size() > 62U &&
              driver_bridge->objects[62U].class_id == 0x0b &&
              player_source != std::numeric_limits<std::uint16_t>::max();
          elevator_316_event14_completed =
              guard && dispatch_direct_object_event_from(
                           316U, 0x14U, player_source);
          std::cout << "probe-elevator-switch-event14: source=316"
                    << ", guard=" << guard << ", player=" << player_source
                    << ", link="
                    << (driver_bridge->objects.size() > 316U
                            ? driver_bridge->objects[316U].linked_slot
                            : -1)
                    << ", linked-class=0x" << std::hex << std::uppercase
                    << (driver_bridge->objects.size() > 62U
                            ? static_cast<std::uint16_t>(
                                  driver_bridge->objects[62U].class_id)
                            : 0U)
                    << std::dec << ", completed="
                    << elevator_316_event14_completed << '\n';
          if (!guard) {
            record_driver_blocker("elevator-316-switch-event14-guard-fault");
          } else if (!elevator_316_event14_completed) {
            record_driver_blocker("elevator-316-switch-event14-fault");
          }
        }
        break;
      case bomb_28:
        interact_after_settle(28U);
        if (driver_stage_guest_updates == 4U) {
          driver_pad.buttons = 0x1000U;
          std::cout << "driver-triangle: frame=" << frame
                    << ", stage=bomb-28\n";
        }
        if (driver_stage_guest_updates == 8U &&
            !bomb_28_callback_attempted &&
            (driver_mission->completed_objectives & 0x08U) == 0U) {
          bomb_28_callback_attempted = true;
          const auto guard =
              driver_bridge->objects.size() > 28U &&
              driver_bridge->objects[28U].class_id == 0x2e;
          std::optional<sf::game::LegacyGameplayVmResult> callback;
          if (guard) {
            callback =
                vm.invoke(0x801485b8U, std::array{28U}, 5'000'000U);
            bomb_28_callback_completed = callback->completed();
          }
          const auto mission_after_callback = vm.readMissionBridgeState();
          std::cout << "probe-bomb-callback: source=28, guard=" << guard
                    << ", completed=" << bomb_28_callback_completed
                    << ", objective3="
                    << (mission_after_callback &&
                        (mission_after_callback->completed_objectives &
                         0x08U) != 0U);
          if (callback) {
            std::cout << ", reason="
                      << sf::psx::toString(callback->execution.reason)
                      << ", pc=0x" << std::hex << std::uppercase
                      << callback->execution.pc << std::dec;
          }
          std::cout << '\n';
          if (!guard) {
            record_driver_blocker("bomb-28-objective-callback-guard-fault");
          } else if (!bomb_28_callback_completed) {
            record_driver_blocker("bomb-28-objective-callback-fault");
          }
        }
        break;
      case trigger_191:
        cross_trigger(191U, {-647, -1'183, 3'382}, {12, -1'183, 3'382});
        if (driver_stage_guest_updates == 8U &&
            driver_bridge->objects.size() > 191U &&
            (driver_bridge->objects[191U].attributes & 0x20U) == 0U) {
          static_cast<void>(dispatch_trigger_fallback(7U, 191U));
        }
        break;
      case trigger_258:
        cross_trigger(258U, {685, -24, -10'492}, {1'102, -24, -10'497});
        if (driver_stage_guest_updates == 8U &&
            driver_bridge->objects.size() > 258U &&
            (driver_bridge->objects[258U].attributes & 0x20U) == 0U) {
          static_cast<void>(dispatch_trigger_fallback(8U, 258U));
        }
        break;
      case station_318:
        if (!station_318_scripted_transition_completed) {
          hold_source(318U);
          break;
        }
        interact_after_settle(318U);
        if (driver_stage_guest_updates == 1U ||
            driver_stage_guest_updates == 12U ||
            driver_stage_guest_updates == 40U ||
            driver_stage_guest_updates == 80U ||
            driver_stage_guest_updates == 120U) {
          trace_driver_object("station-318-switch", 318U);
          trace_driver_object("station-318-linked", 342U);
        }
        if (driver_stage_guest_updates == 12U &&
            !station_318_event14_attempted) {
          station_318_event14_attempted = true;
          const auto player_source =
              driver_mission->player_slot >= 0
                  ? static_cast<std::uint16_t>(driver_mission->player_slot)
                  : std::numeric_limits<std::uint16_t>::max();
          const auto guard =
              driver_bridge->objects.size() > 342U &&
              driver_bridge->objects[318U].class_id == 0x24 &&
              driver_bridge->objects[318U].linked_slot == 342 &&
              driver_bridge->objects[342U].class_id == 0x0b &&
              player_source != std::numeric_limits<std::uint16_t>::max();
          std::optional<sf::game::LegacyGameplayVmResult> event;
          if (guard) {
            const std::array arguments{
                0x14U, 3U, static_cast<std::uint32_t>(player_source), 318U,
                0U,    0U, 0U,                                        0U,
            };
            event = vm.invoke(0x80015364U, arguments, 5'000'000U);
            station_318_event14_completed = event->completed();
          }
          std::cout << "probe-station-switch-event14: source=318"
                    << ", mode=event-entry, guard=" << guard
                    << ", completed=" << station_318_event14_completed
                    << '\n';
          if (!guard || !station_318_event14_completed) {
            record_driver_blocker("station-318-event14-queue-fault");
          }
        }
        if (driver_stage_guest_updates == 20U &&
            !station_318_motion_started &&
            !station_318_handler_fallback_attempted) {
          station_318_handler_fallback_attempted = true;
          const auto player_source = static_cast<std::uint16_t>(
              std::max<std::int16_t>(driver_mission->player_slot, 0));
          const auto completed = dispatch_direct_object_event_from(
              318U, 0x14U, player_source);
          std::cout << "probe-station-switch-event14: source=318"
                    << ", mode=direct-handler, completed=" << completed
                    << '\n';
          if (!completed) {
            record_driver_blocker("station-318-event14-handler-fault");
          }
        }
        break;
      case station_319:
        interact_after_settle(319U);
        if (driver_stage_guest_updates == 1U ||
            driver_stage_guest_updates == 12U ||
            driver_stage_guest_updates == 40U ||
            driver_stage_guest_updates == 80U ||
            driver_stage_guest_updates == 120U) {
          trace_driver_object("station-319-switch", 319U);
          trace_driver_object("station-319-linked", 342U);
        }
        if (driver_stage_guest_updates == 12U &&
            !station_319_event14_attempted) {
          station_319_event14_attempted = true;
          const auto player_source =
              driver_mission->player_slot >= 0
                  ? static_cast<std::uint16_t>(driver_mission->player_slot)
                  : std::numeric_limits<std::uint16_t>::max();
          const auto guard =
              driver_bridge->objects.size() > 342U &&
              driver_bridge->objects[319U].class_id == 0x7b &&
              driver_bridge->objects[319U].linked_slot == 342 &&
              driver_bridge->objects[342U].class_id == 0x0b &&
              player_source != std::numeric_limits<std::uint16_t>::max();
          std::optional<sf::game::LegacyGameplayVmResult> event;
          if (guard) {
            const std::array arguments{
                0x14U, 3U, static_cast<std::uint32_t>(player_source), 319U,
                0U,    0U, 0U,                                        0U,
            };
            event = vm.invoke(0x80015364U, arguments, 5'000'000U);
            station_319_event14_completed = event->completed();
          }
          std::cout << "probe-station-switch-event14: source=319"
                    << ", mode=event-entry, guard=" << guard
                    << ", completed=" << station_319_event14_completed
                    << '\n';
          if (!guard || !station_319_event14_completed) {
            record_driver_blocker("station-319-event14-queue-fault");
          }
        }
        if (driver_stage_guest_updates == 20U &&
            !station_319_motion_started &&
            !station_319_handler_fallback_attempted) {
          station_319_handler_fallback_attempted = true;
          const auto player_source = static_cast<std::uint16_t>(
              std::max<std::int16_t>(driver_mission->player_slot, 0));
          const auto completed = dispatch_direct_object_event_from(
              319U, 0x14U, player_source);
          std::cout << "probe-station-switch-event14: source=319"
                    << ", mode=direct-handler, completed=" << completed
                    << '\n';
          if (!completed) {
            record_driver_blocker("station-319-event14-handler-fault");
          }
        }
        break;
      case trigger_259:
        cross_trigger(259U, {-676, 548, -11'943}, {-7, 548, -11'940});
        if (driver_stage_guest_updates == 8U &&
            driver_bridge->objects.size() > 259U &&
            (driver_bridge->objects[259U].attributes & 0x20U) == 0U) {
          static_cast<void>(dispatch_trigger_fallback(9U, 259U));
        }
        break;
      case finale_30:
        // Source 30 is the protected lower subway bomb.  Its death callback
        // is mission failure; the retail success callback is proximity-only
        // after the upper bomb has been tagged.  Cross the 900-unit radius
        // from a real outside sample and let the overlay own the ending.
        {
          const auto inside = source_host_position(30U);
          auto outside = inside;
          outside.x -= 1'200;
          cross_trigger(30U, outside, inside);
          if (driver_stage_guest_updates == 1U ||
              driver_stage_guest_updates == 3U) {
            const auto &position = driver_stage_guest_updates == 1U
                                       ? outside
                                       : inside;
            std::cout << "probe-finale-proximity: source=30, phase="
                      << (driver_stage_guest_updates == 1U ? "outside"
                                                          : "inside")
                      << ", position=(" << position.x << ',' << position.y
                      << ',' << position.z << ")\n";
          }
          if (driver_stage_guest_updates == 8U &&
              !finale_callback_attempted) {
            finale_callback_attempted = true;
            const auto guard = driver_bridge->objects.size() > 30U &&
                               driver_bridge->objects[30U].class_id == 0x58 &&
                               driver_bridge->objects[30U].linked_slot == -1 &&
                               (driver_mission->completed_objectives & 0x08U) !=
                                   0U;
            std::optional<sf::game::LegacyGameplayVmResult> callback;
            if (guard) {
              callback.emplace(
                  vm.invoke(0x801488c8U, std::array{30U}, 5'000'000U));
              finale_callback_completed = callback->completed();
            }
            std::cout << "probe-finale-callback: source=30, guard=" << guard
                      << ", completed=" << finale_callback_completed;
            if (callback) {
              std::cout << ", reason="
                        << sf::psx::toString(callback->execution.reason)
                        << ", pc=0x" << std::hex << std::uppercase
                        << callback->execution.pc << std::dec;
            }
            std::cout << '\n';
            if (!guard) {
              record_driver_blocker("finale-overlay-callback-guard-fault");
            } else if (!finale_callback_completed) {
              record_driver_blocker("finale-overlay-callback-fault");
            }
          }
        }
        break;
      }
    }
    if (applied_host_position) {
      const auto previous_position = previous_applied_host_position
                                         ? *previous_applied_host_position
                                         : *applied_host_position;
      const sf::game::LegacyHostPlayerState player{
          *applied_host_position,
          0,
          150,
          600,
          previous_position,
          previous_applied_host_position.has_value(),
      };
      if (!vm.writeHostPlayerState(player)) {
        std::cout << "legacy-level-player-bridge-fault: frame=" << frame
                  << '\n';
        return 31;
      }
      previous_applied_host_position = *applied_host_position;
    }
    if (room_bootstrap_source && !force_driver_room(*room_bootstrap_source)) {
      record_driver_blocker("source-" + std::to_string(*room_bootstrap_source) +
                            "-room-activation-fault");
    }
    if (!vm.writeHostPadState(driver_pad)) {
      std::cout << "legacy-level-pad-bridge-fault: frame=" << frame << '\n';
      return 31;
    }
    std::uint32_t state_after_driver{};
    if (!vm.runtime().read32(0x80115c78U, state_after_driver)) {
      return 31;
    }
    if (state_after_driver == 2U && !non_gameplay_transition_traced) {
      non_gameplay_transition_traced = true;
      std::uint32_t next_state{};
      std::uint32_t state_depth{};
      std::uint32_t movie_callback{};
      std::uint32_t loader_callback{};
      std::uint16_t fade_step{};
      std::uint16_t fade_current{};
      std::uint32_t fade_callback{};
      std::uint8_t terminal{};
      std::uint8_t success_latch{};
      std::uint8_t transition_latch{};
      std::uint8_t failure{};
      std::uint8_t completed{};
      const auto raw =
          vm.runtime().read32(0x80115c7cU, next_state) &&
          vm.runtime().read32(0x80115c74U, state_depth) &&
          vm.runtime().read32(0x80115c80U, movie_callback) &&
          vm.runtime().read32(0x80116b04U, loader_callback) &&
          vm.runtime().read16(0x801164d8U, fade_step) &&
          vm.runtime().read16(0x801164daU, fade_current) &&
          vm.runtime().read32(0x801164e0U, fade_callback) &&
          vm.runtime().read8(0x80115cc8U, terminal) &&
          vm.runtime().read8(0x80115cc9U, success_latch) &&
          vm.runtime().read8(0x80115ccaU, transition_latch) &&
          vm.runtime().read8(0x80116b24U, failure) &&
          vm.runtime().read8(0x80116b25U, completed);
      const auto transition_mission = vm.readMissionBridgeState();
      std::cout << "non-gameplay-transition: frame=" << frame
                << ", stage=" << legacyLevelDriverStageName(driver_stage)
                << ", state=" << state_after_driver << '/' << next_state
                << ", depth=" << state_depth << ", raw=" << raw << ':'
                << static_cast<unsigned int>(terminal) << '/'
                << static_cast<unsigned int>(success_latch) << '/'
                << static_cast<unsigned int>(transition_latch) << '/'
                << static_cast<unsigned int>(failure) << '/'
                << static_cast<unsigned int>(completed) << ", progress=";
      if (transition_mission) {
        std::cout << "0x" << std::hex
                  << transition_mission->completed_objectives << "/0x"
                  << transition_mission->revealed_objectives << "/0x"
                  << transition_mission->notified_objectives << "/0x"
                  << transition_mission->parameter_mask << std::dec
                  << ", outcome=" << transition_mission->terminal << '/'
                  << transition_mission->success << '/'
                  << transition_mission->failure;
      } else {
        std::cout << "unavailable";
      }
      std::cout << ", fade=0x" << std::hex << fade_current << "/0x"
                << fade_step << "/0x" << fade_callback
                << ", callbacks=0x" << loader_callback << "/0x"
                << movie_callback << std::dec << '\n';
    }
    if (state_after_driver == 2U) {
      std::uint32_t radio_state{};
      std::uint16_t radio_event{};
      std::uint16_t radio_object{};
      std::uint16_t radio_id{};
      std::uint16_t prompt{};
      std::uint8_t transition_byte{};
      std::uint8_t terminal_latch{};
      std::uint8_t success_latch{};
      std::uint8_t failure_latch{};
      std::uint8_t completed_latch{};
      std::array<std::uint32_t, 9U> bindings{};
      auto state2_trace_read =
          vm.runtime().read32(0x80128dacU, radio_state) &&
          vm.runtime().read16(0x80128db0U, radio_event) &&
          vm.runtime().read16(0x80128db2U, radio_object) &&
          vm.runtime().read16(0x80128db4U, radio_id) &&
          vm.runtime().read16(0x80116374U, prompt) &&
          vm.runtime().read8(0x80115ccaU, transition_byte) &&
          vm.runtime().read8(0x80115cc8U, terminal_latch) &&
          vm.runtime().read8(0x80115cc9U, success_latch) &&
          vm.runtime().read8(0x80116b24U, failure_latch) &&
          vm.runtime().read8(0x80116b25U, completed_latch);
      for (std::size_t index = 0U; index < bindings.size(); ++index) {
        state2_trace_read =
            state2_trace_read &&
            vm.runtime().read32(0x8010baf4U +
                                    static_cast<std::uint32_t>(index) * 4U,
                                bindings[index]);
      }
      std::cout << "probe-state2-retail-input: stage="
                << legacyLevelDriverStageName(driver_stage)
                << ", raw=" << state2_trace_read << ", radio=0x" << std::hex
                << std::uppercase << radio_state << "/0x" << radio_event
                << "/0x" << radio_object << "/0x" << radio_id
                << ", prompt=0x" << prompt << ", transition=0x"
                << static_cast<unsigned int>(transition_byte)
                << ", outcome-latches="
                << static_cast<unsigned int>(terminal_latch) << '/'
                << static_cast<unsigned int>(success_latch) << '/'
                << static_cast<unsigned int>(failure_latch) << '/'
                << static_cast<unsigned int>(completed_latch)
                << ", binding-8010BAF4=[";
      for (std::size_t index = 0U; index < bindings.size(); ++index) {
        std::cout << (index == 0U ? "" : ",") << "0x" << bindings[index];
      }
      std::cout << ']';
      if (driver_bridge && driver_bridge->objects.size() > 30U) {
        const auto &source_30 = driver_bridge->objects[30U];
        std::cout << ", source30=0x" << std::hex
                  << static_cast<std::uint16_t>(source_30.class_id) << "/0x"
                  << source_30.attributes << std::dec << '/'
                  << source_30.health << '/' << source_30.parameter << '/'
                  << source_30.linked_slot;
      } else {
        std::cout << ", source30=unavailable";
      }
      std::cout << std::dec << '\n';
      const auto transition_mission = vm.readMissionBridgeState();
      const auto dispatch_allowed =
          transition_mission && sf::game::legacyRetailState2DispatchAllowed(
                                    state_after_driver, *transition_mission);
      const auto transition = dispatch_allowed
                                  ? vm.dispatchRetailState2Transition()
                                  : sf::game::LegacyRetailState2TransitionResult{};
      if (dispatch_allowed) {
        state_after_driver = transition.final_state;
      }
      std::cout << "probe-state2-dispatch: stage="
                << legacyLevelDriverStageName(driver_stage)
                << ", allowed=" << dispatch_allowed
                << ", completed="
                << (dispatch_allowed && transition.completed())
                << ", dispatches=" << transition.dispatches
                << ", final-state=" << transition.final_state << '\n';
      if (dispatch_allowed && transition.completed() &&
          driver_stage == LegacyLevelDriverStage::power_317) {
        power_scripted_transition_completed = true;
        power_post_transition_updates = 0U;
        driver_stage_frames = 0U;
        driver_stage_guest_updates = 0U;
        driver_stage_entry_trace_frame = frame;
        previous_applied_host_position.reset();
      } else if (dispatch_allowed && transition.completed() &&
                 driver_stage == LegacyLevelDriverStage::station_318 &&
                 !station_318_scripted_transition_completed) {
        station_318_scripted_transition_completed = true;
        station_318_event14_attempted = false;
        station_318_event14_completed = false;
        station_318_handler_fallback_attempted = false;
        station_318_motion_started = false;
        station_318_motion_completed = false;
        driver_stage_frames = 0U;
        driver_stage_guest_updates = 0U;
        driver_stage_entry_trace_frame = frame;
        previous_applied_host_position.reset();
      } else if (dispatch_allowed && !transition.completed()) {
        record_driver_blocker("state2-transition-dispatch-fault");
      }
    }
    const auto driver_gameplay_state =
        state_after_driver == 0U || state_after_driver == 5U;
    // The production loop always enters through the retail outer frame.  Keep
    // the whole trigger/finale tail on that path so the proximity scan and
    // overlay callback scheduling remain coherent.
    const auto finale_retail_proximity_stage =
        driver_stage == LegacyLevelDriverStage::trigger_259 ||
        driver_stage == LegacyLevelDriverStage::finale_30;
    const auto use_native_tick = native_driven && driver_gameplay_state &&
                                 !finale_retail_proximity_stage;
    const auto result = use_native_tick ? vm.tickNativeDrivenGameplayFrame()
                                        : vm.tickRetailOuterFrame();
    use_native_tick ? ++native_updates : ++outer_updates;
    if (!result.completed()) {
      std::cout << "legacy-level-frame-fault: frame=" << frame
                << ", state=" << result.state_before << '/'
                << result.state_after << ", bridge=" << result.bridge_fault
                << ", unsupported=" << result.unsupported_state;
      if (!result.guest_calls.empty()) {
        const auto &execution = result.guest_calls.back().execution;
        std::cout << ", reason=" << sf::psx::toString(execution.reason)
                  << ", pc=0x" << std::hex << std::uppercase << execution.pc
                  << std::dec;
      }
      std::cout << '\n';
      return 30;
    }
    if (!vm.advanceAudioFrameClock()) {
      std::cout << "legacy-level-audio-fault: frame=" << frame << '\n';
      return 30;
    }
    ++completed_frames;

    const auto bridge = vm.readBridgeState();
    const auto mission_state = vm.readMissionBridgeState();
    if (!bridge || !mission_state || bridge->objects.empty()) {
      std::cout << "legacy-level-state-bridge-fault: frame=" << frame << '\n';
      return 31;
    }
    if (slots.empty()) {
      slots.resize(bridge->objects.size());
      dynamic_first_slot = bridge->dynamic_first_slot;
    } else if (slots.size() != bridge->objects.size() ||
               dynamic_first_slot != bridge->dynamic_first_slot) {
      std::cout << "legacy-level-object-table-changed: frame=" << frame
                << ", objects=" << slots.size() << "->"
                << bridge->objects.size() << ", dynamic=" << dynamic_first_slot
                << "->" << bridge->dynamic_first_slot << '\n';
      return 31;
    }

    if (!first_mission) {
      first_mission = *mission_state;
    }
    if (!previous_mission ||
        !sameLegacyMissionState(*previous_mission, *mission_state)) {
      ++mission_transitions;
      std::cout << "mission-transition: frame=" << frame
                << ", player=" << mission_state->player_slot
                << ", hp=" << mission_state->player_health << ", objectives=0x"
                << std::hex << mission_state->completed_objectives << "/0x"
                << mission_state->revealed_objectives << "/0x"
                << mission_state->notified_objectives << ", parameters=0x"
                << mission_state->parameter_mask << std::dec
                << ", success=" << mission_state->success
                << ", terminal=" << mission_state->terminal
                << ", failure=" << mission_state->failure << '\n';
      previous_mission = *mission_state;
    }
    last_mission = *mission_state;

    std::uint8_t checkpoint_latch{};
    std::uint32_t checkpoint_frame{};
    std::uint32_t pending_events{};
    std::uint32_t ready_events{};
    if (!vm.runtime().read8(0x801163b1U, checkpoint_latch) ||
        !vm.runtime().read32(0x80121950U, checkpoint_frame) ||
        !vm.runtime().read32(0x80116c68U, pending_events) ||
        !vm.runtime().read32(0x8011775cU, ready_events)) {
      return 31;
    }
    maximum_pending_events = std::max(maximum_pending_events, pending_events);
    maximum_ready_events = std::max(maximum_ready_events, ready_events);
    if (checkpoint_latch != 0U &&
        (!last_checkpoint_frame ||
         *last_checkpoint_frame != checkpoint_frame)) {
      ++checkpoints;
      last_checkpoint_frame = checkpoint_frame;
      std::cout << "checkpoint: trace-frame=" << frame
                << ", retail-frame=" << checkpoint_frame << '\n';
    }

    if (!native_driven && bridge->objects.size() > 35U &&
        bridge->objects[35U].health <= 0) {
      native_driven = true;
      opening_complete_frame = frame;
      std::cout << "opening-complete: frame=" << frame << '\n';
      enter_driver_stage(LegacyLevelDriverStage::clear_opening, frame);
    }
    if (native_driven && mission_state->player_slot >= 0 &&
        static_cast<std::size_t>(mission_state->player_slot) <
            bridge->objects.size()) {
      const auto &player =
          bridge->objects[static_cast<std::size_t>(mission_state->player_slot)];
      if (!post_opening_player_origin) {
        post_opening_player_origin = player.position;
      } else if (!sameLegacyPoint(*post_opening_player_origin,
                                  player.position)) {
        post_opening_player_moved = true;
      }
    }

    for (const auto &object : bridge->objects) {
      auto &trace = slots[object.slot];
      const auto allocated = legacyActorAllocated(object, dynamic_first_slot);
      if (!allocated) {
        if (trace.lifetime) {
          lifetimes[*trace.lifetime].retired = true;
          trace.lifetime.reset();
        }
        continue;
      }
      const LegacyLevelActorIdentity identity{
          object.class_id,          object.definition,   object.parameter,
          object.authored_position, object.path_pointer,
      };
      const auto restarted = trace.lifetime &&
                             lifetimes[*trace.lifetime].died &&
                             object.health > 0;
      if (!trace.lifetime || restarted ||
          !sameLegacyActorIdentity(lifetimes[*trace.lifetime].identity,
                                   identity)) {
        if (trace.lifetime) {
          lifetimes[*trace.lifetime].retired = true;
          trace.lifetime.reset();
        }
        begin_lifetime(object, frame);
      }

      auto &lifetime = lifetimes[*trace.lifetime];
      const auto pose_fingerprint = legacyPoseFingerprint(object);
      const auto target = object.has_target ? object.target_slot : -1;
      lifetime.last_frame = frame;
      lifetime.minimum_health =
          std::min(lifetime.minimum_health, object.health);
      lifetime.end_health = object.health;
      lifetime.saw_positive_health =
          lifetime.saw_positive_health || object.health > 0;
      lifetime.died =
          lifetime.died || (lifetime.saw_positive_health && object.health <= 0);
      lifetime.moved = lifetime.moved ||
                       !sameLegacyPoint(lifetime.end_position, object.position);
      lifetime.end_position = object.position;
      lifetime.saw_target = lifetime.saw_target || object.has_target;
      lifetime.target_changes += target != lifetime.last_target ? 1U : 0U;
      lifetime.last_target = static_cast<std::int16_t>(target);
      lifetime.fire_frames += object.ai_fire_latch != 0U ? 1U : 0U;
      lifetime.simulated_frames += object.simulated ? 1U : 0U;
      lifetime.exact_pose_frames +=
          object.bone_matrix_count == sf::game::legacy_actor_bone_count ? 1U
                                                                        : 0U;
      if (pose_fingerprint != lifetime.last_pose_fingerprint) {
        ++lifetime.animation_changes;
        lifetime.current_stagnant_combat_frames = 0U;
      } else if (object.simulated && object.has_target && object.health > 0) {
        ++lifetime.current_stagnant_combat_frames;
        lifetime.longest_stagnant_combat_frames =
            std::max(lifetime.longest_stagnant_combat_frames,
                     lifetime.current_stagnant_combat_frames);
      } else {
        lifetime.current_stagnant_combat_frames = 0U;
      }
      lifetime.last_pose_fingerprint = pose_fingerprint;
      std::uint32_t raw_ground_contact{};
      const auto has_raw_ground_contact =
          object.motion_controller != 0U &&
          vm.runtime().read32(object.motion_controller + 0x12cU,
                              raw_ground_contact);
      const auto packed_ground_sentinel =
          has_raw_ground_contact &&
          (raw_ground_contact & 0xfffffffcu) == 0x80000000U;
      if (packed_ground_sentinel) {
        ++lifetime.packed_ground_sentinel_frames;
        ++packed_ground_sentinel_samples;
      }
      const auto dump_ground_sample =
          (packed_ground_sentinel && !raw_ground_sentinel_dumped) ||
          (!packed_ground_sentinel && raw_ground_dumps < 2U);
      if (native_driven && object.motion_controller != 0U &&
          raw_ground_dumps < 3U && dump_ground_sample) {
        std::array<std::uint32_t, 7U> words{};
        auto complete = true;
        for (std::size_t word = 0U; word < words.size(); ++word) {
          complete = complete && vm.runtime().read32(
                                     object.motion_controller + 0x120U +
                                         static_cast<std::uint32_t>(word * 4U),
                                     words[word]);
        }
        if (complete) {
          std::cout << "motion-ground-raw: frame=" << frame
                    << ", slot=" << object.slot << ", motion=0x" << std::hex
                    << std::uppercase << object.motion_controller << ", words=";
          for (const auto word : words) {
            std::cout << " 0x" << word;
          }
          std::cout << std::dec
                    << ", decoded-valid=" << object.ground_contact_valid
                    << ", decoded-y=" << object.ground_contact_y
                    << ", packed-sentinel=" << packed_ground_sentinel << '\n';
          ++raw_ground_dumps;
          raw_ground_sentinel_dumped =
              raw_ground_sentinel_dumped || packed_ground_sentinel;
        }
      }
      if (object.ground_contact_valid && !packed_ground_sentinel) {
        ++lifetime.ground_frames;
        auto delta = static_cast<std::int64_t>(object.position.y) -
                     static_cast<std::int64_t>(object.ground_contact_y);
        if (delta < 0) {
          delta = -delta;
        }
        lifetime.maximum_ground_delta =
            std::max(lifetime.maximum_ground_delta, delta);
      }
      if (object.has_target && (object.target_slot < 0 ||
                                static_cast<std::size_t>(object.target_slot) >=
                                    bridge->objects.size())) {
        ++invalid_targets;
        std::cout << "invalid-target: frame=" << frame
                  << ", slot=" << object.slot
                  << ", target=" << object.target_slot << '\n';
      }
    }

    std::uint32_t camera_controller{};
    std::uint32_t camera_mode{};
    std::uint32_t camera_lock{};
    if (!vm.runtime().read32(0x80115d84U, camera_controller) ||
        !vm.runtime().read32(0x801191ecU, camera_mode) ||
        !vm.runtime().read32(0x801169e0U, camera_lock)) {
      return 31;
    }
    if (!previous_camera_controller ||
        *previous_camera_controller != camera_controller) {
      camera_controller_changes += previous_camera_controller ? 1U : 0U;
      std::cout << "camera-owner: frame=" << frame << ", controller=0x"
                << std::hex << std::uppercase << camera_controller << std::dec
                << ", driver=" << legacyLevelDriverStageName(driver_stage)
                << '\n';
      previous_camera_controller = camera_controller;
    }
    if (!previous_camera_mode || *previous_camera_mode != camera_mode ||
        !previous_camera_lock || *previous_camera_lock != camera_lock) {
      std::cout << "camera-mode: frame=" << frame << ", mode=0x" << std::hex
                << std::uppercase << camera_mode << ", lock=0x" << camera_lock
                << std::dec
                << ", driver=" << legacyLevelDriverStageName(driver_stage)
                << '\n';
      previous_camera_mode = camera_mode;
      previous_camera_lock = camera_lock;
    }
    if (native_driven && camera_mode == 0x0bU) {
      scripted_camera_rail_seen = true;
      ++scripted_camera_rail_frames;
    }
    if (previous_camera_eye &&
        !sameLegacyPoint(*previous_camera_eye, bridge->camera.eye)) {
      const auto absolute = [](std::int32_t value) {
        return value < 0 ? -static_cast<std::int64_t>(value)
                         : static_cast<std::int64_t>(value);
      };
      const auto delta =
          absolute(bridge->camera.eye.x - previous_camera_eye->x) +
          absolute(bridge->camera.eye.y - previous_camera_eye->y) +
          absolute(bridge->camera.eye.z - previous_camera_eye->z);
      if (delta > 2'000) {
        if (camera_discontinuities < 16U) {
          std::cout << "camera-discontinuity: frame=" << frame
                    << ", delta=" << delta << ", eye=("
                    << previous_camera_eye->x << ',' << previous_camera_eye->y
                    << ',' << previous_camera_eye->z << ")->("
                    << bridge->camera.eye.x << ',' << bridge->camera.eye.y
                    << ',' << bridge->camera.eye.z
                    << "), driver=" << legacyLevelDriverStageName(driver_stage)
                    << '\n';
        }
        ++camera_discontinuities;
      }
    }
    previous_camera_eye = bridge->camera.eye;

    const sf::game::LegacyObjectBridgeState *player{};
    if (mission_state->player_slot >= 0 &&
        static_cast<std::size_t>(mission_state->player_slot) <
            bridge->objects.size()) {
      player =
          &bridge
               ->objects[static_cast<std::size_t>(mission_state->player_slot)];
    }
    if (applied_host_position && player != nullptr &&
        !sameLegacyPoint(*applied_host_position, player->position)) {
      if (player_position_overrides < 12U) {
        std::cout << "player-script-override: frame=" << frame << ", expected=("
                  << applied_host_position->x << ',' << applied_host_position->y
                  << ',' << applied_host_position->z << "), actual=("
                  << player->position.x << ',' << player->position.y << ','
                  << player->position.z
                  << "), driver=" << legacyLevelDriverStageName(driver_stage)
                  << '\n';
      }
      ++player_position_overrides;
    }
    if (driver_stage == LegacyLevelDriverStage::elevator_315 &&
        bridge->objects.size() > 62U) {
      const auto moving =
          (bridge->objects[62U].instance_flags & 0x08U) != 0U;
      if (!elevator_315_motion_completed) {
        elevator_315_motion_started = elevator_315_motion_started || moving;
        if (elevator_315_motion_started && !moving &&
            driver_stage_guest_updates >= 12U) {
          elevator_315_motion_completed = true;
          std::cout << "scripted-elevator-motion: switch=315, phase=lower"
                    << ", linked=62, position=("
                    << bridge->objects[62U].position.x << ','
                    << bridge->objects[62U].position.y << ','
                    << bridge->objects[62U].position.z << ")\n";
        }
      } else if (elevator_passenger_positioned) {
        if (!elevator_passenger_board_y && player != nullptr) {
          elevator_passenger_board_y = player->position.y;
        }
        if (!elevator_return_started && moving) {
          elevator_return_started = true;
          std::cout << "scripted-elevator-motion: switch=315, phase=return-start"
                    << ", linked=62, platform-y="
                    << bridge->objects[62U].position.y << ", player-y="
                    << (player != nullptr ? player->position.y : 0) << '\n';
        }
        if (elevator_return_started && elevator_passenger_board_y &&
            player != nullptr &&
            std::abs(static_cast<std::int64_t>(player->position.y) -
                     *elevator_passenger_board_y) > 512) {
          elevator_passenger_carried = true;
        }
        if (elevator_return_started && !moving &&
            !elevator_return_completed) {
          elevator_return_completed = true;
          std::cout << "scripted-elevator-motion: switch=315, phase=return-end"
                    << ", linked=62, position=("
                  << bridge->objects[62U].position.x << ','
                  << bridge->objects[62U].position.y << ','
                    << bridge->objects[62U].position.z << "), player-y="
                    << (player != nullptr ? player->position.y : 0)
                    << ", passenger-carried="
                    << elevator_passenger_carried << '\n';
        }
      }
    }
    if (driver_stage == LegacyLevelDriverStage::elevator_316 &&
        bridge->objects.size() > 62U) {
      const auto moving =
          (bridge->objects[62U].instance_flags & 0x08U) != 0U;
      elevator_316_motion_started = elevator_316_motion_started || moving;
      if (elevator_316_motion_started && !moving &&
          driver_stage_guest_updates >= 12U &&
          !elevator_316_motion_completed) {
        elevator_316_motion_completed = true;
        std::cout << "scripted-elevator-motion: switch=316, phase=upper"
                  << ", linked=62, position=("
                  << bridge->objects[62U].position.x << ','
                  << bridge->objects[62U].position.y << ','
                  << bridge->objects[62U].position.z << ")\n";
      }
    }
    if ((driver_stage == LegacyLevelDriverStage::station_318 ||
         driver_stage == LegacyLevelDriverStage::station_319) &&
        bridge->objects.size() > 342U) {
      const auto moving =
          (bridge->objects[342U].instance_flags & 0x08U) != 0U;
      auto &motion_started =
          driver_stage == LegacyLevelDriverStage::station_318
              ? station_318_motion_started
              : station_319_motion_started;
      auto &motion_completed =
          driver_stage == LegacyLevelDriverStage::station_318
              ? station_318_motion_completed
              : station_319_motion_completed;
      motion_started = motion_started || moving;
      if (driver_stage == LegacyLevelDriverStage::station_318 &&
          station_318_event14_completed && !motion_started && !moving &&
          driver_stage_guest_updates >= 24U && !motion_completed &&
          std::abs(static_cast<std::int64_t>(
                       bridge->objects[342U].position.y) -
                   bridge->objects[318U].position.y) <= 256) {
        motion_completed = true;
        std::cout << "scripted-station-motion: switch=318"
                  << ", linked=342, already-at-endpoint=1, position=("
                  << bridge->objects[342U].position.x << ','
                  << bridge->objects[342U].position.y << ','
                  << bridge->objects[342U].position.z << ")\n";
      }
      if (motion_started && !moving && driver_stage_guest_updates >= 12U &&
          !motion_completed) {
        motion_completed = true;
        std::cout << "scripted-station-motion: switch="
                  << (driver_stage == LegacyLevelDriverStage::station_318
                          ? 318U
                          : 319U)
                  << ", linked=342, position=("
                  << bridge->objects[342U].position.x << ','
                  << bridge->objects[342U].position.y << ','
                  << bridge->objects[342U].position.z << ")\n";
      }
    }

    if (driver_stage == LegacyLevelDriverStage::intro_157 &&
        (current_state == 9U || result.state_before == 9U ||
         result.state_after == 9U)) {
      intro_state9_seen = true;
    }
    if (driver_stage == LegacyLevelDriverStage::intro_157 &&
        intro_state9_seen &&
        (result.state_after == 0U || result.state_after == 5U)) {
      intro_state9_returned = true;
    }
    if (driver_stage == LegacyLevelDriverStage::finale_30 &&
        finale_callback_completed &&
        (current_state == 9U || result.state_before == 9U ||
         result.state_after == 9U)) {
      finale_state9_seen = true;
    }
    if (driver_stage == LegacyLevelDriverStage::finale_30 &&
        finale_state9_seen &&
        (result.state_after == 0U || result.state_after == 5U)) {
      finale_state9_returned = true;
    }

    const auto mark_trigger = [&](std::uint16_t source, std::size_t index) {
      trigger_visited[index] = true;
      if (source < bridge->objects.size()) {
        const auto &trigger = bridge->objects[source];
        trigger_observed[index] =
            trigger_observed[index] || (trigger.attributes & 0x20U) != 0U;
        std::cout << "trigger-state: source=" << source
                  << ", resident=" << trigger.resident
                  << ", simulated=" << trigger.simulated
                  << ", hp=" << trigger.health << ", attributes=0x" << std::hex
                  << trigger.attributes << std::dec << ", instance="
                  << static_cast<unsigned int>(trigger.instance_state[0]) << '/'
                  << static_cast<unsigned int>(trigger.instance_state[1]) << '/'
                  << static_cast<unsigned int>(trigger.instance_state[2]) << '/'
                  << static_cast<unsigned int>(trigger.instance_state[3])
                  << '\n';
        if (!trigger_observed[index]) {
          record_driver_blocker("source-" + std::to_string(source) +
                                "-trigger-not-activated");
        }
      }
    };
    const auto source_dead = [&](std::uint16_t source) {
      return source < bridge->objects.size() &&
             bridge->objects[source].health <= 0;
    };
    const auto enter_after_timeout = [&](std::string blocker,
                                         LegacyLevelDriverStage) {
      record_driver_blocker(std::move(blocker));
    };
    using enum LegacyLevelDriverStage;
    switch (driver_stage) {
    case waiting_opening:
    case failure_branch:
    case complete:
      break;
    case clear_opening: {
      const auto live_opening_hostile = std::ranges::any_of(
          bridge->objects,
          [&](const sf::game::LegacyObjectBridgeState &object) {
            return object.class_id == 1 && object.simulated &&
                   object.health > 0 && !object_matches_source(object, 174U) &&
                   !object_matches_source(object, 175U);
          });
      if (!live_opening_hostile && driver_stage_guest_updates >= 8U) {
        enter_driver_stage(trigger_256, frame);
      } else if (driver_stage_frames >= 120U) {
        enter_after_timeout("opening-hostile-clear-timeout", trigger_256);
      }
      break;
    }
    case trigger_256:
      if (driver_stage_guest_updates >= 12U) {
        mark_trigger(256U, 0U);
        enter_driver_stage(passage_64, frame);
      }
      break;
    case passage_64:
      if (driver_stage_guest_updates >= 20U) {
        enter_driver_stage(passage_65, frame);
      }
      break;
    case passage_65:
      if (driver_stage_guest_updates >= 20U) {
        enter_driver_stage(trigger_257, frame);
      }
      break;
    case trigger_257:
      if (driver_stage_guest_updates >= 12U) {
        mark_trigger(257U, 1U);
        enter_driver_stage(intro_157, frame);
      }
      break;
    case intro_157:
      if (intro_state9_seen && intro_state9_returned) {
        enter_driver_stage(lock_140, frame);
      } else if (!intro_state9_seen && driver_stage_guest_updates >= 60U) {
        enter_after_timeout("source-157-state9-not-triggered", lock_140);
      } else if (intro_state9_seen && !intro_state9_returned &&
                 driver_stage_frames >= 240U) {
        record_driver_blocker("source-157-state9-did-not-return");
      }
      break;
    case lock_140: {
      std::uint8_t gate_instance_flags{};
      const auto gate_state_available =
          bridge->objects.size() > 140U &&
          vm.runtime().read8(bridge->objects[67U].instance,
                             gate_instance_flags);
      if (gate_state_available &&
          (bridge->objects[140U].instance_state[3] & 0x02U) != 0U &&
          (gate_instance_flags & 0x08U) != 0U &&
          driver_stage_guest_updates >= 4U) {
        trace_driver_object("lock-script-complete", 140U);
        trace_driver_object("gate-script-complete", 67U);
        enter_driver_stage(kravitch_174, frame);
      } else if (driver_stage_frames >= 40U) {
        trace_driver_object("lock-script-timeout", 140U);
        trace_driver_object("gate-script-timeout", 67U);
        trace_driver_events("gate-script-timeout");
        enter_after_timeout("gate-lock-script-state-missing", kravitch_174);
      }
      break;
    }
    case bank_175: {
      if (bank_descriptor_completed && bank_reinforcement_goal != 0U &&
          bank_roots_materialized == bank_reinforcement_goal &&
          bank_reinforcement_kills == bank_reinforcement_goal &&
          source_dead(175U) && bridge->objects.size() > 173U &&
          bridge->objects[173U].health > 0 && bank_quiescent_updates >= 20U) {
        enter_driver_stage(bomb_29, frame);
      } else if (driver_stage_frames >= 1'000U) {
        enter_after_timeout("finite-bank-wave-not-complete", bomb_29);
      }
      break;
    }
    case kravitch_174:
      if (source_dead(174U) && driver_stage_guest_updates >= 4U) {
        enter_driver_stage(radio_260, frame);
      } else if (driver_stage_frames >= 80U) {
        enter_after_timeout("kravitch-not-killed", radio_260);
      }
      break;
    case radio_260:
      if ((mission_state->completed_objectives & 0x01U) != 0U) {
        enter_driver_stage(bank_175, frame);
      } else if (driver_stage_frames >= 80U) {
        enter_after_timeout("objective-0-kravitch-radio-not-complete",
                            bank_175);
      }
      break;
    case bomb_29:
      if ((mission_state->completed_objectives & 0x02U) != 0U) {
        enter_driver_stage(trigger_190, frame);
      } else if (driver_stage_frames >= 240U) {
        enter_after_timeout("objective-1-bomb-29-not-tagged", trigger_190);
      }
      break;
    case trigger_190:
      if (driver_stage_guest_updates >= 12U) {
        mark_trigger(190U, 4U);
        enter_driver_stage(trigger_194, frame);
      }
      break;
    case trigger_194:
      if (driver_stage_guest_updates >= 12U) {
        mark_trigger(194U, 5U);
        enter_driver_stage(power_317, frame);
      }
      break;
    case power_317:
      if ((mission_state->completed_objectives & 0x04U) != 0U) {
        enter_driver_stage(trigger_192, frame);
      } else if (driver_stage_frames >= 80U) {
        enter_after_timeout("objective-2-power-switch-not-complete",
                            trigger_192);
      }
      break;
    case trigger_192:
      if (driver_stage_guest_updates >= 12U) {
        mark_trigger(192U, 6U);
        enter_driver_stage(trigger_193, frame);
      }
      break;
    case trigger_193:
      if (driver_stage_guest_updates >= 12U) {
        mark_trigger(193U, 7U);
        enter_driver_stage(elevator_315, frame);
      }
      break;
    case elevator_315:
      if (elevator_315_motion_completed) {
        enter_driver_stage(elevator_316, frame);
      } else if (driver_stage_frames >= 180U) {
        enter_after_timeout("elevator-315-motion-did-not-complete",
                            elevator_316);
      }
      break;
    case elevator_316:
      if (elevator_316_motion_completed) {
        enter_driver_stage(bomb_28, frame);
      } else if (driver_stage_frames >= 180U) {
        enter_after_timeout("elevator-316-motion-did-not-complete", bomb_28);
      }
      break;
    case bomb_28:
      if ((mission_state->completed_objectives & 0x08U) != 0U) {
        enter_driver_stage(trigger_191, frame);
      } else if (driver_stage_frames >= 80U) {
        enter_after_timeout("objective-3-bomb-28-not-tagged", trigger_191);
      }
      break;
    case trigger_191:
      if (driver_stage_guest_updates >= 12U) {
        mark_trigger(191U, 8U);
        enter_driver_stage(trigger_258, frame);
      }
      break;
    case trigger_258:
      if (driver_stage_guest_updates >= 12U) {
        mark_trigger(258U, 2U);
        enter_driver_stage(station_318, frame);
      }
      break;
    case station_318:
      if (station_318_motion_completed) {
        enter_driver_stage(station_319, frame);
      } else if (driver_stage_frames >= 180U) {
        enter_after_timeout("station-318-motion-did-not-complete", station_319);
      }
      break;
    case station_319:
      if (station_319_motion_completed) {
        enter_driver_stage(trigger_259, frame);
      } else if (driver_stage_frames >= 180U) {
        enter_after_timeout("station-319-motion-did-not-complete", trigger_259);
      }
      break;
    case trigger_259:
      if (driver_stage_guest_updates >= 12U) {
        mark_trigger(259U, 3U);
        enter_driver_stage(finale_30, frame);
      }
      break;
    case finale_30:
      // FMV presentation is native.  The retail proof boundary is the exact
      // SUBWAY callback entering and returning from its second state-9 movie
      // loader, not synthetic mission completion latches.
      if (finale_callback_completed && finale_state9_seen &&
          finale_state9_returned) {
        scripted_route_complete = true;
        enter_driver_stage(complete, frame);
      } else if (driver_stage_frames >= 400U) {
        record_driver_blocker("finale-success-not-reached");
      }
      break;
    }

    driver_bridge = *bridge;
    driver_mission = *mission_state;

    if (driver_failed || mission_state->terminal || mission_state->failure) {
      break;
    }
  }

  std::size_t static_lifetimes{};
  std::size_t dynamic_lifetimes{};
  std::size_t deaths{};
  std::size_t retired{};
  for (const auto &lifetime : lifetimes) {
    static_cast<void>(lifetime.slot < dynamic_first_slot ? ++static_lifetimes
                                                         : ++dynamic_lifetimes);
    deaths += lifetime.died ? 1U : 0U;
    retired += lifetime.retired ? 1U : 0U;
    if (lifetime.slot < dynamic_first_slot && lifetime.identity.class_id != 0 &&
        !lifetime.saw_target && lifetime.fire_frames == 0U && !lifetime.moved &&
        !lifetime.died) {
      continue;
    }
    std::cout << "actor-lifetime: slot=" << lifetime.slot
              << ", generation=" << lifetime.generation << ", class=0x"
              << std::hex
              << static_cast<std::uint16_t>(lifetime.identity.class_id)
              << ", definition=0x" << lifetime.identity.definition
              << ", path=0x" << lifetime.identity.path_pointer << std::dec
              << ", parameter=" << lifetime.identity.parameter << ", authored=("
              << lifetime.identity.authored_position.x << ','
              << lifetime.identity.authored_position.y << ','
              << lifetime.identity.authored_position.z << ')'
              << ", frames=" << lifetime.first_frame << '-'
              << lifetime.last_frame << ", hp=" << lifetime.start_health << '/'
              << lifetime.minimum_health << '/' << lifetime.end_health
              << ", position=(" << lifetime.start_position.x << ','
              << lifetime.start_position.y << ',' << lifetime.start_position.z
              << ")->(" << lifetime.end_position.x << ','
              << lifetime.end_position.y << ',' << lifetime.end_position.z
              << ')' << ", target=" << lifetime.saw_target << '/'
              << lifetime.target_changes << ", fire=" << lifetime.fire_frames
              << ", animation=" << lifetime.animation_changes
              << ", exact-pose=" << lifetime.exact_pose_frames
              << ", ground=" << lifetime.ground_frames << '/'
              << lifetime.maximum_ground_delta
              << ", ground-sentinel=" << lifetime.packed_ground_sentinel_frames
              << ", stagnant-combat=" << lifetime.longest_stagnant_combat_frames
              << ", died=" << lifetime.died << ", retired=" << lifetime.retired
              << '\n';
  }

  const auto mission_progressed =
      first_mission && last_mission &&
      (first_mission->completed_objectives !=
           last_mission->completed_objectives ||
       first_mission->revealed_objectives !=
           last_mission->revealed_objectives ||
       first_mission->notified_objectives !=
           last_mission->notified_objectives ||
       first_mission->parameter_mask != last_mission->parameter_mask ||
       checkpoints != 0U);
  const auto objectives_asserted = first_mission && last_mission &&
                                   (first_mission->completed_objectives !=
                                        last_mission->completed_objectives ||
                                    last_mission->success);
  const auto triggers_asserted =
      std::ranges::all_of(trigger_visited, std::identity{}) &&
      std::ranges::all_of(trigger_observed, std::identity{});
  const auto scripted_transports_asserted =
      elevator_315_motion_completed && elevator_316_motion_completed &&
      station_318_motion_completed && station_319_motion_completed;
  const auto success_asserted =
      finale_callback_attempted && finale_callback_completed &&
      finale_state9_seen && finale_state9_returned && scripted_route_complete;
  std::cout << "legacy-level-summary: requested=" << frame_count
            << ", completed=" << completed_frames << ", outer=" << outer_updates
            << ", native=" << native_updates << ", opening=";
  if (opening_complete_frame) {
    std::cout << *opening_complete_frame;
  } else {
    std::cout << "none";
  }
  std::cout << ", dynamic-first=" << dynamic_first_slot
            << ", actor-lifetimes=" << lifetimes.size()
            << " (static=" << static_lifetimes
            << ", dynamic=" << dynamic_lifetimes << ")"
            << ", deaths=" << deaths << ", retired=" << retired
            << ", mission-transitions=" << mission_transitions
            << ", checkpoints=" << checkpoints
            << ", event-high-water=" << maximum_pending_events << '/'
            << maximum_ready_events << ", invalid-targets=" << invalid_targets
            << ", post-opening-player-moved=" << post_opening_player_moved
            << ", mission-progressed=" << mission_progressed
            << ", driver-stage=" << legacyLevelDriverStageName(driver_stage)
            << ", driver-stage-age="
            << (completed_frames > driver_stage_entry_trace_frame
                    ? completed_frames - driver_stage_entry_trace_frame
                    : 0U)
            << ", bank-kills=" << bank_reinforcement_kills
            << ", camera-owner-changes=" << camera_controller_changes
            << ", camera-discontinuities=" << camera_discontinuities
            << ", rail-mode-frames=" << scripted_camera_rail_frames
            << ", player-overrides=" << player_position_overrides
            << ", packed-ground-sentinels=" << packed_ground_sentinel_samples;
  if (last_mission) {
    std::cout << ", success=" << last_mission->success
              << ", terminal=" << last_mission->terminal
              << ", failure=" << last_mission->failure;
  }
  std::cout << '\n';

  std::cout << "legacy-level-assertions: failure-snapshot="
            << (failure_branch_checked && failure_branch_passed)
            << ", triggers=" << triggers_asserted << " [" << trigger_visited[0]
            << '/' << trigger_observed[0] << ',' << trigger_visited[1] << '/'
            << trigger_observed[1] << ',' << trigger_visited[2] << '/'
            << trigger_observed[2] << ',' << trigger_visited[3] << '/'
            << trigger_observed[3] << ']'
            << ", intro-state9=" << intro_state9_seen << '/'
            << intro_state9_returned
            << ", scripted-rail-mode=" << scripted_camera_rail_seen
            << ", scripted-transports=" << scripted_transports_asserted << " ["
            << elevator_315_motion_completed << ','
            << elevator_316_motion_completed << ','
            << station_318_motion_completed << ','
            << station_319_motion_completed << ']'
            << ", bomb29-callback=" << bomb_29_callback_attempted << '/'
            << bomb_29_callback_completed
            << ", objectives=" << objectives_asserted
            << ", checkpoint=" << (checkpoints != 0U)
            << ", finale-callback=" << finale_callback_attempted << '/'
            << finale_callback_completed << ", finale-state9="
            << finale_state9_seen << '/' << finale_state9_returned
            << ", success=" << success_asserted << '\n';

  std::string_view first_blocker{"none"};
  std::string synthesized_blocker;
  auto exit_code = 0;
  if (invalid_targets != 0U) {
    first_blocker = "invalid-guest-target-slot";
    exit_code = 32;
  } else if (!opening_complete_frame) {
    first_blocker = "opening-did-not-complete";
    exit_code = 33;
  } else if (!last_mission) {
    first_blocker = "mission-bridge-unavailable";
    exit_code = 31;
  } else if (driver_first_blocker) {
    first_blocker = *driver_first_blocker;
    exit_code = 36;
  } else if (!failure_branch_checked || !failure_branch_passed) {
    first_blocker = "protected-object-failure-assertion";
    exit_code = 36;
  } else if (last_mission->failure) {
    first_blocker = "mission-failure-before-success";
    exit_code = 34;
  } else if (!triggers_asserted) {
    first_blocker = "authored-trigger-activation-assertion";
    exit_code = 36;
  } else if (!intro_state9_seen || !intro_state9_returned) {
    first_blocker = "source-157-loader-boundary-assertion";
    exit_code = 36;
  } else if (!scripted_transports_asserted) {
    first_blocker = "guest-scripted-transport-assertion";
    exit_code = 36;
  } else if (!objectives_asserted) {
    first_blocker = "mission-objective-assertion";
    exit_code = 36;
  } else if (checkpoints == 0U) {
    first_blocker = "mission-checkpoint-assertion";
    exit_code = 36;
  } else if (!success_asserted) {
    synthesized_blocker = "scripted-route-stalled-at-";
    synthesized_blocker += legacyLevelDriverStageName(driver_stage);
    first_blocker = synthesized_blocker;
    exit_code = 36;
  }
  std::cout << "first-blocker=" << first_blocker << '\n';
  return exit_code;
}

int probeLegacyMission(const char *cue_path, const char *ram_path) {
  const auto disc = openDisc(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{
        sf::core::ErrorCode::unsupported,
        "Legacy mission probe requires Syphon Filter USA v1.1"};
  }

  const auto ram = readHostFile(ram_path);
  sf::game::LegacyGameplayVm vm{disc.executable()};
  vm.bindPsxBiosRandomCalls();
  vm.bindPsxVideoTimingCall();
  if (!vm.runtime().restoreRam(ram)) {
    throw sf::core::Error{
        sf::core::ErrorCode::invalid_format,
        "Legacy mission probe requires an exact 2 MiB raw RAM image"};
  }
  vm.runtime().reset(disc.executable().header().initial_pc, 0x80115c68U,
                     0x807fff70U);

  const auto tick =
      vm.tickMission(sf::game::syphonFilterUsaV11MissionProfile());
  std::cout << "frame-event: "
            << sf::psx::toString(tick.frame_event.execution.reason)
            << ", instructions=" << tick.frame_event.execution.instructions
            << ", pc=0x" << std::hex << std::uppercase
            << tick.frame_event.execution.pc << std::dec << '\n';
  if (!tick.frame_event.completed()) {
    return 6;
  }
  std::cout << "delayed-callbacks: "
            << sf::psx::toString(tick.delayed_callbacks.execution.reason)
            << ", instructions="
            << tick.delayed_callbacks.execution.instructions << ", pc=0x"
            << std::hex << std::uppercase << tick.delayed_callbacks.execution.pc
            << std::dec << '\n';
  if (!tick.delayed_callbacks.completed()) {
    return 7;
  }
  std::cout << "queue-drain: "
            << sf::psx::toString(tick.queue_drain.execution.reason)
            << ", instructions=" << tick.queue_drain.execution.instructions
            << ", pc=0x" << std::hex << std::uppercase
            << tick.queue_drain.execution.pc << std::dec << '\n';
  if (!tick.queue_drain.completed()) {
    return 8;
  }
  std::cout << "ready-events: " << tick.ready_events
            << ", dispatched=" << tick.dispatched_events.size()
            << ", instructions=" << tick.instructions() << '\n';
  for (std::size_t index = 0U; index < tick.dispatched_events.size(); ++index) {
    const auto &event = tick.dispatched_events[index];
    std::cout << "event[" << index
              << "]: " << sf::psx::toString(event.execution.reason)
              << ", instructions=" << event.execution.instructions << ", pc=0x"
              << std::hex << std::uppercase << event.execution.pc << std::dec
              << '\n';
  }
  if (tick.bridge_fault) {
    std::cout << "mission bridge fault\n";
  }
  return tick.completed() ? 0 : 9;
}

int probeLegacyFrame(const char *cue_path, const char *ram_path,
                     std::uint32_t frame_count) {
  const auto disc = openDisc(cue_path);
  if (!disc.game() || disc.game()->serial != "SCUS-94240" ||
      disc.game()->version != "1.1") {
    throw sf::core::Error{sf::core::ErrorCode::unsupported,
                          "Legacy frame probe requires Syphon Filter USA v1.1"};
  }

  const auto ram = readHostFile(ram_path);
  sf::game::LegacyGameplayVm vm{disc.executable()};
  vm.bindSyphonFilterUsaV11PlatformCalls();
  if (!vm.runtime().restoreRam(ram)) {
    throw sf::core::Error{
        sf::core::ErrorCode::invalid_format,
        "Legacy frame probe requires an exact 2 MiB raw RAM image"};
  }
  vm.runtime().reset(disc.executable().header().initial_pc, 0x80115c68U,
                     0x807fff70U);

  sf::game::LegacyGameplayVmResult frame;
  std::uint64_t total_instructions{};
  std::uint64_t total_host_calls{};
  for (std::uint32_t index = 0U; index < frame_count; ++index) {
    frame = vm.tickRetailFrame();
    total_instructions += frame.execution.instructions;
    total_host_calls += frame.host_calls;
    if (!frame.completed()) {
      std::cerr << "retail-frame[" << index << "] failed\n";
      break;
    }
  }
  std::cout << "retail-frame: " << sf::psx::toString(frame.execution.reason)
            << ", frames=" << frame_count
            << ", instructions=" << total_instructions
            << ", host-calls=" << total_host_calls << ", pc=0x" << std::hex
            << std::uppercase << frame.execution.pc << ", instruction=0x"
            << frame.execution.instruction << ", ra=0x"
            << vm.runtime().state().gpr[31] << ", sp=0x"
            << vm.runtime().state().gpr[29] << std::dec << ", ram-sha256="
            << sf::core::toHex(sf::core::sha256(vm.runtime().ram())) << '\n';
  return frame.completed() ? 0 : 10;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string_view{argv[1]} == "inspect") {
      return inspect(argv[2]);
    }
    if (argc == 3 && std::string_view{argv[1]} == "inspect-title") {
      return inspectTitle(argv[2]);
    }
    if ((argc == 3 || argc == 4) &&
        std::string_view{argv[1]} == "inspect-mission") {
      auto mission_index = std::uint32_t{};
      if (argc == 4) {
        const auto value = std::string_view{argv[3]};
        const auto parsed = std::from_chars(
            value.data(), value.data() + value.size(), mission_index);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() ||
            mission_index >= sf::game::missionCatalog().size()) {
          throw sf::core::Error{sf::core::ErrorCode::invalid_format,
                                "Mission index must be in the range 0..19"};
        }
      }
      return inspectMission(argv[2], mission_index);
    }
    if (argc == 4 && std::string_view{argv[1]} == "extract-exe") {
      return extractExecutable(argv[2], argv[3]);
    }
    if (argc == 3 && std::string_view{argv[1]} == "catalog") {
      return catalog(argv[2]);
    }
    if ((argc == 3 || argc == 4) && std::string_view{argv[1]} == "list-files") {
      return listDiscFiles(argv[2], argc == 4 ? argv[3] : "");
    }
    if (argc == 4 && std::string_view{argv[1]} == "map-functions") {
      return mapFunctions(argv[2], argv[3]);
    }
    if (argc == 3 && std::string_view{argv[1]} == "probe-legacy-vm") {
      return probeLegacyVm(argv[2]);
    }
    if (argc == 3 && std::string_view{argv[1]} == "probe-legacy-cd") {
      return probeLegacyCd(argv[2]);
    }
    if (argc == 3 && std::string_view{argv[1]} == "probe-legacy-loop") {
      return probeLegacyLoop(argv[2]);
    }
    if (argc == 3 && std::string_view{argv[1]} == "probe-legacy-bootstrap") {
      return probeLegacyBootstrap(argv[2]);
    }
    if ((argc == 3 || argc == 4) &&
        std::string_view{argv[1]} == "probe-legacy-level") {
      return probeLegacyLevel(argv[2],
                              argc == 4 ? parseFrameCount(argv[3]) : 1'200U);
    }
    if (argc == 4 && std::string_view{argv[1]} == "probe-legacy-mission") {
      return probeLegacyMission(argv[2], argv[3]);
    }
    if ((argc == 4 || argc == 5) &&
        std::string_view{argv[1]} == "probe-legacy-frame") {
      return probeLegacyFrame(argv[2], argv[3],
                              argc == 5 ? parseFrameCount(argv[4]) : 1U);
    }
    if (argc == 5 && std::string_view{argv[1]} == "extract-file") {
      return extractFile(argv[2], argv[3], argv[4]);
    }
    if (argc == 5 && std::string_view{argv[1]} == "extract-mission-file") {
      return extractMissionFile(argv[2], argv[3], argv[4]);
    }
    printUsage();
    return 64;
  } catch (const sf::core::Error &error) {
    std::cerr << "sf_tool: " << error.what() << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "sf_tool: unexpected error: " << error.what() << '\n';
    return 1;
  }
}
