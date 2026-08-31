"""Pick the shape of the -f presets on the size-vs-time frontier.

The ablation showed no single dominant cost at -f1: the match index, the mixer
weight table and the context tables each account for 8-15% of the time, and
each of them individually trades size for time at a *better* rate than the
preset ladder's own local slope.  That means the combination is worth
measuring, and that the answer is a frontier position rather than a winner.

Four files that disagree about what matters -- prose, markup, a source tree,
and a binary -- because a throughput preset tuned on text alone is how you
ship something that collapses on executables.

  python fshape.py f1
"""
import os, sys, time, subprocess

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ.get("TEMP", "/tmp"), "fshape")
os.makedirs(TMP, exist_ok=True)

FILES = ["dickens", "xml", "samba", "mozilla"]

BUILD = ("gcc -O3 -funroll-loops -march=native {defs} "
         "-I tools/zlib-1.3.1 -o {tag}.exe gen26.c tools/zlib-1.3.1/libz.a "
         "-lm -lpsapi")

# (label, -D flags, runtime args).  Memory shown is match index + mixer
# weights + context tables for -f1.
ARMS = [
    ("as built  8+0.1+2.4MB", "",                      []),
    ("mid       1+0.03+2.4",  "-DFMM1=18 -DFMM2=19 -DFWB=11", []),
    ("mid+small 1+0.03+0.6",  "-DFMM1=18 -DFMM2=19 -DFWB=11", ["-m-2"]),
    ("tight   0.25+0.008+2.4", "-DFMM1=16 -DFMM2=17 -DFWB=9", []),
    ("tight+small .25+.008+.6", "-DFMM1=16 -DFMM2=17 -DFWB=9", ["-m-2"]),
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
    for _ in range(6):
        if os.path.exists(c):
            os.remove(c)
        t = time.perf_counter()
        subprocess.run([exe, "c", "-" + lv] + extra + ["-t1", src, c],
                       capture_output=True)
        dt = time.perf_counter() - t
        if os.path.exists(c) and os.path.getsize(c) > 0:
            sz = os.path.getsize(c)
            os.remove(c)
            return sz, dt
        time.sleep(3.0)
    raise SystemExit("no output: " + exe)


def main():
    lv = (sys.argv[1] if len(sys.argv) > 1 else "f1").lstrip("-")
    files = sys.argv[2].split(",") if len(sys.argv) > 2 else FILES
    srcs = [(f, os.path.join(SIL, f)) for f in files]
    base_n = sum(os.path.getsize(p) for _, p in srcs)
    print(f"-{lv} shape sweep over {', '.join(files)} ({base_n:,} B)")
    print(f"  {'arm':<24}{'total':>12}{'bpc':>7}{'time':>8}{'MB/s':>8}"
          f"{'size':>9}{'time':>8}")
    b = None
    for i, (label, defs, extra) in enumerate(ARMS):
        exe = build(defs, "fsh_%d" % i)
        tot, tt = 0, 0.0
        for _, p in srcs:
            sz, dt = comp(exe, p, lv, extra)
            tot += sz
            tt += dt
        if b is None:
            b = (tot, tt)
        print(f"  {label:<24}{tot:>12,}{tot*8/base_n:>7.3f}{tt:>7.1f}s"
              f"{base_n/1e6/tt:>8.2f}{(tot-b[0])*100/b[0]:>+8.2f}%"
              f"{(tt-b[1])*100/b[1]:>+7.1f}%", flush=True)
    print("FSHAPE_DONE")


main()
