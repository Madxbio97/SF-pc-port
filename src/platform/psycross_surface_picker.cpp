#include "psycross_surface_picker.hpp"
#include "psycross_vram.hpp"

#include "sf/platform/player_input.hpp"

#include <PsyX/PsyX_globals.h>
#include <PsyX/PsyX_public.h>
#include <PsyX/PsyX_render.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace sf::platform::detail {
namespace {

constexpr auto vram_width = 1024U;
constexpr auto vram_height = 512U;
constexpr auto host_texture_height = 1536U;
constexpr auto page_word_width = 64U;
constexpr auto page_height = 256U;
constexpr auto picker_center_x = 192.0;
constexpr auto picker_center_y = 120.0;

bool keyDown(std::span<const std::uint8_t> keyboard,
             KeyboardMouseInput input) noexcept {
  const auto index = static_cast<std::size_t>(input);
  return index < keyboard.size() && keyboard[index] != 0U;
}

std::string_view kindName(SurfacePickerKind kind) noexcept {
  switch (kind) {
  case SurfacePickerKind::world:
    return "world";
  case SurfacePickerKind::gmd_object:
    return "gmd-object";
  case SurfacePickerKind::emd_object:
    return "emd-object";
  case SurfacePickerKind::scrim:
    return "scrim";
  }
  return "unknown";
}

std::uint64_t fingerprint(std::span<const std::uint16_t> words) noexcept {
  auto value = std::uint64_t{1469598103934665603ULL};
  for (const auto word : words) {
    value ^= static_cast<std::uint8_t>(word & 0xffU);
    value *= 1099511628211ULL;
    value ^= static_cast<std::uint8_t>(word >> 8U);
    value *= 1099511628211ULL;
  }
  return value;
}

std::string safeName(std::string_view value) {
  auto result = std::string{};
  result.reserve(value.size());
  for (const auto character : value) {
    const auto valid = (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') ||
                       character == '-' || character == '_';
    result.push_back(valid ? character : '_');
  }
  return result.empty() ? "mission" : result;
}

std::array<std::uint8_t, 3U> psxColor(std::uint16_t color) noexcept {
  const auto expand = [](unsigned int value) {
    return static_cast<std::uint8_t>((value << 3U) | (value >> 2U));
  };
  return {expand(color & 0x1fU), expand((color >> 5U) & 0x1fU),
          expand((color >> 10U) & 0x1fU)};
}

std::uint16_t texturePixel(std::span<const std::uint16_t> vram,
                           std::uint16_t tpage, std::uint16_t clut,
                           unsigned int u, unsigned int v) noexcept {
  const auto mode = (tpage >> 7U) & 0x3U;
  const auto extension = static_cast<unsigned int>((tpage >> 10U) & 0x3fU);
  const auto page_x = extension != 0U
                          ? ((extension - 1U) & 15U) * 64U
                          : static_cast<unsigned int>(tpage & 0x0fU) * 64U;
  const auto page_y = extension != 0U
                          ? 512U + ((extension - 1U) / 16U) * 256U
                          : (tpage & 0x10U) != 0U ? 256U : 0U;
  v &= 0xffU;
  if (mode == 2U || mode == 3U) {
    const auto x = page_x + (u & 0x3fU);
    return vram[(page_y + v) * vram_width + x];
  }
  const auto pixels_per_word = mode == 0U ? 4U : 2U;
  const auto bits = mode == 0U ? 4U : 8U;
  const auto x = page_x + ((u & 0xffU) / pixels_per_word);
  const auto packed = vram[(page_y + v) * vram_width + x];
  const auto shift = (u % pixels_per_word) * bits;
  const auto mask = mode == 0U ? 0x0fU : 0xffU;
  const auto palette = (packed >> shift) & mask;
  const auto clut_x = static_cast<unsigned int>(clut & 0x3fU) * 16U;
  const auto clut_y = static_cast<unsigned int>(clut >> 6U);
  const auto address = clut_y * vram_width + clut_x + palette;
  return address < vram.size() ? vram[address] : 0U;
}

void writeBinary(const std::filesystem::path &path,
                 std::span<const std::uint16_t> words) {
  auto output = std::ofstream{path, std::ios::binary};
  output.write(reinterpret_cast<const char *>(words.data()),
               static_cast<std::streamsize>(words.size_bytes()));
}

void writePreview(const std::filesystem::path &path,
                  std::span<const std::uint16_t> vram,
                  const SurfacePickerSurface &surface) {
  const auto mode = (surface.relocated_tpage >> 7U) & 0x3U;
  const auto width = mode == 0U ? 256U : mode == 1U ? 128U : 64U;
  auto output = std::ofstream{path, std::ios::binary};
  output << "P6\n" << width << ' ' << page_height << "\n255\n";
  for (auto y = 0U; y < page_height; ++y) {
    for (auto x = 0U; x < width; ++x) {
      const auto rgb = psxColor(texturePixel(vram, surface.relocated_tpage,
                                             surface.relocated_clut, x, y));
      output.write(reinterpret_cast<const char *>(rgb.data()),
                   static_cast<std::streamsize>(rgb.size()));
    }
  }
}

double selectionScore(const SurfacePickerSurface &surface) noexcept {
  auto centre_x = 0.0;
  auto centre_y = 0.0;
  for (auto index = 0U; index < surface.vertex_count; ++index) {
    centre_x += surface.screen[index].x;
    centre_y += surface.screen[index].y;
  }
  const auto divisor = std::max<unsigned int>(surface.vertex_count, 1U);
  centre_x /= divisor;
  centre_y /= divisor;
  const auto dx = centre_x - picker_center_x;
  const auto dy = centre_y - picker_center_y;
  return dx * dx + dy * dy + static_cast<double>(surface.depth) * 0.001;
}

} // namespace

void PsyCrossSurfacePicker::handleInput(
    std::span<const std::uint8_t> keyboard) {
  static_cast<void>(keyboard);
}

void PsyCrossSurfacePicker::beginFrame(SurfacePickerFrameContext context) {
  context_ = std::move(context);
  visible_.clear();
}

bool PsyCrossSurfacePicker::observe(SurfacePickerSurface surface) {
  static_cast<void>(surface);
  return false;
}

void PsyCrossSurfacePicker::finishFrame() {
}

void PsyCrossSurfacePicker::navigate(int direction) {
  if (previous_visible_.empty()) {
    PsyX_Log_Info("[SurfacePicker] no visible model surfaces yet\n");
    return;
  }
  auto index = std::size_t{};
  if (selected_) {
    const auto match = std::ranges::find(previous_visible_, *selected_,
                                         &SurfacePickerSurface::id);
    if (match != previous_visible_.end()) {
      index = static_cast<std::size_t>(match - previous_visible_.begin());
      index = direction > 0 ? (index + 1U) % previous_visible_.size()
                            : (index + previous_visible_.size() - 1U) %
                                  previous_visible_.size();
    }
  }
  selected_ = previous_visible_[index].id;
  const auto &surface = previous_visible_[index];
  PsyX_Log_Info(
      "[SurfacePicker] selected=%zu/%zu kind=%.*s owner=%u section=%u "
      "polygon=%u label=%s page=%u bank=%u (Insert dumps diagnostics)\n",
      index + 1U, previous_visible_.size(),
      static_cast<int>(kindName(surface.id.kind).size()),
      kindName(surface.id.kind).data(), surface.id.owner, surface.id.section,
      surface.id.polygon, surface.label.c_str(), surface.logical_page,
      surface.texture_bank);
}

void PsyCrossSurfacePicker::dumpSelected(
    const SurfacePickerSurface &surface) const {
  try {
    auto directory = std::filesystem::current_path() / "surface_dumps";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
      throw std::system_error{error};
    }
    const auto sequence = ++dump_sequence_;
    const auto stem =
        safeName(context_.mission) + "_room" + std::to_string(surface.room) +
        "_frame" + std::to_string(surface.guest_frame) + "_" +
        std::string{kindName(surface.id.kind)} + "_" +
        std::to_string(surface.id.owner) + "_" +
        std::to_string(surface.id.section) + "_" +
        std::to_string(surface.id.polygon) + "_" + std::to_string(sequence);
    const auto report_path = directory / (stem + ".txt");
    const auto vram_path = directory / (stem + ".vram.bin");
    const auto page_path = directory / (stem + ".page.bin");
    const auto preview_path = directory / (stem + ".ppm");

    std::vector<std::uint16_t> vram(vram_width * host_texture_height);
    GR_ReadVRAM(vram.data(), 0, 0, static_cast<int>(vram_width),
                static_cast<int>(vram_height));
    std::array<std::uint16_t, texture_page_bytes / sizeof(std::uint16_t)>
        extension_page{};
    for (auto extension = 0U; extension < extended_texture_page_count;
         ++extension) {
      readTexturePageAt(psx_texture_page_count + extension, extension_page);
      const auto page_x = (extension & 15U) * page_word_width;
      const auto page_y = vram_height + (extension / 16U) * page_height;
      for (auto row = 0U; row < page_height; ++row) {
        std::ranges::copy_n(
            extension_page.begin() +
                static_cast<std::ptrdiff_t>(row * page_word_width),
            page_word_width,
            vram.begin() + static_cast<std::ptrdiff_t>(
                               (page_y + row) * vram_width + page_x));
      }
    }
    writeBinary(vram_path, vram);

    const auto extension =
        static_cast<unsigned int>((surface.relocated_tpage >> 10U) & 0x3fU);
    const auto encoded_page = extension != 0U
                                  ? psx_texture_page_count + extension - 1U
                                  : static_cast<unsigned int>(
                                        surface.relocated_tpage & 0x1fU);
    const auto physical_page =
        surface.residency.physical_page.value_or(encoded_page);
    std::vector<std::uint16_t> page(page_word_width * page_height);
    readTexturePageAt(physical_page, page);
    writeBinary(page_path, page);
    writePreview(preview_path, vram, surface);

    std::array<GrVRAMWriteEvent, 4096U> writes{};
    const auto write_count =
        GR_ReadVRAMWriteEvents(surface.residency.upload_write_sequence,
                               writes.data(), static_cast<int>(writes.size()));
    const auto page_x = (physical_page & 15U) * page_word_width;
    const auto page_y = physical_page > 15U ? page_height : 0U;
    const auto intersects_page = [&](const GrVRAMWriteEvent &write) {
      return physical_page < psx_texture_page_count &&
             write.destination_x < static_cast<int>(page_x + page_word_width) &&
             write.destination_x + write.width > static_cast<int>(page_x) &&
             write.destination_y < static_cast<int>(page_y + page_height) &&
             write.destination_y + write.height > static_cast<int>(page_y);
    };

    auto output = std::ofstream{report_path};
    output << "Syphon Filter PC Surface Picker\n"
           << "mission=" << context_.mission << "\n"
           << "guest_frame=" << surface.guest_frame << "\n"
           << "room=" << surface.room << "\n"
           << "camera=" << context_.camera.x << ',' << context_.camera.y << ','
           << context_.camera.z << "\n"
           << "kind=" << kindName(surface.id.kind) << "\n"
           << "label=" << surface.label << "\n"
           << "owner=" << surface.id.owner << "\n"
           << "section=" << surface.id.section << "\n"
           << "polygon=" << surface.id.polygon << "\n"
           << "depth=" << surface.depth << "\n"
           << "texture_bank=" << surface.texture_bank << "\n"
           << "logical_page=" << surface.logical_page << "\n"
           << std::hex << std::setfill('0') << "raw_tpage=0x" << std::setw(4)
           << surface.raw_tpage << "\n"
           << "relocated_tpage=0x" << std::setw(4) << surface.relocated_tpage
           << "\n"
           << "raw_clut=0x" << std::setw(4) << surface.raw_clut << "\n"
           << "relocated_clut=0x" << std::setw(4) << surface.relocated_clut
           << "\n"
           << std::dec << "physical_page=" << physical_page << "\n"
           << "resident_physical_page=";
    if (surface.residency.physical_page) {
      output << *surface.residency.physical_page;
    } else {
      output << "none";
    }
    output << "\nrequired_logical_page="
           << surface.residency.required_logical_page
           << "\nrequired_bank=" << surface.residency.required_bank
           << "\nloaded_logical_page=" << surface.residency.loaded_logical_page
           << "\nloaded_bank=" << surface.residency.loaded_bank
           << "\ngeneration=" << surface.residency.generation
           << "\nresidency_revision=" << surface.residency.residency_revision
           << "\nupload_write_sequence="
           << surface.residency.upload_write_sequence
           << "\nowners=" << surface.residency.owners
           << "\nscrim_dirty=" << surface.residency.scrim_dirty
           << "\ncrate_overlay=" << surface.residency.crate_overlay << '\n'
           << std::hex << std::setfill('0') << "authored_hash=0x"
           << std::setw(16) << surface.residency.authored_hash
           << "\nactual_page_hash=0x" << std::setw(16) << fingerprint(page)
           << "\nruntime_expected_hash=";
    if (surface.residency.runtime_expected_hash) {
      output << "0x" << std::setw(16)
             << *surface.residency.runtime_expected_hash;
    } else {
      output << "none";
    }
    output << std::dec << "\nvertex_count="
           << static_cast<unsigned int>(surface.vertex_count) << '\n';
    for (auto index = 0U; index < surface.vertex_count; ++index) {
      output << "vertex[" << index << "].world=" << surface.world[index].x
             << ',' << surface.world[index].y << ',' << surface.world[index].z
             << " screen=" << surface.screen[index].x << ','
             << surface.screen[index].y
             << " uv=" << static_cast<unsigned int>(surface.uv[index].u) << ','
             << static_cast<unsigned int>(surface.uv[index].v) << " color=0x"
             << std::hex << std::setw(4)
             << texturePixel(vram, surface.relocated_tpage,
                             surface.relocated_clut, surface.uv[index].u,
                             surface.uv[index].v)
             << std::dec << '\n';
    }
    const auto clut_x =
        static_cast<unsigned int>(surface.relocated_clut & 0x3fU) * 16U;
    const auto clut_y = static_cast<unsigned int>(surface.relocated_clut >> 6U);
    output << "clut_location=" << clut_x << ',' << clut_y << "\nclut_words=";
    const auto palette_size =
        ((surface.relocated_tpage >> 7U) & 0x3U) == 0U ? 16U : 256U;
    for (auto index = 0U; index < palette_size; ++index) {
      const auto address = clut_y * vram_width + clut_x + index;
      if (address >= vram.size()) {
        break;
      }
      output << (index == 0U ? "" : ",") << "0x" << std::hex << std::setw(4)
             << vram[address];
    }
    output << std::dec << "\n\nVRAM writes intersecting selected page:\n";
    for (auto index = 0; index < write_count; ++index) {
      const auto &write = writes[static_cast<std::size_t>(index)];
      if (!intersects_page(write)) {
        continue;
      }
      output << "sequence=" << write.sequence << " kind=" << write.kind
             << " source=" << write.source_x << ',' << write.source_y
             << " destination=" << write.destination_x << ','
             << write.destination_y << " size=" << write.width << 'x'
             << write.height << '\n';
    }
    output << "\ncompanions:\nfull_vram=" << vram_path.filename().string()
           << "\nphysical_page=" << page_path.filename().string()
           << "\ndecoded_preview=" << preview_path.filename().string() << '\n';
    output.close();
    PsyX_Log_Info("[SurfacePicker] dump=%s\n", report_path.string().c_str());
  } catch (const std::exception &error) {
    PsyX_Log_Error("[SurfacePicker] dump failed: %s\n", error.what());
  }
}

} // namespace sf::platform::detail
