// Minimal non-Windows implementation of the launcher interface.
//
// The Windows build provides a full Win32 launcher GUI in launcher.cpp.
// On macOS and Linux the game is configured through command-line options
// (see --help output in main.cpp); the graphical launcher is not shown.

#include "launcher.hpp"

#include <cstdio>
#include <filesystem>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <array>
#include <unistd.h>
#endif

namespace {

std::filesystem::path executableDirectory() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
    std::error_code error;
    const auto canonical =
        std::filesystem::weakly_canonical(buffer, error);
    if (!error && !canonical.empty()) {
      return canonical.parent_path();
    }
  }
#elif defined(__linux__)
  std::array<char, 4096> buffer{};
  const auto length =
      ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length > 0) {
    return std::filesystem::path{
        std::string_view(buffer.data(), static_cast<std::size_t>(length))}
        .parent_path();
  }
#endif
  return std::filesystem::current_path();
}

} // namespace

namespace sf::platform {

void loadLauncherSettings(GraphicsSettings & /*graphics*/,
                          KeyboardMouseBindings & /*input*/,
                          game::GameLanguage & /*language*/) noexcept {
  // No persisted launcher settings outside the Windows GUI launcher.
}

bool showGraphicsLauncher(GraphicsSettings & /*settings*/,
                          KeyboardMouseBindings & /*input*/,
                          game::GameLanguage & /*language*/,
                          std::filesystem::path & /*cue_path*/) {
  std::fprintf(stderr,
               "The graphical launcher is not available on this platform.\n"
               "Pass the game image directly, for example:\n"
               "  syphon_filter --no-launcher \"Syphon Filter (USA) "
               "(v1.1).cue\"\n");
  return false;
}

bool retailCheatMarkerExists() noexcept {
  std::error_code error;
  const auto marker = executableDirectory() / "syphon_filter_cheats";
  return std::filesystem::exists(marker, error) && !error;
}

void showLauncherError(std::string_view title,
                       std::string_view message) noexcept {
  std::fprintf(stderr, "%.*s: %.*s\n", static_cast<int>(title.size()),
               title.data(), static_cast<int>(message.size()),
               message.data());
}

} // namespace sf::platform
