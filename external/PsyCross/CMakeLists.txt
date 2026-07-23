set(PSYCROSS_SRCS_C
	src/render/glad.c
	src/util/timer.c
	src/psx/ABS.C
	src/psx/INLINE_C.C
	src/psx/LIBAPI.C
	src/psx/LIBCD.C
	src/psx/LIBETC.C
	src/psx/LIBGPU.C
	src/psx/LIBGTE.C
	src/psx/LIBMCRD.C
	src/psx/LIBPAD.C
	src/psx/LIBSN.C
	src/psx/LIBSPU.C
)

set(PSYCROSS_SRCS_CPP
	src/audio/PsyX_SPUAL.cpp
	src/gpu/PsyX_GPU.cpp
	src/gte/half_float.cpp
	src/gte/PsyX_GTE.cpp
	src/pad/PsyX_pad.cpp
	src/render/PsyX_render.cpp
	src/util/crash_handler.cpp
	src/PsyX_main.cpp
)

set_source_files_properties(${PSYCROSS_SRCS_C} PROPERTIES LANGUAGE C)
set_source_files_properties(${PSYCROSS_SRCS_CPP} PROPERTIES LANGUAGE CXX)
#set_source_files_properties( "src/gte/PsyX_GTE.cpp" PROPERTIES LANGUAGE C)

add_library(psycross_static STATIC ${PSYCROSS_SRCS_C} ${PSYCROSS_SRCS_CPP})
set_target_properties(psycross_static PROPERTIES OUTPUT_NAME "psycross")
target_include_directories(psycross_static PUBLIC "include" "include/psx")

find_package(SDL2 REQUIRED)
target_link_libraries(psycross_static ${SDL2_LIBRARIES})
target_include_directories(psycross_static PRIVATE ${SDL2_INCLUDE_DIRS})

find_package(OPENAL REQUIRED)
target_link_libraries(psycross_static OpenAL)
target_include_directories(psycross_static PRIVATE ${OpenAL_BINARY_DIR} ${OpenAL_SOURCE_DIR}/include)
