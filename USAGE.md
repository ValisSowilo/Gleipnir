# Nyx — a context-mixing archiver

`nyx` compresses substantially harder than general-purpose tools and is
correspondingly slower. This document is about deciding whether that trade is
right for your data, and then operating it safely.

## Is this the right tool for your data?

Read this section before the rest. `nyx` is a poor fit for most jobs.

Measured on the full Silesia corpus (211,938,580 bytes), single-threaded, on a
Ryzen 5 4500:

Ordered by measured cost, which is **not** the order the names suggest — the
`-f` rungs interleave with the numbered ones rather than sitting below them:

| preset | output | ratio | bpc | compress | decompress | RAM |
|--------|--------|-------|-----|----------|------------|-----|
| `-f1`  | 44,279,445 | 4.79× | 1.671 | 1.82 MB/s | 1.91 MB/s | 204 MB |
| `-1`   | 43,207,157 | 4.91× | 1.631 | 1.52 MB/s | 1.52 MB/s | 196 MB |
| `-f2`  | 41,376,463 | 5.12× | 1.562 | 1.27 MB/s | 1.28 MB/s | 238 MB |
| `-2`   | 40,605,710 | 5.22× | 1.533 | 1.19 MB/s | 1.20 MB/s | 233 MB |
| `-3`   | 39,510,297 | 5.36× | 1.491 | 0.85 MB/s | 0.84 MB/s | 304 MB |
| `-5`   | 38,273,415 | 5.54× | 1.445 | 0.62 MB/s | 0.61 MB/s | 482 MB |
| `-7`   | 36,493,092 | 5.81× | 1.377 | 0.52 MB/s | 0.50 MB/s | 923 MB |
| `-9`   | 35,582,296 | 5.96× | 1.343 | 0.30 MB/s | 0.29 MB/s | 1055 MB |

Times are best-of-two across a forward and a reverse-order pass, because this
machine drifts upward within a run and a single sweep is not trustworthy for
comparing adjacent rungs. Every row was round-trip verified. No preset is
dominated: there is no rung that another beats on both size and speed.

Two things follow from that table, and both are load-bearing.

**Compression and decompression cost the same.** This is not a tunable. The
decoder runs the identical model the encoder ran — it has to, because it
reconstructs each prediction to interpret the next bit. There is no fast path
on the read side and there never will be. If you need to restore quickly under
pressure, stop here and use zstd.

**A terabyte at `-5` is about nineteen days each way, single-threaded.** `-t0`
on this six-core box measured a 4.4× speedup (`-f1` went from 1.82 to 8.00
MB/s on a 2.2 GB file), which brings that down to roughly **four and a half
days each way**. Plan capacity from those numbers, not from the MB/s figure.

So `nyx` earns its place on data that is:

- **written once and read rarely** — you pay the compression cost a single
  time and the storage saving every month afterwards;
- **retained for years** — long enough for the saved bytes to outweigh the CPU;
- **in the tens to hundreds of gigabytes** — small enough that a multi-day
  pass is tolerable;
- **text-like or structured** — logs, telemetry, documents, source, XML, JSON,
  database dumps. These are where context mixing beats LZ-based tools by the
  widest margin.

It is the wrong tool for backups you might need to restore under time
pressure, for anything in a latency-sensitive path, for data already
compressed (video, JPEG, most archives), and for petabyte-scale stores.

Against the closest comparable tool, `zpaq -m5`, `nyx -5` wins on all three
axes at once — 2.2% smaller, 1.6× faster, 43% less memory. That advantage is
specific to `-5`: `-9` is 9.0% smaller than zpaq but costs 1.31× the time and
1.26× the memory, so it wins on size alone. Against `zstd --long -19` nyx is
far smaller and far slower. Those are the comparisons worth making.

## Quick start

```bash
nyx c -5 archive.nyx /data/logs        # compress a directory
nyx l archive.nyx                      # list what is in it
nyx t archive.nyx                      # check integrity
nyx x archive.nyx /restore             # extract
nyx r archive.nyx fixed.nyx            # rebuild damaged segments
```

