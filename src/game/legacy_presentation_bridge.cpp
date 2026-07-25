#include "sf/game/legacy_presentation_bridge.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>

namespace sf::game {

std::uint8_t
legacyRetailDangerPercent(std::span<const LegacyUiThreatCommand> threats,
                          std::int16_t player_slot) noexcept {
  constexpr std::uint32_t fixed_one = 0x1000U;
  constexpr std::uint32_t alerted_floor = 0x0ab8U;
  constexpr std::uint32_t retail_bar_maximum = 50U;

  auto safe_q12 = fixed_one;
  for (const auto &threat : threats) {
    if (!threat.resident || threat.health <= 0 || !threat.has_target ||
        threat.target_slot != player_slot) {
      continue;
    }

    auto threat_q12 = std::min(threat.danger_q12 & 0xffff3fffU, fixed_one);
    if ((threat.ai_state & 0xffU) == 9U) {
      threat_q12 = std::max(threat_q12, alerted_floor);
    }
    safe_q12 = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(safe_q12) * (fixed_one - threat_q12)) >>
        12U);
  }

  const auto endpoint =
      retail_bar_maximum - ((safe_q12 * retail_bar_maximum) >> 12U);
  return static_cast<std::uint8_t>(endpoint * 2U);
}

namespace {

void appendCommand(LegacyPresentationFrame &frame,
                   LegacyPresentationCommandType type) {
  if (std::ranges::any_of(frame.commands, [type](const auto &command) {
        return command.type == type;
      })) {
    return;
  }
  frame.commands.push_back(LegacyPresentationCommand{type, frame.sequence});
}

bool validObjectSlot(std::int16_t slot, std::size_t object_count) noexcept {
  return slot >= 0 && static_cast<std::size_t>(slot) < object_count;
}

bool validWeaponEvent(const LegacyWeaponEventBridgeState &event,
                      std::int16_t player_slot,
                      std::size_t object_count) noexcept {
  if (event.weapon >= legacy_inventory_weapon_count ||
      event.actor_slot != player_slot ||
      !validObjectSlot(event.actor_slot, object_count) ||
      (event.aimed_target_slot != -1 &&
       !validObjectSlot(event.aimed_target_slot, object_count)) ||
      (event.enabled &&
       event.type != LegacyWeaponEventType::flashlight_toggle)) {
    return false;
  }
  switch (event.type) {
  case LegacyWeaponEventType::shot:
    return (event.weapon >= 1U && event.weapon <= 17U) || event.weapon == 22U;
  case LegacyWeaponEventType::thrown:
    return event.weapon == 19U || event.weapon == 20U;
  case LegacyWeaponEventType::scanner_begin:
  case LegacyWeaponEventType::scanner_end:
    return event.weapon == 18U;
  case LegacyWeaponEventType::flashlight_toggle:
    return event.weapon == 21U;
  case LegacyWeaponEventType::key_card_use:
    return event.weapon == 23U;
  case LegacyWeaponEventType::c4_use:
    return event.weapon == 24U;
  case LegacyWeaponEventType::antigen_use:
    return event.weapon == 25U;
  }
  return false;
}

bool validLineParticles(
    std::span<const LegacyLineParticleBridgeState> particles) noexcept {
  constexpr std::size_t retail_controller_capacity = 0x58U;
  if (particles.size() > legacy_effect_particle_capacity) {
    return false;
  }
  std::array<bool, legacy_effect_particle_capacity> seen{};
  for (const auto &particle : particles) {
    if (particle.particle >= seen.size() || seen[particle.particle] ||
        particle.controller >= retail_controller_capacity ||
        particle.first.y == std::numeric_limits<std::int32_t>::min() ||
        particle.second.y == std::numeric_limits<std::int32_t>::min() ||
        (particle.raw_packet_authoritative &&
         particle.kind != LegacyLineParticleKind::moving_trail)) {
      return false;
    }
    seen[particle.particle] = true;
    switch (particle.kind) {
    case LegacyLineParticleKind::rain_streak:
      if (particle.remaining_updates < 0 || !particle.semi_transparent ||
          particle.screen_half_width != 0U) {
        return false;
      }
      break;
    case LegacyLineParticleKind::rain_splash:
      if (particle.remaining_updates < 0 || !particle.semi_transparent ||
          particle.screen_half_width == 0U) {
        return false;
      }
      break;
    case LegacyLineParticleKind::ballistic_tracer:
      if (particle.remaining_updates <= 0 || particle.semi_transparent ||
          particle.screen_half_width != 0U) {
        return false;
      }
      break;
    case LegacyLineParticleKind::moving_trail:
      if (particle.remaining_updates == 0 || particle.screen_half_width != 0U) {
        return false;
      }
      break;
    default:
      return false;
    }
  }
  return true;
}

bool validCombatParticles(
    std::span<const LegacyCombatParticleBridgeState> particles,
    std::size_t object_count) noexcept {
  constexpr std::size_t retail_controller_capacity = 0x58U;
  if (particles.size() > legacy_effect_particle_capacity) {
    return false;
  }
  std::array<bool, legacy_effect_particle_capacity> seen{};
  for (const auto &particle : particles) {
    if (particle.particle >= seen.size() || seen[particle.particle] ||
        particle.controller >= retail_controller_capacity ||
        particle.remaining_updates <= 0 ||
        particle.position.y == std::numeric_limits<std::int32_t>::min() ||
        (particle.attached_slot != -1 &&
         !validObjectSlot(particle.attached_slot, object_count)) ||
        (particle.source_slot != -1 &&
         !validObjectSlot(particle.source_slot, object_count))) {
      return false;
    }
    seen[particle.particle] = true;
    switch (particle.kind) {
    case LegacyCombatParticleKind::ejected_shot_line:
      if (particle.semi_transparent) {
        return false;
      }
      break;
    case LegacyCombatParticleKind::blood_impact_triangle:
      if (particle.scale_byte == 0U) {
        return false;
      }
      break;
    default:
      return false;
    }
  }
  return true;
}

bool disjointEffectParticles(
    std::span<const LegacyLineParticleBridgeState> lines,
    std::span<const LegacyCombatParticleBridgeState> particles) noexcept {
  return std::ranges::none_of(lines, [&](const auto &line) {
    return std::ranges::any_of(particles, [&](const auto &particle) {
      return line.controller == particle.controller &&
             line.particle == particle.particle;
    });
  });
}

bool validPark2FlamethrowerRibbons(
    std::span<const LegacyPark2FlamethrowerRibbonBridgeState>
        ribbons) noexcept {
  constexpr std::size_t retail_ribbon_capacity = 72U;
  if (ribbons.size() > retail_ribbon_capacity) {
    return false;
  }
  std::array<bool, retail_ribbon_capacity> seen{};
  for (const auto &ribbon : ribbons) {
    if (ribbon.slot >= seen.size() || seen[ribbon.slot] ||
        ribbon.frame != static_cast<std::uint8_t>(2U + (ribbon.slot & 3U)) ||
        ribbon.ordering_depth == 0U || ribbon.ordering_depth >= 4096U ||
        (ribbon.width_shift != 1U && ribbon.width_shift != 2U) ||
        ribbon.world_first.y == std::numeric_limits<std::int32_t>::min() ||
        ribbon.world_second.y == std::numeric_limits<std::int32_t>::min() ||
        !std::ranges::all_of(ribbon.corners, [](const auto &corner) {
          return corner.x >= -1024 && corner.x <= 1023 && corner.y >= -1024 &&
                 corner.y <= 1023;
        })) {
      return false;
    }
    seen[ribbon.slot] = true;
  }
  return true;
}

bool validVertexLights(
    std::span<const LegacyVertexLightBridgeState> lights) noexcept {
  if (lights.size() > legacy_vertex_light_capacity) {
    return false;
  }
  for (auto index = std::size_t{}; index < lights.size(); ++index) {
    const auto &light = lights[index];
    if (light.source == 0U || (light.source & 3U) != 0U || light.shape < 0 ||
        light.threshold < 0 || light.screen_shift > 31U ||
        light.depth_shift > 31U || (light.channel_mask & 0xff000000U) != 0U ||
        std::ranges::any_of(lights.first(index), [&](const auto &earlier) {
          return earlier.source == light.source;
        })) {
      return false;
    }
  }
  return true;
}

bool validWorldVertexColors(
    std::span<const LegacyWorldSectionColorsBridgeState> sections,
    std::uint16_t world_model_count) noexcept {
  constexpr std::size_t maximum_sections_per_model = 31U;
  constexpr std::size_t maximum_vertices_per_section = 1024U;
  constexpr std::size_t maximum_total_colors = 131072U;
  auto total = std::size_t{};
  for (auto index = std::size_t{}; index < sections.size(); ++index) {
    const auto &section = sections[index];
    if (section.model >= world_model_count ||
        section.section >= maximum_sections_per_model ||
        section.colors.empty() ||
        section.colors.size() > maximum_vertices_per_section ||
        total > maximum_total_colors - section.colors.size() ||
        std::ranges::any_of(sections.first(index), [&](const auto &earlier) {
          return earlier.model == section.model &&
                 earlier.section == section.section;
        })) {
      return false;
    }
    total += section.colors.size();
  }
  return true;
}

bool validDroppedItems(std::span<const LegacyDroppedItemBridgeState> items,
                       std::uint16_t world_model_count) noexcept {
  constexpr std::size_t retail_capacity = 30U;
  if (items.size() > retail_capacity) {
    return false;
  }
  std::array<bool, retail_capacity> seen{};
  return std::ranges::all_of(items, [&](const auto &item) {
    const auto valid_selector =
        item.item < legacy_inventory_weapon_count || item.item == 0x80U;
    if (item.slot >= seen.size() || seen[item.slot] ||
        item.room >= world_model_count || !valid_selector ||
        item.transform.translation.y ==
            std::numeric_limits<std::int32_t>::min() ||
        std::ranges::none_of(item.transform.rotation,
                             [](std::int16_t value) { return value != 0; })) {
      return false;
    }
    seen[item.slot] = true;
    return true;
  });
}

bool validThrownProjectile(
    const std::optional<LegacyThrownProjectileBridgeState> &projectile) {
  return !projectile ||
         ((projectile->weapon == 19U || projectile->weapon == 20U) &&
          projectile->age <= 60U &&
          projectile->transform.translation.y !=
              std::numeric_limits<std::int32_t>::min() &&
          std::ranges::any_of(
              projectile->transform.rotation,
              [](std::int16_t component) { return component != 0; }));
}

bool validGrenadeTrajectory(
    const std::optional<LegacyGrenadeTrajectoryBridgeState> &trajectory) {
  if (!trajectory) {
    return true;
  }
  constexpr std::uint16_t minimum_strength = 0x28fU;
  constexpr std::uint16_t maximum_strength = 0xcccU;
  const auto valid_point = [](const LegacyNativePoint &point) {
    return point.x != std::numeric_limits<std::int32_t>::min() &&
           point.y != std::numeric_limits<std::int32_t>::min() &&
           point.z != std::numeric_limits<std::int32_t>::min();
  };
  return trajectory->strength_q12 >= minimum_strength &&
         trajectory->strength_q12 <= maximum_strength &&
         valid_point(trajectory->origin) && valid_point(trajectory->target) &&
         trajectory->origin != trajectory->target;
}

bool validGuestPackets(const LegacyGameplayBridgeState &renderer) noexcept {
  if (!renderer.guest_camera_lists_captured) {
    return !renderer.renderer_sprite_fast_path &&
           renderer.guest_sprites.empty() && renderer.guest_lines.empty() &&
           renderer.guest_raw_packets.empty();
  }
  if (renderer.guest_sprites.size() > 512U ||
      renderer.guest_lines.size() > 512U ||
      renderer.guest_raw_packets.size() > 1024U) {
    return false;
  }
  if (!std::ranges::all_of(renderer.guest_sprites, [](const auto &sprite) {
        constexpr std::uint32_t ram_begin = 0x80000000U;
        constexpr std::uint32_t ram_end = 0x80200000U;
        const auto valid_source = sprite.source_address == 0U ||
                                  ((sprite.source_address & 3U) == 0U &&
                                   sprite.source_address >= ram_begin &&
                                   sprite.source_address < ram_end);
        if (!valid_source || sprite.tpage >= 0x20U ||
            sprite.effect_particle < -1) {
          return false;
        }
        if (sprite.effect_particle < 0) {
          return sprite.effect_family == 0U && sprite.effect_frame == 0U &&
                 sprite.effect_position.x == 0 &&
                 sprite.effect_position.y == 0 && sprite.effect_position.z == 0;
        }
        if (sprite.effect_position.y ==
            std::numeric_limits<std::int32_t>::min()) {
          return false;
        }
        return legacyEffectSpriteFrameValid(sprite.effect_family,
                                            sprite.effect_frame);
      })) {
    return false;
  }
  return std::ranges::all_of(
      renderer.guest_raw_packets, [](const auto &packet) {
        constexpr std::uint32_t ram_begin = 0x80000000U;
        constexpr std::uint32_t ram_end = 0x80200000U;
        const auto valid_source = packet.source_address == 0U ||
                                  ((packet.source_address & 3U) == 0U &&
                                   packet.source_address >= ram_begin &&
                                   packet.source_address < ram_end);
        const auto valid_provenance =
            packet.effect_particle < -1 ? false
            : packet.effect_particle < 0
                ? !packet.effect_world_position_valid &&
                      packet.effect_position.x == 0 &&
                      packet.effect_position.y == 0 &&
                      packet.effect_position.z == 0
                : packet.effect_world_position_valid &&
                      packet.effect_position.y !=
                          std::numeric_limits<std::int32_t>::min();
        if (!valid_source || packet.word_count == 0U ||
            packet.word_count > legacy_guest_raw_packet_words ||
            !valid_provenance ||
            packet.opcode !=
                static_cast<std::uint8_t>(packet.words[0] >> 24U) ||
            packet.opcode == 0U || (packet.opcode & 0x80U) != 0U) {
          return false;
        }
        const auto base = static_cast<std::uint8_t>(packet.opcode & 0xfdU);
        return (packet.word_count == 2U && base == 0x68U) ||
               (packet.word_count == 3U && base == 0x40U) ||
               (packet.word_count == 4U && (base == 0x20U || base == 0x50U)) ||
               (packet.word_count == 5U && base == 0x28U) ||
               (packet.word_count == 6U && base == 0x30U);
      });
}

bool validScrim(const LegacyScrimBridgeState &scrim) noexcept {
  if (!scrim.resource_present) {
    return !scrim.visible && !scrim.transform_valid &&
           !scrim.vram_moves_active && scrim.vram_moves.empty();
  }
  if (scrim.visible != scrim.transform_valid) {
    return false;
  }
  if (scrim.vram_moves.size() != 0U && scrim.vram_moves.size() != 13U) {
    return false;
  }
  if (scrim.vram_moves_active && scrim.vram_moves.size() != 13U) {
    return false;
  }
  return std::ranges::all_of(scrim.vram_moves, [](const auto &move) {
    const auto page_contained = [](int x, int y, int width, int height) {
      return (x & 63) + width <= 64 && (y & 255) + height <= 256;
    };
    return move.source_x >= 0 && move.source_y >= 0 && move.width > 0 &&
           move.height > 0 && move.destination_x >= 0 &&
           move.destination_y >= 0 && move.source_x + move.width <= 1024 &&
           move.destination_x + move.width <= 1024 &&
           move.source_y + move.height <= 512 &&
           move.destination_y + move.height <= 512 &&
           page_contained(move.source_x, move.source_y, move.width,
                          move.height) &&
           page_contained(move.destination_x, move.destination_y, move.width,
                          move.height);
  });
}

bool validUiMessages(
    std::span<const LegacyUiMessageBridgeState> messages) noexcept {
  if (messages.size() > 64U) {
    return false;
  }
  return std::ranges::all_of(messages, [](const auto &message) {
    const auto valid_channel =
        message.channel == LegacyUiMessageChannel::centered ||
        message.channel == LegacyUiMessageChannel::status;
    const auto valid_backdrop =
        !message.backdrop || message.channel == LegacyUiMessageChannel::status;
    return valid_channel && valid_backdrop && message.text.size() <= 4096U &&
           message.glyphs.size() <= 120U;
  });
}

bool validUiTimer(
    const std::optional<LegacyUiTimerBridgeState> &timer) noexcept {
  return !timer || (timer->handle != 0xffffU && timer->glyphs.size() <= 8U);
}

template <typename T>
void appendBounded(std::vector<T> &pending, const std::vector<T> &incoming,
                   std::size_t maximum) {
  if (incoming.size() >= maximum) {
    pending.assign(incoming.end() - static_cast<std::ptrdiff_t>(maximum),
                   incoming.end());
    return;
  }
  const auto maximum_retained = maximum - incoming.size();
  if (pending.size() > maximum_retained) {
    pending.erase(pending.begin(),
                  pending.begin() + static_cast<std::ptrdiff_t>(
                                        pending.size() - maximum_retained));
  }
  pending.insert(pending.end(), incoming.begin(), incoming.end());
}

template <typename T, typename U>
void eraseEffectIdentity(std::vector<T> &particles, const U &incoming) {
  std::erase_if(particles, [&](const auto &particle) {
    return particle.controller == incoming.controller &&
           particle.particle == incoming.particle;
  });
}

template <typename T>
void upsertEffectParticles(std::vector<T> &pending,
                           const std::vector<T> &incoming,
                           std::size_t maximum) {
  for (const auto &particle : incoming) {
    const auto found =
        std::ranges::find_if(pending, [&](const auto &candidate) {
          return candidate.controller == particle.controller &&
                 candidate.particle == particle.particle;
        });
    if (found != pending.end()) {
      *found = particle;
      continue;
    }
    if (pending.size() == maximum) {
      pending.erase(pending.begin());
    }
    pending.push_back(particle);
  }
}

void upsertRawEffectPackets(
    std::vector<LegacyGuestRawPacketPresentationState> &pending,
    std::span<const LegacyGuestRawPacketPresentationState> incoming,
    std::size_t maximum) {
  for (const auto &entry : incoming) {
    const auto found =
        std::ranges::find_if(pending, [&](const auto &candidate) {
          if (entry.packet.source_address != 0U) {
            return candidate.packet.source_address ==
                   entry.packet.source_address;
          }
          return candidate.packet.source_address == 0U &&
                 candidate.packet.effect_particle ==
                     entry.packet.effect_particle;
        });
    if (found != pending.end()) {
      *found = entry;
      continue;
    }
    if (pending.size() == maximum) {
      pending.erase(pending.begin());
    }
    pending.push_back(entry);
  }
}

void upsertGuestEffectSprites(
    std::vector<LegacyGuestSpritePresentationState> &pending,
    std::span<const LegacyGuestSpritePresentationState> incoming,
    std::size_t maximum) {
  for (const auto &entry : incoming) {
    const auto found =
        std::ranges::find_if(pending, [&](const auto &candidate) {
          if (entry.sprite.source_address != 0U) {
            return candidate.sprite.source_address ==
                   entry.sprite.source_address;
          }
          return candidate.sprite.source_address == 0U &&
                 candidate.sprite.effect_particle ==
                     entry.sprite.effect_particle;
        });
    if (found != pending.end()) {
      *found = entry;
      continue;
    }
    if (pending.size() == maximum) {
      pending.erase(pending.begin());
    }
    pending.push_back(entry);
  }
}

} // namespace

