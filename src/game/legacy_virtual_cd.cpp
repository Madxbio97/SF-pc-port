#include "sf/game/legacy_virtual_cd.hpp"

#include "sf/disc/raw_sector_source.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace sf::game {
namespace {

bool roundToSector(std::uint32_t size, std::uint32_t &rounded) noexcept {
  constexpr auto sector_size =
      static_cast<std::uint64_t>(LegacyVirtualCd::sector_size);
  const auto result = (static_cast<std::uint64_t>(size) + sector_size - 1U) &
                      ~(sector_size - 1U);
  if (result > std::numeric_limits<std::uint32_t>::max()) {
    rounded = 0U;
    return false;
  }
  rounded = static_cast<std::uint32_t>(result);
  return true;
}

} // namespace

LegacyVirtualCd::LegacyVirtualCd() = default;
LegacyVirtualCd::~LegacyVirtualCd() = default;

bool LegacyVirtualCd::addRootFile(std::string path,
                                  std::span<const std::byte> bytes) {
  if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  std::uint32_t padded_size{};
  if (!roundToSector(static_cast<std::uint32_t>(bytes.size()), padded_size)) {
    return false;
  }
  const auto sector_count = padded_size / sector_size;
  const auto end_sector =
      static_cast<std::uint64_t>(next_root_sector_) + sector_count;
  if (end_sector > archive_sector_base) {
    return false;
  }

  auto file = std::make_shared<const std::vector<std::byte>>(bytes.begin(),
                                                             bytes.end());
  root_files_.insert_or_assign(normalizePath(path),
                               FileRecord{std::move(file), next_root_sector_});
  next_root_sector_ = static_cast<std::uint32_t>(end_sector);
  return true;
}

bool LegacyVirtualCd::addArchiveFile(
    std::string archive_path, std::string path,
    std::span<const std::byte> bytes,
    std::optional<std::uint32_t> start_sector) {
  if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }

  std::uint32_t padded_size{};
  if (!roundToSector(static_cast<std::uint32_t>(bytes.size()), padded_size)) {
    return false;
  }
  const auto sector_count = padded_size / sector_size;
  const auto normalized_archive = normalizePath(archive_path);
  const auto normalized_path = normalizePath(path);
  const auto existing_archive = archive_files_.find(normalized_archive);
  const auto assigned_sector =
      start_sector.value_or(existing_archive == archive_files_.end()
                                ? 1U
                                : existing_archive->second.next_sector);
  if (assigned_sector == 0U) {
    return false;
  }

  const auto relative_end =
      static_cast<std::uint64_t>(assigned_sector) + sector_count;
  const auto absolute_start =
      static_cast<std::uint64_t>(archive_sector_base) + assigned_sector;
  const auto absolute_end =
      static_cast<std::uint64_t>(archive_sector_base) + relative_end;
  constexpr auto maximum_sector_end =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
      1U;
  if (absolute_start > std::numeric_limits<std::uint32_t>::max() ||
      absolute_end > maximum_sector_end) {
    return false;
  }

  if (existing_archive != archive_files_.end() && sector_count != 0U) {
    for (const auto &[name, record] : existing_archive->second.files) {
      if (name == normalized_path) {
        continue;
      }
      std::uint32_t existing_padded_size{};
      if (!roundToSector(static_cast<std::uint32_t>(record.bytes->size()),
                         existing_padded_size)) {
        return false;
      }
      const auto existing_sector_count = existing_padded_size / sector_size;
      const auto existing_end =
          static_cast<std::uint64_t>(record.start_sector) +
          existing_sector_count;
      if (existing_sector_count != 0U && assigned_sector < existing_end &&
          record.start_sector < relative_end) {
        return false;
      }
    }
  }

  auto file = std::make_shared<const std::vector<std::byte>>(bytes.begin(),
                                                             bytes.end());
  auto &archive = archive_files_[normalized_archive];
  archive.files.insert_or_assign(normalized_path,
                                 FileRecord{std::move(file), assigned_sector});
  archive.next_sector =
      std::max(archive.next_sector, static_cast<std::uint32_t>(relative_end));
  return true;
}

bool LegacyVirtualCd::attachRawSectorSource(
    std::unique_ptr<disc::RawSectorSource> source, std::uint32_t first_lba,
    std::uint32_t sector_count) noexcept {
  if (!source || sector_count == 0U || first_lba >= source->sectorCount() ||
      sector_count > source->sectorCount() - first_lba ||
      static_cast<std::uint64_t>(first_lba) + sector_count >
          archive_sector_base) {
    return false;
  }

  raw_sector_source_ = std::move(source);
  raw_extent_first_lba_ = first_lba;
  raw_extent_sector_count_ = sector_count;
  return true;
}

