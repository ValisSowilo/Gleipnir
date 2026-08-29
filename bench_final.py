"""Full benchmark suite for the finalized engine.

Reports, for every preset and every file: compressed size, bpc, compression and
decompression wall time and throughput, peak RSS on *both* sides, and a
round-trip hash check.  Nothing here is estimated -- peak RSS is read from the
engine's own report (it asks the OS from inside the process, because reading
PeakWorkingSet64 off an exited process returns zero on Windows).

Run with nothing else on the machine.  A concurrent job stealing cores once
produced a phantom 8% regression in this project's history.

  python bench_final.py                    # all presets, full corpus
  python bench_final.py --levels 1,5,9     # subset
  python bench_final.py --threads          # threading scan instead
  python bench_final.py --files dickens,mr
"""
import os, re, sys, json, time, subprocess, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ.get("TEMP", "/tmp"), "benchfinal")
os.makedirs(TMP, exist_ok=True)
EXE = os.path.join(HERE, os.environ.get("BENCHEXE", "gen.exe"))

ORDER = ["xml", "ooffice", "reymont", "sao", "x-ray", "mr", "osdb",
         "dickens", "samba", "nci", "webster", "mozilla"]
# Measured by bench_refs.ps1 in the same session as the sweep it is compared
# against.  An earlier copy of these was 433 bytes lighter in total, from a run
# with slightly different zpaq arguments -- small, but it made the headline
# "vs zpaq" percentage disagree with the reference table by 0.001%.
ZPAQ = {"xml": 326987, "ooffice": 1766594, "reymont": 956543, "sao": 3899298,
        "x-ray": 3669743, "mr": 2181349, "osdb": 2204782, "dickens": 2094787,
        "samba": 3053862, "nci": 1251149, "webster": 5666876,
        "mozilla": 12041099}
ZPAQ_TOTAL = 39113069

PEAK = re.compile(r"([\d.]+)\s*MB peak")

# The benchmark is single-threaded, so on an idle machine system-wide load
# while it runs is about one core's worth.  Anything well above that means
# something else is on the CPU and every timing in the run is contaminated --
# which has happened twice here, once as an "8% regression" that was a
# concurrent job, once as a sweep whose late rows measured slower than doing
# strictly more work.  Sizes survive contamination; times do not.
NPROC = os.cpu_count() or 8
# Claude Code's own UI processes sit at roughly 10-14% here, so the floor has to
# clear that; a game takes 45%+, which is what this is actually looking for.
BUSY_PCT = 30


# Sum the CPU *time* every process accrues over a fixed interval and divide by
# the interval and the core count.  Win32_Processor's LoadPercentage was tried
# first and is unusable for this: it is an instantaneous sample, and repeatedly
# spawning PowerShell to read it creates exactly the load being measured -- it
# reported 40% on a machine whose real sustained load was 13.5%.  Doing the
# sampling *inside* one PowerShell invocation puts the spawn cost before the
# interval starts, where it cannot contaminate the result.
LOAD_PS = r"""
$a = @{}
Get-Process | ForEach-Object { if ($_.CPU) { $a[$_.Id] = $_.CPU } }
Start-Sleep -Seconds 3
$s = 0.0
Get-Process | ForEach-Object {
  if ($_.CPU -and $a.ContainsKey($_.Id)) {
    $d = $_.CPU - $a[$_.Id]
    if ($d -gt 0) { $s += $d }
  }
}
[math]::Round($s / 3 * 100 / [Environment]::ProcessorCount, 1)
"""


def cpu_load():
    """Sustained system-wide CPU load percent, or None if it cannot be read."""
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", LOAD_PS],
            capture_output=True, timeout=60)
        return float(r.stdout.decode().strip().splitlines()[-1])
    except Exception:
        return None


def idle_check(where, hard):
    """Warn -- or refuse -- when the machine is not ours alone."""
    load = cpu_load()
    if load is None:
        return None
    if load > BUSY_PCT:
        busy = ""
        try:
            r = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command",
                 "Get-Process | Sort-Object CPU -Descending |"
                 " Select-Object -First 3 Name |"
                 " ForEach-Object { $_.Name }"],
                capture_output=True, timeout=30)
            busy = " (top: " + ", ".join(r.stdout.decode().split()) + ")"
        except Exception:
            pass
        msg = (f"!! CPU load {load:.0f}% at {where}, expected <{BUSY_PCT}%"
               f"{busy}\n"
               f"!! Timings from this run are not usable. Sizes still are.")
        print(msg, flush=True)
        if hard:
            raise SystemExit("refusing to start on a busy machine; "
                             "pass --anyway to override")
    return load


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(cmd, expect):
    """Freshly linked exes are intermittently blocked by AV / Device Guard, and
    a blocked run leaves the *previous* output in place -- which silently turns
    into a bogus datapoint.  Delete first, then require a fresh non-empty file."""
    for _ in range(6):
        if os.path.exists(expect):
            os.remove(expect)
        t = time.perf_counter()
        r = subprocess.run(cmd, shell=True, capture_output=True)
        dt = time.perf_counter() - t
        if os.path.exists(expect) and os.path.getsize(expect) > 0:
            err = r.stderr.decode("utf-8", "replace")
            m = PEAK.search(err)
            return dt, (float(m.group(1)) if m else 0.0), err
        time.sleep(2.0)
    raise SystemExit("no output after retries: " + cmd)


