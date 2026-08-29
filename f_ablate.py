"""Where does -f1's time actually go?

-f1 landed at a fraction of its target throughput, and speedup.md's rule is
that Tier 4 only happens if the profile says why.  Guessing is worthless here:
this project has already had one ablation misattributed, because removing a
computation also removed the memory traffic feeding it.

So each row below removes exactly one *table*, holding the computation fixed
wherever possible.  Sizes move, and are printed, but the question is time.

  python f_ablate.py f1 dickens
"""
import os, sys, time, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ.get("TEMP", "/tmp"), "fablate")
os.makedirs(TMP, exist_ok=True)

BUILD = ("gcc -O3 -funroll-loops -march=native {defs} "
         "-I tools/zlib-1.3.1 -o {tag}.exe gen26.c tools/zlib-1.3.1/libz.a "
         "-lm -lpsapi")

# (label, extra -D flags, extra runtime args, what it isolates)
ARMS = [
    ("baseline",        "",                 [],        "-"),
    ("match idx 8MB->256KB", "-DFMM1=16 -DFMM2=16", [], "match-table miss/byte"),
    ("mixer 128KB->8KB",     "-DFWB=9",     [],        "weight-row traffic/bit"),
    ("ctx tables /4",   "",                 ["-m-2"],  "context-table traffic"),
    ("ctx tables /16",  "",                 ["-m-4"],  "context-table traffic"),
    ("no bypass",       "-DBYT=0",          [],        "what the bypass returns"),
]


def build(defs, tag):
    out = os.path.join(HERE, tag + ".exe")
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run(BUILD.format(defs=defs, tag=tag), shell=True,
                       capture_output=True, cwd=HERE)
    if r.returncode:
        raise SystemExit(r.stderr.decode("utf-8", "replace")[-2000:])
    for w in (1, 3, 5, 8, 12):
        time.sleep(w)
        p = subprocess.run([out], capture_output=True)
        if b"usage" in p.stdout + p.stderr:
            return out
    raise SystemExit("binary stayed blocked: " + out)


def comp(exe, src, lv, extra):
    c = os.path.join(TMP, "b.c")
    best = None
    for _ in range(2):                        # take the better of two passes
        for _ in range(6):
            if os.path.exists(c):
                os.remove(c)
            t = time.perf_counter()
            subprocess.run([exe, "c", "-" + lv] + extra + ["-t1", src, c],
                           capture_output=True)
            dt = time.perf_counter() - t
            if os.path.exists(c) and os.path.getsize(c) > 0:
                sz = os.path.getsize(c)
                break
            time.sleep(3.0)
        else:
            raise SystemExit("no output: " + exe)
        best = (sz, dt) if best is None or dt < best[1] else best
    os.remove(c)
    return best


def main():
    lv = (sys.argv[1] if len(sys.argv) > 1 else "f1").lstrip("-")
    fname = sys.argv[2] if len(sys.argv) > 2 else "dickens"
    src = os.path.join(SIL, fname)
    n = os.path.getsize(src)
    print(f"-{lv} ablation on {fname} ({n:,} B), single thread")
    print(f"  {'arm':<22}{'size':>11}{'time':>8}{'MB/s':>8}{'vs base':>9}"
          f"   isolates")
    base = None
    for i, (label, defs, extra, what) in enumerate(ARMS):
        exe = build(defs, "fab_%d" % i)
        sz, dt = comp(exe, src, lv, extra)
        if base is None:
            base = dt
        print(f"  {label:<22}{sz:>11,}{dt:>7.1f}s{n/1e6/dt:>8.2f}"
              f"{(dt - base) * 100 / base:>+8.1f}%   {what}", flush=True)
    print("ABLATE_DONE")


main()
