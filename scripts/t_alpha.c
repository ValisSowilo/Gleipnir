#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define SEGWIN 8192
#define AFMIN 32
#define AGAP 6
static const uint64_t ALPHA_OPS =
    (1ULL << 0x08) | (1ULL << 0x09) | (1ULL << 0x0a) | (1ULL << 0x0c) |
    (1ULL << 0x0d) | (1ULL << 0x0e) | (1ULL << 0x10) | (1ULL << 0x11) |
    (1ULL << 0x12) | (1ULL << 0x13) | (1ULL << 0x14) | (1ULL << 0x15) |
    (1ULL << 0x16) | (1ULL << 0x17) | (1ULL << 0x18) | (1ULL << 0x1a) |
    0xFFFFFFFF00000000ULL;
static int alpha_align_of(const uint8_t *d, size_t n, size_t base, int *ok) {
    *ok = 0;
    if (n < 512) return 0;
    int hit[4] = {0,0,0,0}, tot[4] = {0,0,0,0};
    for (int a = 0; a < 4; a++) {
        size_t st = (size_t)((a - (int)(base & 3)) & 3);
        for (size_t i = st; i + 4 <= n; i += 4) {
            if (ALPHA_OPS >> (d[i+3] >> 2) & 1) hit[a]++;
            tot[a]++;
        }
    }
    int b = 0; double f[4];
    for (int a = 0; a < 4; a++) { f[a] = tot[a] ? (double)hit[a]/tot[a] : 0.0; if (f[a] > f[b]) b = a; }
    double second = 0.0;
    for (int a = 0; a < 4; a++) if (a != b && f[a] > second) second = f[a];
    if (f[b] > AFMIN/100.0 && f[b] - second > AGAP/100.0) *ok = 1;
    return b;
}
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n); fread(d, 1, n, f); fclose(f);
    int det = 0, tot = 0, byal[4] = {0,0,0,0};
    for (long i = 0; i + SEGWIN < n; i += SEGWIN) {
        int ok = 0, a = alpha_align_of(d + i, SEGWIN, (size_t)i, &ok);
        tot++; if (ok) { det++; byal[a]++; }
    }
    printf("%-12s detected %d/%d (%.1f%%)  aligns 0:%d 1:%d 2:%d 3:%d\n",
           argv[1], det, tot, 100.0*det/tot, byal[0], byal[1], byal[2], byal[3]);
    return 0;
}
