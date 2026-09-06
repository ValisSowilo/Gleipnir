"""Semantic fuzzer for the v2 archive format: archives that are structurally
valid but internally inconsistent.

The other three suites each answer a different question, and none of them ask
this one.  fuzz.py and tfuzz.py round-trip *valid* archives.  gfuzz.py corrupts
valid archives at random, so its mutants almost always fail the header or index
checksum and are rejected before the decoder looks at a single segment field.

What none of them build is an archive whose magic, version, header hash, index
hash and segment table are all correct -- so it walks straight past arc_open --
but whose segment fields contradict each other: a stored section shorter than
the blocks that read from it, a rawlen unrelated to the working length, deflate
records out of order, a bits-per-symbol the encoder cannot emit.  Every
memory-safety bug found in the September 2026 decoder audit lived exactly
there, and all three existing suites passed clean both before and after those
bugs were fixed.

The contract is gfuzz.py's, unchanged.  For any input at all, gleipnir must do
exactly one of:

  * exit 0 and produce output identical to what was compressed, or
  * exit 1 or 2 and print a diagnostic

A crash, a hang, or any other exit status is a failure.  For the member-name
cases there is a second requirement, the one SECURITY.md states outright:
extraction must not write outside the destination directory.

  python sfuzz.py [--exe gleipnir] [-v]

Two positive controls run before any hostile case, because every failure mode
of a crafting script is silent: if the builder emits archives gleipnir rejects
for a *structural* reason, every hostile case is "rejected" too and the suite
passes without having tested anything.  Control 1 builds a valid archive with
this file's own builder and requires gleipnir to extract it byte exactly.
Control 2 re-derives the header and index hashes of a real archive with the
XXH64 below and requires both to match.  Either failing aborts the run.
"""
import hashlib, os, struct, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP  = os.path.join(tempfile.gettempdir(), "sfuzz")
TIMEOUT = 120
OK_EXITS = (0, 1, 2)

# ------------------------------------------------------------------ XXH64
# Canonical XXH64, seed 0, matching xxh64() in gleipnir.c.  Control 2 checks
# this against a real archive rather than trusting it.
P1 = 0x9E3779B185EBCA87
P2 = 0xC2B2AE3D27D4EB4F
P3 = 0x165667B19E3779F9
P4 = 0x85EBCA77C2B2AE63
P5 = 0x27D4EB2F165667C5
M  = 0xFFFFFFFFFFFFFFFF


def _rol(x, r):
    return ((x << r) | (x >> (64 - r))) & M


def _rnd(acc, v):
    return (_rol((acc + v * P2) & M, 31) * P1) & M


def _mrg(acc, v):
    return ((acc ^ _rnd(0, v)) * P1 + P4) & M


def xxh64(data, seed=0):
    n, i = len(data), 0
    if n >= 32:
        v1, v2 = (seed + P1 + P2) & M, (seed + P2) & M
        v3, v4 = seed & M, (seed - P1) & M
        while i <= n - 32:
            v1 = _rnd(v1, struct.unpack_from("<Q", data, i)[0]); i += 8
            v2 = _rnd(v2, struct.unpack_from("<Q", data, i)[0]); i += 8
            v3 = _rnd(v3, struct.unpack_from("<Q", data, i)[0]); i += 8
            v4 = _rnd(v4, struct.unpack_from("<Q", data, i)[0]); i += 8
        h = (_rol(v1, 1) + _rol(v2, 7) + _rol(v3, 12) + _rol(v4, 18)) & M
        h = _mrg(h, v1); h = _mrg(h, v2); h = _mrg(h, v3); h = _mrg(h, v4)
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 8 <= n:
        h = (_rol(h ^ _rnd(0, struct.unpack_from("<Q", data, i)[0]), 27) * P1 + P4) & M
        i += 8
    if i + 4 <= n:
        h = (_rol(h ^ ((struct.unpack_from("<I", data, i)[0] * P1) & M), 23) * P2 + P3) & M
        i += 4
    while i < n:
        h = (_rol(h ^ ((data[i] * P5) & M), 11) * P1) & M
        i += 1
    h ^= h >> 33; h = (h * P2) & M
    h ^= h >> 29; h = (h * P3) & M
    h ^= h >> 32
    return h


