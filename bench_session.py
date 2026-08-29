"""Every preset and every reference codec, measured inside ONE session.

This exists because of ARCHITECTURE.md §26.  `-7` has been measured at 404 s,
462 s and 490 s in three different sessions, each internally consistent to
~1%, with byte-identical output.  Within a session this machine is very stable
(0.05% spread on -5); between sessions it is not, and the movement is
preset-specific -- zpaq moved -4.6% and -5 moved +3.9% across the same two
sessions where -7 moved +21%.  Every cross-codec speed ratio in README.md was
therefore built by dividing one session's numerator by another session's
denominator, and none of them are trustworthy.

The fix is not a better estimator.  It is measuring everything once, together.

METHODOLOGY -- deliberately identical to the published runs, because changing
the protocol while fixing the session problem would confound the two:

  * gen archives the whole directory in one go, as the tool is actually used.
  * reference codecs run per file and their sizes and times are summed, which
    is how PER_FILE_ZPAQ in final_bench.py was produced.

The obvious worry is that this gives gen cross-file context the per-file codecs
do not get.  Measured, it is worth 1,100 bytes out of 35.6 million -- 0.003%
(perfile_sizes.py) -- because each member starts its segment sequence fresh and
at 64 MB segments the twelve members barely share model state.  The asymmetry
is inherited from the published numbers on purpose, so this run stays
comparable to them, and it costs nothing worth correcting.

ORDERING.  Cost is deliberately not monotonic in run order, so position and
expense are decorrelated.  Two sentinels -- `-7` and zpaq -- run at both the
start and the end; if the machine drifts over the two hours, the pair of
readings for each says so directly instead of leaving it to be assumed.

  python bench_session.py [--out bench_session.json] [--quick]
"""
import os, re, sys, json, time, shutil, hashlib, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SIL  = os.path.join(HERE, "tools", "corpora", "silesia")
TMP  = os.path.join(os.environ.get("TEMP", "/tmp"), "bsession")
GEN  = os.path.join(HERE, "genv2.exe")
ZPAQ = os.path.join(HERE, "tools", "zpaq", "zpaq64.exe")
LPAQ = os.path.join(HERE, "tools", "lpaq1.exe")
PEAK = re.compile(r"([\d.]+)\s*MB peak")

FILES = ["dickens", "mozilla", "mr", "nci", "ooffice", "osdb",
         "reymont", "sao", "samba", "webster", "x-ray", "xml"]

# (kind, label).  Interleaved so that expensive and cheap alternate and no
# codec family runs as a contiguous block.
PLAN = [
    ("gen", "7"),        ("ref", "zpaq"),      # opening sentinels
    ("gen", "9"),        ("ref", "gzip"),
    ("gen", "f1"),       ("ref", "xz"),
    ("gen", "5"),        ("ref", "bzip2"),
    ("gen", "1"),        ("ref", "lpaq1"),
    ("gen", "3"),        ("ref", "brotli"),
    ("gen", "f2"),
    ("gen", "2"),
    ("gen", "7"),        ("ref", "zpaq"),      # closing sentinels
]

PIPES = {
    "xz":     ("xz -9e -c",        "xz -d -c"),
    "brotli": ("brotli -q 11 -c",  "brotli -d -c"),
    "bzip2":  ("bzip2 -9 -c",      "bzip2 -d -c"),
    "gzip":   ("gzip -9 -c",       "gzip -d -c"),
}
LABEL = {"xz": "xz -9e", "brotli": "brotli -q11", "bzip2": "bzip2 -9",
         "gzip": "gzip -9", "zpaq": "zpaq -m5", "lpaq1": "lpaq1 -6"}


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def timed(cmd, shell=False):
    t = time.perf_counter()
    p = subprocess.run(cmd, shell=shell, capture_output=True)
    return time.perf_counter() - t, p


def pipe_file(src, dst, cmd):
    """Run `cmd` with src on stdin and dst on stdout, timing only the child."""
    with open(src, "rb") as i, open(dst, "wb") as o:
        t = time.perf_counter()
        p = subprocess.run(cmd, shell=True, stdin=i, stdout=o,
                           stderr=subprocess.DEVNULL)
        dt = time.perf_counter() - t
    if p.returncode != 0:
        sys.exit("failed: " + cmd)
    return dt