bool LegacyVirtualCd::registerRawExtentFile(std::string path,
                                            std::uint32_t byte_size) {
  if (raw_sector_source_ == nullptr || byte_size == 0U ||
      static_cast<std::uint64_t>(byte_size) >
          static_cast<std::uint64_t>(raw_extent_sector_count_) * sector_size) {
    return false;
  }
  auto normalized = normalizePath(path);
  if (normalized.empty() || root_files_.contains(normalized)) {
    return false;
  }
  raw_extent_path_ = std::move(normalized);
  raw_extent_byte_size_ = byte_size;
  return true;
}

bool LegacyVirtualCd::mount(std::string_view archive_path) {
  auto normalized = normalizePath(archive_path);
  if (!archive_files_.contains(normalized)) {
    unmount();
    return false;
  }
  mounted_archive_ = std::move(normalized);
  return true;
}

void LegacyVirtualCd::unmount() noexcept {
  mounted_archive_.reset();
  current_raw_sector_ = 0U;
}

std::optional<std::string_view>
LegacyVirtualCd::mountedArchive() const noexcept {
  if (!mounted_archive_) {
    return std::nullopt;
  }
  return *mounted_archive_;
}

std::optional<LegacyCdLocation>
LegacyVirtualCd::locate(std::string_view path) const {
  const auto normalized = normalizePath(path);
  if (!raw_extent_path_.empty() && normalized == raw_extent_path_) {
    return LegacyCdLocation{raw_extent_first_lba_, raw_extent_byte_size_};
  }
  const auto file = findFile(normalized);
  if (file.record == nullptr) {
    return std::nullopt;
  }
  const auto sector =
      file.record->start_sector + (file.archive ? archive_sector_base : 0U);
  return LegacyCdLocation{
      sector, static_cast<std::uint32_t>(file.record->bytes->size())};
}

LegacyCdReadResult
LegacyVirtualCd::readSectors(std::uint32_t sector,
                             std::span<std::byte> destination) const noexcept {
  if (destination.empty() ||
      destination.size() > std::numeric_limits<std::uint32_t>::max() ||
      destination.size() % sector_size != 0U) {
    return {LegacyCdResult::invalid_argument, 0U, 0U};
  }

  const auto transfer_size = static_cast<std::uint32_t>(destination.size());
  const auto requested_sector_count = transfer_size / sector_size;
  const auto requested_end =
      static_cast<std::uint64_t>(sector) + requested_sector_count;
  const auto read_record =
      [&](const FileRecord &record,
          std::uint32_t file_sector) -> std::optional<LegacyCdReadResult> {
    std::uint32_t padded_size{};
    if (!roundToSector(static_cast<std::uint32_t>(record.bytes->size()),
                       padded_size)) {
      return std::nullopt;
    }
    const auto file_sector_count = padded_size / sector_size;
    const auto file_end =
        static_cast<std::uint64_t>(file_sector) + file_sector_count;
    if (sector < file_sector || requested_end > file_end) {
      return std::nullopt;
    }

    const auto byte_offset = static_cast<std::size_t>(sector - file_sector) *
                             static_cast<std::size_t>(sector_size);
    const auto available = byte_offset < record.bytes->size()
                               ? record.bytes->size() - byte_offset
                               : 0U;
    const auto bytes_read = std::min(destination.size(), available);
    std::ranges::copy_n(record.bytes->begin() + byte_offset, bytes_read,
                        destination.begin());
    std::fill(destination.begin() + bytes_read, destination.end(),
              sector_padding);
    return LegacyCdReadResult{LegacyCdResult::success,
                              static_cast<std::uint32_t>(bytes_read),
                              transfer_size};
  };

  if (sector < archive_sector_base) {
    for (const auto &[path, record] : root_files_) {
      static_cast<void>(path);
      if (auto result = read_record(record, record.start_sector)) {
        return *result;
      }
    }
  } else if (mounted_archive_) {
    const auto archive = archive_files_.find(*mounted_archive_);
    if (archive != archive_files_.end()) {
      for (const auto &[path, record] : archive->second.files) {
        static_cast<void>(path);
        if (auto result = read_record(record, archive_sector_base +
                                                  record.start_sector)) {
          return *result;
        }
      }

      // Raw PSX reads are addressed against the mounted FOG image, not
      // against an individual member. Retail room streaming can start
      // near the end of WLDEMD.HOG and intentionally transfer through
      // the following sector-aligned member in one CdRead call.
      const auto relative_sector =
          static_cast<std::uint64_t>(sector) - archive_sector_base;
      const auto relative_end = relative_sector + requested_sector_count;
      if (relative_end > archive->second.next_sector) {
        return {LegacyCdResult::invalid_argument, 0U, 0U};
      }
      const auto find_record =
          [&](std::uint64_t requested_sector) -> const FileRecord * {
        for (const auto &[path, record] : archive->second.files) {
          static_cast<void>(path);
          std::uint32_t padded_size{};
          if (!roundToSector(static_cast<std::uint32_t>(record.bytes->size()),
                             padded_size)) {
            return nullptr;
          }
          const auto record_end =
              static_cast<std::uint64_t>(record.start_sector) +
              padded_size / sector_size;
          if (record.start_sector <= requested_sector &&
              requested_sector < record_end) {
            return &record;
          }
        }
        return nullptr;
      };
      for (std::uint64_t index = 0U; index < requested_sector_count; ++index) {
        if (find_record(relative_sector + index) == nullptr) {
          return {LegacyCdResult::invalid_argument, 0U, 0U};
        }
      }

      std::uint32_t bytes_read{};
      for (std::uint64_t index = 0U; index < requested_sector_count; ++index) {
        const auto *record = find_record(relative_sector + index);
        const auto byte_offset =
            static_cast<std::size_t>(relative_sector + index -
                                     record->start_sector) *
            sector_size;
        const auto available = byte_offset < record->bytes->size()
                                   ? record->bytes->size() - byte_offset
                                   : 0U;
        const auto copied = std::min<std::size_t>(sector_size, available);
        const auto destination_offset =
            static_cast<std::size_t>(index) * sector_size;
        std::ranges::copy_n(record->bytes->begin() + byte_offset, copied,
                            destination.begin() + destination_offset);
        std::fill(destination.begin() + destination_offset + copied,
                  destination.begin() + destination_offset + sector_size,
                  sector_padding);
        bytes_read += static_cast<std::uint32_t>(copied);
      }
      return {LegacyCdResult::success, bytes_read, transfer_size};
    }
  }
  return {LegacyCdResult::invalid_argument, 0U, 0U};
}

