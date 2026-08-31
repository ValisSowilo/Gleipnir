"""Generate the Standard-corpora ranking graphs as dependency-free SVG.

Companion to mkgraphs.py, which owns the Silesia charts.  This one draws the
four extra corpora from the README "Standard corpora" section: a horizontal
bar per codec, sorted best-first, with the gleipnir rows highlighted so where the
engine lands in each field is readable at a glance.

The numbers are embedded here as constants rather than read from a JSON, for
the same reason MEMSWEEP and GATES are embedded in mkgraphs.py: part of the
field is not ours to re-run.  Provenance is per row and per corpus:

  * gleipnir rows were measured here on the released 1.0.0 binary, single stream,
    round-trip SHA-256 verified.
  * enwik8 and enwik9 competitor sizes are the published figures from Matt
    Mahoney's Large Text Compression Benchmark (mattmahoney.net/dc/text.html),
    measured on his hardware.  They are sizes only -- bits per byte is a pure
    size measure and machine-independent, which is why these charts plot bpc
    and never time.
  * Calgary and Canterbury were run in full here, every codec on this machine.

  python mkcorpora.py

Reuses the Plot toolkit from mkgraphs.py (imported by path, so it works whether
this file sits at the repo root or under a scripts/ directory).
"""
import os, importlib.util

SELF = os.path.dirname(os.path.abspath(__file__))   # this script's own dir
ROOT = os.path.dirname(SELF)                         # repo root (scripts/ is one down)
_spec = importlib.util.spec_from_file_location(
    "mkgraphs", os.path.join(SELF, "mkgraphs.py"))
_mkg = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mkg)
Plot = _mkg.Plot

OUT = os.environ.get("GRAPHOUT", os.path.join(ROOT, "graphs"))
os.makedirs(OUT, exist_ok=True)

GLEIPNIR = "#0969da"     # the highlight blue the Silesia charts already use
# Reference families, one colour each, legible on both themes.
FAM = {
    "zpaq": "#d29922", "lpaq1": "#8250df", "xz": "#1a7f37",
    "brotli": "#cf222e", "bzip2": "#6e7781", "gzip": "#953800",
    "paq8px": "#bf3989", "cmix": "#1f6feb", "nncp": "#3fb950",
}


def colour(name):
    if name.startswith("gleipnir"):
        return GLEIPNIR
    return FAM.get(name.split()[0], "#6e7781")