# --------------------------------------------------------- format constants
HDR_BYTES, TRL_BYTES = 48, 20
MAGIC, VERSION = 0x414E4547, 2                 # "GENA"
SEG_STORED, SEG_MODEL = 0, 1
B_MODEL, B_X86, B_STORE, B_ALPHA = 0, 1, 2, 3


def seg_model(bps=0, sym=b"", wn=0, dfl=(), stride=0, width=1, blocks=(),
              alen=0, slen=None, ain=b"", sin=b"", nd_field=None, nb_field=None,
              nsym_field=None):
    """One SEG_MODEL blob.  Fields are written exactly as given, including ones
    that contradict each other -- that is the whole point of this file.  Only
    slen defaults to the honest value, so a case lies about one thing at a time.

    The *_field arguments write a count that disagrees with the records that
    follow it, which no honest encoder can produce and which is otherwise
    unreachable through the normal arguments."""
    b = bytearray()
    b += struct.pack("<BBB", SEG_MODEL, bps,
                     len(sym) if nsym_field is None else nsym_field)
    b += sym
    b += struct.pack("<QI", wn, len(dfl) if nd_field is None else nd_field)
    for pos, clen, plen, wbits, level, strat, mem in dfl:
        b += struct.pack("<QII", pos, clen, plen)
        b += struct.pack("<BBB", wbits & 0xFF, (level | (strat << 4)) & 0xFF, mem)
    b += struct.pack("<HB", stride, width)
    b += struct.pack("<I", len(blocks) if nb_field is None else nb_field)
    for ty, ln in blocks:
        b += struct.pack("<BI", ty, ln)
    if slen is None:
        slen = sum(ln for ty, ln in blocks if (ty & 3) == B_STORE)
    b += struct.pack("<QQ", alen, slen)
    return bytes(b) + ain + sin


def seg_stored(payload):
    return bytes([SEG_STORED]) + payload


def build(path, name, blob, rawlen, seghash=0, sha=b"\0" * 32, lvl=5,
          memshift=0, segoff=None, seg0=0, nseg=1):
    """A complete archive: one member, one segment, with a correct header hash,
    index hash and trailer.  Everything arc_open verifies is right, so the
    decoder is always the thing under test."""
    idxoff = HDR_BYTES + len(blob)
    off = HDR_BYTES if segoff is None else segoff

    ix = bytearray()
    ix += struct.pack("<Q", 1)                          # member count
    nb = name.encode("utf-8", "surrogateescape")
    ix += struct.pack("<H", len(nb)) + nb
    ix += struct.pack("<QQI", rawlen, 0, 0o644)         # size, mtime, mode
    ix += sha
    ix += struct.pack("<QQ", seg0, nseg)
    ix += struct.pack("<Q", 1)                          # segment count
    ix += struct.pack("<QQQQQ", off, len(blob), rawlen, seghash, 0)
    ix += struct.pack("<QQ", 0, 0)                      # pgroup, np

    hdr = bytearray(HDR_BYTES)
    struct.pack_into("<IHH", hdr, 0, MAGIC, VERSION, 0)
    hdr[8] = lvl & 0xFF
    hdr[9] = (memshift + 16) & 0xFF
    struct.pack_into("<I", hdr, 12, 64 << 20)           # segmax
    struct.pack_into("<QQQ", hdr, 16, 1, idxoff, rawlen)
    struct.pack_into("<Q", hdr, 40, xxh64(bytes(hdr[:40])))

    with open(path, "wb") as f:
        f.write(bytes(hdr))
        f.write(blob)
        f.write(bytes(ix))
        f.write(struct.pack("<QQI", xxh64(bytes(ix)), len(ix), MAGIC))


