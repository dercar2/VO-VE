# Third-Party Notes

This file records components linked into VO-VE. It is intentionally limited to code that is
actually used by a product target.

Public releases at <https://github.com/dercar2/VO-VE/releases> include separate original
third-party notices and corresponding-source archives. Preserve these alongside redistributed
binaries. They are distribution materials, not runtime dependencies or optional future offers.
VO-VE-authored code is GPL-3.0-or-later; third-party licenses retain their own terms.
The document worker uses MuPDF 1.28.0 under AGPL-3.0-or-later, not a commercial license.
Windows Qt 6.10.3 is distributed under its GPLv3 open-source option; its third-party components,
LLVM-MinGW runtime and Rust components retain the licenses identified in the release supplements.
Distribution-provided Linux shared libraries are not bundled into the DEB.

## pugixml 1.16

- Purpose: parse bounded InDesign XMP metadata to extract a saved document preview.
- Upstream: <https://github.com/zeux/pugixml/tree/v1.16>.
- Linkage: static, only in the embedded-document worker. XPath is disabled; unused writer/file
  APIs are discarded. No new runtime DLL or dependency of the UI.
- Source hashes: `third_party/pugixml/README.vove.md`.

MIT License

Copyright (c) 2006-2026 Arseny Kapoulkine

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## OpenSSL 3

- Purpose: verify signed update manifests on Linux with the same P-256/SHA-256 contract used by
  Windows CNG.
- Upstream: <https://www.openssl.org/>.
- Linkage: the Linux package uses the distribution-provided `libcrypto.so.3`; OpenSSL is not
  bundled into VO-VE. The verifier is reached only by an explicit update check.
- License: Apache License 2.0. The exact runtime version is selected and serviced by the Linux
  distribution package manager.

## utf8proc 2.11.3

- Purpose: deterministic Unicode NFKC case folding for catalog sorting and filtering.
- Upstream: <https://github.com/JuliaStrings/utf8proc>
- Imported files: `utf8proc.c`, `utf8proc.h`, `utf8proc_data.c` and `LICENSE.md`.
- Linkage: one shared runtime library loaded by the GUI/main process. The catalog helper sends raw
  UTF-8 names and deliberately does not link or load the Unicode tables.
- License: MIT/Expat-compatible terms reproduced in
  [`third_party/utf8proc/LICENSE.md`](third_party/utf8proc/LICENSE.md).
- Pinned upstream commit: `e5e799221b45bbb90f5fdc5c69b6b8dfbf017e78` (`v2.11.3`).

## SQLite 3.53.4

- Purpose: persistent thumbnail-cache index.
- Upstream: <https://www.sqlite.org/>
- Imported files: the official `sqlite3.c` and `sqlite3.h` amalgamation.
- Linkage: statically linked into `vove-cache`; not linked into the UI target.
- License: public domain.
- Archive checksum and exact source details: [`third_party/sqlite/README.md`](third_party/sqlite/README.md).

## Little CMS 2.19.1

- Purpose: worker-side ICC conversion to canonical sRGB thumbnails.
- Upstream: <https://github.com/mm2/Little-CMS>
- Linkage: statically linked into the worker-side color library; not linked into the UI target.
- License: MIT terms reproduced in [`third_party/lcms2/LICENSE`](third_party/lcms2/LICENSE).
- Pinned source details: [`third_party/lcms2/README.md`](third_party/lcms2/README.md).

## QOI reference codec

- Purpose: compact lossless RGBA8 payloads inside VVT cache artifacts.
- Upstream: <https://github.com/phoboslab/qoi>
- Linkage: statically linked into `vove-cache`; not exposed as a user-facing file format.
- License: MIT terms reproduced in [`third_party/qoi/LICENSE`](third_party/qoi/LICENSE).
- Pinned source details: [`third_party/qoi/README.md`](third_party/qoi/README.md).

## libjxl 0.12.0 and Highway 1.2.0

- Purpose: JPEG XL first-frame decoding and conversion to canonical sRGB.
- Upstream: <https://github.com/libjxl/libjxl> and <https://github.com/google/highway>.
- Linkage: static decoder/CMS only in the raster worker, reusing Little CMS; no encoder,
  command-line utility, new DLL, animation playback or JPEG reconstruction is shipped.
- Licenses: libjxl BSD-3-Clause; Highway Apache-2.0. Pinned sources, hashes and build recipe:
  `docs/dependencies/jpegxl-decode-contour.md`. Brotli 1.2.0 is a configuration-only source
  input; its libraries are not built or linked.

## libheif 1.23.1, libde265 1.1.1 and dav1d 1.5.4

- Purpose: decode-only HEIF/HEIC and AVIF primary-image support.
- Upstream: <https://github.com/strukturag/libheif> and
  <https://github.com/strukturag/libde265> and <https://code.videolan.org/videolan/dav1d>.
- Linkage: the reproducible minimal static contour is linked only into the sandboxed raster worker;
  no shared codec library, plugin, encoder, CLI, example or test is shipped.
- Licenses: LGPL-3.0-or-later for libheif and libde265; BSD-2-Clause for dav1d. The public release
  includes their exact corresponding sources and original notices as companion assets.
- Pinned archives, hashes and contour verification:
  [`docs/dependencies/heic-decode-contour.md`](docs/dependencies/heic-decode-contour.md).

## libwebp 1.6.0

- Purpose: decode-only static and animated WebP support; animated files use the first frame and
  preserve the total frame count as page metadata.
- Upstream: <https://chromium.googlesource.com/webm/libwebp>.
- Linkage: only the static decoder and demux objects are linked into the sandboxed raster worker;
  the encoder, mux implementation, tools and duplicate Qt WebP plugin are absent.
