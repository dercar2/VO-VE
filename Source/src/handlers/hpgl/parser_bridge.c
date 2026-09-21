/* Memory-only adapter for the pinned GNU hp2xx 3.4.4 parser.
 * All setjmp/longjmp frames and cleanup are C; callers receive a normal return.
 */
#include "memory_stdio.h"
#include <stdint.h>
#include <errno.h>
#include <limits.h>

/* Including this translation unit makes its per-document state resettable
 * without exporting upstream internals or copying its domain parser. */
#include "hpgl.c"

#define SOURCE_LIMIT ((size_t)32 * 1024 * 1024)
#define COMMAND_LIMIT ((size_t)16 * 1024 * 1024)

typedef struct {
    const unsigned char *input;
    unsigned char *output;
    size_t size, position;
    unsigned int eof_reads;
} MemoryStream;

static MemoryStream input_stream, command_stream;
static jmp_buf failure_boundary;
static int failure_status;
#ifdef VOVE_HPGL_PROBE
size_t vove_hpgl_probe_offset;
int vove_hpgl_probe_command;
#endif
#ifdef _WIN32
static _locale_t number_locale;
#else
static locale_t number_locale;
#endif

typedef char check_float_size[sizeof(float) == 4 ? 1 : -1];
typedef char check_int_size[sizeof(int) == 4 ? 1 : -1];
typedef char check_pen_size[sizeof(PEN_N) == 2 && sizeof(PEN_C) == 2 ? 1 : -1];
typedef char check_opcodes[MOVE_TO == 1 && DRAW_TO == 2 && PLOT_AT == 3 &&
    SET_PEN == 4 && DEF_PW == 5 && DEF_PC == 6 && DEF_LA == 7 ? 1 : -1];

void vove_hpgl_fail(int status)
{
#ifdef VOVE_HPGL_PROBE
    vove_hpgl_probe_offset = input_stream.position;
#endif
    failure_status = status;
    longjmp(failure_boundary, 1);
}

void vove_hpgl_exit(int code)
{
    (void)code;
    vove_hpgl_fail(VOVE_HPGL_MALFORMED);
}

static MemoryStream *memory_stream(FILE *stream)
{
    if (stream == (FILE *)&input_stream) return &input_stream;
    if (stream == (FILE *)&command_stream) return &command_stream;
    vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    return NULL;
}

int vove_hpgl_getc(FILE *stream)
{
    MemoryStream *memory = memory_stream(stream);
    if (memory->position >= memory->size) {
        /* A few upstream numeric lookaheads legitimately repeat EOF. */
        if (++memory->eof_reads > 64) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
        return EOF;
    }
    memory->eof_reads = 0;
    return memory->input[memory->position++];
}

int vove_hpgl_ungetc(int value, FILE *stream)
{
    MemoryStream *memory = memory_stream(stream);
    if (value == EOF) return EOF;
    if (memory->position == 0 || memory->input[memory->position - 1] != (unsigned char)value)
        vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    --memory->position;
    memory->eof_reads = 0;
    return value;
}

size_t vove_hpgl_write(const void *source, size_t size, size_t count, FILE *stream)
{
    MemoryStream *memory = memory_stream(stream);
    size_t bytes;
    if (size == 0 || count == 0) return 0;
    if (memory != &command_stream) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    if (size > COMMAND_LIMIT / count) vove_hpgl_fail(VOVE_HPGL_LIMIT);
    bytes = size * count;
    if (bytes > COMMAND_LIMIT - memory->position) vove_hpgl_fail(VOVE_HPGL_LIMIT);
    memcpy(memory->output + memory->position, source, bytes);
    memory->position += bytes;
    if (memory->size < memory->position) memory->size = memory->position;
    return count;
}

int vove_hpgl_putc(int value, FILE *stream)
{
    unsigned char byte = (unsigned char)value;
    vove_hpgl_write(&byte, 1, 1, stream);
    return byte;
}

size_t vove_hpgl_read(void *destination, size_t size, size_t count, FILE *stream)
{
    MemoryStream *memory = memory_stream(stream);
    size_t available;
    if (size == 0 || count == 0) return 0;
    available = (memory->size - memory->position) / size;
    if (count > available) count = available;
    memcpy(destination, memory->input + memory->position, count * size);
    memory->position += count * size;
    return count;
}

int vove_hpgl_seek(FILE *stream, long offset, int origin)
{
    MemoryStream *memory = memory_stream(stream);
    int64_t position = offset;
    if (origin == SEEK_CUR) position += (int64_t)memory->position;
    else if (origin == SEEK_END) position += (int64_t)memory->size;
    else if (origin != SEEK_SET) return -1;
    if (position < 0 || (uint64_t)position > memory->size) return -1;
    memory->position = (size_t)position;
    memory->eof_reads = 0;
    return 0;
}

int vove_hpgl_fprintf(FILE *stream, const char *format, ...)
{
    (void)stream; (void)format;
    return 0;
}

int vove_hpgl_printf(const char *format, ...) { (void)format; return 0; }
void Eprintf(const char *format, ...) { (void)format; }
void PError(const char *message) { (void)message; }
void NormalWait(void) {}
void SilentWait(void) {}

int vove_hpgl_number(const char *text, float *number)
{
    char *end;
    errno = 0;
#ifdef _WIN32
    *number = _strtof_l(text, &end, number_locale);
#else
    *number = strtof_l(text, &end, number_locale);
#endif
    if (end == text || *end != 0 || !isfinite(*number) || errno == ERANGE) return 0;
    if (fabs(*number) > 16777216.0) vove_hpgl_fail(VOVE_HPGL_LIMIT);
    return 1;
}