def run_gen(lvl, base):
    arc = os.path.join(TMP, "s.gen")
    ext = os.path.join(TMP, "ext")
    ct, p = timed([GEN, "c", "-" + lvl, "-t1", arc, SIL])
    if p.returncode != 0:
        sys.exit(f"gen -{lvl} compress failed\n" +
                 p.stderr.decode("utf-8", "replace"))
    err = p.stderr.decode("utf-8", "replace")
    m = PEAK.search(err)
    cmem = float(m.group(1)) if m else 0.0
    size = os.path.getsize(arc)

    shutil.rmtree(ext, ignore_errors=True)
    os.makedirs(ext)
    dt, p = timed([GEN, "d", "-t1", arc, ext])
    if p.returncode != 0:
        # gen verifies SHA-256 per member on extract, so a non-zero exit here
        # invalidates the row rather than just its timing.
        sys.exit(f"gen -{lvl} EXTRACT FAILED\n" +
                 p.stderr.decode("utf-8", "replace"))
    m = PEAK.search(p.stderr.decode("utf-8", "replace"))
    dmem = float(m.group(1)) if m else 0.0
    os.remove(arc)
    shutil.rmtree(ext, ignore_errors=True)
    return dict(size=size, ctime=ct, dtime=dt, cmem=cmem, dmem=dmem, ok=True)


def run_ref(name, base, files):
    """Per file, summed -- matching how the published reference rows were made."""
    tot = ct = dt = 0.0
    tot = 0
    ok = True
    for fn in files:
        src = os.path.join(SIL, fn)
        c = os.path.join(TMP, "r.c")
        o = os.path.join(TMP, "r.o")
        if name == "zpaq":
            if os.path.exists(c):
                os.remove(c)
            a, p = timed([ZPAQ, "a", c, src, "-m5", "-t1"])
            if p.returncode != 0:
                sys.exit("zpaq failed on " + fn)
            shutil.rmtree(os.path.join(TMP, "zx"), ignore_errors=True)
            b, p = timed([ZPAQ, "x", c, "-to", os.path.join(TMP, "zx"), "-t1"])
        elif name == "lpaq1":
            a, p = timed([LPAQ, "6", src, c])
            if p.returncode != 0:
                sys.exit("lpaq1 failed on " + fn)
            b, p = timed([LPAQ, "d", c, o])
            ok &= (sha(src) == sha(o))
        else:
            cc, dd = PIPES[name]
            a = pipe_file(src, c, cc)
            b = pipe_file(c, o, dd)
            ok &= (sha(src) == sha(o))
        tot += os.path.getsize(c)
        ct += a
        dt += b
    return dict(size=tot, ctime=ct, dtime=dt, cmem=0.0, dmem=0.0, ok=ok)


def main():
    global SIL
    out = "bench_session.json"
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    quick = "--quick" in sys.argv
    files = ["dickens", "xml"] if quick else FILES

    shutil.rmtree(TMP, ignore_errors=True)
    os.makedirs(TMP)

    if quick:
        # gen always archives a whole directory, so limiting only the reference
        # file list would leave every gen row running the full corpus and the
        # "quick" check would cost as much as the real run.  Point gen at a
        # directory holding just the sample instead.
        small = os.path.join(TMP, "corpus")
        os.makedirs(small)
        for f in files:
            shutil.copy2(os.path.join(SIL, f), os.path.join(small, f))
        SIL = small

    base = sum(os.path.getsize(os.path.join(SIL, f)) for f in files)

    res = {"base": base, "files": files, "started": time.strftime("%Y-%m-%d %H:%M"),
           "rows": []}
    print(f"single-session benchmark, {len(files)} files, {base:,} bytes")
    print(f"{'#':>3} {'what':<14}{'output':>13}{'ratio':>7}{'comp':>9}"
          f"{'decomp':>9}{'RSS':>7}")
    t0 = time.perf_counter()

    for n, (kind, name) in enumerate(PLAN, 1):
        if kind == "gen":
            # gen always archives the whole directory; --quick still uses it,
            # so --quick rows are not comparable to full rows.  It exists to
            # shake out the plumbing, not to produce numbers.
            r = run_gen(name, base)
            label = "gen -" + name
        else:
            r = run_ref(name, base, files)
            label = LABEL[name]
        r.update(n=n, label=label, kind=kind)
        res["rows"].append(r)
        json.dump(res, open(os.path.join(HERE, out), "w"), indent=1)
        print(f"{n:>3} {label:<14}{r['size']:>13,}{base/r['size']:>6.2f}x"
              f"{r['ctime']:>8.1f}s{r['dtime']:>8.1f}s"
              f"{r['cmem']:>6.0f}M{'' if r['ok'] else '  VERIFY FAILED'}",
              flush=True)

    print(f"\ntotal wall {time.perf_counter()-t0:.0f}s")

    # sentinels: same work at both ends of the run
    for label in ("gen -7", "zpaq -m5"):
        v = [r for r in res["rows"] if r["label"] == label]
        if len(v) == 2:
            a, b = v[0]["ctime"], v[1]["ctime"]
            print(f"drift sentinel {label:<10} {a:7.1f}s -> {b:7.1f}s "
                  f"({(b-a)*100/a:+.2f}%)")
    print("BENCH_SESSION_DONE")


main()
