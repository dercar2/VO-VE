cmake_minimum_required(VERSION 3.25)
project(vove_jpegxl_dependencies LANGUAGES C CXX)

include(ExternalProject)
include("${CMAKE_CURRENT_SOURCE_DIR}/inputs.cmake")
get_filename_component(_work "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VOVE_JPEGXL_ARCHIVE_DIR "${CMAKE_BINARY_DIR}/downloads" CACHE PATH "Pinned local source archives")
set(VOVE_JPEGXL_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Decoder-only development prefix")
set(VOVE_JPEGXL_LCMS2_LIBRARY "${_work}/out/build/windows-verification/libvove-lcms2.a"
    CACHE FILEPATH "Existing VO-VE static LCMS2 archive, not rebuilt or copied")
set(VOVE_JPEGXL_LCMS2_INCLUDE_DIR "${_work}/third_party/lcms2/include"
    CACHE PATH "Existing VO-VE LCMS2 headers")
set(VOVE_JPEGXL_BUILD_JOBS 3 CACHE STRING "Maximum nested build parallelism")
if(NOT VOVE_JPEGXL_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "VOVE_JPEGXL_BUILD_JOBS must be positive")
endif()
if(NOT EXISTS "${VOVE_JPEGXL_LCMS2_LIBRARY}" OR
    NOT EXISTS "${VOVE_JPEGXL_LCMS2_INCLUDE_DIR}/lcms2.h")
  message(FATAL_ERROR "Provide the existing VO-VE static LCMS2 archive and headers")
endif()
file(SHA256 "${VOVE_JPEGXL_LCMS2_LIBRARY}" _lcms_sha256)

foreach(name IN LISTS VOVE_JXL_INPUTS)
  set(archive "${VOVE_JPEGXL_ARCHIVE_DIR}/${name}-${${name}_commit}.tar.gz")
  if(NOT EXISTS "${archive}")
    message(FATAL_ERROR "Missing local archive: ${archive}; run fetch-sources.cmake separately")
  endif()
  file(SHA256 "${archive}" actual)
  if(NOT actual STREQUAL "${${name}_sha256}")
    message(FATAL_ERROR "SHA256 mismatch: ${archive}")
  endif()
  set(${name}_source "${CMAKE_BINARY_DIR}/source/${name}-${${name}_commit}")
  if(NOT EXISTS "${${name}_source}/.vove-extracted" OR
      (name STREQUAL "libjxl" AND NOT EXISTS "${${name}_source}/tools/CMakeLists.txt"))
    if(name STREQUAL "libjxl")
      # Disabled benchmark tools contain symlinks which Windows cannot extract.
      set(root "${name}-${${name}_commit}")
      file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${CMAKE_BINARY_DIR}/source"
        PATTERNS "${root}/CMakeLists.txt" "${root}/LICENSE" "${root}/PATENTS"
          "${root}/AUTHORS" "${root}/cmake_uninstall.cmake.in"
          "${root}/cmake/*" "${root}/lib/*" "${root}/third_party/*"
          "${root}/tools/*.txt" "${root}/tools/*.cc" "${root}/tools/*.c"
          "${root}/tools/*.h" "${root}/tools/*.cmake")
    else()
      file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${CMAKE_BINARY_DIR}/source")
    endif()
    file(WRITE "${${name}_source}/.vove-extracted" "${actual}\n")
  endif()
endforeach()
foreach(name highway brotli)
  file(COPY "${${name}_source}/" DESTINATION "${libjxl_source}/third_party/${name}")
endforeach()

set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(PROVISION_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(JPEGXL_VERSION "0.12.0" CACHE STRING "Pinned source version" FORCE)
# Brotli's empty-main probe incorrectly succeeds with static try_compile targets.
if(NOT EMSCRIPTEN)
  set(BROTLI_EMSCRIPTEN OFF CACHE INTERNAL "Native build, not Emscripten" FORCE)
endif()
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)
foreach(option BOXES TRANSCODE_JPEG TOOLS DEVTOOLS BENCHMARK EXAMPLES FUZZERS JNI
    SJPEG OPENEXR SKCMS VIEWERS PLUGINS DOXYGEN MANPAGES TCMALLOC COVERAGE LTO)
  set(JPEGXL_ENABLE_${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL "" FORCE)
set(JPEGXL_TEST_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_FORCE_SYSTEM_LCMS2 ON CACHE BOOL "" FORCE)
set(JPEGXL_FORCE_SYSTEM_HWY OFF CACHE BOOL "" FORCE)
set(JPEGXL_FORCE_SYSTEM_BROTLI OFF CACHE BOOL "" FORCE)
set(LCMS2_LIBRARY "${VOVE_JPEGXL_LCMS2_LIBRARY}" CACHE FILEPATH "" FORCE)
set(LCMS2_INCLUDE_DIR "${VOVE_JPEGXL_LCMS2_INCLUDE_DIR}" CACHE PATH "" FORCE)
set(LCMS2_DEBUG_LIBRARY "" CACHE FILEPATH "" FORCE)
foreach(package PNG JPEG OpenEXR)
  set(CMAKE_DISABLE_FIND_PACKAGE_${package} ON)
endforeach()
add_compile_options(-ffunction-sections -fdata-sections)
add_subdirectory("${libjxl_source}" "${CMAKE_BINARY_DIR}/build/libjxl" EXCLUDE_FROM_ALL)

add_custom_target(vove-jpegxl-stage
  COMMAND "${CMAKE_COMMAND}"
    "-DPREFIX=${VOVE_JPEGXL_INSTALL_PREFIX}"
    "-DHEADERS=${CMAKE_BINARY_DIR}/build/libjxl/lib/include/jxl"
    "-DDECODER=$<TARGET_FILE:jxl_dec>" "-DCMS=$<TARGET_FILE:jxl_cms>"
    "-DHWY=$<TARGET_FILE:hwy>" "-DLCMS_SHA256=${_lcms_sha256}"
    "-DCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/vove-jpegxl-decode-config.cmake"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/stage.cmake"
  DEPENDS jxl_dec jxl_cms hwy
  VERBATIM)

set(_smoke_args "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
  "-Dvove-jpegxl-decode_DIR=${VOVE_JPEGXL_INSTALL_PREFIX}/lib/cmake/vove-jpegxl-decode"
  "-DVOVE_JPEGXL_LCMS2_LIBRARY=${VOVE_JPEGXL_LCMS2_LIBRARY}")
if(CMAKE_TOOLCHAIN_FILE)
  list(APPEND _smoke_args "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
    "-DVOVE_LLVM_MINGW_ROOT=${VOVE_LLVM_MINGW_ROOT}")
else()
  list(APPEND _smoke_args "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()
ExternalProject_Add(vove_jpegxl_smoke
  DEPENDS vove-jpegxl-stage
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/smoke"
  BINARY_DIR "${CMAKE_BINARY_DIR}/build/smoke"
  DOWNLOAD_COMMAND "" UPDATE_COMMAND "" PATCH_COMMAND ""
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_smoke_args}
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --parallel "${VOVE_JPEGXL_BUILD_JOBS}"
  BUILD_ALWAYS TRUE
  INSTALL_COMMAND ""
  TEST_COMMAND "<BINARY_DIR>/vove-jpegxl-smoke${CMAKE_EXECUTABLE_SUFFIX}"
  TEST_AFTER_INSTALL TRUE
  USES_TERMINAL_CONFIGURE TRUE USES_TERMINAL_BUILD TRUE USES_TERMINAL_TEST TRUE)

add_custom_target(vove-jpegxl-verify ALL
  COMMAND "${CMAKE_COMMAND}" "-DPREFIX=${VOVE_JPEGXL_INSTALL_PREFIX}"
    "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/verify.cmake"
  DEPENDS vove_jpegxl_smoke
  VERBATIM)
