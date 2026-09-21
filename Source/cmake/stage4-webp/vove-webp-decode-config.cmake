include_guard(GLOBAL)

get_filename_component(
  _vove_webp_decode_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

foreach(required_file
    "${_vove_webp_decode_prefix}/lib/libwebp.a"
    "${_vove_webp_decode_prefix}/lib/libwebpdemux.a"
    "${_vove_webp_decode_prefix}/include/webp/decode.h"
    "${_vove_webp_decode_prefix}/include/webp/demux.h")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR
      "The vove-webp-decode contour is incomplete: ${required_file}")
  endif()
endforeach()

if(NOT TARGET vove-webp-decode::webp)
  add_library(vove-webp-decode::webp STATIC IMPORTED)
  set_target_properties(vove-webp-decode::webp PROPERTIES
    IMPORTED_LOCATION "${_vove_webp_decode_prefix}/lib/libwebp.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_vove_webp_decode_prefix}/include")
endif()

if(NOT TARGET vove-webp-decode::webpdemux)
  add_library(vove-webp-decode::webpdemux STATIC IMPORTED)
  set_target_properties(vove-webp-decode::webpdemux PROPERTIES
    IMPORTED_LOCATION "${_vove_webp_decode_prefix}/lib/libwebpdemux.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_vove_webp_decode_prefix}/include"
    INTERFACE_LINK_LIBRARIES "vove-webp-decode::webp")
endif()

unset(_vove_webp_decode_prefix)
