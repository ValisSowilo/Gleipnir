# Where Gleipnir stands on the published boards

This file records exactly how Gleipnir compares to the two public benchmark
tables it is measured against, and — more importantly — what those comparisons
do *not* mean.

> **Gleipnir 1.0.2 is listed on both boards**, added by Matt Mahoney on
> **2026-09-25**: **35th of 227** on the Large Text Compression Benchmark and
> **49th of 322** on the Silesia Open Source Compression Benchmark. The results
> were submitted by the author and measured on the author's machine (LTCB note
> 116); as the LTCB page says of every submitted entry, Mahoney has not
> re-run them himself.

Both pages were last fetched and parsed on **2026-09-25**. Entry counts move as
Mahoney adds results, so the denominators here are a snapshot with a date on
it, not a constant. The previous revision of the README carried "50th of 211",
a count that had been stale for some time; that is the failure this file exists
to prevent.

---

## Silesia Open Source Compression Benchmark

<http://mattmahoney.net/dc/silesia.html> — ranks by total compressed size over
the twelve-file Silesia corpus (211,938,580 bytes). **322 entries** on
2026-09-25, Gleipnir included (the page reads "as of Sept. 25, 2026").

### The listed figure

The board compresses each file individually. Measured that way on the
**released v1.0.2 binary** (`gleipnir.exe`, SHA-256 `013522ab…6dc7`) on
2026-09-23, `-9 -t1`, one file per archive, every archive decompressed and its
SHA-256 checked against the original:

| file | bytes | KB (page format) |
|---|---:|---:|
| dickens | 2,048,642 | 2048 |
| mozilla | 9,632,216 | 9632 |
| mr | 2,025,644 | 2025 |
| nci | 1,136,166 | 1136 |
| ooffice | 1,745,601 | 1745 |
| osdb | 2,196,117 | 2196 |
| reymont | 882,432 | 882 |
| samba | 2,645,273 | 2645 |
| sao | 3,854,835 | 3854 |
| webster | 5,499,051 | 5499 |
| x-ray | 3,608,251 | 3608 |
| xml | 309,168 | 309 |
| **total** | **35,583,396** | |

719.3 s to compress and 709.4 s to decompress in total, single thread, on an
AMD Ryzen 5 4500 with 16 GB of DDR4 under Windows 11. Peak working set 1056 MB
compressing and 1095 MB decompressing, both on `mozilla`.

### The other totals, and why they are not the one to compare

| figure | what it is |
|---|---|
| **35,583,396** | per-file, v1.0.2 release — the like-for-like figure above |
| 35,582,296 | whole-directory archive from the pre-release build used in the README's timing session; one archive over all twelve files, so deduplication and a shared model work *across* file boundaries |
| 35,773,957 | per-file, from a development build (`genf1.exe`, `bench_final.json`); superseded |

The release is 190,561 bytes smaller per-file than the development build that
produced `bench_final.json`, which is why that file's total should no longer be
quoted.

### Where it is listed

```
  46.  35,336,837   paq8l -8
  47.  35,457,761   fp8_v4 -8
  48.  35,511,180   paq8l -7
  49.  35,583,396   gleipnir 1.0.2 -9 -s1000 -t1   <-- listed 2026-09-25
  50.  35,586,302   paq8pxd_v4 -5
  51.  35,909,528   paq8px_v69 -5
  52.  35,983,639   paq8pxd_v4 -4
```

**49th of 322.** The margin is thin in both directions: Gleipnir clears
`paq8pxd_v4 -5` by 2,906 bytes and trails `paq8l -7` by 72,216.

The row as published:

```
 35583396  2048  9632 2025  1136 1745  2196  882  2645 3584  5499  3698  309 gleipnir 1.0.2 -9 -s1000 -t1
```

The total matches exactly. Two per-file cells do not match the measurement
above: the page shows **sao 3584** and **x-ray 3698** where the measured sizes
are 3854 and 3608 KB. The twelve cells on the page sum to 35,399 KB, not the
~35,579 the total implies, while the measured cells do sum to it — so this looks
like a transcription slip on the page. It does not affect the rank, which is by
total only.

