"""Generate the benchmark graphs as dependency-free SVG.

matplotlib is not installed and this project has no Python dependencies, so the
plots are emitted directly.  SVG also renders sharply inside README.md on GitHub
and stays diffable in git, which a PNG would not.

Inputs:
  bench_final.json   gen, six presets, written by bench_final.py
  bench_refs.json    zpaq / lpaq1 / xz / brotli / bzip2, written by bench_refs.ps1

Output: graphs/*.svg

Every number plotted is measured.  Points that come from a *different* session
or a different machine configuration are drawn hollow and labelled, so they are
never silently mixed with the single-session sweep.

  python mkgraphs.py
"""
import os, json, math

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root (this file lives in scripts/)
DATA = os.environ.get("GRAPHDATA", HERE)          # where the json lives
OUT = os.environ.get("GRAPHOUT", os.path.join(HERE, "graphs"))
os.makedirs(OUT, exist_ok=True)

# ---------------------------------------------------------------- svg toolkit

STYLE = """
  .bg   { fill: #ffffff; }
  .fg   { fill: #1b1f23; }
  .grid { stroke: #d8dee4; stroke-width: 1; }
  .axis { stroke: #57606a; stroke-width: 1.2; }
  .lbl  { fill: #57606a; font: 12px system-ui, -apple-system, Segoe UI, sans-serif; }
  .ttl  { fill: #1b1f23; font: 600 15px system-ui, -apple-system, Segoe UI, sans-serif; }
  .sub  { fill: #57606a; font: 11px system-ui, -apple-system, Segoe UI, sans-serif; }
  .tag  { fill: #1b1f23; font: 11px system-ui, -apple-system, Segoe UI, sans-serif; }
  .tagb { fill: #1b1f23; font: 600 11px system-ui, -apple-system, Segoe UI, sans-serif; }
  .gen  { stroke: #0969da; fill: #0969da; }
  .genl { stroke: #0969da; stroke-width: 2.2; fill: none; }
  .oldl { stroke: #8c959f; stroke-width: 2; fill: none; stroke-dasharray: 5 4; }
  .oldd { stroke: #8c959f; fill: none; stroke-width: 2; }
  @media (prefers-color-scheme: dark) {
    .bg   { fill: #0d1117; }
    .fg   { fill: #e6edf3; }
    .grid { stroke: #30363d; }
    .axis { stroke: #8b949e; }
    .lbl  { fill: #8b949e; }
    .ttl  { fill: #e6edf3; }
    .sub  { fill: #8b949e; }
    .tag  { fill: #e6edf3; }
    .tagb { fill: #e6edf3; }
    .gen  { stroke: #58a6ff; fill: #58a6ff; }
    .genl { stroke: #58a6ff; }
    .oldl { stroke: #6e7681; }
    .oldd { stroke: #6e7681; }
  }
"""

# Reference colours chosen to stay legible on both themes.
CLR = {
    "zpaq -m5":    "#d29922",
    "lpaq1 -6":    "#8250df",
    "xz -9e":      "#1a7f37",
    "brotli -q11": "#cf222e",
    "bzip2 -9":    "#6e7781",
    "gzip -9":     "#953800",
    "paq8px -6":   "#bf3989",
}


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


# 11px system-ui averages a shade under 6px per character across the label
# strings used here; 5.6 is deliberately a slight over-estimate so the wrap and
# the collision boxes both err towards leaving room.
CW = 5.6


def textw(s, size=11):
    return len(s) * CW * size / 11.0


def wrap(s, width_px):
    """Greedy word wrap to a pixel width.  The subtitles carry the measurement
    protocol and are long by design -- they used to run off the right edge and
    lose the half that says which session the numbers came from."""
    out, cur = [], ""
    for w in s.split():
        trial = (cur + " " + w) if cur else w
        if cur and textw(trial) > width_px:
            out.append(cur)
            cur = w
        else:
            cur = trial
    if cur:
        out.append(cur)
    return out


