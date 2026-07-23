#include "sf/game/title.hpp"

#include "sf/assets/hog_archive.hpp"
#include "sf/core/error.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/mission.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace sf::game {
namespace {

std::uint8_t approachTitleBrightness(std::uint8_t current, std::uint8_t target) noexcept {
    if (target == 0) {
        return current <= TitleMenu::brightness_step ? 0 :
            static_cast<std::uint8_t>(current - TitleMenu::brightness_step);
    }
    if (target == TitleMenu::selected_brightness) {
        return current < TitleMenu::selected_brightness - TitleMenu::brightness_step ?
            static_cast<std::uint8_t>(current + TitleMenu::brightness_step) : target;
    }
    if (current < TitleMenu::idle_brightness - TitleMenu::brightness_step) {
        return static_cast<std::uint8_t>(current + TitleMenu::brightness_step);
    }
    if (current <= TitleMenu::idle_brightness + TitleMenu::brightness_step) {
        return target;
    }
    return static_cast<std::uint8_t>(current - TitleMenu::brightness_step);
}

constexpr std::uintmax_t maximum_title_save_bytes = 8192U;

enum class SaveFileReadStatus {
    missing,
    loaded,
    invalid,
};

struct SaveFileReadResult {
    std::optional<TitleSaveSlots> slots;
    SaveFileReadStatus status{SaveFileReadStatus::missing};
};

bool validSaveSlots(const TitleSaveSlots& slots) noexcept {
    const auto mission_count = missionCatalog().size();
    return std::ranges::all_of(slots, [mission_count](const auto& slot) {
        if (!slot.occupied) {
            return !slot.campaign_complete && !slot.pending_eol_mission &&
                !slot.carry;
        }
        if (slot.carry && !validCampaignCarry(*slot.carry)) {
            return false;
        }
        if (slot.pending_eol_mission) {
            return !slot.campaign_complete &&
                slot.mission_index < mission_count &&
                *slot.pending_eol_mission == slot.mission_index &&
                (!slot.carry ||
                 (slot.mission_index + 1U < mission_count &&
                  campaignMissionsShareCarry(slot.mission_index,
                                             slot.mission_index + 1U)));
        }
        if (slot.campaign_complete) {
            return mission_count != 0U &&
                slot.mission_index == mission_count - 1U && !slot.carry;
        }
        return slot.mission_index < mission_count &&
            (!slot.carry ||
             (slot.mission_index != 0U &&
              campaignMissionsShareCarry(slot.mission_index - 1U,
                                         slot.mission_index)));
    });
}

std::filesystem::path saveSiblingPath(
    const std::filesystem::path& path,
    std::string_view suffix) {
    auto sibling = path;
    sibling += suffix;
    return sibling;
}

std::filesystem::path platformUserDataRoot() {
#if defined(_WIN32)
    const auto environment_path = [](const char* name) {
        char* value{};
        std::size_t size{};
        const auto result = _dupenv_s(&value, &size, name);
        std::filesystem::path path;
        if (result == 0 && value != nullptr && *value != '\0') {
            path = value;
        }
        std::free(value);
        return path;
    };
    if (auto local_app_data = environment_path("LOCALAPPDATA");
        !local_app_data.empty()) {
        return local_app_data;
    }
    if (auto app_data = environment_path("APPDATA"); !app_data.empty()) {
        return app_data;
    }
#else
    if (const auto* xdg_data_home = std::getenv("XDG_DATA_HOME");
        xdg_data_home != nullptr && *xdg_data_home != '\0') {
        return std::filesystem::path{xdg_data_home};
    }
    if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} / ".local" / "share";
    }
#endif
    return std::filesystem::temp_directory_path();
}

std::string safeSaveKey(std::string_view serial) {
    std::string key;
    key.reserve(serial.size());
    for (const auto character : serial) {
        const auto valid =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' ||
            character == '_';
        key.push_back(valid ? character : '_');
    }
    return key.empty() ? "unsupported" : key;
}

SaveFileReadResult readSaveFile(const std::filesystem::path& path) {
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) {
        return {{}, SaveFileReadStatus::invalid};
    }
    if (!exists) {
        return {{}, SaveFileReadStatus::missing};
    }
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return {{}, SaveFileReadStatus::invalid};
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum_title_save_bytes) {
        return {{}, SaveFileReadStatus::invalid};
    }

    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return {{}, SaveFileReadStatus::invalid};
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return {{}, SaveFileReadStatus::invalid};
    }
    char trailing{};
    if (input.get(trailing) || input.bad()) {
        return {{}, SaveFileReadStatus::invalid};
    }
    auto slots = parseTitleSaveSlots(bytes);
    if (!slots || !validSaveSlots(*slots)) {
        return {{}, SaveFileReadStatus::invalid};
    }
    return {std::move(slots), SaveFileReadStatus::loaded};
}

} // namespace