# (codec, size_bytes, source) per corpus; source is "here" or "LTCB".
CORPORA = [
    ("enwik8_ranking.svg", "enwik8", 100_000_000,
     "enwik8 — 100 MB of Wikipedia text",
     "gleipnir measured here on the released binary, round-trip verified. The rest "
     "are LTCB published sizes (Mahoney's hardware); bits per byte is "
     "size-only, so it is machine-independent and comparable.",
     [("cmix v21", 14_623_723, "LTCB"), ("nncp v3.2", 14_915_298, "LTCB"),
      ("paq8px -12L", 15_849_084, "LTCB"), ("zpaq -max", 17_855_729, "LTCB"),
      ("gleipnir -9", 18_810_676, "here"), ("lpaq1 -9", 19_755_948, "LTCB"),
      ("gleipnir -5", 19_660_660, "here"), ("xz -9e", 24_703_772, "LTCB"),
      ("brotli -q11", 25_764_698, "LTCB"), ("bzip2 -9", 29_008_736, "LTCB"),
      ("gzip -9", 36_445_248, "LTCB")]),

    ("enwik9_ranking.svg", "enwik9", 1_000_000_000,
     "enwik9 — 1 GB of Wikipedia text",
     "gleipnir measured here, round-trip verified. The rest are LTCB published "
     "sizes (Mahoney's hardware); bpc is size-only and machine-independent.",
     [("nncp v3.2", 106_632_363, "LTCB"), ("cmix v21", 107_963_380, "LTCB"),
      ("paq8px -12L", 124_696_410, "LTCB"), ("zpaq -max", 142_252_605, "LTCB"),
      ("gleipnir -9", 157_073_377, "here"), ("lpaq1 -9", 164_508_919, "LTCB"),
      ("gleipnir -5", 167_360_632, "here"), ("xz tuned", 197_331_816, "LTCB"),
      ("brotli", 223_597_884, "LTCB"), ("bzip2 -9", 253_977_839, "LTCB"),
      ("gzip -9", 322_591_995, "LTCB")]),

    ("calgary_ranking.svg", "Calgary", 3_141_622,
     "Calgary — 3,141,622 bytes, fourteen files",
     "Every codec measured here on this machine, one concatenated stream in "
     "canonical file order. At this size time is startup-dominated, so the "
     "comparison is on size.",
     [("paq8px -8", 560_705, "here"), ("gleipnir -9", 651_967, "here"),
      ("zpaq -m5", 659_513, "here"), ("gleipnir -7", 661_432, "here"),
      ("lpaq1 -6", 682_211, "here"), ("xz -9e", 819_440, "here"),
      ("bzip2 -9", 859_448, "here"), ("gzip -9", 1_021_855, "here")]),

    ("canterbury_ranking.svg", "Canterbury", 2_810_784,
     "Canterbury — 2,810,784 bytes, eleven files",
     "Every codec measured here on this machine, one concatenated stream in "
     "canonical file order. Size comparison; times at this scale are "
     "startup-dominated.",
     [("paq8px -8", 302_791, "here"), ("gleipnir -9", 355_766, "here"),
      ("gleipnir -7", 359_844, "here"), ("zpaq -m5", 362_880, "here"),
      ("lpaq1 -6", 388_787, "here"), ("xz -9e", 483_616, "here"),
      ("bzip2 -9", 569_486, "here"), ("gzip -9", 735_312, "here")]),
]


def ranking(path, insize, title, sub, rows):
    rows = sorted(rows, key=lambda r: r[1])              # best (smallest) first
    data = [(nm, s * 8.0 / insize, src) for nm, s, src in rows]
    n = len(data)
    xmax = data[-1][1]
    band = 30
    w = 860

    p = Plot(w=w, h=0, title=title, sub=sub,
             xlab="bits per byte — lower is better", ylab="")
    p.ml, p.mr = 128, 150
    p.h = p.mt + n * band + p.mb
    xhi = xmax * 1.14
    p.limits(0, xhi, 0, n)
    step = 0.5
    ticks = [step * i for i in range(int(xhi / step) + 1)]
    p.grid(ticks, [], lambda v: f"{v:g}", str)

    for k, (nm, bpc, src) in enumerate(data):
        top = p.py(n - k)
        bot = p.py(n - k - 1)
        cy = (top + bot) / 2
        c = colour(nm)
        is_gleipnir = nm.startswith("gleipnir")
        p.bar(p.px(0), top + band * 0.17, p.px(bpc) - p.px(0), band * 0.66,
              c, 1.0 if is_gleipnir else 0.70)
        # codec name, right-aligned in the left margin
        p.rawtext(p.ml - 10, cy + 4, nm, cls="tagb" if is_gleipnir else "tag",
                  anchor="end", color=c if is_gleipnir else None)
        # bpc at the bar end, with a light LTCB marker where it applies
        lab = f"{bpc:.3f}"
        p.rawtext(p.px(bpc) + 7, cy + 4, lab, cls="tagb", anchor="start",
                  color=c)
        if src == "LTCB":
            p.rawtext(p.px(bpc) + 7 + len(lab) * 7.0 + 6, cy + 4, "LTCB",
                      cls="sub", anchor="start")

    p.topnote("gleipnir in blue; bars are bits per byte", anchor="end")
    p.render(os.path.join(OUT, path))


def main():
    for path, _name, insize, title, sub, rows in CORPORA:
        ranking(path, insize, title, sub, rows)


if __name__ == "__main__":
    main()
