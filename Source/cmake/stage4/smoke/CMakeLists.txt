cmake_minimum_required(VERSION 3.25)

project(vove_heic_decode_smoke LANGUAGES CXX)

find_package(vove-heic-decode CONFIG REQUIRED)

add_executable(vove-heic-decode-smoke main.cpp)
target_compile_features(vove-heic-decode-smoke PRIVATE cxx_std_20)
target_link_libraries(vove-heic-decode-smoke PRIVATE vove-heic-decode::heif)