- License: BSD 3-Clause terms in the pinned upstream archive. Original copyright, license and
  patent notices are reproduced in the public release's component materials.
- Pinned archive, hash and contour verification:
  [`docs/dependencies/webp-decode-contour.md`](docs/dependencies/webp-decode-contour.md).

## miniz 3.1.2

- Purpose: bounded raw-deflate preview entries in CDR/KRA/ORA and per-tile zlib streams in XCF.
- Upstream: <https://github.com/richgel999/miniz>.
- Linkage: only `tinfl` is compiled into the lazy workers that need it. Archive APIs, compression,
  high-level zlib APIs, stdio and miniz allocation helpers are disabled; XCF uses `tinfl`'s bounded
  zlib-header/checksum mode directly.
- License: MIT terms in [`third_party/miniz/LICENSE`](third_party/miniz/LICENSE).
- Pinned source and contour details: [`third_party/miniz/README.md`](third_party/miniz/README.md).

## Google PIEX

- Purpose: locate and extract the embedded JPEG preview from supported RAW camera containers.
- Upstream: <https://android.googlesource.com/platform/external/piex>.
- Linkage: a narrow static preview-extraction contour is linked only into the sandboxed raster
  worker; no full sensor decoder, demosaicer, encoder, command-line tool or additional runtime file
  is shipped.
- License: Apache License 2.0, reproduced in
  [`third_party/piex/LICENSE`](third_party/piex/LICENSE).
- Pinned commit, imported source list and the one portability-only patch are recorded in
  [`third_party/piex/UPSTREAM.md`](third_party/piex/UPSTREAM.md).

## KDE KImageFormats XCF reader 6.22.0

- Purpose: compose visible layers from XCF versions 0 through 19 without installing or launching
  a graphics editor.
- Upstream: <https://github.com/KDE/kimageformats>, pinned commit
  `19df8b03a86b24f85c42f1abcef95cb3c0f85c9b` (`v6.22.0`).
- Linkage: the XCF reader is compiled directly into the lazy isolated `vove-xcf-worker`. Qt
  plug-in registration, the other KImageFormats readers and all command-line applications are
  excluded. NONE/RLE/zlib tiles are supported; XCF 20+ is rejected until visual features can be
  gated safely. The generic raster worker remains independent of QtGui on Windows.
- License: LGPL-2.1-or-later for the selected reader, with retained SPDX notices and license texts
  in [`third_party/kimageformats-xcf`](third_party/kimageformats-xcf).

## MuPDF 1.28.0

- Purpose: render PDF and PDF-compatible AI pages inside the lazy document worker.
- Upstream: <https://mupdf.com/>.
- Linkage: the measured minimal static PDF contour is linked only into
  `vove-document-worker`; MuPDF is absent from the UI and the CDR/raster workers.
- Excluded product surfaces: JavaScript, forms, writers, CLI tools, examples, tests and document
  handlers other than the selected PDF contour are neither exposed nor shipped.
- Selection and measured contour: [`docs/adr/0009-mupdf-pdf-renderer.md`](docs/adr/0009-mupdf-pdf-renderer.md).
- Licensing decisions for public distribution remain a pre-release task during the current
  technical-prototype phase.

## resvg 0.48.1

- Purpose: render SVG and SVGZ into bounded sRGB thumbnails.
- Upstream: <https://github.com/linebender/resvg>.
- Linkage: a narrow Rust-to-C static bridge is linked only into the lazy one-shot
  `vove-svg-worker`; resvg is absent from the UI and other workers.
- Enabled surface: bounded SVGZ expansion, text, system fonts and embedded raster images. External
  image paths are not resolved, and no command-line tool is shipped.
- Exact dependency graph and checksums are pinned by
  [`third_party/resvg_bridge/Cargo.lock`](third_party/resvg_bridge/Cargo.lock); the bridge and
  reproducible build commands are documented in
  [`third_party/resvg_bridge/README.md`](third_party/resvg_bridge/README.md).

## GNU hp2xx 3.4.4

- Purpose: parse first-page PLT/HPGL vectors and built-in stroked labels for the existing SVG worker.
- Upstream: <https://www.gnu.org/software/hp2xx/>.
- Linkage: six selected C sources plus a bounded memory adapter are statically linked only into
  `vove-svg-worker`; no hp2xx CLI, raster output backend, font package or extra runtime file is shipped.
- License: GPL-2.0-or-later. Original source notices are retained, and the archive's complete
  `copying` text is preserved as `cmake/hp2xx/hp2xx-COPYING` in the build directory.
- Pinned archive, checked local patches, exclusions and measurements:
  [`docs/dependencies/hpgl-preview.md`](docs/dependencies/hpgl-preview.md).

## Everything SDK 1.4

- Purpose: a small Windows-only IPC adapter for global filename search inside explicitly allowed
  work roots.
- Upstream: <https://www.voidtools.com/support/everything/sdk/>.
- Linkage: the upstream C wrapper is statically linked only into the lazy
  `vove-everything-helper`; Everything itself is not bundled and remains an optional separately
  installed component.
- Exact source files and archive checksum:
  [`third_party/everything_sdk/README.vove.md`](third_party/everything_sdk/README.vove.md).
- License notices retained from the selected SDK files: Copyright (C) 2016 David Carpenter;
  Copyright (C) 2022 David Carpenter. Permission is hereby granted, free of charge,
  to any person obtaining a copy of this software and associated documentation files (the
  "Software"), to deal in the Software without restriction, including without limitation the
  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
  Software, and to permit persons to whom the Software is furnished to do so, subject to inclusion
  of this copyright and permission notice in all copies or substantial portions. The Software is
  provided "as is", without warranty of any kind; the authors are not liable for claims, damages
  or other liability arising from the Software or its use.
