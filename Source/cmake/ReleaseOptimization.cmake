include(CheckIPOSupported)

option(VOVE_RELEASE_SECTION_GC
    "Split functions/data and collect unused sections in Release builds" ON)
option(VOVE_RELEASE_IPO
    "Enable measured interprocedural optimisation in Release builds" OFF)
option(VOVE_RELEASE_LINK_MAPS
    "Write linker maps for shipped project executables in Release builds" ON)

if(VOVE_RELEASE_SECTION_GC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        "$<$<CONFIG:Release>:-ffunction-sections>"
        "$<$<CONFIG:Release>:-fdata-sections>")
    add_link_options("$<$<CONFIG:Release>:-Wl,--gc-sections>")
endif()

if(VOVE_RELEASE_IPO)
    check_ipo_supported(RESULT vove_ipo_supported OUTPUT vove_ipo_error)
    if(NOT vove_ipo_supported)
        message(FATAL_ERROR "Release IPO/LTO is required but unsupported: ${vove_ipo_error}")
    endif()
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

function(vove_release_link_map target)
    if(NOT TARGET "${target}" OR NOT VOVE_RELEASE_LINK_MAPS)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        if(WIN32)
            target_link_options("${target}" PRIVATE
                "$<$<CONFIG:Release>:-Wl,-Map,$<TARGET_FILE_DIR:${target}>/${target}.map>")
        else()
            target_link_options("${target}" PRIVATE
                "$<$<CONFIG:Release>:-Wl,-Map,$<TARGET_FILE_DIR:${target}>/${target}.map,--cref>")
        endif()
    endif()
endfunction()
