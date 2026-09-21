# KImageFormats XCF reader

This directory contains only the XCF reader sources copied from KDE KImageFormats v6.22.0,
upstream commit `19df8b03a86b24f85c42f1abcef95cb3c0f85c9b`.

VO-VE builds these sources into its dedicated lazy `vove-xcf-worker`. It does not require or
launch GIMP, does not load a system KImageFormats installation, and does not include the other
KImageFormats plugins. VO-VE raises the reader boundary to XCF 19, adds bounded zlib tile inflation
through the already pinned `miniz`, and corrects the uncompressed tile path. XCF 20 and newer are
reported as unsupported.

The vendored contour removes Qt plug-in registration and enum reflection, and exposes the
reader's allocation-limit, unsupported-compression and malformed-tile results to the worker.
VO-VE invokes `XCFHandler` directly, so no MOC-generated plug-in code enters the worker. CMake
verifies every vendored source and license against the canonical LF hashes in `SHA256SUMS` during
configuration; CRLF checkouts are normalized before hashing. The XCF
format changes follow the official specification at <https://developer.gimp.org/core/standards/xcf/>;
no GIMP implementation source is copied.

Upstream: https://github.com/KDE/kimageformats

The copied sources retain their SPDX notices. Applicable license texts are in `LICENSES/`.
