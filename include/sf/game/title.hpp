#pragma once

#include "sf/assets/tim_image.hpp"
#include "sf/game/campaign_state.hpp"
#include "sf/game/disc_movie.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::game {

class GameDisc;

enum class TitleVisual : std::size_t {
    new_game,
    load_game,
    training_video,
    searching,
    count,
};

struct TitleSprite {
    std::string name;
    assets::TimImage image;
    std::int16_t x{};
    std::int16_t y{};
};

class TitleAssets final {
public:
    [[nodiscard]] static TitleAssets load(GameDisc& disc);

    [[nodiscard]] const TitleSprite& sprite(TitleVisual visual) const;
    [[nodiscard]] const std::vector<TitleSprite>& sprites() const noexcept { return sprites_; }

private:
    explicit TitleAssets(std::vector<TitleSprite> sprites);

    std::vector<TitleSprite> sprites_;
};

using TitleMovie = DiscMovie;

class TitleMovies final {
public:
    static constexpr std::size_t startup_movie_count = 3;
    static constexpr std::size_t movie_count = startup_movie_count + 2;

    [[nodiscard]] static TitleMovies load(GameDisc& disc);

    [[nodiscard]] std::vector<TitleMovie>& sequence() noexcept { return sequence_; }
    [[nodiscard]] const std::vector<TitleMovie>& sequence() const noexcept { return sequence_; }
    [[nodiscard]] std::span<TitleMovie> startupMovies() noexcept;
    [[nodiscard]] const TitleMovie& backgroundMovie() const;
    [[nodiscard]] const TitleMovie& trainingMovie() const;

private:
    explicit TitleMovies(std::vector<TitleMovie> sequence);

    std::vector<TitleMovie> sequence_;
};

enum class TitlePhase {
  searching,
  menu,
  select_difficulty,
  agent_warning,
  load_slots,
};

enum class CampaignDifficulty : std::uint8_t {
  original,
  hard_mode,
  agent,
};

[[nodiscard]] constexpr bool
validCampaignDifficulty(CampaignDifficulty difficulty) noexcept {
  return difficulty == CampaignDifficulty::original ||
         difficulty == CampaignDifficulty::hard_mode ||
         difficulty == CampaignDifficulty::agent;
}

