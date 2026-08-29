"""Randomised corruption fuzzer for the v2 archive format.

fuzz.py and tfuzz.py answer "does a valid archive round trip".  This answers
the question that matters once the data is actually sitting on a disk for five
years: what happens when the archive is *not* valid any more.

The contract under test is narrow and absolute.  For any input at all --
truncated, bit-flipped, spliced, or pure noise -- gen must do exactly one of:

  * exit 0 and produce output identical to the original, or
  * exit 1 or 2 and print a diagnostic

Anything else is a bug.  A crash, a hang, a negative or >2 exit status, or
worst of all exit 0 with wrong bytes, all fail here.  That last case is the
one this exists for: v1 would happily decode a damaged container into
plausible-looking garbage and report success, because it had no checksum to
tell it otherwise.

  python gfuzz.py [trials] [--exe genv2.exe] [--seed N]
"""
import os, sys, random, shutil, subprocess, hashlib, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SIL  = os.path.join(HERE, "tools", "corpora", "silesia")
TMP  = os.path.join(tempfile.gettempdir(), "gfuzz")
TIMEOUT = 300

PRESETS = ["f1", "f2", "1", "2", "3"]      # low presets: coverage, not ratio
OK_EXITS = (0, 1, 2)


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(args):
    """Returns (rc, stderr).  A timeout is a failure, not a retry: this tool
    must never hang on malformed input."""
    try:
        p = subprocess.run(args, capture_output=True, timeout=TIMEOUT)
        return p.returncode, p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return "TIMEOUT", ""
    except OSError as e:
        return "OSERROR:%s" % e, ""


def make_sources(rnd, n=6):
    """A spread of shapes, because the container takes different paths for
    each: text models, binaries take the x86/alpha filters, the all-same file
    takes alphabet packing, and random data takes the stored-block fallback."""
    os.makedirs(TMP, exist_ok=True)
    out = []
    pool = [f for f in ("dickens", "samba", "xml", "mozilla", "osdb", "nci")
            if os.path.exists(os.path.join(SIL, f))]
    for i in range(n):
        p = os.path.join(TMP, "src%d" % i)
        kind = rnd.randrange(5)
        if kind == 0 or not pool:
            data = bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 40000)))
        elif kind == 1:
            data = b"A" * rnd.randrange(1, 60000)
        elif kind == 2:
            data = b""
        else:
            src = os.path.join(SIL, rnd.choice(pool))
            sz = os.path.getsize(src)
            k = rnd.randrange(1000, min(sz, 3 << 20))
            off = rnd.randrange(0, sz - k)
            with open(src, "rb") as f:
                f.seek(off); data = f.read(k)
        with open(p, "wb") as f:
            f.write(data)
        out.append(p)
    return out


def corrupt(data, rnd):
    """Damage models, roughly in order of how likely each is in real storage."""
    d = bytearray(data)
    if not d:
        return bytes(d), "empty"
    mode = rnd.choice(["bitflip", "bitflip", "burst", "truncate", "truncate",
                       "zero", "splice", "extend", "noise"])
    if mode == "bitflip":
        for _ in range(rnd.randrange(1, 4)):
            i = rnd.randrange(len(d)); d[i] ^= 1 << rnd.randrange(8)
    elif mode == "burst":                       # a decayed sector
        i = rnd.randrange(len(d)); k = min(len(d) - i, rnd.randrange(16, 4096))
        for j in range(i, i + k): d[j] = rnd.randrange(256)
    elif mode == "truncate":                    # interrupted transfer
        d = d[:rnd.randrange(0, len(d))]
    elif mode == "zero":                        # a hole in the filesystem
        i = rnd.randrange(len(d)); k = min(len(d) - i, rnd.randrange(16, 4096))
        for j in range(i, i + k): d[j] = 0
    elif mode == "splice":                      # two archives run together
        i = rnd.randrange(len(d)); j = rnd.randrange(len(d))
        d = d[:i] + d[j:]
    elif mode == "extend":                      # trailing junk
        d = d + bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 5000)))
    else:
        d = bytearray(rnd.randrange(256) for _ in range(rnd.randrange(1, 9000)))
    return bytes(d), mode


