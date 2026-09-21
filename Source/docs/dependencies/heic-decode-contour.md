# HEIC/AVIF decode-only dependency contour

## Purpose

This is a standalone, offline superbuild for the Stage 4 HEIC/AVIF decoder. It is
not included by the application build and never downloads from the network.
The only accepted inputs are the pinned upstream release archives listed in
`SHA256SUMS`:

- libde265 1.1.1;
- libheif 1.23.1;
- dav1d 1.5.4 ([official release](https://download.videolan.org/pub/videolan/dav1d/1.5.4/),
  [published SHA-256](https://download.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz.sha256)).

The result contains static libraries and public libheif headers. libde265 and
dav1d are internal implementation details; their public headers are not installed
in the final contour. Built-in libde265 provides HEVC decoding and built-in dav1d
provides AV1 decoding. dav1d is built with x86 assembly and both `8,16` bit-depth
implementations (8-bit and AV1 high-bit-depth 10/12-bit), release/O3 and no LTO.
It contains no codec plugins, concrete encoders, command-line tools, examples,
tests, documentation, VVC,
JPEG/JPEG 2000 codecs, uncompressed codec, WebCodecs, or GDK Pixbuf module.

libheif is an upstream monolithic library, so its public encoding API and some
generic encoding core object files remain in the static archive. All concrete
encoder backends are disabled. Upstream also registers an unconditional mask
encoder; the deterministic `patch-libheif-decode-only.cmake` step removes
that registration and its two source files. The patch also rejects bitstream-only
PQ/HLG in the dav1d backend before pixel conversion, releasing the received frame.
The pinned patch SHA-256 is
`6e60dc1fb78d222263504fe44ed23c3798cdf54ef1167caa4363228275cee5dc`.
The runtime smoke requires zero
encoder descriptors, and archive dead stripping keeps unused generic core
objects out of the worker binary. The generated contour manifest records the
patch SHA-256 alongside all three archive hashes.

`ENABLE_DECODER=OFF` in the libde265 cache is intentional: in libde265 1.1.1
that option controls the `dec265` executable, while the decoder implementation
is always part of the `de265` library.

## Windows llvm-mingw build

Run from `WORK` in PowerShell 7. Existing build-local Python, Meson 1.9.2 and
NASM 3.02 are prerequisites, in addition to CMake, Ninja and llvm-mingw.
The recipe never installs tools, changes global PATH, or fetches network inputs.
The existing smoke launcher supplies the compiler runtime directory to its child
process PATH only.
The Python executable, Meson module directory, NASM executable and dav1d archive
are explicit cache inputs. The dav1d archive may also be placed beside the other
archives in `VOVE_HEIC_ARCHIVE_DIR`, which is its default location.

```powershell
$cmake = ".tools/cmake/cmake-4.3.4-windows-x86_64/bin/cmake.exe"
$ninja = (Resolve-Path ".tools/ninja/ninja.exe")
$llvm = (Resolve-Path ".tools/llvm-mingw/llvm-mingw-20260407-ucrt-x86_64")
$archives = (Resolve-Path ".tools/stage4-dist")
$dav1d = (Resolve-Path ".tools/avif-build/downloads/dav1d-1.5.4.tar.xz")
$meson = (Resolve-Path ".tools/avif-build/python-packages/meson-1.9.2")
$nasm = (Resolve-Path ".tools/avif-build/nasm/nasm-3.02/nasm.exe")
$python = "C:/Users/dercar/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe"

& $cmake -S cmake/stage4 -B out/validation/heic-dav1d-win -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$ninja" `
  "-DCMAKE_TOOLCHAIN_FILE=$((Resolve-Path 'cmake/stage4/toolchains/llvm-mingw-x86_64.cmake'))" `
  "-DVOVE_LLVM_MINGW_ROOT=$llvm" `
  "-DVOVE_HEIC_ARCHIVE_DIR=$archives" `
  "-DVOVE_HEIC_DAV1D_ARCHIVE=$dav1d" `
  "-DVOVE_HEIC_PYTHON=$python" `
  "-DVOVE_HEIC_MESON_MODULE_DIR=$meson" `
  "-DVOVE_HEIC_NASM=$nasm" `
  -DVOVE_HEIC_BUILD_JOBS=3
& $cmake --build out/validation/heic-dav1d-win --parallel 3
```

The output is `out/validation/heic-dav1d-win/install`. Use a fresh build directory
when changing the pinned source or patch; ExternalProject extraction and patch
stamps are not a substitute for a clean validation. Dependencies are serialized
so their nested `-j3` invocations cannot multiply the total build concurrency.
The default hard size limit is
13 MiB and can be changed only explicitly with `VOVE_HEIC_MAX_INSTALLED_MIB`.
Existing build caches retain their previous value; reconfigure them with
`-DVOVE_HEIC_MAX_INSTALLED_MIB=13` to adopt the approved limit.

## Linux GCC build

The parent reported a completed native Linux dependency build and passing HEVC
smoke on 2026-08-29; its only failure was the previous 12 MiB size gate.
A native-host Linux build requires CMake, Ninja, GCC/G++, the platform thread library,
Python, Meson 1.9.2 and NASM. Supply local tool paths explicitly; no system codec
development package is used. Cross-OS builds are not supported by this recipe.

```sh
cmake -S cmake/stage4 -B out/heic-decode-linux -G Ninja \
  -DVOVE_HEIC_ARCHIVE_DIR=/absolute/path/to/stage4-dist \
  -DVOVE_HEIC_PYTHON=/absolute/path/to/python \
  -DVOVE_HEIC_MESON_MODULE_DIR=/absolute/path/to/meson-modules \
  -DVOVE_HEIC_NASM=/absolute/path/to/nasm \
  -DVOVE_HEIC_BUILD_JOBS=3
cmake --build out/heic-decode-linux --parallel 3
```

The output is `out/heic-decode-linux/install`.

## Windows dav1d contour measurement

On 2026-08-29 the clean Windows x86-64 build used CMake 4.3.4, Ninja 1.13.2,
llvm-mingw Clang 22.1.3, Python 3.12.13, Meson 1.9.2 and NASM 3.02, with nested
builds capped at three jobs. All source and patch hashes matched. HEVC decoding
passed at 1280x854 RGBA, with exactly libde265 and dav1d decoder descriptors and
zero encoder descriptors. The codec-option, static-only and metadata checks passed.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| libde265.a | 1,011,734 | `25f03dc9a263ab223da165c2bb266d7c6ba78442e92f54e70082a01933a7e112` |
| libdav1d.a | 3,570,446 | `f812b73d2d10cd03e28aed98cecbe0fe1bab98665b0ee351defc9703bcf7b7e7` |
| libheif.a | 7,763,174 | `9a2bb30c6b63fa00bc2d5977ac681001fe34568023a43cd9d369b9a0ac81b5a5` |
| Complete 33-file install prefix | 12,679,352 | |
| Linked smoke executable (outside prefix) | 5,434,368 | |

The original 12 MiB gate (12,582,912 bytes) failed by 96,440 bytes on Windows;
a reconfigure/rebuild and second smoke reproduced that result. The parent
reported a Linux prefix of 12,631,506 bytes, exceeding the old gate by 48,594
bytes, with HEVC smoke passing.

On 2026-08-29 the implementation selected a **13 MiB developer-prefix gate
(13,631,488 bytes)** under the user's existing permission to increase size for
required functionality. After the color correction, the stripped Windows worker
is 5,205,504 bytes versus the PSB baseline of 3,190,784 bytes: **+2,014,720 bytes
(about 1.92 MiB)** with no added DLL. Focused Windows/Linux tests, seven real AVIF
sources through the worker and reopened disk cache, and independent SDR midtone
references pass. HDR rejection covers both container and bitstream-only signaling.

The static development prefix is not the deployed worker: it retains all three
decoder/container archives, the public libheif headers, and relocatable discovery
metadata. Keeping these intact avoids symbol-stripping or archive-member pruning.
The approved limit leaves 952,136 bytes of Windows headroom and 999,982 bytes
of Linux headroom. The runtime increase is accepted for AVIF functionality, not
inferred from static archive sizes. Both platform gates pass at 13 MiB;
no packaging or deployment is part of this change.

After updating the Windows cache to 13 MiB, all contour verification checks and
the existing 1280x854 HEVC RGBA smoke passed again without rebuilding dependencies.

The contour smoke imports KERNEL32, UCRT API-set DLLs, `libc++.dll` and
`libunwind.dll`; it imports no codec DLL. Static codec linkage does not imply a
fully static C++ runtime. Logs, PE imports and machine-readable measurements are
under `out/validation/heic-dav1d-win`. These local logs cover the dependency
contour and HEVC smoke only. The worker sizes, AVIF/HDR test results and Linux
measurement above are parent-reported integration results, not tests run by this
dependency build.

## Historical HEVC-only reference

The previous HEVC-only contour was built and tested from clean build directories
on 2026-08-11. These measurements do not cover the dav1d-enabled contour.
The archive hashes matched `SHA256SUMS`, the runtime smoke decoded the pinned
HEIC sample, and a copied install prefix configured, linked, and ran again on
both platforms.

| Platform | Toolchain | Installed bytes | libde265.a SHA-256 | libheif.a SHA-256 |
| --- | --- | ---: | --- | --- |
| Windows x86-64 | CMake 4.3.4, Ninja 1.13.2, llvm-mingw Clang 22.1.3 | 9,084,361 | `25f03dc9a263ab223da165c2bb266d7c6ba78442e92f54e70082a01933a7e112` | `30d4f4999a7f4ffe5f8cf45d755a6ad1dd32fdb48bf4d25df73ec43cffc93021` |
| Linux x86-64 | CMake 3.31.6, Ninja 1.12.1, GCC 14.2.0 | 8,865,609 | `ef3017a6848051ef0916878b3f14ba6618d5aac1cdd7a0ec30f62249e7ac34c7` | `dd972f8c1b38388a165ac45f71d0d8f15c303c74f89b8bf09cb636a5d9f373f5` |

Two independent clean Windows builds produced byte-identical 32-file install
trees. Static archive hashes are toolchain-specific reference values, not
portable substitutes for the source archive hashes in `SHA256SUMS`.

## Verification contract

Every default build ends with `vove-heic-decode-verify`. It fails when:

- an input archive is missing or has a different SHA-256;
- an expected codec cache option or dav1d Meson option is absent or unexpected;
- a codec capability other than built-in libde265 HEVC or dav1d AV1 decode is enabled;
- a shared library, executable, plugin, tool, or upstream discovery metadata is
  installed;
- the result exceeds the size limit;
- an absolute source, build, or archive path leaks into installed metadata;
- the relocatable package cannot link and run a smoke executable, the runtime
  exposes anything other than one libde265 HEVC decoder, one dav1d AV1 decoder
  and zero encoders, or
  the pinned upstream HEIC sample cannot be decoded to non-empty RGBA pixels.

This contour smoke checks AV1 backend registration and HEVC pixel decoding. It
does not claim AVIF pixel or HDR-policy coverage; those belong to the separate
decoder integration tests. The install prefix is a build dependency artifact,
not a per-format application deployment or release package.

## Relocatable CMake target

Upstream libheif 1.23.1 exports the discovered absolute `LIBDE265_LIBRARY` and
`DAV1D_LIBRARY` paths and writes the absolute plugin directory into
`heif_version.h` even when
plugin loading is disabled. The finalization step removes upstream
CMake/pkg-config metadata, normalizes the unused plugin path to an empty
string, and installs a small relocatable package instead:

```cmake
find_package(vove-heic-decode CONFIG REQUIRED)
target_link_libraries(my_decoder PRIVATE vove-heic-decode::heif)
```

Point `CMAKE_PREFIX_PATH` at the generated install directory. Moving that
directory does not change the target because all locations are derived from
`CMAKE_CURRENT_LIST_DIR`. When using the supplied llvm-mingw toolchain, its
package search is intentionally root-limited; pass
`vove-heic-decode_DIR=<prefix>/lib/cmake/vove-heic-decode` explicitly.

## Upstream diagnostic

GCC 14.2 reports a `-Wstringop-overflow` warning in libde265 1.1.1
`libde265/slice.cc` while compiling the pinned, unmodified release. This
contour does not patch vendored source because its archive identity is part of
the reproducibility contract. HEIC input remains untrusted and this decoder
must run only inside the Stage 3 worker sandbox with the existing resource and
time limits. Re-evaluate the diagnostic when changing the pinned version.
