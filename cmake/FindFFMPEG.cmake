# Finds the FFmpeg libraries used by the STR decoder through pkg-config.
# This module is used on non-Windows hosts (Homebrew, Linux distributions);
# Windows builds keep using the vcpkg toolchain.
#
# Sets the variables consumed by the root CMakeLists.txt:
#   FFMPEG_INCLUDE_DIRS, FFMPEG_LIBRARY_DIRS, FFMPEG_LIBRARIES

find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswresample
    libswscale
)

# Prefer absolute library paths: sf_media is a static library, so its private
# link directories do not propagate to the executables that finally link FFmpeg.
if(FFMPEG_LINK_LIBRARIES)
    set(FFMPEG_LIBRARIES ${FFMPEG_LINK_LIBRARIES})
endif()
