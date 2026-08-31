"""Rebuild gen15 across a parameter grid and report compressed size on a
text-heavy subset.  Sizes only -- round-trip is checked once at the end on the
chosen point, since the parameters are symmetric between encode and decode."""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
SL = os.path.join(HERE, "slices")
TMP = os.path.join(os.environ["TEMP"], "swp")
os.makedirs(TMP, exist_ok=True)
EXE = os.path.join(HERE, os.environ.get("SWEEPEXE", "gen15.exe"))
FILES = sys.argv[1].split(",") if len(sys.argv) > 1 else \
    ["webster", "dickens", "reymont", "samba", "osdb", "nci"]

BUILD = ("gcc -O3 -march=native -funroll-loops -ffast-math "
         "-I tools/zlib-1.3.1 {D} -o {E}.exe {E}.c "
         "tools/zlib-1.3.1/libz.a -lm")


def total(defs):
    d = " ".join(f"-D{k}={v}" for k, v in defs)
    r = subprocess.run(BUILD.format(D=d, E=os.environ.get("SWEEPSRC","gen15")), shell=True, cwd=HERE,
                       capture_output=True)
    if r.returncode:
        return None, r.stderr.decode()[:200]
    t = 0
    for f in FILES:
        c = os.path.join(TMP, "a.c")
        for attempt in range(4):        # freshly linked exes get AV-scanned
            r = subprocess.run(f'"{EXE}" c -9 "{os.path.join(SL, f)}" "{c}"',
                               shell=True, capture_output=True)
            if os.path.exists(c):
                break
            time.sleep(1.0)
        else:
            return None, f"{f}: rc={r.returncode} {r.stderr.decode()[:120]}"
        t += os.path.getsize(c)
        os.remove(c)
    return t, None


def main():
    grid = eval(sys.argv[2]) if len(sys.argv) > 2 else \
        [[("ILR0", v)] for v in (11, 12, 13, 14, 15)]
    best = None
    for defs in grid:
        t, err = total(defs)
        lbl = " ".join(f"{k}={v}" for k, v in defs)
        if t is None:
            print(f"{lbl:<34} BUILD FAIL {err}", flush=True)
            continue
        if best is None:
            base = t
        mark = ""
        if best is None or t < best[0]:
            best = (t, lbl); mark = "  <<"
        print(f"{lbl:<34}{t:>11,}{(t-base)*100/base:>+8.3f}%{mark}", flush=True)
    print(f"\nbest: {best[1]}  {best[0]:,}")


main()
