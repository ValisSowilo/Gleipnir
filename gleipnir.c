/* Gleipnir -- a context-mixing archiver.
 *
 * Copyright (C) 2026 ValisSowilo
 * https://github.com/ValisSowilo
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * zlib is linked for DEFLATE stream recovery under its own permissive
 * licence; see LICENSE.txt in the distribution for that notice.
 *
 * ---------------------------------------------------------------------------
 *
 * Historical note: this file began as gen14.c and the internals still use the
 * `gen` prefix throughout, including the GENA magic number in the archive
 * header.  The container format is unchanged by the rename -- archives written
 * before it still read correctly, and the magic stays GENA so that remains
 * true.  Only the program's name changed.
 *
 * Originally: cache-blocked context-mixing compressor.
 *
 * gen8 was correct but memory-bound: predict() computed a fresh scattered
 * index per model per bit, so a single byte cost ~144 random DRAM accesses.
 * gen14 keeps the same modelling ideas and changes the data layout:
 *
 *   - nibble groups.  Each model's table is an array of 64-byte groups (one
 *     cache line).  A group holds the 15 counters for every partial-bit state
 *     inside one nibble.  We hash once per nibble, touch one line, and serve
 *     four bits from L1.  144 misses/byte -> 18.
 *   - all group addresses for a nibble are computed and prefetched up front,
 *     so the remaining misses overlap instead of serialising.
 *   - a check field per group detects hash collisions, so aliased contexts are
 *     reset rather than silently averaged together.  Costs nothing, helps ratio.
 *   - the mixer runs over a padded 16-float row so -march=native vectorises it.
 *   - counters are 16-bit with integer adaptive rates (no float conversion).
 *
 * Modelling gains over gen8:
 *   - the match model's confidence is *learned* (an adaptive table indexed by
 *     match length and predicted bit) instead of a hand-tuned constant.
 *   - two chained APM/SSE stages instead of one.
 *   - 512 mixer weight sets (partial byte x match state) instead of 256.
 *
 * build:  gcc -O3 -march=native -funroll-loops -ffast-math \
 *           -I tools/zlib-1.3.1 -o gen gen.c tools/zlib-1.3.1/libz.a -lm
 * usage:  gen c|d [-1|-5|-9] [-mN] [-tN] infile outfile
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#define MAXBLK   65536
#define SEGWIN   8192          /* segmentation / content-detection window */
#define MAXCHUNK 64
#define MINCHUNK (1u << 20)     /* below ~1 MB a chunk is all cold-start cost */
#define MAXCTX 32
/* Per-context arrays are written in a loop the vectoriser widens, and a
 * whole-vector store at the tail can reach past MAXCTX entries.  Sizing them
 * with slack keeps any such store inside its own object. */
#define CTXPAD (MAXCTX + 8)
/* Mixer row width.  NIN = NCTX + 5, so 27 contexts sat exactly on the old
 * 32-lane limit and adding any context would have silently clamped MIXW and
 * dropped inputs.  48 is three whole 32-byte vectors, which also means a
 * widened load at the tail of st[] cannot reach past the array. */
#define NPAD   48
#ifndef AFMIN
#define AFMIN 45
#endif
#ifndef AGAP
#define AGAP 12
#endif
#ifndef ADIV
#define ADIV 8         /* min distinct opcodes at the winning alignment */
#endif
#ifndef ADOM
#define ADOM 60        /* max %% of words sharing one opcode */
#endif
#ifndef APHASH
#define APHASH 4   /* fold instruction position into byte orders <= this */
#endif
#ifndef APBITS
#define APBITS 2   /* instruction-position bits in the mixer selector */
#endif
#ifndef WCLSB
#define WCLSB  4               /* bits of previous-byte class in the mixer selector */
#endif
#define WSETS  (1024 << WCLSB << APBITS)

/* SSE/APM stages evaluated, set per level.  Stages 5 and 6 (line position and
 * current word) are text-specific: on mr, four stages are both smaller and
 * faster than six. */
static int NAPM = 6;

/* Hash width of SSE stages a4 and a6.  At 10 bits the two tables are 33 MB
 * and every bit touches a scattered row in them, so they never cache.  The
 * cost of that scales with the file: shrinking to 4 bits is worth -12.3% time
 * on dickens (10 MB) and -21.7% on webster (41 MB), for +0.38% and +0.50%
 * size.  A poor trade at -9, whose job is the smallest output; a good one
 * everywhere else, which is why it is per preset. */
static int A46B = 10;
#define A46MASK ((1u << A46B) - 1)

/* ------------------------------------------------- work that is skipped
 * Three gates that stop paying for a refinement once it cannot change the
 * answer.  Each reads only state the decoder already holds at the moment it
 * makes the same decision -- the mixer's own output, the ISSE chain's running
 * value, the coded bit at update time -- so both sides skip identically and
 * the stream stays valid.  A gate that peeked at the bit being coded would
 * not, which is the one rule none of these may break.
 *
 * -1 means "take the per-level default"; 0 disables the gate entirely.  All
 * three are overridable with -D so they can be swept without touching source.
 */
#ifndef SSEG
#define SSEG -1
#endif
#ifndef SSEK
#define SSEK 2                 /* SSE stages always evaluated */
#endif
#ifndef IXIT
#define IXIT -1
#endif
#ifndef IXMIN
#define IXMIN 4                /* ISSE stages always evaluated */
#endif
#ifndef THIN
#define THIN -1
#endif
/* Speed-preset sizes, exposed so they can be swept rather than assumed.  The
 * whole premise of -f1/-f2 is that table size is the speed lever, which is
 * only worth asserting with a sweep behind it. */
#ifndef FMM1
#define FMM1 21                /* -f1 match index, in bits */
#endif
#ifndef FMM2
#define FMM2 22                /* -f2 match index */
#endif
#ifndef FWB
#define FWB 13                 /* log2 mixer weight sets at an -f preset */
#endif
/* The -f selector is c0 (8 bits) | in-a-match (1 bit) | previous-byte class,
 * widest field last so shrinking FWB drops class bits rather than corrupting
 * the index.  Below 9 there is nothing left to drop. */
_Static_assert(FWB >= 9 && FWB <= 13, "FWB outside the -f selector's range");
#define FWCLS ((1 << (FWB - 9)) - 1)
static int SSEGATE = 0;        /* skip SSE tail when |stretch(p)| >= this */
static int ISSEXIT = 0;        /* leave the ISSE chain once it clamps */
static int THINE   = 0;        /* skip mixer/ISSE training when |err| < this */
static int FASTP   = 0;        /* speed preset: narrow mixer selector */

/* ---------------------------------------------------------------- tables */

static int16_t stretch_t[4096];
static int     squash_t[4096];
static int32_t RCP[64];        /* 16.16 adaptive learning rates */

static int squash(int d) {
    if (d <= -2047) return 1;
    if (d >=  2047) return 4095;
    return squash_t[d + 2047];
}

/* Which blend of orders is right depends on where in the text we are: just
 * after a letter, in whitespace, inside markup, in binary.  Handing the mixer
 * that as part of its weight-set selector lets it keep a separate blend per
 * situation instead of averaging over all of them. */
static uint8_t CCLS[256];
static void build_ccls(void) {
    for (int i = 0; i < 256; i++)
        CCLS[i] = (uint8_t)(
            (i >= 'a' && i <= 'z') ? 0 : (i >= 'A' && i <= 'Z') ? 1 :
            (i >= '0' && i <= '9') ? 2 : (i == ' ' || i == '\t') ? 3 :
            (i == '\n' || i == '\r') ? 4 : (i < 32) ? 5 : (i < 128) ? 6 : 7);
}

static void build_tables(void) {
    for (int d = -2047; d <= 2047; d++)
        squash_t[d + 2047] = (int)(4096.0 / (1.0 + exp(-d / 256.0)));
    int pi = 0;
    for (int x = -2047; x <= 2047; x++) {
        int v = squash(x);
        while (pi <= v && pi < 4096) stretch_t[pi++] = (int16_t)x;
    }
    while (pi < 4096) stretch_t[pi++] = 2047;
    for (int i = 0; i < 64; i++) {
        double r = 1.0 / (i + 1.5);
        if (r < 1.0 / 48.0) r = 1.0 / 48.0;
        RCP[i] = (int32_t)(r * 65536.0);
    }
}

/* ------------------------------------------------------------ nibble group
 * 16 bytes: an 8-bit check plus one bit-history state per partial-nibble
 * state.  Slot s in 0..14 corresponds to nc-1, where nc starts at 1 and
 * shifts in bits.  Four groups share a cache line, so a table holding the
 * same number of contexts as gen14 costs a quarter of the memory.
 */
typedef struct {
    uint8_t chk;
    uint8_t s[15];
} Group;                        /* 16 bytes */

typedef struct { uint16_t *t; int idx; } APM;
#ifndef APMR
#define APMR 8                  /* SSE adaptation rate */
#endif
#ifndef APMW
#define APMW 2                  /* SSE blend weight, in quarters */
#endif

/* All mutable model state.  It lives in a struct rather than in globals so
 * that each worker thread can own an independent copy -- MinGW's __thread is
 * emulated (a call per access, far too slow for the inner loop) and its
 * __declspec(thread) is silently ignored, which would have shared state
 * across threads and corrupted output. */
typedef struct {
    Group   *T[CTXPAD];
    Group   *gp_[CTXPAD];
    uint8_t  gck_[CTXPAD];
    uint32_t gbase_[CTXPAD];
    uint64_t hashes[CTXPAD];
    uint32_t SM[CTXPAD][256];
    uint32_t *sm_e[CTXPAD];
    int16_t  st[NPAD] __attribute__((aligned(32)));
    /* Row stride is MIXW at run time, not NPAD.  A 32-lane row is exactly one
     * 64-byte cache line; padding every row out to the widest model any file
     * might need made text files fetch two lines per bit and cost 10% for
     * nothing.  Files that actually use the wider mixer pay for it, alone. */
    int16_t *W;
    int      wsel, blk_x86, blk_alpha, alpha_align;
    int32_t *mtab;
    uint8_t *buf;
    size_t   buflen;
    int32_t  mptr;
    int      mlen;
    uint32_t mpr[20 * 2 * 8];   /* (length bucket, expected bit, bit index) */
    int      m_idx;
    int32_t  IW[16][512];                /* ISSE: 2 weights per bit history */
    int      ip_in[16], ip_out[16], is_[16];
    uint64_t hist, word, word2, word3;
    int      wlen, wcap, pcap, col, intag, plen;
    uint32_t *ind1, *ind2;              /* what followed this context last time */
    int32_t  *dlast;                    /* last position of each byte value */
    int       dlog;                     /* log2 distance since the last one */
    uint32_t  nest;                     /* bracket / quote nesting state */
    size_t   lstart, plstart;
    int      c0, nc, nbits, pr;
    int      stride, rcol, swidth;
    /* Match-bypass state.  bsm is a tiny fallback StateMap indexed by the
     * partial byte; it exists only to finish a byte after a mid-byte match
     * break inside the bypass, and is trained only on bypassed bits, so the
     * modelled path pays nothing for it.  need_ctx defers rehash/nib_begin
     * across a bypass run: group pointers are refreshed lazily on the first
     * modelled byte after it. */
    uint32_t bsm[256];
    int      need_ctx;
    /* How many SSE stages and ISSE stages actually ran for the current bit.
     * update() must train exactly those and no others: a stage that did not
     * predict has a stale apm->idx and stale ip_in/ip_out, so training it
     * would move a weight that had nothing to do with this bit. */
    int      napm, nisse;
    APM      a1, a2, a3, a4, a5, a6;
    uint8_t *obuf;
    size_t   opos;
    uint32_t x1, x2, xx;
    const uint8_t *ibuf;
    size_t   ipos, ilen;
} Ctx;

/* Nonstationary bit-history counter, with the opposite count discounted on a
 * flip -- that discount is what lets a context whose behaviour just changed
 * adapt in a bit or two instead of waiting out its accumulated history.
 *
 * What matters about a context is mostly how *lopsided* it is, and real ones
 * are extremely lopsided: in English text "th" is followed by "e" thousands
 * of times running.  Packing the state as (n0<<4)|n1 spent all 256 states on
 * a square grid capped at 15 each, so every strongly skewed context collapsed
 * into the same corner and the StateMap could only learn the average
 * behaviour of everything that had saturated there.
 *
 * The counts are bounded asymmetrically instead: one side may run to MAXN
 * while the other is held to MINB.  Same 256 states, spent on the shape
 * contexts actually take, so a 40:1 context stays distinguishable from a 15:1
 * one.  The reachable pairs are enumerated and indexed rather than bit-packed,
 * which is why (0,0) is deliberately state 0 -- a zeroed table has to mean
 * "nothing seen here yet".
 */
#ifndef MAXN
#define MAXN 27
#endif
#ifndef MINB
#define MINB 4
#endif
/* If the pairs do not fit, the enumeration stops early and transitions point
 * at ids that were never assigned -- which shows up as a mysterious ~1% ratio
 * loss rather than as a crash. */
_Static_assert((MAXN + 1) * (MAXN + 1) - (MAXN - MINB) * (MAXN - MINB) <= 256,
               "MAXN/MINB enumerate more than 256 bit-history states");
static uint8_t NEX[256][2];
static uint8_t SN0[256], SN1[256];
static int     NSTATE;

static void build_states(void) {
    static int16_t id[MAXN + 1][MAXN + 1];
    for (int a = 0; a <= MAXN; a++)
        for (int b = 0; b <= MAXN; b++) id[a][b] = -1;
    NSTATE = 0;
    for (int s = 0; s <= 2 * MAXN && NSTATE < 256; s++)
        for (int a = 0; a <= MAXN && a <= s && NSTATE < 256; a++) {
            int b = s - a;
            if (b > MAXN || (a > MINB && b > MINB)) continue;
            id[a][b] = (int16_t)NSTATE;
            SN0[NSTATE] = (uint8_t)a; SN1[NSTATE] = (uint8_t)b; NSTATE++;
        }
    for (int i = 0; i < NSTATE; i++)
        for (int y = 0; y < 2; y++) {
            int a = SN0[i], b = SN1[i];
            if (y) { if (b < MAXN) b++; if (a > 2) a = (a + 1) >> 1; }
            else   { if (a < MAXN) a++; if (b > 2) b = (b + 1) >> 1; }
            while (a > MINB && b > MINB) { if (a > b) b--; else a--; }
            NEX[i][y] = (uint8_t)id[a][b];
        }
    for (int i = NSTATE; i < 256; i++) {
        NEX[i][0] = NEX[i][1] = 0; SN0[i] = SN1[i] = 0;
    }
}

/* StateMap: learns what each of the 256 histories actually means, per model.
 * 1 KB per model, so it never leaves L1.  Entry = p(22 bits) << 10 | count. */
static int32_t   DT[1024];

static void build_dt(void) {
    for (int n = 0; n < 1024; n++) DT[n] = (int32_t)(65536.0 / (n + 2.0));
}
static void sm_init(Ctx *TH) {
    for (int i = 0; i < CTXPAD; i++)
        for (int s = 0; s < 256; s++) {
            double p = (SN1[s] + 0.4) / (SN0[s] + SN1[s] + 0.8);
            TH->SM[i][s] = ((uint32_t)(p * 4194303.0) << 10);
        }
}
static void sm_update(uint32_t *e, int bit, int limit) {
    uint32_t v = *e;
    int n = (int)(v & 1023), p = (int)(v >> 10);
    if (n < limit) v++;
    int d = (int)((((int64_t)((bit ? 4194303 : 0) - p)) * DT[n]) >> 16);
    *e = (uint32_t)((int64_t)v + ((int64_t)d << 10));
}

/* ---------------------------------------------------------------- model */

static int      NCTX, NIN, LEVEL = 9, USE_SSE2 = 1;
static int      ORD[MAXCTX];    /* >0 byte order, -1 word, -2/-3 sparse */
/* Contexts actually worth computing this run.  With no detected stride the
 * record contexts (-4/-5) are constants: their mixer input is forced to zero,
 * so their weights never move and their tables are never usefully read.  The
 * stride is fixed per stream and known before set_level on both sides, so the
 * skip list is static and encoder and decoder always agree on it. */
static int      ACT[MAXCTX], NACT;
static uint8_t  ISACT[MAXCTX];
/* Side tables that only some contexts read.  The indirect tables are a 256 KB
 * random write per byte and nothing outside contexts -13/-14 ever looks at
 * them; below level 5 no context does.  Maintaining them there was the same
 * mistake the active-context list fixed inside the bit loop, one level up:
 * work whose result is never read.  Output is unchanged wherever the reading
 * contexts are absent, which is the only place this switches off. */
static int      NEED_IND, NEED_DLOG, NEED_NEST;
static int      GBITS[MAXCTX];
#ifndef WB1
#define WB1 22                  /* byte orders 4,5,6 */
#endif
#ifndef WB2
#define WB2 23                  /* word, word pair, word triple */
#endif
static uint32_t GMASK[MAXCTX];


/* Integer mixer.  Weights are 16.16 fixed point held in int16, so one SSE
 * pmaddwd folds eight products per instruction and, more importantly, the
 * per-bit dependency chain loses both float<->int conversions. */
static int      MIXW = 16;      /* active mixer lanes: 8 when NIN fits */
#ifndef LRSH
#define LRSH 13                 /* mixer learning-rate shift */
#endif

/* match model */
static uint32_t MMASK;
#ifndef IH1
#define IH1 1                   /* bytes of follow-history in the indirect contexts */
#endif
#ifndef IH2
#define IH2 1
#endif
#ifndef MMASKB
#define MMASKB 22               /* match index size at level 9, in bits */
#endif

/* ------------------------------------------------------------- ISSE chain
 * A flat mixer makes every order compete: each one hands over a single
 * stretched number and the mixer picks a global blend.  That discards the one
 * thing a high order actually knows about itself -- how many times it has seen
 * this context.  By the time the prediction reaches the mixer, "seen twice,
 * both 1" and "seen forty times, all 1" look identical.
 *
 * An ISSE chain instead lets each higher order *refine* what the lower orders
 * already decided.  Stage k takes the running prediction and adjusts it with a
 * two-weight linear map selected by that order's bit-history state, so the
 * size of the adjustment is learned per confidence level.  A rarely-seen
 * context learns to barely move the estimate; a well-established one learns to
 * override it.  Everything stays in the stretched domain and only the final
 * value is squashed, so the chain is six multiply-adds, not six mixers.
 *
 * This is the structural difference between the lpaq-style predictor this
 * started as and zpaq -m5, and it is where the text deficit lives.
 */
static int ISTART = 0;          /* context seeding the chain */
static int ICHAIN[16], NISSE;
#ifndef CHAINN
/* Stages used at level 9.  Measured 15 -> 11: dickens +0.022%, mr +0.009%,
 * samba +0.035% in size for 4.8 / 6.1 / 9.1% less time.  The last four stages
 * refine an estimate that eleven stages have already converged, so they cost
 * latency -- the chain is serial -- and return almost nothing. */
#define CHAINN 11
#endif
#ifndef ILR0
#define ILR0 12                 /* weight learning-rate shifts */
#endif
#ifndef ILR1
#define ILR1 3
#endif
#define ICLAMP (1 << 19)


/* ------------------------------------------------------------ record model
 * Star catalogues, database dumps and raster images are grids: the byte most
 * like the next one is often the byte one row/record earlier, not the byte
 * just before it.  None of the order-N contexts can see that, because the
 * relevant neighbour is hundreds of bytes back.
 *
 * The stride used to be found by voting: each 2-byte context remembered where
 * it last occurred and the gap was a vote.  Measured against the corpus that
 * detector was wrong on every file it mattered for.  It settled on 56 for sao
 * where the record is 28, because every *multiple* of a period recurs just as
 * reliably as the period; it missed the 1024-byte rows of mr entirely; and on
 * ooffice, which has no period at all, it changed its mind 18,944 times, each
 * change resetting the column phase and discarding the statistics.
 *
 * What the record contexts actually ask is "is the byte one stride back a good
 * predictor of this one".  Mean absolute difference answers exactly that, so
 * the stride is now measured directly, once, on the encoder, and carried in
 * the header.  That makes it exact rather than adaptive, removes a 256 KB
 * random table access from every byte, and -- because a file with no period
 * now reports none -- stops two of the 27 contexts modelling noise.
 */

#define MAXSTRIDE 8192

static int DET_STRIDE = 0;      /* row / record length, 0 = no period found */
static int DET_WIDTH  = 1;      /* element width in bytes: 1, 2 or 4 */

/* Mean |d[i] - d[i-s]| over [lo,hi). */
static double mad_win(const uint8_t *d, size_t lo, size_t hi, int s) {
    if (hi <= lo + (size_t)s) return 1e18;
    uint64_t acc = 0; size_t cnt = 0;
    for (size_t i = lo + (size_t)s; i < hi; i++) {
        int v = (int)d[i] - (int)d[i - s];
        acc += (uint64_t)(v < 0 ? -v : v); cnt++;
    }
    return (double)acc / (double)cnt;
}

/* Average MAD over several windows spread through the buffer, so a header, a
 * trailer or one odd region cannot decide the answer for the whole file. */
static double mad_spread(const uint8_t *d, size_t n, int s, size_t win, int nw) {
    double acc = 0; int used = 0;
    for (int k = 0; k < nw; k++) {
        size_t lo = (size_t)((double)n * (k + 1) / (nw + 1));
        if (lo + win > n) { if (n > win) lo = n - win; else lo = 0; }
        size_t hi = lo + win < n ? lo + win : n;
        double m = mad_win(d, lo, hi, s);
        if (m < 1e17) { acc += m; used++; }
    }
    return used ? acc / used : 1e18;
}

static void detect_period(const uint8_t *d, size_t n) {
    DET_STRIDE = 0; DET_WIDTH = 1;
    if (n < 4096) return;

    /* Coarse pass over every stride, on small windows. */
    size_t cw = n / 8 < 32768 ? n / 8 : 32768;
    if (cw < 1024) cw = n < 1024 ? n : 1024;
    int maxs = (int)(cw / 2);
    if (maxs > MAXSTRIDE) maxs = MAXSTRIDE;
    if (maxs < 8) return;

    static double m[MAXSTRIDE + 1];
    for (int s = 1; s <= maxs; s++) m[s] = mad_spread(d, n, s, cw, 2);
    double m1 = m[1];
    if (m1 <= 0.0) return;                    /* constant data: nothing to model */

    /* Element width: the smallest small stride that predicts far better than
     * the neighbouring byte.  x-ray is 16-bit (mad@2 21.0 vs mad@1 132.9);
     * sao is a byte-oriented record and stays at 1. */
    int w = 1;
    for (int c = 2; c <= 4; c++)
        if (m[c] <= m1 * 0.6) { w = c; break; }

    /* Row / record length: best stride at 8 or more, then walk down to the
     * fundamental so sao reports 28 rather than its 56, 84, 112 harmonics. */
    int best = 0; double bv = 1e18;
    for (int s = 8; s <= maxs; s++)
        if (m[s] < bv) { bv = m[s]; best = s; }
    if (!best) return;

    for (int s = 8; s < best; s++)
        if (m[s] <= bv * 1.03) { best = s; bv = m[s]; break; }

    /* Confirm the survivor on much larger windows before committing: a coarse
     * pass on 32 KB can be fooled, and a wrong stride is worse than none. */
    size_t fw = n / 4 < 262144 ? n / 4 : 262144;
    double fb = mad_spread(d, n, best, fw, 4);
    double f1 = mad_spread(d, n, 1, fw, 4);

    /* The gate.  osdb and ooffice have no period -- their best stride is worse
     * than simply looking one byte back -- and must report none, or the record
     * contexts spend the whole file on noise. */
    if (fb > f1 * 0.85) { DET_WIDTH = w; return; }

    DET_STRIDE = best; DET_WIDTH = w;
}

/* ------------------------------------------------------------------- APM */


/* n == 0 means this stage is outside the preset's NAPM and is never evaluated,
 * so it gets no table at all.  Every stage used to be allocated regardless:
 * at -1, where NAPM is 0, that was 43 MB of tables that were never read -- and
 * it multiplied by thread count, since each worker allocates its own. */
static void apm_init(APM *a, int n) {
    a->idx = 0;
    if (n <= 0) { a->t = NULL; return; }
    a->t = malloc((size_t)n * 33 * sizeof(uint16_t));
    if (!a->t) { fprintf(stderr, "oom\n"); exit(1); }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < 33; j++)
            a->t[i * 33 + j] = (uint16_t)(squash((j - 16) * 128) * 16);
}
static int apm_pp(APM *a, int p, int cx) {
    int s = stretch_t[p] + 2048;            /* 1..4095 */
    int w = s & 127;
    int i = (s >> 7) + cx * 33;
    a->idx = i + (w >> 6);
    return (a->t[i] * (128 - w) + a->t[i + 1] * w) >> 11;
}
static void apm_up(APM *a, int bit, int rate) {
    int g = (bit << 16) + (bit << rate) - bit - bit;
    a->t[a->idx] += (g - a->t[a->idx]) >> rate;
}

