#pragma once

#include "vove/handlers/cdr/embedded_preview.hpp"

namespace vove::handlers::indesign {

// Extracts only the first stored document preview, never renders an InDesign page.
// PageInfo/Seq takes precedence over legacy Thumbnails/Alt. No later-entry fallback.
// The candidate contains the original JPEG bytes; dimensions must come from decoding
// those bytes, not MaxPageSize, XMP width/height, or the number of saved previews.
// container stays unknown; page_one means first stored preview (possibly a spread).
//
// Limits are clamped to the CDR hard maxima and reused as follows:
// central_entries: contiguous objects and saved-preview entries (separate counters);
// central_directory_bytes: aggregate read_at bytes, including the two master pages;
// compressed_preview_bytes: each XMP packet and its extracted JPEG;
// total_preview_bytes: returned JPEG bytes. XML additionally has fixed depth/token
// limits. Opaque database/object contents are skipped, not scanned or allocated.
[[nodiscard]] cdr::CdrPreviewResult
extract_indesign_embedded_previews(const raster::NativeSource &source,
                                   const cdr::CdrPreviewLimits &limits = {});

// IDML is a ZIP package. This path verifies the official package mimetype,
// reads only META-INF/metadata.xml and reuses the bounded XMP/JPEG parser above.
// It never interprets spreads, stories, links, fonts or page geometry. An IDML
// package without a saved XMP preview reports no_preview.
[[nodiscard]] cdr::CdrPreviewResult
extract_idml_embedded_previews(const raster::NativeSource &source,
                               const cdr::CdrPreviewLimits &limits = {});

} // namespace vove::handlers::indesign
