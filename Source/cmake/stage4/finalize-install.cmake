cmake_minimum_required(VERSION 3.25)

foreach(required_variable PREFIX DE265_PREFIX DAV1D_PREFIX PATCH_SHA256 CONFIG_TEMPLATE)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

if(NOT EXISTS "${DE265_PREFIX}/lib/libde265.a" OR NOT EXISTS "${PREFIX}/lib/libheif.a"
    OR NOT EXISTS "${DAV1D_PREFIX}/lib/libdav1d.a")
  message(FATAL_ERROR "Expected staged static libraries are missing")
endif()

file(MAKE_DIRECTORY "${PREFIX}/lib")
file(COPY_FILE
  "${DE265_PREFIX}/lib/libde265.a"
  "${PREFIX}/lib/libde265.a"
  ONLY_IF_DIFFERENT)
file(COPY_FILE
  "${DAV1D_PREFIX}/lib/libdav1d.a"
  "${PREFIX}/lib/libdav1d.a"
  ONLY_IF_DIFFERENT)

# Upstream libheif serializes the discovered absolute decoder archive paths into
# its exported target. Remove all upstream discovery metadata and expose one
# small, relocatable config owned by VO-VE instead.
file(REMOVE_RECURSE
  "${PREFIX}/include/libde265"
  "${PREFIX}/lib/cmake/libde265"
  "${PREFIX}/lib/cmake/libheif"
  "${PREFIX}/lib/pkgconfig")

set(version_header "${PREFIX}/include/libheif/heif_version.h")
file(READ "${version_header}" version_header_contents)
string(REGEX REPLACE
  "#define LIBHEIF_PLUGIN_DIRECTORY \"[^\"]*\""
  "#define LIBHEIF_PLUGIN_DIRECTORY \"\""
  version_header_contents "${version_header_contents}")
file(WRITE "${version_header}" "${version_header_contents}")

set(config_dir "${PREFIX}/lib/cmake/vove-heic-decode")
file(MAKE_DIRECTORY "${config_dir}")
configure_file(
  "${CONFIG_TEMPLATE}"
  "${config_dir}/vove-heic-decode-config.cmake"
  COPYONLY)

file(WRITE "${PREFIX}/vove-heic-decode-manifest.txt"
  "contour=vove-heic-decode\n"
  "libde265.version=1.1.1\n"
  "libde265.sha256=fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219\n"
  "libheif.version=1.23.1\n"
  "libheif.sha256=0de0327f60fcd47de90d5654c6fe152232738d60d84fe084ec3e0f35e03b166a\n"
  "libheif.decode_only_patch.sha256=${PATCH_SHA256}\n"
  "dav1d.version=1.5.4\n"
  "dav1d.sha256=686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5\n"
  "dav1d.bitdepths=8,16\n"
  "dav1d.asm=on\n"
  "codecs=hevc-decode-libde265,av1-decode-dav1d\n"
  "linkage=static\n"
  "plugins=off\n"
  "encoders=off\n")