/* -------------------------------------------------------------- hashing */

/* Word bookkeeping, shared by the modelled and the absorbed byte paths.
 * word2/word3 hold the last completed words, so a context can condition on
 * the pair or triple -- which is what carries the grammar in ordinary prose.
 *
 * The hash is case folded.  Left case-sensitive, "The" and "the" are separate
 * contexts that each learn English from scratch, which halves the evidence
 * behind every word context for no gain: the case itself is far better
 * predicted from a handful of shape bits (wcap/wlen) than by duplicating the
 * whole vocabulary.  Raw case is still fully visible to the byte orders.
 */
static void push_word(Ctx *TH, int byte) {
    int up = (byte >= 'A' && byte <= 'Z');
    int c  = up ? byte + 32 : byte;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
        TH->word = TH->word * 0x9E3779B1ULL + (uint64_t)c + 1;
        if (TH->wlen == 0) TH->wcap = up;       /* 1 = first letter capital */
        else if (up)       TH->wcap |= 2;       /* 2 = a later letter too */
        if (TH->wlen < 63) TH->wlen++;
    } else {
        if (TH->word) {
            TH->word3 = TH->word2;
            TH->word2 = TH->word;
            TH->pcap  = TH->wcap;
        }
        TH->word = 0; TH->wlen = 0; TH->wcap = 0;
    }
    /* Line and tag position.  Hard-wrapped prose breaks at a column the byte
     * orders cannot see -- by the time a line is 70 characters long, nothing
     * in the last 16 bytes says so -- and markup makes "inside a tag" a
     * different language from the text around it. */
    /* Indirect contexts: for each recent context, remember the last four
     * bytes that followed it.  A plain order-N context asks "what usually
     * comes next"; this asks "what came next the last few times", which
     * separates a context that alternates from one that repeats even when
     * their aggregate statistics are identical. */
    if (NEED_IND) {
        uint32_t p1 = (uint32_t)(TH->hist & 0xFF);
        uint32_t p2 = (uint32_t)(TH->hist & 0xFFFF);
        TH->ind1[p1] = (TH->ind1[p1] << 8) | (uint32_t)byte;
        TH->ind2[p2] = (TH->ind2[p2] << 8) | (uint32_t)byte;
    }
    /* Distance since this byte value last occurred, bucketed by magnitude.
     * Fixed-width records and periodic data put a strong signal here that no
     * contiguous context can see. */
    if (NEED_DLOG) {
        int32_t prev = TH->dlast[byte];
        TH->dlast[byte] = (int32_t)TH->buflen;
        int d = (int)TH->buflen - prev, k = 0;
        if (prev <= 0) k = 15;
        else while (d > 1 && k < 14) { d >>= 1; k++; }
        TH->dlog = k;
    }
    /* Nesting.  Brackets and quotes make source, markup and structured data
     * behave differently inside than outside, and depth is invisible to a
     * fixed-length context. */
    if (NEED_NEST) switch (byte) {
        case '(': case '[': case '{':
            TH->nest = (TH->nest << 3 | 1) & 0xFFFFFF; break;
        case ')': case ']': case '}':
            TH->nest >>= 3; break;
        case '"': case '\'':
            TH->nest ^= 0x1000000; break;
        default: break;
    }
    if (byte == '\n') {
        TH->plstart = TH->lstart;
        TH->plen    = (int)(TH->buflen - 1 - TH->lstart);   /* excludes the \n */
        TH->lstart  = TH->buflen;
        TH->col     = 0;
    } else if (TH->col < 1023) TH->col++;
    if (byte == '<') TH->intag = 1;
    else if (byte == '>') TH->intag = 0;
}

/* The byte directly above, when there is one.  This is the record model's idea
 * applied to newline-delimited rows instead of a fixed stride, which is what
 * wrapped prose and line-oriented database dumps actually are. */
/* Signed log-ish quantiser for pixel differences: small differences are what
 * carry the information in smooth data, so they keep their own bins while
 * everything large collapses together.  Range -7..7. */
static int qgrad(int v) {
    int a = v < 0 ? -v : v;
    int c = a < 2 ? a : (a < 4 ? 2 : (a < 8 ? 3 : (a < 16 ? 4 :
            (a < 32 ? 5 : (a < 64 ? 6 : 7)))));
    return v < 0 ? -c : c;
}

static int byte_above(const Ctx *TH) {
    return (TH->col < TH->plen) ? TH->buf[TH->plstart + TH->col] : 0x100;
}

static void rehash(Ctx *TH) {
    uint64_t h = TH->hist;
    /* Position within the instruction, for fixed-length code.  Folding it into
     * the byte-order hashes stops an order-3 context from conflating "the last
     * three bytes of one instruction" with "the tail of one and the head of the
     * next" -- two completely different situations that look identical as a
     * byte string.  Zero outside such blocks, so every other file hashes
     * exactly as before. */
    const uint64_t ap = TH->blk_alpha
        ? (uint64_t)((TH->buflen - (size_t)TH->alpha_align) & 3) : 0;
    for (int kk = 0; kk < NACT; kk++) {
        const int k = ACT[kk];
        int o = ORD[k];
        uint64_t v;
        if (o > 8) {
            /* hist only carries eight bytes, so longer orders are hashed off
             * the buffer.  Those bytes sit immediately behind the write
             * pointer and are always hot, which is why this stays cheap. */
            size_t L = TH->buflen;
            int n = (o <= (int)L) ? o : (int)L;
            uint64_t v2 = 0;
            for (int j = 0; j < n; j++)
                v2 = (v2 + TH->buf[L - 1 - j] + 1) * 0x9E3779B97F4A7C15ULL;
            v = (v2 + 0x100ULL * (uint64_t)o) * 0xC2B2AE3D27D4EB4FULL;
        } else if (o > 0) {
            uint64_t ctx = (o >= 8) ? h : (h & ((1ULL << (8 * o)) - 1));
            uint64_t mix = (o <= APHASH) ? ap * 0x2545F4914F6CDD1DULL : 0;
            v = (ctx + 0x100ULL * (uint64_t)o + mix) * 0x9E3779B97F4A7C15ULL;
        } else if (o == -1) {
            v = (TH->word + 1) * 0xC2B2AE3D27D4EB4FULL;
        } else if (o == -2) {
            uint64_t s = ((h >> 8) & 255) | (((h >> 24) & 255) << 8);
            v = (s + 0x51ULL) * 0x27D4EB2F165667C5ULL;
        } else if (o == -3) {
            uint64_t s = (h & 255) | (((h >> 16) & 255) << 8) | (((h >> 32) & 255) << 16);
            v = (s + 0x93ULL) * 0x165667B19E3779F9ULL;
        } else if (o == -4) {                  /* record: byte above + column */
            uint64_t up = (TH->stride && TH->buflen >= (size_t)TH->stride) ? TH->buf[TH->buflen - TH->stride] : 0x100;
            v = ((up << 16) | (uint64_t)TH->rcol) * 0x9E3779B97F4A7C15ULL + 0xF00DULL;
        } else if (o == -5) {                  /* record: byte above + byte left.
                                                * "Left" means one *element* back,
                                                * not one byte: in 16-bit raster
                                                * data the byte at -1 is the other
                                                * half of the current sample and
                                                * predicts almost nothing, while
                                                * the byte at -2 is the same plane
                                                * of the previous sample. */
            uint64_t up = (TH->stride && TH->buflen >= (size_t)TH->stride) ? TH->buf[TH->buflen - TH->stride] : 0x100;
            uint64_t lf = (TH->buflen >= (size_t)TH->swidth) ? TH->buf[TH->buflen - TH->swidth] : 0x100;
            v = ((up << 9) | lf) * 0xC2B2AE3D27D4EB4FULL + 0xBEEFULL;
        } else if (o <= -19 && o >= -21) {
            /* Raster contexts.  Once the stride and element width are known,
             * the useful neighbourhood is two-dimensional: the sample to the
             * left, the one above, and the one above-left.  A byte-order
             * context cannot reach any of them -- above is a row away.
             *
             * These stay silent unless a period was actually detected, so text
             * and code are unaffected. */
            const int R = TH->stride, ew = TH->swidth;
            const size_t L = TH->buflen;
            int Wp = (L >= (size_t)ew)            ? TH->buf[L - ew] : 0;
            int Np = (L >= (size_t)R)             ? TH->buf[L - R]  : 0;
            int NWp = (L >= (size_t)(R + ew))     ? TH->buf[L - R - ew] : 0;
            /* Which half of a 16-bit sample we are in.  The high byte of a
             * medical image is smooth and the low byte is nearly noise; without
             * this they share one set of statistics and blunt each other. */
            const int plane = ew > 1 ? (TH->rcol & (ew - 1)) : 0;
            if (o == -19) {
                /* MED / LOCO-I predictor: the gradient-adjusted estimate that
                 * lossless image coders use, clamped into the range its own
                 * neighbours span. */
                int p = Wp + Np - NWp;
                int lo = Wp < Np ? Wp : Np, hi = Wp < Np ? Np : Wp;
                if (p < lo) p = lo; else if (p > hi) p = hi;
                v = (((uint64_t)p << 4) | (uint64_t)plane)
                  * 0x9E3779B97F4A7C15ULL + 0x1D0BULL;
            } else if (o == -20) {             /* joint N, W, NW */
                uint64_t s = (uint64_t)Np | ((uint64_t)Wp << 8)
                           | ((uint64_t)NWp << 16) | ((uint64_t)plane << 24);
                v = (s + 0x3C7ULL) * 0xC2B2AE3D27D4EB4FULL;
            } else {                           /* -21: local gradient class */
                uint64_t s = (uint64_t)(qgrad(Wp - NWp) + 7)
                           | ((uint64_t)(qgrad(Np - NWp) + 7) << 4)
                           | ((uint64_t)(qgrad(Wp - Np) + 7) << 8)
                           | ((uint64_t)plane << 12)
                           | ((uint64_t)(h & 0xFF) << 13);
                v = (s + 0x6A1ULL) * 0x165667B19E3779F9ULL;
            }
        } else if (o == -6) {                  /* word bigram */
            v = (((TH->word + 1) * 0xC2B2AE3D27D4EB4FULL) ^
                 ((TH->word2 + 1) * 0x9E3779B97F4A7C15ULL)) * 0x27D4EB2F165667C5ULL;
        } else if (o == -7) {                  /* word trigram */
            v = (((TH->word + 1) * 0xC2B2AE3D27D4EB4FULL) ^
                 ((TH->word2 + 1) * 0x9E3779B97F4A7C15ULL) ^
                 ((TH->word3 + 1) * 0x165667B19E3779F9ULL)) * 0x27D4EB2F165667C5ULL;
        } else if (o == -8) {                  /* word shape: what case and how
                                                * far into a word we are, which
                                                * is what the folded word hash
                                                * deliberately threw away */
            uint64_t s = (uint64_t)TH->wcap | ((uint64_t)TH->pcap << 2)
                       | ((uint64_t)(TH->wlen < 15 ? TH->wlen : 15) << 4)
                       | ((h & 0xFFFFULL) << 8);
            v = (s + 0x2711ULL) * 0x9E3779B97F4A7C15ULL;
        } else if (o == -9) {                  /* line/markup position */
            uint64_t s = (uint64_t)(TH->col < 127 ? TH->col : 127)
                       | ((uint64_t)TH->intag << 7)
                       | ((h & 0xFFULL) << 8);
            v = (s + 0x5BULL) * 0xC2B2AE3D27D4EB4FULL;
        } else if (o == -10) {                 /* byte above + column */
            uint64_t s = (uint64_t)byte_above(TH)
                       | ((uint64_t)(TH->col < 127 ? TH->col : 127) << 9)
                       | ((uint64_t)TH->intag << 16);
            v = (s + 0xA5ULL) * 0x9E3779B97F4A7C15ULL;
        } else if (o == -11) {                 /* byte above + byte left */
            uint64_t s = (uint64_t)byte_above(TH) | ((h & 0xFFULL) << 9);
            v = (s + 0xC7ULL) * 0x165667B19E3779F9ULL;
        } else if (o == -15) {                 /* how long since this byte last
                                                * appeared -- periodic data and
                                                * field boundaries show up here
                                                * and nowhere else */
            v = (((uint64_t)TH->dlog << 8) | (h & 0xFF)) * 0x9E3779B97F4A7C15ULL
              + 0x51EDULL;
        } else if (o == -16) {                 /* bracket / quote nesting */
            v = ((uint64_t)TH->nest + ((h & 0xFFULL) << 32) + 0x2B1ULL)
              * 0xC2B2AE3D27D4EB4FULL;
        } else if (o == -17) {                 /* sparse: bytes at -1 and -3 */
            uint64_t sp = (h & 255) | (((h >> 16) & 255) << 8);
            v = (sp + 0x9D1ULL) * 0x165667B19E3779F9ULL;
        } else if (o == -18) {                 /* sparse: bytes at -1 and -4 */
            uint64_t sp = (h & 255) | (((h >> 24) & 255) << 8);
            v = (sp + 0x4C7ULL) * 0x27D4EB2F165667C5ULL;
        } else if (o == -13) {                 /* indirect, order 1 */
            uint32_t c = (uint32_t)(h & 0xFF);
            uint64_t r = TH->ind1[c] & ((1ULL << (8 * IH1)) - 1);
            v = (r + ((uint64_t)c << 33) + 0x3B9ULL) * 0x9E3779B97F4A7C15ULL;
        } else {                               /* indirect, order 2 */
            uint32_t c = (uint32_t)(h & 0xFFFF);
            uint64_t r = TH->ind2[c] & ((1ULL << (8 * IH2)) - 1);
            v = (r + ((uint64_t)c << 33) + 0x7E1ULL) * 0xC2B2AE3D27D4EB4FULL;
        }
        TH->hashes[k] = v ^ (v >> 31);
    }
}

/* Compute + prefetch every group for the nibble about to start, then validate
 * the check fields in a second pass so the misses overlap. */
#ifndef WAYS
#define WAYS 4                  /* groups probed per bucket */
#endif
/* Four groups share one 64-byte line, so probing all four costs the same
 * memory traffic as probing one.  That buys a replacement policy: when the
 * context is not present, evict the *least established* group in the line
 * rather than whichever one the index happened to land on.  With a 41 MB file
 * against 2^20 slots almost every lookup is a collision, and blindly wiping
 * a context with real history to make room for a one-off is where an
 * overloaded table bleeds most of its ratio. */
#ifdef STATS
static uint64_t ST_hit[MAXCTX], ST_evict[MAXCTX], ST_virgin[MAXCTX];
static int ST_i;
#endif
static Group *group_find(Group *T, uint32_t base, uint8_t chk) {
    Group *g = &T[base];
    for (int i = 0; i < WAYS; i++) if (g[i].chk == chk) {
#ifdef STATS
        ST_hit[ST_i]++;
        if (g[i].s[0] == 0) ST_virgin[ST_i]++;
#endif
        return &g[i];
    }
#ifdef STATS
    ST_evict[ST_i]++;
#endif
    int b = 0, bp = 1 << 30;
    for (int i = 0; i < WAYS; i++) {
        int s = g[i].s[0], p = SN0[s] + SN1[s];
        if (p < bp) { bp = p; b = i; }
    }
    memset(g[b].s, 0, 15);
    g[b].chk = chk;
    return &g[b];
}

static void nib_begin(Ctx *TH, int lo_nibble, int hi) {
    /* NACT is a global, so the unroller cannot see that it never exceeds
     * MAXCTX; say so explicitly rather than let it speculate past the arrays. */
    const int n = NACT < MAXCTX ? NACT : MAXCTX;
    for (int k = 0; k < n; k++) {
        const int i = ACT[k];
        uint64_t h = TH->hashes[i];
        if (lo_nibble) h = (h + (uint64_t)hi + 16) * 0x9E3779B97F4A7C15ULL;
        TH->gbase_[i] = (uint32_t)(h >> 20) & (GMASK[i] & ~(uint32_t)(WAYS - 1));
        TH->gck_[i]   = (uint8_t)(h >> 48);
        __builtin_prefetch(&TH->T[i][TH->gbase_[i]], 1, 3);
        if (WAYS > 4) __builtin_prefetch(&TH->T[i][TH->gbase_[i] + 4], 1, 3);
    }
    for (int k = 0; k < n; k++) {
        const int i = ACT[k];
#ifdef STATS
        ST_i = i;
#endif
        TH->gp_[i] = group_find(TH->T[i], TH->gbase_[i], TH->gck_[i]);
    }
}

/* ------------------------------------------------------------- predictor */

static int predict(Ctx *TH) {
    const int slot = TH->nc - 1;
    /* Inactive contexts keep st[i] == 0 forever (zeroed at model_alloc), so
     * their mixer lanes stay inert and their weights never move -- the output
     * is byte-identical to running them and silencing the result, minus every
     * hash, prefetch, probe, load and update they used to cost. */
    for (int k = 0; k < NACT; k++) {
        const int i = ACT[k];
        uint32_t *e = &TH->SM[i][TH->gp_[i]->s[slot]];
        TH->sm_e[i] = e;
        TH->st[i] = stretch_t[*e >> 20];
    }

    TH->m_idx = -1;
    int m = 0;
    if (TH->mlen > 0 && (size_t)TH->mptr < TH->buflen) {
        int pb = TH->buf[TH->mptr];
        if (TH->nbits == 0 || (pb >> (8 - TH->nbits)) == (TH->c0 - (1 << TH->nbits))) {
            int eb = (pb >> (7 - TH->nbits)) & 1;
            int b  = TH->mlen < 16 ? TH->mlen : (TH->mlen < 32 ? 16 : (TH->mlen < 64 ? 17 : (TH->mlen < 400 ? 18 : 19)));
            /* How much a match is worth depends on where in the byte we are,
             * not only on how long the match has run: the high bits of a
             * predicted byte are nearly free, the low ones are where a match
             * actually breaks. */
            TH->m_idx = ((b << 1) | eb) * 8 + TH->nbits;
            m = stretch_t[TH->mpr[TH->m_idx] >> 20];
        } else {
            TH->mlen = 0;
        }
    }
    TH->st[NCTX] = (int16_t)m;           /* TH->st[NCTX+1] = bias */

    /* Refine the low-order estimate through progressively higher orders.  The
     * mixer still sees every model individually, so it can fall back on flat
     * mixing wherever the chain is not helping. */
    int cp = TH->st[ISTART];
    int nk = NISSE;
    for (int k = 0; k < NISSE; k++) {
        int s = TH->gp_[ICHAIN[k]]->s[slot];
        const int32_t *w = &TH->IW[k][s * 2];
        TH->ip_in[k] = cp;
        TH->is_[k]   = s;
        int t = (int)(((int64_t)w[0] * cp + (int64_t)w[1] * 512) >> 16);
        cp = t < -2047 ? -2047 : (t > 2047 ? 2047 : t);
        TH->ip_out[k] = cp;
        /* Once the running estimate has been driven to the rail, the stages
         * after it are refining a number that is already as certain as the
         * representation allows.  Leaving here reads only the chain's own
         * state, which the decoder has reached identically. */
        if (ISSEXIT && k + 1 >= IXMIN && (cp <= -2047 || cp >= 2047)) {
            nk = k + 1; break;
        }
    }
    TH->nisse = nk;
    /* Taps along the chain, not just its end: the mixer rounds up to whole
     * SIMD vectors anyway, so these ride in lanes that were already being
     * multiplied by zero.  When the chain exits early the taps collapse onto
     * the last stage that ran, rather than reading a stale ip_out. */
    int t1 = NISSE / 3, t2 = (NISSE * 2) / 3;
    if (t1 >= nk) t1 = nk - 1;
    if (t2 >= nk) t2 = nk - 1;
    TH->st[NCTX + 2] = (int16_t)(t1 >= 0 ? TH->ip_out[t1] : cp);
    TH->st[NCTX + 3] = (int16_t)(t2 >= 0 ? TH->ip_out[t2] : cp);
    TH->st[NCTX + 4] = (int16_t)cp;

    /* Class of the previous byte, plus whether the one before it was
     * alphanumeric -- together these separate "start of a word" from "inside
     * a word", which want quite different blends. */
    int wc = CCLS[TH->hist & 255] | (CCLS[(TH->hist >> 8) & 255] <= 2 ? 8 : 0);
    /* Inside fixed-length instruction code, which models to trust depends on
     * where in the instruction we are -- an opcode byte and a displacement byte
     * want completely different blends.  Outside such blocks this is always 0,
     * so nothing else sees a change. */
    int ap = TH->blk_alpha
           ? (int)((TH->buflen - (size_t)TH->alpha_align) & 3) : 0;
    /* The full selector spans 65536 weight sets: at 8 lanes that is a 1 MB
     * table, per thread, indexed at random once per bit and again per update.
     * The x86 and instruction-position dimensions of it are there for models
     * that have contexts able to exploit them, which three byte orders are
     * not, so an -f preset drops both and keeps 8192 sets in 128 KB.  On
     * dickens narrowing it further, to 8 KB, measured -11.2% time for +2.06%
     * size -- so this is worth real time as well as the memory. */
    TH->wsel = FASTP
        ? (TH->c0 | (TH->mlen > 0 ? 256 : 0) | ((wc & FWCLS) << 9))
        : (TH->c0 | (TH->mlen > 0 ? 256 : 0) | (TH->blk_x86 ? 512 : 0)
             | ((wc & ((1 << WCLSB) - 1)) << 10)
             | ((ap & ((1 << APBITS) - 1)) << (10 + WCLSB)));
    const int16_t *w = TH->W + (size_t)TH->wsel * (size_t)MIXW;
    int sum = 0;
    /* Constant trip counts so each width unrolls and vectorises cleanly --
     * a variable bound here costs more than the unused lanes do. */
#define MIXDOT(N) for (int i = 0; i < (N); i++) sum += (int)w[i] * TH->st[i]
    switch (MIXW) {
        case  8: MIXDOT(8);  break;
        case 16: MIXDOT(16); break;
        case 24: MIXDOT(24); break;
        case 32: MIXDOT(32); break;
        default: MIXDOT(40); break;
    }
#undef MIXDOT
    int d = sum >> 16;
    if (d < -2047) d = -2047; else if (d > 2047) d = 2047;
    int p = squash(d);

    TH->napm = 0;
    if (USE_SSE2) {
        /* Stage contexts.  Computing them all up front costs nothing and lets
         * the loads below issue together in the parallel arrangement. */
        int mb = TH->mlen == 0 ? 0 : (TH->mlen < 4 ? 1 : (TH->mlen < 8 ? 2 :
                 (TH->mlen < 16 ? 3 : (TH->mlen < 32 ? 4 : (TH->mlen < 64 ? 5 :
                 (TH->mlen < 400 ? 6 : 7))))));
        int cs = ((TH->col < 120 ? TH->col >> 3 : 15) & 15) | (TH->intag << 4)
               | ((TH->wcap & 3) << 5);
        APM *ap[6] = { &TH->a1, &TH->a2, &TH->a3, &TH->a4, &TH->a5, &TH->a6 };
        int cx[6];
        cx[0] = TH->c0;
        /* a2's row stays within one byte's worth of table so it stays cached */
        cx[1] = (int)(((TH->hist & 0xFF) << 8) | (uint64_t)TH->c0);
        /* How long the match model has been right is the single strongest
         * signal for whether the estimate needs pulling toward certainty. */
        cx[2] = (mb << 8) | TH->c0;
        /* A wider context than a2's single byte: two bytes back, hashed down. */
        cx[3] = (int)(((((TH->hist & 0xFFFF) * 0x9E3779B1ULL) >> 22) & A46MASK) << 8)
              | TH->c0;
        cx[4] = (cs << 8) | TH->c0;                       /* line position */
        cx[5] = (int)((((TH->word * 0x9E3779B1ULL) >> 22) & A46MASK) << 8)
              | TH->c0;                                   /* current word */

        /* Strictly serial, and deliberately so.  Evaluating all six against the
         * mixer output instead -- so their six random loads issue together --
         * was tried and is much worse: +4.07% on dickens for only 10.6% less
         * time, where simply dropping to two serial stages gives +0.55% for
         * 24% less time.  The refinement *is* the mechanism; each stage corrects
         * its predecessor, and a learned blend of independent stages does not
         * recover that.  The dependency chain is the price of the method. */
        /* ...but the later stages only earn their latency where the estimate
         * is still in doubt.  Once the first SSEK stages have left p out at
         * the confident end, a3-a6 are polishing hundredths of a bit onto a
         * prediction that already costs almost nothing to code, and each one
         * is a scattered load the next stage has to wait for.  The gate reads
         * p, which is a pure function of state both sides share, so the
         * decoder stops at the same stage. */
        int nrun = NAPM;
        for (int k = 0; k < NAPM; k++) {
            if (SSEGATE && k >= SSEK) {
                int s = stretch_t[p];
                if ((s < 0 ? -s : s) >= SSEGATE) { nrun = k; break; }
            }
            int q = apm_pp(ap[k], p, cx[k]);
            p = (q * APMW + p * (4 - APMW)) >> 2;
        }
        TH->napm = nrun;
    }
    TH->pr = p < 1 ? 1 : (p > 4094 ? 4094 : p);
    return TH->pr;
}

