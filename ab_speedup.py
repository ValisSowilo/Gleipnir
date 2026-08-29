"""Same-session A/B of the two speed changes that landed in gen.c on 2026-07-30:
the active-context list and the match bypass.

  old = gen_base.exe   (built from gen_pre_speedup_backup.c)
  new = genb1.exe      (built from gen.c)

Files chosen to separate the two effects: dickens is prose with few long
matches, so it shows the active-context list almost alone; samba and mozilla
are archives full of repeated content, where the bypass does the work.

Runs strictly sequentially -- a concurrent job once produced a phantom 8%
regression in this project's history.

  python ab_speedup.py [level] [file,file,...]
"""
import os, sys, time, subprocess, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ.get("TEMP", "/tmp"), "abspeed")
os.makedirs(TMP, exist_ok=True)

OLD = os.path.join(HERE, "gen_base.exe")
NEW = os.path.join(HERE, "genb1.exe")


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(exe, args, expect):
    """Delete first, then require a fresh non-empty file -- a blocked run
    otherwise leaves the previous output in place and becomes a bogus point."""
    for _ in range(6):
        if os.path.exists(expect):
            os.remove(expect)
        t = time.perf_counter()
        subprocess.run([exe] + args, capture_output=True)
        dt = time.perf_counter() - t
        if os.path.exists(expect) and os.path.getsize(expect) > 0:
            return dt
        time.sleep(3.0)
    raise SystemExit("no output after retries: " + exe + " " + " ".join(args))


def one(exe, src, lv, tag):
    c = os.path.join(TMP, tag + ".c")
    o = os.path.join(TMP, tag + ".o")
    ct = run(exe, ["c", f"-{lv}", "-t1", src, c], c)
    dt = run(exe, ["d", "-t1", c, o], o)
    sz = os.path.getsize(c)
    ok = sha(o) == sha(src)
    for p in (c, o):
        if os.path.exists(p):
            os.remove(p)
    return sz, ct, dt, ok


def main():
    lv = int(sys.argv[1]) if len(sys.argv) > 1 else 9
    files = (sys.argv[2].split(",") if len(sys.argv) > 2
             else ["dickens", "samba", "mozilla"])
    print(f"active-context list + match bypass, level -{lv}, single thread")
    print(f"  {'file':<9}{'old':>11}{'new':>11}{'size':>9}"
          f"{'old s':>9}{'new s':>9}{'time':>9}  ok")
    tso = tsn = 0
    tto = ttn = 0.0
    for f in files:
        src = os.path.join(SIL, f)
        if not os.path.exists(src):
            continue
        so, co, do, ko = one(OLD, src, lv, "old")
        sn, cn, dn, kn = one(NEW, src, lv, "new")
        tso += so; tsn += sn; tto += co; ttn += cn
        print(f"  {f:<9}{so:>11,}{sn:>11,}{(sn - so) * 100 / so:>+8.3f}%"
              f"{co:>8.1f}s{cn:>8.1f}s{(cn - co) * 100 / co:>+8.1f}%"
              f"  {'OK' if ko and kn else 'FAIL'}", flush=True)
    if tso:
        print(f"  {'TOTAL':<9}{tso:>11,}{tsn:>11,}"
              f"{(tsn - tso) * 100 / tso:>+8.3f}%"
              f"{tto:>8.1f}s{ttn:>8.1f}s{(ttn - tto) * 100 / tto:>+8.1f}%")


main()