### "Ahead of every zpaq entry"

True, and it survives the stricter figure. The board carries **71 entries
involving zpaq**; the smallest is 36,603,712 (`precomp v0.4.4 -cn | zpaq 7.05
-method 7`), and the best plain zpaq is 38,995,519 (`zpaq 6.21 -method 7`).
Every Gleipnir figure above is below all of them.

---

## Large Text Compression Benchmark (enwik9)

<http://mattmahoney.net/dc/text.html> — **227 entries** on 2026-09-25, Gleipnir
included as entry [.1571](https://mattmahoney.net/dc/text.html#1571).

### How it ranks, which is not how you would guess

> "Compressors are ranked by the compressed size of enwik9 (10^9 bytes) plus
> the size of a zip archive containing the decompresser. Options are selected
> for maximum compression at the cost of speed and memory. **Other data in the
> table does not affect rankings.**"

Two consequences that matter:

**The decompressor counts.** Gleipnir's raw 157,073,381 (v1.0.2 release, stored
name `enwik9`) is not the number that would be listed. Zipped at deflate level 9:

| decompressor | zip | total | rank |
|---|---|---|---|
| `gleipnir.c` v1.0.2 (209,435 raw) | 66,527 | 157,139,908 | **35th of 227 — the listed entry** |
| `gleipnir.exe` v1.0.2 (255,857 raw) | 124,882 | 157,198,263 | would also be 35th |

It lands in the same slot either way — the decompressor is 0.04–0.09% of the
total, against a 4.5% margin over the next entry down. zlib was the risk here
and it does not bite: the released executable statically links it, so the exe
zip is self-contained rather than needing zlib source alongside it.

```
  32.  153,238,244   fp8 v3            -8
  33.  156,391,589   WinRK 3.03        pwcm +td 800MB SFX
  34.  157,049,402   ppmonstr J        -m1700 -o16
  35.  157,139,908   gleipnir 1.0.2    -9 -s1000 -t1   <-- listed 2026-09-25
  36.  157,388,188   stc
  37.  159,363,208   zcm 0.93          -m8 -t1
  38.  159,842,292   slim 23d          -m1700 -o12
```

The board gained four entries above this point between 2026-09-07 and
2026-09-23, which is why this reads 35th where the previous revision read 31st.
Nothing about Gleipnir changed.

The row as published, with the source zip (`sd`) as the decompressor and the
hardware in note 116 (Ryzen 5 4500, 16 GB DDR4, Windows 11):

```
gleipnir 1.0.2  -9 -s1000 -t1  18,810,680  157,073,381  66,527 sd  157,139,908  3170  3150  3836  CM  116
```

It matches the v1.0.2 measurements in this file: enwik9 157,073,381, 3170 /
3150 ns/byte, 3,836 MB peak (the decompression peak). The listed enwik8,
18,810,680, is four bytes above the 18,810,676 the README quotes — the same
stored-name effect explained below: the archive stored `enwik8` rather than a
two-character name. The codec output is identical.

The v1.0.2 release was measured on enwik9 on 2026-09-23: **157,073,381** in one
segment (`-9 -s1000 -t1`, stored name `enwik9`), 3169.7 s to compress and
3150.1 s to decompress, peak working set **3,033 MB compressing and 3,836 MB
decompressing**, round trip verified by SHA-256. The size is byte-identical to
the 1.0.1 run below.

**Memory does not affect ranking.** This is worth stating plainly because it is
easy to get wrong. The page says elsewhere:

> "I will select the maximum memory setting that does not cause disk thrashing,
> usually about 1800 MB."

That is Mahoney describing how *he* picks options when testing a program on
*his* hardware. It is not a submission limit, and it does not gate placement.
**34 of the 222 entries on 2026-09-07 exceeded 1800 MB**, the largest by a wide margin —
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
| one segment | 157,139,908 | 35th of 227 (listed) |
| default `-s64` | 164,147,480 | would be 47th of 227 |

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

The board pages change. Re-fetch them before repeating any rank in this file,
and update the date at the top when you do; ranks drift as entries are added
above, even though Gleipnir's own figures do not.