bool mergeLegacyWorldVertexColorCache(
    std::span<LegacyWorldSectionColorsBridgeState> cache,
    std::span<const LegacyWorldSectionColorsBridgeState> updates) noexcept {
  const auto cached = [&cache](const auto &source) {
    return std::ranges::find_if(cache, [&source](const auto &candidate) {
      return candidate.model == source.model &&
             candidate.section == source.section;
    });
  };
  for (const auto &source : updates) {
    const auto destination = cached(source);
    if (destination == cache.end() ||
        destination->colors.size() != source.colors.size()) {
      return false;
    }
  }
  for (const auto &source : updates) {
    const auto destination = cached(source);
    std::ranges::copy(source.colors, destination->colors.begin());
  }
  return true;
}

bool mergeLegacyWorldVertexColorCache(
    std::span<LegacyWorldSectionColorsBridgeState> cache,
    std::span<const LegacyWorldSectionColorsBridgeState> updates,
    std::span<const std::uint16_t> required_models) noexcept {
  const auto cached = [&cache](const auto &source) {
    return std::ranges::find_if(cache, [&source](const auto &candidate) {
      return candidate.model == source.model &&
             candidate.section == source.section;
    });
  };
  const auto required = [&required_models](std::uint16_t model) {
    return std::ranges::find(required_models, model) != required_models.end();
  };

  // Validate the complete required transaction before changing the cache.
  // Optional descriptors are best-effort prefetch samples: a mismatched
  // topology identifies a stale streaming lifetime, not a corrupt frame.
  for (const auto &source : updates) {
    const auto destination = cached(source);
    if ((destination == cache.end() ||
         destination->colors.size() != source.colors.size()) &&
        required(source.model)) {
      return false;
    }
  }
  for (const auto &source : updates) {
    const auto destination = cached(source);
    if (destination != cache.end() &&
        destination->colors.size() == source.colors.size()) {
      std::ranges::copy(source.colors, destination->colors.begin());
    }
  }
  return true;
}

