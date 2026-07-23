# Compatibility module for PsyCross, which asks for an uppercase OPENAL
# package name. vcpkg exports the canonical OpenAL::OpenAL config target.
if(NOT TARGET OpenAL::OpenAL)
    find_package(OpenAL CONFIG REQUIRED)
endif()

set(OPENAL_FOUND TRUE)
set(OpenAL_FOUND TRUE)