TitleAssets::TitleAssets(std::vector<TitleSprite> sprites) : sprites_(std::move(sprites)) {}

TitleMovies::TitleMovies(std::vector<TitleMovie> sequence) : sequence_(std::move(sequence)) {}

TitleAssets TitleAssets::load(GameDisc& disc) {
    struct Definition {
        const char* name;
        std::int16_t x;
        std::int16_t y;
    };
    // Recovered from TITLE.OVL at 0x80148474. The archive order is not the
    // same as the on-screen menu order, so names are resolved explicitly.
    constexpr std::array definitions{
        Definition{"NEW.TIM", 42, 157},
        Definition{"LOAD.TIM", 131, 157},
        Definition{"VIDEO.TIM", 233, 157},
        Definition{"SEARCH.TIM", 133, 166},
    };

    auto archive = assets::HogArchive::parse(disc.image().readFile("COMMON/TITLE.HOG"));
    std::vector<TitleSprite> sprites;
    sprites.reserve(definitions.size());
    for (const auto& definition : definitions) {
        sprites.push_back(TitleSprite{
            definition.name,
            assets::TimImage::parse(archive.file(definition.name)),
            definition.x,
            definition.y,
        });
    }
    return TitleAssets{std::move(sprites)};
}

const TitleSprite& TitleAssets::sprite(TitleVisual visual) const {
    const auto index = static_cast<std::size_t>(visual);
    if (index >= sprites_.size()) {
        throw core::Error{core::ErrorCode::invalid_argument, "Invalid title visual"};
    }
    return sprites_[index];
}

TitleMovies TitleMovies::load(GameDisc& disc) {
    // Recovered from TITLE.OVL's logo sequence, Title_StartTitleMovie at
    // 0x80148f78 and Title_StartTrainingMovie at 0x80149300.
    constexpr std::array<const char*, movie_count> paths{
        "SOL/989LOGO.STR",
        "SOL/EIDETIC.STR",
        "SOL/INTRO.STR",
        "SOL/TITLE.STR",
        "SOL/TRAINING.STR",
    };

    std::vector<TitleMovie> sequence;
    sequence.reserve(paths.size());
    for (const auto* path : paths) {
        sequence.push_back(TitleMovie{path, disc.image().readRawSectorFile(path)});
    }
    return TitleMovies{std::move(sequence)};
}

std::span<TitleMovie> TitleMovies::startupMovies() noexcept {
    return std::span{sequence_}.first(startup_movie_count);
}

const TitleMovie& TitleMovies::backgroundMovie() const {
    if (sequence_.size() != movie_count) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid title movie sequence"};
    }
    return sequence_[startup_movie_count];
}

const TitleMovie& TitleMovies::trainingMovie() const {
    if (sequence_.size() != movie_count) {
        throw core::Error{core::ErrorCode::invalid_format, "Invalid title movie sequence"};
    }
    return sequence_[startup_movie_count + 1U];
}

TitleCommand TitleMenu::update(
    const TitleInput& input,
    std::uint32_t background_movie_frame) {
    advanceSearch();
    updateVisuals(background_movie_frame);

    if (phase_ == TitlePhase::load_slots) {
        constexpr auto row_count = title_save_slot_count + 1U;
        if (!input.confirm_down) {
            load_slot_confirm_armed_ = true;
        }
        if (input.cancel) {
            phase_ = TitlePhase::menu;
            load_slot_confirm_armed_ = true;
            return TitleCommand::none;
        }
        if (input.next) {
            load_slot_selection_ = (load_slot_selection_ + 1U) % row_count;
        }
        if (input.previous) {
            load_slot_selection_ = load_slot_selection_ == 0U
                ? row_count - 1U
                : load_slot_selection_ - 1U;
        }
        if (input.confirm && load_slot_confirm_armed_) {
            if (load_slot_selection_ == title_save_slot_count) {
                phase_ = TitlePhase::menu;
                load_slot_confirm_armed_ = true;
            } else if (save_slots_[load_slot_selection_].occupied &&
                       !save_slots_[load_slot_selection_].campaign_complete) {
                phase_ = TitlePhase::menu;
                load_slot_confirm_armed_ = true;
                return TitleCommand::load_game;
            }
        }
        return TitleCommand::none;
    }
    if (input.cancel) {
        return TitleCommand::exit;
    }

    constexpr std::array commands{
        TitleCommand::new_game,
        TitleCommand::load_game,
        TitleCommand::training_video,
    };
    if (input.confirm && itemEnabled(selection_)) {
        const auto command = commands[selection_];
        if (command == TitleCommand::load_game) {
            phase_ = TitlePhase::load_slots;
            load_slot_selection_ = 0U;
            load_slot_confirm_armed_ = false;
            return TitleCommand::none;
        }
        return command;
    }

    // TITLE.OVL handles the positive direction before the negative direction.
    if (input.next) {
        requestSelection(static_cast<std::ptrdiff_t>(selection_) + 1);
    }
    if (input.previous) {
        requestSelection(static_cast<std::ptrdiff_t>(selection_) - 1);
    }
    return TitleCommand::none;
}