bool LegacyPresentationFrame::contains(
    LegacyPresentationCommandType type) const noexcept {
  return std::ranges::any_of(
      commands, [type](const auto &command) { return command.type == type; });
}

void LegacyWeaponEffectPresentationQueue::observe(
    const std::shared_ptr<const LegacyPresentationFrame> &frame) {
  if (!frame || !frame->renderer || frame->sequence == observed_sequence_) {
    return;
  }
  if (observed_sequence_ != 0U && frame->sequence < observed_sequence_) {
    pending_events_.clear();
    pending_lines_.clear();
    pending_particles_.clear();
    pending_sprites_.clear();
    pending_raw_packets_.clear();
    latest_events_.clear();
    latest_lines_.clear();
    latest_particles_.clear();
    latest_sprites_.clear();
    latest_raw_packets_.clear();
    pending_muzzle_flashes_.clear();
    latest_muzzle_flashes_.clear();
    effect_frame_consumed_ = true;
  }
  observed_sequence_ = frame->sequence;
  constexpr auto maximum_pending_events = legacy_weapon_events_per_frame * 3U;
  if (effect_frame_consumed_) {
    pending_events_.clear();
    pending_lines_.clear();
    pending_particles_.clear();
    pending_sprites_.clear();
    pending_raw_packets_.clear();
    pending_muzzle_flashes_.clear();
  }
  appendBounded(pending_events_, frame->renderer->state.weapon_events,
                maximum_pending_events);
  auto muzzle_flashes = std::vector<LegacyMuzzleFlashPresentationState>{};
  for (const auto &line : frame->renderer->state.line_particles) {
    if (line.kind != LegacyLineParticleKind::ballistic_tracer ||
        line.source_slot < 0) {
      continue;
    }
    const auto previous =
        std::ranges::find_if(latest_lines_, [&](const auto &candidate) {
          return candidate.controller == line.controller &&
                 candidate.particle == line.particle;
        });
    const auto freshly_started =
        previous == latest_lines_.end() ||
        line.remaining_updates > previous->remaining_updates;
    if (!freshly_started ||
        std::ranges::find(muzzle_flashes, line.source_slot,
                          &LegacyMuzzleFlashPresentationState::source_slot) !=
            muzzle_flashes.end()) {
      continue;
    }
    muzzle_flashes.push_back(LegacyMuzzleFlashPresentationState{
        line.source_slot, line.controller, line.particle, frame->sequence});
  }
  appendBounded(pending_muzzle_flashes_, muzzle_flashes,
                maximum_pending_events);
  for (const auto &line : frame->renderer->state.line_particles) {
    eraseEffectIdentity(pending_particles_, line);
  }
  for (const auto &particle : frame->renderer->state.combat_particles) {
    eraseEffectIdentity(pending_lines_, particle);
  }
  upsertEffectParticles(pending_lines_, frame->renderer->state.line_particles,
                        legacy_effect_particle_capacity);
  upsertEffectParticles(pending_particles_,
                        frame->renderer->state.combat_particles,
                        legacy_effect_particle_capacity);
  auto sprites = std::vector<LegacyGuestSpritePresentationState>{};
  sprites.reserve(frame->renderer->state.guest_sprites.size());
  for (const auto &sprite : frame->renderer->state.guest_sprites) {
    if (!legacyGuestSpriteUsesWorldDepth(sprite) ||
        sprite.effect_family != 0U) {
      continue;
    }
    sprites.push_back(LegacyGuestSpritePresentationState{
        frame->renderer->state.camera, sprite,
        frame->renderer->state.renderer_sprite_fast_path});
  }
  upsertGuestEffectSprites(pending_sprites_, sprites,
                           legacy_effect_particle_capacity);
  auto raw_packets = std::vector<LegacyGuestRawPacketPresentationState>{};
  raw_packets.reserve(frame->renderer->state.guest_raw_packets.size());
  for (const auto &packet : frame->renderer->state.guest_raw_packets) {
    if (!legacyGuestRawPacketUsesWorldDepth(packet) ||
        legacyGuestRawPacketHasWorldLine(
            packet, frame->renderer->state.line_particles) ||
        legacyGuestRawPacketHasWorldCombatParticle(
            packet, frame->renderer->state.combat_particles)) {
      continue;
    }
    raw_packets.push_back(LegacyGuestRawPacketPresentationState{
        frame->renderer->state.camera, packet});
  }
  upsertRawEffectPackets(pending_raw_packets_, raw_packets,
                         legacy_effect_particle_capacity);
  latest_events_ = frame->renderer->state.weapon_events;
  latest_lines_ = frame->renderer->state.line_particles;
  latest_particles_ = frame->renderer->state.combat_particles;
  latest_sprites_ = std::move(sprites);
  latest_raw_packets_ = std::move(raw_packets);
  latest_muzzle_flashes_ = std::move(muzzle_flashes);
  effect_frame_consumed_ = false;
}

