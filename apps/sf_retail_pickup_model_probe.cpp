#include "sf/assets/gmd_model.hpp"
#include "sf/assets/tim_image.hpp"
#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/gameplay.hpp"
#include "sf/game/internal/g4_campaign_transition_probe_access.hpp"
#include "sf/game/legacy_presentation_bridge.hpp"
#include "sf/game/mission.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Access = sf::game::G4CampaignTransitionProbeAccess;
void require(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error{message};
}
std::string gmdName(std::string name) {
  name.replace(name.find_last_of('.'), std::string::npos, ".GMD");
  return name;
}
bool covered(const sf::assets::TimImage &tim,
             const sf::assets::GmdTriangle &tri, const sf::assets::EmdUv &uv) {
  const auto page = tri.texture_page & 31U;
  const auto x = static_cast<unsigned>((page & 15U) * 128U + uv.u);
  const auto y = static_cast<unsigned>((page > 15U ? 256U : 0U) + uv.v);
  const auto &p = tim.pixels();
  return tim.mode() == sf::assets::TimPixelMode::indexed8 && x >= p.x * 2U &&
         x < (p.x + p.width_words) * 2U && y >= p.y &&
         y < static_cast<unsigned>(p.y) + p.height;
}
struct Crate {
  std::uint16_t scene;
  std::uint32_t slot;
  std::uint32_t instance;
  std::uint8_t flags;
  std::uint8_t state;
  std::string primary, secondary;
};
std::size_t checkMission(sf::game::GameDisc &disc, std::uint32_t mission) {
  auto package = sf::game::MissionPackage::load(disc, mission);
  auto game = std::make_unique<sf::game::GameplaySession>(package);
  for (unsigned i{}; i < 8U; ++i) {
    game->update({});
    require(game->advanceAudioFrameClock(), "bootstrap audio");
  }
  for (unsigned i{}; i < 64U && !Access::captureCheckpoint(*game); ++i) {
    game->update({});
    require(game->advanceAudioFrameClock(), "checkpoint wait");
  }
  require(Access::captureCheckpoint(*game), "checkpoint unavailable");
  const auto frame = game->legacyPresentationFrame();
  require(frame && frame->renderer, "missing immutable frame");
  std::vector<sf::assets::TimImage> images;
  for (const auto name : sf::game::legacy_crate_texture_names) {
    images.push_back(
        sf::assets::TimImage::parse(package.interfaceAssets().file(name)));
    require(images.back().clut() &&
                images.back().mode() == sf::assets::TimPixelMode::indexed8 &&
                images.back().clut()->words == images.front().clut()->words,
            "incompatible crate texture palette");
    require(std::ranges::any_of(images.back().pixels().words,
                                [&](std::uint16_t word) {
                                  const auto &palette =
                                      images.back().clut()->words;
                                  return palette[word & 0xffU] != 0U ||
                                         palette[word >> 8U] != 0U;
                                }),
            "fully transparent crate texture " + std::string{name});
  }
  std::vector<Crate> crates;
  std::size_t checked_models{}, checked_corners{}, checked_lockers{};
  for (std::size_t source{}; source < package.objects().objects().size();
       ++source) {
    const auto &record = package.objects().objects()[source];
    const auto &definition = package.objects().definition(record.type);
    const auto inventory_crate =
        definition.class_id == 0x4fU || definition.class_id == 0x50U;
    if (definition.primary_model.empty()) {
      // BUNKER class-0x50 sources 313..320 are model-less in the original BIN.
      require(!inventory_crate ||
                  (mission == 8U && source >= 313U && source <= 320U &&
                   definition.class_id == 0x50U &&
                   definition.secondary_model.empty()),
              "unexpected model-less inventory definition");
      continue;
    }
    const auto primary = gmdName(definition.primary_model);
    const auto shared_crate =
        sf::game::legacyWeaponCratePresentation(definition.class_id, primary);
    const auto locker = definition.class_id == 0x50U &&
                        (primary == "LCKRA.GMD" || primary == "LCKRC.GMD" ||
                         primary == "GLCKRA.GMD");
    require(!inventory_crate || shared_crate || locker,
            "uncovered inventory model family " + primary);
    if (!shared_crate && !locker)
      continue;
    require(!definition.secondary_model.empty(),
            "missing authored open crate model");
    const auto secondary = gmdName(definition.secondary_model);
    auto scene = game->objects().size();
    for (std::size_t i{}; i < game->objects().size(); ++i)
      if (game->objects()[i].source_index == source) {
        scene = i;
        break;
      }
    require(scene < game->objects().size(),
            "missing BIN crate source " + std::to_string(source));
    const auto &object = game->objects()[scene];
    require(object.destroyed_model.has_value(), "missing open crate model");
    require(game->objectModels()[object.model].name == primary &&
                game->objectModels()[*object.destroyed_model].name == secondary,
            "container model differs from authored BIN");
    for (const auto index : {object.model, *object.destroyed_model}) {
      const auto &model = game->objectModels()[index];
      const auto *gmd = std::get_if<sf::assets::GmdModel>(&model.geometry);
      require(gmd && !gmd->triangles().empty(),
              "missing container geometry " + model.name);
      if (locker)
        continue;
      require(gmd->renderableTexturePageMask() == 0x3000U,
              "invalid crate texture pages " + model.name);
      require(
          sf::game::legacyWeaponCratePresentation(object.class_id, model.name),
          "unregistered crate overlay " + model.name);
      for (const auto &triangle : gmd->triangles()) {
        if (triangle.flags == 0U || triangle.degenerate)
          continue;
        require(triangle.clut == 0x78f0U,
                "unexpected crate CLUT " + model.name);
        for (const auto &uv : triangle.uv) {
          require(std::ranges::any_of(images,
                                      [&](const auto &im) {
                                        return covered(im, triangle, uv);
                                      }),
                  "uncovered crate texel " + model.name +
                      " page=" + std::to_string(triangle.texture_page & 31U) +
                      " uv=" + std::to_string(uv.u) + "," +
                      std::to_string(uv.v));
          ++checked_corners;
        }
      }
      ++checked_models;
    }
    if (locker) {
      ++checked_lockers;
      continue;
    }
    const auto slot = game->legacyGuestSlotsBySceneObject()[scene];
    if (slot < 0 ||
        static_cast<std::size_t>(slot) >= frame->renderer->state.objects.size())
      continue;
    const auto &guest = frame->renderer->state.objects[slot];
    if (!guest.resident ||
        (guest.instance_state[3] & sf::game::legacy_instance_dormant) != 0U)
      continue;
    crates.push_back({static_cast<std::uint16_t>(scene),
                      static_cast<std::uint32_t>(slot), guest.instance,
                      guest.instance_flags, guest.instance_state[3], primary,
                      secondary});
  }
  auto verify = [&](bool consumed, bool hidden) {
    for (const auto &crate : crates) {
      const auto *model = game->displayedObjectModel(crate.scene);
      require(hidden
                  ? model == nullptr
                  : model && model->name == ((consumed || (crate.flags & 0x20U))
                                                 ? crate.secondary
                                                 : crate.primary),
              "wrong crate presentation source " + std::to_string(crate.slot));
      require(hidden || std::ranges::find(game->activeObjects(), crate.scene) !=
                            game->activeObjects().end(),
              "resident crate left active set");
    }
  };
  verify(false, false);
  auto *vm = Access::occlusionProbeVm(*game);
  require(vm != nullptr, "missing VM");
  for (const auto &crate : crates)
    require(vm->runtime().write8(crate.instance, crate.flags | 0x20U),
            "consumed latch write");
  require(Access::refreshPropPresentation(*game), "consumed frame publish");
  verify(true, false);
  for (const auto &crate : crates)
    require(
        vm->runtime().write8(crate.instance + 0x23U,
                             crate.state | sf::game::legacy_instance_dormant),
        "hidden latch write");
  require(Access::refreshPropPresentation(*game), "hidden frame publish");
  verify(true, true);
  require(game->restartCheckpoint(), "crate checkpoint restore");
  verify(false, false);
  std::cout << "mission=" << mission << " crates=" << crates.size()
            << " model-checks=" << checked_models
            << " texel-corners=" << checked_corners
            << " authored-lockers=" << checked_lockers
            << " closed/open/hide/restore=passed\n";
  return crates.size();
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  try {
    auto disc = sf::game::GameDisc::open(argv[1]);
    std::size_t crates{};
    for (const auto &mission : sf::game::missionCatalog())
      crates += checkMission(disc, mission.index);
    require(crates != 0U, "empty pickup coverage");
    std::cout << "Pickup model gate passed: missions=20 crates=" << crates
              << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Pickup model gate failed: " << e.what() << "\n";
    return 2;
  }
}
