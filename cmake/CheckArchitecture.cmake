if(NOT DEFINED SF_SOURCE_ROOT)
    message(FATAL_ERROR "SF_SOURCE_ROOT is required")
endif()

set(_sf_errors)

function(sf_check_forbidden_includes layer)
    set(options)
    set(one_value_args)
    set(multi_value_args PATHS FORBIDDEN)
    cmake_parse_arguments(PARSE_ARGV 1 arg
        "${options}" "${one_value_args}" "${multi_value_args}")

    set(files)
    foreach(path IN LISTS arg_PATHS)
        file(GLOB_RECURSE path_files
            "${SF_SOURCE_ROOT}/${path}/*.cpp"
            "${SF_SOURCE_ROOT}/${path}/*.inc"
            "${SF_SOURCE_ROOT}/${path}/*.hpp")
        list(APPEND files ${path_files})
    endforeach()

    list(REMOVE_DUPLICATES files)
    list(SORT files)
    foreach(file IN LISTS files)
        # Inspect preprocessor directives only. Searching the complete file
        # made comments, diagnostics and documentation snippets look like real
        # dependencies and produced false architecture failures.
        file(STRINGS "${file}" includes
            REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
        foreach(forbidden IN LISTS arg_FORBIDDEN)
            foreach(include IN LISTS includes)
                string(FIND "${include}" "${forbidden}" position)
                if(NOT position EQUAL -1)
                    file(RELATIVE_PATH relative "${SF_SOURCE_ROOT}" "${file}")
                    list(APPEND _sf_errors
                        "${layer}: ${relative} includes forbidden dependency '${forbidden}'")
                    break()
                endif()
            endforeach()
        endforeach()
    endforeach()
    set(_sf_errors "${_sf_errors}" PARENT_SCOPE)
endfunction()

set(_sf_third_party_presentation_headers
    "#include <PsyX/"
    "#include \"PsyX/"
    "#include <psx/libgpu.h"
    "#include <psx/libgte.h"
    "#include <SDL.h"
    "#include <GL/"
    "#include <AL/"
    "#include <glad.h")

sf_check_forbidden_includes(core
    PATHS src/core include/sf/core
    FORBIDDEN
        "sf/assets/" "sf/disc/" "sf/psx/" "sf/game/"
        "sf/media/" "sf/platform/"
        ${_sf_third_party_presentation_headers})

sf_check_forbidden_includes(assets
    PATHS src/assets include/sf/assets
    FORBIDDEN
        "sf/disc/" "sf/psx/" "sf/game/" "sf/media/" "sf/platform/"
        ${_sf_third_party_presentation_headers})

sf_check_forbidden_includes(disc
    PATHS src/disc include/sf/disc
    FORBIDDEN
        "sf/assets/" "sf/psx/" "sf/game/" "sf/media/" "sf/platform/"
        ${_sf_third_party_presentation_headers})

sf_check_forbidden_includes(psx
    PATHS src/psx include/sf/psx
    FORBIDDEN
        "sf/assets/" "sf/disc/" "sf/game/" "sf/media/" "sf/platform/"
        ${_sf_third_party_presentation_headers})

sf_check_forbidden_includes(game
    PATHS src/game include/sf/game
    FORBIDDEN
        "sf/media/" "sf/platform/"
        ${_sf_third_party_presentation_headers})

# Every project-owned translation unit must belong to an explicit CMake target.
file(READ "${SF_SOURCE_ROOT}/CMakeLists.txt" _sf_build_manifest)
file(GLOB_RECURSE _sf_cmake_modules "${SF_SOURCE_ROOT}/cmake/*.cmake")
foreach(module IN LISTS _sf_cmake_modules)
    file(READ "${module}" module_content)
    string(APPEND _sf_build_manifest "\n${module_content}")
endforeach()
file(GLOB_RECURSE _sf_translation_units
    "${SF_SOURCE_ROOT}/src/*.cpp"
    "${SF_SOURCE_ROOT}/src/*.inc"
    "${SF_SOURCE_ROOT}/apps/*.cpp"
    "${SF_SOURCE_ROOT}/tests/*.cpp")
foreach(file IN LISTS _sf_translation_units)
    file(RELATIVE_PATH relative "${SF_SOURCE_ROOT}" "${file}")
    string(REPLACE "\\" "/" relative "${relative}")
    string(FIND "${_sf_build_manifest}" "${relative}" position)
    if(position EQUAL -1)
        list(APPEND _sf_errors
            "targets: ${relative} is not assigned to a CMake target or source manifest")
    endif()
endforeach()

list(REMOVE_DUPLICATES _sf_errors)
list(SORT _sf_errors)

if(_sf_errors)
    list(JOIN _sf_errors "\n  - " details)
    message(FATAL_ERROR "Architecture check failed:\n  - ${details}")
endif()

message(STATUS "Architecture boundaries and target ownership are valid")