void LegacyWeaponEffectPresentationQueue::consumeFrame() noexcept {
  pending_events_ = latest_events_;
  pending_lines_ = latest_lines_;
  pending_particles_ = latest_particles_;
  pending_sprites_ = latest_sprites_;
  pending_raw_packets_ = latest_raw_packets_;
  pending_muzzle_flashes_ = latest_muzzle_flashes_;
  effect_frame_consumed_ = true;
}

void LegacyWeaponEffectPresentationQueue::reset() noexcept {
  pending_events_.clear();
  pending_lines_.clear();
  pending_particles_.clear();
  pending_sprites_.clear();
  pending_raw_packets_.clear();
  pending_muzzle_flashes_.clear();
  latest_events_.clear();
  latest_lines_.clear();
  latest_particles_.clear();
  latest_sprites_.clear();
  latest_raw_packets_.clear();
  latest_muzzle_flashes_.clear();
  effect_frame_consumed_ = true;
  observed_sequence_ = 0U;
}

void LegacyScrimCopyPresentationQueue::observe(
    const std::shared_ptr<const LegacyPresentationFrame> &frame) {
  if (!frame || !frame->renderer || frame->sequence == observed_sequence_) {
    return;
  }
  if (observed_sequence_ != 0U && frame->sequence < observed_sequence_) {
    pending_phases_.clear();
  }
  observed_sequence_ = frame->sequence;
  const auto &scrim = frame->renderer->state.scrim;
  if (!scrim.resource_present || !scrim.vram_moves_active) {
    return;
  }
  pending_phases_.push_back(
      LegacyScrimCopyPresentationPhase{frame->guest_frame, scrim.vram_moves});
}

