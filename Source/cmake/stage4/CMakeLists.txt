cmake_minimum_required(VERSION 3.25)

project(vove_heic_decode_dependencies LANGUAGES C CXX)

include(ExternalProject)

set(VOVE_HEIC_ARCHIVE_DIR "" CACHE PATH
    "Directory containing the pinned decoder source archives")
get_filename_component(_work_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VOVE_HEIC_DAV1D_ARCHIVE "${VOVE_HEIC_ARCHIVE_DIR}/dav1d-1.5.4.tar.xz" CACHE FILEPATH
    "Pinned local dav1d release archive")
set(VOVE_HEIC_PYTHON "" CACHE FILEPATH "Existing build-local Python executable")
set(VOVE_HEIC_MESON_MODULE_DIR
    "${_work_root}/.tools/avif-build/python-packages/meson-1.9.2" CACHE PATH
    "Existing build-local Meson 1.9.2 module directory; never installed by this build")
set(VOVE_HEIC_NASM "${_work_root}/.tools/avif-build/nasm/nasm-3.02/nasm.exe" CACHE FILEPATH
    "Existing build-local NASM executable")
set(VOVE_HEIC_BUILD_JOBS "3" CACHE STRING "Maximum parallel jobs in each dependency build")
if(NOT VOVE_HEIC_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "VOVE_HEIC_BUILD_JOBS must be a positive integer")
endif()
set(VOVE_HEIC_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH
    "Destination for the relocatable HEIC decode-only contour")
set(VOVE_HEIC_MAX_INSTALLED_MIB "13" CACHE STRING
    "Hard maximum size of the installed contour, in MiB")
set(_de265_install_prefix "${CMAKE_BINARY_DIR}/libde265-install")
set(_dav1d_install_prefix "${CMAKE_BINARY_DIR}/dav1d-install")
set(_dav1d_sha256 "686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5")

set(_de265_archive "${VOVE_HEIC_ARCHIVE_DIR}/libde265-1.1.1.tar.gz")
set(_de265_sha256 "fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219")
set(_heif_archive "${VOVE_HEIC_ARCHIVE_DIR}/libheif-1.23.1.tar.gz")
set(_heif_sha256 "0de0327f60fcd47de90d5654c6fe152232738d60d84fe084ec3e0f35e03b166a")
set(_heif_patch "${CMAKE_CURRENT_SOURCE_DIR}/patch-libheif-decode-only.cmake")
set(_heif_patch_sha256 "6e60dc1fb78d222263504fe44ed23c3798cdf54ef1167caa4363228275cee5dc")

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

vove_require_file("${_de265_archive}" "${_de265_sha256}")
vove_require_file("${_heif_archive}" "${_heif_sha256}")
vove_require_file("${_heif_patch}" "${_heif_patch_sha256}")
vove_require_file("${VOVE_HEIC_DAV1D_ARCHIVE}" "${_dav1d_sha256}")

if(NOT CMAKE_GENERATOR STREQUAL "Ninja" OR
    NOT CMAKE_HOST_SYSTEM_NAME STREQUAL CMAKE_SYSTEM_NAME)
  message(FATAL_ERROR "This contour requires a native-host Ninja build")
endif()
foreach(required_tool "${VOVE_HEIC_PYTHON}" "${VOVE_HEIC_NASM}"
    "${VOVE_HEIC_MESON_MODULE_DIR}/mesonbuild/mesonmain.py" "${CMAKE_MAKE_PROGRAM}")
  if(NOT EXISTS "${required_tool}")
    message(FATAL_ERROR "Required build-local tool is missing: ${required_tool}")
  endif()
endforeach()
configure_file(dav1d-meson-driver.py.in "${CMAKE_BINARY_DIR}/dav1d-meson-driver.py" @ONLY)
configure_file(dav1d-native.ini.in "${CMAKE_BINARY_DIR}/dav1d-native.ini" @ONLY)
set(_meson "${VOVE_HEIC_PYTHON}" -B "${CMAKE_BINARY_DIR}/dav1d-meson-driver.py")
execute_process(COMMAND ${_meson} --version RESULT_VARIABLE _meson_result
  OUTPUT_VARIABLE _meson_version OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _meson_result EQUAL 0 OR NOT _meson_version STREQUAL "1.9.2")
  message(FATAL_ERROR "Build-local Meson 1.9.2 is required (got ${_meson_version})")
