# Applied only to a freshly extracted, hash-verified GNU hp2xx 3.4.4 tree.
function(vove_patch_hp2xx directory)
    function(hp_replace before after)
        string(FIND "${text}" "${before}" position)
        if(position LESS 0)
            message(FATAL_ERROR "hp2xx patch anchor not found: ${before}")
        endif()
        string(REPLACE "${before}" "${after}" text "${text}")
        set(text "${text}" PARENT_SCOPE)
    endfunction()
    function(hp_section first last replacement)
        string(FIND "${text}" "${first}" begin)
        string(FIND "${text}" "${last}" end)
        if(begin LESS 0 OR end LESS_EQUAL begin)
            message(FATAL_ERROR "hp2xx section patch mismatch: ${first}")
        endif()
        string(SUBSTRING "${text}" 0 ${begin} prefix)
        string(SUBSTRING "${text}" ${end} -1 suffix)
        set(text "${prefix}${replacement}\n\n${suffix}" PARENT_SCOPE)
    endfunction()

    file(READ "${directory}/hpgl.c" text)
    # Upstream error reporting copies a label into a 21-byte stack buffer.
    hp_section("static void par_err_exit(" "static void reset_HPGL(void)" [=[
static void par_err_exit(int code, int cmd, FILE *hd)
{
    (void)code; (void)cmd; (void)hd;
    vove_hpgl_fail(VOVE_HPGL_MALFORMED);
}
]=])
    # Every remaining caller uses the bounded global label buffer, never a stack buffer.
    hp_section("void read_string(char *buf, FILE * hd)" "static void read_symbol_char(FILE * hd)" [=[
void read_string(char *buf, FILE *hd)
{
    size_t count = 0;
    int c;
    (void)buf;
    while ((c = getc(hd)) != EOF && c != (unsigned char)StrTerm) {
        if (c == 0) continue;
        if (c >= 128) vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);
        if (count >= 65534) vove_hpgl_fail(VOVE_HPGL_LIMIT);
        if (count + 2 >= strbufsize) {
            unsigned int capacity = strbufsize * 2;
            char *grown;
            if (capacity > 65536) capacity = 65536;
            grown = realloc(strbuf, capacity);
            if (!grown) vove_hpgl_fail(VOVE_HPGL_LIMIT);
            strbuf = grown; strbufsize = capacity;
        }
        strbuf[count++] = (char)c;
    }
    if (c == EOF) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    if (StrTermSilent == 0) strbuf[count++] = (char)c;
    strbuf[count] = 0;
}
]=])
    hp_replace("read_string(tmpstr, hd);" "read_string(strbuf, hd);")
    hp_replace("strlen(tmpstr)" "strlen(strbuf)")
    hp_replace("tmpstr[strlen(strbuf) - 1]" "strbuf[strlen(strbuf) - 1]")
    hp_replace("printf(\"\\n%s\\n\", tmpstr);" "printf(\"\\n%s\\n\", strbuf);")
    hp_replace("*ptr++ = c;\t/* Read number          */" [=[{
        if (ptr >= numbuf + sizeof(numbuf) - 1) vove_hpgl_fail(VOVE_HPGL_LIMIT);
        *ptr++ = (char)c;
    }]=])
    hp_replace("((c >= 'a') && (c <= 'a'))" "((c >= 'a') && (c <= 'z'))")
    hp_replace("if (sscanf(numbuf, \"%f\", pnum) != 1)\n\t\treturn 11;\t/* Should never happen  */"
               "if (!vove_hpgl_number(numbuf, pnum)) vove_hpgl_fail(VOVE_HPGL_MALFORMED);")
    hp_replace("polygons[++vertices] = pf;" [=[if (vertices >= MAXPOLY - 1) vove_hpgl_fail(VOVE_HPGL_LIMIT);
    if (!isfinite(pf.x) || !isfinite(pf.y)) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    polygons[++vertices] = pf;]=])
    hp_replace("if (fwrite((VOID *) pf, sizeof(*pf), 1, td) != 1)" [=[if (!isfinite(pf->x) || !isfinite(pf->y)) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    if (fabs(pf->x) > 1e9 || fabs(pf->y) > 1e9) vove_hpgl_fail(VOVE_HPGL_LIMIT);
    if (fwrite((VOID *) pf, sizeof(*pf), 1, td) != 1)]=])
    # Make the pen-position state explicitly resettable between independent memory calls.
    hp_replace("static HPGL_Pt p_last =" "static HPGL_Pt P_last;\nstatic HPGL_Pt p_last =")
    hp_replace("\tstatic HPGL_Pt P_last;" "")
    hp_replace("pen = -1;\n/*  n_unexpected" "pen = 1;\n    P_last.x = P_last.y = 0;\n/*  n_unexpected")
    hp_replace("StrTermSilent = 1;\n\tif (strbuf == NULL) {"
               "StrTermSilent = 1;\n    mode_vert = 0;\n\tif (strbuf == NULL) {")
    hp_replace("strbuf = malloc(strbufsize);"
               "strbuf = malloc(strbufsize);\n        if (!strbuf) vove_hpgl_fail(VOVE_HPGL_LIMIT);")
    hp_replace("\t    || ((last_page < page_number) && (last_page > 0));\n}\n\nstatic void init_HPGL"
               "\t    || ((last_page < page_number) && (last_page > 0));\n    vove_hpgl_reset_styles();\n}\n\nstatic void init_HPGL")
    hp_replace("switch (cmd & 0xDFDF) {" "vove_hpgl_check_command(cmd & 0xDFDF);\n\tswitch (cmd & 0xDFDF) {")
    hp_replace("pen = (short) p1.x;" [=[if (p1.x < 0 || p1.x >= NUMPENS || p1.x != floor(p1.x))
                vove_hpgl_fail(VOVE_HPGL_MALFORMED);
            pen = (short)p1.x;]=])
    hp_replace("pg->maxpens = (int) ftmp;" [=[if (ftmp < 1 || ftmp > NUMPENS || ftmp != floor(ftmp))
                vove_hpgl_fail(VOVE_HPGL_MALFORMED);
            pg->maxpens = MIN((int)ftmp, NUMPENS - 1);]=])
    hp_replace("if (pen < 0 || (int) pen > pg->maxpens) {"
               "if (pen < 0 || (int) pen > pg->maxpens) {\n            vove_hpgl_fail(VOVE_HPGL_MALFORMED);")
    hp_replace("filltype = (int) ftmp;" [=[filltype = (int)ftmp;
            if (filltype < 1 || filltype > 4) vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);]=])
    # Keep explicit thin vector strokes; the historical raster minimum is not
    # appropriate for SVG. Pen_Width_to_tmpfile enforces finite bounds below.
    hp_replace("\t\t\tif (mywidth < 0.1)\n\t\t\t\tmywidth = 0.1;" "")
    hp_replace("CurrentLinePattern = (int) p1.x;" [=[if (p1.x < LT_MIN || p1.x > LT_MAX || p1.x != floor(p1.x))
                vove_hpgl_fail(VOVE_HPGL_MALFORMED);
            CurrentLinePattern = (int)p1.x;]=])
    hp_replace("if (p1.y <= 0.0)" "if (p1.y <= 0.0) vove_hpgl_fail(VOVE_HPGL_MALFORMED);\n                if (p1.y <= 0.0)")
    hp_replace("while ((c = getc(pi->hd)) != EOF) {" "while ((c = getc(pi->hd)) != EOF) {\n        c = toupper((unsigned char)c);")
    # ESC%0A/1A must leave HPGL mode even when diagnostic output is disabled.
    hp_replace("if (hp && !silent_mode) {" "if (hp) {")
    hp_replace("\t\t\t\tbreak;\n\t\t\tif (c == 'P')" [=[{
                    if (c == ';' || c == ETX || isspace((unsigned char)c)) break;
                    vove_hpgl_fail(VOVE_HPGL_MALFORMED);
                }
            if (c == 'P')]=])
    hp_replace("if (cmd == EOF)\n\t\t\t\t\t\treturn;" "if (cmd == EOF) vove_hpgl_fail(VOVE_HPGL_MALFORMED);")
    hp_replace("if ((cmd = getc(pi->hd)) == 'G')" "if ((cmd = getc(pi->hd)) != EOF && (cmd & 0xDF) == 'G')")
    hp_replace("if ((cmd = getc(pi->hd)) == 'R')" "if ((cmd = getc(pi->hd)) != EOF && (cmd & 0xDF) == 'R')")
    hp_replace("if (cmd == 'F' || cmd == 'H')" "if ((cmd & 0xDF) == 'F' || (cmd & 0xDF) == 'H')")
    hp_replace("if ((c = getc(pi->hd)) == EOF)\n\t\t\t\treturn;"
               "if ((c = getc(pi->hd)) == EOF) vove_hpgl_fail(VOVE_HPGL_MALFORMED);")
    hp_replace("read_HPGL_cmd(pg, cmd, pi->hd);"
               "read_HPGL_cmd(pg, cmd, pi->hd);\n            if (page_number > 1) goto END;")
    # Bare BP is ordinary plot initialization. Parameterized BP has printer
    # metadata/autorotation semantics that this preview does not implement.
    hp_section("\tcase BP:\t\t/* Begin Plot */" "\tcase DF:\t\t/* Set to default" [=[
    case BP:
        if (!read_float(&ftmp, hd)) vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);
        /* Fall through to upstream initialization. */
]=])
    # The common default range is exact. Avoid claiming custom CR support:
    # upstream stores range endpoints as bytes and divides by the endpoint.
    hp_section("\tcase CR:\t\t/* color range */" "\tcase CS:\t\t/*character set selection" [=[
    case CR:
        for (i = 0; i < 6; ++i) {
            if (read_float(&ftmp, hd)) {
                if (i == 0) break;
                vove_hpgl_fail(VOVE_HPGL_MALFORMED);
            }
            if (ftmp != ((i % 2) ? 255.0f : 0.0f))
                vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);
        }
        r_base = g_base = b_base = 0;
        r_max = g_max = b_max = 255;
        break;
    case ('M' << 8) | 'C':
    case ('T' << 8) | 'R':
        if (read_float(&ftmp, hd) || ftmp != 0)
            vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);
        break;
]=])
    # The filler pairs intersections: only even-odd (default/FP0) is implemented.
    hp_replace("case FP:\t\t/* fill polygon */" [=[case FP: /* fill polygon */
        if (!read_float(&ftmp, hd) && ftmp != 0)
            vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);]=])
    file(WRITE "${directory}/hpgl.c" "${text}")

    file(READ "${directory}/pendef.c" text)
    hp_replace("tp = (PEN_N) pen;" [=[if (pen < 0 || pen >= NUMPENS) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    tp = (PEN_N)pen;]=])
    hp_replace("tw = width;" [=[if (!isfinite(width) || width < 0) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    if (width > 1000) vove_hpgl_fail(VOVE_HPGL_LIMIT);
    tw = width;]=])
    hp_replace("r = (PEN_C) red;" [=[if (red < 0 || red > 255 || green < 0 || green > 255 || blue < 0 || blue > 255)
        vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    r = (PEN_C)red;]=])
    # Maintain the parser-side palette so IN/DF snapshots contain the actual current state.
    hp_replace("b = (PEN_C) blue;" "b = (PEN_C) blue;\n    set_color_rgb(tp, (BYTE)r, (BYTE)g, (BYTE)b);")
    file(WRITE "${directory}/pendef.c" "${text}")

    file(READ "${directory}/fillpoly.c" text)
    # numpoints is the final index of an array of endpoint pairs, not vertex count.
    hp_replace("PEN_W SafePenW = pt.width[1];" [=[int active_pen = vove_hpgl_current_pen();
    PEN_W SafePenW = pt.width[active_pen];
    if (active_pen == 0 || numpoints < 1) return;
    if (numpoints >= MAXPOLY || numpoints % 2 == 0) vove_hpgl_fail(VOVE_HPGL_MALFORMED);]=])
    hp_replace("Pen_Width_to_tmpfile(1, penwidth);" "Pen_Width_to_tmpfile(active_pen, penwidth);")
    hp_replace("Pen_Width_to_tmpfile(1, SafePenW);" "Pen_Width_to_tmpfile(active_pen, SafePenW);")
    hp_replace("fprintf(stderr, \"zero area polygon\\n\");\n\t\treturn;"
               "goto VOVE_FILL_RESTORE;")
    hp_replace("\n\tCurrentLineEnd = SafeLineEnd;\n\tPlotCmd_to_tmpfile(DEF_PW);"
               "\nVOVE_FILL_RESTORE:\n\tCurrentLineEnd = SafeLineEnd;\n\tPlotCmd_to_tmpfile(DEF_PW);")
    file(WRITE "${directory}/fillpoly.c" "${text}")

    # A selected but unused character set cannot affect a drawing. Fail only
    # when upstream would actually replace its glyphs or glyph centering.
    file(READ "${directory}/chardraw.c" text)
    hp_replace("default:\t\t/* Currently, only charsets 0-7,30-39 are supported     */"
        "default: /* No silent fallback for an unavailable glyph set. */\n        vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);")
    hp_replace("default:\t\t/* Currently, there is just one charset */"
        "default: /* No silent fallback for unavailable glyph centering. */\n        vove_hpgl_fail(VOVE_HPGL_UNSUPPORTED);")
    file(WRITE "${directory}/chardraw.c" "${text}")

    # Clang reports undefined va_start use on the promoted signed-char formal.
    foreach(file lindef.h lindef.c)
        file(READ "${directory}/${file}" text)
        hp_replace("set_line_style(SCHAR index, ...)" "set_line_style(int index, ...)")
        file(WRITE "${directory}/${file}" "${text}")
    endforeach()

    file(READ "${directory}/lindef.c" text)
    hp_replace("index = (int) tmp;" [=[if (tmp < 1 || tmp > LT_MAX || tmp != floor(tmp))
            vove_hpgl_fail(VOVE_HPGL_MALFORMED);
        index = (int)tmp;]=])
    hp_replace("lt[pos_index][count] = (double) tmp;" [=[/* Leave room for the adaptive end segment and sentinel. */
        if (count >= LT_ELEMENTS - 1) vove_hpgl_fail(VOVE_HPGL_LIMIT);
        if (!isfinite(tmp) || tmp < 0) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
        lt[pos_index][count] = (double)tmp;]=])
    hp_replace("percentage += (int) tmp;" "percentage += (double)tmp;")
    hp_replace("lt[pos_index][count] = -1;" [=[if (count == 0) {
        LINESTYLE saved;
        memcpy(saved, lt, sizeof(saved));
        set_line_style_defaults();
        memcpy(saved[pos_index], lt[pos_index], sizeof(saved[pos_index]));
        memcpy(saved[neg_index], lt[neg_index], sizeof(saved[neg_index]));
        memcpy(lt, saved, sizeof(lt));
        return;
    }
    if (!(percentage > 0)) vove_hpgl_fail(VOVE_HPGL_MALFORMED);
    lt[pos_index][count] = -1;]=])
    file(WRITE "${directory}/lindef.c" "${text}")
endfunction()