std::uint32_t LegacyVirtualCd::sectorCount() const noexcept {
  auto end = next_root_sector_;
  for (const auto &[path, archive] : archive_files_) {
    static_cast<void>(path);
    const auto archive_end =
        static_cast<std::uint64_t>(archive_sector_base) + archive.next_sector;
    end = std::max(
        end, static_cast<std::uint32_t>(std::min<std::uint64_t>(
                 archive_end, std::numeric_limits<std::uint32_t>::max())));
  }
  if (raw_sector_source_ != nullptr) {
    end = std::max(end, raw_extent_first_lba_ + raw_extent_sector_count_);
  }
  return end;
}

bool LegacyVirtualCd::readDataSector(
    std::uint32_t lba,
    std::span<std::byte, psx::CdRomMedia::sector_size> destination) noexcept {
  const auto result = readSectors(lba, destination);
  return result.result == LegacyCdResult::success &&
         result.transfer_size == destination.size();
}

bool LegacyVirtualCd::readRawSector(
    std::uint32_t lba, std::span<std::byte, psx::CdRomMedia::raw_sector_size>
                           destination) noexcept {
  if (raw_sector_source_ != nullptr && lba >= raw_extent_first_lba_ &&
      lba - raw_extent_first_lba_ < raw_extent_sector_count_) {
    try {
      raw_sector_source_->readSectors(lba, destination);
      return true;
    } catch (...) {
      std::fill(destination.begin(), destination.end(), std::byte{0});
      return false;
    }
  }
  return psx::CdRomMedia::readRawSector(lba, destination);
}

LegacyCdResult LegacyVirtualCd::open(std::string_view path,
                                     std::uint32_t &handle) {
  handle = 0U;
  const auto normalized = normalizePath(path);
  const auto file = findFile(normalized);
  if (file.record == nullptr) {
    return LegacyCdResult::not_found;
  }
  if (file.record->bytes->empty()) {
    return LegacyCdResult::empty_file;
  }
  const auto absolute_start =
      static_cast<std::uint64_t>(file.record->start_sector) +
      (file.archive ? archive_sector_base : 0U);
  if (absolute_start > std::numeric_limits<std::uint32_t>::max()) {
    return LegacyCdResult::invalid_argument;
  }
  for (std::uint32_t candidate = 1U; candidate <= maximum_open_files;
       ++candidate) {
    if (!open_files_.contains(candidate)) {
      open_files_.emplace(
          candidate, OpenFile{file.record->bytes,
                              static_cast<std::uint32_t>(absolute_start), 0U});
      handle = candidate;
      return LegacyCdResult::success;
    }
  }
  return LegacyCdResult::no_free_handle;
}

