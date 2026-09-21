# XCF preview contour

VO-VE decodes XCF with the reader from KDE KImageFormats 6.22.0, pinned at commit
`19df8b03a86b24f85c42f1abcef95cb3c0f85c9b`. The selected source is compiled into the dedicated
lazy `vove-xcf-worker`. GIMP is not detected, required or launched.

## Included surface

- composition of visible layers for XCF versions 0 through 19;
- uncompressed, RLE and independently bounded zlib-compressed tiles;
- bounded random-access reads from the already opened read-only source;
- canonical RGBA8 output and conversion of a valid embedded ICC profile to sRGB;
- the existing QOI worker response and persistent thumbnail cache.

The Qt plug-in registration, all other KImageFormats readers and external applications are not
included. VO-VE calls `XCFHandler` directly. Versions newer than 19 return a typed unsupported
result; there is no fallback to an installed editor. Version 20 is deliberately excluded because
layer effects can change the visible composite.

## Limits and verification

The decoder accepts at most 512 MiB of source data, 64 million source pixels and a 4096 px output
edge. The normal request budget can impose lower values. A real layered XCF is compared pixel for
pixel with the upstream reference PNG, and the same fixture is verified through the dedicated XCF
worker protocol and QOI response. The v13/v19/v20 boundary, unknown-property skipping and
NONE/RLE/zlib paths are tested explicitly. Truncated streams, checksum errors, short or long zlib
output and corruption in either tile of a multi-tile file are rejected without retry. Source,
canvas, output, unsupported compression and embedded-decoder allocation limits have distinct typed
results. The Windows installer smoke also renders the layered fixture through the installed AppContainer
worker and its packaged runtime DLLs.

The private visual gate reads 20 real, meaningfully named v19/RLE files from Synology and compares
them with adjacent TIFF exports in normalized sRGB. All 20 pass: SSIM `0.9914-0.9937`, RGB mean
absolute error `0.299-0.551`, maximum channel error `73`, and exact alpha. Set
`VOVE_XCF_CORPUS_ROOT` to run this gate; the client files are never copied into the repository.
No real zlib writer corpus is currently available, so zlib remains covered by specification-built
valid and corrupted fixtures rather than a real-editor corpus.

The complete Windows suite passes `126/126`. On the Linux verification host the current XCF worker
was rebuilt and the decoder, executor and worker-renderer test sets pass `3/3`; the complete Linux
suite was not repeated for this focused change.

The selected upstream files and license texts are recorded in
[`third_party/kimageformats-xcf/README.vo-ve.md`](../../third_party/kimageformats-xcf/README.vo-ve.md).