/* ---------------------------------------------------------------- update */

static void update_match(Ctx *TH, size_t pos) {
    if (TH->mlen > 0 && (size_t)TH->mptr < TH->buflen - 1 && TH->buf[TH->mptr] == TH->buf[pos]) {
        TH->mptr++;
        if (TH->mlen < 65535) TH->mlen++;
    } else {
        TH->mlen = 0;
    }
    uint32_t h = (uint32_t)(((TH->hist & 0xFFFFFFFFFFFFULL) * 0x9E3779B97F4A7C15ULL) >> 24) & MMASK;
    if (TH->mlen == 0) {
        int32_t cand = TH->mtab[h];
        if (cand > 0 && (size_t)cand < TH->buflen) {
            TH->mptr = cand;
            int L = 0;
            long a = cand - 1, b = (long)pos;
            while (L < 60 && a >= 0 && b >= 0 && TH->buf[a] == TH->buf[b]) { L++; a--; b--; }
            TH->mlen = (L >= 2) ? L : 0;
        }
    }
    TH->mtab[h] = (int32_t)(pos + 1);
}

static void update(Ctx *TH, int bit) {
    /* mixer */
    const int err = (bit << 12) - TH->pr;
    /* Training is proportional to the error, so a bit the model already had
     * right moves the weights by a rounding error -- but the loop still runs
     * and the ISSE half of it still squashes and clamps per stage.  By update
     * time the decoder has the bit too, so gating on |err| is symmetric.  Off
     * by default: what it buys is bounded by how much of the runtime training
     * actually is, and what it costs is that the model stops learning from
     * the bits it is already good at, which is most of them. */
    const int ae = err < 0 ? -err : err;
    const int train = !THINE || ae >= THINE;
    if (train) {
    int16_t *w = TH->W + (size_t)TH->wsel * (size_t)MIXW;
#define MIXUP(N) for (int i = 0; i < (N); i++) {                              \
        int v = w[i] + ((err * TH->st[i]) >> LRSH);                           \
        w[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));      \
    }
    switch (MIXW) {
        case  8: MIXUP(8);  break;
        case 16: MIXUP(16); break;
        case 24: MIXUP(24); break;
        case 32: MIXUP(32); break;
        default: MIXUP(40); break;
    }
#undef MIXUP
    }

    if (USE_SSE2) {
        APM *ap[6] = { &TH->a1, &TH->a2, &TH->a3, &TH->a4, &TH->a5, &TH->a6 };
        /* Exactly the stages that predicted: a stage the gate skipped still
         * holds the index it used on some earlier bit. */
        for (int k = 0; k < TH->napm; k++) apm_up(ap[k], bit, APMR);
    }

    /* Each ISSE stage trains on its own error rather than on a backpropagated
     * one: the stage is asked only to be a better estimate than the stage
     * below it, which is exactly the job it was given. */
    if (train) for (int k = 0; k < TH->nisse; k++) {
        int32_t *w = &TH->IW[k][TH->is_[k] * 2];
        int e = (bit << 12) - squash(TH->ip_out[k]);
        int v0 = w[0] + ((e * TH->ip_in[k]) >> ILR0);
        int v1 = w[1] + (e >> ILR1);
        w[0] = v0 > ICLAMP ? ICLAMP : (v0 < -ICLAMP ? -ICLAMP : v0);
        w[1] = v1 > ICLAMP ? ICLAMP : (v1 < -ICLAMP ? -ICLAMP : v1);
    }

    /* bit histories + their state maps */
    const int slot = TH->nc - 1;
    for (int k = 0; k < NACT; k++) {
        const int i = ACT[k];
        sm_update(TH->sm_e[i], bit, 255);
        uint8_t *s = &TH->gp_[i]->s[slot];
        *s = NEX[*s][bit];
    }

    /* learned match confidence */
    if (TH->m_idx >= 0) sm_update(&TH->mpr[TH->m_idx], bit, 1023);

    /* advance bit state */
    TH->c0 = (TH->c0 << 1) | bit;
    TH->nc = (TH->nc << 1) | bit;
    TH->nbits++;

    if (TH->nbits == 4) {
        nib_begin(TH, 1, TH->c0 & 15);
        TH->nc = 1;
    } else if (TH->nbits == 8) {
        int byte = TH->c0 & 255;
        TH->buf[TH->buflen++] = (uint8_t)byte;
        size_t pos = TH->buflen - 1;
        push_word(TH, byte);
        TH->hist = (TH->hist << 8) | (uint64_t)byte;
        TH->c0 = 1; TH->nc = 1; TH->nbits = 0;
        /* TH->rcol tracks the column of the byte about to be predicted, so it
         * advances before rehash.  The stride is fixed for the whole stream
         * now, so the phase never re-anchors and the column statistics keep
         * accumulating instead of being reset mid-file. */
        if (TH->stride && ++TH->rcol >= TH->stride) TH->rcol = 0;
        rehash(TH);
        update_match(TH, pos);
        nib_begin(TH, 0, 0);
    }
}

/* Feed a byte through the history without modelling it.  Stored blocks use
 * this so their contents stay visible to the match model and the record
 * model, while leaving the context tables uncontaminated by noise. */
static void absorb_byte(Ctx *TH, int byte) {
    TH->buf[TH->buflen++] = (uint8_t)byte;
    size_t pos = TH->buflen - 1;
    push_word(TH, byte);
    TH->hist = (TH->hist << 8) | (uint64_t)byte;
    if (TH->stride && ++TH->rcol >= TH->stride) TH->rcol = 0;
    update_match(TH, pos);
}

static void resync(Ctx *TH) { TH->c0 = 1; TH->nc = 1; TH->nbits = 0; rehash(TH); nib_begin(TH, 0, 0); TH->need_ctx = 0; }

/* --------------------------------------------------------- match bypass
 * When the match model has held for BYT bytes running, the full model is
 * being paid to confirm what the match already knows: the bit costs ~0.01
 * bits to code and ~27 group probes, 30 StateMap loads, the ISSE chain, the
 * mixer and up to six serial APMs to predict.  Inside such a run each byte is
 * coded through the match StateMap alone -- still adaptive, one L1 load per
 * bit -- and the byte is then absorbed exactly like a stored byte, so the
 * match model, record phase and word state stay coherent.  paq9a and zpaq's
 * mid methods are the precedent.
 *
 * The gate reads only mlen and mptr, which encoder and decoder maintain
 * identically, so the decision is always symmetric.  Context tables are not
 * updated during a bypassed byte (that is the ratio cost, swept with BYT);
 * group pointers are refreshed lazily on the next modelled byte.
 *
 * If the predicted bit misses mid-byte the match dies, and the rest of the
 * byte is coded through bsm -- a 256-entry StateMap over the partial byte,
 * trained only on bypassed bits, so it is warm for exactly this situation. */
/* Gate in matched bytes, set per preset in set_level: at -9 every point of
 * ratio matters, and 400 is *both* smaller and faster than no bypass on samba
 * (the match StateMap at the top length bucket commits harder than the mixer
 * blend ever does); lower presets trade a fraction of a percent for real
 * time.  0 disables.  Overridable for sweeps. */
#ifndef BYT
#define BYT -1                 /* -1 = per-level default */
#endif
static int BYPT = 0;

static inline int bypass_gate(const Ctx *TH) {
    return BYPT && TH->mlen >= BYPT && (size_t)TH->mptr < TH->buflen;
}

/* Probability for the current bypassed bit, and the entry that learns it. */
static inline uint32_t *bypass_entry(Ctx *TH, int pb, int nb, int *eb_out) {
    if (TH->mlen > 0) {
        int eb = (pb >> (7 - nb)) & 1;
        int b  = TH->mlen < 16 ? TH->mlen
               : (TH->mlen < 32 ? 16 : (TH->mlen < 64 ? 17 : (TH->mlen < 400 ? 18 : 19)));
        *eb_out = eb;
        return &TH->mpr[((b << 1) | eb) * 8 + nb];
    }
    *eb_out = -1;
    return &TH->bsm[TH->c0];
}

static inline void bypass_finish_byte(Ctx *TH, int byte) {
    TH->buf[TH->buflen++] = (uint8_t)byte;
    size_t pos = TH->buflen - 1;
    push_word(TH, byte);
    TH->hist = (TH->hist << 8) | (uint64_t)byte;
    TH->c0 = 1; TH->nc = 1; TH->nbits = 0;
    if (TH->stride && ++TH->rcol >= TH->stride) TH->rcol = 0;
    update_match(TH, pos);
    TH->need_ctx = 1;
}

/* ------------------------------------------------------------ arithmetic */


static void enc_init(Ctx *TH) { TH->x1 = 0; TH->x2 = 0xFFFFFFFFu; }
static void enc_bit(Ctx *TH, int bit, int p) {
    uint32_t xmid = TH->x1 + (uint32_t)(((uint64_t)(TH->x2 - TH->x1) >> 12) * (uint32_t)p);
    if (bit) TH->x2 = xmid; else TH->x1 = xmid + 1;
    while (((TH->x1 ^ TH->x2) & 0xFF000000u) == 0) {
        TH->obuf[TH->opos++] = (uint8_t)(TH->x2 >> 24);
        TH->x1 <<= 8; TH->x2 = (TH->x2 << 8) | 255;
    }
}
static void enc_flush(Ctx *TH) {
    for (int s = 24; s >= 0; s -= 8) TH->obuf[TH->opos++] = (uint8_t)((TH->x1 >> s) & 255);
}
static void dec_init(Ctx *TH, const uint8_t *d, size_t n) {
    TH->ibuf = d; TH->ilen = n; TH->ipos = 4; TH->x1 = 0; TH->x2 = 0xFFFFFFFFu; TH->xx = 0;
    for (int i = 0; i < 4; i++) TH->xx = (TH->xx << 8) | (i < (int)n ? d[i] : 0);
}
static int dec_bit(Ctx *TH, int p) {
    uint32_t xmid = TH->x1 + (uint32_t)(((uint64_t)(TH->x2 - TH->x1) >> 12) * (uint32_t)p);
    int bit;
    if (TH->xx <= xmid) { bit = 1; TH->x2 = xmid; } else { bit = 0; TH->x1 = xmid + 1; }
    while (((TH->x1 ^ TH->x2) & 0xFF000000u) == 0) {
        TH->x1 <<= 8; TH->x2 = (TH->x2 << 8) | 255;
        TH->xx = (TH->xx << 8) | (TH->ipos < TH->ilen ? TH->ibuf[TH->ipos] : 0);
        TH->ipos++;
    }
    return bit;
}

/* Encoder and decoder halves of one bypassed byte.  They must mirror each
 * other exactly: same entry, same clamp, same updates, same break rule. */
static void bypass_enc_byte(Ctx *TH, int byte) {
    const int pb = TH->buf[TH->mptr];
    for (int nb = 0; nb < 8; nb++) {
        int eb, bit = (byte >> (7 - nb)) & 1;
        uint32_t *e = bypass_entry(TH, pb, nb, &eb);
        int p = (int)(*e >> 20);
        p = p < 1 ? 1 : (p > 4094 ? 4094 : p);
        enc_bit(TH, bit, p);
        sm_update(e, bit, eb >= 0 ? 1023 : 255);
        if (eb >= 0 && bit != eb) TH->mlen = 0;
        TH->c0 = (TH->c0 << 1) | bit;
    }
    bypass_finish_byte(TH, byte);
}

static int bypass_dec_byte(Ctx *TH) {
    const int pb = TH->buf[TH->mptr];
    int byte = 0;
    for (int nb = 0; nb < 8; nb++) {
        int eb;
        uint32_t *e = bypass_entry(TH, pb, nb, &eb);
        int p = (int)(*e >> 20);
        p = p < 1 ? 1 : (p > 4094 ? 4094 : p);
        int bit = dec_bit(TH, p);
        sm_update(e, bit, eb >= 0 ? 1023 : 255);
        if (eb >= 0 && bit != eb) TH->mlen = 0;
        TH->c0 = (TH->c0 << 1) | bit;
        byte = (byte << 1) | bit;
    }
    bypass_finish_byte(TH, byte);
    return byte;
}

/* ------------------------------------------------------------ model setup */

/* Returns a 64-byte-aligned interior pointer that carries its own base in the
 * eight bytes just below it, so afree() can hand the original back to the
 * allocator.  The base used to be discarded, which was harmless while a run
 * built exactly one model and then exited.  Segmented archiving builds one per
 * segment in a single process, and at -9 a model is well over a gigabyte, so
 * the second segment of a large file would have exhausted memory.
 *
 * The +8 before rounding is what guarantees room for the base: aligning
 * raw+8 upward lands somewhere in [raw+8, raw+71], never closer than the
 * eight bytes the header needs. */
static void *aalloc(size_t bytes) {
    uint8_t *raw = calloc(bytes + 72, 1);
    if (!raw) { fprintf(stderr, "oom\n"); exit(1); }
    uintptr_t a = ((uintptr_t)raw + 8 + 63) & ~(uintptr_t)63;
    memcpy((void *)(a - 8), &raw, sizeof raw);
    return (void *)a;
}

static void afree(void *p) {
    if (!p) return;
    uint8_t *raw;
    memcpy(&raw, (uint8_t *)p - 8, sizeof raw);
    free(raw);
}

static int MEMSHIFT = 0;       /* added to every GBITS; stored in the header */

/* Weight sets the mixer actually indexes.  The speed presets use a narrower
 * selector, so their weight table is 128 KB instead of 1 MB. */
static int WSETS_ACT = WSETS;

