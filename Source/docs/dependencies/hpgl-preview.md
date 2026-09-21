# PLT / HPGL Preview

## Implementation

GNU hp2xx 3.4.4 is reused for HPGL parsing, curves, clipping, pen definitions, line patterns,
polygon filling and built-in stroked text. Only `hpgl.c`, `chardraw.c`, `clip.c`, `fillpoly.c`,
`pendef.c` and `lindef.c` are compiled. `cmake/hp2xx` pins the official archive SHA-256 and
applies checked edits to a build-local extraction; upstream source notices remain intact.
The C adapter redirects streams to bounded memory, resets parser state per document and keeps
error recovery inside C. C++ translates checked intermediate records to SVG, rendered by the
existing resvg worker. No native Qt decoder, new worker, runtime DLL or font dependency is added.

Source: <https://ftp.gnu.org/gnu/hp2xx/hp2xx-3.4.4.tar.gz>

SHA-256: `47b72fb386a189b52f07e31e424c038954c4e0ce405803841bed742bab488817`.

## Product Boundary

- `.plt` and `.hpgl`, first page only. This is not a general PCL renderer.
- Lines, curves, clipping, supported polygon fills, RGB pens, widths and dash patterns;
  labels use upstream stroke tables, not installed system fonts.
- User-defined `UL` patterns: indices 1-8, at most 19 nonnegative elements with positive total.
- Source up to 32 MiB; intermediate commands and SVG each up to 16 MiB.
  Existing worker memory, time, output and cancellation limits remain unchanged.
- Unsupported instructions or styles return no image, rather than a knowingly incomplete drawing.
  In particular: encoded `PE`, `AD`/`SD`, unavailable glyph tables, non-ASCII labels,
  parameterized `BP`, custom `CR`, nonzero-winding `FP`, `PT` and unimplemented cap/join styles.
- First-page metadata is 1, not an inferred total page count. Pen RGB is treated as sRGB;
  no document ICC profile is invented.
- Cache identity `document.hp2xx.hpgl`, version 1. Canonical thumbnail and larger selected
  preview use the existing persistent cache. HPGL does not depend on SVG system-font fingerprints.

## Verification

Six focused suites pass on both Windows and Linux: core directory classification, worker protocol,
SVG executor, thumbnail backend/cache, HPGL parser/serializer and Qt preview pane.
Tests inspect actual colors and geometry, thin pen widths, miter bounds, redundant pen/style
commands, active-pen fill restoration, malformed input recovery, source/output limits and first page.

The 36 upstream `hp-tests` documents were run at 512 and 2048 px on Windows and Linux:

| Result | Count | Classification |
| --- | ---: | --- |
| Rendered | 32 documents / 64 images | All images are pixel-identical across platforms; every reopened cache result matches |
| `fill.plt` | 1 | Unsupported `FP1` winding rule |
| `la.plt`, `miter.plt` | 2 | Unsupported cap/join styles |
| `spectrum.plt` | 1 | Contains non-HPGL assignments such as `X0=0`; rejected as malformed |

The generic corpus runner expects every nonempty file to render, so its raw report intentionally
contains eight failed expectations (four documents at two sizes). This is not a 36/36 pass claim.
No crash, timeout or cache discrepancy occurred. Raw evidence stays in
`out/validation/hpgl/windows-final` and `linux-final`.
Usual cold thumbnails were 103-178 ms on Windows and 17-54 ms on Linux;
the dense filled-wedge case took 577/448 ms for 512 px and 2845/2576 ms for 2048 px respectively.
These are measurements on the available machines, not a universal speed promise.

## Footprint

| Stripped SVG worker | Before | After | Increase |
| --- | ---: | ---: | ---: |
| Windows | 3,254,272 B | 3,469,312 B | 215,040 B |
| Linux | 3,335,144 B | 3,466,496 B | 131,352 B |

The C++ UI remains unlinked from hp2xx. Windows imports only system/UCRT libraries;
Linux retains the normal C/C++ runtime. Function/data sections and the existing release
linker collection apply. No upstream encoder, raster output backend, CLI, examples or tests
enter the runtime package. Installer size and combined dependency notices are checked at
the batch release, not by publishing a package per format. No deployment was performed here.

Independent scoped review: GO for this bounded subset, with the four corpus exclusions above.