LegacyCdResult LegacyVirtualCd::paddedSize(std::uint32_t handle,
                                           std::uint32_t &size) const noexcept {
  size = 0U;
  const auto file = open_files_.find(handle);
  if (file == open_files_.end()) {
    return LegacyCdResult::invalid_argument;
  }
  return roundToSector(static_cast<std::uint32_t>(file->second.bytes->size()),
                       size)
             ? LegacyCdResult::success
             : LegacyCdResult::invalid_argument;
}

LegacyCdReadPlan
LegacyVirtualCd::planRead(std::uint32_t handle,
                          std::uint32_t requested_bytes) const noexcept {
  const auto file = open_files_.find(handle);
  if (file == open_files_.end() || !file->second.bytes ||
      file->second.cursor > file->second.bytes->size()) {
    return {LegacyCdResult::invalid_argument, 0U, 0U, 0U};
  }

  const auto transfer_capacity =
      requested_bytes - requested_bytes % sector_size;
  if (transfer_capacity == 0U) {
    return {LegacyCdResult::success, 0U, 0U, 0U};
  }

  const auto logical_size =
      static_cast<std::uint32_t>(file->second.bytes->size());
  if (file->second.cursor >= logical_size) {
    return {LegacyCdResult::success, 0U, 0U, 0U};
  }

  const auto remaining = logical_size - file->second.cursor;
  std::uint32_t padded_remaining{};
  if (!roundToSector(remaining, padded_remaining)) {
    return {LegacyCdResult::invalid_argument, 0U, 0U, 0U};
  }

  const auto sector = static_cast<std::uint64_t>(file->second.start_sector) +
                      file->second.cursor / sector_size;
  if (sector > std::numeric_limits<std::uint32_t>::max()) {
    return {LegacyCdResult::invalid_argument, 0U, 0U, 0U};
  }

  const auto transfer_size = std::min(transfer_capacity, padded_remaining);
  return LegacyCdReadPlan{LegacyCdResult::success,
                          static_cast<std::uint32_t>(sector),
                          std::min(transfer_size, remaining), transfer_size};
}

LegacyCdResult LegacyVirtualCd::commitRead(std::uint32_t handle,
                                           std::uint32_t expected_sector,
                                           std::uint32_t bytes_read) noexcept {
  const auto file = open_files_.find(handle);
  if (file == open_files_.end() || !file->second.bytes ||
      file->second.cursor > file->second.bytes->size()) {
    return LegacyCdResult::invalid_argument;
  }
  if (bytes_read == 0U) {
    return expected_sector == 0U ? LegacyCdResult::success
                                 : LegacyCdResult::invalid_argument;
  }

  const auto logical_size =
      static_cast<std::uint32_t>(file->second.bytes->size());
  if (file->second.cursor >= logical_size) {
    return LegacyCdResult::invalid_argument;
  }
  const auto sector = static_cast<std::uint64_t>(file->second.start_sector) +
                      file->second.cursor / sector_size;
  const auto remaining = logical_size - file->second.cursor;
  if (sector != expected_sector || bytes_read > remaining ||
      (bytes_read != remaining && bytes_read % sector_size != 0U)) {
    return LegacyCdResult::invalid_argument;
  }

  file->second.cursor += bytes_read;
  return LegacyCdResult::success;
}

LegacyCdReadResult
LegacyVirtualCd::read(std::uint32_t handle,
                      std::span<std::byte> destination) noexcept {
  if (destination.size() > std::numeric_limits<std::uint32_t>::max()) {
    return {LegacyCdResult::invalid_argument, 0U, 0U};
  }

  const auto plan =
      planRead(handle, static_cast<std::uint32_t>(destination.size()));
  if (plan.result != LegacyCdResult::success || plan.transfer_size == 0U) {
    return {plan.result, plan.bytes_read, plan.transfer_size};
  }

  const auto file = open_files_.find(handle);
  if (file == open_files_.end()) {
    return {LegacyCdResult::invalid_argument, 0U, 0U};
  }
  std::ranges::copy_n(file->second.bytes->begin() + file->second.cursor,
                      plan.bytes_read, destination.begin());
  std::fill(destination.begin() + plan.bytes_read,
            destination.begin() + plan.transfer_size, sector_padding);
  if (commitRead(handle, plan.sector, plan.bytes_read) !=
      LegacyCdResult::success) {
    return {LegacyCdResult::invalid_argument, 0U, 0U};
  }
  return {LegacyCdResult::success, plan.bytes_read, plan.transfer_size};
}