static void set_level(int lvl) {
    LEVEL = lvl;
    FASTP = 0;
    if (lvl >= 100) {
        /* ------------------------------------------------- speed presets
         * Everything above is built to make the output as small as the model
         * can manage.  These two are built the other way round: pick a
         * throughput, then spend what is left on ratio.
         *
         * They were designed on the theory that table size is the speed lever
         * -- keep the working set in L2 and the dependent-load chain shortens
         * from ~80 ns misses to ~12 ns hits.  Measurement did not support it
         * (see the -f1 table below), so what these presets actually are is
         * the ladder continued downward: fewer contexts, a shorter ISSE
         * chain, no SSE, a narrower mixer selector, and a bypass gate one
         * notch more willing to trade.
         *
         * The match model stays large regardless: it is one probe per byte
         * rather than one per bit, and at three orders it carries far more of
         * the ratio than any single context does. */
        if (lvl >= 102) {                      /* -f2 */
            /* Adding order 4 and the word context costs one more probe per
             * nibble and roughly doubles the working set; on text that is
             * where three byte orders alone fall apart. */
            int o[] = {1, 2, 3, 4, -1};
            int g[] = {14, 18, 19, 19, 20};    /* 38 MB of context tables */
            _Static_assert(sizeof g / sizeof g[0] == sizeof o / sizeof o[0],
                           "f2: GBITS and ORD lengths disagree");
            int c[] = {1, 2, 3};
            NCTX = 5; NISSE = 3;
            memcpy(ORD, o, sizeof o); memcpy(GBITS, g, sizeof g);
            memcpy(ICHAIN, c, sizeof c);
            MMASK = (1u << FMM2) - 1; USE_SSE2 = 1; NAPM = 1; A46B = 4;
        } else {                               /* -f1 */
            int o[] = {1, 2, 3};
            /* The premise these presets were designed around -- that a
             * cache-resident table set is the speed lever -- did not survive
             * measurement.  Scanning -m-2 to -m2 over prose, markup, a source
             * tree and a binary moved total time by 2.5% and total size by
             * 4.0%: the per-nibble prefetch overlaps the misses, so a table
             * 16x larger costs almost nothing to read.  These are therefore
             * sized for ratio, not for L2, and the preset's speed comes from
             * running three contexts with no SSE instead of from where they
             * live.  9.4 MB. */
            int g[] = {16, 18, 18};
            _Static_assert(sizeof g / sizeof g[0] == sizeof o / sizeof o[0],
                           "f1: GBITS and ORD lengths disagree");
            int c[] = {1, 2};
            NCTX = 3; NISSE = 2;
            MMASK = (1u << FMM1) - 1; USE_SSE2 = 0; NAPM = 0; A46B = 4;
            memcpy(ORD, o, sizeof o); memcpy(GBITS, g, sizeof g);
            memcpy(ICHAIN, c, sizeof c);
        }
        FASTP = 1;
    } else if (lvl >= 9) {
        int o[] = {1, 2, 3, 4, 5, 6, 7, 8, 12, 16, -1, -6, -7, -8, -9, -10, -11,
                   -2, -3, -4, -5, -13, -14, -15, -16, -17, -18, -19, -20, -21};
        /* Not every context deserves the same table.  The word contexts and
         * the mid byte orders carry most of English; the shape, line and
         * sparse contexts have small domains and would only waste the space. */
        int g[] = {14, 19, 20, WB1, WB1, WB1, 20, 20, 20, 20,
                   WB2, WB2, WB2, 18, 18, 20, 20, 18, 18, 20, 20,
                   20, 20, 18, 18, 18, 18, 16, 20, 18};
        /* One entry per ORD entry, or the tail contexts silently take their
         * size from zeroed globals and get clamped to 2^10 -- 1024 groups
         * instead of a quarter-million, which makes them look worthless when
         * they are merely starved. */
        _Static_assert(sizeof g / sizeof g[0] == sizeof o / sizeof o[0],
                       "level 9: GBITS and ORD lengths disagree");
        int c[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 13};
        /* The three raster contexts are only in the model when a period was
         * actually found.  ICHAIN references indices 1..16 only, so appending
         * at the tail leaves the ISSE chain untouched. */
        NCTX = DET_STRIDE ? 30 : 27; NISSE = CHAINN;
        memcpy(ORD, o, sizeof o); memcpy(GBITS, g, sizeof g);
        memcpy(ICHAIN, c, sizeof c);
        MMASK = (1u << MMASKB) - 1; USE_SSE2 = 1;
        NAPM = 6; A46B = 10;
    } else if (lvl >= 7) {
        /* Everything at level 9 that pays for itself twice over, minus the
         * long orders, the second indirect and the small-domain oddities. */
        int o[] = {1, 2, 3, 4, 5, 6, 7, 8, -1, -6, -7, -8, -9, -10, -11,
                   -13, -17, -4, -5};
        int g[] = {14, 19, 20, WB1, WB1, WB1, 20, 20,
                   WB2, WB2, WB2, 18, 18, 20, 20, 20, 18, 20, 20};
        _Static_assert(sizeof g / sizeof g[0] == sizeof o / sizeof o[0],
                       "level 7: GBITS and ORD lengths disagree");
        int c[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        NCTX = 19; NISSE = 9;
        memcpy(ORD, o, sizeof o); memcpy(GBITS, g, sizeof g);
        memcpy(ICHAIN, c, sizeof c);
        MMASK = (1u << 21) - 1; USE_SSE2 = 1; NAPM = 4; A46B = 6;
    } else if (lvl >= 5) {
        /* The old level 5 carried the two record contexts, which are inert
         * unless a period was detected, and carried no word-pair and no line
         * model -- the two things that actually pay on text.  That is why it
         * lost to lpaq1 on enwik8 on both size and speed. */
        int o[] = {1, 2, 3, 4, 5, 6, -1, -6, -9, -10, -13, -17};
        int g[] = {14, 19, 20, 21, 21, 21, 22, 22, 18, 20, 20, 18};
        _Static_assert(sizeof g / sizeof g[0] == sizeof o / sizeof o[0],
                       "level 5: GBITS and ORD lengths disagree");
        int c[] = {1, 2, 3, 4, 5, 6, 7};
        NCTX = 12; NISSE = 7;
        memcpy(ORD, o, sizeof o); memcpy(GBITS, g, sizeof g);
        memcpy(ICHAIN, c, sizeof c);
        MMASK = (1u << 20) - 1; USE_SSE2 = 1; NAPM = 3; A46B = 6;
    } else if (lvl >= 3) {
        int o[] = {1, 2, 3, 4, 6, -1, -6, -9};
        int g[] = {14, 18, 19, 20, 20, 21, 21, 18};
        _Static_assert(sizeof g / sizeof g[0] == sizeof o / sizeof o[0],
                       "level 3: GBITS and ORD lengths disagree");
        int c[] = {1, 2, 3, 4, 5};
        NCTX = 8; NISSE = 5;
        memcpy(ORD, o, sizeof o); memcpy(GBITS, g, sizeof g);
        memcpy(ICHAIN, c, sizeof c);
        MMASK = (1u << 19) - 1; USE_SSE2 = 1; NAPM = 2; A46B = 4;
    } else if (lvl >= 2) {
        int o[] = {1, 2, 3, 4, 6, -1};
        int g[] = {14, 18, 19, 19, 19, 20};
        _Static_assert(sizeof g / sizeof g[0] == sizeof o / sizeof o[0],
                       "level 2: GBITS and ORD lengths disagree");
        int c[] = {1, 2, 3, 4};
        NCTX = 6; NISSE = 4;
        memcpy(ORD, o, sizeof o); memcpy(GBITS, g, sizeof g);
        memcpy(ICHAIN, c, sizeof c);
        MMASK = (1u << 19) - 1; USE_SSE2 = 1; NAPM = 1; A46B = 4;
    } else {
        int o[] = {1, 2, 3, 4};
        int g[] = {14, 17, 17, 17};
        int c[] = {1, 2, 3};
        NCTX = 4; NISSE = 3;
        memcpy(ORD, o, sizeof o); memcpy(GBITS, g, sizeof g);
        memcpy(ICHAIN, c, sizeof c);
        MMASK = (1u << 18) - 1; USE_SSE2 = 0; NAPM = 0; A46B = 4;
    }
    /* NISSE indexes ICHAIN and the per-stage weight banks; a level or a
     * CHAINN override must not be able to walk off either. */
    if (NISSE > (int)(sizeof ICHAIN / sizeof ICHAIN[0])) NISSE = (int)(sizeof ICHAIN / sizeof ICHAIN[0]);
    if (NISSE > 16) NISSE = 16;
    /* NAPM indexes a six-entry array of APM stages, and NCTX indexes every
     * per-context array.  Both are set from a level table above, so these can
     * only fire on a mistake in that table -- which is exactly the mistake
     * that shipped once already as a GBITS/ORD length mismatch. */
    if (NAPM > 6) NAPM = 6; else if (NAPM < 0) NAPM = 0;
    if (NCTX > MAXCTX) { fprintf(stderr, "level %d: NCTX %d > MAXCTX %d\n",
                                 lvl, NCTX, MAXCTX); exit(1); }
    ISTART = 0;                             /* order-1 seeds the chain */
    NIN = NCTX + 5;                         /* + match, bias, 3 chain taps */
    MIXW = (NIN + 7) & ~7;                  /* whole SIMD vectors only */
    if (MIXW > NPAD) MIXW = NPAD;
    WSETS_ACT = FASTP ? (1 << FWB) : WSETS; /* must cover every wsel below */
    for (int i = 0; i < NCTX; i++) {
        int b = GBITS[i] + MEMSHIFT;
        if (b < 10) b = 10; else if (b > 24) b = 24;
        GBITS[i] = b;
        GMASK[i] = (1u << b) - 1;
    }
    NACT = 0;
    memset(ISACT, 0, sizeof ISACT);
    for (int i = 0; i < NCTX; i++)
        if (DET_STRIDE || (ORD[i] != -4 && ORD[i] != -5)) {
            ACT[NACT++] = i;
            ISACT[i] = 1;
        }
    /* Per-byte side state, kept only where a context in this preset reads it. */
    NEED_IND = NEED_DLOG = NEED_NEST = 0;
    for (int i = 0; i < NCTX; i++) {
        if (ORD[i] == -13 || ORD[i] == -14) NEED_IND  = 1;
        if (ORD[i] == -15)                  NEED_DLOG = 1;
        if (ORD[i] == -16)                  NEED_NEST = 1;
    }
    /* Gates swept on six files that disagree, not on samba alone -- picking a
     * gate on one file is what put 48 here, and 48 is actively harmful:
     *
     *   -1  gate  48   +1.289% size  -15.2% time   nci +14.28%, xml +5.63%
     *   -1  gate  96   -0.263%        -7.0%        worst file  +0.23%
     *   -3  gate  48   +2.096%       -16.4%        nci +22.40%, xml +8.55%
     *   -3  gate  96   -0.142%        -8.2%        worst file  +1.22%
     *
     * Below ~96 the bypass engages on medium-length matches, where the full
     * model still contributes a great deal, and highly repetitive files (nci,
     * xml) pay for it enormously.  At 96 and above the bypass is a *ratio win*
     * as well as a speed win at every preset: at -1 gate 96 is both smaller
     * and faster than gate 192, and both beat no bypass on size.  At -9, 400
     * is the smallest of 0/192/400/800.  See byt_sweep.py. */
    /* The speed presets were specified with gates of 16 and 24.  Swept over
     * the same six files, 16 is the old 48 mistake again, one size down:
     *
     *   -f1 gate  16   +6.19% size  -35.2% time   nci +24.68%, xml +16.50%
     *   -f1 gate  32   +2.04%       -21.8%        nci +14.78%
     *   -f1 gate  64   +0.18%       -10.8%        worst file  +2.99%
     *   -f1 gate  96   -0.34%        -7.5%        worst file  +0.02%
     *
     * 96 dominates no bypass at all on both axes and is what a ratio preset
     * should take.  -f1 exists to be fast, and 64 buys another 3.6% time for
     * half a percent of size, which is a better rate than the rung below it
     * on the ladder offers -- so -f1 takes 64 and stops there, well short of
     * where the repetitive files fall apart.  -f2 sits between -1 and -3,
     * both of which measured 96, and takes 96 by interpolation. */
    BYPT = BYT >= 0 ? BYT
         : (lvl >= 102 ? 96 : (lvl >= 100 ? 64
         : (lvl >= 9 ? 400 : (lvl >= 7 ? 128 : 96))));
    /* Gates on work that cannot change the answer.  -1 takes the default set
     * here, 0 forces off.  A preset with no measured win keeps its gate off
     * rather than inheriting a guess from a neighbour.
     *
     * SSE tail gate, |stretch(p)| beyond which a3..a6 are skipped.  Swept on
     * the six files:
     *   -9  1024  +0.014% size  -14.6% time   (384/512/768: +0.26/+0.19/+0.08)
     *   -7   768  +0.086%        -7.3%        confirmed A-B-A-B
     * Below -7 only one stage would ever be gated (NAPM is 3), so it is not
     * worth a sweep and stays off. */
    SSEGATE = SSEG >= 0 ? SSEG
            : (lvl >= 100 ? 0 : (lvl >= 9 ? 1024 : (lvl >= 7 ? 768 : 0)));
    /* ISSE chain leaves early once its running estimate is railed.
     *   -9  +0.030% size  -3.7% time.  Bar set in advance was <0.05%. */
    ISSEXIT = IXIT >= 0 ? IXIT : (lvl >= 9 && lvl < 100);
    /* Update thinning: skip mixer/ISSE training when |err| is below this.
     * Filed under speed; it is a ratio change, and the largest one here.
     * Swept at four presets, best of {0,4,8,16,32} at -9 and -5 both 16:
     *   -1 -0.215%   -3 -0.377%   -5 -0.447%   -9 -0.702%
     * Monotone in model size, which is what regularisation should look like:
     * the more weights there are to jitter, the more not jittering them is
     * worth.  -7 and the -f presets sit inside that measured range. */
    THINE   = THIN >= 0 ? THIN : 16;
}

static void model_alloc(Ctx *TH, size_t cap) {
    sm_init(TH);        /* NEX/DT are shared and built once, before threads start */
    for (int i = 0; i < NCTX; i++) {
        if (!ISACT[i]) { TH->T[i] = NULL; continue; }  /* never probed */
        size_t ng = (size_t)1 << GBITS[i];
        TH->T[i] = aalloc(ng * sizeof(Group));   /* zeroed == "no history yet" */
    }
    TH->W = aalloc((size_t)WSETS_ACT * (size_t)MIXW * sizeof(int16_t));
    for (size_t i = 0; i < (size_t)WSETS_ACT * (size_t)MIXW; i++)
        TH->W[i] = (int16_t)(65536 / 4 / 8);
    /* Only the stages this preset actually evaluates get a table. */
    apm_init(&TH->a1, NAPM > 0 ? 256           : 0);
    apm_init(&TH->a2, NAPM > 1 ? 65536         : 0);
    apm_init(&TH->a3, NAPM > 2 ? 256 * 8       : 0);
    apm_init(&TH->a4, NAPM > 3 ? 256 << A46B   : 0);
    apm_init(&TH->a5, NAPM > 4 ? 256 * 128     : 0);  /* cs is 7 bits: exact */
    apm_init(&TH->a6, NAPM > 5 ? 256 << A46B   : 0);
    for (int b = 0; b < 20; b++) {
        int k = b < 16 ? b : 15;
        int hi = 32768 + 1800 * k, lo = 32768 - 1800 * k;
        if (hi > 65280) hi = 65280;
        if (lo < 256)   lo = 256;
        for (int j = 0; j < 8; j++) {
            TH->mpr[(((b << 1) | 1) * 8) + j] = (uint32_t)(hi * 64) << 10;
            TH->mpr[(((b << 1) | 0) * 8) + j] = (uint32_t)(lo * 64) << 10;
        }
    }
    /* These are as large as the context tables at high -m and were the only
     * allocations whose failure was unchecked -- a null here faults inside the
     * inner loop, far from the cause. */
    TH->mtab  = calloc((size_t)MMASK + 1, sizeof(int32_t));
    TH->buf   = malloc(cap + 8);
    TH->ind1  = calloc(256, sizeof(uint32_t));
    TH->ind2  = calloc(65536, sizeof(uint32_t));
    TH->dlast = calloc(256, sizeof(int32_t));
    TH->dlog = 0; TH->nest = 0;
    if (!TH->mtab || !TH->buf || !TH->ind1 || !TH->ind2 || !TH->dlast) { fprintf(stderr, "oom\n"); exit(1); }
    /* Measured on the encoder over the whole stream and carried in the header,
     * so both sides start with the same answer and neither has to guess. */
    TH->stride = DET_STRIDE; TH->swidth = DET_WIDTH ? DET_WIDTH : 1;
    TH->rcol = 0; TH->blk_alpha = 0; TH->alpha_align = 0;
    memset(TH->st, 0, sizeof TH->st);
    TH->st[NCTX + 1] = 256;                 /* constant bias input */
    /* Every ISSE stage starts as the identity, so a fresh chain reproduces the
     * order-1 prediction exactly and can only earn its way from there. */
    for (int k = 0; k < 16; k++)
        for (int s = 0; s < 256; s++) {
            TH->IW[k][s * 2] = 65536; TH->IW[k][s * 2 + 1] = 0;
        }
    for (int i = 0; i < 256; i++) TH->bsm[i] = (uint32_t)2097151 << 10;
    TH->need_ctx = 0;
    TH->buflen = 0; TH->mptr = 0; TH->mlen = 0;
    TH->hist = 0; TH->word = 0; TH->word2 = 0; TH->word3 = 0;
    TH->wlen = 0; TH->wcap = 0; TH->pcap = 0; TH->col = 0; TH->intag = 0;
    TH->lstart = 0; TH->plstart = 0; TH->plen = 0;
    TH->c0 = 1; TH->nc = 1; TH->nbits = 0; TH->pr = 2048; TH->wsel = 1;
    TH->napm = 0; TH->nisse = NISSE;
    rehash(TH);
    nib_begin(TH, 0, 0);
}

/* Releases everything model_alloc took.  Ctx is allocated zeroed and only ever
 * writes T[i] for i < NCTX, so sweeping the whole array is safe even when a
 * later segment runs a narrower preset than an earlier one.
 *
 * TH->buf is the decoder's output and is handed to the caller as j->plain, so
 * a decode must copy out of it before calling this. */
static void model_free(Ctx *TH) {
    for (int i = 0; i < CTXPAD; i++) { afree(TH->T[i]); TH->T[i] = NULL; }
    afree(TH->W); TH->W = NULL;
    APM *aps[6] = { &TH->a1, &TH->a2, &TH->a3, &TH->a4, &TH->a5, &TH->a6 };
    for (int i = 0; i < 6; i++) { free(aps[i]->t); aps[i]->t = NULL; }
    free(TH->mtab);  TH->mtab  = NULL;
    free(TH->buf);   TH->buf   = NULL;
    free(TH->ind1);  TH->ind1  = NULL;
    free(TH->ind2);  TH->ind2  = NULL;
    free(TH->dlast); TH->dlast = NULL;
}

/* ------------------------------------------------------------ x86 filter
 * Relative CALL/JMP operands encode the *distance* to their target, so two
 * calls to the same function look completely different as byte strings.
 * Rewriting them as absolute addresses makes them identical, which is what
 * the context models actually need.
 *
 * Reversibility needs two things.  First, a predicate the transform cannot
 * disturb: we test the opcode d[i] and the operand's high byte d[i+4], and
 * rewrite only the low 24 bits in between, so a position's eligibility reads
 * the same before and after.  (Testing the value itself does not work -- the
 * encoder would be asking about rel while the decoder asks about abs, and at
 * the 16 MB boundary those disagree.)
 *
 * Second, matching scan order.  An operand at j overlaps the windows of the
 * positions just below it, so the two passes must observe those bytes in the
 * same state.  Encoding backwards and decoding forwards achieves exactly
 * that: at position i, both passes see every transform above i applied and
 * every one below i not applied.  Neither pass skips, so both visit the same
 * positions, and decoding forwards inverts the backward composition in the
 * right order.
 */
/* DEC Alpha is a fixed-length 32-bit RISC encoding with the opcode in bits
 * 31..26, so in real Alpha code the opcode field is heavily skewed at one
 * 4-byte alignment and near-uniform at the other three.  That asymmetry is the
 * signature: an absolute score alone flags any low-entropy data (nci scores
 * 0.93), whereas the gap between the best alignment and the runner-up is above
 * 0.10 only for genuine fixed-length code.
 *
 * Silesia's mozilla is the Tru64 build -- 134 Alpha COFF objects, 88% of the
 * file -- so this is the difference between modelling it as code and modelling
 * it as noise.  The E8/E9 x86 filter was previously firing on coincidental
 * bytes here and actively scrambling it. */
static const uint64_t ALPHA_OPS =
    (1ULL << 0x08) | (1ULL << 0x09) | (1ULL << 0x0a) | (1ULL << 0x0c) |
    (1ULL << 0x0d) | (1ULL << 0x0e) | (1ULL << 0x10) | (1ULL << 0x11) |
    (1ULL << 0x12) | (1ULL << 0x13) | (1ULL << 0x14) | (1ULL << 0x15) |
    (1ULL << 0x16) | (1ULL << 0x17) | (1ULL << 0x18) | (1ULL << 0x1a) |
    0xFFFFFFFF00000000ULL;             /* 0x20..0x3f: loads, stores, branches */

static int alpha_align_of(const uint8_t *d, size_t n, size_t base, int *ok) {
    *ok = 0;
    if (n < 512) return 0;
    int hit[4] = {0, 0, 0, 0}, tot[4] = {0, 0, 0, 0};
    int seen[4][64];
    memset(seen, 0, sizeof seen);
    for (int a = 0; a < 4; a++) {
        size_t st = (size_t)((a - (int)(base & 3)) & 3);
        for (size_t i = st; i + 4 <= n; i += 4) {
            int op = d[i + 3] >> 2;
            if (ALPHA_OPS >> op & 1) hit[a]++;
            seen[a][op]++;
            tot[a]++;
        }
    }
    int b = 0;
    double f[4];
    for (int a = 0; a < 4; a++) {
        f[a] = tot[a] ? (double)hit[a] / tot[a] : 0.0;
        if (f[a] > f[b]) b = a;
    }
    double second = 0.0;
    for (int a = 0; a < 4; a++) if (a != b && f[a] > second) second = f[a];
    if (!(f[b] > AFMIN / 100.0 && f[b] - second > AGAP / 100.0)) return b;

    /* Opcode diversity.  The test above looks only at the top six bits of every
     * fourth byte, so any format with a near-constant byte at one alignment
     * scores ~1.0 there and low elsewhere -- exactly the signature it is
     * looking for.  Monotonically increasing 32-bit integers do this: their
     * high byte barely changes, and if that value happens to land in ALPHA_OPS
     * the whole run reads as perfectly aligned code.  Measured, that wrongly
     * byte-swapped 1,096 KB of a 4 MB array of int32 and cost 2.67%.
     *
     * Real instruction streams use many opcodes and are not dominated by one.
     * A constant field uses exactly one.  That distinction is what separates
     * them, and it costs one 64-entry histogram per window. */
    int distinct = 0, top = 0;
    for (int o = 0; o < 64; o++) {
        if (seen[b][o]) distinct++;
        if (seen[b][o] > top) top = seen[b][o];
    }
    if (distinct < ADIV) return b;
    if (tot[b] && top * 100 > tot[b] * ADOM) return b;
    *ok = 1;
    return b;
}

/* Alpha is little-endian, so the opcode -- bits 31..26, the single most
 * informative field -- lands in the *last* byte of each instruction.  A
 * context model reads left to right, so it is asked to predict the register
 * and displacement bytes before it has seen the opcode that determines them.
 *
 * Reversing the bytes of each instruction puts the opcode first, which is the
 * order the model wants.  The operation is its own inverse and each 4-byte
 * word is independent, so unlike the x86 filter there is no overlap or scan
 * order to get right.
 */
static void alpha_swap(uint8_t *d, size_t n, int align) {
    for (size_t i = (size_t)align; i + 4 <= n; i += 4) {
        uint8_t t0 = d[i], t1 = d[i + 1];
        d[i] = d[i + 3]; d[i + 1] = d[i + 2];
        d[i + 2] = t1;   d[i + 3] = t0;
    }
}

static void e8e9_step(uint8_t *d, size_t i, int decode) {
    if ((d[i] & 0xFE) != 0xE8) return;
    if (d[i+4] != 0x00 && d[i+4] != 0xFF) return;
    uint32_t v = (uint32_t)d[i+1] | ((uint32_t)d[i+2] << 8) | ((uint32_t)d[i+3] << 16);
    uint32_t delta = (uint32_t)(i + 5);
    uint32_t w = (decode ? v - delta : v + delta) & 0xFFFFFF;
    d[i+1] = (uint8_t)w; d[i+2] = (uint8_t)(w >> 8); d[i+3] = (uint8_t)(w >> 16);
}

static void e8e9(uint8_t *d, size_t n, int decode) {
    if (n < 6) return;
    if (decode) {
        for (size_t i = 0; i + 4 < n; i++) e8e9_step(d, i, 1);
    } else {
        for (size_t i = n - 5; ; i--) { e8e9_step(d, i, 0); if (i == 0) break; }
    }
}

/* ----------------------------------------------------------- alphabet pack
 * A file written over a tiny alphabet -- ASCII '0'/'1', ACGT, hex digits --
 * spends a whole byte on one or two bits of information.  No amount of
 * context modelling recovers that: the model still has to emit eight binary
 * decisions per symbol.  Packing the symbols first is what actually helps,
 * and it makes everything downstream several times faster as a bonus.
 */
static int scan_alphabet(const uint8_t *d, size_t n, uint8_t *sym, int *nsym, int *bps) {
    if (n < 1024) return 0;
    uint8_t seen[256] = {0};
    int k = 0;
    for (size_t i = 0; i < n; i++)
        if (!seen[d[i]]) { seen[d[i]] = 1; if (++k > 16) return 0; }
    *bps = (k <= 2) ? 1 : (k <= 4) ? 2 : 4;
    *nsym = 0;
    for (int i = 0; i < 256; i++) if (seen[i]) sym[(*nsym)++] = (uint8_t)i;
    return 1;
}

static size_t pack_syms(const uint8_t *d, size_t n, uint8_t *out,
                        const uint8_t *sym, int nsym, int bps) {
    uint8_t map[256];
    for (int i = 0; i < nsym; i++) map[sym[i]] = (uint8_t)i;
    int per = 8 / bps;
    size_t on = (n + per - 1) / per;
    memset(out, 0, on);
    for (size_t i = 0; i < n; i++)
        out[i / per] |= (uint8_t)(map[d[i]] << (bps * (per - 1 - (int)(i % per))));
    return on;
}

static void unpack_syms(const uint8_t *p, uint8_t *out, size_t n,
                        const uint8_t *sym, int bps) {
    int per = 8 / bps, mask = (1 << bps) - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = sym[(p[i / per] >> (bps * (per - 1 - (int)(i % per)))) & mask];
}

/* -------------------------------------------------------- deflate recompress
 * Files like a software distribution are mostly *other* compressed files.  A
 * context model can do nothing with a DEFLATE stream -- it is already near 8
 * bits per byte -- but the text behind it compresses beautifully.  So inflate
 * the stream, model the plaintext, and re-deflate on the way back out.
 *
 * The catch is that DEFLATE is not canonical: the same plaintext has many
 * valid encodings, and we must reproduce the original encoder's bytes exactly.
 * We handle that by never trusting the transform.  For each stream we search
 * zlib's parameter space, re-deflate, and compare against the original bytes.
 * Only an exact match is accepted; anything else is left compressed and
 * modelled as-is.  Correctness therefore does not depend on guessing right --
 * a wrong guess costs ratio, never data.
 */
#include <zlib.h>

typedef struct {
    uint64_t pos;        /* offset of the plaintext in the working buffer */
    uint32_t clen;       /* original compressed length */
    uint32_t plen;       /* plaintext length */
    int16_t  wbits;      /* window bits: 15 zlib, -15 raw, 31 gzip */
    uint8_t  level, mem, strat;
} Dfl;

/* zlib's own defaults first: most streams in the wild are produced by zlib or
 * by something that copies its settings, so the search usually ends at once. */
static const struct { uint8_t l, m, s; } DCOMBO[] = {
    {6,8,0},{9,8,0},{1,8,0},{5,8,0},{7,8,0},{8,8,0},{4,8,0},{3,8,0},{2,8,0},
    {6,9,0},{9,9,0},{1,9,0},{5,9,0},{7,9,0},{8,9,0},{4,9,0},{3,9,0},{2,9,0},
    {6,8,1},{9,8,1},{1,8,1},{6,9,1},{9,9,1},{1,9,1},
    {6,7,0},{9,7,0},{1,7,0},{6,6,0},{9,6,0},{1,6,0},
};
#define NCOMBO ((int)(sizeof DCOMBO / sizeof DCOMBO[0]))

/* Inflate at d[0..n).  Returns plaintext (malloc'd) and sets *used to the
 * number of input bytes consumed, or NULL. */
static uint8_t *dfl_inflate(const uint8_t *d, size_t n, int wbits,
                            size_t *plen, size_t *used) {
    z_stream z; memset(&z, 0, sizeof z);
    if (inflateInit2(&z, wbits) != Z_OK) return NULL;
    /* Start small and grow.  Sizing this from the bytes remaining in the file
     * would allocate hundreds of megabytes per candidate on a large input,
     * and most candidates are false positives that fail immediately. */
    size_t cap = n < 65536 ? n * 4 + 65536 : 262144;
    uint8_t *out = malloc(cap);
    if (!out) { inflateEnd(&z); return NULL; }
    z.next_in = (Bytef *)d; z.avail_in = (uInt)n;
    z.next_out = out; z.avail_out = (uInt)cap;
    int r;
    for (;;) {
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_STREAM_END || r == Z_BUF_ERROR || r < 0) break;
        if (z.avail_out == 0) {
            if (cap > (64u << 20)) { r = Z_MEM_ERROR; break; }
            size_t old = cap; cap *= 2;
            uint8_t *t = realloc(out, cap);
            if (!t) { r = Z_MEM_ERROR; break; }
            out = t; z.next_out = out + old; z.avail_out = (uInt)(cap - old);
        }
    }
    if (r != Z_STREAM_END) { free(out); inflateEnd(&z); return NULL; }
    *plen = z.total_out; *used = z.total_in;
    inflateEnd(&z);
    return out;
}

/* Does some parameter set reproduce `orig` exactly from `plain`? */
static int dfl_match(const uint8_t *plain, size_t plen, const uint8_t *orig,
                     size_t clen, int wbits, Dfl *out) {
    uint8_t *tmp = malloc(clen + 64);
    if (!tmp) return 0;
    for (int i = 0; i < NCOMBO; i++) {
        z_stream z; memset(&z, 0, sizeof z);
        if (deflateInit2(&z, DCOMBO[i].l, Z_DEFLATED, wbits,
                         DCOMBO[i].m, DCOMBO[i].s) != Z_OK) continue;
        z.next_in = (Bytef *)plain; z.avail_in = (uInt)plen;
        z.next_out = tmp; z.avail_out = (uInt)(clen + 64);
        int r = deflate(&z, Z_FINISH);
        size_t got = z.total_out;
        deflateEnd(&z);
        if (r == Z_STREAM_END && got == clen && !memcmp(tmp, orig, clen)) {
            out->level = DCOMBO[i].l; out->mem = DCOMBO[i].m;
            out->strat = DCOMBO[i].s; out->wbits = (int16_t)wbits;
            free(tmp);
            return 1;
        }
    }
    free(tmp);
    return 0;
}

/* Re-create the original compressed bytes.  Used on the decode path, where
 * the parameters are known exactly, so this must and does succeed. */
static int dfl_deflate(const uint8_t *plain, size_t plen, const Dfl *f,
                       uint8_t *out, size_t cap) {
    z_stream z; memset(&z, 0, sizeof z);
    if (deflateInit2(&z, f->level, Z_DEFLATED, f->wbits, f->mem, f->strat) != Z_OK)
        return 0;
    z.next_in = (Bytef *)plain; z.avail_in = (uInt)plen;
    z.next_out = out; z.avail_out = (uInt)cap;
    int r = deflate(&z, Z_FINISH);
    size_t got = z.total_out;
    deflateEnd(&z);
    return (r == Z_STREAM_END && got == f->clen);
}

#define MAXDFL 65536

/* Scan for zlib, gzip and ZIP-member streams, verify each, and build the
 * expanded buffer.  Returns the new buffer; *nd gets the descriptor count. */
static uint8_t *dfl_expand(const uint8_t *d, size_t n, Dfl *fl, int *nd,
                           size_t *newlen) {
    size_t cap = n * 2 + 65536, olen = 0;
    uint8_t *out = malloc(cap);
    if (!out) return NULL;
    int cnt = 0;
    size_t i = 0;

    /* A bare zlib header is only two bytes with a five-bit check, so inside
     * tens of megabytes of machine code plenty of positions pass it by chance,
     * and a few of those even inflate and re-deflate byte-exactly.  Replacing
     * them with their "plaintext" splices bytes into the middle of the code,
     * which shifts everything after by a non-multiple of four and destroys the
     * fixed-length instruction alignment the Alpha model depends on --
     * measured as 23 MB of detected code collapsing to 0.3 MB.
     *
     * So map the code regions first and decline bare-zlib candidates inside
     * them.  Real containers (PK, gzip) keep their longer signatures and are
     * still accepted, because those genuinely do appear inside archives. */
    size_t nwin = (n + SEGWIN - 1) / SEGWIN;
    uint8_t *acode = calloc(nwin ? nwin : 1, 1);
    if (acode)
        for (size_t wi = 0; wi < nwin; wi++) {
            size_t off = wi * SEGWIN, len = (n - off < SEGWIN) ? n - off : SEGWIN;
            int ok = 0;
            alpha_align_of(d + off, len, off, &ok);
            acode[wi] = (uint8_t)ok;
        }

    while (i + 8 < n) {
        int wbits = 0;
        size_t at = i, avail = n - i;
        if (d[i] == 'P' && d[i+1] == 'K' && d[i+2] == 3 && d[i+3] == 4 && i + 30 < n) {
            unsigned method = d[i+8] | (d[i+9] << 8);
            unsigned fnlen  = d[i+26] | (d[i+27] << 8);
            unsigned exlen  = d[i+28] | (d[i+29] << 8);
            size_t st = i + 30 + fnlen + exlen;
            if (method == 8 && st < n) { wbits = -15; at = st; avail = n - st; }
        } else if (d[i] == 0x78 && ((d[i] * 256 + d[i+1]) % 31) == 0 && !(d[i+1] & 0x20)) {
            wbits = 15;
        } else if (d[i] == 0x1F && d[i+1] == 0x8B && d[i+2] == 8) {
            wbits = 31;
        }

        if (wbits && !(acode && acode[i / SEGWIN])) {
            size_t plen = 0, used = 0;
            uint8_t *plain = dfl_inflate(d + at, avail, wbits, &plen, &used);
            if (plain && used >= 64 && plen >= 128 && cnt < MAXDFL) {
                Dfl f; f.clen = (uint32_t)used; f.plen = (uint32_t)plen;
                if (dfl_match(plain, plen, d + at, used, wbits, &f)) {
                    size_t head = at - i;               /* bytes before the stream */
                    while (olen + head + plen + 64 > cap) {
                        cap *= 2;
                        uint8_t *t = realloc(out, cap);
                        if (!t) { free(plain); free(out); free(acode); return NULL; }
                        out = t;
                    }
                    memcpy(out + olen, d + i, head); olen += head;
                    f.pos = olen;
                    memcpy(out + olen, plain, plen); olen += plen;
                    fl[cnt++] = f;
                    free(plain);
                    i = at + used;
                    continue;
                }
            }
            free(plain);
        }
        if (olen + 1 > cap) {
            cap *= 2;
            uint8_t *t = realloc(out, cap);
            if (!t) { free(out); free(acode); return NULL; }
            out = t;
        }
        out[olen++] = d[i++];
    }
    while (i < n) {
        if (olen + 1 > cap) {
            cap *= 2;
            uint8_t *t = realloc(out, cap);
            if (!t) { free(out); free(acode); return NULL; }
            out = t;
        }
        out[olen++] = d[i++];
    }
    free(acode);
    *nd = cnt; *newlen = olen;
    return out;
}