`x`, `e` and `d` all extract and are interchangeable — `x` is the spelling
7-Zip, WinRAR and tar use, and `d` is the one this tool started with. Unlike
7-Zip, `e` does not flatten directories here; every spelling restores the
stored structure.

Exit status is `0` on success, `1` for usage or I/O errors, and `2` when an
archive is corrupt or fails verification. Scripts should check it.

## Recommended cold-storage settings

```bash
nyx c -5 -t0 -p32 archive.nyx /data/2019-invoices
```

- **`-5`** is the value preset, and `-7` is the one to consider next. Going
  from `-5` to `-9` buys 7.0% off the size for 2.1× the time on both ends;
  most of that penalty is the last rung alone, which is the worst deal on the
  ladder. See the exchange-rate table below.
- **`-t0`** uses every core. It costs a little ratio — each worker models its
  own segment independently, so segments are smaller and models start colder —
  and buys close to linear speed. On a 10 MB file, `-t4` measured 2.67× faster
  for 2.8% larger output; on a 2.2 GB file, `-t0` measured 4.4×.
- **`-p32`** writes recovery records: one parity block per 32 segments, about
  3% overhead, letting any single damaged segment per group be rebuilt. For
  data going to a medium you will not scrub for years, this is the single most
  valuable flag here. It is off by default because it is not free.

### What each rung actually costs

The ladder plot shows where each preset sits; this shows the exchange rate
between adjacent ones — how much extra time you pay per 1% of size saved.
Lower is a better deal.

| step | size saved | time added | exchange |
|---|---|---|---|
| `-f1`→`-1` | 2.42% | 21.1% | 8.7× |
| `-1`→`-f2` | 4.24% | 11.3% | 2.7× |
| `-f2`→`-2` | 1.86% | 20.9% | 11.2× |
| `-2`→`-3` | 2.70% | 32.6% | 12.1× |
| `-3`→`-5` | 3.13% | 35.6% | 11.4× |
| `-5`→`-7` | 4.65% | 33.8% | 7.3× |
| `-7`→`-9` | 2.50% | 32.5% | **13.0×** |

The whole ladder from `-f1` to `-9` is 19.6% smaller for 5.2× the time.

No step is wildly out of line with the others: the ladder costs roughly
11-13× per rung from `-2` upward. Earlier revisions of this table put
`-7`→`-9` at 30.9× and told you to stop at `-7`; that figure came from
dividing times measured in different sessions, and a single-session
re-measurement puts the step at 13.0× — no worse than `-2`→`-3`.
The advice to stop at `-7` was an artefact of the measurement, not a
property of the ladder. Choose by memory and absolute time instead:
`-9` needs 1055 MB and 597 s where `-7` needs 922 MB and ~450 s.

Treat `-7`'s numbers as the softest here. Its two sentinel readings in the
single-session run were 437.0 s and 464.0 s — a 6.18% spread — where
`zpaq -m5` repeated to 0.00%. So `-5`→`-7` (7.3×) and `-7`→`-9` (13.0×)
each carry roughly ±6% on the time term. Every other preset repeated
cleanly, and every size in the table is exact.

Then, and this is the part people skip:

```bash
nyx t -D archive.nyx && echo "verified"
```

Verify before you delete the source, and keep verifying afterwards. That is
the subject of the next section.

## Scrubbing: finding rot before you need the data

There are two verify modes, and the distinction is operational rather than
cosmetic.

```bash
nyx t archive.nyx        # fast: checks stored checksums, no decoding
nyx t -D archive.nyx     # deep: decodes everything, checks SHA-256 too
```

The fast scrub reads each stored segment and checks it against a checksum of
the compressed bytes. It never runs the model, so it goes at disk speed. The
deep verify decodes every segment and re-derives each member's SHA-256, so it
runs at the decompressor's speed.

Measured on a real 2.2 GB archive (34 segments, `-f1`):