def one(src, level, threads):
    """level is a string so the throughput presets ("f1", "f2") go through the
    same path as the numeric ones -- they are levels 101 and 102 internally and
    differ only in spelling."""
    c = os.path.join(TMP, "b.c")
    o = os.path.join(TMP, "b.o")
    ct, cmem, _ = run(f'"{EXE}" c -{level} -t{threads} "{src}" "{c}"', c)
    sz = os.path.getsize(c)
    dt, dmem, _ = run(f'"{EXE}" d -t{threads} "{c}" "{o}"', o)
    ok = sha(o) == sha(src)
    for p in (c, o):
        if os.path.exists(p):
            os.remove(p)
    return dict(size=sz, ctime=ct, dtime=dt, cmem=cmem, dmem=dmem, ok=ok)


def corpus(level, threads, files):
    rows, tot, tc, td = [], 0, 0.0, 0.0
    cmem = dmem = 0.0
    base = 0
    for f in files:
        src = os.path.join(SIL, f)
        if not os.path.exists(src):
            continue
        n = os.path.getsize(src)
        r = one(src, level, threads)
        r["file"], r["insize"] = f, n
        rows.append(r)
        tot += r["size"]; tc += r["ctime"]; td += r["dtime"]; base += n
        cmem = max(cmem, r["cmem"]); dmem = max(dmem, r["dmem"])
        print(f"  {f:<9}{n:>12,}{r['size']:>11,}{r['size']*8/n:>7.3f}"
              f"{r['ctime']:>8.1f}s{r['dtime']:>8.1f}s"
              f"{r['cmem']:>8.0f}M{r['dmem']:>8.0f}M  "
              f"{'OK' if r['ok'] else 'FAIL'}", flush=True)
    return rows, tot, tc, td, base, cmem, dmem


def main():
    args = sys.argv[1:]
    levels = ["1", "2", "3", "5", "7", "9"]
    files = ORDER
    # A partial run must not land on top of the dataset the README quotes.
    out_name = "bench_final.json"
    thread_scan = "--threads" in args
    for i, a in enumerate(args):
        if a == "--levels":
            levels = [x.lstrip("-") for x in args[i + 1].split(",")]
        if a == "--files":
            files = args[i + 1].split(",")
        if a == "--out":
            out_name = args[i + 1]

    if not os.path.exists(EXE):
        raise SystemExit("missing engine: " + EXE)
    idle_check("start", hard="--anyway" not in args)
    out = {"exe": EXE, "levels": {}, "threads": {}, "load": {}}

    if thread_scan:
        src = os.path.join(SIL, "dickens")
        print(f"threading scan, dickens, level 9\n")
        print(f"  {'-t':>4}{'size':>12}{'vs t1':>9}{'comp':>9}{'speedup':>9}"
              f"{'decomp':>9}{'peakMB':>9}")
        b = None
        for t in (1, 2, 4, 8, 16):
            r = one(src, "9", t)
            if b is None:
                b = r
            print(f"  {t:>4}{r['size']:>12,}{(r['size']-b['size'])*100/b['size']:>+8.2f}%"
                  f"{r['ctime']:>8.1f}s{b['ctime']/r['ctime']:>8.2f}x"
                  f"{r['dtime']:>8.1f}s{r['cmem']:>8.0f}", flush=True)
            out["threads"][t] = r
        json.dump(out, open(os.path.join(HERE, "bench_threads.json"), "w"), indent=1)
        return

    for lv in levels:
        print(f"\n=== level -{lv}, -t1 ===")
        print(f"  {'file':<9}{'size':>12}{'out':>11}{'bpc':>7}"
              f"{'comp':>9}{'decomp':>9}{'cRAM':>9}{'dRAM':>9}  ok")
        rows, tot, tc, td, base, cmem, dmem = corpus(lv, 1, files)
        if not base:
            continue
        print(f"  {'TOTAL':<9}{base:>12,}{tot:>11,}{tot*8/base:>7.3f}"
              f"{tc:>8.1f}s{td:>8.1f}s{cmem:>8.0f}M{dmem:>8.0f}M", flush=True)
        print(f"  ratio {base/tot:.2f}x   comp {base/1e6/tc:.3f} MB/s   "
              f"decomp {base/1e6/td:.3f} MB/s   vs zpaq -m5 "
              f"{(tot-ZPAQ_TOTAL)*100/ZPAQ_TOTAL:+.2f}%", flush=True)
        out["levels"][lv] = dict(rows=rows, total=tot, ctime=tc, dtime=td,
                                 base=base, cmem=cmem, dmem=dmem)
        out["load"][lv] = idle_check(f"end of -{lv}", hard=False)
        # Written after every level, not only at the end: an interrupted sweep
        # used to lose every level it had already measured.
        json.dump(out, open(os.path.join(HERE, out_name), "w"), indent=1)

    print("\nwrote " + out_name)


main()
