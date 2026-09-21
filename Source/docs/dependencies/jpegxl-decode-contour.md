# JPEG XL decoder-only dependency contour

## Scope

Standalone offline CMake recipe in `cmake/jpegxl`, validated on Windows and Linux;
no application integration, operating-system changes, global installations or deployment.
The build stages only `jxl_dec`, `jxl_cms`, `hwy`, eleven decoder/CMS headers,
a relocatable CMake config, and an input manifest. It never invokes upstream
installation, which would install the combined encoder/decoder `jxl` library.

LCMS2 2.19.1 is reused from the existing VO-VE build rather than rebuilt or copied:
`out/build/windows-verification/libvove-lcms2.a` and `third_party/lcms2/include`.
The prefix is intentionally not self-contained with respect to LCMS2. Main-app
consumers define target `vove-lcms2` before finding the package; standalone
consumers supply the existing static archive. Its observed hash is recorded separately.

## Pinned Inputs

These are SHA-256 digests of fetched official GitHub commit archives, not
independently published release checksums. The offline recipe checks every archive
before using it. `inputs.cmake` is the machine-readable pin list.

| Project | Version | Commit | Archive SHA-256 |
| --- | --- | --- | --- |
| [libjxl](https://github.com/libjxl/libjxl/tree/a7a9c787341cf703dede03c2009fa460cae5e5df) | 0.12.0 | `a7a9c787341cf703dede03c2009fa460cae5e5df` | `818398895831069902e3677d285054a7d1255b11b221e94c6aaa1cb83b0a3f29` |
| [Highway](https://github.com/google/highway/tree/457c891775a7397bdb0376bb1031e6e027af1c48) | 1.2.0 | `457c891775a7397bdb0376bb1031e6e027af1c48` | `5124b0501c98d9930dbb065bfa1a5bbbd59ce0f12facb7e1e33aaef01a5f1f1a` |
| [Brotli](https://github.com/google/brotli/tree/028fb5a23661f123017c060daa546b55cf4bde29) | 1.2.0 | `028fb5a23661f123017c060daa546b55cf4bde29` | `0afe09a53c8bad9861c8dd1fc1284308d54f19d2979ba3541cfdcc9b05fe360f` |

Archive names are `<project>-<commit>.tar.gz`. Downloads use
`https://codeload.github.com/<owner>/<project>/tar.gz/<commit>`.

## Build

From `WORK` in PowerShell 7, using the existing CMake, Ninja and llvm-mingw tools:

```powershell
$cmake = '.tools/cmake/cmake-4.3.4-windows-x86_64/bin/cmake.exe'
$root = 'out/validation/jpegxl-deps-win'

# Explicit online preparation only; omit when the pinned archives are present.
& $cmake "-DARCHIVE_DIR=$root/downloads" -P cmake/jpegxl/fetch-sources.cmake

# Configuration and build below use only local inputs.
& $cmake -S cmake/jpegxl -B $root -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$((Resolve-Path '.tools/ninja/ninja.exe').Path)" `
  "-DCMAKE_TOOLCHAIN_FILE=$((Resolve-Path 'cmake/stage4/toolchains/llvm-mingw-x86_64.cmake').Path)" `
  "-DVOVE_LLVM_MINGW_ROOT=$((Resolve-Path '.tools/llvm-mingw/llvm-mingw-20260407-ucrt-x86_64').Path)" `
  -DVOVE_JPEGXL_BUILD_JOBS=3
& $cmake --build $root --parallel 3
```

Use a fresh build directory when changing pinned inputs or compilers. Optional
cache inputs are `VOVE_JPEGXL_ARCHIVE_DIR`, `VOVE_JPEGXL_INSTALL_PREFIX`,
`VOVE_JPEGXL_LCMS2_LIBRARY`, and `VOVE_JPEGXL_LCMS2_INCLUDE_DIR`.
The default output prefix is `out/validation/jpegxl-deps-win/install`.
No main application target or upstream test suite is built. Only the local API
smoke and its empty baseline are built in addition to the three requested libraries.

## Build Boundaries

Release/O3, static libraries, no LTO, upstream default Highway CPU dispatch.
`JPEGXL_ENABLE_SKCMS=OFF` and `JPEGXL_FORCE_SYSTEM_LCMS2=ON` select the reused
LCMS archive. Boxes, JPEG reconstruction, tools, developer tools, benchmarks,
examples, tests, fuzzers, JNI, sjpeg, OpenEXR, viewers, plugins, documentation,
tcmalloc and dependency provisioning are disabled.

The pinned upstream CMake still defines unused encoder, tool and Brotli targets.
The libjxl subdirectory is excluded from the default build, and the stage target
depends only on `jxl_dec`, `jxl_cms` and `hwy`. With boxes and JPEG reconstruction
off, Brotli is needed for configuration/header discovery but none of its archives
are built or linked. The verifier rejects extra built/staged archives.

Two small build accommodations are needed, without upstream source patches:

- Extract build inputs and tool source/configuration files, omitting disabled
  benchmark-script symlinks that fail extraction on this Windows host. Upstream
  enters its tools CMake directory even when all tool options are off.
- Set `BROTLI_EMSCRIPTEN=OFF` for native builds: its empty-main probe falsely
  succeeds with the existing toolchain's static `try_compile` mode.

Highway emits an upstream CMake CMP0111 deprecation warning; it did not prevent
configuration or compilation. No global tool settings or PATH are changed.

## Consumer Target

```cmake
# Set vove-jpegxl-decode_DIR to <prefix>/lib/cmake/vove-jpegxl-decode.
# Define vove-lcms2 first, or set VOVE_JPEGXL_LCMS2_LIBRARY to an existing archive.
find_package(vove-jpegxl-decode CONFIG REQUIRED)
target_link_libraries(my_decoder PRIVATE vove-jpegxl-decode::jxl_dec)
```

The target includes the CMS, Highway and LCMS link requirements and the static
export definitions. An existing `vove-lcms2` target takes precedence over the
archive-path fallback and need not have been built yet. Both the target-first
and existing-archive fallback paths passed configuration checks on Windows;
Linux passed the target-first check and linked the smoke through the archive fallback.
Call `JxlDecoderSetCms(decoder, *JxlGetDefaultCms())` when
using this separate CMS implementation. No `jxl_threads` library is staged;
the default decoder runner is used unless the application supplies its own runner.

Disabling boxes removes box-event extraction/decompression, not the decoder's
ability to consume the ordinary JPEG XL container. Disabling JPEG reconstruction
removes reconstruction of the original JPEG bytes, not pixel decoding. Real-file,
container, color/HDR, cancellation and allocation-limit coverage remains for the
application integration task.

## Windows Measurement

Measured on 2026-08-29 using CMake 4.3.4, Ninja 1.13.2 and llvm-mingw Clang 22.1.3,
with one `-j3` dependency build. Prefix has 16 files and no encoder, tool or DLL.

| Component | Bytes |
| --- | ---: |
| libjxl_dec.a | 3,093,694 |
| libjxl_cms.a | 165,262 |
| libhwy.a | 111,462 |
| Headers, CMake config and manifest | 121,969 |
| **Complete prefix** | **3,492,387** |
| Reused LCMS archive, outside prefix | 838,122 |
| Stripped static API smoke | 1,672,704 |
| Empty executable baseline | 13,824 |
| **Smoke minus baseline** | **1,658,880** |

The native smoke passed version 0.12.0, decoder creation, default-CMS registration,
event subscription, invalid-signature rejection and destruction. It does not
decode real JPEG XL pixels or execute a color transform. The smoke delta includes
the referenced LCMS/C++ runtime code and is not an incremental worker-size claim.

PE imports contain only KERNEL32 and Windows UCRT API sets: no new third-party,
codec, C++ or unwind DLL dependency. Symbol inspection found no `JxlEncoder` or
`BrotliEncoder` definitions in the staged JXL archives. Only the three requested
static archives were produced; no full `libjxl.a` or Brotli archive was built.

The reused LCMS archive SHA-256 was
`2080f10cf406fed379d53a0c67a775daf92f7a501afd9b83ae694a11f6a529ef`.
Build logs, `smoke-imports.txt` and `result.json` with output hashes are under
`out/validation/jpegxl-deps-win`. No runtime-size gate or deployment is introduced
by this dependency probe.

## Linux Build And Measurement

The same recipe was transferred with the three already-pinned local archives to
`/home/workadmin/vove-field-20260827/out/jpegxl-deps-linux` over the existing
strictly pinned VM103 workadmin SSH connection. All remote writes stayed beneath
that output directory. The existing Linux LCMS archive and headers were read only.
No packages, system settings, main-app sources or PNG fixtures were changed.

From that output directory, the transferred recipe was configured and built with:

```sh
cmake -S recipe -B . -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_MAKE_PROGRAM=/usr/bin/ninja \
  -DVOVE_JPEGXL_BUILD_JOBS=3 \
  -DVOVE_JPEGXL_LCMS2_LIBRARY=/home/workadmin/vove-field-20260827/out/build/libvove-lcms2.a \
  -DVOVE_JPEGXL_LCMS2_INCLUDE_DIR=/home/workadmin/vove-field-20260827/third_party/lcms2/include
cmake --build . --parallel 3
```

On 2026-08-29, CMake 3.31.6, Ninja 1.12.1 and GCC 14.2.0 completed the build,
static API smoke and all dependency checks successfully. The output prefix is
`/home/workadmin/vove-field-20260827/out/jpegxl-deps-linux/install`.

| Component | Bytes |
| --- | ---: |
| libjxl_dec.a | 2,933,088 |
| libjxl_cms.a | 168,414 |
| libhwy.a | 96,320 |
| Headers, CMake config and manifest | 121,914 |
| **Complete 16-file prefix** | **3,319,736** |
| Reused LCMS archive, outside prefix | 973,312 |
| Stripped static API smoke | 2,617,416 |
| Empty static executable baseline | 672,160 |
| **Smoke minus baseline** | **1,945,256** |

`readelf` found no dynamic section; `file` reported a stripped, statically linked
x86-64 ELF executable. Staged JXL archives contain no `JxlEncoder` or
`BrotliEncoder` definitions. The main-app-style `vove-lcms2` target also passed a
configuration-only check before its archive existed. No dependency rebuild was
needed for that check.

Linux archive SHA-256 values:

- `libjxl_dec.a`: `cfb8ed3187b3c7fc6345cd9df50de5f1fa290d4a1abf627c8819c7eca758f130`
- `libjxl_cms.a`: `8afd2db8872dd94b73824db099c7c9ca9ff165050aefbe05d1db98d7009388cc`
- `libhwy.a`: `8182b55d5fb81196bc880f0e35c338cf5633704258bd8bb4b25a5738bb18434f`
- Reused LCMS: `32d9be6e662438381a9d8b34192930a6cbfc32ff5331927922ba39283e5cd742`

These are dependency-prefix and standalone static-smoke measurements, not the
application worker size or its incremental growth. The smoke includes referenced
LCMS and static system/C++ runtime code; a worker that already links those may
have a different increment. The parent separately reported successful Linux
worker linkage against this prefix; that main-app build is not part of this
recipe validation. Real JPEG XL pixels, color/HDR behavior and focused wrapper
tests remain separate parent-owned integration coverage.