class Labels:
    """Deferred point labels, placed where they do not collide.

    A scatter with eight presets and seven reference codecs has clusters where
    four labels want the same pixels -- drawing them at a fixed offset stacks
    them into an unreadable smear.  Each label is offered a ring of candidate
    offsets and takes the first that clears every box already placed, every
    marker, and the plot frame.  If nothing clears, the first candidate is used
    anyway: a slightly crowded label beats a silently dropped one.
    """

    def __init__(self, plot):
        self.p = plot
        self.items = []
        self.blocked = []          # marker boxes, kept clear of text

    def marker(self, px, py, r=6):
        self.blocked.append((px - r, py - r, px + r, py + r))

    def add(self, px, py, s, cls="tag", color=None, size=11):
        self.items.append((px, py, s, cls, color, size))

    # candidate (dx, dy, anchor) offsets, in preference order: above, below,
    # right, left, then the diagonals, then a second ring further out
    CAND = []
    for _d in (11, 20, 30):
        CAND += [(0, -_d, "middle"), (0, _d + 3, "middle"),
                 (8, 4, "start"), (-8, 4, "end"),
                 (8, -_d + 6, "start"), (-8, -_d + 6, "end"),
                 (8, _d, "start"), (-8, _d, "end")]

    def _box(self, px, py, s, dx, dy, anchor, size):
        w = textw(s, size)
        x = px + dx
        if anchor == "middle":
            x -= w / 2
        elif anchor == "end":
            x -= w
        return (x - 2, py + dy - size + 1, x + w + 2, py + dy + 3)

    @staticmethod
    def _hit(a, b):
        return not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])

    def flush(self):
        p = self.p
        frame = (p.ml + 1, p.mt + 1, p.w - p.mr - 1, p.h - p.mb - 1)
        placed = list(self.blocked)
        # Densest region first: the labels with the least room get to pick
        # before the ones that have space to spare.
        order = sorted(range(len(self.items)), key=lambda i: -sum(
            1 for j, o in enumerate(self.items)
            if j != i and abs(o[0] - self.items[i][0]) < 70
            and abs(o[1] - self.items[i][1]) < 24))
        for i in order:
            px, py, s, cls, color, size = self.items[i]
            pick = None
            for dx, dy, anchor in self.CAND:
                b = self._box(px, py, s, dx, dy, anchor, size)
                if b[0] < frame[0] or b[2] > frame[2] or \
                   b[1] < frame[1] or b[3] > frame[3]:
                    continue
                if any(self._hit(b, q) for q in placed):
                    continue
                pick = (dx, dy, anchor, b)
                break
            if pick is None:
                dx, dy, anchor = self.CAND[0]
                pick = (dx, dy, anchor, self._box(px, py, s, dx, dy, anchor, size))
            placed.append(pick[3])
            p.rawtext(px + pick[0], py + pick[1], s, cls=cls,
                      anchor=pick[2], color=color)


class Plot:
    """Linear or log axes, pixel space with y flipped."""

    def __init__(self, w=760, h=470, title="", sub="", xlab="", ylab="",
                 xlog=False, ylog=False):
        self.w, self.h = w, h
        self.ml, self.mr, self.mb = 66, 24, 56
        self.xlog, self.ylog = xlog, ylog
        self.title, self.sub, self.xlab, self.ylab = title, sub, xlab, ylab
        # The subtitle wraps rather than running off the plate, so the top
        # margin has to follow it: a three-line protocol note needs 26px more
        # headroom than a one-line one, and hardcoding 52 clipped both the text
        # and whatever annotation sat just under it.
        self.tx = max(14, self.ml - 46)
        self.subs = wrap(sub, w - self.tx - 16) if sub else []
        self.mt = 30 + 15 * len(self.subs) + 16
        self.body = []
        self.x0 = self.y0 = 0.0
        self.x1 = self.y1 = 1.0

    # -- coordinate mapping
    def limits(self, x0, x1, y0, y1):
        f = (lambda v: math.log10(v)) if self.xlog else (lambda v: v)
        g = (lambda v: math.log10(v)) if self.ylog else (lambda v: v)
        self.x0, self.x1 = f(x0), f(x1)
        self.y0, self.y1 = g(y0), g(y1)

    def px(self, x):
        v = math.log10(x) if self.xlog else x
        span = self.w - self.ml - self.mr
        return self.ml + (v - self.x0) / (self.x1 - self.x0) * span

    def py(self, y):
        v = math.log10(y) if self.ylog else y
        span = self.h - self.mt - self.mb
        return self.h - self.mb - (v - self.y0) / (self.y1 - self.y0) * span

    # -- primitives
    def line(self, pts, cls="genl", extra=""):
        d = " ".join(f"{'M' if i == 0 else 'L'}{self.px(x):.1f},{self.py(y):.1f}"
                     for i, (x, y) in enumerate(pts))
        self.body.append(f'<path class="{cls}" d="{d}" {extra}/>')

    def dot(self, x, y, r=5, cls="gen", color=None, hollow=False):
        st = f'class="{cls}"' if color is None else \
             f'stroke="{color}" fill="{"none" if hollow else color}"'
        extra = ' fill="none" stroke-width="2"' if hollow and color is None else ""
        if hollow and color is not None:
            extra = ' stroke-width="2"'
        self.body.append(
            f'<circle cx="{self.px(x):.1f}" cy="{self.py(y):.1f}" r="{r}" '
            f'{st}{extra}/>')

    def text(self, x, y, s, dx=0, dy=0, cls="tag", anchor="start", color=None):
        c = f' fill="{color}"' if color else ""
        self.body.append(
            f'<text class="{cls}" x="{self.px(x) + dx:.1f}" '
            f'y="{self.py(y) + dy:.1f}" text-anchor="{anchor}"{c}>{esc(s)}</text>')

    def rawtext(self, px, py, s, cls="lbl", anchor="start", color=None):
        c = f' fill="{color}"' if color else ""
        self.body.append(f'<text class="{cls}" x="{px:.1f}" y="{py:.1f}" '
                         f'text-anchor="{anchor}"{c}>{esc(s)}</text>')

    def bar(self, px, py, w, h, color, opacity=1.0):
        self.body.append(f'<rect x="{px:.1f}" y="{py:.1f}" width="{w:.1f}" '
                         f'height="{h:.1f}" fill="{color}" '
                         f'opacity="{opacity}" rx="1.5"/>')

    # -- frame
    def grid(self, xticks, yticks, xfmt=str, yfmt=str):
        g = []
        for t in yticks:
            y = self.py(t)
            g.append(f'<line class="grid" x1="{self.ml}" y1="{y:.1f}" '
                     f'x2="{self.w - self.mr}" y2="{y:.1f}"/>')
            g.append(f'<text class="lbl" x="{self.ml - 9}" y="{y + 4:.1f}" '
                     f'text-anchor="end">{esc(yfmt(t))}</text>')
        for t in xticks:
            x = self.px(t)
            g.append(f'<line class="grid" x1="{x:.1f}" y1="{self.mt}" '
                     f'x2="{x:.1f}" y2="{self.h - self.mb}"/>')
            g.append(f'<text class="lbl" x="{x:.1f}" y="{self.h - self.mb + 18}" '
                     f'text-anchor="middle">{esc(xfmt(t))}</text>')
        g.append(f'<line class="axis" x1="{self.ml}" y1="{self.mt}" '
                 f'x2="{self.ml}" y2="{self.h - self.mb}"/>')
        g.append(f'<line class="axis" x1="{self.ml}" y1="{self.h - self.mb}" '
                 f'x2="{self.w - self.mr}" y2="{self.h - self.mb}"/>')
        self.body.insert(0, "\n".join(g))

    def topnote(self, s, anchor="start", color=None, cls="sub"):
        """A note just inside the top of the plot frame.  Outside the frame is
        where the subtitle lives, and the two used to be drawn on the same
        baseline."""
        x = {"start": self.ml + 8, "middle": (self.ml + self.w - self.mr) / 2,
             "end": self.w - self.mr - 8}[anchor]
        self.rawtext(x, self.mt + 15, s, cls=cls, anchor=anchor, color=color)

    def footnote(self, s, anchor="start", color=None, cls="sub"):
        """Likewise at the bottom: below the frame is the tick row and the axis
        title, so caveats go inside it."""
        x = {"start": self.ml + 8, "middle": (self.ml + self.w - self.mr) / 2,
             "end": self.w - self.mr - 8}[anchor]
        self.rawtext(x, self.h - self.mb - 9, s, cls=cls, anchor=anchor,
                     color=color)

    def render(self, path):
        cx = self.ml + (self.w - self.ml - self.mr) / 2
        head = [
            f'<text class="ttl" x="{self.tx}" y="22">{esc(self.title)}</text>',
        ] + [
            f'<text class="sub" x="{self.tx}" y="{39 + 15 * i}">{esc(s)}</text>'
            for i, s in enumerate(self.subs)
        ] + [
            f'<text class="lbl" x="{cx:.1f}" y="{self.h - 10}" '
            f'text-anchor="middle">{esc(self.xlab)}</text>',
            f'<text class="lbl" x="14" y="{self.mt + (self.h - self.mt - self.mb) / 2:.1f}" '
            f'text-anchor="middle" transform="rotate(-90 14 '
            f'{self.mt + (self.h - self.mt - self.mb) / 2:.1f})">{esc(self.ylab)}</text>',
        ]
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" '
               f'height="{self.h}" viewBox="0 0 {self.w} {self.h}" '
               f'font-family="system-ui">\n<style>{STYLE}</style>\n'
               f'<rect class="bg" width="{self.w}" height="{self.h}"/>\n'
               + "\n".join(head) + "\n" + "\n".join(self.body) + "\n</svg>\n")
        with open(path, "w", encoding="utf-8") as f:
            f.write(svg)
        print("wrote", os.path.basename(path))


