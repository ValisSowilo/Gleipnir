/* Ground truth for the periodicity of each file: the full mean-absolute-
 * difference curve over stride 1..8192, its strongest minima, and whether
 * those minima are harmonics of a smaller one.
 *
 * The point is to design the detector against what is actually in the data.
 * The existing vote-by-recurrence detector picks 56 on sao where the record
 * is 28, because every multiple of a period also recurs; any replacement has
 * to prefer the fundamental, so first I need to see the harmonic structure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAXS 8192

static double mad_at(const uint8_t *d, size_t lo, size_t hi, int s) {
    double acc = 0; size_t cnt = 0;
    for (size_t i = lo + (size_t)s; i < hi; i++) {
        int v = (int)d[i] - (int)d[i - s];
        acc += v < 0 ? -v : v; cnt++;
    }
    return cnt ? acc / (double)cnt : 1e18;
}

/* 16-bit interpretation: difference of samples rather than of bytes, which is
 * what actually matters for x-ray and mr if they are 16-bit rasters. */
static double mad16(const uint8_t *d, size_t lo, size_t hi, int s, int be) {
    double acc = 0; size_t cnt = 0;
    for (size_t i = lo + (size_t)s; i + 1 < hi; i += 2) {
        int a = be ? (d[i] << 8 | d[i+1]) : (d[i+1] << 8 | d[i]);
        int b = be ? (d[i-s] << 8 | d[i-s+1]) : (d[i-s+1] << 8 | d[i-s]);
        int v = a - b;
        acc += v < 0 ? -v : v; cnt++;
    }
    return cnt ? acc / (double)cnt : 1e18;
}

int main(int argc, char **argv) {
    static double mad[MAXS + 1];
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { perror(argv[a]); continue; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *d = malloc((size_t)n);
        if (fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "short read\n"); return 1; }
        fclose(f);

        size_t lo = (size_t)n / 3, hi = lo + 262144;
        if (hi > (size_t)n) hi = (size_t)n;

        for (int s = 1; s <= MAXS && s < (int)(hi - lo); s++)
            mad[s] = mad_at(d, lo, hi, s);

        printf("%s  (%ld bytes)\n", argv[a], n);
        printf("   mad@1 = %.1f\n", mad[1]);

        /* the ten strongest minima */
        int idx[MAXS], cnt = 0;
        for (int s = 1; s <= MAXS && s < (int)(hi - lo); s++) idx[cnt++] = s;
        for (int i = 0; i < cnt; i++)
            for (int j = i + 1; j < cnt; j++)
                if (mad[idx[j]] < mad[idx[i]]) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }

        printf("   strongest strides:");
        int shown = 0;
        for (int i = 0; i < cnt && shown < 8; i++) {
            int s = idx[i];
            /* skip a stride that is a multiple of one already shown and no better */
            int red = 0;
            for (int k = 0; k < shown; k++) { (void)k; }
            printf(" %d(%.1f)", s, mad[s]);
            shown++;
            if (red) shown--;
        }
        printf("\n");

        /* fundamental: smallest s whose mad is within 5%% of the global best */
        int best = idx[0], fund = best;
        for (int s = 1; s < best; s++)
            if (mad[s] <= mad[best] * 1.05) { fund = s; break; }
        printf("   global best %d (mad %.1f), fundamental within 5%%: %d (mad %.1f)\n",
               best, mad[best], fund, mad[fund]);

        /* if the fundamental is small, look for the row stride on top of it */
        if (fund <= 4) {
            int rbest = 0; double rv = 1e18;
            for (int s = fund * 4; s <= MAXS && s < (int)(hi - lo); s++)
                if (mad[s] < rv) { rv = mad[s]; rbest = s; }
            printf("   row candidate above 4x sample: %d (mad %.1f)\n", rbest, rv);
        }

        printf("   as 16-bit samples: mad@2 LE %.1f  BE %.1f",
               mad16(d, lo, hi, 2, 0), mad16(d, lo, hi, 2, 1));
        int rs = idx[0] >= 4 ? idx[0] : 1024;
        printf("   mad@%d LE %.1f  BE %.1f\n\n", rs,
               mad16(d, lo, hi, rs, 0), mad16(d, lo, hi, rs, 1));
        free(d);
    }
    return 0;
}
