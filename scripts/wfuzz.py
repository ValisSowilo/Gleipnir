"""Round-trip fuzzer for the word transform.

fuzz.py's inputs top out at 70 KB and the transform starts at 256 KB, so none of
the other suites ever builds a word-transformed segment.  This one generates
text that the transform takes -- and that stresses it: the control bytes it
picks its flags from, bytes above 0x7F that must be escaped, all three case
forms plus mixed case, and words either side of the 32-letter limit -- then
round-trips every file and compares it byte for byte.

  python scripts/wfuzz.py [cases] [--exe EXE] [--level N] [--seed N]

Exit 0 when every case round-trips, 1 otherwise.  A failing input is kept in
the temporary directory and named in the output.
"""
import os, random, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root
TMP = os.path.join(tempfile.gettempdir(), "wfuzz")
SEPS = [b" ", b" ", b" ", b", ", b". ", b"\n", b" 1984 ", b"_", b"<b>", b"\r\n"]


def main():
    n, exe, lvl, seed = 30, "gleipnir.exe" if os.name == "nt" else "gleipnir", "1", 1
    a, i = sys.argv[1:], 0
    while i < len(a):
        if a[i] == "--exe" and i + 1 < len(a):
            exe = a[i + 1]; i += 2
        elif a[i] == "--level" and i + 1 < len(a):
            lvl = a[i + 1]; i += 2
        elif a[i] == "--seed" and i + 1 < len(a):
            seed = int(a[i + 1]); i += 2
        elif a[i].isdigit():
            n = int(a[i]); i += 1
        else:
            sys.exit("wfuzz: unknown argument %r\n"
                     "  usage: wfuzz.py [cases] [--exe EXE] [--level N] [--seed N]" % a[i])
    gen = exe if os.path.isabs(exe) else os.path.join(HERE, exe)
    if not os.path.exists(gen):
        sys.exit("wfuzz: %s does not exist" % gen)

    R = random.Random(seed)
    vocab = ["".join(R.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(R.randint(1, 12)))
             for _ in range(3000)]
    vocab += ["a" * 32, "b" * 33, "x" * 31, "q" * 2]

    def word():
        w = R.choice(vocab)
        k = R.random()
        if k < .15: w = w.capitalize()
        elif k < .2: w = w.upper()
        elif k < .23: w = "".join(c.upper() if R.random() < .5 else c for c in w)
        return w.encode()

    shutil.rmtree(TMP, ignore_errors=True)
    os.makedirs(TMP)
    bad = 0
    for it in range(n):
        out = bytearray()
        size = R.randint(270000, 600000)
        hi = R.random() * 0.015          # under the 2% gate, so it is taken
        ctl = R.random() * 0.02
        while len(out) < size:
            out += word()
            r = R.random()
            if r < ctl: out += bytes([R.randint(1, 31)])
            elif r < ctl + hi: out += bytes([R.randint(128, 255)])
            elif r < ctl + hi + 0.03: pass
            else: out += R.choice(SEPS)
        f = os.path.join(TMP, "w%d" % it)
        with open(f, "wb") as fh:
            fh.write(out)
        g, xd = f + ".gl", f + ".x"
        c = subprocess.run([gen, "c", "-" + lvl, "-q", g, f], capture_output=True)
        x = subprocess.run([gen, "x", "-q", g, xd], capture_output=True)
        got = os.path.join(xd, os.path.basename(f))
        ok = (c.returncode == 0 and x.returncode == 0 and os.path.exists(got)
              and open(got, "rb").read() == bytes(out))
        if ok:
            os.remove(f); os.remove(g)
        else:
            bad += 1
            print("FAIL case %d (%s): compress %d, extract %d %s"
                  % (it, f, c.returncode, x.returncode, x.stderr[:200]))
        shutil.rmtree(xd, ignore_errors=True)
    print("%d cases at -%s, seed %d, %d failures" % (n, lvl, seed, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