[[nodiscard]] constexpr std::string_view
campaignDifficultyDisplayName(CampaignDifficulty difficulty) noexcept {
  switch (difficulty) {
  case CampaignDifficulty::original:
    return "Normal";
  case CampaignDifficulty::hard_mode:
    return "Hard Mode";
  case CampaignDifficulty::agent:
    return "Agent";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view
campaignDifficultyGameplayNotice(CampaignDifficulty difficulty) noexcept {
  switch (difficulty) {
  case CampaignDifficulty::original:
    return {};
  case CampaignDifficulty::hard_mode:
    return "Playing on HARD difficulty";
  case CampaignDifficulty::agent:
    return "Playing Agent mode";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view
campaignDifficultyGameplayPresentation(std::string_view retail_source,
                                       CampaignDifficulty difficulty) noexcept {
  const auto hard_notice =
      campaignDifficultyGameplayNotice(CampaignDifficulty::hard_mode);
  const auto is_hard_notice =
      retail_source == hard_notice ||
      (retail_source.size() >= 8U && hard_notice.starts_with(retail_source));
  return difficulty == CampaignDifficulty::agent && is_hard_notice
             ? campaignDifficultyGameplayNotice(CampaignDifficulty::agent)
             : retail_source;
}

struct TitleSaveSlot {
  bool occupied{};
  std::uint32_t mission_index{};
  bool campaign_complete{};
  // A completed mission is committed in two durable phases. While this is
  // set, mission_index still names the completed mission and its EOL must be
  // played before the cursor may advance (or become campaign_complete).
  std::optional<std::uint32_t> pending_eol_mission;
  std::optional<CampaignCarryState> carry;
  CampaignDifficulty difficulty{CampaignDifficulty::original};

  friend bool operator==(const TitleSaveSlot &,
                         const TitleSaveSlot &) = default;
};

inline constexpr std::size_t title_save_slot_count = 5U;
using TitleSaveSlots = std::array<TitleSaveSlot, title_save_slot_count>;

[[nodiscard]] std::string serializeTitleSaveSlots(const TitleSaveSlots& slots);
[[nodiscard]] std::optional<TitleSaveSlots> parseTitleSaveSlots(std::string_view bytes);

enum class TitleSaveLoadStatus {
    missing,
    loaded,
    recovered,
    invalid,
};

struct TitleSaveLoadResult {
    TitleSaveSlots slots{};
    TitleSaveLoadStatus status{TitleSaveLoadStatus::missing};
};

struct TitleSaveLocation {
    std::filesystem::path primary;
    std::filesystem::path legacy;
};

enum class TitleSaveMigrationStatus {
    not_needed,
    migrated,
    failed,
};

[[nodiscard]] TitleSaveLoadResult loadTitleSaveSlotsFile(
    const std::filesystem::path& path) noexcept;
[[nodiscard]] bool storeTitleSaveSlotsFile(
    const std::filesystem::path& path,
    const TitleSaveSlots& slots) noexcept;
[[nodiscard]] TitleSaveLocation titleSaveLocation(
    const std::filesystem::path& cue_path,
    std::string_view supported_game_serial,
    const std::filesystem::path& user_data_root);
[[nodiscard]] TitleSaveLocation defaultTitleSaveLocation(
    const std::filesystem::path& cue_path,
    std::string_view supported_game_serial) noexcept;
[[nodiscard]] TitleSaveMigrationStatus migrateLegacyTitleSaveSlotsFile(
    const TitleSaveLocation& location) noexcept;

enum class TitleCommand {
    none,
    new_game,
    load_game,
    training_video,
    exit,
};

struct TitleInput {
    bool previous{};
    bool next{};
    bool confirm{};
    bool cancel{};
    // Physical state is kept separate from the edge so the press that opens
    // the memory-card screen cannot also activate its first slot.
    bool confirm_down{};
};

class TitleMenu final {
public:
    static constexpr int screen_width = 320;
    static constexpr int screen_height = 240;
    static constexpr std::size_t item_count = 3;
    static constexpr std::size_t difficulty_count = 3;
    static constexpr std::size_t visual_count = static_cast<std::size_t>(TitleVisual::count);
    static constexpr std::uint32_t search_frames = 60;
    static constexpr std::uint32_t movie_fade_frame = 0x274;
    static constexpr std::uint8_t selected_brightness = 200;
    static constexpr std::uint8_t idle_brightness = 70;
    static constexpr std::uint8_t brightness_step = 10;

    [[nodiscard]] TitleCommand update(
        const TitleInput& input,
        std::uint32_t background_movie_frame = 0);
    [[nodiscard]] TitlePhase phase() const noexcept { return phase_; }
    [[nodiscard]] std::size_t selection() const noexcept { return selection_; }
    [[nodiscard]] bool itemEnabled(std::size_t item) const noexcept;
    [[nodiscard]] std::uint8_t brightness(TitleVisual visual) const noexcept;
    void completeSearch() noexcept;
    void setSaveSlots(TitleSaveSlots slots) noexcept { save_slots_ = slots; }
    [[nodiscard]] const TitleSaveSlots& saveSlots() const noexcept { return save_slots_; }
    [[nodiscard]] std::size_t loadSlotSelection() const noexcept { return load_slot_selection_; }
    [[nodiscard]] std::size_t difficultySelection() const noexcept {
        return difficulty_selection_;
    }
    [[nodiscard]] CampaignDifficulty selectedDifficulty() const noexcept {
        return static_cast<CampaignDifficulty>(difficulty_selection_);
    }
    [[nodiscard]] std::uint8_t
    difficultyBrightness(std::size_t index) const noexcept {
        return index < difficulty_brightness_.size()
            ? difficulty_brightness_[index]
            : 0U;
    }
    [[nodiscard]] std::uint32_t remainingSearchFrames() const noexcept {
        return remaining_search_frames_;
    }

private:
    void advanceSearch() noexcept;
    void requestSelection(std::ptrdiff_t requested) noexcept;
    void updateVisuals(std::uint32_t background_movie_frame) noexcept;

    TitlePhase phase_{TitlePhase::searching};
    std::size_t selection_{};
    std::size_t load_slot_selection_{};
    std::size_t difficulty_selection_{};
    bool load_slot_confirm_armed_{true};
    bool difficulty_confirm_armed_{true};
    bool agent_warning_confirm_armed_{true};
    std::uint32_t remaining_search_frames_{search_frames};
    std::array<std::uint8_t, visual_count> brightness_{};
    std::array<std::uint8_t, difficulty_count> difficulty_brightness_{};
    TitleSaveSlots save_slots_{};
};

[[nodiscard]] std::string_view titleCommandName(TitleCommand command) noexcept;

} // namespace sf::game
