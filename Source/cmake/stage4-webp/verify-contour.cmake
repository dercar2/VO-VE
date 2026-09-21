cmake_minimum_required(VERSION 3.25)

foreach(required_variable
    WEBP_CACHE WEBP_BUILD_DIR PREFIX BUILD_ROOT ARCHIVE_DIR ARCHIVE
    ARCHIVE_SHA256 PATCH PATCH_SHA256 MAX_INSTALLED_KIB NM)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

function(expect_cache cache_file key expected)
  file(STRINGS "${cache_file}" cache_line REGEX "^${key}:[^=]*=" LIMIT_COUNT 1)
  if(NOT cache_line)
    message(FATAL_ERROR "${key} is absent from ${cache_file}")
  endif()
  string(REGEX REPLACE "^[^=]*=" "" actual "${cache_line}")
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
      "Unexpected ${key} in ${cache_file}: expected ${expected}, got ${actual}")
  endif()
endfunction()

file(SHA256 "${ARCHIVE}" actual_archive_sha256)
if(NOT actual_archive_sha256 STREQUAL ARCHIVE_SHA256)
  message(FATAL_ERROR "Pinned libwebp archive hash changed")
endif()
file(SHA256 "${PATCH}" actual_patch_sha256)
if(NOT actual_patch_sha256 STREQUAL PATCH_SHA256)
  message(FATAL_ERROR "Pinned libwebp decode-only patch hash changed")
endif()

foreach(entry
    BUILD_SHARED_LIBS=OFF
    WEBP_LINK_STATIC=ON
    WEBP_ENABLE_SIMD=ON
    WEBP_USE_THREAD=OFF
    WEBP_NEAR_LOSSLESS=OFF
    WEBP_BUILD_ANIM_UTILS=OFF
    WEBP_BUILD_CWEBP=OFF
    WEBP_BUILD_DWEBP=OFF
    WEBP_BUILD_GIF2WEBP=OFF
    WEBP_BUILD_IMG2WEBP=OFF
    WEBP_BUILD_VWEBP=OFF
    WEBP_BUILD_WEBPINFO=OFF
    WEBP_BUILD_LIBWEBPMUX=OFF
    WEBP_BUILD_WEBPMUX=OFF
    WEBP_BUILD_EXTRAS=OFF
    WEBP_BUILD_WEBP_JS=OFF
    WEBP_BUILD_FUZZTEST=OFF
    WEBP_ENABLE_SWAP_16BIT_CSP=OFF
    WEBP_ENABLE_WUNUSED_RESULT=OFF)
  string(REPLACE "=" ";" pair "${entry}")
  list(GET pair 0 key)
  list(GET pair 1 value)
  expect_cache("${WEBP_CACHE}" "${key}" "${value}")
endforeach()

set(expected_files
  "include/webp/decode.h"
  "include/webp/demux.h"
  "include/webp/mux_types.h"
  "include/webp/types.h"
  "lib/cmake/vove-webp-decode/vove-webp-decode-config.cmake"
  "lib/libwebp.a"
  "lib/libwebpdemux.a"
  "vove-webp-decode-manifest.txt")
file(GLOB_RECURSE installed_files RELATIVE "${PREFIX}"
  LIST_DIRECTORIES FALSE "${PREFIX}/*")
list(SORT expected_files)
list(SORT installed_files)
if(NOT installed_files STREQUAL expected_files)
  message(FATAL_ERROR
    "Installed WebP contour is not minimal.\n"
    "  expected: ${expected_files}\n"
    "  actual:   ${installed_files}")
endif()

set(total_bytes 0)
file(TO_CMAKE_PATH "${BUILD_ROOT}" normalized_build_root)
file(TO_CMAKE_PATH "${ARCHIVE_DIR}" normalized_archive_dir)
foreach(relative_file IN LISTS installed_files)
  set(installed_file "${PREFIX}/${relative_file}")
  file(SIZE "${installed_file}" file_bytes)
  math(EXPR total_bytes "${total_bytes} + ${file_bytes}")

  if(relative_file MATCHES "\\.(dll|exe|dylib|so|lib|pdb|pc)$")
    message(FATAL_ERROR "Forbidden package artifact: ${relative_file}")
  endif()

  # Release archives can still leak __FILE__ strings. Scan both metadata and
  # static archives so relocation is not merely a CMake-config property.
  file(STRINGS "${installed_file}" embedded_strings)
  string(JOIN "\n" embedded_text ${embedded_strings})
  string(FIND "${embedded_text}" "${normalized_build_root}" build_position)
  string(FIND "${embedded_text}" "${normalized_archive_dir}" archive_position)
  if(NOT build_position EQUAL -1 OR NOT archive_position EQUAL -1)
    message(FATAL_ERROR "Absolute workspace path leaked into ${relative_file}")
  endif()
endforeach()

math(EXPR maximum_bytes "${MAX_INSTALLED_KIB} * 1024")
if(total_bytes GREATER maximum_bytes)
  message(FATAL_ERROR
    "Installed contour is ${total_bytes} bytes; limit is ${maximum_bytes} bytes")
endif()

foreach(forbidden_file
    "${PREFIX}/include/webp/encode.h"
    "${PREFIX}/include/webp/mux.h"
    "${WEBP_BUILD_DIR}/libwebp.a"
    "${WEBP_BUILD_DIR}/libwebpmux.a"
    "${WEBP_BUILD_DIR}/libsharpyuv.a")
  if(EXISTS "${forbidden_file}")
    message(FATAL_ERROR "Encoder or mux build product exists: ${forbidden_file}")
  endif()
endforeach()

file(GLOB_RECURSE compiled_objects LIST_DIRECTORIES FALSE
  "${WEBP_BUILD_DIR}/*.o" "${WEBP_BUILD_DIR}/*.obj")
foreach(compiled_object IN LISTS compiled_objects)
  if(compiled_object MATCHES
      "[/\\\\](webpencode|webpdsp|webputils)\\.dir[/\\\\]"
      OR compiled_object MATCHES "[/\\\\]src[/\\\\]enc[/\\\\]")
    message(FATAL_ERROR "Encoder object was compiled: ${compiled_object}")
  endif()
endforeach()

execute_process(
  COMMAND "${NM}" -g --defined-only
    "${PREFIX}/lib/libwebp.a" "${PREFIX}/lib/libwebpdemux.a"
  RESULT_VARIABLE nm_result
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "Unable to inspect WebP archives: ${nm_error}")
endif()
foreach(required_symbol WebPDecodeRGBA WebPGetInfo WebPDemuxInternal WebPGetDemuxVersion)
  string(FIND "${symbols}" "${required_symbol}" symbol_position)
  if(symbol_position EQUAL -1)
    message(FATAL_ERROR "Required decode/demux symbol is missing: ${required_symbol}")
  endif()
endforeach()
foreach(forbidden_symbol
    WebPEncode WebPConfigInitInternal WebPValidateConfig WebPPictureInitInternal
    WebPAnimEncoder WebPMux)
  string(FIND "${symbols}" "${forbidden_symbol}" symbol_position)
  if(NOT symbol_position EQUAL -1)
    message(FATAL_ERROR "Encoder or mux symbol survived: ${forbidden_symbol}")
  endif()
endforeach()

math(EXPR total_kib "(${total_bytes} + 1023) / 1024")
message(STATUS
  "WebP decode-only contour verified: ${total_bytes} bytes (${total_kib} KiB), "
  "static decoder + demux only")
