# Where Gleipnir stands on the published boards

This file records exactly how Gleipnir compares to the two public benchmark
tables it is measured against, and — more importantly — what those comparisons
do *not* mean.

> **Gleipnir is not listed on either board.** It has never been submitted to,
> tested by, or verified by anyone but its author. Every placement below is a
> comparison of Gleipnir's own measured totals against figures published on
> those pages. Read them as "would place around", never as "is ranked".

Both pages were fetched and parsed on **2026-09-07**. Entry counts move as
Mahoney adds results, so the denominators here are a snapshot with a date on
it, not a constant. The previous revision of the README carried "50th of 211",
a count that had been stale for some time; that is the failure this file exists
to prevent.

---

## Silesia Open Source Compression Benchmark

<http://mattmahoney.net/dc/silesia.html> — ranks by total compressed size over
the twelve-file Silesia corpus (211,938,580 bytes). **319 entries** on
2026-09-07.

### Two totals, and why they differ

Gleipnir has two defensible Silesia figures, and they are not interchangeable:

| figure | bytes | what it is |
|---|---|---|
| 35,582,296 | whole-directory archive | one archive over all twelve files, so deduplication and a shared model work *across* file boundaries |
| 35,773,957 | per-file sum | each file compressed on its own, then the twelve results added |

The gap is **191,661 bytes**, 0.539%.

**The board compresses the twelve files individually**, so 35,773,957 is the
like-for-like figure and 35,582,296 is not. The larger number is the honest one
to compare here, even though it is the worse one.

### Where each would place

| figure | placement |
|---|---|
| 35,582,296 | 49th of 320 |
| 35,773,957 | **50th of 320** — the protocol-correct comparison |

Neighbourhood for the per-file figure:

```
  47.  35,457,761   fp8_v4 -8
  48.  35,511,180   paq8l -7
  49.  35,586,302   paq8pxd_v4 -5
  50.  35,773,957   gleipnir -9   <-- would place here
  51.  35,909,528   paq8px_v69 -5
  52.  35,983,639   paq8pxd_v4 -4
  53.  36,603,712   precomp v0.4.4 -cn | zpaq 7.05 -method 7
```

The margin is thin in both directions: 3rd place on this stretch is decided by
under 0.6% of the total, and at the whole-directory figure Gleipnir clears
`paq8pxd_v4 -5` by 4,006 bytes.

### "Ahead of every zpaq entry"

True, and it survives the stricter figure. The board carries **71 entries
involving zpaq**; the smallest is 36,603,712 (`precomp v0.4.4 -cn | zpaq 7.05
-method 7`), and the best plain zpaq is 38,995,519 (`zpaq 6.21 -method 7`).
Both Gleipnir figures are below all of them.

---

## Large Text Compression Benchmark (enwik9)

<http://mattmahoney.net/dc/text.html> — **222 entries** on 2026-09-07.

### How it ranks, which is not how you would guess

> "Compressors are ranked by the compressed size of enwik9 (10^9 bytes) plus
> the size of a zip archive containing the decompresser. Options are selected
> for maximum compression at the cost of speed and memory. **Other data in the
> table does not affect rankings.**"

Two consequences that matter:

**The decompressor counts.** Gleipnir's raw 157,073,377 is not the number that
would be listed. Zipped at deflate level 9:

| decompressor | zip | total | placement |
|---|---|---|---|
| `gleipnir.c` (201,173 raw) | 63,850 | 157,137,227 | 31st of 223 |
| `gleipnir.exe` (281,090 raw) | 134,220 | 157,207,597 | 31st of 223 |

It lands in the same slot either way — the decompressor is 0.04–0.09% of the
total, against a 4.5% margin over the next entry down. zlib was the risk here
and it does not bite: the released executable statically links it, so the exe
zip is self-contained rather than needing zlib source alongside it.

```
  28.  153,238,244   fp8 v3            -8
  29.  156,391,589   WinRK 3.03        pwcm +td 800MB SFX
  30.  157,049,402   ppmonstr J        -m1700 -o16
  31.  157,137,227   gleipnir -9       <-- would place here
  32.  157,388,188   stc
  33.  159,363,208   zcm 0.93          -m8 -t1
  34.  159,842,292   slim 23d          -m1700 -o12
```

**Memory does not affect ranking.** This is worth stating plainly because it is
easy to get wrong. The page says elsewhere:

> "I will select the maximum memory setting that does not cause disk thrashing,
> usually about 1800 MB."