endif()

set(_common_cmake_args
  "-DCMAKE_BUILD_TYPE=Release"
  "-DCMAKE_INSTALL_LIBDIR=lib"
  "-DCMAKE_INSTALL_INCLUDEDIR=include"
  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"
  "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF"
  "-DCMAKE_INSTALL_MESSAGE=LAZY"
  "-DBUILD_SHARED_LIBS=OFF")
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
  list(APPEND _common_cmake_args
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
  list(APPEND _smoke_cmake_args
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()
if(CMAKE_MAKE_PROGRAM)
  list(APPEND _common_cmake_args
    "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
  list(APPEND _smoke_cmake_args
    "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
endif()

ExternalProject_Add(vove_libde265
  URL "${_de265_archive}"
  URL_HASH "SHA256=${_de265_sha256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  SOURCE_DIR "${CMAKE_BINARY_DIR}/source/libde265"
  BINARY_DIR "${CMAKE_BINARY_DIR}/build/libde265"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS
    ${_common_cmake_args}
    "-DCMAKE_INSTALL_PREFIX=${_de265_install_prefix}"
    "-DENABLE_SDL=OFF"
    "-DENABLE_DECODER=OFF"
    "-DENABLE_ENCODER=OFF"
    "-DENABLE_SHERLOCK265=OFF"
    "-DENABLE_INTERNAL_DEVELOPMENT_TOOLS=OFF"
    "-DWITH_FUZZERS=OFF"
    "-DUSE_IWYU=OFF"
    "-DFORCE_FULL_VISIBILITY=OFF"
    "-DDE265_LOG_LEVEL=error"
  BUILD_COMMAND
    "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target de265
      --parallel "${VOVE_HEIC_BUILD_JOBS}"
  INSTALL_COMMAND
    "${CMAKE_COMMAND}" --install <BINARY_DIR> --config Release
  USES_TERMINAL_CONFIGURE TRUE
  USES_TERMINAL_BUILD TRUE
  USES_TERMINAL_INSTALL TRUE)

ExternalProject_Add(vove_dav1d
  # Serialize nested builds so their per-project job limits do not multiply.
  DEPENDS vove_libde265
  URL "${VOVE_HEIC_DAV1D_ARCHIVE}"
  URL_HASH "SHA256=${_dav1d_sha256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  SOURCE_DIR "${CMAKE_BINARY_DIR}/source/dav1d"
  BINARY_DIR "${CMAKE_BINARY_DIR}/build/dav1d"
  CONFIGURE_COMMAND ${_meson} setup --reconfigure <BINARY_DIR> <SOURCE_DIR>
    --native-file "${CMAKE_BINARY_DIR}/dav1d-native.ini"
    --prefix "${_dav1d_install_prefix}" --libdir lib --backend=ninja
    --wrap-mode=nodownload --buildtype=release --default-library=static
    -Db_lto=false -Db_ndebug=true -Dbitdepths=8,16 -Denable_asm=true
    -Denable_tools=false -Denable_tests=false -Denable_examples=false -Denable_docs=false
    -Denable_seek_stress=false -Dtestdata_tests=false -Dfuzzing_engine=none
    -Dxxhash_muxer=disabled -Dtrim_dsp=true
  BUILD_COMMAND "${CMAKE_MAKE_PROGRAM}" -C <BINARY_DIR> -j "${VOVE_HEIC_BUILD_JOBS}"
  INSTALL_COMMAND ${_meson} install -C <BINARY_DIR> --no-rebuild
  USES_TERMINAL_CONFIGURE TRUE
  USES_TERMINAL_BUILD TRUE
  USES_TERMINAL_INSTALL TRUE)

ExternalProject_Add(vove_libheif
  DEPENDS vove_libde265 vove_dav1d
  URL "${_heif_archive}"
  URL_HASH "SHA256=${_heif_sha256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  PATCH_COMMAND
    "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>"
      -P "${_heif_patch}"
  SOURCE_DIR "${CMAKE_BINARY_DIR}/source/libheif"
  BINARY_DIR "${CMAKE_BINARY_DIR}/build/libheif"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS
    ${_common_cmake_args}
    "-DCMAKE_INSTALL_PREFIX=${VOVE_HEIC_INSTALL_PREFIX}"
    "-DENABLE_PLUGIN_LOADING=OFF"
    "-DWITH_LIBDE265=ON"
    "-DWITH_LIBDE265_PLUGIN=OFF"
    "-DLIBDE265_INCLUDE_DIR=${_de265_install_prefix}/include"
    "-DLIBDE265_LIBRARY=${_de265_install_prefix}/lib/libde265.a"
    "-DCMAKE_CXX_FLAGS=-DLIBDE265_STATIC_BUILD"
    "-DWITH_X265=OFF"
    "-DWITH_X265_PLUGIN=OFF"
    "-DWITH_KVAZAAR=OFF"
    "-DWITH_KVAZAAR_PLUGIN=OFF"
    "-DWITH_UVG266=OFF"
    "-DWITH_UVG266_PLUGIN=OFF"
    "-DWITH_VVDEC=OFF"
    "-DWITH_VVDEC_PLUGIN=OFF"
    "-DWITH_VVENC=OFF"
    "-DWITH_VVENC_PLUGIN=OFF"
    "-DWITH_X264=OFF"
    "-DWITH_X264_PLUGIN=OFF"
    "-DWITH_OpenH264_DECODER=OFF"
    "-DWITH_OpenH264_DECODER_PLUGIN=OFF"
    "-DWITH_OpenH264_ENCODER=OFF"
    "-DWITH_DAV1D=ON"
    "-DWITH_DAV1D_PLUGIN=OFF"
    "-DDAV1D_INCLUDE_DIR=${_dav1d_install_prefix}/include"
    "-DDAV1D_LIBRARY=${_dav1d_install_prefix}/lib/libdav1d.a"
    "-DWITH_AOM_DECODER=OFF"
    "-DWITH_AOM_DECODER_PLUGIN=OFF"
    "-DWITH_AOM_ENCODER=OFF"
    "-DWITH_AOM_ENCODER_PLUGIN=OFF"
    "-DWITH_SvtEnc=OFF"
    "-DWITH_SvtEnc_PLUGIN=OFF"
    "-DWITH_RAV1E=OFF"
    "-DWITH_RAV1E_PLUGIN=OFF"
    "-DWITH_JPEG_DECODER=OFF"
    "-DWITH_JPEG_DECODER_PLUGIN=OFF"
    "-DWITH_JPEG_ENCODER=OFF"
    "-DWITH_JPEG_ENCODER_PLUGIN=OFF"
    "-DWITH_OpenJPEG_DECODER=OFF"
    "-DWITH_OpenJPEG_DECODER_PLUGIN=OFF"
    "-DWITH_OpenJPEG_ENCODER=OFF"
    "-DWITH_OpenJPEG_ENCODER_PLUGIN=OFF"
    "-DWITH_OPENJPH_ENCODER=OFF"
    "-DWITH_OPENJPH_ENCODER_PLUGIN=OFF"
    "-DWITH_FFMPEG_DECODER=OFF"
    "-DWITH_FFMPEG_DECODER_PLUGIN=OFF"
    "-DWITH_UNCOMPRESSED_CODEC=OFF"
    "-DWITH_WEBCODECS=OFF"
    "-DWITH_LIBSHARPYUV=OFF"
    "-DWITH_LIBSHARPYUV_INTERNAL=OFF"
    "-DWITH_HEADER_COMPRESSION=OFF"
    "-DWITH_EXAMPLES=OFF"
    "-DWITH_EXAMPLE_HEIF_THUMB=OFF"
    "-DWITH_EXAMPLE_HEIF_VIEW=OFF"
    "-DWITH_GDK_PIXBUF=OFF"
    "-DWITH_LIBPNG_INTERNAL=OFF"
    "-DBUILD_DEVELOPMENT_TOOLS=OFF"
    "-DBUILD_DOCUMENTATION=OFF"
    "-DBUILD_TESTING=OFF"
    "-DWITH_FUZZERS=OFF"
    "-DENABLE_COVERAGE=OFF"
    "-DENABLE_MULTITHREADING_SUPPORT=OFF"
    "-DENABLE_PARALLEL_TILE_DECODING=OFF"
    "-DENABLE_EXPERIMENTAL_FEATURES=OFF"
    "-DCMAKE_DISABLE_FIND_PACKAGE_TIFF=ON"
    "-DCMAKE_DISABLE_FIND_PACKAGE_JPEG=ON"
    "-DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON"
    "-DCMAKE_DISABLE_FIND_PACKAGE_WEBP=ON"
  BUILD_COMMAND
    "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target heif
      --parallel "${VOVE_HEIC_BUILD_JOBS}"
  INSTALL_COMMAND
    "${CMAKE_COMMAND}" --install <BINARY_DIR> --config Release
  USES_TERMINAL_CONFIGURE TRUE
  USES_TERMINAL_BUILD TRUE
  USES_TERMINAL_INSTALL TRUE)

add_custom_target(vove_heic_decode_finalize
  COMMAND "${CMAKE_COMMAND}"
    "-DPREFIX=${VOVE_HEIC_INSTALL_PREFIX}"
    "-DDE265_PREFIX=${_de265_install_prefix}"
    "-DDAV1D_PREFIX=${_dav1d_install_prefix}"
    "-DPATCH_SHA256=${_heif_patch_sha256}"
    "-DCONFIG_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/vove-heic-decode-config.cmake"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/finalize-install.cmake"
  DEPENDS vove_libheif
  VERBATIM)

set(_smoke_runtime_dir "")
if(VOVE_LLVM_MINGW_ROOT)
  set(_smoke_runtime_dir "${VOVE_LLVM_MINGW_ROOT}/bin")
endif()

ExternalProject_Add(vove_heic_decode_smoke
  DEPENDS vove_heic_decode_finalize
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/smoke"
  BINARY_DIR "${CMAKE_BINARY_DIR}/build/smoke"
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND ""
  PATCH_COMMAND ""
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS
    ${_smoke_cmake_args}
    "-DCMAKE_PREFIX_PATH=${VOVE_HEIC_INSTALL_PREFIX}"
    "-Dvove-heic-decode_DIR=${VOVE_HEIC_INSTALL_PREFIX}/lib/cmake/vove-heic-decode"
  BUILD_COMMAND
    "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
      --parallel "${VOVE_HEIC_BUILD_JOBS}"
  BUILD_ALWAYS TRUE
  INSTALL_COMMAND ""
  TEST_COMMAND
    "${CMAKE_COMMAND}"
      "-DEXECUTABLE=<BINARY_DIR>/vove-heic-decode-smoke${CMAKE_EXECUTABLE_SUFFIX}"
      "-DRUNTIME_DIR=${_smoke_runtime_dir}"
      "-DSAMPLE=${CMAKE_BINARY_DIR}/source/libheif/examples/example.heic"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/run-smoke.cmake"
  TEST_AFTER_INSTALL TRUE
  USES_TERMINAL_CONFIGURE TRUE
  USES_TERMINAL_BUILD TRUE
  USES_TERMINAL_TEST TRUE)

add_custom_target(vove-heic-decode-verify ALL
  COMMAND "${CMAKE_COMMAND}"
    "-DDE265_CACHE=${CMAKE_BINARY_DIR}/build/libde265/CMakeCache.txt"
    "-DHEIF_CACHE=${CMAKE_BINARY_DIR}/build/libheif/CMakeCache.txt"
    "-DDAV1D_OPTIONS=${CMAKE_BINARY_DIR}/build/dav1d/meson-info/intro-buildoptions.json"
    "-DDAV1D_ARCHIVE=${VOVE_HEIC_DAV1D_ARCHIVE}"
    "-DPREFIX=${VOVE_HEIC_INSTALL_PREFIX}"
    "-DBUILD_ROOT=${CMAKE_BINARY_DIR}"
    "-DARCHIVE_DIR=${VOVE_HEIC_ARCHIVE_DIR}"
    "-DMAX_INSTALLED_MIB=${VOVE_HEIC_MAX_INSTALLED_MIB}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/verify-contour.cmake"
  DEPENDS vove_heic_decode_smoke
  VERBATIM)
