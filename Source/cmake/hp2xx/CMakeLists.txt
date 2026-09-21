cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vove_hpgl_standalone LANGUAGES C CXX)
    include(CTest)
endif()

get_filename_component(VOVE_HPGL_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(VOVE_HP2XX_ARCHIVE_DIR "${VOVE_HPGL_ROOT}/.tools/stage4-dist" CACHE PATH
    "Build-local source archive cache")
set(VOVE_HP2XX_SOURCE_ARCHIVE
    "${VOVE_HP2XX_ARCHIVE_DIR}/hp2xx-3.4.4.tar.gz"
    CACHE FILEPATH "Pinned official GNU hp2xx 3.4.4 source archive")
set(hp2xx_expected_hash "47b72fb386a189b52f07e31e424c038954c4e0ce405803841bed742bab488817")
if(NOT EXISTS "${VOVE_HP2XX_SOURCE_ARCHIVE}")
    get_filename_component(hp2xx_archive_parent "${VOVE_HP2XX_SOURCE_ARCHIVE}" DIRECTORY)
    file(MAKE_DIRECTORY "${hp2xx_archive_parent}")
    file(DOWNLOAD "https://ftp.gnu.org/gnu/hp2xx/hp2xx-3.4.4.tar.gz"
        "${VOVE_HP2XX_SOURCE_ARCHIVE}"
        EXPECTED_HASH "SHA256=${hp2xx_expected_hash}" TLS_VERIFY ON
        TIMEOUT 90 STATUS hp2xx_download_status)
    list(GET hp2xx_download_status 0 hp2xx_download_code)
    if(NOT hp2xx_download_code EQUAL 0)
        message(FATAL_ERROR "hp2xx download failed; supply VOVE_HP2XX_SOURCE_ARCHIVE for offline builds")
    endif()
endif()
file(SHA256 "${VOVE_HP2XX_SOURCE_ARCHIVE}" hp2xx_hash)
if(NOT hp2xx_hash STREQUAL hp2xx_expected_hash)
    message(FATAL_ERROR "hp2xx 3.4.4 source archive SHA-256 mismatch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/patch.cmake")
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/patch.cmake" hp2xx_patch_hash)
set(hp2xx_tree "${CMAKE_CURRENT_BINARY_DIR}/upstream/${hp2xx_patch_hash}")
set(hp2xx_source "${hp2xx_tree}/hp2xx-3.4.4/sources")
if(NOT EXISTS "${hp2xx_tree}/.patched")
    file(MAKE_DIRECTORY "${hp2xx_tree}")
    file(ARCHIVE_EXTRACT INPUT "${VOVE_HP2XX_SOURCE_ARCHIVE}" DESTINATION "${hp2xx_tree}")
    include("${CMAKE_CURRENT_LIST_DIR}/patch.cmake")
    vove_patch_hp2xx("${hp2xx_source}")
    file(WRITE "${hp2xx_tree}/.patched" "${hp2xx_hash}\n${hp2xx_patch_hash}\n")
endif()

set(hpgl_bridge "${VOVE_HPGL_ROOT}/src/handlers/hpgl")
add_library(vove-hp2xx-parser STATIC
    "${hpgl_bridge}/parser_bridge.c"
    "${hp2xx_source}/chardraw.c" "${hp2xx_source}/clip.c"
    "${hp2xx_source}/fillpoly.c" "${hp2xx_source}/pendef.c" "${hp2xx_source}/lindef.c")
target_include_directories(vove-hp2xx-parser PRIVATE "${hp2xx_source}" "${hpgl_bridge}")
target_compile_definitions(vove-hp2xx-parser PRIVATE UNIX _GNU_SOURCE)
set_target_properties(vove-hp2xx-parser PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vove-hp2xx-parser PRIVATE
        -include "${hpgl_bridge}/memory_stdio.h" -ffunction-sections -fdata-sections)
else()
    message(FATAL_ERROR "The bounded hp2xx contour requires GCC or llvm-mingw/Clang")
endif()
if(NOT WIN32)
    target_link_libraries(vove-hp2xx-parser PRIVATE m)
endif()

option(VOVE_HP2XX_BUILD_PROBE "Build a local-fixture parser diagnostic executable" OFF)
if(VOVE_HP2XX_BUILD_PROBE)
    target_compile_definitions(vove-hp2xx-parser PRIVATE VOVE_HPGL_PROBE)
    add_executable(vove-hp2xx-parser-probe "${CMAKE_CURRENT_LIST_DIR}/parser_probe.c")
    target_include_directories(vove-hp2xx-parser-probe PRIVATE "${hpgl_bridge}")
    target_link_libraries(vove-hp2xx-parser-probe PRIVATE vove-hp2xx-parser)
    if(WIN32)
        target_link_options(vove-hp2xx-parser-probe PRIVATE -static)
    endif()
endif()

# Useful for the first compiler checkpoint without the C++ integration target.
option(VOVE_HP2XX_PARSER_ONLY "Build just the bounded C parser adapter" OFF)
if(VOVE_HP2XX_PARSER_ONLY)
    return()
endif()

add_library(vove-hpgl-preview STATIC "${hpgl_bridge}/preview.cpp")
add_library(vove::hpgl_preview ALIAS vove-hpgl-preview)
target_include_directories(vove-hpgl-preview PUBLIC "${VOVE_HPGL_ROOT}/include")
target_link_libraries(vove-hpgl-preview PRIVATE vove-hp2xx-parser)
target_compile_features(vove-hpgl-preview PUBLIC cxx_std_20)
if(COMMAND vove_enable_warnings)
    vove_enable_warnings(vove-hpgl-preview)
endif()

# Preserve the source archive's complete GPL-2.0-or-later license beside the build.
configure_file("${hp2xx_tree}/hp2xx-3.4.4/copying"
               "${CMAKE_CURRENT_BINARY_DIR}/hp2xx-COPYING" COPYONLY)
if(BUILD_TESTING)
    add_executable(vove-hpgl-preview-tests "${VOVE_HPGL_ROOT}/tests/handlers/hpgl_preview_test.cpp")
    target_link_libraries(vove-hpgl-preview-tests PRIVATE vove::hpgl_preview)
    if(COMMAND vove_enable_warnings)
        vove_enable_warnings(vove-hpgl-preview-tests)
    endif()
    if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(vove-hpgl-preview-tests PRIVATE -static)
    endif()
    add_test(NAME vove-hpgl-preview-tests COMMAND vove-hpgl-preview-tests)
endif()
