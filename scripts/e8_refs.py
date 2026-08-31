"""zpaq and lpaq1 on enwik8, so the enwik8 table's reference rows come from the
same session as its gen rows.

Sizes were already known and are deterministic; it is the *times* that were
carried over from an older session, and cross-session times on this machine
have been seen to differ by 11%.
"""
import os, time, subprocess

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
SRC = os.path.join(HERE, "tools", "corpora", "enwik8")
TMP = os.path.join(os.environ["TEMP"], "e8refs")
os.makedirs(TMP, exist_ok=True)
ZPAQ = os.path.join(HERE, "tools", "zpaq", "zpaq64.exe")
LPAQ = os.path.join(HERE, "tools", "lpaq1.exe")


def run(cmd, expect):
    if os.path.exists(expect):
        if os.path.isdir(expect):
            import shutil
            shutil.rmtree(expect, ignore_errors=True)
        else:
            os.remove(expect)
    t = time.perf_counter()
    subprocess.run(cmd, capture_output=True)
    dt = time.perf_counter() - t
    if not os.path.exists(expect):
        raise SystemExit("no output: " + " ".join(cmd))
    return dt


print(f"  {'codec':<12}{'output':>12}{'comp':>9}{'decomp':>9}")

c = os.path.join(TMP, "a.zpaq")
d = os.path.join(TMP, "zx")
ct = run([ZPAQ, "a", c, SRC, "-m5", "-t1"], c)
sz = os.path.getsize(c)
dt = run([ZPAQ, "x", c, "-to", d, "-t1"], d)
print(f"  {'zpaq -m5':<12}{sz:>12,}{ct:>8.1f}s{dt:>8.1f}s", flush=True)

c2 = os.path.join(TMP, "a.lpq")
o2 = os.path.join(TMP, "a.out")
ct = run([LPAQ, "6", SRC, c2], c2)
sz = os.path.getsize(c2)
dt = run([LPAQ, "d", c2, o2], o2)
print(f"  {'lpaq1 -6':<12}{sz:>12,}{ct:>8.1f}s{dt:>8.1f}s", flush=True)
print("E8_REFS_DONE")
