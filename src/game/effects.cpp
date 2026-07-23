#include "sf/game/effects.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <ranges>

namespace sf::game {
namespace {

constexpr std::int32_t native_cfire_vertical_bias = 0x70;
constexpr std::uint64_t lightbar_ticks_per_frame = 2U;
constexpr std::int16_t lightbar_tile_width_words = 16;
constexpr std::int16_t lightbar_tile_height = 32;
constexpr std::int16_t blue_destination_x = 640;
constexpr std::int16_t blue_destination_y = 0;
constexpr std::int16_t red_destination_x = 640;
constexpr std::int16_t red_destination_y = 96;

constexpr EffectTextureCopy textureCopy(std::int16_t source_x,
                                        std::int16_t source_y,
                                        std::int16_t destination_x,
                                        std::int16_t destination_y) noexcept {
  return EffectTextureCopy{
      EffectVramRect{
          source_x,
          source_y,
          lightbar_tile_width_words,
          lightbar_tile_height,
      },
      destination_x,
      destination_y,
  };
}

constexpr std::array<PoliceLightbarFrame, 4> police_lightbar_frames{
    PoliceLightbarFrame{
        textureCopy(656, 0, blue_destination_x, blue_destination_y),
        textureCopy(672, 32, red_destination_x, red_destination_y),
    },
    PoliceLightbarFrame{
        textureCopy(672, 0, blue_destination_x, blue_destination_y),
        textureCopy(656, 96, red_destination_x, red_destination_y),
    },
    PoliceLightbarFrame{
        textureCopy(688, 0, blue_destination_x, blue_destination_y),
        textureCopy(640, 64, red_destination_x, red_destination_y),
    },
    PoliceLightbarFrame{
        textureCopy(640, 32, blue_destination_x, blue_destination_y),
        textureCopy(656, 64, red_destination_x, red_destination_y),
    },
};

constexpr std::uint16_t effectLifetime(GameplayEffectType type) noexcept {
  switch (type) {
  case GameplayEffectType::muzzle_flash:
    return 1U;
  case GameplayEffectType::blood_spray:
    return 3U;
  case GameplayEffectType::blood_decal:
    return 80U;
  case GameplayEffectType::explosion:
    return 8U;
  case GameplayEffectType::burning_fire:
    return 1U;
  }
  return 1U;
}

constexpr bool modelIdentityMatches(std::string_view model_name,
                                    std::string_view stem) noexcept {
  return model_name == stem || (model_name.size() == stem.size() + 4U &&
                                model_name.starts_with(stem) &&
                                model_name.substr(stem.size()) == ".TMD");
}

} // namespace

EffectPoint
cfireSpawnPoint(const assets::MissionTransform &transform) noexcept {
  return EffectPoint{
      transform.x,
      -transform.y - native_cfire_vertical_bias,
      transform.z,
  };
}

const PoliceLightbarFrame &
policeLightbarFrame(std::uint64_t gameplay_tick) noexcept {
  const auto phase =
      static_cast<std::size_t>((gameplay_tick / lightbar_ticks_per_frame) %
                               police_lightbar_frames.size());
  return police_lightbar_frames[phase];
}

bool legacyFireEmitterPresentation(std::uint32_t class_id,
                                   std::string_view model_name) noexcept {
  return (class_id == legacy_cfire_a_class &&
          modelIdentityMatches(model_name, "CFIREA")) ||
         (class_id == legacy_cfire_b_class &&
          modelIdentityMatches(model_name, "CFIREB")) ||
         (class_id == legacy_cfire_c_class &&
          modelIdentityMatches(model_name, "CFIREC"));
}

ObjectDamageResponse
objectDamageResponse(std::uint32_t class_id,
                     std::string_view model_name) noexcept {
  // BASE.OVL class 0x11 is GASPIPE in the subway overlays, while the silo
  // overlay binds the same class entry to HLITE. The resource identity is
  // therefore part of the retail dispatch key and prevents a light from
  // inheriting the gas-pipe no-death presentation path.
  if (class_id == 0x11U && modelIdentityMatches(model_name, "HLITE")) {
    return ObjectDamageResponse::extinguish;
  }
  switch (class_id) {
  case 0x14U: // HOTELGL/YLT
  case 0x20U: // BARWIN/BWIN/GLASS
  case 0x31U: // CGLAS/MOVGL
  case 0x37U: // MGLAS/GLASB
  case 0x4dU: // bar bottles
  case 0x51U: // MIRROR/OWIN/PLACE
  case 0x56U: // GLASRN
    return ObjectDamageResponse::shatter;
  case 0x13U: // PRLIT/AHALT/BARLITE/ARMYLT/LABLT/FLATLT/CAVLIT
  case 0x15U: // GLIT/YLIT
  case 0x16U: // SPOTLT
  case 0x33U: // BARLIT/LAMPY
  case 0x34U: // LIGHT/POOLT
  case 0x46U: // SUBLIT
  case 0x47U: // MET/RNLT
    return ObjectDamageResponse::extinguish;
  case 0x05U: // RADIO
  case 0x6fU: // GRGL
  case 0x3aU: // LOCK
    return ObjectDamageResponse::breakable;
  case 0x11U: // GASPIPE
  case 0x2eU: // BOMB
  case 0x58U: // BOMBSUB
    return ObjectDamageResponse::explosive;
  case 0x2cU: // CP
  case 0x38U: // CPTOP
    return ObjectDamageResponse::vehicle;
  default:
    return ObjectDamageResponse::none;
  }
}

GameplayEffect makeGameplayEffect(GameplayEffectType type, double x, double y,
                                  double z, double direction_x,
                                  double direction_y, double direction_z,
                                  double scale, std::uint32_t seed) noexcept {
  const auto lifetime = effectLifetime(type);
  return GameplayEffect{
      type,        x,     y,        z,        direction_x, direction_y,
      direction_z, scale, lifetime, lifetime, seed,
  };
}

void GameplayMuzzleFlashPresentationQueue::observe(
    std::span<const GameplayEffect> effects) {
  latest_flashes_.clear();
  std::ranges::copy_if(effects, std::back_inserter(latest_flashes_),
                       [](const auto &effect) {
                         return effect.type == GameplayEffectType::muzzle_flash;
                       });
  if (frame_consumed_) {
    pending_flashes_.clear();
  }
  constexpr auto maximum_pending_flashes = std::size_t{48U};
  for (const auto &flash : latest_flashes_) {
    const auto duplicate =
        std::ranges::find(pending_flashes_, flash.seed, &GameplayEffect::seed);
    if (duplicate != pending_flashes_.end()) {
      *duplicate = flash;
      continue;
    }
    if (pending_flashes_.size() == maximum_pending_flashes) {
      pending_flashes_.erase(pending_flashes_.begin());
    }
    pending_flashes_.push_back(flash);
  }
  frame_consumed_ = false;
}

void GameplayMuzzleFlashPresentationQueue::consumeFrame() noexcept {
  pending_flashes_ = latest_flashes_;
  frame_consumed_ = true;
}

void GameplayMuzzleFlashPresentationQueue::reset() noexcept {
  pending_flashes_.clear();
  latest_flashes_.clear();
  frame_consumed_ = true;
}

bool advanceGameplayEffect(GameplayEffect &effect) noexcept {
  if (effect.type == GameplayEffectType::burning_fire) {
    return true;
  }
  if (effect.remaining_updates == 0U) {
    return false;
  }
  --effect.remaining_updates;
  return effect.remaining_updates != 0U;
}

} // namespace sf::game
