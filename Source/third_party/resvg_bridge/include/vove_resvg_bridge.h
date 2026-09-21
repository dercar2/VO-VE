#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
enum vove_resvg_status {
    VOVE_RESVG_OK = 0,
    VOVE_RESVG_INVALID_ARGUMENT = 1,
    VOVE_RESVG_SOURCE_LIMIT = 2,
    VOVE_RESVG_NOT_UTF8 = 3,
    VOVE_RESVG_MALFORMED_GZIP = 4,
    VOVE_RESVG_ELEMENT_LIMIT = 5,
    VOVE_RESVG_INVALID_SIZE = 6,
    VOVE_RESVG_PARSING_FAILED = 7,
    VOVE_RESVG_OUTPUT_TOO_SMALL = 8,
    VOVE_RESVG_INTERNAL_ERROR = 9,
};

struct vove_resvg_result {
    int32_t status;
    uint32_t width;
    uint32_t height;
    size_t bytes_written;
};

/*
 * Renders SVG or SVGZ bytes to straight-alpha sRGB RGBA8888.
 * External image paths are ignored; embedded data images remain available.
 */
struct vove_resvg_result vove_resvg_render(const uint8_t *source, size_t source_bytes,
                                            uint32_t canonical_edge, uint8_t *output,
                                            size_t output_capacity);

#ifdef __cplusplus
}
#endif
