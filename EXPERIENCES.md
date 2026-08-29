# Experiences

What building this actually taught, including the parts that were wrong for
days before anyone noticed. Written to be useful to whoever works on it next —
which may be the same person who wrote it, having forgotten.

The engineering is in [ARCHITECTURE.md](ARCHITECTURE.md). This file is about
the process, and it is deliberately weighted toward mistakes, because those are
the parts that do not survive in the code.

---

## 1. Measurement is harder than the thing being measured

More time went into finding out what the numbers meant than into making them
better. Almost every one of the following was discovered *after* a conclusion
had been published on top of it.

### Consistency is not accuracy

The published `-7` timing was 404.4 s, backed by two independent passes — a
forward sweep and a reverse-order sweep — agreeing to within 1%. A third
protocol dissented at 462.6 s and was written off as an outlier, with a
plausible argument: two passes agreeing beat one that does not, and since noise
only ever *adds* time, the minimum is the right estimator.

Both halves of that argument were wrong.

`-7` has since measured **404, 408, 437, 462, 464, 486, 490 and 492 seconds**,
with byte-identical output every time. The two agreeing passes came from the
same session, and agreement within a session says nothing about the spread
*between* sessions. The "outlier" was an honest reading of a different machine
state.

> **Two measurements that agree may only be telling you they were taken
> together.** Independence has to come from somewhere; running the same
> protocol twice does not supply it.

### `min()` is the wrong estimator against a trend

`min()` is unbiased against random noise, where the smallest observation is a
floor. Against drift that is monotonic in position it silently becomes
"whichever ran first", because *best* and *earliest* stop being independent.

An A-B-A-B design with `min()` therefore gives arm A slot 1 every single time
and arm B never. Alternating the arms *feels* like it controls for order; with
that estimator it does the opposite of nothing — it bakes the bias in and
launders it as a controlled experiment.

The fix is ABBA with a mean: both arms sit at mean position 2.5, so a linear
drift term cancels. Measured here, that bias was worth only 0.6% — small — but
it had been blamed for a 14% discrepancy it did not cause.

### A ratio is only meaningful if numerator and denominator were produced
### the same way, at the same time

This one error produced two *different* wrong answers for the same quantity:

| claim | how it was produced | value |
|---|---|---|
| published | one session's zpaq ÷ another session's `-7` | 1.35× |
| "corrected" | matched session, but zpaq run whole-directory vs the published per-file protocol | 1.06× |
| actual | one session, one protocol | **1.24×** |

The correction was as wrong as the thing it corrected, and *more* dangerous,
because it arrived wearing the language of rigour. Fixing a session mismatch by
introducing a methodology mismatch is not progress.

### Drift sentinels, and what they caught

The eventual fix was to measure everything — eight presets, six reference
codecs — in one interleaved 2 h 25 m run, with `gen -7` and `zpaq -m5` repeated
at *both ends* as sentinels.

That last detail is the one worth stealing. The sentinels turned an assumption
into a measurement:

| sentinel | opening | closing | drift |
|---|---|---|---|
| `zpaq -m5` | 559.4 s | 559.4 s | **0.00%** |
| `gen -7` | 437.0 s | 464.0 s | **+6.18%** |

zpaq repeated to within 0.05 s across two hours. `-7` missed by 27 seconds. In
one run, one machine, same binary, same input. Without sentinels this would
have shipped as another confident number.

**It also proves a negative that would otherwise be unprovable.** Every
"machine-wide" explanation — thermal, background load, power state — dies on
zpaq's 0.00%. That single column eliminated four hypotheses at once.

### Sizes never moved

Through every session, protocol and estimator, **every output size reproduced
exactly**, including zpaq's 39,113,069 to the byte. The entire crisis was
confined to the time axis.

Corollary worth internalising: when an experiment has a deterministic component
and a noisy one, do the deterministic comparisons first and lean on them. The
`-4`/`-6`/`-8` prototype was evaluated on size alone, and that judgement is
still standing while every timing conclusion around it has been revised twice.

---

## 2. Mistakes that came from over-generalising a true fact

The most expensive errors were not false beliefs. They were true beliefs
applied one step too far.

**"Sizes are stable"** — established over many sessions, absolutely true for a
fixed binary — became "the per-file chart is still valid", and it was not: that
chart came from a *different build*, stale by 0.4% on every bar. Sizes are
**session**-stable, not **build**-stable. Two hours had just been spent proving
the first half, which is precisely what made the second half feel safe.

**"The gate sweep is relative, so it stands"** — asserted one message after the
above, without checking. Measured: 22,423,769 today against the table's
22,470,998. From an older build too, 0.21% out.

**"Interpolate the gate between neighbours"** — `-8` was given `SSEGATE` 896,
between `-7`'s 768 and `-9`'s 1024. Ablation showed that gate cost 1,087 bytes
against the rung's *total* gain of ~2,100. It was consuming half the reason the
rung existed. `-9`'s bypass gate is 400 not because `-9` is high on the ladder
but because `-9` has the context set that makes bypassed bytes cheap.

> A parameter that varies monotonically across presets is not thereby
> *interpolatable*. Ordinal position is not a cause.

The codebase already recorded this exact lesson twice, in comments, with
numbers attached. It was committed a third time anyway.

---

## 3. What the engine taught

**The wins came from not doing work, not from doing work faster.** Every
significant speedup was a decision to skip: contexts that cannot affect the
output, SSE stages once the estimate is already confident, the ISSE tail once
it is railed, the whole model inside a long match. Three of them made the
output *smaller* at the same time. Nothing that made the arithmetic faster
mattered, because the arithmetic was never the cost — 60% of runtime is
predict-side memory traffic, and the mixer weight update vectorises away to
approximately zero.

