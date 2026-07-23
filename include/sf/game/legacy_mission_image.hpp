#pragma once

#include "sf/psx/executable.hpp"

#include <cstddef>
#include <memory>
#include <string_view>

namespace sf::assets {
class FogArchive;
}

namespace sf::game {

class GameDisc;
class LegacyVirtualCd;

// Immutable retail program and CD catalog required by one mission VM. The
// source GameDisc is used only while the image is built; every runtime gets a
// fresh mutable LegacyVirtualCd instance from the retained bytes.
class LegacyMissionImage final {
public:
    [[nodiscard]] static LegacyMissionImage load(
        GameDisc& disc,
        const assets::FogArchive& archive,
        std::string_view archive_path);
    [[nodiscard]] static LegacyMissionImage loadFirst(
        GameDisc& disc,
        const assets::FogArchive& archive);

    [[nodiscard]] const psx::Executable& executable() const noexcept;
    [[nodiscard]] std::shared_ptr<LegacyVirtualCd> createVirtualCd() const;
    [[nodiscard]] std::size_t rootFileCount() const noexcept;
    [[nodiscard]] std::size_t archiveFileCount() const noexcept;
    [[nodiscard]] std::string_view archivePath() const noexcept;

private:
    struct Storage;

    explicit LegacyMissionImage(std::shared_ptr<const Storage> storage) noexcept;

    std::shared_ptr<const Storage> storage_;
};

} // namespace sf::game