/* Walk the descriptors and put the compressed streams back. */
static uint8_t *dfl_collapse(const uint8_t *w, size_t wn, const Dfl *fl, int nd,
                             size_t *outlen) {
    size_t cap = wn + 65536;
    for (int i = 0; i < nd; i++) cap += fl[i].clen;
    uint8_t *out = malloc(cap);
    if (!out) return NULL;
    size_t olen = 0, pos = 0;
    for (int i = 0; i < nd; i++) {
        memcpy(out + olen, w + pos, fl[i].pos - pos);
        olen += fl[i].pos - pos;
        if (!dfl_deflate(w + fl[i].pos, fl[i].plen, &fl[i], out + olen, cap - olen)) {
            free(out); return NULL;             /* cannot happen: verified at encode */
        }
        olen += fl[i].clen;
        pos = fl[i].pos + fl[i].plen;
    }
    memcpy(out + olen, w + pos, wn - pos);
    olen += wn - pos;
    *outlen = olen;
    return out;
}

/* ------------------------------------------------------------ segmentation
 * A whole-file verdict is the wrong granularity.  A tar of executables is not
 * "an executable" -- it never starts with MZ, so a file-level x86 check never
 * fires, and the machine code inside goes unfiltered.  Equally, an archive
 * holding already-compressed members wastes both time and model capacity
 * having them modelled.
 *
 * So classify in windows and merge runs: x86 blocks get the E8/E9 filter,
 * high-entropy blocks bypass the model entirely (their bytes still enter the
 * history so later blocks can match against them), everything else is
 * modelled as before.  The block type also selects the mixer weight set, so
 * the mixer keeps separate weights for code and for text.
 */
#define B_MODEL 0
#define B_X86   1
#define B_STORE 2
#define B_ALPHA 3
/* The stored type byte carries the block kind in bits 0-1 and, for Alpha
 * blocks, the instruction alignment within the chunk in bits 2-3.  That keeps
 * the block table one byte per entry, as the container format expects.
 *
 * Deliberately not named BTYPE: winnt.h defines BTYPE(x) as ((x) & N_BTMASK)
 * for COFF symbol types, and <windows.h> is included further down for the
 * thread API.  From that point on the Windows macro silently won, so every
 * test after it matched only type 3 -- Alpha blocks at alignment 0 -- and the
 * transform never ran on the other three alignments.  segment() sits above the
 * include and so behaved correctly, which is why the block table looked right
 * while the model saw almost no Alpha at all. */
#define BKIND(t)  ((t) & 3)
#define BALGN(t)  (((t) >> 2) & 3)

typedef struct { uint8_t type; uint32_t len; } Blk;

/* The store threshold has to be brutal.  Order-0 entropy of 7.90 does *not*
 * mean incompressible -- the match model still finds repeats across DEFLATE
 * members, and on mozilla that was worth 2.7%.  Only data that is random to
 * order-0 is safe to hand over unmodelled. */
static int classify_window(const uint8_t *d, size_t n, size_t base, int file_is_exe) {
    /* A file that opens with MZ or ELF is a single-architecture image, and that
     * architecture is not DEC Alpha.  Testing for Alpha inside one costs real
     * ratio: on a 3.6 MB x86-64 DLL the detector claimed 136 KB, byte-swapped
     * it, and cost 1.19% -- while also stealing three windows from the E8/E9
     * filter, which is what should have handled them.
     *
     * mozilla is unaffected because it is a tar: it has no MZ header, so
     * file_is_exe is false and its genuine Alpha members are still found. */
    if (!file_is_exe) {
        int aok, aal = alpha_align_of(d, n, base, &aok);
        if (aok) return B_ALPHA | (aal << 2);
    }
    size_t cnt[256] = {0};
    for (size_t i = 0; i < n; i++) cnt[d[i]]++;
    double H = 0.0;
    for (int i = 0; i < 256; i++)
        if (cnt[i]) { double p = (double)cnt[i] / n; H -= p * log2(p); }
    if (H > 7.98) return B_STORE;

    int calls = 0;
    for (size_t i = 0; i + 4 < n; i++)
        if ((d[i] & 0xFE) == 0xE8 && (d[i+4] == 0x00 || d[i+4] == 0xFF)) calls++;

    /* Inside a known executable, filter everything that is not random: the
     * data and resource sections cost nothing to run through the filter and
     * the section boundaries are not worth finding exactly.  Elsewhere -- a
     * tar or an installer holding code -- demand real call density. */
    if (file_is_exe) return B_X86;
    if (calls * 600 > (int)n && H > 5.0) return B_X86;
    return B_MODEL;
}

static int segment(const uint8_t *d, size_t n, Blk *out, int maxb, int file_is_exe) {
    /* base offsets are passed through so the Alpha alignment a block records is
     * relative to the chunk, which is what the model can reconstruct. */
    int nb = 0;
    size_t i = 0;
    while (i < n && nb < maxb) {
        size_t w = (n - i < SEGWIN) ? n - i : SEGWIN;
        int t = classify_window(d + i, w, i, file_is_exe);
        size_t start = i;
        i += w;
        while (i < n) {                                  /* extend the run */
            size_t w2 = (n - i < SEGWIN) ? n - i : SEGWIN;
            if (classify_window(d + i, w2, i, file_is_exe) != t) break;
            i += w2;
        }
        /* a lone store window is not worth a block boundary */
        if (BKIND(t) == B_STORE && i - start < 8 * SEGWIN) t = file_is_exe ? B_X86 : B_MODEL;
        if (nb && out[nb-1].type == t) out[nb-1].len += (uint32_t)(i - start);
        else { out[nb].type = (uint8_t)t; out[nb].len = (uint32_t)(i - start); nb++; }
    }
    if (i < n) {                                         /* ran out of blocks */
        if (nb) out[nb-1].len += (uint32_t)(n - i);
        else { out[0].type = B_MODEL; out[0].len = (uint32_t)n; nb = 1; }
    }
    return nb;
}

static int looks_like_exe(const uint8_t *d, size_t n) {
    if (n < 64) return 0;
    if (d[0] == 'M' && d[1] == 'Z') return 1;
    if (d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F') return 1;
    return 0;
}

static double probe_entropy(const uint8_t *d, size_t n) {
    size_t s = n < 262144 ? n : 262144;
    static size_t cnt[256];
    memset(cnt, 0, sizeof cnt);
    for (size_t i = 0; i < s; i++) cnt[d[i]]++;
    double h = 0.0;
    for (int i = 0; i < 256; i++)
        if (cnt[i]) { double p = (double)cnt[i] / s; h -= p * log2(p); }
    return h;
}


/* ------------------------------------------------------------- threading
 * Chunks are compressed independently, each by a worker owning its own Ctx.
 * That is what makes parallelism possible at all -- and also what it costs:
 * every chunk starts with a cold model, so ratio degrades as -t rises.  The
 * chunk count is recorded in the container, so a file compressed with -t8
 * decodes correctly at any -t, and decoding parallelises the same way.
 */
#ifdef _WIN32
  #include <windows.h>
  #include <psapi.h>
  typedef HANDLE thr_t;
#else
  #include <pthread.h>
  #include <sys/resource.h>
  typedef pthread_t thr_t;
#endif

/* Peak resident set, reported by the process itself.  PeakWorkingSet64 read off
 * an exited process returned zero on this machine, and polling from outside only
 * samples; asking the OS from inside is exact.
 *
 * Defined here rather than beside the other allocation helpers deliberately:
 * it needs <windows.h>, which is included at this point and not before.  A
 * previous version of this file put a macro above this include and had it
 * silently redefined by winnt.h -- position relative to this header is load
 * bearing. */
static size_t peak_rss(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        return (size_t)pmc.PeakWorkingSetSize;
    return 0;
#else
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) return (size_t)ru.ru_maxrss * 1024;
    return 0;
#endif
}

typedef struct {
    Ctx     *cx;
    int      mode;              /* 0 = compress, 1 = decompress */
    uint8_t *in;                /* compress: chunk bytes (filtered in place) */
    size_t   n;                 /* chunk length */
    int      file_is_exe;
    Blk     *blk;
    int      nb;
    uint8_t *aout;  size_t alen;    /* arithmetic stream */
    uint8_t *sout;  size_t slen;    /* stored-block bytes */
    const uint8_t *ain, *sin;       /* decompress inputs */
    uint8_t *plain;                 /* decompress output */
} Job;

static void do_compress_chunk(Job *j) {
    Ctx *TH = j->cx;
    j->blk = malloc(sizeof(Blk) * MAXBLK);
    j->nb  = segment(j->in, j->n, j->blk, MAXBLK, j->file_is_exe);
    size_t off = 0;
    for (int i = 0; i < j->nb; i++) {
        if (BKIND(j->blk[i].type) == B_X86) e8e9(j->in + off, j->blk[i].len, 0);
        else if (BKIND(j->blk[i].type) == B_ALPHA)
            alpha_swap(j->in + off, j->blk[i].len,
                       (int)((BALGN(j->blk[i].type) - off) & 3));
        off += j->blk[i].len;
    }
    model_alloc(TH, j->n);
    j->aout = malloc(j->n + j->n / 4 + 65536);
    j->sout = malloc(j->n + 16);
    j->slen = 0;
    TH->obuf = j->aout; TH->opos = 0; enc_init(TH);
    off = 0;
    for (int i = 0; i < j->nb; i++) {
        TH->blk_x86   = (BKIND(j->blk[i].type) == B_X86);
        TH->blk_alpha = (BKIND(j->blk[i].type) == B_ALPHA);
        TH->alpha_align = BALGN(j->blk[i].type);
        if (BKIND(j->blk[i].type) == B_STORE) {
            memcpy(j->sout + j->slen, j->in + off, j->blk[i].len);
            j->slen += j->blk[i].len;
            for (uint32_t k = 0; k < j->blk[i].len; k++) absorb_byte(TH, j->in[off + k]);
            resync(TH);
        } else {
            for (uint32_t k = 0; k < j->blk[i].len; k++) {
                if (bypass_gate(TH)) { bypass_enc_byte(TH, j->in[off + k]); continue; }
                if (TH->need_ctx) { rehash(TH); nib_begin(TH, 0, 0); TH->need_ctx = 0; }
                for (int b = 7; b >= 0; b--) {
                    int bit = (j->in[off + k] >> b) & 1;
                    enc_bit(TH, bit, predict(TH));
                    update(TH, bit);
                }
            }
        }
        off += j->blk[i].len;
    }
    enc_flush(TH);
    j->alen = TH->opos;
}

static void do_decompress_chunk(Job *j) {
    Ctx *TH = j->cx;
    model_alloc(TH, j->n);
    dec_init(TH, j->ain, j->alen);
    size_t soff = 0;
    for (int i = 0; i < j->nb; i++) {
        TH->blk_x86   = (BKIND(j->blk[i].type) == B_X86);
        TH->blk_alpha = (BKIND(j->blk[i].type) == B_ALPHA);
        TH->alpha_align = BALGN(j->blk[i].type);
        if (BKIND(j->blk[i].type) == B_STORE) {
            for (uint32_t k = 0; k < j->blk[i].len; k++) absorb_byte(TH, j->sin[soff + k]);
            soff += j->blk[i].len;
            resync(TH);
        } else {
            for (uint32_t k = 0; k < j->blk[i].len; k++) {
                if (bypass_gate(TH)) { bypass_dec_byte(TH); continue; }
                if (TH->need_ctx) { rehash(TH); nib_begin(TH, 0, 0); TH->need_ctx = 0; }
                for (int b = 0; b < 8; b++) { int bit = dec_bit(TH, predict(TH)); update(TH, bit); }
            }
        }
    }
    j->plain = TH->buf;
}

#ifdef _WIN32
static DWORD WINAPI thr_entry(LPVOID p) {
#else
static void *thr_entry(void *p) {
#endif
    Job *j = (Job *)p;
    if (j->mode) do_decompress_chunk(j); else do_compress_chunk(j);
    return 0;
}

static void run_jobs(Job *jobs, int n) {
    if (n == 1) { thr_entry(&jobs[0]); return; }
    thr_t *h = malloc(sizeof(thr_t) * n);
    for (int i = 0; i < n; i++) {
#ifdef _WIN32
        h[i] = CreateThread(NULL, 0, thr_entry, &jobs[i], 0, NULL);
        if (!h[i]) { thr_entry(&jobs[i]); }
#else
        if (pthread_create(&h[i], NULL, thr_entry, &jobs[i]) != 0) thr_entry(&jobs[i]);
#endif
    }
    for (int i = 0; i < n; i++) {
#ifdef _WIN32
        if (h[i]) { WaitForSingleObject(h[i], INFINITE); CloseHandle(h[i]); }
#else
        pthread_join(h[i], NULL);
#endif
    }
    free(h);
}


/* ===================================================================
 * Archive layer
 * ===================================================================
 *
 * Everything above this line is the model and is unchanged: it turns a buffer
 * into a compressed blob and back.  Everything below turns that into something
 * you can point at a directory and still trust in five years.
 *
 * The v1 container was a benchmark harness.  It opened with a bare type byte,
 * then read lengths straight out of the file and passed them to malloc.  It
 * had no magic number, no version, no checksum of any kind, no bound on any
 * field it parsed, no way to verify an archive without fully restoring it, and
 * it sized the input with a 32-bit ftell so anything past 2 GB was silently
 * wrong.  For measuring compression ratio all of that is irrelevant.  For data
 * you intend to keep, each one is a separate way to lose the archive without
 * being told.
 *
 * v2 layout:
 *
 *   [header 48B]        magic, version, preset, member count, index offset,
 *                       and an XXH64 over the preceding 40 bytes
 *   [segment payloads]  written in order, streamed, never all resident
 *   [index]             members (name, size, mtime, mode, SHA-256, segment
 *                       range) then segments (offset, lengths, XXH64)
 *   [trailer 20B]       index XXH64, index length, magic
 *
 * The index sits at the end so writing never seeks backwards, which is what
 * lets the whole thing stream.  Its position is recorded twice -- once in the
 * header, once implicitly by the trailer -- so a header damaged in storage
 * does not on its own cost you the archive.
 *
 * A segment is the unit of independent compression, integrity, parallelism and
 * damage containment, all at once.  Corruption inside a segment loses that
 * segment; every other segment still decodes, and 't' reports exactly which
 * ones are gone rather than failing the whole file.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef _WIN32
  #include <direct.h>
  #include <sys/utime.h>
#else
  #include <utime.h>
#endif
#ifdef _WIN32
  #define F_SEEK _fseeki64
  #define F_TELL _ftelli64
#else
  #include <dirent.h>
  #include <unistd.h>
  #define F_SEEK fseeko
  #define F_TELL ftello
#endif

static int NTHREAD = 1;

static int cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long k = sysconf(_SC_NPROCESSORS_ONLN);
    return k > 0 ? (int)k : 1;
#endif
}

/* Levels are carried in one header byte, so the speed presets are just levels
 * 101 and 102; only the spelling differs. */
static const char *lvlname(int lvl) {
    static char b[8];
    if (lvl >= 100) snprintf(b, sizeof b, "f%d", lvl - 100);
    else            snprintf(b, sizeof b, "%d", lvl);
    return b;
}

/* MinGW's plain stat() carries a 32-bit size and fails with EOVERFLOW on
 * anything past 2 GB, which silently cost every large file its timestamp and
 * permission bits -- the archive was correct, its metadata was not.  The
 * 64-bit variant has to be named explicitly. */
#ifdef _WIN32
  #define STAT_T struct _stati64
  #define STAT_F _stati64
#else
  #define STAT_T struct stat
  #define STAT_F stat
#endif

#define GEN_MAGIC   0x414E4547u          /* "GENA" little-endian */
#define GEN_VERSION 2

/* Release version of the *program*, which moves independently of the archive
 * format version above.  A format bump breaks compatibility; a release bump
 * usually does not. */
#define GLEIPNIR_RELEASE "1.0.0"
#define HDR_BYTES   48
#define TRL_BYTES   20
#define SEG_MAX_DEF (64u << 20)          /* default cap on a segment, bytes */
#define NAME_MAX_B  4096
/* An index is names plus fixed records; anything past this on a real archive
 * means a corrupt length field, and refusing early keeps us from trying to
 * allocate a nonsense amount before we find that out. */
#define INDEX_MAX   (1ull << 32)

static int EXITCODE = 0;

/* ------------------------------------------------------------------ XXH64
 * Segment and index integrity.  Not a cryptographic hash and not used as one:
 * its job is to catch the storage-decay and transfer-truncation cases, where
 * it is both far faster than SHA-256 and entirely sufficient.  Member content
 * gets SHA-256 as well, below. */
#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull

static inline uint64_t rol64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t xrd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint32_t xrd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t xround(uint64_t acc, uint64_t v) {
    return rol64(acc + v * P2, 31) * P1;
}
static inline uint64_t xmerge(uint64_t acc, uint64_t v) {
    return (acc ^ xround(0, v)) * P1 + P4;
}

static uint64_t xxh64(const void *in, size_t len, uint64_t seed) {
    const uint8_t *p = (const uint8_t *)in, *e = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t v1 = seed + P1 + P2, v2 = seed + P2, v3 = seed, v4 = seed - P1;
        const uint8_t *lim = e - 32;
        do {
            v1 = xround(v1, xrd64(p)); p += 8;
            v2 = xround(v2, xrd64(p)); p += 8;
            v3 = xround(v3, xrd64(p)); p += 8;
            v4 = xround(v4, xrd64(p)); p += 8;
        } while (p <= lim);
        h = rol64(v1, 1) + rol64(v2, 7) + rol64(v3, 12) + rol64(v4, 18);
        h = xmerge(h, v1); h = xmerge(h, v2);
        h = xmerge(h, v3); h = xmerge(h, v4);
    } else {
        h = seed + P5;
    }
    h += (uint64_t)len;
    while (p + 8 <= e) { h = rol64(h ^ xround(0, xrd64(p)), 27) * P1 + P4; p += 8; }
    if (p + 4 <= e) { h = rol64(h ^ ((uint64_t)xrd32(p) * P1), 23) * P2 + P3; p += 4; }
    while (p < e) { h = rol64(h ^ (*p++ * P5), 11) * P1; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

/* ---------------------------------------------------------------- SHA-256
 * Stored per member.  XXH64 already covers integrity, so this is here for a
 * different reason: shops that archive things keep SHA-256 manifests, and an
 * archive you cannot reconcile against the manifest you already have is an
 * archive you have to fully restore to audit. */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } Sha;

static const uint32_t SHA_K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static inline uint32_t ror32(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }

static void sha_block(Sha *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t a = w[i-15], b = w[i-2];
        uint32_t s0 = ror32(a,7) ^ ror32(a,18) ^ (a >> 3);
        uint32_t s1 = ror32(b,17) ^ ror32(b,19) ^ (b >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3];
    uint32_t e=s->h[4],f=s->h[5],g=s->h[6],hh=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e,6) ^ ror32(e,11) ^ ror32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = ror32(a,2) ^ ror32(a,13) ^ ror32(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=hh;
}

static void sha_init(Sha *s) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, iv, sizeof iv); s->len = 0; s->n = 0;
}

static void sha_update(Sha *s, const void *in, size_t k) {
    const uint8_t *p = (const uint8_t *)in;
    s->len += k;
    if (s->n) {
        size_t take = 64 - s->n < k ? 64 - s->n : k;
        memcpy(s->buf + s->n, p, take); s->n += take; p += take; k -= take;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
    while (k >= 64) { sha_block(s, p); p += 64; k -= 64; }
    if (k) { memcpy(s->buf, p, k); s->n = k; }
}

static void sha_final(Sha *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) sha_update(s, &z, 1);
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(bits >> (56 - i * 8));
    sha_update(s, b, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(s->h[i] >> 24); out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);  out[i*4+3] = (uint8_t)(s->h[i]);
    }
}

static void hexout(char *dst, const uint8_t *h, int n) {
    static const char *X = "0123456789abcdef";
    for (int i = 0; i < n; i++) { dst[i*2] = X[h[i] >> 4]; dst[i*2+1] = X[h[i] & 15]; }
    dst[n*2] = 0;
}

/* -------------------------------------------------------- bounded reading
 * Every field the decoder parses comes from a file that may have decayed on
 * disk or been truncated in transit.  v1 read them straight into malloc and
 * pointer arithmetic; the whole point of this reader is that a corrupt length
 * sets a flag instead of walking off the buffer. */
typedef struct { const uint8_t *p; uint64_t n, at; int bad; } Rd;

static int rd_need(Rd *r, uint64_t k) {
    if (r->bad || k > r->n || r->at > r->n - k) { r->bad = 1; return 0; }
    return 1;
}
static uint8_t rd8(Rd *r) { if (!rd_need(r, 1)) return 0; return r->p[r->at++]; }
static uint16_t rd16(Rd *r) {
    if (!rd_need(r, 2)) return 0;
    uint16_t v; memcpy(&v, r->p + r->at, 2); r->at += 2; return v;
}
static uint32_t rd32(Rd *r) {
    if (!rd_need(r, 4)) return 0;
    uint32_t v; memcpy(&v, r->p + r->at, 4); r->at += 4; return v;
}
static uint64_t rd64(Rd *r) {
    if (!rd_need(r, 8)) return 0;
    uint64_t v; memcpy(&v, r->p + r->at, 8); r->at += 8; return v;
}
static const uint8_t *rdbuf(Rd *r, uint64_t k) {
    if (!rd_need(r, k)) return NULL;
    const uint8_t *q = r->p + r->at; r->at += k; return q;
}

/* ------------------------------------------------------------- writing out */
typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void bgrow(Buf *b, size_t need) {
    if (b->n + need <= b->cap) return;
    size_t c = b->cap ? b->cap * 2 : 65536;
    while (c < b->n + need) c *= 2;
    uint8_t *q = realloc(b->p, c);
    if (!q) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
    b->p = q; b->cap = c;
}
static void bput(Buf *b, const void *s, size_t k) {
    if (!k) return;
    bgrow(b, k); memcpy(b->p + b->n, s, k); b->n += k;
}
static void b8(Buf *b, uint8_t v)   { bput(b, &v, 1); }
static void b16(Buf *b, uint16_t v) { bput(b, &v, 2); }
static void b32(Buf *b, uint32_t v) { bput(b, &v, 4); }
static void b64(Buf *b, uint64_t v) { bput(b, &v, 8); }
static void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* fwrite's return was unchecked throughout v1, so a full disk produced a
 * truncated archive and a success message. */
static void wr(FILE *f, const void *p, size_t k, const char *what) {
    if (k && fwrite(p, 1, k, f) != k) {
        fprintf(stderr, "gleipnir: write failed (%s): %s\n", what, strerror(errno));
        exit(1);
    }
}

/* =================================================================== segment
 *
 * One segment is one independently modelled unit.  The blob below is v1's
 * container body with the outer framing removed and the chunk table dropped:
 * a segment *is* a chunk now, so the two concepts that used to be separate --
 * "the thing a thread compresses" and "the thing that fails independently" --
 * became the same thing.  That is what makes damage containment and
 * parallelism fall out of one mechanism instead of two.
 *
 *   u8   kind                 0 stored verbatim, 1 modelled
 *   u8   bps, u8 nsym, sym[]  alphabet packing
 *   u64  wn                   working length after packing / deflate expansion
 *   u32  nd, Dfl[nd]          recovered DEFLATE streams
 *   u16  stride, u8 width     detected record geometry
 *   u32  nb, (u8,u32)[nb]     block segmentation
 *   u64  alen, u64 slen       arithmetic and stored-block stream lengths
 *   bytes                     the two streams
 */
#define SEG_STORED 0
#define SEG_MODEL  1

/* Filters run in place, so a segment that ends up stored has to be put back
 * the way it came.  Mirrors the loop v1 ran before its raw-store fallback. */
static void unfilter(uint8_t *base, const Blk *blk, int nb) {
    uint8_t *cp = base;
    size_t bofs = 0;
    for (int b = 0; b < nb; b++) {
        if (BKIND(blk[b].type) == B_X86) e8e9(cp, blk[b].len, 1);
        else if (BKIND(blk[b].type) == B_ALPHA)
            alpha_swap(cp, blk[b].len, (int)((BALGN(blk[b].type) - bofs) & 3));
        cp += blk[b].len; bofs += blk[b].len;
    }
}

