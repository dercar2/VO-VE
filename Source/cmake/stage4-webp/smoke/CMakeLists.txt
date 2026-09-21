cmake_minimum_required(VERSION 3.25)

project(vove_webp_decode_smoke LANGUAGES C)

find_package(vove-webp-decode CONFIG REQUIRED)

add_executable(vove-webp-decode-smoke main.c)
target_compile_features(vove-webp-decode-smoke PRIVATE c_std_99)
target_link_libraries(vove-webp-decode-smoke PRIVATE
  vove-webp-decode::webpdemux)