# -------------------------------------------------------------------- cases
def cases():
    C = []

    def add(nm, note, blob, rawlen, **kw):
        C.append((nm, note, blob, rawlen, kw))

    plain = b"A" * 4096
    store1 = ((B_STORE, 4096),)

    # the stored section, and the blocks that read out of it
    add("stored_section_absent", "a 64 MB stored block with no bytes behind it",
        seg_model(wn=64 << 20, blocks=((B_STORE, 64 << 20),), slen=0), 64 << 20)
    add("stored_section_short", "blocks claim 4096 stored bytes, section has 16",
        seg_model(wn=4096, blocks=store1, slen=16, sin=b"x" * 16), 4096)
    add("stored_section_long", "section carries more than the blocks account for",
        seg_model(wn=4096, blocks=store1, slen=8192, sin=b"x" * 8192), 4096)

    # bits per symbol: scan_alphabet only ever emits 1, 2 or 4
    for bps in (3, 5, 6, 7, 8):
        add("bps_%d" % bps,
            "bps %d; unpack_syms masks with (1<<bps)-1 into sym[16]" % bps,
            seg_model(bps=bps, sym=bytes(range(16)), wn=4096, blocks=store1,
                      sin=b"\xff" * 4096), 4096)
    add("bps_nsym_zero", "bps 4 with an empty symbol table",
        seg_model(bps=4, wn=4096, blocks=store1, sin=b"\xff" * 4096), 4096)
    add("bps_nsym_over_alphabet", "bps 1 with 16 symbols; index exceeds 1<<bps",
        seg_model(bps=1, sym=bytes(range(16)), wn=4096, blocks=store1,
                  sin=b"\xff" * 4096), 4096)
    add("nsym_field_overlong", "symbol count larger than the table that follows",
        seg_model(bps=4, sym=bytes(range(4)), nsym_field=16, wn=4096,
                  blocks=store1, sin=b"\xff" * 4096), 4096)

    # rawlen against the buffer it is actually read out of
    add("rawlen_over_wn_packed",
        "packed: 64 working bytes, index claims 256 MB of symbols",
        seg_model(bps=4, sym=bytes(range(16)), wn=64, blocks=((B_STORE, 64),),
                  sin=b"\x5a" * 64), 256 << 20)
    add("rawlen_over_wn_plain", "plain: 4096 working bytes, index claims 64 MB",
        seg_model(wn=4096, blocks=store1, sin=plain), 64 << 20)
    add("rawlen_zero_wn_large", "index claims an empty member, blob is not",
        seg_model(wn=4096, blocks=store1, sin=plain), 0)

    # deflate records
    add("dfl_descending", "second record starts before the first ends",
        seg_model(wn=4096, dfl=((8, 2, 0, -15, 6, 0, 8),
                                (0, 2, 0, -15, 6, 0, 8)),
                  blocks=store1, sin=plain), 4096)
    add("dfl_overlapping", "second record starts inside the first",
        seg_model(wn=4096, dfl=((0, 2, 100, -15, 6, 0, 8),
                                (50, 2, 100, -15, 6, 0, 8)),
                  blocks=store1, sin=plain), 4096)
    add("dfl_pos_past_wn", "record positioned past the working buffer",
        seg_model(wn=4096, dfl=((1 << 30, 2, 0, -15, 6, 0, 8),),
                  blocks=store1, sin=plain), 4096)
    add("dfl_plen_past_end", "record plaintext runs off the working buffer",
        seg_model(wn=4096, dfl=((4000, 2, 1 << 20, -15, 6, 0, 8),),
                  blocks=store1, sin=plain), 4096)
    add("dfl_count_over_cap", "record count past MAXDFL, no records behind it",
        seg_model(wn=4096, nd_field=1 << 20, blocks=store1, sin=plain), 4096)
    add("dfl_count_without_records", "in-range record count, no records behind it",
        seg_model(wn=4096, nd_field=8, blocks=store1, sin=plain), 4096)

    # block table
    add("blocks_sum_under_wn", "block lengths sum to less than wn",
        seg_model(wn=8192, blocks=store1, sin=plain), 8192)
    add("blocks_sum_over_wn", "block lengths sum to more than wn",
        seg_model(wn=2048, blocks=store1, sin=plain), 2048)
    add("blocks_none_wn_set", "no blocks at all, non-zero wn",
        seg_model(wn=4096, slen=0), 4096)
    add("blocks_count_over_cap", "block count past MAXBLK",
        seg_model(wn=4096, nb_field=1 << 20, blocks=store1, sin=plain), 4096)
    add("blocks_count_without_entries", "in-range block count, no entries behind it",
        seg_model(wn=4096, nb_field=64, blocks=store1, sin=plain), 4096)
    add("wn_absurd", "working length past the 2^40 cap",
        seg_model(wn=1 << 44, blocks=store1, sin=plain), 4096)
    add("alen_absurd", "arithmetic section longer than the blob",
        seg_model(wn=4096, blocks=store1, alen=1 << 40, sin=plain), 4096)

    # truncation inside the blob itself
    full = seg_model(wn=4096, blocks=store1, sin=plain)
    add("blob_truncated_mid_header", "blob cut inside its own header", full[:6], 4096)
    add("blob_truncated_mid_blocks", "blob cut inside the block table", full[:20], 4096)
    add("blob_empty", "zero-length segment blob", b"", 4096)
    add("blob_kind_unknown", "kind byte is neither stored nor model",
        bytes([7]) + full[1:], 4096)
    add("stored_kind_short", "stored segment shorter than its recorded rawlen",
        seg_stored(b"only a few bytes"), 4096)

    # header fields the writer could never have produced
    add("header_preset_absurd", "header names a preset that does not exist",
        full, 4096, lvl=200)
    add("header_preset_zero", "header names preset 0", full, 4096, lvl=0)
    add("header_memshift_high", "header names -m100", full, 4096, memshift=100)
    add("header_memshift_max", "header names the largest byte-encodable -m",
        full, 4096, memshift=239)

    # index geometry
    add("segment_offset_in_header", "segment offset points into the header",
        full, 4096, segoff=0)
    add("segment_offset_past_index", "segment offset points past the payload",
        full, 4096, segoff=1 << 30)
    add("member_range_past_table", "member claims more segments than exist",
        full, 4096, nseg=64)
    add("member_seg0_past_table", "member starts past the end of the table",
        full, 4096, seg0=99)
    return C


