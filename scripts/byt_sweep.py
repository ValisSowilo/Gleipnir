"""Sweep the match-bypass gate at one preset, across files that disagree.

The shipped gates were chosen on samba alone.  On the full corpus at -1 that
choice costs +14.28% on nci and +5.63% on xml -- both are highly repetitive, so
the bypass fires almost continuously and the match StateMap alone is a much
worse model there than the full mixer.  A gate has to be picked against the
files that lose, not the ones that win.

Compression only: the gate changes size and encode time, and decode time tracks
encode to within a percent by construction.  Round-trip is verified separately
by fuzz.py once a gate is chosen.

  python byt_sweep.py 1 0,48,96,192,400
"""
import os, sys, time, subprocess

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ.get("TEMP", "/tmp"), "bytsweep")
os.makedirs(TMP, exist_ok=True)

# nci/xml/osdb regress, samba/mozilla gain, dickens is near-neutral.
FILES = ["nci", "xml", "osdb", "samba", "dickens", "mozilla"]

"""HERE contains a space, so the output name stays relative and the build runs
with cwd=HERE -- an absolute path here reaches the linker unquoted."""
BUILD = ("gcc -O3 -funroll-loops -march=native -DBYT={g} "
         "-I tools/zlib-1.3.1 -o byt_{tag}.exe gen.c tools/zlib-1.3.1/libz.a "
         "-lm -lpsapi")


def build(gate, tag):
    out = os.path.join(HERE, f"byt_{tag}.exe")
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run(BUILD.format(g=gate, tag=tag), shell=True,
                       capture_output=True, cwd=HERE)
    if r.returncode:
        raise SystemExit(r.stderr.decode("utf-8", "replace")[-2000:])
    # Application Control blocks freshly linked binaries by hash for a moment;
    # a blocked run produces no output at all, which must not read as a result.
    for w in (1, 3, 5, 8, 12):
        time.sleep(w)
        p = subprocess.run([out], capture_output=True)
        if b"usage" in p.stdout + p.stderr:
            return out
    raise SystemExit("binary stayed blocked: " + out)


def comp(exe, src, lv):
    c = os.path.join(TMP, "b.c")
    for _ in range(6):
        if os.path.exists(c):
            os.remove(c)
        t = time.perf_counter()
        subprocess.run([exe, "c", f"-{lv}", "-t1", src, c], capture_output=True)
        dt = time.perf_counter() - t
        if os.path.exists(c) and os.path.getsize(c) > 0:
            sz = os.path.getsize(c)
            os.remove(c)
            return sz, dt
        time.sleep(3.0)
    raise SystemExit("no output: " + exe)


def main():
    lv = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    gates = [int(x) for x in (sys.argv[2] if len(sys.argv) > 2
                              else "0,48,96,192,400").split(",")]
    files = sys.argv[3].split(",") if len(sys.argv) > 3 else FILES

    print(f"match-bypass gate sweep at -{lv}, single thread")
    print("  gate 0 disables the bypass entirely (the pre-change model)\n")

    base = {}
    rows = []
    for g in gates:
        exe = build(g, str(g))
        tot = 0
        tt = 0.0
        cells = []
        for f in files:
            src = os.path.join(SIL, f)
            sz, dt = comp(exe, src, lv)
            tot += sz
            tt += dt
            if g == gates[0]:
                base[f] = (sz, dt)
            cells.append((f, sz, dt))
        rows.append((g, tot, tt, cells))
        d = " ".join(f"{f}:{(sz - base[f][0]) * 100 / base[f][0]:+.2f}%"
                     for f, sz, _ in cells)
        print(f"  gate {g:>4}  total {tot:>11,}  {tt:>7.1f}s   {d}", flush=True)

    b_tot, b_t = rows[0][1], rows[0][2]
    print(f"\n  {'gate':>5}{'total':>13}{'vs gate ' + str(gates[0]):>12}"
          f"{'time':>9}{'vs':>9}   worst file")
    for g, tot, tt, cells in rows:
        worst = max(cells, key=lambda c: (c[1] - base[c[0]][0]) / base[c[0]][0])
        wpct = (worst[1] - base[worst[0]][0]) * 100 / base[worst[0]][0]
        print(f"  {g:>5}{tot:>13,}{(tot - b_tot) * 100 / b_tot:>+11.3f}%"
              f"{tt:>8.1f}s{(tt - b_t) * 100 / b_t:>+8.1f}%   "
              f"{worst[0]} {wpct:+.2f}%")


main()