def logticks(lo, hi):
    """1,2,5 decade ticks covering [lo,hi]."""
    out = []
    d = math.floor(math.log10(lo))
    while 10 ** d <= hi * 1.001:
        for m in (1, 2, 5):
            v = m * 10 ** d
            if lo * 0.999 <= v <= hi * 1.001:
                out.append(v)
        d += 1
    return out


def numfmt(v):
    if v >= 100:
        return f"{v:.0f}"
    if v >= 10:
        return f"{v:.0f}"
    if v >= 1:
        return f"{v:g}"
    return f"{v:g}"


# ---------------------------------------------------------------------- data

def session():
    """bench_session.json reshaped into the two dicts the plots already read.

    Every preset and every reference codec measured in one interleaved run.
    This is preferred over every other dataset on disk because it is the only
    one whose cross-codec ratios were produced at the same time -- see
    ARCHITECTURE.md 26, where -7 spans 22% across sessions with byte-identical
    output, which is enough to move a speed ratio by a third.

    -7 and zpaq ran twice as drift sentinels; their repeats are averaged.
    Reference peak RSS is carried over from bench_refs.json: it was sampled
    rather than self-reported either way, and the session run did not sample it.
    """
    p = os.path.join(DATA, "bench_session.json")
    if not os.path.exists(p):
        return None, None
    d = json.load(open(p))
    old = {}
    rp = os.path.join(DATA, "bench_refs.json")
    if os.path.exists(rp):
        old = json.loads(open(rp, encoding="utf-8-sig").read())

    agg = {}
    for r in d["rows"]:
        agg.setdefault(r["label"], []).append(r)

    def avg(v, k):
        return sum(x[k] for x in v) / float(len(v))

    # Per-file sizes for the -9 bpc chart, measured separately by
    # perfile_sizes.py.  They cannot come from the session run: gen archives
    # the whole directory in one pass, which is the point of it.  Attached only
    # if the file matches the build being plotted -- the previous chart was
    # drawn from an earlier binary and was stale by half a percent on every
    # bar, because "sizes never move between sessions" was mistaken for "sizes
    # never move".
    pf = os.path.join(DATA, "bench_perfile.json")
    perfile = None
    if os.path.exists(pf):
        j = json.load(open(pf))
        if j.get("level") == "9":
            perfile = j["rows"]

    lv, refs = {}, {}
    for lab, v in agg.items():
        if v[0]["kind"] == "gen":
            lv[lab[5:]] = {"total": v[0]["size"], "ctime": avg(v, "ctime"),
                           "dtime": avg(v, "dtime"), "cmem": avg(v, "cmem"),
                           "dmem": avg(v, "dmem"), "base": d["base"]}
            if lab == "gen -9" and perfile:
                lv["9"]["rows"] = perfile
        else:
            refs[lab] = {"total": v[0]["size"], "ctime": avg(v, "ctime"),
                         "dtime": avg(v, "dtime"),
                         "peak": old.get(lab, {}).get("peak", 0),
                         "rows": old.get(lab, {}).get("rows", {})}
    return {"levels": lv, "base": d["base"], "exe": "genv2.exe",
            "session": True}, refs


