set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT VOVE_LLVM_MINGW_ROOT AND DEFINED ENV{VOVE_LLVM_MINGW_ROOT})
  set(VOVE_LLVM_MINGW_ROOT "$ENV{VOVE_LLVM_MINGW_ROOT}")
endif()
if(NOT VOVE_LLVM_MINGW_ROOT)
  message(FATAL_ERROR
    "Set VOVE_LLVM_MINGW_ROOT to the extracted llvm-mingw directory")
endif()

file(TO_CMAKE_PATH "${VOVE_LLVM_MINGW_ROOT}" VOVE_LLVM_MINGW_ROOT)
set(VOVE_LLVM_MINGW_ROOT "${VOVE_LLVM_MINGW_ROOT}" CACHE PATH
    "Extracted llvm-mingw directory" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES VOVE_LLVM_MINGW_ROOT)
set(_vove_llvm_mingw_bin "${VOVE_LLVM_MINGW_ROOT}/bin")
set(_vove_llvm_mingw_target "${VOVE_LLVM_MINGW_ROOT}/x86_64-w64-mingw32")

foreach(required_file
    "${_vove_llvm_mingw_bin}/x86_64-w64-mingw32-clang.exe"
    "${_vove_llvm_mingw_bin}/x86_64-w64-mingw32-clang++.exe"
    "${_vove_llvm_mingw_bin}/llvm-ar.exe"
    "${_vove_llvm_mingw_bin}/llvm-ranlib.exe")
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "Incomplete llvm-mingw toolchain: ${required_file}")
  endif()
endforeach()

set(CMAKE_C_COMPILER
    "${_vove_llvm_mingw_bin}/x86_64-w64-mingw32-clang.exe" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER
    "${_vove_llvm_mingw_bin}/x86_64-w64-mingw32-clang++.exe" CACHE FILEPATH "")
set(CMAKE_RC_COMPILER
    "${_vove_llvm_mingw_bin}/x86_64-w64-mingw32-windres.exe" CACHE FILEPATH "")
set(CMAKE_AR "${_vove_llvm_mingw_bin}/llvm-ar.exe" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_vove_llvm_mingw_bin}/llvm-ranlib.exe" CACHE FILEPATH "")
set(CMAKE_STRIP "${_vove_llvm_mingw_bin}/llvm-strip.exe" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH "${_vove_llvm_mingw_target}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
