#include "mupdf_adapter.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static int seek_file(FILE *source, int64_t offset, int origin) {
    return _fseeki64(source, offset, origin);
}

static int64_t tell_file(FILE *source) {
    return _ftelli64(source);
}
#else
static int seek_file(FILE *source, int64_t offset, int origin) {
    return fseek(source, (long)offset, origin);
}

static int64_t tell_file(FILE *source) {
    return (int64_t)ftell(source);
}
#endif

static void copy_text(char *destination, size_t capacity, const char *source) {
    size_t length;
    if (capacity == 0)
        return;
    if (source == NULL)
        source = "";
    length = strlen(source);
    if (length >= capacity)
        length = capacity - 1;
    for (size_t index = 0; index < length; ++index)
        destination[index] = source[index];
    destination[length] = '\0';
}

static void discard_message(void *user, const char *message) {
    (void)user;
    (void)message;
}

enum vove_color_space_bits {
    VOVE_COLOR_GRAY = 1U << 0U,
    VOVE_COLOR_RGB = 1U << 1U,
    VOVE_COLOR_CMYK = 1U << 2U,
    VOVE_COLOR_LAB = 1U << 3U,
    VOVE_COLOR_SEPARATION = 1U << 4U,
};

struct vove_color_audit_device {
    fz_device super;
    unsigned int color_spaces;
    int conflicting_profiles;
    char profile[97];
};

static void audit_colorspace(fz_context *ctx, fz_device *device, fz_colorspace *space) {
    struct vove_color_audit_device *audit = (struct vove_color_audit_device *)device;
    const char *profile = NULL;
    if (space == NULL)
        return;
    switch (fz_colorspace_type(ctx, space)) {
    case FZ_COLORSPACE_GRAY:
        audit->color_spaces |= VOVE_COLOR_GRAY;
        break;
    case FZ_COLORSPACE_RGB:
    case FZ_COLORSPACE_BGR:
        audit->color_spaces |= VOVE_COLOR_RGB;
        break;
    case FZ_COLORSPACE_CMYK:
        audit->color_spaces |= VOVE_COLOR_CMYK;
        break;
    case FZ_COLORSPACE_LAB:
        audit->color_spaces |= VOVE_COLOR_LAB;
        break;
    case FZ_COLORSPACE_SEPARATION:
        audit->color_spaces |= VOVE_COLOR_SEPARATION;
        profile = fz_colorspace_name(ctx, space);
        break;
    case FZ_COLORSPACE_NONE:
    case FZ_COLORSPACE_INDEXED:
        break;
    }
    if (fz_colorspace_is_icc(ctx, space) && !fz_colorspace_is_device(ctx, space)) {
        const char *icc_name = fz_colorspace_name(ctx, space);
        if (icc_name != NULL && strcmp(icc_name, "Lab") != 0)
            profile = icc_name;
    }
    if (profile != NULL && profile[0] != '\0') {
        if (audit->profile[0] == '\0')
            copy_text(audit->profile, sizeof(audit->profile), profile);
        else if (strcmp(audit->profile, profile) != 0)
            audit->conflicting_profiles = 1;
    }
}

static void audit_fill_path(fz_context *ctx, fz_device *device, const fz_path *path, int even_odd,
                            fz_matrix matrix, fz_colorspace *space, const float *color, float alpha,
                            fz_color_params params) {
    (void)path;
    (void)even_odd;
    (void)matrix;
    (void)color;
    (void)alpha;
    (void)params;
    audit_colorspace(ctx, device, space);
}

static void audit_stroke_path(fz_context *ctx, fz_device *device, const fz_path *path,
                              const fz_stroke_state *stroke, fz_matrix matrix, fz_colorspace *space,
                              const float *color, float alpha, fz_color_params params) {
    (void)path;
    (void)stroke;
    (void)matrix;
    (void)color;
    (void)alpha;
    (void)params;
    audit_colorspace(ctx, device, space);
}

