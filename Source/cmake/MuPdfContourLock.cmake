include_guard(GLOBAL)

set(VOVE_MUPDF_LOCK_VERSION "1.28.0")
set(VOVE_MUPDF_LOCK_SOURCE_ARCHIVE_SHA256
    "21C7F064903154F1C3A7458BEE81F130FC36F9B5147EA13328F9980E02D2DEA2")

set(VOVE_MUPDF_LOCK_WINDOWS_LIBRARY_SHA256
    "AF6AF9CFD09ABAFF312706E25FA318C2810B6CBB19A8F8D3725292749044ABA4")
set(VOVE_MUPDF_LOCK_WINDOWS_THIRD_LIBRARY_SHA256
    "4A4EA017E9EA7FECA645AC9F982FEC3D84C7AA3B18A8DB5A4163D22D07D6D001")
set(VOVE_MUPDF_LOCK_LINUX_LIBRARY_SHA256
    "79CF8A84B85E716AB44DA6807E93684DFE31E4280CE9B0EFF2BD8E0E2F3D6DE6")
set(VOVE_MUPDF_LOCK_LINUX_THIRD_LIBRARY_SHA256
    "FD18A333099A6EEBC05D0844EC7757404297A0CCA47B77548E8EAC67F1E1269F")

set(VOVE_MUPDF_LOCK_PUBLIC_HEADERS_SHA256
    "9F029E950FBDE65D4AB2910D8FE59CA679E9CFD0475AE176BF98B630C233D404")

function(vove_mupdf_sha256 path output)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "MuPDF lock input does not exist: ${path}")
    endif()
    file(SHA256 "${path}" actual_sha256)
    string(TOUPPER "${actual_sha256}" actual_sha256)
    set(${output} "${actual_sha256}" PARENT_SCOPE)
endfunction()

function(vove_mupdf_public_headers_sha256 source_root output)
    if(NOT IS_DIRECTORY "${source_root}/include")
        message(FATAL_ERROR "MuPDF public include directory is missing: ${source_root}/include")
    endif()

    file(GLOB_RECURSE public_headers
        RELATIVE "${source_root}"
        "${source_root}/include/*.h")
    list(SORT public_headers)
    if(NOT public_headers)
        message(FATAL_ERROR "MuPDF public header set is empty: ${source_root}/include")
    endif()

    set(manifest "")
    foreach(relative_path IN LISTS public_headers)
        vove_mupdf_sha256("${source_root}/${relative_path}" header_sha256)
        string(APPEND manifest "${relative_path}\t${header_sha256}\n")
    endforeach()
    string(SHA256 manifest_sha256 "${manifest}")
    string(TOUPPER "${manifest_sha256}" manifest_sha256)
    set(${output} "${manifest_sha256}" PARENT_SCOPE)
endfunction()

function(vove_mupdf_require_sha256 path expected_sha256 label)
    vove_mupdf_sha256("${path}" actual_sha256)
    string(TOUPPER "${expected_sha256}" expected_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
            "${label} does not match the pinned MuPDF contour. "
            "Expected ${expected_sha256}, got ${actual_sha256}: ${path}")
    endif()
endfunction()

