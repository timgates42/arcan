pkg_check_modules(FFMPEG QUIET libavcodec libavdevice libavfilter libavformat libavutil libswresample libswscale)

set(LIBRARIES
	pthread
	arcan_shmif
	arcan_shmif_server
)

# should be probed based on architecture, and then dynamically, need to dive
# into how that is done - but might as well take it for when we also add
# optimized chacha
set(DEFS
	BLAKE3_NO_AVX2
	BLAKE3_NO_AVX512
	BLAKE3_NO_SSE41
)

set(A12_VERSION_MAJOR 0)
set(A12_VERSION_MINOR 1)

if (FFMPEG_FOUND)
	amsg("(a12) ${CL_GRN} ffmpeg support found, H264 enc/dec enabled ${CL_RST}")
	add_definitions(-DWANT_H264_DEC)
	add_definitions(-DWANT_H264_ENC)

	list(APPEND LIBRARIES ${FFMPEG_LINK_LIBRARIES})
	include_directories(${FFMPEG_INCLUDE_DIRS})
else()
	amsg("(a12) ${CL_YEL} ffmpeg support NOT found, video enc/dec DISABLED ${CL_RST}")
endif()

set(A12_SOURCES
	a12.c
	a12_decode.c
	a12_encode.c
	${PLATFORM_ROOT}/posix/mem.c
	${PLATFORM_ROOT}/posix/base64.c
	${PLATFORM_ROOT}/posix/random.c
)

set(EXTERNAL_SOURCES
	external/blake3/blake3.c
	external/blake3/blake3_dispatch.c
	external/blake3/blake3_portable.c
	external/miniz/miniz.c
	external/x25519.c
)

add_library(arcan_a12 SHARED ${A12_SOURCES} ${EXTERNAL_SOURCES})
target_compile_definitions(arcan_a12 PRIVATE ${DEFS})

set_target_properties(arcan_a12 PROPERTIES
	COMPILE_FLAGS -fPIC
	VERSION ${A12_VERSION_MAJOR}.${A12_VERSION_MINOR}
)

target_include_directories(arcan_a12 PRIVATE
	external/blake3
	external
	${PLATFORM_ROOT}/../engine
	${PLATFORM_ROOT}/../shmif
)

target_link_libraries(arcan_a12 ${LIBRARIES})

set(A12_LIBRARIES arcan_a12 PARENT_SCOPE)
install(TARGETS arcan_a12
	LIBRARY DESTINATION lib
	ARCHIVE DESTINATION lib
)
install(FILES a12.h DESTINATION include/arcan)
add_subdirectory("net")
