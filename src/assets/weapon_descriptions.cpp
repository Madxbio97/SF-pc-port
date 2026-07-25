#include "sf/assets/weapon_descriptions.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace sf::assets {
namespace {

std::string trim(std::string_view source) {
    const auto first = source.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = source.find_last_not_of(" \t\r\n");
    return std::string{source.substr(first, last - first + 1U)};
}

std::string normalized(std::string_view source) {
    auto result = trim(source);
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

std::string fieldValue(std::string_view line) {
    const auto separator = line.find_first_of("\t");
    return separator == std::string_view::npos ? std::string{} : trim(line.substr(separator));
}

std::uint8_t romanRating(std::string_view source) noexcept {
    return static_cast<std::uint8_t>(std::min<std::size_t>(
        5U,
        static_cast<std::size_t>(std::count(source.begin(), source.end(), 'I'))));
}

WeaponDescription makeEntry(const std::vector<std::string>& lines) {
    WeaponDescription result;
    if (lines.empty()) {
        return result;
    }
    result.name = trim(lines.front());
    auto description_started = false;
    for (std::size_t index = 1; index < lines.size(); ++index) {
        const auto line = trim(lines[index]);
        if (line.empty()) {
            continue;
        }
        if (line.starts_with("Fire rate")) {
            result.fire_rate = romanRating(fieldValue(line));
            continue;
        }
        if (line.starts_with("Damage")) {
            result.damage = romanRating(fieldValue(line));
            continue;
        }
        if (line.starts_with("Clip size")) {
            result.clip_size = fieldValue(line);
            continue;
        }
        if (line.starts_with("Max rounds")) {
            result.maximum_rounds = fieldValue(line);
            description_started = true;
            continue;
        }
        if (line == "Description:") {
            description_started = true;
            continue;
        }
        if (description_started) {
            if (!result.description.empty()) {
                result.description.push_back(' ');
            }
            result.description += line;
        }
    }
    return result;
}

} // namespace

WeaponDescriptionTable::WeaponDescriptionTable(std::vector<WeaponDescription> entries)
    : entries_(std::move(entries)) {}

WeaponDescriptionTable WeaponDescriptionTable::parse(std::span<const std::byte> bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const auto value : bytes) {
        const auto character = std::to_integer<unsigned char>(value);
        if (character == 0U) {
            break;
        }
        // The only non-ASCII byte in the USA table is a Windows-1252 apostrophe.
        text.push_back(character == 0x92U ? '\'' : static_cast<char>(character));
    }

    std::vector<WeaponDescription> entries;
    std::vector<std::string> record;
    const auto flush = [&] {
        auto entry = makeEntry(record);
        if (!entry.name.empty() && normalized(entry.name) != "WEAPON_COUNT") {
            entries.push_back(std::move(entry));
        }
        record.clear();
    };
    std::size_t offset{};
    while (offset <= text.size()) {
        const auto end = text.find('\n', offset);
        const auto line = trim(std::string_view{text}.substr(
            offset,
            end == std::string::npos ? std::string::npos : end - offset));
        if (line == "*") {
            flush();
        } else {
            record.push_back(line);
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1U;
    }
    flush();
    if (entries.empty()) {
        throw core::Error{core::ErrorCode::invalid_format, "WEAPDESC.TXT has no weapon records"};
    }
    return WeaponDescriptionTable{std::move(entries)};
}

WeaponDescriptionTable WeaponDescriptionTable::parseRussianVit(
    std::span<const std::byte> bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const auto value : bytes) {
        const auto character = std::to_integer<unsigned char>(value);
        if (character == 0U) {
            break;
        }
        text.push_back(static_cast<char>(character));
    }

    std::vector<WeaponDescription> entries;
    std::vector<std::string> record;
    const auto flush = [&] {
        std::vector<std::string> lines;
        for (const auto &candidate : record) {
            const auto line = trim(candidate);
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        if (!lines.empty()) {
            WeaponDescription entry;
            entry.name = lines.front();
            // ViT translated the labels but retained the retail field order.
            if (lines.size() > 1U) {
                entry.fire_rate = romanRating(fieldValue(lines[1U]));
            }
            if (lines.size() > 2U) {
                entry.damage = romanRating(fieldValue(lines[2U]));
            }
            if (lines.size() > 3U) {
                entry.clip_size = fieldValue(lines[3U]);
            }
            if (lines.size() > 4U) {
                entry.maximum_rounds = fieldValue(lines[4U]);
            }
            for (std::size_t index = 5U; index < lines.size(); ++index) {
                if (!entry.description.empty()) {
                    entry.description.push_back(' ');
                }
                entry.description += lines[index];
            }
            entries.push_back(std::move(entry));
        }
        record.clear();
    };
    std::size_t offset{};
    while (offset <= text.size()) {
        const auto end = text.find('\n', offset);
        const auto line = trim(std::string_view{text}.substr(
            offset, end == std::string::npos ? std::string::npos
                                             : end - offset));
        if (line == "*") {
            flush();
        } else {
            record.push_back(line);
        }
        if (end == std::string::npos) {
            break;
        }
        offset = end + 1U;
    }
    flush();
    if (entries.empty()) {
        throw core::Error{core::ErrorCode::invalid_format,
                          "ViT WEAPDESC.TXT has no weapon records"};
    }
    return WeaponDescriptionTable{std::move(entries)};
}

const WeaponDescription* WeaponDescriptionTable::find(std::string_view name) const noexcept {
    const auto wanted = normalized(name);
    const auto match = std::ranges::find_if(entries_, [&](const WeaponDescription& entry) {
        return normalized(entry.name) == wanted;
    });
    return match == entries_.end() ? nullptr : &*match;
}

} // namespace sf::assets
