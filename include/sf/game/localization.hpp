#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::game {

enum class GameLanguage : std::uint8_t {
  english,
  russian_vit,
};

void setGameLanguage(GameLanguage language) noexcept;
[[nodiscard]] GameLanguage gameLanguage() noexcept;
[[nodiscard]] bool russianLanguageActive() noexcept;

void setLocalizationRoot(std::filesystem::path root);
[[nodiscard]] const std::filesystem::path &localizationRoot() noexcept;
[[nodiscard]] bool localizationPackAvailable(GameLanguage language) noexcept;

[[nodiscard]] std::optional<std::vector<std::byte>>
readLocalizedAsset(std::string_view relative_path) noexcept;

// Returns strings in the original ViT single-byte font encoding. The
// lifetime of a translated value is static; an unknown key is returned as-is.
[[nodiscard]] std::string_view localizeText(std::string_view english) noexcept;

// Localizes compound UI text (multi-line labels, prefixed list entries and
// dynamic numeric values) and owns the returned storage.
[[nodiscard]] std::string localizeTextCopy(std::string_view english);

// Encodes authored UTF-8 Cyrillic into the original ViT single-byte glyph
// map. Native UI and exact localization tests share this boundary so expected
// strings cannot silently drift into mojibake.
[[nodiscard]] std::string encodeVitText(std::u8string_view source);

// Retail can expose a gameplay status while its type-on animation has only
// submitted a prefix of the source glyphs. Resolve known HUD prefixes back to
// their complete English source so localization preserves both the intended
// text and the original reveal timing.
[[nodiscard]] std::optional<std::string_view>
completeGameplayTextSource(std::string_view observed) noexcept;

struct LocalizedMissionBriefing {
  std::string location;
  std::string mission_title;
  std::string date_time;
  std::string directive;
  std::string additional_directive;
};

[[nodiscard]] std::optional<LocalizedMissionBriefing>
localizedMissionBriefing(std::uint32_t mission_index) noexcept;

struct LocalizedMissionMenuTexts {
  std::vector<std::string> objectives;
  std::vector<std::string> parameters;
};

[[nodiscard]] std::optional<LocalizedMissionMenuTexts>
localizedMissionMenuTexts(std::uint32_t mission_index,
                          std::span<const std::string> objectives,
                          std::span<const std::string> parameters) noexcept;

} // namespace sf::game
