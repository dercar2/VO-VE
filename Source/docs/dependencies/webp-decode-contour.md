# WebP decode-only dependency contour

## Purpose

This is a standalone, offline superbuild for the VO-VE Stage 4 WebP decoder.
It is not part of the application build and never downloads from the network.
Its only accepted input is the official pinned upstream archive:

- libwebp 1.6.0;
- URL: <https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz>;
- SHA-256: `e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564`.

The result contains only the static decoder, the static demux library, four
public decode/demux headers, a relocatable CMake package, and a contour
manifest. It contains no encoder, mux implementation, shared library, tool,
example, test, documentation, package-discovery metadata from upstream, or
threading dependency.

Upstream `webpdemux` links the full `webp` target, which contains both decoder
and encoder objects. The deterministic
`patch-libwebp-decode-only.cmake` step links it to upstream `webpdecoder`
instead. The superbuild then builds only `webpdecoder` and `webpdemux` and
installs them as `libwebp.a` and `libwebpdemux.a`. The patch SHA-256 is:

`60b917eb01e1170e4b77d5ccee5c7f04c3d6ee5854be527d832109172aa09aea`.

SIMD remains enabled for decode speed. Upstream threading is disabled to keep
the worker contour small and to leave concurrency under VO-VE worker control.

## Windows llvm-mingw build

Run from the repository root. The source archive must already exist in the
offline archive directory.

```powershell
$cmake = ".tools/cmake/cmake-4.3.4-windows-x86_64/bin/cmake.exe"
$ninja = (Resolve-Path ".tools/ninja/ninja.exe")
$llvm = (Resolve-Path ".tools/llvm-mingw/llvm-mingw-20260407-ucrt-x86_64")
$archives = (Resolve-Path ".tools/stage4-dist")
$toolchain = (Resolve-Path `
  "cmake/stage4-webp/toolchains/llvm-mingw-x86_64.cmake")

& $cmake -S cmake/stage4-webp -B out/webp-decode-win -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$ninja" `
  "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
  "-DVOVE_LLVM_MINGW_ROOT=$llvm" `
  "-DVOVE_WEBP_ARCHIVE_DIR=$archives"
& $cmake --build out/webp-decode-win --parallel
```

The result is `out/webp-decode-win/install`. The default hard installed-size
limit is 768 KiB. It can be changed only explicitly with
`VOVE_WEBP_MAX_INSTALLED_KIB`.

## Relocatable CMake targets

```cmake
find_package(vove-webp-decode CONFIG REQUIRED)
target_link_libraries(my_decoder PRIVATE
  vove-webp-decode::webp
  vove-webp-decode::webpdemux)
```

Point `CMAKE_PREFIX_PATH` at the generated install directory. The package
derives all locations from `CMAKE_CURRENT_LIST_DIR`; it contains no build or
archive path. The llvm-mingw toolchain intentionally root-limits package
search, so a cross-build can instead pass
`vove-webp-decode_DIR=<prefix>/lib/cmake/vove-webp-decode` explicitly.

## Reference validation

The contour was built twice from independent extraction, configuration, and
build directories on 2026-08-11 with CMake 4.3.4, Ninja 1.13.2, and
llvm-mingw Clang 22.1.3. Both builds produced byte-identical eight-file
install trees. A copied install prefix configured and linked a new consumer,
then decoded the official upstream `examples/test.webp` through demux and the
RGBA decoder.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| Complete installed contour | 541,738 | tree verified file by file |
| `lib/libwebp.a` | 479,792 | `61fb0e4a8226db6bc2c19ec43f26abef455706e537c489804557591de3df9962` |
| `lib/libwebpdemux.a` | 13,612 | `17fce88b803301407c4d7d319c745bf2faaf0174d953daa01f36e9c4db5facc0` |
| `vove-webp-decode-manifest.txt` | 323 | `ffd1cb5b5ab95e146c6418f0c4e4d5d13ccb30d4045c4df613be7dcb59a54d11` |

Static archive hashes are toolchain-specific reference values. The source
archive hash is the portable identity used by the contour.

## Verification contract

Every default build ends with `vove-webp-decode-verify`. It fails when:

- the local archive or the decode-only patch is missing or has a different
  SHA-256;
- a required deny-all upstream cache option is absent or enabled;
- an encoder object was compiled, or an encoder/mux archive was produced;
- an encoder or mux symbol survives in either installed static archive;
- any file outside the eight-file allowlist is installed;
- a shared library, executable, tool, pkg-config file, encoder header, or mux
  implementation header is installed;
- the result exceeds 768 KiB;
- an absolute build/archive path appears in metadata, headers, or static
  archives;
- a copied install prefix cannot configure and link a consumer;
- decoder/demux versions are not exactly 1.6.0, or the upstream WebP sample
  cannot be demuxed and decoded to non-empty RGBA pixels.

## Direct checks

```powershell
# Re-run the full verification, relocation build, and runtime smoke.
& .tools/cmake/cmake-4.3.4-windows-x86_64/bin/cmake.exe `
  --build out/webp-decode-win --target vove-webp-decode-verify

# Inspect the exact package tree and hashes.
Get-ChildItem out/webp-decode-win/install -Recurse -File |
  ForEach-Object {
    [pscustomobject]@{
      File = $_.FullName
      Bytes = $_.Length
      SHA256 = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
  }
```