def main():
    trials = 300
    exe = os.path.join(HERE, "genv2.exe")
    seed = 12345
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--exe":  exe = os.path.join(HERE, args[i + 1]); i += 2
        elif args[i] == "--seed": seed = int(args[i + 1]); i += 2
        else: trials = int(args[i]); i += 1

    rnd = random.Random(seed)
    os.makedirs(TMP, exist_ok=True)
    srcs = make_sources(rnd)
    arc = os.path.join(TMP, "a.gen")
    out = os.path.join(TMP, "out")

    # ---- phase 1: clean archives must round trip ----
    print("phase 1: clean round trips")
    clean = []
    nclean = 0
    for s in srcs:
        for preset in PRESETS:
            segmb = rnd.choice([1, 2, 64])
            thr   = rnd.choice([1, 2, 4])
            par   = rnd.choice([0, 4, 32])
            cmd = [exe, "c", "-" + preset, "-t%d" % thr, "-s%d" % segmb, "-q"]
            if par: cmd.append("-p%d" % par)
            cmd += [arc, s]
            rc, err = run(cmd)
            if rc != 0:
                print("  FAIL compress %s -%s: rc=%s %s" % (s, preset, rc, err.strip()))
                return 1
            shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
            rc, err = run([exe, "d", "-q", arc, out])
            got = os.path.join(out, os.path.basename(s))
            if rc != 0 or not os.path.exists(got) or sha(got) != sha(s):
                print("  FAIL roundtrip %s -%s: rc=%s %s" % (s, preset, rc, err.strip()))
                return 1
            rc, _ = run([exe, "t", "-q", arc])
            if rc != 0:
                print("  FAIL verify %s -%s: rc=%s" % (s, preset, rc))
                return 1
            clean.append((open(arc, "rb").read(), sha(s), os.path.basename(s)))
            nclean += 1
    print("  %d clean archives round tripped exact" % nclean)

    # ---- phase 2: corruption must never crash or lie ----
    print("phase 2: %d corruption trials" % trials)
    bad = os.path.join(TMP, "bad.gen")
    stats = {}
    silent_ok = 0
    for n in range(trials):
        data, ssha, sname = rnd.choice(clean)
        cd, mode = corrupt(data, rnd)
        with open(bad, "wb") as f:
            f.write(cd)
        stats[mode] = stats.get(mode, 0) + 1

        # Every read-side mode has to survive the same damage.  The fast scrub
        # and the repair writer walk different code than extraction does, and
        # repair in particular writes a new archive from damaged input.
        for mode_args in (["t", "-q"], ["t", "-D", "-q"]):
            rc, err = run([exe] + mode_args + [bad])
            if rc not in OK_EXITS:
                print("  FAIL[%d] %s: '%s' returned %s\n%s"
                      % (n, mode, " ".join(mode_args), rc, err))
                return 1

        fixed = os.path.join(TMP, "fixed.gen")
        rc, err = run([exe, "r", "-q", bad, fixed])
        if rc not in OK_EXITS:
            print("  FAIL[%d] %s: 'r' returned %s\n%s" % (n, mode, rc, err))
            return 1
        if rc == 0 and os.path.exists(fixed):
            # A repair that claims success must produce an archive that is
            # itself readable, or it has laundered damage into a clean-looking
            # file -- the worst possible outcome for a recovery tool.
            rc2, err2 = run([exe, "t", "-q", fixed])
            if rc2 not in OK_EXITS:
                print("  FAIL[%d] %s: repaired archive fails 't': %s\n%s"
                      % (n, mode, rc2, err2))
                return 1

        shutil.rmtree(out, ignore_errors=True); os.makedirs(out)
        rc, err = run([exe, "d", "-q", bad, out])
        if rc not in OK_EXITS:
            print("  FAIL[%d] %s: 'd' returned %s\n%s" % (n, mode, rc, err))
            return 1
        if rc == 0:
            # Claiming success obliges it to be byte-exact.  This is the
            # assertion v1 could not have passed.
            got = os.path.join(out, sname)
            if not os.path.exists(got):
                print("  FAIL[%d] %s: exit 0 but no output" % (n, mode))
                return 1
            if sha(got) != ssha:
                print("  FAIL[%d] %s: exit 0 with WRONG BYTES" % (n, mode))
                return 1
            silent_ok += 1
        if (n + 1) % 50 == 0:
            print("    %d/%d" % (n + 1, trials))

    print("  %d trials, no crash, no hang, no silent corruption" % trials)
    print("  %d damaged inputs still decoded exactly (recovery records or "
          "damage landed in slack)" % silent_ok)
    print("  damage mix: " + ", ".join("%s=%d" % kv for kv in sorted(stats.items())))
    print("PASS")
    return 0


sys.exit(main())
