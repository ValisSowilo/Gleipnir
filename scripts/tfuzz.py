"""Round-trip across thread counts.  Chunking changes the container layout and
the per-chunk cold start, so the cases that matter are the ones sitting on the
chunk-size boundary and the content types that trigger a filter or the store
path inside a worker.
Usage: tfuzz.py [exe]"""
import os, random, subprocess, hashlib, sys, zlib, time, tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
# The v2 archiver takes "c archive input..." where v1 took "c input output",
# so which CLI to drive has to be stated rather than guessed.  Driving the
# wrong one makes every case fail identically, which reads like a real
# regression until you look at the arguments.
V2 = "--v2" in sys.argv
ARGS = [a for a in sys.argv[1:] if a != "--v2"]
GEN = os.path.join(HERE, ARGS[0] if ARGS else "gen16.exe")
# The only argument is an exe name, but "40" looks enough like a trial count
# that it gets passed as one -- and then every case fails identically against a
# binary that does not exist, which reads as a total regression.  That is the
# same trap the --v2 comment above describes, so refuse rather than report it
# as sixty failures.
if not os.path.exists(GEN):
    sys.exit(f"tfuzz: {GEN} does not exist.\n"
             f"  usage: python tfuzz.py [--v2] [exe]   (the argument is an "
             f"executable, not a trial count)")
# gettempdir() rather than os.environ["TEMP"]: TEMP is a Windows variable and
# is simply absent on Linux, so the old line raised KeyError before a single
# case ran.  gfuzz.py already does it this way.
TMP = os.path.join(tempfile.gettempdir(), "tfz")
os.makedirs(TMP, exist_ok=True)
MB = 1 << 20
THREADS = (1, 2, 3, 5, 8, 12)
LEVELS = (9, 7, 5, 3, 2, 1, "f2", "f1")


def rm_retry(p, tries=20):
    """The AV scanner holds a just-written file open for a moment, and an
    os.remove into that window raises WinError 32.  That once aborted a passing
    run *after* the round trips had already succeeded, which reads exactly like
    a compressor failure and is not one."""
    for _ in range(tries):
        if not os.path.exists(p):
            return
        try:
            os.remove(p)
            return
        except PermissionError:
            time.sleep(0.25)


def cases():
    r = random.Random(99)
    text = b"the quick brown fox jumps over the lazy dog\n" * 200000
    code = b"\xe8\x10\x20\x00\x00\x48\x8b\xcf\x55\x8b\xec" * 500000
    payload = b"<config><item name='x' value='1'/></config>\n" * 900
    C = [
        ("text 5MB", text[:5 * MB]),
        ("x86 5MB", b"MZ" + code[:5 * MB]),
        ("random 5MB", os.urandom(5 * MB)),
        ("mixed 6MB", b"".join((text[:300000] if i % 2 else os.urandom(300000))
                               for i in range(20))),
        ("zlib streams", b"".join(zlib.compress(payload + bytes([i & 255]), 6)
                                  for i in range(1200))),
        ("ascii bits", bytes(r.choice(b"01") for _ in range(3 * MB))),
        ("exactly 4MB", text[:4 * MB]),
        ("1MB+1", text[:MB + 1]),
        ("1MB-1", text[:MB - 1]),
        ("just under 2 chunks", text[:2 * MB - 1]),
    ]
    return C


def main():
    fails, n = [], 0
    for name, data in cases():
        src = os.path.join(TMP, "in.bin")
        with open(src, "wb") as f:
            f.write(data)
        ref = hashlib.sha256(data).hexdigest()
        for t in THREADS:
            # Rotate the preset with the thread count so the chunking path is
            # exercised at every model configuration, not only at -9.  The
            # rotation is offset per case as well: with more presets than
            # thread counts, indexing on the thread alone would never reach
            # the last presets in the list.
            lvl = LEVELS[n % len(LEVELS)]
            c, o = os.path.join(TMP, "a.c"), os.path.join(TMP, "a.o")
            dt = THREADS[(THREADS.index(t) + 2) % len(THREADS)]
            if V2:
                odir = os.path.join(TMP, "out")
                os.makedirs(odir, exist_ok=True)
                o = os.path.join(odir, os.path.basename(src))
                rm_retry(o)
                subprocess.run(f'"{GEN}" c -{lvl} -t{t} -q "{c}" "{src}"',
                               shell=True, capture_output=True)
                # decode with a *different* thread count than it was encoded with
                subprocess.run(f'"{GEN}" d -t{dt} -q "{c}" "{odir}"',
                               shell=True, capture_output=True)
            else:
                subprocess.run(f'"{GEN}" c -{lvl} -t{t} "{src}" "{c}"', shell=True,
                               capture_output=True)
                # decode with a *different* thread count than it was encoded with
                subprocess.run(f'"{GEN}" d -t{dt} "{c}" "{o}"', shell=True,
                               capture_output=True)
            got = hashlib.sha256(open(o, "rb").read()).hexdigest() \
                if os.path.exists(o) else "missing"
            n += 1
            if got != ref:
                fails.append(f"{name} -{lvl} -t{t} (decode -t{dt})")
            for p in (c, o):
                rm_retry(p)
        os.remove(src)
    print(f"{n} round trips across {len(THREADS)} thread counts")
    if fails:
        print(f"FAILURES: {len(fails)}")
        for f in fails:
            print("  " + f)
        sys.exit(1)
    print("all exact")


main()