| | time | rate | peak RAM |
|---|---|---|---|
| `nyx t` (scrub) | **0.98 s** | 399 MB/s | 4 MB |
| `nyx t -D` (deep) | **1301 s** | 1.71 MB/s | 305 MB |

**1330× apart.** The scrub is limited by how fast the disk hands over the
bytes; the deep verify is limited by the model. Scaled to a terabyte, that is
roughly forty-five minutes against something over a fortnight.

Use them for different jobs:

- **`nyx t` on a schedule** — monthly, quarterly, whatever your retention
  policy says. It catches storage decay, which is what actually happens to
  archived data, and it is cheap enough that you will really run it.
- **`nyx t -D` once, before deleting the source.** It additionally proves the
  data decodes and matches the SHA-256 you can reconcile against a manifest.
  That catches a different class of problem — an encoder bug rather than a bad
  sector — and is worth doing exactly once, when the original still exists.

When a scrub reports damage:

```
nyx: logs/2019.json: segment 41 is damaged but recoverable from its recovery record
nyx scrub: 812 segments intact, 1 recoverable  (2196 MB read, 480 MB/s)
nyx: run 'nyx r archive.nyx fixed.nyx' to write a clean copy
```

`nyx r` rewrites the archive with every recoverable segment rebuilt. The
payloads and all their checksums carry across unchanged, so the repaired copy
verifies against the same digests as the original:

```
nyx repair: 1 segment rebuilt, 0 unrecoverable -> fixed.nyx
```

If a segment cannot be recovered — no recovery records, or two failures in one
group — `nyx r` says so, copies it through as-is rather than pretending, and
exits 2.

## What protects your data

Four independent layers, because the failure that matters is silent
corruption, not loud corruption.

1. **A magic number and a format version**, so the file identifies itself and
   a future version refuses cleanly rather than misreading it.
2. **Two XXH64s per segment** — one over the compressed blob as stored, one
   over the bytes it decodes to. The first is what makes cheap scrubbing
   possible; the second is checked before any decoded byte is written or
   hashed. Together they turn "your restore is subtly wrong" into "segment 7
   of 40 is bad".
3. **SHA-256 per member**, stored in the index and verified on extract. Also
   printable with `nyx l -L`, so an archive can be reconciled against a
   manifest you already keep without restoring it.
4. **A checksummed index, recorded twice.** Its position is in the header and
   implied by the trailer; the trailer wins, because the header is the only
   part of the file that gets rewritten and so the likelier of the two to be
   torn by a crash.

Every field the reader parses is bounds-checked against the buffer it came
from. A damaged archive produces a diagnostic and exit 2 — never a crash, a
hang, or plausible-looking wrong bytes. This is verified by `gfuzz.py`, which
subjects archives to bit flips, decayed sectors, truncation, splices, zero
holes, trailing junk and pure noise, and asserts that every single outcome is
either byte-exact output or a non-zero exit.

**Damage is contained to a segment.** One bad segment costs you that segment;
every other segment in the archive still decodes, and `t` names exactly which
ones are gone. With `-p` it usually costs you nothing at all.

## Recovery records in detail

`-pN` stores one parity block per `N` segments: the XOR of every compressed
segment blob in the group, zero-padded to the longest. Any one lost segment in
a group is then the XOR of the survivors with that block.

Overhead is about `1/N` **once an archive has many more segments than `N`**.
`-p32` is ~3%, `-p4` is ~25%.  `-p` on its own means 32.

On small archives the ratio is much worse, and this surprises people. A group
always gets one parity block sized to the largest blob in it, so an archive
with three segments and `-p32` pays a full extra block — measured at +54%, not
+3%. Recovery records are for archives large enough to have many groups. Below
a few hundred megabytes, keeping a second copy is cheaper and strictly better.

Check what you actually paid with `nyx l`, which now reports it:

```
        650000         136569  1.681  total
                        73834  recovery records: 1 block, one per 32 segments (+54.1%)
                       210952  archive file on disk
```

