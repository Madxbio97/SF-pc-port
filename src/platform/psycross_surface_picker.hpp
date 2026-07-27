#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sf::platform::detail {

enum class SurfacePickerKind : std::uint8_t {
  world,
  gmd_object,
  emd_object,
  scrim,
};

struct SurfacePickerId {
  SurfacePickerKind kind{};
  std::uint32_t owner{};
  std::uint32_t section{};
  std::uint32_t polygon{};

  friend bool operator==(const SurfacePickerId &,
                         const SurfacePickerId &) = default;
};

struct SurfacePickerPoint3 {
  double x{};
  double y{};
  double z{};
};

struct SurfacePickerPoint2 {
  std::int32_t x{};
  std::int32_t y{};
};

struct SurfacePickerUv {
  std::uint8_t u{};
  std::uint8_t v{};
};

struct SurfacePickerResidency {
  std::optional<unsigned int> physical_page;
  int required_logical_page{-1};
  int required_bank{-1};
  int loaded_logical_page{-1};
  int loaded_bank{-1};
  std::uint64_t generation{};
  std::uint64_t upload_write_sequence{};
  std::uint64_t residency_revision{};
  std::uint64_t authored_hash{};
  std::optional<std::uint64_t> runtime_expected_hash;
  bool scrim_dirty{};
  bool crate_overlay{};
  std::string owners;
};

struct SurfacePickerSurface {
  SurfacePickerId id;
  std::string label;
  std::uint64_t guest_frame{};
  std::uint16_t room{};
  std::uint8_t vertex_count{};
  std::array<SurfacePickerPoint3, 4U> world{};
  std::array<SurfacePickerPoint2, 4U> screen{};
  std::array<SurfacePickerUv, 4U> uv{};
  std::int32_t depth{};
  std::uint16_t raw_tpage{};
  std::uint16_t relocated_tpage{};
  std::uint16_t raw_clut{};
  std::uint16_t relocated_clut{};
  unsigned int logical_page{};
  unsigned int texture_bank{};
  SurfacePickerResidency residency;
};

struct SurfacePickerFrameContext {
  std::string mission;
  std::uint64_t guest_frame{};
  std::uint16_t room{};
  SurfacePickerPoint3 camera{};
};

// Diagnostic-only model-surface selector. It remains completely dormant until
// PageUp/PageDown is pressed; Insert writes a self-contained text report plus
// raw VRAM/page data and a decoded texture preview for the selected surface.
class PsyCrossSurfacePicker final {
public:
  void handleInput(std::span<const std::uint8_t> keyboard);
  void beginFrame(SurfacePickerFrameContext context);
  [[nodiscard]] bool observe(SurfacePickerSurface surface);
  void finishFrame();

  [[nodiscard]] bool active() const noexcept { return false; }

private:
  void navigate(int direction);
  void dumpSelected(const SurfacePickerSurface &surface) const;

  SurfacePickerFrameContext context_;
  std::vector<SurfacePickerSurface> visible_;
  std::vector<SurfacePickerSurface> previous_visible_;
  std::optional<SurfacePickerId> selected_;
  bool page_up_down_{};
  bool page_down_down_{};
  bool insert_down_{};
  bool dump_requested_{};
  mutable std::uint64_t dump_sequence_{};
};

} // namespace sf::platform::detail