def load():
    # The single-session dataset wins wherever it exists.  bench_new.json and
    # bench_final.json are earlier builds' datasets and stay on disk so the
    # before/after plots have something to be "before".
    gen, refs = session()
    if gen is not None:
        return gen, refs
    cur = os.path.join(DATA, "bench_new.json")
    gen = json.load(open(cur if os.path.exists(cur)
                         else os.path.join(DATA, "bench_final.json")))
    rp = os.path.join(DATA, "bench_refs.json")
    refs = {}
    if os.path.exists(rp):
        raw = open(rp, encoding="utf-8-sig").read()
        refs = json.loads(raw)
    return gen, refs


PRESETS = [1, 2, 3, 5, 7, 9]
# The new build adds two rungs below -1.  Kept as strings because that is what
# they are on the command line and in the JSON.
NEW_PRESETS = ["f1", "f2", "1", "2", "3", "5", "7", "9"]
MB = 1 << 20


def gen_points(gen):
    """[(level, size_bytes, ctime, dtime, cmem_MB, base_bytes)] in preset order.

    Walks the full ladder including the -f rungs, so a dataset that has them
    plots them and one that does not simply skips them -- no second code path
    for the older shape.
    """
    out = []
    for k in NEW_PRESETS:
        if k not in gen["levels"]:
            continue
        d = gen["levels"][k]
        out.append((k, d["total"], d["ctime"], d["dtime"], d["cmem"], d["base"]))
    return out


def ref_points(refs):
    """{name: (size, ctime, dtime, peak_MB)} -- peak sampled, a lower bound."""
    out = {}
    for name, d in refs.items():
        out[name] = (d["total"], d["ctime"], d["dtime"], d["peak"] / MB)
    return out


# --------------------------------------------------------------------- plots

def speed_size(gp, rp, base, decomp=False, path="", title="", sub=""):
    ti = 3 if decomp else 2     # gen tuple:  (lv, size, ctime, dtime, mem, base)
    ri = 2 if decomp else 1     # ref tuple:  (size, ctime, dtime, peakMB)
    gs = [(base / MB / g[ti], g[1] / MB) for g in gp]
    rs = {n: (base / MB / v[ri], v[0] / MB) for n, v in rp.items()}

    xs = [p[0] for p in gs] + [v[0] for v in rs.values()]
    ys = [p[1] for p in gs] + [v[1] for v in rs.values()]
    lo, hi = min(xs) / 1.6, max(xs) * 1.9
    ylo, yhi = min(ys) - 1.6, max(ys) + 2.2

    p = Plot(title=title, sub=sub, xlog=True,
             xlab=("decompression speed (MB/s, log)" if decomp
                   else "compression speed (MB/s, log)"),
             ylab="Silesia total (MB) — lower is better")
    p.limits(lo, hi, ylo, yhi)
    p.grid(logticks(lo, hi), [y for y in range(int(ylo) + 1, int(yhi) + 1)
                              if y % 5 == 0], numfmt, lambda v: f"{v:.0f}")
    # gen labels go below their dots and reference labels above, so the two
    # families never collide even where they land on nearly the same point
    # (at -1 the engine and lpaq1 are close enough to overlap, which is the
    # finding, not a plotting bug).
    p.line(gs)
    lab = Labels(p)
    for (lv, *_), (x, y) in zip(gp, gs):
        p.dot(x, y, 5.5)
        lab.marker(p.px(x), p.py(y), 7)
        lab.add(p.px(x), p.py(y), f"-{lv}", cls="tagb")
    for n, (x, y) in rs.items():
        c = CLR.get(n, "#6e7781")
        p.dot(x, y, 5, color=c)
        lab.marker(p.px(x), p.py(y), 7)
        lab.add(p.px(x), p.py(y), n, color=c)
    lab.flush()
    p.topnote("gen presets joined by the blue line", anchor="end")
    p.render(os.path.join(OUT, path))


