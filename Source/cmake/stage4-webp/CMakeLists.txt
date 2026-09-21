cmake_minimum_required(VERSION 3.25)

project(vove_webp_decode_dependencies LANGUAGES C)

include(ExternalProject)

set(VOVE_WEBP_ARCHIVE_DIR "" CACHE PATH
    "Directory containing the pinned libwebp source archive")
set(VOVE_WEBP_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH
    "Destination for the relocatable WebP decode-only contour")
set(VOVE_WEBP_MAX_INSTALLED_KIB "768" CACHE STRING
    "Hard maximum size of the installed contour, in KiB")

set(_webp_archive "${VOVE_WEBP_ARCHIVE_DIR}/libwebp-1.6.0.tar.gz")
set(_webp_sha256
    "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564")
set(_webp_patch "${CMAKE_CURRENT_SOURCE_DIR}/patch-libwebp-decode-only.cmake")
set(_webp_patch_sha256
    "60b917eb01e1170e4b77d5ccee5c7f04c3d6ee5854be527d832109172aa09aea")

function(vove_require_file input_file expected_sha256)
  if(NOT EXISTS "${input_file}")
    message(FATAL_ERROR "Required local input is missing: ${input_file}")
  endif()

  file(SHA256 "${input_file}" actual_sha256)
  if(NOT actual_sha256 STREQUAL expected_sha256)
    message(FATAL_ERROR
      "SHA256 mismatch for ${input_file}\n"
      "  expected: ${expected_sha256}\n"
      "  actual:   ${actual_sha256}")
  endif()
endfunction()

vove_require_file("${_webp_archive}" "${_webp_sha256}")
vove_require_file("${_webp_patch}" "${_webp_patch_sha256}")

set(_common_cmake_args
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"
  "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"
  "-DCMAKE_INSTALL_MESSAGE=LAZY"
  "-DBUILD_SHARED_LIBS=OFF"
  "-DWEBP_LINK_STATIC=ON"
  "-DWEBP_ENABLE_SIMD=ON"
  "-DWEBP_USE_THREAD=OFF"
  "-DWEBP_NEAR_LOSSLESS=OFF"
  "-DWEBP_BUILD_ANIM_UTILS=OFF"
  "-DWEBP_BUILD_CWEBP=OFF"
  "-DWEBP_BUILD_DWEBP=OFF"
  "-DWEBP_BUILD_GIF2WEBP=OFF"
  "-DWEBP_BUILD_IMG2WEBP=OFF"
  "-DWEBP_BUILD_VWEBP=OFF"
  "-DWEBP_BUILD_WEBPINFO=OFF"
  "-DWEBP_BUILD_LIBWEBPMUX=OFF"
  "-DWEBP_BUILD_WEBPMUX=OFF"
  "-DWEBP_BUILD_EXTRAS=OFF"
  "-DWEBP_BUILD_WEBP_JS=OFF"
  "-DWEBP_BUILD_FUZZTEST=OFF"
  "-DWEBP_ENABLE_SWAP_16BIT_CSP=OFF"
  "-DWEBP_ENABLE_WUNUSED_RESULT=OFF")
set(_smoke_cmake_args
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"
  "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF")

if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND _common_cmake_args "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
  list(APPEND _smoke_cmake_args "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}")
  if(VOVE_LLVM_MINGW_ROOT)
    list(APPEND _common_cmake_args
      "-DVOVE_LLVM_MINGW_ROOT=${VOVE_LLVM_MINGW_ROOT}")
    list(APPEND _smoke_cmake_args
      "-DVOVE_LLVM_MINGW_ROOT=${VOVE_LLVM_MINGW_ROOT}")
  endif()