function(vove_verify_mupdf_contour)
    set(one_value_args
        SOURCE_ROOT SOURCE_ARCHIVE LIBRARY THIRD_LIBRARY
        EXPECTED_VERSION EXPECTED_SOURCE_ARCHIVE_SHA256
        EXPECTED_PUBLIC_HEADERS_SHA256 EXPECTED_LIBRARY_SHA256
        EXPECTED_THIRD_LIBRARY_SHA256)
    cmake_parse_arguments(ARG "" "${one_value_args}" "" ${ARGN})

    foreach(required IN ITEMS
            SOURCE_ROOT SOURCE_ARCHIVE LIBRARY THIRD_LIBRARY
            EXPECTED_VERSION EXPECTED_SOURCE_ARCHIVE_SHA256
            EXPECTED_PUBLIC_HEADERS_SHA256 EXPECTED_LIBRARY_SHA256
            EXPECTED_THIRD_LIBRARY_SHA256)
        if(NOT ARG_${required})
            message(FATAL_ERROR "vove_verify_mupdf_contour requires ${required}")
        endif()
    endforeach()

    set(version_header "${ARG_SOURCE_ROOT}/include/mupdf/fitz/version.h")
    if(NOT EXISTS "${version_header}")
        message(FATAL_ERROR "MuPDF version header is missing: ${version_header}")
    endif()
    file(STRINGS "${version_header}" version_lines
        REGEX "^#define[ \t]+FZ_VERSION[ \t]+\"[^\"]+\"")
    if(NOT version_lines)
        message(FATAL_ERROR "MuPDF FZ_VERSION is missing from ${version_header}")
    endif()
    list(GET version_lines 0 version_line)
    string(REGEX REPLACE
        "^#define[ \t]+FZ_VERSION[ \t]+\"([^\"]+)\".*$" "\\1"
        actual_version "${version_line}")
    if(NOT actual_version STREQUAL ARG_EXPECTED_VERSION)
        message(FATAL_ERROR
            "MuPDF version mismatch. Expected ${ARG_EXPECTED_VERSION}, "
            "got ${actual_version}: ${version_header}")
    endif()

    vove_mupdf_require_sha256(
        "${ARG_SOURCE_ARCHIVE}" "${ARG_EXPECTED_SOURCE_ARCHIVE_SHA256}"
        "MuPDF source archive")
    vove_mupdf_public_headers_sha256("${ARG_SOURCE_ROOT}" headers_sha256)
    string(TOUPPER "${ARG_EXPECTED_PUBLIC_HEADERS_SHA256}" expected_headers_sha256)
    if(NOT headers_sha256 STREQUAL expected_headers_sha256)
        message(FATAL_ERROR
            "MuPDF public headers do not match the pinned source archive. "
            "Expected ${expected_headers_sha256}, got ${headers_sha256}: ${ARG_SOURCE_ROOT}/include")
    endif()
    vove_mupdf_require_sha256(
        "${ARG_LIBRARY}" "${ARG_EXPECTED_LIBRARY_SHA256}" "MuPDF library")
    vove_mupdf_require_sha256(
        "${ARG_THIRD_LIBRARY}" "${ARG_EXPECTED_THIRD_LIBRARY_SHA256}"
        "MuPDF third-party library")
endfunction()

function(vove_verify_pinned_mupdf_contour source_root source_archive library third_library)
    if(WIN32)
        set(library_sha256 "${VOVE_MUPDF_LOCK_WINDOWS_LIBRARY_SHA256}")
        set(third_library_sha256 "${VOVE_MUPDF_LOCK_WINDOWS_THIRD_LIBRARY_SHA256}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
           (NOT CMAKE_SYSTEM_NAME AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"))
        set(library_sha256 "${VOVE_MUPDF_LOCK_LINUX_LIBRARY_SHA256}")
        set(third_library_sha256 "${VOVE_MUPDF_LOCK_LINUX_THIRD_LIBRARY_SHA256}")
    else()
        message(FATAL_ERROR
            "The pinned MuPDF contour is currently defined only for Windows and Linux")
    endif()

    vove_verify_mupdf_contour(
        SOURCE_ROOT "${source_root}"
        SOURCE_ARCHIVE "${source_archive}"
        LIBRARY "${library}"
        THIRD_LIBRARY "${third_library}"
        EXPECTED_VERSION "${VOVE_MUPDF_LOCK_VERSION}"
        EXPECTED_SOURCE_ARCHIVE_SHA256 "${VOVE_MUPDF_LOCK_SOURCE_ARCHIVE_SHA256}"
        EXPECTED_PUBLIC_HEADERS_SHA256 "${VOVE_MUPDF_LOCK_PUBLIC_HEADERS_SHA256}"
        EXPECTED_LIBRARY_SHA256 "${library_sha256}"
        EXPECTED_THIRD_LIBRARY_SHA256 "${third_library_sha256}")
endfunction()