void vove_hpgl_reset_styles(void)
{
    int index;
    if (record_off) return;
    for (index = 0; index < NUMPENS; ++index) {
        PEN_N number = (PEN_N)index;
        PEN_C red = pt.clut[pt.color[index]][0];
        PEN_C green = pt.clut[pt.color[index]][1];
        PEN_C blue = pt.clut[pt.color[index]][2];
        putc(DEF_PC, td);
        fwrite(&number, sizeof(number), 1, td);
        fwrite(&red, sizeof(red), 1, td);
        fwrite(&green, sizeof(green), 1, td);
        fwrite(&blue, sizeof(blue), 1, td);
        if (index != 0) {
            putc(DEF_PW, td);
            fwrite(&number, sizeof(number), 1, td);
            fwrite(&pt.width[index], sizeof(PEN_W), 1, td);
        }
    }
    putc(DEF_LA, td); Line_Attr_to_tmpfile(LineAttrEnd, LAE_butt);
    putc(DEF_LA, td); Line_Attr_to_tmpfile(LineAttrJoin, LAJ_plain_miter);
    putc(DEF_LA, td); Line_Attr_to_tmpfile(LineAttrLimit, 5);
    putc(SET_PEN, td); putc(1, td);
}

void vove_hpgl_check_command(int command)
{
#ifdef VOVE_HPGL_PROBE
    vove_hpgl_probe_command = command;
#endif
    /* These upstream paths silently substitute missing fonts/fills or use
     * encodings outside this initial viewer integration. Ordinary HPGL
     * vectors, curves, polygons, and built-in stroked labels stay upstream. */
    switch (command) {
    case AD: case SD: case PE: case PT:
        vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);
    default: break;
    }
}

int vove_hpgl_current_pen(void)
{
    if (pen < 0 || pen >= NUMPENS) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    return pen;
}

static void cleanup_parser(void)
{
    free(strbuf);
    strbuf = NULL;
    strbufsize = MAX_LB_LEN + 1;
#ifdef _WIN32
    if (number_locale) _free_locale(number_locale);
#else
    if (number_locale) freelocale(number_locale);
#endif
    number_locale = 0;
    td = NULL;
    free(command_stream.output);
    memset(&command_stream, 0, sizeof(command_stream));
    memset(&input_stream, 0, sizeof(input_stream));
}

int vove_hpgl_parse(const unsigned char *source, size_t size, struct VoveHpglParsed *result)
{
    GEN_PAR general;
    IN_PAR input;
    int index;
    if (!result) return VOVE_HPGL_MALFORMED;
    memset(result, 0, sizeof(*result));
    if (size > SOURCE_LIMIT) return VOVE_HPGL_LIMIT;
    if (!source || size == 0) return VOVE_HPGL_MALFORMED;
    memset(&input_stream, 0, sizeof(input_stream));
    memset(&command_stream, 0, sizeof(command_stream));
    input_stream.input = source;
    input_stream.size = size;
    command_stream.output = malloc(COMMAND_LIMIT);
    command_stream.input = command_stream.output;
    if (!command_stream.output) {
        cleanup_parser();
        return VOVE_HPGL_LIMIT;
    }
#ifdef _WIN32
    number_locale = _create_locale(LC_NUMERIC, "C");
#else
    number_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
#endif
    if (!number_locale) {
        cleanup_parser();
        return VOVE_HPGL_LIMIT;
    }
    failure_status = VOVE_HPGL_MALFORMED;
    if (setjmp(failure_boundary)) {
        cleanup_parser();
        return failure_status;
    }
    memset(&general, 0, sizeof(general));
    memset(&input, 0, sizeof(input));
    memset(&pt, 0, sizeof(pt));
    for (index = 1; index <= NUMPENS; ++index) {
        pt.width[index] = 0.1f;
        pt.color[index] = xxForeground;
    }
    general.quiet = 1;
    general.mapzero = -1;
    general.maxpensize = 0.1;
    general.td = (FILE *)&command_stream;
    general.xx_mode = XX_EPS;
    input.hd = (FILE *)&input_stream;
    input.first_page = input.last_page = 1;
    input.x0 = input.y0 = 1e10;
    input.x1 = input.y1 = -1e10;
    input.hwlimit.x = 33600;
    input.hwlimit.y = 47520;
    page_number = 1;
    pg_flag = polygon_mode = polygon_penup = saved_penstate = FALSE;
    vertices = -1;
    thickness = hatchspace = hatchangle = 0;
    rot_tmp = rot_ang = 0;
    anchor.x = anchor.y = 100000;
    polystart.x = polystart.y = 0;
    CurrentLineEnd = LAE_butt;
    read_HPGL(&general, &input);
    if (polygon_mode || n_unexpected) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    if (n_unknown || n_commands == 0) vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);
    result->commands = command_stream.output;
    result->size = command_stream.size;
    result->xmin = xmin; result->ymin = ymin;
    result->xmax = xmax; result->ymax = ymax;
    command_stream.output = NULL;
    cleanup_parser();
    return VOVE_HPGL_OK;
}

void vove_hpgl_release(struct VoveHpglParsed *result)
{
    if (!result) return;
    free(result->commands);
    memset(result, 0, sizeof(*result));
}