# Member names.  These carry the second requirement: nothing may be written
# outside the destination.  On POSIX a backslash is an ordinary filename
# character, so those cases test the refusal; on Windows they are separators
# and an unrefused name escapes for real.
NAME_CASES = [
    ("traversal_backslash",  "..\\..\\sfuzz_escaped.txt"),
    ("traversal_slash",      "../../sfuzz_escaped.txt"),
    ("traversal_mixed",      "ok/..\\..\\sfuzz_escaped.txt"),
    ("traversal_deep",       "a/b/../../../sfuzz_escaped.txt"),
    ("traversal_absolute",   "/tmp/sfuzz_escaped.txt"),
    ("traversal_drive",      "C:\\sfuzz_escaped.txt"),
    ("traversal_unc",        "\\\\srv\\share\\sfuzz_escaped.txt"),
    ("traversal_dotdot_only", ".."),
    ("name_escape_sequence", "ok\x1b[31mred\x1b[0m.txt"),
    ("name_newline",         "ok\nsecond.txt"),
    ("name_empty",           ""),
]


# ------------------------------------------------------------------- runner
def run(args):
    """(rc, stderr).  A timeout is a failure, not a retry: this tool must never
    hang on malformed input."""
    try:
        p = subprocess.run(args, capture_output=True, timeout=TIMEOUT)
        return p.returncode, p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return "TIMEOUT", ""
    except OSError as e:
        return "OSERROR:%s" % e, ""


def tree(root):
    out = set()
    for d, _, fs in os.walk(root):
        for f in fs:
            out.add(os.path.join(d, f))
    return out


