"""Rebuild bench_final.json from a bench_final.py stdout log.

bench_final.py only writes its JSON after the last preset finishes, so an
interrupted sweep loses every level it had already measured -- that cost a
90-minute run once.  The log is written line-buffered as each file completes,
so it always holds everything measured so far.  This recovers it.

Use it when a sweep is interrupted, or to fold a partial run into a graphable
file.  Levels are only emitted if their TOTAL line is present, so a
half-finished level is dropped rather than reported short.

  python parse_bench_log.py bench_v6.log            # -> bench_final.json
  python parse_bench_log.py bench_v6.log out.json
"""
import os, re, sys, json

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)

LEVEL = re.compile(r"^=== level -(\d+), -t(\d+) ===")
# file  insize  outsize  bpc  Xs  Ys  AM  BM  OK
ROW = re.compile(
    r"^\s{2}(\S+)\s+([\d,]+)\s+([\d,]+)\s+[\d.]+\s+"
    r"([\d.]+)s\s+([\d.]+)s\s+(\d+)M\s+(\d+)M\s+(OK|FAIL)\s*$")
TOTAL = re.compile(
    r"^\s{2}TOTAL\s+([\d,]+)\s+([\d,]+)\s+[\d.]+\s+"
    r"([\d.]+)s\s+([\d.]+)s\s+(\d+)M\s+(\d+)M\s*$")


def num(s):
    return int(s.replace(",", ""))


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "bench_v6.log"
    dst = sys.argv[2] if len(sys.argv) > 2 else "bench_final.json"
    if not os.path.isabs(src):
        src = os.path.join(HERE, src)
    if not os.path.isabs(dst):
        dst = os.path.join(HERE, dst)

    out = {"exe": "recovered from " + os.path.basename(src),
           "levels": {}, "threads": {}}
    lv = None
    rows = []
    dropped = []

    for line in open(src, encoding="utf-8", errors="replace"):
        m = LEVEL.match(line)
        if m:
            if lv is not None and rows:
                dropped.append(lv)          # no TOTAL reached before next level
            lv, rows = m.group(1), []
            continue
        m = ROW.match(line)
        if m and lv is not None:
            rows.append(dict(file=m.group(1), insize=num(m.group(2)),
                             size=num(m.group(3)), ctime=float(m.group(4)),
                             dtime=float(m.group(5)), cmem=float(m.group(6)),
                             dmem=float(m.group(7)), ok=m.group(8) == "OK"))
            continue
        m = TOTAL.match(line)
        if m and lv is not None:
            out["levels"][lv] = dict(
                rows=rows, base=num(m.group(1)), total=num(m.group(2)),
                ctime=float(m.group(3)), dtime=float(m.group(4)),
                cmem=float(m.group(5)), dmem=float(m.group(6)))
            if lv in dropped:
                dropped.remove(lv)
            lv, rows = None, []

    if lv is not None and rows:
        dropped.append(lv)

    if not out["levels"]:
        raise SystemExit("no complete level found in " + src)

    bad = [r["file"] for d in out["levels"].values()
           for r in d["rows"] if not r["ok"]]
    json.dump(out, open(dst, "w"), indent=1)
    got = ", ".join("-" + k for k in sorted(out["levels"], key=int))
    print(f"wrote {os.path.basename(dst)}: levels {got}")
    if dropped:
        print("  incomplete, dropped: " + ", ".join("-" + d for d in dropped))
    if bad:
        print("  ROUND TRIP FAILED: " + ", ".join(bad))


main()
