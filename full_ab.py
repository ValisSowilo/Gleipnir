"""A/B two builds on full-size Silesia files, with zpaq's number alongside so
the remaining gap is visible per file."""
import os, subprocess, time, hashlib, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ["TEMP"], "fullab")
os.makedirs(TMP, exist_ok=True)

ZPAQ = {"xml": 326953, "ooffice": 1766556, "reymont": 956505, "sao": 3899264,
        "x-ray": 3669707, "mr": 2181316, "osdb": 2204747, "dickens": 2094749,
        "samba": 3053826, "nci": 1251114, "webster": 5666838,
        "mozilla": 12041061}
ORDER = ["xml", "ooffice", "reymont", "sao", "x-ray", "mr", "osdb",
         "dickens", "samba", "nci", "webster", "mozilla"]


def run(cmd):
    t = time.perf_counter()
    subprocess.run(cmd, shell=True, capture_output=True)
    return time.perf_counter() - t


def one(exe, src, args, rt):
    c, o = os.path.join(TMP, "a.c"), os.path.join(TMP, "a.o")
    tc = run(f'"{exe}" c {args} "{src}" "{c}"')
    sz = os.path.getsize(c) if os.path.exists(c) else -1
    ok = True
    if rt:
        run(f'"{exe}" d "{c}" "{o}"')
        h = hashlib.sha256()
        with open(src, "rb") as f:
            for b in iter(lambda: f.read(1 << 20), b""):
                h.update(b)
        g = hashlib.sha256()
        if os.path.exists(o):
            with open(o, "rb") as f:
                for b in iter(lambda: f.read(1 << 20), b""):
                    g.update(b)
        ok = g.hexdigest() == h.hexdigest()
    for p in (c, o):
        if os.path.exists(p):
            os.remove(p)
    return sz, tc, ok


def main():
    base, cand = (os.path.join(HERE, a) for a in sys.argv[1:3])
    files = sys.argv[3:] or ORDER
    args = os.environ.get("ARGS", "-9")
    rt = os.environ.get("RT", "1") == "1"
    print(f"{'file':<9}{'base':>11}{'cand':>11}{'delta':>10}{'%':>8}"
          f"{'zpaq':>11}{'vs zpaq':>10}{'bt':>7}{'ct':>7}  rt")
    tb = tc_ = tz = 0
    tbt = tct = 0.0
    for f in files:
        src = os.path.join(SIL, f)
        if not os.path.exists(src):
            continue
        b, bt, _ = one(base, src, args, False)
        c, ct, ok = one(cand, src, args, rt)
        z = ZPAQ[f]
        tb += b; tc_ += c; tz += z; tbt += bt; tct += ct
        print(f"{f:<9}{b:>11,}{c:>11,}{c-b:>+10,}{(c-b)*100/b:>+8.2f}"
              f"{z:>11,}{(c-z)*100/z:>+9.2f}%{bt:>6.0f}s{ct:>6.0f}s  "
              f"{'OK' if ok else 'FAIL'}", flush=True)
    print(f"{'TOTAL':<9}{tb:>11,}{tc_:>11,}{tc_-tb:>+10,}{(tc_-tb)*100/tb:>+8.2f}"
          f"{tz:>11,}{(tc_-tz)*100/tz:>+9.2f}%{tbt:>6.0f}s{tct:>6.0f}s")
    print(f"gap to zpaq: base {tb-tz:+,}   cand {tc_-tz:+,}")


main()