/* How much a single worker's model will cost.  Used to clamp -t before the
 * allocator finds out the hard way: at -9 a model is well over a gigabyte, so
 * -t0 on a twelve-thread machine asks for more memory than the machine has.
 * v1 never had to care because it only ever built one model per process. */
static uint64_t model_bytes(void) {
    uint64_t b = 0;
    for (int i = 0; i < NCTX; i++)
        if (ISACT[i]) b += ((uint64_t)1 << GBITS[i]) * sizeof(Group);
    b += (uint64_t)WSETS_ACT * (uint64_t)MIXW * sizeof(int16_t);
    b += ((uint64_t)MMASK + 1) * sizeof(int32_t);
    b += 65536 * sizeof(uint32_t) + 256 * sizeof(uint32_t) + 256 * sizeof(int32_t);
    const int an[6] = { 256, 65536, 256 * 8, 256 << A46B, 256 * 128, 256 << A46B };
    for (int k = 0; k < NAPM && k < 6; k++) b += (uint64_t)an[k] * 33 * sizeof(uint16_t);
    b += sizeof(Ctx);
    return b;
}

static uint64_t ram_total(void) {
#ifdef _WIN32
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) return (uint64_t)ms.ullTotalPhys;
#else
    long p = sysconf(_SC_PHYS_PAGES), z = sysconf(_SC_PAGESIZE);
    if (p > 0 && z > 0) return (uint64_t)p * (uint64_t)z;
#endif
    return 0;
}

/* ------------------------------------------------------------ write side
 *
 * A segment is compressed in three phases so that the middle one can run on
 * every segment of a batch at once: prepare (alphabet packing, DEFLATE
 * recovery, block segmentation setup), model, then serialise.  Only the
 * middle phase is expensive, and it is the only one that is thread safe --
 * the other two touch shared globals and the output stream.
 */
typedef struct {
    uint8_t *raw;              /* segment bytes, owned by the caller */
    size_t   rawlen;
    uint64_t hash;
    uint8_t  sym[16];
    int      nsym, bps, nd;
    Dfl     *fl;
    uint8_t *w;   size_t wn;   /* the buffer the model actually sees */
    uint8_t *owned;            /* w, when it is not an alias of raw */
    Ctx     *cx;
    Job     *j;
} SegW;

static void segw_prep(SegW *s, Job *j) {
    s->j = j;
    memset(j, 0, sizeof *j);
    s->fl = NULL; s->owned = NULL; s->nsym = 0; s->bps = 0; s->nd = 0;
    s->w = s->raw; s->wn = s->rawlen;
    if (!s->rawlen) return;

    if (scan_alphabet(s->raw, s->rawlen, s->sym, &s->nsym, &s->bps)) {
        s->owned = malloc(s->rawlen / (8 / s->bps) + 8);
        if (!s->owned) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
        s->w = s->owned;
        s->wn = pack_syms(s->raw, s->rawlen, s->w, s->sym, s->nsym, s->bps);
    } else { s->bps = 0; s->nsym = 0; }

    s->fl = malloc(sizeof(Dfl) * MAXDFL);
    if (!s->fl) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
    if (!s->bps) {
        size_t xn = 0;
        uint8_t *x = dfl_expand(s->w, s->wn, s->fl, &s->nd, &xn);
        if (x && s->nd > 0) { s->w = x; s->wn = xn; s->owned = x; }
        else { if (x) free(x); s->nd = 0; }
    }

    s->cx = aalloc(sizeof(Ctx));
    j->cx = s->cx; j->mode = 0; j->in = s->w; j->n = s->wn;
    j->file_is_exe = !s->bps && !s->nd && looks_like_exe(s->w, s->wn);
}

static void segw_emit(SegW *s, Buf *out) {
    Job *j = s->j;
    out->n = 0;
    if (!s->rawlen) { b8(out, SEG_STORED); return; }

    size_t body = j->alen + j->slen;
    size_t hdr  = 1 + 2 + (size_t)s->nsym + 8 + 4 + (size_t)s->nd * 19 + 3 + 4
                + (size_t)j->nb * 5 + 16;

    if (hdr + body >= s->rawlen + 1) {
        /* Not worth modelling.  If w aliases the caller's buffer the worker
         * filtered it in place, so undo before storing the raw bytes -- the
         * stored form has no field to record a filter in. */
        if (s->w == s->raw) unfilter(s->raw, j->blk, j->nb);
        b8(out, SEG_STORED);
        bput(out, s->raw, s->rawlen);
    } else {
        b8(out, SEG_MODEL);
        b8(out, (uint8_t)s->bps); b8(out, (uint8_t)s->nsym);
        if (s->nsym) bput(out, s->sym, (size_t)s->nsym);
        b64(out, s->wn);
        b32(out, (uint32_t)s->nd);
        for (int i = 0; i < s->nd; i++) {
            b64(out, s->fl[i].pos); b32(out, s->fl[i].clen); b32(out, s->fl[i].plen);
            b8(out, (uint8_t)(s->fl[i].wbits & 0xFF));
            b8(out, (uint8_t)(s->fl[i].level | (s->fl[i].strat << 4)));
            b8(out, s->fl[i].mem);
        }
        b16(out, (uint16_t)DET_STRIDE); b8(out, (uint8_t)DET_WIDTH);
        b32(out, (uint32_t)j->nb);
        for (int b = 0; b < j->nb; b++) { b8(out, j->blk[b].type); b32(out, j->blk[b].len); }
        b64(out, j->alen); b64(out, j->slen);
        bput(out, j->aout, j->alen);
        bput(out, j->sout, j->slen);
    }
}

static void segw_free(SegW *s) {
    if (s->j) { free(s->j->aout); free(s->j->sout); free(s->j->blk); }
    if (s->cx) { model_free(s->cx); afree(s->cx); s->cx = NULL; }
    free(s->fl); s->fl = NULL;
    if (s->owned) { free(s->owned); s->owned = NULL; }
}

/* ------------------------------------------------------------- read side
 *
 * Same three phases mirrored.  Parsing is where every bound is checked; by
 * the time a Job reaches run_jobs every length in it has been validated
 * against the blob it came from.
 */
typedef struct {
    uint8_t *cb;   size_t clen;      /* compressed blob, owned */
    uint64_t rawlen, hash;
    uint8_t *out;                    /* rawlen bytes, owned */
    int      kind, bps, nsym, ok;
    uint8_t  sym[16];
    uint64_t wn, alen, slen;
    Dfl     *fl; uint32_t nd;
    Blk     *blk; uint32_t nb;
    const uint8_t *ain, *sin;
    uint16_t stride; uint8_t width;
    Ctx     *cx;
    Job     *j;
} SegR;

static int segr_parse(SegR *s, Job *j) {
    s->j = j; s->fl = NULL; s->blk = NULL; s->cx = NULL; s->ok = 0;
    memset(j, 0, sizeof *j);
    Rd r = { s->cb, s->clen, 0, 0 };
    s->kind = rd8(&r);
    if (r.bad) return -1;
    if (s->kind == SEG_STORED) {
        const uint8_t *p = rdbuf(&r, s->rawlen);
        if (r.bad) return -1;
        memcpy(s->out, p, (size_t)s->rawlen);
        s->stride = 0; s->width = 1;
        return 0;
    }
    if (s->kind != SEG_MODEL) return -1;

    s->bps = rd8(&r); s->nsym = rd8(&r);
    if (s->bps < 0 || s->bps > 8 || s->nsym > 16) return -1;
    if (s->nsym) {
        const uint8_t *sy = rdbuf(&r, (uint64_t)s->nsym);
        if (r.bad) return -1;
        memcpy(s->sym, sy, (size_t)s->nsym);
    }
    s->wn = rd64(&r);
    s->nd = rd32(&r);
    if (r.bad || s->wn > (1ull << 40) || s->nd > MAXDFL) return -1;

    s->fl = malloc(sizeof(Dfl) * (s->nd ? s->nd : 1));
    if (!s->fl) return -1;
    for (uint32_t i = 0; i < s->nd; i++) {
        s->fl[i].pos  = rd64(&r);
        s->fl[i].clen = rd32(&r);
        s->fl[i].plen = rd32(&r);
        s->fl[i].wbits = (int16_t)(int8_t)rd8(&r);
        uint8_t ls = rd8(&r);
        s->fl[i].level = ls & 15; s->fl[i].strat = ls >> 4;
        s->fl[i].mem   = rd8(&r);
        if (s->fl[i].pos > s->wn || s->fl[i].plen > s->wn - s->fl[i].pos) {
            r.bad = 1; break;
        }
    }
    s->stride = rd16(&r);
    s->width  = rd8(&r);
    s->nb     = rd32(&r);
    if (r.bad || s->nb > MAXBLK) return -1;

    s->blk = malloc(sizeof(Blk) * (s->nb ? s->nb : 1));
    if (!s->blk) return -1;
    uint64_t tot = 0;
    for (uint32_t b = 0; b < s->nb; b++) {
        s->blk[b].type = rd8(&r);
        s->blk[b].len  = rd32(&r);
        tot += s->blk[b].len;
    }
    s->alen = rd64(&r); s->slen = rd64(&r);
    /* Block lengths must sum to the working length exactly, or the decode loop
     * and the filter loop disagree about where blocks end. */
    if (r.bad || tot != s->wn) return -1;
    s->ain = rdbuf(&r, s->alen);
    s->sin = rdbuf(&r, s->slen);
    if (r.bad) return -1;

    s->cx = aalloc(sizeof(Ctx));
    j->cx = s->cx; j->mode = 1; j->n = s->wn; j->nb = (int)s->nb; j->blk = s->blk;
    j->ain = s->ain; j->alen = s->alen; j->sin = s->sin;
    return 0;
}

static int segr_finish(SegR *s) {
    if (s->kind == SEG_STORED) return 0;
    uint8_t *w = malloc((size_t)s->wn + 8);
    if (!w) return -1;
    memcpy(w, s->j->plain, (size_t)s->wn);
    unfilter(w, s->blk, (int)s->nb);

    int rc = 0;
    uint8_t *fin = w, *collapsed = NULL;
    if (s->nd) {
        size_t cl = 0;
        collapsed = dfl_collapse(w, s->wn, s->fl, (int)s->nd, &cl);
        if (!collapsed) rc = -1; else fin = collapsed;
    }
    if (!rc) {
        if (s->bps) unpack_syms(fin, s->out, (size_t)s->rawlen, s->sym, s->bps);
        else if (s->wn >= s->rawlen || collapsed) memcpy(s->out, fin, (size_t)s->rawlen);
        else rc = -1;
    }
    free(collapsed); free(w);
    return rc;
}

static void segr_free(SegR *s) {
    if (s->cx) { model_free(s->cx); afree(s->cx); s->cx = NULL; }
    free(s->fl);  s->fl = NULL;
    free(s->blk); s->blk = NULL;
    free(s->cb);  s->cb = NULL;
}

/* ==================================================================== index */
typedef struct {
    char    *name;
    uint64_t size;
    int64_t  mtime;
    uint32_t mode;
    uint8_t  sha[32];
    uint64_t seg0, nseg;
} Memb;

/* hash covers the decoded bytes; chash covers the compressed blob as stored.
 * The second one is what makes routine scrubbing possible: checking it is
 * disk-speed, where checking `hash` means decoding the segment and so runs at
 * the compressor's own 0.3-1.3 MB/s.  An archive nobody can afford to scrub
 * is an archive whose rot you find at restore time. */
typedef struct { uint64_t off, clen, rawlen, hash, chash; } Seg;

/* One recovery block per group of PGROUP segments: the XOR of every compressed
 * blob in the group, zero-padded to the longest.  Any single lost segment in a
 * group is then the XOR of the survivors with the block.
 *
 * XOR rather than Reed-Solomon because the failure this has to survive is the
 * one that actually happens to archived data: a region decays, and the segment
 * checksum tells us exactly which one.  That is an erasure at a known position,
 * which is the case parity already handles optimally.  RS earns its complexity
 * when you must find the errors as well as fix them; here the index already
 * did that. */
typedef struct { uint64_t off, len; } Par;

typedef struct {
    Memb  *m; uint64_t nm, mcap;
    Seg   *s; uint64_t ns, scap;
    Par   *p; uint64_t np, pcap;
    uint64_t pgroup;
} Index;

static void idx_addm(Index *x, Memb v) {
    if (x->nm == x->mcap) {
        x->mcap = x->mcap ? x->mcap * 2 : 64;
        x->m = realloc(x->m, x->mcap * sizeof(Memb));
        if (!x->m) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
    }
    x->m[x->nm++] = v;
}
static void idx_adds(Index *x, Seg v) {
    if (x->ns == x->scap) {
        x->scap = x->scap ? x->scap * 2 : 256;
        x->s = realloc(x->s, x->scap * sizeof(Seg));
        if (!x->s) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
    }
    x->s[x->ns++] = v;
}
static void idx_addp(Index *x, Par v) {
    if (x->np == x->pcap) {
        x->pcap = x->pcap ? x->pcap * 2 : 64;
        x->p = realloc(x->p, x->pcap * sizeof(Par));
        if (!x->p) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
    }
    x->p[x->np++] = v;
}
static void idx_free(Index *x) {
    for (uint64_t i = 0; i < x->nm; i++) free(x->m[i].name);
    free(x->m); free(x->s); free(x->p);
    memset(x, 0, sizeof *x);
}

static int PGROUP = 0;             /* segments per recovery block, 0 = none */

/* Blobs in a group differ in length, so the accumulator grows to the longest
 * one it has seen and every shorter blob is treated as zero-padded.  That is
 * what makes the reconstruction below able to recover a segment whose length
 * it knows from the index. */
static void par_xor(Buf *par, const uint8_t *p, size_t n) {
    if (n > par->n) {
        bgrow(par, n - par->n);
        memset(par->p + par->n, 0, n - par->n);
        par->n = n;
    }
    for (size_t i = 0; i < n; i++) par->p[i] ^= p[i];
}

static void par_flush(FILE *fo, Index *idx, Buf *par, const char *outp) {
    if (!par->n) return;
    Par p;
    p.off = (uint64_t)F_TELL(fo);
    p.len = par->n;
    wr(fo, par->p, par->n, outp);
    idx_addp(idx, p);
    par->n = 0;                    /* par_xor re-zeroes as it grows again */
}

/* ------------------------------------------------------------- path safety
 * An archive is untrusted input.  Names that are absolute, that carry a drive
 * letter, or that contain a ".." component can otherwise make extraction write
 * outside the destination directory entirely. */
static int name_is_safe(const char *s) {
    if (!s || !*s) return 0;
    if (s[0] == '/' || s[0] == '\\') return 0;
    if (s[0] && s[1] == ':') return 0;
    for (const char *p = s; *p; ) {
        const char *q = p;
        while (*q && *q != '/') q++;
        size_t k = (size_t)(q - p);
        if (k == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!*q) break;
        p = q + 1;
    }
    for (const char *p = s; *p; p++) if ((unsigned char)*p < 0x20) return 0;
    return 1;
}

/* First path component that exists but is not a directory, or NULL.  Returned
 * in a static buffer: it is only ever used to build one error message. */
static const char *blocking_component(const char *path) {
    static char t[4096];
    snprintf(t, sizeof t, "%s", path);
    for (char *p = t; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p; *p = 0;
            if (*t) {
                STAT_T st;
                if (STAT_F(t, &st) == 0 && !(st.st_mode & S_IFDIR)) return t;
            }
            *p = c;
        }
    }
    return NULL;
}

static void mkparents(const char *path) {
    char *t = strdup(path);
    if (!t) return;
    for (char *p = t; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p; *p = 0;
            if (*t) {
#ifdef _WIN32
                _mkdir(t);
#else
                mkdir(t, 0777);
#endif
            }
            *p = c;
        }
    }
    free(t);
}

/* -------------------------------------------------------------- input walk */
typedef struct { char **v; size_t n, cap; } Names;

static void nm_add(Names *s, const char *p) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->v = realloc(s->v, s->cap * sizeof(char *));
        if (!s->v) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
    }
    s->v[s->n++] = strdup(p);
}

/* Minimal glob: '*', '?', and literals.  Enough for the exclusion patterns
 * people actually write -- *.tmp, *.iso, node_modules/* -- and short enough
 * to audit at a glance. */
static int globmatch(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') {
        for (const char *t = s;; t++) {
            if (globmatch(pat + 1, t)) return 1;
            if (!*t) return 0;
        }
    }
    if (*s && (*pat == '?' || *pat == *s)) return globmatch(pat + 1, s + 1);
    return 0;
}

static Names EXCL;

/* Matched against both the full path and the bare filename, so -x '*.log'
 * works without the caller having to know how deep the tree goes. */
