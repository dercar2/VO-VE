cmake_minimum_required(VERSION 3.25)
foreach(required PREFIX HEADERS DECODER CMS HWY LCMS_SHA256 CONFIG)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "Missing ${required}")
  endif()
endforeach()
file(MAKE_DIRECTORY "${PREFIX}/lib" "${PREFIX}/include/jxl" "${PREFIX}/lib/cmake/vove-jpegxl-decode")
file(COPY_FILE "${DECODER}" "${PREFIX}/lib/libjxl_dec.a" ONLY_IF_DIFFERENT)
file(COPY_FILE "${CMS}" "${PREFIX}/lib/libjxl_cms.a" ONLY_IF_DIFFERENT)
file(COPY_FILE "${HWY}" "${PREFIX}/lib/libhwy.a" ONLY_IF_DIFFERENT)
foreach(header decode.h types.h memory_manager.h color_encoding.h codestream_header.h
    cms_interface.h cms.h parallel_runner.h version.h jxl_export.h jxl_cms_export.h)
  file(COPY_FILE "${HEADERS}/${header}" "${PREFIX}/include/jxl/${header}" ONLY_IF_DIFFERENT)
endforeach()
file(COPY_FILE "${CONFIG}" "${PREFIX}/lib/cmake/vove-jpegxl-decode/vove-jpegxl-decode-config.cmake" ONLY_IF_DIFFERENT)
include("${CMAKE_CURRENT_LIST_DIR}/inputs.cmake")
file(WRITE "${PREFIX}/vove-jpegxl-decode-manifest.txt"
  "contour=vove-jpegxl-decode\nlibjxl.version=0.12.0\nhighway.version=1.2.0\nbrotli.version=1.2.0\n")
foreach(name IN LISTS VOVE_JXL_INPUTS)
  file(APPEND "${PREFIX}/vove-jpegxl-decode-manifest.txt"
    "${name}.commit=${${name}_commit}\n${name}.sha256=${${name}_sha256}\n")
endforeach()
file(APPEND "${PREFIX}/vove-jpegxl-decode-manifest.txt"
  "lcms2.reused_archive.sha256=${LCMS_SHA256}\nlinkage=static\nencoders=off\nboxes=off\njpeg-reconstruction=off\nbrotli=config-only\n")
