"""Round-trip gen11 over the edge cases that break container formats:
empty and near-empty files, single-symbol files, alphabet sizes either side of
the pack threshold, sizes either side of the filter minimums, executables that
are too short to filter, and incompressible input at every level."""
import os, random, subprocess, hashlib, sys

HERE = os.path.dirname(os.path.abspath(__file__))
# The v2 archiver takes "c archive input..." where v1 took "c input output",
# so which CLI to drive has to be stated rather than guessed.
V2 = "--v2" in sys.argv
ARGS = [a for a in sys.argv[1:] if a != "--v2"]
GEN = os.path.join(HERE, ARGS[0] if ARGS else "gen11.exe")
TMP = os.path.join(HERE, "fz.bin")
ODIR = os.path.join(HERE, "fz_out")


def case(name, data):
    return (name, data)


def build_cases():
    r = random.Random(1234)
    C = []
    for n in (0, 1, 2, 5, 6, 7, 63, 64, 1023, 1024, 1025, 4096, 70000):
        C.append(case(f"random n={n}", bytes(r.randrange(256) for _ in range(n))))
    for n in (0, 1, 1023, 1024, 5000):
        C.append(case(f"zeros n={n}", b"\x00" * n))
        C.append(case(f"one symbol n={n}", b"Q" * n))
    for k in (2, 3, 4, 5, 16, 17):
        alpha = bytes(range(65, 65 + k))
        for n in (1023, 1024, 20000):
            C.append(case(f"alphabet k={k} n={n}",
                          bytes(alpha[r.randrange(k)] for _ in range(n))))
    C.append(case("ascii bits", bytes(r.choice(b"01") for _ in range(50000))))
    C.append(case("text", (b"the quick brown fox jumps over the lazy dog\n" * 700)))
    C.append(case("MZ short", b"MZ" + bytes(r.randrange(256) for _ in range(40))))
    C.append(case("MZ with calls",
                  b"MZ" + b"\x90" * 100 +
                  (b"\xe8\x10\x20\x00\x00\x48\x8b\xcf" * 3000)))
    C.append(case("MZ boundary e8",
                  b"MZ" + b"\x00" * 60 + b"\xe8\xff\xff\xff\xff" * 4000))
    C.append(case("all 256 bytes", bytes(range(256)) * 300))
    C.append(case("incompressible", os.urandom(200000)))
    C.append(case("MZ incompressible", b"MZ" + os.urandom(200000)))

    # block segmentation: mixed content, boundary-aligned sizes, many blocks
    text = b"the quick brown fox jumps over the lazy dog\n" * 2000
    code = (b"\xe8\x10\x20\x00\x00\x48\x8b\xcf\x55\x8b\xec" * 8000)
    for W in (8192, 8191, 8193, 65536):
        C.append(case(f"text|random @{W}", text[:W] + os.urandom(W) + text[:W]))
        C.append(case(f"code|random @{W}", code[:W] + os.urandom(W) + code[:W]))
    C.append(case("alternating 20 segments",
                  b"".join((text[:9000] if i % 2 else os.urandom(9000))
                           for i in range(20))))
    C.append(case("MZ then random tail", b"MZ" + code[:100000] + os.urandom(100000)))
    C.append(case("random then code", os.urandom(100000) + code[:100000]))
    C.append(case("all stored", os.urandom(400000)))

    # --- deflate recompression: the path where a wrong guess corrupts data ---
    import zlib
    payload = (b"<config><item name='x' value='1'/></config>\n" * 900)

    for lvl in (1, 6, 9):
        C.append(case(f"zlib stream lvl{lvl}", zlib.compress(payload, lvl)))
    for mem in (1, 8, 9):
        co = zlib.compressobj(6, zlib.DEFLATED, 15, mem)
        C.append(case(f"zlib memLevel{mem}", co.compress(payload) + co.flush()))
    # a strategy we deliberately do NOT search: must fall back, not corrupt
    co = zlib.compressobj(6, zlib.DEFLATED, 15, 8, zlib.Z_HUFFMAN_ONLY)
    C.append(case("zlib HUFFMAN_ONLY", co.compress(payload) + co.flush()))
    co = zlib.compressobj(6, zlib.DEFLATED, 15, 8, zlib.Z_RLE)
    C.append(case("zlib RLE", co.compress(payload) + co.flush()))
    # raw deflate and gzip wrappers
    co = zlib.compressobj(6, zlib.DEFLATED, -15)
    C.append(case("raw deflate", co.compress(payload) + co.flush()))
    co = zlib.compressobj(6, zlib.DEFLATED, 31)
    C.append(case("gzip stream", co.compress(payload) + co.flush()))

    z = zlib.compress(payload, 6)
    C.append(case("text + zlib + text", text[:5000] + z + text[:5000]))
    C.append(case("two zlib streams", z + z))
    C.append(case("zlib at EOF", text[:5000] + z))
    C.append(case("truncated zlib", text[:2000] + z[:len(z) // 2]))
    C.append(case("corrupt zlib body",
                  text[:2000] + z[:20] + bytes(r.randrange(256) for _ in range(60))
                  + z[80:]))
    C.append(case("zlib of random", zlib.compress(os.urandom(80000), 6)))
    C.append(case("nested zlib", zlib.compress(zlib.compress(payload, 6), 6)))
    C.append(case("many small zlib",
                  b"".join(zlib.compress(payload[:300] + bytes([i]), 6)
                           for i in range(200))))
    C.append(case("false zlib headers",
                  bytes((0x78 if i % 64 == 0 else r.randrange(256))
                        for i in range(60000))))
    # a real ZIP container
    import io, zipfile
    bio = io.BytesIO()
    with zipfile.ZipFile(bio, "w", zipfile.ZIP_DEFLATED) as zf:
        for i in range(12):
            zf.writestr(f"f{i}.xml", payload + bytes([i]))
    C.append(case("zip archive", bio.getvalue()))
    return C


# f2 and f1 are levels 102 and 101 in the header and are separate model
# configurations like every other preset, so they get their own round trips.
LEVELS = (9, 7, 5, 3, 2, 1, "f2", "f1")


def main():
    cases = build_cases()
    fails = []
    for name, data in cases:
        with open(TMP, "wb") as f:
            f.write(data)
        ref = hashlib.sha256(data).hexdigest()
        # Every preset is now a distinct model configuration -- different
        # context set, ISSE length and SSE stage count -- so every preset needs
        # its own round trip, not just the three endpoints.
        for lvl in LEVELS:
            c, o = TMP + ".c", TMP + ".o"
            if V2:
                # v2 takes the archive first and any number of inputs after it,
                # and extracts into a directory rather than to a named file.
                os.makedirs(ODIR, exist_ok=True)
                o = os.path.join(ODIR, os.path.basename(TMP))
                if os.path.exists(o):
                    os.remove(o)
                rc = subprocess.run(f'"{GEN}" c -{lvl} -q "{c}" "{TMP}"',
                                    shell=True, capture_output=True)
                rd = subprocess.run(f'"{GEN}" d -q "{c}" "{ODIR}"',
                                    shell=True, capture_output=True)
            else:
                rc = subprocess.run(f'"{GEN}" c -{lvl} "{TMP}" "{c}"',
                                    shell=True, capture_output=True)
                rd = subprocess.run(f'"{GEN}" d "{c}" "{o}"',
                                    shell=True, capture_output=True)
            ok = os.path.exists(o) and hashlib.sha256(
                open(o, "rb").read()).hexdigest() == ref
            if not ok:
                got = os.path.getsize(o) if os.path.exists(o) else -1
                fails.append(f"{name} -{lvl}: in={len(data)} out={got} "
                             f"rc={rc.returncode}/{rd.returncode} "
                             f"{rd.stderr.decode(errors='replace')[:80]}")
            for p in (c, o):
                if os.path.exists(p):
                    os.remove(p)
    os.remove(TMP)
    print(f"{len(cases)} cases x {len(LEVELS)} levels = "
          f"{len(cases)*len(LEVELS)} round trips")
    if fails:
        print(f"FAILURES: {len(fails)}")
        for f in fails[:30]:
            print("  " + f)
        sys.exit(1)
    print("all round trips exact")


main()
