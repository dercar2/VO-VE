cmake_minimum_required(VERSION 3.25)

foreach(required_variable
    DE265_CACHE HEIF_CACHE DAV1D_OPTIONS DAV1D_ARCHIVE
    PREFIX BUILD_ROOT ARCHIVE_DIR MAX_INSTALLED_MIB)
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

file(READ "${DAV1D_OPTIONS}" dav1d_options)
string(JSON dav1d_option_count LENGTH "${dav1d_options}")
math(EXPR dav1d_last_option "${dav1d_option_count} - 1")
function(expect_dav1d option expected)
  foreach(index RANGE 0 ${dav1d_last_option})
    string(JSON name GET "${dav1d_options}" ${index} name)
    if(name STREQUAL option)
      if(option STREQUAL "bitdepths")
        string(JSON count LENGTH "${dav1d_options}" ${index} value)
        string(JSON low GET "${dav1d_options}" ${index} value 0)
        string(JSON high GET "${dav1d_options}" ${index} value 1)
        set(actual "${count}:${low},${high}")
      else()
        string(JSON actual GET "${dav1d_options}" ${index} value)
      endif()
      if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Unexpected dav1d ${option}: expected ${expected}, got ${actual}")
      endif()
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "dav1d option is absent: ${option}")
endfunction()
foreach(entry
    default_library=static buildtype=release optimization=3 debug=OFF
    b_lto=OFF b_ndebug=true bitdepths=2:8,16 enable_asm=ON
    enable_tools=OFF enable_tests=OFF enable_examples=OFF enable_docs=OFF
    enable_seek_stress=OFF testdata_tests=OFF fuzzing_engine=none
    xxhash_muxer=disabled trim_dsp=true wrap_mode=nodownload)
  string(REPLACE "=" ";" pair "${entry}")
  list(GET pair 0 key)
  list(GET pair 1 value)
  expect_dav1d("${key}" "${value}")
endforeach()

foreach(entry
    BUILD_SHARED_LIBS=OFF
    ENABLE_SDL=OFF
    ENABLE_DECODER=OFF
    ENABLE_ENCODER=OFF
    ENABLE_SHERLOCK265=OFF
    ENABLE_INTERNAL_DEVELOPMENT_TOOLS=OFF
    WITH_FUZZERS=OFF
    USE_IWYU=OFF
    FORCE_FULL_VISIBILITY=OFF
    DE265_LOG_LEVEL=error)
  string(REPLACE "=" ";" pair "${entry}")
  list(GET pair 0 key)
  list(GET pair 1 value)
  expect_cache("${DE265_CACHE}" "${key}" "${value}")
endforeach()

foreach(entry
    BUILD_SHARED_LIBS=OFF
    ENABLE_PLUGIN_LOADING=OFF
    WITH_LIBDE265=ON
    WITH_LIBDE265_PLUGIN=OFF
    WITH_X265=OFF
    WITH_X265_PLUGIN=OFF
    WITH_KVAZAAR=OFF
    WITH_KVAZAAR_PLUGIN=OFF
    WITH_UVG266=OFF
    WITH_UVG266_PLUGIN=OFF
    WITH_VVDEC=OFF
    WITH_VVDEC_PLUGIN=OFF
    WITH_VVENC=OFF
    WITH_VVENC_PLUGIN=OFF
    WITH_X264=OFF
    WITH_X264_PLUGIN=OFF
    WITH_OpenH264_DECODER=OFF
    WITH_OpenH264_DECODER_PLUGIN=OFF
    WITH_OpenH264_ENCODER=OFF
    WITH_DAV1D=ON
    WITH_DAV1D_PLUGIN=OFF
    WITH_AOM_DECODER=OFF
    WITH_AOM_DECODER_PLUGIN=OFF
    WITH_AOM_ENCODER=OFF
    WITH_AOM_ENCODER_PLUGIN=OFF
    WITH_SvtEnc=OFF
    WITH_SvtEnc_PLUGIN=OFF
    WITH_RAV1E=OFF
    WITH_RAV1E_PLUGIN=OFF
    WITH_JPEG_DECODER=OFF
    WITH_JPEG_DECODER_PLUGIN=OFF
    WITH_JPEG_ENCODER=OFF
    WITH_JPEG_ENCODER_PLUGIN=OFF
    WITH_OpenJPEG_DECODER=OFF
    WITH_OpenJPEG_DECODER_PLUGIN=OFF
    WITH_OpenJPEG_ENCODER=OFF
    WITH_OpenJPEG_ENCODER_PLUGIN=OFF
    WITH_OPENJPH_ENCODER=OFF
    WITH_OPENJPH_ENCODER_PLUGIN=OFF
    WITH_FFMPEG_DECODER=OFF
    WITH_FFMPEG_DECODER_PLUGIN=OFF
    WITH_UNCOMPRESSED_CODEC=OFF
    WITH_WEBCODECS=OFF
    WITH_LIBSHARPYUV=OFF
    WITH_LIBSHARPYUV_INTERNAL=OFF
    WITH_HEADER_COMPRESSION=OFF
    WITH_EXAMPLES=OFF
    WITH_EXAMPLE_HEIF_THUMB=OFF
    WITH_EXAMPLE_HEIF_VIEW=OFF
    WITH_GDK_PIXBUF=OFF
    WITH_LIBPNG_INTERNAL=OFF
    BUILD_DEVELOPMENT_TOOLS=OFF
    BUILD_DOCUMENTATION=OFF
    BUILD_TESTING=OFF
    WITH_FUZZERS=OFF
    ENABLE_COVERAGE=OFF
    ENABLE_MULTITHREADING_SUPPORT=OFF
    ENABLE_PARALLEL_TILE_DECODING=OFF
    ENABLE_EXPERIMENTAL_FEATURES=OFF)
  string(REPLACE "=" ";" pair "${entry}")
  list(GET pair 0 key)
  list(GET pair 1 value)
  expect_cache("${HEIF_CACHE}" "${key}" "${value}")