bool TitleMenu::itemEnabled(std::size_t item) const noexcept {
    return phase_ != TitlePhase::load_slots && item < item_count &&
        !(item == 1U && phase_ == TitlePhase::searching);
}

std::uint8_t TitleMenu::brightness(TitleVisual visual) const noexcept {
    const auto index = static_cast<std::size_t>(visual);
    return index < brightness_.size() ? brightness_[index] : 0;
}

void TitleMenu::completeSearch() noexcept {
    remaining_search_frames_ = 0;
    if (phase_ == TitlePhase::searching) {
        phase_ = TitlePhase::menu;
    }
}

void TitleMenu::advanceSearch() noexcept {
    if (phase_ != TitlePhase::searching) {
        return;
    }
    if (remaining_search_frames_ > 0) {
        --remaining_search_frames_;
    }
    if (remaining_search_frames_ == 0) {
        completeSearch();
    }
}

void TitleMenu::requestSelection(std::ptrdiff_t requested) noexcept {
    if (requested == 1 && !itemEnabled(1U)) {
        // Native Title_SetSelection skips the unavailable memory-card item.
        requested = 2 - static_cast<std::ptrdiff_t>(selection_);
    }
    if (requested < 0 || requested >= static_cast<std::ptrdiff_t>(item_count)) {
        return;
    }
    const auto item = static_cast<std::size_t>(requested);
    if (itemEnabled(item)) {
        selection_ = item;
    }
}

void TitleMenu::updateVisuals(std::uint32_t background_movie_frame) noexcept {
    for (std::size_t index = 0; index < brightness_.size(); ++index) {
        const auto is_load = index == static_cast<std::size_t>(TitleVisual::load_game);
        const auto is_search = index == static_cast<std::size_t>(TitleVisual::searching);
        const auto hidden = background_movie_frame > movie_fade_frame ||
            (is_load && phase_ == TitlePhase::searching) ||
            (is_search && phase_ != TitlePhase::searching);
        const auto target = hidden ? 0U :
            index == selection_ ? selected_brightness : idle_brightness;
        brightness_[index] = approachTitleBrightness(
            brightness_[index], static_cast<std::uint8_t>(target));
    }
}

std::string serializeTitleSaveSlots(const TitleSaveSlots& slots) {
    std::ostringstream output;
    output << "SFPC_SAVE_V4\n";
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const auto& slot = slots[index];
        output << index << ' ' << (slot.occupied ? 1 : 0) << ' '
               << slot.mission_index << ' '
               << (slot.campaign_complete ? 1 : 0) << ' '
               << (slot.pending_eol_mission ? 1 : 0) << ' '
               << slot.pending_eol_mission.value_or(0U) << ' '
               << (slot.carry ? 1 : 0);
        if (slot.carry) {
            output << ' ' << static_cast<unsigned>(slot.carry->current_weapon)
                   << ' ' << slot.carry->owned_weapons << ' '
                   << slot.carry->health << ' ' << slot.carry->armor;
            for (const auto magazine : slot.carry->magazines) {
                output << ' ' << magazine;
            }
            for (const auto reserve : slot.carry->reserves) {
                output << ' ' << reserve;
            }
        }
        output << '\n';
    }
    return output.str();
}