void LegacyScrimCopyPresentationQueue::consumeFrame() noexcept {
  pending_phases_.clear();
}

void LegacyScrimCopyPresentationQueue::reset() noexcept {
  observed_sequence_ = 0U;
  pending_phases_.clear();
}

bool legacyPresentationFrameConsumable(const LegacyPresentationFrame &frame,
                                       std::uint64_t after_sequence) noexcept {
  if (!frame.valid() || frame.sequence == 0U ||
      frame.sequence <= after_sequence ||
      frame.renderer->guest_frame != frame.guest_frame ||
      frame.ui->guest_frame != frame.guest_frame ||
      !frame.contains(LegacyPresentationCommandType::present_renderer) ||
      !frame.contains(LegacyPresentationCommandType::refresh_ui) ||
      frame.contains(LegacyPresentationCommandType::runtime_fault)) {
    return false;
  }
  return std::ranges::all_of(frame.commands, [&frame](const auto &command) {
    return command.sequence == frame.sequence;
  });
}

std::shared_ptr<const LegacyPresentationFrame> buildLegacyPresentationFrame(
    std::uint64_t sequence, std::uint64_t guest_frame,
    const LegacyGameplayBridgeState &renderer,
    const LegacyMissionBridgeState &ui,
    std::span<const LegacyPresentationCommandType> edge_commands) {
  if (sequence == 0U || renderer.dynamic_first_slot > renderer.objects.size() ||
      !legacyActiveWorldModels(renderer, renderer.world_model_count) ||
      !validObjectSlot(ui.player_slot, renderer.objects.size()) ||
      ui.objective_count > legacy_mission_entry_limit ||
      ui.parameter_count > legacy_mission_entry_limit ||
      ui.objective_texts.size() != ui.objective_count ||
      ui.parameter_texts.size() != ui.parameter_count ||
      !validUiMessages(ui.messages) || !validUiTimer(ui.timer) ||
      !validLineParticles(renderer.line_particles) ||
      !validCombatParticles(renderer.combat_particles,
                            renderer.objects.size()) ||
      !disjointEffectParticles(renderer.line_particles,
                               renderer.combat_particles) ||
      !validPark2FlamethrowerRibbons(renderer.park2_flamethrower_ribbons) ||
      !validScrim(renderer.scrim) ||
      !validVertexLights(renderer.vertex_lights) ||
      !validWorldVertexColors(renderer.world_vertex_colors,
                              renderer.world_model_count) ||
      !validDroppedItems(renderer.dropped_items, renderer.world_model_count) ||
      !validGrenadeTrajectory(renderer.grenade_trajectory) ||
      !validThrownProjectile(renderer.thrown_projectile) ||
      !validThrownProjectile(renderer.enemy_thrown_projectile) ||
      (renderer.flashlight_enabled && renderer.vertex_lights.empty()) ||
      !validGuestPackets(renderer) ||
      renderer.weapon_events.size() > legacy_weapon_events_per_frame ||
      !std::ranges::all_of(renderer.weapon_events, [&](const auto &event) {
        return validWeaponEvent(event, ui.player_slot, renderer.objects.size());
      })) {
    return {};
  }

  auto frame = std::make_shared<LegacyPresentationFrame>();
  frame->sequence = sequence;
  frame->guest_frame = guest_frame;
  frame->renderer.emplace(LegacyRendererCommandFrame{guest_frame, renderer});
  frame->ui.emplace();
  frame->ui->guest_frame = guest_frame;
  frame->ui->mission = ui;

  const auto &player =
      renderer.objects[static_cast<std::size_t>(ui.player_slot)];
  const auto target_valid =
      validObjectSlot(player.target_slot, renderer.objects.size());
  const auto aimed_target_valid =
      validObjectSlot(renderer.aimed_target_slot, renderer.objects.size());
  const auto proximity_target_valid =
      validObjectSlot(renderer.proximity_target_slot, renderer.objects.size());
  const auto headshot_callout = std::ranges::any_of(
      renderer.world_callouts, [&renderer](const auto &callout) {
        return callout.headshot &&
               callout.guest_slot == renderer.aimed_target_slot;
      });
  const auto headshot_flag =
      aimed_target_valid &&
      (renderer.objects[static_cast<std::size_t>(renderer.aimed_target_slot)]
           .danger_q12 &
       0x8000U) != 0U;
  frame->ui->target = LegacyUiTargetCommand{
      ui.player_slot,
      player.target_slot,
      aimed_target_valid ? renderer.aimed_target_slot : std::int16_t{-1},
      proximity_target_valid ? renderer.proximity_target_slot
                             : std::int16_t{-1},
      player.target_meter,
      target_valid ? player.target_flags : 0U,
      renderer.target_hit_result,
      renderer.target_lock_active && player.has_target && target_valid,
      renderer.target_hit_result != 0U && (headshot_callout || headshot_flag),
  };

  frame->ui->world_callouts.reserve(renderer.world_callouts.size());
  for (const auto &callout : renderer.world_callouts) {
    if (!validObjectSlot(callout.guest_slot, renderer.objects.size()) ||
        callout.text.empty()) {
      continue;
    }
    frame->ui->world_callouts.push_back(LegacyUiCalloutCommand{
        callout.guest_slot, callout.text, callout.headshot});
  }

  frame->ui->threats.reserve(renderer.tracked_slots.size());
  for (const auto slot : renderer.tracked_slots) {
    if (!validObjectSlot(slot, renderer.objects.size())) {
      continue;
    }
    const auto &object = renderer.objects[static_cast<std::size_t>(slot)];
    frame->ui->threats.push_back(LegacyUiThreatCommand{
        slot,
        object.target_slot,
        object.health,
        object.ai_state,
        object.danger_q12,
        object.resident,
        object.has_target,
    });
  }

  appendCommand(*frame, LegacyPresentationCommandType::present_renderer);
  appendCommand(*frame, LegacyPresentationCommandType::refresh_ui);
  for (const auto command : edge_commands) {
    if (command != LegacyPresentationCommandType::runtime_fault) {
      appendCommand(*frame, command);
    }
  }
  return frame;
}

std::shared_ptr<const LegacyPresentationFrame>
buildLegacyPresentationFaultFrame(std::uint64_t sequence,
                                  std::uint64_t guest_frame) {
  if (sequence == 0U) {
    return {};
  }
  auto frame = std::make_shared<LegacyPresentationFrame>();
  frame->sequence = sequence;
  frame->guest_frame = guest_frame;
  appendCommand(*frame, LegacyPresentationCommandType::runtime_fault);
  return frame;
}

} // namespace sf::game
