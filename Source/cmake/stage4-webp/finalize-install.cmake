cmake_minimum_required(VERSION 3.25)

foreach(required_variable SOURCE_DIR BINARY_DIR PREFIX PATCH_SHA256 CONFIG_TEMPLATE)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

set(decoder_archive "${BINARY_DIR}/libwebpdecoder.a")
set(demux_archive "${BINARY_DIR}/libwebpdemux.a")
foreach(required_file "${decoder_archive}" "${demux_archive}")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "Expected staged static library is missing: ${required_file}")
  endif()
endforeach()

file(REMOVE_RECURSE "${PREFIX}/include" "${PREFIX}/lib")
file(REMOVE "${PREFIX}/vove-webp-decode-manifest.txt")
file(MAKE_DIRECTORY "${PREFIX}/include/webp" "${PREFIX}/lib")

file(COPY_FILE "${decoder_archive}" "${PREFIX}/lib/libwebp.a"
  ONLY_IF_DIFFERENT)
file(COPY_FILE "${demux_archive}" "${PREFIX}/lib/libwebpdemux.a"
  ONLY_IF_DIFFERENT)

foreach(header decode.h demux.h mux_types.h types.h)
  file(COPY_FILE
    "${SOURCE_DIR}/src/webp/${header}"
    "${PREFIX}/include/webp/${header}"
    ONLY_IF_DIFFERENT)
endforeach()

set(config_dir "${PREFIX}/lib/cmake/vove-webp-decode")
file(MAKE_DIRECTORY "${config_dir}")
configure_file(
  "${CONFIG_TEMPLATE}"
  "${config_dir}/vove-webp-decode-config.cmake"
  COPYONLY)

file(WRITE "${PREFIX}/vove-webp-decode-manifest.txt"
  "contour=vove-webp-decode\n"
  "libwebp.version=1.6.0\n"
  "libwebp.sha256=e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564\n"
  "libwebp.decode_only_patch.sha256=${PATCH_SHA256}\n"
  "libraries=webpdecoder,webpdemux\n"
  "linkage=static\n"
  "simd=on\n"
  "threads=off\n"
  "encoders=off\n"
  "mux=off\n")
