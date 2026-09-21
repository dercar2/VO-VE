cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(cmake_lists "${SOURCE_DIR}/CMakeLists.txt")
if(NOT EXISTS "${cmake_lists}")
  message(FATAL_ERROR "libwebp CMakeLists.txt is missing: ${cmake_lists}")
endif()

file(READ "${cmake_lists}" contents)
set(original "target_link_libraries(webpdemux webp)")
set(replacement "target_link_libraries(webpdemux webpdecoder)")

string(FIND "${contents}" "${replacement}" replacement_position)
if(NOT replacement_position EQUAL -1)
  message(STATUS "libwebp decode-only patch is already applied")
  return()
endif()

string(REGEX MATCHALL "target_link_libraries\\(webpdemux webp\\)" matches
  "${contents}")
list(LENGTH matches match_count)
if(NOT match_count EQUAL 1)
  message(FATAL_ERROR
    "Expected exactly one libwebp demux/full-codec link, found ${match_count}")
endif()

string(REPLACE "${original}" "${replacement}" contents "${contents}")
file(WRITE "${cmake_lists}" "${contents}")