def ram_size(gp, rp, path, title, sub):
    gs = [(g[4], g[1] / MB) for g in gp]
    rs = {n: (v[3], v[0] / MB) for n, v in rp.items()}
    xs = [p[0] for p in gs] + [v[0] for v in rs.values()]
    ys = [p[1] for p in gs] + [v[1] for v in rs.values()]
    lo, hi = min(xs) / 1.7, max(xs) * 2.4
    ylo, yhi = min(ys) - 1.6, max(ys) + 2.2

    p = Plot(title=title, sub=sub, xlog=True,
             xlab="peak resident set (MB, log)",
             ylab="Silesia total (MB) — lower is better")
    p.limits(lo, hi, ylo, yhi)
    p.grid(logticks(lo, hi), [y for y in range(int(ylo) + 1, int(yhi) + 1)
                              if y % 5 == 0], numfmt, lambda v: f"{v:.0f}")
    p.line(gs)
    lab = Labels(p)
    for (lv, *_), (x, y) in zip(gp, gs):
        p.dot(x, y, 5.5)
        lab.marker(p.px(x), p.py(y), 7)
        lab.add(p.px(x), p.py(y), f"-{lv}", cls="tagb")
    for n, (x, y) in rs.items():
        c = CLR.get(n, "#6e7781")
        p.dot(x, y, 5, color=c, hollow=True)
        lab.marker(p.px(x), p.py(y), 7)
        lab.add(p.px(x), p.py(y), n, color=c)
    lab.flush()
    p.topnote("hollow = sampled at 50 ms (lower bound); gen self-reports exact",
              anchor="end")
    p.render(os.path.join(OUT, path))


def per_file(gen, refs, path, title, sub):
    """Grouped bars: bits per character per file, gen -9 vs references."""
    order = ["xml", "ooffice", "reymont", "sao", "x-ray", "mr", "osdb",
             "dickens", "samba", "nci", "webster", "mozilla"]
    g9 = {r["file"]: r for r in gen["levels"]["9"]["rows"]}
    series = [("gen -9", None)] + [(n, n) for n in
                                   ("zpaq -m5", "lpaq1 -6", "xz -9e")
                                   if refs.get(n, {}).get("rows")]

    w, h = 880, 430
    p = Plot(w=w, h=h, title=title, sub=sub, xlab="", ylab="bits per byte")
    p.mb = 62
    ymax = 0.0
    for f in order:
        n = g9[f]["insize"]
        ymax = max(ymax, g9[f]["size"] * 8 / n)
        for _, key in series[1:]:
            r = refs[key]["rows"].get(f)
            if r:
                ymax = max(ymax, r["size"] * 8 / r["insize"])
    ymax = math.ceil(ymax * 1.12)
    p.limits(0, len(order), 0, ymax)
    p.grid([], [y for y in range(0, ymax + 1)], str, lambda v: f"{v:g}")

    slot = (w - p.ml - p.mr) / len(order)
    bw = slot * 0.78 / len(series)
    for i, f in enumerate(order):
        x0 = p.ml + i * slot + slot * 0.11
        n = g9[f]["insize"]
        vals = [g9[f]["size"] * 8 / n]
        for _, key in series[1:]:
            r = refs[key]["rows"].get(f)
            vals.append(r["size"] * 8 / r["insize"] if r else 0.0)
        for j, v in enumerate(vals):
            if v <= 0:
                continue
            name = series[j][0]
            c = "#0969da" if j == 0 else CLR.get(name, "#6e7781")
            top = p.py(v)
            p.bar(x0 + j * bw, top, bw * 0.92, p.py(0) - top, c,
                  1.0 if j == 0 else 0.72)
        p.rawtext(x0 + slot * 0.39, h - p.mb + 16, f, cls="lbl",
                  anchor="middle")

    lx = p.ml
    for j, (name, _) in enumerate(series):
        c = "#0969da" if j == 0 else CLR.get(name, "#6e7781")
        p.body.append(f'<rect x="{lx}" y="{h - 22}" width="11" height="11" '
                      f'fill="{c}" opacity="{1.0 if j == 0 else 0.72}" rx="1.5"/>')
        p.rawtext(lx + 16, h - 12, name, cls="lbl")
        lx += 22 + len(name) * 7.0
    p.render(os.path.join(OUT, path))


def ladder(gp, path, title, sub):
    """Per preset: time and RAM cost of each step down the ladder."""
    w, h = 800, 420
    p = Plot(w=w, h=h, title=title, sub=sub, xlab="preset",
             ylab="relative to -9  (%)")
    p.mb = 62
    base_sz = gp[-1][1]
    base_t = gp[-1][2]
    base_m = gp[-1][4]
    rows = []
    for lv, sz, ct, dt, mem, _ in gp:
        rows.append((lv, sz * 100.0 / base_sz, ct * 100.0 / base_t,
                     mem * 100.0 / base_m))
    ymax = math.ceil(max(max(r[1:]) for r in rows) / 10.0) * 10 + 5
    p.limits(0, len(rows), 0, ymax)
    p.grid([], list(range(0, ymax + 1, 20)), str, lambda v: f"{v:g}")

    series = [("size", "#0969da"), ("time", "#d29922"), ("RAM", "#1a7f37")]
    slot = (w - p.ml - p.mr) / len(rows)
    bw = slot * 0.74 / 3
    for i, r in enumerate(rows):
        x0 = p.ml + i * slot + slot * 0.13
        for j, (nm, c) in enumerate(series):
            v = r[1 + j]
            top = p.py(v)
            p.bar(x0 + j * bw, top, bw * 0.9, p.py(0) - top, c, 0.85)
            p.rawtext(x0 + j * bw + bw * 0.45, top - 5, f"{v:.0f}", cls="sub",
                      anchor="middle")
        p.rawtext(x0 + slot * 0.37, h - p.mb + 18, f"-{r[0]}", cls="lbl",
                  anchor="middle")
    lx = p.ml
    for nm, c in series:
        p.body.append(f'<rect x="{lx}" y="{h - 22}" width="11" height="11" '
                      f'fill="{c}" opacity="0.85" rx="1.5"/>')
        p.rawtext(lx + 16, h - 12, nm, cls="lbl")
        lx += 26 + len(nm) * 7.5
    p.render(os.path.join(OUT, path))


