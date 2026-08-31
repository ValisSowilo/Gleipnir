"""Before/after on the full corpus, interleaved per file, in one session.

Two builds, every preset, whole Silesia, old and new run back to back on *each
file* before moving on.

Interleaving per preset was tried first and is not enough.  A corpus pass at
-9 takes a quarter of an hour, so the two arms of a preset ended up fifteen
minutes apart -- and this machine drifts monotonically over that scale even
with nothing else running.  It showed -1.7% at -7, where a controlled A-B-A-B
sweep of the same change measures -7.3%, and -25.0% at -9, where the parts sum
to -18%.  Per-file interleaving puts the two arms seconds to a couple of
minutes apart instead.

Sizes are deterministic and were never in doubt; this is entirely about making
the time axis mean something.

The new build is also decompressed and hashed against the source, so one run
produces the comparison, the decode times and the round-trip check together.

Presets the old build does not have (the -f rungs) are run on the new build
alone and reported with no counterpart.

  python ab_corpus.py gen.exe gen26.exe f1,f2,1,2,3,5,7,9
"""
import os, re, sys, json, time, subprocess, hashlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ.get("TEMP", "/tmp"), "abcorpus")
os.makedirs(TMP, exist_ok=True)

ORDER = ["xml", "ooffice", "reymont", "sao", "x-ray", "mr", "osdb",
         "dickens", "samba", "nci", "webster", "mozilla"]
NEW_ONLY = ("f1", "f2")
PEAK = re.compile(r"([\d.]+)\s*MB peak")


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def once(cmd, expect):
    """A blocked run leaves the previous output in place, which silently turns
    into a bogus datapoint -- so delete first and demand a fresh file."""
    for _ in range(6):
        if os.path.exists(expect):
            os.remove(expect)
        t = time.perf_counter()
        r = subprocess.run(cmd, capture_output=True)
        dt = time.perf_counter() - t
        if os.path.exists(expect) and os.path.getsize(expect) > 0:
            m = PEAK.search(r.stderr.decode("utf-8", "replace"))
            return dt, (float(m.group(1)) if m else 0.0)
        time.sleep(3.0)
    raise SystemExit("no output: " + " ".join(cmd))


def blank(base=0):
    return {"total": 0, "ctime": 0.0, "dtime": 0.0, "cmem": 0.0, "dmem": 0.0,
            "base": base, "rows": {}, "ok": True}


def add(acc, f, n, sz, ct, dt, cm, dm, ok):
    acc["rows"][f] = {"size": sz, "ctime": ct, "dtime": dt, "insize": n,
                      "ok": ok}
    acc["total"] += sz
    acc["ctime"] += ct
    acc["dtime"] += dt
    acc["base"] += n
    acc["cmem"] = max(acc["cmem"], cm)
    acc["dmem"] = max(acc["dmem"], dm)
    acc["ok"] = acc["ok"] and ok


def main():
    old = os.path.join(HERE, sys.argv[1] if len(sys.argv) > 1 else "gen.exe")
    new = os.path.join(HERE, sys.argv[2] if len(sys.argv) > 2 else "gen26.exe")
    levels = (sys.argv[3] if len(sys.argv) > 3
              else "f1,f2,1,2,3,5,7,9").split(",")
    # Only for smoke-testing this harness; a reported comparison uses the
    # whole corpus.
    files = sys.argv[4].split(",") if len(sys.argv) > 4 else ORDER

    out = {"old_exe": os.path.basename(old), "new_exe": os.path.basename(new),
           "old": {}, "new": {}}
    print(f"{os.path.basename(old)} vs {os.path.basename(new)}, "
          f"full Silesia, single thread, interleaved")
    print(f"  {'preset':>7}{'old':>12}{'new':>12}{'size':>9}"
          f"{'old s':>9}{'new s':>9}{'time':>9}{'MB/s':>8}")
    oc = os.path.join(TMP, "o.c")
    nc = os.path.join(TMP, "n.c")
    nd = os.path.join(TMP, "n.out")

    for lv in levels:
        has_old = lv not in NEW_ONLY
        o = blank() if has_old else None
        n = blank()
        for f in files:
            src = os.path.join(SIL, f)
            if not os.path.exists(src):
                continue
            sz = os.path.getsize(src)
            # Old and new on the same file, back to back, before anything else
            # touches the machine.  This adjacency is the whole design.
            if has_old:
                oct_, ocm = once([old, "c", "-" + lv, "-t1", src, oc], oc)
                osz = os.path.getsize(oc)
                os.remove(oc)
                add(o, f, sz, osz, oct_, 0.0, ocm, 0.0, True)
            nct, ncm = once([new, "c", "-" + lv, "-t1", src, nc], nc)
            nsz = os.path.getsize(nc)
            ndt, ndm = once([new, "d", "-t1", nc, nd], nd)
            ok = sha(nd) == sha(src)
            os.remove(nc)
            os.remove(nd)
            add(n, f, sz, nsz, nct, ndt, ncm, ndm, ok)
        if not n["base"]:
            continue
        out["new"][lv] = n
        if has_old:
            out["old"][lv] = o
        base = n["base"]
        if not n["ok"]:
            print(f"  !! round trip FAILED at -{lv}", flush=True)
        if o:
            print(f"  {'-' + lv:>7}{o['total']:>12,}{n['total']:>12,}"
                  f"{(n['total']-o['total'])*100/o['total']:>+8.3f}%"
                  f"{o['ctime']:>8.1f}s{n['ctime']:>8.1f}s"
                  f"{(n['ctime']-o['ctime'])*100/o['ctime']:>+8.1f}%"
                  f"{base/1e6/n['ctime']:>8.3f}", flush=True)
        else:
            print(f"  {'-' + lv:>7}{'-':>12}{n['total']:>12,}{'-':>9}"
                  f"{'-':>9}{n['ctime']:>8.1f}s{'new':>9}"
                  f"{base/1e6/n['ctime']:>8.3f}", flush=True)
        # Written after every preset: an interrupted run used to lose
        # everything it had already measured.
        json.dump(out, open(os.path.join(HERE, "bench_ab.json"), "w"), indent=1)
    print("\nwrote bench_ab.json")
    print("AB_CORPUS_DONE")


main()
