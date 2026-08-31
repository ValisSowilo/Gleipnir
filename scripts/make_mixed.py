"""Build a diverse test corpus of file types Silesia does not contain.

Silesia is two English texts, one Polish text, XML, HTML, a source tarball, two
executables, two medical rasters, a star catalogue and a database dump.  It has
no JSON, no CSV, no logs, no float arrays, no UTF-16, no base64, no already-
compressed data, no audio, no genomic data and no 64-bit code.  Every weakness
found in this project so far came from testing something new, so this makes the
untested cases available.

Synthetic files are generated with a fixed seed so results are reproducible.
Real files are copied from the system where a realistic example exists.
"""
import os, sys, json, random, struct, zlib, base64, shutil, glob, sysconfig

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
OUT = os.path.join(HERE, "tools", "corpora", "mixed")
os.makedirs(OUT, exist_ok=True)
R = random.Random(20260730)
TARGET = 4 << 20          # aim for ~4 MB per file; big enough that the models fill


def put(name, data):
    p = os.path.join(OUT, name)
    with open(p, "wb") as f:
        f.write(data)
    print(f"  {name:<16}{len(data):>12,}")


def grow(chunkfn, target=TARGET):
    buf = bytearray()
    while len(buf) < target:
        buf += chunkfn()
    return bytes(buf[:target])


# ---- structured text -------------------------------------------------------
FIRST = ["ada", "grace", "alan", "edsger", "barbara", "donald", "tony", "leslie"]
LAST = ["lovelace", "hopper", "turing", "dijkstra", "liskov", "knuth", "hoare"]
CITY = ["london", "paris", "tokyo", "boston", "berlin", "lagos", "lima", "oslo"]


def json_chunk():
    recs = []
    for _ in range(200):
        recs.append({
            "id": R.randrange(1 << 31),
            "user": {"first": R.choice(FIRST), "last": R.choice(LAST),
                     "city": R.choice(CITY)},
            "score": round(R.uniform(0, 100), 3),
            "active": R.random() > 0.3,
            "tags": [R.choice(["alpha", "beta", "gamma", "delta"])
                     for _ in range(R.randrange(1, 4))],
            "ts": 1700000000 + R.randrange(10 ** 7),
        })
    return json.dumps(recs, indent=1).encode()


def csv_chunk():
    rows = []
    for _ in range(500):
        rows.append("%d,%s,%s,%.4f,%.4f,%d" % (
            R.randrange(10 ** 6), R.choice(FIRST), R.choice(CITY),
            R.uniform(-180, 180), R.uniform(-90, 90), R.randrange(1 << 20)))
    return ("\n".join(rows) + "\n").encode()


def log_chunk():
    lvl = ["INFO", "WARN", "ERROR", "DEBUG"]
    msg = ["connection established", "cache miss for key",
           "request completed", "retrying after timeout",
           "user session expired", "wrote checkpoint"]
    out = []
    for _ in range(400):
        out.append("2026-07-%02d %02d:%02d:%02d.%03d [%s] worker-%d %s id=%d" % (
            R.randrange(1, 31), R.randrange(24), R.randrange(60),
            R.randrange(60), R.randrange(1000), R.choice(lvl),
            R.randrange(16), R.choice(msg), R.randrange(1 << 24)))
    return ("\n".join(out) + "\n").encode()


# ---- numeric ---------------------------------------------------------------
def f64_chunk():
    """A smooth signal plus noise as IEEE doubles -- the case the record model
    should find (stride 8) and the byte orders cannot."""
    import math
    vals = []
    for i in range(4096):
        t = f64_chunk.phase + i * 0.001
        vals.append(math.sin(t) * 100 + math.sin(t * 3.7) * 10 + R.gauss(0, 0.5))
    f64_chunk.phase += 4.096
    return struct.pack("<4096d", *vals)


f64_chunk.phase = 0.0


def i32_chunk():
    """Monotonically increasing 32-bit ints with small deltas: timestamps,
    row ids, offsets.  Stride 4."""
    out = []
    v = i32_chunk.v
    for _ in range(8192):
        v += R.randrange(1, 40)
        out.append(v)
    i32_chunk.v = v
    return struct.pack("<8192I", *out)