# Memory sweep, measured separately on dickens at -9 (see README, "How memory
# trades against speed").  Table geometry is exact; times are single-thread on
# an idle machine.  (-m, MB, output bytes, seconds)
MEMSWEEP = [
    (-7,   78, 2229805, 47.7), (-6,   84, 2173939, 47.0),
    (-5,   96, 2135531, 47.3), (-4,  121, 2107804, 45.6),
    (-3,  171, 2086720, 46.3), (-2,  270, 2071044, 47.6),
    (-1,  468, 2060536, 49.7), (0,   864, 2053946, 52.1),
    (1,  1656, 2049995, 55.3), (2,  2473, 2048061, 59.6),
]


def mem_sweep(path, title, sub):
    b_sz, b_t = 2053946.0, 52.1          # the -m0 row is the baseline
    sz = [(m, s * 100.0 / b_sz - 100.0) for _, m, s, _ in MEMSWEEP]
    tm = [(m, t * 100.0 / b_t - 100.0) for _, m, _, t in MEMSWEEP]
    xs = [p[0] for p in sz]
    lo, hi = min(xs) / 1.35, max(xs) * 1.35
    ylo, yhi = -16, 16

    p = Plot(w=800, h=440, title=title, sub=sub, xlog=True,
             xlab="table memory (MB, log)",
             ylab="relative to -m0 (%)")
    p.limits(lo, hi, ylo, yhi)
    p.grid(logticks(lo, hi), list(range(-15, 16, 5)), numfmt,
           lambda v: f"{v:+g}" if v else "0")
    p.body.append(f'<line class="axis" x1="{p.ml}" y1="{p.py(0):.1f}" '
                  f'x2="{p.w - p.mr}" y2="{p.py(0):.1f}" stroke-dasharray="4 3"/>')
    for pts, c, nm in ((sz, "#0969da", "output size"), (tm, "#d29922", "time")):
        d = " ".join(f"{'M' if i == 0 else 'L'}{p.px(x):.1f},{p.py(y):.1f}"
                     for i, (x, y) in enumerate(pts))
        p.body.append(f'<path d="{d}" stroke="{c}" stroke-width="2.2" fill="none"/>')
        for x, y in pts:
            p.dot(x, y, 4.5, color=c)
        p.text(pts[0][0], pts[0][1], nm, dx=6, dy=-9, color=c, cls="tagb")
    p.rawtext(p.px(864), p.mt + 14, "-m0 (default)", cls="sub", anchor="middle")
    p.body.append(f'<line class="grid" x1="{p.px(864):.1f}" y1="{p.mt + 20}" '
                  f'x2="{p.px(864):.1f}" y2="{p.h - p.mb}" stroke-dasharray="3 3"/>')
    p.render(os.path.join(OUT, path))


# Match-bypass gate sweep (byt_sweep.py).  Sizes only: they are deterministic,
# whereas part of that sweep's timings were taken while another process was on
# the machine.  Totals are over the sweep's file subset, not the whole corpus,
# so only the percentages relative to gate 0 are meaningful.
GATES = {
    "-1": [(0, 22530343), (48, 22820768), (96, 22470998),
           (192, 22474566), (400, 22490953), (1024, 22507947)],
    "-3": [(0, 20392607), (48, 20820120), (96, 20363686), (192, 20355155)],
    "-9": [(0, 6181065), (192, 6172589), (400, 6170036), (800, 6174221)],
}
GATE_CLR = {"-1": "#0969da", "-3": "#8250df", "-9": "#1a7f37"}


def gate_sweep(path, title, sub):
    """Categorical x: gate 0 means the bypass is off, which has no log position."""
    cols = [0, 48, 96, 192, 400, 800, 1024]
    p = Plot(w=780, h=430, title=title, sub=sub,
             xlab="match-bypass gate (matched bytes before the bypass engages)",
             ylab="output size vs bypass off (%)")
    p.mb = 62
    p.limits(-0.5, len(cols) - 0.5, -0.6, 2.4)
    p.grid([], [-0.5, 0, 0.5, 1.0, 1.5, 2.0], str,
           lambda v: "0" if v == 0 else f"{v:+g}")
    p.body.append(f'<line class="axis" x1="{p.ml}" y1="{p.py(0):.1f}" '
                  f'x2="{p.w - p.mr}" y2="{p.py(0):.1f}" stroke-dasharray="4 3"/>')
    for i, g in enumerate(cols):
        p.rawtext(p.px(i), p.h - p.mb + 18, "off" if g == 0 else str(g),
                  cls="lbl", anchor="middle")
    for lvl, pts in GATES.items():
        base = pts[0][1]
        xy = [(cols.index(g), (s - base) * 100.0 / base) for g, s in pts]
        d = " ".join(f"{'M' if i == 0 else 'L'}{p.px(x):.1f},{p.py(y):.1f}"
                     for i, (x, y) in enumerate(xy))
        c = GATE_CLR[lvl]
        p.body.append(f'<path d="{d}" stroke="{c}" stroke-width="2.2" fill="none"/>')
        for x, y in xy:
            p.dot(x, y, 4.5, color=c)
        p.text(xy[-1][0], xy[-1][1], lvl, dx=10, dy=4, color=c, cls="tagb")
    # The shipped gate for -1/-3/-5 is 96; mark where the cliff ends.
    p.body.append(f'<line class="grid" x1="{p.px(2):.1f}" y1="{p.mt + 22}" '
                  f'x2="{p.px(2):.1f}" y2="{p.h - p.mb}" stroke-dasharray="3 3"/>')
    p.rawtext(p.px(2), p.mt + 15, "shipped at -1/-3/-5", cls="sub",
              anchor="middle")
    p.rawtext(p.ml + 6, p.py(0) + 14, "below this line the bypass is a ratio win",
              cls="sub")
    p.render(os.path.join(OUT, path))