That is Mahoney describing how *he* picks options when testing a program on
*his* hardware. It is not a submission limit, and it does not gate placement.
**34 of the 222 entries exceed 1800 MB**, the largest by a wide margin —
`nakamichi 2019-Jul-01` at 302,000 MB, `ghost` at 88,000 MB, `nanozip 0.09a`
at 32,000 MB. Gleipnir's single-segment enwik9 footprint is unremarkable in
that company.

---

## The segmentation caveat, which applies to both

The enwik8 and enwik9 figures in [README.md](README.md) are measured with `-s`
set above the file size, so the whole file is one segment. That matches how the
reference codecs on those pages compress them, and the README says so.

At the default 64 MB segment size Gleipnir splits enwik9 into sixteen segments,
each restarting from a cold model, which costs size and saves memory. The
README has long quoted that cost as "about 4.3%" as an estimate rather than a
measurement.

### Measured on 2026-09-07

It is no longer an estimate. Both runs on one machine against the canonical
1,000,000,000-byte enwik9 (`sha256 159b8535…`), compressed with a 1.0.1 build
(`built Aug 31 2026 18:34:50`). That build is not byte-identical to the
`gleipnir.exe` attached to the v1.0.1 release, but the two were checked against
each other and produce identical output at `-9`, so the figures stand for the
release.

| run | archive | member data | bpc | peak RSS | segments |
|---|---|---|---|---|---|
| `-9 -s1000` — one segment | 157,073,381 | 157,073,165 | 1.257 | **3,032 MB** | 1 |
| `-9` — default `-s64` | **164,080,953** | — | 1.313 | **977 MB** | 15 |

The 3,032 MB confirms the "peaks at 3.0 GB" the README had asserted without a
measurement behind it.

#### A four-byte discrepancy worth explaining rather than rounding away

The single-segment archive came out **157,073,381**, four bytes above the
157,073,377 this project has published since the figure was first taken. On a
project whose README says sizes "reproduce across every session ever run", four
bytes is not something to wave through.

They reproduce. The compressed stream is identical; the container is not.
**Archive size includes the stored member name, one byte per character.**
Compressing identical content under names of different lengths:

```
  stored as "e9"      (2 chars) ->  2,027,458
  stored as "enwik9"  (6 chars) ->  2,027,462     +4 bytes for +4 characters
```

The run above stored `enwik9`; the published figure was taken from a run that
stored a two-character name. That is the whole of the difference, and the codec
output is byte-for-byte the same.

Worth carrying into any submission: **an archive total is not a pure codec
metric here.** Two people compressing the same bytes with the same binary will
report different totals if they named the file differently. Quote the stored
name alongside the number, or the number cannot be checked.

**The segmentation cost is +4.46%**, against the "about 4.3%" the README had
carried. Close, but the estimate was low, and it is now a measurement.

Two things fell out of measuring it that had been wrong in the docs:

- **Fifteen segments, not sixteen.** 1,000,000,000 bytes is 953.7 MiB, and
  953.7 / 64 = 14.9, so the file needs fifteen. `gleipnir t` on the resulting
  archive agrees: `15 segments intact`.
- **Peak RSS at default segmentation is 977 MB** — comfortably inside the
  1800 MB figure Mahoney mentions, and roughly a third of what the
  single-segment run is believed to need. If a memory-constrained result were
  ever wanted, this is it, and it already exists.

The placement cost of running that way:

| run | total with `gleipnir.c` zip | placement |
|---|---|---|
| one segment | 157,137,227 | 31st of 223 |
| default `-s64` | 164,144,803 | 43rd of 223 |

Twelve places for 4.46%, which is what a board this dense at the top costs.

The default run took 3192.0s at 0.31 MB/s, against 3186.3s published for the
single-segment run — segmenting bought no speed here, which is worth knowing
because one might reasonably expect it to. **That timing was taken while this
machine was not verifiably idle, so read it as indicative only**; the size, the
segment count and the peak RSS are deterministic and are unaffected by load.

---

## Reproducing any of this

Sizes here are deterministic and reproduce exactly; only timings vary between
sessions (see [README.md](README.md) on why every timing table comes from a
single interleaved run).

```bash
# the two Silesia totals
python scripts/bench_final.py            # writes bench_final.json
python -c "import json; d=json.load(open('bench_final.json')); \
           print(sum(r[1] for r in d['levels']['9']['rows']))"

# enwik9, both segmentations
gleipnir c -9 enwik9-s64.gl   enwik9      # default: sixteen segments
gleipnir c -9 -s1000 enwik9-solid.gl enwik9   # one segment
```

The board pages change. Re-fetch them before repeating any placement claim in
this file, and update the date at the top when you do.
