/* What does opcode diversity actually look like in real Alpha code versus in
 * the numeric data that false-positives?
 *
 * Replicates alpha_align_of exactly -- same mask, same AFMIN/AGAP test, same
 * choice of winning alignment -- then, for every window that passes, reports
 * the distinct opcode count and the share held by the most common opcode.
 * Choosing ADIV/ADOM from these two distributions beats sweeping thresholds
 * against a 300-second file, and is the same measure-first approach that fixed
 * the stride detector.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SEGWIN 8192
#ifndef AFMIN
#define AFMIN 45
#endif
#ifndef AGAP
#define AGAP 12
#endif

static const uint64_t ALPHA_OPS =
    (1ULL << 0x08) | (1ULL << 0x09) | (1ULL << 0x0a) | (1ULL << 0x0c) |
    (1ULL << 0x0d) | (1ULL << 0x0e) | (1ULL << 0x10) | (1ULL << 0x11) |
    (1ULL << 0x12) | (1ULL << 0x13) | (1ULL << 0x14) | (1ULL << 0x15) |
    (1ULL << 0x16) | (1ULL << 0x17) | (1ULL << 0x18) | (1ULL << 0x1a) |
    0xFFFFFFFF00000000ULL;

int main(int argc, char **argv) {
    for (int a = 1; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb");
        if (!fp) { perror(argv[a]); continue; }
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint8_t *d = malloc((size_t)n);
        if (!d || fread(d, 1, (size_t)n, fp) != (size_t)n) return 1;
        fclose(fp);

        long passed = 0, total = 0;
        long dcnt[65];
        long scnt[11];
        memset(dcnt, 0, sizeof dcnt);
        memset(scnt, 0, sizeof scnt);

        for (size_t off = 0; off + SEGWIN <= (size_t)n; off += SEGWIN) {
            const uint8_t *w = d + off;
            int seen[4][64], tot[4], hit[4];
            memset(seen, 0, sizeof seen);
            memset(tot, 0, sizeof tot);
            memset(hit, 0, sizeof hit);
            for (int al = 0; al < 4; al++) {
                size_t st = (size_t)((al - (int)(off & 3)) & 3);
                for (size_t i = st; i + 4 <= SEGWIN; i += 4) {
                    int op = w[i + 3] >> 2;
                    if (ALPHA_OPS >> op & 1) hit[al]++;
                    seen[al][op]++;
                    tot[al]++;
                }
            }
            int b = 0;
            double f[4];
            for (int al = 0; al < 4; al++) {
                f[al] = tot[al] ? (double)hit[al] / tot[al] : 0.0;
                if (f[al] > f[b]) b = al;
            }
            double second = 0.0;
            for (int al = 0; al < 4; al++)
                if (al != b && f[al] > second) second = f[al];
            total++;
            if (!(f[b] > AFMIN / 100.0 && f[b] - second > AGAP / 100.0)) continue;
            passed++;

            int distinct = 0, top = 0;
            for (int o = 0; o < 64; o++) {
                if (seen[b][o]) distinct++;
                if (seen[b][o] > top) top = seen[b][o];
            }
            if (distinct > 64) distinct = 64;
            dcnt[distinct]++;
            int sh = tot[b] ? (int)(10L * top / tot[b]) : 0;
            if (sh > 10) sh = 10;
            scnt[sh]++;
        }

        printf("%s\n  %ld of %ld windows pass AFMIN/AGAP\n",
               argv[a], passed, total);
        if (!passed) { free(d); printf("\n"); continue; }

        long acc = 0;
        printf("  distinct opcodes  ");
        for (int t = 4; t <= 40; t += 4) {
            acc = 0;
            for (int i = 0; i <= t; i++) acc += dcnt[i];
            printf("<=%d:%ld%%  ", t, 100 * acc / passed);
        }
        printf("\n  top-opcode share  ");
        for (int s = 0; s <= 10; s += 2) {
            acc = 0;
            for (int i = s; i <= 10; i++) acc += scnt[i];
            printf(">=%d0%%:%ld%%  ", s, 100 * acc / passed);
        }
        printf("\n\n");
        free(d);
    }
    return 0;
}
