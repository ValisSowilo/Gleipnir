"""A/B two builds on the slice set: size delta and round-trip check.
Usage: cmp_gen.py baseline.exe candidate.exe [file ...]"""
import os, subprocess, time, hashlib, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
SL = os.path.join(HERE, "slices")
TMP = os.path.join(os.environ.get("TEMP", HERE), "cmpgen")
os.makedirs(TMP, exist_ok=True)

FILES = ["webster", "dickens", "nci", "osdb", "xml", "reymont",
         "samba", "sao", "x-ray", "mr", "ooffice", "mozilla", "enwik8"]


def run(cmd):
    t = time.perf_counter()
    subprocess.run(cmd, shell=True, capture_output=True)
    return time.perf_counter() - t


def one(exe, src, lvl, want_rt):
    c, o = os.path.join(TMP, "a.c"), os.path.join(TMP, "a.o")
    tc = run(f'"{exe}" c -{lvl} "{src}" "{c}"')
    sz = os.path.getsize(c) if os.path.exists(c) else -1
    ok, td = True, 0.0
    if want_rt:
        td = run(f'"{exe}" d "{c}" "{o}"')
        ref = hashlib.sha256(open(src, "rb").read()).hexdigest()
        ok = os.path.exists(o) and hashlib.sha256(open(o, "rb").read()).hexdigest() == ref
    for p in (c, o):
        if os.path.exists(p):
            os.remove(p)
    return sz, tc, td, ok


def main():
    base = os.path.join(HERE, sys.argv[1])
    cand = os.path.join(HERE, sys.argv[2])
    files = sys.argv[3:] or FILES
    lvl = int(os.environ.get("LVL", "9"))
    rt = os.environ.get("RT", "1") == "1"
    print(f"{'file':<10}{'base':>11}{'cand':>11}{'delta':>10}{'%':>8}"
          f"{'bt':>7}{'ct':>7}  rt")
    tb = tc_ = 0
    tbt = tct = 0.0
    for f in files:
        src = os.path.join(SL, f)
        if not os.path.exists(src):
            continue
        b, bt, _, _ = one(base, src, lvl, False)
        c, ct, dt, ok = one(cand, src, lvl, rt)
        tb += b; tc_ += c; tbt += bt; tct += ct
        print(f"{f:<10}{b:>11,}{c:>11,}{c-b:>+10,}{(c-b)*100/b:>+8.2f}"
              f"{bt:>6.1f}s{ct:>6.1f}s  {'OK' if ok else 'FAIL'}", flush=True)
    print(f"{'TOTAL':<10}{tb:>11,}{tc_:>11,}{tc_-tb:>+10,}"
          f"{(tc_-tb)*100/tb:>+8.2f}{tbt:>6.1f}s{tct:>6.1f}s"
          f"   {tct/tbt:.2f}x time")


main()