def before_after(ab, rp, path, title, sub):
    """The shipped ladder against the new one, on the size-vs-speed plane.

    Both arms come from bench_ab.json -- one session, interleaved per preset --
    because this machine's absolute times carry an 11% session-to-session
    spread and a two-session comparison of speed would not mean anything.  The
    reference codecs are drawn faintly for scale only; they are from the
    reference run and are positioned on size, which is deterministic.

    The new curve carries two rungs the old one has no counterpart for, which
    is the point of drawing the curves together rather than tabulating deltas.
    """
    any_row = next(iter(ab["new"].values()))
    base = any_row["base"]

    def pts(d, keys):
        return [(k, base / MB / d[k]["ctime"], d[k]["total"] / MB)
                for k in keys if k in d]

    op = pts(ab["old"], [str(k) for k in PRESETS])
    np_ = pts(ab["new"], NEW_PRESETS)
    if not op or not np_:
        return

    xs = [p[1] for p in op + np_] + [base / MB / v[1] for v in rp.values()]
    ys = [p[2] for p in op + np_] + [v[0] / MB for v in rp.values()]
    lo, hi = min(xs) / 1.6, max(xs) * 1.9
    ylo, yhi = min(ys) - 1.6, max(ys) + 2.2

    p = Plot(title=title, sub=sub, xlog=True,
             xlab="compression speed (MB/s, log)",
             ylab="Silesia total (MB) — lower is better")
    p.limits(lo, hi, ylo, yhi)
    p.grid(logticks(lo, hi), [y for y in range(int(ylo) + 1, int(yhi) + 1)
                              if y % 5 == 0], numfmt, lambda v: f"{v:.0f}")

    lab = Labels(p)
    for n, v in rp.items():
        x, y = base / MB / v[1], v[0] / MB
        p.dot(x, y, 4, color="#adb5bd")
        lab.marker(p.px(x), p.py(y), 6)
        lab.add(p.px(x), p.py(y), n, color="#adb5bd")

    p.line([(x, y) for _, x, y in op], cls="oldl")
    for k, x, y in op:
        p.dot(x, y, 4.5, cls="oldd")
        lab.marker(p.px(x), p.py(y), 6)

    p.line([(x, y) for _, x, y in np_])
    for k, x, y in np_:
        p.dot(x, y, 5.5)
        lab.marker(p.px(x), p.py(y), 7)
        lab.add(p.px(x), p.py(y), "-" + k, cls="tagb")
    lab.flush()

    p.topnote("dashed/hollow = shipped build   solid = new build", anchor="end")
    p.render(os.path.join(OUT, path))


def delta_bars(ab, path, title, sub):
    """Per preset, what the new build changed: size, compression time, decode
    time.  Signed, so below the zero line is an improvement on every axis.

    Only presets the old build also has appear here -- the -f rungs have no
    counterpart to be a delta against, and inventing one would be worse than
    omitting them.
    """
    keys = [k for k in NEW_PRESETS if k in ab["old"] and k in ab["new"]]
    if not keys:
        return
    rows = []
    for k in keys:
        o, n = ab["old"][k], ab["new"][k]
        rows.append((k,
                     (n["total"] - o["total"]) * 100.0 / o["total"],
                     (n["ctime"] - o["ctime"]) * 100.0 / o["ctime"]))

    w, h = 800, 420
    p = Plot(w=w, h=h, title=title, sub=sub, xlab="preset",
             ylab="change vs shipped build  (%)   lower is better")
    p.mb = 62
    lo = min(min(r[1], r[2]) for r in rows)
    hi = max(max(r[1], r[2]) for r in rows)
    ylo = math.floor(min(lo, 0) - 2)
    yhi = math.ceil(max(hi, 0) + 2)
    p.limits(0, len(rows), ylo, yhi)
    step = max(1, int(math.ceil((yhi - ylo) / 8.0)))
    ticks = list(range(int(math.ceil(ylo / step)) * step, yhi + 1, step))
    p.grid([], ticks, str, lambda v: f"{v:+g}")

    zero = p.py(0)
    p.body.append(f'<line class="axis" x1="{p.ml}" y1="{zero:.1f}" '
                  f'x2="{p.w - p.mr}" y2="{zero:.1f}"/>')

    series = [("size", "#0969da"), ("compression time", "#d29922")]
    slot = (w - p.ml - p.mr) / len(rows)
    bw = slot * 0.7 / len(series)
    for i, r in enumerate(rows):
        x0 = p.ml + i * slot + slot * 0.15
        for j, (nm, c) in enumerate(series):
            v = r[1 + j]
            y = p.py(v)
            top, hgt = (min(y, zero), abs(zero - y))
            p.bar(x0 + j * bw, top, bw * 0.88, max(hgt, 0.8), c, 0.85)
            p.rawtext(x0 + j * bw + bw * 0.44,
                      (y - 5) if v <= 0 else (y + 13),
                      f"{v:+.2f}" if abs(v) < 10 else f"{v:+.1f}",
                      cls="sub", anchor="middle")
        p.rawtext(x0 + slot * 0.35, h - p.mb + 18, "-" + r[0], cls="lbl",
                  anchor="middle")
    lx = p.ml
    for nm, c in series:
        p.body.append(f'<rect x="{lx}" y="{h - 22}" width="11" height="11" '
                      f'fill="{c}" opacity="0.85" rx="1.5"/>')
        p.rawtext(lx + 16, h - 12, nm, cls="lbl")
        lx += 26 + len(nm) * 7.0
    p.render(os.path.join(OUT, path))