else()
  list(APPEND _common_cmake_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
  list(APPEND _smoke_cmake_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
endif()
if(CMAKE_MAKE_PROGRAM)
  list(APPEND _common_cmake_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
  list(APPEND _smoke_cmake_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
endif()

set(_webp_source_dir "${CMAKE_BINARY_DIR}/source/libwebp")
set(_webp_build_dir "${CMAKE_BINARY_DIR}/build/libwebp")

ExternalProject_Add(vove_libwebp
  URL "${_webp_archive}"
  URL_HASH "SHA256=${_webp_sha256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  PATCH_COMMAND
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>" -P "${_webp_patch}"
  SOURCE_DIR "${_webp_source_dir}"
  BINARY_DIR "${_webp_build_dir}"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_common_cmake_args}
  BUILD_COMMAND
    "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
      --target webpdecoder webpdemux
  INSTALL_COMMAND
    "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=<SOURCE_DIR>"
      "-DBINARY_DIR=<BINARY_DIR>"
      "-DPREFIX=${VOVE_WEBP_INSTALL_PREFIX}"
      "-DPATCH_SHA256=${_webp_patch_sha256}"
      "-DCONFIG_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/vove-webp-decode-config.cmake"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/finalize-install.cmake"
  USES_TERMINAL_CONFIGURE TRUE
  USES_TERMINAL_BUILD TRUE
  USES_TERMINAL_INSTALL TRUE)

set(_relocated_prefix "${CMAKE_BINARY_DIR}/relocated-install")
add_custom_target(vove_webp_decode_relocate
  COMMAND "${CMAKE_COMMAND}"
    "-DPREFIX=${VOVE_WEBP_INSTALL_PREFIX}"
    "-DDESTINATION=${_relocated_prefix}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/prepare-relocated-install.cmake"
  DEPENDS vove_libwebp
  VERBATIM)

set(_smoke_runtime_dir "")
if(VOVE_LLVM_MINGW_ROOT)
  set(_smoke_runtime_dir "${VOVE_LLVM_MINGW_ROOT}/bin")
endif()

ExternalProject_Add(vove_webp_decode_smoke
  DEPENDS vove_webp_decode_relocate
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/smoke"
  BINARY_DIR "${CMAKE_BINARY_DIR}/build/smoke"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS
    ${_smoke_cmake_args}
    "-DCMAKE_PREFIX_PATH=${_relocated_prefix}"
    "-Dvove-webp-decode_DIR=${_relocated_prefix}/lib/cmake/vove-webp-decode"
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
  BUILD_ALWAYS TRUE
  INSTALL_COMMAND ""
  TEST_COMMAND
    "${CMAKE_COMMAND}"
      "-DEXECUTABLE=<BINARY_DIR>/vove-webp-decode-smoke${CMAKE_EXECUTABLE_SUFFIX}"
      "-DRUNTIME_DIR=${_smoke_runtime_dir}"
      "-DSAMPLE=${_webp_source_dir}/examples/test.webp"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run-smoke.cmake"
  TEST_AFTER_INSTALL TRUE
  USES_TERMINAL_CONFIGURE TRUE
  USES_TERMINAL_BUILD TRUE
  USES_TERMINAL_TEST TRUE)

add_custom_target(vove-webp-decode-verify ALL
  COMMAND "${CMAKE_COMMAND}"
    "-DWEBP_CACHE=${_webp_build_dir}/CMakeCache.txt"
    "-DWEBP_BUILD_DIR=${_webp_build_dir}"
    "-DPREFIX=${VOVE_WEBP_INSTALL_PREFIX}"
    "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
    "-DARCHIVE_DIR=${VOVE_WEBP_ARCHIVE_DIR}"
    "-DARCHIVE=${_webp_archive}"
    "-DARCHIVE_SHA256=${_webp_sha256}"
    "-DPATCH=${_webp_patch}"
    "-DPATCH_SHA256=${_webp_patch_sha256}"
    "-DMAX_INSTALLED_KIB=${VOVE_WEBP_MAX_INSTALLED_KIB}"
    "-DNM=${CMAKE_NM}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/verify-contour.cmake"
  DEPENDS vove_webp_decode_smoke
  VERBATIM)
