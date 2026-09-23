cmake_minimum_required(VERSION 3.24)

project(VO_VE VERSION 0.2.20 LANGUAGES C CXX)

option(VOVE_BUILD_TESTS "Build headless core tests" ON)
option(VOVE_BUILD_BENCHMARKS "Build headless benchmarks" ON)
option(VOVE_BUILD_QT_APP "Build the production Qt application" OFF)
option(VOVE_BUILD_QT_SPIKE "Build the Qt Gate A reference spike" OFF)
option(VOVE_BUILD_WIN32_SPIKE "Build the native Win32 Gate A reference spike" OFF)
option(VOVE_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
option(VOVE_EXPECT_STRICT_SANDBOX "Require strict sandbox controls in supervisor tests" OFF)
option(VOVE_BUILD_MUPDF_DOCUMENT_WORKER "Build the isolated MuPDF PDF/AI worker" OFF)
option(VOVE_BUILD_RESVG_SVG_WORKER "Build the isolated resvg SVG/SVGZ worker" OFF)
option(VOVE_RELEASE_BUILD "Require the complete platform runtime for packaging" OFF)
option(VOVE_BUILD_FUZZERS "Build sanitizer-instrumented parser fuzz targets" OFF)
if(VOVE_BUILD_MUPDF_DOCUMENT_WORKER AND NOT VOVE_BUILD_QT_APP)
    message(FATAL_ERROR "VOVE_BUILD_MUPDF_DOCUMENT_WORKER requires VOVE_BUILD_QT_APP=ON")
endif()
if(VOVE_BUILD_RESVG_SVG_WORKER AND NOT VOVE_BUILD_QT_APP)
    message(FATAL_ERROR "VOVE_BUILD_RESVG_SVG_WORKER requires VOVE_BUILD_QT_APP=ON")
endif()
include(CTest)
set(VOVE_HEIC_DECODE_ROOT "" CACHE PATH "Installed decode-only HEIC contour prefix")
set(VOVE_WEBP_DECODE_ROOT "" CACHE PATH "Installed decode-only WebP contour prefix")
set(VOVE_JPEGXL_DECODE_ROOT "" CACHE PATH "Installed decode-only JPEG XL contour prefix")
set(VOVE_HEIC_TEST_SOURCE_ROOT "" CACHE PATH "Extracted libheif source used only by tests")
set(VOVE_WEBP_TEST_SOURCE_ROOT "" CACHE PATH "Extracted libwebp source used only by tests")
set(VOVE_MUPDF_SOURCE_ROOT "" CACHE PATH "Extracted MuPDF source root")
set(VOVE_MUPDF_SOURCE_ARCHIVE "" CACHE FILEPATH "Pinned MuPDF source archive")
set(VOVE_MUPDF_LIBRARY "" CACHE FILEPATH "Minimal static MuPDF library")
set(VOVE_MUPDF_THIRD_LIBRARY "" CACHE FILEPATH "Minimal static MuPDF third-party library")
set(VOVE_RESVG_BRIDGE_LIBRARY "" CACHE FILEPATH "Pinned static VO-VE resvg bridge library")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(ReleaseOptimization)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MuPdfContourLock.cmake")
find_package(Threads REQUIRED)

if(BUILD_TESTING AND VOVE_BUILD_TESTS)
    foreach(test_mode IN ITEMS
            pass wrong-version tampered-archive tampered-header
            tampered-library tampered-third-library)
        add_test(NAME vove-mupdf-contour-lock-${test_mode}
            COMMAND ${CMAKE_COMMAND}
                -DTEST_MODE=${test_mode}
                -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/mupdf-lock-test-${test_mode}
                -DTEST_SAFE_PARENT=${CMAKE_CURRENT_BINARY_DIR}
                -DMODULE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/cmake/MuPdfContourLock.cmake
                -DINNER_TEST_PATH=${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/MuPdfContourLockTest.cmake
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/MuPdfContourLockDriverTest.cmake)
    endforeach()
    add_test(NAME vove-mupdf-contour-lock-absolute-include
        COMMAND ${CMAKE_COMMAND}
            -DPROJECT_CMAKE=${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/MuPdfAbsoluteIncludeTest.cmake)
endif()

if(WIN32)
    get_filename_component(VOVE_CXX_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(VOVE_CXX_RUNTIME_FILES
        "${VOVE_CXX_RUNTIME_DIR}/libc++.dll"
        "${VOVE_CXX_RUNTIME_DIR}/libunwind.dll")

    function(vove_copy_cxx_runtime target)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${VOVE_CXX_RUNTIME_FILES}
                $<TARGET_FILE_DIR:${target}>
            VERBATIM)
    endfunction()
else()
    function(vove_copy_cxx_runtime target)
    endfunction()
endif()
add_library(vove-utf8proc STATIC
    third_party/utf8proc/utf8proc.c
)
target_include_directories(vove-utf8proc PUBLIC third_party/utf8proc)
target_compile_definitions(vove-utf8proc PUBLIC UTF8PROC_STATIC)
set_target_properties(vove-utf8proc PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden
)

add_library(vove-sqlite STATIC EXCLUDE_FROM_ALL
    third_party/sqlite/sqlite3.c
)
target_include_directories(vove-sqlite PUBLIC third_party/sqlite)
target_compile_definitions(vove-sqlite PRIVATE
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DQS=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
)
set_target_properties(vove-sqlite PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

add_library(vove-lcms2 STATIC EXCLUDE_FROM_ALL
    third_party/lcms2/src/cmsalpha.c
    third_party/lcms2/src/cmscam02.c
    third_party/lcms2/src/cmscgats.c
    third_party/lcms2/src/cmscnvrt.c
    third_party/lcms2/src/cmserr.c
    third_party/lcms2/src/cmsgamma.c
    third_party/lcms2/src/cmsgmt.c
    third_party/lcms2/src/cmshalf.c
    third_party/lcms2/src/cmsintrp.c
    third_party/lcms2/src/cmsio0.c
    third_party/lcms2/src/cmsio1.c
    third_party/lcms2/src/cmslut.c
    third_party/lcms2/src/cmsmd5.c
    third_party/lcms2/src/cmsmtrx.c
    third_party/lcms2/src/cmsnamed.c
    third_party/lcms2/src/cmsopt.c
    third_party/lcms2/src/cmspack.c
    third_party/lcms2/src/cmspcs.c
    third_party/lcms2/src/cmsplugin.c
    third_party/lcms2/src/cmsps2.c
    third_party/lcms2/src/cmssamp.c
    third_party/lcms2/src/cmssm.c
    third_party/lcms2/src/cmstypes.c
    third_party/lcms2/src/cmsvirt.c
    third_party/lcms2/src/cmswtpnt.c
    third_party/lcms2/src/cmsxform.c
)
target_include_directories(vove-lcms2 PUBLIC third_party/lcms2/include)
set_target_properties(vove-lcms2 PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

add_library(vove-qoi STATIC
    third_party/qoi/qoi.c
)
target_include_directories(vove-qoi PUBLIC third_party/qoi)
set_target_properties(vove-qoi PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

add_library(vove-qoi-codec STATIC
    src/cache/qoi_codec.cpp
)
add_library(vove::qoi_codec ALIAS vove-qoi-codec)

add_library(vove-miniz-inflate STATIC EXCLUDE_FROM_ALL
    third_party/miniz/miniz_tinfl.c
)
target_include_directories(vove-miniz-inflate PUBLIC third_party/miniz)
target_compile_definitions(vove-miniz-inflate PRIVATE
    MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_DEFLATE_APIS
    MINIZ_NO_ZLIB_APIS
    MINIZ_NO_MALLOC
    MINIZ_NO_STDIO
)
set_target_properties(vove-miniz-inflate PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

add_library(vove-core STATIC
    src/core/directory_model.cpp
)
add_library(vove::core ALIAS vove-core)
target_link_libraries(vove-core PRIVATE vove-utf8proc)

add_library(vove-search STATIC
    src/search/global_search.cpp
    src/search/search_protocol.cpp
)
add_library(vove::search ALIAS vove-search)
target_include_directories(vove-search PRIVATE src/search)

add_library(vove-catalog STATIC
    src/catalog/catalog_session.cpp
)
add_library(vove::catalog ALIAS vove-catalog)

add_library(vove-color STATIC
    src/color/color_transform.cpp
)
if(WIN32)
    target_sources(vove-color PRIVATE src/color/monitor_profile_windows.cpp)
    target_link_libraries(vove-color PRIVATE gdi32)
else()
    target_sources(vove-color PRIVATE src/color/monitor_profile_posix.cpp)
    target_link_libraries(vove-color PRIVATE ${CMAKE_DL_LIBS})
endif()
add_library(vove::color ALIAS vove-color)

add_library(vove-cache STATIC
    src/cache/artifact_store.cpp
    src/cache/cache_key.cpp
    src/cache/cache_maintenance.cpp
    src/cache/memory_artifact_cache.cpp
    src/cache/persistent_thumbnail_cache.cpp
    src/cache/sqlite_artifact_index.cpp
)
add_library(vove::cache ALIAS vove-cache)

add_library(vove-preview STATIC
    src/preview/folder_mosaic.cpp
    src/preview/rgba_scaler.cpp
    src/preview/thumbnail_scheduler.cpp
)
add_library(vove::preview ALIAS vove-preview)

add_library(vove-preview-protocol STATIC
    src/preview/helper_protocol.cpp
)
add_library(vove::preview_protocol ALIAS vove-preview-protocol)

add_library(vove-preview-pipeline STATIC
    src/preview/thumbnail_pipeline.cpp
)
add_library(vove::preview_pipeline ALIAS vove-preview-pipeline)

add_library(vove-worker-thumbnail-renderer STATIC
    src/preview/worker_thumbnail_renderer.cpp
)
add_library(vove::worker_thumbnail_renderer ALIAS vove-worker-thumbnail-renderer)

add_library(vove-raster-base STATIC
    src/handlers/raster/native_source.cpp
    src/handlers/raster/signature_probe.cpp
)
add_library(vove::raster_base ALIAS vove-raster-base)

add_library(vove-piex STATIC
    third_party/piex/src/binary_parse/cached_paged_byte_array.cc
    third_party/piex/src/binary_parse/range_checked_byte_ptr.cc
    third_party/piex/src/image_type_recognition/image_type_recognition_lite.cc
    third_party/piex/src/tiff_directory/tiff_directory.cc
    third_party/piex/src/piex.cc
    third_party/piex/src/piex_cr3.cc
    third_party/piex/src/tiff_parser.cc
)
add_library(vove::piex ALIAS vove-piex)
target_include_directories(vove-piex PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/piex>)
target_compile_features(vove-piex PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(vove-piex PRIVATE /w)
else()
    target_compile_options(vove-piex PRIVATE -w)
endif()

add_library(vove-raster-raw-preview STATIC
    src/handlers/raster/raw_preview_decoder.cpp
)
add_library(vove::raster_raw_preview ALIAS vove-raster-raw-preview)
target_include_directories(vove-raster-raw-preview PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(vove-raster-raw-preview PUBLIC cxx_std_20)
target_link_libraries(vove-raster-raw-preview PUBLIC
    vove::raster_base PRIVATE vove::piex)
vove_enable_warnings(vove-raster-raw-preview)

add_library(vove-raster-psd STATIC
    src/handlers/raster/psd_decoder.cpp
)
add_library(vove::raster_psd ALIAS vove-raster-psd)
target_include_directories(vove-raster-psd PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(vove-raster-psd PUBLIC cxx_std_20)
target_link_libraries(vove-raster-psd PUBLIC
    vove::raster_base vove::color vove-miniz-inflate)
vove_enable_warnings(vove-raster-psd)

add_library(vove-archive-preview STATIC
    src/handlers/archive/zip_reader.cpp
    src/handlers/archive/embedded_preview.cpp)
add_library(vove::archive_preview ALIAS vove-archive-preview)
target_include_directories(vove-archive-preview PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(vove-archive-preview PUBLIC cxx_std_20)
target_link_libraries(vove-archive-preview PUBLIC vove::raster_base PRIVATE vove-miniz-inflate)
vove_enable_warnings(vove-archive-preview)

add_library(vove-cdr-preview STATIC
    src/handlers/cdr/embedded_preview.cpp
)
add_library(vove::cdr_preview ALIAS vove-cdr-preview)
target_include_directories(vove-cdr-preview PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(vove-cdr-preview PUBLIC cxx_std_20)
target_link_libraries(vove-cdr-preview PUBLIC
    vove::raster_base vove::archive_preview)
vove_enable_warnings(vove-cdr-preview)

add_library(vove-dxf-preview STATIC
    src/handlers/dxf/embedded_preview.cpp
)
add_library(vove::dxf_preview ALIAS vove-dxf-preview)
target_include_directories(vove-dxf-preview PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(vove-dxf-preview PUBLIC cxx_std_20)
target_link_libraries(vove-dxf-preview PUBLIC vove::raster_base)
vove_enable_warnings(vove-dxf-preview)

if(WIN32)
    add_library(vove-raster-wic STATIC
        src/handlers/raster/wic_raster_decoder.cpp)
    add_library(vove::raster_wic ALIAS vove-raster-wic)
    target_include_directories(vove-raster-wic PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(vove-raster-wic PUBLIC cxx_std_20)
    target_link_libraries(vove-raster-wic PUBLIC
        vove::raster_base vove::color windowscodecs ole32 propsys)
    vove_enable_warnings(vove-raster-wic)
endif()

add_library(vove-worker-protocol STATIC
    src/worker/protocol.cpp
)
add_library(vove::worker_protocol ALIAS vove-worker-protocol)

if(VOVE_BUILD_FUZZERS)
    if(VOVE_BUILD_TESTS OR VOVE_BUILD_BENCHMARKS OR VOVE_BUILD_QT_APP OR
       VOVE_BUILD_QT_SPIKE OR VOVE_BUILD_WIN32_SPIKE OR VOVE_BUILD_MUPDF_DOCUMENT_WORKER)
        message(FATAL_ERROR
            "VOVE_BUILD_FUZZERS requires a dedicated contour with tests, benchmarks, UI, "
            "spikes and the document worker disabled")
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "VOVE_BUILD_FUZZERS requires Clang C and C++ compilers")
    endif()
    if(WIN32 AND NOT MINGW)
        message(FATAL_ERROR "The Windows fuzz contour requires LLVM-MinGW")
    endif()

    if(WIN32 AND MINGW)
        if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
            message(FATAL_ERROR "The LLVM-MinGW fuzz contour currently supports x86_64 only")
        endif()
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} --print-resource-dir
            OUTPUT_VARIABLE VOVE_CLANG_RESOURCE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        set(VOVE_LIBFUZZER_RUNTIME
            "${VOVE_CLANG_RESOURCE_DIR}/lib/windows/libclang_rt.fuzzer-x86_64.a")
        if(NOT EXISTS "${VOVE_LIBFUZZER_RUNTIME}")
            message(FATAL_ERROR "LLVM-MinGW libFuzzer runtime was not found")
        endif()
        set(VOVE_FUZZ_COMPILE_OPTIONS
            -fno-omit-frame-pointer
            -fsanitize=address,undefined
            -fsanitize-coverage=inline-8bit-counters,indirect-calls,trace-cmp,pc-table)
        set(VOVE_FUZZ_LINK_OPTIONS
            -fno-omit-frame-pointer
            -fsanitize=address,undefined)
    else()
        set(VOVE_FUZZ_COMPILE_OPTIONS
            -fno-omit-frame-pointer
            -fsanitize=address,undefined,fuzzer-no-link)
        set(VOVE_FUZZ_LINK_OPTIONS
            -fno-omit-frame-pointer
            -fsanitize=address,undefined,fuzzer)
    endif()

    foreach(instrumented_target IN ITEMS
            vove-qoi
            vove-qoi-codec
            vove-cache
            vove-miniz-inflate
            vove-raster-base
            vove-raster-psd
            vove-piex
            vove-raster-raw-preview
            vove-archive-preview
            vove-cdr-preview
            vove-worker-protocol)
        target_compile_options(${instrumented_target} PRIVATE ${VOVE_FUZZ_COMPILE_OPTIONS})
    endforeach()

    function(vove_add_fuzzer target source)
        add_executable(${target} ${source})
        target_compile_options(${target} PRIVATE ${VOVE_FUZZ_COMPILE_OPTIONS})
        target_link_options(${target} PRIVATE ${VOVE_FUZZ_LINK_OPTIONS})
        if(VOVE_LIBFUZZER_RUNTIME)
            target_link_libraries(${target} PRIVATE "${VOVE_LIBFUZZER_RUNTIME}")
        endif()
        vove_enable_warnings(${target})
    endfunction()

    vove_add_fuzzer(vove-worker-protocol-fuzzer tests/fuzz/worker_protocol_fuzzer.cpp)
    target_link_libraries(vove-worker-protocol-fuzzer PRIVATE vove::worker_protocol)

    vove_add_fuzzer(vove-cdr-container-fuzzer tests/fuzz/cdr_container_fuzzer.cpp)
    target_link_libraries(vove-cdr-container-fuzzer PRIVATE vove::cdr_preview)

    vove_add_fuzzer(vove-psd-composite-fuzzer tests/fuzz/psd_composite_fuzzer.cpp)
    target_link_libraries(vove-psd-composite-fuzzer PRIVATE vove::raster_psd)

    vove_add_fuzzer(vove-raw-preview-fuzzer tests/fuzz/raw_preview_fuzzer.cpp)
    target_link_libraries(vove-raw-preview-fuzzer PRIVATE vove::raster_raw_preview)

    vove_add_fuzzer(vove-qoi-cache-fuzzer tests/fuzz/qoi_cache_fuzzer.cpp)
    target_link_libraries(vove-qoi-cache-fuzzer PRIVATE vove::qoi_codec)

    vove_add_fuzzer(vove-vvt1-cache-fuzzer tests/fuzz/vvt1_cache_fuzzer.cpp)
    target_include_directories(vove-vvt1-cache-fuzzer PRIVATE src/cache)
    target_link_libraries(vove-vvt1-cache-fuzzer PRIVATE vove::cache vove::qoi_codec)
endif()

add_library(vove-worker-runtime STATIC
    src/worker/runtime.cpp
)
target_include_directories(vove-worker-runtime PRIVATE src/worker)
target_link_libraries(vove-worker-runtime PUBLIC vove-worker-protocol)
add_library(vove::worker_runtime ALIAS vove-worker-runtime)

add_library(vove-worker-supervisor STATIC)
if(WIN32)
    target_sources(vove-worker-supervisor PRIVATE
        src/worker/windows/supervisor.cpp src/worker/windows/runtime_access.cpp)
    target_compile_definitions(vove-worker-supervisor PRIVATE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00)
    if(BUILD_TESTING AND VOVE_BUILD_TESTS)
        target_compile_definitions(vove-worker-supervisor PRIVATE
            VOVE_SUPERVISOR_TEST_HOOKS=1)
    endif()
    target_link_libraries(vove-worker-supervisor PRIVATE advapi32 userenv)
else()
    target_sources(vove-worker-supervisor PRIVATE src/worker/posix/supervisor.cpp)
endif()
target_link_libraries(vove-worker-supervisor PUBLIC vove-worker-protocol)
add_library(vove::worker_supervisor ALIAS vove-worker-supervisor)

add_library(vove-platform STATIC
    src/platform/catalog_protocol.cpp
    src/platform/catalog_protocol_decode.cpp
    src/platform/directory_enumerator.cpp
    src/platform/directory_service.cpp
)
if(WIN32)
    target_sources(vove-platform PRIVATE src/platform/windows/catalog_process.cpp
        src/platform/windows/directory_enumerator.cpp
        src/platform/windows/read_only_source.cpp)
else()
    target_sources(vove-platform PRIVATE src/platform/posix/catalog_process.cpp
        src/platform/posix/directory_enumerator.cpp
        src/platform/posix/read_only_source.cpp)
endif()
add_library(vove::platform ALIAS vove-platform)
if(WIN32)
    target_compile_definitions(vove-platform PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

add_library(vove-fileops STATIC
    src/fileops/basic_directory_transfer.cpp
    src/fileops/basic_directory_transfer_protocol.cpp
    src/fileops/batch_rename.cpp
    src/fileops/batch_rename_coordinator.cpp
    src/fileops/batch_rename_transaction.cpp
    src/fileops/current_operation_lease.cpp
    src/fileops/current_operation_journal.cpp
    src/fileops/directory_transfer_control.cpp
    src/fileops/directory_leaf_transfer.cpp
    src/fileops/directory_root_staging.cpp
    src/fileops/directory_transfer_manifest.cpp
    src/fileops/directory_transfer_manifest_store.cpp
    src/fileops/directory_transfer_progress.cpp
    src/fileops/directory_transfer_progress_store.cpp
    src/fileops/durable_journal.cpp
    src/fileops/filename_component_limit.cpp
    src/fileops/file_operation.cpp
    src/fileops/file_operation_protocol.cpp
    src/fileops/file_operation_service.cpp
    src/fileops/file_transfer_coordinator.cpp
    src/fileops/file_transfer_protocol.cpp
    src/fileops/file_transfer_service.cpp
    src/fileops/file_transfer_transaction.cpp
    src/fileops/permanent_delete_coordinator.cpp
    src/fileops/permanent_delete_transaction.cpp
    src/fileops/sha256.cpp
    src/fileops/trash_catalog.cpp
    src/fileops/trash_coordinator.cpp
    src/fileops/trash_transaction.cpp
)
if(WIN32)
    target_sources(vove-fileops PRIVATE src/fileops/windows/trash_security.cpp)
endif()
target_include_directories(vove-fileops PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
target_link_libraries(vove-fileops PUBLIC vove::platform Threads::Threads)
if(WIN32)
    target_link_libraries(vove-fileops PRIVATE advapi32 bcrypt normaliz)
    target_compile_definitions(vove-fileops PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
if(VOVE_BUILD_TESTS AND UNIX)
    target_compile_definitions(vove-fileops PRIVATE VOVE_DURABLE_JOURNAL_TEST_HOOKS=1)
endif()
if(VOVE_BUILD_TESTS)
    target_compile_definitions(vove-fileops PRIVATE
        VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS=1
        VOVE_TRASH_CATALOG_TEST_HOOKS=1
        VOVE_TRASH_COORDINATOR_TEST_HOOKS=1)
endif()
add_library(vove::fileops ALIAS vove-fileops)

add_library(vove-fileop-executor STATIC
    src/fileops/file_operation_executor.cpp
)
if(WIN32)
    target_sources(vove-fileop-executor PRIVATE
        src/fileops/windows/file_operation.cpp
        src/fileops/windows/file_transfer_executor.cpp)
    target_link_libraries(vove-fileop-executor PRIVATE ntdll)
else()
    target_sources(vove-fileop-executor PRIVATE
        src/fileops/posix/file_operation.cpp
        src/fileops/posix/file_transfer_executor.cpp)
endif()
target_include_directories(vove-fileop-executor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
target_link_libraries(vove-fileop-executor PUBLIC vove::fileops)
if(WIN32)
    target_compile_definitions(vove-fileop-executor PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()
add_library(vove::fileop_executor ALIAS vove-fileop-executor)

if((BUILD_TESTING AND VOVE_BUILD_TESTS) OR VOVE_BUILD_QT_SPIKE OR VOVE_BUILD_WIN32_SPIKE)
    add_library(vove-spike-support STATIC EXCLUDE_FROM_ALL
        src/spike/async_catalog_source.cpp
        src/spike/catalog_session.cpp
        src/spike/catalog_source.cpp
        src/spike/synthetic_catalog.cpp
        src/spike/ui_benchmark.cpp
    )
    add_library(vove::spike_support ALIAS vove-spike-support)
endif()

foreach(target
        vove-core
        vove-search
        vove-catalog
        vove-cache
        vove-color
        vove-qoi-codec
        vove-preview
        vove-preview-protocol
        vove-preview-pipeline
        vove-worker-thumbnail-renderer
        vove-raster-base
        vove-worker-protocol
        vove-worker-runtime
        vove-worker-supervisor
        vove-platform
        vove-fileops
        vove-fileop-executor)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${target} PUBLIC cxx_std_20)
    vove_enable_warnings(${target})
endforeach()

target_link_libraries(vove-catalog PUBLIC vove::core)
target_link_libraries(vove-search PUBLIC vove::core)
target_link_libraries(vove-qoi-codec PRIVATE vove-qoi)
target_link_libraries(vove-cache PRIVATE vove::qoi_codec vove-sqlite)
target_link_libraries(vove-color PRIVATE vove-lcms2)
target_link_libraries(vove-preview-pipeline PUBLIC vove::cache vove::preview)
target_link_libraries(vove-worker-thumbnail-renderer PUBLIC
    vove::preview_pipeline vove::platform vove::raster_base vove::worker_supervisor)
target_link_libraries(vove-platform PUBLIC vove::catalog vove::core Threads::Threads)
if(TARGET vove-spike-support)
    target_include_directories(vove-spike-support
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_compile_features(vove-spike-support PUBLIC cxx_std_20)
    vove_enable_warnings(vove-spike-support)
    target_link_libraries(vove-spike-support PUBLIC vove::core)
endif()

add_executable(vove-catalog-helper src/platform/helper/main.cpp)
target_include_directories(vove-catalog-helper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
target_link_libraries(vove-catalog-helper PRIVATE vove::platform)
vove_enable_warnings(vove-catalog-helper)
vove_copy_cxx_runtime(vove-catalog-helper)

if(WIN32)
    add_library(vove-everything-sdk STATIC EXCLUDE_FROM_ALL
        third_party/everything_sdk/src/Everything.c)
    target_include_directories(vove-everything-sdk PUBLIC
        third_party/everything_sdk/include
        third_party/everything_sdk/ipc)
    target_compile_definitions(vove-everything-sdk PRIVATE UNICODE _UNICODE)
    set_target_properties(vove-everything-sdk PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)

    add_executable(vove-everything-helper
        src/search/everything_helper/main.cpp)
    target_include_directories(vove-everything-helper PRIVATE src/search)
    target_link_libraries(vove-everything-helper PRIVATE
        vove::search
        vove-everything-sdk
        user32)
    vove_enable_warnings(vove-everything-helper)
    vove_copy_cxx_runtime(vove-everything-helper)
else()
    add_executable(vove-plocate-helper
        src/search/plocate_helper/main.cpp)
    target_include_directories(vove-plocate-helper PRIVATE src/search)
    target_link_libraries(vove-plocate-helper PRIVATE vove::search)
    vove_enable_warnings(vove-plocate-helper)
    vove_copy_cxx_runtime(vove-plocate-helper)
endif()

add_executable(vove-fileop-helper src/fileops/helper/main.cpp)
target_include_directories(vove-fileop-helper PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
target_link_libraries(vove-fileop-helper PRIVATE vove::fileop_executor)
vove_enable_warnings(vove-fileop-helper)
vove_copy_cxx_runtime(vove-fileop-helper)
if(VOVE_BUILD_TESTS)
    target_compile_definitions(vove-fileop-helper PRIVATE
        VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS=1)
endif()

if(VOVE_BUILD_TESTS)
    add_executable(vove-worker src/worker/main.cpp)
    target_include_directories(vove-worker PRIVATE src/worker)
    target_link_libraries(vove-worker PRIVATE vove::worker_runtime)
    vove_enable_warnings(vove-worker)
    if(MINGW)
        target_link_options(vove-worker PRIVATE -static)
    endif()
endif()

if(VOVE_BUILD_BENCHMARKS)
    add_executable(vove-bench
        src/bench/main.cpp
        src/bench/process_memory.cpp
    )
    target_link_libraries(vove-bench PRIVATE vove::core)
    vove_enable_warnings(vove-bench)
    if(WIN32)
        target_link_libraries(vove-bench PRIVATE psapi)
    endif()
    if(MINGW)
        target_link_options(vove-bench PRIVATE -static)
    endif()

    add_executable(vove-color-profile-probe src/bench/color_profile_probe.cpp)
    target_link_libraries(vove-color-profile-probe PRIVATE vove::color)
    vove_enable_warnings(vove-color-profile-probe)
    if(MINGW)
        target_link_options(vove-color-profile-probe PRIVATE -static)
    endif()

    add_executable(vove-cache-profile-bench src/bench/cache_profile_bench.cpp)
    target_link_libraries(vove-cache-profile-bench PRIVATE vove::cache)
    vove_enable_warnings(vove-cache-profile-bench)
    if(MINGW)
        target_link_options(vove-cache-profile-bench PRIVATE -static)
    endif()
endif()

if(BUILD_TESTING AND VOVE_BUILD_TESTS)
    add_executable(vove-global-search-tests
        tests/search/global_search_test.cpp)
    target_include_directories(vove-global-search-tests PRIVATE src/search)
    target_link_libraries(vove-global-search-tests PRIVATE vove::search)
    vove_enable_warnings(vove-global-search-tests)
    if(MINGW)
        target_link_options(vove-global-search-tests PRIVATE -static)
    endif()
    add_test(NAME vove-global-search-tests COMMAND vove-global-search-tests)

    if(WIN32)
        add_executable(vove-everything-helper-tests
            tests/search/everything_helper_test.cpp)
        target_include_directories(vove-everything-helper-tests PRIVATE
            src/search
            third_party/everything_sdk/include
            third_party/everything_sdk/ipc)
        target_link_libraries(vove-everything-helper-tests PRIVATE vove::search)
        vove_enable_warnings(vove-everything-helper-tests)
        if(MINGW)
            target_link_options(vove-everything-helper-tests PRIVATE -static)
        endif()
        add_test(NAME vove-everything-helper-tests COMMAND vove-everything-helper-tests)
        set_tests_properties(vove-everything-helper-tests PROPERTIES TIMEOUT 15)
    else()
        add_executable(vove-plocate-provider-fixture
            tests/search/plocate_provider_fixture.cpp)
        vove_enable_warnings(vove-plocate-provider-fixture)

        add_executable(vove-plocate-helper-tests
            tests/search/plocate_helper_test.cpp)
        target_include_directories(vove-plocate-helper-tests PRIVATE src/search)
        target_link_libraries(vove-plocate-helper-tests PRIVATE vove::search)
        target_compile_definitions(vove-plocate-helper-tests PRIVATE
            "VOVE_TEST_PLOCATE_HELPER_PATH=\"$<TARGET_FILE:vove-plocate-helper>\""
            "VOVE_TEST_PLOCATE_PROVIDER_PATH=\"$<TARGET_FILE:vove-plocate-provider-fixture>\"")
        add_dependencies(vove-plocate-helper-tests
            vove-plocate-helper vove-plocate-provider-fixture)
        vove_enable_warnings(vove-plocate-helper-tests)
        add_test(NAME vove-plocate-helper-tests COMMAND vove-plocate-helper-tests)
        set_tests_properties(vove-plocate-helper-tests PROPERTIES ENVIRONMENT "LC_ALL=C")
    endif()

    add_executable(vove-core-tests tests/core/directory_model_test.cpp)
    target_link_libraries(vove-core-tests PRIVATE vove::core)
    vove_enable_warnings(vove-core-tests)
    if(MINGW)
        target_link_options(vove-core-tests PRIVATE -static)
    endif()
    add_test(NAME vove-core-tests COMMAND vove-core-tests)

    add_executable(vove-async-source-tests tests/core/async_catalog_source_test.cpp)
    target_link_libraries(vove-async-source-tests PRIVATE vove::spike_support)
    vove_enable_warnings(vove-async-source-tests)
    if(MINGW)
        target_link_options(vove-async-source-tests PRIVATE -static)
    endif()
    add_test(NAME vove-async-source-tests COMMAND vove-async-source-tests)

    add_executable(vove-catalog-session-tests tests/core/catalog_session_test.cpp)
    target_link_libraries(vove-catalog-session-tests PRIVATE vove::spike_support)
    vove_enable_warnings(vove-catalog-session-tests)
    if(MINGW)
        target_link_options(vove-catalog-session-tests PRIVATE -static)
    endif()
    add_test(NAME vove-catalog-session-tests COMMAND vove-catalog-session-tests)

    add_executable(vove-ui-benchmark-tests tests/core/ui_benchmark_test.cpp)
    target_link_libraries(vove-ui-benchmark-tests PRIVATE vove::spike_support)
    vove_enable_warnings(vove-ui-benchmark-tests)
    if(MINGW)
        target_link_options(vove-ui-benchmark-tests PRIVATE -static)
    endif()
    add_test(NAME vove-ui-benchmark-tests COMMAND vove-ui-benchmark-tests)

    add_executable(vove-production-catalog-tests tests/catalog/catalog_session_test.cpp)
    target_link_libraries(vove-production-catalog-tests PRIVATE vove::catalog)
    vove_enable_warnings(vove-production-catalog-tests)
    if(MINGW)
        target_link_options(vove-production-catalog-tests PRIVATE -static)
    endif()
    add_test(NAME vove-production-catalog-tests COMMAND vove-production-catalog-tests)

    add_executable(vove-color-tests tests/color/color_transform_test.cpp)
    target_link_libraries(vove-color-tests PRIVATE vove::color vove-lcms2)
    vove_enable_warnings(vove-color-tests)
    if(MINGW)
        target_link_options(vove-color-tests PRIVATE -static)
    endif()
    add_test(NAME vove-color-tests COMMAND vove-color-tests)
    set_tests_properties(vove-color-tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    add_executable(vove-artifact-store-tests tests/cache/artifact_store_test.cpp)
    target_link_libraries(vove-artifact-store-tests PRIVATE vove::cache)
    vove_enable_warnings(vove-artifact-store-tests)
    if(MINGW)
        target_link_options(vove-artifact-store-tests PRIVATE -static)
    endif()
    add_test(NAME vove-artifact-store-tests COMMAND vove-artifact-store-tests)

    add_executable(vove-memory-artifact-cache-tests
        tests/cache/memory_artifact_cache_test.cpp)
    target_link_libraries(vove-memory-artifact-cache-tests PRIVATE vove::cache)
    vove_enable_warnings(vove-memory-artifact-cache-tests)
    if(MINGW)
        target_link_options(vove-memory-artifact-cache-tests PRIVATE -static)
    endif()
    add_test(NAME vove-memory-artifact-cache-tests COMMAND vove-memory-artifact-cache-tests)

    add_executable(vove-persistent-thumbnail-cache-tests
        tests/cache/persistent_thumbnail_cache_test.cpp)
    target_link_libraries(vove-persistent-thumbnail-cache-tests PRIVATE vove::cache)
    vove_enable_warnings(vove-persistent-thumbnail-cache-tests)
    if(MINGW)
        target_link_options(vove-persistent-thumbnail-cache-tests PRIVATE -static)
    endif()
    add_test(NAME vove-persistent-thumbnail-cache-tests
        COMMAND vove-persistent-thumbnail-cache-tests)

    add_executable(vove-qoi-codec-tests tests/cache/qoi_codec_test.cpp)
    target_link_libraries(vove-qoi-codec-tests PRIVATE vove::qoi_codec)
    vove_enable_warnings(vove-qoi-codec-tests)
    if(MINGW)
        target_link_options(vove-qoi-codec-tests PRIVATE -static)
    endif()
    add_test(NAME vove-qoi-codec-tests COMMAND vove-qoi-codec-tests)

    add_executable(vove-sqlite-artifact-index-tests
        tests/cache/sqlite_artifact_index_test.cpp)
    target_link_libraries(vove-sqlite-artifact-index-tests PRIVATE vove::cache vove-sqlite)
    vove_enable_warnings(vove-sqlite-artifact-index-tests)
    if(MINGW)
        target_link_options(vove-sqlite-artifact-index-tests PRIVATE -static)
    endif()
    add_test(NAME vove-sqlite-artifact-index-tests
        COMMAND vove-sqlite-artifact-index-tests)

    add_executable(vove-cache-maintenance-tests tests/cache/cache_maintenance_test.cpp)
    target_link_libraries(vove-cache-maintenance-tests PRIVATE vove::cache vove-sqlite)
    vove_enable_warnings(vove-cache-maintenance-tests)
    if(MINGW)
        target_link_options(vove-cache-maintenance-tests PRIVATE -static)
    endif()
    add_test(NAME vove-cache-maintenance-tests COMMAND vove-cache-maintenance-tests)

    add_executable(vove-thumbnail-scheduler-tests
        tests/preview/thumbnail_scheduler_test.cpp)
    target_link_libraries(vove-thumbnail-scheduler-tests PRIVATE vove::preview)
    vove_enable_warnings(vove-thumbnail-scheduler-tests)
    if(MINGW)
        target_link_options(vove-thumbnail-scheduler-tests PRIVATE -static)
    endif()
    add_test(NAME vove-thumbnail-scheduler-tests COMMAND vove-thumbnail-scheduler-tests)

    add_executable(vove-folder-mosaic-tests tests/preview/folder_mosaic_test.cpp)
    target_link_libraries(vove-folder-mosaic-tests PRIVATE vove::preview)
    vove_enable_warnings(vove-folder-mosaic-tests)
    if(MINGW)
        target_link_options(vove-folder-mosaic-tests PRIVATE -static)
    endif()
    add_test(NAME vove-folder-mosaic-tests COMMAND vove-folder-mosaic-tests)

    add_executable(vove-rgba-scaler-tests tests/preview/rgba_scaler_test.cpp)
    target_link_libraries(vove-rgba-scaler-tests PRIVATE vove::preview)
    vove_enable_warnings(vove-rgba-scaler-tests)
    if(MINGW)
        target_link_options(vove-rgba-scaler-tests PRIVATE -static)
    endif()
    add_test(NAME vove-rgba-scaler-tests COMMAND vove-rgba-scaler-tests)

    add_executable(vove-thumbnail-pipeline-tests tests/preview/thumbnail_pipeline_test.cpp)
    target_link_libraries(vove-thumbnail-pipeline-tests PRIVATE vove::preview_pipeline)
    vove_enable_warnings(vove-thumbnail-pipeline-tests)
    if(MINGW)
        target_link_options(vove-thumbnail-pipeline-tests PRIVATE -static)
    endif()
    add_test(NAME vove-thumbnail-pipeline-tests COMMAND vove-thumbnail-pipeline-tests)

    add_executable(vove-preview-helper-protocol-tests tests/preview/helper_protocol_test.cpp)
    target_link_libraries(vove-preview-helper-protocol-tests PRIVATE vove::preview_protocol)
    vove_enable_warnings(vove-preview-helper-protocol-tests)
    if(MINGW)
        target_link_options(vove-preview-helper-protocol-tests PRIVATE -static)
    endif()
    add_test(NAME vove-preview-helper-protocol-tests COMMAND vove-preview-helper-protocol-tests)

    add_executable(vove-raster-signature-probe-tests
        tests/handlers/raster_signature_probe_test.cpp)
    target_link_libraries(vove-raster-signature-probe-tests PRIVATE vove::raster_base)
    vove_enable_warnings(vove-raster-signature-probe-tests)
    if(MINGW)
        target_link_options(vove-raster-signature-probe-tests PRIVATE -static)
    endif()
    add_test(NAME vove-raster-signature-probe-tests COMMAND vove-raster-signature-probe-tests)

    add_executable(vove-raster-native-source-tests
        tests/handlers/raster_native_source_test.cpp)
    target_link_libraries(vove-raster-native-source-tests PRIVATE vove::raster_base)
    vove_enable_warnings(vove-raster-native-source-tests)
    if(MINGW)
        target_link_options(vove-raster-native-source-tests PRIVATE -static)
    endif()
    add_test(NAME vove-raster-native-source-tests COMMAND vove-raster-native-source-tests)

    add_executable(vove-worker-protocol-tests tests/worker/protocol_test.cpp)
    target_link_libraries(vove-worker-protocol-tests PRIVATE vove::worker_protocol)
    vove_enable_warnings(vove-worker-protocol-tests)
    if(MINGW)
        target_link_options(vove-worker-protocol-tests PRIVATE -static)
    endif()
    add_test(NAME vove-worker-protocol-tests COMMAND vove-worker-protocol-tests)

    add_executable(vove-worker-runtime-tests tests/worker/runtime_test.cpp)
    target_include_directories(vove-worker-runtime-tests PRIVATE src/worker)
    target_link_libraries(vove-worker-runtime-tests PRIVATE vove::worker_runtime)
    vove_enable_warnings(vove-worker-runtime-tests)
    if(MINGW)
        target_link_options(vove-worker-runtime-tests PRIVATE -static)
    endif()
    add_test(NAME vove-worker-runtime-tests COMMAND vove-worker-runtime-tests)

    if(WIN32)
        add_executable(vove-worker-supervisor-tests
            tests/worker/windows_supervisor_test.cpp)
        target_compile_definitions(vove-worker-supervisor-tests PRIVATE
            _WIN32_WINNT=0x0A00 WINVER=0x0A00
            "VOVE_TEST_WORKER_PATH=\"$<TARGET_FILE:vove-worker-supervisor-tests>\"")
        add_dependencies(vove-worker-supervisor-tests vove-worker)
    else()
        add_executable(vove-worker-supervisor-tests
            tests/worker/posix_supervisor_test.cpp)
        target_compile_definitions(vove-worker-supervisor-tests PRIVATE
            "VOVE_TEST_WORKER_PATH=\"$<TARGET_FILE:vove-worker>\"")
        add_dependencies(vove-worker-supervisor-tests vove-worker)
        if(VOVE_EXPECT_STRICT_SANDBOX)
            target_compile_definitions(vove-worker-supervisor-tests PRIVATE
                VOVE_EXPECT_STRICT_SANDBOX=1)
        endif()
    endif()
    target_link_libraries(vove-worker-supervisor-tests PRIVATE
        vove::worker_supervisor)
    if(WIN32)
        target_link_libraries(vove-worker-supervisor-tests PRIVATE ws2_32)
    endif()
    vove_enable_warnings(vove-worker-supervisor-tests)
    if(MINGW)
        target_link_options(vove-worker-supervisor-tests PRIVATE -static)
    endif()
    add_test(NAME vove-worker-supervisor-tests
        COMMAND vove-worker-supervisor-tests)
    set_tests_properties(vove-worker-supervisor-tests PROPERTIES RUN_SERIAL TRUE)

    add_executable(vove-catalog-protocol-tests tests/platform/catalog_protocol_test.cpp)
    target_include_directories(vove-catalog-protocol-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
    target_link_libraries(vove-catalog-protocol-tests PRIVATE vove::platform)
    vove_enable_warnings(vove-catalog-protocol-tests)
    if(MINGW)
        target_link_options(vove-catalog-protocol-tests PRIVATE -static)
    endif()
    add_test(NAME vove-catalog-protocol-tests COMMAND vove-catalog-protocol-tests)

    add_executable(vove-directory-recursive-tests tests/platform/directory_recursive_test.cpp)
    target_include_directories(vove-directory-recursive-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
    target_link_libraries(vove-directory-recursive-tests PRIVATE vove::platform)
    vove_enable_warnings(vove-directory-recursive-tests)
    if(MINGW)
        target_link_options(vove-directory-recursive-tests PRIVATE -static)
    endif()
    add_test(NAME vove-directory-recursive-tests COMMAND vove-directory-recursive-tests)

    add_executable(vove-directory-service-tests tests/platform/directory_service_test.cpp)
    target_link_libraries(vove-directory-service-tests PRIVATE
        vove::platform vove::worker_thumbnail_renderer)
    vove_enable_warnings(vove-directory-service-tests)
    if(MINGW)
        target_link_options(vove-directory-service-tests PRIVATE -static)
    endif()
    add_test(NAME vove-directory-service-tests COMMAND vove-directory-service-tests)

    add_executable(vove-read-only-source-tests tests/platform/read_only_source_test.cpp)
    target_link_libraries(vove-read-only-source-tests PRIVATE vove::platform)
    if(WIN32)
        target_include_directories(vove-read-only-source-tests PRIVATE src/platform/windows)
    endif()
    vove_enable_warnings(vove-read-only-source-tests)
    if(MINGW)
        target_link_options(vove-read-only-source-tests PRIVATE -static)
    endif()
    add_test(NAME vove-read-only-source-tests COMMAND vove-read-only-source-tests)

    set(vove_fileop_test_executor vove::fileop_executor)
    if(WIN32)
        add_library(vove-fileop-executor-testable STATIC
            src/fileops/file_operation_executor.cpp
            src/fileops/windows/file_operation.cpp
            src/fileops/windows/file_transfer_executor.cpp)
        target_compile_definitions(vove-fileop-executor-testable PUBLIC
            VOVE_FILEOP_TEST_HOOKS=1
            VOVE_FILE_TRANSFER_TEST_HOOKS=1
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00)
        target_include_directories(vove-fileop-executor-testable PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
        target_link_libraries(vove-fileop-executor-testable PUBLIC vove::fileops)
        target_link_libraries(vove-fileop-executor-testable PRIVATE ntdll)
        target_compile_features(vove-fileop-executor-testable PUBLIC cxx_std_20)
        vove_enable_warnings(vove-fileop-executor-testable)
        set(vove_fileop_test_executor vove-fileop-executor-testable)
    elseif(UNIX)
        add_library(vove-fileop-executor-testable STATIC
            src/fileops/file_operation_executor.cpp
            src/fileops/posix/file_operation.cpp
            src/fileops/posix/file_transfer_executor.cpp)
        target_compile_definitions(vove-fileop-executor-testable PUBLIC
            VOVE_FILEOP_TEST_HOOKS=1)
        target_include_directories(vove-fileop-executor-testable PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
        target_link_libraries(vove-fileop-executor-testable PUBLIC vove::fileops)
        target_compile_features(vove-fileop-executor-testable PUBLIC cxx_std_20)
        vove_enable_warnings(vove-fileop-executor-testable)
        set(vove_fileop_test_executor vove-fileop-executor-testable)
    endif()

    add_executable(vove-file-operation-tests tests/fileops/file_operation_test.cpp)
    target_include_directories(vove-file-operation-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
    target_link_libraries(vove-file-operation-tests PRIVATE ${vove_fileop_test_executor})
    vove_enable_warnings(vove-file-operation-tests)
    if(MINGW)
        target_link_options(vove-file-operation-tests PRIVATE -static)
    endif()
    add_test(NAME vove-file-operation-tests COMMAND vove-file-operation-tests)

    if(WIN32)
        add_executable(vove-windows-recycle-spike-tests
            tests/fileops/windows_recycle_bin_spike_test.cpp)
        target_link_libraries(vove-windows-recycle-spike-tests PRIVATE
            ole32 shell32 uuid)
        target_compile_definitions(vove-windows-recycle-spike-tests PRIVATE
            _WIN32_WINNT=0x0A00 WINVER=0x0A00)
        vove_enable_warnings(vove-windows-recycle-spike-tests)
        if(MINGW)
            target_link_options(vove-windows-recycle-spike-tests PRIVATE -static)
        endif()
        add_test(NAME vove-windows-recycle-spike-tests
            COMMAND vove-windows-recycle-spike-tests)
    endif()

    add_executable(vove-batch-rename-tests tests/fileops/batch_rename_test.cpp)
    target_link_libraries(vove-batch-rename-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-batch-rename-tests)
    if(MINGW)
        target_link_options(vove-batch-rename-tests PRIVATE -static)
    endif()
    add_test(NAME vove-batch-rename-tests COMMAND vove-batch-rename-tests)

    add_executable(vove-batch-rename-transaction-tests
        tests/fileops/batch_rename_transaction_test.cpp)
    target_link_libraries(vove-batch-rename-transaction-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-batch-rename-transaction-tests)
    if(MINGW)
        target_link_options(vove-batch-rename-transaction-tests PRIVATE -static)
    endif()
    add_test(NAME vove-batch-rename-transaction-tests
        COMMAND vove-batch-rename-transaction-tests)

    add_executable(vove-permanent-delete-transaction-tests
        tests/fileops/permanent_delete_transaction_test.cpp)
    target_link_libraries(vove-permanent-delete-transaction-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-permanent-delete-transaction-tests)
    if(MINGW)
        target_link_options(vove-permanent-delete-transaction-tests PRIVATE -static)
    endif()
    add_test(NAME vove-permanent-delete-transaction-tests
        COMMAND vove-permanent-delete-transaction-tests)

    add_executable(vove-trash-transaction-tests
        tests/fileops/trash_transaction_test.cpp)
    target_link_libraries(vove-trash-transaction-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-trash-transaction-tests)
    if(MINGW)
        target_link_options(vove-trash-transaction-tests PRIVATE -static)
    endif()
    add_test(NAME vove-trash-transaction-tests COMMAND vove-trash-transaction-tests)

    add_executable(vove-file-transfer-transaction-tests
        tests/fileops/file_transfer_transaction_test.cpp)
    target_link_libraries(vove-file-transfer-transaction-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-file-transfer-transaction-tests)
    if(MINGW)
        target_link_options(vove-file-transfer-transaction-tests PRIVATE -static)
    endif()
    add_test(NAME vove-file-transfer-transaction-tests
        COMMAND vove-file-transfer-transaction-tests)

    add_executable(vove-directory-leaf-transfer-tests
        tests/fileops/directory_leaf_transfer_test.cpp)
    target_link_libraries(vove-directory-leaf-transfer-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-directory-leaf-transfer-tests)
    if(MINGW)
        target_link_options(vove-directory-leaf-transfer-tests PRIVATE -static)
    endif()
    add_test(NAME vove-directory-leaf-transfer-tests
        COMMAND vove-directory-leaf-transfer-tests)

    add_executable(vove-basic-directory-transfer-tests
        tests/fileops/basic_directory_transfer_test.cpp)
    target_link_libraries(vove-basic-directory-transfer-tests PRIVATE vove::fileops)
    target_compile_definitions(vove-basic-directory-transfer-tests PRIVATE
        VOVE_BASIC_DIRECTORY_TRANSFER_TEST_HOOKS=1)
    vove_enable_warnings(vove-basic-directory-transfer-tests)
    if(MINGW)
        target_link_options(vove-basic-directory-transfer-tests PRIVATE -static)
    endif()
    add_test(NAME vove-basic-directory-transfer-tests
        COMMAND vove-basic-directory-transfer-tests)

    add_executable(vove-directory-root-staging-tests
        tests/fileops/directory_root_staging_test.cpp)
    target_link_libraries(vove-directory-root-staging-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-directory-root-staging-tests)
    if(MINGW)
        target_link_options(vove-directory-root-staging-tests PRIVATE -static)
    endif()
    add_test(NAME vove-directory-root-staging-tests
        COMMAND vove-directory-root-staging-tests)

    add_executable(vove-directory-transfer-manifest-tests
        tests/fileops/directory_transfer_manifest_test.cpp)
    target_include_directories(vove-directory-transfer-manifest-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops)
    target_link_libraries(vove-directory-transfer-manifest-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-directory-transfer-manifest-tests)
    if(MINGW)
        target_link_options(vove-directory-transfer-manifest-tests PRIVATE -static)
    endif()
    add_test(NAME vove-directory-transfer-manifest-tests
        COMMAND vove-directory-transfer-manifest-tests)

    add_executable(vove-directory-transfer-manifest-store-tests
        tests/fileops/directory_transfer_manifest_store_test.cpp)
    target_link_libraries(vove-directory-transfer-manifest-store-tests PRIVATE vove::fileops)
    target_include_directories(vove-directory-transfer-manifest-store-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops)
    vove_enable_warnings(vove-directory-transfer-manifest-store-tests)
    if(MINGW)
        target_link_options(vove-directory-transfer-manifest-store-tests PRIVATE -static)
    endif()
    add_test(NAME vove-directory-transfer-manifest-store-tests
        COMMAND vove-directory-transfer-manifest-store-tests)

    add_executable(vove-directory-transfer-control-tests
        tests/fileops/directory_transfer_control_test.cpp)
    target_link_libraries(vove-directory-transfer-control-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-directory-transfer-control-tests)
    if(MINGW)
        target_link_options(vove-directory-transfer-control-tests PRIVATE -static)
    endif()
    add_test(NAME vove-directory-transfer-control-tests
        COMMAND vove-directory-transfer-control-tests)

    add_executable(vove-directory-transfer-progress-tests
        tests/fileops/directory_transfer_progress_test.cpp)
    target_include_directories(vove-directory-transfer-progress-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops)
    target_link_libraries(vove-directory-transfer-progress-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-directory-transfer-progress-tests)
    if(MINGW)
        target_link_options(vove-directory-transfer-progress-tests PRIVATE -static)
    endif()
    add_test(NAME vove-directory-transfer-progress-tests
        COMMAND vove-directory-transfer-progress-tests)

    add_executable(vove-directory-transfer-progress-store-tests
        tests/fileops/directory_transfer_progress_store_test.cpp)
    target_include_directories(vove-directory-transfer-progress-store-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops)
    target_link_libraries(vove-directory-transfer-progress-store-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-directory-transfer-progress-store-tests)
    if(MINGW)
        target_link_options(vove-directory-transfer-progress-store-tests PRIVATE -static)
    endif()
    add_test(NAME vove-directory-transfer-progress-store-tests
        COMMAND vove-directory-transfer-progress-store-tests)

    add_executable(vove-file-transfer-protocol-tests
        tests/fileops/file_transfer_protocol_test.cpp)
    target_link_libraries(vove-file-transfer-protocol-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-file-transfer-protocol-tests)
    if(MINGW)
        target_link_options(vove-file-transfer-protocol-tests PRIVATE -static)
    endif()
    add_test(NAME vove-file-transfer-protocol-tests
        COMMAND vove-file-transfer-protocol-tests)

    if(UNIX)
        add_executable(vove-posix-file-transfer-executor-tests
            tests/fileops/posix_file_transfer_executor_test.cpp)
        target_include_directories(vove-posix-file-transfer-executor-tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops)
        target_link_libraries(vove-posix-file-transfer-executor-tests PRIVATE
            vove-fileop-executor-testable)
        vove_enable_warnings(vove-posix-file-transfer-executor-tests)
        add_test(NAME vove-posix-file-transfer-executor-tests
            COMMAND vove-posix-file-transfer-executor-tests)

        add_executable(vove-posix-file-transfer-service-tests
            tests/fileops/posix_file_transfer_service_test.cpp)
        target_compile_definitions(vove-posix-file-transfer-service-tests PRIVATE
            "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\"")
        target_link_libraries(vove-posix-file-transfer-service-tests PRIVATE vove::fileops)
        add_dependencies(vove-posix-file-transfer-service-tests vove-fileop-helper)
        vove_enable_warnings(vove-posix-file-transfer-service-tests)
        add_test(NAME vove-posix-file-transfer-service-tests
            COMMAND vove-posix-file-transfer-service-tests)

        add_executable(vove-posix-file-transfer-coordinator-tests
            tests/fileops/posix_file_transfer_coordinator_test.cpp)
        target_compile_definitions(vove-posix-file-transfer-coordinator-tests PRIVATE
            "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\"")
        target_link_libraries(vove-posix-file-transfer-coordinator-tests PRIVATE vove::fileops)
        add_dependencies(vove-posix-file-transfer-coordinator-tests vove-fileop-helper)
        vove_enable_warnings(vove-posix-file-transfer-coordinator-tests)
        add_test(NAME vove-posix-file-transfer-coordinator-tests
            COMMAND vove-posix-file-transfer-coordinator-tests)
    endif()

    add_executable(vove-basic-directory-transfer-service-tests
        tests/fileops/basic_directory_transfer_service_test.cpp)
    target_compile_definitions(vove-basic-directory-transfer-service-tests PRIVATE
        "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\"")
    target_link_libraries(vove-basic-directory-transfer-service-tests PRIVATE vove::fileops)
    add_dependencies(vove-basic-directory-transfer-service-tests vove-fileop-helper)
    vove_enable_warnings(vove-basic-directory-transfer-service-tests)
    if(MINGW)
        target_link_options(vove-basic-directory-transfer-service-tests PRIVATE -static)
    endif()
    add_test(NAME vove-basic-directory-transfer-service-tests
        COMMAND vove-basic-directory-transfer-service-tests)

    if(WIN32)
        add_executable(vove-fileop-helper-test src/fileops/helper/main.cpp)
        target_include_directories(vove-fileop-helper-test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
        target_link_libraries(vove-fileop-helper-test PRIVATE vove-fileop-executor-testable)
        vove_enable_warnings(vove-fileop-helper-test)
        if(MINGW)
            target_link_options(vove-fileop-helper-test PRIVATE -static)
        endif()

        add_executable(vove-file-transfer-helper-fixture
            tests/fileops/file_transfer_helper_fixture.cpp)
        target_include_directories(vove-file-transfer-helper-fixture PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
        target_link_libraries(vove-file-transfer-helper-fixture PRIVATE vove::fileops)
        vove_enable_warnings(vove-file-transfer-helper-fixture)
        if(MINGW)
            target_link_options(vove-file-transfer-helper-fixture PRIVATE -static)
        endif()

        add_executable(vove-file-transfer-helper-tests
            tests/fileops/file_transfer_helper_test.cpp)
        target_include_directories(vove-file-transfer-helper-tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
        target_compile_definitions(vove-file-transfer-helper-tests PRIVATE
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
            "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\""
            "VOVE_TEST_FILEOP_TEST_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper-test>\"")
        target_link_libraries(vove-file-transfer-helper-tests PRIVATE vove::fileops)
        add_dependencies(vove-file-transfer-helper-tests vove-fileop-helper
            vove-fileop-helper-test)
        vove_enable_warnings(vove-file-transfer-helper-tests)
        if(MINGW)
            target_link_options(vove-file-transfer-helper-tests PRIVATE -static)
        endif()
        add_test(NAME vove-file-transfer-helper-tests
            COMMAND vove-file-transfer-helper-tests)

        add_executable(vove-file-transfer-service-tests
            tests/fileops/file_transfer_service_test.cpp)
        target_include_directories(vove-file-transfer-service-tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
        target_compile_definitions(vove-file-transfer-service-tests PRIVATE
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
            "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\""
            "VOVE_TEST_TRANSFER_FIXTURE_PATH=\"$<TARGET_FILE:vove-file-transfer-helper-fixture>\"")
        target_link_libraries(vove-file-transfer-service-tests PRIVATE vove::fileops)
        add_dependencies(vove-file-transfer-service-tests vove-fileop-helper
            vove-file-transfer-helper-fixture)
        vove_enable_warnings(vove-file-transfer-service-tests)
        if(MINGW)
            target_link_options(vove-file-transfer-service-tests PRIVATE -static)
        endif()
        add_test(NAME vove-file-transfer-service-tests
            COMMAND vove-file-transfer-service-tests)
    endif()

    add_executable(vove-sha256-tests tests/fileops/sha256_test.cpp)
    target_include_directories(vove-sha256-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops)
    target_link_libraries(vove-sha256-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-sha256-tests)
    if(MINGW)
        target_link_options(vove-sha256-tests PRIVATE -static)
    endif()
    add_test(NAME vove-sha256-tests COMMAND vove-sha256-tests)

    add_executable(vove-trash-catalog-tests
        tests/fileops/trash_catalog_test.cpp)
    target_include_directories(vove-trash-catalog-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops)
    target_compile_definitions(vove-trash-catalog-tests PRIVATE
        VOVE_TRASH_CATALOG_TEST_HOOKS=1)
    target_link_libraries(vove-trash-catalog-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-trash-catalog-tests)
    if(MINGW)
        target_link_options(vove-trash-catalog-tests PRIVATE -static)
    endif()
    add_test(NAME vove-trash-catalog-tests COMMAND vove-trash-catalog-tests)

    add_executable(vove-trash-coordinator-tests
        tests/fileops/trash_coordinator_test.cpp)
    target_include_directories(vove-trash-coordinator-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops)
    target_link_libraries(vove-trash-coordinator-tests PRIVATE vove::fileops)
    target_compile_definitions(vove-trash-coordinator-tests PRIVATE
        VOVE_TRASH_COORDINATOR_TEST_HOOKS=1
        "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\""
        "VOVE_TEST_FILEOP_FIXTURE_PATH=\"$<TARGET_FILE:vove-fileop-helper-fixture>\"")
    add_dependencies(vove-trash-coordinator-tests vove-fileop-helper
        vove-fileop-helper-fixture)
    vove_enable_warnings(vove-trash-coordinator-tests)
    if(MINGW)
        target_link_options(vove-trash-coordinator-tests PRIVATE -static)
    endif()
    add_test(NAME vove-trash-coordinator-tests COMMAND vove-trash-coordinator-tests)

    add_executable(vove-permanent-delete-coordinator-tests
        tests/fileops/permanent_delete_coordinator_test.cpp)
    target_link_libraries(vove-permanent-delete-coordinator-tests PRIVATE vove::fileops)
    target_compile_definitions(vove-permanent-delete-coordinator-tests PRIVATE
        "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\""
        "VOVE_TEST_FILEOP_FIXTURE_PATH=\"$<TARGET_FILE:vove-fileop-helper-fixture>\"")
    add_dependencies(vove-permanent-delete-coordinator-tests vove-fileop-helper)
    vove_enable_warnings(vove-permanent-delete-coordinator-tests)
    if(MINGW)
        target_link_options(vove-permanent-delete-coordinator-tests PRIVATE -static)
    endif()
    add_test(NAME vove-permanent-delete-coordinator-tests
        COMMAND vove-permanent-delete-coordinator-tests)

    add_executable(vove-durable-journal-tests
        tests/fileops/durable_journal_test.cpp)
    target_include_directories(vove-durable-journal-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops)
    target_link_libraries(vove-durable-journal-tests PRIVATE vove::fileops)
    vove_enable_warnings(vove-durable-journal-tests)
    if(MINGW)
        target_link_options(vove-durable-journal-tests PRIVATE -static)
    endif()
    add_test(NAME vove-durable-journal-tests COMMAND vove-durable-journal-tests)

    add_executable(vove-batch-rename-coordinator-tests
        tests/fileops/batch_rename_coordinator_test.cpp)
    target_link_libraries(vove-batch-rename-coordinator-tests PRIVATE vove::fileops)
    target_compile_definitions(vove-batch-rename-coordinator-tests PRIVATE
        "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\"")
    add_dependencies(vove-batch-rename-coordinator-tests vove-fileop-helper)
    vove_enable_warnings(vove-batch-rename-coordinator-tests)
    if(MINGW)
        target_link_options(vove-batch-rename-coordinator-tests PRIVATE -static)
    endif()
    add_test(NAME vove-batch-rename-coordinator-tests
        COMMAND vove-batch-rename-coordinator-tests)

    add_executable(vove-fileop-helper-fixture
        tests/fileops/file_operation_helper_fixture.cpp)
    target_include_directories(vove-fileop-helper-fixture PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
    target_link_libraries(vove-fileop-helper-fixture PRIVATE vove::fileop_executor)
    vove_enable_warnings(vove-fileop-helper-fixture)
    if(MINGW)
        target_link_options(vove-fileop-helper-fixture PRIVATE -static)
    endif()
    target_compile_definitions(vove-batch-rename-coordinator-tests PRIVATE
        "VOVE_TEST_FILEOP_FIXTURE_PATH=\"$<TARGET_FILE:vove-fileop-helper-fixture>\"")
    add_dependencies(vove-batch-rename-coordinator-tests vove-fileop-helper-fixture)
    add_dependencies(vove-permanent-delete-coordinator-tests vove-fileop-helper-fixture)

    if(WIN32)
        add_executable(vove-fileop-helper-testable src/fileops/helper/main.cpp)
        target_include_directories(vove-fileop-helper-testable PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/fileops
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
        target_link_libraries(vove-fileop-helper-testable PRIVATE
            vove-fileop-executor-testable)
        vove_enable_warnings(vove-fileop-helper-testable)
        if(MINGW)
            target_link_options(vove-fileop-helper-testable PRIVATE -static)
        endif()

        add_executable(vove-file-transfer-coordinator-tests
            tests/fileops/file_transfer_coordinator_test.cpp)
        target_include_directories(vove-file-transfer-coordinator-tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
        target_compile_definitions(vove-file-transfer-coordinator-tests PRIVATE
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00
            "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\""
            "VOVE_TEST_FILEOP_HELPER_TESTABLE_PATH=\"$<TARGET_FILE:vove-fileop-helper-testable>\""
            "VOVE_TEST_FILEOP_FIXTURE_PATH=\"$<TARGET_FILE:vove-fileop-helper-fixture>\"")
        target_link_libraries(vove-file-transfer-coordinator-tests PRIVATE vove::fileops)
        add_dependencies(vove-file-transfer-coordinator-tests vove-fileop-helper
            vove-fileop-helper-testable vove-fileop-helper-fixture)
        vove_enable_warnings(vove-file-transfer-coordinator-tests)
        if(MINGW)
            target_link_options(vove-file-transfer-coordinator-tests PRIVATE -static)
        endif()
        add_test(NAME vove-file-transfer-coordinator-tests
            COMMAND vove-file-transfer-coordinator-tests)
    endif()

    add_executable(vove-file-operation-service-tests
        tests/fileops/file_operation_service_test.cpp)
    target_link_libraries(vove-file-operation-service-tests PRIVATE vove::fileops)
    target_compile_definitions(vove-file-operation-service-tests PRIVATE
        "VOVE_TEST_FILEOP_HELPER_PATH=\"$<TARGET_FILE:vove-fileop-helper>\""
        "VOVE_TEST_FILEOP_FIXTURE_PATH=\"$<TARGET_FILE:vove-fileop-helper-fixture>\"")
    add_dependencies(vove-file-operation-service-tests
        vove-fileop-helper vove-fileop-helper-fixture)
    vove_enable_warnings(vove-file-operation-service-tests)
    if(MINGW)
        target_link_options(vove-file-operation-service-tests PRIVATE -static)
    endif()
    add_test(NAME vove-file-operation-service-tests COMMAND vove-file-operation-service-tests)

    add_executable(vove-catalog-helper-fixture tests/platform/catalog_helper_fixture.cpp)
    target_include_directories(vove-catalog-helper-fixture PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
    target_link_libraries(vove-catalog-helper-fixture PRIVATE vove::platform)
    vove_enable_warnings(vove-catalog-helper-fixture)
    if(MINGW)
        target_link_options(vove-catalog-helper-fixture PRIVATE -static)
    endif()
    add_dependencies(vove-directory-service-tests
        vove-catalog-helper
        vove-catalog-helper-fixture)
    target_compile_definitions(vove-directory-service-tests PRIVATE
        "VOVE_TEST_CATALOG_HELPER_PATH=\"$<TARGET_FILE:vove-catalog-helper>\""
        "VOVE_TEST_HANG_HELPER_PATH=\"$<TARGET_FILE:vove-catalog-helper-fixture>\"")
endif()

if(VOVE_BUILD_QT_APP)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Gui LinguistTools Network Widgets)
    if(BUILD_TESTING AND VOVE_BUILD_TESTS)
        find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    endif()
    function(vove_set_qt_test_runtime test_name)
        if(WIN32)
            get_filename_component(_vove_cxx_runtime_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
            add_custom_command(TARGET ${test_name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_vove_cxx_runtime_dir}/libc++.dll"
                    "${_vove_cxx_runtime_dir}/libunwind.dll"
                    $<TARGET_FILE_DIR:${test_name}>
                VERBATIM)
            set_tests_properties(${test_name} PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>\;PATH=path_list_prepend:${_vove_cxx_runtime_dir}")
        endif()
    endfunction()
    function(vove_set_qt_image_plugins test_name)
        if(TARGET Qt6::QTiffPlugin)
            set_tests_properties(${test_name} PROPERTIES
                ENVIRONMENT "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::QTiffPlugin>/..")
        endif()
    endfunction()
    set(_vove_xcf_vendor_files
        "gimp_p.h|a3cdd304cd4cba61b9d2551a9818744ba07039d1aa432b1003b00da9fa1795a3"
        "util_p.h|2afe3aa5c77e29ff928e02f5c3e93e3c31f9d2180e37b3f878d3a7d292017068"
        "xcf_p.h|c9c56a16d9c3e18026af10d6dd5367d4630961d6302745452674ac824886c42a"
        "xcf.cpp|8dcb867c407f1d2be3a823a897290e186ba541dc04bff7763afcec514f68d9ba"
        "LICENSES/LGPL-2.0-or-later.txt|de588a8b1c41fe73ffe1201f9d12c718a988ed8e1302929625a6e7c2bced7461"
        "LICENSES/LGPL-2.1-or-later.txt|1ccf09bf2f598308df4bed9cd8e9657dc5cd0973d2800318f2e241486e2edf3f")
    foreach(_vove_xcf_vendor_entry IN LISTS _vove_xcf_vendor_files)
        string(REPLACE "|" ";" _vove_xcf_vendor_pair "${_vove_xcf_vendor_entry}")
        list(GET _vove_xcf_vendor_pair 0 _vove_xcf_vendor_path)
        list(GET _vove_xcf_vendor_pair 1 _vove_xcf_vendor_expected)
        file(READ
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/kimageformats-xcf/${_vove_xcf_vendor_path}"
            _vove_xcf_vendor_contents)
        string(REPLACE "\r\n" "\n" _vove_xcf_vendor_contents "${_vove_xcf_vendor_contents}")
        string(SHA256 _vove_xcf_vendor_actual "${_vove_xcf_vendor_contents}")
        if(NOT _vove_xcf_vendor_actual STREQUAL _vove_xcf_vendor_expected)
            message(FATAL_ERROR
                "Pinned KImageFormats XCF source drift: ${_vove_xcf_vendor_path}")
        endif()
    endforeach()
    unset(_vove_xcf_vendor_files)
    unset(_vove_xcf_vendor_entry)
    unset(_vove_xcf_vendor_pair)
    unset(_vove_xcf_vendor_path)
    unset(_vove_xcf_vendor_expected)
    unset(_vove_xcf_vendor_actual)
    unset(_vove_xcf_vendor_contents)
    add_library(vove-xcf-preview STATIC
        src/handlers/xcf/kimageformats_xcf_decoder.cpp
        third_party/kimageformats-xcf/xcf.cpp)
    target_include_directories(vove-xcf-preview
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE third_party/kimageformats-xcf)
    target_compile_features(vove-xcf-preview PUBLIC cxx_std_20)
    target_link_libraries(vove-xcf-preview
        PUBLIC vove::raster_base Qt6::Gui
        PRIVATE vove-miniz-inflate)
    add_library(vove::xcf_preview ALIAS vove-xcf-preview)
    if(BUILD_TESTING AND VOVE_BUILD_TESTS)
        add_executable(vove-xcf-preview-tests tests/handlers/xcf_decoder_test.cpp)
        target_link_libraries(vove-xcf-preview-tests PRIVATE vove::xcf_preview)
        target_compile_definitions(vove-xcf-preview-tests PRIVATE
            VOVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        vove_enable_warnings(vove-xcf-preview-tests)
        add_test(NAME vove-xcf-preview-tests COMMAND vove-xcf-preview-tests)
        vove_set_qt_test_runtime(vove-xcf-preview-tests)
        set_tests_properties(vove-xcf-preview-tests PROPERTIES
            TIMEOUT 30
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    if(VOVE_HEIC_DECODE_ROOT STREQUAL "")
        if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/out/heic-decode-win/install")
            set(VOVE_HEIC_DECODE_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/out/heic-decode-win/install")
        elseif(UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/out/heic-decode-linux/install")
            set(VOVE_HEIC_DECODE_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/out/heic-decode-linux/install")
        else()
            message(FATAL_ERROR
                "Build the pinned HEIC contour from cmake/stage4 and set VOVE_HEIC_DECODE_ROOT")
        endif()
    endif()
    find_package(vove-heic-decode CONFIG REQUIRED
        PATHS "${VOVE_HEIC_DECODE_ROOT}/lib/cmake/vove-heic-decode"
        NO_DEFAULT_PATH)
    if(VOVE_HEIC_TEST_SOURCE_ROOT STREQUAL "")
        if(WIN32)
            set(VOVE_HEIC_TEST_SOURCE_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/out/heic-decode-win/source/libheif")
        else()
            set(VOVE_HEIC_TEST_SOURCE_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/out/heic-decode-linux/source/libheif")
        endif()
    endif()
    if(VOVE_WEBP_DECODE_ROOT STREQUAL "")
        if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/out/webp-decode-win/install")
            set(VOVE_WEBP_DECODE_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/out/webp-decode-win/install")
        elseif(UNIX AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/out/webp-decode-linux/install")
            set(VOVE_WEBP_DECODE_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/out/webp-decode-linux/install")
        else()
            message(FATAL_ERROR
                "Build the pinned WebP contour from cmake/stage4-webp and set VOVE_WEBP_DECODE_ROOT")
        endif()
    endif()
    find_package(vove-webp-decode CONFIG REQUIRED
        PATHS "${VOVE_WEBP_DECODE_ROOT}/lib/cmake/vove-webp-decode"
        NO_DEFAULT_PATH)
    if(VOVE_WEBP_TEST_SOURCE_ROOT STREQUAL "")
        if(WIN32)
            set(VOVE_WEBP_TEST_SOURCE_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/out/webp-decode-win/source/libwebp")
        else()
            set(VOVE_WEBP_TEST_SOURCE_ROOT
                "${CMAKE_CURRENT_SOURCE_DIR}/out/webp-decode-linux/source/libwebp")
        endif()
    endif()

    add_library(vove-raster-heic STATIC
        src/handlers/raster/heic_decoder.cpp)
    add_library(vove::raster_heic ALIAS vove-raster-heic)
    target_include_directories(vove-raster-heic PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(vove-raster-heic PUBLIC cxx_std_20)
    target_link_libraries(vove-raster-heic PUBLIC
        vove::raster_base vove::color vove-heic-decode::heif PRIVATE vove-lcms2)
    vove_enable_warnings(vove-raster-heic)
    if(VOVE_JPEGXL_DECODE_ROOT STREQUAL "")
        message(FATAL_ERROR "Build cmake/jpegxl and set VOVE_JPEGXL_DECODE_ROOT")
    endif()
    find_package(vove-jpegxl-decode CONFIG REQUIRED
        PATHS "${VOVE_JPEGXL_DECODE_ROOT}/lib/cmake/vove-jpegxl-decode" NO_DEFAULT_PATH)
    add_library(vove-raster-jpegxl STATIC src/handlers/raster/jpegxl_decoder.cpp)
    add_library(vove::raster_jpegxl ALIAS vove-raster-jpegxl)
    target_include_directories(vove-raster-jpegxl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(vove-raster-jpegxl PUBLIC cxx_std_20)
    target_link_libraries(vove-raster-jpegxl PUBLIC vove::raster_base
        PRIVATE vove::color vove::preview vove-lcms2 vove-jpegxl-decode::jxl_dec)
    vove_enable_warnings(vove-raster-jpegxl)
    add_library(vove-raster-webp STATIC
        src/handlers/raster/webp_decoder.cpp)
    add_library(vove::raster_webp ALIAS vove-raster-webp)
    target_include_directories(vove-raster-webp PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(vove-raster-webp PUBLIC cxx_std_20)
    target_link_libraries(vove-raster-webp PUBLIC
        vove::raster_base vove::color
        vove-webp-decode::webp vove-webp-decode::webpdemux)
    vove_enable_warnings(vove-raster-webp)
    add_library(vove-raster-qt STATIC
        src/handlers/raster/qt_raster_decoder.cpp
    )
    add_library(vove::raster_qt ALIAS vove-raster-qt)
    target_include_directories(vove-raster-qt PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(vove-raster-qt PUBLIC cxx_std_20)
    target_link_libraries(vove-raster-qt PUBLIC vove::raster_base vove::color Qt6::Gui)
    vove_enable_warnings(vove-raster-qt)

    add_library(vove-raster-executor STATIC
        src/worker/raster_executor.cpp
    )
    target_include_directories(vove-raster-executor
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/worker)
    target_link_libraries(vove-raster-executor
        PUBLIC vove::worker_protocol
        PRIVATE vove::color vove::qoi_codec vove::raster_psd vove::raster_raw_preview
            vove::raster_jpegxl)
    if(WIN32)
        target_link_libraries(vove-raster-executor PRIVATE
            vove::raster_wic vove::raster_qt vove::raster_heic vove::raster_webp)
    else()
        target_link_libraries(vove-raster-executor PRIVATE
            vove::raster_qt vove::raster_heic vove::raster_webp)
    endif()
    target_compile_features(vove-raster-executor PUBLIC cxx_std_20)
    vove_enable_warnings(vove-raster-executor)

    add_executable(vove-raster-worker src/worker/raster_main.cpp)
    target_include_directories(vove-raster-worker PRIVATE src/worker)
    target_link_libraries(vove-raster-worker PRIVATE
        vove-raster-executor vove::worker_runtime Qt6::Core)
    if(WIN32)
        add_custom_command(TARGET vove-raster-worker POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:vove-raster-worker>/imageformats"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::QJpegPlugin>"
                "$<TARGET_FILE_DIR:vove-raster-worker>/imageformats/$<TARGET_FILE_NAME:Qt6::QJpegPlugin>"
            VERBATIM)
    endif()
    vove_enable_warnings(vove-raster-worker)
    if(MINGW)
        target_link_options(vove-raster-worker PRIVATE -static)
    endif()

    add_library(vove-xcf-executor STATIC src/worker/xcf_executor.cpp)
    target_include_directories(vove-xcf-executor PUBLIC src/worker)
    target_link_libraries(vove-xcf-executor
        PUBLIC vove::worker_protocol
        PRIVATE vove::qoi_codec vove::xcf_preview)
    target_compile_features(vove-xcf-executor PUBLIC cxx_std_20)
    vove_enable_warnings(vove-xcf-executor)

    add_executable(vove-xcf-worker src/worker/xcf_main.cpp)
    target_include_directories(vove-xcf-worker PRIVATE src/worker)
    target_link_libraries(vove-xcf-worker PRIVATE vove-xcf-executor vove::worker_runtime)
    vove_enable_warnings(vove-xcf-worker)
    vove_copy_cxx_runtime(vove-xcf-worker)
    if(WIN32)
        add_custom_command(TARGET vove-xcf-worker POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Qt6::Core>
                $<TARGET_FILE:Qt6::Gui>
                $<TARGET_FILE_DIR:vove-xcf-worker>
            VERBATIM)
        if(BUILD_TESTING AND VOVE_BUILD_TESTS)
            add_custom_command(TARGET vove-xcf-worker POST_BUILD
                COMMAND icacls.exe $<TARGET_FILE:vove-xcf-worker>
                    /grant "*S-1-15-2-1:(RX)"
                COMMAND icacls.exe $<TARGET_FILE:Qt6::Core>
                    /grant "*S-1-15-2-1:(RX)"
                COMMAND icacls.exe $<TARGET_FILE:Qt6::Gui>
                    /grant "*S-1-15-2-1:(RX)"
                COMMAND icacls.exe $<TARGET_FILE_DIR:vove-xcf-worker>/libc++.dll
                    /grant "*S-1-15-2-1:(RX)"
                COMMAND icacls.exe $<TARGET_FILE_DIR:vove-xcf-worker>/libunwind.dll
                    /grant "*S-1-15-2-1:(RX)"
                VERBATIM)
        endif()
    endif()

    if(VOVE_BUILD_BENCHMARKS)
        add_executable(vove-raw-corpus-bench src/bench/raw_corpus_bench.cpp)
        target_include_directories(vove-raw-corpus-bench PRIVATE src/worker)
        target_compile_definitions(vove-raw-corpus-bench PRIVATE
            "VOVE_RAW_CORPUS_WORKER_NAME=\"$<TARGET_FILE_NAME:vove-raster-worker>\"")
        target_link_libraries(vove-raw-corpus-bench PRIVATE
            vove::cache vove::raster_base vove::raster_raw_preview vove::worker_supervisor)
        add_dependencies(vove-raw-corpus-bench vove-raster-worker)
        vove_enable_warnings(vove-raw-corpus-bench)
    endif()

    add_library(vove-pugixml STATIC third_party/pugixml/pugixml.cpp)
    target_include_directories(vove-pugixml SYSTEM PUBLIC third_party/pugixml)
    target_compile_definitions(vove-pugixml PUBLIC PUGIXML_NO_XPATH PUGIXML_NO_EXCEPTIONS)
    target_compile_features(vove-pugixml PUBLIC cxx_std_20)

    add_library(vove-publishing-preview STATIC
        src/handlers/indesign/embedded_preview.cpp
        src/handlers/affinity/embedded_preview.cpp)
    target_include_directories(vove-publishing-preview PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(vove-publishing-preview PUBLIC vove::raster_base
        PRIVATE vove-pugixml vove::archive_preview)
    target_compile_features(vove-publishing-preview PUBLIC cxx_std_20)
    vove_enable_warnings(vove-publishing-preview)

    add_library(vove-cdr-executor STATIC
        src/worker/cdr_executor.cpp)
    target_include_directories(vove-cdr-executor PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src/worker)
    target_link_libraries(vove-cdr-executor
        PUBLIC vove::worker_protocol
        PRIVATE vove::cdr_preview vove::dxf_preview vove-publishing-preview vove::qoi_codec)
    if(WIN32)
        target_compile_definitions(vove-cdr-executor PRIVATE
            _WIN32_WINNT=0x0A00
            WINVER=0x0A00)
        target_link_libraries(vove-cdr-executor PRIVATE vove::raster_wic)
    else()
        target_link_libraries(vove-cdr-executor PRIVATE vove::raster_qt Qt6::Gui)
    endif()
    target_compile_features(vove-cdr-executor PUBLIC cxx_std_20)
    vove_enable_warnings(vove-cdr-executor)

    add_executable(vove-cdr-worker src/worker/cdr_main.cpp)
    target_include_directories(vove-cdr-worker PRIVATE src/worker)
    target_link_libraries(vove-cdr-worker PRIVATE
        vove-cdr-executor vove::worker_runtime)
    vove_enable_warnings(vove-cdr-worker)
    if(MINGW)
        target_link_options(vove-cdr-worker PRIVATE -static)
    endif()

    if(VOVE_BUILD_RESVG_SVG_WORKER)
        if(VOVE_RESVG_BRIDGE_LIBRARY STREQUAL "")
            if(WIN32)
                set(VOVE_RESVG_BRIDGE_LIBRARY
                    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/resvg_bridge/target/x86_64-pc-windows-gnullvm/production/libvove_resvg_bridge.a")
            else()
                set(VOVE_RESVG_BRIDGE_LIBRARY
                    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/resvg_bridge/target/production/libvove_resvg_bridge.a")
            endif()
        endif()
        if(NOT EXISTS "${VOVE_RESVG_BRIDGE_LIBRARY}")
            message(FATAL_ERROR
                "Build third_party/resvg_bridge with its pinned production profile and set VOVE_RESVG_BRIDGE_LIBRARY")
        endif()

        add_library(vove-resvg-bridge STATIC IMPORTED GLOBAL)
        set_target_properties(vove-resvg-bridge PROPERTIES
            IMPORTED_LOCATION "${VOVE_RESVG_BRIDGE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/resvg_bridge/include")
        if(WIN32)
            set_property(TARGET vove-resvg-bridge PROPERTY INTERFACE_LINK_LIBRARIES
                "ws2_32;ntdll;userenv;bcrypt;iphlpapi;advapi32;ole32;shell32")
        else()
            set_property(TARGET vove-resvg-bridge PROPERTY INTERFACE_LINK_LIBRARIES
                "Threads::Threads;${CMAKE_DL_LIBS};m")
        endif()

        add_subdirectory(cmake/hp2xx)
        add_library(vove-svg-executor STATIC src/worker/svg_executor.cpp)
        target_include_directories(vove-svg-executor PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/worker)
        target_link_libraries(vove-svg-executor
            PUBLIC vove::worker_protocol
            PRIVATE vove::raster_base vove::qoi_codec vove-resvg-bridge vove-hpgl-preview)
        target_compile_features(vove-svg-executor PUBLIC cxx_std_20)
        vove_enable_warnings(vove-svg-executor)

        add_executable(vove-svg-worker src/worker/svg_main.cpp)
        target_include_directories(vove-svg-worker PRIVATE src/worker)
        target_link_libraries(vove-svg-worker PRIVATE
            vove-svg-executor vove::worker_runtime)
        vove_enable_warnings(vove-svg-worker)
        if(MINGW)
            target_link_options(vove-svg-worker PRIVATE -static)
        endif()

        if(BUILD_TESTING AND VOVE_BUILD_TESTS)
            add_executable(vove-svg-executor-tests tests/worker/svg_executor_test.cpp)
            target_include_directories(vove-svg-executor-tests PRIVATE src/worker)
            target_link_libraries(vove-svg-executor-tests PRIVATE
                vove-svg-executor vove::qoi_codec)
            vove_enable_warnings(vove-svg-executor-tests)
            add_test(NAME vove-svg-executor-tests COMMAND vove-svg-executor-tests)
        endif()
    endif()

    if(VOVE_BUILD_BENCHMARKS)
        add_executable(vove-cdr-corpus-bench src/bench/cdr_corpus_bench.cpp)
        target_include_directories(vove-cdr-corpus-bench PRIVATE src/worker)
        target_compile_definitions(vove-cdr-corpus-bench PRIVATE
            "VOVE_CDR_CORPUS_WORKER_PATH=\"$<TARGET_FILE:vove-cdr-worker>\"")
        target_link_libraries(vove-cdr-corpus-bench PRIVATE
            vove::cdr_preview vove::cache vove::worker_supervisor)
        add_dependencies(vove-cdr-corpus-bench vove-cdr-worker)
        vove_enable_warnings(vove-cdr-corpus-bench)
    endif()

    if(VOVE_BUILD_MUPDF_DOCUMENT_WORKER)
        foreach(required_path IN ITEMS
                VOVE_MUPDF_SOURCE_ROOT VOVE_MUPDF_SOURCE_ARCHIVE
                VOVE_MUPDF_LIBRARY VOVE_MUPDF_THIRD_LIBRARY)
            if(NOT ${required_path} OR NOT EXISTS "${${required_path}}")
                message(FATAL_ERROR
                    "${required_path} must name an existing MuPDF production contour path")
            endif()
        endforeach()

        vove_verify_pinned_mupdf_contour(
            "${VOVE_MUPDF_SOURCE_ROOT}"
            "${VOVE_MUPDF_SOURCE_ARCHIVE}"
            "${VOVE_MUPDF_LIBRARY}"
            "${VOVE_MUPDF_THIRD_LIBRARY}")

        add_custom_target(vove-verify-mupdf-contour ALL
            COMMAND ${CMAKE_COMMAND}
                "-DVOVE_MUPDF_SOURCE_ROOT=${VOVE_MUPDF_SOURCE_ROOT}"
                "-DVOVE_MUPDF_SOURCE_ARCHIVE=${VOVE_MUPDF_SOURCE_ARCHIVE}"
                "-DVOVE_MUPDF_LIBRARY=${VOVE_MUPDF_LIBRARY}"
                "-DVOVE_MUPDF_THIRD_LIBRARY=${VOVE_MUPDF_THIRD_LIBRARY}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyMuPdfContour.cmake"
            COMMENT "Verifying the pinned MuPDF contour"
            VERBATIM)
        if(BUILD_TESTING AND VOVE_BUILD_TESTS)
            add_test(NAME vove-mupdf-contour-lock-post-configure-mutation
                COMMAND ${CMAKE_COMMAND}
                    -DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/mupdf-build-time-lock-test
                    -DTEST_SAFE_PARENT=${CMAKE_CURRENT_BINARY_DIR}
                    -DSOURCE_ROOT=${VOVE_MUPDF_SOURCE_ROOT}
                    -DSOURCE_ARCHIVE=${VOVE_MUPDF_SOURCE_ARCHIVE}
                    -DLIBRARY=${VOVE_MUPDF_LIBRARY}
                    -DTHIRD_LIBRARY=${VOVE_MUPDF_THIRD_LIBRARY}
                    -DVERIFY_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyMuPdfContour.cmake
                    "-DGENERATOR=${CMAKE_GENERATOR}"
                    -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/MuPdfBuildTimeLockTest.cmake)
        endif()

        add_library(vove-mupdf STATIC IMPORTED GLOBAL)
        set_target_properties(vove-mupdf PROPERTIES
            IMPORTED_LOCATION "${VOVE_MUPDF_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${VOVE_MUPDF_SOURCE_ROOT}/include")
        add_library(vove-mupdf-third STATIC IMPORTED GLOBAL)
        set_target_properties(vove-mupdf-third PROPERTIES
            IMPORTED_LOCATION "${VOVE_MUPDF_THIRD_LIBRARY}")

        add_library(vove-mupdf-adapter STATIC
            src/handlers/document/mupdf_adapter.c)
        target_include_directories(vove-mupdf-adapter PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/handlers/document)
        target_link_libraries(vove-mupdf-adapter PUBLIC
            vove-mupdf vove-mupdf-third)
        set_target_properties(vove-mupdf-adapter PROPERTIES
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
        add_dependencies(vove-mupdf-adapter vove-verify-mupdf-contour)
        vove_enable_warnings(vove-mupdf-adapter)

        add_library(vove-document-executor STATIC
            src/worker/document_executor.cpp)
        target_include_directories(vove-document-executor PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/worker)
        target_link_libraries(vove-document-executor
            PUBLIC vove::worker_protocol
            PRIVATE vove::qoi_codec vove::preview vove-mupdf-adapter)
        target_compile_features(vove-document-executor PUBLIC cxx_std_20)
        vove_enable_warnings(vove-document-executor)

        add_executable(vove-document-worker src/worker/document_main.cpp)
        target_include_directories(vove-document-worker PRIVATE src/worker)
        target_link_libraries(vove-document-worker PRIVATE
            vove-document-executor vove::worker_runtime)
        vove_enable_warnings(vove-document-worker)
        if(MINGW)
            target_link_options(vove-document-worker PRIVATE -static)
        endif()

        if(VOVE_BUILD_BENCHMARKS)
            add_executable(vove-document-corpus-bench
                src/bench/document_corpus_bench.cpp)
            target_include_directories(vove-document-corpus-bench PRIVATE src/worker)
            target_compile_definitions(vove-document-corpus-bench PRIVATE
                "VOVE_DOCUMENT_CORPUS_WORKER_PATH=\"$<TARGET_FILE:vove-document-worker>\"")
            target_link_libraries(vove-document-corpus-bench PRIVATE
                vove::cache vove::raster_base vove::worker_supervisor)
            add_dependencies(vove-document-corpus-bench vove-document-worker)
            vove_enable_warnings(vove-document-corpus-bench)
            if(MINGW)
                target_link_options(vove-document-corpus-bench PRIVATE -static)
            endif()
            if(BUILD_TESTING AND VOVE_BUILD_TESTS)
                add_test(NAME vove-document-corpus-bench-smoke
                    COMMAND vove-document-corpus-bench)
                set_tests_properties(vove-document-corpus-bench-smoke PROPERTIES
                    ENVIRONMENT
                        "VOVE_DOCUMENT_CORPUS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/worker/fixtures;VOVE_DOCUMENT_CORPUS_AI_LIMIT=4;VOVE_DOCUMENT_CORPUS_PDF_LIMIT=5;VOVE_DOCUMENT_CORPUS_MIN_SUCCESS_PERCENT=40")
                add_test(NAME vove-document-corpus-bench-rejects-undersized
                    COMMAND vove-document-corpus-bench)
                set_tests_properties(vove-document-corpus-bench-rejects-undersized PROPERTIES
                    ENVIRONMENT
                        "VOVE_DOCUMENT_CORPUS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/worker/fixtures;VOVE_DOCUMENT_CORPUS_AI_LIMIT=5;VOVE_DOCUMENT_CORPUS_PDF_LIMIT=5;VOVE_DOCUMENT_CORPUS_MIN_SUCCESS_PERCENT=40"
                    WILL_FAIL TRUE)
                add_test(NAME vove-document-corpus-bench-rejects-low-success
                    COMMAND vove-document-corpus-bench)
                set_tests_properties(vove-document-corpus-bench-rejects-low-success PROPERTIES
                    ENVIRONMENT
                        "VOVE_DOCUMENT_CORPUS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/worker/fixtures;VOVE_DOCUMENT_CORPUS_AI_LIMIT=4;VOVE_DOCUMENT_CORPUS_PDF_LIMIT=5;VOVE_DOCUMENT_CORPUS_MIN_SUCCESS_PERCENT=100"
                    WILL_FAIL TRUE)
            endif()
        endif()

        if(BUILD_TESTING AND VOVE_BUILD_TESTS)
            add_executable(vove-mupdf-adapter-tests tests/handlers/mupdf_adapter_test.c)
            target_link_libraries(vove-mupdf-adapter-tests PRIVATE vove-mupdf vove-mupdf-third)
            set_target_properties(vove-mupdf-adapter-tests PROPERTIES
                C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF LINKER_LANGUAGE CXX)
            add_dependencies(vove-mupdf-adapter-tests vove-verify-mupdf-contour)
            vove_enable_warnings(vove-mupdf-adapter-tests)
            if(MINGW)
                target_link_options(vove-mupdf-adapter-tests PRIVATE -static)
            endif()
            add_test(NAME vove-mupdf-adapter-tests COMMAND vove-mupdf-adapter-tests)
            set_tests_properties(vove-mupdf-adapter-tests PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
            add_executable(vove-document-executor-tests
                tests/worker/document_executor_test.cpp)
            target_include_directories(vove-document-executor-tests PRIVATE
                src/worker src/handlers/document)
            target_link_libraries(vove-document-executor-tests PRIVATE
                vove-document-executor vove::qoi_codec)
            vove_enable_warnings(vove-document-executor-tests)
            add_test(NAME vove-document-executor-tests
                COMMAND vove-document-executor-tests)
            set_tests_properties(vove-document-executor-tests PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
    endif()
    if(BUILD_TESTING AND VOVE_BUILD_TESTS)
        add_executable(vove-archive-preview-tests tests/handlers/archive_preview_test.cpp)
        target_link_libraries(vove-archive-preview-tests PRIVATE vove::archive_preview)
        vove_enable_warnings(vove-archive-preview-tests)
        add_test(NAME vove-archive-preview-tests COMMAND vove-archive-preview-tests)
        vove_set_qt_test_runtime(vove-archive-preview-tests)
        set_tests_properties(vove-archive-preview-tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)

        add_executable(vove-cdr-executor-tests tests/worker/cdr_executor_test.cpp)
        target_include_directories(vove-cdr-executor-tests PRIVATE src/worker)
        target_link_libraries(vove-cdr-executor-tests PRIVATE
            vove-cdr-executor vove::cdr_preview vove::qoi_codec Qt6::Core Qt6::Gui)
        vove_enable_warnings(vove-cdr-executor-tests)
        add_test(NAME vove-cdr-executor-tests COMMAND vove-cdr-executor-tests)
        vove_set_qt_test_runtime(vove-cdr-executor-tests)
        vove_set_qt_image_plugins(vove-cdr-executor-tests)
        set_tests_properties(vove-cdr-executor-tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

        foreach(format IN ITEMS indesign affinity idml)
            add_executable(vove-${format}-preview-tests tests/handlers/${format}_preview_test.cpp)
            target_link_libraries(vove-${format}-preview-tests PRIVATE
                vove-publishing-preview Qt6::Core)
            vove_enable_warnings(vove-${format}-preview-tests)
            add_test(NAME vove-${format}-preview-tests COMMAND vove-${format}-preview-tests)
            vove_set_qt_test_runtime(vove-${format}-preview-tests)
            set_tests_properties(vove-${format}-preview-tests PROPERTIES TIMEOUT 30)
        endforeach()

        add_executable(vove-raster-qt-tests tests/handlers/qt_raster_decoder_test.cpp)
        target_link_libraries(vove-raster-qt-tests PRIVATE vove::raster_qt)
        target_compile_definitions(vove-raster-qt-tests PRIVATE
            VOVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        vove_enable_warnings(vove-raster-qt-tests)
        add_test(NAME vove-raster-qt-tests COMMAND vove-raster-qt-tests)
        vove_set_qt_test_runtime(vove-raster-qt-tests)
        vove_set_qt_image_plugins(vove-raster-qt-tests)

        add_executable(vove-raster-psd-tests tests/handlers/psd_decoder_test.cpp)
        target_link_libraries(vove-raster-psd-tests PRIVATE vove::raster_psd)
        if(WIN32)
            target_link_libraries(vove-raster-psd-tests PRIVATE psapi)
        endif()
        vove_enable_warnings(vove-raster-psd-tests)
        add_test(NAME vove-raster-psd-tests COMMAND vove-raster-psd-tests)
        set_tests_properties(vove-raster-psd-tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

        add_executable(vove-raster-raw-preview-tests
            tests/handlers/raw_preview_decoder_test.cpp)
        target_link_libraries(vove-raster-raw-preview-tests PRIVATE
            vove::raster_raw_preview Qt6::Gui)
        vove_enable_warnings(vove-raster-raw-preview-tests)
        add_test(NAME vove-raster-raw-preview-tests
            COMMAND vove-raster-raw-preview-tests)
        vove_set_qt_test_runtime(vove-raster-raw-preview-tests)
        vove_set_qt_image_plugins(vove-raster-raw-preview-tests)

        add_executable(vove-raster-executor-tests tests/worker/raster_executor_test.cpp)
        target_include_directories(vove-raster-executor-tests PRIVATE src/worker)
        target_link_libraries(vove-raster-executor-tests PRIVATE
            vove-raster-executor vove::qoi_codec Qt6::Gui)
        vove_enable_warnings(vove-raster-executor-tests)
        add_test(NAME vove-raster-executor-tests COMMAND vove-raster-executor-tests)
        vove_set_qt_test_runtime(vove-raster-executor-tests)
        vove_set_qt_image_plugins(vove-raster-executor-tests)

        add_executable(vove-xcf-executor-tests tests/worker/xcf_executor_test.cpp)
        target_include_directories(vove-xcf-executor-tests PRIVATE src/worker)
        target_compile_definitions(vove-xcf-executor-tests PRIVATE
            VOVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(vove-xcf-executor-tests PRIVATE
            vove-xcf-executor vove::qoi_codec Qt6::Core)
        vove_enable_warnings(vove-xcf-executor-tests)
        add_test(NAME vove-xcf-executor-tests COMMAND vove-xcf-executor-tests)
        vove_set_qt_test_runtime(vove-xcf-executor-tests)

        add_executable(vove-raster-jpegxl-tests tests/handlers/jpegxl_decoder_test.cpp)
        target_link_libraries(vove-raster-jpegxl-tests PRIVATE vove::raster_jpegxl)
        vove_enable_warnings(vove-raster-jpegxl-tests)
        add_test(NAME vove-raster-jpegxl-tests COMMAND vove-raster-jpegxl-tests)
        vove_set_qt_test_runtime(vove-raster-jpegxl-tests)
        set_tests_properties(vove-raster-jpegxl-tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)

        add_executable(vove-raster-heic-tests tests/handlers/heic_decoder_test.cpp)
        target_link_libraries(vove-raster-heic-tests PRIVATE vove::raster_heic)
        vove_enable_warnings(vove-raster-heic-tests)
        add_test(NAME vove-raster-heic-tests COMMAND vove-raster-heic-tests)
        vove_set_qt_test_runtime(vove-raster-heic-tests)
        set_tests_properties(vove-raster-heic-tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT
                "VOVE_HEIC_FIXTURE=${VOVE_HEIC_TEST_SOURCE_ROOT}/examples/example.heic;VOVE_HEIC_ALPHA_FIXTURE=${VOVE_HEIC_TEST_SOURCE_ROOT}/tests/data/with-alpha-512x512.heic;VOVE_HEIC_GRAYSCALE_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/handlers/fixtures/compressed-grayscale.heic;VOVE_HEIC_CORPUS_DIR=${VOVE_HEIC_TEST_SOURCE_ROOT}/fuzzing/data/corpus")

        add_executable(vove-raster-webp-tests tests/handlers/webp_decoder_test.cpp)
        target_link_libraries(vove-raster-webp-tests PRIVATE vove::raster_webp)
        vove_enable_warnings(vove-raster-webp-tests)
        add_test(NAME vove-raster-webp-tests COMMAND vove-raster-webp-tests)
        set(VOVE_WEBP_TEST_FIXTURE
            "${VOVE_WEBP_TEST_SOURCE_ROOT}/examples/test.webp")
        set_tests_properties(vove-raster-webp-tests PROPERTIES
            ENVIRONMENT
                "VOVE_WEBP_FIXTURE=${VOVE_WEBP_TEST_FIXTURE}")

        if(WIN32)
            add_executable(vove-raster-wic-tests
                tests/handlers/wic_raster_decoder_test.cpp)
            target_link_libraries(vove-raster-wic-tests PRIVATE
                vove::raster_wic vove-lcms2)
            vove_enable_warnings(vove-raster-wic-tests)
            if(MINGW)
                target_link_options(vove-raster-wic-tests PRIVATE -static)
            endif()
            add_test(NAME vove-raster-wic-tests COMMAND vove-raster-wic-tests)
            vove_set_qt_test_runtime(vove-raster-wic-tests)
        endif()

        add_executable(vove-raster-supervisor-tests
            tests/worker/raster_supervisor_test.cpp)
        target_compile_definitions(vove-raster-supervisor-tests PRIVATE
            "VOVE_TEST_RASTER_WORKER_PATH=\"$<TARGET_FILE:vove-raster-worker>\""
            "VOVE_TEST_HEIC_PATH=\"${VOVE_HEIC_TEST_SOURCE_ROOT}/examples/example.heic\""
            "VOVE_TEST_WEBP_PATH=\"${VOVE_WEBP_TEST_SOURCE_ROOT}/examples/test.webp\"")
        target_link_libraries(vove-raster-supervisor-tests PRIVATE
            vove::worker_supervisor vove::qoi_codec Qt6::Gui)
        add_dependencies(vove-raster-supervisor-tests vove-raster-worker)
        vove_enable_warnings(vove-raster-supervisor-tests)
        add_test(NAME vove-raster-supervisor-tests COMMAND vove-raster-supervisor-tests)
        vove_set_qt_test_runtime(vove-raster-supervisor-tests)
        vove_set_qt_image_plugins(vove-raster-supervisor-tests)
    endif()

    add_library(vove-preview-helper-service STATIC
        src/preview/helper/service.cpp)
    target_include_directories(vove-preview-helper-service
        PUBLIC src/preview/helper)
    target_link_libraries(vove-preview-helper-service PUBLIC
        vove::preview_pipeline vove::preview_protocol Qt6::Core Qt6::Network)
    target_compile_features(vove-preview-helper-service PUBLIC cxx_std_20)
    vove_enable_warnings(vove-preview-helper-service)

    add_executable(vove-preview-helper src/preview/helper/main.cpp)
    target_include_directories(vove-preview-helper PRIVATE src/preview/helper)
    target_link_libraries(vove-preview-helper PRIVATE
        vove-preview-helper-service vove::cache vove::worker_thumbnail_renderer)
    add_dependencies(vove-preview-helper vove-raster-worker vove-xcf-worker vove-cdr-worker)
    if(VOVE_BUILD_MUPDF_DOCUMENT_WORKER)
        target_compile_definitions(vove-preview-helper PRIVATE
            VOVE_HAS_MUPDF_DOCUMENT_WORKER=1)
        add_dependencies(vove-preview-helper vove-document-worker)
    endif()
    if(VOVE_BUILD_RESVG_SVG_WORKER)
        target_compile_definitions(vove-preview-helper PRIVATE
            VOVE_HAS_RESVG_SVG_WORKER=1)
        add_dependencies(vove-preview-helper vove-svg-worker)
    endif()
    vove_enable_warnings(vove-preview-helper)

    if(BUILD_TESTING AND VOVE_BUILD_TESTS)
        add_executable(vove-test-delayed-raster-worker
            tests/worker/delayed_raster_worker.cpp)
        target_compile_definitions(vove-test-delayed-raster-worker PRIVATE VOVE_TEST_DELAY_MS=1000)
        target_include_directories(vove-test-delayed-raster-worker PRIVATE src/worker)
        target_link_libraries(vove-test-delayed-raster-worker PRIVATE
            vove-raster-executor vove::worker_runtime)
        vove_enable_warnings(vove-test-delayed-raster-worker)

        if(WIN32)
            add_executable(vove-test-slow-raster-worker
                tests/worker/delayed_raster_worker.cpp)
            target_compile_definitions(vove-test-slow-raster-worker PRIVATE VOVE_TEST_DELAY_MS=5000)
            target_include_directories(vove-test-slow-raster-worker PRIVATE src/worker)
            target_link_libraries(vove-test-slow-raster-worker PRIVATE
                vove-raster-executor vove::worker_runtime)
            vove_enable_warnings(vove-test-slow-raster-worker)
        endif()

        add_executable(vove-publishing-corpus tests/preview/publishing_corpus.cpp)
        target_link_libraries(vove-publishing-corpus PRIVATE
            vove::cache vove::worker_thumbnail_renderer Qt6::Core Qt6::Gui)
        add_dependencies(vove-publishing-corpus vove-cdr-worker)
        vove_enable_warnings(vove-publishing-corpus)
        if(MINGW)
            target_link_options(vove-test-delayed-raster-worker PRIVATE -static)
            if(TARGET vove-test-slow-raster-worker)
                target_link_options(vove-test-slow-raster-worker PRIVATE -static)
            endif()
        endif()

        add_executable(vove-worker-thumbnail-renderer-tests
            tests/preview/worker_thumbnail_renderer_test.cpp)
        if(WIN32)
            target_compile_definitions(vove-worker-thumbnail-renderer-tests PRIVATE
                "VOVE_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
                "VOVE_TEST_RASTER_WORKER_PATH=L\"$<TARGET_FILE:vove-raster-worker>\""
                "VOVE_TEST_XCF_WORKER_PATH=L\"$<TARGET_FILE:vove-xcf-worker>\""
                "VOVE_TEST_DELAYED_RASTER_WORKER_PATH=L\"$<TARGET_FILE:vove-test-delayed-raster-worker>\""
                "VOVE_TEST_CDR_WORKER_PATH=L\"$<TARGET_FILE:vove-cdr-worker>\"")
        else()
            target_compile_definitions(vove-worker-thumbnail-renderer-tests PRIVATE
                "VOVE_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\""
                "VOVE_TEST_RASTER_WORKER_PATH=\"$<TARGET_FILE:vove-raster-worker>\""
                "VOVE_TEST_XCF_WORKER_PATH=\"$<TARGET_FILE:vove-xcf-worker>\""
                "VOVE_TEST_DELAYED_RASTER_WORKER_PATH=\"$<TARGET_FILE:vove-test-delayed-raster-worker>\""
                "VOVE_TEST_CDR_WORKER_PATH=\"$<TARGET_FILE:vove-cdr-worker>\"")
        endif()
        target_link_libraries(vove-worker-thumbnail-renderer-tests PRIVATE
            vove::platform vove::worker_thumbnail_renderer Qt6::Gui)
        add_dependencies(vove-worker-thumbnail-renderer-tests
            vove-raster-worker vove-xcf-worker vove-test-delayed-raster-worker vove-cdr-worker)
        if(VOVE_BUILD_MUPDF_DOCUMENT_WORKER)
            if(WIN32)
                target_compile_definitions(vove-worker-thumbnail-renderer-tests PRIVATE
                    "VOVE_TEST_DOCUMENT_WORKER_PATH=L\"$<TARGET_FILE:vove-document-worker>\"")
            else()
                target_compile_definitions(vove-worker-thumbnail-renderer-tests PRIVATE
                    "VOVE_TEST_DOCUMENT_WORKER_PATH=\"$<TARGET_FILE:vove-document-worker>\"")
            endif()
            add_dependencies(vove-worker-thumbnail-renderer-tests vove-document-worker)
        endif()
        if(VOVE_BUILD_RESVG_SVG_WORKER)
            if(WIN32)
                target_compile_definitions(vove-worker-thumbnail-renderer-tests PRIVATE
                    "VOVE_TEST_SVG_WORKER_PATH=L\"$<TARGET_FILE:vove-svg-worker>\"")
            else()
                target_compile_definitions(vove-worker-thumbnail-renderer-tests PRIVATE
                    "VOVE_TEST_SVG_WORKER_PATH=\"$<TARGET_FILE:vove-svg-worker>\"")
            endif()
            add_dependencies(vove-worker-thumbnail-renderer-tests vove-svg-worker)
        endif()
        vove_enable_warnings(vove-worker-thumbnail-renderer-tests)
        add_test(NAME vove-worker-thumbnail-renderer-tests
            COMMAND vove-worker-thumbnail-renderer-tests)
        set_tests_properties(vove-worker-thumbnail-renderer-tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        vove_set_qt_test_runtime(vove-worker-thumbnail-renderer-tests)
        vove_set_qt_image_plugins(vove-worker-thumbnail-renderer-tests)
    endif()

    if(BUILD_TESTING AND VOVE_BUILD_TESTS)
        add_executable(vove-preview-helper-service-tests
            tests/preview/helper_service_test.cpp)
        target_include_directories(vove-preview-helper-service-tests PRIVATE
            src/preview/helper)
        target_link_libraries(vove-preview-helper-service-tests PRIVATE
            vove-preview-helper-service vove::cache Qt6::Test)
        vove_enable_warnings(vove-preview-helper-service-tests)
        add_test(NAME vove-preview-helper-service-tests
            COMMAND vove-preview-helper-service-tests)
        vove_set_qt_test_runtime(vove-preview-helper-service-tests)
    endif()

    add_subdirectory(src/ui/qt)
endif()

foreach(vove_runtime_target IN ITEMS
        vove-ui-qt
        vove-open-helper
        vove-catalog-helper
        vove-fileop-helper
        vove-preview-helper
        vove-raster-worker
        vove-xcf-worker
        vove-document-worker
        vove-cdr-worker
        vove-svg-worker)
    vove_release_link_map(${vove_runtime_target})
endforeach()
if(TARGET vove-everything-helper)
    vove_release_link_map(vove-everything-helper)
endif()
if(TARGET vove-plocate-helper)
    vove_release_link_map(vove-plocate-helper)
endif()

if(VOVE_RELEASE_BUILD)
    if(CMAKE_CONFIGURATION_TYPES OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR
            "VOVE_RELEASE_BUILD requires a single-configuration Release generator")
    endif()
    if(VOVE_BUILD_TESTS)
        message(FATAL_ERROR
            "VOVE_RELEASE_BUILD requires VOVE_BUILD_TESTS=OFF to exclude test hooks")
    endif()
    if(NOT VOVE_BUILD_QT_APP OR NOT VOVE_BUILD_MUPDF_DOCUMENT_WORKER OR
       NOT VOVE_BUILD_RESVG_SVG_WORKER)
        message(FATAL_ERROR
            "VOVE_RELEASE_BUILD requires the Qt application, MuPDF PDF/AI worker and resvg SVG worker")
    endif()
    if(UNIX AND NOT APPLE AND NOT VOVE_EXPECT_STRICT_SANDBOX)
        message(FATAL_ERROR "Linux release builds require the strict bubblewrap contract")
    endif()

    set(vove_required_runtime_targets
        vove-ui-qt
        vove-open-helper
        vove-catalog-helper
        vove-fileop-helper
        vove-preview-helper
        vove-raster-worker
        vove-xcf-worker
        vove-document-worker
        vove-cdr-worker
        vove-svg-worker)
    if(WIN32)
        list(APPEND vove_required_runtime_targets vove-everything-helper)
    elseif(UNIX AND NOT APPLE)
        list(APPEND vove_required_runtime_targets vove-plocate-helper)
    endif()
    foreach(vove_required_runtime_target IN LISTS vove_required_runtime_targets)
        if(NOT TARGET "${vove_required_runtime_target}")
            message(FATAL_ERROR
                "Release runtime target is missing: ${vove_required_runtime_target}")
        endif()
    endforeach()
    add_custom_target(vove-release-runtime ALL DEPENDS ${vove_required_runtime_targets})
endif()

if(VOVE_BUILD_QT_SPIKE)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
    add_subdirectory(src/spikes/qt)
endif()

if(VOVE_BUILD_WIN32_SPIKE)
    if(NOT WIN32)
        message(FATAL_ERROR "The Win32 spike can only be built on Windows")
    endif()
    add_subdirectory(src/spikes/win32)
endif()
