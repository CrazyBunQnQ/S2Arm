// Byte-parity check for the compressed-header writer pair: reads the canonical cases dumped by the managed
// test (`vp9-hdr` mode), replays them through s2a_compressed_write, and diffs the emitted bytes. Inputs come
// from the dump — a case table duplicated in both writers could carry a matching typo.
#include "hdr.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static size_t unhex(const char *s, uint8_t *dst, size_t cap)
{
    size_t n = 0;
    while (hexval(s[0]) >= 0 && hexval(s[1]) >= 0 && n < cap)
    {
        dst[n++] = (uint8_t)(hexval(s[0]) * 16 + hexval(s[1]));
        s += 2;
    }
    return n;
}

int main(void)
{
    char line[8200];
    uint8_t defs[3087], probs[3087], got[3456], want[3456];
    if (!fgets(line, sizeof line, stdin) || strncmp(line, "defaults ", 9)) { fprintf(stderr, "no defaults\n"); return 1; }
    if (unhex(line + 9, defs, sizeof defs) != sizeof defs) { fprintf(stderr, "short defaults\n"); return 1; }
    int fails = 0, cases = 0;
    while (fgets(line, sizeof line, stdin))
    {
        if (strncmp(line, "case ", 5)) continue;
        int id = atoi(line + 5);
        // pic k,i,e,l,tx,ref,interp,hp,b1,b2,b3
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "pic ", 4)) { fprintf(stderr, "case %d no pic\n", id); return 1; }
        s2a_pic p; memset(&p, 0, sizeof p);
        int v[11] = {0};
        sscanf(line + 4, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]);
        p.flags = (v[0] ? S2A_KEY : 0) | (v[1] ? S2A_INTRA_ONLY : 0) | (v[2] ? S2A_ERROR_RES : 0);
        p.lossless = (uint8_t)v[3];
        p.tx_mode = (uint8_t)v[4]; p.ref_mode = (uint8_t)v[5]; p.interp = (uint8_t)v[6]; p.allow_hp = (uint8_t)v[7];
        p.sign_bias[1] = (int8_t)v[8]; p.sign_bias[2] = (int8_t)v[9]; p.sign_bias[3] = (int8_t)v[10];
        memcpy(probs, defs, sizeof probs);
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "muts ", 5)) { fprintf(stderr, "case %d no muts\n", id); return 1; }
        for (char *t = strtok(line + 5, " \n"); t; t = strtok(NULL, " \n"))
        {
            int off = atoi(t);
            if (off >= 0 && off < (int)sizeof probs) probs[off] = (uint8_t)((probs[off] | 1) + 2);
        }
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "compressed ", 11)) { fprintf(stderr, "case %d no output\n", id); return 1; }
        size_t want_len = unhex(line + 11, want, sizeof want);
        int n = s2a_compressed_write(&p, probs, defs, got, sizeof got);
        cases++;
        if (n < 0 || (size_t)n != want_len || memcmp(got, want, want_len))
        {
            fails++;
            printf("case %d MISMATCH c=%zu cs=%d\n", id, want_len, n);
        }
        else
        {
            printf("case %d match %zu bytes\n", id, want_len);
        }
        // consume 'end'
        while (fgets(line, sizeof line, stdin) && strncmp(line, "end", 3)) { }
    }
    printf("%s: %d/%d cases byte-identical\n", fails ? "FAIL" : "PASS", cases - fails, cases);
    return fails ? 1 : 0;
}
