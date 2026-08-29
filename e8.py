"""enwik8 across presets on the current engine, both directions, round-trip
verified.  One uninterrupted run so the rows are comparable to each other."""
import os, sys, time, hashlib, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "tools", "corpora", "enwik8")
TMP = os.path.join(os.environ["TEMP"], "e8"); os.makedirs(TMP, exist_ok=True)
EXE = os.path.join(HERE, os.environ.get("BENCHEXE", "genf1.exe"))
N = os.path.getsize(SRC)

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()

def run(args, expect):
    for _ in range(6):
        if os.path.exists(expect): os.remove(expect)
        t = time.perf_counter()
        subprocess.run([EXE] + args, capture_output=True)
        dt = time.perf_counter() - t
        if os.path.exists(expect) and os.path.getsize(expect) > 0: return dt
        time.sleep(3)
    raise SystemExit("no output")

levels = [int(x) for x in (sys.argv[1] if len(sys.argv) > 1
                           else "1,3,5,7,9").split(",")]
src_hash = sha(SRC)
print(f"enwik8 ({N:,} bytes), single thread, one run")
print(f"  {'lvl':>4}{'output':>12}{'bpc':>8}{'comp':>9}{'decomp':>9}  ok")
c = os.path.join(TMP, "e.c"); o = os.path.join(TMP, "e.o")
for lv in levels:
    ct = run(["c", f"-{lv}", "-t1", SRC, c], c)
    sz = os.path.getsize(c)
    dt = run(["d", "-t1", c, o], o)
    ok = sha(o) == src_hash
    print(f"  -{lv:<3}{sz:>12,}{sz*8/N:>8.4f}{ct:>8.1f}s{dt:>8.1f}s  "
          f"{'OK' if ok else 'FAIL'}", flush=True)
    for p in (c, o):
        if os.path.exists(p): os.remove(p)
print("E8_ALL_DONE")
