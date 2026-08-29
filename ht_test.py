"""speedup.md Tier 4.1: is the inner loop latency-bound or resource-bound?

Run one compressor pinned to one logical CPU, then two pinned to the two SMT
siblings of the *same* physical core, and compare aggregate throughput.  If a
core running two independent streams gets close to twice the work done, its
execution resources were idle waiting on dependent loads, and interleaving two
chunks in software would buy most of that without a second process.  If it
gets barely more than one stream's worth, the loads are saturating the core's
miss queue and there is nothing to reclaim.

speedup.md's own thresholds: >= 1.6x is worth pursuing, < 1.2x drops the idea.

This measures throughput, not latency: neither stream gets faster.  It only
says whether the hardware has room.

  python ht_test.py 9,f1 dickens
"""
import os, sys, time, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ.get("TEMP", "/tmp"), "httest")
os.makedirs(TMP, exist_ok=True)
EXE = os.path.join(HERE, os.environ.get("HTEXE", "g26a.exe"))

# Windows numbers SMT siblings consecutively, so CPUs 0 and 1 are the two
# threads of physical core 0.  Pinning both runs there is the whole point --
# putting them on separate cores would just measure two cores.
SIB = (0x1, 0x2)


def spawn(src, lv, out, mask):
    """Start one pinned compressor.  'start /affinity' is a cmd builtin, and
    it needs the mask in hex without 0x."""
    cmd = (f'start "" /affinity {mask:X} /wait /b "{EXE}" c -{lv} -t1 '
           f'"{src}" "{out}"')
    return subprocess.Popen(cmd, shell=True,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def run(src, lv, n):
    outs = [os.path.join(TMP, f"o{i}.c") for i in range(n)]
    for o in outs:
        if os.path.exists(o):
            os.remove(o)
    t = time.perf_counter()
    ps = [spawn(src, lv, outs[i], SIB[i]) for i in range(n)]
    for p in ps:
        p.wait()
    dt = time.perf_counter() - t
    for o in outs:
        if not os.path.exists(o) or os.path.getsize(o) == 0:
            raise SystemExit("no output at -" + str(lv))
        os.remove(o)
    return dt


def main():
    levels = (sys.argv[1] if len(sys.argv) > 1 else "9,f1").split(",")
    fname = sys.argv[2] if len(sys.argv) > 2 else "dickens"
    src = os.path.join(SIL, fname)
    n = os.path.getsize(src)
    print(f"SMT pairing test on {fname} ({n:,} B), {os.path.basename(EXE)}")
    print(f"  {'preset':>7}{'1 stream':>11}{'2 streams':>12}"
          f"{'aggregate':>11}   verdict")
    for lv in levels:
        # A-B-A-B: one, two, one, two, and take the better of each, so a
        # transient elsewhere on the machine cannot decide the answer.
        t1a = run(src, lv, 1)
        t2a = run(src, lv, 2)
        t1b = run(src, lv, 1)
        t2b = run(src, lv, 2)
        t1 = min(t1a, t1b)
        t2 = min(t2a, t2b)
        agg = 2 * t1 / t2                     # work done per unit time, vs one
        v = ("worth pursuing" if agg >= 1.6
             else ("drop it" if agg < 1.2 else "marginal"))
        print(f"  {'-' + lv:>7}{t1:>10.1f}s{t2:>11.1f}s{agg:>10.2f}x   {v}",
              flush=True)
    print("HT_DONE")


main()