endforeach()

file(STRINGS "${HEIF_CACHE}" enabled_codec_options
  REGEX "^((WITH_.*(ENCODER|DECODER|AOM|DAV1D|RAV1E|SvtEnc|X26|KVAZAAR|LIBDE265|LIBSHARPYUV|OPENJPH|OpenJPEG|JXL|UVG|VVDEC|VVENC|WEBCODECS|UNCOMPRESSED).*)|(ENABLE_PLUGIN_LOADING)):BOOL=ON$")
foreach(cache_line IN LISTS enabled_codec_options)
  if(NOT cache_line STREQUAL "WITH_LIBDE265:BOOL=ON" AND
      NOT cache_line STREQUAL "WITH_DAV1D:BOOL=ON")
    message(FATAL_ERROR "Unexpected enabled codec capability: ${cache_line}")
  endif()
endforeach()

foreach(required_file
    "${PREFIX}/lib/libde265.a"
    "${PREFIX}/lib/libdav1d.a"
    "${PREFIX}/lib/libheif.a"
    "${PREFIX}/include/libheif/heif.h"
    "${PREFIX}/lib/cmake/vove-heic-decode/vove-heic-decode-config.cmake"
    "${PREFIX}/vove-heic-decode-manifest.txt")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "Installed contour is missing ${required_file}")
  endif()
endforeach()

foreach(forbidden_path
    "${PREFIX}/include/libde265"
    "${PREFIX}/include/dav1d"
    "${PREFIX}/bin"
    "${PREFIX}/lib/cmake/libde265"
    "${PREFIX}/lib/cmake/libheif"
    "${PREFIX}/lib/pkgconfig"
    "${PREFIX}/share")
  if(EXISTS "${forbidden_path}")
    message(FATAL_ERROR "Non-relocatable upstream metadata survived: ${forbidden_path}")
  endif()
endforeach()

file(GLOB_RECURSE installed_files LIST_DIRECTORIES FALSE "${PREFIX}/*")
set(total_bytes 0)
foreach(installed_file IN LISTS installed_files)
  file(SIZE "${installed_file}" file_bytes)
  math(EXPR total_bytes "${total_bytes} + ${file_bytes}")

  if(installed_file MATCHES "\\.(dll|exe|dylib)$" OR installed_file MATCHES "\\.so(\\.|$)")
    message(FATAL_ERROR "Dynamic library or tool found in static contour: ${installed_file}")
  endif()

  if(installed_file MATCHES "[/\\\\]lib[/\\\\].*\\.a$"
      AND NOT installed_file MATCHES "[/\\\\]lib[/\\\\]lib(de265|dav1d|heif)\\.a$")
    message(FATAL_ERROR "Unexpected static archive found in contour: ${installed_file}")
  endif()
endforeach()

math(EXPR maximum_bytes "${MAX_INSTALLED_MIB} * 1024 * 1024")

file(GLOB_RECURSE metadata_files LIST_DIRECTORIES FALSE
  "${PREFIX}/*.cmake" "${PREFIX}/*.h" "${PREFIX}/*.pc" "${PREFIX}/*.txt"
  "${PREFIX}/*.json")
file(TO_CMAKE_PATH "${BUILD_ROOT}" normalized_build_root)
file(TO_CMAKE_PATH "${ARCHIVE_DIR}" normalized_archive_dir)
file(TO_CMAKE_PATH "${DAV1D_ARCHIVE}" normalized_dav1d_archive)
foreach(metadata_file IN LISTS metadata_files)
  file(READ "${metadata_file}" metadata)
  string(FIND "${metadata}" "${normalized_build_root}" build_root_position)
  string(FIND "${metadata}" "${normalized_archive_dir}" archive_dir_position)
  string(FIND "${metadata}" "${normalized_dav1d_archive}" dav1d_archive_position)
  if(NOT build_root_position EQUAL -1 OR NOT archive_dir_position EQUAL -1 OR
      NOT dav1d_archive_position EQUAL -1)
    message(FATAL_ERROR "Absolute build path leaked into ${metadata_file}")
  endif()
endforeach()

file(STRINGS "${PREFIX}/include/libheif/heif_version.h" plugin_directory_line
  REGEX "^#define LIBHEIF_PLUGIN_DIRECTORY")
if(NOT plugin_directory_line STREQUAL "#define LIBHEIF_PLUGIN_DIRECTORY \"\"")
  message(FATAL_ERROR "libheif plugin directory was not neutralized")
endif()

if(total_bytes GREATER maximum_bytes)
  message(FATAL_ERROR
    "Installed contour is ${total_bytes} bytes; limit is ${maximum_bytes} bytes")
endif()

math(EXPR total_kib "(${total_bytes} + 1023) / 1024")
message(STATUS
  "HEIC decode-only contour verified: ${total_bytes} bytes (${total_kib} KiB), "
  "libde265 + dav1d + libheif, HEVC/AV1 decode only")
