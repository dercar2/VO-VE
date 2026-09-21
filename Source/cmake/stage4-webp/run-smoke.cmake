cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED EXECUTABLE OR NOT EXISTS "${EXECUTABLE}")
  message(FATAL_ERROR "Smoke executable is missing: ${EXECUTABLE}")
endif()
if(NOT DEFINED SAMPLE OR NOT EXISTS "${SAMPLE}")
  message(FATAL_ERROR "WebP smoke sample is missing: ${SAMPLE}")
endif()

if(DEFINED RUNTIME_DIR AND NOT RUNTIME_DIR STREQUAL "")
  if(NOT IS_DIRECTORY "${RUNTIME_DIR}")
    message(FATAL_ERROR "Smoke runtime directory is missing: ${RUNTIME_DIR}")
  endif()
  set(ENV{PATH} "${RUNTIME_DIR};$ENV{PATH}")
endif()

get_filename_component(smoke_directory "${EXECUTABLE}" DIRECTORY)
set(local_sample "${smoke_directory}/vove-webp-decode-smoke.webp")
file(COPY_FILE "${SAMPLE}" "${local_sample}" ONLY_IF_DIFFERENT)
execute_process(
  COMMAND "${EXECUTABLE}" "vove-webp-decode-smoke.webp"
  WORKING_DIRECTORY "${smoke_directory}"
  RESULT_VARIABLE smoke_result)
file(REMOVE "${local_sample}")
if(NOT smoke_result EQUAL 0)
  message(FATAL_ERROR "WebP decode contour smoke failed: ${smoke_result}")
endif()