i32_chunk.v = 1_500_000_000


def wav_chunk():
    """16-bit stereo PCM: two correlated channels, stride 4, which is the
    interleaving the raster/record contexts have never been tested against."""
    import math
    frames = []
    for i in range(2048):
        t = wav_chunk.phase + i * 0.002
        l = int(math.sin(t * 2) * 8000 + math.sin(t * 5) * 2000 + R.gauss(0, 60))
        r = int(l * 0.85 + R.gauss(0, 60))
        frames.append(max(-32768, min(32767, l)))
        frames.append(max(-32768, min(32767, r)))
    wav_chunk.phase += 4.096
    return struct.pack("<%dh" % len(frames), *frames)


wav_chunk.phase = 0.0


# ---- encodings -------------------------------------------------------------
def utf16_chunk():
    words = ["the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
             "compression", "entropy", "context", "model", "arithmetic"]
    s = " ".join(R.choice(words) for _ in range(2000)) + "\n"
    return s.encode("utf-16-le")


def b64_chunk():
    return base64.b64encode(bytes(R.randrange(256) for _ in range(3000))) + b"\n"


def dna_chunk():
    """FASTA: a 4-letter alphabet, which should trigger small-alphabet packing."""
    seq = "".join(R.choice("ACGT") for _ in range(60 * 800))
    lines = [">seq%d\n" % R.randrange(10 ** 6)]
    for i in range(0, len(seq), 60):
        lines.append(seq[i:i + 60] + "\n")
    return "".join(lines).encode()


def main():
    print("synthetic:")
    put("json.txt", grow(json_chunk))
    put("csv.txt", grow(csv_chunk))
    put("log.txt", grow(log_chunk))
    put("f64.bin", grow(f64_chunk))
    put("i32.bin", grow(i32_chunk))
    put("pcm16.bin", grow(wav_chunk))
    put("utf16.txt", grow(utf16_chunk))
    put("base64.txt", grow(b64_chunk))
    put("dna.fasta", grow(dna_chunk))

    # already-compressed: the store path should recognise this and not waste time
    src = grow(json_chunk, 12 << 20)
    put("precomp.zlib", zlib.compress(src, 9)[:TARGET])

    print("real:")
    # 64-bit Windows DLLs -- Silesia's executables are 32-bit x86 and Alpha
    dlls = sorted(glob.glob("C:/Windows/System32/*.dll"), key=os.path.getsize)
    big = [d for d in dlls if 2 << 20 < os.path.getsize(d) < 12 << 20]
    if big:
        shutil.copy(big[len(big) // 2], os.path.join(OUT, "x64.dll"))
        print(f"  {'x64.dll':<16}{os.path.getsize(os.path.join(OUT,'x64.dll')):>12,}"
              f"   ({os.path.basename(big[len(big)//2])})")
    # a font: binary tables plus glyph outlines
    fonts = sorted(glob.glob("C:/Windows/Fonts/*.ttf"), key=os.path.getsize)
    fb = [f for f in fonts if 2 << 20 < os.path.getsize(f) < 20 << 20]
    if fb:
        shutil.copy(fb[0], os.path.join(OUT, "font.ttf"))
        print(f"  {'font.ttf':<16}{os.path.getsize(os.path.join(OUT,'font.ttf')):>12,}"
              f"   ({os.path.basename(fb[0])})")
    # python source tree concatenated: source code, different from samba's C
    py = []
    # Ask the running interpreter where its stdlib is rather than hardcoding a
    # path: this member only needs "a few megabytes of real Python source" and
    # any installation supplies that.  Like the other copied-from-the-system
    # members it is not byte-identical across machines; only the synthetic ones
    # are.
    lib = sysconfig.get_paths()["stdlib"]
    for root, _, files in os.walk(lib):
        for f in files:
            if f.endswith(".py"):
                try:
                    py.append(open(os.path.join(root, f), "rb").read())
                except OSError:
                    pass
        if sum(len(x) for x in py) > TARGET:
            break
    if py:
        put("python.src", b"".join(py)[:TARGET])


main()
