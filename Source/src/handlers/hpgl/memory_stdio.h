#pragma once

// Include CRT declarations before redirecting only the pinned parser's stdio calls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <locale.h>
#include <setjmp.h>
#include "parser_bridge.h"

int vove_hpgl_getc(FILE *stream);
int vove_hpgl_ungetc(int value, FILE *stream);
int vove_hpgl_putc(int value, FILE *stream);
size_t vove_hpgl_read(void *destination, size_t size, size_t count, FILE *stream);
size_t vove_hpgl_write(const void *source, size_t size, size_t count, FILE *stream);
int vove_hpgl_seek(FILE *stream, long offset, int origin);
int vove_hpgl_fprintf(FILE *stream, const char *format, ...);
int vove_hpgl_printf(const char *format, ...);
void vove_hpgl_exit(int code);
void vove_hpgl_fail(int status);
int vove_hpgl_number(const char *text, float *number);
void vove_hpgl_reset_styles(void);
void vove_hpgl_check_command(int command);
int vove_hpgl_current_pen(void);

#undef getc
#undef fgetc
#undef ungetc
#undef putc
#undef fputc
#define getc vove_hpgl_getc
#define fgetc vove_hpgl_getc
#define ungetc vove_hpgl_ungetc
#define putc vove_hpgl_putc
#define fputc vove_hpgl_putc
#define fread vove_hpgl_read
#define fwrite vove_hpgl_write
#define fseek vove_hpgl_seek
#define fprintf vove_hpgl_fprintf
#define printf vove_hpgl_printf
#define exit vove_hpgl_exit

// These units must never acquire a file or create scratch storage.
#define fopen VOVE_HPGL_FILESYSTEM_OPEN_IS_FORBIDDEN
#define freopen VOVE_HPGL_FILESYSTEM_REOPEN_IS_FORBIDDEN
#define tmpfile VOVE_HPGL_TEMPFILE_IS_FORBIDDEN