def link_cases(GEN):
    """Extraction must not follow a link that is already sitting in the
    destination.  name_is_safe proves things about the *name* -- relative, no
    drive letter, no ".." -- and can prove nothing about what is already on the
    disk.  A well-formed name still lands outside the destination if some
    component of it, or the member file itself, is a link that was put there
    earlier.

    Returns a list of failures, or None where links cannot be created at all
    (Windows without Developer Mode or an elevated shell), in which case this
    machine simply cannot run the check."""
    root = os.path.join(TMP, "link")
    outside = os.path.join(root, "outside")
    os.makedirs(outside, exist_ok=True)
    secret = os.path.join(outside, "secret.txt")
    with open(secret, "w") as f:
        f.write("UNTOUCHED")
    try:
        os.symlink(outside, os.path.join(root, "probe"), target_is_directory=True)
    except (OSError, NotImplementedError, AttributeError):
        return None

    body = b"escaped\n"
    sha = hashlib.sha256(body).digest()
    fails = []

    # A path component of the destination is a link pointing out of it.
    d1 = os.path.join(root, "d1")
    os.makedirs(d1, exist_ok=True)
    os.symlink(outside, os.path.join(d1, "sub"), target_is_directory=True)
    p1 = os.path.join(TMP, "link_component.gl")
    build(p1, "sub/evil.txt", seg_stored(body), len(body),
          seghash=xxh64(body), sha=sha)
    rc, _ = run([GEN, "x", "-q", p1, d1])
    if rc not in OK_EXITS:
        fails.append("link_component: exit %r" % rc)
    if os.path.exists(os.path.join(outside, "evil.txt")):
        fails.append("link_component: wrote through a directory link, "
                     "creating %s" % os.path.join(outside, "evil.txt"))

    # The member file itself already exists and is a link to something else.
    d2 = os.path.join(root, "d2")
    os.makedirs(d2, exist_ok=True)
    os.symlink(secret, os.path.join(d2, "plain.txt"))
    p2 = os.path.join(TMP, "link_member.gl")
    build(p2, "plain.txt", seg_stored(body), len(body),
          seghash=xxh64(body), sha=sha)
    rc, _ = run([GEN, "x", "-q", p2, d2])
    if rc not in OK_EXITS:
        fails.append("link_member: exit %r" % rc)
    with open(secret) as f:
        if f.read() != "UNTOUCHED":
            fails.append("link_member: followed the link and overwrote %s" % secret)

    # The opposite requirement, and the one this file originally missed: a
    # destination the user chose is theirs, and the route to it is very often a
    # link through nobody's fault -- /tmp and /var are symlinks to /private/* on
    # macOS.  Refusing those refused every extraction into a temporary directory
    # on that platform, which CI caught and this suite did not.  Only the tail
    # of the path, the part the archive named, is attacker-controlled.
    real = os.path.join(root, "real_dest")
    os.makedirs(real, exist_ok=True)
    via = os.path.join(root, "via_link")
    os.symlink(real, via, target_is_directory=True)
    p3 = os.path.join(TMP, "link_ok.gl")
    build(p3, "sub/ok.txt", seg_stored(body), len(body),
          seghash=xxh64(body), sha=sha)
    rc, err = run([GEN, "x", "-q", p3, via])
    if rc != 0:
        fails.append("link_destination: refused a legitimate destination reached "
                     "through a symlink (exit %r) -- %s" % (rc, err.strip()))
    elif not os.path.exists(os.path.join(real, "sub", "ok.txt")):
        fails.append("link_destination: exit 0 but wrote nothing")
    return fails


def controls(GEN):
    payload = bytes(range(256)) * 8
    ctrl = os.path.join(TMP, "control.gl")
    build(ctrl, "control.bin", seg_stored(payload), len(payload),
          seghash=xxh64(payload), sha=hashlib.sha256(payload).digest())
    dest = os.path.join(TMP, "control_out")
    os.makedirs(dest, exist_ok=True)
    rc, err = run([GEN, "x", "-q", ctrl, dest])
    got = os.path.join(dest, "control.bin")
    if rc != 0 or not os.path.exists(got) or open(got, "rb").read() != payload:
        sys.exit("sfuzz: CONTROL 1 FAILED -- a valid archive from this file's own\n"
                 "  builder did not round trip (rc=%r).  Every crafted archive below\n"
                 "  would then be rejected for the wrong reason and the suite would\n"
                 "  pass without testing anything.  Fix the builder first.\n%s"
                 % (rc, err))

    real = os.path.join(TMP, "real.gl")
    src = os.path.join(TMP, "src.bin")
    with open(src, "wb") as f:
        f.write(payload * 40)
    rc, err = run([GEN, "c", "-1", "-q", real, src])
    if rc != 0:
        sys.exit("sfuzz: CONTROL 2 FAILED -- could not create a reference "
                 "archive (rc=%r)\n%s" % (rc, err))
    d = open(real, "rb").read()
    ixh, ixn = struct.unpack_from("<QQ", d, len(d) - TRL_BYTES)
    ix = d[len(d) - TRL_BYTES - ixn:len(d) - TRL_BYTES]
    if xxh64(ix) != ixh or xxh64(d[:40]) != struct.unpack_from("<Q", d, 40)[0]:
        sys.exit("sfuzz: CONTROL 2 FAILED -- the XXH64 in this file does not match\n"
                 "  the one in gleipnir.c, so every crafted archive would be rejected\n"
                 "  at the header or index and nothing below would be reached.")
    print("controls passed: builder round trips, XXH64 matches the archive")


