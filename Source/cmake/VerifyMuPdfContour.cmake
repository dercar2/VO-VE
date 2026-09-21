cmake_minimum_required(VERSION 3.24)

foreach(required IN ITEMS
        VOVE_MUPDF_SOURCE_ROOT VOVE_MUPDF_SOURCE_ARCHIVE
        VOVE_MUPDF_LIBRARY VOVE_MUPDF_THIRD_LIBRARY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/MuPdfContourLock.cmake")
vove_verify_pinned_mupdf_contour(
    "${VOVE_MUPDF_SOURCE_ROOT}"
    "${VOVE_MUPDF_SOURCE_ARCHIVE}"
    "${VOVE_MUPDF_LIBRARY}"
    "${VOVE_MUPDF_THIRD_LIBRARY}")
message(STATUS "Pinned MuPDF ${VOVE_MUPDF_LOCK_VERSION} contour verified")
