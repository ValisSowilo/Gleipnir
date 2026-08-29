# Architecture

`gen` is a context-mixing compressor. It predicts the next bit, codes that bit
against the prediction, and learns from the result — 8 times per byte, with no
dictionary, no literals, and no match lengths in the output stream. The whole
output is one arithmetic-coded probability stream.

This document is in six parts:

- **Part I — how it works.** The pipeline end to end, one bit through the
  predictor, the coder, and why decompression costs the same as compression.
  Start here.
- **Part II — what the engine models.** The components that add prediction
  power: period detection, raster models, executable detection, preset staging.
- **Part III — what it declines to model.** Where the engine deliberately does
  less work per byte, and the invariant every one of those shortcuts obeys.
- **Part IV — the archive layer.** Container format, integrity, damage
  containment, recovery, parallelism.
- **Part V — speed.** Where the time actually goes, everything already done,
  and an honest ledger of what is left — including three ideas that do not
  work and why.
- **Part VI — measurement and status.** What is built, what is not, and the
  reproducibility problem that invalidates the headline speed claim.

Parts II–IV give the mechanism, the maths, the code as it ships, and the
numbers that justified it. Every component exists because a measurement said
the old behaviour was wrong.

### Contents

**Part I — How it works**

1. [The pipeline](#1-the-pipeline) — file to segments to bits
2. [Predicting one bit](#2-predicting-one-bit) — the seven stages
3. [The arithmetic coder](#3-the-arithmetic-coder)
4. [Learning from one bit](#4-learning-from-one-bit)
5. [Decompression](#5-decompression) — and why it cannot be made faster

**Part II — What the engine models**

6. [Period detection](#6-period-detection)
7. [Raster models](#7-raster-models-contexts-2830)
8. [Alpha detection gates](#8-alpha-detection-gates)
9. [Per-preset staging](#9-per-preset-staging)

**Part III — What it declines to model**

10. [The active-context list](#10-the-active-context-list)
11. [The match bypass](#11-the-match-bypass)
12. [Side tables nobody reads](#12-side-tables-nobody-reads)
13. [Three gates on work that cannot change the answer](#13-three-gates-on-work-that-cannot-change-the-answer)
14. [The throughput presets](#14-the-throughput-presets)

**Part IV — The archive layer**

15. [What was wrong with the v1 container](#15-what-was-wrong-with-the-v1-container)
16. [The segment](#16-the-segment)
17. [Integrity: two hashes](#17-integrity-two-hashes-deliberately)
18. [Damage containment and recovery](#18-damage-containment-and-recovery)
19. [Memory, threads, and the RAM clamp](#19-memory-threads-and-the-ceiling-that-made--t-honest)
20. [Deduplication and the CLI](#20-deduplication-and-the-cli)

**Part V — Speed**

21. [Where the time goes](#21-where-the-time-goes)
22. [What has already been done](#22-what-has-already-been-done)
23. [The speedup ledger](#23-the-speedup-ledger) — what is left, ranked, with what refutes each

**Part VI — Measurement and status**

24. [What was tried and rejected](#24-what-was-tried-and-rejected)
25. [Measurement methodology](#25-measurement-methodology)
26. [The reproducibility problem](#26-the-reproducibility-problem)
27. [The missing rungs: real `-4`, `-6`, `-8`](#27-the-missing-rungs-real--4--6-and--8)
28. [Status](#28-status-what-is-and-is-not-done)

---

# Part I — How it works

## 1. The pipeline

A file becomes an archive in five stages. Only the last is expensive.

```
file(s)
  │
  ├─ dedup pre-pass          SHA-256 every input; duplicates become
  │                          index entries pointing at the same segments
  │
  ├─ split into segments     64 MB default (-sN).  The unit of compression,
  │                          integrity, parallelism and damage containment
  │
  └─ per segment:
       ├─ alphabet packing   if the segment uses ≤16 distinct byte values,
       │                     pack 2/4 symbols per byte  (scan_alphabet)
       ├─ DEFLATE recovery   find zlib streams, inflate them, record the
       │                     parameters needed to rebuild them  (dfl_expand)
       ├─ period detection   measure record/row geometry once  (§6)
       ├─ block segmentation split into B_TEXT / B_X86 / B_ALPHA / B_STORE
       │                     runs  (segment())
       ├─ filters, in place  E8E9 on x86 blocks, byte-swap on Alpha blocks
       └─ THE BIT LOOP       ← everything above is a rounding error next to this
```

The four transforms before the bit loop exist to make the modelled bytes more
predictable, not to compress them. Alphabet packing turns a DNA-like file from
8 bits per symbol into 2. DEFLATE recovery undoes an upstream compressor so the
model can see the original text rather than its entropy-coded residue — you
cannot model what has already been whitened. E8E9 converts x86 relative call
targets to absolute, so the same call site looks the same everywhere instead of
different at every address.

`B_STORE` blocks are the escape hatch: a run the segmenter judges
incompressible is copied to a second stream verbatim. Its bytes are still fed
through `absorb_byte`, so the match model and the record model can see them,
but they never touch the context tables — noise in the tables costs ratio on
everything after it.

### The bit loop

```c
for each byte:
    if (bypass_gate(TH)) { bypass_enc_byte(TH, b); continue; }   /* §11 */
    if (TH->need_ctx) { rehash(TH); nib_begin(TH, 0, 0); }
    for (int b = 7; b >= 0; b--) {
        int bit = (byte >> b) & 1;
        enc_bit(TH, bit, predict(TH));
        update(TH, bit);
    }
```

That is the entire compressor. `predict` returns a 12-bit probability that the
next bit is 1; `enc_bit` narrows the arithmetic coder's range by that
probability; `update` moves every table that contributed toward the truth.

Note what is *not* in the output: no literal bytes, no match offsets, no
lengths, no Huffman table. The compressed stream is nothing but the residue of
predictions that were wrong. This is why the format has no "decode" step
distinct from the model — and why Part V's ceiling is where it is.

## 2. Predicting one bit

`predict()` is seven stages. Roughly 60% of total runtime is the memory traffic
of stages 1–3.

### Stage 1 — context hashing (once per byte)

`rehash()` computes one 64-bit hash per active context model. A *context* is a
question about the recent past: "what were the last 3 bytes?", "what word are
we in?", "what byte sits directly above this one in the previous row?"

```c
if (o > 0)          /* byte order o: the last o bytes */
    v = (ctx + 0x100*o + mix) * 0x9E3779B97F4A7C15ULL;
else if (o == -1)   /* current word, case-folded */
    v = (TH->word + 1) * 0xC2B2AE3D27D4EB4FULL;
else if (o == -2)   /* sparse: bytes at -2 and -4, skipping -1 and -3 */
    ...
else if (o == -4)   /* record: the byte one row above, plus the column */
    ...
```

At `-9` there are 27 of these (30 when a period was detected). The full list
spans byte orders 1–8, 12 and 16; word, word-pair and word-triple; four sparse
patterns; two record contexts; indirect, line, nest and raster contexts.

Case folding on the word hash is deliberate: left case-sensitive, `The` and
`the` become separate contexts that each learn English from scratch, halving
the evidence behind every word context for no gain.

### Stage 2 — group lookup

Each context hash indexes a table of `Group`s. A group holds the bit histories
for one context value across the four bits of a nibble.

```c
/* Four groups share one 64-byte line, so probing all four costs the same
 * memory traffic as probing one.  That buys a replacement policy: when the
 * context is not present, evict the *least established* group in the line
 * rather than whichever one the index happened to land on. */
static Group *group_find(Group *T, uint32_t base, uint8_t chk);
```

This is where the cache misses live. All the addresses for a nibble are
computed and prefetched up front, then the check fields are validated in a
second pass so the misses overlap:

```c
for (int k = 0; k < n; k++) {                       /* compute + prefetch */
    TH->gbase_[i] = (uint32_t)(h >> 20) & (GMASK[i] & ~(uint32_t)(WAYS-1));
    TH->gck_[i]   = (uint8_t)(h >> 48);
    __builtin_prefetch(&TH->T[i][TH->gbase_[i]], 1, 3);
}
for (int k = 0; k < n; k++)                         /* then validate */
    TH->gp_[i] = group_find(TH->T[i], TH->gbase_[i], TH->gck_[i]);
```

With a 41 MB file against 2²⁰ slots almost every lookup is a collision, so the
eviction policy is not a detail — blindly wiping a context with real history to
make room for a one-off is where an overloaded table bleeds most of its ratio.

### Stage 3 — bit history → probability

Each group slot holds one byte: a *bit history* state, from a 256-state machine
that encodes approximately "how many 0s, how many 1s, and what came last". The
state is not itself a probability; a per-model **StateMap** learns what each
state actually means:

```c
/* StateMap: 1 KB per model, so it never leaves L1.
 * Entry = p(22 bits) << 10 | count. */
static void sm_update(uint32_t *e, int bit, int limit) {
    uint32_t v = *e;
    int n = (int)(v & 1023), p = (int)(v >> 10);
    if (n < limit) v++;
    int d = (int)((((int64_t)((bit ? 4194303 : 0) - p)) * DT[n]) >> 16);
    *e = (uint32_t)((int64_t)v + ((int64_t)d << 10));
}
```

`DT[n] = 65536/(n+2)` makes the adaptation rate fall as evidence accumulates:
fast when a context is new, slow once it is established. That single table is
the whole learning-rate schedule.

The probability is then converted to the **logit domain**:

```
stretch(p) = ln(p / (1-p))          squash = its inverse
```

Everything from here to the coder happens in stretched space, because that is
the domain in which combining predictions is linear.

### Stage 4 — the match model

Separately from the context models, a hash of the last several bytes points at
the most recent place the same context occurred. If the bytes since have agreed,
`mlen` counts how long the match has run and `mptr` points at the predicted
byte.

```c
int b = TH->mlen < 16 ? TH->mlen
      : (TH->mlen < 32 ? 16 : (TH->mlen < 64 ? 17 : (TH->mlen < 400 ? 18 : 19)));
TH->m_idx = ((b << 1) | eb) * 8 + TH->nbits;
m = stretch_t[TH->mpr[TH->m_idx] >> 20];
```

The confidence is *learned*, indexed by match length bucket, predicted bit, and
**bit position within the byte** — because the high bits of a predicted byte
are nearly free while the low ones are where a match actually breaks.

### Stage 5 — the ISSE chain

Indirect Secondary Symbol Estimation. The prediction from the lowest order is
refined by progressively higher orders, each stage a two-weight linear
correction selected by the *bit history* of its context:

```c
int cp = TH->st[ISTART];
for (int k = 0; k < NISSE; k++) {
    int s = TH->gp_[ICHAIN[k]]->s[slot];
    const int32_t *w = &TH->IW[k][s * 2];
    int t = (int)(((int64_t)w[0] * cp + (int64_t)w[1] * 512) >> 16);
    cp = t < -2047 ? -2047 : (t > 2047 ? 2047 : t);
}
```

This is strictly serial — stage *k* cannot start until *k−1* finishes — and
that dependency chain is a real cost. It is paid because the refinement *is*
the mechanism: each stage corrects its predecessor rather than voting
independently.

### Stage 6 — the mixer

Every model's stretched prediction is combined by a dot product with learned
weights:

```c
const int16_t *w = TH->W + (size_t)TH->wsel * (size_t)MIXW;
int sum = 0;
for (int i = 0; i < MIXW; i++) sum += (int)w[i] * TH->st[i];
int d = sum >> 16;
```

The important part is `wsel`. The mixer does not have one weight vector, it has
tens of thousands, selected by the current partial byte, whether a match is
active, whether we are in x86 code, the class of the previous byte, and the
position within an instruction:

```c
TH->wsel = TH->c0 | (TH->mlen > 0 ? 256 : 0) | (TH->blk_x86 ? 512 : 0)
         | ((wc & ((1 << WCLSB) - 1)) << 10)
         | ((ap & ((1 << APBITS) - 1)) << (10 + WCLSB));
```

So "which models to trust" is itself context-dependent — inside x86 code an
opcode byte and a displacement byte want completely different blends. This is a
1 MB table per thread, indexed at random once per bit and again per update,
which is why the `-f` presets shrink it (§14).

The loop is written with constant trip counts so each width unrolls and
vectorises cleanly. The mixer weight update measures at **~0% of runtime** — it
vectorises away entirely. This matters for Part V.

### Stage 7 — SSE / APM

The mixed prediction is refined again by up to six APM stages, each a
2-D interpolated table indexed by (stretched probability, some context):

```c
static int apm_pp(APM *a, int p, int cx) {
    int s = stretch_t[p] + 2048;
    int w = s & 127;
    int i = (s >> 7) + cx * 33;
    a->idx = i + (w >> 6);
    return (a->t[i] * (128 - w) + a->t[i + 1] * w) >> 11;
}
```

Contexts are: the partial byte; the previous byte; **match length**; two bytes
hashed; line position; and the current word. Each stage's output is blended
with its input rather than replacing it (`APMW/4`).

Also strictly serial, and deliberately so — evaluating all six against the
mixer output so their loads issue together was tried and costs **+4.07%** size
on dickens for 10.6% less time, where simply dropping to two serial stages
costs +0.55% for 24% less time. The dependency chain is the price of the
method.

SSE stages are **31% of the bit path** — the single largest identified
component.

## 3. The arithmetic coder

A carryless binary arithmetic coder over a 32-bit range, with 12-bit
probabilities.

```c
static void enc_bit(Ctx *TH, int bit, int p) {
    uint32_t xmid = TH->x1 + (uint32_t)(((uint64_t)(TH->x2 - TH->x1) >> 12) * (uint32_t)p);
    if (bit) TH->x2 = xmid; else TH->x1 = xmid + 1;
    while (((TH->x1 ^ TH->x2) & 0xFF000000u) == 0) {   /* leading byte settled */
        TH->obuf[TH->opos++] = (uint8_t)(TH->x2 >> 24);
        TH->x1 <<= 8; TH->x2 = (TH->x2 << 8) | 255;
    }
}
```

The range `[x1, x2]` is split at `xmid` in proportion to `p`; the bit selects a
half. When the top bytes of both ends agree they can never change again, so
that byte is emitted and the range is shifted left. A bit costs
`-log2(p)` bits of output, so a prediction of 0.99 that is right costs 0.014
bits and one that is wrong costs 6.6 — the asymmetry that makes confident
mistakes expensive and is exactly what the SSE stages exist to temper.

The coder itself is negligible in the profile. Compression is the model.

## 4. Learning from one bit

`update()` walks the same structures backwards, each with its own learning
rule:

```c
const int err = (bit << 12) - TH->pr;      /* prediction error */

/* mixer: gradient step, error × input */
for (int i = 0; i < MIXW; i++) {
    int v = w[i] + ((err * TH->st[i]) >> LRSH);
    w[i] = clamp16(v);
}

/* SSE: only the stages that actually ran */
for (int k = 0; k < TH->napm; k++) apm_up(ap[k], bit, APMR);

/* ISSE: each stage trains on its OWN error, not a backpropagated one */
for (int k = 0; k < TH->nisse; k++) {
    int e = (bit << 12) - squash(TH->ip_out[k]);
    w[0] += (e * TH->ip_in[k]) >> ILR0;
    w[1] += e >> ILR1;
}

/* bit histories and their StateMaps */
for (int k = 0; k < NACT; k++) {
    sm_update(TH->sm_e[i], bit, 255);
    *s = NEX[*s][bit];                     /* state machine transition */
}
```

The ISSE rule is worth noting: each stage is trained to be a better estimate
than the stage below it, which is exactly the job it was given. No
backpropagation through the chain.

At a byte boundary the buffer advances, the word and history registers shift,
the record column advances, `rehash()` recomputes every context hash, and the
match model is updated.

## 5. Decompression

The decoder is the encoder with two lines changed:

```c
/* encode */                          /* decode */
int bit = (byte >> b) & 1;            int bit = dec_bit(TH, predict(TH));
enc_bit(TH, bit, predict(TH));        update(TH, bit);
update(TH, bit);
```

`predict()` and `update()` are the *same functions*, called in the same order,
on state that both sides have driven identically. The decoder recovers each bit
by asking where the coded value falls in the range that `p` splits — and it
needs the same `p`, so it must run the entire model.

**This is why decompression cannot be made faster than compression, and it is
not an implementation limitation.** Every preset decodes within ~4% of its
encode speed. For comparison, `xz` decodes about 20× faster than it encodes and
`brotli` about 334×, because their decoders execute a plan the encoder wrote
down. Here there is no plan — the "plan" is the model's state, which only
exists by having processed every preceding bit.

### The invariant

Everything in Part III depends on one rule:

> **A decision to skip work must be computable by the decoder from state it
> already holds.**

The match bypass reads `mlen`, which both sides track. The SSE gate reads
`stretch(p)` after the mixer, which both sides have computed. The ISSE early
exit reads the chain's own running estimate. Update thinning reads `|err|`,
which by update time the decoder also has, because it has just decoded the bit.

Violate this and the encoder and decoder take different paths, the predictions
diverge, and the archive is silently corrupt from that byte on. Every shortcut
in this document was designed against that constraint first and measured
second.

---

# Part II — What the engine models

This part covers the components that add prediction power: the **period
detector**, the **raster models**, the **Alpha detection gates**, and the
**per-preset staging** of the SSE chain and its tables.

**On the before/after figures below.** Each is an A/B from the session in which
that change landed — same source, one `-D` different — so the *delta* is exact.
The absolute "after" sizes are the sizes as of that change, and a few have since
moved by around 0.1% because the match bypass (§11) landed later and slightly
alters output on files with long matches. Current absolute numbers live in
[README.md](README.md#benchmarks).

**On the identifiers in the code.** Three kinds appear, and the distinction
matters if you intend to change one:

- **Compile-time constants** — `ADIV` (8, §8), `MAXSTRIDE` (8192, §6),
  `IXMIN` (4, §13). `#define`s near the top of the source. `IXMIN` is wrapped
  in `#ifndef` so a sweep can override it with `-DIXMIN=n`; that is how its
  value was chosen.
- **Per-preset globals** — `NAPM`, `A46B` (§9), `THINE` (§13), the bypass gate
  (§11). `static int`, assigned in the preset table from the command-line
  level. `THINE = 0` disables update thinning, which is how the §13 A/B ran.
- **Detected per file, then written into the container** — `DET_STRIDE`,
  `DET_WIDTH` (§6). Set once by the encoder; the decoder reads them from the
  segment header rather than re-deriving them, so the two cannot diverge.
- **Recorded in the archive header** — `MEMSHIFT`, so the decoder allocates the
  same table sizes the encoder used.

The Tier 2 identifiers (`THINE`, `IXMIN`, `ISSEXIT`) live in `gen26.c` and
`genv2.c`, not in `gen.c` — `gen.c` predates them.

## 6. Period detection

### The problem

Three of the 27 context models ask a question of the form *"what was the byte
one record back?"* Answering it requires knowing the record length. Star
catalogues, database rows, raster images and sampled signals all have one;
prose and source code do not.

The original detector voted: each 2-byte context remembered where it last
occurred, and the gap since then was a vote for that gap being the stride. Four
candidate slots, each having to earn its place. It was wrong on every file that
depended on it.

| file | it settled on | truth | failure mode |
|---|---|---|---|
| sao | 56 | 28 | picked the **harmonic** |
| x-ray | 56 | 3800-byte rows, 2-byte samples | missed both |
| mr | 4 / 6, thrashing | 1024 | missed entirely |
| osdb | 998 | no period | invented one |
| ooffice | 78, **18,944 changes** | no period | thrashed |

Three distinct defects:

1. **Harmonics.** Every integer multiple of a true period recurs exactly as
   reliably as the period itself. A "which gap repeats most" statistic cannot
   distinguish 28 from 56, and noise decides.
2. **No way to say "none".** The detector always produced an answer, so files
   with no period spent their whole length feeding two models with noise.
3. **No hysteresis.** Each change of mind reset the column phase, discarding
   accumulated statistics.

### The mechanism

The record contexts do not actually care which gap recurs. They care whether
`d[i-s]` is a good predictor of `d[i]`. **Mean absolute difference measures
exactly that**, so measure it directly:

```
        MAD(s) = (1 / |W|) · Σ  |d[i] − d[i−s]|
                            i∈W
```

`MAD(1)` is the baseline — how well the immediately preceding byte predicts.
A stride is worth using only if it beats that baseline substantially.

### The algorithm

```
1. coarse    MAD(s) for every s in 1..8192, over 2 windows of 32 KB
2. width     w = smallest s ≤ 4 with MAD(s) ≤ 0.60 · MAD(1),  else 1
3. row       R = argmin MAD(s) over s ≥ 8
4. harmonic  walk down: first s ≥ 8 with MAD(s) ≤ 1.03 · MAD(R) replaces R
5. confirm   re-measure R and 1 on 4 windows of 256 KB
6. gate      accept only if MAD(R) ≤ 0.85 · MAD(1),  else report no period
```

Step 4 is the harmonic fix and step 6 is the "none" fix. Step 2 separates the
*element width* from the *row length*, which is what makes 16-bit rasters work.

```c
static void detect_period(const uint8_t *d, size_t n) {
    DET_STRIDE = 0; DET_WIDTH = 1;
    if (n < 4096) return;

    size_t cw = n / 8 < 32768 ? n / 8 : 32768;
    int maxs = (int)(cw / 2);
    if (maxs > MAXSTRIDE) maxs = MAXSTRIDE;

    static double m[MAXSTRIDE + 1];
    for (int s = 1; s <= maxs; s++) m[s] = mad_spread(d, n, s, cw, 2);
    double m1 = m[1];
    if (m1 <= 0.0) return;                    /* constant data */

    int w = 1;                                /* element width */
    for (int c = 2; c <= 4; c++)
        if (m[c] <= m1 * 0.6) { w = c; break; }

    int best = 0; double bv = 1e18;           /* row / record length */
    for (int s = 8; s <= maxs; s++)
        if (m[s] < bv) { bv = m[s]; best = s; }
    if (!best) return;

    for (int s = 8; s < best; s++)            /* walk down to the fundamental */
        if (m[s] <= bv * 1.03) { best = s; bv = m[s]; break; }

    size_t fw = n / 4 < 262144 ? n / 4 : 262144;
    double fb = mad_spread(d, n, best, fw, 4);
    double f1 = mad_spread(d, n, 1, fw, 4);

    if (fb > f1 * 0.85) { DET_WIDTH = w; return; }   /* no usable period */
    DET_STRIDE = best; DET_WIDTH = w;
}
```

### Why measured once, on the encoder

The result is written into the container (2 bytes of stride, 1 of width), so:

- both sides start from the identical answer — no divergence risk;
- the column phase never re-anchors, so statistics accumulate for the whole file;
- the per-byte cost is **negative**: removing the vote removed a random 256 KB
  table access from every byte.

### What it finds

| file | stride | width | MAD(R) vs MAD(1) |
|---|---|---|---|
| x-ray | 3800 | 2 (LE) | 24.3 vs 132.9 |
| mr | 1024 | 2 (BE) | 7.2 vs 60.8 |
| sao | 28 | 1 | 58.8 vs 83.1 |
| osdb | — | — | rejected: nothing beats 0.85·MAD(1) |
| ooffice | — | — | rejected |

sao's 28 is exact: 7,251,944 = 28 × 258,998.

---

## 7. Raster models (contexts 28–30)

Present **only when a period was detected**. On text and code they do not exist
at all — carrying them and zeroing their inputs would cost time for
byte-identical output.

### Geometry

With row stride `R` and element width `w`, at buffer position `L` the
two-dimensional neighbourhood is:

```
        NW      N            NW = buf[L − R − w]
                             N  = buf[L − R]
        W       ?            W  = buf[L − w]
```

`W` steps back one **element**, not one byte. In 16-bit raster data the byte at
−1 is the other half of the current sample and predicts almost nothing; the byte
at −2 is the same plane of the previous sample.

The **plane** is which byte within an element we are coding:

```
        plane = rcol & (w − 1)          w ∈ {1, 2, 4}, so a mask suffices
```

This matters because the high byte of a medical image is smooth while the low
byte is nearly noise. Without the plane in the context they share one set of
statistics and blunt each other.

### Model 28 — MED / LOCO-I predictor

The gradient-adjusted predictor from lossless image coding (JPEG-LS):

```
        p = W + N − NW,  clamped to [min(W,N), max(W,N)]
```

The unclamped term `W + N − NW` is a planar extrapolation: it assumes the local
surface is a plane through the three known neighbours. The clamp is what makes
it robust at edges — if `W` and `N` straddle an edge, the planar estimate
overshoots, and clamping into the range the neighbours actually span converts
the prediction into an edge-following one.

Context is `(p, plane)`, so the model learns *"given the neighbourhood predicts
value p and we are on plane k, what is this bit?"*

```c
int p = Wp + Np - NWp;
int lo = Wp < Np ? Wp : Np, hi = Wp < Np ? Np : Wp;
if (p < lo) p = lo; else if (p > hi) p = hi;
v = (((uint64_t)p << 4) | (uint64_t)plane) * 0x9E3779B97F4A7C15ULL + 0x1D0BULL;
```

### Model 29 — joint neighbourhood

The three neighbours as one 32-bit context, unreduced:

```c
uint64_t s = (uint64_t)Np | ((uint64_t)Wp << 8)
           | ((uint64_t)NWp << 16) | ((uint64_t)plane << 24);
v = (s + 0x3C7ULL) * 0xC2B2AE3D27D4EB4FULL;
```

Where model 28 compresses the neighbourhood into a single predicted value,
this keeps it whole. It is sparser — 2^24 possible contexts against 2^12 — so it
is slower to learn and only pays where the same neighbourhood recurs. It gets a
2^20 table; 28 gets 2^16, because its domain is genuinely smaller.

### Model 30 — local gradient class

Quantised differences rather than values, which generalises across brightness:

```c
static int qgrad(int v) {
    int a = v < 0 ? -v : v;
    int c = a < 2 ? a : (a < 4 ? 2 : (a < 8 ? 3 : (a < 16 ? 4 :
            (a < 32 ? 5 : (a < 64 ? 6 : 7)))));
    return v < 0 ? -c : c;
}
```

Signed, roughly logarithmic, range −7..7. Small differences are what carry
information in smooth data, so they keep individual bins while everything large
collapses. Context is the three gradients plus the plane plus the previous byte:

```c
uint64_t s = (uint64_t)(qgrad(Wp - NWp) + 7)
           | ((uint64_t)(qgrad(Np - NWp) + 7) << 4)
           | ((uint64_t)(qgrad(Wp - Np) + 7) << 8)
           | ((uint64_t)plane << 12)
           | ((uint64_t)(h & 0xFF) << 13);
```

A flat region, a vertical edge and a diagonal ramp each get their own context
regardless of absolute brightness — which is why this generalises where 29
cannot.

### Mixer width

`NIN = NCTX + 5`, so 27 contexts sat exactly on the old 32-lane mixer limit and
adding any context would have silently clamped `MIXW` and dropped inputs.
`NPAD` is now 48 — three whole 32-byte vectors, which also guarantees a widened
tail load cannot read past `st[]`.

### Measured

| file | before | after | |
|---|---|---|---|
| mr | 2,180,051 | 2,029,087 | **−6.93%** |
| x-ray | 3,684,087 | 3,612,587 | **−1.94%** |
| sao | 3,898,724 | 3,856,927 | −1.07% |

x-ray was the only Silesia file behind `zpaq -m5` (+0.4%); it is now −1.6%, and
every file is ahead.

---

## 8. Alpha detection gates

### The original test

DEC Alpha instructions are 4 bytes, and the opcode is the top 6 bits of the last
byte (little-endian). The detector scores each of the four alignments by the
fraction of words whose opcode is architecturally valid, and requires the best
alignment to beat the runner-up:

```
        f(a) = |{ i ≡ a (mod 4) : opcode(d[i+3]) ∈ VALID }| / count(a)

        accept if  f(best) > 0.45  and  f(best) − f(second) > 0.12
```

Absolute validity alone does not work — 0.93 of nci's 4-byte words have a
"valid" opcode, and nci is ASCII. **Alignment asymmetry** is the real signature.

### Failure: near-constant bytes

The test reads only the top six bits of every fourth byte. Any format with a
near-constant byte at one alignment scores ≈1.0 there and low elsewhere — which
is precisely the signature being hunted. An array of monotonically increasing
32-bit integers does exactly this: the high byte barely changes, and if its value
lands in the opcode mask the whole run reads as perfectly aligned code.

Measured on a 4 MB `int32` array: **1,096 KB byte-swapped, 2.67% lost.**

### Gate 1 — opcode diversity

Real instruction streams use many opcodes and are not dominated by one. A
constant field uses exactly one. The thresholds come from measured
distributions, not intuition:

| | distinct opcodes at winning alignment | share held by top opcode |
|---|---|---|
| mozilla (real Alpha) | 0% of windows ≤8, 1% ≤12 | 11% ≥40%, **0% ≥60%** |
| int32 array | **100% ≤4** | **100% ≈100%** |

```c
int distinct = 0, top = 0;
for (int o = 0; o < 64; o++) {
    if (seen[b][o]) distinct++;
    if (seen[b][o] > top) top = seen[b][o];
}
if (distinct < ADIV) return b;                       /* ADIV = 8  */
if (tot[b] && top * 100 > tot[b] * ADOM) return b;   /* ADOM = 60 */
*ok = 1;
```

`ADIV = 8` rejects 100% of the integer array and 0% of mozilla. `ADOM = 60`
likewise. A first attempt at `ADIV = 12, ADOM = 40` looked reasonable and
**regressed mozilla by 0.37%** by rejecting 11% of its genuine Alpha windows —
the thresholds must come from the distributions.

### Gate 2 — never inside a PE or ELF image

After gate 1, a 3.6 MB x86-64 DLL was *still* claiming 136 KB of Alpha, losing
**1.19%**, and stealing three windows from the E8/E9 filter that should have
handled them. Rather than a third threshold, a structural rule:

```c
/* A file that opens with MZ or ELF is a single-architecture image, and that
 * architecture is not DEC Alpha. */
if (!file_is_exe) {
    int aok, aal = alpha_align_of(d, n, base, &aok);
    if (aok) return B_ALPHA | (aal << 2);
}
```

mozilla is deliberately unaffected: it is a tar, has no MZ header, so
`file_is_exe` is false and its 479 genuine Alpha blocks are still found.

### Measured

| | before | after | |
|---|---|---|---|
| `i32.bin` | 761,225 | 740,932 | −2.67% |
| `x64.dll` | 817,679 | 807,923 | −1.19% |
| ooffice | 1,762,975 | 1,749,589 | −0.76% |
| mozilla | 9,757,645 | 9,757,996 | +0.004% (noise) |

Three false positives — integer arrays, float arrays, x86-64 binaries — and
**Silesia contains none of them**. That is the argument for the mixed corpus.

---

## 9. Per-preset staging

Three quantities that were fixed are now part of the preset, because they trade
against each other rather than independently.

![Preset ladder relative to -9: size stays within 100–121% across the whole
ladder while time and memory fall by factors, so the cheap presets give up
little ratio for large savings](graphs/preset_ladder.svg)

The ladder is deliberately non-uniform: size barely moves across it (100–121%
of `-9`) while time and memory move by factors. That asymmetry is what makes a
preset ladder worth having at all — the cheap presets give up very little ratio
for a great deal of time.

### SSE stage count (`NAPM`)

The six APM stages are strictly serial — each corrects its predecessor. Stages
5 and 6 are text-specific (line position, current word). On mr, **four stages
are both smaller and faster than six**:

| | dickens size | dickens time | mr size | mr time |
|---|---|---|---|---|
| 6 stages | *baseline* | *baseline* | *baseline* | *baseline* |
| 4 stages | +0.215% | −12.4% | **−0.202%** | −7.0% |
| 2 stages | +0.546% | −24.0% | +0.004% | −19.7% |
| 0 stages | +0.854% | −31.4% | +0.468% | −26.2% |

### Hash width of a4 and a6 (`A46B`)

At 10 bits the pair is 33 MB and every bit lands on a scattered row, so they
never cache — and the cost grows with file size as they compete with the context
tables for L3:

| hash | pair size | dickens (10 MB) | webster (41 MB) | mr (10 MB) |
|---|---|---|---|---|
| 10 bits | 33.0 MB | *baseline* | *baseline* | *baseline* |
| 8 bits | 8.2 MB | −3.4% time, +0.061% | −0.6%, +0.107% | −1.2%, +0.030% |
| 4 bits | 0.5 MB | −12.3% time, +0.384% | **−21.7%**, +0.498% | −4.1%, +0.102% |

`-9` keeps 10 bits — its job is the smallest output. `-7` takes 6 for −8.6% time
at +0.12% size.

### Allocate only the stages a preset evaluates

Found by disbelieving a null result: shrinking a4/a6 produced *byte-identical*
output at `-5` and `-3`, which is impossible if those stages run. They do not —
`NAPM` is 3 and 2 there — but every stage was allocated regardless. At `-1`,
where `NAPM` is 0, that was **43 MB allocated and never read**, per thread.

```c
apm_init(&TH->a1, NAPM > 0 ? 256         : 0);
apm_init(&TH->a2, NAPM > 1 ? 65536       : 0);
apm_init(&TH->a3, NAPM > 2 ? 256 * 8     : 0);
apm_init(&TH->a4, NAPM > 3 ? 256 << A46B : 0);
apm_init(&TH->a5, NAPM > 4 ? 256 * 128   : 0);
apm_init(&TH->a6, NAPM > 5 ? 256 << A46B : 0);
```

`a5` was separately 2× over-allocated: its context is 7 bits plus `c0`, so it
can never index past 32,768 rows and had 65,536.

Result, with provably identical output at every preset except `-7`. RSS here is
**on dickens (10 MB)**, not the corpus peak — peak RSS is driven by the largest
file, so the corpus figures in the README are several times these:

| preset | RSS before → after | time |
|---|---|---|
| `-1` | 79 → **38 MB** | −12% |
| `-2` | 116 → **75 MB** | −9% |
| `-3` | 184 → **147 MB** | −8% |
| `-5` | 362 → **325 MB** | −9% |
| `-9` | 848 → **846 MB** | −17% |

---

# Part III — What it declines to model

Each of these does *less* work per byte than the full model allows, and
each rests on the invariant in §5: a decision to skip must be computable
by the decoder from state it already holds, or the archive is silently
corrupt.

## 10. The active-context list

### Skipping is not the same as silencing

Contexts −4 and −5 are the record models: *"what was the byte one stride
back?"*. When `detect_period` reports no period they have nothing to say, and
the original code handled that by zeroing their mixer inputs at the end of
`predict`:

```c
if (!TH->stride)
    for (int i = 0; i < NCTX; i++)
        if (ORD[i] == -4 || ORD[i] == -5 ||
            (ORD[i] <= -19 && ORD[i] >= -21)) TH->st[i] = 0;
```

That silences the *output* and pays the entire cost of producing it: a hash in
`rehash`, a prefetch and a 4-way bucket probe in `nib_begin`, a StateMap load
in `predict`, and a StateMap update plus a bit-history transition in `update` —
every byte, for a number that is then thrown away.

The stride is decided once on the encoder and carried in the header, so **both
sides know before `set_level` runs** whether the record contexts can ever fire.
That makes the skip list static, and static is what makes it safe: encoder and
decoder build the identical list, so there is no way for them to disagree.

```c
NACT = 0;
memset(ISACT, 0, sizeof ISACT);
for (int i = 0; i < NCTX; i++)
    if (DET_STRIDE || (ORD[i] != -4 && ORD[i] != -5)) {
        ACT[NACT++] = i;
        ISACT[i] = 1;
    }
```

Every per-byte loop then walks `ACT[]` instead of `0..NCTX`:

```c
for (int k = 0; k < NACT; k++) {
    const int i = ACT[k];
    ...
}
```

### Why the output is unchanged

An inactive context's `st[i]` is zeroed at `model_alloc` and never written
again, so its mixer lane contributes `w·0 = 0` to the dot product, and its
weight update is proportional to that same zero input — the weight never moves.
A lane that is always zero and whose weight never moves is arithmetically
identical to a lane that was silenced after the fact. The output is
byte-identical; what disappears is the work.

`model_alloc` also stops allocating their tables outright:

```c
if (!ISACT[i]) { TH->T[i] = NULL; continue; }  /* never probed */
```

A `NULL` here is safe precisely because every loop that would dereference it is
now driven by `ACT[]`.

**Measured: byte-identical output, −7.4% time on dickens `-9`, −32 MB per
thread.**

---

## 11. The match bypass

### The observation

The match model finds the most recent occurrence of the current order-6 context
and predicts the byte that followed it. When it has been right for hundreds of
consecutive bytes, its prediction is nearly certain — and the engine is still
paying the full model to agree with it. On a byte inside such a run the coder
spends on the order of 0.01 bits, while predicting it costs 27 group probes, 30
StateMap loads, the ISSE chain, the mixer, and up to six serial APM stages.

The cost is almost entirely independent of how confident the answer is. That is
the asymmetry the bypass exploits. paq9a and zpaq's mid methods are the
precedent.

### The mechanism

Inside a run of at least `BYPT` matched bytes, the byte is coded through the
**match StateMap alone** — one L1-resident load per bit — and then absorbed
exactly as a stored byte would be, so the match model, the record phase and the
word state all stay coherent:

```c
static inline int bypass_gate(const Ctx *TH) {
    return BYPT && TH->mlen >= BYPT && (size_t)TH->mptr < TH->buflen;
}
```

The probability comes from the same count-adaptive StateMap the match model
already uses, indexed by (length bucket, predicted bit, bit position):

```c
static inline uint32_t *bypass_entry(Ctx *TH, int pb, int nb, int *eb_out) {
    if (TH->mlen > 0) {
        int eb = (pb >> (7 - nb)) & 1;
        int b  = TH->mlen < 16 ? TH->mlen
               : (TH->mlen < 32 ? 16 : (TH->mlen < 64 ? 17
               : (TH->mlen < 400 ? 18 : 19)));
        *eb_out = eb;
        return &TH->mpr[((b << 1) | eb) * 8 + nb];
    }
    *eb_out = -1;
    return &TH->bsm[TH->c0];
}
```

Three details carry the correctness:

1. **The gate reads only `mlen` and `mptr`**, both of which encoder and decoder
   maintain identically from data both have already seen. The decision is
   therefore always symmetric — neither side has to signal it.
2. **A mid-byte break is handled.** If the predicted bit is wrong the match
   dies (`mlen = 0`) partway through a byte, and the remaining bits are coded
   through `bsm` — a 256-entry StateMap over the partial byte `c0`, trained
   *only* on bypassed bits, so it is warm for exactly this situation and costs
   the modelled path nothing.
3. **Context tables are not updated during a bypassed byte.** That is the
   ratio cost, and it is what the gate sweep is really measuring. Group
   pointers are refreshed lazily on the first modelled byte afterwards, via
   `need_ctx`, rather than eagerly at the end of every run.

Encoder and decoder are written as mirror images and must stay that way — same
entry, same clamp, same updates, same break rule:

```c
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
```

### Why it can be *smaller*, not just faster

A bypass that only traded ratio for speed would be unsurprising. At high gates
it does neither — it wins on both. The reason is that at the top length bucket
the match StateMap has accumulated enormous evidence for one specific
(length, predicted bit, bit position) cell, and **commits harder than the mixer
blend does**. The mixer must hedge: it is combining 27 opinions, most of which
are far less certain than the match, and the logistic blend cannot push as close
to the extremes as a single saturated StateMap can. Inside a long match, hedging
is a cost, not insurance.

### Choosing the gate — the part that was wrong

The gates originally shipped as 400 / 128 / 96 / **48**, with 48 at `-1` and
`-3` chosen from a sweep on samba alone. Swept across six files that disagree
(`byt_sweep.py`), 48 turns out to be actively harmful:

| preset | gate | total size | time | worst file |
|---|---|---|---|---|
| `-1` | 48 | **+1.289%** | −15.2% | nci **+14.28%**, xml +5.63% |
| `-1` | 96 | **−0.263%** | −7.0% | xml +0.23% |
| `-1` | 192 | −0.248% | −4.5% | none above 0 |
| `-3` | 48 | **+2.096%** | −16.4% | nci **+22.40%**, xml +8.55% |
| `-3` | 96 | **−0.142%** | −8.2% | xml +1.22% |
| `-3` | 192 | −0.184% | −5.1% | xml +0.29% |
| `-9` | 192 | −0.137% | −3.9% | xml +0.52% |
| `-9` | 400 | **−0.178%** | — | xml +0.09% |

![Match-bypass gate sweep: the size penalty is flat from 96 upward but rises
steeply below it, with repetitive files such as nci and xml paying 14–22% at
the low gates](graphs/gate_sweep.svg)

Below roughly 96 the bypass starts engaging on *medium*-length matches, where
the full model still contributes a great deal, and highly repetitive files pay
for it enormously — nci is a chemical database of fixed-format lines, and xml is
markup, both almost entirely made of medium matches. At 96 and above the bypass
is a ratio win as well as a speed win at every preset.

The shape of that curve is the whole argument. It is not a monotone trade where
more speed costs more ratio; it is a cliff with a flat, slightly-negative floor
beyond it. A parameter chosen anywhere past the cliff is nearly free, and one
chosen just before it is very expensive — which is exactly the situation in
which sweeping a single file gives a confident wrong answer, because whether a
given file *has* a cliff there depends entirely on its match-length
distribution.

At `-1`, gate 96 **strictly dominates** gate 192 — smaller *and* faster — which
settles that choice without a judgement call. The shipped ladder is now:

```c
BYPT = BYT >= 0 ? BYT
     : (lvl >= 9 ? 400 : (lvl >= 7 ? 128 : 96));
```

The `-9` times for gates 400 and 800 in that sweep are omitted above because
they measured *slower than disabling the bypass entirely* (+21.5%, +28.3%),
which is impossible — a higher gate strictly does less work. Another process had
started on the machine partway through that sweep. The sizes, being
deterministic, were unaffected, and the gate was chosen on those; the impossible
ordering is what revealed the contamination in the first place.

### Measured, same session, old binary vs new

Both changes together, at `-9`, compression time:

| file | size | time |
|---|---|---|
| dickens | −0.063% | −9.0% |
| samba | −0.131% | −14.5% |
| mozilla | −0.126% | −4.6% |
| **total** | **−0.118%** | **−7.5%** |

dickens is prose with few long matches, so it shows the active-context list
almost alone; samba is an archive of repeated source trees, where the bypass
does the work.

---

## 12. Side tables nobody reads

The active-context list (§10) removed work inside the bit loop whose result no
model consumed. The same mistake existed one level up, per *byte*.

`push_word` maintains six pieces of side state on every byte, whatever the
preset. Two of them are not cheap:

```c
uint32_t p1 = (uint32_t)(TH->hist & 0xFF);
uint32_t p2 = (uint32_t)(TH->hist & 0xFFFF);
TH->ind1[p1] = (TH->ind1[p1] << 8) | (uint32_t)byte;
TH->ind2[p2] = (TH->ind2[p2] << 8) | (uint32_t)byte;
```

`ind2` is a 256 KB table written at a data-dependent index once per byte — a
scattered write that will usually miss L2. It exists for exactly one consumer,
context −14 (indirect order 2). Likewise `ind1` serves only −13, `dlast`/`dlog`
only −15, and `nest` only −16.

**Below level 5 none of those contexts is in the model.** `-3`, `-2` and `-1`
carry orders 1–4, word, word-pair and line — and were still paying for two
indirect tables, a recency table and a nesting state machine that nothing would
ever read.

The fix mirrors §5: decide once, in `set_level`, from the same `ORD[]` both
sides build.

```c
NEED_IND = NEED_DLOG = NEED_NEST = 0;
for (int i = 0; i < NCTX; i++) {
    if (ORD[i] == -13 || ORD[i] == -14) NEED_IND  = 1;
    if (ORD[i] == -15)                  NEED_DLOG = 1;
    if (ORD[i] == -16)                  NEED_NEST = 1;
}
```

Each block in `push_word` becomes conditional on its flag. Because a flag is
off only when no context reads the table, the skipped writes cannot change any
prediction: **output is byte-identical at every preset**, verified by SHA-256
against the previous build at `-1`, `-3`, `-5`, `-7` and `-9` on dickens and
samba.

This is worth stating plainly because it is the cheapest class of optimisation
there is and the easiest to miss: the cost was never in a hot loop anyone was
looking at, it was in a helper that had grown consumers and then lost them
again as the preset ladder was carved out beneath it.

---

## 13. Three gates on work that cannot change the answer

§5 and §7 remove work whose result is unread. This section removes work whose
result is *read but immaterial* — refinements applied where the estimate is
already as good as it is going to get.

All three share one safety requirement, and it is the only thing that makes
them legal: **the condition must be computable by the decoder at the moment it
makes the same decision.** The mixer's own output, the ISSE chain's running
value and the coded bit at update time all qualify — the decoder has each of
them, from the identical model, before it needs the decision. The bit being
coded, at predict time, does not. Get that wrong and the archive is corrupt in
a way no amount of testing on compressible files will reveal.

### 13.1 Confidence-gated SSE

The SSE chain is 31% of the bit path and its six stages are strictly serial by
design (§7's rejected parallel variant explains why). But a stage only earns
its latency where the estimate is still in doubt. When the first stages have
already pushed `p` out to 0.99, a3–a6 are polishing hundredths of a bit onto a
prediction that costs almost nothing to code — and each is a scattered load the
next stage waits on.

```c
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
```

The first `SSEK` (2) stages always run; beyond that the gate reads `|stretch(p)|`
against a threshold. `TH->napm` then drives `update`, because a stage that did
not predict still holds the `apm->idx` it used on some *earlier* bit — training
it would move an unrelated entry:

```c
for (int k = 0; k < TH->napm; k++) apm_up(ap[k], bit, APMR);
```

**Measured at `-9`** over dickens, xml, samba, mozilla, osdb and nci:

| gate | size | time | worst file |
|---|---|---|---|
| off | *baseline* | *baseline* | *baseline* |
| 384 | +0.257% | −16.2% | nci +0.78% |
| 512 | +0.185% | −16.4% | nci +0.62% |
| 768 | +0.081% | −15.7% | nci +0.40% |
| **1024** | **+0.014%** | **−14.6%** | nci +0.24% |

Gate 384 is dominated — 512 is both smaller and faster. At 1024 the trade is
essentially free: a seventh of the runtime for a fourteen-thousandth of the
size. For scale, the alternative already on record — dropping to two serial
stages — costs +0.55% for −24%; per unit of time saved the gate is about twenty
times cheaper in ratio.

At `-7` only two stages are gated (`NAPM` is 4), and the win is proportionally
smaller. That sweep also produced the session's one contaminated reading: gate
1024 measured *2.4% slower* than doing strictly more work, which is impossible.
Repeating it A-B-A-B settled it — baseline 343.3s and 342.8s, gate 768 at 318.5s
and 317.8s, so the real figure is **−7.3% for +0.086%** and the anomaly was
background load. A single arm that says something impossible is a measurement,
not a discovery.

### 13.2 ISSE tail gating

Same shape, smaller stakes. Once the chain's running estimate has been driven
to ±2047 — the rail of the stretched domain — the stages after it are
sharpening a number that is already as certain as the representation allows.

```c
if (ISSEXIT && k + 1 >= IXMIN && (cp <= -2047 || cp >= 2047)) {
    nk = k + 1; break;
}
```

The taps the mixer reads from partway along the chain have to follow, or they
would read an `ip_out[]` left over from a previous bit:

```c
int t1 = NISSE / 3, t2 = (NISSE * 2) / 3;
if (t1 >= nk) t1 = nk - 1;
if (t2 >= nk) t2 = nk - 1;
```

A stale tap would in fact still be *symmetric* — both sides would read the same
stale value — so this is a clarity and ratio fix rather than a correctness one.
That distinction is worth keeping straight: symmetry is what makes an archive
valid, and it is a weaker condition than doing the sensible thing.

**Measured at `-9`: +0.030% size for −3.7% time**, worst file nci +0.17%. The
acceptance bar set for this idea in advance was "size cost under 0.05%", and it
clears it. Not swept at other presets, so it is enabled at `-9` only.

### 13.3 Update thinning — the tip that was backwards

The third gate was predicted to buy time: training is proportional to the
error, so a bit the model already had right moves the weights by a rounding
error, yet the loop still runs.

```c
const int ae = err < 0 ? -err : err;
const int train = !THINE || ae >= THINE;
```

Whatever it does for time, **what it does is buy ratio.** Sizes at `-9` over
the six files (deterministic, so comparable across runs):

| threshold | size |
|---|---|
| off | — |
| 4 | −0.636% |
| 8 | −0.690% |
| **16** | **−0.702%** |
| 32 | −0.637% |

Skipping training on bits the model already predicts well makes the output
**0.70% smaller**, with a clear optimum at 16. It transfers down the ladder:
`-5` measures −0.447% at the same threshold. Every one of the six files
improves; the smallest gain is dickens at −0.28%, the largest xml at −0.90%.

The explanation is ordinary regularisation — not chasing near-zero errors stops
the weights being jittered by noise on the overwhelming majority of bits — but
it was not what the tip predicted, and the direction of the *speed* effect is
still not established. Two independent sweeps disagreed about its sign, and the
reason is instructive: the same binary doing identical work measured baselines
of 569.3, 567.5, 560.0 and 495.5 seconds across four runs, and within each run
the arms drift monotonically in the order they ran. A ±4% time delta read off a
single non-interleaved sweep at `-9` is drift, not signal. The honest combined
figure comes from the interleaved whole-corpus A/B, not from here.

By this project's standards 0.70% at `-9` is a large ratio change — the entire
ISSE chain-length retune was 0.03% — and it arrived from a tip filed under
*speed*. The lesson is not that the tip was wrong about the mechanism. It is
that the sweep recorded both axes, so a finding could land on the one the plan
was not looking at.

---

## 14. The throughput presets

`-f1` and `-f2` continue the preset ladder below `-1`. They are levels 101 and
102, carried in the same single header byte as every other preset, so nothing
in the container changed:

| | contexts | ISSE | SSE | mixer sets | match index | bypass gate |
|---|---|---|---|---|---|---|
| `-f1` | 3 (orders 1–3) | 2 | 0 | 8,192 | 2^21 | 64 |
| `-f2` | 5 (orders 1–4, word) | 3 | 1 | 8,192 | 2^22 | 96 |

The one structural addition is a narrower mixer selector. The full selector
spans 65,536 weight sets — at 8 lanes a 1 MB table per thread, indexed at
random once per bit and again per update. Its x86 and instruction-position
dimensions exist for models with contexts able to exploit them, which three
byte orders are not:

```c
TH->wsel = FASTP
    ? (TH->c0 | (TH->mlen > 0 ? 256 : 0) | ((wc & FWCLS) << 9))
    : (TH->c0 | ... | (TH->blk_x86 ? 512 : 0) | ...);
```

### What the design premise got wrong

These presets were specified around the claim that **table size is the speed
lever** — keep the working set in L2 and the dependent-load chain shortens from
~80 ns misses to ~12 ns hits. It was swept, over prose, markup, a source tree
and a binary, across a 16× range of table memory:

| `-m` | total | bpc | time | MB/s |
|---|---|---|---|---|
| −2 | 20,205,223 | 1.829 | 44.5s | 1.99 |
| 0 | 19,704,911 | 1.784 | 45.4s | 1.95 |
| +1 | 19,540,659 | 1.769 | 45.5s | 1.94 |
| **+2** | **19,395,057** | **1.756** | 45.6s | 1.94 |

Time moves 2.5% across the whole range while size moves 4.0%. The reason was
already documented for `-9`: all of a nibble's group addresses are prefetched
together, so the misses overlap into roughly one memory latency rather than one
each — which is exactly why 32× the memory costs only 31% more time there. The
premise was refuted by a measurement the project had already made and had not
applied to itself.

So `-f1` ships tables sized for **ratio** (9.4 MB), not for L2, and its speed
comes from running three contexts with no SSE.

A single-file ablation said the opposite. On dickens alone, shrinking the match
index looked worth −15.3% time, the mixer weights −11.2%, the context tables
−8.5% — each individually a better size-for-time rate than the ladder's own
local slope, which argues for taking all three. Over the four-file set the
combination was **−6.3% time for +7.14% size**, because dickens is 11% of that
set by volume and has the least match coverage in it.

### The bypass gate, for the third time

Specified at 16 for `-f1` and 24 for `-f2`. Swept over the six files:

| gate | total | vs off | time | worst file |
|---|---|---|---|---|
| off | 22,995,538 | — | — | — |
| 16 | 24,418,653 | +6.19% | −35.2% | nci **+24.68%**, xml +16.50% |
| 32 | 23,465,348 | +2.04% | −21.8% | nci +14.78% |
| 64 | 23,035,949 | +0.18% | −10.8% | nci +2.99% |
| 96 | **22,917,028** | **−0.34%** | −7.5% | osdb +0.02% |

This is the gate-48 finding (§6) reproduced exactly, one size down: below ~96
the bypass fires on medium-length matches where the full model still has a
great deal to contribute, and the repetitive files pay enormously. Gate 96 is
again *both* smaller and faster than no bypass at all.

`-f1` ships 64 — the most aggressive gate whose worst file stays under +3%, and
a better size-for-time rate than the rung above it — and `-f2` ships 96.

### What they are worth

The targets were 8–15 MB/s for `-f1` and 3–6 for `-f2`. They land near **1.7–2.0
and 1.1**, against `-1`'s 1.3. The `-f` family is a 1.25–1.5× step below `-1`,
not the 6–10× step the plan assumed, and no amount of table tuning closes that
gap because table size is not what the time is going to. What sets the floor is
the structure the whole design rests on: one dependent load chain per bit,
serially, which is the same reason this class of compressor cannot approach the
LZ family's decode speed at any preset.

---

# Part IV — The archive layer

Everything below lives in `genv2.c` and nothing in it changes a single
predicted bit. That claim was checked, not assumed: v1 and v2 output at `-3`
share a 437,114-byte identical common suffix — the entire compressed stream
after the differing container framing. The model region was touched in exactly
two places, both allocation, neither predictive:

```c
/* 64-byte alignment for the context tables, and a free() that can find the
 * original pointer again.  v1 leaked every table because it never needed to
 * run the model twice in one process; a threaded archiver does. */
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
```

plus `model_free(Ctx *)`, which releases `T[]`, `W`, `a1..a6.t`, `mtab`, `buf`,
`ind1`, `ind2` and `dlast`. v1 allocated the model once and exited; v2 builds
and destroys one per segment per worker, so a leak that was invisible becomes
a hard ceiling.

## 15. What was wrong with the v1 container

The v1 format was not a format. It was whatever `fwrite` happened to emit, and
every one of the following was true of it:

| defect | consequence |
|---|---|
| no magic number | any file at all was "a gen archive" until it crashed |
| no version field | a format change would silently mis-decode old files |
| no checksum anywhere | corruption surfaced as wrong bytes, not as an error |
| `memcpy` into `malloc` sized from the file's own fields | a hostile or damaged archive was an arbitrary write |
| `ftell` / `fseek` (long) | a hard 2 GB ceiling on Windows |
| whole file in memory, twice | 8 GB of RAM to compress a 2 GB file |
| `fwrite` return unchecked | a full disk produced a truncated archive and exit 0 |

The last one is the one worth dwelling on. A backup tool that reports success
while writing a short file is worse than one that crashes, because the failure
is discovered at restore time — which is to say, at the worst possible time.
Every write now goes through:

```c
static void wr(FILE *f, const void *p, size_t k, const char *what) {
    if (k && fwrite(p, 1, k, f) != k) {
        fprintf(stderr, "gen: %s: write failed: %s\n", what, strerror(errno));
        exit(1);
    }
}
```

## 16. The segment

A **segment** is the unit of independent compression. It is also the unit of
integrity, of parallelism, and of damage containment — and that is the central
design decision of the whole layer, because those four properties fall out of
*one* mechanism rather than four.

v1 had two separate concepts: the chunk a thread compressed, and the block
that framed the output. v2 collapses them. A segment *is* a chunk. Default
64 MB, `-sN` to change it:

| type | field | meaning |
|---|---|---|
| `u8` | `kind` | 0 stored verbatim, 1 modelled |
| `u8`, `u8`, `u8[nsym]` | `bps`, `nsym`, `sym[]` | alphabet packing |
| `u64` | `wn` | working length after packing / DEFLATE expansion |
| `u32`, `Dfl[nd]` | `nd`, `fl[]` | recovered DEFLATE streams, 19 bytes each |
| `u16`, `u8` | `stride`, `width` | detected record geometry (§6) |
| `u32`, `(u8,u32)[nb]` | `nb`, `blk[]` | block segmentation: kind + length |
| `u64`, `u64` | `alen`, `slen` | arithmetic and stored-block stream lengths |
| `u8[alen]`, `u8[slen]` | `aout`, `sout` | the two streams |

Every field after `kind` is absent when `kind == 0`; a stored segment is the
tag byte followed by `rawlen` raw bytes, and `rawlen` comes from the index
rather than from the segment.

The consequences of the segment size are all the same trade in different
clothes: **smaller segments bound peak memory, bound blast radius, and
parallelise better; larger segments compress better** because the model keeps
more history. There is no free direction, which is why it is a flag.

### Three phases, because only the middle one is safe

Compression is split so that the expensive phase can run on every segment of a
batch simultaneously:

```c
segw_prep(SegW *, Job *)    /* alphabet packing, DEFLATE recovery, setup */
                            /* -- touches globals, single-threaded       */
run_jobs(...)               /* the model.  the only expensive phase,     */
                            /* and the only thread-safe one              */
segw_emit(SegW *, Buf *)    /* serialise -- touches the output stream    */
segw_free(SegW *)
```

The read side mirrors it exactly: `segr_parse` / `run_jobs` / `segr_finish`.

**This structure is a bug fix, not a design.** The first threaded version sized
segments for the thread count and then ran them sequentially, so `-t8` paid the
full ratio cost of small segments and bought exactly zero speed. Batching the
middle phase across workers is what made `-t` real.

### The stored fallback

If the modelled form is not smaller, the segment is stored verbatim. This needs
care, because the filters run **in place**:

```c
if (hdr + body >= s->rawlen + 1) {
    /* If w aliases the caller's buffer the worker filtered it in place, so
     * undo before storing -- the stored form has no field to record a
     * filter in. */
    if (s->w == s->raw) unfilter(s->raw, j->blk, j->nb);
    b8(out, SEG_STORED);
    bput(out, s->raw, s->rawlen);
}
```

Omitting `unfilter` here yields an archive that stores E8E9-transformed bytes
and returns them as if they were the original. It round-trips within one build
and is wrong.

## 17. Integrity: two hashes, deliberately

Each segment carries **two** checksums, and the second is the operationally
important one:

```c
typedef struct { uint64_t off, clen, rawlen, hash, chash; } Seg;
```

- `hash` — XXH64 of the **decoded** bytes. Proves the decode was correct.
  Checking it costs a full decode: 0.3–1.3 MB/s.
- `chash` — XXH64 of the **compressed blob as stored**. Proves the bytes on
  disk are the bytes that were written. Checking it is a sequential read:
  disk speed.

Plus SHA-256 per member, for interop with `sha256sum` — a check that shares no
code with the thing being checked.

This split is the single most useful feature in the layer. Measured on a real
2.2 GB archive the two verification modes are **1330× apart**:

| mode | what it proves | speed |
|---|---|---|
| `gen t` | the stored bytes have not decayed | disk speed |
| `gen t -D` | the archive decodes to the right bytes | model speed |

At scale that is the difference between a **nineteen-day-per-TB** verification
and a **~45-minute** one. An archive nobody can afford to scrub is an archive
whose rot is discovered at restore time — so the cheap check is what makes
routine scrubbing a policy rather than an aspiration.

## 18. Damage containment and recovery

### Index at the end, trailer as a second route to it

```
[48-byte header][segment payload ...][index][20-byte trailer]
```

The index is written last because its contents are not known until compression
finishes — which keeps compression single-pass and streaming. The trailer at a
fixed offset from EOF is what makes it findable. The header also records
`idxoff`, and the two are cross-checked:

```c
/* The trailer is the authority on where the index is; the header's copy is a
 * cross-check.  That ordering is deliberate -- the trailer is written once and
 * never revisited, while the header is the one field in the file that gets
 * rewritten, so it is the likelier of the two to be torn by a crash. */
if (a->h.idxoff && a->h.idxoff != ixoff)
    fprintf(stderr, "gen: warning: header and trailer disagree on index "
                    "position; trusting the trailer\n");
```

### Every field from the file is hostile

All index parsing goes through a bounds-checked reader:

```c
typedef struct { const uint8_t *p; uint64_t n, at; int bad; } Rd;
```

`rd8/16/32/64/rdbuf` set `bad` and return zero rather than reading past the
end, and `bad` is checked before anything is trusted. Structural invariants
are enforced on top of that — a segment must lie entirely inside the payload
region, or a corrupt offset becomes a seek anywhere in the file followed by
decoding noise:

```c
if (s.off < HDR_BYTES || s.clen > ixoff || s.off > ixoff - s.clen ||
    s.rawlen > (1ull << 40)) { r.bad = 1; break; }
```

Note the subtraction order: `s.off > ixoff - s.clen`, never
`s.off + s.clen > ixoff`, which overflows on exactly the input an attacker
would choose.

Member ranges are validated the same way (`m->seg0 > ns || m->nseg > ns -
m->seg0`), and extraction refuses unsafe names — absolute paths, drive
letters, and any `..` component — because an archive is untrusted input and
zip-slip is the default behaviour of a naive extractor.

### XOR parity, and why not Reed–Solomon

`-pN` writes one recovery block per N segments: the XOR of every compressed
blob in the group, zero-padded to the longest. Any one lost segment in a group
is then the XOR of the survivors with the block.

#### Why XOR rather than Reed–Solomon

The failure this has to survive is the one that actually happens to archived
data: a region decays, and the segment checksum tells us exactly which one.
That is an **erasure at a known position**, which is the case parity already
handles optimally. RS earns its complexity when you must *find* the errors as
well as fix them; here the index already did that.

`-p32` costs about 3% and is the recommended cold-storage setting. Recovery is
one loss per group, not two. A repaired segment is not trusted on the strength
of the arithmetic alone — `try_recover` performs a full serial decode and
checksum before accepting it.

## 19. Memory, threads, and the ceiling that made `-t` honest

Peak memory is `model + largest segment's buffers`, per worker. Since each
worker owns a complete model, `-t8` at `-9` is eight times a gigabyte, and a
machine that cannot hold that will thrash or die. So `-t` is clamped to what
the machine actually has:

```c
static int threads_for(uint64_t segbytes);   /* uses model_bytes() + ram_total() */
```

This is why `-t0` (all cores) is safe to recommend at `-5` and is quietly
reduced at `-9`. The clamp is a correctness feature: without it the tool's most
attractive-looking flag combination is also its most likely to fail overnight.

## 20. Deduplication and the CLI

### Dedup

A pre-pass SHA-256s every input before anything is compressed, then indexes
digests by open addressing on the first eight bytes — with a full 32-byte
compare before any two files are called equal:

```c
if (fsz[j] == fsz[i] && !memcmp(fsha[j], fsha[i], 32)) { dup_of[i] = (long)j; break; }
```

Duplicates become index entries pointing at the *same segments*: `m.seg0 =
o->seg0; m.nseg = o->nseg`. The economics are lopsided and that is the whole
argument for doing it — hashing runs at disk speed, compressing runs at under
a megabyte a second, so the pre-pass costs a fraction of a percent and can
remove entire files from the run.

### CLI

```
gen c [opts] archive path...   compress files or directories
gen d [opts] archive [dir]     extract (into dir, default .)
gen t [opts] archive           check integrity   (-D = deep, decode + SHA-256)
gen l [opts] archive           list contents
gen r archive out.gen          rebuild damaged segments
```

Exit codes are the contract that makes `gen` scriptable:

| code | meaning |
|---|---|
| **0** | success — and for `t`, the archive verified |
| **1** | usage or I/O error: bad flag, missing path, unreadable file, full disk |
| **2** | archive corrupt, or verification failed |

**1 and 2 are deliberately distinct.** A cron scrub has to be able to tell
"the disk is rotting" from "the path was wrong", because those page different
people. Collapsing them into a single non-zero code is the single easiest way
to make an archiver's monitoring useless.

Three platform details cost real debugging time:

- **`int _CRT_glob = 0;`** — MinGW's startup expands wildcards in `argv`
  against the CWD *before* `main` sees them. `-x '*.tmp'` was being rewritten
  to `c.tmp`, a real file in the project directory, while `-x '*.iso'`
  survived untouched because nothing matched. The bug was invisible in exactly
  the test that looked most conclusive.
- **`_stati64` / `_ftelli64`** behind `STAT_T`/`STAT_F` macros — plain `stat`
  returns `EOVERFLOW` past 2 GB, which showed up as large files getting an
  mtime of `1969-12-31`.
- **Exclusions filter stored names, not walk paths.** `-x 'logs/*'` matched
  nothing when tested against absolute paths.

---


---

# Part V — Speed

## 21. Where the time goes

Measured with `-DSTATS` builds and switch experiments, not guessed. Shares of
the bit path:

| component | share |
|---|---|
| SSE / APM stages | 31% |
| ISSE weight update | 6.3% |
| StateMap update | 2.4% |
| bit-history transition | 0.3% |
| mixer weight update | ~0% (vectorises away) |
| **predict-side memory traffic** | **~60%** |

That last row is 30 StateMap reads per bit and 60 group lookups per byte.

**It is not overhead — it is the algorithm.** Reducing it means running fewer
contexts, which is exactly what the preset ladder exposes. Any proposal that
attacks the arithmetic rather than the memory traffic is attacking the 40%,
and most of that 40% is the SSE chain, whose serial structure is load-bound
rather than ALU-bound anyway.

Two corollaries worth stating plainly, because they kill whole classes of
optimisation:

- **The mixer is already free.** Its weight update vectorises away to ~0%.
  Moving work *onto* the mixer costs nothing and buys nothing.
- **The engine is latency-bound, not throughput-bound.** The dependency chains
  (ISSE stage *k* → *k+1*, APM stage *k* → *k+1*, and predict → code → update
  within a bit) mean the CPU spends its time waiting, not computing.

## 22. What has already been done

For anyone proposing an optimisation, this is the list of things that are not
available to propose, because they shipped:

| optimisation | mechanism | measured |
|---|---|---|
| **Per-nibble prefetch** | every group address for the nibble computed and prefetched up front, validated in a second pass so misses overlap | makes table size nearly free: `-m-2..-m2`, a 16× footprint change, moved time 2.5% |
| **4-way groups in one cache line** | probing 4 candidates costs the same traffic as 1, and buys a least-established eviction policy | ratio, on overloaded tables |
| **Active-context list** (§10) | inactive contexts never hashed, probed or updated; output byte-identical | removes whole models from the loop |
| **Match bypass** (§11) | inside a long match, skip the whole model; code against a learned per-length confidence | *both* smaller and faster at gates ≥96 |
| **Confidence-gated SSE** (§13.1) | skip `a3..a6` once the estimate is already confident | `-9`: −14.6% time for +0.014% size |
| **ISSE tail gate** (§13.2) | leave the chain once its estimate is railed | `-9`: −3.7% time for +0.030% size |
| **Update thinning** (§13.3) | skip training when the model was already right | −0.70% size *and* faster |
| **Preset staging** (§9) | allocate only the stages a preset evaluates | 43 MB of never-read tables at `-1` |
| **Mixer selector narrowing** (§14) | `-f` presets drop the x86 and instruction-position dimensions | dickens: −11.2% time for +2.06% size |
| **Segment-parallel threading** | `-tN` batches the model phase across workers | near-linear to core count, RAM-clamped |

Note the pattern: the wins came from **not doing work**, not from doing the
same work faster. Three of them improve ratio at the same time.

## 23. The speedup ledger

Ranked by expected value, with the evidence for or against each. Four of the
seven are refuted by measurements already in this document.

### 23.1 Huge pages — best remaining candidate, currently untestable

At `-9` the model is ~1 GB. With 4 KB pages that is ~256K pages against an L2
TLB of roughly 1.5–2K entries: **under 1% coverage**, so nearly every random
group access risks a page-table walk. With 2 MB pages the same footprint is
~512 pages and fits comfortably.

**Status: blocked on this machine.** `t_huge.c` probes it and reports
`GetLargePageMinimum()` = 2 MB, but `SeLockMemoryPrivilege` not held
(`ERROR_NOT_ALL_ASSIGNED`) and a 1 GB `MEM_LARGE_PAGES` reservation failing
with `ERROR_PRIVILEGE_NOT_HELD` (1314). Granting "Lock pages in memory"
requires `secpol.msc` plus a logoff and cannot be done per-process. On Linux
`MADV_HUGEPAGE` needs no privilege.

**The obvious objection, and why it does not settle the question:** §14 records
that sweeping `-m-2..-m2` — a 16× table footprint change — moved total time by
only 2.5%, which reads as "footprint does not matter". But that measurement was
taken while designing `-f1`, whose tables are 9.4 MB and cache-resident
regardless. It says nothing about the 1 GB case.

There is now a second reason to want this answered: §26 shows `-7`'s time moves
21% between sessions while `zpaq` moves 4.6% and `-5` moves 3.9%. The effect is
specific to the preset with the largest footprint short of `-9`, which is
exactly the signature a TLB/page-residency effect would have. That is a
hypothesis, not a finding — but it is the same axis.

### 23.2 Stream multiplexing — strongest structural idea, largest rewrite

The engine is latency-bound on serial dependency chains (§21). Interleaving
*N* independent bit-streams within one thread fills those stalls: while stream
A waits on an L3 miss, streams B and C issue their arithmetic.

The naive version — split the segment into *N* independent mini-streams — is
just a smaller segment size, whose ratio cost `-sN` already exposes, and buys
nothing that `-t` does not.

The version worth building keeps **one shared set of context tables** and
interleaves *N* coder states with independent bit positions. Contexts stay
trained on all the data; round-robin scheduling keeps the update order
deterministic, so the decoder replicates it exactly. Ratio loss is confined to
each stream having its own recent-history register rather than the full
sequential history.

**Why it is worth considering at all:** it multiplies *single-thread*
throughput, which still matters at `-t0` because that is throughput per core.
`-t` and multiplexing compose.

**Cost:** a format change and a rewrite of the hottest loop in the engine.
High risk, and it should be prototyped against a fixed corpus before anything
commits to it.

### 23.3 An asymmetric LZ hybrid — real, but a different product

The only way to break the symmetry in §5 is to stop invoking the mixer for most
bytes. Build an aggressive LZ77 parser, code matches cleanly, and invoke the
context mixer only for literals and control codes. The decoder then spends most
of its time in `memcpy`, and decompression becomes far faster than compression.

**§11 already prices this.** The match bypass is the same trade at small scale,
and its gate sweep is unambiguous: below ~96 the bypass engages on
medium-length matches where the full model still contributes a great deal, and
repetitive files pay **14–22%** (nci +22.40%, xml +8.55% at gate 48). An
aggressive LZ parser is that experiment taken to its limit. The files it hurts
most — fixed-format records, markup, database dumps — are exactly what cold
storage is full of.

It is a legitimate design. It is not a tuning of this one; it is LZMA with a CM
literal coder, and it should be judged as a separate product with its own
ratio target.

### 23.4 Speculative two-outcome prefetch — already solved, better

*The proposal:* while waiting on the load for bit *n*, compute the context
hashes for bit *n+1* under both possible outcomes and prefetch both.

*Why it does not apply:* the premise is that each bit needs a fresh random
access worth speculating on. It does not. Group lookup happens **once per
nibble**, not once per bit — four groups share one 64-byte line, so all four
bits of a nibble read memory that is already resident. The addresses are known
exactly at nibble start and are already prefetched there (§22). The speculative
version would issue two loads for addresses the engine already knows, doubling
traffic on a machine that is latency-bound.

### 23.5 Replacing sparse tables with a small quantised neural net — refuted

*The proposal:* replace high-order context models with a few dense int8 layers,
shifting the bottleneck from memory latency (which improves slowly across
hardware generations) to vector compute (which improves quickly).

*Why it fails on this engine's own profile:* the mixer weight update is already
**~0% of runtime** — it vectorises away. The ALU side is free; moving work onto
it optimises the part that costs nothing. Meanwhile the 60% is *the algorithm*
(§21), and the sparse tables are not an implementation detail standing in for a
model — memorising gigabytes of file-specific statistics **is** the
compression. A few dense layers cannot hold that. This is why NNCP-class
compressors run orders of magnitude slower per byte, not faster.

The mixer is already a one-layer network trained by gradient descent (§2, stage
6). The idea is not wrong in kind; it is already there, and it is the cheap
part.

### 23.6 Parallelising the SSE chain — tried, rejected

Evaluating all six APM stages against the mixer output so their loads issue
together, instead of serially: **+4.07% size on dickens for 10.6% less time**,
where simply dropping to two serial stages gives +0.55% for 24% less time. The
refinement is the mechanism; a learned blend of independent stages does not
recover it. See §24.

### 23.7 Smaller, cache-resident tables — tried, rejected as a premise

The `-f` presets were designed around the theory that a cache-resident table
set is the speed lever. Sweeping `-m-2..-m2` over prose, markup, a source tree
and a binary moved total time **2.5%** and total size **4.0%**. The per-nibble
prefetch overlaps the misses, so a table 16× larger costs almost nothing to
read. The `-f` presets are therefore sized for ratio, and their speed comes
from running three contexts with no SSE — from *doing less*, not from where the
tables live.

### Summary

| idea | verdict | evidence |
|---|---|---|
| Huge pages | **untested, worth doing** | blocked by OS privilege; §14's counter-evidence does not cover 1 GB |
| Stream multiplexing | **plausible, large rewrite** | attacks the real bottleneck (§21 latency) |
| LZ hybrid | **different product** | §11 gate sweep prices it at 14–22% on repetitive data |
| Two-outcome prefetch | **already done, better** | per-nibble prefetch, §22 |
| Quantised NN | **refuted** | mixer already ~0%; tables *are* the model |
| Parallel SSE | **rejected** | +4.07% size for 10.6% time |
| Cache-resident tables | **rejected premise** | 16× footprint = 2.5% time |

The honest summary: **the large remaining wins are not in this engine's
arithmetic.** They are in memory behaviour (huge pages), in filling stalls
(multiplexing), or in changing what the product is (LZ hybrid). Everything
cheap has been done, and three of the cheap things improved ratio at the same
time.

---

# Part VI — Measurement and status

What the engine cost to measure, what the measurements later turned out
to mean, and what is and is not built.

## 24. What was tried and rejected

Negative results, with the measurement that killed each.

**Parallel SSE stages.** The six stages are the only place in the bit path with
six *dependent* random loads back to back, two into 17 MB tables. Evaluating all
six against the mixer output and blending with a learned mixer should have
collapsed six latencies into one. It cost **+4.07% size for −10.6% time** on
dickens, where simply dropping to two serial stages gives +0.55% for −24%. The
refinement *is* the mechanism; independent stages estimate the same thing six
times instead of successively sharpening it.

**Profile-guided optimization.** +0.12% / −0.28%, output byte-identical. Noise.
The null result is load-bearing: combined with the memory sweep (32× more memory
for 31% more time), it establishes the loop is bound by dependent-load latency
and arithmetic — not branches, not bandwidth.

**Switch dispatch in `rehash`.** The 20-deep `if/else` over `ORD[k]` runs 30
times per byte, so context −18 costs 18 comparisons. Converting to a `switch`
gave byte-identical output and no speed change (+0.24%, +0.65%, −0.98%). Every
context takes the same branch every byte, so the predictor is already perfect.

**Rolling hash for orders 12 and 16.** Ruled out by arithmetic before building:
28 serial multiplies per byte is ~112 cycles against ~5,110 ns/byte total, i.e.
0.6%.

**Two-layer mixer** (+0.48%), **8-way buckets**, **last-bit recency in bit
histories** (−0.007%, noise), **a larger match index** (−0.08%), **an Alpha
branch transform** (+1.5%).

---

## 25. Measurement methodology

**An ablation usually removes more than it claims.** Disabling `rehash` for
seven bytes in eight measured as 22% of runtime, reading as "hashing is
expensive". It is not — the switch experiment above proves the dispatch is free.
Skipping `rehash` also leaves the *previous* byte's hashes in place, so
`group_find` keeps hitting the same cache lines. Most of that 22% was the cache
effect of not moving.

Corrected shares of the bit path:

| component | share |
|---|---|
| SSE / APM stages | 31% |
| ISSE weight update | 6.3% |
| StateMap update | 2.4% |
| bit-history transition | 0.3% |
| mixer weight update | ~0% (vectorises away) |
| **predict-side memory traffic** | **~60%** |

That last row is 30 StateMap reads per bit and 60 group lookups per byte. It is
not overhead — it is the algorithm. Reducing it means running fewer contexts,
which is exactly what the preset ladder exposes.

**Both detectors were fixed the same way**: replicate the decision in a
standalone probe (`t_period.c`, `t_adiv.c`), dump the distribution it is
deciding on, and choose thresholds from that. Both times, thresholds chosen by
intuition were wrong in a way that surfaced only as a ratio loss on one file.

---

### Estimators and run order

This is the methodological result the archiver work depended on, and it is
recorded here because it invalidated an earlier conclusion.

A single-pass sweep **places each preset on the curve** but **cannot compare
adjacent presets**. `bench_v2.py` therefore walks the presets in a deliberately
non-monotonic order, and `--reverse` runs it backwards; the per-preset minimum
of the two passes is the estimator.

Two caveats have since been measured, and both matter more than the protocol
they qualify:

**`min()` is the wrong estimator against a trend.** It is unbiased against
random noise, where "best observed" is a floor. Against drift that is monotonic
in position it silently becomes "whichever run happened first", because *best*
and *earliest* stop being independent. An A-B-A-B design then hands arm A slot 1
every time and arm B never. `ab_balanced.py` fixes this with ABBA — both arms
at mean position 2.5, so a linear drift term cancels — and reports the mean.

**The thermal-drift premise is weaker than this document long assumed.** A
four-run ABBA on full Silesia measured `-5` at 353.5 s and 353.4 s — a 0.05%
spread — and `-7` *faster* on its later run (486.1 s at position 3 against
492.4 s at position 2). Within a session this machine is very stable, and the
position bias above is worth about 0.6%. Run-order drift is real but small; it
is **not** what the `-5`→`-7` disagreement was about. §26 is.

---

---

## 26. The reproducibility problem

**Resolved for the comparison table, still open for `-7` itself.**
`bench_session.py` measured all eight presets and all six reference codecs in
one interleaved 2 h 25 m session, with `gen -7` and `zpaq -m5` repeated at both
ends as drift sentinels. That settles what the cross-codec ratios are. It does
not explain why `-7` is unstable, and it produced a sharper version of that
question.

### What the single session established

**Sizes were never the problem and are now confirmed exactly.** Every preset
reproduced its published output size to the byte, and so did `zpaq -m5` at
39,113,069. `xz` and `gzip` differ by 96 and 108 bytes respectively, which is
embedded filename and timestamp metadata, not compression. Across every session
ever run, on every preset, the size axis has never moved.

**The corrected comparison**, `gen` archiving the directory and reference
codecs measured per file and summed — the published methodology, changed only
in that everything ran together:

| preset | output | bpc | comp | decomp | vs zpaq size | vs zpaq speed |
|---|---|---|---|---|---|---|
| `-f1` | 44,279,445 | 1.671 | 115.0 s | 115.0 s | +13.21% | 4.87× faster |
| `-1` | 43,207,157 | 1.631 | 139.2 s | 138.7 s | +10.47% | 4.02× faster |
| `-f2` | 41,376,463 | 1.562 | 155.0 s | 162.0 s | +5.79% | 3.61× faster |
| `-2` | 40,605,710 | 1.533 | 187.4 s | 187.8 s | +3.82% | 2.99× faster |
| `-3` | 39,510,297 | 1.491 | 248.5 s | 248.0 s | +1.02% | 2.25× faster |
| `-5` | 38,273,415 | 1.445 | 336.8 s | 342.4 s | **−2.15%** | **1.66× faster** |
| `-7` | 36,493,092 | 1.377 | 450.5 s | 467.4 s | **−6.70%** | **1.24× faster** |
| `-9` | 35,582,296 | 1.343 | 597.1 s | 672.5 s | **−9.03%** | 1.07× slower |

`zpaq -m5`: 39,113,069, 559.4 s compress, 581.7 s decompress.

### Two claims were wrong, in opposite directions

The published **1.35×** for `-7` divided one session's zpaq time by another
session's `-7`. The correct figure is **1.24×**.

The *correction* to that figure was also wrong. An attempt to fix it measured
`gen -7` and `zpaq` in one session and reported **1.06×** — but that run gave
zpaq a whole-directory archive, while every published reference number is
per-file-summed. zpaq is 7.6% slower per-file (559.4 s) than
whole-directory (520.1 s), because it pays twelve process startups and gets no
cross-file context. So the "fix" swapped a session mismatch for a methodology
mismatch and moved the number too far.

Both errors have the same root: **a ratio is only meaningful when numerator
and denominator were produced the same way, at the same time.**

`-9` moved the other way — 1.31× slower as published, **1.07× slower** when
measured properly (597 s here against 716 s before). Cross-session error is not
a bias in one direction; it is noise, and it happened to be flattering `-7`
while penalising `-9`.

### What is still unexplained

The sentinels are the finding. In one run, on one machine, minutes apart from
the same binary on the same input:

| sentinel | opening | closing | drift |
|---|---|---|---|
| `zpaq -m5` | 559.4 s | 559.4 s | **0.00%** |
| `gen -7` | 437.0 s | 464.0 s | **+6.18%** |

zpaq repeated to within 0.05 s across two hours. `-7` did not repeat to within
27 seconds. **The instability is inside `gen -7`, not the machine**, and it is
not confined to crossing sessions — it happens within one.

`-7`'s readings to date, all with byte-identical output: 404, 408, 437, 462,
464, 486, 490, 492 s. A 22% span.

Refuted, with the evidence:

1. **Thermal drift / run position** — a four-run ABBA measured `-5` at 353.5
   and 353.4 s (0.05%) and `-7` *faster* on its later run.
2. **Estimator choice** — `min()` versus a balanced mean differ by 0.6%.
3. **Memory pressure** — 10.2 GB free against `-7`'s 923 MB.
4. **Harness or invocation** — the exact `bench_v2.py` command reproduces the
   high value; `-q` only sets `VERBOSE`.
5. **Anything machine-wide** — zpaq's 0.00% sentinel in the same run.

What is left points at something specific to this preset's memory behaviour.
`-7` has the largest table footprint short of `-9` and the largest active
context count that still fits under a gigabyte. That is the axis huge pages
would address (§23.1), and that lever is unavailable on this machine, which is
more frustrating than it was before this run.

**Practical consequence.** Quote `-7` as **1.24× faster than zpaq, range
1.21–1.28×**, and do not quote its absolute seconds without a range. Every
other preset in the table repeated cleanly.

### Methodology note

The comparison above inherits a protocol asymmetry, kept deliberately so this
run stays comparable to what it replaces: `gen` archives the whole directory
while the reference codecs run per file. The obvious worry is that this gives
`gen` cross-file context the others do not get, flattering it on **size**.

**Measured, that worry is unfounded.** `perfile_sizes.py` compressed all twelve
Silesia files individually at `-9`:

| | total |
|---|---|
| per file, summed | 35,583,396 |
| whole directory, one archive | 35,582,296 |

**1,100 bytes apart — 0.003%.** The cross-file advantage is essentially nil,
and for a structural reason: each member starts its segment sequence fresh, and
at a 64 MB segment size the twelve members do not share enough model state for
adjacency to pay. The protocol asymmetry is real but worth three thousandths of
a percent, which is far below anything else on this page. It does not need
fixing.

The speed picture is unaffected for a different reason: zpaq's two
methodologies differ by 7.6% and `-7`'s own noise is 6%, so a matched run would
move the speed ratio by less than `-7`'s error bar.

## 27. The missing rungs: real `-4`, `-6` and `-8`

`gen.c` advertises nine levels and implements six. The preset dispatch is a
`>=` ladder with rungs at **9, 7, 5, 3, 2**, so `-8` falls into the `lvl >= 7`
branch, `-6` into `lvl >= 5`, and `-4` into `lvl >= 3`. Every parameter keyed
off the level — the context set, `NAPM`, `A46B`, `MMASK`, and all three Tier 2
gates — resolves identically. Verified on dickens: `-7` and `-8` payloads are
byte-identical past the 48-byte header, differing only in `h[8]`, the recorded
level byte. Same for `-5`/`-6` and `-3`/`-4`.

Nothing is *wrong* with this — the archives decode correctly — but a user who
picks `-8` expecting something between `-7` and `-9` silently gets `-7`.

`genlv.c` fills the three gaps. It is a prototype and is **not** promoted.

### Why the gaps are worth filling — and how that case weakened

**The original argument does not survive §26.** These rungs were designed
because `-7`→`-9` measured **30.9×**, by far the worst step on the ladder, and
a rung placed where the ladder is steepest has the most room to be useful.
That figure divided times from different sessions. Re-measured in one session:

| step | as designed against | single session |
|---|---|---|
| `-f2`→`-2` | 3.5× | 11.2× |
| `-2`→`-3` | 14.8× | 12.1× |
| `-3`→`-5` | 11.8× | 11.4× |
| `-5`→`-7` | 4.0× | 7.3× |
| `-7`→`-9` | **30.9×** | **13.0×** |

The ladder is far more even than it looked: roughly 11–13× per rung from `-2`
upward, with no step badly out of line. `-7`→`-9` is still the steepest, but
by a hair rather than by a factor of three.

So the *siting* argument for `-8` is much weaker than when it was built. What
survives is the measurement, because it was size-only and §26 confirms sizes
never moved: `-8` closes **74.8%** of the `-7`→`-9` size gap for **7.3%** more
memory than `-7`, where `-9` costs 15% more. That is a memory-axis result, and
it stands on its own. `-8`'s *time* is unmeasured, and given that `-7` — the
rung directly below it — is the one preset with 6% within-session instability,
measuring it is not a formality.

### What each rung is

| | contexts | NISSE | NAPM | A46B | MMASK | notes |
|---|---|---|---|---|---|---|
| `-4` | 10 | 6 | 3 | 4 | 2¹⁹ | `-3` + order 5 + sparse −10 |
| `-6` | 15 | 8 | 4 | 6 | 2²¹ | `-5` + orders 7, 8 + word-triple −7 |
| `-8` | 23 (26 raster) | 10 | 5 | 8 | 2²² | `-7` + orders 12, 16 + indirect −2, −3 |

`-4` deliberately omits −13, which `-5` carries: it sets `NEED_IND`, costing a
per-byte side-table update whether or not the context earns it, which is the
wrong thing to buy on a cheap rung. `-6` leaves the record contexts −4/−5 to
`-7` because they are inert unless a period was detected, and keeps `-5`'s
22-bit word tables rather than `-7`'s 23 — the point of the rung is to reach
for `-7`'s ratio without the footprint. `-8` omits −14/−15/−16/−18, the
small-domain contexts `-7`'s own comment calls out as not paying for
themselves twice over.

### The gates, which is where this went wrong first

`-8` was initially given `-9`'s gate posture on the reasoning that a high rung
should look like `-9`. Ablated on dickens, against a total `-8` gain over `-7`
of ~2,100 bytes:

| gate | first choice | cost | outcome |
|---|---|---|---|
| `SSEGATE` | 896 (interpolated 768→1024) | **1,087 bytes** | **off** |
| `BYPT` | 256 (interpolated 128→400) | **270 bytes** | **128**, as `-7` |
| `ISSEXIT` | on | **25 bytes** | **on** — time for free |

`SSEGATE` alone was consuming half the rung's entire reason to exist. This is
the mistake §11 records twice, committed a third time: **a gate interpolated
between neighbours is a guess, and guesses here have cost 14–22% before.**
`-9`'s bypass gate is 400 not because `-9` sits high on the ladder but because
`-9` has the context set that makes bypassed bytes cheap; `-8` does not, so it
wants `-7`'s gate. Only measurement separated the harmless guess (`ISSEXIT`,
25 bytes) from the expensive one.

`-6` and `-4` keep `SSEGATE` and `ISSEXIT` off, following the rule already in
the source that an unswept preset does not inherit a neighbour's guess.

### Measured, size only

Sizes are used deliberately: they have never varied across sessions, where
§26 shows times move 21%. Full Silesia, 211,938,580 bytes.

| preset | output | bpc | ratio | gap closed | peak RSS † |
|---|---|---|---|---|---|
| `-3` | 39,510,297 | 1.491 | 5.36× | — | 148 MB |
| **`-4`** | **38,985,303** | **1.472** | **5.44×** | **42.4%** of `-3`→`-5` | **180 MB** |
| `-5` | 38,273,415 | 1.445 | 5.54× | — | 326 MB |
| **`-6`** | **37,708,046** | **1.423** | **5.62×** | **31.8%** of `-5`→`-7` | **523 MB** |
| `-7` | 36,493,092 | 1.377 | 5.81× | — | 735 MB |
| **`-8`** | **35,811,692** | **1.352** | **5.92×** | **74.8%** of `-7`→`-9` | **789 MB** |
| `-9` | 35,582,296 | 1.343 | 5.96× | — | 846 MB |

† RSS measured on dickens alone, so these are model size plus one 10 MB
member's buffers — comparable to each other, but lower than the full-corpus
figures elsewhere in this document, which are dominated by mozilla at 51 MB.

**`-8` is the result worth having.** It closes three quarters of the
`-7`→`-9` size gap for 7.3% more memory than `-7`, where `-9` costs 15% more.
Placed at the steepest step on the ladder, it captures most of the size and
little of the cost — which is exactly what a rung there is for.

### The file-type inversion

The single-file and whole-corpus rankings **disagree completely**, and this is
the most useful thing the experiment produced:

| rung | dickens (prose) | full Silesia |
|---|---|---|
| `-4` | 53% | 42.4% |
| `-6` | **89%** | 31.8% |
| `-8` | 41% | **74.8%** |

`-6` looks excellent on prose and mediocre on the corpus; `-8` is the reverse.
The reason is what each rung adds. `-6`'s additions — orders 7 and 8, the
word-triple — are English-shaped, and dickens is nothing but English. `-8`'s
additions — the long orders 12 and 16, the second indirect pair — pay on
structured and binary data, which dickens contains none of and Silesia is half
made of.

This is the slice-benchmark trap in a new costume: a single file is not a small
corpus, it is a *different* corpus. Judging `-8` on dickens alone would have
rejected the best of the three rungs.

### Prototype status

Prototype. `genlv.c` sets `GEN_VERSION 3` deliberately: the level byte is
stored in the header and the decoder rebuilds the whole model from it, so a v2
archive written at `-8` — meaning `-7` — would decode against this build's `-8`
model. The checksums would catch the result, but "caught by the checksum" is
not a compatibility story, so each build refuses the other's archives by name.

Verified: all six pre-existing levels byte-identical to `genv2.exe` past the
header, and all nine levels round-trip byte-exact (`lvcheck.py`).

Not done: no timing (§26 makes it meaningless until task #26), no `gfuzz.py`
corruption run against v3 archives, no sweep of the new rungs' gates on the
six-file set — only the `-8` ablation above, on one file.
## 28. Status: what is and is not done

### Built and verified

| area | state |
|---|---|
| container: magic, version, header checksum | done |
| bounds-checked parsing of every file-sourced field | done |
| zip-slip prevention on extraction | done |
| per-segment `hash` + `chash`, per-member SHA-256 | done |
| fast scrub vs deep verify (`t` / `t -D`) | done |
| XOR recovery records (`-pN`) + `gen r` repair | done |
| directories, multi-path, exclusion globs | done |
| deduplication | done |
| threading with a RAM-aware clamp | done |
| 2 GB ceiling removed; O(segment) memory | done, verified to 2.2 GB |
| mtime and POSIX permission restore | done |
| 8 presets benchmarked on full Silesia, best-of-two | done |

**Verification gates.** Three suites must pass before a build is used:
`fuzz.py` and `tfuzz.py` (round-trip, `--v2` for the new CLI) and `gfuzz.py`
(corruption). `gfuzz.py` asserts the property that matters for a backup tool:
for *any* input, `gen` either exits 0 with byte-exact output, or exits 1/2 with
a diagnostic. 250 randomized trials across seven damage models — bitflip,
burst, truncate, zero, splice, extend, noise — produced no crash, no hang, and
**no case of exit 0 with wrong bytes**.

### Deliberately not done

- **No encryption.** Encrypting an archive destroys the ability to scrub or
  repair it — neither checksums nor parity are readable through ciphertext.
  That is a real trade, not an oversight: encrypt per-archive copies, keep an
  unencrypted scrubbing copy, or accept losing repairability.
- **No v1 read path.** The v1 container had no magic and no checksum, so there
  is nothing to safely detect it *by*. `gen26.exe` still reads v1 archives.
- **No append.** Adding to an archive means rewriting it. The index-at-end
  layout would permit it; nothing depends on it yet.
- **No random access** to a member without decoding its segments from that
  member's start.
- **Decompression is as slow as compression** and cannot be made otherwise.
  This is inherent to context mixing — the decoder runs the same model. Every
  preset decodes within ~4% of its encode speed, where `xz` decodes 20× faster
  than it encodes and `brotli` 334×. It is the reason this is not a
  general-purpose tool, and the decompression graph is the honest way to show
  it.
- **Reed–Solomon**, for the reason in §18.

### Unfinished

Split by what closing it actually takes, because the two halves need different
tools and different people.

### Code and format

- **`genv2.c` is not promoted to `gen.c`.** This is the main open item. The
  CLI argument order changed (`c archive path...`), and every benchmark script
  in the repo — `bench_final.py`, `ab_corpus.py`, `a46_ab.py` — drives the old
  form. Promotion is a judgement call about breaking the measurement harness,
  not a routine edit.
- **Recovery is one loss per group.** Two losses in a group is unrecoverable.
- **DEFLATE recovery does not cross segment boundaries**, so a zlib stream
  larger than one segment is modelled as ordinary bytes.
- **Huge pages are blocked on this machine, not evaluated.** At `-9` the model
  is ~1 GB against an L2 TLB of roughly 2K entries, so under 1% of it is
  covered and nearly every group access risks a page walk; 2 MB pages would
  cut that to ~512 pages. `t_huge.c` probes whether the experiment can run and
  reports it cannot: `GetLargePageMinimum()` is 2 MB, but
  `SeLockMemoryPrivilege` is not held (`ERROR_NOT_ALL_ASSIGNED`) and a 1 GB
  `MEM_LARGE_PAGES` reservation fails with `ERROR_PRIVILEGE_NOT_HELD` (1314).
  Granting "Lock pages in memory" needs `secpol.msc` and a logoff and cannot
  be done per-process, so the idea is untested rather than rejected. Note that
  the `-m-2..-m2` sweep in §14 does **not** settle it: that was measured while
  designing `-f1`, whose 9.4 MB tables are cache-resident anyway, and says
  nothing about the 1 GB case. On Linux `MADV_HUGEPAGE` needs no privilege.

### Benchmarking and tooling

- **Verified to 2.2 GB per file, not beyond.** The 32-bit ceiling is genuinely
  gone; nothing larger has been tried.
- **`-7`'s absolute time is not reproducible across sessions, and this is the
  most serious open problem in the project.** See §26 — it invalidates the
  headline speed claim and every cross-session comparison in README.md.
- **`ab57.py` was accused of a defect it does not have.** An earlier revision
  of this section called its 462.6 s reading for `-7` "systematically wrong"
  and "unexplained". It is neither: §26 measures `-7` at 489.6 s in one session
  and 404.4 s in another, and 462.6 s is an honest reading of a third machine
  state. The script's A-B-A-B-with-`min()` design *is* biased in principle
  (`min()` selects for earliest position, so arm A always gets slot 1), and
  `ab_balanced.py` replaces it with ABBA and a mean — but measurement showed
  that bias is worth only 0.6% here, not the 14% it was blamed for.
- **RESOLVED: `-2`'s 5.2% spread was cross-session variance, not a property of
  `-2`.** In the single session it measured 187.4 s compress and 187.8 s
  decompress, and the `-f2`→`-2` step is 11.2×, not the 3.5× that spread had
  made it look like. `-7` is now the only preset with a known instability.
- **enwik8 figures in README are from the previous build** and are flagged as
  such in place. Only the Silesia cross-reference was updated.
- **RESOLVED: reference-codec timings are no longer from another session.**
  `bench_session.py` measures every preset and every reference codec in one
  interleaved run, so the asymmetry that favoured gen on the time axis is gone.
  The protocol asymmetry on the *size* axis has since been measured and is
  negligible: whole-directory and per-file-summed differ by 1,100 bytes out of
  35.6 million, 0.003% (§26). No matched run is needed.