def main():
    exe = "gleipnir.exe" if os.name == "nt" else "gleipnir"
    verbose = False
    a, i = sys.argv[1:], 0
    while i < len(a):
        if a[i] == "--exe" and i + 1 < len(a):
            exe = a[i + 1]; i += 2
        elif a[i] in ("-v", "--verbose"):
            verbose = True; i += 1
        else:
            sys.exit("sfuzz: unknown argument %r\n"
                     "  usage: sfuzz.py [--exe EXE] [-v]" % a[i])
    GEN = exe if os.path.isabs(exe) else os.path.join(HERE, exe)
    if not os.path.exists(GEN):
        sys.exit("sfuzz: %s does not exist.\n"
                 "  usage: sfuzz.py [--exe EXE]   (the argument is an executable,\n"
                 "  not a trial count)" % GEN)

    shutil.rmtree(TMP, ignore_errors=True)
    os.makedirs(TMP, exist_ok=True)
    controls(GEN)
    fails = []

    C = cases()
    for nm, note, blob, rawlen, kw in C:
        p = os.path.join(TMP, nm + ".gl")
        build(p, "m.bin", blob, rawlen, **kw)
        for mode in (["t", "-D", "-t1"], ["x", "-q"]):
            out = os.path.join(TMP, "out_" + nm)
            os.makedirs(out, exist_ok=True)
            cmd = [GEN] + mode + ([p] if mode[0] == "t" else [p, out])
            rc, err = run(cmd)
            if rc not in OK_EXITS:
                fails.append("%s [%s]: exit %r -- %s" % (nm, mode[0], rc, note))
            elif rc != 0 and not err.strip():
                fails.append("%s [%s]: non-zero exit with no diagnostic" % (nm, mode[0]))
        if verbose:
            print("  %-30s %s" % (nm, note))

    # Member names.  Extraction runs in TMP/n_<nm>/dest/sub; anything appearing
    # anywhere else under TMP during that run escaped the destination.
    for nm, member in NAME_CASES:
        sub = os.path.join(TMP, "n_" + nm, "dest", "sub")
        os.makedirs(sub, exist_ok=True)
        body = b"escaped\n"
        p = os.path.join(TMP, "n_" + nm + ".gl")
        build(p, member, seg_stored(body), len(body), seghash=xxh64(body),
              sha=hashlib.sha256(body).digest())
        before = tree(TMP)
        rc, err = run([GEN, "x", "-q", p, sub])
        new = tree(TMP) - before
        if rc not in OK_EXITS:
            fails.append("name %s: exit %r" % (nm, rc))
        escaped = [f for f in new if os.path.commonpath([f, sub]) != sub]
        if escaped:
            fails.append("name %s (%r): wrote OUTSIDE the destination: %s"
                         % (nm, member,
                            ", ".join(os.path.relpath(f, TMP) for f in escaped)))
        if verbose:
            print("  %-30s member=%r rc=%s" % (nm, member, rc))

    lf = link_cases(GEN)
    if lf is None:
        print("note: symlink cases skipped -- this host cannot create links "
              "(Windows needs Developer Mode or an elevated shell)")
    else:
        fails.extend(lf)

    print("%d segment-field cases x 2 modes + %d member-name cases%s = %d runs"
          % (len(C), len(NAME_CASES), "" if lf is None else " + 3 link cases",
             len(C) * 2 + len(NAME_CASES) + (0 if lf is None else 3)))
    if fails:
        print("\n%d FAILURES:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1
    print("no crash, no hang, no escape; every rejection carried a diagnostic")
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
