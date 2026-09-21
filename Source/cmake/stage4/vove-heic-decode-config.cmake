include_guard(GLOBAL)

get_filename_component(
  _vove_heic_decode_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

foreach(required_file
    "${_vove_heic_decode_prefix}/lib/libde265.a"
    "${_vove_heic_decode_prefix}/lib/libdav1d.a"
    "${_vove_heic_decode_prefix}/lib/libheif.a"
    "${_vove_heic_decode_prefix}/include/libheif/heif.h")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR
      "The vove-heic-decode contour is incomplete: ${required_file}")
  endif()
endforeach()

find_package(Threads REQUIRED)

if(NOT TARGET vove-heic-decode::libde265)
  add_library(vove-heic-decode::libde265 STATIC IMPORTED)
  set_target_properties(vove-heic-decode::libde265 PROPERTIES
    IMPORTED_LOCATION "${_vove_heic_decode_prefix}/lib/libde265.a"
    INTERFACE_COMPILE_DEFINITIONS "LIBDE265_STATIC_BUILD"
    INTERFACE_LINK_LIBRARIES "Threads::Threads;$<$<PLATFORM_ID:Linux>:m>")
endif()

if(NOT TARGET vove-heic-decode::dav1d)
  add_library(vove-heic-decode::dav1d STATIC IMPORTED)
  set_target_properties(vove-heic-decode::dav1d PROPERTIES
    IMPORTED_LOCATION "${_vove_heic_decode_prefix}/lib/libdav1d.a"
    INTERFACE_LINK_LIBRARIES "$<$<NOT:$<PLATFORM_ID:Windows>>:Threads::Threads>;$<$<PLATFORM_ID:Linux>:m>")
endif()

if(NOT TARGET vove-heic-decode::heif)
  add_library(vove-heic-decode::heif STATIC IMPORTED)
  set_target_properties(vove-heic-decode::heif PROPERTIES
    IMPORTED_LOCATION "${_vove_heic_decode_prefix}/lib/libheif.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_vove_heic_decode_prefix}/include"
    INTERFACE_COMPILE_DEFINITIONS "LIBHEIF_STATIC_BUILD"
    INTERFACE_LINK_LIBRARIES "vove-heic-decode::libde265;vove-heic-decode::dav1d")
endif()

unset(_vove_heic_decode_prefix)
