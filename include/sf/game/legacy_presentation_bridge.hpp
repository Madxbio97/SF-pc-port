#pragma once

#include "sf/game/legacy_bridge_types.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace sf::game {

// Commands emitted at a completed guest frame boundary. The native side may
// consume them, but it must never acknowledge them by mutating gameplay RAM.
enum class LegacyPresentationCommandType : std::uint8_t {
  present_renderer,
  refresh_ui,
  checkpoint_commit,
  play_intro_fmv,
  play_ending_fmv,
  restart_after_failure,
  runtime_fault,
};

struct LegacyPresentationCommand {
  LegacyPresentationCommandType type{};
  std::uint64_t sequence{};
};

// Deep-copied renderer input. Keeping the typed guest snapshot inside this
// command frame lets existing presentation code migrate without exposing RAM.
struct LegacyRendererCommandFrame {
  std::uint64_t guest_frame{};
  LegacyGameplayBridgeState state;
};

struct LegacyUiTargetCommand {
  std::int16_t guest_slot{-1};
  std::int16_t target_slot{-1};
  std::int16_t aimed_target_slot{-1};
  std::int16_t proximity_target_slot{-1};
  std::int16_t target_meter{};
  std::uint32_t target_flags{};
  std::uint32_t hit_result{};
  bool active{};
  bool headshot{};
};

struct LegacyUiCalloutCommand {
  std::int16_t guest_slot{-1};
  std::string text;
  bool headshot{};
};

struct LegacyUiThreatCommand {
  std::int16_t guest_slot{-1};
  std::int16_t target_slot{-1};
  std::int16_t health{};
  std::uint16_t ai_state{};
  std::uint32_t danger_q12{};
  bool resident{};
  bool has_target{};
};

// Reproduces the retail HUD's Q12 danger aggregation over DAT_8011ba00.
// The bridge list is already the guest-owned tracked-target list; native scene
// residency/disposition must not filter it a second time.
[[nodiscard]] std::uint8_t
legacyRetailDangerPercent(std::span<const LegacyUiThreatCommand> threats,
                          std::int16_t player_slot) noexcept;

// UI input is intentionally smaller than the renderer snapshot. It contains
// only retail-owned values needed by HUD, inventory and mission panels.
struct LegacyUiCommandFrame {
  std::uint64_t guest_frame{};
  LegacyMissionBridgeState mission;
  LegacyUiTargetCommand target;
  std::vector<LegacyUiThreatCommand> threats;
  std::vector<LegacyUiCalloutCommand> world_callouts;
};

// One atomic guest-to-host handoff. Runtime owners expose this through a
// shared_ptr<const ...>, so renderer and UI always observe the same guest tick.
struct LegacyPresentationFrame {
  std::uint64_t sequence{};
  std::uint64_t guest_frame{};
  std::optional<LegacyRendererCommandFrame> renderer;
  std::optional<LegacyUiCommandFrame> ui;
  std::vector<LegacyPresentationCommand> commands;

  [[nodiscard]] bool valid() const noexcept {
    return renderer.has_value() && ui.has_value();
  }
  [[nodiscard]] bool
  contains(LegacyPresentationCommandType type) const noexcept;
};

// An effect-pool sprite/raw packet together with the exact retail camera which
// projected it. Short-lived glass shards may be born and expire while several
// 20 Hz guest ticks are caught up before a single host presentation.
struct LegacyGuestSpritePresentationState {
  LegacyCameraBridgeState camera;
  LegacyGuestSpriteBridgeState sprite;
  bool renderer_fast_path{};
};

struct LegacyGuestRawPacketPresentationState {
  LegacyCameraBridgeState camera;
  LegacyGuestRawPacketBridgeState packet;
};

// A newly allocated/restarted retail ballistic line is the authoritative
// per-shot signal for NPC firearms; the weapon-event hook is player-only.
struct LegacyMuzzleFlashPresentationState {
  std::int16_t source_slot{-1};
  std::uint16_t controller{};
  std::uint16_t particle{};
  std::uint64_t sequence{};
};

// The retail pickup allocator publishes owner, descriptor and MATRIX through
// separate stores. At a room transition the host can sample the single 20 Hz
// tick between those stores and briefly lose an otherwise live pickup. Keep
// only the last validated value for one missing guest tick while the retail
// owner still names a floor room. Collected, attached and vacant slots clear
// immediately, including a second capture of the same guest tick.
class LegacyDroppedItemPresentationCache final {
public:
  static constexpr std::size_t capacity = 30U;

  void reconcile(std::uint64_t guest_frame,
                 std::uint32_t floor_owner_mask,
                 std::vector<LegacyDroppedItemBridgeState> &items);
  void reset() noexcept;

private:
  struct Entry {
    LegacyDroppedItemBridgeState item;
    std::uint64_t last_valid_guest_frame{};
    bool valid{};
  };

  std::array<Entry, capacity> entries_{};
  std::vector<LegacyDroppedItemBridgeState> stable_items_;
  std::uint64_t observed_guest_frame_{};
  bool observed_{};
};

// The guest can advance several retail ticks before one host presentation.
// Preserve one-tick weapon events and effect-pool packets until both OTs have
// consumed them; otherwise catch-up erases muzzle flashes, tracers and the
// short-lived GsSPRITEs/raw triangles used by retail glass destruction.
class LegacyWeaponEffectPresentationQueue final {
public:
  void observe(const std::shared_ptr<const LegacyPresentationFrame> &frame);