static void audit_fill_text(fz_context *ctx, fz_device *device, const fz_text *text,
                            fz_matrix matrix, fz_colorspace *space, const float *color, float alpha,
                            fz_color_params params) {
    (void)text;
    (void)matrix;
    (void)color;
    (void)alpha;
    (void)params;
    audit_colorspace(ctx, device, space);
}

static void audit_stroke_text(fz_context *ctx, fz_device *device, const fz_text *text,
                              const fz_stroke_state *stroke, fz_matrix matrix, fz_colorspace *space,
                              const float *color, float alpha, fz_color_params params) {
    (void)text;
    (void)stroke;
    (void)matrix;
    (void)color;
    (void)alpha;
    (void)params;
    audit_colorspace(ctx, device, space);
}

static void audit_fill_shade(fz_context *ctx, fz_device *device, fz_shade *shade, fz_matrix matrix,
                             float alpha, fz_color_params params) {
    (void)matrix;
    (void)alpha;
    (void)params;
    audit_colorspace(ctx, device, shade != NULL ? shade->colorspace : NULL);
}

static void audit_fill_image(fz_context *ctx, fz_device *device, fz_image *image, fz_matrix matrix,
                             float alpha, fz_color_params params) {
    (void)matrix;
    (void)alpha;
    (void)params;
    audit_colorspace(ctx, device, image != NULL ? image->colorspace : NULL);
}

static void audit_fill_image_mask(fz_context *ctx, fz_device *device, fz_image *image,
                                  fz_matrix matrix, fz_colorspace *space, const float *color,
                                  float alpha, fz_color_params params) {
    (void)image;
    (void)matrix;
    (void)color;
    (void)alpha;
    (void)params;
    audit_colorspace(ctx, device, space);
}

static void audit_begin_mask(fz_context *ctx, fz_device *device, fz_rect area, int luminosity,
                             fz_colorspace *space, const float *background,
                             fz_color_params params) {
    (void)area;
    (void)luminosity;
    (void)background;
    (void)params;
    audit_colorspace(ctx, device, space);
}

