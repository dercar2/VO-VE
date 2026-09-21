cmake_minimum_required(VERSION 3.25)

foreach(required_variable PREFIX DESTINATION)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()
if(NOT EXISTS "${PREFIX}/lib/cmake/vove-webp-decode/vove-webp-decode-config.cmake")
  message(FATAL_ERROR "The source install prefix is incomplete: ${PREFIX}")
endif()

file(REMOVE_RECURSE "${DESTINATION}")
file(MAKE_DIRECTORY "${DESTINATION}")
file(COPY "${PREFIX}/" DESTINATION "${DESTINATION}")
