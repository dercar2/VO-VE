/* Optional local-fixture harness; not linked into the viewer or its worker. */
#include "parser_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern size_t vove_hpgl_probe_offset;
extern int vove_hpgl_probe_command;

static int self_test(void)
{
    static const struct { const char *text; int expected; } cases[] = {
        {"IN;UL1,2,2,2,2,2,2,2,2,2,14;LT1;PD1000,1000;", VOVE_HPGL_OK},
        {"IN;UL1,.25,.75;LT-1;PD1000,1000;", VOVE_HPGL_OK},
        {"IN;UL1,1,1;UL1;LT1;PD1000,1000;", VOVE_HPGL_OK},
        {"IN;UL1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1;PD100,100;", VOVE_HPGL_OK},
        {"IN;UL1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1;", VOVE_HPGL_LIMIT},
        {"IN;UL1,0,0;", VOVE_HPGL_MALFORMED},
        {"IN;UL1,1,-2;", VOVE_HPGL_MALFORMED},
        {"IN;UL9,1,1;", VOVE_HPGL_MALFORMED},
        {"IN;LBunfinished", VOVE_HPGL_MALFORMED},
        {"IN;DV1;PU0,0;LBA\nB\003;", VOVE_HPGL_OK}
    };
    const char *reference = "SP1;PU0,0;LBA\nB\003;";
    struct VoveHpglParsed baseline, result;
    char resets[15001];
    size_t i;
    int status;
    if (vove_hpgl_parse((const unsigned char *)reference, strlen(reference), &baseline)) return 1;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        status = vove_hpgl_parse((const unsigned char *)cases[i].text, strlen(cases[i].text), &result);
        if (status != cases[i].expected || (status && (result.commands || result.size))) {
            printf("self-test case %zu status=%d expected=%d\n", i, status, cases[i].expected);
            vove_hpgl_release(&result); vove_hpgl_release(&baseline); return 1;
        }
        vove_hpgl_release(&result);
        status = vove_hpgl_parse((const unsigned char *)reference, strlen(reference), &result);
        if (status || result.size != baseline.size || memcmp(result.commands, baseline.commands, baseline.size)) {
            printf("self-test state recovery after case %zu failed\n", i);
            vove_hpgl_release(&result); vove_hpgl_release(&baseline); return 1;
        }
        vove_hpgl_release(&result);
    }
    vove_hpgl_release(&baseline);
    for (i = 0; i < sizeof(resets) - 1; i += 3) memcpy(resets + i, "IN;", 3);
    status = vove_hpgl_parse((const unsigned char *)resets, sizeof(resets) - 1, &result);
    if (status != VOVE_HPGL_LIMIT || result.commands || result.size) {
        vove_hpgl_release(&result); return 1;
    }
    if (vove_hpgl_parse((const unsigned char *)reference, 32U * 1024U * 1024U + 1U, &result) != VOVE_HPGL_LIMIT)
        return 1;
    puts("Parser bounds and state-recovery self-tests passed");
    return 0;
}

int main(int argc, char **argv)
{
    FILE *file;
    long length;
    unsigned char *source;
    struct VoveHpglParsed parsed;
    int status;
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "--self-test")) return self_test();
    file = fopen(argv[1], "rb");
    if (!file) return 2;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        length > 32L * 1024 * 1024 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return 2;
    }
    source = malloc(length ? (size_t)length : 1);
    if (!source) { fclose(file); return 2; }
    if (fread(source, 1, (size_t)length, file) != (size_t)length) {
        free(source); fclose(file); return 2;
    }
    fclose(file);
    status = vove_hpgl_parse(source, (size_t)length, &parsed);
    printf("status=%d source=%ld commands=%zu bounds=%g,%g,%g,%g\n",
           status, length, parsed.size, parsed.xmin, parsed.ymin, parsed.xmax, parsed.ymax);
    if (status != VOVE_HPGL_OK)
        printf("stop_offset=%zu last_command=0x%04x\n",
               vove_hpgl_probe_offset, (unsigned int)vove_hpgl_probe_command);
    vove_hpgl_release(&parsed);
    free(source);
    return status == VOVE_HPGL_OK ? 0 : 1;
}
