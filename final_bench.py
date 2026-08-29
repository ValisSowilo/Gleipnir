"""Final full-size Silesia run for gen16 at one and eight threads, with the
reference codecs' numbers from bench_large.log alongside so the position is
readable without cross-checking two files."""
import os, subprocess, time, hashlib, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ["TEMP"], "finalb")
os.makedirs(TMP, exist_ok=True)
GEN = os.path.join(HERE, os.environ.get("BENCHEXE", "gen17.exe"))

# measured previously, same machine, same corpus (bench_large.log)
REF = {
    "zpaq -m5":    (39112636, 629.4, 647.0),
    "gen13 -9":    (39739887, 235.4, 238.5),
    "lpaq1 -6":    (43006234, 205.4, 211.6),
    "xz -9e":      (48456004, 131.0,   2.5),
    "brotli -q11": (49564563, 431.3,   1.2),
    "bzip2 -9":    (54506769,  18.0,  10.4),
    "gzip -9":     (67631990,  17.2,   1.4),
}
PER_FILE_ZPAQ = {
    "xml": 326953, "ooffice": 1766556, "reymont": 956505, "sao": 3899264,
    "x-ray": 3669707, "mr": 2181316, "osdb": 2204747, "dickens": 2094749,
    "samba": 3053826, "nci": 1251114, "webster": 5666838, "mozilla": 12041061,
}
ORDER = ["xml", "ooffice", "reymont", "sao", "x-ray", "mr", "osdb",
         "dickens", "samba", "nci", "webster", "mozilla"]


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(cmd, expect=None):
    """Freshly linked exes are intermittently blocked by AV / Device Guard;
    retry rather than lose a half-hour run to it."""
    for _ in range(5):
        t = time.perf_counter()
        subprocess.run(cmd, shell=True, capture_output=True)
        dt = time.perf_counter() - t
        if expect is None or os.path.exists(expect):
            return dt
        time.sleep(2.0)
    raise SystemExit("command produced no output after retries: " + cmd)


def bench(t):
    tot = tc = td = 0
    rows = []
    for f in ORDER:
        src = os.path.join(SIL, f)
        if not os.path.exists(src):
            continue
        c, o = os.path.join(TMP, "a.c"), os.path.join(TMP, "a.o")
        a = run(f'"{GEN}" c -9 -t{t} "{src}" "{c}"', c)
        b = run(f'"{GEN}" d -t{t} "{c}" "{o}"', o)
        sz = os.path.getsize(c)
        ok = os.path.exists(o) and sha(o) == sha(src)
        n = os.path.getsize(src)
        z = PER_FILE_ZPAQ[f]
        rows.append((f, n, sz, z, a, b, ok))
        tot += sz; tc += a; td += b
        for p in (c, o):
            if os.path.exists(p):
                os.remove(p)
    return rows, tot, tc, td


def main():
    threads = [int(x) for x in (sys.argv[1:] or ["1", "8"])]
    base = sum(os.path.getsize(os.path.join(SIL, f)) for f in ORDER
               if os.path.exists(os.path.join(SIL, f)))
    results = {}
    for t in threads:
        rows, tot, tc, td = bench(t)
        results[t] = (tot, tc, td)
        print(f"\n=== gen16 -9 -t{t} ===", flush=True)
        print(f"{'file':<9}{'size':>12}{'out':>11}{'bpc':>7}{'zpaq -m5':>11}"
              f"{'vs zpaq':>10}{'comp':>8}{'decomp':>8}  ok")
        for f, n, sz, z, a, b, ok in rows:
            print(f"{f:<9}{n:>12,}{sz:>11,}{sz*8/n:>7.3f}{z:>11,}"
                  f"{(sz-z)*100/z:>+9.2f}%{a:>7.1f}s{b:>7.1f}s  "
                  f"{'OK' if ok else 'FAIL'}", flush=True)
        print(f"{'TOTAL':<9}{base:>12,}{tot:>11,}{tot*8/base:>7.3f}"
              f"{39112636:>11,}{(tot-39112636)*100/39112636:>+9.2f}%"
              f"{tc:>7.1f}s{td:>7.1f}s", flush=True)

    print("\n\n=== FIELD ===", flush=True)
    print(f"input {base:,} bytes")
    print(f"{'codec':<14}{'total':>13}{'ratio':>8}{'bpc':>8}{'comp':>9}"
          f"{'MB/s':>8}{'decomp':>9}{'MB/s':>8}")
    table = dict(REF)
    for t, (tot, tc, td) in results.items():
        table[f"gen16 -9 -t{t}"] = (tot, tc, td)
    for nm, (sz, tc, td) in sorted(table.items(), key=lambda kv: kv[1][0]):
        print(f"{nm:<14}{sz:>13,}{base/sz:>7.2f}x{sz*8/base:>8.3f}{tc:>8.1f}s"
              f"{base/1e6/tc:>8.2f}{td:>8.1f}s{base/1e6/td:>8.2f}")


main()