std::optional<TitleSaveSlots> parseTitleSaveSlots(std::string_view bytes) {
    std::istringstream input{std::string{bytes}};
    std::string magic;
    if (!std::getline(input, magic) ||
        (magic != "SFPC_SAVE_V1" && magic != "SFPC_SAVE_V2" &&
         magic != "SFPC_SAVE_V3" && magic != "SFPC_SAVE_V4")) {
        return std::nullopt;
    }
    const auto version_two_or_newer = magic != "SFPC_SAVE_V1";
    const auto version_three_or_newer =
        magic == "SFPC_SAVE_V3" || magic == "SFPC_SAVE_V4";
    const auto version_four = magic == "SFPC_SAVE_V4";

    TitleSaveSlots slots{};
    for (std::size_t expected = 0; expected < slots.size(); ++expected) {
        std::size_t index{};
        unsigned int occupied{};
        std::uint32_t mission_index{};
        unsigned int campaign_complete{};
        unsigned int pending_eol{};
        std::uint32_t pending_eol_mission{};
        unsigned int has_carry{};
        if (!(input >> index >> occupied >> mission_index) ||
            (version_two_or_newer && !(input >> campaign_complete)) ||
            (version_three_or_newer &&
             !(input >> pending_eol >> pending_eol_mission)) ||
            (version_four && !(input >> has_carry)) ||
            index != expected || occupied > 1U || campaign_complete > 1U ||
            pending_eol > 1U || has_carry > 1U ||
            (!pending_eol && pending_eol_mission != 0U)) {
            return std::nullopt;
        }
        auto carry = std::optional<CampaignCarryState>{};
        if (has_carry != 0U) {
            unsigned int current_weapon{};
            CampaignCarryState state;
            if (!(input >> current_weapon >> state.owned_weapons >>
                  state.health >> state.armor) ||
                current_weapon > std::numeric_limits<std::uint8_t>::max()) {
                return std::nullopt;
            }
            state.current_weapon = static_cast<std::uint8_t>(current_weapon);
            for (auto& magazine : state.magazines) {
                if (!(input >> magazine)) {
                    return std::nullopt;
                }
            }
            for (auto& reserve : state.reserves) {
                if (!(input >> reserve)) {
                    return std::nullopt;
                }
            }
            if (!validCampaignCarry(state)) {
                return std::nullopt;
            }
            carry = std::move(state);
        }
        slots[index] = TitleSaveSlot{
            occupied != 0U, mission_index, campaign_complete != 0U,
            pending_eol != 0U
                ? std::optional<std::uint32_t>{pending_eol_mission}
                : std::nullopt,
            std::move(carry)};
    }
    std::string trailing;
    if (input >> trailing) {
        return std::nullopt;
    }
    return slots;
}

TitleSaveLoadResult loadTitleSaveSlotsFile(
    const std::filesystem::path& path) noexcept {
    try {
        const auto primary = readSaveFile(path);
        if (primary.status == SaveFileReadStatus::loaded) {
            return {*primary.slots, TitleSaveLoadStatus::loaded};
        }

        const auto backup = readSaveFile(saveSiblingPath(path, ".bak"));
        if (backup.status == SaveFileReadStatus::loaded) {
            return {*backup.slots, TitleSaveLoadStatus::recovered};
        }
        const auto status = primary.status == SaveFileReadStatus::missing &&
                backup.status == SaveFileReadStatus::missing
            ? TitleSaveLoadStatus::missing
            : TitleSaveLoadStatus::invalid;
        return {{}, status};
    } catch (...) {
        return {{}, TitleSaveLoadStatus::invalid};
    }
}

