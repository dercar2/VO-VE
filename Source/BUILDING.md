# Building VO-VE

Generated from clean tracked WORK at commit c318544645c89c8fcc7203277c7c6ac01013e3c7.
Build label: PREVIEW-ISOLATION-20260923
In the public tag archive, this snapshot is the Source/ directory beside Site/.
See PUBLIC-DISTRIBUTION.md (added during release assembly) for pinned supplement URLs
and SHA-256 digests. LICENSE, THIRD_PARTY_NOTES.md and dependency notices accompany the source.

Run from this snapshot directory with CMake, Ninja and a C++20 compiler:

    cmake --preset source-release
    cmake --build --preset source-release

This lean snapshot excludes developer tests, benchmarks and experimental interfaces.
The source-release preset builds the headless libraries/helpers. For the complete viewer,
use the linux-release preset on Linux, or enable the options below on Windows.

For the full viewer on Windows, provision LLVM-MinGW 20260407 and Qt 6.10.3.
On Linux, provision Qt/OpenSSL development packages and bubblewrap for verification.
Both platforms need the HEIC/WebP/JPEG XL contours in docs/dependencies, MuPDF via
scripts/build_mupdf_contour.ps1 (Windows) or .sh (Linux), and the pinned Rust
bridge via scripts/build_resvg_bridge.ps1 or .sh. These pin libheif 1.23.1,
libde265 1.1.1, dav1d 1.5.4, WebP 1.6.0, libjxl 0.12.0, Highway 1.2.0,
MuPDF 1.28.0 and Rust 1.98.0. The MuPDF scripts
also enforce native compiler/make versions; retain their verification checks.
Supply VOVE_HEIC_DECODE_ROOT,
VOVE_WEBP_DECODE_ROOT, VOVE_JPEGXL_DECODE_ROOT, VOVE_MUPDF_SOURCE_ROOT, VOVE_MUPDF_SOURCE_ARCHIVE,
VOVE_MUPDF_LIBRARY, VOVE_MUPDF_THIRD_LIBRARY and VOVE_RESVG_BRIDGE_LIBRARY paths.
Configure a Release build with VOVE_BUILD_QT_APP, VOVE_BUILD_MUPDF_DOCUMENT_WORKER,
VOVE_BUILD_RESVG_SVG_WORKER and VOVE_RELEASE_BUILD enabled, and VOVE_BUILD_TESTS
and VOVE_BUILD_BENCHMARKS disabled. Dependency/tool caches are not in this snapshot;
offline rebuilding requires those inputs to have been provisioned locally.
The compiled UI artwork is included. The brand directory contains only the three approved
AI logo originals; it is not a runtime asset directory. Other authoring designs and fonts
are not required to compile the viewer. Regenerating or verifying installer artwork needs
the pinned inputs listed in packaging/windows/artwork/installer-artwork-manifest.json;
provision those separately before running the Windows installer script.
Run packaging scripts only after a full release build.
