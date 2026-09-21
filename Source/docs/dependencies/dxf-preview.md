# DXF Embedded Preview

## Decision

VO-VE supports ASCII DXF only when the document contains a saved `THUMBNAILIMAGE` section. It
does not interpret CAD geometry and does not claim complete rendering of blocks, text, dimensions,
line styles or viewports. A valid DXF without a saved thumbnail returns the normal
`embedded preview unavailable` result.

This boundary keeps DXF support inside the lightweight viewer contract: no CAD engine, runtime DLL,
external process or new package dependency is added. The existing embedded-document worker receives
an already-opened read-only source, extracts the saved image and sends it through the common raster,
ICC, QOI and persistent-cache path.

## Format contour

- ASCII code/value pairs only. Binary DXF is reported as unsupported.
- A real `SECTION` / `THUMBNAILIMAGE` structure is required; the `.dxf` suffix only selects the
  candidate handler.
- Group `90` supplies the exact image byte count. One or more group `310` records supply hexadecimal
  image chunks. Odd, non-hex, missing or mismatched data fails closed.
- The saved image is a headerless Windows DIB. VO-VE accepts only documented 12/40/52/56/108/124-byte
  headers, validates the palette/mask extent and size, adds only the BMP file header, then delegates
  pixel and color validation to the existing raster decoder. A V5 DIB carrying profile payload is
  currently rejected because its packed pixel/profile ordering needs a real writer corpus.
- The source is read in 64 KiB windows. A line is limited to 4 KiB, scan to 512 MiB, pair count to
  16,777,216 and embedded image to 16 MiB. Worker time, memory and output limits remain unchanged.
- The source identity is revalidated after extraction. The dedicated handler id/version prevents a
  stale result from another decoder contour from being reused.

## Verification

Generated fixtures cover a valid DIB, LF/CRLF input, the official group-310 line limit, absent
thumbnail, byte-count mismatch, invalid hex, nested/truncated sections, hard scan/pair limits,
unrelated text, binary DXF and conservative V5 rejection. The valid fixture is exercised through
extraction, raster decoding, QOI encoding, helper routing, isolated worker and persistent-cache
reopen/scale reuse on Windows and Linux.

The approved Synology corpus contained no `.dxf` sample during the bounded read-only search on
2026-08-29. Therefore real-writer compatibility remains an explicit corpus gap before a public
support claim. Synthetic fixtures prove the documented container contract, not every AutoCAD writer
variant.

Official references:

- [Autodesk THUMBNAILIMAGE section group codes](https://help.autodesk.com/cloudhelp/2025/ENU/AutoCAD-DXF/files/GUID-F0369984-9699-40D5-8F9A-139491A14231.htm)
- [Autodesk: binary chunk interpretation and DIB thumbnail](https://blog.autodesk.io/dxf-files-binary-chunk-interpretation/)
- [Autodesk: DXF thumbnails are not generally provided in Explorer](https://www.autodesk.com/support/technical/article/caas/sfdcarticles/sfdcarticles/DXF-files-don-t-show-in-the-Windows-Explorer-Preview-Pane-or-as-thumbnails.html)