bool storeTitleSaveSlotsFile(
    const std::filesystem::path& path,
    const TitleSaveSlots& slots) noexcept {
    try {
        if (!validSaveSlots(slots)) {
            return false;
        }
        if (path.empty() || path.filename().empty()) {
            return false;
        }
        std::error_code error;
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error) {
                return false;
            }
        }
        const auto temporary = saveSiblingPath(path, ".tmp");
        const auto backup = saveSiblingPath(path, ".bak");
        static_cast<void>(std::filesystem::remove(temporary, error));
        if (error) {
            return false;
        }

        const auto bytes = serializeTitleSaveSlots(slots);
        {
            std::ofstream output{
                temporary, std::ios::binary | std::ios::trunc};
            if (!output) {
                return false;
            }
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            output.close();
            if (!output) {
                std::error_code ignored;
                static_cast<void>(std::filesystem::remove(temporary, ignored));
                return false;
            }
        }

        const auto primary = readSaveFile(path);
        const auto primary_exists = primary.status != SaveFileReadStatus::missing;
        auto primary_backed_up = false;
        if (primary_exists) {
            if (!std::filesystem::is_regular_file(path, error) || error) {
                std::error_code ignored;
                static_cast<void>(std::filesystem::remove(temporary, ignored));
                return false;
            }
            if (primary.status == SaveFileReadStatus::loaded) {
                static_cast<void>(std::filesystem::remove(backup, error));
                if (error) {
                    std::error_code ignored;
                    static_cast<void>(std::filesystem::remove(temporary, ignored));
                    return false;
                }
                std::filesystem::rename(path, backup, error);
                if (error) {
                    std::error_code ignored;
                    static_cast<void>(std::filesystem::remove(temporary, ignored));
                    return false;
                }
                primary_backed_up = true;
            } else {
                static_cast<void>(std::filesystem::remove(path, error));
                if (error) {
                    std::error_code ignored;
                    static_cast<void>(std::filesystem::remove(temporary, ignored));
                    return false;
                }
            }
        }

        std::filesystem::rename(temporary, path, error);
        if (error) {
            if (primary_backed_up) {
                std::error_code ignored;
                std::filesystem::rename(backup, path, ignored);
            }
            std::error_code ignored;
            static_cast<void>(std::filesystem::remove(temporary, ignored));
            return false;
        }

        // A first commit has no previous primary to rotate. A stale/corrupt
        // .bak is no better than a missing one, so validate it before deciding
        // that this commit already has a recoverable copy.
        if (readSaveFile(backup).status != SaveFileReadStatus::loaded) {
            const auto temporary_backup = saveSiblingPath(path, ".bak.tmp");
            error.clear();
            static_cast<void>(std::filesystem::remove(temporary_backup, error));
            if (!error) {
                std::filesystem::copy_file(
                    path, temporary_backup,
                    std::filesystem::copy_options::none, error);
            }
            if (!error) {
                static_cast<void>(std::filesystem::remove(backup, error));
            }
            if (!error) {
                std::filesystem::rename(temporary_backup, backup, error);
            }
            if (error) {
                std::error_code ignored;
                static_cast<void>(
                    std::filesystem::remove(temporary_backup, ignored));
                // The primary rename already committed a complete save file.
                // Report degraded durability so the host can offer Retry,
                // but never delete the only valid copy while repairing .bak.
                return false;
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

TitleSaveLocation titleSaveLocation(
    const std::filesystem::path& cue_path,
    std::string_view supported_game_serial,
    const std::filesystem::path& user_data_root) {
    const auto save_name = safeSaveKey(supported_game_serial) + ".sav";
    return TitleSaveLocation{
        user_data_root / "SyphonFilterPC" / "Saves" / save_name,
        cue_path.parent_path() / "SyphonFilterPC.sav",
    };
}

TitleSaveLocation defaultTitleSaveLocation(
    const std::filesystem::path& cue_path,
    std::string_view supported_game_serial) noexcept {
    try {
        return titleSaveLocation(
            cue_path, supported_game_serial, platformUserDataRoot());
    } catch (...) {
        try {
            return titleSaveLocation(
                cue_path, supported_game_serial,
                std::filesystem::temp_directory_path());
        } catch (...) {
            return {};
        }
    }
}

TitleSaveMigrationStatus migrateLegacyTitleSaveSlotsFile(
    const TitleSaveLocation& location) noexcept {
    try {
        if (location.primary.empty() || location.legacy.empty()) {
            return TitleSaveMigrationStatus::not_needed;
        }
        const auto current = loadTitleSaveSlotsFile(location.primary);
        if (current.status == TitleSaveLoadStatus::loaded ||
            current.status == TitleSaveLoadStatus::recovered) {
            return TitleSaveMigrationStatus::not_needed;
        }
        const auto legacy = loadTitleSaveSlotsFile(location.legacy);
        if (legacy.status == TitleSaveLoadStatus::missing) {
            return current.status == TitleSaveLoadStatus::missing
                ? TitleSaveMigrationStatus::not_needed
                : TitleSaveMigrationStatus::failed;
        }
        if (legacy.status == TitleSaveLoadStatus::invalid ||
            !storeTitleSaveSlotsFile(location.primary, legacy.slots)) {
            return TitleSaveMigrationStatus::failed;
        }
        return TitleSaveMigrationStatus::migrated;
    } catch (...) {
        return TitleSaveMigrationStatus::failed;
    }
}

std::string_view titleCommandName(TitleCommand command) noexcept {
    switch (command) {
    case TitleCommand::none:
        return "None";
    case TitleCommand::new_game:
        return "New Game";
    case TitleCommand::load_game:
        return "Load Game";
    case TitleCommand::training_video:
        return "Training Video";
    case TitleCommand::exit:
        return "Exit";
    }
    return "Unknown";
}

} // namespace sf::game
