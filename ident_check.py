"""Byte-identity check between two builds, per file and per preset.

With every Tier-2 gate off, the new build only stops maintaining side tables
that no context in the preset reads.  That claim is testable rather than
arguable: the output must be identical to the previous build's, bit for bit,
at every level where the reading contexts are absent -- and at every level
where they are present, since nothing there changed at all.

  python ident_check.py gen.exe g26a.exe 1,3,5,7,9
"""
import os, sys, time, subprocess, hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
SIL = os.path.join(HERE, "tools", "corpora", "silesia")
TMP = os.path.join(os.environ.get("TEMP", "/tmp"), "identchk")
os.makedirs(TMP, exist_ok=True)

FILES = ["dickens", "samba", "nci", "x-ray"]


def comp(exe, src, lv, out):
    for _ in range(6):
        if os.path.exists(out):
            os.remove(out)
        subprocess.run([exe, "c", f"-{lv}", "-t1", src, out], capture_output=True)
        if os.path.exists(out) and os.path.getsize(out) > 0:
            h = hashlib.sha256(open(out, "rb").read()).hexdigest()
            return os.path.getsize(out), h
        time.sleep(2.0)
    raise SystemExit("no output: " + exe)


def main():
    # Bare "gen.exe" is not resolved against the working directory by
    # CreateProcess, so anchor both to HERE.
    a = os.path.join(HERE, sys.argv[1])
    b = os.path.join(HERE, sys.argv[2])
    levels = sys.argv[3].split(",") if len(sys.argv) > 3 else ["1", "3", "5", "7", "9"]
    files = sys.argv[4].split(",") if len(sys.argv) > 4 else FILES
    bad = 0
    for lv in levels:
        for f in files:
            src = os.path.join(SIL, f)
            if not os.path.exists(src):
                continue
            sa, ha = comp(a, src, lv, os.path.join(TMP, "a.bin"))
            sb, hb = comp(b, src, lv, os.path.join(TMP, "b.bin"))
            ok = ha == hb
            bad += not ok
            print(f"  -{lv:<3}{f:<10}{sa:>11,}{sb:>11,}  "
                  f"{'identical' if ok else 'DIFFERS %+.4f%%' % ((sb-sa)*100/sa)}",
                  flush=True)
    print("IDENT_OK" if not bad else f"IDENT_DIFF {bad}")


main()