LegacyCdResult LegacyVirtualCd::rewind(std::uint32_t handle) noexcept {
  const auto file = open_files_.find(handle);
  if (file == open_files_.end()) {
    return LegacyCdResult::invalid_argument;
  }
  file->second.cursor = 0U;
  return LegacyCdResult::success;
}

LegacyCdResult LegacyVirtualCd::close(std::uint32_t handle) noexcept {
  return open_files_.erase(handle) != 0U ? LegacyCdResult::success
                                         : LegacyCdResult::invalid_argument;
}

LegacyVirtualCd::Snapshot LegacyVirtualCd::captureSnapshot() const {
  Snapshot snapshot;
  snapshot.owner_ = this;
  snapshot.mounted_archive_ = mounted_archive_;
  snapshot.open_files_ = open_files_;
  snapshot.current_raw_sector_ = current_raw_sector_;
  return snapshot;
}

bool LegacyVirtualCd::restoreSnapshot(const Snapshot &snapshot) noexcept {
  if (snapshot.owner_ != this ||
      snapshot.open_files_.size() > maximum_open_files ||
      (snapshot.mounted_archive_ &&
       !archive_files_.contains(*snapshot.mounted_archive_))) {
    return false;
  }
  for (const auto &[handle, file] : snapshot.open_files_) {
    if (handle == 0U || handle > maximum_open_files || !file.bytes ||
        file.bytes->empty() || file.cursor > file.bytes->size() ||
        (file.cursor != file.bytes->size() &&
         file.cursor % sector_size != 0U) ||
        !catalogContains(file)) {
      return false;
    }
  }

  try {
    auto mounted_archive = snapshot.mounted_archive_;
    auto open_files = snapshot.open_files_;
    mounted_archive_.swap(mounted_archive);
    open_files_.swap(open_files);
    current_raw_sector_ = snapshot.current_raw_sector_;
    return true;
  } catch (...) {
    return false;
  }
}

std::string LegacyVirtualCd::normalizePath(std::string_view path) {
  std::string result;
  result.reserve(path.size());
  for (const auto character : path) {
    if ((character == '/' || character == '\\') && result.empty()) {
      continue;
    }
    const auto normalized = character == '\\' ? '/' : character;
    result.push_back(static_cast<char>(
        std::toupper(static_cast<unsigned char>(normalized))));
  }
  const auto version = result.find(';');
  if (version != std::string::npos) {
    result.resize(version);
  }
  return result;
}

std::string_view LegacyVirtualCd::basename(std::string_view path) noexcept {
  const auto separator = path.find_last_of('/');
  return separator == std::string_view::npos ? path
                                             : path.substr(separator + 1U);
}

LegacyVirtualCd::FileLookup
LegacyVirtualCd::findFile(std::string_view normalized_path) const noexcept {
  if (mounted_archive_) {
    const auto archive = archive_files_.find(*mounted_archive_);
    if (archive != archive_files_.end()) {
      if (const auto file = archive->second.files.find(normalized_path);
          file != archive->second.files.end()) {
        return {&file->second, true};
      }
      if (const auto file =
              archive->second.files.find(basename(normalized_path));
          file != archive->second.files.end()) {
        return {&file->second, true};
      }
    }
  }
  if (const auto file = root_files_.find(normalized_path);
      file != root_files_.end()) {
    return {&file->second, false};
  }
  return {};
}

bool LegacyVirtualCd::catalogContains(const OpenFile &file) const noexcept {
  for (const auto &[path, record] : root_files_) {
    static_cast<void>(path);
    if (record.bytes == file.bytes &&
        record.start_sector == file.start_sector) {
      return true;
    }
  }
  for (const auto &[archive_path, archive] : archive_files_) {
    static_cast<void>(archive_path);
    for (const auto &[path, record] : archive.files) {
      static_cast<void>(path);
      const auto absolute_sector =
          static_cast<std::uint64_t>(archive_sector_base) + record.start_sector;
      if (record.bytes == file.bytes && absolute_sector == file.start_sector) {
        return true;
      }
    }
  }
  return false;
}

} // namespace sf::game
