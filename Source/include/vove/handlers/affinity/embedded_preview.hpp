#pragma once

#include "vove/handlers/cdr/embedded_preview.hpp"

namespace vove::handlers::affinity {

// Extracts only the header-referenced document thumbnail, never placed artwork.
// Supports the observed single-PNG Thmb layout in Prsn archives (versions 7-12).
// The result's container stays unknown; page_one is false (no page-count claim).
// CDR limits bound metadata bytes, PNG chunk count, encoded bytes and estimated
// RGBA decode bytes respectively. Values above the CDR hard caps are rejected.
[[nodiscard]] cdr::CdrPreviewResult
extract_affinity_embedded_previews(const raster::NativeSource &source,
                                  const cdr::CdrPreviewLimits &limits = {});

} // namespace vove::handlers::affinity
