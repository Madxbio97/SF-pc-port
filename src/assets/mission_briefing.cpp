#include "sf/assets/mission_briefing.hpp"

#include "sf/core/error.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sf::assets {
namespace {

constexpr std::size_t data_offset_field = 0x14U;
constexpr std::size_t briefing_header_size = 0x18U;

std::uint32_t readLe32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
    throw core::Error{core::ErrorCode::invalid_format, "Truncated DLF header"};
  }
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

bool isTextByte(std::byte value) noexcept {
  const auto character = std::to_integer<unsigned char>(value);
  return character == '\n' || character == '\r' || character == '\t' ||
         (character >= 0x20U && character <= 0x7eU) ||
         (character >= 0xdfU && character <= 0xfcU);
}

void trimLineEnding(std::string &value) {
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
    value.pop_back();
  }
}

bool isDateTime(std::string_view value) noexcept {
  return value.size() >= 11U && value[2] == '/' && value[5] == ' ' &&
         value[8] == ':' && value[0] >= '0' && value[0] <= '9' &&
         value[1] >= '0' && value[1] <= '9' && value[3] >= '0' &&
         value[3] <= '9' && value[4] >= '0' && value[4] <= '9' &&
         value[6] >= '0' && value[6] <= '9' && value[7] >= '0' &&
         value[7] <= '9' && value[9] >= '0' && value[9] <= '9' &&
         value[10] >= '0' && value[10] <= '9';
}

std::vector<std::string> readBriefingStrings(std::span<const std::byte> dlf) {
  const auto data_offset =
      static_cast<std::size_t>(readLe32(dlf, data_offset_field));
  if (data_offset > dlf.size() ||
      dlf.size() - data_offset < briefing_header_size) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "DLF briefing header is invalid"};
  }
  const auto first_text = data_offset + briefing_header_size;
  auto limit = dlf.size();
  const auto archive_offset = static_cast<std::size_t>(readLe32(dlf, 0U));
  if (archive_offset > first_text && archive_offset <= dlf.size()) {
    limit = archive_offset;
  }

  std::vector<std::string> strings;
  auto cursor = first_text;
  while (cursor < limit) {
    while (cursor < limit && !isTextByte(dlf[cursor])) {
      ++cursor;
    }
    const auto start = cursor;
    while (cursor < limit && isTextByte(dlf[cursor])) {
      ++cursor;
    }
    if (cursor > start && cursor < limit && dlf[cursor] == std::byte{}) {
      std::string value(cursor - start, '\0');
      std::transform(dlf.begin() + static_cast<std::ptrdiff_t>(start),
                     dlf.begin() + static_cast<std::ptrdiff_t>(cursor),
                     value.begin(), [](std::byte character) {
                       return static_cast<char>(
                           std::to_integer<unsigned char>(character));
                     });
      strings.push_back(std::move(value));
    }
    ++cursor;
  }
  return strings;
}

std::vector<std::string>
readOverlayBriefingStrings(std::span<const std::byte> overlay) {
  std::vector<std::string> strings;
  auto cursor = std::size_t{};
  while (cursor < overlay.size()) {
    while (cursor < overlay.size() && !isTextByte(overlay[cursor])) {
      ++cursor;
    }
    const auto start = cursor;
    while (cursor < overlay.size() && isTextByte(overlay[cursor])) {
      ++cursor;
    }
    if (cursor - start >= 3U && cursor < overlay.size() &&
        overlay[cursor] == std::byte{}) {
      std::string value(cursor - start, '\0');
      std::transform(overlay.begin() + static_cast<std::ptrdiff_t>(start),
                     overlay.begin() + static_cast<std::ptrdiff_t>(cursor),
                     value.begin(), [](std::byte character) {
                       return static_cast<char>(
                           std::to_integer<unsigned char>(character));
                     });
      trimLineEnding(value);
      strings.push_back(std::move(value));
    }
    ++cursor;
  }
  return strings;
}

struct ParsedBriefingRecord {
  std::string location;
  std::string mission_title;
  std::string date_time;
  std::string directive;
  std::string additional_directive;
};

ParsedBriefingRecord parseBriefingRecord(std::vector<std::string> strings,
                                         std::size_t record_index,
                                         std::string_view mission_title) {
  struct Record {
    std::size_t date{};
  };
  std::vector<Record> records;
  for (std::size_t index = 2U; index + 1U < strings.size(); ++index) {
    if (isDateTime(strings[index])) {
      records.push_back(Record{index});
    }
  }
  if (record_index >= records.size()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Mission briefing record is missing"};
  }

  const auto date = records[record_index].date;
  auto date_time = strings[date];
  trimLineEnding(date_time);
  auto title =
      mission_title.empty() ? strings[date + 1U] : std::string{mission_title};
  // Shared continuation tables store the geographic location only after
  // their first record. Every later record inherits it.
  auto location = std::string{};
  const auto first_date = records.front().date;
  if (first_date + 2U < strings.size()) {
    location = strings[first_date + 2U];
  }
  return ParsedBriefingRecord{
      std::move(location),           std::move(title),
      std::move(date_time),          std::move(strings[date - 1U]),
      std::move(strings[date - 2U]),
  };
}

} // namespace

MissionBriefing::MissionBriefing(std::string location,
                                 std::string mission_title,
                                 std::string date_time, std::string directive,
                                 std::string additional_directive)
    : location_(std::move(location)), mission_title_(std::move(mission_title)),
      date_time_(std::move(date_time)), directive_(std::move(directive)),
      additional_directive_(std::move(additional_directive)) {}

MissionBriefing MissionBriefing::parse(std::span<const std::byte> dlf) {
  return parseRecord(dlf, 0U);
}

MissionBriefing MissionBriefing::parseRecord(std::span<const std::byte> dlf,
                                             std::size_t record_index,
                                             std::string_view mission_title) {
  auto parsed = parseBriefingRecord(readBriefingStrings(dlf), record_index,
                                    mission_title);
  return MissionBriefing{
      std::move(parsed.location), std::move(parsed.mission_title),
      std::move(parsed.date_time), std::move(parsed.directive),
      std::move(parsed.additional_directive)};
}

MissionBriefing
MissionBriefing::parseOverlayRecord(std::span<const std::byte> overlay,
                                    std::size_t record_index,
                                    std::string_view mission_title) {
  auto parsed = parseBriefingRecord(readOverlayBriefingStrings(overlay),
                                    record_index, mission_title);
  return MissionBriefing{
      std::move(parsed.location), std::move(parsed.mission_title),
      std::move(parsed.date_time), std::move(parsed.directive),
      std::move(parsed.additional_directive)};
}

MissionBriefing MissionBriefing::fallback(std::string title) {
  return MissionBriefing{"", std::move(title), "", "", ""};
}

MissionBriefing MissionBriefing::fromFields(std::string location,
                                            std::string mission_title,
                                            std::string date_time,
                                            std::string directive,
                                            std::string additional_directive) {
  return MissionBriefing{std::move(location), std::move(mission_title),
                         std::move(date_time), std::move(directive),
                         std::move(additional_directive)};
}

std::string MissionBriefing::retailTitle() const {
  if (location_.empty()) {
    return mission_title_;
  }
  auto result = location_;
  result += ": ";
  result += mission_title_;
  return result;
}

} // namespace sf::assets
