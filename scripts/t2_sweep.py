"""Sweep one compile-time gate across files that disagree, at one preset.

The Tier-2 gates all have the same shape: skip a refinement once some
already-shared quantity says it cannot matter.  They also all have the same
failure mode -- a threshold picked on the file where the refinement was
useless, then shipped to the file where it was the whole model.  That is
exactly how the match-bypass gate came to be 48.  So the same six-file rule
applies here, and the accept criterion is the size-vs-time frontier, not
either axis alone.

Compression only: these gates change encode and decode symmetrically by
construction, and round-trip is verified separately by fuzz.py.

  python t2_sweep.py SSEG 9 0,384,512,768,1024
  python t2_sweep.py IXIT 9 0,1
  python t2_sweep.py THIN 7 0,16,32,64
"""
import os, sys, time, subprocess

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ.get("TEMP", "/tmp"), "t2sweep")
os.makedirs(TMP, exist_ok=True)

# Text, markup, source, binary, tabular, and the one that is nearly all match.
FILES = ["dickens", "xml", "samba", "mozilla", "osdb", "nci"]

SRC = os.environ.get("T2SRC", "gen26.c")
# HERE contains a space: keep the output name relative and build with cwd=HERE.
BUILD = ("gcc -O3 -funroll-loops -march=native -D{name}={val} "
         "-I tools/zlib-1.3.1 -o {tag}.exe " + SRC + " tools/zlib-1.3.1/libz.a "
         "-lm -lpsapi")


def build(name, val, tag):
    out = os.path.join(HERE, tag + ".exe")
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run(BUILD.format(name=name, val=val, tag=tag), shell=True,
                       capture_output=True, cwd=HERE)
    if r.returncode:
        raise SystemExit(r.stderr.decode("utf-8", "replace")[-2000:])
    # Application Control blocks a freshly linked binary by hash for a few
    # seconds; a blocked run writes nothing, which must not read as a result.
    # While blocked it does not merely fail -- CreateProcess raises WinError
    # 4551 -- so the probe has to survive an exception, not just empty output.
    # An uncaught one killed a sweep after 40 minutes of good measurements.
    for w in (1, 3, 5, 8, 12, 20, 30):
        time.sleep(w)
        try:
            p = subprocess.run([out], capture_output=True)
        except OSError:
            continue
        if b"usage" in p.stdout + p.stderr:
            return out
    raise SystemExit("binary stayed blocked: " + out)


def comp(exe, src, lv):
    c = os.path.join(TMP, "b.c")
    for _ in range(6):
        if os.path.exists(c):
            os.remove(c)
        t = time.perf_counter()
        subprocess.run([exe, "c", lv, "-t1", src, c], capture_output=True)
        dt = time.perf_counter() - t
        if os.path.exists(c) and os.path.getsize(c) > 0:
            sz = os.path.getsize(c)
            os.remove(c)
            return sz, dt
        time.sleep(3.0)
    raise SystemExit("no output: " + exe)


def main():
    name = sys.argv[1]
    lv = sys.argv[2] if len(sys.argv) > 2 else "9"
    lv = "-" + lv.lstrip("-")
    vals = [x for x in (sys.argv[3] if len(sys.argv) > 3 else "0").split(",")]
    files = sys.argv[4].split(",") if len(sys.argv) > 4 else FILES

    print(f"{name} sweep at {lv}, single thread, {SRC}")
    print(f"  {vals[0]} is the baseline; 0 means the gate is off\n")

    base, rows = {}, []
    for v in vals:
        exe = build(name, v, f"t2_{name}_{v}".replace("-", "m"))
        tot, tt, cells = 0, 0.0, []
        for f in files:
            sz, dt = comp(exe, os.path.join(SIL, f), lv)
            tot += sz
            tt += dt
            if v == vals[0]:
                base[f] = (sz, dt)
            cells.append((f, sz, dt))
        rows.append((v, tot, tt, cells))
        d = " ".join(f"{f}:{(sz - base[f][0]) * 100 / base[f][0]:+.2f}%"
                     for f, sz, _ in cells)
        print(f"  {name}={v:>5}  total {tot:>11,}  {tt:>7.1f}s   {d}", flush=True)

    b_tot, b_t = rows[0][1], rows[0][2]
    print(f"\n  {name:>6}{'total':>13}{'vs ' + vals[0]:>11}{'time':>9}"
          f"{'vs':>9}   worst file")
    for v, tot, tt, cells in rows:
        worst = max(cells, key=lambda c: (c[1] - base[c[0]][0]) / base[c[0]][0])
        wpct = (worst[1] - base[worst[0]][0]) * 100 / base[worst[0]][0]
        print(f"  {v:>6}{tot:>13,}{(tot - b_tot) * 100 / b_tot:>+10.3f}%"
              f"{tt:>8.1f}s{(tt - b_t) * 100 / b_t:>+8.1f}%   "
              f"{worst[0]} {wpct:+.2f}%")
    print("T2_SWEEP_DONE")


main()