Parity rather than Reed–Solomon because the failure this must survive is an
*erasure at a known position* — the segment checksum already identified which
one — and parity handles that case optimally. Reed–Solomon earns its
complexity when you must locate the errors too, and here the index already
did.

The limit is one loss per group. Two damaged segments in the same group cannot
be recovered, and `nyx` will say so rather than hand you a reconstruction it
cannot vouch for: the garbage XOR fails the same segment checksum that
triggered the attempt.

## Memory and threads

Each worker owns a complete private model, so memory scales with `-t`. At `-9`
a single model is over a gigabyte, which means `-t0` on a twelve-thread machine
would ask for roughly 18 GB. `nyx` measures installed RAM and caps the thread
count to fit, telling you when it does:

```
nyx: -t12 needs ~18432 MB, capping at -t7 (16384 MB installed)
```

Memory is bounded by the segment size and thread count, **not** by how large
the input is. Compressing a 2.2 GB file at `-f1 -t0` peaked at 2572 MB across
six workers; deep-verifying the same archive single-threaded peaked at 305 MB.
Neither figure is a function of the 2.2 GB.

`-sN` sets the maximum segment size in MB (default 64). Smaller segments bound
memory and shrink the blast radius of damage; larger ones compress better,
because the model sees more history before it resets. Segments are also the
unit of parallelism, so on a file smaller than `N × -t`, `nyx` shrinks them to
keep every core busy.

`-mN` scales every context table by `2^N`. `-m-1` halves memory; `-m1` doubles
it. This changes the format's memory requirement on the *decode* side too, and
that requirement is recorded in the archive — so a file written with `-m2` needs
that much memory to read back.

## How members are named

With **one input**, the archive is rooted at it — `nyx c a.nyx /data/tree`
stores `/data/tree/logs/app.log` as `logs/app.log`.

With **several inputs**, each keeps its own last component, so they cannot
collide with each other — `nyx c a.nyx /data/tree /other/set` stores
`tree/logs/app.log` and `set/...`.

If two inputs still resolve to the same stored name, `nyx` warns and exits 1
rather than letting extraction silently keep only the last one:

```
nyx: warning: /x/data/f and /y/data/f both store as 'data/f';
     extraction will keep only the last
```

Names are always relative and slash-separated, so an archive written on
Windows extracts correctly on Linux and back.

## Excluding files

`-x GLOB` skips paths matching a pattern, and is repeatable:

```bash
nyx c -5 -t0 -p32 -x '*.iso' -x '*.mp4' -x 'cache/*' archive.nyx /data
```

**Every option has to come before the archive name.** Anything after it is an
input path, so a trailing `-x '*.iso'` is read as two files that do not exist —
`nyx` reports them and exits 1, having archived everything you meant to skip.

Patterns support `*` and `?`, and are matched against both the name the file
will have *inside* the archive and its bare filename — so `-x '*.log'` works
regardless of how deep the tree goes, and `-x 'cache/*'` refers to the
archive's own layout rather than to a path on the machine you happen to be
running from.

Excluding data that is already compressed is usually worth it. `nyx` will
detect incompressible input and store it verbatim, but it pays full model
speed to discover that.

## Inventory and manifests

`nyx l -M` prints a `sha256sum`-compatible manifest:

```
7838d85690aa8248ee7a45a9053903dab5777695339836b6e71941609de8ca65  logs/app.log
5b31bc156c1a10af2a0e17dbcd6925e03141ce65607fa980c7670005d6ebdcbc  logs/data.xml
```

Two uses. Before archiving, save it next to the archive so the contents are
auditable without opening it. After restoring, pipe it to `sha256sum -c` to
confirm the extracted tree independently of `nyx`'s own verification — a check
that does not share code with the thing being checked.

## Deduplication

Identical files are compressed once. `nyx` hashes every input before
compressing anything — reading at disk speed to avoid re-compressing at under
a megabyte a second — and duplicates become index entries pointing at the same
segments. On redundant directory trees this is not a small effect, and it is
reported:

```
nyx: 2 duplicate files stored once, 1 MB not recompressed
```

