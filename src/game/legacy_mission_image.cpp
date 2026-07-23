#include "sf/game/legacy_mission_image.hpp"

#include "sf/assets/fog_archive.hpp"
#include "sf/core/error.hpp"
#include "sf/disc/raw_sector_source.hpp"
#include "sf/game/game_disc.hpp"
#include "sf/game/legacy_virtual_cd.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sf::game {
namespace {

struct RootFile {
  std::string path;
  std::vector<std::byte> bytes;
};

struct ArchiveFile {
  std::string path;
  std::vector<std::byte> bytes;
  std::uint32_t start_sector{};
};

std::vector<std::byte> copyBytes(std::span<const std::byte> bytes) {
  return {bytes.begin(), bytes.end()};
}

} // namespace

struct LegacyMissionImage::Storage {
  psx::Executable executable;
  std::filesystem::path cue_path;
  std::uint32_t xa_first_lba{};
  std::uint32_t xa_sector_count{};
  std::uint32_t xa_byte_size{};
  std::string archive_path;
  std::vector<RootFile> root_files;
  std::vector<ArchiveFile> archive_files;
};

LegacyMissionImage::LegacyMissionImage(
    std::shared_ptr<const Storage> storage) noexcept
    : storage_(std::move(storage)) {}

LegacyMissionImage
LegacyMissionImage::loadFirst(GameDisc &disc,
                              const assets::FogArchive &archive) {
  return load(disc, archive, "FOG/SUBWAY.FOG");
}

LegacyMissionImage LegacyMissionImage::load(GameDisc &disc,
                                            const assets::FogArchive &archive,
                                            std::string_view archive_path) {
  constexpr std::array root_directories{"BIN", "COMMON"};
  constexpr std::string_view xa_path{"XA/INGAME.XA"};
  constexpr std::uint64_t iso_sector_size = 2048U;

  auto storage = std::make_shared<Storage>();
  storage->executable = disc.executable();
  storage->cue_path = disc.cuePath();
  storage->archive_path = archive_path;

  const auto xa_entry = disc.image().find(std::string{xa_path});
  const auto xa_sector_count =
      (static_cast<std::uint64_t>(xa_entry.size) + iso_sector_size - 1U) /
      iso_sector_size;
  if (xa_entry.is_directory || xa_sector_count == 0U ||
      xa_sector_count > std::numeric_limits<std::uint32_t>::max()) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Mission XA extent is invalid: " + std::string{xa_path}};
  }
  storage->xa_first_lba = xa_entry.extent_lba;
  storage->xa_sector_count = static_cast<std::uint32_t>(xa_sector_count);
  storage->xa_byte_size = xa_entry.size;

  for (const auto *directory : root_directories) {
    for (const auto &entry : disc.image().list(directory)) {
      if (entry.is_directory) {
        continue;
      }
      auto path = std::string{directory} + '/' + entry.name;
      storage->root_files.push_back(RootFile{
          path,
          disc.image().readFile(path),
      });
    }
  }

  storage->archive_files.reserve(archive.entries().size());
  for (const auto &entry : archive.entries()) {
    storage->archive_files.push_back(ArchiveFile{
        entry.name,
        copyBytes(archive.file(entry.name)),
        entry.start_sector,
    });
  }

  return LegacyMissionImage{std::move(storage)};
}

const psx::Executable &LegacyMissionImage::executable() const noexcept {
  return storage_->executable;
}

std::shared_ptr<LegacyVirtualCd> LegacyMissionImage::createVirtualCd() const {
  auto virtual_cd = std::make_shared<LegacyVirtualCd>();
  for (const auto &file : storage_->root_files) {
    if (!virtual_cd->addRootFile(file.path, file.bytes)) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Legacy virtual CD root file is too large: " +
                            file.path};
    }
  }
  for (const auto &file : storage_->archive_files) {
    if (!virtual_cd->addArchiveFile(storage_->archive_path, file.path,
                                    file.bytes, file.start_sector)) {
      throw core::Error{core::ErrorCode::invalid_format,
                        "Legacy virtual CD archive file is too large: " +
                            file.path};
    }
  }
  auto raw_sector_source = std::make_unique<disc::RawSectorSource>(
      disc::RawSectorSource::open(storage_->cue_path));
  if (!virtual_cd->attachRawSectorSource(std::move(raw_sector_source),
                                         storage_->xa_first_lba,
                                         storage_->xa_sector_count) ||
      !virtual_cd->registerRawExtentFile("XA/INGAME.XA",
                                         storage_->xa_byte_size)) {
    throw core::Error{core::ErrorCode::invalid_format,
                      "Mission XA extent lies outside the source data track"};
  }
  return virtual_cd;
}

std::size_t LegacyMissionImage::rootFileCount() const noexcept {
  return storage_->root_files.size();
}

std::size_t LegacyMissionImage::archiveFileCount() const noexcept {
  return storage_->archive_files.size();
}

std::string_view LegacyMissionImage::archivePath() const noexcept {
  return storage_->archive_path;
}

} // namespace sf::game
