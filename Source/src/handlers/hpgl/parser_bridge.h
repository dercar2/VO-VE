#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum VoveHpglStatus { VOVE_HPGL_OK, VOVE_HPGL_UNSUPPORTED, VOVE_HPGL_MALFORMED, VOVE_HPGL_LIMIT };
struct VoveHpglParsed {
    unsigned char *commands;
    size_t size;
    float xmin, ymin, xmax, ymax;
};

/* Byte opcode, then native-endian fields without padding:
 * 1 MOVE, 2 DRAW, 3 PLOT: float x, float y (HPGL units, 40/mm).
 * 4 SET_PEN: unsigned char pen.
 * 5 DEF_PW: uint16 pen, float width_mm (pen 0 sets pens 1..255).
 * 6 DEF_PC: uint16 pen, r, g, b (components 0..255).
 * 7 DEF_LA: int32 kind, int32 value; kind 0=end, 1=join, 2=miter limit.
 * Initial/reset styles are explicit records, followed by SET_PEN 1.
 * HPGL defaults to pen 1; explicit SP or SP0 still emits SET_PEN 0.
 * There is no EOF record; size is the exact command-stream extent.
 * Calls must be serialized: upstream parser state is process-global.
 * On failure result is empty; release successful results with vove_hpgl_release.
 */
int vove_hpgl_parse(const unsigned char *source, size_t size, struct VoveHpglParsed *result);
void vove_hpgl_release(struct VoveHpglParsed *result);

#ifdef __cplusplus
}
#endif
