include_guard(GLOBAL)
get_filename_component(_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(VOVE_JPEGXL_LCMS2_LIBRARY "" CACHE FILEPATH "Existing VO-VE static LCMS2 archive")
if(TARGET vove-lcms2)
  set(_lcms2_link vove-lcms2)
else()
  if(NOT EXISTS "${VOVE_JPEGXL_LCMS2_LIBRARY}")
    message(FATAL_ERROR "Provide target vove-lcms2 or set VOVE_JPEGXL_LCMS2_LIBRARY to the existing static LCMS2 archive")
  endif()
  set(_lcms2_link "${VOVE_JPEGXL_LCMS2_LIBRARY}")
endif()
find_package(Threads REQUIRED)
foreach(name jxl_dec jxl_cms hwy)
  if(NOT EXISTS "${_prefix}/lib/lib${name}.a")
    message(FATAL_ERROR "Missing static JPEG XL dependency: ${name}")
  endif()
  if(NOT TARGET vove-jpegxl-decode::${name})
    add_library(vove-jpegxl-decode::${name} STATIC IMPORTED)
    set_target_properties(vove-jpegxl-decode::${name} PROPERTIES
      IMPORTED_LOCATION "${_prefix}/lib/lib${name}.a"
      INTERFACE_INCLUDE_DIRECTORIES "${_prefix}/include")
  endif()
endforeach()
set_target_properties(vove-jpegxl-decode::jxl_cms PROPERTIES
  INTERFACE_COMPILE_DEFINITIONS "JXL_CMS_STATIC_DEFINE"
  INTERFACE_LINK_LIBRARIES "vove-jpegxl-decode::hwy;${_lcms2_link}")
set_target_properties(vove-jpegxl-decode::jxl_dec PROPERTIES
  INTERFACE_COMPILE_DEFINITIONS "JXL_STATIC_DEFINE"
  INTERFACE_LINK_LIBRARIES "vove-jpegxl-decode::jxl_cms;vove-jpegxl-decode::hwy;Threads::Threads;$<$<PLATFORM_ID:Linux>:m>")
unset(_prefix)
unset(_lcms2_link)
