#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vove_mupdf_status {
    VOVE_MUPDF_SUCCESS = 0,
    VOVE_MUPDF_UNSUPPORTED,
    VOVE_MUPDF_MALFORMED,
    VOVE_MUPDF_RESOURCE_LIMIT,
    VOVE_MUPDF_PASSWORD_REQUIRED,
    VOVE_MUPDF_AUTHENTICATION_FAILED,
    VOVE_MUPDF_INTERNAL_ERROR,
    VOVE_MUPDF_MEMORY_LIMIT,
};

enum vove_mupdf_color_model {
    VOVE_MUPDF_COLOR_UNKNOWN = 0,
    VOVE_MUPDF_COLOR_GRAY,
    VOVE_MUPDF_COLOR_RGB,
    VOVE_MUPDF_COLOR_CMYK,
    VOVE_MUPDF_COLOR_LAB,
    VOVE_MUPDF_COLOR_MIXED,
};

struct vove_mupdf_result {
    enum vove_mupdf_status status;
    enum vove_mupdf_color_model color_model;
    uint32_t width;
    uint32_t height;
    uint32_t page_index;
    uint32_t page_count;
    unsigned char *rgba8;
    size_t rgba8_bytes;
    char color_profile[97];
    char diagnostic[513];
};

struct vove_mupdf_request {
    uint64_t maximum_input_bytes;
    uint64_t memory_limit_bytes;
    uint32_t page_index;
    uint32_t canonical_edge;
};

void vove_mupdf_result_init(struct vove_mupdf_result *result);
void vove_mupdf_result_drop(struct vove_mupdf_result *result);

void vove_mupdf_render_pdf(FILE *source, const struct vove_mupdf_request *request,
                           const char *password_utf8, struct vove_mupdf_result *result);

#ifdef __cplusplus
}
#endif