**Skipping requires an invariant, and the invariant is the design.** A decision
to skip must be computable by the decoder from state it already holds. Violate
it and the archive is silently corrupt from that byte on. Every shortcut was
designed against that constraint *first* and measured second, which is the only
order that works — a shortcut that wins 20% and cannot be replicated by the
decoder is worth nothing.

**One mechanism, four properties.** The single best structural decision was
making a segment the unit of compression, integrity, parallelism *and* damage
containment simultaneously. The v1 design had "the thing a thread compresses"
and "the thing that fails independently" as separate concepts. Collapsing them
meant containment and threading did not each need their own machinery.

**The most valuable feature was an asymmetry nobody asked for.** Storing two
checksums per segment — one over the decoded bytes, one over the compressed
blob — allows integrity checking at *disk speed* instead of model speed. They
are 1330× apart. That converts verification from nineteen days per terabyte to
about forty-five minutes, which is the difference between a scrub policy that
exists and one that is written down and never run. An archive nobody can
afford to check is an archive whose rot is discovered at restore time.

**Choose the algorithm for the failure you actually have.** Recovery uses XOR
parity rather than Reed–Solomon because the checksum has *already localised*
the damage. That makes it an erasure at a known position, which parity handles
optimally. RS earns its complexity when you must find the errors as well as fix
them.

---

## 4. Process

**Prototype in a separate file when the change alters output semantics.** The
`-4`/`-6`/`-8` rungs went into `genlv.c` with the format version bumped to 3,
because the level byte is stored in the archive header and the decoder rebuilds
the whole model from it. A v2 archive written at `-8` means `-7`; decoded by a
build where `-8` means something else, it produces garbage. The checksums would
catch it — but *"caught by the checksum"* is not a compatibility story. The
version field exists exactly so two builds can refuse each other by name.

**Verify the baseline before trusting the delta.** Before measuring the new
rungs, `lvcheck.py` asserted that all six *existing* levels were byte-identical
to the previous binary past the header. Without that, every comparison would
have been against a moving baseline and no one would have known.

**Test the harness on something cheap first.** The single-session benchmark got
a two-file smoke run before the 2 h 25 m version. That caught a `--quick` mode
which limited the reference files but still ran `gen` over the whole corpus —
so the "quick" check would have cost as much as the real thing.

**Say what a number cannot support.** `-7` is published as 1.24× with an
explicit ±6% and a stated range, because its instability is real and unexplained.
Every other preset repeated cleanly and carries no caveat. A number with an
honest error bar is worth more than a precise one that quietly is not.

**Never let a pipe eat an exit code.** Running `python tfuzz.py … | tail -6` in
the release check reported success, because the shell returns `tail`'s status
and not the fuzzer's. Re-run without the pipe: **exit 1**. The verdict line had
been scrolled off by `tail` and the failure had been invisible.

The failure turned out to be an invocation error — `tfuzz.py` takes an
executable as its argument, and it had been handed `40`, a trial count it does
not accept, so every case ran against a nonexistent binary and failed
identically. That is *precisely* the trap documented in the script's own header
comment about driving the wrong CLI. Reading it did not prevent repeating it.

Two fixes, both cheap: the harness now refuses to start when the binary does
not exist, and release checks do not pipe the thing whose exit code they
depend on. The general form:

> A test that cannot fail loudly is not a test. Check that your check can
> actually report failure — most easily by making it fail on purpose once.

---

## 5. Still open

- **`-7`'s 6% within-session instability is unexplained.** Refuted: thermal
  drift, estimator choice, memory pressure, harness differences, and anything
  machine-wide. What remains points at memory residency — it has the largest
  table footprint short of `-9` — which is the axis huge pages would address,
  and that is blocked on a Windows privilege that cannot be granted
  per-process.
- **No version control.** Twenty-four near-duplicate `gen*.c` files, 1.9 MB of
  them, and no history. This is not a stylistic complaint: it directly caused
  two of the errors in §2, because "which binary produced this number" is not
  recoverable from the filesystem. `git init` would have prevented both.
- **zstd is missing from the comparison set.** The references are zpaq, lpaq1,
  xz, brotli, bzip2 and gzip — essentially the 2015 lineup. `zstd --ultra -22`
  is what anyone would ask about first, and its absence makes the comparison
  look better chosen than it is.
- **RESOLVED: Linux works.** Verified on Ubuntu 26.04 with gcc 15.2 -- builds
  clean, passes both fuzz suites, and archives cross platforms in both
  directions with SHA-256 intact. The code audit that preceded it was the
  pleasant surprise: every Windows API already had a POSIX branch, and exactly
  one line (`_CRT_glob`) needed guarding.

  The interesting finding was a claim that turned out false. "The format is
  byte-identical across platforms" was written into the README on the strength
  of the format being fixed-width and little-endian. It is not true: two
  archives of the same directory differ in almost every byte, because
  `readdir` and `FindFirstFile` return members in different orders and each
  member is its own segment. Per-member *compressed sizes* are identical, which
  is the claim that actually mattered -- but the one that had been written down
  was the one nobody had tested.

---

## 6. The short version

1. Two agreeing measurements may only be telling you they were taken together.
2. `min()` selects for *earliest*, not *best*, whenever position matters.
3. A ratio needs its numerator and denominator produced the same way, at the
   same time.
4. Repeat one cheap thing at both ends of a long run; it converts "the machine
   was probably stable" into a number, and it can prove a negative.
5. Do the deterministic comparisons first and lean on them.
6. A true fact applied one step too far is more dangerous than a false one,
   because it arrives with evidence attached.
7. Ordinal position is not a cause; do not interpolate a parameter just because
   it happens to be monotone.
8. The fastest code is the code that establishes it does not need to run — and
   proves the other side can reach the same conclusion independently.
