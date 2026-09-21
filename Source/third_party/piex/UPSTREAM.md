# Google PIEX

- Upstream: `https://android.googlesource.com/platform/external/piex`
- Commit: `eaddfa72c1693728db64101701431421371c38da`
- Retrieved: 2026-08-24
- License: Apache-2.0 (`LICENSE`)

Imported files (`18`):

- `LICENSE`, `README`, `UPSTREAM.md`;
- `src/piex.cc`, `src/piex.h`, `src/piex_cr3.cc`, `src/piex_cr3.h`, `src/piex_types.h`;
- `src/tiff_parser.cc`, `src/tiff_parser.h`;
- `src/binary_parse/cached_paged_byte_array.cc` and `.h`;
- `src/binary_parse/range_checked_byte_ptr.cc` and `.h`;
- `src/image_type_recognition/image_type_recognition_lite.cc` and `.h`;
- `src/tiff_directory/tiff_directory.cc` and `.h`.

The vendored contour contains only the PIEX preview extractor and its required parsers. It is
compiled as a static library and adds no runtime file to the VO-VE package. VO-VE uses it only to
locate an embedded JPEG preview; full RAW decoding and demosaicing are deliberately outside this
contour.

Local portability patch:

- `image_type_recognition_lite.cc` includes `<cstring>` and calls `std::strlen` explicitly so the
  same source builds with GCC 14 and LLVM-MinGW without relying on a transitive C header.