## Full option list

```
nyx c [opts] archive path...            compress files or directories
nyx x [opts] archive [dir] [member...]  extract (into dir, default .)
                                        d and e do the same thing
nyx t [opts] archive                    check integrity
nyx l [opts] archive                    list contents
nyx r archive out.nyx                   rebuild damaged segments

-1..-9    preset, -1 fastest .. -9 smallest (default 5)
-f1/-f2   throughput presets below -1
-mN       resize every context table by 2^N (-m-1 halves them)
-tN       worker threads, -t0 = all cores (default 1)
-sN       segment size in MB (default 64)
-pN       recovery records, one parity block per N segments (-p alone = 32)
-D        with t, decode everything and check SHA-256 as well
-L        with l, also print each member's SHA-256
-M        with l, print a sha256sum-compatible manifest instead
-x GLOB   exclude paths matching GLOB (repeatable)
-q        quiet
--version, --help
```

Extraction takes an optional list of member names after the destination, so
`nyx x archive.nyx /restore logs/2019.json` pulls out just that one.

## Safety when extracting

An archive is untrusted input. `nyx` refuses member names that are absolute,
carry a drive letter, contain a `..` component, or hold control characters, so
extraction cannot write outside the destination directory. Stored names are
always relative and slash-separated, so an archive written on Windows extracts
correctly on Linux and back.

Modification times are restored, and permission bits are recorded on every
platform and reapplied on POSIX. They are deliberately not reapplied on
Windows: the only bit `st_mode` really carries there is read-only, and setting
it would leave extracted files the user cannot delete without a separate step.

## Format

```
[header 48B]        magic "GENA", version, preset, member count,
                    index offset, XXH64 over the preceding 40 bytes
[segment payloads]  written in order, streamed, never all resident
[recovery blocks]   interleaved after each group, when -p is used
[index]             members (name, size, mtime, mode, SHA-256, segment
                    range) then segments (offset, lengths, XXH64),
                    then the parity group size and block table
[trailer 20B]       index XXH64, index length, magic
```

The index is at the end so writing never seeks backwards, which is what lets
the whole thing stream. A segment is the unit of independent compression,
integrity, parallelism and damage containment — one mechanism serving all
four.

## Known limits

- **Decompression is as slow as compression** and cannot be made otherwise.
- **No append.** Adding to an existing archive means rewriting it.
- **No random access to a member** without decoding its segments from the
  start of that member.
- **DEFLATE recovery does not cross segment boundaries**, so a zlib stream
  larger than one segment is modelled as ordinary bytes rather than recovered.
- **v1 archives are unreadable.** The pre-archive container had no magic
  number and no checksum. `gen26.exe` still reads them.
- **Permissions are restored on POSIX only** (deliberately — see above).
- **Verified to 2.2 GB per file, not beyond.** A 2,223,777,559-byte file
  compresses, scrubs and deep-verifies with its SHA-256 intact, so the old
  32-bit size ceiling is genuinely gone; nothing larger has been tried.
- **Recovery covers one loss per group**, not two.
- **No encryption.** If the data needs it, encrypt the archive afterwards —
  and note that doing so destroys the ability to scrub or repair it, since
  neither the checksums nor the parity blocks are readable through the
  ciphertext. Encrypt per-archive copies, keep an unencrypted scrubbing copy,
  or accept that you are trading repairability for confidentiality.

## Verification

Everything above is checked by three suites that must all pass before a build
is used:

```bash
python fuzz.py  nyx.exe --v2      # 81 edge cases x 8 presets = 648 round trips
python tfuzz.py nyx.exe --v2      # decode at a different -t than encoded
python gfuzz.py 250 --exe nyx.exe     # corruption: every mode, every damage model
```

`gfuzz.py` is the one specific to this format. It asserts the contract that
makes an archiver trustworthy: for *any* input, `nyx` either exits 0 with
byte-exact output or exits non-zero with a diagnostic. Never a crash, never a
hang, and never exit 0 with wrong bytes.
