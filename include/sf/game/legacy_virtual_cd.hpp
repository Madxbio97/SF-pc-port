#pragma once

#include "sf/psx/cdrom.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::disc {
class RawSectorSource;
}

namespace sf::game {

enum class LegacyCdResult : std::uint32_t {
  success = 0U,
  invalid_argument = 1U,
  no_free_handle = 3U,
  empty_file = 4U,
  not_found = 5U,
};

struct LegacyCdReadResult {
  LegacyCdResult result{LegacyCdResult::success};
  std::uint32_t bytes_read{};
  std::uint32_t transfer_size{};
};

// Immutable description of the next sector-aligned file read. Planning does
// not advance the file cursor, allowing the transfer to be routed through the
// emulated CD-ROM and DMA3 before it is committed.
struct LegacyCdReadPlan {
  LegacyCdResult result{LegacyCdResult::success};
  std::uint32_t sector{};
  std::uint32_t bytes_read{};
  std::uint32_t transfer_size{};
};

struct LegacyCdLocation {
  std::uint32_t sector{};
  std::uint32_t size{};
};

// Deterministic, file-backed replacement for the PSX CD transport. The retail
// filesystem, loaders, relocations and allocator continue to run in guest code.
class LegacyVirtualCd final : public psx::CdRomMedia {
private:
  using FileBytes = std::shared_ptr<const std::vector<std::byte>>;

  struct OpenFile {
    FileBytes bytes;
    std::uint32_t start_sector{};
    std::uint32_t cursor{};

    friend bool operator==(const OpenFile &, const OpenFile &) = default;
  };

public:
  LegacyVirtualCd();
  ~LegacyVirtualCd() override;

  // Mutable transport state only. The catalog remains host configuration,
  // so a snapshot is valid only for the CD instance that created it.
  class Snapshot final {
  public:
    Snapshot() = default;

    friend bool operator==(const Snapshot &, const Snapshot &) = default;

  private:
    friend class LegacyVirtualCd;

    const LegacyVirtualCd *owner_{};
    std::optional<std::string> mounted_archive_;
    std::map<std::uint32_t, OpenFile> open_files_;
    std::uint32_t current_raw_sector_{};
  };

  static constexpr std::uint32_t sector_size = 2048U;
  static constexpr std::uint32_t maximum_open_files = 5U;
  static constexpr std::uint32_t root_sector_base = 150U;
  // Keep synthetic FOG-member LBAs above the supported disc image. The old
  // 0x40000 base overlapped XA/INGAME.XA and routed SLF.RFF reads into raw XA
  // sectors, leaving the retail sound-bank object filled with allocator poison.
  static constexpr std::uint32_t archive_sector_base = 0x00050000U;
  static constexpr std::byte sector_padding{0x00};

  [[nodiscard]] bool addRootFile(std::string path,
                                 std::span<const std::byte> bytes);
  [[nodiscard]] bool
  addArchiveFile(std::string archive_path, std::string path,
                 std::span<const std::byte> bytes,
                 std::optional<std::uint32_t> start_sector = std::nullopt);
  [[nodiscard]] bool
  attachRawSectorSource(std::unique_ptr<disc::RawSectorSource> source,
                        std::uint32_t first_lba,
                        std::uint32_t sector_count) noexcept;
  // Exposes the ISO9660 name of the attached raw extent to CdSearchFile.
  // Streaming still uses readRawSector(); the large XA file is not copied.
  [[nodiscard]] bool registerRawExtentFile(std::string path,
                                           std::uint32_t byte_size);

  [[nodiscard]] bool mount(std::string_view archive_path);
  void unmount() noexcept;
  [[nodiscard]] std::optional<std::string_view> mountedArchive() const noexcept;
  [[nodiscard]] std::optional<LegacyCdLocation>
  locate(std::string_view path) const;
  [[nodiscard]] LegacyCdReadResult
  readSectors(std::uint32_t sector,
              std::span<std::byte> destination) const noexcept;
  [[nodiscard]] std::uint32_t sectorCount() const noexcept override;
  [[nodiscard]] bool
  readDataSector(std::uint32_t lba,
                 std::span<std::byte, psx::CdRomMedia::sector_size>
                     destination) noexcept override;
  [[nodiscard]] bool
  readRawSector(std::uint32_t lba,
                std::span<std::byte, psx::CdRomMedia::raw_sector_size>
                    destination) noexcept override;

  [[nodiscard]] LegacyCdResult open(std::string_view path,
                                    std::uint32_t &handle);
  [[nodiscard]] LegacyCdResult paddedSize(std::uint32_t handle,
                                          std::uint32_t &size) const noexcept;
  [[nodiscard]] LegacyCdReadPlan
  planRead(std::uint32_t handle, std::uint32_t requested_bytes) const noexcept;
  [[nodiscard]] LegacyCdResult commitRead(std::uint32_t handle,
                                          std::uint32_t expected_sector,
                                          std::uint32_t bytes_read) noexcept;
  [[nodiscard]] LegacyCdReadResult
  read(std::uint32_t handle, std::span<std::byte> destination) noexcept;
  [[nodiscard]] LegacyCdResult rewind(std::uint32_t handle) noexcept;
  [[nodiscard]] LegacyCdResult close(std::uint32_t handle) noexcept;

  [[nodiscard]] Snapshot captureSnapshot() const;
  [[nodiscard]] bool restoreSnapshot(const Snapshot &snapshot) noexcept;

  [[nodiscard]] std::uint32_t currentRawSector() const noexcept {
    return current_raw_sector_;
  }
  void setCurrentRawSector(std::uint32_t sector) noexcept {
    current_raw_sector_ = sector;
  }
  void advanceRawSectors(std::uint32_t sectors) noexcept {
    current_raw_sector_ += sectors;
  }

private:
  struct FileRecord {
    FileBytes bytes;
    std::uint32_t start_sector{};
  };

  using FileMap = std::map<std::string, FileRecord, std::less<>>;

  struct ArchiveCatalog {
    FileMap files;
    std::uint32_t next_sector{1U};
  };

  struct FileLookup {
    const FileRecord *record{};
    bool archive{};
  };

  [[nodiscard]] static std::string normalizePath(std::string_view path);
  [[nodiscard]] static std::string_view
  basename(std::string_view path) noexcept;
  [[nodiscard]] FileLookup
  findFile(std::string_view normalized_path) const noexcept;
  [[nodiscard]] bool catalogContains(const OpenFile &file) const noexcept;

  FileMap root_files_;
  std::map<std::string, ArchiveCatalog, std::less<>> archive_files_;
  std::optional<std::string> mounted_archive_;
  std::map<std::uint32_t, OpenFile> open_files_;
  std::unique_ptr<disc::RawSectorSource> raw_sector_source_;
  std::string raw_extent_path_;
  std::uint32_t raw_extent_byte_size_{};
  std::uint32_t raw_extent_first_lba_{};
  std::uint32_t raw_extent_sector_count_{};
  std::uint32_t current_raw_sector_{};
  std::uint32_t next_root_sector_{root_sector_base};
};

} // namespace sf::game