  [[nodiscard]] std::span<const LegacyWeaponEventBridgeState>
  events() const noexcept {
    return pending_events_;
  }

  [[nodiscard]] std::span<const LegacyLineParticleBridgeState>
  lines() const noexcept {
    return pending_lines_;
  }

  [[nodiscard]] std::span<const LegacyCombatParticleBridgeState>
  particles() const noexcept {
    return pending_particles_;
  }

  [[nodiscard]] std::span<const LegacyGuestSpritePresentationState>
  sprites() const noexcept {
    return pending_sprites_;
  }

  [[nodiscard]] std::span<const LegacyGuestRawPacketPresentationState>
  rawPackets() const noexcept {
    return pending_raw_packets_;
  }

  [[nodiscard]] std::span<const LegacyMuzzleFlashPresentationState>
  muzzleFlashes() const noexcept {
    return pending_muzzle_flashes_;
  }

  void consumeFrame() noexcept;
  void reset() noexcept;

private:
  std::uint64_t observed_sequence_{};
  std::vector<LegacyWeaponEventBridgeState> pending_events_;
  std::vector<LegacyLineParticleBridgeState> pending_lines_;
  std::vector<LegacyCombatParticleBridgeState> pending_particles_;
  std::vector<LegacyGuestSpritePresentationState> pending_sprites_;
  std::vector<LegacyGuestRawPacketPresentationState> pending_raw_packets_;
  std::vector<LegacyMuzzleFlashPresentationState> pending_muzzle_flashes_;
  std::vector<LegacyWeaponEventBridgeState> latest_events_;
  std::vector<LegacyLineParticleBridgeState> latest_lines_;
  std::vector<LegacyCombatParticleBridgeState> latest_particles_;
  std::vector<LegacyGuestSpritePresentationState> latest_sprites_;
  std::vector<LegacyGuestRawPacketPresentationState> latest_raw_packets_;
  std::vector<LegacyMuzzleFlashPresentationState> latest_muzzle_flashes_;
  bool effect_frame_consumed_{true};
};

struct LegacyScrimCopyPresentationPhase {
  std::uint64_t guest_frame{};
  std::vector<LegacyVramMoveBridgeState> moves;
};

enum class LegacyScrimCopyPhasePosition : std::uint8_t {
  preceding_display,
  displayed_frame,
  future_frame,
};

[[nodiscard]] constexpr LegacyScrimCopyPhasePosition
legacyScrimCopyPhasePosition(std::uint64_t phase_guest_frame,
                             std::uint64_t displayed_guest_frame) noexcept {
  if (phase_guest_frame < displayed_guest_frame) {
    return LegacyScrimCopyPhasePosition::preceding_display;
  }
  return phase_guest_frame == displayed_guest_frame
             ? LegacyScrimCopyPhasePosition::displayed_frame
             : LegacyScrimCopyPhasePosition::future_frame;
}

// SCRIM submits one positive copy phase every other retail tick. Several
// guest ticks may complete before one host draw, so retain every fresh phase
// instead of sampling only the final signed controller state.
class LegacyScrimCopyPresentationQueue final {
public:
  void observe(const std::shared_ptr<const LegacyPresentationFrame> &frame);

  [[nodiscard]] std::span<const LegacyScrimCopyPresentationPhase>
  phases() const noexcept {
    return pending_phases_;
  }

  void consumeFrame() noexcept;
  void reset() noexcept;

private:
  std::uint64_t observed_sequence_{};
  std::vector<LegacyScrimCopyPresentationPhase> pending_phases_;
};

// Merges the sections published by the current guest frame into a complete
// mission-lifetime cache. Missing sections are deliberately retained: the
// retail 4:3 streamer may drop them while the native widescreen envelope can
// still see them. The operation validates every update before mutating any
// cached color.
[[nodiscard]] bool mergeLegacyWorldVertexColorCache(
    std::span<LegacyWorldSectionColorsBridgeState> cache,
    std::span<const LegacyWorldSectionColorsBridgeState> updates) noexcept;

// Streamed-out descriptors may retain a structurally valid payload from an
// earlier room lifetime. Ignore topology-mismatched optional updates while
// keeping the validated resident/visible set fail-closed and atomic.
[[nodiscard]] bool mergeLegacyWorldVertexColorCache(
    std::span<LegacyWorldSectionColorsBridgeState> cache,
    std::span<const LegacyWorldSectionColorsBridgeState> updates,
    std::span<const std::uint16_t> required_models) noexcept;

// Production consumers accept each complete renderer/UI frame exactly once.
// Replayed, split-tick or fault frames are rejected before native projections
// can be updated.
[[nodiscard]] bool
legacyPresentationFrameConsumable(const LegacyPresentationFrame &frame,
                                  std::uint64_t after_sequence) noexcept;

// Returns null when the two source snapshots cannot form one coherent frame.
// All vectors are deep-copied before the const frame is published.
[[nodiscard]] std::shared_ptr<const LegacyPresentationFrame>
buildLegacyPresentationFrame(
    std::uint64_t sequence, std::uint64_t guest_frame,
    const LegacyGameplayBridgeState &renderer,
    const LegacyMissionBridgeState &ui,
    std::span<const LegacyPresentationCommandType> edge_commands = {});

[[nodiscard]] std::shared_ptr<const LegacyPresentationFrame>
buildLegacyPresentationFaultFrame(std::uint64_t sequence,
                                  std::uint64_t guest_frame);

} // namespace sf::game