def main():
    gen, refs = load()
    gp = gen_points(gen)
    if not gp:
        raise SystemExit("bench_final.json has no preset rows")
    base = gp[0][5]
    rp = ref_points(refs)

    n = len(gp)
    sub = (f"Silesia, {base / MB:.0f} MB, single thread, one uninterrupted run. "
           f"{n} gen presets.")
    if rp:
        # The published record is not plotted: it was not measured on this
        # machine and at 26.5 MB it would stretch the axis by a third to add a
        # point nothing else can be compared against on speed.
        speed_size(gp, rp, base, False, "speed_vs_size.svg",
                   "Compression: size against speed",
                   sub + "  Silesia record is 26.5 MB (paq8px -12L, 29 GB).")
        speed_size(gp, rp, base, True, "decomp_vs_size.svg",
                   "Decompression: size against speed", sub)
        ram_size(gp, rp, "ram_vs_size.svg",
                 "Memory: size against peak resident set", sub)
        # The single-session dataset has no per-file gen rows and cannot have
        # any: gen archives the whole directory in one pass, which is how the
        # tool is used and where its cross-file context comes from.  This chart
        # is bits-per-byte -- pure size -- and sizes have not moved in any
        # session, so the existing SVG stays valid and is left alone rather
        # than regenerated from a dataset that would have to fake it.
        if "rows" in gen["levels"].get("9", {}):
            # Its own subtitle: this chart plots one gen preset, not eight, and
            # its per-file sizes come from a separate compression-only run
            # rather than from the interleaved session the other charts use.
            # Inheriting the shared subtitle would claim both wrongly.
            per_file(gen, refs, "per_file_bpc.svg",
                     "Per file, bits per byte",
                     f"Silesia, {base / MB:.0f} MB, gen -9 against the "
                     f"references. Sizes only, measured per file "
                     f"(perfile_sizes.py); deterministic, so no timing "
                     f"protocol applies.")
        else:
            print("skipped per_file_bpc.svg (session data is whole-corpus; "
                  "sizes unchanged, existing chart still valid)")
    ladder(gp, "preset_ladder.svg",
           "What each preset costs, relative to -9", sub)
    # This sweep predates the current binary: its -m0 baseline is dickens at
    # -9 = 2,053,946 bytes, where the current build gives 2,048,642.  Every
    # point is plotted relative to that baseline, so the trade it shows is
    # internally consistent and the conclusion stands -- but it is not
    # current-build data and the chart says so rather than implying otherwise.
    mem_sweep("memory_vs_speed.svg",
              "How table memory trades against size and speed",
              "dickens (10 MB) at -9, single thread. 32x the memory costs 31% "
              "more time. Relative sweep from an earlier build (-m0 baseline "
              "2,053,946 B vs 2,048,642 B today); the trade is unchanged.")
    # Checked against the current binary rather than assumed: -1 at the default
    # gate 96 over these six files measures 22,423,769 today where the table
    # says 22,470,998, so the sweep predates this build by 0.21%.  Every point
    # is plotted as a percentage of that sweep's own gate-0 baseline, so the
    # shape -- and the conclusion that below ~96 the bypass costs real ratio --
    # is unaffected.  The absolutes are not current, and the chart says so.
    gate_sweep("gate_sweep.svg",
               "Match-bypass gate: below ~96 the shortcut costs real ratio",
               "Six Silesia files that disagree. Sizes only -- deterministic. "
               "Relative sweep from a build 0.21% behind the current one "
               "(-1/gate 96: 22,470,998 B then, 22,423,769 B now); the shape "
               "is unchanged.")
    ab_path = os.path.join(DATA, "bench_ab.json")
    if os.path.exists(ab_path):
        ab = json.load(open(ab_path))
        absub = (f"Silesia, {base / MB:.0f} MB, single thread, compression. "
                 f"Both builds in one session, interleaved per file so the "
                 f"two arms are seconds apart, not a quarter hour.")
        before_after(ab, rp, "before_after.svg",
                     "Before and after: the ladder, and two rungs below it",
                     absub)
        delta_bars(ab, "before_after_delta.svg",
                   "What the new build changed, per preset", absub)


# Guarded so the SVG toolkit above can be imported by other scripts -- mkpresets.py
# reuses Plot rather than carrying a second copy of it.
if __name__ == "__main__":
    main()