/* The parameter order is fixed by the MuPDF device callback ABI. */
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
static void audit_begin_group(fz_context *ctx, fz_device *device, fz_rect area,
                              fz_colorspace *space, int isolated, int knockout, int blend_mode,
                              float alpha) {
    (void)area;
    (void)isolated;
    (void)knockout;
    (void)blend_mode;
    (void)alpha;
    audit_colorspace(ctx, device, space);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

static struct vove_color_audit_device *new_color_audit_device(fz_context *ctx) {
    struct vove_color_audit_device *audit = (struct vove_color_audit_device *)fz_new_device_of_size(
        ctx, (int)sizeof(struct vove_color_audit_device));
    audit->super.fill_path = audit_fill_path;
    audit->super.stroke_path = audit_stroke_path;
    audit->super.fill_text = audit_fill_text;
    audit->super.stroke_text = audit_stroke_text;
    audit->super.fill_shade = audit_fill_shade;
    audit->super.fill_image = audit_fill_image;
    audit->super.fill_image_mask = audit_fill_image_mask;
    audit->super.begin_mask = audit_begin_mask;
    audit->super.begin_group = audit_begin_group;
    return audit;
}

static enum vove_mupdf_color_model audited_color_model(unsigned int spaces) {
    const unsigned int gray = (unsigned int)VOVE_COLOR_GRAY;
    const unsigned int cmyk = (unsigned int)VOVE_COLOR_CMYK;
    const unsigned int separation = (unsigned int)VOVE_COLOR_SEPARATION;
    if ((spaces & ~gray) != 0)
        spaces &= ~gray;
    if ((spaces & separation) != 0) {
        spaces &= ~separation;
        spaces |= cmyk;
    }
    if (spaces == 0)
        return VOVE_MUPDF_COLOR_UNKNOWN;
    if ((spaces & (spaces - 1U)) != 0)
        return VOVE_MUPDF_COLOR_MIXED;
    if ((spaces & VOVE_COLOR_GRAY) != 0)
        return VOVE_MUPDF_COLOR_GRAY;
    if ((spaces & VOVE_COLOR_RGB) != 0)
        return VOVE_MUPDF_COLOR_RGB;
    if ((spaces & VOVE_COLOR_CMYK) != 0 || (spaces & VOVE_COLOR_SEPARATION) != 0)
        return VOVE_MUPDF_COLOR_CMYK;
    if ((spaces & VOVE_COLOR_LAB) != 0)
        return VOVE_MUPDF_COLOR_LAB;
    return VOVE_MUPDF_COLOR_UNKNOWN;
}

static enum vove_mupdf_color_model color_model(fz_context *ctx, fz_colorspace *space) {
    if (space == NULL)
        return VOVE_MUPDF_COLOR_UNKNOWN;
    switch (fz_colorspace_type(ctx, space)) {
    case FZ_COLORSPACE_GRAY:
        return VOVE_MUPDF_COLOR_GRAY;
    case FZ_COLORSPACE_RGB:
    case FZ_COLORSPACE_BGR:
        return VOVE_MUPDF_COLOR_RGB;
    case FZ_COLORSPACE_CMYK:
    case FZ_COLORSPACE_SEPARATION:
        return VOVE_MUPDF_COLOR_CMYK;
    case FZ_COLORSPACE_LAB:
        return VOVE_MUPDF_COLOR_LAB;
    case FZ_COLORSPACE_NONE:
    case FZ_COLORSPACE_INDEXED:
        return VOVE_MUPDF_COLOR_UNKNOWN;
    }
    return VOVE_MUPDF_COLOR_UNKNOWN;
}

static int allocation_failure_message(int code, const char *message) {
    if (code != FZ_ERROR_SYSTEM || message == NULL)
        return 0;
    return strncmp(message, "malloc (", 8) == 0 || strncmp(message, "malloc array (", 14) == 0 ||
           strncmp(message, "realloc (", 9) == 0 || strncmp(message, "realloc array (", 15) == 0 ||
           strncmp(message, "calloc (", 8) == 0 ||
           strstr(message, "hash table resize failed; out of memory (") == message;
}

static enum vove_mupdf_status caught_status(int code, int system_errno, const char *message) {
    if (allocation_failure_message(code, message)) {
        /* MuPDF also reports checked allocation-size overflow as FZ_ERROR_SYSTEM. */
        return strstr(message, "(overflow)") != NULL ? VOVE_MUPDF_RESOURCE_LIMIT
                                                     : VOVE_MUPDF_MEMORY_LIMIT;
    }
    if (code == FZ_ERROR_SYSTEM && system_errno == ENOMEM)
        return VOVE_MUPDF_MEMORY_LIMIT;
    switch (code) {
    case FZ_ERROR_FORMAT:
    case FZ_ERROR_SYNTAX:
    case FZ_ERROR_ARGUMENT:
        return VOVE_MUPDF_MALFORMED;
    case FZ_ERROR_LIMIT:
        return VOVE_MUPDF_RESOURCE_LIMIT;
    case FZ_ERROR_UNSUPPORTED:
        return VOVE_MUPDF_UNSUPPORTED;
    default:
        return VOVE_MUPDF_INTERNAL_ERROR;
    }
}

void vove_mupdf_result_init(struct vove_mupdf_result *result) {
    if (result == NULL)
        return;
    *result = (struct vove_mupdf_result){0};
    result->status = VOVE_MUPDF_INTERNAL_ERROR;
}

void vove_mupdf_result_drop(struct vove_mupdf_result *result) {
    if (result == NULL)
        return;
    free(result->rgba8);
    vove_mupdf_result_init(result);
}

void vove_mupdf_render_pdf(FILE *source, const struct vove_mupdf_request *request,
                           const char *password_utf8, struct vove_mupdf_result *result) {
    fz_context *ctx = NULL;
    fz_document *document = NULL;
    fz_page *page = NULL;
    fz_pixmap *pixmap = NULL;
    fz_stream *stream = NULL;
    struct vove_color_audit_device *audit = NULL;
    unsigned char *rgba8 = NULL;
    size_t rgba8_bytes = 0;
    volatile int caught_code = 0;
    volatile int caught_errno = 0;
    char caught_message[513] = {0};
    size_t store_limit;

    if (result == NULL)
        return;
    vove_mupdf_result_init(result);
    if (source == NULL || request == NULL || request->maximum_input_bytes == 0 ||
        request->memory_limit_bytes == 0 || request->canonical_edge == 0 ||
        request->canonical_edge > 4096) {
        copy_text(result->diagnostic, sizeof(result->diagnostic), "invalid document limits");
        return;
    }

    /* The process limit also covers pixmaps, fonts and allocator overhead. Giving the
       MuPDF store the entire budget makes a healthy document fail at the OS limit. */
    store_limit = (size_t)(request->memory_limit_bytes / 2U);
    ctx = fz_new_context(NULL, NULL, store_limit);
    if (ctx == NULL) {
        result->status = VOVE_MUPDF_MEMORY_LIMIT;
        copy_text(result->diagnostic, sizeof(result->diagnostic), "MuPDF context is unavailable");
        return;
    }
    fz_set_error_callback(ctx, discard_message, NULL);
    fz_set_warning_callback(ctx, discard_message, NULL);
    fz_var(document);
    fz_var(page);
    fz_var(pixmap);
    fz_var(stream);
    fz_var(audit);
    fz_var(rgba8);
    fz_var(rgba8_bytes);
    fz_try(ctx) {
        int64_t source_length;
        int page_count;
        int needs_password;
        fz_rect bounds;
        float scale;
        fz_matrix transform;
        fz_colorspace *output_intent;
        int width;
        int height;
        int components;
        int alpha;
        int stride;
        unsigned char *samples;
        size_t row_bytes;
        size_t row;

        if (seek_file(source, 0, SEEK_END) != 0)
            fz_throw(ctx, FZ_ERROR_SYSTEM, "document size is unavailable");
        source_length = tell_file(source);
        if (source_length < 0 || (uint64_t)source_length > request->maximum_input_bytes) {
            result->status = VOVE_MUPDF_RESOURCE_LIMIT;
            copy_text(result->diagnostic, sizeof(result->diagnostic),
                      "document exceeds the input limit");
        } else if (seek_file(source, 0, SEEK_SET) != 0)
            fz_throw(ctx, FZ_ERROR_SYSTEM, "document rewind failed");
        else {
            stream = fz_open_file_ptr_no_close(ctx, source);
            document = (fz_document *)pdf_open_document_with_stream(ctx, stream);
            needs_password = fz_needs_password(ctx, document);
            if (needs_password && (password_utf8 == NULL || password_utf8[0] == '\0')) {
                result->status = VOVE_MUPDF_PASSWORD_REQUIRED;
                copy_text(result->diagnostic, sizeof(result->diagnostic), "password required");
            } else if (needs_password && !fz_authenticate_password(ctx, document, password_utf8)) {
                result->status = VOVE_MUPDF_AUTHENTICATION_FAILED;
                copy_text(result->diagnostic, sizeof(result->diagnostic), "password rejected");
            } else {
                page_count = fz_count_pages(ctx, document);
                if (page_count <= 0 || page_count > 1000000 ||
                    request->page_index >= (uint32_t)page_count)
                    fz_throw(ctx, FZ_ERROR_LIMIT, "document page count is outside limits");
                page = fz_load_page(ctx, document, (int)request->page_index);
                audit = new_color_audit_device(ctx);
                fz_run_page(ctx, page, &audit->super, fz_identity, NULL);
                fz_close_device(ctx, &audit->super);
                bounds = fz_bound_page(ctx, page);
                if (!isfinite(bounds.x0) || !isfinite(bounds.y0) || !isfinite(bounds.x1) ||
                    !isfinite(bounds.y1) || bounds.x1 <= bounds.x0 || bounds.y1 <= bounds.y0)
                    fz_throw(ctx, FZ_ERROR_FORMAT, "document page bounds are invalid");
                scale = (float)(request->canonical_edge > 1 ? request->canonical_edge - 1 : 1) /
                        fmaxf(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
                if (!isfinite(scale) || scale <= 0)
                    fz_throw(ctx, FZ_ERROR_FORMAT, "document page scale is invalid");
                transform = fz_scale(scale, scale);
                pixmap = fz_new_pixmap_from_page(ctx, page, transform, fz_device_rgb(ctx), 0);
                width = fz_pixmap_width(ctx, pixmap);
                height = fz_pixmap_height(ctx, pixmap);
                components = fz_pixmap_components(ctx, pixmap);
                alpha = fz_pixmap_alpha(ctx, pixmap);
                stride = fz_pixmap_stride(ctx, pixmap);
                if (width <= 0 || height <= 0 || width > (int)request->canonical_edge ||
                    height > (int)request->canonical_edge || components < 3 || stride <= 0)
                    fz_throw(ctx, FZ_ERROR_LIMIT, "rendered page exceeds pixel limits");
                row_bytes = (size_t)width * 4U;
                if ((size_t)height > SIZE_MAX / row_bytes)
                    fz_throw(ctx, FZ_ERROR_LIMIT, "rendered page byte count overflows");
                rgba8_bytes = row_bytes * (size_t)height;
                rgba8 = (unsigned char *)malloc(rgba8_bytes);
                if (rgba8 == NULL) {
                    errno = ENOMEM;
                    fz_throw(ctx, FZ_ERROR_SYSTEM, "rendered page allocation failed");
                }
                samples = fz_pixmap_samples(ctx, pixmap);
                for (row = 0; row < (size_t)height; ++row) {
                    const unsigned char *input = samples + row * (size_t)stride;
                    unsigned char *output = rgba8 + row * row_bytes;
                    int column;
                    for (column = 0; column < width; ++column) {
                        output[column * 4 + 0] = input[column * components + 0];
                        output[column * 4 + 1] = input[column * components + 1];
                        output[column * 4 + 2] = input[column * components + 2];
                        output[column * 4 + 3] =
                            alpha ? input[column * components + components - 1] : 255;
                    }
                }

                output_intent = fz_document_output_intent(ctx, document);
                result->color_model = audited_color_model(audit->color_spaces);
                if (result->color_model == VOVE_MUPDF_COLOR_UNKNOWN)
                    result->color_model = color_model(ctx, output_intent);
                if (output_intent != NULL) {
                    copy_text(result->color_profile, sizeof(result->color_profile),
                              fz_colorspace_name(ctx, output_intent));
                } else if (audit->conflicting_profiles) {
                    copy_text(result->color_profile, sizeof(result->color_profile),
                              "Multiple profiles");
                } else {
                    copy_text(result->color_profile, sizeof(result->color_profile), audit->profile);
                }
                result->status = VOVE_MUPDF_SUCCESS;
                result->width = (uint32_t)width;
                result->height = (uint32_t)height;
                result->page_index = request->page_index;
                result->page_count = (uint32_t)page_count;
                result->rgba8 = rgba8;
                result->rgba8_bytes = rgba8_bytes;
                rgba8 = NULL;
                rgba8_bytes = 0;
            }
        }
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pixmap);
        fz_drop_page(ctx, page);
        fz_drop_device(ctx, audit != NULL ? &audit->super : NULL);
        fz_drop_document(ctx, document);
        fz_drop_stream(ctx, stream);
    }
    fz_catch(ctx) {
        caught_code = fz_caught(ctx);
        if (caught_code == FZ_ERROR_SYSTEM)
            caught_errno = fz_caught_errno(ctx);
        copy_text(caught_message, sizeof(caught_message), fz_caught_message(ctx));
        fz_ignore_error(ctx);
    }

    if (caught_code != 0) {
        free(rgba8);
        free(result->rgba8);
        result->rgba8 = NULL;
        result->rgba8_bytes = 0;
        result->width = 0;
        result->height = 0;
        result->page_count = 0;
        result->page_index = 0;
        result->color_model = VOVE_MUPDF_COLOR_UNKNOWN;
        result->color_profile[0] = '\0';
        result->status = caught_status(caught_code, caught_errno, caught_message);
        copy_text(result->diagnostic, sizeof(result->diagnostic), caught_message);
    }
    fz_drop_context(ctx);
}
