"""Benchmark the mixed-type corpus against the reference codecs.

The point is to find content types where this engine loses ground relative to
its peers, not to produce a headline number.  A file where zpaq or xz is close
to gen -9 is a file whose structure gen is failing to model.
"""
import os, sys, time, subprocess, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
MIX = os.path.join(HERE, "tools", "corpora", "mixed")
TMP = os.path.join(os.environ["TEMP"], "mixbench")
os.makedirs(TMP, exist_ok=True)
GEN = os.path.join(HERE, "gen.exe")
ZPAQ = os.path.join(HERE, "tools", "zpaq", "zpaq64.exe")
LPAQ = os.path.join(HERE, "tools", "lpaq1.exe")
XZ = "xz"


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def attempt(cmd, out, tries=8):
    for i in range(tries):
        for p in ([out] if isinstance(out, str) else out):
            if os.path.isfile(p):
                try:
                    os.remove(p)
                except OSError:
                    pass
        t = time.perf_counter()
        r = subprocess.run(cmd, shell=True, capture_output=True)
        dt = time.perf_counter() - t
        chk = out if isinstance(out, str) else out[0]
        if os.path.exists(chk) and os.path.getsize(chk) > 0:
            return dt, r
        time.sleep(3.0 * (i + 1))
    return None, None


def gen_run(src, lvl):
    c, o = os.path.join(TMP, "g.c"), os.path.join(TMP, "g.o")
    ct, r = attempt(f'"{GEN}" c -{lvl} -t1 "{src}" "{c}"', c)
    if ct is None:
        return None
    sz = os.path.getsize(c)
    dt, _ = attempt(f'"{GEN}" d -t1 "{c}" "{o}"', o)
    ok = dt is not None and sha(o) == sha(src)
    note = ""
    err = r.stderr.decode("utf-8", "replace")
    for k in ("packed", "dfl", "stored", "period", "x86", "alpha"):
        if k == "dfl" and " 0 dfl" in err:
            continue
        if k == "x86" and " 0 x86" in err:
            continue
        if k == "alpha" and " 0 alpha" in err:
            continue
        if k == "stored" and " 0 stored" in err:
            continue
        if k in err:
            note += k + " "
    for p in (c, o):
        if os.path.exists(p):
            os.remove(p)
    return sz, ct, dt, ok, note.strip()


def ref_zpaq(src):
    c = os.path.join(TMP, "z.zpaq")
    t, _ = attempt(f'"{ZPAQ}" a "{c}" "{src}" -m5 -t1', c)
    if t is None:
        return None, None
    sz = os.path.getsize(c)
    os.remove(c)
    return sz, t


def ref_xz(src):
    c = os.path.join(TMP, "x.xz")
    t, _ = attempt(f'"{XZ}" -9e -k -c "{src}" > "{c}"', c)
    if t is None:
        return None, None
    sz = os.path.getsize(c)
    os.remove(c)
    return sz, t


def ref_lpaq(src):
    c = os.path.join(TMP, "l.c")
    t, _ = attempt(f'"{LPAQ}" 6 "{src}" "{c}"', c)
    if t is None:
        return None, None
    sz = os.path.getsize(c)
    os.remove(c)
    return sz, t


def main():
    files = sorted(os.listdir(MIX))
    if len(sys.argv) > 1:
        files = sys.argv[1:]
    print(f"{'file':<14}{'input':>11}{'gen -9':>11}{'bpc':>7}"
          f"{'zpaq':>11}{'xz':>11}{'lpaq1':>11}{'best?':>8}  notes")
    for f in files:
        src = os.path.join(MIX, f)
        if not os.path.isfile(src):
            continue
        n = os.path.getsize(src)
        g = gen_run(src, 9)
        if g is None:
            print(f"{f:<14}{n:>11,}   blocked"); continue
        gs, gct, gdt, ok, note = g
        zs, _ = ref_zpaq(src)
        xs, _ = ref_xz(src)
        ls, _ = ref_lpaq(src)
        cands = {"gen": gs, "zpaq": zs, "xz": xs, "lpaq1": ls}
        cands = {k: v for k, v in cands.items() if v}
        best = min(cands, key=cands.get)
        print(f"{f:<14}{n:>11,}{gs:>11,}{gs*8/n:>7.3f}"
              f"{(zs or 0):>11,}{(xs or 0):>11,}{(ls or 0):>11,}"
              f"{best:>8}  {'' if ok else 'ROUNDTRIP-FAIL '}{note}", flush=True)


main()