static int excluded(const char *path) {
    const char *b = path, *p;
    for (p = path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    for (size_t i = 0; i < EXCL.n; i++)
        if (globmatch(EXCL.v[i], path) || globmatch(EXCL.v[i], b)) return 1;
    return 0;
}

static int is_dir(const char *p) {
    STAT_T st;
    if (STAT_F(p, &st) != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR;
}

static void walk(const char *path, Names *out) {
    if (excluded(path)) return;
    if (!is_dir(path)) { nm_add(out, path); return; }
#ifdef _WIN32
    char pat[NAME_MAX_B];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char sub[NAME_MAX_B];
        snprintf(sub, sizeof sub, "%s/%s", path, fd.cFileName);
        walk(sub, out);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[NAME_MAX_B];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        walk(sub, out);
    }
    closedir(d);
#endif
}

/* Stored names are relative and slash-separated regardless of host, so an
 * archive written on Windows extracts correctly on Linux and back. */
static const char *store_name(const char *full, const char *root) {
    size_t rl = strlen(root);
    const char *s = full;
    if (rl && !strncmp(full, root, rl)) {
        s = full + rl;
        while (*s == '/' || *s == '\\') s++;
        if (!*s) {                       /* the root itself is a plain file */
            const char *b = strrchr(full, '/');
            const char *c = strrchr(full, '\\');
            if (c > b) b = c;
            s = b ? b + 1 : full;
        }
    }
    return s;
}

/* Sorting stored names to find collisions, keeping the original index so the
 * warning can name the two real files involved. */
typedef struct { const char *name; size_t idx; } NameRef;

static int nameref_cmp(const void *a, const void *b) {
    return strcmp(((const NameRef *)a)->name, ((const NameRef *)b)->name);
}

/* Strips the last path component, so an input path can contribute its own
 * name to the stored names rather than being stripped away entirely. */
static void parent_of(const char *p, char *out, size_t cap) {
    snprintf(out, cap, "%s", p);
    size_t n = strlen(out);
    while (n && (out[n-1] == '/' || out[n-1] == '\\')) out[--n] = 0;
    while (n && out[n-1] != '/' && out[n-1] != '\\')   out[--n] = 0;
    while (n && (out[n-1] == '/' || out[n-1] == '\\')) out[--n] = 0;
}

/* ================================================================== header */
static void put_header(FILE *f, int lvl, uint64_t nmemb, uint64_t idxoff,
                       uint64_t rawtotal, uint32_t segmax) {
    uint8_t h[HDR_BYTES];
    memset(h, 0, sizeof h);
    uint32_t magic = GEN_MAGIC; memcpy(h + 0, &magic, 4);
    uint16_t ver = GEN_VERSION; memcpy(h + 4, &ver, 2);
    uint16_t flags = 0;         memcpy(h + 6, &flags, 2);
    h[8] = (uint8_t)lvl;
    h[9] = (uint8_t)(MEMSHIFT + 16);
    memcpy(h + 12, &segmax, 4);
    memcpy(h + 16, &nmemb, 8);
    memcpy(h + 24, &idxoff, 8);
    memcpy(h + 32, &rawtotal, 8);
    uint64_t hh = xxh64(h, 40, 0);
    memcpy(h + 40, &hh, 8);
    wr(f, h, HDR_BYTES, "header");
}

typedef struct {
    int      lvl, memshift;
    uint64_t nmemb, idxoff, rawtotal;
    uint32_t segmax;
} Head;

static int parse_header(const uint8_t *h, Head *o) {
    uint32_t magic; memcpy(&magic, h + 0, 4);
    if (magic != GEN_MAGIC) { fprintf(stderr, "gleipnir: not a Gleipnir archive\n"); return -1; }
    uint16_t ver; memcpy(&ver, h + 4, 2);
    if (ver != GEN_VERSION) {
        fprintf(stderr, "gleipnir: archive format v%u, this build reads v%u\n",
                ver, GEN_VERSION);
        return -1;
    }
    uint64_t want; memcpy(&want, h + 40, 8);
    if (xxh64(h, 40, 0) != want) {
        fprintf(stderr, "gleipnir: archive header is corrupt\n");
        return -1;
    }
    o->lvl = h[8]; o->memshift = (int)h[9] - 16;
    memcpy(&o->segmax, h + 12, 4);
    memcpy(&o->nmemb, h + 16, 8);
    memcpy(&o->idxoff, h + 24, 8);
    memcpy(&o->rawtotal, h + 32, 8);
    return 0;
}

/* ================================================================ compress */
static int SEGMAX = SEG_MAX_DEF;
static int VERBOSE = 1;
#define PROGRESS_MIN (32u << 20)   /* below this a run is short enough to wait out */

static uint64_t fsize64(FILE *f) {
    if (F_SEEK(f, 0, SEEK_END) != 0) return 0;
    long long v = F_TELL(f);
    F_SEEK(f, 0, SEEK_SET);
    return v < 0 ? 0 : (uint64_t)v;
}

/* A cold-storage run is measured in hours.  Silence for that long is
 * indistinguishable from a hang, and the first thing anyone does about a
 * suspected hang is kill it. */
static void progress(const char *what, uint64_t done, uint64_t total, double sec) {
    if (!VERBOSE || total < PROGRESS_MIN) return;
    double rate = sec > 0 ? done / 1e6 / sec : 0.0;
    double eta  = rate > 0 ? (double)(total - done) / 1e6 / rate : 0.0;
    fprintf(stderr, "\r  %s %5.1f%%  %llu/%llu MB  %.2f MB/s  eta %.0fh%02.0fm    ",
            what, total ? done * 100.0 / total : 0.0,
            (unsigned long long)(done >> 20), (unsigned long long)(total >> 20),
            rate, floor(eta / 3600), fmod(floor(eta / 60), 60));
    fflush(stderr);
}
static void progress_done(uint64_t total) {
    if (VERBOSE && total >= PROGRESS_MIN) fprintf(stderr, "\r%78s\r", "");
}

/* Clamps -t to what the machine can actually hold.  Each worker owns a full
 * private model, so the cost of a thread at -9 is over a gigabyte; -t0 on a
 * twelve-thread box would otherwise ask for ~18 GB and die in the allocator
 * with no indication that the thread count was the problem. */
static int threads_for(uint64_t segbytes) {
    int n = NTHREAD;
    if (n <= 1) return 1;
    uint64_t ram = ram_total();
    if (!ram) return n;
    uint64_t per = model_bytes() + segbytes * 2;
    uint64_t budget = ram / 4 * 3;
    if (!per) return n;
    int fit = (int)(budget / per);
    if (fit < 1) fit = 1;
    if (fit < n) {
        if (VERBOSE)
            fprintf(stderr, "gleipnir: -t%d needs ~%llu MB, capping at -t%d "
                            "(%llu MB installed)\n",
                    n, (unsigned long long)((per * n) >> 20), fit,
                    (unsigned long long)(ram >> 20));
        n = fit;
    }
    return n;
}

static int do_compress(char **paths, int npath, const char *outp, int lvl) {
    Names files = {0};
    size_t *bound = malloc(((size_t)npath + 1) * sizeof *bound);
    if (!bound) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
    bound[0] = 0;
    for (int i = 0; i < npath; i++) { walk(paths[i], &files); bound[i + 1] = files.n; }
    if (!files.n) { fprintf(stderr, "gleipnir: nothing to compress\n"); free(bound); return 1; }

    /* Stored names are resolved here, once, because each input path has its own
     * root.  With a single input the archive is rooted at it, so
     * /data/tree/logs/a.log stores as logs/a.log.  With several, stripping each
     * input entirely would collide -- /a/x and /b/x would both become x -- so
     * the parent is stripped instead and the input keeps its own name:
     * /data/tree stores as tree/logs/a.log.
     *
     * Getting this wrong is not cosmetic.  The previous version passed an empty
     * root whenever there was more than one input, which stored absolute paths;
     * extraction then correctly refused every one of them as unsafe, so the
     * archive was writable but not readable. */
    char **snames = calloc(files.n, sizeof *snames);
    if (!snames) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
    for (int i = 0; i < npath; i++) {
        char root[NAME_MAX_B];
        if (npath == 1) snprintf(root, sizeof root, "%s", paths[0]);
        else            parent_of(paths[i], root, sizeof root);
        for (size_t j = bound[i]; j < bound[i + 1]; j++)
            snames[j] = strdup(store_name(files.v[j], root));
    }
    free(bound);

    /* Exclusions match the name the file will carry *inside* the archive, since
     * that is the name the caller is thinking in: -x 'logs/*' means this
     * archive's logs directory, not a path that happens to be spelled that way
     * on this machine.  The walk already pruned whole directories by basename,
     * which is the cheap half; this is the half that needed the root first. */
    if (EXCL.n) {
        size_t w = 0;
        for (size_t i = 0; i < files.n; i++) {
            if (excluded(snames[i])) { free(files.v[i]); free(snames[i]); continue; }
            files.v[w] = files.v[i]; snames[w] = snames[i]; w++;
        }
        files.n = w;
    }
    if (!files.n) { fprintf(stderr, "gleipnir: nothing to compress\n"); return 1; }

    /* Two inputs can resolve to the same stored name -- `gen c a.gen /x/data
     * /y/data` gives both members the name "data" -- and extraction would then
     * silently overwrite the first with the second.  Warn rather than refuse,
     * since the archive is still well formed and the caller may not care, but
     * do not let it pass unmentioned. */
    {
        NameRef *nr = malloc(files.n * sizeof *nr);
        if (!nr) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
        for (size_t i = 0; i < files.n; i++) { nr[i].name = snames[i]; nr[i].idx = i; }
        qsort(nr, files.n, sizeof *nr, nameref_cmp);
        for (size_t i = 1; i < files.n; i++)
            if (!strcmp(nr[i].name, nr[i-1].name)) {
                fprintf(stderr, "gleipnir: warning: %s and %s both store as '%s'; "
                                "extraction will keep only the last\n",
                        files.v[nr[i-1].idx], files.v[nr[i].idx], nr[i].name);
                EXITCODE = 1;
            }
        free(nr);
    }

    /* Pre-pass: hash every input so identical files are compressed once and
     * the rest become index entries pointing at the same segments.  Reading a
     * file to hash it runs at disk speed; compressing it runs at under a
     * megabyte a second.  The pass therefore costs a fraction of a percent of
     * the run and can remove whole files from it, which on the redundant
     * directory trees that get archived is not a small effect. */
    uint64_t grand = 0;
    uint8_t (*fsha)[32] = calloc(files.n ? files.n : 1, 32);
    uint64_t *fsz  = calloc(files.n ? files.n : 1, sizeof(uint64_t));
    long *dup_of   = malloc((files.n ? files.n : 1) * sizeof(long));
    long *memb_of  = malloc((files.n ? files.n : 1) * sizeof(long));
    if (!fsha || !fsz || !dup_of || !memb_of) {
        fprintf(stderr, "gleipnir: out of memory\n"); exit(1);
    }
    for (size_t i = 0; i < files.n; i++) { dup_of[i] = -1; memb_of[i] = -1; }
    {
        uint8_t *rb = malloc(1 << 20);
        if (!rb) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
        for (size_t i = 0; i < files.n; i++) {
            FILE *f = fopen(files.v[i], "rb");
            if (!f) continue;
            Sha s; sha_init(&s);
            size_t k;
            while ((k = fread(rb, 1, 1 << 20, f)) > 0) { sha_update(&s, rb, k); fsz[i] += k; }
            sha_final(&s, fsha[i]);
            fclose(f);
            grand += fsz[i];
        }
        free(rb);
        /* Open addressing on the first eight bytes of the digest; a collision
         * there still has to survive a full 32-byte compare below. */
        size_t cap = 16; while (cap < files.n * 2 + 2) cap <<= 1;
        long *tab = malloc(cap * sizeof(long));
        for (size_t i = 0; i < cap; i++) tab[i] = -1;
        for (size_t i = 0; i < files.n; i++) {
            uint64_t k64; memcpy(&k64, fsha[i], 8);
            size_t h = (size_t)(k64 & (cap - 1));
            for (;;) {
                if (tab[h] < 0) { tab[h] = (long)i; break; }
                size_t j = (size_t)tab[h];
                if (fsz[j] == fsz[i] && !memcmp(fsha[j], fsha[i], 32)) {
                    dup_of[i] = (long)j; break;
                }
                h = (h + 1) & (cap - 1);
            }
        }
        free(tab);
    }

    FILE *fo = fopen(outp, "wb");
    if (!fo) { fprintf(stderr, "gleipnir: %s: %s\n", outp, strerror(errno)); return 1; }
    put_header(fo, lvl, 0, 0, 0, (uint32_t)SEGMAX);

    Index idx = {0};
    uint64_t rawtotal = 0;
    clock_t t0 = clock();
    time_t  w0 = time(NULL);
    Buf blob = {0}, par = {0};
    idx.pgroup = (uint64_t)PGROUP;

    uint64_t ndup = 0, dupbytes = 0;
    for (size_t fi = 0; fi < files.n; fi++) {
        const char *full = files.v[fi];

        /* Already archived byte for byte under another name: emit an index
         * entry pointing at the same segments and skip the work entirely. */
        if (dup_of[fi] >= 0 && memb_of[dup_of[fi]] >= 0) {
            Memb *o = &idx.m[memb_of[dup_of[fi]]];
            STAT_T ds; memset(&ds, 0, sizeof ds);
            STAT_F(full, &ds);
            Memb m; memset(&m, 0, sizeof m);
            m.name  = strdup(snames[fi]);
            m.size  = o->size;
            m.mtime = (int64_t)ds.st_mtime;
            m.mode  = (uint32_t)ds.st_mode;
            memcpy(m.sha, o->sha, 32);
            m.seg0 = o->seg0; m.nseg = o->nseg;
            memb_of[fi] = (long)idx.nm;
            idx_addm(&idx, m);
            rawtotal += m.size;
            ndup++; dupbytes += m.size;
            continue;
        }

        FILE *in = fopen(full, "rb");
        if (!in) {
            fprintf(stderr, "gleipnir: %s: %s\n", full, strerror(errno));
            EXITCODE = 1; continue;
        }
        STAT_T st; memset(&st, 0, sizeof st);
        STAT_F(full, &st);
        uint64_t sz = fsize64(in);

        /* Segments fill the requested thread count on small inputs and are
         * capped on large ones.  A single-threaded run over a file that fits
         * therefore still models it in one piece, which is what keeps ratio
         * identical to the pre-archive builds. */
        uint64_t seg = sz;
        if (NTHREAD > 1) seg = (sz + (uint64_t)NTHREAD - 1) / (uint64_t)NTHREAD;
        if (seg > (uint64_t)SEGMAX) seg = SEGMAX;
        if (seg < MINCHUNK) seg = MINCHUNK;

        Memb m; memset(&m, 0, sizeof m);
        m.name  = strdup(snames[fi]);
        m.size  = sz;
        m.mtime = (int64_t)st.st_mtime;
        m.mode  = (uint32_t)st.st_mode;
        m.seg0  = idx.ns;

        Sha sh; sha_init(&sh);
        uint64_t done = 0;
        int nthr = 1, first = 1;
        SegW *sw = NULL; Job *jb = NULL;

        while (done < sz) {
            /* Record geometry is a property of the file, not of where a
             * segment boundary landed, so it is detected once from the head of
             * the file and reused.  It also has to stay fixed across a batch,
             * because set_level reads it out of a global. */
            if (first) {
                size_t probe = (size_t)(sz < seg ? sz : seg);
                uint8_t *pb = malloc(probe + 8);
                if (!pb) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
                if (fread(pb, 1, probe, in) != probe) {
                    fprintf(stderr, "gleipnir: %s: short read\n", full);
                    free(pb); EXITCODE = 1; break;
                }
                F_SEEK(in, 0, SEEK_SET);
                detect_period(pb, probe);
                set_level(lvl);
                free(pb);
                nthr = threads_for(seg);
                sw = calloc((size_t)nthr, sizeof(SegW));
                jb = calloc((size_t)nthr, sizeof(Job));
                if (!sw || !jb) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
                first = 0;
            }

            int nb = 0;
            for (; nb < nthr && done < sz; nb++) {
                size_t k = (size_t)(sz - done < seg ? sz - done : seg);
                sw[nb].raw = malloc(k + 8);
                if (!sw[nb].raw) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
                if (fread(sw[nb].raw, 1, k, in) != k) {
                    fprintf(stderr, "gleipnir: %s: short read\n", full);
                    free(sw[nb].raw); EXITCODE = 1; break;
                }
                sw[nb].rawlen = k;
                sw[nb].hash = xxh64(sw[nb].raw, k, 0);
                sha_update(&sh, sw[nb].raw, k);
                segw_prep(&sw[nb], &jb[nb]);
                done += k;
            }
            if (!nb) break;

            run_jobs(jb, nb);

            for (int i = 0; i < nb; i++) {
                segw_emit(&sw[i], &blob);
                Seg s;
                s.off = (uint64_t)F_TELL(fo);
                s.clen = blob.n; s.rawlen = sw[i].rawlen; s.hash = sw[i].hash;
                s.chash = xxh64(blob.p, blob.n, 0);
                wr(fo, blob.p, blob.n, outp);
                idx_adds(&idx, s);
                if (PGROUP) {
                    par_xor(&par, blob.p, blob.n);
                    if (idx.ns % (uint64_t)PGROUP == 0)
                        par_flush(fo, &idx, &par, outp);
                }
                segw_free(&sw[i]);
                free(sw[i].raw); sw[i].raw = NULL;
            }
            progress("compressing", rawtotal + done, grand,
                     (double)(time(NULL) - w0));
        }
        free(sw); free(jb);
        sha_final(&sh, m.sha);
        m.nseg = idx.ns - m.seg0;
        memb_of[fi] = (long)idx.nm;
        idx_addm(&idx, m);
        rawtotal += done;
        fclose(in);
    }
    /* A trailing partial group still gets a recovery block; otherwise the last
     * few segments of every archive would be the unprotected ones. */
    if (PGROUP) par_flush(fo, &idx, &par, outp);
    bfree(&blob); bfree(&par);
    progress_done(grand);

    /* ---- index ---- */
    uint64_t idxoff = (uint64_t)F_TELL(fo);
    Buf ix = {0};
    b64(&ix, idx.nm);
    for (uint64_t i = 0; i < idx.nm; i++) {
        Memb *m = &idx.m[i];
        uint16_t nl = (uint16_t)strlen(m->name);
        b16(&ix, nl); bput(&ix, m->name, nl);
        b64(&ix, m->size); b64(&ix, (uint64_t)m->mtime); b32(&ix, m->mode);
        bput(&ix, m->sha, 32);
        b64(&ix, m->seg0); b64(&ix, m->nseg);
    }
    b64(&ix, idx.ns);
    for (uint64_t i = 0; i < idx.ns; i++) {
        b64(&ix, idx.s[i].off);    b64(&ix, idx.s[i].clen);
        b64(&ix, idx.s[i].rawlen); b64(&ix, idx.s[i].hash);
        b64(&ix, idx.s[i].chash);
    }
    b64(&ix, idx.pgroup);
    b64(&ix, idx.np);
    for (uint64_t i = 0; i < idx.np; i++) {
        b64(&ix, idx.p[i].off); b64(&ix, idx.p[i].len);
    }
    uint64_t ixh = xxh64(ix.p, ix.n, 0);
    wr(fo, ix.p, ix.n, "index");
    uint8_t trl[TRL_BYTES];
    memcpy(trl + 0, &ixh, 8);
    uint64_t ixn = ix.n; memcpy(trl + 8, &ixn, 8);
    uint32_t magic = GEN_MAGIC; memcpy(trl + 16, &magic, 4);
    wr(fo, trl, TRL_BYTES, "trailer");

    /* One seek back, purely to fill in what could not be known up front.  If
     * it fails the archive is still readable: the trailer is the other, and
     * primary, route to the index. */
    if (F_SEEK(fo, 0, SEEK_SET) == 0)
        put_header(fo, lvl, idx.nm, idxoff, rawtotal, (uint32_t)SEGMAX);
    F_SEEK(fo, 0, SEEK_END);
    uint64_t outsz = (uint64_t)F_TELL(fo);
    if (fclose(fo)) { fprintf(stderr, "gleipnir: %s: close failed\n", outp); return 1; }
    bfree(&ix);

    double sec = (double)(time(NULL) - w0);
    if (sec < 1) sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (VERBOSE)
        fprintf(stderr,
            "gleipnir -%s: %llu file%s, %llu -> %llu  %.3f bpc  %.1fs  %.2f MB/s"
            "  %.0f MB peak  [%llu segment%s]\n",
            lvlname(lvl), (unsigned long long)idx.nm, idx.nm == 1 ? "" : "s",
            (unsigned long long)rawtotal, (unsigned long long)outsz,
            rawtotal ? outsz * 8.0 / rawtotal : 0.0, sec,
            sec > 0 ? rawtotal / 1e6 / sec : 0.0,
            peak_rss() / 1048576.0, (unsigned long long)idx.ns,
            idx.ns == 1 ? "" : "s");
    if (VERBOSE && ndup)
        fprintf(stderr, "gleipnir: %llu duplicate file%s stored once, %llu MB "
                        "not recompressed\n",
                (unsigned long long)ndup, ndup == 1 ? "" : "s",
                (unsigned long long)(dupbytes >> 20));
    idx_free(&idx);
    free(fsha); free(fsz); free(dup_of); free(memb_of);
    for (size_t i = 0; i < files.n; i++) { free(files.v[i]); free(snames[i]); }
    free(snames);
    free(files.v);
    return EXITCODE;
}

/* ============================================================ open + index */
typedef struct {
    FILE    *f;
    Head     h;
    Index    x;
    uint64_t fsz;
} Archive;

static int arc_open(const char *path, Archive *a) {
    memset(a, 0, sizeof *a);
    a->f = fopen(path, "rb");
    if (!a->f) { fprintf(stderr, "gleipnir: %s: %s\n", path, strerror(errno)); return -1; }
    a->fsz = fsize64(a->f);
    if (a->fsz < HDR_BYTES + TRL_BYTES) {
        fprintf(stderr, "gleipnir: %s: too short to be an archive\n", path);
        return -1;
    }
    uint8_t hb[HDR_BYTES];
    if (fread(hb, 1, HDR_BYTES, a->f) != HDR_BYTES) return -1;
    if (parse_header(hb, &a->h) != 0) return -1;

    /* The trailer is the authority on where the index is; the header's copy is
     * a cross-check.  That ordering is deliberate -- the trailer is written
     * once and never revisited, while the header is the one field in the file
     * that gets rewritten, so it is the likelier of the two to be torn by a
     * crash mid-write. */
    uint8_t trl[TRL_BYTES];
    if (F_SEEK(a->f, (long long)(a->fsz - TRL_BYTES), SEEK_SET) != 0) return -1;
    if (fread(trl, 1, TRL_BYTES, a->f) != TRL_BYTES) return -1;
    uint32_t tmagic; memcpy(&tmagic, trl + 16, 4);
    uint64_t ixh, ixn;
    memcpy(&ixh, trl + 0, 8);
    memcpy(&ixn, trl + 8, 8);
    if (tmagic != GEN_MAGIC || ixn > INDEX_MAX ||
        ixn > a->fsz - HDR_BYTES - TRL_BYTES) {
        fprintf(stderr, "gleipnir: %s: archive index is unreadable "
                        "(truncated or corrupt)\n", path);
        return -1;
    }
    uint64_t ixoff = a->fsz - TRL_BYTES - ixn;
    if (a->h.idxoff && a->h.idxoff != ixoff)
        fprintf(stderr, "gleipnir: warning: header and trailer disagree on index "
                        "position; trusting the trailer\n");

    uint8_t *ib = malloc((size_t)ixn + 1);
    if (!ib) { fprintf(stderr, "gleipnir: out of memory\n"); return -1; }
    if (F_SEEK(a->f, (long long)ixoff, SEEK_SET) != 0 ||
        fread(ib, 1, (size_t)ixn, a->f) != ixn) { free(ib); return -1; }
    if (xxh64(ib, (size_t)ixn, 0) != ixh) {
        fprintf(stderr, "gleipnir: %s: archive index is corrupt\n", path);
        free(ib); return -1;
    }

    Rd r = { ib, ixn, 0, 0 };
    uint64_t nm = rd64(&r);
    if (r.bad || nm > (1u << 24)) { free(ib); goto bad; }
    for (uint64_t i = 0; i < nm; i++) {
        Memb m; memset(&m, 0, sizeof m);
        uint16_t nl = rd16(&r);
        const uint8_t *nb = rdbuf(&r, nl);
        if (r.bad) break;
        m.name = malloc((size_t)nl + 1);
        if (!m.name) { free(ib); goto bad; }
        memcpy(m.name, nb, nl); m.name[nl] = 0;
        m.size  = rd64(&r);
        m.mtime = (int64_t)rd64(&r);
        m.mode  = rd32(&r);
        const uint8_t *sh = rdbuf(&r, 32);
        if (!r.bad) memcpy(m.sha, sh, 32);
        m.seg0 = rd64(&r); m.nseg = rd64(&r);
        if (r.bad) { free(m.name); break; }
        idx_addm(&a->x, m);
    }
    uint64_t ns = rd64(&r);
    if (r.bad || ns > (1u << 28)) { free(ib); goto bad; }
    for (uint64_t i = 0; i < ns; i++) {
        Seg s;
        s.off = rd64(&r); s.clen = rd64(&r);
        s.rawlen = rd64(&r); s.hash = rd64(&r); s.chash = rd64(&r);
        if (r.bad) break;
        /* A segment must lie entirely inside the payload region.  Without this
         * a corrupt offset would seek anywhere in the file and decode noise. */
        if (s.off < HDR_BYTES || s.clen > ixoff || s.off > ixoff - s.clen ||
            s.rawlen > (1ull << 40)) { r.bad = 1; break; }
        idx_adds(&a->x, s);
    }
    /* Recovery records are optional and were added after the segment table, so
     * an index that simply ends here is a well-formed archive without them. */
    if (!r.bad && r.at < r.n) {
        a->x.pgroup = rd64(&r);
        uint64_t np = rd64(&r);
        if (!r.bad && np <= (1u << 28)) {
            for (uint64_t i = 0; i < np; i++) {
                Par p;
                p.off = rd64(&r); p.len = rd64(&r);
                if (r.bad) break;
                if (p.off < HDR_BYTES || p.len > ixoff || p.off > ixoff - p.len) {
                    r.bad = 1; break;
                }
                idx_addp(&a->x, p);
            }
        }
    }
    if (r.bad) { free(ib); goto bad; }
    for (uint64_t i = 0; i < a->x.nm; i++) {
        Memb *m = &a->x.m[i];
        if (m->seg0 > a->x.ns || m->nseg > a->x.ns - m->seg0) { free(ib); goto bad; }
    }
    free(ib);
    MEMSHIFT = a->h.memshift;
    return 0;
bad:
    fprintf(stderr, "gleipnir: %s: archive index is corrupt\n", path);
    return -1;
}

static void arc_close(Archive *a) {
    if (a->f) fclose(a->f);
    idx_free(&a->x);
}

/* Rebuilds segment si's compressed blob as the XOR of its group's recovery
 * block with every other blob in the group.  Returns a buffer of the segment's
 * recorded length, or NULL if there is no recovery data or a second segment in
 * the same group is also unreadable.
 *
 * Nothing here has to trust the result: a reconstruction built from a group
 * that had two failures produces noise, and that noise then fails the same
 * segment checksum that sent us here. */
static uint8_t *seg_recover(Archive *a, uint64_t si) {
    if (!a->x.pgroup || !a->x.np || si >= a->x.ns) return NULL;
    uint64_t g = si / a->x.pgroup;
    if (g >= a->x.np) return NULL;
    Par *P = &a->x.p[g];
    uint64_t lo = g * a->x.pgroup, hi = lo + a->x.pgroup;
    if (hi > a->x.ns) hi = a->x.ns;
    if (a->x.s[si].clen > P->len) return NULL;

    uint8_t *acc = calloc((size_t)P->len + 1, 1);
    uint8_t *tmp = malloc((size_t)P->len + 1);
    if (!acc || !tmp) { free(acc); free(tmp); return NULL; }
    if (F_SEEK(a->f, (long long)P->off, SEEK_SET) != 0 ||
        fread(acc, 1, (size_t)P->len, a->f) != P->len) {
        free(acc); free(tmp); return NULL;
    }
    for (uint64_t k = lo; k < hi; k++) {
        if (k == si) continue;
        Seg *s = &a->x.s[k];
        if (s->clen > P->len) { free(acc); free(tmp); return NULL; }
        if (F_SEEK(a->f, (long long)s->off, SEEK_SET) != 0 ||
            fread(tmp, 1, (size_t)s->clen, a->f) != s->clen) {
            free(acc); free(tmp); return NULL;
        }
        for (uint64_t i = 0; i < s->clen; i++) acc[i] ^= tmp[i];
    }
    free(tmp);
    uint8_t *out = malloc((size_t)a->x.s[si].clen + 1);
    if (!out) { free(acc); return NULL; }
    memcpy(out, acc, (size_t)a->x.s[si].clen);
    free(acc);
    return out;
}

/* Full serial decode of one recovered segment, checksum included.  Returns 0
 * only if the reconstruction round-trips to the hash the index recorded. */
static int try_recover(Archive *a, uint64_t si, uint8_t *out, int lvl) {
    uint8_t *cb = seg_recover(a, si);
    if (!cb) return -1;
    Seg *s = &a->x.s[si];
    SegR sr; memset(&sr, 0, sizeof sr);
    Job j;
    sr.cb = cb; sr.clen = (size_t)s->clen;
    sr.rawlen = s->rawlen; sr.hash = s->hash; sr.out = out;
    int rc = -1;
    if (segr_parse(&sr, &j) == 0) {
        if (sr.kind == SEG_MODEL) {
            DET_STRIDE = sr.stride;
            DET_WIDTH  = sr.width ? sr.width : 1;
            set_level(lvl);
            run_jobs(&j, 1);
        }
        if (segr_finish(&sr) == 0 &&
            xxh64(out, (size_t)s->rawlen, 0) == s->hash) rc = 0;
    }
    segr_free(&sr);
    return rc;
}

/* Restores the recorded modification time.  An archive that silently resets
 * every timestamp to the restore date destroys the one piece of metadata a
 * long-term archive is most often asked about. */
static void set_mtime(const char *path, int64_t mt) {
    struct utimbuf tb;
    tb.actime = (time_t)mt; tb.modtime = (time_t)mt;
    utime(path, &tb);
}

/* Permission bits are recorded on every platform so that a Windows-written
 * archive still restores sensibly on POSIX.  Re-applying them is POSIX-only:
 * on Windows the only bit st_mode really carries is read-only, and setting it
 * would leave extracted files that the user cannot delete without a separate
 * step -- a worse outcome than not restoring a bit that meant little to begin
 * with. */
static void set_perms(const char *path, uint32_t mode) {
#ifndef _WIN32
    if (mode & 07777) chmod(path, (mode_t)(mode & 07777));
#else
    (void)path; (void)mode;
#endif
}

/* ========================================================= extract / test
 *
 * Segments are decoded a batch at a time so -t is real on the read side too.
 * A batch is capped both by the thread count and by the requirement that
 * every member of it shares the same detected record geometry, since
 * set_level reads that out of a global.  Our own writer always satisfies
 * that within a member, but a batch is split rather than trusted.
 */
static int run_members(Archive *a, const char *destdir, int test_only,
                       char **want, int nwant) {
    int rc = 0;
    uint64_t nbad = 0, nok = 0, bytes = 0, grand = 0, nrec = 0;
    time_t w0 = time(NULL);

    for (uint64_t i = 0; i < a->x.nm; i++) grand += a->x.m[i].size;

    int nthr = NTHREAD > 1 ? NTHREAD : 1;
    SegR *sr = calloc((size_t)nthr, sizeof(SegR));
    Job  *jb = calloc((size_t)nthr, sizeof(Job));
    Job  *rj = calloc((size_t)nthr, sizeof(Job));
    int  *map = calloc((size_t)nthr, sizeof(int));
    int  *hand = calloc((size_t)nthr, sizeof(int));
    if (!sr || !jb || !rj || !map || !hand) {
        fprintf(stderr, "gleipnir: out of memory\n"); exit(1);
    }

    for (uint64_t i = 0; i < a->x.nm; i++) {
        Memb *m = &a->x.m[i];
        if (nwant) {
            int hit = 0;
            for (int k = 0; k < nwant; k++) if (!strcmp(want[k], m->name)) hit = 1;
            if (!hit) continue;
        }
        if (!name_is_safe(m->name)) {
            fprintf(stderr, "gleipnir: refusing unsafe member name: %s\n", m->name);
            rc = 2; nbad++; continue;
        }
        FILE *fo = NULL;
        char path[NAME_MAX_B];
        path[0] = 0;
        if (!test_only) {
            if (destdir && *destdir)
                snprintf(path, sizeof path, "%s/%s", destdir, m->name);
            else
                snprintf(path, sizeof path, "%s", m->name);
            mkparents(path);
            fo = fopen(path, "wb");
            if (!fo) {
                /* When a path component exists but is a file rather than a
                 * directory, mkparents cannot create the directory and fopen
                 * reports ENOENT -- "No such file or directory", about a path
                 * whose parent plainly does exist.  Say what is actually
                 * wrong, because the obvious reading sends people looking in
                 * the wrong place entirely. */
                const char *blocker = blocking_component(path);
                if (blocker)
                    fprintf(stderr, "gleipnir: %s: cannot create, because '%s' "
                                    "already exists and is a file, not a "
                                    "directory\n", path, blocker);
                else
                    fprintf(stderr, "gleipnir: %s: %s\n", path, strerror(errno));
                rc = 1; nbad++; continue;
            }
        }
        Sha sh; sha_init(&sh);
        int member_ok = 1;
        uint64_t got = 0, k = 0;

        while (k < m->nseg) {
            int nb = 0;
            /* ---- fill the batch: read and parse ---- */
            for (; nb < nthr && k < m->nseg; nb++, k++) {
                Seg *s = &a->x.s[m->seg0 + k];
                memset(&sr[nb], 0, sizeof sr[nb]);
                sr[nb].rawlen = s->rawlen;
                sr[nb].hash   = s->hash;
                sr[nb].clen   = (size_t)s->clen;
                sr[nb].out    = malloc((size_t)s->rawlen + 8);
                sr[nb].cb     = malloc((size_t)s->clen + 1);
                if (!sr[nb].out || !sr[nb].cb) {
                    fprintf(stderr, "gleipnir: out of memory\n"); exit(1);
                }
                if (F_SEEK(a->f, (long long)s->off, SEEK_SET) != 0 ||
                    fread(sr[nb].cb, 1, (size_t)s->clen, a->f) != s->clen) {
                    sr[nb].ok = -1;
                } else {
                    sr[nb].ok = segr_parse(&sr[nb], &jb[nb]) == 0 ? 1 : -1;
                }
            }

            /* ---- model, grouped by record geometry ---- */
            for (int t = 0; t < nb; t++) hand[t] = 0;
            for (;;) {
                int seed = -1;
                for (int t = 0; t < nb; t++)
                    if (!hand[t] && sr[t].ok == 1 && sr[t].kind == SEG_MODEL) { seed = t; break; }
                if (seed < 0) break;
                DET_STRIDE = sr[seed].stride;
                DET_WIDTH  = sr[seed].width ? sr[seed].width : 1;
                set_level(a->h.lvl);
                int nrun = 0;
                for (int t = seed; t < nb; t++) {
                    if (hand[t] || sr[t].ok != 1 || sr[t].kind != SEG_MODEL) continue;
                    if (sr[t].stride != sr[seed].stride || sr[t].width != sr[seed].width)
                        continue;
                    map[nrun] = t; rj[nrun] = jb[t]; nrun++;
                    hand[t] = 1;
                }
                if (!nrun) break;
                run_jobs(rj, nrun);
                for (int t = 0; t < nrun; t++) jb[map[t]] = rj[t];
            }

            /* ---- verify and write, in order ---- */
            for (int t = 0; t < nb; t++) {
                Seg *s = &a->x.s[m->seg0 + (k - nb) + t];
                const char *why = NULL;
                if (sr[t].ok != 1) why = "malformed segment";
                else if (segr_finish(&sr[t]) != 0) why = "malformed segment";
                else if (xxh64(sr[t].out, (size_t)sr[t].rawlen, 0) != s->hash)
                    why = "checksum mismatch";

                if (why && try_recover(a, m->seg0 + (k - nb) + t,
                                       sr[t].out, a->h.lvl) == 0) {
                    if (VERBOSE)
                        fprintf(stderr, "gleipnir: %s: segment %llu was damaged (%s)"
                                        " and has been rebuilt from the recovery"
                                        " record\n",
                                m->name, (unsigned long long)((k - nb) + t + 1), why);
                    nrec++;
                    why = NULL;
                }
                if (why) {
                    fprintf(stderr, "gleipnir: %s: segment %llu of %llu is bad (%s)\n",
                            m->name, (unsigned long long)((k - nb) + t + 1),
                            (unsigned long long)m->nseg, why);
                    member_ok = 0; rc = 2;
                    /* Damage stops at the segment.  Writing a hole of the right
                     * length keeps every later segment at its correct offset, so
                     * one bad segment costs you that segment and nothing else. */
                    if (fo) {
                        memset(sr[t].out, 0, (size_t)sr[t].rawlen);
                        wr(fo, sr[t].out, (size_t)sr[t].rawlen, path);
                    }
                } else {
                    sha_update(&sh, sr[t].out, (size_t)sr[t].rawlen);
                    if (fo) wr(fo, sr[t].out, (size_t)sr[t].rawlen, path);
                }
                got += s->rawlen;
                segr_free(&sr[t]);
                free(sr[t].out); sr[t].out = NULL;
            }
            progress(test_only ? "verifying " : "extracting", bytes + got, grand,
                     (double)(time(NULL) - w0));
        }

        uint8_t dig[32]; sha_final(&sh, dig);
        if (member_ok && got != m->size) {
            fprintf(stderr, "gleipnir: %s: length mismatch (%llu, expected %llu)\n",
                    m->name, (unsigned long long)got, (unsigned long long)m->size);
            member_ok = 0; rc = 2;
        }
        if (member_ok && memcmp(dig, m->sha, 32) != 0) {
            fprintf(stderr, "gleipnir: %s: SHA-256 mismatch\n", m->name);
            member_ok = 0; rc = 2;
        }
        if (fo) {
            fclose(fo);
            if (member_ok) { set_perms(path, m->mode); set_mtime(path, m->mtime); }
        }
        bytes += got;
        if (member_ok) nok++; else nbad++;
        if (VERBOSE && a->x.nm > 1 && grand < PROGRESS_MIN)
            fprintf(stderr, "  %-44s %s\n", m->name, member_ok ? "OK" : "FAILED");
    }
    progress_done(grand);
    free(sr); free(jb); free(rj); free(map); free(hand);

    double sec = (double)(time(NULL) - w0);
    if (sec < 1) sec = 1;
    if (VERBOSE)
        fprintf(stderr, "gleipnir %s: %llu OK, %llu bad, %llu bytes  %.1fs  %.2f MB/s"
                        "  %.0f MB peak\n",
                test_only ? "-t" : "-d", (unsigned long long)nok,
                (unsigned long long)nbad, (unsigned long long)bytes, sec,
                bytes / 1e6 / sec, peak_rss() / 1048576.0);
    if (VERBOSE && nrec)
        fprintf(stderr, "gleipnir: %llu segment%s rebuilt from recovery records\n",
                (unsigned long long)nrec, nrec == 1 ? "" : "s");
    return rc;
}

/* ============================================================ scrub, repair
 *
 * The operational pair.  Scrubbing finds rot while you still have a good copy
 * somewhere else; repair puts the archive back using its recovery records.
 * Both work on the stored blobs and neither decodes anything, so both run at
 * disk speed rather than at the compressor's.  That difference is the whole
 * point: a full decode verify of a terabyte at -5 is nineteen days, which
 * means in practice it never gets run, which means rot is discovered at
 * restore time.
 */
static const char *member_of(Archive *a, uint64_t si) {
    for (uint64_t i = 0; i < a->x.nm; i++) {
        Memb *m = &a->x.m[i];
        if (si >= m->seg0 && si < m->seg0 + m->nseg) return m->name;
    }
    return "?";
}

/* Reads segment si and returns its blob, repairing from parity if the stored
 * copy fails its checksum.  *state is 0 intact, 1 repaired, -1 lost. */
static uint8_t *blob_read(Archive *a, uint64_t si, int *state) {
    Seg *s = &a->x.s[si];
    /* Zeroed, not merely allocated: a truncated archive gives a short read, and
     * repair would otherwise copy whatever happened to be in the heap into the
     * output file. */
    uint8_t *b = calloc((size_t)s->clen + 1, 1);
    if (!b) { fprintf(stderr, "gleipnir: out of memory\n"); exit(1); }
    int good = 0;
    if (F_SEEK(a->f, (long long)s->off, SEEK_SET) == 0 &&
        fread(b, 1, (size_t)s->clen, a->f) == s->clen)
        good = xxh64(b, (size_t)s->clen, 0) == s->chash;
    if (good) { *state = 0; return b; }

    uint8_t *r = seg_recover(a, si);
    if (r && xxh64(r, (size_t)s->clen, 0) == s->chash) {
        free(b); *state = 1; return r;
    }
    free(r);
    *state = -1;
    return b;                       /* damaged, returned so repair can copy it */
}

static int do_scrub(Archive *a, const char *path) {
    uint64_t intact = 0, repaired = 0, lost = 0, bytes = 0;
    time_t w0 = time(NULL);
    for (uint64_t i = 0; i < a->x.ns; i++) {
        int st;
        uint8_t *b = blob_read(a, i, &st);
        free(b);
        bytes += a->x.s[i].clen;
        if (st == 0) intact++;
        else if (st == 1) {
            repaired++;
            if (VERBOSE)
                fprintf(stderr, "gleipnir: %s: segment %llu is damaged but "
                                "recoverable from its recovery record\n",
                        member_of(a, i), (unsigned long long)i);
        } else {
            lost++;
            fprintf(stderr, "gleipnir: %s: segment %llu is damaged and cannot be "
                            "recovered\n", member_of(a, i), (unsigned long long)i);
        }
    }
    double sec = (double)(time(NULL) - w0);
    if (sec < 1) sec = 1;
    if (VERBOSE) {
        fprintf(stderr, "gleipnir scrub: %llu segments intact", (unsigned long long)intact);
        if (repaired) fprintf(stderr, ", %llu recoverable", (unsigned long long)repaired);
        if (lost)     fprintf(stderr, ", %llu LOST", (unsigned long long)lost);
        fprintf(stderr, "  (%llu MB read, %.0f MB/s)\n",
                (unsigned long long)(bytes >> 20), bytes / 1e6 / sec);
        if (repaired && !lost)
            fprintf(stderr, "gleipnir: run 'gleipnir r %s fixed.gl' to write a clean copy\n",
                    "archive");
        if (!a->x.np && VERBOSE)
            fprintf(stderr, "gleipnir: note: this archive has no recovery records; "
                            "damage would be unrepairable\n");
    }
    return lost ? 2 : 0;
}

/* Rewrites the archive with every recoverable segment restored.  Offsets move,
 * so the index and the recovery blocks are rebuilt; the segment payloads and
 * every checksum in them are carried across unchanged, which is what makes the
 * output verifiable against the same digests as the original. */
static int do_repair(Archive *a, const char *outp) {
    FILE *fo = fopen(outp, "wb");
    if (!fo) { fprintf(stderr, "gleipnir: %s: %s\n", outp, strerror(errno)); return 1; }
    put_header(fo, a->h.lvl, 0, 0, 0, a->h.segmax);

    Index nx; memset(&nx, 0, sizeof nx);
    nx.pgroup = a->x.pgroup;
    Buf par = {0};
    uint64_t repaired = 0, lost = 0;

    for (uint64_t i = 0; i < a->x.ns; i++) {
        int st;
        uint8_t *b = blob_read(a, i, &st);
        if (st == 1) repaired++;
        if (st < 0) {
            lost++;
            fprintf(stderr, "gleipnir: %s: segment %llu could not be recovered; "
                            "copying it as-is\n", member_of(a, i),
                    (unsigned long long)i);
        }
        Seg s = a->x.s[i];
        s.off = (uint64_t)F_TELL(fo);
        wr(fo, b, (size_t)s.clen, outp);
        idx_adds(&nx, s);
        if (nx.pgroup) {
            par_xor(&par, b, (size_t)s.clen);
            if (nx.ns % nx.pgroup == 0) par_flush(fo, &nx, &par, outp);
        }
        free(b);
    }
    if (nx.pgroup) par_flush(fo, &nx, &par, outp);
    bfree(&par);

    uint64_t idxoff = (uint64_t)F_TELL(fo);
    Buf ix = {0};
    b64(&ix, a->x.nm);
    uint64_t rawtotal = 0;
    for (uint64_t i = 0; i < a->x.nm; i++) {
        Memb *m = &a->x.m[i];
        uint16_t nl = (uint16_t)strlen(m->name);
        b16(&ix, nl); bput(&ix, m->name, nl);
        b64(&ix, m->size); b64(&ix, (uint64_t)m->mtime); b32(&ix, m->mode);
        bput(&ix, m->sha, 32);
        b64(&ix, m->seg0); b64(&ix, m->nseg);
        rawtotal += m->size;
    }
    b64(&ix, nx.ns);
    for (uint64_t i = 0; i < nx.ns; i++) {
        b64(&ix, nx.s[i].off);    b64(&ix, nx.s[i].clen);
        b64(&ix, nx.s[i].rawlen); b64(&ix, nx.s[i].hash);
        b64(&ix, nx.s[i].chash);
    }
    b64(&ix, nx.pgroup);
    b64(&ix, nx.np);
    for (uint64_t i = 0; i < nx.np; i++) { b64(&ix, nx.p[i].off); b64(&ix, nx.p[i].len); }

    uint64_t ixh = xxh64(ix.p, ix.n, 0);
    wr(fo, ix.p, ix.n, "index");
    uint8_t trl[TRL_BYTES];
    memcpy(trl + 0, &ixh, 8);
    uint64_t ixn = ix.n; memcpy(trl + 8, &ixn, 8);
    uint32_t magic = GEN_MAGIC; memcpy(trl + 16, &magic, 4);
    wr(fo, trl, TRL_BYTES, "trailer");
    if (F_SEEK(fo, 0, SEEK_SET) == 0)
        put_header(fo, a->h.lvl, a->x.nm, idxoff, rawtotal, a->h.segmax);
    if (fclose(fo)) { fprintf(stderr, "gleipnir: %s: close failed\n", outp); return 1; }
    bfree(&ix);
    free(nx.s); free(nx.p);

    if (VERBOSE)
        fprintf(stderr, "gleipnir repair: %llu segment%s rebuilt, %llu unrecoverable"
                        " -> %s\n",
                (unsigned long long)repaired, repaired == 1 ? "" : "s",
                (unsigned long long)lost, outp);
    return lost ? 2 : 0;
}

/* ===================================================================== list */
static int do_list(Archive *a, int longform, int manifest) {
    /* sha256sum-compatible output, so an extracted tree can be checked with
     * the tooling a shop already has rather than with gen itself. */
    if (manifest) {
        for (uint64_t i = 0; i < a->x.nm; i++) {
            char hx[65]; hexout(hx, a->x.m[i].sha, 32);
            printf("%s  %s\n", hx, a->x.m[i].name);
        }
        return 0;
    }
    uint64_t raw = 0, comp = 0;
    for (uint64_t i = 0; i < a->x.nm; i++) raw += a->x.m[i].size;
    for (uint64_t i = 0; i < a->x.ns; i++) comp += a->x.s[i].clen;
    printf("archive format v%u, preset -%s, %llu member%s, %llu segment%s\n",
           GEN_VERSION, lvlname(a->h.lvl),
           (unsigned long long)a->x.nm, a->x.nm == 1 ? "" : "s",
           (unsigned long long)a->x.ns, a->x.ns == 1 ? "" : "s");
    for (uint64_t i = 0; i < a->x.nm; i++) {
        Memb *m = &a->x.m[i];
        uint64_t c = 0;
        for (uint64_t k = 0; k < m->nseg; k++) c += a->x.s[m->seg0 + k].clen;
        char tb[32] = "";
        time_t t = (time_t)m->mtime;
        struct tm *g = localtime(&t);
        if (g) strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", g);
        printf("%14llu %14llu %6.3f  %s  %s\n",
               (unsigned long long)m->size, (unsigned long long)c,
               m->size ? c * 8.0 / m->size : 0.0, tb, m->name);
        if (longform) {
            char hx[65]; hexout(hx, m->sha, 32);
            printf("               sha256 %s\n", hx);
        }
    }
    printf("%14llu %14llu %6.3f  total\n", (unsigned long long)raw,
           (unsigned long long)comp, raw ? comp * 8.0 / raw : 0.0);
    /* The per-member column only counts segment payloads, so without this the
     * total does not add up to the size of the file on disk and the difference
     * looks like a bug rather than the recovery records the caller asked for. */
    if (a->x.np) {
        uint64_t pb = 0;
        for (uint64_t i = 0; i < a->x.np; i++) pb += a->x.p[i].len;
        printf("%29llu  recovery records: %llu block%s, one per %llu segments"
               " (%+.1f%%)\n",
               (unsigned long long)pb, (unsigned long long)a->x.np,
               a->x.np == 1 ? "" : "s", (unsigned long long)a->x.pgroup,
               comp ? pb * 100.0 / comp : 0.0);
    } else {
        printf("%29s  no recovery records\n", "-");
    }
    printf("%29llu  archive file on disk\n", (unsigned long long)a->fsz);
    return 0;
}

/* ===================================================================== main */

/* MinGW's startup code expands wildcards in argv against the current
 * directory, which quietly destroys -x patterns: `-x '*.tmp'` arrived as the
 * name of whatever .tmp file happened to be sitting in the working directory,
 * so the pattern excluded one unrelated file and nothing else.  Quoting at the
 * shell does not help, because the rewriting happens inside this process after
 * the shell is done.  Turning it off makes argv mean what the caller wrote on
 * every platform. */
#ifdef _WIN32
int _CRT_glob = 0;
#endif
/* POSIX shells expand wildcards before the process starts, so there is no
 * equivalent to switch off there -- argv already means what the caller wrote. */

static void usage(void) {
    fprintf(stderr,
      "Gleipnir -- a context-mixing archiver\n"
      "\n"
      "usage: gleipnir c [opts] archive path...   compress files or directories\n"
      "       gleipnir x [opts] archive [dir] [member...]\n"
      "                                      extract (into dir, default .)\n"
      "                                      d and e do the same thing\n"
      "       gleipnir t [opts] archive           check integrity\n"
      "       gleipnir l [opts] archive           list contents\n"
      "       gleipnir r archive out.gl          rebuild damaged segments\n"
      "\n"
      "  -1..-9    preset, -1 fastest .. -9 smallest (default 5)\n"
      "            -1  4 ctx  no SSE      -2  6 ctx  1 SSE\n"
      "            -3  8 ctx  2 SSE       -5 12 ctx  3 SSE\n"
      "            -7 19 ctx  4 SSE       -9 27 ctx  6 SSE (30 on rasters)\n"
      "  -f1/-f2   throughput presets below -1\n"
      "  -mN       resize every context table by 2^N (-m-1 halves them)\n"
      "  -tN       worker threads, -t0 = all cores (default 1)\n"
      "  -sN       segment size in MB (default 64); smaller bounds memory and\n"
      "            damage, larger compresses better\n"
      "  -pN       recovery records: one parity block per N segments, letting\n"
      "            any one damaged segment per group be rebuilt.  -p alone\n"
      "            means 32, about 3%% overhead.  Default off.\n"
      "  -D        with t, decode everything and check SHA-256 as well.\n"
      "            Without it, t checks stored checksums only -- which catches\n"
      "            storage decay, and runs at disk speed instead of at the\n"
      "            compressor's, so it is the one you can afford to run often\n"
      "  -L        with l, also print each member's SHA-256\n"
      "  -M        with l, print a sha256sum-compatible manifest\n"
      "  -x GLOB   exclude paths matching GLOB.  Repeatable.  Matched against\n"
      "            the stored name and the bare filename, so -x \"*.log\" works\n"
      "            whatever directory the file is in\n"
      "  -q        quiet\n"
      "  --version, --help\n"
      "\n"
      "cold storage:  gleipnir c -5 -t0 -p32 archive.gl /data   then verify with\n"
      "               gleipnir t archive.gl  before deleting the source\n"
      "\n"
      "exit: 0 ok, 1 usage or I/O error, 2 archive corrupt or verify failed\n");
}

int main(int argc, char **argv) {
    /* Before the argc<3 check: `gen --version` and `gen --help` are two
     * arguments short of any real command, and answering them with a usage
     * screen and exit 1 is the kind of small rudeness that makes a tool
     * annoying to script around. */
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        printf("Gleipnir %s\n", GLEIPNIR_RELEASE);
        printf("archive format v%d  (this build reads and writes v%d only)\n",
               GEN_VERSION, GEN_VERSION);
        printf("built %s %s\n", __DATE__, __TIME__);
        /* GPL section 5 asks that a copyright notice and a warranty
         * disclaimer be kept intact and shown where the program
         * announces itself.  A binary handed on without LICENSE.md
         * beside it would otherwise carry no notice at all, so the
         * essentials are printed here. */
        printf("\nCopyright (C) 2026 ValisSowilo\n");
        printf("License: GPL-3.0-or-later"
               "  <https://www.gnu.org/licenses/gpl-3.0.html>\n");
        printf("This is free software: you are free to change and"
               " redistribute it.\n");
        printf("There is NO WARRANTY, to the extent permitted by law.\n");
        printf("Source: https://github.com/ValisSowilo\n");
        return 0;
    }
    if (argc >= 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        usage();
        return 0;                       /* asked for, so not an error */
    }
    if (argc < 3) { usage(); return 1; }
    int a = 1;
    char mode = argv[a][0];
    /* x and e extract, as aliases for d.  Every other archiver spells it that
     * way -- and in 7-Zip and WinRAR, `d` means *delete from archive*, which
     * is the one word you do not want a backup tool to be ambiguous about.
     * gen has no delete and never has had, so `gen d` was always harmless;
     * the aliases are so that typing what 7-Zip taught you does the thing you
     * meant.  Note that 7-Zip's `e` flattens paths and this one does not: gen
     * always restores the stored directory structure, so here e and x are the
     * same command. */
    if (!strchr("cdtlrxe", mode) || argv[a][1]) { usage(); return 1; }
    if (mode == 'x' || mode == 'e') mode = 'd';
    a++;
    int lvl = 5, longform = 0, deep = 0, manifest = 0;
    while (a < argc && argv[a][0] == '-' && argv[a][1]) {
        char c = argv[a][1];
        if      (c == 'm') MEMSHIFT = atoi(argv[a] + 2);
        else if (c == 'q') VERBOSE = 0;
        else if (c == 'L') longform = 1;
        else if (c == 'D') deep = 1;
        else if (c == 'M') manifest = 1;
        else if (c == 'x') {
            if (!argv[a][2]) {
                if (a + 1 >= argc) { usage(); return 1; }
                nm_add(&EXCL, argv[++a]);
            } else nm_add(&EXCL, argv[a] + 2);
        }
        else if (c == 'p') {
            PGROUP = argv[a][2] ? atoi(argv[a] + 2) : 32;
            if (PGROUP < 0) PGROUP = 0;
        }
        else if (c == 's') {
            int mb = atoi(argv[a] + 2);
            if (mb < 1) { fprintf(stderr, "gleipnir: -s needs at least 1 MB\n"); return 1; }
            SEGMAX = mb << 20;
        }
        else if (c == 't') {
            NTHREAD = atoi(argv[a] + 2);
            if (NTHREAD <= 0) NTHREAD = cpu_count();
            if (NTHREAD > MAXCHUNK) NTHREAD = MAXCHUNK;
        }
        else if (c == 'f') {
            int f = atoi(argv[a] + 2);
            if (f < 1 || f > 2) { usage(); return 1; }
            lvl = 100 + f;
        }
        else if (c >= '1' && c <= '9' && !argv[a][2]) lvl = c - '0';
        else { fprintf(stderr, "gleipnir: unknown option %s\n", argv[a]); return 1; }
        a++;
    }
    if (a >= argc) { usage(); return 1; }
    const char *arc = argv[a++];

    build_tables(); build_states(); build_dt(); build_ccls();

    if (mode == 'c') {
        if (a >= argc) { fprintf(stderr, "gleipnir: nothing to compress\n"); return 1; }
        return do_compress(argv + a, argc - a, arc, lvl);
    }

    Archive ar;
    if (arc_open(arc, &ar) != 0) return 2;
    int rc;
    if (mode == 'l')      rc = do_list(&ar, longform, manifest);
    else if (mode == 'r') {
        if (a >= argc) {
            fprintf(stderr, "gleipnir: r needs an output archive\n");
            arc_close(&ar); return 1;
        }
        rc = do_repair(&ar, argv[a]);
    }
    else if (mode == 't') rc = deep ? run_members(&ar, NULL, 1, NULL, 0)
                                    : do_scrub(&ar, arc);
    else                  rc = run_members(&ar, a < argc ? argv[a] : ".", 0,
                                           a + 1 < argc ? argv + a + 1 : NULL,
                                           a + 1 < argc ? argc - a - 1 : 0);
    arc_close(&ar);
    return rc;
}
